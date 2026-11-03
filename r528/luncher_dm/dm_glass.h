/* V1 Home / lock-screen material. Keep the surface cheap: one fill, one rim,
 * one symmetric shadow. The wash is drawn by the existing screen object. */
#ifndef DESKMATE_DM_GLASS_H
#define DESKMATE_DM_GLASS_H

#include <lvgl/lvgl.h>
#include "dm_res.h"

#define DM_GLASS_WALLPAPER DM_RES_ROOT "/wallpaper/blue-hour.png"

/* 壁纸：开机解码一次，按 lv_image_dsc_t 常驻（实现见 dm_glass.c）。
 * 不再用 bg_image_src = 文件路径——那条路每帧都要重新读 NAND + 解码，
 * 而读路径偶发坏字节会让解码失败，失败结果不进缓存，于是每帧重试，
 * 单轮耗时顶到 1.3s（P348 实测 765 条 decode failed/70s）。 */
int  dm_glass_wallpaper_prepare(void);
const void *dm_glass_wallpaper_src(void);

#define DM_GLASS_WASH_TOP       0xDCE7FA
#define DM_GLASS_WASH_BOTTOM    0xF4EDF6
#define DM_GLASS_CARD_OPA       LV_OPA_70
#define DM_GLASS_EDGE_OPA       LV_OPA_80
#define DM_GLASS_EDGE_WIDTH     2
#define DM_GLASS_SHADOW_COLOR   0x40516A
#define DM_GLASS_SHADOW_OPA     LV_OPA_30
#define DM_GLASS_SHADOW_WIDTH   20
#define DM_GLASS_SHADOW_Y       3
#define DM_GLASS_RADIUS         40

static inline void dm_glass_apply_wash(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(DM_GLASS_WASH_TOP), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(DM_GLASS_WASH_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

/* Home and lock screen use the same file path so LVGL can reuse its image cache.
 * Keep the existing wash as a fallback if the resource is unavailable. */
static inline void dm_glass_apply_wallpaper(lv_obj_t *obj,
                                            lv_coord_t width,
                                            lv_coord_t height)
{
    const void *src = dm_glass_wallpaper_src();
    lv_image_header_t header;

    /* 未加载/尺寸不符 → 调用方已铺的 wash 渐变保持不动 */
    if (!src)
        return;
    if (lv_image_decoder_get_info(src, &header) != LV_RESULT_OK ||
        header.w != width || header.h != height) {
        LV_LOG_WARN("V1 wallpaper size mismatch %dx%d != %dx%d",
                    (int)header.w, (int)header.h, (int)width, (int)height);
        return;
    }

    lv_obj_set_style_bg_image_src(obj, src, 0);
    lv_obj_set_style_bg_image_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(obj, false, 0);
}

/* For containers that already have local padding and interaction flags. */
static inline void dm_glass_apply_panel(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, DM_GLASS_CARD_OPA, 0);
    lv_obj_set_style_radius(obj, DM_GLASS_RADIUS, 0);
    lv_obj_set_style_border_width(obj, DM_GLASS_EDGE_WIDTH, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(obj, DM_GLASS_EDGE_OPA, 0);
    lv_obj_set_style_shadow_width(obj, DM_GLASS_SHADOW_WIDTH, 0);
    lv_obj_set_style_shadow_offset_x(obj, 0, 0);
    lv_obj_set_style_shadow_offset_y(obj, DM_GLASS_SHADOW_Y, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(DM_GLASS_SHADOW_COLOR), 0);
    lv_obj_set_style_shadow_opa(obj, DM_GLASS_SHADOW_OPA, 0);
}

#endif /* DESKMATE_DM_GLASS_H */
