# P343 — 猫"说英语"：模型漏出的 tool_call 包装被念了出来

2026-11-02；V1 ynm3000。用户反馈"跟这猫交互，会说英语很奇怪"。

## 现象与证据

- 串口不打印回复正文，改从板端会话历史取证：`cat /data/ai_agent/sessions/tg_voice.jsonl`。
- 回复其实是中文，但被模型自己包了一层 `<tool_call><function=tts_audio><parameter=text>走走走，我陪你…</parameter></function></tool_call>`。
- 原样送 TTS，标签被念成 "tool call function tts audio parameter text"，听感就是"突然说英语"；
  同一轮还伴随 `[xiaomi_tts] HTTP 400 (body 98 bytes)`。
- 复现条件：本轮没下发 tools（`fast voice path`），模型仍自造 `tts_audio`。代码里没有这个工具，是 MiMo
  自带的"语音助手"话术；一旦进了历史还会被后续轮模仿。

## 改动

- `core/agent_loop.c`：新增 `extract_spoken_text()`。有 `<parameter=text>` 就取其中正文（找不到闭合
  标签则取到 `</function>` 或串尾），没有就退化成剥掉所有 `<...>` 标签，并去首尾空白。
- 同上：`voice_stream_flush()` 先取正文再 `voice_channel_speak()`，取空不算一句，避免"只有标签"的
  流被误判为成功；新增 `voice_spoken_len()`，分句阈值按"去掉标签后的字数"算，否则 46 字节标签会把
  60 字强切提前触发、第一句只剩半截；`v->buf` 每写一字节补 `\0`，保证 `strstr` 安全。
- 同上：`dispatch_response()` 统一清洗 `final_text`，让 UI、会话历史、TTS 拿到同一份干净文本，断掉"历史里的 XML 继续被模仿"的回路。
- `core/context_builder.c`：Rules 加一条"直接输出要说的话，不要包 `<tool_call>`/`<parameter=text>` 标签，也不要自造工具名（如 tts_audio）"。

## 验证（代码级）

- 宿主侧用真实样例跑 `output/p343/test_extract.c`：整段 XML 169B→87B 中文、尾部残留 98B→63B、
  未闭合 101B→54B、半个标签 27B→空、纯中文原样透传。
- 构建通过；`vela.bin == nsh.fex`；镜像 `702dafa4e6ad2d55ba4c9b9578fcaed2`（51,523,584 字节）。
- `vela.bin` 里 `tts_audio` 提示词与新日志串各 1 处；`usrdata.fex` 的 config.json 已预置 Key 与
  `wifi_ssid`/`wifi_pass`（与 `etc/wifi/wapi.conf` 一致），开机不再报 `No saved WiFi credentials`。

## 待上板验证

1. 再问同类问题，听到的是中文正文，不再有 "tool call / parameter text"。
2. `[xiaomi_tts] HTTP 400` 是否消失（尖括号曾是最可疑的诱因）。
3. 会话历史里 `tg_voice.jsonl` 的 assistant 行是否已是纯中文。

## 遗留

- 治标：取正文。治本要模型别自造工具，提示词只降低概率、不保证；若整轮全是标签没有正文，`dispatch_response()` 会当失败处理并回"AI 服务请求失败"，需观察是否误伤。
