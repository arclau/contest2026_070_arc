/* Shared bounded PNG cache. All functions run on the LVGL thread. */
#ifndef DM_PET_ASSETS_H
#define DM_PET_ASSETS_H
#include <lvgl/lvgl.h>
#include "pet_actions.h"
#define PET_CACHE_SLOTS 25
#define PET_CACHE_PIXEL_BUDGET (PET_CACHE_SLOTS * PET_FRAME_BYTES)
typedef struct pet_asset_entry_s pet_asset_entry_t;
pet_asset_entry_t *pet_assets_acquire(unsigned asset, bool mirror);
void pet_assets_release(pet_asset_entry_t *entry);
const lv_image_dsc_t *pet_assets_image(const pet_asset_entry_t *entry);
void pet_assets_trim(void);
unsigned pet_assets_allocated_bytes(void);
#endif
