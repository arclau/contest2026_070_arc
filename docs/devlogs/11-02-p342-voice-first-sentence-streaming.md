# P342 — 首音前移：LLM 流式 + 首句先出声

2026-11-02；V1 ynm3000。针对 P341 量到的 4.3s 首音，改"什么时候能出声"，不换服务商。

## 结论

- 阻塞式 LLM 让首音必然等于 `ASR + 整段生成 + TTS 首包`；改 SSE 增量后第一句一到就合成，不必等后面几句生成完。
- 但**不能每句都切**：每多切一句就多付一次 TTS 首包（约 920ms），三段回复切三次反而更慢。故只早切第一句，
  剩余内容在流结束时一次性合成。另：`app:*` 本地命令此前完全不出声，现补一句"好的"。

## 改动

- `llm/llm_proxy.c`：新增 `llm_chat_stream()`。SSE 行缓冲 + 解析 `choices[0].delta.content`，累积全文并回调
  增量；`"stream": 1`、不带 tools；`xiaomimimo.com` 强制 `thinking.type=disabled`（与非流式一致）；非 443
  或走代理直接返回 ERROR，逼调用方回落阻塞路径。
- `core/agent_loop.c`：仅 `SIMPLE + voice 通道 + 无工具` 走流式，失败自动回落阻塞路径。分句：句末标点必切、
  逗号类攒够 18 字切、单句 60 字强制切；`first_done` 闩锁保证只早切第一句。
- 同上：`dispatch_response()` 在已播报时把 `chat_id` 置为 `voice:spoken`，`agent_main.c` 据此跳过二次播报。
- `voice/voice_channel.c`：`voice_speak_hold()` / `voice_speak_session_finish()` 抑制分句之间的 `pet_idle` /
  `done`，整轮结束才发一次，避免 UI 表情句中反复横跳。
- 同上：`voice_local_cmd_ack()`，`app:` 命令（`app:music` 除外，它自己出声）先播"好的"再执行。
- Key 落位：**不再编进固件**，改预置在 usrdata 镜像的 `ai_agent/config/config.json`（源目录
  `board/r528s3/ynm3000_nand/data/usrdata/ai_agent/config/`）。`/data` 是 yaffs、刷整包会重写，而 40 位 Key
  在板子上没法手输；设置页改 Key 覆盖同一份文件。`agent_secrets.h` 的 MiMo 行已注释，避免 clean build 又编
  进二进制。`voice_channel_reload_backends()` 回退为"只看运行期配置"：`/data` 被 factory 格式化时 LLM 与语音
  一起回落火山，不留半残状态。

**验证（代码级）**：构建通过，`vela.bin == nsh.fex`，镜像 `0ec3a7cfb62d6897b1b167ed3953b16d`（51,523,584
字节）；符号表确认 `llm_chat_stream`、`voice_speak_hold`、`voice_speak_session_finish`、`voice_turn_mark`、
`audio_playback_xrun_count/short_write_count` 均已链接；`usrdata.fex` 解出 config.json 为
`asr=xiaomi tts=xiaomi model=mimo-v2.6-flash`、Key 长 51，`vela.bin` 里搜不到 Key 明文。

## 待上板验证

1. 首音是否明显前移（`TURN llm_stream_req` → `tts_first_write`）；2. 会不会"只念了前半句"——流结束早于第二
   段合成属本轮已知边界。
3. 长回复（>60 字）是否触发 SSE 读取超时并回落（日志 `voice stream fallback`）；4. `app:` 是否真念出"好的"。

**已知风险**：首句 `voice_channel_speak()` 在 SSE 回调里同步执行，会阻塞读取 1~3s，回复很长时可能让服务端
攒包或超时（兜底是回落阻塞路径）；增量分句按字节扫 UTF-8，跨 delta 断在半个汉字上时该处不切，只影响切点。
