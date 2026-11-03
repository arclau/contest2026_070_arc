# 交接：给外部优化者的系统说明（11-02）

> **这份文档的用途**：让没有上下文的人（或另一个 AI）读完就能定位代码、判断
> 「哪些能力本来就不该依赖网络」，并给出可落地的优化。
>
> 阅读顺序建议：本文 → `AGENTS.md`（硬约束）→ `devlog.md` 首行「进度」快照。
> 各节点的技术细节在 `project_docs/devlogs/11-02-p34x-*.md`。

---

## 1. 系统边界

| 项 | 值 |
|---|---|
| 硬件 | Allwinner R528（`sun8iw20p1`），板 `ynm3000`，1920×1200 |
| 系统 | openvela / NuttX RTOS，`CONFIG_BUILD_FLAT=y`（无 MMU 隔离） |
| 存储 | NAND：`bootloader(nsh.fex)` / `res(res.fex)`→`/resource` / `usrdata(usrdata.fex)`→`/data` |
| 界面 | LVGL |
| 角色 | 桌面宠物「银月」+ 语音助手 |

**两个进程**，通过本机 WebSocket 通信（`127.0.0.1:28789`）：

```
┌─────────────────────┐        WS 127.0.0.1:28789        ┌──────────────────────┐
│  luncher_dm (UI)    │  ←──── voice_evt / 回复 ────────  │  ai_agent (agent)    │
│  LVGL、宠物、设置页  │  ───── 用户话语 / 指令 ────────→  │  ASR/TTS/LLM/ws_server│
└─────────────────────┘                                   └──────────────────────┘
```

关键代码位置：

| 模块 | 路径 |
|---|---|
| UI 侧 WS 客户端 | `vendor/allwinnertech/apps/luncher_dm/dm_ai.c`（`:31-32` host/port） |
| UI 侧 voice_evt 分发 | `dm_ai.c:807-860`（kind → 宠物阶段 / stt / llm / cmd / error） |
| UI 侧本地命令执行 | `deskmate_ui.c:2206` 表 + `:2218` `ai_local_cmd_dispatch()` |
| UI 侧系统级轮询 | `deskmate_ui.c:2252` `dm_ai_cmd_poll_cb()`（300ms） |
| agent 侧 WS 广播 | `packages/ai_agent/src/channels/ws_server.c:653` |
| agent 侧语音主流程 | `packages/ai_agent/src/voice/voice_channel.c` |
| agent 侧命令匹配 | `voice_channel.c:1180` 表 + `:1202` `voice_local_cmd_parse()` |

---

## 2. 一次语音指令的完整链路

```
按键/唤醒
   ↓  audio_capture
① 录音 PCM
   ↓  voice_asr_recognize()   ← ★ 云端（xiaomi_asr → api.xiaomimimo.com:443）
② 文本
   ↓  voice_local_cmd_parse() ← ★ 纯本地字符串匹配，voice_channel.c:1202
   ├─ 命中 → voice_local_cmd_run() :1281 → 广播 voice_evt(kind=cmd)
   │           ↓  UI: ai_local_cmd_dispatch() :2218  → ★ 纯本地执行
   └─ 未命中 → 送 LLM（云端）→ TTS（云端）
```

**★ 标记的三处里，只有 ① 和「未命中的 LLM/TTS」在云端。**

也就是说：**命令匹配和执行全在本地，唯一的硬依赖是「怎么把声音变成文字」。**

---

## 3. 断网能力矩阵（核心）

| 能力 | 实现 | 断网可用 | 依据 |
|---|---|---|---|
| 文本 → 命令匹配 | `g_local_cmd_matchers[]`，纯字符串 | ✅ | `voice_channel.c:1180/1202` |
| 命令执行（8 类） | `ai_local_cmd_dispatch()` → 本地 executor | ✅ | `deskmate_ui.c:2206/2218` |
| ├ `app:` 开页面 | `dm_cmd_app` | ✅ | `deskmate_ui.c:2169` |
| ├ `music:` 本地曲库 | `dm_cmd_music` | ✅ | `deskmate_ui.c:1845` |
| ├ `volume:` 音量 | `dm_cmd_volume` | ✅ | `deskmate_ui.c:1884` |
| ├ `brightness:` 亮度 | `dm_cmd_brightness` | ✅ | `deskmate_ui.c:1902` |
| ├ `pet:` 宠物动作 | `dm_cmd_pet` | ✅ | `deskmate_ui.c:1926` |
| ├ `alarm:` 闹钟 | `dm_cmd_alarm` | ✅ | `deskmate_ui.c:2004` |
| ├ `scene:` 场景 | `dm_cmd_scene` | ✅ | `deskmate_ui.c:2109` |
| └ `health:` 健康打卡 | `dm_cmd_health` | ✅ | `deskmate_ui.c:2136` |
| 本地提示音 | `dm_tone_play()` 播 `/resource/tones/*.wav` | ✅ | `deskmate_ui.c:3298` |
| **语音 → 文本** | `xiaomi_asr` / `volc_asr` | ❌ | `voice_channel.c:1308` |
| 确认音「好的」 | `voice_local_cmd_ack()` → 云端 TTS | ❌ | `voice_channel.c:1258` |
| 天气 / 笑话 / 闲聊 | 云端 API + LLM | ❌ | — |

**结论：断网时「能做」的能力远多于「能触发」的能力。缺的是入口，不是能力。**

---

## 4. 三个离线落地点（按投入排序）

### A. 屏幕按钮直连本地 API（最省力，零新依赖）

不需要语音、不需要 ASR、不需要网络。UI 里任何按钮只要调一行：

```c
ai_local_cmd_dispatch("pet:sit");       /* 猫坐下 */
ai_local_cmd_dispatch("pet:sleep");     /* 猫睡觉 */
ai_local_cmd_dispatch("pet:walk:300");  /* 散步 5 分钟 */
ai_local_cmd_dispatch("music:play");
ai_local_cmd_dispatch("volume:+10");
```

`ai_local_cmd_dispatch()` 会查 `g_dm_local_cmds[]` 前缀、执行、按 `needs_ack`
决定是否播确认。**这条路径完全绕开 agent 和网络。**

> 现状：宠物只有长按/拖拽等触摸交互，没有「坐下/睡觉/散步」按钮。
> 加按钮即可获得完整的离线宠物控制。

### B. 断网时确认音改本地 wav（解决"执行了但没声音"）

`voice_local_cmd_ack()`（`voice_channel.c:1258`）现在调 `voice_channel_speak("好的")`，
那是云端 TTS，断网必失败——动作执行了，但用户听不到确认。

现成本地能力：`dm_tone_play("/resource/tones/<name>.wav")`，预合成、带声卡仲裁。
可选做法：agent 侧广播 `voice_evt(kind=cmd, text="tone:ack")`，UI 侧
`ai_local_cmd_dispatch()` 里加一条 `tone:` 前缀 → `dm_tone_play()`。
**需要确认 `/resource/tones/` 里有没有合适的短确认音**（现有为
`popup_*` / `bg_reward_*` / `care_01` 等）。

### C. 本地关键词唤醒（KWS）——真正解决"断网也能说"

在录音链路前面加一层轻量关键词检测（能量 + 模板 / 小型 CNN），命中即直接
`ai_local_cmd_dispatch()`，不录音、不上云、不等 ASR。

- 优点：断网可用，延迟极低
- 代价：要做模型或模板，词表有限（建议只覆盖 `pet:` 和 `volume/brightness`）
- 注意：`CONFIG_BUILD_FLAT=y`，模型占内存要算进 `LV_CACHE_DEF_SIZE` 之外的部分

---

## 5. 已修 / 待优化清单

### 已修（本轮）

| 节点 | 问题 |
|---|---|
| P353 | 断网时**纯本地的快问被联网门禁误拦**：`ai_chip_click_cb`（`deskmate_ui.c:236`）先判网络再判断句，「播放音乐」走本地曲库却在门禁处被 return。已改为 `local_only` 跳过门禁 |
| P352 | ASR 失败静默：原来只发 `pet_idle`，零提示。新增 `voice_evt(kind=error)`，系统级 timer 弹宠物气泡 |
| P351 | 语音「睡觉」后 0.42s 被 TTS 播报阶段打断（`PET_EVT_VOICE_PHASE(3)` 把 `sleep_enter` 换成 `wake`） |
| P350 | 词表优先级：`match_comfort` 被 `match_volume` 抢走 |
| P349/P348 | 见 `devlog.md` |

### 待优化（按收益/风险排）

| # | 项 | 说明 | 风险 |
|---|---|---|---|
| 1 | 屏幕按钮接本地 API | 见 §4-A，投入最小、收益直接 | 低（纯 UI 增加） |
| 2 | `dm_net_wifi_connected()` 语义 | `dm_net.c:807` 只判 `wlan0` 有无 IPv4，**连 AP 无上行时返回真** → 所有联网门禁失效 | 中：天气/设置页都依赖它，改成"可达性探测 + 缓存"要防误判 |
| 3 | 断网确认音 | 见 §4-B | 低 |
| 4 | 宠物取帧失败 | NAND 读路径 `PNG input mismatch` 成串出现，单次可到 `+1350ms`。已排除：三方 CRC 全对、坏块不在 `res` 分区、同一文件每次读出不同 CRC ⇒ 指向 cache/DMA 而非介质 | 高：`AGENTS.md §五` 明确**禁止应用层打补丁**，需底层排查 |
| 5 | 壁纸恢复 | P348 是"关掉文件壁纸"不是"修好"。`blue-hour.png` 1920×1200 解出 8.8MB > 5MB 缓存 | 中：建议开机解一次 → 缩到 ≤1/4 → `lv_image_dsc_t` 常驻 + `lv_image_set_scale` |
| 6 | 银月 → 音乐 同音 | ASR 把「银月」听成「音乐」。当前仍能命中 `pet:*`（`match_app` 的"音乐"要求带开启动词） | 低：可把 `match_pet` 提到 `match_app` 之前 |
| 7 | MiMo Key 明文 | usrdata `config.json` 里有 Key | 提 PR 前清掉 |

---

## 6. 硬约束（改之前必须知道）

1. **`AGENTS.md` ≤ 8192 字节**，改文档要量（当前 8180）。
2. **`AGENTS.md §五` 禁止在应用层给 NAND 读路径打补丁**——宠物取帧问题必须底层解决。
3. **增量构建不跟踪头文件依赖**：改了 `.h` 必须 `touch` 所有 include 它的 `.c`，
   否则"构建 PASS 但代码没进镜像"。
4. **构建脚本用 `@` 抑制了逐文件编译行**，日志里只有 `CPP:`/`LD:`。
   判断是否真重编要看 `vendor/.../<name>.c.data.dm.*.o` 的 mtime，**不能 grep `CC:`**。
5. **`CONFIG_BUILD_FLAT=y`**，没有进程隔离，一个野指针能搞死全系统。
6. LVGL **单线程**，所有 UI 操作必须在 LVGL 线程；跨线程只能通过 poll 标志
   （现有模式：agent 广播 → `dm_ai.c` 存 static 缓冲 + seq → LVGL timer 轮询）。

---

## 7. 验证手段

| 手段 | 说明 |
|---|---|
| 串口 | `/dev/ttyUSB0 @1500000`，日志在 `logs/serial/latest.log` |
| 抓取进程 | `dm_serial_log.py _daemon`。**刷机后必查**：USB 重枚举会让 termios 掉回默认波特率，表现为低速乱码（~89 B/s、零可读行） |
| 构建 | `./vendor/allwinnertech/tools/pack_deskmate.sh ynm3000`，看 `PASS` + 产物 md5 |
| 产物 | `output/ynm3000/ynm3000.img` |

**日志分析注意**：串口日志会自动换行，关键词可能被截断在两行之间，
`grep 'voice_evt('` 会漏计 → 先 `tr -d '\r\n'` 再统计。
