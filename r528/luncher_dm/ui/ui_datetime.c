/*
 * ui_datetime.c — 日期与时间子页【2026-09-10 P211：Settings 通用区独立子页，宠物同款嵌套】
 *
 * 结构（iOS 设置→通用→日期与时间同款）：
 *   hero 大时钟（1s 活刷）+ 自动设置区（总闸开关/上次同步/立即同步）
 *   + 手动设置区（日期/时间行 → 年月日时分滚轮面板，星期预览 + 月日联动）。
 * 总闸是真开关：关则天气链路不再 settimeofday（dm_weather 门控），手动长期有效。
 * 关页经 ui_datetime_close_cleanup 停 timer + 清面板（close_subpage 接线）。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_weather.h"   /* 自动总闸/上次同步/HTTP Date 对时门控 */

#define DT_YEAR_LO   2000
#define DT_YEAR_HI   2099

/* ── 本地日期工具（与 ui_calendar.c 同款，不跨文件引用） ── */

static int dt_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int dt_dim(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31 };
    if (m == 2 && dt_leap(y))
        return 29;
    return (m >= 1 && m <= 12) ? d[m - 1] : 30;
}

/* 东八区墙钟（与 ui_home.c 时钟同源：gmtime + 8h + mktime 归一化） */
static void dt_now(struct tm *out)
{
    time_t now;

    time(&now);
    {
        struct tm *utc = gmtime(&now);
        if (utc == NULL) {
            memset(out, 0, sizeof(*out));
            out->tm_year = 2026 - 1900;
            out->tm_mon = 8;
            out->tm_mday = 10;
            return;
        }
        *out = *utc;
        out->tm_hour += 8;
        mktime(out);
    }
}

static const char *dt_wday_cn(int w)
{
    static const char *n[] = { "周日", "周一", "周二", "周三",
                               "周四", "周五", "周六" };
    return (w >= 0 && w < 7) ? n[w] : "";
}

/* 年月日→周几（中午 12 点归一化，避开午夜边界） */
static int dt_wday_of(int y, int mo, int d)
{
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    mktime(&tm);
    return tm.tm_wday;
}

/* ── 页面状态 ── */

static lv_obj_t *g_dt_hero_time;
static lv_obj_t *g_dt_hero_date;
static lv_obj_t *g_dt_auto_hint;
static lv_obj_t *g_dt_sync_val;
static lv_obj_t *g_dt_date_val;
static lv_obj_t *g_dt_time_val;
static lv_timer_t *g_dt_timer;
static long g_dt_shown_sync = -1;   /* 已展示的上次同步值（去重刷） */

/* ── hero + 同步行刷新 ── */

static void dt_refresh_clock(void)
{
    struct tm tm;
    char tbuf[16], dbuf[48];

    if (!g_dt_hero_time || !lv_obj_is_valid(g_dt_hero_time))
        return;
    dt_now(&tm);
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(g_dt_hero_time, tbuf);
    if (g_dt_hero_date && lv_obj_is_valid(g_dt_hero_date)) {
        snprintf(dbuf, sizeof(dbuf), "%d月%d日 %s",
                 tm.tm_mon + 1, tm.tm_mday, dt_wday_cn(tm.tm_wday));
        lv_label_set_text(g_dt_hero_date, dbuf);
    }
}

/* 上次同步行：只在值变化时重刷（省 LVGL 重排） */
static void dt_refresh_sync(void)
{
    long last = dm_time_last_sync();
    char buf[48];

    if (last == g_dt_shown_sync)
        return;
    g_dt_shown_sync = last;
    if (!g_dt_sync_val || !lv_obj_is_valid(g_dt_sync_val))
        return;
    if (last <= 0) {
        lv_label_set_text(g_dt_sync_val, "尚未同步");
        return;
    }
    {
        time_t t = (time_t)last + 8 * 3600;
        struct tm *utc = gmtime(&t);
        if (utc == NULL) {
            lv_label_set_text(g_dt_sync_val, "尚未同步");
            return;
        }
        snprintf(buf, sizeof(buf), "%d月%d日 %02d:%02d",
                 utc->tm_mon + 1, utc->tm_mday,
                 utc->tm_hour, utc->tm_min);
        lv_label_set_text(g_dt_sync_val, buf);
    }
}

static void dt_tick_cb(lv_timer_t *t)
{
    (void)t;
    dt_refresh_clock();
    dt_refresh_sync();
}

/* ── 自动设置区 ── */

static void dt_refresh_auto_hint(void)
{
    if (!g_dt_auto_hint || !lv_obj_is_valid(g_dt_auto_hint))
        return;
    lv_label_set_text(g_dt_auto_hint,
        dm_time_auto_enabled()
        ? "已联网自动校准（天气链路），手动设置下次同步后被覆盖"
        : "自动同步已关闭，手动设置长期有效");
}

static void dt_auto_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;

    reset_idle_timer();
    dm_time_set_auto(on);
    dt_refresh_auto_hint();
    if (on)
        weather_kick_fetch();   /* 打开即同步一次 */
}

static void dt_sync_now_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    weather_kick_fetch();
}

/* 手动行值刷新（保存后调用；行指针页内有效，先声明） */
static void wdt_refresh_manual_rows(int y, int mo, int d, int h, int mi);

/* ── 手动滚轮面板（年月日时分 + 星期预览 + 月日联动） ── */

static lv_obj_t *g_dt_overlay;
static lv_obj_t *g_dt_rollers[5];
static lv_obj_t *g_dt_preview;

/* 选项串静态缓冲（set_options 会拷贝，进回调前保持有效即可） */
static char g_dt_opts_y[512];   /* 2000~2099：100×5-1=499B */
static char g_dt_opts_mo[64];
static char g_dt_opts_d[96];    /* 31×3-1=92B */
static char g_dt_opts_h[96];
static char g_dt_opts_mi[256];  /* 60×3-1=179B */

static void dt_build_opts(char *b, size_t cap, int lo, int hi)
{
    size_t o = 0;
    int i;

    for (i = lo; i <= hi; i++)
        o += snprintf(b + o, cap > o ? cap - o : 0, "%s%02d",
                      i == lo ? "" : "\n", i);
}

static void dt_close_panel(void)
{
    if (g_dt_overlay && lv_obj_is_valid(g_dt_overlay))
        lv_obj_del(g_dt_overlay);
    g_dt_overlay = NULL;
    g_dt_rollers[0] = g_dt_rollers[1] = g_dt_rollers[2] =
        g_dt_rollers[3] = g_dt_rollers[4] = NULL;
    g_dt_preview = NULL;
}

/* 按当前滚轮值刷星期预览；年月变化时联动日滚轮上限（修 2 月 31 日） */
static void dt_update_preview(void)
{
    int y, mo, d, h, mi, dim, w;
    char buf[48];

    if (!g_dt_preview || !lv_obj_is_valid(g_dt_preview))
        return;
    if (!g_dt_rollers[0] || !lv_obj_is_valid(g_dt_rollers[0]))
        return;
    y = DT_YEAR_LO + (int)lv_roller_get_selected(g_dt_rollers[0]);
    mo = 1 + (int)lv_roller_get_selected(g_dt_rollers[1]);
    dim = dt_dim(y, mo);
    d = 1 + (int)lv_roller_get_selected(g_dt_rollers[2]);
    /* 年月动了且日超限：重建日选项并钳住（先读后建，避免选中态丢失） */
    if (d > dim) {
        dt_build_opts(g_dt_opts_d, sizeof(g_dt_opts_d), 1, dim);
        lv_roller_set_options(g_dt_rollers[2], g_dt_opts_d,
                              LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(g_dt_rollers[2], (uint16_t)(dim - 1),
                               LV_ANIM_OFF);
        d = dim;
    }
    h = (int)lv_roller_get_selected(g_dt_rollers[3]);
    mi = (int)lv_roller_get_selected(g_dt_rollers[4]);
    w = dt_wday_of(y, mo, d);
    snprintf(buf, sizeof(buf), "%d年%d月%d日 %s %02d:%02d",
             y, mo, d, dt_wday_cn(w), h, mi);
    lv_label_set_text(g_dt_preview, buf);
}

static void dt_roller_changed_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dt_update_preview();
}

static void dt_panel_cancel_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dt_close_panel();
}

static void dt_panel_save_cb(lv_event_t *e)
{
    struct tm tm;
    struct timeval tv;
    int y, mo, d, hh, mi;

    (void)e;
    reset_idle_timer();
    if (!g_dt_rollers[0] || !lv_obj_is_valid(g_dt_rollers[0]))
        return;
    y = DT_YEAR_LO + (int)lv_roller_get_selected(g_dt_rollers[0]);
    mo = 1 + (int)lv_roller_get_selected(g_dt_rollers[1]);
    d = 1 + (int)lv_roller_get_selected(g_dt_rollers[2]);
    if (d > dt_dim(y, mo))
        d = dt_dim(y, mo);   /* 兜底钳位（预览已联动，此处防万一） */
    hh = (int)lv_roller_get_selected(g_dt_rollers[3]);
    mi = (int)lv_roller_get_selected(g_dt_rollers[4]);
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mi;
    /* 板上 epoch 按 UTC 走（显示层 +8）：手配东八区墙钟→转 UTC 落时钟 */
    tv.tv_sec = mktime(&tm) - 8 * 3600;
    tv.tv_usec = 0;
    if (tv.tv_sec > 0)
        settimeofday(&tv, NULL);
    /* 行值即时刷新（hero 下一秒 timer 自刷） */
    wdt_refresh_manual_rows(y, mo, d, hh, mi);
    LV_LOG_USER("Manual datetime set %04d-%02d-%02d %02d:%02d",
                y, mo, d, hh, mi);
    dt_close_panel();
}

/* 手动行值刷新（保存后调用；行指针页内有效） */
static void wdt_refresh_manual_rows(int y, int mo, int d, int h, int mi)
{
    char dbuf[32], tbuf[16];

    snprintf(dbuf, sizeof(dbuf), "%d月%d日 %s", mo, d, dt_wday_cn(dt_wday_of(y, mo, d)));
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, mi);
    if (g_dt_date_val && lv_obj_is_valid(g_dt_date_val))
        lv_label_set_text(g_dt_date_val, dbuf);
    if (g_dt_time_val && lv_obj_is_valid(g_dt_time_val))
        lv_label_set_text(g_dt_time_val, tbuf);
}

static void dt_panel_open_cb(lv_event_t *e)
{
    struct tm tm;
    static const char *caps[] = { "年", "月", "日", "时", "分" };
    int vals[5], maxs[5], i;
    lv_obj_t *subpage, *box, *title, *rrow, *brow, *cancel, *save, *l;

    (void)e;
    reset_idle_timer();
    if (g_dt_overlay)
        return;
    dt_now(&tm);

    subpage = lv_obj_get_parent(lv_event_get_current_target(e));
    while (subpage && !lv_obj_has_flag(subpage, LV_OBJ_FLAG_FLOATING))
        subpage = lv_obj_get_parent(subpage);
    if (!subpage)
        return;

    /* 当前值钳进 2000~2099（RTC 没电回到 1970 也不崩） */
    vals[0] = tm.tm_year + 1900;
    if (vals[0] < DT_YEAR_LO) vals[0] = DT_YEAR_LO;
    if (vals[0] > DT_YEAR_HI) vals[0] = DT_YEAR_HI;
    vals[1] = tm.tm_mon + 1;
    vals[2] = tm.tm_mday;
    vals[3] = tm.tm_hour;
    vals[4] = tm.tm_min;
    maxs[0] = DT_YEAR_HI - DT_YEAR_LO;
    maxs[1] = 11;
    maxs[2] = dt_dim(vals[0], vals[1]) - 1;
    maxs[3] = 23;
    maxs[4] = 59;
    if (vals[2] > maxs[2] + 1) vals[2] = maxs[2] + 1;

    dt_build_opts(g_dt_opts_y, sizeof(g_dt_opts_y), DT_YEAR_LO, DT_YEAR_HI);
    dt_build_opts(g_dt_opts_mo, sizeof(g_dt_opts_mo), 1, 12);
    dt_build_opts(g_dt_opts_d, sizeof(g_dt_opts_d), 1, maxs[2] + 1);
    dt_build_opts(g_dt_opts_h, sizeof(g_dt_opts_h), 0, 23);
    dt_build_opts(g_dt_opts_mi, sizeof(g_dt_opts_mi), 0, 59);

    g_dt_overlay = lv_obj_create(subpage);
    lv_obj_set_size(g_dt_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_dt_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_dt_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_dt_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_dt_overlay, 0, 0);
    lv_obj_clear_flag(g_dt_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_dt_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(g_dt_overlay, LV_OBJ_FLAG_CLICKABLE);

    box = lv_obj_create(g_dt_overlay);
    lv_obj_set_size(box, DM(560), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, DM(16), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, DM(12), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, DM(8), 0);

    title = lv_label_create(box);
    lv_label_set_text(title, "设置日期与时间");
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);

    /* 星期预览（滚轮一动即刷，所见即所得） */
    g_dt_preview = lv_label_create(box);
    lv_obj_set_style_text_font(g_dt_preview, FONT_LABEL, 0);
    lv_obj_set_style_text_color(g_dt_preview, lv_color_hex(COL_BLUE), 0);

    rrow = lv_obj_create(box);
    lv_obj_set_size(rrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(rrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rrow, 0, 0);
    lv_obj_set_style_pad_all(rrow, 0, 0);
    lv_obj_set_style_pad_column(rrow, DM(2), 0);
    {
        char *opts[5] = { g_dt_opts_y, g_dt_opts_mo, g_dt_opts_d,
                          g_dt_opts_h, g_dt_opts_mi };
        int base[5] = { DT_YEAR_LO, 1, 1, 0, 0 };
        for (i = 0; i < 5; i++) {
            lv_obj_t *cap;
            g_dt_rollers[i] = lv_roller_create(rrow);
            lv_roller_set_options(g_dt_rollers[i], opts[i],
                                  LV_ROLLER_MODE_NORMAL);
            lv_roller_set_visible_row_count(g_dt_rollers[i], 3);
            lv_roller_set_selected(g_dt_rollers[i],
                                   (uint16_t)(vals[i] - base[i]),
                                   LV_ANIM_OFF);
            lv_obj_set_style_text_font(g_dt_rollers[i], FONT_LABEL, 0);
            lv_obj_add_event_cb(g_dt_rollers[i], dt_roller_changed_cb,
                                LV_EVENT_VALUE_CHANGED, NULL);
            cap = lv_label_create(rrow);
            lv_label_set_text(cap, caps[i]);
            lv_obj_set_style_text_font(cap, FONT_LABEL, 0);
            lv_obj_set_style_text_color(cap, lv_color_hex(COL_SEC), 0);
        }
    }
    dt_update_preview();

    brow = lv_obj_create(box);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_column(brow, DM(8), 0);

    cancel = lv_btn_create(brow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, DM(40));
    lv_obj_set_style_radius(cancel, DM(12), 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, dt_panel_cancel_cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(cancel);
    lv_label_set_text(l, "取消");
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l);

    save = lv_btn_create(brow);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_height(save, DM(40));
    lv_obj_set_style_radius(save, DM(12), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(save, 0, 0);
    lv_obj_add_event_cb(save, dt_panel_save_cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(save);
    lv_label_set_text(l, "保存");
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
}

/* ── 关页清理（close_subpage 接线） ── */

void ui_datetime_close_cleanup(void)
{
    if (g_dt_timer) {
        lv_timer_del(g_dt_timer);
        g_dt_timer = NULL;
    }
    dt_close_panel();
    g_dt_hero_time = NULL;
    g_dt_hero_date = NULL;
    g_dt_auto_hint = NULL;
    g_dt_sync_val = NULL;
    g_dt_date_val = NULL;
    g_dt_time_val = NULL;
    g_dt_shown_sync = -1;
}

/* ── 子页构建（show_subpage("Datetime") → deskmate_ui.c dispatch） ── */

void ui_datetime_create(lv_obj_t *parent)
{
    struct tm tm;
    char dbuf[32], tbuf[16];

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, DM(6), 0);

    subpage_big_title(parent, "日期与时间");

    /* hero 大时钟卡 */
    {
        lv_obj_t *hero = settings_group_card(parent);
        lv_obj_set_style_pad_top(hero, DM(12), 0);
        lv_obj_set_style_pad_bottom(hero, DM(12), 0);
        g_dt_hero_time = lv_label_create(hero);
        lv_obj_set_style_text_font(g_dt_hero_time, FONT_CLOCK, 0);
        lv_obj_set_style_text_color(g_dt_hero_time,
                                    lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_align(g_dt_hero_time,
                                    LV_TEXT_ALIGN_CENTER, 0);
        g_dt_hero_date = lv_label_create(hero);
        lv_obj_set_style_text_font(g_dt_hero_date, FONT_BODY, 0);
        lv_obj_set_style_text_color(g_dt_hero_date,
                                    lv_color_hex(COL_SEC), 0);
        lv_obj_set_style_text_align(g_dt_hero_date,
                                    LV_TEXT_ALIGN_CENTER, 0);
        dt_refresh_clock();
    }

    /* 自动设置区 */
    settings_section_label(parent, "自动设置");
    {
        lv_obj_t *card = settings_group_card(parent);
        settings_row_switch_cb(card, LV_SYMBOL_REFRESH, COL_BLUE, "自动设置",
                               dm_time_auto_enabled(), dt_auto_sw_cb);
        settings_add_separator(card);
        lv_obj_t *sync_row = settings_row_value(card, LV_SYMBOL_LOOP,
                                                COL_GREEN, "上次同步",
                                                "尚未同步", false);
        g_dt_sync_val = (lv_obj_t *)lv_obj_get_user_data(sync_row);
        settings_add_separator(card);
        lv_obj_t *now_row = settings_row_value(card, LV_SYMBOL_GPS,
                                               COL_BLUE, "立即同步",
                                               "", true);
        lv_obj_add_event_cb(now_row, dt_sync_now_cb, LV_EVENT_CLICKED, NULL);
        /* 说明小字（卡内底部署名式提示） */
        lv_obj_t *hint = lv_label_create(card);
        lv_obj_set_style_text_font(hint, FONT_LABEL, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_SEC), 0);
        lv_obj_set_style_pad_top(hint, DM(4), 0);
        lv_obj_set_style_pad_bottom(hint, DM(4), 0);
        g_dt_auto_hint = hint;
        dt_refresh_auto_hint();
    }

    /* 手动设置区 */
    settings_section_label(parent, "手动设置");
    {
        lv_obj_t *card = settings_group_card(parent);
        dt_now(&tm);
        snprintf(dbuf, sizeof(dbuf), "%d月%d日 %s",
                 tm.tm_mon + 1, tm.tm_mday, dt_wday_cn(tm.tm_wday));
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_obj_t *date_row = settings_row_value(card, LV_SYMBOL_LIST,
                                                COL_ORANGE, "日期",
                                                dbuf, true);
        g_dt_date_val = (lv_obj_t *)lv_obj_get_user_data(date_row);
        lv_obj_add_event_cb(date_row, dt_panel_open_cb,
                            LV_EVENT_CLICKED, NULL);
        settings_add_separator(card);
        lv_obj_t *time_row = settings_row_value(card, LV_SYMBOL_EDIT,
                                                COL_BLUE, "时间",
                                                tbuf, true);
        g_dt_time_val = (lv_obj_t *)lv_obj_get_user_data(time_row);
        lv_obj_add_event_cb(time_row, dt_panel_open_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    g_dt_shown_sync = -1;
    dt_refresh_sync();
    if (!g_dt_timer)
        g_dt_timer = lv_timer_create(dt_tick_cb, 1000, NULL);
}
