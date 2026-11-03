# P285 — R528 显示移植：命令包类型与 burst 时序核对

目标：定位颜色发淡、残影的主机软件原因。平台：R528 Gemini-S1，native 720×1440，横屏 1440×720。
验收：同一硬件、可识别固件上的单变量复测；编译及宿主测试不证明画面修复。

## 观察与当前结论

- 用户本轮确认：屏幕在 RK3588、指定 Helitai-BOE DTS 下显示正常；四套初始化序列在 R528 上刷过，表现相同。
- 将屏幕本体故障降为非优先方向，重点检查移植的软件语义、DSI 主机和显示管线。
- **确认两个代码层面的错误：初始化 DCS 被映射为 generic；burst 延时计算发生无符号下溢。**
- **颜色/残影根因尚未闭环。** 用户后续确认已刷 E1，显示无明显变化；详见下方实机反馈。
  制包调查时未检测到串口设备，本次反馈未附启动日志。
- 不再换初始化参数表。E1 保留原有 31 条的命令、参数、顺序、延时，仅修正初始化发送接口。

## 基线与事实源

`vendor/allwinnertech` HEAD：`4c122d896b8ca786ad3f9a6768f37eeecdfe71e8`；有既存未提交改动，未清理、提交或打 tag。
`workspace_check.py --all-repos` 退出 1：静态需关注，不是构建失败。开始时保存差异和源码/配置/ELF/镜像副本到
`/tmp/p285-display-audit/`。有效配置为 `nuttx/.config` 的 R528/SUN8IW20，不是 goldfish。

| 开始时文件 | SHA256 |
|---|---|
| `BOE_1440x720.c` | `65bce6eb1df45733e023256edbfb275d7bf0d8b5bd32c108d32720e71af07176` |
| `BOE_1440x720_mipi_config.c` | `1982a5db3d327bc799bed2f4aca4e0e43d48a3138838897838b1b349d406528d` |
| `nuttx/.config` | `222e3192d0cd65dfff59c7d60db1da88d52b5613e9be8a5be883b96fe8b3caf5` |
| `nuttx/nuttx` | `a950b5dbb094d0a008d78e5103d53b90bd519c9c32040545390c8a31d4d5c06d` |
| 原整包 | `a11d2cdd7c8d333c2cf14de97fa117b5c9d79ea99fc39d8ba662abcbcdc172b1` |

基线文件同处一工作区不证明同次构建；当前板上最后刷入版本仍待用户确认。
`BOE_1440x720_port.md` 描述了四变体及读回诊断，但会话开始时实际 C 文件只有 31 条固定表，无该诊断。
因此不能将历史 MD 当成当前可执行代码。

本地对照：

- `/data/hx83102b-dsi-bringup/dts/rk3588-firefly-roc-rk3588s-pc-helitai-boe-1440x720p.dtsi`
- `/data/hx83102b-dsi-bringup/panels/01-himax-hx83102b-boe/init-sequence.txt`
- 实际编译接线：`disp2/Make.defs` 选 `de_dsi.c` 和 `de_lcd_sun50iw10.c`；
  `de_feat.h` 的 SUN8IW20 分支启用 DSI_VERSION_40/SUPPORT_COMBO_DPHY。
- 不以低层目录中另一份 Linux 风格 Makefile 推断 NuttX 实际编译哪个 DSI 实现。

## H1 / E1：移植时把 DCS 私有命令错当 generic

完整调用链：`lcd_panel_init` → `sunxi_lcd_dsi_gen_write` → `src_ops` →
`bsp_disp_lcd_dsi_gen_wr` → `dsi_gen_wr` → TX FIFO → LP 发送。

DSI 类型 `0x39` 是 **DCS long write**；generic long 是 `0x29`。
厂商私有寄存器同样可以通过 DCS 发送，不能用“私有命令”判断为 generic。
本地参考文本与旧移植说明都把 `0x39` 写成 generic，是同一语义错误的传播，不能互相充当验证。

Rockchip 的 [panel-simple.c](https://github.com/rockchip-linux/kernel/blob/develop-5.10/drivers/gpu/drm/panel/panel-simple.c)
把 DTS 的 `0x39` 交给 `mipi_dsi_dcs_write_buffer`。
其 [drm_mipi_dsi.c](https://github.com/rockchip-linux/kernel/blob/develop-5.10/drivers/gpu/drm/drm_mipi_dsi.c)
按总载荷 1/2/更多字节选择 `0x05/0x15/0x39`。
这是参考分支源码核对，尚未取得用户正常 RK 板的实际内核版本。

| 参数数（不含命令） | R528 原路径实际类型 | RK DCS 路径类型 | 当前 31 条中数量 |
|---|---|---|---|
| 0 | `0x05` | `0x05` | 3 |
| 1 | `0x23` generic short | `0x15` DCS short | 9 |
| ≥2 | `0x29` generic long | `0x39` DCS long | 19 |

E1 唯一行为变量：`lcd_panel_init()` 全部调用 `sunxi_lcd_dsi_dcs_write()`。
日志新增身份 `BOE1440: P285 E1 DCS transport (31 cmds)`；无初始化参数/延时、DSI 模式、时序或 G2D 修改。
关机路径沿用基线，旧 `04 5A` generic 映射亦有同类问题，但本实验不测试关机/休眠，不夹带修改。

宿主实验：提取并执行当前 `dsi_dcs_wr`、`dsi_gen_wr`、ECC/CRC、DSI 配置函数与 `lcd_panel_init`；
采用源码寄存器 bitfield，RAM 代替 MMIO，仅替换硬件启动、延时和日志。
31 条载荷校验通过，类型不匹配由 **28 → 0**；ASan/UBSan 运行通过。
首次 LeakSanitizer 在沙箱 ptrace 下失败，经授权在宿主重跑通过，未关闭检查器。
证据：[基线](p285-display-evidence/transport-baseline.log)、[E1](p285-display-evidence/transport-fixed.log)、
[复现脚本](p285-display-evidence/check_transport.py)、[本轮补丁](p285-display-evidence/e1.patch)。

结论边界：证明传输映射错误，不证明面板一定拒收 generic。四表若共用错误发送路径，换表不能排除共同问题；
本轮未逐个反汇编 V1～V4 镜像，也未证明用户实际刷入的包都与当前代码相同。

## H2：RK burst 参数不能直接等同于全志 burst 实现

`de_dsi.c:dsi_basic_cfg()` 的 burst 分支：

```text
loop_n1 = (lcd_ht - lcd_x) * 150 / (lcd_dclk_freq * 8) - 50
        = (800 - 720) * 150 / (80 * 8) - 50
        = 18 - 50
        = -32 → uint32 下溢 → 12 位寄存器 4064
```

实际执行源码函数得到 VIDEO `loop_n1=49`，BURST `loop_n1=4064`，两份循环寄存器均使用该公式。
这是窄水平消隐/当前像素时钟触发的主机适配错误，不能靠再次盲切 burst 验证。
旧 P283 同时换模式和序列，已有混杂变量；历史“burst 已证伪”应撤销。
本轮 **未修改 burst 分支**，没有足够硬件时序依据把负值随意截断为 0 当作修复。

## H3：像素、颜色和帧缓冲路径核对

| 项目 | 本轮静态/宿主核对 | 边界 |
|---|---|---|
| 像素格式 | framebuffer 32bpp/ARGB8888，DSI RGB888；生成像素包 DT=`0x3e`、WC=2160 | 内存含 alpha、线上 24bit 本身合法，不是位宽错配证据 |
| 时序转换 | HBP=50 含 HSA20 → 后肩30；VBP17 含 VSA2 → 后肩15；HT800/VT1657 | 未测线上实际频率 |
| VIDEO 包长度 | HSA50/HBP84/HFP74，像素2160字节；无该分支负数长度 | 不是带宽/信号质量验收 |
| 像素时钟 | 请求80MHz，与RK79.54MHz相差约0.58%；DSI_DIV=24/4=6 | PLL实际值、帧率待读回 |
| `lcd_dsi_clk_rate=160` | 当前解析/计算路径不读此键；时钟从 dclk/分频器推导 | 修改160不能作为有效时钟实验 |
| `lcd_dsi_port_num=3` | 单 TCON 模式不走其双DSI分支 | 注释“DSI端口”含糊，当前不是 lane 数 |
| LP / EOT | 命令调用 LPTX；视频 HS EOT关闭 | 与参考 flags 方向一致，未上总线抓包 |
| `lcd_frm=0` | 含义为 FRM bypass，旧注释“0 RGB/1 RBG”不正确 | 未启用RGB666/565抖动 |
| Gamma / cmap | 未配置 gamma/cmap enable；面板 gamma 表为恒等表 | 未读实机寄存器，不排除运行时改变 |
| CSC | LCD静态配置为RGB，manager默认全范围；CSC normal为50/50/50/50 | 未发现固定16～235的证据，运行态待验 |
| G2D | degree3进入独立旋转模块，该分支不使用 alpha/mode 做混色 | 不把 `alpha=1,mode=255` 直接认定为变淡原因 |
| G2D 待查风险 | 实例由 malloc分配，未整体清零，haddr字段进入旋转地址计算 | 属于需验证风险；E1未修改，无实机内存证据 |

当前没有证据把现象归因于 ARM32/ARM64 指针宽度本身；已经确认的是 **主机接口语义及控制器时序约束没有等价移植**。

## 构建与实验包

- `bash /data/vela/pack.sh`：R528 编译成功。第一次 dragon 被沙箱 SIGSYS（Bad system call）终止，
  `check_res` 发现整包缺新 nsh 并失败；该次结果不交付。
- 经授权在宿主重跑：编译、dragon 打包、`check_res` 全通过，退出 0。
- 包内完整包含本次 nsh 与 res；`vela.bin` 检出 E1 身份字符串；配置及 MIPI 配置与基线哈希相同。
- 既存 `pwm_ret` 格式警告仍在。NuttX checkpatch不通过（基线279条、本轮277条诊断），
  此厂商文件使用非NuttX格式；本轮未进行整体格式化，不能报告风格检查通过。
- 打包日志仍有 SD卡 boot0 checksum 提示；当前 NAND `image.cfg` 选用 `boot0_nand.fex`，
  不是该提示对应的 `boot0_sdcard.fex`。未修改 bootloader，未验证刷机启动。

实验包：`/data/dm/output/p285-display-audit/P285-E1-dcs-video.img`（53,338,112 B）。

| 产物 | SHA256 |
|---|---|
| E1 整包 | `f93ebeea1e1b78ff590a734079dbaa164e8c35be998763f7653e1796fb5c9374` |
| ELF | `3796e7bbbe0cda595640cc13dfab1ecf4032e6e2f84def966378b0f38f053fd2` |
| vela.bin | `81378b566132db4ad9f49551317feb909010a86f1213c72d6d5e5b61d5c2d1e0` |

整包 MD5：`53b2050cd333170ab94055a0fed1c73a`。
日志：[编译/打包](p285-display-evidence/pack-host.log)、[哈希](p285-display-evidence/e1.sha256)。

## E1 实机反馈（2026-10-22，用户报告）

O：用户明确反馈：“刷了E1 但是并未发生什么变化。”记录为已刷 E1，颜色发淡/残影无明显改善。
证据来源：用户肉眼观察；未附串口启动标记、刷入校验或画面对照，尚未独立核验运行身份。
唯一变量：初始化发送 generic → DCS；命令参数表、延时、VIDEO模式和时序保持原样。
结果：未观察到预期的显示改善。
C：削弱“包类型映射是当前显示异常主要原因”的假设；单改映射未解决现象。
不能据此排除整个DSI模块、时序适配或显示管线，也不能把burst下溢认定为当前VIDEO模式的根因。
补丁去留：E1映射修正暂留为协议修复候选，未回退、未标已修复；本次仅记录，不修改固件、不增加实验。

## 新证据（2026-10-23，用户报告）：同屏同转接板在另一参考板显示正常

同一块 BOE 720×1440 屏 + 同一块转接板，在 sun252iw2（SyterKit/Zephyr demo）板上跑色彩条/灰阶渐变 demo **颜色完全正常、无残影**。用户附启动日志，关键参数：

- 4 lane，format 0（RGB888），flags 0xd（Zephyr 语义：VIDEO_MODE | NO_EOT | SYNC_PULSE，**非 burst**）
- D-PHY HS clock 472 MHz，720x1440@60.003Hz，tcon-lcd → dphy → panel-dsi 直连，无 G2D/DE 复杂管线
- demo 帧率 scan≈59.5，uf/wait/write 均为 0

结论边界：屏本体与转接板故障方向可基本排除；残影/发淡的主因收敛到 **R528 主机侧**（DSI 控制器配置/时序/初始化发送路径）。参考主机明确用的是 VIDEO SYNC_PULSE + NO_EOT + RGB888，与 R528 当前 VIDEO 配置方向一致、与 burst 方向相反，为 H2 的 burst 下溢实验再次降低优先级。
下一步仍是既定计划：R528 上用 TCON 内建色条分层；若色条同样异常，直接对照该参考日志逐项核对 lane 数/PHY 速率/包类型/时序。

## E2（2026-10-23）：整表照抄 F101 实机验证配置

用户指定参考 `/data/F101-YuzukiNeko/`。该项目 10-21/22 已验证：同屏同转接板在 F101（sun252iw2 Zephyr）
上用 11 条短表 + non-burst video 显示正常（E3 镜像 `15a60fdba…`，"显示 Ok"），并准备过 R528 回灌补丁
（当时因沙箱只读未写入）。本会话核对：

- 现有 `patches/r528-boe-display-from-f101.patch` 基线是更早的"长序列版"工作树，与当前 E1 文件
  （SHA `7d1d8eec…`）不匹配，不能直接 apply；改为手工把 `lcd_panel_init` 整体替换为 11 条表。
- 唯一变量：31 条 DCS 表 → F101 11 条表（0x35 00/0x11/0x29 + FF 22/FF 27/FB 01/AD 25/AE 55/AF 05/FF 10/FB 01），
  传输分类照抄 F101（厂商命令 generic、标准 DCS 走 DCS），0x29 后 50ms；复位后 120ms 等待按 F101 去掉。
  mipi_config、退出序列、GPIO/电源/触摸不动。
- 注意与 P284 记录冲突：P284 称 11 条短表为 Novatek 玻璃"勿再用"，但 F101 同屏实机验证该表显示正常，
  该论断撤销。
- 产物：`output/p285-display-audit/P285-E2-f101-11cmd.img`（53,338,112 B），
  SHA256 `de460fa090e70141142d59f4098aeca1c9e3ba0d72b6768ed9bc11770ed8c50c`；
  pack/check_res 通过（EXIT=0），身份字符串 `BOE1440: init F101 verified 11-command sequence` 已核实编入。
  **未刷机**。上板验收：颜色/残影对照 F101 参考，回归触摸。

## E2 实机反馈（2026-10-23，用户报告）

O：用户刷入 E2，反馈"情况一样"——颜色/残影与之前各版本相同，无改善。
证据来源：用户肉眼观察，无串口日志；运行身份未独立核验（镜像含身份字符串，包哈希 `de460fa0…`）。
唯一变量：31 条 DCS 表 → F101 实机验证 11 条表 + generic 传输，其余全不动。
结果：未观察到改善。
C（本轮复核修订）：**初始化表方向降低优先级，尚不能排除初始化生效问题**。
同屏在 F101 正常、用户报告 R528 多版同症，支持优先检查 R528 输出及绘制路径；
运行身份、面板收包/寄存器状态未独立核验，不能把历史组合数当成排除整个模块的证据。
补丁去留：E2 表保留（与 F101 验证参考一致，最不可能引入新变量），不回退。
下一步：按既定计划实施 R528 TCON 内建色条分层——色条异常→DSI/PHY 视频输出路径；
色条正常→DE/framebuffer/G2D。

## E3（2026-10-23）：启用 TCON 色条分层实验工具

目的：E2 同症后按既定计划做分层——TCON 内建色条绕过 DE/framebuffer，区分 DSI/PHY 输出与上层管线。

- 发现 `disp_debug.c` 的 `disp_colorbar_store` 调试接口完整存在但两层未接线：
  `Make.defs` 第 55 行注释掉了 `disp_debug.c`；`disp_debug.c` 尾部 RT-Thread FINSH 注册是死代码。
  修改：Make.defs 启用该文件 + 文件内新增 `dispdebug_main`（FLAT 可直接挂）+
  `apps/builtin/registry/dispdebug.{bdat,pdat}` 注册 nsh 命令。
- 踩坑记录：`apps/builtin/registry/.updated` 时间戳文件过期导致 builtin_list 不重生成，
  链接期报 undefined reference；删除该生成文件后重生成通过。builtin 注册是通配收集，
  新增 registry 文件必须清 `.updated` 才生效。
- 产物：`output/p285-display-audit/P285-E3-colorbar-tool.img`
  SHA256 `c97d19d663e3a466eb3cae8b05bd854e9966de7104951428a86a2b660f58ce3c`；
  pack/check_res 通过（EXIT=0），`dispdebug` 字符串已核实入镜像。含 E2 的 11 条 init。
- 用法（nsh）：`dispdebug -c 0 1` = TCON 内建色条（绕过 DE）；`dispdebug -c 0 2` 灰阶；
  `dispdebug -c 0 8` = DE 图层色条（绕过 framebuffer 内容/G2D 输入）；`dispdebug -r` dump DE/TCON 寄存器。
- 判读：1/2 异常 → TCON 图案源及其下游输出/面板状态；1/2 正常而 8 异常 → DE 配置/合成或测试本身；
  8 绕过 G2D 输入，不能用其异常证明 G2D 故障。8 正常而 UI 异常，再查 framebuffer/G2D/LVGL。

## 颜色浅淡、背景条纹与 LVGL 分层复核（2026-10-23）

目标：定位并修复 R528 颜色浅淡/背景条纹；验收必须有相同画面的实机前后对照。
本轮完成静态分析和 E3 产物核验，未修改固件源码/配置、未重新构建或刷机；未宣称修复。

O：用户再次报告颜色浅淡、背景条纹，并询问 LVGL 兼容性。未获得 E3 色条观察结果。
宿主 `/dev/ttyUSB0`、`/dev/ttyACM0`、`/dev/serial/by-id` 均不存在，串口采集器未运行。

静态事实（均为当前工作树，不能代替运行态）：

- `nuttx/.config` 为 R528，`LV_COLOR_DEPTH=32`、`LV_USE_DRAW_G2D=y`、硬件旋转启用。
  LVGL 仓为 `apps/graphics/lvgl/lvgl`，HEAD `b1a5606b25510a1de8f0ee1d5448ac8fca278896`。
- `lv_nuttx_fbdev.c:fb_fmt_to_color_format()` 把驱动 `FB_FMT_RGB32` 映射成 XRGB8888，
  按驱动 stride 建立 DIRECT 缓冲；当前 `FB_NUM=1`。没有发现 16/32bpp 配置不匹配证据。
- `dev_fb.c` 显示层按 ARGB8888、`alpha_mode=0` 配置；`fb_g2d_rot.c` 同样使用 ARGB8888。
  XRGB 的第4字节与下游 alpha 语义值得检查，但不能仅凭名称差异认定像素错误。
  软件不透明填充会写 `lv_color_to_u32()`，仍需取实际像素/寄存器验证。
- `dm_theme_wash_bg()` 是 `#DCE7FA` → `#F4EDF6` 的淡色竖渐变，三个通道跨度仅24/6/4。
  `lv_draw_sw_gradient.c` 按整数插值生成颜色；`lv_draw_g2d.c` 不接管渐变填充。
  因此背景可能本身具有量化色阶；不能未经纯色对照把背景条纹等同 DSI 传输异常。
- `disp_colorbar_store(0,1)` 经 device/AL 最终写 TCON `src_sel=1`，绕过 LVGL、fb、G2D、DE。
  值4/5为全0/全1，值0恢复DE输入。值8创建DE常量色图层，**同样绕过G2D输入**，
  且不保存/恢复原图层配置，不能用值0撤销其新建图层；首轮不执行8。

H（最多三项，均未确认）：
1. TCON/DSI/PHY/面板运行状态造成输出异常。
2. 板端 LVGL 加速、fb/G2D旋转、DE格式/混合或缓存适配造成像素异常。
3. UI本身淡色渐变的量化色阶，可能与另一项输出问题同时存在。

E（待板测，唯一变量=图案源）：已复核 E3 包 SHA256 为
`c97d19d663e3a466eb3cae8b05bd854e9966de7104951428a86a2b660f58ce3c`。
在确认运行该包后，依次单独执行并观察 `dispdebug -c 0 1`、`-c 0 4`、`-c 0 5`，
最后 `dispdebug -c 0 0` 恢复UI；色条固有边界不算条纹，观察各色块内部/纯色是否均匀。
若同症，优先查TCON及下游，LVGL渲染不再是该测试图案异常的必要原因；若正常，
下一步直接fb纯色对照，保留G2D旋转/DE而绕过LVGL，再决定是否做仅关闭LVGL绘制加速的A/B。
关闭LVGL绘制加速不会关闭fb硬件旋转，不可将两者合并成一个实验。

C：目前不能确认“就是驱动”或“LVGL不兼容”；E1/E2未消除现象，不再叠加初始化表补丁。
用户进一步要求修复，现阶段缺实机分层证据，保留现有补丁并等待色条结果。
本次来源Codex，session `01a16727-e086-76a3-9f8a-c79523f2d59a`；日志采集状态以收尾检查为准。

## 下一步（待后续执行）

先核对运行身份与观测方式，再用 TCON 内建色条分离 framebuffer/DE 与 DSI/面板输出。
若色条同样异常，优先检查TCON之后的DSI时序/输出路径；若色条正常，优先检查framebuffer/G2D/DE。
该实验尚未实施；不继续换初始化表、不直接叠加burst补丁。

代码：E1 用户已刷、无明显改善，不标已修复。文档：本报告覆盖旧因果断言。
源码镜像：发现镜像独有 `ui_paneltest.c` 及其他既存差异，本轮不执行会删除/覆盖它们的全量同步；
本轮补丁已单独保存。会话日志：未证实本次原始会话已采集，收尾单列缺口。

收尾检查器退出1：CODE=WARN（既存dirty/工作区静态提醒，另有本轮构建证据）；
DOCS=WARN（镜像一致，15项既存超长节点/未索引详情提醒，本轮链接无失败）；
LOGS=MISSING（本次session `01a165ea-05fd-7613-b323-975c61521f37` 未入manifest/JSONL，事件数不可核实，来源Codex）。
未用手写总结冒充原始会话，也未调用当日MiMo全量导出。

## E4（2026-10-25）：触摸 RST 脉冲 A/B（依据 F101 #56）

O：用户指出 `/data/F101-YuzukiNeko` 同屏项目曾出现横纹；其 #56 用 A/B 证明"触摸驱动启动 PD3 复位脉冲"与横纹相关（去掉 `RESET_ON_INIT` 后无横纹，机理未证明）。
本机 `logs/serial/20261020-004840_p275-panel.log` 另显示：R528 触摸 init 成功，但 `0x30` 读持续失败，worker 每次 `re-reset + retry`，即运行期也在周期性打 RST 脉冲。

H：R528 的残影/横条与触摸 IC 的 RST 脉冲（启动一次 + 失败重试周期）相关。

E：唯一变量 `HX83102_RESET_PULSE`（新增宏，分支在 `hx83102_pin_reset()`）。0 = 不拉低，仅保持 RST 高并保留 70 ms 延时；1 = 原 20 ms 低 + 50 ms 高。
I2C 轮询、重试次数、200 ms settle、面板初始化表（E2 11 条）与时序均不变。启动打印 `[HX83102] P285-E4 RST pulse=0` 用于核对运行身份。

产物（同一棵树、仅该宏不同；`bash /data/vela/pack.sh` 两次均 EXIT=0）：

| 包 | 大小 | SHA256 | MD5 |
|---|---:|---|---|
| `output/p285-display-audit/P285-E4-no-touch-reset.img` | 62,516,224 B | `96b6c05e…` | `2c59ecc8…` |
| `output/p285-display-audit/P285-E4-control-reset-pulse.img` | 62,516,224 B | `e4b381e9…` | `8310599a…` |

**未刷机**，无实机观察。

判读：先刷 control 确认现象复现，再刷 no-reset。若 no-reset 消失而 control 复现 → 复位脉冲是相关变量（仍不是机理证明）；
若两包同症 → 触摸复位方向降级，转 I2C 流量/供电或 TCON 色条分层。no-reset 下触摸可能不可用（芯片未获复位），本实验以显示为准。

补丁去留：`HX83102_RESET_PULSE 0` 暂留为实验态；未获板测前不写"已修复"，也不改面板初始化表。

## E5（2026-10-25）：关闭触摸驱动后颜色恢复

O：刷 E4 对照包（`RESET_PULSE=1`，触摸行为与之前一致）后，用户报告 UI 布局变化（当前工作区 UI 源码所致，非触摸）且**颜色依然惨淡**。
随后按用户要求把 `CONFIG_HX83102_IIC_TOUCH` 置 n（保留 `CONFIG_R528_TWI0`，bus0 上无其他设备），重打包上板。
用户报告：**颜色回来了，屏幕显示 OK**。

E：唯一变量 = 触摸驱动是否编入（驱动、`pre/post-touch` 探针、轮询全部移除；镜像内 `HX83102` 字符串计数 0）。
产物 `output/p285-display-audit/P285-E5-no-touch-driver.img`，62,512,128 B，SHA256 `3580a79a…`，MD5 `f67a9f32…`；pack/check_res EXIT=0。

C：**触摸子系统活动与颜色惨淡/横条强相关**（E3，用户目视）。边界：
- 机理未证明。当前电平转换仍是错接，1.8V 侧被体二极管钳在约 2.6V 并向 1.8V 轨灌流；触摸驱动一跑就调制该注入，
  "驱动 bug"与"电平转换缺陷"两条假设目前不可分。
- 未验证触摸恢复后的显示状态；E4 的 RST 脉冲 A/B 因此降为次要（整条触摸通路都已是变量）。

补丁去留：defconfig 暂留 `CONFIG_HX83102_IIC_TOUCH=n`（显示优先）；触摸源码 `HX83102_RESET_PULSE 0` 保留但当前未编入。
下一步：修好电平转换后重新启用触摸，单变量确认显示是否仍正常、触摸是否可用。


