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

/****************************************************************************
 * 配置常量（呈现层）
 ****************************************************************************/

#define PET_SIZE             DM(107)  /* 精灵原生 128×128 → 屏显约 256px (P199 定稿 2x, 勿动) */
#define PET_BUBBLE_FONT      FONT_BODY   /* Montserrat 不含中文→气泡用项目中文字体 */

/* 动画节拍（门面 timer 周期，数值放这里备查） */
#define PET_ANIM_TICK_MS        120   /* 动画帧推进（帧率由切帧分频定，见 anim_tick） */

/* 触摸检测 */
#define PET_RAPID_TAP_WINDOW_MS 800   /* 连续点击窗口 */
#define PET_RAPID_TAP_COUNT     3     /* 三连点 = 开喂食（喂食难触发，菜单已删，直达） */
#define PET_LONG_PRESS_MS       600   /* 长按阈值（抚摸，无菜单） */
#define PET_DRAG_THRESHOLD 8          /* 超过8px算拖拽 */

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

/* 动画帧（双计数器 + 窗跟踪：core 窗变化即从窗首重播） */
static int         g_tick = 0;               /* 时序（呼吸/位移用，不做帧索引） */
static int         g_state_frame = 0;
static int         g_walk_frame = 0;
static int         g_win_first = -1;
static int         g_win_last = -1;

/* 位置/朝向/漫游 */
static lv_coord_t g_pet_x = 0;
static lv_coord_t g_pet_y = 0;
static bool       g_face_right = false;   /* 朝向：true=面朝右（默认面朝左） */
static int        g_walk_pause = 0;       /* 蹲坐暂停 ticks（>0 时不动） */
static lv_coord_t g_bounce_off = 0;       /* 弹跳 y 偏移 */

/* 漫游区覆盖（ui_home 等布局拥有者可注入；无覆盖用默认启发式） */
static bool       g_roam_override = false;
static int        g_roam_x1, g_roam_y1, g_roam_x2, g_roam_y2;

/* 触摸检测 */
static uint32_t    g_last_tap_ms = 0;
static int         g_rapid_tap_count = 0;
static uint32_t    g_press_start_ms = 0;
static bool        g_press_active = false;
static bool        g_dragging = false;
static lv_coord_t  g_drag_start_x = 0;
static lv_coord_t  g_drag_start_y = 0;

/* 弹窗 */
static lv_obj_t *g_feed_popup = NULL;   /* 喂食弹窗（鱼/零食） */
static lv_obj_t *g_hunger_alert = NULL;
static lv_obj_t *g_hunger_alert_lbl = NULL;

/****************************************************************************
 * 前向声明
 ****************************************************************************/

static void pet_show_feed_popup(void);
static void pet_set_body_src(const lv_image_dsc_t *dsc);
static void pet_apply_sprite_scale(void);
static void pet_park_bottom_right(void);
static void pet_relocate_on_lock(void);
static void pet_consume_click_cb(lv_event_t *e);
static bool pet_walk_active(void);
static int rand_range(int lo, int hi);

/****************************************************************************
 * 工具
 ****************************************************************************/

static int rand_range(int lo, int hi)
{
    if (lo >= hi) return lo;
    return lo + rand() % (hi - lo + 1);
}

/* 朝右时精灵镜像缓冲（LVGL9 负 scale_x 不会绘制 → 运行时行翻转）
 * LV_USE_OS=0 单线程渲染：单缓冲即安全；猫帧 128×128 ARGB8888 = 65536B */
#define PET_MIRROR_BUF_BYTES    (128 * 128 * 4)
static uint8_t               g_mirror_px[PET_MIRROR_BUF_BYTES];
static lv_image_dsc_t        g_mirror_dsc;
static const lv_image_dsc_t *g_mirror_src = NULL;

static const lv_image_dsc_t *pet_x_mirror(const lv_image_dsc_t *src)
{
    if (!src || src->header.w == 0 || src->header.h == 0) return src;
    uint32_t w = src->header.w, h = src->header.h;
    if (w > 128 || h > 128) return src;              /* 超出缓冲 → 原样绘制 */
    if (src == g_mirror_src) return &g_mirror_dsc;   /* 同帧缓存，免重复翻转 */

    const uint8_t *in = src->data;
    uint8_t *out = g_mirror_px;
    uint32_t stride = src->header.stride;
    uint32_t iw = w * 4;                              /* ARGB8888：4B/px */
    for (uint32_t y = 0; y < h; y++)
        {
            const uint8_t *row = in + (size_t)y * stride;
            uint8_t *dst = out + (size_t)y * iw;
            for (uint32_t x = 0; x < w; x++)
                {
                    uint32_t sx = (w - 1 - x) * 4;
                    dst[x * 4 + 0] = row[sx + 0];
                    dst[x * 4 + 1] = row[sx + 1];
                    dst[x * 4 + 2] = row[sx + 2];
                    dst[x * 4 + 3] = row[sx + 3];
                }
        }
    g_mirror_dsc = *src;
    g_mirror_dsc.data = g_mirror_px;
    g_mirror_src = src;
    return &g_mirror_dsc;
}

/* 统一设置宠物帧源：朝右时自动镜像（替代 LVGL9 不可用的负 scale_x 翻转） */
static void pet_set_body_src(const lv_image_dsc_t *dsc)
{
    if (!g_body) return;
    if (g_face_right && dsc)
        dsc = pet_x_mirror(dsc);
    lv_image_set_src(g_body, dsc);
}

/* 是否正在走路（状态可走 + 蹲坐倒计时归零 + 未被拖拽） */
static bool pet_walk_active(void)
{
    return pet_core_can_walk() && !g_dragging && g_walk_pause == 0;
}

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

static void pet_park_bottom_right(void)
{
    if (!g_cont) return;
    int x1, y1, x2, y2;
    pet_roam_rect(&x1, &y1, &x2, &y2);
    /* 与 anim 位移的 zone 对齐：右下留白区底边，面朝左 */
    g_pet_x = x2;
    g_pet_y = y2;
    g_face_right = false;
    g_bounce_off = 0;
    g_walk_pause = rand_range(12, 24);
    lv_obj_set_pos(g_cont, g_pet_x - lv_obj_get_width(g_cont) / 2,
                   g_pet_y - lv_obj_get_height(g_cont) / 2);
    lv_obj_move_foreground(g_cont);
}

/* 进入锁屏/复用创建：保留散步区内拖拽位置，越界则复位右下角 */
static void pet_relocate_on_lock(void)
{
    if (!g_cont) return;
    int x1, y1, x2, y2;
    pet_roam_rect(&x1, &y1, &x2, &y2);
    if (g_pet_x < x1 || g_pet_x > x2)
        {
            pet_park_bottom_right();
            return;
        }
    g_bounce_off = 0;
    g_walk_pause = rand_range(6, 12);
    lv_obj_set_pos(g_cont, g_pet_x - lv_obj_get_width(g_cont) / 2,
                   g_pet_y - lv_obj_get_height(g_cont) / 2);
    lv_obj_move_foreground(g_cont);
}

static void pet_apply_sprite_scale(void)
{
    if (!g_body) return;
    const lv_image_dsc_t *dsc = pet_core_frames()[pet_core_state()].frames[0];
    uint32_t nw = (dsc && dsc->header.w > 0) ? dsc->header.w : 128;
    /* 256 = 1.0x；把原生 nw 像素缩放到 PET_SIZE 显示 */
    lv_image_set_scale(g_body, (uint32_t)PET_SIZE * 256 / nw);
}

/****************************************************************************
 * 动画节拍（门面 120ms timer 驱动）
 ****************************************************************************/

void pet_view_anim_tick(void)
{
    if (!g_body || !g_cont) return;

    g_tick++;

    /* 帧推进：core 窗变化即从窗首重播（行为切换不断帧、无跳变）；
     * 每 3 tick 切一帧（≈360ms ≈ 2.8fps，对齐 spec §十 2-4fps） */
    int first, last;
    pet_core_anim_range(&first, &last);
    if (first != g_win_first || last != g_win_last)
        {
            g_win_first = first;
            g_win_last = last;
            g_state_frame = first;
        }
    if (g_tick % 3 == 0)
        {
            g_state_frame++;
            if (g_state_frame > g_win_last) g_state_frame = g_win_first;
            g_walk_frame = (g_walk_frame + 1) % PET_WALK_FRAMES;
        }

    /* 拖拽中显示状态帧（walk_active 含 !dragging），松手不跳变 */
    if (pet_walk_active())
        {
            const lv_image_dsc_t *wf = g_cat_walk_frames.frames[g_walk_frame];
            if (wf) pet_set_body_src(wf);
        }
    else
        {
            const lv_image_dsc_t *frame =
                pet_core_frames()[pet_core_state()].frames[g_state_frame];
            if (frame) pet_set_body_src(frame);
        }

    /* ── 右下留白区内左右踱步 + 蹲坐交替（只 X 方向，Y 固定） ──
     * 拖拽中跳过整段移动，位置由 pressing 回调实时控制 */
    g_bounce_off = 0;
    if (!g_dragging)
        {
            /* 弹跳跟随帧推进（小幅），旧 8Hz ±16px 是 P199 视觉误判推手之一 */
            if (pet_core_state() == PET_STATE_PLAY)
                g_bounce_off = (g_state_frame % 2) ? -4 : 4;
            else if (pet_core_state() == PET_STATE_CELEBRATE)
                g_bounce_off = (g_state_frame % 2) ? -6 : 6;
        }

    {
        int x1, y1, x2, y2;
        pet_roam_rect(&x1, &y1, &x2, &y2);
        lv_coord_t zone_x1 = x1, zone_x2 = x2, zone_y = y2;

        if (!g_dragging)
            {
                /* 平滑归位到走路道：拖拽/二次锁屏位置先停原地，
                 * 每 tick 朝 zone_y 走近几步，恢复踱步时已自然滑回 */
                if (g_pet_y > zone_y + 2)      g_pet_y -= 6;
                else if (g_pet_y < zone_y - 2) g_pet_y += 6;
                else                           g_pet_y = zone_y;

                if (pet_core_can_walk())
                    {
                        if (g_walk_pause > 0)
                            {
                                g_walk_pause--;          /* 蹲坐倒计时 */
                            }
                        else if (g_tick % 2 == 0)
                            {
                                /* 走路帧已开播，每2tick 移2px（缓慢踱步） */
                                if (g_face_right)
                                    g_pet_x += 2;
                                else
                                    g_pet_x -= 2;
                                /* 到边缘 → 转身 + 进入蹲坐 */
                                if (g_pet_x >= zone_x2) { g_pet_x = zone_x2; g_face_right = false; g_walk_pause = 16 + rand() % 20; }
                                if (g_pet_x <= zone_x1) { g_pet_x = zone_x1; g_face_right = true;  g_walk_pause = 16 + rand() % 20; }
                            }
                    }
                /* 非可走动状态：原地静止，不位移、不走帧 */
            }
    }

    /* ── 呼吸脉动（0.7Hz ±0.8%；旧 2.6Hz ±1.5% 是点击大小跳变观感来源） ── */
    {
        const lv_image_dsc_t *dsc = pet_core_frames()[pet_core_state()].frames[0];
        uint32_t nw = (dsc && dsc->header.w > 0) ? dsc->header.w : 128;
        uint32_t base = (uint32_t)PET_SIZE * 256 / nw;
        float breathe = sinf(g_tick * 0.08f);
        uint32_t s = base + (uint32_t)(breathe * (float)base / 128); /* ±0.8% */
        lv_image_set_scale_x(g_body, s);
        lv_image_set_scale_y(g_body, s);
    }

    /* 应用位置（容器中心对齐到 g_pet_x, g_pet_y） */
    lv_obj_set_pos(g_cont, g_pet_x - lv_obj_get_width(g_cont) / 2,
                   g_pet_y - lv_obj_get_height(g_cont) / 2 + g_bounce_off);
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

static const pet_view_ops_t g_ops =
{
    .show_bubble       = pet_view_show_bubble,
    .show_hunger_alert = view_show_hunger_alert_impl,
    .hide_hunger_alert = view_hide_hunger_alert_impl,
};

/****************************************************************************
 * 触摸回调
 ****************************************************************************/

static void pet_body_press_cb(lv_event_t *e)
{
    /* 阻止冒泡到 standby_overlay → 否则一点宠物就 wake 回主屏 */
    lv_event_stop_bubbling(e);
    g_press_active = true;
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
        }
}

/* PRESSING 回调：手指移动时实时更新宠物位置 */
static void pet_body_pressing_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (!g_press_active) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (!g_dragging)
        {
            /* 检查是否超过拖拽阈值 */
            int dx = pt.x - g_drag_start_x;
            int dy = pt.y - g_drag_start_y;
            if (dx * dx + dy * dy < PET_DRAG_THRESHOLD * PET_DRAG_THRESHOLD)
                return;
            g_dragging = true;
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
}

static void pet_body_release_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (!g_press_active) return;
    g_press_active = false;

    /* 拖拽结束：松手先蹲坐一阵；同时发 DRAG 事件 → 开心反馈 */
    if (g_dragging)
        {
            g_dragging = false;
            g_walk_pause = rand_range(12, 24);
            dm_pet_on_event(PET_EVT_DRAG);
            return;
        }

    uint32_t duration = lv_tick_get() - g_press_start_ms;
    uint32_t now = lv_tick_get();

    if (duration >= PET_LONG_PRESS_MS)
        {
            /* 长按 = 抚摸（无菜单：互动条已删，短按只触摸，长按只抚摸） */
            dm_pet_on_event(PET_EVT_LONG_PRESS);
        }
    else
        {
            /* 短按 = 轻点 */
            uint32_t gap = now - g_last_tap_ms;
            g_last_tap_ms = now;

            if (gap < PET_RAPID_TAP_WINDOW_MS)
                g_rapid_tap_count++;
            else
                g_rapid_tap_count = 1;

            if (g_rapid_tap_count >= PET_RAPID_TAP_COUNT)
                {
                    /* 三连点 = 直开喂食（喂食入口难触发，互动条已删，不绕菜单） */
                    pet_show_feed_popup();
                    g_rapid_tap_count = 0;
                }
            else
                {
                    /* 第1/2下：只给轻触反应，不弹任何菜单；
                     * 第2下静默等第3下进 PLAY */
                    lv_area_t coords;
                    lv_obj_get_coords(g_body, &coords);
                    lv_coord_t h = coords.y2 - coords.y1 + 1;
                    lv_indev_t *indev = lv_indev_get_act();
                    lv_point_t pt;
                    if (indev)
                        {
                            lv_indev_get_point(indev, &pt);
                            if (pt.y < coords.y1 + h / 3)
                                dm_pet_on_event(PET_EVT_TAP_HEAD);
                            else
                                dm_pet_on_event(PET_EVT_TAP_BODY);
                        }
                    else
                        {
                            dm_pet_on_event(PET_EVT_TAP_BODY);
                        }
                }
        }
}

/* 吞掉 CLICKED，防止冒泡唤醒锁屏 */
static void pet_consume_click_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

/****************************************************************************
 * 弹窗：喂食弹窗 / 饥饿提示（P202：互动条已删，三连点直开喂食）
 ****************************************************************************/

/* 喂食弹窗内的食物按钮回调 */
static void feed_btn_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    int food_idx = (int)(intptr_t)lv_event_get_user_data(e);
    dm_pet_on_event((dm_pet_event_t)((int)PET_EVT_FEED_FISH + food_idx));
    if (g_feed_popup) { lv_obj_del(g_feed_popup); g_feed_popup = NULL; }
}

static void feed_close_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (g_feed_popup) { lv_obj_del(g_feed_popup); g_feed_popup = NULL; }
}

/* 喂食弹窗自动关闭（LVGL 9 timer 回调签名；勿复用 event 回调） */
static void feed_auto_close_cb(lv_timer_t *t)
{
    (void)t;
    if (g_feed_popup) { lv_obj_del(g_feed_popup); g_feed_popup = NULL; }
}

/* 喂食弹窗（三连点/饥饿红泡进入：鱼主食 + 零食成年奖励） */
static void pet_show_feed_popup(void)
{
    if (g_feed_popup) { lv_obj_del(g_feed_popup); g_feed_popup = NULL; }

    lv_obj_t *parent = lv_obj_get_parent(g_cont);
    if (!parent) return;

    /* 弹窗容器（列布局：属性行 + 标题 + 分隔 + 食物行 + 关闭） */
    g_feed_popup = lv_obj_create(parent);
    lv_obj_set_size(g_feed_popup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(g_feed_popup, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(g_feed_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_feed_popup, pet_consume_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align_to(g_feed_popup, g_cont, LV_ALIGN_OUT_TOP_MID, 0, -DM(8));
    lv_obj_move_foreground(g_feed_popup);
    lv_obj_set_style_bg_color(g_feed_popup, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_feed_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_feed_popup, DM(12), 0);
    lv_obj_set_style_border_width(g_feed_popup, 1, 0);
    lv_obj_set_style_border_color(g_feed_popup, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_pad_all(g_feed_popup, DM(8), 0);
    lv_obj_set_style_shadow_width(g_feed_popup, 16, 0);
    lv_obj_set_style_shadow_opa(g_feed_popup, LV_OPA_30, 0);
    lv_obj_set_flex_flow(g_feed_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_feed_popup, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_row(g_feed_popup, DM(4), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(g_feed_popup);
    lv_label_set_text(title, LV_SYMBOL_DOWN " 喂食");
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

    /* 分隔 */
    lv_obj_t *sep = lv_obj_create(g_feed_popup);
    lv_obj_set_size(sep, DM(180), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* 食物行：只留两样（P202 简化：鱼=主食天天吃，零食=成年奖励；
     * 肉/菜仍在引擎枚举里（狗/语音以后用），UI 不摆出来添乱。
     * 属性五维搬 Settings 宠物区看，这里不堆） */
    lv_obj_t *food_row = lv_obj_create(g_feed_popup);
    lv_obj_set_size(food_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(food_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(food_row, 0, 0);
    lv_obj_set_flex_flow(food_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(food_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(food_row, DM(8), 0);

    /* 大按钮：名 + 一行小注（未成年零食置灰，点按引擎给“还小”提示） */
    {
        bool adult = (pet_core_life_stage() >= PET_LIFE_ADULT);
        struct { int idx; const char *name; const char *hint; bool locked; } foods[] = {
            { PET_FOOD_FISH,  "鱼",   "主食",     false   },
            { PET_FOOD_SNACK, "零食", adult ? "奖励" : "成年解锁", !adult },
        };
        for (int i = 0; i < 2; i++)
            {
                lv_obj_t *btn = lv_btn_create(food_row);
                lv_obj_set_size(btn, DM(110), DM(64));
                lv_obj_set_style_radius(btn, DM(12), 0);
                lv_obj_set_style_bg_color(btn,
                    foods[i].locked ? lv_color_hex(0xE0E0E0) : lv_color_hex(0xFFB84D), 0);
                lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(btn, 0, 0);
                lv_obj_add_event_cb(btn, feed_btn_cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)foods[i].idx);
                lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

                lv_obj_t *fl = lv_label_create(btn);
                lv_label_set_text(fl, foods[i].name);
                lv_obj_set_style_text_font(fl, FONT_TITLE, 0);
                lv_obj_set_style_text_color(fl, lv_color_hex(0xFFFFFF), 0);

                lv_obj_t *hl = lv_label_create(btn);
                lv_label_set_text(hl, foods[i].hint);
                lv_obj_set_style_text_font(hl, FONT_CAPTION, 0);
                lv_obj_set_style_text_color(hl, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_opa(hl, LV_OPA_80, 0);
            }
    }

    /* 关闭按钮 */
    lv_obj_t *close_btn = lv_btn_create(g_feed_popup);
    lv_obj_set_size(close_btn, DM(36), DM(36));
    lv_obj_set_style_radius(close_btn, DM(8), 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, feed_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(close_lbl);

    /* 5秒自动关闭 */
    lv_timer_t *auto_close = lv_timer_create(feed_auto_close_cb, 5000, NULL);
    lv_timer_set_repeat_count(auto_close, 1);
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
    lv_obj_align_to(g_hunger_alert, g_cont, LV_ALIGN_OUT_TOP_MID, 0, -DM(10));
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
    lv_timer_t *auto_close = lv_timer_create(hunger_alert_auto_close_cb, 8000, NULL);
    lv_timer_set_repeat_count(auto_close, 1);
}

static void view_hide_hunger_alert_impl(void)
{
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
    lv_obj_add_event_cb(g_cont, pet_consume_click_cb, LV_EVENT_CLICKED, NULL);
    pet_park_bottom_right();

    /* 身体：lv_image 宠物；scale 按帧 header.w 自适应（全帧 128×128）
     * 不单独 CLICKABLE——命中穿透到 g_cont，避免子/父双触发 */
    g_body = lv_image_create(g_cont);
    lv_obj_set_size(g_body, PET_SIZE, PET_SIZE);
    lv_obj_align(g_body, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(g_body, LV_OBJ_FLAG_CLICKABLE);
    pet_view_refresh_sprite();

    /* 气泡（容器顶部，紧贴精灵头顶） */
    g_bubble = lv_obj_create(g_cont);
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
    pet_relocate_on_lock();
    pet_apply_sprite_scale();
    return true;
}

void pet_view_destroy(void)
{
    pet_core_unregister_view();
    /* 弹窗挂在父容器上（非 g_cont 子树），随 g_cont 删除带不走，必须显式删 */
    if (g_feed_popup && lv_obj_is_valid(g_feed_popup)) lv_obj_del(g_feed_popup);
    if (g_hunger_alert && lv_obj_is_valid(g_hunger_alert)) lv_obj_del(g_hunger_alert);
    if (g_cont && lv_obj_is_valid(g_cont)) lv_obj_del(g_cont);
    g_cont = NULL;
    g_body = NULL; g_bubble = NULL; g_bubble_lbl = NULL;
    g_feed_popup = NULL;
    g_hunger_alert = NULL; g_hunger_alert_lbl = NULL;
    g_bubble_ticks = 0;
}

void pet_view_refresh_sprite(void)
{
    if (!g_body) return;
    const lv_image_dsc_t *f0 = pet_core_frames()[pet_core_state()].frames[0];
    if (f0) pet_set_body_src(f0);
    pet_apply_sprite_scale();
}
