/*
 * ui_home.c — 主界面 + 状态栏 + 大时钟 + Dock + 待机【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_deskmate_screen（改名
 * ui_home_create）+ 状态栏/大时钟/天气卡/AI卡/首页音乐卡/Dock/待机 +
 * 4 个系统级 timer（clock 1s / weather 10min / idle 1s / music 500ms）。
 * 逻辑一字不改。涉及面最大、常驻对象最多，Phase 1 最后拆。
 *
 * 红线（勿碰）：
 *   - 系统级 timer（clock/weather/idle/music）随进程常驻，不随子页销毁。
 *   - idle_timer_cb 判空分支（standby_overlay）原样保留；g_idle_timeout
 *     被 ui_settings.c（Auto-Lock）extern 引用（deskmate_ui.h 声明）。
 *   - standby 音乐控件复用 ui_music.c 的按钮/回调（music_round_btn、
 *     music_play_btn_create、music_prev/next_cb），不重复实现。
 */

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_weather.h"
#include "dm_net.h"   /* P113：dm_net_wifi_connected（WiFi 连上立即拉天气） */
#include "deskmate_icons.h"
#include "dm_health.h"   /* 2026-08-15 健康助理：双圈倒计时 + 弹窗事件 */
#include "dm_ld2410b.h"  /* 2026-08-23：人体感应源 LD2410B（锁屏人体状态显示） */
#include "dm_health_cfg.h" /* 2026-08-15 健康助理：文案/参数配置文件 */
#include "dm_ai.h"       /* 2026-08-15 健康助理：L2/L3 弹窗 AI 语音播报 */
#include "dm_voice.h"    /* 2026-08-28 Voice Director：坐下播报决策 */
#include "dm_als.h"      /* P153：LTR553 ALS 环境光（锁屏亮度显示 + 暗光开灯提醒） */
#include "dm_pet.h"      /* 电子宠物：锁屏宠物 + 健康事件联动 */

/* 前向声明（launch_app_after_fade 引用 show_standby；show_standby 定义靠后） */
static void show_standby(void);
static void home_health_rings_refresh(void);    /* P103：HOME 健康卡双小圈刷新 */

/* ================================================================
 * CLOCK UPDATE CALLBACK（1s 系统 timer，2026-08-09 从 deskmate_ui.c 搬入）
 * ================================================================ */

void clock_update_cb(lv_timer_t *timer)
{
    (void)timer;
    time_t now;
    struct tm tm;
    time(&now);
    /* Board has no TZ config: force UTC+8 (same as luncher_dm update_time_cb) */
    struct tm *utc = gmtime(&now);
    if (utc == NULL) return;
    tm = *utc;
    tm.tm_hour += 8;
    mktime(&tm);

    char buf[32];

    /* Time HH:MM — P145：hero 大时钟已移除，时间只在各状态栏左上角 */
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(clock_label, buf);
    /* 2026-08-10 子页状态栏时钟（show_subpage 创建，关闭前置空） */
    if (subpage_clock_lbl)
        lv_label_set_text(subpage_clock_lbl, buf);
    /* 2026-08-10 锁屏状态栏时钟（show_standby 创建） */
    if (standby_bar_clock_lbl)
        lv_label_set_text(standby_bar_clock_lbl, buf);

    /* Date: 周日 8月28日（2026-08-28 巡检汉化：原 Sun/Aug 英文星期+月） */
    static const char *cn_weekdays[] =
        { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    static const char *cn_months[] =
        { "1月", "2月", "3月", "4月", "5月", "6月",
          "7月", "8月", "9月", "10月", "11月", "12月" };
    snprintf(buf, sizeof(buf), "%s %s%d日",
             cn_weekdays[tm.tm_wday],
             cn_months[tm.tm_mon],
             tm.tm_mday);
    lv_label_set_text(clock_date_label, buf);
    if (subpage_date_lbl)
        lv_label_set_text(subpage_date_lbl, buf);
    if (standby_bar_date_lbl)
        lv_label_set_text(standby_bar_date_lbl, buf);

    /* 网络状态图标（2026-08-08 接线驱动，每秒刷新一次） */
    dm_net_status_refresh();

    /* P113：WiFi 刚连上（0→1 边沿）→ 立即踢天气 fetch：HTTP Date 对时 +
     * 天气数据一把梭，不等 30s 轮询 timer（开机网络就绪提速关键）。 */
    {
        static int g_wifi_prev_conn = -1;   /* -1=开机首次未判断（首次观察不踢，开机初始 kick 在 ui_home_create） */
        int conn = dm_net_wifi_connected();
        if (g_wifi_prev_conn == 0 && conn == 1)
            weather_kick_fetch();
        g_wifi_prev_conn = conn;
    }
}

/* ================================================================
 * APP ICON CALLBACKS（Dock 图标点击 → 淡出 → 打开子页/待机）
 * ================================================================ */

static void fade_dim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void launch_app_after_fade(lv_anim_t *a)
{
    const char *name = (const char *)lv_anim_get_user_data(a);
    /* Restore full opacity */
    lv_obj_set_style_opa((lv_obj_t *)a->var, 255, 0);
    if      (strcmp(name, "music") == 0)    show_subpage("Music");
    else if (strcmp(name, "calendar") == 0) show_subpage("Calendar");
    else if (strcmp(name, "books") == 0)    show_subpage("Books");
    else if (strcmp(name, "files") == 0)    show_subpage("Files");
    else if (strcmp(name, "ai") == 0)       show_subpage("AI");
    else if (strcmp(name, "settings") == 0) show_subpage("Settings");
    else if (strcmp(name, "lock") == 0)     show_standby();
}

void app_icon_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = (const char *)lv_obj_get_user_data(btn);
    LV_LOG_USER("Launch app: %s", name ? name : "unknown");
    reset_idle_timer();

    /* iOS-style tap dim: icon fades to 50% then back, then launch */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_exec_cb(&a, fade_dim_cb);
    lv_anim_set_values(&a, 255, 128);   /* 100% → 50% opacity */
    lv_anim_set_time(&a, 80);
    lv_anim_set_playback_time(&a, 120);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_repeat_count(&a, 1);
    lv_anim_set_ready_cb(&a, launch_app_after_fade);
    lv_anim_set_user_data(&a, (void *)name);
    lv_anim_start(&a);
}

/* ================================================================
 * SCREEN INPUT — reset idle timer on any interaction
 * ================================================================ */

void screen_input_cb(lv_event_t *e)
{
    reset_idle_timer();
    uint32_t key = lv_event_get_key(e);
    if (key && g_group)
        lv_group_send_data(g_group, key);
}

/* ================================================================
 * STATUS BAR — thin top bar with clock, weather, icons
 * ================================================================ */

/* 2026-08-10 共享状态栏：Home 与子页（show_subpage）调用同一份代码，
 * 视觉完全一致（132px 全宽：左时钟两行 + 中天气 + 右 wifi/bt/battery）。
 * P145：左侧改两行容器（上=时间、下=周几日期），点击容器手动对时
 * （原 hero 大时钟的对时功能迁移至此）。输出指针由调用方提供：
 * Home 传全局（clock_label/status_*），子页传 subpage_*。 */

/* 2026-08-11：点击状态栏时钟 → 手动触发一轮天气 fetch（fetch 成功后
 * dm_weather_fetch 内部会用 HTTP Date 头 settimeofday 对时，worker
 * 线程 ≤5s 完成，时钟即刻跳变）。P145：功能自 hero 大时钟迁移至此。 */
static void clock_click_cb(lv_event_t *e)
{
    (void)e;
    /* 2026-09-10 P206：状态栏时钟点按进闹钟子页（Apple 范）；
     * 手动对时仍走天气卡点按（weather_card_click_cb），功能不丢。 */
    LV_LOG_USER("Clock clicked: open alarm");
    reset_idle_timer();
    show_subpage("Alarm");
}

void create_status_bar_ex(lv_obj_t *bar,
                          lv_obj_t **clock_out, lv_obj_t **date_out,
                          lv_obj_t **weather_out,
                          lv_obj_t **wifi_out, lv_obj_t **bt_out)
{
    /* bar is already created by caller, just populate it.
     * P150：pad 由 EDGE_PAD(96, 卡片级) 收窄为 DM(12) —— 96px 把时钟推到
     * "顶部左中"而非左上角（用户反馈）；DM(12)≈28px 贴角又不贴边。
     * 中间温湿度仍居中：两侧 flex_grow 平分剩余空间，与 pad 无关。 */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, DM(12), 0);
    lv_obj_set_style_pad_right(bar, DM(12), 0);

    /* Left: two-line clock container（时间 + 周几日期），点击可手动对时。
     * P147：flex_grow(1) 让左右两侧平分剩余空间 → 中间温湿度精确居中于
     * 屏幕中心。
     * P150 修订（P149 的 LV_ALIGN_TOP_LEFT 无效——LVGL flex 布局会覆盖
     * lv_obj_set_align）：flex-grow(1) 会把 clk_cont 沿主轴（横向）强制
     * 拉伸到「左缘~屏幕中点」（lv_flex.c place_content：grow 子项被
     * area_set_main_size 硬设宽度），此时 column 的交叉轴若为 CENTER，
     * 时间/日期会被横向居中于该大容器 → 表现为「顶部左中」（用户反馈）。
     * 真正贴「左上角」= ①clk_cont 高度=TOP_BAR_H 填满 bar（垂直不再居中）
     * + ②交叉轴改 START 贴容器左缘（宽度无所谓）。bar 交叉轴仍 CENTER，
     * 对填满高度的容器无影响。 */
    lv_obj_t *clk_cont = make_clean_cont(bar);
    lv_obj_set_size(clk_cont, LV_SIZE_CONTENT, TOP_BAR_H);
    lv_obj_set_flex_grow(clk_cont, 1);
    lv_obj_set_flex_flow(clk_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clk_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(clk_cont, DM(2), 0);
    lv_obj_add_flag(clk_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clk_cont, clock_click_cb, LV_EVENT_CLICKED, NULL);

    /* 上行：时间 — P151 专用档 FONT_CLOCK(56px)，比原 48px 大 17%，
     * 283PPI ≈5.0mm 字高达正文可读标准；右移量见函数尾部对齐补偿 */
    *clock_out = lv_label_create(clk_cont);
    lv_label_set_text(*clock_out, "00:00");
    lv_obj_set_style_text_color(*clock_out, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(*clock_out, FONT_CLOCK, 0);
    lv_obj_set_style_text_letter_space(*clock_out, 2, 0);

    /* 下行：周几日期 — P151：FONT_LABEL(30)→FONT_ICON(36px) 放大，
     * COL_SEC 浅灰→COL_TEXT 深色（用户反馈"太淡"，与时间同深）；与时间
     * 水平居中（中心对齐）见函数尾部对齐补偿 */
    *date_out = lv_label_create(clk_cont);
    lv_label_set_text(*date_out, "周日 9月4日");
    lv_obj_set_style_text_color(*date_out, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(*date_out, FONT_ICON, 0);
    lv_obj_set_style_text_letter_space(*date_out, 1, 0);

    /* Center: weather — FONT_ICON, black text for readability */
    *weather_out = lv_label_create(bar);
    lv_label_set_text(*weather_out, "21\xc2\xb0""C  |  65%");
    lv_obj_set_style_text_color(*weather_out, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(*weather_out, FONT_ICON, 0);

    /* Right: icon row（P147：flex_grow(1) 与左侧对半分剩余空间，中间居中）。
     * P150：pad_column 16→DM(12) 拉开 WiFi/蓝牙/电池间距（用户反馈图标
     * 太密集贴电池）；icons 本身右对齐，pad_right 由 bar 提供。 */
    lv_obj_t *icons = make_clean_cont(bar);
    lv_obj_set_size(icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(icons, 1);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icons, DM(12), 0);

    lv_obj_t *wifi = lv_label_create(icons);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_text_font(wifi, FONT_ICON, 0);
    *wifi_out = wifi;

    lv_obj_t *bt = lv_label_create(icons);
    lv_label_set_text(bt, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(bt, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_text_font(bt, FONT_ICON, 0);
    *bt_out = bt;
    /* 2026-09-10 P206：删电池图标——桌面供电设备无电池，无电量计，
     * 恒满格绿图标是假数据，不如不显示。 */

    /* P151 对齐补偿（时间右移 + 日期与时间水平居中）：
     * clk_cont 被 flex-grow 拉伸（左缘~屏幕中点），column 交叉轴 START
     * 让两行贴容器左缘（P150 已闭环）。translate_x 是 flex 位置之上的
     * 偏移（lv_flex.c place_content 会重新应用 translate，无竞态）。
     * ① 时间右移 DM(8)≈19px（用户"往右挪一点点"）；
     * ② 日期比时间宽（"周日 9月4日"@36px ≈ 190px vs "00:00"@56px ≈ 150px），
     *    补偿 (tw-dw)/2 后两行水平中心对齐（用户"整体居中"）。
     * 垂直位置仍由 column 流保证（时间贴顶、日期在其下）。 */
    lv_obj_update_layout(clk_cont);
    int32_t tw = lv_obj_get_width(*clock_out);
    int32_t dw = lv_obj_get_width(*date_out);
    lv_obj_set_style_translate_x(*clock_out, DM(8), 0);
    lv_obj_set_style_translate_x(*date_out, DM(8) + (tw - dw) / 2, 0);

    /* P152：日期上移"贴住"上方时钟（用户反馈）。
     * column flex gap=0 时日期本已紧贴时间 label 底边，但 label 行高
     * （freetype line_height ≈ 1.4×字号）内嵌上/下空白，glyph 视觉上
     * 仍有间距。实测 date 顶 - time 底 = 视觉间距，translate_y 反向
     * 抵消 → 日期 glyph 顶部贴住时间 glyph 底部（两行合成一个时钟块，
     * 符合设计规范"时钟块=时间+日期一体"）。translate_y 同样会被
     * lv_flex.c place_content 在每次重排时重新应用，无竞态。 */
    int32_t vgap = lv_obj_get_y(*date_out)
                   - (lv_obj_get_y(*clock_out) + lv_obj_get_height(*clock_out));
    lv_obj_set_style_translate_y(*date_out, -vgap, 0);
}

/* ================================================================
 * CARD CREATION — generic card with frosted glass style
 * ================================================================ */

static lv_obj_t *create_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

/* ================================================================
 * WEATHER GLYPH — hand-drawn icons on a 64×64 canvas
 * (sun / cloud / rain / storm / snow / fog), inspired by
 * cyd-weather-station's custom glyphs. No font dependency.
 * ================================================================ */

#define WCV_SIZE 64

static void cv_px(lv_obj_t *cv, int x, int y, uint32_t color)
{
    lv_canvas_set_px(cv, x, y, lv_color_hex(color), LV_OPA_COVER);
}

static void cv_fill_circle(lv_obj_t *cv, int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                cv_px(cv, cx + dx, cy + dy, color);
}

static void cv_line(lv_obj_t *cv, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1)
        {
            cv_px(cv, x0, y0, color);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
}

static void cv_cloud(lv_obj_t *cv, int cy, uint32_t color)
{
    cv_fill_circle(cv, 22, cy, 9, color);
    cv_fill_circle(cv, 32, cy - 6, 12, color);
    cv_fill_circle(cv, 42, cy, 9, color);
}

static void draw_weather_glyph(lv_obj_t *cv, int code)
{
    lv_canvas_fill_bg(cv, lv_color_hex(0x000000), LV_OPA_TRANSP);

    if (code == 0)
        {
            /* Sun: disc + 8 rays */
            cv_fill_circle(cv, 32, 32, 13, 0xFF9500);
            for (int i = 0; i < 8; i++)
                {
                    int dx0, dy0, dx1, dy1;
                    switch (i)
                        {
                        case 0: dx0 = 18; dy0 = 0;  dx1 = 27; dy1 = 0;  break;
                        case 1: dx0 = 13; dy0 = 13; dx1 = 19; dy1 = 19; break;
                        case 2: dx0 = 0;  dy0 = 18; dx1 = 0;  dy1 = 27; break;
                        case 3: dx0 = -13;dy0 = 13; dx1 = -19;dy1 = 19; break;
                        case 4: dx0 = -18;dy0 = 0;  dx1 = -27;dy1 = 0;  break;
                        case 5: dx0 = -13;dy0 = -13;dx1 = -19;dy1 = -19;break;
                        case 6: dx0 = 0;  dy0 = -18;dx1 = 0;  dy1 = -27;break;
                        default:dx0 = 13; dy0 = -13;dx1 = 19; dy1 = -19;break;
                        }
                    cv_line(cv, 32 + dx0, 32 + dy0, 32 + dx1, 32 + dy1, 0xFF9500);
                }
        }
    else if (code >= 1 && code <= 3)
        {
            /* Cloudy */
            cv_cloud(cv, 38, 0xC7C7CC);
        }
    else if (code == 45 || code == 48)
        {
            /* Fog: cloud + flat lines */
            cv_cloud(cv, 36, 0xB0B0B8);
            cv_line(cv, 16, 50, 48, 50, 0x8E8E93);
            cv_line(cv, 22, 56, 44, 56, 0x8E8E93);
        }
    else if (code >= 51 && code <= 82)
        {
            /* Rain: cloud + drops */
            cv_cloud(cv, 36, 0xC7C7CC);
            for (int i = 0; i < 4; i++)
                {
                    int x = 18 + i * 9;
                    cv_line(cv, x, 48, x - 4, 58, 0x007AFF);
                }
        }
    else if (code >= 95)
        {
            /* Storm: cloud + bolt */
            cv_cloud(cv, 36, 0xC7C7CC);
            cv_line(cv, 30, 44, 36, 52, 0xFFCC00);
            cv_line(cv, 36, 52, 30, 56, 0xFFCC00);
            cv_line(cv, 30, 56, 38, 60, 0xFFCC00);
        }
    else if (code >= 71 && code <= 86)
        {
            /* Snow: cloud + flakes */
            cv_cloud(cv, 36, 0xC7C7CC);
            for (int i = 0; i < 3; i++)
                cv_fill_circle(cv, 20 + i * 12, 52, 2, 0x5AC8FA);
        }
    else
        {
            cv_cloud(cv, 38, 0xC7C7CC);
        }
}

/* iOS-style daily forecast — horizontal cards, one rounded tile per day */
static void weather_card_click_cb(lv_event_t *e);   /* 前向声明（定义在
                                                     * weather_kick_fetch 后） */
static void create_weather_card(lv_obj_t *card)
{
    /* 2026-09-10 P206 汉化：英文星期 Sun..Sat → 中文（状态栏已是中文） */
    static const char *wdays[] = { "周日", "周一", "周二", "周三",
                                   "周四", "周五", "周六" };

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_top(card, DM(14), 0);   /* push temp down a bit */

    /* Row 1: current temp (big) + description, one centered row */
    lv_obj_t *top_row = make_clean_cont(card);
    lv_obj_set_size(top_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_row, 16, 0);

    w_cur_temp_lbl = lv_label_create(top_row);
    lv_label_set_text(w_cur_temp_lbl, "--\xc2\xb0""C");
    lv_obj_set_style_text_color(w_cur_temp_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(w_cur_temp_lbl, FONT_TITLE, 0);

    w_cur_desc_lbl = lv_label_create(top_row);
    lv_label_set_text(w_cur_desc_lbl, "加载中…");
    lv_obj_set_style_text_color(w_cur_desc_lbl, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_text_font(w_cur_desc_lbl, FONT_LABEL, 0);

    /* Spacer then 5 day-tiles: rounded, light-bg, equal-width fill */
    lv_obj_t *sp = make_clean_cont(card);
    lv_obj_set_size(sp, DM(1), DM(1));
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t *tiles = make_clean_cont(card);
    lv_obj_set_size(tiles, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tiles, 12, 0);
    lv_obj_set_style_pad_top(tiles, 8, 0);

    for (int i = 0; i < DM_WEATHER_DAYS; i++) {
        lv_obj_t *tile = lv_obj_create(tiles);
        lv_obj_set_size(tile, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(tile, 1);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0xF2F2F7), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_60, 0);
        lv_obj_set_style_radius(tile, RAD_CARD, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_style_pad_row(tile, 6, 0);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        w_day_lbl[i] = lv_label_create(tile);
        lv_label_set_text(w_day_lbl[i], wdays[i]);
        lv_obj_set_style_text_font(w_day_lbl[i], FONT_BODY, 0);
        lv_obj_set_style_text_color(w_day_lbl[i], lv_color_hex(COL_TEXT), 0);

        /* Hand-drawn weather glyph on a 64×64 canvas */
        w_icon_cv[i] = lv_canvas_create(tile);
        lv_obj_set_size(w_icon_cv[i], WCV_SIZE, WCV_SIZE);
        lv_canvas_set_buffer(w_icon_cv[i], w_icon_buf[i],
                             WCV_SIZE, WCV_SIZE, LV_COLOR_FORMAT_ARGB8888);
        draw_weather_glyph(w_icon_cv[i], 3);

        w_temp_lbl[i] = lv_label_create(tile);
        lv_label_set_text(w_temp_lbl[i], "--\xc2\xb0/--\xc2\xb0");
        lv_obj_set_style_text_font(w_temp_lbl[i], FONT_BODY, 0);
        lv_obj_set_style_text_color(w_temp_lbl[i], lv_color_hex(COL_TEXT), 0);
    }

    /* 2026-08-10：点击整个天气卡 → 强制后台刷新 + 显示 API debug 信息 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, weather_card_click_cb, LV_EVENT_CLICKED, NULL);

    /* 2026-08-10 按压反馈（抄 ui_bt.c/ui_files.c 列表行 + music_round_btn
     * 观感）：按下轻微放大 + 背景变实 + 蓝色光晕，松开自动恢复白玻璃。 */
    lv_obj_set_style_transform_width(card, 3, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(card, 3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(card, 14, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(card, lv_color_hex(COL_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, LV_STATE_PRESSED);
}

/* ================================================================
 * WEATHER — 后台 fetch 线程（2026-08-10）
 * dm_weather_fetch 阻塞最多 5s（http 超时），不能占 UI 线程；
 * 仿 dm_net_scan_worker：worker 线程拉数据，UI 定时器只查标志。
 * ================================================================ */

static pthread_t        g_weather_thread;
static volatile int     g_weather_busy;   /* 1 = worker 运行中（防重入） */
static volatile int     g_weather_done;   /* 1 = 最新一轮 fetch 已结束 */
static volatile int     g_weather_ok;     /* 1 = 最新一轮 fetch 成功 */

static void *weather_fetch_worker(void *arg)
{
    (void)arg;
    g_weather_ok = (dm_weather_fetch(&g_weather) == 0);
    g_weather_busy = 0;
    g_weather_done = 1;
    /* 2026-08-10：把 API 对接结果打到串口，便于定位 FAIL 具体环节
     * （DNS/connect/recv/parse）。2026-08-23：UI 不再显示时延信息。 */
    LV_LOG_USER("weather fetch: %s", dm_weather_debug());
    return NULL;
}

/* 发起一轮后台 fetch（busy 时跳过，防并发扫描叠加）
 * 2026-09-10 去 static：Settings 城市切换后即时刷新（deskmate_ui.h 声明）。 */
void weather_kick_fetch(void)
{
    if (g_weather_busy)
        return;

    g_weather_busy = 1;
    g_weather_done = 0;
    if (pthread_create(&g_weather_thread, NULL, weather_fetch_worker,
                       NULL) != 0)
        {
            g_weather_busy = 0;
            g_weather_done = 1;
            g_weather_ok   = 0;
        }
    else
        {
            pthread_detach(g_weather_thread);
        }
}

/* 2026-08-10：点击天气卡 → 强制后台刷新。 */
static void weather_card_click_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("Weather card clicked: force refresh");
    weather_kick_fetch();
}

/* Refresh weather card + status bar from live data (called by lv_timer) */
static void weather_update_cb(lv_timer_t *timer)
{
    /* 2026-09-10 P206 汉化：同上 */
    static const char *wdays[] = { "周日", "周一", "周二", "周三",
                                   "周四", "周五", "周六" };

    /* worker 还在跑：不阻塞 UI，等下一轮 timer 再查 */
    if (g_weather_busy)
        return;

    /* 尚未有结果：发起 fetch，30s 后回来检查 */
    if (!g_weather_done)
        {
            weather_kick_fetch();
            if (timer)
                lv_timer_set_period(timer, 30000);
            return;
        }

    /* 上一轮失败：30s 快速重试（开机 WiFi 未就绪时首次必失败，
     * 联网后应立即拉到真数据） */
    if (!g_weather_ok)
        {
            /* 2026-08-10：失败 30s 快速重试（开机 WiFi 未就绪时首次必失败，
             * 联网后应立即拉到真数据）；UI 不再显示 debug 信息。 */
            weather_kick_fetch();
            if (timer)
                lv_timer_set_period(timer, 30000);
            return;
        }

    /* 成功：更新天气卡，恢复 2 小时周期，并预取下一轮（P113 原 4h，
     * 2026-08-23 用户拍板：开机同步一次后每 2h 更新即可） */
    if (timer)
        lv_timer_set_period(timer, 7200000);

    /* P103 说明：天气成功 = HTTP Date 对时已完成。
     * Step 5 修正：不再补播欢迎语（坐下已立即播，不依赖对时）。 */

    /* P145：天气卡加「室外」前缀，与状态栏室内传感器温湿度区分 */
    lv_label_set_text_fmt(w_cur_temp_lbl, "室外 %d\xc2\xb0""C",
                          g_weather.cur_temp);
    lv_label_set_text_fmt(w_cur_desc_lbl, "%s  |  %d%%",
                          dm_wmo_text(g_weather.cur_code),
                          g_weather.cur_humid);

    for (int i = 0; i < g_weather.n_days && i < DM_WEATHER_DAYS; i++) {
        int wd = g_weather.days[i].wday;
        if (wd < 0 || wd > 6)
            wd = 0;
        lv_label_set_text(w_day_lbl[i], wdays[wd]);
        draw_weather_glyph(w_icon_cv[i], g_weather.days[i].code);
        lv_label_set_text_fmt(w_temp_lbl[i], "%d\xc2\xb0 / %d\xc2\xb0",
                              g_weather.days[i].tmax,
                              g_weather.days[i].tmin);
    }

    /* 2026-08-10：状态栏温湿度改由传感器驱动（luncher_dm.c update_sensor_cb），
     * 本函数只更新天气卡（温湿度 = 天气 API）——两处数据源分开，勿再改回。 */

    /* 预取下一轮（2 小时后 timer 回来时结果已就绪，直接刷新） */
    weather_kick_fetch();
    dm_pet_on_event_param(PET_EVT_WEATHER_CHANGE, g_weather.cur_code);
}

/* ================================================================
 * HOME HEALTH CARD — 喝水/久坐双小圈（2026-08-16 P103 替换原 AI 静态卡）
 * 与锁屏双圈同数据源（dm_health_* API），配色跟随反色：喝水橙、久坐蓝。
 * 刷新由 home_health_rings_refresh（RING 事件同调）驱动。
 * ================================================================ */

/* P103：HOME 健康卡双小圈（声明在函数前，供 create_home_health_card 与
 * home_health_rings_refresh 共用） */
static lv_obj_t *home_water_ring;
static lv_obj_t *home_sit_ring;
static lv_obj_t *home_water_lbl;
static lv_obj_t *home_sit_lbl;
static lv_timer_t *home_water_flash_timer;   /* P180：打卡金色闪烁恢复 timer */

/* P180：喝水多入口——点击 HOME 喝水圈 / 锁屏"喝水 N"行即打卡。
 * 打卡走 dm_health_water_drank（弹窗左键同款）：计数+1、计时重置、
 * 发 DISMISS 收弹窗 + RING 刷新（双圈/健康卡瞬时 +1）；
 * 反馈 = bg_reward_std 短确认音 + HOME 喝水圈金色闪烁 1.2s。
 * 锁屏点击会冒泡到 standby_click_cb → 唤醒（打卡同时亮屏，符合直觉）。 */
static void home_water_flash_back(lv_timer_t *t)
{
    lv_timer_del(t);
    home_water_flash_timer = NULL;
    if (home_water_ring)
    {
        lv_obj_set_style_arc_color(home_water_ring, lv_color_hex(COL_ORANGE),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(home_water_ring, lv_color_hex(COL_ORANGE),
                                  LV_PART_KNOB);
    }
}

static void water_tap_cb(lv_event_t *e)
{
    (void)e;
    dm_health_water_drank();
    dm_tone_play("bg_reward_std");
    if (home_water_ring)
    {
        lv_obj_set_style_arc_color(home_water_ring, lv_color_hex(0xFFB000),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(home_water_ring, lv_color_hex(0xFFB000),
                                  LV_PART_KNOB);
        if (home_water_flash_timer)
            lv_timer_del(home_water_flash_timer);
        home_water_flash_timer = lv_timer_create(home_water_flash_back, 1200,
                                                 NULL);
        lv_timer_set_repeat_count(home_water_flash_timer, 1);
    }
}

static void create_home_health_card(lv_obj_t *card)
{
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, DM(24), 0);

    /* 喝水小圈（左）——背景浅灰 + 指示器橙 + knob 橙。
     * P180：整圈可点 → 打卡喝水（water_tap_cb） */
    lv_obj_t *w_cont = make_clean_cont(card);
    lv_obj_set_size(w_cont, DM(88), DM(110));
    lv_obj_set_flex_flow(w_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(w_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(w_cont, water_tap_cb, LV_EVENT_CLICKED, NULL);

    home_water_ring = lv_arc_create(w_cont);
    lv_obj_set_size(home_water_ring, DM(76), DM(76));
    lv_arc_set_bg_angles(home_water_ring, 0, 360);
    lv_arc_set_angles(home_water_ring, 0, 360);
    lv_obj_set_style_arc_color(home_water_ring, lv_color_hex(0xE5E5EA),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(home_water_ring, lv_color_hex(COL_ORANGE),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(home_water_ring, lv_color_hex(COL_ORANGE),
                              LV_PART_KNOB);
    lv_obj_set_style_arc_opa(home_water_ring, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(home_water_ring, LV_OPA_COVER,
                             LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(home_water_ring, 8, 0);
    lv_obj_remove_flag(home_water_ring, LV_OBJ_FLAG_CLICKABLE);

    home_water_lbl = lv_label_create(home_water_ring);
    lv_label_set_text(home_water_lbl, "--");
    lv_obj_set_style_text_font(home_water_lbl, FONT_ICON, 0);
    lv_obj_set_style_text_color(home_water_lbl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(home_water_lbl);

    lv_obj_t *w_cap = lv_label_create(w_cont);
    lv_label_set_text(w_cap, "喝水");
    lv_obj_set_style_text_font(w_cap, FONT_BODY, 0);
    lv_obj_set_style_text_color(w_cap, lv_color_hex(COL_SEC), 0);

    /* 久坐小圈（右）——背景浅灰 + 指示器蓝 + knob 蓝 */
    lv_obj_t *s_cont = make_clean_cont(card);
    lv_obj_set_size(s_cont, DM(88), DM(110));
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    home_sit_ring = lv_arc_create(s_cont);
    lv_obj_set_size(home_sit_ring, DM(76), DM(76));
    lv_arc_set_bg_angles(home_sit_ring, 0, 360);
    lv_arc_set_angles(home_sit_ring, 0, 360);
    lv_obj_set_style_arc_color(home_sit_ring, lv_color_hex(0xE5E5EA),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(home_sit_ring, lv_color_hex(COL_BLUE),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(home_sit_ring, lv_color_hex(COL_BLUE),
                              LV_PART_KNOB);
    lv_obj_set_style_arc_opa(home_sit_ring, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(home_sit_ring, LV_OPA_COVER,
                             LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(home_sit_ring, 8, 0);
    lv_obj_remove_flag(home_sit_ring, LV_OBJ_FLAG_CLICKABLE);

    home_sit_lbl = lv_label_create(home_sit_ring);
    lv_label_set_text(home_sit_lbl, "--");
    lv_obj_set_style_text_font(home_sit_lbl, FONT_ICON, 0);
    lv_obj_set_style_text_color(home_sit_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(home_sit_lbl);

    lv_obj_t *s_cap = lv_label_create(s_cont);
    lv_label_set_text(s_cap, "久坐");
    lv_obj_set_style_text_font(s_cap, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_cap, lv_color_hex(COL_SEC), 0);

    home_health_rings_refresh();
}

/* ================================================================
 * HOME MUSIC WIDGET — 首页常驻音乐小组件（2026-08-08 后台化）
 * 显示当前歌曲名 + 播放状态 + 控制按钮（上一曲/播放暂停/下一曲）。
 * 点击小组件进入完整播放器页面。music_playing/music_resume/
 * music_pause/music_album_next 驱动真实播放，与播放器页共享状态。
 * ================================================================ */

static void home_music_open_cb(lv_event_t *e)
{
    (void)e;
    show_subpage("Music");
}

static lv_obj_t *create_home_music_widget(lv_obj_t *parent)
{
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t card_w = sw - 2 * EDGE_PAD;

    lv_obj_t *card = create_card(parent, card_w, DM(56));
    lv_obj_add_event_cb(card, home_music_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 12, 0);
    lv_obj_set_style_pad_left(card, DM(12), 0);
    lv_obj_set_style_pad_right(card, DM(8), 0);

    /* 音乐图标（蓝底圆角方块） */
    lv_obj_t *icon = lv_obj_create(card);
    lv_obj_set_size(icon, DM(36), DM(36));
    lv_obj_set_style_radius(icon, 10, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_grad_color(icon, lv_color_hex(COL_PURPLE), 0);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_t *icon_lbl = lv_label_create(icon);
    lv_label_set_text(icon_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(icon_lbl, FONT_ICON, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(icon_lbl);

    /* 歌名（flex-grow 占满，超长省略） */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "音乐");
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, lv_pct(100));
    home_music_title_lbl = title;

    /* 控制按钮：上一曲 / 播放暂停 / 下一曲 */
    lv_obj_t *prev_btn = music_round_btn(card, DM(36), LV_SYMBOL_PREV,
                                         music_prev_cb, NULL);
    (void)prev_btn;

    lv_obj_t *play_btn = music_play_btn_create(card, DM(42),
                                               &home_music_play_icon);
    (void)play_btn;

    lv_obj_t *next_btn = music_round_btn(card, DM(36), LV_SYMBOL_NEXT,
                                         music_next_cb, NULL);
    (void)next_btn;

    home_music_card = card;
    home_music_visible = true;
    return card;
}

/* ================================================================
 * APP DOCK — bottom application launcher bar
 * ================================================================ */

static void create_app_dock(lv_obj_t *dock_area)
{
    /* 2026-09-10 P206：游戏下掉——模拟器/NES 超 9.20 窗口，手写小游戏
     * 不加分不做；Dock 6 入口，不留死按钮（见 ok-20260910-5/6 来回）。 */
    static const struct deskmate_app apps[] = {
        { "music",    "音乐",    &dm_icon_music },
        { "calendar", "日历",    &dm_icon_calendar },
        { "books",    "书籍",    &dm_icon_books },
        { "files",    "文件",    &dm_icon_files },
        { "ai",       "AI对话",  &dm_icon_ai },
        { "settings", "设置",    &dm_icon_settings },
        { "lock",     "锁屏",    &dm_icon_lock },
    };
    int num = sizeof(apps) / sizeof(apps[0]);

    /* Dock background: light frosted pill */
    lv_obj_t *dock_bg = lv_obj_create(dock_area);
    lv_obj_set_style_bg_color(dock_bg, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(dock_bg, LV_OPA_60, 0);
    lv_obj_set_style_border_width(dock_bg, 0, 0);
    lv_obj_set_style_radius(dock_bg, RAD_CARD, 0);
    lv_obj_set_style_shadow_width(dock_bg, 8, 0);
    lv_obj_set_style_shadow_color(dock_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(dock_bg, LV_OPA_10, 0);
    lv_obj_set_style_pad_left(dock_bg, 20, 0);
    lv_obj_set_style_pad_right(dock_bg, 20, 0);
    lv_obj_set_style_pad_top(dock_bg, 8, 0);
    lv_obj_set_style_pad_bottom(dock_bg, 8, 0);
    lv_obj_set_style_pad_column(dock_bg, DOCK_ICON_GAP, 0);
    lv_obj_set_scrollbar_mode(dock_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(dock_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dock_bg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dock_bg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock_bg, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(dock_bg);

    for (int i = 0; i < num; i++) {
        /* Each app: column with icon button + label */
        lv_obj_t *item = make_clean_cont(dock_bg);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(item, 2, 0);

        /* Icon button — 浅色底遮住图标边缘锯齿 */
        lv_obj_t *btn = lv_btn_create(item);
        lv_obj_set_size(btn, DOCK_ICON_SIZE, DOCK_ICON_SIZE);
        lv_obj_set_style_radius(btn, RAD_CARD, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_100, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_user_data(btn, (void *)apps[i].name);

        /* Icon — PNG from icon pack (deskmate_icons.c) */
        lv_obj_t *icon = lv_image_create(btn);
        lv_image_set_src(icon, apps[i].icon);
        lv_obj_center(icon);

        /* Label below icon — FONT_LABEL, black text for readability */
        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, apps[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(lbl, FONT_LABEL, 0);

        /* Events & navigation */
        lv_obj_add_event_cb(btn, app_icon_click_cb, LV_EVENT_CLICKED, NULL);
        if (g_group)
            lv_group_add_obj(g_group, btn);
    }

    LV_LOG_USER("Dock: %d apps", num);
}

/* ================================================================
 * STANDBY / LOCK SCREEN
 * ================================================================ */

static void show_standby(void);
static void wake_from_standby(void);

/* ── 2026-08-15 健康助理：锁屏双圈（喝水/久坐）+ AI 语音圈 ── */
static lv_obj_t *standby_water_ring;
static lv_obj_t *standby_sit_ring;
static lv_obj_t *standby_water_lbl;
static lv_obj_t *standby_sit_lbl;

static lv_obj_t *standby_ai_btn;
static lv_obj_t *standby_h1_lbl;   /* 健康卡：今日在座时长 */
static lv_obj_t *standby_h2_lbl;   /* 健康卡：今日喝水次数 */
static lv_obj_t *standby_h3_lbl;   /* 健康卡：今日起立次数 */
static lv_obj_t *standby_prox_lbl; /* 锁屏人体读数标签（2026-08-27 review-15 定稿） */
static char       g_prox_lbl_text[32];  /* 2026-09-01 乱码修复：上次文本缓存（变化才 set_text） */
static lv_obj_t *standby_lux_lbl;  /* P153：锁屏环境光亮度显示（人体 label 右侧） */
static char       g_lux_lbl_text[24];   /* P153：亮度文本去重缓存（P144 同款防 glyph 乱码） */

/* ── 2026-08-28 Step 4：欢迎回来卡（V1 动态数据走 UI，坐下显示 5s 自动隐藏）── */
static lv_obj_t *standby_welcome_card;
static lv_obj_t *standby_welcome_lbl;
static time_t    g_welcome_show_ts;

static void welcome_card_show(void)
{
    char buf[64];

    if (!standby_welcome_card)
        return;
    snprintf(buf, sizeof(buf), "欢迎回来 今日坐姿 %d 分钟 喝水 %d 次",
             (int)(dm_health_seat_s() / 60), dm_health_water_count());
    lv_label_set_text(standby_welcome_lbl, buf);
    lv_obj_clear_flag(standby_welcome_card, LV_OBJ_FLAG_HIDDEN);
    g_welcome_show_ts = time(NULL);
    LV_LOG_USER("[voice] welcome card shown");
}

static void welcome_card_tick(void)
{
    if (standby_welcome_card &&
        !lv_obj_has_flag(standby_welcome_card, LV_OBJ_FLAG_HIDDEN) &&
        time(NULL) - g_welcome_show_ts >= 5)
        lv_obj_add_flag(standby_welcome_card, LV_OBJ_FLAG_HIDDEN);
}

/* 刷新双圈：剩余分钟数字 + 弧长（剩余比例）。未在座显示 "--" 空环。 */
static void standby_health_rings_refresh(void)
{
    char buf[24];

    if (!standby_water_ring || !standby_sit_ring)
        return;

    if (!dm_health_is_seated())
    {
        lv_arc_set_angles(standby_water_ring, 0, 0);
        lv_arc_set_angles(standby_sit_ring, 0, 0);
        lv_label_set_text(standby_water_lbl, "--");
        lv_label_set_text(standby_sit_lbl, "--");
        goto refresh_cards;
    }

    int wr = dm_health_water_remain_s();
    int wt = dm_health_water_total_s();
    int sr = dm_health_sit_remain_s();
    int st = dm_health_sit_total_s();

    lv_snprintf(buf, sizeof(buf), "%d'", (wr + 59) / 60);
    lv_label_set_text(standby_water_lbl, buf);
    lv_arc_set_angles(standby_water_ring, 0, wt > 0 ? (360 * wr) / wt : 0);

    lv_snprintf(buf, sizeof(buf), "%d'", (sr + 59) / 60);
    lv_label_set_text(standby_sit_lbl, buf);
    lv_arc_set_angles(standby_sit_ring, 0, st > 0 ? (360 * sr) / st : 0);

refresh_cards:
    /* 底部健康卡：真实今日计数（在座时长/喝水/起立） */
    if (standby_h1_lbl)
    {
        long seat_s = dm_health_seat_s();
        /* P103：加"在座"标签明确语义；HH:MM:SS 带秒——秒级跳动可见，
         * 坐下即开始累计（原 HH:MM 秒级变化不可见，被误认为没累计） */
        lv_snprintf(buf, sizeof(buf), LV_SYMBOL_BELL " 在座 %02ld:%02ld:%02ld",
                    seat_s / 3600, (seat_s % 3600) / 60, seat_s % 60);
        lv_label_set_text(standby_h1_lbl, buf);
    }
    if (standby_h2_lbl)
    {
        lv_snprintf(buf, sizeof(buf), LV_SYMBOL_TINT "  喝水 %d",
                    dm_health_water_count());
        lv_label_set_text(standby_h2_lbl, buf);
    }
    if (standby_h3_lbl)
    {
        lv_snprintf(buf, sizeof(buf), LV_SYMBOL_UP "  起立 %d",
                    dm_health_stand_count());
        lv_label_set_text(standby_h3_lbl, buf);
    }

    /* 2026-08-27：人体感应状态 + 距离（LD2410B，P124 迁 /dev/uart3）
     * P127：dist>=0 路径补回 lv_label_set_text（原只 snprintf 未写入，
     * label 停留初始"人体：--"不显示距离）
     * 2026-09-01 乱码修复：文本未变化时跳过 set_text——RING 每秒无条件
     * set_text 触发 label 重绘，配合 FreeType glyph cache LRU 淘汰，被
     * 淘汰字形重建竞态导致「体」字渲染错乱（P126 遗留，P128 只验证了
     * 距离显示未验证乱码）。去重后文本稳定时不重绘，缓存抖动消失。 */
    if (standby_prox_lbl)
    {
        int p = dm_ld2410b_present();
        int dist = dm_ld2410b_get_dist_cm();
        char text[32];
        if (p < 0)
            lv_snprintf(text, sizeof(text), "人体：不可用");
        else if (p == 0)
            lv_snprintf(text, sizeof(text), "人体：无人");
        else
        {
            /* 有人：附带探测距离（0.75m 距离门分辨率，显示到 0.1m；
             * 2026-08-28：dist=0 = 目标在最近距离门内（<0.75m），
             * 也必须显示距离——原 dist>0 判断会让它退化成"人体：有人"；
             * 显示用整数拼接（LVGL builtin lv_snprintf 不支持 %f，
             * CONFIG_LV_USE_FLOAT 未启用——温度显示同款惯例） */
            if (dist >= 0)
                lv_snprintf(text, sizeof(text), "人体：有人 %d.%dm",
                            dist / 100, (dist % 100) / 10);
            else
                lv_snprintf(text, sizeof(text), "人体：有人");
        }
        if (!g_prox_lbl_text[0] || strcmp(g_prox_lbl_text, text) != 0)
        {
            lv_label_set_text(standby_prox_lbl, text);
            lv_strlcpy(g_prox_lbl_text, text, sizeof(g_prox_lbl_text));
        }
    }

    /* P153：锁屏环境光（LTR553 ALS）——文本去重（P144 同款防 glyph 乱码）；
     * lux<0（无传感器）显示 "光感 -- lux" */
    if (standby_lux_lbl)
    {
        char lux_text[32];
        int lux = dm_als_get_lux();
        if (lux < 0)
            lv_snprintf(lux_text, sizeof(lux_text), "光感 -- lux");
        else
            lv_snprintf(lux_text, sizeof(lux_text), "光感 %d lux", lux);
        if (strcmp(g_lux_lbl_text, lux_text) != 0)
        {
            lv_label_set_text(standby_lux_lbl, lux_text);
            lv_strlcpy(g_lux_lbl_text, lux_text, sizeof(g_lux_lbl_text));
        }
    }
}

/* P103：HOME 健康卡双小圈刷新（与锁屏同数据源，RING 事件每秒驱动）。
 * 未在座显示 "--" 空环；在座显示剩余分钟 + 弧长。 */
static void home_health_rings_refresh(void)
{
    char buf[16];

    if (!home_water_ring || !home_sit_ring)
        return;

    if (!dm_health_is_seated())
    {
        lv_arc_set_angles(home_water_ring, 0, 0);
        lv_arc_set_angles(home_sit_ring, 0, 0);
        lv_label_set_text(home_water_lbl, "--");
        lv_label_set_text(home_sit_lbl, "--");
        return;
    }

    int wr = dm_health_water_remain_s();
    int wt = dm_health_water_total_s();
    int sr = dm_health_sit_remain_s();
    int st = dm_health_sit_total_s();

    lv_snprintf(buf, sizeof(buf), "%d'", (wr + 59) / 60);
    lv_label_set_text(home_water_lbl, buf);
    lv_arc_set_angles(home_water_ring, 0, wt > 0 ? (360 * wr) / wt : 0);

    lv_snprintf(buf, sizeof(buf), "%d'", (sr + 59) / 60);
    lv_label_set_text(home_sit_lbl, buf);
    lv_arc_set_angles(home_sit_ring, 0, st > 0 ? (360 * sr) / st : 0);
}

/* ── 2026-08-15 健康助理：提醒弹窗（玻璃卡 + 标题 + 两键）──
 * 挂 lv_scr_act() 顶层（FLOATING + move_foreground），锁屏/主屏都可覆盖；
 * 不挂 standby_overlay，避免与 standby_click_cb（点击唤醒）冲突。
 * 按键动作直接调 dm_health（计数/升档），由 dm_health 事件驱动收起。 */
static lv_obj_t *health_popup;        /* 弹窗容器 */
static lv_obj_t *health_popup_icon;   /* 图标 label（随 kind 切换） */
static lv_obj_t *health_popup_title;  /* 提醒文案 */
static lv_obj_t *health_popup_left;   /* 左键：我喝了/我起来了 */
static lv_obj_t *health_popup_right;  /* 右键：先不喝/继续坐 */
static lv_obj_t *health_popup_btnrow; /* 按钮行（dm_popup_fit 实测补高用） */
static int       health_popup_kind;   /* 0=喝水 1=久坐 */

static void health_popup_hide(void)
{
    if (health_popup)
        lv_obj_add_flag(health_popup, LV_OBJ_FLAG_HIDDEN);
}

/* P111：弹窗排队——同一时刻只显示一个提醒弹窗，后到的先挂起，
 * 当前弹窗关闭后再补显，修"喝水/久坐同 tick 同时触发 → 第二个弹窗
 * 覆盖第一个（health_popup_kind 被改写）→ 被覆盖的提醒 remain=0
 * 永远等不到按钮、倒计时冻结"。 */
static int g_popup_pending_kind = -1;   /* -1=无挂起；0=水 1=坐 */
static int g_popup_pending_level = 0;

/* 提醒文案（cfg 可配；弹窗与排队补显共用同一来源，避免两处维护） */
static const char *health_popup_text(int kind, int level)
{
    if (kind == 0)
        return dm_health_cfg_get_str(level >= 3 ? "popup_water_3" :
                                     level == 2 ? "popup_water_2" : "popup_water_1",
            level >= 3 ? "水都凉啦，先喝两口吧。" :
            level == 2 ? "水还没喝呢，记得呀。" : "喝口水吧~");
    return dm_health_cfg_get_str(level >= 3 ? "popup_sit_3" :
                                 level == 2 ? "popup_sit_2" : "popup_sit_1",
        level >= 3 ? "腰在抗议啦，起来活动下。" :
        level == 2 ? "坐得有点久了，动一动呀。" : "起来走走嘛。");
}

/* 弹窗关闭后补显挂起的提醒；被覆盖的提醒状态已变（按钮按下/离开已
 * 重置）则跳过——level>0 且 remain==0 才是"正等按钮"状态 */
static void health_popup_show(int kind, int level, const char *text); /* 前向声明（定义在下方） */
static void health_popup_flush_pending(void)
{
    int kind = g_popup_pending_kind;
    int level = g_popup_pending_level;

    if (kind < 0)
        return;
    g_popup_pending_kind = -1;
    g_popup_pending_level = 0;
    int waiting = (kind == 0) ? (dm_health_water_level() > 0 && dm_health_water_remain_s() == 0)
                              : (dm_health_sit_level()  > 0 && dm_health_sit_remain_s()  == 0);
    if (!waiting)
        return;
    health_popup_show(kind, level, health_popup_text(kind, level));
}

static void health_popup_left_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    if (health_popup_kind == 0)
        dm_health_water_drank();
    else
        dm_health_sit_stood();
    /* 收起交给 DM_HEALTH_EVT_DISMISS（内部 hide + 补显挂起提醒） */
}

static void health_popup_right_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    if (health_popup_kind == 0)
        dm_health_water_snooze();
    else
        dm_health_sit_snooze();
    /* 收起交给 DM_HEALTH_EVT_DISMISS（内部 hide + 补显挂起提醒） */
}

/* 弹窗：kind 0=水 1=坐；level 1..3（L2/L3 放大 + AI 语音播报） */
static void health_popup_show(int kind, int level, const char *text)
{
    /* P111：已有弹窗在显示 → 挂起，不覆盖（否则 kind 被改写、被覆盖的
     * 提醒 remain=0 永远等不到按钮，倒计时冻结） */
    if (health_popup && !lv_obj_has_flag(health_popup, LV_OBJ_FLAG_HIDDEN))
    {
        g_popup_pending_kind = kind;
        g_popup_pending_level = level;
        return;
    }

    if (!health_popup)
    {
        /* 首次创建（P86 教训：align 放函数末尾统一执行），Liquid Glass 统一弹窗语言 */
        health_popup = dm_popup_card(lv_scr_act(), DM(560));

        health_popup_icon = dm_popup_icon(health_popup, LV_SYMBOL_TINT, COL_ORANGE);
        health_popup_title = dm_popup_title(health_popup, "");

        lv_obj_t *btns = dm_popup_btns(health_popup);
        health_popup_btnrow = btns;
        health_popup_left = dm_popup_btn(btns, "", 0, health_popup_left_cb);
        health_popup_right = dm_popup_btn(btns, "", 1, health_popup_right_cb);
    }

    health_popup_kind = kind;
    lv_label_set_text(health_popup_title, text);

    /* 图标标识随 kind 切换：喝水=水滴(橙) 久坐=铃铛(蓝)，与首页双圈配色一致 */
    if (kind == 0)
        dm_popup_icon_set(health_popup_icon, LV_SYMBOL_TINT, COL_ORANGE);
    else
        dm_popup_icon_set(health_popup_icon, LV_SYMBOL_BELL, COL_BLUE);

    lv_obj_t *l_l = lv_obj_get_user_data(health_popup_left);
    lv_obj_t *l_r = lv_obj_get_user_data(health_popup_right);
    if (l_l)
        lv_label_set_text(l_l, kind == 0 ? "我喝了" : "我起来了");
    if (l_r)
        lv_label_set_text(l_r, kind == 0 ? "先不喝" : "继续坐");

    /* L2/L3 升级：弹窗更宽 + AI 语音播报（直接 TTS，不走 PTT 录音） */
    if (level >= 2)
        lv_obj_set_size(health_popup, DM(720), LV_SIZE_CONTENT);
    else
        lv_obj_set_size(health_popup, DM(560), LV_SIZE_CONTENT);
    lv_obj_clear_flag(health_popup, LV_OBJ_FLAG_HIDDEN);
    /* P232：自适应补高（与暗光弹窗同病根：改文案/改宽后 CONTENT 高度需重测，
     * 否则两颗按钮被裁一半）→ 撑足后再居中 */
    dm_popup_fit(health_popup, health_popup_btnrow);
    lv_obj_move_foreground(health_popup);
    lv_obj_center(health_popup);

    /* P102（2026-08-16）：本地提示音 wav 播放（/resource/tones/，预合成），
     * 不依赖云端 TTS；wav 缺失时回退 dm_ai_voice_speak 兜底。
     * 2026-09-01 P142：前置"标准"背景音乐（bg_reward_std）开场音效，
     * 再播语音提示——避免突然人声惊吓。 */
    {
        char tone[48];
        snprintf(tone, sizeof(tone), "popup_%s_%d",
                 kind == 0 ? "water" : "sit",
                 level > 3 ? 3 : level);
        dm_tone_play_seq("bg_reward_std", tone);
        voice_director_on_reminder(kind, level);   /* Step 3：提醒历史/预算记录 */
    }

    LV_LOG_USER("[health] popup kind=%d level=%d: %s", kind, level, text);
}

/* ── 2026-08-28 Step 3/5：坐下播报移交 Voice Director（dm_voice.c）──
 * 2026-08-28 Step 5 修正：去掉"等对时"defer——坐下立即按算法决策播本地
 * wav（不依赖 WiFi/对时）；时间不可信由 dm_voice.c 内部处理（hour=-1）。 */

/* 旧 health_sit_greeting 函数已删除（2026-08-28 Step 3）——
 * 坐下播报逻辑由 voice_director_on_seated()（dm_voice.c）接管 */

/* P103 catchup（health_greet_maybe_catchup）已于 Step 5 删除——
 * 坐下不再等对时，本地 wav 立即播；对时只影响时段场景精度 */

/* dm_health 事件回调（每秒 RING + 弹窗/离开事件） */
static void standby_health_cb(int evt, int arg)
{
    switch (evt)
    {
    case DM_HEALTH_EVT_RING:
        standby_health_rings_refresh();
        home_health_rings_refresh();   /* P103：HOME 健康卡双小圈同刷 */
        welcome_card_tick();           /* Step 4：欢迎卡 5s 自动隐藏 */
        break;
    case DM_HEALTH_EVT_PRESENCE:
        if (arg == 0)
        {
            health_popup_hide();      /* 离开：收起弹窗（dm_health 已清零） */
            g_popup_pending_kind = -1; /* P111：离开清零，挂起的提醒作废 */
        }
        else
        {
            /* Step 5 修正：坐下立即按算法决策播本地 wav（不依赖对时/网络）；
             * 时间不可信（未对时）由 dm_voice.c 内部退化时段场景 */
            voice_director_on_seated();
            dm_pet_on_event(PET_EVT_PRESENCE);   /* P1：有人坐下 → 猫好奇看过来 */
            welcome_card_show();   /* Step 4：坐下显示欢迎回来卡（动态数据走 UI） */
        }
        standby_health_rings_refresh();
        break;
    case DM_HEALTH_EVT_WATER_POP:
        health_popup_show(0, arg, health_popup_text(0, arg));
        break;
    case DM_HEALTH_EVT_SIT_POP:
        health_popup_show(1, arg, health_popup_text(1, arg));
        dm_pet_on_event(PET_EVT_SIT_ALERT);   /* P1：久坐提醒 → 猫担忧 */
        break;
    case DM_HEALTH_EVT_DISMISS:
        health_popup_hide();          /* P111：收起后补显挂起的下一个提醒 */
        health_popup_flush_pending();
        break;
    case DM_HEALTH_EVT_WATER_6:
        /* 2026-09-01 P142：今日喝满 6 次水 → 豪华背景音乐 + 夸奖语音
         * （夸奖语音暂用 care_01.wav 占位，可换 tts_proto 合成专用） */
        dm_tone_play_seq("bg_reward_lux", "care_01");
        dm_pet_on_event(PET_EVT_WATER_GOAL);   /* P1：喝水达标 → 猫庆祝 */
        break;
    default:
        break;
    }
}

/* ── P153 暗光开灯提醒弹窗（LTR553 ALS）──────────────
 * 独立于 health_popup（不掺和喝水/久坐排队）：dm_als 暗光状态机触发
 * 回调 → 仅有人才弹（LD2410B 无人不打扰，判定在 dm_als 状态机内）→
 * 单按钮"知道了" + 60s 自动消失。挂 lv_scr_act() 顶层 FLOATING +
 * move_foreground，锁屏/主屏可盖。 */
static lv_obj_t  *dark_popup;        /* 弹窗容器（懒创建） */
static lv_obj_t  *dark_popup_btnrow; /* 按钮行（自适应补高实测用） */
static lv_timer_t *dark_popup_timer; /* 60s 自动消失 timer */

static void dark_popup_hide(void)
{
    if (dark_popup)
        lv_obj_add_flag(dark_popup, LV_OBJ_FLAG_HIDDEN);
    if (dark_popup_timer) {
        lv_timer_delete(dark_popup_timer);
        dark_popup_timer = NULL;
    }
}

static void dark_popup_ok_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dark_popup_hide();
}

static void dark_popup_auto_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    dark_popup_hide();
}

/* dm_als 暗光回调：环境光 <1 lux（==0）+ 真人在场 持续2分钟 → 弹窗提醒开灯 */
static void dark_popup_trigger_cb(void)
{
    LV_LOG_USER("[dark] cb enter popup=%p", dark_popup);

    if (!dark_popup) {
        /* 懒创建（2026-09-14 统一 Liquid Glass 弹窗语言）。
         * 旧太阳图标用 FONT_ICON(MiSans) 填 FA 字形 U+F185——字库无此字形不渲染，
         * 改 LV_SYMBOL_WARNING(FONT_SYMBOL=fa-solid 保证渲染) 琥珀警示。 */
        dark_popup = dm_popup_card(lv_scr_act(), DM(500));

        dm_popup_icon(dark_popup, LV_SYMBOL_WARNING, COL_ORANGE);
        dm_popup_title(dark_popup, "光线有点暗");
        dm_popup_text(dark_popup, "开个灯看得更清楚，也更护眼～");

        lv_obj_t *btn_row = dm_popup_btns(dark_popup);
        dark_popup_btnrow = btn_row;
        dm_popup_btn(btn_row, "知道了", 0, dark_popup_ok_cb);
    }

    lv_obj_clear_flag(dark_popup, LV_OBJ_FLAG_HIDDEN);
    dm_popup_fit(dark_popup, dark_popup_btnrow); /* P229/P232：实测补高，防按钮半显 */
    lv_obj_move_foreground(dark_popup);
    lv_obj_center(dark_popup);
    /* P164：暗光弹窗提示音——与健康提醒同款：bg_reward_std 背景 + 语音 */
    dm_tone_play_seq("bg_reward_std", "popup_dark_1");
    LV_LOG_USER("[dark] popup shown w=%d h=%d",
           lv_obj_get_width(dark_popup), lv_obj_get_height(dark_popup));

    /* 60s 自动消失（一次性 timer，回调里自删）；用户点"知道了"随时关 */
    if (!dark_popup_timer) {
        dark_popup_timer = lv_timer_create(dark_popup_auto_hide_cb, 60000, NULL);
        if (dark_popup_timer)
            lv_timer_set_repeat_count(dark_popup_timer, 1);
    }
}

static void idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 2026-09-10 P206：自动锁定=永不（g_idle_timeout=0）时直接返回——
     * 旧逻辑 countdown-- 到 -1<=0 照锁，唤醒1s又锁，"永不"反杀。 */
    if (g_idle_timeout <= 0)
        return;
    if (standby_overlay && !lv_obj_has_flag(standby_overlay, LV_OBJ_FLAG_HIDDEN)) {
        idle_countdown = g_idle_timeout;
        return;
    }
    /* 2026-08-27：子页（UART 调试工具等）打开时不锁屏——只
     * 有返回 HOME（subpage_overlay 为空）才重新 30s 倒计时锁屏 */
    if (subpage_overlay) {
        idle_countdown = g_idle_timeout;
        return;
    }
    idle_countdown--;
    if (idle_countdown <= 0)
        show_standby();
}

/* Phase 1 拆分：reset_idle_timer 被 ui_settings.c（Auto-Lock 回调）extern 调用 */
void reset_idle_timer(void)
{
    idle_countdown = g_idle_timeout;
}

static void wake_from_standby(void)
{
    if (!standby_overlay) return;
    if (dm_pet_is_enabled())
        dm_pet_on_event(PET_EVT_WAKE_UP);
    if (standby_ring)
        lv_anim_delete(standby_ring, NULL);
    lv_obj_add_flag(standby_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Restore dock only when no subpage is open — if a subpage is still
     * open, keep the dock hidden; close_subpage() will restore it later.
     * Otherwise the dock can peek through behind a subpage overlay. */
    if (g_dock && !subpage_overlay)
        lv_obj_clear_flag(g_dock, LV_OBJ_FLAG_HIDDEN);
    reset_idle_timer();
    LV_LOG_USER("Wake from standby");
}

static void standby_click_cb(lv_event_t *e)
{
    (void)e;
    wake_from_standby();
}

static void show_standby(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

    /* Hide dock — do NOT move_background() it: the screen uses flex
     * column layout, so reordering children would move the dock from
     * the bottom (after the status bar) up to the top of the screen.
     * standby_overlay is full-screen, opaque and move_foreground()ed,
     * so HIDDEN alone keeps the dock off the lock screen. */
    if (g_dock) lv_obj_add_flag(g_dock, LV_OBJ_FLAG_HIDDEN);

    /* Reuse existing overlay */
    if (standby_overlay) {
        lv_obj_clear_flag(standby_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(standby_overlay);
        /* 2026-08-15 健康助理：原呼吸环动画移除，改为双圈实时刷新 */
        standby_health_rings_refresh();
        clock_update_cb(NULL);
        /* 幂等：确保宠物仍挂在 overlay 上并停靠右下角（总开关关着不建） */
        if (dm_pet_is_enabled()) {
            dm_pet_create(standby_overlay);
            dm_pet_on_event(PET_EVT_SLEEP);
        }
        LV_LOG_USER("Show standby (reused)");
        return;
    }

    /* ── First-time creation ── */
    standby_overlay = lv_obj_create(scr);
    lv_obj_set_size(standby_overlay, sw, sh);
    lv_obj_set_style_bg_color(standby_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(standby_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(standby_overlay, 0, 0);
    lv_obj_set_style_radius(standby_overlay, 0, 0);
    lv_obj_set_style_pad_all(standby_overlay, 0, 0);
    lv_obj_set_scrollbar_mode(standby_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(standby_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* 2026-08-10 修复：scr 是 flex column，standby_overlay 若不 FLOATING
     * 会被 flex 重排导致盖不全子页（WiFi/蓝牙子页内容残留在锁屏界面）。
     * FLOATING 让其脱离 flex 流、全屏覆盖。 */
    lv_obj_add_flag(standby_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(standby_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(standby_overlay, standby_click_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(standby_overlay, standby_click_cb,
                        LV_EVENT_KEY, NULL);
    lv_obj_move_foreground(standby_overlay);

    /* ── 2026-08-28 Step 4：欢迎回来卡（坐下显示动态数据，5s 自动隐藏）──
     * Liquid Glass 样式同健康卡体系；初始隐藏，welcome_card_show 显示。 */
    standby_welcome_card = lv_obj_create(standby_overlay);
    lv_obj_set_size(standby_welcome_card, DM(760), DM(88));
    lv_obj_set_style_bg_color(standby_welcome_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(standby_welcome_card, LV_OPA_30, 0);
    lv_obj_set_style_border_width(standby_welcome_card, 0, 0);
    lv_obj_set_style_radius(standby_welcome_card, DM(44), 0);
    lv_obj_set_style_shadow_width(standby_welcome_card, DM(16), 0);
    lv_obj_set_style_shadow_color(standby_welcome_card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(standby_welcome_card, LV_OPA_20, 0);
    lv_obj_align(standby_welcome_card, LV_ALIGN_BOTTOM_MID, 0, -DM(350));
    lv_obj_add_flag(standby_welcome_card, LV_OBJ_FLAG_HIDDEN);
    standby_welcome_lbl = lv_label_create(standby_welcome_card);
    lv_label_set_text(standby_welcome_lbl, "欢迎回来");
    lv_obj_set_style_text_font(standby_welcome_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(standby_welcome_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(standby_welcome_lbl);

    /* ── 2026-08-10 锁屏状态栏：与 Home 顶部完全一致（132px 全宽）──
     * 复用 create_status_bar_ex：左时钟+中天气+右 wifi/bt/battery。
     * 指针由 clock_update_cb / weather_update_cb / dm_net_status_refresh
     * 刷新（standby_overlay 常驻不删除）。
     * -23 曾用 FLOATING+align+pad_top：lv_obj_align 是相对父容器「内容区」
     * （含 pad）定位，pad_top=132 把状态栏也拉到 y=132（用户反馈「状态栏
     * 被拉下来」），且 FLOATING 子元素被 flex 跳过（lv_flex.c 判 FLOATING
     * 跳过）。-24 改法：sbar 作为 flex column 的第一个普通子元素（不加
     * FLOATING），flex 从 y=0 顺序排布 → 状态栏固定顶部不动，其余内容
     * （光环/时钟/日期/音乐/健康卡）自然排在其下方、由 grow 间隔器在
     * 剩余空间内居中——状态栏不动、页面整体下移让位（skill 铁律2）。 */
    lv_obj_t *sbar = make_clean_cont(standby_overlay);
    lv_obj_set_size(sbar, lv_pct(100), TOP_BAR_H);
    lv_obj_set_style_bg_color(sbar, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(sbar, 0, 0);
    lv_obj_set_style_radius(sbar, 0, 0);
    create_status_bar_ex(sbar, &standby_bar_clock_lbl, &standby_bar_date_lbl,
                         &standby_bar_weather_lbl,
                         &standby_bar_wifi_lbl, &standby_bar_bt_lbl);

    /* Flex column: centered content */
    lv_obj_set_flex_flow(standby_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(standby_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(standby_overlay, 12, 0);

    /* Top spacer — 状态栏与双圈之间保底呼吸间距（P146：原无最小高度致
     * 双圈死顶状态栏；DM(24)≥skill 20px 留白规范）。P147：grow 3（原 1）
     * 与 sp_mid(grow 2) 按 3:2 分配剩余空间 → 中间内容组（双圈/AI/音乐）
     * 在状态栏与健康卡之间居中偏上，不紧贴健康卡也不挤状态栏。 */
    lv_obj_t *sp_top = make_clean_cont(standby_overlay);
    lv_obj_set_size(sp_top, DM(1), DM(24));
    lv_obj_set_flex_grow(sp_top, 3);

    /* ── 2026-08-15 健康助理：锁屏双圈 + AI 语音圈（原呼吸环移除）──
     * P145：原中间大时钟/日期已移除（时间/周几日期在左上角状态栏），
     * 双圈行紧跟状态栏下方。 */

    /* ── 双圈行：左喝水（蓝）右久坐（橙）── */
    lv_obj_t *rings_row = make_clean_cont(standby_overlay);
    lv_obj_set_size(rings_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rings_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rings_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rings_row, DM(60), 0);

    /* 喝水圈（左）——Liquid Glass 玻璃圆底（与健康卡/AI 圈材质统一）
     * 尺寸：DM(160)×DM(170) 防锁屏纵向溢出（固定内容 ≤1200px） */
    lv_obj_t *w_cont = make_clean_cont(rings_row);
    lv_obj_set_size(w_cont, DM(160), DM(170));
    lv_obj_set_style_bg_color(w_cont, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(w_cont, LV_OPA_60, 0);          /* 半透明白底 */
    lv_obj_set_style_border_color(w_cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(w_cont, 3, 0);            /* 半透明白边 */
    lv_obj_set_style_border_opa(w_cont, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(w_cont, 20, 0);           /* 柔和阴影 */
    lv_obj_set_style_shadow_opa(w_cont, LV_OPA_40, 0);
    lv_obj_set_style_radius(w_cont, RAD_CARD, 0);
    lv_obj_set_flex_flow(w_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    standby_water_ring = lv_arc_create(w_cont);
    lv_obj_set_size(standby_water_ring, DM(120), DM(120));
    lv_arc_set_bg_angles(standby_water_ring, 0, 360);
    lv_arc_set_angles(standby_water_ring, 0, 360);
    /* P103 反色对撞：喝水环改橙色（原蓝）。背景轨浅灰、指示器橙、
     * knob 圆头点显式设橙（原 knob 未设色 → LVGL 默认蓝，倒数"点"变蓝） */
    lv_obj_set_style_arc_color(standby_water_ring, lv_color_hex(0xE5E5EA),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(standby_water_ring, lv_color_hex(COL_ORANGE),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(standby_water_ring, lv_color_hex(COL_ORANGE),
                              LV_PART_KNOB);
    lv_obj_set_style_arc_opa(standby_water_ring, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(standby_water_ring, LV_OPA_COVER,
                             LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(standby_water_ring, 10, 0);
    lv_obj_remove_flag(standby_water_ring, LV_OBJ_FLAG_CLICKABLE);

    standby_water_lbl = lv_label_create(standby_water_ring);
    lv_label_set_text(standby_water_lbl, "--");
    lv_obj_set_style_text_font(standby_water_lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(standby_water_lbl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_center(standby_water_lbl);

    lv_obj_t *w_cap = lv_label_create(w_cont);
    lv_label_set_text(w_cap, "喝水");
    lv_obj_set_style_text_font(w_cap, FONT_BODY, 0);
    lv_obj_set_style_text_color(w_cap, lv_color_hex(COL_SEC), 0);

    /* 久坐圈（右）——Liquid Glass 玻璃圆底 */
    lv_obj_t *s_cont = make_clean_cont(rings_row);
    lv_obj_set_size(s_cont, DM(160), DM(170));
    lv_obj_set_style_bg_color(s_cont, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_60, 0);
    lv_obj_set_style_border_color(s_cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_cont, 3, 0);
    lv_obj_set_style_border_opa(s_cont, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(s_cont, 20, 0);
    lv_obj_set_style_shadow_opa(s_cont, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_cont, RAD_CARD, 0);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    standby_sit_ring = lv_arc_create(s_cont);
    lv_obj_set_size(standby_sit_ring, DM(120), DM(120));
    lv_arc_set_bg_angles(standby_sit_ring, 0, 360);
    lv_arc_set_angles(standby_sit_ring, 0, 360);
    /* P103 反色对撞：久坐环改蓝色（原橙），与喝水环橙色对撞。背景轨浅灰、
     * 指示器蓝、knob 圆头点显式设蓝（原 knob 未设色 → LVGL 默认蓝，恰好
     * 与久坐环同色，倒数"点"不再突兀） */
    lv_obj_set_style_arc_color(standby_sit_ring, lv_color_hex(0xE5E5EA),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(standby_sit_ring, lv_color_hex(COL_BLUE),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(standby_sit_ring, lv_color_hex(COL_BLUE),
                              LV_PART_KNOB);
    lv_obj_set_style_arc_opa(standby_sit_ring, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(standby_sit_ring, LV_OPA_COVER,
                             LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(standby_sit_ring, 10, 0);
    lv_obj_remove_flag(standby_sit_ring, LV_OBJ_FLAG_CLICKABLE);

    standby_sit_lbl = lv_label_create(standby_sit_ring);
    lv_label_set_text(standby_sit_lbl, "--");
    lv_obj_set_style_text_font(standby_sit_lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(standby_sit_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(standby_sit_lbl);

    lv_obj_t *s_cap = lv_label_create(s_cont);
    lv_label_set_text(s_cap, "久坐");
    lv_obj_set_style_text_font(s_cap, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_cap, lv_color_hex(COL_SEC), 0);

    /* ── AI 语音圈（双圈下方居中，按住说话，复用 PTT 回调）──
     * 2026-08-16 P103：样式对齐锁屏音乐播放按钮（music_play_btn_create
     * 同款：白色玻璃圆 + 蓝色阴影 + 无边框；按压蓝紫渐变 + 光晕），
     * 仅图标符号保留麦克风（LV_SYMBOL_AUDIO）——UI 独立、功能共享。 */
    standby_ai_btn = lv_btn_create(standby_overlay);
    lv_obj_set_size(standby_ai_btn, DM(46), DM(46));
    lv_obj_set_style_radius(standby_ai_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(standby_ai_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(standby_ai_btn, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(standby_ai_btn, 8, 0);
    lv_obj_set_style_shadow_color(standby_ai_btn, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_shadow_opa(standby_ai_btn, LV_OPA_20, 0);
    lv_obj_set_style_border_width(standby_ai_btn, 0, 0);
    /* 按压反馈（对齐音乐播放按钮 PRESSED：蓝紫渐变玻璃 + 蓝光晕）——
     * dm_ai_voice_press_cb 内另有动态按压样式，二者叠加不冲突 */
    lv_obj_set_style_transform_width(standby_ai_btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(standby_ai_btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(standby_ai_btn, lv_color_hex(COL_BLUE),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(standby_ai_btn, lv_color_hex(COL_PURPLE),
                                   LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(standby_ai_btn, LV_GRAD_DIR_VER,
                                 LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(standby_ai_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(standby_ai_btn, 14, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(standby_ai_btn, lv_color_hex(COL_BLUE),
                                  LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(standby_ai_btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_flag(standby_ai_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(standby_ai_btn, dm_ai_voice_press_cb, LV_EVENT_PRESSED,
                        NULL);
    lv_obj_add_event_cb(standby_ai_btn, dm_ai_voice_release_cb,
                        LV_EVENT_RELEASED, NULL);

    lv_obj_t *ai_mic = lv_label_create(standby_ai_btn);
    lv_label_set_text(ai_mic, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(ai_mic, FONT_ICON, 0);
    lv_obj_set_style_text_color(ai_mic, lv_color_hex(COL_BLUE), 0);
    lv_obj_center(ai_mic);

    /* Hint */
    standby_ai_text = lv_label_create(standby_overlay);
    lv_label_set_text(standby_ai_text, "按住说话");
    lv_obj_set_style_text_font(standby_ai_text, FONT_BODY, 0);
    lv_obj_set_style_text_color(standby_ai_text, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_text_opa(standby_ai_text, LV_OPA_60, 0);

    /* 音乐后台化（2026-08-08）：standby 锁屏音乐控件——歌曲名 + 控制。
     * standby 不停止音乐；歌曲名/播放图标由 music_update_track_info/
     * music_resume/music_pause 同步（指针判空保护）。 */
    standby_music_title_lbl = lv_label_create(standby_overlay);
    lv_label_set_text(standby_music_title_lbl, "");
    lv_obj_set_style_text_font(standby_music_title_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(standby_music_title_lbl, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *sm_row = make_clean_cont(standby_overlay);
    lv_obj_set_size(sm_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sm_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sm_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sm_row, 12, 0);

    lv_obj_t *sm_prev = music_round_btn(sm_row, DM(36), LV_SYMBOL_PREV,
                                        music_prev_cb, NULL);
    (void)sm_prev;

    lv_obj_t *sm_play = music_play_btn_create(sm_row, DM(42),
                                              &standby_music_play_icon);
    (void)sm_play;

    lv_obj_t *sm_next = music_round_btn(sm_row, DM(36), LV_SYMBOL_NEXT,
                                        music_next_cb, NULL);
    (void)sm_next;

    /* Bottom spacer — P147：grow 2（原 3）与 sp_top(grow 3) 按 3:2 分配
     * 剩余空间，内容组居中偏上、与健康卡保持 ≥20px 视觉间距（原 1:3
     * 把双圈/AI/音乐压贴健康卡，用户反馈间距失衡）。健康卡仍贴底
     * （其后无子元素，pad_row 只作用于元素之间）。 */
    lv_obj_t *sp_mid = make_clean_cont(standby_overlay);
    lv_obj_set_size(sp_mid, DM(1), DM(1));
    lv_obj_set_flex_grow(sp_mid, 2);

    /* Health card at bottom */
    lv_obj_t *health = lv_obj_create(standby_overlay);
    lv_obj_set_size(health, DM(800), DM(96));
    lv_obj_add_style(health, &style_card, 0);
    /* Override style_card padding — content needs room for 3× FONT_BODY rows */
    lv_obj_set_style_pad_all(health, 12, 0);
    lv_obj_set_style_pad_top(health, 16, 0);
    /* P103：卡片边缘蓝色光晕（对齐播放按钮发光语言），覆盖 style_card
     * 的黑色淡影——与双圈/音乐钮统一 Liquid Glass 光晕手法 */
    lv_obj_set_style_shadow_width(health, 12, 0);
    lv_obj_set_style_shadow_color(health, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_shadow_opa(health, LV_OPA_20, 0);
    lv_obj_set_scrollbar_mode(health, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(health, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(health, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(health, standby_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(health, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(health, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* P103 反色对撞：健康卡跟随双圈——在座/起立（久坐主题）蓝色、
     * 喝水橙色，与双圈（坐蓝/水橙）呼应，层次更丰富 */
    standby_h1_lbl = lv_label_create(health);
    lv_label_set_text(standby_h1_lbl, LV_SYMBOL_BELL "  00:00");
    lv_obj_set_style_text_color(standby_h1_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(standby_h1_lbl, FONT_BODY, 0);

    standby_h2_lbl = lv_label_create(health);
    lv_label_set_text(standby_h2_lbl, LV_SYMBOL_TINT "  喝水 0");
    lv_obj_set_style_text_color(standby_h2_lbl, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_text_font(standby_h2_lbl, FONT_BODY, 0);
    /* P180：锁屏喝水行也可点 → 打卡 +1（冒泡到 health 的
     * standby_click_cb → 同时唤醒；LVGL 事件先子后父，顺序正确） */
    lv_obj_add_flag(standby_h2_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(standby_h2_lbl, water_tap_cb, LV_EVENT_CLICKED, NULL);

    standby_h3_lbl = lv_label_create(health);
    lv_label_set_text(standby_h3_lbl, LV_SYMBOL_UP "  起立 0");
    lv_obj_set_style_text_color(standby_h3_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font(standby_h3_lbl, FONT_BODY, 0);

    /* 2026-08-27 锁屏人体显示（review-15 定稿，非临时 debug）：
     * 挂 health 卡内（flex row 第 4 项）——之前挂 overlay flex column
     * 末尾会被排到贴底的健康卡下方，挤出屏幕外看不到。
     * P124 迁 UART3 后数据源 = dm_ld2410b_present/get_dist_cm。 */
    standby_prox_lbl = lv_label_create(health);
    lv_label_set_text(standby_prox_lbl, "人体：--");
    lv_obj_set_style_text_font(standby_prox_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(standby_prox_lbl, lv_color_hex(COL_SEC), 0);
    /* 2026-08-28 修复：LVGL flex 缓存 label 初始宽度（"人体：--"≈52px），
     * 文本更新为"人体：有人 0.5m"后不重排 → 52px 内 WRAP 只显示"人体："，
     * "有人 0.5m"被裁（用户见"人体-"无内容）。显式宽度根治：
     * P128 字号统一 FONT_BODY 24px（与在座/喝水/起立一致，用户要求），
     * 24px 下最长文本"人体：有人 2.2m"≈224px → 宽度 260 留余量。
     * P126 遗留"人体"二字乱码嫌疑 = LV_LABEL_LONG_CLIP 对 FreeType
     * 位图字形渲染有裁切风险（全项目仅此一处用 CLIP），故不设 CLIP——
     * 默认 WRAP 在 260px 内不会换行，行为等价且渲染安全 */
    /* P147：原 sp_bot(DM12) 残留空隙——锁屏 pad_row 只作用于 flex 子元素
     * 之间，健康卡已是最后一个子元素，删掉 sp_bot 即紧贴屏幕底零留空 */
    lv_obj_set_width(standby_prox_lbl, 260);

    /* P153：锁屏环境光显示（人体 label 右侧，LTR553 ALS）。
     * dm_als_get_lux() 由 standby_health_rings_refresh 每秒读取刷新；
     * 无光感传感器返回 -1 → 显示 "光感 -- lux"。
     * 创建必须先于首次 refresh（审查发现：原顺序导致锁屏首帧无亮度行）。 */
    standby_lux_lbl = lv_label_create(health);
    lv_label_set_text(standby_lux_lbl, "光感 -- lux");
    lv_obj_set_style_text_font(standby_lux_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(standby_lux_lbl, lv_color_hex(COL_SEC), 0);
    /* 最长文本 "光感 9999 lux"@24px ≈ 180px，显式宽度留余量（P126 教训：
     * flex 缓存初始宽度，文本变长后 WRAP 裁切，需显式宽度） */
    lv_obj_set_width(standby_lux_lbl, 200);

    /* 2026-08-15 健康助理：注册事件回调（双圈每秒刷新）+ 首次刷新 */
    dm_health_set_cb(standby_health_cb);
    standby_health_rings_refresh();

    /* 电子宠物：锁屏右下角宠物（总开关关着不建） */
    if (dm_pet_is_enabled()) {
        dm_pet_create(standby_overlay);
        dm_pet_on_event(PET_EVT_SLEEP);  /* 恢复动画 + 停靠右下角 */
    }

    clock_update_cb(NULL);
    LV_LOG_USER("Enter standby");
}

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

/* Phase 1 拆分：create_deskmate_screen → ui_home_create（deskmate_ui.c
 * 的 deskmate_ui_create 入口改为调用本函数） */
void ui_home_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

    /* Screen background */
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Screen-level flex: column */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(scr, 0, 0);

    init_shared_styles();

    /* Input callbacks */
    lv_obj_add_event_cb(scr, screen_input_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_input_cb, LV_EVENT_KEY, NULL);

    /* ══════ HEADER ══════ */
    lv_obj_t *header = make_clean_cont(scr);
    lv_obj_set_size(header, lv_pct(100), TOP_BAR_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_50, 0);
    create_status_bar_ex(header, &clock_label, &clock_date_label,
                         &status_weather_label,
                         &status_wifi_lbl, &status_bt_lbl);

    /* ══════ CONTENT AREA ══════ */
    lv_obj_t *content = make_clean_cont(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 0, 0);

    /* ── P146：hero 大时钟已移除（时间/周几日期在左上角状态栏），
     * 卡片区上移但保留合理顶部呼吸间距——原 DM(12) 致天气/圈圈卡
     * "死顶"状态栏、下方留白失衡；DM(56) 让卡片区垂直居中偏上，
     * 底部留白由 sp_bot flex-grow 收敛（skill 留白≥20px 规范）。 */
    lv_obj_t *sp_top = make_clean_cont(content);
    lv_obj_set_size(sp_top, DM(1), DM(56));

    /* Cards row: weather + AI side by side (card_h ≤400 per design spec) */
    lv_coord_t card_w = (sw - 2 * EDGE_PAD - CARD_GAP) / 2;
    lv_coord_t card_h = DM(150);

    lv_obj_t *cards_row = make_clean_cont(content);
    lv_obj_set_size(cards_row, sw - 2 * EDGE_PAD, card_h);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cards_row, CARD_GAP, 0);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *weather_card = create_card(cards_row, card_w, card_h);
    create_weather_card(weather_card);

    lv_obj_t *ai_card = create_card(cards_row, card_w, card_h);
    create_home_health_card(ai_card);   /* P103：右侧卡改为喝水/久坐双小圈 */

    /* 音乐后台化（2026-08-08）：HOME 首页常驻音乐小组件——显示当前
     * 歌曲名 + 播放状态 + 控制按钮，点击进入完整播放器。 */
    /* 与天气/AI 卡片之间留垂直间距，避免音乐插件与天气模块边界重叠
     * （2026-08-08 布局修复：用户反馈边界冲突）。 */
    lv_obj_t *sp_music = make_clean_cont(content);
    lv_obj_set_size(sp_music, DM(1), DM(18));
    create_home_music_widget(content);

    /* Bottom spacer: pushes dock down */
    lv_obj_t *sp_bot = make_clean_cont(content);
    lv_obj_set_size(sp_bot, DM(1), DM(1));
    lv_obj_set_flex_grow(sp_bot, 1);

    /* ══════ NAVIGATION GROUP ══════ */
    g_group = lv_group_create();
    lv_group_set_wrap(g_group, true);

    g_keypad_indev = NULL;
    {
        lv_indev_t *indev = lv_indev_get_next(NULL);
        while (indev) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
                lv_indev_set_group(indev, g_group);
                g_keypad_indev = indev;
                LV_LOG_USER("Keypad indev → group attached");
            }
            indev = lv_indev_get_next(indev);
        }
    }

    /* ══════ DOCK ══════ */
    lv_obj_t *dock = make_clean_cont(scr);
    lv_obj_set_size(dock, lv_pct(100), DOCK_H);
    g_dock = dock;
    create_app_dock(dock);

    /* Clock timer (1s) */
    lv_timer_t *clk_timer = lv_timer_create(clock_update_cb, 1000, NULL);
    if (clk_timer) lv_timer_set_repeat_count(clk_timer, -1);
    clock_update_cb(NULL);

    /* Weather timer: kick the first background fetch now; the callback
     * polls result flags every 30s on failure / 4h on success (P113),
     * and a WiFi connect edge also kicks an immediate fetch. */
    weather_update_cb(NULL);
    lv_timer_t *w_timer = lv_timer_create(weather_update_cb, 30000, NULL);
    if (w_timer) lv_timer_set_repeat_count(w_timer, -1);

    /* Idle timer (g_idle_timeout → standby) */
    idle_countdown = g_idle_timeout;
    idle_timer = lv_timer_create(idle_timer_cb, 1000, NULL);
    if (idle_timer) lv_timer_set_repeat_count(idle_timer, -1);

    /* 音乐后台化（2026-08-08）：music timer 是系统级常驻——开机创建，
     * 驱动 XPlayer 异步状态机（PREPARED→start、COMPLETE→next）与进度
     * 更新，不随音乐子页面开/关而创建/销毁。播放时 resume、暂停时
     * pause（music_resume/music_pause 控制）；子页面关闭后仍轮询，
     * 保证后台播放与自动连播。 */
    g_music_timer = lv_timer_create(music_timer_cb, 500, NULL);
    if (g_music_timer) {
        lv_timer_set_repeat_count(g_music_timer, -1);
        lv_timer_pause(g_music_timer);   /* 未播放不轮询，节省 CPU */
    }

    /* 2026-08-16 P103：健康事件回调注册提前到开机（原在 show_standby 才注册，
     * 锁屏前坐下事件丢失 → 欢迎语/双圈反馈全丢）。回调内部对 standby UI
     * 判空：standby_health_rings_refresh / health_popup_hide / health_popup_show
     * 均有 NULL 保护，锁屏前只播语音/弹窗、不刷双圈，安全。 */
    dm_health_set_cb(standby_health_cb);

    /* P153：暗光开灯提醒——dm_als 状态机触发 → 弹窗（仅有人时显示） */
    dm_als_set_dark_cb(dark_popup_trigger_cb);

    LV_LOG_USER("Desktop Mate v4.0 — %dx%d (flex layout, 3-tier fonts)",
                (int)sw, (int)sh);
}
