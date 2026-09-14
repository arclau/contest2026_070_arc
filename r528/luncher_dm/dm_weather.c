/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_weather.c
 * Open-Meteo daily weather client for Desktop Mate (board + simulator)
 *
 * Plain-C HTTP GET over BSD socket — no TLS, no external JSON library.
 * Endpoint: http://api.open-meteo.com/v1/forecast (free, no API key).
 * Parses only the fields we need (hand-rolled strstr extraction).
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

#include "dm_weather.h"
#include "dm_city.h"      /* 2026-09-10 A 方案：坐标按 Settings 城市运行时取 */
#include "dm_health_cfg.h"  /* 2026-09-10 P211：自动对时总闸持久化 */

/* 默认坐标（惠州，dm_city 表 idx0 同值；cfg 缺失时 dm_city 兜底用） */
#define DM_W_LAT      "23.11"
#define DM_W_LON      "114.42"
/* 2026-09-10 P206 板上实锤：Asia%2FShanghai 被 open-meteo 拒
 * （body={"error":true,"reason":"Invalid timezone"}，见 910bootlog），
 * 编码斜杠过代理会被拒；改直写斜杠后天气链路可用。失败时直接返回 -1，
 * UI 保持"加载中…"并 30s 重试，不再填充假数据。 */
#define DM_W_TZ       "Asia/Shanghai"
#define DM_W_HOST     "api.open-meteo.com"
#define DM_W_PORT     80

#ifndef DM_W_DAYS_STR
#define DM_W_DAYS_STR  "5"
#endif

/* 2026-09-10：URL 改运行时拼接（城市切换即换坐标；旧 DM_W_PATH
 * 编译期写死惠州已删）。 */
static void dm_w_build_path(char *out, size_t cap)
{
    const dm_city_t *c = dm_city_cur();
    snprintf(out, cap,
        "/v1/forecast?latitude=%s&longitude=%s"
        "&current=temperature_2m,relative_humidity_2m,weather_code"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&timezone=" DM_W_TZ "&forecast_days=" DM_W_DAYS_STR,
        c->lat, c->lon);
}

/* ================================================================
 * 自动对时总闸（2026-09-10 P211：日期与时间子页）
 * 开=天气链路 HTTP Date 到即 settimeofday（默认，保持旧行为）；
 * 关=只取天气不对时，手动设置长期有效。落盘 dm_health.cfg。
 * ================================================================ */

#define DM_TIME_CFG_KEY   "time_auto_sync"
#define DM_TIME_STATE_PATH "/data/dm_time.state"
#define DM_TIME_VALID_MIN 1600000000L   /* 2020-09-13；早于此视为未对时 */

static long g_time_last_sync;   /* 上次自动同步的 UTC epoch（0=从未） */

int dm_time_auto_enabled(void)
{
    return dm_health_cfg_get_int(DM_TIME_CFG_KEY, 1) ? 1 : 0;
}

void dm_time_set_auto(int on)
{
    dm_health_cfg_set_int(DM_TIME_CFG_KEY, on ? 1 : 0);
}

void dm_time_note_synced(long utc_sec)
{
    g_time_last_sync = utc_sec;
    /* 落盘：无 RTC 电池，重启会回到 1970；开机早期用此值兜底，让时段
     * 问候/时钟立即合理，联网对时到了再覆盖校正。 */
    FILE *f = fopen(DM_TIME_STATE_PATH, "w");
    if (f)
        {
            fprintf(f, "%ld\n", utc_sec);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
        }
}

long dm_time_last_sync(void)
{
    return g_time_last_sync;
}

/* 开机早期调用：系统时间仍未对时（<2020）时，用上次落盘时间兜底。
 * 返回 1=已恢复 / 0=本就有有效时间 / -1=无可用存档。 */
int dm_time_restore_at_boot(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec >= DM_TIME_VALID_MIN)
        return 0;

    FILE *f = fopen(DM_TIME_STATE_PATH, "r");
    if (!f)
        return -1;
    long saved = 0;
    if (fscanf(f, "%ld", &saved) != 1)
        saved = 0;
    fclose(f);
    if (saved < DM_TIME_VALID_MIN)
        return -1;

    struct timeval tv;
    tv.tv_sec  = saved;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0)
        return -1;
    g_time_last_sync = saved;
    printf("[time] restored last known UTC %ld at boot\n", saved);
    return 1;
}

/* ================================================================
 * HTTP GET (blocking, ~5s timeout)
 * ================================================================ */

/* 2026-08-10：API 对接 debug（最后一步失败原因 + 总耗时）。
 * worker 线程写，UI 线程经 dm_weather_debug() 整串读取。 */
static char g_dbg[128];

static void dbg_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_dbg, sizeof(g_dbg), fmt, ap);
    va_end(ap);
}

/* P133（2026-08-28）：天气落盘 /data/dm_weather.state 共享给 ai_agent——
 * AI 播报天气直接读本地数据（dm_weather 已拉 open-meteo），不联网搜索。
 * 格式："温度 湿度 中文描述 城市"（2026-09-10 加第 4 字段城市名；
 * agent 侧读前 3 字段兼容旧文件，城市缺失即按"本地"播报）。 */
static void weather_state_save(const dm_weather_t *w)
{
    FILE *f = fopen("/data/dm_weather.state", "w");
    if (f)
    {
        fprintf(f, "%d %d %s %s\n", w->cur_temp, w->cur_humid,
                dm_wmo_text(w->cur_code), dm_city_cur()->name);
        fclose(f);
    }
}

/* 2026-08-11：解析 HTTP 响应头 Date:（RFC 7231 IMF-fixdate，UTC 时间）
 * → time_t。Date 头是 GMT，必须用 timegm（按 UTC 解释）——不能用
 * mktime：板上 rc.sysinit 已 set TZ Asia/Shanghai（CONFIG_LIBC_LOCALTIME=y），
 * mktime 会按东八区解释导致 -8h 偏差（上板实测时间慢 8 小时）。成功返回
 * 0，失败返回 -1。 */
static int parse_http_date(const char *raw, time_t *out)
{
    static const char *months[] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    /* "Date: Tue, 11 Aug 2026 01:05:02 GMT" */
    const char *p = strstr(raw, "Date:");
    int mday = 0, year = 0, hh = 0, mi = 0, ss = 0;
    char mon[4] = {0};
    int mon_i = -1;
    int i;
    struct tm tm;

    if (p == NULL)
        return -1;

    if (sscanf(p, "Date: %*s %d %3s %d %d:%d:%d GMT",
               &mday, mon, &year, &hh, &mi, &ss) != 6)
        return -1;

    for (i = 0; i < 12; i++)
        if (strncmp(mon, months[i], 3) == 0)
            {
                mon_i = i;
                break;
            }

    if (mon_i < 0 || year < 2000 || year > 2100 ||
        mday < 1 || mday > 31 || hh > 23 || mi > 59 || ss > 60)
        return -1;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon_i;
    tm.tm_mday = mday;
    tm.tm_hour = hh;
    tm.tm_min  = mi;
    tm.tm_sec  = ss;

    *out = timegm(&tm);   /* Date 头是 GMT：timegm 按 UTC 解释（勿用 mktime，TZ 会致 -8h） */
    return (*out == (time_t)-1) ? -1 : 0;
}

static int http_get(const char *path, char *out, int outlen,
                    time_t *server_time)
{
    struct hostent *he;
    struct sockaddr_in sa;
    int fd, n, total = 0;
    /* 2026-08-10 修复：请求实际 261 字节，原 req[256] 截断成
     * "Connection: clos"（缺 \r\n\r\n）→ 服务器等不到完整请求头
     * → 板端 recv 5s 超时空收（上板日志 recv empty (timeout?)）。
     * 加大缓冲 + snprintf 返回值防截断。 */
    char req[512];
    int reqlen;

    he = gethostbyname(DM_W_HOST);
    if (he == NULL)
        {
            dbg_set("DNS fail: %s", DM_W_HOST);
            return -1;
        }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        {
            dbg_set("socket fail");
            return -1;
        }

    /* 2026-08-10：加 5s 收发超时——开机 WiFi 未就绪时 http_get 可能
     * 阻塞在 connect/recv 上，天气 timer 在 UI 线程执行会卡界面。 */
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(DM_W_PORT);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        {
            dbg_set("connect fail (%d)", errno);
            close(fd);
            return -1;
        }

    reqlen = snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, DM_W_HOST);
    if (reqlen < 0 || reqlen >= (int)sizeof(req))
        {
            dbg_set("req truncated (%d)", reqlen);
            close(fd);
            return -1;
        }

    if (send(fd, req, reqlen, 0) < 0)
        {
            dbg_set("send fail (%d)", errno);
            close(fd);
            return -1;
        }

    while (total < outlen - 1)
        {
            n = recv(fd, out + total, outlen - 1 - total, 0);
            if (n <= 0)
                break;
            total += n;
        }

    close(fd);
    out[total] = '\0';

    /* 2026-08-11：跳头前先解析 Date: 头（NTP 不可达时的对时兜底）。
     * 服务器返回的 Date 是 UTC，直接得到 UTC 时间戳供 settimeofday。 */
    if (server_time != NULL)
        parse_http_date(out, server_time);

    /* Skip HTTP header */
    char *body = strstr(out, "\r\n\r\n");
    if (body != NULL)
        {
            body += 4;
            memmove(out, body, strlen(body) + 1);
        }

    if (strlen(out) == 0)
        {
            dbg_set("recv empty (timeout?)");
            return -1;
        }

    return 0;
}

/* ================================================================
 * Tiny JSON field extraction (no library)
 * ================================================================ */

/* Find "key" then a following ':' then an integer number. */
static int json_int(const char *json, const char *key, int *val)
{
    const char *p = strstr(json, key);
    if (p == NULL)
        return -1;

    p = strchr(p, ':');
    if (p == NULL)
        return -1;
    p++;

    while (*p == ' ' || *p == '\t')
        p++;

    *val = atoi(p);
    return 0;
}

/* Extract an array of integers: "key":[a,b,c] */
static int json_int_array(const char *json, const char *key,
                          int *out, int max)
{
    const char *p = strstr(json, key);
    int n = 0;

    if (p == NULL)
        return 0;

    p = strchr(p, '[');
    if (p == NULL)
        return 0;
    p++;

    while (n < max)
        {
            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            if (*p == ']' || *p == '\0')
                break;
            out[n++] = atoi(p);
            /* 2026-09-10 P206 rev：负号必须在数字前跳过——旧代码先跳数字
             * 后跳负号，"-5" 被记成 -5 和 5 两个元素，冬季 tmin<0 时数组
             * 错位（日期/温度对不上）。 */
            if (*p == '-')          /* negative */
                p++;
            while (*p >= '0' && *p <= '9')
                p++;
            if (*p == '.')          /* decimals: skip fraction */
                {
                    p++;
                    while (*p >= '0' && *p <= '9')
                        p++;
                }
        }

    return n;
}

/* Extract an array of quoted strings: "key":["a","b",...] */
static int json_str_array(const char *json, const char *key,
                          char out[][16], int max)
{
    const char *p = strstr(json, key);
    int n = 0;

    if (p == NULL)
        return 0;

    p = strchr(p, '[');
    if (p == NULL)
        return 0;
    p++;

    while (n < max)
        {
            const char *q = strchr(p, '"');
            if (q == NULL)
                break;
            q++;
            const char *e = strchr(q, '"');
            if (e == NULL)
                break;
            size_t len = e - q;
            if (len > 15)
                len = 15;
            memcpy(out[n], q, len);
            out[n][len] = '\0';
            n++;
            p = e + 1;
        }

    return n;
}

/* ================================================================
 * Weekday from "YYYY-MM-DD" (Zeller) → 0=Sun..6=Sat
 * ================================================================ */

static int weekday_from_date(const char *date)
{
    int y, m, d;
    if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3)
        return -1;
    if (m < 3)
        {
            m += 12;
            y--;
        }
    /* Zeller: h = (q + 13(m+1)/5 + K + K/4 + J/4 + 5J) % 7, 0=Sat */
    int h = (d + (13 * (m + 1)) / 5 + (y % 100) + (y % 100) / 4
             + (y / 100) / 4 + 5 * (y / 100)) % 7;
    return (h + 6) % 7;     /* shift 0=Sat → 0=Sun */
}

/* ================================================================
 * Public API
 * ================================================================ */

int dm_weather_fetch(dm_weather_t *w)
{
    static char buf[4096];
    char dates[DM_WEATHER_DAYS][16];
    int codes[DM_WEATHER_DAYS]  = {0};
    int tmax[DM_WEATHER_DAYS]   = {0};
    int tmin[DM_WEATHER_DAYS]   = {0};
    int n, i;
    struct timeval t0, t1;
    time_t server_time = 0;

    if (w == NULL)
        return -1;

    memset(w, 0, sizeof(*w));

    /* 2026-08-11：记录耗时供 debug 显示 */
    gettimeofday(&t0, NULL);

    /* 2026-09-10：按当前城市拼 URL（旧 DM_W_PATH 编译期写死已删） */
    {
        char path[256];
        dm_w_build_path(path, sizeof(path));
        if (http_get(path, buf, sizeof(buf), &server_time) < 0)
        {
            gettimeofday(&t1, NULL);
            /* 2026-08-10 修复：g_dbg 既是 vsnprintf 目标又是 %s 源，
             * 重叠导致 "FAIL xms: FAIL xms: " 双重嵌套、真实原因被覆盖。
             * 先拷到局部缓冲再格式化。 */
            char reason[sizeof(g_dbg)];
            strncpy(reason, g_dbg, sizeof(reason) - 1);
            reason[sizeof(reason) - 1] = '\0';
            dbg_set("FAIL %ldms: %s",
                    (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                           (t1.tv_usec - t0.tv_usec) / 1000),
                    reason);   /* http_get 写的具体失败原因 */
            return -1;   /* 2026-08-10：失败返回 -1，UI 据此缩短重试周期 */
        }
    }

    /* 2026-08-11：HTTP Date 头兜底对时——NTP UDP123 不可达时的备胎。
     * 天气链路已验证可用，Date 头即服务器 UTC 时间（显示层 +8 一致），
     * 与 NTP 双保险。settimeofday 设 UTC，clock_update_cb 手动 +8。
     * 2026-09-10 P211：受日期与时间页自动总闸控制（关则只取天气不对时）。 */
    if (server_time > 0 && dm_time_auto_enabled())
        {
            struct timeval tv;
            tv.tv_sec  = server_time;
            tv.tv_usec = 0;
            if (settimeofday(&tv, NULL) == 0)
                {
                    dm_time_note_synced(server_time);
                    printf("[time] sync via HTTP Date OK\n");
                }
        }

    /* Current weather — 2026-08-10 修复：必须从 "current": 段内解析。
     * 之前 strstr 全串找 "temperature_2m" 会命中 current_units 里的
     * 单位字符串（"°C"）→ atoi=0；本地实测全部提取为 0。 */
    const char *cur = strstr(buf, "\"current\":");
    if (cur != NULL)
        {
            json_int(cur, "\"temperature_2m\"", &w->cur_temp);
            json_int(cur, "\"relative_humidity_2m\"", &w->cur_humid);
            json_int(cur, "\"weather_code\"", &w->cur_code);
        }

    /* Daily forecast — 2026-08-10 修复：从 "daily": 段内解析。
     * 之前全串找 "weather_code" 会命中 daily_units 单位串且 '[' 落到
     * daily.time 数组上，导致 codes/tmax/tmin 全部错乱。 */
    const char *dl = strstr(buf, "\"daily\":");
    if (dl != NULL)
        {
            n = json_str_array(dl, "\"time\"", dates, DM_WEATHER_DAYS);
            if (n == 0)
                {
                    gettimeofday(&t1, NULL);
                    dbg_set("FAIL %ldms: parse (no time[] in JSON)",
                            (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                                   (t1.tv_usec - t0.tv_usec) / 1000));
                    return -1;   /* 2026-08-10：解析失败同样返回 -1 触发快速重试 */
                }

            json_int_array(dl, "\"weather_code\"", codes, DM_WEATHER_DAYS);
            json_int_array(dl, "\"temperature_2m_max\"", tmax, DM_WEATHER_DAYS);
            json_int_array(dl, "\"temperature_2m_min\"", tmin, DM_WEATHER_DAYS);
        }
    else
        {
            gettimeofday(&t1, NULL);
            /* 2026-09-10 P206：parse 失败带 body 前 64B（2026-09-10 板上
             * 连续"no daily in JSON"，光看原因猜不出是 API 变了还是包被截断） */
            {
                char head[65];
                strncpy(head, buf, 64);
                head[64] = '\0';
                dbg_set("FAIL %ldms: parse (no daily in JSON) body=%.64s",
                        (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                               (t1.tv_usec - t0.tv_usec) / 1000),
                        head);
            }
            return -1;
        }

    w->n_days = n;
    for (i = 0; i < n; i++)
        {
            strncpy(w->days[i].date, dates[i], sizeof(w->days[i].date) - 1);
            w->days[i].date[sizeof(w->days[i].date) - 1] = '\0';
            w->days[i].code  = codes[i];
            w->days[i].tmax  = tmax[i];
            w->days[i].tmin  = tmin[i];
            w->days[i].wday  = weekday_from_date(dates[i]);
        }

    gettimeofday(&t1, NULL);
    dbg_set("OK %ldms: %s, %d days, %dC/%d%%",
            (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                   (t1.tv_usec - t0.tv_usec) / 1000),
            dm_city_cur()->name, w->n_days, w->cur_temp, w->cur_humid);
    weather_state_save(w);   /* P133：落盘共享给 ai_agent 读 */
    return 0;
}

/* 2026-08-10：返回最近一次 fetch 的 debug 字符串（worker 线程写） */
const char *dm_weather_debug(void)
{
    return g_dbg;
}

const char *dm_wmo_text(int code)
{
    switch (code)
        {
        case 0:  return "晴";
        case 1:  return "晴间多云";
        case 2:  return "多云";
        case 3:  return "阴";
        case 45:
        case 48: return "雾";
        case 51:
        case 53:
        case 55: return "毛毛雨";
        case 61:
        case 63:
        case 65: return "雨";
        case 66:
        case 67: return "冻雨";
        case 71:
        case 73:
        case 75: return "雪";
        case 77: return "米雪";
        case 80:
        case 81:
        case 82: return "阵雨";
        case 85:
        case 86: return "阵雪";
        case 95: return "雷阵雨";
        case 96:
        case 99: return "冰雹";
        default: return "—";
        }
}
