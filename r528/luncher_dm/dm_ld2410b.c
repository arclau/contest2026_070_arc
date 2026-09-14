/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_ld2410b.c
 * LD2410B 24GHz 毫米波人体存在传感器（UART3 串口读取）
 *
 * 2026-08-23：替代 LTR553 接近传感器，作为 dm_health 人体感应数据源。
 * 架构：常驻后台线程 read /dev/uart3 → 帧状态机解析 → 全局状态
 * （g_ld2410b_state / g_ld2410b_dist_cm），dm_health 通过
 * dm_prox_get_cm() 间接读取（见 luncher_dm.c）。
 *
 * 帧格式（小端，正常工作模式"目标基本信息"，len=0x0D）：
 *   F4 F3 F2 F1 | len(2B) | type=0x02 0xAA
 *   | 目标状态(1B) | 运动距离(2B) | 运动能量(1B)
 *   | 静止距离(2B) | 静止能量(1B) | 探测距离(2B)
 *   | 0x55 0x00 | F8 F7 F6 F5
 *   目标状态: 0x00 无 / 0x01 运动 / 0x02 静止 / 0x03 运动&静止
 *
 * 距离分辨率：每距离门 0.75m，探测距离为 0.75m 整数倍
 *   （0/75/150/225...cm），故 200cm 在座阈值实际边界在 150~225cm 之间。
 * 2026-08-26 按官方《说明书 V1.09》+《串口通信协议 V1.08》修正：
 *   - 设备路径 /dev/ttyS0 → /dev/uart0（板级注册名，r528_serial.c）
 *   - open 改 O_RDWR + 初始化下发配置（最大距离门/无人延时，命令 0x0060）
 *   - ⚠ 模块默认波特率 256000，必须先 USB-TTL 改成 115200 才能通信
 * 2026-08-27 P124：UART0 已禁用（与 TF 卡冲突），传感器迁到 /dev/uart3
 *   （PD10=UART3_TX / PD11=UART3_RX mux5；UART3 原为调试工具口，见 P123）
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <pthread.h>
#include <sys/param.h>

#include <lvgl/lvgl.h>   /* LV_LOG_* 日志宏（与 luncher_dm.c 一致） */

#include "dm_ld2410b.h"

/****************************************************************************
 * Private Definitions
 ****************************************************************************/

#define LD2410B_DEV           "/dev/uart3"
#define LD2410B_BAUD          B115200

/* 帧头/帧尾 */
#define FRAME_HDR0            0xF4
#define FRAME_HDR1            0xF3
#define FRAME_HDR2            0xF2
#define FRAME_HDR3            0xF1
#define FRAME_TAIL0           0xF8
#define FRAME_TAIL1           0xF7
#define FRAME_TAIL2           0xF6
#define FRAME_TAIL3           0xF5

#define DATA_TYPE_BASIC       0x02   /* 目标基本信息数据 */
#define DATA_HEAD_AA          0xAA
#define DATA_TAIL_55          0x55

/* 数据区字段偏移（数据区起点 = type 字节） */
#define OFF_STATE      2   /* 目标状态 1B */
#define OFF_MOVE_DIST  3   /* 运动距离 2B LE */
#define OFF_STAT_DIST  6   /* 静止距离 2B LE */
#define OFF_DET_DIST   9   /* 探测距离 2B LE */
#define DATA_LEN_BASIC 13  /* type..0x00 共 13B（len 字段值 = 0x0D） */

/* 命令帧（小端，协议 V1.08 §2.1.2）：
 *   FD FC FB FA | len(2B) | cmd(2B) | value(N) | 04 03 02 01 */
#define CMD_HDR0            0xFD
#define CMD_HDR1            0xFC
#define CMD_HDR2            0xFB
#define CMD_HDR3            0xFA
#define CMD_TAIL0           0x04
#define CMD_TAIL1           0x03
#define CMD_TAIL2           0x02
#define CMD_TAIL3           0x01

#define CMD_ENABLE_CFG      0x00FF   /* 使能配置命令（值 0x0001） */
#define CMD_DISABLE_CFG     0x00FE   /* 结束配置命令（无值） */
#define CMD_SET_GATE_NODELAY 0x0060  /* 最大距离门 + 无人延时配置命令 */

/* 初始化下发配置（掉电不丢失；无人延时即时生效）：
 * 出厂默认：最大距离门 8（6m）、无人延时 5s。
 *  - 最大距离门 3 = 2.25m：贴合 200cm 在座阈值，滤掉 3~6m 路过目标误报"有人"
 *  - 无人延时 2s：出厂 5s 会叠加 dm_health 5s 防抖，离开判定延迟 ~10s，调小加快 */
#define LD2410B_CFG_MAX_GATE    3    /* 最大距离门（2~8，每门 0.75m） */
#define LD2410B_CFG_NODELAY_S   2    /* 无人延时秒 */

/* 帧状态机 */
enum
{
  ST_WAIT_HDR = 0,  /* 等 F4 */
  ST_HDR1,          /* 等 F3 */
  ST_HDR2,          /* 等 F2 */
  ST_HDR3,          /* 等 F1 */
  ST_LEN_LO,        /* 等 len 低字节 */
  ST_LEN_HI,        /* 等 len 高字节 */
  ST_DATA,          /* 收帧内数据 len 字节 */
  ST_TAIL0,         /* 等 F8 */
  ST_TAIL1,         /* 等 F7 */
  ST_TAIL2,         /* 等 F6 */
  ST_TAIL3,         /* 等 F5（收齐 = 一帧完成） */
};

#define FRAME_MAX_LEN         128   /* 工程模式帧 < 64B，留余量 */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_t       g_ld2410b_thread;
static volatile bool   g_ld2410b_run;      /* 线程运行标志 */
static volatile bool   g_ld2410b_ready;    /* 串口已打开 */
static volatile int    g_ld2410b_state;    /* 最近一帧目标状态 */
static volatile int    g_ld2410b_dist_cm;  /* 最近一帧探测距离 */
static volatile bool   g_ld2410b_frame_ok; /* 已成功解析过完整帧 */
static volatile int    g_ld2410b_last_ts;  /* 最近一帧到达时刻（lv_tick ms，WARN3 帧过期用） */
/* 2026-08-28：删除刷屏调试 LOG 时一并移除的计数变量（rx_bytes/frame_cnt/log_state） */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 提取小端 2B 值 */
static int ld2410b_le16(const uint8_t *p)
{
  return (int)(p[0] | (p[1] << 8));
}

/* 校验并解析一帧数据区（d 指向 type 字节，len = 数据区长度）
 * 成功更新 g_ld2410b_state / g_ld2410b_dist_cm 并返回 0 */
static int ld2410b_parse_frame(const uint8_t *d, int len)
{
  if (len < DATA_LEN_BASIC)
    return -1;
  if (d[0] != DATA_TYPE_BASIC || d[1] != DATA_HEAD_AA)
    return -1;                       /* 非目标基本信息帧 */
  if (d[OFF_STATE] > 0x03)
    return -1;                       /* 状态值非法（底噪检测等不理会） */
  if (d[11] != DATA_TAIL_55 || d[12] != 0x00)
    return -1;                       /* 数据区尾部校验失败 */

  g_ld2410b_state   = d[OFF_STATE];
  g_ld2410b_dist_cm = ld2410b_le16(&d[OFF_DET_DIST]);
  g_ld2410b_frame_ok = true;
  g_ld2410b_last_ts = lv_tick_get();   /* WARN3：记录帧到达时刻供过期判断 */
  return 0;
}

/* 发送一条配置命令帧（协议 V1.08 §2.1.2，小端）：
 *   FD FC FB FA | len(2B)=2+vlen | cmd(2B) | value(vlen) | 04 03 02 01
 * best-effort：模块未接/波特率不符时静默失败，不影响主流程。 */
static void ld2410b_send_cmd(int fd, uint16_t cmd, const uint8_t *val, int vlen)
{
  uint8_t buf[32];
  int n = 0;
  int i;

  buf[n++] = CMD_HDR0;
  buf[n++] = CMD_HDR1;
  buf[n++] = CMD_HDR2;
  buf[n++] = CMD_HDR3;
  buf[n++] = (uint8_t)((2 + vlen) & 0xff);
  buf[n++] = (uint8_t)((2 + vlen) >> 8);
  buf[n++] = (uint8_t)(cmd & 0xff);
  buf[n++] = (uint8_t)(cmd >> 8);
  for (i = 0; i < vlen; i++)
    buf[n++] = val[i];
  buf[n++] = CMD_TAIL0;
  buf[n++] = CMD_TAIL1;
  buf[n++] = CMD_TAIL2;
  buf[n++] = CMD_TAIL3;

  (void)write(fd, buf, n);
}

/* 初始化下发模块配置：最大距离门 + 无人延时（命令 0x0060，协议 V1.08 §2.2.3）。
 * 流程：使能配置 → 0x0060 → 结束配置（协议 §2.4.1）。
 * 无人延时即时生效；最大距离门掉电不丢失。 */
static void ld2410b_apply_config(int fd)
{
  uint8_t enable[2] = { 0x01, 0x00 };   /* 使能配置命令值 0x0001 */
  uint8_t val[18];
  int i = 0;

  /* 0x0060 命令值：2B 最大运动距离门字(0x0000)+4B 参数
   *               +2B 最大静止距离门字(0x0001)+4B 参数
   *               +2B 无人持续时间字(0x0002)+4B 参数 */
  val[i++] = 0x00; val[i++] = 0x00;                    /* 最大运动距离门字 */
  val[i++] = (uint8_t)(LD2410B_CFG_MAX_GATE & 0xff);
  val[i++] = (uint8_t)(LD2410B_CFG_MAX_GATE >> 8);
  val[i++] = 0x00; val[i++] = 0x00;
  val[i++] = 0x01; val[i++] = 0x00;                    /* 最大静止距离门字 */
  val[i++] = (uint8_t)(LD2410B_CFG_MAX_GATE & 0xff);
  val[i++] = (uint8_t)(LD2410B_CFG_MAX_GATE >> 8);
  val[i++] = 0x00; val[i++] = 0x00;
  val[i++] = 0x02; val[i++] = 0x00;                    /* 无人持续时间字 */
  val[i++] = (uint8_t)(LD2410B_CFG_NODELAY_S & 0xff);
  val[i++] = (uint8_t)(LD2410B_CFG_NODELAY_S >> 8);
  val[i++] = 0x00; val[i++] = 0x00;

  ld2410b_send_cmd(fd, CMD_ENABLE_CFG, enable, 2);
  usleep(20 * 1000);
  ld2410b_send_cmd(fd, CMD_SET_GATE_NODELAY, val, 18);
  usleep(20 * 1000);
  ld2410b_send_cmd(fd, CMD_DISABLE_CFG, NULL, 0);
  usleep(20 * 1000);

  /* 2026-08-26 调试日志：配置下发结果（best-effort，模块未接/波特率不符时不回 ACK） */
  LV_LOG_USER("[ld2410b] cfg sent: maxgate=%d nodelay=%ds", LD2410B_CFG_MAX_GATE,
              LD2410B_CFG_NODELAY_S);
}

/* 串口读取线程：poll → read → 状态机 → 提取目标状态/距离
 * 2026-08-23 审查修复（FAIL1）：O_NONBLOCK 直 read 无数据时忙转烧 CPU，
 * 改 poll(POLLIN, 200ms) 带超时等待——无数据时线程休眠，不空转。 */
static void *ld2410b_reader(void *arg)
{
  int      fd = (int)(intptr_t)arg;
  uint8_t  buf[64];
  uint8_t  frame[FRAME_MAX_LEN];
  int      fpos = 0;
  int      flen = 0;
  int      st   = ST_WAIT_HDR;
  struct pollfd pfd;

  pfd.fd     = fd;
  pfd.events = POLLIN;

  while (g_ld2410b_run)
    {
      int pr = poll(&pfd, 1, 200);
      if (pr <= 0)
        continue;                    /* 超时/无数据：正常休眠重试 */

      /* P215：同 dm_uart_dbg——poll>0 未必可读，须验 POLLIN 防忙转 */
      if (!(pfd.revents & POLLIN))
        {
          usleep(5000);
          continue;
        }

      ssize_t n = read(fd, buf, sizeof(buf));

      if (n <= 0)
        continue;                    /* 无数据可读（非阻塞，直接重试） */

      for (ssize_t i = 0; i < n; i++)
        {
          uint8_t b = buf[i];

          switch (st)
            {
            case ST_WAIT_HDR:
              if (b == FRAME_HDR0) st = ST_HDR1;
              break;
            case ST_HDR1:
              st = (b == FRAME_HDR1) ? ST_HDR2 : ST_WAIT_HDR;
              break;
            case ST_HDR2:
              st = (b == FRAME_HDR2) ? ST_HDR3 : ST_WAIT_HDR;
              break;
            case ST_HDR3:
              st = (b == FRAME_HDR3) ? ST_LEN_LO : ST_WAIT_HDR;
              break;
            case ST_LEN_LO:
              flen = b;
              st   = ST_LEN_HI;
              break;
            case ST_LEN_HI:
              flen |= (b << 8);
              if (flen < DATA_LEN_BASIC || flen > FRAME_MAX_LEN)
                {
                  st = ST_WAIT_HDR;  /* 长度非法，重新找头 */
                }
              else
                {
                  fpos = 0;
                  st   = ST_DATA;
                }
              break;
            case ST_DATA:
              frame[fpos++] = b;
              if (fpos >= flen)
                st = ST_TAIL0;
              break;
            case ST_TAIL0:
              st = (b == FRAME_TAIL0) ? ST_TAIL1 : ST_WAIT_HDR;
              break;
            case ST_TAIL1:
              st = (b == FRAME_TAIL1) ? ST_TAIL2 : ST_WAIT_HDR;
              break;
            case ST_TAIL2:
              st = (b == FRAME_TAIL2) ? ST_TAIL3 : ST_WAIT_HDR;
              break;
            case ST_TAIL3:
              if (b == FRAME_TAIL3)
                {
                  ld2410b_parse_frame(frame, flen);   /* 一帧收齐 */
                }
              st = ST_WAIT_HDR;
              break;
            default:
              st = ST_WAIT_HDR;
              break;
            }
        }
    }

  close(fd);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_ld2410b_init(void)
{
  struct termios tio;
  int fd;

  if (g_ld2410b_ready)
    return 0;                        /* 已初始化 */

  fd = open(LD2410B_DEV, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      LV_LOG_ERROR("[ld2410b] open %s failed: %d", LD2410B_DEV, errno);
      return -1;
    }

  /* 115200 8N1，raw 模式 */
  memset(&tio, 0, sizeof(tio));
  if (tcgetattr(fd, &tio) < 0)
    {
      close(fd);
      return -1;
    }
  cfmakeraw(&tio);
  cfsetispeed(&tio, LD2410B_BAUD);
  cfsetospeed(&tio, LD2410B_BAUD);
  tio.c_cflag |= (CLOCAL | CREAD);
  if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
      close(fd);
      return -1;
    }

  /* 2026-08-26：初始化下发模块配置（最大距离门 + 无人延时，命令 0x0060）。
   * best-effort：模块未接/波特率仍为 256000 时静默失败，不影响主流程。 */
  ld2410b_apply_config(fd);

  g_ld2410b_run = true;
  if (pthread_create(&g_ld2410b_thread, NULL, ld2410b_reader,
                     (void *)(intptr_t)fd) != 0)
    {
      close(fd);
      g_ld2410b_run = false;
      return -1;
    }

  g_ld2410b_ready = true;
  LV_LOG_USER("[ld2410b] init ok (%s @115200)", LD2410B_DEV);
  return 0;
}

int dm_ld2410b_get_state(void)
{
  return (int)g_ld2410b_state;
}

int dm_ld2410b_get_dist_cm(void)
{
  if (!g_ld2410b_frame_ok)
    return -1;
  return (int)g_ld2410b_dist_cm;
}

int dm_ld2410b_present(void)
{
  /* FAIL2：未就绪 或 从未收到过完整帧 → -1（回退纯计时，保提醒不失效）。
   * WARN3：最近帧超过 2s 未更新（传感器掉线/波特率错）→ -1 同样回退。 */
  if (!g_ld2410b_ready || !g_ld2410b_frame_ok)
    return -1;
  if (lv_tick_get() - g_ld2410b_last_ts > 2000)
    return -1;
  return LD2410B_STATE_PRESENT(g_ld2410b_state) ? 1 : 0;
}

void dm_ld2410b_deinit(void)
{
  if (!g_ld2410b_ready)
    return;
  g_ld2410b_run = false;
  pthread_join(g_ld2410b_thread, NULL);
  g_ld2410b_ready = false;
  LV_LOG_USER("[ld2410b] deinit ok");
}
