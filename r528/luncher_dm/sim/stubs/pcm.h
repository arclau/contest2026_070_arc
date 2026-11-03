/* SIM stub: pcm.h (aw-alsa-lib) — goldfish/luncher_dm 用 */
#ifndef __SIM_PCM_H
#define __SIM_PCM_H

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
/* 完整定义：deskmate_ui.c 的 snd_pcm_sw_params_alloca 需要 sizeof（真实库为不透明类型） */
struct _snd_pcm_sw_params { int dummy; };
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

typedef enum _snd_pcm_stream {
    SND_VELA_PCM_STREAM_PLAYBACK = 0,
    SND_VELA_PCM_STREAM_CAPTURE,
    SND_VELA_PCM_STREAM_LAST = SND_VELA_PCM_STREAM_CAPTURE
} snd_pcm_stream_t;

typedef enum _snd_pcm_access {
    SND_PCM_ACCESS_MMAP_INTERLEAVED = 0,
    SND_PCM_ACCESS_MMAP_NONINTERLEAVED,
    SND_PCM_ACCESS_MMAP_COMPLEX,
    SND_PCM_ACCESS_RW_INTERLEAVED,
    SND_PCM_ACCESS_RW_NONINTERLEAVED,
    SND_PCM_ACCESS_LAST = SND_PCM_ACCESS_RW_NONINTERLEAVED
} snd_pcm_access_t;

typedef enum _snd_pcm_format {
    SND_PCM_FORMAT_UNKNOWN = -1,
    SND_PCM_FORMAT_S8 = 0,
    SND_PCM_FORMAT_U8,
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_S16_BE,
    SND_PCM_FORMAT_U16_LE,
    SND_PCM_FORMAT_U16_BE,
    SND_PCM_FORMAT_S16 = SND_PCM_FORMAT_S16_LE,
} snd_pcm_format_t;

#define snd_pcm_sw_params_alloca(ptr) \
    do { *(ptr) = (snd_pcm_sw_params_t *)alloca(sizeof(snd_pcm_sw_params_t)); \
         memset(*(ptr), 0, sizeof(snd_pcm_sw_params_t)); } while (0)

int snd_vela_pcm_open(snd_pcm_t **pcm, const char *name,
                      snd_pcm_stream_t stream, int mode);
int snd_vela_pcm_close(snd_pcm_t *pcm);
int snd_vela_pcm_prepare(snd_pcm_t *pcm);
int snd_vela_pcm_drain(snd_pcm_t *pcm);
int snd_vela_pcm_pause(snd_pcm_t *pcm, int enable);
int snd_vela_pcm_state(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_vela_pcm_writei(snd_pcm_t *pcm, const void *buffer,
                                      snd_pcm_uframes_t size);

int snd_vela_pcm_hw_params_malloc(snd_pcm_hw_params_t **ptr);
void snd_vela_pcm_hw_params_free(snd_pcm_hw_params_t *obj);
int snd_vela_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_vela_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_vela_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                      snd_pcm_access_t access);
int snd_vela_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                      snd_pcm_format_t val);
int snd_vela_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                        unsigned int val);
int snd_vela_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                    unsigned int val, int dir);
int snd_vela_pcm_hw_params_set_period_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                           snd_pcm_uframes_t val, int dir);
int snd_vela_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                           snd_pcm_uframes_t val);

int snd_vela_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
int snd_vela_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
int snd_vela_pcm_sw_params_get_boundary(snd_pcm_sw_params_t *params,
                                        snd_pcm_uframes_t *val);
int snd_vela_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                               snd_pcm_uframes_t val);
int snd_vela_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                              snd_pcm_uframes_t val);
int snd_vela_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                            snd_pcm_uframes_t val);
int snd_vela_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                         snd_pcm_uframes_t val);

#ifdef __cplusplus
}
#endif
#endif
