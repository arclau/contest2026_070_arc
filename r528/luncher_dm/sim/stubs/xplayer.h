/* SIM stub: xplayer.h — goldfish/luncher_dm 用，XPlayer 全部空实现 */
#ifndef __SIM_XPLAYER_H
#define __SIM_XPLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SoundCtrl SoundCtrl;
typedef struct LayerCtrl LayerCtrl;
typedef struct SubCtrl SubCtrl;
typedef struct Deinterlace Deinterlace;
typedef struct MediaInfo MediaInfo;
typedef struct HdcpOpsS HdcpOpsS;
typedef struct XAudioPlaybackRate XAudioPlaybackRate;
typedef struct AwBufferingSettings AwBufferingSettings;

/* 与真实 xplayer.h 的 enum MediaEventType 对齐（仅取 luncher_dm 用到的值） */
enum MediaEventType {
    AWPLAYER_MEDIA_NOP = 0,          /* MEDIA_EVENT_VALID_RANGE_MIN */
    AWPLAYER_MEDIA_PREPARED,
    AWPLAYER_MEDIA_PLAYBACK_COMPLETE,
    AWPLAYER_MEDIA_BUFFERING_UPDATE,
    AWPLAYER_MEDIA_SEEK_COMPLETE,
    AWPLAYER_MEDIA_SET_VIDEO_SIZE,
    AWPLAYER_MEDIA_STARTED,
    AWPLAYER_MEDIA_PAUSED,
    AWPLAYER_MEDIA_STOPPED,
    AWPLAYER_MEDIA_SKIPPED,
    AWPLAYER_MEDIA_TIMED_TEXT,
    AWPLAYER_MEDIA_ERROR,
    AWPLAYER_MEDIA_INFO,
};

typedef int (*XPlayerNotifyCallback)(void *pUser, int msg, int ext1, void *para);

typedef struct PlayerContext XPlayer;

XPlayer *XPlayerCreate(void);
void XPlayerDestroy(XPlayer *p);
int XPlayerSetNotifyCallback(XPlayer *p, XPlayerNotifyCallback notifier, void *pUser);
int XPlayerInitCheck(XPlayer *p);
int XPlayerSetDataSourceUrl(XPlayer *p, const char *pUrl,
                            void *httpService, const void *pHeaders);
int XPlayerPrepareAsync(XPlayer *p);
int XPlayerStart(XPlayer *p);
int XPlayerStop(XPlayer *p);
int XPlayerPause(XPlayer *p);
int XPlayerGetCurrentPosition(XPlayer *p, int *msec);
int XPlayerGetDuration(XPlayer *p, int *msec);
int XPlayerReset(XPlayer *p);
void XPlayerSetAudioSink(XPlayer *p, const SoundCtrl *audioSink);

#ifdef __cplusplus
}
#endif
#endif
