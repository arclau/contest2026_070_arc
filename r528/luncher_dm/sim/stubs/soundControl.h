/* SIM stub: soundControl.h + adecoder.h 类型 — goldfish/luncher_dm 用 */
#ifndef __SIM_SOUND_CONTROL_H
#define __SIM_SOUND_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cedar_raw_data {
    int nChannels;
    int nSamplerate;
    int nBitpersample;
} CdxPlaybkCfg;

typedef struct XAudioPlaybackRate {
    float mSpeed;
    float mPitch;
    int   mAudioMode;
} XAudioPlaybackRate;

typedef struct SoundCtrl SoundCtrl;
typedef struct SoundControlOpsS SoundControlOpsT;

struct SoundControlOpsS {
    void (*destroy)(SoundCtrl *s);
    void (*setFormat)(SoundCtrl *s, CdxPlaybkCfg *cfg);
    int  (*start)(SoundCtrl *s);
    int  (*stop)(SoundCtrl *s);
    int  (*pause)(SoundCtrl *s);
    int  (*write)(SoundCtrl *s, void *pData, int nDataSize);
    int  (*reset)(SoundCtrl *s);
    int  (*getCachedTime)(SoundCtrl *s);
    int  (*getFrameCount)(SoundCtrl *s);
    int  (*setPlaybackRate)(SoundCtrl *s, const XAudioPlaybackRate *rate);
    int  (*control)(SoundCtrl *s, int cmd, void *para);
};

struct SoundCtrl {
    struct SoundControlOpsS *ops;
};

#ifndef CDX_CHECK
#define CDX_CHECK(e) do { } while (0)
#endif

#ifdef __cplusplus
}
#endif
#endif
