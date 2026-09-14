# DevLog 2026-08-23 part3 — 音频仲裁层（review-10，治本）

## 节点

- **review-10** 音频仲裁层：统一"停音乐→释放声卡→播 TTS/提示音→恢复音乐"，根治 P103/P107/review-5 抢声卡 → ok-20260823-10（三仓 + ai_agent 独立仓 4419441）

---

# DevLog 2026-08-23 — 音频仲裁层（review-10，第二阶段·治本）

## 背景

用户拍板"开始优化整体"——对应 review-9 遗留的「第二阶段：音频仲裁层（治本）」。此前 P103/P107/review-5 反复发作的"抢声卡"（EBUSY）问题，靠三处散落的 `music_audio_force_stop()` 打补丁，且**只停不恢复**（音乐一旦被语音/提示音打断就永久停掉）。本次统一收口为一个仲裁层，并补齐"恢复"能力。

## 现状调查（声卡持有方）

声卡 `hw:audiocodec` 是单独占设备，三个持有方各自 `open`：

| 持有方 | 进程 | 释放点 |
|--------|------|--------|
| 音乐 XPlayer | luncher_dm | `XPlayerStop` → `PlayerClear`（关 sink 释放）；**`XPlayerPause` 只 `snd_vela_pcm_pause` 不关 handle（P107）** |
| TTS（agent） | ai_agent 独立进程 | `voice_channel_speak` 内 `audio_playback_open/close` |
| 提示音 dm_tone | luncher_dm 后台线程 | `dm_tone_worker` 内 `open/drain/close` |

抢声卡根因：音乐**播放或暂停都占声卡**（pause 不释放），前台音频（TTS 确认/健康提示音）`open` 必 EBUSY 静默失败。

## 关键事实（读 libcedarx xplayer.c 确认）

- `XPlayerStop` → `PlayerStop` + `PlayerClear`（清空媒体+关 sink），此后状态 `STOPPED`。
- `XPlayerStart` 仅接受 PREPARED/STARTED/PAUSED/COMPLETE 态，**STOPPED 态返回 -1**。
- ⇒ 「恢复」不能用 `music_resume()`（`XPlayerStart` 会失败），必须 `music_play(当前曲)` 整曲重播（内部 destroy 旧 player 重建）。

## 方案（仲裁层）

`dm_audio_fg_acquire()` / `dm_audio_fg_release()` / `dm_audio_fg_poll()`（deskmate_ui.c）：

1. **acquire**（前台音频申请声卡，LVGL 主线程）：音乐播放中才记录待恢复（`g_audio_music_resume=1` + 挂起时刻），随后统一 `music_audio_force_stop()` 释放声卡。
2. **完成信号**（worker/事件线程只置 `volatile` flag，主线程 300ms poll 消费）：
   - 提示音：`dm_tone_worker` 播完置 `g_audio_tone_done`。
   - TTS：agent `voice_channel_speak` 每次结束广播 `voice_evt kind=done` → dm_ai 事件线程置 `g_ai_speak_done` → `dm_ai_voice_evt_done_poll()`。
3. **release**（主线程）：恢复音乐 = `music_play(music_track_id)`；挂起超 30s（无完成信号，如 PTT 空文本）放弃恢复，避免音乐迟到复活。

## 改动文件

| 文件（仓） | 改动 |
|-----------|------|
| `apps/.../luncher_dm/deskmate_ui.c`（vendor） | 新增仲裁层 3 函数（acquire/release/poll）+ `g_audio_tone_done`；PTT 按下、`dm_tone_play`、音量/亮度 ack 三处 `force_stop` → `dm_audio_fg_acquire()`；`dm_ai_cmd_poll_cb` 追加 `dm_audio_fg_poll()`；`dm_tone_worker` 播完置位 |
| `apps/.../luncher_dm/dm_ai.c`（vendor） | 事件线程新增 `kind=done` 分支（在 `text[0]` 判空外，因 done 无 text）+ `g_ai_speak_done` + `dm_ai_voice_evt_done_poll()` |
| `apps/.../luncher_dm/dm_ai.h`（vendor） | 声明 `dm_ai_voice_evt_done_poll()` |
| `packages/ai_agent/src/voice/voice_channel.c`（ai_agent） | `voice_channel_speak` 6 个出口（成功/流失败/open 失败/跳过）补 `ws_server_broadcast_voice_evt("done","")` |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（LD: nuttx → vela.bin，无 error；voice_channel.c .o 06:58 晚于源 06:57，确认 ai_agent 已重编）
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅（字体/FW 在 res.fex 内、镜像含完整 res.fex 块）
4. 固化：**ok-20260823-10**（三仓 tag + ai_agent 独立仓 commit 4419441 / tag ok-20260823-10）

## 遗留

- 待上板：烧录 ok-20260823-10 复测——①PTT 说话 → AI 播报完**音乐自动恢复**（重播当前曲）；②健康提示音/欢迎语播完音乐自动恢复；③"音量调到五十"确认播报完音乐自动恢复；④暂停的音乐被语音打断后**保持停止**（不自动恢复）
- 位置不保留：恢复 = 整曲重播（XPlayer STOPPED 态无法续播），如需续播需 prepare+seek，暂不做
- 30s 超时放弃恢复：PTT 空文本（无 speak）场景音乐不恢复（与旧行为一致，无回归）

---

# DevLog 2026-08-23 — 死机修复：force_stop 后 XPlayerStart 崩溃（review-11）

## 现象

烧录 ok-20260823-10 上板：WiFi 0x15 已消失（`Network connected: 192.168.*.*` 真实 IP——res 分区确认上板，0x15 根因 = 烧录环节 res 未写入，镜像本身经验证无问题）。新问题：**连续两次 PTT 说"播放音乐" → 第二次语音命令后约 6s，XPlayer 线程 `Prefetch abort`（PC a04e8f7e）→ luncher_dm 死机**。

## 根因

- 第 1 次"播放音乐"→ `music_play` 起播；第 2 次 PTT 按下 → `dm_audio_fg_acquire()` → `music_audio_force_stop()` → **`XPlayerStop` → `PlayerClear` 清空媒体+关 sink（STOPPED 态）**
- 第 2 次语音命令 play → `dm_cmd_music("play")` → `music_inited=true` → **`music_resume()` → `XPlayerStart(STOPPED 态 player)`** → XPlayer 内部对已销毁组件调用 → Prefetch abort（设计仲裁层时已读 xplayer.c 确认 STOPPED 态 Start 无效——恢复本应走重建，但语音命令 play 分支漏了，只走了 music_resume）
- 该路径旧代码同样存在（PTT force_stop 是旧行为，review-5 后音乐命令仍走 music_resume），本次时序上板触发

## 修复（deskmate_ui.c，6 处）

| 改动 | 内容 |
|------|------|
| `g_music_player_cleared` | 共享区新增标志：1 = XPlayer 已被 force_stop clear（XPlayerStart 不安全） |
| `music_audio_force_stop` | `XPlayerStop` 后置 `cleared=1` |
| `music_audio_start` | `XPlayerCreate` 成功后置 `cleared=0` |
| `music_audio_resume` | `XPlayerStart` 加 `!cleared` 防护（防御所有恢复路径） |
| `music_audio_poll` | PREPARED→Start 加 `!cleared` 防护 |
| `music_resume` | cleared 时走 `music_audio_start(当前曲)` 重建播放器，否则正常续播 |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（exit=0 无 error，vela.bin 08:55）
2. `lunch_nuttx 2 && pack` ✅
3. `bash /data/vela/check_res.sh` ✅（romfs 路径级验证通过）
4. 固化：**ok-20260823-11**（三仓；ai_agent 无改动）

## 遗留

- 待上板：烧录 ok-20260823-11 复测——①音乐播放中 PTT 说"播放音乐"→ 重建重播**不崩**；②音频仲裁恢复项全回归（PTT→AI 播完音乐自动恢复、提示音/欢迎语播完恢复、音量亮度确认播完恢复、暂停被打断保持停止）

---

# DevLog 2026-08-23 — XPlayer 生命周期审计 + 同类隐患防御加固（review-12）

## 背景

review-11 死机修复后用户问「类似的 BUG 能不能提前修改」——对全部 XPlayer API 调用点做系统性审计，提前消除同类隐患，不等上板再炸。

## 审计结论（deskmate_ui.c 全部 14 处 XPlayer 调用点逐一核对 + xplayer.c handler 行为确认）

| API | 调用点 | STOPPED 态行为 | 处置 |
|-----|--------|---------------|------|
| `XPlayerStart` | resume / poll | 非法 → **崩溃**（review-11 死机真凶） | ✅ review-11 已加 `!cleared` 防护 |
| `XPlayerStop` | force_stop | 设计目标 | ✅ |
| `XPlayerPause` | music_pause | 返回 -1，安全 | 🟡 review-12 加防护 |
| `XPlayerGetCurrentPosition` | music_timer_cb | 返回 `mTimeMsBeforeStop`，安全 | 🟡 review-12 加防护 |
| `XPlayerGetDuration` | music_timer_cb | 允许列表含 STOPPED，mediaInfo NULL 给 0 | 🟡 review-12 加防护 |
| `XPlayerReset` | 新建后 SetDataSourceUrl 失败 | 设计用于任意态（cancel prepare/seek） | ✅ |
| `XPlayerDestroy` | 重建路径 | 设计用于终态（P14 双 free 教训注释内） | ✅ |

**核心结论：会崩的只有 `XPlayerStart`，其余 API 在 STOPPED 态安全返回——无第二颗雷。**

## 其他审计项

- `g_music_sound`：仅 5 处引用（声明/置 NULL/create/SetAudioSink），无二次 free 路径 ✓
- `music_timer_cb` 在 cleared 窗口：force_stop → music_pause 停 timer + `music_playing=false`，重建前 timer 不跑，无暴露 ✓
- 双核（CPU0/1）竞态：沿用现有 volatile 标志模式，无新增风险 ✓

## 防御加固（deskmate_ui.c 3 处）

| 改动 | 内容 |
|------|------|
| `music_audio_pause` | `XPlayerPause` 加 `!g_music_player_cleared` |
| `music_audio_position` | `XPlayerGetCurrentPosition` 加 `!cleared` |
| `music_audio_duration` | `XPlayerGetDuration` 加 `!cleared` |

效果：**所有 XPlayer 调用点在 player 被清空后都不会碰它**，同类崩溃从结构上杜绝。

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（exit=0 无 error，vela.bin 09:03）
2. `lunch_nuttx 2 && pack` ✅
3. `bash /data/vela/check_res.sh` ✅（romfs 路径级验证通过）
4. 固化：**ok-20260823-12**（三仓；ai_agent 无改动）

## 遗留

- 待上板：烧录 **ok-20260823-12** 复测——①音乐播放中 PTT 说"播放音乐"→ 重建重播**不崩**（review-11 死机点）；②音频仲裁恢复项全回归（PTT→AI 播完音乐自动恢复、提示音/欢迎语播完恢复、音量亮度确认播完恢复、暂停被打断保持停止）
- 后续可继续审计：ai_agent WS 连接状态机、蓝牙 H4 生命周期（用户可选方向）

*DevLog by AtomCode (deepseek-v4-pro)*
