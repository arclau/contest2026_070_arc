/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_voice.h
 * Voice Director — 语音决策层（Phase 6 Step 1，2026-08-28）
 *
 * 设计/规格：
 *   project_docs/specs/VOICE_INTERACTION_DESIGN.md（§四/§五 决策规则）
 *   project_docs/specs/VOICE_DIRECTOR_SPEC.md（§二 接口 / §三 voice_ctx_t）
 *
 * 铁律：本模块只做决策层——不修改 dm_tone_play / dm_tone_worker /
 * dm_audio_fg / PTT 打断；输出 wav 名交调用方 dm_tone_play 播放。
 * 决策核心（voice_director_decide）为纯函数，无 LVGL 依赖，可单测。
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

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_VOICE_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_VOICE_H

#include <stdbool.h>

/* ── 输出动作（VOICE_DIRECTOR_SPEC.md §二）── */
typedef enum
{
    VOICE_NONE = 0,      /* 静默（也是有效输出） */
    VOICE_UI_ONLY,       /* 只 UI（欢迎回来卡），不播语音 */
    VOICE_PARTICLE,      /* 粒子（调味料，0.3~1.5s） */
    VOICE_SHORT,         /* 自然短句（1~3s） */
    VOICE_FULL           /* 情境化完整句（3~6s） */
} voice_action_t;

/* ── 分类 budget（§5.1.2：greeting/health_reminder/teasing/caring/system_response）── */
typedef enum
{
    CAT_GREETING = 0,
    CAT_HEALTH_REMINDER,
    CAT_TEASING,
    CAT_CARING,
    CAT_SYSTEM_RESPONSE,
    CAT_LAST
} voice_category_t;

/* ── 决策输出 ── */
typedef struct
{
    voice_action_t   action;    /* 输出动作 */
    voice_category_t category;  /* 输出分类 */
    const char      *wav;       /* 选中 wav 名（不含路径）；NULL=静默/UI */
} voice_decision_t;

/* ── 决策上下文（§三 定稿）──
 * 数据来源：dm_health（sit_s/today_sit_cnt/water/stand/decline/away_s）、
 * dm_health.state（持久化计数）、dm_ai（since_ai_s/ai_busy）、
 * music 模块（music_playing）、time()（hour）。收集逻辑在 dm_voice.c 内部。 */
typedef struct
{
    bool  seated;
    int   sit_s;            /* 连续坐姿秒 */
    int   today_sit_cnt;    /* 今日坐下次数 */
    int   water_cnt;        /* 今日喝水 */
    int   stand_cnt;        /* 今日起立 */
    int   away_s;           /* 本次离开秒数（离开起计时，回来时读取） */
    int   since_voice_s;    /* 距最近一次语音（含提醒） */
    int   since_ai_s;       /* 距最近 AI 对话 */
    int   decline_cnt;      /* 连续拒绝提醒次数（"继续坐/先不喝"） */
    int   budget[CAT_LAST]; /* 各分类今日已用计数 */
    int   recent_ids[8];    /* 最近播放 wav 轻量 id（环形，防重复） */
    bool  music_playing;
    bool  ai_busy;
    int   hour;             /* 当前小时 0~23 */
} voice_ctx_t;

/* ── 事件入口（Step 3 接线；Step 1 仅声明）── */
void voice_director_on_seated(void);          /* 坐下事件（dm_health seated 判定成功） */
void voice_director_on_reminder(int kind, int level); /* 提醒弹窗（0=水 1=坐），保持现链路+抢占判断 */
void voice_director_on_ai_interaction(void);  /* AI 对话开始时间戳（dm_ai 调用） */
void voice_director_on_music_changed(bool playing);  /* 音乐状态变化（music 模块调用） */

/* ── 纯决策（Step 2 实现，可单测）── */
voice_decision_t voice_director_decide(const voice_ctx_t *ctx);
void voice_director_note_played(voice_category_t cat, const char *wav); /* 播放后回写历史/预算 */

/* ── 生命周期（Step 2 实现；dm_health_init 旁调用）── */
void voice_director_init(void);

/* ── 调试入口（voice_sim 测试命令专用）──
 * 绕过 classify_scene 的全局状态依赖，直接指定 scene + 合成 ctx，
 * 走完整决策流水线。scene 编号参见 classify_scene()。hour=-1 用场景默认。 */
voice_decision_t voice_director_test_scene(int scene, int hour,
                                           int away_s, int today_sit_cnt,
                                           bool music_on, int decline_cnt);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_VOICE_H */
