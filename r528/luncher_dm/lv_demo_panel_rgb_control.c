/**
 * @file lv_demo_panel_rgb_control.h
 *
 */

#ifdef __cplusplus
extern "C" {
#endif
/*********************
 *      INCLUDES
 *********************/
#include "lv_demo_panel_rgb_control.h"
#include "sunxi_hal_ledc.h"
#include <debug.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#ifndef CONFIG_WS2812_DEV_PATH
#ifdef CONFIG_LED_RGB_WS2812
//#define CONFIG_WS2812_DEV_PATH "/dev/ws2812"
#define CONFIG_WS2812_DEV_PATH "/dev/leds0"
#else
#define CONFIG_WS2812_DEV_PATH "/dev/leds0"
#endif
#endif

// 全局状态
static led_status_t g_led_status = {.color = LED_COLOR_WHITE, // 默认白色
                                    .mode = LED_MODE_STATIC,
                                    .brightness = 100,
                                    .frequency = 1,
                                    .is_running = false};

static bool g_initialized = false;

// 内部函数声明
static led_error_t apply_led_settings(void);

int dm_ctrl_ws2812(unsigned int color) {
  int fd = open(CONFIG_WS2812_DEV_PATH, O_RDWR);
  if (fd < 0) {
    return -1;
  } else {
    int ret = write(fd, &color, sizeof(int));
    close(fd);
    if (sizeof(int) != ret) {
      return -1;
    }
    return 0;
  }
}

// ==================== 初始化与销毁 ====================
led_error_t dm_led_controller_init(void) {
  if (g_initialized) {
    return LED_SUCCESS;
  }

#ifndef CONFIG_LED_RGB_WS2812
  // HAL接口初始化
  if (hal_ledc_init() != 0) {
    _info("Failed to initialize LEDC controller\n");
    return LED_ERROR_INIT_FAILED;
  }
#endif

  g_initialized = true;
  // 初始化后默认关闭LED
  g_led_status.is_running = false;

  _info("LED controller initialized successfully\n");
  return LED_SUCCESS;
}

led_error_t dm_led_controller_deinit(void) {
  if (!g_initialized) {
    return LED_SUCCESS;
  }

  // 先关闭LED
  dm_led_off();

#ifndef CONFIG_LED_RGB_WS2812

  hal_ledc_deinit();
#endif

  g_initialized = false;
  _info("LED controller deinitialized\n");
  return LED_SUCCESS;
}

// ==================== 基础开关控制 ====================
led_error_t dm_led_on(void) {
  if (!g_initialized) {
    return LED_ERROR_NOT_RUNNING;
  }

  g_led_status.is_running = true;
  return apply_led_settings();
}

led_error_t dm_led_off(void) {
  if (!g_initialized) {
    return LED_ERROR_NOT_RUNNING;
  }

  uint32_t saved_color = g_led_status.color;

  // 关闭LED
  g_led_status.is_running = false;

#ifdef CONFIG_LED_RGB_WS2812
    if (dm_ctrl_ws2812(LED_COLOR_OFF) != 0) {
    _info("LED controller OFF FAILED\n");
  }
#else
  sunxi_set_led_brightness(0, LED_COLOR_OFF);
#endif

  // 恢复颜色设置但不应用
  g_led_status.color = saved_color;
  return LED_SUCCESS;
}

led_error_t dm_led_toggle(void) {
  if (g_led_status.is_running) {
    return dm_led_off();
  } else {
    return dm_led_on();
  }
}

led_error_t dm_led_is_on(bool *state) {
  if (state == NULL) {
    return LED_ERROR_INVALID_PARAM;
  }

  *state = g_led_status.is_running;
  return LED_SUCCESS;
}

// ==================== 基础颜色控制 ====================
led_error_t dm_led_set_color(uint32_t color) {
  if (!g_initialized) {
    return LED_ERROR_NOT_RUNNING;
  }

  g_led_status.color = color;
  g_led_status.mode = LED_MODE_STATIC;

  // 如果LED当前是开启状态，立即应用新颜色
  if (g_led_status.is_running) {
    return apply_led_settings();
  }

  return LED_SUCCESS;
}

led_error_t dm_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
  uint32_t color = (red << 16) | (green << 8) | blue;
  return dm_led_set_color(color);
}

led_error_t dm_led_set_brightness(int32_t brightness) {
  if (brightness < 0 || brightness > 100) {
    return LED_ERROR_INVALID_PARAM;
  }

  g_led_status.brightness = brightness;

  // 如果LED当前是开启状态，立即应用新亮度
  if (g_led_status.is_running) {
    return apply_led_settings();
  }

  return LED_SUCCESS;
}

// ==================== 预定义颜色快捷方式 ====================
led_error_t dm_led_red(void) { return dm_led_set_color(LED_COLOR_RED); }

led_error_t dm_led_green(void) { return dm_led_set_color(LED_COLOR_GREEN); }

led_error_t dm_led_blue(void) { return dm_led_set_color(LED_COLOR_BLUE); }

led_error_t dm_led_yellow(void) { return dm_led_set_color(LED_COLOR_YELLOW); }

led_error_t dm_led_cyan(void) { return dm_led_set_color(LED_COLOR_CYAN); }

led_error_t dm_led_magenta(void) { return dm_led_set_color(LED_COLOR_MAGENTA); }

led_error_t dm_led_white(void) { return dm_led_set_color(LED_COLOR_WHITE); }

// ==================== 模式控制 ====================
led_error_t dm_led_set_mode_static(uint32_t color) {
  g_led_status.color = color;
  g_led_status.mode = LED_MODE_STATIC;
  g_led_status.frequency = 1;

  if (g_led_status.is_running) {
    return apply_led_settings();
  }

  return LED_SUCCESS;
}

led_error_t dm_led_set_mode_blink(uint32_t color, int frequency) {
  if (frequency <= 0 || frequency > 10) {
    return LED_ERROR_INVALID_PARAM;
  }

  g_led_status.color = color;
  g_led_status.mode = LED_MODE_BLINK;
  g_led_status.frequency = frequency;

  // 注意：闪烁模式需要额外的线程实现，这里只是设置参数
  // 实际应用中需要启动一个线程来处理闪烁逻辑

  if (g_led_status.is_running) {
    return apply_led_settings();
  }

  return LED_SUCCESS;
}

led_error_t dm_led_set_mode_rainbow(int speed) {
  if (speed <= 0 || speed > 10) {
    return LED_ERROR_INVALID_PARAM;
  }

  g_led_status.mode = LED_MODE_RAINBOW;
  g_led_status.frequency = speed;

  // 注意：彩虹模式需要额外的线程实现

  if (!g_led_status.is_running) {
    dm_led_on(); // 彩虹模式自动开启LED
  }

  return LED_SUCCESS;
}

// ==================== 状态查询 ====================
led_error_t dm_led_get_status(led_status_t *status) {
  if (status == NULL) {
    return LED_ERROR_INVALID_PARAM;
  }

  memcpy(status, &g_led_status, sizeof(led_status_t));
  return LED_SUCCESS;
}

// ==================== 工具函数 ====================
uint32_t dm_led_rgb_to_hex(uint8_t r, uint8_t g, uint8_t b) {
  return (r << 16) | (g << 8) | b;
}

void dm_led_hex_to_rgb(uint32_t hex, uint8_t *r, uint8_t *g, uint8_t *b) {
  if (r)
    *r = (hex >> 16) & 0xFF;
  if (g)
    *g = (hex >> 8) & 0xFF;
  if (b)
    *b = hex & 0xFF;
}

const char *dm_led_get_color_name(uint32_t color) {
  switch (color) {
  case LED_COLOR_RED:
    return "Red";
  case LED_COLOR_GREEN:
    return "Green";
  case LED_COLOR_BLUE:
    return "Blue";
  case LED_COLOR_YELLOW:
    return "Yellow";
  case LED_COLOR_CYAN:
    return "Cyan";
  case LED_COLOR_MAGENTA:
    return "Magenta";
  case LED_COLOR_WHITE:
    return "White";
  case LED_COLOR_OFF:
    return "Off";
  default:
    return "Custom";
  }
}

const char *dm_led_get_error_string(led_error_t error) {
  switch (error) {
  case LED_SUCCESS:
    return "Success";
  case LED_ERROR_INIT_FAILED:
    return "Initialization failed";
  case LED_ERROR_INVALID_PARAM:
    return "Invalid parameter";
  case LED_ERROR_HARDWARE:
    return "Hardware error";
  case LED_ERROR_NOT_RUNNING:
    return "LED controller not running";
  default:
    return "Unknown error";
  }
}

void dm_demo_advanced_control(void) {
  printf("\n=== LED Advanced Control Demo ===\n");

  dm_led_controller_init();

  // 设置自定义颜色并开启
  printf("Setting custom color and turning ON...\n");
  dm_led_set_rgb(255, 165, 0); // 橙色
  dm_led_on();
  sleep(2);

  // 调整亮度
  printf("Adjusting brightness...\n");
  for (int i = 100; i >= 0; i -= 20) {
    dm_led_set_brightness(i);
    printf("Brightness: %d%%\n", i);
    sleep(1);
  }

  for (int i = 0; i <= 100; i += 20) {
    dm_led_set_brightness(i);
    printf("Brightness: %d%%\n", i);
    sleep(1);
  }

  // 关闭
  dm_led_off();
  dm_led_controller_deinit();
}

// ==================== 内部函数实现 ====================

static led_error_t apply_led_settings(void) {
  if (!g_initialized) {
    return LED_ERROR_NOT_RUNNING;
  }

  // 如果LED是关闭状态，不应用任何设置
  if (!g_led_status.is_running) {
    return LED_SUCCESS;
  }

  int ret;
  uint32_t actual_color = g_led_status.color;

  // 应用亮度调整
  if (g_led_status.brightness < 100) {
    uint8_t r, g, b;
    dm_led_hex_to_rgb(actual_color, &r, &g, &b);
    r = (r * g_led_status.brightness) / 100;
    g = (g * g_led_status.brightness) / 100;
    b = (b * g_led_status.brightness) / 100;
    actual_color = dm_led_rgb_to_hex(r, g, b);
  }

#ifdef CONFIG_LED_RGB_WS2812
  // 文件接口
  int write_ret = dm_ctrl_ws2812(actual_color);
  if (write_ret != 0) {
    _info("Error setting LED color: %d\n", write_ret);
    return LED_ERROR_HARDWARE;
  }
#else
  // HAL接口
  ret = sunxi_set_led_brightness(0, actual_color);
  if (ret != 0) {
    _info("Error setting LED color: %d\n", ret);
    return LED_ERROR_HARDWARE;
  }
#endif

  return LED_SUCCESS;
}

#ifdef __cplusplus
}
#endif