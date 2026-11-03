#ifndef DM_PET_RENDERER_H
#define DM_PET_RENDERER_H
#include "pet_assets.h"
#include "pet_playback.h"
typedef struct {
    lv_obj_t *image;
    pet_asset_entry_t *pinned;
    pet_playback_t clock;
    unsigned last_asset;
    bool last_mirror;
    bool request_valid;
    bool resource_error;
} pet_renderer_t;
void pet_renderer_init(pet_renderer_t *renderer,lv_obj_t *image,unsigned display_size);
bool pet_renderer_step(pet_renderer_t *renderer,pet_action_t action,
        uint32_t instance,bool held,bool mirror,uint32_t now);
/* Call after image deletion, or while the image is detached/hidden. */
void pet_renderer_clear(pet_renderer_t *renderer);
#endif
