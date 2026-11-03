# P345 — 快问按钮：毒缓存、时间念 epoch、只上屏不出声

2026-11-02；V1 ynm3000。用户反馈：「语音AI里面 四个按钮 问天气 会报不会说，现在几点也是，还有说个笑话调用也是有问题的。」

## 复现（先用探针量，不改代码）

四个 chips（`ai_chip_click_cb`）走**文本通道**：`dm_ai_ask()` → WS `{"type":"message",…,"chat_id":"ui"}`
→ agent → `{"type":"response"}` → `dm_ai_poll()` → 对话框，全程不碰 TTS。`output/p345/ws_probe.py`
从构建机直连设备 `<private-ip>:28789`：时间 0.01s 回 `…UNIX epoch: 1793587505`（本地 fast-path）；
"讲个笑话" 0.02s 回"请求超时…"；"你叫什么名字" 3.93s 正常回"我叫银月呀"——真实往返 4s，超时是假的。

## 三个根因

1. **毒缓存**：`send_reply:` 无条件 `llm_cache_put(final_text)`，把 `LLM_TIMEOUT_MSG` 当"答案"存了；
   命中后又走同一条路径再 put 刷新 TTL(300s)，毒化**永不失效**，只有重启能救。串口对得上：
   02:43:30 `LLM watchdog: call took 139978 ms` → `No WS client for chat_id=ui`。
2. **时间念 epoch**：`kw_time` 直接映射 `get_current_time`，把 `tool_get_time.c:98` 的
   `…CST (UTC+8), UNIX epoch: 1793…` 原样当回复上屏/播报。
3. **只上屏不出声**：文本通道只 `message_bus_push_outbound`，没有 TTS。

## 改动

- `agent_loop.c` 加 `reply_is_cacheable()`：超时/`⚠`/`AI 不可用` 不进缓存；命中非可缓存内容时丢弃并
  `llm_cache_remove()`（自愈，不用重启）；`llm_cache.c/.h` 新增该接口。
- `agent_loop.c` 加 `local_time_reply()`，fast-path 先认时间 →「现在是11月2日周一，10点52分。」
  （`kw_time` 移出 `s_intents`）；时区沿用 `gmtime_r(t+8h)`，绕开 romfs 缺 zoneinfo。
- 出声：`agent_msg_t` 加 `uint8_t speak`，`ws_server.c` 解析 `"speak":true`，`dm_ai.c` 带 `"speak":1`，
  `agent_loop.c` 加 `speak_async()`（detached 线程，不阻塞 loop），三处挂载（PTT 已播报，按 channel 排除）。

## 验证（代码级）

- 构建通过；镜像 `6cc49ee0637b1b26740cd77ba9083b0b`（51,531,776 字节）；`nsh.fex` 里
  `Evicting poisoned cache entry`、`NL fast path: local time`、`现在是`、`"speak"` 各 1 处。
- 坑：增量构建不跟踪头文件，改 `message_bus.h` 后 `ws_server.o` 不重编、`speak` 会静默丢失；已 touch 74 个源文件重打，日志见 `CC: src/channels/ws_server.c`。
- 待上板：点"现在几点"听到「现在是11月2日周一，X点X分。」；天气先上屏再出声；"讲个笑话"正常讲（4~13s）不再秒回报错。

## 遗留

- 139978ms 偶发：`vela_tls.c` 复用连接读超时压到 10s，`WANT_READ` 两次(20s)判 stale 重连重发，新连接 120s 又跑满；thinking 模型 13s 正卡边界。只让失败不再固化。
- "播放音乐"走 UI 本地曲库（不发 agent），本次没补 TTS。
