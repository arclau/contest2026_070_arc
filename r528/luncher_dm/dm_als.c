/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_als.c
 * LTR553 ALS 环境光服务层（P153：锁屏亮度显示 + 暗光开灯提醒）
 *
 * 架构（与 dm_weather / dm_ld2410b 同款后台轮询风格）：
 *   dm_als_init()   → orb_subscribe_multi(ORB_ID(sensor_light)) + LVGL 1s timer
 *   dm_als_timer_cb → poll + orb_copy → 更新 s_cur_lux（供 UI 显示）
 *                     暗光状态机（阈值/防抖/冷却）→ 触发 dm_als_set_dark_cb
 *   dm_als_get_lux()→ 锁屏健康卡每秒读取显示（文本去重由 UI 层做）
 *
 * 驱动侧：ltr553.c 轮询线程计算 lux → push_event(SENSOR_TYPE_LIGHT) →
 * uORB sensor_light（float light 单位 lux）。本模块只订阅不写驱动。
 ****************************************************************************/

#include <nuttx/config.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
/* P153：uORB 接口与 luncher_dm.c 一致（sensor_temp/humi 同款）——
 * sensor_light 结构由 <sensor/light.h> 提供（uORB topic 头），
 * orb_* API 在 <uORB/uORB.h>。勿改用 nuttx/uorb.h（结构未导出）。 */
#include <sensor/light.h>
#include <uORB/uORB.h>

#include <lvgl.h>

#include "dm_als.h"
#include "dm_health_cfg.h"   /* P180：自动亮度开关持久化 */
#include "dm_ld2410b.h"      /* 2026-09-14：暗光弹窗仅对真人触发（降误报） */
#include "deskmate_ui.h"     /* P180：dm_brightness_write + g_screen_brightness
                              * （extern）+ dm_night_shift_enabled（仲裁） */

/* ── 全局状态 ── */
static int          s_light_fd = -1;   /* sensor_light 订阅 fd */
static int          s_cur_lux  = -1;   /* 最近一次 lux（int，-1=无数据） */
static dm_als_dark_cb_t s_dark_cb = NULL;

/* P180：自动亮度状态（内存镜像，落盘由 UI 开关负责写 dm_health.cfg） */
static bool         g_auto_bright = false;
static int          g_auto_cur    = -1;  /* 引擎当前写入的背光值；-1=未起步 */

/* P180：lux → 目标亮度映射（阶梯，暗→亮）。按环境照明分档，
 * CFG 可改？——固定档位先上板验证，观感不合适再配置化。 */
static int dm_auto_bright_target(int lux)
{
    if (lux < 1)    return 5;
    if (lux < 5)    return 10;
    if (lux < 20)   return 18;
    if (lux < 50)   return 28;
    if (lux < 150)  return 40;
    if (lux < 400)  return 55;
    if (lux < 900)  return 70;
    return 85;
}

/* P180：自动亮度平滑引擎（每次拿到新 lux 调用）。
 * 夜览开启时让步（夜览压暗是用户主动意图）；未在自动状态只更新 g_auto_cur
 * 基准不写背光（关闭后下次开启从当前亮度平滑起步）。 */
static void dm_auto_bright_tick(int lux)
{
    if (!g_auto_bright)
    {
        g_auto_cur = -1;
        return;
    }
    if (dm_night_shift_enabled())
        return;                       /* 夜览优先：不干扰背光 */

    int target = dm_auto_bright_target(lux);
    if (g_auto_cur < 0)
        g_auto_cur = g_screen_brightness;   /* 起步：从手动基准平滑过渡 */
    int diff = target - g_auto_cur;
    /* 每秒限幅 ±3% 平滑渐变（防环境光跳变闪背光） */
    if (diff > 0)
        g_auto_cur += diff < 3 ? diff : 3;
    else if (diff < 0)
        g_auto_cur += diff > -3 ? diff : -3;
    if (g_auto_cur != g_screen_brightness || diff != 0)
    {
        if (g_auto_cur < 0)  g_auto_cur = 0;
        if (g_auto_cur > 100) g_auto_cur = 100;
        dm_brightness_write(g_auto_cur);
    }
}

/* 暗光状态机（实际行为）：连续暗光只触发一次；光线恢复后再次变暗，
 * 距上次触发 ≥ 冷却期才再次弹窗（限频）。s_dark_active 期间不重复触发。 */
static bool         s_dark_active = false;   /* 当前处于暗光状态（未恢复） */
static lv_timer_t  *s_poll_timer = NULL;
static uint32_t     s_start_ms   = 0;        /* P156：init 时刻（开机稳定期基准） */

static void dm_als_timer_cb(lv_timer_t *timer);

/****************************************************************************
 * 内部：暗光状态机 tick（每次拿到新 lux 调用一次）
 ****************************************************************************/
static void dm_als_dark_tick(int lux, uint32_t now_ms)
{
    static uint32_t s_debounce_start = 0;   /* 进入暗光候选的起始时刻 */
    static uint32_t s_last_trigger  = 0;    /* 上次弹窗时刻（冷却） */

    /* P157 确认门已移除（2026-09-04）：传感器清洁后能正常读数，
     * 无需等待 ≥200 lux 才启用暗光检测。仅保留开机稳定期保险。 */

    /* P156：开机稳定期内不触发暗光弹窗——驱动首个有效 ALS 转换前
     * 可能持续推 0（CH0 未就绪），开机即弹"知道了"是误报 */
    if (now_ms - s_start_ms < DM_ALS_STARTUP_MS)
        return;

    bool too_dark  = (lux >= 0) && (lux < DM_ALS_DARK_LUX);
    bool recovered = (lux >= DM_ALS_DARK_EXIT_LUX);

    /* 2026-09-14 降误报①：退出迟滞——只有 lux ≥ EXIT_LUX 才判"恢复"并复位。
     * 1~7 lux 灰区保持当前状态，避免阈值边缘抖动反复重弹。 */
    if (recovered) {
        s_dark_active = false;
        s_debounce_start = 0;
        return;
    }

    /* 灰区（未到退出阈值也未到触发阈值）：暂停防抖计时（起点顺延到当前），
     * 防灰区久留"垫"满 120s、回暗瞬间误触发。 */
    if (!too_dark) {
        if (s_debounce_start != 0)
            s_debounce_start = now_ms;
        return;
    }

    if (!s_dark_active) {
        /* 刚进入暗光：记录候选起点，等 debounce 确认 */
        if (s_debounce_start == 0)
            s_debounce_start = now_ms;
        if (now_ms - s_debounce_start >= DM_ALS_DARK_DEBOUNCE_MS) {
            /* 2026-09-14 降误报②：触发前要求检测到真人（LD2410B）——无人不弹。
             * present<0（传感器不可用）按有人处理，避免无雷达板子功能失效。 */
            int present = dm_ld2410b_present();
            if (present == 0) {
                /* 无人：重置候选，等人来后在黑暗中重新计时 */
                s_debounce_start = 0;
                return;
            }
            /* 防抖通过：确认暗光 */
            s_dark_active = true;
            s_debounce_start = 0;
            /* 冷却检查：距上次触发 ≥ 冷却期才再次提醒 */
            if (s_last_trigger == 0 ||
                now_ms - s_last_trigger >= DM_ALS_DARK_COOLDOWN_MS) {
                s_last_trigger = now_ms;
                syslog(LOG_INFO, "[als] dark_tick TRIGGER lux=%d present=%d cb=%s\n",
                       lux, present, s_dark_cb ? "set" : "NULL");
                if (s_dark_cb)
                    s_dark_cb();
            }
        }
    }
    /* 已确认暗光：保持，不重复触发（冷却已含） */
}

/****************************************************************************
 * 内部：LVGL 轮询 timer（1s）
 ****************************************************************************/
static void dm_als_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    struct sensor_light light;
    struct pollfd fds;
    int ret;

    if (s_light_fd < 0)
        return;

    fds.fd = s_light_fd;
    fds.events = POLLIN;
    fds.revents = 0;

    ret = poll(&fds, 1, 0);
    if (ret <= 0)
        return;   /* 无新数据：正常（驱动按自己的节奏推送） */

    if (orb_copy(ORB_ID(sensor_light), s_light_fd, &light) == OK) {
        int lux = (int)light.light;
        if (lux < 0)
            lux = 0;
        s_cur_lux = lux;
        dm_als_dark_tick(lux, lv_tick_get());
        dm_auto_bright_tick(lux);   /* P180：自动亮度引擎（夜览时自动让位） */
    }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/
int dm_als_init(void)
{
    if (s_light_fd >= 0)
        return 0;   /* 已初始化 */

    s_light_fd = orb_subscribe_multi(ORB_ID(sensor_light), 0);
    if (s_light_fd < 0) {
        /* 无 LTR553 ALS：不打扰（锁屏亮度行显示 "--"） */
        return -1;
    }

    s_poll_timer = lv_timer_create(dm_als_timer_cb, DM_ALS_POLL_MS, NULL);
    if (!s_poll_timer) {
        /* timer 创建失败：释放订阅 fd，避免 fd 泄漏 + 亮度永远 "--" */
        orb_unsubscribe(s_light_fd);
        s_light_fd = -1;
        return -1;
    }
    lv_timer_set_repeat_count(s_poll_timer, -1);   /* 无限循环 */
    s_start_ms = lv_tick_get();   /* P156：开机稳定期基准（init 时刻） */

    /* P180：自动亮度开关记忆（cfg 落盘），开机即恢复上次状态 */
    g_auto_bright = dm_health_cfg_get_int(DM_ALS_AUTO_BRIGHT_CFG, 0) != 0;
    if (g_auto_bright)
        LV_LOG_USER("[auto-bright] restored ON from cfg");

    return 0;
}

int dm_als_get_lux(void)
{
    return s_cur_lux;
}

bool dm_als_auto_enabled(void)
{
    return g_auto_bright;
}

void dm_als_set_auto_bright(bool on)
{
    g_auto_bright = on;
    if (!on)
    {
        /* 关：恢复手动基准亮度 */
        if (g_auto_cur >= 0)
        {
            g_auto_cur = -1;
            dm_brightness_write(g_screen_brightness);
        }
        LV_LOG_USER("[auto-bright] off → restore %d%%", g_screen_brightness);
    }
    else
    {
        g_auto_cur = g_screen_brightness;   /* 下个 tick 无缝平滑起步 */
        LV_LOG_USER("[auto-bright] on (cur lux=%d)", s_cur_lux);
    }
}

void dm_als_set_dark_cb(dm_als_dark_cb_t cb)
{
    s_dark_cb = cb;
}

void dm_als_deinit(void)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_light_fd >= 0) {
        orb_unsubscribe(s_light_fd);
        s_light_fd = -1;
    }
    s_cur_lux = -1;
    g_auto_cur = -1;
}
