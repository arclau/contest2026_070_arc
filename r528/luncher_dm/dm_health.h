/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_health.h
 * DesktopMate — 在座感知 + 久坐/喝水双提醒状态机（2026-08-15 新模块）
 *
 * 设计定稿：project_docs/specs/health-assistant-design.md
 *  - 接近传感器判定坐下/离开（防抖），离开清零重来
 *  - 喝水：L1 45min（天热 30min）→ 两键弹窗 → "先不喝" 15min 升级一档
 *  - 久坐：30/60/90min 三级，同套路
 *  - 数据持久化 /data/dm_health.state（yaffs，仿 dm_music.state）
 *
 * 线程模型：状态机跑在 LVGL 主线程（1s lv_timer），UI 事件经回调同步触发。
 * UI 层（ui_home.c standby）订阅 dm_health_set_cb 渲染双圈与弹窗。
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

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_HEALTH_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_HEALTH_H

#include <stdbool.h>

/* ── UI 事件（回调 evt/arg）── */
enum
{
    DM_HEALTH_EVT_PRESENCE = 0,  /* arg: 1=坐下 0=离开（会话开始/清零） */
    DM_HEALTH_EVT_WATER_POP,     /* arg: 档位 1..3，UI 弹喝水窗 */
    DM_HEALTH_EVT_SIT_POP,       /* arg: 档位 1..3，UI 弹久坐窗 */
    DM_HEALTH_EVT_DISMISS,       /* arg: 0=水窗 1=坐窗，UI 收起（离开/清零） */
    DM_HEALTH_EVT_RING,          /* arg: 0，每秒触发，UI 刷新双圈剩余秒 */
    DM_HEALTH_EVT_WATER_6,       /* arg: 0，今日喝水达 6 次（P142 夸奖奖励） */
    DM_HEALTH_EVT_MAX
};

typedef void (*dm_health_cb_t)(int evt, int arg);

/* 创建 1s 状态机 timer + 载入持久化计数（luncher_dm.c main 调用） */
void dm_health_init(void);

/* 注册 UI 回调（standby 创建时注册一次，常驻） */
void dm_health_set_cb(dm_health_cb_t cb);

/* ── 状态查询（UI 双圈 / 弹窗文案用）── */
bool dm_health_is_seated(void);
int  dm_health_water_remain_s(void);   /* 距下一喝水事件剩余秒 */
int  dm_health_water_total_s(void);    /* 当前档位总时长（进度分母） */
int  dm_health_water_level(void);      /* 0=未触发 1..3 */
int  dm_health_sit_remain_s(void);
int  dm_health_sit_total_s(void);
int  dm_health_sit_level(void);
int  dm_health_water_count(void);      /* 今日已喝水次数 */
int  dm_health_stand_count(void);      /* 今日起立次数 */
long dm_health_seat_s(void);           /* 今日累计在座秒（战报数据） */

/* ── Voice Director 查询（2026-08-28 Step 1）── */
int  dm_health_sit_cnt_today(void);    /* 今日坐下次数 */
long dm_health_last_away_s(void);      /* 最近一次离开时长秒（回来时锁定） */
int  dm_health_decline_cnt(void);      /* 连续拒绝提醒次数（"继续坐/先不喝"） */
int  dm_health_voice_cnt_today(void);  /* 今日语音总数（budget 持久化） */

/* ── Voice Director budget 落盘（2026-08-28 Step 2）── */
void dm_health_note_voice(void);       /* 今日语音总数 +1 并落盘（任何语音播放后） */
void dm_health_note_tease(void);       /* 今日吐槽计数 +1 并落盘（teasing 类播放后） */

/* ── 弹窗按钮（UI 回调里调用）── */
void dm_health_water_drank(void);      /* 我喝了：计数+1、计时重置 */
void dm_health_water_snooze(void);     /* 先不喝：15min 后升一档 */
void dm_health_sit_stood(void);        /* 我起来了：起立+1、双计时重置 */
void dm_health_sit_snooze(void);       /* 继续坐：升一档 */

/* luncher_dm.c 提供：接近传感器当前距离 cm，-1=传感器不可用 */
int dm_prox_get_cm(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_HEALTH_H */
