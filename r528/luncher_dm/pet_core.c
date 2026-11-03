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
#include <unistd.h>

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

/* P349：语音命令的保持时长。原先 g_hold_mode 置位了却从不设 g_hold_until_ms，
 * 保持实际是"无限"——既让"坐下"再也不走，也让"睡觉"没法用时间兜底。 */
#define PET_HOLD_SIT_MS         (10u * 60u * 1000u)  /* 语音"坐下"保持 10 分钟 */
#define PET_HOLD_SLEEP_MS       (30u * 60u * 1000u)  /* 语音"睡觉"保持 30 分钟 */

/* 打盹（闲置入睡）。阈值和时长都带随机，避免"到点就倒"：
 * 没人碰屏幕 30~60 分钟之间某个随机点趴下，睡 8~13 分钟，
 * 上午最多 2 次、下午最多 2 次，两次之间至少醒 40 分钟。 */
#define PET_NAP_IDLE_MIN_MS   (30 * 60 * 1000)  /* 最早：闲置 30 分钟 */
#define PET_NAP_IDLE_MAX_MS   (60 * 60 * 1000)  /* 最晚：闲置 60 分钟 */
#define PET_NAP_LEN_MIN_MS    (8 * 60 * 1000)   /* 单次打盹 8~13 分钟 */
#define PET_NAP_LEN_MAX_MS    (13 * 60 * 1000)
#define PET_NAP_GAP_MS        (40 * 60 * 1000)  /* 两次打盹之间至少醒 40 分钟 */
#define PET_NAP_AM_MAX        2                 /* 06:00~11:59 */
#define PET_NAP_PM_MAX        2                 /* 12:00~17:59 */
#define PET_NIGHT_DELAY_MAX   30                /* 夜里 22:00 后随机 0~29 分钟才倒 */

/* 投喂冷却 */
#define PET_FEED_COOLDOWN_MS     1500 /* 防连点疯狂刷成长 */

/* 事件驱动态固定持续时间（ticks，1 tick = 500ms 行为节拍） */
#define PET_EAT_DURATION_TICKS   8   /* 吃东西 ~4秒 */
#define PET_PLAY_DURATION_TICKS  12  /* 玩耍 ~6秒 */
#define PET_TOUCH_DURATION_TICKS 10  /* 抚摸 ~5秒（短爆发，防快速循环闪烁） */
#define PET_CELEB_DURATION_TICKS 16  /* 庆祝 ~8秒 */
/* The view considers another walk after 4-8s sitting. Keep random autonomous
 * actions from cutting that rest short; allow for timer phase and the next
 * 33ms animation tick that starts the view's idle deadline. */
#define PET_POST_WALK_IDLE_TICKS 18

/****************************************************************************
 * 自主动作池（权重、真实动作ID和气泡）
 ****************************************************************************/

/* Autonomy names refer to complete authored actions, never arbitrary frame windows. */
static const pet_behavior_t g_cat_idle[] = {
    {"blink",PET_ACTION_BLINK,60,NULL},
    {"notice_user",PET_ACTION_NOTICE_USER,10,NULL},
    {"groom_paw",PET_ACTION_GROOM_PAW,8,"舔舔~"},
    {"stretch",PET_ACTION_STRETCH,4,NULL},
    {"sit",PET_ACTION_IDLE_SIT,35,NULL},
};
#define CAT_IDLE_COUNT (sizeof(g_cat_idle)/sizeof(g_cat_idle[0]))

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
    [PET_FOOD_FISH]  = {-30, 20, 2, 10, "好吃！♥"},
    [PET_FOOD_MEAT]  = {-20, 10, 1,  8, "凑合"},
    [PET_FOOD_VEGGIE]= {-15,  5, 1,  5, "不想吃"},
    [PET_FOOD_SNACK] = {-10, 25, 3,  3, "哼，还行"},
};

/****************************************************************************
 * 引擎内部状态
 ****************************************************************************/

static dm_pet_data_t  g_pet;                  /* 宠物数据 */
static dm_pet_state_t g_state   = PET_STATE_IDLE;
/* 分钟余量属于当前宠物，重新养一只时必须一起清零。 */
static int acc_hunger, acc_energy, acc_happy, acc_health, acc_sleep;

/* 当前行为 */
static const pet_behavior_t *g_cur_behavior = NULL;
static int                   g_behavior_ticks_left = 0;
static uint32_t              g_animation_id = 0;
static pet_action_t g_action = PET_ACTION_IDLE_SIT;
static pet_action_t g_after_transition = PET_ACTION_COUNT;
static bool g_touch_held;
static bool g_play_active;
static unsigned g_play_ticks;
static uint32_t g_play_rest_until;
static bool g_voice_active;
static pet_action_t g_voice_action;
static uint32_t g_voice_deadline;
/* P363：这一轮语音开始前猫在睡觉 → 播报结束后躺回去。
 * 用户反馈："AI 回复的时候这猫一直在睡觉，应该先起来说话，说完再睡觉。" */
static bool g_voice_resume_sleep;
static uint32_t g_last_stretch_ms;

/* 视图回调（view 注册；全部调用点判空：deinit/未建 UI 时事件照常消费） */
static const pet_view_ops_t *g_view_ops = NULL;

/* 欢迎回归标记（离线>12h 置位，门面消费一次） */
static bool g_welcome_back_pending = false;

static bool g_inited = false;
static uint32_t g_last_feed_ms = 0;           /* 投喂冷却时间戳 */

/* 打盹状态（见 PET_NAP_*）。g_nap_active 区分"打盹"与"夜眠"：
 * 夜眠由 pet_next_state 的时段规则管，打盹由 pet_nap_tick 定时叫醒。 */
static bool     g_nap_active;       /* 当前 SLEEP 是不是打盹 */
static uint32_t g_nap_until_ms;     /* 本次打盹结束时刻 */
static uint32_t g_nap_target_ms;    /* 本轮随机出来的闲置阈值 30~60 分钟 */
static uint32_t g_nap_last_end_ms;  /* 上次打盹结束时刻 */
static int      g_nap_am, g_nap_pm; /* 当天上午/下午已睡次数 */
static int      g_nap_day = -1;     /* 本地日序号，跨天清零 */
static int      g_night_delay_min;  /* 每晚 22:00 后随机延迟入睡的分钟数 */

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

pet_action_t pet_core_action(void) { return g_action; }
bool pet_core_touch_held(void) { return g_touch_held; }
bool pet_core_playing(void) { return g_play_active; }
/* 2026-11-01 P331：语音命令保持模式。0=无 1=坐着 2=睡觉。
 * 置位期间 pet_core_can_walk() 为假 → 不会自己走开，命令才"听话"。 */
static int g_hold_mode;
static uint32_t g_hold_until_ms;   /* 0=无限；到点自动解除，防"说一次坐下永远不动" */
static uint32_t g_hold_set_ms;     /* 本次 hold 是什么时候下的（P363） */
/* P363：hold=2 只该压住"去睡觉"那一声回应，不该压住之后每一次用户主动
 * 发起的对话。实测 11:40:39 ASR 把话听成"维纳睡…"，本地词表命中 → cmd=2
 * → 30 分钟 hold，之后猫就再也不起身说话了（用户："AI 回复的时候这猫一直
 * 在睡觉"）。所以：命令下来时置位，这一轮播报结束（VOICE_PHASE 0）解除，
 * 另有 15 秒兜底（万一那一轮没收到 0）。 */
static bool     g_hold_voice_mute;

/* P349：hold 到期检查抽成函数。原来只在 pet_core_can_walk() 里做，
 * pet_next_state() 完全看不到到期状态，于是语音"睡觉"进去后，下一个
 * time-tick 按"白天且精力够"直接判回 IDLE —— 用户看到的就是"叫它睡它不睡"。 */
static int hold_mode_now(void)
{
    if (g_hold_mode && g_hold_until_ms &&
        (int32_t)(lv_tick_get() - g_hold_until_ms) >= 0) {
        LV_LOG_USER("[pet] hold expired (mode=%d)", g_hold_mode);
        g_hold_mode = 0;
        g_hold_until_ms = 0;
    }
    return g_hold_mode;
}

/* P363：这一轮语音要不要因为 hold=2 而保持不动。 */
static bool hold_voice_muted(void)
{
    if (!g_hold_voice_mute)
        return false;
    if ((int32_t)(lv_tick_get() - g_hold_set_ms) >= 15000) {
        g_hold_voice_mute = false;      /* 兜底：那一轮没收到 VOICE_PHASE 0 */
        return false;
    }
    return true;
}

bool pet_core_can_walk(void)
{
    /* P334 审计修复：保持模式必须有时限。到期自动回到自由活动，
     * 否则用户说一次"坐下"，猫就再也不走了（除非记得说"停下"）。 */
    return g_state==PET_STATE_IDLE && g_action==PET_ACTION_IDLE_SIT &&
           g_after_transition==PET_ACTION_COUNT && !g_touch_held && !g_voice_active &&
           hold_mode_now()==0;
}
uint32_t pet_core_animation_id(void) { return g_animation_id; }
static bool pet_interaction_active(void)
{
    return g_voice_active || g_state==PET_STATE_EAT || g_state==PET_STATE_PLAY ||
           g_state==PET_STATE_TOUCH || g_state==PET_STATE_CELEBRATE;
}
static void pet_set_action(pet_action_t action,const char *why)
{
    g_action=action;++g_animation_id;
    if (action==PET_ACTION_STRETCH) g_last_stretch_ms=lv_tick_get();
    LV_LOG_USER("[pet] select action=%s instance=%lu reason=%s",
        g_pet_actions[action].name,(unsigned long)g_animation_id,why);
}
static void pet_request_action(pet_action_t action,const char *why)
{
    if (g_action==PET_ACTION_WAKE || g_action==PET_ACTION_SETTLE) {
        g_after_transition=action;return;
    }
    if ((g_action==PET_ACTION_SLEEP_ENTER || g_action==PET_ACTION_SLEEP_LOOP) &&
        action!=PET_ACTION_SLEEP_ENTER && action!=PET_ACTION_SLEEP_LOOP) {
        g_after_transition=action;pet_set_action(PET_ACTION_WAKE,why);return;
    }
    if ((g_action==PET_ACTION_WALK || g_action==PET_ACTION_TURN) &&
        action!=PET_ACTION_WALK && action!=PET_ACTION_SETTLE) {
        g_after_transition=action;pet_set_action(PET_ACTION_SETTLE,why);return;
    }
    g_after_transition=PET_ACTION_COUNT;pet_set_action(action,why);
}
bool pet_core_begin_walk(void)
{
    if (!pet_core_can_walk()) return false;
    pet_set_action(PET_ACTION_TURN,"roam-begin");return true;
}
void pet_core_finish_walk(uint32_t instance)
{
    if (instance==g_animation_id &&
        (g_action==PET_ACTION_WALK || g_action==PET_ACTION_TURN)) {
        g_after_transition=PET_ACTION_IDLE_SIT;pet_set_action(PET_ACTION_SETTLE,"roam-arrived");
    }
}

/* 2026-11-01 P331：语音强制动作（pet:sit / sleep / stop）。
 * 走位不在这里——散步由 pet_view 的自由区接管（见 pet_view_stroll_start）。 */
static void pet_transition_to(dm_pet_state_t new_state,const char *why);

void pet_core_hold_clear(void)
{
    g_hold_mode=0;
    g_hold_until_ms=0;
}

void pet_core_command(int cmd, int seconds)
{
    (void)seconds;

    g_hold_set_ms = lv_tick_get();
    g_hold_voice_mute = (cmd == 2);

    switch (cmd) {
    case 1:   /* 坐下 */
        g_hold_mode=1;
        g_hold_until_ms=lv_tick_get()+PET_HOLD_SIT_MS;
        pet_transition_to(PET_STATE_IDLE,"voice-sit");
        pet_request_action(PET_ACTION_IDLE_SIT,"voice-sit");
        break;
    case 2:   /* 睡觉 */
        g_hold_mode=2;
        g_hold_until_ms=lv_tick_get()+PET_HOLD_SLEEP_MS;
        pet_transition_to(PET_STATE_SLEEP,"voice-sleep");
        pet_request_action(PET_ACTION_SLEEP_ENTER,"voice-sleep");
        break;
    case 4:   /* 舔爪（P344）：只请求一次性动作，不动状态与 hold */
        pet_request_action(PET_ACTION_GROOM_PAW,"voice-groom");
        break;
    case 3:   /* 停下 / 回来 */
    default:
        g_hold_mode=0;
        g_hold_until_ms=0;
        pet_transition_to(PET_STATE_IDLE,"voice-stop");
        pet_request_action(PET_ACTION_IDLE_SIT,"voice-stop");
        break;
    }
    LV_LOG_USER("[pet] voice cmd=%d hold=%d until=%ums", cmd, g_hold_mode,
        (unsigned)(g_hold_until_ms ? (g_hold_until_ms - lv_tick_get()) : 0u));
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

static int get_min(void)
{
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    return utc ? utc->tm_min : 0;
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

/* 首次运行/存档损坏：生成默认宠物（猫 + 维拉 + 初始属性） */
static void pet_default_new(void)
{
    memset(&g_pet, 0, sizeof(g_pet));
    acc_hunger = acc_energy = acc_happy = acc_health = acc_sleep = 0;
    g_pet.version   = PET_SAVE_VERSION;
    g_pet.pet_type  = PET_TYPE_CAT;
    strcpy(g_pet.name, "维拉");
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
            /* 改名迁移：旧存档里默认名是"银月"，而"银月"常被 ASR 听成"音乐"。
             * 只改默认名，用户自己起的名字不动。 */
            if (strcmp(g_pet.name, "银月") == 0)
                strcpy(g_pet.name, "维拉");
        }
    else
        strcpy(g_pet.name, "维拉");
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
            if (g_pet.attrs.hunger > 80)
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

    /* hunger 越大越饿：严重饥饿时每小时损失 1 点健康。 */
    if (g_pet.attrs.hunger > 80)
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

    /* P351：hold=2（语音"睡觉"）优先于一切。P349 只在 g_state==SLEEP 分支里
     * 认 hold，可 sleep_enter 一旦被 voice-phase 换成 wake/speak，
     * pet_core_animation_done 末尾那句 g_state=PET_STATE_IDLE 就把状态写走了，
     * 之后再没有任何路径能回到 SLEEP —— hold 形同虚设。
     * 这里让 hold 自愈：除进食/玩耍/被摸/庆祝这类有固定时长的状态外，一律拉回睡眠。 */
    /* P363：和 VOICE_PHASE 共用 hold_voice_muted()——只压住"睡觉"那一声回应，
     * 之后用户主动说话时不许把它按回睡眠（否则回复说到一半就被拉去睡）。 */
    if (hold_mode_now() == 2 && hold_voice_muted() &&
        g_state != PET_STATE_SLEEP &&
        g_state != PET_STATE_EAT && g_state != PET_STATE_PLAY &&
        g_state != PET_STATE_TOUCH && g_state != PET_STATE_CELEBRATE)
        return PET_STATE_SLEEP;

    if (g_action!=PET_ACTION_IDLE_SIT && g_action!=PET_ACTION_SLEEP_LOOP)
        return g_state;

    /* 睡觉中：打盹由 pet_nap_tick 到点叫醒；夜眠按天亮 + 精力判 */
    if (g_state == PET_STATE_SLEEP)
        {
            /* P349：语音强制睡觉期间，天亮/精力规则不许把它叫醒。
             * 到点由 hold_mode_now() 自动解除，之后回归原有夜眠逻辑。 */
            if (hold_mode_now() == 2)
                return PET_STATE_SLEEP;
            if (g_nap_active)
                return PET_STATE_SLEEP;
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

    /* 夜间强制睡觉（22:00~05:59）。22:00 整点不立刻倒，按当天随机延迟
     * 0~29 分钟再睡，避免"到点就睡"的机械感。 */
    if (h < PET_WAKE_HOUR_START)
        return PET_STATE_SLEEP;
    if (h >= PET_WAKE_HOUR_END &&
        (h > PET_WAKE_HOUR_END || get_min() >= g_night_delay_min))
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
 * 行为选择（加权随机；播放器按实际动作完成回调）
 ****************************************************************************/


/* P307: idle 池按状态调权。原来无论饿/累/心情如何都抽同一张表，
 * 宠物没有"情绪"。这里只改权重，不改动作集合与节奏。 */
static const pet_behavior_t *pick_idle_behavior(void)
{
    const dm_pet_attrs_t *a = &g_pet.attrs;
    int w[CAT_IDLE_COUNT];
    int cur = -1, total = 0;
    long idle_s = 0;

    for (int i = 0; i < (int)CAT_IDLE_COUNT; i++)
        {
            w[i] = g_cat_idle[i].weight;
            if (&g_cat_idle[i] == g_cur_behavior) cur = i;
        }

    if (g_pet.last_interaction > 0)
        idle_s = (long)(time(NULL) - g_pet.last_interaction);

    if (a->hunger > 60)     w[1] += 14;                 /* notice_user */
    if (a->energy < 30)   { w[3] += 16; w[0] -= 15; }   /* stretch / blink */
    if (a->happiness < 40) { w[2] += 10; w[0] -= 10; }  /* groom_paw */
    if (idle_s > 300)       w[1] += 18;                 /* 很久没人理 */

    /* 心情极低：呆坐、几乎不眨眼（无 sad 专属帧，靠节奏表现低落） */
    if (a->happiness < 20) { w[0] = 6; w[4] += 30; }

    for (int i = 0; i < (int)CAT_IDLE_COUNT; i++)
        {
            if (w[i] < 1) w[i] = 1;
            if (i != cur) total += w[i];
        }
    if (total <= 0) return &g_cat_idle[0];

    int r = rand() % total;
    for (int i = 0; i < (int)CAT_IDLE_COUNT; i++)
        {
            if (i == cur) continue;
            r -= w[i];
            if (r < 0) return &g_cat_idle[i];
        }
    return &g_cat_idle[0];
}

static void pet_select_behavior(void)
{
    pet_action_t action=PET_ACTION_IDLE_SIT;
    const pet_behavior_t *b=NULL;
    switch (g_state) {
    case PET_STATE_IDLE:
        b=pick_idle_behavior();action=b->action;
        if (action==PET_ACTION_STRETCH && (uint32_t)(lv_tick_get()-g_last_stretch_ms)<60000)
            action=PET_ACTION_IDLE_SIT;
        break;
    case PET_STATE_SLEEP: action=PET_ACTION_SLEEP_ENTER;break;
    case PET_STATE_EAT: action=PET_ACTION_EAT;break;
    case PET_STATE_PLAY:
    case PET_STATE_HAPPY:
    case PET_STATE_CELEBRATE: action=PET_ACTION_HAPPY;break;
    case PET_STATE_HUNGRY: action=PET_ACTION_HUNGRY;break;
    case PET_STATE_CURIOUS: action=PET_ACTION_NOTICE_USER;break;
    case PET_STATE_TOUCH: action=PET_ACTION_PET_NUZZLE;break;
    case PET_STATE_GROOM: action=PET_ACTION_GROOM_PAW;break;
    default: break; /* No invented 'sad' animation: retain a neutral sitting pose. */
    }
    g_cur_behavior=b;
    g_behavior_ticks_left=6+rand()%9;
    pet_request_action(action,"behavior");
    if (b && b->bubble) view_bubble(b->bubble,PET_BUBBLE_DURATION_TICKS);
}

static void pet_transition_to(dm_pet_state_t new_state,const char *why)
{
    dm_pet_state_t old=g_state;g_state=new_state;
    if (new_state!=old) pet_apply_state_effects();
    pet_select_behavior();
    LV_LOG_USER("[pet] %s -> %s (%s)",pet_core_state_name(old),pet_core_state_name(new_state),why);
    if (new_state==PET_STATE_SAD && old!=PET_STATE_SAD)
        view_bubble("……", PET_BUBBLE_DURATION_TICKS);
    view_hunger_alert(new_state==PET_STATE_HUNGRY);
}

/* 打盹判定：每分钟跑一次（PET_EVT_TIME_TICK）。
 * "没人碰屏幕"用 LVGL 的 inactive time——任何输入都会清零，跟控件是否
 * 消费这次点击无关；不绑自动锁屏，因为锁屏默认 30 秒就进，绑上去猫会
 * 一分钟睡一次。阈值和时长每次重新随机，所以不会天天同一时刻倒。 */
static uint32_t nap_rand(uint32_t lo, uint32_t hi)
{
    if (hi <= lo) return lo;
    return lo + (uint32_t)(rand() % (int)(hi - lo + 1));
}

static void pet_nap_tick(void)
{
    uint32_t now = lv_tick_get();
    int day = (int)((time(NULL) + 8 * 3600) / 86400);

    if (day != g_nap_day)
        {
            g_nap_day = day;
            g_nap_am = 0;
            g_nap_pm = 0;
            g_night_delay_min = (int)nap_rand(0, PET_NIGHT_DELAY_MAX - 1);
        }
    if (!g_nap_target_ms)
        g_nap_target_ms = nap_rand(PET_NAP_IDLE_MIN_MS, PET_NAP_IDLE_MAX_MS);

    if (g_state == PET_STATE_SLEEP)
        {
            if (!g_nap_active) return;                 /* 夜眠不归这里管 */
            if ((int32_t)(now - g_nap_until_ms) < 0) return;
            g_nap_active = false;
            /* P351：打盹中被语音强制睡觉，nap 到点不许自己爬起来。
             * 这条路径直接 pet_transition_to(IDLE)，绕过了 pet_next_state。 */
            if (hold_mode_now() == 2) return;
            g_nap_last_end_ms = now;
            pet_transition_to(PET_STATE_IDLE, "nap-done");
            return;
        }

    if (g_state != PET_STATE_IDLE && g_state != PET_STATE_GROOM) return;
    if (pet_interaction_active()) return;

    int h = get_hour();
    int *used = NULL;
    int cap = 0;
    if (h >= PET_WAKE_HOUR_START && h < 12)      { used = &g_nap_am; cap = PET_NAP_AM_MAX; }
    else if (h >= 12 && h < 18)                  { used = &g_nap_pm; cap = PET_NAP_PM_MAX; }
    if (!used || *used >= cap) return;

    if (g_nap_last_end_ms &&
        (uint32_t)(now - g_nap_last_end_ms) < PET_NAP_GAP_MS) return;

    if (lv_display_get_inactive_time(NULL) < g_nap_target_ms) return;

    /* 入睡：阈值和时长都重新随机，下一次不会踩同一个时间点 */
    (*used)++;
    g_nap_active = true;
    g_nap_until_ms = now + nap_rand(PET_NAP_LEN_MIN_MS, PET_NAP_LEN_MAX_MS);
    g_nap_target_ms = nap_rand(PET_NAP_IDLE_MIN_MS, PET_NAP_IDLE_MAX_MS);
    pet_transition_to(PET_STATE_SLEEP, "nap-idle");
}

void pet_core_animation_done(uint32_t instance)
{
    if (instance!=g_animation_id) return;
    if (g_action==PET_ACTION_SLEEP_ENTER) { pet_set_action(PET_ACTION_SLEEP_LOOP,"sleep-enter-done");return; }
    if (g_action==PET_ACTION_TURN) { pet_set_action(PET_ACTION_WALK,"turn-done");return; }
    if (g_action==PET_ACTION_WAKE || g_action==PET_ACTION_SETTLE) {
        bool settled=g_action==PET_ACTION_SETTLE;
        pet_action_t next=g_after_transition;
        g_after_transition=PET_ACTION_COUNT;
        if (next==PET_ACTION_COUNT) next=PET_ACTION_IDLE_SIT;
        if (!g_play_active && next==PET_ACTION_IDLE_SIT && g_state!=PET_STATE_HUNGRY && g_state!=PET_STATE_SAD)
            g_state=PET_STATE_IDLE;
        pet_set_action(next,"pose-transition-done");
        g_behavior_ticks_left=settled && next==PET_ACTION_IDLE_SIT ?
            PET_POST_WALK_IDLE_TICKS : 6+rand()%9;return;
    }
    if (g_pet_actions[g_action].mode==PET_PLAY_LOOP) return;
    if (g_play_active) { pet_set_action(PET_ACTION_IDLE_SIT,"play-ready");return; }
    if (g_state==PET_STATE_TOUCH && g_touch_held && g_action==PET_ACTION_BLINK) {
        pet_set_action(PET_ACTION_PET_NUZZLE,"relaxed-nuzzle");return;
    }
    if (g_state!=PET_STATE_HUNGRY && g_state!=PET_STATE_SAD) g_state=PET_STATE_IDLE;
    g_touch_held=false;
    if (g_voice_active) {
        g_state=PET_STATE_CURIOUS;pet_set_action(g_voice_action,"resume-voice");return;
    }
    pet_set_action(PET_ACTION_IDLE_SIT,"animation-done");
    g_behavior_ticks_left=g_state==PET_STATE_HUNGRY ? 40+rand()%40 : 6+rand()%9;
}

/* 统一投喂：冷却 + 零食成年解锁 + 食物效果 + 防“喂完就睡” + 记互动 + 进 EAT */
static void pet_apply_food(int fi)
{
    uint32_t now_ms = lv_tick_get();
    if (now_ms - g_last_feed_ms < PET_FEED_COOLDOWN_MS) return; /* 防连点刷成长 */
    g_last_feed_ms = now_ms;

    if (fi < 0 || fi >= PET_FOOD_COUNT) return;

    /* 饱食度上限保护：hunger 越小越饱，<=10 相当于饱食度 >=90% */
    if (g_pet.attrs.hunger <= 10)
        {
            view_bubble("吃饱啦~ ♥", PET_BUBBLE_DURATION_TICKS);
            return;
        }

    /* 零食只有成年才解锁，幼年/成长喂零食只给提示不给效果 */
    if (fi == PET_FOOD_SNACK && pet_core_life_stage() < PET_LIFE_ADULT)
        {
            view_bubble("还小，不吃零食~", PET_BUBBLE_DURATION_TICKS);
            return;
        }

    /* 喂睡着的猫：先叫醒再吃（睡眠互动需有惊醒反应） */
    if (g_state == PET_STATE_SLEEP)
        pet_transition_to(PET_STATE_IDLE, "wake-for-feed");

    g_play_active=false;g_touch_held=false;
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
    /* 队列消费成功后再通知，冷却或未解锁时不能显示成功反馈。 */
    if (g_view_ops && g_view_ops->show_satiety) g_view_ops->show_satiety();
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
    if (g_state != PET_STATE_SLEEP) return;
    /* P351：手摸/拖动它就是要它醒，别让语音"睡觉"的 hold 再把它按回去。 */
    pet_core_hold_clear();
    if (g_nap_active)
        {
            g_nap_active = false;
            g_nap_last_end_ms = lv_tick_get();  /* 被摸醒也算一次，重新计间隔 */
        }
    pet_transition_to(PET_STATE_IDLE, "wake-interact");
}

void pet_core_process_event(dm_pet_event_t evt, int param)
{
    switch (evt)
        {
        /* ── 触摸互动：短触摸进 TOUCH 态（窗循环 3-6s 短爆发） ── */
        case PET_EVT_TAP_HEAD:
            g_play_active=false;g_touch_held=false;
            pet_wake_for_interaction();
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 2, 0, 100);
            g_pet.attrs.affection = clamp(g_pet.attrs.affection + 1, 0, 100);
            g_pet.attrs.growth++;
            g_pet.last_interaction = time(NULL);
            pet_transition_to(PET_STATE_TOUCH, "tap-head");
            view_bubble("蹭蹭~", 3);
            break;

        case PET_EVT_TAP_BODY:
            g_play_active=false;g_touch_held=false;
            pet_wake_for_interaction();
            g_pet.last_interaction = time(NULL);
            g_state=PET_STATE_TOUCH;
            pet_request_action(PET_ACTION_NOTICE_USER,"belly-touch");
            view_bubble("肚子不给摸~", 3);
            break;

        case PET_EVT_LONG_PRESS:
            g_play_active=false;
            pet_wake_for_interaction();
            g_pet.last_interaction = time(NULL);
            if (param==2) {
                g_touch_held=false;g_state=PET_STATE_TOUCH;
                pet_request_action(PET_ACTION_NOTICE_USER,"belly-hold");
                view_bubble("这里怕痒~", 3);
                break;
            }
            g_touch_held=param!=0;
            g_pet.attrs.happiness = clamp(g_pet.attrs.happiness + 3, 0, 100);
            g_pet.attrs.affection = clamp(g_pet.attrs.affection + 2, 0, 100);
            g_pet.attrs.growth++;
            pet_transition_to(PET_STATE_TOUCH, "long-press");
            view_bubble("再摸摸~", 3);
            break;

        case PET_EVT_PET_RELAX:
            if (g_state==PET_STATE_TOUCH && g_touch_held) {
                pet_set_action(PET_ACTION_BLINK,"relaxed-blink");
                view_bubble("呼噜呼噜~", 6);
            }
            break;

        case PET_EVT_PLAY_BEGIN:
            if (g_play_active) break;
            if (g_pet.attrs.energy<20 || (g_play_rest_until && (int32_t)(lv_tick_get()-g_play_rest_until)<0)) {
                view_bubble("歇一会儿再玩~", 4);break;
            }
            if (g_pet.attrs.hunger>80 || g_state==PET_STATE_EAT || g_voice_active) {
                view_bubble(g_pet.attrs.hunger>80 ? "先吃点东西嘛~" : "等我忙完再玩~",4);break;
            }
            pet_wake_for_interaction();
            g_play_active=true;g_play_ticks=0;g_touch_held=false;
            g_state=PET_STATE_PLAY;g_pet.last_interaction=time(NULL);
            pet_request_action(PET_ACTION_NOTICE_USER,"play-begin");
            view_bubble("让我抓住你~",4);
            break;

        case PET_EVT_PLAY_PHASE:
            if (!g_play_active) break;
            if (param==PET_PLAY_CHASE && g_action==PET_ACTION_IDLE_SIT)
                pet_set_action(PET_ACTION_TURN,"chase-begin");
            else if (param==PET_PLAY_POUNCE &&
                     (g_action==PET_ACTION_WALK || g_action==PET_ACTION_IDLE_SIT)) {
                /* HAPPY is the existing paw/jump clip; no new artwork. */
                g_after_transition=PET_ACTION_COUNT;
                pet_set_action(PET_ACTION_HAPPY,"catch-finger");
                view_bubble("抓到啦~",3);
            } else if (param==PET_PLAY_LOOK &&
                       (g_action==PET_ACTION_WALK || g_action==PET_ACTION_TURN)) {
                g_after_transition=PET_ACTION_NOTICE_USER;
                pet_set_action(PET_ACTION_SETTLE,"finger-released");
            }
            break;

        case PET_EVT_PLAY_END:
            if (!g_play_active) break;
            g_play_active=false;
            if (g_play_ticks>=6) {
                g_pet.attrs.happiness=clamp(g_pet.attrs.happiness+5,0,100);
                g_pet.attrs.affection=clamp(g_pet.attrs.affection+1,0,100);
                g_pet.attrs.growth+=2;
            }
            g_touch_held=false;g_state=PET_STATE_IDLE;
            g_after_transition=PET_ACTION_IDLE_SIT;
            pet_set_action(PET_ACTION_SETTLE,"play-end");
            g_pet.last_interaction=time(NULL);
            if (param) {
                g_play_rest_until=lv_tick_get()+15000;
                view_bubble("玩累啦，歇一会儿~",6);
            }
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
            pet_request_action(PET_ACTION_SETTLE,"drag-released");
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
            if (pet_interaction_active() || g_state == PET_STATE_SLEEP) break;
            pet_transition_to(PET_STATE_HAPPY, "music-on");
            break;

        case PET_EVT_MUSIC_OFF:
            if (g_state == PET_STATE_HAPPY)
                pet_transition_to(PET_STATE_IDLE, "music-off");
            break;

        case PET_EVT_WEATHER_CHANGE:
            /* param 是 WMO code，不是温度或私有天气枚举。 */
            if (!pet_interaction_active() && g_state != PET_STATE_SLEEP)
                {
                    const char *text = NULL;
                    if (param >= 1 && param <= 3) text = "今天有云~";
                    else if (param == 45 || param == 48) text = "雾蒙蒙的~";
                    else if ((param >= 51 && param <= 67) ||
                             (param >= 80 && param <= 82)) text = "下雨了~";
                    else if ((param >= 71 && param <= 77) ||
                             param == 85 || param == 86) text = "下雪了~";
                    else if (param == 95 || param == 96 || param == 99)
                        text = "打雷了~";
                    if (text)
                        {
                            pet_transition_to(PET_STATE_CURIOUS, "weather");
                            view_bubble(text, PET_BUBBLE_DURATION_TICKS);
                        }
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
            if (g_state != PET_STATE_SLEEP && !pet_interaction_active())
                pet_transition_to(PET_STATE_CURIOUS, "presence");
            break;

        case PET_EVT_WATER_GOAL:
            if (pet_interaction_active() || g_state == PET_STATE_SLEEP) break;
            pet_transition_to(PET_STATE_CELEBRATE, "water-goal");
            break;

        case PET_EVT_SIT_ALERT:
            /* 久坐提醒：仅待机/舔毛时转担忧，不打断事件态 */
            if (g_state == PET_STATE_IDLE || g_state == PET_STATE_GROOM) {
                g_state=PET_STATE_CURIOUS;
                pet_request_action(PET_ACTION_STRETCH,"sit-alert");
                view_bubble("一起活动一下~",PET_BUBBLE_DURATION_TICKS);
            }
            break;

        case PET_EVT_AI_CHAT:
            if (g_state != PET_STATE_SLEEP && !pet_interaction_active())
                {
                    pet_wake_for_interaction();
                    pet_transition_to(PET_STATE_CURIOUS, "ai-chat");
                }
            break;

        case PET_EVT_TOUCH_RELEASE:
            g_touch_held=false;
            break;
        case PET_EVT_VOICE_PHASE:
            /* P351：语音"睡觉"期间（hold=2）整个播报阶段都不许动它。
             * 说"睡觉"后助手立刻回一句"好，我去睡了"，VOICE_PHASE(3) 会在
             * 0.4 秒内把 sleep_enter 换成 wake（见 pet_request_action 的过渡分支），
             * 串口实测 10.938 睡 / 11.369 抬头。画面看就是"叫它睡它不睡"。 */
            /* P351：说"睡觉"后助手立刻回一句，那条回话的 VOICE_PHASE(3) 会把
             * 猫从 sleep_enter 里拽出来（"叫它睡它不睡"）。P363：但 hold 只
             * 该压住那一声回应，压不住用户之后主动发起的对话——否则
             * "说睡觉 → 再跟它说话 → 猫一直不动"（实测 11:40 就卡在这里）。 */
            if (param == 0)
                g_hold_voice_mute = false;   /* 这一轮说完了，解除压制 */
            if (hold_mode_now() == 2 && hold_voice_muted())
                break;
            if (param==0) {
                if (g_voice_active) {
                    g_voice_active=false;
                    /* P363：本来在睡觉就别回 IDLE（会先挑一个待机动作再躺下，
                     * 画面闪两下），直接回 SLEEP。 */
                    if (!g_voice_resume_sleep &&
                        (g_action==PET_ACTION_LISTEN || g_action==PET_ACTION_THINK ||
                         g_action==PET_ACTION_SPEAK || g_action==PET_ACTION_WAKE))
                        pet_transition_to(PET_STATE_IDLE,"voice-finished");
                }
                if (g_voice_resume_sleep) {
                    g_voice_resume_sleep=false;
                    /* 打盹到点在语音期间可能已经过了，补 1 分钟再叫醒，
                     * 否则刚躺下就被 nap_tick 拉起来。 */
                    if (g_nap_active && (int32_t)(lv_tick_get()-g_nap_until_ms)>=0)
                        g_nap_until_ms = lv_tick_get()+60000;
                    pet_transition_to(PET_STATE_SLEEP,"voice-sleep-back");
                }
            } else if (param>=1 && param<=3) {
                static const pet_action_t phases[]={PET_ACTION_IDLE_SIT,PET_ACTION_LISTEN,PET_ACTION_THINK,PET_ACTION_SPEAK};
                g_voice_active=true;g_voice_action=phases[param];
                g_voice_deadline=lv_tick_get()+(param==1 ? 60000u : 45000u);
                /* P363：睡着被叫醒说话 —— 记下来，播完躺回去 */
                if (g_state==PET_STATE_SLEEP)
                    g_voice_resume_sleep=true;
                if (g_state!=PET_STATE_EAT && g_state!=PET_STATE_PLAY &&
                    g_state!=PET_STATE_TOUCH && g_state!=PET_STATE_CELEBRATE) {
                    g_state=PET_STATE_CURIOUS;
                    pet_request_action(g_voice_action,"voice-phase");
                }
            }
            break;
        case PET_EVT_TIME_TICK:
            pet_nap_tick();
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
    if (g_voice_active && (int32_t)(lv_tick_get()-g_voice_deadline)>=0) {
        pet_core_process_event(PET_EVT_VOICE_PHASE,0);
    }
    if (g_play_active) {
        if (++g_play_ticks%4==0)
            g_pet.attrs.energy=clamp(g_pet.attrs.energy-1,0,100);
        if (g_pet.attrs.energy<=15 || g_play_ticks>=60)
            pet_core_process_event(PET_EVT_PLAY_END,1);
        return;
    }
    /* Only idle scheduling uses this timer. Authored actions finish on playback completion. */
    if (g_action!=PET_ACTION_IDLE_SIT || g_voice_active || g_touch_held) return;
    if (g_behavior_ticks_left>0 && --g_behavior_ticks_left>0) return;
    pet_select_behavior();
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
    g_after_transition=PET_ACTION_COUNT;
    if (g_state==PET_STATE_IDLE) {
        pet_set_action(PET_ACTION_IDLE_SIT,"initial");g_behavior_ticks_left=6+rand()%9;
    } else pet_select_behavior();
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
    pet_set_action(PET_ACTION_IDLE_SIT,"reset");
    g_after_transition=PET_ACTION_COUNT;g_touch_held=false;g_voice_active=false;
    g_play_active=false;g_play_ticks=0;g_play_rest_until=0;
    g_cur_behavior = NULL;
    g_behavior_ticks_left = 0;
    g_welcome_back_pending = false;
    pet_core_save();
    view_hunger_alert(false);
    LV_LOG_USER("[pet] reset to new pet");
}
