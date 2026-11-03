# P351 「叫它睡觉它不睡」的真凶：hold 被语音播报阶段打断

> P349 已经把 `g_hold_mode` 接进了 `pet_next_state()` 的 SLEEP 分支，但用户复测仍是"不睡"。
> 这次直接从串口日志按毫秒对齐，找到了 P349 没覆盖到的另一条路径。

## 证据（`logs/serial/latest.log`，2026-11-02 03:45）

```
03:45:10.938  pet_transition_to: [pet] CURIOUS -> SLEEP (voice-sleep)
03:45:10.946  pet_core_command: [pet] voice cmd=2 hold=2 pet_core.c:313
03:45:11.369  pet_set_action:   [pet] select action=wake instance=84 reason=voice-phase
03:45:51.437  pet_transition_to: [pet] SLEEP -> IDLE (time-tick)
```

`hold=2` 明明设上了，**0.42 秒后**动作就从 `sleep_enter` 被换成了 `wake`。
另外 7 次 `voice cmd=2 hold=2` 全部复现同样的模式（01:59:56 / 02:00:26 / 02:20:35 /
02:20:40 / 02:38:24 / 03:45:10 / 03:45:14）。

## 链路

1. `pet_core_command(2)` → `g_state=SLEEP`、`pet_request_action(SLEEP_ENTER)`，正常。
2. 助手要回一句"好，我去睡了"，TTS 起播时上层发 `PET_EVT_VOICE_PHASE(3)`（SPEAK）。
3. `PET_EVT_VOICE_PHASE` 里 `pet_request_action(SPEAK,"voice-phase")`。
4. `pet_request_action()` 的过渡分支：

   ```c
   if ((g_action==PET_ACTION_SLEEP_ENTER || g_action==PET_ACTION_SLEEP_LOOP) &&
       action!=PET_ACTION_SLEEP_ENTER && action!=PET_ACTION_SLEEP_LOOP) {
       g_after_transition=action;pet_set_action(PET_ACTION_WAKE,why);return;
   }
   ```

   当前动作是 `SLEEP_ENTER`，请求的是 `SPEAK` → 挂起 SPEAK，先播 **WAKE**。
5. `pet_core_animation_done()` 收尾那句 `g_state=PET_STATE_IDLE;`（以及 `g_voice_active`
   时的 `g_state=PET_STATE_CURIOUS`）把状态从 SLEEP 写走。
6. 之后 `pet_next_state()` 只在 `g_state==SLEEP` 分支里认 hold，状态已经不是 SLEEP，
   hold 再也起不了作用 → 下一个 time-tick 判 IDLE。

**所以 P349 的早退是必要的，但不充分**：它守住的是"还在 SLEEP 时别被叫醒"，
守不住"已经被叫醒之后再拉回来"。

## 修改（`pet_core.c` 四处）

1. `pet_next_state()` 开头让 hold 自愈 —— 除进食/玩耍/被摸/庆祝这类有固定时长的状态外，
   `hold==2` 一律拉回 `SLEEP`。这条是兜底：任何路径把状态写走，下一拍都能回来。

   ```c
   if (hold_mode_now() == 2 &&
       g_state != PET_STATE_SLEEP &&
       g_state != PET_STATE_EAT && g_state != PET_STATE_PLAY &&
       g_state != PET_STATE_TOUCH && g_state != PET_STATE_CELEBRATE)
       return PET_STATE_SLEEP;
   ```

2. `PET_EVT_VOICE_PHASE` 开头早退 —— 睡觉期间整个播报阶段不动它（治本，去掉 0.42s 那次抖动）：

   ```c
   if (hold_mode_now() == 2) break;
   ```

3. `pet_nap_tick()` 的 `nap-done` 分支加 hold 检查 —— 这条路径直接
   `pet_transition_to(IDLE)`，绕过 `pet_next_state()`。打盹中被语音强制睡觉，
   原来几分钟后会自己爬起来。

4. `pet_wake_for_interaction()` 里 `pet_core_hold_clear()` —— 手摸/拖它就是要它醒，
   别让语音 hold 再把它按回去。

## 验证

- 板端期望日志（说"银月睡觉"后 60 秒内）：
  - `voice cmd=2 hold=2 until=1800000ms`（30 分钟）
  - `IDLE -> SLEEP (voice-sleep)`
  - **不再出现** `action=wake ... reason=voice-phase`
  - **不再出现** `SLEEP -> IDLE (time-tick)`
- 摸它一下应立刻出现 `SLEEP -> IDLE (wake-interact)`，且之后不再自动睡回去。
- 镜像 `91d1db40` 待刷。

## 遗留

- 用户提到的"叫银月睡觉不睡、叫阿强睡觉才睡"：7 次 `voice cmd=2` 说明两种说法**都**进了
  `pet_core_command(2)`，差异不在这条链上。最可能是 ASR 把"银月"识别成"音乐"，
  进而走了别的话术分支（同音词问题，见 P349 遗留）。
- 宠物取帧仍在 NAND 读路径上失败（`PNG input mismatch` 成串出现，单次 `+1350ms`），
  这是 AGENTS §五 明确不许在应用层打补丁的部分。
