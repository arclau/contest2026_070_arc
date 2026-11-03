# MIPI 屏幕移植信息 — BOE_1440x720

> **P285 核对更正**：下文是历史记录，不能代替当前源码或板测结论。
> `0x39` 是 DCS long，generic long 是 `0x29`；当前初始化误用 generic 的问题已制作 E1 实验包。
> burst 时序公式在本屏参数下写入 `loop_n1=4064`，历史“burst 已证伪”及“初始化导致残影”断言证据不足。
> 本轮开始时实际源码无下文所称四变体/读回诊断。当前证据与待验步骤见工作区主源
> `project_docs/devlogs/10-22-p285-display-host-audit.md`。画面根因仍待实机复测。

> 来源 DTS: `rk3588-firefly-roc-rk3588s-pc-helitai-boe-1440x720p.dtsi`
> 初始化序列来源: **魅族 S6(m1712) 原厂 `update.zip` 里的 `boot.img` → kernel，现场反汇编**
> 目标平台: Vela/NuttX (R528 Gemini-S1 + OpenVela SDK)
> 模板驱动: `BOE_1200x1920` (同板级接线，竖屏 native + 旋转)
> 生成 2026-10-18；**2026-10-22 21:00 按原厂固件重核并重写初始化序列**

## 1. 基本信息

| 参数 | 值 | 来源 |
|------|-----|------|
| 面板名称 | BOE_1440x720 | 用户指定（产品横屏分辨率命名） |
| 面板 IC | Himax HX83102B | SETEXTC 载荷 `B9 83 10 2B`，ID=83102B |
| 玻璃 | Helitai-BOE | 驱动名 `Himax_Helitai_BOE_hd720_dsi_vdo_lcd_driver` |
| native 分辨率 | 720 × 1440 (竖屏) | DTS display-timings |
| 产品分辨率 | 1440 × 720 (横屏) | 旋转实现 (degree0=3) |
| 物理尺寸 | 80 × 120 mm (≈5.7") | DTS width-mm/height-mm |
| 接口 | MIPI DSI 4-lane, RGB888 | DTS dsi,lanes / dsi,format |
| DSI 模式 | Video (非 burst) | `lcd_dsi_if = LCD_DSI_IF_VIDEO_MODE` |

## 2. 时序参数 (native 720x1440) —— 已核，未改

| 参数 | DTS 值 | Vela 值 | 单位 |
|------|--------|---------|------|
| 像素时钟 | 79540000 | 80 | Hz / MHz |
| 水平有效 | 720 | 720 (lcd_x) | px |
| 水平前肩 | 30 | 30 (lcd_hfp) | px |
| 水平后肩(含 hspw) | 30+20 | 50 (lcd_hbp) | px |
| 水平同步 | 20 | 20 (lcd_hspw) | px |
| 水平总周期 | 800 | 800 (lcd_ht) | px |
| 垂直前肩 | 200 | 200 (lcd_vfp) | px |
| 垂直后肩(含 vspw) | 15+2 | 17 (lcd_vbp) | px |
| 垂直同步 | 2 | 2 (lcd_vspw) | px |
| 垂直总周期 | 1657 | 1657 (lcd_vt) | px |
| 刷新率 | ~60 | 60.3 | Hz |

> **语义已核（de_dsi.c 源码，不是推断）**：`dsi_hbp = (lcd_hbp - lcd_hspw) * bpp/8`
> （L1160）、`dsi_vbp = lcd_vbp - lcd_vspw`（L1203）→ `lcd_hbp`/`lcd_vbp` **含**同步脉宽，
> 故 50 / 17 正确。`lcd_dsi_clk_rate` 在 disp2 里只赋值、从不读取（DSI 时钟由
> `disp_al_lcd_get_clk_info()` 的 `dsi_div` 从 `lcd_dclk_freq` 推导），写 160 无副作用。
> `lcd_dsi_format = 0` = `LCD_DSI_FORMAT_RGB888`（include.h L632）。

## 3. 初始化序列

### 3.1 反汇编复核链（2026-10-22，自己重做，不用二手文本）

| 步骤 | 结果 |
|---|---|
| `stock/update.zip` → `boot.img` | 16843024 B，md5 `e431230257602779001bcc06239b3052`，magic `ANDROID!`，page=4096 |
| `boot.img` → kernel | 15170200 B，md5 `7d842c2bf8059ca161ec4e94d0b06016`，ARM64 Image（`ARMd`@0x38），`Linux version 4.4.13-g529cfe9-dirty` |
| 地址换算 | 文件偏移 = VA − `0xffffffc000080000`（Image 加载地址 0x80080000） |
| 写屏函数 | `0x307bf8`（与旧提取脚本一致，已复核为 `stp x29,x30` 正常函数头） |

**关键方法修正**：旧提取脚本 `tools/extract_for_rk.py` 里的 `FUNCS` 函数起点列表是猜的，
其中 `0x31cfec` 处首条指令就是 `ret`、`0x31d104` 处是 `cmp w0,#2`，都不是函数头。
本次改为**按 `ret` 指令切分函数边界**（0x31c000–0x31f400 共 40 个 ret），才得到真实边界。

### 3.2 原厂各回调的真实边界

| 地址 | 作用 | 内容 |
|---|---|---|
| `0x31cee0` | 显示开 | `35 00` / `11` / `29` |
| `0x31d028` | **显示关** | `28` / `10` / `04 5A` / **`05 5A`** |
| `0x31d1a0` | 深待机 | `B9 83 10 2B` + `D5`（两个未解析 msleep） |
| `0x31d454` | 关屏 2 | `28` / `10` |
| **`0x31d510`** | **INIT** | **31 条：`B9` … `E6` / `35` / `11` / `29`** |
| `0x31e0a4` | **关屏 3** | `28` / `10` / **`4F 01`** |
| `0x31e1bc` | Novatek 屏 init | `FF 22` / `FF 27` / … （别的玻璃，与本屏无关） |

ops 函数指针表在 `0xd7bfc8`：
`{0x31d190(stub), 0x31d454, 0x31d510, 0x31d198(stub), 0, 0x31d1a0}`
→ **init 是第 3 槽 `0x31d510`**；第 6 槽 `0x31d1a0` 是深待机。

### 3.3 前两次为什么都不行

**第一次（11 条 `FF 22/FF 27/FB 01/AD 25/AE 55/AF 05/FF 10/FB 01`）**
命令指纹是 **Novatek**（`FF <page>`+`FB 01` 切页），不是 Himax。里面没有
`B1/B2/B4/B6/C1`，等于电源、VCOM、Gamma 全没写 → 残影 + 色淡。
文件注释里"F101-YuzukiNeko overlay / SHA256"的出处查不到，不可采信。

**第二次（43 条长序列）** 数据是对的，但**串错了**，三处都来自边界误判：

1. 把 ops 第 6 槽 `0x31d1a0`（深待机）当成了 init → 序列前面多出 `05 5A` + `B9` + `D5`；
2. 把显示开 `0x31cee0`（`35/11/29`）与显示关 `0x31d028`（`28/10/04 5A/05 5A`）整段串进来
   → 先点亮、再关掉、再初始化；
3. 尾部带 `0x29 → 0x29 → 0x11 → 4F 01`，而 **`4F 01` 已证实属于关屏函数 `0x31e0a4`**
   → 点亮之后又发了一条关机提交，面板被推进低功耗态，**这本身就是"色淡 + 残影"的成因**。

### 3.4 现在发的序列（`hx_init_seq[31]`，原厂 `0x31d510` 函数体）

```
B9 83 10 2B | B1 | D2 | B2 | B4 | CC | D3 | D5 | D6 | E7 | BD 01 | E7 (+12ms) |
BD 00 | B6 | C1 01 | BD 01 | C1 | BD 02 | C1 | BD 03 | C1 | BD 00 |
C0 | BF | C7 | CE | E5 | E6 | 35 | 11 (+75ms) | 29
```

* 前 28 条与尾部 3 条全部按原厂顺序；只有 `E7(bank1)` 后 12ms、`0x11` 后 75ms 两个延时。
* 传输类型：前 28 条 generic-long(0x39)，尾部 `35/11/29` 为 DCS-short(0x05)，与原厂一致。
* **逐字节校验**：`/tmp/cmp2.py` 直接读 `kernel.bin`、现场解出 31 条，与驱动表比对
  → **31/31 条命令与全部载荷字节完全一致**；唯一差异是 `0x29` 后延时取 20ms（原厂 0）。
* 载荷长度交叉验证：rodata 里各 blob 首尾 8 字节对齐间隙自洽
  （`0x90bb28+45→0x90bb58`、`0x90bd58+61→0x90bd98` 等），长度无错位。
* 初始化前补 120ms：原厂 init 函数体内无前置延时，那段等待在三星 dsim 驱动里做；
  R528 这边 power_on 末次复位到 init 只隔 ~60ms，Himax 要求复位后 ≥120ms 才收命令。

### 3.5 退出序列（原厂 `0x31d028`，已补全）

| 字节 | 含义 | 延时 |
|------|------|------|
| `28` (DCS) | Display Off | 10ms |
| `10` (DCS) | Sleep In | 120ms |
| `04 5A` (GEN) | 厂商私有 | 120ms |
| `05 5A` (GEN) | 深待机提交（旧版漏了这条） | 120ms |

## 4. GPIO / 电源 / 背光（沿用 R528 板级）

| 功能 | Vela 配置 |
|------|-----------|
| 复位 | PD19（`lcd_gpio_0`），低有效 |
| 背光 PWM | ch4, PD20, 40KHz, 默认亮度 200 |
| 电源 | AXP2101 dcdc1=1.8V, dcdc3=1.8V, dcdc2(标称 9.6V) |
| DSI 引脚 | PD0~PD9 |

> RK 板级值全部丢弃（gpio1/pwm14/i2c4/vcc1v8_lcd0/vcc5v0_sys 是 Firefly 板）。
> DTSI 里的 Himax 触摸：本板 defconfig 开的是 `CONFIG_HX83102_IIC_TOUCH=y`（`boards/r528/drivers/hx83102_iic_touch.c`，I2C 0x48 / RST PB4 / INT PB5），不是 GT9271；但该驱动仍标 SKELETON、探测只做总线状态读，不读 chip ID。

## 5. 验证状态（2026-10-22 21:00）

| 项 | 结果 |
|---|---|
| 反汇编复核 | 自 `update.zip` 重新提取，31 条逐字节一致 |
| 编译 | 通过，新增 0 告警（仅剩 `pwm_ret=%d` 历史告警） |
| 打包 | 通过，`check_res` 全过（`res.fex` md5 `13b344a6…` 未变） |
| 镜像 | `rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img` md5 **`4cce19d8f5a440fc12718f8affefd843`** |
| 代码入镜像 | `strings vela.bin` 含 `init HX83102B 0x31d510 body` |
| **实机画面** | **未验** |

### 刷机后怎么看

1. 串口应有 `BOE1440: init HX83102B 0x31d510 body (31 cmds)` 与 `BOE1440: init done`。
2. 残影消失、颜色正常 → 收工。
3. 仍色淡/残影 → 下一个单变量：`BOE_1440x720_mipi_config.c` 的 `lcd_dsi_if`
   由 `LCD_DSI_IF_VIDEO_MODE` 改 `LCD_DSI_IF_BURST_MODE`（RK DTS 用的是 `VIDEO_BURST`）。
4. 黑屏 → 检查 120ms 前置延时是否够；必要时加大到 200ms 再试一次。
5. 画面正常但方向反 → `degree0` 3 ↔ 1，与触摸映射一起改。

## 6. 固件里到底有几套屏（2026-10-22 21:40 补核）

用户提示"里面有好几个屏幕的配置"，已核实。`update.zip` 的内核里确实有 **4 家面板**，
全部 720x1440 / 80x120mm：

| 面板 | init 表地址 | 指纹 |
|---|---|---|
| Himax HX83102B（本屏） | `0x90bb20`（代码引用）/ `0xdfc0e4`（无引用） | `B9 83 10 2B` |
| Novatek | `0x90bda0` | `FF 22 / FF 27 / FB 01` |
| FT8006U | `0x90bdd8` | `55 80 / 35 00 / FF 87 16 01` |
| FT8613 | `0x90bdf0` | `FF 87 16 01` |

**Himax 有两份表**，差异只有一条 `B6 BF BF E0`：

* `0x90bb20` 那份**带 B6**，被 `0x31d1cc`（深待机 0x31d1a0）和 `0x31d53c`（init 0x31d510）引用 → **活代码**；
* `0xdfc0e4` 那份**不带 B6**，带长度前缀（`03 B9 83 10 2B 0D B1 …`），
  全 text 段扫描 **0 处引用** → 死数据，运行时不用。

所以"表选错了"这个方向可以排除：驱动发的就是活的那一份，且已逐字节对齐。
DTB（`stock/dtb.dts`）里没有 `panel-init-sequence`，init 只存在于内核 rodata，来源唯一。

## 7. 当前未解：颜色淡（2026-10-22 21:50）

逻辑收敛：

1. 面板**完全没有 init** 时跑 OTP 默认值，画面应该是**正常**的（这是出厂标定值）。
   所以"颜色淡"不可能由"init 被忽略"解释，只能由"某条命令写进去了但值不对"或"主机侧"解释。
2. init 已证明是原厂活表、逐字节一致 → 序列本身不是嫌疑。
3. 剩下三类嫌疑：① 这块玻璃与原厂那批不是同一 lot，Gamma/模拟档位（`C1`/`B1`/`B2`/`B6`）不匹配；
   ② 主机侧颜色链路（DE gamma / CSC / dither / 背光）；③ 模拟供电档位。

**已加只读诊断**（`BOE1440_DIAG_READ`，默认 1，不发任何写命令、读通路有界不会挂死）：
`lcd_panel_init` 前后各读一次 DCS `0x0A`（电源模式）、`0x0B`（显示模式）、`0x04`（DDB）。
串口会打出类似：

```
BOE1440: pre  DCS 0x0A 1B: 08 00 ...
BOE1440: pre  DCS 0x04 8B: 03 48 49 4D ...     <- DDB，可看厂商串
BOE1440: init HX83102B 0x31d510 body (31 cmds)
BOE1440: post DCS 0x0A 1B: 09 ...
```

判读：
* `pre 0x0A` 读得到（不是 FAIL）→ DSI 读通路 + 面板应答正常，可以继续用读回做判据；
* `pre 0x04` 的前几字节含厂商串 → 直接确认这颗 IC 是谁（Himax 一般给 `HX...`）；
* `post 0x0A` 从 `08`（sleep out 后正常）变 `09`（display on）→ 说明尾部 `0x11/0x29` 确实被面板接受。
* 三条全 FAIL → 读通路不通，改用"极端 gamma 看画面是否变化"的可见性实验。

下一步（按代价排序）：
1. 看屏排线上的丝印/标签，直接确认 DDI 型号（零成本、最可靠）。
2. 刷本诊断包，把 `pre/post` 那几行日志发回。
3. 若确认是 Himax 且写进去了，则改 `C1` gamma 与 `B1/B2/B6` 模拟档位做单变量试调。

## 8. 四变体对照实验（2026-10-22 22:40）

驱动改成"一份源码 + 四个序列表"，用 `BOE1440_PANEL_VARIANT`（1..4）选一个编译。
四个序列都编进同一个 .c，未选中的会被链接器 gc 掉（可用 `B9 83 10 2B` 是否为 0
判断当前是哪个变体）。

| 变体 | 面板 | 条数 | 镜像 | md5 |
|---|---|---|---|---|
| V1 | Himax HX83102B / Helitai-BOE | 31 | `variants/V1-himax-boe.img` | `4a75bb1a0397cda61f8e84010d9a1418` |
| V2 | Novatek NT36xxx / Innolux-CMI | 13 | `variants/V2-novatek.img` | `b843afa12dee222680f61c3d1c0bbc69` |
| V3 | FocalTech FT8006U / Innolux-CMI | 12 | `variants/V3-ft8006u.img` | `cd2d96201784257c01f6a272ffa67c3c` |
| V4 | FocalTech FT8613 / TXD-CSOT | 25 | `variants/V4-ft8613.img` | `6923029f3775ce46ab9055b4842d1666` |

镜像目录：`vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/variants/`。
V2~V4 在原厂序列末尾补了 `0x11`/`0x29`，因为 R528 的 `lcd_open_flow` 在
`panel_init` 之后不会再发点亮命令（原厂靠上层 dsim 驱动补）。

**时序保持 800×1657@79.54MHz 不动**，只换 init 序列（单变量）。若某变体出图但
闪烁/滚动，说明面板对了、时序要换成该家的值。

### V2 不只是"试 Novatek"，它同时是对照组

Novatek 那 13 条里**没有 B1/B2/B4/B6/C1**，也就是不写电源/VCOM/Gamma，
等于让面板跑自己的 OTP 默认值。所以：

* V2 画面**正常** → 面板跑 OTP 就是好的 → "颜色淡"来自 V1 那套表里的某条写坏了；
* V2 画面**仍然淡** → 与面板型号无关，问题在主机侧（DE 颜色链路 / 背光 / 供电）；
* V2 **黑屏/花屏** → 不是 Novatek，换 V3。

串口判读：

```
BOE1440: ===== variant V2 Novatek NT36xxx / Innolux-CMI =====
BOE1440: pre  DCS 0x04 8B: 03 48 49 4D ...    <- DDB，可能直接报出厂商串
BOE1440: init done (13 cmds)
```
