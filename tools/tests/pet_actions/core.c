#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "pet_core.h"
#include "pet_playback.h"
static uint32_t now_ms=100000;
uint32_t lv_tick_get(void) { return now_ms; }
/* These interaction tests model an active user, so idle naps do not start. */
uint32_t lv_display_get_inactive_time(void *display) { (void)display; return 0; }
/* Keep host tests away from the device save and all existing user data. */
static FILE *isolated_fopen(const char *path,const char *mode) { (void)path;(void)mode;return NULL; }
#define fopen isolated_fopen
#include "pet_core.c"
#undef fopen
static const char *last_bubble;
static void bubble(const char *text,int ticks) { (void)ticks;last_bubble=text; }
static const pet_view_ops_t ops={.show_bubble=bubble};
static void reset_model(void)
{
    pet_default_new();g_state=PET_STATE_IDLE;g_action=PET_ACTION_IDLE_SIT;
    g_after_transition=PET_ACTION_COUNT;g_touch_held=false;g_voice_active=false;
    g_cur_behavior=NULL;g_last_feed_ms=0;g_behavior_ticks_left=20;
    g_play_active=false;g_play_ticks=0;g_play_rest_until=0;
    pet_core_register_view(&ops);now_ms=100000;
}
int main(void)
{
    for (int a=0;a<PET_ACTION_COUNT;a++) {
        const pet_action_clip_t *clip=&g_pet_actions[a];
        pet_playback_t clock={0};bool done;
        assert(pet_playback_frame(&clock,a,1,1000,false,&done)==0 && !done);
        uint32_t elapsed=0;
        for (unsigned f=0;f<clip->frame_count;f++) {
            assert(pet_playback_frame(&clock,a,1,1000+elapsed,false,&done)==f && !done);
            elapsed+=clip->frames[f].duration_ms;
            assert(pet_playback_frame(&clock,a,1,1000+elapsed-1,false,&done)==f && !done);
        }
        assert(elapsed==clip->duration_ms);
        unsigned end=pet_playback_frame(&clock,a,1,1000+elapsed,false,&done);
        assert(clip->mode==PET_PLAY_LOOP ? end==0 && !done : end==clip->frame_count-1 && done);
        pet_playback_frame(&clock,a,1,1000+elapsed+1,false,&done);assert(!done);
        assert(pet_playback_frame(&clock,a,2,1000+elapsed+2,false,&done)==0 && !done);
    }
    puts("PASS authored playback: all 18 clips, every duration boundary, restart and one-shot completion");
    pet_playback_t held={0};bool done;
    const pet_action_clip_t *n=&g_pet_actions[PET_ACTION_PET_NUZZLE];
    uint32_t intro=0;for (unsigned i=0;i<n->hold_frame;i++) intro+=n->frames[i].duration_ms;
    pet_playback_frame(&held,PET_ACTION_PET_NUZZLE,4,10,true,&done);
    assert(pet_playback_frame(&held,PET_ACTION_PET_NUZZLE,4,10+intro+n->frames[n->hold_frame].duration_ms*50,true,&done)==n->hold_frame && !done);
    assert(pet_playback_frame(&held,PET_ACTION_PET_NUZZLE,4,30000,false,&done)==n->release_start_frame && !done);
    pet_playback_frame(&held,PET_ACTION_PET_NUZZLE,4,35000,false,&done);assert(done);
    pet_playback_t wrap={0};
    pet_playback_frame(&wrap,PET_ACTION_BLINK,8,UINT32_MAX-50,false,&done);
    assert(pet_playback_frame(&wrap,PET_ACTION_BLINK,8,49,false,&done)==1);
    puts("PASS held interaction: stable hold, release exit, no duplicate completion, tick rollover");

    reset_model();pet_transition_to(PET_STATE_SLEEP,"test");
    assert(g_action==PET_ACTION_SLEEP_ENTER);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_SLEEP_LOOP);
    int hunger=g_pet.attrs.hunger;pet_core_process_event(PET_EVT_FEED_FISH,0);
    assert(g_action==PET_ACTION_WAKE && g_after_transition==PET_ACTION_EAT);
    assert(g_pet.attrs.hunger==hunger-30);
    uint32_t stale=g_animation_id;pet_core_animation_done(stale);
    assert(g_action==PET_ACTION_EAT);
    pet_core_animation_done(stale);assert(g_action==PET_ACTION_EAT);
    for (int i=0;i<100;i++) pet_core_behavior_tick();
    assert(g_action==PET_ACTION_EAT); /* 500ms business timer cannot truncate the clip. */
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_IDLE_SIT);
    puts("PASS sleep and feed: enter/loop/wake/eat/idle, actual completion, stale-instance rejection");

    reset_model();int growth=g_pet.attrs.growth;
    pet_core_process_event(PET_EVT_LONG_PRESS,1);
    assert(g_action==PET_ACTION_PET_NUZZLE && g_touch_held);
    pet_core_process_event(PET_EVT_TOUCH_RELEASE,0);
    assert(!g_touch_held && g_pet.attrs.growth==growth+1);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_IDLE_SIT);
    assert(pet_core_begin_walk() && g_action==PET_ACTION_TURN);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_WALK);
    pet_core_finish_walk(g_animation_id-1);assert(g_action==PET_ACTION_WALK);
    pet_core_finish_walk(g_animation_id);assert(g_action==PET_ACTION_SETTLE);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_IDLE_SIT);
    pet_core_process_event(PET_EVT_SIT_ALERT,0);assert(g_action==PET_ACTION_STRETCH);
    puts("PASS interaction mapping: long-press counted once, release, turn/walk/settle and stretch reminder");

    reset_model();pet_core_process_event(PET_EVT_TAP_BODY,0);
    assert(g_action==PET_ACTION_NOTICE_USER && !g_touch_held);
    assert(strstr(last_bubble,"肚子"));
    reset_model();pet_core_process_event(PET_EVT_LONG_PRESS,1);
    growth=g_pet.attrs.growth;
    pet_core_process_event(PET_EVT_PET_RELAX,0);
    assert(g_action==PET_ACTION_BLINK && g_touch_held);
    pet_core_animation_done(g_animation_id);
    assert(g_action==PET_ACTION_PET_NUZZLE && g_touch_held);
    pet_core_process_event(PET_EVT_TOUCH_RELEASE,0);
    assert(!g_touch_held && g_pet.attrs.growth==growth);
    reset_model();pet_core_process_event(PET_EVT_LONG_PRESS,2);
    assert(g_action==PET_ACTION_NOTICE_USER && !g_touch_held);
    puts("PASS petting: head nuzzle/relaxed blink/held return/release, belly refusal, no repeat rewards");

    reset_model();int energy=g_pet.attrs.energy;
    pet_core_process_event(PET_EVT_PLAY_BEGIN,0);
    assert(pet_core_playing() && g_action==PET_ACTION_NOTICE_USER);
    pet_core_animation_done(g_animation_id);assert(g_state==PET_STATE_PLAY && !pet_core_can_walk());
    pet_core_process_event(PET_EVT_PLAY_PHASE,PET_PLAY_CHASE);assert(g_action==PET_ACTION_TURN);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_WALK);
    pet_core_process_event(PET_EVT_PLAY_PHASE,PET_PLAY_POUNCE);assert(g_action==PET_ACTION_HAPPY);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_IDLE_SIT && pet_core_playing());
    pet_core_process_event(PET_EVT_PLAY_PHASE,PET_PLAY_CHASE);
    pet_core_animation_done(g_animation_id);
    pet_core_process_event(PET_EVT_PLAY_PHASE,PET_PLAY_LOOK);assert(g_action==PET_ACTION_SETTLE);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_NOTICE_USER);
    pet_core_animation_done(g_animation_id);assert(g_state==PET_STATE_PLAY);
    for (int i=0;i<60;i++) {now_ms+=500;pet_core_behavior_tick();}
    assert(!pet_core_playing() && g_pet.attrs.energy==energy-15);
    growth=g_pet.attrs.growth;
    pet_core_process_event(PET_EVT_PLAY_END,0);assert(growth==g_pet.attrs.growth);
    pet_core_process_event(PET_EVT_PLAY_BEGIN,0);assert(!pet_core_playing());
    now_ms+=15001;pet_core_process_event(PET_EVT_PLAY_BEGIN,0);assert(pet_core_playing());
    pet_core_process_event(PET_EVT_FEED_FISH,0);assert(!pet_core_playing());
    pet_core_process_event(PET_EVT_PLAY_PHASE,PET_PLAY_CHASE);assert(g_state==PET_STATE_EAT);
    reset_model();g_pet.attrs.energy=19;
    pet_core_process_event(PET_EVT_PLAY_BEGIN,0);assert(!pet_core_playing());
    g_pet.attrs.energy=21;pet_core_process_event(PET_EVT_PLAY_BEGIN,0);
    for (int i=0;i<24;i++) pet_core_behavior_tick();
    assert(!pet_core_playing() && g_pet.attrs.energy==15);
    reset_model();now_ms=UINT32_MAX-100;
    pet_core_process_event(PET_EVT_PLAY_BEGIN,0);assert(pet_core_playing());
    pet_core_process_event(PET_EVT_PLAY_END,1);
    now_ms=10;pet_core_process_event(PET_EVT_PLAY_BEGIN,0);assert(!pet_core_playing());
    now_ms=15000;pet_core_process_event(PET_EVT_PLAY_BEGIN,0);assert(pet_core_playing());
    puts("PASS play: notice/turn/chase/catch/settle, release, energy cost, 30s limit, rest/wrap, stale phases, low energy");

    const dm_pet_state_t active[]={PET_STATE_EAT,PET_STATE_PLAY,PET_STATE_TOUCH,PET_STATE_CELEBRATE};
    const dm_pet_event_t env[]={PET_EVT_MUSIC_ON,PET_EVT_PRESENCE,PET_EVT_WEATHER_CHANGE,PET_EVT_WATER_GOAL,PET_EVT_AI_CHAT};
    for (unsigned i=0;i<4;i++) for (unsigned j=0;j<5;j++) {
        reset_model();g_state=active[i];pet_select_behavior();uint32_t id=g_animation_id;
        pet_core_process_event(env[j],61);assert(g_state==active[i] && id==g_animation_id);
    }
    reset_model();pet_core_process_event(PET_EVT_VOICE_PHASE,1);assert(g_action==PET_ACTION_LISTEN);
    pet_core_process_event(PET_EVT_VOICE_PHASE,2);assert(g_action==PET_ACTION_THINK);
    pet_core_process_event(PET_EVT_VOICE_PHASE,3);assert(g_action==PET_ACTION_SPEAK);
    pet_core_process_event(PET_EVT_TAP_HEAD,0);assert(g_action==PET_ACTION_PET_NUZZLE);
    pet_core_process_event(PET_EVT_VOICE_PHASE,2);assert(g_action==PET_ACTION_PET_NUZZLE);
    pet_core_animation_done(g_animation_id);assert(g_action==PET_ACTION_THINK);
    pet_core_process_event(PET_EVT_VOICE_PHASE,0);assert(!g_voice_active);
    pet_core_process_event(PET_EVT_VOICE_PHASE,3);now_ms+=46000;pet_core_behavior_tick();assert(!g_voice_active);
    puts("PASS priority: 20 environment/interaction pairs, real voice phases, interruption/resume and timeout");

    reset_model();last_bubble=NULL;pet_core_process_event(PET_EVT_WEATHER_CHANGE,2);
    assert(last_bubble && strstr(last_bubble,"云"));
    last_bubble=NULL;pet_core_process_event(PET_EVT_WEATHER_CHANGE,61);
    assert(last_bubble && strstr(last_bubble,"雨"));
    g_pet.attrs.hunger=90;g_pet.attrs.health=100;
    for (int i=0;i<60;i++) pet_update_attrs_tick();assert(g_pet.attrs.health==99);
    g_pet.attrs.hunger=10;int health=g_pet.attrs.health;
    for (int i=0;i<60;i++) pet_update_attrs_tick();assert(g_pet.attrs.health==health);
    for (int i=0;i<200;i++) {g_state=PET_STATE_IDLE;pet_core_animation_done(g_animation_id);pet_select_behavior();assert((unsigned)g_action<PET_ACTION_COUNT);}
    puts("PASS retained behavior: WMO weather, high-hunger health decay and generated action bounds");
    return 0;
}
