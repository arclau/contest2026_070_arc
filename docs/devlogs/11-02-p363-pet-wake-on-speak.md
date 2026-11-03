# P363 语音回复时宠物不起身（顺带：语音事件订阅自愈）

目标：AI 回复时猫先起来说话、说完再睡；查清语音事件到底有没有到宠物。
平台：V1 `ynm3000` / `luncher_dm` + `ai_agent`。

## 一、现象与证据

用户："AI 回复的时候这猫一直在睡觉，应该先起来说话，说完再睡觉。"

- 11:21 那轮：串口里**一条宠物动作都没有**（无 `select action`、无 `pet_transition_to`），
  而 09:50 那轮有 `select action=listen/speak reason=voice-phase`。
- 11:32 从主机直连板端 28789 发 `{"type":"voice","action":"speak"}`：agent 确实广播了
  `voice_evt pet_speak`（我的客户端收到），板端仍然没有任何宠物动作。
- `/proc/net/tcp`（adb <private-ip>）：127.0.0.1:27702 ↔ 28789 是 ESTABLISHED 且 rxsz=0
  —— 连接在、数据也被读走了，但宠物没反应。**结论只到"事件没落到宠物"，断点未定位。**

## 二、改动

`luncher_dm/dm_ai.c`
- 订阅保活：300ms 定时（`dm_ai_voice_evt_keepalive`）每 10s 发 WS ping，25s 收不到任何帧
  就 `shutdown` 逼 worker 重连。
- 收到任何 voice_evt 打印 `[voice-evt] <kind>`——这条链路以前静默失效时串口一行都没有。
- 自愈判据用 worker 心跳 `g_ai_evt_beat`（每转一圈自增，含重连重试的 usleep），
  心跳停滞 >15s 才判定线程已死。**不能用 `g_ai_evt_fd < 0`**（见第四节）。
- `dm_ai_voice_evt_subscribe()` 加 `g_ai_evt_threads` 守卫：线程还活着就拒绝再建。

`ai_agent/ws_server.c`
- 广播发不出去时打 WARNING `voice_evt(<kind>) dropped: no client`（原来是 INFO，被
  `setlogmask(LOG_UPTO(LOG_NOTICE))` 过滤）；客户端注册打 NOTICE `client+ fd=N`。

`luncher_dm/pet_core.c`
- 睡着被叫醒说话 → `g_voice_resume_sleep`；`VOICE_PHASE(0)` 直接回 SLEEP
  （`voice-sleep-back`，不先回 IDLE，避免"挑个待机动作再躺下"闪两下）。
- **hold=2 只压住"去睡觉"那一声回应**：`g_hold_voice_mute` 在 `pet_core_command()`
  收到 cmd=2 时置位，`VOICE_PHASE(0)`（这一轮播完）解除，另有 15 秒兜底。
  用 `hold_voice_muted()` 替换原来的无条件 `if (hold_mode_now()==2) break;`。

## 三、第一版刷机实测（镜像 ef873ec5）

事件链路恢复：11:40:14/16/31 有 `select action=listen/think/speak reason=voice-phase`。
但用户反馈"越改越坏"，串口给出两个独立问题：

1. **每个事件送两遍**（我自己引入的，见第四节）：同一 tick 两次
   `select action=speak instance=22 / instance=23`、`listen 25/26`、`think 27/28`；
   `[voice-evt] pet_speak` 同一秒两行；`/proc/net/tcp` 两组 ESTABLISHED（5593、5594）。
   宠物动作被自己打断重来，帧缓存反复冲掉 → `missing frame` 15、`PNG input mismatch` 42
   （正常启动是 4 左右）。
2. **11:40:39 猫躺下不是 bug**：`ASR: 维纳睡…`（识别串了）→ 本地词表命中"睡" →
   `voice cmd=2 hold=2 until=1799999ms`。问题在那之后：30 分钟 hold 让后面每一轮
   语音都进不了动作分支，用户看到的就是"回复时猫一直在睡觉"。上一版把它误判成
   "08:21 遗留的陈旧 hold"——**归因错了**，08:21 那条 30 分钟早过期了。

## 四、自己踩的坑：双订阅

第一版自愈写的是 `if (g_ai_evt_fd < 0 && 心跳停滞 > 10s) 重启订阅线程`。
但开机时 agent 还没起来，fd 本来就是 -1；重连重试期间也一直是 -1。于是反复重启
订阅线程 → 两条订阅并存。

修法两层：
1. 判据换成心跳 `g_ai_evt_beat`，线程活着就一定会推进。
2. `subscribe()` 内部加 `g_ai_evt_threads` 守卫：`run=0` 后老线程还没退出时
   调用会被拒绝，等老线程自己退（≤30s），下一拍 300ms 定时再订阅。
   **"fd 为负"不等于"线程死了"；单例守卫要放在被调方，不能只靠调用方自觉。**

## 五、最终镜像

`pack_deskmate.sh ynm3000` PASS，`vela.bin == nsh.fex`（a8dde7bc），
distclean 后全量重编重打（用户要求，res 分区异常）：镜像 53,149,696 B，
MD5 `ccb08b241d3170d5b9a43d4880ad8fbb`，`vela.bin == nsh.fex`（9703827a），待刷。

板上验收（说一句长回复，再看一句"睡觉"）：
1. `[voice-evt] pet_listen/pet_think/pet_speak` **每个 kind 只一行**；`client+ fd=` 只一条常驻。
2. `select action=listen/think/speak reason=voice-phase` **每个相位只选一次**。
3. 播完 `-> SLEEP (voice-sleep-back)`（睡着被叫醒的场景）。
4. 说"睡觉"后 3～5 秒再问一句 → 猫应起身回答（hold 不再压住新一轮）。
5. `missing frame` 回到个位数；`PNG input mismatch` 只剩已知坏页。
