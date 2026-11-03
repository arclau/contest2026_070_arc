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

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent_compat.h"
#include "agent_config.h"

#include "core/agent_loop.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "channels/nsh_commands.h"
#include "infra/config_store.h"
#include "infra/cron_service.h"
#ifdef CONFIG_AI_AGENT_FEISHU
#include "channels/feishu_bot.h"
#endif
#include "channels/ws_server.h"
#include "infra/heartbeat.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "core/memory_store.h"
#include "core/session_mgr.h"
#ifdef CONFIG_AI_AGENT_MQTT
#include "channels/mqtt_channel.h"
#endif
#include "infra/network_manager.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_client.h"
#include "node/node_manager.h"
#endif
#include "infra/http_proxy.h"
#include "tools/tool_guard.h"
#include "tools/skill_loader.h"
#include "tools/tool_media.h"
#include "tools/tool_registry.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_bridge.h"
#endif
#include "llm/llm_cache.h"
#include "infra/vela_tls.h"
#include "infra/pair_server.h"
#if AGENT_SKILL_SYNC_ENABLED
#include "tools/skill_sync.h"
#endif
#include "voice/voice_channel.h"
#ifdef CONFIG_AI_AGENT_WEIXIN
#include "channels/weixin_channel.h"
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif

static const char* TAG = "agent";

/* ── stdout mutex — shared with nsh_commands.c ──────────────── */
/* Prevents concurrent printf from outbound_dispatch_task and cli_thread
 * which causes adbd shell_service_uv assert (wait_ack != 0). */
pthread_mutex_t g_stdout_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Global shutdown flag ─────────────────────────────────────── */
/* Set by cmd_quit via agent_request_shutdown(); checked by main loop
 * to trigger graceful teardown instead of calling exit(). */
static volatile bool g_shutdown_requested = false;

void agent_request_shutdown(void)
{
    g_shutdown_requested = true;
}

bool agent_shutdown_requested(void)
{
    return g_shutdown_requested;
}

/* ── Network watcher (async) ──────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static volatile bool g_net_services_started = false;

static void net_state_change_cb(net_state_t state, void* arg)
{
    (void)arg;
    if (state == NET_STATE_CONNECTED && !g_net_services_started) {
        syslog(LOG_INFO, "[%s] Network recovered, starting services\n", TAG);
#ifdef CONFIG_AI_AGENT_FEISHU
        feishu_bot_start();
#endif
        agent_loop_start();
        ws_server_start();
#ifdef CONFIG_AI_AGENT_NODE
        node_client_start();
#endif
#ifdef CONFIG_AI_AGENT_MQTT
        mqtt_channel_start();
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        weixin_channel_start();
#endif
        g_net_services_started = true;
    } else if (state == NET_STATE_DISCONNECTED) {
        syslog(LOG_WARNING,
            "[%s] Network lost, services may be affected\n", TAG);
        /* Services will handle disconnection via their own error paths.
         * We just log the event here. g_net_services_started stays true
         * so we don't re-start services that are already running. */
    }
}
#endif

/**
 * Runs in a background thread: waits for network, then starts all
 * network-dependent services. Main thread is NOT blocked.
 */
static void* network_watch_task(void* arg)
{
    (void)arg;
    syslog(LOG_DEBUG, "[%s] Network watcher started\n", TAG);

    network_wifi_reconnect();

    if (network_wait_connected(30000) == OK) {
        syslog(LOG_NOTICE, "[%s] Network connected: %s\n", TAG, network_get_ip());

#if AGENT_SKILL_SYNC_ENABLED
        /* Sync skills from Bitable before starting agent loop */
        if (skill_sync_from_bitable() != OK) {
            syslog(LOG_WARNING, "[%s] Bitable skill sync failed, using local skills\n", TAG);
        }
#endif

#ifdef CONFIG_AI_AGENT_FEISHU
        if (feishu_bot_start() != OK)
            syslog(LOG_WARNING, "[%s] feishu_bot_start failed\n", TAG);
#endif
        if (agent_loop_start() != OK)
            syslog(LOG_WARNING, "[%s] agent_loop_start failed\n", TAG);
        if (ws_server_start() != OK)
            syslog(LOG_WARNING, "[%s] ws_server_start failed\n", TAG);
        /* 2026-11-01 P330：手机扫码配对页（Settings→AI 语音 的二维码指向它） */
        if (pair_server_start() != OK)
            syslog(LOG_WARNING, "[%s] pair_server_start failed\n", TAG);
#ifdef CONFIG_AI_AGENT_NODE
        if (node_client_start() != OK)
            syslog(LOG_WARNING, "[%s] node_client_start failed\n", TAG);
#endif
#ifdef CONFIG_AI_AGENT_MQTT
        if (mqtt_channel_start() != OK)
            syslog(LOG_WARNING, "[%s] mqtt_channel_start failed\n", TAG);
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        if (weixin_channel_start() != OK)
            syslog(LOG_WARNING, "[%s] weixin_channel_start failed\n", TAG);
#endif

        syslog(LOG_NOTICE, "[%s] All network services started!\n", TAG);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
        g_net_services_started = true;
#endif
    } else {
        syslog(LOG_WARNING, "[%s] Network timeout — net services not started.\n", TAG);
        syslog(LOG_WARNING, "[%s] Use 'config_show' / 'set_*' CLI commands to configure.\n", TAG);
    }

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    /* Register listener for network state changes — auto-restart services on reconnect */
    network_register_listener(net_state_change_cb, NULL);

    /* Keep thread alive to handle reconnection events */
    while (!g_shutdown_requested) {
        sleep(1);
    }
#endif

    return NULL;
}

/* ── Outbound dispatch ────────────────────────────────────────── */

/* ── Voice failure cooldown — prevent error-message cascade ──── */

static volatile bool s_voice_cooldown;
static time_t s_voice_cooldown_until;

/**
 * Reads messages from the outbound queue and dispatches them to the
 * appropriate channel (飞书, WebSocket, CLI, etc.).
 */
static void* outbound_dispatch_task(void* arg)
{
    (void)arg;
    syslog(LOG_DEBUG, "[%s] Outbound dispatch started\n", TAG);

    while (!g_shutdown_requested) {
        agent_msg_t msg;
        if (message_bus_pop_outbound(&msg, 1000) != OK)
            continue;
        msg.channel[sizeof(msg.channel) - 1] = '\0';
        msg.chat_id[sizeof(msg.chat_id) - 1] = '\0';

        if (!msg.content) {
            syslog(LOG_WARNING, "[%s] Skip outbound message with NULL content\n", TAG);
            continue;
        }

        syslog(LOG_INFO, "[%s] Dispatching response → %s:%s\n", TAG, msg.channel, msg.chat_id);

        /* Let registered taps intercept before normal dispatch */
        if (mbus_tap_try_deliver(&msg)) {
            free(msg.content);
            continue;
        }

        if (strcmp(msg.channel, AGENT_CHAN_FEISHU) == 0) {
            bool delivered = false;
#ifdef CONFIG_AI_AGENT_FEISHU
            const char* app_id = feishu_get_app_id();
            if (app_id && app_id[0] != '\0') {
                feishu_send_message(msg.chat_id, msg.content);
                delivered = true;
            }
#endif
#ifdef CONFIG_AI_AGENT_NODE
            if (!delivered) {
                delivered = node_client_send_chat_message(
                    msg.channel, msg.chat_id, msg.content) == OK;
            }
#endif
            if (!delivered) {
                syslog(LOG_WARNING,
                    "[%s] Feishu message dropped: no local config "
                    "and no gateway\n",
                    TAG);
            }
        } else if (strcmp(msg.channel, AGENT_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content);
#ifdef CONFIG_AI_AGENT_MQTT
        } else if (strcmp(msg.channel, AGENT_CHAN_MQTT) == 0) {
            mqtt_channel_send(msg.chat_id, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
            /* Check voice cooldown — skip voice dispatch if a recent
             * speak call failed, to prevent error-message cascade. */
            if (s_voice_cooldown && time(NULL) < s_voice_cooldown_until) {
                syslog(LOG_WARNING,
                    "[%s] voice cooldown active, dropping message\n", TAG);
            } else {
                s_voice_cooldown = false;
                /* 2026-08-15 P84：LLM 回复文本广播给 UI 显示
                 * （小智式"AI 回答显字"——播报同时文字上屏） */
                ws_server_broadcast_voice_evt("llm", msg.content);
                /* P342：流式回合在生成过程中已经分句播报过，这里只上屏，
                 * 否则同一段回复会被整段再播一遍。 */
                if (strcmp(msg.chat_id, "voice:spoken") == 0) {
                    syslog(LOG_INFO,
                        "[%s] voice reply already streamed, skip re-speak\n",
                        TAG);
                } else {
                    int vret = voice_channel_speak(msg.content);

                    if (vret != 0) {
                        syslog(LOG_ERR, "[%s] voice_channel_speak failed: %d\n", TAG, vret);
                        s_voice_cooldown = true;
                        s_voice_cooldown_until = time(NULL) + 5;
                    }
                }
            }
#ifdef CONFIG_AI_AGENT_LVGL_UI
        } else if (strcmp(msg.channel, AGENT_CHAN_LVGL_UI) == 0) {
            int uret = lvgl_ui_channel_send(msg.content);
            if (uret != 0) {
                syslog(LOG_ERR, "[%s] lvgl_ui_channel_send failed: %d\n", TAG, uret);
            }
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
        } else if (strcmp(msg.channel, AGENT_CHAN_WEIXIN) == 0) {
            /* chat_id format: "from_user_id|context_token" */
            char uid[64] = "";
            const char* ctx = "";
            char* sep = strchr(msg.chat_id, '|');
            if (sep) {
                size_t ulen = (size_t)(sep - msg.chat_id);
                if (ulen >= sizeof(uid))
                    ulen = sizeof(uid) - 1;
                memcpy(uid, msg.chat_id, ulen);
                uid[ulen] = '\0';
                ctx = sep + 1;
            } else {
                strncpy(uid, msg.chat_id, sizeof(uid) - 1);
            }
            weixin_channel_send(uid, ctx, msg.content);
#endif
        } else if (strcmp(msg.channel, AGENT_CHAN_SYSTEM) == 0) {
            syslog(LOG_INFO, "[%s] System message [%s]: %.128s\n", TAG, msg.chat_id, msg.content);
        } else if (strcmp(msg.channel, "cli") == 0) {
            pthread_mutex_lock(&g_stdout_lock);
            printf("\n[Agent]: %s\nvela> ", msg.content);
            fflush(stdout);
            pthread_mutex_unlock(&g_stdout_lock);
            syslog(LOG_INFO, "[agent] [Agent]: %s\n", msg.content);
        } else {
            syslog(LOG_WARNING, "[%s] Unknown channel: %s\n", TAG, msg.channel);
        }

        free(msg.content);
    }

    syslog(LOG_INFO, "[%s] Outbound dispatch exiting\n", TAG);
    return NULL;
}

/* ── Entry point ──────────────────────────────────────────────── */

/* ── Startup timing helpers ───────────────────────────────────── */

static inline long boot_ms(struct timespec* t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)((now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L);
}

#define BOOT_LOG(t0, phase, msg) do { (void)(t0); (void)(phase); (void)(msg); } while (0)

/* 2026-08-15 降噪：成功 (rc==OK) 不再打印（boot 阶段模块已全部确认 OK），
 * 仅失败 (rc!=OK) 输出——保留错误排查能力，去掉启动噪音 */
#define BOOT_LOG_RC(t0, phase, msg, rc) \
    do { if ((rc) != OK) \
        syslog(LOG_WARNING, "[%s] [boot +%ldms] " phase ": " msg " (rc=%d)\n", \
            TAG, boot_ms(t0), (rc)); \
    } while (0)

int ai_agent_main(int argc, char* argv[])
{
    /* 2026-08-12 TASK-01：一次性测试模式（NSH 直跑）——
     * P59 自启 ai_agent </dev/null & 已关闭进程内 CLI（cli_thread 读
     * stdin 遇 EOF 退出），voice_test_* 在 NSH 全部不可达
     * （实测 "voice_test_tts: command not found"）。子命令模式不
     * listen WS、不抢 stdin、不 init 全部服务，仅注册 voice backend
     * 后执行测试函数退出；与自启服务实例不冲突（WS 不占、voice
     * 设备懒打开，idle 时可复用）。 */
    if (argc >= 2 && strcmp(argv[1], "--test") == 0) {
        if (argc < 3) {
            printf("Usage: ai_agent --test {config|tts|asr|llm|full} ...\n");
            return 0;
        }

        voice_channel_init();   /* 仅注册 backend + 重置状态，不占设备 */

        if (strcmp(argv[2], "config") == 0) {
            char mimo[160] = { 0 };
            char host[128] = { 0 };
            char model[64] = { 0 };
            char asr[24] = { 0 };
            char tts[24] = { 0 };
            int have_mimo;

            have_mimo = claw_config_get(AGENT_CFG_KEY_MIMO_KEY,
                mimo, sizeof(mimo)) == OK && mimo[0] != '\0';
            claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host));
            claw_config_get(AGENT_CFG_KEY_MODEL, model, sizeof(model));
            claw_config_get(AGENT_CFG_KEY_VOICE_ASR_BACKEND,
                asr, sizeof(asr));
            claw_config_get(AGENT_CFG_KEY_VOICE_TTS_BACKEND,
                tts, sizeof(tts));

            printf("CONFIG file: %s\n", AGENT_CONFIG_FILE);
            printf("CONFIG mimo_key=%s llm=%s%s model=%s asr=%s tts=%s\n",
                have_mimo ? "set" : "missing",
                have_mimo ? AGENT_LLM_MIMO_HOST :
                    (host[0] ? host : "(unset)"),
                have_mimo ? AGENT_LLM_MIMO_PATH : "",
                have_mimo ? AGENT_LLM_MIMO_MODEL :
                    (model[0] ? model : "(unset)"),
                asr[0] ? asr : "(unset)", tts[0] ? tts : "(unset)");
            return 0;
        }

        if (strcmp(argv[2], "tts") == 0) {
            const char* text = (argc >= 4) ? argv[3] : "你好";

            printf("TEST TTS: \"%s\"\n", text);
            return voice_channel_test_tts(text,
                AGENT_DATA_DIR "/tts_out.pcm");
        }

        if (strcmp(argv[2], "asr") == 0) {
            const char* path = (argc >= 4) ? argv[3] : NULL;

            if (!path) {
                printf("Usage: ai_agent --test asr <pcm_or_wav>\n");
                return 0;
            }

            printf("TEST ASR: %s\n", path);
            /* voice_channel_test_asr 是后台线程（asr_file_worker），
             * main 返回会杀线程——sleep 等待其完成（一次 ASR ~2-5s） */
            int rc = voice_channel_test_asr(path);

            sleep(15);
            return rc;
        }

        if (strcmp(argv[2], "llm") == 0) {
            /* TASK-03：LLM 冻结区（P54 上板实测 ask 1.8s 通）——只补
             * 同步测试入口，不改已验证协议。llm_chat 同步返回回复。 */
            const char* q = (argc >= 4) ? argv[3] : "你好";
            char messages[512];
            char resp[2048];

            snprintf(messages, sizeof(messages),
                "[{\"role\":\"user\",\"content\":\"%s\"}]", q);
            printf("TEST LLM: %s\n", q);
            llm_proxy_init();   /* 加载 Key/模型（冻结区，不重新验证） */
            int rc = llm_chat("You are a helpful assistant.", messages,
                resp, sizeof(resp));

            printf("LLM result: %s\n", resp);
            return rc;
        }

        if (strcmp(argv[2], "full") == 0) {
            /* TASK-04：端到端——capture Ns → ASR → LLM → TTS → speaker */
            int secs = (argc >= 4) ? atoi(argv[3]) : 4;

            printf("TEST FULL (%ds)\n", secs);
            return voice_channel_test_full(secs);
        }

        printf("Unknown test: %s\n", argv[2]);
        return 0;
    }

    (void)argc;
    (void)argv;

    /* 2026-08-15 日志降噪：默认 CONFIG_SYSLOG_DEFAULT_MASK=0xff 全输出，
     * 此前把稳定日志改 LOG_DEBUG 无效（照样打印）。这里用 setlogmask
     * 全局过滤 INFO/DEBUG，仅保留 NOTICE 及以上（EMERG..NOTICE）。
     * 需要保留的关键行（ready/network/ws started）已提升为 LOG_NOTICE。 */
    setlogmask(LOG_UPTO(LOG_NOTICE));

    g_shutdown_requested = false;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    syslog(LOG_NOTICE, "[%s] AI Agent - Vela AI Agent starting (build: %s)\n",
        TAG, AGENT_BUILD_VERSION);

    /* ── Phase 0: Timezone ──────────────────────────────────── */
    /* Set TZ before any time calls so localtime_r returns CST+8.
     * NuttX with CONFIG_LIBC_LOCALTIME+CONFIG_LIBC_ZONEINFO supports
     * both POSIX ("CST-8") and zoneinfo ("Asia/Shanghai") formats. */
    setenv("TZ", AGENT_TIMEZONE, 1);
#ifdef CONFIG_LIBC_LOCALTIME
    tzset();
#endif
    BOOT_LOG(&t0, "P0", "timezone set");

    /* ── Phase 0: Storage Bootstrapping (Auto-mount & Mkdir) ── */

    struct stat st;
    if (stat("/data", &st) != 0) {
        syslog(LOG_INFO, "[%s] Mounting /data as tmpfs for simulation...\n", TAG);
        mount(NULL, "/data", "tmpfs", 0, NULL);
    }

    /* Keep runtime paths identical to AGENT_CONFIG_FILE and other stores. */
    mkdir(AGENT_DATA_DIR, 0755);
    mkdir(AGENT_CONFIG_DIR, 0755);
    mkdir(AGENT_MEMORY_DIR, 0755);
    mkdir(AGENT_SESSION_DIR, 0755);
    mkdir(AGENT_SKILLS_DIR, 0755);
    BOOT_LOG(&t0, "P0", "storage ready");

    /* Memory info */
    {
        struct mallinfo mi = mallinfo();
        syslog(LOG_DEBUG, "[%s] [boot +%ldms] heap: arena=%d free=%d used=%d\n",
            TAG, boot_ms(&t0), mi.arena, mi.fordblks, mi.uordblks);
    }

    /* ── Phase 1: Core infrastructure ──────────────────────── */
    {
        int rc;
        rc = config_store_init();
        BOOT_LOG_RC(&t0, "P1", "config_store_init", rc);

        rc = message_bus_init();
        BOOT_LOG_RC(&t0, "P1", "message_bus_init", rc);
        if (rc != OK)
            return -1;

        rc = memory_store_init();
        BOOT_LOG_RC(&t0, "P1", "memory_store_init", rc);

        rc = session_mgr_init();
        BOOT_LOG_RC(&t0, "P1", "session_mgr_init", rc);
    }

    /* ── Phase 2: Proxy / networking ───────────────────────── */
    {
        int rc = http_proxy_init();
        BOOT_LOG_RC(&t0, "P2", "http_proxy_init", rc);
    }

    /* ── Phase 3: Application services ─────────────────────── */
    {
        int rc;
#ifdef CONFIG_AI_AGENT_FEISHU
        rc = feishu_bot_init();
        BOOT_LOG_RC(&t0, "P3", "feishu_bot_init", rc);
#endif

        rc = llm_proxy_init();
        BOOT_LOG_RC(&t0, "P3", "llm_proxy_init", rc);

        rc = llm_router_init();
        BOOT_LOG_RC(&t0, "P3", "llm_router_init", rc);

        /* node_manager must init BEFORE tool_registry so it can
         * register itself as a tool provider. */
#ifdef CONFIG_AI_AGENT_NODE
        rc = node_manager_init();
        BOOT_LOG_RC(&t0, "P3", "node_manager_init", rc);
#endif

        rc = tool_registry_init();
        BOOT_LOG_RC(&t0, "P3", "tool_registry_init", rc);

        rc = tool_guard_init();
        BOOT_LOG_RC(&t0, "P3", "tool_guard_init", rc);

        /* Skills must init AFTER tool_registry so executable skills
         * can register themselves as tools. */
        rc = skill_loader_init();
        BOOT_LOG_RC(&t0, "P3", "skill_loader_init", rc);

        rc = agent_loop_init();
        BOOT_LOG_RC(&t0, "P3", "agent_loop_init", rc);

        rc = cron_service_init();
        BOOT_LOG_RC(&t0, "P3", "cron_service_init", rc);

        rc = heartbeat_init();
        BOOT_LOG_RC(&t0, "P3", "heartbeat_init", rc);

#ifdef CONFIG_AI_AGENT_NODE
        rc = node_client_init();
        BOOT_LOG_RC(&t0, "P3", "node_client_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_MQTT
        rc = mqtt_channel_init();
        BOOT_LOG_RC(&t0, "P3", "mqtt_channel_init", rc);
#endif

        rc = voice_channel_init();
        BOOT_LOG_RC(&t0, "P3", "voice_channel_init", rc);

#ifdef CONFIG_AI_AGENT_WEIXIN
        rc = weixin_channel_init();
        BOOT_LOG_RC(&t0, "P3", "weixin_channel_init", rc);
#endif

#ifdef CONFIG_AI_AGENT_LVGL_UI
        rc = lvgl_ui_channel_init();
        BOOT_LOG_RC(&t0, "P3", "lvgl_ui_channel_init", rc);
#endif
    }

    /* ── Phase 4: CLI — register commands only, no thread yet ── */
    {
        int rc = nsh_commands_init();
        BOOT_LOG_RC(&t0, "P4", "nsh_commands_init", rc);
    }

    /* ── Phase 5: Network — async, does NOT block ready ────── */

    /* Kick off WiFi reconnect and start outbound dispatch immediately.
     * Network-dependent services (feishu, agent, ws, node) are started
     * inside the network_watch thread once the link comes up. */

    /* Outbound dispatch thread — start before network so queued messages
     * are drained even during the connection window. */
    if (agent_task_create(outbound_dispatch_task, "outbound",
            AGENT_OUTBOUND_STACK, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_ERR, "[%s] Failed to start outbound dispatch thread\n", TAG);
        return -1;
    }
    BOOT_LOG(&t0, "P5", "outbound dispatch thread started");

    /* Cron + heartbeat don't need network */
    cron_service_start();
    heartbeat_start();
    BOOT_LOG(&t0, "P5", "cron + heartbeat started");

#ifdef CONFIG_AI_AGENT_LVGL_UI
    if (lvgl_ui_channel_start() != OK)
        syslog(LOG_WARNING, "[%s] lvgl_ui_channel_start failed\n", TAG);
    BOOT_LOG(&t0, "P5", "lvgl_ui_channel started");
#endif

    /* Network watcher thread: reconnect + wait + start net services */
    if (agent_task_create(network_watch_task, "net_watch",
            AGENT_OUTBOUND_STACK, NULL,
            AGENT_OUTBOUND_PRIO)
        != OK) {
        syslog(LOG_WARNING, "[%s] Failed to start network_watch thread\n", TAG);
    }
    BOOT_LOG(&t0, "P5", "network_watch thread started (async)");

    /* ── Phase 6: CLI thread — all services now in known state ── */
    {
        int rc = nsh_commands_start();
        BOOT_LOG_RC(&t0, "P6", "nsh_commands_start", rc);
    }

    syslog(LOG_NOTICE, "[%s] [boot +%ldms] AI Agent ready. Type 'help' in NSH for commands.\n",
        TAG, boot_ms(&t0));

    /* Block main thread until shutdown is requested */
    while (!g_shutdown_requested) {
        sleep(1);
    }

    syslog(LOG_INFO, "[%s] Shutdown requested — stopping services...\n", TAG);

    /* Wake all threads blocked on message bus before stopping services */
    message_bus_wakeup();

    /* ── Graceful teardown (reverse of startup order) ──────── */

    /* Phase 5 services (network-dependent) */
#ifdef CONFIG_AI_AGENT_WEIXIN
    weixin_channel_stop();
#endif
#ifdef CONFIG_AI_AGENT_NODE
    node_client_stop();
#endif
#ifdef CONFIG_AI_AGENT_MQTT
    mqtt_channel_stop();
#endif
    ws_server_stop();
    /* feishu_bot and agent_loop have no _stop(); they will exit
     * naturally once their blocking I/O returns an error after
     * the network sockets are closed below, or they can check
     * agent_shutdown_requested() in their loops. */

#ifdef CONFIG_AI_AGENT_LVGL_UI
    lvgl_ui_channel_stop();
#endif

    /* Phase 5 services (non-network) */
    cron_service_stop();
    heartbeat_stop();

    /* Give threads a moment to notice and exit */
    usleep(500 * 1000);

    /* Cleanup — reverse order of init */
    tool_media_cleanup();
#ifdef CONFIG_AI_AGENT_MCP
    mcp_bridge_cleanup();
#endif
    tool_guard_cleanup();
    tool_registry_cleanup();
    llm_cache_cleanup();
    vela_tls_pool_cleanup();
    message_bus_destroy();

    syslog(LOG_INFO, "[%s] Shutdown complete.\n", TAG);

    return 0;
}
