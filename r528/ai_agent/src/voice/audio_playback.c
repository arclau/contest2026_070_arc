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

/* audio_playback.c — Streaming audio playback via board aw-alsa-lib.
 *
 * 2026-08-11 上板修复（与 audio_capture 同源）：原实现走 openvela
 * media_player 框架，但 R528 gemini-s1 固件未编译 media server
 * （CONFIG_LIB_FFMPEG / CONFIG_LIB_PFW 均未启用），media_player_open()
 * 同样会失败。改为直接调用板级 aw-alsa-lib（snd_vela_pcm_*），与
 * luncher_dm 的 dm_sound_open 播放路径一致（period 1024/buffer 8192，
 * start_threshold=buffer 填满即启动——切歌无声排查确认的稳定参数）。
 *
 * writei() 同步阻塞写入，TTS PCM 块到达即播，无需临时文件。 */

#include "voice/audio_playback.h"
#include "agent_config.h"

#include "pcm.h" /* aw-alsa-lib: snd_vela_pcm_* */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

/* 2026-11-02 P341：观测计数。只统计，不改变写入行为。 */
static volatile unsigned s_xrun_count;
static volatile unsigned s_short_write_count;

unsigned audio_playback_xrun_count(void)
{
    return s_xrun_count;
}

unsigned audio_playback_short_write_count(void)
{
    return s_short_write_count;
}


/* 板级 PCM 卡名：与 luncher_dm dm_sound_open 一致（CONFIG_AW_AUDIO_CODEC=y） */
#define AGENT_PLAYBACK_CARD "hw:audiocodec"

/* 播放 buffer 参数（切歌无声修复验证值：buffer 8192 帧 ≈170ms） */
#define AGENT_PLAYBACK_PERIOD 1024
#define AGENT_PLAYBACK_BUFFER 8192

/* 2026-09-14：AI 语音音量跟随音乐音量（用户反馈 TTS 比音乐响太多）。
 * luncher_dm 把音量滑块/语音命令的值持久化到 /data/dm_health.cfg 的
 * music_volume=NN；agent 每次开播前读一次，按同比例对 TTS PCM 做定点衰减，
 * 与音乐（dm_sound_write）用同一套软音量曲线。 */
#define AGENT_VOLUME_CFG     "/data/dm_health.cfg"
#define AGENT_VOLUME_KEY     "music_volume="
#define AGENT_VOLUME_DEFAULT 30

struct audio_playback {
    snd_pcm_t* pcm;
    size_t total_written;
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bits_per_sample;
    volatile int stopped; /* set by audio_playback_stop() from another thread */
    int16_t last_sample;  /* 2026-08-31 尾音修复：最后写入样本（fade-out 起点） */
    int volume;           /* 0-100：软件音量（跟随音乐音量） */
};

/* 读 /data/dm_health.cfg 的 music_volume（0-100）；缺失/异常回默认 30 */
static int audio_pb_read_volume(void)
{
    FILE* f = fopen(AGENT_VOLUME_CFG, "r");
    char line[128];
    int vol = AGENT_VOLUME_DEFAULT;

    if (!f) {
        return vol;
    }

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, AGENT_VOLUME_KEY, sizeof(AGENT_VOLUME_KEY) - 1) == 0) {
            int v = atoi(line + (sizeof(AGENT_VOLUME_KEY) - 1));
            if (v < 0) {
                v = 0;
            }
            if (v > 100) {
                v = 100;
            }
            vol = v;
            break;
        }
    }

    fclose(f);
    return vol;
}

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    (void)dev_path; /* aw-alsa-lib 按卡名路由 */

    snd_pcm_t* handle = NULL;
    snd_pcm_hw_params_t* params = NULL;
    snd_pcm_uframes_t period = AGENT_PLAYBACK_PERIOD;
    snd_pcm_uframes_t buffer = AGENT_PLAYBACK_BUFFER;
    int ret;
    int attempt;

    /* P103：声卡单实例（hw:audiocodec），音乐（luncher_dm XPlayer/dm_sound）
     * 播放中 open 会 EBUSY(-16)。TTS 播报优先级低于音乐：重试等待声卡
     * 释放（切歌/暂停/播完），最多 10 次 × 200ms = 2s，仍失败才降级。
     * （日志实证：music WRITE_GAP 427s 期间 TTS playback open failed -16） */
    for (attempt = 0; attempt < 10; attempt++) {
        ret = snd_vela_pcm_open(&handle, AGENT_PLAYBACK_CARD,
            SND_VELA_PCM_STREAM_PLAYBACK, 0);
        if (ret >= 0)
            break;
        if (ret == -EBUSY) {
            usleep(200 * 1000);
            continue;
        }
        break;   /* 非 EBUSY 错误不重试 */
    }
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] snd_vela_pcm_open(%s, PLAYBACK) fail %d\n",
            TAG, AGENT_PLAYBACK_CARD, ret);
        return NULL;
    }

    snd_vela_pcm_hw_params_malloc(&params);
    ret = snd_vela_pcm_hw_params_any(handle, params);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] hw_params_any fail %d\n", TAG, ret);
        goto err;
    }

    snd_vela_pcm_hw_params_set_access(handle, params,
        SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_vela_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    snd_vela_pcm_hw_params_set_channels(handle, params, channels);
    snd_vela_pcm_hw_params_set_rate(handle, params, sample_rate, 0);
    snd_vela_pcm_hw_params_set_period_size(handle, params, period, 0);
    snd_vela_pcm_hw_params_set_buffer_size(handle, params, buffer);

    ret = snd_vela_pcm_hw_params(handle, params);
    snd_vela_pcm_hw_params_free(params);
    params = NULL;
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] hw_params fail %d\n", TAG, ret);
        goto err;
    }

    /* SW params：start_threshold=buffer（填满才启动），参考 dm_sound_open */
    {
        snd_pcm_sw_params_t* sw;
        snd_pcm_uframes_t boundary = 0;

        snd_pcm_sw_params_alloca(&sw);
        snd_vela_pcm_sw_params_current(handle, sw);
        snd_vela_pcm_sw_params_get_boundary(sw, &boundary);
        snd_vela_pcm_sw_params_set_start_threshold(handle, sw, buffer);
        snd_vela_pcm_sw_params_set_stop_threshold(handle, sw, buffer);
        snd_vela_pcm_sw_params_set_silence_size(handle, sw, boundary);
        snd_vela_pcm_sw_params_set_avail_min(handle, sw, period);
        ret = snd_vela_pcm_sw_params(handle, sw);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] sw_params fail %d\n", TAG, ret);
            goto err;
        }
    }

    audio_playback_t* pb = calloc(1, sizeof(*pb));

    if (!pb) {
        goto err;
    }

    pb->pcm = handle;
    pb->sample_rate = sample_rate;
    pb->channels = channels;
    pb->bits_per_sample = bits_per_sample;
    pb->stopped = 0;
    pb->volume = audio_pb_read_volume();

    syslog(LOG_INFO, "[%s] opened %s playback (%uHz %uch %ubit vol=%d%%)\n",
        TAG, AGENT_PLAYBACK_CARD, sample_rate, channels, bits_per_sample,
        pb->volume);
    return pb;

err:
    if (params) {
        snd_vela_pcm_hw_params_free(params);
    }

    if (handle) {
        snd_vela_pcm_close(handle);
    }

    return NULL;
}

int audio_playback_write(audio_playback_t* pb,
    const void* buf, size_t len)
{
    int16_t* scaled = NULL;
    const void* pcm_buf = buf;

    if (!pb || !pb->pcm || !buf || len == 0) {
        return -EINVAL;
    }

    /* AEC workaround: voice_channel_start() sets stopped to skip
     * remaining TTS chunks and prevent echo into the microphone. */
    if (pb->stopped) {
        return -ECANCELED;
    }

    int frame_bytes = (int)pb->channels * ((int)pb->bits_per_sample / 8);
    snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(len / (size_t)frame_bytes);
    snd_pcm_sframes_t n;
    int retries = 0;

    if (frames == 0) {
        return 0;
    }

    /* 2026-09-14：软音量——TTS 原为满幅直写，听感比音乐响很多。按
     * music_volume 做定点衰减（gain = vol/100*32768，与 dm_sound_write
     * 同曲线）+ 软限幅；不就地改调用方缓冲（WS 解码块可能复用），
     * 缩放进临时缓冲。vol>=100 时零拷贝直写。 */
    if (pb->volume < 100) {
        size_t nsamp = len / sizeof(int16_t);
        int gain = (pb->volume * 32768) / 100;
        scaled = (int16_t*)malloc(len);
        if (scaled) {
            const int16_t* src = (const int16_t*)buf;
            for (size_t i = 0; i < nsamp; i++) {
                int v = (src[i] * gain) >> 15;
                if (v > 30000) {
                    v = 30000 + (v - 30000) / 4;
                } else if (v < -30000) {
                    v = -30000 - (v + 30000) / 4;
                }
                scaled[i] = (int16_t)v;
            }
            pcm_buf = scaled;
        }
    }

    /* EAGAIN 短暂重试；EPIPE（xrun）prepare 恢复后重写 */
    for (;;) {
        n = snd_vela_pcm_writei(pb->pcm, pcm_buf, frames);
        if (n == -EAGAIN && retries < 10) {
            usleep(10 * 1000);
            retries++;
            continue;
        } else if (n == -EPIPE) {
            s_xrun_count++;
            syslog(LOG_WARNING, "[%s] xrun, recovering\n", TAG);
            snd_vela_pcm_prepare(pb->pcm);
            if (retries < 10) {
                retries++;
                continue;
            }
        }

        break;
    }

    /* 2026-11-02 P341：短写只计数，暂不补写（先取证再改行为）。 */
    if (n >= 0 && (snd_pcm_uframes_t)n < frames) {
        s_short_write_count++;
    }

    if (n < 0) {
        syslog(LOG_ERR, "[%s] writei fail %ld\n", TAG, (long)n);
        free(scaled);
        return (int)n;
    }

    size_t written = (size_t)n * (size_t)frame_bytes;

    pb->total_written += written;
    /* 2026-08-31 尾音修复：记录本次块最后样本（fade-out 起点电平，
     * 避免 close 时从非零电平硬切出爆音）。块内最后样本 = 交错最后声道。 */
    if (n > 0)
        pb->last_sample = ((const int16_t*)pcm_buf)[(size_t)n * pb->channels - 1];
    free(scaled);
    return (int)written;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (pb) {
        pb->stopped = 1;
        /* drop 立即停流，让可能阻塞中的 writei() 返回，
         * 与旧 media_player_stop 语义一致，任意线程可调。 */
        if (pb->pcm) {
            snd_vela_pcm_drop(pb->pcm);
        }
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    if (pb->pcm) {
        syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
            TAG, pb->total_written);
        /* P103：正常播完（stopped=0）用 drain 等缓冲播完再关，防尾部
         * 截断（TTS 最后一块还在 8192 帧≈170ms buffer 里就被 drop 丢
         * 掉 → 播报"立马断掉"）；被打断（stopped=1）才 drop 立即停。 */
        if (pb->stopped) {
            snd_vela_pcm_drop(pb->pcm);
        } else {
            /* 2026-08-31 尾音修复：drain 前补 fade-out（30ms 线性衰减到 0）。
             * close() 内部会无条件 snd_vela_pcm_drop（硬切 SETUP），若此刻
             * DAC 仍输出非零电平会产生"多一个残缺尾音"（爆音）；先淡出到 0
             * 再 drain/close，硬切发生在静音电平上。小块循环，勿大栈数组。 */
            int fade_frames = (int)(pb->sample_rate * 30 / 1000);
            int done = 0;
            unsigned int ch = pb->channels;
            while (done < fade_frames)
            {
                int16_t fade[512];
                int n = fade_frames - done;
                int max_frames = (int)sizeof(fade) / (2 * (int)ch);
                if (n > max_frames)
                    n = max_frames;
                for (int i = 0; i < n; i++)
                {
                    /* 从最后样本电平线性衰减到 0（防首帧跳变爆音） */
                    int32_t v = (int32_t)pb->last_sample *
                        (int32_t)(fade_frames - done - i) / (int32_t)fade_frames;
                    for (unsigned int c = 0; c < ch; c++)
                        fade[i * (int)ch + c] = (int16_t)v;
                }
                snd_vela_pcm_writei(pb->pcm, fade, n);
                done += n;
            }
            snd_vela_pcm_drain(pb->pcm);
        }
        snd_vela_pcm_close(pb->pcm);
        pb->pcm = NULL;
        syslog(LOG_INFO, "[%s] playback done\n", TAG);
    }

    free(pb);
}
