/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* 2026-11-01 P330：按 config（voice_asr_backend / voice_tts_backend）与
 * 小米 Key 是否存在重新选择 ASR/TTS 后端。改完 Key 后调用即生效。 */
void voice_channel_reload_backends(void);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* P362：先合成到内存、等上一段播完再接着放。
 * 用于"第一句已经先出声、剩下的稍后才到"的分段播报：合成与上一段的
 * 播放重叠，避免两段之间空等一次 TTS 首包（实测约 1s）。
 * 没有上一段在播、或合成失败时返回负值，调用方退回 voice_channel_speak()。 */
int voice_channel_speak_prefetch(const char *text);

/* 是否正在合成/播放（供上面那条决定走哪条路径）。 */
int voice_channel_is_speaking(void);

/* 已开始的播报序号（单调递增）。用来确认异步发起的那一段真的开始播了，
 * 避免收尾段抢在它前面。 */
unsigned voice_channel_speak_seq(void);

/* 2026-08-15 P84：打断正在播报的 TTS（小智 AbortSpeaking 式）。
 * 置 tts_abort + 停 playback；无播报时为空操作。返回 0 成功。 */
int voice_channel_interrupt(void);

/* 2026-11-02 P341：一轮语音的分段观测。所有打点统一输出
 * "[voice] TURN <tag> +Nms"，N 相对"松手"时刻，宿主可直接拼时间轴。
 * 没有正在进行的回合时为空操作，可安全地在任意线程调用。 */
void voice_turn_mark(const char *tag);

/* 2026-11-02 P342：流式对话时一句话发一个 TTS 请求，句间不应把 UI/宠物
 * 打回待命。整段回复开始时 hold(1)，全部播完后 session_finish()。
 * hold 只影响 pet_idle/done 的广播，不改变播报本身。 */
void voice_speak_hold(int on);
void voice_speak_session_finish(void);

/* Test TTS: synthesize text, save PCM to file. */
int voice_channel_test_tts(const char *text, const char *out_path);

/* Test ASR: read PCM file, recognize, print result. */
int voice_channel_test_asr(const char *pcm_path);

/* TASK-04: end-to-end — capture Ns → ASR → LLM → TTS → speaker. */
int voice_channel_test_full(int seconds);

/* 2026-08-11 MIC DEMO: record N seconds from mic to a WAV file
 * (16kHz/16bit/mono, AW media_recorder — same path as voice chat).
 * Used to verify mic hardware + capture chain; file is playable. */
int voice_channel_test_mic(int seconds, const char *out_path);

/* 2026-08-15 P80 唤醒词：说「你好 openvela」免按键唤醒。
 * voice_wake_start(): 启动唤醒监听（空闲周期短录 + 能量门控 + stream ASR
 *   匹配 openvela → 命中后自动播提示音 + 录用户指令 → ASR→LLM→TTS 闭环）。
 * voice_wake_stop(): 停止监听。返回 0 成功。
 * ⚠️ 2026-08-15 P83 已弃用：云端唤醒监听每 ~2s 建一次 TLS/ASR 会话，
 * 资源占用大且空文本不触发，用户决定改回纯按钮 PTT（UI 不再发
 * wake_start）。函数保留仅供防御性 wake_stop / 旧固件兼容，勿再启用。 */
int voice_wake_start(void);
int voice_wake_stop(void);

#ifdef __cplusplus
}
#endif
