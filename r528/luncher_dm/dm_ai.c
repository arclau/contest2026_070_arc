/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_ai.c
 * AI Agent WebSocket client for Desktop Mate UI (openvela packages/ai_agent)
 *
 * Talks to the official ai_agent WS server (127.0.0.1:28789).  The server
 * accepts {"type":"message","content":"<text>"} and replies with
 * {"type":"response","content":"<text>"}.  Background worker thread so the
 * UI never blocks; UI polls dm_ai_poll() from a timer.
 *
 * Wire protocol (RFC 6455 subset, mirrors packages/ai_agent ws_server.c):
 *   - HTTP Upgrade handshake with Sec-WebSocket-Key
 *   - client→server: masked text frames (0x81, MASK bit set)
 *   - server→client: unmasked text frames
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dm_ai.h"
#include "dm_voice.h"   /* 2026-08-28 Step 3：AI 对话开始时间戳（Director 静默窗口） */

#define AI_WS_HOST   "127.0.0.1"
#define AI_WS_PORT   28789
#define AI_WS_GUID   "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define AI_REQ_MAX   256

static pthread_t        g_ai_thread;
static volatile int     g_ai_busy;      /* 1 = worker 运行中 */
static volatile int     g_ai_done;      /* 1 = 最新一轮已结束 */
static volatile int     g_ai_ok;        /* 1 = 最新一轮成功拿到回复 */
static char             g_ai_req[AI_REQ_MAX];
static char             g_ai_resp[AI_RESP_MAX];

/* base64 for Sec-WebSocket-Key (16 random bytes → 24 chars).
 * 2026-09-10 P206 rev：旧实现 `while (i + 2 < inlen)` 在 inlen=16 时
 * 只编码前 15 字节（i=0,3,…,12），最后一个字节丢失、且无 '=' 补位。
 * 服务端不校验 key 内容所以一直"能用"，但属于错实现——改标准版。 */
static void b64_encode(const unsigned char *in, int inlen,
                       char *out, int outcap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, o = 0;
    while (i < inlen && o + 4 < outcap)
        {
            unsigned int v = (unsigned int)in[i++] << 16;
            int n = 1;
            if (i < inlen)
                {
                    v |= (unsigned int)in[i++] << 8;
                    n++;
                }
            if (i < inlen)
                {
                    v |= (unsigned int)in[i++];
                    n++;
                }
            out[o++] = tbl[(v >> 18) & 63];
            out[o++] = tbl[(v >> 12) & 63];
            out[o++] = (n > 1) ? tbl[(v >> 6) & 63] : '=';
            out[o++] = (n > 2) ? tbl[v & 63] : '=';
        }
    out[o] = '\0';
}

/* Escape text for embedding inside a JSON string literal.  User input may
 * contain " \ \n \r \t — unescaped they would corrupt the WS message JSON
 * and ws_server's cJSON_Parse would drop the message silently. */
static void json_escape(const char *in, char *out, size_t outcap)
{
    size_t o = 0;
    for (; *in && o + 6 < outcap; in++)
        {
            switch (*in)
                {
                case '"':  out[o++] = '\\'; out[o++] = '"';  break;
                case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
                case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
                case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
                case '\t': out[o++] = '\\'; out[o++] = 't';  break;
                default:   out[o++] = *in;                    break;
                }
        }
    out[o] = '\0';
}

/* Send one masked text frame (client → server) */
static int ws_send_text(int fd, const char *text)
{
    size_t len = strlen(text);
    unsigned char hdr[14];
    int hlen = 0;

    hdr[hlen++] = 0x81;                 /* FIN + text */
    if (len < 126)
        {
            hdr[hlen++] = (unsigned char)(0x80 | len);
        }
    else if (len < 65536)
        {
            hdr[hlen++] = 0x80 | 126;
            hdr[hlen++] = (unsigned char)(len >> 8);
            hdr[hlen++] = (unsigned char)(len & 0xFF);
        }
    else
        {
            hdr[hlen++] = 0x80 | 127;
            for (int i = 7; i >= 0; i--)
                hdr[hlen++] = (unsigned char)((len >> (i * 8)) & 0xFF);
        }

    unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    if (send(fd, hdr, hlen, 0) != hlen)
        return -1;

    for (size_t off = 0; off < len; )
        {
            size_t todo = len - off;
            unsigned char chunk[128];
            if (todo > sizeof(chunk))
                todo = sizeof(chunk);
            for (size_t i = 0; i < todo; i++)
                chunk[i] = (unsigned char)text[off + i] ^ mask[(off + i) & 3];
            if (send(fd, chunk, todo, 0) != (int)todo)
                return -1;
            off += todo;
        }
    return 0;
}

/* Receive one text frame (server → client, unmasked). Returns payload len,
 * 0 = 无消息（ping 已回 pong，连接正常，调用方应 continue），
 * -1 on error/close（errno：EAGAIN=30s 超时可继续等，其它=断开需重连）。 */
static int ws_recv_text(int fd, char *out, int outcap)
{
    unsigned char b0, b1;
    /* P215：recv==0（对端有序 close）不设 errno，若残留上次超时 EAGAIN，
     * 调用方会误判 continue 空转 → 死 fd 忙等。统一：0=EOF 置 ECONNRESET。 */
    int r0 = recv(fd, &b0, 1, MSG_WAITALL);
    if (r0 != 1)
        {
            if (r0 == 0)
                errno = ECONNRESET;
            return -1;
        }
    int r1 = recv(fd, &b1, 1, MSG_WAITALL);
    if (r1 != 1)
        {
            if (r1 == 0)
                errno = ECONNRESET;
            return -1;
        }

    int opcode = b0 & 0x0F;
    size_t plen = b1 & 0x7F;
    int masked = (b1 & 0x80) != 0;

    if (plen == 126)
        {
            unsigned char ext[2];
            int re = recv(fd, ext, 2, MSG_WAITALL);
            if (re != 2)
                {
                    if (re == 0)
                        errno = ECONNRESET;
                    return -1;
                }
            plen = (ext[0] << 8) | ext[1];
        }
    else if (plen == 127)
        {
            unsigned char ext[8];
            int re = recv(fd, ext, 8, MSG_WAITALL);
            if (re != 8)
                {
                    if (re == 0)
                        errno = ECONNRESET;
                    return -1;
                }
            plen = 0;
            for (int i = 0; i < 8; i++)
                plen = (plen << 8) | ext[i];
        }

    unsigned char mask[4] = { 0, 0, 0, 0 };
    if (masked)
        {
            int rm = recv(fd, mask, 4, MSG_WAITALL);
            if (rm != 4)
                {
                    if (rm == 0)
                        errno = ECONNRESET;
                    return -1;
                }
        }

    if (opcode == 0x8)                  /* close */
        return -1;
    if (opcode == 0x9)                  /* ping → pong */
        {
            unsigned char pong[2] = { 0x8A, 0x00 };
            send(fd, pong, 2, 0);
            return 0;
        }
    if (opcode != 0x1)                  /* ignore non-text */
        {
            unsigned char discard[256];
            size_t left = plen;
            while (left > 0)
                {
                    size_t todo = left > sizeof(discard) ?
                                  sizeof(discard) : left;
                    int rd = recv(fd, discard, todo, MSG_WAITALL);
                    if (rd != (int)todo)
                        {
                            if (rd == 0)
                                errno = ECONNRESET;
                            return -1;
                        }
                    left -= todo;
                }
            return 0;
        }

    /* P215：127 帧 plen 为 64bit，(int)plen 在 >INT_MAX 时变负、
     * 负值 <outcap 恒成立 → 截断失效 → out+got 越界写。
     * 改 size_t 比较；超长帧收 outcap-1 + 丢弃尾部（流对齐不断帧）。 */
    if (plen >= (size_t)outcap)
        {
            size_t want = (size_t)(outcap - 1);
            size_t got = 0;
            while (got < want)
                {
                    int n = recv(fd, out + got, want - got, 0);
                    if (n <= 0)
                        {
                            if (n == 0)
                                errno = ECONNRESET;
                            return -1;
                        }
                    got += (size_t)n;
                }
            size_t left = plen - want;
            unsigned char discard[256];
            while (left > 0)
                {
                    size_t todo = left > sizeof(discard) ?
                                  sizeof(discard) : left;
                    int rd = recv(fd, discard, todo, MSG_WAITALL);
                    if (rd != (int)todo)
                        {
                            if (rd == 0)
                                errno = ECONNRESET;
                            return -1;
                        }
                    left -= todo;
                }
            for (size_t i = 0; i < got; i++)
                out[i] ^= mask[i & 3];
            out[got] = '\0';
            return (int)got;
        }
    size_t got = 0;
    while (got < plen)
        {
            int n = recv(fd, out + got, plen - got, 0);
            if (n <= 0)
                {
                    if (n == 0)
                        errno = ECONNRESET;
                    return -1;
                }
            got += n;
        }
    for (size_t i = 0; i < got; i++)
        out[i] ^= mask[i & 3];
    out[got] = '\0';
    return (int)got;
}

/* Extract "content":"..." value from a JSON response */
static void extract_content(const char *json, char *out, int outcap)
{
    const char *p = strstr(json, "\"content\"");
    if (!p)
        {
            out[0] = '\0';
            return;
        }
    p = strchr(p, ':');
    if (!p)
        {
            out[0] = '\0';
            return;
        }
    p = strchr(p, '"');
    if (!p)
        {
            out[0] = '\0';
            return;
        }
    p++;
    int o = 0;
    while (*p && *p != '"' && o < outcap - 1)
        {
            if (*p == '\\' && (p[1] == 'n' || p[1] == 't' || p[1] == '"'))
                {
                    out[o++] = (p[1] == 'n') ? '\n' : (p[1] == 't') ? '\t' : '"';
                    p += 2;
                }
            else
                {
                    out[o++] = *p++;
                }
        }
    out[o] = '\0';
}

/* Connect to the local ai_agent (127.0.0.1:28789) and complete the WS
 * upgrade handshake.  Returns connected fd on success, -1 on failure.
 * P53: extracted from ai_worker so the voice control path reuses it. */
static int ai_ws_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(AI_WS_PORT);
    sa.sin_addr.s_addr = inet_addr(AI_WS_HOST);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        {
            close(fd);
            return -1;
        }

    /* P53 调优（AIYOUHUA A1）：30s 收包超时，防止 agent 不回复时永久阻塞 */
    struct timeval tv;
    tv.tv_sec  = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* HTTP Upgrade handshake */
    unsigned char rnd[16];
    for (int i = 0; i < 16; i++)
        rnd[i] = (unsigned char)(rand() & 0xFF);
    char key_b64[64];
    b64_encode(rnd, 16, key_b64, sizeof(key_b64));
    char req[512];
    snprintf(req, sizeof(req),
             "GET / HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             AI_WS_HOST, AI_WS_PORT, key_b64);
    if (send(fd, req, strlen(req), 0) != (int)strlen(req))
        {
            close(fd);
            return -1;
        }

    /* read handshake response until header end */
    char hb[512];
    int total = 0;
    while (total < (int)sizeof(hb) - 1)
        {
            int n = recv(fd, hb + total, sizeof(hb) - 1 - total, 0);
            if (n <= 0)
                {
                    close(fd);
                    return -1;
                }
            total += n;
            hb[total] = '\0';
            if (strstr(hb, "\r\n\r\n"))
                break;
        }
    if (!strstr(hb, "101"))
        {
            close(fd);
            return -1;   /* not a websocket upgrade */
        }
    return fd;
}

static void *ai_worker(void *arg)
{
    (void)arg;
    int fd = -1;
    /* P53 调优：buf 改 static——AI_RESP_MAX 1024→4096 后栈上 4KB 缓冲可能
     * 压爆默认 pthread 栈；worker 受 g_ai_busy 保护单实例，static 无并发风险。 */
    static char buf[AI_RESP_MAX];

    g_ai_ok = 0;
    g_ai_done = 0;

    fd = ai_ws_connect();
    if (fd < 0)
        goto fail;

    /* 3. Send question as JSON message frame */
    {
        /* 2026-08-11 调优：用户输入 JSON 转义——原实现 %s 直接拼入 JSON，
         * 文本含 " 或 \ 时协议损坏，ws_server cJSON_Parse 失败丢消息，
         * UI 盲等 30s。转义后任意文本均可安全传输。 */
        char esc[AI_REQ_MAX * 2 + 1];
        json_escape(g_ai_req, esc, sizeof(esc));
        char json[sizeof(esc) + 64];
        snprintf(json, sizeof(json),
                 "{\"type\":\"message\",\"content\":\"%s\",\"chat_id\":\"ui\"}",
                 esc);
        if (ws_send_text(fd, json) < 0)
            goto fail;
    }

    /* 4. Read frames until a response containing "content" */
    for (int spins = 0; spins < 200; spins++)
        {
            int n = ws_recv_text(fd, buf, sizeof(buf));
            if (n == 0)
                continue;   /* P215：ping 已回 pong，继续等 response */
            if (n < 0)
                goto fail;
            if (strstr(buf, "\"type\":\"response\""))
                {
                    extract_content(buf, g_ai_resp, sizeof(g_ai_resp));
                    if (g_ai_resp[0] != '\0')
                        {
                            g_ai_ok = 1;
                            break;
                        }
                }
        }
    /* 2026-09-10 P206 rev：200 轮耗尽（ping 洪水/全是空 content）旧代码
     * 直接落到 close + done=1 + ok=0 → dm_ai_poll 返回 1 但缓冲为空串 →
     * UI 无任何提示回"随时待命"，用户以为 AI 已回实则没回。
     * 耗尽 = 失败，走 fail 写明错误。 */
    if (!g_ai_ok)
        goto fail;

    close(fd);
    g_ai_busy = 0;
    g_ai_done = 1;
    return NULL;

fail:
    if (fd >= 0)
        close(fd);
    /* 2026-08-11 调优：失败时给出错误提示而非静默空回复——原实现
     * g_ai_ok=0 → dm_ai_poll 返回空串 → UI 回复区被清空且无任何说明。
     * 现写入错误信息（g_ai_ok=1 使 poll 正常返回该文本）。 */
    if (!g_ai_ok)
        {
            snprintf(g_ai_resp, sizeof(g_ai_resp),
                     "⚠️ AI 不可用：agent 未启动或连接失败/超时");
            g_ai_ok = 1;
        }
    g_ai_busy = 0;
    g_ai_done = 1;
    return NULL;
}

int dm_ai_ask(const char *text)
{
    if (!text || text[0] == '\0')
        return -1;
    if (g_ai_busy)
        return -1;

    strncpy(g_ai_req, text, sizeof(g_ai_req) - 1);
    g_ai_req[sizeof(g_ai_req) - 1] = '\0';
    g_ai_busy = 1;
    g_ai_done = 0;
    g_ai_ok = 0;
    voice_director_on_ai_interaction();   /* Step 3：AI 对话开始时间戳 */

    if (pthread_create(&g_ai_thread, NULL, ai_worker, NULL) != 0)
        {
            g_ai_busy = 0;
            return -1;
        }
    pthread_detach(g_ai_thread);
    return 0;
}

int dm_ai_poll(char *buf, int buflen)
{
    if (!buf || buflen <= 0)   /* 防止 buflen-1 转 size_t 后越界写 */
        return 0;
    if (!g_ai_done)
        return 0;
    if (g_ai_ok && g_ai_resp[0] != '\0')
        {
            strncpy(buf, g_ai_resp, buflen - 1);
            buf[buflen - 1] = '\0';
        }
    else
        {
            if (buflen > 0)
                buf[0] = '\0';
        }
    g_ai_done = 0;
    g_ai_ok = 0;
    return 1;
}

int dm_ai_busy(void)
{
    return g_ai_busy;
}

/* ── 2026-08-11 P53 方案A：语音控制消息（按住说话）────────────
 * UI 语音按钮按住 → dm_ai_voice("start")（PTT 录音），松开 →
 * dm_ai_voice("stop")（ASR→LLM→TTS）。短连接 fire-and-forget，
 * ws_server 侧解析后调 voice_channel_start/stop（同 agent CLI）。
 * 2026-08-15 P80：dm_ai_voice("wake_start"/"wake_stop") 启停
 * 唤醒词监听（说「你好 openvela」免按键唤醒，agent 侧自动闭环）。 */
static char             g_ai_voice_action[16];
static volatile int     g_ai_voice_busy;

static void *ai_voice_worker(void *arg)
{
    (void)arg;
    int fd = ai_ws_connect();
    if (fd >= 0)
        {
            char json[80];
            snprintf(json, sizeof(json),
                     "{\"type\":\"voice\",\"action\":\"%s\"}",
                     g_ai_voice_action);
            ws_send_text(fd, json);
            close(fd);
        }
    g_ai_voice_busy = 0;
    return NULL;
}

int dm_ai_voice(const char *action)
{
    if (!action || (strcmp(action, "start") != 0 &&
                    strcmp(action, "stop") != 0 &&
                    strcmp(action, "interrupt") != 0 &&
                    strcmp(action, "wake_start") != 0 &&
                    strcmp(action, "wake_stop") != 0))
        return -1;
    /* 2026-08-12 审查修复（B4）：上一轮 worker（如快速点按的 start）未
     * 结束时直接返回会丢消息——release 的 stop 被拒 → agent 一直录音。
     * 短忙等 ≤500ms 等 worker 收尾再发，避免丢帧（localhost 发送 <10ms）。 */
    for (int tries = 0; g_ai_voice_busy && tries < 50; tries++)
        usleep(10 * 1000);
    if (g_ai_voice_busy)
        return -1;

    strncpy(g_ai_voice_action, action, sizeof(g_ai_voice_action) - 1);
    g_ai_voice_action[sizeof(g_ai_voice_action) - 1] = '\0';
    g_ai_voice_busy = 1;

    if (pthread_create(&g_ai_thread, NULL, ai_voice_worker, NULL) != 0)
        {
            g_ai_voice_busy = 0;
            return -1;
        }
    pthread_detach(g_ai_thread);
    return 0;
}

/* ── 2026-08-15 健康助理：直接 TTS 播报文本（健康提醒 L2/L3 语音触达）──
 * 发送 {"type":"voice","action":"speak","text":"..."} → ws_server 调
 * voice_channel_speak()（agent 进程内同源 TTS）。独立 busy 防重入。
 * 2026-08-15 Q2 拍板：记录播报起始时刻，PTT press 时先 interrupt。 */
#define AI_SPEAK_TEXT_MAX 192
#define AI_SPEAK_WINDOW_MS 22000   /* 与 AI_SPEAKING_TIMEOUT_MS 同窗（TTS 最长约 19s） */
static char             g_ai_speak_text[AI_SPEAK_TEXT_MAX];
static volatile int     g_ai_speak_busy;
static uint64_t         g_ai_speak_start_ms;   /* monotonic ms，播报起始 */

static uint64_t ai_speak_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *ai_speak_worker(void *arg)
{
    (void)arg;
    int fd = ai_ws_connect();
    if (fd >= 0)
        {
            char esc[AI_SPEAK_TEXT_MAX * 2 + 8];
            json_escape(g_ai_speak_text, esc, sizeof(esc));
            char json[sizeof(esc) + 64];
            snprintf(json, sizeof(json),
                     "{\"type\":\"voice\",\"action\":\"speak\",\"text\":\"%s\"}",
                     esc);
            ws_send_text(fd, json);
            close(fd);
        }
    g_ai_speak_busy = 0;
    return NULL;
}

int dm_ai_voice_speak(const char *text)
{
    if (!text || !text[0])
        return -1;

    for (int tries = 0; g_ai_speak_busy && tries < 50; tries++)
        usleep(10 * 1000);
    if (g_ai_speak_busy)
        return -1;

    strncpy(g_ai_speak_text, text, sizeof(g_ai_speak_text) - 1);
    g_ai_speak_text[sizeof(g_ai_speak_text) - 1] = '\0';
    g_ai_speak_busy = 1;

    if (pthread_create(&g_ai_thread, NULL, ai_speak_worker, NULL) != 0)
        {
            g_ai_speak_busy = 0;
            return -1;
        }
    pthread_detach(g_ai_thread);
    g_ai_speak_start_ms = ai_speak_now_ms();
    return 0;
}

/* 1 = 健康 speak 播报窗口内（PTT press 先 interrupt 再录音） */
int dm_ai_voice_speaking(void)
{
    if (g_ai_speak_start_ms == 0)
        return 0;
    return (ai_speak_now_ms() - g_ai_speak_start_ms) < AI_SPEAK_WINDOW_MS;
}

/* ── 2026-08-11 MIC DEMO：AI 子页「录音 DEMO」快捷入口 ──────
 * 发送 {"type":"mic","action":"demo"} → ws_server 调 voice_channel_test_mic
 * （录音 5s 落盘 WAV + 自动回放）。短连接 fire-and-forget，独立 busy 防重入。 */
static volatile int     g_ai_mic_busy;

static void *ai_mic_worker(void *arg)
{
    (void)arg;
    int fd = ai_ws_connect();
    if (fd >= 0)
        {
            static const char json[] = "{\"type\":\"mic\",\"action\":\"demo\"}";
            ws_send_text(fd, json);
            close(fd);
        }
    g_ai_mic_busy = 0;
    return NULL;
}

int dm_ai_mic_demo(void)
{
    if (g_ai_mic_busy)
        return -1;
    g_ai_mic_busy = 1;
    if (pthread_create(&g_ai_thread, NULL, ai_mic_worker, NULL) != 0)
        {
            g_ai_mic_busy = 0;
            return -1;
        }
    pthread_detach(g_ai_thread);
    return 0;
}

/* ── 2026-08-15 P84：语音事件长连接订阅（小智式文字显示）──────
 * agent 侧在 ASR/LLM 文本产生时广播
 *   {"type":"voice_evt","kind":"stt|llm","text":"..."}
 * 本线程建常驻 WS 连接收事件，写入 g_ai_evt_stt/g_ai_evt_llm + seq；
 * UI 侧 ai_poll_cb 调 dm_ai_voice_evt_poll 消费（seq 对比判新）。
 * 与 fire-and-forget 短连接 worker 互不冲突（独立线程变量）。 */
static pthread_t        g_ai_evt_thread;
static volatile int     g_ai_evt_run;      /* 1 = 订阅线程运行中 */
static volatile int     g_ai_evt_fd = -1;  /* 常驻连接 fd（unsubscribe 唤醒） */
static char             g_ai_evt_stt[AI_RESP_MAX];
static char             g_ai_evt_llm[AI_RESP_MAX];
static volatile unsigned g_ai_evt_stt_seq;
static volatile unsigned g_ai_evt_llm_seq;
/* 2026-08-23 P115：本地设备控制命令（agent 侧短路 LLM 后广播
 * {"type":"voice_evt","kind":"cmd","text":"volume:-5"|"brightness:+5"}） */
static char             g_ai_evt_cmd[64];
static volatile unsigned g_ai_evt_cmd_seq;
/* 2026-08-23 review-10：agent TTS 播报完成信号（voice_evt kind=done）。
 * 由订阅线程置位，音频仲裁层 dm_audio_fg_poll 消费恢复音乐。 */
static volatile int     g_ai_speak_done;

/* 从 voice_evt JSON 中取 "key":"value"（简单解析，与 extract_content 同风格） */
static void evt_extract(const char *json, const char *key,
                        char *out, int outcap)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        {
            out[0] = '\0';
            return;
        }
    p = strchr(p, ':');
    if (!p)
        {
            out[0] = '\0';
            return;
        }
    p = strchr(p, '"');
    if (!p)
        {
            out[0] = '\0';
            return;
        }
    p++;
    int o = 0;
    while (*p && *p != '"' && o < outcap - 1)
        {
            if (*p == '\\' && p[1] == 'n')      { out[o++] = '\n'; p += 2; }
            else if (*p == '\\' && p[1] == 't') { out[o++] = '\t'; p += 2; }
            else if (*p == '\\' && p[1] == '"') { out[o++] = '"';  p += 2; }
            else if (*p == '\\' && p[1] == '\\'){ out[o++] = '\\'; p += 2; }
            else                                { out[o++] = *p++; }
        }
    out[o] = '\0';
}

static void *ai_evt_worker(void *arg)
{
    (void)arg;
    while (g_ai_evt_run)
        {
            int fd = ai_ws_connect();
            if (fd < 0)
                {
                    usleep(500 * 1000);
                    continue;
                }
            g_ai_evt_fd = fd;
            static char buf[AI_RESP_MAX];
            while (g_ai_evt_run)
                {
                    int n = ws_recv_text(fd, buf, sizeof(buf));
                    if (n == 0)
                        continue;   /* P215：ping 已回 pong，无消息但连接正常 */
                    if (n < 0)
                        {
                            /* SO_RCVTIMEO 30s 超时：连接仍在，继续等；
                             * 断开/close（ECONNRESET 等）→ 退出内循环重连 */
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                continue;
                            break;
                        }
                    if (strstr(buf, "\"type\":\"voice_evt\""))
                        {
                            /* 2026-08-15 P87 修复：text[AI_RESP_MAX]=4096B
                             * 若为栈上数组，默认 pthread 栈（≤8KB）直接溢出，
                             * 破坏 TCB 致 assert 崩溃（P53 教训复发：AI_RESP_MAX
                             * 4096 后栈上 4KB 缓冲压爆默认线程栈）——改 static，
                             * 与同函数 buf 保持一致。kind 同步 static。 */
                            static char kind[16];
                            static char text[AI_RESP_MAX];
                            evt_extract(buf, "kind", kind, sizeof(kind));
                            evt_extract(buf, "text", text, sizeof(text));
                            /* 2026-08-23 review-10：agent 播报完成信号（text 为空，
                             * 必须在 text[0] 判空之外处理，供音频仲裁层恢复音乐） */
                            if (strcmp(kind, "done") == 0)
                                g_ai_speak_done = 1;
                            else if (text[0])
                                {
                                    if (strcmp(kind, "stt") == 0)
                                        {
                                            strncpy(g_ai_evt_stt, text,
                                                sizeof(g_ai_evt_stt) - 1);
                                            g_ai_evt_stt[sizeof(g_ai_evt_stt) - 1] = '\0';
                                            g_ai_evt_stt_seq++;
                                        }
                                    else if (strcmp(kind, "llm") == 0)
                                        {
                                            strncpy(g_ai_evt_llm, text,
                                                sizeof(g_ai_evt_llm) - 1);
                                            g_ai_evt_llm[sizeof(g_ai_evt_llm) - 1] = '\0';
                                            g_ai_evt_llm_seq++;
                                        }
                                    else if (strcmp(kind, "cmd") == 0)
                                        {
                                            /* 2026-08-23 P115：本地设备控制命令 */
                                            strncpy(g_ai_evt_cmd, text,
                                                sizeof(g_ai_evt_cmd) - 1);
                                            g_ai_evt_cmd[sizeof(g_ai_evt_cmd) - 1] = '\0';
                                            g_ai_evt_cmd_seq++;
                                        }
                                }
                        }
                }
            close(fd);
            g_ai_evt_fd = -1;
        }
    return NULL;
}

int dm_ai_voice_evt_subscribe(void)
{
    if (g_ai_evt_run)
        return 0;
    g_ai_evt_run = 1;
    if (pthread_create(&g_ai_evt_thread, NULL, ai_evt_worker, NULL) != 0)
        {
            g_ai_evt_run = 0;
            return -1;
        }
    pthread_detach(g_ai_evt_thread);
    return 0;
}

int dm_ai_voice_evt_unsubscribe(void)
{
    g_ai_evt_run = 0;
    /* 快照 fd 再置 -1，避免 TOCTOU：检查后 worker 换新 fd 导致 shutdown 误伤 */
    int fd = g_ai_evt_fd;
    g_ai_evt_fd = -1;
    /* 2026-09-10 P206 rev：若 worker 在快照后已重连（g_ai_evt_fd 变回
     * >=0），快照 fd 已死——同进程 fd 号可能被其他线程复用（如天气
     * HTTP），此时 shutdown 会误伤，跳过（worker 的 recv 有 30s 超时，
     * 超时/收包后见 run=0 自退）。只有 worker 没重连时才 shutdown
     * 唤醒卡在 recv 里的旧连接。 */
    if (fd >= 0 && g_ai_evt_fd < 0)
        {
            shutdown(fd, SHUT_RDWR);  /* 唤醒 recv 让线程退出 */
        }
    return 0;
}

int dm_ai_voice_evt_poll(char *stt, int stt_cap,
                         char *llm, int llm_cap)
{
    static unsigned last_stt_seq;
    static unsigned last_llm_seq;
    int got = 0;
    unsigned ss = g_ai_evt_stt_seq;
    unsigned ls = g_ai_evt_llm_seq;

    if (ss != last_stt_seq)
        {
            last_stt_seq = ss;
            if (stt && stt_cap > 0 && g_ai_evt_stt[0])
                {
                    strncpy(stt, g_ai_evt_stt, stt_cap - 1);
                    stt[stt_cap - 1] = '\0';
                    got = 1;
                }
        }
    else if (stt && stt_cap > 0)
        {
            /* 2026-08-15 P88 修复：无新 stt 事件必须清空输出缓冲——
             * ai_poll_cb 的 evt_stt 是 static，残留上一次值会导致
             * 下一轮 llm 事件时把旧 stt 重复追加（"你：你好"×2）。 */
            stt[0] = '\0';
        }
    if (ls != last_llm_seq)
        {
            last_llm_seq = ls;
            if (llm && llm_cap > 0 && g_ai_evt_llm[0])
                {
                    strncpy(llm, g_ai_evt_llm, llm_cap - 1);
                    llm[llm_cap - 1] = '\0';
                    got = 1;
                }
        }
    else if (llm && llm_cap > 0)
        {
            /* 2026-08-15 P88 修复：同上，llm 无新事件清空防残留 */
            llm[0] = '\0';
        }
    return got;
}

/* 2026-08-23 P115：消费本地设备控制命令事件（agent 侧短路 LLM 后广播）。
 * 有新 cmd 事件则拷入 cmd 缓冲（如 "volume:-5"）并返回 1，否则返回 0。 */
int dm_ai_voice_evt_cmd_poll(char *cmd, int cmd_cap)
{
    static unsigned last_cmd_seq;
    if (!cmd || cmd_cap <= 0)
        return 0;
    unsigned cs = g_ai_evt_cmd_seq;
    if (cs != last_cmd_seq)
        {
            last_cmd_seq = cs;
            if (g_ai_evt_cmd[0])
                {
                    strncpy(cmd, g_ai_evt_cmd, cmd_cap - 1);
                    cmd[cmd_cap - 1] = '\0';
                    return 1;
                }
        }
    return 0;
}

/* 2026-08-23 review-10：消费 agent TTS 播报完成信号（kind=done）。
 * 有新 done 事件则返回 1（供音频仲裁层恢复音乐），否则返回 0。 */
int dm_ai_voice_evt_done_poll(void)
{
    if (g_ai_speak_done)
        {
            g_ai_speak_done = 0;
            return 1;
        }
    return 0;
}
