/* Variable-duration, per-instance playback. No UI or filesystem dependency. */
#ifndef DM_PET_PLAYBACK_H
#define DM_PET_PLAYBACK_H
#include "pet_actions.h"

typedef struct {
    uint32_t instance;
    uint32_t started_ms;
    pet_action_t action;
    bool valid;
    bool held;
    bool released;
    bool done_sent;
} pet_playback_t;

static inline unsigned pet_playback_frame(pet_playback_t *clock,
        pet_action_t action, uint32_t instance, uint32_t now, bool held,
        bool *completed)
{
    const pet_action_clip_t *clip=&g_pet_actions[action];
    *completed=false;
    if (!clock->valid || clock->instance!=instance || clock->action!=action) {
        *clock=(pet_playback_t){.instance=instance,.started_ms=now,
            .action=action,.valid=true,.held=held};
        return 0; /* The first frame always gets its full authored duration. */
    }
    uint32_t elapsed=now-clock->started_ms;
    if (clip->mode==PET_PLAY_HELD) {
        uint32_t intro=0;
        for (unsigned i=0;i<clip->hold_frame;i++) intro+=clip->frames[i].duration_ms;
        if (clock->held && !held && !clock->released) {
            uint32_t exit_offset=0;
            for (unsigned i=0;i<clip->release_start_frame;i++) exit_offset+=clip->frames[i].duration_ms;
            clock->started_ms=now-exit_offset;
            elapsed=exit_offset;
            clock->released=true;
        }
        clock->held=held;
        if (held && !clock->released && elapsed>=intro)
            elapsed=intro+(elapsed-intro)%clip->frames[clip->hold_frame].duration_ms;
    }
    if (clip->mode==PET_PLAY_LOOP) elapsed%=clip->duration_ms;
    if (elapsed>=clip->duration_ms) {
        if (!clock->done_sent) { *completed=true; clock->done_sent=true; }
        return clip->frame_count-1;
    }
    for (unsigned i=0;i<clip->frame_count;i++) {
        if (elapsed<clip->frames[i].duration_ms) return i;
        elapsed-=clip->frames[i].duration_ms;
    }
    return clip->frame_count-1;
}
#endif
