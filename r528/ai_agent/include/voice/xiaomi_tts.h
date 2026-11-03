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

#include "voice/voice_tts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the Xiaomi MiMo TTS backend ("xiaomi"). */
int xiaomi_tts_register(void);

/* Streaming synthesis (SSE + 24kHz PCM16). Used by voice_tts.c when the
 * active backend is "xiaomi". */
int xiaomi_tts_speak_stream(const char *text, voice_tts_chunk_cb cb,
                            void *user_data);

#ifdef __cplusplus
}
#endif
