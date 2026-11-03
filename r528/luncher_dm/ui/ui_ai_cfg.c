/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/ui/ui_ai_cfg.c
 * AI 语音设置子页（2026-11-01 P330）
 *
 * 目的：把「改 API / 换模型 / 换音色」从改源码编译，变成设置页点两下。
 *
 * 页面内容：
 *   1. 当前服务：小米 MiMo（大模型 / ASR / TTS）与音色；
 *   2. Key 状态：读 /data/agent/config/config.json 里的 mimo_api_key；
 *   3. 扫码配置：设备局域网配对页 http://<本机IP>:9000/ 的二维码，
 *      手机扫码 → 粘贴 Key → 保存 → ai_agent 立刻切到小米语音。
 *
 * 铁律：本页只读配置 + 展示二维码；写 Key 由 ai_agent 的配对页完成，
 *      避免 UI 进程直接改 agent 的配置文件造成并发写坏。
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "deskmate_ui.h"
#include "dm_net.h"

#define AI_CFG_FILE "/data/ai_agent/config/config.json"
#define AI_PAIR_PORT 9000

/* 读 config.json 里 "mimo_api_key" 是否有非空值。
 * 只做字符串扫描：不解析整个 JSON，避免在 UI 线程引入 cJSON 依赖。 */
static int ai_key_present(void)
{
    FILE *fp;
    char buf[2048];
    size_t n;
    const char *tag = "\"mimo_api_key\"";
    const char *p;

    fp = fopen(AI_CFG_FILE, "r");
    if (!fp)
        return 0;

    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    p = strstr(buf, tag);
    if (!p)
        return 0;

    p += strlen(tag);
    while (*p == ' ' || *p == ':')
        p++;

    return (*p == '"' && p[1] != '"') ? 1 : 0;
}

/* 当前音色（config.json 里 mimo_tts_voice，缺省"冰糖"） */
static void ai_voice_name(char *out, size_t cap)
{
    FILE *fp;
    char buf[2048];
    size_t n;
    const char *tag = "\"mimo_tts_voice\"";
    const char *p;
    const char *s;
    size_t o = 0;

    strlcpy(out, "冰糖", cap);

    fp = fopen(AI_CFG_FILE, "r");
    if (!fp)
        return;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    p = strstr(buf, tag);
    if (!p)
        return;
    p += strlen(tag);
    while (*p && *p != '"')
        p++;
    if (*p != '"')
        return;
    p++;
    s = p;
    while (*s && *s != '"' && o + 1 < cap)
        out[o++] = *s++;
    out[o] = '\0';
    if (out[0] == '\0')
        strlcpy(out, "冰糖", cap);
}

/* 点按二维码卡片 → 关闭子页。
 * P330 反馈：二维码弹出后用户找不到关闭入口（返回键在 lv_layer_top，但
 * 大二维码把视线和注意力都吸走了）。给一个"点哪都能关"的出口。 */
static void ai_cfg_card_close_cb(lv_event_t *e)
{
    (void)e;
    LV_LOG_USER("AI cfg: tap to close");
    close_subpage();
}

void ui_ai_cfg_create(lv_obj_t *parent)
{
    char ip[32] = { 0 };
    char url[64];
    char voice[32];
    int have_key = ai_key_present();
    int have_ip = (dm_net_wifi_ip(ip, sizeof(ip)) == 0);

    /* 2026-11-01 修复（真根因）：子页 content 不是 flex 容器，必须由构建函数
     * 自己设成纵向 flex——否则所有子对象都落在同一个原点互相重叠：后建的
     * 二维码卡盖住整页，分组标题被压在卡片底下（用户看到的"二维码弹出来
     * 关不掉 / 手机扫码配置 位置不对"就是这个）。与 ui_settings / ui_wifi /
     * ui_pet 等所有子页保持同一写法。 */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);

    subpage_big_title(parent, "AI 语音");

    /* ── 当前服务 ── */
    settings_section_label(parent, "当前服务");
    {
        lv_obj_t *card = settings_group_card(parent);

        settings_row_value(card, LV_SYMBOL_SETTINGS, COL_BLUE,
                           "大模型", "小米 MiMo V2.6", false);
        settings_add_separator(card);
        settings_row_value(card, LV_SYMBOL_AUDIO, COL_PURPLE,
                           "语音识别", "MiMo ASR（中英+方言）", false);
        settings_add_separator(card);
        ai_voice_name(voice, sizeof(voice));
        {
            static char vbuf[64];
            snprintf(vbuf, sizeof(vbuf), "MiMo TTS · %s", voice);
            settings_row_value(card, LV_SYMBOL_VOLUME_MAX, COL_GREEN,
                               "语音合成", vbuf, false);
        }
    }

    /* ── 密钥状态 ── */
    settings_section_label(parent, "密钥");
    {
        lv_obj_t *card = settings_group_card(parent);

        settings_row_value(card, LV_SYMBOL_KEYBOARD,
                           have_key ? COL_GREEN : COL_RED, "MiMo API Key",
                           have_key ? "已配置" : "未配置", false);
    }

    /* ── 扫码配置 ── */
    settings_section_label(parent, "手机扫码配置");
    {
        lv_obj_t *card = settings_group_card(parent);
        lv_obj_t *box = lv_obj_create(card);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, DM(10), 0);
        lv_obj_set_style_pad_row(box, DM(6), 0);
        /* 内层容器只做布局：去掉默认的滚动/点击，免得把拖动和点按吃掉，
         * 手势交还外层 content（页面能正常滚动）。 */
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, ai_cfg_card_close_cb, LV_EVENT_CLICKED, NULL);

        if (have_ip)
        {
            snprintf(url, sizeof(url), "http://%s:%d/", ip, AI_PAIR_PORT);
        }
        else
        {
            url[0] = '\0';
        }

#if defined(CONFIG_LV_USE_QRCODE) && defined(CONFIG_GRAPHICS_LVGL)
        if (have_ip)
        {
            lv_obj_t *qr = lv_qrcode_create(box);
            if (qr)
            {
                /* P330 反馈：DM(190)≈456px 太大，占掉半屏还逼得整页滚动。
                 * 收到一半（≈228px），扫码距离仍然够。 */
                lv_qrcode_set_size(qr, DM(95));
                lv_qrcode_set_dark_color(qr, lv_color_hex(COL_TEXT));
                lv_qrcode_set_light_color(qr, lv_color_white());
                lv_qrcode_update(qr, url, (uint32_t)strlen(url));
                lv_obj_set_style_border_color(qr, lv_color_hex(COL_SEP), 0);
                lv_obj_set_style_border_width(qr, 1, 0);
                lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
            }
        }
#endif

        {
            lv_obj_t *hint = lv_label_create(box);
            if (have_ip)
            {
                lv_label_set_text(hint, "手机相机扫码，粘贴 Key 后保存");
            }
            else
            {
                lv_label_set_text(hint,
                    "设备未联网，无法显示二维码\n请先在设置里连上 Wi-Fi");
            }
            lv_obj_set_style_text_font(hint, FONT_CAPTION, 0);
            lv_obj_set_style_text_color(hint, lv_color_hex(COL_SEC), 0);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        }

        if (have_ip)
        {
            lv_obj_t *addr = lv_label_create(box);
            lv_label_set_text(addr, url);
            lv_obj_set_style_text_font(addr, FONT_LABEL, 0);
            lv_obj_set_style_text_color(addr, lv_color_hex(COL_BLUE), 0);
        }

        {
            lv_obj_t *note = lv_label_create(box);
            lv_label_set_text(note, "点按此处关闭");
            lv_obj_set_style_text_font(note, FONT_CAPTION, 0);
            lv_obj_set_style_text_color(note, lv_color_hex(COL_SEC), 0);
            lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
}
