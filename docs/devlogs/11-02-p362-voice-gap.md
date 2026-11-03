# P362 语音播报句间空档（一卡一卡）修复

目标：语音对话每次回复中间固定空约 1 秒。平台：V1 `ynm3000` / `ai_agent` + `luncher_dm`。

## 一、证据（串口 08:21 那轮）

```
tts_start +1748ms → first chunk 1012ms → first_write +2760ms
speak_done +4537ms（第一句播完）→ LLM latency 3690ms → 第二段 first chunk 1041ms
```

每轮切两段送 TTS，第二段重付一次 TLS 握手 + 首包（约 1s），而第一段这时已经播完 →
中间硬空一秒（07:30 那轮同样 1051/974ms）。根因两层：① `voice_stream_flush()` 第一句
时**同步**调 `voice_channel_speak()`，播完才返回，这 2~4 秒 LLM 流被卡住、剩下的字收
不下来；② 收尾段只能等流结束再送 TTS，第二次首包完全暴露在空档里。

## 二、改动

`agent_loop.c`：第一句改 `speak_async()` 不阻塞流，随后只等它拿到播放权
（`voice_channel_speak_seq()` 变化，几毫秒），保证收尾段不抢前面；新增
`voice_stream_flush_tail()`，上一段还在播就走流水线，短回复照旧流式播。

`voice_channel.c`：新增 `voice_channel_speak_prefetch()` **边收边放**——攒够 1 秒就
`trylock` 试着接上播放（拿不到说明上一段还在播，继续攒，下一块再试），接上后一块
写一块；流结束还没接上就阻塞等上一段播完再写完。不能"整段攒完再放"：长回复的合成
时间≈音频时长，那会更卡。`voice_speak_session_finish()` 广播待命前先等最后一段播完
（上限 30s）；`voice_speak_hold(1)` 复位 pet_speak，同轮第二段不再让宠物动画重跳。
并发安全：TLS 原始缓冲本就是 2 槽池，`CONFIG_MBEDTLS_THREADING_C=y`。

`deskmate_ui.c`（同轮顺手的两处 UI 开销）：`ai_status_set()` 只在文字真变化时才
`lv_label_set_text`——原来 `ai_poll_cb` 每 300ms 无条件重设，freetype 逐字重排，一轮
白重绘上百次；对话区历史从最多 8KB 收到最近 1600 字节（`AI_CHAT_KEEP_BYTES`，按行切），
整段 label 每次追加都要全量重排，历史越长"文字上屏"那一下越卡。

## 三、验证

`pack_deskmate.sh ynm3000` PASS，三个新符号都在 ELF 里。镜像 53,149,696 B，
MD5 `72f9ecd8a7411db7d578d853addef871`。待验（板上看串口）：① 第二轮应出现
`prefetch joined with N bytes buffered`，1s 首包不再落在"第一段播完之后"；② 两段之间
只剩设备重开的几十毫秒；③ 宠物说话动画一轮只起一次；④ 短回复不受影响。
