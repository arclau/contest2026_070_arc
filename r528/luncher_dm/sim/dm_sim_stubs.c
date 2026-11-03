/* dm_sim_stubs.c — goldfish/luncher_dm 板级 API 空实现
 *
 * 覆盖：XPlayer / SoundCtrl 兼容 / aw-alsa pcm / sunxi hal_pwm / hal_ledc。
 * 音乐播放、背光、LED 在模拟器上均为 no-op，UI 流程可走通。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xplayer.h"
#include "soundControl.h"
#include "pcm.h"
#include "sunxi_hal_pwm.h"
#include "sunxi_hal_ledc.h"

/* ---------------- XPlayer ---------------- */

struct PlayerContext {
    int dummy;
    int pos;
    int dur;
};

XPlayer *XPlayerCreate(void)
{
    XPlayer *p = calloc(1, sizeof(*p));
    if (p) {
        p->dur = 180000;
    }
    return p;
}

void XPlayerDestroy(XPlayer *p)
{
    free(p);
}

int XPlayerSetNotifyCallback(XPlayer *p, XPlayerNotifyCallback notifier, void *pUser)
{
    (void)p; (void)notifier; (void)pUser;
    return 0;
}

int XPlayerInitCheck(XPlayer *p) { (void)p; return 0; }

int XPlayerSetDataSourceUrl(XPlayer *p, const char *pUrl,
                            void *httpService, const void *pHeaders)
{
    (void)p; (void)pUrl; (void)httpService; (void)pHeaders;
    return 0;
}

int XPlayerPrepareAsync(XPlayer *p) { (void)p; return 0; }
int XPlayerStart(XPlayer *p) { (void)p; return 0; }
int XPlayerStop(XPlayer *p) { (void)p; return 0; }
int XPlayerPause(XPlayer *p) { (void)p; return 0; }

int XPlayerGetCurrentPosition(XPlayer *p, int *msec)
{
    if (p && msec) {
        p->pos += 250;
        if (p->pos > p->dur) p->pos = p->dur;
        *msec = p->pos;
    }
    return 0;
}

int XPlayerGetDuration(XPlayer *p, int *msec)
{
    if (p && msec) *msec = p->dur;
    return 0;
}

int XPlayerReset(XPlayer *p)
{
    if (p) p->pos = 0;
    return 0;
}

void XPlayerSetAudioSink(XPlayer *p, const SoundCtrl *audioSink)
{
    (void)p; (void)audioSink;
}

/* ---------------- aw-alsa pcm ---------------- */

struct _snd_pcm { int dummy; };
struct _snd_pcm_hw_params { int dummy; };
/* struct _snd_pcm_sw_params 定义在 pcm.h（deskmate_ui.c 的 alloca 需 sizeof） */

int snd_vela_pcm_open(snd_pcm_t **pcm, const char *name,
                      snd_pcm_stream_t stream, int mode)
{
    (void)name; (void)stream; (void)mode;
    *pcm = calloc(1, sizeof(snd_pcm_t));
    return *pcm ? 0 : -1;
}

int snd_vela_pcm_close(snd_pcm_t *pcm) { free(pcm); return 0; }
int snd_vela_pcm_prepare(snd_pcm_t *pcm) { (void)pcm; return 0; }
int snd_vela_pcm_drain(snd_pcm_t *pcm) { (void)pcm; return 0; }
int snd_vela_pcm_pause(snd_pcm_t *pcm, int enable) { (void)pcm; (void)enable; return 0; }
int snd_vela_pcm_state(snd_pcm_t *pcm) { (void)pcm; return 0; }

snd_pcm_sframes_t snd_vela_pcm_writei(snd_pcm_t *pcm, const void *buffer,
                                      snd_pcm_uframes_t size)
{
    (void)pcm; (void)buffer;
    return (snd_pcm_sframes_t)size;
}

int snd_vela_pcm_hw_params_malloc(snd_pcm_hw_params_t **ptr)
{
    *ptr = calloc(1, sizeof(snd_pcm_hw_params_t));
    return *ptr ? 0 : -1;
}
void snd_vela_pcm_hw_params_free(snd_pcm_hw_params_t *obj) { free(obj); }
int snd_vela_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{ (void)pcm; (void)params; return 0; }
int snd_vela_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{ (void)pcm; (void)params; return 0; }
int snd_vela_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                      snd_pcm_access_t access)
{ (void)pcm; (void)params; (void)access; return 0; }
int snd_vela_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                      snd_pcm_format_t val)
{ (void)pcm; (void)params; (void)val; return 0; }
int snd_vela_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                        unsigned int val)
{ (void)pcm; (void)params; (void)val; return 0; }
int snd_vela_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                    unsigned int val, int dir)
{ (void)pcm; (void)params; (void)val; (void)dir; return 0; }
int snd_vela_pcm_hw_params_set_period_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                           snd_pcm_uframes_t val, int dir)
{ (void)pcm; (void)params; (void)val; (void)dir; return 0; }
int snd_vela_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                           snd_pcm_uframes_t val)
{ (void)pcm; (void)params; (void)val; return 0; }

int snd_vela_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{ (void)pcm; (void)params; return 0; }
int snd_vela_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{ (void)pcm; (void)params; return 0; }
int snd_vela_pcm_sw_params_get_boundary(snd_pcm_sw_params_t *params,
                                        snd_pcm_uframes_t *val)
{ (void)params; if (val) *val = 8192; return 0; }
int snd_vela_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                               snd_pcm_uframes_t val)
{ (void)pcm; (void)params; (void)val; return 0; }
int snd_vela_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                              snd_pcm_uframes_t val)
{ (void)pcm; (void)params; (void)val; return 0; }
int snd_vela_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                            snd_pcm_uframes_t val)
{ (void)pcm; (void)params; (void)val; return 0; }
int snd_vela_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
                                         snd_pcm_uframes_t val)
{ (void)pcm; (void)params; (void)val; return 0; }

/* ---------------- sunxi HAL ---------------- */

pwm_status_t hal_pwm_control(int channel, struct pwm_config *config_pwm)
{
    (void)channel; (void)config_pwm;
    return 0;
}

int hal_ledc_init(void) { return 0; }
int hal_ledc_deinit(void) { return 0; }
int sunxi_set_led_brightness(int led_num, unsigned int brightness)
{ (void)led_num; (void)brightness; return 0; }
