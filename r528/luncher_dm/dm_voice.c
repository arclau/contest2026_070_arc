/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_voice.c
 * Voice Director — 语音决策引擎（Phase 6 Step 2，2026-08-28）
 *
 * 规格：project_docs/specs/VOICE_DIRECTOR_SPEC.md（§五 决策流程伪代码）
 * 设计：project_docs/specs/VOICE_INTERACTION_DESIGN.md（§四 场景 / §五 决策规则）
 *
 * 职责：坐下事件 → 场景分类 → 闭嘴检查 → 类别选择 → 加权随机选 wav → 输出。
 * 铁律：只做决策层；播放走 dm_tone_play（现有链路零改动）；
 *       wav 缺失时 dm_tone_worker 打印跳过，安全（Step 5 合成后自动生效）。
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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include <lvgl/lvgl.h>   /* LV_LOG_USER */

#include "dm_voice.h"
#include "dm_health.h"
#include "dm_health_cfg.h"
#include "dm_pet.h"        /* P1：AI 对话 → 宠物开心 */
#include "deskmate_ui.h"   /* dm_tone_play */

#define CFG_I(key, def) dm_health_cfg_get_int(key, def)

/****************************************************************************
 * 内部状态
 ****************************************************************************/

static time_t      g_last_voice_ts;     /* 最近一次语音时间戳（含提醒） */
static time_t      g_ai_ts;             /* 最近一次 AI 对话时间戳 */
static bool        g_music_playing;     /* 音乐播放中 */
static int         g_budget[CAT_LAST];  /* 今日各分类预算计数 */
static const char *g_recent_ids[8];     /* 最近播放 wav 名（环形，防重复） */
static int         g_recent_pos;
static bool        g_boot_greet_done;   /* 开机首次欢迎已播（场景 1） */
static int         g_period;            /* 时段 id（-1=未初始化） */
static int         g_period_cnt;        /* 当前时段已播次数 */

/****************************************************************************
 * cfg 工具
 ****************************************************************************/

/****************************************************************************
 * 候选池（最终音频文件名；缺失时 dm_tone_play 静默跳过，Step 5 合成后生效）
 ****************************************************************************/

typedef struct
{
    const char *name;
    int         weight;
} voice_cand_t;

/* 候选池 = 「res/tones/ 实际已合成的 wav」。
 * - 时段问候（greet_morning/noon/afternoon/evening/night）不在此池：由
 *   greet_for_hour() 严格按当前小时选，做到"不同时段说不同的话"；
 * - CAT_CARING 只留通用关怀 care_01（时段问候缺失/已播过时的回退）；
 * - CAT_TEASING 只留中性回来短句 back_01/02（P206 砍毒舌，tease_01 不用）。 */
static const voice_cand_t g_pool[CAT_LAST][8] = {
    [CAT_GREETING] = {        /* 场景 1/2：欢迎（开机首次 + 当天首次） */
        { "greet_first", 10 },  /* 开机首次（现有） */
        { "greet_hello", 8 },   /* 当天首次（现有） */
    },
    [CAT_HEALTH_REMINDER] = { /* 提醒直通（health_popup_show 现链路），不参与选择 */
        { NULL, 0 },
    },
    [CAT_TEASING] = {         /* 中性回来短句 */
        { "back_01", 6 }, { "back_02", 5 },
    },
    [CAT_CARING] = {          /* 通用关怀（时段问候的回退） */
        { "care_01", 5 },
    },
    [CAT_SYSTEM_RESPONSE] = { /* 粒子（调味料，低频） */
        { "tiny_01", 3 },
    },
};

static const int g_pool_cnt[CAT_LAST] = { 2, 0, 2, 1, 1 };

/****************************************************************************
 * 上下文收集（规格 §三）
 ****************************************************************************/

static voice_ctx_t collect_ctx(void)
{
    voice_ctx_t c;
    time_t now = time(NULL);
    struct tm tm;

    memset(&c, 0, sizeof(c));
    /* 2026-08-31 时段 bug 修复：系统 time 为 UTC（settimeofday 设 UTC），
     * UI 层统一 gmtime + 手动 +8 东八区（ui_home.c clock_update_cb /
     * luncher_dm.c 同款）。此前用 localtime_r 直接取 tm_hour 得到 UTC 小时
     * （本地 16 点 = UTC 8 点）→ 被当 morning 播早安。与 UI 层对齐。 */
    {
        struct tm *utc = gmtime(&now);
        if (utc == NULL)
        {
            tm.tm_hour = -1;
        }
        else
        {
            tm = *utc;
            tm.tm_hour += 8;
            mktime(&tm);   /* 规范化跨天（与 ui_home.c 同款） */
        }
    }

    c.seated        = dm_health_is_seated();
    c.sit_s         = (int)dm_health_seat_s();      /* 今日累计≈连续坐姿（离开清零） */
    c.today_sit_cnt = dm_health_sit_cnt_today();
    c.water_cnt     = dm_health_water_count();
    c.stand_cnt     = dm_health_stand_count();
    c.away_s        = (int)dm_health_last_away_s();
    c.decline_cnt   = dm_health_decline_cnt();
    time_t dv = g_last_voice_ts ? (now - g_last_voice_ts) : INT_MAX;
    time_t da = g_ai_ts ? (now - g_ai_ts) : INT_MAX;
    c.since_voice_s = (dv > INT_MAX) ? INT_MAX : (int)dv;
    c.since_ai_s    = (da > INT_MAX) ? INT_MAX : (int)da;
    c.music_playing = g_music_playing;
    /* Step 5 修正：未对时（time 仍为 1970 时钟）→ hour=-1，时段场景退化。
     * 2026-01-01 00:00 UTC = 1767225600，早于它说明 NTP/HTTP Date 未生效。 */
    c.hour          = (now >= 1767225600) ? tm.tm_hour : -1;
    memcpy(c.budget, g_budget, sizeof(g_budget));
    memcpy(c.recent_ids, g_recent_ids, sizeof(g_recent_ids));
    return c;
}

/****************************************************************************
 * 场景分类（规格 §5.3：12 场景）
 ****************************************************************************/

static int classify_scene(const voice_ctx_t *c)
{
    if (c->since_ai_s <= CFG_I("recent_ai_interaction_cooldown", 300)) return 10;
    if (c->music_playing && CFG_I("music_sit_silent", 1))               return 11;
    if (!c->seated)                                                     return 0;
    if (!g_boot_greet_done)                                             return 1;  /* 开机首次 */
    if (c->today_sit_cnt == 1)                                          return 2;  /* 当天首次 */
    if (c->away_s <= CFG_I("away_short_max_s", 300))                    return 4;  /* 短暂离开 */
    if (c->away_s >= CFG_I("away_long_min_s", 1800))                    return 5;  /* 长离开 */
    if (c->decline_cnt > 0 && c->away_s <= CFG_I("decline_revisit_s", 900))
                                                                        return 12; /* 拒后回来 */
    if (c->sit_s >= CFG_I("sit_long_for_revisit_s", 3600))              return 6;  /* 久坐后回来 */
    if (c->today_sit_cnt >= CFG_I("sit_cnt_quiet_th", 6))               return 7;  /* 多次趋静 */
    if (c->hour >= 0)   /* Step 5：hour<0=未对时，时段不可信，跳过时段场景 */
    {
        if (c->hour >= 23 || c->hour < 5)                               return 9;  /* 深夜 */
        if (c->hour >= 18)                                              return 8;  /* 晚上 */
    }
    return 3;                                                           /* 普通回来 */
}

/****************************************************************************
 * budget / 时段限播
 ****************************************************************************/

static bool budget_full(voice_category_t cat)
{
    int limit;
    switch (cat)
    {
    case CAT_GREETING:        limit = CFG_I("budget_greeting", 3); break;
    case CAT_HEALTH_REMINDER: return false;   /* 0=按触发不设上限；L3 不可拦截 */
    case CAT_TEASING:         limit = CFG_I("budget_teasing", 4); break;
    case CAT_CARING:          limit = CFG_I("budget_caring", 4); break;
    case CAT_SYSTEM_RESPONSE: limit = CFG_I("budget_system_response", 5); break;
    default:                  return false;
    }
    return g_budget[cat] >= limit;
}

static void update_period(int hour)
{
    int p = (hour >= 5 && hour < 11) ? 0 :
            (hour >= 11 && hour < 14) ? 1 :
            (hour >= 14 && hour < 18) ? 2 :
            (hour >= 18 && hour < 23) ? 3 : 4;
    if (p != g_period)
    {
        g_period = p;
        g_period_cnt = 0;
    }
}

static bool period_cnt_full(void)
{
    return g_period_cnt >= CFG_I("greet_max_per_period", 2);
}

/****************************************************************************
 * 闭嘴检查（规格 §5.1 顺序链）
 ****************************************************************************/

static bool pass_silence_checks(const voice_ctx_t *c, int scene)
{
    /* 2026-09-05：回来场景（3~6/8/9/12）用独立短冷却 back_cooldown_s，
     * 不再被全局 voice_cooldown_s=600 一刀切封死——"离开再回来有回应"；
     * 问候（1/2）与多次坐趋静（7）保持全局冷却（防话痨）。 */
    int cooldown;
    switch (scene)
    {
    case 3: case 4: case 5: case 6: case 8: case 9: case 12:
        cooldown = CFG_I("back_cooldown_s", 180);
        break;
    default:
        cooldown = CFG_I("voice_cooldown_s", 600);
        break;
    }
    if (c->since_voice_s <= cooldown) return false;                        /* #3 */
    if (scene == 10 || scene == 11) return false;                          /* #1/#2 场景层已拦 */
    if (scene <= 2 && budget_full(CAT_GREETING)) return false;             /* #4 欢迎配额 */
    /* 2026-09-11：删掉"吐槽配额满 → 全静默"——配额只该封调侃本身
     * （pick/select 内按类归零），不应连关心/问候一起封死。时段限播
     * period_cnt_full() 已下沉到 select_return()，只约束时段问候。 */
    return true;
}

/****************************************************************************
 * 加权随机选 wav（规格 §5.5：剔除最近播放 + 权重）
 ****************************************************************************/

static const char *weighted_select(voice_category_t cat, const voice_ctx_t *c)
{
    int cand[8], cnt = 0, total = 0, r, i;

    (void)c;
    for (i = 0; i < g_pool_cnt[cat]; i++)
    {
        int recent = 0;
        for (int r2 = 0; r2 < 8; r2++)
        {
            if (g_recent_ids[r2] &&
                strcmp(g_recent_ids[r2], g_pool[cat][i].name) == 0)
            {
                recent = 1;
                break;
            }
        }
        if (!recent)
            cand[cnt++] = i;
    }
    if (cnt == 0)
        return NULL;                       /* 全在最近历史 → UI_ONLY */

    for (i = 0; i < cnt; i++)
        total += g_pool[cat][cand[i]].weight;
    r = rand() % total;
    for (i = 0; i < cnt; i++)
    {
        r -= g_pool[cat][cand[i]].weight;
        if (r < 0)
            return g_pool[cat][cand[i]].name;
    }
    return g_pool[cat][cand[cnt - 1]].name;
}

/****************************************************************************
 * 回来场景选择（严格当前时段问候 → care → back → 粒子）
 ****************************************************************************/

/* 时段 → 问候 wav：
 *   05:00~10:59 morning / 11:00~13:59 noon / 14:00~17:59 afternoon /
 *   18:00~22:59 evening / 23:00~04:59 night；未对时（hour<0）返回 NULL。 */
static const char *greet_for_hour(int hour)
{
    if (hour < 0)                 return NULL;
    if (hour >= 5  && hour < 11)  return "greet_morning";
    if (hour >= 11 && hour < 14)  return "greet_noon";
    if (hour >= 14 && hour < 18)  return "greet_afternoon";
    if (hour >= 18 && hour < 23)  return "greet_evening";
    return "greet_night";
}

static bool is_recent(const char *name)
{
    if (!name)
        return true;
    for (int i = 0; i < 8; i++)
        if (g_recent_ids[i] && strcmp(g_recent_ids[i], name) == 0)
            return true;
    return false;
}

/* 回来场景：以当前时段问候为准（严格不串时段）；若该句已播过 / 时段未知 /
 * 该类配额满，则依次回退 care_01 → back 中性短句 → tiny 粒子，全不可用才静默。
 * 返回选中 wav，并回填用于 budget 记账的分类。 */
static const char *select_return(const voice_ctx_t *c, voice_category_t *cat_out)
{
    static const voice_category_t fallback[3] = {
        CAT_CARING, CAT_TEASING, CAT_SYSTEM_RESPONSE
    };
    const char *g = greet_for_hour(c->hour);
    int i;

    /* 时段问候不占 caring 日配额：其话痨上限由 back_cooldown + 每时段限播
     * (greet_max_per_period) 控制，避免"一天只问候 4 次"把核心价值砍没。 */
    if (g && !period_cnt_full() && !is_recent(g))
    {
        g_period_cnt++;              /* 时段问候计入"每时段限播" */
        *cat_out = CAT_CARING;
        return g;
    }
    for (i = 0; i < 3; i++)
    {
        voice_category_t cat = fallback[i];
        const char *w;
        if (budget_full(cat))
            continue;
        w = weighted_select(cat, c);
        if (w)
        {
            *cat_out = cat;
            return w;
        }
    }
    return NULL;
}

static voice_action_t action_of(voice_category_t cat)
{
    switch (cat)
    {
    case CAT_GREETING:        return VOICE_FULL;
    case CAT_HEALTH_REMINDER: return VOICE_FULL;
    case CAT_TEASING:         return VOICE_SHORT;
    case CAT_CARING:          return VOICE_SHORT;
    case CAT_SYSTEM_RESPONSE: return VOICE_PARTICLE;
    default:                  return VOICE_NONE;
    }
}

/****************************************************************************
 * 对外接口
 ****************************************************************************/

voice_decision_t voice_director_decide(const voice_ctx_t *ctx)
{
    voice_decision_t d;
    int scene;
    const char *wav;

    memset(&d, 0, sizeof(d));
    scene = classify_scene(ctx);
    update_period(ctx->hour);

    if (!pass_silence_checks(ctx, scene))
    {
        LV_LOG_USER("[voice] scene=%d hour=%d -> silent (cooldown/budget)",
                    scene, ctx->hour);
        return d;                          /* VOICE_NONE：静默也是人格 */
    }

    if (scene <= 2)
    {
        /* 开机首次 / 当天首次欢迎（P235：固定"早呀"曾在 16 点播出事故；
         * hour 有效且时段问候可播时优先时段，否则回退 greet_first/hello） */
        const char *gh = greet_for_hour(ctx->hour);
        wav = select_return(ctx, &d.category);
        if (wav != gh)   /* 回退链（care/back/tiny）不适合首次欢迎场合 */
        {
            d.category = CAT_GREETING;
            wav = weighted_select(CAT_GREETING, ctx);
        }
        if (scene == 1 && wav)
            g_boot_greet_done = true;  /* 时段问候走 CAT_CARING，不经 note_played 置位 */
    }
    else if (scene == 7)
    {
        /* 当天多次坐：趋静，只允许粒子 */
        d.category = CAT_SYSTEM_RESPONSE;
        wav = weighted_select(CAT_SYSTEM_RESPONSE, ctx);
    }
    else
    {
        /* 回来场景：严格当前时段问候 + 回退（g_period_cnt 内部计数） */
        wav = select_return(ctx, &d.category);
    }

    if (!wav)
    {
        d.action = VOICE_UI_ONLY;          /* 候选耗尽 → 只 UI */
        LV_LOG_USER("[voice] scene=%d hour=%d -> ui_only (candidates exhausted)",
                    scene, ctx->hour);
        return d;
    }

    d.wav = wav;
    d.action = action_of(d.category);
    LV_LOG_USER("[voice] scene=%d hour=%d cat=%d -> %s",
                scene, ctx->hour, d.category, wav);
    return d;
}

void voice_director_note_played(voice_category_t cat, const char *wav)
{
    g_last_voice_ts = time(NULL);
    if (cat < CAT_LAST)
        g_budget[cat]++;
    dm_health_note_voice();
    if (cat == CAT_TEASING)
        dm_health_note_tease();
    if (wav)
    {
        g_recent_ids[g_recent_pos % 8] = wav;
        g_recent_pos++;
    }
    if (cat == CAT_GREETING)
        g_boot_greet_done = true;
}

/* ── 事件入口（Step 3 接线；本 Step 实现逻辑）── */

void voice_director_on_seated(void)
{
    voice_ctx_t ctx = collect_ctx();
    voice_decision_t d = voice_director_decide(&ctx);

    if (d.action >= VOICE_PARTICLE && d.wav)
    {
        /* 2026-09-01 P142：开机欢迎语（greeting 类）前置"心之容器"背景
         * 音乐，避免突然人声惊吓；其余分类（粒子/关心等）保持原样。 */
        if (d.category == CAT_GREETING)
            dm_tone_play_seq("bg_reward_heart", d.wav);
        else
            dm_tone_play(d.wav);
        voice_director_note_played(d.category, d.wav);
    }
    /* VOICE_UI_ONLY / VOICE_NONE：欢迎回来卡由 ui_home（Step 4）处理 */
}

void voice_director_on_reminder(int kind, int level)
{
    /* 提醒弹窗保持现链路（health_popup_show 直接 dm_tone_play popup_*），
     * 此处只记录历史/预算（L2+ 音乐抢占已由 dm_tone_play 内 dm_audio_fg_acquire 覆盖） */
    voice_director_note_played(CAT_HEALTH_REMINDER, NULL);
    (void)kind;
    (void)level;
}

void voice_director_on_ai_interaction(void)
{
    g_ai_ts = time(NULL);
    dm_pet_on_event(PET_EVT_AI_CHAT);   /* P1：AI 对话进行中 → 猫开心 */
}

void voice_director_on_music_changed(bool playing)
{
    g_music_playing = playing;
}

void voice_director_init(void)
{
    srand((unsigned)time(NULL));
    g_last_voice_ts = 0;
    g_ai_ts = 0;
    g_music_playing = false;
    g_boot_greet_done = false;
    g_period = -1;
    g_period_cnt = 0;
    memset(g_budget, 0, sizeof(g_budget));
    memset(g_recent_ids, 0, sizeof(g_recent_ids));
    /* budget 明细（greeting/caring/system）重启清零可接受（规格 §6.3）；
     * tease 持久化计数由 dm_health 维护，重启后从 0 重新累计 */
    LV_LOG_USER("[voice] director init");
}

/****************************************************************************
 * 调试入口（voice_sim 测试命令专用）
 *
 * 绕过 classify_scene（依赖 g_boot_greet_done 等全局状态），
 * 直接指定 scene + 构造合成 ctx，走完整决策流水线：
 *   classify_scene(合成ctx) → pass_silence_checks → (select_return |
 *   weighted_select) → action_of
 *
 * scene 编号参见 classify_scene()：1=开机首次 2=当天首次 3=普通回来
 * 4=短离开 5=长离开 6=久坐后回来 7=多次趋静 8=晚上 9=深夜
 * 10=AI交互中 11=音乐中 12=拒后回来
 *
 * 注意：classify_scene 仍会执行（它读 ctx 字段），但 voice_sim 通过
 * 构造合成 ctx 让 classify_scene 产出指定 scene——覆盖所有分支。
 ****************************************************************************/

voice_decision_t voice_director_test_scene(int scene, int hour,
                                           int away_s, int today_sit_cnt,
                                           bool music_on, int decline_cnt)
{
    voice_ctx_t c;

    memset(&c, 0, sizeof(c));
    c.seated        = true;
    c.hour          = hour;
    c.since_voice_s = 9999;          /* 绕过冷却 */
    c.since_ai_s    = 9999;          /* 绕过 AI 交互冷却 */
    c.away_s        = away_s;
    c.today_sit_cnt = today_sit_cnt;
    c.decline_cnt   = decline_cnt;
    c.music_playing = music_on;
    c.sit_s         = 0;
    c.water_cnt     = 0;
    c.stand_cnt     = 0;
    c.ai_busy       = false;
    memcpy(c.budget, g_budget, sizeof(g_budget));
    memcpy(c.recent_ids, g_recent_ids, sizeof(g_recent_ids));

    /* 场景特殊构造：让 classify_scene 产出指定 scene */
    switch (scene)
    {
    case 1:  /* 开机首次：g_boot_greet_done=false + seated */
        g_boot_greet_done = false;
        c.today_sit_cnt = 0;
        break;
    case 2:  /* 当天首次：today_sit_cnt=1 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 1;
        break;
    case 3:  /* 普通回来：away_s 在短~长之间 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.away_s = 600;            /* >300 短, <1800 长 */
        break;
    case 4:  /* 短暂离开：away_s ≤300 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.away_s = 120;
        break;
    case 5:  /* 长离开：away_s ≥1800 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.away_s = 3600;
        break;
    case 6:  /* 久坐后回来：sit_s ≥3600 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.sit_s = 7200;
        c.away_s = 600;
        break;
    case 7:  /* 多次趋静：today_sit_cnt ≥6 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 7;
        c.away_s = 600;
        break;
    case 8:  /* 晚上：hour ≥18 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.away_s = 600;
        c.hour = 20;
        break;
    case 9:  /* 深夜：hour ≥23 或 <5 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.away_s = 600;
        c.hour = 0;
        break;
    case 10: /* AI 交互中：since_ai_s ≤300 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.since_ai_s = 60;
        break;
    case 11: /* 音乐中 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.music_playing = true;
        break;
    case 12: /* 拒后回来：decline_cnt >0 && away_s ≤900 */
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.decline_cnt = 1;
        c.away_s = 300;
        break;
    default:
        g_boot_greet_done = true;
        c.today_sit_cnt = 2;
        c.away_s = 600;
        break;
    }

    /* hour 覆盖（用户指定时优先） */
    if (hour >= 0 && hour <= 23)
        c.hour = hour;

    return voice_director_decide(&c);
}
