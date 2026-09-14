# DevLog 2026-08-05 (P8) — 音乐播放器 XPlayer 集成（mp3/flac/ogg/aac/wav）+ 真实进度 + 清单 UI 改造

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，再读本文件。本文件记录 P8 全部背景、已完成改动、编译卡点与接手步骤。**

---

## 一、任务背景

1. 用户贴出 **f3e10224 上板日志**：WAV 播放成功（IGNIS.wav 正常播放 → `playfile ret=0` → 自动切歌 Aurora Bore...，长文件名正常、音量 75% 生效）；日志中 `Xrun...` 是 `MMCSD_MULTIBLOCK_LIMIT=1` 单块读变慢导致的欠载（驱动自动 prepare 恢复，非致命）。
   → **WAV 播放问题确认解决**（用户确认"已解决"）。
2. 用户新需求（本会话）：
   - **mp3/flac 支持**（devlog 需求格式 = mp3/flac/ogg/aac/wav，UI 清单已按此扫描）
   - **进度条 / 剩余时间真实同步**（原来是 timer 模拟 val++）
   - **播放清单 UI**：① 从居中改为**三点按钮下方弹出**（dropdown）；② **背景不透明**（原 LV_OPA_20 遮罩 + 90% 卡片，字看不清）；③ **点击歌曲有按压效果**（否则不知道点没点到）
3. 方案决策（**用户确认**）：**统一走 XPlayer** —— mp3/flac/ogg/aac/wav 全部改用 libcedarx XPlayer，替换 nxplayer。

## 二、侦察结论（本会话已核实，勿重复翻源码）

| 项 | 结论 |
|----|------|
| 需求格式 | mp3/flac/ogg/aac/wav（`deskmate_ui.c` 扫描表 supported[]） |
| 已开配置 | `CONFIG_LIBCEDARX=y`、`CONFIG_AW_MULTIMEDIA=y`、`CONFIG_FLAC_PARSER_ENABLE=y`；.config 里 AVI/FLV/TS/MKV/MOV/AAC/ID3V2/MP3/OGG/PLS/WAV_PARSER_ENABLE=y |
| xplayer 是否在构建内 | ✅ `libarch.a` 含 `xplayer.o`（`XPlayerCreate` 符号在，grep=1）；ELF 里没有是因为**静态库惰性链接**（无人引用不拉取）→ 只要代码调用 XPlayer API 就会链入 |
| xplayer.o 未定义符号 | 45 个，几乎全部在 libarch.a 内解析（DemuxComp*/AwMessageQueue*/Cdx*），libc 符号由 libc 提供 → 链接面可控 |
| 音频解码库 | `lib/audio/libaw_mp3dec.a / libaw_flacdec.a / libaw_wavdec.a / libaw_oggdec.a / libaw_aacdec.a`：**ARM ELF32 可重定位文件，非 ASAN（0 个 __asan 引用）** ✅ 可用；libcedarx/Make.defs 的 EXTRA_LIBS 已配 `-law_*dec -lcedarc` + EXTRA_LIBPATHS 指向 audio/libcedarc |
| **libcedarc.a（预编译）** | ⚠️ ARM ELF32 但 **ASAN 插桩版**（46/49 对象含 `__asan_*`，去重 11 个符号）；固件无 ASAN runtime → 链接必失败 → **已写 stub 解决**（见三） |
| **libcedarc.a 还依赖 Melis 内核 API** | `enter_critical_section` / `leave_critical_section`（ionAlloc.o 引用）→ NuttX 此配置无（critmon 门控）→ **已写 stub**（见三） |
| **parser 预编译库（致命坑）** | `libcore/parser/*/libcdx_*_parser.a`（flac/mp3/wav/ogg/aac/...）全部是 **RISC-V ELF64**！与 R528（ARM）**架构不匹配，不能链接** → parser 必须用**源码**编译（CHIP_CSRCS），且 **flac 源码行在 Make.defs 里被注释**（106 行） |
| flacParserCtor | 定义在 `CdxFlacParse.c:1204`，但 Make.defs 106 行 `#CHIP_CSRCS += ...flac...CdxFlacParse.c` 被注释 → 链接报 `undefined reference to flacParserCtor`（当前编译卡点） |
| CdxParser.c 引用 | 70 处 `ParserCtor`（asf/avi/flv/ts/mov/mkv/mpg/flac/mp3/wav/ogg/aac/...），受 `#ifdef CONFIG_*_PARSER_ENABLE` 控制；Make.defs 中多数 parser 源码行已启用，**唯独 flac（106）、pls/ape/asf/mpg 等被注释** → 接手时需逐个核对"启用配置 ↔ 源码行 ↔ ctor 引用"三者一致 |

## 三、已完成改动（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c` | ① `#ifdef CONFIG_LUNCHER_DM_APP` 内 include 改为 `xplayer.h / soundControl.h / adecoder.h / pcm.h`；② 新增全局 `g_xplayer` + `volatile g_xp_state`（XPlayer 异步状态机 0 idle/1 prepared/2 playing/3 complete/-1 error，**回调线程只写标志，LVGL timer 单消费者轮询**，规避线程安全）；③ 播放后端 nxplayer → **XPlayer**（Create→SetNotifyCallback→InitCheck→SetAudioSink→SetDataSourceUrl→PrepareAsync→Start/Pause/Stop）+ **自写 SoundCtrl**（`dm_sound_*`，用 aw-alsa-lib `snd_vela_pcm_open/writei/prepare/pause/reset/close` 输出 `hw:audiocodec`，hw_params：S16_LE/S32_LE、period 1024、buffer 4096）；④ 事件回调 `dm_xplayer_cb` 只置 `g_xp_state`；⑤ `music_timer_cb` 改**真实进度**（`music_audio_position/duration` → XPlayerGetCurrentPosition/GetDuration）+ 总时长标签同步 + 播放完成/出错自动切歌（`music_audio_poll`）；⑥ 清单卡片：`lv_obj_center` → `LV_ALIGN_TOP_RIGHT, -DM(16), DM(52)`（三点按钮下方）+ 卡片 `LV_OPA_COVER` 不透明 + 行加 `LV_STATE_PRESSED` 蓝色按压效果 + transform 微放大；⑦ 补 `music_audio_position/duration` 前置声明（否则 timer 隐式声明报错） |
| `apps/luncher_dm/Makefile` | CSRCS 加 `dm_asan_stub.c`；CFLAGS 加 libcedarx 全部 include 路径（libcore/include、base、common/iniparser、common/plugin、stream、parser/base/id3base、parser/include、playback、xplayer、decoding/osal、decoding/include、decoding/tools/audiomix/src、external/include/adecoder、external/include/libcedarc、external/include/zlib）+ `aw-alsa-lib` pcm.h |
| `apps/luncher_dm/dm_asan_stub.c`（**新增**） | libcedarc.a 缺失符号 no-op stub：`__asan_load/store[1/2/4/8/N]_noabort`（11 个）+ `enter_critical_section` / `leave_critical_section`（Melis 内核 API，NuttX 此配置无；no-op 等价关闭检测/临界区，单线程音乐播放路径安全） |

模拟器端不受影响：以上全部在 `#ifdef CONFIG_LUNCHER_DM_APP` 内或 #else 空实现，lv_port_linux 编译时排除。

## 四、编译状态（当前卡点，重要）

命令：`cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)`

- 已解决：ASAN 缺失（stub）→ enter/leave_critical_section 缺失（stub）→ include 路径缺失（CdxParser.h/CdxStream.h，补 Makefile）→ **当前卡在 `flacParserCtor` undefined reference**（CdxParser.c:567 AwParserInit）
- **未完成打包**（nsh.fex / vela.bin 未更新，固件仍是 f3e10224）
- 后续链接可能还有缺失（libcedarc.a 其它 Melis API 等），需继续编译迭代

## 五、遗留事项 / 下一步（接手指引）

1. **编译卡点：flacParserCtor（优先级最高）**
   - 方向 A（推荐）：`libcedarx/Make.defs` 第 106 行启用 `CHIP_CSRCS += ...parser/flac/CdxFlacParse.c`（去掉 `#`），用源码编译 flac parser（预编译 .a 是 RISC-V 用不了）。注意它依赖 `CdxFlacParser.h` 头文件，检查 include 是否齐。
   - 方向 B：若不想用 flac，可 defconfig 关 `CONFIG_FLAC_PARSER_ENABLE` —— **但用户明确要 flac，不可取**。
   - ⚠️ 同时核对 CdxParser.c 的 70 处 ctor 引用 ↔ .config 各 `CONFIG_*_PARSER_ENABLE=y` ↔ Make.defs CHIP_CSRCS 三者一致；被注释的 parser 源码行若其 ctor 被 `#ifdef CONFIG_*_PARSER_ENABLE` 引用，会同样 undefined（pls/ape/asf/mpg 行已被注释，若对应配置为 y 需处理）。
2. 继续 `./build.sh` 编译，按 undefined reference 逐个补齐（缺什么 stub 什么；Melis API stub 注意运行时语义）。
3. 编译通过 → **打包**：`cd vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack` → 验证 `nsh.fex == vela.bin`（md5）。
4. **固化（AGENTS 第八章硬性规则）**：`bash /data/vela/git_snapshot.sh`（三仓 vendor/nuttx/apps 自动 tag ok-YYYYMMDD-N）。
5. **上板验证**：mp3/flac/ogg/aac/wav 播放不崩、进度条/剩余时间真实同步、清单三点按钮下方弹出/不透明/点击有按压反馈；XPlayer 首次在 NuttX 上跑，重点观察内存与线程表现（libcedarc.a 是 ASAN 插桩版，stub 后无检测，若有越界不易察觉）。
6. 若 XPlayer 集成实在走不通（运行时问题多），回退方案：WAV 保留 nxplayer（已验证），mp3/flac 另想办法（当时用户确认统一 XPlayer，但可再议）。
7. 已知遗留（本会话未处理）：SDMMC 多块读 DMA 根治、模拟器 freetype、蓝牙符号观感、背景图 1920x1200、Text Size 演示态等（见 AGENTS.md 下一步清单）。

---

*DevLog by AtomCode (deepseek-v4-flash)*
