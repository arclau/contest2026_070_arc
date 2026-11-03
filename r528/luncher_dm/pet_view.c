/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/pet_view.c
 * 电子宠物 — 视图层实现（全部 LVGL 对象/输入/动画/弹窗）
 *
 * 数据只读经 pet_core.h；输入经 dm_pet_on_event() 进门面队列；
 * 引擎通知经 pet_view_ops_t 回调进来（本文件实现 + 注册）。
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lvgl/lvgl.h>
#include "dm_pet.h"
#include "deskmate_ui.h"
#include "pet_core.h"
#include "pet_view.h"
#include "pet_renderer.h"

/****************************************************************************
 * 配置常量（呈现层）
 ****************************************************************************/

#define PET_SIZE             DM(107)  /* 320px authored canvas -> about 256px display */
#define PET_BUBBLE_FONT      FONT_BODY   /* Montserrat 不含中文→气泡用项目中文字体 */

/* 动画节拍（门面 timer 周期，数值放这里备查） */
#define PET_ANIM_TICK_MS        120   /* 动画帧推进（帧率由切帧分频定，见 anim_tick） */

/* 触摸检测 */
#define PET_FEED_TAP_WINDOW_MS  800   /* 相邻短点击的最大间隔（含边界） */
#define PET_FEED_TAP_COUNT      2     /* 双击开喂食，后续连击保持入口 */

/* 交互胶囊（喂鱼/逗猫）。尺寸按 24px 正文字体定，不按 DM_SCALE 放大：
 * 原来 DM(80)×DM(38)=192×91，两个字撑在 192×91 的按钮里，整条 464×119，
 * 比 256px 的猫还宽 1.8 倍，所以看着过大。 */
#define PET_MENU_BTN_W       DM(42)   /* 100px：两字约 48px，左右各留 26px */
#define PET_MENU_BTN_H       DM(26)   /*  62px：24px 字高的 2.6 倍 */
#define PET_MENU_PAD_H       DM(10)
#define PET_MENU_PAD_V       DM(5)
#define PET_MENU_PAD_COL     DM(8)
#define PET_MENU_SHADOW      DM(9)
#define PET_LONG_PRESS_MS       600   /* 长按阈值（抚摸，无菜单） */
#define PET_RELAX_MS           2200
#define PET_PLAY_SPEED_PX_S    DM(75)
#define PET_PLAY_IDLE_MS       3000u

/* 自由位置：拖拽松手或退出逗猫后，猫留在原地 5 分钟不回漫游区；
 * 到点自己走回去（不是滑回去）。 */
#define PET_FREE_HOLD_MS       (5 * 60 * 1000)
#define PET_RETURN_SPEED_PX_S  (PET_PLAY_SPEED_PX_S / 2)  /* 回位比漫游快、比逗猫慢 */
#define PET_DRAG_THRESHOLD 20         /* 适度防抖：>20px才算拖拽，避免轻点误判 */

/* Physical display pixels: deliberately independent of DM_SCALE. */
#define PET_ROAM_MIN_PX          40
#define PET_ROAM_MAX_PX          70
#define PET_WALK_SPEED_PX_S      30
#define PET_IDLE_MIN_MS        4000
#define PET_IDLE_MAX_MS        8000

/****************************************************************************
 * 视图内部状态
 ****************************************************************************/

/* LVGL 对象 */
static lv_obj_t   *g_cont;                   /* 容器 */
static lv_obj_t   *g_body;                   /* 身体（lv_image 宠物） */
static lv_obj_t   *g_bubble;                 /* 气泡容器 */
static lv_obj_t   *g_bubble_lbl;             /* 气泡文字 */

/* 气泡倒计时（500ms 节拍） */
static int         g_bubble_ticks = 0;

/* Each visible view has its own clock and pins one shared-cache entry. */
static pet_renderer_t g_renderer;
static uint32_t g_idle_deadline;
static uint32_t g_seen_instance;
static uint32_t g_walk_started_ms;
static int g_walk_origin_x, g_walk_origin_y;
static int g_walk_target_x, g_walk_target_y;
static bool g_walk_started;
static bool g_return_walking;      /* 当前这段走路是"回漫游区"，完成后恢复漫游 */
static uint32_t g_return_start_ms; /* 回位开始时刻：超时兜底，防止卡在"回位中" */
#define PET_RETURN_TIMEOUT_MS  90000u
static uint32_t g_free_until_ms;   /* 自由位置截止；0 = 无 */
static bool g_returning;
static int g_return_from_x, g_return_from_y;
static int g_return_to_x, g_return_to_y;

/* 位置/朝向/漫游 */
static lv_coord_t g_pet_x = 0;
static lv_coord_t g_pet_y = 0;
static bool       g_face_right = false;   /* 朝向：true=面朝右（默认面朝左） */



/* 漫游区覆盖（ui_home 等布局拥有者可注入；无覆盖用默认启发式） */
static bool       g_roam_override = false;
static int        g_roam_x1, g_roam_y1, g_roam_x2, g_roam_y2;

/* 触摸检测 */
static uint32_t    g_last_tap_ms = 0;
static int         g_tap_count = 0;
static uint32_t    g_press_start_ms = 0;
static bool        g_press_active = false;
static bool        g_long_press_sent = false;
static bool        g_dragging = false;
static dm_pet_touch_zone_t g_press_zone;
static bool g_relax_sent;
static lv_obj_t *g_play_layer, *g_play_target;
static bool g_play_pointer, g_play_can_pounce;
static lv_point_t g_play_point, g_play_anchor;
static uint32_t g_play_last_ms, g_pounce_started_ms;
static uint32_t g_play_last_touch_ms;
static bool g_pouncing;
static int g_pounce_from_x, g_pounce_from_y, g_pounce_to_x, g_pounce_to_y;
static lv_coord_t  g_drag_start_x = 0;
static lv_coord_t  g_drag_start_y = 0;

/* 弹窗 */
static lv_obj_t *g_feed_popup = NULL;   /* 喂食弹窗（鱼/零食） */
static lv_obj_t *g_hunger_alert = NULL;
static lv_obj_t *g_hunger_alert_lbl = NULL;
static lv_timer_t *g_feed_close_timer;
static lv_timer_t *g_hunger_close_timer;
static lv_obj_t *g_feed_satiety_lbl;
static lv_obj_t *g_feed_satiety_bar;
static lv_obj_t *g_satiety;
static lv_obj_t *g_satiety_lbl;
static lv_obj_t *g_satiety_bar;
static lv_timer_t *g_satiety_timer;

/****************************************************************************
 * 前向声明
 ****************************************************************************/

static void pet_show_feed_popup(void);
static void pet_close_feed_popup(void);
static void pet_park_bottom_right(void);
static void pet_relocate_on_lock(void);
static void pet_consume_click_cb(lv_event_t *e);
static int rand_range(int lo, int hi);
static void pet_align_satiety(void);
static void pet_align_popups(void);
static void pet_hide_satiety(void);
static void pet_play_stop(bool tired);
static void pet_play_input(lv_event_t *e);
static int pet_play_tick(uint32_t now);

/****************************************************************************
 * 工具
 ****************************************************************************/

static int rand_range(int lo, int hi)
{
    if (lo >= hi) return lo;
    return lo + rand() % (hi - lo + 1);
}

/* Mirrored 320px frames use the same bounded cache as normal frames. */

/****************************************************************************
 * 漫游区
 *
 * 默认启发式：右下留白区（X 右半避锁屏音乐控件，Y 固定健康栏上方）。
 * 布局归 ui_home 所有，启发式只是 fallback；home 可随时 set_roam 覆盖。
 ****************************************************************************/

static void pet_roam_rect(int *x1, int *y1, int *x2, int *y2)
{
    if (g_roam_override)
        {
            *x1 = g_roam_x1; *y1 = g_roam_y1;
            *x2 = g_roam_x2; *y2 = g_roam_y2;
            return;
        }
    lv_coord_t sw = lv_display_get_horizontal_resolution(NULL);
    lv_coord_t sh = lv_display_get_vertical_resolution(NULL);
    /* 左边界避让锁屏音乐控件区（sm_row 居中于 sw/2，DM(160)≈384px 起步隔离） */
    *x1 = sw / 2 + DM(160);
    *x2 = sw - PET_SIZE / 2 - DM(20);
    /* Y 固定（健康栏上方，不上下移） */
    *y1 = *y2 = sh - PET_SIZE / 2 - DM(80);
}

void pet_view_set_roam(int x1, int y1, int x2, int y2)
{
    g_roam_x1 = x1; g_roam_y1 = y1;
    g_roam_x2 = x2; g_roam_y2 = y2;
    g_roam_override = true;
}

void pet_view_clear_roam(void)
{
    g_roam_override = false;
}

/* 自由位置：把猫留在原地一段时间，期间不拉回漫游区。
 * 触发点只有"用户搬动过它"——拖拽松手、退出逗猫。 */
static void pet_free_hold(void)
{
    g_free_until_ms = lv_tick_get() + PET_FREE_HOLD_MS;
}

static bool pet_free_active(void)
{
    return g_free_until_ms && (int32_t)(lv_tick_get() - g_free_until_ms) < 0;
}

/* ── 散步模式（2026-11-01 P331 语音"走五分钟"）────────────────────
 * 与 free_hold 的区别：free_hold 是"待在原地别回来"，散步是"满屏走，
 * 到点再回来"。两者互斥；散步结束把 g_free_until_ms 置 1（非 0 且已过期），
 * 交给下面既有的"自由位置到点 → 走回漫游区"分支，不重复写回位逻辑。 */
static uint32_t g_stroll_until_ms;

void pet_view_stroll_start(int seconds)
{
    if (seconds <= 0) seconds = 300;
    if (seconds > 3600) seconds = 3600;
    g_stroll_until_ms = lv_tick_get() + (uint32_t)seconds * 1000u;
    g_free_until_ms = 0;
    g_idle_deadline = 0;                 /* 立刻迈第一步，别先坐 8 秒 */
    LV_LOG_USER("[pet] stroll start %ds", seconds);
}

void pet_view_stroll_stop(void)
{
    if (!g_stroll_until_ms) return;
    g_stroll_until_ms = 0;
    g_free_until_ms = 1;                 /* 非 0 且已过期 → 回位分支接手 */
    LV_LOG_USER("[pet] stroll stop");
}

bool pet_view_stroll_active(void)
{
    return g_stroll_until_ms != 0;
}

static void pet_stop_walk(void)
{
    pet_action_t action = pet_core_action();
    if (action == PET_ACTION_WALK || action == PET_ACTION_TURN)
        pet_core_finish_walk(pet_core_animation_id());
    g_walk_started = false;
}

static void pet_return_exec_cb(void *var, lv_anim_value_t value)
{
    g_pet_x = g_return_from_x +
        (g_return_to_x - g_return_from_x) * value / 1024;
    g_pet_y = g_return_from_y +
        (g_return_to_y - g_return_from_y) * value / 1024;
    lv_obj_t *cont = var;
    lv_obj_set_pos(cont, g_pet_x - lv_obj_get_width(cont) / 2,
                   g_pet_y - lv_obj_get_height(cont) / 2);
    pet_align_popups();
}

static void pet_return_done_cb(lv_anim_t *a)
{
    (void)a;
    g_returning = false;
    g_idle_deadline = lv_tick_get() + rand_range(3000, 6000);
}

static void pet_cancel_return(void)
{
    if (g_cont) lv_anim_delete(g_cont, pet_return_exec_cb);
    g_returning = false;
}

static bool pet_return_to_roam(void)
{
    if (!g_cont) return false;
    if (g_returning) return true;
    int x1, y1, x2, y2;
    pet_roam_rect(&x1, &y1, &x2, &y2);
    int x = g_pet_x;
    if (x < x1) x = x1;
    if (x > x2) x = x2;
    if (x == g_pet_x && g_pet_y == y2) return false;

    /* 统一回位入口，避免松手后或漫游门控把宠物留在基准线外。 */
    pet_stop_walk();
    g_return_from_x = g_pet_x; g_return_from_y = g_pet_y;
    g_return_to_x = x; g_return_to_y = y2;
    g_returning = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_cont);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, pet_return_exec_cb);
    lv_anim_set_completed_cb(&a, pet_return_done_cb);
    lv_anim_start(&a);
    return true;
}

static void pet_park_bottom_right(void)
{
    if (!g_cont) return;
    int x1, y1, x2, y2;
    pet_roam_rect(&x1, &y1, &x2, &y2);
    /* 与 anim 位移的 zone 对齐：右下留白区底边，面朝左 */
    g_pet_x = x2;
    g_pet_y = y2;
    g_face_right = false;
    g_idle_deadline=lv_tick_get()+rand_range(3000,6000);
    lv_obj_set_pos(g_cont, g_pet_x - lv_obj_get_width(g_cont) / 2,
                   g_pet_y - lv_obj_get_height(g_cont) / 2);
    lv_obj_move_foreground(g_cont);
    pet_align_popups();
}

/* 进入锁屏/复用创建：保留散步区内拖拽位置，越界则复位右下角 */
static void pet_relocate_on_lock(void)
{
    if (!g_cont) return;
    g_free_until_ms = 0;      /* 锁屏重排：自由位置与回位走路都作废 */
    g_return_walking = false;
    int x1, y1, x2, y2;
    pet_roam_rect(&x1, &y1, &x2, &y2);
    if (g_pet_x < x1 || g_pet_x > x2 ||
        g_pet_y < y1 || g_pet_y > y2)
        {
            pet_park_bottom_right();
            return;
        }
    g_idle_deadline=lv_tick_get()+rand_range(3000,6000);
    lv_obj_set_pos(g_cont, g_pet_x - lv_obj_get_width(g_cont) / 2,
                   g_pet_y - lv_obj_get_height(g_cont) / 2);
    lv_obj_move_foreground(g_cont);
    pet_align_popups();
}

/* The engine owns turn -> walk -> settle. The view applies displacement only
 * while the actual selected action is WALK; it never substitutes walk images.
 */
void pet_view_anim_tick(void)
{
    if (!g_body || !g_cont) return;
    uint32_t now=lv_tick_get();
    /* 回位超时兜底：走路被打断（睡着/换动作）时不能让漫游永久停摆。 */
    if (g_return_walking && (int32_t)(now-g_return_start_ms) > (int32_t)PET_RETURN_TIMEOUT_MS)
        g_return_walking=false;
    int hop=pet_play_tick(now);
    uint32_t instance=pet_core_animation_id();
    pet_action_t action=pet_core_action();
    if (g_seen_instance!=instance) {
        g_seen_instance=instance;
        if (action==PET_ACTION_IDLE_SIT)
            g_idle_deadline=now+rand_range(PET_IDLE_MIN_MS,PET_IDLE_MAX_MS);
    }
    if (action==PET_ACTION_WALK && !g_walk_started) {
        g_walk_started=true;g_walk_started_ms=now;
        g_walk_origin_x=g_pet_x;g_walk_origin_y=g_pet_y;
    } else if (action!=PET_ACTION_WALK) g_walk_started=false;
    bool completed=pet_renderer_step(&g_renderer,action,instance,
        pet_core_touch_held(),!g_face_right,now); /* Native new walk faces right. */
    if (completed) pet_core_animation_done(instance);
    if (g_renderer.resource_error)
        pet_view_show_bubble("宠物素材未就绪",2);
    if (!g_play_layer && !g_dragging && !g_press_active && !g_returning && action==PET_ACTION_WALK) {
        /* About one or two 1.2s walk cycles, then the engine selects SETTLE.
         * 位移同时插值 x/y：漫游时目标 y 等于起点 y（纯水平，行为不变），
         * 回位时走的是二维直线。 */
        int speed = g_return_walking ? PET_RETURN_SPEED_PX_S : PET_WALK_SPEED_PX_S;
        uint32_t distance=(uint64_t)(now-g_walk_started_ms)*speed/1000u;
        int dxs=g_walk_target_x-g_walk_origin_x, dys=g_walk_target_y-g_walk_origin_y;
        int span=abs(dxs)>abs(dys) ? abs(dxs) : abs(dys);
        if (span<1) span=1;
        int traveled=(int)distance;
        if (traveled>=span) {
            g_pet_x=g_walk_target_x;g_pet_y=g_walk_target_y;
            g_return_walking=false;
            pet_core_finish_walk(instance);
        } else {
            g_pet_x=g_walk_origin_x+dxs*traveled/span;
            g_pet_y=g_walk_origin_y+dys*traveled/span;
        }
    }
    /* 自由位置到点：自己走回漫游区（不是滑回去）。中途被摸/拖会取消。 */
    if (g_free_until_ms && !pet_free_active() && !g_return_walking &&
        !g_play_layer && !g_dragging && !g_press_active && !g_returning &&
        action==PET_ACTION_IDLE_SIT && pet_core_can_walk()) {
        int x1,y1,x2,y2;pet_roam_rect(&x1,&y1,&x2,&y2);
        int tx=g_pet_x;
        if (tx<x1) tx=x1;
        if (tx>x2) tx=x2;
        g_free_until_ms=0;
        if (tx!=g_pet_x || g_pet_y!=y2) {
            g_walk_target_x=tx;g_walk_target_y=y2;
            g_face_right = tx>g_pet_x;
            g_return_walking=true;
            g_return_start_ms=now;
            pet_core_begin_walk();
        }
    }

    /* 散步到点 → 收工回位（把 free_until 置 1，让下面的回位分支接手） */
    if (g_stroll_until_ms && (int32_t)(now - g_stroll_until_ms) >= 0)
        pet_view_stroll_stop();

    /* 散步中：满屏随机选点，2D 走（位移插值本来就支持 x/y 同时变）。
     * 到点后坐 4~8 秒再走下一段，节奏由 g_idle_deadline 控制。 */
    if (g_stroll_until_ms && g_cont && !g_play_layer && !g_dragging &&
        !g_press_active && !g_returning && !g_feed_popup &&
        action==PET_ACTION_IDLE_SIT && pet_core_can_walk() &&
        (int32_t)(now-g_idle_deadline)>=0) {
        lv_obj_t *scr = lv_screen_active();
        int W = lv_obj_get_width(scr), H = lv_obj_get_height(scr);
        int hw = lv_obj_get_width(g_cont)/2, hh = lv_obj_get_height(g_cont)/2;
        int sx1 = hw+8, sx2 = W-hw-8;
        int sy1 = H/4, sy2 = H-hh-8;          /* 上 1/4 让开状态栏 */
        if (sx2>sx1 && sy2>sy1) {
            int tx = rand_range(sx1,sx2), ty = rand_range(sy1,sy2);
            /* 别原地打转：目标太近就换到另一侧 */
            if (abs(tx-g_pet_x)<60 && abs(ty-g_pet_y)<60)
                tx = (tx < (sx1+sx2)/2) ? sx2 : sx1;
            g_walk_target_x=tx; g_walk_target_y=ty;
            g_face_right = tx > g_pet_x;
            pet_core_begin_walk();
        }
        g_idle_deadline=now+rand_range(PET_IDLE_MIN_MS,PET_IDLE_MAX_MS);
    }

    if (!g_play_layer && !g_dragging && !g_press_active && !g_returning && !g_feed_popup &&
        !g_free_until_ms && !g_stroll_until_ms && !g_return_walking &&
        /* Completion may select IDLE_SIT above. Observe that new instance on
         * the next tick and start its rest before considering another trip. */
        action==PET_ACTION_IDLE_SIT && pet_core_can_walk() &&
        (int32_t)(now-g_idle_deadline)>=0) {
        int x1,y1,x2,y2;pet_roam_rect(&x1,&y1,&x2,&y2);
        if (pet_return_to_roam()) return;
        if (x2-x1>=PET_ROAM_MIN_PX) {
            int room=g_face_right ? x2-g_pet_x : g_pet_x-x1;
            /* Keep direction until the boundary requires a turn. Never clip
             * a requested trip into a tiny back-and-forth shuffle. */
            if (room<PET_ROAM_MIN_PX) {
                g_face_right=!g_face_right;
                room=g_face_right ? x2-g_pet_x : g_pet_x-x1;
            }
            if (room>=PET_ROAM_MIN_PX) {
                int distance=rand_range(PET_ROAM_MIN_PX,
                    room<PET_ROAM_MAX_PX ? room : PET_ROAM_MAX_PX);
                g_walk_target_x=g_pet_x+(g_face_right ? distance : -distance);
                g_walk_target_y=g_pet_y;
                pet_core_begin_walk();
            }
        }
        g_idle_deadline=now+rand_range(PET_IDLE_MIN_MS,PET_IDLE_MAX_MS);
    }
    lv_obj_set_pos(g_cont,g_pet_x-lv_obj_get_width(g_cont)/2,
        g_pet_y-lv_obj_get_height(g_cont)/2-hop);
    pet_align_popups();
}

/* 气泡隐藏倒计时（门面 500ms timer 驱动） */
void pet_view_bubble_tick(void)
{
    if (g_bubble_ticks > 0)
        {
            g_bubble_ticks--;
            if (g_bubble_ticks == 0 && g_bubble)
                lv_obj_add_flag(g_bubble, LV_OBJ_FLAG_HIDDEN);
        }
}

/****************************************************************************
 * 引擎通知实现（ops：core → view）
 ****************************************************************************/

void pet_view_show_bubble(const char *txt, int ticks)
{
    if (!txt || !g_bubble || !g_bubble_lbl) return;
    lv_label_set_text(g_bubble_lbl, txt);
    lv_obj_clear_flag(g_bubble, LV_OBJ_FLAG_HIDDEN);
    g_bubble_ticks = ticks;
}

static void view_show_hunger_alert_impl(void);
static void view_hide_hunger_alert_impl(void);
static void view_show_satiety_impl(void);

static const pet_view_ops_t g_ops =
{
    .show_bubble       = pet_view_show_bubble,
    .show_hunger_alert = view_show_hunger_alert_impl,
    .hide_hunger_alert = view_hide_hunger_alert_impl,
    .show_satiety      = view_show_satiety_impl,
};

/****************************************************************************
 * 触摸回调
 ****************************************************************************/

/* Play input exists only after the explicit button: normal lock-screen taps
 * keep their wake behavior. Releasing the pointer starts the idle timeout. */
static void pet_play_stop(bool tired)
{
    g_play_pointer=false;g_pouncing=false;
    if (g_play_layer) lv_obj_delete(g_play_layer);
    g_play_layer=NULL;g_play_target=NULL;
    dm_pet_on_input(PET_EVT_PLAY_END,tired);
    g_walk_started=false;
    g_idle_deadline=lv_tick_get()+PET_IDLE_MAX_MS;
    pet_free_hold();   /* 逗猫停在哪儿就待哪儿，5 分钟后自己走回漫游区 */
    if (g_cont) {
        lv_obj_set_pos(g_cont,g_pet_x-lv_obj_get_width(g_cont)/2,
                       g_pet_y-lv_obj_get_height(g_cont)/2);
        pet_align_popups();
    }
}

static void pet_play_input(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    lv_event_code_t code=lv_event_get_code(e);
    g_play_last_touch_ms=lv_tick_get();
    if (code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST) {
        g_play_pointer=false;
        if (g_play_target) lv_obj_add_flag(g_play_target,LV_OBJ_FLAG_HIDDEN);
        dm_pet_on_input(PET_EVT_PLAY_PHASE,PET_PLAY_LOOK);
        return;
    }
    lv_indev_t *indev=lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev,&g_play_point);
    if (!g_play_pointer) { g_play_can_pounce=true;g_play_anchor=g_play_point; }
    g_play_pointer=true;
    int dx=g_play_point.x-g_play_anchor.x,dy=g_play_point.y-g_play_anchor.y;
    if (dx*dx+dy*dy>=DM(8)*DM(8)) g_play_can_pounce=true;
    if (g_play_target) {
        lv_obj_t *parent=lv_obj_get_parent(g_play_target);
        lv_area_t area;lv_obj_get_content_coords(parent,&area);
        lv_obj_set_pos(g_play_target,g_play_point.x-area.x1-DM(5),
                       g_play_point.y-area.y1-DM(5));
        lv_obj_remove_flag(g_play_target,LV_OBJ_FLAG_HIDDEN);
    }
}

static void pet_play_start_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    pet_close_feed_popup();
    dm_pet_on_input(PET_EVT_PLAY_BEGIN,0);
    if (!pet_core_playing()) return;
    pet_cancel_return();g_walk_started=false;
    g_play_pointer=false;g_pouncing=false;g_tap_count=0;
    g_play_last_ms=g_play_last_touch_ms=lv_tick_get();
    g_play_layer=lv_obj_create(lv_obj_get_parent(g_cont));
    lv_obj_remove_style_all(g_play_layer);
    lv_obj_set_size(g_play_layer,LV_PCT(100),LV_PCT(100));
    lv_obj_add_flag(g_play_layer,LV_OBJ_FLAG_FLOATING|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_play_layer,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_play_layer,pet_play_input,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(g_play_layer,pet_play_input,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(g_play_layer,pet_play_input,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(g_play_layer,pet_play_input,LV_EVENT_PRESS_LOST,NULL);
    lv_obj_add_event_cb(g_play_layer,pet_consume_click_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *hint=lv_label_create(g_play_layer);
    lv_label_set_text(hint,"按住空白处移动，松手片刻自动结束");
    lv_obj_set_style_text_font(hint,FONT_BODY,0);
    lv_obj_set_style_text_color(hint,lv_color_hex(COL_TEXT),0);
    lv_obj_align(hint,LV_ALIGN_TOP_MID,0,DM(50));
    g_play_target=lv_obj_create(g_play_layer);
    lv_obj_remove_style_all(g_play_target);
    lv_obj_set_size(g_play_target,DM(10),DM(10));
    lv_obj_set_style_bg_color(g_play_target,lv_color_hex(COL_BLUE),0);
    lv_obj_set_style_bg_opa(g_play_target,LV_OPA_60,0);
    lv_obj_set_style_radius(g_play_target,LV_RADIUS_CIRCLE,0);
    lv_obj_remove_flag(g_play_target,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_play_target,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_cont);
}

static int pet_play_tick(uint32_t now)
{
    if (!g_play_layer) return 0;
    if (!pet_core_playing()) { pet_play_stop(false);return 0; }
    if (!g_play_pointer && now-g_play_last_touch_ms>=PET_PLAY_IDLE_MS) {
        pet_play_stop(false);return 0;
    }
    uint32_t elapsed=now-g_play_last_ms;
    g_play_last_ms=now;
    if (elapsed>100) elapsed=100;
    pet_action_t action=pet_core_action();
    if (g_pouncing && action==PET_ACTION_HAPPY) {
        uint32_t t=now-g_pounce_started_ms;
        if (t<400) {
            g_pet_x=g_pounce_from_x+(g_pounce_to_x-g_pounce_from_x)*(int)t/400;
            g_pet_y=g_pounce_from_y+(g_pounce_to_y-g_pounce_from_y)*(int)t/400;
            return DM(12)*(int)(t<200 ? t : 400-t)/200;
        }
        g_pet_x=g_pounce_to_x;g_pet_y=g_pounce_to_y;
    }
    if (action!=PET_ACTION_HAPPY) g_pouncing=false;
    if (!g_play_pointer) return 0;
    int sw=lv_display_get_horizontal_resolution(NULL),sh=lv_display_get_vertical_resolution(NULL);
    int margin=PET_SIZE/2+DM(12);
    int tx=g_play_point.x,ty=g_play_point.y;
    if (tx<margin) tx=margin;
    if (tx>sw-margin) tx=sw-margin;
    if (ty<margin) ty=margin;
    if (ty>sh-margin) ty=sh-margin;
    int dx=tx-g_pet_x,dy=ty-g_pet_y;
    int distance=abs(dx)>abs(dy) ? abs(dx)+abs(dy)/2 : abs(dy)+abs(dx)/2;
    if (dx) g_face_right=dx>0;
    /* Wait for the authored look/get-up/catch clip rather than restarting it. */
    if (action!=PET_ACTION_IDLE_SIT && action!=PET_ACTION_WALK) return 0;
    if (distance<=DM(34)) {
        /* Screen-edge clamping must not turn a distant finger into a catch. */
        int fx=g_play_point.x-g_pet_x,fy=g_play_point.y-g_pet_y;
        if (abs(fx)>DM(50) || abs(fy)>DM(50)) {
            if (action==PET_ACTION_WALK) dm_pet_on_input(PET_EVT_PLAY_PHASE,PET_PLAY_LOOK);
            return 0;
        }
        if (g_play_can_pounce) {
            g_play_can_pounce=false;g_play_anchor=g_play_point;
            dm_pet_on_input(PET_EVT_PLAY_PHASE,PET_PLAY_POUNCE);
            g_pouncing=true;g_pounce_started_ms=now;
            g_pounce_from_x=g_pet_x;g_pounce_from_y=g_pet_y;
            g_pounce_to_x=tx;g_pounce_to_y=ty;
        } else if (action==PET_ACTION_WALK) dm_pet_on_input(PET_EVT_PLAY_PHASE,PET_PLAY_LOOK);
    } else if (action==PET_ACTION_IDLE_SIT) {
        dm_pet_on_input(PET_EVT_PLAY_PHASE,PET_PLAY_CHASE);
    } else {
        int step=PET_PLAY_SPEED_PX_S*(int)elapsed/1000;
        if (step<1) step=1;
        if (step>distance) step=distance;
        g_pet_x+=dx*step/distance;g_pet_y+=dy*step/distance;
    }
    return 0;
}

static dm_pet_touch_zone_t pet_touch_zone(lv_point_t point)
{
    /* Image object stays 320px wide before scale. Use the visible authored
     * silhouette (not its unscaled LVGL bounds) to keep the head reachable. */
    return point.y<g_pet_y-PET_SIZE/10 ? PET_TOUCH_HEAD : PET_TOUCH_BODY;
}

static void pet_belly_dodge(int touch_x)
{
    pet_cancel_return();pet_stop_walk();
    int margin=PET_SIZE/2+DM(12);
    int limit=lv_display_get_horizontal_resolution(NULL)-margin;
    int next=g_pet_x+(touch_x<g_pet_x ? DM(14) : -DM(14));
    if (next<margin) next=margin;
    if (next>limit) next=limit;
    g_return_from_x=g_pet_x;g_return_from_y=g_pet_y;
    g_return_to_x=next;g_return_to_y=g_pet_y;g_returning=true;
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,g_cont);
    lv_anim_set_values(&a,0,1024);lv_anim_set_duration(&a,180);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a,pet_return_exec_cb);
    lv_anim_set_completed_cb(&a,pet_return_done_cb);lv_anim_start(&a);
}

static void pet_body_press_cb(lv_event_t *e)
{
    /* 阻止冒泡到 standby_overlay → 否则一点宠物就 wake 回主屏 */
    lv_event_stop_bubbling(e);
    if (g_play_layer) { pet_play_input(e);return; }
    if (g_return_walking) { g_return_walking=false; pet_stop_walk(); }  /* 回位途中被抓住 */
    g_relax_sent=false;
    g_press_active = true;
    g_long_press_sent = false;
    g_press_start_ms = lv_tick_get();
    g_dragging = false;
    /* 记录按下时的触摸坐标 */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev)
        {
            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            g_drag_start_x = pt.x;
            g_drag_start_y = pt.y;
            g_press_zone=pet_touch_zone(pt);
        }
}

/* PRESSING 回调：手指移动时实时更新宠物位置 */
static void pet_body_pressing_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (g_play_layer) { pet_play_input(e);return; }
    if (!g_press_active) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (g_long_press_sent) {
        /* After a hold begins, finger strokes stay petting instead of teleporting the cat. */
        if (!g_relax_sent && g_press_zone==PET_TOUCH_HEAD &&
            lv_tick_get()-g_press_start_ms>=PET_RELAX_MS) {
            g_relax_sent=true;dm_pet_on_input(PET_EVT_PET_RELAX,0);
        }
        return;
    }
    if (!g_dragging)
        {
            /* 检查是否超过拖拽阈值 */
            int dx = pt.x - g_drag_start_x;
            int dy = pt.y - g_drag_start_y;
            if (dx * dx + dy * dy < PET_DRAG_THRESHOLD * PET_DRAG_THRESHOLD)
                {
                    if (!g_long_press_sent &&
                        lv_tick_get() - g_press_start_ms >= PET_LONG_PRESS_MS)
                        {
                            g_long_press_sent = true;
                            g_tap_count = 0;
                            if (g_press_zone==PET_TOUCH_BODY) pet_belly_dodge(pt.x);
                            dm_pet_on_input(PET_EVT_LONG_PRESS,g_press_zone==PET_TOUCH_HEAD ? 1 : 2);
                        }
                    return;
                }
            g_dragging = true;
            g_tap_count = 0;
            pet_cancel_return();
            /* 丢弃旧漫游路径，松手后不能再沿旧起点计算位移。 */
            pet_stop_walk();
        }

    /* 拖拽中：直接更新宠物位置跟随手指 */
    g_pet_x = pt.x;
    g_pet_y = pt.y;
    /* 边缘钳位 */
    lv_coord_t sw = lv_display_get_horizontal_resolution(NULL);
    lv_coord_t sh = lv_display_get_vertical_resolution(NULL);
    lv_coord_t mx = PET_SIZE / 2;
    if (g_pet_x < mx)    g_pet_x = mx;
    if (g_pet_x > sw-mx)  g_pet_x = sw-mx;
    if (g_pet_y < mx)    g_pet_y = mx;
    if (g_pet_y > sh-mx)  g_pet_y = sh-mx;
    /* 立即应用位置 */
    lv_obj_set_pos(g_cont, g_pet_x - lv_obj_get_width(g_cont) / 2,
                   g_pet_y - lv_obj_get_height(g_cont) / 2);
    pet_align_popups();
}

static void pet_body_release_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (g_play_layer) { pet_play_input(e);return; }
    if (!g_press_active) return;
    g_press_active = false;

    /* 拖拽结束：松手先蹲坐一阵；同时发 DRAG 事件 → 开心反馈 */
    if (g_dragging)
        {
            g_dragging = false;
            g_idle_deadline=lv_tick_get()+rand_range(3000,6000);
            dm_pet_on_event(PET_EVT_DRAG);
            pet_free_hold();   /* 搬过就留在原地，5 分钟后自己走回去 */
            return;
        }

    uint32_t duration = lv_tick_get() - g_press_start_ms;

    if (g_long_press_sent) {
        dm_pet_on_input(PET_EVT_TOUCH_RELEASE,0);
        g_long_press_sent=false;return;
    }

    if (duration >= PET_LONG_PRESS_MS)
        {
            /* 长按 = 抚摸（短按只开关菜单，不再摸猫） */
            g_tap_count = 0;
            if (g_press_zone==PET_TOUCH_BODY) pet_belly_dodge(g_drag_start_x);
            dm_pet_on_input(PET_EVT_LONG_PRESS,g_press_zone==PET_TOUCH_HEAD ? 0 : 2);
        }
    else
        {
            /* 短按只管开关胶囊：摸猫归长按（PET_LONG_PRESS_MS）。
             * 原来短按同时发 TAP_HEAD/TAP_BODY 再弹菜单，等于"一摸必弹窗"，
             * 快速点两下就是开了又关，看着像没反应。 */
            if (g_feed_popup)
                {
                    pet_close_feed_popup();
                    g_tap_count = 0;
                }
            else
                {
                    pet_show_feed_popup();
                    g_tap_count = 1;
                }
        }
}

static void pet_body_cancel_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (g_play_layer) { pet_play_input(e);return; }
    bool was_dragging = g_dragging;
    if (g_long_press_sent) dm_pet_on_event(PET_EVT_TOUCH_RELEASE);
    g_press_active = false;
    g_dragging = false;
    g_long_press_sent = false;
    g_tap_count = 0;
    if (was_dragging) pet_return_to_roam();
}

/* 吞掉 CLICKED，防止冒泡唤醒锁屏 */
static void pet_consume_click_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

/****************************************************************************
 * 弹窗：喂食弹窗 / 饥饿提示（双击或三连点直开喂食）
 ****************************************************************************/

static lv_obj_t *pet_create_satiety_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, DM(120), DM(5));
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_SEP), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, DM(3), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, DM(3), LV_PART_INDICATOR);
    return bar;
}

static void pet_update_satiety(void)
{
    int value = 100 - pet_core_data()->attrs.hunger;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (g_feed_satiety_lbl)
        lv_label_set_text_fmt(g_feed_satiety_lbl, "饱食度 %d%%", value);
    if (g_feed_satiety_bar)
        lv_bar_set_value(g_feed_satiety_bar, value, LV_ANIM_OFF);
    if (g_satiety_lbl)
        lv_label_set_text_fmt(g_satiety_lbl, "饱食度 %d%%", value);
    if (g_satiety_bar)
        lv_bar_set_value(g_satiety_bar, value, LV_ANIM_OFF);
}

/* Compare global coordinates, then apply the delta in the parent's coordinate
 * system. This also works when the shared parent has padding or an offset.
 * Call only after content layout / alignment so LV_SIZE_CONTENT is resolved.
 */
static void pet_clamp_to_screen(lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_display_t *display = lv_obj_get_display(obj);
    lv_coord_t margin = DM(12);
    lv_coord_t right = lv_display_get_horizontal_resolution(display) - margin - 1;
    lv_coord_t bottom = lv_display_get_vertical_resolution(display) - margin - 1;
    lv_coord_t dx = 0, dy = 0;
    if (area.x2 > right) dx = right - area.x2;
    if (area.x1 + dx < margin) dx = margin - area.x1;
    if (area.y2 > bottom) dy = bottom - area.y2;
    if (area.y1 + dy < margin) dy = margin - area.y1;
    if (dx || dy)
        lv_obj_set_pos(obj, lv_obj_get_x(obj) + dx, lv_obj_get_y(obj) + dy);
}

static void pet_align_satiety(void)
{
    if (!g_satiety || !g_cont) return;
    /* 弹窗打开时让反馈排在其上方，避免两者互相遮挡。 */
    lv_obj_align_to(g_satiety, g_feed_popup ? g_feed_popup : g_cont,
                   LV_ALIGN_OUT_TOP_MID, 0, -DM(8));
    pet_clamp_to_screen(g_satiety);
}

static void pet_align_popups(void)
{
    if (!g_cont) return;
    if (g_feed_popup)
        {
            lv_obj_align_to(g_feed_popup, g_cont, LV_ALIGN_OUT_TOP_MID, 0, -DM(8));
            pet_clamp_to_screen(g_feed_popup);
        }
    if (g_hunger_alert)
        {
            lv_obj_align_to(g_hunger_alert, g_cont, LV_ALIGN_OUT_TOP_MID, 0, -DM(10));
            pet_clamp_to_screen(g_hunger_alert);
        }
    pet_align_satiety();
}

static void pet_hide_satiety(void)
{
    if (g_satiety_timer)
        {
            lv_timer_del(g_satiety_timer);
            g_satiety_timer = NULL;
        }
    if (g_satiety) lv_obj_del(g_satiety);
    g_satiety = NULL;
    g_satiety_lbl = NULL;
    g_satiety_bar = NULL;
}

static void satiety_auto_close_cb(lv_timer_t *t)
{
    (void)t;
    g_satiety_timer = NULL;
    pet_hide_satiety();
}

static void view_show_satiety_impl(void)
{
    if (!g_cont) return;
    if (!g_satiety)
        {
            g_satiety = lv_obj_create(lv_obj_get_parent(g_cont));
            lv_obj_set_size(g_satiety, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_add_flag(g_satiety, LV_OBJ_FLAG_FLOATING);
            lv_obj_clear_flag(g_satiety, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(g_satiety, lv_color_hex(COL_CARD), 0);
            lv_obj_set_style_bg_opa(g_satiety, LV_OPA_90, 0);
            lv_obj_set_style_radius(g_satiety, DM(10), 0);
            lv_obj_set_style_border_width(g_satiety, 1, 0);
            lv_obj_set_style_border_color(g_satiety, lv_color_hex(COL_SEP), 0);
            lv_obj_set_style_pad_hor(g_satiety, DM(8), 0);
            lv_obj_set_style_pad_ver(g_satiety, DM(4), 0);
            lv_obj_set_style_pad_row(g_satiety, DM(4), 0);
            lv_obj_set_flex_flow(g_satiety, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(g_satiety, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            g_satiety_lbl = lv_label_create(g_satiety);
            lv_obj_set_style_text_font(g_satiety_lbl, FONT_BODY, 0);
            lv_obj_set_style_text_color(g_satiety_lbl, lv_color_hex(COL_TEXT), 0);
            g_satiety_bar = pet_create_satiety_bar(g_satiety);
        }
    pet_update_satiety();
    pet_align_popups();
    lv_obj_move_foreground(g_satiety);
    if (g_satiety_timer) lv_timer_reset(g_satiety_timer);
    else
        {
            g_satiety_timer = lv_timer_create(satiety_auto_close_cb, 3000, NULL);
            lv_timer_set_repeat_count(g_satiety_timer, 1);
        }
    if (g_feed_close_timer) lv_timer_reset(g_feed_close_timer);
}

/* 喂食弹窗内的食物按钮回调 */
static void feed_btn_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    int food_idx = (int)(intptr_t)lv_event_get_user_data(e);
    dm_pet_on_event((dm_pet_event_t)((int)PET_EVT_FEED_FISH + food_idx));
}

static void feed_close_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    pet_close_feed_popup();
}

/* 喂食弹窗自动关闭（LVGL 9 timer 回调签名；勿复用 event 回调） */
static void feed_auto_close_cb(lv_timer_t *t)
{
    (void)t;
    g_feed_close_timer=NULL;
    pet_close_feed_popup();
}

static void pet_close_feed_popup(void)
{
    if (g_feed_close_timer) { lv_timer_del(g_feed_close_timer);g_feed_close_timer=NULL; }
    if (g_feed_popup) { lv_obj_del(g_feed_popup);g_feed_popup=NULL; }
    g_feed_satiety_lbl = NULL;
    g_feed_satiety_bar = NULL;
    pet_align_popups();
}

/* 浮动胶囊菜单内按钮动作：点选即执行并收起胶囊 */
static void feed_fish_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    dm_pet_on_input(PET_EVT_FEED_FISH, 0);
    pet_close_feed_popup();
}

static void pet_show_feed_popup(void)
{
    pet_close_feed_popup();

    lv_obj_t *parent = lv_obj_get_parent(g_cont);
    if (!parent) return;

    /* 弹出菜单期间暂停漫游移动，让猫咪站立等待交互 */
    pet_stop_walk();
    g_idle_deadline = lv_tick_get() + 8000;

    g_feed_satiety_lbl = NULL;
    g_feed_satiety_bar = NULL;

    /* 轻量浮动玻璃胶囊 */
    g_feed_popup = lv_obj_create(parent);
    lv_obj_set_size(g_feed_popup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_feed_popup, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_grad_color(g_feed_popup, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_grad_dir(g_feed_popup, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_feed_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_feed_popup, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_feed_popup, 1, 0);
    lv_obj_set_style_border_color(g_feed_popup, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(g_feed_popup, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(g_feed_popup, PET_MENU_SHADOW, 0);
    lv_obj_set_style_shadow_color(g_feed_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(g_feed_popup, LV_OPA_30, 0);
    lv_obj_set_style_pad_hor(g_feed_popup, PET_MENU_PAD_H, 0);
    lv_obj_set_style_pad_ver(g_feed_popup, PET_MENU_PAD_V, 0);
    lv_obj_set_style_pad_column(g_feed_popup, PET_MENU_PAD_COL, 0);
    lv_obj_set_scrollbar_mode(g_feed_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g_feed_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_feed_popup, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(g_feed_popup, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_feed_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(g_feed_popup, pet_consume_click_cb, LV_EVENT_CLICKED, NULL);

    /* 胶囊按钮 1：喂鱼（经典深蓝） */
    lv_obj_t *b_feed = lv_btn_create(g_feed_popup);
    lv_obj_set_size(b_feed, PET_MENU_BTN_W, PET_MENU_BTN_H);
    lv_obj_set_style_radius(b_feed, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b_feed, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(b_feed, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(b_feed, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b_feed, 1, 0);
    lv_obj_set_style_border_color(b_feed, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_opa(b_feed, LV_OPA_60, 0);
    lv_obj_clear_flag(b_feed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b_feed, feed_fish_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_feed = lv_label_create(b_feed);
    lv_label_set_text(l_feed, "喂鱼");
    lv_obj_set_style_text_font(l_feed, FONT_BODY, 0);
    lv_obj_set_style_text_color(l_feed, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(l_feed);

    /* 胶囊按钮 2：逗猫（温暖活力橙） */
    lv_obj_t *b_play = lv_btn_create(g_feed_popup);
    lv_obj_set_size(b_play, PET_MENU_BTN_W, PET_MENU_BTN_H);
    lv_obj_set_style_radius(b_play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b_play, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_bg_opa(b_play, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(b_play, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b_play, 1, 0);
    lv_obj_set_style_border_color(b_play, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_opa(b_play, LV_OPA_60, 0);
    lv_obj_clear_flag(b_play, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b_play, pet_play_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_play = lv_label_create(b_play);
    lv_label_set_text(l_play, "逗猫");
    lv_obj_set_style_text_font(l_play, FONT_BODY, 0);
    lv_obj_set_style_text_color(l_play, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(l_play);

    lv_obj_move_foreground(g_feed_popup);
    pet_align_popups();

    g_feed_close_timer = lv_timer_create(feed_auto_close_cb, 6000, NULL);
    if (g_feed_close_timer)
        lv_timer_set_repeat_count(g_feed_close_timer, 1);
}

/* 饥饿提示：饿时显示可点击气泡，点击直接投喂默认食物 */
static void hunger_alert_click_cb(lv_event_t *e);
static void hunger_alert_auto_close_cb(lv_timer_t *t);

static void view_show_hunger_alert_impl(void)
{
    if (!g_cont || g_hunger_alert) return;

    lv_obj_t *parent = lv_obj_get_parent(g_cont);
    if (!parent) return;

    g_hunger_alert = lv_obj_create(parent);
    lv_obj_set_size(g_hunger_alert, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(g_hunger_alert, LV_OBJ_FLAG_FLOATING);
    lv_obj_move_foreground(g_hunger_alert);
    lv_obj_set_style_bg_color(g_hunger_alert, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_bg_opa(g_hunger_alert, 242, 0);  /* 95% ≈ 242/255 */
    lv_obj_set_style_radius(g_hunger_alert, DM(12), 0);
    lv_obj_set_style_pad_all(g_hunger_alert, DM(10), 0);
    lv_obj_set_style_shadow_width(g_hunger_alert, 16, 0);
    lv_obj_set_style_shadow_opa(g_hunger_alert, LV_OPA_30, 0);
    lv_obj_add_flag(g_hunger_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_hunger_alert, hunger_alert_click_cb, LV_EVENT_CLICKED, NULL);

    g_hunger_alert_lbl = lv_label_create(g_hunger_alert);
    /* 只养猫：默认投喂鱼（狗/肉分支 P202 已删） */
    lv_label_set_text(g_hunger_alert_lbl, "饿啦！点我喂鱼");
    lv_obj_set_style_text_font(g_hunger_alert_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(g_hunger_alert_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(g_hunger_alert_lbl);

    /* 8秒自动隐藏（饿态持续由 core 再次触发显示） */
    g_hunger_close_timer = lv_timer_create(hunger_alert_auto_close_cb, 8000, NULL);
    lv_timer_set_repeat_count(g_hunger_close_timer, 1);
    pet_align_popups();
}

static void view_hide_hunger_alert_impl(void)
{
    if (g_hunger_close_timer) { lv_timer_del(g_hunger_close_timer);g_hunger_close_timer=NULL; }
    if (g_hunger_alert)
        {
            lv_obj_del(g_hunger_alert);
            g_hunger_alert = NULL;
            g_hunger_alert_lbl = NULL;
        }
}

static void hunger_alert_click_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    /* 点击直接投喂默认食物：猫喂鱼 */
    dm_pet_on_event(PET_EVT_FEED_FISH);
    view_hide_hunger_alert_impl();
}

static void hunger_alert_auto_close_cb(lv_timer_t *t)
{
    (void)t;
    g_hunger_close_timer=NULL;
    view_hide_hunger_alert_impl();
}

/****************************************************************************
 * 构建/销毁（门面调用）
 ****************************************************************************/

void pet_view_create(lv_obj_t *parent)
{
    if (!parent) return;
    pet_core_register_view(&g_ops);

    /* 容器：右下角（FLOATING 脱离 flex，不挤健康卡）
     * 尺寸收紧：只比宠物大一圈（气泡+名字），减少空白区吞触摸 */
    g_cont = lv_obj_create(parent);
    lv_obj_set_size(g_cont, PET_SIZE + DM(24), PET_SIZE + DM(48));
    lv_obj_set_style_bg_opa(g_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_cont, 0, 0);
    lv_obj_set_style_pad_all(g_cont, 0, 0);
    lv_obj_clear_flag(g_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_cont, LV_OBJ_FLAG_FLOATING);
    /* 整容器可点：放大命中区；吞 CLICKED 防唤醒 */
    lv_obj_add_flag(g_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_cont, pet_body_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_cont, pet_body_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_cont, pet_body_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_cont, pet_body_cancel_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(g_cont, pet_consume_click_cb, LV_EVENT_CLICKED, NULL);
    pet_park_bottom_right();

    /* 身体：lv_image 宠物；scale 按帧 header.w 自适应（全帧 128×128）
     * 不单独 CLICKABLE——命中穿透到 g_cont，避免子/父双触发 */
    g_body = lv_image_create(g_cont);
    lv_obj_set_size(g_body, PET_SIZE, PET_SIZE);
    lv_obj_align(g_body, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(g_body, LV_OBJ_FLAG_CLICKABLE);
    pet_renderer_init(&g_renderer,g_body,PET_SIZE);
    pet_view_refresh_sprite();

    /* 气泡（容器顶部，紧贴精灵头顶） */
    g_bubble = lv_obj_create(g_cont);
    /* Feedback must not steal the next tap or bubble it to the lock screen. */
    lv_obj_clear_flag(g_bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(g_bubble, LV_ALIGN_TOP_MID, 0, DM(14));
    lv_obj_set_style_bg_color(g_bubble, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_bubble, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_bubble, DM(10), 0);
    lv_obj_set_style_border_width(g_bubble, 1, 0);
    lv_obj_set_style_border_color(g_bubble, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_pad_hor(g_bubble, DM(8), 0);
    lv_obj_set_style_pad_ver(g_bubble, DM(4), 0);
    lv_obj_add_flag(g_bubble, LV_OBJ_FLAG_HIDDEN);

    g_bubble_lbl = lv_label_create(g_bubble);
    lv_label_set_text(g_bubble_lbl, "");
    lv_obj_set_style_text_font(g_bubble_lbl, PET_BUBBLE_FONT, 0);
    lv_obj_set_style_text_color(g_bubble_lbl, lv_color_hex(0x333333), 0);
}

bool pet_view_reuse(void)
{
    if (!g_cont || !lv_obj_is_valid(g_cont))
        return false;
    if (g_play_layer) pet_play_stop(false);
    pet_cancel_return();
    pet_stop_walk();
    pet_hide_satiety();
    g_press_active = false;
    g_dragging = false;
    g_long_press_sent = false;
    g_tap_count = 0;
    dm_pet_on_event(PET_EVT_TOUCH_RELEASE);
    g_renderer.clock.valid=false;g_walk_started=false;
    pet_view_refresh_sprite();
    pet_relocate_on_lock();
    return true;
}

void pet_view_destroy(void)
{
    if (g_play_layer) pet_play_stop(false);
    pet_core_unregister_view();
    pet_cancel_return();
    pet_hide_satiety();
    if (g_long_press_sent) dm_pet_on_event(PET_EVT_TOUCH_RELEASE);
    /* 弹窗挂在父容器上（非 g_cont 子树），随 g_cont 删除带不走，必须显式删 */
    pet_close_feed_popup();
    view_hide_hunger_alert_impl();
    if (g_cont && lv_obj_is_valid(g_cont)) lv_obj_del(g_cont);
    g_cont = NULL;
    g_body = NULL; g_bubble = NULL; g_bubble_lbl = NULL;
    g_feed_popup = NULL;
    g_hunger_alert = NULL; g_hunger_alert_lbl = NULL;
    g_bubble_ticks = 0;
    g_press_active = false;
    g_dragging = false;
    g_long_press_sent = false;
    g_tap_count = 0;
    pet_renderer_clear(&g_renderer);g_walk_started=false;
}

void pet_view_refresh_sprite(void)
{
    if (!g_body) return;
    g_renderer.clock.valid=false;
    pet_renderer_step(&g_renderer,pet_core_action(),pet_core_animation_id(),
        pet_core_touch_held(),!g_face_right,lv_tick_get());
}

void pet_view_suspend(void)
{
    if (g_play_layer) pet_play_stop(false);
    pet_cancel_return();
    pet_stop_walk();
    pet_hide_satiety();
    if (g_long_press_sent) dm_pet_on_event(PET_EVT_TOUCH_RELEASE);
    g_press_active=false;g_dragging=false;g_long_press_sent=false;
    g_tap_count=0;g_walk_started=false;g_renderer.clock.valid=false;
}
