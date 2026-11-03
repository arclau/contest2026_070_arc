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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#pragma once
/*
 * agent_config.h — AI Agent global configuration (Vela/NuttX version)
 *
 * Build-time secrets are loaded from agent_secrets.h if present.
 * Runtime overrides are stored in the file-based config store
 * (config_store.c) under CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR.
 */

#include <nuttx/config.h>

/* ── Build version — auto-updated on every compilation ──────── */
/* The build system touches agent_main.c before each build
 * (CMake: add_custom_target touch; Make: -D flag with shell date)
 * so __DATE__/__TIME__ are always fresh.  The Makefile also injects
 * AGENT_BUILD_TIMESTAMP via -D for extra reliability. */
#ifdef AGENT_BUILD_TIMESTAMP
#define AGENT_BUILD_VERSION AGENT_BUILD_TIMESTAMP
#else
#define AGENT_BUILD_VERSION (__DATE__ " " __TIME__)
#endif

#if __has_include("agent_secrets.h")
#include "agent_secrets.h"
#endif

/* Build-time secrets defaults */
#ifndef AGENT_SECRET_API_KEY
#define AGENT_SECRET_API_KEY ""
#endif
#ifndef AGENT_SECRET_MIMO_API_KEY
#define AGENT_SECRET_MIMO_API_KEY ""
#endif
#ifndef AGENT_SECRET_MODEL
#define AGENT_SECRET_MODEL ""
#endif
#ifndef AGENT_SECRET_PROXY_HOST
#define AGENT_SECRET_PROXY_HOST ""
#endif
#ifndef AGENT_SECRET_PROXY_PORT
#define AGENT_SECRET_PROXY_PORT ""
#endif
#ifndef AGENT_SECRET_SEARCH_KEY
#define AGENT_SECRET_SEARCH_KEY ""
#endif
#ifndef AGENT_SECRET_SERP_KEY
#define AGENT_SECRET_SERP_KEY ""
#endif
#ifndef AGENT_SECRET_EXA_KEY
#define AGENT_SECRET_EXA_KEY ""
#endif
#ifndef AGENT_SECRET_TAVILY_KEY
#define AGENT_SECRET_TAVILY_KEY ""
#endif
#ifndef AGENT_SECRET_NEWS_KEY
#define AGENT_SECRET_NEWS_KEY ""
#endif
#ifndef AGENT_SECRET_FEISHU_APP_ID
#define AGENT_SECRET_FEISHU_APP_ID ""
#endif
#ifndef AGENT_SECRET_FEISHU_APP_SECRET
#define AGENT_SECRET_FEISHU_APP_SECRET ""
#endif
/* 2026-08-11 豆包语音凭证编译期默认（volc_tts.c/volc_asr.c 回退用） */
#ifndef AGENT_SECRET_VOLC_API_KEY
#define AGENT_SECRET_VOLC_API_KEY ""
#endif
#ifndef AGENT_SECRET_VOLC_APPID
#define AGENT_SECRET_VOLC_APPID ""
#endif
#ifndef AGENT_SECRET_VOLC_TOKEN
#define AGENT_SECRET_VOLC_TOKEN ""
#endif
#ifndef AGENT_SECRET_VOLC_ASR_CLUSTER
#define AGENT_SECRET_VOLC_ASR_CLUSTER ""
#endif

/* ── Data directories ────────────────────────────────────── */
#ifndef CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#define CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR "/data/agent"
#endif

#define AGENT_DATA_DIR CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#define AGENT_CONFIG_DIR AGENT_DATA_DIR "/config"
#define AGENT_MEMORY_DIR AGENT_DATA_DIR "/memory"
#define AGENT_SESSION_DIR AGENT_DATA_DIR "/sessions"
#define AGENT_MEMORY_FILE AGENT_DATA_DIR "/memory/MEMORY.md"
#define AGENT_SOUL_FILE AGENT_DATA_DIR "/config/SOUL.md"
#define AGENT_USER_FILE AGENT_DATA_DIR "/config/USER.md"
#define AGENT_CONFIG_FILE AGENT_DATA_DIR "/config/config.json"

/* ── Feishu (Lark) Bot ──────────────────────────────────────── */
#define AGENT_FEISHU_POLL_STACK (20 * 1024) /* WS client needs more stack \
                                                */
#define AGENT_FEISHU_POLL_PRIO 50
#define AGENT_FEISHU_MAX_MSG_LEN 4000 /* Feishu text message limit */

/* ── Agent Loop ─────────────────────────────────────────────── */
#define AGENT_AI_AGENT_STACK (32 * 1024)
#define AGENT_AI_AGENT_PRIO 60
#define AGENT_AI_AGENT_CORE 0
#define AGENT_AI_AGENT_MAX_HISTORY 10
#define AGENT_AI_AGENT_MAX_TOOL_ITER 10
#define AGENT_MAX_TOOL_CALLS 2
#define AGENT_TOOL_NAME_REPEAT_MAX 4

/* ── LLM Watchdog ──────────────────────────────────────────── */
/* Agent-level timeout for a single LLM call (seconds).
 * If llm_chat_tools takes longer than this, the agent treats it as
 * a timeout and replies with a user-friendly error message.
 * The socket-level SO_RCVTIMEO (AGENT_LLM_SOCKET_TIMEOUT_SEC)
 * acts as the hard backstop that actually unblocks the read. */
#define AGENT_LLM_TIMEOUT_SEC 60

/* Socket-level read timeout applied via SO_RCVTIMEO in vela_tls.
 * Must be >= AGENT_LLM_TIMEOUT_SEC to allow the agent-level
 * watchdog to fire first on normal slow responses.  Set higher
 * to cover TLS handshake + full response read. */
#define AGENT_LLM_SOCKET_TIMEOUT_SEC 120

/* ── Timezone (POSIX TZ format) ────────────────────────────── */
#define AGENT_TIMEZONE "CST-8"

/* ── LLM ────────────────────────────────────────────────────── */
#define AGENT_LLM_DEFAULT_MODEL "deepseek-v3-241226"
#define AGENT_LLM_MAX_TOKENS 4096
#define AGENT_LLM_MAX_TOKENS_OPENAI 16384
/* 2026-08-11: 火山方舟（Volcengine Ark）OpenAI 兼容端点——默认值从
 * agent_secrets.h（AGENT_SECRET_*）注入 Key/模型；host/path 在此固化。
 * 运行时 set_llm 可覆盖（持久化 /data/agent/config/config.json）。 */
#define AGENT_LLM_API_HOST "ark.cn-beijing.volces.com"
#define AGENT_LLM_API_PATH "/api/v3/chat/completions"
#ifndef AGENT_LLM_API_URL
#define AGENT_LLM_API_URL ""
#endif
#define AGENT_LLM_API_VERSION "openai"
#define AGENT_LLM_MAX_RETRIES 3
#define AGENT_LLM_RETRY_BASE_SEC 2
#define AGENT_LLM_STREAM_BUF_SIZE (8 * 1024)
#define AGENT_LLM_MAX_RESP_SIZE \
    (512 * 1024) /* hard cap for growable resp buffer */

/* ── Qwen (Alibaba DashScope) backend constants ─────────────── */
#define AGENT_LLM_QWEN_HOST "dashscope.aliyuncs.com"
#define AGENT_LLM_QWEN_PATH "/compatible-mode/v1/chat/completions"
#define AGENT_LLM_QWEN_MODEL "qwen-turbo"

/* ── OpenRouter (unified multi-model gateway) ───────────────── */
#define AGENT_LLM_OPENROUTER_HOST "openrouter.ai"
#define AGENT_LLM_OPENROUTER_PATH "/api/v1/chat/completions"
#define AGENT_LLM_OPENROUTER_MODEL "qwen/qwen3-coder:free"

/* ── MiMo (Xiaomi MiMo platform) ───────────────────────────── */
#define AGENT_LLM_MIMO_HOST "api.xiaomimimo.com"
#define AGENT_LLM_MIMO_PATH "/v1/chat/completions"
#define AGENT_LLM_MIMO_MODEL "mimo-v2.6-flash"

/* ── Message Bus ────────────────────────────────────────────── */
#define AGENT_BUS_QUEUE_LEN 16
#define AGENT_BUS_PUSH_TIMEOUT_MS 5000
#define AGENT_OUTBOUND_STACK (16 * 1024)
#define AGENT_OUTBOUND_PRIO 50
#define AGENT_OUTBOUND_CORE 0

/* ── Memory / Context ───────────────────────────────────────── */
#define AGENT_CONTEXT_BUF_SIZE (8 * 1024)

/* ── Cron / Heartbeat ──────────────────────────────────────── */
#define AGENT_CRON_FILE AGENT_DATA_DIR "/cron.json"
#define AGENT_CRON_FILE_MAX_SIZE (8 * 1024)
#define AGENT_CRON_MAX_JOBS 16
#define AGENT_CRON_CHECK_INTERVAL_MS (10 * 1000)
#define AGENT_CRON_ID_LEN 9 /* 8 hex chars + NUL */
#define AGENT_CRON_STACK (8 * 1024)
#define AGENT_CRON_PRIO 40
#define AGENT_HEARTBEAT_FILE AGENT_DATA_DIR "/HEARTBEAT.md"
#define AGENT_HEARTBEAT_INTERVAL_MS (30 * 60 * 1000)

/* ── Skills ─────────────────────────────────────────────────── */
#define AGENT_SKILLS_DIR AGENT_DATA_DIR "/skills/"
#define AGENT_SESSION_MAX_MSGS 20

/* ── Bitable Skill Sync ───────────────────────────────────────── */
#ifdef CONFIG_AI_AGENT_SKILL_SYNC
#define AGENT_SKILL_SYNC_ENABLED 1
#else
#define AGENT_SKILL_SYNC_ENABLED 0
#endif

/* Bitable app_token (multi-dimensional table ID) */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_APP_TOKEN
#define CONFIG_AI_AGENT_SKILL_SYNC_APP_TOKEN ""
#endif
#define AGENT_SKILL_SYNC_APP_TOKEN CONFIG_AI_AGENT_SKILL_SYNC_APP_TOKEN

/* Bitable table_id */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_TABLE_ID
#define CONFIG_AI_AGENT_SKILL_SYNC_TABLE_ID ""
#endif
#define AGENT_SKILL_SYNC_TABLE_ID CONFIG_AI_AGENT_SKILL_SYNC_TABLE_ID

/* Sync mode: 0=incremental (version-based), 1=full (rewrite all) */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_MODE
#define CONFIG_AI_AGENT_SKILL_SYNC_MODE 0
#endif
#define AGENT_SKILL_SYNC_MODE CONFIG_AI_AGENT_SKILL_SYNC_MODE

/* Device type identifier for target_device filtering */
#ifndef CONFIG_AI_AGENT_SKILL_SYNC_DEVICE_TYPE
#define CONFIG_AI_AGENT_SKILL_SYNC_DEVICE_TYPE "all"
#endif
#define AGENT_SKILL_SYNC_DEVICE_TYPE CONFIG_AI_AGENT_SKILL_SYNC_DEVICE_TYPE

/* HTTP response buffer size for Bitable API */
#define AGENT_SKILL_SYNC_RESP_SIZE (64 * 1024)

/* Local version tracking file */
#define AGENT_SKILL_SYNC_VERSION_FILE AGENT_SKILLS_DIR ".versions.json"

/* ── WebSocket Gateway ──────────────────────────────────────── */
#define AGENT_WS_PORT 28789
#define AGENT_WS_MAX_CLIENTS 8

/* ── WS client thread stack ──────────────────────────────────── */
#define AGENT_WS_CLIENT_STACK (12 * 1024)

/* ── Serial CLI ─────────────────────────────────────────────── */
#define AGENT_CLI_STACK (16 * 1024)
#define AGENT_CLI_PRIO 30
#define AGENT_CLI_CORE 0

/* ── Config store keys (config store key strings) ─── */
#define AGENT_CFG_KEY_API_KEY "api_key"
#define AGENT_CFG_KEY_MODEL "model"
#define AGENT_CFG_KEY_PROXY_HOST "proxy_host"
#define AGENT_CFG_KEY_PROXY_PORT "proxy_port"
#define AGENT_CFG_KEY_SEARCH_KEY "search_key"
#define AGENT_CFG_KEY_SERP_KEY "serp_key"
#define AGENT_CFG_KEY_EXA_KEY "exa_key"
#define AGENT_CFG_KEY_TAVILY_KEY "tavily_key"
#define AGENT_CFG_KEY_NEWS_KEY "news_key"
#define AGENT_CFG_KEY_WIFI_SSID "wifi_ssid"
#define AGENT_CFG_KEY_WIFI_PASS "wifi_pass"
#define AGENT_CFG_KEY_FEISHU_APP_ID "feishu_app_id"
#define AGENT_CFG_KEY_FEISHU_APP_SECRET "feishu_app_secret"
#define AGENT_CFG_KEY_FEISHU_USER_TOKEN "feishu_user_token"
#define AGENT_CFG_KEY_LLM_HOST "llm_host"
#define AGENT_CFG_KEY_LLM_PATH "llm_path"
#define AGENT_CFG_KEY_GATEWAY_HOST "gateway_host"
#define AGENT_CFG_KEY_GATEWAY_PORT "gateway_port"
#define AGENT_CFG_KEY_GATEWAY_TOKEN "gateway_token"

/* ── Vision / Multimodal ─────────────────────────────────────── */
#define AGENT_VISION_MAX_IMAGE_SIZE \
    (256 * 1024) /* max JPEG file size: 256 KB */
#define AGENT_VISION_MAX_B64_SIZE (350 * 1024) /* base64 ≈ 4/3 × raw */
#define AGENT_VISION_MAX_TOKENS 4096
#define AGENT_VISION_DEFAULT_PROMPT                                      \
    "Please analyze this image in detail. If it contains text, tables, or " \
    "documents, extract and describe the content accurately."
#define AGENT_CAPTURE_PATH AGENT_DATA_DIR "/capture.jpg"
#define AGENT_TEST_IMAGE_PATH AGENT_DATA_DIR "/test.jpg"

/* ── Integration test image directory (Kconfig overridable) ──── */
#ifdef CONFIG_AI_AGENT_TEST_IMAGE_DIR
#define AGENT_TEST_IMAGE_DIR CONFIG_AI_AGENT_TEST_IMAGE_DIR
#else
#define AGENT_TEST_IMAGE_DIR AGENT_DATA_DIR "/test"
#endif

/* ── Node client identity ─────────────────────────────────── */
#ifndef CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID
#define CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID "watch-01"
#endif
#ifndef CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_DISPLAY_NAME
#define CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_DISPLAY_NAME "Smart Watch"
#endif

#define AGENT_NODE_ID CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID
#define AGENT_NODE_DISPLAY_NAME \
    CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_DISPLAY_NAME

/* ── Channel identifiers ────────────────────────────────────── */
#define AGENT_CHAN_WEBSOCKET "websocket"
#define AGENT_CHAN_CLI "cli"
#define AGENT_CHAN_SYSTEM "system"
#define AGENT_CHAN_FEISHU "feishu"
#define AGENT_CHAN_MQTT "mqtt"
#define AGENT_CHAN_WEIXIN "weixin"
#ifdef CONFIG_AI_AGENT_LVGL_UI
#define AGENT_CHAN_LVGL_UI "lvgl_ui"
#endif

/* ── WeChat Channel ─────────────────────────────────────────── */
#define AGENT_WEIXIN_STACK (12 * 1024)
#define AGENT_WEIXIN_PRIO 45

/* ── LVGL UI Channel ───────────────────────────────────────── */
#ifdef CONFIG_AI_AGENT_LVGL_UI
#define AGENT_LVGL_UI_STACK (16 * 1024)
#define AGENT_LVGL_UI_PRIO 45
#endif

/* ── MQTT Channel ───────────────────────────────────────────── */
#define AGENT_MQTT_DEFAULT_PORT 1883
#define AGENT_MQTT_KEEPALIVE 60
#define AGENT_MQTT_BUF_SIZE 1024
#define AGENT_MQTT_STACK (12 * 1024)
#define AGENT_MQTT_PRIO 45
#define AGENT_MQTT_SYNC_INTERVAL_MS 200
#define AGENT_MQTT_TOPIC_IN_DEFAULT "agent/in"
#define AGENT_MQTT_TOPIC_OUT_DEFAULT "agent/out"

#define AGENT_CFG_KEY_MQTT_BROKER "mqtt_broker"
#define AGENT_CFG_KEY_MQTT_CLIENT_ID "mqtt_client_id"
#define AGENT_CFG_KEY_MQTT_TOPIC_IN "mqtt_topic_in"
#define AGENT_CFG_KEY_MQTT_TOPIC_OUT "mqtt_topic_out"
#define AGENT_CFG_KEY_MQTT_USERNAME "mqtt_username"
#define AGENT_CFG_KEY_MQTT_PASSWORD "mqtt_password"

/* ── Voice Channel (Doubao ASR/TTS) ─────────────────────────── */
#define AGENT_CHAN_VOICE "voice"

#define AGENT_VOICE_STACK (32 * 1024)   /* 2026-08-12 B7：16KB 内跑 mbedtls
                                         * TLS handshake 会栈溢出（P60 教训：
                                         * 12KB WS 线程 test_mic 崩），翻倍 */
#define AGENT_VOICE_PRIO 50

/* Doubao TTS V3 API endpoint (HTTP Chunked, x-api-key auth) */
#define AGENT_DOUBAO_TTS_HOST "openspeech.bytedance.com"
#define AGENT_DOUBAO_TTS_PORT "443"
#define AGENT_DOUBAO_TTS_V3_PATH "/api/v3/tts/unidirectional"
/* 2026-08-14 改造：新版「双向流式语音合成 WebSocket」接口
 * （wss://openspeech.bytedance.com/api/v3/tts/bidirection）的
 * X-Api-Resource-Id 必须是 seed-tts-2.0（豆包语音合成大模型2.0），
 * 原值 volc.service_type.10029 是旧版控制台服务类型，未开通→HTTP 403
 * （volc.service_type.10029 requested resource not granted）。 */
#define AGENT_DOUBAO_TTS_RESOURCE "seed-tts-2.0"

/* Doubao streaming ASR WebSocket API — 2026-08-11 适配「流式语音识别大模型」
 * （Seed，用户控制台开通的是大模型版，非传统 v2/asr）：
 *   接口 wss://openspeech.bytedance.com/api/v3/sauc/bigmodel
 *   鉴权 X-Api-App-Key / X-Api-Access-Key / X-Api-Resource-Id（非 Bearer token） */
#define AGENT_DOUBAO_ASR_HOST "openspeech.bytedance.com"
#define AGENT_DOUBAO_ASR_PORT "443"
#define AGENT_DOUBAO_ASR_WS_PATH "/api/v3/sauc/bigmodel"
/* 大模型版 Resource-Id：用户开通「流式语音识别大模型-小时版」→ Seed 小时版 */
#define AGENT_DOUBAO_ASR_RESOURCE_ID "volc.bigasr.sauc.duration" /* 2026-08-12 修复：官方文档示例值（原 volc.seedasr.sauc.duration 致 WS upgrade HTTP 400） */

/* Default ASR cluster (streaming, common Chinese) */
#define AGENT_DOUBAO_ASR_CLUSTER "volcengine_streaming_common"

/* Audio device paths (platform-specific) */
#ifndef AGENT_AUDIO_CAPTURE_DEV
#define AGENT_AUDIO_CAPTURE_DEV "/dev/audio/pcm0c"
#endif
#ifndef AGENT_AUDIO_PLAYBACK_DEV
#define AGENT_AUDIO_PLAYBACK_DEV "/dev/audio/pcm0p"
#endif

/* Audio chunk size for streaming: 3200 bytes = 100ms at 16kHz/16bit/mono */
#define AGENT_ASR_CHUNK_SIZE 3200

/* Audio parameters: PCM 16-bit signed LE, 16kHz, mono */
#define AGENT_VOICE_SAMPLE_RATE 16000
#define AGENT_VOICE_CHANNELS 1
#define AGENT_VOICE_BITS 16
#define AGENT_VOICE_FRAME_MS 20

/* Buffer sizes — keep small to avoid OOM during concurrent agent+voice.
 * 128KB PCM ≈ 4s at 16kHz/16bit/mono; enough for typical TTS output.
 * Recording uses the same cap; 128KB ≈ 4s of audio which is fine for
 * short voice commands.  For longer recordings, increase as needed.
 * 2026-08-14：256KB ≈ 8s —— --test asr 用 arecord 5s WAV（44B 头 +
 * 160000B PCM = 160044B）超 128KB 被拒（-27 EFBIG），增大支持。 */
#define AGENT_VOICE_PCM_BUF_SIZE (256 * 1024)
#define AGENT_VOICE_RESP_BUF_SIZE (128 * 1024)
#define AGENT_VOICE_B64_BUF_SIZE (96 * 1024)

/* Config store keys */
#define AGENT_CFG_KEY_VOLC_APPKEY "volc_appkey"
#define AGENT_CFG_KEY_VOLC_TOKEN "volc_token"
#define AGENT_CFG_KEY_VOLC_API_KEY "volc_api_key"
#define AGENT_CFG_KEY_VOLC_SPEAKER "volc_speaker"
#define AGENT_CFG_KEY_VOLC_CLUSTER "volc_cluster"
#define AGENT_CFG_KEY_VOLC_ASR_CLUSTER "volc_asr_cluster"

/* ── Xiaomi MiMo voice (ASR/TTS) — 2026-11-01 P330 ──────────────
 * 小米语音走 OpenAI 兼容的 /v1/chat/completions：
 *   ASR: model=mimo-v2.5-asr，user.content=[{type:input_audio,
 *        input_audio:{data:"data:audio/wav;base64,..."}}]，asr_options.language
 *   TTS: model=mimo-v2.5-tts，assistant.content=<文本>，audio{format,voice}
 *        format=pcm16 + stream=true → delta.audio.data(base64, 24kHz PCM16LE)
 * 播放端 AGENT_TTS_WS_SAMPLE_RATE 就是 24kHz，无需重采样。 */
#define AGENT_MIMO_ASR_MODEL "mimo-v2.5-asr"
#define AGENT_MIMO_TTS_MODEL "mimo-v2.5-tts"
#define AGENT_MIMO_TTS_VOICE_DEFAULT "冰糖"
#define AGENT_MIMO_ASR_LANG_DEFAULT "zh"
#define AGENT_MIMO_HOST_DEFAULT "api.xiaomimimo.com"

/* 运行时 Key：优先 mimo_api_key，其次通用 api_key（set_llm 写入的那个）。
 * 两者都为空时才回退构建期 AGENT_SECRET_MIMO_API_KEY。 */
#define AGENT_CFG_KEY_MIMO_KEY "mimo_api_key"
#define AGENT_CFG_KEY_MIMO_TTS_VOICE "mimo_tts_voice"
#define AGENT_CFG_KEY_MIMO_ASR_LANG "mimo_asr_lang"

/* 模型 ID / 端点也必须可配：MiMo 会下线模型名（v2.5-pro、v2.5 文本已于
 * 2026-10-21 下线，v2.6 只有文本模型）。写死在宏里意味着每次官方换名字
 * 都要重编固件——这正是 P329 之前的老毛病。留空则用上面的默认值。 */
#define AGENT_CFG_KEY_MIMO_ASR_MODEL "mimo_asr_model"
#define AGENT_CFG_KEY_MIMO_TTS_MODEL "mimo_tts_model"
#define AGENT_CFG_KEY_MIMO_HOST "mimo_host"

/* 语音后端选择（"xiaomi" / "volc"），空=按 Key 自动选 */
#define AGENT_CFG_KEY_VOICE_ASR_BACKEND "voice_asr_backend"
#define AGENT_CFG_KEY_VOICE_TTS_BACKEND "voice_tts_backend"

/* ASR 单次请求音频上限（base64 前字节）。小米限制 base64 后 ≤10MB，
 * 这里按板端内存收紧：16kHz/16bit/mono ≈ 32KB/s，256KB ≈ 8s。 */
#define AGENT_MIMO_ASR_MAX_PCM (256 * 1024)

/* 手机配对页（Settings→AI 语音 扫码）监听端口。
 * 2026-11-01：8088 是抄来的惯性数字，改成 9000（V1 板内无其他服务占用）。 */
#define AGENT_PAIR_PORT 9000

/* Default TTS speaker and cluster */
/* 2026-08-14：55000000 resource mismatched with speaker 根因=原音色
 * zh_male_beijingxiaoye_emo_v2_mars_bigtts 是 1.0 音色（*_mars_bigtts），
 * 与新版资源 seed-tts-2.0 不匹配。2.0 音色必须 *_uranus_bigtts 系列
 * （火山音色库官方 2.0 大模型音色）。默认选男声「魅力苏菲」。 */
#define AGENT_VOICE_DEFAULT_SPEAKER "zh_male_sophie_uranus_bigtts"
#define AGENT_VOICE_DEFAULT_CLUSTER "volcano_tts"

/* WebSocket TTS output sample rate (big-model voices output 24kHz) */
#define AGENT_TTS_WS_SAMPLE_RATE 24000

/* ── Tool Guard (security) ──────────────────────────────────── */
#define AGENT_TOOL_MAX_INPUT_LEN (32 * 1024)
#define AGENT_TOOL_RATE_LIMIT_WINDOW_SEC 60
#define AGENT_TOOL_RATE_LIMIT_MAX_CALLS 10

/* ── Shell security policy ──────────────────────────────────── */
#define AGENT_SHELL_SECURITY_ALLOWLIST 0 /* Whitelist mode (production) */
#define AGENT_SHELL_SECURITY_FULL 1 /* Full access (development/skills) */
#define AGENT_SHELL_SECURITY_DENY 2 /* Disabled */

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_ALLOWLIST
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_ALLOWLIST
#elif defined(CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_FULL)
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_FULL
#elif defined(CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_DENY)
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_DENY
#else
#define AGENT_SHELL_SECURITY AGENT_SHELL_SECURITY_ALLOWLIST /* default \
                                                                   */
#endif
