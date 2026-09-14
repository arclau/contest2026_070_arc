/*
 * ui_alarm.c — 闹钟子页【2026-09-10 P206：桌面 OS 补闹钟，Apple 范】
 *
 * 列表（时间大字 + 单次/每天 + 开关 + 删除）+ 添加面板
 * （时/分滚轮 + 单次/每天 + 保存）。入口：状态栏时钟点按、
 * Settings 通用行、AI 语音定闹（cmd）。无常驻 timer
 * （引擎 timer 在 dm_alarm_init），close_subpage 无需清理。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_alarm.h"

/* 添加面板状态（打开期间有效） */
static lv_obj_t *g_alarm_add_overlay;
static lv_obj_t *g_alarm_hour_roller;
static lv_obj_t *g_alarm_min_roller;
static lv_obj_t *g_alarm_add_title;    /* 满额提示用 */
static int       g_alarm_add_repeat;   /* 0=一次，1=每天 */
static int       g_alarm_add_tone;     /* 铃声序号（点选即试听） */
static lv_obj_t *g_alarm_tone_val;

static void ui_alarm_rebuild_list(void);
static void ui_alarm_add_close(void);

/* 引擎响一次性后关开关 → 刷新列表（页没开时 g_alarm_list 已空，安全） */
void ui_alarm_refresh_all(void)
{
    ui_alarm_rebuild_list();
}

static void ui_alarm_del_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    reset_idle_timer();
    dm_alarm_del((int)idx);
    ui_alarm_rebuild_list();
}

static void ui_alarm_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    reset_idle_timer();
    dm_alarm_set_enabled((int)idx, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* 滚轮选项串："00\n01\n…\n23"（静态拼一次） */
static const char *ui_alarm_roller_opts(int max)
{
    static char bufs[2][256];
    static int which;
    char *b = bufs[which ^= 1];
    int o = 0, i;

    for (i = 0; i <= max; i++)
        o += snprintf(b + o, sizeof(bufs[0]) - (size_t)o, "%s%02d",
                      i ? "\n" : "", i);
    return b;
}

/* 铃声循环切换 + 即时试听 */
static void ui_alarm_tone_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    g_alarm_add_tone = (g_alarm_add_tone + 1) % dm_alarm_tone_count();
    if (g_alarm_tone_val && lv_obj_is_valid(g_alarm_tone_val))
        lv_label_set_text(g_alarm_tone_val,
                          dm_alarm_tone_label(g_alarm_add_tone));
    dm_tone_play(dm_alarm_tone_name(g_alarm_add_tone));
}

static void ui_alarm_add_save_cb(lv_event_t *e)
{
    (void)e;
    int hh, mm;

    reset_idle_timer();
    hh = (int)lv_roller_get_selected(g_alarm_hour_roller);
    mm = (int)lv_roller_get_selected(g_alarm_min_roller);
    if (dm_alarm_add(hh, mm, g_alarm_add_repeat, g_alarm_add_tone) < 0) {
        /* 2026-09-10 P206 rev：满 8 个给明示（旧代码静默 return） */
        if (g_alarm_add_title && lv_obj_is_valid(g_alarm_add_title))
            lv_label_set_text(g_alarm_add_title, "闹钟已满（8个），先删一个");
        return;
    }
    ui_alarm_add_close();
    ui_alarm_rebuild_list();
}

static void ui_alarm_add_cancel_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    ui_alarm_add_close();
}

static void ui_alarm_add_close(void)
{
    if (g_alarm_add_overlay && lv_obj_is_valid(g_alarm_add_overlay))
        lv_obj_del(g_alarm_add_overlay);
    g_alarm_add_overlay = NULL;
    g_alarm_hour_roller = NULL;
    g_alarm_min_roller = NULL;
    g_alarm_add_title = NULL;
    g_alarm_tone_val = NULL;
}

static void ui_alarm_repeat_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    reset_idle_timer();
    g_alarm_add_repeat = (int)v;
    /* 两个选项互斥高亮：重刷边框 */
    {
        lv_obj_t *row = lv_obj_get_parent(lv_event_get_target(e));
        uint32_t n = lv_obj_get_child_count(row);
        uint32_t i;
        for (i = 0; i < n; i++) {
            lv_obj_t *b = lv_obj_get_child(row, i);
            if (lv_obj_check_type(b, &lv_button_class))
                lv_obj_set_style_border_width(b, 0, 0);
        }
        lv_obj_set_style_border_width(lv_event_get_target(e), 3, 0);
        lv_obj_set_style_border_color(lv_event_get_target(e),
                                      lv_color_hex(COL_BLUE), 0);
    }
}

static void ui_alarm_add_open_cb(lv_event_t *e)
{
    (void)e;
    time_t now;
    struct tm tm;

    reset_idle_timer();
    if (g_alarm_add_overlay)
        return;

    time(&now);
    {
        struct tm *utc = gmtime(&now);
        if (utc == NULL)
            return;
        tm = *utc;
        tm.tm_hour += 8;
        mktime(&tm);
    }

    lv_obj_t *subpage = lv_obj_get_parent(lv_event_get_current_target(e));
    while (subpage && !lv_obj_has_flag(subpage, LV_OBJ_FLAG_FLOATING))
        subpage = lv_obj_get_parent(subpage);
    if (!subpage)
        return;

    g_alarm_add_repeat = 1;   /* 默认每天 */
    g_alarm_add_tone = 0;     /* 默认明亮 */
    g_alarm_add_overlay = lv_obj_create(subpage);
    lv_obj_set_size(g_alarm_add_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_alarm_add_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_alarm_add_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_alarm_add_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_alarm_add_overlay, 0, 0);
    lv_obj_clear_flag(g_alarm_add_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_alarm_add_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(g_alarm_add_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = lv_obj_create(g_alarm_add_overlay);
    lv_obj_set_size(box, DM(420), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, DM(16), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, DM(12), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, DM(8), 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "添加闹钟");
    g_alarm_add_title = title;
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);

    /* 时:分滚轮 */
    {
        lv_obj_t *rrow = lv_obj_create(box);
        lv_obj_set_size(rrow, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(rrow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rrow, 0, 0);
        lv_obj_set_style_pad_all(rrow, 0, 0);
        lv_obj_set_style_pad_column(rrow, DM(4), 0);

        g_alarm_hour_roller = lv_roller_create(rrow);
        lv_roller_set_options(g_alarm_hour_roller, ui_alarm_roller_opts(23),
                              LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(g_alarm_hour_roller, 3);
        lv_roller_set_selected(g_alarm_hour_roller, (uint16_t)tm.tm_hour,
                               LV_ANIM_OFF);
        lv_obj_set_style_text_font(g_alarm_hour_roller, FONT_BODY, 0);

        lv_obj_t *colon = lv_label_create(rrow);
        lv_label_set_text(colon, ":");
        lv_obj_set_style_text_font(colon, FONT_TITLE, 0);
        lv_obj_set_style_text_color(colon, lv_color_hex(COL_TEXT), 0);

        g_alarm_min_roller = lv_roller_create(rrow);
        lv_roller_set_options(g_alarm_min_roller, ui_alarm_roller_opts(59),
                              LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(g_alarm_min_roller, 3);
        lv_roller_set_selected(g_alarm_min_roller, (uint16_t)tm.tm_min,
                               LV_ANIM_OFF);
        lv_obj_set_style_text_font(g_alarm_min_roller, FONT_BODY, 0);
    }

    /* 单次/每天 */
    {
        lv_obj_t *prow = lv_obj_create(box);
        lv_obj_set_size(prow, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_bg_opa(prow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(prow, 0, 0);
        lv_obj_set_style_pad_all(prow, 0, 0);
        lv_obj_set_style_pad_column(prow, DM(8), 0);

        lv_obj_t *once = lv_btn_create(prow);
        lv_obj_set_flex_grow(once, 1);
        lv_obj_set_height(once, DM(40));
        lv_obj_set_style_radius(once, DM(12), 0);
        lv_obj_set_style_bg_color(once, lv_color_hex(0xF0F0F0), 0);
        lv_obj_add_event_cb(once, ui_alarm_repeat_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)0);
        {
            lv_obj_t *l = lv_label_create(once);
            lv_label_set_text(l, "一次");
            lv_obj_set_style_text_font(l, FONT_BODY, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
            lv_obj_center(l);
        }

        lv_obj_t *daily = lv_btn_create(prow);
        lv_obj_set_flex_grow(daily, 1);
        lv_obj_set_height(daily, DM(40));
        lv_obj_set_style_radius(daily, DM(12), 0);
        lv_obj_set_style_bg_color(daily, lv_color_hex(0xF0F0F0), 0);
        lv_obj_set_style_border_width(daily, 3, 0);
        lv_obj_set_style_border_color(daily, lv_color_hex(COL_BLUE), 0);
        lv_obj_add_event_cb(daily, ui_alarm_repeat_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);
        {
            lv_obj_t *l = lv_label_create(daily);
            lv_label_set_text(l, "每天");
            lv_obj_set_style_text_font(l, FONT_BODY, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
            lv_obj_center(l);
        }
    }

    /* 铃声：点按循环切换，切换即试听（dm_tone_play 直播） */
    {
        lv_obj_t *trow = lv_obj_create(box);
        lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(trow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(trow, 0, 0);
        lv_obj_set_style_pad_all(trow, 0, 0);
        lv_obj_set_style_pad_column(trow, DM(8), 0);

        lv_obj_t *cap = lv_label_create(trow);
        lv_label_set_text(cap, "铃声");
        lv_obj_set_style_text_font(cap, FONT_BODY, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(COL_SEC), 0);

        lv_obj_t *tb = lv_btn_create(trow);
        lv_obj_set_flex_grow(tb, 1);
        lv_obj_set_height(tb, DM(40));
        lv_obj_set_style_radius(tb, DM(12), 0);
        lv_obj_set_style_bg_color(tb, lv_color_hex(0xF0F0F0), 0);
        lv_obj_set_style_border_width(tb, 0, 0);
        lv_obj_add_event_cb(tb, ui_alarm_tone_cb, LV_EVENT_CLICKED, NULL);
        g_alarm_tone_val = lv_label_create(tb);
        lv_label_set_text(g_alarm_tone_val, dm_alarm_tone_label(0));
        lv_obj_set_style_text_font(g_alarm_tone_val, FONT_BODY, 0);
        lv_obj_set_style_text_color(g_alarm_tone_val, lv_color_hex(COL_TEXT), 0);
        lv_obj_center(g_alarm_tone_val);
    }

    /* 保存/取消 */
    {
        lv_obj_t *brow = lv_obj_create(box);
        lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(brow, 0, 0);
        lv_obj_set_style_pad_all(brow, 0, 0);
        lv_obj_set_style_pad_column(brow, DM(8), 0);

        lv_obj_t *cancel = lv_btn_create(brow);
        lv_obj_set_flex_grow(cancel, 1);
        lv_obj_set_height(cancel, DM(40));
        lv_obj_set_style_radius(cancel, DM(12), 0);
        lv_obj_set_style_bg_color(cancel, lv_color_hex(0xF0F0F0), 0);
        lv_obj_set_style_border_width(cancel, 0, 0);
        lv_obj_add_event_cb(cancel, ui_alarm_add_cancel_cb, LV_EVENT_CLICKED, NULL);
        {
            lv_obj_t *l = lv_label_create(cancel);
            lv_label_set_text(l, "取消");
            lv_obj_set_style_text_font(l, FONT_BODY, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
            lv_obj_center(l);
        }

        lv_obj_t *save = lv_btn_create(brow);
        lv_obj_set_flex_grow(save, 1);
        lv_obj_set_height(save, DM(40));
        lv_obj_set_style_radius(save, DM(12), 0);
        lv_obj_set_style_bg_color(save, lv_color_hex(COL_ORANGE), 0);
        lv_obj_set_style_border_width(save, 0, 0);
        lv_obj_add_event_cb(save, ui_alarm_add_save_cb, LV_EVENT_CLICKED, NULL);
        {
            lv_obj_t *l = lv_label_create(save);
            lv_label_set_text(l, "保存");
            lv_obj_set_style_text_font(l, FONT_BODY, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(l);
        }
    }
}

/* 闹钟列表容器（重建式刷新） */
static lv_obj_t *g_alarm_list;

static void ui_alarm_rebuild_list(void)
{
    int n, i;

    if (!g_alarm_list || !lv_obj_is_valid(g_alarm_list))
        return;
    lv_obj_clean(g_alarm_list);
    n = dm_alarm_count();
    if (n == 0) {
        lv_obj_t *em = lv_label_create(g_alarm_list);
        lv_label_set_text(em, "还没有闹钟，点下面添加");
        lv_obj_set_style_text_font(em, FONT_BODY, 0);
        lv_obj_set_style_text_color(em, lv_color_hex(COL_SEC), 0);
        return;
    }
    for (i = 0; i < n; i++) {
        const dm_alarm_t *a = dm_alarm_get(i);
        char tbuf[16];
        lv_obj_t *row = lv_obj_create(g_alarm_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, DM(12), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, DM(8), 0);
        lv_obj_set_style_pad_column(row, DM(8), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", a->hh, a->mm);
        lv_obj_t *tt = lv_label_create(row);
        lv_label_set_text(tt, tbuf);
        lv_obj_set_style_text_font(tt, FONT_BODY, 0);
        lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(tt, 1);

        lv_obj_t *rp = lv_label_create(row);
        {
            char rbuf[32];
            snprintf(rbuf, sizeof(rbuf), "%s · %s",
                     a->repeat ? "每天" : "一次",
                     dm_alarm_tone_label(a->tone));
            lv_label_set_text(rp, rbuf);
        }
        lv_obj_set_style_text_font(rp, FONT_LABEL, 0);
        lv_obj_set_style_text_color(rp, lv_color_hex(COL_SEC), 0);

        lv_obj_t *sw = lv_switch_create(row);
        if (a->enabled)
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, ui_alarm_sw_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);

        lv_obj_t *del = lv_btn_create(row);
        lv_obj_set_size(del, DM(36), DM(36));
        lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_0, 0);
        lv_obj_set_style_border_width(del, 0, 0);
        lv_obj_set_style_shadow_width(del, 0, 0);
        lv_obj_add_event_cb(del, ui_alarm_del_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        {
            lv_obj_t *ic = lv_label_create(del);
            lv_label_set_text(ic, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_font(ic, FONT_BODY, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(COL_SEC), 0);
            lv_obj_center(ic);
        }
    }
}

void ui_alarm_close_cleanup(void)
{
    ui_alarm_add_close();
    g_alarm_list = NULL;
}

void ui_alarm_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, DM(6), 0);

    subpage_big_title(parent, "闹钟");

    g_alarm_list = lv_obj_create(parent);
    lv_obj_set_size(g_alarm_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_alarm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(g_alarm_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_alarm_list, 0, 0);
    lv_obj_set_style_pad_all(g_alarm_list, 0, 0);
    lv_obj_set_style_pad_row(g_alarm_list, DM(6), 0);
    lv_obj_clear_flag(g_alarm_list, LV_OBJ_FLAG_SCROLLABLE);
    ui_alarm_rebuild_list();

    /* 添加按钮 */
    {
        lv_obj_t *add = lv_btn_create(parent);
        lv_obj_set_size(add, lv_pct(100), DM(48));
        lv_obj_set_style_radius(add, DM(14), 0);
        lv_obj_set_style_bg_color(add, lv_color_hex(COL_ORANGE), 0);
        lv_obj_add_event_cb(add, ui_alarm_add_open_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *al = lv_label_create(add);
        lv_label_set_text(al, "+ 添加闹钟");
        lv_obj_set_style_text_font(al, FONT_BODY, 0);
        lv_obj_set_style_text_color(al, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(al);
    }
}
