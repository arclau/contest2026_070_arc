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

/* Xiaomi MiMo TTS backend (2026-11-01 P330)
 *
 * 小米语音合成复用 OpenAI 兼容的 /v1/chat/completions：
 *   model = mimo-v2.5-tts
 *   messages = [ {role:user, content:<风格指令>},
 *                {role:assistant, content:<要朗读的文本>} ]
 *   audio = {format:"pcm16", voice:"冰糖"}
 *   stream = true  → SSE，每条 delta.audio.data 是一段 base64 PCM
 *
 * 输出为 24kHz / 16bit / mono PCM16LE，正好等于播放端
 * AGENT_TTS_WS_SAMPLE_RATE，无需重采样。
 *
 * 用 vela_https_post_stream() 边收边解，内存不随音频长度增长；
 * 这样长回复也不会撑爆板端堆。
 */

#include "voice/voice_tts.h"
#include "voice/xiaomi_tts.h"

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

#define TAG "xiaomi_tts"

#define XIAOMI_TTS_SSE_INIT (16 * 1024)
#define XIAOMI_TTS_SSE_MAX  (192 * 1024)
#define XIAOMI_TTS_RESP_BUF AGENT_VOICE_RESP_BUF_SIZE

static char s_key[160];
static char s_model[48];
static char s_voice[64];
static char s_host[64];

/****************************************************************************
 * 凭据
 ****************************************************************************/

static void load_key(void)
{
    s_key[0] = '\0';

    if (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, s_key, sizeof(s_key)) == OK
        && s_key[0] != '\0')
    {
        return;
    }

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

static int xiaomi_tts_init(void)
{
    load_key();

    strncpy(s_model, AGENT_MIMO_TTS_MODEL, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
    if (claw_config_get(AGENT_CFG_KEY_MIMO_TTS_MODEL, s_model, sizeof(s_model)) != OK
        || s_model[0] == '\0')
    {
        strncpy(s_model, AGENT_MIMO_TTS_MODEL, sizeof(s_model) - 1);
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

    strncpy(s_voice, AGENT_MIMO_TTS_VOICE_DEFAULT, sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    if (claw_config_get(AGENT_CFG_KEY_MIMO_TTS_VOICE, s_voice, sizeof(s_voice)) != OK
        || s_voice[0] == '\0')
    {
        strncpy(s_voice, AGENT_MIMO_TTS_VOICE_DEFAULT, sizeof(s_voice) - 1);
        s_voice[sizeof(s_voice) - 1] = '\0';
    }

    syslog(LOG_INFO, "[%s] init: host=%s model=%s voice=%s key=%s\n",
           TAG, s_host, s_model, s_voice, s_key[0] ? "set" : "MISSING");
    return s_key[0] ? 0 : -EACCES;
}

static void xiaomi_tts_deinit(void)
{
    memset(s_key, 0, sizeof(s_key));
}

/****************************************************************************
 * JSON 转义 / base64 解码
 ****************************************************************************/

static size_t json_escape(const char *in, char *out, size_t outcap)
{
    size_t o = 0;

    for (; *in && o + 7 < outcap; in++)
    {
        unsigned char c = (unsigned char)*in;
        switch (c)
        {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20)
            {
                o += (size_t)snprintf(out + o, outcap - o, "\\u%04x", c);
            }
            else
            {
                out[o++] = (char)c;
            }
            break;
        }
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 解码到 out（容量 outcap）。返回写入字节数，负值=错误。 */
static int b64_decode(const char *in, size_t inlen, unsigned char *out,
                      size_t outcap)
{
    unsigned int acc = 0;
    int bits = 0;
    size_t o = 0;

    for (size_t i = 0; i < inlen; i++)
    {
        int v;

        if (in[i] == '=' || in[i] == '\n' || in[i] == '\r')
            continue;

        v = b64_val(in[i]);
        if (v < 0)
            return -EINVAL;

        acc = (acc << 6) | (unsigned int)v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            if (o >= outcap)
                return -ENOSPC;
            out[o++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }

    return (int)o;
}

/****************************************************************************
 * 流式会话
 ****************************************************************************/

typedef struct
{
    voice_tts_chunk_cb cb;
    void              *user;
    char              *acc;
    size_t             acc_len;
    size_t             acc_cap;
    unsigned char     *pcm;
    size_t             pcm_cap;
    int                done;
    int                got_audio;
    int                http_status;
    int                err;
} tts_stream_t;

/* 处理一条完整 SSE 行 */
static void handle_sse_line(tts_stream_t *st, char *line)
{
    char *payload;
    cJSON *root;
    cJSON *choices;
    cJSON *delta;
    cJSON *audio;
    cJSON *data;
    int n;

    if (strncmp(line, "data:", 5) != 0)
        return;

    payload = line + 5;
    while (*payload == ' ')
        payload++;

    if (strcmp(payload, "[DONE]") == 0)
    {
        st->done = 1;
        return;
    }

    root = cJSON_Parse(payload);
    if (!root)
        return;

    choices = cJSON_GetObjectItem(root, "choices");
    delta = choices ? cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta")
                    : NULL;
    audio = delta ? cJSON_GetObjectItem(delta, "audio") : NULL;

    /* 官方示例 delta.audio 是对象 {data:...}；也兼容字符串形式 */
    if (cJSON_IsObject(audio))
        data = cJSON_GetObjectItem(audio, "data");
    else if (cJSON_IsString(audio))
        data = audio;
    else
        data = NULL;

    if (cJSON_IsString(data) && data->valuestring && data->valuestring[0])
    {
        n = b64_decode(data->valuestring, strlen(data->valuestring),
                       st->pcm, st->pcm_cap);
        if (n > 0)
        {
            st->got_audio = 1;
            if (st->cb)
                st->cb(st->pcm, (size_t)n, 0, st->user);
        }
        else if (n < 0)
        {
            syslog(LOG_WARNING, "[%s] b64 decode failed: %d\n", TAG, n);
        }
    }

    cJSON_Delete(root);
}

static void tts_body_cb(const char *data, size_t len, void *user)
{
    tts_stream_t *st = (tts_stream_t *)user;
    size_t off = 0;

    if (!st || st->err || st->done)
        return;

    while (off < len)
    {
        const char *nl = memchr(data + off, '\n', len - off);
        size_t seg = nl ? (size_t)(nl - (data + off)) : (len - off);

        /* 追加到累加缓冲（保留半行） */
        if (st->acc_len + seg + 1 > st->acc_cap)
        {
            size_t need = st->acc_len + seg + 1;
            size_t cap = st->acc_cap;
            char *nb;

            while (cap < need && cap < XIAOMI_TTS_SSE_MAX)
                cap *= 2;
            if (cap > XIAOMI_TTS_SSE_MAX)
                cap = XIAOMI_TTS_SSE_MAX;
            if (cap < need)
            {
                syslog(LOG_ERR, "[%s] SSE line too long (%zu)\n", TAG, need);
                st->err = -ENOSPC;
                return;
            }

            nb = realloc(st->acc, cap);
            if (!nb)
            {
                st->err = -ENOMEM;
                return;
            }
            st->acc = nb;
            st->acc_cap = cap;
        }

        if (seg > 0)
        {
            memcpy(st->acc + st->acc_len, data + off, seg);
            st->acc_len += seg;
        }

        if (nl)
        {
            /* 整行：去尾部 \r，交给解析 */
            while (st->acc_len > 0
                   && (st->acc[st->acc_len - 1] == '\r'
                       || st->acc[st->acc_len - 1] == '\n'))
            {
                st->acc_len--;
            }
            st->acc[st->acc_len] = '\0';
            if (st->acc_len > 0)
                handle_sse_line(st, st->acc);
            st->acc_len = 0;
            off += seg + 1;
        }
        else
        {
            off = len;
        }
    }
}

/****************************************************************************
 * 请求体构造
 ****************************************************************************/

static char *build_body(const char *text, int stream, size_t *out_len)
{
    static const char fmt[] =
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"用自然亲切的语气朗读，语速适中\"},"
        "{\"role\":\"assistant\",\"content\":\"%s\"}],"
        "\"audio\":{\"format\":\"pcm16\",\"voice\":\"%s\"}%s}";

    const char *stream_flag = stream ? ",\"stream\":true" : "";
    size_t text_cap = strlen(text) * 6 + 8;
    char *escaped = malloc(text_cap);
    char *body;
    size_t cap;

    if (!escaped)
        return NULL;

    json_escape(text, escaped, text_cap);

    cap = strlen(fmt) + strlen(s_model) + strlen(escaped) + strlen(s_voice)
        + strlen(stream_flag) + 32;
    body = malloc(cap);
    if (!body)
    {
        free(escaped);
        return NULL;
    }

    snprintf(body, cap, fmt, s_model, escaped, s_voice, stream_flag);
    free(escaped);

    if (out_len)
        *out_len = strlen(body);
    return body;
}

static void fill_auth(vela_header_t hdrs[2], char *buf, size_t buflen)
{
    snprintf(buf, buflen, "Bearer %s", s_key);
    hdrs[0].name = "Authorization";
    hdrs[0].value = buf;
    hdrs[1].name = NULL;
    hdrs[1].value = NULL;
}

/****************************************************************************
 * 流式合成
 ****************************************************************************/

static int xiaomi_tts_speak_stream_impl(const char *text, voice_tts_chunk_cb cb,
                                        void *user_data)
{
    tts_stream_t st;
    vela_header_t hdrs[2];
    char auth[192];
    char *body;
    size_t body_len = 0;
    size_t got = 0;
    int status;
    int ret;

    if (!text || text[0] == '\0')
        return -EINVAL;

    if (!s_key[0])
    {
        syslog(LOG_ERR, "[%s] no API key configured\n", TAG);
        return -EACCES;
    }

    memset(&st, 0, sizeof(st));
    st.cb = cb;
    st.user = user_data;
    st.acc_cap = XIAOMI_TTS_SSE_INIT;
    st.acc = malloc(st.acc_cap);
    /* 24kHz/16bit/mono：一条 SSE 事件通常 ≤2s 音频 → 96KB PCM，留余量 */
    st.pcm_cap = 128 * 1024;
    st.pcm = malloc(st.pcm_cap);

    if (!st.acc || !st.pcm)
    {
        free(st.acc);
        free(st.pcm);
        return -ENOMEM;
    }

    body = build_body(text, 1, &body_len);
    if (!body)
    {
        free(st.acc);
        free(st.pcm);
        return -ENOMEM;
    }

    fill_auth(hdrs, auth, sizeof(auth));

    status = vela_https_post_stream(s_host, "443",
                                    AGENT_LLM_MIMO_PATH, hdrs,
                                    body, body_len,
                                    tts_body_cb, &st, &got);
    free(body);

    st.http_status = status;

    if (status != 200)
    {
        /* 错误体通常是一小段 JSON，acc 里可能留有残余，直接打印 */
        syslog(LOG_ERR, "[%s] HTTP %d (body %zu bytes)\n", TAG, status, got);
        if (st.acc && st.acc_len > 0)
        {
            st.acc[st.acc_len] = '\0';
            syslog(LOG_ERR, "[%s] resp: %.200s\n", TAG, st.acc);
        }
        ret = -EIO;
    }
    else if (st.err)
    {
        ret = st.err;
    }
    else
    {
        ret = 0;
    }

    if (ret == 0 && !st.got_audio)
    {
        syslog(LOG_WARNING, "[%s] no audio chunk received\n", TAG);
        ret = -ENODATA;
    }

    if (cb)
        cb(NULL, 0, 1, user_data);

    syslog(LOG_INFO, "[%s] stream done: status=%d body=%zu audio=%d\n",
           TAG, status, got, st.got_audio);

    free(st.acc);
    free(st.pcm);
    return ret;
}

/****************************************************************************
 * 一次性合成（测试/兼容路径）
 ****************************************************************************/

static int xiaomi_tts_synthesize(const char *text, unsigned char *pcm_out,
                                 size_t pcm_cap, size_t *pcm_len)
{
    vela_header_t hdrs[2];
    char auth[192];
    char *body;
    char *resp;
    size_t body_len = 0;
    int status;
    int ret = -EIO;

    if (!text || text[0] == '\0' || !pcm_out || !pcm_len)
        return -EINVAL;

    if (!s_key[0])
        return -EACCES;

    body = build_body(text, 0, &body_len);
    if (!body)
        return -ENOMEM;

    resp = malloc(XIAOMI_TTS_RESP_BUF);
    if (!resp)
    {
        free(body);
        return -ENOMEM;
    }

    fill_auth(hdrs, auth, sizeof(auth));

    status = vela_https_post_json(s_host, "443",
                                  AGENT_LLM_MIMO_PATH, hdrs,
                                  body, resp, XIAOMI_TTS_RESP_BUF);
    free(body);

    if (status != 200)
    {
        syslog(LOG_ERR, "[%s] HTTP %d: %.200s\n", TAG, status, resp);
        free(resp);
        return -EIO;
    }

    {
        cJSON *root = cJSON_Parse(resp);
        cJSON *choices = root ? cJSON_GetObjectItem(root, "choices") : NULL;
        cJSON *msg = choices
            ? cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message")
            : NULL;
        cJSON *audio = msg ? cJSON_GetObjectItem(msg, "audio") : NULL;
        cJSON *data = audio ? cJSON_GetObjectItem(audio, "data") : NULL;

        if (cJSON_IsString(data) && data->valuestring)
        {
            int n = b64_decode(data->valuestring, strlen(data->valuestring),
                               pcm_out, pcm_cap);
            if (n > 0)
            {
                *pcm_len = (size_t)n;
                ret = 0;
            }
            else
            {
                ret = n ? n : -ENODATA;
            }
        }
        else
        {
            syslog(LOG_ERR, "[%s] no audio in response\n", TAG);
        }

        if (root)
            cJSON_Delete(root);
    }

    free(resp);
    return ret;
}

static const voice_tts_ops_t s_xiaomi_tts_ops = {
    .name = "xiaomi",
    .init = xiaomi_tts_init,
    .synthesize = xiaomi_tts_synthesize,
    .deinit = xiaomi_tts_deinit,
};

int xiaomi_tts_register(void)
{
    return voice_tts_register(&s_xiaomi_tts_ops);
}

/* voice_tts.c 的流式派发需要按当前后端选择，故对外暴露一个入口 */
int xiaomi_tts_speak_stream(const char *text, voice_tts_chunk_cb cb,
                            void *user_data)
{
    return xiaomi_tts_speak_stream_impl(text, cb, user_data);
}
