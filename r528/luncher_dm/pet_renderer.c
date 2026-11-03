#include <nuttx/config.h>
#include <string.h>
#include "pet_renderer.h"

/* P346: 取帧失败时 last_asset 会被置 -1 强制下一拍重试，而本函数由 33ms
 * 定时器驱动——坏素材活跃期间每秒会走 30 次失败分支。素材侧已加退避
 * （pet_assets.c），这里再把日志限流成"同一 asset 每秒最多一行"。 */
static uint32_t s_miss_log_ms;
static unsigned s_miss_log_asset = (unsigned)-1;

void pet_renderer_init(pet_renderer_t *r,lv_obj_t *image,unsigned display_size)
{
    memset(r,0,sizeof(*r));r->image=image;
    lv_obj_set_size(image,PET_FRAME_SIZE,PET_FRAME_SIZE);
    lv_image_set_pivot(image,PET_FRAME_SIZE/2,PET_FRAME_SIZE/2);
    lv_image_set_scale(image,display_size*256/PET_FRAME_SIZE);
}
bool pet_renderer_step(pet_renderer_t *r,pet_action_t action,
        uint32_t instance,bool held,bool mirror,uint32_t now)
{
    if (!r->image || action<0 || action>=PET_ACTION_COUNT) return false;
    bool changed=!r->clock.valid || r->clock.action!=action || r->clock.instance!=instance;
    bool done=false;
    unsigned frame=pet_playback_frame(&r->clock,action,instance,now,held,&done);
    unsigned asset=g_pet_actions[action].frames[frame].asset;
    if (changed || !r->request_valid || asset!=r->last_asset || mirror!=r->last_mirror) {
        pet_asset_entry_t *next=pet_assets_acquire(asset,mirror);
        r->request_valid=true;
        if (next) {
            r->last_asset=asset;
            r->last_mirror=mirror;
            r->resource_error=false;
            lv_image_set_src(r->image,pet_assets_image(next));
            lv_obj_remove_flag(r->image,LV_OBJ_FLAG_HIDDEN);
            pet_assets_release(r->pinned);
            r->pinned=next;
        } else {
            /* Invalidate last_asset so the next tick can immediately retry if the frame didn't advance */
            r->last_asset=(unsigned)-1;
            /* Never hide the image if a previous frame is already pinned: seamless fallback */
            if (!r->pinned) {
                lv_obj_add_flag(r->image,LV_OBJ_FLAG_HIDDEN);
                r->resource_error=true;
            } else {
                r->resource_error=false;
            }
            /* 同上一拍会重复进这里，限流避免 30Hz 刷串口（串口写本身也占 UI 线程） */
            uint32_t now_ms = lv_tick_get();
            if (asset != s_miss_log_asset
                || (int32_t)(now_ms - s_miss_log_ms) >= 1000) {
                s_miss_log_ms = now_ms;
                s_miss_log_asset = asset;
                LV_LOG_USER("[pet] missing frame (retaining previous) action=%s frame=%u asset=%u",
                    g_pet_actions[action].name,frame,asset);
            }
        }
    }
    if (changed)
        LV_LOG_USER("[pet] action=%s instance=%lu frames=%u duration=%lu mirror=%d resource_ok=%d",
            g_pet_actions[action].name,(unsigned long)instance,g_pet_actions[action].frame_count,
            (unsigned long)g_pet_actions[action].duration_ms,mirror,!r->resource_error);
    return done;
}
void pet_renderer_clear(pet_renderer_t *r)
{
    pet_assets_release(r->pinned);memset(r,0,sizeof(*r));pet_assets_trim();
}
