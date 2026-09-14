/*
 * ui_settings.c — 设置子页【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_settings_subpage（改名
 * ui_settings_create）+ settings 回调（brightness/volume/autolock/
 * wifi/bt/wifi_row/bt_row）+ g_autolock 档位 static。
 * 逻辑一字不改；共享构建器（settings_* / make_clean_cont / subpage_big_title
 * 等）留在 deskmate_ui.c，经 deskmate_ui.h extern 引用。
 *
 * 跨页耦合（Phase 1 保持原样，Phase 4 状态聚合时消除）：
 *   - settings_volume_cb 经 extern 引用 g_music_volume / music_vol_lbl
 *     （正式定义在 deskmate_ui.c music 区）。
 *   - settings_autolock_cb 经 extern 引用 g_idle_timeout / reset_idle_timer
 *     （待机状态在 deskmate_ui.c）。
 *   - settings_wifi_row_open_cb / settings_bt_row_open_cb 调 show_subpage()
 *     打开嵌套子页（WiFi/蓝牙已拆至 ui/ 目录）。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/statfs.h>                /* 存储真实容量（TF 卡 statfs） */
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_net.h"                    /* WiFi/蓝牙开关回调 + 初始状态查询 */
#include "dm_city.h"                   /* 2026-09-10 A 方案：城市设置（天气城市级定位，默认惠州） */
#include "dm_weather.h"                /* 2026-09-10 P211：日期与时间行自动/手动值 */
#include "dm_health_cfg.h"             /* 夜览/自动亮度开关持久化（/data/dm_health.cfg） */
#include "dm_als.h"                    /* P180：自动亮度开关（lux 驱动，夜览让位） */
#include "dm_pet.h"                    /* 宠物设置：名字/类型 */
#ifdef CONFIG_LUNCHER_DM_APP
#include "sunxi_hal_pwm.h"               /* 屏幕背光 PWM ch4（BOE 面板） */
#endif
#ifdef CONFIG_LED_RGB_WS2812
#include "lv_demo_panel_rgb_control.h"   /* WS2812 LED 亮度 */
#endif

/* ================================================================
 * SETTINGS SUBPAGE — iPadOS-style grouped settings
 * ================================================================ */

/* Brightness slider → 屏幕背光（PWM ch4 @40KHz，BOE 面板）+ WS2812 LED
 * P176~179 注：硬件写已上提为公共接口 dm_brightness_write()（deskmate_ui.c，
 * 滑块/夜览/自动亮度共用），见 deskmate_ui.h。此处只维护 g_screen_brightness
 * 基准 + 落盘。 */

static void settings_brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(slider);
    lv_obj_t *val_lbl = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (val_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (int)v);
        lv_label_set_text(val_lbl, buf);
    }
    g_screen_brightness = (int)v;   /* 2026-08-23 P115：同步共享背光状态（AI 语音命令基准） */
    /* 2026-09-10 P206：亮度落盘（重启保持手动值；开机会恢复，见 luncher_dm.c） */
    dm_health_cfg_set_int("screen_brightness", (int)v);
    /* P180：自动亮度开着时手动拖滑块 → 切回手动（与手机"拖亮度关自动"一致） */
    if (dm_als_auto_enabled())
    {
        dm_health_cfg_set_int(DM_ALS_AUTO_BRIGHT_CFG, 0);
        dm_als_set_auto_bright(false);
        LV_LOG_USER("[auto-bright] manual override, auto off");
    }
    dm_brightness_write((int)v);
}

/* P180：自动亮度开关——开=引擎按环境光 lux 驱动背光（每秒平滑 ±3%），
 * 夜览开启时自动让位；关=恢复手动基准 g_screen_brightness。
 * 落盘 auto_brightness，开机恢复；拖亮度滑块会切回手动。 */
static void settings_autobright_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dm_health_cfg_set_int(DM_ALS_AUTO_BRIGHT_CFG, on ? 1 : 0);
    dm_als_set_auto_bright(on);
}

/* Volume slider → 播放器音量（0-100）。
 * 2026-08-08 修复：此前音量滑块 cb=NULL 无任何回调 → 拖动无效（"不能用"）；
 * 现改 g_music_volume（dm_sound_write 下一包自动按新音量衰减），并同步
 * 播放器右侧音量数值（若播放器页已创建）。
 * Phase 1 拆分：g_music_volume / music_vol_lbl 正式定义在 deskmate_ui.c
 * （music 区），本文件经 deskmate_ui.h extern 引用，行为不变。 */
static void settings_volume_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(slider);
    lv_obj_t *val_lbl = (lv_obj_t *)lv_obj_get_user_data(slider);
    if (val_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (int)v);
        lv_label_set_text(val_lbl, buf);
    }
    g_music_volume = (int)v;
    /* 2026-09-14：落盘供 AI 语音同步 + 重启保持；走 800ms 防抖（滑块拖动
     * 高频 VALUE_CHANGED，直写会反复整写 dm_health.cfg 刷 flash） */
    music_volume_persist_debounced();
    if (music_vol_lbl)
        lv_label_set_text_fmt(music_vol_lbl, "%d", g_music_volume);
    printf("[music] settings volume=%d\n", g_music_volume);
}

/* Auto-Lock → cycles lock timeout; persists into g_idle_timeout */
static const int g_autolock_secs[]  = { 30, 60, 300, 0 };      /* 0 = never */
static const char *g_autolock_labels[] = { "30 秒", "1 分钟", "5 分钟", "永不" };
static int g_autolock_idx = 0;

static void settings_autolock_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_t *val_lbl = (lv_obj_t *)lv_obj_get_user_data(row);
    if (!val_lbl) return;

    g_autolock_idx = (g_autolock_idx + 1) % 4;
    lv_label_set_text(val_lbl, g_autolock_labels[g_autolock_idx]);
    g_idle_timeout = g_autolock_secs[g_autolock_idx];
    reset_idle_timer();
    LV_LOG_USER("Auto-Lock → %s (%ds)", g_autolock_labels[g_autolock_idx],
                g_idle_timeout);
}

/* ═══════════ 夜览（P179）：手动暗光模式 ═══════════
 * 开关 on → 立即压暗到 night_shift_brightness（默认 10%）；off → 立即
 * 恢复用户亮度 g_screen_brightness。档位/开关落盘 /data/dm_health.cfg
 * （night_shift / night_shift_brightness）。
 * 屏为 BOE MIPI 面板，无暖色温寄存器——夜览只做亮度。 */
#define NIGHT_SHIFT_CFG       "night_shift"
#define NIGHT_SHIFT_BRIGHT_CFG "night_shift_brightness"

static int  g_ns_bright;
static bool g_ns_enabled;
static bool g_ns_loaded;
static bool g_ns_applied;               /* 开机已按 cfg 恢复过 */

static void night_shift_reload_cfg(void)
{
    g_ns_bright  = dm_health_cfg_get_int(NIGHT_SHIFT_BRIGHT_CFG, 10);
    g_ns_enabled = dm_health_cfg_get_int(NIGHT_SHIFT_CFG, 0) != 0;
    g_ns_loaded  = true;
}

/* Settings「夜览」开关：落盘 + 立即生效 */
static void settings_night_shift_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_ns_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dm_health_cfg_set_int(NIGHT_SHIFT_CFG, g_ns_enabled ? 1 : 0);
    /* 开→立即压暗；关→无条件恢复用户亮度（33 版按时段应用致"关不回去"） */
    dm_brightness_write(g_ns_enabled ? g_ns_bright : g_screen_brightness);
    g_ns_applied = true;
    LV_LOG_USER("Night Shift → %s (bright=%d%%)", g_ns_enabled ? "on" : "off",
                g_ns_enabled ? g_ns_bright : g_screen_brightness);
}

/* P180：夜览开关状态（自动亮度引擎仲裁用——夜览开启时自动亮度让步） */
bool dm_night_shift_enabled(void)
{
    if (!g_ns_loaded)
        night_shift_reload_cfg();
    return g_ns_enabled;
}

/* 开机恢复（挂 luncher_dm update_time_cb，仅在首次 tick 生效一次）：
 * cfg 开着时把背光恢复成夜览档——下次开机仍保持上次的夜览状态。 */
void dm_night_shift_hook(int hour)
{
    (void)hour;
    if (!g_ns_loaded)
        night_shift_reload_cfg();
    if (g_ns_applied)
        return;
    g_ns_applied = true;
    if (g_ns_enabled)
        dm_brightness_write(g_ns_bright);
}

/* ═══════════ 存储（P179）：TF 卡真实容量 ═══════════
 * statfs("/sdcard") 读取；无卡/挂载失败 → 显示「未插入」+ 进度条清零。 */
static void storage_update(lv_obj_t *val_lbl, lv_obj_t *bar)
{
    struct statfs f;
    char buf[32];

    if (statfs("/sdcard", &f) != 0 || f.f_bsize <= 0 || f.f_blocks <= 0) {
        lv_label_set_text(val_lbl, "未插入");
        if (bar)
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        return;
    }
    uint64_t total = (uint64_t)f.f_blocks * f.f_bsize;
    uint64_t free  = (uint64_t)f.f_bavail * f.f_bsize;
    uint64_t used  = total > free ? total - free : 0;
    /* LVGL lv_snprintf 不保证 %f（LV_SPRINTF_USE_FLOAT 需另开）→ 整数 MB/GB */
    uint64_t mb  = used / (1024u * 1024u);
    uint64_t tmb = total / (1024u * 1024u);
    if (tmb >= 1024u) {
        if (mb < 1024u) {
            /* 用量 <1GB：用 MB 显示，避免截断成「已用 0 GB」（37MB 曾被显示为 0） */
            lv_snprintf(buf, sizeof(buf), "已用 %d MB / 共 %d GB",
                        (int)mb, (int)(tmb / 1024u));
        } else {
            /* 用量 >=1GB：GB + 1 位小数 */
            lv_snprintf(buf, sizeof(buf), "已用 %d.%d / 共 %d.%d GB",
                        (int)(mb / 1024u), (int)((mb % 1024u) * 10u / 1024u),
                        (int)(tmb / 1024u), (int)((tmb % 1024u) * 10u / 1024u));
        }
    } else {
        lv_snprintf(buf, sizeof(buf), "已用 %d / 共 %d MB",
                    (int)mb, (int)tmb);
    }
    lv_label_set_text(val_lbl, buf);
    if (bar) {
        /* 2026-09-14：大卡上几百 MB 不足 1% 会被取整成 0 → 白条“没关联”。
         * 四舍五入 + used>0 保底 1%，有占用必见绿。 */
        int pct = total > 0 ? (int)((used * 100 + total / 2) / total) : 0;
        if (used > 0 && pct == 0) pct = 1;
        if (pct > 100) pct = 100;
        printf("[storage] total=%lluMB used=%lluMB pct=%d%%\n",
               (unsigned long long)(total / (1024u * 1024u)),
               (unsigned long long)(used / (1024u * 1024u)), pct);
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    }
}

/* Settings → WiFi 开关（2026-08-08 接线 RTL8733BS） */
static void settings_wifi_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    /* wifijianyi.md 防狂飙：驱动操作期间禁用自身，防 1s 连点 10 次 */
    lv_obj_add_state(sw, LV_STATE_DISABLED);
    dm_net_wifi_set(on ? 1 : 0);
    dm_net_status_refresh();
    lv_obj_clear_state(sw, LV_STATE_DISABLED);
}

/* Settings WiFi 行点击 → 打开 WiFi 管理子页
 * （wifijianyi.md：switch 已自消费 CLICKED，此处 row 收到的 CLICKED
 * 一定是点击行本体，无需再 target 过滤） */
static void settings_wifi_row_open_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("WiFi row clicked!");
    show_subpage("WiFi");
}

static void settings_bt_row_open_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("BT row clicked!");
    show_subpage("Bluetooth");
}

/* ═══════════ 城市（2026-09-10 A 方案）═══════════
 * 板无 GPS，天气只需城市精度：点按循环切换（autolock 同款交互），
 * 落盘 weather_city_idx；一切换立即踢一轮天气 fetch，首页卡即刻刷新，
 * ai_agent 下次问天气读同一城市的落盘数据。默认惠州。 */
static void settings_city_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_t *val_lbl = (lv_obj_t *)lv_obj_get_user_data(row);
    if (!val_lbl) return;

    int idx = (dm_city_cur_idx() + 1) % dm_city_count();
    dm_city_set_idx(idx);
    lv_label_set_text(val_lbl, dm_city_get(idx)->name);
    reset_idle_timer();
    weather_kick_fetch();   /* 即时刷新，不等下一轮周期 */
    LV_LOG_USER("City → %s", dm_city_get(idx)->name);
}

/* ═══════════ 日期与时间入口（2026-09-10 P211：独立 Datetime 子页，宠物同款） ═══════════ */

static void settings_datetime_row_open_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Datetime row clicked!");
    show_subpage("Datetime");
}

/* close_subpage 清理：手配面板已搬 Datetime 子页（自带清理），此处保留空壳
 * （deskmate_ui.c close_subpage 有调用，删函数要动两处，不值）。 */
void ui_settings_close_cleanup(void)
{
}

/* Settings → UART 调试工具（2026-08-27 新增）：打开串口调试子页 */
static void settings_uart_dbg_open_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("UART debug row clicked!");
    show_subpage("UART");
}

/* Settings → 闹钟（2026-09-10 P206：状态栏时钟同入口） */
static void settings_alarm_row_open_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Alarm row clicked!");
    show_subpage("Alarm");
}

/* ═══════════ 宠物入口（2026-09-10 P206：独立 Pet 子页，WiFi 同款） ═══════════ */

static void settings_pet_row_open_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Pet row clicked!");
    show_subpage("Pet");
}

void ui_settings_create(lv_obj_t *parent)
{
    /* Scrollable column container */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);

    /* iOS-style large title */
    lv_obj_t *big_title = lv_label_create(parent);
    lv_label_set_text(big_title, "设置");
    lv_obj_set_style_text_font(big_title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(big_title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_pad_bottom(big_title, 12, 0);

    /* ═══════ NETWORK ═══════ */
    settings_section_label(parent, "网络");
    lv_obj_t *net = settings_group_card(parent);
    /* 2026-08-08 接线：初始状态读真实驱动状态（wifi_is_up / bt state） */
    int wifi_up = dm_net_wifi_is_up();
    lv_obj_t *wifi_row = settings_row_switch_cb(net, LV_SYMBOL_WIFI, COL_BLUE,
                                                "Wi-Fi", wifi_up > 0,
                                                settings_wifi_cb);
    /* 点击 WiFi 行（非开关处）→ 进入 WiFi 管理子页（手机式 SSID 清单） */
    lv_obj_add_flag(wifi_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_row, settings_wifi_row_open_cb,
                        LV_EVENT_CLICKED, NULL);
    settings_add_separator(net);
    int bt_on = dm_net_bt_is_on();
    lv_obj_t *bt_row = settings_row_switch_cb(net, LV_SYMBOL_BLUETOOTH, COL_BLUE,
                                              "蓝牙", bt_on, settings_bt_cb);
    /* 点击蓝牙行（非开关处）→ 进入蓝牙管理子页（手机式设备清单） */
    lv_obj_add_flag(bt_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bt_row, settings_bt_row_open_cb, LV_EVENT_CLICKED, NULL);

    /* ═══════ DISPLAY & BRIGHTNESS ═══════ */
    settings_section_label(parent, "显示与亮度");
    lv_obj_t *disp = settings_group_card(parent);
    settings_row_slider(disp, LV_SYMBOL_TINT, COL_BLUE, "亮度",
                        g_screen_brightness, settings_brightness_cb);
    settings_add_separator(disp);
    /* P180：自动亮度（环境光 lux 驱动，夜览让位；拖滑块自动切回手动） */
    settings_row_switch_cb(disp, LV_SYMBOL_BARS, COL_GREEN, "自动亮度",
                           dm_als_auto_enabled(), settings_autobright_cb);
    settings_add_separator(disp);
    lv_obj_t *al_row = settings_row_value(disp, LV_SYMBOL_KEYBOARD, COL_BLUE,
                                          "自动锁定", g_autolock_labels[g_autolock_idx], true);
    lv_obj_add_event_cb(al_row, settings_autolock_cb, LV_EVENT_CLICKED, NULL);
    settings_add_separator(disp);
    /* P179：夜览（手动暗光）开关即生效，持久化 dm_health.cfg */
    if (!g_ns_loaded)
        night_shift_reload_cfg();
    settings_row_switch_cb(disp, LV_SYMBOL_EYE_OPEN, COL_ORANGE, "夜览",
                           g_ns_enabled, settings_night_shift_cb);

    /* ═══════ SOUND ═══════ */
    settings_section_label(parent, "声音");
    lv_obj_t *snd = settings_group_card(parent);
    settings_row_slider(snd, LV_SYMBOL_VOLUME_MAX, COL_RED, "音量",
                        g_music_volume, settings_volume_cb);

    /* ═══════ GENERAL ═══════ */
    settings_section_label(parent, "通用");
    lv_obj_t *gen = settings_group_card(parent);
    /* 2026-09-10 P211：日期与时间独立子页（宠物同款嵌套），本页只留一行入口；
     * 行值=自动/手动（返回本页不重建，改完以子页内显示为准，宠物改名同款惯例）。 */
    lv_obj_t *dt_row = settings_row_value(gen, LV_SYMBOL_REFRESH, COL_BLUE,
                                          "日期与时间",
                                          dm_time_auto_enabled() ? "自动" : "手动",
                                          true);
    lv_obj_add_event_cb(dt_row, settings_datetime_row_open_cb,
                        LV_EVENT_CLICKED, NULL);
    settings_add_separator(gen);
    /* 2026-09-10 P206：闹钟入口（子页，时钟点按同入口） */
    lv_obj_t *alarm_row = settings_row_value(gen, LV_SYMBOL_BELL, COL_ORANGE,
                                             "闹钟", "设置提醒", true);
    lv_obj_add_event_cb(alarm_row, settings_alarm_row_open_cb,
                        LV_EVENT_CLICKED, NULL);
    settings_add_separator(gen);
    /* 2026-09-10 城市（天气定位用，默认惠州；点按循环切换） */
    lv_obj_t *city_row = settings_row_value(gen, LV_SYMBOL_HOME, COL_GREEN,
                                            "城市", dm_city_cur()->name,
                                            true);
    lv_obj_add_event_cb(city_row, settings_city_cb, LV_EVENT_CLICKED, NULL);
    settings_add_separator(gen);
    /* UART 调试工具（2026-08-27）：接收其他设备 debug log 的串口窗口。
     * 2026-08-27 v2：value 显示端口号 UART1（PD21/PD22 mux4，P124 迁回：
     * 外部调试设备接 BSN20 电平转换外设侧接口） */
    lv_obj_t *uart_dbg_row = settings_row_value(gen, LV_SYMBOL_SETTINGS,
                                                COL_BLUE, "UART 调试工具",
                                                "UART1", true);
    lv_obj_add_event_cb(uart_dbg_row, settings_uart_dbg_open_cb,
                        LV_EVENT_CLICKED, NULL);

    /* ═══════ PET ═══════ */
    settings_section_label(parent, "宠物");
    lv_obj_t *pet = settings_group_card(parent);
    /* 2026-09-10 P206：整区搬 Pet 嵌套子页（WiFi/蓝牙同款），本页只留一行入口 */
    const dm_pet_data_t *pet_pd = dm_pet_get_data();
    const char *pet_entry_name = (pet_pd && pet_pd->name[0]) ? pet_pd->name : "旺财";
    lv_obj_t *pet_row = settings_row_value(pet, LV_SYMBOL_IMAGE, COL_GREEN,
                                           "宠物", pet_entry_name, true);
    lv_obj_add_event_cb(pet_row, settings_pet_row_open_cb, LV_EVENT_CLICKED, NULL);

    /* ═══════ STORAGE ═══════ */
    settings_section_label(parent, "存储");
    lv_obj_t *stor = settings_group_card(parent);
    /* P179：TF 卡真实容量（statfs /sdcard）+ 使用进度条 */
    lv_obj_t *stor_row = settings_row_value(stor, LV_SYMBOL_SD_CARD, COL_GREEN,
                                            "内存卡存储", "读取中…", false);

    /* Storage bar embedded in a custom row */
    {
        lv_obj_t *bar_row = make_clean_cont(stor);
        lv_obj_set_size(bar_row, lv_pct(100), DM(20));
        lv_obj_set_style_pad_left(bar_row, 40, 0);  /* indent past icon */
        lv_obj_set_style_pad_right(bar_row, 8, 0);
        lv_obj_set_style_pad_bottom(bar_row, 8, 0);

        lv_obj_t *bar = lv_bar_create(bar_row);
        lv_obj_set_size(bar, lv_pct(100), DM(6));
        lv_obj_set_style_radius(bar, DM(3), 0);
        /* P179：背景轨 0xE5E5EA 不透明（原 0x000000@10% 在白色卡片上≈隐形
         * 白条），indicator COL_GREEN 与存储卡绿色徽标一致 */
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xE5E5EA), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_GREEN),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, DM(3), LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);

        storage_update((lv_obj_t *)lv_obj_get_user_data(stor_row), bar);
    }

    /* ═══════ ABOUT ═══════ */
    settings_section_label(parent, "关于");
    lv_obj_t *about = settings_group_card(parent);
    settings_row_value(about, LV_SYMBOL_HOME, COL_BLUE, "设备名称",
                       "DesktopMate", false);
    settings_add_separator(about);
    settings_row_value(about, LV_SYMBOL_SETTINGS, COL_SEC, "软件版本",
                       "Vela OS " DM_VERSION, false);
    settings_add_separator(about);
    /* 2026-09-11：产品型号 YNM-3000 + 硬件平台真实板型，评委对得上硬件 */
    settings_row_value(about, LV_SYMBOL_WARNING, COL_SEC, "型号", "YNM-3000",
                       false);
    settings_add_separator(about);
    settings_row_value(about, LV_SYMBOL_LIST, COL_SEC, "硬件平台",
                       "Allwinner R528S3-Gemini-S1", false);

    /* Bottom padding for scroll comfort */
    lv_obj_t *bottom_pad = make_clean_cont(parent);
    lv_obj_set_size(bottom_pad, DM(1), DM(24));
}
