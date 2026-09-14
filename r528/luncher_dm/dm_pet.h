/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_pet.h
 * 电子宠物系统 — Pet Engine
 *
 * 设计依据：project_docs/specs/PET_PRODUCT_SPEC.md
 * 架构（P201 重构，三层）：
 *   pet_core.*  引擎：属性/状态机/行为选择/事件消费/投喂/持久化（零 lv_obj）
 *   pet_view.*  视图：全部 LVGL 对象/输入/动画/弹窗（只读 core，不改状态）
 *   dm_pet.c    门面：公共 API + 事件队列（线程边界）+ timer 装配
 *   DM app 只调本头 API；帧表/行为池是内部实现，不得跨层引用。
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

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_PET_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_PET_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

/****************************************************************************
 * 宠物类型（P202：狗不做——固件空间/刷机时间不够；类型系统删除，
 * 只留猫。pet_type 存档字段保留（恒 0），老存档免迁移）
 ****************************************************************************/
typedef enum
{
    PET_TYPE_CAT = 0,       /* 猫：傲娇、慵懒、被动互动 */
    PET_TYPE_COUNT
} dm_pet_type_t;

/****************************************************************************
 * 宠物状态（行为系统的顶层分类）
 * 注意：状态集合与精灵素材严格 1:1 对齐（参见 PET_SPRITE_SPEC.md）——
 *   cat_sprites 共 12 组：11 个行为态 + 1 个独立 WALK 组，无 SLEEPY 素材，
 *   精力不足时直接进 SLEEP。每组原生 12 帧。
 ****************************************************************************/
typedef enum
{
    PET_STATE_IDLE = 0,     /* 默认待机：随机行为池 */
    PET_STATE_PLAY,         /* 玩耍中 */
    PET_STATE_EAT,          /* 吃东西中 */
    PET_STATE_SLEEP,        /* 睡觉 */
    PET_STATE_HAPPY,        /* 开心 */
    PET_STATE_SAD,          /* 不开心 */
    PET_STATE_HUNGRY,       /* 饥饿 */
    PET_STATE_CURIOUS,      /* 好奇 */
    PET_STATE_CELEBRATE,    /* 庆祝 */
    PET_STATE_TOUCH,        /* 被抚摸（短爆发） */
    PET_STATE_GROOM,        /* 舔毛（自我清洁） */
    PET_STATE_COUNT
} dm_pet_state_t;

/****************************************************************************
 * 宠物事件（外部触发）
 ****************************************************************************/
typedef enum
{
    PET_EVT_NONE = 0,
    /* 触摸互动 */
    PET_EVT_TAP_BODY,       /* 轻点身体 */
    PET_EVT_TAP_HEAD,       /* 轻点头部 */
    PET_EVT_LONG_PRESS,     /* 长按（抚摸） */
    PET_EVT_RAPID_TAP,      /* 连续快速点击（语音玩耍命令用；三连点手势已改直开喂食） */
    PET_EVT_DRAG,           /* 拖拽 */
    /* 投喂 */
    PET_EVT_FEED_FISH,      /* 投喂鱼（主食） */
    PET_EVT_FEED_MEAT,      /* 投喂肉（保留枚举，UI 未摆） */
    PET_EVT_FEED_VEGGIE,    /* 投喂蔬菜（保留枚举，UI 未摆） */
    PET_EVT_FEED_SNACK,     /* 投喂零食（成年奖励） */
    /* 环境 */
    PET_EVT_MUSIC_ON,       /* 音乐开始播放 */
    PET_EVT_MUSIC_OFF,      /* 音乐停止 */
    PET_EVT_WEATHER_CHANGE, /* 天气变化（param: weather type） */
    PET_EVT_VOICE_CMD,      /* 语音命令（param: command type） */
    PET_EVT_NAME_CHANGED,   /* 名字改变（需保存） */
    /* 生命周期 */
    PET_EVT_WAKE_UP,        /* 唤醒（锁屏→主屏，暂停动画） */
    PET_EVT_SLEEP,          /* 进入锁屏，恢复动画 */
    PET_EVT_TIME_TICK,      /* 每分钟时间推进（内部用） */
    /* P1（2026-09-11）环境/健康联动：全部复用现有状态素材，不新增帧 */
    PET_EVT_PRESENCE,       /* 有人坐下（雷达）→ 好奇看过去 */
    PET_EVT_WATER_GOAL,     /* 今日喝水达标 → 庆祝 */
    PET_EVT_SIT_ALERT,      /* 久坐提醒 → 担忧 */
    PET_EVT_AI_CHAT,        /* AI 对话进行中 → 开心 */
    PET_EVT_COUNT
    /* P202 删除：健康联动 5 事件（WATER_DRANK/STOOD_UP/SIT_REMIND/
     * WATER_GOAL/HEALTH_GOAL）——2026-09-11 P1 以 4 个精简事件回归 */
} dm_pet_event_t;

/****************************************************************************
 * 食物定义
 ****************************************************************************/
typedef enum
{
    PET_FOOD_FISH = 0,      /* 鱼：猫+高快乐 */
    PET_FOOD_MEAT,          /* 肉：狗+高快乐 */
    PET_FOOD_VEGGIE,        /* 蔬菜：通用 */
    PET_FOOD_SNACK,         /* 零食：高快乐，成年解锁 */
    PET_FOOD_COUNT
} dm_pet_food_t;

/****************************************************************************
 * 宠物属性（全部有实际表现）
 ****************************************************************************/
typedef struct
{
    int   hunger;           /* 0~100，低→没精神/看向食物/撒娇 */
    int   energy;           /* 0~100，低→打哈欠/动作慢/找地方睡 */
    int   happiness;        /* 0~100，低→动作减少/表情低落；高→主动跑跳 */
    int   affection;        /* 0~100，只增不减，代表羁绊值 */
    int   health;           /* 0~100，间接属性（hunger<20时衰减） */
    int   growth;           /* 0→∞，累计互动次数，驱动生命周期 */
} dm_pet_attrs_t;

/****************************************************************************
 * 宠物持久化数据
 ****************************************************************************/
typedef struct
{
    int   version;          /* 数据格式版本号（v2 起含 enabled/hunger_alert） */
    dm_pet_type_t pet_type; /* 恒 PET_TYPE_CAT（字段保留免迁移） */
    char  name[32];         /* 宠物名字 */
    dm_pet_attrs_t attrs;   /* 属性 */
    int   enabled;          /* 宠物总开关（0=锁屏不显示） */
    int   hunger_alert;     /* 饥饿红泡提醒开关 */
    long  last_interaction; /* 上次互动时间戳 */
    long  last_update;      /* 上次属性更新时间戳 */
    long  created_at;       /* 创建时间戳 */
    int   total_days;       /* 累计陪伴天数 */
} dm_pet_data_t;

/****************************************************************************
 * 触摸区域（细分触摸位置）
 ****************************************************************************/
typedef enum
{
    PET_TOUCH_BODY = 0,     /* 身体 */
    PET_TOUCH_HEAD,         /* 头部（上方1/3） */
    PET_TOUCH_COUNT
} dm_pet_touch_zone_t;

/****************************************************************************
 * 生命周期阶段
 ****************************************************************************/
typedef enum
{
    PET_LIFE_BABY = 0,      /* 幼年：Growth 0~99，体型小，好奇心强 */
    PET_LIFE_JUVENILE,      /* 成长：Growth 100~499，体型中等 */
    PET_LIFE_ADULT,         /* 成年：Growth 500+，体型完全，特殊行为解锁 */
    PET_LIFE_COUNT
} dm_pet_life_stage_t;

/****************************************************************************
 * 对外 API
 ****************************************************************************/

/* 创建宠物（挂到 parent 容器上，锁屏时可见）。
 * 首次运行用默认属性 + 名字“旺财” + 猫。
 * 后续运行从 /data/pet_state.json 恢复。
 * 幂等：重复调用只重停靠/置顶，不重复建 timer。
 * 返回 0=成功/-1=失败。 */
int dm_pet_create(lv_obj_t *parent);

/* 处理事件（可任意线程调：内部队列 + LVGL timer 主线程消费）。
 * WAKE_UP/SLEEP（锁屏进出）门面同步处理 timer 暂停/恢复与停靠。 */
void dm_pet_on_event(dm_pet_event_t evt);

/* 带参数的事件处理（天气类型、语音命令等） */
void dm_pet_on_event_param(dm_pet_event_t evt, int param);

/* 销毁宠物 UI + 保存状态 */
void dm_pet_deinit(void);

/* 获取当前宠物数据（只读） */
const dm_pet_data_t *dm_pet_get_data(void);

/* 获取当前状态名（调试用） */
const char *dm_pet_state_name(dm_pet_state_t state);

/* 获取生命周期阶段 */
dm_pet_life_stage_t dm_pet_get_life_stage(void);

/* 设置宠物名字并保存 */
void dm_pet_set_name(const char *name);

/* 总开关：关=锁屏不再创建/立即拆 UI；开=下次进锁屏恢复（Settings 用） */
void dm_pet_set_enabled(bool enabled);
bool dm_pet_is_enabled(void);

/* 饥饿红泡提醒开关（关了只留 HUNGRY 行为，不弹可点红泡） */
void dm_pet_set_hunger_alert(bool on);
bool dm_pet_get_hunger_alert(void);

/* 重新开始：名字/成长/属性全部回默认并落盘（Settings 二次确认后调） */
void dm_pet_reset(void);

/* 漫游区覆盖（屏幕坐标，布局拥有者如 ui_home 可注入“排除音乐/健康卡后”
 * 的可走区；不调则用 view 默认启发式。NULL = 恢复默认） */
typedef struct { int x1, y1, x2, y2; } dm_pet_roam_t;
void dm_pet_set_roam_rect(const dm_pet_roam_t *rect);
void dm_pet_clear_roam_rect(void);

/* 宠物帧集（每状态动画帧，pet_frame_table.c 定义；原生 12 帧，与 cat_sprites 1:1） */
#define PET_FRAMES_PER_STATE 12
typedef struct { const lv_image_dsc_t *frames[PET_FRAMES_PER_STATE]; } pet_frame_set_t;

/* 走路/移动帧组（12帧独立动画，与状态帧组分开；pet_frame_table.c 定义） */
#define PET_WALK_FRAMES 12
typedef struct { const lv_image_dsc_t *frames[PET_WALK_FRAMES]; } pet_walk_set_t;
extern const pet_walk_set_t g_cat_walk_frames;

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_PET_H */
