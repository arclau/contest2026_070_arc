/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_uart_dbg.c
 * UART 调试工具——串口接收层实现
 *
 * 2026-08-27 新增：Settings → UART 调试工具子页的数据源。
 * 后台线程 poll /dev/uart1 → read → 8KB 环形缓冲（单写单读无锁）；
 * UI 定时器消费缓冲显示。UART1 = PD21(TX)/PD22(RX) mux4，引脚经板上
 * BSN20 电平转换电路引出（2026-08-27 P124：外部调试设备接 BSN20 外设
 * 侧接口，勿直接接芯片引脚——电平转换电路会就地处理信号）。
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

#include <lvgl/lvgl.h>   /* LV_LOG_* 日志宏 */

#include "dm_uart_dbg.h"

/****************************************************************************
 * Private Definitions
 ****************************************************************************/

/* 2026-08-27：UART0 在 gemini-s1 与 TF 卡(SDC0)共用 PF2/PF4（sys_config
 * sdc_clk/sdc_d3 佐证），SD 卡初始化后 UART0 收不到数据——调试工具改用
 * UART1（PD21/PD22 mux4；2026-08-27 P124 迁回——外部设备接板上
 * BSN20 电平转换外设侧接口，UART3 让位给 LD2410B 人体传感器）。 */
#define DM_UART_DBG_DEV   "/dev/uart1"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 波特率档位表（R528 dl = 24MHz/(baud<<4)；256000 除不尽已排除；
 * 1500000 已移除（P120 实证：24M 时钟下 dl=1 是分频器极限，回环 rx=0
 * 且全志官方文档「24M 满足不了 1.5M 以上」；2026-08-27 用户拍板接受，
 * 回归 115200 —— 人体传感器同款波特率） */
const struct dm_uart_dbg_baud_s dm_uart_dbg_bauds[] =
{
  { B9600,    "9600"   },
  { B19200,   "19200"  },
  { B38400,   "38400"  },
  { B57600,   "57600"  },
  { B115200,  "115200" },
};

static pthread_t       g_dbg_thread;
static volatile bool   g_dbg_run;          /* 接收线程运行标志 */
static volatile bool   g_dbg_open;         /* 串口已打开 */
static volatile bool   g_dbg_paused;       /* clear 时暂停 reader 防并发 */
static volatile bool   g_dbg_suspend;      /* 用户暂停：reader 丢弃数据防 burst 积压 */
static int             g_dbg_fd = -1;

/* 环形缓冲：单写（接收线程）单读（UI 定时器），无锁 */
static volatile uint8_t g_dbg_ring[DM_UART_DBG_BUF_SIZE];
static volatile int     g_dbg_ring_head;   /* 写位置（线程独占） */
static volatile int     g_dbg_ring_count;  /* 有效字节数 */
static volatile uint32_t g_dbg_total;      /* 累计接收字节 */
static volatile uint32_t g_dbg_dropped;    /* 溢出丢弃字节数 */

/* 2026-08-27 NSH 自测注入：ring 由接收线程独占写，外部线程（uartdbg_test
 * 命令）不能直接写——改为写注入缓冲 + 置 pending，接收线程在 poll 循环
 * 里消费，保证「单写者」不破。 */
#define DM_UART_DBG_INJECT_MAX 512
static volatile char  g_dbg_inject_buf[DM_UART_DBG_INJECT_MAX];
static volatile int   g_dbg_inject_len;
static volatile bool  g_dbg_inject_pending;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 入环（接收线程独占调用：read 数据 / 自测注入共用） */
static void uart_dbg_ring_push(const uint8_t *buf, int n)
{
  for (int i = 0; i < n; i++)
    {
      int pos = (g_dbg_ring_head + g_dbg_ring_count) % DM_UART_DBG_BUF_SIZE;
      g_dbg_ring[pos] = buf[i];
      if (g_dbg_ring_count < DM_UART_DBG_BUF_SIZE)
        {
          g_dbg_ring_count++;
        }
      else
        {
          /* 满：覆盖最旧（head 前移），计数溢出丢弃 */
          g_dbg_ring_head = (g_dbg_ring_head + 1) % DM_UART_DBG_BUF_SIZE;
          g_dbg_dropped++;
        }
    }
}

/* 接收线程：poll(POLLIN, 200ms) → read → 入环（溢出丢最旧）
 * 每轮先消费自测注入（pending），保证注入与真实数据同源同序。 */
static void *uart_dbg_reader(void *arg)
{
  int fd = (int)(intptr_t)arg;
  uint8_t buf[256];
  int last_report_ts = 0;
  struct pollfd pfd;

  pfd.fd     = fd;
  pfd.events = POLLIN;

  while (g_dbg_run)
    {
      /* clear() 暂停标志：skip 本轮避免与 head/count 重置竞态 */
      if (g_dbg_paused)
        {
          usleep(5000);
          continue;
        }
      /* 用户暂停：丢弃注入 + 清已积压 + 读出丢弃（不入环不计总数），
       * 继续后从新数据开始，防恢复后 burst 刷屏 */
      if (g_dbg_suspend)
        {
          g_dbg_inject_len = 0;
          g_dbg_inject_pending = false;
          g_dbg_ring_head = 0;
          g_dbg_ring_count = 0;
          int spr = poll(&pfd, 1, 200);
          if (spr > 0 && (pfd.revents & POLLIN))
            {
              ssize_t sn = read(fd, buf, sizeof(buf));
              (void)sn;              /* 读出即丢弃 */
            }
          else if (spr > 0)
            usleep(5000);            /* P215：ERR/HUP/NVAL 无可读数据，勿忙转 */
          continue;
        }
      if (g_dbg_inject_pending)
        {
          int ilen = (int)g_dbg_inject_len;
          if (ilen > 0)
            {
              g_dbg_total += (uint32_t)ilen;
              uart_dbg_ring_push((const uint8_t *)g_dbg_inject_buf, ilen);
              LV_LOG_USER("[uart_dbg] injected %d bytes (selftest)", ilen);
            }
          g_dbg_inject_len = 0;
          g_dbg_inject_pending = false;
        }

      int pr = poll(&pfd, 1, 200);
      if (pr <= 0)
        {
          /* 2026-08-27 调试：每 5s 无条件打印收字节数/环缓冲水位——
           * 零信号也打印，可区分「无电信号」vs「有数据但 UI 未显示」 */
          if (lv_tick_get() - last_report_ts > 5000)
            {
              last_report_ts = lv_tick_get();
              LV_LOG_USER("[uart_dbg] rx=%uB ring=%d",
                          (unsigned)g_dbg_total, (int)g_dbg_ring_count);
            }
          continue;                    /* 超时/无数据：休眠重试 */
        }
      /* P215：poll>0 未必可读（ERR/HUP/NVAL），须验 POLLIN，
       * 否则无条件 read 失败忙转吃 CPU */
      if (!(pfd.revents & POLLIN))
        {
          usleep(5000);
          continue;
        }

      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0)
        continue;

      g_dbg_total += (uint32_t)n;
      uart_dbg_ring_push(buf, (int)n);
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_uart_dbg_open(speed_t baud)
{
  struct termios tio;
  int fd;

  if (g_dbg_open)
    return 0;                        /* 已打开 */

  fd = open(DM_UART_DBG_DEV, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      LV_LOG_ERROR("[uart_dbg] open %s failed: %d", DM_UART_DBG_DEV, errno);
      return -1;
    }

  /* raw 模式 + 波特率 8N1 */
  memset(&tio, 0, sizeof(tio));
  if (tcgetattr(fd, &tio) < 0)
    {
      close(fd);
      return -1;
    }
  cfmakeraw(&tio);
  cfsetispeed(&tio, baud);
  cfsetospeed(&tio, baud);
  tio.c_cflag |= (CLOCAL | CREAD);
  if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
      close(fd);
      return -1;
    }

  g_dbg_fd = fd;
  g_dbg_ring_head = 0;
  g_dbg_ring_count = 0;
  g_dbg_total = 0;
  g_dbg_dropped = 0;
  g_dbg_suspend = false;
  g_dbg_run = true;
  if (pthread_create(&g_dbg_thread, NULL, uart_dbg_reader,
                     (void *)(intptr_t)fd) != 0)
    {
      close(fd);
      g_dbg_fd = -1;
      g_dbg_run = false;
      return -1;
    }

  g_dbg_open = true;
  LV_LOG_USER("[uart_dbg] open %s ok", DM_UART_DBG_DEV);
  return 0;
}

int dm_uart_dbg_set_baud(speed_t baud)
{
  struct termios tio;

  if (!g_dbg_open || g_dbg_fd < 0)
    return -1;

  if (tcgetattr(g_dbg_fd, &tio) < 0)
    return -1;
  cfsetispeed(&tio, baud);
  cfsetospeed(&tio, baud);
  return tcsetattr(g_dbg_fd, TCSANOW, &tio);
}

void dm_uart_dbg_close(void)
{
  int fd;

  if (!g_dbg_open)
    return;

  fd = g_dbg_fd;               /* 先快照 fd，join 后再 close（防 fd 泄漏） */
  g_dbg_run = false;
  g_dbg_suspend = false;
  pthread_join(g_dbg_thread, NULL);  /* 线程退出（≤200ms） */

  if (fd >= 0)
    close(fd);                 /* reader 只读不关，此处统一 close */
  g_dbg_fd = -1;
  g_dbg_open = false;

  LV_LOG_USER("[uart_dbg] closed");
}

int dm_uart_dbg_is_open(void)
{
  return (int)g_dbg_open;
}

int dm_uart_dbg_avail(void)
{
  return (int)g_dbg_ring_count;
}

int dm_uart_dbg_read(uint8_t *buf, int maxlen)
{
  int n = 0;

  if (!buf || maxlen <= 0)
    return 0;
  while (n < maxlen && g_dbg_ring_count > 0)
    {
      buf[n++] = g_dbg_ring[g_dbg_ring_head];
      g_dbg_ring_head = (g_dbg_ring_head + 1) % DM_UART_DBG_BUF_SIZE;
      g_dbg_ring_count--;
    }
  return n;
}

/* 2026-08-27 NSH 自测注入（uartdbg_test 命令）：填注入缓冲 + 置 pending，
 * 由接收线程在 poll 循环消费入环——ring 单写者不破，UI 定时器正常显示。
 * 返回 0=成功/-1=缓冲满。 */
int dm_uart_dbg_inject_test(void)
{
  static const char test_log[] =
    "[uart_dbg selftest] UART1 loopback check 2026-08-27\r\n"
    "  line1: rx path OK — UI 显示链路正常\r\n"
    "  line2: 波特率 115200（1500000 已移除：24M 时钟下 dl=1 极限不可用）\r\n"
    "  line3: end of selftest\r\n";

  int slen = (int)strlen(test_log);
  if (slen >= DM_UART_DBG_INJECT_MAX)
    return -1;

  memcpy((char *)g_dbg_inject_buf, test_log, slen);
  g_dbg_inject_len = slen;
  g_dbg_inject_pending = true;
  LV_LOG_USER("[uart_dbg] selftest inject queued (%d bytes)", slen);
  return 0;
}

/* 内部回环自测：MCR.LOOP(bit4) → write → 接收线程入环 → 关回环。
 * 不碰引脚，验证控制器+驱动全通。 */
int dm_uart_dbg_loop_test(void)
{
  static const char msg[] = "UART1-LOOP-TEST-0123456789\r\n";
  int mcr = 0x10;                    /* UART_MCR_LOOP (1<<4) */
  int n;

  if (!g_dbg_open || g_dbg_fd < 0)
    return -1;

  /* 清环 + 开内部回环 */
  g_dbg_ring_head = 0;
  g_dbg_ring_count = 0;
  if (ioctl(g_dbg_fd, TIOCMSET, &mcr) < 0)
    {
      LV_LOG_ERROR("[uart_dbg] loop: TIOCMSET fail errno=%d", errno);
      return -1;
    }

  /* 发测试字节（内部 TX→RX 环回，不经引脚） */
  (void)write(g_dbg_fd, msg, sizeof(msg) - 1);

  /* 等接收线程 poll(200ms) 读到入环 */
  usleep(300000);

  n = (int)g_dbg_ring_count;

  /* 关内部回环 */
  mcr = 0;
  (void)ioctl(g_dbg_fd, TIOCMSET, &mcr);

  LV_LOG_USER("[uart_dbg] loop test: rx=%d bytes (internal loopback)",
              n);
  return n;
}

void dm_uart_dbg_clear(void)
{
  g_dbg_paused = true;   /* 暂停 reader 线程避免并发写 */
  g_dbg_ring_head = 0;
  g_dbg_ring_count = 0;
  g_dbg_total = 0;
  g_dbg_dropped = 0;
  g_dbg_paused = false;  /* 恢复 */
}

uint32_t dm_uart_dbg_total(void)
{
  return g_dbg_total;
}

/* 用户暂停开关（UI 暂停/继续按钮调用；reader 侧丢弃数据防积压） */
void dm_uart_dbg_set_paused(int paused)
{
  g_dbg_suspend = (paused != 0);
}

void dm_uart_dbg_get_stats(uint32_t *total_bytes, uint32_t *dropped_bytes)
{
  if (total_bytes)
    *total_bytes = g_dbg_total;
  if (dropped_bytes)
    *dropped_bytes = g_dbg_dropped;
}

/* 写数据到 UART1（供回车按钮等 UI 功能调用；只读模式的简单 TX 通路） */
int dm_uart_dbg_write(const void *buf, int len)
{
  if (!g_dbg_open || g_dbg_fd < 0)
    return -1;
  return (int)write(g_dbg_fd, buf, (size_t)len);
}
