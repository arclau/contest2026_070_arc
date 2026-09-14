# DevLog 2026-08-06 (Part1) — XPlayer 音乐播放器收尾：编译/UI/日志 + 无声问题排查

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

P8（2026-08-05）音乐播放器 XPlayer 集成：mp3/flac/ogg/aac/wav 统一走 libcedarx XPlayer + 自写 SoundCtrl（dm_sound）+ 真实进度 + 清单 UI 改造。当日代码已完成但编译卡在 `flacParserCtor undefined`，未打包未固化（固件仍 f3e10224）。

今日 Part1 工作分三块：
1. **编译收尾**：启用 flac/pls parser 源码 → 链接通过 → 打包固化（ok-20260806-1）
2. **功能修复**：播放按钮不播第一首、大 ID3 tag prepare 失败、播放位置记忆、settings 打开死机
3. **无声问题排查**（未解决，当前进行中）：XPlayer 播放 MP3/WAV 均无声，已定位到声卡启动层，仍在攻坚

---

## 二、编译收尾（P8 遗留，ok-20260806-1）

### 卡点与根因

`flacParserCtor undefined reference`：libcedarx 的 parser 预编译库（`libcdx_*_parser.a`）是 **RISC-V ELF64**，与 R528 ARM 架构不匹配不能链接 → parser 必须用源码编译（CHIP_CSRCS）。`CONFIG_FLAC_PARSER_ENABLE=y` 已开、`CdxParser.c` 的 `#ifdef` 引用生效，但 `Make.defs:106` 的 flac 源码行被注释。

### 三表一致性核对（一次性收口，不盲补）

核对 70 处 `ParserCtor` 引用 ↔ `.config` 各 `CONFIG_*_PARSER_ENABLE` ↔ `Make.defs` CHIP_CSRCS：

| 结论 | 项 |
|------|-----|
| **仅 2 个缺口** | FLAC（`CdxFlacParse.c`）、PLS（`CdxPlsParser.c`）——配置 y 但源码行被注释 |
| 已一致 ✅ | MP3/WAV/OGG/AAC/AVI/FLV/TS/MKV/MOV/ID3V2 源码行已启用 |
| 无问题 ✅ | ASF/MPG/AMR/ATRAC/HLS/PMP 配置 n，注释合理 |

### 改动（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `chips/r528/components/multimedia/src/libcedarx/Make.defs` | 启用 `CdxFlacParse.c` + `CdxPlsParser.c` 两行 CHIP_CSRCS；补 flac/pls 两个 CFLAGS include 路径 |
| `apps/luncher_dm/Makefile` | 补 flac/pls include 路径（deskmate_ui.c 侧） |

### 结果

- 编译一次通过，无新增 undefined（后续启用音频后暴露的 audiomix/opus 见第五节）
- 打包 ✅ `nsh.fex == vela.bin`，固化 tag **ok-20260806-1**

---

## 三、功能修复（ok-20260806-2 / ok-20260806-3 / ok-20260806-4）

### 3.1 播放按钮不播第一首（ok-20260806-2）

**现象**：上板日志确认 `music_play_pause_cb` 首次点击走了 `music_pause()`（按钮初始无 `LV_STATE_CHECKED`），从未触发播放。

**修复**（`deskmate_ui.c`）：
- 回调改按 `music_playing` 分流：播放中→暂停；已初始化未播放→恢复；从未播放→`music_play(music_track_id)` 播当前曲目
- 新增 `music_inited` 标志（`music_play()` 置位），区分"暂停可恢复"与"从未启动"

### 3.2 大 ID3 tag 的 mp3 prepare 失败（ok-20260806-2）

**现象**：Aurora Borealis.mp3 的 ID3 tag 达 518KB（超过 probe buffer 128KB）→ id3v2 parser 走 reopen 路径构造 `fd://18?offset=...` URL → `__FileStreamConnect` 只认 `file://` → "not file stream" → prepare 失败。

**修复**（`CdxFileStream.c`）：`__FileStreamConnect` 增加 `fd://<fd>?offset=<o>&length=<l>` 前缀支持——解析 fd 号/offset/length，复用已打开 fd、seek 到 offset，无需重新 open。

### 3.3 播放位置记忆（断电记忆，ok-20260806-3）

**需求（用户确认）**：老式 MP3 行为——只记曲目（不记进度秒），打开音乐页停在记忆曲目上、不自动播放；首次无记忆默认第一首。

**实现**（`deskmate_ui.c`）：
- `music_state_save/load`：读写 `/data/dm_music.state`（yaffs 持久分区，掉电不丢，比 SD 卡稳妥）
- 打开音乐页：`music_track_id = music_state_load()`，超范围 clamp 到 0
- 切歌：`music_play()` 统一入口即写（列表点击/自动切歌/上一首/下一首全覆盖）

### 3.4 settings 打开死机（ok-20260806-4）

**现象**：打开 Settings 页即死机，日志 `freetype_image_create_cb: FT_Load_Glyph error(0x14)` → `lv_cache_entry_get_data: Asserted (entry != NULL)`。

**根因**：Settings 页 `LV_SYMBOL_BLUETOOTH (U+F293)` 在 **fa-solid-900.ttf 中缺失**（cmap 实测 MISSING，其余 settings 符号均在），fallback 链解析出无效 glyph_index → `FT_Load_Glyph` 返回 0x14 → create 回调返回 false → `lv_cache_acquire_or_create` 返回 NULL entry → 断言崩溃。**LVGL freetype 对"字体缺字形"无占位降级，任何缺字形字体都会死机**。

**修复**（`apps/graphics/lvgl/.../lv_freetype_image.c`）：`freetype_image_create_cb` 中 `FT_Load_Glyph`/`FT_Render_Glyph` 失败时不再返回 false，改为返回 1×1 空白占位 bitmap（true）——缺字形降级为空白而非崩溃。

---

## 四、XPlayer 音频路径启用（ok-20260806-4，链接面补齐）

启用音频后链接报缺符号，逐一补齐：

| 缺失 | 处理 |
|------|------|
| `do_AuMIX` / `Init_ResampleInfo` / `Destroy_ResampleInfo` | `Make.defs` 启用 audiomix 源码 `audiomix.c`（原被注释） |
| `AudioOpusDecInit` / `AudioOpusDecExit` | `Make.defs` 启用 `-law_opusdec`（原被注释） |

同时修 `player.c` 的 `CONFIG_DISABLE_AUDIO (1)→(0)`（**根因：硬编码禁用音频路径**——`PlayerSetAudioStreamInfo` 静默 `return 0`，`pAudioRender` 从未创建 → `PlayerHasAudio==0` → "neither video nor audio stream can be played"。WAV 此前靠 nxplayer 播，XPlayer 音频路径从未真正工作过）。

---

## 五、无声问题排查（核心攻坚，当前进行中 ⚠️）

### 5.1 现象

XPlayer 播放 MP3/WAV 均无声（用户反馈），但 f3e10224 时代 nxplayer 播 WAV 有声。

### 5.2 排查历程

| 阶段 | 假设 | 验证结果 |
|------|------|---------|
| A | 24bit WAV 格式错位（`nBitsPerSample=24` → S16_LE） | `dm_sound_open` 加 S24_LE 映射——**无效**（XPlayer 传的 bit 恒 16，分支从未触发） |
| B | 声卡设备名错误 | ✅ **排除**：`hw:audiocodec` = SDK 默认 `CONFIG_AW_AUDIO_CODEC_DEFAULT_CARDNAME` |
| C | aplay 判别 | aplay 播 IGNIS.wav 报 `format invalid`——**aplay 自己解析 24bit WAV 头失败**（channels/rate/bits 全 0），非声卡问题 |
| D | aplay sine 测试（关键） | **`aplay -D hw:audiocodec -s -r 48000 -c 2 -f 16 -t 5` 有声音！** → 声卡硬件/驱动/功放/aw-tiny-alsa 全链路正常，问题 100% 在 XPlayer/dm_sound 对接 |
| E | period/buffer 与参考不符 | sunxi_alsa 参考用 256/1024，dm_sound 用 1024/4096 → 改为 256/1024（ok-20260806-11） |
| F | **缺 sw_params（根因突破）** | aplay 参考 `set_param` 设置 `start_threshold=buffer_size` 等 sw_params，dm_sound **完全没设置** → 默认 start_threshold 可能永不触发 → writei 成功但 codec 不启动。补 sw_params 后（ok-20260806-13）日志出现 `wr=1920` 成功——**声卡终于启动** |
| G | underrun（-EPIPE） | 启动后 `wr=-32 (EPIPE)`：解码供给慢（`doRender timeout:37ms`）+ 无 xrun 恢复 → dm_sound_write 遇 -EPIPE 调 `snd_vela_pcm_prepare` 恢复（ok-20260806-14） |
| H | **当前状态（最新日志）** | writei 持续成功（`wr=1920/144/19/235` 均 >0，无 -32），但**依然无声**；`checkSampleRate` 出现 `needDirect 0→1`、`bitPerSample 16→16` 变化 → 触发 Stop+SetFormat+Start 重配 |

### 5.3 最新日志关键行（08:00:57 播放 IGNIS.wav）

```
writei size=7680 bits=16 ch=2 frames=1920 wr=1920   ← 声卡已启动且写入成功
checkSampleRate: needDirect 0→1, bitPerSample 16→16 → Stop+SetFormat+Start
writei ... wr=144 / wr=19 ...（持续成功，无欠载）
```

### 5.4 遗留疑点（下一步排查方向）

1. **writei 成功但无声**：数据已进声卡（wr>0）且硬件能响（sine 有声），矛盾点集中在 **XPlayer 送入的 PCM 数据内容/格式** 与声卡实际配置的匹配
2. **`bits=16` 始终未变 24**：IGNIS.wav 是 24bit，但 `dm_sound` 的 `sc->bits` 恒 16——`checkSampleRate` 在 `requestPcmData` 之前调用、首次 `p->cfg` 未填（nBitpersample=0→16）；且 adecoder 对普通 PCM（`modeflag==0`）原本不填 `raw_data.nBitpersample`（已修 ok-20260806-12）
3. **`needDirect 0→1` 触发重配**：checkSampleRate 检测到 direct 标志变化 → Stop/SetFormat/Start，期间可能造成播放中断/状态异常
4. **声道/采样率**：`sample rate change from 48000 to 48000`、`channel 2→2` 显示值相同也触发（疑似判断条件含 `nBitRate`/`audioInfoNotified` 附加变化）

---

## 六、日志降噪（ok-20260806-8 / ok-20260806-9 / ok-20260806-14）

用户反馈串口日志刷屏（Mp3HeadResync / AacProbe / Resync no dice 每秒几十行），要求降噪。三批降级：

| 批次 | 改动 |
|------|------|
| ok-20260806-8 | `cdx_log.c` 全局日志级别 `LOG_LEVEL_DEBUG → LOG_LEVEL_WARNING`（只留 WARNING/ERROR） |
| ok-20260806-9 | 8 个文件 12 处 probe 阶段"预期失败"日志 `LOGE/LOGW → LOGD`（AacProbe latm / FlvProbe / AviProbe / OggProbe / FlacProbe / Mp3 Resync no dice / ParserTypeGuess / FileStream key not found） |
| ok-20260806-14 | `CdxWavParser.c:881/785`（Wav parser no dice / giving up）`LOGW → LOGD` |

---

## 七、Git 版本管理（今日 14 个固化点）

| tag | 内容 |
|-----|------|
| ok-20260806-1 | flac/pls parser 源码启用，P8 编译收尾 |
| ok-20260806-2 | 播放按钮播第一首 + fd:// 大 ID3 tag 修复 |
| ok-20260806-3 | 播放位置记忆（/data/dm_music.state） |
| ok-20260806-4 | CONFIG_DISABLE_AUDIO=0 + audiomix/opus + settings 崩溃修复 |
| ok-20260806-5 | 切歌前 XPlayerReset（状态机 IDLE） |
| ok-20260806-6 | dm_sound_open S24_LE 映射（后被证无效，未触发） |
| ok-20260806-7 | checkSampleRate 传真实 bit + dm_sound_write 物理宽度换算 |
| ok-20260806-8 | 全局日志级别 WARNING |
| ok-20260806-9 | probe 日志降级 12 处 |
| ok-20260806-10 | writei 诊断打印 |
| ok-20260806-11 | period/buffer 256/1024（对齐参考） |
| ok-20260806-12 | adecoder 普通 PCM 填 nBitpersample |
| ok-20260806-13 | **sw_params 补齐（声卡启动突破）** |
| ok-20260806-14 | underrun(-EPIPE) 恢复 + CdxWavParser 降级 |

---

## 八、遗留事项 / 下一步

1. **🔴 无声问题（最高优先）**：writei 成功但无声，声卡硬件已排除（sine 有声）。下一步按 5.4 排查：
   - 确认 XPlayer 送入的 PCM 数据内容（加数据头 dump 或比对 sine 数据）
   - 追 `bits=16` 传递链：`checkSampleRate` 时序 + `needDirect 0→1` 重配影响
   - 若确认数据/格式无误，查 codec 侧数字音量（`digital_vol=0x0`，历史文档 7-26 修复值为 `0x13`）与功放使能
2. 已知遗留（非本任务）：SDMMC 多块读 DMA 根治、模拟器 freetype、蓝牙符号观感、背景图 1920x1200、Text Size 演示态、天气城市写死上海、G2D vs 软件渲染
3. 若 XPlayer 实在走不通，回退方案：WAV 留 nxplayer（已验证），mp3/flac 另想办法（用户已确认统一 XPlayer，可再议）

---

*DevLog by AtomCode (deepseek-v4-flash)*
