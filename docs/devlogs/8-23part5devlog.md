# DevLog 2026-08-23 — LD2410B 毫米波雷达替代接近传感器（P117）

> 用户需求：LD2410B（24GHz 毫米波人体存在传感器）替代现有距离感应器（LTR553）感应人体。
> 固化 **ok-20260823-16**。

## 背景

现有链路：LTR553（I2C 接近传感器）→ uORB `sensor_prox` → luncher_dm.c 订阅轮询 → `prox_subject`（cm×10）→ `dm_prox_get_cm()` → dm_health.c 判定在座/离开（≤30cm 在座、>30cm 离开、-1 回退纯计时）。

LTR553 局限：探测距离近（≤30cm 分级值 0/1/5/10/20cm），只能感应"手伸到屏前"，无法真实判断"人坐下/离开"。

## 方案（用户拍板）

| 决策点 | 选择 |
|--------|------|
| 接入层级 | **应用层直读串口**（luncher_dm 开 /dev/ttyS0 读帧解析） |
| 替换方式 | **直接替代**（LTR553 prox 判定废弃，LD2410B 唯一人体感应源） |
| 在座判定 | **距离过滤**（有人且探测距离 ≤ 阈值 2m 才算在座） |

## 硬件接入

- **UART0 空闲可用**：nsh 原始 defconfig 未启用 UART0（UART2 = `CONFIG_UART2_SERIAL_CONSOLE=y` 是 debug 口；蓝牙 rtk_hci 默认走 `/dev/uart1` = 物理 UART1，均不冲突）→ defconfig 启用。
- 接线：VCC(5V)→5V；GND→GND；UART_Tx→PB23(UART0_RX)；UART_Rx→PB22(UART0_TX)。
- ⚠ **模块默认波特率 256000，R528 UART 驱动波特率表无 256000**（map: 300~4000000 跳过 256000）→ 需先用 USB-TTL 把模块波特率配置改为 **115200**（命令持久化），defconfig 已配 `CONFIG_UART0_BAUD=115200`。

## LD2410B 协议要点（官方 V1.08）

上报帧（小端，"目标基本信息" len=0x0D）：
`F4 F3 F2 F1 | len(2B)=0x0D | type=0x02 0xAA | 目标状态(1B) | 运动距离(2B) | 运动能量(1B) | 静止距离(2B) | 静止能量(1B) | 探测距离(2B) | 0x55 0x00 | F8 F7 F6 F5`

目标状态：`0x00` 无 / `0x01` 运动 / `0x02` 静止 / `0x03` 运动&静止（0x04~0x06 为底噪检测，忽略）。

## 改动清单

| 文件 | 改动 |
|------|------|
| `boards/.../nsh/defconfig` | 启用 `CONFIG_R528_UART0=y` + `CONFIG_UART0_BAUD=115200` + `CONFIG_UART0_RXBUFSIZE=2048`（参考 nsh_minidisplay 的 UART1 模式） |
| `dm_ld2410b.h`（新增） | 状态宏 + 有人判定 + 在座阈值（LD2410B_SEAT_CM_DEFAULT=200cm）+ 4 个 API |
| `dm_ld2410b.c`（新增） | open /dev/ttyS0（O_RDONLY|O_NONBLOCK）+ termios 115200 8N1 raw + 常驻线程 read → 帧状态机（ST_WAIT_HDR→LEN→DATA→TAIL）→ 校验 type/0xAA/0x55/0x00 → 提取 state/det_dist；日志 LV_LOG（lvgl.h 头） |
| `luncher_dm.c` | ① include dm_ld2410b.h；② `init_sensors()` 里调 `dm_ld2410b_init()`；③ **`dm_prox_get_cm()` 数据源切换**：有人(≠NONE)且距离≤200cm→返回 0（在座）；无人→99（离开）；串口未就绪→-1（回退纯计时）——dm_health.c **一行未改** |
| `Makefile` | CSRCS 追加 `dm_ld2410b.c` |

## 映射语义

- 返回 0：`present==1 && 0≤dist≤200` → dm_health 判据 `prox≤30` 通过 → 在座
- 返回 99：无人 或 有人但 dist>200 / 距离未知 → `prox>30` → 离开
- 返回 -1：`dm_ld2410b_present()<0`（串口未就绪）→ 回退纯计时模式

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（exit=0，vela.bin 12:46；仅既有 LED %d warning）
2. `lunch_nuttx 2 && pack` ✅（整包镜像）
3. `bash /data/vela/check_res.sh` ✅（固件/字体/12 提示音路径级全对）
4. 固化：**ok-20260823-16**（三仓；nuttx/apps 无改动跳过）

## 遗留 / 待上板

- **硬件**：接线 J5 + 5V 供电（>200mA）；**必须先给模块下发波特率改 115200 命令**（USB-TTL 或官方 App 配置），否则 256000 收不到帧 → 日志看 `[ld2410b] init ok` + `Raw distance`（新模块暂无此日志，需加帧数日志验证）
- **验证点**：板端锁屏健康卡距离读数变化（有人 0cm / 无人 99cm）、坐下 5s 会话启动、离开 5s 清零——与 LTR553 行为一致
- 串口线程 fd 生命周期：应用退出时未 join（沿现有常驻线程惯例，无二次 init 风险——g_ld2410b_ready 防重入）
- 阈值 200cm 目前硬编码 LD2410B_SEAT_CM_DEFAULT，后续可下沉 dm_health_cfg（`ld2410_seat_cm`）

---

# 追加 2026-08-23 — LD2410B 审查修复（review-15，固化 ok-20260823-17）

> 人体传感器是核心器件，按 nuttx-driver-development 六维清单审查后修 5 项（code_review 工具因 /data/dm 非 git 仓根无法运行，人工审查）。

## FAIL1 — 串口线程忙转（100% CPU 空转）

- **现象**：`O_NONBLOCK` 直 read 无数据时 `continue` 立即重读，无 `poll`/睡眠 → 满速空转
- **修复**（`dm_ld2410b.c`）：改 `poll(&pfd, 1, 200)` 带超时等待 → `read`，无数据线程休眠

## FAIL2 — 无帧时误判「离开」，提醒全失效（行为回归）

- **现象**：`present` 只查 `g_ld2410b_ready`，首帧未到/传感器断开时 `state=NONE` → 返回 0 → `dm_prox_get_cm()` 返回 99 → 喝水/久坐提醒永不触发（旧 LTR553 无数据回退纯计时）
- **修复**：`dm_ld2410b_present()` 加 `!g_ld2410b_frame_ok` 判据 → 无有效帧返回 -1（回退纯计时）

## WARN3 — 中途掉线数据永久冻结

- **修复**：帧解析记录 `g_ld2410b_last_ts = lv_tick_get()`，present 判 `lv_tick_get() - last_ts > 2000` → 掉线 2s 返回 -1

## WARN4 — LTR553 prox 订阅死代码清理（luncher_dm.c）

- 删除：`sensor/prox.h` include、`prox_sub` 成员、`LV_DEMO_UPDATE_PROX`、订阅初始化块、轮询块、清理块、`prox_subject`（变量+init）、`prox_observer_cb`（无绑定点死函数）
- `LV_DEMO_POLLFD_NUM` 3→2、`LV_DEMO_UPDATE_ALL` 0x07→0x03
- 净效果：-92 行；prox 数据源唯一 = LD2410B（`dm_prox_get_cm` 读串口）

## WARN5 — 锁屏显示语义修正（ui_home.c）

- `"距离：%d cm"`（0/99 误导）→ `"人体：有人/无人/不可用"`（读 `dm_ld2410b_present()`）

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（exit=0，仅既有 LED %d warning）
2. `lunch_nuttx 2 && pack` ✅
3. `bash /data/vela/check_res.sh` ✅（路径级验证通过）
4. 固化：**ok-20260823-17**（三仓；nuttx/apps 无改动跳过，3 文件 +30/-92）

*DevLog by AtomCode (deepseek-v4-flash)*
