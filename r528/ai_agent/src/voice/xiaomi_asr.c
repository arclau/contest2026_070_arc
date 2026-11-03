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

/* Xiaomi MiMo ASR backend (2026-11-01 P330)
 *
 * 小米语音识别没有 WebSocket 流式接口，只有一次性 HTTP：
 *   POST https://api.xiaomimimo.com/v1/chat/completions
 *   model = mimo-v2.5-asr
 *   messages[0].content[0] = {type:"input_audio",
 *       input_audio:{data:"data:audio/wav;base64,<B64>"}}
 *   asr_options.language = "zh" | "en" | "auto"
 *
 * 因此本后端只实现 recognize()（批量识别）。voice_channel 在
 * voice_asr_stream_open() 返回 NULL 时会自动回落到批量路径——
 * PTT 松开后一次性识别，符合"按住说话"的交互。
 *
 * 板端音频是 16kHz/16bit/mono 裸 PCM，这里现场拼 44 字节 WAV 头再
 * base64；base64 采用流式编码（44B 头 + PCM 分两段喂入），避免再复制
 * 一份整段音频。
 */

#include "voice/voice_asr.h"
#include "voice/xiaomi_asr.h"

#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define TAG "xiaomi_asr"

/* 响应体很小（只有识别文本），8KB 足够 */
#define XIAOMI_ASR_RESP_BUF (8 * 1024)

static char s_key[160];
static char s_model[48];
static char s_lang[8];
static char s_host[64];

/****************************************************************************
 * base64 流式编码
 ****************************************************************************/

typedef struct
{
    char         *out;
    size_t        cap;
    size_t        len;
    int           err;
    unsigned char carry[3];
    int           ncarry;
} b64enc_t;

static const char s_b64_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_put(b64enc_t *b, char c)
{
    if (b->len + 2 >= b->cap)
    {
        b->err = 1;
        return;
    }
    b->out[b->len++] = c;
}

static void b64_feed(b64enc_t *b, const unsigned char *in, size_t n)
{
    size_t i;

    if (b->err)
        return;

    for (i = 0; i < n; i++)
    {
        b->carry[b->ncarry++] = in[i];
        if (b->ncarry == 3)
        {
            unsigned int v = ((unsigned int)b->carry[0] << 16)
                           | ((unsigned int)b->carry[1] << 8)
                           | (unsigned int)b->carry[2];
            b64_put(b, s_b64_tbl[(v >> 18) & 63]);
            b64_put(b, s_b64_tbl[(v >> 12) & 63]);
            b64_put(b, s_b64_tbl[(v >> 6) & 63]);
            b64_put(b, s_b64_tbl[v & 63]);
            b->ncarry = 0;
        }
    }
}

static void b64_finish(b64enc_t *b)
{
    if (b->err)
        return;

    if (b->ncarry == 1)
    {
        unsigned int v = (unsigned int)b->carry[0] << 16;
        b64_put(b, s_b64_tbl[(v >> 18) & 63]);
        b64_put(b, s_b64_tbl[(v >> 12) & 63]);
        b64_put(b, '=');
        b64_put(b, '=');
    }
    else if (b->ncarry == 2)
    {
        unsigned int v = ((unsigned int)b->carry[0] << 16)
                       | ((unsigned int)b->carry[1] << 8);
        b64_put(b, s_b64_tbl[(v >> 18) & 63]);
        b64_put(b, s_b64_tbl[(v >> 12) & 63]);
        b64_put(b, s_b64_tbl[(v >> 6) & 63]);
        b64_put(b, '=');
    }
    b->ncarry = 0;
    b->out[b->len] = '\0';
}

/****************************************************************************
 * WAV 头（16kHz / 16bit / mono）
 ****************************************************************************/

static void wav_header(unsigned char h[44], unsigned int pcm_len,
                       unsigned int rate)
{
    unsigned int byte_rate = rate * 2; /* mono * 16bit */
    unsigned int riff = 36 + pcm_len;

    memcpy(h + 0, "RIFF", 4);
    h[4]  = (unsigned char)(riff & 0xff);
    h[5]  = (unsigned char)((riff >> 8) & 0xff);
    h[6]  = (unsigned char)((riff >> 16) & 0xff);
    h[7]  = (unsigned char)((riff >> 24) & 0xff);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;              /* fmt chunk size */
    h[17] = 0;
    h[18] = 0;
    h[19] = 0;
    h[20] = 1;               /* PCM */
    h[21] = 0;
    h[22] = 1;               /* channels = 1 */
    h[23] = 0;
    h[24] = (unsigned char)(rate & 0xff);
    h[25] = (unsigned char)((rate >> 8) & 0xff);
    h[26] = (unsigned char)((rate >> 16) & 0xff);
    h[27] = (unsigned char)((rate >> 24) & 0xff);
    h[28] = (unsigned char)(byte_rate & 0xff);
    h[29] = (unsigned char)((byte_rate >> 8) & 0xff);
    h[30] = (unsigned char)((byte_rate >> 16) & 0xff);
    h[31] = (unsigned char)((byte_rate >> 24) & 0xff);
    h[32] = 2;               /* block align */
    h[33] = 0;
    h[34] = 16;              /* bits per sample */
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)(pcm_len & 0xff);
    h[41] = (unsigned char)((pcm_len >> 8) & 0xff);
    h[42] = (unsigned char)((pcm_len >> 16) & 0xff);
    h[43] = (unsigned char)((pcm_len >> 24) & 0xff);
}

/****************************************************************************
 * 凭据加载
 ****************************************************************************/

static void load_key(void)
{
    s_key[0] = '\0';

    if (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, s_key, sizeof(s_key)) == OK
        && s_key[0] != '\0')
    {
        return;
    }

    /* 兼容：set_llm mimo <key> 只写通用 api_key */
    if (claw_config_get(AGENT_CFG_KEY_API_KEY, s_key, sizeof(s_key)) == OK
        && s_key[0] != '\0')
    {
        return;
    }

    if (AGENT_SECRET_MIMO_API_KEY[0] != '\0')
    {
        strncpy(s_key, AGENT_SECRET_MIMO_API_KEY, sizeof(s_key) - 1);
        s_key[sizeof(s_key) - 1] = '\0';
    }
}

static int xiaomi_asr_init(void)
{
    load_key();

    strncpy(s_model, AGENT_MIMO_ASR_MODEL, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
    if (claw_config_get(AGENT_CFG_KEY_MIMO_ASR_MODEL, s_model, sizeof(s_model)) != OK
        || s_model[0] == '\0')
    {
        strncpy(s_model, AGENT_MIMO_ASR_MODEL, sizeof(s_model) - 1);
        s_model[sizeof(s_model) - 1] = '\0';
    }

    strncpy(s_host, AGENT_MIMO_HOST_DEFAULT, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    if (claw_config_get(AGENT_CFG_KEY_MIMO_HOST, s_host, sizeof(s_host)) != OK
        || s_host[0] == '\0')
    {
        strncpy(s_host, AGENT_MIMO_HOST_DEFAULT, sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = '\0';
    }

    strncpy(s_lang, AGENT_MIMO_ASR_LANG_DEFAULT, sizeof(s_lang) - 1);
    s_lang[sizeof(s_lang) - 1] = '\0';
    if (claw_config_get(AGENT_CFG_KEY_MIMO_ASR_LANG, s_lang, sizeof(s_lang)) != OK
        || s_lang[0] == '\0')
    {
        strncpy(s_lang, AGENT_MIMO_ASR_LANG_DEFAULT, sizeof(s_lang) - 1);
        s_lang[sizeof(s_lang) - 1] = '\0';
    }

    syslog(LOG_INFO, "[%s] init: host=%s model=%s lang=%s key=%s\n",
           TAG, s_host, s_model, s_lang, s_key[0] ? "set" : "MISSING");
    return s_key[0] ? 0 : -EACCES;
}

static void xiaomi_asr_deinit(void)
{
    memset(s_key, 0, sizeof(s_key));
}

/****************************************************************************
 * 识别
 ****************************************************************************/

static int xiaomi_asr_recognize(const unsigned char *pcm_data,
                                size_t pcm_len,
                                char *text_out,
                                size_t text_cap)
{
    static const char prefix[] =
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
        "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
        "\"data:audio/wav;base64,";
    static const char suffix[] =
        "\"}}]}],\"asr_options\":{\"language\":\"%s\"}}";

    char hdr_auth[192];
    vela_header_t hdrs[2];
    unsigned char wav[44];
    b64enc_t b64;
    char *body = NULL;
    char *resp = NULL;
    size_t prefix_len;
    size_t b64_cap;
    size_t body_cap;
    size_t total;
    int status;
    int ret = -EIO;

    if (text_out && text_cap > 0)
        text_out[0] = '\0';

    if (!pcm_data || pcm_len == 0)
        return -EINVAL;

    if (!s_key[0])
    {
        syslog(LOG_ERR, "[%s] no API key configured\n", TAG);
        return -EACCES;
    }

    if (pcm_len > AGENT_MIMO_ASR_MAX_PCM)
    {
        syslog(LOG_WARNING, "[%s] pcm %zu > cap %d, truncated\n",
               TAG, pcm_len, AGENT_MIMO_ASR_MAX_PCM);
        pcm_len = AGENT_MIMO_ASR_MAX_PCM;
    }

    wav_header(wav, (unsigned int)pcm_len, AGENT_VOICE_SAMPLE_RATE);

    /* 前缀里有一个 %s（模型名），后缀一个 %s（语种）。
     * 2026-11-01 P336 修复：原来直接 strlen(prefix) + strlen(s_model)，
     * 把 "%s" 这两个字节也算进长度 → b64.len 从实际前缀之后 2 字节开始，
     * 中间留下 snprintf 的 '\0' 和未初始化字节。vela_https_post_json()
     * 用 strlen() 取 body 长度，于是请求体在 base64 之前就被截断，
     * 小米返回 400 "Invalid JSON in request body"（板端 16:31:21 实测）。
     * 正确长度 = 前缀去掉 "%s" 再拼上模型名。 */
    prefix_len = strlen(prefix) - 2 + strlen(s_model);
    b64_cap = ((44 + pcm_len + 2) / 3) * 4 + 4;
    body_cap = prefix_len + b64_cap + strlen(suffix) + strlen(s_lang) + 8;

    body = malloc(body_cap);
    if (!body)
    {
        syslog(LOG_ERR, "[%s] OOM body %zu\n", TAG, body_cap);
        return -ENOMEM;
    }

    if ((size_t)snprintf(body, body_cap, prefix, s_model) != prefix_len)
    {
        syslog(LOG_ERR, "[%s] prefix length mismatch\n", TAG);
        free(body);
        return -EIO;
    }

    memset(&b64, 0, sizeof(b64));
    b64.out = body;
    b64.cap = body_cap;
    b64.len = prefix_len;
    b64_feed(&b64, wav, sizeof(wav));
    b64_feed(&b64, pcm_data, pcm_len);
    b64_finish(&b64);

    if (b64.err)
    {
        syslog(LOG_ERR, "[%s] base64 overflow (cap=%zu)\n", TAG, body_cap);
        free(body);
        return -ENOMEM;
    }

    total = b64.len;
    total += (size_t)snprintf(body + total, body_cap - total, suffix, s_lang);

    syslog(LOG_INFO, "[%s] POST %d bytes (pcm %d, ~%us audio)\n",
           TAG, (int)total, (int)pcm_len,
           (unsigned)(pcm_len / (AGENT_VOICE_SAMPLE_RATE * 2)));

    resp = malloc(XIAOMI_ASR_RESP_BUF);
    if (!resp)
    {
        free(body);
        return -ENOMEM;
    }

    snprintf(hdr_auth, sizeof(hdr_auth), "Bearer %s", s_key);
    hdrs[0].name = "Authorization";
    hdrs[0].value = hdr_auth;
    hdrs[1].name = NULL;
    hdrs[1].value = NULL;

    status = vela_https_post_json(s_host, "443",
                                  AGENT_LLM_MIMO_PATH, hdrs,
                                  body, resp, XIAOMI_ASR_RESP_BUF);
    free(body);

    if (status <= 0)
    {
        syslog(LOG_ERR, "[%s] request failed: %d\n", TAG, status);
        free(resp);
        return -EIO;
    }

    if (status != 200)
    {
        syslog(LOG_ERR, "[%s] HTTP %d (sent %d bytes): %.200s\n",
               TAG, status, (int)total, resp);
        free(resp);
        return -EIO;
    }

    {
        cJSON *root = cJSON_Parse(resp);
        if (!root)
        {
            syslog(LOG_ERR, "[%s] bad json: %.120s\n", TAG, resp);
            free(resp);
            return -EIO;
        }

        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *first = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;

        if (cJSON_IsString(content) && content->valuestring)
        {
            strncpy(text_out, content->valuestring, text_cap - 1);
            text_out[text_cap - 1] = '\0';
            ret = 0;
        }
        else
        {
            syslog(LOG_ERR, "[%s] no content in response: %.200s\n", TAG, resp);
        }

        cJSON_Delete(root);
    }

    free(resp);
    return ret;
}

static const voice_asr_ops_t s_xiaomi_asr_ops = {
    .name = "xiaomi",
    .init = xiaomi_asr_init,
    .recognize = xiaomi_asr_recognize,
    .deinit = xiaomi_asr_deinit,
};

int xiaomi_asr_register(void)
{
    return voice_asr_register(&s_xiaomi_asr_ops);
}
