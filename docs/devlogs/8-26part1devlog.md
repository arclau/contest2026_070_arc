# DevLog 2026-08-26 — LD2410B 上板调试：设备路径/配置下发/波特率/零字节排查（P118）

> 用户需求：P117 接入的 LD2410B 上板验证——锁屏「人体」一直显示不可用，排查到模块波特率配置。
> 固化 **ok-20260826-1 ~ ok-20260826-5**（五连）。

## 背景

P117 已把 LD2410B 接入 luncher_dm（UART0 应用层直读 + 帧解析 + 在座判定）。上板后发现：`ls /dev/` 有 uart0、日志有 `init ok` + `cfg sent`，但**没有任何 `state=` 日志**（零字节），锁屏「人体：不可用」。

## 改动清单（五连固化）

| tag | 文件 | 改动 |
|-----|------|------|
| ok-20260826-1 | `dm_ld2410b.c/.h` | `LD2410B_DEV` `/dev/ttyS0`→`/dev/uart0`（R528 板级注册名是 `/dev/uartN`，非 `/dev/ttyS0`——旧固件 open 失败根因） |
| ok-20260826-2 | `dm_ld2410b.c/.h` | 按协议 V1.08 §2.1.2/§2.2.3/§2.4.1 新增命令帧宏（CMD_HDR/TAIL、0x00FF 使能/0x00FE 结束/0x0060 距离门+无人延时）、`ld2410b_send_cmd()`、`ld2410b_apply_config()`（使能→0x0060→结束，间插 usleep 20ms）；`open()` 改 `O_RDWR|O_NONBLOCK`；`LD2410B_CFG_MAX_GATE=3`(2.25m)、`LD2410B_CFG_NODELAY_S=2`(出厂 5s→2s)；.h 头注释加波特率 ⚠⚠ + 距离分辨率说明（每门 0.75m，200cm 阈值实际边界 150~225cm） |
| ok-20260826-3 | `dm_ld2410b.c` | 补 3 处 `LV_LOG_USER` 调试日志：`[ld2410b] state=%d dist=%dcm`（状态变化才打，`g_ld2410b_log_state` 防刷屏）/ `cfg sent: maxgate=%d nodelay=%ds` / `init ok` 由 INFO 改 USER（`CONFIG_LV_LOG_LEVEL_ERROR=y` 滤掉 INFO，这是早期"无传感器日志"三重原因之一） |
| ok-20260826-4 | `dm_ld2410b.c` | 字节计数调试日志：`g_ld2410b_rx_bytes`/`g_ld2410b_frame_cnt` 累计，每 5s 打印 `rx=%uB frames=%u`（区分"完全无信号"vs"有信号但解析失败"） |
| ok-20260826-5 | `dm_ld2410b.c` | `rx=` 从"收到数据才打印"改为**每 5s 无条件打印**（零字节也可见 `rx=0B frames=0`，回环测试可观测） |

## 排障过程（现象→假设→验证→结论）

### ① 上板零字节（init ok + cfg sent，无 state=）
- 现象：uart0 设备在、驱动打开成功、配置命令发出，但 UART0 RX 收不到任何帧
- 假设 A：**模块波特率仍是出厂 256000**（R528 波特率表无 256000，`r528_uartdl = 24MHz/(baud<<4)`，256000 除不尽取整 5 → 实际 300000，差 17% 无法通信）——**成立**
- 验证：USB-TTL + SSCOM@256000 接模块，按协议 §2.4.1 发 3 条命令（0x00FF 使能 → 0x00A1 值 0x0005=115200 → 0x00FE 结束），ACK `A1 01 00 00` = 设置成功，**断电重启后**模块按 115200 工作（§2.2.9：波特率重启后生效，掉电不丢失）
- 关键坑：SSCOM 必须勾「十六进制发送」（否则发的是 ASCII 字符 "F"、"D"… 模块看不懂，发记录显示 `46 44 46 43...` 即文本）；接收勾「十六进制显示」（二进制帧在文本模式下显示乱码是正常的）

### ② 模块确认 115200 后仍零字节 → 查板端引脚
- 现象：模块在电脑 USB-TTL@115200 上数据帧正常（`F4 F3 F2 F1 0D 00 02 AA...`，目标状态 03 = 有人 63~83cm），但接回板子 J5 口仍零字节
- 假设 B：CIR 红外驱动抢占 UART0 引脚（board.h 注释 PB22/PB23 同时是 UART0_RX_1/IR1_RX）——**排除**
- 验证：R528(sun8iw20) CIR RX=**GPIO_PB7**、TX=**GPIO_PB0**（`platform_cir_rx.h`/`cir_tx_sun20iw1.h`），不碰 PB22/PB23
- 假设 C：驱动配置的 UART0 引脚与物理接线不一致——**关键发现**
  - 驱动 `r528_uart0config()`（gemini-s1 分支，nuttx 与 vendor 两份一致）= `GPIOF(2)`=TX、`GPIOF(4)`=RX，function 3，`up_earlyserialinit` 调用（实际生效）
  - HAL `uart-sun8iw20.h` = `UART0_TX GPIOF(2)` / `UART0_RX GPIOF(4)` + function 3（与驱动一致）
  - velaevb1 板 sys_config.fex：`uart_debug_tx = port:PF02<3>` / `rx = port:PF04<3>` —— **官方确认 UART0 在 PF2/PF4、mux 3，与驱动完全一致**
  - board.h 注释「J5 口 = PB22/PB23」是**过时文档**（`PIO_UART0_RX/TX` 宏无任何代码使用）；用户板子丝印 **SDC0_CLK=PF2 / SDC0_D3=PF4** 与驱动一致
- **结论：软件配置全对，无需改驱动**。零字节 = 电信号没到 PF4 → 硬件侧（供电/接线位置）

### ③ 回环测试（未完成，明天续）
- 短接 SDC0_CLK/SDC0_D3（PF2/PF4）+ `echo "hello" > /dev/uart0`，`rx=` 无增长（旧固件 rx= 只在收到数据时打印，存在观测盲区 → 已改 ok-20260826-5 无条件打印）
- 注意：设备名是 `/dev/uart0`（有 0），输成 `/dev/uart` 会 `open failed: 2`（ENOENT）

## 遗留事项（明天续排）

1. **烧录 ok-20260826-5** 确认 boot 后每 5s 出现 `rx=0B frames=0`（无条件循环输出）
2. **回环测试复测**：短接 PF2/PF4 + echo /dev/uart0 → `rx=` 应增长（增长=UART0 收发全通，问题在模块侧；不变=J5 口丝印不是真 PF2/PF4，需查原理图）
3. **供电确认**：万用表量模块 VCC 对 GND = 5V（>200mA，平均 82mA）；J5 Debug 口很可能没有 5V → 用 USB-TTL 的 5V 单独给模块供电，只把 TX/RX/GND 接板子
4. **USB-TTL 探测法**：luncher_dm 启动时往 UART0 发配置命令（FD FC FB FA...，115200）——用 USB-TTL RX 逐个探测 J5 口引脚，能收到命令字节的引脚就是真 UART0_TX（一锤定音）

## 验证命令与产物

```bash
# 编译 / 打包 / 资源检查 / 固化（每轮必做）
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
bash /data/vela/check_res.sh && bash /data/vela/git_snapshot.sh   # tag ok-20260826-N
```

产物：vela.bin = nsh.fex（8122896B，ok-20260826-5）；镜像 `lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`。

## 关键协议速查（官方《LD2410B 串口通信协议 V1.08》，已拷 `/data/dm/LD2410B串口通信协议V1.08.pdf`）

- 命令帧（小端）：`FD FC FB FA | len(2B)=2+vlen | cmd(2B) | value(N) | 04 03 02 01`；ACK 命令字 = 发送命令字 | 0x0100
- 改波特率 §2.2.9：`0x00A1`，值 `0x0005`=115200（出厂 `0x0007`=256000），**重启后生效、掉电不丢失**
- 上报帧 §2.1.2：`F4 F3 F2 F1 | 0D 00 | 02 AA | 状态 | 运动距离(2B LE cm) | 运动能量 | 静止距离(2B) | 静止能量 | 探测距离(2B) | 55 00 | F8 F7 F6 F5`；状态 0x00 无/0x01 运动/0x02 静止/0x03 运动&静止
- 配置流程 §2.4.1：`0x00FF` 使能（值 0x0001）→ 配置命令 → `0x00FE` 结束；0x0060 值 18B（运动门字 0x0000+4B、静止门字 0x0001+4B、无人延时字 0x0002+4B）
- 在座判定（luncher_dm.c `dm_prox_get_cm()`）：present<0→-1（纯计时回退）；==0→99 离开；dist≤200cm→0 在座；帧 2s 过期（`g_ld2410b_last_ts`）→-1

*DevLog by AtomCode (deepseek-v4-flash)*
