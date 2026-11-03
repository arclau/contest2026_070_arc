# P331 — 语音控制宠物：散步 / 坐 / 睡 / 吃 / 停

2026-11-01；目标平台 V1 `ynm3000`。用户诉求："阿强 走路"→猫全屏走 5 分钟回右下角；
"坐下/睡觉/吃饭"→照做。要求走本地链路，不经过大模型。

验收：板端说一句话，猫做出对应动作并播 ack；5 分钟后自己走回漫游区。

## 链路（全部复用既有设施，零新增线程）

```
PTT → ASR 文本
  → voice_local_cmd_parse()            voice_channel.c:833  （表驱动 matcher）
  → match_pet() 命中 → "pet:walk:300"   voice_channel.c（本轮新增）
  → ws_server_broadcast_voice_evt("cmd") 
  → dm_ai_cmd_poll_cb → ai_local_cmd_dispatch()   deskmate_ui.c:1955（已有表）
  → dm_cmd_pet() → dm_pet_command()     （本轮新增分支）
  → pet_core_command() / pet_view_stroll_start()
```

**不经过 LLM**：零 token、零额外网络往返（只有 ASR 一次），断网也能演。

## 本轮实现

### 1. agent 侧命令识别（`packages/ai_agent/src/voice/voice_channel.c`）

`match_pet()`：动作词表 + 防误触发两条硬约束。

- 词表：散步/走路/走两步/走走 → `pet:walk`；坐下/坐着 → `pet:sit`；
  睡觉/去睡 → `pet:sleep`；吃饭/吃东西 → `pet:eat`；停下/回来/别走了 → `pet:stop`
- **不匹配宠物名**：名字只有 UI 知道，且"阿强/阿墙"同音字会误伤，只认动作词
- 整句 ≤12 汉字、动作词后剩余 ≤3 汉字（"我睡觉之前先刷牙"不算命令）
- "走五分钟"→ 复用现成的 `cn_num()`（voice_channel.c:581）解析中文数字，
  输出 `pet:walk:300`

### 2. 宠物层（`dm_pet.c/h` + `pet_core.c` + `pet_view.c`）

- `dm_pet_command(cmd, seconds)` 新门面，LVGL 线程内直调（cmd 轮询 timer）
- `pet_core_command()`：sit/sleep/stop 走 `pet_transition_to()` + `pet_request_action()`；
  新增 `g_hold_mode`，置位时 `pet_core_can_walk()` 为假 → 坐着/睡着不会自己走开
- **散步模式**（walk 的核心，`pet_view.c`）：
  - 新状态 `g_stroll_until_ms`，与既有 `g_free_until_ms` 互斥
  - 期间满屏随机选点、**2D 走**（位移插值本来就支持 x/y 同时变），
    每段之间坐 4~8 秒（复用 `g_idle_deadline`），上 1/4 屏让开状态栏
  - 到点把 `g_free_until_ms` 置 1（非 0 且已过期）→ 交给**既有**的
    "自由位置到点 → 走回漫游区"分支（pet_view.c:356），不重写回位逻辑
  - 拖拽/抚摸/逗猫/投喂弹窗期间不迈步（沿用原守卫条件）

## 验证与下一步

- 编译 + `pack_deskmate.sh ynm3000` 全项 PASS；无新增告警
- 镜像 `output/ynm3000/ynm3000.img` MD5 `ef9a478865b43d52c7b229950b228abb`
- **未刷板**：动作是否真的"听话"、散步节奏好不好看、5 分钟后回位是否准，
  都要板端看。可观测日志：
  `[voice] local cmd: pet:walk:300` → `[pet] stroll start 300s` →
  `[pet] select action=walk` → 到点 `[pet] stroll stop` → `settle` → `idle_sit`
- 已知取舍：sleep 的"保持"依赖 `g_hold_mode`，若引擎因属性（精力过低/互动宽限）
  自己切走，本轮不拦；等板端看表现再决定是否强锁。
