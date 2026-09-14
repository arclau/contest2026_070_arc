# DevLog 2026-08-27 — UART 调试工具 + UART0 与 TF 卡冲突真相 + 换口 UART1

> 延续 P118（LD2410B 上板排障）遗留：UART0 到底能不能用。今日新增
> Settings → UART 调试工具子 APP（接收外部设备串口 log），并借它把
> 「UART0 零字节」的真因查清：**gemini-s1 板上 UART0 与 TF 卡共用
> PF2/PF4，P118「引脚正确」结论是误判（错用 velaevb1 佐证）**。随后
> 全链路禁用 UART0、启用板上空闲的 UART1，并新增 NSH 自测命令。

## 背景

- 用户需求：Settings 加「UART 调试工具」，接收其他设备经串口发来的 debug log，并验证 UART0 是否可用（P118 遗留：LD2410B 挂在 /dev/uart0 收不到任何字节）。
- 约束：HOME 不动、沿用子页框架、中文、触摸操作、波特率含 1500000、不纯黑白底（3 套 IDE 配色）、与 LD2410B 互斥=直接禁用 LD2410B 初始化（用户拍板）。
- 今天经历：功能搭建 → 布局打磨（3 次）→ 零字节排障（2 次反转）→ 换口 UART1 → 自测命令，固化 ok-20260827-6 ~ ok-20260827-14。

## 一、UART 调试工具功能搭建（ok-20260827-1~5，前段会话）

**现象**：Settings 里需要「UART 调试工具」入口，点开是独立子页，显示串口收到的 log。

**方案**：沿用 show_subpage/close_subpage 子页框架。
- `dm_uart_dbg.c/h`：open `/dev/uart0`（当时仍以为用 UART0）+ 后台线程 poll(200ms)→read → 8KB 环形缓冲（单写单读无锁）；波特率表 6 档含 1500000（B1500000 在 termios 存在，dl=24MHz/(1500000<<4)=1 整除，UART2 console 实测可用；256000 除不尽排除）。
- `ui/ui_uart_dbg.c`：日志窗口 + 工具栏（状态/波特率/ASCII-HEX/暂停/清屏/3 配色：One Dark 深色/浅色/豆沙绿护眼，当前主题蓝色描边高亮）。
- 接线：deskmate_ui.c show_subpage dispatch + close_subpage 调 cleanup；ui_settings.c 入口行 + 回调；Makefile 加源文件。
- LD2410B 让位：`luncher_dm.c` 注释 `dm_ld2410b_init()`，删 `dm_ld2410b_deinit()`（无调用者）；dm_uart_dbg 内互斥逻辑删除。

**坑（布局）**：content pad_top=TOP_BAR_H+DM(48)=247px 顶距 → 标题/工具栏叠左上角；缺 parent flex column（ui_wifi 同坑）；`LV_FLEX_ALIGN_STRETCH` 是 grid 枚举 flex 没有 → 改 START + `lv_pct(100)` 撑高。

## 二、布局打磨 3 次（ok-20260827-6~8）

**用户需求**：①返回键那一横行全留白 → 工具栏上移右移；②删状态文字/显示模式 → 一行只留 波特率/暂停/清屏/配色；③日志窗口底部留白太多 → 扩大。

| 固化 | 改动 |
|------|------|
| ok-20260827-6 | 工具栏与日志窗口 **FLOATING 定位到 subpage_overlay**（绕过 content pad_top=247px）：工具栏 pos(DM(16)+DM(40)+DM(10), TOP_BAR_H+DM(4)) 与返回键同行；日志窗口从返回键底边 TOP_BAR_H+DM(44)=238px 起；删大标题；set_pos 后加边界断言 |
| ok-20260827-7 | 删「已连接 N 字节」状态文字 + 「显示 ASCII」按钮（`uart_dbg_status_lbl`/`uart_dbg_mode_cb`/`uart_dbg_hex` 全删），feed 固定 ASCII（可打印直出、\r\n 归一、非打印转 '.'） |
| ok-20260827-8 | 日志窗口高度 `-EDGE_PAD(96)` → `-DM(8)=19px`：底边 1103.6 → 1180.6，高 866 → 943 |

## 三、子 APP 打开不锁屏（ok-20260827-9）

**现象**：跑 UART 调试工具时 30s 超时被锁屏挡住；用户要求任何子 APP 打开都不锁，只有回 HOME 才 30s 锁。

**根因**：`idle_timer_cb`（ui_home.c）只在 standby 已显示时重置倒计时，从不检查子页是否打开。

**方案**：子页打开（`subpage_overlay` 非空）时重置倒计时并 return：

```c
if (subpage_overlay) { idle_countdown = g_idle_timeout; return; }
```

所有子 APP（Books/Music/Files/WiFi/…）共用 subpage_overlay，全部生效；看书不再被锁。

## 四、🔴 真相：UART0 与 TF 卡共用 PF2/PF4（ok-20260827-10）

**现象**：开发板 1500000 发 log（TTL-USB 验证 OK），波特率两边一致，接 R528 怎么接 rx 都是 0；回环 echo /dev/uart1 也 0。

**根因（两次反转）**：

1. 用户提示「UART0 可能不是真引脚」→ 查 gemini-s1 `sys_config.fex`：
   - `[card0_boot_para]`：`sdc_clk = port:PF2<2>`、`sdc_d3 = port:PF4<2>` —— **PF2/PF4 是 TF 卡 SDC0，mux2**
   - `[uart_para]`：`uart_debug_port = 2`、`tx=PC00<2>`、`rx=PC01<2>` —— **调试口是 UART2，不是 UART0**
   - 而 r528_serial.c gemini-s1 分支把 UART0 配在 GPIOF(2)/GPIOF(4) mux3 → **与 TF 卡冲突**，SD 卡初始化后引脚回到 SDC0，UART0 物理上收不到
   - **P118 误判**：当时用 velaevb1 的 sys_config（那边 PF2/PF4 才是 UART0）佐证 gemini-s1「引脚正确」——**跨板佐证是错的**，gemini-s1 board.h「J5=PB22/PB23」注释其实没错
2. 用户提议「UART2 是 debug，剩 UART1」→ 查 PIN out 确认 UART1 可用。

**方案（用户拍板：不动 TF 卡）**：
- defconfig 删 `CONFIG_R528_UART0=y`（编译后 `.config` 为 `# CONFIG_R528_UART0 is not set`，`r528_uart0config` 整个不编译，PF2/PF4 完全归 TF 卡）；删 `CONFIG_UART0_RXBUFSIZE=2048`
- defconfig 加 `CONFIG_R528_UART1=y`（注册 /dev/uart1，驱动已就绪）
- `dm_uart_dbg.c` 设备路径 `/dev/uart0` → `/dev/uart1`
- LD2410B 保持禁用（也是 UART0）；全链路残留检查：仅 `dm_ld2410b_init()` 注释 + 无其他 open /dev/uart0 路径
- 固件验证：`strings vela.bin | grep -c uart0` = 1（仅无害字符串），`.config` 确认 UART0 关/UART1 开

**验证**：distclean 全量重编（defconfig 变更纪律）→ pack → check_res 12/12 → ok-20260827-10。

## 五、🔴 再反转：UART1 真引脚 = PD21/PD22 mux4，不是 GPIOG6/7（ok-20260827-12~13）

**现象**：烧 UART1 版后 open /dev/uart1 ok，但 rx 仍 0B（每 5s `[uart_dbg] rx=0B ring=0` 稳定打印 = reader 线程正常，纯硬件没收到）。

**根因**：驱动 `r528_uart1config` 配的 GPIOG(6)/GPIOG(7) mux2 是 **PG 组复用，gemini-s1 板上并未引出**（与 UART0 同款误配）。用户提示 UART1 在 PD20/21 → 查官方《R528-S3PIN out-V2.xlsx》（/data/vela/webdoc/）：
- sheet2（gemini-s1 板级）：**PD21=UART1-TX、PD22=UART1-RX**，栏位注明「排针 需要加电源、地」
- sheet1（芯片复用表）表头 G 列=Function4 → PD21/PD22 的 UART1 = **mux4**
- 用户记的 PD20/21 差一位：PD20 实际是背光 PWM4（BOE BL_PWM），不能占用

**方案**：`r528_uart1config` 改 `UART1_TX=GPIOD(21)`、`UART1_RX=GPIOD(22)`、`UART1_GPIO_FUNCTION=(4)`；RTS/CTS 不用流控不配置。

**验证**：ok-20260827-13；接线指引：开发板 TX → PD22（UART1_RX）、GND 共地。

## 六、NSH 自测命令 uartdbg_test（ok-20260827-14）

**需求**：不接硬件就能验证「串口→UI」显示链路（用户选方案 B：NSH 命令）。

**实现**（线程安全注入）：
- `dm_uart_dbg.c`：抽 `uart_dbg_ring_push()` 入环（read/注入共用）；注入缓冲 `g_dbg_inject_buf/len/pending`——外部线程（命令）只写缓冲 + 置 pending，**接收线程在 poll 循环消费入环**，ring 单写者不破
- `dm_uart_dbg_inject_test()`：填测试日志（含中文 line1~3）→ 置 pending
- 新建 `apps/vendor/allwinnertech/apps/uartdbg_test/`（Kconfig + Makefile + main.c 调注入接口，CFLAGS 加 luncher_dm 头路径）；父 Kconfig source；defconfig `CONFIG_UARTDBG_TEST_APP=y`
- 顺带修 `deskmate_ui.c` 两个隐式声明警告（该文件**不 include deskmate_ui.h**，跨文件函数需显式前向声明——补 `ui_uart_dbg_create/close_cleanup`）

**用法**：先打开 Settings → UART 调试工具 → NSH 敲 `uartdbg_test` → 窗口显示测试日志 + 日志 `[uart_dbg] injected N bytes`。

## 改动文件表

| 文件 | 内容 |
|------|------|
| `apps/luncher_dm/dm_uart_dbg.c/h` | 接收层：环形缓冲/波特率表含 1500000/线程/注入接口/设备路径 uart1 |
| `apps/luncher_dm/ui/ui_uart_dbg.c` | 子页：FLOATING 工具栏+日志窗口/3 配色/rx 消费 timer |
| `apps/luncher_dm/deskmate_ui.c/h` | show_subpage dispatch + cleanup + 前向声明 |
| `apps/luncher_dm/ui/ui_settings.c` | 入口行 value 显示 UART1 + 回调 |
| `apps/luncher_dm/luncher_dm.c` | `dm_ld2410b_init()` 注释（LD2410B 让位） |
| `apps/luncher_dm/ui/ui_home.c` | idle_timer_cb：子页打开不锁屏 |
| `boards/.../nsh/defconfig` | 删 UART0 / 加 UART1 / 加 UARTDBG_TEST_APP |
| `chips/r528/r528_serial.c` | UART1 引脚 PD21/22 mux4（原 GPIOG6/7 mux2 误配） |
| `apps/vendor/.../uartdbg_test/`（新建） | NSH 自测命令 |

## 验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)   # ok-20260827-10 起走 distclean 一次
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
bash /data/vela/check_res.sh      # 提示音 12/12 + res 完整，全通过
bash /data/vela/git_snapshot.sh   # ok-20260827-6 ~ ok-20260827-14
```

镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（每次固化后重新打包）。

## 遗留事项（下会话）

1. 上板验证（未做）：烧 ok-20260827-14 → 开 UART 调试工具 → 敲 `uartdbg_test` 验证 UI 链路 → 短接 **PD21/PD22** 回环 `echo hello > /dev/uart1` 验证硬件
2. UART1 硬件链路未通待查：若 rx 仍 0，需确认烧录镜像版本（-13 才含 PD21/22 修正）+ 短接引脚编号（PD21↔PD22，非 GPIOG6/7、非 PD20/21）
3. 可选：r528_serial.c 加 TIOCMSET 支持 UART_MCR_LOOP 内部回环（软件自证控制器，不碰引脚）——已提议未实现
4. UART0 已彻底禁用：**PF2/PF4 归 TF 卡**，勿再改回（改回会撞 TF 卡）
5. 蓝牙 H4 110 挂起（boot 日志 open timeout）——已知遗留，勿擅改驱动

## 沉淀教训

- **跨板佐证是坑**：velaevb1 的 sys_config 不能佐证 gemini-s1 的引脚——每块板子引脚不同，必须查本板配置（gemini-s1 sys_config / 官方 PIN out xlsx）
- **「驱动配置引脚」≠「板上实际引出」**：UART0 配 PF2/4 实际是 TF 卡；UART1 配 GPIOG6/7 实际板上是 PD21/22——排障先确认「板上到底引出哪组」再信驱动
- **defconfig 变更必须 distclean 全量重编**（AGENTS.md §四坑 3）
- **deskmate_ui.c 不 include deskmate_ui.h**：跨文件函数要显式前向声明，改完必须看编译警告
- **环形缓冲单写者纪律**：外部线程注入只写 pending，接收线程消费，UI 侧零锁安全

*DevLog by AtomCode (deepseek-v4-flash)*
