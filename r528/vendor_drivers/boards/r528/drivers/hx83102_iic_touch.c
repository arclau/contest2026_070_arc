/****************************************************************************
 * vendor/allwinnertech/boards/r528/drivers/hx83102_iic_touch.c
 *
 * HX83102 IIC Touchscreen Driver for R528 Platform (NuttX/OpenVela)
 *
 * Ported from the Linux Himax HX83102 driver
 * (Firefly_SDK/kernel-6.1/drivers/input/touchscreen/hxchipset/) to the
 * NuttX touch_lowerhalf framework, modeled on the in-tree gt9271 driver.
 *
 * The BOE module runs resident firmware.  Bring-up checks the AHB bridge
 * and controller ID before registering the NuttX touch device.  Runtime
 * reads 56-byte event reports directly from command 0x30.
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

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/wqueue.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>

#include <arch/board/board.h>
#include "aw_common.h"
#include "hal_gpio.h"
#include "hx83102_iic_touch.h"

/* Current board-test cadence.  PB5's logic level is not yet reliable, so
 * idle reads are timed by contact state.  These values remain unchanged
 * during the E13 initialization experiment. */

#define HX83102_POLL_MS     20
/* P286-E22: with no contact tracked the worker only read the event stack once
 * per second (E11 set 1000 ms while the link was still being hammered).  A
 * 100-200 ms tap falls entirely between two reads, which is exactly the
 * "tapped but nothing happened" report.  F101 runs IDLE_MS=200 on the same
 * panel; 26 ms is the documented danger zone, 100 ms keeps a 4x margin. */
#define HX83102_IDLE_MS     100
/* P286-R2: the trigger for a device recovery is "no valid frame for a while",
 * not "N consecutive errors".  The HAL already retries every transaction, so
 * an error count fires on a link that is working. */
#define HX83102_STALL_MS          600
#define HX83102_RECOVER_GUARD_MS  250
#define HX83102_RELEASE_MS  200
#define HX83102_DUMP_FRAMES 3

#define HX83102_PROBE_TRIES      5
#define HX83102_RESET_SETTLE_MS  200
#define HX83102_PROBE_GAP_MS     50
/* P286-E22: 5 s cap made a burst of bad frames feel like a dead panel. */
#define HX83102_BACKOFF_MAX_MS   500

struct hx83102_touch_dev_s
{
  struct touch_lowerhalf_s lower;          /* Standard touch lower half */
  uint8_t touch_buf[HX83102_TOUCH_DATA_LEN];
  FAR struct i2c_master_s *i2c;
  uint32_t frequency;
  struct work_s work;
  spinlock_t work_lock;     /* Serializes stop against worker requeue */
  bool running;
  uint8_t i2c_addr;
  uint32_t i2c_fail_count;
  uint32_t last_poll_ms;    /* uptime of the last event read */
  uint32_t last_good_ms;    /* uptime of the last accepted frame */
  uint32_t last_reident_ms; /* uptime of the last re-identify (E11) */
  uint8_t err_streak;       /* consecutive failed/invalid event reads */
  uint8_t recover_tries;    /* recovery attempts since the last valid frame */
  int8_t int_level;         /* last sampled INT level, -1 before first poll */
  uint8_t last_state;
  int16_t last_x;
  int16_t last_y;
  /* Touch-panel native resolution (from chip firmware). Used to scale
   * raw coordinates into logical LCD pixels. TODO: confirm from panel. */
  uint16_t touch_max_x;
  uint16_t touch_max_y;
};

static int hx83102_detect_controller(FAR struct hx83102_touch_dev_s *priv);
static void hx83102_worker(FAR void *arg);

/* P286-E3 diagnostic: dump the raw 0x30 frame while the read cadence is being
 * reduced.  Bounded so it cannot flood a 20 ms polling loop. */

static uint8_t g_dump_left = HX83102_DUMP_FRAMES;

/* P286-R2c: the boot-time budget above is spent before the first dropout, so
 * nothing showed what the IC actually returns while the link is down.  Dump
 * the frame that starts each stall, bounded so a dead panel cannot flood. */

static uint8_t g_stall_dumps = 12;
static uint8_t g_first_bad_dumps = 6;
static bool g_seen_valid;

static void hx83102_dump_frame(FAR const char *tag, FAR const uint8_t *f)
{
  char hex[HX83102_FRAME_LEN * 3 + 1];
  int i;

  for (i = 0; i < HX83102_FRAME_LEN; i++)
    {
      snprintf(&hex[i * 3], 4, " %02x", f[i]);
    }

  printf("[HX83102] %s%s\n", tag, hex);
}

/* P286-E10: up_udelay()/up_mdelay() busy-wait on CONFIG_BOARD_LOOPSPERMSEC,
 * which is wrong for this board's CPU frequency (measured ~200x too fast:
 * up_udelay(200000) returned in <=1 ms across 4 boots, so every "settle"
 * and "retry gap" below was a no-op).  usleep sleeps on the system tick
 * (CONFIG_USEC_PER_TICK=1000), so sub-millisecond requests round up to one
 * tick.  Precedent: the realtek touch driver in this tree uses usleep.
 */

static void hx83102_delay_ms(unsigned int ms)
{
  usleep(ms * 1000);
}

static uint32_t hx83102_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/* INT is driven by the controller and is active low.  Returns -1 if the
 * GPIO could not be sampled. */

static int hx83102_int_level(void)
{
  gpio_data_t level = GPIO_DATA_HIGH;

  if (hal_gpio_get_data(HX83102_INT_GPIO, &level) != 0)
    {
      return -1;
    }

  return (int)level;
}

static struct hx83102_touch_dev_s g_hx83102_touch =
{
  .lower.maxpoint = 1,      /* Single-contact output; wire format has 10 slots */
  .lower.control = NULL,
  .lower.write = NULL,
  .frequency = HX83102_I2C_FREQUENCY,
  .int_level = -1,
  .last_state = 1,           /* TOUCH_INVALID */
  .last_x = 0,
  .last_y = 0,
  .touch_max_x = HX83102_TOUCH_NATIVE_X,
  .touch_max_y = HX83102_TOUCH_NATIVE_Y,
};

static struct touch_sample_s g_sample;
static mutex_t g_lifecycle_lock = NXMUTEX_INITIALIZER;

/* -----------------------------------------------------------------------
 * Pin configuration / reset (wiring identical to GT9271 on Gemini-S1)
 * ----------------------------------------------------------------------- */
static void hx83102_pins_config(void)
{
  hal_gpio_pinmux_set_function(HX83102_I2C_SDA, HX83102_I2C_PIN_MUXSEL);
  hal_gpio_pinmux_set_function(HX83102_I2C_SCL, HX83102_I2C_PIN_MUXSEL);

  /* P286-E8: with the level converter in place, drive RST high through
   * PB4 again.  Preload the DATA register high while the pin is still an
   * input, then switch to output, so no active-low pulse is produced
   * (order verified on E2, where touch worked).  Keep the internal pull
   * disabled: the panel-side 1.8 V pull-up defines the idle level. */
  hal_gpio_set_data(HX83102_RST_GPIO, GPIO_DATA_HIGH);
  hal_gpio_set_pull(HX83102_RST_GPIO, GPIO_PULL_DOWN_DISABLED);
  hal_gpio_pinmux_set_function(HX83102_RST_GPIO, GPIO_MUXSEL_OUT);
  hal_gpio_set_direction(HX83102_RST_GPIO, GPIO_DIRECTION_OUTPUT);

  /* INT is driven by the touch controller and has an external 1.8 V pull-up.
   * Keep the R528's internal pull-up disabled while sampling PB5. */
  hal_gpio_pinmux_set_function(HX83102_INT_GPIO, GPIO_MUXSEL_IN);
  hal_gpio_set_pull(HX83102_INT_GPIO, GPIO_PULL_DOWN_DISABLED);
  hal_gpio_set_direction(HX83102_INT_GPIO, GPIO_DIRECTION_INPUT);
}

static void hx83102_release_reset(void)
{
  /* Release active-low RST through the level converter.  This bring-up
   * path does not issue a reset pulse. */
  hal_gpio_set_data(HX83102_RST_GPIO, GPIO_DATA_HIGH);
}

/* P286-E14: a wedged HX83102B keeps NACKing 0x48 until its RST is pulsed;
 * observed on the board after the bus itself had been released.  This is
 * only used when init has already failed, so it never touches a working
 * panel during a normal boot.  PB4 reaches the module through the 1.8 V
 * level shifter installed for E8. */
static void hx83102_reset_pulse(void)
{
  printf("[HX83102] E14: RST pulse 10 ms low\n");
  hal_gpio_set_data(HX83102_RST_GPIO, GPIO_DATA_LOW);
  hx83102_delay_ms(10);
  hal_gpio_set_data(HX83102_RST_GPIO, GPIO_DATA_HIGH);
  hx83102_delay_ms(HX83102_RESET_SETTLE_MS);
  printf("[HX83102] E14: RST released after pulse, INT=%d\n",
         hx83102_int_level());
}

/* -----------------------------------------------------------------------
 * I2C transport: Himax uses a 1-byte command protocol (unlike GT9xx which
 * uses a 2-byte register address).
 *   read :  [cmd] -> [data...]
 *   write:  [cmd][data...]
 * ----------------------------------------------------------------------- */
static int hx83102_i2c_read_cmd(FAR struct hx83102_touch_dev_s *priv,
                                uint8_t command, FAR uint8_t *buffer,
                                size_t buflen)
{
  struct i2c_msg_s msg[2];
  int ret;

  msg[0].frequency = priv->frequency;
  msg[0].addr      = priv->i2c_addr;
  msg[0].flags     = 0;
  msg[0].buffer    = &command;
  msg[0].length    = 1;

  msg[1].frequency = priv->frequency;
  msg[1].addr      = priv->i2c_addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buffer;
  msg[1].length    = buflen;

  ret = I2C_TRANSFER(priv->i2c, msg, 2);
  if (ret)
    {
      printf("[HX83102] i2c read cmd=0x%02x len=%u transfer=%d\n",
             command, (unsigned int)buflen, ret);
      return ret < 0 ? ret : -EIO;
    }

  return OK;
}

static int hx83102_i2c_write_cmd(FAR struct hx83102_touch_dev_s *priv,
                                 uint8_t command, FAR const uint8_t *buffer,
                                 size_t buflen)
{
  struct i2c_msg_s msg;
  uint8_t *buf;
  int ret;

  buf = kmm_malloc(1 + buflen);
  if (!buf)
    return -ENOMEM;

  buf[0] = command;
  if (buflen)
    memcpy(buf + 1, buffer, buflen);

  msg.frequency = priv->frequency;
  msg.addr      = priv->i2c_addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 1 + buflen;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);
  kmm_free(buf);
  if (ret)
    {
      printf("[HX83102] i2c write cmd=0x%02x len=%u transfer=%d\n",
             command, (unsigned int)buflen, ret);
      return ret < 0 ? ret : -EIO;
    }

  return OK;
}

/* P286-R4a: Firefly switches the AHB/I2C burst path off around every
 * event-stack read (0x11=0x00 before 0x30, 0x11=0x01 after).  Leaving it
 * enabled was the protocol difference while SDA stayed low during drags. */

static int hx83102_event_read(FAR struct hx83102_touch_dev_s *priv,
                              FAR uint8_t *buffer, size_t buflen)
{
  uint8_t burst = HX83102_AHB_BURST_OFF;
  int wret;
  int ret;

  ret = hx83102_i2c_write_cmd(priv, HX83102_AHB_BURST_CTRL, &burst, 1);
  if (ret == OK)
    {
      ret = hx83102_i2c_read_cmd(priv, HX83102_ADDR_EVENT, buffer, buflen);
    }
  else
    {
      printf("[HX83102] burst off failed ret=%d\n", ret);
    }

  burst = HX83102_AHB_BURST_ON;
  wret = hx83102_i2c_write_cmd(priv, HX83102_AHB_BURST_CTRL, &burst, 1);
  if (wret != OK)
    {
      printf("[HX83102] burst on failed ret=%d (event read=%d)\n",
             wret, ret);
      if (ret == OK)
        {
          ret = wret;
        }
    }

  return ret;
}

/* -----------------------------------------------------------------------
 * Coordinate parse (Himax report format) + rotation to landscape.
 * Reports a single active finger (matches the proven gt9271 model;
 * multi-touch state tracking is a TODO).
 * ----------------------------------------------------------------------- */
static void hx83102_touch_process_event_one(uint8_t touch_state,
                                           FAR struct hx83102_touch_dev_s *priv,
                                           FAR struct touch_sample_s *sample)
{
  sample->npoints = 1;
  sample->point[0].id = 0;
  sample->point[0].flags = TOUCH_POS_VALID | TOUCH_ID_VALID;

  if (touch_state == 1)
    {
      if (priv->last_state != 0)
        {
          sample->point[0].flags |= TOUCH_DOWN;
        }
      else
        {
          /* A repeated frame is still the same contact, never another DOWN. */

          sample->point[0].flags |= TOUCH_MOVE;
        }

      priv->last_state = 0;
      priv->last_x = sample->point[0].x;
      priv->last_y = sample->point[0].y;
    }
  else
    {
      if (priv->last_state != 0)
        {
          return;
        }

      sample->point[0].flags |= TOUCH_UP;
      sample->point[0].x = priv->last_x;
      sample->point[0].y = priv->last_y;
      sample->point[0].timestamp = touch_get_time();
      priv->last_state = 1;
    }

  touch_event(priv->lower.priv, sample);
}

/* Decode one 56-byte event-stack frame.
 *
 *   [0 .. 39]  : 10 slots x (x_hi, x_lo, y_hi, y_lo); unused slots are 0xFFFF
 *   [40 .. 51] : padding / per-point width and pressure
 *   [52]       : high nibble raw_out_sel, low nibble point count (0xFF = none)
 *   [53 .. 55] : status/checksum tail
 *
 * The 56 bytes must sum to 0 (mod 256).
 *
 * An all-zero stack is how this controller answers a poll while nothing is
 * pending (Firefly only treats it as ESD after >5 zero events on INT-driven
 * reads).  Accept it as idle while no contact is tracked; while a contact is
 * tracked it stays a fault so a dead link still synthesizes a release.
 *
 * Returns 0 = contact (sample filled), 1 = valid idle frame, -1 = rejected.
 */

static int hx83102_decode(FAR struct hx83102_touch_dev_s *priv,
                          FAR struct touch_sample_s *sample)
{
  FAR const uint8_t *f = priv->touch_buf;
  unsigned int sum = 0;
  unsigned int nonzero = 0;
  unsigned int count;
  unsigned int found = 0;
  int first = -1;
  int i;

  for (i = 0; i < HX83102_FRAME_LEN; i++)
    {
      sum += f[i];
      nonzero |= f[i];
    }

  if ((sum & 0xFF) != 0)
    {
      return -1;
    }

  if (nonzero == 0)
    {
      return priv->last_state != 0 ? 1 : -1;
    }

  count = (f[HX83102_POINT_CNT_OFFSET] == 0xFF)
          ? 0 : (unsigned int)(f[HX83102_POINT_CNT_OFFSET] & 0x0F);
  if (count > HX83102_MAX_TOUCH_POINTS)
    {
      return -1;
    }

  if (count == 0)
    {
      return 1;
    }

  /* Slots are sparse and unused slots read back as 0xFFFF, so a slot is
   * valid only when both coordinates are inside the native panel. */

  for (i = 0; i < HX83102_MAX_TOUCH_POINTS; i++)
    {
      uint16_t x = (uint16_t)((f[i * 4] << 8) | f[i * 4 + 1]);
      uint16_t y = (uint16_t)((f[i * 4 + 2] << 8) | f[i * 4 + 3]);

      if (x < HX83102_TOUCH_NATIVE_X && y < HX83102_TOUCH_NATIVE_Y)
        {
          if (first < 0)
            {
              first = i;
            }

          found++;
        }
    }

  if (found != count || first < 0)
    {
      return -1;
    }

  /* Single-touch model: report the first valid slot.  Native coords are
   * 720x1440 portrait; rotate 90 deg into the 1440x720 landscape frame
   * (same transform as F101's touch_point()). */

  {
    uint16_t raw_x = (uint16_t)((f[first * 4] << 8) | f[first * 4 + 1]);
    uint16_t raw_y = (uint16_t)((f[first * 4 + 2] << 8) | f[first * 4 + 3]);
    uint16_t nx = (uint16_t)((uint32_t)raw_x * HX83102_LCD_HEIGHT
                             / HX83102_TOUCH_NATIVE_X);
    uint16_t ny = (uint16_t)((uint32_t)raw_y * HX83102_LCD_WIDTH
                             / HX83102_TOUCH_NATIVE_Y);

    sample->npoints = 1;
    sample->point[0].x = (uint16_t)(HX83102_LCD_WIDTH - 1 - ny);
    sample->point[0].y = nx;
    sample->point[0].id       = 0;
    sample->point[0].h        = 0;
    sample->point[0].w        = 0;
    sample->point[0].pressure = 0;
    sample->point[0].timestamp = touch_get_time();
  }

  return 0;
}

/* P286-E15: on this link a write can report NACK/BUS_ERR after the
 * controller has already committed the payload.  The board console applied
 * 0x13=0x31 despite BUS_ERR and then read back 0x31.  A transport result is
 * therefore not a verdict; read the bridge register back and let that
 * decide.  A failed readback is still fatal. */

static int hx83102_write_verify(FAR struct hx83102_touch_dev_s *priv,
                                uint8_t command, uint8_t value)
{
  uint8_t readback = 0;
  int wret;
  int ret;

  wret = hx83102_i2c_write_cmd(priv, command, &value, 1);
  if (wret != OK)
    {
      printf("[HX83102] E15 write cmd=0x%02x ret=%d, readback decides\n",
             command, wret);
    }

  ret = hx83102_i2c_read_cmd(priv, command, &readback, 1);
  if (ret != OK)
    {
      return ret;
    }

  if (readback != value)
    {
      printf("[HX83102] bridge 0x%02x readback=0x%02x expected=0x%02x\n",
             command, readback, value);
      return -EPROTO;
    }

  return OK;
}

/* Establish the AHB bridge before using its address/data ports.  The wake
 * read must succeed; each configuration write is proven by its readback. */

static int hx83102_interface_on(FAR struct hx83102_touch_dev_s *priv)
{
  uint8_t wake;
  int ret;

  ret = hx83102_i2c_read_cmd(priv, HX83102_AHB_RDATA_BYTE_0, &wake, 1);
  if (ret != OK)
    {
      return ret;
    }

  ret = hx83102_write_verify(priv, HX83102_BURST_CONTI,
                             HX83102_BURST_CONTI_VAL);
  if (ret != OK)
    {
      return ret;
    }

  return hx83102_write_verify(priv, HX83102_BURST_INCR4,
                              HX83102_BURST_INCR4_VAL);
}

/* Identify the controller through the AHB bridge: 0x00/0x0C/0x08 read the
 * ID at 0xd0000090.  Expect 00 2b 10 83 (HX83102AB). */

static int hx83102_detect_controller(FAR struct hx83102_touch_dev_s *priv)
{
  const uint8_t addr_id[4] = {0xd0, 0x00, 0x00, 0x90};
  uint8_t id[4] = {0, 0, 0, 0};
  uint8_t dir = 0;
  int ret = hx83102_interface_on(priv);

  if (ret != OK)
    {
      printf("[HX83102] bridge init failed ret=%d\n", ret);
      return ret;
    }

  /* The AHB address and direction writes are proven by the ID read below;
   * their transport result alone is ambiguous on this link (E15). */

  ret = hx83102_i2c_write_cmd(priv, HX83102_AHB_ADDR_BYTE_0,
                              addr_id, sizeof(addr_id));
  if (ret != OK)
    {
      printf("[HX83102] E15 AHB addr write ret=%d, ID read decides\n", ret);
    }

  ret = hx83102_i2c_write_cmd(priv, HX83102_AHB_ACCESS_DIR, &dir, 1);
  if (ret != OK)
    {
      printf("[HX83102] E15 AHB dir write ret=%d, ID read decides\n", ret);
    }

  ret = hx83102_i2c_read_cmd(priv, HX83102_AHB_RDATA_BYTE_0,
                             id, sizeof(id));
  if (ret != OK)
    {
      printf("[HX83102] ID read failed ret=%d\n", ret);
      return ret;
    }

  printf("[HX83102] ID=%02x %02x %02x %02x\n", id[0], id[1], id[2], id[3]);

  if (id[3] != 0x83 || id[2] != 0x10
      || (id[1] != 0x2a && id[1] != 0x2b))
    {
      printf("[HX83102] unexpected controller ID\n");
      return -ENODEV;
    }

  return OK;
}

static int hx83102_control_initialize(void)
{
  int ret;
  int i;
  gpio_pull_status_t sda_pull = GPIO_PULL_DOWN_DISABLED;
  gpio_pull_status_t scl_pull = GPIO_PULL_DOWN_DISABLED;
  gpio_pull_status_t rst_pull = GPIO_PULL_DOWN_DISABLED;
  gpio_pull_status_t int_pull = GPIO_PULL_DOWN_DISABLED;
  FAR struct hx83102_touch_dev_s *priv = &g_hx83102_touch;

  printf("[HX83102] ===== INIT START =====\n");
  printf("[HX83102] P286-R4s stop-error propagation, R4a base, 50k\n");
  printf("[HX83102] GPIO: RST=PB4 INT=PB5 I2C_ADDR=0x%02X\n",
         HX83102_I2C_ADDR);
  printf("[HX83102] I2C bus frequency: %lu Hz\n",
         (unsigned long)priv->frequency);

  hx83102_pins_config();
  priv->i2c_addr = HX83102_I2C_ADDR;

  hal_gpio_get_pull(HX83102_I2C_SDA, &sda_pull);
  hal_gpio_get_pull(HX83102_I2C_SCL, &scl_pull);
  hal_gpio_get_pull(HX83102_RST_GPIO, &rst_pull);
  hal_gpio_get_pull(HX83102_INT_GPIO, &int_pull);
  printf("[HX83102] E18 pulls SDA=%d SCL=%d RST=%d INT=%d "
         "(0=none 1=up 2=down)\n",
         (int)sda_pull, (int)scl_pull, (int)rst_pull, (int)int_pull);

  hx83102_release_reset();

  /* The BOE module boots resident firmware; no SRAM upload is needed. */

  hx83102_delay_ms(HX83102_RESET_SETTLE_MS);
  printf("[HX83102] RST released, INT=%d\n", hx83102_int_level());

  /* Identify the controller.  The link drops individual transactions even
   * at 100 kHz, and a failed ID read here would otherwise leave the board
   * without /dev/input0 for the whole session, so probe a few times. */

  ret = -EIO;
  for (i = 0; i < HX83102_PROBE_TRIES; i++)
    {
      ret = hx83102_detect_controller(priv);
      if (ret == OK)
        {
          break;
        }

      hx83102_delay_ms(HX83102_PROBE_GAP_MS);
    }

  if (ret != OK)
    {
      printf("[HX83102] E14 init failed ret=%d, one RST pulse then retry\n",
             ret);
      hx83102_reset_pulse();

      ret = -EIO;
      for (i = 0; i < HX83102_PROBE_TRIES; i++)
        {
          ret = hx83102_detect_controller(priv);
          if (ret == OK)
            {
              break;
            }

          hx83102_delay_ms(HX83102_PROBE_GAP_MS);
        }
    }

  if (ret != OK)
    {
      printf("[HX83102] ===== INIT FAIL ret=%d =====\n", ret);
      return ret;
    }

  priv->touch_max_x = HX83102_TOUCH_NATIVE_X;
  priv->touch_max_y = HX83102_TOUCH_NATIVE_Y;
  printf("[HX83102] init ok (native %u x %u)\n",
         priv->touch_max_x, priv->touch_max_y);
  return OK;
}

static void hx83102_worker(FAR void *arg)
{
  FAR struct hx83102_touch_dev_s *priv = (FAR struct hx83102_touch_dev_s *)arg;
  FAR struct touch_sample_s *sample = &g_sample;
  uint32_t now = hx83102_now_ms();
  uint32_t delay = HX83102_POLL_MS;
  int level = hx83102_int_level();
  int ret = -EIO;
  int state = -1;
  uint8_t first_bad[HX83102_FRAME_LEN];
  bool dump_pair = false;
  irqstate_t flags;

  if (level != priv->int_level)
    {
      printf("[HX83102] INT %d -> %d at %lu ms\n", priv->int_level, level,
             (unsigned long)now);
      priv->int_level = (int8_t)level;
    }

  /* P286-E3: do not trust INT for the idle cadence.  On this carrier the
   * line reads low even when nobody touches, so the old INT-gated condition
   * never throttled and the worker hammered 0x30 every 20 ms.  Enforce the
   * slow gap whenever no contact is tracked; only a tracked contact keeps
   * the fast period: 100 ms idle cadence, then 20 ms tracking. */

  if (priv->last_state != 0
      && (uint32_t)(now - priv->last_poll_ms) < HX83102_IDLE_MS)
    {
      goto reschedule;
    }

  priv->last_poll_ms = now;

  /* Read once, then let the common error path schedule any retry. */

  ret = hx83102_event_read(priv, priv->touch_buf, HX83102_FRAME_LEN);

  if (ret == OK)
    {
      state = hx83102_decode(priv, sample);
    }

  /* P286-E23: a non-zero frame that fails validation may be a torn read (the
   * IC ACKs and returns bytes while the transfer reports success, so the
   * transport retry in drv_twi never fires).  The idle all-zero case is now
   * accepted in hx83102_decode (R3); this retry is left for genuinely bad
   * data.  The second read may contain a newer sample; FIFO consumption on a
   * bad read is unknown. */

  if (ret == OK && state < 0)
    {
      /* Preserve the first rejected frame before the retry overwrites it.
       * Print after both reads so logging adds no gap between them.
       */

      if (g_first_bad_dumps > 0)
        {
          g_first_bad_dumps--;
          memcpy(first_bad, priv->touch_buf, sizeof(first_bad));
          dump_pair = true;
        }

      ret = hx83102_event_read(priv, priv->touch_buf, HX83102_FRAME_LEN);
      if (ret == OK)
        {
          state = hx83102_decode(priv, sample);
        }
    }

  if (dump_pair)
    {
      printf("[HX83102-R4s] reread ret=%d state=%d tracking=%u\n",
             ret, state, priv->last_state == 0 ? 1u : 0u);
      hx83102_dump_frame("first rejected frame:", first_bad);
      if (ret == OK)
        {
          hx83102_dump_frame("reread frame:", priv->touch_buf);
        }
    }

  if (ret != OK || state < 0)
    {
      uint32_t silent = (uint32_t)(now - priv->last_good_ms);

      if (priv->err_streak < UINT8_MAX)
        {
          priv->err_streak++;
        }
      priv->i2c_fail_count++;

      if (ret == OK && g_dump_left > 0)
        {
          g_dump_left--;
          hx83102_dump_frame("reject frame:", priv->touch_buf);
        }

      /* Bound traffic after consecutive transport or report errors. */

      delay = (uint32_t)HX83102_POLL_MS
              << (priv->err_streak > 8 ? 8 : priv->err_streak);
      if (delay > HX83102_BACKOFF_MAX_MS)
        {
          delay = HX83102_BACKOFF_MAX_MS;
        }

      /* P286-R2: escalate on liveness, not on an error count.  A read that
       * the HAL already recovered costs the user nothing, so counting it as
       * a fault reset a healthy link (E23: "re-identify after 6 errors").
       * What the user actually feels is "no valid frame for a while", and
       * that is what this measures.  -EBUSY is stronger still: the bus layer
       * ran its recovery and SDA is still held, so waiting cannot help.
       *
       * Ladder: bus retry (HAL) -> bridge re-init + ID -> RST + full init. */

      if ((ret == -EBUSY || silent >= HX83102_STALL_MS)
          && (uint32_t)(now - priv->last_reident_ms)
              >= ((uint32_t)HX83102_RECOVER_GUARD_MS
                  << (priv->recover_tries > 3 ? 3 : priv->recover_tries)))
        {
          bool ok;

          printf("[HX83102] link down %lu ms (ret=%d, errs=%u, tries=%u)\n",
                 (unsigned long)silent, ret, priv->err_streak,
                 priv->recover_tries);

          if (ret == OK && g_stall_dumps > 0)
            {
              g_stall_dumps--;
              hx83102_dump_frame("stalled frame:", priv->touch_buf);
            }

          priv->last_reident_ms = now;
          priv->err_streak = 0;
          if (priv->recover_tries < UINT8_MAX)
            {
              priv->recover_tries++;
            }

          if (priv->recover_tries == 1)
            {
              /* Rung 1: re-open the AHB bridge.  Cheap, and enough when the
               * IC only lost its burst configuration. */

              ok = (hx83102_detect_controller(priv) == OK);
              if (!ok)
                {
                  printf("[HX83102] bridge re-init failed, resetting device\n");
                  hx83102_reset_pulse();
                  priv->last_reident_ms = hx83102_now_ms();
                  ok = (hx83102_detect_controller(priv) == OK);
                }
            }
          else
            {
              /* Rung 2: the bridge answered on an earlier attempt and the
               * link still produced no frame, so the IC itself is out of
               * step.  Only a reset pulse brings it back. */

              printf("[HX83102] no frame since last recovery, resetting device\n");
              hx83102_reset_pulse();
              priv->last_reident_ms = hx83102_now_ms();
              ok = (hx83102_detect_controller(priv) == OK);
            }

          /* A readable ID is not a working link: only a decoded frame clears
           * the ladder (see the success path below). */

          delay = ok ? HX83102_POLL_MS : HX83102_BACKOFF_MAX_MS;
        }

      /* Do not leave the UI pressed while the link is down. */

      if (priv->last_state == 0
          && (uint32_t)(now - priv->last_good_ms) > HX83102_RELEASE_MS)
        {
          hx83102_touch_process_event_one(0, priv, sample);
        }

      goto reschedule;
    }

  priv->err_streak = 0;
  priv->recover_tries = 0;
  priv->last_good_ms = now;
  if (!g_seen_valid)
    {
      g_seen_valid = true;
      hx83102_dump_frame("first valid frame:", priv->touch_buf);
    }

  hx83102_touch_process_event_one((uint8_t)(state == 0 ? 1 : 0), priv, sample);

reschedule:
  flags = spin_lock_irqsave(&priv->work_lock);
  if (priv->running)
    {
      ret = work_queue(LPWORK, &priv->work, hx83102_worker, priv,
                       MSEC2TICK(delay));
    }
  else
    {
      ret = OK;
    }

  spin_unlock_irqrestore(&priv->work_lock, flags);
  if (ret < 0)
    {
      ierr("HX83102: worker requeue failed: %d\n", ret);
    }
}

int hx83102_register(FAR const char *devpath, FAR struct i2c_master_s *dev)
{
  FAR struct hx83102_touch_dev_s *priv = &g_hx83102_touch;
  int ret;

  if (dev == NULL || devpath == NULL)
    {
      ierr("HX83102: i2c_master_s instance is NULL\n");
      return -EINVAL;
    }
  ret = nxmutex_lock(&g_lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->i2c != NULL)
    {
      nxmutex_unlock(&g_lifecycle_lock);
      return -EBUSY;
    }

  priv->i2c = dev;
  priv->last_state = 1;
  priv->err_streak = 0;
  priv->last_x = 0;
  priv->last_y = 0;
  memset(&g_sample, 0, sizeof(g_sample));
  g_seen_valid = false;
  g_dump_left = HX83102_DUMP_FRAMES;
  g_first_bad_dumps = 6;

  ret = hx83102_control_initialize();
  if (ret != OK)
    {
      ierr("HX83102: failed to initialize control\n");
      goto err_clear;
    }

  ret = touch_register(&priv->lower, devpath, HX83102_MAX_TOUCH_POINTS);
  if (ret < 0)
    {
      ierr("HX83102: failed to register touch driver: %d\n", ret);
      goto err_clear;
    }
  iinfo("HX83102: IIC touchscreen successfully initialized and registered\n");

  /* Delay the first idle read and rate-limit subsequent re-identification. */

  priv->last_poll_ms = hx83102_now_ms();
  priv->last_reident_ms = priv->last_poll_ms;
  priv->last_good_ms = priv->last_poll_ms;   /* P286-R2: liveness window */

  priv->running = true;
  ret = work_queue(LPWORK, &priv->work, hx83102_worker, priv,
                   MSEC2TICK(HX83102_POLL_MS));
  if (ret < 0)
    {
      priv->running = false;
      touch_unregister(&priv->lower, devpath);
      goto err_clear;
    }

  nxmutex_unlock(&g_lifecycle_lock);
  return OK;

err_clear:
  priv->lower.priv = NULL;
  priv->i2c = NULL;
  nxmutex_unlock(&g_lifecycle_lock);
  return ret;
}

void hx83102_unregister(FAR const char *devpath)
{
  FAR struct hx83102_touch_dev_s *priv = &g_hx83102_touch;
  irqstate_t flags;

  if (nxmutex_lock(&g_lifecycle_lock) < 0)
    {
      return;
    }

  if (priv->i2c == NULL)
    {
      nxmutex_unlock(&g_lifecycle_lock);
      return;
    }

  /* Callers must close input readers before unregistering the device. */

  flags = spin_lock_irqsave(&priv->work_lock);
  priv->running = false;
  spin_unlock_irqrestore(&priv->work_lock, flags);
  work_cancel_sync(LPWORK, &priv->work);
  touch_unregister(&priv->lower, devpath);
  priv->lower.priv = NULL;
  priv->i2c = NULL;
  nxmutex_unlock(&g_lifecycle_lock);
}
