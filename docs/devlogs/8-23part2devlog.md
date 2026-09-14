# DevLog 2026-08-23 part2 — 审查修复与打磨：review-4~review-9

## 节点

- **review-4** P115/P116 代码审查 + 越界读修复 → ok-20260823-4
- **review-5** 播放命令 AI 不响应 → 声卡冲突修复 → ok-20260823-5
- **review-6** 语音命令表驱动重构 → ok-20260823-6
- **review-7** 锁屏语音命令不生效 → 订阅/消费常驻化 → ok-20260823-7
- **review-8** 串口日志降噪 → ok-20260823-8
- **review-9** 死代码清理 → ok-20260823-9

---

# DevLog 2026-08-23 — P115/P116 代码审查 + 越界读修复（review-4）

## 背景

用户要求 rev code 怕有 BUG，对 P115/P116 全量 diff（vendor 仓 dac9cd0d..765c0b2f + ai_agent 仓 041c696..cfeeb9f）做审查。

## 发现

**🔴 必须修：agent 侧中文数字解析越界读（UB）**——`voice_channel.c` `cn_num()`：
- 输入"音量调到五"（"五"为末字）时 `strncmp(s+3, "十", 3)` 越过 `'\0'` 读 text 缓冲之后内存（`s+3` 已过字符串尾）；`cn_digit(s+6)` 同理。
- 功能上碰巧算对（strncmp 读到垃圾 ≠ "十" 即返回），但属未定义行为，嵌入式不可接受。

**🟡 边界限制（记录不改）**：
1. 语音命令仅在 **AI 子页打开时**生效——`ai_poll_cb` 定时器随子页创建/销毁、`dm_ai_voice_evt_subscribe` 也在子页内启停；锁屏 standby 语音按钮说话时 cmd 事件无人消费（主场景是 AI 子页 PTT，可接受）。
2. 曲库为空时 `music_play(0)` 直接 return，但 TTS 仍播报"好的，开始播放"（播报与事实不符，小瑕疵）。

## 修复

`cn_digit()` 加 `s[0]=='\0'` 早退；`cn_num()` 所有 `s+N` 偏移前用 `strlen(s)` 剩余长度防护（left≥6/≥9 才做 s+3/s+6 读），防止越过 `'\0'` 越界。

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 固化：**ok-20260823-4**（三仓 tag + ai_agent 独立仓 3c6d99a）

---

# DevLog 2026-08-23 — 上板复测"播放"命令 AI 不响应 → 声卡冲突修复（review-5）

## 现象

烧录 ok-20260823-4 上板，按住 PTT 说"播放"：
- 日志：`ASR: 播放。` → `[codec] dapn on`（音乐起播）→ **无任何 `[voice] speak:` 日志**
- 现象：音乐起播了，但 AI 无 TTS 确认回应；随后再说话 ASR 全空文本（`parsed resp but no text`），表现为"AI 不响应了"

## 根因

P115/P116 的 UI 侧 `ai_poll_cb` 音乐命令分支：执行 `music_play(0)` 起播后**立即 `dm_ai_voice_speak("好的，开始播放")` 播报 TTS 确认**。但音乐起播后 XPlayer 独占声卡（P103 实证：`snd_vela_pcm_pause` 不关 handle，`XPlayerStop` 才释放），agent 侧 `audio_playback_open` 必 EBUSY → TTS 确认**静默失败**（无日志）→ 用户听不到 AI 回应 = "AI 不响应"。音量/亮度命令无此问题（不涉及 XPlayer）。

## 方案

**音乐命令不播 TTS 确认**——音乐起播/暂停/切歌本身即反馈，无需语音确认（且确认必然与音乐抢声卡失败）。保留 printf 日志排障。音量/亮度命令的 TTS 确认不受影响（保留）。

## 改动

| 文件 | 改动 |
|------|------|
| `apps/.../luncher_dm/deskmate_ui.c` | `ai_poll_cb` 音乐分支：删除 `dm_ai_voice_speak(ack)`，只执行 music_play/pause/album_next + printf |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 固化：**ok-20260823-5**（三仓 + ai_agent 独立仓 tag）

## 遗留

- 待上板：烧录 ok-20260823-5 复测"播放"→ 音乐起播即反馈（无确认播报属预期）；"暂停/下一曲/上一曲"；音量/亮度命令仍应有 TTS 确认
- 音乐命令确认播报若未来要恢复：需先 force_stop 音乐再播确认（P103 同款），但语义矛盾，暂不做

---

# DevLog 2026-08-23 — 语音命令表驱动重构（review-6，结构性优化）

## 背景

用户反馈"不能头痛治头，结构需要优化"——此前每加一个命令要改 agent 侧 `voice_local_cmd_parse` 的 if/else + UI 侧 `ai_poll_cb` 的 if/else 两处，且"是否播 TTS 确认"每次手写判断（review-5 就是漏了这条踩坑）。重构为**表驱动**。

## 方案

1. **agent 侧**（`voice_channel.c`）：`voice_local_cmd_parse` 改为**匹配器数组**驱动——
   - 每个命令一个 `match_*` 函数（`match_music_play/pause/next/prev/volume/brightness`），命中填 cmd 返回 1
   - `g_local_cmd_matchers[]` 表顺序遍历（顺序即优先级："播放下一曲"→next 优先）
   - 音量/亮度共用 `dev_adjust_parse()`（绝对/相对解析提取一次）
   - 新增命令 = 写一个匹配器 + 表加一行
2. **UI 侧**（`deskmate_ui.c`）：`ai_poll_cb` 命令消费改为**命令表 + 分发函数**——
   - `struct dm_local_cmd { prefix, needs_ack, exec }` 表：music(needs_ack=0) / volume(1) / brightness(1)
   - 每个命令一个执行器 `dm_cmd_*`；`ai_local_cmd_dispatch()` 查表执行 + 按 needs_ack 播确认
   - **needs_ack 成为命令属性**（表字段），不再每分支手写——review-5 教训固化成结构
   - 新增命令 = 写一个执行器 + 表加一行

## 改动文件

| 文件 | 改动 |
|------|------|
| `packages/ai_agent/src/voice/voice_channel.c` | `voice_local_cmd_parse` → 匹配器数组 + `dev_adjust_parse` 共用解析 |
| `apps/.../luncher_dm/deskmate_ui.c` | `ai_poll_cb` 只调 `ai_local_cmd_dispatch()`；命令表 + 执行器 + needs_ack 策略定义在 music 区 |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 固化：**ok-20260823-6**（三仓 + ai_agent 独立仓 4d1697f）

## 遗留

- 待上板：烧录 ok-20260823-6 复测全部命令（音量/亮度 ±5 与绝对、播放/暂停/上一曲/下一曲）行为与 ok-20260823-5 一致（重构无行为变化）
- 后续扩展命令（健康查询/状态播报/WiFi/蓝牙）：agent 加匹配器 + UI 加执行器，各一行表项

---

# DevLog 2026-08-23 — 锁屏语音命令不生效 → 订阅/消费常驻化（review-7）

## 现象

烧录 ok-20260823-6 上板，锁屏（standby）下按住 AI 语音圈说"播放音乐"：
- 日志：`show_standby: Enter standby` → `ASR: 播放音乐。` → codec dapm on（音乐起播？）→ **无 `[voice-cmd] music:play` 日志**
- 现象：用户听到（或以为听到）音乐起播但实际不播放/不生效，再按 PTT 录音 #2 无反馈

## 根因

**cmd 事件消费随 AI 子页启停**：
- `dm_ai_voice_evt_subscribe()`（deskmate_ui.c create_ai_subpage）与 `g_ai_timer`（ai_poll_cb，300ms 轮询消费 cmd）都只在 **AI 子页创建时**存在，退出子页（close_subpage）会 unsubscribe + 删 timer
- 锁屏 standby 的 AI 语音按钮（ui_home.c standby_ai_btn）复用 PTT 回调，用户在锁屏说话 → agent ASR 命中关键词广播 cmd 事件 → **但 AI 子页未开 → 无订阅线程、无 ai_poll_cb → cmd 事件无人消费 → 音乐/音量命令不执行**
- review-4 已记录此边界限制，用户锁屏使用场景触发

## 方案（常驻化）

1. **订阅常驻**：`dm_ai_voice_evt_subscribe()` 提前到 `deskmate_ui_create()`（应用启动即订阅，幂等防重入）；退出 AI 子页**不再 unsubscribe**（订阅线程常驻成本 = 一条 WS 连接，可忽略）
2. **cmd 消费常驻**：cmd 事件消费从 `ai_poll_cb`（AI 子页 timer）移至新的系统级 timer `dm_ai_cmd_poll_cb`（deskmate_ui_create 创建，300ms，不随子页删）→ 调 `ai_local_cmd_dispatch()`
3. stt/llm 文字显示仍由 AI 子页 ai_poll_cb 消费（仅子页显示需要）

## 改动

| 文件 | 改动 |
|------|------|
| `apps/.../luncher_dm/deskmate_ui.c` | `deskmate_ui_create`：+subscribe + 常驻 cmd timer；close_subpage：去掉 unsubscribe；ai_poll_cb：去掉 cmd 消费块；新增 `dm_ai_cmd_poll_cb` |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 固化：**ok-20260823-7**（三仓 + ai_agent 独立仓 tag）

## 遗留

- 待上板：烧录 ok-20260823-7 复测**锁屏下**说"播放音乐/声音小一点"→ 命令生效；AI 子页内命令回归
- 订阅常驻后 ws_server 多一条长连接，观察稳定性（原 P84 设计为省资源，常驻化是功能优先取舍）

---

# DevLog 2026-08-23 — 串口日志降噪（review-8）

## 背景

review-7 上板验证"播放音乐"生效，用户反馈"LOG 噪音好大"——播放/录音时串口被大量调试日志刷屏。

## 噪音源（按刷屏量排序）

| 来源 | 现象 | 处置 |
|------|------|------|
| `volc_asr` 空文本 WARNING | 流式 ASR 每 100ms 一个空中间结果，一次录音刷屏 47 条 `parsed resp but no text` | `#if 0` 关闭（链路已闭环） |
| codec dapm 打点 | `sun8iw20-codec.c` 每次播放/录音 dump 十几行寄存器（PRE_ON/POST_ON/dapm on/off） | `#if 0` 关闭 dump 函数体 + 三处打点 + 序号变量 |
| `[music] writei` 逐包诊断 | `deskmate_ui.c` 每 1000 包打印一次 + WRITE_GAP 异常检测 | `#if 0` 关闭（P73~P113 排障用，已闭环） |
| awplayer WARNING | `WavProbe fail / message 0x40a not handled / checkSampleRate` 等正常流程刷 WARNING | `CONFIG_LOG_LEVEL` 5→6（只留 ERROR） |

## 改动文件

| 文件 | 改动 |
|------|------|
| `apps/.../hal/source/sound/codecs/sun8iw20-codec.c` | `sunxi_codec_out_dump` 函数体 + PRE_ON/POST_ON 打点 + seq 变量 `#if 0` |
| `packages/ai_agent/src/voice/volc_asr.c` | 空文本 dump WARNING `#if 0` |
| `apps/.../luncher_dm/deskmate_ui.c` | `dm_sound_write` writei 打印 + WRITE_GAP 检测 `#if 0` |
| `apps/.../libcedarx/libcore/base/include/cdx_log.h` | `CONFIG_LOG_LEVEL` 5→6（关闭 WARNING 级） |

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 固化：**ok-20260823-8**（三仓 + ai_agent 独立仓 b35dcf2）

## 遗留

- 待上板：烧录 ok-20260823-8 确认播放/录音串口干净（只剩 ERROR 级 + 必要业务日志）
- 排障时需按各文件注释改 `#if 0→#if 1`（codec/volc_asr/writei）或 `CONFIG_LOG_LEVEL 6→5`（awplayer）恢复

---

# DevLog 2026-08-23 — 死代码清理（review-9，结构瘦身）

## 背景

架构评审后用户拍板"按建议开搞"第一步：清理迭代遗留的死代码/占位/备份，防误导后续会话。

## 清理清单

| 项 | 大小 | 问题 | 处置 |
|----|------|------|------|
| `deskmate_ui.c.bak` | 210KB | 纯备份 | 归档 `project_docs/archive/deskmate_ui.c.bak-20260823` |
| `music_service.c/h` | 22KB | 死代码陷阱——`music_service.h` 被 deskmate_ui.c include 但函数零调用，XPlayer 实际在 deskmate_ui.c 直接用，头注释"XPlayer 唯一持有者"与事实相反 | git rm + 删死 include |
| `service/bt_service.c` / `wifi_service.c` | 各 ~400B | 占位空壳，自注释"未完成前禁止编译"（架构先行产物） | git rm |
| `ui/ui_manager.c` | 593B | Phase 3 占位，从未接入 | git rm |
| `ui/` 下 `.o`/`.su` | 21 个文件 | 本地编译残留（git 未跟踪，.gitignore 已覆盖 `*.o`/`*.su`） | find -delete 清理 |

## 改动

- `vendor/allwinnertech` 仓：5 文件 git rm + deskmate_ui.c 删死 include + 归档 .bak
- 无功能改动，纯删除/归档，编译零风险

## 验证

1. `./build.sh .../configs/nsh -j$(nproc)` ✅（无 music_service/bt/wifi/ui_manager 相关 error）
2. `lunch_nuttx 2 && pack` ✅
3. res.fex 三段检查 ✅
4. 固化：**ok-20260823-9**（三仓；ai_agent 仓无改动）

## 遗留

- 第二阶段：音频仲裁层（治本，待做）——统一"停音乐→释放→播→恢复"，根治 P103/P107/review-5 反复发作的抢声卡
- ALS 自动亮度 + AI 流式化（下一批功能）

*DevLog by AtomCode (deepseek-v4-flash)*
