#ifndef DESKMATE_PET_ANIM_CLOCK_H
#define DESKMATE_PET_ANIM_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Playback time belongs to an instance, never to the global UI timer phase. */
typedef struct
{
    bool valid;
    bool walking;
    uint32_t instance;
    uint32_t started_ms;
} pet_anim_clock_t;

static inline int pet_anim_clock_frame(pet_anim_clock_t *clock,
                                      uint32_t instance, bool walking,
                                      uint32_t now_ms, int first, int last,
                                      uint32_t frame_ms, bool loop)
{
    if (!clock->valid || clock->instance != instance ||
        clock->walking != walking)
        {
            clock->valid = true;
            clock->instance = instance;
            clock->walking = walking;
            clock->started_ms = now_ms;
        }

    if (last < first || frame_ms == 0) return first;
    uint32_t count = (uint32_t)(last - first + 1);
    uint32_t step = (uint32_t)(now_ms - clock->started_ms) / frame_ms;
    if (loop) step %= count;
    else if (step >= count) step = count - 1;
    return first + (int)step;
}

#endif
