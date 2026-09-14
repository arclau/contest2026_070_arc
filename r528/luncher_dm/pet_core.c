/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/pet_core.c
 * 电子宠物 — 引擎层实现（属性/状态机/行为/事件/投喂/持久化）
 *
 * 设计依据：project_docs/specs/PET_PRODUCT_SPEC.md
 * 本层零 lv_obj 操作；一切呈现经 pet_view_ops_t 回调交 view。
 * 线程：只在 LVGL 主线程调用（门面队列消费）；pet_core_data() 只读例外。
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>
#include "netutils/cJSON.h"   /* 存档 load（CONFIG_NETUTILS_CJSON=y，随 libapps.a 链接） */
#include "dm_pet.h"
#include "pet_core.h"

/****************************************************************************
 * 配置常量
 ****************************************************************************/

#define PET_SAVE_PATH       "/data/pet_state.json"
#define PET_SAVE_VERSION    2   /* v2 起含 enabled/hunger_alert（v1 档照读，缺项回默认开） */
#define PET_SAVE_INTERVAL_S 60   /* 门面 save timer 周期（门面 timer 拥有者，数值放这里备查） */

/* 属性衰减速率（每小时，PET_PRODUCT_SPEC §三；只养猫，单速率） */
#define PET_HUNGER_DECAY_CAT    1
#define PET_ENERGY_DECAY        1    /* 每2小时 -1（≈-0.5/h） */
#define PET_HAPPINESS_DECAY     3    /* 每10小时 -3（≈-0.3/h） */
#define PET_HEALTH_DECAY        1    /* hunger<20 时每小时 -1 */
#define PET_ENERGY_SLEEP_RECOVER 1   /* 睡觉时每3分钟 +1（≈+20/h） */

/* 属性下限（不惩罚用户） */
#define PET_HUNGER_FLOOR        10
#define PET_ENERGY_FLOOR        5
#define PET_HAPPINESS_FLOOR     20
#define PET_HEALTH_FLOOR        30

/* 睡眠唤醒阈值 */
#define PET_WAKE_ENERGY_MIN     30   /* 天亮且精力≥此值才自然醒 */
#define PET_WAKE_HOUR_START     6
#define PET_WAKE_HOUR_END       22
#define PET_SLEEP_ENERGY_MIN    15   /* 精力低于此值直接入睡（SLEEPY 态无素材已移除） */
#define PET_SLEEP_GRACE_S       300  /* 互动宽限：互动后至少 5 分钟不入睡（防刚喂完就睡） */

/* 投喂冷却 */
#define PET_FEED_COOLDOWN_MS     1500 /* 防连点疯狂刷成长 */

/* 事件驱动态固定持续时间（ticks，1 tick = 500ms 行为节拍） */
#define PET_EAT_DURATION_TICKS   8   /* 吃东西 ~4秒 */
#define PET_PLAY_DURATION_TICKS  12  /* 玩耍 ~6秒 */
#define PET_TOUCH_DURATION_TICKS 10  /* 抚摸 ~5秒（短爆发，防快速循环闪烁） */
#define PET_CELEB_DURATION_TICKS 16  /* 庆祝 ~8秒 */

/****************************************************************************
 * 行为池（权重/Duration/气泡 + 动画帧窗）
 *
 * 帧窗约定：12 帧环按行为语义切窗，同一状态不同行为播不同窗。
 * 窗是“动作切片”而非逐行为定制素材（cat_sprites 每态即一组通用动作环），
 * 故窗边界按“3~4 帧一段”均切 + 气泡/时长做行为区分，诚实且够用。
 ****************************************************************************/

/* 猫 IDLE 行为池（spec §四：5 个核心行为） */
static const pet_behavior_t g_cat_idle[] =
{
    /* name          weight dur  bubble  win */
    {"look_left",    20,    4,   NULL,   0, 2},
    {"look_right",   20,    4,   NULL,   3, 5},
    {"yawn",         10,    6,   "哈~",  6, 7},
    {"stretch",       8,    5,   NULL,   8, 9},
    {"sit",          15,    8,   NULL,  10, 11},
};
#define CAT_IDLE_COUNT (sizeof(g_cat_idle) / sizeof(g_cat_idle[0]))

/* SLEEP 行为池（猫狗共用） */
static const pet_behavior_t g_sleep_behaviors[] =
{
    {"sleep_deep",  50, 20, "zzZ...", 0, 5},
    {"sleep_light", 30, 10, NULL,     6, 8},
    {"twitch",      10,  3, NULL,     9, 10},
    {"wake_peek",   10,  4, NULL,    11, 11},
};
#define SLEEP_COUNT (sizeof(g_sleep_behaviors) / sizeof(g_sleep_behaviors[0]))

/* HAPPY 行为池 */
static const pet_behavior_t g_happy_behaviors[] =
{
    {"jump",        25, 4,  "开心！", 0, 2},
    {"spin",        20, 5,  NULL,     3, 5},
    {"approach",    20, 6,  NULL,     6, 7},
    {"purr",        15, 8,  "咕噜~",  8, 9},
    {"bounce",      20, 4,  NULL,    10, 11},
};
#define HAPPY_COUNT (sizeof(g_happy_behaviors) / sizeof(g_happy_behaviors[0]))

/* SAD 行为池 */
static const pet_behavior_t g_sad_behaviors[] =
{
    {"look_away",   30, 8,  NULL,  0, 2},
    {"hunch",       30, 10, NULL,  3, 5},
    {"sigh",        20, 6,  "...", 6, 8},
    {"slow_move",   20, 8,  NULL,  9, 11},
};
#define SAD_COUNT (sizeof(g_sad_behaviors) / sizeof(g_sad_behaviors[0]))

/* CELEBRATE 行为池 */
static const pet_behavior_t g_celebrate_behaviors[] =
{
    {"jump_high",   30, 5,  "太棒了！", 0, 2},
    {"spin_fast",   25, 6,  NULL,       3, 5},
    {"happy_run",   25, 8,  NULL,       6, 8},
    {"pose",        20, 6,  "耶~",      9, 11},
};
#define CELEBRATE_COUNT (sizeof(g_celebrate_behaviors) / sizeof(g_celebrate_behaviors[0]))

/* HUNGRY 行为池（饥饿：捂肚子、看饭盆、舔嘴） */
static const pet_behavior_t g_hungry_behaviors[] =
{
    {"paw_tummy",  40, 6,  "饿了…", 0, 3},
    {"look_bowl",  30, 5,  NULL,    4, 7},
    {"lick_lips",  30, 5,  "想吃…", 8, 11},
};
#define HUNGRY_COUNT (sizeof(g_hungry_behaviors) / sizeof(g_hungry_behaviors[0]))

/* CURIOUS 行为池（好奇：歪头、扒窗、竖耳张望） */
static const pet_behavior_t g_curious_behaviors[] =
{
    {"head_tilt",  40, 5,  "嗯？", 0, 3},
    {"peek_window", 30, 8, NULL,   4, 7},
    {"perk_ears",  30, 5,  NULL,   8, 11},
};
#define CURIOUS_COUNT (sizeof(g_curious_behaviors) / sizeof(g_curious_behaviors[0]))

/* TOUCH 行为池（被抚摸：短爆发3-6s） */
static const pet_behavior_t g_touch_behaviors[] =
{
    {"nuzzle",    40, 6, "呼噜~", 0, 3},
    {"roll_over", 30, 5, NULL,    4, 7},
    {"press_paw", 30, 4, NULL,    8, 11},
};
#define TOUCH_COUNT (sizeof(g_touch_behaviors) / sizeof(g_touch_behaviors[0]))

/* GROOM 行为池（舔毛：自我清洁，蹲坐时随机触发） */
static const pet_behavior_t g_groom_behaviors[] =
{
    {"lick_paw",   35, 6,  NULL,  0, 2},
    {"lick_body",  30, 8,  NULL,  3, 5},
    {"shake_head", 15, 3,  NULL,  6, 8},
    {"lick_chest", 20, 7,  NULL,  9, 11},
};
#define GROOM_COUNT (sizeof(g_groom_behaviors) / sizeof(g_groom_behaviors[0]))

/****************************************************************************
 * 食物效果表
 ****************************************************************************/
typedef struct
{
    int hunger_delta;   /* 负数 = 减少饥饿 */
    int happiness_delta;
    int affection_delta;
    int energy_delta;   /* 吃东西恢复精力（防止喂完就睡） */
    const char *bubble;
} pet_food_effect_t;

static const pet_food_effect_t g_food_effects[PET_FOOD_COUNT] =
{
    [PET_FOOD_FISH]  = {-30, 20, 2, 10, "好吃！"},
    [PET_FOOD_MEAT]  = {-20, 10, 1,  8, "凑合"},
    [PET_FOOD_VEGGIE]= {-15,  5, 1,  5, "不想吃"},
    [PET_FOOD_SNACK] = {-10, 25, 3,  3, "哼，还行"},
};

/****************************************************************************
 * 引擎内部状态
 ****************************************************************************/

static dm_pet_data_t  g_pet;                  /* 宠物数据 */
static dm_pet_state_t g_state   = PET_STATE_IDLE;

/* 当前行为 */
static const pet_behavior_t *g_cur_behavior = NULL;
static int                   g_behavior_ticks_left = 0;

/* 视图回调（view 注册；全部调用点判空：deinit/未建 UI 时事件照常消费） */
static const pet_view_ops_t *g_view_ops = NULL;

/* 欢迎回归标记（离线>12h 置位，门面消费一次） */
static bool g_welcome_back_pending = false;

static bool g_inited = false;
static uint32_t g_last_feed_ms = 0;           /* 投喂冷却时间戳 */

/****************************************************************************
 * 视图通知（判空安全壳）
 ****************************************************************************/

static void view_bubble(const char *txt, int ticks)
{
    if (g_view_ops && g_view_ops->show_bubble && txt)
        g_view_ops->show_bubble(txt, ticks);
}

static void view_hunger_alert(bool show)
{
    if (!g_view_ops) return;
    /* 提醒开关：关了只留 HUNGRY 行为，不弹红泡 */
    if (show && !g_pet.hunger_alert) return;
    if (show)
        {
            if (g_view_ops->show_hunger_alert) g_view_ops->show_hunger_alert();
        }
    else
        {
            if (g_view_ops->hide_hunger_alert) g_view_ops->hide_hunger_alert();
        }
}

/****************************************************************************
 * 工具函数
 ****************************************************************************/

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static const char *state_names[] =
{
    "IDLE", "PLAY", "EAT", "SLEEP",
    "HAPPY", "SAD", "HUNGRY", "CURIOUS", "CELEBRATE", "TOUCH", "GROOM"
};

const char *pet_core_state_name(dm_pet_state_t s)
{
    if (s >= 0 && s < PET_STATE_COUNT)
        return state_names[s];
    return "???";
}

dm_pet_life_stage_t pet_core_life_stage(void)
{
    if (g_pet.attrs.growth < 100)  return PET_LIFE_BABY;
    if (g_pet.attrs.growth < 500)  return PET_LIFE_JUVENILE;
    return PET_LIFE_ADULT;
}

dm_pet_state_t pet_core_state(void)
{
    return g_state;
}

const dm_pet_data_t *pet_core_data(void)
{
    return &g_pet;
}

/* 根据当前宠物类型返回对应帧集（只养猫，固定猫帧） */
const pet_frame_set_t *pet_core_frames(void)
{
    return g_cat_frames;
}

/* 状态层面是否可走（view 再叠加 walk_pause/dragging 得最终“走路中”） */
bool pet_core_can_walk(void)
{
    return (g_state == PET_STATE_IDLE || g_state == PET_STATE_HAPPY ||
            g_state == PET_STATE_SAD || g_state == PET_STATE_HUNGRY ||
            g_state == PET_STATE_CURIOUS);
}

/* 当前动画窗（行为窗或整幅；窗端点钳入 [0, PET_FRAMES_PER_STATE-1]） */
void pet_core_anim_range(int *first, int *last)
{
    int f = 0, l = PET_FRAMES_PER_STATE - 1;
    if (g_cur_behavior && g_cur_behavior->afirst >= 0)
        {
            f = g_cur_behavior->afirst;
            l = g_cur_behavior->alast;
        }
    f = clamp(f, 0, PET_FRAMES_PER_STATE - 1);
    l = clamp(l, 0, PET_FRAMES_PER_STATE - 1);
    if (l < f) l = f;
    if (first) *first = f;
    if (last)  *last  = l;
}

/* 北京时间（设备无 TZ 配置，gmtime+8；UTC 取失败返回 -1 → 调用方按白天处理） */
static int get_hour(void)
{
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    if (!utc) return -1;
    int h = utc->tm_hour + 8;
    if (h >= 24) h -= 24;
    return h;
}

/****************************************************************************
 * 持久化
 *
 * emit 保持历史 printf 格式（与旧存档/旧 loader 字节兼容，可回滚）；
 * load 改 cJSON（容忍字段顺序/空白/多余字段；坏项单独回落不整档丢弃）。
 ****************************************************************************/

void pet_core_save(void)
{
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", PET_SAVE_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fprintf(f,
        "{\"version\":%d,\"pet_type\":%d,\"name\":\"%s\","
        "\"hunger\":%d,\"energy\":%d,\"happiness\":%d,"
        "\"affection\":%d,\"health\":%d,\"growth\":%d,"
        "\"enabled\":%d,\"hunger_alert\":%d,"
        "\"last_interaction\":%ld,\"last_update\":%ld,"
        "\"created_at\":%ld,\"total_days\":%d}\n",
        PET_SAVE_VERSION, g_pet.pet_type, g_pet.name,
        g_pet.attrs.hunger, g_pet.attrs.energy, g_pet.attrs.happiness,
        g_pet.attrs.affection, g_pet.attrs.health, g_pet.attrs.growth,
        g_pet.enabled, g_pet.hunger_alert,
        g_pet.last_interaction, g_pet.last_update,
        g_pet.created_at, g_pet.total_days);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp_path, PET_SAVE_PATH);
}

/* 首次运行/存档损坏：生成默认宠物（猫 + 旺财 + 初始属性） */
static void pet_default_new(void)
{
    memset(&g_pet, 0, sizeof(g_pet));
    g_pet.version   = PET_SAVE_VERSION;
    g_pet.pet_type  = PET_TYPE_CAT;
    strcpy(g_pet.name, "旺财");
    g_pet.attrs.hunger    = 50;
    g_pet.attrs.energy    = 80;
    g_pet.attrs.happiness = 60;
    g_pet.attrs.affection = 20;
    g_pet.attrs.health    = 100;
    g_pet.attrs.growth    = 0;
    g_pet.enabled           = 1;   /* 总开关默认开 */
    g_pet.hunger_alert      = 1;   /* 饥饿提醒默认开 */
    g_pet.last_interaction = time(NULL);
    g_pet.last_update      = time(NULL);
    g_pet.created_at       = time(NULL);
    g_pet.total_days       = 0;
}

static int json_int(cJSON *root, const char *key, int dflt, int lo, int hi)
{
    cJSON *o = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(o)) return dflt;
    return clamp((int)o->valuedouble, lo, hi);
}

static long json_long(cJSON *root, const char *key, long dflt)
{
    cJSON *o = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(o)) return dflt;
    long v = (long)o->valuedouble;
    return v < 0 ? dflt : v;
}

static void pet_load(void)
{
    FILE *f = fopen(PET_SAVE_PATH, "r");
    if (!f)
        {
            pet_default_new();
            LV_LOG_USER("[pet] first boot: %s the cat", g_pet.name);
            return;
        }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096)
        {
            fclose(f);
            pet_default_new();
            LV_LOG_USER("[pet] save bad size %ld, re-created default", len);
            return;
        }
    char *buf = malloc((size_t)len + 1);
    if (!buf)
        {
            fclose(f);
            pet_default_new();
            return;
        }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root)
        {
            pet_default_new();
            LV_LOG_USER("[pet] save corrupted (not JSON), re-created default");
            return;
        }

    cJSON *vo = cJSON_GetObjectItemCaseSensitive(root, "version");
    int ver = cJSON_IsNumber(vo) ? (int)vo->valuedouble : -1;
    if (ver < 1 || ver > PET_SAVE_VERSION)
        {
            cJSON_Delete(root);
            pet_default_new();
            LV_LOG_USER("[pet] save version %d unsupported, re-created default", ver);
            return;
        }

    memset(&g_pet, 0, sizeof(g_pet));
    g_pet.version = ver;
    g_pet.pet_type = PET_TYPE_CAT;   /* 只养猫：老存档 dog 值直接归猫 */
    cJSON *no = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(no) && no->valuestring)
        {
            strncpy(g_pet.name, no->valuestring, sizeof(g_pet.name) - 1);
            g_pet.name[sizeof(g_pet.name) - 1] = '\0';
        }
    else
        strcpy(g_pet.name, "旺财");
    g_pet.attrs.hunger    = json_int(root, "hunger", 50, 0, 100);
    g_pet.attrs.energy    = json_int(root, "energy", 80, 0, 100);
    g_pet.attrs.happiness = json_int(root, "happiness", 60, 0, 100);
    g_pet.attrs.affection = json_int(root, "affection", 20, 0, 100);
    g_pet.attrs.health    = json_int(root, "health", 100, 0, 100);
    int gr = json_int(root, "growth", 0, 0, 1000000);
    g_pet.attrs.growth    = gr < 0 ? 0 : gr;
    /* v2 新增开关：v1 老档无此键 → 默认开（json_int 缺省值） */
    g_pet.enabled      = json_int(root, "enabled", 1, 0, 1);
    g_pet.hunger_alert = json_int(root, "hunger_alert", 1, 0, 1);
    time_t now = time(NULL);
    g_pet.last_interaction = json_long(root, "last_interaction", (long)now);
    g_pet.last_update      = json_long(root, "last_update", (long)now);
    g_pet.created_at       = json_long(root, "created_at", (long)now);
    g_pet.total_days       = json_int(root, "total_days", 0, 0, 100000);
    cJSON_Delete(root);

    /* 离线时间计算：关机期间属性缓慢变化 */
    long elapsed_h = (now - g_pet.last_update) / 3600;
    if (elapsed_h > 0 && elapsed_h < 720)  /* 最多算30天 */
        {
            g_pet.attrs.hunger = clamp(g_pet.attrs.hunger +
                                       PET_HUNGER_DECAY_CAT * (int)elapsed_h,
                                       0, 100);
            /* 离线恢复精力：每天最多 +12（≈半天睡眠），防止长离线免费满血 */
            long energy_gain = elapsed_h / 2;
            long cap = 12 * (elapsed_h / 24 + 1);
            if (energy_gain > cap) energy_gain = cap;
            g_pet.attrs.energy = clamp(g_pet.attrs.energy + (int)energy_gain, 0, 100);
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness - (int)(elapsed_h / 10),
                                          PET_HAPPINESS_FLOOR, 100);
            if (g_pet.attrs.hunger < 20)
                g_pet.attrs.health = clamp(g_pet.attrs.health - (int)elapsed_h,
                                           PET_HEALTH_FLOOR, 100);
            g_pet.total_days += (int)(elapsed_h / 24);
        }
    g_pet.last_update = now;

    /* 长离线欢迎：离线 >12h → 标记显示欢迎气泡（仅气泡，无属性加成） */
    if (elapsed_h > 12)
        g_welcome_back_pending = true;

    LV_LOG_USER("[pet] loaded: %s type=%d hunger=%d happy=%d growth=%d elapsed_h=%ld",
                g_pet.name, g_pet.pet_type, g_pet.attrs.hunger,
                g_pet.attrs.happiness, g_pet.attrs.growth, elapsed_h);
}

/****************************************************************************
 * 属性系统（每分钟调用一次；整数分钟累计，满 60 分钟合一小时落地）
 ****************************************************************************/

static void pet_update_attrs_tick(void)
{
    static int acc_hunger = 0, acc_energy = 0, acc_happy = 0, acc_health = 0,
               acc_sleep = 0;

    int hunger_rate = PET_HUNGER_DECAY_CAT;

    acc_hunger += hunger_rate;
    acc_energy += PET_ENERGY_DECAY;
    acc_happy  += PET_HAPPINESS_DECAY;
    if (g_state == PET_STATE_SLEEP)
        acc_sleep += PET_ENERGY_SLEEP_RECOVER;

    if (acc_hunger >= 60) { g_pet.attrs.hunger = clamp(g_pet.attrs.hunger + 1, 0, 100); acc_hunger -= 60; }
    if (acc_energy >= 120) { g_pet.attrs.energy = clamp(g_pet.attrs.energy - 1, 0, 100); acc_energy -= 120; }
    if (acc_happy >= 600) { g_pet.attrs.happiness = clamp(g_pet.attrs.happiness - 3, 0, 100); acc_happy -= 600; }
    if (acc_sleep >= 3) { g_pet.attrs.energy = clamp(g_pet.attrs.energy + 1, 0, 100); acc_sleep -= 3; }

    /* 健康间接衰减：hunger<20 每分钟 -1/60（每小时 -1） */
    if (g_pet.attrs.hunger < 20)
        {
            acc_health += PET_HEALTH_DECAY;
            if (acc_health >= 60)
                {
                    g_pet.attrs.health = clamp(g_pet.attrs.health - 1, PET_HEALTH_FLOOR, 100);
                    acc_health -= 60;
                }
        }

    g_pet.last_update = time(NULL);
}

/****************************************************************************
 * 状态转移逻辑
 ****************************************************************************/

static dm_pet_state_t pet_next_state(void)
{
    int h = get_hour();

    /* 睡觉中：天亮 + 精力恢复够 → 自然醒（解决睡眠死锁，spec §五时间联动） */
    if (g_state == PET_STATE_SLEEP)
        {
            if (h >= PET_WAKE_HOUR_START && h < PET_WAKE_HOUR_END &&
                g_pet.attrs.energy >= PET_WAKE_ENERGY_MIN)
                return PET_STATE_IDLE;
            return PET_STATE_SLEEP;  /* 否则继续睡 */
        }

    /* 事件驱动态（EAT/PLAY/TOUCH/CELEBRATE）有固定持续时间，
     * TIME_TICK 不应打断——由行为节拍到时自动回 IDLE */
    if (g_state == PET_STATE_EAT || g_state == PET_STATE_PLAY ||
        g_state == PET_STATE_TOUCH || g_state == PET_STATE_CELEBRATE)
        return g_state;

    /* 夜间强制睡觉（22:00~05:59，对齐 PET_WAKE_HOUR_END=22；P1 时间联动） */
    if (h >= 0 && (h >= PET_WAKE_HOUR_END || h < PET_WAKE_HOUR_START))
        return PET_STATE_SLEEP;

    /* 精力不足且久未互动 → 入睡（互动宽限内不睡，防“刚喂完就睡”） */
    if (g_pet.attrs.energy < PET_SLEEP_ENERGY_MIN &&
        time(NULL) - g_pet.last_interaction >= PET_SLEEP_GRACE_S)
        return PET_STATE_SLEEP;

    /* 饥饿 */
    if (g_pet.attrs.hunger > 80)
        return PET_STATE_HUNGRY;

    /* 快乐极低 */
    if (g_pet.attrs.happiness < 20)
        return PET_STATE_SAD;

    /* P3：待机时偶尔舔毛（复用既有 GROOM 素材，让猫更“活”）；
     * 一次行为结束即自动回 IDLE（见 pet_core_behavior_tick）。 */
    if ((rand() % 100) < 12)
        return PET_STATE_GROOM;

    return PET_STATE_IDLE;
}

static void pet_apply_state_effects(void)
{
    switch (g_state)
        {
        case PET_STATE_SLEEP:
            g_pet.attrs.energy = clamp(g_pet.attrs.energy + 5, 0, 100);
            break;
        case PET_STATE_HAPPY:
        case PET_STATE_CELEBRATE:
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 2, 0, 100);
            break;
        case PET_STATE_SAD:
            /* 不额外惩罚，只是表现 */
            break;
        default:
            break;
        }
}

/****************************************************************************
 * 行为选择（加权随机；view 经 anim_range 取窗播放）
 ****************************************************************************/

static const pet_behavior_t *pick_from_pool(const pet_behavior_t *pool,
                                            int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
        total += pool[i].weight;
    if (total <= 0) return &pool[0];

    int r = rand() % total;
    for (int i = 0; i < count; i++)
        {
            r -= pool[i].weight;
            if (r < 0) return &pool[i];
        }
    return &pool[count - 1];
}

static void pet_select_behavior(void)
{
    const pet_behavior_t *b = NULL;

    switch (g_state)
        {
        case PET_STATE_IDLE:      b = pick_from_pool(g_cat_idle, CAT_IDLE_COUNT); break;
        case PET_STATE_SLEEP:     b = pick_from_pool(g_sleep_behaviors, SLEEP_COUNT); break;
        case PET_STATE_HAPPY:     b = pick_from_pool(g_happy_behaviors, HAPPY_COUNT); break;
        case PET_STATE_SAD:       b = pick_from_pool(g_sad_behaviors, SAD_COUNT); break;
        case PET_STATE_CELEBRATE: b = pick_from_pool(g_celebrate_behaviors, CELEBRATE_COUNT); break;
        case PET_STATE_HUNGRY:    b = pick_from_pool(g_hungry_behaviors, HUNGRY_COUNT); break;
        case PET_STATE_CURIOUS:   b = pick_from_pool(g_curious_behaviors, CURIOUS_COUNT); break;
        case PET_STATE_TOUCH:     b = pick_from_pool(g_touch_behaviors, TOUCH_COUNT); break;
        case PET_STATE_GROOM:     b = pick_from_pool(g_groom_behaviors, GROOM_COUNT); break;
        default:
            /* EAT/PLAY 由事件直接驱动，不走随机行为池（整幅播放） */
            g_cur_behavior = NULL;
            return;
        }

    g_cur_behavior = b;
    g_behavior_ticks_left = b->duration;

    /* view 检测 anim_range 变化即从窗首重播；此处只负责气泡。
     * （P200 教训：旧逻辑每次选行为硬切 frames[0] 即一次形态跳变） */
    if (b->bubble)
        view_bubble(b->bubble, PET_BUBBLE_DURATION_TICKS);
}

/* 统一状态切换：所有触发共用（日志 + 入态效果 + 选行为 + 事件态定时 + 饥饿提示） */
static void pet_transition_to(dm_pet_state_t new_state, const char *why)
{
    dm_pet_state_t old = g_state;
    if (new_state == old)
        {
            /* 同态重入：刷新持续时间，防止残值为0导致立刻跳走 */
            if (old == PET_STATE_EAT)
                g_behavior_ticks_left = PET_EAT_DURATION_TICKS;
            else if (old == PET_STATE_PLAY)
                g_behavior_ticks_left = PET_PLAY_DURATION_TICKS;
            else if (old == PET_STATE_TOUCH)
                g_behavior_ticks_left = PET_TOUCH_DURATION_TICKS;
            else if (old == PET_STATE_CELEBRATE)
                g_behavior_ticks_left = PET_CELEB_DURATION_TICKS;
            else
                pet_select_behavior();
            return;
        }
    g_state = new_state;
    pet_apply_state_effects();
    pet_select_behavior();

    /* 事件驱动态无行为池，设固定持续时间后自动回 IDLE */
    if (new_state == PET_STATE_EAT)
        g_behavior_ticks_left = PET_EAT_DURATION_TICKS;
    else if (new_state == PET_STATE_PLAY)
        g_behavior_ticks_left = PET_PLAY_DURATION_TICKS;
    else if (new_state == PET_STATE_TOUCH)
        g_behavior_ticks_left = PET_TOUCH_DURATION_TICKS;
    else if (new_state == PET_STATE_CELEBRATE)
        g_behavior_ticks_left = PET_CELEB_DURATION_TICKS;

    LV_LOG_USER("[pet] %s → %s (%s)",
                pet_core_state_name(old), pet_core_state_name(new_state), why);

    /* 进入饥饿状态 → 显示可点击喂食提示；离开即撤 */
    view_hunger_alert(new_state == PET_STATE_HUNGRY);
}

/* 统一投喂：冷却 + 零食成年解锁 + 食物效果 + 防“喂完就睡” + 记互动 + 进 EAT */
static void pet_apply_food(int fi)
{
    uint32_t now_ms = lv_tick_get();
    if (now_ms - g_last_feed_ms < PET_FEED_COOLDOWN_MS) return; /* 防连点刷成长 */
    g_last_feed_ms = now_ms;

    if (fi < 0 || fi >= PET_FOOD_COUNT) return;

    /* 零食只有成年才解锁，幼年/成长喂零食只给提示不给效果 */
    if (fi == PET_FOOD_SNACK && pet_core_life_stage() < PET_LIFE_ADULT)
        {
            view_bubble("还小，不吃零食~", PET_BUBBLE_DURATION_TICKS);
            return;
        }

    /* 喂睡着的猫：先叫醒再吃（睡眠互动需有惊醒反应） */
    if (g_state == PET_STATE_SLEEP)
        pet_transition_to(PET_STATE_IDLE, "wake-for-feed");

    const pet_food_effect_t *fe = &g_food_effects[fi];
    g_pet.attrs.hunger    = clamp(g_pet.attrs.hunger + fe->hunger_delta, 0, 100);
    g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + fe->happiness_delta, 0, 100);
    g_pet.attrs.affection = clamp(g_pet.attrs.affection + fe->affection_delta, 0, 100);
    g_pet.attrs.energy    = clamp(g_pet.attrs.energy + fe->energy_delta, 0, 100);
    if (g_pet.attrs.energy < PET_SLEEP_ENERGY_MIN)
        g_pet.attrs.energy = PET_SLEEP_ENERGY_MIN; /* 吃完精力保底：不会刚喂完就睡 */
    g_pet.attrs.growth++;
    g_pet.last_interaction = time(NULL);           /* 互动宽限内不入睡 */
    pet_transition_to(PET_STATE_EAT, "feed");
    view_bubble(fe->bubble, PET_BUBBLE_DURATION_TICKS);  /* 食物口感气泡 */
    pet_core_save();
}

/****************************************************************************
 * 事件处理（门面队列出口，主线程）
 *
 * 注意：WAKE_UP/SLEEP（锁屏进出）的 timer 暂停/恢复 + 停靠由门面同步处理
 * （timer 归门面管），core 只管纯引擎语义，这里不再收这两个事件。
 ****************************************************************************/

/* 互动类事件：若宠物在睡觉先叫醒再反应，避免“睡梦中被摸无反应”。
 * timer 恢复是死代码故不做（P201：timer 只在 WAKE_UP/主屏路径暂停，
 * 彼时宠物不可见、无互动入口；睡眠态锁屏下 timer 本就在跑）。 */
static void pet_wake_for_interaction(void)
{
    if (g_state == PET_STATE_SLEEP)
        pet_transition_to(PET_STATE_IDLE, "wake-interact");
}

void pet_core_process_event(dm_pet_event_t evt, int param)
{
    switch (evt)
        {
        /* ── 触摸互动：短触摸进 TOUCH 态（窗循环 3-6s 短爆发） ── */
        case PET_EVT_TAP_HEAD:
            pet_wake_for_interaction();
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 2, 0, 100);
            g_pet.attrs.affection = clamp(g_pet.attrs.affection + 1, 0, 100);
            g_pet.attrs.growth++;
            g_pet.last_interaction = time(NULL);
            pet_transition_to(PET_STATE_TOUCH, "tap-head");
            break;

        case PET_EVT_TAP_BODY:
            pet_wake_for_interaction();
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 1, 0, 100);
            g_pet.attrs.growth++;
            g_pet.last_interaction = time(NULL);
            /* 猫有2/3概率回应触摸（傲娇：不是每次都理你） */
            if ((rand() % 3) > 0)
                pet_transition_to(PET_STATE_TOUCH, "tap-body");
            break;

        case PET_EVT_LONG_PRESS:
            pet_wake_for_interaction();
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 3, 0, 100);
            g_pet.attrs.affection = clamp(g_pet.attrs.affection + 2, 0, 100);
            g_pet.attrs.growth++;
            g_pet.last_interaction = time(NULL);
            pet_transition_to(PET_STATE_TOUCH, "long-press");
            break;

        case PET_EVT_RAPID_TAP:
            pet_wake_for_interaction();
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 5, 0, 100);
            g_pet.attrs.energy = clamp(g_pet.attrs.energy - 5, 0, 100);
            g_pet.attrs.growth += 2;
            g_pet.last_interaction = time(NULL);
            pet_transition_to(PET_STATE_PLAY, "rapid-tap");
            break;

        case PET_EVT_DRAG:
            pet_wake_for_interaction();
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 2, 0, 100);
            g_pet.attrs.growth++;
            g_pet.last_interaction = time(NULL);
            pet_transition_to(PET_STATE_PLAY, "drag");
            break;

        /* ── 投喂 ── */
        case PET_EVT_FEED_FISH:
        case PET_EVT_FEED_MEAT:
        case PET_EVT_FEED_VEGGIE:
        case PET_EVT_FEED_SNACK:
            pet_apply_food((int)evt - (int)PET_EVT_FEED_FISH);
            break;

        /* ── 环境 ── */
        case PET_EVT_MUSIC_ON:
            pet_wake_for_interaction();
            pet_transition_to(PET_STATE_HAPPY, "music-on");
            break;

        case PET_EVT_MUSIC_OFF:
            if (g_state == PET_STATE_HAPPY)
                pet_transition_to(PET_STATE_IDLE, "music-off");
            break;

        case PET_EVT_WEATHER_CHANGE:
            /* 非晴极端天气 → CURIOUS + 天气气泡；晴天无动作 */
            if (g_state != PET_STATE_SLEEP && param != 0)
                {
                    const char *wtxt = (param == 1) ? "下雨了~"
                                     : (param == 2) ? "好热啊…"
                                     :               "有点冷…";
                    pet_transition_to(PET_STATE_CURIOUS, "weather");
                    view_bubble(wtxt, PET_BUBBLE_DURATION_TICKS);
                }
            break;

        case PET_EVT_VOICE_CMD:
            if (param >= (int)PET_EVT_FEED_FISH && param <= (int)PET_EVT_FEED_SNACK)
                {
                    /* 语音投喂：直接处理食物，不递归入队 */
                    pet_apply_food(param - (int)PET_EVT_FEED_FISH);
                }
            else if (param == (int)PET_EVT_RAPID_TAP)
                {
                    g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 5, 0, 100);
                    g_pet.attrs.energy = clamp(g_pet.attrs.energy - 5, 0, 100);
                    g_pet.attrs.growth += 2;
                    g_pet.last_interaction = time(NULL);
                    pet_transition_to(PET_STATE_PLAY, "voice-play");
                }
            break;

        /* ── P1 环境/健康联动（复用现有状态，不加新素材）── */
        case PET_EVT_PRESENCE:
            /* 有人坐下：睡着/吃东西时不打扰，否则抬头好奇看一眼 */
            if (g_state != PET_STATE_SLEEP && g_state != PET_STATE_EAT)
                pet_transition_to(PET_STATE_CURIOUS, "presence");
            break;

        case PET_EVT_WATER_GOAL:
            pet_wake_for_interaction();
            pet_transition_to(PET_STATE_CELEBRATE, "water-goal");
            break;

        case PET_EVT_SIT_ALERT:
            /* 久坐提醒：仅待机/舔毛时转担忧，不打断事件态 */
            if (g_state == PET_STATE_IDLE || g_state == PET_STATE_GROOM)
                pet_transition_to(PET_STATE_SAD, "sit-alert");
            break;

        case PET_EVT_AI_CHAT:
            if (g_state != PET_STATE_SLEEP)
                {
                    pet_wake_for_interaction();
                    pet_transition_to(PET_STATE_HAPPY, "ai-chat");
                }
            break;

        case PET_EVT_TIME_TICK:
            pet_update_attrs_tick();
            {
                dm_pet_state_t next = pet_next_state();
                if (next != g_state)
                    pet_transition_to(next, "time-tick");
            }
            break;

        default:
            /* WAKE_UP/SLEEP 由门面同步处理；未知事件忽略 */
            break;
        }
}

/****************************************************************************
 * 节拍入口（门面 timer 驱动）
 ****************************************************************************/

void pet_core_behavior_tick(void)
{
    /* 行为倒计时 */
    if (g_behavior_ticks_left > 0)
        {
            g_behavior_ticks_left--;
            if (g_behavior_ticks_left == 0)
                {
                    /* TOUCH 短爆发结束自动回 IDLE */
                    if (g_state == PET_STATE_TOUCH)
                        pet_transition_to(PET_STATE_IDLE, "touch-done");
                    /* SAD：快乐恢复够了 → 回 IDLE（阈值对齐进入条件 <20） */
                    else if (g_state == PET_STATE_SAD && g_pet.attrs.happiness >= 20)
                        pet_transition_to(PET_STATE_IDLE, "sad-recovered");
                    /* EAT/PLAY/CELEBRATE/GROOM 事件驱动态，时间到自动回 IDLE */
                    else if (g_state == PET_STATE_EAT || g_state == PET_STATE_PLAY ||
                             g_state == PET_STATE_CELEBRATE || g_state == PET_STATE_GROOM)
                        pet_transition_to(PET_STATE_IDLE, "action-done");
                    else
                        pet_select_behavior();
                }
        }
    else
        {
            pet_select_behavior();
        }
}

void pet_core_time_tick(void)
{
    pet_core_process_event(PET_EVT_TIME_TICK, 0);
}

/****************************************************************************
 * 门面 API 实现（core 侧）
 ****************************************************************************/

void pet_core_init(void)
{
    if (g_inited) return;
    srand((unsigned)time(NULL));
    pet_load();
    g_inited = true;
}

void pet_core_register_view(const pet_view_ops_t *ops)
{
    g_view_ops = ops;
}

void pet_core_unregister_view(void)
{
    g_view_ops = NULL;
}

void pet_core_enter_initial(void)
{
    g_state = pet_next_state();
    pet_select_behavior();
}

bool pet_core_consume_welcome(void)
{
    bool w = g_welcome_back_pending;
    g_welcome_back_pending = false;
    return w;
}

void pet_core_set_name(const char *name)
{
    if (!name || !*name) return;
    /* pet_state.json 用 fprintf("%s") 直写名字，含 " 或 \ 会破坏 JSON →
     * 下次开机 cJSON 解析失败回落默认宠物（静默数据丢失）。这里清洗掉。 */
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j < sizeof(g_pet.name) - 1; i++) {
        if (name[i] == '"' || name[i] == '\\')
            continue;
        g_pet.name[j++] = name[i];
    }
    g_pet.name[j] = '\0';
    if (g_pet.name[0] == '\0')
        return;
    pet_core_save();
    LV_LOG_USER("[pet] name changed to %s", g_pet.name);
}

void pet_core_set_enabled(bool enabled)
{
    g_pet.enabled = enabled ? 1 : 0;
    pet_core_save();
    LV_LOG_USER("[pet] enabled=%d", g_pet.enabled);
}

bool pet_core_is_enabled(void)
{
    return g_pet.enabled != 0;
}

void pet_core_set_hunger_alert(bool on)
{
    g_pet.hunger_alert = on ? 1 : 0;
    pet_core_save();
    if (!on)
        view_hunger_alert(false);   /* 关开关立即撤已弹红泡 */
}

bool pet_core_get_hunger_alert(void)
{
    return g_pet.hunger_alert != 0;
}

/* 重新开始：回默认（开关保持用户选择，只清养成数据）+ 状态机复位 */
void pet_core_reset(void)
{
    int enabled = g_pet.enabled;
    int alert = g_pet.hunger_alert;
    pet_default_new();
    g_pet.enabled = enabled;
    g_pet.hunger_alert = alert;
    g_state = PET_STATE_IDLE;
    g_cur_behavior = NULL;
    g_behavior_ticks_left = 0;
    g_welcome_back_pending = false;
    pet_core_save();
    view_hunger_alert(false);
    LV_LOG_USER("[pet] reset to new pet");
}
