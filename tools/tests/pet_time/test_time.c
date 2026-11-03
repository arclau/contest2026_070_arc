#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include "pet_view.h"
#include "pet_core.h"
/* Isolate persistent save I/O while retaining the real model. */
static FILE *isolated_fopen(const char *p,const char *m) { (void)p;(void)m;return NULL; }
#define fopen isolated_fopen
#include "pet_core.c"
#undef fopen
#undef PET_ANIM_TICK_MS
#undef LV_LOG_USER
#define LV_LOG_USER(...) do { if (0) fprintf(stderr,__VA_ARGS__); } while (0)
#include "dm_pet.c"
/* Rendering is isolated; the core, facade, event queue and LVGL timers are real. */
void pet_view_create(lv_obj_t *p) { (void)p; }
void pet_view_destroy(void) { pet_core_unregister_view(); }
bool pet_view_reuse(void) { return false; }
void pet_view_anim_tick(void) {}
void pet_view_bubble_tick(void) {}
void pet_view_show_bubble(const char *s,int t) { (void)s;(void)t; }
void pet_view_refresh_sprite(void) {}
void pet_view_suspend(void) {}
void pet_view_set_roam(int a,int b,int c,int d) { (void)a;(void)b;(void)c;(void)d; }
void pet_view_clear_roam(void) {}
void pet_view_stroll_start(int s) { (void)s; }
void pet_view_stroll_stop(void) {}
bool pet_view_stroll_active(void) { return false; }

static void test_idle_cb(lv_timer_t *timer) { (void)timer; }
static void minute(void) {
    lv_tick_inc(60000); lv_timer_handler();
    lv_tick_inc(100); lv_timer_handler();
}
int main(void) {
    lv_init();
    pet_default_new();
    g_anim_timer=lv_timer_create(test_idle_cb,33,NULL);
    g_behavior_timer=lv_timer_create(test_idle_cb,500,NULL);
    g_time_timer=lv_timer_create(pet_time_cb,60000,NULL);
    g_poll_timer=lv_timer_create(pet_poll_cb,100,NULL);
    g_save_timer=lv_timer_create(test_idle_cb,60000,NULL);
    dm_pet_on_event(PET_EVT_WAKE_UP);
    assert(g_anim_timer->paused && g_behavior_timer->paused);
    assert(!g_time_timer->paused && !g_poll_timer->paused && !g_save_timer->paused);
    for (int i=0;i<60;i++) minute();
    assert(g_pet.attrs.hunger==51);
    puts("PASS main screen: actual LVGL timers advance hunger for 60 minutes");
    dm_pet_on_event(PET_EVT_SLEEP);
    assert(!g_anim_timer->paused && !g_behavior_timer->paused && !g_time_timer->paused);
    for (int i=0;i<60;i++) minute();
    assert(g_pet.attrs.hunger==52);
    puts("PASS return to lock screen: time continues without double counting");
    pet_core_reset();
    for (int i=0;i<59;i++) pet_update_attrs_tick();
    pet_core_reset();
    pet_update_attrs_tick();
    assert(g_pet.attrs.hunger==50 && g_pet.attrs.energy==80 && g_pet.attrs.happiness==60);
    for (int i=1;i<60;i++) pet_update_attrs_tick();
    assert(g_pet.attrs.hunger==51);
    puts("PASS reset: new pet starts a fresh full hunger interval");
    pet_core_reset();
    g_state=PET_STATE_SLEEP;
    pet_update_attrs_tick();pet_update_attrs_tick();
    pet_core_reset();g_state=PET_STATE_SLEEP;
    pet_update_attrs_tick();assert(g_pet.attrs.energy==80);
    puts("PASS reset: sleep recovery remainder does not carry over");
    dm_pet_set_enabled(false);
    assert(!g_anim_timer && !g_behavior_timer && !g_time_timer && !g_poll_timer && !g_save_timer);
    dm_pet_deinit();
    puts("PASS disable/deinit: timers released, repeated teardown safe");
    return 0;
}
