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

#include "voice/voice_channel.h"
#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
#include <compexp.h>
#include <echo_canceller.h>
#include <heap_api.h>
#endif
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/volc_asr.h"
#include "voice/volc_tts.h"
#include "voice/xiaomi_asr.h"
#include "voice/xiaomi_tts.h"
#include "llm/llm_proxy.h"      /* 2026-08-12 TASK-04：voice_test_full 用 llm_chat */
#include "channels/ws_server.h" /* 2026-08-15 P84：stt 文本广播给 UI 显示 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "voice";

/* P362：定义在 speak 之后，收尾广播前要等它 */
int voice_channel_is_speaking(void);

/* P341：ASR 完成时刻。批量识别路径（MiMo）也要更新，否则
 * [voice] LLM latency 会沿用上一轮起点而失真。 */
static struct timespec s_asr_done_ts;
static int s_backends_registered;

/* ── Audio preprocessing (NS + AGC via BES ec2float) ────── */

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS

#define PREPROC_HEAP_SIZE (150 * 1024)
#define PREPROC_FRAME_MS 15
#define PREPROC_FRAME_SIZE \
    (AGENT_VOICE_SAMPLE_RATE / 1000 * PREPROC_FRAME_MS)

static const Ec2FloatConfig s_ns_cfg = {
    .bypass = 0,
    .hpf_enabled = 1,
    .af_enabled = 0, /* no AEC */
    .adprop_enabled = 0,
    .varistep_enabled = 0,
    .nlp_enabled = 0, /* no NLP */
    .clip_enabled = 0,
    .stsupp_enabled = 0,
    .hfsupp_enabled = 0,
    .constrain_enabled = 0,
    .ns_enabled = 1, /* noise suppression ON */
    .cng_enabled = 0,
    .blocks = 1,
    .delay = 0,
    .gamma = 0.9f,
    .echo_band_start = 300,
    .echo_band_end = 1800,
    .min_ovrd = 2,
    .target_supp = -40,
    .highfre_band_start = 4000,
    .highfre_supp = 8.f,
    .noise_supp = -15,
    .cng_type = 0,
    .cng_level = -60,
    .clip_threshold = -20.f,
    .banks = 64,
};

static const CompexpConfig s_agc_cfg = {
    .bypass = 0,
    .type = 0,
    .comp_threshold = -20.f,
    .comp_ratio = 2.f,
    .expand_threshold = -45.f,
    .expand_ratio = 0.5556f,
    .attack_time = 0.001f,
    .release_time = 0.006f,
    .makeup_gain = 6,
    .delay = 32,
    .tav = 0.2f,
};

typedef struct {
    void* heap;
    Ec2FloatState* ns;
    CompexpState* agc;
} audio_preproc_t;

static audio_preproc_t* preproc_create(void)
{
    audio_preproc_t* pp = calloc(1, sizeof(*pp));
    if (!pp) {
        return NULL;
    }

    pp->heap = malloc(PREPROC_HEAP_SIZE);
    if (!pp->heap) {
        free(pp);
        return NULL;
    }
    med_heap_init(pp->heap, PREPROC_HEAP_SIZE);

    pp->ns = ec2float_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, 0, &s_ns_cfg);
    pp->agc = compexp_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, &s_agc_cfg);

    syslog(LOG_DEBUG, "[%s] preproc: NS=%p AGC=%p heap=%dKB\n",
        TAG, (void*)pp->ns, (void*)pp->agc,
        PREPROC_HEAP_SIZE / 1024);
    return pp;
}

static void preproc_destroy(audio_preproc_t* pp)
{
    if (!pp) {
        return;
    }
    if (pp->agc) {
        compexp_destroy(pp->agc);
    }
    if (pp->ns) {
        ec2float_destroy(pp->ns);
    }
    free(pp->heap);
    free(pp);
}

static void preproc_chunk(audio_preproc_t* pp,
    unsigned char* buf, int bytes)
{
    if (!pp || (!pp->ns && !pp->agc)) {
        return;
    }

    int16_t* samples = (int16_t*)buf;
    int total = bytes / 2;
    int fsz = PREPROC_FRAME_SIZE;
    int32_t in32[PREPROC_FRAME_SIZE];
    int32_t ref32[PREPROC_FRAME_SIZE];
    int32_t out32[PREPROC_FRAME_SIZE];

    memset(ref32, 0, sizeof(ref32));

    for (int off = 0; off < total; off += fsz) {
        int n = (off + fsz <= total) ? fsz : (total - off);

        for (int i = 0; i < n; i++) {
            in32[i] = samples[off + i];
        }

        if (pp->ns) {
            ec2float_process(pp->ns, in32, ref32, n, out32);
        } else {
            memcpy(out32, in32, n * sizeof(int32_t));
        }

        if (pp->agc) {
            for (int i = 0; i < n; i++) {
                out32[i] <<= 8;
            }
            compexp_process_int24(pp->agc, out32, n);
            for (int i = 0; i < n; i++) {
                out32[i] >>= 8;
            }
        }

        for (int i = 0; i < n; i++) {
            int32_t v = out32[i];
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            samples[off + i] = (int16_t)v;
        }
    }
}

#endif /* CONFIG_AI_AGENT_AUDIO_PREPROCESS */

/* ── Recording state machine (PTT mode) ─────────────────── */

enum voice_state {
    VOICE_IDLE = 0,
    VOICE_RECORDING,
    VOICE_STOPPING
};

static struct {
    enum voice_state state;
    pthread_mutex_t lock;
    audio_capture_t* cap;
    unsigned char* pcm_buf; /* accumulated PCM data */
    size_t pcm_len; /* bytes recorded so far */
    size_t pcm_cap; /* buffer capacity */
    pthread_t rec_thread;
    voice_asr_stream_t* asr_stream; /* streaming ASR handle */
    sem_t rec_ready; /* posted by recording_thread when ready to consume */
    volatile int tts_abort; /* set to 1 to stop active TTS playback */
    audio_playback_t* tts_pb; /* active TTS playback handle (or NULL) */
    pthread_mutex_t speak_lock; /* serialize concurrent speak calls */
} s_voice = {
    .state = VOICE_IDLE,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .speak_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* P362：已经开始的播报序号（拿到 speak_lock 即 +1）。分句播报时调用方
 * 用它确认异步发起的那一段真的开播了，避免收尾段抢在前面。 */
static volatile unsigned s_speak_seq;

/* ── File-based helpers (kept for QEMU test commands) ──── */

static int read_pcm_file(const char* path,
    unsigned char** out, size_t* out_len)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot open %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    off_t fsize = lseek(fd, 0, SEEK_END);

    if (fsize <= 0 || (size_t)fsize > AGENT_VOICE_PCM_BUF_SIZE) {
        syslog(LOG_ERR, "[%s] File too large or empty: %ld\n",
            TAG, (long)fsize);
        close(fd);
        return -EFBIG;
    }

    lseek(fd, 0, SEEK_SET);

    unsigned char* buf = malloc((size_t)fsize);

    if (!buf) {
        close(fd);
        return -ENOMEM;
    }

    ssize_t nread = read(fd, buf, (size_t)fsize);

    close(fd);

    if (nread != (ssize_t)fsize) {
        free(buf);
        return -EIO;
    }

    /* 2026-08-12 TASK-02：WAV 头剥离——arecord 落盘是 WAV（RIFF/WAVE +
     * 44B 头），原实现整文件当 PCM 发给 ASR，头垃圾会污染首包。检测
     * WAV 魔数后解析 chunk 链，只保留 data 段（PCM 数据）。 */
    if ((size_t)fsize >= 12
        && memcmp(buf, "RIFF", 4) == 0
        && memcmp(buf + 8, "WAVE", 4) == 0) {
        size_t off = 12;

        while (off + 8 <= (size_t)fsize) {
            uint32_t clen = (uint32_t)buf[off + 4]
                | ((uint32_t)buf[off + 5] << 8)
                | ((uint32_t)buf[off + 6] << 16)
                | ((uint32_t)buf[off + 7] << 24);

            if (memcmp(buf + off, "data", 4) == 0) {
                size_t data_len = clen;

                if (off + 8 + data_len > (size_t)fsize) {
                    data_len = (size_t)fsize - off - 8;
                }
                memmove(buf, buf + off + 8, data_len);
                *out = buf;
                *out_len = data_len;
                return 0;
            }
            off += 8 + clen + (clen & 1);   /* chunk 2 字节对齐 */
        }
    }

    *out = buf;
    *out_len = (size_t)fsize;
    return 0;
}

static int write_pcm_file(const char* path,
    const unsigned char* data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] Cannot create %s: %d\n",
            TAG, path, errno);
        return -errno;
    }

    ssize_t nw = write(fd, data, len);

    close(fd);
    return (nw == (ssize_t)len) ? 0 : -EIO;
}

/* ── Recording cleanup helper ────────────────────────────── */

static void recording_cleanup_locked(voice_asr_stream_t* stream)
{
    /* Abort streaming ASR if active */
    if (stream) {
        voice_asr_stream_abort(stream);
        s_voice.asr_stream = NULL;
    }

    /* Close capture device */
    if (s_voice.cap) {
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
    }

    /* Free PCM buffer */
    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;

    /* Reset state to IDLE */
    s_voice.state = VOICE_IDLE;
}

/* Allocate fallback PCM buffer (must hold lock) */
static int alloc_fallback_buffer_locked(void)
{
    if (s_voice.pcm_buf) {
        return 0;
    }

    s_voice.pcm_buf = malloc(s_voice.pcm_cap);
    if (!s_voice.pcm_buf) {
        syslog(LOG_ERR, "[%s] fallback buffer alloc failed\n", TAG);
        return -ENOMEM;
    }

    s_voice.pcm_len = 0;
    return 0;
}

/* Accumulate PCM chunk to fallback buffer (must hold lock) */
static void accumulate_pcm_locked(const unsigned char* chunk, size_t len)
{
    if (s_voice.pcm_buf && s_voice.pcm_len + len <= s_voice.pcm_cap) {
        memcpy(s_voice.pcm_buf + s_voice.pcm_len, chunk, len);
        s_voice.pcm_len += len;
    }
}

/* Process one audio chunk: send to ASR or accumulate for fallback */
static int process_audio_chunk(voice_asr_stream_t** stream_ptr,
    const unsigned char* chunk, size_t len, int* need_fallback,
    size_t* bytes_sent)
{
    if (*need_fallback) {
        pthread_mutex_lock(&s_voice.lock);
        accumulate_pcm_locked(chunk, len);
        pthread_mutex_unlock(&s_voice.lock);
    }

    if (*stream_ptr) {
        int ret = voice_asr_stream_send(*stream_ptr, chunk, len);

        if (ret != 0) {
            syslog(LOG_ERR,
                "[%s] stream send failed: %d, batch fallback\n", TAG, ret);
            voice_asr_stream_abort(*stream_ptr);
            *stream_ptr = NULL;
            *need_fallback = 1;

            pthread_mutex_lock(&s_voice.lock);
            alloc_fallback_buffer_locked();
            pthread_mutex_unlock(&s_voice.lock);
        } else {
            *bytes_sent += len;
        }
    }

    return 0;
}

/* ── Recording thread (PTT mode — streaming ASR) ─────────── */

static void* recording_thread(void* arg)
{
    (void)arg;
    unsigned char chunk[AGENT_ASR_CHUNK_SIZE];
    int need_fallback = 0;
    // int dump_fd = -1;
    size_t total_bytes_read = 0;
    size_t total_bytes_sent = 0;
    int chunk_count = 0;
    int abnormal_exit = 0;
    int startup_retries = 0;  /* 2026-08-12：capture 启动竞态重试计数 */

    syslog(LOG_INFO, "[%s] recording thread started (streaming)\n", TAG);

    /* Use pre-connected ASR stream if available (opened in
     * voice_channel_start to avoid TLS handshake delay during
     * recording).  Fall back to opening a new session if not. */
    pthread_mutex_lock(&s_voice.lock);
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL; /* thread now owns it */
    pthread_mutex_unlock(&s_voice.lock);

    if (!stream) {
        /* No pre-connected stream — open now (may lose first 300-800ms) */
        stream = voice_asr_stream_open();

        if (!stream) {
            /* 2026-08-12：仅在确实失败时进 batch 兜底（原无条件打印误导） */
            syslog(LOG_WARNING,
                "[%s] streaming ASR unavailable, batch fallback\n", TAG);
            need_fallback = 1;

            pthread_mutex_lock(&s_voice.lock);
            if (alloc_fallback_buffer_locked() != 0) {
                s_voice.state = VOICE_IDLE;
                pthread_mutex_unlock(&s_voice.lock);
                sem_post(&s_voice.rec_ready);
                return NULL;
            }
            pthread_mutex_unlock(&s_voice.lock);
        }
    } else {
        syslog(LOG_INFO, "[%s] using pre-connected ASR stream\n", TAG);
    }

    /* Signal voice_channel_start() that we are ready to consume
     * audio frames. Without this, sem_wait in start() deadlocks. */
    sem_post(&s_voice.rec_ready);

    /* No flush needed: the new start sequence guarantees that
     * audio_capture_start() runs AFTER this thread is ready,
     * so there is no stale data in the queue. The old flush
     * was harmful — audio_capture_read() blocks, so it would
     * consume real audio frames instead of stale ones. */

    while (1) {
        pthread_mutex_lock(&s_voice.lock);
        enum voice_state st = s_voice.state;
        pthread_mutex_unlock(&s_voice.lock);

        if (st != VOICE_RECORDING) {
            break;
        }

        int n = audio_capture_read(s_voice.cap,
            chunk, sizeof(chunk));

        if (n <= 0) {
            /* 2026-08-12：启动竞态——recording thread 先于
             * audio_capture_start(prepare) 进入读循环时 readi 可能
             * 报错；累计 0 字节期间短重试 ≤1s，避免 batch 兜底丢 PCM */
            if (total_bytes_read == 0 && startup_retries < 100) {
                startup_retries++;
                usleep(10 * 1000);
                continue;
            }

            syslog(LOG_WARNING,
                "[%s] capture read returned %d, stopping\n", TAG, n);
            abnormal_exit = 1;
            break;
        }

        total_bytes_read += (size_t)n;
        chunk_count++;

        /* Dump raw PCM to file */
        // if (dump_fd >= 0) {
        //     write(dump_fd, chunk, (size_t)n);
        // }

        /* Log audio level for first few chunks and periodically
         * to diagnose silence vs actual speech from QEMU mic.
         * 2026-08-15 P77 诊断：前 3→10 + 每 50→10，覆盖 36 chunks 全分布 */
        if (chunk_count <= 10 || chunk_count % 10 == 0) {
            int16_t* samples = (int16_t*)chunk;
            int sample_count = n / 2;
            int32_t peak = 0;

            for (int i = 0; i < sample_count; i++) {
                int32_t v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak) {
                    peak = v;
                }
            }
            syslog(LOG_INFO,
                "[%s] chunk#%d: %d bytes, peak=%ld\n",
                TAG, chunk_count, n, (long)peak);
        }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
        preproc_chunk(pp, chunk, n);
#endif

        process_audio_chunk(&stream, chunk, (size_t)n,
            &need_fallback, &total_bytes_sent);
    }

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
    preproc_destroy(pp);
#endif

    /* Store stream handle for voice_channel_stop to finish */
    // if (dump_fd >= 0) {
    //     close(dump_fd);
    //     syslog(LOG_INFO, "[%s] PCM dump saved to /data/cap_dump.pcm\n", TAG);
    // }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.asr_stream = stream;

    if (abnormal_exit && s_voice.state == VOICE_RECORDING) {
        syslog(LOG_WARNING, "[%s] abnormal exit, cleaning up\n", TAG);
        recording_cleanup_locked(stream);
    }
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO,
        "[%s] recording thread exit: %d chunks, %zu read, %zu sent%s\n",
        TAG, chunk_count, total_bytes_read, total_bytes_sent,
        abnormal_exit ? " (abnormal)" : "");
    return NULL;
}

/* ── ASR worker thread ──────────────────────────────────── */

#define ASR_THREAD_STACK (24 * 1024)

/* ══════════════════════════════════════════════════════════════
 * 本地设备控制命令（2026-08-23 P115/P116 表驱动重构）
 *
 * 结构：每个命令一个匹配器（ASR 文本命中 → 填 cmd_out 返回 1），
 * 注册进 g_local_cmd_matchers[] 数组；主循环顺序遍历。
 * 新增命令 = 写一个匹配器 + 数组加一行，无需改主循环。
 *
 * cmd 协议：music:play|pause|next|prev / volume:±N(相对)|N(绝对)
 *           / brightness:±N(相对)|N(绝对)
 * ══════════════════════════════════════════════════════════════ */

/* 中文数字 0-9（单字 3 字节 UTF-8），匹配返回 0-9，否则 -1 */
static int cn_digit(const char *s)
{
    static const char *t[] = { "零","一","二","三","四","五","六","七","八","九" };
    int i;
    if (!s || s[0] == '\0')
        return -1;
    for (i = 0; i < 10; i++)
        if (strncmp(s, t[i], 3) == 0)
            return i;
    return -1;
}

/* 解析中文数字 0-100（"五十"→50，"二十五"→25，"一百"→100），成功返回 0。
 * 2026-08-23 review 修复：所有 s+N 偏移前用 strlen 剩余长度防护，
 * 防止"音量调到五"（末字）时 strncmp(s+3/s+6) 越过 '\0' 越界读（UB）。 */
static int cn_num(const char *s, int *out)
{
    int d, d2;
    size_t left;

    if (!s || s[0] == '\0' || !out)
        return -1;
    left = strlen(s);
    if (left >= 6 && strncmp(s, "一百", 6) == 0)
        { *out = 100; return 0; }
    if (strncmp(s, "十", 3) == 0)
        {
            *out = 10;
            if (left >= 6 && (d2 = cn_digit(s + 3)) > 0)
                *out += d2;
            return 0;
        }
    d = cn_digit(s);
    if (d < 0)
        return -1;
    if (left >= 6 && strncmp(s + 3, "十", 3) == 0)
        {
            *out = d * 10;
            if (left >= 9 && (d2 = cn_digit(s + 6)) > 0)
                *out += d2;
            return 0;
        }
    *out = d;
    return 0;
}

/* ── 音量/亮度共用解析：设备词已确认后，解析绝对或相对步进 ── */
static int dev_adjust_parse(const char *text, const char *dev,
                            char *cmd_out, size_t cap)
{
    /* 绝对设置："调到五十/调到50" / "百分之五十/百分之50" / "50%" */
    int absval = -1;
    {
        const char *pct = strstr(text, "百分之");
        const char *to  = strstr(text, "调到");
        if (!to) to = strstr(text, "调成");
        if (!to) to = strstr(text, "调为");
        const char *num = NULL;
        if (pct)
            num = pct + 9;                /* "百分之" = 3 汉字 = 9 字节 */
        else if (to)
            num = to + 6;                 /* "调到" = 2 汉字 = 6 字节 */
        else
            {
                const char *pc = strchr(text, '%');
                if (pc && pc > text)
                    {
                        const char *q = pc - 1;
                        while (q >= text && *q >= '0' && *q <= '9')
                            q--;
                        num = q + 1;
                    }
            }
        if (num)
            {
                if (*num >= '0' && *num <= '9')
                    {
                        int v = atoi(num);
                        if (v >= 0 && v <= 100)
                            absval = v;
                    }
                else if (cn_num(num, &absval) != 0 || absval > 100)
                    absval = -1;
            }
    }
    if (absval >= 0)
        { snprintf(cmd_out, cap, "%s:%d", dev, absval); return 1; }

    /* 相对步进 ±5——方向先查负向再正向，
     * 关键："亮度调暗" 同时含"亮"（亮度自身）与"暗"，负向优先。 */
    int dir = 0;
    if (strstr(text, "小") || strstr(text, "低") || strstr(text, "暗") ||
        strstr(text, "减") || strstr(text, "降"))
        dir = -1;
    else if (strstr(text, "大") || strstr(text, "高") || strstr(text, "亮") ||
             strstr(text, "加") || strstr(text, "增"))
        dir = 1;
    if (dir == 0)
        return 0;

    snprintf(cmd_out, cap, "%s:%+d", dev, dir * 5);
    return 1;
}

/* ── 命令匹配器（每个命令一个；命中填 cmd_out 返回 1）── */

static int match_music_next(const char *text, char *cmd_out, size_t cap)
{
    if (strstr(text, "下一曲") || strstr(text, "下一首") ||
        strstr(text, "切歌") || strstr(text, "换一首") ||
        strstr(text, "换歌") || strstr(text, "下首歌") ||
        strstr(text, "切下一首") || strstr(text, "换一个") ||
        strstr(text, "不想听这个"))
        { snprintf(cmd_out, cap, "music:next"); return 1; }
    return 0;
}

static int match_music_prev(const char *text, char *cmd_out, size_t cap)
{
    if (strstr(text, "上一曲") || strstr(text, "上一首") ||
        strstr(text, "上首歌") || strstr(text, "回到上一首") ||
        strstr(text, "再听一遍") || strstr(text, "重放"))
        { snprintf(cmd_out, cap, "music:prev"); return 1; }
    return 0;
}

static int match_music_pause(const char *text, char *cmd_out, size_t cap)
{
    /* 2026-11-01 P334：移除"安静"——它归 match_comfort（"安静点"=调低音量）。
     * 音乐暂停只留明确的暂停语义。 */
    if (strstr(text, "暂停") || strstr(text, "停一下") ||
        strstr(text, "别放") || strstr(text, "别唱") ||
        strstr(text, "关掉音乐") || strstr(text, "不要放了") ||
        strstr(text, "关音乐"))
        { snprintf(cmd_out, cap, "music:pause"); return 1; }
    return 0;
}

static int match_music_play(const char *text, char *cmd_out, size_t cap)
{
    if (strstr(text, "播放") || strstr(text, "继续") ||
        strstr(text, "放音乐") || strstr(text, "来一首") ||
        strstr(text, "唱一首") || strstr(text, "来点音乐") ||
        strstr(text, "放首歌") || strstr(text, "放点歌") ||
        strstr(text, "听歌") || strstr(text, "接着放") ||
        strstr(text, "开始放"))
        { snprintf(cmd_out, cap, "music:play"); return 1; }
    return 0;
}

static int match_volume(const char *text, char *cmd_out, size_t cap)
{
    if (!strstr(text, "音量") && !strstr(text, "声音"))
        return 0;
    return dev_adjust_parse(text, "volume", cmd_out, cap);
}

static int match_brightness(const char *text, char *cmd_out, size_t cap)
{
    if (!strstr(text, "亮度") && !strstr(text, "背光"))
        return 0;
    return dev_adjust_parse(text, "brightness", cmd_out, cap);
}

/* ── 口语化舒适度（2026-11-01 P332）─────────────────────────
 * "太吵了 / 小声点 / 大声点 / 刺眼 / 太暗了 / 看不见"——这些句子里没有
 * "音量/亮度"字样，原来的 matcher 一律漏掉。这里按**明确极性**映射，
 * 不走 dev_adjust_parse 的方向词推断（"太亮了"含"亮"会被判成调高，反了）。
 * 步进 ±10，比"音量调一点"的 ±5 更符合口语诉求。
 * 表内顺序=优先级：长词/更具体的放前面（"太大声"先于"声音大"）。 */
static int match_comfort(const char *text, char *cmd_out, size_t cap)
{
    static const struct { const char *w; const char *cmd; } t[] = {
        /* 音量：负向 */
        { "太大声",   "volume:-10" },
        { "太吵",     "volume:-10" },
        { "吵死",     "volume:-10" },
        { "小点声",   "volume:-10" },
        { "小声点",   "volume:-10" },
        { "声音小",   "volume:-10" },
        { "轻一点",   "volume:-10" },
        { "安静点",   "volume:-10" },
        /* 音量：正向 */
        { "大声点",   "volume:+10" },
        { "大点声",   "volume:+10" },
        { "声音大",   "volume:+10" },
        { "听不清",   "volume:+10" },
        { "听不见",   "volume:+10" },
        /* 音量：极值 */
        { "静音",     "volume:0" },
        { "关掉声音", "volume:0" },
        { "别出声",   "volume:0" },
        { "最大声",   "volume:100" },
        { "声音开到最大", "volume:100" },
        /* 亮度：负向 */
        { "太亮",     "brightness:-10" },
        { "刺眼",     "brightness:-10" },
        { "晃眼",     "brightness:-10" },
        { "暗一点",   "brightness:-10" },
        { "调暗",     "brightness:-10" },
        /* 亮度：正向 */
        { "太暗",     "brightness:+10" },
        { "看不见",   "brightness:+10" },
        { "亮一点",   "brightness:+10" },
        { "亮一些",   "brightness:+10" },
        { "调亮",     "brightness:+10" },
    };
    size_t i;

    if (strlen(text) > 36)              /* ≤12 汉字，防长句误触 */
        return 0;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        {
            if (strstr(text, t[i].w))
                {
                    strlcpy(cmd_out, t[i].cmd, cap);
                    return 1;
                }
        }
    return 0;
}

/* ── 闹钟（2026-09-10 P206）："明早7点叫我/定个闹钟7点半/下午3点提醒我"
 * 中文时间解析：[明天|后天] [上午|中午|下午|晚上|凌晨|夜里|早上] X点[半|Y分]。
 * 数字支持阿拉伯 + 中文（复用 cn_num，0..23 点 / 0..59 分）。
 * cmd 格式 "alarm:HH:MM:R"（R=0 一次，1 每天）：说明天/后天→每天；
 * 今天且已过→每天；今天未过→一次。无"点"字或数字非法→不命中走 LLM。 */
/* 时分必须是完整数字，拒绝“几/二十几/7abc”等不确定识别结果。 */
static int alarm_number(const char *s, int *out)
{
    size_t n = strlen(s);
    if (n && n <= 2 && s[0] >= '0' && s[0] <= '9' &&
        (n == 1 || (s[1] >= '0' && s[1] <= '9'))) {
        *out = atoi(s); return 0;
    }
    if (!strcmp(s, "两")) { *out = 2; return 0; }
    if ((n == 3 && (cn_digit(s) >= 0 || !strcmp(s, "十"))) ||
        (n == 6 && ((!strncmp(s, "十", 3) && cn_digit(s+3) > 0) ||
                    (cn_digit(s) > 0 && !strcmp(s+3, "十")))) ||
        (n == 9 && cn_digit(s) > 0 && !strncmp(s+3, "十", 3) &&
                    cn_digit(s+6) > 0))
        return cn_num(s, out);
    return -1;
}

static int alarm_num_before(const char *text, const char *dot, int *out)
{
    /* dot 指"点"字首字节；向前取最长数字串（阿拉伯/中文混排） */
    const char *s = dot;
    char tmp[32];
    int len = 0;

    while (s > text && len < (int)sizeof(tmp) - 4) {
        /* 阿拉伯数字（单字节倒查） */
        if (s[-1] >= '0' && s[-1] <= '9') {
            s--;
            len++;
            continue;
        }
        /* 中文数字字（3 字节倒查：零..九/十/百/两） */
        if (s - text >= 3 &&
            (cn_digit(s - 3) >= 0 || strncmp(s - 3, "十", 3) == 0 ||
             strncmp(s - 3, "百", 3) == 0 || strncmp(s - 3, "两", 3) == 0)) {
            s -= 3;
            len += 3;
            continue;
        }
        break;
    }
    if (len <= 0 || len >= (int)sizeof(tmp))
        return -1;
    memcpy(tmp, s, (size_t)len);
    tmp[len] = '\0';
    /* "两"作"二"（两点/两分） */
    if (strncmp(tmp, "两", 3) == 0)
        memcpy(tmp, "二", 3);
    return alarm_number(tmp, out);
}

static int match_alarm(const char *text, char *cmd_out, size_t cap)
{
    const char *dot;
    int hh = -1, mm = 0, rp = 0;

    if (!strstr(text, "闹钟") && !strstr(text, "叫我") && !strstr(text, "叫醒") &&
        !strstr(text, "提醒我") && !strstr(text, "起床"))
        return 0;
    dot = strstr(text, "点");
    if (!dot)
        return 0;
    if (alarm_num_before(text, dot, &hh) != 0 || hh < 0 || hh > 23)
        return 0;
    /* 分：点后"半"=30 / "Y分"=Y */
    {
        const char *after = dot + 3;
        if (strncmp(after, "半", 3) == 0) {
            mm = 30;
        } else {
            const char *fen = strstr(after, "分");
            if (fen && fen - after < 12) {
                char t2[16];
                size_t l = (size_t)(fen - after);
                if (l >= sizeof(t2))
                    return 0;
                memcpy(t2, after, l);
                t2[l] = '\0';
                if (t2[0] >= '0' && t2[0] <= '9')
                    mm = atoi(t2);
                else if (cn_num(t2, &mm) != 0)
                    return 0;
                if (mm < 0 || mm > 59)
                    return 0;
            }
        }
    }
    /* 上下午：下午/晚上/夜里/凌晨 1..11 点 +12（12 点不动） */
    if ((strstr(text, "下午") || strstr(text, "晚上") || strstr(text, "夜里") ||
         strstr(text, "傍晚")) && hh >= 1 && hh <= 11)
        hh += 12;
    /* 每天：说明天/后天，或今天已过 */
    if (strstr(text, "明天") || strstr(text, "后天")) {
        rp = 1;
    } else {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        if (lt && (hh < lt->tm_hour ||
                   (hh == lt->tm_hour && mm <= lt->tm_min)))
            rp = 1;
    }
    snprintf(cmd_out, cap, "alarm:%02d:%02d:%d", hh, mm, rp);
    return 1;
}

/* ── 闹钟管理 / 场景 / 健康打卡（2026-11-01 P333）────────────
 * 都放在 match_alarm 之前：否则"删掉七点的闹钟"会被当成"新增七点闹钟"。 */
static int match_alarm_ctl(const char *text, char *cmd_out, size_t cap)
{
    const char *dot;
    int hh = -1, mm = 0;

    /* 贪睡本身就是完整指令，不要求再说“闹钟”。 */
    if (strstr(text, "再睡五分钟") || strstr(text, "再睡5分钟") ||
        strstr(text, "贪睡"))
        { strlcpy(cmd_out, "alarm:snooze", cap); return 1; }

    if (!strstr(text, "闹钟") && !strstr(text, "提醒"))
        return 0;

    if (strstr(text, "删") || strstr(text, "取消") || strstr(text, "去掉"))
        {
            /* 时间缺失/识别失败不能升级成清空；明确全量词才允许全删。 */
            if (strstr(text, "全部") || strstr(text, "所有") ||
                strstr(text, "全删") || strstr(text, "都删") ||
                strstr(text, "全取消") || strstr(text, "都取消"))
                { strlcpy(cmd_out, "alarm:delall", cap); return 1; }
            strlcpy(cmd_out, "alarm:invalid", cap);
            dot = strstr(text, "点");
            if (dot && alarm_num_before(text, dot, &hh) == 0 && hh >= 0 && hh <= 23)
                {
                    const char *after = dot + 3;
                    if (strncmp(after, "半", 3) == 0)
                        mm = 30;
                    else
                        {
                            const char *fen = strstr(after, "分");
                            if (fen)
                                {
                                    char t2[16];
                                    size_t l = (size_t)(fen - after);
                                    if (l > 0 && l < sizeof(t2))
                                        {
                                            memcpy(t2, after, l);
                                            t2[l] = '\0';
                                            if (alarm_number(t2, &mm) != 0)
                                                return 1;
                                        }
                                    else return 1;
                                }
                            else if (*after && strncmp(after, "的闹钟", 9) &&
                                     strncmp(after, "闹钟", 6) &&
                                     strncmp(after, "的提醒", 9) &&
                                     strncmp(after, "提醒", 6))
                                return 1;
                        }
                    if (mm < 0 || mm > 59) return 1;
                    if ((strstr(text, "下午") || strstr(text, "晚上") ||
                         strstr(text, "夜里") || strstr(text, "傍晚")) &&
                        hh >= 1 && hh <= 11)
                        hh += 12;
                    snprintf(cmd_out, cap, "alarm:del:%02d:%02d", hh, mm);
                    return 1;
                }
            return 1;
        }

    if (strstr(text, "关") || strstr(text, "停") || strstr(text, "别响"))
        { strlcpy(cmd_out, "alarm:off", cap); return 1; }

    if (strstr(text, "哪些") || strstr(text, "几个") || strstr(text, "列出") ||
        strstr(text, "有没有闹钟"))
        { strlcpy(cmd_out, "alarm:list", cap); return 1; }

    if (strstr(text, "再睡") || strstr(text, "贪睡") || strstr(text, "再响") ||
        strstr(text, "五分钟后再"))
        { strlcpy(cmd_out, "alarm:snooze", cap); return 1; }

    return 0;
}

/* 场景："我要睡觉了"→关音乐+调暗+猫睡；"我出门了"→关音乐+调亮；
 * "我回来了"→亮度回中。注意和宠物命令的边界：只有带"我要/我去/准备/
 * 晚安"才当场景，"睡觉"两个字单独说是让猫睡（match_pet）。 */
static int match_scene(const char *text, char *cmd_out, size_t cap)
{
    if (strstr(text, "晚安") ||
        ((strstr(text, "我要") || strstr(text, "我去") ||
          strstr(text, "准备") || strstr(text, "该")) &&
         (strstr(text, "睡觉") || strstr(text, "睡了") || strstr(text, "休息"))))
        { strlcpy(cmd_out, "scene:sleep", cap); return 1; }

    if (strstr(text, "出门") || strstr(text, "我走了") || strstr(text, "我出去"))
        { strlcpy(cmd_out, "scene:out", cap); return 1; }

    if (strstr(text, "我回来") || strstr(text, "到家") || strstr(text, "回来了"))
        { strlcpy(cmd_out, "scene:home", cap); return 1; }

    return 0;
}

/* 健康打卡：不是查询，是记录动作（会推动进度环 + 猫的反应） */
static int match_health(const char *text, char *cmd_out, size_t cap)
{
    /* 只记录已完成的动作，不能把计划、问题或否定句当打卡。 */
    if (strstr(text, "没") || strstr(text, "不") || strstr(text, "别") ||
        strstr(text, "吗") || strstr(text, "么") || strstr(text, "？") ||
        strchr(text, '?') || strstr(text, "提醒") || strstr(text, "准备") ||
        strstr(text, "我要") || strstr(text, "打算"))
        return 0;
    if (strstr(text, "喝水了") || strstr(text, "喝了水") || strstr(text, "喝了一杯"))
        { strlcpy(cmd_out, "health:water", cap); return 1; }

    if (strstr(text, "起来了") || strstr(text, "起身了") || strstr(text, "站了一会"))
        { strlcpy(cmd_out, "health:stand", cap); return 1; }

    if (strstr(text, "再等") || strstr(text, "等会") || strstr(text, "先不") ||
        strstr(text, "待会"))
        { strlcpy(cmd_out, "health:snooze", cap); return 1; }

    return 0;
}

/* ── 宠物动作（2026-11-01 P331）─────────────────────────────
 * "阿强 走路 / 坐下 / 睡觉 / 吃饭 / 停下"。名字不参与匹配——宠物名只有
 * UI 知道，且同音字（阿强/阿墙）会误伤，所以只认动作词。
 * 防误触发：整句 ≤12 个汉字才认，且动作词必须在句尾附近。 */
static int match_pet(const char *text, char *cmd_out, size_t cap)
{
    static const struct { const char *word; const char *cmd; } t[] = {
        { "散步",   "pet:walk" },
        { "走路",   "pet:walk" },
        { "走两步", "pet:walk" },
        { "走走",   "pet:walk" },
        { "坐下",   "pet:sit"  },
        { "坐着",   "pet:sit"  },
        { "睡觉",   "pet:sleep"},
        { "去睡",   "pet:sleep"},
        { "吃饭",   "pet:eat"  },
        { "吃东西", "pet:eat"  },
        /* P344：舔爪是宠物本来就有的成熟动作（pet_actions 里的 groom_paw），
         * 顺手接到语音上。 */
        { "舔毛",   "pet:groom"},
        { "舔舔",   "pet:groom"},
        { "梳毛",   "pet:groom"},
        { "停下",   "pet:stop" },
        { "回来",   "pet:stop" },
        { "别走了", "pet:stop" },
    };
    size_t i, len = strlen(text);
    int secs = 0;

    if (len == 0 || len > 36)               /* 36 字节 ≈ 12 个汉字 */
        return 0;

    /* P350：口语"走五分钟"里既没有"走路"也没有"散步"，词表一个都匹配不到，
     * 下面那段"分钟→秒"的解析就成了死代码。这里补一条：以"走"开头且带"分钟"
     * 才认，避免"走开""走啦"被当成散步。 */
    if (strncmp(text, "走", 3) == 0 && strstr(text, "分钟"))
        {
            const char *m = strstr(text, "分钟");
            const char *num = m;
            int n = 0;
            char buf[16];
            while (num > text && n < 3 && (unsigned char)*(num - 3) >= 0xE0)
                {
                    num -= 3;
                    n++;
                }
            if (n > 0)
                {
                    memcpy(buf, num, (size_t)n * 3);
                    buf[n * 3] = '\0';
                    if (cn_num(buf, &secs) == 0)
                        secs *= 60;
                }
            snprintf(cmd_out, cap, secs > 0 ? "pet:walk:%d" : "pet:walk", secs);
            return 1;
        }

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        {
            const char *p = strstr(text, t[i].word);
            size_t tail;
            if (!p)
                continue;
            /* 动作词后面不许再跟一长串（"我睡觉之前先刷牙"这类不算） */
            tail = strlen(p + strlen(t[i].word));
            if (tail > 9)                   /* ≤3 个汉字 */
                continue;

            if (strcmp(t[i].cmd, "pet:walk") == 0)
                {
                    /* "走五分钟" → 秒数；cn_num 认中文数字 */
                    const char *m = strstr(p, "分钟");
                    if (m)
                        {
                            /* 往前找数字："五分钟" 里 "五" 在 "分钟" 前 6 字节 */
                            char buf[16];
                            const char *num = m;
                            int n = 0;
                            while (num > text && n < 3 &&
                                   (unsigned char)*(num - 3) >= 0xE0)
                                {
                                    num -= 3;
                                    n++;
                                }
                            if (n > 0)
                                {
                                    memcpy(buf, num, (size_t)n * 3);
                                    buf[n * 3] = '\0';
                                    if (cn_num(buf, &secs) == 0)
                                        secs *= 60;
                                }
                        }
                    if (secs > 0)
                        snprintf(cmd_out, cap, "pet:walk:%d", secs);
                    else
                        strlcpy(cmd_out, "pet:walk", cap);
                }
            else
                {
                    strlcpy(cmd_out, t[i].cmd, cap);
                }
            return 1;
        }
    return 0;
}

/* ── 打开页面（2026-11-01 P332）─────────────────────────────
 * "打开书本 / 打开日历 / 打开串口调试"。只做导航，不碰页面内容。
 * 歧义词（音乐/闹钟/设置/文件）必须带开启动词才算，避免"我设了闹钟"
 * 这种叙述被当成命令；"书本/日历/串口"本身歧义小，直接命中。 */
static int match_app(const char *text, char *cmd_out, size_t cap)
{
    static const struct { const char *w; const char *cmd; int need_verb; } t[] = {
        { "宠物设置", "app:pet",      0 },
        { "书本",     "app:books",    0 },
        { "看书",     "app:books",    0 },
        { "读书",     "app:books",    0 },
        { "日历",     "app:calendar", 0 },
        { "串口",     "app:uart",     0 },
        { "调试工具", "app:uart",     0 },
        { "uart",     "app:uart",     0 },
        { "UART",     "app:uart",     0 },
        { "音乐",     "app:music",    1 },
        { "闹钟",     "app:alarm",    1 },
        { "设置",     "app:settings", 1 },
        { "文件",     "app:files",    1 },
        { "回桌面",   "app:home",     0 },
        { "退出页面", "app:home",     0 },
        { "关闭页面", "app:home",     0 },
    };
    int has_verb = strstr(text, "打开") || strstr(text, "进入") ||
                   strstr(text, "看看") || strstr(text, "开一下") ||
                   strncmp(text, "去", 3) == 0;
    size_t i;

    if (strlen(text) > 36)              /* ≤12 汉字 */
        return 0;

    /* 裸“关掉/退出”才回桌面，带对象的关闭交给对应执行器。 */
    if (!strcmp(text, "关掉") || !strcmp(text, "退出"))
        { strlcpy(cmd_out, "app:home", cap); return 1; }
    if (strstr(text, "去掉")) return 0;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        {
            if (!strstr(text, t[i].w))
                continue;
            if (t[i].need_verb && !has_verb)
                continue;
            strlcpy(cmd_out, t[i].cmd, cap);
            return 1;
        }
    return 0;
}

/* ── 命令表（顺序即优先级：具体词在前，如"播放下一曲"→next）── */
typedef int (*local_cmd_matcher_t)(const char *text, char *cmd_out, size_t cap);

static const local_cmd_matcher_t g_local_cmd_matchers[] = {
    match_app,
    match_music_next,
    match_music_prev,
    match_music_pause,
    match_music_play,
    /* P350：comfort 必须排在 volume/brightness 前面。两者都靠"音量/声音 + 方向词"
     * 推断，而 comfort 表里的"声音小""声音大""声音开到最大"整句都含"声音"，
     * 排在后面会被 dev_adjust_parse 抢走：说"声音开到最大"只 +5（期望 100），
     * 说"声音小"只 -5（期望 -10）。comfort 的词更具体，先给它。 */
    match_comfort,
    match_volume,
    match_brightness,
    match_alarm_ctl,
    match_scene,
    match_health,
    match_alarm,
    match_pet,
};

/* ASR 文本 → 本地命令；命中填 cmd_out（如 "music:play" / "volume:-5"）
 * 返回 1；否则返回 0 走正常 LLM 链路。 */
static int voice_local_cmd_parse(const char *text, char *cmd_out, size_t cap)
{
    size_t i;
    char normalized[512];

    if (!text || !text[0] || !cmd_out || cap < 16)
        return 0;
    /* ASR 常带尾部标点，完整短指令比较前先去掉首尾空白/标点。 */
    if (strlen(text) >= sizeof(normalized)) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\n') ++text;
    strlcpy(normalized, text, sizeof(normalized));
    size_t len = strlen(normalized);
    while (len) {
        if (strchr(" \t\r\n.!", normalized[len-1])) normalized[--len] = 0;
        else if (len >= 3 && (!strcmp(normalized+len-3, "。") ||
                             !strcmp(normalized+len-3, "！"))) {
            len -= 3; normalized[len] = 0;
        } else break;
    }
    text = normalized;
    /* 否定/询问不执行本地动作；保留明确的停止和感受类短语。 */
    if (strstr(text, "吗") || strstr(text, "么") || strstr(text, "？") ||
        strchr(text, '?')) return 0;
    if (strstr(text, "不") || strstr(text, "没") || strstr(text, "别")) {
        static const char *allowed[] = {
            "别放", "别放了", "别唱", "别唱了", "不要放了",
            "别出声", "不想听这个", "别走了", "听不清", "听不清了",
            "听不见", "听不见了", "看不见", "看不见了", "闹钟别响了"
        };
        char remainder[sizeof(normalized)];
        strlcpy(remainder, text, sizeof(remainder));
        for (i = 0; i < sizeof(allowed)/sizeof(allowed[0]); ++i) {
            char *word;
            while ((word = strstr(remainder, allowed[i])) != NULL)
                memmove(word, word + strlen(allowed[i]),
                        strlen(word + strlen(allowed[i])) + 1);
        }
        if (strstr(remainder, "不") || strstr(remainder, "没") ||
            strstr(remainder, "别")) return 0;
    }
    for (i = 0; i < sizeof(g_local_cmd_matchers) /
                     sizeof(g_local_cmd_matchers[0]); i++)
        {
            if (g_local_cmd_matchers[i](text, cmd_out, cap))
                return 1;
        }
    return 0;
}

/* Local execution and feedback belong to the UI. No cloud acknowledgement:
 * pet motion must not wait for TTS, and volume/alarm must not trigger it twice. */
static void voice_local_cmd_run(const char* cmd)
{
    s_asr_done_ts.tv_sec = 0;
    ws_server_broadcast_voice_evt("cmd", cmd);
    ws_server_broadcast_voice_evt("pet_idle", "");
}

/* P352：ASR 失败必须让用户看见，不能静默回 idle。
 *
 * 现场：WiFi 已关联 AP 但路由器没有上行（或 DNS 挂了）时，
 * dm_net_wifi_connected() 只判 wlan0 有没有 IPv4（dm_net.c:807），依然为真，
 * UI 的联网拦截放行 → 白录一遍 → ASR 请求失败 → 这里只发 pet_idle。
 * 用户看到的就是"猫思考了一下就没了，什么也没说"。
 *
 * ret<0  = 请求没打通（断网/服务不可用）；
 * ret==0 = 通了但没识别出字（没说话/太吵）。
 * 两种都发一条 error 事件，UI 侧按"维拉说的一句话"显示。 */
static void voice_report_asr_failure(int ret)
{
    const char *msg = ret < 0 ? "网络连不上…" : "没听清…";

    /* 自己打一行，验证时不依赖 ws_server 的广播日志（实测那行从未出现过）。 */
    syslog(LOG_INFO, "[%s] ASR failure -> bubble: %s\n", TAG, msg);
    ws_server_broadcast_voice_evt("pet_idle", "");
    ws_server_broadcast_voice_evt("error", msg);
}

static void* asr_and_dispatch(void* arg)
{
    unsigned char* pcm = (unsigned char*)arg;
    size_t pcm_len;

    /* pcm_len is stored in the first sizeof(size_t) bytes */
    memcpy(&pcm_len, pcm, sizeof(size_t));
    const unsigned char* audio = pcm + sizeof(size_t);

    syslog(LOG_INFO, "[%s] ASR: recognizing %zu bytes\n",
        TAG, pcm_len);

    char text[512];
    voice_turn_mark("asr_start");
    int ret = voice_asr_recognize(audio, pcm_len,
        text, sizeof(text));
    voice_turn_mark("asr_done");

    if (ret == 0 && text[0] != '\0') {
        syslog(LOG_INFO, "[%s] ASR result: %s\n", TAG, text);
        printf("ASR: %s\n", text);

        /* P341：MiMo 走批量识别，此路径原先不更新 s_asr_done_ts，
         * 导致 [voice] LLM latency 沿用上一轮起点而失真。 */
        clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts);

        /* 2026-08-15 P84：ASR 文本广播给 UI 显示（小智式"说话显字"） */
        ws_server_broadcast_voice_evt("stt", text);

        /* 2026-08-23 P115：本地命令短路——命中音量/亮度关键词则广播
         * cmd 事件给 UI 执行硬件（g_music_volume / PWM 背光），
         * 不再 push LLM（省 token + 避免 LLM 瞎回"无法调整"）。 */
        {
            char cmd[64];
            if (voice_local_cmd_parse(text, cmd, sizeof(cmd))) {
                syslog(LOG_INFO, "[%s] local cmd: %s\n", TAG, cmd);
                voice_local_cmd_run(cmd);
                free(pcm);
                return NULL;
            }
        }

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        } else ws_server_broadcast_voice_evt("pet_idle", "");
    } else {
        syslog(LOG_WARNING, "[%s] ASR failed or empty: %d\n",
            TAG, ret);
        voice_report_asr_failure(ret);
    }

    free(pcm);
    return NULL;
}

/* ── 语音回合分段观测（2026-11-02 P341）────────────────────
 * 目的：把"语音慢"拆成可归因的段，而不是笼统的体感。
 * 时间轴：TURN begin(ptt_up) → asr_start → asr_done → llm_req
 *        → llm_done → tts_start → tts_first_chunk → tts_first_write
 *        → TURN end(speak_done)，并附本回合 xrun / 短写增量。
 * 全部相对"松手"时刻计时，宿主按 TURN 前缀即可还原一轮。
 * 只做观测，不改任何控制流；tts_abort / 失败路径也走 end 以便计数闭合。 */

static struct timespec s_turn_t0;
static volatile int s_turn_active;
static unsigned s_turn_xrun0;
static unsigned s_turn_short0;

static long voice_turn_elapsed_ms(void)
{
    if (!s_turn_active) {
        return -1;
    }

    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - s_turn_t0.tv_sec) * 1000
        + (now.tv_nsec - s_turn_t0.tv_nsec) / 1000000;
}

static void voice_turn_begin(const char* why)
{
    clock_gettime(CLOCK_MONOTONIC, &s_turn_t0);
    s_turn_xrun0 = audio_playback_xrun_count();
    s_turn_short0 = audio_playback_short_write_count();
    s_turn_active = 1;
    syslog(LOG_NOTICE, "[%s] TURN begin(%s) xrun0=%u short0=%u\n",
        TAG, why, s_turn_xrun0, s_turn_short0);
}

void voice_turn_mark(const char* tag)
{
    long ms = voice_turn_elapsed_ms();

    if (ms < 0) {
        return;
    }

    syslog(LOG_NOTICE, "[%s] TURN %s +%ldms\n", TAG, tag, ms);
}

static void voice_turn_end(const char* why)
{
    long ms = voice_turn_elapsed_ms();

    if (ms < 0) {
        return;
    }

    unsigned dx = audio_playback_xrun_count() - s_turn_xrun0;
    unsigned ds = audio_playback_short_write_count() - s_turn_short0;

    syslog(LOG_NOTICE, "[%s] TURN end(%s) +%ldms xrun=%u short=%u\n",
        TAG, why, ms, dx, ds);
    s_turn_active = 0;
}

/* ── TTS streaming callback ──────────────────────────────── */

static struct timespec s_tts_start;
static int s_tts_first_chunk;
static int s_pet_tts_started;

static void tts_stream_cb(const unsigned char* pcm_data,
    size_t pcm_len, int is_last, void* user_data)
{
    audio_playback_t* pb = (audio_playback_t*)user_data;

    /* Abort early if PTT recording started */
    if (s_voice.tts_abort) {
        return;
    }

    if (pcm_data && pcm_len > 0 && pb) {
        if (!s_tts_first_chunk) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - s_tts_start.tv_sec) * 1000
                + (now.tv_nsec - s_tts_start.tv_nsec) / 1000000;
            syslog(LOG_NOTICE, "[%s] TTS first chunk: %ldms\n",
                TAG, ms);
            s_tts_first_chunk = 1;
            voice_turn_mark("tts_first_chunk");
        }
        int written=audio_playback_write(pb, pcm_data, pcm_len);
        if (written>0 && !s_pet_tts_started) {
            s_pet_tts_started=1;
            voice_turn_mark("tts_first_write");
            ws_server_broadcast_voice_evt("pet_speak", "");
        }
    }

    (void)is_last;
}

/* ── Public API ──────────────────────────────────────────── */

int voice_channel_init(void)
{
    syslog(LOG_INFO, "[%s] Voice channel initializing\n", TAG);

    /* Reset state on init — static vars may persist across process restarts
     * in NuttX depending on how the binary is loaded. */
    s_voice.state = VOICE_IDLE;
    s_voice.cap = NULL;
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;
    s_voice.tts_abort = 0;
    s_voice.tts_pb = NULL;
    sem_init(&s_voice.rec_ready, 0, 0);

    if (!s_backends_registered) {
        /* 顺序有意义：第一个注册的成为默认后端。火山先注册保持旧行为，
         * 之后由 voice_channel_reload_backends() 按配置/Key 重新选。 */
        volc_tts_register();
        volc_asr_register();
        xiaomi_tts_register();
        xiaomi_asr_register();
        s_backends_registered = 1;
    }

    voice_channel_reload_backends();

    return 0;
}

/* 2026-11-01 P330：按配置选择 ASR/TTS 后端。
 * 优先级：config 显式指定 > 有小米 Key 则用小米 > 火山。
 * 设置页/NSH 改完 Key 后调用即可生效，不需要重启进程。 */
void voice_channel_reload_backends(void)
{
    char key[160] = { 0 };
    char asr[24] = { 0 };
    char tts[24] = { 0 };
    int have_xiaomi;

    /* 2026-11-02 P342：只看运行期配置里的 Key。
     * 交付版把 MiMo Key 预置在 usrdata 镜像的 config.json 里，所以刷完机
     * 这里就能读到；万一 /data 被 factory.sh 格式化掉，则整体回落到火山
     * （LLM 也一起回落），不会出现"后端选了小米但没有 Key"的半残状态。 */
    have_xiaomi = (claw_config_get(AGENT_CFG_KEY_MIMO_KEY, key, sizeof(key)) == OK
                      && key[0] != '\0')
               || (claw_config_get(AGENT_CFG_KEY_API_KEY, key, sizeof(key)) == OK
                      && key[0] != '\0');

    if (claw_config_get(AGENT_CFG_KEY_VOICE_ASR_BACKEND, asr, sizeof(asr)) != OK
        || asr[0] == '\0') {
        strlcpy(asr, have_xiaomi ? "xiaomi" : "volcengine", sizeof(asr));
    }
    if (claw_config_get(AGENT_CFG_KEY_VOICE_TTS_BACKEND, tts, sizeof(tts)) != OK
        || tts[0] == '\0') {
        strlcpy(tts, have_xiaomi ? "xiaomi" : "volcengine", sizeof(tts));
    }

    if (voice_asr_set_backend(asr) != 0) {
        syslog(LOG_WARNING, "[%s] ASR backend '%s' unavailable, keep default\n",
            TAG, asr);
    }
    if (voice_tts_set_backend(tts) != 0) {
        syslog(LOG_WARNING, "[%s] TTS backend '%s' unavailable, keep default\n",
            TAG, tts);
    }

    syslog(LOG_INFO, "[%s] backends: asr=%s tts=%s (mimo key %s)\n", TAG,
        voice_asr_get_backend() ? voice_asr_get_backend() : "none",
        voice_tts_get_backend() ? voice_tts_get_backend() : "none",
        have_xiaomi ? "set" : "missing");
}

int voice_channel_start(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING, "[%s] already recording\n", TAG);
        return -EBUSY;
    }

    /* Drain stale sem posts from previous sessions to prevent
     * sem_wait returning immediately before thread is ready. */
    while (sem_trywait(&s_voice.rec_ready) == 0)
        ;

    /* ── AEC workaround: stop active TTS to prevent echo ── */
    if (s_voice.tts_pb) {
        syslog(LOG_INFO,
            "[%s] PTT: stopping active TTS to prevent echo\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);

        /* Release lock so the speak thread can finish closing the
         * playback device.  Without this, we hold the lock through
         * the entire ASR pre-connect (300-800ms), blocking the speak
         * thread from calling audio_playback_close().  The playback
         * device stays open while we open the capture device, and on
         * BES hardware the shared codec (ci2s) gets confused, causing
         * incomplete or corrupted recording. */
        pthread_mutex_unlock(&s_voice.lock);

        /* Wait for the speak thread to finish — speak_lock is held
         * for the entire duration of voice_channel_speak(). */
        pthread_mutex_lock(&s_voice.speak_lock);
        pthread_mutex_unlock(&s_voice.speak_lock);

        syslog(LOG_INFO,
            "[%s] PTT: TTS playback fully closed\n", TAG);

        /* Re-acquire state lock and re-check — another thread may
         * have started recording while we released the lock. */
        pthread_mutex_lock(&s_voice.lock);
        if (s_voice.state != VOICE_IDLE) {
            pthread_mutex_unlock(&s_voice.lock);
            syslog(LOG_WARNING, "[%s] state changed during TTS stop\n", TAG);
            return -EBUSY;
        }
    }

    /* PCM buffer is lazy-allocated only when streaming ASR fails
     * (batch fallback). This saves 128KB RAM on the happy path. */
    s_voice.pcm_cap = AGENT_VOICE_PCM_BUF_SIZE;
    s_voice.pcm_buf = NULL;
    s_voice.pcm_len = 0;
    s_voice.asr_stream = NULL;

    /* Pre-open streaming ASR session HERE (before spawning the
     * recording thread) so that TLS handshake (~300-800ms) completes
     * before audio capture starts.  Without this, the handshake runs
     * inside recording_thread while audio is already flowing, causing
     * the first 300-800ms of speech to be lost.
     *
     * If the pre-open fails we fall back to batch mode as before. */
    voice_asr_stream_t* pre_stream = voice_asr_stream_open();
    if (!pre_stream) {
        syslog(LOG_WARNING,
            "[%s] pre-open ASR failed, will retry in thread\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] ASR pre-connected\n", TAG);
        s_voice.asr_stream = pre_stream;
    }

    /* Open capture device (but do NOT start yet — start after the
     * recording thread is spawned so the consumer is ready before
     * audio frames begin flowing, preventing media_recorder queue
     * overflow "data queue is more than max count(4)"). */
    s_voice.cap = audio_capture_open(
        AGENT_AUDIO_CAPTURE_DEV,
        AGENT_VOICE_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!s_voice.cap) {
        if (pre_stream) {
            voice_asr_stream_abort(pre_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_ERR, "[%s] capture open failed\n", TAG);
        return -EIO;
    }

    s_voice.state = VOICE_RECORDING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Spawn recording thread BEFORE starting capture */
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_VOICE_STACK);

    int ret = pthread_create(&s_voice.rec_thread, &attr,
        recording_thread, NULL);

    pthread_attr_destroy(&attr);

    if (ret != 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        pthread_mutex_unlock(&s_voice.lock);
        return -ret;
    }

    /* Now start capture — wait for recording thread to be ready first */
    sem_wait(&s_voice.rec_ready);

    if (audio_capture_start(s_voice.cap) < 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        if (s_voice.asr_stream) {
            voice_asr_stream_abort(s_voice.asr_stream);
            s_voice.asr_stream = NULL;
        }
        pthread_mutex_unlock(&s_voice.lock);
        pthread_join(s_voice.rec_thread, NULL);
        audio_capture_close(s_voice.cap);
        s_voice.cap = NULL;
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] PTT recording started\n", TAG);
    ws_server_broadcast_voice_evt("pet_listen", "");
    printf("Voice: recording... (use voice_stop to finish)\n");
    return 0;
}

int voice_channel_stop(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);
    voice_turn_begin("ptt_up");
    ws_server_broadcast_voice_evt("pet_think", "");

    /* Wait for recording thread to finish */
    pthread_join(s_voice.rec_thread, NULL);

    /* Close capture device */
    audio_capture_close(s_voice.cap);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (stream=%p)\n",
        TAG, pcm_len, (void*)stream);

    /* If streaming ASR was active, finish it to get result.
     * Note: in streaming mode pcm_len is always 0 because audio
     * goes directly to the ASR server, not to pcm_buf. */
    if (stream) {
        printf("Voice: streaming ASR active, finishing...\n");
        struct timespec asr_t0;
        clock_gettime(CLOCK_MONOTONIC, &asr_t0);
        char text[512];
        voice_turn_mark("asr_start");
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));
        voice_turn_mark("asr_done");
        struct timespec asr_t1;
        clock_gettime(CLOCK_MONOTONIC, &asr_t1);
        long asr_ms = (asr_t1.tv_sec - asr_t0.tv_sec) * 1000
            + (asr_t1.tv_nsec - asr_t0.tv_nsec) / 1000000;
        syslog(LOG_NOTICE, "[%s] ASR finish: %ldms\n", TAG, asr_ms);

        free(s_voice.pcm_buf); /* may be NULL */
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR: %s\n", TAG, text);
            printf("ASR: %s\n", text);
            clock_gettime(CLOCK_MONOTONIC, &s_asr_done_ts);

            /* 2026-08-15 P84：ASR 文本广播给 UI 显示（小智式"说话显字"） */
            ws_server_broadcast_voice_evt("stt", text);

            /* 2026-08-23 P115：本地命令短路（同 batch 路径） */
            {
                char cmd[64];
                if (voice_local_cmd_parse(text, cmd, sizeof(cmd))) {
                    syslog(LOG_INFO, "[%s] local cmd: %s\n", TAG, cmd);
                    voice_local_cmd_run(cmd);
                    return 0;
                }
            }

            agent_msg_t msg;

            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, AGENT_CHAN_VOICE,
                sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "voice",
                sizeof(msg.chat_id) - 1);
            msg.content = strdup(text);
            if (msg.content) {
                message_bus_push_inbound(&msg);
            } else ws_server_broadcast_voice_evt("pet_idle", "");
        } else {
            syslog(LOG_WARNING,
                "[%s] stream ASR failed: %d\n", TAG, ret);
            voice_report_asr_failure(ret);
        }
        return 0;
    }

    /* Batch fallback: pcm_buf must exist if we reach here */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        voice_report_asr_failure(0);      /* 没录到东西 → "没听清" */
        return -ENODATA;
    }

    /* Batch fallback: pack pcm_len + audio for ASR thread */
    size_t total = sizeof(size_t) + pcm_len;
    unsigned char* pkg = malloc(total);

    if (!pkg) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        ws_server_broadcast_voice_evt("pet_idle", "");
        return -ENOMEM;
    }

    memcpy(pkg, &pcm_len, sizeof(size_t));
    memcpy(pkg + sizeof(size_t), s_voice.pcm_buf, pcm_len);
    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;

    /* Spawn ASR worker thread */
    int ret = agent_task_create(asr_and_dispatch, "asr_worker",
        ASR_THREAD_STACK, pkg, AGENT_VOICE_PRIO);

    if (ret != OK) {
        free(pkg);
        ws_server_broadcast_voice_evt("pet_idle", "");
        return -EIO;
    }

    return 0;
}

int voice_channel_stop_with_text(char* text_out, size_t text_cap)
{
    if (!text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_RECORDING) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }
    s_voice.state = VOICE_STOPPING;
    pthread_mutex_unlock(&s_voice.lock);

    /* Wait for recording thread to finish */
    pthread_join(s_voice.rec_thread, NULL);

    /* Close capture device */
    audio_capture_close(s_voice.cap);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.cap = NULL;
    size_t pcm_len = s_voice.pcm_len;
    voice_asr_stream_t* stream = s_voice.asr_stream;
    s_voice.asr_stream = NULL;
    s_voice.state = VOICE_IDLE;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] recorded %zu bytes (with_text, stream=%p)\n",
        TAG, pcm_len, (void*)stream);

    /* Streaming ASR path: finish and copy text to caller.
     * In streaming mode audio goes directly to server, so
     * pcm_len may be 0 — that's OK, don't abort. */
    if (stream) {
        char text[512];
        int ret = voice_asr_stream_finish(stream, text,
            sizeof(text));

        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;

        if (ret == 0 && text[0] != '\0') {
            syslog(LOG_INFO, "[%s] stream ASR (with_text): %s\n",
                TAG, text);
            strncpy(text_out, text, text_cap - 1);
            text_out[text_cap - 1] = '\0';
        }
        return 0;
    }

    /* Batch fallback: synchronous ASR inline (no thread) */
    if (!s_voice.pcm_buf || pcm_len == 0) {
        free(s_voice.pcm_buf);
        s_voice.pcm_buf = NULL;
        syslog(LOG_ERR, "[%s] no PCM data for batch ASR\n", TAG);
        return -ENODATA;
    }

    syslog(LOG_INFO, "[%s] batch ASR (with_text): %zu bytes\n",
        TAG, pcm_len);

    char text[512];
    int ret = voice_asr_recognize(s_voice.pcm_buf, pcm_len,
        text, sizeof(text));

    free(s_voice.pcm_buf);
    s_voice.pcm_buf = NULL;

    if (ret == 0 && text[0] != '\0') {
        syslog(LOG_INFO, "[%s] batch ASR result: %s\n", TAG, text);
        strncpy(text_out, text, text_cap - 1);
        text_out[text_cap - 1] = '\0';
    } else if (ret != 0) {
        syslog(LOG_WARNING, "[%s] batch ASR failed: %d\n",
            TAG, ret);
    }

    return 0;
}

/* Strip Markdown formatting that TTS would read aloud.
 * Removes: **bold**, *italic*, # headings, - list bullets.
 * Operates in-place, result is always <= original length. */
static void tts_strip_markdown(char* s)
{
    char* r = s; /* read pointer */
    char* w = s; /* write pointer */

    while (*r) {
        /* Skip ** (bold markers) */
        if (r[0] == '*' && r[1] == '*') {
            r += 2;
            continue;
        }

        /* Skip lone * (italic markers) \xe2\x80\x94 but keep * in
         * contexts like "3*4" (digit before and after) */
        if (r[0] == '*') {
            if ((r == s || !isdigit((unsigned char)r[-1]))
                || !isdigit((unsigned char)r[1])) {
                r++;
                continue;
            }
        }

        /* Skip # at line start (headings) */
        if (r[0] == '#' && (r == s || r[-1] == '\n')) {
            while (*r == '#' || *r == ' ') {
                r++;
            }
            continue;
        }

        /* Skip "- " at line start (list bullets) */
        if (r[0] == '-' && r[1] == ' '
            && (r == s || r[-1] == '\n')) {
            r += 2;
            continue;
        }

        *w++ = *r++;
    }

    *w = '\0';
}

/* 2026-08-15 P84：打断正在播报的 TTS（小智 AbortSpeaking 式）。
 * 置 tts_abort + 停 playback（与 speak 内部 abort 同路径）；
 * 无播报时为空操作。由 WS "interrupt" 控制消息 / CLI 调用。 */
int voice_channel_interrupt(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb) {
        syslog(LOG_INFO, "[%s] interrupt: aborting active TTS\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);
    }
    pthread_mutex_unlock(&s_voice.lock);
    return 0;
}

/* ── 分句播报的中间态抑制（2026-11-02 P342）──────────────
 * 流式对话时一句话发一个 TTS 请求。句与句之间不应该把 UI/宠物打回待命，
 * 否则"播报中"会闪、猫会在 speak/idle 之间来回跳。
 * agent_loop 在整段回复开始前 hold(1)，全部播完后 session_finish()。 */
static volatile int s_speak_hold;

void voice_speak_hold(int on)
{
    /* P362：一轮开始复位，让本轮的第一次出声还能广播 pet_speak；
     * 之后同一轮的第二段不再广播，宠物"说话"动画不会中途重跳。 */
    if (on) {
        s_pet_tts_started = 0;
    }
    s_speak_hold = on;
}

static void voice_speak_end_events(void)
{
    if (s_speak_hold) {
        return;
    }

    ws_server_broadcast_voice_evt("pet_idle", "");
    ws_server_broadcast_voice_evt("done", "");
}

void voice_speak_session_finish(void)
{
    /* P362：第一句改成异步播之后，收尾时它可能还在播。等它播完再广播
     * 待命，否则宠物/状态栏会比声音先回到 idle（用户会看到"说完还在动"）。
     * 上限 30s，只是兜底，正常等的是最后一句的尾巴。 */
    for (int i = 0; i < 600 && voice_channel_is_speaking(); i++) {
        usleep(50 * 1000);
    }

    s_speak_hold = 0;
    ws_server_broadcast_voice_evt("pet_idle", "");
    ws_server_broadcast_voice_evt("done", "");
}

int voice_channel_speak(const char* text)
{
    if (!text || text[0] == '\0') {
        return -EINVAL;
    }
    /* If another thread is already speaking, abort its TTS playback
     * so we don't overlap media_player sessions (which causes the
     * media framework to attempt a ~1.3GB allocation and crash). */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb) {
        syslog(LOG_INFO, "[%s] speak: aborting previous TTS\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);
    }
    pthread_mutex_unlock(&s_voice.lock);

    pthread_mutex_lock(&s_voice.speak_lock);
    s_speak_seq++;   /* P362：调用方用它确认"这一段已经开始播" */

    /* ── AEC workaround: reject TTS while PTT is recording ── */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        pthread_mutex_unlock(&s_voice.speak_lock);
        syslog(LOG_INFO,
            "[%s] speak: skipped, recording active\n", TAG);
        ws_server_broadcast_voice_evt("done", "");
        return -EBUSY;
    }
    pthread_mutex_unlock(&s_voice.lock);

    /* Work on a mutable copy so we can strip markdown in-place */
    size_t text_len = strlen(text);
    char* clean = malloc(text_len + 1);

    if (!clean) {
        pthread_mutex_unlock(&s_voice.speak_lock);
        voice_speak_end_events();
        return -ENOMEM;
    }

    memcpy(clean, text, text_len + 1);
    tts_strip_markdown(clean);

    /* If stripping left nothing, bail out */
    if (clean[0] == '\0') {
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        voice_speak_end_events();
        return 0;
    }

    syslog(LOG_INFO, "[%s] speak: \"%.*s\" (%zu bytes)\n",
        TAG, 60, clean, strlen(clean));

    clock_gettime(CLOCK_MONOTONIC, &s_tts_start);
    s_tts_first_chunk = 0;
    /* P362：hold 期间（一轮分多段播）只有第一段广播 pet_speak，
     * 复位交给 voice_speak_hold(1)；非 hold 的单次播报仍每次复位。 */
    if (!s_speak_hold) {
        s_pet_tts_started = 0;
    }
    voice_turn_mark("tts_start");

    if (s_asr_done_ts.tv_sec > 0) {
        long llm_ms = (s_tts_start.tv_sec - s_asr_done_ts.tv_sec) * 1000
            + (s_tts_start.tv_nsec - s_asr_done_ts.tv_nsec) / 1000000;
        syslog(LOG_NOTICE, "[%s] LLM latency: %ldms\n", TAG, llm_ms);
    }

    /* Try streaming TTS first (callback per chunk) */
    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!pb) {
        syslog(LOG_ERR, "[%s] playback open failed\n", TAG);
        free(clean);
        pthread_mutex_unlock(&s_voice.speak_lock);
        voice_speak_end_events();
        return -EIO;
    }

    /* Register active playback so voice_channel_start() can abort it */
    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_abort = 0;
    s_voice.tts_pb = pb;
    pthread_mutex_unlock(&s_voice.lock);

    /* Use streaming TTS: each chunk writes to playback file */
    int ret = voice_tts_speak_stream(clean,
        tts_stream_cb, pb);

    struct timespec tts_net;
    clock_gettime(CLOCK_MONOTONIC, &tts_net);
    long net_ms = (tts_net.tv_sec - s_tts_start.tv_sec) * 1000
        + (tts_net.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_INFO, "[%s] TTS network done: %ldms\n", TAG, net_ms);

    /* Unregister active playback */
    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_pb = NULL;
    s_voice.tts_abort = 0;
    pthread_mutex_unlock(&s_voice.lock);

    audio_playback_close(pb);
    free(clean);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TTS stream failed: %d\n", TAG, ret);
        voice_turn_end("tts_fail");
        pthread_mutex_unlock(&s_voice.speak_lock);
        voice_speak_end_events();
        return ret;
    }

    struct timespec tend;
    clock_gettime(CLOCK_MONOTONIC, &tend);
    long total_ms = (tend.tv_sec - s_tts_start.tv_sec) * 1000
        + (tend.tv_nsec - s_tts_start.tv_nsec) / 1000000;
    syslog(LOG_NOTICE, "[%s] speak done: total %ldms (play wait %ldms)\n",
        TAG, total_ms, total_ms - net_ms);
    voice_turn_end("speak_done");
    pthread_mutex_unlock(&s_voice.speak_lock);
    voice_speak_end_events();
    return 0;
}

/* ── P362：句间空档修复 —— 下一段做成"边收边放"的流水线 ──────────
 *
 * 现象（用户："AI 语音对话一卡一卡的"，每轮都有）：串口实测一轮里有两次
 * "TTS first chunk" —— 第一句 2760ms 出声、4537ms 播完，第二段又要等
 * 1041ms 首包才出声，中间固定空 1 秒。
 *
 * 原因：P342 的"第一句先出声"要送两次 TTS（第一句一次、剩下的等流结束
 * 再一次），第二次得重付一遍 TLS 握手 + 首包，而这时第一句已经播完了。
 *
 * 做法：收尾段一边收一边攒，攒够 PREFETCH_START_MS 就试着接上播放
 * （拿不到 speak_lock 说明上一段还在播，继续攒，下一块再试），接上以后
 * 来一块写一块；流结束时还没接上就阻塞等上一段播完再一次性写完。
 *
 * 注意不能"整段攒完再放"：长回复的合成时间 ≈ 音频时长，那会比现在还卡。
 * 攒够 1 秒就开播，既不空等、也不会断。
 * 失败返回负值，调用方退回 voice_channel_speak()。
 */

#define PREFETCH_START_MS   1000
#define PREFETCH_MAX_BYTES  (4u * 1024u * 1024u)

typedef struct {
    unsigned char   *pcm;
    size_t           len;
    size_t           cap;
    int              err;
    int              playing;   /* 1 = 已经接上设备，后续直接写 */
    audio_playback_t *pb;
} prefetch_t;

static int prefetch_append(prefetch_t *p, const unsigned char *data, size_t len)
{
    if (p->len + len > p->cap)
        {
            size_t cap = p->cap ? p->cap : (256 * 1024);
            while (cap < p->len + len)
                cap *= 2;
            if (cap > PREFETCH_MAX_BYTES)
                return -EOVERFLOW;

            unsigned char *np = realloc(p->pcm, cap);
            if (!np)
                return -ENOMEM;
            p->pcm = np;
            p->cap = cap;
        }

    memcpy(p->pcm + p->len, data, len);
    p->len += len;
    return 0;
}

int voice_channel_is_speaking(void)
{
    int on;

    pthread_mutex_lock(&s_voice.lock);
    on = (s_voice.tts_pb != NULL);
    pthread_mutex_unlock(&s_voice.lock);
    return on;
}

unsigned voice_channel_speak_seq(void)
{
    return s_speak_seq;
}

/* 试着接上播放。block=0 用 trylock（上一段还在播就返回 -EAGAIN 继续攒）；
 * block=1 阻塞等上一段播完。成功返回 0 并持有 speak_lock。 */
static int prefetch_begin(prefetch_t *p, int block)
{
    if (p->playing)
        return 0;

    int rc = block ? pthread_mutex_lock(&s_voice.speak_lock)
                   : pthread_mutex_trylock(&s_voice.speak_lock);
    if (rc != 0)
        return -EAGAIN;

    if (s_voice.state != VOICE_IDLE)
        {
            pthread_mutex_unlock(&s_voice.speak_lock);
            syslog(LOG_INFO, "[%s] prefetch dropped: recording active\n", TAG);
            return -EBUSY;
        }

    audio_playback_t *pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!pb)
        {
            pthread_mutex_unlock(&s_voice.speak_lock);
            return -EIO;
        }

    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_abort = 0;
    s_voice.tts_pb = pb;
    pthread_mutex_unlock(&s_voice.lock);

    p->pb = pb;
    p->playing = 1;
    voice_turn_mark("tts_prefetch_play");
    syslog(LOG_NOTICE, "[%s] prefetch joined with %zu bytes buffered\n",
        TAG, p->len);

    size_t off = 0;
    while (off < p->len)
        {
            int n = audio_playback_write(pb, p->pcm + off, p->len - off);
            if (n <= 0)
                {
                    p->err = -EIO;
                    break;
                }
            off += (size_t)n;
        }

    free(p->pcm);
    p->pcm = NULL;
    p->len = 0;
    p->cap = 0;
    return 0;
}

static void prefetch_cb(const unsigned char *data, size_t len, int is_last,
                        void *user)
{
    prefetch_t *p = (prefetch_t *)user;

    if (is_last || !data || len == 0 || p->err)
        return;

    if (p->playing)
        {
            int n = audio_playback_write(p->pb, data, len);
            if (n <= 0)
                p->err = -EIO;
            return;
        }

    if (prefetch_append(p, data, len) != 0)
        {
            p->err = -ENOMEM;
            return;
        }

    if (p->len >= (size_t)AGENT_TTS_WS_SAMPLE_RATE * 2u * PREFETCH_START_MS / 1000u)
        prefetch_begin(p, 0);
}

int voice_channel_speak_prefetch(const char *text)
{
    if (!text || text[0] == '\0')
        return -EINVAL;

    size_t text_len = strlen(text);
    char *clean = malloc(text_len + 1);

    if (!clean)
        return -ENOMEM;
    memcpy(clean, text, text_len + 1);
    tts_strip_markdown(clean);
    if (clean[0] == '\0')
        {
            free(clean);
            return -EINVAL;
        }

    prefetch_t p;
    memset(&p, 0, sizeof(p));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int ret = voice_tts_speak_stream(clean, prefetch_cb, &p);
    free(clean);

    if (ret != 0 && !p.playing)
        {
            syslog(LOG_WARNING, "[%s] prefetch synth failed ret=%d err=%d len=%zu\n",
                TAG, ret, p.err, p.len);
            free(p.pcm);
            return ret;
        }

    /* 流结束还没接上（剩余段太短，没攒到 1 秒）：等上一段播完一次性写完 */
    if (!p.playing)
        {
            int rc = prefetch_begin(&p, 1);
            if (rc != 0)
                {
                    free(p.pcm);
                    return rc;
                }
        }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    syslog(LOG_NOTICE, "[%s] prefetch done in %ldms (err=%d)\n", TAG,
        (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000,
        p.err);

    pthread_mutex_lock(&s_voice.lock);
    s_voice.tts_pb = NULL;
    s_voice.tts_abort = 0;
    pthread_mutex_unlock(&s_voice.lock);

    audio_playback_close(p.pb);
    free(p.pcm);
    pthread_mutex_unlock(&s_voice.speak_lock);

    /* 已经接上播过就不报错：否则调用方会退回整段重念一遍，变成重复出声。
     * 中途写失败只记日志。 */
    return 0;
}

/* ── Test commands (file-based, for QEMU) ───────────────── */

/* 2026-08-14：--test tts 从 voice_tts_speak（旧 HTTP V3 unidirectional）
 * 改接 voice_tts_speak_stream（新版双向流式 WS V3）。原路径走
 * volc_tts.c 的 HTTP 接口，X-Api-Resource-Id 换成 seed-tts-2.0 后
 * 旧接口报 55000000 音色不匹配且永远走不到新版 WS。流式回调把
 * PCM chunk 累积进缓冲，行为与原"一次性返回"一致。 */
typedef struct {
    unsigned char* pcm;
    size_t len;
    size_t cap;
    int err;
} tts_collect_t;

static void tts_collect_cb(const unsigned char* data, size_t len,
    int is_last, void* user_data)
{
    tts_collect_t* acc = (tts_collect_t*)user_data;

    if (is_last || !data || len == 0) {
        return;
    }

    if (acc->len + len > acc->cap) {
        acc->err = -EOVERFLOW;
        return;
    }

    memcpy(acc->pcm + acc->len, data, len);
    acc->len += len;
}

int voice_channel_test_tts(const char* text, const char* out_path)
{
    if (!text || !out_path) {
        printf("Usage: voice_test_tts <text> [output_path]\n");
        return -EINVAL;
    }

    printf("TTS: synthesizing \"%s\" ...\n", text);

    unsigned char* pcm = malloc(AGENT_VOICE_PCM_BUF_SIZE);

    if (!pcm) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    tts_collect_t acc = { .pcm = pcm, .cap = AGENT_VOICE_PCM_BUF_SIZE };
    int ret = voice_tts_speak_stream(text, tts_collect_cb, &acc);

    if (ret != 0 || acc.err != 0) {
        free(pcm);
        printf("TTS failed: %d\n", ret ? ret : acc.err);
        return ret ? ret : acc.err;
    }

    size_t pcm_len = acc.len;

    ret = write_pcm_file(out_path, pcm, pcm_len);

    if (ret == 0) {
        printf("TTS OK: %zu bytes -> %s\n", pcm_len, out_path);
    } else {
        printf("Failed to write %s: %d\n", out_path, ret);
    }

    /* 2026-08-12 TASK-01：TTS 合成后直接喇叭播放（原实现只落盘不播放，
     * 无法验证"出声"）。照抄 voice_channel_speak 的 playback 用法：
     * TTS 输出 24kHz（AGENT_TTS_WS_SAMPLE_RATE）→ audio_playback_write。 */
    printf("TTS: playing %zu bytes...\n", pcm_len);

    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (!pb) {
        printf("TTS playback open failed\n");
        free(pcm);
        return -EIO;
    }

    if (audio_playback_write(pb, pcm, pcm_len) < 0) {
        printf("TTS playback write failed\n");
    }

    audio_playback_close(pb);
    printf("TTS: playback done\n");

    free(pcm);
    return ret;
}

/* ── 2026-08-11 MIC DEMO：录音 N 秒 → WAV 文件（16kHz/16bit/mono）──
 * 复用 audio_capture（AW media_recorder，与语音对讲同源），用于验证
 * mic 硬件 + 录音链路；产物 WAV 可直接回放（音乐播放器/XPlayer）。 */
int voice_channel_test_mic(int seconds, const char* out_path)
{
    if (seconds <= 0) {
        seconds = 5;
    }

    if (!out_path) {
        out_path = AGENT_DATA_DIR "/mic_demo.wav";
    }

    printf("MIC: recording %d sec to %s ...\n", seconds, out_path);

    audio_capture_t* cap = audio_capture_open(
        AGENT_AUDIO_CAPTURE_DEV, AGENT_VOICE_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS, AGENT_VOICE_BITS);

    if (!cap) {
        printf("MIC error: capture open failed\n");
        return -EIO;
    }

    if (audio_capture_start(cap) != 0) {
        audio_capture_close(cap);
        printf("MIC error: capture start failed\n");
        return -EIO;
    }

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        printf("MIC error: cannot create %s: %d\n", out_path, errno);
        audio_capture_close(cap);
        return -errno;
    }

    /* Write placeholder WAV header (44 bytes), patched after recording */
    unsigned char wav_hdr[44] = { 0 };

    if (write(fd, wav_hdr, sizeof(wav_hdr)) != (ssize_t)sizeof(wav_hdr)) {
        close(fd);
        audio_capture_close(cap);
        return -EIO;
    }

    size_t total = 0;
    size_t target = (size_t)seconds * AGENT_VOICE_SAMPLE_RATE
        * AGENT_VOICE_CHANNELS * (AGENT_VOICE_BITS / 8);
    unsigned char chunk[AGENT_ASR_CHUNK_SIZE];

    while (total < target) {
        size_t want = (target - total < sizeof(chunk))
            ? (target - total) : sizeof(chunk);
        int n = audio_capture_read(cap, chunk, want);

        if (n <= 0) {
            syslog(LOG_WARNING, "[%s] mic: read stopped early: %d\n",
                TAG, n);
            break;
        }

        if (write(fd, chunk, (size_t)n) != n) {
            close(fd);
            audio_capture_close(cap);
            return -EIO;
        }

        total += (size_t)n;
    }

    audio_capture_close(cap);

    /* Patch WAV header */
    uint32_t data_len = (uint32_t)total;
    uint32_t riff_len = data_len + 36;
    uint32_t byte_rate = AGENT_VOICE_SAMPLE_RATE * AGENT_VOICE_CHANNELS
        * (AGENT_VOICE_BITS / 8);
    uint16_t block_align = (uint16_t)(AGENT_VOICE_CHANNELS
        * (AGENT_VOICE_BITS / 8));

    memcpy(wav_hdr, "RIFF", 4);
    wav_hdr[4] = (unsigned char)(riff_len & 0xFF);
    wav_hdr[5] = (unsigned char)((riff_len >> 8) & 0xFF);
    wav_hdr[6] = (unsigned char)((riff_len >> 16) & 0xFF);
    wav_hdr[7] = (unsigned char)((riff_len >> 24) & 0xFF);
    memcpy(wav_hdr + 8, "WAVE", 4);
    memcpy(wav_hdr + 12, "fmt ", 4);
    wav_hdr[16] = 16; wav_hdr[17] = 0; wav_hdr[18] = 0; wav_hdr[19] = 0;
    wav_hdr[20] = 1; wav_hdr[21] = 0; /* PCM */
    wav_hdr[22] = (unsigned char)AGENT_VOICE_CHANNELS;
    wav_hdr[23] = 0;
    wav_hdr[24] = (unsigned char)(AGENT_VOICE_SAMPLE_RATE & 0xFF);
    wav_hdr[25] = (unsigned char)((AGENT_VOICE_SAMPLE_RATE >> 8) & 0xFF);
    wav_hdr[26] = (unsigned char)((AGENT_VOICE_SAMPLE_RATE >> 16) & 0xFF);
    wav_hdr[27] = (unsigned char)((AGENT_VOICE_SAMPLE_RATE >> 24) & 0xFF);
    wav_hdr[28] = (unsigned char)(byte_rate & 0xFF);
    wav_hdr[29] = (unsigned char)((byte_rate >> 8) & 0xFF);
    wav_hdr[30] = (unsigned char)((byte_rate >> 16) & 0xFF);
    wav_hdr[31] = (unsigned char)((byte_rate >> 24) & 0xFF);
    wav_hdr[32] = (unsigned char)(block_align & 0xFF);
    wav_hdr[33] = (unsigned char)((block_align >> 8) & 0xFF);
    wav_hdr[34] = (unsigned char)(AGENT_VOICE_BITS & 0xFF);
    wav_hdr[35] = (unsigned char)((AGENT_VOICE_BITS >> 8) & 0xFF);
    memcpy(wav_hdr + 36, "data", 4);
    wav_hdr[40] = (unsigned char)(data_len & 0xFF);
    wav_hdr[41] = (unsigned char)((data_len >> 8) & 0xFF);
    wav_hdr[42] = (unsigned char)((data_len >> 16) & 0xFF);
    wav_hdr[43] = (unsigned char)((data_len >> 24) & 0xFF);

    lseek(fd, 0, SEEK_SET);
    write(fd, wav_hdr, sizeof(wav_hdr));
    close(fd);

    /* ── 2026-08-11 MIC DEMO 自动回放：读 WAV data 段（跳 44B 头）播放 ──
     * 验证 mic→capture→WAV→playback 完整闭环；半双工共用 codec，
     * 先 close capture 再 open playback（顺序已由 audio_capture_close 保证）。 */
    printf("MIC: playing back %s ...\n", out_path);

    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV, AGENT_VOICE_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS, AGENT_VOICE_BITS);

    if (pb) {
        int pfd = open(out_path, O_RDONLY);

        if (pfd >= 0) {
            lseek(pfd, 44, SEEK_SET); /* skip WAV header */
            unsigned char pbuf[4096];
            ssize_t n;

            while ((n = read(pfd, pbuf, sizeof(pbuf))) > 0) {
                if (audio_playback_write(pb, pbuf, (size_t)n) < 0)
                    break;
            }

            close(pfd);
        }

        audio_playback_close(pb);
        printf("MIC: playback done\n");
    } else {
        printf("MIC: playback open failed (mic file kept)\n");
    }

    printf("MIC OK: %zu bytes (%d sec) -> %s\n", total, seconds,
        out_path);
    return 0;
}

typedef struct {
    char pcm_path[256];
} asr_task_args_t;

static void* asr_file_worker(void* arg)
{
    asr_task_args_t* a = (asr_task_args_t*)arg;
    unsigned char* pcm = NULL;
    size_t pcm_len = 0;

    int ret = read_pcm_file(a->pcm_path, &pcm, &pcm_len);

    if (ret != 0) {
        printf("Failed to read %s: %d\n", a->pcm_path, ret);
        free(a);
        return NULL;
    }

    /* 2026-08-14 诊断增强：读文件后先算 PCM 能量（peak/非零比），
     * 区分「录到声音但服务器判无语音（增益问题）」与「根本没说话
     * （静音文件）」。peak ≈ 每样本绝对值的最大值，16bit 满幅 32768；
     * 静音环境录的 wav 通常 peak < 200。 */
    {
        size_t n = pcm_len / 2;
        int peak = 0;
        size_t nonzero = 0;

        for (size_t i = 0; i < n; i++) {
            int16_t s = (int16_t)((uint16_t)pcm[2 * i]
                | ((uint16_t)pcm[2 * i + 1] << 8));
            int abs_s = (s < 0) ? -s : s;

            if (abs_s > peak) {
                peak = abs_s;
            }
            if (abs_s > 20) {
                nonzero++;
            }
        }

        printf("ASR: pcm=%zuB peak=%d (16bit full=%d) "
            "nonzeroratio=%.1f%%\n",
            pcm_len, peak, 32768,
            n ? 100.0 * (double)nonzero / (double)n : 0.0);
    }

    printf("ASR: recognizing %zu bytes from %s ...\n",
        pcm_len, a->pcm_path);

    char text[512];

    ret = voice_asr_recognize(pcm, pcm_len, text, sizeof(text));
    free(pcm);

    if (ret == 0) {
        printf("ASR result: %s\n", text);

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            message_bus_push_inbound(&msg);
            printf("Sent to agent.\n");
        }
    } else {
        printf("ASR failed: %d\n", ret);
    }

    free(a);
    return NULL;
}

int voice_channel_test_asr(const char* pcm_path)
{
    if (!pcm_path) {
        printf("Usage: voice_test_asr <pcm_file>\n");
        return -EINVAL;
    }

    asr_task_args_t* args = malloc(sizeof(asr_task_args_t));

    if (!args) {
        printf("Error: out of memory\n");
        return -ENOMEM;
    }

    strncpy(args->pcm_path, pcm_path, sizeof(args->pcm_path) - 1);
    args->pcm_path[sizeof(args->pcm_path) - 1] = '\0';

    int ret = agent_task_create(asr_file_worker, "asr_test",
        ASR_THREAD_STACK, args, AGENT_VOICE_PRIO);

    if (ret != OK) {
        printf("Error: failed to create ASR thread\n");
        free(args);
        return -EIO;
    }

    printf("ASR test started (background thread).\n");
    return 0;
}

/* 2026-08-12 TASK-04：端到端测试——capture Ns → ASR → LLM → TTS → speaker。
 * 全部复用已验证组件（audio_capture / voice_asr_recognize / llm_chat /
 * voice_tts_speak / audio_playback），执行入口 ai_agent --test full。
 * 待上板实测（TASK-04 判定）。 */
int voice_channel_test_full(int seconds)
{
    if (seconds <= 0) {
        seconds = 4;
    }

    /* 1. Capture */
    printf("FULL: capture %ds ...\n", seconds);

    audio_capture_t* cap = audio_capture_open(AGENT_AUDIO_CAPTURE_DEV,
        AGENT_VOICE_SAMPLE_RATE, AGENT_VOICE_CHANNELS, AGENT_VOICE_BITS);

    if (!cap) {
        printf("FULL: capture open failed\n");
        return -EIO;
    }

    audio_capture_start(cap);

    size_t cap_cap = (size_t)AGENT_VOICE_SAMPLE_RATE * AGENT_VOICE_CHANNELS
        * (AGENT_VOICE_BITS / 8) * (size_t)seconds;
    unsigned char* pcm = malloc(cap_cap);

    if (!pcm) {
        audio_capture_close(cap);
        return -ENOMEM;
    }

    size_t got = 0;

    while (got < cap_cap) {
        size_t want = cap_cap - got;

        if (want > AGENT_ASR_CHUNK_SIZE) {
            want = AGENT_ASR_CHUNK_SIZE;
        }

        int n = audio_capture_read(cap, pcm + got, want);

        if (n <= 0) {
            usleep(10 * 1000);
            continue;
        }
        got += (size_t)n;
    }

    audio_capture_close(cap);
    printf("FULL: captured %zu bytes\n", got);

    /* 2. ASR */
    char text[512];
    int ret = voice_asr_recognize(pcm, got, text, sizeof(text));

    free(pcm);

    if (ret != 0) {
        printf("FULL: ASR failed: %d\n", ret);
        return ret;
    }

    printf("FULL: ASR = %s\n", text);

    /* 3. LLM（冻结区，llm_chat 同步） */
    char messages[1024];
    char reply[2048];

    /* 2026-08-14：ASR 文本最多 511B，messages[512] 放不下
     * "[{"role":"user","content":"%s"}]" 包装 → 截断告警。改 1024。 */
    snprintf(messages, sizeof(messages),
        "[{\"role\":\"user\",\"content\":\"%s\"}]", text);
    llm_proxy_init();
    ret = llm_chat("You are a helpful assistant.", messages,
        reply, sizeof(reply));

    printf("FULL: LLM = %s\n", reply);

    /* 4. TTS → speaker（2026-08-14：同 --test tts 改接双向流式 WS
     * speak_stream，原 voice_tts_speak 走旧 HTTP V3 路径） */
    unsigned char* tts_pcm = malloc(AGENT_VOICE_PCM_BUF_SIZE);
    size_t tts_len = 0;

    if (!tts_pcm) {
        return -ENOMEM;
    }

    tts_collect_t tts_acc = { .pcm = tts_pcm, .cap = AGENT_VOICE_PCM_BUF_SIZE };

    ret = voice_tts_speak_stream(reply, tts_collect_cb, &tts_acc);

    if (ret != 0 || tts_acc.err != 0) {
        printf("FULL: TTS failed: %d\n", ret ? ret : tts_acc.err);
        free(tts_pcm);
        return ret ? ret : tts_acc.err;
    }

    tts_len = tts_acc.len;

    printf("FULL: TTS %zu bytes, playing...\n", tts_len);

    audio_playback_t* pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV,
        AGENT_TTS_WS_SAMPLE_RATE,
        AGENT_VOICE_CHANNELS,
        AGENT_VOICE_BITS);

    if (pb) {
        audio_playback_write(pb, tts_pcm, tts_len);
        audio_playback_close(pb);
        printf("FULL: playback done\n");
    } else {
        printf("FULL: playback open failed\n");
    }

    free(tts_pcm);
    printf("FULL: done\n");
    return 0;
}

/* ── 唤醒词监听（2026-08-15 P80：说「你好 openvela」免按键唤醒）──
 * 空闲周期短录 → 能量门控（静音不上 ASR，省 API）→ stream ASR 匹配
 * openvela → 命中后播提示音 + 自动聆听指令（静音自动结束）→ batch ASR
 * → LLM → TTS 闭环。与 PTT 互斥：PTT 录音/播报中跳过监听窗口。 */

#define WAKE_WINDOW_MS     1500   /* 每轮监听窗口时长 */
#define WAKE_ENERGY_MIN    1200   /* 能量门控：peak 低于此值视为静音，跳过 ASR */
#define WAKE_LISTEN_MAX_MS 8000   /* 唤醒后自动聆听上限 */
#define WAKE_SILENCE_MS    1200   /* 唤醒后聆听：连续静音多久自动结束 */

static volatile int s_wake_active;
static volatile int s_wake_triggered;
static pthread_t s_wake_thread;

/* 文本是否含唤醒词（忽略大小写/空白）：openvela */
static int wake_text_match(const char* text)
{
    if (!text) {
        return 0;
    }

    char buf[128];
    size_t w = 0;

    for (const char* p = text; *p && w < sizeof(buf) - 1; p++) {
        char c = *p;

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c += 32;
        }
        buf[w++] = c;
    }
    buf[w] = '\0';
    return strstr(buf, "openvela") != NULL;
}

/* 录音 ms 时长到 buf；silence_ms>0 时连续静音达阈值提前返回。
 * 返回实际字节数，out_peak 返回窗口 peak。 */
static size_t wake_record_window(audio_capture_t* cap,
    unsigned char* buf, size_t cap_size, int ms,
    int silence_ms, int* out_peak)
{
    int bytes_per_s = AGENT_VOICE_SAMPLE_RATE
        * AGENT_VOICE_CHANNELS * (AGENT_VOICE_BITS / 8);
    size_t need = (size_t)bytes_per_s * ms / 1000;
    size_t got = 0;
    int peak = 0;
    int silent_run_ms = 0;
    unsigned char chunk[AGENT_ASR_CHUNK_SIZE];

    while (got < need && got + sizeof(chunk) <= cap_size) {
        int n = audio_capture_read(cap, chunk, sizeof(chunk));

        if (n <= 0) {
            usleep(10 * 1000);
            continue;
        }

        int16_t* sp = (int16_t*)chunk;

        for (int i = 0; i < n / 2; i++) {
            int v = sp[i] < 0 ? -sp[i] : sp[i];

            if (v > peak) {
                peak = v;
            }
        }
        memcpy(buf + got, chunk, (size_t)n);
        got += (size_t)n;

        if (silence_ms > 0) {
            int chunk_ms = n * 1000 / bytes_per_s;

            if (peak < WAKE_ENERGY_MIN) {
                silent_run_ms += chunk_ms;
            } else {
                silent_run_ms = 0;
            }
            if (silent_run_ms >= silence_ms) {
                break;
            }
        }
    }

    if (out_peak) {
        *out_peak = peak;
    }
    return got;
}

static void* wake_thread(void* arg)
{
    (void)arg;
    unsigned char* win = malloc(AGENT_VOICE_PCM_BUF_SIZE);

    if (!win) {
        return NULL;
    }

    syslog(LOG_INFO, "[%s] wake: listening for 'hello openvela'\n", TAG);

    while (s_wake_active) {
        /* 与 PTT/TTS 互斥：录音中或播报中跳过本轮监听 */
        pthread_mutex_lock(&s_voice.lock);
        int busy = (s_voice.state != VOICE_IDLE)
            || (s_voice.tts_pb != NULL);
        pthread_mutex_unlock(&s_voice.lock);
        if (busy) {
            usleep(200 * 1000);
            continue;
        }

        audio_capture_t* cap = audio_capture_open(
            AGENT_AUDIO_CAPTURE_DEV, AGENT_VOICE_SAMPLE_RATE,
            AGENT_VOICE_CHANNELS, AGENT_VOICE_BITS);

        if (!cap) {
            usleep(500 * 1000);
            continue;
        }
        if (audio_capture_start(cap) < 0) {
            audio_capture_close(cap);
            usleep(500 * 1000);
            continue;
        }

        int peak = 0;
        size_t got = wake_record_window(cap, win,
            AGENT_VOICE_PCM_BUF_SIZE, WAKE_WINDOW_MS, 0, &peak);

        audio_capture_close(cap);
        if (!s_wake_active) {
            break;
        }

        /* 能量门控：静音窗口不上 ASR（省 API） */
        if (got < (size_t)AGENT_ASR_CHUNK_SIZE || peak < WAKE_ENERGY_MIN) {
            continue;
        }

        /* stream ASR 识别窗口内容 */
        voice_asr_stream_t* st = voice_asr_stream_open();

        if (!st) {
            usleep(300 * 1000);
            continue;
        }
        size_t off = 0;

        while (off < got) {
            size_t clen = got - off;

            if (clen > AGENT_ASR_CHUNK_SIZE) {
                clen = AGENT_ASR_CHUNK_SIZE;
            }
            if (voice_asr_stream_send(st, win + off, clen) != 0) {
                break;
            }
            off += clen;
        }

        char text[512];
        int ret = voice_asr_stream_finish(st, text, sizeof(text));

        syslog(LOG_INFO, "[%s] wake: window ASR ret=%d text=%.60s\n",
            TAG, ret, text);

        if (ret == 0 && wake_text_match(text)) {
            s_wake_triggered = 1;
            syslog(LOG_INFO, "[%s] wake: TRIGGERED by '%.60s'\n",
                TAG, text);

            /* 提示音（阻塞至播完，随后聆听不会录到提示音） */
            voice_channel_speak("我在，请说");

            if (!s_wake_active) {
                break;
            }

            /* 自动聆听用户指令：录到静音或上限 */
            audio_capture_t* cap2 = audio_capture_open(
                AGENT_AUDIO_CAPTURE_DEV, AGENT_VOICE_SAMPLE_RATE,
                AGENT_VOICE_CHANNELS, AGENT_VOICE_BITS);

            if (cap2 && audio_capture_start(cap2) == 0) {
                int peak2 = 0;
                size_t got2 = wake_record_window(cap2, win,
                    AGENT_VOICE_PCM_BUF_SIZE, WAKE_LISTEN_MAX_MS,
                    WAKE_SILENCE_MS, &peak2);

                audio_capture_close(cap2);

                if (got2 >= (size_t)AGENT_ASR_CHUNK_SIZE) {
                    /* batch ASR → LLM → TTS（复用 asr_and_dispatch 打包） */
                    size_t total = sizeof(size_t) + got2;
                    unsigned char* pkg = malloc(total);

                    if (pkg) {
                        memcpy(pkg, &got2, sizeof(size_t));
                        memcpy(pkg + sizeof(size_t), win, got2);
                        agent_task_create(asr_and_dispatch, "asr_worker",
                            ASR_THREAD_STACK, pkg, AGENT_VOICE_PRIO);
                    }
                }
            }
        }
    }

    free(win);
    s_wake_active = 0;
    syslog(LOG_INFO, "[%s] wake: stopped\n", TAG);
    return NULL;
}

int voice_wake_start(void)
{
    if (s_wake_active) {
        return 0;
    }

    s_wake_active = 1;
    s_wake_triggered = 0;

    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_VOICE_STACK);

    int ret = pthread_create(&s_wake_thread, &attr, wake_thread, NULL);

    pthread_attr_destroy(&attr);

    if (ret != 0) {
        s_wake_active = 0;
        return -ret;
    }

    syslog(LOG_INFO, "[%s] wake: listening started\n", TAG);
    return 0;
}

int voice_wake_stop(void)
{
    if (!s_wake_active && !s_wake_thread) {
        return 0;
    }

    s_wake_active = 0;

    if (s_wake_thread) {
        pthread_join(s_wake_thread, NULL);
        s_wake_thread = 0;
    }

    syslog(LOG_INFO, "[%s] wake: stopped\n", TAG);
    return 0;
}
