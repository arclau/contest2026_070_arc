/* Auto-generated unified frame table — do not edit */
/* 与 dm_pet.h 枚举顺序严格对齐（IDLE..GROOM 共 PET_STATE_COUNT 个），
 * 每态原生 12 帧（PET_FRAMES_PER_STATE=12），walk 为独立移动帧组 */
#include "pet_sprites.h"
#include "dm_pet.h"

const pet_frame_set_t g_cat_frames[PET_STATE_COUNT] = {
    /* idle */ { {&pet_cat_idle_00, &pet_cat_idle_01, &pet_cat_idle_02, &pet_cat_idle_03, &pet_cat_idle_04, &pet_cat_idle_05, &pet_cat_idle_06, &pet_cat_idle_07, &pet_cat_idle_08, &pet_cat_idle_09, &pet_cat_idle_10, &pet_cat_idle_11} },
    /* play */ { {&pet_cat_play_00, &pet_cat_play_01, &pet_cat_play_02, &pet_cat_play_03, &pet_cat_play_04, &pet_cat_play_05, &pet_cat_play_06, &pet_cat_play_07, &pet_cat_play_08, &pet_cat_play_09, &pet_cat_play_10, &pet_cat_play_11} },
    /* eat */ { {&pet_cat_eat_00, &pet_cat_eat_01, &pet_cat_eat_02, &pet_cat_eat_03, &pet_cat_eat_04, &pet_cat_eat_05, &pet_cat_eat_06, &pet_cat_eat_07, &pet_cat_eat_08, &pet_cat_eat_09, &pet_cat_eat_10, &pet_cat_eat_11} },
    /* sleep */ { {&pet_cat_sleep_00, &pet_cat_sleep_01, &pet_cat_sleep_02, &pet_cat_sleep_03, &pet_cat_sleep_04, &pet_cat_sleep_05, &pet_cat_sleep_06, &pet_cat_sleep_07, &pet_cat_sleep_08, &pet_cat_sleep_09, &pet_cat_sleep_10, &pet_cat_sleep_11} },
    /* happy */ { {&pet_cat_happy_00, &pet_cat_happy_01, &pet_cat_happy_02, &pet_cat_happy_03, &pet_cat_happy_04, &pet_cat_happy_05, &pet_cat_happy_06, &pet_cat_happy_07, &pet_cat_happy_08, &pet_cat_happy_09, &pet_cat_happy_10, &pet_cat_happy_11} },
    /* sad */ { {&pet_cat_sad_00, &pet_cat_sad_01, &pet_cat_sad_02, &pet_cat_sad_03, &pet_cat_sad_04, &pet_cat_sad_05, &pet_cat_sad_06, &pet_cat_sad_07, &pet_cat_sad_08, &pet_cat_sad_09, &pet_cat_sad_10, &pet_cat_sad_11} },
    /* hungry */ { {&pet_cat_hungry_00, &pet_cat_hungry_01, &pet_cat_hungry_02, &pet_cat_hungry_03, &pet_cat_hungry_04, &pet_cat_hungry_05, &pet_cat_hungry_06, &pet_cat_hungry_07, &pet_cat_hungry_08, &pet_cat_hungry_09, &pet_cat_hungry_10, &pet_cat_hungry_11} },
    /* curious */ { {&pet_cat_curious_00, &pet_cat_curious_01, &pet_cat_curious_02, &pet_cat_curious_03, &pet_cat_curious_04, &pet_cat_curious_05, &pet_cat_curious_06, &pet_cat_curious_07, &pet_cat_curious_08, &pet_cat_curious_09, &pet_cat_curious_10, &pet_cat_curious_11} },
    /* celebrate */ { {&pet_cat_celebrate_00, &pet_cat_celebrate_01, &pet_cat_celebrate_02, &pet_cat_celebrate_03, &pet_cat_celebrate_04, &pet_cat_celebrate_05, &pet_cat_celebrate_06, &pet_cat_celebrate_07, &pet_cat_celebrate_08, &pet_cat_celebrate_09, &pet_cat_celebrate_10, &pet_cat_celebrate_11} },
    /* touch */ { {&pet_cat_touch_00, &pet_cat_touch_01, &pet_cat_touch_02, &pet_cat_touch_03, &pet_cat_touch_04, &pet_cat_touch_05, &pet_cat_touch_06, &pet_cat_touch_07, &pet_cat_touch_08, &pet_cat_touch_09, &pet_cat_touch_10, &pet_cat_touch_11} },
    /* groom */ { {&pet_cat_groom_00, &pet_cat_groom_01, &pet_cat_groom_02, &pet_cat_groom_03, &pet_cat_groom_04, &pet_cat_groom_05, &pet_cat_groom_06, &pet_cat_groom_07, &pet_cat_groom_08, &pet_cat_groom_09, &pet_cat_groom_10, &pet_cat_groom_11} },
};

const pet_walk_set_t g_cat_walk_frames = {
    {&pet_cat_walk_00, &pet_cat_walk_01, &pet_cat_walk_02, &pet_cat_walk_03, &pet_cat_walk_04, &pet_cat_walk_05, &pet_cat_walk_06, &pet_cat_walk_07, &pet_cat_walk_08, &pet_cat_walk_09, &pet_cat_walk_10, &pet_cat_walk_11},
};
