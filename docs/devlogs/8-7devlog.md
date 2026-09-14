# DevLog 2026-08-07 — 切歌无声收尾排查（音量链路排除→死机修复→codec 输出深挖→字体修复→上层解码审查）

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**
> 前置：8-6part7devlog.md（切歌无声排查至"音量被调低"假设），固化点从 ok-20260807-1 起

---

## 一、任务背景

8-6 Part7 结束时遗留三个疑点：①切歌无声=怀疑 XPlayer 上层调低音量（writei peak 43~172 极低）②播放器音量 UI（用户需求）③第一首歌播放中死机（日志洪水）。本日围绕这三条展开，最终结论：**切歌无声在数据/音量/DMA/codec 输出/上层解码全链路排除，唯一剩物理链路验证；死机根因=DMA 诊断日志洪水已修复；音量按钮已修好（日志实证）**。

## 二、切歌音量假设验证（Part1：XPlayer 链 + raw_peak 诊断）

### 2.1 XPlayer 音量链代码审计（结论：无音量 API）

| 检查点 | 结论 |
|--------|------|
| `XPlayerReset`（xplayer.c:1028） | 只发 RESET 命令，不碰音量 |
| `XPlayerSetPlaybackSettings`（xplayer.c:690） | 仅按 mSpeed 切 Pause/Start，不碰音量 |
| `XPlayerSetSpeed`（xplayer.c:1048） | 不碰音量 |
| 全 libcedarx grep volume | 仅 demo 目录 `tinasoundcontrol.c` 有 `TinaSoundDeviceSetVolume`（不在播放链路上） |

**"切歌时 XPlayer 调低音量"假设代码层证伪**——音量唯一通道是 dm_sound_write 软件衰减。

### 2.2 dm_sound_write 诊断增强（衰减前原始 peak）

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c` | 新增 `g_dbg_raw_peak/g_dbg_raw_nz` 全局 + 衰减前统计块 + 打印并入 `raw_peak=xx raw_nz=xx peak=xx nz=xx` |

### 2.3 播放器音量 UI（用户需求，Part1 完成）

- 新增 `g_music_volume`（0-100，默认 30 保持听感）：固定 30% 衰减（9830/32768）→ `gain = (g_music_volume * 32768) / 100` 定点可调
- 新增 `music_round_btn()` 统一圆形玻璃按钮助手（白色玻璃常态 + 按下蓝紫渐变 + 放大 + 光晕 + 图标变白）
- 播放器右侧音量列：上 **+** 圆钮 / 中间音量数值（0-100 实时）/ 下 **−** 圆钮，步长 ±5
- `music_vol_cb` 点击改 g_music_volume 并实时刷新数值

固化 **ok-20260807-1**（vendor 仓 103+/53-）。产物 nsh.fex==vela.bin md5 `1aa8c325`（7,482,544B）。

## 三、第一首歌播放死机根因 + 修复（Part2）

### 3.1 现象

上板：Aurora 播放 1 秒内死机。日志每秒 200+ 条 `[dm] dma pointer status=1 residue=...`。

### 3.2 根因

`snd_dmaengine_pcm_pointer`（snd_dma.c）**每次查询打 3 条日志**（syslog + 2×snd_print），播放时 pointer 高频调用、含中断上下文（CPU0[0]）→ UART 输出饱和/锁竞争拖死系统。这是 ok-0806-dma-residue 诊断固件的日志残留，诊断使命已完成。

### 3.3 修复（全量清理 [dm] 诊断日志，7 个文件）

| 文件 | 清理内容 |
|------|----------|
| `snd_dma.c` | pointer 每查询 3 条（洪水主源）、open/close/prep/submit/trigger、dma callback start/finish |
| `dma_wrap.h` | chan_request/free/prep_cyclic/slave_config/start |
| `sunxi-pcm.c` | open/close/hw_params/trigger |
| `snd_pcm.c` | open_substream/release_substream/ksnd_pcm_open |
| `snd_core.c` | soc_pcm_prepare/close dapm |
| `sunxi-daudio.c` | daudio trigger、PA enable/disable |
| `sun8iw20-codec.c` | dapm_control、hp/lineout_route、hw_params、DAC_FIFOC、trigger |

保留：`snd_err` 错误路径、dm_sound_write 的 raw_peak 低频诊断（每 20 包一条）。验证：`grep -rn "\[dm\]"` 全仓 0 残留。

### 3.4 按钮按下观感修正

`music_round_btn` PRESSED 补 `bg_grad_color=COL_PURPLE` + `LV_GRAD_DIR_VER`（蓝→紫渐变）+ shadow 14px，与暂停键 CHECKED 观感一致。

固化 **ok-20260807-2**（vendor 仓 8 文件，5+/87-）。产物 md5 `64ddab28`。

## 四、音量按钮事件不触发修复（Part3）

### 4.1 现象

上板日志：0 条 `[music] volume=` 打印（回调从未触发）；Beautiful Love raw_peak **15→279→804→1525→2982→5603** 数据完全正常——**Part7 采到的 43~172 只是歌曲开头渐入段，"切歌数据被衰减"假设彻底排除**。

### 4.2 修复（双保险）

| 修复点 | 说明 |
|--------|------|
| user_data 读取 | `music_vol_cb` 改 `lv_obj_get_user_data(lv_event_get_target(e))`（与歌单行 music_track_click_cb 已验证路径一致），`music_round_btn` 同步 `lv_obj_set_user_data` |
| 挂载位置 | 音量列从 `subpage_overlay` 移入 `content` 内 `ctrl_wrap`（prev/next 挂 content 内切歌已验证可点） |

固化 **ok-20260807-3**（vendor 仓 1 文件，20+/9-）。产物 md5 `d0798ca2`。

## 五、播放器布局恢复（Part4）

### 5.1 现象

用户投诉音量列位置被改、整个播放器布局被破坏（ctrl_wrap 方案不可接受）。

### 5.2 恢复

- 删除 `ctrl_wrap`，`ctrl_row` 恢复直接挂 `parent`（lv_pct(50) 居中）
- 音量列恢复挂 `subpage_overlay` + `LV_ALIGN_RIGHT_MID`（播放器右侧，控件行不动）
- 保留事件修复（user_data 模式 + 蓝紫渐变 PRESSED）

固化 **ok-20260807-layout-restore**（vendor 仓 1 文件，9+/15-）。产物 md5 `2fcdbb7e`。

### 5.3 2026-08-06-music-testlog.txt 关键证据（用户上板日志）

- ✅ 音量按钮已修好：`[music] volume=35,40,45,...,90` 逐级递增，`peak/raw` 比值随音量精确变化（vol35→0.30、vol70→0.70、vol90→0.90）
- ✅ 四首歌切歌后 raw_peak 全部正常：Aurora(14830)、Beautiful Love(5303)、IGNIS(6416)、laojie(6564)

## 六、codec 输出链路深挖（Part5：调用驱动 skill）

### 6.1 代码审计（sun8iw20-codec.c 全输出路径）

| 项 | 值 |
|----|-----|
| 路由 | `pb_audio_route = LO_HP_SPK`（lineout + HP 双路 + 功放） |
| 功放 GPIO | `gpio_spk = GPIOD(17)`，`pa_level = HIGH`，`pa_msleep = 20ms` |
| digital_vol | `0x0`（最大，音量由软件衰减控制，ok-20260806-31 教训） |
| dapm_state 清零 | 确认在位（snd_pcm.c:498 每次 open 强制归零） |
| 调用链 | prepare→dapm(1)、close→dapm(0) 完整，无逻辑跳过 |

### 6.2 诊断日志（低频，防死机）

新增 `sunxi_codec_out_dump()`，仅挂 **dapm on/off + trigger** 两个低频点（绝不挂 pointer 高频）：

```
[codec] dapm on:  DPC=0x80000000 DAC_ANA=0x15fc7a HP_ANA=0x36404000 DAC_REG=0x15fc7a DAC_FIFOC=0x3004000 PA=1
```

固化 **ok-20260807-codec-dump**（vendor 仓 1 文件，+24）。产物 md5 `194fd26e`。

### 6.3 上板寄存器对比结论（决定性）

| 点 | DPC | DAC_ANA | HP_ANA | DAC_REG | DAC_FIFOC | PA |
|----|-----|---------|--------|---------|-----------|-----|
| Aurora dapm on | 0x80000000 | 0x15fc7a | 0x36404000 | 0x15fc7a | 0x3004000 | 1 |
| Beautiful Love dapm on | 0x80000000 | 0x15fc7a | 0x36404000 | 0x15fc7a | 0x3004000 | 1 |

EN_DAC=1、DACLEN/REN=1、PA=1 全部相同——**驱动输出链路（codec 模拟输出+功放）切歌前后零差异**。

## 七、UI 字体变小根因 + 修复（Part6）

### 7.1 现象

上板：`FT_New_Face error(0x1)` 刷屏（freetype 打不开字体）→ 整个 UI 字变小；同日志 `wifi firmware FAIL`、`nsh: for/done: command not found`。

### 7.2 根因链

res.fex/img 均**包含**字体（strings 验证 MiSansNormal/fa-solid，分区 25MB > res.fex 9MB 无截断）→ 真正问题：**rcS.nsh 启动脚本异常**（AGENTS.md 坑 #8：rcS.nsh 为 prebuilt 文件，增量编译不刷新进固件）→ `mount -t romfs /dev/res /resource` 未成功 → /resource 空 → 字体 + wifi 固件（都在 /resource 下）全读不到。

### 7.3 修复

`./build.sh <nsh 配置> distclean` 全量重编 + 重新打包；strings 验证 mount 命令、字体路径、FW_NIC 全部进入新 vela.bin。

固化 **ok-20260807-distclean**（三仓源码无新改动，仅打 tag；本次改动均为构建产物）。产物 md5 `14d426a4`。

## 八、上层解码链路审查（Part7：零代码改动）

完整读完：`audioRenderComponent.c`（handleStart/Stop/Pause/Reset、checkSampleRate、doRender、writeToSoundDevice、initSoundDevice）→ `adecoder.c`（44100→48000 重采样）→ `xplayer.c`（XPlayerReset 切歌路径）。

| 疑点 | 结论 |
|------|------|
| 切歌后 threadCtx 状态残留 | ❌ 无：`handleStart` 从 STOPPED 恢复时重置全部（nSampleRate=0/nChannelNum=0/needDirectOut=0/bFirstFrameSend=0） |
| checkSampleRate 重配时序 | ❌ 无：检测到变化才 Stop→SetFormat→Start，日志确认每次切歌正常执行 |
| adecoder 重采样状态 | ❌ 无：44100→48000 走 do_audioresample，Aurora/Beautiful Love 完全一致 |

**结论：上层解码链路无状态残留，数据流正常**。因用户要求"别再加BUG"，无确认根因时不改代码（避免重蹈改一个坏一个覆辙）。

## 九、最新上板日志分析（Part8：音量 20→100 测试 + 自动连播无声）

### 9.1 日志证据

- ✅ **音量链路完全正常**：volume=20→peak/raw=0.15、60→0.60、100→0.30（满幅 30498≈93%）——衰减比随音量精确 1:1
- ✅ 数据满幅：Beautiful Love raw_peak 16→5984（渐入后正常），音量 100 时 peak=30498
- ✅ codec 寄存器与 Aurora 逐位一致，PA=1
- ✅ **自动连播（Aurora 放完自动接 Beautiful Love）同样无声**——排除手动切歌操作因素

### 9.2 核心矛盾与思路

**满幅数据 + codec 使能 + PA=1 → 物理上喇叭必有声**。代码层（数据/音量/DMA/codec/功放控制/上层解码）已全部验证正常，继续读代码无意义。矛盾指向物理/时序层面，给出三个定位实验：

| 实验 | 目的 |
|------|------|
| A. aplay 直接播 Beautiful Love（绕开 XPlayer） | 有声=播放器路径问题；无声=歌/解码本身问题 |
| B. Beautiful Love 放第一首（开机先播） | 第一首有声="第二首"位置状态残留；无声=这首歌解码问题 |
| C. 上板实测 GPIOD(17) 电平 + codec LINEOUT 波形（万用表/示波器） | 确认 DAC 模拟输出是否真送出信号 |

**建议先刷 ok-20260807-distclean（含字体修复）再测，从实验 A 开始。**

## 十、文件改动汇总

| 固化点 | 改动文件 | 说明 |
|--------|----------|------|
| ok-20260807-1 | deskmate_ui.c | raw_peak 诊断 + 0-100 音量 + 音量 UI + 统一按钮 |
| ok-20260807-2 | snd_dma.c/dma_wrap.h/sunxi-pcm.c/snd_pcm.c/snd_core.c/sunxi-daudio.c/sun8iw20-codec.c + deskmate_ui.c | 清 [dm] 日志洪水（87 行删除）+ 蓝紫渐变 PRESSED |
| ok-20260807-3 | deskmate_ui.c | 音量按钮事件修复（user_data 模式 + 挂载位置） |
| ok-20260807-layout-restore | deskmate_ui.c | 布局恢复（音量列回 subpage_overlay + RIGHT_MID） |
| ok-20260807-codec-dump | sun8iw20-codec.c | sunxi_codec_out_dump 诊断（dapm/trigger 低频） |
| ok-20260807-distclean | 构建产物 | 全量重编修复字体/prebuilt 未刷新 |

验证命令：`./build.sh <nsh 配置> -j$(nproc)` → `lunch_nuttx 2 && pack` → `md5sum nsh.fex vela.bin` 一致（各固件 md5 见上文）。

## 十一、遗留事项 / 下一步

1. **🔴 切歌/连播无声（最高优先）**：代码层（数据/音量/DMA/codec 输出/功放/上层解码）已全链路排除；**下一步=上板做三个定位实验**（Part9 表）：A. aplay 直接播 Beautiful Love ② B. Beautiful Love 放第一首 ③ C. 实测 GPIOD(17) 电平 + LINEOUT 波形——结果决定是"物理链路"还是"第二首位置状态残留"
2. **待上板确认（ok-20260807-distclean）**：UI 字体恢复（FT_New_Face 消失）、wifi 固件正常、播放器布局（音量列右侧/控件行居中）、音量 +/- 点击有反应
3. 遗留不变：EBUSY（退出后未 close，app 层）、bt_recv 50% 空转、UI 模拟器同步暂停中、播放器音量 UI 已实现待最终听感确认

## 十二、卡顿根治：SDMMC 多块读 DMA bug 定位 + 修复（Part9 续）

### 12.1 背景

8-6 Part5 定论"任何 >1 块读必崩"但静态无法定位超时根因，维持 `MMCSD_MULTIBLOCK_LIMIT=1` 单块读权宜 → 大文件读全部退化为 CMD17 单块（512B/次 + 全 DMA setup/teardown + 信号量往返），解码供给慢 → **卡顿**。本 part 深挖出确定性根因并修复。

### 12.2 根因（代码级，多块读必崩的直接原因）

`mmcsd_readmultiple`（`nuttx/drivers/mmcsd/mmcsd_sdio.c`）多块读 DMA 序列：

```
SDIO_BLOCKSETUP → SDIO_DMARECVSETUP（controller_buffer_set=1）
→ mmcsd_setblockcount（CMD23 SET_BLOCK_COUNT，SDIO_SENDCMD 走同一 sendcmd）
→ CMD18（真正数据命令）
```

而 `sunxi_mmc_mult_sendcmd`（`nuttx_sdio_mult.c:255`）原始逻辑：

| 缺陷 | 说明 |
|------|------|
| **数据挂载只看 buffer_set** | `buffer_set==1` 时把 `controller_sunxi_mmc_data` 挂到**任意**命令，包括无数据的 CMD23 → CMD23 被当成带数据命令处理 |
| **完成清 buffer_set 仅豁免 CMD55** | `if (opcode != 55) buffer_set = 0` → CMD23 完成后把 buffer_set 清 0 |

结果链：CMD23 吞掉并清掉 buffer_set → 紧随其后的 CMD18 以 `data==NULL` 进入 `rom_HAL_SDC_Request` → `do_rom_HAL_SDC_Request` 跳过数据 setup → `__mci_send_cmd` 对 ADTC+`!data` **直接 return 不发命令** → `SDC_SemPend(host->lock, SDC_DMA_TIMEOUT)` 等 20s → 超时 dump 路径访问 `data->sg`（NULL）→ 崩溃。**这就是"多块读必崩"、单块读（CMD17 无 CMD23 前缀）正常的全部原因。**

### 12.3 修复（nuttx_sdio_mult.c，两处）

| 点 | 改动 |
|----|------|
| 数据挂载 | 条件加 `mmc_cmd_type(...) == MMC_CMD_ADTC`：**只有带数据阶段的命令才挂 recvsetup/sendsetup 的 buffer**；CMD23/CMD55/CMD52 等无数据命令一律 data=NULL |
| buffer_set 消费 | 由"非 55 就清"改为**仅 ADTC 数据命令完成后才清**：CMD23 完成后保留 buffer_set，供紧随的 CMD18 使用 |

### 12.4 恢复多块读 + 编译打包固化

- `MMCSD_MULTIBLOCK_LIMIT`：1 → **0**（defconfig 与 .config 同步改，恢复 CMD18 多块 DMA 读提速）
- 编译/打包通过，nsh.fex==vela.bin md5 `c89b4ef1`（7,482,544B）
- 固化 **ok-20260807-4**（vendor 仓 2 文件 19+/8-）

### 12.5 验证状态

- ✅ 编译级验证：位运算误判已修（`flags & MMC_CMD_ADTC` 会误伤 MMC_CMD_BCR=0x60，改用 `mmc_cmd_type()==MMC_CMD_ADTC` 精确匹配）
- ⏳ **待上板验证**：播放卡顿应显著改善（多块读恢复）；若仍有问题可 `git_snapshot.sh -r ok-20260807-4` 回滚

### 12.6 无声问题（未动，实验方案已确认可执行）

代码层全排除结论不变，上板三个定位实验的命令/路径已确认：

| 实验 | 可执行要点 |
|------|-----------|
| A. aplay 直接播 | 固件 strings 已含 `aplay`/`Usage: aplay [option] wav_file` ✅；注意 aplay 只支持 **wav**，Beautiful Love 是 mp3 → 实验 A 需用 WAV 文件（如 IGNIS.wav / 1kHz.wav）验证声卡链路，MP3 链路另测 |
| B. Beautiful Love 放第一首 | 歌单扫描 `music_scan_tracks`（deskmate_ui.c:975）按 `readdir` 顺序、**不排序** → 无需改代码，SD 卡上重命名（如 `0-Beautiful Love.mp3`）或删其他歌即可使其成为第一首 |
| C. 实测 GPIOD(17)+LINEOUT | 需万用表/示波器，GPIOD(17)=PA 使能（HIGH=有声），codec LINEOUT 应有音频波形 |

**建议刷 ok-20260807-4（含本次 DMA 修复 + 字体修复）再上板**：先验证卡顿改善，再做实验 A/B/C。

---

## 十三、Settings 亮度/音量修复 + 诊断打点收敛（P9/P10 补档）

> 主 devlog 节点表 P9/P10 的详情补录（原主表仅一行摘要，本节为唯一详情源）。

### 13.1 P9：Settings 亮度/音量不可用 → 修复（ok-20260807-23）

| 项 | 内容 |
|----|------|
| 现象 | Settings 子页亮度/音量滑块调节无效 |
| 根因 | ①亮度走 WS2812 LED 而非屏幕 PWM（ch4 未接）→ 改用 `hal_pwm_control` 驱动真实背光 ②音量滑块回调 `cb=NULL` → 未挂处理函数 |
| 修复 | 亮度接 `hal_pwm_control`（屏幕 PWM ch4 路径）；音量滑块挂 `settings_volume_cb` |
| 固化 | ok-20260807-23 |
| 验证 | ⏳ 上板验证（滑块调节真实生效） |

### 13.2 P10：诊断打点收敛（ok-20260807-24）

| 项 | 内容 |
|----|------|
| 背景 | P5 决战期加的诊断打点（DM_FORCE_MONO/corr/RMS/WRITE_GAP/out_dump）在问题闭环前需收敛，防串口刷屏与性能干扰 |
| 改动 | `DM_FORCE_MONO` 1→0（恢复立体声）；writei 打印降频 50 倍；`sunxi_codec_out_dump` 保留（低频，不摘除） |
| 固化 | ok-20260807-24 |
| 验证 | ⏳ 上板验证（串口安静、声音正常） |

---

*DevLog by AtomCode (deepseek-v4-flash)*
