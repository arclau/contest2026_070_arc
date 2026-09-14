/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_health_cfg.c
 * DesktopMate — 配置文件实现（/data/dm_health.cfg）
 *
 * 格式：key=value 每行一个；# 开头为注释；空行忽略。
 * 加载策略：
 *   1) 文件不存在 → 写入默认模板（带注释，方便用户直接编辑）
 *   2) 逐行解析，缺项/非法值 → 调用方回退硬编码默认值
 * 存储：静态 key/value 表（内存快照），set_* 更新内存并整文件重写。
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>

#include <lvgl/lvgl.h>   /* LV_LOG_USER */

#include "dm_health_cfg.h"

/****************************************************************************
 * 配置表（内存快照）
 ****************************************************************************/

#define DM_CFG_MAX_ENTRIES 96   /* 2026-08-28：60 个 key（Voice Director 五组参数并入） */
#define DM_CFG_KEY_MAX     48
#define DM_CFG_VAL_MAX     224

typedef struct
{
    char key[DM_CFG_KEY_MAX];
    char val[DM_CFG_VAL_MAX];
} dm_cfg_entry_t;

static dm_cfg_entry_t g_cfg[DM_CFG_MAX_ENTRIES];
static int            g_cfg_count;
static bool           g_cfg_loaded;

/* 默认模板：生成到 /data/dm_health.cfg 供用户直接编辑。
 * 注意：模板内容即默认值，加载缺项时调用方需提供相同硬编码兜底。 */
static const char *g_cfg_template[] =
{
    "# DesktopMate配置 — 改完重启或下一次读取即生效",
    "# 删掉本行即可改；文件删除 = 恢复全部默认",
    "",
    "# ── 坐下欢迎语（开机首次，2026-08-28 银月化）──",
    "greet_first=诶嘿，来啦~今天也要加油哦。",
    "# ── 分时段问候（Voice Director 候选池文案参考）──",
    "greet_morning=早呀，又开工啦？",
    "greet_noon=饭点啦，先去吃饭嘛。",
    "greet_afternoon=下午好，腰还好吗？别急，过两小时我再提醒你起来活动。",
    "greet_evening=天都黑了还在卷，这工位是焊在你身上了吗？",
    "greet_night=都这么晚了，早点休息呀。",
    "# ── 欢迎语每时段限播次数（P102：离座/入座反复不重复提醒，跨时段重置）──",
    "greet_max_per_period=2",
    "# ── 提醒弹窗文案（L1/L2/L3，2026-08-28 银月化）──",
    "popup_water_1=喝口水吧~",
    "popup_water_2=水还没喝呢，记得呀。",
    "popup_water_3=水都凉啦，先喝两口吧。",
    "popup_sit_1=起来走走嘛。",
    "popup_sit_2=坐得有点久了，动一动呀。",
    "popup_sit_3=腰在抗议啦，起来活动下。",
    "# ── 提醒参数（秒/厘米/℃）──",
    "water_l1_s=2700",
    "water_hot_l1_s=1800",
    "water_hot_temp_c=30",
    "water_escalate_s=900",
    "sit_l1_s=1800",
    "sit_l2_s=3600",
    "sit_l3_s=5400",
    "ld2410_seat_cm=150",
    "prox_seat_debounce_s=5",
    "prox_absent_debounce_s=30",
    "# ── Voice Director 场景参数（S，2026-08-28）──",
    "away_short_max_s=300",
    "away_long_min_s=1800",
    "sit_long_for_revisit_s=3600",
    "sit_cnt_quiet_th=6",
    "decline_revisit_s=900",
    "# ── Voice Director 静默检查（Q）──",
    "recent_ai_interaction_cooldown=300",
    "voice_cooldown_s=600",
    "back_cooldown_s=180",
    "music_sit_silent=1",
    "night_short_only=1",
    "# ── Voice Director 分类预算（B，替代固定每日 12 条）──",
    "budget_greeting=3",
    "budget_health_reminder=0",
    "budget_teasing=4",
    "budget_caring=4",
    "budget_system_response=5",
    "# ── Voice Director 单 wav 冷却（C，秒）──",
    "cooldown_particle_s=300",
    "cooldown_short_s=1800",
    "cooldown_reminder_s=900",
    "# ── Voice Director 时段情绪权重（W，相对系数）──",
    "w_morning_teasing=0.5",
    "w_morning_caring=1.0",
    "w_morning_silent=0.8",
    "w_forenoon_teasing=1.0",
    "w_forenoon_caring=0.8",
    "w_forenoon_silent=0.8",
    "w_noon_teasing=0.6",
    "w_noon_caring=1.5",
    "w_noon_silent=0.8",
    "w_afternoon_teasing=1.5",
    "w_afternoon_caring=0.6",
    "w_afternoon_silent=0.8",
    "w_evening_teasing=1.0",
    "w_evening_caring=1.2",
    "w_evening_silent=1.0",
    "w_night_teasing=0.2",
    "w_night_caring=2.0",
    "w_night_silent=2.0",
    NULL
};

/****************************************************************************
 * 内部工具
 ****************************************************************************/

static int cfg_find(const char *key)
{
    for (int i = 0; i < g_cfg_count; i++)
        if (strcmp(g_cfg[i].key, key) == 0)
            return i;
    return -1;
}

static int cfg_upsert(const char *key, const char *val)
{
    int i = cfg_find(key);
    if (i >= 0)
        {
            strncpy(g_cfg[i].val, val, DM_CFG_VAL_MAX - 1);
            g_cfg[i].val[DM_CFG_VAL_MAX - 1] = '\0';
            return 0;
        }
    if (g_cfg_count >= DM_CFG_MAX_ENTRIES)
        return -1;
    strncpy(g_cfg[g_cfg_count].key, key, DM_CFG_KEY_MAX - 1);
    g_cfg[g_cfg_count].key[DM_CFG_KEY_MAX - 1] = '\0';
    strncpy(g_cfg[g_cfg_count].val, val, DM_CFG_VAL_MAX - 1);
    g_cfg[g_cfg_count].val[DM_CFG_VAL_MAX - 1] = '\0';
    g_cfg_count++;
    return 0;
}

/* 去掉行首/行尾空白 */
static char *cfg_trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
    return s;
}

static void cfg_write_file(void)
{
    FILE *f = fopen(DM_HEALTH_CFG_FILE, "w");
    if (!f)
        return;
    for (int i = 0; i < g_cfg_count; i++)
        fprintf(f, "%s=%s\n", g_cfg[i].key, g_cfg[i].val);
    fclose(f);
}

static void cfg_write_template(void)
{
    FILE *f = fopen(DM_HEALTH_CFG_FILE, "w");
    if (!f)
        return;
    for (int i = 0; g_cfg_template[i]; i++)
        fprintf(f, "%s\n", g_cfg_template[i]);
    fclose(f);
    LV_LOG_USER("[cfg] template written to %s", DM_HEALTH_CFG_FILE);
}

static void cfg_load_file(void)
{
    FILE *f = fopen(DM_HEALTH_CFG_FILE, "r");
    if (!f)
        return;
    char line[DM_CFG_KEY_MAX + DM_CFG_VAL_MAX + 8];
    while (fgets(line, sizeof(line), f))
    {
        char *p = cfg_trim(line);
        if (*p == '\0' || *p == '#')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = cfg_trim(p);
        char *val = cfg_trim(eq + 1);
        if (*key && *val)
            cfg_upsert(key, val);
    }
    fclose(f);
}

/****************************************************************************
 * 公共 API
 ****************************************************************************/

void dm_health_cfg_load(void)
{
    if (g_cfg_loaded)
        return;
    g_cfg_loaded = true;

    cfg_load_file();
    if (g_cfg_count == 0)
        cfg_write_template();   /* 首次：生成默认模板供编辑 */

    LV_LOG_USER("[cfg] loaded %d entries from %s", g_cfg_count,
                DM_HEALTH_CFG_FILE);
}

int dm_health_cfg_get_int(const char *key, int def)
{
    int i = cfg_find(key);
    if (i < 0)
        return def;
    char *end = NULL;
    long v = strtol(g_cfg[i].val, &end, 10);
    if (end == g_cfg[i].val)
        return def;             /* 非数字 → 兜底 */
    if (v > INT_MAX || v < INT_MIN)
        return def;             /* long→int 溢出保护 */
    return (int)v;
}

const char *dm_health_cfg_get_str(const char *key, const char *def)
{
    int i = cfg_find(key);
    if (i < 0)
        return def;
    return g_cfg[i].val;
}

int dm_health_cfg_set_int(const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return dm_health_cfg_set_str(key, buf);
}

int dm_health_cfg_set_str(const char *key, const char *value)
{
    if (!key || !value)
        return -1;
    if (cfg_upsert(key, value) != 0)
        return -1;
    cfg_write_file();
    LV_LOG_USER("[cfg] set %s=%s", key, value);
    return 0;
}
