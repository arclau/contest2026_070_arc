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

#include "cJSON.h"
#include "agent_compat.h"
#include "agent_config.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int llm_proxy_init(void);
int llm_set_backend(const char* host, const char* path);
int llm_set_port(const char* port);

/**
 * Atomically set all LLM proxy fields in a single lock acquisition.
 * Used by llm_router to avoid partial updates.
 * Any NULL or empty field is skipped (not cleared).
 */
int llm_set_all(const char* host, const char* path,
    const char* port, const char* api_key,
    const char* model);

int llm_chat(const char* system_prompt, const char* messages_json,
    char* response_buf, size_t buf_size);

/* ── Tool Use ─────────────────────────────────────────────── */

typedef struct {
    char id[64];
    char name[32];
    char* input; /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct {
    char* text;
    size_t text_len;
    char* reasoning_content; /* heap-allocated; must be echoed back in tool-call turns */
    llm_tool_call_t calls[AGENT_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;

    /* Token usage from API response */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
} llm_response_t;

void llm_response_free(llm_response_t* resp);

int llm_chat_tools(const char* system_prompt,
    cJSON* messages,
    const char* tools_json,
    llm_response_t* resp,
    bool enable_thinking);

/* ── 流式对话（2026-11-02 P342）──────────────────────────────
 * 语音回合专用：不带 tools、默认不带 thinking，用 SSE 增量拿文本。
 * 目的：第一句一到就能开始合成，不必等整段回复生成完。
 *
 * cb 每收到一段 delta 文本调用一次（可能是半个词，也可能一整句）；
 * full_out 累积完整回复，供 UI 上屏和会话历史使用。
 * 返回 OK 表示流正常结束且至少收到一个 delta；ERROR 表示失败或不可用，
 * 调用方应回退到 llm_chat_tools() 的阻塞路径。 */
typedef void (*llm_delta_cb)(const char* delta, void* user);

int llm_chat_stream(const char* system_prompt, cJSON* messages,
    bool enable_thinking, llm_delta_cb cb, void* user,
    char* full_out, size_t full_cap);

/** Vision chat: send text + base64 image to a vision-capable model. *  image_b64 is the raw base64 string (no data: prefix).
 *  mime_type: "image/jpeg", "image/png", etc. NULL defaults to "image/jpeg". */
int llm_chat_vision(const char* prompt, const char* image_b64,
    const char* mime_type,
    char* response_buf, size_t buf_size);

/** Memory-optimized vision chat: accepts raw image bytes instead of
 *  pre-encoded base64.  Performs base64 encoding and JSON construction
 *  in a single buffer to avoid multiple copies of the large image data.
 *  Caller can free raw_image immediately after this returns. */
int llm_chat_vision_raw(const char* prompt,
    const unsigned char* raw_image, size_t raw_len,
    const char* mime_type,
    char* response_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
