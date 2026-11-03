# P333 — 语音：闹钟增删 / 场景模式 / 健康打卡

2026-11-01；V1 `ynm3000`。用户否决了"查询类"语音（按按钮说话时人已在屏前，
念时间/天气是绕远路），只保留**动作类**。本轮做三组动作，截图不做，猫叫等素材。

## 新增命令

| 说 | cmd | 动作 |
|---|---|---|
| 删掉七点的闹钟 / 取消八点半的闹钟 | `alarm:del:HH:MM` | 按时间匹配后 `dm_alarm_del` |
| 把闹钟全删了 | `alarm:delall` | 逐个删 |
| 关掉闹钟 | `alarm:off` | 全部 `set_enabled(0)` |
| 我有哪些闹钟 / 几个闹钟 | `alarm:list` | 拼 ack："有2个：7点00分(每天)、8点30分" |
| 再睡五分钟 / 贪睡 | `alarm:snooze` | `dm_alarm_snooze()` |
| 我要睡觉了 / 晚安 | `scene:sleep` | 关音乐 + 亮度 20 + 猫睡觉 |
| 我出门了 | `scene:out` | 关音乐 + 亮度 100 + 猫回位 |
| 我回来了 / 到家 | `scene:home` | 亮度 60 + 猫回位 |
| 我喝水了 | `health:water` | `dm_health_water_drank()` + 猫开心 + 播"第N杯" |
| 我起来了 | `health:stand` | `dm_health_sit_stood()` + 播"第N次" |
| 再等会 / 待会 | `health:snooze` | 坐着→久坐贪睡，否则→喝水贪睡 |

## 实现要点

- **matcher 顺序是关键**：`match_alarm_ctl` 必须排在 `match_alarm` 前面，
  否则"删掉七点的闹钟"会被当成"新增七点闹钟"（旧 matcher 只认"点"+数字）。
- **场景与宠物命令的边界**：只有带"我要/我去/准备/该/晚安"才算场景，
  光说"睡觉"是让猫睡（`pet:sleep`）。`match_scene` 排在 `match_pet` 之前。
- **场景复用既有执行器**：`dm_cmd_scene` 内部直接调 `dm_cmd_music("pause")`
  和 `dm_cmd_brightness("20")`，不重写音乐/背光逻辑。
- **打卡是动作不是查询**：`dm_health_water_drank()` 会推首页进度环、落盘、
  并让猫开心一下，一次说完屏幕三处在动——这是决赛演示的价值点。
- 修一个隐式声明：`deskmate_ui.c` 原来没 include `dm_health.h`，
  P333 首次编译出现 7 条 `implicit declaration`，已补。

## 验证

- 编译 + `pack_deskmate.sh ynm3000` 全项 PASS，无新增告警
- 镜像 `output/ynm3000/ynm3000.img` MD5 `86ab3d036a0cc031ffcc167711f4e4da`
- **未刷板**。板端可观测日志：
  `[voice] local cmd: scene:sleep` → `[pet] voice cmd=2`；
  `alarm:list` 的 ack 会经 TTS 播出来

## 待办

- **猫叫**：用户去找素材。接线方案已定——`dm_tone_play("meow_*")` 必须在
  LVGL 线程调，因此挂在 `dm_pet.c` 的事件消费处，不挂 `dm_pet_on_event()`
  （可能来自任意线程）。素材放 `data/res/tones/meow_*.wav`
  （24kHz/16bit/mono，与现有提示音同格式），缺失时静默跳过。
