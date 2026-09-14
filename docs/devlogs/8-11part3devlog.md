# DevLog 2026-08-11 — MIC 上板修复 + 控制台恢复 + 串口降噪全景（P56~P62）

> 本次会话主题：把 P55 遗留的「MIC DEMO 待上板」真正跑通——从录音打不开、到通路使能、到独立工具、到控制台被抢、到日志刷屏，一条线修到底。
> 覆盖：现象 → 根因 → 方案 → 验证（每节点四段式），改动列文件表，记录验证命令与产物。
> 固化 tag：ok-20260811-25 ~ ok-20260811-31（详见 §7 时间线）。
> 姊妹文档：`8-11part2devlog.md`（P48~P55：AI 链路 + MIC DEMO 代码 + 崩溃修复）。

---

## 1. 背景：本次对话在做什么

P55 结束时 MIC DEMO 代码闭环但**待上板**。本轮用户上板逐次反馈，暴露的是一条完整的「录音链路」问题链，且伴随两个系统级副作用（控制台被抢、日志刷屏）。按用户指示逐步收敛：

1. **P56**：点 Record 卡片 → `media_recorder_open failed` / `mic demo: -5`（-EIO）——openvela media 框架根本没编进固件
2. **P57**：改用板级 `snd_vela_pcm_*` 后 capture 报 `-22`——codec MIC 通路默认关闭 + 串口被 ANSI 颜色码/调试行污染
3. **P58**：用户提议录音 DEMO 应是独立小工具 → 确认 SDK 自带 `arecord`/`aplay` 本就在固件，补 MIC 使能即可
4. **P59**：刷机后 `vela>` 提示符占住串口回不到 nsh——ai_agent CLI 线程抢 stdin
5. **P60**：AI 录音 DEMO 点开 Data abort（栈溢出）→ arecord 栈加大防崩
6. **P61**：NSH 能进但被日志刷屏按不动回车——weixin 每 5s 一条 No token
7. **P62**：用户不用微信 → 直接禁用 weixin 频道（Kconfig default y → defconfig not set）

**核心结论（用户视角）**：录音验证与 AI 完全解耦——`nsh> arecord -d 5 -t` 一条命令录 5s 自动回放；AI DEMO 崩不崩不关心。

---

## 2. 录音链路：为什么 AI DEMO 一开始是死的（P56）

### 2.1 现象
点 AI 子页 Record 卡片 → 串口：
```
[audio_cap] media_recorder_open failed
MIC error: capture open failed
[ws] mic demo: -5
```

### 2.2 根因（逐层剥）
`audio_capture_open()` → `media_recorder_open(MEDIA_SOURCE_MIC)` 返回 NULL。调用链：

```
media_recorder_open("Capture")
  → media_open(MEDIA_ID_RECORDER, "Capture")   [client, media_graph.c:532]
  → media_proxy → 服务端 media_recorder_handler
  → media_stub_get_stream_name("Capture", ...)  [media_stub.c:180]
      #ifdef CONFIG_LIB_PFW ... #else return -ENOSYS
  → media_recorder_open(ctx, name)
  → audio_graph_open(&ctx->audio_input, "Capture")
      #ifdef CONFIG_LIB_FFMPEG ... 否则 plugin 未注册
```

三个开关全部没启用（`nuttx/.config` 核实）：
| 开关 | 状态 | 影响 |
|------|------|------|
| `CONFIG_LIB_FFMPEG` | `# ... is not set` | `media_daemon.c` g_media[] 数组**根本没有 recorder 插件**（`#ifdef CONFIG_LIB_FFMPEG` 才注册） |
| `CONFIG_LIB_PFW` | `# ... is not set` | `media_stub_get_stream_name` 恒返 `-ENOSYS` |
| `CONFIG_MEDIA_SERVER` | 未设 | media daemon 未编译 |

→ 本板固件**没有 openvela media server**，`media_recorder_open` 恒 NULL → `audio_capture_open` 失败 → `voice_channel_test_mic` 返回 `-EIO`（-5）。

> 关键对比：音乐播放正常因为走的是 **aw-alsa-lib `snd_vela_pcm_*`** 原生路径（luncher_dm `dm_sound_open` 同源），与 media 框架无关。

### 2.3 方案
重写 `packages/ai_agent/src/voice/audio_capture.c` + `audio_playback.c`，从 media 框架改为板级 aw-alsa-lib：

```c
snd_vela_pcm_open(&handle, "hw:audiocodec", SND_VELA_PCM_STREAM_CAPTURE, 0);
// hw: period 1024 / buffer 4096；sw: start_threshold=1（capture 有数据即启动）
// readi/writei 均带 EAGAIN/EPIPE 重试恢复
```

### 2.4 验证
- 编译打包产物一致 7758552B；strings 命中 `snd_vela_pcm_open(%s, CAPTURE)`
- 固化 **ok-20260811-25**（三仓 + ai_agent 独立仓单独 commit+tag）

---

## 3. 通路使能 + 串口污染清理（P57）

### 3.1 现象
上板 P56 后：capture 报
```
[SND_ERR][sunxi_codec_hw_params:1362]capture only support 1~3 channel
[audio_cap] hw_params fail -22
```
且串口出现大片乱码（ANSI 转义序列被串口当字符显示）+ 每次 open/close 打无意义调试行。

### 3.2 根因
1. **-22**：`sunxi_get_adc_ch()`（sun8iw20-codec.c:126）读 ADC1/2/3 的 `SUNXI_*_ANA_CTL` 寄存器 `MIC_PGA_EN`（MIC1 input switch）——**默认全 0** → 返回 -1 → capture 被拒。本板集成 MIC 走 **MICIN1P/MICIN1N + MBIAS → ADC1**，需先把 MIC1 输入通路打开。
2. **乱码**：`snd_err`/`awalsa_err` 宏带 ANSI 颜色码（`SNDRV_LOG_COLOR_RED = "\e[31m"`）→ 串口乱码。
3. **噪音**：aw-tiny-alsa-lib `pcm.c` 5 处无条件 `syslog(LOG_ERR, ...)` 调试行（`mutex:%p`/`%d, %s`/`prepare:%p`/`mutex_destroy:%p`），每次 open/close 必打。

### 3.3 方案（改动文件表）
| 文件 | 改动 |
|------|------|
| `packages/ai_agent/src/voice/audio_capture.c` | open 前 `snd_ctl_set("audiocodec","MIC1 input switch",1)` + `MIC1 gain volume=19`（参考 HAL aloop 测试写法） |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c` | **不改**（读代码确认 MIC_PGA_EN 由 ctl 控制，非驱动 bug） |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal/sound/snd_core.h` | `snd_err` 宏去掉 `SNDRV_LOG_COLOR_RED/NONE` 颜色码 |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal/aw-alsa-lib/pcm.h` | `awalsa_err` 宏去掉 `AW_ALSA_LOG_COLOR_RED/NONE` |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/component/aw-tiny-alsa-lib/pcm.c` | 删 5 处无条件 LOG_ERR 调试行 |

### 3.4 验证
- **坑 3 变体再验证**：改头文件（pcm.h/snd_core.h）增量构建**不重编 HAL .o**（旧 pcm_hw.o/pcm_mmap.o 残留旧颜色串）→ 删 sound/codec 全部 .o 强制重编
- strings 验证污染串全 0（`mutex:%p`/`[31m[AWALSA_ERR` 等）+ `MIC1 input switch` 进固件
- 产物一致 7754456B；固化 **ok-20260811-26**

---

## 4. 独立录音工具（P58，与 AI 解耦）

### 4.1 背景
用户：录音 DEMO 应是**独立小工具**、跟 AI 无关，别搞复杂。

### 4.2 发现
SDK 自带 `arecord`/`aplay`（`CONFIG_AUDIO_TEST=y`，`vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/test/sound/`）**本就在固件**，NSH 直接可用。仅缺 MIC 使能（P57 只加在 ai_agent）→ `arecord.c` main 开头补同样 4 行 `snd_ctl_set`（MIC1 gain/switch）。

### 4.3 验证
- 产物一致 7754456B；`arecord` 命令在固件；固化 **ok-20260811-27**
- 上板命令：`nsh> arecord -d 5 -t`（录 5s 自动回放）/ `arecord -c 1 -d 5 /data/mic.wav`

---

## 5. 控制台被抢（P59）——回不到 nsh

### 5.1 现象
刷机后串口常驻 `[cli] NSH CLI started` + `vela>` 提示符，NSH 输入无效，WiFi 也无法操作。

### 5.2 根因
ai_agent `nsh_commands.c` 的 `cli_thread()` 用 `fgets(stdin)` **阻塞读终端**做 65 个命令的交互解析；rcS.nsh 自启 `ai_agent &` **继承控制台 stdin** → 与 NSH 抢同一串口输入。查证 NuttX **无 `nsh_command()` 动态注册 API**（无法把命令注册成真 NSH 命令）。

### 5.3 方案
rcS.nsh 自启改：
```
ai_agent </dev/null &
```
`CONFIG_DEV_NULL=y` 可用；cli_thread 的 `fgets` 立即读 EOF → 线程退出 → 控制台归 NSH。AI 交互走 WS/UI 不受影响。

### 5.4 验证
- **坑 3**：rcS.nsh 是 prebuilt，必须 `distclean` 全量重编 + strings 验证
- 产物一致 7754456B；strings 命中 `ai_agent </dev/null &`；固化 **ok-20260811-28**
- 上板确认：`nsh>` 正常出现 + WiFi 连 wifi-home + 天气 OK

---

## 6. AI DEMO 崩溃定位 + arecord 栈加大（P60）

### 6.1 现象
AI 子页点录音 DEMO → Data abort（PC 4147adc4，DFAR fffb0024，`SP not within stack`）。用户指示：**DEMO 用不用没关系**，重点是命令行测 MIC。

### 6.2 根因
addr2line（strip 前 nuttx.elf）：崩溃在 ai_agent **ws client_thread**（ws_server.c:355，处理 mic demo 消息）；`SP not within stack` + 二次故障落在 getpid（读 TCB）= **栈破坏/溢出**——WS 线程栈仅 12KB（AGENT_WS_CLIENT_STACK），`voice_channel_test_mic` 栈上还有 3200B chunk + readi 深调用链（snd_pcm_lib_read → wait_for_avail → transfer → memcpy）。

### 6.3 方案
arecord/aplay（SDK CONFIG_AUDIO_TEST 独立进程）STACKSIZE 仅 **4096B** → 加大到 **16384B**（`vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/test/sound/Makefile`），防 capture 路径同样栈溢出。

### 6.4 验证
产物一致 7754456B；固化 **ok-20260811-29**。

---

## 7. 串口降噪（P61）+ 移除 weixin（P62）

### 7.1 P61 现象/根因/方案
NSH 能进但**被日志刷屏按不动回车**。根因：weixin poll 线程无 token 时每 5s 打一条 `No token, waiting...`（`weixin_channel.c:321`，`WEIXIN_RECONNECT_DELAY_S=5`）无限刷。LVGL User 日志是操作日志（非持续）、weather FAIL 仅在 WiFi 未就绪时偶发（连上 wifi-home 后即 OK）。

**方案**：`s_no_token_warned` 标志——无 token 只提示一次，之后静默重试；token 恢复后重置（下次再缺仍提示一次）。产物一致 7754456B，固化 **ok-20260811-30**。

### 7.2 P62 现象/根因/方案
用户：**我又不用微信，把它去掉**。`AI_AGENT_WEIXIN` 是 Kconfig `default y`（.config 5164）→ defconfig 显式加 `# CONFIG_AI_AGENT_WEIXIN is not set` 覆盖。无残留确认：agent_main.c 全部 7 处调用在 `#ifdef CONFIG_AI_AGENT_WEIXIN` 内 + CMakeLists/Makefile 条件编译 + stubs.c weak 兜底。

**验证**：产物一致 **7750232B**（-4KB）；strings 验证 weixin 特征串（No token/Long-poll/ilinkai.weixin.qq.com）全 0；固化 **ok-20260811-31**。

---

## 8. 固化时间线

| tag | 内容 |
|-----|------|
| ok-20260811-25 | P56 media 框架 → snd_vela_pcm 原生路径 |
| ok-20260811-26 | P57 MIC1 通路使能 + ANSI 颜色码/调试行清理 |
| ok-20260811-27 | P58 arecord 补 MIC 使能（独立工具） |
| ok-20260811-28 | P59 ai_agent </dev/null 控制台回归 NSH |
| ok-20260811-29 | P60 arecord 栈 4096→16384B |
| ok-20260811-30 | P61 weixin No-token 降噪 |
| ok-20260811-31 | P62 禁用 weixin 频道 |

产物大小：7758552 → 7754456 → **7750232B**（weixin 移除 -4KB）。

---

## 9. 遗留事项

1. **`nsh> arecord -d 5 -t` 上板实测**（唯一未闭环项）——不崩、能录能放 = MIC 硬件确认；若崩 → 查 capture DMA 驱动层（AI DEMO 的 DFAR=fffb0024 是非 DDR 非法地址，疑似 capture 的 runtime->dma_addr 建立问题，arecord 若崩大概率同一处）
2. **AI 子页录音 DEMO 崩溃**（P60 定位 = WS 线程 12KB 栈溢出）——用户指示不用管；若以后要修，加 AGENT_WS_CLIENT_STACK 或把 test_mic 的 chunk 改堆分配
3. 语音链路（TTS/ASR 实测）待 MIC 确认后继续
4. 蓝牙 H4 110 硬件排查挂起（用户暂缓，勿擅改驱动）

---

*DevLog by AtomCode (deepseek-v4-flash)*
