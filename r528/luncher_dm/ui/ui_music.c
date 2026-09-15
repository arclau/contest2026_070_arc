/*
 * ui_music.c — 音乐子页 UI【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_music_subpage（改名
 * ui_music_create）+ 播放器页 UI 回调（music_play_pause/prev/next/vol_cb、
 * music_track_click_cb、playlist overlay、music_ui_clear_ptrs）。
 * 逻辑一字不改。
 *
 * 红线（勿碰，属播放核心，留在 deskmate_ui.c）：
 *   - dm_sound_* / music_audio_* / g_xplayer：XPlayer 生命周期 + codec
 *     route（POWER_ANA_CTL@0x348 只 update_bits 禁整写；RAMP SRST 时序）。
 *   - music_play/resume/pause/album_next/scan_tracks/state_load/timer_cb：
 *     经 deskmate_ui.h extern 调用，Phase 2c 一次性接入 music_service。
 *   - g_music_timer 系统级常驻（create_deskmate_screen 创建），不随子页
 *     销毁；本文件只复位 UI 状态。
 */

#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_health_cfg.h"   /* 2026-09-14：音乐音量落盘供 AI 语音同步 */

/* 前向声明（create_music_playlist_overlay 在 toggle 定义之前引用） */
static void music_playlist_toggle_cb(lv_event_t *e);
static void music_playlist_toggle(void);

/* ================================================================
 * MUSIC SUBPAGE — ported from LVGL music demo (lv_demo_music)
 * Light theme, no intro animation, DM() scaled for 1920x1200.
 * Track list is scanned from the TF card (/sdcard/music), filtered
 * to the supported formats (mp3/flac/ogg/aac/wav).
 * ================================================================ */

/* 子页面关闭后清空播放器页 UI 指针（close_subpage 调用）。music_timer
 * 是系统级常驻，后台播放时只轮询状态机不刷新已销毁的播放器页 UI。
 * 注意：必须覆盖【全部】播放器页对象——2026-08-08 曾漏 music_vol_lbl，
 * Settings 音量滑块回调判空通过后访问已释放对象 → lv_obj_get_parent
 * Data Abort（PC=0x415dd582）。 */
void music_ui_clear_ptrs(void)
{
    music_title_lbl = NULL;
    music_artist_lbl = NULL;
    music_slider = NULL;
    music_time_lbl = NULL;
    music_time_total_lbl = NULL;
    music_play_btn = NULL;
    music_play_icon = NULL;
    music_vol_lbl = NULL;
    music_list_cont = NULL;
    music_active_btn = NULL;
    /* P215：overlay/empty_lbl 同属播放器页对象，漏清则 timer/回调判空
     * 通过后访问已释放对象 → UAF（对照 8-08 music_vol_lbl 教训） */
    music_playlist_overlay = NULL;
    music_empty_lbl = NULL;
}

/* ── 播放/暂停按钮（LVGL 事件回调入口）── */
static void music_play_pause_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();   /* 2026-08-08：音乐按钮点击也是交互，防 idle 到期跳锁屏 */
    if (music_playing)
        music_pause();
    else if (music_inited)
        music_resume();
    else
        music_play(music_track_id);
}

/* HOME/锁屏音乐控件（deskmate_ui.c）也挂 prev/next 回调——非 static */
void music_prev_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    music_album_next(false);
}

void music_next_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    music_album_next(true);
}

/* 音量 +/- 回调（2026-08-07 播放器音量 UI）：user_data 携带步长（±5）。
 * 直接改 g_music_volume，dm_sound_write 下一包自动按新音量衰减，
 * 无需重启播放；数值实时刷新到右侧音量列。 */
static lv_timer_t *s_vol_save_timer = NULL;   /* 落盘防抖：连点只写最后一次 */

static void music_vol_save_cb(lv_timer_t *t)
{
    (void)t;
    s_vol_save_timer = NULL;   /* repeat_count=1：到期自删 */
    dm_health_cfg_set_int("music_volume", g_music_volume);
}

/* 音量落盘统一入口（800ms 防抖）：音乐页/设置页/语音命令共用，
 * 防滑块拖动或连点每次 ± 都整写 dm_health.cfg 刷 flash。 */
void music_volume_persist_debounced(void)
{
    if (s_vol_save_timer) {
        lv_timer_reset(s_vol_save_timer);   /* 连点：顺延 800ms */
        return;
    }
    s_vol_save_timer = lv_timer_create(music_vol_save_cb, 800, NULL);
    if (s_vol_save_timer)
        lv_timer_set_repeat_count(s_vol_save_timer, 1);
    else
        dm_health_cfg_set_int("music_volume", g_music_volume);   /* timer 建失败兜底直写 */
}

static void music_vol_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int step = (int)(intptr_t)lv_obj_get_user_data(btn);
    reset_idle_timer();   /* 2026-08-08：音量按钮点击同样算交互 */
    g_music_volume += step;
    if (g_music_volume < 0) g_music_volume = 0;
    if (g_music_volume > 100) g_music_volume = 100;
    if (music_vol_lbl)
        lv_label_set_text_fmt(music_vol_lbl, "%d", g_music_volume);
    /* 2026-09-14：落盘供 AI 语音（agent）同步 TTS；800ms 防抖防连点刷 flash */
    music_volume_persist_debounced();
    printf("[music] volume=%d\n", g_music_volume);
}

/* 播放/暂停圆形玻璃按钮（2026-08-08 三 UI 统一）：默认白色玻璃圆 + 蓝色
 * 三角符号；播放中（CHECKED）与按下（PRESSED）均为蓝紫渐变玻璃 + 白色
 * 符号 + 蓝光晕，与 music_round_btn（prev/next/音量±）按压观感一致。
 * icon_out 返回符号 label 指针（music_resume/music_pause 改文字/颜色用）。
 * Phase 1 拆分：HOME 音乐卡 / standby 锁屏控件（deskmate_ui.c）也调用，
 * 经 deskmate_ui.h 声明跨文件引用。 */
lv_obj_t *music_play_btn_create(lv_obj_t *parent, lv_coord_t size,
                                       lv_obj_t **icon_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(btn, 8, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    /* Playing (CHECKED): blue→purple gradient glass fill + glow shadow,
     * matching the album cover — override the default theme red so the
     * state looks intentional. Icon turns white so it is visible on blue. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BLUE), LV_STATE_CHECKED);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(COL_PURPLE), LV_STATE_CHECKED);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(btn, 14, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(COL_BLUE), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_STATE_CHECKED);
    /* Pressed feedback so the tap is obvious */
    lv_obj_set_style_transform_width(btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(COL_PURPLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 14, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn, music_play_pause_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(icon, FONT_BODY, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_BLUE), 0);
    /* Icon is a child label: CHECKED/PRESSED belongs to the button, not the
     * icon, so state styles on the icon never apply. Keep the base color blue
     * here; music_resume/music_pause switch it to white explicitly. */
    lv_obj_center(icon);
    if (icon_out) *icon_out = icon;
    return btn;
}

/* 播放列表行点击 → 播放该曲（停止冒泡，防 overlay 点击回调二次触发） */
static void music_track_click_cb(lv_event_t *e)
{
    /* Stop bubbling: row click must not also reach the overlay's toggle
     * callback, which would re-open the just-hidden playlist. */
    lv_event_stop_bubbling(e);

    lv_obj_t *btn = lv_event_get_target(e);
    uint32_t idx = (uint32_t)(uintptr_t)lv_obj_get_user_data(btn);

    /* 2026-08-08：去选择条——不再整行背景高亮，当前歌曲由
     * music_update_track_info 用文字蓝色标记。 */

    /* Close playlist overlay and start playback */
    if (music_playlist_overlay)
        lv_obj_add_flag(music_playlist_overlay, LV_OBJ_FLAG_HIDDEN);

    music_play(idx);
}

/* ── Playlist overlay (full-screen, hidden by default) ── */
static void create_music_playlist_overlay(lv_obj_t *scr)
{
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, sw, sh);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_set_scrollbar_mode(ov, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ov);
    lv_obj_add_event_cb(ov, music_playlist_toggle_cb, LV_EVENT_CLICKED, NULL);
    music_playlist_overlay = ov;

    /* Dropdown card below the three-dot button: header + scrollable list.
     * Opaque background (readable text) + pressed feedback on rows.
     * 2026-08-10 修复（-29）：原 y=DM(52)≈125px 顶进 0~132 状态栏区域
     * （"播放列表跑状态栏上"）。改 TOP_BAR_H+DM(52) 整体下移——与菜单键
     * 保持原 DM(8) 相对间距（菜单键底 TOP_BAR_H+DM(44)，卡片顶 +DM(52)）。 */
    lv_obj_t *card = lv_obj_create(ov);
    /* 2026-09-10 P206：DM(180)→DM(240)≈576px——长歌名+序号放得下，
     * 配 DOT 单行省略（下），歌多只滚不动布局。 */
    lv_obj_set_size(card, DM(240), DM(360));
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -DM(16), TOP_BAR_H + DM(52));
    assert(sw - DM(16) - DM(240) >= 0 && TOP_BAR_H + DM(52) + DM(360) <= sh &&
           "deskmate: 播放列表面板越界");
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);   /* 2026-08-08 玻璃感：80% 半透明，隐约透出下层播放器 */
    lv_obj_set_style_radius(card, RAD_CARD, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    /* Header row: title + close */
    lv_obj_t *hdr = make_clean_cont(card);
    lv_obj_set_size(hdr, lv_pct(100), DM(30));
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(hdr, DM(8), 0);
    lv_obj_set_style_pad_right(hdr, DM(4), 0);

    lv_obj_t *ht = lv_label_create(hdr);
    lv_label_set_text(ht, "播放列表");
    lv_obj_set_style_text_font(ht, FONT_BODY, 0);
    lv_obj_set_style_text_color(ht, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(ht, 1);

    lv_obj_t *close_btn = lv_btn_create(hdr);
    lv_obj_set_size(close_btn, DM(20), DM(20));
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, music_playlist_toggle_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, FONT_BODY, 0);
    lv_obj_set_style_text_color(close_icon, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(close_icon);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_40, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* Scrollable track list。
     * 2026-09-14 P222：修复歌多滚不动——原 lv_pct(100)/c+clear SCROLLABLE
     * 双保险把滚动彻底关掉（>12 首点不到）。改用 flex_grow(1) 吃掉
     * header+分隔线外的剩余高度，保持 SCROLLABLE → LVGL 默认弹性/惯性
     * 滑动（与 Settings 长页同手感），行超高自动滚动。 */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, DM(2), 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    music_list_cont = list;

    /* Refresh from TF card so newly copied songs appear */
    music_scan_tracks();

    /* 2026-09-10 P206：标题带计数"播放列表（N)"，歌多一目了然 */
    {
        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "播放列表（%u)", (unsigned)dm_track_count);
        lv_label_set_text(ht, hbuf);
    }

    if (dm_track_count == 0)
    {
        music_empty_lbl = lv_label_create(list);
        lv_label_set_text(music_empty_lbl,
                          "未找到音乐\n请将歌曲放入 " DM_MUSIC_DIR);
        lv_obj_set_style_text_font(music_empty_lbl, FONT_BODY, 0);
        lv_obj_set_style_text_color(music_empty_lbl, lv_color_hex(COL_SEC), 0);
        lv_obj_set_style_text_align(music_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(music_empty_lbl, DM(8), 0);
        return;
    }

    for (uint32_t i = 0; i < dm_track_count; i++) {
        lv_obj_t *row = make_clean_cont(list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, DM(24), 0);
        lv_obj_set_style_pad_top(row, 0, 0);
        lv_obj_set_style_pad_bottom(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(row, 10, 0);
        lv_obj_set_style_pad_right(row, 10, 0);
        /* 2026-08-08：紧凑列表——去圆角、去分隔线，行距收紧提高显示效率 */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, music_track_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, (void *)(uintptr_t)i);
        /* Pressed feedback so the tap is obvious */
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);

        /* 2026-09-10 P206：序号并入同一 label（child(0) 仍是标题行，
         * music_update_track_info 高亮逻辑零改）+ DOT 单行省略——
         * 长名不再 WRAP 撑高行，64 首也是一行一条。 */
        lv_obj_t *title_lbl = lv_label_create(row);
        {
            char rowbuf[160];
            snprintf(rowbuf, sizeof(rowbuf), "%02u. %s",
                     (unsigned)(i + 1), dm_tracks[i].title);
            lv_label_set_text(title_lbl, rowbuf);
        }
        lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title_lbl, lv_pct(100));
        lv_obj_set_style_text_font(title_lbl, FONT_LABEL, 0);
        lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_TEXT), 0);
    }
}

/* ── Three-dot menu button (top-right) → toggle playlist overlay ── */
static void music_playlist_toggle_cb(lv_event_t *e)
{
    /* Stop bubbling so the overlay's own click handler (also wired to this
     * cb) doesn't fire twice and immediately re-open the popup. */
    lv_event_stop_bubbling(e);
    music_playlist_toggle();
}

static void music_playlist_toggle(void)
{
    if (!music_playlist_overlay) return;
    if (lv_obj_has_flag(music_playlist_overlay, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(music_playlist_overlay, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(music_playlist_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_music_create(lv_obj_t *parent)
{
    /* Column layout, vertically centered — no top bar, cover centers */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, DM(10), 0);

    /* ── Three-dot menu button — frosted glass, same size as back btn ──
     * 2026-08-10 修复（-29）：原 y=DM(4)≈10px 坐在 0~132 状态栏区域内
     * （创建晚于 move_foreground(sbar) → 盖在状态栏上）。
     * 改 TOP_BAR_H+DM(4) 与左下方返回键（-23 同坐标）镜像对称：同高、左右对开。 */
    lv_obj_t *menu_btn = lv_btn_create(subpage_overlay);
    lv_obj_set_size(menu_btn, DM(40), DM(40));
    lv_obj_align(menu_btn, LV_ALIGN_TOP_RIGHT, -DM(16), TOP_BAR_H + DM(4));
    assert(sw - DM(16) - DM(40) >= 0 && TOP_BAR_H + DM(4) + DM(40) <= sh &&
           "deskmate: 播放列表按钮越界");
    lv_obj_set_style_radius(menu_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(menu_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(menu_btn, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_add_event_cb(menu_btn, music_playlist_toggle_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_move_foreground(menu_btn);
    /* Three vertical dots — drawn as small circles, no font dependency */
    lv_obj_t *dots = make_clean_cont(menu_btn);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dots, 3, 0);
    lv_obj_center(dots);
    for (int d = 0; d < 3; d++) {
        lv_obj_t *dot = lv_obj_create(dots);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_60, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
    }

    /* ── Album cover (large, centered) ── */
    lv_obj_t *cover = lv_obj_create(parent);
    lv_obj_set_size(cover, DM(120), DM(120));   /* 1.5× of DM(80) */
    /* Keep the bottom edge where DM(80) was: shift up by (120-80)/2*2.4 */
    lv_obj_set_style_margin_top(cover, -DM(20), 0);
    lv_obj_set_style_radius(cover, RAD_CARD, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_hex(COL_PURPLE), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(cover, 16, 0);
    lv_obj_set_style_shadow_color(cover, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_shadow_opa(cover, LV_OPA_30, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_scrollbar_mode(cover, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cover_icon = lv_label_create(cover);
    lv_label_set_text(cover_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(cover_icon, FONT_TITLE, 0);
    lv_obj_set_style_text_color(cover_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cover_icon);

    /* ── Track info ── */
    music_title_lbl = lv_label_create(parent);
    lv_label_set_text(music_title_lbl, "暂无曲目");
    lv_obj_set_style_text_font(music_title_lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(music_title_lbl, lv_color_hex(COL_TEXT), 0);

    music_artist_lbl = lv_label_create(parent);
    lv_label_set_text(music_artist_lbl, "");
    lv_obj_set_style_text_font(music_artist_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(music_artist_lbl, lv_color_hex(COL_SEC), 0);

    /* ── Progress row: elapsed | slider | total ── */
    lv_obj_t *progress_row = make_clean_cont(parent);
    lv_obj_set_size(progress_row, lv_pct(80), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(progress_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(progress_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(progress_row, 12, 0);

    /* Elapsed time on the left of the slider, total on the right. */
    music_time_lbl = lv_label_create(progress_row);
    lv_label_set_text(music_time_lbl, "0:00");
    lv_obj_set_style_text_font(music_time_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(music_time_lbl, lv_color_hex(COL_SEC), 0);

    music_slider = lv_slider_create(progress_row);
    lv_obj_set_flex_grow(music_slider, 1);
    lv_obj_set_height(music_slider, DM(6));      /* thicker progress bar */
    lv_obj_set_style_bg_color(music_slider, lv_color_hex(COL_BLUE),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(music_slider, DM(3), LV_PART_INDICATOR);
    lv_obj_set_style_radius(music_slider, DM(3), LV_PART_KNOB);
    lv_obj_set_style_bg_color(music_slider, lv_color_hex(0xFFFFFF),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(music_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(music_slider, DM(3), LV_PART_KNOB);

    music_time_total_lbl = lv_label_create(progress_row);
    lv_label_set_text(music_time_total_lbl, "0:00");
    lv_obj_set_style_text_font(music_time_total_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(music_time_total_lbl, lv_color_hex(COL_SEC), 0);

    /* ── Controls row: prev | play | next ── */
    lv_obj_t *ctrl_row = make_clean_cont(parent);
    lv_obj_set_size(ctrl_row, lv_pct(50), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *prev_btn = music_round_btn(ctrl_row, DM(40), LV_SYMBOL_PREV,
                                         music_prev_cb, NULL);

    music_play_btn = music_play_btn_create(ctrl_row, DM(52), &music_play_icon);

    lv_obj_t *next_btn = music_round_btn(ctrl_row, DM(40), LV_SYMBOL_NEXT,
                                         music_next_cb, NULL);

    /* ── 右侧音量列（2026-08-07 用户需求）：竖向排列 上=+ 下=−，
     * 中间实时显示 0-100 数值。挂在 subpage_overlay 上与 menu_btn 同
     * 生命周期，随子页关闭一并删除。按下效果与 prev/play/next 一致。
     * 2026-08-07 第2版：恢复原布局（音量列在播放器右侧，不动控件行）。 ── */
    lv_obj_t *vol_col = make_clean_cont(subpage_overlay);
    lv_obj_set_size(vol_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vol_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vol_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(vol_col, DM(6), 0);
    lv_obj_align(vol_col, LV_ALIGN_RIGHT_MID, -DM(20), 0);
    lv_obj_move_foreground(vol_col);

    music_round_btn(vol_col, DM(40), LV_SYMBOL_PLUS, music_vol_cb,
                    (void *)(intptr_t)5);
    music_vol_lbl = lv_label_create(vol_col);
    lv_label_set_text_fmt(music_vol_lbl, "%d", g_music_volume);
    lv_obj_set_style_text_font(music_vol_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(music_vol_lbl, lv_color_hex(COL_TEXT), 0);
    music_round_btn(vol_col, DM(40), LV_SYMBOL_MINUS, music_vol_cb,
                    (void *)(intptr_t)-5);

    /* Bottom spacer so controls sit above the bottom edge */
    lv_obj_t *spacer = make_clean_cont(parent);
    lv_obj_set_size(spacer, DM(1), DM(8));

    /* ── Playlist overlay (hidden until menu button tapped) ──
     * Mounted on subpage_overlay so it is deleted when the subpage
     * closes — mounting on lv_scr_act() would leave a lingering
     * full-screen overlay on the home screen and block touches. */
    create_music_playlist_overlay(subpage_overlay);

    /* 音乐后台化（2026-08-08）：music timer 是系统级（create_deskmate_screen
     * 创建，500ms，不随子页面开/关销毁）——此处只复位 UI 状态，不再删建
     * timer。g_music_timer 若为空（理论不发生）则兜底创建一次。 */
    if (g_music_timer == NULL) {
        g_music_timer = lv_timer_create(music_timer_cb, 500, NULL);
        lv_timer_set_repeat_count(g_music_timer, -1);
    }
    music_active_btn = NULL;
    if (music_playing) {
        /* 2026-09-15：重进时后台正在播，不要重置为停止态。music_playing
         * 在 close_subpage 未被清，仍反映后端真实状态。直接恢复播放态 UI
         *（按钮置 PAUSE + timer 恢复，进度/时间由 500ms timer 接管刷新）；
         * 曲目保持当前 music_track_id（自动连播已推进），不读盘覆盖。
         * music_resume 内 audio 已在播时 resume 为空操作。 */
        music_resume();
    } else {
        lv_timer_pause(g_music_timer);
        music_track_id = music_state_load();   /* resume last-played track */
        if (music_track_id >= dm_track_count)
            music_track_id = 0;
    }
    music_update_track_info();   /* show remembered / first track / "No track" hint */
}
