#include <stdint.h>
#include <hal_clk.h>
#include <hal_gpio.h>
#include "../disp/disp_sys_intf.h"

typedef uint32_t u32;
typedef int32_t  s32;

#include "disp_board_config.h"

/* BOE 1200x1920 LCD0 配置参数
 * 严格按照厂家规格配置 (从 DTS panel 节点移植):
 * VDD=1.8V (dcdc1), VDD_IF=1.8V (dcdc3), AVDD=9.6V (dcdc2)
 * MIPI DSI: 4-lane, video mode, RGB888
 * 像素时钟: 146MHz, DSI 时钟: 292MHz
 * 时序: H_BP=30, H_FP=20, H_PW=10, V_BP=16, V_FP=16, V_PW=4
 * 背光: PWM ch4 @ 40KHz
 */
struct property_t g_lcd0_config[] = {
    // 基本LCD参数配置
    {
        .name = "lcd_used",
        .type = PROPERTY_INTGER,
        .v.value = 1,  // 使能LCD0
    },
    {
        .name = "lcd_driver_name",
        .type = PROPERTY_STRING,
        .v.str = "BOE_1200x1920",
    },
    {
        .name = "lcd_if",
        .type = PROPERTY_INTGER,
        .v.value = 4,  // 4=DSI接口
    },
    // 分辨率配置 (1200x1920)
    {
        .name = "lcd_x",
        .type = PROPERTY_INTGER,
        .v.value = 1200,
    },
    {
        .name = "lcd_y",
        .type = PROPERTY_INTGER,
        .v.value = 1920,
    },
    {
        .name = "lcd_width",
        .type = PROPERTY_INTGER,
        .v.value = 154,   // 物理宽度 (mm)
    },
    {
        .name = "lcd_height",
        .type = PROPERTY_INTGER,
        .v.value = 86,    // 物理高度 (mm)
    },
    {
        .name = "lcd_dclk_freq",
        .type = PROPERTY_INTGER,
        .v.value = 146,   // 像素时钟 146MHz (DTS: 146000000Hz)
    },
    // MIPI DSI 配置
    {
        .name = "lcd_dsi_if",
        .type = PROPERTY_INTGER,
        .v.value = LCD_DSI_IF_VIDEO_MODE,  // 0=视频模式 (Video Burst)
    },
    {
        .name = "lcd_dsi_lane",
        .type = PROPERTY_INTGER,
        .v.value = 4,     // 4-lane MIPI
    },
    {
        .name = "lcd_dsi_format",
        .type = PROPERTY_INTGER,
        .v.value = 0,     // 0=RGB888格式
    },
    {
        .name = "lcd_dsi_te",
        .type = PROPERTY_INTGER,
        .v.value = 0,     // 不使用TE信号
    },
    {
        .name = "lcd_dsi_port_num",
        .type = PROPERTY_INTGER,
        .v.value = 3,     // DSI端口
    },
    {
        .name = "lcd_dsi_clk_rate",
        .type = PROPERTY_INTGER,
        .v.value = 292,   // DSI时钟率 292MHz
    },
    // 水平时序 (HT=1200+20+30+10=1260)
    {
        .name = "lcd_ht",
        .type = PROPERTY_INTGER,
        .v.value = 1260,  // 水平总计
    },
    {
        .name = "lcd_hbp",
        .type = PROPERTY_INTGER,
        .v.value = 30,    // 水平后肩
    },
    {
        .name = "lcd_hfp",
        .type = PROPERTY_INTGER,
        .v.value = 20,    // 水平前肩
    },
    {
        .name = "lcd_hspw",
        .type = PROPERTY_INTGER,
        .v.value = 10,    // 水平同步脉宽
    },
    // 垂直时序 (VT=1920+16+16+4=1956)
    {
        .name = "lcd_vt",
        .type = PROPERTY_INTGER,
        .v.value = 1956,  // 垂直总计
    },
    {
        .name = "lcd_vbp",
        .type = PROPERTY_INTGER,
        .v.value = 16,    // 垂直后肩
    },
    {
        .name = "lcd_vfp",
        .type = PROPERTY_INTGER,
        .v.value = 16,    // 垂直前肩
    },
    {
        .name = "lcd_vspw",
        .type = PROPERTY_INTGER,
        .v.value = 4,     // 垂直同步脉宽
    },
    {
        .name = "lcd_frm",
        .type = PROPERTY_INTGER,
        .v.value = 0,     // 0=RGB, 1=RBG
    },
    // 背光PWM配置 (PWM4, 40KHz)
    {
        .name = "lcd_pwm_used",
        .type = PROPERTY_INTGER,
        .v.value = 1,     // 使用PWM背光控制
    },
    {
        .name = "lcd_pwm_ch",
        .type = PROPERTY_INTGER,
        .v.value = 4,     // PWM通道4 (PD20)
    },
    {
        .name = "lcd_pwm_freq",
        .type = PROPERTY_INTGER,
        .v.value = 40000, // PWM频率40KHz (DTS: period=25000ns)
    },
    {
        .name = "lcd_pwm_pol",
        .type = PROPERTY_INTGER,
        .v.value = 0,     // PWM极性正常
    },
    {
        .name = "lcd_pwm_max_limit",
        .type = PROPERTY_INTGER,
        .v.value = 255,   // 最大亮度值
    },
    // 背光控制参数
    {
        .name = "lcd_backlight",
        .type = PROPERTY_INTGER,
        .v.value = 200,   // 默认亮度 (DTS: default-brightness-level=200)
    },
    {
        .name = "lcd_backlight_curve",
        .type = PROPERTY_INTGER,
        .v.value = 0,     // 线性亮度曲线
    },
    {
        .name = "lcd_bl_en_power",
        .type = PROPERTY_INTGER,
        .v.value = 1,
    },
    // GPIO: 复位引脚 (DSI_RESET - PD19)
    {
        .name = "lcd_gpio_0",
        .type = PROPERTY_GPIO,
        .v.gpio_list = {
            .gpio_name = "PD19",
            .port = 3,            // GPIOD是端口3
            .port_num = 19,       // PD19
            .mul_sel = GPIO_MUXSEL_OUT,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    // PWM背光控制GPIO配置 (BL_PWM - PD20)
    {
        .name = "lcd_bl_en",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD20",
            .port = 3,
            .port_num = 20,       // PD20
            .mul_sel = GPIO_MUXSEL_FUNCTION5,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_HIGH,
        },
    },
    // DSI数据线配置 (MIPI DSI接口引脚 - PD0~PD9)
    {
        .name = "dsi_dp0",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD0",
            .port = 3,
            .port_num = 0,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dn0",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD1",
            .port = 3,
            .port_num = 1,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dp1",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD2",
            .port = 3,
            .port_num = 2,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dn1",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD3",
            .port = 3,
            .port_num = 3,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_ckp",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD4",
            .port = 3,
            .port_num = 4,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_ckn",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD5",
            .port = 3,
            .port_num = 5,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dp2",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD6",
            .port = 3,
            .port_num = 6,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dn2",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD7",
            .port = 3,
            .port_num = 7,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dp3",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD8",
            .port = 3,
            .port_num = 8,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    {
        .name = "dsi_dn3",
        .type = PROPERTY_PIN,
        .v.gpio_list = {
            .gpio_name = "PD9",
            .port = 3,
            .port_num = 9,
            .mul_sel = GPIO_MUXSEL_FUNCTION4,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
        },
    },
    // 电源配置 (三路电源)
    {
        .name = "lcd_power0",  // VDD 1.8V 数字电源
        .type = PROPERTY_POWER,
        .v.power = {
            .power_name = "dcdc1",
            .power_type = AXP2101_REGULATOR,
            .power_id = AXP2101_ID_DCDC1,
            .power_vol = 1800000,
            .always_on = true,
        },
    },
    {
        .name = "lcd_power1",  // VDD_IF 1.8V 接口电源
        .type = PROPERTY_POWER,
        .v.power = {
            .power_name = "dcdc3",
            .power_type = AXP2101_REGULATOR,
            .power_id = AXP2101_ID_DCDC3,
            .power_vol = 1800000,
            .always_on = true,
        },
    },
    {
        .name = "lcd_power2",  // AVDD 模拟电源
        .type = PROPERTY_POWER,
        .v.power = {
            .power_name = "dcdc2",
            .power_type = AXP2101_REGULATOR,
            .power_id = AXP2101_ID_DCDC2,
            .power_vol = 9600000,
            .always_on = true,
        },
    },
    {
        .name = "lcd_bright_curve_en",
        .type = PROPERTY_INTGER,
        .v.value = 0,
    },
    {
        .name = "lcd_size",
        .type = PROPERTY_STRING,
        .v.str = "1200x1920",
    },
    {
        .name = "lcd_model_name",
        .type = PROPERTY_STRING,
        .v.str = "BOE_1200x1920",
    },
};

/* 显示器通用配置 */
struct property_t g_disp_config[] = {
    {
        .name = "disp_init_enable",
        .type = PROPERTY_INTGER,
        .v.value = 1,
    },
    {
        .name = "disp_mode",
        .type = PROPERTY_INTGER,
        .v.value = 0,
    },
    {
        .name = "screen0_output_type",
        .type = PROPERTY_INTGER,
        .v.value = 1,     // 1=LCD输出
    },
    {
        .name = "screen0_output_mode",
        .type = PROPERTY_INTGER,
        .v.value = 4,     // 4=DSI输出
    },
    {
        .name = "screen1_output_type",
        .type = PROPERTY_INTGER,
        .v.value = 4,     // 不使用screen1
    },
    {
        .name = "fb0_buffer_num",
        .type = PROPERTY_INTGER,
        .v.value = 2,
    },
    {
        .name = "fb0_width",
        .type = PROPERTY_INTGER,
        .v.value = 1200,
    },
    {
        .name = "fb0_height",
        .type = PROPERTY_INTGER,
        .v.value = 1920,
    },
#ifdef CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT
    {
        .name = "disp_rotation_used",
        .type = PROPERTY_INTGER,
        .v.value = 1,
    },
    {
        .name = "degree0",
        .type = PROPERTY_INTGER,
        /* 面板物理 1200x1920 竖屏；横屏 1920x1200 使用需 90° 旋转。
         * degree0=1(90°)/3(270°) 时 dev_fb.c 会交换 fb 宽高为 1920x1200。
         * 方向需与 GT9271 触摸旋转映射一致（见 gt9271_iic_touch.c）。 */
        .v.value = 3,
    },
#endif
};

/* LCD1配置 (不使用) */
struct property_t g_lcd1_config[] = {
    {
        .name = "lcd_used",
        .type = PROPERTY_INTGER,
        .v.value = 0,
    },
};

struct property_t *g_lcd0_config_list[] = {
    g_lcd0_config,
    NULL,
    NULL,
};

u32 g_lcd0_config_len_list[] = {
    sizeof(g_lcd0_config) / sizeof(struct property_t),
    sizeof(g_lcd1_config) / sizeof(struct property_t),
};

u32 g_lcd0_config_list_len = sizeof(g_lcd0_config_len_list) / sizeof(u32);
u32 g_lcd1_config_len = sizeof(g_lcd1_config) / sizeof(struct property_t);
u32 g_disp_config_len = sizeof(g_disp_config) / sizeof(struct property_t);
u32 g_lcd0_config_len = sizeof(g_lcd0_config) / sizeof(struct property_t);
