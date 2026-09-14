/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/deskmate_ui.h
 * Desktop Mate UI for luncher_dm (ported from lv_port_linux simulator)
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

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DESKMATE_UI_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DESKMATE_UI_H

#include <lvgl/lvgl.h>
#include "dm_weather.h"   /* dm_weather_t / DM_WEATHER_DAYS（天气卡 extern 需要） */

/****************************************************************************
 * Fonts — MiSans freetype, owned and loaded by luncher_dm.c (init_fonts)
 ****************************************************************************/

extern lv_font_t *dm_font_title;    /* big clock / hero numbers */
extern lv_font_t *dm_font_clock;    /* P151: status-bar time 专用档 56px */
extern lv_font_t *dm_font_symbol;   /* dock app glyphs (×1.5 of icon) */
extern lv_font_t *dm_font_icon;     /* status-bar time, status icons */
extern lv_font_t *dm_font_body;     /* card titles / labels */
extern lv_font_t *dm_font_label;    /* dock labels / secondary text */
extern lv_font_t *dm_font_caption;  /* helper text / captions */

/****************************************************************************
 * Shared design macros (Phase 1 拆分：从 deskmate_ui.c 迁入，供 ui_*.c 共用)
 ****************************************************************************/

#define DM_SCALE        2.4f
#define TOP_BAR_H       132     /* status bar height — fits 110px title clock */
#define CARD_GAP        48      /* gap between cards */
#define DOCK_ICON_SIZE  154     /* dock icon button size (OK, keep) */
#define DOCK_ICON_GAP   67      /* dock icon spacing */
#define DOCK_H          240     /* dock bar height (labels enlarged) */
#define EDGE_PAD        96      /* edge padding */
#define SETTING_ROW_H   125     /* settings row height */

/* ── 版本号（Settings → ABOUT 显示；发版时同步更新）──
 * DM_VERSION     = 用户可见版本号（与 /data/vela/releases/ 目录一致）
 * DM_BUILD_TAG   = 固化 git tag（git_snapshot.sh 自动生成 ok-YYYYMMDD-N）
 * 维护规则：AI 发版时（用户指定新版本号后）同步改这里 + AGENTS.md「三」版本行 */
#define DM_VERSION      "V1.0.0-20260911"
#define DM_BUILD_TAG    "ok-20260911-21"

/* Unified corner radius (see deskmate_ui.c design notes) */
#define RAD_CARD    40

/* Scale a simulator pixel size to board 1920x1200 */
#define DM(x) ((lv_coord_t)((x) * DM_SCALE))

/* 6-tier font system */
#define FONT_TITLE   (dm_font_title)
#define FONT_CLOCK   (dm_font_clock)   /* P151：状态栏时间专用档（56px），不动全局 title 档 */
#define FONT_SYMBOL  (dm_font_symbol)
#define FONT_ICON    (dm_font_icon)
#define FONT_BODY    (dm_font_body)
#define FONT_LABEL   (dm_font_label)
#define FONT_CAPTION (dm_font_caption)

/* Apple-style light theme palette */
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

/* Music 常量（Phase 1 拆分：ui_music.c 使用，deskmate_ui.c 播放核心也引用） */
#define DM_MAX_TRACKS   64
#define DM_MUSIC_DIR    "/sdcard/music"
#define DM_MUSIC_DIR_NORFLASH "/data/music"
#define DM_MUSIC_STATE_FILE "/data/dm_music.state"

/* Books 常量（Phase 1 拆分：ui_books.c 使用，deskmate_ui.c files_open_file 也引用） */
#define DM_MAX_BOOKS   32
#define DM_BOOK_DIR1   "/data/book"           /* 内置公版书 */
#define DM_BOOK_DIR2   "/sdcard/book"         /* TF卡（短路径兼容） */
#define DM_BOOK_DIR3   "/mnt/sdcard/book"     /* TF卡（标准挂载点） */

/****************************************************************************
 * Shared helpers (defined in deskmate_ui.c, Phase 1 跨页 extern 集中区)
 ****************************************************************************/

lv_obj_t *make_clean_cont(lv_obj_t *parent);
lv_obj_t *subpage_big_title(lv_obj_t *parent, const char *text);
void dm_net_status_refresh(void);

/* 2026-09-14 弹窗工具箱（Liquid Glass 统一弹窗语言，三类弹窗共用） */
lv_obj_t *dm_popup_card(lv_obj_t *parent, lv_coord_t w);       /* 玻璃卡容器（居中） */
lv_obj_t *dm_popup_icon(lv_obj_t *parent, const char *sym, uint32_t color); /* 图标块→返回 label */
void dm_popup_icon_set(lv_obj_t *ic, const char *sym, uint32_t color); /* 换肤：字形+字色+底盘同色 */
lv_obj_t *dm_popup_title(lv_obj_t *parent, const char *text);  /* 标题（返回 label） */
lv_obj_t *dm_popup_text(lv_obj_t *parent, const char *text);   /* 说明（居中） */
lv_obj_t *dm_popup_btns(lv_obj_t *parent);                     /* 按钮行 */
lv_obj_t *dm_popup_btn(lv_obj_t *parent, const char *txt, int kind, lv_event_cb_t cb);
                                                                /* 胶囊按钮 0主蓝/1次灰/2危险红 */
void dm_popup_fit(lv_obj_t *card, lv_obj_t *btnrow); /* 自适应补高：实测按钮行溢出则撑足卡片（show后调） */
void dm_utf8_trim(char *s);   /* 2026-09-14：截断尾部不完整 UTF-8 序列（中文文件名） */

/* 本地提示音（2026-08-16 P102）：播 /resource/tones/<name>.wav
 * 后台线程直读 WAV → snd_vela_pcm，不经过 XPlayer（避免与音乐状态机纠缠）；
 * 声卡被音乐占用时 open 失败 → 跳过提示音（不打断音乐）。 */
void dm_tone_play(const char *name);

/* 2026-09-01 P142：开场背景音效 + 语音提示 顺序播放（"intro|voice"）。
 * 背景音乐先响、语音随后，避免"突然有人说话"；两文件格式可不同。 */
void dm_tone_play_seq(const char *intro, const char *voice);

/* Settings 构建器（Phase 1 拆分：ui_bt.c 等子页复用，定义留在 deskmate_ui.c） */
lv_obj_t *settings_section_label(lv_obj_t *parent, const char *text);
lv_obj_t *settings_group_card(lv_obj_t *parent);
lv_obj_t *settings_row_switch_cb(lv_obj_t *card, const char *icon,
                                 uint32_t icon_color, const char *label,
                                 bool initial_state, lv_event_cb_t cb);
void settings_add_separator(lv_obj_t *card);
void settings_bt_cb(lv_event_t *e);   /* Settings 蓝牙开关（ui_bt.c 控制组复用） */
lv_obj_t *settings_row_slider(lv_obj_t *card, const char *icon,
                              uint32_t icon_color, const char *label,
                              int value_pct, lv_event_cb_t cb);
lv_obj_t *settings_row_value(lv_obj_t *card, const char *icon,
                             uint32_t icon_color, const char *label,
                             const char *value, bool show_arrow);
lv_obj_t *settings_row_switch(lv_obj_t *card, const char *icon,
                              uint32_t icon_color, const char *label,
                              bool initial_state);

/* 待机/音量共享状态（Phase 1 拆分：ui_settings.c 引用） */
extern int          g_idle_timeout;     /* 自动锁屏秒数（0=Never） */
void reset_idle_timer(void);
extern int          g_music_volume;     /* 软件音量 0-100（music 区定义） */
extern lv_obj_t    *music_vol_lbl;      /* 播放器音量数值 label */
extern int          g_screen_brightness; /* 2026-08-23 P115：屏幕背光 0-100（AI 语音命令步进） */
void dm_brightness_write(int v);         /* P180：背光硬件写（屏幕 PWM ch4 + WS2812），滑块/夜览/自动亮度共用 */
void dm_night_shift_hook(int hour);      /* P179：夜览开机状态恢复（update_time_cb 挂载，仅首 tick 生效） */
bool dm_night_shift_enabled(void);       /* P180：夜览开关状态（自动亮度引擎仲裁用） */
void weather_kick_fetch(void);           /* 2026-09-10：后台天气 fetch（ui_home.c；城市切换后即时刷新用） */

/* 共享 UI 工具（Phase 1 拆分：ui_books.c 等子页复用） */
lv_obj_t *music_round_btn(lv_obj_t *parent, lv_coord_t size,
                          const char *symbol, lv_event_cb_t cb,
                          void *user_data);
void music_round_btn_press_icon_cb(lv_event_t *e);  /* 按钮 icon 按下变白/松开恢复 */

/* 页面导航（Phase 1 拆分：ui_files.c files_go_up 退出子页调用） */
void close_subpage(void);

/* Music 播放核心状态（Phase 1 拆分：定义在 deskmate_ui.c music 区，
 * ui_music.c 仅 UI 引用；Phase 2c music_service 接入时收拢） */
struct dm_track
{
    char     path[256];   /* full file path */
    char     title[128];  /* filename without extension */
    uint32_t length;      /* seconds; 0 = unknown */
};
extern struct dm_track dm_tracks[DM_MAX_TRACKS];
extern uint32_t        dm_track_count;
extern int             music_track_id;
extern bool            music_playing;
extern bool            music_inited;
/* P145：当前播放文件（独立于曲库列表——文件浏览器点播任意目录文件时，
 * 播放器页/锁屏标题一律读 dm_now_*，不依赖扫描列表索引，避免列表重建后
 * 显示旧曲目（AURORA bug 根因） */
extern char            dm_now_path[256];   /* 当前播放文件完整路径（空=从未播放） */
extern char            dm_now_title[128];  /* 当前播放标题（文件名去扩展名） */
void music_play_path(const char *path);    /* P145：按路径播放（不在曲库也播） */
extern lv_obj_t       *music_title_lbl;
extern lv_obj_t       *music_artist_lbl;
extern lv_obj_t       *music_slider;
extern lv_obj_t       *music_time_lbl;
extern lv_obj_t       *music_time_total_lbl;
extern lv_obj_t       *music_play_btn;
extern lv_obj_t       *music_play_icon;
extern lv_obj_t       *music_playlist_overlay;
extern lv_obj_t       *music_list_cont;
extern lv_obj_t       *music_empty_lbl;
extern lv_obj_t       *music_active_btn;
extern lv_timer_t     *g_music_timer;
extern lv_style_t      style_card;      /* 共享玻璃卡样式（playlist 卡用） */

/* Music 播放核心函数（Phase 1 拆分：定义在 deskmate_ui.c，ui_music.c 调用） */
void music_play(uint32_t id);
void music_resume(void);
void music_pause(void);
void music_album_next(bool next);
void music_update_track_info(void);
uint32_t music_state_load(void);
uint32_t music_scan_tracks(void);
void music_timer_cb(lv_timer_t *t);

/* 页面导航状态（Phase 1 拆分：book_open_reader 挂载父 overlay 需要） */
extern lv_obj_t *subpage_overlay;

/* ================================================================
 * HOME 共享状态（Phase 1 拆分：ui_home.c 定义与使用，deskmate_ui.c 保留
 * 播放核心/导航；此段为跨文件 extern 集中区）
 * ================================================================ */

/* Dock app 数据结构（create_app_dock 用，ui_home.c 内部） */
struct deskmate_app
{
    const char          *name;      /* internal name */
    const char          *label;     /* display label */
    const lv_image_dsc_t *icon;     /* PNG icon (deskmate_icons.h) */
};

/* HOME 构建与交互回调（ui_home.c 定义） */
void ui_home_create(void);
void clock_update_cb(lv_timer_t *timer);   /* 1s 时钟 + 状态栏网络图标 */
void app_icon_click_cb(lv_event_t *e);     /* Dock 图标点击 → 子页 */
void screen_input_cb(lv_event_t *e);       /* 任意交互重置待机 */
void init_shared_styles(void);             /* 共享玻璃卡样式 */

/* HOME 常驻 UI 指针/状态（ui_home.c 与 deskmate_ui.c 共享） */
extern lv_obj_t     *clock_label;        /* status bar clock（左上角时间） */
extern lv_obj_t     *clock_date_label;   /* status bar date（左上角周几日期，P145 复用原 hero 日期名） */
extern lv_obj_t     *status_weather_label;  /* status bar weather */
extern lv_group_t   *g_group;
extern lv_indev_t   *g_keypad_indev;
extern lv_obj_t     *g_dock;
extern lv_obj_t     *status_wifi_lbl;
extern lv_obj_t     *status_bt_lbl;
/* 2026-08-10 子页顶部状态栏（show_subpage 创建；clock_update_cb /
 * dm_net_status_refresh 每秒刷新；close_subpage 删除 overlay 前置空）——
 * 子页全屏 overlay 盖住主屏 header，必须自带时钟+网络图标，否则进 Settings
 * 等子页看不到状态栏 */
extern lv_obj_t     *subpage_clock_lbl;   /* 子页状态栏时钟 */
extern lv_obj_t     *subpage_date_lbl;    /* P145：子页状态栏周几日期 */
extern lv_obj_t     *subpage_weather_lbl; /* 子页状态栏天气 */
extern lv_obj_t     *subpage_wifi_lbl;    /* 子页状态栏 wifi 图标 */
extern lv_obj_t     *subpage_bt_lbl;      /* 子页状态栏 bt 图标 */
/* 2026-08-10 共享状态栏构建（ui_home.c 定义）：Home 与子页复用同一份代码，
 * 视觉完全一致（132px 全宽：左时钟两行(时间/周几日期) + 中天气 + 右 wifi/bt/battery）。
 * P145：左侧改为两行容器，clock_out=时间、date_out=周几日期；点击容器可手动对时。
 * 调用方提供输出指针：Home 传全局（clock_label/status_*），子页传 subpage_*。 */
void create_status_bar_ex(lv_obj_t *bar,
                          lv_obj_t **clock_out, lv_obj_t **date_out,
                          lv_obj_t **weather_out,
                          lv_obj_t **wifi_out, lv_obj_t **bt_out);
extern lv_obj_t     *home_music_card;
extern lv_obj_t     *home_music_title_lbl;
extern lv_obj_t     *home_music_play_icon;
extern bool          home_music_visible;
extern lv_obj_t     *standby_music_title_lbl;
extern lv_obj_t     *standby_music_play_icon;

/* 天气卡状态（ui_home.c 创建，deskmate_ui.c weather_update_cb 已搬走仍共享） */
extern dm_weather_t  g_weather;
extern lv_obj_t     *w_cur_temp_lbl;
extern lv_obj_t     *w_cur_desc_lbl;
extern lv_obj_t     *w_day_lbl[DM_WEATHER_DAYS];
extern lv_obj_t     *w_icon_cv[DM_WEATHER_DAYS];
extern uint32_t      w_icon_buf[DM_WEATHER_DAYS][64 * 64];
extern lv_obj_t     *w_temp_lbl[DM_WEATHER_DAYS];

/* 待机/锁屏状态（ui_home.c 定义，ui_home.c 内部 static 使用） */
extern lv_obj_t     *standby_overlay;
extern lv_obj_t     *standby_ring;
extern lv_obj_t     *standby_ai_text;
extern lv_timer_t   *idle_timer;
extern int           idle_countdown;

/* 2026-08-15 健康助理：AI 语音 PTT 回调导出（AI 子页 + 锁屏 AI 圈共用）。
 * 样式操作基于 lv_event_get_target(e)，任一按钮按住说话均生效。 */
void dm_ai_voice_press_cb(lv_event_t *e);
void dm_ai_voice_release_cb(lv_event_t *e);

/* 2026-08-10 锁屏状态栏指针（show_standby 创建，复用 create_status_bar_ex；
 * clock_update_cb / weather_update_cb / dm_net_status_refresh 刷新；
 * standby_overlay 常驻不删除，无需置空） */
extern lv_obj_t     *standby_bar_clock_lbl;
extern lv_obj_t     *standby_bar_date_lbl;   /* P145：锁屏状态栏周几日期 */
extern lv_obj_t     *standby_bar_weather_lbl;
extern lv_obj_t     *standby_bar_wifi_lbl;
extern lv_obj_t     *standby_bar_bt_lbl;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Build the full Desktop Mate screen (call after lv_nuttx_init + init_fonts) */
void deskmate_ui_create(void);

/* Page builders (Phase 1 拆分：每拆一页在 ui/ 目录新增 ui_xxx.c) */
void ui_wifi_create(lv_obj_t *parent);      /* ui_wifi.c */
void ui_wifi_close_cleanup(void);           /* ui_wifi.c：密码层等根屏 overlay 清理 */
void ui_bt_create(lv_obj_t *parent);        /* ui_bt.c */
void ui_books_create(lv_obj_t *parent);     /* ui_books.c */
void book_reader_close(void);               /* ui_books.c：close_subpage 清理阅读器 */
void ui_files_create(lv_obj_t *parent);     /* ui_files.c */
void files_ui_clear_ptrs(void);             /* ui_files.c：close_subpage 清理多选栏 */
void ui_settings_create(lv_obj_t *parent);  /* ui_settings.c */
void ui_settings_close_cleanup(void);       /* ui_settings.c：close_subpage 清日期面板 */
void ui_uart_dbg_create(lv_obj_t *parent);      /* ui_uart_dbg.c：UART 调试工具子页 */
void ui_uart_dbg_close_cleanup(void);           /* ui_uart_dbg.c：close_subpage 停 timer+关串口 */
void show_subpage(const char *title);       /* ui_settings.c 开 WiFi/蓝牙嵌套子页 */
void ui_pet_create(lv_obj_t *parent);       /* ui_pet.c：宠物子页（Settings 一行入口） */
void ui_pet_close_cleanup(void);            /* ui_pet.c：close_subpage 清起名/重置浮层 */
void ui_alarm_create(lv_obj_t *parent);     /* ui_alarm.c：闹钟子页 */
void ui_alarm_close_cleanup(void);          /* ui_alarm.c：close_subpage 清添加面板 */
void ui_datetime_create(lv_obj_t *parent);  /* ui_datetime.c：日期与时间子页 */
void ui_datetime_close_cleanup(void);       /* ui_datetime.c：close_subpage 停timer+清面板 */
void ui_music_create(lv_obj_t *parent);     /* ui_music.c */
void music_ui_clear_ptrs(void);             /* ui_music.c：close_subpage 清播放器页指针 */
void music_prev_cb(lv_event_t *e);          /* ui_music.c：HOME/锁屏 ⏮ 复用 */
void music_next_cb(lv_event_t *e);          /* ui_music.c：HOME/锁屏 ⏭ 复用 */
lv_obj_t *music_play_btn_create(lv_obj_t *parent, lv_coord_t size,
                                lv_obj_t **icon_out);  /* ui_music.c：播放键（三 UI 共用） */
void music_volume_persist_debounced(void);  /* ui_music.c：音量落盘 800ms 防抖（音乐/设置/语音共用） */
void ui_home_create(void);                  /* ui_home.c：主界面 + 状态栏 + Dock + 待机 */
void ui_calendar_create(lv_obj_t *parent);  /* ui_calendar.c：日历月视图 */
void ui_calendar_close_cleanup(void);       /* ui_calendar.c：close_subpage 停午夜timer+纪念日浮层 */

/* Books 共享状态（Phase 1 拆分：deskmate_ui.c files_open_file 引用） */
void book_open_reader(int idx);             /* ui_books.c */
extern char     dm_book_paths[DM_MAX_BOOKS][160];
extern char     dm_book_titles[DM_MAX_BOOKS][64];
extern uint32_t dm_book_count;

/* ── 共享键盘工具（dm_kb_*）──────────────────────────────
 * 统一管理 LVGL 键盘的创建、显隐、事件路由。
 * 用法：
 *   static void my_send_cb(lv_obj_t *ta, void *ud) { ... }
 *   dm_kb_state_t *kb = dm_kb_create(parent, textarea, my_send_cb, ctx);
 *   // 退出时 dm_kb_destroy(kb);
 * ────────────────────────────────────────────────────── */
typedef struct dm_kb_state_s dm_kb_state_t;
typedef void (*dm_kb_ready_cb_t)(lv_obj_t *ta, void *user_data);

/* 创建键盘：绑定 textarea，初始隐藏，点击 textarea 弹出，回车触发 on_ready，
 * 取消/失焦隐藏。parent 通常是 subpage_overlay。 */
dm_kb_state_t *dm_kb_create(lv_obj_t *parent, lv_obj_t *ta,
                             dm_kb_ready_cb_t on_ready, void *user_data);

/* 手动隐藏键盘 */
void dm_kb_hide(dm_kb_state_t *state);

/* 销毁键盘（释放 state，从 textarea 解绑） */
void dm_kb_destroy(dm_kb_state_t *state);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DESKMATE_UI_H */
