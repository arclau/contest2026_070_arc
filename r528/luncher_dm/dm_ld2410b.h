/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_ld2410b.h
 * LD2410B 24GHz 毫米波人体存在传感器（UART3, 115200 8N1）
 *
 * 替代 LTR553 接近传感器作为「人体感应」数据源（2026-08-23）：
 *   LTR553 探测距离近（≤30cm 分级），只能感应"手伸到屏前"；
 *   LD2410B 探测 0.75m~6m 可调、有人/无人 + 运动/静止 + 距离，
 *   更符合「人坐下/离开」的真实场景。
 *
 * 接线（2026-08-27 P124：UART3 = PD10/PD11 mux5，UART0 已禁用）：
 *   LD2410B VCC(5V) → 板端 5V；GND → GND；
 *   UART_Tx → PD11(UART3_RX)；UART_Rx → PD10(UART3_TX，下发配置)。
 *   ⚠⚠ 模块默认波特率 256000，R528 UART 驱动波特率表无 256000，
 *     必须先 USB-TTL 把模块波特率配置改为 115200（命令持久化，掉电不丢）
 *     ——否则串口收不到任何帧，锁屏显示"人体：不可用"。
 *
 * 上报帧（小端，正常工作模式"目标基本信息"）：
 *   F4 F3 F2 F1 | len(2B LE)=0x0D | type=0x02 | 0xAA
 *   | 目标状态(1B) | 运动距离(2B LE cm) | 运动能量(1B)
 *   | 静止距离(2B LE cm) | 静止能量(1B) | 探测距离(2B LE)
 *   | 0x55 0x00 | F8 F7 F6 F5
 *   目标状态: 0x00 无 / 0x01 运动 / 0x02 静止 / 0x03 运动&静止
 *
 * 距离分辨率：每距离门 0.75m，探测距离为 0.75m 整数倍
 *   （0/75/150/225...cm），故 200cm 在座阈值实际边界在 150~225cm 之间。
 * 2026-08-26 按官方《说明书 V1.09》+《串口通信协议 V1.08》修正：
 *   - 设备路径 /dev/ttyS0 → /dev/uart0（板级注册名，r528_serial.c）
 *   - open 改 O_RDWR + 初始化下发配置（最大距离门/无人延时，命令 0x0060）
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_LD2410B_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_LD2410B_H

/* 目标状态（帧内 1 字节，见表 14） */
#define LD2410B_STATE_NONE      0x00  /* 无目标 */
#define LD2410B_STATE_MOVING    0x01  /* 运动目标 */
#define LD2410B_STATE_STATIC    0x02  /* 静止目标 */
#define LD2410B_STATE_BOTH      0x03  /* 运动&静止目标 */

/* 有人判定：状态非 NONE 即视为人体存在 */
#define LD2410B_STATE_PRESENT(s) ((s) != LD2410B_STATE_NONE)

/* 在座距离阈值默认值（cm）：有人且探测距离 ≤ 该值判定在座。
 * 2026-08-27 用户拍板：1.5m（2 门，0.75m/门）——200cm 会被距离门
 * 吸到 150cm，注释直写生效边界；可经 dm_health_cfg 的
 * ld2410_seat_cm 覆盖（默认 150） */
#define LD2410B_SEAT_CM_DEFAULT  150

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 初始化：打开 /dev/uart3 并启动后台解析线程。返回 0=成功/-1=失败
 * 2026-08-27 P124：UART0 禁用（TF 卡冲突）→ 迁 UART3，init_sensors 已恢复调用 */
int  dm_ld2410b_init(void);

/* 目标状态：LD2410B_STATE_*，无数据返回 LD2410B_STATE_NONE */
int  dm_ld2410b_get_state(void);

/* 人体距离 cm（探测距离；无目标/无数据返回 -1） */
int  dm_ld2410b_get_dist_cm(void);

/* 有人存在（状态≠NONE）返回 1，否则 0；串口未就绪返回 -1 */
int  dm_ld2410b_present(void);

/* 停止后台线程并释放 UART3 资源 */
void dm_ld2410b_deinit(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_LD2410B_H */
