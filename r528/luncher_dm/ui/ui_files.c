/*
 * ui_files.c — 文件管理器子页【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_files_subpage（改名
 * ui_files_create）+ files_* 全部（目录扫描/多选/剪贴板/面包屑）。
 * 无 system() 调用：新建文件夹走 mkdir(2) 逐级建，删除走 files_rmrf
 * 递归（防 shell 注入）；系统目录黑名单见 files_is_protected。
 *
 * 红线（勿碰）：
 *   - MMCSD_MULTIBLOCK_LIMIT=1 勿改（两次上板实证多块读必崩）。
 *   - files_sel_bar 挂在 subpage_overlay 上，close_subpage 经
 *     files_ui_clear_ptrs() 前置空（对象+清理一起搬）。
 *   - files_open_file 引用 books 共享状态（dm_book 数组 / book_open_reader，
 *     deskmate_ui.h 已声明），Files 页 .txt → Books 阅读器。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"

/* 前向声明（原 deskmate_ui.c 顶部同款；函数定义在下方） */
static void files_scan_dir(const char *path);
static void files_refresh_list(void);
static void files_row_click_cb(lv_event_t *e);
static void files_row_long_cb(lv_event_t *e);
static void files_row_press_lost_cb(lv_event_t *e);
static void files_blank_long_cb(lv_event_t *e);      /* P145：空白处/空目录长按 → 操作栏 */
static void files_enter_select_mode(int idx);
static void files_exit_select_mode(void);
static void files_toggle_select(int idx);
static void files_cb_copy(lv_event_t *e);
static void files_cb_cut(lv_event_t *e);
static void files_cb_delete(lv_event_t *e);
static void files_open_file(const char *path, int type);
static void files_cb_internal_root(lv_event_t *e);
static void files_cb_tf_root(lv_event_t *e);
static void files_cb_path_root(lv_event_t *e);
static void files_cb_path_level(lv_event_t *e);
static void files_cb_sel_paste(lv_event_t *e);
static void files_cb_sel_newfolder(lv_event_t *e);
static void files_cb_sel_copy(lv_event_t *e);
static void files_cb_sel_cut(lv_event_t *e);
static void files_cb_sel_delete(lv_event_t *e);
static void files_cb_exit_select(lv_event_t *e);
static void files_go_up_cb(lv_event_t *e);
static void files_update_breadcrumb(void);
static void files_update_up_btn(void);
static void files_clip_paste(void);
static void files_toast_show(const char *msg);      /* P0-3/P1-5：操作反馈 toast */
static int  files_is_protected(const char *path); /* 黑名单：系统目录禁删/禁移/禁粘 */
static void files_dialog_show(const char *msg, void (*ok_cb)(void)); /* P0-3/P145：确认弹窗 */
static void files_do_delete(void);                  /* P0-3：确认后的实际删除 */
static int  files_paste_check_conflicts(void);      /* P145：统计与目标目录重名项数 */
static void files_paste_confirm_go(void);           /* P145：重名确认 OK → 执行粘贴 */
static void files_copy_ui_show(void);               /* P145：进度条 UI + 轮询定时器 */
static void files_copy_timer_cb(lv_timer_t *t);     /* P145：进度刷新/收尾 */
static void files_img_viewer_open(const char *path);/* 2026-09-11：图片查看器 */
static void files_img_viewer_close(void);

/* ================================================================
 * FILES SUBPAGE — 专业平板文件管理器 UI
 * 设计原则：
 * 1. 左侧可折叠目录导航 + 面包屑路径
 * 2. 右侧主内容区：可滚动文件列表
 * 3. 快捷入口：内部存储 + TF卡
 * 4. FAB 按钮不遮挡内容
 * 5. 多选模式：底部操作栏
 * ================================================================ */

/* Files 浏览器状态 */
#define FILES_MAX_ENTRIES  128
#define FILES_MAX_CLIPBOARD 32

/* 文件类型 */
enum { FILE_TYPE_DIR = 0, FILE_TYPE_TXT = 1, FILE_TYPE_MP3 = 2, FILE_TYPE_IMG = 3, FILE_TYPE_OTHER = 4 };

/* 剪贴板 */
static char     clip_paths[FILES_MAX_CLIPBOARD][256];
static int      clip_count;
static int      clip_op_cut;   /* 1=剪贴(粘贴后删除源), 0=复制 */
static int      clip_paste_confirmed;  /* P145：重名弹窗已确认（防重复弹） */

/* P145：后台复制线程 + 进度（UI 定时器轮询读取；仅 UI 线程写 lv_obj） */
#define FILES_COPY_BUF   (32 * 1024)
static pthread_t   files_copy_thread;
static volatile int files_copy_running;    /* 1=复制进行中 */
static volatile int files_copy_done;       /* 1=已完成（成功或失败） */
static volatile int files_copy_ok;         /* 0=全部成功 */
static volatile long files_copy_total;     /* 总字节 */
static volatile long files_copy_done_bytes;/* 已复制字节 */
static int files_copy_n;                   /* 成功项数 */
static int files_copy_renamed;             /* 重名自动改名项数 */

/* 当前浏览状态 */
static char     files_cur_path[256] = "/";
static char     files_paths[FILES_MAX_ENTRIES][256];
static char     files_names[FILES_MAX_ENTRIES][64];
static int      files_types[FILES_MAX_ENTRIES];
static long     files_sizes[FILES_MAX_ENTRIES];   /* 字节（目录=0，行尾小字） */
static uint32_t files_count;
static lv_obj_t *files_list_cont;              /* 列表容器 */

/* 选择模式 */
static int      files_select_mode;
static int      files_selected[FILES_MAX_ENTRIES];
static int      files_sel_count;

/* P0-2：长按进入选择模式后，释放时 LVGL 仍会发 CLICKED（lv_indev.c
 * 释放分支不检查 long_pr_sent）→ 用 guard 吞掉这次 CLICKED，防"进即退" */
static int      files_long_guard;

/* UI 对象 — 统一 iOS 玻璃风格 */
static lv_obj_t *files_list_scrl;     /* 可滚动列表区域 */
static lv_obj_t *files_breadcrumb;    /* 面包屑路径栏（玻璃胶囊） */
static lv_obj_t *files_up_btn;        /* 标题行右侧「上级」圆钮（根目录隐藏） */
static lv_obj_t *files_sel_bar;       /* 多选模式底部圆形操作栏 */
static lv_obj_t *files_sel_paste_lbl; /* 操作栏粘贴按钮图标（剪贴板空则置灰） */
static lv_obj_t *files_toast;         /* P0-3：操作反馈 toast（底部居中胶囊） */
static lv_timer_t *files_toast_timer; /* toast 自动隐藏定时器 */
static lv_obj_t *files_dialog;        /* P0-3：删除确认弹窗（玻璃卡） */
static lv_obj_t *files_copy_ui;       /* P145：复制进度条（玻璃卡） */
static lv_timer_t *files_copy_timer;  /* P145：进度刷新定时器 */
static lv_obj_t *files_copy_arc;      /* P145：进度环 */
static lv_obj_t *files_copy_pct_lbl;  /* P145：环中心百分比 */

/* 2026-09-11 图片查看器：文件浏览器点图片 → 全屏 overlay，等比适配 +
 * 关闭按钮 + 缩放 +/- + 点背景关闭，避免"全屏后出不去" */
static lv_obj_t *files_img_overlay;
static lv_obj_t *files_img_widget;
static char      files_img_path[256];     /* LVGL 按路径延迟解码，字符串需常驻 */
static uint32_t  files_img_fit_scale = 256;
static uint32_t  files_img_scale = 256;
#define FILES_IMG_SCALE_MIN 32
#define FILES_IMG_SCALE_MAX 1024

/* 操作栏/上级按钮随子页销毁——close_subpage 删除 overlay 前置空，防野指针。
 * Phase 1 拆分：非 static（deskmate_ui.c close_subpage 调用，deskmate_ui.h 声明） */
void files_ui_clear_ptrs(void)
{
    files_sel_bar       = NULL;
    files_sel_paste_lbl = NULL;
    files_up_btn        = NULL;
    files_toast         = NULL;
    files_dialog        = NULL;
    if (files_toast_timer) { lv_timer_del(files_toast_timer); files_toast_timer = NULL; }
    files_copy_ui       = NULL;   /* P145：进度条随子页销毁，timer 回调靠 NULL 守卫自删 */
    files_copy_timer    = NULL;
    files_copy_arc      = NULL;
    files_copy_pct_lbl  = NULL;
    files_copy_running  = 0;      /* 后台线程跑完无害；清标志防下次粘贴被 running 挡住 */
    files_img_viewer_close();     /* 2026-09-11：图片查看器随子页销毁 */
}

/* 根据扩展名判断文件类型 */
static int file_type_from_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return FILE_TYPE_OTHER;
    if (strcasecmp(dot, ".txt") == 0) return FILE_TYPE_TXT;
    if (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0) return FILE_TYPE_MP3;
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".png") == 0) return FILE_TYPE_IMG;
    return FILE_TYPE_OTHER;
}

/* 扫描目录 */
static void files_scan_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) { files_count = 0; return; }

    files_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && files_count < FILES_MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full[256];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        int type = S_ISDIR(st.st_mode) ? FILE_TYPE_DIR : file_type_from_name(ent->d_name);

        strncpy(files_paths[files_count], full, sizeof(files_paths[0]) - 1);
        files_paths[files_count][sizeof(files_paths[0]) - 1] = '\0';
        strncpy(files_names[files_count], ent->d_name, sizeof(files_names[0]) - 1);
        files_names[files_count][sizeof(files_names[0]) - 1] = '\0';
        dm_utf8_trim(files_names[files_count]);   /* 2026-09-14：截断勿劈开多字节尾 */
        files_types[files_count] = type;
        files_sizes[files_count] = S_ISDIR(st.st_mode) ? 0 : (long)st.st_size;
        files_selected[files_count] = 0;
        files_count++;
    }
    closedir(dir);

    /* 排序：目录在前、按名称升序（安卓文件管理器默认行为） */
    for (uint32_t i = 1; i < files_count; i++) {
        uint32_t j = i;
        while (j > 0) {
            int a_dir = (files_types[j - 1] == FILE_TYPE_DIR);
            int b_dir = (files_types[j] == FILE_TYPE_DIR);
            int swap = 0;
            if (a_dir != b_dir)            swap = b_dir;          /* 目录优先 */
            else if (strcmp(files_names[j - 1], files_names[j]) > 0) swap = 1; /* 名称升序 */
            if (!swap) break;

            char tmp_p[256], tmp_n[64];
            int  tmp_t; long tmp_s;
            strcpy(tmp_p, files_paths[j - 1]);   strcpy(files_paths[j - 1], files_paths[j]);   strcpy(files_paths[j], tmp_p);
            strcpy(tmp_n, files_names[j - 1]);   strcpy(files_names[j - 1], files_names[j]);   strcpy(files_names[j], tmp_n);
            tmp_t = files_types[j - 1];          files_types[j - 1] = files_types[j];          files_types[j] = tmp_t;
            tmp_s = files_sizes[j - 1];          files_sizes[j - 1] = files_sizes[j];          files_sizes[j] = tmp_s;
            j--;
        }
    }
}

/* 更新标题行「上级」按钮可见性（根目录 / 与 /sdcard 无上级 → 隐藏） */
static void files_update_up_btn(void)
{
    if (!files_up_btn) return;
    bool is_root = (strcmp(files_cur_path, "/") == 0 ||
                    strcmp(files_cur_path, "/sdcard") == 0);
    if (is_root)
        lv_obj_add_flag(files_up_btn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(files_up_btn, LV_OBJ_FLAG_HIDDEN);
}

/* 更新面包屑路径栏 */
static void files_update_breadcrumb(void)
{
    if (!files_breadcrumb) return;

    /* 先释放未点击层级按钮 strdup 的路径（点击回调已置 NULL，防泄漏/双释放） */
    uint32_t nch = lv_obj_get_child_count(files_breadcrumb);
    for (uint32_t i = 0; i < nch; i++) {
        void *ud = lv_obj_get_user_data(lv_obj_get_child(files_breadcrumb, i));
        if (ud)
            free(ud);
    }
    lv_obj_clean(files_breadcrumb);
    files_update_up_btn();

    /* 根入口按钮 */
    lv_obj_t *root_btn = lv_btn_create(files_breadcrumb);
    lv_obj_set_size(root_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(root_btn, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(root_btn, DM(4), 0);
    lv_obj_t *root_lbl = lv_label_create(root_btn);
    bool is_sd = (strcmp(files_cur_path, "/sdcard") == 0 || strncmp(files_cur_path, "/sdcard/", 7) == 0);
    lv_label_set_text(root_lbl, is_sd ? LV_SYMBOL_SD_CARD " TF卡" : LV_SYMBOL_HOME " 本机");
    lv_obj_set_style_text_font(root_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(root_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_add_event_cb(root_btn, files_cb_path_root, LV_EVENT_CLICKED, NULL);

    /* 解析路径各层 */
    if (strcmp(files_cur_path, "/") == 0 || strcmp(files_cur_path, "/sdcard") == 0) return;

    char path_copy[256];
    strncpy(path_copy, files_cur_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    char *parts[32];
    int part_count = 0;
    char *token = strtok(path_copy, "/");
    while (token && part_count < 32) {
        parts[part_count++] = token;
        token = strtok(NULL, "/");
    }

    char accum[256] = "";
    for (int i = 0; i < part_count; i++) {
        /* 分隔符 */
        lv_obj_t *sep = lv_label_create(files_breadcrumb);
        lv_label_set_text(sep, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(sep, FONT_BODY, 0);
        lv_obj_set_style_text_color(sep, lv_color_hex(COL_SEC), 0);
        lv_obj_set_style_pad_left(sep, DM(2), 0);
        lv_obj_set_style_pad_right(sep, DM(2), 0);

        strcat(accum, "/");
        strcat(accum, parts[i]);

        int is_last = (i == part_count - 1);
        lv_obj_t *pbtn = lv_btn_create(files_breadcrumb);
        lv_obj_set_size(pbtn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(pbtn, LV_OPA_0, 0);
        lv_obj_set_style_pad_all(pbtn, DM(4), 0);
        lv_obj_t *pl = lv_label_create(pbtn);
        lv_label_set_text(pl, parts[i]);
        lv_obj_set_style_text_font(pl, FONT_BODY, 0);
        lv_obj_set_style_text_color(pl, lv_color_hex(is_last ? COL_TEXT : COL_BLUE), 0);

        if (!is_last) {
            char *cur_accum = strdup(accum);
            lv_obj_set_user_data(pbtn, cur_accum);
            lv_obj_add_event_cb(pbtn, files_cb_path_level, LV_EVENT_CLICKED, NULL);
        }
    }
}

/* 刷新文件列表 — iOS 分组玻璃卡：实心彩色徽章 + 行 + 细分隔线 */
static void files_refresh_list(void)
{
    if (!files_list_cont) return;
    lv_obj_clean(files_list_cont);

    if (files_count == 0) {
        lv_obj_t *empty = lv_label_create(files_list_cont);
        lv_label_set_text(empty, "此目录为空");
        lv_obj_set_style_text_font(empty, FONT_BODY, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_SEC), 0);
        lv_obj_set_style_pad_top(empty, DM(40), 0);
        lv_obj_set_style_pad_bottom(empty, DM(40), 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        /* P145：空目录长按 → 操作栏（粘贴/新建文件夹入口，空白目录可粘贴） */
        lv_obj_add_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(empty, files_blank_long_cb, LV_EVENT_LONG_PRESSED, NULL);
        return;
    }

    for (uint32_t i = 0; i < files_count; i++) {
        int type = files_types[i];
        const char *name = files_names[i];

        lv_obj_t *row = lv_obj_create(files_list_cont);
        lv_obj_set_size(row, lv_pct(100), DM(56));
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, DM(16), 0);
        lv_obj_set_style_pad_right(row, DM(16), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, DM(12), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_user_data(row, (void *)(uintptr_t)i);

        lv_obj_add_event_cb(row, files_row_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(row, files_row_long_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(row, files_row_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);

        /* 选择模式：选中行浅蓝底（iOS 惯例，去掉复选框 → 修复 checkbox+row 双触） */
        if (files_select_mode && files_selected[i]) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0xD9E9FF), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }

        const char *symbol;
        uint32_t color;
        switch (type) {
            case FILE_TYPE_DIR:   symbol = LV_SYMBOL_DIRECTORY; color = COL_BLUE;   break;
            case FILE_TYPE_TXT:   symbol = LV_SYMBOL_FILE;      color = COL_SEC;    break;
            case FILE_TYPE_MP3:   symbol = LV_SYMBOL_AUDIO;     color = COL_PURPLE; break;
            case FILE_TYPE_IMG:   symbol = LV_SYMBOL_IMAGE;     color = COL_ORANGE; break;
            default:              symbol = LV_SYMBOL_FILE;      color = COL_SEC;    break;
        }

        /* 实心彩色圆角徽章 + 白色图标（对齐 Settings 行 / 游戏卡 / 书架） */
        lv_obj_t *icon_box = lv_obj_create(row);
        lv_obj_set_size(icon_box, DM(36), DM(36));
        lv_obj_set_style_radius(icon_box, RAD_CARD, 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(icon_box, 6, 0);
        lv_obj_set_style_shadow_color(icon_box, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_opa(icon_box, LV_OPA_30, 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *icon = lv_label_create(icon_box);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, FONT_ICON, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(icon);

        lv_obj_t *fname = lv_label_create(row);
        lv_label_set_text(fname, name);
        lv_obj_set_style_text_font(fname, FONT_BODY, 0);
        lv_obj_set_style_text_color(fname, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(fname, 1);
        lv_label_set_long_mode(fname, LV_LABEL_LONG_DOT);

        if (files_select_mode) {
            if (files_selected[i]) {
                lv_obj_t *ok = lv_label_create(row);
                lv_label_set_text(ok, LV_SYMBOL_OK);
                lv_obj_set_style_text_font(ok, FONT_BODY, 0);
                lv_obj_set_style_text_color(ok, lv_color_hex(COL_BLUE), 0);
            }
        } else if (type == FILE_TYPE_DIR) {
            lv_obj_t *arrow = lv_label_create(row);
            lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_font(arrow, FONT_BODY, 0);
            lv_obj_set_style_text_color(arrow, lv_color_hex(COL_SEC), 0);
        } else {
            /* 行尾大小小字（安卓文件管理器惯例） */
            long sz = files_sizes[i];
            char sizestr[24];
            if (sz < 1024)            snprintf(sizestr, sizeof(sizestr), "%ld B", sz);
            else if (sz < 1024*1024)  snprintf(sizestr, sizeof(sizestr), "%ld KB", sz / 1024);
            else                      snprintf(sizestr, sizeof(sizestr), "%ld MB", sz / (1024*1024));
            lv_obj_t *szl = lv_label_create(row);
            lv_label_set_text(szl, sizestr);
            lv_obj_set_style_text_font(szl, FONT_CAPTION, 0);
            lv_obj_set_style_text_color(szl, lv_color_hex(COL_SEC), 0);
        }

        /* 行间细分隔线（非最后一行，随分组卡圆角裁剪） */
        if (i < files_count - 1) {
            lv_obj_t *line = lv_obj_create(files_list_cont);
            lv_obj_set_size(line, lv_pct(100), 1);
            lv_obj_set_style_bg_color(line, lv_color_hex(COL_SEP), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
            lv_obj_set_style_border_width(line, 0, 0);
            lv_obj_set_style_pad_all(line, 0, 0);
            lv_obj_set_style_radius(line, 0, 0);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

/* 行点击 */
static void files_row_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(row);

    /* P0-2：长按进入选择模式后释放，LVGL 必发 CLICKED → 吞掉这次点击 */
    if (files_long_guard) { files_long_guard = 0; return; }

    /* 2026-09-10 P206 rev：行 idx 无边界检查——rescan 前后 CLICKED 延迟
     * 投递可能带过期 idx，直读 files_types/paths 越界。先卡位。 */
    if (idx < 0 || idx >= (int)files_count) return;

    if (files_select_mode) { files_toggle_select(idx); return; }

    if (files_types[idx] == FILE_TYPE_DIR) {
        strcpy(files_cur_path, files_paths[idx]);
        files_scan_dir(files_cur_path);
        files_refresh_list();
        files_update_breadcrumb();
    } else {
        files_open_file(files_paths[idx], files_types[idx]);
    }
}

/* 行长按 */
static void files_row_long_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(row);
    /* 2026-09-10 P206 rev：同上，过期 idx 直接吞（长按无 destructive 动作） */
    if (idx < 0 || idx >= (int)files_count) return;
    files_long_guard = 1;   /* P0-2：先置 guard，吞掉释放时必发的 CLICKED */
    files_enter_select_mode(idx);
}

/* 长按期间按压丢失（如长按后滑动列表）：guard 无人消费 → 清掉防吞掉下次点击 */
static void files_row_press_lost_cb(lv_event_t *e)
{
    (void)e;
    files_long_guard = 0;
}

/* P145：空白处/空目录长按 → 进入选择模式（无选中项），操作栏可粘贴/新建文件夹。
 * LVGL LONG_PRESSED 只发给按下目标（行对象无 EVENT_BUBBLE 不冒泡），
 * 行内长按由 files_row_long_cb 处理，此处仅命中容器空白区域，无需守卫。 */
static void files_blank_long_cb(lv_event_t *e)
{
    (void)e;
    if (files_select_mode) return;
    files_enter_select_mode(-1);
}

/* 选择模式 UI 切换：底部圆形操作栏 建/删 + 上级按钮让位 */
static void files_toggle_select_ui(int entering)
{
    if (entering) {
        /* 2026-08-09 修复：操作栏悬浮底部会遮挡列表最后几行 →
         * 列表底部留出操作栏高度空间（DM(36)），内容可滚动到操作栏上方 */
        if (files_list_scrl)
            lv_obj_set_style_pad_bottom(files_list_scrl, DM(36), 0);
        if (!files_sel_bar) {
            files_sel_bar = lv_obj_create(subpage_overlay);
            lv_obj_set_size(files_sel_bar, LV_SIZE_CONTENT, DM(56));
            /* 2026-08-10 修复：原白底(COL_CARD)与白色按钮叠在一起，按钮圆
             * 边界不可见（"上半部分消失"）。改浅灰磨砂底(COL_BG_GRAD)，
             * 白玻璃按钮靠明度差+柔和阴影自然浮出，符合玻璃配方、无描边更简约 */
            lv_obj_set_style_bg_color(files_sel_bar, lv_color_hex(COL_BG_GRAD), 0);
            lv_obj_set_style_bg_opa(files_sel_bar, LV_OPA_90, 0);
            lv_obj_set_style_radius(files_sel_bar, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(files_sel_bar, 1, 0);
            lv_obj_set_style_border_color(files_sel_bar, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_opa(files_sel_bar, LV_OPA_50, 0);
            lv_obj_set_style_shadow_width(files_sel_bar, 10, 0);
            lv_obj_set_style_shadow_color(files_sel_bar, lv_color_hex(0x000000), 0);
            lv_obj_set_style_shadow_opa(files_sel_bar, LV_OPA_30, 0);
            lv_obj_clear_flag(files_sel_bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(files_sel_bar, LV_ALIGN_BOTTOM_MID, 0, -DM(24));
            lv_obj_set_flex_flow(files_sel_bar, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(files_sel_bar, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(files_sel_bar, DM(6), 0);
            /* 2026-08-10 修复：原 DM(4)≈10px < skill 留白规范 20px → DM(10)≈24px */
            lv_obj_set_style_pad_column(files_sel_bar, DM(10), 0);

            /* 六个圆形按钮 — 白玻璃常态 + 蓝紫渐变按压放大（对齐音乐播放器） */
            struct { const char *sym; lv_event_cb_t cb; } btns[6] = {
                { LV_SYMBOL_COPY,  files_cb_sel_copy },
                { LV_SYMBOL_CUT,   files_cb_sel_cut },
                { LV_SYMBOL_PASTE, files_cb_sel_paste },
                { LV_SYMBOL_TRASH, files_cb_sel_delete },
                { LV_SYMBOL_PLUS,  files_cb_sel_newfolder },
                { LV_SYMBOL_CLOSE, files_cb_exit_select },
            };
            for (int i = 0; i < 6; i++) {
                lv_obj_t *b = lv_btn_create(files_sel_bar);
                lv_obj_set_size(b, DM(44), DM(44));
                lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
                lv_obj_set_style_border_width(b, 0, 0);
                lv_obj_set_style_pad_all(b, 0, 0);
                /* 2026-08-10 修复：白圆叠白条边界不可见 → 柔和阴影浮出
                 * （玻璃配方=半透明白底+柔和阴影，不描边更简约；OPA 用 10 的倍数） */
                lv_obj_set_style_shadow_width(b, 8, 0);
                lv_obj_set_style_shadow_color(b, lv_color_hex(0x000000), 0);
                lv_obj_set_style_shadow_opa(b, LV_OPA_20, 0);
                /* 按压：蓝紫渐变 + 轻微放大 + 光晕（music_round_btn 同款） */
                lv_obj_set_style_transform_width(b, 3, LV_STATE_PRESSED);
                lv_obj_set_style_transform_height(b, 3, LV_STATE_PRESSED);
                lv_obj_set_style_bg_color(b, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
                lv_obj_set_style_bg_grad_color(b, lv_color_hex(COL_PURPLE), LV_STATE_PRESSED);
                lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
                lv_obj_set_style_shadow_width(b, 14, LV_STATE_PRESSED);
                lv_obj_set_style_shadow_color(b, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
                lv_obj_set_style_shadow_opa(b, LV_OPA_40, LV_STATE_PRESSED);
                lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
                lv_obj_t *l = lv_label_create(b);
                lv_label_set_text(l, btns[i].sym);
                lv_obj_set_style_text_font(l, FONT_ICON, 0);
                lv_obj_set_style_text_color(l, lv_color_hex(COL_BLUE), 0);
                lv_obj_center(l);
                /* icon 不继承父 PRESSED，显式按下变白（与播放键一致） */
                lv_obj_add_event_cb(b, music_round_btn_press_icon_cb,
                                    LV_EVENT_PRESSED, l);
                lv_obj_add_event_cb(b, music_round_btn_press_icon_cb,
                                    LV_EVENT_RELEASED, l);
                lv_obj_add_event_cb(b, music_round_btn_press_icon_cb,
                                    LV_EVENT_PRESS_LOST, l);
                if (i == 2) files_sel_paste_lbl = l;   /* 剪贴板空 → 置灰 */
            }
        }
        if (files_sel_paste_lbl)
            lv_obj_set_style_text_color(files_sel_paste_lbl,
                lv_color_hex(clip_count ? COL_BLUE : 0xC6C6C8), 0);
        if (files_up_btn) lv_obj_add_flag(files_up_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (files_sel_bar) {
            lv_obj_del(files_sel_bar);
            files_sel_bar = NULL;
            files_sel_paste_lbl = NULL;
        }
        files_update_up_btn();
    }
}

/* 进入选择模式（first_idx=-1 = 空白长按：无选中项，仅弹操作栏） */
static void files_enter_select_mode(int first_idx)
{
    files_select_mode = 1;
    files_sel_count = 0;
    memset(files_selected, 0, sizeof(files_selected));
    /* 2026-09-10 P206 rev：first_idx 卡上界（files_selected 定长 128） */
    if (first_idx >= 0 && first_idx < (int)files_count) {
        files_selected[first_idx] = 1;
        files_sel_count = 1;
    }
    files_toggle_select_ui(1);
    files_refresh_list();
}

/* 退出选择模式 */
static void files_exit_select_mode(void)
{
    files_select_mode = 0;
    files_sel_count = 0;
    memset(files_selected, 0, sizeof(files_selected));
    files_toggle_select_ui(0);
    files_refresh_list();
}

/* 切换选择 */
static void files_toggle_select(int idx)
{
    if (idx < 0 || idx >= (int)files_count) return;
    files_selected[idx] = !files_selected[idx];
    files_sel_count += files_selected[idx] ? 1 : -1;
    if (files_sel_count == 0) { files_exit_select_mode(); return; }
    files_refresh_list();
}

/* 复制/剪贴：收集选中项 → 退出选择模式 */
static void files_cb_copy(lv_event_t *e)
{
    (void)e;
    if (files_copy_running) { files_toast_show("正在粘贴中，请稍候"); return; }
    clip_count = 0; clip_op_cut = 0;
    for (uint32_t i = 0; i < files_count && clip_count < FILES_MAX_CLIPBOARD; i++)
        if (files_selected[i]) { strncpy(clip_paths[clip_count], files_paths[i], sizeof(clip_paths[0]) - 1); clip_count++; }
    files_exit_select_mode();
    if (clip_count > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已复制 %d 项", clip_count);
        files_toast_show(msg);
    }
}

static void files_cb_cut(lv_event_t *e)
{
    (void)e;
    if (files_copy_running) { files_toast_show("正在粘贴中，请稍候"); return; }
    /* 黑名单命中则 toast 拒绝（系统目录禁止移走） */
    for (uint32_t i = 0; i < files_count; i++)
        if (files_selected[i] && files_is_protected(files_paths[i])) {
            files_toast_show("系统目录禁止移动");
            return;
        }
    clip_count = 0; clip_op_cut = 1;
    for (uint32_t i = 0; i < files_count && clip_count < FILES_MAX_CLIPBOARD; i++)
        if (files_selected[i]) { strncpy(clip_paths[clip_count], files_paths[i], sizeof(clip_paths[0]) - 1); clip_count++; }
    files_exit_select_mode();
    if (clip_count > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已剪切 %d 项", clip_count);
        files_toast_show(msg);
    }
}

/* P145：粘贴目标目录快照（后台线程只读，防粘贴期间切目录） */
static char files_paste_dst_dir[256];

/* P145：路径拼接（strncpy/strncat 让编译器可证不越界，消除 -Wformat-truncation） */
static void files_join_path(char *dst, int dst_size, const char *dir, const char *name)
{
    strncpy(dst, dir, dst_size - 1); dst[dst_size - 1] = '\0';
    strncat(dst, "/", dst_size - (int)strlen(dst) - 1);
    strncat(dst, name, dst_size - (int)strlen(dst) - 1);
}

/* P1-9：目标路径已存在 → 自动改名「原名 (n).ext」去重（dir=目标目录） */
static void files_unique_dst(const char *dir, const char *src_name, char *dst, int dst_size)
{
    struct stat st;
    if (stat(dst, &st) != 0) return;   /* 无冲突，直接用原名 */

    /* 拆扩展名：base + ext（.txt/.mp3/…，目录无扩展） */
    char base[256], ext[32] = "";
    const char *dot = strrchr(src_name, '.');
    if (dot && dot != src_name) {
        int bl = (int)(dot - src_name);
        if (bl >= (int)sizeof(base)) bl = (int)sizeof(base) - 1;
        memcpy(base, src_name, bl); base[bl] = '\0';
        strncpy(ext, dot, sizeof(ext) - 1); ext[sizeof(ext) - 1] = '\0';
    } else {
        strncpy(base, src_name, sizeof(base) - 1); base[sizeof(base) - 1] = '\0';
    }

    for (int k = 1; k < 1000; k++) {
        char num[16];
        snprintf(num, sizeof(num), " (%d)", k);
        dst[0] = '\0';
        strncat(dst, dir, dst_size - 1);
        strncat(dst, "/", dst_size - (int)strlen(dst) - 1);
        strncat(dst, base, dst_size - (int)strlen(dst) - 1);
        strncat(dst, num, dst_size - (int)strlen(dst) - 1);
        strncat(dst, ext, dst_size - (int)strlen(dst) - 1);
        if (stat(dst, &st) != 0) return;   /* 找到可用名 */
    }
}

/* P145：递归统计源总字节（进度分母） */
static long files_stat_total(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (long)st.st_size;

    long total = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[512]; snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        total += files_stat_total(full);
    }
    closedir(dir);
    return total;
}

/* P145：递归复制（目录逐层建，文件按块读入并累加进度字节） */
static int files_copy_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0777) != 0 && errno != EEXIST) return -1;
        DIR *dir = opendir(src);
        if (!dir) return -1;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char s[512], d[512];
            snprintf(s, sizeof(s), "%s/%s", src, ent->d_name);
            snprintf(d, sizeof(d), "%s/%s", dst, ent->d_name);
            if (files_copy_recursive(s, d) != 0) { closedir(dir); return -1; }
        }
        closedir(dir);
        return 0;
    }

    int fi = open(src, O_RDONLY);
    if (fi < 0) return -1;
    int fo = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fo < 0) { close(fi); return -1; }

    static char buf[FILES_COPY_BUF];
    ssize_t r;
    while ((r = read(fi, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(fo, buf + off, r - off);
            if (w <= 0) { close(fi); close(fo); return -1; }
            off += w;
        }
        files_copy_done_bytes += r;
    }
    close(fi);
    close(fo);
    return (r < 0) ? -1 : 0;   /* r==0 正常读完；r<0 读错误 */
}

/* P145：后台复制线程——先统计总字节，再逐项复制/剪切，结束后置 done */
static void *files_copy_worker(void *arg)
{
    (void)arg;
    int n = 0, renamed = 0;
    files_copy_total = 0;
    for (int i = 0; i < clip_count; i++) {
        const char *nm = strrchr(clip_paths[i], '/'); nm = nm ? nm + 1 : clip_paths[i];
        char dst[512]; files_join_path(dst, sizeof(dst), files_paste_dst_dir, nm);
        if (strcmp(dst, clip_paths[i]) == 0) continue;      /* 同目录自粘贴跳过 */
        if (stat(dst, &(struct stat){0}) == 0) renamed++;   /* 重名将自动改名 */
        files_copy_total += files_stat_total(clip_paths[i]);
    }

    files_copy_done_bytes = 0;
    for (int i = 0; i < clip_count; i++) {
        /* 黑名单剪切源跳过（入口已拦，此处纵深防御防删系统目录） */
        if (clip_op_cut && files_is_protected(clip_paths[i]))
            continue;
        const char *nm = strrchr(clip_paths[i], '/'); nm = nm ? nm + 1 : clip_paths[i];
        char dst[512]; files_join_path(dst, sizeof(dst), files_paste_dst_dir, nm);
        if (strcmp(dst, clip_paths[i]) == 0) continue;
        files_unique_dst(files_paste_dst_dir, nm, dst, sizeof(dst));

        if (clip_op_cut) {
            if (rename(clip_paths[i], dst) == 0) { n++; continue; }
            /* 跨文件系统 rename 失败 → 降级为复制+删源 */
            if (files_copy_recursive(clip_paths[i], dst) == 0) { unlink(clip_paths[i]); n++; continue; }
            files_copy_ok = 1;
        } else {
            if (files_copy_recursive(clip_paths[i], dst) != 0) files_copy_ok = 1;
            else n++;
        }
    }
    files_copy_n = n;
    files_copy_renamed = renamed;
    files_copy_done = 1;
    return NULL;
}

/* P145：进度条 UI（玻璃卡：标题 + 环形进度 + 百分比），定时器轮询刷新 */
static void files_copy_ui_show(void)
{
    if (!subpage_overlay || files_copy_ui) return;
    lv_obj_t *box = lv_obj_create(subpage_overlay);
    files_copy_ui = box;
    lv_obj_set_size(box, DM(220), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_radius(box, RAD_CARD, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(box, 16, 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(box, DM(12), 0);
    lv_obj_set_style_pad_column(box, DM(8), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(box);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, "正在复制…");
    lv_obj_set_style_text_font(t, FONT_BODY, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT), 0);

    files_copy_arc = lv_arc_create(box);
    lv_obj_set_size(files_copy_arc, DM(96), DM(96));
    lv_arc_set_bg_angles(files_copy_arc, 0, 360);
    lv_arc_set_angles(files_copy_arc, 0, 0);
    lv_obj_set_style_arc_color(files_copy_arc, lv_color_hex(0xE5E5EA), LV_PART_MAIN);
    lv_obj_set_style_arc_color(files_copy_arc, lv_color_hex(COL_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(files_copy_arc, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(files_copy_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(files_copy_arc, 8, 0);
    lv_obj_set_style_bg_color(files_copy_arc, lv_color_hex(COL_BLUE), LV_PART_KNOB);
    lv_obj_remove_flag(files_copy_arc, LV_OBJ_FLAG_CLICKABLE);

    files_copy_pct_lbl = lv_label_create(files_copy_arc);
    lv_label_set_text(files_copy_pct_lbl, "0%");
    lv_obj_set_style_text_font(files_copy_pct_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(files_copy_pct_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(files_copy_pct_lbl);

    files_copy_timer = lv_timer_create(files_copy_timer_cb, 100, NULL);
}

/* P145：进度轮询——刷新环/百分比，完成则收尾（清理 UI + toast + 刷新列表） */
static void files_copy_timer_cb(lv_timer_t *t)
{
    if (!files_copy_ui) { lv_timer_del(t); files_copy_timer = NULL; return; }   /* 子页已关，自删防空指针 */

    long total = files_copy_total;
    long done = files_copy_done_bytes;
    if (total <= 0) total = 1;   /* 目录/空文件防除零 */
    int pct = (int)((done * 100) / total);
    if (pct > 100) pct = 100;
    lv_arc_set_angles(files_copy_arc, 0, (360 * pct) / 100);
    lv_label_set_text_fmt(files_copy_pct_lbl, "%d%%", pct);

    if (!files_copy_done) return;

    lv_timer_del(t); files_copy_timer = NULL;
    files_copy_running = 0;
    lv_obj_del(files_copy_ui); files_copy_ui = NULL;
    files_copy_arc = NULL; files_copy_pct_lbl = NULL;

    clip_count = 0; clip_op_cut = 0;
    files_scan_dir(files_cur_path); files_refresh_list();
    if (files_sel_paste_lbl)
        lv_obj_set_style_text_color(files_sel_paste_lbl, lv_color_hex(0xC6C6C8), 0);

    if (files_copy_n > 0) {
        char msg[64];
        if (files_copy_renamed > 0)
            snprintf(msg, sizeof(msg), "已粘贴 %d 项（%d 项重名已改名）", files_copy_n, files_copy_renamed);
        else
            snprintf(msg, sizeof(msg), "已粘贴 %d 项", files_copy_n);
        files_toast_show(msg);
    } else {
        files_toast_show(files_copy_ok ? "粘贴失败" : "已在当前目录，无需粘贴");
    }
}

/* 粘贴核心：快照目标目录 → 启动后台复制线程（进度条不卡 UI） */
static void files_clip_paste(void)
{
    if (clip_count == 0) return;
    if (files_copy_running) return;   /* 上一次粘贴未结束，忽略重入 */
    /* 黑名单命中则 toast 拒绝（系统目录禁止粘入） */
    if (files_is_protected(files_cur_path)) {
        files_toast_show("系统目录禁止粘贴");
        return;
    }

    strcpy(files_paste_dst_dir, files_cur_path);   /* 快照，线程只读 */
    files_copy_total = 0; files_copy_done_bytes = 0;
    files_copy_done = 0; files_copy_ok = 0;
    files_copy_n = 0; files_copy_renamed = 0;
    files_copy_running = 1;

    files_copy_ui_show();                            /* 进度条 UI（定时器轮询） */
    if (pthread_create(&files_copy_thread, NULL, files_copy_worker, NULL) != 0) {
        /* 线程创建失败 → 回滚，避免进度条卡死 */
        files_copy_running = 0;
        if (files_copy_timer) { lv_timer_del(files_copy_timer); files_copy_timer = NULL; }
        if (files_copy_ui) { lv_obj_del(files_copy_ui); files_copy_ui = NULL; }
        files_copy_arc = NULL; files_copy_pct_lbl = NULL;
        files_toast_show("粘贴失败");
        return;
    }
    pthread_detach(files_copy_thread);
}

/* P145：粘贴入口——重名 → 弹窗确认（确认后自动重命名）；无重名 → 直接粘贴 */
static void files_cb_sel_paste(lv_event_t *e)
{
    (void)e;
    if (clip_count == 0) return;
    /* 黑名单命中则 toast 拒绝（系统目录禁止粘入，先于重名弹窗检查） */
    if (files_is_protected(files_cur_path)) {
        files_toast_show("系统目录禁止粘贴");
        return;
    }
    int c = files_paste_check_conflicts();
    if (c > 0 && !clip_paste_confirmed) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%d 个文件与目标目录重名，\n确定粘贴后将自动重命名，继续吗？", c);
        files_dialog_show(msg, files_paste_confirm_go);
        return;
    }
    files_clip_paste();
}

/* P145：重名确认 OK → 标记已确认并执行粘贴（自动重命名去重） */
static void files_paste_confirm_go(void)
{
    clip_paste_confirmed = 1;
    files_clip_paste();
    clip_paste_confirmed = 0;
}

/* P145：统计剪贴板与当前目录重名项数（同目录自粘贴不算） */
static int files_paste_check_conflicts(void)
{
    int c = 0;
    struct stat st;
    for (int i = 0; i < clip_count; i++) {
        const char *name = strrchr(clip_paths[i], '/'); name = name ? name + 1 : clip_paths[i];
        char dst[512]; files_join_path(dst, sizeof(dst), files_cur_path, name);
        if (strcmp(dst, clip_paths[i]) == 0) continue;
        if (stat(dst, &st) == 0) c++;
    }
    return c;
}

/* ── P0-3/P1-5：操作反馈 toast（底部居中深色胶囊，2.5s 自动隐藏）── */
static void files_toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (files_toast) {
        lv_obj_del(files_toast);
        files_toast = NULL;
    }
    files_toast_timer = NULL;
}

static void files_toast_show(const char *msg)
{
    if (!subpage_overlay) return;
    if (files_toast_timer) { lv_timer_del(files_toast_timer); files_toast_timer = NULL; }
    if (files_toast) { lv_obj_del(files_toast); files_toast = NULL; }

    files_toast = lv_obj_create(subpage_overlay);
    lv_obj_set_size(files_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(files_toast, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(files_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(files_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(files_toast, 0, 0);
    lv_obj_set_style_pad_all(files_toast, DM(6), 0);
    lv_obj_set_style_pad_left(files_toast, DM(10), 0);
    lv_obj_set_style_pad_right(files_toast, DM(10), 0);
    lv_obj_clear_flag(files_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(files_toast, LV_OBJ_FLAG_CLICKABLE);
    /* 2026-08-09 修复：选择模式下操作栏在底部（-DM(24)），toast 若仍
     * -DM(40) 会与操作栏重叠 → 选择模式时上移到操作栏上方 */
    lv_obj_align(files_toast, LV_ALIGN_BOTTOM_MID, 0,
                 files_select_mode ? -DM(110) : -DM(40));
    lv_obj_move_foreground(files_toast);

    lv_obj_t *l = lv_label_create(files_toast);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);

    files_toast_timer = lv_timer_create(files_toast_timer_cb, 2500, NULL);
    lv_timer_set_repeat_count(files_toast_timer, 1);
}

/* ── P0-3/P145：确认弹窗（玻璃卡：取消/确认）── */
static void (*files_dialog_ok_cb)(void);   /* P145：确认后执行的回调（删除/粘贴重命名） */

static void files_dialog_close(void)
{
    if (files_dialog) {
        lv_obj_del(files_dialog);
        files_dialog = NULL;
    }
}

static void files_cb_dialog_cancel(lv_event_t *e)
{
    (void)e;
    files_dialog_close();
}

static void files_cb_dialog_ok(lv_event_t *e)
{
    (void)e;
    void (*cb)(void) = files_dialog_ok_cb;
    files_dialog_ok_cb = NULL;
    files_dialog_close();
    if (cb) cb();
}

static void files_dialog_show(const char *msg, void (*ok_cb)(void))
{
    if (!subpage_overlay) return;
    files_dialog_close();
    files_dialog_ok_cb = ok_cb;

    lv_obj_t *dlg = lv_obj_create(subpage_overlay);
    files_dialog = dlg;
    lv_obj_set_size(dlg, DM(220), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_90, 0);
    lv_obj_set_style_radius(dlg, RAD_CARD, 0);
    lv_obj_set_style_border_width(dlg, 1, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(dlg, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(dlg, 16, 0);
    lv_obj_set_style_shadow_opa(dlg, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(dlg, DM(8), 0);
    lv_obj_set_style_pad_column(dlg, DM(6), 0);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(dlg);

    /* 提示文案 */
    lv_obj_t *m = lv_label_create(dlg);
    lv_label_set_text(m, msg);
    lv_obj_set_style_text_font(m, FONT_BODY, 0);
    lv_obj_set_style_text_color(m, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(m, lv_pct(100));

    /* 按钮行：取消（灰） + 删除（红） */
    lv_obj_t *row = lv_obj_create(dlg);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, DM(6), 0);

    lv_obj_t *cancel = lv_btn_create(row);
    lv_obj_set_size(cancel, DM(40), DM(16));
    lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xE5E5EA), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, files_cb_dialog_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, FONT_BODY, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_btn_create(row);
    lv_obj_set_size(ok, DM(40), DM(16));
    lv_obj_set_style_radius(ok, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_add_event_cb(ok, files_cb_dialog_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "删除");
    lv_obj_set_style_text_font(ol, FONT_BODY, 0);
    lv_obj_set_style_text_color(ol, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ol);
}

/* 系统目录黑名单：/、/resource、/etc、/proc、/sys、/dev
 * 及其子目录禁止删除/移入/粘入（防删固件字体与系统配置） */
static int files_is_protected(const char *path)
{
    static const char *roots[] = { "/resource", "/etc", "/proc", "/sys", "/dev" };
    if (!path || !path[0])
        return 1;
    /* 去尾部 '/'（"/" 本体保留） */
    char norm[512];
    strncpy(norm, path, sizeof(norm) - 1); norm[sizeof(norm) - 1] = '\0';
    int len = (int)strlen(norm);
    while (len > 1 && norm[len - 1] == '/') norm[--len] = '\0';
    if (strcmp(norm, "/") == 0)
        return 1;
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        int rlen = (int)strlen(roots[i]);
        if (strcmp(norm, roots[i]) == 0)
            return 1;
        if (strncmp(norm, roots[i], rlen) == 0 && norm[rlen] == '/')
            return 1;
    }
    return 0;
}

/* mkdir -p 等价（mkdir(2) 逐级建，替代 system("mkdir -p") 防注入） */
static int files_mkdir_p(const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
    int len = (int)strlen(tmp);
    if (len <= 0)
        return -1;
    /* 逐级建：跳过首 '/'，遇到 '/' 即建父级 */
    for (int i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char save = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0' && mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = save;
        }
    }
    return 0;
}

/* 递归删除路径（替代 system("rm -rf")，防命令注入） */
static int files_rmrf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return -1;
        struct dirent *ent;
        int ret = 0;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (files_rmrf(child) != 0) ret = -1;
        }
        closedir(dir);
        if (rmdir(path) != 0) ret = -1;
        return ret;
    }
    return unlink(path);
}

/* 删除（确认弹窗 OK 后执行；黑名单路径跳过不删，防误删系统目录） */
static void files_do_delete(void)
{
    int n = 0;
    for (uint32_t i = 0; i < files_count; i++)
        if (files_selected[i]) {
            if (files_is_protected(files_paths[i]))
                continue;   /* 黑名单跳过（入口已拦，此处纵深防御） */
            files_rmrf(files_paths[i]);
            n++;
        }
    files_exit_select_mode();
    files_scan_dir(files_cur_path);
    files_refresh_list();
    if (n > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "已删除 %d 项", n);
        files_toast_show(msg);
    }
}

static void files_cb_delete(lv_event_t *e)
{
    (void)e;
    if (files_sel_count <= 0) return;
    /* 黑名单命中则 toast 拒绝（删除/移走系统目录禁止） */
    for (uint32_t i = 0; i < files_count; i++)
        if (files_selected[i] && files_is_protected(files_paths[i])) {
            files_toast_show("系统目录禁止删除");
            return;
        }
    /* 确认框带路径（父目录 + 前 3 个名字），不只显示个数 */
    char msg[256];
    int off = snprintf(msg, sizeof(msg), "确定删除 %s 下的 %d 项？",
                       files_cur_path, files_sel_count);
    /* snprintf 返回"理论长度"，长路径时可超过 buf → 必须先钳位，否则后续
     * sizeof(msg) - off 下溢成巨大 size_t，msg + off 越界写 */
    if (off < 0)
        off = 0;
    if (off > (int)sizeof(msg) - 1)
        off = (int)sizeof(msg) - 1;
    int shown = 0;
    for (uint32_t i = 0; i < files_count && shown < 3; i++) {
        if (!files_selected[i])
            continue;
        if (off >= (int)sizeof(msg) - 24)
            break;
        int n = snprintf(msg + off, sizeof(msg) - off, "\n%s", files_names[i]);
        if (n < 0)
            break;
        off += n;
        shown++;
    }
    if (files_sel_count > shown && off < (int)sizeof(msg) - 8)
        snprintf(msg + off, sizeof(msg) - off, "\n…");
    files_dialog_show(msg, files_do_delete);
}

/* ═══════════ 图片查看器（2026-09-11）═══════════
 * 文件浏览器点 .png/.jpg → 全屏 overlay：
 *   - 图片等比适配屏幕（留边），不强行拉伸
 *   - 右上角白色关闭圆钮（LV_SYMBOL_CLOSE）
 *   - 底部 −/+ 缩放（step 25%，下限 fit/4、上限 4x）
 *   - 点图片外的背景也可关闭（图片本身不接点击，事件冒泡到 overlay） */
static void files_img_viewer_close(void)
{
    if (files_img_overlay) {
        lv_obj_del(files_img_overlay);
        files_img_overlay = NULL;
        files_img_widget = NULL;
    }
}

static void files_img_close_cb(lv_event_t *e)  { (void)e; files_img_viewer_close(); }
static void files_img_backdrop_cb(lv_event_t *e) { (void)e; files_img_viewer_close(); }

static void files_img_apply_zoom(int dir)
{
    uint32_t next;

    if (files_img_widget == NULL) return;
    next = (dir > 0) ? files_img_scale + files_img_scale / 4
                     : files_img_scale - files_img_scale / 4;
    if (next > FILES_IMG_SCALE_MAX)       next = FILES_IMG_SCALE_MAX;
    if (next < files_img_fit_scale / 4)   next = files_img_fit_scale / 4;
    if (next < FILES_IMG_SCALE_MIN)       next = FILES_IMG_SCALE_MIN;
    files_img_scale = next;
    lv_image_set_scale(files_img_widget, files_img_scale);
}

static void files_img_zoom_in_cb(lv_event_t *e)  { (void)e; files_img_apply_zoom(1); }
static void files_img_zoom_out_cb(lv_event_t *e) { (void)e; files_img_apply_zoom(-1); }

/* 圆形玻璃按钮：图标居中 */
static lv_obj_t *files_img_round_btn(lv_obj_t *parent, const char *sym,
                                     lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, DM(44), DM(44));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_font(lbl, FONT_ICON, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl);
    return btn;
}

static void files_img_viewer_open(const char *path)
{
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());
    lv_image_header_t hdr;
    lv_obj_t *parent;
    lv_obj_t *btn;
    uint32_t fit = 256;

    if (path == NULL || path[0] == '\0') return;
    if (files_img_overlay) files_img_viewer_close();

    snprintf(files_img_path, sizeof(files_img_path), "%s", path);

    /* 先探尺寸：不支持的格式/坏文件 → toast，不进空 overlay */
    if (lv_image_decoder_get_info(files_img_path, &hdr) != LV_RESULT_OK ||
        hdr.w == 0 || hdr.h == 0) {
        files_toast_show("无法打开此图片");
        return;
    }
    /* 防超大图解码 OOM（约 4B/px；图片缓存上限 64MB） */
    if ((uint32_t)hdr.w * (uint32_t)hdr.h > 12u * 1024u * 1024u) {
        files_toast_show("图片过大，暂不支持");
        return;
    }

    parent = subpage_overlay ? subpage_overlay : lv_scr_act();
    files_img_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(files_img_overlay);
    lv_obj_set_size(files_img_overlay, sw, sh);
    lv_obj_set_style_bg_color(files_img_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(files_img_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(files_img_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(files_img_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(files_img_overlay, files_img_backdrop_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(files_img_overlay);

    files_img_widget = lv_image_create(files_img_overlay);
    lv_obj_set_size(files_img_widget, sw, sh);
    lv_obj_set_style_bg_opa(files_img_widget, LV_OPA_0, 0);
    lv_obj_set_style_border_width(files_img_widget, 0, 0);
    lv_obj_clear_flag(files_img_widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(files_img_widget, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_inner_align(files_img_widget, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_src(files_img_widget, files_img_path);

    /* 等比适配（留边 DM(48)） */
    {
        uint32_t avail_w = (uint32_t)(sw > DM(48) ? sw - DM(48) : sw);
        uint32_t avail_h = (uint32_t)(sh > DM(48) ? sh - DM(48) : sh);
        uint32_t sx = avail_w * 256u / (uint32_t)hdr.w;
        uint32_t sy = avail_h * 256u / (uint32_t)hdr.h;
        fit = (sx < sy) ? sx : sy;
        if (fit < FILES_IMG_SCALE_MIN) fit = FILES_IMG_SCALE_MIN;
    }
    files_img_fit_scale = fit;
    files_img_scale = fit;
    lv_image_set_scale(files_img_widget, files_img_scale);

    /* 右上角关闭（必须显眼，防全屏出不去） */
    btn = files_img_round_btn(files_img_overlay, LV_SYMBOL_CLOSE,
                              files_img_close_cb);
    lv_obj_set_pos(btn, sw - DM(60), DM(16));
    assert(sw - DM(60) >= 0 && sw - DM(60) + DM(44) <= sw &&
           DM(16) + DM(44) <= sh);

    /* 底部缩放 −/+ */
    btn = files_img_round_btn(files_img_overlay, LV_SYMBOL_MINUS,
                              files_img_zoom_out_cb);
    lv_obj_set_pos(btn, sw / 2 - DM(56), sh - DM(64));
    assert(sw / 2 - DM(56) >= 0 && sw / 2 - DM(56) + DM(44) <= sw &&
           sh - DM(64) + DM(44) <= sh);
    btn = files_img_round_btn(files_img_overlay, LV_SYMBOL_PLUS,
                              files_img_zoom_in_cb);
    lv_obj_set_pos(btn, sw / 2 + DM(12), sh - DM(64));
    assert(sw / 2 + DM(12) >= 0 && sw / 2 + DM(12) + DM(44) <= sw &&
           sh - DM(64) + DM(44) <= sh);
}

/* 打开文件（.txt → Books 阅读器；.mp3/.wav → Music 播放；其他 → toast 提示） */
static void files_open_file(const char *path, int type)
{
    if (type == FILE_TYPE_TXT) {
        for (uint32_t i = 0; i < dm_book_count; i++)
            if (strcmp(dm_book_paths[i], path) == 0) { book_open_reader(i); return; }
        if (dm_book_count < DM_MAX_BOOKS) {
            strncpy(dm_book_paths[dm_book_count], path, sizeof(dm_book_paths[0]) - 1);
            dm_book_paths[dm_book_count][sizeof(dm_book_paths[0]) - 1] = '\0';
            const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
            const char *dot = strrchr(name, '.');
            int len = dot ? (int)(dot - name) : strlen(name);
            snprintf(dm_book_titles[dm_book_count], sizeof(dm_book_titles[0]), "%.*s", len, name);
            dm_utf8_trim(dm_book_titles[dm_book_count]);   /* 2026-09-14：截断勿劈开多字节尾 */
            book_open_reader(dm_book_count); dm_book_count++;
        }
    } else if (type == FILE_TYPE_MP3) {
        /* P145：按路径播放（不污染曲库 dm_tracks）——文件任意目录可播，
         * 播放器页/锁屏标题走 dm_now_* 正确显示，不再受列表扫描影响。 */
        music_play_path(path);
        files_toast_show("正在播放");
    } else if (type == FILE_TYPE_IMG) {
        /* 2026-09-11：图片查看器（全屏 overlay + 关闭按钮，防出不去） */
        files_img_viewer_open(path);
    } else {
        /* OTHER：暂无查看器 → 提示反馈（P1-7 消灭交互死区） */
        files_toast_show("暂不支持打开此类文件");
    }
}

/* 选择模式操作 */
static void files_cb_sel_delete(lv_event_t *e) { (void)e; files_cb_delete(NULL); }
static void files_cb_sel_copy(lv_event_t *e) { (void)e; files_cb_copy(NULL); }
static void files_cb_sel_cut(lv_event_t *e) { (void)e; files_cb_cut(NULL); }
static void files_cb_exit_select(lv_event_t *e) { (void)e; files_exit_select_mode(); }

/* 操作栏"新建文件夹"：固定名 NewFolder，已存在则自动加序号去重
 * （mkdir(2) 逐级建，替代 system("mkdir -p") 防 shell 注入） */
static void files_cb_sel_newfolder(lv_event_t *e)
{
    (void)e;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/NewFolder", files_cur_path);
    struct stat st;
    for (int k = 1; stat(dir, &st) == 0; k++)   /* 同名已存在 → NewFolder1/2/… */
        snprintf(dir, sizeof(dir), "%s/NewFolder%d", files_cur_path, k);
    /* 逐级建目录（mkdir(2)，无 shell，不拼接命令） */
    if (files_mkdir_p(dir) != 0) {
        files_toast_show("创建文件夹失败");
        return;
    }
    files_scan_dir(files_cur_path); files_refresh_list();
    files_toast_show("已创建文件夹");
}

/* 返回上级（选择模式下=取消选择） */
static void files_go_up(void)
{
    if (files_select_mode) { files_exit_select_mode(); return; }
    /* 已到根目录 → 再按一次退出子页 */
    if (strcmp(files_cur_path, "/") == 0 || strcmp(files_cur_path, "/sdcard") == 0) {
        close_subpage();
        return;
    }
    char *last = strrchr(files_cur_path, '/');
    if (last && last != files_cur_path) *last = '\0';
    else { files_cur_path[0] = '/'; files_cur_path[1] = '\0'; }
    files_scan_dir(files_cur_path); files_refresh_list(); files_update_breadcrumb();
}

/* 面包屑回调 */
static void files_cb_path_root(lv_event_t *e)
{
    (void)e;
    if (files_select_mode) { files_exit_select_mode(); return; }
    bool is_sd = (strcmp(files_cur_path, "/sdcard") == 0 || strncmp(files_cur_path, "/sdcard/", 7) == 0);
    strcpy(files_cur_path, is_sd ? "/sdcard" : "/");
    files_scan_dir(files_cur_path); files_refresh_list(); files_update_breadcrumb();
}

static void files_cb_path_level(lv_event_t *e)
{
    /* P0-1 修复：路径存在对象 user_data（lv_obj_set_user_data），
     * 必须用 lv_obj_get_user_data 取；lv_event_get_user_data 返回的是
     * 注册回调时传入的 user_data（此处为 NULL）→ 原实现恒 NULL 点击无效。 */
    lv_obj_t *btn = lv_event_get_target(e);
    char *cur_accum = (char*)lv_obj_get_user_data(btn);
    if (!cur_accum) return;
    if (files_select_mode) { files_exit_select_mode(); }
    strcpy(files_cur_path, cur_accum);
    /* P0-4：free 后必须置 NULL，否则下次 files_update_breadcrumb
     * 的回收循环会对同一指针二次 free（原注释承诺置 NULL 但代码没有） */
    free(cur_accum);
    lv_obj_set_user_data(btn, NULL);
    files_scan_dir(files_cur_path); files_refresh_list(); files_update_breadcrumb();
}

/* 创建 Files 子页 — 专业平板文件管理器 */
void ui_files_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    /* 2026-08-09 修复：原 pad_row=0 导致 app_bar/快捷入口/面包屑/列表
     * 四层卡片 0 间距贴边重叠；加 DM(6) 间距让阴影/边框自然区分
     * （参照 Settings 分组卡之间留白） */
    lv_obj_set_style_pad_row(parent, DM(6), 0);

    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());

    /* ── 顶部栏（玻璃风格）── */
    lv_obj_t *app_bar = lv_obj_create(parent);
    lv_obj_set_size(app_bar, lv_pct(100), DM(56));
    lv_obj_set_style_bg_color(app_bar, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(app_bar, LV_OPA_90, 0);
    lv_obj_set_style_radius(app_bar, RAD_CARD, 0);
    lv_obj_set_style_border_width(app_bar, 1, 0);
    lv_obj_set_style_border_color(app_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(app_bar, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(app_bar, 4, 0);
    lv_obj_set_style_shadow_opa(app_bar, LV_OPA_10, 0);
    lv_obj_set_style_pad_left(app_bar, DM(12), 0);
    lv_obj_set_style_pad_right(app_bar, DM(12), 0);
    lv_obj_set_flex_flow(app_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(app_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(app_bar, DM(8), 0);
    lv_obj_clear_flag(app_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* P1-8 导航去重：左上角返回（退出 Files 页）由 show_subpage 框架
     * 的 back_btn 提供，此处不再自建返回键；仅保留标题 + 上级键。 */

    /* 标题 */
    lv_obj_t *title = lv_label_create(app_bar);
    lv_label_set_text(title, "文件");
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(title, 1);

    /* 「上级」圆钮 — 进子目录后显示，根目录隐藏（files_update_up_btn 控制） */
    files_up_btn = lv_btn_create(app_bar);
    lv_obj_set_size(files_up_btn, DM(40), DM(40));
    lv_obj_set_style_radius(files_up_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(files_up_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(files_up_btn, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(files_up_btn, 0, 0);
    lv_obj_set_style_border_width(files_up_btn, 0, 0);
    lv_obj_add_event_cb(files_up_btn, files_go_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(files_up_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *up_icon = lv_label_create(files_up_btn);
    lv_label_set_text(up_icon, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(up_icon, FONT_ICON, 0);
    lv_obj_set_style_text_color(up_icon, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(up_icon);

    /* ── 快捷入口（内部存储 + TF卡）── */
    lv_obj_t *quick_cont = lv_obj_create(parent);
    lv_obj_set_size(quick_cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(quick_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(quick_cont, 0, 0);
    lv_obj_set_style_pad_all(quick_cont, 0, 0);
    lv_obj_set_flex_flow(quick_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(quick_cont, CARD_GAP, 0);

    /* 内部存储快捷入口 */
    lv_obj_t *internal_btn = lv_btn_create(quick_cont);
    lv_obj_set_size(internal_btn, (sw - 2 * EDGE_PAD - CARD_GAP) / 2, DM(56));
    lv_obj_set_style_bg_color(internal_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(internal_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(internal_btn, RAD_CARD, 0);
    lv_obj_set_style_border_width(internal_btn, 1, 0);
    lv_obj_set_style_border_color(internal_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(internal_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(internal_btn, 6, 0);
    lv_obj_set_style_shadow_opa(internal_btn, LV_OPA_10, 0);
    lv_obj_add_event_cb(internal_btn, files_cb_internal_root, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(internal_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(internal_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(internal_btn, DM(12), 0);
    lv_obj_t *ib = lv_obj_create(internal_btn);
    lv_obj_set_size(ib, DM(36), DM(36));
    lv_obj_set_style_radius(ib, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ib, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(ib, LV_OPA_20, 0);
    lv_obj_clear_flag(ib, LV_OBJ_FLAG_CLICKABLE);
    /* 2026-08-10 修复（-26）：原仅清 CLICKABLE，漏关滚动条——FONT_ICON 36px
     * 字号变大后内容盒超出容器边缘，AUTO 滚动条显示为"上下拉伸条"；
     * 对照文件行/Settings 徽章补 scrollbar OFF + 清 SCROLLABLE */
    lv_obj_set_scrollbar_mode(ib, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ib, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ii = lv_label_create(ib);
    lv_label_set_text(ii, LV_SYMBOL_HOME);
    /* 2026-08-10 修复：原漏设字体走默认 16px → 86px 徽章里符号过小；
     * 补 FONT_ICON(36px) 与文件行图标/Settings 行图标同档（skill 六档字体） */
    lv_obj_set_style_text_font(ii, FONT_ICON, 0);
    lv_obj_set_style_text_color(ii, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_scrollbar_mode(ii, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ii, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(ii);
    lv_obj_t *il = lv_label_create(internal_btn);
    lv_label_set_text(il, "内部存储");
    lv_obj_set_style_text_font(il, FONT_BODY, 0);
    lv_obj_set_style_text_color(il, lv_color_hex(COL_TEXT), 0);

    /* TF卡快捷入口 */
    lv_obj_t *tf_btn = lv_btn_create(quick_cont);
    lv_obj_set_size(tf_btn, (sw - 2 * EDGE_PAD - CARD_GAP) / 2, DM(56));
    lv_obj_set_style_bg_color(tf_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(tf_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(tf_btn, RAD_CARD, 0);
    lv_obj_set_style_border_width(tf_btn, 1, 0);
    lv_obj_set_style_border_color(tf_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(tf_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(tf_btn, 6, 0);
    lv_obj_set_style_shadow_opa(tf_btn, LV_OPA_10, 0);
    lv_obj_add_event_cb(tf_btn, files_cb_tf_root, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(tf_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tf_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tf_btn, DM(12), 0);
    lv_obj_t *tb = lv_obj_create(tf_btn);
    lv_obj_set_size(tb, DM(36), DM(36));
    lv_obj_set_style_radius(tb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_20, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_CLICKABLE);
    /* 2026-08-10 修复（-26）：同内部存储，关滚动条防"上下拉伸条" */
    lv_obj_set_scrollbar_mode(tb, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ti = lv_label_create(tb);
    lv_label_set_text(ti, LV_SYMBOL_SD_CARD);
    /* 2026-08-10 修复：同内部存储，补 FONT_ICON(36px) */
    lv_obj_set_style_text_font(ti, FONT_ICON, 0);
    lv_obj_set_style_text_color(ti, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_scrollbar_mode(ti, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ti, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(ti);
    lv_obj_t *tl = lv_label_create(tf_btn);
    lv_label_set_text(tl, "TF卡");
    lv_obj_set_style_text_font(tl, FONT_BODY, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(COL_TEXT), 0);

    /* ── 面包屑路径栏（玻璃胶囊）── */
    files_breadcrumb = lv_obj_create(parent);
    lv_obj_set_size(files_breadcrumb, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(files_breadcrumb, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(files_breadcrumb, LV_OPA_70, 0);
    lv_obj_set_style_radius(files_breadcrumb, RAD_CARD, 0);
    lv_obj_set_style_border_width(files_breadcrumb, 1, 0);
    lv_obj_set_style_border_color(files_breadcrumb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(files_breadcrumb, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(files_breadcrumb, 4, 0);
    lv_obj_set_style_shadow_opa(files_breadcrumb, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(files_breadcrumb, DM(8), 0);
    lv_obj_set_flex_flow(files_breadcrumb, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(files_breadcrumb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(files_breadcrumb, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(files_breadcrumb, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 可滚动文件列表 ── */
    files_list_scrl = lv_obj_create(parent);
    lv_obj_set_size(files_list_scrl, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(files_list_scrl, 1);
    lv_obj_set_flex_flow(files_list_scrl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(files_list_scrl, LV_OPA_0, 0);
    lv_obj_set_style_border_width(files_list_scrl, 0, 0);
    lv_obj_set_style_pad_all(files_list_scrl, 0, 0);
    lv_obj_set_scrollbar_mode(files_list_scrl, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(files_list_scrl, LV_DIR_VER);

    /* 文件列表 = 整块玻璃分组卡（行 + 细分隔线，随卡圆角裁剪） */
    files_list_cont = lv_obj_create(files_list_scrl);
    lv_obj_set_size(files_list_cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(files_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(files_list_cont, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(files_list_cont, LV_OPA_70, 0);
    lv_obj_set_style_radius(files_list_cont, RAD_CARD, 0);
    lv_obj_set_style_clip_corner(files_list_cont, true, 0);
    lv_obj_set_style_shadow_width(files_list_cont, 6, 0);
    lv_obj_set_style_shadow_opa(files_list_cont, LV_OPA_10, 0);
    lv_obj_set_style_border_width(files_list_cont, 1, 0);
    lv_obj_set_style_border_color(files_list_cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(files_list_cont, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(files_list_cont, 0, 0);
    lv_obj_set_style_pad_row(files_list_cont, 4, 0);
    lv_obj_clear_flag(files_list_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* P145：列表区域空白长按 → 操作栏（行内长按由行回调处理，不冒泡） */
    lv_obj_add_event_cb(files_list_scrl, files_blank_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(files_list_cont, files_blank_long_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ── 初始化 ── */
    strcpy(files_cur_path, "/");
    files_scan_dir("/");
    files_refresh_list();
    files_update_breadcrumb();
}

static void files_go_up_cb(lv_event_t *e) { (void)e; files_go_up(); }

/* 快捷入口回调 */
static void files_cb_internal_root(lv_event_t *e) {
    (void)e; strcpy(files_cur_path, "/"); files_scan_dir("/"); files_refresh_list(); files_update_breadcrumb();
}
static void files_cb_tf_root(lv_event_t *e) {
    (void)e; strcpy(files_cur_path, "/sdcard"); files_scan_dir("/sdcard"); files_refresh_list(); files_update_breadcrumb();
}
