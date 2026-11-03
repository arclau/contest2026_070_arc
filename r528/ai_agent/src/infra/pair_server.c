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

/* pair_server.c — 手机扫码配对页（2026-11-01 P330）
 *
 * 目的：板子没有键盘，输入 32 位 API Key 不现实。设备在局域网里起一个
 * 极小的 HTTP 服务（默认 9000），设置页把 http://<设备IP>:9000/ 显示成
 * 二维码；手机扫码 → 浏览器打开表单 → 粘贴 Key → 保存到
 * /data/ai_agent/config/config.json，并立刻把语音后端切到小米。
 *
 * 为什么不用"真正的扫码登录"：小米 MiMo 开放平台没有 OAuth 设备授权
 * 端点，唯一凭据就是控制台创建的 API Key。所以扫码只能是"把 Key 送进
 * 设备"的通道，而不是账号授权。这个页面等价于路由器/ESP32 的配网页，
 * 无外部依赖、断网也能用。
 *
 * 安全边界：只监听局域网、只接受 4KB 以内的表单；Key 只落本机，
 * 不回显（页面显示"已保存"）。同网段其他人也能打开这个页面，这是
 * 已知限制，写入的是他们自己的 Key，不会泄露设备上的 Key。
 */

#include "infra/pair_server.h"

#include "infra/config_store.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "voice/voice_channel.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#define TAG "pair"

#define PAIR_REQ_MAX 4096
#define PAIR_STACK (8 * 1024)
#define PAIR_PRIO 45

static int s_listen_fd = -1;
static pthread_t s_thread;
static volatile int s_running;

/****************************************************************************
 * 页面 — 材质与配色对齐 V1 设计语言（dm_glass.h + deskmate_ui.h）
 *
 *   背景：蓝青→薄荷大圆弧洗色 #DCE7FA → #F4EDF6（DM_GLASS_WASH_*）
 *   卡片：白 70% + 白边 80% + 对称柔影 #40516A/30%（DM_GLASS_CARD/EDGE/SHADOW）
 *   圆角：DM_GLASS_RADIUS 40（1920 宽面板 → 手机按比例收到 22px）
 *   文字：#1C1C1E 主 / #8E8E93 次；分隔线 #C6C6C8；强调色 #007AFF
 *   版式：大标题 + 分组卡 + 行（图标块/主文案/值），与设置页同构
 *
 * 属性一律用单引号，C 字符串里就不用反斜杠转义。
 ****************************************************************************/

#define PAIR_PAGE_BUF (8 * 1024)

/* 行首图标（白字形 + 彩色圆角底，对应 dm_popup_icon） */
static const char s_ic_spark[] =
    "<svg viewBox='0 0 24 24'><path d='M12 2l2.3 6.4L21 11l-6.7 2.6L12 20l-2.3-6.4L3 11l6.7-2.6z'/></svg>";
static const char s_ic_mic[] =
    "<svg viewBox='0 0 24 24'><path d='M12 15a3.5 3.5 0 0 0 3.5-3.5V6a3.5 3.5 0 1 0-7 0v5.5A3.5 3.5 0 0 0 12 15zm6-3.5a6 6 0 0 1-5 5.9V20h-2v-2.6a6 6 0 0 1-5-5.9h2a4 4 0 0 0 8 0z'/></svg>";
static const char s_ic_spk[] =
    "<svg viewBox='0 0 24 24'><path d='M4 9v6h4l5 4V5L8 9H4zm12.5 3a4.5 4.5 0 0 0-2.5-4v8a4.5 4.5 0 0 0 2.5-4z'/></svg>";
static const char s_ic_key[] =
    "<svg viewBox='0 0 24 24'><path d='M14 2a6 6 0 0 0-5.7 7.9L2 16.2V22h5.8l6.3-6.3A6 6 0 1 0 14 2zm2.5 5a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3z'/></svg>";

static const char s_html_head[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<meta name='color-scheme' content='light'>"
    "<title>桌面伴侣 · AI 语音</title><style>"
    "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
    "html,body{margin:0;padding:0}"
    "body{min-height:100vh;padding-bottom:44px;color:#1C1C1E;"
    "background:linear-gradient(180deg,#DCE7FA 0%,#F4EDF6 100%);"
    "font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Helvetica Neue',sans-serif;"
    "-webkit-font-smoothing:antialiased}"
    ".wrap{max-width:520px;margin:0 auto;padding:26px 16px 0}"
    "h1{font-size:34px;line-height:1.15;font-weight:700;letter-spacing:-.4px;margin:6px 0 6px}"
    ".sub{font-size:15px;color:#8E8E93;margin:0 0 24px}"
    ".label{font-size:13px;font-weight:500;color:#8E8E93;margin:22px 0 8px;padding-left:16px}"
    ".card{background:rgba(255,255,255,.70);border:1px solid rgba(255,255,255,.80);"
    "border-radius:22px;box-shadow:0 3px 20px rgba(64,81,106,.30);overflow:hidden;"
    "backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px)}"
    ".row{display:flex;align-items:center;gap:12px;min-height:54px;padding:0 16px}"
    ".row+.row{border-top:.5px solid rgba(198,198,200,.65)}"
    ".ico{flex:0 0 auto;width:29px;height:29px;border-radius:8px;display:flex;"
    "align-items:center;justify-content:center}"
    ".ico svg{width:17px;height:17px;fill:#fff}"
    ".k{flex:1;font-size:17px;line-height:1.3}"
    ".v{font-size:17px;color:#8E8E93;white-space:nowrap;overflow:hidden;"
    "text-overflow:ellipsis;max-width:54%}"
    ".badge{display:inline-flex;align-items:center;gap:7px;font-size:17px;color:#8E8E93}"
    ".dot{width:8px;height:8px;border-radius:50%}"
    ".dot-on{background:#34C759}.dot-off{background:#FF3B30}"
    "input,select{font:inherit;font-size:17px;color:#1C1C1E;background:transparent;"
    "border:0;outline:0;width:100%;padding:16px 0}"
    "input::placeholder{color:#B4B4B9}"
    "select{appearance:none;-webkit-appearance:none;padding-right:22px}"
    ".sel{position:relative;flex:0 0 40%}"
    ".sel:after{content:'';position:absolute;right:2px;top:50%;width:7px;height:7px;"
    "border-right:2px solid #C6C6C8;border-bottom:2px solid #C6C6C8;"
    "transform:translateY(-70%) rotate(45deg)}"
    "details summary{list-style:none;cursor:pointer}"
    "details summary::-webkit-details-marker{display:none}"
    ".chev{width:7px;height:7px;border-right:2px solid #C6C6C8;border-bottom:2px solid #C6C6C8;"
    "transform:rotate(45deg);transition:transform .18s ease}"
    "details[open] .chev{transform:rotate(-135deg)}"
    "button{display:block;width:100%;margin-top:24px;padding:17px;font:inherit;"
    "font-size:17px;font-weight:600;color:#fff;background:#007AFF;border:0;"
    "border-radius:999px;box-shadow:0 6px 18px rgba(0,122,255,.32)}"
    "button:active{opacity:.86}"
    ".hint{font-size:13px;line-height:1.7;color:#8E8E93;padding:14px 0 0}"
    ".ok{color:#34C759}"
    "</style></head><body><div class='wrap'>";

static const char s_html_tail[] = "</div></body></html>";

static const char s_form_fmt[] =
    "<h1>AI 语音</h1><p class='sub'>小米 MiMo · 大模型 / 识别 / 合成</p>"
    "<div class='label'>服务</div><div class='card'>"
    "<div class='row'><span class='ico' style='background:#8B5CF6'>%s</span>"
    "<span class='k'>当前服务</span>"
    "<span class='badge'><span class='dot %s'></span>%s</span></div>"
    "<div class='row'><span class='ico' style='background:#FF9500'>%s</span>"
    "<span class='k'>语音识别</span><span class='v'>%s</span></div>"
    "<div class='row'><span class='ico' style='background:#34C759'>%s</span>"
    "<span class='k'>语音合成</span><span class='v'>%s</span></div>"
    "</div>"
    "<form method='post' action='/api/config'>"
    "<div class='label'>密钥</div><div class='card'>"
    "<div class='row'><span class='ico' style='background:#007AFF'>%s</span>"
    "<input name='key' type='text' placeholder='%s' autocomplete='off' spellcheck='false'></div>"
    "<div class='row'><span class='k'>音色</span><span class='sel'><select name='voice'>%s</select></span></div>"
    "</div>"
    "<details><summary><div class='card' style='margin-top:12px'><div class='row'>"
    "<span class='k'>高级 · 模型 ID</span><span class='chev'></span></div></div></summary>"
    "<div class='card' style='margin-top:10px'>"
    "<div class='row'><span class='k'>识别模型</span>"
    "<input name='asr_model' placeholder='mimo-v2.5-asr' value='%s'></div>"
    "<div class='row'><span class='k'>合成模型</span>"
    "<input name='tts_model' placeholder='mimo-v2.5-tts' value='%s'></div>"
    "</div></details>"
    "<button type='submit'>保存到设备</button></form>"
    "<p class='hint'>Key 只写入本机 /data/ai_agent/config/config.json，不经过第三方服务器。<br>"
    "官方换模型名时在这里改 ID 即可，不用重刷固件。</p>";

static const char s_page_ok[] =
    "<h1>已保存</h1><p class='sub ok'>语音已切到小米，可以直接对设备说话。</p>"
    "<div class='card'><div class='row'><span class='k'>后端</span>"
    "<span class='v'>小米 MiMo</span></div></div>"
    "<p class='hint'>可以关闭这个页面了。需要换 Key 就重新扫码。</p>";

static const char s_page_fail[] =
    "<h1>保存失败</h1><p class='sub'>Key 无效，或设备配置无法写入。</p>"
    "<p class='hint'>检查 Key 后重试；仍失败时检查 /data 空间和写权限。</p>";

/****************************************************************************
 * 工具
 ****************************************************************************/

/* 预置音色（与小米文档一致；顺序即下拉顺序） */
static const char *s_voices[] = {
    "冰糖", "茉莉", "苏打", "白桦", "Mia", "Chloe", "Milo", "Dean"
};

/* 拼完整页面（head + 正文 + tail），动态部分：Key 状态、当前音色/模型 */
static void build_form_page(char *buf, size_t cap)
{
    char key[160] = { 0 };
    char voice[64] = { 0 };
    char asr[48] = { 0 };
    char tts[48] = { 0 };
    char opts[640];
    size_t o = 0;
    int has;
    int i;
    int n;

    has = (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, key, sizeof(key)) == OK
           && key[0] != '\0');

    if (claw_config_get(AGENT_CFG_KEY_MIMO_TTS_VOICE, voice, sizeof(voice)) != OK
        || voice[0] == '\0')
        strlcpy(voice, AGENT_MIMO_TTS_VOICE_DEFAULT, sizeof(voice));

    if (claw_config_get(AGENT_CFG_KEY_MIMO_ASR_MODEL, asr, sizeof(asr)) != OK
        || asr[0] == '\0')
        strlcpy(asr, AGENT_MIMO_ASR_MODEL, sizeof(asr));

    if (claw_config_get(AGENT_CFG_KEY_MIMO_TTS_MODEL, tts, sizeof(tts)) != OK
        || tts[0] == '\0')
        strlcpy(tts, AGENT_MIMO_TTS_MODEL, sizeof(tts));

    opts[0] = '\0';
    for (i = 0; i < (int)(sizeof(s_voices) / sizeof(s_voices[0])); i++)
    {
        int used = snprintf(opts + o, sizeof(opts) - o, "<option%s>%s</option>",
                            strcmp(voice, s_voices[i]) == 0 ? " selected" : "",
                            s_voices[i]);
        if (used <= 0 || o + (size_t)used >= sizeof(opts))
            break;
        o += (size_t)used;
    }

    n = snprintf(buf, cap, "%s", s_html_head);
    if (n <= 0 || (size_t)n >= cap)
        return;

    n += snprintf(buf + n, cap - (size_t)n, s_form_fmt,
                  s_ic_spark, has ? "dot-on" : "dot-off", has ? "已配置" : "未配置",
                  s_ic_mic, asr,
                  s_ic_spk, tts,
                  s_ic_key, has ? "已保存，重新输入可覆盖" : "sk-...",
                  opts, asr, tts);
    if (n <= 0 || (size_t)n >= cap)
        return;

    snprintf(buf + n, cap - (size_t)n, "%s", s_html_tail);
}

static void url_decode(char *s)
{
    char *o = s;

    while (*s)
    {
        if (*s == '+')
        {
            *o++ = ' ';
            s++;
        }
        else if (*s == '%' && s[1] && s[2])
        {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        }
        else
        {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/* 从 form-urlencoded body 里取字段（原地修改 body） */
static char *form_field(char *body, const char *name)
{
    size_t nlen = strlen(name);
    char *p = body;

    while (p && *p)
    {
        char *amp = strchr(p, '&');
        char *eq = strchr(p, '=');

        if (eq && (size_t)(eq - p) == nlen && strncmp(p, name, nlen) == 0)
        {
            char *val = eq + 1;
            if (amp)
                *amp = '\0';
            url_decode(val);
            return val;
        }

        if (!amp)
            break;
        p = amp + 1;
    }
    return NULL;
}

static void send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0)
            return;
        off += (size_t)n;
    }
}

static void send_response(int fd, int code, const char *status,
                          const char *body)
{
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n\r\n",
        code, status, (int)strlen(body));

    send_all(fd, hdr, (size_t)hlen);
    send_all(fd, body, strlen(body));
}

/* 用同一套 head/tail 包住正文，保证成功页/失败页与表单页材质一致 */
static void send_page(int fd, int code, const char *status, const char *body)
{
    char *page = malloc(PAIR_PAGE_BUF);

    if (!page)
        return;

    if (snprintf(page, PAIR_PAGE_BUF, "%s%s%s",
                 s_html_head, body, s_html_tail) > 0)
    {
        send_response(fd, code, status, page);
    }

    free(page);
}

static void send_status_json(int fd)
{
    char json[256];
    char key[160] = { 0 };
    int has = (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, key, sizeof(key)) == OK
               && key[0] != '\0');
    const char *asr = voice_asr_get_backend();
    const char *tts = voice_tts_get_backend();

    snprintf(json, sizeof(json),
        "{\"mimo_key\":%s,\"asr\":\"%s\",\"tts\":\"%s\"}\n",
        has ? "true" : "false",
        asr ? asr : "none", tts ? tts : "none");

    {
        char hdr[160];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n",
            (int)strlen(json));
        send_all(fd, hdr, (size_t)hlen);
        send_all(fd, json, strlen(json));
    }
}

/****************************************************************************
 * 请求处理
 ****************************************************************************/

static void handle_client(int fd)
{
    char *req = malloc(PAIR_REQ_MAX);
    size_t len = 0;
    char *body;
    char *hdr_end;
    int content_length = 0;
    int is_post;
    char method[8] = { 0 };
    char path[128] = { 0 };

    if (!req)
        return;

    /* 读请求头 */
    while (len < PAIR_REQ_MAX - 1)
    {
        ssize_t n = recv(fd, req + len, PAIR_REQ_MAX - 1 - len, 0);
        if (n <= 0)
            break;
        len += (size_t)n;
        req[len] = '\0';
        if (memmem(req, len, "\r\n\r\n", 4))
            break;
    }
    req[len] = '\0';

    if (len == 0 || sscanf(req, "%7s %127s", method, path) != 2)
    {
        free(req);
        return;
    }

    hdr_end = (char *)memmem(req, len, "\r\n\r\n", 4);
    if (!hdr_end)
    {
        free(req);
        return;
    }

    {
        char *cl = strcasestr(req, "Content-Length:");
        if (cl && cl < hdr_end)
            content_length = atoi(cl + strlen("Content-Length:"));
    }

    body = hdr_end + 4;
    if (content_length > 0)
    {
        size_t have = (size_t)(req + len - body);
        while (have < (size_t)content_length && len < PAIR_REQ_MAX - 1)
        {
            ssize_t n = recv(fd, req + len, PAIR_REQ_MAX - 1 - len, 0);
            if (n <= 0)
                break;
            len += (size_t)n;
            req[len] = '\0';
            have = (size_t)(req + len - body);
        }
        have = (size_t)(req + len - body);
        if (have > (size_t)content_length)
            have = (size_t)content_length;
        body[have] = '\0';
    }
    else
    {
        body[0] = '\0';
    }

    is_post = (strcmp(method, "POST") == 0);

    syslog(LOG_INFO, "[%s] %s %s (%d bytes)\n", TAG, method, path, content_length);

    if (is_post && strcmp(path, "/api/config") == 0)
    {
        char *key = form_field(body, "key");
        char *voice = form_field(body, "voice");
        char *asr_model = form_field(body, "asr_model");
        char *tts_model = form_field(body, "tts_model");

        if (!key || strlen(key) < 8)
        {
            send_page(fd, 400, "Bad Request", s_page_fail);
        }
        else
        {
            int cfg_ok = OK;

#define PAIR_SET_CFG(k, v) do { \
    if (claw_config_set((k), (v)) != OK) \
        cfg_ok = ERROR; \
} while (0)

            PAIR_SET_CFG(AGENT_CFG_KEY_MIMO_KEY, key);
            PAIR_SET_CFG(AGENT_CFG_KEY_VOICE_ASR_BACKEND, "xiaomi");
            PAIR_SET_CFG(AGENT_CFG_KEY_VOICE_TTS_BACKEND, "xiaomi");
            if (voice && voice[0])
                PAIR_SET_CFG(AGENT_CFG_KEY_MIMO_TTS_VOICE, voice);
            if (asr_model && asr_model[0])
                PAIR_SET_CFG(AGENT_CFG_KEY_MIMO_ASR_MODEL, asr_model);
            if (tts_model && tts_model[0])
                PAIR_SET_CFG(AGENT_CFG_KEY_MIMO_TTS_MODEL, tts_model);

            /* 文本大模型也一起切到小米：一个 Key 打通 LLM+ASR+TTS，
             * 用户扫码后不需要再进 NSH 敲 set_llm。 */
            PAIR_SET_CFG(AGENT_CFG_KEY_LLM_HOST, AGENT_LLM_MIMO_HOST);
            PAIR_SET_CFG(AGENT_CFG_KEY_LLM_PATH, AGENT_LLM_MIMO_PATH);
            PAIR_SET_CFG(AGENT_CFG_KEY_MODEL, AGENT_LLM_MIMO_MODEL);
            PAIR_SET_CFG(AGENT_CFG_KEY_API_KEY, key);

            if (cfg_ok != OK)
            {
                syslog(LOG_ERR, "[%s] failed to save MiMo config\n", TAG);
                send_page(fd, 500, "Internal Error", s_page_fail);
                free(req);
                return;
            }

#undef PAIR_SET_CFG
            llm_set_all(AGENT_LLM_MIMO_HOST, AGENT_LLM_MIMO_PATH, "443",
                        key, AGENT_LLM_MIMO_MODEL);

            /* P337: llm_set_all 只改 llm_proxy 的全局量，但每次对话前
             * agent_loop 会 llm_router_apply(idx) 把 router 自己的 backend
             * 覆盖回去。若 router slot 0 还是旧的（如构建期 ark 默认值），
             * 扫码写进来的 MiMo Key 永远不会用于 LLM —— 板端表现为
             * ASR 正常、LLM 400 InvalidEndpoint.ClosedEndpoint。
             * 所以扫码保存必须同时重写 router slot 0。 */
            llm_backend_t mb = { 0 };
            strlcpy(mb.host, AGENT_LLM_MIMO_HOST, sizeof(mb.host));
            strlcpy(mb.path, AGENT_LLM_MIMO_PATH, sizeof(mb.path));
            strlcpy(mb.port, "443", sizeof(mb.port));
            strlcpy(mb.model, AGENT_LLM_MIMO_MODEL, sizeof(mb.model));
            strlcpy(mb.api_key, key, sizeof(mb.api_key));
            mb.priority = 0;
            mb.cost_tier = 1;
            mb.enabled = true;
            llm_router_set_backend(0, &mb);
            llm_router_apply(0);

            voice_channel_reload_backends();
            syslog(LOG_INFO, "[%s] mimo key saved from phone (voice=%s)\n",
                   TAG, voice ? voice : "default");
            send_page(fd, 200, "OK", s_page_ok);
        }
    }
    else if (!is_post && strcmp(path, "/api/status") == 0)
    {
        send_status_json(fd);
    }
    else if (!is_post)
    {
        char *page = malloc(PAIR_PAGE_BUF);

        if (page)
        {
            build_form_page(page, PAIR_PAGE_BUF);
            send_response(fd, 200, "OK", page);
            free(page);
        }
        else
        {
            send_page(fd, 500, "Internal Error", s_page_fail);
        }
    }
    else
    {
        send_page(fd, 404, "Not Found", s_page_fail);
    }

    free(req);
}

/****************************************************************************
 * 线程
 ****************************************************************************/

static void *pair_thread(void *arg)
{
    (void)arg;

    while (s_running)
    {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int fd = accept(s_listen_fd, (struct sockaddr *)&cli, &clen);

        if (fd < 0)
        {
            if (s_running)
                usleep(100 * 1000);
            continue;
        }

        {
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        handle_client(fd);
        close(fd);
    }

    return NULL;
}

int pair_server_start(void)
{
    struct sockaddr_in addr;
    int opt = 1;
    pthread_attr_t attr;

    if (s_running)
        return OK;

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0)
    {
        syslog(LOG_ERR, "[%s] socket failed: %d\n", TAG, errno);
        return ERROR;
    }

    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(AGENT_PAIR_PORT);

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        syslog(LOG_ERR, "[%s] bind %d failed: %d\n", TAG, AGENT_PAIR_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ERROR;
    }

    if (listen(s_listen_fd, 4) < 0)
    {
        syslog(LOG_ERR, "[%s] listen failed: %d\n", TAG, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ERROR;
    }

    s_running = 1;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PAIR_STACK);
    if (pthread_create(&s_thread, &attr, pair_thread, NULL) != 0)
    {
        pthread_attr_destroy(&attr);
        s_running = 0;
        close(s_listen_fd);
        s_listen_fd = -1;
        syslog(LOG_ERR, "[%s] pthread_create failed\n", TAG);
        return ERROR;
    }
    pthread_attr_destroy(&attr);

    syslog(LOG_NOTICE, "[%s] pairing page on http://0.0.0.0:%d/\n",
           TAG, AGENT_PAIR_PORT);
    return OK;
}

void pair_server_stop(void)
{
    if (!s_running)
        return;

    s_running = 0;
    if (s_listen_fd >= 0)
    {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    pthread_join(s_thread, NULL);
}
