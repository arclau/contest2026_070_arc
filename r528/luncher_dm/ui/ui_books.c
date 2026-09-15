/*
 * ui_books.c — 书架 + 阅读器子页【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_books_subpage（改名
 * ui_books_create）+ book_* 全部（书架/阅读器/进度/护眼持久化 3 文件）。
 * 纯 UI+轻 IO，无驱动依赖。逻辑一字不改。
 *
 * 红线（勿碰）：
 *   - book_reader_overlay 挂在 subpage_overlay 上，close_subpage 经
 *     book_reader_close() 清理（对象+清理一起搬，禁止拆散）。
 *   - 持久化文件 /data/book_progress.conf /data/book_theme.conf 格式
 *     与既有数据兼容，不得改动。
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>     /* 2026-09-14：unlink 删书 */
#include <iconv.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"

/* 前向声明（ui_books_create 使用 book_card_click_cb；book_open_reader 声明
 * 在 deskmate_ui.h，deskmate_ui.c files_open_file 也引用） */
static void book_card_click_cb(lv_event_t *e);
static void book_card_longpress_cb(lv_event_t *e);   /* 2026-09-14：长按删书 */
static void book_del_show(int idx);                  /* 2026-09-14：删除确认弹窗 */
static void book_first_char(const char *s, char out[8]);   /* P0-1：UTF-8 首字符 */
static void book_toast_show(const char *msg);              /* P1-5：档位提示 toast */
static long book_view_top_byte(void);                      /* 连续滚动：视口顶部字节 */

/* ================================================================
 * BOOKS 子 APP（电子书阅读器）— books-reader-solution.md 重设计
 * 书架扫描 /mnt/sdcard/book（TF卡）+ /data/book（内置），流式分页阅读，
 * 避免大 txt 整本载入 LVGL 造成 OOM。
 * Apple 级高定 UI：毛玻璃卡片 + 柔和阴影 + 极简排版 + 微动画
 *
 * 常量 DM_MAX_BOOKS / DM_BOOK_DIR1/2/3 已上移到 deskmate_ui.h
 * （deskmate_ui.c 的 files_open_file 也引用，Phase 1 拆分后单一来源）。
 * ================================================================ */

/* 书架/阅读器共享状态（Phase 1 拆分：deskmate_ui.c files_open_file 引用，
 * deskmate_ui.h 有 extern 声明） */
char     dm_book_paths[DM_MAX_BOOKS][160];
char     dm_book_titles[DM_MAX_BOOKS][64];
uint32_t dm_book_count;

/* 阅读器状态（分块滑动窗口连续滚动，只保留有限块，见下方窗口引擎） */
static lv_obj_t *book_reader_overlay;   /* 全屏阅读器覆盖层 */
static lv_obj_t *book_reader_inner;     /* 正文内容容器（flex 列，挂分块 label） */
static lv_obj_t *book_reader_title_lbl; /* 顶部书名 */
static lv_obj_t *book_reader_prog;      /* 底部百分比 */
static lv_obj_t *book_reader_slider;    /* 底部进度条（可拖动跳转） */
static lv_obj_t *book_reader_bar;       /* P0-2：底部控件条（护眼配色随档位） */
static lv_obj_t *book_reader_back_btn;  /* P0-2：顶部返回按钮（护眼配色） */
static lv_obj_t *book_reader_eye_btn;   /* P0-2：顶部护眼按钮（护眼配色） */
static lv_obj_t *book_reader_scroll;    /* 正文滚动容器（视口） */
static lv_obj_t *book_toast;            /* P1-5：档位提示 toast */
static lv_timer_t *book_toast_timer;    /* toast 自动隐藏定时器 */
static int       book_cur_idx;          /* 当前阅读书籍索引 */
static long      book_offset;           /* 当前阅读字节（挂起/续读锚点） */
static long      book_file_size;        /* 文件总字节 */
static int       g_eye_care_mode;       /* 护眼模式（0=白/1=米/2=黑） */
static int       g_book_slider_guard;   /* 防 set_value 触发 VALUE_CHANGED 递归 */

/* P222：正文编码与数据源（GBK 书读 UTF-8 缓存；见 book_prepare_source） */
static int       book_enc;              /* 0=UTF-8 1=GBK 2=UTF-16LE 3=UTF-16BE */
static int       book_have_cache;       /* 1=book_read_path 指向 UTF-8 缓存 */
static char      book_read_path[248];   /* 阅读器实际读取路径 */

/* 2026-09-14 连续滚动窗口：分块（4KB/块）+ 上限回收，内存恒定。
 * 距底自动追加、距顶自动前补；删顶/前补都做像素补偿保持视口不动。 */
#define BOOK_CHUNK_BYTES 4096   /* 每块字节数（UTF-8 边界截断） */
#define BOOK_MAX_CHUNKS  40     /* 窗口最多块数（≈160KB 文本） */
#define BOOK_MIN_CHUNKS  8      /* 初始/续读最少加载块数（≈32KB，20+ 屏） */
#define BOOK_TRIM_CHUNKS 10     /* 满载回收：一次删顶部块数 */

typedef struct {
    lv_obj_t *lbl;              /* 该块正文 label（inner 子对象，flex 列） */
    long      start;            /* 该块起始字节（有效数据偏移） */
    long      end;              /* 该块结束字节 == 下一块起点 */
} book_chunk_t;

static book_chunk_t book_chunks[BOOK_MAX_CHUNKS];
static int  book_chunk_cnt;     /* 当前块数 */
static long book_first_byte;    /* 窗口首块起始字节 */
static int  book_all_loaded;    /* 已读到文件尾 */
static int  book_win_busy;      /* 防 SCROLL 回调重入 */

/* 2026-09-14 长按删书状态：content 容器（删后重建书架） + 确认弹窗 */
static lv_obj_t *books_content_parent = NULL;
static lv_obj_t *book_del_popup = NULL;
static int       book_del_idx = -1;
static int       book_longpress_suppress = 0;  /* 长按后吞掉本次 CLICKED */

#define BOOK_PROGRESS_FILE "/data/book_progress.conf"
#define BOOK_THEME_FILE    "/data/book_theme.conf"
#define BOOK_CACHE_DIR     "/data/bookcache"

/* 扫描 /data/book + /sdcard/book + /mnt/sdcard/book 下 .txt 文件
 * 返回数量（去重）。优先顺序：内置 > TF卡短路径 > TF卡标准路径 */
static uint32_t book_scan_directory(void)
{
    DIR *dir;
    struct dirent *ent;
    uint32_t count = 0;
    char seen[DM_MAX_BOOKS][64] = {0};

    dm_book_count = 0;

    /* 扫描三个目录，合并去重（按文件名） */
    const char *dirs[3] = {DM_BOOK_DIR1, DM_BOOK_DIR2, DM_BOOK_DIR3};
    for (int d = 0; d < 3; d++) {
        dir = opendir(dirs[d]);
        if (dir == NULL)
            continue;

        while ((ent = readdir(dir)) != NULL && count < DM_MAX_BOOKS) {
            const char *dot = strrchr(ent->d_name, '.');
            if (dot == NULL || dot == ent->d_name)
                continue;
            if (strcasecmp(dot, ".txt") != 0)
                continue;

            /* 去重：同名文件只保留第一个（优先顺序见上） */
            int dup = 0;
            for (int i = 0; i < count; i++) {
                if (strcmp(seen[i], ent->d_name) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                continue;

            strncpy(seen[count], ent->d_name, sizeof(seen[0]) - 1);
            seen[count][sizeof(seen[0]) - 1] = '\0';
            dm_utf8_trim(seen[count]);   /* 2026-09-14：截断勿劈开多字节尾 */
            snprintf(dm_book_paths[count], sizeof(dm_book_paths[count]),
                     "%s/%s", dirs[d], ent->d_name);
            snprintf(dm_book_titles[count], sizeof(dm_book_titles[count]),
                     "%.*s", (int)(dot - ent->d_name), ent->d_name);
            dm_utf8_trim(dm_book_titles[count]);   /* 2026-09-14：截断勿劈开多字节尾 */
            count++;
        }
        closedir(dir);
    }

    dm_book_count = count;
    return count;
}

/* 进度持久化：读取 */
static void book_progress_load(int idx)
{
    if (idx < 0 || idx >= (int)dm_book_count)
        return;

    FILE *f = fopen(BOOK_PROGRESS_FILE, "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char path[160];
        long offset;
        if (sscanf(line, "%159[^:]:%ld", path, &offset) == 2) {
            if (strcmp(path, dm_book_paths[idx]) == 0) {
                book_offset = offset;
                break;
            }
        }
    }
    fclose(f);
}

/* 进度持久化：保存 */
static void book_progress_save(int idx)
{
    if (idx < 0 || idx >= (int)dm_book_count)
        return;

    /* 读取现有内容，更新对应行 */
    char lines[DM_MAX_BOOKS][256];
    int line_count = 0;
    FILE *f = fopen(BOOK_PROGRESS_FILE, "r");
    if (f) {
        while (line_count < DM_MAX_BOOKS &&
               fgets(lines[line_count], sizeof(lines[0]), f))
            line_count++;
        fclose(f);
    }

    int found = 0;
    for (int i = 0; i < line_count; i++) {
        char path[160];
        if (sscanf(lines[i], "%159[^:]:", path) == 1 &&
            strcmp(path, dm_book_paths[idx]) == 0) {
            snprintf(lines[i], sizeof(lines[i]), "%s:%ld\n",
                     dm_book_paths[idx], book_offset);
            found = 1;
            break;
        }
    }
    if (!found && line_count < DM_MAX_BOOKS) {
        snprintf(lines[line_count], sizeof(lines[0]), "%s:%ld\n",
                 dm_book_paths[idx], book_offset);
        line_count++;
    }

    f = fopen(BOOK_PROGRESS_FILE, "w");
    if (f) {
        for (int i = 0; i < line_count; i++)
            fputs(lines[i], f);
        fclose(f);
    }
}

/* 护眼模式持久化：读取 */
static void book_theme_load(void)
{
    FILE *f = fopen(BOOK_THEME_FILE, "r");
    if (f) {
        fscanf(f, "%d", &g_eye_care_mode);
        if (g_eye_care_mode < 0 || g_eye_care_mode > 2)
            g_eye_care_mode = 0;
        fclose(f);
    } else {
        g_eye_care_mode = 0;
    }
}

/* 护眼模式持久化：保存 */
static void book_theme_save(void)
{
    FILE *f = fopen(BOOK_THEME_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", g_eye_care_mode);
        fclose(f);
    }
}

#define BOOK_BG_NORMAL   0xFFFFFF
#define BOOK_BG_EYE      0xF9F6F0
#define BOOK_BG_DARK     0x1C1C1E
#define BOOK_TX_NORMAL   COL_TEXT
#define BOOK_TX_EYE      0x4A3F35
#define BOOK_TX_DARK     0xE0E0E0

static void book_apply_eye_style(void)
{
    uint32_t bg, tx, bar_bg, btn_bg;
    switch (g_eye_care_mode) {
    case 1:  bg = BOOK_BG_EYE;  tx = BOOK_TX_EYE;  bar_bg = 0xF3EFE6; btn_bg = 0xF3EFE6; break;
    case 2:  bg = BOOK_BG_DARK; tx = BOOK_TX_DARK; bar_bg = 0x2C2C2E; btn_bg = 0x2C2C2E; break;
    default: bg = BOOK_BG_NORMAL; tx = BOOK_TX_NORMAL; bar_bg = 0xFFFFFF; btn_bg = COL_BG; break;
    }
    if (book_reader_overlay) lv_obj_set_style_bg_color(book_reader_overlay, lv_color_hex(bg), 0);
    /* 正文分块 label 不单独设色，统一继承 inner 容器文字色 */
    if (book_reader_inner) lv_obj_set_style_text_color(book_reader_inner, lv_color_hex(tx), 0);
    if (book_reader_title_lbl) lv_obj_set_style_text_color(book_reader_title_lbl, lv_color_hex(tx), 0);

    /* P0-2：底部控件条 + 顶部按钮 + 百分比 + slider 轨道全套随档位
     * （原实现只改背景/正文，黑档下 bar 白条 + 按钮浅色，视觉割裂） */
    if (book_reader_bar) {
        lv_obj_set_style_bg_color(book_reader_bar, lv_color_hex(bar_bg), 0);
        lv_obj_set_style_bg_opa(book_reader_bar,
            g_eye_care_mode == 2 ? LV_OPA_90 : LV_OPA_70, 0);
    }
    if (book_reader_back_btn) lv_obj_set_style_bg_color(book_reader_back_btn, lv_color_hex(btn_bg), 0);
    if (book_reader_eye_btn)  lv_obj_set_style_bg_color(book_reader_eye_btn,  lv_color_hex(btn_bg), 0);
    if (book_reader_prog) lv_obj_set_style_text_color(book_reader_prog, lv_color_hex(tx), 0);
    if (book_reader_slider) {
        /* 轨道：白档浅灰 / 黑档深灰；indicator 蓝 + knob 白保持高对比 */
        lv_obj_set_style_bg_color(book_reader_slider,
            lv_color_hex(g_eye_care_mode == 2 ? 0x3A3A3C : 0xE5E5EA), 0);
    }
}

static void book_eye_toggle_cb(lv_event_t *e)
{
    (void)e;
    g_eye_care_mode = (g_eye_care_mode + 1) % 3;
    book_apply_eye_style();
    book_theme_save();
    /* P1-5：toast 提示当前档位（原实现切换无任何反馈） */
    book_toast_show(g_eye_care_mode == 0 ? "护眼：白" :
                    g_eye_care_mode == 1 ? "护眼：米" : "护眼：黑");
}

/* P1-5：档位提示 toast（底部居中深色胶囊，2s 自动隐藏） */
static void book_toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (book_toast) {
        lv_obj_del(book_toast);
        book_toast = NULL;
    }
    book_toast_timer = NULL;
}

static void book_toast_show(const char *msg)
{
    lv_obj_t *p = book_reader_overlay ? book_reader_overlay : subpage_overlay;
    if (!p) return;
    if (book_toast_timer) { lv_timer_del(book_toast_timer); book_toast_timer = NULL; }
    if (book_toast) { lv_obj_del(book_toast); book_toast = NULL; }

    book_toast = lv_obj_create(p);
    lv_obj_set_size(book_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(book_toast, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(book_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(book_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(book_toast, 0, 0);
    lv_obj_set_style_pad_all(book_toast, DM(6), 0);
    lv_obj_set_style_pad_left(book_toast, DM(10), 0);
    lv_obj_set_style_pad_right(book_toast, DM(10), 0);
    lv_obj_clear_flag(book_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(book_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(book_toast, LV_ALIGN_BOTTOM_MID, 0, -DM(40));
    lv_obj_move_foreground(book_toast);

    lv_obj_t *l = lv_label_create(book_toast);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);

    book_toast_timer = lv_timer_create(book_toast_timer_cb, 2000, NULL);
    lv_timer_set_repeat_count(book_toast_timer, 1);
}

    /* 继续阅读卡：扫描 book_progress.conf 找第一本 offset>0 的书（书架横幅） */
static int book_last_reading_idx(void)
{
    FILE *f = fopen(BOOK_PROGRESS_FILE, "r");
    if (!f) return -1;

    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char path[160];
        long offset;
        if (sscanf(line, "%159[^:]:%ld", path, &offset) == 2 && offset > 0) {
            for (uint32_t i = 0; i < dm_book_count; i++) {
                if (strcmp(path, dm_book_paths[i]) == 0) {
                    found = (int)i;
                    break;
                }
            }
            if (found >= 0) break;
        }
    }
    fclose(f);
    return found;
}

/* P0-1：取书名首字符（完整 UTF-8 序列，中文 3 字节不乱码）。
 * 原实现 `char fc[2] = {s[0], '\0'}` 只取首字节，中文必显示乱码。 */
static void book_first_char(const char *s, char out[8])
{
    if (!s || !*s) { strcpy(out, "?"); return; }
    unsigned char c = (unsigned char)s[0];
    int n = 1;
    if ((c & 0xE0) == 0xC0 && s[1]) n = 2;                    /* 2 字节 */
    else if ((c & 0xF0) == 0xE0 && s[1] && s[2]) n = 3;       /* 3 字节（中文） */
    else if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) n = 4; /* 4 字节 */
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
}

/* ================================================================
 * P222 (2026-09-14)：外置 txt 编码识别 + UTF-8 转换
 *
 * 现象：点击 TF 卡中文小说，正文满屏「口口」。
 * 根因：Windows 记事本默认「ANSI」= GBK，字节序列不是合法 UTF-8；
 *       LVGL 按 UTF-8 解码逐字节失败 → 每个字节一个替换方框。
 *       书架/阅读器自身中文是编译期 UTF-8 字面量，显示正常，故与字体无关。
 * 方案：开书时嗅探 BOM / UTF-8 合法性：
 *       · UTF-8（含纯 ASCII）→ 原样分页读；
 *       · GBK / UTF-16LE / UTF-16BE → 用 NuttX libc iconv 整本转成
 *         /data/bookcache/ 下的 .u8 缓存，阅读器只读缓存（字节分页逻辑不变）。
 *       整本转缓存可避免 GBK 变长编码在字节分页边界被劈开导致整页错位。
 * 依赖：defconfig 打开 CONFIG_LIBC_LOCALE_CHINESE（给 iconv 挂 GBK 表）。
 * ================================================================ */

enum { BOOK_ENC_UTF8 = 0, BOOK_ENC_GBK, BOOK_ENC_UTF16LE, BOOK_ENC_UTF16BE };

/* UTF-8 合法性：首/尾不完整序列都视为合法（采样/分页边界可能劈开），
 * 中间出现非法序列即判为非 UTF-8（典型 GBK 中文）。 */
static int book_utf8_valid(const unsigned char *p, size_t n)
{
    size_t i = 0;
    /* 采样起点是任意字节偏移（中部采样 sz/2），可能落在多字节中间：
     * 跳过开头残缺的 continuation 字节，否则合法 UTF-8 会被误判 GBK。 */
    while (i < n && (p[i] & 0xC0) == 0x80) i++;
    while (i < n) {
        unsigned char c = p[i];
        int len;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return 0;
        if (i + (size_t)len > n) break;              /* 尾部不完整：忽略 */
        for (int k = 1; k < len; k++)
            if ((p[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)len;
    }
    return 1;
}

/* 头部嗅探编码：头 4KB + 中部 2KB 双采样（防英文前言/空行开头的 GBK 书
 * 头部全 ASCII 被误判 UTF-8）；NUL 字节占比>5% 判无 BOM UTF-16LE。 */
static void book_detect_encoding(int idx)
{
    static unsigned char head[4096];
    static unsigned char mid[2048];
    size_t n, mn, off = 0, i, zeros = 0;
    FILE *f;
    struct stat st;
    long sz = 0;

    book_enc = BOOK_ENC_UTF8;
    f = fopen(dm_book_paths[idx], "rb");
    if (f == NULL) return;
    n = fread(head, 1, sizeof(head), f);

    if (stat(dm_book_paths[idx], &st) == 0 && st.st_size > 0)
        sz = (long)st.st_size;
    /* 中部采样：文件 1/2 处读 2KB（英文前言书的中文正文在此现形） */
    mn = 0;
    if (sz > (long)(n + sizeof(mid))) {
        if (fseek(f, sz / 2, SEEK_SET) == 0)
            mn = fread(mid, 1, sizeof(mid), f);
    }
    fclose(f);

    if (n >= 2 && head[0] == 0xFF && head[1] == 0xFE) { book_enc = BOOK_ENC_UTF16LE; return; }
    if (n >= 2 && head[0] == 0xFE && head[1] == 0xFF) { book_enc = BOOK_ENC_UTF16BE; return; }
    if (n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) off = 3;
    /* 无 BOM UTF-16LE 启发：文本含大量 NUL（ASCII 偶字节 0x00），UTF-8/GBK 正常文本无 NUL */
    for (i = off; i < n; i++) if (head[i] == 0x00) zeros++;
    if (n > off && zeros > (n - off) / 20) { book_enc = BOOK_ENC_UTF16LE; return; }
    if (off < n && !book_utf8_valid(head + off, n - off)) { book_enc = BOOK_ENC_GBK; return; }
    if (mn > 0 && !book_utf8_valid(mid, mn)) book_enc = BOOK_ENC_GBK;
}

static const char *book_iconv_from(void)
{
    switch (book_enc) {
    case BOOK_ENC_GBK:     return "gbk";
    case BOOK_ENC_UTF16LE: return "utf16le";   /* NuttX find_charmap 不认连字符 */
    case BOOK_ENC_UTF16BE: return "utf16be";
    default:               return "utf8";
    }
}

/* 缓存路径：/data/bookcache/<basename>.<size>.u8（源文件大小变化即失效重建） */
static int book_cache_path(int idx, char *out, size_t n)
{
    const char *base = strrchr(dm_book_paths[idx], '/');
    struct stat st;
    long sz;

    base = base ? base + 1 : dm_book_paths[idx];
    if (stat(dm_book_paths[idx], &st) != 0) return -1;
    sz = (long)st.st_size;
    snprintf(out, n, BOOK_CACHE_DIR "/%s.%ld.u8", base, sz);
    return 0;
}

static void book_cache_ensure_dir(void)
{
    struct stat st;
    if (stat(BOOK_CACHE_DIR, &st) != 0)
        mkdir(BOOK_CACHE_DIR, 0777);
}

/* 进度徽章用「有效大小」：有缓存按缓存，否则原文件 */
static long book_effective_size(int idx)
{
    char cache[248];
    struct stat st;
    if (book_cache_path(idx, cache, sizeof(cache)) == 0 &&
        stat(cache, &st) == 0 && st.st_size > 0)
        return (long)st.st_size;
    if (stat(dm_book_paths[idx], &st) == 0)
        return (long)st.st_size;
    return 0;
}

/* 整本转 UTF-8 缓存；成功 0，失败 -1（失败不留残件） */
static int book_build_cache(const char *src, const char *dst)
{
    static char ibuf[2048];
    static char obuf[8192];
    size_t carry = 0;
    iconv_t cd;
    FILE *fi, *fo;
    int ok = 0;

    cd = iconv_open("utf8", book_iconv_from());
    if (cd == (iconv_t)-1) return -1;
    fi = fopen(src, "rb");
    if (fi == NULL) { iconv_close(cd); return -1; }
    fo = fopen(dst, "wb");
    if (fo == NULL) { fclose(fi); iconv_close(cd); return -1; }

    for (;;) {
        size_t rd = fread(ibuf + carry, 1, sizeof(ibuf) - carry, fi);
        size_t inlen = carry + rd;
        char *pin = ibuf, *pout = obuf;
        size_t inb = inlen, outb = sizeof(obuf);

        if (inlen == 0) { ok = 1; break; }

        for (;;) {
            size_t r = iconv(cd, &pin, &inb, &pout, &outb);
            if (r != (size_t)-1) break;
            if (errno == E2BIG) {
                if (pout > obuf &&
                    fwrite(obuf, 1, (size_t)(pout - obuf), fo) != (size_t)(pout - obuf))
                    goto out;
                pout = obuf; outb = sizeof(obuf);
                continue;
            }
            if (errno == EINVAL) break;              /* 尾部不完整：留下块续接 */
            if (errno == EILSEQ) {                   /* 非法字节：跳过并补 '?' */
                pin++; inb--;
                if (outb < 1) {
                    if (fwrite(obuf, 1, sizeof(obuf), fo) != sizeof(obuf)) goto out;
                    pout = obuf; outb = sizeof(obuf);
                }
                *pout++ = '?'; outb--;
                continue;
            }
            goto out;
        }

        if (pout > obuf &&
            fwrite(obuf, 1, (size_t)(pout - obuf), fo) != (size_t)(pout - obuf))
            goto out;

        carry = inb;
        if (carry) memmove(ibuf, pin, carry);
        if (rd == 0) { ok = 1; break; }              /* EOF：丢弃尾部残序列 */
    }

out:
    fclose(fi);
    fclose(fo);
    iconv_close(cd);
    if (!ok) { remove(dst); return -1; }
    return 0;
}

/* 缓存不可用时的逐页兜底解码（页首可能错位，尽力而为） */
static size_t book_decode_page(const char *in, size_t inlen, char *out, size_t outsz)
{
    iconv_t cd;
    char *pin, *pout;
    size_t inb, outb;

    if (outsz == 0) return 0;
    cd = iconv_open("utf8", book_iconv_from());
    if (cd == (iconv_t)-1) {
        size_t m = inlen < outsz - 1 ? inlen : outsz - 1;
        memcpy(out, in, m);
        out[m] = '\0';
        return m;
    }
    pin = (char *)in; inb = inlen;
    pout = out; outb = outsz - 1;
    for (;;) {
        size_t r = iconv(cd, &pin, &inb, &pout, &outb);
        if (r != (size_t)-1) break;
        if (errno != EILSEQ) break;                  /* E2BIG/EINVAL：停 */
        pin++; inb--;
        if (outb < 1) break;
        *pout++ = '?'; outb--;
    }
    *pout = '\0';
    iconv_close(cd);
    return (size_t)(pout - out);
}

/* 开书前解析数据源：嗅探编码 → 必要时建缓存 → 填 book_read_path / file_size */
static void book_cache_gc_keep(int idx, const char *keep);
static void book_prepare_source(int idx)
{
    struct stat st;
    long sz = 0;
    char cache[248];

    snprintf(book_read_path, sizeof(book_read_path), "%s", dm_book_paths[idx]);
    book_enc = BOOK_ENC_UTF8;
    book_have_cache = 0;
    if (stat(dm_book_paths[idx], &st) == 0 && st.st_size > 0)
        sz = (long)st.st_size;

    book_detect_encoding(idx);

    if (book_enc != BOOK_ENC_UTF8 && book_cache_path(idx, cache, sizeof(cache)) == 0) {
        struct stat cst;
        if (stat(cache, &cst) != 0 || cst.st_size <= 0) {
            book_cache_ensure_dir();
            if (book_build_cache(dm_book_paths[idx], cache) != 0)
                cache[0] = '\0';               /* 失败：退回逐页解码 */
        }
        if (cache[0] && stat(cache, &cst) == 0 && cst.st_size > 0) {
            snprintf(book_read_path, sizeof(book_read_path), "%s", cache);
            book_have_cache = 1;
            sz = (long)cst.st_size;
            book_cache_gc_keep(idx, cache);   /* 同书旧尺寸孤儿缓存清掉 */
        }
    }

    book_file_size = sz;
}

/* 某本书的阅读百分比（书架进度徽章用，0 表示未读） */
static int book_progress_pct(int idx)
{
    if (idx < 0 || idx >= (int)dm_book_count) return 0;

    long offset = 0;
    FILE *f = fopen(BOOK_PROGRESS_FILE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char path[160];
            long off;
            if (sscanf(line, "%159[^:]:%ld", path, &off) == 2 &&
                strcmp(path, dm_book_paths[idx]) == 0) {
                offset = off;
                break;
            }
        }
        fclose(f);
    }
    if (offset <= 0) return 0;

    long total = book_effective_size(idx);
    if (total <= 0) return 0;
    int pct = (int)(offset * 100 / total);
    return pct > 100 ? 100 : pct;
}

void ui_books_create(lv_obj_t *parent)
{
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());

    /* 2026-09-14：记录 content 容器，删书确认后用来重建书架 */
    books_content_parent = parent;

    /* Column layout */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, CARD_GAP, 0);

    subpage_big_title(parent, "书籍");

    /* 扫描目录 */
    book_scan_directory();

    /* 书封配色 */
    static const uint32_t book_colors[] = {
        0xD35400, 0x8E44AD, 0x2980B9, 0x16A085,
        0xC0392B, 0x2C3E50, 0x7D3C98, 0x148F77,
    };

    /* ── 继续阅读横幅（Android 阅读器惯例）：有进度时置顶，一键续读 ── */
    int resume_idx = book_last_reading_idx();
    if (resume_idx >= 0 && resume_idx < (int)dm_book_count) {
        lv_obj_t *resume = lv_obj_create(parent);
        lv_obj_set_size(resume, lv_pct(100), DM(64));
        lv_obj_set_style_bg_color(resume, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(resume, LV_OPA_90, 0);
        lv_obj_set_style_radius(resume, RAD_CARD, 0);
        lv_obj_set_style_shadow_width(resume, 12, 0);
        lv_obj_set_style_shadow_color(resume, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(resume, LV_OPA_20, 0);
        lv_obj_set_style_border_width(resume, 0, 0);
        lv_obj_set_style_pad_left(resume, DM(8), 0);
        lv_obj_set_style_pad_right(resume, DM(8), 0);
        lv_obj_set_flex_flow(resume, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(resume, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(resume, DM(10), 0);
        lv_obj_clear_flag(resume, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(resume, book_card_click_cb, LV_EVENT_CLICKED,
                            NULL);
        lv_obj_set_user_data(resume, (void *)(uintptr_t)resume_idx);

        /* 左侧小封面（渐变 + 首字母；2026-08-09 对齐新书封：彩色→浅灰白） */
        lv_obj_t *thumb = lv_obj_create(resume);
        lv_obj_set_size(thumb, DM(36), DM(48));
        lv_obj_set_style_radius(thumb, DM(4), 0);
        lv_obj_set_style_bg_color(thumb, lv_color_hex(book_colors[resume_idx % 8]), 0);
        lv_obj_set_style_bg_grad_color(thumb, lv_color_hex(0xF2F2F7), 0);
        lv_obj_set_style_bg_grad_dir(thumb, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(thumb, 0, 0);
        lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *tf = lv_label_create(thumb);
        char tfc[8];
        book_first_char(dm_book_titles[resume_idx], tfc);   /* P0-1：完整 UTF-8 首字符 */
        lv_label_set_text(tf, tfc);
        lv_obj_set_style_text_font(tf, FONT_TITLE, 0);
        lv_obj_set_style_text_color(tf, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(tf);

        /* 中间：继续阅读 + 书名 + 进度 */
        lv_obj_t *tcol = make_clean_cont(resume);
        lv_obj_set_size(tcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(tcol, DM(2), 0);
        lv_obj_set_flex_grow(tcol, 1);
        lv_obj_clear_flag(tcol, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *hint = lv_label_create(tcol);
        lv_label_set_text(hint, "继续阅读");
        lv_obj_set_style_text_font(hint, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_SEC), 0);

        lv_obj_t *rt = lv_label_create(tcol);
        lv_label_set_long_mode(rt, LV_LABEL_LONG_DOT);
        lv_obj_set_width(rt, sw - 2 * EDGE_PAD - DM(160));
        lv_label_set_text(rt, dm_book_titles[resume_idx]);
        lv_obj_set_style_text_font(rt, FONT_BODY, 0);
        lv_obj_set_style_text_color(rt, lv_color_hex(COL_TEXT), 0);

        /* 右侧继续阅读圆钮（复用音乐播放器按钮样式与点击回调；
         * P2-6 由 LV_SYMBOL_PLAY 改为 LV_SYMBOL_NEXT——阅读前进语义） */
        music_round_btn(resume, DM(40), LV_SYMBOL_NEXT, book_card_click_cb,
                        (void *)(uintptr_t)resume_idx);
    }

    /* ── Bookshelf grid ──
     * 6 列书封卡片，A4 比例（1:1.414），书封外观
     * 2026-08-09 改 4 列 → 6 列：原卡 396×560 过大，6 列 248×350 紧凑统一 */
    int cols = 6;
    lv_coord_t card_w = (sw - 2 * EDGE_PAD - (cols - 1) * CARD_GAP) / cols;
    lv_coord_t card_h = (lv_coord_t)(card_w * 1.414f);  /* A4 1:1.414 书封 */

    lv_obj_t *shelf_cont = make_clean_cont(parent);
    lv_obj_set_width(shelf_cont, lv_pct(100));
    lv_obj_set_height(shelf_cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(shelf_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(shelf_cont, CARD_GAP, 0);
    lv_obj_set_style_pad_row(shelf_cont, CARD_GAP, 0);
    lv_obj_set_flex_align(shelf_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* 书封配色（定义见函数开头，供继续阅读横幅与书封共用） */
    uint32_t n = dm_book_count > DM_MAX_BOOKS ? DM_MAX_BOOKS : dm_book_count;

    for (uint32_t i = 0; i < n; i++) {
        /* 书封卡片 — Liquid Glass 白玻璃卡 + 顶部柔和渐变书封区 + 深色书名
         * （2026-08-09 封面重设计：弃深色大色块/书脊，对齐全 UI 浅色玻璃语言） */
        lv_obj_t *bcard = lv_obj_create(shelf_cont);
        lv_obj_set_size(bcard, card_w, card_h);
        lv_obj_set_style_bg_color(bcard, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(bcard, LV_OPA_70, 0);          /* 白玻璃 */
        lv_obj_set_style_border_width(bcard, 1, 0);
        lv_obj_set_style_border_color(bcard, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(bcard, LV_OPA_50, 0);
        lv_obj_set_style_radius(bcard, RAD_CARD, 0);            /* 统一 40px 圆角 */
        lv_obj_set_style_shadow_width(bcard, 8, 0);
        lv_obj_set_style_shadow_color(bcard, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(bcard, LV_OPA_10, 0);
        lv_obj_set_style_clip_corner(bcard, true, 0);           /* 子元素随卡圆角裁剪 */
        lv_obj_set_style_pad_all(bcard, 0, 0);
        lv_obj_set_scrollbar_mode(bcard, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(bcard, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(bcard, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(bcard, LV_FLEX_FLOW_COLUMN);
        /* 按压反馈：轻微放大 + 阴影加深 */
        lv_obj_set_style_transform_width(bcard, 4, LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(bcard, 4, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(bcard, 20, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_opa(bcard, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_user_data(bcard, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(bcard, book_card_click_cb, LV_EVENT_CLICKED, NULL);
        /* 2026-09-14：长按 → 删除确认（内置示例书提示不可删） */
        lv_obj_add_event_cb(bcard, book_card_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

        /* 书封区（上部 62%）：彩色 → 浅灰白 柔和渐变 + 白色首字符 */
        lv_obj_t *cover = lv_obj_create(bcard);
        lv_obj_set_size(cover, lv_pct(100), lv_pct(62));
        lv_obj_set_style_bg_color(cover, lv_color_hex(book_colors[i % 8]), 0);
        lv_obj_set_style_bg_grad_color(cover, lv_color_hex(0xF2F2F7), 0);
        lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cover, 0, 0);
        lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cover, LV_OBJ_FLAG_CLICKABLE);

        /* 首字符（白色 FONT_TITLE，书封区中央） */
        lv_obj_t *mi = lv_label_create(cover);
        char fc[8];
        book_first_char(dm_book_titles[i], fc);             /* P0-1：完整 UTF-8 首字符 */
        lv_label_set_text(mi, fc);
        lv_obj_set_style_text_font(mi, FONT_TITLE, 0);
        lv_obj_set_style_text_color(mi, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_opa(mi, LV_OPA_90, 0);
        lv_obj_align(mi, LV_ALIGN_CENTER, 0, -DM(4));

        /* 信息区（下部 38%）：书名（深色）+ .txt 标签 */
        lv_obj_t *info = lv_obj_create(bcard);
        lv_obj_set_size(info, lv_pct(100), lv_pct(38));
        lv_obj_set_style_bg_opa(info, LV_OPA_0, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(info, DM(8), 0);
        lv_obj_set_style_pad_row(info, DM(4), 0);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

        /* 书名（深色，居中，超长省略） */
        lv_obj_t *bt = lv_label_create(info);
        lv_label_set_long_mode(bt, LV_LABEL_LONG_DOT);
        lv_obj_set_width(bt, card_w - DM(24));
        lv_label_set_text(bt, dm_book_titles[i]);
        lv_obj_set_style_text_font(bt, FONT_BODY, 0);
        lv_obj_set_style_text_color(bt, lv_color_hex(COL_TEXT), 0);

        lv_obj_t *tag = lv_label_create(info);
        lv_label_set_text(tag, ".txt");
        lv_obj_set_style_text_font(tag, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(COL_SEC), 0);

        /* 右下角进度徽章（读过才显示，Android 阅读器惯例） */
        int bpct = book_progress_pct((int)i);
        if (bpct > 0) {
            lv_obj_t *pbadge = lv_obj_create(bcard);
            lv_obj_set_size(pbadge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_align(pbadge, LV_ALIGN_BOTTOM_RIGHT, -DM(4), -DM(4));
            lv_obj_set_style_bg_color(pbadge, lv_color_hex(COL_BLUE), 0);
            lv_obj_set_style_bg_opa(pbadge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(pbadge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(pbadge, 0, 0);
            lv_obj_set_style_shadow_width(pbadge, 6, 0);
            lv_obj_set_style_shadow_color(pbadge, lv_color_hex(COL_BLUE), 0);
            lv_obj_set_style_shadow_opa(pbadge, LV_OPA_40, 0);
            lv_obj_set_style_pad_left(pbadge, DM(4), 0);
            lv_obj_set_style_pad_right(pbadge, DM(4), 0);
            lv_obj_set_style_pad_top(pbadge, DM(1), 0);
            lv_obj_set_style_pad_bottom(pbadge, DM(1), 0);
            lv_obj_clear_flag(pbadge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *pl = lv_label_create(pbadge);
            lv_label_set_text_fmt(pl, "%d%%", bpct);
            lv_obj_set_style_text_font(pl, FONT_CAPTION, 0);
            lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(pl);
        }
    }

    /* 书架为空 */
    if (dm_book_count == 0) {
        lv_obj_t *empty = lv_label_create(parent);
        lv_label_set_text(empty, "把 .txt 书放到 /data/book 或 /sdcard/book 后重新进入");
        lv_obj_set_style_text_font(empty, FONT_BODY, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_SEC), 0);
    }
}

/* ================================================================
 * BOOK READER — 全屏阅读器
 * ================================================================ */

/* 书架卡片点击 → 打开阅读器 */
static void book_card_click_cb(lv_event_t *e)
{
    /* 2026-09-14：长按释放后也会补发 CLICKED——吞掉，避免删书弹窗出现在
     * 刚长按的卡片上时又立刻进阅读器 */
    if (book_longpress_suppress) {
        book_longpress_suppress = 0;
        return;
    }
    lv_obj_t *card = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(card);
    if (idx >= 0 && idx < (int)dm_book_count) {
        book_open_reader(idx);
    }
}

/* ================================================================
 * 长按删书（2026-09-14）
 * 作用范围：TF 卡（/sdcard/book、/mnt/sdcard/book）与内置都弹窗；
 * 内置 /data/book 是固件示例书，删除按钮置灰提示"不可删除"。
 * ================================================================ */

/* 从进度文件清除指定书行 */
static void book_progress_remove(const char *path)
{
    char lines[DM_MAX_BOOKS][256];
    int n = 0;
    FILE *f = fopen(BOOK_PROGRESS_FILE, "r");
    if (f) {
        while (n < DM_MAX_BOOKS && fgets(lines[n], sizeof(lines[0]), f))
            n++;
        fclose(f);
    }
    f = fopen(BOOK_PROGRESS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) {
        char p[160];
        if (sscanf(lines[i], "%159[^:]:", p) == 1 && strcmp(p, path) == 0)
            continue;   /* 已被删的书：进度记录一并去掉 */
        fputs(lines[i], f);
    }
    fclose(f);
}

/* 删除对应 UTF-8 缓存（源文件还在时由 book_cache_path 推算路径）。
 * 源大小变化会导致旧缓存改名残留（<basename>.<oldsize>.u8），此处按 basename
 * 前缀全清，避免 /data/bookcache 越积越大。 */
static void book_cache_remove(int idx)
{
    const char *base = strrchr(dm_book_paths[idx], '/');
    DIR *d;
    base = base ? base + 1 : dm_book_paths[idx];
    d = opendir(BOOK_CACHE_DIR);
    if (d == NULL) {
        char cache[248];
        if (book_cache_path(idx, cache, sizeof(cache)) == 0)
            unlink(cache);
        return;
    }
    struct dirent *ent;
    size_t blen = strlen(base);
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, base, blen) != 0) continue;
        if (ent->d_name[blen] != '.') continue;
        char full[264];
        snprintf(full, sizeof(full), BOOK_CACHE_DIR "/%s", ent->d_name);
        unlink(full);
    }
    closedir(d);
}

/* 建缓存后 GC：同 basename、非本次 keep 的旧尺寸缓存全部清掉 */
static void book_cache_gc_keep(int idx, const char *keep)
{
    const char *base = strrchr(dm_book_paths[idx], '/');
    DIR *d;
    base = base ? base + 1 : dm_book_paths[idx];
    d = opendir(BOOK_CACHE_DIR);
    if (d == NULL) return;
    struct dirent *ent;
    size_t blen = strlen(base);
    while ((ent = readdir(d)) != NULL) {
        char full[264];
        if (strncmp(ent->d_name, base, blen) != 0) continue;
        if (ent->d_name[blen] != '.') continue;
        snprintf(full, sizeof(full), BOOK_CACHE_DIR "/%s", ent->d_name);
        if (keep && strcmp(full, keep) == 0) continue;
        unlink(full);
    }
    closedir(d);
}

static void book_del_hide(void)
{
    if (book_del_popup) {
        lv_obj_del(book_del_popup);
        book_del_popup = NULL;
    }
    book_del_idx = -1;
    /* 长按后若手指有位移，LVGL 不会补发 CLICKED（改发 SCROLL_THROW_BEGIN），
     * 吞标志就永远留在 1 → 下次正常点书被吃掉。弹窗收起即复位，闭合窗口。 */
    book_longpress_suppress = 0;
}

/* close_subpage 清理入口（删书遮罩挂根屏，不随子页 overlay 删除） */
void book_del_popup_close(void)
{
    book_del_hide();
}

static void book_del_confirm_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);   /* 阻断向 scrim 冒泡：防确认后又触发取消/二次 hide */
    reset_idle_timer();
    int idx = book_del_idx;
    if (idx < 0 || idx >= (int)dm_book_count) {
        book_del_hide();
        return;
    }

    /* 顺序：先清进度+缓存，再删源文件（book_cache_path 依赖文件存在） */
    book_progress_remove(dm_book_paths[idx]);
    book_cache_remove(idx);
    if (unlink(dm_book_paths[idx]) != 0) {
        LV_LOG_USER("[books] delete %s fail errno=%d", dm_book_paths[idx], errno);
    }
    book_del_hide();

    /* 重新扫描 + 重建书架（删掉的是 content 的子对象，弹窗在 scr 上不受影响） */
    book_scan_directory();
    if (books_content_parent) {
        lv_obj_clean(books_content_parent);
        ui_books_create(books_content_parent);
    }
}

static void book_del_cancel_cb(lv_event_t *e)
{
    /* scrim 点空白取消：子对象冒泡上来的 CLICKED 一律忽略（目标≠本层），
     * 防点"删除/取消"按钮时冒泡到 scrim 又触发一次取消（hide 后 UAF）。 */
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    lv_event_stop_bubbling(e);
    reset_idle_timer();
    book_del_hide();
}

/* 弹窗按钮统一走 dm_popup_btn（deskmate_ui.c，2026-09-14 Liquid Glass 胶囊） */

static void book_del_show(int idx)
{
    if (book_del_popup) {
        lv_obj_del(book_del_popup);
        book_del_popup = NULL;
    }
    if (idx < 0 || idx >= (int)dm_book_count) return;

    book_del_idx = idx;
    int builtin = (strncmp(dm_book_paths[idx], DM_BOOK_DIR1,
                           strlen(DM_BOOK_DIR1)) == 0);

    lv_obj_t *scr = lv_scr_act();
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

    /* 半透明全屏遮罩：点空白取消；挡住底下书架点击 */
    lv_obj_t *scrim = lv_obj_create(scr);
    lv_obj_set_size(scrim, sw, sh);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_move_foreground(scrim);
    lv_obj_add_event_cb(scrim, book_del_cancel_cb, LV_EVENT_CLICKED, NULL);
    book_del_popup = scrim;

    /* 居中玻璃卡片（2026-09-14 统一 Liquid Glass 弹窗语言） */
    lv_obj_t *card = dm_popup_card(scrim, DM(560));

    /* 图标块：可删=红色垃圾桶 / 内置=蓝色 OK */
    dm_popup_icon(card, builtin ? LV_SYMBOL_OK : LV_SYMBOL_TRASH,
                  builtin ? COL_BLUE : COL_RED);

    /* 书名 */
    lv_obj_t *tt = dm_popup_title(card, "");
    lv_label_set_text_fmt(tt, "《%s》", dm_book_titles[idx]);

    /* 提示 */
    dm_popup_text(card, builtin ? "这是内置示例书，不可删除"
                                : "删除后无法恢复，确定要删吗？");

    /* 按钮行 */
    lv_obj_t *row = dm_popup_btns(card);
    if (builtin) {
        dm_popup_btn(row, "知道了", 0, book_del_cancel_cb);
    } else {
        dm_popup_btn(row, "取消", 1, book_del_cancel_cb);
        dm_popup_btn(row, "删除", 2, book_del_confirm_cb);
    }
    /* P232：自适应补高（健康/暗光弹窗同病根），撑足后重新居中 */
    dm_popup_fit(card, row);
    lv_obj_center(card);
}

/* 书架卡片长按 → 删除确认 */
static void book_card_longpress_cb(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(card);
    if (idx < 0 || idx >= (int)dm_book_count) return;
    book_longpress_suppress = 1;   /* 吞掉长按释放后的 CLICKED，防误开阅读器 */
    book_del_show(idx);
}

/* 阅读器关闭（close_subpage 清理入口，deskmate_ui.h 声明） */
void book_reader_close(void)
{
    if (book_reader_overlay) {
        /* 先取当前视口顶部字节作为续读锚点（要趁对象还在） */
        book_offset = book_view_top_byte();
        book_progress_save(book_cur_idx);
        /* P1-5：阅读器销毁前先清 toast 定时器/对象，防野指针 */
        if (book_toast_timer) { lv_timer_del(book_toast_timer); book_toast_timer = NULL; }
        book_toast = NULL;
        lv_obj_del(book_reader_overlay);
        book_reader_overlay = NULL;
        book_reader_inner = NULL;
        book_reader_title_lbl = NULL;
        book_reader_prog = NULL;
        book_reader_slider = NULL;
        book_reader_bar = NULL;        /* P0-2 */
        book_reader_back_btn = NULL;   /* P0-2 */
        book_reader_eye_btn = NULL;    /* P0-2 */
        book_reader_scroll = NULL;     /* P1-3 */
        book_chunk_cnt = 0;
        book_all_loaded = 0;
        book_win_busy = 0;
    }
    /* 回到书架：恢复全局返回键（阅读器自带的已随 overlay 销毁） */
    dm_subpage_back_btn_set_hidden(false);
}

static void book_reader_back_cb(lv_event_t *e)
{
    (void)e;
    book_reader_close();
}

/* UTF-8 边界安全截断：NUL 终止后委托 dm_utf8_trim 回退尾部不完整序列
 * （原实现遇多字节头直接 break，会把残缺头字节留在末尾 → �）。 */
static int book_utf8_trim(char *buf, int len)
{
    if (len <= 0) { if (buf) buf[0] = '\0'; return 0; }
    buf[len] = '\0';
    dm_utf8_trim(buf);
    return (int)strlen(buf);
}

/* ================================================================
 * 连续滚动窗口引擎（2026-09-14 P228）
 * 整本按固定大小分块（每块一个 label）；窗口内只保留有限块，滑到底自动
 * 追加、滑到顶自动前补；超出上限从顶部回收并做像素补偿——视口内容不动。
 * 无页码/翻页键，仅底部进度条（可拖动跳转）。
 * ================================================================ */

/* 读一块并生成 label，追加到 inner / book_chunks 末尾。
 * maxlen 限制原始读取字节（前补时不许越过已有首块）；返回推进的源字节数。 */
static long book_load_chunk(long start, long maxlen, lv_obj_t *inner)
{
    static char raw[BOOK_CHUNK_BYTES + 8];
    static char dec[BOOK_CHUNK_BYTES * 4 + 8];
    FILE *f;
    size_t rd, skip = 0;
    long consumed;
    char *txt;
    lv_obj_t *lbl;

    if (inner == NULL) return 0;
    if (maxlen <= 0 || maxlen > BOOK_CHUNK_BYTES + 4) maxlen = BOOK_CHUNK_BYTES;
    if (start < 0 || start >= book_file_size) return 0;

    f = fopen(book_read_path[0] ? book_read_path : dm_book_paths[book_cur_idx], "rb");
    if (f == NULL) return 0;
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return 0; }
    rd = fread(raw, 1, (size_t)maxlen, f);
    fclose(f);
    if (rd == 0) return 0;

    if (book_enc != BOOK_ENC_UTF8 && !book_have_cache) {
        /* 缓存构建失败兜底：逐块解码（块首可能错位，尽力而为） */
        book_decode_page(raw, rd, dec, sizeof(dec));
        txt = dec;
        consumed = (long)rd;
    } else {
        book_utf8_trim(raw, (int)rd);
        while (raw[skip] != '\0' && ((unsigned char)raw[skip] & 0xC0) == 0x80)
            skip++;                 /* 起点可能落在多字节中间：跳过残缺头 */
        txt = raw + skip;
        consumed = (long)skip + (long)strlen(txt);
        if (consumed <= 0) { txt = raw; consumed = (long)rd; }
    }

    /* 优先在换行处断块：避免一句话被切成两块（下一块行首突兀） */
    if (consumed > BOOK_CHUNK_BYTES / 2) {
        char *nl = strrchr(txt, '\n');
        if (nl && (nl - txt) > consumed / 2) {
            long cut = (long)(nl - txt) + 1;   /* txt 内截断长度 */
            txt[cut] = '\0';
            consumed = (long)skip + cut;       /* 加上被跳过的残缺头字节 */
        }
    }

    lbl = lv_label_create(inner);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, txt);

    book_chunks[book_chunk_cnt].lbl   = lbl;
    book_chunks[book_chunk_cnt].start = start + (long)skip;   /* 记录真实字符边界 */
    book_chunks[book_chunk_cnt].end   = start + consumed;
    book_chunk_cnt++;
    return consumed;
}

/* 追加下一块；返回 1=已追加，0=无更多/无空间 */
static int book_append_chunk(void)
{
    long start, consumed;

    if (book_all_loaded || book_chunk_cnt >= BOOK_MAX_CHUNKS) return 0;
    start = book_chunk_cnt ? book_chunks[book_chunk_cnt - 1].end : book_first_byte;
    if (start >= book_file_size) { book_all_loaded = 1; return 0; }
    consumed = book_load_chunk(start, 0, book_reader_inner);
    if (consumed <= 0) { book_all_loaded = 1; return 0; }
    if (book_chunks[book_chunk_cnt - 1].end >= book_file_size) book_all_loaded = 1;
    return 1;
}

/* 删除窗口首块（像素补偿：删后 scroll_y 减该块高，视口内容不动） */
static void book_remove_top(void)
{
    int32_t old_sy, h, desired;
    int i;

    if (book_chunk_cnt <= 0 || book_reader_scroll == NULL) return;
    old_sy = lv_obj_get_scroll_y(book_reader_scroll);
    h = lv_obj_get_height(book_chunks[0].lbl);
    book_first_byte = book_chunks[0].end;
    lv_obj_del(book_chunks[0].lbl);
    for (i = 1; i < book_chunk_cnt; i++)
        book_chunks[i - 1] = book_chunks[i];
    book_chunk_cnt--;
    lv_obj_update_layout(book_reader_scroll);
    desired = old_sy - h;
    if (desired < 0) desired = 0;
    lv_obj_scroll_to_y(book_reader_scroll, desired, LV_ANIM_OFF);
}

/* 满载时腾空间：从顶部一次删 BOOK_TRIM_CHUNKS 块 */
static int book_make_room(void)
{
    int n = BOOK_TRIM_CHUNKS;
    if (book_chunk_cnt < BOOK_MAX_CHUNKS) return 1;
    while (n-- > 0 && book_chunk_cnt > 1)
        book_remove_top();
    return book_chunk_cnt < BOOK_MAX_CHUNKS;
}

/* 前补一块（像素补偿：补后 scroll_y 加新块高，视口内容不动） */
static int book_prepend_chunk(void)
{
    long first, start, consumed;
    int idx, i;
    int32_t old_sy, h;
    book_chunk_t nc;

    if (book_chunk_cnt == 0 || book_chunk_cnt >= BOOK_MAX_CHUNKS) return 0;
    first = book_chunks[0].start;
    if (first <= 0) return 0;
    start = first - BOOK_CHUNK_BYTES;
    if (start < 0) start = 0;
    else if (start > 4) start -= 4;   /* 多回退 4B，保证跨界的汉字头也在块内 */

    old_sy = lv_obj_get_scroll_y(book_reader_scroll);
    idx = book_chunk_cnt;
    consumed = book_load_chunk(start, first - start, book_reader_inner);
    if (consumed <= 0) return 0;

    nc = book_chunks[idx];
    for (i = idx; i > 0; i--)
        book_chunks[i] = book_chunks[i - 1];
    book_chunks[0] = nc;
    lv_obj_move_to_index(nc.lbl, 0);
    book_first_byte = nc.start;

    lv_obj_update_layout(book_reader_scroll);
    h = lv_obj_get_height(nc.lbl);
    lv_obj_scroll_to_y(book_reader_scroll, old_sy + h, LV_ANIM_OFF);
    return 1;
}

/* 视口顶部对应字节（进度/续读锚点）：按所在块的像素比例估算 */
static long book_view_top_byte(void)
{
    int32_t sy, base;
    int i;

    if (book_chunk_cnt <= 0 || book_reader_scroll == NULL) return book_first_byte;
    sy = lv_obj_get_scroll_y(book_reader_scroll);
    base = lv_obj_get_y(book_reader_inner);
    for (i = 0; i < book_chunk_cnt; i++) {
        int32_t cy = lv_obj_get_y(book_chunks[i].lbl) - base;
        int32_t h  = lv_obj_get_height(book_chunks[i].lbl);
        if (h <= 0) continue;
        if (sy < cy + h) {
            long span = book_chunks[i].end - book_chunks[i].start;
            int32_t off = sy - cy;
            if (off < 0) off = 0;
            if (off > h) off = h;
            return book_chunks[i].start + span * off / h;
        }
    }
    return book_chunks[book_chunk_cnt - 1].end;
}

/* 刷新底部进度条 + 百分比（guard 防 set_value 触发 VALUE_CHANGED 递归） */
static void book_update_prog(void)
{
    int pct;
    if (book_file_size <= 0) return;
    book_offset = book_view_top_byte();
    pct = (int)(book_offset * 100 / book_file_size);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_book_slider_guard = 1;
    if (book_reader_slider) lv_slider_set_value(book_reader_slider, pct, LV_ANIM_OFF);
    g_book_slider_guard = 0;
    if (book_reader_prog) lv_label_set_text_fmt(book_reader_prog, "%d%%", pct);
}

/* 初始化窗口：从 start 字节起加载到填满，start 置于视口顶部（续读锚点） */
static void book_window_init(long start)
{
    if (book_reader_inner == NULL) return;
    lv_obj_clean(book_reader_inner);
    book_chunk_cnt = 0;
    book_all_loaded = 0;
    if (start < 0) start = 0;
    if (start > book_file_size) start = book_file_size;
    book_first_byte = start;

    while (!book_all_loaded && book_chunk_cnt < BOOK_MIN_CHUNKS)
        if (!book_append_chunk()) break;
    if (book_chunk_cnt > 0)
        book_first_byte = book_chunks[0].start;   /* 对齐到真实字符边界 */

    if (book_reader_scroll) {
        lv_obj_update_layout(book_reader_scroll);
        lv_obj_scroll_to_y(book_reader_scroll, 0, LV_ANIM_OFF);
    }
    book_update_prog();
}

/* 滚动回调：近底追加、近顶前补（busy 防 scroll_to_y 重入） */
static void book_scroll_cb(lv_event_t *e)
{
    int guard;
    (void)e;
    if (book_win_busy || book_reader_scroll == NULL) return;
    book_win_busy = 1;

    guard = 0;
    while (lv_obj_get_scroll_bottom(book_reader_scroll) < DM(800) && guard++ < 6) {
        if (!book_make_room()) break;
        if (!book_append_chunk()) break;
        lv_obj_update_layout(book_reader_scroll);
    }

    guard = 0;
    while (lv_obj_get_scroll_top(book_reader_scroll) < DM(400) && guard++ < 2) {
        if (!book_prepend_chunk()) break;
    }

    book_update_prog();
    book_win_busy = 0;
}

/* 进度条拖动：实时只刷百分比；松手才跳转重建窗口 */
static void book_slider_cb(lv_event_t *e)
{
    (void)e;
    if (g_book_slider_guard || book_reader_slider == NULL) return;
    if (book_reader_prog)
        lv_label_set_text_fmt(book_reader_prog, "%d%%",
                              (int)lv_slider_get_value(book_reader_slider));
}

static void book_slider_release_cb(lv_event_t *e)
{
    long target;
    (void)e;
    if (g_book_slider_guard) return;
    if (book_file_size <= 0 || book_reader_slider == NULL) return;
    target = (long)lv_slider_get_value(book_reader_slider) * book_file_size / 100;
    if (target < 0) target = 0;
    if (target > book_file_size) target = book_file_size;
    book_window_init(target);
}

/* 打开阅读器（Phase 1 拆分：deskmate_ui.c files_open_file 调用，非 static） */
void book_open_reader(int idx)
{
    lv_coord_t sw, sh;

    if (idx < 0 || idx >= (int)dm_book_count) return;
    if (book_reader_overlay) book_reader_close();

    book_theme_load();
    book_progress_load(idx);
    /* GBK→UTF-8 缓存后字节总数变化：老进度是源文件字节 offset，直接复用会跳页
     * 错位。先记源大小，转缓存后按比例折算（替代原来 offset>size 直接归零丢位置）。
     * 注意：仅在本 open「首次」建缓存时折算——缓存已存在说明进度早已按缓存字节
     * 存盘，再折算会每次打开按 源/缓存 比例反复放大（越开越靠后）。 */
    long old_src_sz = 0;
    long old_off = book_offset;
    {
        struct stat ost;
        if (stat(dm_book_paths[idx], &ost) == 0 && ost.st_size > 0)
            old_src_sz = (long)ost.st_size;
    }
    int had_cache = 0;
    {
        char cpath[248];
        struct stat cst;
        if (book_cache_path(idx, cpath, sizeof(cpath)) == 0 &&
            stat(cpath, &cst) == 0 && cst.st_size > 0)
            had_cache = 1;
    }

    book_cur_idx = idx;
    book_prepare_source(idx);   /* P222：嗅探编码 + 需要时建 UTF-8 缓存 */

    if (!had_cache && book_have_cache && old_src_sz > 0 && old_off > 0 &&
        old_off <= old_src_sz)
        book_offset = old_off * book_file_size / old_src_sz;
    if (book_offset < 0) book_offset = 0;
    if (book_offset > book_file_size) book_offset = 0;

    sw = lv_disp_get_hor_res(lv_disp_get_default());
    sh = lv_disp_get_ver_res(lv_disp_get_default());

    uint32_t init_bg = (g_eye_care_mode == 1) ? BOOK_BG_EYE :
                       (g_eye_care_mode == 2) ? BOOK_BG_DARK : BOOK_BG_NORMAL;
    lv_obj_t *reader_parent = subpage_overlay ? subpage_overlay : lv_scr_act();
    book_reader_overlay = lv_obj_create(reader_parent);
    lv_obj_set_size(book_reader_overlay, sw, sh);
    lv_obj_set_style_bg_color(book_reader_overlay, lv_color_hex(init_bg), 0);
    lv_obj_set_style_bg_opa(book_reader_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(book_reader_overlay, 0, 0);
    lv_obj_set_style_pad_all(book_reader_overlay, 0, 0);
    lv_obj_clear_flag(book_reader_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(book_reader_overlay);
    /* 阅读器自带返回键：藏起 layer_top 上的全局返回键，避免双返回键 */
    dm_subpage_back_btn_set_hidden(true);

    /* 顶部：返回按钮（玻璃圆形）*/
    lv_obj_t *back_btn = lv_btn_create(book_reader_overlay);
    lv_obj_set_size(back_btn, DM(40), DM(40));
    lv_obj_set_pos(back_btn, DM(16), DM(8));
    assert(DM(16) + DM(40) <= sw && DM(8) + DM(40) <= sh);   /* 铁律2：边界断言 */
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_70, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, book_reader_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_ICON, 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(back_icon);
    book_reader_back_btn = back_btn;   /* P0-2：护眼配色随档位 */

    /* 顶部：书名（居中）*/
    book_reader_title_lbl = lv_label_create(book_reader_overlay);
    lv_label_set_long_mode(book_reader_title_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(book_reader_title_lbl, dm_book_titles[idx]);
    lv_obj_set_width(book_reader_title_lbl, sw - DM(140));
    lv_obj_align(book_reader_title_lbl, LV_ALIGN_TOP_MID, 0, DM(16));
    lv_obj_set_style_text_font(book_reader_title_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(book_reader_title_lbl, lv_color_hex(COL_TEXT), 0);

    /* 顶部：护眼模式切换按钮 */
    lv_obj_t *eye_btn = lv_btn_create(book_reader_overlay);
    lv_obj_set_size(eye_btn, DM(40), DM(40));
    lv_obj_set_pos(eye_btn, sw - DM(56), DM(8));
    assert(sw - DM(56) >= 0 && sw - DM(56) + DM(40) <= sw && DM(8) + DM(40) <= sh);   /* 铁律2 */
    lv_obj_set_style_radius(eye_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_70, 0);
    lv_obj_set_style_shadow_width(eye_btn, 0, 0);
    lv_obj_set_style_border_width(eye_btn, 0, 0);
    lv_obj_add_event_cb(eye_btn, book_eye_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eye_icon = lv_label_create(eye_btn);
    lv_label_set_text(eye_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(eye_icon, FONT_ICON, 0);
    lv_obj_set_style_text_color(eye_icon, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(eye_icon);
    book_reader_eye_btn = eye_btn;     /* P0-2：护眼配色随档位 */

    /* ── 底部：进度条（连续滚动，无页码/翻页键；可拖动跳转）──
     * 2026-09-14 P228：先建 bar 再量高度，正文视口随后铺满剩余空间。
     * （旧写死 sh-DM(190) 在 bar 顶留下 ~170px 死区 = 进度条上方留白） */
    lv_obj_t *bar = lv_obj_create(book_reader_overlay);
    lv_obj_set_size(bar, sw - 2 * DM(32), LV_SIZE_CONTENT);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -DM(20));
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 8, 0);
    lv_obj_set_style_shadow_opa(bar, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(bar, DM(6), 0);
    lv_obj_set_style_pad_column(bar, DM(10), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    book_reader_bar = bar;             /* P0-2：护眼配色随档位 */

    book_reader_slider = lv_slider_create(bar);
    lv_obj_set_flex_grow(book_reader_slider, 1);
    lv_obj_set_height(book_reader_slider, DM(6));
    lv_obj_set_style_bg_color(book_reader_slider, lv_color_hex(COL_BLUE),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(book_reader_slider, DM(3), LV_PART_INDICATOR);
    lv_obj_set_style_radius(book_reader_slider, DM(3), LV_PART_KNOB);
    lv_obj_set_style_bg_color(book_reader_slider, lv_color_hex(0xFFFFFF),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(book_reader_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(book_reader_slider, DM(3), LV_PART_KNOB);
    lv_slider_set_range(book_reader_slider, 0, 100);
    lv_obj_add_event_cb(book_reader_slider, book_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(book_reader_slider, book_slider_release_cb,
                        LV_EVENT_RELEASED, NULL);   /* 松手才跳转 */

    book_reader_prog = lv_label_create(bar);
    lv_label_set_text(book_reader_prog, "0%");
    lv_obj_set_style_text_font(book_reader_prog, FONT_BODY, 0);
    lv_obj_set_style_text_color(book_reader_prog, lv_color_hex(COL_SEC), 0);

    /* 实测 bar 顶部，正文视口从 DM(56) 铺到 bar 顶 - DM(12) */
    lv_obj_update_layout(book_reader_overlay);
    lv_coord_t bar_top = lv_obj_get_y(bar);
    lv_coord_t view_h = bar_top - DM(12) - DM(56);
    if (view_h < DM(200) || view_h > sh)
        view_h = sh - DM(190);   /* 异常兜底：回旧值，保证可用 */
    assert(DM(32) + (sw - 2 * DM(32)) <= sw && DM(56) + view_h <= sh);   /* 铁律2 */

    /* 内容区：固定视口 + flex 列内容容器（分块 label）——连续滚动 */
    lv_obj_t *scroll_cont = lv_obj_create(book_reader_overlay);
    lv_obj_set_size(scroll_cont, sw - 2 * DM(32), view_h);
    lv_obj_set_pos(scroll_cont, DM(32), DM(56));
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_radius(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 0, 0);
    lv_obj_set_scrollbar_mode(scroll_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    /* 2026-09-14 P228：连续滚动——滚动事件触发窗口追加/前补 */
    lv_obj_add_event_cb(scroll_cont, book_scroll_cb, LV_EVENT_SCROLL, NULL);
    book_reader_scroll = scroll_cont;

    /* 内容容器：flex 列、高度按内容撑开，分块正文 label 挂这里 */
    book_reader_inner = lv_obj_create(scroll_cont);
    lv_obj_set_width(book_reader_inner, lv_pct(100));
    lv_obj_set_height(book_reader_inner, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(book_reader_inner, LV_OPA_0, 0);
    lv_obj_set_style_border_width(book_reader_inner, 0, 0);
    lv_obj_set_style_pad_all(book_reader_inner, 0, 0);
    lv_obj_set_style_pad_row(book_reader_inner, 0, 0);
    lv_obj_set_style_text_font(book_reader_inner, FONT_BODY, 0);   /* label 继承 */
    lv_obj_set_flex_flow(book_reader_inner, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(book_reader_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(book_reader_inner, LV_OBJ_FLAG_CLICKABLE);

    book_apply_eye_style();
    book_window_init(book_offset);   /* 续读锚点：加载窗口并定位到该字节 */
}
