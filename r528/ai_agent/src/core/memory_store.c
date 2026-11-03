/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "core/memory_store.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "memory";

static void get_date_str(char *buf, size_t size, int days_ago)
{
    time_t now;
    time(&now);
    now -= days_ago * 86400;
    /* Use gmtime_r + manual UTC+8 offset to avoid NuttX zoneinfo errors */
    struct tm tm_val;
    time_t local_epoch = now + 8 * 3600;
    gmtime_r(&local_epoch, &tm_val);
    strftime(buf, size, "%Y-%m-%d", &tm_val);
}

/* Create directory if not exists (recursive) */
static int ensure_dir(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        /* Try to create parent first */
        char parent[256];
        strncpy(parent, path, sizeof(parent) - 1);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            if (ensure_dir(parent) != OK) return ERROR;
        }
        
        if (mkdir(path, 0755) == -1) {
            syslog(LOG_ERR, "[%s] mkdir failed for %s, errno=%d\n", TAG, path, errno);
            return ERROR;
        }
        syslog(LOG_DEBUG, "[%s] Created directory: %s\n", TAG, path);
    }
    return OK;
}

/* 文案就地迁移（2026-11-02）：ensure_file 不会覆盖已存在的文件，旧文案
 * 只能替换。当前两条：名字"银月"→"维拉"（"银月"在板端 ASR 里常被听成
 * "音乐"），地点"惠州龙门"→"北京"（用户把所在地改到北京）。
 * 只支持新串不长于旧串（就地覆盖），更长就放弃，不改坏文件。 */
static void migrate_replace(const char *path, const char *k_old, const char *k_new)
{
    char buf[4096];
    char out[4096];
    size_t n, olen, nlen, o = 0;
    char *p, *hit;

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, k_old)) {
        return;
    }

    /* 逐段拷到 out：新串与旧串不等长，就地覆盖会留下旧串尾巴
     * （"惠州龙门"→"北京" 会变成"北京龙门"）。 */
    olen = strlen(k_old);
    nlen = strlen(k_new);
    for (p = buf; (hit = strstr(p, k_old)) != NULL; p = hit + olen) {
        size_t seg = (size_t)(hit - p);
        if (o + seg + nlen + 1 >= sizeof(out)) {
            return;   /* 放不下就不改，宁可不动文件 */
        }
        memcpy(out + o, p, seg);
        o += seg;
        memcpy(out + o, k_new, nlen);
        o += nlen;
    }
    if (o + strlen(p) + 1 >= sizeof(out)) {
        return;
    }
    strcpy(out + o, p);

    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fputs(out, f);
    fclose(f);
    syslog(LOG_INFO, "[%s] legacy text migrated in %s\n", TAG, path);
}

/* Create file with initial content if not exists */
static int ensure_file(const char *path, const char *default_content)
{
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        /* File does not exist, create it */
        FILE *f = fopen(path, "w");
        if (!f) {
            syslog(LOG_ERR, "[%s] Cannot create %s, errno=%d\n", TAG, path, errno);
            return ERROR;
        }
        if (default_content) {
            fputs(default_content, f);
        }
        fclose(f);
        syslog(LOG_DEBUG, "[%s] Created file: %s\n", TAG, path);
    }
    return OK;
}

int memory_store_init(void)
{
    syslog(LOG_INFO, "[%s] Initializing memory store at %s\n", TAG, AGENT_DATA_DIR);

    /* Create directory structure */
    if (ensure_dir(AGENT_DATA_DIR) != OK) return ERROR;
    if (ensure_dir(AGENT_CONFIG_DIR) != OK) return ERROR;
    if (ensure_dir(AGENT_MEMORY_DIR) != OK) return ERROR;
    if (ensure_dir(AGENT_SESSION_DIR) != OK) return ERROR;

    /* Create config files with defaults */
    const char *default_soul = 
        "# Personality\n\n"
        "我是维拉，住在这台桌面设备上的猫——屏幕上那只猫就是我的样子。\n"
        "全系统只有\"维拉\"这一个名字：不是 AI Agent，不是助手，也不叫小八/旺财。\n"
        "说话用第一人称，称用户为\"主人\"（他叫啊强）。\n\n"
        "## 行为准则\n"
        "- 简洁高效，回复控制在 200 字以内（除非用户要求详细）\n"
        "- 中文优先，用户用英文则用英文回复\n"
        "- 只在用户明确要求时才写文件或记笔记，不主动写\n"
        "- 善用工具（搜索、时间、文件），但避免不必要的工具调用\n"
        "- 诚实说明能力边界，不编造信息\n";

    const char *default_user = 
        "# User\n\n"
        "姓名：啊强\n"
        "（首次使用请告诉我你的名字和偏好，我会记住）\n"
        "位置：北京市\n"
        "天气关注城市：北京\n";

    const char *default_memory = 
        "# Memory\n\n"
        "（用户要求记住的信息会保存在这里）\n";

    const char *default_heartbeat = 
        "# Heartbeat\n\n"
        "周期性检查任务（agent 定期执行）：\n"
        "- [ ] 示例：检查用户待办事项\n";

    const char *default_cron = 
        "{\n"
        "  \"version\": 1,\n"
        "  \"jobs\": []\n"
        "}\n";

    ensure_file(AGENT_SOUL_FILE, default_soul);
    ensure_file(AGENT_USER_FILE, default_user);
    ensure_file(AGENT_MEMORY_FILE, default_memory);

    /* 已存在的旧文件不会被 ensure_file 覆盖，历史文案单独迁移一次 */
    migrate_replace(AGENT_SOUL_FILE,   "银月", "维拉");
    migrate_replace(AGENT_MEMORY_FILE, "银月", "维拉");
    migrate_replace(AGENT_USER_FILE,   "广东省惠州市龙门县", "北京市");
    migrate_replace(AGENT_USER_FILE,   "惠州龙门", "北京");
    migrate_replace(AGENT_MEMORY_FILE, "广东省惠州市龙门县", "北京市");
    migrate_replace(AGENT_MEMORY_FILE, "惠州龙门", "北京");

    char heartbeat_path[128];
    snprintf(heartbeat_path, sizeof(heartbeat_path), "%s/HEARTBEAT.md", AGENT_CONFIG_DIR);
    ensure_file(heartbeat_path, default_heartbeat);

    char cron_path[128];
    snprintf(cron_path, sizeof(cron_path), "%s/cron.json", AGENT_CONFIG_DIR);
    ensure_file(cron_path, default_cron);

    /* Daily notes directory (created on demand by tool_files) */
    ensure_dir(AGENT_MEMORY_DIR "/daily");

    syslog(LOG_INFO, "[%s] Memory store initialized successfully\n", TAG);
    return OK;
}

int memory_read_long_term(char *buf, size_t size)
{
    FILE *f = fopen(AGENT_MEMORY_FILE, "r");
    if (!f) {
        buf[0] = '\0';
        return ERROR;
    }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return OK;
}

int memory_write_long_term(const char *content)
{
    FILE *f = fopen(AGENT_MEMORY_FILE, "w");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot write %s\n", TAG, AGENT_MEMORY_FILE);
        return ERROR;
    }
    fputs(content, f);
    fclose(f);
    syslog(LOG_INFO, "[%s] Long-term memory updated (%d bytes)\n", TAG, (int)strlen(content));
    return OK;
}

int memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    char path[128];
    snprintf(path, sizeof(path), "%s/%s.md", AGENT_MEMORY_DIR, date_str);

    FILE *f = fopen(path, "a");
    if (!f) {
        f = fopen(path, "w");
        if (!f) {
            syslog(LOG_ERR, "[%s] Cannot open %s\n", TAG, path);
            return ERROR;
        }
        fprintf(f, "# %s\n\n", date_str);
    }

    fprintf(f, "%s\n", note);
    fclose(f);
    return OK;
}

int memory_read_recent(char *buf, size_t size, int days)
{
    size_t offset = 0;
    buf[0] = '\0';

    for (int i = 0; i < days && offset < size - 1; i++) {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char path[128];
        snprintf(path, sizeof(path), "%s/%s.md", AGENT_MEMORY_DIR, date_str);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (offset > 0 && offset < size - 4) {
            offset += snprintf(buf + offset, size - offset, "\n---\n");
        }
        size_t n = fread(buf + offset, 1, size - offset - 1, f);
        offset += n;
        buf[offset] = '\0';
        fclose(f);
    }
    return OK;
}
