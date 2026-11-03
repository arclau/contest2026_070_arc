/* V1 壁纸：随固件编进 .rodata，开机解码一次，之后只按 lv_image_dsc_t 绘制。
 *
 * 为什么不再读 /resource/wallpaper/blue-hour.png：
 * 板端 romfs 读路径会偶发返回坏字节（P312/P355，同一文件每次读出的 CRC 都不同），
 * 1.6MB 的壁纸几乎每次读都坏。P348 实测 70 秒里 765 条 "png decode failed"、
 * 相邻两条 90~107ms——每一帧都在重读重解，把单轮耗时顶到 1.3s。
 * P359 改成"开机读一次 + 最多 4 次重读"后板上仍然没有壁纸：4 次读全部失败
 * （读坏概率太高，重试救不回来）。
 *
 * 现在 PNG 由 tools/gen_wallpaper_blob.py 生成成 C 数组，跟内核一起被 U-Boot
 * 一次性载入内存，运行时不再碰 NAND，解码结果确定。解码失败只可能是代码/
 * 资源本身的问题，所以用 LV_LOG_ERROR 记（本工程 LV_LOG_LEVEL=ERROR，
 * WARN 是编译掉的，P359 的失败日志就是这样被吞掉的）。
 *
 * 内存：PNG 1.6MB（.rodata）+ 解码 9.2MB（堆）。RAM 128MB，堆约 48MB 空闲。
 */
#include <nuttx/config.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dm_glass.h"

/* 由 tools/gen_wallpaper_blob.py 生成 */
extern const unsigned char dm_wallpaper_png[];
extern const size_t dm_wallpaper_png_len;

static lv_image_dsc_t s_wp;
static uint8_t *s_wp_pixels;
static int s_wp_state;          /* 0=未尝试, 1=就绪, -1=失败 */

int dm_glass_wallpaper_prepare(void)
{
    if (s_wp_state)
        return s_wp_state > 0 ? 0 : -1;

    png_image png;
    memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, dm_wallpaper_png,
                                          dm_wallpaper_png_len))
        {
            LV_LOG_ERROR("[glass] wallpaper header: %s (blob %u B)",
                         png.message, (unsigned)dm_wallpaper_png_len);
            png_image_free(&png);
            s_wp_state = -1;
            return -1;
        }

    size_t bytes = (size_t)png.width * png.height * 4u;
    uint8_t *pixels = malloc(bytes);
    if (!pixels)
        {
            LV_LOG_ERROR("[glass] wallpaper %ux%u needs %u B, alloc failed",
                         (unsigned)png.width, (unsigned)png.height,
                         (unsigned)bytes);
            png_image_free(&png);
            s_wp_state = -1;
            return -1;
        }

    /* LV_COLOR_FORMAT_ARGB8888 on little-endian == PNG BGRA */
    /* 计时：这几十毫秒是壁纸唯一的一次性成本，之后每帧只是内存拷贝。
     * 日志留在 USER 级（ERROR 配置下仍会打印），上板可直接核。 */
    uint32_t t0 = lv_tick_get();

    png.format = PNG_FORMAT_BGRA;
    if (!png_image_finish_read(&png, NULL, pixels, 0, NULL))
        {
            LV_LOG_ERROR("[glass] wallpaper decode: %s", png.message);
            png_image_free(&png);
            free(pixels);
            s_wp_state = -1;
            return -1;
        }

    s_wp.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_wp.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    s_wp.header.flags  = 0;
    s_wp.header.w      = (uint16_t)png.width;
    s_wp.header.h      = (uint16_t)png.height;
    s_wp.header.stride = (uint16_t)(png.width * 4u);
    s_wp.data_size     = (uint32_t)bytes;
    s_wp.data          = pixels;

    s_wp_pixels = pixels;
    s_wp_state = 1;
    LV_LOG_USER("[glass] wallpaper ready %ux%u %u B decode=%ums",
                (unsigned)png.width, (unsigned)png.height, (unsigned)bytes,
                (unsigned)(lv_tick_get() - t0));
    png_image_free(&png);
    return 0;
}

const void *dm_glass_wallpaper_src(void)
{
    return s_wp_state > 0 ? (const void *)&s_wp : NULL;
}
