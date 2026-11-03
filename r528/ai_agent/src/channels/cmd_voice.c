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

#include "channels/cmd_voice.h"
#include "infra/config_store.h"
#include "llm/llm_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"
#include "voice/voice_channel.h"
#include "voice/voice_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cmd_set_volc_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_volc_key <api_key>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_API_KEY, argv[1]);
    printf("Doubao voice API key saved.\n");
}

void cmd_set_volc_speaker(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_volc_speaker <speaker_id>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_SPEAKER, argv[1]);
    printf("TTS speaker set to: %s\n", argv[1]);
}

void cmd_set_volc_asr(int argc, char** argv)
{
    if (argc < 4) {
        printf("Usage: set_volc_asr <app_id> <token> <cluster>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_APPKEY, argv[1]);
    claw_config_set(AGENT_CFG_KEY_VOLC_TOKEN, argv[2]);
    claw_config_set(AGENT_CFG_KEY_VOLC_ASR_CLUSTER, argv[3]);
    printf("ASR credentials saved (app_id=%s, cluster=%s).\n",
        argv[1], argv[3]);
}

void cmd_voice_start(void)
{
    voice_channel_start();
}

void cmd_voice_stop(void)
{
    voice_channel_stop();
}

void cmd_voice_test_tts(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: voice_test_tts <text> [output.pcm]\n");
        return;
    }

    const char* out = (argc >= 3)
        ? argv[2]
        : AGENT_DATA_DIR "/tts_out.pcm";

    voice_channel_test_tts(argv[1], out);
}

void cmd_voice_test_asr(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: voice_test_asr <pcm_file>\n");
        return;
    }
    voice_channel_test_asr(argv[1]);
}

void cmd_voice_test_mic(int argc, char** argv)
{
    int seconds = 5;
    const char* out = AGENT_DATA_DIR "/mic_demo.wav";

    if (argc >= 2) {
        seconds = atoi(argv[1]);
    }

    if (argc >= 3) {
        out = argv[2];
    }

    voice_channel_test_mic(seconds, out);
}

void cmd_set_voice_tts(int argc, char** argv)
{
    if (argc < 2) {
        const char* cur = voice_tts_get_backend();

        printf("Current TTS backend: %s\n",
            cur ? cur : "(none)");
        printf("Usage: set_voice_tts <backend_name>\n");
        return;
    }

    int ret = voice_tts_set_backend(argv[1]);

    if (ret == 0) {
        claw_config_set(AGENT_CFG_KEY_VOICE_TTS_BACKEND, argv[1]);
        printf("TTS backend set to: %s\n", argv[1]);
    } else {
        printf("TTS backend '%s' not found.\n", argv[1]);
    }
}

void cmd_set_voice_asr(int argc, char** argv)
{
    if (argc < 2) {
        const char* cur = voice_asr_get_backend();

        printf("Current ASR backend: %s\n",
            cur ? cur : "(none)");
        printf("Usage: set_voice_asr <backend_name>\n");
        return;
    }

    int ret = voice_asr_set_backend(argv[1]);

    if (ret == 0) {
        claw_config_set(AGENT_CFG_KEY_VOICE_ASR_BACKEND, argv[1]);
        printf("ASR backend set to: %s\n", argv[1]);
    } else {
        printf("ASR backend '%s' not found.\n", argv[1]);
    }
}

/* ── 2026-11-01 P330：小米语音 / 运行时配置 ─────────────────── */

/* set_mimo_key <api_key> —— 保存小米 MiMo Key 并切到小米语音 */
void cmd_set_mimo_key(int argc, char** argv)
{
    if (argc < 2) {
        char cur[160] = { 0 };
        int has = (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, cur, sizeof(cur)) == OK
                   && cur[0] != '\0');
        printf("MiMo API key: %s\n", has ? "(set)" : "(not set)");
        printf("Usage: set_mimo_key <api_key>\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_MIMO_KEY, argv[1]);
    /* 显式把语音也切到小米：用户意图是"全部用小米" */
    claw_config_set(AGENT_CFG_KEY_VOICE_ASR_BACKEND, "xiaomi");
    claw_config_set(AGENT_CFG_KEY_VOICE_TTS_BACKEND, "xiaomi");
    /* 文本大模型一起切（一个 Key 打通 LLM+ASR+TTS） */
    claw_config_set(AGENT_CFG_KEY_LLM_HOST, AGENT_LLM_MIMO_HOST);
    claw_config_set(AGENT_CFG_KEY_LLM_PATH, AGENT_LLM_MIMO_PATH);
    claw_config_set(AGENT_CFG_KEY_MODEL, AGENT_LLM_MIMO_MODEL);
    claw_config_set(AGENT_CFG_KEY_API_KEY, argv[1]);
    llm_set_all(AGENT_LLM_MIMO_HOST, AGENT_LLM_MIMO_PATH, "443",
                argv[1], AGENT_LLM_MIMO_MODEL);
    voice_channel_reload_backends();
    printf("MiMo key saved; LLM+ASR+TTS -> xiaomi.\n");
}

/* voice_provider [asr] [tts] —— 查看/设置语音后端 */
void cmd_voice_provider(int argc, char** argv)
{
    if (argc < 2) {
        const char* a = voice_asr_get_backend();
        const char* t = voice_tts_get_backend();
        char key[160] = { 0 };
        int has = (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, key, sizeof(key)) == OK
                   && key[0] != '\0');

        printf("ASR backend : %s\n", a ? a : "(none)");
        printf("TTS backend : %s\n", t ? t : "(none)");
        printf("MiMo key    : %s\n", has ? "set" : "not set");
        printf("Usage: voice_provider <xiaomi|volcengine> [same]\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_VOICE_ASR_BACKEND, argv[1]);
    claw_config_set(AGENT_CFG_KEY_VOICE_TTS_BACKEND,
        argc >= 3 ? argv[2] : argv[1]);
    voice_channel_reload_backends();
    printf("Voice provider -> asr=%s tts=%s\n", argv[1],
        argc >= 3 ? argv[2] : argv[1]);
}

/* voice_voice [voice_id] —— 查看/设置小米 TTS 音色（冰糖/茉莉/苏打/白桦…） */
void cmd_voice_voice(int argc, char** argv)
{
    char cur[64] = { 0 };

    if (argc < 2) {
        if (claw_config_get(AGENT_CFG_KEY_MIMO_TTS_VOICE, cur, sizeof(cur)) != OK
            || cur[0] == '\0') {
            strlcpy(cur, AGENT_MIMO_TTS_VOICE_DEFAULT, sizeof(cur));
        }
        printf("MiMo TTS voice: %s\n", cur);
        printf("Usage: voice_voice <voice_id>\n");
        return;
    }

    claw_config_set(AGENT_CFG_KEY_MIMO_TTS_VOICE, argv[1]);
    voice_channel_reload_backends();
    printf("MiMo TTS voice -> %s\n", argv[1]);
}
