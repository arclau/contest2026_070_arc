/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_uart_dbg.h
 * UART 调试工具——串口接收层（Settings → UART 调试工具子页的数据源）
 *
 * 2026-08-27 新增：接收其他设备经串口发来的 debug log 文本。
 *  - 设备：/dev/uart1（PD21/PD22 mux4；2026-08-27 P124 迁回——外部
 *    调试设备接板上 BSN20 电平转换外设侧接口，UART3 让位给 LD2410B）
 *  - 波特率：9600/19200/38400/57600/115200 五档（R528 波特率表
 *    dl = 24MHz/(baud<<4)，256000 除不尽不可用，勿加）
 *  - 架构：后台线程 poll(POLLIN,200ms) → read → 8KB 环形缓冲
 *    （单写单读无锁，与 dm_ld2410b 同风格）；UI 定时器消费显示。
 *  - 只读：不发送（需求 v1 = 接收其他设备 debug log 并显示）
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_UART_DBG_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_UART_DBG_H

#include <termios.h>

/* 环形缓冲大小（8KB，够承接 burst；溢出丢最旧） */
#define DM_UART_DBG_BUF_SIZE  8192

/* 波特率档位表（r528_serial.c dl 换算，勿加 256000） */
struct dm_uart_dbg_baud_s
{
  speed_t  baud;         /* termios speed_t（B9600...B115200） */
  const char *label;     /* UI 显示 "9600"... */
};
extern const struct dm_uart_dbg_baud_s dm_uart_dbg_bauds[];
#define DM_UART_DBG_BAUD_CNT 5
#define DM_UART_DBG_BAUD_DEFAULT B115200

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 打开串口（默认 /dev/uart1 = PD21/PD22 mux4，外部设备接 BSN20 外设侧）
 * 并启动后台接收线程。返回 0=成功/-1=失败 */
int dm_uart_dbg_open(speed_t baud);

/* 切换波特率（termios TCSANOW 重设，线程不断流） */
int dm_uart_dbg_set_baud(speed_t baud);

/* 关闭串口：停线程 + join + close fd */
void dm_uart_dbg_close(void);

/* 是否已打开 */
int dm_uart_dbg_is_open(void);

/* 环形缓冲中可读字节数（UI 轮询用） */
int dm_uart_dbg_avail(void);

/* 消费读取：把环形缓冲数据拷出（最多 maxlen 字节），返回实际字节数 */
int dm_uart_dbg_read(uint8_t *buf, int maxlen);

/* 清空环形缓冲（清屏时丢弃已收数据） */
void dm_uart_dbg_clear(void);

/* 累计接收字节数（状态栏"已收 N 字节"） */
uint32_t dm_uart_dbg_total(void);

/* 用户暂停开关：暂停期间 reader 读出丢弃（不入环不积压），
 * 继续后从新数据开始（防恢复后 burst 刷屏） */
void dm_uart_dbg_set_paused(int paused);

/* 统计信息：累计接收字节 + 环形缓冲溢出丢弃字节数 */
void dm_uart_dbg_get_stats(uint32_t *total_bytes, uint32_t *dropped_bytes);

/* 写数据到 UART1（回车按钮等 UI TX 通路；返回写入字节数/-1=失败） */
int dm_uart_dbg_write(const void *buf, int len);

/* 2026-08-27 NSH 自测注入（uartdbg_test 命令）：注入一段测试日志到环形
 * 缓冲，UI 窗口立即显示——用于不接硬件验证「串口→UI」显示链路。
 * 线程安全：接收线程 poll 循环消费 pending，ring 单写者不破。
 * 返回 0=成功/-1=缓冲满 */
int dm_uart_dbg_inject_test(void);

/* 2026-08-27 内部回环自测（uartdbg_test loop 命令）：ioctl 置
 * MCR.LOOP(bit4) → write 测试字节 → 等接收线程入环 → 返回环内字节数。
 * 不经任何引脚，验证「控制器+驱动」链路（接线问题 vs 驱动问题一刀切）。
 * 返回环内字节数（>0=收发全通）；-1=未打开 */
int dm_uart_dbg_loop_test(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_UART_DBG_H */
