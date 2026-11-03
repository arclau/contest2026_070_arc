/* BOE 1440x720 LCD0 配置参数 (R528 Gemini-S1)
 *
 * 面板 native 分辨率为 720x1440 (竖屏)，产品横屏 1440x720 通过
 * display controller 旋转 (degree0=3) 实现，模式与旧 BOE_1200x1920 一致。
 *
 * 时序/DSI/初始化序列来源:
 *   rk3588-firefly-roc-rk3588s-pc-helitai-boe-1440x720p.dtsi
 *   (clock-frequency=79540000Hz, hactive=720, vactive=1440,
 *    hfp=30, hbp=30, hspw=20, vfp=200, vbp=15, vspw=2,
 *    MIPI 4-lane, RGB888, video burst)
 *
 * 注意 R528 的 lcd_hbp / lcd_vbp 语义与 DTS 的 back-porch 不同:
 *   de_dsi.c  dsi_hbp = (lcd_hbp - lcd_hspw) * bpp / 8
 *             dsi_vbp =  lcd_vbp - lcd_vspw
 * 即这两个字段已包含同步脉宽。故 lcd_hbp = hbp + hspw = 50，
 * lcd_vbp = vbp + vspw = 17；lcd_ht / lcd_vt 不变，前肩自动为
 * hfp = 800 - 50 - 720 = 30、vfp = 1657 - 17 - 1440 = 200，与 DTS 一致。
 *
 * 板级接线 (复位/背光/电源/DSI 引脚) 沿用 R528 模板，不取 DTS 的 RK 板级值:
 *   VDD=1.8V (dcdc1), VDD_IF=1.8V (dcdc3), AVDD=9.6V (dcdc2)
 *   复位 PD19, 背光 PWM ch4 (PD20) @40KHz, DSI PD0~PD9
 */
#include <stdint.h>
#include <hal_clk.h>
#include <hal_gpio.h>
#include "../disp/disp_sys_intf.h"

typedef uint32_t u32;
typedef int32_t  s32;

#include "disp_board_config.h"

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
		.v.str = "BOE_1440x720",
	},
	{
		.name = "lcd_if",
		.type = PROPERTY_INTGER,
		.v.value = 4,  // 4=DSI接口
	},
	// 分辨率配置 (native 720x1440)
	{
		.name = "lcd_x",
		.type = PROPERTY_INTGER,
		.v.value = 720,
	},
	{
		.name = "lcd_y",
		.type = PROPERTY_INTGER,
		.v.value = 1440,
	},
	{
		.name = "lcd_width",
		.type = PROPERTY_INTGER,
		.v.value = 80,   // 物理宽度 (mm, 720侧)
	},
	{
		.name = "lcd_height",
		.type = PROPERTY_INTGER,
		.v.value = 120,  // 物理高度 (mm, 1440侧)
	},
	{
		.name = "lcd_dclk_freq",
		.type = PROPERTY_INTGER,
		.v.value = 80,   // 像素时钟 ~80MHz (DTS: 79540000Hz, 取整)
	},
	// MIPI DSI 配置
	{
		.name = "lcd_dsi_if",
		.type = PROPERTY_INTGER,
		.v.value = LCD_DSI_IF_VIDEO_MODE,  // burst 实验 P283 证伪：只剩暗影，回退 video
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
		.v.value = 160,   // DSI时钟率 ~160MHz (dclk*8/lane = 80*8/4)
	},
	// 水平时序 (HT=720+30+30+20=800)
	{
		.name = "lcd_ht",
		.type = PROPERTY_INTGER,
		.v.value = 800,  // 水平总计
	},
	{
		.name = "lcd_hbp",
		.type = PROPERTY_INTGER,
		.v.value = 50,    // 后肩含同步脉宽: 30 (DTS hbp) + 20 (hspw)
	},
	{
		.name = "lcd_hfp",
		.type = PROPERTY_INTGER,
		.v.value = 30,    // 水平前肩
	},
	{
		.name = "lcd_hspw",
		.type = PROPERTY_INTGER,
		.v.value = 20,    // 水平同步脉宽
	},
	// 垂直时序 (VT=1440+200+15+2=1657)
	{
		.name = "lcd_vt",
		.type = PROPERTY_INTGER,
		.v.value = 1657, // 垂直总计
	},
	{
		.name = "lcd_vbp",
		.type = PROPERTY_INTGER,
		.v.value = 17,    // 后肩含同步脉宽: 15 (DTS vbp) + 2 (vspw)
	},
	{
		.name = "lcd_vfp",
		.type = PROPERTY_INTGER,
		.v.value = 200,   // 垂直前肩
	},
	{
		.name = "lcd_vspw",
		.type = PROPERTY_INTGER,
		.v.value = 2,     // 垂直同步脉宽
	},
	{
		.name = "lcd_frm",
		.type = PROPERTY_INTGER,
		.v.value = 0,     // 0=RGB, 1=RBG
	},
	// 背光PWM配置 (PWM4, 40KHz, 默认亮度200 -- 沿用 R528 板级)
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
		.v.value = 40000, // PWM频率40KHz
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
		.v.value = 200,   // 默认亮度
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
		.v.str = "720x1440",
	},
	{
		.name = "lcd_model_name",
		.type = PROPERTY_STRING,
		.v.str = "BOE_1440x720",
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
		.v.value = 720,   // native 宽 (旋转后由 degree0 变为 1440)
	},
	{
		.name = "fb0_height",
		.type = PROPERTY_INTGER,
		.v.value = 1440,  // native 高 (旋转后由 degree0 变为 720)
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
		/* 面板物理 720x1440 竖屏；横屏 1440x720 使用需 90° 旋转。
		 * degree0=1(90°)/3(270°) 时 dev_fb.c 会交换 fb 宽高为 1440x720。
		 * 方向需与触摸旋转映射一致（本板 GT9271，见 gt9271_iic_touch.c）。
		 * 若实机图像上下/左右颠倒，改 1 与 3 互换。 */
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
