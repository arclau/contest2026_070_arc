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

/* BOE 1200x1920 MIPI DSI panel driver (R528 disp2 framework).
 * Code-level port from the legacy SDK panel driver, following the
 * t070s140b template of the new SDK. All register values below are
 * panel-specific and MUST NOT be changed.
 */

#include "BOE_1200x1920.h"
#include <string.h>
#include <syslog.h>

extern s32 bsp_disp_lcd_set_bright(u32 disp, u32 bright);

static void lcd_power_on(u32 sel);
static void lcd_power_off(u32 sel);
static void lcd_bl_open(u32 sel);
static void lcd_bl_close(u32 sel);
static void lcd_panel_init(u32 sel);
static void lcd_panel_exit(u32 sel);

/* BOE 1200x1920 复位控制 (DSI_RESET - PD19) */
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
        syslog(LOG_ERR, "BOE_1200x1920: ERROR - panel info pointer is NULL!\n");
        return;
    }

    memset(info, 0, sizeof(struct panel_extend_para));

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

/* 上电时序 */
static void lcd_power_on(u32 sel)
{
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
    sunxi_lcd_pwm_enable(sel);
    sunxi_lcd_delay_ms(100);
    sunxi_lcd_backlight_enable(sel);
    sunxi_lcd_delay_ms(100);
}

/* 关闭背光 */
static void lcd_bl_close(u32 sel)
{
    sunxi_lcd_backlight_disable(sel);
    sunxi_lcd_pwm_disable(sel);
    sunxi_lcd_delay_ms(200);
}

/* 面板初始化序列 (从 DTS panel-init-sequence 翻译) */
static void lcd_panel_init(u32 sel)
{
    /* 05 14 01 10 : Sleep Out (0x10) + 20ms */
    sunxi_lcd_dsi_dcs_write_0para(sel, 0x10);
    sunxi_lcd_delay_ms(20);

    /* 15 00 02 b0 05 : Page 0x05 */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x05);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x52);
    /* Page 0x01 */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x01);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0x26);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0x26);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDC, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDD, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xE0, 0x26);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xE1, 0x26);

    /* Page 0x03 */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x03);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0x2A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xE7, 0x2A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0x2A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDE, 0x2A);

    /* Page 0x00 */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x03);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x8B);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x15);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x18);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0x14);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0x02);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0x14);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0x02);

    /* Page 0x06 */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x06);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0xA5);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD5, 0x20);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x00);

    /* Page 0x02 - Gamma Positive */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x02);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0x02);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0x06);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0x16);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0x0E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0x18);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0x26);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0x32);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0x3D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCF, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD0, 0x07);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD2, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD3, 0x02);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD4, 0x06);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD5, 0x12);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD6, 0x0A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD7, 0x14);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD8, 0x22);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xD9, 0x2E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDA, 0x3D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDB, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDC, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDD, 0x3F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDE, 0x3D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xDF, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xE0, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xE1, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xE2, 0x07);

    /* Page 0x07 - VCOM Positive */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x07);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB1, 0x24);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB2, 0x25);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x2F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB4, 0x49);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x62);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x79);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB7, 0xA6);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB8, 0xCC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB9, 0x22);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x71);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBB, 0xF8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBC, 0x6D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBD, 0x71);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBE, 0xD5);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x37);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x66);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0x98);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0xAD);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0xC1);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0xCB);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0xD4);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0xDD);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0xE1);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0xE4);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x56);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0xAF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0xFF);

    /* Page 0x08 - VCOM Negative */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x08);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB1, 0x1C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB2, 0x20);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x2E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB4, 0x49);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x64);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x7B);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB7, 0xA9);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB8, 0xD2);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB9, 0x2C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x7D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBB, 0x09);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBC, 0x80);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBD, 0x83);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBE, 0xE9);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x4D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x82);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0xAE);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0xC3);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0xD7);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0xE0);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0xE9);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0xF1);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0xF5);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0xF8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x5A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0xAF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0xFF);

    /* Page 0x09 - GAMMA */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x09);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB1, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB2, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x18);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB4, 0x49);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x64);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x7D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB7, 0xAC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB8, 0xD5);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB9, 0x30);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x84);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBB, 0x0F);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBC, 0x86);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBD, 0x8A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBE, 0xF4);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x5B);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x8A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0xB6);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0xCB);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0xDF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0xE8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0xF0);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0xF8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0xFA);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0xFC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x5A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0xAF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0xFF);

    /* Page 0x0A - GAMMA */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x0A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB1, 0x18);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB2, 0x19);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x2E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB4, 0x52);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x72);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x8C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB7, 0xBD);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB8, 0xEB);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB9, 0x47);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x96);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBB, 0x1E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBC, 0x90);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBD, 0x93);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBE, 0xFA);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x56);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x8C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0xB7);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0xCC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0xDF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0xE8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0xF0);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0xF8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0xFA);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0xFC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x5A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0xAF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0xFF);

    /* Page 0x0B - GAMMA */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x0B);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB1, 0x04);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB2, 0x15);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x2D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB4, 0x51);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x72);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x8D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB7, 0xBE);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB8, 0xED);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB9, 0x4A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x9A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBB, 0x23);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBC, 0x95);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBD, 0x98);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBE, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x59);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x8E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0xB9);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0xCD);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0xDF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0xE8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0xF0);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0xF8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0xFA);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0xFC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x5A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0xAF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0xFF);

    /* Page 0x0C - GAMMA */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x0C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB1, 0x04);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB2, 0x2C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x36);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB4, 0x53);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x73);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x8E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB7, 0xC0);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB8, 0xEF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB9, 0x4C);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBA, 0x9D);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBB, 0x25);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBC, 0x96);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBD, 0x9A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBE, 0x01);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xBF, 0x59);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC0, 0x8E);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC1, 0xB9);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC2, 0xCD);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC3, 0xDF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC4, 0xE8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC5, 0xF0);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC6, 0xF8);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC7, 0xFA);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC8, 0xFC);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xC9, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCA, 0x00);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCB, 0x5A);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCC, 0xBF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCD, 0xFF);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xCE, 0xFF);

    /* Page 0x04 */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x04);
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB5, 0x02);
    /* 15 0a 02 b6 01 : 10ms delay */
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB6, 0x01);
    sunxi_lcd_delay_ms(10);

    /* 15 64 02 11 00 : Exit Sleep Mode + 100ms */
    sunxi_lcd_dsi_gen_write_1para(sel, 0x11, 0x00);
    sunxi_lcd_delay_ms(100);

    /* 15 0a 02 29 00 : Display On + 10ms */
    sunxi_lcd_dsi_gen_write_1para(sel, 0x29, 0x00);
    sunxi_lcd_delay_ms(10);
}

/* 面板退出序列 */
static void lcd_panel_exit(u32 sel)
{
    /* 05 00 01 28 : Display Off */
    sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_SET_DISPLAY_OFF);
    /* 05 78 01 10 : Sleep In + 120ms */
    sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_ENTER_SLEEP_MODE);
    sunxi_lcd_delay_ms(120);
}

/* 用户自定义函数 */
static s32 lcd_user_defined_func(u32 sel, u32 para1, u32 para2, u32 para3)
{
    bsp_disp_lcd_set_bright(sel, 60);
    return 0;
}

struct __lcd_panel BOE_1200x1920_panel = {
    /* panel driver name, must match lcd_driver_name in mipi config */
    .name = "BOE_1200x1920",
    .func = {
        .cfg_panel_info = lcd_cfg_panel_info,
        .cfg_open_flow = lcd_open_flow,
        .cfg_close_flow = lcd_close_flow,
        .lcd_user_defined_func = lcd_user_defined_func,
    },
};
