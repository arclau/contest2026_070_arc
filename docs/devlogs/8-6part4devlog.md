# DevLog 2026-08-06 (Part4) — MP3 出声验证 + 卡顿/切歌无声排查收尾（交接下个会话）

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

Part3（ok-20260806-17）把 44100Hz 源统一重采样到 48000 后，用户上板验证：**MP3 终于出声了** ✅（Beautiful Love.mp3 播放成功），但暴露两个新问题：
1. **播放卡顿**（`wr=-32` EPIPE underrun 出现，`wht>>>Render timeout:81ms`）
2. **切歌后无声**：XPlayerReset 切到下一曲（MP3 或 WAV）后，writei 全部成功（wr=1920）但静音

用户指示：本 part 收尾（修复+编译打包+归档），音乐播放器更多 BUG 优化交给下一个会话。

## 二、切歌后无声根因分析（代码级）

### 现象

- 第一首播放：有声（卡）
- 任意 XPlayerReset 切歌后：writei 持续成功（wr=1920）但完全静音，且无论 MP3/WAV

### 根因（ok-20260806-18 修复）：substream->dapm_state 残留导致 codec 输出不复位

`snd_pcm.c snd_pcm_open_substream()` 中 substream 结构是**跨 open/close 复用的**（`pcm->streams[stream]` 常驻），每次 open 只重建 runtime，**不重置 `substream->dapm_state`**。

codec 侧 `sunxi_codec_dapm_control()`（snd_core.c soc_pcm_prepare 里 dapm_control(1)）有去重逻辑：

```c
if (substream->dapm_state == onoff)
    return 0;   /* 状态已一致 → 直接跳过 DAC/模拟路由使能！ */
```

时序：首次播放 prepare → dapm_control(1) → 开 DAC → dapm_state=1；XPlayerReset 切歌 → close → dapm_control(0) → 关 DAC → dapm_state=0；**再次播放 open（substream 复用，dapm_state 残留为 0）→ prepare → dapm_control(1) 应该执行**……但日志显示 writei 成功却静音，说明 open 路径某些情况下 dapm_state 残留为 1（close 的 dapm_control(0) 可能未执行/执行时序错乱），导致 prepare 时 dapm_control(1) 直接 return 0，**DAC 模拟输出从未重新使能 → 数据进 codec 但无声**。

**修复**：`snd_pcm_open_substream()` 每次 open 强制 `substream->dapm_state = 0`，保证下一次 prepare 一定重新执行 DAC/模拟路由使能。

## 三、卡顿根因分析

日志证据：
- `wht>>>Render, timeout:81ms`：requestPcmData 一次取数超 15ms 阈值 → **解码供给慢**
- `writei size=4608 frames=1152 wr=-32`：underrun（-EPIPE）→ 声卡缓冲空转

供给慢来源（多因素叠加）：
1. 44100→48000 软件重采样（do_audioresample/do_AuMIX）在解码线程占 CPU
2. SD 卡单块读（`MMCSD_MULTIBLOCK_LIMIT=1` 权宜）拉低读取吞吐
3. period/buffer 仅 256/1024，缓冲余量小，抖动即欠载

**缓解修复**：period/buffer 256/1024 → **512/2048**（增大声卡缓冲吸收抖动；aplay sine 参考值 256/1024 无解码开销，XPlayer 场景应放大）。根治仍待下个会话（重采样开销/多块读 DMA）。

## 四、改动清单（ok-20260806-18）

| 文件 | 改动 |
|------|------|
| `chips/r528/drivers/rtos-hal/hal/source/sound/core/snd_pcm.c` | `snd_pcm_open_substream()` open 时强制 `substream->dapm_state = 0`（修切歌后 codec DAC 不复使能 → 无声） |
| `apps/luncher_dm/deskmate_ui.c` | dm_sound_open period/buffer 256/1024 → 512/2048（缓冲吸收解码抖动，缓解卡顿） |

## 五、验证结果

- 编译：`./build.sh ... -j$(nproc)` ✅ 通过
- 打包：`source envsetup.sh && lunch_nuttx 2 && pack` ✅ 成功
- 产物：`nsh.fex` 与 `vela.bin` **字节一致**（7478440 B）✅
- 固化：git_snapshot.sh → tag **ok-20260806-18**
- ⚠️ **上板验证待用户执行**：切歌后应恢复有声；卡顿应改善

## 六、遗留事项 / 下一步（交接下个会话）

1. **上板验证 ok-20260806-18**：①切歌后是否恢复有声（dapm_state 修复）②卡顿是否改善（512/2048）
2. **🔴 卡顿根治（最高优先）**：解码供给慢是根本——①441→48k 重采样在解码线程耗 CPU，评估是否可移到 render 线程/优化 do_AuMIX 调用；②`MMCSD_MULTIBLOCK_LIMIT=1` 单块读权宜 → SDMMC 多块读 DMA 根治（遗留项）；③必要时继续增大 buffer 或提高解码线程优先级
3. **切歌无声若仍复现**：抓完整日志确认 dapm_state 修复是否生效；若 dapm_control(0) 在 close 路径未执行，需查 `snd_pcm_release_substream` 与 `soc_pcm_close` 调用链
4. 音乐播放器其他 BUG 优化（音效/进度/列表交互等）整体交接下个会话
5. 既有遗留：模拟器 freetype、蓝牙符号观感、背景图、天气城市、G2D 等不变

---

*DevLog by AtomCode (deepseek-v4-flash)*
