/* 320px BGRA cache: at most five decoded frames, including mirrored frames.
 * Active image descriptors are pinned; cache eviction never changes a live image.
 *
 * P314 diagnostic: every decode now reads the whole PNG into a static buffer,
 * verifies size/CRC32 against the shipped file (pet_png_crc.h) and decodes from
 * memory, then re-verifies the same buffer after the decoder ran. One log line
 * per anomaly answers "did the bytes change before or during decode".
 */
#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <png.h>
#include "dm_res.h"
#include "pet_assets.h"
#include "pet_png_crc.h"
#ifndef PET_ASSET_ROOT
#define PET_ASSET_ROOT DM_RES_ROOT "/pet"
#endif
/* Largest shipped pet PNG is 87100 B; the bound keeps a runaway read out of BSS. */
#define PET_PNG_INPUT_MAX 98304u
/* P316: P315 split the read below the 2048-byte sector and the per-read
 * corruption rate went up, so the damage tracks the number of read() calls, not
 * the romfs path. One call for the whole file, then verify/retry. */
#define PET_PNG_READ_CHUNK (sizeof(png_input))
#define PET_PNG_READ_TRIES 4

struct pet_asset_entry_s {
    lv_image_dsc_t image;
    uint8_t *pixels;
    unsigned asset;
    unsigned pins;
    uint32_t age;
    bool mirror;
    bool valid;
};
static pet_asset_entry_t cache[PET_CACHE_SLOTS];
static uint32_t serial;
static int revision_checked;
/* Not on the heap on purpose: heap overlap must not be able to explain a CRC
 * mismatch, and libpng reads this buffer directly. */
static uint8_t png_input[PET_PNG_INPUT_MAX];
static unsigned png_ok_logged;

/* P346: 校验不过的 asset 会被每帧重新读取——一次失败 = 4×78KB NAND 读 +
 * 8 次 78KB CRC32 + 4 行日志，而且永远不会成功（串口日志实测：67 个 asset、
 * 4996 次尝试，其中 asset 66/7/80/62 各 550~650 次）。失败一次就退避，
 * 退避期内直接返回 NULL 让渲染沿用上一帧（与现状视觉一致，但不再烧 NAND/CPU）。
 * 退避是有限且递增的——NAND 抖包是间歇性的，必须留恢复窗口。 */
#define PET_ASSET_FAIL_BACKOFF_MS 30000u
static uint32_t asset_retry_at[PET_PNG_EXPECT_COUNT];
static uint8_t asset_fail_streak[PET_PNG_EXPECT_COUNT];

static bool asset_in_backoff(unsigned asset)
{
    if (asset >= PET_PNG_EXPECT_COUNT || asset_retry_at[asset] == 0)
        return false;
    return (int32_t)(asset_retry_at[asset] - lv_tick_get()) > 0;
}

static void asset_mark_ok(unsigned asset)
{
    if (asset >= PET_PNG_EXPECT_COUNT)
        return;
    asset_retry_at[asset] = 0;
    asset_fail_streak[asset] = 0;
}

static void asset_mark_fail(unsigned asset)
{
    if (asset >= PET_PNG_EXPECT_COUNT)
        return;
    uint8_t streak = asset_fail_streak[asset];
    if (streak < 4)
        streak++;
    asset_fail_streak[asset] = streak;
    asset_retry_at[asset] = lv_tick_get() + PET_ASSET_FAIL_BACKOFF_MS * streak;
    LV_LOG_USER("[pet] asset=%u 校验失败，%us 内不再重试（连续第 %u 次）",
        asset, (PET_ASSET_FAIL_BACKOFF_MS * streak) / 1000u, streak);
}

static uint32_t png_crc32(const uint8_t *data, size_t len)
{
    /* P346: 表驱动。原实现逐位算（78KB = 62 万次循环），一次失败要算 4~8 遍，
     * 是点返回时"卡一下"的主要来源之一。多项式/初值/取反都不变，结果一致。 */
    static uint32_t table[256];
    static int table_ready;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (unsigned bit = 0; bit < 8; bit++)
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_ready = 1;
    }
    uint32_t crc = 0xffffffffu;
    while (len--)
        crc = table[(crc ^ *data++) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

static int png_read_once(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    size_t got = 0;
    for (;;) {
        size_t room = sizeof(png_input) - got;
        if (room == 0) { close(fd); return -EFBIG; }
        size_t want = room < PET_PNG_READ_CHUNK ? room : PET_PNG_READ_CHUNK;
        ssize_t n = read(fd, png_input + got, want);
        if (n < 0) { int err = -errno; close(fd); return err; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    *len = got;
    return 0;
}

static bool check_revision(void)
{
    if (revision_checked) return revision_checked>0;
    char revision[32]={0};
    FILE *f=fopen(PET_ASSET_ROOT "/revision","r");
    if (f) { if (!fgets(revision,sizeof(revision),f)) revision[0]='\0'; fclose(f); }
    revision[strcspn(revision,"\r\n")]='\0';
    revision_checked=strcmp(revision,g_pet_resource_revision)==0 ? 1 : -1;
    if (revision_checked<0)
        LV_LOG_USER("[pet] resource revision missing/mismatched expected=%s root=%s",g_pet_resource_revision,DM_RES_ROOT);
    else
        LV_LOG_USER("[pet] resources revision=%s canvas=320 cache_limit=%u",revision,(unsigned)PET_CACHE_PIXEL_BUDGET);
    return revision_checked>0;
}

pet_asset_entry_t *pet_assets_acquire(unsigned asset, bool mirror)
{
    if (asset>=g_pet_asset_count || !check_revision()) return NULL;
    /* P346: 已知验不过的帧，退避期内不再读 NAND（渲染沿用上一帧） */
    if (asset_in_backoff(asset)) return NULL;
    pet_asset_entry_t *victim=NULL;
    for (unsigned i=0;i<PET_CACHE_SLOTS;i++) {
        pet_asset_entry_t *e=&cache[i];
        if (e->valid && e->asset==asset && e->mirror==mirror) {
            e->pins++;e->age=++serial;return e;
        }
        if (!e->pins && (!victim || !e->pixels || (victim->pixels && e->age<victim->age))) victim=e;
    }
    if (!victim) { LV_LOG_USER("[pet] PNG cache exhausted: all %u frames pinned (want asset=%u mirror=%d)",(unsigned)PET_CACHE_SLOTS,asset,(int)mirror); return NULL; }
    if (victim->valid) lv_image_cache_drop(&victim->image);
    victim->valid=false;
    if (!victim->pixels) victim->pixels=malloc(PET_FRAME_BYTES);
    if (!victim->pixels) { LV_LOG_USER("[pet] PNG allocation failed"); return NULL; }
    char path[160];
    snprintf(path,sizeof(path),PET_ASSET_ROOT "/%s",g_pet_asset_files[asset]);
    const struct pet_png_expect_s *expect =
        asset<PET_PNG_EXPECT_COUNT ? &g_pet_png_expect[asset] : NULL;
    uint32_t exp_size=expect?expect->size:0, exp_crc=expect?expect->crc:0;
    size_t len=0;
    uint32_t crc=0;
    int read_err=0;
    bool verified=false;
    /* P349：已经失败过的 asset（streak>0）只再试 1 次。实测单次读损坏概率
     * p≈0.93，4 次重试把成功率从 7% 抬到 25%，却让每次取帧固定花掉 4 次
     * NAND 读 + 8 次 78KB CRC32（约 30~50ms，全在 LVGL 帧回调里）。
     * 对这批长期坏的素材，多试 3 次换来的收益远小于每 5 秒一次的画面顿挫。 */
    unsigned tries = asset<PET_PNG_EXPECT_COUNT && asset_fail_streak[asset]
                     ? 1u : PET_PNG_READ_TRIES;
    for (unsigned attempt=1;attempt<=tries;attempt++) {
        read_err=png_read_once(path,&len);
        if (read_err<0) break;
        crc=png_crc32(png_input,len);
        if (expect && len==exp_size && crc==exp_crc) { verified=true; break; }
        /* Re-hash the same buffer without re-reading: if it changes, something
         * is writing our buffer instead of the read returning bad bytes. */
        uint32_t again=png_crc32(png_input,len);
        (void)again; /* LV_LOG_USER compiles out when logging is off */
        /* Never swallowed: every bad read is logged with its own CRC. */
        LV_LOG_USER("[pet] PNG input mismatch asset=%u attempt=%u size=%u/%lu crc=%08lx again=%08lx exp=%08lx path=%s",
            asset,attempt,(unsigned)len,(unsigned long)exp_size,
            (unsigned long)crc,(unsigned long)again,(unsigned long)exp_crc,path);
    }
    if (read_err<0) {
        LV_LOG_USER("[pet] PNG read failed asset=%u path=%s err=%d",asset,path,read_err);
        asset_mark_fail(asset);
        return NULL;
    }
    if (!verified) {
        LV_LOG_USER("[pet] PNG input unverified asset=%u attempts=%u",asset,(unsigned)tries);
        asset_mark_fail(asset);
        return NULL;
    }
    if (png_ok_logged<8) {
        png_ok_logged++;
        LV_LOG_USER("[pet] PNG input verified asset=%u size=%u crc=%08lx",asset,(unsigned)len,(unsigned long)crc);
    }
    png_image png;
    memset(&png,0,sizeof(png));png.version=PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png,png_input,len)) {
        LV_LOG_USER("[pet] PNG open failed asset=%u: %s",asset,png.message);png_image_free(&png);asset_mark_fail(asset);return NULL;
    }
    if (png.width!=PET_FRAME_SIZE || png.height!=PET_FRAME_SIZE) {
        LV_LOG_USER("[pet] PNG dimensions invalid asset=%u",asset);png_image_free(&png);asset_mark_fail(asset);return NULL;
    }
    png.format=PNG_FORMAT_BGRA; /* LV_COLOR_FORMAT_ARGB8888 on both little-endian targets. */
    if (!png_image_finish_read(&png,NULL,victim->pixels,0,NULL)) {
        uint32_t after=png_crc32(png_input,len);
        (void)after; /* LV_LOG_USER compiles out when logging is off */
        LV_LOG_USER("[pet] PNG decode failed asset=%u size=%u crc=%08lx exp=%08lx after=%08lx: %s",
            asset,(unsigned)len,(unsigned long)crc,(unsigned long)exp_crc,
            (unsigned long)after,png.message);
        png_image_free(&png);asset_mark_fail(asset);return NULL;
    }
    png_image_free(&png);
    {
        uint32_t after=png_crc32(png_input,len);
        (void)after; /* LV_LOG_USER compiles out when logging is off */
        if (after!=crc)
            LV_LOG_USER("[pet] PNG input changed during decode asset=%u crc=%08lx after=%08lx",
                asset,(unsigned long)crc,(unsigned long)after);
    }
    if (mirror) {
        for (unsigned y=0;y<PET_FRAME_SIZE;y++)
            for (unsigned x=0;x<PET_FRAME_SIZE/2;x++) {
                uint8_t *a=victim->pixels+(y*PET_FRAME_SIZE+x)*4;
                uint8_t *b=victim->pixels+(y*PET_FRAME_SIZE+PET_FRAME_SIZE-1-x)*4;
                for (unsigned c=0;c<4;c++) { uint8_t t=a[c];a[c]=b[c];b[c]=t; }
            }
    }
    victim->image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,
        .cf=LV_COLOR_FORMAT_ARGB8888,.w=PET_FRAME_SIZE,.h=PET_FRAME_SIZE,.stride=PET_FRAME_SIZE*4},
        .data_size=PET_FRAME_BYTES,.data=victim->pixels};
    victim->asset=asset;victim->mirror=mirror;victim->valid=true;victim->pins=1;victim->age=++serial;
    asset_mark_ok(asset);
    return victim;
}

void pet_assets_release(pet_asset_entry_t *entry)
{
    if (entry && entry->pins) entry->pins--;
}
const lv_image_dsc_t *pet_assets_image(const pet_asset_entry_t *entry)
{
    return entry ? &entry->image : NULL;
}
void pet_assets_trim(void)
{
    for (unsigned i=0;i<PET_CACHE_SLOTS;i++) if (!cache[i].pins) {
        if (cache[i].valid) lv_image_cache_drop(&cache[i].image);
        free(cache[i].pixels);memset(&cache[i],0,sizeof(cache[i]));
    }
    revision_checked=0; /* Recheck the manifest after a resource reload/reopen. */
    /* P346: 资源重载/重开后清退避表——文件可能已经被换掉了，必须重新给机会 */
    memset(asset_retry_at,0,sizeof(asset_retry_at));
    memset(asset_fail_streak,0,sizeof(asset_fail_streak));
}
unsigned pet_assets_allocated_bytes(void)
{
    unsigned total=0;
    for (unsigned i=0;i<PET_CACHE_SLOTS;i++) if (cache[i].pixels) total+=PET_FRAME_BYTES;
    return total;
}
