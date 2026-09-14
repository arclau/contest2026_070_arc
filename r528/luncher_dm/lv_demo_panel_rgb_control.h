#ifndef LV_DEMO_PANEL_RGB_CONTROL_H
#define LV_DEMO_PANEL_RGB_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// LED颜色定义
typedef enum {
    LED_COLOR_RED     = 0xFF0000,
    LED_COLOR_GREEN   = 0x00FF00,
    LED_COLOR_BLUE    = 0x0000FF,
    LED_COLOR_YELLOW  = 0xFFFF00,
    LED_COLOR_CYAN    = 0x00FFFF,
    LED_COLOR_MAGENTA = 0xFF00FF,
    LED_COLOR_WHITE   = 0xFFFFFF,
    LED_COLOR_OFF     = 0x000000
} led_color_t;

// LED模式定义
typedef enum {
    LED_MODE_STATIC,      // 静态颜色
    LED_MODE_BLINK,       // 闪烁
    LED_MODE_BREATHE,     // 呼吸效果
    LED_MODE_RAINBOW,     // 彩虹序列
    LED_MODE_CUSTOM       // 自定义序列
} led_mode_t;

// LED状态结构体
typedef struct {
    uint32_t color;
    led_mode_t mode;
    int brightness;       // 亮度 0-100
    int frequency;        // 闪烁/呼吸频率 (Hz)
    bool is_running;
} led_status_t;

// 错误码定义
typedef enum {
    LED_SUCCESS = 0,
    LED_ERROR_INIT_FAILED = -1,
    LED_ERROR_INVALID_PARAM = -2,
    LED_ERROR_HARDWARE = -3,
    LED_ERROR_NOT_RUNNING = -4
} led_error_t;

// ==================== 初始化与销毁 ====================
led_error_t dm_led_controller_init(void);
led_error_t dm_led_controller_deinit(void);

// ==================== 基础开关控制 ====================
led_error_t dm_led_on(void);                    // 开启LED（使用当前颜色）
led_error_t dm_led_off(void);                   // 关闭LED
led_error_t dm_led_toggle(void);                // 切换开关状态
led_error_t dm_led_is_on(bool* state);          // 查询LED状态

// ==================== 基础颜色控制 ====================
led_error_t dm_led_set_color(uint32_t color);
led_error_t dm_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
led_error_t dm_led_set_brightness(int32_t brightness); // 0-100

// ==================== 预定义颜色快捷方式 ====================
led_error_t dm_led_red(void);
led_error_t dm_led_green(void);
led_error_t dm_led_blue(void);
led_error_t dm_led_yellow(void);
led_error_t dm_led_cyan(void);
led_error_t dm_led_magenta(void);
led_error_t dm_led_white(void);

// ==================== 模式控制 ====================
led_error_t dm_led_set_mode_static(uint32_t color);
led_error_t dm_led_set_mode_blink(uint32_t color, int frequency);
led_error_t dm_led_set_mode_breathe(uint32_t color, int frequency);
led_error_t dm_led_set_mode_rainbow(int speed);
led_error_t dm_led_set_mode_custom_sequence(const uint32_t* colors, int count, int interval_ms);

// ==================== 状态查询 ====================
led_error_t dm_led_get_status(led_status_t* status);
const char* dm_led_get_error_string(led_error_t error);

// ==================== 工具函数 ====================
uint32_t dm_led_rgb_to_hex(uint8_t r, uint8_t g, uint8_t b);
void dm_led_hex_to_rgb(uint32_t hex, uint8_t* r, uint8_t* g, uint8_t* b);
const char* dm_led_get_color_name(uint32_t color);
// ==================== demo ====================
void dm_demo_advanced_control(void);
#ifdef __cplusplus
}
#endif

#endif /* PANEL_RGB_CONTROL_H */