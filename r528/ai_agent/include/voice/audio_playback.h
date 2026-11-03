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

/* Audio playback session handle (opaque). */
typedef struct audio_playback audio_playback_t;

/* Open the playback device and configure for PCM output.
 * Returns NULL on failure. */
audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample);

/* Write PCM data to the playback device (blocking).
 * Returns bytes written, or negative errno on error. */
int audio_playback_write(audio_playback_t* pb,
    const void* buf, size_t len);

/* Force-stop an active playback from another thread.
 * Stops the underlying media_player and marks the handle so that
 * subsequent write() calls return -ECANCELED immediately.
 * Safe to call from any thread; safe if pb is NULL. */
void audio_playback_stop(audio_playback_t* pb);

/* Stop playback and release all resources. */
void audio_playback_close(audio_playback_t* pb);

/* 2026-11-02 P341：观测计数（进程级累加，不重置）。
 * xrun    : writei 返回 -EPIPE（DMA 缓冲被抽干）的次数。
 * short   : writei 返回帧数少于请求帧数的次数——当前实现不补写剩余样本，
 *           这些样本会被直接丢弃，是"断音/咔哒"的候选原因。
 * 只读计数，不改写入行为；先用数据判断是否真发生，再决定是否修。 */
unsigned audio_playback_xrun_count(void);
unsigned audio_playback_short_write_count(void);

#ifdef __cplusplus
}
#endif
