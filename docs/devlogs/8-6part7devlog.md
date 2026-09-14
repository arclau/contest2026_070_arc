# DevLog 2026-08-06 (Part7) — 切歌无声排查：驱动层全线排除 + 音量新线索

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**
> 前置 Part：Part1~6（`8-6part1devlog.md`~`8-6part6devlog.md`），当前基线 tag **ok-0806-dma-residue**

---

## 一、任务背景

Part6 结论：切歌后再 aplay 报 `snd_vela_pcm_open_config failed (return: -16)` = **EBUSY 声卡设备未释放**，-16 来源已定位为 `ksnd_pcm_open`（snd_pcm.c:1477-1479 `ref_count != 0 → -EBUSY`）。用户选择先修**切歌无声**（核心痛点）。

本 part 通过三版诊断日志固件逐层排查，**驱动层（aw-tiny-alsa + snd_core + codec + DMA）全线排除**，同时获得**重大新线索：切歌后播放数据 peak 异常低（疑似音量被调低）**。

## 二、三轮诊断固件与日志结论

### 2.1 第一版：codec 层诊断（tag ok-0806-dm-audio-dbg）

**改动（仅加日志，零行为变更）**：

| 文件 | 日志点 |
|------|--------|
| `snd_pcm.c` | `ksnd_pcm_open` ref_count / `open_substream` / `release_substream` |
| `snd_core.c` | `soc_pcm_prepare` dapm_control(1) 前后 / `soc_pcm_close` dapm_control(0) 前后 |
| `sun8iw20-codec.c` | dapm_control 入口（stream/onoff/dapm_state/route）/ lineout_route / hp_route / trigger |
| `sunxi-daudio.c` | daudio trigger（cmd/playback_en）/ PA enable/disable |

**结论（2026-08-06-music-testlog 第一版）**：
- **codec dapm/PA/trigger 调用序列 Aurora（有声）与切歌后（无声）完全一致**：dapm_state 每次 open 强制清零（18 号修复生效）、`hp_route spk=1 onoff=1`（PA GPIO 拉高）每次都执行、trigger START 每次都到达 codec
- **日志全程无 `daudio`/`PA` 字样** → **audiocodec 卡不走 sunxi-daudio 平台**（PA 由 codec 内 gpio_spk 控制），daudio 日志点实际无效
- 18 号 dapm_state 修复确认已生效但不足以解决切歌无声 → 排除 codec 使能层

### 2.2 第二版：DMA 层诊断（tag ok-0806-dma-dbg）

**改动（仅加日志）**：

| 文件 | 日志点 |
|------|--------|
| `sunxi-pcm.c` | pcm open/close（DMA 通道指针）/ hw_params（rate/fmt/dst_addr/drq）/ trigger |
| `snd_dma.c` | dmaengine open_request_chan / close_release_chan / prep_cyclic+submit / trigger |
| `dma_wrap.h` | `hal_dma_chan_request` / `hal_dma_chan_free` / `hal_dma_prep_cyclic` / `hal_dma_slave_config` / `hal_dma_start` 状态码 |

**结论（2026-08-06-music-testlog 第二版）**：
- **DMA 层全部成功**：`hal_dma_chan_request status=1`、`slave_config status=0`、`prep_cyclic status=0`、`hal_dma_start status=0` —— 切歌 reopen 后 DMA 通道正常重建并启动
- Aurora 与 Beautiful Love 的**驱动调用序列 100% 一致**（open → hw_params → prepare → dapm → trigger START → DMA 全成功 → writei）

### 2.3 第三版：pointer residue + DAC_FIFOC（tag ok-0806-dma-residue）

**改动（仅加日志）**：
- `snd_dma.c` `snd_dmaengine_pcm_pointer`：每次查询打印 `dma pointer status/residue`（**DMA 是否真的在搬运数据的铁证**）
- `sun8iw20-codec.c` `sunxi_codec_hw_params`：打印 rate/fmt/ch + `DAC_FIFOC` 寄存器读回（S16/rate_bit）

**结论（2026-08-06-music-testlog 第三版，1047 行）**：
- **DMA residue 循环变化（0x2000→0x1800→0x1000→0x800→0x2000）= DMA 确实在搬运数据到 codec FIFO**，切歌后 Beautiful Love 播放时同样如此 → 数据在流动
- `codec DAC_FIFOC=0x3004000`，44100 与 48000 的 rate_bit 均为 0（`sample_rate_conv[]` 表 `{44100,0},{48000,0}`）→ 采样率字段未区分，但 Aurora 48000 有声 → 非根因

## 三、🔴 决定性新线索：writei peak 差异 + 用户操作序列

### 3.1 用户操作序列（关键输入）

> "我按播放键，有声音；我按暂停，音乐暂停；我再继续播放还是有声音（因为没切歌）；**当我一按暂停，再按下一首歌切歌，就没有声音了**。"

- 暂停 → 继续（同一首歌）：**有声** ✅（cmd=3 PAUSE_PUSH 被 sunxi_pcm_trigger 映射成 dmaengine STOP，恢复时 cmd=4 PAUSE_RELEASE 映射成 START → 重新 prep_cyclic+start，路径正常）
- 暂停 → 切歌（XPlayerReset）：**无声** ❌
- 用户确认：切歌后为**完全无声**（非小声）

### 3.2 writei peak 值对比（衰减后）

| 歌曲 | writei peak（衰减后） | 原始估算（÷0.3） |
|------|---------------------|-----------------|
| Aurora（有声） | 1392 ~ 4448 | ~4640 ~ 14800（正常响度） |
| **Beautiful Love（无声）** | **43 ~ 172** | **~143 ~ 573（-35~-47dB，极低！）** |
| IGNIS（Part6，无声） | 33 ~ 1771 | ~110 ~ 5900 |

- nz 计数正常（3639/3840 ≈ 满）→ **数据非静音，但幅度极小**
- Beautiful Love 的 peak 比 Aurora 低 10~30 倍 → **怀疑切歌后 XPlayer 内部把音量调到最低 / 数据被衰减**

### 3.3 日志证据补充

- 切歌时 XPlayerReset 前出现 `cmd=4 (PAUSE_RELEASE)`（恢复暂停的流）→ 映射成 DMA START → 又 `cmd=0 (STOP)` → 363 close → **完整释放链（ref_count=0、hal_dma_chan_free 都正常）** → 切歌过程本身无设备占用残留
- EBUSY 仅在"退出播放器后最后一条流未 close"时出现（Part6 已证），与切歌无声是**两个独立问题**

## 四、驱动层排除汇总（本 part 铁证）

| 层 | 检查项 | 结果 |
|----|--------|------|
| 用户态 close 链 | `release_substream ref_count=0` | ✅ 每次切歌都干净释放 |
| codec dapm | dapm_control(1) / route=5 / PA GPIO | ✅ 每次都执行（18 号修复生效） |
| codec trigger | START/STOP 到达 | ✅ 每次都到达 |
| DMA 通道 | chan_request / slave_config / prep_cyclic / start | ✅ 全部 status=0 |
| DMA 搬运 | pointer residue 循环变化 | ✅ 数据确实流入 codec FIFO |
| 数据 | writei 返回 wr=1920、nz≈满 | ✅ 有数据，**但 peak 极低** |

**结论：切歌无声 ≠ 驱动问题。唯一可见差异是 writei 数据 peak 异常低（音量问题），根因大概率在 XPlayer/awplayer 上层（切歌时把音量调到最低 / 重采样增益 / 音量状态残留）。**

## 五、文件改动汇总（本轮三版，均为诊断日志，固化三个 tag）

| tag | 改动文件 | 说明 |
|-----|---------|------|
| ok-0806-dm-audio-dbg | snd_pcm.c / snd_core.c / sun8iw20-codec.c / sunxi-daudio.c | codec/dapm/PA/trigger 诊断 |
| ok-0806-dma-dbg | sunxi-pcm.c / snd_dma.c / dma_wrap.h | DMA 通道/配置/启动诊断 |
| ok-0806-dma-residue | snd_dma.c（pointer residue）/ sun8iw20-codec.c（DAC_FIFOC） | 搬运验证 + 寄存器读回 |

验证命令：`./build.sh <nsh 配置> -j$(nproc)` → `lunch_nuttx 2 && pack` → `md5sum nsh.fex vela.bin` 一致（各 7,482,544 字节）。

## 六、遗留事项 / 下一步

1. **🔴 验证"切歌音量被调低"假设（最高优先）**：
   - 检查 XPlayer/awplayer 是否在切歌（XPlayerReset）时把音量/增益调到最低（`XPlayerSetVolume`/`mSpeed`/`SetPlaybackRate` 相关，xplayer.c:703-707 `mSpeed==0 → XPlayerPause`）
   - 在 `dm_sound_write` 打印**衰减前**原始 peak（当前 peak 是 30% 软件衰减后统计）→ 区分"数据本身小" vs "衰减/增益问题"
   - 对比 Aurora（有声）与 Beautiful Love（无声）的解码输出原始峰值
2. **🔴 播放器音量 UI（用户需求）**：播放器右侧增加**音量加/减按钮**，中间显示**音量数值**（当前无 SetVolume 通道，需先确认 XPlayer/awalsa 音量接口）
3. 切歌无声若确认是音量问题：修复音量状态残留后大概率解决；若仍无声再回查驱动
4. 遗留不变：EBUSY（退出后未 close，app 层）、卡顿（SDMMC 多块读 DMA bug）、bt_recv 50% 空转、UI 模拟器同步暂停中

---

*DevLog by AtomCode (deepseek-v4-flash)*
