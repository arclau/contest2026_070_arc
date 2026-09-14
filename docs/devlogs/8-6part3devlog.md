# DevLog 2026-08-06 (Part3) — MP3 无声根治：44100→48000 重采样 + fd:// seek 误报修复

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

Part2（ok-20260806-16）已修复 24bit WAV 无声（writei 字节账目 + 位深传递），上板验证 IGNIS.wav 出声 ✅。但用户反馈两个新问题：

1. **MP3（Aurora Borealis.mp3，44100Hz）仍无声**——writei 持续成功（wr=1764）但静音
2. **WAV 播完切下一曲（MP3）后无声**——与问题 1 同根因

## 二、根因分析

### 根因 1（MP3 无声）：44100Hz 采样率链路未经验证，实际不出声

对比日志：

| 文件 | 采样率 | writei | 结果 |
|------|--------|--------|------|
| IGNIS.wav (24bit) | 48000 | size=7680 frames=1920 wr=1920 | ✅ 有声 |
| Aurora Borealis.mp3 | 44100 | size=7056 frames=1764 wr=1764 | ❌ 无声 |

数据尺寸、帧数、writei 返回值全部正常（7056=1764×4 字节 16bit 立体声，40ms@44100=1764 帧），但无声。**aplay sine 测试只验证过 48000Hz（`-r 48000`）**，从未验证 44100Hz——结合 codec `sample_rate_conv` 中 44100 与 48000 映射相同的 rate_bit、daudio 时钟 22579200 vs 24576000 两条分频链路，判断 **44100Hz 在 R528 codec/daudio 链路实际不出声**（硬件/驱动层未支持，或需不同时钟配置）。

**修复方案（走已验证路径）**：adecoder 已有重采样到 48000 的能力（`do_audioresample`，原仅对 `<RATE_LIMIT(32000)` 或单声道触发）→ **把 44100Hz 也纳入重采样到 DEFUALT_RATE=48000**，让 MP3 以 48000Hz 输出，走与 WAV 完全相同的已验证出声路径。重采样后 `out_samplerate=48000` 同步更新，checkSampleRate/dm_sound 侧自动跟随，无需改 UI 层。

### 根因 2（日志误报）：fd:// reopen 的 seek 检查 `ret == 0` 恒误报

`CdxFileStream.c` fd:// 分支（ok-2 大 ID3 tag reopen 路径）：`vfs_lseek` 成功返回**新偏移**（非 0），原检查 `ret == 0` 对任何非 0 offset 都误报 "fd seek errno(11)"（errno 是残留 EAGAIN）。seek 实际成功，但日志刷屏且掩盖真实错误。

**修复**：改为 `ret < 0` 才算失败（vfs_lseek 失败返回 (uint32_t)-1），失败才 goto failure。

## 三、改动清单

| 文件 | 改动 |
|------|------|
| `chips/r528/components/multimedia/src/libcedarx/decoding/adecoder.c` | 重采样条件增加 `Samplerate == 44100`：44100Hz 源（MP3 等）统一重采样到 48000 输出（走已验证出声路径） |
| `chips/r528/components/multimedia/src/libcedarx/libcore/stream/file/CdxFileStream.c` | fd:// reopen 的 seek 检查 `ret == 0` → `ret < 0`（消除非 0 偏移误报，真实失败才报错） |

## 四、验证结果

- 编译：`./build.sh ... -j$(nproc)` ✅ 通过
- 打包：`source envsetup.sh && lunch_nuttx 2 && pack` ✅ 成功
- 产物：`nsh.fex` 与 `vela.bin` **字节一致**（7478440 B）✅
- 固化：git_snapshot.sh → tag **ok-20260806-17**
- ⚠️ **上板验证待用户执行**：播放 Aurora Borealis.mp3 应出声（重采样后 48000Hz）；若仍无声，需用 `aplay -s -r 44100` 判别硬件层 44100 支持

## 五、遗留事项 / 下一步

1. **上板验证 MP3 有声**（最高优先）：ok-20260806-17 固件播放 Aurora Borealis.mp3
   - 若仍无声 → `aplay -D hw:audiocodec -s -r 44100 -c 2 -f 16 -t 5` 判别：aplay 也无声=硬件/驱动 44100 链路问题（深挖 daudio 时钟/ASRC）；aplay 有声=XPlayer 侧另查
2. SDMMC 多块读 DMA 根治（遗留）
3. 模拟器 freetype（遗留）
4. 其余 Part1/Part2 遗留项不变

---

*DevLog by AtomCode (deepseek-v4-flash)*
