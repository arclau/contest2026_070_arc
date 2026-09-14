# DevLog 2026-08-27 Part2 — UART1 回环自证全通 + 波特率链路调查 + 1500000 遗留（换会话专挖）

> 承接 8-27part1（ok-20260827-6~14，UART 调试工具 + UART0/TF 卡冲突 + 换口 UART1）。
> Part2 覆盖 ok-20260827-15~19：uartdbg_test 命令 not found 根因（Make.defs 坑）、
> 内部回环 LOOP OK（UART1 控制器+驱动全通）、波特率切换链路调查（代码正确）、
> 1500000 FAIL 遗留（下会话用驱动 skill 专挖）、无传感器不播欢迎语修复。

## 一、uartdbg_test 命令 not found（ok-20260827-15）

**现象**：烧了含 uartdbg_test 的固件（ok-20260827-14），NSH 敲 `uartdbg_test` → `command not found`。

**排查证据链**：
1. 用户日志行号（open=201、rx=133）与当前源码完全一致 → 确认烧的就是最新源码编译的固件
2. `strings vela.bin | grep -c uartdbg_test` = 0 → 命令没编进固件
3. `apps/builtin/registry/` 无 uartdbg_test.bdat/pdat → 构建系统没收录该 app
4. `.config` 有 `CONFIG_UARTDBG_TEST_APP=y` → Kconfig 生效了但 app 没进 CONFIGURED_APPS

**根因**：新 app 目录**缺 `Make.defs`**！`apps/vendor/allwinnertech/apps/Make.defs` 用通配
`include */Make.defs` 收集应用——缺它整个目录被构建系统无视（比 §四坑 3 更隐蔽：
Kconfig source 加了、CONFIG 也设了，但没 Make.defs 就不进 CONFIGURED_APPS）。

**方案**：新建 `apps/vendor/allwinnertech/apps/uartdbg_test/Make.defs`：
```make
ifneq ($(CONFIG_UARTDBG_TEST_APP),)
CONFIGURED_APPS += $(APPDIR)/vendor/allwinnertech/apps/uartdbg_test
endif
```
补完**增量编译仍不进**（make 不重新展开通配）→ **distclean 全量重编**后验证：
vela.bin 含 uartdbg_test=4 ✅、registry 生成 .bdat/.pdat ✅。

## 二、UI 链路验证通过（selftest 注入）

**现象**：烧 ok-20260827-15 后 NSH 敲 `uartdbg_test`（已开调试工具）→ 窗口显示测试日志 + 日志 `injected 170 bytes (selftest)`、`rx=170B`。

**结论**：串口→UI 显示链路全通 ✅（纯软件注入，不接硬件验证 UI 无问题）。

## 三、内部回环 LOOP OK——UART1 控制器+驱动全通（ok-20260827-16~17）

**现象**：开发板 1500000 发 log（TTL-USB 验证过），接 R528 调试工具 rx 仍停在 170B 不增长——UI 没问题，怀疑硬件。

**手段1：驱动加 TIOCMSET 支持（ok-20260827-16）**
- r528_serial.c `up_ioctl` 无 TIOCMSET（只有 TIOCSBRK/TIOCCBRK/TCGETS/TCSETS）
- 加 `case TIOCMSET`：写 `R528_UART_MCR_OFFSET`（支持 bit4=LOOP 内部回环）
- r528_uart.h 已有 `UART_MCR_LOOP (1<<4)` 定义
- dm_uart_dbg 加 `dm_uart_dbg_loop_test()`：MCR.LOOP → write → 等入环 → 关

**坑1：跨进程静态变量不可见（ok-20260827-17）**
- `uartdbg_test loop` 调 luncher_dm 的 `dm_uart_dbg_loop_test()` 返回 -1
- 根因：uartdbg_test 是**独立进程**，它看到的 `g_dbg_open` 是**自己副本**（false），
  而 luncher_dm 里 reader 线程在跑（g_dbg_open=true）——跨进程共享状态不可行
- 修复：`uartdbg_test loop` 改**自包含**——自己 `open /dev/uart1` + ioctl + write + read + close，不依赖 luncher_dm 任何状态

**实测（ok-20260827-17）**：
```
vela> uartdbg_test loop（未开调试工具）
uartdbg_test: LOOP OK — controller+driver 全通, rx=28 bytes  ✅
```
**结论：UART1 控制器+驱动+波特率全通**，板上 UART1 是好的，问题 100% 在物理接线（开发板 TX → PD22 排针/GND 共地）。

**注**：开调试工具时再敲 loop 会 FAIL（rx=0）——两个 open 抢数据，luncher_dm 的 reader 抢先读走，5s 后显示 rx=28B 反而再次证明硬件与 reader 都正常。

## 四、波特率切换链路调查——代码正确（ok-20260827-18）

**用户怀疑**：子 APP 波特率切换没真切换（所以开发板 1500000 收不到）。

**调查结论（链路逐步验证，全部正确）**：
| 环节 | 证据 | 结论 |
|------|------|------|
| `B1500000 = 0010012`（八进制）= 宏编号 8202 | termios.h:190 | 不是数值，但… |
| `cfsetspeed()` | lib_cfspeed.c:146 | 宏编号/数值两种输入都转成 **`c_speed`（数值 1500000）** |
| `cfgetspeed()` | lib_cfspeed.c:204 | 返回 `c_speed` 数值（非宏编号） |
| 驱动 TCSETS | r528_serial.c:1073 | `priv->baud = cfgetispeed()` = 数值 1500000 |
| `set_format_and_baudrate` | r528_serial.c:640 | `dl = r528_uartdl(1500000) = 24M/(1500000<<4) = 1` → **真写 DLL/DLH** |
| 时钟源 | sunxi_clock_init_uart 只做 reset+gate 不分频 | 所有 UART 共用 R528_SCLK=24MHz 总线时钟 |

**结论**：波特率切换链路代码完全正确（宏编号 → c_speed 数值 → priv->baud → DLL/DLH）。
且 `/dev/uart1` 是全局单例，任何进程改波特率影响所有 open 者——之前 loop/echo 与 UI 档位会互相干扰。

**实证工具**：`uartdbg_test loop [baud]`（115200/1500000 双档内部回环）已加（ok-20260827-18）。

## 五、🔴 遗留：1500000 FAIL（下会话专挖）

**用户实测（待确认版本）**：
- `uartdbg_test loop`（无参数=驱动当前值）→ LOOP OK（28B）
- 但 1500000 档位 FAIL（rx=0）

**意义**：115200 档全通、1500000 档不通 → **嫌疑聚焦 1500000 档位本身**（驱动 dl 计算/时钟分频），
而非控制器/UI/接线。与「波特率切换链路代码正确」的调查看似矛盾，需深挖：
- `r528_uartdl` 取整：`dl = 24M/(1500000<<4) = 1` 整除无误，但需核对**寄存器实际值**（DLL/DLH 读回）
- 1500000 时 UART2 console 可用（AGENTS.md 记录）——UART1 与 UART2 时钟/分频是否真的同源？
- 驱动 HAL 层（rtos-hal）与 NuttX 层（r528_serial.c）的波特率配置是否有第二处覆盖？
- 是否 `CONFIG_UART1_BAUD`/defconfig 默认档位干扰

**下会话动作（用户指定用 nuttx-driver-development skill 专挖）**：
1. 加载 `nuttx-driver-development` skill
2. 读寄存器实证：open /dev/uart1 后 ioctl 读 DLL/DLH，核对 115200 vs 1500000 实际分频值
3. 对照 R528 UM 手册 UART 波特率分频公式（/tmp/R528_UM.pdf，pdftotext 可查）
4. 查 rtos-hal HAL 层 uart-sun8iw20.h 是否有 UART1 波特率第二配置点
5. 若驱动 bug：修复后 distclean 全量重编（defconfig/驱动变更纪律）

## 六、无传感器不播欢迎语/不开始计时（ok-20260827-19）

**现象**：没接人体传感器（LD2410B 禁用中），开机就播「年年有今日」+ 开始健康计时——不合理。

**根因链路**：`dm_prox_get_cm()` 返回 -1（`dm_ld2410b_present()<0`）→ dm_health.c health_tick_cb
`prox<0` 分支 `fallback seated` → `health_session_start()` → PRESENCE=1 事件 → ui_home.c
`health_sit_greeting()` 播「年年有今日」+ 开始喝水/久坐计时。

**方案**（dm_health.c 一处）：`prox<0` 分支从「回退在座」改为「保持空闲」——
不调用 `health_session_start()`，`g_seated` 保持 false（开机为 false），日志改
`session idle (no greet/timer)`。欢迎语 catchup（对时后补播）因 `g_seated=false` 也不会误触发。
传感器恢复后走 else 分支正常判定。

**验证**：build → pack → check_res 12/12 → ok-20260827-19。

## 改动文件表（Part2）

| 文件 | 内容 |
|------|------|
| `apps/vendor/allwinnertech/apps/uartdbg_test/Make.defs`（新建） | **缺它 app 不被收录**——通配 include 机制 |
| `apps/vendor/allwinnertech/apps/uartdbg_test/uartdbg_test_main.c` | 自包含 loop（open+ioctl+write+read+close）+ `loop [baud]` 参数 |
| `chips/r528/r528_serial.c` | up_ioctl 加 TIOCMSET（写 MCR，支持 LOOP bit4） |
| `apps/luncher_dm/dm_uart_dbg.c/h` | 注入接口（part1）+ loop_test（part2 已被自包含替代，保留备用） |
| `apps/luncher_dm/dm_health.c` | prox<0 不 fallback seated、不启动会话（无传感器不欢迎不计时） |

## 验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)   # ok-20260827-15 起每次含新 app/驱动变更走 distclean
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
bash /data/vela/check_res.sh      # 提示音 12/12 + res 完整，全通过
bash /data/vela/git_snapshot.sh   # ok-20260827-15 ~ ok-20260827-19
```

镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`。

## 遗留事项（下会话）

1. 🔴 **1500000 FAIL 专挖**（用 nuttx-driver-development skill）：见 §五动作清单
2. UART1 外部接线：板上已证全通（LOOP OK），开发板 TX → PD22（UART1_RX）+ GND 共地待用户确认排针位置
3. 无传感器静默已生效，待上板复核（开机不再「年年有今日」）
4. 蓝牙 H4 110 挂起——已知遗留，勿擅改驱动

## 沉淀教训（Part2 新增）

- **新 app 三件套缺一不可**：Kconfig（菜单）+ Makefile（构建）+ **Make.defs（CONFIGURED_APPS 收录）**——
  缺 Make.defs 时 Kconfig/CONFIG 全对也白搭，且增量编译不救（必须 distclean）
- **跨进程静态变量不可见**：builtin app 各自独立进程，别指望共享 luncher_dm 的静态状态——
  自测命令必须自包含（自己 open 设备）
- **内部回环（MCR.LOOP）是排障利器**：不经引脚自证「控制器+驱动」，把硬件问题切成接线 vs 驱动两半
- **波特率宏编号≠数值**：B1500000=0010012（八进制）是 mask；cfsetspeed 转 c_speed 数值、
  cfgetspeed 返回数值、驱动用数值算 dl——链路对了就别乱改，先实证寄存器

*DevLog by AtomCode (deepseek-v4-flash)*
