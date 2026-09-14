/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/luncher_dm.c
 * Mini launcher application for OpenVela with LED control functionality.
 * (luncher_dm: copy of luncher, used as base for new app)
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   `http://www.apache.org/licenses/LICENSE-2.0` 
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_LUNCHER_APP

#include <unistd.h>
#include <sys/boardctl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h> 

/* 包含LED控制库头文件 */
#include "lv_demo_panel_rgb_control.h"

/* Desktop Mate UI (ported from lv_port_linux simulator) */
#include "deskmate_ui.h"
#include "dm_health.h"   /* DesktopMate 健康模块：在座感知 + 双提醒状态机 */
#include "dm_ld2410b.h"  /* 2026-08-23：LD2410B 毫米波雷达替代 LTR553 接近传感器 */
#include "dm_health_cfg.h" /* 2026-08-27：ld2410_seat_cm 在座距离阈值配置化 */
#include "dm_voice.h"      /* 2026-08-28 Voice Director：语音决策层 */
#include "dm_als.h"        /* P153：LTR553 ALS 环境光（锁屏亮度显示 + 暗光开灯提醒） */
#include "pet_core.h"      /* 2026-09-10 P206：宠物引擎开机初始化（先有鸡先有蛋修复，见下） */
#include "dm_alarm.h"      /* 2026-09-10 P206：闹钟引擎（读盘+1s检查timer） */
#include "dm_weather.h"    /* T1：开机用上次落盘时间兜底（无 RTC 电池） */
#include "dm_screenshot.h" /* 2026-09-11：LRADC 按键触发全屏截图存 TF 卡（本地固件） */

/* 包含传感器相关头文件 */
#include <poll.h>
#include <sensor/temp.h>
#include <sensor/humi.h>
#include <uORB/uORB.h>
#include <syslog.h> 

/* 条件包含 libuv 头文件 */
#ifdef CONFIG_LV_USE_NUTTX_LIBUV
#include <uv.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Should we perform board-specific driver initialization? There are two
 * ways that board initialization can occur:  1) automatically via
 * board_late_initialize() during bootupif CONFIG_BOARD_LATE_INITIALIZE
 * or 2).
 * via a call to boardctl() if the interface is enabled
 * (CONFIG_BOARDCTL=y).
 * If this task is running as an NSH built-in application, then that
 * initialization has probably already been performed otherwise we do it
 * here.
 */

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
static void lv_nuttx_uv_loop(uv_loop_t *loop, lv_nuttx_result_t *result)
{
  lv_nuttx_uv_t uv_info;
  void *data;

  uv_loop_init(loop);

  lv_memset(&uv_info, 0, sizeof(uv_info));
  uv_info.loop = loop;
  uv_info.disp = result->disp;
  uv_info.indev = result->indev;
#ifdef CONFIG_UINPUT_TOUCH
  uv_info.uindev = result->utouch_indev;
#endif

  data = lv_nuttx_uv_init(&uv_info);
  uv_run(loop, UV_RUN_DEFAULT);
  lv_nuttx_uv_deinit(&data);
}
#endif

/****************************************************************************
 * 配置参数
 ****************************************************************************/
/* 1920x1200 BOE 横屏: 由 320x240 参考设计按比例放大 (宽 6x / 高 5x) */
#define SCREEN_WIDTH   1920
#define SCREEN_HEIGHT  1200

/****************************************************************************
 * 传感器相关定义和结构体
 ****************************************************************************/
#define LV_DEMO_POLLFD_NUM 2 /* 温度 + 湿度（2026-08-23：prox 已由 LD2410B 替代） */

/* 传感器订阅者结构体 */
typedef struct {
  int temperature_sub;
  int humidity_sub;
  struct pollfd fds[LV_DEMO_POLLFD_NUM];
  bool initialized;
} sensor_subscriber;

static sensor_subscriber sensor_sub = {
    .temperature_sub = -1,
    .humidity_sub = -1,
    .initialized = false
};

/* 传感器更新选项枚举 */
typedef enum {
    LV_DEMO_UPDATE_TEMPERATURE = 0x01,
    LV_DEMO_UPDATE_HUMIDITY    = 0x02,
    LV_DEMO_UPDATE_ALL         = 0x03  // 所有传感器的位或
} lv_demo_sensor_select_t;

/****************************************************************************
 * 全局变量声明
 ****************************************************************************/

/* LED 初始化状态（luncher_dm_main 使用） */
static bool light_initialized = false;

/* 中文支持字体变量 */
static lv_font_t *g_misans_normal_11;
static lv_font_t *g_misans_normal_12;
static lv_font_t *g_misans_normal_16;

/* Desktop Mate 7-tier fonts (exported, consumed by deskmate_ui.c) */
lv_font_t *dm_font_title;    /* big clock / hero numbers  (MiSans 110) */
lv_font_t *dm_font_clock;    /* P151: status-bar time 专用档 (MiSans 56) */
lv_font_t *dm_font_icon;     /* status-bar time, dock symbols (MiSans 76) */
lv_font_t *dm_font_symbol;   /* dock app glyphs ×1.5 (MiSans 140) */
lv_font_t *dm_font_body;     /* card titles / settings labels (MiSans 56) */
lv_font_t *dm_font_label;    /* dock labels / secondary text (MiSans 66) */
lv_font_t *dm_font_caption;  /* helper text / captions (MiSans 34) */

/* Symbol render scale (1/256 units): board uses full-size 140px MiSans
 * glyphs (no scaling), simulator doubles montserrat_48 to match. */
int dm_symbol_scale = 256;

/* LED状态 */
static bool g_led_is_on = false;
static int32_t g_led_brightness = 100;

/* 时间和日期主题（仅传感器温度/湿度主题在用；桌面时钟由 ui_home clock_update_cb 渲染） */
static lv_subject_t temperature_subject;  // 温度
static lv_subject_t humidity_subject;     // 湿度

/****************************************************************************
 * 函数声明
 ****************************************************************************/

/* 时间相关函数 */
static void update_time_cb(lv_timer_t *timer);

/* LED控制适配函数 */
static led_error_t led_adapter_init(void);
static led_error_t led_adapter_deinit(void);
static led_error_t led_adapter_on(void);
static led_error_t led_adapter_off(void);
static led_error_t led_adapter_set_brightness(int32_t brightness);
static void led_adapter_diagnose(void);

/* 传感器相关函数 */
static int lv_demo_init_sensor_subscriptions(void);
static int lv_demo_update_sensor_data(lv_demo_sensor_select_t sensors_to_update, void *ctx);
static void update_sensor_cb(lv_timer_t *timer);
static int init_sensors(void);

/* 字体初始化函数 */
static void init_fonts(void);

/****************************************************************************
 * LED控制适配函数实现
 ****************************************************************************/

static led_error_t led_adapter_init(void)
{
    led_error_t err = dm_led_controller_init();
    if (err != LED_SUCCESS) {
        LV_LOG_ERROR("LED controller init failed: %s",
                     dm_led_get_error_string(err));
        return err;
    }
    /* 初始化为关闭状态，白色，100%亮度 */
    g_led_is_on = false;
    g_led_brightness = 100;
    /* 设置初始颜色但不开启 */
    dm_led_set_color(LED_COLOR_WHITE);
    dm_led_set_brightness(g_led_brightness);
    LV_LOG_INFO("LED adapter initialized (brightness=%d%%)", g_led_brightness);
    return LED_SUCCESS;
}

static led_error_t led_adapter_deinit(void)
{
    /* 先关闭LED */
    led_adapter_off();
    /* 然后清理资源 */
    led_error_t err = dm_led_controller_deinit();
    if (err != LED_SUCCESS) {
        LV_LOG_ERROR("LED adapter deinit error: %s", dm_led_get_error_string(err));
    }
    LV_LOG_INFO("LED adapter deinitialized");
    return err;
}

static led_error_t led_adapter_on(void)
{
    led_error_t err = dm_led_on();
    if (err == LED_SUCCESS) {
        g_led_is_on = true;
        LV_LOG_INFO("LED adapter: ON (brightness=%d%%)", g_led_brightness);
    } else {
        LV_LOG_ERROR("LED adapter: Failed to turn ON: %s", dm_led_get_error_string(err));
    }
    return err;
}

static led_error_t led_adapter_off(void)
{
    led_error_t err = dm_led_off();
    if (err == LED_SUCCESS) {
        g_led_is_on = false;
        LV_LOG_INFO("LED adapter: OFF");
    } else {
        LV_LOG_ERROR("LED adapter: Failed to turn OFF: %s", dm_led_get_error_string(err));
    }
    return err;
}

static led_error_t led_adapter_set_brightness(int32_t brightness)
{
    if (brightness < 0 || brightness > 100) {
        LV_LOG_ERROR("LED adapter: Invalid brightness %d", brightness);
        return LED_ERROR_INVALID_PARAM;
    }
    g_led_brightness = brightness;
    /* 应用亮度设置 */
    led_error_t err = dm_led_set_brightness(brightness);
    if (err == LED_SUCCESS) {
        LV_LOG_INFO("LED adapter: Brightness set to %d%%", brightness);
        
        /* 如果LED当前是开启的，需要重新应用设置 */
        if (g_led_is_on) {
            /* 临时关闭再开启以应用新亮度 */
            dm_led_off();
            usleep(10000);  // 10ms延迟
            dm_led_on();
        }
    } else {
        LV_LOG_ERROR("LED adapter: Failed to set brightness: %s", 
                     dm_led_get_error_string(err));
    }
    return err;
}

static void led_adapter_diagnose(void)
{
    LV_LOG_INFO("=== LED Adapter Diagnosis ===");
    /* 检查初始化状态 */
    bool led_state = false;
    dm_led_is_on(&led_state);
    LV_LOG_INFO("LED is on: %s", led_state ? "YES" : "NO");
    LV_LOG_INFO("Current brightness: %d%%", g_led_brightness);
    /* 获取当前颜色信息 */
    led_status_t status;
    if (dm_led_get_status(&status) == LED_SUCCESS) {
        LV_LOG_INFO("Current color: 0x%06X (%s)",
                    status.color, dm_led_get_color_name(status.color));
        LV_LOG_INFO("Current mode: %d", status.mode);
    }
    LV_LOG_INFO("=== End Diagnosis ===");
}

/****************************************************************************
 * 字体初始化函数
 ****************************************************************************/
static void init_fonts(void)
{
    #ifdef LV_USE_FREETYPE
    /* 加载MiSans-Normal.ttf字体 */
    g_misans_normal_11 = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                               LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 66,
                                               LV_FREETYPE_FONT_STYLE_NORMAL);
    g_misans_normal_12 = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                               LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 72,
                                               LV_FREETYPE_FONT_STYLE_NORMAL);
    g_misans_normal_16 = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                               LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 96,
                                               LV_FREETYPE_FONT_STYLE_NORMAL);
    /* Desktop Mate 六档字体——字号对齐模拟器观感（montserrat 48/36/24/30/14）。
     * 原 110/76/56/66/34 是 800x480→1920x1200 ×2.3 过度标定：模拟器证明 1920x1200
     * 下 48/36/24/30/14 观感正确，板端大字导致字体过大、dock（66px 标签行高~92px）
     * 撑爆 DOCK_H=240 显示不全/比例失调，故板端改为与模拟器同值。
     * 符号修复：MiSans 不含 LVGL 符号码点（U+F000 私有区）且 freetype 字体 fallback
     * 默认为 NULL，LV_SYMBOL_*（播放键/返回箭头/settings 图标）全部显示口口；每个
     * MiSans 字体 fallback → 同字号 montserrat 内置字体（自带全部 LV_SYMBOL 字形，
     * 模拟器符号正常即靠它，两端渲染路径一致）。 */
    dm_font_title = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 48,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    /* P151：状态栏时间专用档 56px（283PPI ≈ 5.0mm 字高，达 8 寸屏正文可读
     * 标准；独立档位避免 48px title 被 26 处引用波及）。纯数字+冒号，
     * MiSans 全覆盖，无需符号 fallback。 */
    dm_font_clock = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 56,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    dm_font_symbol = lv_freetype_font_create("/resource/fonts/fa-solid-900.ttf",
                                             LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 140,
                                             LV_FREETYPE_FONT_STYLE_NORMAL); /* 备用：UI 当前未引用 FONT_SYMBOL */
    dm_font_icon = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                           LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 36,
                                           LV_FREETYPE_FONT_STYLE_NORMAL);
    dm_font_body = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                           LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 24,
                                           LV_FREETYPE_FONT_STYLE_NORMAL);
    dm_font_label = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 30,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    dm_font_caption = lv_freetype_font_create("/resource/fonts/MiSans-Normal.ttf",
                                              LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 14,
                                              LV_FREETYPE_FONT_STYLE_NORMAL);
    /* LV_SYMBOL_* fallback → fa-solid（WIFI/PLAY/BATTERY 等 57/61 符号）
     * → fa-brands（BLUETOOTH U+F293 在 FA6 只收录于 brands 字重）
     * → montserrat 兜底。
     * 注意：勿直接用 montserrat 做符号 fallback——LVGL 官方内置 montserrat
     * 的符号字形不完整（62 个符号仅前 18 个有真实位图，其余 bitmap_index
     * 越界读数组外内存，渲染为空白/垃圾），但 cmap 有映射导致 fallback
     * 链不会继续走到外部字体，WIFI/蓝牙因此不显示。 */
    static lv_font_t *fs_sym_48, *fs_sym_36, *fs_sym_24, *fs_sym_30, *fs_sym_14;
    static lv_font_t *fb_sym_48, *fb_sym_36, *fb_sym_24, *fb_sym_30, *fb_sym_14;
#define DM_SYMBOL_FALLBACK(dmfont, sz) do { \
        lv_font_t *fs = lv_freetype_font_create("/resource/fonts/fa-solid-900.ttf", \
                LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (sz), LV_FREETYPE_FONT_STYLE_NORMAL); \
        lv_font_t *fb = lv_freetype_font_create("/resource/fonts/fa-brands-400.ttf", \
                LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (sz), LV_FREETYPE_FONT_STYLE_NORMAL); \
        if ((dmfont) && fs) { \
            (dmfont)->fallback = fs; \
            fs->fallback = (fb) ? fb : &lv_font_montserrat_##sz; \
        } \
    } while (0)
    DM_SYMBOL_FALLBACK(dm_font_title, 48);
    DM_SYMBOL_FALLBACK(dm_font_icon, 36);
    DM_SYMBOL_FALLBACK(dm_font_body, 24);
    DM_SYMBOL_FALLBACK(dm_font_label, 30);
    DM_SYMBOL_FALLBACK(dm_font_caption, 14);
#undef DM_SYMBOL_FALLBACK
    /* 检查字体加载是否成功，如果失败则回退到默认字体 */
    if(g_misans_normal_11 == NULL) {
        g_misans_normal_11 = &lv_font_montserrat_12;
        LV_LOG_WARN("Failed to load MiSans-Normal 11, fallback to default font");
    }
    if(g_misans_normal_12 == NULL) {
        g_misans_normal_12 = &lv_font_montserrat_12;
        LV_LOG_WARN("Failed to load MiSans-Normal 12, fallback to default font");
    }
    if(g_misans_normal_16 == NULL) {
        g_misans_normal_16 = &lv_font_montserrat_16;
        LV_LOG_WARN("Failed to load MiSans-Normal 16, fallback to default font");
    }
    if(dm_font_title == NULL) {
        dm_font_title = g_misans_normal_16;
        LV_LOG_WARN("Failed to load dm_font_title, fallback to g_misans 96");
    }
    if(dm_font_clock == NULL) {
        dm_font_clock = dm_font_title;
        LV_LOG_WARN("Failed to load dm_font_clock, fallback to dm_font_title");
    }
    if(dm_font_symbol == NULL) {
        dm_font_symbol = g_misans_normal_16;
        LV_LOG_WARN("Failed to load dm_font_symbol, fallback to g_misans 96");
    }
    if(dm_font_icon == NULL) {
        dm_font_icon = g_misans_normal_12;
        LV_LOG_WARN("Failed to load dm_font_icon, fallback to g_misans 72");
    }
    if(dm_font_body == NULL) {
        dm_font_body = g_misans_normal_12;
        LV_LOG_WARN("Failed to load dm_font_body, fallback to g_misans 72");
    }
    if(dm_font_label == NULL) {
        dm_font_label = g_misans_normal_11;
        LV_LOG_WARN("Failed to load dm_font_label, fallback to g_misans 66");
    }
    if(dm_font_caption == NULL) {
        dm_font_caption = g_misans_normal_11;
        LV_LOG_WARN("Failed to load dm_font_caption, fallback to g_misans 66");
    }
    #else
    /* 如果不支持FreeType，则使用cjk中文支持字体 */
    g_misans_normal_11 = &lv_font_simsun_16_cjk;
    g_misans_normal_12 = &lv_font_simsun_16_cjk;
    g_misans_normal_16 = &lv_font_simsun_16_cjk;
    dm_font_title = &lv_font_simsun_16_cjk;
    dm_font_symbol = &lv_font_simsun_16_cjk;
    dm_font_icon = &lv_font_simsun_16_cjk;
    dm_font_body = &lv_font_simsun_16_cjk;
    dm_font_label = &lv_font_simsun_16_cjk;
    dm_font_caption = &lv_font_simsun_16_cjk;
    #endif
}

/****************************************************************************
 * 更新时间定时器回调
 ****************************************************************************/
static void update_time_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    /* 获取系统实时时间 */
    time_t now;
    struct tm *utc_time;
    struct tm local_time;
    time(&now);
    utc_time = gmtime(&now);
    if (utc_time == NULL) {
        LV_LOG_ERROR("Failed to get UTC time");
        return;
    }
    /* 复制 UTC 时间并按东八区 +8h 归一（mktime 处理跨日/跨月进位） */
    local_time = *utc_time;
    local_time.tm_hour += 8;
    mktime(&local_time);
    /* P179：夜览开机状态恢复（cfg 开着时把背光恢复成夜览档，仅首 tick） */
    dm_night_shift_hook(local_time.tm_hour);
}

/****************************************************************************
 * 传感器初始化函数
 ****************************************************************************/
static int lv_demo_init_sensor_subscriptions(void) {
  LV_LOG_INFO("Initializing temperature and humidity sensor subscriptions...");
  int pollfd_count = 0;
  bool any_success = false;
  /* 初始化温度传感器订阅 */
  sensor_sub.temperature_sub = orb_subscribe_multi(ORB_ID(sensor_temp), 0);
  if (sensor_sub.temperature_sub < 0) {
    LV_LOG_ERROR("Failed to subscribe to temperature topic");
  } else {
    LV_LOG_INFO("Temperature subscription initialized: fd=%d", sensor_sub.temperature_sub);
    /* 初始化温度传感器的pollfd */
    sensor_sub.fds[pollfd_count].fd = sensor_sub.temperature_sub;
    sensor_sub.fds[pollfd_count].events = POLLIN;
    pollfd_count++;
    any_success = true;
  }
  /* 初始化湿度传感器订阅 */
  sensor_sub.humidity_sub = orb_subscribe_multi(ORB_ID(sensor_humi), 0);
  if (sensor_sub.humidity_sub < 0) {
    LV_LOG_ERROR("Failed to subscribe to humidity topic");
  } else {
    LV_LOG_INFO("Humidity subscription initialized: fd=%d", sensor_sub.humidity_sub);
    /* 初始化湿度传感器的pollfd */
    sensor_sub.fds[pollfd_count].fd = sensor_sub.humidity_sub;
    sensor_sub.fds[pollfd_count].events = POLLIN;
    pollfd_count++;
    any_success = true;
  }
  /* 初始化剩余的pollfd为无效值 */
  for (int i = pollfd_count; i < LV_DEMO_POLLFD_NUM; i++) {
    sensor_sub.fds[i].fd = -1;
    sensor_sub.fds[i].events = 0;
  }
  if (any_success) {
    sensor_sub.initialized = true;
    LV_LOG_INFO("Sensor subscriptions initialized successfully: %d sensors available", pollfd_count);
    return 0;
  } else {
    LV_LOG_ERROR("Failed to initialize any sensor subscriptions");
    return -1;
  }
}

/****************************************************************************
 * 传感器数据更新函数
 ****************************************************************************/
static int lv_demo_update_sensor_data(lv_demo_sensor_select_t sensors_to_update, void *ctx) {
    LV_UNUSED(ctx); /* 如果不需要上下文，可以忽略 */
    if (!sensor_sub.initialized) {
        LV_LOG_WARN("Sensor subscriptions not initialized");
        return -1;
    }
    /* 只poll有效的文件描述符 */
    int valid_fd_count = 0;
    for (int i = 0; i < LV_DEMO_POLLFD_NUM; i++) {
        if (sensor_sub.fds[i].fd != -1) {
            valid_fd_count++;
        }
    }
    if (valid_fd_count == 0) {
        LV_LOG_WARN("No valid sensor file descriptors available");
        return 0;
    }
    int ret = poll(sensor_sub.fds, LV_DEMO_POLLFD_NUM, 100);
    if (ret < 0) {
        LV_LOG_ERROR("Failed to poll sensor data: %d", ret);
        return -1;
    }
    if (ret == 0) {
        /* 没有可读数据是正常的，不是错误 */
        return 0;
    }
    int updated = 0;
    /* 更新温度数据 */
    if ((sensors_to_update & LV_DEMO_UPDATE_TEMPERATURE) &&
        (sensor_sub.temperature_sub >= 0) &&
        (sensor_sub.fds[0].revents & POLLIN)) {
        struct sensor_temp temp_data;
        if (orb_copy(ORB_ID(sensor_temp), sensor_sub.temperature_sub, &temp_data) == OK) {
            /* 直接更新全局温度主题 */
            lv_subject_set_int(&temperature_subject, (int)(temp_data.temperature * 10));
            LV_LOG_INFO("Temperature updated: %.1f°C", temp_data.temperature);
            updated++;
        } else {
            LV_LOG_WARN("Failed to copy temperature data");
        }
    }
    /* 更新湿度数据 */
    if ((sensors_to_update & LV_DEMO_UPDATE_HUMIDITY) &&
        (sensor_sub.humidity_sub >= 0) &&
        (sensor_sub.fds[1].revents & POLLIN)) {
        struct sensor_humi humi_data;
        if (orb_copy(ORB_ID(sensor_humi), sensor_sub.humidity_sub, &humi_data) == OK) {
            /* 直接更新全局湿度主题 */
            lv_subject_set_int(&humidity_subject, (int)(humi_data.humidity * 10));
            LV_LOG_INFO("Humidity updated: %.1f%%", humi_data.humidity);
            updated++;
        } else {
            LV_LOG_WARN("Failed to copy humidity data");
        }
    }
    /* 2026-08-23：prox 数据源已切换 LD2410B（dm_prox_get_cm 改读串口），
     * LTR553 接近传感器订阅/轮询整体移除（见 dm_ld2410b.c） */
    //syslog(LOG_DEBUG, "Sensors updated: %d", updated);
    return updated;
}

/****************************************************************************
 * dm_health.c 调用：人体感应距离 cm
 * 2026-08-23：数据源从 LTR553 接近传感器切换为 LD2410B 毫米波雷达。
 *   - 有人（状态≠NONE）且 探测距离 ≤ 在座阈值 → 返回 0（在座）
 *   - 有人但超出阈值 / 无人 → 返回 99（离开）
 *   - LD2410B 串口不可用 → -1（dm_health 回退纯计时模式）
 * 2026-08-27：阈值从写死 200 改为读 dm_health_cfg 的 ld2410_seat_cm
 *   （默认 150 = 1.5m/2 门，用户拍板；原 prox_seat_cm=30 死配置已删） */
int dm_prox_get_cm(void)
{
    int present = dm_ld2410b_present();
    int seat_cm = dm_health_cfg_get_int("ld2410_seat_cm",
                                        LD2410B_SEAT_CM_DEFAULT);
    int dist;

    if (present < 0)
        return -1;       /* 串口未就绪：回退纯计时 */
    if (present == 0)
        return 99;       /* 无人：离开 */

    dist = dm_ld2410b_get_dist_cm();
    if (dist >= 0 && dist <= seat_cm)
        return 0;        /* 有人且在座距离内：在座 */
    return 99;           /* 有人但超出阈值 / 距离未知：离开 */
}

/****************************************************************************
 * 传感器更新定时器回调
 ****************************************************************************/
static void update_sensor_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    /* 更新温度和湿度传感器数据 */
    int updated = lv_demo_update_sensor_data(LV_DEMO_UPDATE_ALL, NULL);
    if (updated < 0) {
        LV_LOG_ERROR("Failed to update sensor data (error code: %d)", updated);
    } else if (updated > 0) {
        LV_LOG_INFO("Sensors updated: %d sensors refreshed", updated);
    } else {
        LV_LOG_INFO("No sensor data available for update");
    }

    /* 2026-08-10：状态栏温湿度 = 传感器（天气卡温湿度仍 = 天气 API，
     * 见 ui_home.c weather_update_cb）。sensor subject 存 10 倍值。
     * P145：加「室内」前缀与天气卡（室外）区分，避免数值混淆。
     * 2026-09-10 P206：无传感器数据（updated<=0）显示"室内 --"，
     * 不再显示"室内 0°C | 0%"假零值。 */
    char sbuf[32];
    if (updated <= 0) {
        snprintf(sbuf, sizeof(sbuf), "室内 --");
    } else {
        int t = lv_subject_get_int(&temperature_subject);
        int h = lv_subject_get_int(&humidity_subject);
        snprintf(sbuf, sizeof(sbuf), "室内 %d\xc2\xb0""C | %d%%", t / 10, h / 10);
    }
    if (status_weather_label)
        lv_label_set_text(status_weather_label, sbuf);
    if (subpage_weather_lbl)
        lv_label_set_text(subpage_weather_lbl, sbuf);
    if (standby_bar_weather_lbl)
        lv_label_set_text(standby_bar_weather_lbl, sbuf);
}

/****************************************************************************
 * 初始化传感器
 ****************************************************************************/
static int init_sensors(void) {
    /* 2026-08-23：LD2410B 毫米波雷达（人体感应源，替代 LTR553 prox）
     * 2026-08-27 P124：恢复启用——已迁到 /dev/uart3（PD10/PD11）：
     * UART0 曾与 TF 卡冲突（P119 禁用），现 UART3 专供传感器，
     * UART1 留给「UART 调试工具」（接 BSN20 电平转换外设侧）。 */
    dm_ld2410b_init();
    /* P153：LTR553 ALS 环境光订阅（锁屏亮度显示 + 暗光开灯提醒）。
     * 必须独立于温湿度先初始化——shtc3 订阅失败会提前 return，导致
     * 光感/暗光提醒整体失效（审查发现）。失败不致命：无光感传感器时
     * 锁屏亮度行显示 "--"，暗光提醒静默。 */
    if (dm_als_init() != 0) {
        LV_LOG_WARN("LTR553 ALS init failed (no light sensor), lux disabled");
    }
    /* 直接初始化传感器订阅 */
    if (lv_demo_init_sensor_subscriptions() != 0) {
        LV_LOG_ERROR("Failed to initialize sensor subscriptions");
        return -1;
    }
    LV_LOG_INFO("Temperature and humidity and prox sensors initialized successfully");
    return 0;
}

/****************************************************************************
 * 主函数
 ****************************************************************************/
int luncher_dm_main(int argc, FAR char *argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;
    if (lv_is_initialized()) {
        LV_LOG_ERROR("LVGL already initialized");
        return -1;
    }
    #ifdef NEED_BOARDINIT
        boardctl(BOARDIOC_INIT, 0);
    #endif
    /* LVGL初始化 */
    lv_init();
    lv_nuttx_dsc_init(&info);
    #ifdef CONFIG_LV_USE_NUTTX_LCD
        info.fb_path = "/dev/lcd0";
    #endif 
    #ifdef CONFIG_INPUT_TOUCHSCREEN
        info.input_path = CONFIG_LUNCHER_APP_INPUT_DEVPATH;
    #endif
    #ifdef CONFIG_LV_USE_NUTTX_LIBUV
        uv_loop_t ui_loop;
        lv_memzero(&ui_loop, sizeof(ui_loop));
    #endif
    /* NuttX后端初始化 */
    lv_nuttx_init(&info, &result);
    /* 必要延时，影响初始化顺序 */
    usleep(100000);
    if (result.disp == NULL) {
        LV_LOG_ERROR("LVGL initialization failed");
        return 1;
    }
    /* 初始化中文支持字体 */
    init_fonts();
    /* T1（2026-09-11）：无 RTC 电池，开机时间为 1970；先用上次成功对时
     * 落盘的时间兜底，时段问候/时钟立即合理，联网对时到了再覆盖校正。 */
    dm_time_restore_at_boot();
    /* 创建 Desktop Mate 界面 */
    deskmate_ui_create();
    /* 2026-09-11：按键截图（本地固件；任一 LRADC 键 → /sdcard/screenshots/） */
    dm_screenshot_init();
    /* 传感器相关初始化（数据采集保留，UI 显示后续接入 deskmate 卡片） */
    /* 初始化传感器主题 */
    lv_subject_init_int(&temperature_subject, 250);  // 默认25.0°C
    lv_subject_init_int(&humidity_subject, 600);     // 默认60.0%
    /* 初始化传感器 */
    if (init_sensors() != 0) {
        LV_LOG_ERROR("Failed to initialize sensors");
    } else {
        LV_LOG_INFO("Sensors initialized successfully");
        /* 创建传感器数据更新定时器 */
        lv_timer_t *sensor_timer = lv_timer_create(update_sensor_cb, 1000, NULL);
        if (sensor_timer == NULL) {
            LV_LOG_ERROR("Failed to create sensor update timer");
        } else {
            lv_timer_set_repeat_count(sensor_timer, -1);
            LV_LOG_INFO("Sensor update timer created successfully");
        }
    }
    /* DesktopMate 健康模块（2026-08-15）：在座感知 + 久坐/喝水状态机。
     * 自带 1s timer，独立于传感器 timer；prox 不可用时回退纯计时。 */
    dm_health_init();
    /* 2026-09-10 P206 宠物先有鸡先有蛋修复（910bootlog 实锤）：
     * pet_core_init 此前只在 dm_pet_create 内调用，而 standby 建猫门控
     * dm_pet_is_enabled() 读的是未初始化的 g_pet（bss 零 = 关）。
     * 存档在（老用户）→ 门控过 → create 内 init → 一切正常；
     * 存档丢失（/data 丢失/首烧）→ 门控永远不过 → 猫永不创建 →
     * 存档永不落盘 → 死锁。本次开机即 init（幂等），门控读到真值。 */
    pet_core_init();
    dm_alarm_init();       /* 2026-09-10 P206：闹钟读盘 + 1s 到分检查（常驻） */
    /* 2026-09-10 P206：音量/亮度开机恢复（cfg 在 dm_health_init 内已加载；
     * 缺项回编译期默认 30/75。亮度立即写硬件，夜览 hook 稍后可能再压暗。） */
    g_music_volume = dm_health_cfg_get_int("music_volume", 30);
    if (g_music_volume < 0) g_music_volume = 0;
    if (g_music_volume > 100) g_music_volume = 100;
    g_screen_brightness = dm_health_cfg_get_int("screen_brightness", 75);
    if (g_screen_brightness < 0) g_screen_brightness = 0;
    if (g_screen_brightness > 100) g_screen_brightness = 100;
    dm_brightness_write(g_screen_brightness);
    voice_director_init();   /* 2026-08-28 Step 3：Voice Director 初始化（dm_health_init 旁） */
    /* 初始化LED适配器 */
    LV_LOG_INFO("Initializing LED adapter...");
    led_error_t led_err = led_adapter_init();
    if (led_err != LED_SUCCESS) {
        LV_LOG_ERROR("Failed to initialize LED adapter: %s", 
                     dm_led_get_error_string(led_err));
        light_initialized = false;
    } else {
        light_initialized = true;
        LV_LOG_INFO("LED adapter initialized successfully");
        /* 运行LED诊断 */
        led_adapter_diagnose();
        /* 快速测试LED */
        LV_LOG_INFO("Quick LED test...");
        led_adapter_on();
        usleep(200000);
        led_adapter_off();
        usleep(200000);
        // led_adapter_on(); // 默认关闭灯光
        LV_LOG_INFO("LED test completed");
    }
    /* 创建更新时间定时器 */
    lv_timer_t *time_timer = lv_timer_create(update_time_cb, 1000, NULL);
    if (time_timer == NULL) {
        LV_LOG_ERROR("Failed to create time update timer");
    } else {
        lv_timer_set_repeat_count(time_timer, -1);
        LV_LOG_INFO("Time update timer created successfully");
    }
    /* 显示分辨率信息 */
    lv_disp_t *disp = lv_disp_get_default();
    lv_coord_t disp_width = lv_disp_get_hor_res(disp);
    lv_coord_t disp_height = lv_disp_get_ver_res(disp);
    LV_LOG_INFO("Display: %dx%d, App area: %dx%d",
                disp_width, disp_height, SCREEN_WIDTH, SCREEN_HEIGHT);
    /* 主循环 */
    #ifdef CONFIG_LV_USE_NUTTX_LIBUV
        lv_nuttx_uv_loop(&ui_loop, &result);
    #else
        while (1) {
            uint32_t idle = lv_timer_handler();
            usleep(idle ? idle * 1000 : 5000);
        }
    #endif
    /* 清理资源 */
    if (light_initialized) {
        led_adapter_deinit();
    }
    return 0;
}

#endif /* CONFIG_LUNCHER_APP */