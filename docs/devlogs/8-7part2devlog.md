# DevLog 2026-08-07 Part2 — 切歌/连播无声：歌曲级差异定位（供外部 AI 独立分析）

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 本文档目的：**提供给第三方 AI（ChatGPT 等）做独立根因分析的完整背景报告**，包含工程背景、需求、音频软件链路全景、两轮排查的证据、当前核心矛盾与待研判的问题假设。所有结论均基于上板实测日志（`2026-08-06-music-testlog.txt` 与本文件附录日志），非推测。
> 前置阅读：`AGENTS.md`（环境/编译/固化规则）、`8-7devlog.md`（Part1：音量链路排除→死机修复→codec 输出深挖→字体修复→上层解码审查）

---

## 一、工程与硬件背景

| 项 | 值 |
|----|-----|
| 工程根 | `/data/dm`（openvela，R528 SoC） |
| 目标配置 | `vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh` |
| 屏幕 | BOE 1200x1920 MIPI DSI 屏（横屏 1920x1200 使用），驱动层 G2D 旋转 |
| 触摸 | GT9271（I2C），全屏子页面测试通过 |
| 音频 codec | R528 内置 `sun8iw20-codec`（audiocodec），`pb_audio_route = LO_HP_SPK`（LINEOUT + HP 双路 + 外部功放） |
| 外部功放使能脚 | `GPIOD(17)`，`pa_level = GPIO_DATA_HIGH`，`pa_msleep = 20ms` |
| 固件播放器 | libcedarx `XPlayer`（mp3/flac/ogg/aac/wav），异步状态机 |
| 音频 sink | 自研 `SoundCtrl`（基于 SDK 的 `aw-tiny-alsa-lib`，即 `snd_vela_pcm_*` API） |

固化点基线：`ok-20260807-11`（md5 `6ecf0bdf`，nsh.fex == vela.bin 7,482,544B），含本次新增的三项闭环诊断打点（见 §6）。

---

## 二、需求 / 问题定义

**一句话**：播放器第一首（Aurora）有声；**切歌后（Beautiful Love / laojie / IGNIS）无声**；自动连播（一首放完自动接下一首）同样无声。该问题已持续排查两天，代码层全链路已排除，最新一轮实验将范围收窄到**歌曲级差异**。

**问题的两个关键观察维度**：

1. **位置无关**：laojie.mp3 作为第一首播放也无声；Beautiful Love 作为第一首时，用户从歌单点击 Aurora 却有声音 → **排除"第二首位置残留"假设**。
2. **操作方式无关**：手动切歌无声、自动连播无声、直接点击歌单里某一首（非第一首）播放 Aurora 有声 → **与 XPlayerReset 切歌路径无关**。

**结论**：问题与**具体歌曲**强相关（Aurora 有声，Beautiful Love / laojie 无声），与播放位置、切歌操作、自动连播均无关。

---

## 三、音频软件链路全景（从应用到寄存器）

```
┌─ 应用层（apps/luncher_dm/deskmate_ui.c，板端唯一 UI 源）
│    music_play() → music_audio_start(path)
│    → XPlayerSetDataSourceUrl → PrepareAsync → Start（异步）
│    → XPlayer 回调线程置 g_xp_state，LVGL 定时器轮询驱动 UI
│
├─ 播放内核（vendor/.../libcedarx）
│    xplayer.c        — XPlayerReset / SetDataSource / 状态机
│    audioRenderComponent.c — handleStart/handleStop/doRender/writeToSoundDevice/checkSampleRate
│    adecoder.c       — 44100→48000 重采样（do_audioresample）
│
├─ 音频 sink（deskmate_ui.c 内自研 SoundCtrl）
│    dm_sound_open    — snd_vela_pcm_open("hw:audiocodec")，period=512 buffer=2048，S16_LE，SW start_threshold=buffer
│    dm_sound_write   — 24/32bit→16bit 降转 + 软件音量衰减(0-100, 默认30) + snd_vela_pcm_writei
│    dm_sound_stop/reset — drain + close + 清 handle
│
├─ aw-tiny-alsa-lib（hal/source/sound/component/aw-tiny-alsa-lib/）
│    pcm.c      — snd_vela_pcm_open/close/writei/state
│    pcm_hw.c   — hw_close → ksnd_pcm_release
│
├─ NuttX 侧 snd 核心（hal/source/sound/core/）
│    snd_pcm.c   — ksnd_pcm_open（ref_count 计数，非 0 即 -EBUSY）、open/release_substream、dapm_state 强制清零
│    snd_core.c  — soc_pcm_prepare（dapm on）/ soc_pcm_trigger / soc_pcm_close（dapm off + shutdown）
│    snd_dma.c   — snd_dmaengine_pcm_trigger：START→prep_cyclic+submit+issue；STOP→terminate_async；pointer→tx_status residue
│
├─ DMA（dma_wrap.h → hal_dma.c）
│    hal_dma_prep_cyclic / hal_dma_start / hal_dma_stop（PAUSE→STOP→RESUME，cyclic=false，desc 不释放）
│
├─ platform（hal/source/sound/platform/sunxi-daudio.c）— 本工程实际**不走** daudio（audiocodec 自带 FIFO+DMA 直连）
│
└─ codec（hal/source/sound/codecs/sun8iw20-codec.c）
     sunxi_codec_dapm_control — dapm on/off：DAC 数字/模拟使能 + LINEOUT/HP 路由 + **GPIO PA 拉高**
     sunxi_codec_trigger — START/STOP：DAC_DRQ_EN 置位/清零
     sunxi_codec_out_dump — 低频诊断打点（dapm on/off + trigger 各 cmd），读 DPC/DAC_ANA/HP_ANA/DAC_REG/DAC_FIFOC/FIFOS/CNT/PA(物理回读)
```

---

## 四、第一轮排查（Part1，代码层全链路排除）——证据汇总

### 4.1 上层解码链（零代码改动，结论：无状态残留）

| 疑点 | 结论 |
|------|------|
| XPlayer 切歌时调低音量 | ❌ 证伪：XPlayer 链无任何音量 API（XPlayerReset/SetPlaybackSettings/SetSpeed 均不碰音量） |
| 切歌后 audioRender 状态残留 | ❌ 无：handleStart 从 STOPPED 恢复时重置全部（nSampleRate/nChannelNum/needDirectOut/bFirstFrameSend=0） |
| checkSampleRate 重配时序 | ❌ 无：每次切歌正常 Stop→SetFormat→Start，日志确认 |
| adecoder 重采样状态 | ❌ 无：44100→48000 走 do_audioresample，各歌完全一致 |

### 4.2 音量链路（上板实测）

- 软件衰减 0-100 可调，**衰减比随音量精确 1:1**（vol20→0.15、60→0.60、90→0.90、100→1.00）
- 数据满幅：Beautiful Love raw_peak 16→5984（渐入后正常），音量 100 时 peak=30498（≈93% 满幅）

### 4.3 codec 输出链路（上板实测寄存器逐位对比）

`sunxi_codec_out_dump` 低频打点（dapm on/off + trigger），Aurora 与 Beautiful Love 播放时**逐位一致**：

| 点 | DPC | DAC_ANA | HP_ANA | DAC_REG | DAC_FIFOC | PA |
|----|-----|---------|--------|---------|-----------|-----|
| Aurora dapm on | 0x80000000 | 0x15fc7a | 0x36404000 | 0x15fc7a | 0x3004000 | 1 |
| Beautiful Love dapm on | 0x80000000 | 0x15fc7a | 0x36404000 | 0x15fc7a | 0x3004000 | 1 |

- EN_DAC=1（bit31）、DACLEN/DACREN=1、LINEOUTL_EN/LINEOUTR_EN=1、PA=1（`hal_gpio_get_data` **物理回读**，非寄存器写入值）
- **驱动输出链路切歌前后零差异**

---

## 五、第二轮实验（Part2，今天）——决定性转折

### 5.1 实验 A：aplay 直接播 WAV（绕开 XPlayer）

```
aplay /sdcard/music/IGNIS.wav
```
结果：**有声音，但是是杂音**（用户原话："有声音，，但是是杂音，吓我一跳"）。

附带日志：
```
[codec] dapm on:  ... CNT=0(delta=0) PA=1
[codec] START:    ... CNT=0(delta=0) PA=1
[codec] STOP:     ... CNT=34816(delta=34816) PA=1
snd_pcm_writei return -32        ← -EPIPE（underrun）
Xrun...
prepare:0x4143c605
[codec] START:    ... CNT=0(delta=0) PA=1
```

**解读**：
1. **物理链路（DAC→LINEOUT→功放→喇叭）确认 OK**——aplay 路径能出声音，这是两天来最重要的正面证据。
2. aplay 播 WAV 出现 **underrun（-EPIPE）** 且只播放了约 0.7s（CNT=34816/48000≈0.73s）就被 Xrun 打断 → aplay 数据供给断流（嫌疑：SDMMC 读卡 / WAV 解析 / buffer 配置），且**播放的是杂音而非正常音乐**。

### 5.2 实验 B：aplay 之后立即用播放器播 laojie.mp3 → EBUSY

```
[music] xplayer play: /data/music/laojie.mp3
...
[AWALSA_ERR][snd_vela_pcm_open:350]pcm "hw:audiocodec": snd_pcm_open_config failed (return: -16)
```

**解读**：aplay 播完后声卡设备**未释放**（`snd_vela_pcm_open` 返回 -16 = EBUSY，来自 `ksnd_pcm_open` 的 `ref_count != 0` 检查）。**此 EBUSY 是 aplay 路径残留，污染了"紧接着测试 laojie"的结果——laojie 那次无声是因为声卡根本没打开，不是歌的问题**。EBUSY 与切歌无声是两个独立问题（EBUSY 仅在 aplay 退出后流未 close 时出现）。

### 5.3 实验 C：歌曲级差异（重启后纯播放器重测，无 aplay 干扰）——**决定性**

| 播放场景 | 结果 |
|----------|------|
| Aurora 第一首 | ✅ 有声 |
| 切到 Beautiful Love / laojie | ❌ 无声 |
| 自动连播到下一首 | ❌ 无声 |
| **laojie.mp3 作为第一首（重启后直接播）** | ❌ **无声**（与位置无关！） |
| Beautiful Love 作为第一首，用户进播放器**不点播放按钮**，从歌单点击 Aurora | ✅ **有声**（与切歌操作无关！） |

**结论**：问题与**歌曲本身**强相关——Aurora 有声，Beautiful Love / laojie 无声；与播放位置、切歌方式、自动连播完全无关。

### 5.4 硬件测量

- 用户实测：播放时 GPIOD(17)（功放 ctrl）为**高电平，约 1.5V-1.7V**；有声无声时均为高电平。
- ⚠️ **注意**：1.5-1.7V 明显低于 3.3V 逻辑高电平。此值需要关注：是 GPIO bank 供电电压（若 1.8V bank 则正常）？还是开漏/弱驱动被功放输入拉低？若功放 EN 阈值高于此值，功放可能未完全开启 → 这一点作为**待确认物理疑点**（用户无示波器，未测 LINEOUT 波形与功放输出）。

---

## 六、第二轮的代码层诊断增强（ok-20260807-11，本轮新增三个闭环证据点）

| 证据点 | 改动 | 目的 |
|--------|------|------|
| ① writei 返回值 + PCM state | `deskmate_ui.c` dm_sound_write：非 -EPIPE 负值不再静默，立即打 `[music] writei ERR wr=%d state=%d`；正常日志加 `state=` | 抓"数据没进声卡"（EBUSY/-EBADFD 曾全被 `if (wr<0) return 0` 吞掉） |
| ② DAC 消费状态 | `sun8iw20-codec.c` out_dump 加 `FIFOS(TXE)` + `CNT(delta)`（DAC_CNT 为只读 TX 帧计数，trigger START→STOP 的 delta = 本次播放进入 DAC 的帧数） | **判断数据是否真进 DAC** |
| ③ trigger 命令名 | out_dump 的 tag 从 "trigger" 改为 START/STOP/PAUSE_PUSH/PAUSE_RELEASE 等 | 暂停续播 vs 切歌序列可直接对比 |

### 6.1 证据点①结果：writei 全成功，无任何错误

切歌后（Beautiful Love / laojie 播放）所有日志：
```
[music] writei size=7680 bits=16 ch=2 frames=1920 wr=1920 state=3 raw_peak=... peak=... nz=...
```
- `wr=1920`（满帧写入）、`state=3`（RUNNING）、**全程零 `writei ERR`** → 数据确实进入了声卡设备，EBUSY/-EBADFD 方向排除。

### 6.2 证据点②结果：DAC_CNT 满速增长 —— 数据确实进了 DAC！

| 歌曲 | 段落 | STOP 时 CNT (delta) |
|------|------|---------------------|
| Aurora（第一首） | 段1 | 211968 (delta=211968) |
| Aurora | 段2 | 215040 (delta=215040) |
| Beautiful Love（切歌后） | 段1 | 215040 (delta=215040) |
| Beautiful Love | 段2 | 211968 (delta=211968) |
| laojie（切歌后） | 段1 | 215040 (delta=215040) |

- delta ≈ 21 万帧 ≈ 4.5 秒 @48kHz 的数据量，与每段播放时长吻合 → **DAC 数字侧在满速消费数据**。
- TXE（TX Empty）在播放中为 0（FIFO 非空、有数据在出），START 瞬间为 1（刚启动尚未填充，正常）。

### 6.3 证据点③结果：暂停续播 vs 切歌，寄存器逐位一致

暂停续播（PAUSE_PUSH→PAUSE_RELEASE）与切歌（STOP→dapm off→dapm on→START）的 DPC/DAC_ANA/HP_ANA/DAC_REG/DAC_FIFOC/FIFOS/PA 全部一致，PA=1 全程（物理回读）→ 驱动输出零差异。

---

## 七、歌曲文件分析（本机 laojie.mp3）

用 Python 解析 MP3 帧头（本机 `/data/dm/laojie.mp3`，与板端同 md5 `5113303e...`）：

| 项 | 值 |
|----|-----|
| 文件大小 | 5,478,893 B（含 ID3v2 标签 377,147 B） |
| 格式 | MPEG1 Layer III，12206 帧 |
| 采样率 | 44100 Hz（全部帧一致） |
| 码率 | 128 kbps CBR（全部帧一致） |
| 声道模式 | joint stereo（12205 帧）/ stereo（1 帧） |
| 估算时长 | ≈318 秒 |

**文件本身完全正常**（标准 44.1kHz/128kbps CBR joint-stereo MP3）。无 Aurora.mp3 / Beautiful Love.mp3 的本地副本可对比（在板端 SD 卡上）。

---

## 八、当前核心矛盾（供外部 AI 研判的核心）

**矛盾点**：对 Beautiful Love / laojie 播放，以下所有链路证据均指向"应该有声音"，但用户实测无声：

1. 数据满幅：writei raw_peak 数千（Beautiful Love 5603 / laojie 3446~6992，衰减后 peak 约 1000-2100）——非静音数据
2. writei 全部成功：wr=1920，state=3（RUNNING），零错误
3. DAC 在消费：DAC_CNT 满速增长（每段 21 万帧 ≈ 4.5s）
4. codec 寄存器逐位一致：EN_DAC / DACLEN/REN / LINEOUT_EN=1，与有声的 Aurora 完全相同
5. 功放使能：PA=1（GPIO 物理回读高电平）
6. 物理链路：aplay 播 WAV 有声音（虽是杂音）→ DAC→功放→喇叭链路通
7. **唯一差异在歌曲本身**：Aurora 有声，Beautiful Love / laojie 无声，与位置/操作无关

**即：同一套驱动、同一条链路、同样的"数据满速进 DAC"，为什么 Aurora 有声而另两首无声？**

---

## 九、可能的问题假设（按可能性排序，待研判）

### 假设 1：解码输出的 PCM 内容问题（最高嫌疑）
laojie / Beautiful Love 的解码输出可能包含**静音段、极低幅度段、或声道相位异常**（如左右声道反相/幅度失衡），在写满 16bit 满幅时 raw_peak 统计仍显"正常"（峰值可来自个别样本），但人耳听感接近无声。
- **验证**：ffprobe/ffmpeg 分析这两首歌的响度（loudness）、声道相关性；或把 MP3 转 WAV 在 PC 上播放对比；或板端 dump 解码原始 PCM 前 N 秒样本分布（raw_peak 只统计峰值，未统计 RMS/均值）。

### 假设 2：声道反相 / 相位抵消（硬件路径放大）
若歌曲为 joint stereo 且左右声道反相，而外部功放将 LINEOUT L/R 合并（或喇叭只接单端），**反相信号在模拟合并处抵消 → 无声**；Aurora 若声道正常则有声。raw_peak 统计绝对值，无法反映反相。
- **验证**：检查 laojie.mp3 L/R 相位相关性（PC 端）；板端改 `pb_audio_route` 只走单声道 LINEOUTL（关掉 R）试听；或用耳机直插 LINEOUT L/R 分别听。

### 假设 3：GPIOD(17) 电平 1.5-1.7V 不足以完全开启功放（物理疑点）
3.3V 逻辑高电平应为 ~3.3V；实测 1.5-1.7V 偏低。若功放 EN 输入阈值高于此值（或功放需要更高的使能电流），功放可能处于**半开/关闭**状态——但此假设与"Aurora 有声"矛盾，除非 Aurora 播放时电平更高（用户未分别测两首歌的电平）。
- **验证**：分别测 Aurora 播放 vs laojie 播放时的 GPIOD(17) 电压；查功放芯片 EN 阈值规格；用示波器/万用表 AC 档测 LINEOUT 模拟波形。

### 假设 4：aplay 的"杂音"暴露了 DAC 输出链路隐藏问题
aplay 播 IGNIS.wav 出声但**是杂音**（且 underrun）——若 IGNIS.wav 本身是正常音乐，则说明 **DAC 模拟输出级或时钟配置存在音质级异常**（例如 MCLK/BCLK 分频、DAC 采样率与数据速率不匹配、模拟滤波器配置），音乐信号被严重失真成"杂音"，而某些歌曲（Aurora）恰好落在可听范围内。
- **验证**：确认 IGNIS.wav 的格式（采样率/位深）与 aplay 参数；用 aplay 播 1kHz.wav（已知正弦）对比听感；检查 codec DAC_FS（采样率）寄存器配置与 MCLK 来源。

### 假设 5：软件音量衰减后的可听性（已部分排除）
衰减 30% 后 peak≈1000-2100（约 3-6% 满幅），若喇叭灵敏度低，可能"几乎听不到"。但此假设与用户此前"音量 100 时 peak=30498"（满幅）仍有声音测试部分矛盾——需确认音量 100 时 laojie/Beautiful Love 是否真无声。
- **验证**：将音量调到 100 播放 laojie，确认是否仍无声。

---

## 十、建议的下一步验证（按成本递增）

1. **PC 端分析歌曲**（零成本，最快）：ffprobe 检查 Aurora / Beautiful Love / laojie 的响度（mean_volume / max_volume）与声道相位（L/R 相关性），确认是否有静音/反相嫌疑。
2. **音量 100 试听 laojie**：确认高音量下是否仍完全无声（区分"数据小"vs"真无声"）。
3. **aplay 播 1kHz.wav**：对比正弦波听感（若正弦清晰 = DAC 时钟/输出级正常；若正弦也失真 = 输出级/时钟问题）。
4. **单声道路由测试**：改 `pb_audio_route` 为仅 LINEOUTL（或仅 HP），排除 L/R 相位抵消。
5. **实测 GPIOD(17) 在 Aurora vs laojie 播放时的电平**；有条件时用万用表 AC 档测 LINEOUT 波形。
6. **板端 dump 解码原始 PCM**：dm_sound_write 增加 RMS/均值统计（raw_peak 只统计峰值，加 raw_rms/raw_mean 判断是否为静音/直流/反相数据）。

---

## 十一、涉及文件与固化点

| 固化点 | 改动文件 | 说明 |
|--------|----------|------|
| ok-20260807-11 | `apps/luncher_dm/deskmate_ui.c` | 证据点①：writei 负值即时打印 + state；evidence 打点 |
| ok-20260807-11 | `hal/source/sound/codecs/sun8iw20-codec.c` | 证据点②③：out_dump 加 FIFOS/CNT/delta + trigger 命令名 |

验证命令：`./build.sh <nsh> -j$(nproc)` → `lunch_nuttx 2 && pack` → `md5sum nsh.fex vela.bin`（6ecf0bdf 一致）→ `bash /data/vela/git_snapshot.sh`（tag ok-20260807-11）。

---

## 十二、遗留事项

- aplay 播完后声卡未释放（EBUSY -16 残留）——独立于切歌无声，待修（嫌疑：aplay 路径 close 链 / `snd_vela_pcm_close` 的 `donot_close` 或 ref_count 未归零）。
- aplay 播 IGNIS.wav 为杂音 + underrun——DAC 输出链路音质疑点，待查。
- GPIOD(17) 电平 1.5-1.7V 偏低——物理疑点待确认。
- 无示波器：LINEOUT 波形、功放输出未实测。

---
*DevLog by AtomCode (deepseek-v4-flash)*
