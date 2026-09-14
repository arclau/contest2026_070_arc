/*
 * ui_bt.c — 蓝牙管理子页（嵌套于 Settings）【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_bt_subpage +
 * bt_*_cb + bt_page_destructor_cb + bt static 变量。
 * 逻辑一字不改；共享宏/工具（DM/COL/FONT、make_clean_cont、
 * subpage_big_title、settings_* 构建器）来自 deskmate_ui.h。
 *
 * 红线（勿碰）：
 *   - bt_pair_overlay 挂在根屏幕，由 bt_page_destructor_cb 删除
 *     （对象+清理一起搬，禁止拆散）。
 *   - bt_timer/destructor 绑定子页容器生命周期，随本页搬走。
 *   - settings_bt_cb 定义在 deskmate_ui.c（Settings 开关回调），
 *     ui_bt.c 仅作回调指针使用，不重复实现。
 */

#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_net.h"

/* ================================================================
 * 蓝牙管理子页状态（原 deskmate_ui.c 259-265 行）
 * ================================================================ */

static lv_obj_t      *bt_status_lbl;      /* 子页顶部适配器状态 */
static lv_obj_t      *bt_list_cont;       /* 附近设备清单容器 */
static lv_obj_t      *bt_paired_cont;     /* 已配对设备清单容器 */
static lv_timer_t    *bt_timer;           /* 发现/配对轮询（500ms） */
static lv_obj_t      *bt_pair_overlay;    /* 配对请求弹窗 */
static int            bt_paired_tick;     /* 已配对列表低频刷新计数 */
/* 2026-09-10 P206：连接结果回读状态（connect 异步无回调，poll 轮询查询；
 * tick 以 500ms 计，20 次≈10s 超时）+ 单轮发现只重建一次列表 */
static uint8_t        bt_conn_addr[6];    /* 正在连接的对端 MAC */
static int            bt_conn_pending;    /* 1 = 等待连接结果回读 */
static int            bt_conn_tick;       /* 等待计数 */
static int            bt_list_built;      /* 1 = 本轮发现已重建列表 */

/* ================================================================
 * BLUETOOTH 管理子页（wifijianyi.md 手机式接入）
 * 控制组（主开关 + Discoverable）+ 已配对/附近设备双列表 + 配对弹窗
 * 回调（on_discovery_result/on_pair_request）运行在 bluetoothd socket
 * 线程，只更新 dm_net 静态缓存，UI 经 bt_timer 轮询拉取。
 * ================================================================ */

/* Discoverable 开关（wifijianyi.md：开放可见性，120s 倒计时由驱动侧控制） */
static void bt_discover_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dm_net_bt_set_discoverable(on ? 1 : 0);
}

/* 配对弹窗：接受/拒绝 → dm_net_bt_pair_reply */
static void bt_pair_accept_cb(lv_event_t *e)
{
    (void)e;
    dm_net_bt_pair_reply(1);
    if (bt_pair_overlay) {
        lv_obj_del(bt_pair_overlay);
        bt_pair_overlay = NULL;
    }
}

static void bt_pair_reject_cb(lv_event_t *e)
{
    (void)e;
    dm_net_bt_pair_reply(0);
    if (bt_pair_overlay) {
        lv_obj_del(bt_pair_overlay);
        bt_pair_overlay = NULL;
    }
}

/* 有配对请求 → 弹确认框（显示对端地址） */
static void bt_pair_prompt(void)
{
    uint8_t addr[6];
    char info[48];

    if (bt_pair_overlay)
        return;
    if (!dm_net_bt_pair_pending())
        return;
    dm_net_bt_pair_get_addr(addr);
    snprintf(info, sizeof(info), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    lv_obj_t *scr = lv_scr_act();
    bt_pair_overlay = lv_obj_create(scr);
    lv_obj_set_size(bt_pair_overlay,
                    lv_disp_get_hor_res(lv_disp_get_default()),
                    lv_disp_get_ver_res(lv_disp_get_default()));
    lv_obj_set_style_bg_color(bt_pair_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bt_pair_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bt_pair_overlay, 0, 0);
    lv_obj_set_style_pad_all(bt_pair_overlay, 0, 0);
    /* wifijianyi.md：遮罩吸收点击，防穿透到背后列表引发重入 */
    lv_obj_add_flag(bt_pair_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bt_pair_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(bt_pair_overlay);

    lv_obj_t *box = lv_obj_create(bt_pair_overlay);
    lv_obj_set_size(box, lv_pct(70), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, DM(20), 0);
    lv_obj_set_style_radius(box, RAD_CARD, 0);

    lv_obj_t *ttl = lv_label_create(box);
    lv_label_set_text(ttl, "配对请求");
    lv_obj_set_style_text_font(ttl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *info_lbl = lv_label_create(box);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_font(info_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(COL_SEC), 0);

    lv_obj_t *btnrow = lv_obj_create(box);
    lv_obj_set_size(btnrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_pad_all(btnrow, 0, 0);

    lv_obj_t *acc = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(acc, 1);
    lv_obj_add_event_cb(acc, bt_pair_accept_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *acc_l = lv_label_create(acc);
    lv_label_set_text(acc_l, "接受");
    lv_obj_set_style_text_font(acc_l, FONT_BODY, 0);

    lv_obj_t *rej = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(rej, 1);
    lv_obj_add_event_cb(rej, bt_pair_reject_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rej_l = lv_label_create(rej);
    lv_label_set_text(rej_l, "拒绝");
    lv_obj_set_style_text_font(rej_l, FONT_BODY, 0);
}

/* 附近设备行点击 → 连接该设备 */
static void bt_dev_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    dm_net_bt_dev_t dev;

    if (dm_net_bt_dev_get(idx, &dev) != 0)
        return;
    dm_net_bt_connect(dev.addr);
    /* 2026-09-10 P206：记下对端 MAC，bt_poll_cb 轮询回读连接结果 */
    memcpy(bt_conn_addr, dev.addr, sizeof(bt_conn_addr));
    bt_conn_pending = 1;
    bt_conn_tick = 0;
    if (bt_status_lbl)
        lv_label_set_text_fmt(bt_status_lbl, "正在连接 %s…",
                              dev.name[0] ? dev.name : "设备");
}

/* 已配对设备行点击 → 连接/断开切换 */
static void bt_paired_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    dm_net_bt_dev_t dev;

    if (dm_net_bt_paired_get(idx, &dev) != 0)
        return;
    /* 已配对设备默认执行连接；断开由行内独立"断开"小按钮
     * （2026-09-10 P206，改动最小：不动整行点击手势） */
    dm_net_bt_connect(dev.addr);
    /* 2026-09-10 P206：记下对端 MAC，bt_poll_cb 轮询回读连接结果 */
    memcpy(bt_conn_addr, dev.addr, sizeof(bt_conn_addr));
    bt_conn_pending = 1;
    bt_conn_tick = 0;
    if (bt_status_lbl)
        lv_label_set_text_fmt(bt_status_lbl, "正在连接 %s…",
                              dev.name[0] ? dev.name : "设备");
}

/* 2026-09-10 P206：已配对行内独立"断开"小按钮（dm_net_bt_disconnect
 * 此前 UI 零调用）；user_data = 已配对索引，断开只调 disconnect 不动连接逻辑 */
static void bt_paired_disc_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    dm_net_bt_dev_t dev;

    if (dm_net_bt_paired_get(idx, &dev) != 0)
        return;
    if (dm_net_bt_disconnect(dev.addr) == 0 && bt_status_lbl)
        lv_label_set_text(bt_status_lbl, "已断开连接");
}

/* 重建附近设备清单（发现完成后调用；行 user_data = 设备索引） */
static void bt_rebuild_list(void)
{
    if (bt_list_cont == NULL)
        return;

    lv_obj_clean(bt_list_cont);
    int n = dm_net_bt_dev_count();
    for (int i = 0; i < n; i++) {
        dm_net_bt_dev_t dev;
        if (dm_net_bt_dev_get(i, &dev) != 0)
            break;

        lv_obj_t *row = lv_obj_create(bt_list_cont);
        lv_obj_set_size(row, lv_pct(100), DM(56));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, DM(12), 0);
        /* 2026-08-10：分组卡内透明行（对齐 WiFi/Files 列表行） */
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, DM(16), 0);
        lv_obj_set_style_pad_right(row, DM(16), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, bt_dev_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);

        /* 左侧蓝色蓝牙图标徽章（对齐 WiFi/Files 行图标徽章风格） */
        lv_obj_t *bbox = lv_obj_create(row);
        lv_obj_set_size(bbox, DM(36), DM(36));
        lv_obj_set_style_radius(bbox, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bbox, lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_bg_opa(bbox, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(bbox, 6, 0);
        lv_obj_set_style_shadow_color(bbox, lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_shadow_opa(bbox, LV_OPA_30, 0);
        lv_obj_set_style_border_width(bbox, 0, 0);
        lv_obj_clear_flag(bbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *bi = lv_label_create(bbox);
        lv_label_set_text(bi, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_font(bi, FONT_ICON, 0);
        lv_obj_set_style_text_color(bi, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(bi);

        lv_obj_t *nm = lv_label_create(row);
        /* wifijianyi.md：设备名未知长度，强制省略号截断防行高撑爆 */
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, dev.name[0] ? dev.name : "未知设备");
        /* 2026-08-10：去掉固定 pct(70)（与 flex_grow 冲突，且左侧徽章已占位） */
        lv_obj_set_style_text_font(nm, FONT_BODY, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(nm, 1);

        lv_obj_t *mt = lv_label_create(row);
        lv_label_set_text(mt, "点击连接");
        lv_obj_set_style_text_font(mt, FONT_BODY, 0);
        lv_obj_set_style_text_color(mt, lv_color_hex(COL_BLUE), 0);
    }
}

/* 重建已配对设备清单（MY DEVICES 组） */
static void bt_rebuild_paired(void)
{
    if (bt_paired_cont == NULL)
        return;

    lv_obj_clean(bt_paired_cont);
    int n = dm_net_bt_paired_count();
    for (int i = 0; i < n; i++) {
        dm_net_bt_dev_t dev;
        if (dm_net_bt_paired_get(i, &dev) != 0)
            break;

        lv_obj_t *row = lv_obj_create(bt_paired_cont);
        lv_obj_set_size(row, lv_pct(100), DM(56));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, DM(12), 0);
        /* 2026-08-10：分组卡内透明行（对齐附近设备/WiFi/Files 列表行） */
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, DM(16), 0);
        lv_obj_set_style_pad_right(row, DM(16), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, bt_paired_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);

        /* 左侧蓝色蓝牙图标徽章（对齐附近设备行） */
        lv_obj_t *pbox = lv_obj_create(row);
        lv_obj_set_size(pbox, DM(36), DM(36));
        lv_obj_set_style_radius(pbox, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pbox, lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_bg_opa(pbox, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(pbox, 6, 0);
        lv_obj_set_style_shadow_color(pbox, lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_shadow_opa(pbox, LV_OPA_30, 0);
        lv_obj_set_style_border_width(pbox, 0, 0);
        lv_obj_clear_flag(pbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *pi = lv_label_create(pbox);
        lv_label_set_text(pi, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_font(pi, FONT_ICON, 0);
        lv_obj_set_style_text_color(pi, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(pi);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, dev.name[0] ? dev.name : "未知设备");
        /* 2026-08-10：去掉固定 pct(70)（与 flex_grow 冲突，且左侧徽章已占位） */
        lv_obj_set_style_text_font(nm, FONT_BODY, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(nm, 1);

        lv_obj_t *mt = lv_label_create(row);
        lv_label_set_text(mt, "已配对");
        lv_obj_set_style_text_font(mt, FONT_BODY, 0);
        lv_obj_set_style_text_color(mt, lv_color_hex(COL_GREEN), 0);

        /* 2026-09-10 P206：行内独立"断开"小按钮（改动最小，不碰整行点击=连接）。
         * 2026-09-10 P206b：Liquid Glass 药丸对齐 WiFi 断开（半透明白+白边+红字）。 */
        lv_obj_t *disc = lv_btn_create(row);
        lv_obj_set_size(disc, LV_SIZE_CONTENT, DM(32));
        lv_obj_set_style_radius(disc, DM(16), 0);
        lv_obj_set_style_bg_color(disc, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(disc, LV_OPA_60, 0);
        lv_obj_set_style_border_color(disc, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(disc, 2, 0);
        lv_obj_set_style_border_opa(disc, LV_OPA_50, 0);
        lv_obj_set_style_pad_left(disc, DM(14), 0);
        lv_obj_set_style_pad_right(disc, DM(14), 0);
        lv_obj_add_event_cb(disc, bt_paired_disc_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(disc, (void *)(intptr_t)i);
        lv_obj_t *disc_l = lv_label_create(disc);
        lv_label_set_text(disc_l, "断开");
        lv_obj_set_style_text_font(disc_l, FONT_BODY, 0);
        lv_obj_set_style_text_color(disc_l, lv_color_hex(COL_BLUE), 0);
        lv_obj_center(disc_l);
    }
}

/* 轮询：发现完成 → 重建清单；连接等待 → 回读结果；配对请求 → 弹窗 */
static void bt_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 2026-09-10 P206：连接等待期间不刷通用状态，保住"正在连接…"不被覆盖 */
    if (!bt_conn_pending && bt_status_lbl) {
        int on = dm_net_bt_is_on();
        lv_label_set_text(bt_status_lbl,
            on ? "蓝牙已开启，正在搜索附近设备…" : "蓝牙未开启");
    }
    /* 2026-09-10 P206：scan_done 置1后单轮只重建一次（防每500ms重复刷表） */
    if (dm_net_bt_scan_done() && bt_list_cont && !bt_list_built) {
        bt_rebuild_list();
        bt_list_built = 1;
    }
    /* 2026-09-10 P206 推理链：connect 异步无回调 → 在此轮询回读
     * dm_net_bt_is_connected；成功显"已连接"并刷新已配对表，
     * 20 tick（≈10s）超时回"连接失败，点击重试"可再点 */
    if (bt_conn_pending) {
        if (dm_net_bt_is_connected(bt_conn_addr)) {
            bt_conn_pending = 0;
            if (bt_status_lbl)
                lv_label_set_text(bt_status_lbl, "已连接");
            dm_net_bt_paired_refresh();
            bt_rebuild_paired();
        } else if (++bt_conn_tick >= 20) {
            bt_conn_pending = 0;
            if (bt_status_lbl)
                lv_label_set_text(bt_status_lbl, "连接失败，点击重试");
        }
    }
    /* 已配对列表低频刷新（MY DEVICES）：配对请求处理后更新 */
    if (bt_paired_cont && (++bt_paired_tick % 20 == 0)) {
        dm_net_bt_paired_refresh();
        bt_rebuild_paired();
    }
    bt_pair_prompt();   /* 有配对请求弹确认框 */
}

/* 子页析构（wifijianyi.md）：容器销毁自动删 timer，防野指针 */
static void bt_page_destructor_cb(lv_event_t *e)
{
    (void)e;
    if (bt_timer) {
        lv_timer_del(bt_timer);
        bt_timer = NULL;
    }
    bt_status_lbl = NULL;
    bt_list_cont  = NULL;
    bt_paired_cont = NULL;
    /* 2026-09-10 P206：页销毁丢弃连接等待/重建标记，防野 MAC 误判 */
    bt_conn_pending = 0;
    bt_conn_tick = 0;
    bt_list_built = 0;
    if (bt_pair_overlay) {
        lv_obj_del(bt_pair_overlay);
        bt_pair_overlay = NULL;
    }
}

void ui_bt_create(lv_obj_t *parent)
{
    LV_LOG_USER("create_bt_subpage called! parent=%p", parent);
    /* 2026-08-10 修复：缺 flex column 导致标题/状态/列表全部叠在左上角
     * （同 WiFi 子页根因）。补上 + 层间距统一 DM(6)。 */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, DM(6), 0);

    subpage_big_title(parent, "蓝牙");

    /* ── 状态卡（Liquid Glass）：适配器状态 ── */
    lv_obj_t *status_card = lv_obj_create(parent);
    lv_obj_set_size(status_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_70, 0);
    lv_obj_set_style_radius(status_card, RAD_CARD, 0);
    lv_obj_set_style_border_width(status_card, 1, 0);
    lv_obj_set_style_border_color(status_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(status_card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(status_card, 6, 0);
    lv_obj_set_style_shadow_opa(status_card, LV_OPA_10, 0);
    lv_obj_set_style_pad_left(status_card, DM(16), 0);
    lv_obj_set_style_pad_right(status_card, DM(16), 0);
    lv_obj_set_style_pad_top(status_card, DM(8), 0);
    lv_obj_set_style_pad_bottom(status_card, DM(8), 0);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

    bt_status_lbl = lv_label_create(status_card);
    lv_obj_set_style_text_font(bt_status_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(bt_status_lbl, lv_color_hex(COL_TEXT), 0);

    /* 控制组：主开关 + Discoverable */
    settings_section_label(parent, "蓝牙");
    lv_obj_t *ctrl = settings_group_card(parent);
    settings_row_switch_cb(ctrl, LV_SYMBOL_BLUETOOTH, COL_BLUE, "蓝牙",
                           dm_net_bt_is_on(), settings_bt_cb);
    settings_add_separator(ctrl);
    settings_row_switch_cb(ctrl, LV_SYMBOL_EYE_OPEN, COL_ORANGE,
                           "可被发现", false, bt_discover_switch_cb);

    /* 已配对设备组（我的设备，低频轮询刷新） */
    settings_section_label(parent, "我的设备");
    lv_obj_t *paired_card = settings_group_card(parent);
    bt_paired_cont = paired_card;

    /* 附近设备组 */
    settings_section_label(parent, "其他设备");
    lv_obj_t *other_card = settings_group_card(parent);
    bt_list_cont = other_card;

    /* 轮询 timer（500ms）+ 生命周期绑定 */
    bt_timer = lv_timer_create(bt_poll_cb, 500, NULL);
    if (bt_timer) lv_timer_set_repeat_count(bt_timer, -1);
    lv_obj_add_event_cb(parent, bt_page_destructor_cb, LV_EVENT_DELETE, NULL);

    /* 适配器已开 → 自动开始发现 */
    if (dm_net_bt_is_on()) {
        dm_net_bt_start_scan();
        bt_list_built = 0;   /* 2026-09-10 P206：新一轮发现允许重建列表 */
        if (bt_status_lbl)
            lv_label_set_text(bt_status_lbl, "正在搜索附近设备…");
    }
}
