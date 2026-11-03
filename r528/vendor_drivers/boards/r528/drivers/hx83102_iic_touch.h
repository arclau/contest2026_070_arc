/****************************************************************************
 * vendor/allwinnertech/boards/r528/drivers/hx83102_iic_touch.h
 *
 * HX83102 IIC Touchscreen Driver for R528 Platform (NuttX/OpenVela)
 *
 * Ported from the Linux Himax HX83102 driver
 * (Firefly_SDK/.../hxchipset/) to the NuttX touch_lowerhalf framework,
 * modeled on the in-tree gt9271_iic_touch.c.
 *
 * Resident-firmware BOE/HX83102AB support.  Protocol constants follow the
 * working F101 implementation; R528 touch accuracy and stability require
 * board validation.
 *
 ****************************************************************************/

#ifndef __HX83102_IIC_TOUCH_H
#define __HX83102_IIC_TOUCH_H

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

/* -----------------------------------------------------------------------
 * Pin wiring (same as the legacy GT9271 on Gemini-S1)
 *   TWI  : PB2 (SDA) / PB3 (SCL)  -- managed by the TWI controller
 *   RST  : PB4                      -- output high via 3.3V->1.8V level
 *                                      shifter (P286-E8); no boot reset
 *   INT  : PB5                      -- not level-shifted; input, no pull
 *                                      (diagnostic sampling only)
 * ----------------------------------------------------------------------- */
#define HX83102_RST_GPIO   GPIOB(4)
#define HX83102_INT_GPIO   GPIOB(5)
#define HX83102_I2C_SDA    GPIOB(2)
#define HX83102_I2C_SCL    GPIOB(3)
#define HX83102_I2C_PIN_MUXSEL 4

/* P286-E19: back to the E15 baseline (50 kHz); the only E19 variable is the
 * TWI bus-idle wait before each transfer. */
#define HX83102_I2C_FREQUENCY   50000
#define HX83102_I2C_ADDR        0x48     /* 7-bit slave address (0x90 >> 1) */

/* -----------------------------------------------------------------------
 * Touch report geometry (event stack read from 0x30, 56 bytes)
 *     [0 .. 39]  : 10 slots x (x_hi, x_lo, y_hi, y_lo); unused = 0xFFFF
 *     [40 .. 51] : padding / per-point width and pressure
 *     [52]       : high nibble raw_out_sel (0xF = normal), low nibble points
 *     [53 .. 55] : status/checksum tail
 *   Require sum modulo 256 == 0 and reject all-zero frames separately.
 *   Point count is byte 52 - reading
 *   it from the coordinate tail (offset 40) always yields padding zeros.
 *   Layout confirmed on the same BOE/HX83102B module in F101 TOUCH.md.
 * ----------------------------------------------------------------------- */
#define HX83102_MAX_TOUCH_POINTS   10
#define HX83102_FRAME_LEN          56
#define HX83102_POINT_CNT_OFFSET   52
#define HX83102_TOUCH_DATA_LEN     HX83102_FRAME_LEN

/* -----------------------------------------------------------------------
 * Panel / touch resolution
 *   The DISPLAY native panel is 720x1440 (portrait) and is shown as a
 *   1440x720 landscape framebuffer via Allwinner degree0=3 rotation.
 *   The TOUCH controller reports 720x1440 portrait coordinates, confirmed
 *   on the same BOE/HX83102B module in F101 (a frame decoded as x=620,
 *   y=292, both inside 720x1440).  The driver rotates them 90 deg into the
 *   1440x720 landscape framebuffer; the direction still needs an on-screen
 *   four-corner check.
 * ----------------------------------------------------------------------- */
#define HX83102_TOUCH_NATIVE_X    720
#define HX83102_TOUCH_NATIVE_Y    1440
#define HX83102_LCD_WIDTH         1440  /* logical landscape width  */
#define HX83102_LCD_HEIGHT        720   /* logical landscape height */

/* -----------------------------------------------------------------------
 * Himax AHB register access (1-byte command I2C protocol)
 *   A register read/write is: write 4-byte AHB address to cmd 0x00, then
 *   read data from cmd 0x08 (or write data to cmd 0x00). Burst mode
 *   (continuous) is enabled via cmd 0x13 / cmd 0x0D.
 * ----------------------------------------------------------------------- */
#define HX83102_AHB_ADDR_BYTE_0   0x00   /* AHB address / data port */
#define HX83102_AHB_ACCESS_DIR    0x0C   /* access direction; 0x00 = read (Linux ic_cmd_ahb_access_direction_read) */
#define HX83102_AHB_RDATA_BYTE_0  0x08   /* AHB read-data port */
#define HX83102_BURST_CONTI       0x13   /* continuous-burst enable  (val 0x31) */
#define HX83102_BURST_CONTI_VAL   0x31
#define HX83102_BURST_INCR4       0x0D   /* auto-increment enable (val 0x10|auto) */
#define HX83102_BURST_INCR4_VAL   0x10
#define HX83102_AHB_BURST_CTRL    0x11   /* AHB/I2C burst switch */
#define HX83102_AHB_BURST_OFF     0x00
#define HX83102_AHB_BURST_ON      0x01

/* Event / coordinate report address (read touch data here) */
#define HX83102_ADDR_EVENT        0x30

#ifdef __cplusplus
extern "C"
{
#endif

int hx83102_register(FAR const char *devpath, FAR struct i2c_master_s *dev);
void hx83102_unregister(FAR const char *devpath);

#ifdef __cplusplus
}
#endif

#endif /* __HX83102_IIC_TOUCH_H */
