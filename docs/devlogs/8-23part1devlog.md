# DevLog 2026-08-23 part1 — 功能开发：天气优化 + AI 语音控制音量/亮度/音乐（P114~P116）

## 节点

- **P114** 天气同步 2h 周期 + 去 UI 时延 → ok-20260823-1
- **P115** AI 语音本地命令：音量/亮度 ±5% → ok-20260823-2
- **P116** AI 语音命令扩展：音乐控制 + 绝对设置 → ok-20260823-3

---

# DevLog 2026-08-23 — 天气同步 2h 周期 + 去掉 UI 时延信息（P114）

## 背景

ok-20260817-4（重启 WiFi 自动连接加固）上板复测通过（日志：`wapi show wlan0` → ESSID=wifi-home, IP=192.168.*.*，wifi startup done；天气 fetch OK、NTP 对时完成）。用户随后提出：

1. 去掉 home UI 天气同步的时延信息（`w_debug_lbl` 显示 `OK 752758882ms: ...` 这类 debug 串）
2. 天气只需开机时同步一次，之后每 2h 更新一次（原 4h，P113 定）

## 现象

开机日志中 `weather_fetch_worker: weather fetch: OK 752758882ms: 23.11, 5 days, 32C/69%` —— 该 ms 值是 NTP 对时前后系统时间跳变导致的假象（fetch 起点在 1970 年时钟上），UI 天气卡 `w_debug_lbl` 也显示同一串，无实际价值。

## 根因 / 方案

- 时延信息本是 API 对接排障用（P73~P113 期间），链路稳定后 UI 上无必要；串口日志 `LV_LOG_USER("weather fetch: %s", ...)` 保留排障能力。
- 周期：`weather_update_cb` 成功分支 `lv_timer_set_period(timer, 14400000)`（4h）→ `7200000`（2h）。

## 改动（唯一 UI 工程 vendor/allwinnertech/apps/luncher_dm/）

| 文件 | 改动 |
|------|------|
| `ui/ui_home.c` | 周期 14400000→7200000（含注释）；删 `clock_click_cb`/`weather_card_click_cb`/`weather_update_cb`（成功+失败分支）中的 `w_debug_lbl` 更新；删 `create_weather_card` 中 label 创建；worker 注释同步 |
| `deskmate_ui.c` | 删 `w_debug_lbl` 全局定义 |
| `deskmate_ui.h` | 删 `w_debug_lbl` extern |
| `dm_weather.h` | 注释同步（debug 仅串口使用） |

保留：点击天气卡/hero 时钟手动刷新 + 对时功能（仍走 `weather_kick_fetch`）；失败 30s 快速重试；成功后预取下一轮。

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅
2. `lunch_nuttx 2 && pack` ✅ → `rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
3. res.fex 资源三段检查 ✅（字体/WiFi 固件/整包块）
4. 产物一致：nsh.fex == vela.bin（8114528 B）
5. 固化：**ok-20260823-1**（三仓 tag）

## 遗留

- 待上板：烧录 ok-20260823-1 验证天气卡无 debug 行 + 2h 周期生效
- 其余未变（AI 流式化待用户拍板 / ALS 自动亮度 / 健康②层等见 AGENTS.md §三）

---

# DevLog 2026-08-23 — AI 语音本地命令：音量/亮度控制（P115）

## 背景

用户需求：对 AI 说「你好 openvela 声音小一点」→ 音量减小 5%；亮度同理。即 AI 语音链路接入本地 API（声音大小 / 屏幕亮度），不走 LLM。

## 现象 / 架构分析

- 语音链路：PTT 录音 → agent 侧 `voice_channel.c` ASR（batch 与 stream 两路径）→ 广播 `voice_evt(stt)` → push `message_bus` → LLM → TTS。
- ai_agent 是独立进程（`ai_agent &`），无法直接调 UI 的音量/亮度接口；但 LLM 在 agent 侧被调用，若不短路会收到「我无法调整音量」类瞎回 + 浪费 token。
- 结论：**本地命令必须在 agent 侧 ASR 后短路 LLM**，广播 `cmd` 事件给 UI 执行硬件。

## 方案

1. **agent 侧**（`packages/ai_agent/src/voice/voice_channel.c`）：
   - 新增 `voice_local_cmd_parse()`：中文关键词匹配（音量/声音、亮度/背光 + 小/低/暗/减/降 → -5；大/高/亮/加/增 → +5），输出 `"volume:-5"` / `"brightness:+5"`。
   - batch `asr_and_dispatch` 与 stream `voice_channel_stop` 两处 ASR 成功后：命中则 `ws_server_broadcast_voice_evt("cmd", ...)` + 跳过 `message_bus_push_inbound`（短路 LLM）。
2. **UI 侧**（luncher_dm）：
   - `dm_ai.c`：订阅线程处理 `kind="cmd"` → `g_ai_evt_cmd` + seq；新增 `dm_ai_voice_evt_cmd_poll()`。
   - `deskmate_ui.c` `ai_poll_cb`：消费 cmd → `volume` 改 `g_music_volume`（clamp 0-100，同步 music_vol_lbl）；`brightness` 改 `g_screen_brightness` + `hal_pwm_control(4, &pcfg)`（PWM ch4，25000ns，duty=25000*v/100，polarity NORMAL，与 settings 一致）→ `dm_ai_voice_speak("好的，音量已调到 X%")` TTS 确认。
   - 新增共享 `g_screen_brightness`（deskmate_ui.c 定义 75，ui_settings.c settings_brightness_cb 同步，保证 AI 命令与设置页基准一致）。

## 改动文件

| 文件 | 改动 |
|------|------|
| `packages/ai_agent/src/voice/voice_channel.c` | `voice_local_cmd_parse` + 两处短路钩子 |
| `apps/.../luncher_dm/dm_ai.c` / `.h` | cmd 事件存储/seq + `dm_ai_voice_evt_cmd_poll` |
| `apps/.../luncher_dm/deskmate_ui.c` / `.h` | `ai_poll_cb` 执行音量/亮度 + TTS 确认；`g_screen_brightness` 共享变量 |
| `apps/.../luncher_dm/ui/ui_settings.c` | settings_brightness_cb 同步 `g_screen_brightness` |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（仅无关 LED warning）
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅（字体/WiFi 固件/整包块）
4. 产物一致：nsh.fex == vela.bin（8118632 B）
5. 固化：**ok-20260823-2**（三仓 + ai_agent 独立仓手动 commit/tag）

## 遗留

- 待上板：烧录 ok-20260823-2 实测「声音小一点/亮度调暗」→ 音量/亮度 ±5 + TTS 确认；ASR 识别到「你好 openvela」唤醒词场景（P83 已弃用云端唤醒词，当前为 PTT 按钮触发）
- 关键词表可扩展（后续加更多设备命令：WiFi/蓝牙/天气等）

---

# DevLog 2026-08-23 — AI 语音本地命令扩展：音乐控制 + 绝对设置（P116）

## 背景

P115（音量/亮度 ±5%）上板待测期间，用户提出扩展：播放音乐、暂停音乐、上一曲、下一曲，以及音量/亮度绝对设置（"音量调到 50"）。

## 方案

沿用 P115 同构架构（agent 侧 ASR 短路 LLM → cmd 事件广播 → UI 执行 + TTS 确认），只扩展两端：

1. **agent 侧**（`voice_channel.c` `voice_local_cmd_parse`）：
   - 新增音乐命令：`music:next`（下一曲/下一首/切歌/换一首/换歌/下首歌）、`music:prev`（上一曲/上一首/上首歌）、`music:pause`（暂停/停一下/别放/停下）、`music:play`（播放/继续/放音乐/来一首/唱一首）——具体词优先（"播放下一曲" 切歌而非 resume）。
   - 新增绝对设置：`volume:50` / `brightness:80`（解析"调到五十/调到50"、"百分之五十/百分之50"、"50%"），支持中文数字（cn_digit/cn_num：五十=50、二十五=25、一百=100）。
   - 相对步进 `volume:+5/-5` 保留（P115）。
2. **UI 侧**（`deskmate_ui.c` `ai_poll_cb`）：
   - music:play → 已播放中/继续播放（music_resume）/首播（music_play(0) 补扫曲库+恢复上次）三态；pause → music_pause；next/prev → music_album_next(true/false)。
   - volume/brightness 区分绝对（冒号后无 +/- 号 → 直接赋值）与相对（有 +/- → 步进）。
   - 编译修复：deskmate_ui.c 自身不 include deskmate_ui.h，ai_poll_cb（125 行）提前引用 music_playing/music_inited（定义在 1237 行）需前向 extern 声明。

## 改动文件

| 文件 | 改动 |
|------|------|
| `packages/ai_agent/src/voice/voice_channel.c` | `voice_local_cmd_parse` 扩展：音乐命令 + 中文数字解析 + 绝对设置 |
| `apps/.../luncher_dm/deskmate_ui.c` | `ai_poll_cb` 执行分支扩展 + music_playing/music_inited 前向声明 |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（首次失败：music_playing 未声明 → 补前向 extern 修复）
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 产物一致：nsh.fex == vela.bin（8118632 B）
5. 固化：**ok-20260823-3**（三仓 + ai_agent 独立仓手动 commit/tag）

## 遗留

- 待上板：烧录 ok-20260823-3 实测「播放音乐/暂停/下一曲/上一曲」「音量调到五十/亮度百分之八十」
- 关键词表可继续扩展（健康查询/状态播报/WiFi/蓝牙，见 AGENTS.md §三 待办）

*DevLog by AtomCode (deepseek-v4-flash)*
