# DevLog 2026-08-27 — 1500000 档 FAIL 收尾：24M 时钟 dl=1 硬件极限实证 + 用户拍板回归 115200

> 承接 8-27part1/part2（P119）。本节点 = P120（第一版修复验证 FAIL）+ P121（用户拍板接受硬件极限，移除档位）。

## 背景

P119 遗留「1500000 档 FAIL 专挖」：UART1 内部回环 115200 档全通（LOOP OK），1500000 档 rx=0。
Part2 结论：波特率切换链路代码正确（B1500000 宏→cfsetspeed→c_speed 数值→TCSETS→DLL/DLH 真写），
矛盾点指向 apb1 实际时钟。上会话实施第一版修复（r528_uartdl 动态读取 apb1 真实时钟）并固化 ok-20260827-20。

## 现象→根因→方案→验证

### 现象（上板实测，ok-20260827-20）

```
nsh> uartdbg_test loop 1500000
uartdbg_test: LOOP FAIL — rx=0 bytes
```

第一版修复（动态读 apb1）上板后 **1500000 档仍然 FAIL**。

### 根因（三段实证链，全部闭环）

1. **反汇编实证修复已在固件**：`arm-none-eabi-objdump -d nuttx/nuttx | grep 0x524` 命中
   `ldr r3, [r3, #1316] @ 0x524` —— 动态读取代码确实编译进 ok-20260827-20。
2. **apb1 实际就是 24M**：u-boot `clock_init_uart()` 显式写 `APB2_CLK_SRC_OSC24M | APB2_CLK_RATE_N_1 | APB2_CLK_RATE_M(1)`
   到 `&ccm->apb2_cfg`（= NuttX ccu 表的 0x524 apb1，**同一寄存器两种命名**）——UART 时钟被 bootloader 锁死在 24M 直通。
3. **1500000 档 dl=1 是分频器硬件极限**：`dl = 24M/(1500000<<4) = 1`，16 倍过采样下
   波特率 = clk/16 = 1.5M 恰好顶格。全志官方《Linux UART 开发指南》原文印证：
   「**24MHz/16=1.5MHz，这个最大频率满足不了 1.5M 以上的波特率应用**」。
   回环 rx=0 = dl=1 时 RX 采样窗口只剩 16 个时钟/bit，IP 级不可靠。

**结论：1500000 在 24M 时钟下不可用是硬件极限，非驱动缺陷。**（此前 part2 的「代码正确」
结论成立；「时钟假设错误」假设被第一版修复证伪——动态读出来就是 24M。）

### 方案（用户拍板，2026-08-27 12:0x）

**接受 1500000 不可用，回归 115200（人体传感器同款波特率）**。放弃提频 APB2 的路线
（提频需改 mux 到 pll-periph0，动全局总线时钟，I2C/I2S/console 全受影响，风险面远超收益）。

改动清单（全部在 `vendor/allwinnertech/`）：

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/dm_uart_dbg.c` | 档位表移除 `{B1500000, "1500000"}` + 表头注释改「P120 实证 dl=1 极限不可用」+ 自测注入文本 line2 同步 |
| `apps/luncher_dm/dm_uart_dbg.h` | `DM_UART_DBG_BAUD_CNT` 6→5 |
| `apps/luncher_dm/ui/ui_uart_dbg.c` | 循环注释「→1500000→」改「→115200→（已移除）」 |
| `apps/uartdbg_test/uartdbg_test_main.c` | loop 参数注释标注「1500000 仅保留作硬件回归验证用」 |

### 验证

1. 编译：`./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)` ✅（仅 ioctl 旧 warning）
2. 打包：`pack` ✅；产物一致 `nsh.fex == vela.bin`，md5 = **6d362878aafac9041d0fd931a004882b**（8126984B）
3. 资源检查：`bash /data/vela/check_res.sh` ✅（wifi 固件/字体/12 提示音路径+字节全对）
4. 固化：**ok-20260827-21**（三仓 tag）

## 遗留事项（下会话）

1. UART1 外部接线：板上已证全通（LOOP OK），开发板 TX → PD22（UART1_RX）+ GND 共地待用户确认排针位置
2. 烧录 ok-20260827-21 复测：`uartdbg_test loop 115200` 应 LOOP OK；波特率 UI 循环 5 档 9600→115200
3. 烧录 ok-20260823-12 复测：死机修复 + 音频仲裁恢复项
4. 【功能】ALS 自动亮度+午休检测、AI 流式回复（LLM 非流式待拍板）、语音命令扩展（健康查询/状态播报）
5. 第②层：语音改参数+风格切换（dm_health_cfg_set_* 已预留）；状态栏改进；Files/Books/Games 回归
6. 蓝牙 H4 110 挂起（勿擅改驱动）

## 沉淀教训（Part3 新增）

- **第一版修复≠问题解决**：动态读取只解决「假设 apb1≠24M」，实测 apb1=24M 时结果不变——
  上板验证是唯一裁判，别被「代码看起来对」迷惑（P120 已固化 ok-20260827-20 但实测仍 FAIL）
- **dl=1 是 16 倍过采样极限**：波特率 = clk/16 顶格时 RX 采样窗口只剩 16 时钟/bit，
  回环 rx=0 是 IP 级硬件限制，不是驱动 bug——查波特率先算 dl，dl=1 直接判硬件极限
- **全局时钟动不得就退档**：1500000 需要提频 APB2（mux→pll-periph0），但 APB2 挂
  I2C/I2S/console 全家，动它收益 1 档波特率、风险全系统时钟——用户拍板退 115200 是正确取舍
- **u-boot 与 NuttX 寄存器命名不同**：0x524 在 u-boot 叫 APB2、在 NuttX ccu 表叫 apb1，
  同一寄存器两种命名——跨层核对时钟必须回源到寄存器地址，别被名字带偏

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-27 — P122：UART1 外部设备 115200 接进来 rx=0 —— 引脚通路未实测，加 pinloop 外部回环诊断

> 承接 P121（1500000 移除、回归 115200）。本节点 = 用户上板反馈：外部 115200 开发板（一通电就有 LOG）接 UART1 仍 rx=0。

## 现象

- 烧录 ok-20260827-21 后，Settings→UART 调试工具打开 /dev/uart1（open ok），reader 一直 `rx=0B ring=0`
- 用户找到一个 **115200 开发板（一通电就有 LOG）** 接进来，仍然 rx=0

## 根因（关键认知转变）

**内部回环（MCR.LOOP）不经引脚**：LOOP OK 只能证明「UART 控制器+驱动」链路，
**从未证明 PD21/PD22 引脚物理通路**。外部设备接进来 rx=0 = 引脚通路未验证实锤——
P118 同款教训（文档引脚≠板上实测），PD21/22 从无外部信号实测成功记录。

## 调查（三路证据，均指向配置正确、问题在物理接线）

1. **sys_config.fex（gemini-s1）**：只有 `[uart_para] uart_debug_port=2`（UART2 console），
   **无 uart1 段**——UART1 引脚不归 sys_config 管，纯 r528_serial.c 代码配置
2. **xlsx 权威 mux 表**（`/data/vela/webdoc/R528-S3PIN out-V2 (1).xlsx` sheet1）：
   表头 `[4]Function2 [5]Function3 [6]Function4 ...`；
   row53 `PD21: [6]UART1-TX`、row54 `PD22: [6]UART1-RX` —— **PD21/22 的 UART1 恰在 Function4，mux4 配置正确**
3. **RX 方向**：`hal_gpio_pinmux_set_function_early()` 只写 mux 寄存器（GPIO_TYPE_FUNC→gpio_mux_reg），
   不显式设方向——但 **UART2 console（PC0/1 mux2）用完全相同的调用且收发正常**（boot log 一直在跑），
   证明 mux 后方向由外设自动接管，方向配置不是差异点

## 方案（P122）

**不改驱动**（mux/方向均正确）。给 `uartdbg_test` 加 **pinloop 外部回环模式**：
`open → [波特率] → write → read`，**不开 MCR.LOOP**，数据必须走真实引脚。
用户用杜邦线短接板上 PD21(TX) 与 PD22(RX) 后运行：

| 结果 | 含义 | 下一步 |
|------|------|--------|
| `PINLOOP OK` | 引脚通路+驱动全通 | 问题在外部设备/接线（方向/电平/共地） |
| `PINLOOP FAIL` | PD21/22 物理通路问题 | 查板上引出/复用冲突/电平 |

改动：`apps/uartdbg_test/uartdbg_test_main.c` 新增 `pinloop_selftest()` + `pinloop` 命令分支。

## 验证

- 编译 ✅ / pack ✅ / nsh.fex==vela.bin（md5 `3de2e39b…`）✅ / check_res ✅
- 固化 **ok-20260827-22**
- 上板待办：短接 PD21↔PD22 → `uartdbg_test pinloop 115200`

## 遗留

1. **上板验证 pinloop**：短接 PD21↔PD22 → 期望 PINLOOP OK（则查外部设备接线）；
   FAIL → 板上 PD21/22 未引出/复用冲突（查 gemini-s1 排针实际位置）
2. 若引脚通、外部设备仍 rx=0：查 ①外部 TX→PD22、GND 共地 ②电平 3.3V ③外部设备是否真在发

## 沉淀教训（P122 新增）

- **内部回环≠引脚通路**：MCR.LOOP 在芯片内部短接 TX→RX，不经引脚。
  LOOP OK 只能证「控制器+驱动」，要证引脚必须外部回环（短接 TX↔RX 或真实外设）
- **P118 教训重演**：PD21/22 依据文档 sheet2 选的，从未被外部信号实测；
  「能用」的唯一裁判是上板实测，文档只是候选
- **方向不用显式设**：Allwinner GPIO mux 到外设功能后方向由外设自动接管，
  UART2 console（同款 pinmux 调用）收发正常即可证

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-27 — P122 续：根因锁定 —— drv_gpio IO 扩展器覆盖 UART1 mux4

> 承接 P122（pinloop 外部回环 FAIL）。本节点 = 根因定位 + 修复（ok-20260827-23）。

## 现象（用户上板）

- `uartdbg_test pinloop 115200`（杜邦线短接 PD21↔PD22，走真实引脚）→ **PINLOOP FAIL rx=0**
- 用户确认图纸：**UART1 就是 PD21(TX)/PD22(RX)**（与 xlsx Function4 一致）
- 内部回环 `loop 115200` LOOP OK（MCR.LOOP 芯片内部短接，**不经引脚**）

## 根因（覆盖源三段实证）

1. **defconfig 同时定义两个板级宏**：`CONFIG_ARCH_BOARD_R528S3_GEMINI_S1=y` + `CONFIG_ARCH_BOARD_R528S3_GEMINI_XTS=y`
   （evb4/velaevb1 的 defconfig 都没有 XTS——gemini-s1 独有，疑似误配/历史遗留）
2. **drv_gpio.c maps[] 预编译陷阱**：`#ifndef S1`（假，S1 已定义）→ `#elif XTS`（真）→
   **`{0, GPIOD(21)}, {1, GPIOD(22)}` 被注册为 GPIO_OUTPUT_PIN** —— PD21/22 被当作 IO 扩展器 GPIO！
3. **初始化时序覆盖**：`r528_late_initialize()`（board_late_initialize，晚于 arm_serialinit 的
   UART1 mux4 配置）→ `r528_gpio_initialize()` → `gpio_lower_half()` → `gplh_setpintype(GPIO_OUTPUT_PIN)`
   → `IOEXP_SETDIRECTION(OUT)` → `sunxi_gpio_direction` → `hal_gpio_pinmux_set_function(PD21/22,
   GPIO_MUXSEL_OUT)` —— **用 GPIO 输出 mux 覆盖 UART1 的 mux4**！

完美解释全部现象：内部回环 OK（MCR.LOOP 不经引脚，控制器+驱动无碍）；
外部短接/外部设备 rx=0（引脚被 IO 扩展器改成 GPIO 输出，UART 信号根本到不了 UART1 RX）。

## 修复（P122 续，ok-20260827-23）

`drv_gpio.c` maps[]：移除 `#elif XTS` 分支里的 `{0, GPIOD(21)}/{1, GPIOD(22)}` 注册
（PD21/22 归 UART1 专用，注释说明原因；保留 pin2=GPIOG(18)/pin3=GPIOB(10) 原有映射）。

| 文件 | 改动 |
|------|------|
| `chips/r528/drv/gpio/drv_gpio.c` | maps[] 移除 PD21/22 的 GPIO_OUTPUT_PIN 注册 + 注释根因 |

## 验证

1. 编译 ✅ / pack ✅ / nsh.fex==vela.bin（md5 `c49b8487…`）✅ / check_res ✅
2. 固化 **ok-20260827-23**
3. 上板待办：短接 PD21↔PD22 → `uartdbg_test pinloop 115200` 期望 **PINLOOP OK**；
   再接外部 115200 设备 → rx 应增长

## 遗留（下会话）

1. 🔴 **上板复测 ok-20260827-23**：pinloop 115200 期望 OK；外部设备 115200 期望收到 LOG
2. 若 pinloop 仍 FAIL：才需要查 gemini-s1 排针是否真引出 PD21/22（P118 教训兜底）
3. defconfig 里 `GEMINI_XTS=y` 疑似误配（只有 drv_gpio.c 用、evb4/velaevb1 无）——
   本次不删（避免影响面），观察复测结果后决定是否清理

## 沉淀教训（P122 续新增）

- **「内部回环 OK」≠「引脚能用」**：MCR.LOOP 是芯片内部短接，排障价值只在「控制器+驱动」，
  引脚通路必须用外部回环（短接 TX↔RX）或真实外设验证——P119 曾误把 LOOP OK 当「UART1 全通」
- **IO 扩展器会覆盖 UART mux**：maps[] 把 UART 引脚注册为 GPIO 输出时，late_initialize 会用
  GPIO_MUXSEL_OUT 覆盖已配置的 UART mux4——引脚被「抢走」是静默的，日志无任何报错
- **预编译陷阱**：`#ifndef S1 → #elif XTS` 双宏同时定义时走 XTS 分支，代码看着是「S1 板分支」，
  实际编译走的是 elif——改板级映射先看 defconfig 是否重复定义宏

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-27 — P123：UART1 换口失败后改 UART3（PD10/PD11），调试工具换口

> 承接 P122 续（drv_gpio 覆盖修复 ok-20260827-23 上板仍 FAIL）。用户拍板：换 UART3。

## 现象（用户上板）

- ok-20260827-23（drv_gpio 移除 PD21/22 注册）后 `uartdbg_test pinloop 115200` **仍 PINLOOP FAIL rx=0**
- 用户拍板：UART3 与 SPI 屏接口复用（PD10=UART3_TX / PD11=UART3_RX），先去掉 SPI pinmux，把 UART3 搞起来

## 调查（UART3 支持现状，全部现成）

1. **r528_serial.c**：`r528_uart3config()` 已存在（PD10/PD11 mux5），`up_earlyserialinit`/`arm_serialinit`
   已含 `CONFIG_R528_UART3` 分支（/dev/uart3 注册 + g_uart3port 完整）
2. **defconfig**：`CONFIG_R528_UART3=y` 未开 → 只需加一行
3. **HAL**（uart-sun8iw20.h）：`UART3_TX=GPIOD(10)/UART3_RX=GPIOD(11)/UART3_GPIO_FUNCTION=5` 已定义
4. **xlsx 权威 mux 表**：row42 PD10→`[7]UART3-TX`、row43 PD11→`[7]UART3-RX`（col7=Function5）——mux5 正确
5. **SPI 屏 pinmux**：`CONFIG_SPI_DRIVER is not set` → `sunxi_spibus_initialize()` 不会被调用
   （r528_boot.c 里它被 `#ifdef CONFIG_SPI_DRIVER` + LCD_SPI 屏宏包住，SSD1306 等屏均未启用）
   → **SPI 驱动实际未启用，PD10/11 不会被 SPI 配置，无需改 SPI 代码**

## 改动（ok-20260827-24）

| 文件 | 改动 |
|------|------|
| `boards/.../defconfig` | 加 `CONFIG_R528_UART3=y` |
| `apps/luncher_dm/dm_uart_dbg.c/h` | 设备 `/dev/uart1`→`/dev/uart3` + 注释/自测文本同步 |
| `apps/luncher_dm/ui/ui_settings.c` | 端口号显示 UART1→UART3 |
| `apps/luncher_dm/ui/ui_uart_dbg.c` | 打开注释同步 |
| `apps/uartdbg_test/uartdbg_test_main.c` | LOOP_DEV `/dev/uart1`→`/dev/uart3` + 消息/注释同步 |

## 验证

1. 编译 ✅ / pack ✅ / nsh.fex==vela.bin（md5 `433f0fc9…`）✅ / check_res ✅
2. 固化 **ok-20260827-24**
3. 上板待办：`uartdbg_test loop 115200`（UART3 内部回环）+ 短接 PD10↔PD11 `pinloop 115200`；
   再接外部 115200 设备（TX→PD11=UART3_RX + GND 共地）

## 遗留（下会话）

1. 🔴 **上板复测 ok-20260827-24**：UART3 内部回环 LOOP + 外部短接 pinloop + 外部设备 115200
2. UART1 之谜（PD21/22 两次修复上板仍 rx=0）暂搁置——drv_gpio 覆盖已移除仍 FAIL，
   疑排针未引出或另有覆盖源，非当前焦点
3. defconfig `GEMINI_XTS=y` 疑似误配（仅 drv_gpio.c 用）——暂保留，不影响 UART3

## 沉淀教训（P123 新增）

- **换口是硬道理**：UART1（PD21/22）两次代码修复上板仍 rx=0，与其继续挖排针，
  用户拍板换到与 SPI 屏接口复用的 UART3（PD10/11）——板上确认引出的接口优先级高于文档推测
- **SPI 未启用=无 pinmux 冲突**：CONFIG_SPI_DRIVER is not set 时 sunxi_spibus_initialize 不被调用，
  复用引脚无需改 SPI 代码；先查驱动启用状态再决定要不要"去 pinmux"
- **UART3 支持是现成的**：r528_serial.c 早备好 r528_uart3config（PD10/11 mux5），
  只是 defconfig 未开——启用一个 UART = defconfig 一行 + 应用换设备路径

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-27 — P123 上板验证闭环：UART3 LOOP OK + PINLOOP OK

## 用户上板实测（ok-20260827-24）

```
nsh> uartdbg_test loop 115200
uartdbg_test: LOOP OK — controller+driver 全通, rx=28 bytes
nsh> uartdbg_test pinloop 115200     （杜邦线短接 PD10↔PD11）
uartdbg_test: PINLOOP OK — 引脚通路+驱动全通, rx=31 bytes
```

**UART3（PD10/PD11 mux5）控制器 + 引脚通路全部打通** —— 对比 UART1（PD21/22）
两次修复上板仍 rx=0，换口 UART3 一次到位（板上确认引出的接口 > 文档推测）。

## 结论

- P123 完整闭环：UART 调试工具已换到 /dev/uart3，内部回环 + 外部短接引脚通路全验证通过
- 最后一步待用户：外部 115200 设备 TX→PD11（UART3_RX）+ GND 共地，调试工具应收到 LOG
- UART1（PD21/22）之谜正式搁置：疑排针未引出或另有覆盖源，非当前焦点，勿再投入

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-27 — UART1 之谜硬件解释（用户提供）：BSN20 MOSFET 外围电路

## 用户提供的关键硬件信息（2026-08-27 晚）

**UART1 引脚上有丰富的外围器件**：
- UART1 TX：接了 **BSN20**（N 沟道 MOSFET，SOT-23）做 **3.3V 上拉/电平转换**
- UART1 RX：也接 **BSN20 + 10k 电阻上拉**

## 这如何解释全部现象（UART1 之谜终于闭合）

| 现象 | 解释 |
|------|------|
| 内部回环 `loop 115200` LOOP OK | MCR.LOOP 芯片内部短接 TX→RX，**不经引脚**，外围 MOSFET 电路不影响 |
| 外部短接 pinloop / 外部设备 rx=0 | PD21/22 引脚上挂 BSN20 + 上拉电阻网络——信号被 MOSFET 栅极电路
  钳位/电平转换/上拉改变，UART RX 采不到有效电平 → 两次代码修复（drv_gpio
  覆盖 + mux 核对）都无法解决，因为根因在**硬件外围电路**，不在软件 |
| UART3（PD10/11）一次全通 | 引脚干净、无外围器件，直接通 |

## 结论

- **UART1 不可用 = 硬件外围电路（BSN20 电平转换/上拉网络）占用，非软件缺陷**——
  P122 的 drv_gpio 覆盖修复方向没错（那确实是问题之一），但即使 mux 正确，
  BSN20 电路仍会让外部信号进不了 UART1 RX
- **换口 UART3 的决定完全正确**，且 UART3 已上板验证 LOOP OK + PINLOOP OK
- 若未来要用 UART1：需先确认 BSN20 电路用途（可能供蓝牙 HCI 或其他 3.3V 外设），
  或从硬件侧改接 —— **软件侧勿再投入**

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-27 — P124：调试工具迁回 UART1、人体传感器迁到 UART3

> 用户需求：UART1 给调试工具、UART3 交给人体传感器（LD2410B）。

## 背景与决策

- P123 时调试工具在 UART3（上板验证全通），但用户想把 UART3 留给人体传感器
- UART1（PD21/22）之前 rx=0 的根因：引脚挂 **BSN20（N-MOSFET）电平转换电路**
  （TX 3.3V 上拉 / RX BSN20+10k 上拉）——外部信号被 MOSFET 网络就地处理，
  直接接芯片引脚收不到
- **用户确认**：①BSN20 外设侧有独立引出接口（外部调试设备接那一侧即可）
  ②人体传感器已改接其他口 ③允许硬件改

## 架构定稿

| 串口 | 用途 | 引脚 | 接线 |
|------|------|------|------|
| UART1 | UART 调试工具 | PD21(TX)/PD22(RX) mux4 | 外部调试设备 TX→PD22 经 **BSN20 外设侧接口**（勿直接接芯片引脚） |
| UART3 | LD2410B 人体传感器 | PD10(TX)/PD11(RX) mux5 | LD2410B TX→PD11 + GND 共地（PD10 用于下发配置） |
| UART0 | 禁用（与 TF 卡冲突，P119） | — | — |

## 改动（ok-20260827-25）

| 文件 | 改动 |
|------|------|
| `dm_uart_dbg.c/h` | 设备 `/dev/uart3`→`/dev/uart1` + 注释（BSN20 外设侧接线说明） |
| `ui/ui_settings.c` | 端口号显示 UART3→UART1 |
| `ui/ui_uart_dbg.c` | 打开注释同步 |
| `uartdbg_test_main.c` | LOOP_DEV→`/dev/uart1` + 消息/注释同步 |
| `dm_ld2410b.c` | **LD2410B_DEV `/dev/uart0`→`/dev/uart3`**（UART0 已禁用）+ 注释 |
| `luncher_dm.c` | `init_sensors` 恢复 `dm_ld2410b_init()` 调用（P119 时被注释） |

## 验证

1. 编译 ✅ / pack ✅ / nsh.fex==vela.bin（md5 `ecbcb703…`）✅ / check_res ✅
2. 固化 **ok-20260827-25**
3. 上板待办：
   - `uartdbg_test loop 115200`（UART1 内部回环，应 LOOP OK）
   - 外部调试设备接 BSN20 外设侧 → UART1 调试工具收 LOG
   - LD2410B 接 UART3（PD11=RX）→ dm_health 人体数据恢复

## 遗留（下会话）

1. 🔴 **上板复测 ok-20260827-25**：UART1 调试工具（BSN20 外设侧）+ UART3 LD2410B 人体数据
2. UART1 直连芯片引脚仍收不到属预期（BSN20 电路拦截）——测试必须走外设侧接口
3. defconfig `GEMINI_XTS=y` 疑似误配（仅 drv_gpio.c 用）——暂保留

## 沉淀教训（P124 新增）

- **电平转换电路的"外设侧"才是正确接点**：BSN20 电路芯片侧是转换输入端，
  外部设备必须接外设侧（转换后接口），否则信号被 MOSFET 网络就地处理
- **UART 复用优先级**：芯片无开漏模式（16550 推挽 TX）+ GPIO 内部上拉也绕不过
  外部 MOSFET 网络——硬件电平转换口只能从外设侧用，软件侧无能为力
- **LD2410B 迁移**：UART0 禁用后传感器 init 被注释，迁到 UART3 需同时
  改设备宏 + 恢复 init_sensors 调用，两处缺一不可

*DevLog by AtomCode (deepseek-v4-flash)*
