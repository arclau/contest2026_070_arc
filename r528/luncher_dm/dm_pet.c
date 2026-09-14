/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_pet.c
 * 电子宠物 — 门面层（Facade/装配）
 *
 * 架构（P201 重构）：
 *   pet_core.*  引擎：属性/状态机/行为/事件/投喂/持久化（零 lv_obj）
 *   pet_view.*  视图：全部 LVGL 对象/输入/动画/弹窗
 *   dm_pet.c    本文件：公共 API + 事件队列（线程边界）+ timer 装配 +
 *               锁屏进出（WAKE_UP/SLEEP）的 timer 暂停/恢复与停靠。
 *
 * 线程契约：
 *   dm_pet_on_event*() 可任意线程调用（语音 pet: 命令经 LVGL timer
 *   分发、今天实际全是主线程，但队列是跨线程安全网，保留）；
 *   队列出口 pump 只在 LVGL 主线程（poll timer）消费。
 *   dm_pet_get_data() 只读，可任意线程读；dm_pet_set_name() 仅内存+
 *   文件 IO，可语音线程直调；其余 API 限主线程。
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

#include <nuttx/config.h>
#include <stdio.h>

#include <lvgl/lvgl.h>
#include "dm_pet.h"
#include "pet_core.h"
#include "pet_view.h"

/****************************************************************************
 * Timer 周期（拥有者：本门面）
 ****************************************************************************/
#define PET_BEHAVIOR_TICK_MS    500   /* 行为选择节拍（core durations 以此为单位） */
#define PET_TIME_TICK_MS        (60 * 1000)  /* 属性时间推进：每分钟一次 */
#define PET_ANIM_TICK_MS        120   /* 动画节拍（view 内部再分频到 ~2.8fps） */
#define PET_POLL_TICK_MS        100   /* 事件队列消费（10Hz，主线程） */
#define PET_SAVE_INTERVAL_S     60

/****************************************************************************
 * 事件队列（线程边界：多生产者入队，主线程 poll 消费）
 *
 * 2 的幂容量 + 掩码取模；满时丢弃并计数（旧逻辑静默丢，有坑不自知）。
 ****************************************************************************/
#define EVT_QUEUE_SIZE 64
#define EVT_QUEUE_MASK (EVT_QUEUE_SIZE - 1)
typedef struct { dm_pet_event_t evt; int param; } pet_evt_entry_t;
static pet_evt_entry_t g_evt_queue[EVT_QUEUE_SIZE];
static volatile int g_evt_head = 0;
static volatile int g_evt_tail = 0;
static unsigned g_evt_dropped = 0;

/****************************************************************************
 * Timer 句柄（拥有者：本门面）
 ****************************************************************************/
static lv_timer_t *g_anim_timer;             /* 动画帧 timer */
static lv_timer_t *g_behavior_timer;         /* 行为选择 timer */
static lv_timer_t *g_save_timer;             /* 自动保存 timer */
static lv_timer_t *g_poll_timer;             /* 事件轮询 timer */
static lv_timer_t *g_time_timer;             /* 属性时间推进 timer */

/****************************************************************************
 * Timer 回调（路由到 core/view）
 ****************************************************************************/

static void pet_poll_cb(lv_timer_t *t)
{
    (void)t;
    while (g_evt_tail != g_evt_head)
        {
            pet_evt_entry_t e = g_evt_queue[g_evt_tail];
            g_evt_tail = (g_evt_tail + 1) & EVT_QUEUE_MASK;
            pet_core_process_event(e.evt, e.param);
        }
}

static void pet_behavior_cb(lv_timer_t *t)
{
    (void)t;
    pet_core_behavior_tick();
    pet_view_bubble_tick();
}

static void pet_anim_cb(lv_timer_t *t)
{
    (void)t;
    pet_view_anim_tick();
}

static void pet_save_cb(lv_timer_t *t)
{
    (void)t;
    pet_core_save();
}

/* 属性时间推进：进队列保序（与触摸/投喂等事件同序消费），不直调 core */
static void pet_time_cb(lv_timer_t *t)
{
    (void)t;
    dm_pet_on_event(PET_EVT_TIME_TICK);
}

static void timers_pause(bool pause)
{
    if (pause)
        {
            /* 进主屏：宠物不可见，停动画/行为/属性推进省 CPU；
             * poll 照常（事件照常消费），save 照常（定时落盘不断） */
            if (g_anim_timer)     lv_timer_pause(g_anim_timer);
            if (g_behavior_timer) lv_timer_pause(g_behavior_timer);
            if (g_time_timer)     lv_timer_pause(g_time_timer);
        }
    else
        {
            if (g_anim_timer)     lv_timer_resume(g_anim_timer);
            if (g_behavior_timer) lv_timer_resume(g_behavior_timer);
            if (g_time_timer)     lv_timer_resume(g_time_timer);
        }
}

/****************************************************************************
 * 对外 API
 ****************************************************************************/

int dm_pet_create(lv_obj_t *parent)
{
    if (!parent) return -1;

    pet_core_init();

    /* 总开关关着：拒绝创建（ui_home 已门控，此为纵深防御） */
    if (!pet_core_is_enabled())
        return -1;

    /* 幂等：已创建则只重新停靠/置顶，避免重复 timer 泄漏 */
    if (pet_view_reuse())
        {
            timers_pause(false);
            return 0;
        }

    pet_view_create(parent);

    /* 初始状态（view 建完后：行为气泡经 ops 能正常显示） */
    pet_core_enter_initial();
    pet_view_refresh_sprite();

    /* Timer 装配 */
    g_anim_timer       = lv_timer_create(pet_anim_cb, PET_ANIM_TICK_MS, NULL);
    g_behavior_timer   = lv_timer_create(pet_behavior_cb, PET_BEHAVIOR_TICK_MS, NULL);
    g_save_timer       = lv_timer_create(pet_save_cb, PET_SAVE_INTERVAL_S * 1000, NULL);
    g_poll_timer       = lv_timer_create(pet_poll_cb, PET_POLL_TICK_MS, NULL);
    g_time_timer       = lv_timer_create(pet_time_cb, PET_TIME_TICK_MS, NULL);

    const dm_pet_data_t *pd = pet_core_data();
    LV_LOG_USER("[pet] created: %s (type=%d) state=%s mood=%d/%d/%d",
                pd->name, pd->pet_type, pet_core_state_name(pet_core_state()),
                pd->attrs.hunger, pd->attrs.happiness, pd->attrs.energy);

    /* 长离线欢迎气泡（一次有效） */
    if (pet_core_consume_welcome())
        pet_view_show_bubble("想你啦~", PET_BUBBLE_DURATION_TICKS * 2);

    return 0;
}

void dm_pet_on_event(dm_pet_event_t evt)
{
    dm_pet_on_event_param(evt, 0);
}

void dm_pet_on_event_param(dm_pet_event_t evt, int param)
{
    if (evt <= PET_EVT_NONE || evt >= PET_EVT_COUNT) return;

    /* 锁屏进出：timer 暂停/恢复 + 停靠是门面职责，同步处理保即时性；
     * core 不再收这两个事件（纯引擎语义，无视之）。 */
    if (evt == PET_EVT_WAKE_UP)
        {
            timers_pause(true);
            return;
        }
    if (evt == PET_EVT_SLEEP)
        {
            timers_pause(false);
            pet_view_reuse();   /* 越界才复位，否则保留拖拽位置 */
            return;
        }

    int next = (g_evt_head + 1) & EVT_QUEUE_MASK;
    if (next == g_evt_tail)
        {
            /* 队列满：丢弃并计数（旧逻辑静默丢）。LVGL 主线程 10Hz 消费，
             * 正常 UW 下 64 深 Venus 绰绰有余；持续增长即上游风暴，加日志定位 */
            if ((++g_evt_dropped % 16) == 1)
                LV_LOG_USER("[pet] evt queue full, dropped=%u evt=%d", g_evt_dropped, (int)evt);
            return;
        }
    g_evt_queue[g_evt_head].evt = evt;
    g_evt_queue[g_evt_head].param = param;
    g_evt_head = next;
}

/* 拆 UI + 停 timer（deinit/关总开关共用；队列保留，事件照进） */
static void pet_teardown(void)
{
    if (g_anim_timer)     { lv_timer_del(g_anim_timer);     g_anim_timer = NULL; }
    if (g_behavior_timer) { lv_timer_del(g_behavior_timer); g_behavior_timer = NULL; }
    if (g_save_timer)     { lv_timer_del(g_save_timer);     g_save_timer = NULL; }
    if (g_poll_timer)     { lv_timer_del(g_poll_timer);     g_poll_timer = NULL; }
    if (g_time_timer)     { lv_timer_del(g_time_timer);     g_time_timer = NULL; }
    pet_view_destroy();
}

void dm_pet_deinit(void)
{
    pet_core_save();
    pet_teardown();
    LV_LOG_USER("[pet] deinit, state saved (dropped_evts=%u)", g_evt_dropped);
}

const dm_pet_data_t *dm_pet_get_data(void)
{
    return pet_core_data();
}

const char *dm_pet_state_name(dm_pet_state_t s)
{
    return pet_core_state_name(s);
}

dm_pet_life_stage_t dm_pet_get_life_stage(void)
{
    return pet_core_life_stage();
}

void dm_pet_set_name(const char *name)
{
    pet_core_set_name(name);
}

void dm_pet_set_enabled(bool enabled)
{
    bool old = pet_core_is_enabled();
    pet_core_set_enabled(enabled);
    /* 关→开：下次进锁屏 create（Settings 在主屏， standby 不可见，无需立即建）；
     * 开→关：立即拆 UI 停 timer */
    if (old && !enabled)
        pet_teardown();
}

bool dm_pet_is_enabled(void)
{
    return pet_core_is_enabled();
}

void dm_pet_set_hunger_alert(bool on)
{
    pet_core_set_hunger_alert(on);
}

bool dm_pet_get_hunger_alert(void)
{
    return pet_core_get_hunger_alert();
}

void dm_pet_reset(void)
{
    pet_core_reset();
    /* view 若在（锁屏挂着时极少走这里，防万一）：刷回 IDLE 首帧 */
    pet_view_refresh_sprite();
}

void dm_pet_set_roam_rect(const dm_pet_roam_t *rect)
{
    if (!rect)
        {
            pet_view_clear_roam();
            return;
        }
    pet_view_set_roam(rect->x1, rect->y1, rect->x2, rect->y2);
}

void dm_pet_clear_roam_rect(void)
{
    pet_view_clear_roam();
}
