/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_ai.h
 * AI Agent WebSocket client for Desktop Mate UI (openvela packages/ai_agent)
 *
 * Connects to the official ai_agent WS server (127.0.0.1:28789), sends
 * {"type":"message","content":"..."} and receives
 * {"type":"response","content":"..."} replies.  Background thread, UI polls.
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_AI_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_AI_H

#define AI_RESP_MAX  4096   /* 单轮回复最大字节（dm_ai_poll 缓冲） */

/* Trigger one round of question-answer (non-blocking; returns 0 if queued,
 * -1 if a round is already in flight or text invalid). */
int dm_ai_ask(const char *text);

/* Poll for a finished reply.  Returns 1 and copies the reply into buf when
 * a new result is ready (result consumed), 0 when nothing pending. */
int dm_ai_poll(char *buf, int buflen);

/* 1 while a question is being processed (UI can show "thinking...") */
int dm_ai_busy(void);

/* Send a voice control message to ai_agent (PTT 按住说话).
 * action: "start" (start recording), "stop" (finish ASR→LLM→TTS),
 *         "interrupt" (打断播报，小智 AbortSpeaking 式).
 * Non-blocking fire-and-forget via a background thread; returns 0 if
 * queued, -1 if action invalid or a voice command is already in flight. */
int dm_ai_voice(const char *action);

/* 2026-08-15 健康助理：直接 TTS 播报指定文本（健康提醒 L2/L3 语音触达）。
 * 发送 {"type":"voice","action":"speak","text":"..."} → agent 侧
 * voice_channel_speak() 立即合成并播放。fire-and-forget，独立 busy。
 * dm_ai_voice_speaking()：1 = speak 播报窗口内（PTT press 先 interrupt）。 */
int dm_ai_voice_speak(const char *text);
int dm_ai_voice_speaking(void);

/* 2026-08-15 P84：语音事件长连接订阅（小智式文字显示）。
 * agent 侧在 ASR/LLM 文本产生时广播
 *   {"type":"voice_evt","kind":"stt|llm","text":"..."}
 * subscribe 建立一条常驻 WS 连接收事件；unsubscribe 停止。
 * dm_ai_voice_evt_poll 消费新事件：有新 stt/llm 文本则分别拷入
 * stt/llm 缓冲（可为 NULL 跳过）并返回 1，无新事件返回 0。 */
int dm_ai_voice_evt_subscribe(void);
int dm_ai_voice_evt_unsubscribe(void);
int dm_ai_voice_evt_poll(char *stt, int stt_cap,
                         char *llm, int llm_cap);

/* 2026-08-23 P115：消费本地设备控制命令事件（音量/亮度）。
 * agent 侧 ASR 命中关键词后短路 LLM，广播
 *   {"type":"voice_evt","kind":"cmd","text":"volume:-5"} 之类；
 * UI 收到后执行硬件（g_music_volume / PWM 背光）+ TTS 确认。
 * 有新 cmd 事件则拷入 cmd（如 "volume:-5"）并返回 1，否则返回 0。 */
int dm_ai_voice_evt_cmd_poll(char *cmd, int cmd_cap);

/* 2026-08-23 review-10：消费 agent TTS 播报完成信号（kind=done），
 * 供音频仲裁层在播报结束后恢复被挂起的音乐。有新事件返回 1。 */
int dm_ai_voice_evt_done_poll(void);

/* 2026-08-11 MIC DEMO: trigger a 5s mic recording + auto-playback on the
 * agent side ({"type":"mic","action":"demo"}). Fire-and-forget. */
int dm_ai_mic_demo(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_AI_H */
