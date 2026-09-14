/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_alarm.c
 * 闹钟引擎实现（见 dm_alarm.h）。
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lvgl/lvgl.h>

#include "dm_alarm.h"
#include "deskmate_ui.h"   /* dm_tone_play_seq / reset_idle_timer */

#define DM_ALARM_FILE   "/data/dm_alarm.list"
#define DM_ALARM_TMP    "/data/dm_alarm.tmp"    /* 原子落盘用（写tmp再rename） */

void ui_alarm_refresh_all(void);   /* ui_alarm.c：列表重刷（页没开则内部空转） */

static dm_alarm_t g_alarms[DM_ALARM_MAX];
static int        g_alarm_n;
static int        g_fire_tone;          /* 正在响的铃声序号（再响继承用，修snooze固定明亮） */
static long       g_snooze_absmin = -1; /* 再响目标（单调分钟，跨天跨年安全） */
static void dm_alarm_fire(const dm_alarm_t *a);

/* 铃声表（res/tones/ 实有 wav；试听即 dm_tone_play 直播） */
static const struct { const char *label, *name; } g_tones[] = {
    { "明亮", "bg_reward_high" },
    { "温柔", "care_01" },
    { "豪华", "bg_reward_lux" },
    { "心动", "bg_reward_heart" },
    { "标准", "bg_reward_std" },
    { "滴答", "popup_sit_1" },
};
#define DM_ALARM_TONE_N   ((int)(sizeof(g_tones) / sizeof(g_tones[0])))

int dm_alarm_tone_count(void)
{
    return DM_ALARM_TONE_N;
}

const char *dm_alarm_tone_name(int idx)
{
    if (idx < 0 || idx >= DM_ALARM_TONE_N)
        idx = 0;
    return g_tones[idx].name;
}

const char *dm_alarm_tone_label(int idx)
{
    if (idx < 0 || idx >= DM_ALARM_TONE_N)
        idx = 0;
    return g_tones[idx].label;
}

static void dm_alarm_save(void)
{
    FILE *f = fopen(DM_ALARM_TMP, "w");
    int i;

    if (!f)
        return;
    for (i = 0; i < g_alarm_n; i++)
        fprintf(f, "%d %d %d %d %d\n", g_alarms[i].hh, g_alarms[i].mm,
                g_alarms[i].repeat, g_alarms[i].enabled, g_alarms[i].tone);
    fclose(f);
    /* 原子替换：写tmp再rename，掉电/满盘只丢新档不坏旧档 */
    rename(DM_ALARM_TMP, DM_ALARM_FILE);
}

/* 按时间排序（列表按先后展示，读盘/添加后调用） */
static void dm_alarm_sort(void)
{
    int i, j;

    for (i = 1; i < g_alarm_n; i++) {
        dm_alarm_t t = g_alarms[i];
        int tk = t.hh * 60 + t.mm;
        j = i - 1;
        while (j >= 0 && g_alarms[j].hh * 60 + g_alarms[j].mm > tk) {
            g_alarms[j + 1] = g_alarms[j];
            j--;
        }
        g_alarms[j + 1] = t;
    }
}

static void dm_alarm_load(void)
{
    FILE *f = fopen(DM_ALARM_FILE, "r");
    int hh, mm, rp, en;

    g_alarm_n = 0;
    if (!f)
        return;
    /* P215：旧 4 字段档逐行解析——fscanf 跨行取数时第 5 个 %d 会偷下行
     * 行首 hh，导致整档错位。改 fgets 按行 + sscanf，4/5 字段天然兼容，
     * 缺省 tone=明亮(0)。 */
    char line[64];
    while (g_alarm_n < DM_ALARM_MAX && fgets(line, sizeof(line), f))
        {
            int to = 0;
            int n = sscanf(line, "%d %d %d %d %d", &hh, &mm, &rp, &en, &to);
            if (n != 4 && n != 5)
                continue;
            if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
                continue;
            g_alarms[g_alarm_n].hh = hh;
            g_alarms[g_alarm_n].mm = mm;
            g_alarms[g_alarm_n].repeat = rp ? 1 : 0;
            g_alarms[g_alarm_n].enabled = en ? 1 : 0;
            g_alarms[g_alarm_n].tone = (to >= 0 && to < DM_ALARM_TONE_N) ? to : 0;
            g_alarm_n++;
        }
    fclose(f);
    dm_alarm_sort();
}

int dm_alarm_count(void)
{
    return g_alarm_n;
}

const dm_alarm_t *dm_alarm_get(int idx)
{
    if (idx < 0 || idx >= g_alarm_n)
        return NULL;
    return &g_alarms[idx];
}

int dm_alarm_add(int hh, int mm, int repeat, int tone)
{
    int i, pos, key;

    if (g_alarm_n >= DM_ALARM_MAX)
        return -1;
    if (hh < 0) hh = 0;
    if (hh > 23) hh = 23;
    if (mm < 0) mm = 0;
    if (mm > 59) mm = 59;
    /* 有序插入（列表恒按时间先后，返回真实序号） */
    key = hh * 60 + mm;
    pos = 0;
    while (pos < g_alarm_n &&
           g_alarms[pos].hh * 60 + g_alarms[pos].mm <= key)
        pos++;
    for (i = g_alarm_n; i > pos; i--)
        g_alarms[i] = g_alarms[i - 1];
    g_alarms[pos].hh = hh;
    g_alarms[pos].mm = mm;
    g_alarms[pos].repeat = repeat ? 1 : 0;
    g_alarms[pos].enabled = 1;
    g_alarms[pos].tone = (tone >= 0 && tone < DM_ALARM_TONE_N) ? tone : 0;
    g_alarm_n++;
    dm_alarm_save();
    return pos;
}

void dm_alarm_del(int idx)
{
    int i;

    if (idx < 0 || idx >= g_alarm_n)
        return;
    for (i = idx; i + 1 < g_alarm_n; i++)
        g_alarms[i] = g_alarms[i + 1];
    g_alarm_n--;
    dm_alarm_save();
}

void dm_alarm_set_enabled(int idx, int on)
{
    if (idx < 0 || idx >= g_alarm_n)
        return;
    g_alarms[idx].enabled = on ? 1 : 0;
    dm_alarm_save();
}

/* ── 响铃 ── */

static lv_obj_t *g_alarm_popup;

static void dm_alarm_popup_close(void)
{
    if (g_alarm_popup && lv_obj_is_valid(g_alarm_popup))
        lv_obj_del(g_alarm_popup);
    g_alarm_popup = NULL;
}

static void dm_alarm_stop_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dm_alarm_popup_close();
}

static void dm_alarm_snooze_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dm_alarm_snooze();
    dm_alarm_popup_close();
}

/* 覆盖层挂 lv_scr_act() 置顶（锁屏 overlay 之上也看得见，不用先唤醒） */
static void dm_alarm_fire(const dm_alarm_t *a)
{
    char tbuf[16];

    reset_idle_timer();
    dm_alarm_popup_close();
    /* 记下本次铃声：再响5分钟继承原声（修旧代码未初始化栈变量致恒明亮） */
    g_fire_tone = (a->tone >= 0 && a->tone < DM_ALARM_TONE_N) ? a->tone : 0;
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", a->hh, a->mm);

    g_alarm_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_alarm_popup, DM(360), LV_SIZE_CONTENT);
    lv_obj_center(g_alarm_popup);
    lv_obj_set_style_bg_color(g_alarm_popup, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(g_alarm_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_alarm_popup, DM(24), 0);
    lv_obj_set_style_border_width(g_alarm_popup, 0, 0);
    lv_obj_set_style_pad_all(g_alarm_popup, DM(20), 0);
    lv_obj_set_flex_flow(g_alarm_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_alarm_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_alarm_popup, DM(12), 0);
    lv_obj_move_foreground(g_alarm_popup);

    lv_obj_t *icon = lv_label_create(g_alarm_popup);
    lv_label_set_text(icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(icon, FONT_ICON, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_ORANGE), 0);

    lv_obj_t *tt = lv_label_create(g_alarm_popup);
    lv_label_set_text(tt, tbuf);
    lv_obj_set_style_text_font(tt, FONT_TITLE, 0);
    lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *cap = lv_label_create(g_alarm_popup);
    lv_label_set_text(cap, "时间到");
    lv_obj_set_style_text_font(cap, FONT_BODY, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(COL_SEC), 0);

    lv_obj_t *row = lv_obj_create(g_alarm_popup);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, DM(8), 0);

    lv_obj_t *sn = lv_btn_create(row);
    lv_obj_set_flex_grow(sn, 1);
    lv_obj_set_height(sn, DM(40));
    lv_obj_set_style_radius(sn, DM(12), 0);
    lv_obj_set_style_bg_color(sn, lv_color_hex(0xF0F0F0), 0);
    lv_obj_add_event_cb(sn, dm_alarm_snooze_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *l = lv_label_create(sn);
        lv_label_set_text(l, "再响5分钟");
        lv_obj_set_style_text_font(l, FONT_BODY, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
        lv_obj_center(l);
    }

    lv_obj_t *ok = lv_btn_create(row);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, DM(40));
    lv_obj_set_style_radius(ok, DM(12), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_ORANGE), 0);
    lv_obj_add_event_cb(ok, dm_alarm_stop_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *l = lv_label_create(ok);
        lv_label_set_text(l, "关闭");
        lv_obj_set_style_text_font(l, FONT_BODY, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
    }

    /* 响铃声（所选铃声直拔，离线也响；不用 TTS，省 token） */
    dm_tone_play(dm_alarm_tone_name(a->tone));
}

void dm_alarm_snooze(void)
{
    /* 2026-09-10 P206 rev：旧 yday 方案跨年断（12-31→01-01 yday 归零对不上），
     * 改单调分钟 time()/60+5，跨天跨年天然连续。 */
    g_snooze_absmin = (long)(time(NULL) / 60) + 5;
}

/* 1s 检查（与状态栏时钟同源 +8；分钟变化才全量扫描，同分钟多闹钟全收） */
static void dm_alarm_tick(lv_timer_t *t)
{
    time_t now;
    struct tm tm;
    long cur_min;
    int i, fire_idx = -1;

    (void)t;
    time(&now);
    /* 分钟门控：1s 唤醒只做一次 time()+除法，分钟没变直接回（省电） */
    cur_min = (long)(now / 60);
    {
        static long s_last_probe = -1;
        if (cur_min == s_last_probe)
            return;
        s_last_probe = cur_min;
    }
    {
        struct tm *utc = gmtime(&now);
        if (utc == NULL)
            return;
        tm = *utc;
        tm.tm_hour += 8;
        mktime(&tm);
    }

    /* 再响（单调分钟比对，跨天跨年安全；铃声继承原闹钟） */
    if (g_snooze_absmin >= 0 && cur_min >= g_snooze_absmin) {
        dm_alarm_t a;
        a.hh = tm.tm_hour;
        a.mm = tm.tm_min;
        a.repeat = 0;
        a.enabled = 1;
        a.tone = g_fire_tone;
        g_snooze_absmin = -1;
        dm_alarm_fire(&a);
        return;
    }

    /* 同分钟多闹钟全收：一次性全部自关，只弹一个窗（旧代码 return 只响第一个） */
    for (i = 0; i < g_alarm_n; i++) {
        if (!g_alarms[i].enabled)
            continue;
        if (g_alarms[i].hh == tm.tm_hour && g_alarms[i].mm == tm.tm_min) {
            if (fire_idx < 0)
                fire_idx = i;
            if (!g_alarms[i].repeat)
                g_alarms[i].enabled = 0;
        }
    }
    if (fire_idx >= 0) {
        dm_alarm_save();
        dm_alarm_fire(&g_alarms[fire_idx]);
        /* UI 若开着闹钟页则刷新开关态（一次性响后关） */
        ui_alarm_refresh_all();
    }
}

void dm_alarm_init(void)
{
    dm_alarm_load();
    lv_timer_create(dm_alarm_tick, 1000, NULL);
}
