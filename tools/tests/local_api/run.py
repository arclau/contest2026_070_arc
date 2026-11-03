#!/usr/bin/env python3
"""Exercise production dispatch/callbacks with counted cloud and device boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
ui = (ROOT / 'vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c').read_text()
voice = (ROOT / 'packages/ai_agent/src/voice/voice_channel.c').read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
typedef const char lv_event_t;
static int actions, tones, cloud, net_checks, busy_checks, events;
static int online, busy, speaking;
static time_t test_time(time_t *t) {if(t) *t=1704126600; return 1704126600;}
#define time test_time
static bool music_playing, ai_voice_active;
static void *ai_status_lbl;
static char last[192], event_cmd[64];
static struct timespec s_asr_done_ts;
#define PET_BUBBLE_DURATION_TICKS 100
static void reset_idle_timer(void) {}
static void *lv_event_get_user_data(lv_event_t *e) {return (void *)e;}
static void lv_label_set_text(void *p, const char *t) {(void)p; (void)t;}
static void ai_chat_append(const char *who, const char *text) {
    (void)who; snprintf(last, sizeof(last), "%s", text);
}
static void ai_chat_refresh(void) {}
static void pet_view_show_bubble(const char *text, int ticks) {
    (void)text; (void)ticks;
}
static int dm_ai_voice_speaking(void) {return speaking;}
static void dm_tone_play(const char *name) {
    assert(!strcmp(name, "bg_reward_std")); tones++;
}
static int dm_net_wifi_connected(void) {net_checks++; return online;}
static int dm_ai_busy(void) {busy_checks++; return busy;}
static int dm_ai_ask(const char *q) {(void)q; cloud++; return 0;}
static void dm_audio_fg_acquire(void) {}
static void device(const char *arg, char *ack, size_t cap) {
    actions++; snprintf(ack, cap, "%s", arg);
}
#define dm_cmd_app device
#define dm_cmd_music device
#define dm_cmd_volume device
#define dm_cmd_brightness device
#define dm_cmd_pet device
#define dm_cmd_scene device
#define dm_cmd_health device
#define dm_cmd_alarm device
static void ws_server_broadcast_voice_evt(const char *kind, const char *text) {
    events++;
    if (!strcmp(kind,"cmd")) snprintf(event_cmd,sizeof(event_cmd),"%s",text);
    else assert(!strcmp(kind,"pet_idle"));
}
'''
struct_start = ui.index('struct dm_local_cmd {')
table_start = ui.index('static const struct dm_local_cmd g_dm_local_cmds[]')
code = prefix + ui[struct_start:ui.index('};', struct_start) + 2] + '\n'
code += ui[table_start:ui.index('};', table_start) + 2] + '\n'
code += function(ui, 'int dm_local_cmd_dispatch(const char *cmd)\n{')
code += function(ui, 'static void ai_chip_click_cb(')
code += function(ui, 'static void ai_local_click_cb(')
code += function(voice, 'static void voice_local_cmd_run(')
code += r'''
int main(void) {
    const char *cmds[] = {"app:calendar", "music:play", "volume:+10",
        "brightness:60", "pet:sit", "scene:sleep", "health:water", "alarm:list"};
    for (unsigned i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++) {
        assert(dm_local_cmd_dispatch(cmds[i]) == 1);
        assert(actions == (int)i+1);
        voice_local_cmd_run(cmds[i]);
        assert(!strcmp(event_cmd,cmds[i]));
        assert(events == ((int)i+1)*2);
    }
    assert(tones == 3 && cloud == 0);
    assert(dm_local_cmd_dispatch(NULL) == 0);
    assert(dm_local_cmd_dispatch("") == 0);
    assert(dm_local_cmd_dispatch("unknown:test") == 0);
    assert(actions == 8);
    music_playing = true;
    dm_local_cmd_dispatch("volume:+10");
    assert(tones == 3); /* Never stop music just to acknowledge volume. */
    music_playing = false; ai_voice_active = true;
    dm_local_cmd_dispatch("brightness:60");
    assert(tones == 3);
    ai_voice_active = false; speaking = 1;
    dm_local_cmd_dispatch("alarm:list");
    assert(tones == 3);
    for (online=0; online<2; online++) for (busy=0; busy<2; busy++) {
        int before = actions;
        ai_chip_click_cb("播放音乐");
        ai_chip_click_cb("现在几点了");
        assert(!strcmp(last,"现在是00点30分"));
        ai_local_click_cb("pet:sleep");
        ai_local_click_cb("pet:walk:300");
        ai_local_click_cb("pet:stop");
        assert(actions == before+4);
        assert(cloud == 0 && net_checks == 0 && busy_checks == 0);
    }
    online=0; busy=0;
    ai_chip_click_cb("讲个笑话"); assert(cloud == 0);
    online=1; busy=1;
    ai_chip_click_cb("讲个笑话"); assert(cloud == 0);
    busy=0;
    ai_chip_click_cb("讲个笑话"); assert(cloud == 1);
    puts("PASS: 8 command families, offline/busy callbacks, local feedback, cloud boundary");
}
'''
with tempfile.TemporaryDirectory(prefix='dm-local-api-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
