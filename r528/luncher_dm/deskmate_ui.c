/*******************************************************************
 * deskmate_ui.c — Desktop Mate UI for OpenVela R528 (luncher_dm)
 *
 * Ported from: /data/lv_port_linux/src/main.c (PC SDL2 simulator)
 * 1920×1200  landscape tablet
 * Design: Clean, minimal, iPad-style widget layout
 * Font:   3-tier MiSans — Title / Body / Caption (freetype, set via dm_font_*)
 * Layout: Status bar → Clock (upper area) → Cards → Dock (bottom)
 * All icons: LV_SYMBOL_* built-in glyphs only
 * All text:  English (kept from simulator; CJK via MiSans)
 ******************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* dm_kb_state_t 在 deskmate_ui.h 中声明，此处不需要完整包含（避免重复定义冲突）
 * 键盘工具函数定义在本文件，对外声明在 deskmate_ui.h */

/* Board-only audio playback backend (libcedarx XPlayer: mp3/flac/ogg/aac/wav).
 * The PC simulator build (lv_port_linux) has no CONFIG_LUNCHER_DM_APP,
 * so these headers/calls are compiled out there. */
#ifdef CONFIG_LUNCHER_DM_APP
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include "xplayer.h"
#include "soundControl.h"
#include "adecoder.h"
#include "pcm.h"

/* XPlayer async state (board only): 0 idle, 1 prepared, 2 playing,
 * 3 playback complete, -1 error. Written from XPlayer callback thread,
 * polled from the LVGL music timer (single consumer). */
static XPlayer *g_xplayer;
static SoundCtrl *g_music_sound;   /* 当前 dm_sound 对象，切歌重建时显式释放防泄漏 */
static volatile int g_xp_state;
#endif

#include <lvgl/lvgl.h>

#include "dm_weather.h"
#include "dm_ai.h"                      /* 2026-08-11 AI 子页 WS client */
#include "dm_voice.h"                   /* 2026-08-28 Step 3：音乐状态接线给 Voice Director */
#include "dm_health_cfg.h"              /* 2026-09-14：音量落盘供 AI 语音同步 */
#include "deskmate_icons.h"
#include "aibg.h"                       /* 2026-08-12 AI 子页品牌横幅 */
#ifdef CONFIG_LED_RGB_WS2812
#include "lv_demo_panel_rgb_control.h"   /* board-only LED control */
#endif
#ifdef CONFIG_LUNCHER_DM_APP
#include "sunxi_hal_pwm.h"               /* 屏幕背光 PWM ch4（BOE 面板） */

/* 网络驱动接线（2026-08-08）：状态栏图标 + Settings 开关直连驱动。
 * 注意：WiFi/蓝牙头（wifi_conf.h 的 dlist.h 与 HAL aw_list.h 的
 * struct list_head 重定义冲突）不能与 sunxi_hal_*.h 同编译单元，
 * 故全部封装在 dm_net.c（独立编译单元），这里只调其 API。 */
#include "dm_net.h"
#include "dm_pet.h"                  /* 电子宠物：音乐/语音联动 */
#include "dm_alarm.h"                /* 2026-09-10 P206：语音定闹（alarm: 命令） */
#endif

/* 网络接线（2026-08-08）：Settings WiFi/蓝牙开关回调 + 状态栏图标刷新。
 * Phase 1 拆分：settings_volume_cb / settings_wifi_cb / settings_*_row_open_cb
 * 已搬至 ui/ui_settings.c，经 deskmate_ui.h 声明跨文件引用 */
void dm_net_status_refresh(void);   /* Phase 1 拆分：供 ui_wifi.c wifi_poll_cb 跨文件调用 */
/* tentative definition：正式定义在 music 区（g_music_volume=30、music_vol_lbl），
 * Phase 1 拆分：ui_settings.c settings_volume_cb 经 deskmate_ui.h extern 引用 */
int      g_music_volume;
lv_obj_t *music_vol_lbl;
int      g_screen_brightness = 75;   /* 2026-08-23 P115：屏幕背光 0-100（AI 语音命令步进用） */

/* P180：背光硬件写（屏幕 PWM ch4 @40KHz + WS2812 LED）——公共接口，
 * 亮度滑块/夜览/自动亮度共用。不写 g_screen_brightness（手动基准留给
 * 恢复；夜览/自动亮度为临时覆盖）。P176~179 从 ui_settings.c 上提。 */
void dm_brightness_write(int v)
{
#ifdef CONFIG_LUNCHER_DM_APP
    {
        struct pwm_config pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.period_ns  = 25000;                       /* 40KHz */
        pcfg.duty_ns    = (uint32_t)(25000u * v / 100); /* 0-100 → 0-25000ns */
        pcfg.polarity   = PWM_POLARITY_NORMAL;         /* 2026-08-08 上板实测
                                                        * INVERSED 方向反了
                                                        * （100% 最暗），改
                                                        * NORMAL 后 100% 最亮 */
        hal_pwm_control(4, &pcfg);                     /* 屏幕背光 PWM ch4 */
    }
#endif
#ifdef CONFIG_LED_RGB_WS2812
    dm_led_set_brightness(v);   /* WS2812 LED，0-100 (board only) */
#endif
}

/* Fonts provided by luncher_dm.c (MiSans freetype) */
extern lv_font_t *dm_font_title;
extern lv_font_t *dm_font_clock;    /* P151: 状态栏时间专用档 56px */
extern lv_font_t *dm_font_symbol;
extern lv_font_t *dm_font_icon;
extern lv_font_t *dm_font_body;
extern lv_font_t *dm_font_label;
extern lv_font_t *dm_font_caption;

/* Dock glyph render scale (1/256): board = 256, simulator = 512 */
extern int dm_symbol_scale;

/* Forward declarations */
void reset_idle_timer(void);   /* ui_home.c（Phase 1 拆分） */
void show_subpage(const char *title);   /* Phase 1 拆分：ui_settings.c 开 WiFi/蓝牙嵌套子页 */
void ui_music_create(lv_obj_t *parent);   /* ui_music.c（Phase 1 拆分） */
void music_audio_force_stop(void);
void music_prev_cb(lv_event_t *e);        /* ui_music.c：HOME/锁屏 ⏮ 复用 */
void music_next_cb(lv_event_t *e);        /* ui_music.c：HOME/锁屏 ⏭ 复用 */
lv_obj_t *music_play_btn_create(lv_obj_t *parent, lv_coord_t size,
                                lv_obj_t **icon_out); /* ui_music.c：播放键 */
void music_volume_persist_debounced(void); /* ui_music.c：音量落盘 800ms 防抖 */
void ui_books_create(lv_obj_t *parent);   /* ui_books.c（Phase 1 拆分） */
void ui_calendar_create(lv_obj_t *parent); /* ui_calendar.c（2026-09-10 日历） */
void ui_calendar_close_cleanup(void);   /* ui_calendar.c：close_subpage 清理 */
void ui_pet_create(lv_obj_t *parent);     /* ui_pet.c（2026-09-10 宠物独立子页） */
void ui_pet_close_cleanup(void);          /* ui_pet.c：close_subpage 清理 */
void ui_alarm_create(lv_obj_t *parent);   /* ui_alarm.c（2026-09-10 闹钟） */
void ui_alarm_close_cleanup(void);        /* ui_alarm.c：close_subpage 清理 */
void ui_datetime_create(lv_obj_t *parent); /* ui_datetime.c（2026-09-10 P211 日期与时间） */
void ui_datetime_close_cleanup(void);     /* ui_datetime.c：close_subpage 清理 */
void book_reader_close(void);             /* ui_books.c：close_subpage 清理（deskmate_ui.h 已声明） */

/* Phase 1 拆分：books 共享状态（ui_books.c 导出，files_open_file 引用） */
#define DM_MAX_BOOKS   32
void book_open_reader(int idx);
extern char     dm_book_paths[DM_MAX_BOOKS][160];
extern char     dm_book_titles[DM_MAX_BOOKS][64];
extern uint32_t dm_book_count;
void ui_files_create(lv_obj_t *parent);   /* ui_files.c（Phase 1 拆分） */
void files_ui_clear_ptrs(void);           /* ui_files.c：close_subpage 清理多选栏 */
void ui_uart_dbg_create(lv_obj_t *parent);      /* ui_uart_dbg.c：UART 调试工具子页 */
void ui_uart_dbg_close_cleanup(void);           /* ui_uart_dbg.c：close_subpage 清理 */
static void create_ai_subpage(lv_obj_t *parent);

/* 2026-08-12 AI 子页（create_ai_subpage 私有，close_subpage 清理）：
 * WS client（dm_ai.c）轮询 timer + 语音状态/语音对讲按钮指针。
 * 2026-08-12 重构：文字输入窗口/回复区/录音 DEMO 已按需求移除，
 * AI 子页改为「品牌横幅 + 语音圆圈 + 快捷卡」语音优先布局。 */
static lv_timer_t *g_ai_timer;
static lv_obj_t   *ai_status_lbl; /* 语音状态（Ready/Listening/Thinking/Speaking） */
static lv_obj_t   *ai_voice_btn;  /* 语音对讲圆圈按钮（按住说话） */
static lv_obj_t   *ai_chat_lbl;   /* 2026-08-15 P84：对话文字区（stt/llm 滚动文本） */
static bool        ai_voice_active; /* 2026-08-12：press 真正开始录音才置位，防误发 stop */
static lv_timer_t *g_ai_auto_stop_timer; /* 2026-08-12 点按延录：快速松手后自动 stop */
static uint32_t    ai_voice_press_ts;    /* 2026-08-12：press 时刻（lv_tick ms），判点按 vs 长按 */
static uint32_t    ai_speaking_ts;       /* 2026-08-15 P81：进入 Speaking 时刻，超时兜底复位 */
#define AI_SPEAKING_TIMEOUT_MS 22000     /* 2026-08-15 P81：TTS 播报最长约 19s（LLM 2s+首包 5.6s+播放 11.5s），留余量 */
static void        ai_auto_stop_cb(lv_timer_t *t); /* 2026-08-12 点按延录到期回调（前向声明，定义在 ai_voice_release_cb 后） */
/* P116：music 区正式定义在 1237 行（deskmate_ui.h 已 extern），
 * ai_poll_cb（125 行）提前引用需此处前向声明——deskmate_ui.c 自身不 include deskmate_ui.h */
extern bool music_playing;
extern bool music_inited;
/* 2026-08-23 表驱动重构：本地设备控制命令分发（实现定义在 music 区
 * 前向声明之后，见 ai_local_cmd_dispatch——含命令表 + needs_ack 策略） */
static void ai_local_cmd_dispatch(const char *cmd);

/* 2026-08-23 review-10 音频仲裁层：前台音频（TTS 播报/提示音）挂起/恢复音乐。
 * 定义在 music 区之后（见 dm_audio_fg_acquire），此处前向声明供 PTT 按下
 * （dm_ai_voice_press_cb）、提示音（dm_tone_play）、cmd 轮询（dm_ai_cmd_poll_cb）提前引用。 */
void dm_audio_fg_acquire(void);
void dm_audio_fg_poll(void);

/* 2026-08-12：文字输入/发送回调已移除（文本链路入口删除，语音优先）
 * 2026-09-10 快问 chips：键盘无拼音输入法，打字入口无意义；改为 4 个
 * 点按快问（时间/天气/音乐/笑话，全走 dm_ai_ask 文本链路，agent 侧
 * fast-path 短路，无需 LLM，保证无麦也能演示 AI 闭环）。 */

/* 2026-09-10：对话区共享追加器（快问回显 + stt/llm 事件 + 文本回复共用）。
 * 原内联在 ai_poll_cb 内的 static chat_buf 上提为文件域，快问 chips 与
 * poll 共用同一缓冲，避免两处各记各的导致对话断裂。 */
static char ai_chat_buf[AI_RESP_MAX * 3];

static void ai_chat_append(const char *who, const char *text)
{
    size_t used;
    int n;

    if (!text || text[0] == '\0')
        return;
    used = strlen(ai_chat_buf);
    n = snprintf(ai_chat_buf + used, sizeof(ai_chat_buf) - used,
        "%s：%s\n", who, text);
    if (n > 0 && (size_t)n < sizeof(ai_chat_buf) - used)
        used += (size_t)n;
    else
        used = sizeof(ai_chat_buf) - 1;
    /* 防累积无限增长：超限保留后半段 */
    if (used > sizeof(ai_chat_buf) * 2 / 3)
        {
            memmove(ai_chat_buf, ai_chat_buf + sizeof(ai_chat_buf) / 3,
                sizeof(ai_chat_buf) - sizeof(ai_chat_buf) / 3);
            ai_chat_buf[sizeof(ai_chat_buf) - sizeof(ai_chat_buf) / 3] = '\0';
        }
}

static void ai_chat_refresh(void)
{
    if (!ai_chat_lbl)
        return;
    lv_label_set_text(ai_chat_lbl, ai_chat_buf);
    /* 最新消息在底部 → 滚到底（旧代码滚到顶，新消息看不见） */
    lv_obj_scroll_to_y(lv_obj_get_parent(ai_chat_lbl),
        LV_COORD_MAX, LV_ANIM_OFF);
}

/* 2026-09-10 快问点击：user_data = 完整问句（静态字符串，免释放）。
 * 与 PTT 同门禁（未联网拦截），busy 时拦截防 worker 顶替。 */
static void ai_chip_click_cb(lv_event_t *e)
{
    const char *q = (const char *)lv_event_get_user_data(e);

    reset_idle_timer();   /* 交互防 idle 跳锁屏（铁律 4） */
    if (!q || q[0] == '\0')
        return;
    if (!dm_net_wifi_connected()) {
        if (ai_status_lbl)
            lv_label_set_text(ai_status_lbl, "未连接 Wi-Fi，请先联网");
        return;
    }
    if (dm_ai_busy()) {
        if (ai_status_lbl)
            lv_label_set_text(ai_status_lbl, "思考中…");
        return;
    }
    ai_chat_append("你", q);
    ai_chat_refresh();
    /* 2026-09-10 P206 板上实锤：快问"播放音乐"走 agent 云点歌
     * （music_search→music_play）依赖 agent 侧播放器，
     * 板上报 AGENT_MUSIC media_player_open failed。
     * 改走本地曲库直播（music_play，离线可用、无曲给暂无曲目），
     * 不发 agent，回复本地回显。 */
    if (strcmp(q, "播放音乐") == 0)
        {
            extern uint32_t dm_track_count;
            extern uint32_t music_track_id;
            music_play(music_track_id);
            if (dm_track_count > 0)
                ai_chat_append("AI", "正在播放本地音乐");
            else
                ai_chat_append("AI", "曲库是空的，把歌放到 /sdcard/music 吧");
            ai_chat_refresh();
            if (ai_status_lbl)
                lv_label_set_text(ai_status_lbl, "随时待命");
            return;
        }
    if (ai_status_lbl)
        lv_label_set_text(ai_status_lbl, "思考中…");
    dm_audio_fg_acquire();   /* 与 PTT 一致：挂起音乐让路，播完自动恢复 */
    if (dm_ai_ask(q) != 0 && ai_status_lbl)
        lv_label_set_text(ai_status_lbl, "思考中，稍等…");
}

/* 轮询 timer：worker 完成 → dm_ai_poll 取回复更新显示 */
static void ai_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 2026-08-15 P84：消费语音事件（stt 用户话语 / llm AI 回复），
     * 追加到对话文字区（小智式"说话显字/回答显字"） */
    if (ai_chat_lbl)
        {
            static char evt_stt[AI_RESP_MAX];
            static char evt_llm[AI_RESP_MAX];
            if (dm_ai_voice_evt_poll(evt_stt, sizeof(evt_stt),
                                     evt_llm, sizeof(evt_llm)))
                {
                    ai_chat_append("你", evt_stt);
                    ai_chat_append("AI", evt_llm);
                    ai_chat_refresh();
                }
        }
    /* 2026-08-23 常驻化：cmd 事件消费移至 dm_ai_cmd_poll_cb（deskmate_ui_create
     * 创建的系统级 timer，不随 AI 子页启停）——修复锁屏/主界面按语音按钮说话
     * 时 cmd 无人消费（AI 子页未开，ai_poll_cb 不存在）导致音乐/音量不生效。 */
    /* 2026-08-11 调优：AI_RESP_MAX 1024→4096 后栈上 4KB 缓冲转 static，
     * 防 LVGL timer 回调线程栈压力（LVGL 单线程，无并发风险） */
    static char buf[AI_RESP_MAX];
    if (dm_ai_poll(buf, sizeof(buf)))
        {
            /* 2026-09-10 快问回显：文本链路回复只走 WEBSOCKET 通道
             * （无 TTS、无 llm 事件广播，见 agent_main/agent_loop），
             * 此处是回复上屏的唯一路径——必须显示内容，不只回状态。 */
            ai_chat_append("AI", buf);
            ai_chat_refresh();
            if (ai_status_lbl)
                lv_label_set_text(ai_status_lbl, "随时待命");
        }
    else if (dm_ai_busy() && ai_status_lbl)
        {
            lv_label_set_text(ai_status_lbl, "思考中…");
        }
    /* 2026-08-15 P81：语音链路（PTT→ASR→LLM→TTS）播报完成后 agent
     * 不会回调 UI，Speaking 状态无复位路径——超时兜底复位回 Ready。
     * 正常链路 LLM 2s + TTS 首包 5.6s + 播放 ~11.5s ≈ 19s，留余量。 */
    else if (ai_speaking_ts && ai_status_lbl &&
             (uint32_t)(lv_tick_get() - ai_speaking_ts) > AI_SPEAKING_TIMEOUT_MS)
        {
            ai_speaking_ts = 0;
            lv_label_set_text(ai_status_lbl,
                dm_net_wifi_connected() ? "按住说话"
                                        : "未连接 Wi-Fi，请先联网");
        }
}

/* 2026-08-11 语音对讲按钮（按住说话）：P53 方案A 已闭环——
 * 按住 → dm_ai_voice("start") 发 WS 控制消息，ws_server 解析后调
 * voice_channel_start（PTT 录音）；松开 → dm_ai_voice("stop") →
 * voice_channel_stop（ASR→LLM→TTS→播放）。mic 硬件/火山语音凭证
 * 就绪后即全链路可用（当前状态联动为 UI 反馈，底层已真实触发）。
 * 2026-08-15 健康助理锁屏改造：导出为非 static 供锁屏双圈复用，
 * 样式操作改用 lv_event_get_target(e)（不再写死 AI 子页按钮）。 */
void dm_ai_voice_press_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    reset_idle_timer();   /* 2026-08-11：交互防 idle 到期跳锁屏（铁律 4） */

    /* 2026-08-12 未联网提示（用户反馈）：WiFi 未连 AP 时语音链路必失败
     * （ASR/TTS 走公网 openspeech），直接文字提示并拦截录音，不再白录 */
    if (!dm_net_wifi_connected()) {
        if (ai_status_lbl)
            lv_label_set_text(ai_status_lbl, "未连接 Wi-Fi，请先联网");
        return;
    }

    /* 2026-08-15 P84：正在播报（Speaking）时按下 → 先打断 TTS 再录音
     * （小智 AbortSpeaking 式：按住即抢话，打断 AI 播报）
     * 2026-08-15 Q2：健康提醒 speak 播报窗口内按下同样先 interrupt，
     * 避免健康语音与录音/后续 TTS 重叠（dm_ai_voice_speaking 判定）。 */
    if (ai_speaking_ts || dm_ai_voice_speaking()) {
        ai_speaking_ts = 0;
        dm_ai_voice("interrupt");
        if (ai_status_lbl)
            lv_label_set_text(ai_status_lbl, "已打断");
    }

    ai_voice_active = true;
    ai_voice_press_ts = lv_tick_get();   /* 2026-08-12：记录按下时刻判点按/长按 */
    ai_speaking_ts = 0;                  /* 2026-08-15 P81：新一轮录音，清 Speaking 计时 */
    if (ai_status_lbl)
        lv_label_set_text(ai_status_lbl, "聆听中…");
    /* 2026-08-16 P103：PTT 按下 → 停音乐释放声卡。XPlayer pause 只调
     * snd_vela_pcm_pause 不关 handle（PAUSE_PUSH 后 CNT 停住、ai_agent
     * TTS snd_vela_pcm_open 持续 EBUSY -16），必须 force_stop 真正释放，
     * 否则 AI 回复播报永远 playback open failed。语音优先，音乐让路。
     * 2026-08-23 review-10：改走音频仲裁层 acquire——挂起音乐（记录待恢复），
     * AI 播报完成（agent "done" 事件）后由 dm_audio_fg_poll 自动恢复。 */
    dm_audio_fg_acquire();
    /* 2026-08-12 动态感（用户反馈）：按下放大 1.12x + 蓝紫阴影加深 +
     * 玻璃提亮（原按红 0xFF3B30 删除）；LV_STATE_PRESSED 样式松开自动回退 */
    lv_obj_set_style_transform_scale(btn, 287, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 26, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    dm_ai_voice("start");   /* P53 方案A：真实触发 PTT 录音 */
}

void dm_ai_voice_release_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    if (!ai_voice_active)   /* 2026-08-12：press 被网络拦截时勿发 stop */
        return;
    /* 2026-08-12 点按延录（用户诉求：点按也应录音转写）：按住 <1.5s
     * 视为点按——capture period 64ms 起步，几十 ms 的点按读不到数据
     * （0 字节）。不立即 stop，延录 4s 后自动 stop，再按一次取消延录
     * 保持录音。 */
    if ((uint32_t)(lv_tick_get() - ai_voice_press_ts) < 1500) {
        if (!g_ai_auto_stop_timer) {
            g_ai_auto_stop_timer =
                lv_timer_create(ai_auto_stop_cb, 4000, NULL);
            if (g_ai_auto_stop_timer)
                lv_timer_set_repeat_count(g_ai_auto_stop_timer, 1);
        }
        return;
    }
    ai_voice_active = false;
    ai_speaking_ts = lv_tick_get();   /* 2026-08-15 P81：进入 Speaking，记录时刻供 poll 超时复位 */
    if (ai_status_lbl)
        lv_label_set_text(ai_status_lbl, "播报中…");
    dm_ai_voice("stop");   /* P53 方案A：结束录音 → ASR→LLM→TTS */
}

/* 2026-08-12 点按延录到期：自动结束录音（发 stop，agent 收尾 ASR→LLM→TTS） */
static void ai_auto_stop_cb(lv_timer_t *t)
{
    (void)t;
    g_ai_auto_stop_timer = NULL;
    ai_voice_active = false;
    ai_speaking_ts = lv_tick_get();   /* 2026-08-15 P81：同上 */
    if (ai_status_lbl)
        lv_label_set_text(ai_status_lbl, "播报中…");
    dm_ai_voice("stop");
}

/* 2026-08-15 P85：6 个快捷卡已按用户要求移除，ai_action_cb 随之删除 */

/* 2026-08-12：虚拟键盘/MIC DEMO 回调已随输入窗口与录音卡移除 */
void ui_settings_create(lv_obj_t *parent);  /* ui_settings.c（Phase 1 拆分） */
void ui_settings_close_cleanup(void);       /* ui_settings.c：close_subpage 清理 */
void ui_wifi_create(lv_obj_t *parent);   /* ui_wifi.c（Phase 1 拆分） */
void ui_wifi_close_cleanup(void);        /* ui_wifi.c：close_subpage 清理 */
void ui_bt_create(lv_obj_t *parent);     /* ui_bt.c（Phase 1 拆分） */

/* ================================================================
 * DESIGN SYSTEM v1 — 8" 1920×1200 tablet (PPI ≈ 283, 1mm ≈ 11.2px)
 * 物理基准：8 英寸 16:10 面板，像素密度 ≈283 PPI。
 * 字号/间距按"物理可读性"定值，不按像素比例盲目放大。
 *
 * 字号五档（板端 MiSans / 模拟器 montserrat）：
 *   title  110/48 — 大时钟、天气主温度（≈9.8mm）
 *   icon    76/36 — 状态栏时间、dock 符号、状态图标（≈6.8mm）
 *   body    56/24 — 卡片标题、设置行、AI 环内文字（≈5.0mm）
 *   label   44/20 — dock 标签、温湿度、卡片次级文字（≈3.9mm）
 *   caption 34/14 — 辅助说明、预报小时、quote（≈3.0mm）
 *
 * 结构规则：
 *   状态栏 ≤96px 单行：左时间(icon) 中天气(label) 右图标(icon)
 *   触摸目标 ≥110px（≈10mm）；dock 图标 154px，符号 ≥ 图标 45%
 *   卡片 ≤340px 高，AI 环 ≤180px，不侵占屏幕呼吸空间
 *   留白：边缘 ≥96px(EDGE_PAD)，元素间距 ≥24px
 * ================================================================ */

#define DM_SCALE        2.4f
#define TOP_BAR_H       132     /* status bar height — fits 110px title clock */
#define CARD_GAP        48      /* gap between cards */
#define DOCK_ICON_SIZE  154     /* dock icon button size (OK, keep) */
#define DOCK_ICON_GAP   67      /* dock icon spacing */
#define DOCK_H          240     /* dock bar height (labels enlarged) */
#define EDGE_PAD        96      /* edge padding */
#define SETTING_ROW_H   125     /* settings row height */

/* ── Unified corner radius (per user request) ──
 * EVERYTHING with a visible corner uses RAD_CARD = 40px — Home plugins,
 * Settings groups, every sub-app card, icon badges, album/book covers,
 * weather day tiles, playlist rows, dock pill and dock icon buttons.
 * Exceptions (kept intentionally):
 *   round buttons / touch dots  LV_RADIUS_CIRCLE (fully round)
 *   progress bars / sliders     DM(2) / DM(3)    (half-height pill)
 *   separators / fullscreen     radius 0         (no visible corner) */
#define RAD_CARD    40

/* Scale a simulator pixel size to board 1920x1200 */
#define DM(x) ((lv_coord_t)((x) * DM_SCALE))

/* ================================================================
 * 6-TIER FONT SYSTEM (MiSans freetype, provided by luncher_dm.c)
 * Title:   main clock, hero numbers
 * Symbol:  dock app glyphs (×1.5 of icon)
 * Icon:    status-bar time, status icons
 * Body:    card titles, settings labels, nav titles
 * Label:   dock labels, secondary text
 * Caption: helper text, captions
 * ================================================================ */

#define FONT_TITLE   (dm_font_title)
#define FONT_SYMBOL  (dm_font_symbol)
#define FONT_ICON    (dm_font_icon)
#define FONT_BODY    (dm_font_body)
#define FONT_LABEL   (dm_font_label)
#define FONT_CAPTION (dm_font_caption)

/* ================================================================
 * COLOR PALETTE — Apple-style light theme
 * ================================================================ */

#define COL_BG        0xF2F2F7   /* screen background */
#define COL_BG_GRAD   0xE5E5EC   /* gradient bottom */
#define COL_CARD      0xFFFFFF   /* card surface */
#define COL_TEXT      0x1C1C1E   /* primary text */
#define COL_SEC       0x8E8E93   /* secondary text */
#define COL_BLUE      0x007AFF   /* accent blue */
#define COL_GREEN     0x34C759   /* accent green */
#define COL_ORANGE    0xFF9500   /* accent orange */
#define COL_RED       0xFF3B30   /* accent red */
#define COL_PURPLE    0x8B5CF6   /* accent purple */
#define COL_SEP       0xC6C6C8   /* separator line */

/* ================================================================
 * GLOBAL VARIABLES
 * ================================================================ */

/* Phase 1 拆分：HOME 常驻变量（ui_home.c 使用，deskmate_ui.h 声明 extern） */
lv_obj_t     *clock_label;           /* status bar clock（左上角时间） */
lv_obj_t     *clock_date_label;      /* status bar date（左上角周几日期，P145 复用原 hero 日期名） */
lv_obj_t     *status_weather_label;  /* status bar weather */
lv_group_t   *g_group;
lv_indev_t   *g_keypad_indev;

/* Dock container (hidden during standby) */
lv_obj_t     *g_dock;

/* Weather (live data via dm_weather.c, refreshed by lv_timer) */
dm_weather_t  g_weather;
lv_obj_t     *w_cur_temp_lbl;   /* current temp "33°C" */
lv_obj_t     *w_cur_desc_lbl;   /* "Sunny | AQI 42" */
lv_obj_t     *w_day_lbl[DM_WEATHER_DAYS];    /* weekday */
lv_obj_t     *w_icon_cv[DM_WEATHER_DAYS];    /* drawn weather glyph */
/* Canvas backing store for ARGB8888 (4 bytes/px). lv_color_t is only 3 bytes
 * in LVGL 9.1 (XRGB8888), so a lv_color_t array under-allocates: each 64×64
 * canvas needs 64*64*4 bytes but would only get 64*64*3 → lv_canvas_fill_bg
 * overflowed 4 KB per canvas, corrupting adjacent globals (board crash:
 * spec_attr garbage → Data Abort in clock_update_cb). uint32_t fixes it. */
uint32_t     w_icon_buf[DM_WEATHER_DAYS][64 * 64];
lv_obj_t     *w_temp_lbl[DM_WEATHER_DAYS];   /* "36°/25°" */

/* Standby / lock screen */
lv_obj_t     *standby_overlay;
lv_obj_t     *standby_ring;
lv_obj_t     *standby_ai_text;
/* 2026-08-10 锁屏状态栏指针（show_standby 创建，create_status_bar_ex 输出，
 * 与 Home/子页同一份代码：左时钟两行 + 中天气 + 右 wifi/bt/battery）。
 * 由 clock_update_cb / weather_update_cb / dm_net_status_refresh 刷新；
 * standby_overlay 常驻（wake 仅隐藏不删除），指针无需置空。 */
lv_obj_t     *standby_bar_clock_lbl;
lv_obj_t     *standby_bar_date_lbl;    /* P145：锁屏状态栏周几日期 */
lv_obj_t     *standby_bar_weather_lbl;
lv_obj_t     *standby_bar_wifi_lbl;
lv_obj_t     *standby_bar_bt_lbl;
lv_timer_t   *idle_timer;
int           idle_countdown;
/* Phase 1 拆分：g_idle_timeout 被 ui_settings.c（Auto-Lock 档位）extern 引用 */
int g_idle_timeout = 30;   /* seconds; Settings Auto-Lock */

/* Music player progress timer (owned here so close_subpage can clean it)
 * Phase 1 拆分：g_music_timer 被 ui_music.c（ui_music_create 兜底创建）extern 引用 */
lv_timer_t   *g_music_timer;

/* 网络状态（2026-08-08 接线驱动）：状态栏图标 + Settings 开关直连
 * 驱动 API 封装在 dm_net.c（独立编译单元，规避 dlist.h/aw_list.h 冲突）
 * Phase 1 拆分：status_wifi_lbl / status_bt_lbl 由 ui_home.c 创建，extern 共享 */
lv_obj_t      *status_wifi_lbl;
lv_obj_t      *status_bt_lbl;

/* 2026-08-10 子页顶部状态栏（show_subpage 创建；clock_update_cb /
 * dm_net_status_refresh 每秒刷新；close_subpage 删除 overlay 前置空）。
 * 子页全屏 overlay 盖住主屏 header → 必须自带时钟+网络图标，否则进
 * Settings 等子页看不到状态栏。 */
lv_obj_t      *subpage_clock_lbl;
lv_obj_t      *subpage_date_lbl;   /* P145：子页状态栏周几日期 */
lv_obj_t      *subpage_weather_lbl;
lv_obj_t      *subpage_wifi_lbl;
lv_obj_t      *subpage_bt_lbl;

/* Phase 1 拆分：WiFi 子页 static 状态已移至 ui/ui_wifi.c */

/* Phase 1 拆分：蓝牙子页 static 状态已移至 ui/ui_bt.c */

/* ================================================================
 * SHARED CARD STYLE — frosted glass appearance
 * ================================================================ */

/* Phase 1 拆分：style_card 被 ui_music.c（playlist 卡）extern 引用 */
lv_style_t style_card;
static bool style_card_inited = false;

/* Phase 1 拆分：init_shared_styles 被 ui_home.c（ui_home_create）extern 调用 */
void init_shared_styles(void)
{
    if (style_card_inited) return;
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(COL_CARD));
    lv_style_set_bg_opa(&style_card, LV_OPA_90);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_radius(&style_card, RAD_CARD);
    lv_style_set_shadow_width(&style_card, 8);
    lv_style_set_shadow_color(&style_card, lv_color_hex(0x000000));
    lv_style_set_shadow_opa(&style_card, LV_OPA_10);
    lv_style_set_pad_all(&style_card, 16);
    style_card_inited = true;
}

/* ================================================================
 * HELPER: create a transparent flex container (no border, no bg)
 * ================================================================ */

lv_obj_t *make_clean_cont(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(c, LV_OPA_0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ================================================================
 * 2026-09-14 弹窗工具箱：Liquid Glass 弹窗统一语言
 * 玻璃卡(半透明白+白渐变+半透白边+柔和阴影) → 图标块 → 标题 →
 * 说明 → 胶囊按钮。健康提醒/暗光/书删除 三个弹窗共用。
 * 图标一律 LV_SYMBOL_*（FONT_SYMBOL = fa-solid-900 保证渲染）。
 * ================================================================ */

/* 玻璃卡片容器（居中）：调用方往里塞 icon/title/text/btns */
lv_obj_t *dm_popup_card(lv_obj_t *parent, lv_coord_t w)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, RAD_CARD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(card, DM(28), 0);
    lv_obj_set_style_pad_row(card, DM(14), 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);
    return card;
}

/* 图标块：20% 同色浅底圆角盘 + 彩色 LV_SYMBOL；返回图标 label 供改图标/颜色 */
lv_obj_t *dm_popup_icon(lv_obj_t *parent, const char *sym, uint32_t color)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_size(chip, DM(96), DM(96));
    lv_obj_set_style_radius(chip, DM(30), 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ic = lv_label_create(chip);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_font(ic, FONT_SYMBOL, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(color), 0);
    lv_obj_center(ic);
    return ic;
}

/* 图标块换肤：kind 切换时同步改字形+字色+底盘底色（底盘是 label 的父对象）。
 * ic 为 dm_popup_icon 返回的 label；parent(chip) 背景同步同色，防"蓝铃铛配橙底"。 */
void dm_popup_icon_set(lv_obj_t *ic, const char *sym, uint32_t color)
{
    if (ic == NULL) return;
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, lv_color_hex(color), 0);
    lv_obj_t *chip = lv_obj_get_parent(ic);
    if (chip)
        lv_obj_set_style_bg_color(chip, lv_color_hex(color), 0);
}

/* 标题（居中，返回 label 供改文案） */
lv_obj_t *dm_popup_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_text(t, text);
    lv_obj_set_style_text_font(t, FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    return t;
}

/* 说明文字（居中，COL_SEC） */
lv_obj_t *dm_popup_text(lv_obj_t *parent, const char *text)
{
    lv_obj_t *d = lv_label_create(parent);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, lv_pct(100));
    lv_label_set_text(d, text);
    lv_obj_set_style_text_font(d, FONT_BODY, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    return d;
}

/* 按钮行（等距横排，居中） */
lv_obj_t *dm_popup_btns(lv_obj_t *parent)
{
    lv_obj_t *row = make_clean_cont(parent);
    lv_obj_set_width(row, lv_pct(100));
    /* 2026-09-14 P234（"椭圆显示不全"真根因）：lv_obj 类默认高 = LV_DPI_DEF
     * (130px，见 lv_obj.c height_def)，而 make_clean_cont 不自设高度 →
     * 行高只剩 130 < 按钮高 DM(60)=144 → 按钮底边被**行**裁掉（卡片补高没用，
     * 因为裁它的是行不是卡片）。这里显式给行高度 = 按钮高 + 上下留白。 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(row, DM(8), 0);
    lv_obj_set_style_pad_bottom(row, DM(8), 0);
    lv_obj_set_style_pad_column(row, DM(20), 0);
    lv_obj_set_height(row, DM(60) + DM(16));   /* 按钮高 + 上下留白，防裁切/裁影 */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

/* 胶囊按钮：玻璃风 tonal（同色 20% 浅底 + 彩字 + 细边框，按压底加深到 40%）。
 * kind 0=主蓝 1=次灰 2=危险红；柔影保留同色但降到 20%，呼应图标盘，与卡片统一语言。
 * label 存入 user_data，供调用方后续改文案。 */
lv_obj_t *dm_popup_btn(lv_obj_t *parent, const char *txt, int kind, lv_event_cb_t cb)
{
    uint32_t c = COL_BLUE, fg = COL_BLUE;
    if (kind == 1) { c = COL_SEC; fg = COL_TEXT; }
    else if (kind == 2) { c = COL_RED; fg = COL_RED; }
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, DM(176), DM(60));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(c), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(btn, DM(6), 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(c), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    lv_obj_set_user_data(btn, l);
    return btn;
}

/* 自适应补高（P229 暗光 / P232 健康弹窗按钮半显修复）：卡片高=CONTENT 靠
 * flex 首轮测量；若测量滞后致按钮底部超出内容区，按实测把卡片撑足。
 * P233 修正：v1 量的是按钮行底边——若行自身高度被低估，按钮仍会探出行外；
 * 现直接量每颗按钮的绝对坐标，只增不减。每次 show 后调用。 */
void dm_popup_fit(lv_obj_t *card, lv_obj_t *btnrow)
{
    uint32_t i, n;
    int32_t want = 0, have, pad_top;
    lv_area_t ca, ba;
    if (card == NULL || btnrow == NULL) return;
    lv_obj_update_layout(card);   /* 同步刷全屏 pending 布局，坐标新鲜 */
    lv_obj_get_coords(card, &ca);
    pad_top = lv_obj_get_style_pad_top(card, LV_PART_MAIN);
    have = lv_area_get_height(&ca) - pad_top
         - lv_obj_get_style_pad_bottom(card, LV_PART_MAIN);
    n = lv_obj_get_child_count(btnrow);
    for (i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(btnrow, (int32_t)i);
        int32_t bottom;
        if (b == NULL) continue;
        lv_obj_get_coords(b, &ba);
        bottom = ba.y2 - ca.y1 - pad_top;   /* 换算到卡片内容坐标 */
        if (bottom > want) want = bottom;
    }
    if (want > have)
        lv_obj_set_height(card, lv_obj_get_height(card) + (want - have));
    lv_obj_update_layout(card);
    LV_LOG_USER("[popup] fit card=%p btn_bottom=%d content_h=%d card_h=%d",
           card, (int)want, (int)have,
           (int)lv_obj_get_height(card));
}

/* UTF-8 截断保护（2026-09-14 中文文件名修复）：strncpy/snprintf 按字节截断
 * 可能把多字节字符拦腰劈开，尾部残留非法序列 → LVGL 显示 �。
 * 本函数把尾部不完整序列回退掉，保证 NUL 结尾且字节合法。
 * Phase 1 拆分：ui_files.c / ui_books.c 经 deskmate_ui.h extern 调用 */
void dm_utf8_trim(char *s)
{
    size_t n, k;
    unsigned char c;
    int need;

    if (s == NULL) return;
    n = strlen(s);
    if (n == 0) return;
    c = (unsigned char)s[n - 1];
    if ((c & 0x80) == 0) return;               /* ASCII：完整 */

    k = 0;                                     /* 末尾连续 continuation 字节数 */
    while (k < n && ((unsigned char)s[n - 1 - k] & 0xC0) == 0x80) k++;
    if (k >= n) { s[0] = '\0'; return; }       /* 全 continuation：非法，清空 */
    c = (unsigned char)s[n - 1 - k];           /* 候选头字节 */
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else { s[n - k] = '\0'; return; }          /* 头非法：丢弃尾部 continuation */

    if ((size_t)need == k + 1) return;         /* 完整字符：保留 */
    s[n - 1 - k] = '\0';                       /* 不完整：从该头截断 */
}

/* ================================================================
 * APP DATA STRUCTURE
 * ================================================================ */

/* Phase 1 拆分：struct deskmate_app / clock_update_cb / app_icon_click_cb /
 * fade_dim_cb / launch_app_after_fade / screen_input_cb 已搬至 ui/ui_home.c
 * （deskmate_ui.h 声明跨文件引用） */

/* ================================================================
 * SCREEN INPUT — reset idle timer on any interaction
 * ================================================================ */

/* Phase 1 拆分：subpage_overlay 被 ui_books.c（阅读器挂载）extern 引用 */
lv_obj_t *subpage_overlay;
/* 嵌套父 overlay：Settings 内打开 WiFi/蓝牙子页时保存 Settings 的
 * overlay，关闭子页后恢复（2026-08-08 修复：原 show_subpage 防重入
 * 把嵌套打开直接 return，导致点 WiFi/蓝牙行进不去子页） */
static lv_obj_t *subpage_parent;

/* Phase 1 拆分：close_subpage 被 ui_files.c（files_go_up 退出子页）extern 引用 */
void close_subpage(void);
/* Phase 1 拆分：music_ui_clear_ptrs 被 close_subpage 调用（定义在 ui_music.c） */
void music_ui_clear_ptrs(void);

/* Phase 1 拆分：close_subpage 被 ui_files.c（files_go_up 退出子页）extern 引用 */
void close_subpage(void)
{
    if (subpage_overlay) {
        /* 音乐后台化（2026-08-08）：退出音乐页只关 UI，不停音乐——
         * 音乐是系统级常驻能力，XPlayer/解码线程/播放状态保留，
         * 由系统级 music_timer 继续驱动（见 create_music_subpage 注释）。
         * 进度 timer 由系统级持有，不在子页面关闭时删除。 */
        /* 先清空音乐 UI 指针再删除对象（2026-08-08 审计）：
         * 若顺序相反，lv_obj_del 之后、置空之前的窗口内，LVGL 事件
         * 队列中待处理的回调（或未来 lv_async_call）可能引用已释放的
         * label/slider。先置空消除一切引用，再删对象，杜绝野指针。 */
        music_ui_clear_ptrs();
        /* WiFi 密码输入层（wifijianyi.md 扫雷漏洞 1）：该 overlay 挂在
         * 根屏幕 scr 上，不随 subpage_overlay 删除——必须显式销毁并
         * 置 NULL。Phase 1 拆分：对象+清理一起搬至 ui_wifi.c。 */
        ui_wifi_close_cleanup();
        /* 阅读器覆盖层挂在根屏幕，不随 subpage_overlay 删除——显式清理 */
        book_reader_close();
        /* Files 多选操作栏/上级按钮（挂 overlay/content 上）——删除前置空 */
        files_ui_clear_ptrs();
        /* UART 调试工具：停刷新 timer + 关串口（内部恢复 LD2410B） */
        ui_uart_dbg_close_cleanup();
        /* 2026-09-10 宠物子页：清起名键盘/浮层 + 重置浮层指针（对象随父删，
         * 此处只销毁 kb 状态 + 置空，防重进野指针致命名点不开） */
        ui_pet_close_cleanup();
        /* 2026-09-10 闹钟子页：清添加面板（对象随父删，只置空指针） */
        ui_alarm_close_cleanup();
        /* 2026-09-10 日历子页：停午夜 timer + 纪念日浮层/键盘清理 */
        ui_calendar_close_cleanup();
        /* 2026-09-10 P211 日期与时间子页：停 hero timer + 手配面板清理 */
        ui_datetime_close_cleanup();
        /* 2026-09-10 设置页：清日期手配面板（同上，防野指针致下次点不开） */
        ui_settings_close_cleanup();
        /* 2026-08-10 子页状态栏（挂 overlay 上）——删除前置空防野指针 */
        subpage_clock_lbl   = NULL;
        subpage_date_lbl    = NULL;
        subpage_weather_lbl = NULL;
        subpage_wifi_lbl    = NULL;
        subpage_bt_lbl      = NULL;
        /* 2026-08-12 AI 子页：先删轮询 timer 再置空 UI 指针（防回调访问
         * 已释放对象；dm_ai 后台线程自清理，无需干预） */
        if (g_ai_timer)
            {
                lv_timer_del(g_ai_timer);
                g_ai_timer = NULL;
            }
        /* 2026-08-12 审查修复（B1）：PTT 录音中退出子页必须补发 stop——
         * 否则 agent 侧 voice_channel 一直录音占用 capture（无超时），
         * 后续语音/录音全部 EBUSY（P63 同款坑重现）。 */
        if (g_ai_auto_stop_timer)
            {
                lv_timer_del(g_ai_auto_stop_timer);
                g_ai_auto_stop_timer = NULL;
            }
        if (ai_voice_active)
            {
                dm_ai_voice("stop");
                ai_voice_active = false;
            }
        /* 2026-08-15 P83：不再自动发 wake_start（纯按钮 PTT），
         * 此处 wake_stop 仅防御性保留——若旧固件/其他路径曾启动过
         * wake 线程，退出子页确保停掉，避免残留监听抢声卡 */
        dm_ai_voice("wake_stop");
        /* 2026-08-23 review-7：不再 dm_ai_voice_evt_unsubscribe()——
         * 语音事件订阅已改系统级常驻（deskmate_ui_create），退出 AI 子页
         * 停订阅会导致锁屏/主界面语音命令（cmd 事件）收不到。订阅线程
         * 常驻成本可忽略（一条 WS 连接），stt/llm 仅在 AI 子页消费显示。
         * 清文字区指针（防 ai_poll_cb 访问已释放对象）。 */
        ai_chat_lbl = NULL;
        ai_status_lbl = NULL;
        ai_voice_btn  = NULL;
        lv_obj_del(subpage_overlay);
        subpage_overlay = NULL;

        if (subpage_parent) {
            /* 嵌套子页（WiFi/蓝牙）关闭 → 恢复父 overlay（Settings），
             * dock 仍保持隐藏（父 overlay 还是全屏子页） */
            subpage_overlay = subpage_parent;
            subpage_parent  = NULL;
            lv_obj_move_foreground(subpage_overlay);
            return;
        }

        /* Restore dock — but not while standby is showing (standby hides
         * the dock; restoring it here would let it peek through the lock
         * screen when a subpage was open at lock time). */
        if (g_dock && (!standby_overlay ||
                       lv_obj_has_flag(standby_overlay, LV_OBJ_FLAG_HIDDEN)))
            lv_obj_clear_flag(g_dock, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ================================================================
 * 共享键盘工具（dm_kb_*）
 * 统一管理 LVGL 键盘的创建、显隐、事件路由。
 * ================================================================ */

typedef void (*dm_kb_ready_cb_t)(lv_obj_t *ta, void *user_data);

typedef struct dm_kb_state_s {
    lv_obj_t          *kb;           /* 键盘对象 */
    lv_obj_t          *ta;           /* 关联的 textarea */
    dm_kb_ready_cb_t   on_ready;     /* 回车回调（LV_EVENT_READY） */
    void              *user_data;
    lv_coord_t         ta_parent_orig_y; /* 键盘弹出前 textarea 父容器 y 坐标 */
    bool               ta_parent_saved;  /* 是否已保存过原始坐标 */
} dm_kb_state_t;

/* 键盘隐藏/显示回调：textarea 聚焦→显示+上移父容器，失焦→隐藏+复位 */
static void dm_kb_ta_focused_cb(lv_event_t *e)
{
    dm_kb_state_t *state = lv_event_get_user_data(e);
    if (!state || !state->kb)
        return;
    lv_obj_clear_flag(state->kb, LV_OBJ_FLAG_HIDDEN);

    /* 把 textarea 的父容器移到键盘上方，避免被键盘遮挡 */
    lv_obj_t *ta_parent = lv_obj_get_parent(state->ta);
    if (ta_parent && !state->ta_parent_saved) {
        state->ta_parent_orig_y = lv_obj_get_y(ta_parent);
        state->ta_parent_saved  = true;
    }
    if (ta_parent) {
        lv_coord_t kb_top = lv_obj_get_y(state->kb);
        lv_coord_t gap    = DM(4);
        lv_coord_t target = kb_top - lv_obj_get_height(ta_parent) - gap;
        if (target < 0) target = 0;
        lv_obj_set_y(ta_parent, target);
    }
}

static void dm_kb_ta_defocused_cb(lv_event_t *e)
{
    dm_kb_state_t *state = lv_event_get_user_data(e);
    if (!state || !state->kb)
        return;
    lv_obj_t *new_focus = lv_event_get_param(e);
    if (new_focus == state->kb || new_focus == state->ta)
        return;
    lv_obj_add_flag(state->kb, LV_OBJ_FLAG_HIDDEN);

    /* 恢复 textarea 父容器原始位置 */
    if (state->ta_parent_saved) {
        lv_obj_t *ta_parent = lv_obj_get_parent(state->ta);
        if (ta_parent)
            lv_obj_set_y(ta_parent, state->ta_parent_orig_y);
        state->ta_parent_saved = false;
    }
}

/* 键盘事件：回车→on_ready 回调，取消→隐藏 */
static void dm_kb_event_cb(lv_event_t *e)
{
    dm_kb_state_t *state = lv_event_get_user_data(e);
    if (!state)
        return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (state->on_ready)
            state->on_ready(state->ta, state->user_data);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(state->kb, LV_OBJ_FLAG_HIDDEN);
    }
}

dm_kb_state_t *dm_kb_create(lv_obj_t *parent, lv_obj_t *ta,
                             dm_kb_ready_cb_t on_ready, void *user_data)
{
    if (!parent || !ta)
        return NULL;

    dm_kb_state_t *state = lv_malloc(sizeof(dm_kb_state_t));
    if (!state)
        return NULL;
    memset(state, 0, sizeof(*state));

    state->ta        = ta;
    state->on_ready  = on_ready;
    state->user_data = user_data;

    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(50));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_30, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_keyboard_set_textarea(kb, ta);

    /* textarea 聚焦/失焦 → 显隐键盘 */
    lv_obj_add_event_cb(ta, dm_kb_ta_focused_cb, LV_EVENT_FOCUSED, state);
    lv_obj_add_event_cb(ta, dm_kb_ta_defocused_cb, LV_EVENT_DEFOCUSED, state);

    /* 键盘 READY/CANCEL 事件 */
    lv_obj_add_event_cb(kb, dm_kb_event_cb, LV_EVENT_READY, state);
    lv_obj_add_event_cb(kb, dm_kb_event_cb, LV_EVENT_CANCEL, state);

    /* 初始隐藏 */
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    state->kb = kb;
    return state;
}

void dm_kb_hide(dm_kb_state_t *state)
{
    if (state && state->kb)
        lv_obj_add_flag(state->kb, LV_OBJ_FLAG_HIDDEN);
}

void dm_kb_destroy(dm_kb_state_t *state)
{
    if (!state)
        return;
    /* 解绑 textarea 事件（用 user_data 匹配）
     * 防悬挂指针：overlay 被 lv_obj_del 时子对象已销毁 */
    if (state->ta && lv_obj_is_valid(state->ta)) {
        lv_obj_remove_event_cb_with_user_data(state->ta,
                                              dm_kb_ta_focused_cb, state);
        lv_obj_remove_event_cb_with_user_data(state->ta,
                                              dm_kb_ta_defocused_cb, state);
    }
    if (state->kb && lv_obj_is_valid(state->kb)) {
        lv_obj_remove_event_cb_with_user_data(state->kb,
                                              dm_kb_event_cb, state);
        lv_obj_del(state->kb);
    }
    lv_free(state);
}

static void subpage_back_cb(lv_event_t *e)
{
    (void)e;
    close_subpage();
}

/* Phase 1 拆分：show_subpage 被 ui_settings.c（WiFi/蓝牙嵌套子页入口）extern 引用 */
void show_subpage(const char *title)
{
    /* 2026-08-08 嵌套导航：Settings 内点 WiFi/蓝牙行开子页时，
     * subpage_overlay 已存在（Settings 自己）。把当前 overlay 存为
     * subpage_parent，新子页盖在其上；close_subpage 恢复父 overlay。
     * 仅允许一层嵌套：subpage_parent 已非空（已在 WiFi/蓝牙子页里）
     * 时拒绝第三层，同时防点击抖动双重创建。 */
    LV_LOG_USER("show_subpage: %s (overlay=%p parent=%p)", title, subpage_overlay, subpage_parent);
    if (subpage_overlay) {
        if (subpage_parent) {
            /* 已在嵌套子页内（WiFi/蓝牙页再点行）：先关闭当前子页恢复父
             * overlay，再重新进入同一层嵌套——实现子页间切换与重复进入
             * 刷新（2026-08-09 修复：原逻辑直接 return，点 WiFi/蓝牙行
             * 毫无响应，日志只打一行） */
            lv_obj_t *p = subpage_parent;
            close_subpage();          /* 恢复父 overlay，parent 置空 */
            subpage_parent = p;       /* 保持嵌套层，继续开新子页 */
        } else {
            subpage_parent = subpage_overlay;   /* 保存父（Settings） */
        }
    }

    lv_obj_t *scr = lv_scr_act();
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

    /* Full-screen overlay */
    subpage_overlay = lv_obj_create(scr);
    lv_obj_set_size(subpage_overlay, sw, sh);
    lv_obj_set_style_bg_color(subpage_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(subpage_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(subpage_overlay, 0, 0);
    lv_obj_set_style_radius(subpage_overlay, 0, 0);
    lv_obj_set_style_pad_all(subpage_overlay, 0, 0);
    lv_obj_set_scrollbar_mode(subpage_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(subpage_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* 2026-08-10 修复：scr 是 flex column 布局（ui_home.c），overlay 若
     * 留在 flex 流中会被布局系统重排——lv_obj_set_y(sh) 与 slide-in 动画
     * 全部失效（日志佐证：Open subpage done 时 overlay y=0 而非 sh），
     * WiFi/蓝牙嵌套子页被排到父 overlay 之后、屏幕外，表现为"点行进不去"。
     * FLOATING 让 overlay 脱离 flex 布局，set_y/动画恢复生效。 */
    lv_obj_add_flag(subpage_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_move_foreground(subpage_overlay);
    /* Hide dock behind overlay */
    if (g_dock) lv_obj_add_flag(g_dock, LV_OBJ_FLAG_HIDDEN);

    LV_LOG_USER("show_subpage: overlay created y=%d sh=%d", lv_obj_get_y(subpage_overlay), sh);

    /* Slide-in animation from bottom */
    lv_obj_set_y(subpage_overlay, sh);
    lv_anim_t slide_in;
    lv_anim_init(&slide_in);
    lv_anim_set_var(&slide_in, subpage_overlay);
    lv_anim_set_exec_cb(&slide_in, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&slide_in, sh, 0);
    lv_anim_set_time(&slide_in, 300);
    lv_anim_set_path_cb(&slide_in, lv_anim_path_ease_out);
    lv_anim_start(&slide_in);

    /* Round back button — frosted glass, below the status bar (top-left) */
    lv_obj_t *back_btn = lv_btn_create(subpage_overlay);
    lv_obj_set_size(back_btn, DM(40), DM(40));
    /* 2026-08-10 修复（-23）：状态栏 132px 全宽固定顶部，返回键必须下移
     * 让出状态栏，禁止元素冲突（deskmate-ui skill 铁律2） */
    lv_obj_set_pos(back_btn, DM(16), TOP_BAR_H + DM(4));
    assert(DM(16) + DM(40) <= sw && TOP_BAR_H + DM(4) + DM(40) <= sh &&
           "deskmate: 返回键越界");
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, subpage_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_ICON, 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(back_icon);

    /* ── 2026-08-10 子页状态栏：与 Home 顶部完全一致（132px 全宽）──
     * 复用 create_status_bar_ex（ui_home.c）：左时钟两行(时间/周几日期) +
     * 中天气 + 右 wifi/bt/battery 图标，与 Home header 同一份代码、视觉一致。
     * 子页全屏 overlay 盖住主屏 header → 必须自带状态栏。
     * 指针由 clock_update_cb / weather_update_cb / dm_net_status_refresh
     * 每秒刷新，close_subpage 删除 overlay 前置空。 */
    lv_obj_t *sbar = make_clean_cont(subpage_overlay);
    lv_obj_set_size(sbar, lv_pct(100), TOP_BAR_H);
    lv_obj_set_style_bg_color(sbar, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(sbar, 0, 0);
    lv_obj_set_style_radius(sbar, 0, 0);
    lv_obj_align(sbar, LV_ALIGN_TOP_MID, 0, 0);
    create_status_bar_ex(sbar, &subpage_clock_lbl, &subpage_date_lbl,
                         &subpage_weather_lbl,
                         &subpage_wifi_lbl, &subpage_bt_lbl);

    /* Ensure back button is on top of content */
    lv_obj_t *content = lv_obj_create(subpage_overlay);
    lv_obj_set_size(content, sw, sh);
    lv_obj_set_style_bg_opa(content, LV_OPA_0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, EDGE_PAD, 0);
    /* 2026-08-10 修复（-22 初版/-23 完善）：状态栏 132px 全宽固定顶部，
     * 页面内容必须整体下移让位。初版 TOP_BAR_H+DM(10) 只避开状态栏，
     * 未避开下移后的返回键（底边 TOP_BAR_H+DM(44)）→ 内容仍压返回键。
     * 现 pad_top=TOP_BAR_H+DM(48) 同时让出状态栏与返回键，禁止元素冲突。 */
    lv_obj_set_style_pad_top(content, TOP_BAR_H + DM(48), 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    /* keep SCROLLABLE so long pages (Settings) can be dragged down */

    /* 2026-08-10 修复（-27）：状态栏必须在滚动内容之上——content 晚于 sbar
     * 创建，LVGL 后创建的子对象绘制在上层；Settings 等长页向上滚动时内容
     * 会滑入状态栏区域并覆盖它。move_foreground(sbar) 让状态栏恒在顶层，
     * back_btn 再 move_foreground 保持最顶。 */
    lv_obj_move_foreground(sbar);
    lv_obj_move_foreground(back_btn);

    LV_LOG_USER("show_subpage: content created, calling builder for %s", title);

    /* Dispatch to page builders */
    if      (strcmp(title, "Music") == 0)    ui_music_create(content);
    else if (strcmp(title, "Calendar") == 0) ui_calendar_create(content);
    else if (strcmp(title, "Books") == 0)    ui_books_create(content);
    else if (strcmp(title, "Files") == 0)    ui_files_create(content);
    else if (strcmp(title, "AI") == 0)       create_ai_subpage(content);
    else if (strcmp(title, "Settings") == 0) ui_settings_create(content);
    else if (strcmp(title, "WiFi") == 0)     ui_wifi_create(content);
    else if (strcmp(title, "Bluetooth") == 0) ui_bt_create(content);
    else if (strcmp(title, "Pet") == 0)       ui_pet_create(content);
    else if (strcmp(title, "Alarm") == 0)     ui_alarm_create(content);
    else if (strcmp(title, "Datetime") == 0)  ui_datetime_create(content);
    else if (strcmp(title, "UART") == 0)      ui_uart_dbg_create(content);
    else {
        lv_obj_t *placeholder = lv_label_create(content);
        lv_label_set_text(placeholder, "敬请期待…");
        lv_obj_set_style_text_font(placeholder, FONT_BODY, 0);
        lv_obj_set_style_text_color(placeholder, lv_color_hex(COL_SEC), 0);
    }

    LV_LOG_USER("Open subpage: %s done (overlay y=%d)", title, lv_obj_get_y(subpage_overlay));
}

/* ================================================================
 * SETTINGS HELPERS — row & group builders
 * ================================================================ */

/* Sub-page large title — iOS style, same look as Settings' title:
 * FONT_TITLE, primary text, small bottom pad. Sits right below the
 * round back button (content pad_top DM(48) clears it). */
lv_obj_t *subpage_big_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_pad_bottom(lbl, 12, 0);
    return lbl;
}

/* Create a section header label */
lv_obj_t *settings_section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_pad_left(lbl, 16, 0);
    lv_obj_set_style_pad_top(lbl, 20, 0);
    lv_obj_set_style_pad_bottom(lbl, 8, 0);
    return lbl;
}

/* Create a card group container for settings rows */
lv_obj_t *settings_group_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_card, 0);
    /* Unified card radius: same 40px as every card across the app */
    lv_obj_set_style_radius(card, RAD_CARD, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_left(card, 16, 0);
    lv_obj_set_style_pad_right(card, 16, 0);
    lv_obj_set_style_pad_top(card, 4, 0);
    lv_obj_set_style_pad_bottom(card, 4, 0);
    /* Stronger elevation so each group reads as a floating iOS card */
    lv_obj_set_style_shadow_width(card, 16, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/* Create a settings row with icon, label, value text, and optional arrow */
lv_obj_t *settings_row_value(lv_obj_t *card, const char *icon,
                                     uint32_t icon_color, const char *label,
                                     const char *value, bool show_arrow)
{
    lv_obj_t *row = make_clean_cont(card);
    lv_obj_set_size(row, lv_pct(100), SETTING_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* Pressed visual feedback for clickable rows */
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, RAD_CARD, LV_STATE_PRESSED);

    /* Icon badge: colored rounded square */
    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, DM(36), DM(36));
    lv_obj_set_style_radius(badge, RAD_CARD, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    /* Colored glow — echoes Music cover / Games badge */
    lv_obj_set_style_shadow_width(badge, 8, 0);
    lv_obj_set_style_shadow_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(badge);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, FONT_ICON, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ic);

    /* Label */
    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, FONT_BODY, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(lb, 1);

    /* Value text */
    if (value) {
        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, value);
        lv_obj_set_style_text_font(val, FONT_BODY, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(COL_SEC), 0);
        /* Expose the value label so cycle callbacks (Auto-Lock) can update it */
        lv_obj_set_user_data(row, val);
    }

    /* Arrow */
    if (show_arrow) {
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, FONT_BODY, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COL_SEC), 0);
    }

    return row;
}

/* Create a settings row with icon, label, and a switch */
/* wifijianyi.md 事件冒泡修复：Switch 自消费 CLICKED，防止冒泡到 Row。
 * 原先用 target!=current_target 拦截在复合控件中会误杀点击，改为
 * switch 自己 stop_bubbling，Row 保留干净 CLICKED 响应。 */
static void settings_switch_consume_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

lv_obj_t *settings_row_switch_cb(lv_obj_t *card, const char *icon,
                                        uint32_t icon_color, const char *label,
                                        bool initial_state, lv_event_cb_t cb)
{
    lv_obj_t *row = make_clean_cont(card);
    lv_obj_set_size(row, lv_pct(100), SETTING_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    /* Pressed visual feedback for clickable rows */
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, RAD_CARD, LV_STATE_PRESSED);

    /* Icon badge */
    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, DM(36), DM(36));
    lv_obj_set_style_radius(badge, RAD_CARD, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    /* Colored glow — echoes Music cover / Games badge */
    lv_obj_set_style_shadow_width(badge, 8, 0);
    lv_obj_set_style_shadow_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(badge);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, FONT_ICON, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ic);

    /* Label */
    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, FONT_BODY, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(lb, 1);

    /* Switch */
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, DM(44), DM(24));
    if (initial_state)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb)
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* wifijianyi.md：switch 自消费 CLICKED，不冒泡到 Row（行点击进子页） */
    lv_obj_add_event_cb(sw, settings_switch_consume_cb, LV_EVENT_CLICKED, NULL);

    return row;
}

/* No-callback variant (all other settings rows) */
lv_obj_t *settings_row_switch(lv_obj_t *card, const char *icon,
                                     uint32_t icon_color, const char *label,
                                     bool initial_state)
{
    return settings_row_switch_cb(card, icon, icon_color, label,
                                  initial_state, NULL);
}

/* Create a settings row with a slider (for brightness/volume) */
lv_obj_t *settings_row_slider(lv_obj_t *card, const char *icon,
                                      uint32_t icon_color, const char *label,
                                      int value_pct, lv_event_cb_t cb)
{
    lv_obj_t *row = make_clean_cont(card);
    lv_obj_set_size(row, lv_pct(100), SETTING_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    /* Pressed visual feedback for clickable rows */
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, RAD_CARD, LV_STATE_PRESSED);

    /* Icon badge */
    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, DM(36), DM(36));
    lv_obj_set_style_radius(badge, RAD_CARD, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    /* Colored glow — echoes Music cover / Games badge */
    lv_obj_set_style_shadow_width(badge, 8, 0);
    lv_obj_set_style_shadow_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(badge);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, FONT_ICON, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ic);

    /* Label */
    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, FONT_BODY, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(COL_TEXT), 0);

    /* Slider — thick with white knob. NOTE: no LV_PART_INDICATOR gradient
     * + LV_RADIUS_CIRCLE combo — LVGL 9.1 takes the layer/mask path there
     * and corrupts the heap (munmap_chunk crash, same as lv_bar). */
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, DM(6));
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(slider, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_BLUE),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, DM(3), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, DM(3), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, DM(3), LV_PART_KNOB);
    lv_slider_set_value(slider, value_pct, LV_ANIM_OFF);

    /* Value label */
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d%%", value_pct);
    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, vbuf);
    lv_obj_set_style_text_font(val, FONT_BODY, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(COL_SEC), 0);

    /* Optional real-action callback (e.g. backlight) */
    if (cb) {
        lv_obj_set_user_data(slider, val);
        lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    return row;
}

/* Add a thin separator line inside a group card */
void settings_add_separator(lv_obj_t *card)
{
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_scrollbar_mode(line, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
}

/* ================================================================
 * SETTINGS SUBPAGE — Phase 1 拆分：已整体搬至 ui/ui_settings.c
 * （ui_settings_create + settings 回调，deskmate_ui.h 声明跨文件引用）
 * ================================================================ */

/* 状态栏 wifi/bt 图标 ← 驱动实时状态（每秒由 clock timer 调用）
 * 2026-08-10 修复（-28）：开关语义——WiFi/BT 打开 → 显示蓝色符号；
 * 关闭 → 整枚隐藏（对齐 Settings 开关逻辑：开了才显示，弃"断开灰"
 * 低对比显示）。Home/子页/锁屏三处指针统一处理；隐藏后 flex 自动收拢不占位。 */
/* P146 三态图标刷新：st=0 隐藏 / st=1 灰色（未连接、扫描中/失败）/
 * st=2 蓝色（已连接、已开启）。WiFi 未连接不再整体隐藏——灰色图标
 * 常驻提示网络状态，消除"图标凭空消失"（用户反馈）。 */
static void dm_status_icon_refresh(lv_obj_t *lbl, int st)
{
    if (!lbl) return;
    if (st == 2) {
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_BLUE), 0);
    } else if (st == 1) {
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_SEC), 0);
    } else {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void dm_net_status_refresh(void)
{
    /* P145 修复：状态栏 WiFi 图标永不显示——原用 dm_net_wifi_is_up()
     * （wifi_is_up 读驱动内部关联标志），开机 start_wifi.sh 直连驱动路径
     * 下恒 0（P66 同款坑，P46/P47 教训）→ 图标永远 HIDDEN。
     * P146：三态——已连接=蓝；未连接（含扫描失败/进行中）=灰常驻；
     * 蓝牙保持 开=蓝 / 关=隐藏 两态。
     * P148 修复：未连接误判蓝色——IP 判定（dm_net_wifi_connected）在
     * 固件下载失败等场景下 wlan0 残留旧 IP（日志：有 10.0.0.2 但
     * wapi_get_essid failed、ssid=NULL not connected）→ 误报已连接。
     * 改用与 WiFi 子页「已连接 %s / 未连接网络」文本同源的 SSID 判定
     * （dm_net_wifi_cur_ssid = wifi_get_setting，日志证实未连接时正确
     * 返回空 SSID）。 */
    char ssid[64] = "";
    int wifi_st = (dm_net_wifi_cur_ssid(ssid, sizeof(ssid)) == 0 && ssid[0]) ? 2 : 1;
    int bt_st   = dm_net_bt_is_on() ? 2 : 0;   /* socket IPC 低频刷新在 dm_net.c 内部 */

    /* Home 状态栏 */
    dm_status_icon_refresh(status_wifi_lbl, wifi_st);
    dm_status_icon_refresh(status_bt_lbl,   bt_st);
    /* 子页状态栏（挂 overlay，关闭前置空） */
    dm_status_icon_refresh(subpage_wifi_lbl, wifi_st);
    dm_status_icon_refresh(subpage_bt_lbl,   bt_st);
    /* 锁屏状态栏（show_standby 创建） */
    dm_status_icon_refresh(standby_bar_wifi_lbl, wifi_st);
    dm_status_icon_refresh(standby_bar_bt_lbl,   bt_st);
}

/* Settings → 蓝牙开关（2026-08-08 接线 bluetoothd，实例懒创建）
 * Phase 1 拆分：ui_bt.c 控制组复用此回调（deskmate_ui.h 已声明） */
void settings_bt_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_obj_add_state(sw, LV_STATE_DISABLED);   /* 防连点 */
    dm_net_bt_set(on ? 1 : 0);
    dm_net_status_refresh();
    lv_obj_clear_state(sw, LV_STATE_DISABLED);
}

/* Phase 1 拆分：Settings WiFi/蓝牙行回调（settings_wifi_cb /
 * settings_wifi_row_open_cb / settings_bt_row_open_cb）与
 * create_settings_subpage 已搬至 ui/ui_settings.c（ui_settings_create） */

/* ================================================================
 * MUSIC SUBPAGE — ported from LVGL music demo (lv_demo_music)
 * Light theme, no intro animation, DM() scaled for 1920x1200.
 * Track list is scanned from the TF card (/sdcard/music), filtered
 * to the supported formats (mp3/flac/ogg/aac/wav).
 * ================================================================ */

#define DM_MAX_TRACKS   64
#define DM_MUSIC_DIR    "/sdcard/music"
/* Norflash user-space music dir (yaffs /data): used for test songs like
 * laojie.mp3 that bypass the SD card single-block-read I/O bottleneck
 * (MMCSD_MULTIBLOCK_LIMIT=1 — multi-block DMA read crashes on R528). */
#define DM_MUSIC_DIR_NORFLASH "/data/music"

/* Resume-memory: remember which track the player stopped on (like classic
 * MP3 players). Persisted to /data (yaffs, survives reboot); first run
 * (no file) defaults to track 0. */
#define DM_MUSIC_STATE_FILE "/data/dm_music.state"

/* P145：曲库/当前播放全局（需先于 music_state_save/load 定义——两者引用
 * dm_tracks/dm_track_count/dm_now_path。原定义块后移至此）。 */
struct dm_track
{
    char     path[256];   /* full file path */
    char     title[128];  /* filename without extension */
    uint32_t length;      /* seconds; 0 = unknown */
};
struct dm_track dm_tracks[DM_MAX_TRACKS];
uint32_t        dm_track_count;
/* P145：当前播放文件（独立于曲库列表）。music_play / music_play_path 统一
 * 同步，标题/进度/高亮一律以它为源——文件浏览器点播任意目录的文件后，
 * 播放器页扫描重建 dm_tracks 也不受影响。 */
char dm_now_path[256] = "";
char dm_now_title[128] = "";

static void music_state_save(uint32_t id)
{
    /* P145：持久化当前播放文件路径（非索引）——列表重建后索引会漂移。
     * 兼容：未设 dm_now 时退回 id 对应曲目路径。 */
    FILE *f = fopen(DM_MUSIC_STATE_FILE, "w");
    if (f)
    {
        const char *p = dm_now_path[0] ? dm_now_path
                      : (id < dm_track_count ? dm_tracks[id].path : "");
        fprintf(f, "%s\n", p);
        fclose(f);
    }
}

/* Phase 1 拆分：music_state_load 被 ui_music.c（ui_music_create 恢复曲目）extern 调用 */
uint32_t music_state_load(void)
{
    FILE *f = fopen(DM_MUSIC_STATE_FILE, "r");
    char path[256] = "";
    if (f)
    {
        if (fgets(path, sizeof(path), f) == NULL)
            path[0] = '\0';
        fclose(f);
    }
    path[strcspn(path, "\r\n")] = '\0';
    if (path[0] == '\0')
        return 0;

    /* P145：兼容旧格式（纯数字 = 旧索引） */
    int all_digit = 1;
    for (const char *p = path; *p; p++)
        if (*p < '0' || *p > '9') { all_digit = 0; break; }
    if (all_digit)
        return (uint32_t)atoi(path);

    /* 路径 → 在当前曲库列表中找索引（找不到 → 0，标题由 dm_now 兜底） */
    for (uint32_t i = 0; i < dm_track_count; i++)
        if (strcmp(dm_tracks[i].path, path) == 0)
            return i;
    return 0;
}

/* 2026-08-23 死机修复：1 = XPlayer 已被 force_stop（XPlayerStop → PlayerClear
 * 清空媒体+关 sink）处于 STOPPED 态——此后 XPlayerStart 无效且会崩（XPlayer
 * 线程 Prefetch abort，上板实证 16:51:20）。恢复播放必须先重建播放器。 */
static int          g_music_player_cleared;

lv_obj_t    *music_play_btn;
lv_obj_t    *music_play_icon;
lv_obj_t    *music_slider;
lv_obj_t    *music_time_lbl;
/* 诊断（2026-08-07 切歌无声排查）：dm_sound_write 衰减前原始 peak/nz/rms
 * 由 write 线程统计、打印线程读取（同一 LVGL timer 线程上下文，无竞态）。 */
static int          g_dbg_raw_peak;
static int          g_dbg_raw_nz;
static int          g_dbg_raw_rms;    /* 衰减前 RMS（全部样本均方根），区分
                                       * "数据小" vs "静音/直流" */

/* 强制 mono 混音实验开关（2026-08-07 切歌无声排查）：L/R 平均 → dual mono
 * 后写声卡。验证"外部功放单端合并处反相抵消"假设——若 mono 后无声歌曲
 * 恢复声音 = 反相抵消坐实；仍无声 = 问题在解码输出内容。实验已排除反相
 * （mono 后仍无声），2026-08-08 收敛：置 0 恢复立体声。 */
#define DM_FORCE_MONO 0
/* 软件音量 0-100（2026-08-07 播放器音量 UI）：dm_sound_write 按此值
 * 对 S16 样本做定点衰减，默认 30（=9830/32768，保持既有听感不变）。
 * Phase 1 拆分：ui_settings.c settings_volume_cb 经 deskmate_ui.h extern 引用 */
int             g_music_volume = 30;
lv_obj_t        *music_vol_lbl;      /* 播放器右侧音量数值显示（0-100） */
lv_obj_t    *music_time_total_lbl;
lv_obj_t    *music_title_lbl;
lv_obj_t    *music_artist_lbl;
lv_obj_t    *music_playlist_overlay;   /* full-screen, hidden */
lv_obj_t    *music_list_cont;          /* list inside overlay */
lv_obj_t    *music_empty_lbl;          /* "no tracks" hint */
lv_obj_t    *music_active_btn;
uint32_t     music_track_id;
bool         music_playing;
bool         music_inited;   /* player has a loaded source (paused vs never-started) */

/* 音乐后台化（2026-08-08）：HOME 首页音乐小组件（常驻，不随子页面销毁）——
 * 显示当前歌曲名 + 播放状态，提供 上一曲/播放暂停/下一曲 控制。
 * 点击小组件进入完整播放器页面。UI 指针在 close_subpage 时置空，
 * 播放/暂停状态变化时同步更新（music_resume/music_pause）。
 * Phase 1 拆分：home_music_* 由 ui_home.c 创建，extern 共享 */
lv_obj_t    *home_music_card;      /* 小组件容器（可点击进播放器） */
lv_obj_t    *home_music_title_lbl; /* 当前歌曲名 */
lv_obj_t    *home_music_play_icon; /* 播放/暂停符号 */
bool         home_music_visible;   /* 小组件是否已创建（deskmate_screen 常驻） */

/* standby 锁屏音乐控件（2026-08-08 后台化）：standby_overlay 常驻，
 * 显示当前歌曲名 + 播放控制。standby_overlay 首次创建时挂载，
 * 歌曲名/播放图标由 music_update_track_info/music_resume/music_pause
 * 同步更新（判空保护：standby 未创建时为 NULL）。
 * Phase 1 拆分：standby_music_* 由 ui_home.c（show_standby）创建，extern 共享 */
lv_obj_t    *standby_music_title_lbl;
lv_obj_t    *standby_music_play_icon;

/* Phase 1 拆分：music_ui_clear_ptrs 已搬至 ui/ui_music.c（close_subpage 经
 * deskmate_ui.h extern 调用）；播放器页 UI 回调（music_play_pause/prev/next/
 * vol_cb、music_track_click_cb、playlist overlay、music_play_btn_create、
 * create_music_subpage → ui_music_create）亦已搬走。 */

void music_play(uint32_t id);
void music_resume(void);
void music_pause(void);
void music_album_next(bool next);
void music_update_track_info(void);
static void music_audio_poll(void);
static int  music_audio_position(int *ms);
static int  music_audio_duration(int *ms);

/* ══════════════════════════════════════════════════════════════
 * 本地设备控制命令分发（2026-08-23 P115/P116 表驱动重构）
 *
 * 结构：命令表（前缀 + needs_ack 确认策略 + 执行器）顺序匹配；
 * ai_poll_cb 消费 cmd 事件 → ai_local_cmd_dispatch() 查表执行。
 * 新增命令 = 写一个执行器 + 表加一行，无需改 ai_poll_cb。
 *
 * needs_ack 策略：音量/亮度=1（执行后 TTS 播报确认，不碰 XPlayer）；
 * 音乐=0（review-5 实证：起播后 XPlayer 独占声卡，紧随的 speak 必
 * EBUSY 静默失败 = "AI 不响应"——音乐本身即反馈，不播确认）。
 * ══════════════════════════════════════════════════════════════ */

struct dm_local_cmd {
    const char *prefix;              /* cmd 前缀："music:" / "volume:" 等 */
    int  needs_ack;                  /* 1=执行后 TTS 播报确认；0=不播（音乐即反馈） */
    void (*exec)(const char *arg, char *ack, size_t ack_cap);
};

/* ── 执行器（各命令一个；ack 仅在 needs_ack=1 时填写）── */

static void dm_cmd_music(const char *op, char *ack, size_t ack_cap)
{
    /* needs_ack=0：音乐本身即反馈，不 TTS 播报；仍填 ack 供对话区显示（B2） */
    if (strcmp(op, "play") == 0)
        {
            if (music_playing)
                {
                    printf("[voice-cmd] music already playing\n");
                    snprintf(ack, ack_cap, "音乐正在播放");
                    return;
                }
            else if (music_inited)
                music_resume();      /* 继续播放 */
            else
                music_play(0);       /* 补扫曲库 + 恢复上次/第一首 */
            snprintf(ack, ack_cap, "音乐继续播放");
        }
    else if (strcmp(op, "pause") == 0)
        {
            music_pause();
            snprintf(ack, ack_cap, "音乐已暂停");
        }
    else if (strcmp(op, "next") == 0)
        {
            music_album_next(true);
            snprintf(ack, ack_cap, "切到下一首");
        }
    else if (strcmp(op, "prev") == 0)
        {
            music_album_next(false);
            snprintf(ack, ack_cap, "切到上一首");
        }
    else
        {
            snprintf(ack, ack_cap, "音乐：%s", op);
        }
    printf("[voice-cmd] music:%s\n", op);
}

static void dm_cmd_volume(const char *val, char *ack, size_t ack_cap)
{
    int n = atoi(val);
    if (*val == '+' || *val == '-')
        g_music_volume += n;         /* 相对步进 */
    else
        g_music_volume = n;          /* 绝对设置 */
    if (g_music_volume < 0) g_music_volume = 0;
    if (g_music_volume > 100) g_music_volume = 100;
    if (music_vol_lbl)
        lv_label_set_text_fmt(music_vol_lbl, "%d", g_music_volume);
    /* 2026-09-14：落盘供 AI 语音（agent）同步 TTS；与音乐/设置页共用
     * 800ms 防抖，避免语音命令连发时反复刷 flash */
    music_volume_persist_debounced();
    printf("[voice-cmd] volume=%d\n", g_music_volume);
    snprintf(ack, ack_cap, "好的，音量已调到 %d%%", g_music_volume);
}

static void dm_cmd_brightness(const char *val, char *ack, size_t ack_cap)
{
    int n = atoi(val);
    if (*val == '+' || *val == '-')
        g_screen_brightness += n;
    else
        g_screen_brightness = n;
    if (g_screen_brightness < 0) g_screen_brightness = 0;
    if (g_screen_brightness > 100) g_screen_brightness = 100;
#ifdef CONFIG_LUNCHER_DM_APP
    {
        struct pwm_config pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.period_ns  = 25000;               /* 40KHz */
        pcfg.duty_ns    = (uint32_t)(25000u * g_screen_brightness / 100);
        pcfg.polarity   = PWM_POLARITY_NORMAL;  /* 与 settings 一致 */
        hal_pwm_control(4, &pcfg);              /* 屏幕背光 PWM ch4 */
    }
#endif
    printf("[voice-cmd] brightness=%d\n", g_screen_brightness);
    snprintf(ack, ack_cap, "好的，亮度已调到 %d%%", g_screen_brightness);
}

/* pet: feed|play|name <名字>|status */
static void dm_cmd_pet(const char *op, char *ack, size_t ack_cap)
{
    if (strncmp(op, "feed", 4) == 0)
    {
        /* pet:feed 鱼/肉/菜/零食 → 默认鱼 */
        dm_pet_on_event(PET_EVT_FEED_FISH);
        snprintf(ack, ack_cap, "已投喂");
    }
    else if (strncmp(op, "play", 4) == 0)
    {
        dm_pet_on_event(PET_EVT_RAPID_TAP);
        snprintf(ack, ack_cap, "陪它玩一会");
    }
    else if (strncmp(op, "name", 4) == 0)
    {
        const char *name = op + 4;
        while (*name == ' ') name++;
        if (*name)
        {
            dm_pet_set_name(name);
            snprintf(ack, ack_cap, "已改名为%s", name);
        }
        else
            snprintf(ack, ack_cap, "用法：pet:name 新名字");
    }
    else if (strncmp(op, "status", 6) == 0)
    {
        const dm_pet_data_t *pd = dm_pet_get_data();
        snprintf(ack, ack_cap,
                 "%s(猫) 饿%d 力%d 乐%d 亲%d 健%d",
                 pd->name[0] ? pd->name : "旺财",
                 pd->attrs.hunger, pd->attrs.energy, pd->attrs.happiness,
                 pd->attrs.affection, pd->attrs.health);
    }
    else
    {
        snprintf(ack, ack_cap, "用法：pet:feed|play|name <名字>|status");
    }
}

/* alarm:HH:MM:R（agent 中文时间解析后广播，R=0 一次/1 每天） */
static void dm_cmd_alarm(const char *arg, char *ack, size_t ack_cap)
{
    int hh = -1, mm = 0, rp = 1;

    if (sscanf(arg, "%d:%d:%d", &hh, &mm, &rp) < 2 || hh < 0 || hh > 23)
        {
            snprintf(ack, ack_cap, "没听清几点，再说一次吧");
            return;
        }
    if (mm < 0) mm = 0;
    if (mm > 59) mm = 59;
    if (rp != 0) rp = 1;
    if (dm_alarm_add(hh, mm, rp, 0) < 0)
        {
            snprintf(ack, ack_cap, "闹钟满了，先删一个吧");
            return;
        }
    snprintf(ack, ack_cap, "好的，%s%02d点%02d分叫你",
             rp ? "每天" : "", hh, mm);
}

/* ── 命令表（顺序即优先级）── */
static const struct dm_local_cmd g_dm_local_cmds[] = {
    { "music:",      0, dm_cmd_music },
    { "volume:",     1, dm_cmd_volume },
    { "brightness:", 1, dm_cmd_brightness },
    { "pet:",        0, dm_cmd_pet },
    { "alarm:",      1, dm_cmd_alarm },
};

/* 分发：查表匹配前缀 → 执行 + 按 needs_ack 播确认 */
static void ai_local_cmd_dispatch(const char *cmd)
{
    size_t i;

    for (i = 0; i < sizeof(g_dm_local_cmds) / sizeof(g_dm_local_cmds[0]); i++)
        {
            size_t plen = strlen(g_dm_local_cmds[i].prefix);
            if (strncmp(cmd, g_dm_local_cmds[i].prefix, plen) == 0)
                {
                    char ack[64] = "";
                    g_dm_local_cmds[i].exec(cmd + plen, ack, sizeof(ack));
                    if (ack[0])
                        {
                            /* B2（2026-09-11）：执行可见——对话区追加一行
                             * “执行：xxx”（AI 子页开着时即时刷新） */
                            ai_chat_append("执行", ack);
                            ai_chat_refresh();
                        }
                    if (g_dm_local_cmds[i].needs_ack && ack[0]) {
                        /* review-10：音量/亮度确认播报前挂起音乐，播完恢复 */
                        dm_audio_fg_acquire();
                        dm_ai_voice_speak(ack);
                    }
                    return;
                }
        }
    printf("[voice-cmd] unknown: %s\n", cmd);
}

/* 2026-08-23 常驻化（review-7）：系统级 cmd 事件消费 timer 回调。
 * 修复：锁屏/主界面按语音按钮说话（standby_ai_btn 复用 PTT）时，
 * AI 子页未开 → ai_poll_cb 定时器不存在 → agent 广播的 cmd 事件
 * 无人消费 → "播放音乐"等命令不生效。本 timer 在 deskmate_ui_create
 * 创建，不随 AI 子页启停，保证任意界面语音命令都能执行。 */
static void dm_ai_cmd_poll_cb(lv_timer_t *t)
{
    (void)t;
    static char evt_cmd[64];
    if (dm_ai_voice_evt_cmd_poll(evt_cmd, sizeof(evt_cmd)))
        ai_local_cmd_dispatch(evt_cmd);
    /* review-10 音频仲裁：消费前台音频完成信号 → 恢复被挂起的音乐 */
    dm_audio_fg_poll();
}

/* Parse RIFF/WAVE header for duration in seconds (0 if not a valid WAV). */
static uint32_t music_wav_duration(const char *path)
{
    FILE *fp;
    uint8_t hdr[44];
    uint32_t byte_rate, data_len;

    fp = fopen(path, "rb");
    if (fp == NULL) return 0;

    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
    {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return 0;

    memcpy(&byte_rate, hdr + 28, 4);
    memcpy(&data_len, hdr + 40, 4);
    if (byte_rate == 0) return 0;

    return data_len / byte_rate;
}

/* Scan /sdcard/music and /data/music (norflash) for supported audio files
 * (mp3/flac/ogg/aac/wav). Returns the number of tracks found. Call each
 * time the playlist overlay opens so newly copied songs appear.
 * Phase 1 拆分：music_scan_tracks 被 ui_music.c（playlist 刷新）extern 调用 */
uint32_t music_scan_tracks(void)
{
    DIR *dir;
    struct dirent *ent;
    const char *supported[] = { ".mp3", ".flac", ".ogg", ".aac", ".wav" };
    const char *scan_dirs[] = { DM_MUSIC_DIR, DM_MUSIC_DIR_NORFLASH };
    uint32_t count = 0;
    unsigned di;

    dm_track_count = 0;
    for (di = 0; di < sizeof(scan_dirs) / sizeof(scan_dirs[0]); di++)
    {
        const char *base = scan_dirs[di];

        dir = opendir(base);
        if (dir == NULL)
            continue;

        while ((ent = readdir(dir)) != NULL && count < DM_MAX_TRACKS)
        {
            const char *dot = strrchr(ent->d_name, '.');
            int i, ok = 0;

            if (dot == NULL || dot == ent->d_name)
                continue;

            for (i = 0; i < 5; i++)
            {
                if (strcasecmp(dot, supported[i]) == 0)
                {
                    ok = 1;
                    break;
                }
            }
            if (!ok)
                continue;

            snprintf(dm_tracks[count].path, sizeof(dm_tracks[count].path),
                     "%s/%s", base, ent->d_name);
            snprintf(dm_tracks[count].title, sizeof(dm_tracks[count].title),
                     "%.*s", (int)(dot - ent->d_name), ent->d_name);
            dm_tracks[count].length = music_wav_duration(dm_tracks[count].path);
            count++;
        }

        closedir(dir);
    }

    dm_track_count = count;
    return count;
}

/* Phase 1 拆分：music_timer_cb 被 ui_music.c（ui_music_create 兜底建 timer）extern 调用 */
void music_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* Drive XPlayer async state machine (PREPARED→start, COMPLETE/ERR→next). */
    music_audio_poll();
    if (!music_playing) return;
    if (music_track_id >= dm_track_count) return;
    /* 音乐后台化（2026-08-08）：子页面关闭后 music_title_lbl 等 UI 对象
     * 为 NULL（close_subpage 已置空）——后台播放时只轮询状态机与进度，
     * 跳过 UI 刷新，防止访问已释放对象。 */
    if (music_title_lbl == NULL || music_slider == NULL)
        return;

    /* Real progress from the player; fall back to simulated slider if the
     * backend has no position info (simulator / not yet prepared). */
    int pos_ms = -1, dur_ms = -1;
    music_audio_position(&pos_ms);
    music_audio_duration(&dur_ms);

    if (dur_ms > 0)
    {
        /* Real duration known → keep total-time label in sync */
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%d:%02d",
                 (int)(dur_ms / 60000), (int)((dur_ms / 1000) % 60));
        if (music_time_total_lbl) lv_label_set_text(music_time_total_lbl, tbuf);

        if (pos_ms >= 0)
        {
            uint32_t pos_s = pos_ms / 1000;
            uint32_t dur_s = dur_ms / 1000;
            lv_slider_set_range(music_slider, 0, dur_s);
            lv_slider_set_value(music_slider, pos_s, LV_ANIM_OFF);

            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d",
                     (int)(pos_s / 60), (int)(pos_s % 60));
            lv_label_set_text(music_time_lbl, buf);
            return;
        }
    }

    /* Fallback: simulated progress (no backend position) */
    uint32_t len = dm_tracks[music_track_id].length;
    if (len == 0) return;

    int32_t val = lv_slider_get_value(music_slider);
    val++;
    if (val > (int32_t)len) {
        val = 0;
        music_album_next(true);
        return;
    }
    lv_slider_set_value(music_slider, val, LV_ANIM_OFF);
    lv_slider_set_range(music_slider, 0, len);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", (int)(val / 60), (int)(val % 60));
    lv_label_set_text(music_time_lbl, buf);
}

/* Phase 1 拆分：music_play_pause_cb / music_prev_cb / music_next_cb /
 * music_vol_cb 已搬至 ui/ui_music.c（播放器页按钮回调） */

/* 2026-08-08：icon 是按钮的子 label，不继承父按钮的 PRESSED 状态样式，
 * 之前给 icon 设 LV_STATE_PRESSED 白色样式不生效 → 按下时符号不变白。
 * 改为显式事件回调：按下变白、松开/丢失恢复蓝（与播放键观感一致）。 */
/* Phase 1 拆分：music_round_btn_press_icon_cb 被 ui_files.c（多选操作栏）extern 引用 */
void music_round_btn_press_icon_cb(lv_event_t *e)
{
    lv_obj_t *icon = (lv_obj_t *)lv_event_get_user_data(e);
    if (!icon) return;
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
    else
        lv_obj_set_style_text_color(icon, lv_color_hex(COL_BLUE), 0);
}

/* 圆形玻璃控制按钮（2026-08-07 统一点击效果）：白色玻璃常态，
 * 按下变实心蓝 + 轻微放大 + 蓝色光晕，松开自动恢复白色玻璃。
 * 播放键的 PRESSED/CHECKED、prev/next、音量 +/- 全部走这一套，
 * 保证"一点就是蓝色、点完回玻璃"的观感一致。 */
/* Phase 1 拆分：music_round_btn 被 ui_books.c（阅读器翻页钮）extern 引用 */
lv_obj_t *music_round_btn(lv_obj_t *parent, lv_coord_t size,
                                 const char *symbol, lv_event_cb_t cb,
                                 void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(btn, 4, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    /* 按下：蓝紫渐变玻璃 + 蓝光晕（复刻播放键 CHECKED 观感），
     * 松开自动恢复白色玻璃 */
    lv_obj_set_style_transform_width(btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(COL_PURPLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 14, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    /* 与歌单行（music_track_click_cb）同模式存 user_data，回调统一用
     * lv_obj_get_user_data 读取（LVGL 9.1 已验证路径）。 */
    lv_obj_set_user_data(btn, user_data);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, FONT_BODY, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_BLUE), 0);
    /* icon 不继承父按钮 PRESSED 状态，用事件回调显式切换白/蓝 */
    lv_obj_add_event_cb(btn, music_round_btn_press_icon_cb,
                        LV_EVENT_PRESSED, icon);
    lv_obj_add_event_cb(btn, music_round_btn_press_icon_cb,
                        LV_EVENT_RELEASED, icon);
    lv_obj_add_event_cb(btn, music_round_btn_press_icon_cb,
                        LV_EVENT_PRESS_LOST, icon);
    lv_obj_center(icon);
    return btn;
}

/* Phase 1 拆分：music_play_btn_create / music_track_click_cb
 * 已搬至 ui/ui_music.c（播放器页 UI 回调） */

/* ── Playback backend (board only): libcedarx XPlayer (mp3/flac/ogg/aac/wav) ──
 * Audio sink is a self-made SoundCtrl on top of aw-alsa-lib (snd_vela_pcm_*):
 * NuttX has no standard ALSA (snd_pcm_*) that the SDK tplayer awsink needs.
 * XPlayer is async: SetDataSourceUrl → PrepareAsync → PREPARED → Start;
 * PLAYBACK_COMPLETE → auto next. The XPlayer callback thread only sets
 * g_xp_state; the LVGL music timer polls it and drives the UI. */
#ifdef CONFIG_LUNCHER_DM_APP

/* ---- SoundCtrl (aw-alsa-lib backend) ---- */
typedef struct
{
    SoundCtrl base;
    snd_pcm_t *handle;
    unsigned int samplerate;
    unsigned int channels;
    int bits;
} dm_sound_ctrl_t;

static void dm_sound_destroy(SoundCtrl *s)
{
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    if (sc->handle)
    {
        /* P174 尾音修复：TTS 播完后直接 close → 片段残留。
         * 先补 30ms 静音填充 + drain，确保 DAC 输出归零后再关 handle。
         * 参考 dm_tone_play_one 同款 fade-out 策略。 */
        unsigned int rate = sc->samplerate ? sc->samplerate : 48000;
        unsigned int ch   = sc->channels   ? sc->channels   : 2;
        int fade_frames   = (int)(rate * 30 / 1000);
        int done = 0;
        int16_t zero[512];
        memset(zero, 0, sizeof(zero));
        int max_frames = (int)(sizeof(zero) / (2 * ch));
        while (done < fade_frames)
        {
            int n = fade_frames - done;
            if (n > max_frames) n = max_frames;
            snd_vela_pcm_writei(sc->handle, zero, n);
            done += n;
        }
        snd_vela_pcm_drain(sc->handle);
        snd_vela_pcm_close(sc->handle);
    }
    free(sc);
}

static void dm_sound_set_format(SoundCtrl *s, CdxPlaybkCfg *cfg)
{
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    if (cfg)
    {
        sc->samplerate = cfg->nSamplerate ? cfg->nSamplerate : 48000;
        sc->channels   = cfg->nChannels   ? cfg->nChannels   : 2;
        sc->bits       = cfg->nBitpersample ? cfg->nBitpersample : 16;
    }
}

static int dm_sound_open(dm_sound_ctrl_t *sc)
{
    snd_pcm_hw_params_t *params;
    /* Match the verified-working reference (sunxi_alsa set_param uses
     * period_size/4, buffer_size/4 = 256/1024). dm_sound previously used
     * 1024/4096 which the codec may reject/accept differently → silence.
     * XPlayer (real decode + 44100→48000 resample) jitters more than aplay
     * sine, so a slightly larger 512/2048 buffer absorbs underruns. */
    /* 2026-08-07 切歌无声排查：日志证实周期性 XRUN（state=4）——buffer
     * 2048 帧仅 42.7ms，writei 偶发停顿（WRITE_GAP 71~2036ms）超过余量
     * → buffer 空 → avail>=stop_threshold → xrun → STOP/START 循环。
     * 修复：buffer 2048→8192（170ms），period 512→1024，吸收解码/线程
     * 抖动。start_threshold=buffer 语义不变（填满才启动）。 */
    snd_pcm_uframes_t period = 1024;
    snd_pcm_uframes_t buffer = 8192;
    int ret;

    if (sc->handle)
        return 0;

    ret = snd_vela_pcm_open(&sc->handle, "hw:audiocodec",
                            SND_VELA_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0)
    {
        printf("[music] snd_vela_pcm_open fail %d\n", ret);
        sc->handle = NULL;
        return ret;
    }

    snd_vela_pcm_hw_params_malloc(&params);
    snd_vela_pcm_hw_params_any(sc->handle, params);
    snd_vela_pcm_hw_params_set_access(sc->handle, params,
                                      SND_PCM_ACCESS_RW_INTERLEAVED);
    /* Always S16_LE: dm_sound_write downconverts 24/32-bit sources to 16-bit
     * (S24_LE here means a 32-bit physical container, which does not match the
     * decoder's packed 24-bit output — aplay sine S16_LE is the proven path). */
    snd_vela_pcm_hw_params_set_format(sc->handle, params,
                                      SND_PCM_FORMAT_S16_LE);
    snd_vela_pcm_hw_params_set_channels(sc->handle, params, sc->channels);
    snd_vela_pcm_hw_params_set_rate(sc->handle, params, sc->samplerate, 0);
    snd_vela_pcm_hw_params_set_period_size(sc->handle, params, period, 0);
    snd_vela_pcm_hw_params_set_buffer_size(sc->handle, params, buffer);
    ret = snd_vela_pcm_hw_params(sc->handle, params);
    snd_vela_pcm_hw_params_free(params);
    if (ret < 0)
    {
        printf("[music] hw_params fail %d\n", ret);
        snd_vela_pcm_close(sc->handle);
        sc->handle = NULL;
        return ret;
    }

    /* SW params are REQUIRED: the reference (aplay set_param) sets
     * start_threshold=buffer_size so the substream auto-starts once the
     * buffer fills. Without them the default start_threshold may never be
     * reached → writei succeeds but the codec never starts → silence. */
    {
        snd_pcm_sw_params_t *sw;
        snd_pcm_uframes_t boundary = 0;
        snd_pcm_sw_params_alloca(&sw);
        snd_vela_pcm_sw_params_current(sc->handle, sw);
        snd_vela_pcm_sw_params_get_boundary(sw, &boundary);
        snd_vela_pcm_sw_params_set_start_threshold(sc->handle, sw, buffer);
        snd_vela_pcm_sw_params_set_stop_threshold(sc->handle, sw, buffer);
        snd_vela_pcm_sw_params_set_silence_size(sc->handle, sw, boundary);
        snd_vela_pcm_sw_params_set_avail_min(sc->handle, sw, period);
        ret = snd_vela_pcm_sw_params(sc->handle, sw);
        if (ret < 0)
        {
            printf("[music] sw_params fail %d\n", ret);
            snd_vela_pcm_close(sc->handle);
            sc->handle = NULL;
            return ret;
        }
    }
    return 0;
}

static int dm_sound_start(SoundCtrl *s)
{
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    int ret = dm_sound_open(sc);
    if (ret < 0)
        return ret;
    return snd_vela_pcm_prepare(sc->handle);
}

static int dm_sound_stop(SoundCtrl *s)
{
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    if (sc->handle)
    {
        snd_vela_pcm_drain(sc->handle);
        snd_vela_pcm_close(sc->handle);
        sc->handle = NULL;
    }
    return 0;
}

static int dm_sound_pause(SoundCtrl *s)
{
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    if (sc->handle)
        return snd_vela_pcm_pause(sc->handle, 1);
    return 0;
}

static int dm_sound_write(SoundCtrl *s, void *data, int size)
{
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    snd_pcm_sframes_t frames;
    static int16_t conv[3840];      /* 24/32-bit → 16-bit scratch (max 40ms @48k) */
    void *out = data;
    int out_bytes = size;
    /* Source bytes per frame (packed): 16-bit=2*ch, 24-bit=3*ch, 32-bit=4*ch.
     * The caller (writeToSoundDevice) accounts in BYTES: it subtracts the
     * return value from nPcmDataLen, advances the pointer by it, and releases
     * that many decoder bytes — so we must return bytes, NOT frames. */
    int src_fb = ((sc->bits / 8) * sc->channels);
    int bps = 2 * sc->channels;     /* device always S16_LE */

    if (!sc->handle || !data || bps <= 0 || src_fb <= 0)
        return 0;

    /* 24-bit WAV (packed 3 B/sample) / 32-bit → downconvert to 16-bit.
     * aplay sine (S16_LE) is the only proven-audible path on this codec. */
    if (sc->bits >= 24)
    {
        int in_bps = (sc->bits / 8);
        int nsamp = size / in_bps;  /* total samples (frames * channels) */
        const uint8_t *src = data;
        int i;
        if (nsamp > (int)(sizeof(conv) / sizeof(conv[0])))
            nsamp = sizeof(conv) / sizeof(conv[0]);
        if (sc->bits == 24)
        {
            for (i = 0; i < nsamp; i++)
            {
                int32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
                if (v & 0x800000)
                    v |= ~0xFFFFFF;   /* sign-extend 24-bit */
                conv[i] = (int16_t)(v >> 8);
                src += 3;
            }
        }
        else
        {
            const int32_t *src32 = data;
            for (i = 0; i < nsamp; i++)
            {
                int32_t v = src32[i] >> 16;
                conv[i] = (int16_t)v;
            }
        }
        out = conv;
        out_bytes = nsamp * 2;
    }

    frames = out_bytes / bps;
    if (frames <= 0)
        return 0;

    /* 诊断（2026-08-07）：统计衰减前原始 peak/rms——区分"解码数据本身小"
     * vs "衰减/增益问题"。切歌无声排查中 Beautiful Love peak 仅 43~172
     * （衰减后），若衰减前原始 peak 同样极低 → 解码输出本身幅度小，
     * 与 30% 衰减无关；若原始 peak 正常 → 增益链路问题。RMS 补充判别：
     * 峰值高但 RMS≈0 → 数据是稀疏尖峰/直流，实际能量极低。 */
    {
        const int16_t *p16 = out;
        int ns = out_bytes / 2;
        int n = ns > 4096 ? 4096 : ns;
        int i;
        int64_t sum_sq = 0;
        g_dbg_raw_peak = 0;
        g_dbg_raw_nz = 0;
        for (i = 0; i < n; i++)
        {
            int v = p16[i];
            if (v < 0) v = -v;
            if (v > g_dbg_raw_peak) g_dbg_raw_peak = v;
            if (v != 0) g_dbg_raw_nz++;
            sum_sq += (int64_t)v * v;
        }
        if (n > 0)
            g_dbg_raw_rms = (int)sqrt((double)sum_sq / n);
        else
            g_dbg_raw_rms = 0;
    }

    /* 软件音量（用户需求）：codec digital_vol 直接改寄存器会绕过
     * sunxi_set_data_invert 反转语义（寄存器=63-音量）致整机无声
     * （ok-20260806-31 教训），改在此对 S16 样本做定点衰减，可靠可控。
     * gain = 音量(0-100)/100 * 32768：30% ≈ 9830（整数定点，无浮点开销）。 */
    {
        const int16_t *p16 = out;
        int ns = out_bytes / 2;
        int i;
        int gain = (g_music_volume * 32768) / 100;
        for (i = 0; i < ns; i++)
        {
            int v = p16[i];
            v = (v * gain) >> 15;
            /* P103 软限幅：解码/重采样输出副歌可达满幅（raw_peak=32767，
             * 日志实证），线性衰减后仍会顶格硬削波 → 破音/发沙。
             * 对 |v|>30000 的峰值做折线压缩（斜率 1/4，近似 tanh），
             * 保留动态但削去硬顶，改善听感。 */
            if (v > 30000)
                v = 30000 + (v - 30000) / 4;
            else if (v < -30000)
                v = -30000 - (v + 30000) / 4;
            ((int16_t *)out)[i] = (int16_t)v;
        }
    }

    {
        static int dbg_cnt;
        static uint32_t last_print_tick = 0;   /* 上次打印时的 lv_tick，算包间节奏 */
        int wr;
        int do_print = 0;
        uint32_t now_tick = 0, delta_ms = 0;
        /* 诊断统计（原始 stereo，mono 混音之前）——writei 后打印用 */
        int dbg_peak = 0, dbg_nz = 0, dbg_n = 0;
        int dbg_L_rms = 0, dbg_R_rms = 0;
        double dbg_corr = 0.0;

        if ((++dbg_cnt % 1000) == 1)   /* 每 1000 次打印一次（2026-08-08 收敛，
                                        * 原每 20 次 → 降频 50 倍，问题已定位） */
        {
            do_print = 1;
            /* 包间节奏诊断（2026-08-07 切歌无声排查）：1920 帧 @48k = 40ms/包，
             * 20 包累积正常应稳定 ≈800ms。若 delta_ms 出现 800→2500→5000 等
             * 大跳变 = 中途停顿（underrun/restart），区分"主动 pause/restart"
             * vs "PCM 供给不足导致 ALSA 重启"。 */
            now_tick = lv_tick_get();
            if (last_print_tick == 0)
                delta_ms = 0;
            else
                delta_ms = now_tick - last_print_tick;   /* uint32 回绕安全 */
            last_print_tick = now_tick;
            /* PCM 能量诊断：统计写入数据的峰值/非零比例——
             * 峰值正常却无声 → 驱动/HAL 层问题；峰值≈0 → 解码输出静音。 */
            const int16_t *p16 = out;
            int n = frames * sc->channels;
            int i, nz = 0, peak = 0;
            if (n > 4096) n = 4096;   /* 只看前 4096 样本，防慢 */
            for (i = 0; i < n; i++)
            {
                int v = p16[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
                if (v != 0) nz++;
            }
            dbg_peak = peak;
            dbg_nz = nz;
            dbg_n = n;
            /* 左右声道相关性 + 分声道 RMS（切歌无声排查 2026-08-07）：
             * 若解码输出 L/R 反相（corr≈-1），外部功放单端合并处信号抵消
             * → 无声。raw_peak 统计绝对值无法反映相位，此诊断直接算皮尔逊
             * 相关：corr = sumLR/(sqrt(sumL2)*sqrt(sumR2))，
             * +1 同相 / 0 无关 / -1 反相。L_rms/R_rms 为衰减后分声道能量。 */
            if (sc->channels == 2 && n >= 2)
            {
                int nf = n / 2;   /* 帧数 */
                int64_t sumL2 = 0, sumR2 = 0, sumLR = 0;
                double corr = 0.0;
                int L_rms = 0, R_rms = 0;
                for (i = 0; i < nf; i++)
                {
                    int64_t L = p16[2 * i];
                    int64_t R = p16[2 * i + 1];
                    sumL2 += L * L;
                    sumR2 += R * R;
                    sumLR += L * R;
                }
                if (nf > 0)
                {
                    L_rms = (int)sqrt((double)sumL2 / nf);
                    R_rms = (int)sqrt((double)sumR2 / nf);
                }
                if (sumL2 > 0 && sumR2 > 0)
                    corr = (double)sumLR /
                        (sqrt((double)sumL2) * sqrt((double)sumR2));
                dbg_L_rms = L_rms;
                dbg_R_rms = R_rms;
                dbg_corr = corr;
            }
        }

#if defined(DM_FORCE_MONO) && DM_FORCE_MONO
        /* 强制 mono 混音实验（2026-08-07 切歌无声排查）：L/R 平均 →
         * dual mono 后写声卡。验证"外部功放单端合并处反相抵消"假设——
         * 若 mono 后无声歌曲恢复声音 = 反相抵消坐实；仍无声 = 解码内容
         * 问题。诊断统计已在上方（mono 前）完成，不影响 corr 有效性。 */
        if (sc->channels == 2)
        {
            int i;
            int16_t *p = (int16_t *)out;
            for (i = 0; i < frames; i++)
            {
                int16_t l = p[2 * i];
                int16_t r = p[2 * i + 1];
                int16_t m = (int16_t)(((int)l + (int)r) / 2);
                p[2 * i] = m;
                p[2 * i + 1] = m;
            }
        }
#endif

        /* 单包间隔异常检测（2026-08-07 切歌无声排查）：writei 每次调用
         * 前记录 tick，若与上次调用间隔 >60ms（正常 40ms/包）说明 audioRender
         * 喂数被阻塞（线程抢占/解码慢/HPWORK 干扰）→ 打印一次。仅在异常时
         * 打印，防刷屏。
         * 2026-08-23 降噪（review-8）：链路已闭环，默认关闭，排障改 #if 1。 */
#if 0
        {
            static uint32_t last_write_tick = 0;
            uint32_t w_now = lv_tick_get();
            if (last_write_tick != 0)
            {
                uint32_t gap = w_now - last_write_tick;
                if (gap > 60)
                    printf("[music] WRITE_GAP %u ms (frames=%d)\n",
                           gap, (int)frames);
            }
            last_write_tick = w_now;
        }
#endif

        wr = (int)snd_vela_pcm_writei(sc->handle, out, frames);

        /* 2026-08-23 降噪（review-8）：逐包 writei 诊断打印已关闭（原每 1000
         * 包一条，P73~P113 排障用），排障改 #if 1。 */
#if 0
        if (do_print)
        {
            if (sc->channels == 2 && dbg_n >= 2)
                printf("[music] writei size=%d bits=%d ch=%d frames=%d wr=%d "
                       "state=%d raw_peak=%d raw_nz=%d raw_rms=%d "
                       "peak=%d nz=%d/%d L_rms=%d R_rms=%d corr=%.3f "
                       "delta_ms=%u avg_ms=%u\n",
                       size, sc->bits, sc->channels, (int)frames, wr,
                       (int)snd_vela_pcm_state(sc->handle),
                       g_dbg_raw_peak, g_dbg_raw_nz, g_dbg_raw_rms,
                       dbg_peak, dbg_nz, dbg_n,
                       dbg_L_rms, dbg_R_rms, dbg_corr,
                       delta_ms, delta_ms / 20);
            else
                printf("[music] writei size=%d bits=%d ch=%d frames=%d wr=%d "
                       "state=%d raw_peak=%d raw_nz=%d raw_rms=%d "
                       "peak=%d nz=%d/%d delta_ms=%u avg_ms=%u\n",
                       size, sc->bits, sc->channels, (int)frames, wr,
                       (int)snd_vela_pcm_state(sc->handle),
                       g_dbg_raw_peak, g_dbg_raw_nz, g_dbg_raw_rms,
                       dbg_peak, dbg_nz, dbg_n,
                       delta_ms, delta_ms / 20);
        }
#endif
        /* -EPIPE = underrun: the reference (sunxi_alsa xrun) recovers by
         * re-prepare so the substream restarts on the next write. Without
         * this, once the decoder is briefly slow the codec stays stuck. */
        if (wr == -EPIPE)
        {
            if ((++dbg_cnt % 20) == 1)
                printf("[music] underrun, re-prepare\n");
            snd_vela_pcm_prepare(sc->handle);
            return 0;
        }
        if (wr < 0)
        {
            /* 切歌无声排查（2026-08-07 证据点①）：writei 返回非 -EPIPE
             * 负值时不能静默——raw_peak 在衰减前统计，若切歌后旧流未释放
             * （EBUSY）或设备状态异常（-EBADFD 等），数据根本没进声卡，
             * 但 raw_peak 照样满幅。立即打印（错误稀少不会刷屏）。
             * 注：snd_vela_pcm_avail_update 在该 tiny-alsa 库中只有声明
             * 无实现（链接会挂），故只打印 snd_vela_pcm_state。 */
            printf("[music] writei ERR wr=%d state=%d\n",
                   wr, (int)snd_vela_pcm_state(sc->handle));
            return 0;
        }
        /* Return SOURCE bytes consumed (frames * source bytes-per-frame). */
        return (int)(wr * src_fb);
    }
}

static int dm_sound_reset(SoundCtrl *s)
{
    /* 切歌时 XPlayerReset → PlayerClear → handleReset 走这里。必须像
     * dm_sound_stop 一样真正 close 并清 handle：若只 snd_vela_pcm_reset
     * 而 handle 残留，新歌 dm_sound_open 会因 `if (sc->handle) return 0`
     * 跳过重新 open，继续用残留的 substream → 下一首无声 + aplay EBUSY。 */
    dm_sound_ctrl_t *sc = (dm_sound_ctrl_t *)s;
    if (sc->handle)
    {
        snd_vela_pcm_drain(sc->handle);
        snd_vela_pcm_close(sc->handle);
        sc->handle = NULL;
    }
    return 0;
}

static int dm_sound_get_cached_time(SoundCtrl *s) { (void)s; return 0; }
static int dm_sound_get_frame_count(SoundCtrl *s) { (void)s; return 0; }
static int dm_sound_set_playback_rate(SoundCtrl *s,
                                      const XAudioPlaybackRate *r)
{
    (void)s; (void)r; return 0;
}
static int dm_sound_control(SoundCtrl *s, int cmd, void *para)
{
    (void)s; (void)cmd; (void)para; return 0;
}

static SoundControlOpsT dm_sound_ops =
{
    dm_sound_destroy,
    dm_sound_set_format,
    dm_sound_start,
    dm_sound_stop,
    dm_sound_pause,
    dm_sound_write,
    dm_sound_reset,
    dm_sound_get_cached_time,
    dm_sound_get_frame_count,
    dm_sound_set_playback_rate,
    dm_sound_control,
};

static SoundCtrl *dm_sound_create(void)
{
    dm_sound_ctrl_t *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return NULL;
    sc->base.ops = &dm_sound_ops;
    sc->samplerate = 48000;
    sc->channels = 2;
    sc->bits = 16;
    return (SoundCtrl *)sc;
}

/* XPlayer notify callback — runs on XPlayer's internal thread, only flags. */
static int dm_xplayer_cb(void *user, int msg, int ext1, void *para)
{
    (void)user; (void)ext1; (void)para;
    switch (msg)
    {
    case AWPLAYER_MEDIA_PREPARED:
        g_xp_state = 1;
        break;
    case AWPLAYER_MEDIA_PLAYBACK_COMPLETE:
        g_xp_state = 3;
        break;
    case AWPLAYER_MEDIA_ERROR:
        g_xp_state = -1;
        break;
    default:
        break;
    }
    return 0;
}

static void music_audio_start(const char *path)
{
    printf("[music] xplayer play: %s\n", path ? path : "(null)");

    if (g_xplayer != NULL)
    {
        /* 切歌无声根因修复（2026-08-07 ok-20260807-17）：
         * 上板实证——XPlayerReset 复用路径（PlayerClear 销毁 audioRender 再
         * 重建）切歌后无声，而首次创建（XPlayerCreate + dm_sound_create）
         * 路径有声。改用 XPlayerDestroy 销毁后重新走 create 路径，使每次
         * 播放（含切歌）都经过已验证有声的初始化链。 */
        printf("[music] destroy & recreate player for new source\n");
        XPlayerDestroy(g_xplayer);
        g_xplayer = NULL;
        /* 切歌死机修复（2026-08-08）：XPlayerDestroy → PlayerDestroy →
         * PlayerClear 内部已调用 SoundDeviceDestroy(p->pAudioSink)（player.c
         * CONFIG_ONLY_DISABLE_AUDIO=0 生效）销毁 sink——pAudioSink 就是
         * XPlayerSetAudioSink 传入的 dm_sound。此处若再调
         * g_music_sound->ops->destroy() 会对同一对象二次 free → use-after-free
         * → 崩溃（堆地址 Undefined instruction，backtrace 0x4233xxxx）。
         * 因此只置 NULL，内存由 XPlayer 内部释放。 */
        g_music_sound = NULL;
    }

    if (g_xplayer == NULL)
    {
        g_xplayer = XPlayerCreate();
        if (g_xplayer == NULL)
        {
            printf("[music] ERROR: XPlayerCreate() failed\n");
            if (music_artist_lbl)   /* 子页已关闭时指针为空，后台重播仍会走到此 */
                lv_label_set_text(music_artist_lbl, "播放器初始化失败");
            return;
        }
        XPlayerSetNotifyCallback(g_xplayer, dm_xplayer_cb, NULL);
        XPlayerInitCheck(g_xplayer);
        g_music_player_cleared = 0;   /* 新播放器可正常 XPlayerStart */
        g_music_sound = dm_sound_create();
        XPlayerSetAudioSink(g_xplayer, (void *)g_music_sound);
        printf("[music] xplayer created\n");
    }

    g_xp_state = 0;
    if (XPlayerSetDataSourceUrl(g_xplayer, path, NULL, NULL) != 0)
    {
        printf("[music] setDataSourceUrl failed, reset player\n");
        XPlayerReset(g_xplayer);   /* 复位到 IDLE，允许下一次 setDataSource */
        g_xp_state = -1;           /* poll 自动跳到下一首 */
        if (music_artist_lbl)
            lv_label_set_text(music_artist_lbl, "播放出错");
        return;
    }
    XPlayerPrepareAsync(g_xplayer);
}

static void music_audio_pause(void)
{
    /* 防御（review-12）：player 已被 force_stop clear 时 pause 无意义
     * （STOPPED 态 XPlayerPause 返回 -1），跳过避免误操作。 */
    if (g_xplayer && !g_music_player_cleared) XPlayerPause(g_xplayer);
}

static void music_audio_resume(void)
{
    /* 2026-08-23 死机修复：XPlayerStop 后（STOPPED 态，PlayerClear 已清空）
     * XPlayerStart 会崩——禁止对已 clear 的 player 调 Start。 */
    if (g_xplayer && !g_music_player_cleared) XPlayerStart(g_xplayer);
}

/*
 * 强制停止播放（2026-08-08 改名自 music_audio_stop）。
 * 不用于页面导航（退出音乐页/返回 HOME 不停音乐——后台化设计）。
 * Reserved for:
 *   - user stop command
 *   - shutdown
 *   - power management / bluetooth disconnect
 */
void music_audio_force_stop(void)
{
    /* P103：先同步 UI 状态（music_playing=false + 三处播放按钮回 PLAY
     * 图标/蓝三角/去 CHECKED），再 XPlayerStop 真正停止并释放声卡——
     * 修复 AI 打断音乐（PTT 按下 force_stop）后三处按钮残留
     * CHECKED/PAUSE 的"状态不同步"问题。music_pause 前向声明见 L1168。 */
    music_pause();
    if (g_xplayer) {
        XPlayerStop(g_xplayer);        /* PlayerClear 清空媒体+关 sink → STOPPED 态 */
        g_music_player_cleared = 1;    /* 之后 XPlayerStart 会崩，恢复必须重建 */
    }
}

/* ── 本地提示音（2026-08-16 P102）：播 /resource/tones/<name>.wav ──
 * 后台线程直读 WAV → snd_vela_pcm，不经过 XPlayer（避免与音乐状态机纠缠）；
 * 声卡被音乐占用时 open 失败 → 跳过提示音（不打断音乐）。 */
static pthread_t g_tone_thread;
static volatile int g_tone_busy;
static volatile int g_audio_tone_done;   /* review-10：提示音播完置位（主线程 poll 消费恢复音乐） */

/* 播放单个提示音 wav：open → hw_params（按 wav 头）→ 写循环 → fade-out → drain → close。
 * 2026-09-01 P142：抽成独立函数，供 dm_tone_worker 顺序播放 intro/voice 两个文件
 * （格式可不同——背景音乐 32000/stereo vs 语音 24000/mono，各自独立 open）。
 * 返回 0=成功 / <0=失败（文件缺失/坏 wav/EBUSY/hw_params fail 均静默跳过）。 */
static int tone_play_one(const char *name)
{
    char path[96];
    uint8_t hdr[44];
    uint16_t channels, bits;
    uint32_t rate;
    snd_pcm_t *handle = NULL;
    FILE *fp;
    int ret;

    snprintf(path, sizeof(path), "/resource/tones/%s.wav", name);

    fp = fopen(path, "rb");
    if (!fp)
    {
        printf("[tone] open fail: %s\n", path);
        return -1;
    }
    if (fread(hdr, 1, 44, fp) != 44 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        fclose(fp);
        printf("[tone] bad wav: %s\n", path);
        return -1;
    }
    channels = hdr[22] | (hdr[23] << 8);
    rate     = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    bits     = hdr[34] | (hdr[35] << 8);
    (void)bits;   /* 合成规格固定 16bit，格式统一 S16_LE */

    ret = snd_vela_pcm_open(&handle, "hw:audiocodec",
                            SND_VELA_PCM_STREAM_PLAYBACK, 0);
    /* P111：EBUSY 短暂重试（PTT/TTS 等其他进程可能正在释放声卡） */
    for (int i = 0; ret == -16 && i < 5; i++)
    {
        usleep(200 * 1000);
        ret = snd_vela_pcm_open(&handle, "hw:audiocodec",
                                SND_VELA_PCM_STREAM_PLAYBACK, 0);
    }
    if (ret < 0)
    {
        printf("[tone] pcm busy %d (skip)\n", ret);
        fclose(fp);
        return -1;
    }

    /* hw params 与 dm_sound_open 同款（period 1024 / buffer 8192） */
    {
        snd_pcm_hw_params_t *params;
        snd_pcm_uframes_t period = 1024, buffer = 8192;
        snd_vela_pcm_hw_params_malloc(&params);
        snd_vela_pcm_hw_params_any(handle, params);
        snd_vela_pcm_hw_params_set_access(handle, params,
                                          SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_vela_pcm_hw_params_set_format(handle, params,
                                          SND_PCM_FORMAT_S16_LE);
        snd_vela_pcm_hw_params_set_channels(handle, params, channels);
        snd_vela_pcm_hw_params_set_rate(handle, params, rate, 0);
        snd_vela_pcm_hw_params_set_period_size(handle, params, period, 0);
        snd_vela_pcm_hw_params_set_buffer_size(handle, params, buffer);
        ret = snd_vela_pcm_hw_params(handle, params);
        snd_vela_pcm_hw_params_free(params);
        if (ret < 0)
        {
            printf("[tone] hw_params fail %d\n", ret);
            snd_vela_pcm_close(handle);
            fclose(fp);
            return -1;
        }
        /* 2026-09-11 尾音修复：补 sw_params（与 dm_sound_open / ai_agent
         * audio_playback_open 完全同款）。此前 tone_play_one 只设 hw_params，
         * 缺 silence_size/stop_threshold——健康提醒 WAV 短，文件 EOF 后 DMA
         * 欠载，缺 silence_size 时硬件不补静音而重播上一段 → 播完"多一声"
         * 尾音。音乐/TTS 路径都设了 sw_params 所以无此现象，唯健康提示音有。 */
        {
            snd_pcm_sw_params_t *sw;
            snd_pcm_uframes_t boundary = 0;
            snd_pcm_sw_params_alloca(&sw);
            snd_vela_pcm_sw_params_current(handle, sw);
            snd_vela_pcm_sw_params_get_boundary(sw, &boundary);
            snd_vela_pcm_sw_params_set_start_threshold(handle, sw, buffer);
            snd_vela_pcm_sw_params_set_stop_threshold(handle, sw, buffer);
            snd_vela_pcm_sw_params_set_silence_size(handle, sw, boundary);
            snd_vela_pcm_sw_params_set_avail_min(handle, sw, period);
            ret = snd_vela_pcm_sw_params(handle, sw);
            if (ret < 0)
            {
                printf("[tone] sw_params fail %d\n", ret);
                snd_vela_pcm_close(handle);
                fclose(fp);
                return -1;
            }
        }
        snd_vela_pcm_prepare(handle);
    }

    /* 循环写 PCM（小块缓冲，勿大栈数组——P87 教训） */
    {
        int16_t buf[512];
        size_t got;
        int16_t last = 0;   /* 2026-08-31 尾音修复：最后写入样本（fade-out 起点） */
        while ((got = fread(buf, 1, sizeof(buf), fp)) > 0)
        {
            int frames = got / (2 * channels);
            /* 2026-09-14：提示音/提醒语音跟随播放器音量（g_music_volume）。
             * 此前 tone_play_one 直写 WAV 满幅 → 提醒音/开场音比音乐响很多
             * （用户反馈"声音太大、要跟播放器同步"）。与 dm_sound_write /
             * ai_agent audio_playback_write 同曲线（gain=vol*32768/100）+ 软限幅。 */
            if (g_music_volume < 100)
            {
                int gain = (g_music_volume * 32768) / 100;
                int ns = (int)got / 2;
                for (int i = 0; i < ns; i++)
                {
                    int v = (buf[i] * gain) >> 15;
                    if (v > 30000)
                        v = 30000 + (v - 30000) / 4;
                    else if (v < -30000)
                        v = -30000 - (v + 30000) / 4;
                    buf[i] = (int16_t)v;
                }
            }
            if (frames > 0)
            {
                snd_pcm_sframes_t wr = snd_vela_pcm_writei(handle, buf, frames);
                /* 2026-09-11 尾音修复：旧代码不检查 writei 返回值——欠载
                 * (XRUN/EPIPE) 后剩余 PCM 被静默丢弃或错位，也是尾音来源。
                 * 与 audio_playback_write 同款：EPIPE 则 prepare 恢复后重写。 */
                if (wr == -EPIPE)
                {
                    snd_vela_pcm_prepare(handle);
                    wr = snd_vela_pcm_writei(handle, buf, frames);
                }
                if (wr > 0)
                    last = buf[(size_t)wr * channels - 1];
            }
        }
        /* 2026-08-31 尾音修复：drain 前补 fade-out（30ms 线性衰减到 0）。
         * snd_vela_pcm_close() 内部无条件 drop（硬切 SETUP），若 DAC 此刻仍
         * 输出非零电平会产生"多一个残缺尾音"（爆音）；先淡出到 0 再
         * drain/close，硬切发生在静音电平上。小块循环，勿大栈数组。 */
        {
            int fade_frames = rate * 30 / 1000;
            int done = 0;
            int16_t fade[512];
            while (done < fade_frames)
            {
                int n = fade_frames - done;
                int max_frames = (int)sizeof(fade) / (2 * channels);
                if (n > max_frames)
                    n = max_frames;
                for (int i = 0; i < n; i++)
                {
                    int32_t v = (int32_t)last * (fade_frames - done - i) / fade_frames;
                    for (int c = 0; c < channels; c++)
                        fade[i * channels + c] = (int16_t)v;
                }
                snd_vela_pcm_writei(handle, fade, n);
                done += n;
            }
        }
    }
    snd_vela_pcm_drain(handle);
    snd_vela_pcm_close(handle);
    fclose(fp);
    printf("[tone] done: %s\n", path);
    return 0;
}

static void *dm_tone_worker(void *arg)
{
    /* arg = "intro|voice"（dm_tone_play_seq）或 "name"（dm_tone_play）：
     * 先播开场背景音效（intro），再播语音提示（voice）——避免"突然有人
     * 说话"。两文件格式可不同，tone_play_one 各自独立 open/hw_params。 */
    char *copy = (char *)arg;
    char *intro = copy;
    char *voice = strchr(copy, '|');

    if (voice)
    {
        *voice = '\0';
        voice++;
    }
    if (intro[0])
        tone_play_one(intro);
    if (voice && voice[0])
        tone_play_one(voice);
    free(copy);

    g_tone_busy = 0;
    g_audio_tone_done = 1;   /* review-10：提示音播完，主线程 poll 恢复被挂起的音乐 */
    return NULL;
}

void dm_tone_play(const char *name)
{
    char *copy;

    if (!name || !name[0] || g_tone_busy)
        return;
    /* P111：语音优先——XPlayer 持有声卡（播放/暂停都未关 handle，
     * P107 实证 pause 不释放句柄）时先停音乐释放声卡，否则提示音
     * open 必 EBUSY 静默跳过，健康提醒完全无声（对齐 PTT 行为）。
     * 本函数由 LVGL 主线程调用（弹窗/欢迎语），操作音乐状态安全。
     * 2026-08-23 review-10：改走音频仲裁层 acquire，提示音播完自动恢复音乐。 */
    dm_audio_fg_acquire();
    copy = strdup(name);
    if (!copy)
    {
        dm_audio_fg_release();
        return;
    }
    g_tone_busy = 1;
    if (pthread_create(&g_tone_thread, NULL, dm_tone_worker, copy) != 0)
    {
        g_tone_busy = 0;
        free(copy);
        dm_audio_fg_release();
    }
    else
    {
        /* 一次性 worker：detach 后由内核回收 TCB/栈，避免每次提醒泄漏线程 */
        pthread_detach(g_tone_thread);
    }
}

/* 2026-09-01 P142：开场背景音效 + 语音提示 顺序播放。
 * 背景音乐（如 bg_reward_std）先响，语音（如 popup_water_1）随后——
 * 避免"突然有人说话"的惊吓感。两文件格式可不同，worker 内部各自
 * open/drain/close（tone_play_one）。由 LVGL 主线程调用，与 dm_tone_play
 * 同生命周期纪律（dm_audio_fg_acquire + g_tone_busy 防重入）。 */
void dm_tone_play_seq(const char *intro, const char *voice)
{
    char *copy;
    size_t ilen = intro ? strlen(intro) : 0;
    size_t vlen = voice ? strlen(voice) : 0;

    if ((!intro || !intro[0]) && (!voice || !voice[0]))
        return;
    if (g_tone_busy)
        return;
    dm_audio_fg_acquire();
    /* 拼 "intro|voice"：intro 缺省时留空段（worker 跳过空段） */
    copy = malloc(ilen + vlen + 2);
    if (!copy)
    {
        dm_audio_fg_release();
        return;
    }
    if (ilen)
        memcpy(copy, intro, ilen);
    copy[ilen] = '|';
    if (vlen)
        memcpy(copy + ilen + 1, voice, vlen);
    copy[ilen + vlen + 1] = '\0';
    g_tone_busy = 1;
    if (pthread_create(&g_tone_thread, NULL, dm_tone_worker, copy) != 0)
    {
        g_tone_busy = 0;
        free(copy);
        dm_audio_fg_release();
    }
    else
    {
        /* 一次性 worker：detach 后由内核回收 TCB/栈，避免每次提醒泄漏线程 */
        pthread_detach(g_tone_thread);
    }
}

/* Poll async XPlayer state from the LVGL timer thread. */
static void music_audio_poll(void)
{
    if (g_xplayer == NULL) return;

    if (g_xp_state == 1)          /* PREPARED → start playback */
    {
        g_xp_state = 2;
        if (!g_music_player_cleared)
            XPlayerStart(g_xplayer);
    }
    else if (g_xp_state == 3)     /* PLAYBACK_COMPLETE → auto next */
    {
        g_xp_state = 0;
        /* P145：文件浏览器点播的外来文件（不在曲库列表）播完即停——试听
         * 语义，与曲库连播逻辑解耦；曲库内歌曲仍自动连播下一首。 */
        int in_lib = 0;
        for (uint32_t i = 0; i < dm_track_count; i++)
            if (strcmp(dm_tracks[i].path, dm_now_path) == 0) { in_lib = 1; break; }
        if (!in_lib) {
            /* P145：试听播完即停，且复位播放会话——music_inited=false →
             * 下次按播放键走 music_play(music_track_id) 从曲库开始播放，
             * 而不是续播外来文件；music_track_id 恢复曲库记忆位置。 */
            music_pause();
            music_inited = false;
            music_track_id = music_state_load();
            if (music_track_id >= dm_track_count)
                music_track_id = 0;
            return;
        }
        music_album_next(true);
    }
    else if (g_xp_state == -1)    /* ERROR → skip to next */
    {
        g_xp_state = 0;
        printf("[music] xplayer error, skip\n");
        music_album_next(true);
    }
}

static int music_audio_position(int *ms)
{
    if (g_xplayer && !g_music_player_cleared) return XPlayerGetCurrentPosition(g_xplayer, ms);
    return -1;
}

static int music_audio_duration(int *ms)
{
    if (g_xplayer && !g_music_player_cleared) return XPlayerGetDuration(g_xplayer, ms);
    return -1;
}
#else
static void music_audio_start(const char *path) { (void)path; }
static void music_audio_pause(void) { }
static void music_audio_resume(void) { }
void music_audio_force_stop(void) { }
static void music_audio_poll(void) { }
static int music_audio_position(int *ms) { (void)ms; return -1; }
static int music_audio_duration(int *ms) { (void)ms; return -1; }
#endif

/* Phase 1 拆分：music_play 被 ui_music.c（播放/切歌回调）extern 调用 */
void music_play(uint32_t id)
{
    if (dm_track_count == 0)
    {
        /* HOME/锁屏首次点播放：歌曲列表扫描原只在播放器页打开时进行
         * （music_scan_tracks），这里补扫一次；并恢复上次播放的歌曲
         * （state 文件），无记录则默认第一首。 */
        music_scan_tracks();
        if (dm_track_count == 0)
            {
                /* 2026-09-10 P206：无曲目不再静默 return——调一次
                 * update 把"暂无曲目/请将音乐放入"刷到播放器页 +
                 * HOME/锁屏标题，按键有反馈，不再像死按钮。 */
                music_update_track_info();
                return;
            }
        music_track_id = music_state_load();
        if (music_track_id >= dm_track_count)
            music_track_id = 0;
        id = music_track_id;
    }
    if (id >= dm_track_count) id = 0;
    music_track_id = id;
    /* P145：同步当前播放状态（标题源）——列表重建后依然正确显示 */
    strncpy(dm_now_path, dm_tracks[id].path, sizeof(dm_now_path) - 1);
    dm_now_path[sizeof(dm_now_path) - 1] = '\0';
    strncpy(dm_now_title, dm_tracks[id].title, sizeof(dm_now_title) - 1);
    dm_now_title[sizeof(dm_now_title) - 1] = '\0';
    music_inited = true;
    music_state_save(music_track_id);
    music_update_track_info();
    music_audio_start(dm_now_path);
    music_resume();
}

/* P145：按路径播放（文件浏览器点播任意目录文件）——不污染曲库列表。
 * 文件恰在曲库中则同步 music_track_id（高亮/进度复用）；否则置哨兵
 * music_track_id = dm_track_count（当前播放不在曲库）。标题/锁屏显示
 * 一律走 dm_now_*，不受列表重建影响。 */
void music_play_path(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return;

    strncpy(dm_now_path, path, sizeof(dm_now_path) - 1);
    dm_now_path[sizeof(dm_now_path) - 1] = '\0';
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    const char *dot = strrchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    snprintf(dm_now_title, sizeof(dm_now_title), "%.*s", len, name);

    int found = -1;
    for (uint32_t i = 0; i < dm_track_count; i++)
        if (strcmp(dm_tracks[i].path, path) == 0) { found = (int)i; break; }
    music_track_id = (found >= 0) ? (uint32_t)found : dm_track_count;

    music_inited = true;
    /* P145：试听不覆盖曲库记忆（state 保留上次曲库播放位置），
     * 试听播完复位后播放键从曲库记忆位继续。 */
    music_update_track_info();
    music_audio_start(dm_now_path);
    music_resume();
}

/* Phase 1 拆分：music_resume 被 ui_music.c（播放键）extern 调用 */
void music_resume(void)
{
    music_playing = true;
    voice_director_on_music_changed(true);   /* Step 3：音乐状态 → Director */
    dm_pet_on_event(PET_EVT_MUSIC_ON);
    /* 音乐后台化（2026-08-08）：HOME 小组件 + standby 控件常驻，播放图标同步更新。
     * 2026-08-08 统一按钮：三处 play 按钮同款 CHECKED 蓝紫玻璃 + 白色符号。 */
    if (home_music_play_icon) {
        lv_obj_t *b = lv_obj_get_parent(home_music_play_icon);
        if (b) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_label_set_text(home_music_play_icon, LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(home_music_play_icon, lv_color_hex(0xFFFFFF), 0);
    }
    if (standby_music_play_icon) {
        lv_obj_t *b = lv_obj_get_parent(standby_music_play_icon);
        if (b) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_label_set_text(standby_music_play_icon, LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(standby_music_play_icon, lv_color_hex(0xFFFFFF), 0);
    }
    /* 自动连播在子页面关闭后仍会走到这里，
     * UI 对象可能为 NULL（close_subpage 已置空）——判空保护。 */
    if (music_play_btn)
        lv_obj_add_state(music_play_btn, LV_STATE_CHECKED);
    if (music_play_icon) {
        lv_label_set_text(music_play_icon, LV_SYMBOL_PAUSE);
        /* Icon is a child label: LV_STATE_CHECKED belongs to the button, not the
         * icon, so a CHECKED style on the icon never applies. Switch its color
         * here explicitly — white pause symbol on the blue gradient fill. */
        lv_obj_set_style_text_color(music_play_icon, lv_color_hex(0xFFFFFF), 0);
    }
    if (g_music_timer) lv_timer_resume(g_music_timer);
    /* 2026-08-23 死机修复：XPlayerStop（force_stop）→ PlayerClear 清空播放器后
     * 处于 STOPPED 态，XPlayerStart 会崩（上板实证 Prefetch abort）——恢复必须
     * 重建播放器（music_audio_start 内部 destroy 旧 player 重建），不能续播。
     * P145：按 dm_now_path 重建（文件浏览器点播的曲目可能不在曲库列表）。 */
    if (g_music_player_cleared && music_inited && dm_now_path[0])
        music_audio_start(dm_now_path);
    else
        music_audio_resume();
}

/* Phase 1 拆分：music_pause 被 ui_music.c（播放键）extern 调用 */
void music_pause(void)
{
    music_playing = false;
    voice_director_on_music_changed(false);   /* Step 3：音乐状态 → Director */
    dm_pet_on_event(PET_EVT_MUSIC_OFF);
    /* 音乐后台化（2026-08-08）：HOME 小组件 + standby 控件常驻，播放图标同步更新。
     * 2026-08-08 统一按钮：三处 play 按钮同款 CHECKED 蓝紫玻璃 + 白色符号。 */
    if (home_music_play_icon) {
        lv_obj_t *b = lv_obj_get_parent(home_music_play_icon);
        if (b) lv_obj_remove_state(b, LV_STATE_CHECKED);
        lv_label_set_text(home_music_play_icon, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(home_music_play_icon, lv_color_hex(COL_BLUE), 0);
    }
    if (standby_music_play_icon) {
        lv_obj_t *b = lv_obj_get_parent(standby_music_play_icon);
        if (b) lv_obj_remove_state(b, LV_STATE_CHECKED);
        lv_label_set_text(standby_music_play_icon, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(standby_music_play_icon, lv_color_hex(COL_BLUE), 0);
    }
    if (music_play_btn)
        lv_obj_remove_state(music_play_btn, LV_STATE_CHECKED);
    if (music_play_icon) {
        lv_label_set_text(music_play_icon, LV_SYMBOL_PLAY);
        /* Restore blue triangle on the light button. */
        lv_obj_set_style_text_color(music_play_icon, lv_color_hex(COL_BLUE), 0);
    }
    if (g_music_timer) lv_timer_pause(g_music_timer);
    music_audio_pause();
}

/* Phase 1 拆分：music_album_next 被 ui_music.c（prev/next 键）extern 调用 */
void music_album_next(bool next)
{
    if (dm_track_count == 0)
        music_scan_tracks();   /* HOME/锁屏 ⏮/⏭ 首次点击同样补扫 */
    if (dm_track_count == 0) return;
    /* P145：按当前播放文件定位基准（不在曲库 base=-1 → 切歌从列表头/尾
     * 接续；base=0 表示曲库第一首，避免哨兵导致跳过第一首）。 */
    int base = -1;
    for (uint32_t i = 0; i < dm_track_count; i++)
        if (strcmp(dm_tracks[i].path, dm_now_path) == 0) { base = (int)i; break; }
    if (next) {
        music_track_id = (base < 0) ? 0 : (base + 1) % dm_track_count;
    } else {
        music_track_id = (base <= 0) ? dm_track_count - 1 : (uint32_t)(base - 1);
    }
    music_play(music_track_id);
}

/* ── 2026-08-23 review-10 音频仲裁层（治本）────────────────────────
 * 统一"停音乐→释放声卡→播 TTS/提示音→恢复音乐"，根治 P103/P107/
 * review-5 反复发作的抢声卡（EBUSY）问题。
 *
 * 模型：前台音频（agent TTS 播报 / 本地提示音）需要声卡时调
 * dm_audio_fg_acquire() 挂起音乐（播放中才记录待恢复 + force_stop
 * 释放声卡）；播完由 dm_audio_fg_poll()（300ms LVGL 主线程）消费完成
 * 信号（提示音 worker 置 g_audio_tone_done / agent "done" 事件经
 * dm_ai_voice_evt_done_poll）调 dm_audio_fg_release() 恢复。
 *
 * 关键事实（P107/review-5 实证）：
 *   - XPlayerPause 只 snd_vela_pcm_pause 不关 handle，暂停仍占声卡；
 *   - XPlayerStop → PlayerClear 清空媒体+关 sink 才真正释放声卡，但
 *     此后处于 STOPPED 态，XPlayerStart 无法续播 → 恢复必须整曲重播
 *     （music_play 内部 destroy 旧 player 重建）。
 *   - 挂起超时（无完成信号，如 PTT 空文本未触发 speak）放弃恢复，
 *     避免音乐在很久后突然复活。 */
#define DM_AUDIO_FG_RESUME_WINDOW_MS  30000   /* 挂起后 30s 内完成才恢复 */

static int      g_audio_music_resume;      /* 1 = 音乐被前台音频挂起待恢复 */
static uint32_t g_audio_suspend_ms;        /* 挂起时刻（lv_tick） */

void dm_audio_fg_acquire(void)
{
    /* 播放中才记录待恢复；暂停的音乐保持停止（用户意图）。 */
    if (music_playing) {
        g_audio_music_resume = 1;
        g_audio_suspend_ms = lv_tick_get();
    }
    /* 播放/暂停都持有声卡（P107），统一 force_stop 释放；无音乐时
     * music_pause() 判空无害，XPlayerStop 有 g_xplayer 判空。 */
    music_audio_force_stop();
}

void dm_audio_fg_release(void)
{
    if (!g_audio_music_resume)
        return;
    g_audio_music_resume = 0;
    /* 超时放弃恢复（无完成信号场景），避免音乐迟到复活 */
    if ((uint32_t)(lv_tick_get() - g_audio_suspend_ms) > DM_AUDIO_FG_RESUME_WINDOW_MS)
        return;
    if (music_inited && dm_track_count > 0 && !music_playing)
        music_play(music_track_id);
}

void dm_audio_fg_poll(void)
{
    int done = 0;
    if (g_audio_tone_done) {
        g_audio_tone_done = 0;
        done = 1;
    }
    if (dm_ai_voice_evt_done_poll())
        done = 1;
    if (done)
        dm_audio_fg_release();
}

/* Phase 1 拆分：music_update_track_info 被 ui_music.c（ui_music_create 初始化）extern 调用 */
void music_update_track_info(void)
{
    /* P145：标题一律取当前播放状态（dm_now）——文件浏览器点播的文件可能
     * 不在曲库列表中，扫描重建 dm_tracks 后索引失效，不能再从列表取标题。 */
    const char *now_title = dm_now_path[0] ? dm_now_title : NULL;

    /* 音乐后台化（2026-08-08）：HOME 小组件 + standby 控件常驻，歌名在
     * 任何状态都更新（后台播放时播放器页 UI 为 NULL，但两者仍在）。 */
    if (home_music_title_lbl) {
        const char *title = now_title ? now_title
            : ((music_track_id < dm_track_count) ? dm_tracks[music_track_id].title : "暂无曲目");
        lv_label_set_text(home_music_title_lbl, title);
    }
    if (standby_music_title_lbl) {
        const char *title = now_title ? now_title
            : ((music_track_id < dm_track_count) ? dm_tracks[music_track_id].title : "");
        lv_label_set_text(standby_music_title_lbl, title);
    }
    /* 播放器页 UI：子页面关闭后 music_title_lbl 为 NULL
     * （close_subpage 置空），自动连播（COMPLETE→next）在后台仍会调
     * 用本函数——直接返回，只改播放状态不刷 UI。 */
    if (music_title_lbl == NULL)
        return;
    if (now_title == NULL && music_track_id >= dm_track_count)
    {
        lv_label_set_text(music_title_lbl, "暂无曲目");
        lv_label_set_text(music_artist_lbl, "请将音乐放入 " DM_MUSIC_DIR);
        lv_slider_set_value(music_slider, 0, LV_ANIM_OFF);
        lv_slider_set_range(music_slider, 0, 0);
        lv_label_set_text(music_time_lbl, "0:00");
        if (music_time_total_lbl) lv_label_set_text(music_time_total_lbl, "0:00");
        return;
    }

    /* P145：时长取当前播放曲目（恰在曲库才有；文件浏览器点播的未知 → 0） */
    uint32_t len = 0;
    if (music_track_id < dm_track_count && dm_now_path[0] &&
        strcmp(dm_tracks[music_track_id].path, dm_now_path) == 0)
        len = dm_tracks[music_track_id].length;

    lv_label_set_text(music_title_lbl, now_title ? now_title : dm_tracks[music_track_id].title);
    lv_label_set_text(music_artist_lbl, "本地音乐");
    lv_slider_set_value(music_slider, 0, LV_ANIM_OFF);
    lv_slider_set_range(music_slider, 0, len);
    lv_label_set_text(music_time_lbl, "0:00");

    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%d:%02d", (int)(len / 60), (int)(len % 60));
    if (music_time_total_lbl) lv_label_set_text(music_time_total_lbl, tbuf);

    /* P145：高亮当前播放行——按路径定位（不在曲库则不高亮） */
    if (music_list_cont && dm_now_path[0]) {
        int hi = -1;
        for (uint32_t i = 0; i < dm_track_count; i++)
            if (strcmp(dm_tracks[i].path, dm_now_path) == 0) { hi = (int)i; break; }
        if (hi >= 0) {
            lv_obj_t *row = lv_obj_get_child(music_list_cont, hi);
            if (row) {
                if (music_active_btn && music_active_btn != row) {
                    lv_obj_t *pl = lv_obj_get_child(music_active_btn, 0);
                    if (pl) lv_obj_set_style_text_color(pl, lv_color_hex(COL_TEXT), 0);
                }
                lv_obj_t *lbl = lv_obj_get_child(row, 0);
                if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(COL_BLUE), 0);
                music_active_btn = row;
            }
        }
    }
}

/* Phase 1 拆分：create_music_playlist_overlay / music_playlist_toggle_cb /
 * music_playlist_toggle / create_music_subpage（→ ui_music_create）
 * 已整体搬至 ui/ui_music.c */

/* 2026-09-10 P206：游戏子页整段删除——4 卡零玩法，模拟器（超级玛丽级）
 * 工作量超 9.20 窗口；Dock 入口同步下掉（ui_home.c），不留死按钮。
 * show_subpage "Games" 分支同步删除，误调走"敬请期待…"兜底。 */

/* ================================================================
 * BOOKS SUBPAGE — reading center
 * ================================================================ */

/* ================================================================
 * BOOKS 子 APP — Phase 1 拆分：已整体搬至 ui/ui_books.c
 * （书架扫描/阅读器/进度/护眼持久化，ui_books_create + book_reader_close）
 * ================================================================ */

/* Phase 1 拆分：FILES 子页已整体搬至 ui/ui_files.c
 * （files_* static 状态 / files_ui_clear_ptrs / ui_files_create） */

/* ================================================================
 * AI SUBPAGE — AI assistant center
 * ================================================================ */

static void create_ai_subpage(lv_obj_t *parent)
{
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());

    /* 2026-08-15 P85 重构（用户要求，deskmate-ui skill）：
     * ①去掉 6 个快捷卡；②布局改「按钮上方玻璃对话框（对话文字区）
     *  + 按钮居中 + aibg PNG 下移到屏幕中下方」。
     * 全部子元素用 lv_obj_align 绝对定位（不再用 flex column）。 */
    (void)sw;

    /* 2026-09-10：每次进页清空对话（缓冲是文件域常驻，label 是新建，
     * 不清会造成“空屏但缓冲有旧文，下次追加时旧文突然冒出”）。 */
    ai_chat_buf[0] = '\0';
    /* 2026-09-10 P206 rev：关页后才完成的上一轮问答（g_ai_done=1 残留）
     * 会在下次进页首个 poll tick 被当新回复显示——旧回复串页。
     * 进页即消费丢弃，只清标志位。 */
    {
        static char drain[AI_RESP_MAX];
        dm_ai_poll(drain, sizeof(drain));
    }

    /* ── AI 品牌横幅：aibg.png（560×154 预缩放 840×231）——下移到中下方
     * 2026-08-15 P86 修复：align 延迟到函数末尾统一执行（P85 在对象树
     * 构建中途调用 lv_obj_align 触发整树布局 → children 野指针崩溃）。 */
    lv_obj_t *bg = lv_image_create(parent);
    lv_image_set_src(bg, &dm_ai_bg);

    /* ── 语音对讲圆圈（按住说话）：唯一 Hero 元素，屏幕居中 ──
     * 2026-08-12 二次调优（用户反馈）：玻璃半透明蓝紫风格——白玻璃底
     * （COL_CARD→COL_PURPLE 渐变 @LV_OPA_60）+ 半透明白边 + 蓝紫外圈
     * 阴影（0x6C5CE7）+ 蓝色麦克风符号；触摸目标 DM(46)=110px
     * （skill ≥110px），符号 FONT_ICON(36px) ≈ 33% 圆径。
     * 2026-08-15 P85：按钮绝对定位屏幕正中（LV_ALIGN_CENTER）。 */
    ai_voice_btn = lv_btn_create(parent);
    lv_obj_set_size(ai_voice_btn, DM(46), DM(46));
    lv_obj_set_style_radius(ai_voice_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ai_voice_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_grad_color(ai_voice_btn, lv_color_hex(COL_PURPLE), 0);
    lv_obj_set_style_bg_grad_dir(ai_voice_btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ai_voice_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_color(ai_voice_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(ai_voice_btn, 3, 0);
    lv_obj_set_style_border_opa(ai_voice_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(ai_voice_btn, 16, 0);
    lv_obj_set_style_shadow_color(ai_voice_btn, lv_color_hex(0x6C5CE7), 0);
    lv_obj_set_style_shadow_opa(ai_voice_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_spread(ai_voice_btn, 4, 0);
    lv_obj_add_flag(ai_voice_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ai_voice_btn, dm_ai_voice_press_cb, LV_EVENT_PRESSED,
                        NULL);
    lv_obj_add_event_cb(ai_voice_btn, dm_ai_voice_release_cb, LV_EVENT_RELEASED,
                        NULL);
    /* P86：align 延迟到函数末尾统一执行 */

    lv_obj_t *mic_icon = lv_label_create(ai_voice_btn);
    lv_label_set_text(mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic_icon, FONT_ICON, 0);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(mic_icon);

    /* 语音状态：Ready/Listening/Thinking/Speaking（ai_poll_cb /
     * ai_voice_*_cb 更新）——圆圈下方小字，语音反馈的唯一载体。
     * 2026-08-12：初始值按网络状态显示（未连 WiFi 直接提示联网）
     * 2026-08-15 P85：定位在按钮正下方（按钮底部 + 间距）。 */
    ai_status_lbl = lv_label_create(parent);
    lv_label_set_text(ai_status_lbl,
        dm_net_wifi_connected() ? "按住说话" : "未连接 Wi-Fi，请先联网");
    lv_obj_set_style_text_font(ai_status_lbl, FONT_LABEL, 0);
    lv_obj_set_style_text_color(ai_status_lbl, lv_color_hex(COL_SEC), 0);
    /* P86：align 延迟到函数末尾统一执行 */

    /* ── 2026-08-15 P84/P85：对话文字区（小智式"说话显字/回答显字"）──
     * 玻璃对话框（Liquid Glass 配方：半透明白底 + 半透明白边 + 柔和阴影），
     * 绝对定位在按钮上方；可滚动容器 + 多行 label，ai_poll_cb 消费
     * stt/llm 事件追加显示，随对话增长自动滚动到底部。
     * 2026-08-15 P86 修复：①宽度改固定像素（lv_pct 嵌套在未布局容器
     * 内读取野 children → 崩溃）；②align 延迟到函数末尾统一执行。 */
    lv_obj_t *chat_cont = lv_obj_create(parent);
    lv_obj_set_width(chat_cont, 1920 * 70 / 100);
    /* 2026-09-10 快问行：高度 DM(210)→DM(180)，腾出底部一行放 4 快问
     * chips（顶 180 底 612，chips 中心 648，按钮顶部 689 不碰）。 */
    lv_obj_set_height(chat_cont, DM(180));
    lv_obj_set_style_radius(chat_cont, RAD_CARD, 0);
    lv_obj_set_style_bg_color(chat_cont, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(chat_cont, LV_OPA_60, 0);
    lv_obj_set_style_border_color(chat_cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(chat_cont, 3, 0);
    lv_obj_set_style_border_opa(chat_cont, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(chat_cont, 20, 0);
    lv_obj_set_style_shadow_color(chat_cont, lv_color_hex(0x6C5CE7), 0);
    lv_obj_set_style_shadow_opa(chat_cont, LV_OPA_20, 0);
    lv_obj_set_style_shadow_spread(chat_cont, 4, 0);
    lv_obj_set_style_pad_all(chat_cont, 12, 0);
    lv_obj_set_scrollbar_mode(chat_cont, LV_SCROLLBAR_MODE_AUTO);
    /* P86：align 延迟到函数末尾统一执行 */

    ai_chat_lbl = lv_label_create(chat_cont);
    lv_label_set_long_mode(ai_chat_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ai_chat_lbl, lv_pct(100));
    lv_obj_set_style_text_font(ai_chat_lbl, FONT_LABEL, 0);
    lv_obj_set_style_text_color(ai_chat_lbl, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ai_chat_lbl, "");

    /* 2026-08-15 P85：6 个快捷卡已按用户要求移除（ai_action_cb 同步删除）——
     * 布局简化为「玻璃对话框（按钮上方）+ 语音按钮（居中）+ aibg（中下方）」。
     * 2026-09-10 快问 chips（4 个，键盘无拼音的替代方案）：对话框与按钮
     * 之间一行，点按即问。问句选 agent fast-path 短路项（时间/天气本地
     * tool，音乐搜播，笑话走 LLM），无麦/无打字也可演示 AI 闭环。 */
    static const char *ai_chip_labels[4] = {
        "现在几点", "今天天气", "播放音乐", "讲个笑话",
    };
    static const char *ai_chip_questions[4] = {
        "现在几点了",
        "今天天气怎么样",
        "播放音乐",
        "讲个笑话",
    };
    lv_obj_t *ai_chips[4];
    int i;
    for (i = 0; i < 4; i++)
        {
            ai_chips[i] = lv_btn_create(parent);
            /* 行宽 70%(1344)：4×324 + 3×16 间隙 */
            lv_obj_set_size(ai_chips[i], 324, DM(22));
            lv_obj_set_style_radius(ai_chips[i], 16, 0);
            lv_obj_set_style_bg_color(ai_chips[i], lv_color_hex(COL_CARD), 0);
            lv_obj_set_style_bg_opa(ai_chips[i], LV_OPA_60, 0);
            lv_obj_set_style_border_color(ai_chips[i],
                lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(ai_chips[i], 2, 0);
            lv_obj_set_style_border_opa(ai_chips[i], LV_OPA_50, 0);
            lv_obj_add_event_cb(ai_chips[i], ai_chip_click_cb,
                LV_EVENT_CLICKED, (void *)ai_chip_questions[i]);
            {
                lv_obj_t *chip_lbl = lv_label_create(ai_chips[i]);
                lv_label_set_text(chip_lbl, ai_chip_labels[i]);
                lv_obj_set_style_text_font(chip_lbl, FONT_LABEL, 0);
                lv_obj_set_style_text_color(chip_lbl,
                    lv_color_hex(COL_BLUE), 0);
                lv_obj_center(chip_lbl);
            }
        }

    /* ── 2026-08-15 P86 修复：全部定位延迟到此统一执行 ──
     * P85 在对象树构建中途逐个 lv_obj_align（其内部第一行就调用
     * lv_obj_update_layout → 递归整树 layout_update_core），此时 chat_cont
     * 等尚未完全构建/宽度未解析，children 数组读到野指针（Data Abort
     * 415fb678 = lv_obj_get_child_count，DFAR 00e3a018）。
     * 统一在子对象全部创建完成后定位：布局只跑一次且对象树完整。 */
    /* 2026-08-15 P88/P89/P90/P91：布局——按钮不动(744)，Hold to talk 再下移
     * 一个身位（DM(96)→DM(114)，中心 830→874，PNG 透明可压入图上区域），
     * 对话框高 DM(210)（顶 144 底 648）。
     * 自上而下：对话框(396) → 按钮(744) → Hold to talk(874) → PNG(972)。 */
    /* 2026-09-10：快问行定位（P86 教训：全部定位延迟到末尾统一执行，
     * 构建中途不对齐）。4 chips 横向居中：总宽 4×324+3×16=1344，
     * 中心 648（对话框底 612 / 按钮顶 689 之间）。 */
    lv_obj_align(ai_chips[0], LV_ALIGN_CENTER, -((324 + 16) * 3 + 324) / 2 + 324 / 2, DM(20));
    lv_obj_align(ai_chips[1], LV_ALIGN_CENTER, -((324 + 16) * 3 + 324) / 2 + 324 / 2 + (324 + 16), DM(20));
    lv_obj_align(ai_chips[2], LV_ALIGN_CENTER, -((324 + 16) * 3 + 324) / 2 + 324 / 2 + (324 + 16) * 2, DM(20));
    lv_obj_align(ai_chips[3], LV_ALIGN_CENTER, -((324 + 16) * 3 + 324) / 2 + 324 / 2 + (324 + 16) * 3, DM(20));
    lv_obj_align(bg, LV_ALIGN_CENTER, 0, DM(155));          /* aibg：底部，中心 972 */
    lv_obj_align(ai_voice_btn, LV_ALIGN_CENTER, 0, DM(60)); /* 按钮：中心 744（不动） */
    lv_obj_align(ai_status_lbl, LV_ALIGN_CENTER, 0, DM(114));/* Hold to talk：中心 874 压入 PNG */
    lv_obj_align(chat_cont, LV_ALIGN_CENTER, 0, -DM(85));   /* 玻璃对话框：中心 396，扩大 */

    /* 轮询 timer（close_subpage 删除）——2026-08-12：文字输入/回复区/
     * 虚拟键盘已按需求移除，timer 仅驱动语音状态反馈（ai_poll_cb） */
    g_ai_timer = lv_timer_create(ai_poll_cb, 300, NULL);
    lv_timer_set_repeat_count(g_ai_timer, -1);

    /* 2026-08-15 P83：放弃云端唤醒词（P80 唤醒监听每 ~2s 一次云端 ASR
     * 会话，资源占用大且识别空文本不触发），改回纯按钮 PTT——
     * 只有按住 AI 圆圈才录音/对话，不按不录。不再自动发 wake_start。
     * 2026-08-23 review-7：语音事件订阅已改系统级常驻（deskmate_ui_create
     * 调 dm_ai_voice_evt_subscribe），此处幂等调用仅确保子页内一定已订阅；
     * 退出子页不再停订阅（见 close_subpage）。 */
    dm_ai_voice_evt_subscribe();
}

/* Phase 1 拆分：home 区（create_status_bar / create_card / 天气 glyph /
 * create_weather_card / weather_update_cb / create_ai_card /
 * create_home_music_widget / create_app_dock / standby / create_deskmate_screen）
 * 已整体搬至 ui/ui_home.c（ui_home_create），此处分段删除残留。
 * P145：原 hero 大时钟（create_big_clock）已移除，时间/周几日期并入
 * 状态栏两行容器（create_status_bar_ex 内），对时功能迁移至时钟容器。 */

/* Phase 1 拆分：create_weather_card / weather_update_cb / create_ai_card /
 * create_home_music_widget / create_app_dock / standby / create_deskmate_screen
 * 已整体搬至 ui/ui_home.c（ui_home_create），此处分段删除残留 */

/* ================================================================
 * HOME MUSIC WIDGET — 首页常驻音乐小组件（2026-08-08 后台化）
 * 显示当前歌曲名 + 播放状态 + 控制按钮（上一曲/播放暂停/下一曲）。
 * 点击小组件进入完整播放器页面。music_playing/music_resume/
 * music_pause/music_album_next 驱动真实播放，与播放器页共享状态。
 * ================================================================ */

/* Phase 1 拆分：home_music_open_cb / create_home_music_widget /
 * create_app_dock 已整体搬至 ui/ui_home.c（ui_home_create） */

/* ================================================================
/* Phase 1 拆分：STANDBY 区（idle_timer_cb / wake_from_standby /
 * standby_click_cb / arc_opa_anim_cb / show_standby）已整体搬至
 * ui/ui_home.c（ui_home_create），此处分段删除残留 */

/* ================================================================
 * MAIN SCREEN — full flex layout
 *
 * Structure:
 *   screen (COLUMN flex)
 *   ├── header (48px, flex row: clock | weather | icons)
 *   ├── content (flex_grow=1, COLUMN flex)
 *   │   ├── top_spacer (24px)
 *   │   ├── clock_section (auto)
 *   │   ├── gap (32px)
 *   │   ├── cards_row (ROW flex: weather | ai)
 *   │   └── bottom_spacer (flex_grow=1)
 *   └── dock (100px, centered)
 * ================================================================ */

/* Phase 1 拆分：create_deskmate_screen 已整体搬至 ui/ui_home.c
 * （改名 ui_home_create，主界面构建入口） */

/* ================================================================
 * PUBLIC ENTRY — called from luncher_dm.c after lv_nuttx init
 * Phase 1 拆分：主界面构建已搬至 ui/ui_home.c（ui_home_create）
 * ================================================================ */

void deskmate_ui_create(void)
{
    ui_home_create();
    /* 2026-08-23 常驻化（review-7）：语音事件订阅 + cmd 消费改为系统级常驻，
     * 不随 AI 子页启停——修复锁屏/主界面按语音按钮说话（standby_ai_btn 复用
     * PTT）时 cmd 事件无人消费（AI 子页未开 → ai_poll_cb 不存在）导致
     * "播放音乐"等语音命令不生效。dm_ai_voice_evt_subscribe 幂等防重入。 */
    dm_ai_voice_evt_subscribe();
    lv_timer_t *cmd_timer = lv_timer_create(dm_ai_cmd_poll_cb, 300, NULL);
    if (cmd_timer)
        lv_timer_set_repeat_count(cmd_timer, -1);
}