/*
 * ui_wifi.c — WiFi 管理子页（嵌套于 Settings）【Phase 1 拆分】
 *
 * 2026-08-09 从 deskmate_ui.c 原样搬入：create_wifi_subpage +
 * wifi_*_cb + wifi_page_destructor_cb + wifi static 变量。
 * 逻辑一字不改；共享宏/工具（DM/COL/FONT、make_clean_cont、
 * subpage_big_title、dm_net_status_refresh）来自 deskmate_ui.h。
 *
 * 红线（勿碰）：
 *   - wifi_pwd_overlay 挂在根屏幕，close_subpage 经 ui_wifi_close_cleanup()
 *     清理（对象+清理一起搬，禁止拆散）。
 *   - wifi_timer/destructor 绑定子页容器生命周期，随本页搬走。
 *   - 驱动状态机在 dm_net.c（g_conn_gen 代数），本文件只读快照。
 */

#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_net.h"

/* ================================================================
 * WiFi 管理子页状态（原 deskmate_ui.c 256-263 行）
 * ================================================================ */

static lv_obj_t      *wifi_status_lbl;    /* 子页顶部当前连接状态 */
static lv_obj_t      *wifi_list_cont;     /* SSID 清单容器（重建用） */
static lv_timer_t    *wifi_timer;         /* 扫描/连接轮询（500ms） */
static lv_obj_t      *wifi_pwd_overlay;   /* 密码输入层（挂根屏） */
static lv_obj_t      *wifi_pwd_ta;        /* 密码 textarea */
static char           wifi_pwd_ssid[33];  /* 待连接 SSID */
static int            wifi_last_conn_state;
/* 2026-08-10 iPad 风格增强：已连接卡 / 已保存网络卡 / 手动加入输入层 */
static lv_obj_t      *wifi_conn_lbl;      /* 已连接网络卡标签 */
static lv_obj_t      *wifi_disc_btn;      /* 已连接卡"断开"按钮（仅已连时可见） */
static lv_obj_t      *wifi_saved_card;    /* 已保存网络卡容器 */
static lv_obj_t      *wifi_saved_lbl;     /* 已保存网络卡标签 */
static lv_obj_t      *wifi_manual_overlay;/* 手动加入输入层（挂根屏） */
static lv_obj_t      *wifi_manual_ssid_ta;
static lv_obj_t      *wifi_manual_psk_ta;
static dm_kb_state_t *wifi_manual_kb;     /* dm_kb_* 共享键盘 */
static dm_kb_state_t *wifi_pwd_kb;        /* dm_kb_* 共享键盘 */
/* P215：scan_done 粘滞（dm_net 仅下次 scan_start 清零）→ 无守卫则每 500ms
 * 全量 lv_obj_clean+重建（UI 抖动 + 触摸中的行被清）。对照 ui_bt.c
 * bt_list_built：本轮扫描只建一次，新扫描再清零。 */
static int            wifi_list_built;

/* 2026-08-10 修复（-32）：密码明文/密文切换回调（定义在 wifi_pwd_ok_cb 前，
 * 手动加入弹窗 wifi_manual_open_cb 更靠前 → 需前向声明） */
static void wifi_ta_eye_cb(lv_event_t *e);

/* close_subpage 清理：密码输入层挂在根屏幕 scr 上，不随 subpage_overlay
 * 删除——必须显式销毁并置 NULL，否则物理返回键关闭子页后残留黑色
 * 键盘层遮挡一切（wifijianyi.md 扫雷漏洞 1）。 */
void ui_wifi_close_cleanup(void)
{
    if (wifi_pwd_overlay) {
        dm_kb_destroy(wifi_pwd_kb);
        wifi_pwd_kb = NULL;
        lv_obj_del(wifi_pwd_overlay);
        wifi_pwd_overlay = NULL;
        wifi_pwd_ta     = NULL;
    }
    /* 2026-08-10：手动加入输入层同样挂根屏，需一并清理防残留 */
    if (wifi_manual_overlay) {
        dm_kb_destroy(wifi_manual_kb);
        wifi_manual_kb = NULL;
        lv_obj_del(wifi_manual_overlay);
        wifi_manual_overlay   = NULL;
        wifi_manual_ssid_ta   = NULL;
        wifi_manual_psk_ta    = NULL;
    }
}

/* ================================================================
 * WIFI 管理子页（2026-08-08 手机式接入）
 * SSID 清单（wifi_scan_networks 异步扫描）→ 点击 AP →
 *    OPEN 直连 / 加密弹软键盘输密码 → wifi_connect 后台验证 →
 *    成功写 wapi.conf（开机自动重连）+ 列表打勾 + 状态栏变蓝
 * ================================================================ */

static void wifi_scan_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    (void)btn;
    if (dm_net_wifi_scan_start() != 0) {
        /* 驱动忙：扫描未发起，提示用户稍后重试 */
        if (wifi_status_lbl)
            lv_label_set_text(wifi_status_lbl, "Wi-Fi 忙，请稍后再试");
        return;
    }
    wifi_list_built = 0;   /* P215：新一轮扫描，允许重建一次 */
    if (wifi_status_lbl)
        lv_label_set_text(wifi_status_lbl, "扫描中…");
}

/* 2026-08-10 iPad 风格：Wi-Fi 主开关（接驱动开/关） */
static void wifi_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dm_net_wifi_set(on ? 1 : 0);
    if (wifi_status_lbl) {
        if (on) {
            lv_label_set_text(wifi_status_lbl, "正在开启…");
            if (dm_net_wifi_scan_start() == 0)   /* 开启后自动扫描 */
                wifi_list_built = 0;   /* P215：新一轮扫描，允许重建一次 */
        } else {
            lv_label_set_text(wifi_status_lbl, "Wi-Fi 已关闭");
        }
    }
}

/* 2026-09-10 P206：已连接卡"断开"按钮（底层有 wifi_disconnect，
 * 无 forget 接口，详见 dm_net.c dm_net_wifi_disconnect 注释） */
static void wifi_disc_btn_cb(lv_event_t *e)
{
    (void)e;
    if (dm_net_wifi_disconnect() == 0) {
        if (wifi_conn_lbl)
            lv_label_set_text(wifi_conn_lbl, "未连接网络");
        /* 2026-09-10 P206：断开后按钮自己藏起来——没连还摆个断开是 bug */
        if (wifi_disc_btn)
            lv_obj_add_flag(wifi_disc_btn, LV_OBJ_FLAG_HIDDEN);
        if (wifi_status_lbl)
            lv_label_set_text(wifi_status_lbl, "已断开连接");
        dm_net_status_refresh();
    } else if (wifi_status_lbl) {
        lv_label_set_text(wifi_status_lbl, "断开失败，请重试");
    }
}

/* 手动加入输入层：确定 → 起后台连接；取消 → 关闭输入层 */
static void wifi_manual_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = wifi_manual_ssid_ta ?
                       lv_textarea_get_text(wifi_manual_ssid_ta) : "";
    const char *psk  = wifi_manual_psk_ta ?
                       lv_textarea_get_text(wifi_manual_psk_ta) : "";
    if (ssid[0]) {
        /* 2026-09-10 P206：connect_start 因 state==1 拒并发时返回 -1，
         * 此时仍提示"正在连接中，请稍候"而非"正在连接新 SSID"误导 */
        if (dm_net_wifi_connect_start(ssid, psk[0] ? psk : NULL) != 0) {
            if (wifi_status_lbl)
                lv_label_set_text(wifi_status_lbl, "正在连接中，请稍候");
        } else if (wifi_status_lbl) {
            lv_label_set_text_fmt(wifi_status_lbl, "正在连接 %s…", ssid);
        }
    }
    if (wifi_manual_overlay) {
        dm_kb_destroy(wifi_manual_kb);
        wifi_manual_kb = NULL;
        lv_obj_del(wifi_manual_overlay);
        wifi_manual_overlay   = NULL;
        wifi_manual_ssid_ta   = NULL;
        wifi_manual_psk_ta    = NULL;
    }
}

static void wifi_manual_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_manual_overlay) {
        dm_kb_destroy(wifi_manual_kb);
        wifi_manual_kb = NULL;
        lv_obj_del(wifi_manual_overlay);
        wifi_manual_overlay   = NULL;
        wifi_manual_ssid_ta   = NULL;
        wifi_manual_psk_ta    = NULL;
    }
}

/* "其他网络…" → 弹手动加入输入层（SSID + 密码，挂根屏覆盖子页） */
static void wifi_manual_open_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_manual_overlay)
        return;

    lv_obj_t *scr = lv_scr_act();
    wifi_manual_overlay = lv_obj_create(scr);
    lv_obj_set_size(wifi_manual_overlay,
                    lv_disp_get_hor_res(lv_disp_get_default()),
                    lv_disp_get_ver_res(lv_disp_get_default()));
    lv_obj_set_style_bg_color(wifi_manual_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_manual_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(wifi_manual_overlay, 0, 0);
    lv_obj_set_style_pad_all(wifi_manual_overlay, 0, 0);
    lv_obj_add_flag(wifi_manual_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(wifi_manual_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(wifi_manual_overlay);

    lv_obj_t *box = lv_obj_create(wifi_manual_overlay);
    /* 2026-08-10 修复（-32）：同密码弹窗——缩小 50%（宽 80%→40%）+ 上移 */
    lv_obj_set_size(box, lv_pct(40), LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, TOP_BAR_H + DM(8));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    /* 2026-08-10 修复（-31）：Liquid Glass 卡片（skill 玻璃配方，同密码弹窗） */
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(box, 10, 0);
    lv_obj_set_style_shadow_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(box, DM(24), 0);
    lv_obj_set_style_pad_row(box, DM(16), 0);
    lv_obj_set_style_radius(box, RAD_CARD, 0);

    lv_obj_t *ttl = lv_label_create(box);
    lv_label_set_text(ttl, "加入其他网络");
    lv_obj_set_style_text_font(ttl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(COL_TEXT), 0);

    wifi_manual_ssid_ta = lv_textarea_create(box);
    lv_obj_set_size(wifi_manual_ssid_ta, lv_pct(100), DM(50));
    lv_textarea_set_one_line(wifi_manual_ssid_ta, true);
    lv_textarea_set_placeholder_text(wifi_manual_ssid_ta, "网络名称 (SSID)");
    lv_obj_set_style_text_font(wifi_manual_ssid_ta, FONT_BODY, 0);
    /* 2026-08-10 修复（-31）：输入框白底 + 灰边 + 圆角（同密码弹窗） */
    lv_obj_set_style_bg_color(wifi_manual_ssid_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(wifi_manual_ssid_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_manual_ssid_ta, 1, 0);
    lv_obj_set_style_border_color(wifi_manual_ssid_ta, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_border_opa(wifi_manual_ssid_ta, LV_OPA_60, 0);
    lv_obj_set_style_radius(wifi_manual_ssid_ta, DM(8), 0);
    lv_obj_set_style_pad_all(wifi_manual_ssid_ta, DM(8), 0);

    /* 2026-08-10 修复（-32）：密码行 = 输入框 + 眼睛按钮（明文/密文切换） */
    lv_obj_t *psk_row = lv_obj_create(box);
    lv_obj_set_size(psk_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(psk_row, LV_FLEX_FLOW_ROW);
    /* 2026-08-10 修复（-33）：眼睛按钮与密码栏纵向居中（cross CENTER） */
    lv_obj_set_flex_align(psk_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(psk_row, 0, 0);
    lv_obj_set_style_bg_opa(psk_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(psk_row, 0, 0);
    lv_obj_set_style_pad_column(psk_row, DM(4), 0);

    wifi_manual_psk_ta = lv_textarea_create(psk_row);
    lv_obj_set_size(wifi_manual_psk_ta, LV_SIZE_CONTENT, DM(50));
    lv_obj_set_flex_grow(wifi_manual_psk_ta, 1);
    lv_textarea_set_password_mode(wifi_manual_psk_ta, true);
    lv_textarea_set_one_line(wifi_manual_psk_ta, true);
    lv_textarea_set_placeholder_text(wifi_manual_psk_ta, "密码（开放网络可留空）");
    lv_obj_set_style_text_font(wifi_manual_psk_ta, FONT_BODY, 0);
    lv_obj_set_style_bg_color(wifi_manual_psk_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(wifi_manual_psk_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_manual_psk_ta, 1, 0);
    lv_obj_set_style_border_color(wifi_manual_psk_ta, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_border_opa(wifi_manual_psk_ta, LV_OPA_60, 0);
    lv_obj_set_style_radius(wifi_manual_psk_ta, DM(8), 0);
    lv_obj_set_style_pad_all(wifi_manual_psk_ta, DM(8), 0);

    lv_obj_t *eye = lv_btn_create(psk_row);
    lv_obj_set_size(eye, DM(20), DM(20));
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_add_event_cb(eye, wifi_ta_eye_cb, LV_EVENT_CLICKED, wifi_manual_psk_ta);
    lv_obj_t *eye_lbl = lv_label_create(eye);
    lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(eye_lbl, FONT_ICON, 0);
    lv_obj_set_style_text_color(eye_lbl, lv_color_hex(COL_SEC), 0);
    lv_obj_center(eye_lbl);

    /* dm_kb_* 共享键盘：初始隐藏，点 textarea 自动弹出，回车=连接 */
    wifi_manual_kb = dm_kb_create(wifi_manual_overlay, wifi_manual_psk_ta,
                                  NULL, NULL);

    lv_obj_t *btnrow = lv_obj_create(box);
    lv_obj_set_size(btnrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_pad_all(btnrow, 0, 0);
    lv_obj_set_style_pad_column(btnrow, DM(8), 0);

    /* 2026-08-10 修复（-31）：连接=主按钮（蓝底白字），取消=次按钮（浅灰底深字） */
    lv_obj_t *ok = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, DM(40));
    lv_obj_set_style_radius(ok, RAD_CARD, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_add_event_cb(ok, wifi_manual_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_l = lv_label_create(ok);
    lv_label_set_text(ok_l, "连接");
    lv_obj_set_style_text_font(ok_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(ok_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ok_l);

    lv_obj_t *cancel = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, DM(40));
    lv_obj_set_style_radius(cancel, RAD_CARD, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, wifi_manual_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "取消");
    lv_obj_set_style_text_font(cancel_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(cancel_l);
}

/* 2026-08-10 修复（-32）：密码明文/密文切换（眼睛图标）——
 * user_data = textarea；图标在按钮第一个子对象。
 * 语义：当前密文(hide) → 显示 open 眼（点它看明文）；
 * 当前明文 → 显示 slash 眼（点它恢复密文）。 */
static void wifi_ta_eye_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    if (!ta || !btn) return;
    bool hide = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, !hide);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl)
        lv_label_set_text(lbl, hide ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

/* 密码输入层：确认 → 起后台连接；取消 → 关闭输入层 */
static void wifi_pwd_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *psk = wifi_pwd_ta ? lv_textarea_get_text(wifi_pwd_ta) : "";
    if (wifi_pwd_ssid[0]) {
        /* 2026-09-10 P206：同手动加入——并发拒绝(-1)时提示"请稍候" */
        if (dm_net_wifi_connect_start(wifi_pwd_ssid, psk[0] ? psk : NULL) != 0) {
            if (wifi_status_lbl)
                lv_label_set_text(wifi_status_lbl, "正在连接中，请稍候");
        } else if (wifi_status_lbl) {
            lv_label_set_text_fmt(wifi_status_lbl, "正在连接 %s…", wifi_pwd_ssid);
        }
    }
    if (wifi_pwd_overlay) {
        dm_kb_destroy(wifi_pwd_kb);
        wifi_pwd_kb = NULL;
        lv_obj_del(wifi_pwd_overlay);
        wifi_pwd_overlay = NULL;
        wifi_pwd_ta     = NULL;
    }
}

static void wifi_pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_pwd_overlay) {
        dm_kb_destroy(wifi_pwd_kb);
        wifi_pwd_kb = NULL;
        lv_obj_del(wifi_pwd_overlay);
        wifi_pwd_overlay = NULL;
        wifi_pwd_ta     = NULL;
    }
}

/* 点击清单中的某个 AP 行 */
static void wifi_ap_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    dm_net_ap_t ap;

    if (dm_net_wifi_scan_get(idx, &ap) != 0)
        return;

    if (ap.security == 0) {          /* OPEN：直连无需密码 */
        /* 2026-09-10 P206：并发拒绝(-1)时提示"请稍候"，防"正在连接新SSID"误导 */
        if (dm_net_wifi_connect_start(ap.ssid, NULL) != 0) {
            if (wifi_status_lbl)
                lv_label_set_text(wifi_status_lbl, "正在连接中，请稍候");
        } else if (wifi_status_lbl) {
            lv_label_set_text_fmt(wifi_status_lbl, "正在连接 %s…", ap.ssid);
        }
        return;
    }

    /* 加密网络：弹出密码输入层（全屏，覆盖在子页之上） */
    if (wifi_pwd_overlay)
        return;
    /* wifijianyi.md 扫雷漏洞 2：strncpy 不保证补 \0，满 32 字节 SSID
     * 会越界打印脏数据 → 必须显式置终止符。 */
    strncpy(wifi_pwd_ssid, ap.ssid, sizeof(wifi_pwd_ssid) - 1);
    wifi_pwd_ssid[sizeof(wifi_pwd_ssid) - 1] = '\0';

    lv_obj_t *scr = lv_scr_act();
    wifi_pwd_overlay = lv_obj_create(scr);
    lv_obj_set_size(wifi_pwd_overlay,
                    lv_disp_get_hor_res(lv_disp_get_default()),
                    lv_disp_get_ver_res(lv_disp_get_default()));
    lv_obj_set_style_bg_color(wifi_pwd_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_pwd_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(wifi_pwd_overlay, 0, 0);
    lv_obj_set_style_pad_all(wifi_pwd_overlay, 0, 0);
    /* wifijianyi.md：遮罩吸收点击，防用户狂点背后列表引发重入 */
    lv_obj_add_flag(wifi_pwd_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(wifi_pwd_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(wifi_pwd_overlay);

    lv_obj_t *box = lv_obj_create(wifi_pwd_overlay);
    /* 2026-08-10 修复（-32）：卡片缩小 50%（宽 80%→40%）+ 再上移（+DM(20)→+DM(8)），
     * 键盘完全不挡窗口（-30 已解遮挡，-32 优化尺寸/位置） */
    lv_obj_set_size(box, lv_pct(40), LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, TOP_BAR_H + DM(8));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    /* 2026-08-10 修复（-31）：Liquid Glass 卡片——半透明白底+白边+柔和阴影
     * （skill 玻璃配方），与 Settings/WiFi 子页卡片同一语言 */
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(box, 10, 0);
    lv_obj_set_style_shadow_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(box, DM(24), 0);
    lv_obj_set_style_pad_row(box, DM(16), 0);
    lv_obj_set_style_radius(box, RAD_CARD, 0);

    lv_obj_t *ttl = lv_label_create(box);
    lv_label_set_text_fmt(ttl, "%s 连接 %s", LV_SYMBOL_WIFI, wifi_pwd_ssid);
    lv_obj_set_style_text_font(ttl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(COL_TEXT), 0);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ttl, lv_pct(100));

    /* 2026-08-10 修复（-32）：密码行 = 输入框 + 眼睛按钮（明文/密文切换） */
    lv_obj_t *pwd_row = lv_obj_create(box);
    lv_obj_set_size(pwd_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pwd_row, LV_FLEX_FLOW_ROW);
    /* 2026-08-10 修复（-33）：眼睛按钮与密码栏纵向居中（cross CENTER） */
    lv_obj_set_flex_align(pwd_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(pwd_row, 0, 0);
    lv_obj_set_style_bg_opa(pwd_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(pwd_row, 0, 0);
    lv_obj_set_style_pad_column(pwd_row, DM(4), 0);

    wifi_pwd_ta = lv_textarea_create(pwd_row);
    lv_obj_set_size(wifi_pwd_ta, LV_SIZE_CONTENT, DM(60));
    lv_obj_set_flex_grow(wifi_pwd_ta, 1);
    lv_textarea_set_password_mode(wifi_pwd_ta, true);
    lv_textarea_set_one_line(wifi_pwd_ta, true);
    lv_textarea_set_placeholder_text(wifi_pwd_ta, "输入密码");
    lv_obj_set_style_text_font(wifi_pwd_ta, FONT_BODY, 0);
    /* 输入框白底 + 灰边 + 圆角（对齐页面输入控件） */
    lv_obj_set_style_bg_color(wifi_pwd_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(wifi_pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_pwd_ta, 1, 0);
    lv_obj_set_style_border_color(wifi_pwd_ta, lv_color_hex(COL_SEP), 0);
    lv_obj_set_style_border_opa(wifi_pwd_ta, LV_OPA_60, 0);
    lv_obj_set_style_radius(wifi_pwd_ta, DM(8), 0);
    lv_obj_set_style_pad_all(wifi_pwd_ta, DM(8), 0);

    lv_obj_t *eye = lv_btn_create(pwd_row);
    lv_obj_set_size(eye, DM(20), DM(20));
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_add_event_cb(eye, wifi_ta_eye_cb, LV_EVENT_CLICKED, wifi_pwd_ta);
    lv_obj_t *eye_lbl = lv_label_create(eye);
    lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(eye_lbl, FONT_ICON, 0);
    lv_obj_set_style_text_color(eye_lbl, lv_color_hex(COL_SEC), 0);
    lv_obj_center(eye_lbl);

    /* dm_kb_* 共享键盘：初始隐藏，点 textarea 自动弹出 */
    wifi_pwd_kb = dm_kb_create(wifi_pwd_overlay, wifi_pwd_ta, NULL, NULL);

    lv_obj_t *btnrow = lv_obj_create(box);
    lv_obj_set_size(btnrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_pad_all(btnrow, 0, 0);
    lv_obj_set_style_pad_row(btnrow, 0, 0);
    lv_obj_set_style_pad_column(btnrow, DM(8), 0);

    /* 2026-08-10 修复（-31）：连接=主按钮（蓝底白字圆角），取消=次按钮（浅灰底深字） */
    lv_obj_t *ok = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, DM(40));
    lv_obj_set_style_radius(ok, RAD_CARD, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_add_event_cb(ok, wifi_pwd_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_l = lv_label_create(ok);
    lv_label_set_text(ok_l, "连接");
    lv_obj_set_style_text_font(ok_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(ok_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ok_l);

    lv_obj_t *cancel = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, DM(40));
    lv_obj_set_style_radius(cancel, RAD_CARD, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, wifi_pwd_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "取消");
    lv_obj_set_style_text_font(cancel_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(cancel_l);
}

/* 重建 SSID 清单（扫描完成后调用；行 user_data = AP 索引） */
static void wifi_rebuild_list(void)
{
    if (wifi_list_cont == NULL)
        return;

    lv_obj_clean(wifi_list_cont);
    char cur[33] = {0};
    dm_net_wifi_cur_ssid(cur, sizeof(cur));

    int n = dm_net_wifi_scan_count();
    for (int i = 0; i < n; i++) {
        dm_net_ap_t ap;
        if (dm_net_wifi_scan_get(i, &ap) != 0)
            break;

        lv_obj_t *row = lv_obj_create(wifi_list_cont);
        lv_obj_set_size(row, lv_pct(100), DM(56));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, DM(12), 0);
        /* 2026-08-10：分组卡内透明行（对齐 Files 列表行：无底/无圆角/按下变灰） */
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, DM(16), 0);
        lv_obj_set_style_pad_right(row, DM(16), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xE5E5EA), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, wifi_ap_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);

        /* 左侧蓝色 WiFi 图标徽章（对齐 Files 行图标徽章风格） */
        lv_obj_t *wbox = lv_obj_create(row);
        lv_obj_set_size(wbox, DM(36), DM(36));
        lv_obj_set_style_radius(wbox, RAD_CARD, 0);
        lv_obj_set_style_bg_color(wbox, lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_bg_opa(wbox, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(wbox, 6, 0);
        lv_obj_set_style_shadow_color(wbox, lv_color_hex(COL_BLUE), 0);
        lv_obj_set_style_shadow_opa(wbox, LV_OPA_30, 0);
        lv_obj_set_style_border_width(wbox, 0, 0);
        lv_obj_clear_flag(wbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *wi = lv_label_create(wbox);
        lv_label_set_text(wi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(wi, FONT_ICON, 0);
        lv_obj_set_style_text_color(wi, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(wi);

        lv_obj_t *nm = lv_label_create(row);
        /* wifijianyi.md：SSID 长度未知，强制省略号截断防行高撑爆 */
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, ap.ssid);
        /* 2026-08-10：去掉固定 pct(70)（与 flex_grow 冲突，且左侧徽章已占位） */
        lv_obj_set_style_text_font(nm, FONT_BODY, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(nm, 1);

        /* 右侧：已连接 / 加密锁 + 信号 */
        char meta[48];
        if (cur[0] && strcmp(ap.ssid, cur) == 0)
            snprintf(meta, sizeof(meta), "%s 已连接", LV_SYMBOL_WIFI);
        else
            snprintf(meta, sizeof(meta), "%s%s %d dBm",
                     ap.security ? LV_SYMBOL_BELL : LV_SYMBOL_WIFI,
                     ap.security ? "" : "", (int)ap.rssi);
        lv_obj_t *mt = lv_label_create(row);
        lv_label_set_text(mt, meta);
        lv_obj_set_style_text_font(mt, FONT_BODY, 0);
        lv_obj_set_style_text_color(mt,
            (cur[0] && strcmp(ap.ssid, cur) == 0)
                ? lv_color_hex(COL_GREEN) : lv_color_hex(COL_SEC), 0);
    }
}

/* 轮询：扫描完成 → 重建清单；连接状态变化 → 更新状态/保存配置 */
static void wifi_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 扫描失败（驱动忙/超时）：提示重试，避免永远卡"扫描中…" */
    if (dm_net_wifi_scan_failed() && wifi_status_lbl) {
        lv_label_set_text(wifi_status_lbl, "扫描失败，请点\"重新扫描\"重试");
    } else if (dm_net_wifi_scan_done() && wifi_list_cont && !wifi_list_built) {
        wifi_rebuild_list();
        wifi_list_built = 1;   /* P215：本轮已建，粘滞标志不再触发重建 */
    }

    int st = dm_net_wifi_connect_state();
    if (st != wifi_last_conn_state && wifi_status_lbl) {
        if (st == 2) {   /* 连接成功：刷新状态栏/清单
                            * （配置由连接线程存真 psk，此处禁 save_conf 覆写） */
            char cur[33] = {0};
            dm_net_wifi_cur_ssid(cur, sizeof(cur));
            lv_label_set_text_fmt(wifi_status_lbl, "已连接 %s", cur);
            if (wifi_conn_lbl)
                lv_label_set_text_fmt(wifi_conn_lbl, "%s 已连接", cur);
            /* 2026-09-10 P206：连上才亮断开按钮 */
            if (wifi_disc_btn)
                lv_obj_clear_flag(wifi_disc_btn, LV_OBJ_FLAG_HIDDEN);
            if (wifi_saved_lbl)
                lv_label_set_text_fmt(wifi_saved_lbl, "%s 已保存", cur);
            wifi_rebuild_list();
            dm_net_status_refresh();
        } else if (st == 3) {
            lv_label_set_text(wifi_status_lbl,
                              "连接失败：密码错误或无法连接");
            if (wifi_conn_lbl)
                lv_label_set_text(wifi_conn_lbl, "未连接网络");
            if (wifi_disc_btn)
                lv_obj_add_flag(wifi_disc_btn, LV_OBJ_FLAG_HIDDEN);
        }
        wifi_last_conn_state = st;
    }
}

/* wifijianyi.md 幽灵定时器修复：子页容器销毁（LV_EVENT_DELETE）时自动
 * 删除轮询 timer 并置 NULL，杜绝野指针——否则关闭子页后 timer 仍在
 * 跑，下一次回调访问已销毁的 wifi_list_cont 必然 Data Abort。 */
static void wifi_page_destructor_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_timer) {
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    }
    wifi_status_lbl = NULL;
    wifi_list_cont  = NULL;
    wifi_conn_lbl   = NULL;
    wifi_disc_btn   = NULL;
    wifi_saved_card = NULL;
    wifi_saved_lbl  = NULL;
    wifi_list_built = 0;   /* P215：下次进页新扫描重建 */
    /* wifijianyi.md：退出子页 = 放弃在途连接——递增代数丢弃旧结果 */
    dm_net_wifi_connect_abort();
}

void ui_wifi_create(lv_obj_t *parent)
{
    LV_LOG_USER("create_wifi_subpage called! parent=%p", parent);
    /* 2026-08-10 修复：缺 flex column 导致标题/状态/列表全部叠在左上角
     * （对照 Files/Books/Settings 均有此行）。补上 + 层间距统一 DM(6)。 */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, DM(6), 0);

    subpage_big_title(parent, "Wi-Fi");

    /* ── 状态卡（Liquid Glass）：当前状态 + 重新扫描按钮 ── */
    lv_obj_t *head = lv_obj_create(parent);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(head, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_70, 0);
    lv_obj_set_style_radius(head, RAD_CARD, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(head, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(head, 6, 0);
    lv_obj_set_style_shadow_opa(head, LV_OPA_10, 0);
    lv_obj_set_style_pad_left(head, DM(16), 0);
    lv_obj_set_style_pad_right(head, DM(8), 0);
    lv_obj_set_style_pad_top(head, DM(8), 0);
    lv_obj_set_style_pad_bottom(head, DM(8), 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    wifi_status_lbl = lv_label_create(head);
    lv_obj_set_style_text_font(wifi_status_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(wifi_status_lbl, 1);

    lv_obj_t *scan_btn = lv_btn_create(head);
    lv_obj_set_size(scan_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(scan_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_20, 0);
    lv_obj_set_style_border_width(scan_btn, 0, 0);
    lv_obj_add_event_cb(scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH " 重新扫描");
    lv_obj_set_style_text_font(scan_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(COL_BLUE), 0);

    /* ── 2026-08-10 iPad 风格：Wi-Fi 主开关卡（接驱动开/关） ── */
    lv_obj_t *ctrl = settings_group_card(parent);
    settings_row_switch_cb(ctrl, LV_SYMBOL_WIFI, COL_BLUE, "Wi-Fi",
                           dm_net_wifi_is_up() != 0, wifi_switch_cb);

    /* ── 已连接网络卡（iPad 列表顶部"当前网络"）── */
    lv_obj_t *conn_card = lv_obj_create(parent);
    lv_obj_set_size(conn_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(conn_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(conn_card, LV_OPA_70, 0);
    lv_obj_set_style_radius(conn_card, RAD_CARD, 0);
    lv_obj_set_style_border_width(conn_card, 1, 0);
    lv_obj_set_style_border_color(conn_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(conn_card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(conn_card, 6, 0);
    lv_obj_set_style_shadow_opa(conn_card, LV_OPA_10, 0);
    lv_obj_set_style_pad_left(conn_card, DM(16), 0);
    lv_obj_set_style_pad_right(conn_card, DM(16), 0);
    lv_obj_set_style_pad_top(conn_card, DM(10), 0);
    lv_obj_set_style_pad_bottom(conn_card, DM(10), 0);
    lv_obj_set_flex_flow(conn_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(conn_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(conn_card, LV_OBJ_FLAG_SCROLLABLE);
    wifi_conn_lbl = lv_label_create(conn_card);
    lv_obj_set_style_text_font(wifi_conn_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(wifi_conn_lbl, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_flex_grow(wifi_conn_lbl, 1);   /* 2026-09-10 P206：挤出右侧断开位 */
    {
        char cur[33] = {0};
        if (dm_net_wifi_cur_ssid(cur, sizeof(cur)) == 0 && cur[0])
            lv_label_set_text_fmt(wifi_conn_lbl, "%s 已连接", cur);
        else
            lv_label_set_text(wifi_conn_lbl, "未连接网络");
    }
    /* 2026-09-10 P206：已连接卡"断开"按钮（wifi_disconnect，无 forget 接口）。
     * 两修：①Liquid Glass 药丸（半透明白+白边+红字，对齐 AI 快问 chips 设计语言，
     * 原生蓝方块太丑）；②进页按实际连接态显隐——没连也摆断开是 bug。 */
    {
        bool connected = false;
        {
            char cur0[33] = {0};
            connected = (dm_net_wifi_cur_ssid(cur0, sizeof(cur0)) == 0 && cur0[0]);
        }
        wifi_disc_btn = lv_btn_create(conn_card);
        lv_obj_set_size(wifi_disc_btn, LV_SIZE_CONTENT, DM(32));
        lv_obj_set_style_radius(wifi_disc_btn, DM(16), 0);
        lv_obj_set_style_bg_color(wifi_disc_btn, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(wifi_disc_btn, LV_OPA_60, 0);
        lv_obj_set_style_border_color(wifi_disc_btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(wifi_disc_btn, 2, 0);
        lv_obj_set_style_border_opa(wifi_disc_btn, LV_OPA_50, 0);
        lv_obj_set_style_pad_left(wifi_disc_btn, DM(14), 0);
        lv_obj_set_style_pad_right(wifi_disc_btn, DM(14), 0);
        lv_obj_add_event_cb(wifi_disc_btn, wifi_disc_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *disc_l = lv_label_create(wifi_disc_btn);
        lv_label_set_text(disc_l, "断开");
        lv_obj_set_style_text_font(disc_l, FONT_BODY, 0);
        lv_obj_set_style_text_color(disc_l, lv_color_hex(COL_BLUE), 0);
        lv_obj_center(disc_l);
        if (!connected)
            lv_obj_add_flag(wifi_disc_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── SSID 清单容器（Liquid Glass 分组卡，行随 Files 风格）── */
    wifi_list_cont = lv_obj_create(parent);
    lv_obj_set_size(wifi_list_cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wifi_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(wifi_list_cont, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(wifi_list_cont, LV_OPA_70, 0);
    lv_obj_set_style_radius(wifi_list_cont, RAD_CARD, 0);
    lv_obj_set_style_clip_corner(wifi_list_cont, true, 0);
    lv_obj_set_style_shadow_width(wifi_list_cont, 6, 0);
    lv_obj_set_style_shadow_opa(wifi_list_cont, LV_OPA_10, 0);
    lv_obj_set_style_border_width(wifi_list_cont, 1, 0);
    lv_obj_set_style_border_color(wifi_list_cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(wifi_list_cont, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(wifi_list_cont, 0, 0);
    lv_obj_set_style_pad_row(wifi_list_cont, 4, 0);   /* 行间距（Files 同款） */
    lv_obj_clear_flag(wifi_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(wifi_list_cont, LV_SCROLLBAR_MODE_OFF);

    /* ── 2026-08-10 iPad 风格："其他网络…"手动加入按钮 ── */
    lv_obj_t *other_btn = lv_btn_create(parent);
    lv_obj_set_size(other_btn, lv_pct(100), DM(56));
    lv_obj_set_style_bg_color(other_btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(other_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(other_btn, RAD_CARD, 0);
    lv_obj_set_style_border_width(other_btn, 1, 0);
    lv_obj_set_style_border_color(other_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(other_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(other_btn, 6, 0);
    lv_obj_set_style_shadow_opa(other_btn, LV_OPA_10, 0);
    lv_obj_set_flex_flow(other_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(other_btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(other_btn, DM(16), 0);
    lv_obj_add_event_cb(other_btn, wifi_manual_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *other_lbl = lv_label_create(other_btn);
    lv_label_set_text(other_lbl, LV_SYMBOL_PLUS " 其他网络…");
    lv_obj_set_style_text_font(other_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(other_lbl, lv_color_hex(COL_BLUE), 0);

    /* ── 2026-08-10 iPad 风格：已保存网络卡（读 wapi.conf）── */
    wifi_saved_card = lv_obj_create(parent);
    lv_obj_set_size(wifi_saved_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(wifi_saved_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(wifi_saved_card, LV_OPA_70, 0);
    lv_obj_set_style_radius(wifi_saved_card, RAD_CARD, 0);
    lv_obj_set_style_border_width(wifi_saved_card, 1, 0);
    lv_obj_set_style_border_color(wifi_saved_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(wifi_saved_card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(wifi_saved_card, 6, 0);
    lv_obj_set_style_shadow_opa(wifi_saved_card, LV_OPA_10, 0);
    lv_obj_set_style_pad_left(wifi_saved_card, DM(16), 0);
    lv_obj_set_style_pad_right(wifi_saved_card, DM(16), 0);
    lv_obj_set_style_pad_top(wifi_saved_card, DM(10), 0);
    lv_obj_set_style_pad_bottom(wifi_saved_card, DM(10), 0);
    lv_obj_set_flex_flow(wifi_saved_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_saved_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wifi_saved_card, LV_OBJ_FLAG_SCROLLABLE);
    wifi_saved_lbl = lv_label_create(wifi_saved_card);
    lv_obj_set_style_text_font(wifi_saved_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(wifi_saved_lbl, lv_color_hex(COL_SEC), 0);
    char saved[33] = {0};
    if (dm_net_wifi_saved_ssid(saved, sizeof(saved)) == 0 && saved[0])
        lv_label_set_text_fmt(wifi_saved_lbl, "%s 已保存", saved);
    else
        lv_label_set_text(wifi_saved_lbl, "未保存网络");

    /* ── 2026-08-10 iPad 风格：底部提示区 ── */
    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint,
        "连接过的网络将自动保存，开机自动重连。附近网络需在路由器开启 SSID 广播。");
    lv_obj_set_style_text_font(hint, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_SEC), 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(hint, DM(4), 0);

    /* 轮询 timer（500ms）：扫描完成重建清单、连接状态刷新 */
    wifi_last_conn_state = dm_net_wifi_connect_state();
    wifi_timer = lv_timer_create(wifi_poll_cb, 500, NULL);
    if (wifi_timer) lv_timer_set_repeat_count(wifi_timer, -1);

    /* wifijianyi.md：timer 绑定子页容器生命周期——容器销毁时
     * LV_EVENT_DELETE 触发 wifi_page_destructor_cb 删 timer，防野指针 */
    lv_obj_add_event_cb(parent, wifi_page_destructor_cb, LV_EVENT_DELETE, NULL);

    /* 进入子页即自动扫描（失败时由 wifi_poll_cb 提示重试） */
    wifi_list_built = 0;   /* P215：新一轮扫描，允许重建一次 */
    if (dm_net_wifi_scan_start() != 0) {
        if (wifi_status_lbl)
            lv_label_set_text(wifi_status_lbl, "Wi-Fi 忙，请稍后再试");
    } else if (wifi_status_lbl) {
        lv_label_set_text(wifi_status_lbl, "扫描中…");
    }
}
