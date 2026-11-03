#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include "pet_assets.h"
#include "expected_crc.h"
static unsigned drops;
void lv_image_cache_drop(const void *src) { assert(src);drops++; }
static void check(pet_asset_entry_t *e,unsigned asset,bool mirror)
{
    assert(e);const lv_image_dsc_t *im=pet_assets_image(e);
    assert(im->header.w==320 && im->header.h==320 && im->header.stride==1280);
    assert(im->data_size==PET_FRAME_BYTES && im->header.cf==LV_COLOR_FORMAT_ARGB8888);
    assert(crc32(0,im->data,im->data_size)==expected_crc[asset][mirror ? 1 : 0]);
}
int main(void)
{
    for (unsigned a=0;a<g_pet_asset_count;a++) for (int mirror=0;mirror<2;mirror++) {
        pet_asset_entry_t *e=pet_assets_acquire(a,mirror);check(e,a,mirror);pet_assets_release(e);
        assert(pet_assets_allocated_bytes()<=PET_CACHE_PIXEL_BUDGET);
    }
    pet_assets_trim();assert(pet_assets_allocated_bytes()==0);
    pet_asset_entry_t *pins[PET_CACHE_SLOTS];
    for (unsigned a=0;a<PET_CACHE_SLOTS;a++) { pins[a]=pet_assets_acquire(a,false);check(pins[a],a,false); }
    assert(pet_assets_allocated_bytes()==PET_CACHE_PIXEL_BUDGET);
    assert(!pet_assets_acquire(PET_CACHE_SLOTS,false)); /* Pinned image data cannot be evicted. */
    pet_assets_release(pins[0]);pins[0]=pet_assets_acquire(PET_CACHE_SLOTS,true);check(pins[0],PET_CACHE_SLOTS,true);
    for (unsigned a=1;a<PET_CACHE_SLOTS;a++) check(pins[a],a,false);
    for (unsigned a=0;a<PET_CACHE_SLOTS;a++) pet_assets_release(pins[a]);
    pet_assets_trim();assert(pet_assets_allocated_bytes()==0 && drops>0);
    char path[256];snprintf(path,sizeof(path),"%s/revision",PET_ASSET_ROOT);
    FILE *f=fopen(path,"w");assert(f);fputs("bad-revision\n",f);fclose(f);
    assert(!pet_assets_acquire(0,false));
    f=fopen(path,"w");assert(f);fprintf(f,"%s\n",g_pet_resource_revision);fclose(f);
    pet_assets_trim();
    pet_asset_entry_t *e=pet_assets_acquire(0,false);check(e,0,false);pet_assets_release(e);
    pet_assets_trim();
    snprintf(path,sizeof(path),"%s/%s",PET_ASSET_ROOT,g_pet_asset_files[0]);
    assert(remove(path)==0);f=fopen(path,"w");assert(f);fputs("invalid PNG",f);fclose(f);
    assert(!pet_assets_acquire(0,false));pet_assets_trim();assert(pet_assets_allocated_bytes()==0);
    printf("PASS real PNG cache: %u images, BGRA/mirror pixel CRCs, %uMiB bound, pinning, revision and decode errors\n",
        g_pet_asset_count, (unsigned)(PET_CACHE_PIXEL_BUDGET / (1024 * 1024)));
    return 0;
}
