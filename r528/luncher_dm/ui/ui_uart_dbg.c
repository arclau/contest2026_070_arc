/*
 * ui_uart_dbg.c — UART 调试工具子页（嵌套于 Settings）【2026-08-27 新增】
 *
 * 需求：Settings → UART 调试工具，接收其他设备经串口发来的 debug log。
 * 布局（2026-08-27 v3/v5）：顶部横排工具栏（波特率循环 / 暂停 / 清屏 /
 *       三套配色主题按钮，一行排布）+ 日志窗口占满主体，自动滚底。
 *       配色抄 IDE 经典三套：深色(One Dark)/浅色/护眼绿，可随时切换。
 * 数据流：dm_uart_dbg 后台线程收串口 → 8KB 环形缓冲 → 本页 lv_timer
 *       200ms 消费 → 文本缓冲 → label 显示（超限丢最旧）。
 * 生命周期：进入子页 create 时 dm_uart_dbg_open()；close_subpage 调
 *       ui_uart_dbg_close_cleanup() 停 timer + 关串口（并恢复 LD2410B）。
 *
 * 共享宏/工具（DM/COL/FONT、make_clean_cont、subpage_big_title、
 * settings_row_value 等）来自 deskmate_ui.h。
 */

#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_uart_dbg.h"
#include "dm_health_cfg.h"   /* 波特率档位持久化（/data/dm_health.cfg） */

/* 波特率档位持久化键（存档位索引 0..DM_UART_DBG_BAUD_CNT-1） */
#define UART_DBG_BAUD_CFG "uart_dbg_baud"

/* ================================================================
 * UART 调试工具子页状态
 * ================================================================ */

/* 终端文本缓冲（渲染后文本；超限从头部丢弃，防无限增长） */
#define UART_DBG_TEXT_MAX   6000
static char        g_uart_dbg_text[UART_DBG_TEXT_MAX];
static int         g_uart_dbg_text_len;

/* 配色主题（抄 IDE 经典搭配：深色 One Dark / 浅色 / 护眼绿）
 * bg=终端底色，fg=文字色。切换按钮用各自底色做小色块。 */
struct uart_dbg_theme_s
{
  const char *name;      /* 主题名（按钮悬停/日志） */
  uint32_t    bg;        /* 终端底色 */
  uint32_t    fg;        /* 终端文字色 */
};
#define UART_DBG_THEME_CNT 3
static const struct uart_dbg_theme_s uart_dbg_themes[UART_DBG_THEME_CNT] =
{
  { "深色", 0x282C34, 0xABB2BF },  /* One Dark（VS Code 默认深色） */
  { "浅色", 0xFFFFFF, 0x1F1F1F },  /* 浅色 */
  { "护眼", 0xC7EDCC, 0x1C1C1C },  /* 豆沙绿护眼（绿底黑字） */
};

static lv_obj_t   *uart_dbg_term;       /* 终端滚动容器（占主体） */
static lv_obj_t   *uart_dbg_tl;         /* 终端 label */
static lv_obj_t   *uart_dbg_baud_val;   /* 波特率按钮 label */
static lv_obj_t   *uart_dbg_theme_btns[UART_DBG_THEME_CNT]; /* 主题色块按钮 */
static lv_timer_t *uart_dbg_timer;      /* 100ms 消费环形缓冲 */

static int         uart_dbg_baud_idx;   /* 当前波特率档位索引 */
static int         uart_dbg_theme_idx;  /* 当前配色主题索引 */

/* P165：功能增强 */
static lv_obj_t   *uart_dbg_toolbar_lbl; /* 工具栏端口名+活动+字节 label */
static bool        uart_dbg_hex_mode;  /* true=HEX 显示模式 */
static uint32_t    uart_dbg_last_rx_ms; /* 最近一次收到数据的时刻（活动指示灯） */
static lv_obj_t   *uart_dbg_ta;        /* 底部输入框 */
static dm_kb_state_t *uart_dbg_kb_state; /* 键盘（共享工具管理） */
static bool        uart_dbg_paused;    /* 暂停接收标志 */
static lv_obj_t   *uart_dbg_pause_btn; /* 暂停/继续按钮 */

/* HEX 模式状态（跨 feed 调用保持） */
static int         uart_dbg_hex_off = 0;          /* 行内偏移 */
static uint32_t    uart_dbg_hex_addr = 0;         /* 全局字节地址 */

/* 前向声明（uart_dbg_hex_cb 提前调用） */
static void uart_dbg_clear_cb(lv_event_t *e);

/* ================================================================
 * 文本缓冲工具
 * ================================================================ */

/* 追加渲染后文本；超限从头部丢弃旧内容 */
static void uart_dbg_append(const char *s)
{
    int sl = (int)strlen(s);
    if (sl <= 0)
        return;
    if (g_uart_dbg_text_len + sl >= UART_DBG_TEXT_MAX) {
        int keep = UART_DBG_TEXT_MAX - sl - 1;
        if (keep <= 0) {
            /* 单次追加就超过上限：只保留尾部 */
            strncpy(g_uart_dbg_text, s + (sl - keep + 1), keep);
            g_uart_dbg_text[keep] = '\0';
            g_uart_dbg_text_len   = keep;
            return;
        }
        memmove(g_uart_dbg_text, g_uart_dbg_text + (g_uart_dbg_text_len - keep),
                keep);
        g_uart_dbg_text_len = keep;
        g_uart_dbg_text[keep] = '\0';
    }
    memcpy(g_uart_dbg_text + g_uart_dbg_text_len, s, sl);
    g_uart_dbg_text_len += sl;
    g_uart_dbg_text[g_uart_dbg_text_len] = '\0';
}

/* 把一段原始字节渲染为文本并追加。
 * ASCII 模式：可打印直出，\r\n 归一为 \n；非打印转 '.'。
 * HEX 模式：每行16字节 "00 11 22 ..." 末尾附 ASCII 预览。 */
static void uart_dbg_feed(const uint8_t *buf, int n)
{
    char tmp[512];
    int  ti = 0;

    if (!uart_dbg_hex_mode) {
        /* ── ASCII 模式（原有逻辑） ── */
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (b == '\r') continue;
            if (ti >= (int)sizeof(tmp) - 2) {
                tmp[ti] = '\0';
                uart_dbg_append(tmp);
                ti = 0;
            }
            if (b == '\n')      tmp[ti++] = '\n';
            else if (b >= 0x20 && b < 0x7F) tmp[ti++] = (char)b;
            else                tmp[ti++] = '.';
        }
    } else {
        /* ── HEX 模式：每行16字节 hex + ASCII 预览 ── */
        for (int i = 0; i < n; i++) {
            if (uart_dbg_hex_off == 0) {
                ti += snprintf(tmp + ti, sizeof(tmp) - ti, "%08lX: ", uart_dbg_hex_addr);
            }
            ti += snprintf(tmp + ti, sizeof(tmp) - ti, "%02X ", buf[i]);
            uart_dbg_hex_off++;
            uart_dbg_hex_addr++;
            if (uart_dbg_hex_off >= 16 || i == n - 1) {
                /* 补齐 + ASCII 预览 */
                for (int j = uart_dbg_hex_off; j < 16; j++)
                    ti += snprintf(tmp + ti, sizeof(tmp) - ti, "   ");
                ti += snprintf(tmp + ti, sizeof(tmp) - ti, " |");
                for (int j = i - uart_dbg_hex_off + 1; j <= i; j++) {
                    uint8_t c = buf[j];
                    tmp[ti++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                }
                ti += snprintf(tmp + ti, sizeof(tmp) - ti, "|\n");
                uart_dbg_hex_off = 0;
            }
            if (ti >= (int)sizeof(tmp) - 48) {
                tmp[ti] = '\0';
                uart_dbg_append(tmp);
                ti = 0;
            }
        }
    }

    if (ti > 0) {
        tmp[ti] = '\0';
        uart_dbg_append(tmp);
    }
}

/* 终端滚动到底部 */
static void uart_dbg_scroll_bottom(void)
{
    if (uart_dbg_term && uart_dbg_tl)
        lv_obj_scroll_to_y(uart_dbg_term, lv_obj_get_height(uart_dbg_tl),
                           LV_ANIM_OFF);
}

/* ================================================================
 * 左侧工具栏回调
 * ================================================================ */

/* 波特率：点击循环到下一档（9600 → … → 115200 → 9600；1500000 已移除） */
static void uart_dbg_baud_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();   /* deskmate-ui 铁律 4：交互重置闲置计时器 */
    uart_dbg_baud_idx = (uart_dbg_baud_idx + 1) % DM_UART_DBG_BAUD_CNT;
    speed_t b = dm_uart_dbg_bauds[uart_dbg_baud_idx].baud;
    dm_uart_dbg_set_baud(b);
    /* 切档即落盘，下次打开恢复 */
    dm_health_cfg_set_int(UART_DBG_BAUD_CFG, uart_dbg_baud_idx);
    if (uart_dbg_baud_val)
        lv_label_set_text_fmt(uart_dbg_baud_val, "波特率 %s",
                              dm_uart_dbg_bauds[uart_dbg_baud_idx].label);
    LV_LOG_USER("[uart_dbg] baud -> %s",
                dm_uart_dbg_bauds[uart_dbg_baud_idx].label);
}

/* 注入测试数据：调用 dm_uart_dbg_inject_test() 往环形缓冲灌一段测试文本，
 * UI 200ms timer 自动消费显示——验证串口→UI 显示链路（无需外部接线）。 */
static void uart_dbg_inject_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dm_uart_dbg_inject_test();
}

/* 回车按钮：单次发 \r\n 验证 UART1 TX 通路 */
static void uart_dbg_enter_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    static const char crlf[] = "\r\n";
    dm_uart_dbg_write(crlf, 2);
    LV_LOG_USER("[uart_dbg] sent CRLF");
}

/* HEX/ASCII 切换：清屏 + 翻转模式 + 重置 HEX 地址 */
static void uart_dbg_hex_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    uart_dbg_hex_mode = !uart_dbg_hex_mode;
    /* 切模式时清屏，避免混显 */
    uart_dbg_clear_cb(NULL);
}

/* 清屏：清环形缓冲 + 清文本 + 重置 HEX 地址 */
static void uart_dbg_clear_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    dm_uart_dbg_clear();
    g_uart_dbg_text_len = 0;
    g_uart_dbg_text[0]  = '\0';
    uart_dbg_hex_off = 0;
    uart_dbg_hex_addr = 0;
    if (uart_dbg_tl)
        lv_label_set_text(uart_dbg_tl, "");
}

/* 暂停/继续接收（UI 停消费 + reader 丢弃数据，防恢复后 burst 积压） */
static void uart_dbg_pause_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    uart_dbg_paused = !uart_dbg_paused;
    /* reader 侧同步暂停：暂停期读出丢弃，继续后从新数据开始 */
    dm_uart_dbg_set_paused(uart_dbg_paused ? 1 : 0);
    if (uart_dbg_pause_btn) {
        lv_obj_t *lbl = lv_obj_get_child(uart_dbg_pause_btn, 1);
        if (lbl) {
            lv_label_set_text(lbl, uart_dbg_paused ? LV_SYMBOL_PLAY " 继续" : LV_SYMBOL_PAUSE " 暂停");
        }
    }
    LV_LOG_USER("[uart_dbg] %s", uart_dbg_paused ? "paused" : "resumed");
}

/* 键盘回车回调 / 发送按钮（dm_kb_ready_cb_t 签名）：
 * 非空才发（空输入直接返回，不发 CRLF）；发送成功后终端回显 [TX] 行 */
static void uart_dbg_send_ready_cb(lv_obj_t *ta, void *user_data)
{
    (void)user_data;
    reset_idle_timer();
    if (!ta)
        return;
    const char *txt = lv_textarea_get_text(ta);
    if (!txt || !txt[0])
        return;   /* 空输入不发，避免盲发 CRLF */
    int tlen = (int)strlen(txt);
    int w1 = dm_uart_dbg_write(txt, tlen);
    /* 正文发出后才补 \r\n，对端收到完整命令行 */
    int w2 = (w1 >= 0) ? dm_uart_dbg_write("\r\n", 2) : -1;
    LV_LOG_USER("[uart_dbg] sent %d bytes", tlen);
    if (w1 >= 0 && w2 >= 0) {
        /* 终端回显一行 [TX] 内容（复用显示链，HEX 模式直追加保可读） */
        char echo[256];
        snprintf(echo, sizeof(echo), "[TX] %s\n", txt);
        uart_dbg_append(echo);
        if (uart_dbg_tl && g_uart_dbg_text_len > 0)
            lv_label_set_text(uart_dbg_tl, g_uart_dbg_text);
        uart_dbg_scroll_bottom();
    }
    lv_textarea_set_text(ta, "");
}

/* 发送按钮回调（兼容 LV_EVENT_CLICKED）：复用 uart_dbg_send_ready_cb */
static void uart_dbg_send_btn_cb(lv_event_t *e)
{
    (void)e;
    uart_dbg_send_ready_cb(uart_dbg_ta, NULL);
}

/* 应用当前配色主题：终端底色/文字色 + 主题按钮高亮描边 */
static void uart_dbg_apply_theme(void)
{
    const struct uart_dbg_theme_s *t = &uart_dbg_themes[uart_dbg_theme_idx];

    if (uart_dbg_term)
        lv_obj_set_style_bg_color(uart_dbg_term, lv_color_hex(t->bg), 0);
    if (uart_dbg_tl)
        lv_obj_set_style_text_color(uart_dbg_tl, lv_color_hex(t->fg), 0);

    for (int i = 0; i < UART_DBG_THEME_CNT; i++) {
        if (!uart_dbg_theme_btns[i])
            continue;
        if (i == uart_dbg_theme_idx)
            lv_obj_set_style_border_color(uart_dbg_theme_btns[i],
                                          lv_color_hex(COL_BLUE), 0);
        else
            lv_obj_set_style_border_color(uart_dbg_theme_btns[i],
                                          lv_color_hex(COL_SEP), 0);
    }
}

/* 主题切换：点击色块按钮 → 应用对应配色 */
static void uart_dbg_theme_cb(lv_event_t *e)
{
    reset_idle_timer();
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx < 0 || idx >= UART_DBG_THEME_CNT)
        return;
    uart_dbg_theme_idx = idx;
    uart_dbg_apply_theme();
    LV_LOG_USER("[uart_dbg] theme -> %s", uart_dbg_themes[idx].name);
}

/* ================================================================
 * 刷新 timer：消费环形缓冲 → 渲染 → 滚动到底
 * ================================================================ */

static void uart_dbg_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (uart_dbg_paused)
        return;

    uint8_t chunk[256];
    int     n;
    bool    new_data = false;

    /* 工具栏端口名刷新：数据活动指示灯 */
    if (uart_dbg_toolbar_lbl) {
        uint32_t total = dm_uart_dbg_total();
        static uint32_t last_total = 0;
        if (total != last_total)
            uart_dbg_last_rx_ms = lv_tick_get();
        last_total = total;

        bool active = (lv_tick_get() - uart_dbg_last_rx_ms < 500);
        const char *act_dot = active ? " ●" : " ○";
        uint32_t act_col = active ? 0x4CAF50 : 0x999999;
        char ibuf[32];
        snprintf(ibuf, sizeof(ibuf), "/dev/uart1  %s", act_dot);
        lv_label_set_text(uart_dbg_toolbar_lbl, ibuf);
        lv_obj_set_style_text_color(uart_dbg_toolbar_lbl,
                                    lv_color_hex(act_col), 0);
    }

    while ((n = dm_uart_dbg_read(chunk, sizeof(chunk))) > 0) {
        uart_dbg_feed(chunk, n);
        new_data = true;
    }

    if (new_data && g_uart_dbg_text_len > 0 && uart_dbg_tl)
        lv_label_set_text(uart_dbg_tl, g_uart_dbg_text);

    if (new_data)
        uart_dbg_scroll_bottom();
}

/* ================================================================
 * 子页构建（show_subpage("UART") → deskmate_ui.c dispatch）
 * ================================================================ */

/* 顶部工具栏胶囊按钮（icon + 文本一体；文本 label 经 user_data 暴露，
 * 波特率/显示/暂停回调直接改它） */
static lv_obj_t *uart_dbg_tool_btn(lv_obj_t *parent, const char *icon,
                                    uint32_t icon_color, const char *text,
                                    lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, DM(30));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(btn, 6, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_pad_hor(btn, DM(10), 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, DM(4), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, FONT_ICON, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(icon_color), 0);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, FONT_BODY, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_user_data(btn, lb);
    return btn;
}

/* 主题色块圆形按钮：底色=该主题 bg，选中蓝描边（apply_theme 更新） */
static lv_obj_t *uart_dbg_theme_btn(lv_obj_t *parent, int idx)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, DM(30), DM(30));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(uart_dbg_themes[idx].bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, uart_dbg_theme_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
    return btn;
}

void ui_uart_dbg_create(lv_obj_t *parent)
{
    /* parent = content；其父 = subpage_overlay（返回键/状态栏同层）。
     * 2026-08-27 v4：工具栏与日志窗口 FLOATING 定位到 overlay 上——
     * 绕过 content pad_top=TOP_BAR_H+DM(48)=247px 的顶距，把工具栏
     * 提到返回键右侧同一行（返回键 x=DM(16) w=DM(40)，底=TOP_BAR_H+DM(44)），
     * 日志窗口从返回键底边起占满剩余——消灭顶部大片留白。 */
    lv_obj_t *ov = lv_obj_get_parent(parent);
    lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
    lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

/* ── 顶部工具栏：返回键右侧同行（波特率/暂停/清屏/配色）── */
    lv_obj_t *bar = make_clean_cont(ov);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, DM(30));
    lv_obj_set_pos(bar, DM(16) + DM(40) + DM(30), TOP_BAR_H + DM(4));
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, DM(6), 0);
    lv_obj_update_layout(bar);
    assert(lv_obj_get_x(bar) + lv_obj_get_width(bar) <= sw &&
           TOP_BAR_H + DM(4) + DM(30) <= sh && "uart_dbg: 工具栏越界");

    /* ── 最左端：信息区——端口名 + 活动指示灯（小卡片框，压缩宽度）── */
    lv_obj_t *port_card = lv_obj_create(bar);
    lv_obj_set_size(port_card, LV_SIZE_CONTENT, DM(26));
    lv_obj_set_style_bg_color(port_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(port_card, LV_OPA_70, 0);
    lv_obj_set_style_radius(port_card, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(port_card, 1, 0);
    lv_obj_set_style_border_color(port_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(port_card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(port_card, 6, 0);
    lv_obj_set_style_shadow_opa(port_card, LV_OPA_10, 0);
    lv_obj_set_style_pad_hor(port_card, DM(8), 0);
    lv_obj_set_style_pad_ver(port_card, DM(2), 0);
    lv_obj_set_flex_flow(port_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(port_card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(port_card, DM(4), 0);
    lv_obj_clear_flag(port_card, LV_OBJ_FLAG_SCROLLABLE);

    uart_dbg_toolbar_lbl = lv_label_create(port_card);
    lv_label_set_text(uart_dbg_toolbar_lbl, "UART1  ○");
    lv_obj_set_style_text_font(uart_dbg_toolbar_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(uart_dbg_toolbar_lbl, lv_color_hex(COL_TEXT), 0);

    /* ── 分隔符（负 margin 收紧与两侧元素间距） ── */
    lv_obj_t *sep1 = lv_label_create(bar);
    lv_label_set_text(sep1, "|");
    lv_obj_set_style_text_color(sep1, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_text_font(sep1, FONT_BODY, 0);
    lv_obj_set_style_margin_left(sep1, DM(-3), 0);
    lv_obj_set_style_margin_right(sep1, DM(-3), 0);

    /* ── 配置区：波特率（上次档位落盘恢复，缺项回退默认） ── */
    uart_dbg_baud_idx = 0;
    for (int i = 0; i < DM_UART_DBG_BAUD_CNT; i++) {
        if (dm_uart_dbg_bauds[i].baud == DM_UART_DBG_BAUD_DEFAULT) {
            uart_dbg_baud_idx = i;
            break;
        }
    }
    int saved_idx = dm_health_cfg_get_int(UART_DBG_BAUD_CFG, uart_dbg_baud_idx);
    if (saved_idx >= 0 && saved_idx < DM_UART_DBG_BAUD_CNT)
        uart_dbg_baud_idx = saved_idx;
    char bbuf[24];
    snprintf(bbuf, sizeof(bbuf), "波特率 %s",
             dm_uart_dbg_bauds[uart_dbg_baud_idx].label);
    lv_obj_t *bd_btn = uart_dbg_tool_btn(bar, LV_SYMBOL_SETTINGS, COL_BLUE,
                                         bbuf, uart_dbg_baud_cb);
    uart_dbg_baud_val = lv_obj_get_user_data(bd_btn);

    /* ── 分隔符 ── */
    lv_obj_t *sep2 = lv_label_create(bar);
    lv_label_set_text(sep2, "|");
    lv_obj_set_style_text_color(sep2, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_text_font(sep2, FONT_BODY, 0);
    lv_obj_set_style_margin_left(sep2, DM(-3), 0);
    lv_obj_set_style_margin_right(sep2, DM(-3), 0);

    /* ── 操作区：清屏 / 注入 / HEX / 暂停 / 回车 ── */
    uart_dbg_tool_btn(bar, LV_SYMBOL_TRASH, COL_RED, "清屏",
                      uart_dbg_clear_cb);
    uart_dbg_tool_btn(bar, LV_SYMBOL_IMAGE, COL_GREEN, "注入",
                      uart_dbg_inject_cb);
    uart_dbg_tool_btn(bar, LV_SYMBOL_LOOP, COL_PURPLE, "HEX",
                      uart_dbg_hex_cb);
    uart_dbg_pause_btn = uart_dbg_tool_btn(bar, LV_SYMBOL_PAUSE, COL_ORANGE, "暂停",
                                           uart_dbg_pause_cb);
    uart_dbg_tool_btn(bar, LV_SYMBOL_RIGHT, COL_BLUE, "回车",
                      uart_dbg_enter_cb);

    /* ── 分隔符 + 主题区 ── */
    lv_obj_t *sep3 = lv_label_create(bar);
    lv_label_set_text(sep3, "|");
    lv_obj_set_style_text_color(sep3, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_text_font(sep3, FONT_BODY, 0);
    lv_obj_set_style_margin_left(sep3, DM(-3), 0);
    lv_obj_set_style_margin_right(sep3, DM(-3), 0);

    for (int i = 0; i < UART_DBG_THEME_CNT; i++)
        uart_dbg_theme_btns[i] = uart_dbg_theme_btn(bar, i);

    /* ── 日志窗口：从返回键底边起到屏幕底部（仅留 DM(8) 小边距），配色随主题 ── */
    uart_dbg_term = lv_obj_create(ov);
    lv_obj_set_pos(uart_dbg_term, EDGE_PAD, TOP_BAR_H + DM(44));
    lv_obj_set_size(uart_dbg_term, sw - 2 * EDGE_PAD,
                    sh - (TOP_BAR_H + DM(44)) - DM(58));
    assert(EDGE_PAD + (sw - 2 * EDGE_PAD) <= sw &&
           TOP_BAR_H + DM(44) + (sh - (TOP_BAR_H + DM(44)) - DM(58)) <= sh &&
           "uart_dbg: 日志窗口越界");
    lv_obj_add_flag(uart_dbg_term, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(uart_dbg_term,
                              lv_color_hex(uart_dbg_themes[0].bg), 0);
    lv_obj_set_style_bg_opa(uart_dbg_term, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(uart_dbg_term, RAD_CARD, 0);
    lv_obj_set_style_border_width(uart_dbg_term, 1, 0);
    lv_obj_set_style_border_color(uart_dbg_term, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(uart_dbg_term, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(uart_dbg_term, 6, 0);
    lv_obj_set_style_shadow_opa(uart_dbg_term, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(uart_dbg_term, DM(14), 0);
    lv_obj_set_scrollbar_mode(uart_dbg_term, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(uart_dbg_term, LV_OBJ_FLAG_SCROLLABLE);

    uart_dbg_tl = lv_label_create(uart_dbg_term);
    lv_label_set_long_mode(uart_dbg_tl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(uart_dbg_tl, "");
    lv_obj_set_width(uart_dbg_tl, lv_pct(100));
    lv_obj_set_style_text_font(uart_dbg_tl, FONT_BODY, 0);
    lv_obj_set_style_text_color(uart_dbg_tl,
                                lv_color_hex(uart_dbg_themes[0].fg), 0);

    /* 默认配色：深色（One Dark），并高亮当前主题按钮 */
    uart_dbg_theme_idx = 0;
    uart_dbg_apply_theme();

    /* ── 底部输入条：textarea + 发送按钮（键盘弹出时终端自动缩小） ── */
    lv_coord_t input_y = sh - DM(50) - DM(8);
    lv_obj_t *input_bar = make_clean_cont(ov);
    lv_obj_set_pos(input_bar, EDGE_PAD, input_y);
    lv_obj_set_size(input_bar, sw - 2 * EDGE_PAD, DM(50));
    lv_obj_add_flag(input_bar, LV_OBJ_FLAG_FLOATING);
    /* Liquid Glass 半透明风格 */
    lv_obj_set_style_bg_color(input_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(input_bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(input_bar, RAD_CARD, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_border_color(input_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(input_bar, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(input_bar, 24, 0);
    lv_obj_set_style_shadow_opa(input_bar, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(input_bar, DM(6), 0);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_bar, DM(10), 0);

    uart_dbg_ta = lv_textarea_create(input_bar);
    lv_obj_set_flex_grow(uart_dbg_ta, 1);
    lv_textarea_set_one_line(uart_dbg_ta, true);
    lv_textarea_set_placeholder_text(uart_dbg_ta, "输入文本发送到 UART1...");
    lv_obj_set_style_text_font(uart_dbg_ta, FONT_BODY, 0);
    lv_obj_add_flag(uart_dbg_ta, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *send_btn = lv_btn_create(input_bar);
    lv_obj_set_size(send_btn, DM(80), DM(40));
    lv_obj_set_style_radius(send_btn, RAD_CARD, 0);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(send_btn, LV_OPA_70, 0);
    lv_obj_set_style_border_width(send_btn, 1, 0);
    lv_obj_set_style_border_color(send_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(send_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(send_btn, 12, 0);
    lv_obj_set_style_shadow_opa(send_btn, LV_OPA_30, 0);
    lv_obj_add_event_cb(send_btn, uart_dbg_send_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, LV_SYMBOL_RIGHT " 发送");
    lv_obj_set_style_text_font(send_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(send_lbl);

    /* ── 键盘（dm_kb_* 统一管理，初始隐藏，点击输入框弹出） ── */
    uart_dbg_kb_state = dm_kb_create(ov, uart_dbg_ta,
                                     uart_dbg_send_ready_cb, NULL);

    /* 打开串口（/dev/uart1；PD21/PD22 mux4，P124 迁回；波特率=上次落盘档） */
    /* 先初始化暂停/显示状态（open 成败都需确定性初值） */
    uart_dbg_paused = false;
    uart_dbg_hex_mode = false;
    uart_dbg_hex_off = 0;
    uart_dbg_hex_addr = 0;
    uart_dbg_timer = NULL;

    if (dm_uart_dbg_open(dm_uart_dbg_bauds[uart_dbg_baud_idx].baud) != 0) {
        /* open 失败不建 timer，终端显示错误提示行（端口/接线/波特率） */
        uart_dbg_append("打开 /dev/uart1 失败\n请检查接线与波特率后重进\n");
        if (uart_dbg_tl && g_uart_dbg_text_len > 0)
            lv_label_set_text(uart_dbg_tl, g_uart_dbg_text);
        LV_LOG_USER("[uart_dbg] open failed, no timer");
        return;
    }

    /* 100ms 刷新 timer（仅 open 成功才建） */
    uart_dbg_timer = lv_timer_create(uart_dbg_timer_cb, 100, NULL);
    uart_dbg_timer_cb(NULL);           /* 立即刷一次状态 */
}

/* close_subpage 清理（deskmate_ui.c 调用）：停 timer + 关串口 + 置空 */
void ui_uart_dbg_close_cleanup(void)
{
    if (uart_dbg_timer) {
        lv_timer_del(uart_dbg_timer);
        uart_dbg_timer = NULL;
    }
    /* 先解 reader 暂停（open 失败/暂停中退出时清标志，防带到下次打开） */
    dm_uart_dbg_set_paused(0);
    dm_uart_dbg_close();
    dm_kb_destroy(uart_dbg_kb_state);

    uart_dbg_term      = NULL;
    uart_dbg_tl        = NULL;
    uart_dbg_baud_val  = NULL;
    uart_dbg_toolbar_lbl = NULL;
    uart_dbg_ta        = NULL;
    uart_dbg_pause_btn = NULL;
    uart_dbg_kb_state  = NULL;
    g_uart_dbg_text_len = 0;
    g_uart_dbg_text[0]  = '\0';
    uart_dbg_paused = false;
    uart_dbg_hex_mode = false;
    uart_dbg_hex_off = 0;
    uart_dbg_hex_addr = 0;
}
