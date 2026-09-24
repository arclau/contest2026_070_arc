#ifndef __GT9271_IIC_TOUCH_H_
#define __GT9271_IIC_TOUCH_H_

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

#define GT9271_I2C_FREQUENCY         100000    /* 100 kHz for 4.7k pull-up resistors */
#define GT9271_TOUCH_DATA_LEN        41        /* Length of touch data buffer */

#define GT9271_LCD_WIDTH             1200      /* LCD width in pixels (BOE 1200x1920) */
#define GT9271_LCD_HEIGHT            1920      /* LCD height in pixels (BOE 1200x1920) */

/* GT9271 I2C addresses (7-bit addresses) */
#define GT9271_I2C_ADDR_1            0x5D      /* 0xBA>>1 Primary I2C address  */
#define GT9271_I2C_ADDR_2            0x14      /* 0x28>>1 Alternative address */

/* GT9271 Register addresses (16-bit) */
#define GT9271_REG_COMMAND           0x8040    /* Real-time command register */
#define GT9271_REG_MODE_SWITCH       0x804D    /* Mode switch register */
#define GT9271_REG_COORD_RESOLUTION  0x8146    /* Coordinate resolution register */
#define GT9271_REG_COORD_ADDR        0x814E    /* Coordinate data register */
#define GT9271_REG_CHECK_SUM         0x80FF    /* Configuration checksum */
#define GT9271_REG_PRODUCT_ID        0x8140    /* Product ID register */
#define GT9271_REG_FIRMWARE_VERSION  0x8144    /* Firmware version register */

/* GT9271 Commands */
#define GT9271_CMD_READY             0x00      /* Exit gesture mode */
#define GT9271_CMD_GESTURE_MODE      0x04      /* Enter gesture mode */
#define GT9271_CMD_HOTKNOT_TX        0x21      /* HotKnot transmitter mode */
#define GT9271_CMD_HOTKNOT_RX        0x20      /* HotKnot receiver mode */

/* Touch point data structure for GT9271 */
#define GT9271_POINT_SIZE            8         /* Each touch point is 8 bytes */
#define GT9271_MAX_TOUCH_POINTS      5         /* Maximum contact points */

#define TOUCH_POINT_GET_STATUS(t)         ((t) & 0x80)
#define TOUCH_POINT_GET_NUM(t)            ((t) & 0x0F)
#define TOUCH_POINT_GET_LARGE(t)          ((t) & 0x40)
#define TOUCH_POINT_GET_ID(t)             ((t).id)
#define TOUCH_POINT_GET_X(t)              ((((t).xh & 0x0F) << 8) | (t).xl)
#define TOUCH_POINT_GET_Y(t)              ((((t).yh & 0x0F) << 8) | (t).yl)
#define TOUCH_POINT_GET_SIZE(t)           (((t).sizeh << 8) | (t).sizel)

struct gt9271_touch_point_s
{
  uint8_t id;        /* Touch point ID */
  uint8_t xl;
  uint8_t xh;
  uint8_t yl;
  uint8_t yh;
  uint8_t sizel;
  uint8_t sizeh;
  uint8_t reserved;
};
/* Describes all touch data returned by the gt9271 */
struct gt9271_touch_data_s
{
  uint8_t status_id;      //buff status/large detect/number of touch points
  struct gt9271_touch_point_s touch[GT9271_MAX_TOUCH_POINTS];
};

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif


int gt9271_register(FAR const char *devpath,FAR struct i2c_master_s *dev);
void gt9271_unregister(FAR const char *devpath);
#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __GT9271_IIC_TOUCH_H */
