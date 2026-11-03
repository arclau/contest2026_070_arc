# P286 — HX83102B 触摸驱动按 F101 实机结论重写

目标：R528 **ynm3000_v2（V2 板）** + BOE/HX83102B 模组，让 /dev/input0 出报点并驱动 LVGL。
平台：R528 板端。验收：刷机后出 `ID=00 2b 10 83`、触摸时 `INT 1 -> 0`、界面响应；
编译与打包通过不证明触摸可用。

> **2026-10-28 证据清理**：本文引用的 `output/p286-*` 目录（构建/打包日志、补丁、baseline 源码）
> 已随 V2 清理删除，明细见 `output/_deleted-20261028-034939.txt`；下文 `output/...` 路径仅作历史记录。

## 参考事实（F101-YuzukiNeko，同型号模组，2026-10-25 E3）

- 硬件：2SK3018 电平转换接反（G=3.3V、S=主控、D=触摸）时体二极管把触摸侧抬到
  2.3~2.5V，ID 与读 0 位全失败；正确为 G=1.8V、S=触摸、D=主控。用户本轮已改硬件。
- 协议：写 0x13=0x31、0x0d=0x10 开 AHB 桥；ID 在 0xd0000090（`00 2b 10 83`）；
  事件栈从 0x30 读 56 字节。
- 帧：56 字节和 ≡ 0 (mod 256)；点数在 byte 52 低半字节（0xFF=无点）；全 `00` 帧是
  校验失败，不是抬手。
- 时序：空闲时 26ms 间隔的第二帧即全 `00`，20ms 盲轮询随后出现 NACK 窗口。
- RST 与面板复位共用时不能拉低，否则出横纹（F101 #56）。

## 本项目驱动原来的问题

| 项 | 原实现 | 后果 |
|---|---|---|
| 点数偏移 | 40（坐标尾部 padding） | 永远读 0，总线通也不出点 |
| 帧长 | 54 | 读不到校验尾，无法判全 00 |
| 校验 | 无 | 坏帧被当成抬手 |
| 触发 | 16ms 盲轮询 | 踩中 IC 承受不了的读取节奏 |
| 失败 | 立即 pins_config + pin_reset 重试 | 单次丢包代价大，还反复碰 RST |
| 构建 | defconfig 未开 `CONFIG_HX83102_IIC_TOUCH` | 驱动根本没编进固件 |

## 本轮改动

- `hx83102_iic_touch.h`：帧长 56、点数偏移 52、I2C 400k→100k、INT 改为输入说明。
- `hx83102_iic_touch.c`：56 字节校验解码；INT(PB5，低有效) 门控 + 200ms 空闲读；
  失败按 `20ms << streak` 退避封顶 500ms；连续 5 次失败才重读 ID；200ms 无好帧
  释放触点；初始化 200ms 稳定后唤醒 + 读 ID（5 次重试），不再写 safe-mode/sense_on。
- `deskmate/defconfig`：`CONFIG_HX83102_IIC_TOUCH=y`（savedefconfig 回写后保留）。

## 构建与产物

目标板为 V2，`pack.sh`/`lunch_nuttx 5` 会把项目路径取成 `ynm3000_v2_nand`（缺 `r528s3/`
前缀），回落到 `board/common` 的 nor 配置后打包失败；改用显式路径：

```bash
cd /data/dm
./build.sh vendor/allwinnertech/boards/r528/ynm3000_v2/configs/deskmate -j"$(nproc)"
cd vendor/allwinnertech/lichee
bash tools/scripts/pack_img.sh -c sun8iw20p1 -p rtos -b r528s3-ynm3000-v2 -o nuttx \
  -d uart0 -s none -m normal -w none -v none -i none \
  -t /data/dm/vendor/allwinnertech/lichee -f r528s3/ynm3000_v2_nand -g r528s3/ynm3000_v2_nand
```

产物 `out/r528s3/ynm3000_v2_nand/rtos_nuttx_r528s3-ynm3000-v2_uart0_128Mnand.img`，
md5 `64fa6a461de48b19d38ae6541a9912a4`（53,342,208 B）。已人工核对：
`nsh.fex == nuttx/vela.bin`（md5 `056074511c2a15a342c1cffb2bac121a`）、res.fex（23,961,600 B）
与 nsh.fex 均在镜像内；ELF 含 `ID=%02x %02x %02x %02x`、`INT %d -> %d at`。
打包当时未刷；后续用户刷入并报告有条纹、触摸可用（见 E1）。

## 待验证

1. E2 串口应有 `P286-E2 RST preload-high` 与 `ID=00 2b 10 83`；触摸时出现 `INT 1 -> 0` 才说明
   INT 真的通到 PB5。若 INT 恒为 1，事件读会降到 200ms 一次（能用但迟滞）。
2. 落点：当前用 F101 的 90° 换算（x=1439-y、y=x），方向未实机核对；点四角确认。
3. 拖动/多指/抬起未验收。V2 defconfig 未开 CONFIG_HIDMOUSE_TSCIF，无 /dev/input0 冲突
   （gemini-s1 开着，若在那边用需注意）。
4. 若 `ID read failed ret=` 连续 5 次：先查 2SK3018 接法与 1.8V 电平，再查 I2C 上拉。

## E1：RST 引脚完全不接管（2026-10-25，已板测）

O：用户刷入上面的 `64fa6a46…` 包后报告有条纹、触摸可用；本次没有板端串口或刷入身份日志。
隔壁 F101 项目的 `ad3cec3b…` 镜像已由用户确认触摸可点按且无条纹；其无复位配置
`CONFIG_INPUT_HIMAX_RESET_ON_INIT=n` 使驱动从不配置或驱动 RST 脚（见 F101 devlog 56、59、60）。

H：R528 驱动虽设 `HX83102_RESET_PULSE=0`，但 `hx83102_pins_config()` 仍把 PB4 切成输出，
`hx83102_pin_reset()` 又写高；切换方向时可能产生短暂电平变化，并与条纹相关。尚无波形或
板端 A/B 证明，不能把它写成根因。R528 面板软件复位脚标为 PD19，PB4 是否在实际载板上
影响屏幕仍需实测。

E1 唯一行为变量：`HX83102_RESET_PULSE=0` 时完全不配置、不写 PB4。保持 70 ms + 200 ms
启动等待、I²C 100 kHz、INT 门控、退避、报点协议及显示代码不变；只更换版本日志。
预测：若 PB4 输出切换是相关变量，新包应保留触摸并消除条纹；若仍有条纹，该预测不成立，
下一步核对运行身份与显示分层，不继续叠加 RST 补丁。

| 产物 | SHA-256 | MD5 | 状态 |
|---|---|---|---|
| 旧包：PB4 输出高（镜像已清理） | `ab1f82f2fe3cb08009fa0017b244f6312d481894ef5b120f2b2f5b3354c34dab` | `64fa6a461de48b19d38ae6541a9912a4` | 用户报告有条纹、触摸可用 |
| E1：PB4 不接管（镜像已清理） | `c3240d1fa30cee2c564c95baa055400a0615a985a9f1d158708b152c631cc2ad` | `591212888c8a2c03f3410c9deb191001` | 用户报告显示正常、无触摸 |

两包各 53,342,208 B。V2 配置编译退出 0，`hx83102_iic_touch.c` 已重编，ELF/`vela.bin`
包含 `P286-E1 RST drive=0`；`编译日志`（`output/p286-touch-stripes/P286-E1-build.log`）。
首次 Dragon 在沙箱因 `Bad system call` 失败，脚本仍误报 `pack finish`；经授权在宿主重跑，
`Dragon execute image.cfg SUCCESS !`，见 `打包日志`（`output/p286-touch-stripes/P286-E1-pack.log`）。
人工核对 `nsh.fex == nuttx/vela.bin`（MD5 `c4cd4c53f50c813ed2ebe42de9db7ed3`），
完整 `nsh.fex`、`res.fex` 均在新镜像中；V2 不使用仅覆盖 gemini-s1 的 `check_res.sh` 判据。
`checkpatch.sh` 仍因原有厂商文件格式失败（基线 90 项 error，E1 88 项），未做无关格式化。
`sync_r528.sh --check` 遇到既存 `luncher_dm` 源/镜像差异并中止；直接同步会删除镜像中的
`CMakeLists.txt` 和 `assets/`，故未运行源码镜像同步，也未覆盖这些已有文件。

C：E1 的显示改善与触摸失效同时出现，支持 PB4 操作与两种现象相关；没有引脚波形或
有效芯片 ID，不能认定是短暂低脉冲或 RST 电平造成。E1 未同时满足两个验收条件；镜像保留，
当前源码已进入 E2，未标修复或稳点。

2026-10-25 串口补证（`logs/serial/20261025-131938_p286-e2-touch.log`，文件标签不代表板上版本）：
板端明确打印 `P286-E1 RST drive=0`；面板完成初始化后，pre-touch 扫描 `0x48`、`0x71` 有
ACK。驱动打印 `RST untouched, INT=0`，随后 5 次 `ID read failed ret=1`；post-touch 扫描
0 个设备，`/dev/input0` 未注册，LVGL 打印 `cannot open touchscreen device`。启动后再用
`i2c dev -b 0 -f 100000 -z` 分别只探测 `0x48`、`0x71`，均无 ACK。证据表明失败发生在
触摸初始化/I²C 通路，不能只归咎于 UI；尚不能区分复位电平、线路状态和总线恢复问题。

## E2：先预装 RST 高电平，再切 PB4 为输出（2026-10-25，已板测）

H：旧包在设置 PB4 输出后才写高，方向切换到写高之间可能短暂输出低电平；E1 不驱动 PB4
则可能让触摸 IC 一直处于复位。两者均为待验证解释，尚无 PB4 电平波形。

E2 相对旧包的唯一行为变量是 PB4 的写入顺序：先向 DATA 寄存器写高，再将 PB4 切为输出；
不产生主动低脉冲。70 ms + 200 ms 等待、I²C、INT 门控、退避、触摸协议和显示代码不变。
若短低脉冲假设成立，应同时无条纹且触摸可用；若只恢复触摸却重现条纹，需把持续驱动高
或其它触摸活动列为候选，不能继续称短低脉冲为已证根因。

产物：P286-E2-rst-preload-high.img（镜像已清理），
53,342,208 B，SHA-256 `8ef7522141aee33a44ef428e4b7f25433e1fcf476c6b3605d77685e0c305ecc4`，
MD5 `95199517913db76eaa7b8f209af455f7`。V2 编译退出 0，固件含
`P286-E2 RST preload-high`，`CONFIG_HX83102_IIC_TOUCH=y`、`CONFIG_LUNCHER_DM_V2_APP=y`；
`编译日志`（`output/p286-touch-stripes/P286-E2-build.log`）。宿主 Dragon 打包成功，
`打包日志`（`output/p286-touch-stripes/P286-E2-pack.log`）含 `Dragon execute image.cfg SUCCESS !`。
人工核对 `nsh.fex == vela.bin`（MD5 `051bf8970aeeb460f5bf2ad4511d0a5b`），完整 nsh/res
均在镜像中；V2 仍以人工检查代替只覆盖 gemini-s1 的 `check_res.sh`。
`checkpatch.sh` 有 88 项厂商文件格式 error（旧包基线 90），未做无关格式化。

C：用户已刷 E2，报告**可以触摸，但屏幕有横纹**。串口 BOOT #6 出现 E2 标记，
`ID=00 2b 10 83`、`init ok`，LVGL 成功打开 `/dev/input0`。因此“预装高电平可同时保留
触摸并消除横纹”的预测被否定；不能把切换方向时的短低脉冲认定为横纹根因。
E2 运行时 `INT -1 -> 0` 后长时间低电平，工作线程几乎每秒打印
`re-identify after 5 errors`；多数 ID 仍可读，也有读失败/错误 ID。用户能点按并不代表
事件链稳定。当前源码保留 E2 对照，不标修复。源码镜像同步仍因既存 `luncher_dm`
镜像差异暂缓，以免覆盖用户已有文件。

## E3：串口现场切 PB4 为输入上拉（2026-10-25，无刷机）

O：E2 运行时 PB4 寄存器读值：MUX `0x02000030=0x5f0144f3`（PB4 输出、PB5 输入），
DAT `0x02000040=0x00000010`（PB4 高），PULL `0x02000054=0x00050450`（PB4 无上拉、
PB5 上拉）。F101 无横纹配置 `CONFIG_INPUT_HIMAX_RESET_ON_INIT=n`，完全不配置/驱动其
触摸 RST（PD3）；R528 E1 照搬这一点后触摸失联。两载板复位线状态不能直接等同。

H：PB4 需要释放触摸复位，但强驱高可能关联横纹；输入上拉或许能兼顾。没有电压/波形，
这只是待验假设。

E3 唯一行为变量：通过 NSH `mw` 先把 PB4 PULL 改为上拉 (`0x00050550`)，再把 MUX
改为输入 (`0x5f0044f3`)，不改显示/触摸代码、不刷机。DAT 仍读到高；串口在约
100 秒内只有 1 次正确 ID，另有约 27 次错误 ID、20 次 ID 读失败和 87 次重识别，
说明输入上拉没有维持可靠触摸。用户目视横纹结果尚待回报，不能据此判定显示影响。
之后把 MUX 和 PULL 依次恢复原值；恢复输出高后 ID 又能持续读到，PB5 曾打印
`INT 0 -> 1`、`1 -> 0`，但重识别仍频繁。恢复是运行态寄存器操作，重启也会清除。

C：输入上拉至少不够稳定；PB4 强驱输出与触摸可用相关。横纹的电气或启动时序机理
仍未知。下一步先收用户对 E3 的目视结果，再区分复位线持续驱动与启动时序；不要把
F101 的无复位配置直接作为 R528 成品配置。

面板代码来源核对：F101 的 `boe-hx83102b-720x1440-ref.overlay` 注明 11 条初始化命令
取自本 R528 项目的 `BOE_1440x720.c`；当前 R528 文件又注称采用 F101 的 11 条命令。
这是同源往返，不是两份独立的面板驱动互相验证。P284 已指出这 11 条含 Novatek 页命令，
对 HX83102B 的显示机理仍按 P285 未闭环处理；本轮未改变显示驱动。

## E4：无触点强制 200ms 慢读 + 原始帧 dump（2026-10-25，已刷；初始化失败）

O（E2 运行态串口直读，`logs/serial/20261025-131938_p286-e2-touch.log`）：
`i2c dump -b 0 -a 0x48 -r 0x30 N`（N=1/2/4/8/16/56）全部返回 `0x00`；同总线
`0x13=0x31`、`0x0d=0x10`、`0x00=0xd0`、`0x08=0x00`，AHB ID 仍为 `00 2b 10 83`。
即总线与 AHB 桥正常，但事件栈持续是全 0 坏帧（F101 判为 IC 未驱动 SDA），不是合法空闲帧
`ff*55+37`。运行中 INT 恒 0，驱动每 20ms 读一次，约每秒 `re-identify after 5 errors`。

H：INT 恒低使旧门控失效，20ms 读节奏落在 F101 已证明的“读太快→全 00”窗口。

E4 唯一行为变量：无触点（`last_state != 0`）时无论 INT 电平都强制 `IDLE_MS=200` 才读；
仅已跟踪触点保持 20ms。PB4 仍预装高后切输出，I²C 100k、协议、重试/退避、显示代码不变。
诊断附加：dump 前 3 个被拒帧和首个合法帧（有界，不改变控制流）；版本日志
`P286-E4 slow-read RST preload-high`。

产物：P286-E4-slow-read.img（镜像已清理），
53,342,208 B，SHA-256 `2840e777d72b139aacc4f417a942d00d2aa39068c08fd8a8108ccb8340ad1f31`，
MD5 `5af917f8610907d199bf6ddbee522f2e`。V2 编译 EXIT=0，`CC hx83102_iic_touch.c`；
宿主 `pack_img.sh` EXIT=0，`Dragon execute image.cfg SUCCESS !`；
人工核对 `nsh.fex == nuttx/vela.bin`（MD5 `9d91a6a83fbedeb1d5001c5fe9a8c0be`），
镜像内含 `P286-E4 slow-read` 与 `first valid frame:`。
日志：`编译`（`output/p286-touch-stripes/P286-E4-build.log`）、`打包`（`output/p286-touch-stripes/P286-E4-pack.log`）。

验收（需刷机 + 串口）：先出现 `first valid frame:` 且为 `ff … ff 37` 型合法空闲帧，
触摸时 `INT` 能翻转、点按可响应；横纹是否随读流量下降而变化需目视对照。
若仍持续全 00，则慢读不足以让 IC 恢复，下一步转复位/电平路径（PB4 与 INT），
不再继续调读节奏。未刷机、未标修复。

刷入结果（2026-10-25 14:05，boot #9）：**未进入慢读路径**。`pre-touch ACK 0x48` 正常，
版本串确认 `P286-E4 slow-read`；随后 `RST held high, INT=0` → `ID read failed ret=1` ×5
→ init 中止，`post-touch scan 0 device(s)`，`/dev/input0` 未创建（LVGL 报 errno=2）。
即 PB4 置高后 IC 立刻 NACK；E2 同段代码同一天 13:26 的启动却 `ID=00 2b 10 83` 成功。
这是 init 阶段的 ID 不稳（与读节奏无关），E2 的成功不具备可重复性。
尝试软重启复测时板子停在 u-boot `usb update probe`/`SUNXI_USB_SEND_DATA=32` 等待
（USB 烧录口仍被主机占用），需断电重启后才能继续。**E4 假设尚未被检验。**

## E5：关闭 PB5 内部上拉（2026-10-25，已刷；横纹未变）

O：用户确认 RST、INT 未做电平转换，触摸侧各由 1.8 V 上拉；R528 驱动仍将
PB4 配成输出高，并给 PB5 输入开启 SoC 内部上拉。R528 数据手册将 PB4/PB5
归于 3.3 V VCC-IO 域；普通 GPIO 的 VIH 最低为 0.7×VCC-IO。
E2 的 PB5 长时间读低与此电平不匹配相符，但尚无两端电压/波形，不能定为根因。

H：PB5 内部上拉参与了 INT 电平异常或显示横纹。E5 相对 E4 的唯一行为变量是
`hx83102_pins_config()` 把 PB5 的 pull 从 `GPIO_PULL_UP` 改为
`GPIO_PULL_DOWN_DISABLED`（该枚举值 0 表示上下拉均关闭）。外部 1.8 V 上拉、
PB5 输入方向、PB4 输出高、I²C、慢读和显示配置保持原样；启动日志改为
`P286-E5 PB5 internal pull disabled` 以核实运行身份。

预测：若 PB5 内部上拉是横纹必要因素，刷入 E5 后横纹应消失；若 PB5 仍读低，
不能据此否定 INT 线路，因为 1.8 V 低于 3.3 V GPIO 的保证高电平门槛。
若横纹仍在，只否定这一单变量预测，不排除 PB4、电源或面板启动时序。

产物：P286-E5-pb5-no-internal-pull.img（镜像已清理），
53,342,208 B，SHA-256 `c2ca851eb5bf90d5a34674a94d276136355943c12c5ea55926cd2ebad7ac7215`。
V2 编译 EXIT=0，日志确认 `CC hx83102_iic_touch.c`，有效配置启用 HX83102/V2 应用；
ELF 和整包均含 E5 标记。沙箱 Dragon 遇 `Bad system call`，脚本仍打印 `pack finish`，
该次旧整包不用；宿主重跑退出 0，出现 `Dragon execute image.cfg SUCCESS !`。
`nsh.fex` 与 `nuttx/vela.bin` 字节一致（SHA-256
`7722a44d388aebf4f8df6576f83d6df3fe7a0ee80680818e346251137cb45184`），
完整 nsh/res 均在新镜像内。V2 的 `check_res.sh` 不覆盖此目标，按流程人工核对。
证据：`编译`（`output/p286-touch-stripes/P286-E5-build.log`）、
`宿主打包`（`output/p286-touch-stripes/P286-E5-pack-host.log`）。

刷前验收计划：核实 E5 启动标记、`RST held high, INT=`、ID、`/dev/input0`、触摸响应及
横纹目视；测 PB4/RST、PB5/INT、1.8 V 轨。E5 **未改变 PB4 推挽输出高**，
不能作为 1.8 V RST 接线的长期安全配置。失败后保留 E4/E5 镜像作对照，
不因单次失败认定显示或触摸硬件损坏。

刷入结果（2026-10-25 23:54，完整启动 BOOT #3；
`logs/serial/20261025-235312_p286-e5-pb5-nopull.log`）：串口确认
`P286-E5 PB5 internal pull disabled`，用户目视**横纹没有变化**。面板初始化完成；
触摸前扫描 `0x48`、`0x71` 均 ACK。随后 `RST held high, INT=0`，
五次 `ID read failed ret=1`，初始化中止，LVGL 无法打开 `/dev/input0`（errno=2）。
触摸后扫描两地址仍 ACK，但地址应答不能证明完整 AHB ID 事务成功；
与 E4 的触摸后零 ACK 不同，不能在不同次上电间把差异归因于 PB5 pull。

C：E5 版本身份与横纹反馈相配，**关闭 PB5 内部上拉不足以消除横纹**；
PB5 仍读低，触摸未注册，慢读路径仍未运行。截至此次串口采集，尚无 PB4/RST、
PB5/INT 与 1.8 V 电源轨的实测电压/波形，根因未知。保留 E5 镜像作对照；下一步只核实这几根线
在驱动配置前后的实际电压与连线，不继续叠加读节奏或显示补丁。

## 2026-10-26 电压补证（用户现场读数，未改固件）

O：用户报告，在 TP RST 外接 1.8 V 上拉的情况下，**屏幕端 TP RST 仍测得 2.88 V**；
INT 测得 **1.7 V**。这是用户现场测量，尚无测量仪表、测点照片、时序波形或
PB4/PB5 两端同步读数。当前 E5 代码关闭 PB5 内部上拉，但 PB4 仍预装高后配置为
推挽输出，代码未给 PB4 配内部上拉；故不能把 RST 的 2.88 V 解释成“内部上拉”。
串口已确认 E5 的软件读数 `INT=0`、ID 连续失败，用户报告横纹仍在。

解释边界：RST 的 2.88 V 高于触摸侧 1.8 V 上拉，和 PB4 主动驱动高的代码方向一致；
没有 PB4 实测与连线图，尚不能证明 2.88 V 唯一由 PB4 提供，也不能认定它是横纹根因。
INT 的 1.7 V 说明线上已接近触摸侧上拉电压，但 R528 3.3 V GPIO 的保证高电平门槛
约为 2.3 V；软件仍读低与此相容，不能把 `INT=0` 直接解释为触摸 IC 持续拉低。

下一步仅核对同次上电下 PB4 引脚与屏幕端 TP RST、PB5 引脚与屏幕端 INT、
触摸 1.8 V 轨的实际连接/电压；若两端一致，再按 3.3 V↔1.8 V 接口设计处理；
若不一致，先查线束/转接板。测量结果回来前不再叠加固件补丁。

## E6：PB4 显式高阻，隔离 3.3 V 输出（2026-10-26，已板测）

O：用户测得屏幕端 TP RST=2.88 V（外接 1.8 V 上拉），而 E5 源码将 PB4
配置为推挽输出高。E1 曾完全不接管 PB4，显示无横纹但触摸失联；E1 未核对
屏幕端 RST 电压。E6 用新测量证据检验 PB4 是否为高电压来源。

唯一行为变量：相对 E5，`hx83102_pins_config()` 将 PB4 从输出高改成
`GPIO_MUXSEL_IN` + 关闭内部上下拉 + 输入方向，由屏幕端已有 1.8 V 上拉释放
RST；不产生复位低脉冲。PB5 仍无内部上拉，I²C、200 ms 空闲读、显示和启动
等待不变。仅增加身份/状态日志 `P286-E6 PB4 high-Z PB5 no-pull`、
`RST released (PB4 high-Z)`；触摸能否恢复不是本实验的先验结论。

预测：在 E6 启动标记出现后，于同一测点测量；若此前由 PB4 主动输出供电，
TP RST 应从 2.88 V
回到接近 1.8 V。若仍接近 2.88 V，先查其他灌入路径/连线，不继续改轮询。
电压回落也不证明横纹机理或触摸功能已修复；另记录横纹、ID、`/dev/input0`。
E6 只控制驱动初始化后的 PB4；上电及 bootloader 阶段电平尚未核对，
永久方案仍需确认两端接线及正确的 3.3 V→1.8 V 接口。

产物：P286-E6-pb4-high-z.img（镜像已清理），
53,342,208 B，SHA-256 `8e477c13c8fed28a5c2dd4c6e6384509a556c75202f4b125a41490157f9fbfb0`。
V2 编译退出 0，`hx83102_iic_touch.c` 重编，有效配置启用 HX83102/V2 应用；
ELF 与整包含 E6 身份。宿主 Dragon 出现 `Dragon execute image.cfg SUCCESS !`；
`nsh.fex == nuttx/vela.bin`（SHA-256
`c915dd0b481ded62bfc61b5d9b7421b8cf702794e682e17ac0c4d59d69b2d923`），
完整 nsh/res 均嵌入镜像。V2 按流程人工核对，不用 gemini-s1 的 `check_res.sh`。
证据：`编译`（`output/p286-touch-stripes/P286-E6-build.log`）、
`宿主打包`（`output/p286-touch-stripes/P286-E6-pack-host.log`）。
当前源码停在 E6 实验状态，不标完整修复。

刷入结果（2026-10-26 00:15，`logs/serial/20261026-001449_p286-e6-rst-highz.log`，
BOOT #2）：串口出现 `P286-E6 PB4 high-Z PB5 no-pull`，确认运行身份。
面板初始化完成；触摸前 `0x48` 有 ACK（共 1 个设备）。驱动打印
`RST released (PB4 high-Z), INT=0`，随即 5 次 `ID read failed ret=1`；
触摸后扫描 0 个设备，LVGL 打开 `/dev/input0` 失败（errno=2）。因此 E6
未进入慢读/报点路径。用户随后报告：屏幕显示正常、横纹消失，RST 在 GPIO
端与屏幕端均约 1.7 V，触摸仍无响应。这是用户现场稳态测量与目视结果；
上电/复位瞬态、触摸供电和 I²C 波形未测。

C：E5→E6 仅把 PB4 从推挽高改为高阻；RST 电压回落与横纹消失同时出现，
支持 PB4 高电平驱动与横纹有关，但尚未直接捕获灌入电流或排除其他瞬态。
E6 没有恢复触摸初始化。`INT=0` 是 3.3 V GPIO 对约 1.7 V 线的采样，
且 ID 读取发生在 INT 门控前，不能用 INT 读低解释这次 ID 失败。
`hx83102_i2c_read_cmd/write_cmd()` 将底层错误统一变为 `ERROR_TRANSFER=1`，
`hx83102_burst_enable()` 又忽略返回值；现有日志不能区分写地址、写方向、
读数据或先行 burst 设置哪一步失败。保留 E6 电气状态作对照。
下一步只增加分段 I²C 错误观测，保持 PB4 高阻及其他行为不变：若首个失败
发生在更改 PB2/PB3 复用后，先核对引脚复用/总线；若事务成功却读到错误 ID，
再核对 RST 释放时序和芯片状态。决定加三极管前需确认接口方向与两侧电压。

## E7：只记录底层 I²C 失败位置（2026-10-26，已板测）

O：E6 的 `ID read failed ret=1` 来自统一的 `ERROR_TRANSFER`，丢失底层
`I2C_TRANSFER` 返回值；触摸前扫描 `0x48` ACK，触摸初始化后扫描 0 设备。
尚不能判断是哪笔事务先失败，故不能判定必须修改 RST 硬件。

唯一变量：在 `hx83102_i2c_read_cmd/write_cmd()` 的传输失败分支打印读写命令、
长度、底层返回值；身份标记改为 `P286-E7 I2C transfer trace`。PB4 高阻、
PB5 无内部上拉、I²C 时钟、初始化/复位等待、重试次数与显示代码保持 E6 状态。
日志仅在传输失败时增加，可能轻微改变失败后的重试间隔，板测须留意。

预测：若 `0x48` ACK 但首个 AHB 命令传输失败，日志会给出命令和底层返回值；
若 AHB 事务均成功但 ID 不符，则会打印 ID 字节。前者再区分引脚复用、
总线时序与芯片状态；后者查复位释放和协议。此次不主动拉高 PB4。

产物：P286-E7-i2c-trace.img（镜像已清理），
53,342,208 B，SHA-256 `806c23c1aefcb174afa8f5d60e46067da6883eb29dae3696b66b9837cb313946`。
V2 构建退出 0，`hx83102_iic_touch.c` 重编；有效配置含
`CONFIG_HX83102_IIC_TOUCH=y`、`CONFIG_LUNCHER_DM_V2_APP=y`，`vela.bin` 含 E7
标记和新日志。宿主打包日志有 `Dragon execute image.cfg SUCCESS !`；
`nsh.fex == nuttx/vela.bin`（SHA-256
`dc92b19817239d27cd68f4836720afd2db49d38042100923142e42ea0af58748`），
打包日志含 nsh/res。证据：`构建`（`output/p286-touch-stripes/P286-E7-build.log`）、
`打包`（`output/p286-touch-stripes/P286-E7-pack-host.log`）。首次沙箱打包的
Dragon 报 `Bad system call`，重跑后上述打包成功；不能把首次 `pack finish`
作为成功证据。诊断日志暂留。

板测（`logs/serial/20261026-002905_p286-e7-check.log`，BOOT #5）：
`P286-E7` 标记确认运行身份。相同 TWI0 总线在触摸初始化前扫描到
`0x48`、`0x71`；驱动配置引脚、释放 PB4 为高阻后，第一笔 `0x08`
读取 `I2C_TRANSFER=-1`，后续 `0x13`、`0x0d`、`0x00` 写入也均为 `-1`；
5 次 ID 读取失败，初始化后扫描 0 设备，无 `/dev/input0`。
用户报告此时显示仍正常、触摸不工作。此日志定位到传输失败，不能单独
区分 PB2/PB3 重设复用、RST 释放状态或总线电气状态；未测 I²C 波形。

E8 曾准备单变量跳过驱动中的 PB2/PB3 重复复用：V2 编译退出 0、宿主
Dragon 打包成功，**未刷板**。用户决定改做 RST 硬件电平转换，E8 实验取消；
仅撤回 E8 源码改动，当前驱动源码回到 E7。默认生成目录的 E8 镜像/ELF
仍可能存在，不能当作当前源码或交付镜像；稳定保存的 E7 镜像见上文。

当前方向：用户对 RST GPIO 增加电平转换。硬件完成后先核对主控侧与
屏端 RST 的上电/稳态电压、极性及是否共享面板复位，再按实际转换电路
调整 PB4 控制并板测。当前 E7 固件 PB4 始终高阻，不主动发复位脉冲，
不能仅凭焊上转换器就预期它会主动复位触摸。INT 仍约 1.7 V，软件读低；
但此次 ID 失败发生在 INT 参与事件处理之前。根因仍未确定。

## E8：RST 经 2SK3018 电平变换后恢复 PB4 输出驱动（2026-10-26，已板测；横纹消除、触摸仍失败）

O：用户给 RST 加 2SK3018 电平变换：板端 3.3 V/4.7 kΩ 上拉、屏端 1.8 V/10 kΩ
上拉（按 F101 正确接法 G=1.8 V 轨、S=屏端、D=板端）。用户实测稳态电压：
板端 RST 3.3 V、屏端 1.7 V，变换器工作符合设计。E6 证明 PB4 高阻可消横纹但
触摸初始化后 I²C 全失败；E7 只定位到传输失败、不能区分引脚复用/总线/芯片
状态。电气隔离改由硬件承担后，检验恢复 PB4 输出驱动能否两者兼得。

唯一行为变量：相对 E7，hx83102_pins_config() 把 PB4 从高阻输入改为 E2 式
预装——先写 DATA 高再切 GPIO_MUXSEL_OUT+输出（方向切换不出低脉冲）；
hx83102_pin_reset() 恢复写高、仍不发低脉冲（RST 可能与面板复位共用，
F101 #56）。PB5 无内部上拉、I²C 100 kHz、200 ms 空闲读、退避、I²C trace、
显示代码全部保持 E7。身份串 P286-E8 RST normal drive PB5 no-pull I2C trace，
状态日志 RST held high (PB4 output)。

板测（logs/serial/20261026-013758_p286-e8-flash.log，boot #2，01:38:37）：
身份串确认运行 E8；寄存器直读确认引脚状态符合预期——0x02000030=0x5f0144f3
（PB4 输出）、0x02000040=0x00000010（PB4 高）、0x02000054=0x00050050
（PB4/PB5 无内部上拉，PB2/PB3 上拉正常）。用户目视横纹消失，与 E6 高阻时
一致；板端 3.3 V/屏端 1.7 V 与变换器设计相符。但触摸仍失败：第一笔
i2c read cmd=0x08 transfer=-1，随后 0x13/0x0d/0x00 写全 -1，5 次 ID 失败，
post-touch scan 0 device(s)，/dev/input0 未创建（LVGL errno=2）——与 E7
失败签名逐字一致。

复核原始串口与 `apps/system/i2c/i2c_dev.c`（2026-10-26）：上述地址 ACK/
数据阶段判断有误。E8 boot #2 的触摸前后扫描均为 **0 device(s)**；约两分钟后
`i2c dev -b 0 -f 100000 -z 48 48` 的 0x48 栏显示 `--`，源码中 `--` 代表
传输失败，显示 `48` 才代表成功。`i2c dump -b 0 -a 0x48 -r 0x30 56` 也报
`Transfer failed`。`r528_transfer()` 将 HAL 失败统一转为 `-1`，现有日志无法
区分地址 NACK、数据 NACK、总线忙或超时，更不能据此排除引脚配置。

C：RST 电平变换后 PB4 恢复输出且横纹不重现，屏端 RST 稳态约 1.7 V；触摸
初始化仍因 I²C 传输失败而未注册。E8 启动时连地址探测也没有成功，失败层级
应记为“总线事务未完成”，根因未知。PB2/PB3 的 TWI 内部上拉仍开启，疑有
3.3 V/1.8 V 电平不匹配；尚无两端实测电压或波形，不能定为根因。下一步在
同次上电测 PB2/PB3 与屏端 SDA/SCL 的空闲电压和探测时波形，同时核对 1.8 V
供电、RST；若电平或波形异常先处理接口，若均正常再查 TWI 状态码/时序。

## 2026-10-26 接线状态补充（用户报告，尚无新板测）

用户明确：当前只有 INT 未做电平转换，RST 与 SDA/SCL 已做电平转换。
SDA/SCL 转换完成时间与 E8 串口对应关系尚未确认；E8 的“0x48 探测无应答”
只能代表该次启动，不能直接代表现接线的运行结果。此前将 PB2/PB3 仍未转换
当作当前状态的推断撤回。INT 在屏端约 1.7 V、PB5 属于 3.3 V 域，软件可能
读低；但 ID 探测发生在 INT 报点门控之前，INT 未转换不能单独解释 ID 事务
失败。下一次只用现接线复测 `0x48` 探测与驱动 ID 日志；若仍失败，再同步测
SDA/SCL 两端电平和波形，区分转换器接线、供电/复位与 TWI 控制器状态。

## 现接线冷启动复测（2026-10-26，串口 BOOT #5）

O：用户报告当前 RST、SDA、SCL 均已转换，仅 INT 未转换；沿用 E8 固件，
串口采集器持续监听，在 `P286 current wiring reboot` 标记后记录到 BOOT #5
（`logs/serial/20261026-013758_p286-e8-flash.log`，01:54:59 起）。此记录不
证明电平转换器的实际波形或完成时间，也未改变固件。启动前扫描 `0x48` 与
`0x71` 均 ACK；驱动初次 ID 为 `00 3f 10 83`、`00 2f 10 83`，第三次为正确
`00 2b 10 83`，打印 `init ok (native 720 x 1440)`。启动后 `0x48` 仍 ACK，
LVGL 成功打开 `/dev/input0`。这证实本次启动通过注册关口。

运行阶段：PB5 的软件读值 `INT=0`；`0x30` 事件帧读取频繁 `transfer=-1`，
有一次 `first valid frame`（串口与其他输出交织，原始行不完整），也出现全零
拒收帧；反复 `re-identify after 5 errors`，后续 ID 仍有正确值与错误值。用户
点屏是否响应尚待反馈，不能用 `/dev/input0` 已创建证明触摸功能通过。

C：现接线下 I²C 并非全程失联，但字节读取与持续传输不稳定；仅 INT 未转换
不能单独解释 ID 错位和 `0x30` 传输失败。下一步在保持接线/固件不变时，
同步观察主控与屏端 SDA/SCL 的 100 kHz 波形及两侧空闲电压，对照正确与错误
ID 的 ACK、上升沿和读数据位；若波形正常，再看 TWI 控制器错误状态与时序。
监听器保持运行，用户触摸响应结果待补。

产物：P286-E8-rst-normal.img（output/p286-touch-stripes/），53,342,208 B，
SHA-256 df33f8f5aa7d61ce2ea3e5330fb31481a20f1ad101fd287c87893f4bcedcee0c，
MD5 011b7a52d35f0ebab58840df20b09c13。V2 编译 EXIT=0，CC hx83102_iic_touch.c；
沙箱 Dragon 直接成功（Dragon execute image.cfg SUCCESS !）；
nsh.fex == nuttx/vela.bin（SHA-256 de7ef1f6…890），res.fex 23,961,600 B；
vela.bin 与整包含 P286-E8 身份串。证据：P286-E8-build.log、P286-E8-pack.log、
板测串口 20261026-013758_p286-e8-flash.log（采集常驻）。
编号说明：原“跳过 PB2/PB3 重复复用”E8 实验已取消未刷，其打包日志改名
P286-E8-canceled-mux-pack-host.log；默认生成目录可能残留的旧 E8 镜像/ELF
与本实验无关。

## E9：per-message 频率从未生效——总线实际跑 400 kHz（2026-10-26，已构建打包，未刷板）

O（源码审计，未改固件前）：`hal_twi_init()` 把 `twi->freq` 硬编码为
`TWI_FREQUENCY_400K`；`drv_twi.c r528_transfer()` 只拷贝 addr/flags/len/buf、
丢弃 `msgs[].frequency`，HAL 也没有运行时设频 API。即触摸驱动自 P286 起
请求的 100 kHz 从未到达线上，TWI0 一直跑 400 kHz。F101 早已证明 400k 在
此模组链路上不可用（devlog 45/58），其驱动内置 100k→50k→25k 阶梯、
并建议事件读降到 50k。BOOT #5 的 ID 单位错误（0x2b→0x3f/0x2f、0x10→0x18、
0x83→0xc3）、56 字节读几乎全败、偶发合法空闲帧，与“400k 跑边际电平转换
总线”的签名一致；上节“观察 100 kHz 波形”的计划建立在总线并非 100k 的
事实误差上，本实验先纠正速率再谈波形。

H：总线实际速率 400 kHz 是 BOOT #5 字节级不稳的根因候选；降到驱动一直
请求的 100 kHz 应显著减少位错误与传输失败。

唯一行为变量：`hal_twi.c` 新增 `hal_twi_set_frequency()`（按 init 同一
`twi_set_clock` 路径重设分频器）；`r528_transfer()` 在 `hal_twi_xfer` 前按
`msgs[0].frequency` 调频。触摸（TWI0）由此 400k→100k；传感器（TWI2）
msg 本就请求 400k，行为不变；TWI1 无用户。触摸协议、等待、退避、引脚
配置（PB4 预装高输出/PB5 无内部上拉）、显示代码零改动；仅版本串改为
`P286-E9 RST drive PB5 no-pull TWI msg-freq` 并增加一行实际频率日志。

预测：刷入后 ID 首次探测即 `00 2b 10 83`（不再出现 0x3f/0x2f 类位错误）；
`0x30` 读成功率上升，`first valid frame:` 后持续出合法帧；用户点按有响应。
若 100k 仍有位错误/读败：下一轮只改 `HX83102_I2C_FREQUENCY` 为 50000
（调频基础设施已就位，不必再动 HAL）；若 100k 干净且延迟可接受则维持。

产物：P286-E9-msgfreq.img（镜像已清理），
53,342,208 B，SHA-256
`15d400320b94e814e122c7e709cbe31f19ca140822e3da7b267a0a635477eeee`。
V2 构建 EXIT=0，`CC hal_twi.c`、`CC drv_twi.c`、`CC hx83102_iic_touch.c`；
宿主 Dragon `Dragon execute image.cfg SUCCESS !`；
`nsh.fex == nuttx/vela.bin`（MD5 `e46d830be65497400b1eb170a30fb083`），
ELF 含 `hal_twi_set_frequency`，整包含 E9 身份串与 `I2C bus frequency`。
证据：`构建`（`output/p286-touch-stripes/P286-E9-msgfreq-build.log`）、
`打包`（`output/p286-touch-stripes/P286-E9-msgfreq-pack.log`）。

编号备注（补记 2026-10-26 02:11 未归档尝试）：本 session 接手前，源码树在
02:04–02:11 有一次未写入 devlog 的 E9 尝试——把 `hal_twi_init()` 的
`twi->freq` 全局改成 100 kHz（全部 TWI 端口），产出
`P286-E9-clock100k-unflashed.img`（未刷），02:11:25 回退重构
（`P286-E9-revert-build.log`），源码恢复 400K 基线。本 E9 改按
per-message 频率实现：同为“触摸降到 100k”，但传感器保持其请求的 400k，
单一变量更纯，且后续试 50k 只改触摸常量。两次产物均未刷、都未标修复。

验收（需刷机 + 串口）：出现 E9 标记与 `I2C bus frequency: 100000 Hz`；
ID 一次正确；`first valid frame:`；点按有响应；横纹目视对照。
未刷机、未标修复。

## E10：F101 pacing——拆启动锤击 + 真实延时 + 启动延时（2026-10-26，已构建打包，未刷板）

O（E9 刷机复核，串口 logs/serial/20261026-013758_p286-e8-flash.log）：E9 身份串与
`I2C bus frequency: 100000 Hz` 在 02:51/03:02/03:37 三次启动确认，100k 确已上线，但：
- 02:51：pre-touch 扫描 0 设备（总线已死）→ init 5 轮×6 笔全 transfer=-1（142ms
  内跑完，轮间无真实间隔）→ INIT FAIL；post-touch 0 设备。
- 03:02：pre-touch 扫描 ACK 0x48+0x71（2 设备，扫描时总线还活）→ init 同样 30 笔
  全败 → INIT FAIL；post-touch 0 设备——init 背靠背锤击把总线打死。
- 03:37：pre-touch 2 设备 → 0x13 写报败但 ID 首探即 `00 2b 10 83` 正确 → init ok
  → 13ms 后 first valid frame（55×ff+0x37 合法空闲帧）；随后 0x30 读 3298 次全败、
  4 次 re-identify，至 03:44 未恢复。
- E9 窗口合计 ID 读 29 正确 / 33 单位错误（2b→3f/2f/3b、10→18、83→c3）：
  100k 未消除位错误。

对照（人工 i2c 工具，笔间隔以秒计）：03:24-26 统计 100k 读 5/5 通、400k 读 0/5 败；
03:35 `dump -r 30 56` 得 55×ff+0x37（校验和≡0 mod 256）合法空闲帧，30s 后第二次全
00（IC 休眠）；03:39 窗口关闭 9 笔全败。即有真实间隔时 100k 读可靠。

平台缺陷实证：`up_udelay(200000)` 4 次启动实测均 ≤1ms（CONFIG_BOARD_LOOPSPERMSEC
忙等在本板 CPU 频率下快约 200 倍）→ 驱动 200ms settle、2ms/20ms 重试间隔全是空转；
首笔事件读可在 init 后 ~20ms 发生（last_poll_ms 初值 0），正落 F101 gap test 危险区。

H：根因是节奏不是速率。F101 同型号模组实证 IC 扛不住连续快速传输（200ms 间隔两读
成功、<26ms 连续轮询全 0 随后 NACK 窗口）；其解法 INT 门控 + IDLE_MS=1000 + 退避 +
START_DELAY + 温和探测。本板 INT(PB5) 未转电平永远读 0，INT 门控不可用，取其余
pacing 手段。

唯一行为变量（纯软件；接线/100k/协议/显示不动）：
1. r528_boot.c 启动扫描：112 地址×2 笔背靠背全扫描（224 笔）→ 只探 0x48 两笔；
   探测前 usleep 300ms settle、笔间 usleep 100ms；日志改 `probe 0x48: %d/2 ok`。
2. 驱动延时全换 usleep（真实 tick 延时，realtek 驱动先例）：新增 delay_ms/us()；
   RESET_SETTLE 200000us→200ms、READ_RETRY 2000us→2ms、新增 PROBE_GAP_MS=50；
   pin_reset 删无效 up_udelay(70000)。
3. hx83102_register 播种 last_poll_ms：首笔事件读 ≥200ms 后发生。
4. 版本串 `P286-E10 F101 pacing: gentle probe, real delays, 100k`。

预测：pre-touch 探到 0x48 且不拖死总线；init 前两轮拿到正确 ID；first valid frame
后 0x30 读在 200ms 节奏下持续成功；点按有响应。若仍败，下一轮只改
HX83102_I2C_FREQUENCY=50000（E9 调频基础设施在位）。

产物：P286-E10-pacing.img（镜像已清理），
53,342,208 B，SHA-256
`3ac5ae4cc303ff3da1886e8d5abb534a7a208901dc48c8918fbf558252272a00`（注释修正后重建一次，
哈希以此为准）。V2 构建 EXIT=0
（CC r528_boot.c、CC hx83102_iic_touch.c）；宿主 Dragon SUCCESS；
nsh.fex == nuttx/vela.bin（SHA-256 0b4af98d…），res.fex 23M 在镜像内。
证据：`构建`（`output/p286-touch-stripes/P286-E10-pacing-build.log`）、
`打包`（`output/p286-touch-stripes/P286-E10-pacing-pack.log`）。
未刷机、未标修复。

## E10 刷机结果：阴性——启动即总线全死（2026-10-26 03:54，用户已刷入并确认点屏无响应）

O（串口 20261026-013758_p286-e8-flash.log，03:54:57 起）：E10 身份串
`P286-E10 F101 pacing: gentle probe, real delays, 100k` 确认上线。
- pre-touch probe 0x48: **0/2 ok**（零长写 + 1 字节读两笔均败；探测前已留
  300ms settle、笔间 100ms 真实间隔——本启动此前零 I2C 活动，第一笔即败）；
- init 5 轮 × 6 笔全 transfer=-1（轮间 50ms 真实间隔首次在板上落地，日志
  时间戳 .717/.768/.819/.870/.921 可证），5× ID read failed → INIT FAIL
  （03:54:58.123，init 全程 657ms）；
- post-touch probe 0x48: 0/2 ok；无 `/dev/input0`；用户确认点屏无响应。

C：E10 的 pacing 改造全部生效，但启动时刻总线本就处于死窗。死窗来源：
上一会话 03:39 手动 9 笔全败后窗口关闭，至 03:54 启动已 15 分钟未自愈；
且 IC 卡死状态**跨重启保持**（RST 全程拉高从未脉冲，重启不重置 IC）。
03:37 最佳启动（ID 首探即对 + 合法空闲帧）与 03:54 全死的差别是启动瞬间
IC 所处状态，不是本版代码差异。

新机会状态：本次 init 失败 → 无 worker 轮询、无 re-identify 风暴，总线自
03:54:58 起完全安静。下一步单一区分实验（尚未做）：控制台单笔地址探测
`i2c dev -b 0 -f 100000 -z 48 48`，间隔 60s 再一次——ACK 则证实“IC 需
分钟级安静恢复”，E11 = 耐心启动探测（F101 START_DELAY 1500ms + 按秒级
间隔重试 probe/init，或把首次识别推迟到 worker）；仍 NACK 则安静无效，
转硬件核查（SDA/SCL 两侧空闲电压、2SK3018 极性）或评估 RST 脉冲（与面板
复位是否共用未验证，F101 #56）。

未决假设（按优先级）：① IC 恢复需分钟级安静，启动 300ms settle 远不够；
② IC 深睡/卡死需断电或 RST 脉冲清除，重启无效；③ INT(PB5) 未转电平
（永读 0）与 2SK3018 极性未核——静态接线故障基本排除（历史上有正确 ID、
合法 56 字节帧、100k 人工 5/5 读通），边际状态不能排除。

## E10 后安静探测：88 分钟真安静仍 NACK（2026-10-26 05:23，串口 052159 会话）

O（`i2c dev -b 0 -f 100000 -z 48 48`，扫描表 `--`=NACK/`%02x`=ACK，
语义见 apps/system/i2c/i2c_dev.c）：E10 启动失败后总线真安静 88 分钟
（03:54:58 起驱动死、无轮询；注意 03:54 重启前那 15 分钟并不安静——旧 E9
固件的 worker 还在每 ~500ms 锤），单笔零长写探 0x48 仍 **NACK**。

C：“IC 需分钟级安静恢复”假设**否定**（88 分钟未愈）。结合 03:37→03:54 的
跨重启保持，IC 进入的状态：掉电级以下事件才能清除（重启无效、长时间安静
无效）。下一实验（用户侧物理动作）：**整机断电重上电**（非 reboot）——
IC 状态不应扛过掉电。若上电后总线 ACK，则 E10 的 pacing 假设首次获得在
健康 IC 上的测试机会（此前每次启动都生在死窗里）；若仍 NACK，转硬件故障
排查（IC 或 2SK3018 转换通路损坏，用户侧量 SDA/SCL 两侧空闲电压）。

源码新事实（为 RST 脉冲恢复实验排除障碍）：BOE_1440x720.c 面板复位用
disp2 LCD GPIO0（PD19），与触摸 RST(PB4) 是本板上两根独立引脚；“RST 与
面板复用”仅为自 F101 #56 继承的未验证警告，本板不成立（PCB 级粘连不能
从源码排除，实验时盯显示即可）。

## E10 断电验证：启动链全过，运行链仍死（2026-10-26 05:28，断电重上电后）

O（串口 052159 会话，05:28:15 启动；掉电确认真实断电——`resetcause=power_on(0)`）：

启动链 **E10 全部预设现象成立**：
- `pre-touch probe 0x48: 1/2 ok`——温和探测探到 IC 且未楔死总线（E9 时代
  224 笔全扫描后 init 全死的模式消失）；
- init 探测轮间隔 50ms 真实落地（时间戳 .954/.008/.116/.137），第 4 轮拿到
  正确 `ID=00 2b 10 83` → init ok；
- `first valid frame`（55×ff+0x37 合法空闲帧）出现在 init ok 后 380ms——
  E10 的首读延 200ms 生效；
- `/dev/input0` 创建成功，LVGL `touchscreen /dev/input0 open success`。

运行链 **仍败**：
- first valid frame 后 **28ms** 的第一笔重读即 -1（正落 F101 26ms 危险区：
  worker 的 3 连重试间隔仅 2ms，本身就是小锤击）；
- 之后 ~200-400ms 节奏（IDLE 门控生效）的 0x30 读 100 秒内 **442 次全败**，
  `re-identify after 5 errors` **39 次**（每次 6 笔背靠背写入，1ms 内跑完）；
- 偶发成功读每 ~12-19s 才出现一次（05:28:19.9/38.4/50.3、05:29:09.4），
  IC 大部时间死、短暂窗口才应答；点屏预计仍无响应（帧没流起来）。

C：E10 修复了“启动锤击+假延时”问题（健康 IC 上启动链全过），但运行链
节奏相对 F101 实测配方仍过激：F101 IDLE_MS=1000（我们 200）、失败只退避
不立即重试（我们 3 连重试）、无 re-identify 风暴（我们每 5 错锤 6 笔）。
另 IC 需 ~15s 级安静才偶发应答，提示可能还有更深层劣化（待 E11 数据）。

E11 提案（待用户确认）：① IDLE_MS 200→1000；② 读败即退避、删 3 连重试；
③ re-identify 改为长间隔（如 30-60s）或仅在 init 做；④ 仍不稳则事件读降
50k（F101 对边际链路的建议）；⑤ 启动 probe 持续失败时 RST 脉冲恢复
（面板复位走 PD19 与 PB4 独立，脉冲安全性已排除共用障碍）。
（E11 已按 ①②③ 实施，见下节；④⑤ 留 E12。）

## E11 实施：F101 运行节奏（2026-10-26 05:35，已构建打包，未刷机）

按 E10 段末提案的 ①②③ 项实施（④事件读降 50k、⑤RST 脉冲恢复留待 E12），
单变量原则：只动运行链节奏，不动启动链（E10 已验证启动链在健康 IC 上全过）。

代码（hx83102_iic_touch.c，版本串 `P286-E11 F101 pacing: idle 1s, single
read, long backoff`）：
- IDLE_MS 200→1000（F101 实测值；E10 的 200ms 下 IC 仍在若干读后进 NACK 窗）；
- worker 删 3 连重试改**单次读**——E10 首重读在 28ms 即败，正落 F101 26ms
  危险区，重试比等待更糟；连带删除 `HX83102_READ_RETRIES/READ_RETRY_MS`
  宏与 worker 的 `int i;`；
- 错误退避指数上限 4→8、封顶 500ms→`HX83102_BACKOFF_MAX_MS 5000`（E10 实测
  IC 需 ~12-19s 安静才偶发应答，500ms 封顶等于仍在 ~2 读/s 锤）；
- re-identify 加 30s 间隔（新增 `last_reident_ms` 字段 +
  `HX83102_REIDENTIFY_MS 30000`，register 时播种）——E10 的 39 次
  re-identify 每次 6 笔背靠背写入，是自持风暴；败后 delay 同样封顶 5s。

产物：`out/…/rtos_nuttx_r528s3-ynm3000-v2_uart0_128Mnand.img`
（53,342,208 B，SHA-256 `9375418b57e43d780b7c0f19bb28df38a810a74df02284585d1d6a29fd9c19c6`），
已归档 P286-E11-idle1s.img（镜像已清理）；
已人工核对 nsh.fex == nuttx/vela.bin（SHA-256 `4c2d3a15…`），res.fex 23M 在镜像内。
证据：`构建`（`output/p286-touch-stripes/P286-E11-idle1s-build.log`）、
`打包`（`output/p286-touch-stripes/P286-E11-idle1s-pack.log`）
（`boot0 checksum fail` 为 E8/E9 起就有的良性行，Dragon SUCCESS 照常）。
驱动编译 `CC hx83102_iic_touch.c` 见首轮构建输出；增量重编无新单元。

**刷机前置条件（必须）**：IC 卡死状态跨重启保持（E10 已证），刷 E11 前先
**整机断电重上电**（非 reboot），否则 E11 会在死窗里假失败。上电后先控制台
`i2c dev -b 0 -f 100000 -z 48 48` 确认 ACK 再评估刷机。

验收现象：启动串口应出 `P286-E11 F101 pacing: idle 1s, single read, long
backoff`；空闲时 0x30 读间隔 ~1s；一旦读败，退避按 40/80/160ms… 增至 5s
封顶、re-identify 最多 30s 一次；若链路稳住，应见持续合法空闲帧
（55×ff+0x37）与触摸时 `INT 1 -> 0`、/dev/input0 出报点。

## E11 首刷结果：reboot 假失败，未获公平测试（2026-10-26 05:39，用户已刷）

O（串口 052159 会话 05:39:21 起）：
- `resetcause=cpu_soft_reset(reboot)`——刷机触发的是 **reboot 不是掉电**；
- 05:28-05:38 E10 运行链 10 分钟 4985 次 0x30 读 + 399 次 re-identify，
  又把 IC 锤进死窗（INT 全程无变化，恒 0，未转电平的老现象）；
- E11 启动：pre-touch probe 0/2、init 5 轮 30 笔全 transfer=-1、INIT FAIL、
  `/dev/input0` 打不开（errno=2）——与 E10 03:54 死窗启动失败模式一致。

C：E11 的节奏改动**没有机会运行**（init 失败 → 无 worker → 无轮询）。
失败原因是流程违规：IC 卡死跨重启保持（E10 已证），刷机 reboot 清不了，
E11 生在死窗里。这不是 E11 配方的阴性结果。

下一步（用户侧物理动作）：**整机断电重上电**（拔电源数秒）。上电后 E11
自动启动即首次公平测试：预期启动链 probe 1/2、ID 正确、init ok、
/dev/input0 出现；运行链看空闲读 ~1s 间隔、读败退避增至 5s 封顶、
re-identify ≤30s 一次、持续合法空闲帧。上电后可先控制台
`i2c dev -b 0 -f 100000 -z 48 48` 确认 ACK。
（注：该次上电后 init 实际全败，控制台 A/B 翻转结论——见 E12 节。）

## E12：总线降 50k——console A/B 实证，IC 一直好的（2026-10-26 05:45-05:53，已构建打包，未刷机）

O（串口 052159 会话，控制台直输，只读探测）：
- 05:42:26 掉电重上电（`resetcause=power_on(0)`）后 E11 启动：pre-touch probe
  1/2、init 5 轮 30 笔全 -1、INIT FAIL、`/dev/input0` errno=2。用户判断
  “不如 400k 时代偶尔能动”，并问 RST 是否还内部上拉；
- 05:43-05:44 四次 100k 探测全 NACK；
- **05:45 同窗口 A/B：400k NACK、100k NACK、50k ACK；50k 复探 5/5**；
- 05:47 `i2c dump -b 0 -f 50000 -a 48 -r 30 56` 直读事件栈拿到**合法空闲帧
  55×ff+0x37**——IC 完全健康；
- 05:48 连续 8 次 dump（~1s 间隔）后转全 00 帧，随后 50k 探测也 NACK——
  连续读在 50k 下依旧会把它打回 NACK 窗（F101 模式与频率无关）。

C：**推翻“IC 卡死跨重启保持”判断**——IC 自始至终是好的，是链路在
≥100k 边际失效。03:39 A/B（100k 5/5 vs 400k 0/5）时链路还够用，之后天花板
降到 100k 以下；F101 devlog 59 的 100k→50k→25k 阶梯与“边际链路降 50k”
建议对本板命案。RST 不用动：50k 能应答即 RST 已拉住（PB4 内部上下拉实为
关闭，空闲电平靠输出高+模组侧上拉，代码见 pins_config）。用户“400k 偶尔
能动、100k 全死”的观察方向正确——唯一受控 A/B（03:39）曾支持 100k，但
链路劣化后结论反转，本次 A/B 为证。

代码（单变量：总线速率；E11 pacing 不动）：
- `hx83102_iic_touch.h`：`HX83102_I2C_FREQUENCY` 100000→50000，注释记录
  A/B 证据与“E10 pacing-not-speed 结论作废”沿革；
- `r528_boot.c`：pre/post-touch probe 两笔同步 100000→50000——否则探测
  日志在 50k 好链路下也报 NACK，重蹈“死模组”误判；
- 版本串 `P286-E12 F101 pacing: idle 1s, single read, 50k bus`。

产物：`out/…/rtos_nuttx_r528s3-ynm3000-v2_uart0_128Mnand.img`
（53,342,208 B，SHA-256 `cf5b009813cfa6e78b67336c3cfa29e0ffa3db6e28d23c9b6245520cfe0fad92`），
归档 P286-E12-50k.img（镜像已清理）；
nsh.fex == nuttx/vela.bin（`e75a086a…`）。证据：
`构建`（`output/p286-touch-stripes/P286-E12-50k-build.log`）、
`打包`（`output/p286-touch-stripes/P286-E12-50k-pack.log`）。

**刷机前置**：IC/TWI 现处 NACK 窗（05:48 连续 dump 打入），**刷前整机断电
重上电**。预期：50k 下 probe 2/2、ID 正确、init ok、/dev/input0 出现；
运行链看 1s 单次读 pacing 能否维持合法帧流（05:48 证明仍不能连续读）。
若 50k 下 init 仍败，下一阶 25k（F101 阶梯第三级）。
（注：首刷未断电，假失败；真掉电结果见下节。）

## E12 首刷：又是 reboot 假失败（2026-10-26 05:55，用户已刷）

O（串口 052159 会话）：E12 身份串与 `I2C bus frequency: 50000 Hz` 确认上线，
但 `resetcause=cpu_soft_reset(reboot)`——**刷前仍未断电**。日志证据：05:52:33
至 05:54:48 板子持续正常输出（LVGL 帧更新），05:54:53 `board_reset:status:0`
软重启，随后 BOOT #10-13 为刷机工具连刷；全程无掉电间隙。E12 启动：pre-touch
probe 0/2、init 5 轮 30 笔全 -1、INIT FAIL。用户 05:57:55 自行控制台探测
50k 亦 NACK。IC 自 05:48 被我连续 dump 打入 NACK 窗后未恢复，软重启不清。

C：与 E11 首刷同一假失败模式——**刷机工具的软重启不等于断电**，IC 的 wedge
状态跨软重启保持。E12 的 50k 至此仍未在健康 IC 上测过。

## E12 真掉电首测：probe 2/2 证实 IC 健康，init 突发又把它打晕（2026-10-26 06:00-06:02）

O（串口 052159 会话；用户整机断电重上电，`resetcause=power_on(0)`，BOOT0
06:00:18 冷启动）：
- 06:00:31.703 `pre-touch probe 0x48: 2/2 ok`（50k）——**史上首次 probe 满贯，
  IC 干净复活且在 50k 完全应答**；
- 200ms settle 后 06:00:31.908 init 第 1 轮：`read cmd=0x08 len=4`、write
  0x13、write 0x0d 三笔连续 -1；5 轮 30 笔全 -1 → 06:00:32.365 INIT FAIL；
- 06:00:32.565 post-touch probe **0/2**——init 突发又把 IC 打回 NACK 窗；
- 06:01:21/31/41、06:02:24 控制台 50k 探测 4 次全 NACK，wedge 持续 ≥2min。

C：IC health 不再是问题（2/2 满贯 + 05:45 合法帧两证）；**新瓶颈是 init 本身
的突发节奏**：pre-touch probe 2 笔温和事务 IC 全应答，但 200ms 后 init 每轮
6 笔背靠背（轮间仅 50ms）一笔都不成，且败后 post-probe 也 0/2——突发把 IC
打晕。对比 05:28（E10/100k）同款 init 第 4 轮还拿到正确 ID：IC 对突发的耐受
随时间劣化（或被 50k 下单笔更长的事务拖出窗口）。恢复节律参考：05:42 wedge
后静默 ~2-6min，05:45 起 50k 自行恢复应答（窗口会自行重开）。

## 历史交接（2026-10-26 06:03；由文末 E13 交接替代）

- 固件：E12 已刷（总线 50k + E11 pacing：idle 1s/单次读/退避 5s/re-identify
  30s），镜像 `output/p286-touch-stripes/P286-E12-50k.img`
  （SHA-256 `cf5b0098…`）；boot probe 已同步 50k。
- IC 状态：06:00:32 起 wedged（50k 探测 NACK），init 失败故无 worker，总线安静。
- **判任何果之前必须整机断电重上电**（resetcause=power_on(0) 为准；刷机工具的
  board_reset 软重启无效，已被 05:39/05:55 两次假失败证明）。

E13 候选（按优先级，单变量渐进）：
1. **init 改温和耐心**（首选）：删/缩 probe 与 init 间的 200ms settle（06:00
   实证 IC 在 .703 应答、.908 已沉默——窗口等不起）；init 事务间加 100ms+
   真实间隔，不再 6 笔背靠背；轮间退避 1s 起；探测轮数加至 ~10、轮距
   500ms-1s（F101 START_DELAY/PROBE 精神按本板窗口调整）。
2. **RST 脉冲施救**：F101 对 wedge IC 的标准解法；源码已证 PB4 与面板复位
   PD19 独立（E8 起经 2SK3018 驱高），脉冲低 10ms 再高后重 init；若出横纹
   即证 PCB 级共用（F101 #56），立即停。
3. **init 败也注册、worker 周期重试**：利用“窗口自行重开”（05:42→05:45），
   让 re-identify（30s 间隔、单笔）在后台捕恢复窗，而不是启动失败即放弃。
4. 兜底：仍不稳则事件读降 25k（F101 阶梯第三级）。

方法论备忘：本 IC 只耐受温和稀疏访问（probe 2 笔成、6 笔突发卒）；所有
“IC 卡死/坏死”结论在下结论前必须先用 50k 温和探测排除频率与节奏因素
（本次已因此推翻一次错误根因）。

## E13：初始化错误处理修复与驱动清理（2026-10-26，已打包，板测待回报）

目标/平台：R528 ynm3000_v2 + HX83102AB；先使初始化事务可检查、错误可定位，
最终验收仍为正确 ID、持续合法帧及实际触摸响应。用户要求修驱动、调试并清理前序混乱代码。
本节是当前交接；前文实验按历史保留，以下纠正优先于旧推断。

### 复盘后的证据边界

- E12 的 pre-probe 2/2 是零长度写及无命令字的单字节读，不能等同 AHB/带命令的组合读成功，
  也不能证明 IC 全面健康。初始化第一笔 0x08 读取已失败，不能据此认定后续突发造成失联。
- 同窗口降速改善和 ID 位错误支持排查总线电气/控制器时序；尚无波形，不认定唯一硬件根因。
  `transfer=-1` 统一错误也不能一概写成地址 NACK。
- 本机 F101 参考已找到：`/data/F101-YuzukiNeko/sdk/zephyr/drivers/input/input_himax_hx83102.c`
  和 `drivers/i2c/i2c_sunxi.c`（后一条相对该 Zephyr 目录）。触摸初始化本身连续执行多笔事务，
  没有每笔 100ms 等待；因此不采用旧交接的“删 settle/加间隔/加重试/降25k”组合猜测。
- 曾检查 R528 `twi_restart()` 的两次控制写。START 属自清零命令位，不能仅凭第二次写入0
  宣称它被取消；没有据此改底层 START/STOP 时序，也未做建立在该假设上的模拟测试。
- 用户补充：F101 的 RST 有/无电平转换都曾可触摸。该事实提高 R528 平台路径的排查优先级，
  但不能单凭它区分 R528 电气接口与 TWI/Himax 驱动。

### 已实施及唯一行为变量

功能变量是 **AHB 初始化的事务顺序和失败处理**；50k、200ms settle、5次探测/50ms轮距、
空闲1s/触摸20ms、退避5s及30s重识别均保留。RST保持当前转换后释放高，不发低脉冲。

1. `hx83102_interface_on()`：唤醒读0x08 → 写0x13=0x31 → 写0x0d=0x10 → 读回两配置；
   任一步失败立即返回，读回不符返回 `-EPROTO`。
2. 仅桥验证成功后发送 ID 地址、读方向、ID 读取；初始化和重识别共用这条路径。
   去掉原初始化中不检查返回值的独立唤醒读及通用 AHB helper 的无条件收尾写。
3. 传输/初始化/注册改传负 errno，保留传输错误；不把失败注册伪装成设备可用。
4. 清理从未启用的 SRAM固件下载、sense on/off及其通用AHB辅助代码和宏；V2 Makefile
   不再编入未使用的 `hx83102_firmware.c`。原固件文件保留。Kconfig说明改为模组常驻固件。
5. `hal_twi.c` 增加有界诊断：TWI0前16次失败，ISR只保存STOP前状态，线程持原总线锁打印。
   格式 `[TWI0-E13] fail=... source=irq-before-stop/thread addr=... msgs=... code=...`
   `msg=... byte=... st=... ctl=... lcr=... clk=...`；不修改传输状态机或吞错误。
   日志可能改变失败后的间隔，板测结果应保留这一边界。

基线：vendor HEAD `4c122d896b8ca786ad3f9a6768f37eeecdfe71e8`，已有大量未提交改动未清除。
修改前副本、配置指纹见 `baseline.json`（`output/p286-e13-bridge/baseline.json`） 及同目录
`*.before`；本轮相对进入时的差异为 `E13-only.patch`（`output/p286-e13-bridge/E13-only.patch`）。
部分触摸文件未被 vendor Git 跟踪，不能只用 `git diff` 代替这些备份。

### 验证与产物

- 故障注入运行真实源码中的初始化函数：`test_bridge.py`（`output/p286-e13-bridge/test_bridge.py`）。
  旧版在首笔唤醒失败后仍完成8笔事务并可误报 init ok，回归退出1；新版本12场景退出0：
  8个传输失败点立即停止、2个配置读回错误拒收、错误ID拒收、合法顺序完成。
  ASan/UBSan宿主验证通过；首次沙箱 LeakSanitizer 不能运行，已宿主重跑，未关闭检查器。
  证据：`旧版`（`output/p286-e13-bridge/test-before.log`）、
  `新版`（`output/p286-e13-bridge/test-after.log`）。模型只证明软件错误处理，不证明电气或触摸恢复。
- V2 build退出0，首次编译新增诊断格式告警已修正，最终增量无新告警：
  `build.log`（`output/p286-e13-bridge/build.log`）、
  `build-final.log`（`output/p286-e13-bridge/build-final.log`）。defconfig与本轮基线字节一致。
- 沙箱Dragon `Bad system call`，脚本仍退出0并打印pack finish，该次不算成功；
  `宿主打包`（`output/p286-e13-bridge/pack-host.log`）退出0且有 `Dragon execute image.cfg SUCCESS !`。
- 原 `check_res.sh` 固定 gemini-s1；本轮复制到证据目录，仅替换V2产物路径，
  `check-res-v2.sh`（`output/p286-e13-bridge/check-res-v2.sh`）全部原检查通过：
  nsh与vela相同、整包含完整nsh/res、资源字体/Wi-Fi固件/23提示音及新鲜度均通过。
  `校验日志`（`output/p286-e13-bridge/check-res-v2.log`）。
- `checkpatch -f` 已执行，两份厂商格式文件仍有风格报错，未宣称checkpatch通过；本轮相关
  tracked文件 `git diff --check` 通过。源码镜像检查遇既有 luncher_dm 目标独有文件/差异，
  未运行带 `--delete` 的全量同步，避免覆盖已有改动；`镜像检查`（`output/p286-e13-bridge/mirror-check.log`）。

镜像：P286-E13-checked-bridge.img（镜像已清理），
53,342,208字节；MD5 `d1dc470a72fc2ac83e0b8e6dd71391bc`；SHA-256
`58c29933157119479461a2f264775b0140c86be90e5ab9b6a7ed54c67c60a2ed`。
ELF与配置SHA-256见 `artifact.json`（`output/p286-e13-bridge/artifact.json`）。
整包和ELF均核实包含 `P286-E13 checked bridge + TWI failure trace, 50k` 和 `[TWI0-E13]`。

### 停工交接与下一步

用户要求先更新devlog下线，计划自行刷当前新包，约5小时后继续。**未收到刷入完成/触摸结果，
不标修复。**本轮未刷机、未改接线；串口采集器链接在线，但06:21发只读 `mw 0250200c 18`
无回显，不能由此判断板上电源状态。采集文件仍是 `logs/serial/20261026-052159_p286-probe.log`。

下次唯一动作：读取用户刷E13并整机断电重上电后的这一段串口，核对版本串及 `resetcause`，
提取 **首个** `[TWI0-E13]` 与对应触摸命令；不要先改参数。

- 若 init成功：核实合法帧、实际按下/拖动/释放及错误是否持续增长；仅ID成功不算通过。
- 若 init失败：`st=20/30/48`分别为写地址/写数据/读地址未获ACK；`38`仲裁丢失，
  `00`总线错误，结合msg/byte与CTL/LCR区分失效阶段。`source=thread`不是STOP前IRQ快照，
  要结合原始code判断超时/START/总线空闲失败，不能一律称NACK。
- `lcr` bit5=SCL、bit4=SDA，只是捕获瞬间逻辑电平，不代替上升沿、电源与转换器波形。
- 未闭环事项：INT电平及可靠门控、当前1s空闲延迟、销毁路径/工作队列生命周期与触摸坐标
  验收仍需后续检查，本轮不宣称驱动已完成全面产品化。

会话日志：当前Codex session ID未确认，原始会话导出**缺失**；没有拿串口日志或手写交接
冒充会话原始日志。未commit/push/tag。

## E14：TWI0 线路恢复 + RST 兜底（2026-10-26 06:59 打包；07:06 冷启动阴性）

目标/平台：R528 V2 + HX83102AB。变量：每笔 TWI0 传输前检查 SDA/SCL 实际电平，
必要时 9 clock + STOP + 引擎复位；init 失败后追加一次 10ms RST 脉冲重试。
50k、200ms settle、5 次探测/50ms、运行节奏均保留。
`E14-only.patch`（`output/p286-e14-recovery/E14-only.patch`）

结果：07:06:46 `resetcause=power_on(0)` 冷启动仍 INIT FAIL，日志出现
`ID read failed` 与 0x48 地址 NACK；`[TWI0-E14] recover=1..6` 多数
`lcr 2a→3a` 成功释放，一次 `still stuck lcr=2a st=f9`；RST 脉冲后的第二轮
仍失败；LVGL `cannot open /dev/input0 (errno=2)`。构建/打包/资源检查见
`artifact.json`（`output/p286-e14-recovery/artifact.json`）、
`pack.log`（`output/p286-e14-recovery/pack.log`）、
`check-res-v2.log`（`output/p286-e14-recovery/check-res-v2.log`）。
结论边界：恢复能解开总线，但不解决 init 失败；不据此判定 RST 脉冲对 IC 无效。
E13 冷启动数据点未取得（07:06 已运行 E14），不能补记。

## E15：写 NACK 容错与读回判据（2026-10-26，已打包，板测待回报）

目标/平台：R528 V2 + HX83102AB；验收仍是正确 ID、合法帧、实际按下/拖动/释放。

控制台证据（E14 运行中，15:11–15:16 板时）：RST 脉冲后 0x48 ACK；`get -r 08`
返回 0x00；`set -w 16 3113`（`[0x13,0x31]`）报 BUS_ERR、总线卡住，随后
`get -r 13` 读回 0x31；`set -w 16 100d` 成功且 `get -r 0d` 读回 0x10；逐寄存器
写 AHB 地址/方向后 `dump ... -r 08 4` 返回 `00 2b 10 83`。边界：控制台事务
间隔约 0.7–1.2s，只证明“写报错不等于写被拒”，不证明驱动背靠背节奏成立。

唯一行为变量：`interface_on()` 唤醒读仍须成功；0x13/0x0d 的写结果只打印，
以读回为准；AHB 地址/方向写错误不中止，以 4 字节 ID 为最终判据。50k、settle、
探测轮数/轮距、运行节奏、退避、30s 重识别和 E14 RST 兜底不变；5 字节 AHB
突发不变（若 ID 仍失败，E16 单独改逐寄存器写）。
`E15-only.patch`（`output/p286-e15-tolerance/E15-only.patch`）

主机模型覆盖 8 个传输失败点 ×（已应用/未应用）、读回损坏、坏 ID、完整 init；
ASan/UBSan 退出 0：`test_bridge_e15.py`（`output/p286-e15-tolerance/test_bridge_e15.py`）、
`test-e15.log`（`output/p286-e15-tolerance/test-e15.log`）。模型不证明电气/触摸。

V2 构建退出 0、0 warning；pack `Dragon execute image.cfg SUCCESS`、退出 0；
V2 产物检查通过（nsh==vela `842f29a7`，res.fex `502446c8`）：
`build.log`（`output/p286-e15-tolerance/build.log`）、
`pack.log`（`output/p286-e15-tolerance/pack.log`）、
`check-res-v2.log`（`output/p286-e15-tolerance/check-res-v2.log`）。

镜像 P286-E15-tolerance.img（镜像已清理），
53,342,208B，MD5 `a30c1bd54566424d9e2083f30853ba7b`，SHA-256
`d4f6f73d12f9ba638587267274b67869c49fd9ef0d184e4cd6cc6256bf37a49e`；
ELF SHA-256 `84f658bce0962acd27dff0e70a5169d278dc1e3f48ce4f2374ea79ea6a312148`；
镜像内含 E15 版本串与日志串，见 `artifact.json`（`output/p286-e15-tolerance/artifact.json`）。

### 板测回报（07:23，软复位，非冷启动）

07:23:46 E15 启动；首轮 `bridge 0x13 readback=0x39 expected=0x31`（单比特错），
E15 正确拒收；连续 5 轮失败后触发 E14 RST 脉冲，07:23:48 再探测得到
`ID=00 2b 10 83`、`init ok`，LVGL 打开 `/dev/input0`（maxpoint 10）。
07:24:13 首帧 `02 22 03 04 ...` 被接受，Settings 子页由 `app_icon_click_cb` 打开：
至少一帧真实触点已送达 UI。但随后 0x30 56B 读大量 `transfer=-1`，
`[TWI0-E13] ... st=00 lcr=2a byte=13` 显示读在中途失败并压住 SDA；E14 反复
recover；07:24:18 起 re-identify 读到 `0x0d=0x18`（期望 0x10）并以 -EPROTO
失败，07:25:57 又读到 `0x13=0x39`。本次两次读回都只多出 bit3（0x08）：
`0x31|0x08`、`0x10|0x08`；但 E9 窗口曾出现 `2b→3f/2f/3b`、`83→c3` 等多位错误，
不能把 bit3 当固定故障位。只能说明读数据位采样不可靠，无法据此区分 SCL 采样点、
SDA 电平/上拉与转换器波形。至 07:28 日志末 0x30 读仍持续失败，未再出现触摸帧。
启动原因为 `cpu_soft_reset(reboot)`，不是 power_on；冷启动验证仍缺失。

**未标修复。** 先整机断电重上电做冷启动复测，并实际按下/拖动/释放；若仍出现
0x39/0x18 单比特错或 0x30 中途 BUS_ERR，下一单变量为 25k（E16），不改延时。
会话日志：本轮 Qoder 原始会话未按收尾流程导出核对；缺失记缺失，不以串口日志代替。

### 用户补充：未转换 RST 时可触摸（2026-10-26）

用户指出：E2 那版 RST 未做电平转换、屏端 1.8 V/10 kΩ 上拉叠板端 3.3 V 上拉时
**可以触摸**，只是 RST 约 2.88 V 带来横纹；改电平转换后横纹消失但触摸失效；
F101 同模组转换与否都能触摸。对照日志：E2 确实「可触摸+横纹」；E6/E8 为 1.7 V
无横纹但 init 失败；E15 1.7 V 偶发 ID 与一帧。2.88 V 也不是稳定解（E4/E5 同
RST 高仍 init 失败），更像 RST 高电平/边沿/驱动能力处于临界。此前「RST 静态
电平基本排除」的判断撤回，不能把过压方案当交付。下一步优先：保持 SDA/SCL
转换，单独把 RST 暂时恢复直驱做 A/B（短测、接受横纹），或测屏端 RST 空闲值与
边沿并与 F101 对比；同时按计划先做 25k 单变量。

### 分层判断与下一步（2026-10-26）

- E13 的 `st=00` 是 TWI **bus error（非法电平）**，`st=f8` 是总线非空闲，
  `st=f9` 为超时/未知，`st=48` 才是地址 NACK；`lcr=2a` 表示故障瞬间 SCL 高、
  SDA 被拉低。读帧的 repeated START 已核对（HAL 在两条 msg 间 restart），
  帧格式不是主因。结论：当前主故障在物理层，不是写回判据。
- 用户明确：旧的全直联（RST 3.3 V 直驱 + I2C 直联）已证明可触摸但有横纹，
  不再复测；横纹与 RST 高电平相关，降到 1.7 V 后消失。目标是在保持 1.8 V
  域的前提下恢复触摸，故不做全直联 A/B。
- 10 kΩ 上拉在 50 kHz 已贴标准模式 1 µs 上升沿上限（10 kΩ×100 pF=1 µs，
  200 pF=2 µs），2SK3018 无源转换只会更慢；屏端 RST 实测 1.7 V 而非 1.8 V，
  说明 10 kΩ 已被轻微负载拉低。先并 1 kΩ 到 1.8 V，把高电平顶到约 1.79 V
  并加快边沿（仍在 1.8 V 域，不会重新花屏）；不行再换有源 1.8 V 推挽转换，
  不要长期用 2.88 V。SDA/SCL 若仍位翻转，再并 2.2 kΩ 或换有源转换。
- INT(PB5) 1.7 V 进 3.3 V GPIO 永远读低，驱动只能盲轮询；首帧后 active 20 ms
  正落在 F101 记录的 <26 ms 危险区。需把 INT 做 1.8 V→3.3 V 转换/比较器，
  改为中断触发；临时至少 active ≥30–50 ms，禁止 2 ms 连重试。
- 固件顺序：先做 RST 1 kΩ/有源转换的单变量硬件实验，再看结果；若 RST 修好后
  触摸稳定，再做 active 20→50 ms 的节奏实验；25 k 仅作 SI 诊断。不再先叠加
  容错补丁。

## E16：RST/INT 恢复 GT911 引脚状态与复位时序（2026-10-26，已打包，待冷启动板测）

用户决定先不做 1 kΩ 硬件改动，要求把 RST/INT 引脚状态恢复成 GT911 驱动的做法
再试。仅复制引脚状态与复位波形；HX83102 的固定 0x48 地址、0x13/0x0d 唤醒、
5 字节 AHB 与 E15 读回判据全部保留，不复制 GT911 的 INT 地址选择逻辑。

唯一行为变量（`hx83102_pins_config()` / `hx83102_reset_sequence()`）：

- RST(PB4)、INT(PB5) 均为输出、`GPIO_DRIVING_LEVEL3`，不再预置 RST 高电平。
- 复位：RST/INT 同时拉低 20 ms → RST 拉高 50 ms → INT 拉高后立即转
  `GPIO_MUXSEL_IN` + 方向输入 + 关闭上下拉，再等 20 ms；随后保留原 200 ms
  settle 才探测。E14 失败重试也改走同一整套时序（INT 先恢复输出）。
- GT911 原驱动用 `up_udelay()`，本板实测快约 200 倍（E10），故这里仍用
  `usleep` 的毫秒延时；总时长约 90 ms + 200 ms，不是照抄微秒数。

风险：INT 未做电平转换，R528 以 3.3 V 输出拉低 20 ms 时可能与模组 INT 争用；
这是按用户要求的短测，不视为最终接线方案。若阴性，回到 RST 并 1 kΩ 到 1.8 V
或有源 1.8 V 推挽转换，再考虑 INT 1.8 V→3.3 V。

构建/打包：首次用 `/data/vela/pack.sh` 误走了 gemini-s1 流程，其 defconfig 未开
`CONFIG_HX83102_IIC_TOUCH`，重编译后镜像不含驱动，已废弃。改按 V2 流程
`build.sh ynm3000_v2` + `pack_img.sh -f r528s3/ynm3000_v2_nand`；该次 pack.sh 的
`savedefconfig` 改写了 gemini-s1 defconfig（含用户既有改动），未回滚。V2 构建退出 0，
`.config` 含 `CONFIG_HX83102_IIC_TOUCH=y`，ELF 含 E16 与 18 处 HX83102 串；
pack `Dragon execute image.cfg SUCCESS`，V2 资源自证通过（nsh==vela `3414a72a`、
res.fex `502446c8`）：`build-v2.log`（`output/p286-e16/build-ynm3000_v2-final.log`）、
`pack-v2.log`（`output/p286-e16/pack-ynm3000_v2-final.log`）、
`check-res-v2.log`（`output/p286-e16/check-res-v2.log`）、
`E16 patch`（`output/p286-e16/e16-driver.patch`）。

镜像 P286-E16-gt911-pins.img（镜像已清理），
53,342,208B，MD5 `8e2d2c0aa250823cd15bbaed3b5b5471`；内含
`P286-E16 GT911 RST/INT pin state + reset seq, 50k`。

板测要求：**整机断电冷启动**（resetcause=power_on），核对 E16 三步复位日志、
`ID=00 2b 10 83`、`/dev/input0`、首帧与后续 0x30 读是否稳定；必须实际按下/
拖动/释放。只重启（soft reset）不算。**未标修复。**

### 板测回报（08:16，仍是软复位，阴性且比 E15 差）

启动：E16 三步复位日志齐全，`ID=00 2b 10 83`，`/dev/input0` 打开；08:16:47
首个“valid”帧为全 FF（空闲），08:16:48–50 连续三帧全 00 被拒，08:16:51 起
0x30 读 `transfer=-1`，出现 st=00/48/30 与 E14 recover；到 08:17:50 无任何
真实触点帧，E15 曾出现的 `02 22 03 04` 与 Settings 打开未复现。系统/LVGL/显示
仍在运行。`resetcause=cpu_soft_reset(reboot)`，仍非冷启动。

判断：GT911 的 INT 输出/复位时序不适用于 HX83102——GT911 的 INT 是地址
strap 输入，HX83102 的 INT 是控制器输出；主动复位运行中的控制器后，ID 能读
但事件栈只回全 0，且总线更快退化。用户判定冷/热启动不构成变量（设计正确应
两者都能用），**不再做冷启动，E16 封档为阴性**。剩余方向只有 RST/I2C 电平
转换的有源方案（优先对比 F101）或 25k 软件绕过；若都不做，V2 触摸按未解收口。

## E17：25k 单变量绕过（2026-10-26，已刷机，阴性）

用户选择最后一条低成本软件实验。回退 E16 的 GT911 引脚/复位，恢复 E15 状态
（RST 预置高、INT 输入、无启动复位，E14 失败脉冲保留），唯一变量 50k→25k。
目的：看 56 字节长读是否还中途 `st=00`/卡 SDA；能通也只是绕过，不是修复。

V2 构建退出 0，`.config` 含 `CONFIG_HX83102_IIC_TOUCH=y`，ELF/镜像含 E17；
pack 成功，V2 资源自证通过（nsh==vela `7a51ac5f`、res.fex `502446c8`）：
`build.log`（`output/p286-e17-25k/build-ynm3000_v2.log`）、
`pack.log`（`output/p286-e17-25k/pack-ynm3000_v2.log`）、
`check-res-v2.log`（`output/p286-e17-25k/check-res-v2.log`）、
`E17 patch`（`output/p286-e17-25k/E17-c-only.patch`）。

镜像 P286-E17-25k.img（镜像已清理），
53,342,208B，MD5 `711f8c9cf5710588ad03692838218369`；版本串
`P286-E17 25k diagnostic, E15 pins, no boot reset`。

板测看点：`I2C bus frequency: 25000 Hz`、`ID=00 2b 10 83`、`/dev/input0`、
首帧与 0x30 长读是否稳定；实际按下/拖动/释放。

### 板测回报（08:29，软复位，阴性）

08:29:17 E17 启动，确认 `25000 Hz`。首个 `interface_on()` 读失败
（`st=00 lcr=2a`），E14 recover 后第二次探测得到 `ID=00 2b 10 83`、init ok，
LVGL 打开 `/dev/input0`（maxpoint 10）。08:29:18–20 仅三帧：全 FF、全 00、
`00..01..00`，均被拒；08:29:48 累计 12 次错误后 re-identify，AHB 写、ID 读、
0x30 56B 读连续 `transfer=-1`，`st=00/30/f8/f9`、`lcr=2a`（SCL 高、SDA 低）
反复出现；到 08:30:22 无真实触点帧。`resetcause=cpu_soft_reset(reboot)`；
沿用用户「冷/热启动不构成变量」的判定，不再做冷启动。

结论：25k 与 100k/50k 同症，降速不解决，50–100k 的上升沿余量不是主因
（至少不是唯一因素）；不据此断言根因。E17 封档阴性，驱动源已回退 E15/50k。
V2 触摸按未解收口；剩余只走硬件路线（RST/I2C 有源 1.8V 转换，优先与 F101
对比），不再叠加软件容错。



## E18：400k 在当前转换接线下复测（2026-10-26，已打包，待刷机）

用户指出旧的 400k 数据不能直接否定当前接线：当时可能尚未做 SDA/SCL 电平转换，
且内部下拉/上拉状态未核对；devlog 中 SDA/SCL 转换完成时间也确实未确认。旧结论
只代表旧条件，E18 在**现接线**下重测 400k。

基线仍取最好的 E15（曾出真实触点帧并打开 Settings），唯一行为变量
`HX83102_I2C_FREQUENCY` 50k→400k。另加一条只读诊断：启动时用
`hal_gpio_get_pull()` 打印 SDA/SCL/RST/INT 的 pull 值（0=无 1=上拉 2=下拉），
不改变引脚配置，用于确认“内部下拉”到底是否开启（E8 曾读到
`0x02000054=0x00050050`，即 PB2/PB3 上拉、PB4/PB5 无上下拉）。
`E18 patch`（`output/p286-e18-400k/E18-c-only.patch`）

V2 构建退出 0、0 warning，`.config` 含 `CONFIG_HX83102_IIC_TOUCH=y`，ELF/镜像
含 E18；pack 成功，V2 资源自证通过（nsh==vela `022ddebb`、res.fex `502446c8`）：
`build.log`（`output/p286-e18-400k/build-ynm3000_v2.log`）、
`pack.log`（`output/p286-e18-400k/pack-ynm3000_v2.log`）、
`check-res-v2.log`（`output/p286-e18-400k/check-res-v2.log`）。

镜像 P286-E18-400k.img（镜像已清理），
53,342,208B，MD5 `b6b78a4c7f26cc37289ab5f14aef0401`；版本串
`P286-E18 400k, E15 pins, pull log`。

板测先看 `E18 pulls SDA=? SCL=? RST=? INT=?`：若 SDA/SCL 为 2（下拉），
本次 400k 仍被下拉污染，下一步先单独关下拉；若为 1（上拉）或无下拉，才是
干净的 400k 对照。再看 `I2C bus frequency: 400000 Hz`、`ID=00 2b 10 83`、
`/dev/input0`、0x30 长读与真实按下/拖动/释放。若 400k 通过而 50k 不通过，
需再用“50k+相同 pull 状态”隔离；若 400k 更差，则当前接线下确属越快越差。
**未刷机，未标修复。**

## E19：事务边界等总线空闲（2026-10-26，已打包，待刷机）

依据见 [参考板对照](10-26-p286-reference-boards.md)：F101 与 R528 共用同一块转接板、
静态电平完全一致（板端 3.3 V / 屏端 1.7 V），F101 的 init 同样是 8 笔无延时连发并
一次通过；而 R528 在**同一控制器、同一 50 kHz** 下，控制台单笔事务 ACK 且能读出合法帧，
驱动连续事务却全败。E13 的 `st=00` + `lcr=2a` 与 E14 的 9-clock 恢复进一步说明：
总线会在**事务边界**卡住，而不是 IC 不应答。

H：上一笔 STOP 尚未释放 SDA 时就发起下一笔 START，控制器判总线错误。

唯一行为变量（相对 E15 基线）：**每笔传输前等待总线空闲**。

1. `hal_twi.c` 新增 `hal_twi_wait_bus_idle()`：读 `TWI_LCR_REG`，bit5=SCL、bit4=SDA，
   两者都为 1 才放行；50 µs 轮询，超时可配（本实验 10 ms）。
2. `drv_twi.c` 的 `r528_transfer()` 在 `hal_twi_xfer()` 前调用它；超时只打印
   `[TWI0-E19] bus busy before xfer`（最多 8 条），不改变控制流、不吞错误。
3. `HX83102_I2C_FREQUENCY` 400000 → **50000**（回 E15 基线；E18 未刷，不叠加）。
4. 版本串 `P286-E19 bus-idle wait, E15 pins, 50k`。

按用户要求**直接改驱动源码**，不产出补丁；改动文件为 `hal_twi.c`、`sunxi_hal_twi.h`、
`drv_twi.c`、`hx83102_iic_touch.c/.h`。编辑过程中 `hal_twi.c` 一度被写坏（截断至
2037 行），已用 `output/p286-e14-recovery/` 的备份 + E14 补丁恢复为 2864 行，
前 1990 行与损坏前逐行一致，构建通过可作旁证。

产物：P286-E19-bus-idle.img（镜像已清理），
53,342,208 B，MD5 `e2f50c3d59bd215109975d44bbdd3948`，SHA-256
`f93ceea419b9bf72f3609ee7ee9b7f4d5e0265f13c4f7fa1e157a428a8605f18`。
V2 构建退出 0，`vela.bin` 含 `P286-E19` 与 `[TWI0-E19]`；pack
`Dragon execute image.cfg SUCCESS !`；`nsh.fex == vela.bin`
（MD5 `970fabc2c24ca4cf9b543141f8e5344d`），`res.fex` 23,961,600 B。
证据：`构建`（`output/p286-e19/build.log`）、`打包`（`output/p286-e19/pack.log`）。

验收：出现 `P286-E19 bus-idle wait`、`I2C bus frequency: 50000 Hz`；
ID 是否一次成功、`/dev/input0` 是否出现、`[TWI0-E13]` 的 `st=00` 是否消失、
是否有真实按下/拖动/释放帧。

判读：若 init 通过且触点正常 → 根因方向为事务边界，修法是等待而非延时；
若仍失败且日志出现 `[TWI0-E19] bus busy` → 总线确实长期不空闲，转总线恢复/上拉；
若既无 busy 又失败 → 等空闲不是关键，下一步 GPIO bit-bang 切开 TWI 外设。
**未刷机，未标修复。**

### E19 板测（2026-10-26 09:54–09:55，冷启动）

日志 `logs/serial/20261026-094536_p286-e19.log`（boot #4 起为 E19）。

**结果：触摸第一次真正驱动了 UI。** LVGL 记录两次 `app_icon_click_cb`，
分别打开 Settings 与 Calendar——这是 R528 上首次出现可用的点按。

启动链：`[TWI0-E19] bus busy before xfer` 出现 6–7 次，每次紧跟
`[TWI0-E14] recover=N stuck lcr=2a st=f8` → `recovered lcr=3a`；
E15 容错 + E14 恢复后 `ID=00 2b 10 83`、`init ok`、`first valid frame`（全 ff）。

运行链仍有失败：`transfer=-1` 9 次、全 00 拒收帧 3 次、`re-identify` 0 次。
E13 明细给出关键分布：

```
fail=1  msgs=1 byte=1  st=00 lcr=2a   <- 单笔地址写，第 1 字节后
fail=10 msgs=2 byte=55 st=00 lcr=2a   <- 56 字节读，第 55 字节（最后一字节）
fail=11 msgs=2 byte=36 st=00 lcr=2a
fail=12 msgs=2 byte=3  st=00 lcr=3a
```

**H（新）：失败集中在读的最后一字节。** 长读在 byte=55/56 失败，且失败后总线停在
`lcr=2a`（SCL 高、SDA 低），说明主机在最后一字节的 ACK/NACK→STOP 边界没有让从机
释放 SDA，控制器把随后的 STOP 判成非法电平（st=00 总线错误）。这与"事务边界"假设一致，
但把位置收窄到了**读的收尾**，而不是每条事务的起点。

下一步（未实施）：在长读最后一字节后加一次 SCL 脉冲或延时再发 STOP，或核对
`hal_twi` 在 NACK 阶段的 A_ACK 处理；同时把 E19 的空闲等待从"下一笔前"改成
"失败后立即恢复"，避免每次失败都留一个脏总线给下一条。

**E19 不标修复**：触摸可用但每读若干帧仍会失败并触发恢复，属部分可用。

## E20：读收尾 STOP + 失败即恢复（2026-10-26，已打包，待刷机）

O（E19 板测）：触摸可用但"不灵敏"；`transfer=-1` 9 次、全 00 拒收帧 3 次；
E13 明细显示长读在 **byte=55/56（最后一字节）** 以 `st=00` 失败，失败后总线停在
`lcr=2a`（SCL 高、SDA 低）。

H：最后一字节 NACK 后从机尚未释放 SDA，主机就发 STOP，控制器把该边沿判成非法电平；
失败后又不清理总线，下一帧继续踩脏总线，形成丢帧连锁。

唯一行为变量（相对 E19）：**事务收尾的总线卫生**。

1. `hal_twi.c` 新增 `twi_wait_sda_release()`：STOP 前最多等 100 µs 让 SDA 回到高。
2. `hal_twi_core_process()` 的 `ok_out/err_out` 收尾：**成功才发 STOP**（先等 SDA 释放）；
   出错时不再盲发 STOP——脏总线上再补一个 STOP 只会多一个非法边沿，交给恢复处理。
3. `drv_twi.c`：`hal_twi_xfer()` 失败后**立即** `hal_twi_recover_bus()`（9 clock + STOP），
   不再把脏总线留给下一笔。
4. 版本串 `P286-E20 last-byte stop + immediate recover, 50k`。

产物：P286-E20-lastbyte.img（镜像已清理），
53,342,208 B，MD5 `43887a6c807282eb72cddfa4ba5c7e1d`，SHA-256
`b6927f570444e2b54e673460e27dcaaf57ec2d97fb3992638f6d2390882fa853`。
V2 构建退出 0（`CC hal_twi.c`、`CC drv_twi.c`、`CC hx83102_iic_touch.c`）；
pack `Dragon execute image.cfg SUCCESS !`；`nsh.fex == vela.bin`
（MD5 `078bdb93a289e974bf9ca8db1b64e902`）。
证据：`构建`（`output/p286-e20/build.log`）、`打包`（`output/p286-e20/pack.log`）。

验收：`P286-E20` 标记；与 E19 同一组动作下 `transfer=-1` 与拒收帧计数是否下降、
`[TWI0-E14] recover` 是否减少、点按是否更跟手。**未刷机，未标修复。**

### E20 板测（2026-10-26，10 次点按测试）：**比 E19 差，已回退**

日志 `logs/serial/20261026-095858_p286-e20.log`。用户报"偶尔点了没反应"。

| 指标 | E19 | E20 |
|---|---|---|
| 10 次点按命中 | — | **7** |
| `transfer=-1` | 9 | 10 |
| 全 00 拒收帧 | 3 | 3 |
| `[TWI0-E14] recovered` | 7 | **15** |
| `[TWI0-E19] bus busy` | 6 | 3 |
| `re-identify` | 0 | **5** |

失败位置也不固定：msg=1 的 byte=3/4/14/40/55 都有，**不是**只发生在最后一字节。
因此 E20 的两个假设都不成立：

- "最后一字节 NACK 后未释放 SDA" —— 若成立应集中在 byte=55，实测分散；
- "出错时不该发 STOP" —— 恢复次数反而翻倍，说明不发 STOP 让脏状态留得更久。

E20 的两处 ISR 改动（`twi_wait_sda_release`、成功才 STOP）已**整段回退**到 E19 行为。

### 重新定位

F101 驱动注释写明：**这块面板链路本身就会随机丢单笔事务**，其对策是每次读都重试
3 次（`PROBE_TRIES`），而不是把总线修到不丢。我们的 E11 曾因 2 ms 连重试踩中 26 ms
危险区而删掉重试；现在有了 E19 的空闲等待和 E14 的恢复，可以在**恢复之后再重试**，
而不是原地连打。

## E21：失败后恢复并重试一次（2026-10-26，已打包，待刷机）

唯一行为变量（相对 E19）：`r528_transfer()` 失败 → `hal_twi_recover_bus()` →
**同一笔事务再试一次** → 仍失败才返回错误。计数日志
`[TWI0-E21] retry ok|retry failed`（各上限 16 条），用于量化重试命中率。

E20 的 ISR 改动已回退；E19 的空闲等待保留。

产物：P286-E21-retry.img（镜像已清理），
53,342,208 B，MD5 `a306b08f275d30b9ef05cbc89e501b2e`，SHA-256
`443704abac8515ef8ad070cf8982ab84ac3cf43395a3a38815e1ad5eaad32e16`；
`nsh.fex == vela.bin`（MD5 `ed5ad21e8bb8c65b389bc6f8cabe4940`）；
pack `Dragon execute image.cfg SUCCESS !`。证据：
`构建`（`output/p286-e21/build.log`）、`打包`（`output/p286-e21/pack.log`）。

验收：`P286-E21`；同一组 10 次点按下命中数是否高于 7/10；`retry ok` 与
`retry failed` 的比例；`re-identify` 是否回落。**未刷机，未标修复。**

### E21 板测（2026-10-26 10:08，软复位）

`retry ok = 1`、`retry failed = 0`。唯一那次重试救回的是**初始化**（第一笔事务
`st=f9` 超时 → 恢复 → 重试成功 → `ID=00 2b 10 83`）。
整段启动 `transfer=-1 = 0`，说明"恢复后重试一次"确实吃掉了传输层丢包。

但触摸仍只有 1 次命中，且 10:09:26 出现 `re-identify after 5 errors`——
失败形态从"传输失败"变成了"**读到全 00 帧**"。

## E22：无接触时轮询间隔 1000 ms → 100 ms（2026-10-26，已打包，待刷机）

复盘 `hx83102_worker()` 的门控：

```c
if (priv->last_state != 0                       /* 无接触 */
    && (now - priv->last_poll_ms) < HX83102_IDLE_MS)
  goto reschedule;                              /* IDLE_MS = 1000 */
```

无接触时**每秒只读一次事件栈**。一次点按约 100–200 ms，两次轮询之间整个被跳过——
这正是"点了没反应"。该 1000 ms 是 E11 在链路还被锤死时设的，当时合理，现在有
E19 空闲等待 + E14 恢复 + E21 重试，已经不需要这么长。

唯一行为变量：轮询延迟预算。

- `HX83102_IDLE_MS` 1000 → **100**（F101 同板用 200；26 ms 是已知危险区，100 ms 留 4 倍余量）
- `HX83102_BACKOFF_MAX_MS` 5000 → **500**（5 秒封顶会让一串坏帧感觉像死屏）
- 版本串 `P286-E22 idle 100ms, backoff cap 500ms, 50k`

产物：P286-E22-idle100.img（镜像已清理），
53,342,208 B，MD5 `5de329f7997f876179d7bb4199745e14`，SHA-256
`3672e627f8050f13ce086251dafbaffd2e31dc319fa91209258be8fba1698ba7`；
`nsh.fex == vela.bin`（MD5 `6fdb559947d7c64cc1dd91a027a3a724`）；
pack `Dragon execute image.cfg SUCCESS !`。证据：
`构建`（`output/p286-e22/build.log`）、`打包`（`output/p286-e22/pack.log`）。

验收：`P286-E22`；同样 10 次点按的命中数；`reject frame` 与 `re-identify` 是否随
轮询加快而上升（若上升，说明 100 ms 仍偏快，回 200 ms）。**未刷机，未标修复。**

## E23：全 00 帧重读（2026-10-26 10:18 打包，11:49 刷机）

### 设计依据，以及一条被证伪的分支

官方 Firefly 内核驱动（`hxchipset/`）与本驱动有两点不同：每笔传输重试 3 次且不插延时；
`himax_checksum_cal()` 把全 00 事件栈判为 `HX_CHKSUM_FAIL`（错误），不是空闲帧。
第一条 E21 已覆盖，E23 落实第二条。

原计划“读零帧就 `I2C_RESET()` 复位总线”编不过：本树 `ARCH_CHIP_R528`（`arch/dummy/Kconfig`）
没有 `select ARCH_HAVE_I2CRESET`，Kconfig 直接把 `CONFIG_I2C_RESET=y` 丢掉，`I2C_RESET` 未定义，
链接报 `undefined reference`。改判据时也否掉了这个方向：全 00 帧是 IC ACK 后回零（内部状态），
不是 SDA 被拉住——后者会在地址相 NACK，表现为传输失败，已由 `r528_transfer()` 的恢复+重试兜住。

唯一行为变量：`state < 0` 时把事件栈再读一次，仍失败才计错误。
版本串 `P286-E23 zero-frame reread, idle 100ms, 50k`；`defconfig` 里试加的
`CONFIG_I2C_RESET=y` 已撤（该符号在本树恒被丢弃，留着是假配置）。

产物：P286-E23-zeroframe-reread.img（镜像已清理），
53,342,208 B，MD5 `ccdb6b77643931084a35b8e9d23c0724`，SHA-256
`ed04e9b2b6da6cba40bce673c9c5335543f0815a113328247a5e85375ecf31b2`；
`nsh.fex == vela.bin`（MD5 `7f0cfc63df0dffccd0629d56e0238a6e`）；
pack `Dragon execute image.cfg SUCCESS !`。证据：
`构建`（`output/p286-e23/build.log`）、`打包`（`output/p286-e23/pack.log`）、
[串口](../../logs/serial/20261026-104628_p286-e23.log)。

### 板测（2026-10-26 11:49–11:51，冷启动）：触摸首次连续可用

启动链：3 次 U-Boot（刷机过程中的两次重启 + 正式启动）；正式启动 11:50:08，
`E18 pulls SDA=1 SCL=1 RST=0 INT=0`、`50000 Hz`、`ID=00 2b 10 83`、
首帧 `ff…ff 37`（合法空闲帧），init 仅 1 次 `st=00`（irq-before-stop）后自愈。

- **3 次真实点按跨应用成功**：11:50:20 Settings、11:50:29 Music、11:50:32 Calendar
- 3 次全 00 `reject frame`（11:50:10，启动初期）
- 计数（打印上限 16，实际 ≥）：`E13 fail=16`、`E14 recovered=16`、`retry ok=16`、
  `retry failed=8`、`bus busy=4`、`transfer=-1=5`、`re-identify=3`

**新证据（决定下一步）**：11:50:38–11:50:41 连续出现
`[TWI0-E14] still stuck lcr=2a st=f9 srst=00`。`lcr=0x2a` ⇒ SCL=1、SDA=0，
即 **SDA 被 IC 拉住**；9 时钟恢复解不开，只有 `re-identify` 里的 RST 脉冲能让它回来
（11:51:14 读回 ID），随后 11:51:45 再次卡死。失败簇与 wifi 扫描/重连、tone 播放、
天气抓取同窗。

结论：触摸首次达到“可用”（连续跨应用点击），但总线仍会周期性被 IC 拉住 SDA，
恢复只能靠 RST 脉冲，一次约 30 s。**显示线（P285）未受影响，仍未闭环。**

### 下一步（单一变量）

E24 候选：50k → **100k**（F101 在同一块转接板/同一上拉与电平转换下的已验证速率）。
若卡死与事务边沿密度相关，提速率应改变失败率；若反而更差，说明该 IC 在此接线下对边沿敏感，
回到“缩短恢复代价”（RST 脉冲代替完整 init）方向。**未做，仅登记。**

## R1–R2c 外部评审轮：一次成功移植与两次自造回归（2026-10-26 12:00–13:18）

> **2026-10-27 复核更正**：下文“软复位清 BUSEN 已证实”“set_frequency 会使能”与
> “ret=0 证明不是总线问题”均不能成立为当前结论；源码调用链与日志不足以支持。
> 另用户确认 INT 仅上拉 1.8 V、没有电平转换。见[专项复核](10-27-p286-touch-review.md)。

### 背景

外部 AI 对驱动做了一轮评审，改了总线层（端口锁 + `hal_twi_xfer_configured()`），
中途配额耗尽，只编译未打包。我接手：补打包、板测，然后按“分层”重构恢复策略。
**教训先行**：我这一轮三次改动里有两次是错的，且都不是“调参”，是结构判断错。

### R1（外部改动原样，12:12 打包 / 12:30 刷机）—— 目前最好

- 改动：`hal_twi_xfer_configured()` 把 set_frequency、等空闲、传输、恢复、重试纳入同一把端口锁；
  `drv_twi` 只负责传频率。
- 顺带（未声明）：**删掉了每笔传输前的 `hal_twi_init()`**。R1 相对 E23 因此是两个变量。
- 板测 12:31–12:34：**7 次跨应用点击**（settings/music/calendar/files/settings/ai/uart_dbg），
  贯穿错误期无死区；`transfer=-1` = 0；`retry ok` = 25；`still stuck` = 4；
  re-identify 每 30 s 一次（5 次），每次都在 1 s 内完成。

### R2（我的第一版分层重构，12:40 打包 / 12:54 刷机）—— 回归

三处改动：① 恢复次序改为“先软复位再打 9 时钟”；② 传输前总线忙就先恢复；③ 新增
`TWI_STATUS_ERROR_BUS_STUCK`(-5) → `-EBUSY`，触摸侧按 liveness（600 ms）升级。

板测 50 s：17 次 `link down`、5 次 RST、2 次点击。关键日志：

```
[TWI0-E14] recover=1 stuck lcr=2a st=f8
[TWI0-E14] still stuck lcr=3a st=00 srst=00   ← 线已放开(3a)，状态锁在 00
[TWI0] retry failed
```

`lcr=3a` 证明总线已自由，`st=00` 只是引擎把 bit-bang 的 STOP 记成 bus error 并锁存。
**我删掉了原来的收尾软复位**，没人清锁存 → 恢复成功却判失败 → `-EBUSY` → 每 250 ms 复位。

### R2b（补收尾软复位 + 分频器移到每次传输前，12:58 打包 / 13:03 刷机）—— 更差

板测 14.6 min：5 次点击、**966 次 `link down`、385 次 RST 脉冲**。关键日志：

```
[TWI0-E14] still stuck lcr=2a st=f9 srst=00   ← SDA 仍低，9 时钟完全没生效
```

**根因（已由板子证实）**：软复位会清 `TWI_CTL_REG.BUSEN`。没有总线时钟，LCR 手动拉 SCL
不生效，9 个时钟是空转。我把 `hal_twi_set_frequency()`（它同时 `twi_enable(BUSEN)`）
挪到了恢复之后，于是恢复时总线时钟是关的。
**“先停引擎再打时钟”在这个硬件上不成立**——引擎必须保持使能才能 bit-bang。

附带证据：R1 里“恢复后重试成功”说明分频器未必被软复位清掉，
我“软复位清 `TWI_CLK_REG`、必须重设频率”的推断至少不完整，目前仍属未验证改动。

### R2c（回退恢复次序 + 断触帧转储，13:06 打包，**未刷机**）

- `twi_port0_recover()` 回退到 R1 次序：等复位完成 → 9 时钟 → STOP → 软复位 → 验证。
- 新增 `stalled frame:` 转储（上限 12 帧）：启动时的 3 帧 dump 额度在断触前就用尽，
  至今**没有任何一帧坏帧的内容**。

### “断触”的真实形态（本轮最有价值的新事实）

R2b 里绝大多数是 `link down 845 ms (ret=0, errs=5)`。`ret=0` 说明**传输成功、帧无效**，
不是总线不通：IC 会连续 0.8–1 s 只回坏帧。与 E23 的全 00 帧同类，但持续时间长得多。
目前缺少的唯一证据就是这些帧的内容。

### 当前代码状态（交接）

| 文件 | 现状 |
|---|---|
| `hal_twi.c:1020` `twi_port0_recover()` | R1 次序（已回退），收尾软复位保留 |
| `hal_twi.c:2945` `hal_twi_xfer_configured()` | 传输前总线忙→先恢复；失败→恢复+重试；每次尝试前重设分频（**未验证**） |
| `sunxi_hal_twi.h` | 新增 `TWI_STATUS_ERROR_BUS_STUCK = -5` |
| `drv_twi.c:126` | BUS_STUCK → `-EBUSY`，其余 → `-EIO` |
| `hx83102_iic_touch.c:60,86,725` | liveness 阶梯：`STALL_MS 600`、guard 250 ms 起几何退避、`recover_tries` 只在解出有效帧时归零；stall 帧转储 12 帧 |

### 证据边界

- R1 的“好”只有一次 3 分钟窗口、7 次点击，没有重复次数。
- R2b 的 966/385 说明：**恢复失效时 liveness 阶梯会自激**，每 2 s 复位一次。
- 未验证：`-EBUSY` 立即升级在真死链上是否过于激进；600 ms 阈值是否合适；分频器重设是否必要。
- 显示线（P285）与本轮无关，分开判断。

### 建议下一步（未做）

1. 先刷 R2c（= R1 次序 + 转储），**只取 `stalled frame:`，不要同时改阈值**。
2. 按帧内容分叉：全 `00` → IC 内部状态丢失；`ff…ff 37` → 判据错（其实是空闲帧）；
   校验和不对的残帧 → 读时序/半帧。
3. 拿到帧再定阶梯阈值；若 `-EBUSY` 仍高频，先确认恢复是否真成功（看 `recovered` 行）。

## R3：空闲全 00 帧改为合法空闲（2026-10-27 00:33，已打包，未刷机）

O（R2b 板测日志复盘，`logs/serial/20261026-104628_p286-e23.log`）：R2b 窗口
13:03–14:05 内 1665 次 `link down`、770 次 RST；`tries=0` 795 次、`tries=1` 841 次，
所有 `reject frame:` 均为 56×00；每次 RST 后第一帧都是 `ff…ff 37`，ID 均可读
`00 2b 10 83`。E23 日志 11:50:09.905 合法空闲帧之后，11:50:10.000/.065/.135 三帧全 00，
期间无触摸，11:50:20 点按仍成功。即无触点轮询时 0x30 返回 56×00 是“事件栈为空”；
Himax 只在 INT 触发读且连续 >5 次全 00 时才按 ESD 处理。原解码把空闲 00 判坏，叠加
R2 的 600 ms liveness + 250 ms guard，形成“空闲 00 → 无好帧 → 重初始化 → RST →
一帧合法空闲 → 空闲 00”的自激环。

H：把空闲全 00 当坏帧是 R2/R2b 断触与复位风暴的直接维持因素；接受它可退出自激。

唯一行为变量（`hx83102_decode()`）：校验和仍须为 0；全 00 在 `last_state != 0`
（未跟踪触点）时返回 1（合法空闲），跟踪触点时仍返回 -1，保留 200 ms 无好帧合成抬手。
E23 立即重读只对非零坏帧保留；R2c 的 HAL 次序、50 kHz、100 ms 空闲/20 ms 跟踪、
退避与显示代码不变。身份串 `P286-R3 idle-zero benign, R2c HAL, 50k`。

产物：P286-R3-idlezero.img（镜像已清理），
53,346,304 B，SHA-256
`7b669a96c12d517dfd4a956bab7b71e7831c267f4f1bba2b9af418b0d737104f`，
MD5 `86e919e8d3c45a1389ffef2cd579f601`。V2 构建 EXIT=0，`CC hx83102_iic_touch.c`；
宿主打包 EXIT=0，`Dragon execute image.cfg SUCCESS !`；`nsh.fex == nuttx/vela.bin`
（MD5 `acaf5289acad4404b8e8927bef4c67f8`），整包与 nsh 均含 R3 身份串，
res.fex 23,961,600 B。证据：`编译`（`output/p286-r3/P286-R3-build.log`）、
`打包`（`output/p286-r3/P286-R3-pack.log`）。打包日志第 48 行是既存 `usrdata`
缺省警告（`try default data`），不影响 Dragon 成功。

预测与验收：空闲时不再出现 `link down … (ret=0)` 与周期性 `RST pulse`；10 次点按与
10 s 拖动应连续响应；`stalled frame:` 若出现应为非零坏帧。注意启动日志的
`first valid frame:` 现在可能直接是 56×00（已属合法空闲），不能再要求它必须是
`ff…ff 37`。需刷机 + 串口；14:05 采集器因 `/dev/ttyUSB0` 消失中断，刷机前先恢复采集。
未刷机，不标修复。

### R3 板测（2026-10-27 00:52，已刷）

用户刷入 `P286-R3-idlezero.img` 并重启，串口确认 `P286-R3 idle-zero benign,
R2c HAL, 50k`；`ID=00 2b 10 83`、`init ok`、LVGL 打开 `/dev/input0`（maxpoint 1）。
启动后空闲 73 s：`link down` 0、RST 0、`reject frame` 0、`re-identify` 0；仅初始化
1 次 `transfer=-16` 后 `retry ok`。R3 的空闲全 00 接受消除了此前的空闲自激 RST。

日历连续滑动（00:55:31–00:56:00，29 s）体验差，日志：11 次 `link down`
（678–1229 ms）、4 次 RST、8 个 `stalled frame`、10 次 `retry failed`、9 次 ID 重读。
坏帧不是全 00，而是带真实触点的撕裂帧：例如前导 0/14/17 字节为零，随后
`01 b1 00 5b`（x=433,y=91）或 `02 67 00 57`（x=615,y=87），再跟重复的
`02 cf ff ff`；校验和失败。说明 IC 在出触点，但读取被截断/缓冲区残留旧数据。
驱动在无有效帧 200 ms 后合成抬手、600 ms 后进入 RST 阶梯，连续滑动因此被反复打断
（本次拖动期间 4 次 RST）。

下一步候选 R4（未实施，单变量）：跟踪到触点期间，把“无有效帧”的容忍延长到
2 s；这 2 s 内不发合成抬手、不升级 RST，只用现有退避重试；超过 2 s 才按旧策略
释放并复位。空闲路径保持 R3 不变。预测：跨过 0.6–1.2 s 的坏帧窗口，拖动不断线；
代价是真死链时手指可能保持按下最多 2 s。证据：
`logs/serial/20261027-004751_p286-r3.log`。

## R4：评审与修复流程（2026-10-27，未改代码）

对驱动做双轮代码评审：77.2/100，`NEEDS_FIX`，无 Critical，4 High + 2 Medium，
设计健康度 6/10（B）。主要发现：

- DR-F001（High）：读失败/HAL 隐藏重试/E23 立即重读后不排空，事件栈失步；`:675–698`。
- DR-F002（High）：恢复与触点状态次序、过期 `now`、复位后首帧变 MOVE；`:734–796`。
- DR-F003（High）：`work_queue` 重排队失败后轮询静默停止；`:812–828`。
- DR-F004（High）：跟踪中全 00 当故障，可能误合成抬手/升级 RST；`:379–382`。
- DR-F005（Medium）：register 未重置恢复/诊断状态；DR-F006（Medium）：缺
  `raw_out_sel`/结构校验。
- 警告：unregister 与 open fd 契约、100 ms 空闲门控、LPWORK 阻塞、`found!=count`
  过严、复位脉冲 10 ms、printf 过多等。

用户否决“延长无帧容忍到 2 s”的 R4 草案（会冻结拖动）。改为先修事务/帧同步，再修
恢复状态，最后才调整释放语义。完整阶段、单变量、验收与回滚见
[修复流程](../specs/misc/hx83102-touch-fix-process.md)。

当前停止点（2026-10-27）：P0 拖动基线已采集（22 `link down`/7 RST/3 帧拒收）；
首个单变量 E1.0（AHB burst 门控）已改码、V2 构建打包通过，镜像
`output/.../rtos_nuttx_r528s3-ynm3000-v2_uart0_128Mnand.img`（MD5 `ebbcf3db…`，
`nsh.fex == vela.bin`，身份串 `P286-R4a burst-gate`），**未刷机**。R3 仍在板上。
下一步：用户确认后刷 R4a，按流程跑空闲/点按/拖动三段验收。根因未确认：
H1=HAL 隐藏恢复推进事件栈指针（主候选），H2=轮询节奏不匹配，H3=IC 持有 SDA
与读取事务冲突（E1.0 直接检验）。

## R4s：STOP 错误传播修复（2026-10-27，已打包，未刷机）

目标/平台：R528 V2 + HX83102B；修正“数据收完但 STOP 失败仍报成功”，并保存首次拒收帧。
本轮用户授权修改驱动。实际入场源码已是 R4a（含 0x11 门控），没有覆盖这部分先前改动。
基线：vendor/allwinnertech HEAD `4c122d896b8ca786ad3f9a6768f37eeecdfe71e8` + dirty；
当前 V2 基线镜像 SHA-256 `a15d3114f4e4265fa2e33f207bd68486566702a25454f7797134b0e66cd5da46`。
完整备份、原有 TWI diff、配置、逐文件哈希在 `output/p286-r4s-stop/`。

O：twi_stop 返回 -4，但 hal_twi_core_process 只比 -1 且仅打印；成功路径仍通知 code=0。
H：STOP 假成功使已有 HAL 恢复不能及时执行，可能放大后续故障；断触因果尚待板测。
唯一行为变量（相对入场 R4a）：STOP 非零结果传播到 completion；已有数据错误优先保留。
原有 NOSTOP、门控、频率、轮询、恢复阈值、INT 无内部上拉均不变。
附加观测：ISR 仅保存 STOP 后寄存器，线程打印最多 16 条 `[TWI0-R4s] stop-fail`；
首次拒收帧在重读覆盖前保存，读完后打印最多 6 对。帧转储改单次 printf，减少混行。
日志可能影响时序，且额度可能在启动耗尽；不能把它当无扰动测量。

验证：
- 从实际源文件提取 ISR/completion 做故障注入：旧版 STOP=-4 仍成功（断言失败），
  新版 16 组通过，涵盖单/双消息读写、NOSTOP、原始 NACK 优先；ASan/UBSan 通过。
- 两路独立代码复核无新增阻塞问题；新增/修改行 nxstyle 退出 0。
  checkpatch 的范围封装有脚本参数错误及已有上下文格式告警，未宣称全文件风格通过。
- `pack_deskmate.sh ynm3000_v2` 实际调用 SDK build.sh 和正确 V2 -f/-g 路径。
  首次编译含 CC hal_twi.c / hx83102_iic_touch.c 和 LD nuttx；Dragon 在沙箱被 SIGSYS 中止。
  获准在沙箱外重跑后退出 0，Dragon SUCCESS；check_deskmate 验证配置、ELF、启动、
  181 份资源、nsh==vela.bin、整包内嵌完整 nsh/res 均 PASS。
- 源码/配置快照复核：仅 hx83102_iic_touch.c 与 hal_twi.c 改变；defconfig/.config 未改变。
- 串口采集器已核对在线，1.5 M；本轮未发板端命令、未刷机，硬件验证 NOT_RUN。

交付镜像：`output/ynm3000_v2/ynm3000_v2.img`，53,346,304 B，SHA-256：
`2ce5109cdcb9dbbc222ee61725b7aa92e1ff5a68e607e1ca344f8da209967d3f`。
固件身份：`P286-R4s stop-error propagation, R4a base, 50k`。
证据副本：`output/p286-r4s-stop/P286-R4s-stop.img`、`change.patch`、
`test-before.log`、`test-after.log`、`nxstyle-added-lines.log`、`build-pack-unsandboxed.log`。

C：STOP 假成功的软件缺陷已通过故障注入证实并修正；实际触摸稳定性未知。
补丁保留待板测。R4a 尚无确认板测，本包不能相对 R3 声称单变量：R3→R4a 是门控，
R4a→R4s 是 STOP 传播（另有有限观测）。需按相同动作对照，避免混淆改善来源。
下一步：用户确认刷机后核对 R4s 启动串，先空闲，再点按/拖动；观察 STOP 日志与坏帧关联。
若出现空闲故障或 RST >5/min，停止该实验并按授权恢复已有基线；若稳定，再进行完整三段验收。

收尾边界：源码镜像检查发现无关 luncher_dm 大范围漂移和镜像独有文件，未执行全量覆盖；
HAL TWI 文件目前也未列在镜像 manifest 映射中。本轮完整补丁和源文件备份已保存，镜像同步待处理。
会话原始日志是否入仓以 finalize_check 为准，不以手写记录代替。
