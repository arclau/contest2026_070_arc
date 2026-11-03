/*
 * ui_pet.c — 宠物子页【2026-09-10 P206：Settings 宠物区独立成页】
 *
 * 2026-09-09 宠物行全堆在 Settings（名字/种类/成长/状态/双开关/重养占半屏），
 * 先折叠后用户拍板：学 WiFi 做嵌套子页，Settings 只留一行入口。
 * 全部逻辑从 ui_settings.c 原样搬入（P202 命名 overlay 确定/取消闭环、
 * P205 三开关/清档二次确认，行为一字不改）；对象+清理一起搬，禁止拆散。
 *
 * 跨页耦合：起名/重置 overlay 挂 subpage overlay（FLOATING），close_subpage
 * 经 ui_pet_close_cleanup() 清理（修复旧隐患：开着起名页退子页，
 * g_pet_name_overlay 野指针致命名永久点不开，只能重启）。
 */

#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_pet.h"                    /* 宠物数据/开关/重置 */
#include "pet_renderer.h"

/* Independent preview: does not alter the pet's save, mood or lock-screen state. */
static pet_renderer_t g_preview;
static lv_timer_t *g_preview_timer;
static lv_obj_t *g_preview_choices;
static lv_obj_t *g_preview_status;
static pet_action_t g_preview_action;
static uint32_t g_preview_instance;
static bool g_preview_held;
static bool g_preview_mirror;

static void pet_preview_clear(void)
{
    if (g_preview_timer) { lv_timer_del(g_preview_timer);g_preview_timer=NULL; }
    if (g_preview.image && lv_obj_is_valid(g_preview.image)) lv_image_set_src(g_preview.image,NULL);
    pet_renderer_clear(&g_preview);
    g_preview_choices=NULL;g_preview_status=NULL;g_preview_held=false;
}

static void pet_preview_tick(lv_timer_t *timer)
{
    (void)timer;
    pet_renderer_step(&g_preview,g_preview_action,g_preview_instance,
        g_preview_held,g_preview_mirror,lv_tick_get());
    if (g_preview_status)
        lv_label_set_text(g_preview_status,g_preview.resource_error ? "素材加载失败" :
            (g_preview_action==PET_ACTION_PET_NUZZLE ? "按住猫猫贴贴，松手收势" : g_pet_actions[g_preview_action].label));
}

static void pet_preview_start(pet_action_t action)
{
    g_preview_action=action;++g_preview_instance;g_preview_held=false;
    pet_preview_tick(NULL);
}

static void pet_preview_choose(lv_event_t *event)
{
    pet_preview_start((pet_action_t)lv_dropdown_get_selected(lv_event_get_target(event)));
}

static void pet_preview_control(lv_event_t *event)
{
    int command=(int)(intptr_t)lv_event_get_user_data(event);
    if (command==2) g_preview_mirror=!g_preview_mirror;
    else if (command) g_preview_action=(pet_action_t)(((int)g_preview_action+command+PET_ACTION_COUNT)%PET_ACTION_COUNT);
    lv_dropdown_set_selected(g_preview_choices,g_preview_action);
    pet_preview_start(g_preview_action);
}

static void pet_preview_touch(lv_event_t *event)
{
    if (g_preview_action!=PET_ACTION_PET_NUZZLE) return;
    bool pressed=lv_event_get_code(event)==LV_EVENT_PRESSED;
    g_preview_held=pressed;
    if (pressed) ++g_preview_instance;
    pet_preview_tick(NULL);
}

static void pet_preview_deleted(lv_event_t *event)
{
    (void)event;pet_preview_clear();
}

static void pet_preview_create(lv_obj_t *parent)
{
    pet_preview_clear();
    lv_obj_t *card=settings_group_card(parent);
    lv_obj_set_style_pad_all(card,DM(10),0);
    lv_obj_set_style_pad_row(card,DM(6),0);
    lv_obj_add_event_cb(card,pet_preview_deleted,LV_EVENT_DELETE,NULL);
    lv_obj_t *title=lv_label_create(card);
    lv_label_set_text(title,"动作预览");lv_obj_set_style_text_font(title,FONT_BODY,0);
    lv_obj_set_style_text_color(title,lv_color_hex(COL_TEXT),0);
    lv_obj_t *stage=lv_obj_create(card);
    lv_obj_remove_style_all(stage);
    lv_obj_set_size(stage,LV_PCT(100),DM(116));
    lv_obj_clear_flag(stage,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stage,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage,pet_preview_touch,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(stage,pet_preview_touch,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(stage,pet_preview_touch,LV_EVENT_PRESS_LOST,NULL);
    lv_obj_t *image=lv_image_create(stage);
    lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE);
    pet_renderer_init(&g_preview,image,DM(107));lv_obj_center(image);
    g_preview_status=lv_label_create(card);
    lv_obj_set_style_text_font(g_preview_status,FONT_CAPTION,0);
    lv_obj_set_style_text_color(g_preview_status,lv_color_hex(COL_SEC),0);
    g_preview_choices=lv_dropdown_create(card);
    lv_obj_set_width(g_preview_choices,LV_PCT(100));
    lv_obj_set_style_text_font(g_preview_choices,FONT_BODY,0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(g_preview_choices),FONT_BODY,0);
    char options[256]={0};size_t used=0;
    for (int i=0;i<PET_ACTION_COUNT;i++) {
        int n=snprintf(options+used,sizeof(options)-used,"%s%s",i ? "\n" : "",g_pet_actions[i].label);
        if (n<0 || (size_t)n>=sizeof(options)-used) break;
        used+=(size_t)n;
    }
    lv_dropdown_set_options(g_preview_choices,options);
    lv_obj_add_event_cb(g_preview_choices,pet_preview_choose,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_t *controls=lv_obj_create(card);lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls,LV_PCT(100),DM(34));
    lv_obj_set_flex_flow(controls,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(controls,DM(6),0);
    const char *labels[]={"上一个","重播","下一个","转向"};
    const int commands[]={-1,0,1,2};
    for (unsigned i=0;i<4;i++) {
        lv_obj_t *button=lv_button_create(controls);
        lv_obj_set_flex_grow(button,1);lv_obj_set_height(button,LV_PCT(100));
        lv_obj_set_style_bg_color(button,lv_color_hex(COL_BLUE),0);
        lv_obj_set_style_radius(button,DM(8),0);
        lv_obj_add_event_cb(button,pet_preview_control,LV_EVENT_CLICKED,(void *)(intptr_t)commands[i]);
        lv_obj_t *label=lv_label_create(button);lv_label_set_text(label,labels[i]);
        lv_obj_set_style_text_font(label,FONT_BODY,0);lv_obj_center(label);
    }
    g_preview_mirror=false;pet_preview_start(PET_ACTION_IDLE_SIT);
    g_preview_timer=lv_timer_create(pet_preview_tick,33,NULL);
}

/* ================================================================
 * 宠物设置状态（原 ui_settings.c）
 * ================================================================ */

static lv_obj_t *g_pet_name_lbl = NULL;
static lv_obj_t *g_pet_stage_lbl = NULL;
static lv_obj_t *g_pet_status_lbl = NULL;

/* 宠物起名浮层（P202 根因：旧 overlay 无确定/取消、无关闭路径，
 * 键盘 READY 回调只删 keyboard 不删 overlay → 半透明遮罩永久吞点击，
 * 用户即“卡死在命名里”；CANCEL 更是什么都不做） */
static lv_obj_t *g_pet_name_overlay = NULL;
static lv_obj_t *g_pet_name_ta = NULL;

/* 宠物名字输入键盘状态 */
static dm_kb_state_t *g_pet_name_kb = NULL;

static void settings_pet_name_cleanup(void)
{
    if (g_pet_name_kb) {
        dm_kb_destroy(g_pet_name_kb);
        g_pet_name_kb = NULL;
    }
    if (g_pet_name_overlay) {
        if (lv_obj_is_valid(g_pet_name_overlay))
            lv_obj_del(g_pet_name_overlay);
        g_pet_name_overlay = NULL;
    }
    g_pet_name_ta = NULL;
}

/* 确定：非空保存 + 全量清理（overlay 必删） */
static void settings_pet_name_confirm(void)
{
    if (g_pet_name_ta && lv_obj_is_valid(g_pet_name_ta)) {
        const char *name = lv_textarea_get_text(g_pet_name_ta);
        if (name && *name) {
            dm_pet_set_name(name);
            if (g_pet_name_lbl && lv_obj_is_valid(g_pet_name_lbl))
                lv_label_set_text(g_pet_name_lbl, name);
            LV_LOG_USER("Pet name set to: %s", name);
        }
    }
    settings_pet_name_cleanup();
}

static void settings_pet_name_ready_cb(lv_obj_t *ta, void *user_data)
{
    /* 键盘 ✓ 键 = 确定 */
    (void)ta;
    (void)user_data;
    settings_pet_name_confirm();
}

static void settings_pet_name_ok_cb(lv_event_t *e)
{
    (void)e;
    settings_pet_name_confirm();
}

static void settings_pet_name_cancel_cb(lv_event_t *e)
{
    (void)e;
    settings_pet_name_cleanup();
}

static void settings_pet_name_cb(lv_event_t *e)
{
    (void)e;
    if (g_pet_name_overlay) return;  /* 防连点叠遮罩（叠两层即假卡死） */
    const dm_pet_data_t *pd = dm_pet_get_data();
    if (!pd) return;

    /* 在当前 subpage overlay 上创建全屏遮罩（和 WiFi 密码输入同模式）
     * 不能直接在 scrollable card 上建 textarea+keyboard——
     * LVGL 9 键盘弹出时布局计算会崩 */
    lv_obj_t *subpage = lv_obj_get_parent(lv_event_get_current_target(e));
    /* 往上找到 subpage overlay（FLOATING 的全屏容器） */
    while (subpage && !lv_obj_has_flag(subpage, LV_OBJ_FLAG_FLOATING))
        subpage = lv_obj_get_parent(subpage);
    if (!subpage) return;

    lv_obj_t *overlay = lv_obj_create(subpage);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE); /* 吞点击防穿透（不点空白关闭，防误触丢输入） */
    g_pet_name_overlay = overlay;

    /* 白卡：标题 + 输入框 + 确定/取消（WiFi 手动连接同款双按钮） */
    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, DM(320), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, DM(16), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, DM(12), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, DM(8), 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "给宠物起名");
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_obj_set_size(ta, LV_PCT(100), DM(44));
    lv_textarea_set_one_line(ta, true);
    /* name[32] 字节 ≈ 10 个汉字；旧“≤15字”写超会被 strncpy 静默截断 */
    lv_textarea_set_max_length(ta, 10);
    lv_textarea_set_placeholder_text(ta, "输入名字（≤10字）");
    lv_textarea_set_text(ta, pd->name[0] ? pd->name : "维拉");
    lv_obj_set_style_text_font(ta, FONT_BODY, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(ta, DM(8), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xCCCCCC), 0);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    g_pet_name_ta = ta;

    lv_obj_t *btnrow = lv_obj_create(box);
    lv_obj_set_size(btnrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_pad_all(btnrow, 0, 0);
    lv_obj_set_style_pad_column(btnrow, DM(8), 0);

    lv_obj_t *cancel = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, DM(40));
    lv_obj_set_style_radius(cancel, DM(12), 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, settings_pet_name_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "取消");
    lv_obj_set_style_text_font(cancel_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_l, lv_color_hex(0x333333), 0);
    lv_obj_center(cancel_l);

    lv_obj_t *ok = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, DM(40));
    lv_obj_set_style_radius(ok, DM(12), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_add_event_cb(ok, settings_pet_name_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_l = lv_label_create(ok);
    lv_label_set_text(ok_l, "确定");
    lv_obj_set_style_text_font(ok_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(ok_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ok_l);

    g_pet_name_kb = dm_kb_create(overlay, ta, settings_pet_name_ready_cb, NULL);
}

/* ═══════════ 宠物开关/重置 ═══════════ */

static void settings_pet_enable_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    dm_pet_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void settings_pet_alert_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    dm_pet_set_hunger_alert(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* 名字/阶段/状态三行重刷（改名确认/重置后调） */
static void settings_pet_refresh_info(void)
{
    static const char *stage_names[] = { "幼年", "成长", "成年" };
    const dm_pet_data_t *pd = dm_pet_get_data();
    const char *pet_name = (pd && pd->name[0]) ? pd->name : "维拉";
    if (g_pet_name_lbl && lv_obj_is_valid(g_pet_name_lbl))
        lv_label_set_text(g_pet_name_lbl, pet_name);
    if (g_pet_stage_lbl && lv_obj_is_valid(g_pet_stage_lbl))
        lv_label_set_text(g_pet_stage_lbl, stage_names[dm_pet_get_life_stage()]);
    if (g_pet_status_lbl && lv_obj_is_valid(g_pet_status_lbl)) {
        char status[64];
        if (pd)
            snprintf(status, sizeof(status), "饱%d 力%d 乐%d 亲%d 健%d 成长%d",
                     100 - pd->attrs.hunger, pd->attrs.energy, pd->attrs.happiness,
                     pd->attrs.affection, pd->attrs.health, pd->attrs.growth);
        else
            snprintf(status, sizeof(status), "--");
        lv_label_set_text(g_pet_status_lbl, status);
    }
}

static lv_obj_t *g_pet_reset_overlay = NULL;

static void settings_pet_reset_cleanup(void)
{
    if (g_pet_reset_overlay) {
        if (lv_obj_is_valid(g_pet_reset_overlay))
            lv_obj_del(g_pet_reset_overlay);
        g_pet_reset_overlay = NULL;
    }
}

static void settings_pet_reset_ok_cb(lv_event_t *e)
{
    (void)e;
    dm_pet_reset();
    settings_pet_refresh_info();
    settings_pet_reset_cleanup();
}

static void settings_pet_reset_cancel_cb(lv_event_t *e)
{
    (void)e;
    settings_pet_reset_cleanup();
}

/* 重新开始：二次确认（名字/成长/属性全清，开关保持） */
static void settings_pet_reset_cb(lv_event_t *e)
{
    if (g_pet_reset_overlay) return;
    lv_obj_t *subpage = lv_obj_get_parent(lv_event_get_current_target(e));
    while (subpage && !lv_obj_has_flag(subpage, LV_OBJ_FLAG_FLOATING))
        subpage = lv_obj_get_parent(subpage);
    if (!subpage) return;

    lv_obj_t *overlay = lv_obj_create(subpage);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    g_pet_reset_overlay = overlay;

    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, DM(320), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, DM(16), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, DM(12), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, DM(8), 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "重新开始养？");
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

    lv_obj_t *desc = lv_label_create(box);
    lv_label_set_text(desc, "名字/成长/属性全部清零");
    lv_obj_set_style_text_font(desc, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(desc, lv_color_hex(0x888888), 0);

    lv_obj_t *btnrow = lv_obj_create(box);
    lv_obj_set_size(btnrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_pad_all(btnrow, 0, 0);
    lv_obj_set_style_pad_column(btnrow, DM(8), 0);

    lv_obj_t *cancel = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, DM(40));
    lv_obj_set_style_radius(cancel, DM(12), 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, settings_pet_reset_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "取消");
    lv_obj_set_style_text_font(cancel_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_l, lv_color_hex(0x333333), 0);
    lv_obj_center(cancel_l);

    lv_obj_t *ok = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, DM(40));
    lv_obj_set_style_radius(ok, DM(12), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_add_event_cb(ok, settings_pet_reset_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_l = lv_label_create(ok);
    lv_label_set_text(ok_l, "清档重养");
    lv_obj_set_style_text_font(ok_l, FONT_BODY, 0);
    lv_obj_set_style_text_color(ok_l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ok_l);
}

/* close_subpage 清理：起名键盘/浮层 + 重置浮层（对象随父 overlay 删除，
 * 此处只销毁 kb 状态 + 置空指针，防重进野指针） */
void ui_pet_close_cleanup(void)
{
    pet_preview_clear();
    if (g_pet_name_kb) {
        dm_kb_destroy(g_pet_name_kb);
        g_pet_name_kb = NULL;
    }
    g_pet_name_overlay = NULL;
    g_pet_name_ta = NULL;
    g_pet_reset_overlay = NULL;
    g_pet_name_lbl = NULL;
    g_pet_stage_lbl = NULL;
    g_pet_status_lbl = NULL;
}

void ui_pet_create(lv_obj_t *parent)
{
    /* Scrollable column container（Settings 同款） */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);

    subpage_big_title(parent, "宠物");
    pet_preview_create(parent);

    lv_obj_t *pet = settings_group_card(parent);
    {
        const dm_pet_data_t *pd = dm_pet_get_data();
        const char *pet_name = (pd && pd->name[0]) ? pd->name : "维拉";

        /* 名字行：value 显示名字（旧误传 pet_type 再事后改 label，语义错位） */
        lv_obj_t *name_row = settings_row_value(pet, LV_SYMBOL_EDIT, COL_PURPLE,
                                                "名字", pet_name, true);
        lv_obj_t *val_lbl = (lv_obj_t *)lv_obj_get_user_data(name_row);
        if (val_lbl) {
            lv_label_set_text(val_lbl, pet_name);
            g_pet_name_lbl = val_lbl;
        }
        lv_obj_add_event_cb(name_row, settings_pet_name_cb, LV_EVENT_CLICKED, NULL);
        settings_add_separator(pet);

        /* 种类行（只读：只养猫，P202 狗已删） */
        settings_row_value(pet, LV_SYMBOL_HOME, COL_SEC, "种类", "猫", false);
        settings_add_separator(pet);

        /* 成长行（只读：生命周期阶段） */
        {
            static const char *stage_names[] = { "幼年", "成长", "成年" };
            lv_obj_t *stage_row = settings_row_value(pet, LV_SYMBOL_UP, COL_GREEN, "成长",
                                                     stage_names[dm_pet_get_life_stage()],
                                                     false);
            lv_obj_t *stage_val = (lv_obj_t *)lv_obj_get_user_data(stage_row);
            if (stage_val)
                g_pet_stage_lbl = stage_val;
        }
        settings_add_separator(pet);

        /* 状态行（只读；饱=饱食度 100-hunger） */
        if (pd) {
            char status[64];
            snprintf(status, sizeof(status), "饱%d 力%d 乐%d 亲%d 健%d 成长%d",
                     100 - pd->attrs.hunger, pd->attrs.energy, pd->attrs.happiness,
                     pd->attrs.affection, pd->attrs.health, pd->attrs.growth);
            lv_obj_t *st_row = settings_row_value(pet, LV_SYMBOL_LIST, COL_SEC,
                                                  "状态", status, false);
            lv_obj_t *st_val = (lv_obj_t *)lv_obj_get_user_data(st_row);
            if (st_val)
                g_pet_status_lbl = st_val;
        }
        settings_add_separator(pet);

        /* 宠物总开关（关=锁屏不显示，立即拆 UI） */
        settings_row_switch_cb(pet, LV_SYMBOL_IMAGE, COL_GREEN, "宠物显示",
                               dm_pet_is_enabled(), settings_pet_enable_cb);
        settings_add_separator(pet);

        /* 饥饿提醒开关（关=只留饿行为，不弹红泡） */
        settings_row_switch_cb(pet, LV_SYMBOL_BELL, COL_ORANGE, "饥饿提醒",
                               dm_pet_get_hunger_alert(), settings_pet_alert_cb);
        settings_add_separator(pet);

        /* 重新开始（二次确认，开关保持） */
        lv_obj_t *reset_row = settings_row_value(pet, LV_SYMBOL_TRASH, COL_RED,
                                                 "重新开始", "清档重养", true);
        lv_obj_add_event_cb(reset_row, settings_pet_reset_cb, LV_EVENT_CLICKED, NULL);
    }
}
