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
/**
 * message_bus.h — inter-task message bus
 *
 * with POSIX mutex + condition variable + ring buffer.
 */

#include "agent_compat.h"
#include <stdint.h>

/** A single message travelling on the bus. */
typedef struct {
    char  channel[16];   /**< "feishu", "websocket", "cli", "system" */
    char  chat_id[64];   /**< chat_id (Feishu IDs are ~36 chars) */
    char *content;       /**< Heap-allocated text; receiver must free. */
    char *image_b64;     /**< Optional base64-encoded image; receiver must free. NULL if none. */
    /** 2026-11-02 P345：本条消息的回复要不要用 TTS 念出来。
     *  UI「语音 AI」页的四个快问按钮走的是文本通道（没有录音），但用户在
     *  语音页点按钮时期望听到回答，所以请求里带 speak:true。语音通道
     *  （PTT）本来就会播报，不用这个标志。 */
    uint8_t speak;
} agent_msg_t;

/** Free heap members (content, image_b64) of a message.
 *  Safe to call on a zeroed or already-freed message. */
void message_bus_msg_free(agent_msg_t *msg);

/** Initialise inbound and outbound queues. */
int message_bus_init(void);

/** Destroy queues and release synchronisation resources. */
void message_bus_destroy(void);

/** Wake all threads blocked on pop (used during shutdown). */
void message_bus_wakeup(void);

/** Push a message to the inbound queue (towards the agent loop).
 *  On success the bus takes ownership of heap members.
 *  On failure (queue full / timeout) the caller still owns them. */
int message_bus_push_inbound(const agent_msg_t *msg);

/** Block until an inbound message is available (or timeout expires).
 *  Caller must free msg->content / msg->image_b64 when done. */
int message_bus_pop_inbound(agent_msg_t *msg, uint32_t timeout_ms);

/** Push a message to the outbound queue (towards channels).
 *  On success the bus takes ownership of heap members.
 *  On failure the caller still owns them. */
int message_bus_push_outbound(const agent_msg_t *msg);

/** Block until an outbound message is available (or timeout expires). */
int message_bus_pop_outbound(agent_msg_t *msg, uint32_t timeout_ms);
