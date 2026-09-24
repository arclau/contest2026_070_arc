/* Copyright (c) 2019-2025 Allwinner Technology Co., Ltd. ALL rights reserved.

 * Allwinner is a trademark of Allwinner Technology Co.,Ltd., registered in
 * the the People's Republic of China and other countries.
 * All Allwinner Technology Co.,Ltd. trademarks are used with permission.

 * DISCLAIMER
 * THIRD PARTY LICENCES MAY BE REQUIRED TO IMPLEMENT THE SOLUTION/PRODUCT.
 * IF YOU NEED TO INTEGRATE THIRD PARTY’S TECHNOLOGY (SONY, DTS, DOLBY, AVS OR MPEGLA, ETC.)
 * IN ALLWINNERS’SDK OR PRODUCTS, YOU SHALL BE SOLELY RESPONSIBLE TO OBTAIN
 * ALL APPROPRIATELY REQUIRED THIRD PARTY LICENCES.
 * ALLWINNER SHALL HAVE NO WARRANTY, INDEMNITY OR OTHER OBLIGATIONS WITH RESPECT TO MATTERS
 * COVERED UNDER ANY REQUIRED THIRD PARTY LICENSE.
 * YOU ARE SOLELY RESPONSIBLE FOR YOUR USAGE OF THIRD PARTY’S TECHNOLOGY.


 * THIS SOFTWARE IS PROVIDED BY ALLWINNER"AS IS" AND TO THE MAXIMUM EXTENT
 * PERMITTED BY LAW, ALLWINNER EXPRESSLY DISCLAIMS ALL WARRANTIES OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING WITHOUT LIMITATION REGARDING
 * THE TITLE, NON-INFRINGEMENT, ACCURACY, CONDITION, COMPLETENESS, PERFORMANCE
 * OR MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * IN NO EVENT SHALL ALLWINNER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS, OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/video/fb.h>
#include <hal_cache.h>
#include <hal_mutex.h>
#include "arm_internal.h"
#include "dev_disp.h"

#include <arch/board/board.h>

#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
#include "fb_g2d_rot.h"
#endif
#include "r528_memorymap.h"
extern struct disp_drv_info g_disp_drv;
extern int fb_wait_for_vsync(u32 sel);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/


/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Get information about the video controller configuration and the
 * configuration of each color plane.
 */

static int sunxifb_getvideoinfo(struct fb_vtable_s *vtable,
             struct fb_videoinfo_s *vinfo);
static int sunxifb_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
             struct fb_planeinfo_s *pinfo);
int sunxifb_setpower(FAR struct fb_vtable_s *vtable, int power);
int sunxifb_getpower(FAR struct fb_vtable_s *vtable);
#ifdef CONFIG_FB_SYNC
int sunxifb_waitforvsync(struct fb_vtable_s *vtable);
#endif

#ifdef CONFIG_FB_UPDATE
int sunxifb_updatearea(struct fb_vtable_s *vtable, FAR const struct fb_area_s *area);
#endif

#ifdef CONFIG_FB_OVERLAY
static int sunxifb_getoverlayinfo(struct fb_vtable_s *vtable,
			int overlayno, struct fb_overlayinfo_s *oinfo);
static int sunxifb_setblank(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo);
static int sunxifb_setarea(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo);
static int sunxifb_setdisplayarea(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo);
static int sunxifb_settransp(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo);
static int sunxifb_pandisplayoverlay(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo);
#endif

#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
struct fb_g2d_rot_t *fb_rot;
#endif


/****************************************************************************
 * Private Data
 ****************************************************************************/

/* This structure describes the video controller */

static struct fb_videoinfo_s g_videoinfo;

/* This structure describes the single color plane */

struct fb_planeinfo_s g_planeinfo;

#ifdef CONFIG_FB_OVERLAY
static struct fb_overlayinfo_s g_overlayinfo;
static hal_mutex_t layer_lock;
#endif


/* Current cursor position */

#ifdef CONFIG_FB_HWCURSOR
static struct fb_cursorpos_s g_cpos;

/* Current cursor size */

#ifdef CONFIG_FB_HWCURSORSIZE
static struct fb_cursorsize_s g_csize;
#endif
#endif

/* The framebuffer object -- There is no private state information in this
 * framebuffer driver.
 */

struct fb_vtable_s g_fbobject =
{
	.getvideoinfo  = sunxifb_getvideoinfo,
	.getplaneinfo  = sunxifb_getplaneinfo,
	.setpower = sunxifb_setpower,
	.getpower = sunxifb_getpower,
#ifdef CONFIG_FB_SYNC
	.waitforvsync = sunxifb_waitforvsync,
#endif
#ifdef CONFIG_FB_UPDATE
	.updatearea = sunxifb_updatearea,
#endif
#ifdef CONFIG_FB_OVERLAY
	.getoverlayinfo = sunxifb_getoverlayinfo,
	.setblank = sunxifb_setblank,
	.setarea = sunxifb_setarea,
	.setdestarea = sunxifb_setdisplayarea,
	.settransp = sunxifb_settransp,
	.panoverlay = sunxifb_pandisplayoverlay
#endif

};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sunxifb_getvideoinfo
 ****************************************************************************/

static int sunxifb_getvideoinfo(struct fb_vtable_s *vtable,
                              struct fb_videoinfo_s *vinfo)
{
	lcdinfo("vtable=%p vinfo=%p\n", vtable, vinfo);
	if (vtable && vinfo)
	{
	memcpy(vinfo, &g_videoinfo, sizeof(struct fb_videoinfo_s));
	return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;
}

/****************************************************************************
 * Name: sunxifb_getplaneinfo
 ****************************************************************************/

static int sunxifb_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                              struct fb_planeinfo_s *pinfo)
{
	lcdinfo("vtable=%p planeno=%d pinfo=%p\n", vtable, planeno, pinfo);
	if (vtable && planeno == 0 && pinfo)
	{
	memcpy(pinfo, &g_planeinfo, sizeof(struct fb_planeinfo_s));
	return OK;
	}

	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;
}

#ifdef CONFIG_FB_SYNC
int sunxifb_waitforvsync(struct fb_vtable_s *vtable)
{
	return fb_wait_for_vsync(0);
}
#endif

#ifdef CONFIG_FB_UPDATE
int sunxifb_updatearea(struct fb_vtable_s *vtable, FAR const struct fb_area_s *area)
{
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
	/* 2026-09-15 流畅度专项 A1：
	 * 原实现每帧无条件 hal_dcache_clean_invalidate(整帧 8.8MB) + G2D 全屏旋转，
	 * 完全无视 LVGL 传进来的脏区。A1 = 缓存清理改按脏区；并加每帧耗时打点
	 * （clean_us / g2d_us），用真实数据决定是否继续做 A2（脏区 G2D 旋转）。 */
	static uint32_t s_cnt, s_clean_us, s_g2d_us;
	struct timespec t0, t1, t2;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (area && area->w > 0 && area->h > 0 &&
	    area->x >= 0 && area->y >= 0 &&
	    (unsigned int)(area->x + area->w) <= g_videoinfo.xres &&
	    (unsigned int)(area->y + area->h) <= g_videoinfo.yres) {
		unsigned long start = (unsigned long)g_planeinfo.fbmem +
		                      (unsigned long)area->y * g_planeinfo.stride +
		                      (unsigned long)area->x * 4;
		hal_dcache_clean_invalidate(start,
		                            (unsigned long)area->h * g_planeinfo.stride);
	} else {
		hal_dcache_clean_invalidate((unsigned long)g_planeinfo.fbmem, g_planeinfo.fblen);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	{
	unsigned int *addr = (unsigned int *)0x05103010;
	struct disp_layer_config config;
	config.channel = 1;
	config.layer_id = 0;
	struct disp_manager *mgr =  g_disp_drv.mgr[0];
	if (mgr && mgr->get_layer_config)
		mgr->get_layer_config(mgr, &config, 1);
	int y_offset = (*addr -  (unsigned int)fb_rot->dst_vir_addr) / fb_rot->pinfo->stride;
	y_offset = (y_offset + g_videoinfo.yres) % (g_planeinfo.yres_virtual << 1);
	if (fb_rot) {
		fb_rot->pinfo->yoffset= y_offset;
		fb_rot->apply(fb_rot);
	}
	config.info.fb.addr[0] = (unsigned long long)(unsigned int)(fb_rot->dst_phy_addr + fb_rot->pinfo->stride * y_offset);
	if (mgr && mgr->set_layer_config)
	mgr->set_layer_config(mgr, &config, 1);
	}
	clock_gettime(CLOCK_MONOTONIC, &t2);

	s_clean_us += (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000;
	s_g2d_us   += (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_nsec - t1.tv_nsec) / 1000;
	if (++s_cnt >= 300) {
		printf("[fb] upd avg: clean=%uus g2d+cfg=%uus (%u frames, last area %dx%d)\n",
		       (unsigned)(s_clean_us / s_cnt), (unsigned)(s_g2d_us / s_cnt), (unsigned)s_cnt,
		       (int)(area ? area->w : 0), (int)(area ? area->h : 0));
		s_cnt = s_clean_us = s_g2d_us = 0;
	}
#else
	hal_dcache_clean_invalidate((unsigned long)g_planeinfo.fbmem, g_planeinfo.fblen);
#endif
	return 0;
}
#endif

#ifdef CONFIG_FB_OVERLAY
static int sunxifb_getoverlayinfo(struct fb_vtable_s *vtable, int overlayno, struct fb_overlayinfo_s *oinfo)
{
	lcdinfo("vtable=%p overlayno=%d oinfo=%p\n", vtable, overlayno, oinfo);
	if (vtable && (overlayno == 0) && oinfo) {
		memcpy(oinfo, &g_overlayinfo, sizeof(struct fb_overlayinfo_s));
		return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;
}

static int sunxifb_setblank(struct fb_vtable_s *vtable,
                          const struct fb_overlayinfo_s *oinfo)
{
	if (oinfo->overlay == 0)
	{
		hal_mutex_lock(layer_lock);
		g_overlayinfo.blank=oinfo->blank;
		hal_mutex_unlock(layer_lock);
		return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;
}

static int sunxifb_setarea(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo)
{
	if (oinfo->overlay == 0)
	{
		hal_mutex_lock(layer_lock);
		g_overlayinfo.sarea=oinfo->sarea;
		hal_mutex_unlock(layer_lock);
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
		fb_rot->realloc(fb_rot, oinfo);
#endif
		return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;

}

static int sunxifb_setdisplayarea(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo)
{
	if (oinfo->overlay == 0)
	{
		hal_mutex_lock(layer_lock);
		g_overlayinfo.darea=oinfo->darea;
		hal_mutex_unlock(layer_lock);
		return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;
}

static int sunxifb_settransp(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo)
{
	if (oinfo->overlay == 0)
	{
		hal_mutex_lock(layer_lock);
		g_overlayinfo.transp=oinfo->transp;
		hal_mutex_unlock(layer_lock);
		return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;
}

static int sunxifb_pandisplayoverlay(struct fb_vtable_s *vtable,
			const struct fb_overlayinfo_s *oinfo)
{
	if (oinfo->overlay == 0)
	{
		hal_mutex_lock(layer_lock);
		g_overlayinfo.yoffset=oinfo->yoffset;
		hal_mutex_unlock(layer_lock);
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
		if (fb_rot)
			fb_rot->apply_overlay(fb_rot, oinfo);
#endif
		return OK;
	}
	lcderr("ERROR: Returning EINVAL\n");
	return -EINVAL;

}


#endif
/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_fbinitialize
 *
 * Description:
 *   Initialize the framebuffer video hardware associated with the display.
 *
 * Input Parameters:
 *   display - In the case of hardware with multiple displays, this
 *     specifies the display.  Normally this is zero.
 *
 * Returned Value:
 *   Zero is returned on success; a negated errno value is returned on any
 *   failure.
 *
 ****************************************************************************/


int up_fbinitialize(int display)
{
	int ret = -1;
	// 获取旋转角度
	s32 rotation_degree = 0;
	disp_sys_script_get_item("disp", "degree0", &rotation_degree, 1);
	disp_probe();
	if ((g_disp_drv.disp_init.fb_width[display] == 0)
		|| (g_disp_drv.disp_init.fb_height[display] == 0)) {
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
		g_videoinfo.xres =
			bsp_disp_get_screen_height_from_output_type
			(display,
			g_disp_drv.disp_init.output_type[display],
			g_disp_drv.disp_init.output_mode[display]);
		g_videoinfo.yres =
			bsp_disp_get_screen_width_from_output_type
			(display,
			g_disp_drv.disp_init.output_type[display],
			g_disp_drv.disp_init.output_mode[display]);

#else
		g_videoinfo.xres =
			bsp_disp_get_screen_width_from_output_type
			(display,
			g_disp_drv.disp_init.output_type[display],
			g_disp_drv.disp_init.output_mode[display]);
		g_videoinfo.yres =
			bsp_disp_get_screen_height_from_output_type
			(display,
			g_disp_drv.disp_init.output_type[display],
			g_disp_drv.disp_init.output_mode[display]);
#endif
	} else {
// 只有90/270度才交换宽高
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
	if (rotation_degree == 1 || rotation_degree == 3) { // 90或270度
		g_videoinfo.xres = g_disp_drv.disp_init.fb_height[display];
		g_videoinfo.yres = g_disp_drv.disp_init.fb_width[display];
	} else { // 0或180度不交换
		g_videoinfo.xres = g_disp_drv.disp_init.fb_width[display];
		g_videoinfo.yres = g_disp_drv.disp_init.fb_height[display];
	}
#else
		g_videoinfo.xres = g_disp_drv.disp_init.fb_width[display];
		g_videoinfo.yres = g_disp_drv.disp_init.fb_height[display];
#endif
	}
	g_videoinfo.fmt = FB_FMT_RGB32;
	g_videoinfo.nplanes = 1;
#ifdef CONFIG_FB_OVERLAY
	g_videoinfo.noverlays = 1;
#endif
	g_planeinfo.bpp = 32;
	g_planeinfo.display = 0;
	g_planeinfo.stride = g_videoinfo.xres * g_planeinfo.bpp / 8;
	g_planeinfo.fblen = g_videoinfo.xres * g_videoinfo.yres * g_planeinfo.bpp / 8 * FB_NUM;

	/* Ensure 64-byte alignment for LVGL */
	uintptr_t map_vaddr = (uintptr_t)R528_DISP_DDR_MAPVADDR;
	if (map_vaddr % 64 != 0)
	{
		lcdwarn("Warning: R528_DISP_DDR_MAPVADDR not 64-byte aligned: 0x%lx\\n", map_vaddr);
		map_vaddr = (map_vaddr + 63) & ~63;
	}
	g_planeinfo.fbmem = (void *)map_vaddr;

	DEBUGASSERT(g_planeinfo.fbmem != NULL);
	g_planeinfo.xres_virtual = g_videoinfo.xres;
	g_planeinfo.yres_virtual = g_videoinfo.yres * FB_NUM;
	g_planeinfo.display = display;
	memset(g_planeinfo.fbmem, 0, g_planeinfo.fblen);

	struct disp_layer_config config;
	memset(&config, 0, sizeof(struct disp_layer_config));
	config.channel = 1;
	config.layer_id = 0;
	config.enable = 1;
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
if (rotation_degree == 1 || rotation_degree == 3) { // 90或270度才交换
    config.info.screen_win.width = g_videoinfo.yres;   // 600
    config.info.screen_win.height = g_videoinfo.xres;  // 1024
} else { // 0或180度不交换
    config.info.screen_win.width = g_videoinfo.xres;   // 1024
    config.info.screen_win.height = g_videoinfo.yres;  // 600
}
#else
    config.info.screen_win.width = g_videoinfo.xres;   // 1024
    config.info.screen_win.height = g_videoinfo.yres;  // 600
#endif
	config.info.mode = LAYER_MODE_BUFFER;
	config.info.zorder = 1;
	config.info.alpha_mode = 0;
	config.info.alpha_value = 0xff;
	config.info.fb.crop.x = (0LL) << 32;
	config.info.fb.crop.y = ((long long)g_planeinfo.yoffset) << 32;
	config.info.fb.crop.width =
	    ((long long)g_videoinfo.xres) << 32;
	config.info.fb.crop.height =
	    ((long long)g_videoinfo.yres) << 32;
	config.info.screen_win.x = 0;
	config.info.screen_win.y = 0;

	config.info.fb.addr[0] =
	    (unsigned long long)(unsigned int)g_planeinfo.fbmem;
	config.info.fb.addr[1] = 0;
	config.info.fb.addr[2] = 0;
	config.info.fb.flags = DISP_BF_NORMAL;
	config.info.fb.scan = DISP_SCAN_PROGRESSIVE;
	config.info.fb.size[0].width = g_planeinfo.xres_virtual;
	config.info.fb.size[0].height = g_planeinfo.yres_virtual;
	config.info.fb.size[1].width = g_planeinfo.xres_virtual;
	config.info.fb.size[1].height = g_planeinfo.yres_virtual;
	config.info.fb.size[2].width = g_planeinfo.xres_virtual;
	config.info.fb.size[2].height = g_planeinfo.yres_virtual;
	config.info.fb.color_space = DISP_BT601;
	hal_dcache_clean_invalidate((unsigned long)g_planeinfo.fbmem, g_planeinfo.fblen);
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
	fb_rot = fb_g2d_rot_create(&g_videoinfo, &g_planeinfo, 0, &config);
#ifdef CONFIG_DISP_BOOTLOADER_SYNC
	unsigned int *addr = (unsigned int *)0x05103010;
	config.info.fb.addr[0] = (unsigned long long)(*addr);
#endif
	if (fb_rot)
		fb_rot->apply(fb_rot);
#endif
	struct disp_manager *mgr =  g_disp_drv.mgr[display];
	if (mgr && mgr->set_layer_config)
		ret = mgr->set_layer_config(mgr, &config, 1);
#ifdef CONFIG_FB_OVERLAY
	layer_lock = hal_mutex_create();
	syslog(LOG_DEBUG, "up_fbinitialize:%d", VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2 * FB_NUM_OVERLAY);
	syslog(LOG_DEBUG, "VIDEO_WIDTH:%d", VIDEO_WIDTH);
	syslog(LOG_DEBUG, "VIDEO_HEIGHT:%d", VIDEO_HEIGHT);
	syslog(LOG_DEBUG, "FB_NUM_OVERLAY:%d", FB_NUM_OVERLAY);
	g_overlayinfo.fbmem = hal_malloc_align(VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2 * FB_NUM_OVERLAY, 4 * 1024);
	memset(g_overlayinfo.fbmem, 0, VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2 * FB_NUM_OVERLAY);
	g_overlayinfo.fblen = VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2 * FB_NUM_OVERLAY;
	g_overlayinfo.stride = VIDEO_WIDTH;
	g_overlayinfo.overlay = 0;
	g_overlayinfo.bpp = 8;
	g_overlayinfo.blank = 0;
	g_overlayinfo.chromakey = 0;
	g_overlayinfo.color = FB_FMT_NV21;
	g_overlayinfo.transp.transp_mode = FB_CONST_ALPHA;
	g_overlayinfo.transp.transp = 0xff;
	g_overlayinfo.sarea.x = 0;
	g_overlayinfo.sarea.y = 0;
	g_overlayinfo.sarea.w = VIDEO_WIDTH;
	g_overlayinfo.sarea.h = VIDEO_HEIGHT;
	g_overlayinfo.xoffset = 0;
	g_overlayinfo.yoffset = 0;
	g_overlayinfo.xres = VIDEO_WIDTH;
	g_overlayinfo.yres = VIDEO_HEIGHT;
	g_overlayinfo.xres_virtual=VIDEO_WIDTH;
	g_overlayinfo.yres_virtual=VIDEO_HEIGHT * FB_NUM_OVERLAY;
	g_overlayinfo.darea.x = 0;
	g_overlayinfo.darea.y = 0;
	g_overlayinfo.darea.w = g_videoinfo.xres;
	g_overlayinfo.darea.h = g_videoinfo.yres;
	g_overlayinfo.accl = (FB_ACCL_TRANSP | FB_ACCL_AREA);
#endif
	return ret;
}

int sunxifb_setpower(FAR struct fb_vtable_s *vtable, int power)
{
	struct disp_manager *mgr =  g_disp_drv.mgr[0];
	int ret = 0;
	if (mgr->device && mgr->device->set_bright)
				ret = mgr->device->set_bright(mgr->device, power);
	return ret;
}

int sunxifb_getpower(FAR struct fb_vtable_s *vtable)
{
	struct disp_manager *mgr =  g_disp_drv.mgr[0];
	int ret = 0;
	if (mgr->device && mgr->device->get_bright)
				ret = mgr->device->get_bright(mgr->device);
	return ret;
}

/****************************************************************************
 * Name: up_fbgetvplane
 *
 * Description:
 *   Return a a reference to the framebuffer object for the specified video
 *   plane of the specified plane.  Many OSDs support multiple planes of
 *   video.
 *
 * Input Parameters:
 *   display - In the case of hardware with multiple displays, this
 *     specifies the display.  Normally this is zero.
 *   vplane - Identifies the plane being queried.
 *
 * Returned Value:
 *   A non-NULL pointer to the frame buffer access structure is returned on
 *   success; NULL is returned on any failure.
 *
 ****************************************************************************/

struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
	lcdinfo("vplane: %d\n", vplane);
	if (vplane == 0)
	{
		return &g_fbobject;
	} else {
		return NULL;
	}
}

/****************************************************************************
 * Name: up_fbuninitialize
 *
 * Description:
 *   Uninitialize the framebuffer support for the specified display.
 *
 * Input Parameters:
 *   display - In the case of hardware with multiple displays, this
 *     specifies the display.  Normally this is zero.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_fbuninitialize(int display)
{
	arm_lowputc('u');
	arm_lowputc('p');
	arm_lowputc('\r');
	arm_lowputc('\n');
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
		if (fb_rot)
			fb_rot->free(fb_rot);
#endif
}
