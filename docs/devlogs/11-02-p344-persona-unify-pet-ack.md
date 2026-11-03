# P344 — 人设统一为"银月"，宠物动作先应声再做

2026-11-02；V1 ynm3000。用户反馈：一会儿叫银月、一会儿啊强、一会儿又变成"猫在说话"，很不统一；
并希望语音叫猫做事时先回一句"好的"再动。

## 根因

- 系统提示词开头写"你是银月"，但 `USER.md` 里 `姓名：啊强` 是**用户**的名字，模型时常拿来当自己的。
- `memory_store.c` 的 `default_soul` 写"我是 AI Agent，运行在 Vela 上的 AI 助手"，被 `append_file`
  追加在身份段之后 → 两套身份互相打架。
- 猫的形象和助手被当成两个角色：模型会说"猫在舔毛""它趴下了"，第三人称指代自己；宠物默认名又是
  "旺财"，与银月无关，屏幕上还挂着这个名字。

## 改动

- `context_builder.c`：身份段重写为「# 银月」——明确"桌面上那只猫就是你本人的样子"、"全系统只有银月
  一个名字（不是 AI/助手/Agent/小八/旺财/阿强）"、"用户叫啊强、称他主人"、"说话用第一人称"。同时改
  `memory_store.c` 的 `default_soul`，镜像另预置 `config/SOUL.md`。
- 宠物默认名 `旺财`→`银月`（`pet_core.c`/`deskmate_ui.c`/`ui_pet.c`/`ui_settings.c`/`dm_pet.h`）；
  聊天气泡说话人 `AI`→`银月`（`deskmate_ui.c` 两处 `ai_chat_append`）。
- `voice_channel.c`：`voice_local_cmd_ack()` 覆盖面从 `app:` 扩到 `pet:`；新增 `voice_local_cmd_run()`
  统管顺序——`pet:` **先应"好的"再广播 cmd**（用户要的先后），其余仍"先执行再应声"（页面/硬件不能
  等 TTS）。`app:music` 继续例外。
- 新增舔爪：`match_pet` 加 `舔毛/舔舔/梳毛 → pet:groom`；`dm_pet.h` 加 `DM_PET_CMD_GROOM`，`dm_pet.c`
  走 `pet_core_command(4)`；`pet_core.c` case 4 只请求 `PET_ACTION_GROOM_PAW`（一次性，2.06s），不动
  状态机与 hold；`deskmate_ui.c` 的 `dm_cmd_pet` 加 groom 分支。

## 验证（代码级）

- 构建通过；`vela.bin == nsh.fex`；镜像 `9c657efde1d1bf7ea13aa5d991dea81e`（51,531,776 字节）。
- `vela.bin` 里 `银月` 8 处、`pet:groom`/`voice-groom`/`舔舔~` 各 1 处，`旺财` 只剩"不要自称…旺财"的否定列举。
- `usrdata.fex` 里能读到新 `SOUL.md`（"我是银月，住在这台桌面设备上的猫"），Key/WiFi 预置未受影响。
- 待上板：问"你是谁"应答银月、不自称 AI、不叫用户阿强、气泡显示"银月"；说"睡觉"先听到"好的"再入睡，
  说"舔舔"出现舔爪动作。

## 遗留

- `ensure_file` 只在文件缺失时写入：只刷 `nsh.fex`（不动 usrdata）时旧 `SOUL.md` 会留着，整包刷不受影响。
