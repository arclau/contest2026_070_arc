/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_screenshot.c
 * 截图模块——LRADC 物理按键触发全屏截图，存 24bit BMP 到 TF 卡
 *
 * 2026-09-11 新增（本地固件，不随作品提交）：
 *  - 触发：/dev/input/event1（drv_lradc 注册的 5 个 LRADC 按键，bit0~4）。
 *    用 poll() 等按键驱动通知（避免每帧 read 触发驱动内 syslog 刷屏），
 *    收到事件后读一次当前键值，任一键按下沿 → 截屏（400ms 消抖）。
 *  - 输出：/sdcard/screenshots/shot_YYYYMMDD-HHMMSS-N.png
 *    满分辨率 1920x1200，XRGB8888 快照经 libpng 编码为 PNG（体积远小于 BMP）。
 *  - 线程：全程在 LVGL 线程的 lv_timer 内（lvgl 非线程安全），无锁。
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <png.h>
#include <nuttx/input/buttons.h>

#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_screenshot.h"

#define DM_SHOT_DEV      "/dev/input/event1"
#define DM_SHOT_DIR      "/sdcard/screenshots"
#define DM_SHOT_PERIOD   100        /* 按键轮询周期 ms */
#define DM_SHOT_DEBOUNCE 400        /* 连续截屏最小间隔 ms */

static int             s_btn_fd = -1;
static btn_buttonset_t s_prev;
static uint32_t        s_last_ms;
static uint32_t        s_seq;
static lv_timer_t     *s_timer;
static char            s_last_path[160];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 把 XRGB8888 快照经 libpng 编码成 PNG（逐行转换，避免整帧 RGB 缓冲） */
static int png_write24(const char *path, const lv_draw_buf_t *db)
{
    uint32_t w = db->header.w;
    uint32_t h = db->header.h;
    uint32_t stride = db->header.stride ? db->header.stride : w * 4u;
    const uint8_t *src = db->data;
    uint8_t *row;
    FILE *f;
    png_structp png = NULL;
    png_infop info = NULL;
    uint32_t y;

    if (w == 0 || h == 0 || src == NULL)
        return -1;

    row = malloc(w * 3u);
    if (row == NULL)
        return -1;

    f = fopen(path, "wb");
    if (f == NULL) {
        free(row);
        return -1;
    }

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        fclose(f);
        free(row);
        return -1;
    }
    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_write_struct(&png, NULL);
        fclose(f);
        free(row);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        free(row);
        return -1;
    }

    png_init_io(png, f);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (y = 0; y < h; y++) {
        const uint8_t *s = src + y * stride;
        uint8_t *d = row;
        uint32_t x;

        for (x = 0; x < w; x++) {
            /* XRGB8888 小端内存序：B,G,R,X → PNG 需要 R,G,B */
            *d++ = s[2];
            *d++ = s[1];
            *d++ = s[0];
            s += 4;
        }
        png_write_row(png, row);
    }

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    free(row);
    return 0;
}

static lv_obj_t   *s_toast;
static lv_obj_t   *s_toast_lbl;
static lv_timer_t *s_toast_timer;

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
        s_toast_lbl = NULL;
    }
    s_toast_timer = NULL;   /* 一次性 timer 由 LVGL 自动回收 */
}

/* 复用单个底部胶囊 toast；重复调用只改文字 + 重置消失倒计时 */
static void toast_show(const char *msg, uint32_t ms)
{
    if (s_toast == NULL) {
        lv_obj_t *scr = lv_screen_active();
        s_toast = lv_obj_create(scr);
        lv_obj_remove_style_all(s_toast);
        lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1C1C1E), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_toast, DM(12), 0);
        lv_obj_set_style_pad_hor(s_toast, DM(20), 0);
        lv_obj_set_style_pad_ver(s_toast, DM(12), 0);
        lv_obj_set_style_max_width(s_toast, lv_pct(90), 0);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);

        s_toast_lbl = lv_label_create(s_toast);
        lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_toast_lbl, FONT_BODY, 0);
        lv_obj_set_style_text_color(s_toast_lbl, lv_color_white(), 0);
        lv_obj_center(s_toast_lbl);
    }
    lv_label_set_text(s_toast_lbl, msg);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -DM(90));
    lv_obj_move_foreground(s_toast);

    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, ms, NULL);
    if (s_toast_timer)
        lv_timer_set_repeat_count(s_toast_timer, 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_screenshot_capture(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_draw_buf_t *db;
    time_t now;
    struct tm tmv;
    char path[sizeof(s_last_path)];
    uint32_t w;
    uint32_t h;
    int rc;

    if (scr == NULL)
        return -1;

    db = lv_snapshot_take(scr, LV_COLOR_FORMAT_XRGB8888);
    if (db == NULL) {
        LV_LOG_ERROR("[screenshot] snapshot failed");
        toast_show("截图失败", 2000);
        return -1;
    }

    w = db->header.w;
    h = db->header.h;

    mkdir("/sdcard", 0755);
    mkdir(DM_SHOT_DIR, 0755);

    /* 无 RTC 时开机为 1970，加 8h 得到北京时间（与 dm_alarm 同惯例）；
     * 追加序号防同秒/未对时情况下重名覆盖。 */
    now = time(NULL) + 8 * 3600;
    gmtime_r(&now, &tmv);
    snprintf(path, sizeof(path), "%s/shot_%04d%02d%02d-%02d%02d%02d-%u.png",
             DM_SHOT_DIR, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (unsigned)(s_seq++ % 1000));

    /* 编码前弹即时提示并立即刷一帧：PNG 编码 1~2s，避免"按了没反应"。
     * 截图已拍完（toast 此后才创建），不会进入画面。 */
    toast_show("正在截图…", 15000);
    lv_refr_now(NULL);

    rc = png_write24(path, db);
    lv_draw_buf_destroy(db);

    if (rc != 0) {
        LV_LOG_ERROR("[screenshot] write %s failed: %d", path, errno);
        toast_show("截图失败", 2000);
        return -1;
    }

    snprintf(s_last_path, sizeof(s_last_path), "%s", path);
    LV_LOG_USER("[screenshot] saved %s (%ux%u)", path,
                (unsigned)w, (unsigned)h);
    {
        char msg[sizeof(s_last_path) + 16];
        snprintf(msg, sizeof(msg), "已保存\n%s", path);
        toast_show(msg, 3000);
    }
    return 0;
}

const char *dm_screenshot_last_path(void)
{
    return s_last_path[0] ? s_last_path : NULL;
}

static void btn_poll_cb(lv_timer_t *t)
{
    struct pollfd pfd;
    btn_buttonset_t cur = 0;
    btn_buttonset_t pressed;

    (void)t;
    if (s_btn_fd < 0)
        return;

    pfd.fd = s_btn_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
        return;

    if (read(s_btn_fd, &cur, sizeof(cur)) != (ssize_t)sizeof(cur))
        return;

    pressed = (btn_buttonset_t)(cur & ~s_prev);
    s_prev = cur;
    if (pressed == 0)
        return;

    uint32_t now = lv_tick_get();
    if (now - s_last_ms < DM_SHOT_DEBOUNCE)
        return;
    s_last_ms = now;

    LV_LOG_USER("[screenshot] key=0x%x", (unsigned)pressed);
    dm_screenshot_capture();
}

int dm_screenshot_init(void)
{
    if (s_btn_fd >= 0)
        return 0;

    s_btn_fd = open(DM_SHOT_DEV, O_RDONLY | O_NONBLOCK);
    if (s_btn_fd < 0) {
        LV_LOG_WARN("[screenshot] open %s failed: %d", DM_SHOT_DEV, errno);
        return -1;
    }

    s_timer = lv_timer_create(btn_poll_cb, DM_SHOT_PERIOD, NULL);
    if (s_timer == NULL) {
        close(s_btn_fd);
        s_btn_fd = -1;
        return -1;
    }
    lv_timer_set_repeat_count(s_timer, -1);

    LV_LOG_USER("[screenshot] ready: any LRADC key -> %s", DM_SHOT_DIR);
    return 0;
}
