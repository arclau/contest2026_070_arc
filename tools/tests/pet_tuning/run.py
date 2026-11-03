#!/usr/bin/env python3
"""Compile actual pet_core and playback clock with isolated host dependencies."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
APP = ROOT / 'vendor/allwinnertech/apps/luncher_dm'
with tempfile.TemporaryDirectory(prefix='dm-pet-test-') as tmp:
    out = Path(tmp)
    for d in ('nuttx', 'lvgl', 'netutils'):
        (out / d).mkdir()
    (out / 'nuttx/config.h').write_text('')
    (out / 'lvgl/lvgl.h').write_text('''#pragma once
#include <stdint.h>
typedef void lv_obj_t;
typedef int lv_coord_t;
typedef struct { int unused; } lv_image_dsc_t;
uint32_t lv_tick_get(void);
#define LV_LOG_USER(...) ((void)0)
''')
    (out / 'netutils/cJSON.h').symlink_to(ROOT / 'apps/netutils/cjson/cJSON/cJSON.h')
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pet_core.h"
#include "pet_anim_clock.h"
uint32_t lv_tick_get(void) { return 100000; }
/* Tests never touch the real device save directory. */
static FILE *test_fopen(const char *path, const char *mode)
{ (void)path; (void)mode; return NULL; }
#define fopen test_fopen
#include "pet_core.c"
#undef fopen
const pet_frame_set_t g_cat_frames[PET_STATE_COUNT] = {0};
const pet_walk_set_t g_cat_walk_frames = {0};
static const char *last_bubble;
static void bubble(const char *text, int ticks)
{ (void)ticks; last_bubble = text; }
static const pet_view_ops_t ops = { .show_bubble = bubble };

int main(void)
{
    pet_anim_clock_t clock = {0};
    assert(pet_anim_clock_frame(&clock, 1, false, 359, 2, 4, 240, true) == 2);
    assert(pet_anim_clock_frame(&clock, 1, false, 598, 2, 4, 240, true) == 2);
    assert(pet_anim_clock_frame(&clock, 1, false, 599, 2, 4, 240, true) == 3);
    assert(pet_anim_clock_frame(&clock, 1, false, 1079, 2, 4, 240, true) == 2);
    /* Same frame window, different instance: restart and display first frame. */
    assert(pet_anim_clock_frame(&clock, 2, false, 1100, 2, 4, 240, true) == 2);
    assert(pet_anim_clock_frame(&clock, 2, true, 1190, 0, 11, 120, true) == 0);
    assert(pet_anim_clock_frame(&clock, 2, true, 1430, 0, 11, 120, true) == 2);
    assert(pet_anim_clock_frame(&clock, 3, false, 2000, 0, 11, 240, false) == 0);
    assert(pet_anim_clock_frame(&clock, 3, false, 9000, 0, 11, 240, false) == 11);
    clock.valid = false;
    assert(pet_anim_clock_frame(&clock, 4, false, UINT32_MAX-50, 0, 2, 120, true) == 0);
    assert(pet_anim_clock_frame(&clock, 4, false, 69, 0, 2, 120, true) == 1);
    puts("PASS playback: first frame, restart, elapsed time, wrap, one-shot hold");

    pet_default_new();
    pet_core_register_view(&ops);
    g_state = PET_STATE_IDLE;
    g_cur_behavior = &g_cat_idle[2];
    assert(!pet_core_can_walk());
    g_cur_behavior = &g_cat_idle[4];
    assert(pet_core_can_walk());
    for (int state = 0; state < PET_STATE_COUNT; ++state)
        if (state != PET_STATE_IDLE)
            { g_state = state; assert(!pet_core_can_walk()); }
    puts("PASS feedback: roam cannot replace non-neutral reactions");

    const dm_pet_state_t active[] = { PET_STATE_EAT, PET_STATE_PLAY,
                                    PET_STATE_TOUCH, PET_STATE_CELEBRATE };
    const dm_pet_event_t background[] = { PET_EVT_MUSIC_ON, PET_EVT_PRESENCE,
                 PET_EVT_WEATHER_CHANGE, PET_EVT_WATER_GOAL, PET_EVT_AI_CHAT };
    for (unsigned i = 0; i < sizeof(active)/sizeof(active[0]); ++i)
        for (unsigned j = 0; j < sizeof(background)/sizeof(background[0]); ++j)
            {
                g_state = active[i];
                uint32_t id = g_animation_id;
                pet_core_process_event(background[j], 61);
                assert(g_state == active[i] && g_animation_id == id);
            }
    puts("PASS priority: 20 background-event / interaction combinations");

    for (unsigned seed = 0; seed < 30; ++seed)
        {
            srand(seed); g_state = PET_STATE_IDLE;
            uint32_t id = g_animation_id;
            pet_core_process_event(PET_EVT_TAP_BODY, 0);
            assert(g_state == PET_STATE_TOUCH && g_animation_id != id);
            id = g_animation_id;
            pet_core_process_event(PET_EVT_LONG_PRESS, 0);
            assert(g_animation_id != id);
        }
    g_state = PET_STATE_IDLE;
    for (int i = 0; i < 100; ++i)
        {
            const pet_behavior_t *previous = g_cur_behavior;
            pet_select_behavior();
            assert(g_cur_behavior != previous);
        }
    puts("PASS interaction: body feedback, same-state restart, no adjacent repeats");
    g_state = PET_STATE_EAT;
    uint32_t completed = g_animation_id;
    pet_core_animation_done(completed);
    assert(g_state == PET_STATE_IDLE && g_animation_id != completed);
    pet_core_process_event(PET_EVT_TAP_HEAD, 0);
    pet_core_animation_done(completed);
    assert(g_state == PET_STATE_TOUCH);
    puts("PASS completion: one-shot returns idle; stale completion cannot cancel touch");


    const int codes[] = {1, 2, 61, 71, 45, 95};
    const char *expected[] = {"今天有云~", "今天有云~", "下雨了~",
                            "下雪了~", "雾蒙蒙的~", "打雷了~"};
    for (unsigned i = 0; i < sizeof(codes)/sizeof(codes[0]); ++i)
        {
            g_state = PET_STATE_IDLE; last_bubble = NULL;
            pet_core_process_event(PET_EVT_WEATHER_CHANGE, codes[i]);
            assert(last_bubble && strcmp(last_bubble, expected[i]) == 0);
        }
    g_state = PET_STATE_IDLE; last_bubble = NULL;
    pet_core_process_event(PET_EVT_WEATHER_CHANGE, 0);
    assert(g_state == PET_STATE_IDLE && !last_bubble);
    puts("PASS WMO: clouds, rain, snow, fog, storm, clear sky");

    pet_default_new(); g_state = PET_STATE_IDLE;
    g_pet.attrs.hunger = 0;
    for (int i = 0; i < 60; ++i) pet_update_attrs_tick();
    assert(g_pet.attrs.health == 100);
    g_pet.attrs.hunger = 90;
    for (int i = 0; i < 60; ++i) pet_update_attrs_tick();
    assert(g_pet.attrs.health == 99);
    puts("PASS health: full cat stays healthy; severe hunger reduces health");
    return 0;
}
'''
    (out / 'test.c').write_text(harness)
    binary = out / 'test'
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=undefined,address', '-g', '-I'+str(out),
                    '-I'+str(APP), str(out/'test.c'),
                    str(ROOT/'apps/netutils/cjson/cJSON/cJSON.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
