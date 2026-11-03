# P349 — 叫它睡觉它不睡；顺带削掉重复取帧的顿挫

2026-11-02；V1 ynm3000。用户反馈："叫银月睡觉，就不睡，叫啊强睡觉才睡，而已会很卡。"

## 现象与取证

串口日志里 `pet_transition_to` 的序列是关键：

```
IDLE    -> SLEEP (voice-sleep)   ← 命令其实收到了
SLEEP   -> IDLE  (time-tick)     ← 下一个 tick 自己爬起来了
CURIOUS -> SLEEP (voice-sleep)
SLEEP   -> IDLE  (time-tick)
```

ASR 侧另有干扰项：说"银月睡觉"被识别成 **"音乐，睡觉。"**（银月 yín yuè ≈ 音乐 yīn yuè）。
但这次仍然命中了 `pet:sleep`（`match_app` 里"音乐"要求带开启动词，所以没被 `app:music` 抢走），
**所以"名字"不是根因，状态机才是**。

## 根因

`pet_core_command(2)` 做了两件事：`g_hold_mode=2`、`pet_transition_to(PET_STATE_SLEEP)`。
但 `g_hold_mode` 只被 `pet_core_can_walk()` 使用（挡走路），**`pet_next_state()` 完全不看它**：

```c
if (g_state == PET_STATE_SLEEP) {
    if (g_nap_active) return PET_STATE_SLEEP;
    if (h >= 6 && h < 22 && energy >= 30) return PET_STATE_IDLE;  /* ← 白天+精力够 = 醒 */
    return PET_STATE_SLEEP;
}
```

白天（6~22 点）精力又够，`sleep_enter` 播完切到 `sleep_loop` 后的第一个 time-tick 就判回 IDLE。
用户看到的就是"刚趴下又起来"。

附带两个隐患：`g_hold_mode` 置位后**从不设 `g_hold_until_ms`**（注释写着"0=无限"），
所以"坐下"实际是永久保持；而 `pet_core_hold_clear()` 也只清 mode 不清时间戳。

## 改动

- `pet_core.c`：新增 `hold_mode_now()`——把到期检查抽出来（原来只写在 `pet_core_can_walk()` 里，
  别的调用点看不到到期）。`pet_core_can_walk()` 改用它。
- `pet_core.c`：`pet_next_state()` 的 SLEEP 分支最前面加
  `if (hold_mode_now() == 2) return PET_STATE_SLEEP;` —— 语音强制睡觉期间，天亮/精力规则不许叫醒。
- `pet_core.c`：新增 `PET_HOLD_SIT_MS`(10min) / `PET_HOLD_SLEEP_MS`(30min)，在
  `pet_core_command()` 的 1/2 分支里置 `g_hold_until_ms`；cmd 3 与 `pet_core_hold_clear()` 一并清零。
  这样"说一次坐下永远不动"和"睡着永不醒"两个极端都堵住了。
- `pet_assets.c`：`streak>0` 的素材只再试 **1 次**（原 4 次）。实测单次读损坏概率 p≈0.93，
  4 次重试只把成功率从 7% 抬到 25%，却让每次取帧固定花 4×78KB NAND 读 + 8 次 CRC32
  （约 30~50ms，全在 LVGL 帧回调里）——就是 `round max` 100ms 级顿挫的来源。

## 验证

- `pet_core.c` / `pet_assets.c` 均在构建日志里有 `CC:`；`check_deskmate.py` PASS。
- 镜像 `f9c9c6d88d8e6153f573f3ca34586267`（51,531,776 B）。
- `nsh.fex` 能搜到 `voice cmd=%d hold=%d until=`、`hold expired`；`blue-hour` 仍为 0。
- 待上板：说"银月睡觉"后应看到 `voice cmd=2 hold=2 until=1800000ms`，之后**不再出现**
  `SLEEP -> IDLE (time-tick)`；30 分钟后才允许自然醒。

## 遗留

- 猫的动画仍然取不到帧（`missing frame` 持续），根因是 NAND/romfs 读回坏字节，
  按 AGENTS §五 不在应用层打补丁。本次只是把每次失败的代价从 ~50ms 压到 ~10ms。
- "银月"→"音乐"的同音误识别没动（本次未造成错误动作）。若以后真的被 `app:music` 抢走，
  再考虑把 `match_pet` 提到 `match_app` 前面。
