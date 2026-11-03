# P341 — 语音一轮到底慢在哪：把计时打准

2026-11-02；V1 ynm3000。本轮只做测量，不改语音链路行为。此前日志里的
`[voice] LLM latency` 是推算值，不能作为优化依据。

## 结论

- 旧 `LLM latency` 不可用：它依赖 `s_asr_done_ts`，而该时间戳只在**流式 ASR
  成功**分支赋值；MiMo 走录完整段识别（batch）分支，时间戳一直是 0。
- 板端实测（8 轮，3 轮完整）：ASR 848ms、LLM 2471ms、TTS 首包 920ms、首音
  4324ms（中位），`xrun=0 short=0`。
- 即播放侧是干净的，慢在"整段生成 + TTS 首包"两段串联。

## 改动

- `voice/voice_channel.c`：新增轮级打点 `voice_turn_begin/mark/end`，输出
  `[voice] TURN begin(ptt_up) xrun0=N short0=N`、`TURN <tag> +Nms`、
  `TURN end(<why>) +Nms xrun=N short=N`；覆盖 `asr_req/asr_done`、
  `llm_req/llm_done`、`llm_retry_*`、`tts_first_write`。
- 同上文件：`asr_and_dispatch()` 的 batch 分支补
  `clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts)`，让 MiMo 路径计时成立。
- `voice/audio_playback.c` + `include/voice/audio_playback.h`：新增
  `audio_playback_xrun_count()` / `audio_playback_short_write_count()`。
  `-EPIPE` 计 xrun；短写只计数、**暂不修**（短写会丢余下样本，先量化再动）。
- 工具 `contest2026_070_arc/tools/voice_trace.py`：解析 `[voice] TURN` 还原
  每轮时间线，给出各段中位与 xrun/短写合计；`output/p341/analyze_voice.py`
  供无打点的旧固件使用。

## 验证

- 构建通过；`res.fex` 三次独立打包 SHA-256 一致（`66ea36de…`）。
- 上板 8 轮数据如上，播放侧零欠载、零短写。

## 遗留

- 首音 4.3s 里 LLM 占 2.5s，是主要可压缩项；P342 用流式首句试解。
- `readi fail -77`（每次开始录音出现 5 次）仍未解释。
- `Voice: recording...` 是 printf 缓冲输出，两轮都滞后 203ms，不能用于计时。
