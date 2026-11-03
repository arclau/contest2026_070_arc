/* Copyright (c) 2019-2025 Allwinner Technology Co., Ltd. ALL rights reserved.
 *
 * Allwinner is a trademark of Allwinner Technology Co.,Ltd., registered in
 * the the People's Republic of China and other countries.
 * All Allwinner Technology Co.,Ltd. trademarks are used with permission.
 *
 * DISCLAIMER
 * THIRD PARTY LICENCES MAY BE REQUIRED TO IMPLEMENT THE SOLUTION/PRODUCT.
 * IF YOU NEED TO INTEGRATE THIRD PARTY'S TECHNOLOGY (SONY, DTS, DOLBY, AVS OR MPEGLA, ETC.)
 * IN ALLWINNERS'S DK OR PRODUCTS, YOU SHALL BE SOLELY RESPONSIBLE TO OBTAIN
 * ALL APPROPRIATELY REQUIRED THIRD PARTY LICENCES.
 * ALLWINNER SHALL HAVE NO WARRANTY, INDEMNITY OR OTHER OBLIGATIONS WITH RESPECT TO MATTERS
 * COVERED UNDER ANY REQUIRED THIRD PARTY LICENSE.
 * YOU ARE SOLELY RESPONSIBLE FOR YOUR USAGE OF THIRD PARTY'S TECHNOLOGY.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY ALLWINNER"AS IS" AND TO THE MAXIMUM EXTENT
 * PERMITTED BY LAW, ALLWINNER EXPRESSLY DISCLAIMS ALL WARRANTIES OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING WITHOUT LIMITATION REGARDING
 * THE TITLE, NON-INFRINGEMENT, ACCURACY, CONDITION, COMPLETENESS, PERFORMANCE
 * OR MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * IN NO EVENT SHALL ALLWINNER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS, OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* BOE 1440x720 (landscape) MIPI DSI panel driver (R528 disp2 framework).
 *
 * Native panel resolution is 720x1440 (portrait); the product uses it in
 * landscape 1440x720 via the display-controller rotation (degree0=3 in the
 * SOC mipi_config), following the same pattern as the former BOE_1200x1920.
 *
 * The panel init / exit sequences are translated from the Linux DTS
 * rk3588-firefly-roc-rk3588s-pc-helitai-boe-1440x720p.dtsi
 * (panel-init-sequence / panel-exit-sequence byte streams).
 * RK-board-specific GPIO/PWM/regulator nodes in that DTS are NOT used here;
 * the R528 Gemini-S1 board wiring (PD19 reset, PD20 PWM, AXP2101 power,
 * PD0~PD9 DSI) is taken from the BOE_1200x1920 template.
 */

#include "BOE_1440x720.h"
#include <string.h>
#include <syslog.h>

extern s32 bsp_disp_lcd_set_bright(u32 disp, u32 bright);

static void lcd_power_on(u32 sel);
static void lcd_power_off(u32 sel);
static void lcd_bl_open(u32 sel);
static void lcd_bl_close(u32 sel);
static void lcd_panel_init(u32 sel);
static void lcd_panel_exit(u32 sel);

/* BOE 1440x720 复位控制 (DSI_RESET - PD19) */
#define panel_reset(sel, val) sunxi_lcd_gpio_set_value(sel, 0, val)

static void lcd_cfg_panel_info(struct panel_extend_para *info)
{
	u32 i = 0, j = 0;
	u32 items;

	u8 lcd_gamma_tbl[][2] = {
		{0, 0},
		{15, 15},
		{30, 30},
		{45, 45},
		{60, 60},
		{75, 75},
		{90, 90},
		{105, 105},
		{120, 120},
		{135, 135},
		{150, 150},
		{165, 165},
		{180, 180},
		{195, 195},
		{210, 210},
		{225, 225},
		{240, 240},
		{255, 255},
	};

	u32 lcd_cmap_tbl[2][3][4] = {
		{
			{LCD_CMAP_G0, LCD_CMAP_B1, LCD_CMAP_G2, LCD_CMAP_B3},
			{LCD_CMAP_B0, LCD_CMAP_R1, LCD_CMAP_B2, LCD_CMAP_R3},
			{LCD_CMAP_R0, LCD_CMAP_G1, LCD_CMAP_R2, LCD_CMAP_G3},
		},
		{
			{LCD_CMAP_B3, LCD_CMAP_G2, LCD_CMAP_B1, LCD_CMAP_G0},
			{LCD_CMAP_R3, LCD_CMAP_B2, LCD_CMAP_R1, LCD_CMAP_B0},
			{LCD_CMAP_G3, LCD_CMAP_R2, LCD_CMAP_G1, LCD_CMAP_R0},
		},
	};

	if (!info) {
		syslog(LOG_ERR, "BOE_1440x720: ERROR - panel info pointer is NULL!\n");
		return;
	}

	/* disp_lcd_init() owns the brightness curve.  This callback also runs
	 * on panel registration and enable, so only update gamma and cmap.
	 */

	/* 生成完整的Gamma校正表 */
	items = sizeof(lcd_gamma_tbl) / 2;
	for (i = 0; i < items - 1; i++) {
		u32 num = lcd_gamma_tbl[i + 1][0] - lcd_gamma_tbl[i][0];

		for (j = 0; j < num; j++) {
			u32 value = 0;

			value = lcd_gamma_tbl[i][1] +
				((lcd_gamma_tbl[i + 1][1] - lcd_gamma_tbl[i][1]) * j) / num;
			info->lcd_gamma_tbl[lcd_gamma_tbl[i][0] + j] =
				(value << 16) + (value << 8) + value;
		}
	}

	info->lcd_gamma_tbl[255] = (lcd_gamma_tbl[items - 1][1] << 16) +
					   (lcd_gamma_tbl[items - 1][1] << 8) +
					   lcd_gamma_tbl[items - 1][1];

	memcpy(info->lcd_cmap_tbl, lcd_cmap_tbl, sizeof(lcd_cmap_tbl));
}

/* 开机时序配置 */
static s32 lcd_open_flow(u32 sel)
{
	LCD_OPEN_FUNC(sel, lcd_power_on, 10);
	LCD_OPEN_FUNC(sel, lcd_panel_init, 10);
	LCD_OPEN_FUNC(sel, sunxi_lcd_tcon_enable, 50);
	LCD_OPEN_FUNC(sel, lcd_bl_open, 0);

	return 0;
}

/* 关机时序配置 */
static s32 lcd_close_flow(u32 sel)
{
	LCD_CLOSE_FUNC(sel, lcd_bl_close, 0);
	LCD_CLOSE_FUNC(sel, sunxi_lcd_tcon_disable, 0);
	LCD_CLOSE_FUNC(sel, lcd_panel_exit, 200);
	LCD_CLOSE_FUNC(sel, lcd_power_off, 500);

	return 0;
}

/* 上电时序 (R528 Gemini-S1 board wiring) */
static void lcd_power_on(u32 sel)
{
	syslog(LOG_INFO, "BOE1440: power_on start\n");
	panel_reset(sel, GPIO_DATA_LOW);
	sunxi_lcd_power_enable(sel, AXP2101_ID_DCDC1);
	sunxi_lcd_power_enable(sel, AXP2101_ID_DCDC3);
	sunxi_lcd_delay_ms(20);
	sunxi_lcd_pin_cfg(sel, 1);
	sunxi_lcd_delay_ms(50);
	panel_reset(sel, GPIO_DATA_HIGH);
	sunxi_lcd_delay_ms(20);
	panel_reset(sel, GPIO_DATA_LOW);
	sunxi_lcd_delay_ms(30);
	panel_reset(sel, GPIO_DATA_HIGH);
	sunxi_lcd_delay_ms(30);
	sunxi_lcd_power_enable(sel, AXP2101_ID_DCDC2);
	sunxi_lcd_delay_ms(20);
	sunxi_lcd_dsi_clk_enable(sel);
	syslog(LOG_INFO, "BOE1440: power_on done\n");
}

/* 下电时序 (上电的逆序) */
static void lcd_power_off(u32 sel)
{
	sunxi_lcd_dsi_clk_disable(sel);
	sunxi_lcd_delay_ms(20);
	sunxi_lcd_power_disable(sel, AXP2101_ID_DCDC2);
	sunxi_lcd_delay_ms(20);
	panel_reset(sel, GPIO_DATA_LOW);
	sunxi_lcd_delay_ms(20);
	sunxi_lcd_pin_cfg(sel, 0);
	sunxi_lcd_delay_ms(20);
	sunxi_lcd_power_disable(sel, AXP2101_ID_DCDC3);
	sunxi_lcd_power_disable(sel, AXP2101_ID_DCDC1);
}

/* 开启背光 */
static void lcd_bl_open(u32 sel)
{
	s32 pwm_ret = sunxi_lcd_pwm_enable(sel);

	syslog(LOG_INFO, "BOE1440: bl_open pwm_ret=%d\n", pwm_ret);
	sunxi_lcd_delay_ms(100);
	sunxi_lcd_backlight_enable(sel);
	syslog(LOG_INFO, "BOE1440: bl_open backlight_enable done\n");
	sunxi_lcd_delay_ms(100);
}

/* 关闭背光 */
static void lcd_bl_close(u32 sel)
{
	sunxi_lcd_backlight_disable(sel);
	sunxi_lcd_pwm_disable(sel);
	sunxi_lcd_delay_ms(200);
}

/* P285 E2: adopt the F101-verified 11-command sequence verbatim
 * (F101 E3 image 15a60fdba..., same panel + adapter, display confirmed OK;
 * source F101-YuzukiNeko/overlays/boe-hx83102b-720x1440-ref.overlay).
 * Transport follows F101: vendor commands via generic, standard DCS via
 * DCS.  Replaces the 31-command table and the E1 all-DCS transport;
 * exit path and mipi_config are unchanged.
 */
static void lcd_panel_init(u32 sel)
{
	syslog(LOG_INFO, "BOE1440: init F101 verified 11-command sequence\n");

	sunxi_lcd_dsi_gen_write_1para(sel, 0x35, 0x00);
	sunxi_lcd_delay_ms(5);
	sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_EXIT_SLEEP_MODE);
	sunxi_lcd_delay_ms(120);
	sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_SET_DISPLAY_ON);
	sunxi_lcd_delay_ms(50);

	sunxi_lcd_dsi_gen_write_1para(sel, 0xff, 0x22);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xff, 0x27);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xfb, 0x01);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xad, 0x25);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xae, 0x55);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xaf, 0x05);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xff, 0x10);
	sunxi_lcd_delay_ms(1);
	sunxi_lcd_dsi_gen_write_1para(sel, 0xfb, 0x01);

	syslog(LOG_INFO, "BOE1440: init done\n");
}

/* 面板退出序列 (翻译自 DTS panel-exit-sequence)
 *   05 0A 01 28 -> Display Off (0x28, 标准DCS) + 10ms
 *   05 78 01 10 -> Sleep In    (0x10, 标准DCS) + 120ms
 *   39 00 02 04 5A -> GEN 1para 0x04=0x5A (厂商私有命令)
 */
static void lcd_panel_exit(u32 sel)
{
	/* 05 0A 01 28 : Display Off (标准 DCS) */
	sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_SET_DISPLAY_OFF);
	sunxi_lcd_delay_ms(10);

	/* 05 78 01 10 : Sleep In (标准 DCS) + 120ms */
	sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_ENTER_SLEEP_MODE);
	sunxi_lcd_delay_ms(120);

	/* 39 00 02 04 5A : 厂商私有命令，用 gen_write */
	sunxi_lcd_dsi_gen_write_1para(sel, 0x04, 0x5A);
}

/* 用户自定义函数 */
static s32 lcd_user_defined_func(u32 sel, u32 para1, u32 para2, u32 para3)
{
	bsp_disp_lcd_set_bright(sel, 60);
	return 0;
}

struct __lcd_panel BOE_1440x720_panel = {
	/* panel driver name, must match lcd_driver_name in mipi config */
	.name = "BOE_1440x720",
	.func = {
		.cfg_panel_info = lcd_cfg_panel_info,
		.cfg_open_flow = lcd_open_flow,
		.cfg_close_flow = lcd_close_flow,
		.lcd_user_defined_func = lcd_user_defined_func,
	},
};
