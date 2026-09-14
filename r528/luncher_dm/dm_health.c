/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_health.c
 * DesktopMate — 在座感知 + 久坐/喝水双提醒状态机（2026-08-15 新模块）
 *
 * 设计定稿：project_docs/specs/health-assistant-design.md
 *
 * 状态机概览（1s lv_timer，LVGL 主线程）：
 *   ABSENT ──prox≤30cm 持续5s──▶ SEATED（会话开始，双计时归零启动）
 *   SEATED ──prox>30cm 立即暂停倒计时、持续5s──▶ ABSENT（清零重来，弹窗收起）
 *   喝水：L1=45min(天热30min) → "先不喝" 15min 升档 → L2/L3
 *   久坐：L1=30min / L2=60min / L3=90min，同套路
 *   "我喝了"：喝水计数+1、计时重置；"我起来了"：起立+1、双计时重置
 *
 * 2026-08-16 P102：离开感应（prox>30cm）立即暂停倒计时（g_leaving），
 *   防抖 5s 后 health_session_end() 清零——修"离开后圈圈仍倒数"。
 *
 * 持久化 /data/dm_health.state（yaffs）：日期戳 + 今日喝水/起立/在座秒。
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dm_health.h"
#include "dm_health_cfg.h"   /* 参数/文案配置文件（/data/dm_health.cfg） */
#include "deskmate_ui.h"   /* g_weather（天热判据） */

/****************************************************************************
 * 参数（设计定稿 §3/§4）
 ****************************************************************************/

/* ── 默认参数（配置文件缺项时兜底；实际值优先取 /data/dm_health.cfg）── */
#define DM_HEALTH_STATE_FILE  "/data/dm_health.state"

#define PROX_SEAT_DEBOUNCE_S  5     /* 连续 5s 判定坐下 */
#define PROX_ABSENT_DEBOUNCE_S 30   /* 离开 30s 才清零（2026-08-27 用户拍板：
                                     * 短暂离开只暂停不重置——倒杯水回来接着计） */
#define STAND_MIN_S            3    /* Phase A：离开感应区 ≥3s 判定一次起立（防抖，
                                     * 只记一次；回来重置后可再记） */
#define WATER_L1_S            2700  /* 45min */
#define WATER_HOT_L1_S        1800  /* 30min（≥30℃ 天热） */
#define WATER_HOT_TEMP_C      30
#define WATER_ESCALATE_S      900   /* "先不喝"后 15min 升档 */
#define SIT_L1_S              1800  /* 30min */
#define SIT_L2_S              3600  /* 60min */
#define SIT_L3_S              5400  /* 90min */

/* 配置读取简写：key 缺项回退默认宏 */
#define CFG_I(key, def)  dm_health_cfg_get_int(key, def)

/* 传感器不可用时回退：视为始终在座（纯计时模式），避免功能全失 */
#define PROX_FALLBACK_SEATED  1

/****************************************************************************
 * 状态
 ****************************************************************************/

static lv_timer_t *g_health_timer;

static bool   g_seated;                 /* 当前是否在座 */
static int    g_seat_cnt;               /* 连续在座秒（防抖） */
static int    g_absent_cnt;             /* 连续离开秒（防抖） */
static long   g_seat_today_s;           /* 今日累计在座秒 */

/* 喝水：剩余秒 + 档位（0=未触发） */
static int    g_water_remain_s;
static int    g_water_total_s;
static int    g_water_level;
static int    g_water_count_today;

/* 久坐 */
static int    g_sit_remain_s;
static int    g_sit_total_s;
static int    g_sit_level;
static int    g_stand_count_today;

static int    g_state_day;              /* 今日 YYYYMMDD（跨天清零） */
static dm_health_cb_t g_cb;
static bool   g_prox_fallback;          /* 传感器不可用标记（日志一次） */
static bool   g_leaving;                /* P102：离开感应中（倒计时暂停，防抖满后清零） */
static bool   g_stand_recorded;         /* Phase A：本次离开已记起立（回来重置） */

/* ── Voice Director 统计（2026-08-28 Phase 6 Step 1，规格 VOICE_DIRECTOR_SPEC.md §六）── */
static long   g_away_s;                 /* 离开感应中累计离开秒 */
static long   g_last_away_s;            /* 最近一次离开时长（回来时锁定，供场景分类） */
static int    g_sit_cnt_today;          /* 今日坐下次数 */
static int    g_decline_cnt;            /* 连续拒绝提醒次数（"继续坐/先不喝"） */
static int    g_tease_cnt_today;        /* 今日吐槽已用（budget[CAT_TEASING] 持久化） */
static int    g_voice_cnt_today;        /* 今日语音总数（budget 持久化） */

/****************************************************************************
 * 持久化（yaffs /data，仿 dm_music.state）
 ****************************************************************************/

static void health_state_save(void)
{
    FILE *f = fopen(DM_HEALTH_STATE_FILE, "w");
    if (f)
    {
        /* 2026-08-28 Step 1：追加 4 字段（sit_cnt/tease/decline/voice），
         * 供 Voice Director budget 跨天/重启保持（规格 §6.3） */
        fprintf(f, "%d %d %d %ld %d %d %d %d\n", g_state_day,
                g_water_count_today, g_stand_count_today, g_seat_today_s,
                g_sit_cnt_today, g_tease_cnt_today, g_decline_cnt,
                g_voice_cnt_today);
        fclose(f);
    }
}

static void health_state_load(void)
{
    FILE *f = fopen(DM_HEALTH_STATE_FILE, "r");
    if (f)
    {
        int n = fscanf(f, "%d %d %d %ld", &g_state_day,
                       &g_water_count_today, &g_stand_count_today,
                       &g_seat_today_s);
        if (n != 4)
        {
            g_state_day = 0;    /* 损坏文件按新一天处理 */
        }
        else
        {
            /* 旧文件只有 4 字段 → EOF，新增字段=0，向后兼容 */
            int sit = 0, tease = 0, decline = 0, voice = 0;
            if (fscanf(f, "%d %d %d %d", &sit, &tease, &decline, &voice) != 4)
                sit = tease = decline = voice = 0;
            g_sit_cnt_today = sit;
            g_tease_cnt_today = tease;
            g_decline_cnt = decline;
            g_voice_cnt_today = voice;
        }
        fclose(f);
    }
}

/****************************************************************************
 * 内部：事件上报
 ****************************************************************************/

static void health_evt(int evt, int arg)
{
    if (g_cb)
        g_cb(evt, arg);
}

/* 今日日期戳 YYYYMMDD（本地时区，对时链路已闭环） */
static int today_stamp(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/****************************************************************************
 * 内部：会话生命周期
 ****************************************************************************/

/* 坐下：双计时按当前档位起点启动 */
static void health_session_start(void)
{
    int hot = (g_weather.cur_temp >= CFG_I("water_hot_temp_c", WATER_HOT_TEMP_C)) ? 1 : 0;

    g_seated = true;
    /* Step 1：今日坐下次数 +1；锁定最近离开时长供 Voice Director 场景分类 */
    g_sit_cnt_today++;
    g_last_away_s = g_away_s;
    g_away_s = 0;
    /* P141：离开去倒水回来接着计——上次离开时喝水计时仍进行中
     * （remain>0）则保留续计，不重置；仅首次/已触发弹窗（remain==0）
     * 才新起一轮。久坐每次坐下重新计时（连续坐姿中断，合理）。 */
    if (g_water_remain_s <= 0)
    {
        g_water_level = 0;
        g_water_total_s = hot ? CFG_I("water_hot_l1_s", WATER_HOT_L1_S)
                              : CFG_I("water_l1_s", WATER_L1_S);
        g_water_remain_s = g_water_total_s;
    }
    g_sit_level = 0;
    g_sit_total_s = CFG_I("sit_l1_s", SIT_L1_S);
    g_sit_remain_s = CFG_I("sit_l1_s", SIT_L1_S);

    LV_LOG_USER("[health] seated, water=%ds(%s), sit=%ds",
                g_water_total_s, hot ? "hot" : "normal", g_sit_total_s);
    health_evt(DM_HEALTH_EVT_PRESENCE, 1);
    health_evt(DM_HEALTH_EVT_RING, 0);
}

/* 离开：久坐清零重来（用户拍板）；喝水计时保留（P141 去倒水回来接着计） */
static void health_session_end(void)
{
    g_seated = false;
    g_leaving = false;
    g_sit_level = 0;
    g_sit_remain_s = 0;
    /* 不重置 g_water_*：离开只暂停喝水计时，回来 session_start 续计 */
    LV_LOG_USER("[health] left, session cleared");
    health_evt(DM_HEALTH_EVT_PRESENCE, 0);
    health_evt(DM_HEALTH_EVT_DISMISS, 0);   /* 收水窗 */
    health_evt(DM_HEALTH_EVT_DISMISS, 1);   /* 收坐窗 */
    health_evt(DM_HEALTH_EVT_RING, 0);
}

/****************************************************************************
 * 状态机（1s tick）
 ****************************************************************************/

static void health_tick_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 跨天：清零今日计数并保存 */
    int day = today_stamp();
    if (g_state_day == 0)
        g_state_day = day;
    else if (day != g_state_day)
    {
        g_state_day = day;
        g_water_count_today = 0;
        g_stand_count_today = 0;
        g_seat_today_s = 0;
        /* Step 1：Voice Director 计数跨天清零 */
        g_sit_cnt_today = 0;
        g_tease_cnt_today = 0;
        g_decline_cnt = 0;
        g_voice_cnt_today = 0;
        health_state_save();
        LV_LOG_USER("[health] new day %d, counters reset", day);
    }

    /* ── 在座感知（prox 判据 + 防抖）── */
    int prox = dm_prox_get_cm();
    if (prox < 0)
    {
        /* 2026-08-27：传感器不可用（LD2410B 未接入/串口失败）——
         * 不启动健康会话：不播欢迎语、不开始计时（用户要求：
         * 没有人体传感器就不要开始响应和计时）。等传感器恢复才响应。 */
        if (!g_prox_fallback)
        {
            g_prox_fallback = true;
            LV_LOG_WARN("[health] prox sensor unavailable, session idle (no greet/timer)");
        }
        /* 保持 g_seated 现状（开机为 false）：不 fallback seated，
         * 欢迎语（PRESENCE=1）与健康计时都不会启动 */
    }
    else
    {
        /* 2026-08-27：dm_prox_get_cm() 只回 0/99/-1 三态（阈值判断在
         * luncher_dm.c 内部按 ld2410_seat_cm 完成），此处 prox==0 即"有人
         * 且在座距离内"——原 prox<=prox_seat_cm(30) 死配置已删 */
        if (prox == 0)
        {
            g_absent_cnt = 0;
            g_leaving = false;   /* P102：回到感应区，恢复倒计时 */
            g_stand_recorded = false;   /* Phase A：回来 → 下次离开可再记起立 */
            if (!g_seated)
            {
                if (++g_seat_cnt >= CFG_I("prox_seat_debounce_s", PROX_SEAT_DEBOUNCE_S))
                {
                    g_seat_cnt = 0;
                    health_session_start();
                }
            }
        }
        else
        {
            g_seat_cnt = 0;
            if (g_seated)
            {
                /* P102：离开感应区 → 立即暂停倒计时（防抖满才清零） */
                g_leaving = true;
                g_away_s++;     /* Step 1：累计离开秒（回来时锁定给 Voice Director） */
                /* Phase A：离开 ≥STAND_MIN_S 判定一次起立（防抖；只记一次） */
                if (!g_stand_recorded && g_away_s >= STAND_MIN_S)
                {
                    g_stand_recorded = true;
                    g_stand_count_today++;
                    health_state_save();
                    LV_LOG_USER("[health] auto stand, count=%d", g_stand_count_today);
                }
                if (++g_absent_cnt >= CFG_I("prox_absent_debounce_s", PROX_ABSENT_DEBOUNCE_S))
                {
                    g_absent_cnt = 0;
                    health_session_end();
                }
            }
        }
    }

    /* 每秒 RING（含未在座）：双圈/健康卡/prox debug 实时刷新——
     * 2026-08-15：prox 调试显示不依赖坐下，RING 提前到 g_seated 判断前 */
    health_evt(DM_HEALTH_EVT_RING, 0);

    /* ── 在座时：累计 + 双提醒倒计时（P102：离开感应中 g_leaving 暂停）── */
    if (!g_seated || g_leaving)
        return;

    g_seat_today_s++;

    /* 喝水：未触发时倒计时；触发后等按钮（remain=0 停住） */
    if (g_water_remain_s > 0)
    {
        g_water_remain_s--;
        if (g_water_remain_s == 0)
        {
            g_water_level++;
            LV_LOG_USER("[health] water popup L%d", g_water_level);
            health_evt(DM_HEALTH_EVT_WATER_POP, g_water_level);
        }
    }

    /* 久坐 */
    if (g_sit_remain_s > 0)
    {
        g_sit_remain_s--;
        if (g_sit_remain_s == 0)
        {
            g_sit_level++;
            LV_LOG_USER("[health] sit popup L%d", g_sit_level);
            health_evt(DM_HEALTH_EVT_SIT_POP, g_sit_level);
        }
    }

    /* 每分钟落盘一次在座秒（计数变化即时存） */
    if (g_seat_today_s % 60 == 0)
        health_state_save();
}

/****************************************************************************
 * 公共 API
 ****************************************************************************/

void dm_health_init(void)
{
    if (g_health_timer)
        return;

    dm_health_cfg_load();   /* 2026-08-15 配置文件：/data/dm_health.cfg */
    health_state_load();
    LV_LOG_USER("[health] init: day=%d water=%d stand=%d seat=%lds",
                g_state_day, g_water_count_today, g_stand_count_today,
                g_seat_today_s);

    g_health_timer = lv_timer_create(health_tick_cb, 1000, NULL);
    if (g_health_timer) lv_timer_set_repeat_count(g_health_timer, -1);
}

void dm_health_set_cb(dm_health_cb_t cb)
{
    g_cb = cb;
}

bool dm_health_is_seated(void)
{
    return g_seated;
}

int dm_health_water_remain_s(void) { return g_water_remain_s; }
int dm_health_water_total_s(void)  { return g_water_total_s; }
int dm_health_water_level(void)    { return g_water_level; }
int dm_health_sit_remain_s(void)   { return g_sit_remain_s; }
int dm_health_sit_total_s(void)    { return g_sit_total_s; }
int dm_health_sit_level(void)      { return g_sit_level; }
int dm_health_water_count(void)    { return g_water_count_today; }
int dm_health_stand_count(void)    { return g_stand_count_today; }
long dm_health_seat_s(void)        { return g_seat_today_s; }

/* ── Voice Director 查询（2026-08-28 Step 1，规格 §6.2）── */
int  dm_health_sit_cnt_today(void)  { return g_sit_cnt_today; }
long dm_health_last_away_s(void)    { return g_last_away_s; }
int  dm_health_decline_cnt(void)    { return g_decline_cnt; }
int  dm_health_voice_cnt_today(void){ return g_voice_cnt_today; }

/* ── Voice Director budget 落盘（2026-08-28 Step 2）── */
void dm_health_note_voice(void)
{
    g_voice_cnt_today++;
    health_state_save();
}

void dm_health_note_tease(void)
{
    g_tease_cnt_today++;
    health_state_save();
}

/* ── 弹窗按钮 ── */

void dm_health_water_drank(void)
{
    int hot = (g_weather.cur_temp >= CFG_I("water_hot_temp_c", WATER_HOT_TEMP_C)) ? 1 : 0;

    g_water_count_today++;
    g_decline_cnt = 0;      /* Step 1：喝了=接受提醒，拒绝计数清零 */
    g_water_level = 0;
    g_water_total_s = hot ? CFG_I("water_hot_l1_s", WATER_HOT_L1_S)
                          : CFG_I("water_l1_s", WATER_L1_S);
    g_water_remain_s = g_water_total_s;
    health_state_save();
    LV_LOG_USER("[health] water drank, count=%d", g_water_count_today);
    /* 2026-09-01 P142：今日喝满 6 次 → 夸奖奖励事件（UI 播 豪华背景+夸奖语音） */
    if (g_water_count_today == 6)
    {
        LV_LOG_USER("[health] water milestone 6 reached");
        health_evt(DM_HEALTH_EVT_WATER_6, 0);
    }
    health_evt(DM_HEALTH_EVT_DISMISS, 0);
    health_evt(DM_HEALTH_EVT_RING, 0);
}

void dm_health_water_snooze(void)
{
    g_decline_cnt++;        /* Step 1：先不喝=拒绝一次 */
    if (g_water_level > 0 && g_water_remain_s == 0)
    {
        g_water_remain_s = CFG_I("water_escalate_s", WATER_ESCALATE_S);
        LV_LOG_USER("[health] water snooze, next L%d in %ds",
                    g_water_level + 1, g_water_remain_s);
    }
    health_evt(DM_HEALTH_EVT_DISMISS, 0);
    health_evt(DM_HEALTH_EVT_RING, 0);
}

void dm_health_sit_stood(void)
{
    int hot = (g_weather.cur_temp >= CFG_I("water_hot_temp_c", WATER_HOT_TEMP_C)) ? 1 : 0;

    g_stand_count_today++;
    g_decline_cnt = 0;      /* Step 1：起来了=接受提醒，拒绝计数清零 */
    g_sit_level = 0;
    g_sit_total_s = CFG_I("sit_l1_s", SIT_L1_S);
    g_sit_remain_s = CFG_I("sit_l1_s", SIT_L1_S);
    /* 起来=顺便喝水：喝水计时一并重置（设计定稿 §4） */
    g_water_level = 0;
    g_water_total_s = hot ? CFG_I("water_hot_l1_s", WATER_HOT_L1_S)
                          : CFG_I("water_l1_s", WATER_L1_S);
    g_water_remain_s = g_water_total_s;
    health_state_save();
    LV_LOG_USER("[health] sit stood, stand=%d", g_stand_count_today);
    health_evt(DM_HEALTH_EVT_DISMISS, 0);
    health_evt(DM_HEALTH_EVT_DISMISS, 1);
    health_evt(DM_HEALTH_EVT_RING, 0);
}

void dm_health_sit_snooze(void)
{
    g_decline_cnt++;        /* Step 1：继续坐=拒绝一次 */
    if (g_sit_level > 0 && g_sit_remain_s == 0)
    {
        if (g_sit_level == 1)
        {
            g_sit_total_s = CFG_I("sit_l2_s", SIT_L2_S);
            g_sit_remain_s = g_sit_total_s;
        }
        else
        {
            g_sit_total_s = CFG_I("sit_l3_s", SIT_L3_S);
            g_sit_remain_s = g_sit_total_s;
        }
        LV_LOG_USER("[health] sit snooze, next L%d in %ds",
                    g_sit_level + 1, g_sit_remain_s);
    }
    health_evt(DM_HEALTH_EVT_DISMISS, 1);
    health_evt(DM_HEALTH_EVT_RING, 0);
}
