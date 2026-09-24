/****************************************************************************
 * vendor/allwinnertech/boards/r528/drivers/gt9271_iic_touch.c
 *
 * GT9271 IIC Touchscreen Driver for R528 Platform
 * GT9271 capacitive touch controller driver
 *
 * Code-level port from the legacy SDK driver (GT911 IIC touch), following
 * the new SDK's GT911 driver framework. Register map is identical to GT911,
 * but this driver keeps the project-specific fixes:
 *   - product ID detection accepts "911"/"927" (GT911/GT9271/GT9272)
 *   - raw coordinates scaled to LCD pixel resolution (1200x1920)
 *   - I2C failure re-reset with pinmux restore (LCD pin_cfg race defense)
 *   - poll mode with adaptive poll interval
 *
 ****************************************************************************/

#include <nuttx/config.h>
#ifndef OPEN_MAX
#define OPEN_MAX 256
#endif
#ifndef CLOCK_MAX
#define CLOCK_MAX 4294967295U
#endif

#include <stdbool.h>
#include <stdint.h>
#include <debug.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/wqueue.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>
#include "aw_common.h"
#include "hal_gpio.h"
#include "gt9271_iic_touch.h"

#define POLL_MINDELAY  (16)
#define POLL_MAXDELAY  (100)
#define POLL_INCREMENT (8)

#define RST GPIOB(4)
#define INT GPIOB(5)

/* I2C pins (managed by TWI controller, set here for defense against pinmux glitches) */
#define I2C_SDA GPIOB(2)
#define I2C_SCL GPIOB(3)
#define I2C_PIN_MUXSEL 4
#define GPIO_SET(pin, val) hal_gpio_set_data(pin,val)

#define TOUCH_INVALID (1 << 0)
#define TOUCH_VALID   (1 << 1)
#define TOUCH_EVENT_MASK (0x00)
#define TOUCH_COUNT 0x0F
#define GT9271_MOVE_THRESH_PIX 4
#define HIGH true
#define LOW false
#define GT9271_TOUCH_POLLMODE

enum{
  ERROR_TRANSFER  = 1,
  ERROR_READ,
  ERROR_SLAVE,
  ERROR_REGISTERED,
  ERROR_REGISTER,
  ERROR_CONTROL,
  ERROR_IRQ,
};

/* GT9271 touch device instance */
struct gt9271_touch_dev_s
{
  struct touch_lowerhalf_s lower;         /* Standard touch lower half */
  uint8_t touch_buf[GT9271_TOUCH_DATA_LEN]; /* Raw touch data buffer */
  FAR struct i2c_master_s *i2c;           /* I2C driver instance */
  uint32_t  frequency;                    /* Current I2C frequency */
  struct work_s work;                     /* Work queue for touch processing */
  bool touch_valid;                       /* Valid touch data flag */
  uint8_t i2c_addr;                       /* Detected I2C address */
  uint32_t i2c_fail_count;                /* I2C failure counter */
  sem_t waitsem;                          /* Semaphore for ISR synchronization */
#ifdef GT9271_TOUCH_POLLMODE
  uint32_t poll_interval;                 /* Polling interval in milliseconds */
#else
  uint32_t irq;                           /* Interrupt line for GT9271 */
#endif
  uint8_t last_state;
  int16_t last_x;
  int16_t last_y;
  uint16_t touch_max_x;                 /* Touch panel max X (from chip config) */
  uint16_t touch_max_y;                 /* Touch panel max Y (from chip config) */
};

extern void up_udelay(useconds_t microseconds);

static int gt9271_detect_controller(FAR struct gt9271_touch_dev_s *priv);
/* GT9271 touch device instance */
static struct gt9271_touch_dev_s g_gt9271_touch = {
  .lower.maxpoint = GT9271_MAX_TOUCH_POINTS,
  .lower.control = NULL,
  .lower.write = NULL,
#ifdef GT9271_TOUCH_POLLMODE
  .poll_interval = POLL_MINDELAY,
#endif
  .frequency = GT9271_I2C_FREQUENCY,
  .last_state = TOUCH_INVALID,
  .last_x = 0,
  .last_y = 0,
};

static uint8_t data[GT9271_TOUCH_DATA_LEN];
static void gt9271_worker(FAR void *arg);

static void gt9271_reset_chip(FAR struct gt9271_touch_dev_s *priv)
{
  hal_gpio_set_data(RST, LOW);
  hal_gpio_set_data(INT, LOW);
  up_udelay(20000);
  hal_gpio_set_data(RST, HIGH);
  up_udelay(50000);
}

/* 集中配置 GT9271 全部相关引脚：
 *  - PB2/PB3: TWI(I2C) 功能复用
 *  - PB4: 触摸复位 RST (输出)
 *  - PB5: 触摸中断 INT (输出)
 * LCD 上电 (sunxi_lcd_pin_cfg) 可能改写 PB2~PB5 的 pinmux，因此
 * 初始化与 I2C 失败恢复路径都必须调用本函数，避免重复代码。 */
static void gt9271_pins_config(void)
{
  hal_gpio_pinmux_set_function(I2C_SDA, I2C_PIN_MUXSEL);
  hal_gpio_pinmux_set_function(I2C_SCL, I2C_PIN_MUXSEL);

  hal_gpio_pinmux_set_function(RST, GPIO_MUXSEL_OUT);
  hal_gpio_set_driving_level(RST, GPIO_DRIVING_LEVEL3);
  hal_gpio_set_direction(RST, GPIO_DIRECTION_OUTPUT);

  hal_gpio_pinmux_set_function(INT, GPIO_MUXSEL_OUT);
  hal_gpio_set_driving_level(INT, GPIO_DRIVING_LEVEL3);
  hal_gpio_set_direction(INT, GPIO_DIRECTION_OUTPUT);
}

static void gt9271_dump_hex(const char *tag, const uint8_t *data, size_t len)
{
  size_t i;
  char buf[128];
  int pos = 0;

  for (i = 0; i < len && pos < (int)sizeof(buf) - 4; i++)
    {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", data[i]);
    }
  printf("[hexdump %s] %zu bytes: %s\n", tag, len, buf);
}

static int gt9271_i2c_read(FAR struct gt9271_touch_dev_s *priv,
                              uint16_t regaddr, FAR uint8_t *buffer, size_t buflen)
{
  struct i2c_msg_s msg[2];
  uint8_t addr_buf[2];
  int ret;

  DEBUGASSERT(priv && priv->i2c);

  if (!priv->i2c) {
    printf("[GT9271] ERROR: I2C instance is NULL!\n");
    return -EINVAL;
  }

  addr_buf[0] = (regaddr >> 8) & 0xFF;
  addr_buf[1] = regaddr & 0xFF;

  msg[0].frequency = priv->frequency;
  msg[0].addr      = priv->i2c_addr;
  msg[0].flags     = 0;
  msg[0].buffer    = addr_buf;
  msg[0].length    = 2;

  msg[1].frequency = priv->frequency;
  msg[1].addr      = priv->i2c_addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buffer;
  msg[1].length    = buflen;

  ret = I2C_TRANSFER(priv->i2c, msg, 2);
  if (ret)
    {
      printf("[GT9271] I2C_READ FAIL: reg=0x%04X len=%zu slave=0x%02X ret=%d\n",
             regaddr, buflen, priv->i2c_addr, ret);
      return ERROR_TRANSFER;
    }

  return OK;
}

static int gt9271_i2c_write(FAR struct gt9271_touch_dev_s *priv,
                                uint16_t regaddr, uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t dat[3];
  int ret;

  DEBUGASSERT(priv && priv->i2c);

  dat[0] = (regaddr >> 8) & 0xFF;
  dat[1] = regaddr & 0xFF;
  dat[2] = value;

  msg.frequency = priv->frequency;
  msg.addr      = priv->i2c_addr;
  msg.flags     = 0;
  msg.buffer    = dat;
  msg.length    = 3;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);
  if (ret)
    {
      printf("[GT9271] I2C_WRITE FAIL: reg=0x%04X val=0x%02X slave=0x%02X ret=%d\n",
             regaddr, value, priv->i2c_addr, ret);
      return ERROR_TRANSFER;
    }

  return OK;
}

static int gt9271_detect_controller(FAR struct gt9271_touch_dev_s *priv)
{
  uint8_t product_id[5];
  int ret;

  memset(product_id, 0, sizeof(product_id));
  ret = gt9271_i2c_read(priv, GT9271_REG_PRODUCT_ID, product_id, 4);
  /* GT9xx product_id register returns 4 bytes: 3-char ID + 1 version byte.
   * e.g. GT911 → "911\0", GT9271 → "9271", GT9272 → "9272"
   * Truncate at [3] so strcmp only compares the 3-char model string. */
  product_id[3] = '\0';
  printf("[GT9271] detect ret=%d, product_id=[%s]\n",
         ret, product_id);
  if(strcmp((char*)product_id,"911") && strcmp((char*)product_id,"927")){
    printf("[GT9271] Unknown product ID: '%s'\n", product_id);
    return ERROR_READ;
  }
  printf("[GT9271] Detected GT%s touch controller\n", product_id);

  return OK;
}

static void gt9271_touch_process_event_one(uint8_t touch_state, FAR struct gt9271_touch_dev_s *priv, FAR struct touch_sample_s *sample){

  sample->npoints = GT9271_MAX_TOUCH_POINTS;
  sample->point[0].flags &= TOUCH_EVENT_MASK;

  if(touch_state == TOUCH_VALID){

    switch(priv->last_state){

      case TOUCH_INVALID:

        sample->point[0].flags |= TOUCH_DOWN;
        priv->last_state = TOUCH_VALID;
        touch_event(priv->lower.priv, sample);
        break;

      case TOUCH_VALID:

        if(abs(sample->point[0].x - priv->last_x) > GT9271_MOVE_THRESH_PIX ||
           abs(sample->point[0].y - priv->last_y) > GT9271_MOVE_THRESH_PIX)
        {
          sample->point[0].flags |= TOUCH_MOVE;
          priv->last_x = sample->point[0].x;
          priv->last_y = sample->point[0].y;
        }
        else
        {
          sample->point[0].flags |= TOUCH_DOWN;
          priv->last_x = sample->point[0].x;
          priv->last_y = sample->point[0].y;
        }
        touch_event(priv->lower.priv, sample);
        break;
    }
  }
  else{

    switch(priv->last_state){

      case TOUCH_INVALID:

       sample->npoints = 0;
       memset(&sample->point[0], 0, sizeof(sample->point[0]));
       break;

      case TOUCH_VALID:

       sample->npoints = GT9271_MAX_TOUCH_POINTS;
       sample->point[0].flags = TOUCH_UP;
       sample->point[0].x = priv->last_x;
       sample->point[0].y = priv->last_y;
       sample->point[0].timestamp = touch_get_time();
       priv->last_state = TOUCH_INVALID;
       priv->last_x = 0;
       priv->last_y = 0;
       touch_event(priv->lower.priv, sample);
       break;
    }
  }
}

static int gt9271_touch_process_data(FAR struct gt9271_touch_dev_s *priv,FAR struct touch_sample_s *sample)
{
  FAR struct gt9271_touch_data_s *raw = (FAR struct gt9271_touch_data_s *)priv->touch_buf;
  if(!TOUCH_POINT_GET_STATUS(raw->status_id) || !TOUCH_POINT_GET_NUM(raw->status_id) || TOUCH_POINT_GET_LARGE(raw->status_id)){

    gt9271_i2c_write(priv, GT9271_REG_COORD_ADDR, 0x00);
    return TOUCH_INVALID;
  }
  sample->npoints = TOUCH_POINT_GET_NUM(raw->status_id);
  if (sample->npoints > GT9271_MAX_TOUCH_POINTS)
    sample->npoints = GT9271_MAX_TOUCH_POINTS;

  for (int i = 0; i < sample->npoints; i++)
  {
    /* GT9271 returns coordinates in chip's native resolution
     * (touch_max_x x touch_max_y). Scale to pixel coordinates
     * then rotate 90° clockwise for landscape display.
     * BOE panel: 1200x1920 portrait, degree0=3 → fb 1920x1200 landscape.
     * G2D rotates fb content 90° CW onto the panel. Touch coords
     * must be mapped: logical X = 1920-1-py, logical Y = px. */
    uint16_t raw_x = TOUCH_POINT_GET_X(raw->touch[i]);
    uint16_t raw_y = TOUCH_POINT_GET_Y(raw->touch[i]);
    uint32_t div_x = (uint32_t)priv->touch_max_x + 1;
    uint32_t div_y = (uint32_t)priv->touch_max_y + 1;
    /* 物理面板坐标 (px, py) */
    uint16_t px = (uint16_t)((uint32_t)raw_x * GT9271_LCD_WIDTH / div_x);
    uint16_t py = (uint16_t)((uint32_t)raw_y * GT9271_LCD_HEIGHT / div_y);
    /* 顺时针 90° 旋转映射到逻辑 1920x1200 */
    sample->point[i].x = (uint16_t)(GT9271_LCD_HEIGHT - 1 - py);
    sample->point[i].y = px;
    sample->point[i].id       = TOUCH_POINT_GET_ID(raw->touch[i]);
    sample->point[i].h        = 0;
    sample->point[i].w        = 0;
    sample->point[i].pressure = TOUCH_POINT_GET_SIZE(raw->touch[i]);
    sample->point[i].timestamp = touch_get_time();
    memset(&raw->touch[i], 0, sizeof(raw->touch[i]));
  }

  raw->status_id = 0;
  gt9271_i2c_write(priv, GT9271_REG_COORD_ADDR, 0x00);
  return TOUCH_VALID;
}

static int gt9271_control_initialize(void){

  int ret;
  FAR struct gt9271_touch_dev_s *priv = &g_gt9271_touch;

  printf("[GT9271] ===== INIT START =====\n");
  printf("[GT9271] GPIO: RST=PB%d INT=PB%d\n", 4, 5);

  /* Ensure I2C pins (PB2/PB3) are in TWI function mode before any I2C operation.
   * TWI driver sets these during init, but LCD init may glitch pinmux registers.
   * Setting them here provides defense-in-depth against cross-CPU pinmux race. */
  gt9271_pins_config();

  printf("[GT9271] Reset seq #1: RST=LOW INT=LOW -> 20ms -> RST=HIGH -> 50ms\n");
  priv->i2c_addr = GT9271_I2C_ADDR_1;

  hal_gpio_set_data(RST, LOW);
  hal_gpio_set_data(INT, LOW);
  up_udelay(20000);
  hal_gpio_set_data(RST, HIGH);
  up_udelay(50000);

#ifndef GT9271_TOUCH_POLLMODE
  hal_gpio_set_data(INT, HIGH);
  hal_gpio_pinmux_set_function(INT, GPIO_MUXSEL_IN);
  hal_gpio_set_direction(INT, GPIO_DIRECTION_INPUT);
  hal_gpio_set_pull(INT, GPIO_PULL_DOWN_DISABLED);
  up_udelay(20000);
#endif

  printf("[GT9271] Try addr 0x%02X...\n", priv->i2c_addr);
  ret = gt9271_detect_controller(priv);

  if(ret) {
    printf("[GT9271] Reset seq #2: RST=LOW INT=HIGH -> 20ms -> RST=HIGH -> 5ms -> INT=LOW -> 50ms\n");
    priv->i2c_addr = GT9271_I2C_ADDR_2;
    /* 确保 INT 恢复为输出 (非 POLLMODE 下首次检测后 INT 被设为输入) */
    gt9271_pins_config();

    hal_gpio_set_data(RST, LOW);
    hal_gpio_set_data(INT, HIGH);
    up_udelay(20000);

    hal_gpio_set_data(RST, HIGH);
    up_udelay(5000);
    hal_gpio_set_data(INT, LOW);
    up_udelay(50000);

    printf("[GT9271] Try addr 0x%02X...\n", priv->i2c_addr);
    ret = gt9271_detect_controller(priv);
    if(ret != OK) {
      printf("[GT9271] ===== INIT FAIL: both addresses failed =====\n");
      return ERROR_SLAVE;
    }
  }

  /* Read touch panel max X/Y resolution from chip config registers
   * 0x8146-0x8147: X max, 0x8148-0x8149: Y max (16-bit LE) */
  {
    uint8_t res_buf[4];
    if (gt9271_i2c_read(priv, GT9271_REG_COORD_RESOLUTION, res_buf, 4) == 0)
      {
        priv->touch_max_x = (uint16_t)res_buf[0] | ((uint16_t)res_buf[1] << 8);
        priv->touch_max_y = (uint16_t)res_buf[2] | ((uint16_t)res_buf[3] << 8);
      }
    else
      {
        priv->touch_max_x = 4095;
        priv->touch_max_y = 4095;
      }
    printf("[GT9271] touch_max_x=%u touch_max_y=%u\n",
           priv->touch_max_x, priv->touch_max_y);
  }

  return OK;
}

static void gt9271_worker(FAR void *arg){

  FAR struct gt9271_touch_dev_s *priv = (FAR struct gt9271_touch_dev_s *)arg;
  FAR struct touch_sample_s *sample = (FAR struct touch_sample_s *)data;
  uint8_t status;
  uint8_t touch_num;
  uint8_t touch_state;
  int ret;

  DEBUGASSERT(priv);

  ret = gt9271_i2c_read(priv, GT9271_REG_COORD_ADDR, &status, 1);
  if(ret){
    priv->i2c_fail_count++;
    printf("[GT9271] I2C fail #%lu (ret=%d), re-reset + retry\n",
           (unsigned long)priv->i2c_fail_count, ret);
    printf("[GT9271]   last_ok=%lu poll=%lu\n",
           (unsigned long)priv->i2c_fail_count - 1,
           (unsigned long)priv->poll_interval);
    /* LCD power-on (sunxi_lcd_pin_cfg) may change pinmux of PB2/PB3/PB4/PB5.
     * Restore ALL GT9271-related pins before chip reset, same as init. */
    gt9271_pins_config();
    gt9271_reset_chip(priv);
    printf("[GT9271]   reset done, retrying read...\n");
    ret = gt9271_i2c_read(priv, GT9271_REG_COORD_ADDR, &status, 1);
    if(ret){
      printf("[GT9271] retry ALSO failed (ret=%d), keeping old poll\n", ret);
      priv->poll_interval = 500;
      goto error_read;
    }
    printf("[GT9271] retry SUCCEEDED after re-reset!\n");
  }

  priv->i2c_fail_count = 0;

  if(!(status & 0x80)){
    priv->poll_interval += POLL_INCREMENT;
    if(priv->poll_interval > POLL_MAXDELAY)
      priv->poll_interval = POLL_MAXDELAY;
    goto error_read;
  }

  touch_num = status & 0x0f;
  if(touch_num == 0){
    gt9271_i2c_write(priv, GT9271_REG_COORD_ADDR, 0);
    up_udelay(2000);
    if (priv->last_state == TOUCH_VALID)
      {
        gt9271_touch_process_event_one(TOUCH_INVALID, priv, sample);
      }
    priv->poll_interval = POLL_MINDELAY;
    goto error_read;
  }
  if(touch_num > GT9271_MAX_TOUCH_POINTS)
    touch_num = GT9271_MAX_TOUCH_POINTS;

  ret = gt9271_i2c_read(priv, GT9271_REG_COORD_ADDR, priv->touch_buf,
                       1 + touch_num * GT9271_POINT_SIZE);
  if(ret){
    priv->i2c_fail_count++;
    priv->poll_interval = 1000;
    goto error_read;
  }

  gt9271_i2c_write(priv, GT9271_REG_COORD_ADDR, 0);

  touch_state = gt9271_touch_process_data(priv, sample);
  /* 关闭调试日志，避免刷屏 */
  /* printf("[GT9271] TOUCH touch_num=%d coord=(%d,%d) state=%d\n",
         touch_num,
         (int)sample->point[0].x,
         (int)sample->point[0].y,
         touch_state); */
  gt9271_touch_process_event_one(touch_state, priv, sample);

  priv->poll_interval = POLL_MINDELAY;

error_read:
  work_queue(HPWORK, &priv->work, gt9271_worker, priv, priv->poll_interval);
}

#ifndef GT9271_TOUCH_POLLMODE
static hal_irqreturn_t gt9271_interrupt_handler(FAR void *arg){

  int ret = 0;
  FAR struct gt9271_touch_dev_s *priv = (FAR struct gt9271_touch_dev_s *)arg;
  DEBUGASSERT(priv->work.worker == NULL);

  ret = work_queue(HPWORK, &priv->work, gt9271_worker, priv, 0);
  if (ret != 0)
    {
      ierr("ERROR: Failed to queue work: %d\n", ret);
      return HAL_IRQ_ERR;
    }
  return HAL_IRQ_OK;
}

#endif

int gt9271_register(FAR const char *devpath,FAR struct i2c_master_s *dev)
{
  FAR struct gt9271_touch_dev_s *priv = &g_gt9271_touch;
  int ret;

  if(dev == NULL){
    ierr("GT9271: ERROR: i2c_master_s instance is NULL\n");
    return ERROR_REGISTERED;
  }
  priv->i2c = dev;

  if(gt9271_control_initialize()){
    ierr("GT9271: ERROR: Failed to initialize control\n");
    return ERROR_CONTROL;
    }

#ifndef  GT9271_TOUCH_POLLMODE
  hal_gpio_to_irq(INT, &priv->irq);
  hal_gpio_irq_request(priv->irq, gt9271_interrupt_handler, IRQ_TYPE_EDGE_RISING, priv);
  nxsem_init(&priv->waitsem, 0, 0);
#endif
  ret = touch_register(&priv->lower, devpath, GT9271_MAX_TOUCH_POINTS);
  if (ret < 0)
    {
      ierr("GT9271: ERROR: Failed to register touch driver: %d\n", ret);
      goto errout;
    }
  iinfo("GT9271: IIC touchscreen successfully initialized and registered\n");
#ifdef GT9271_TOUCH_POLLMODE
  work_queue(HPWORK, &priv->work, gt9271_worker, priv, priv->poll_interval);
#else
  hal_gpio_irq_enable(priv->irq);
#endif
  return OK;

errout:
  nxsem_destroy(&priv->waitsem);
  return ERROR_REGISTER;
}

void gt9271_unregister(FAR const char *devpath){

  FAR struct gt9271_touch_dev_s *priv = &g_gt9271_touch;
#ifndef GT9271_TOUCH_POLLMODE
  hal_gpio_irq_disable(priv->irq);
  hal_gpio_irq_free(priv->irq);
#endif
  touch_unregister(&priv->lower, devpath);
  priv->i2c = NULL;
}
