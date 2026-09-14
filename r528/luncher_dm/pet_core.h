/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/pet_core.h
 * 电子宠物 — 引擎层（Engine/Model）
 *
 * 分层（P201 重构）：
 *   pet_core  引擎：属性/状态机/行为选择（含帧窗）/事件消费/投喂/持久化。
 *             零 lv_obj 操作（仅 lv_tick_get 计时 + LV_LOG），可被 view/facade 调用。
 *   pet_view  视图：全部 LVGL 对象/timer/input/弹窗/动画渲染（见 pet_view.h）。
 *   dm_pet.c  门面：公共 API + 事件队列（线程边界）+ timer 装配。
 *
 * 依赖方向（无环）：view → core → dm_pet.h；facade → core + view；
 * core → view 只经 pet_view_ops_t 回调表（view 在 create 时注册，destroy 时注销）。
 *
 * 线程契约：core 所有函数只在 LVGL 主线程调用（经门面事件队列消费），
 * 除了 pet_core_data()（只读，可任意线程读）与 pet_core_save()
 * （纯文件 IO，set_name 从语音线程直调安全）。
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_PET_CORE_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_PET_CORE_H

#include <lvgl/lvgl.h>   /* lv_tick_get（冷却计时） */
#include <stdbool.h>
#include "dm_pet.h"

/****************************************************************************
 * 行为定义：State → Behavior → 帧窗 → Duration → Next
 *
 * 12 帧环按行为语义切窗（afirst..alast 含端点）：同一状态的不同行为
 * 播不同窗，行为才有视觉差异（P201 前行为层有名无形，全态播整环）。
 * afirst < 0 表示整幅 0..PET_FRAMES_PER_STATE-1（EAT/PLAY 等无池状态）。
 ****************************************************************************/
typedef struct
{
    const char *name;       /* 行为名（调试用） */
    int         weight;     /* 随机权重 */
    int         duration;   /* 持续 ticks（1 tick = 500ms 行为节拍） */
    const char *bubble;     /* 气泡文字（可选） */
    int         afirst;     /* 动画窗首帧（-1 = 整幅） */
    int         alast;      /* 动画窗尾帧（含） */
} pet_behavior_t;

/****************************************************************************
 * 视图回调（core → view 单向通知；view 负责注册/注销）
 ****************************************************************************/
typedef struct
{
    /* 气泡（ticks = 显示节拍数，由 view 倒计时隐藏） */
    void (*show_bubble)(const char *txt, int ticks);
    void (*show_hunger_alert)(void);
    void (*hide_hunger_alert)(void);
} pet_view_ops_t;

/* 气泡默认显示节拍（engine 请求时长，view 倒计时隐藏；
 * 1 tick = 500ms 行为节拍，12 = 6秒） */
#define PET_BUBBLE_DURATION_TICKS 12

/* pet_frame_table.c 生成的帧表（view 经 core 只读访问） */
extern const pet_frame_set_t g_cat_frames[PET_STATE_COUNT];
extern const pet_walk_set_t  g_cat_walk_frames;

/****************************************************************************
 * 生命周期（门面调用，主线程）
 ****************************************************************************/

/* 加载存档/建默认（幂等，可重复调） */
void pet_core_init(void);

/* 注册/注销视图回调（view create/destroy 时调；core 内全部判空） */
void pet_core_register_view(const pet_view_ops_t *ops);
void pet_core_unregister_view(void);

/* 首次进入：按属性/时间算初始态 + 选行为（view 建完后由门面调一次） */
void pet_core_enter_initial(void);

/* 长离线欢迎气泡消费（一次有效，门面在 create 后查） */
bool pet_core_consume_welcome(void);

/****************************************************************************
 * 只读访问（view 渲染/门面 API 用；data() 可任意线程读）
 ****************************************************************************/
dm_pet_state_t       pet_core_state(void);
const dm_pet_data_t *pet_core_data(void);
dm_pet_life_stage_t  pet_core_life_stage(void);
const char          *pet_core_state_name(dm_pet_state_t s);

/* 状态层面是否可走（view 再叠加 walk_pause/dragging 得最终“走路中”） */
bool pet_core_can_walk(void);

/* 当前动画窗 [first..last]（行为窗或整幅；view 检测变化即重播） */
void pet_core_anim_range(int *first, int *last);

/* 当前宠物类型对应帧集（狗资源就绪前固定猫帧） */
const pet_frame_set_t *pet_core_frames(void);

/****************************************************************************
 * 节拍（门面 timer 驱动，主线程）
 ****************************************************************************/
void pet_core_behavior_tick(void);  /* 500ms：倒计时/自动回 IDLE/行为选择 */
void pet_core_time_tick(void);      /* 1min：属性衰减 + 状态转移 */
void pet_core_save(void);           /* 立即落盘（60s timer/关键事件） */

/****************************************************************************
 * 事件消费（门面队列出口，主线程）
 ****************************************************************************/
void pet_core_process_event(dm_pet_event_t evt, int param);

/* 改名（可语音线程直调：仅内存+文件 IO，无 LVGL） */
void pet_core_set_name(const char *name);

/* 总开关/饥饿提醒（存档字段，立即落盘；拆 UI 由门面做） */
void pet_core_set_enabled(bool enabled);
bool pet_core_is_enabled(void);
void pet_core_set_hunger_alert(bool on);
bool pet_core_get_hunger_alert(void);

/* 重新开始：回默认并落盘（状态机复位 IDLE，门面负责刷 view） */
void pet_core_reset(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_PET_CORE_H */
