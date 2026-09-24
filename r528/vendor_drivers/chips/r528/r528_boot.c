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

#ifndef OPEN_MAX
#define OPEN_MAX 256
#endif
#ifndef CLOCK_MAX
#define CLOCK_MAX 4294967295U
#endif
#include <sys/mount.h>
#include <nuttx/config.h>
#include <nuttx/audio/audio.h>
#if defined(__has_include)
#  if __has_include(<nuttx/lib/modlib.h>)
#    include <nuttx/lib/modlib.h>
#    define R528_USE_MODLIB 1
#  elif __has_include(<nuttx/lib/elf.h>)
#    include <nuttx/lib/elf.h>
#    define R528_USE_LIBELF 1
#  else
#    error "Neither <nuttx/lib/modlib.h> nor <nuttx/lib/elf.h> is available"
#  endif
#else
#  include <nuttx/lib/modlib.h>
#  define R528_USE_MODLIB 1
#endif
#ifdef CONFIG_VIDEO_FB
#include <nuttx/video/fb.h>
#endif
#ifdef CONFIG_LCD_DEV
#include <nuttx/lcd/lcd_dev.h>
#endif
#ifdef CONFIG_USBADB
#include <nuttx/usb/adb.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#ifdef CONFIG_PAGING
#include <nuttx/page.h>
#endif

#include <arm_internal.h>
#include "arm.h"
#include "chip.h"
#include "gic.h"
#include "mmu.h"
#include "scu.h"

#include "r528_boot.h"
#include "r528_lowputc.h"

#include <nuttx/syslog/syslog.h>
#include <sys/boardctl.h>
#include <syslog.h>

#if defined(CONFIG_DRIVERS_TWI)
#include <nuttx/i2c/i2c_master.h>
#endif

#ifdef CONFIG_WATCHDOG
#ifdef CONFIG_R528_WATCHDOG
#include <arch/chip/r528_wdt.h>
#endif
#endif

#ifdef CONFIG_BOARD_USBDEV_SERIALSTR
#include <arch/chip/mi_serialstr.h>
#endif

#if defined(CONFIG_BOARDCTL_BOOT_IMAGE) && !defined(CONFIG_NSH_DISABLE_BOOT)
#include <fcntl.h>
#include <private_rtos.h>
#include <sys/stat.h>
// #include <string.h>
// #include <stdlib.h>
#endif

#ifdef CONFIG_DRIVERS_CCMU
#include <hal_clk.h>
#endif

#ifdef CONFIG_DRIVERS_CE
#include <sunxi_hal_ce.h>
#endif

#include "hwspinlock.h"
#include "platform/timer_sun20iw1.h"
#ifdef CONFIG_ARCH_TRUSTZONE_SECURE
#include "r528_secure.h"
#include "sm.h"
#include "sunxi_hal_timer.h"
#endif

#include <nuttx/rptun/rptun.h>

int g2d_probe(void);
int tlsc6x_init(void);
int r528_pwm_initialize(FAR const char *devpath, int channel_id);
FAR struct i2c_master_s *r528_i2c_initialize(FAR const char *devpath, int i2c_id);
int r528_gpadc_initialize(FAR const char *devpath, int channel_id);
int r528_button_initialize(FAR const char *devname);
int r528_touchscreen_initialize(FAR const char *devname);
#ifdef CONFIG_IEEE80211_REALTEK_WIFI
int realtek_wlan_bringup(void);
#endif

#ifdef CONFIG_MICRO_TF
int micro_sd_initialize(void);
#endif

int r528_read_resetflag(void);
/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* The vectors are, by default, positioned at the beginning of the text
 * section.  They will always have to be copied to the correct location.
 *
 * If we are using high vectors (CONFIG_ARCH_LOWVECTORS=n).  In this case,
 * the vectors will lie at virtual address 0xffff:000 and we will need
 * to a) copy the vectors to another location, and b) map the vectors
 * to that address, and
 *
 * For the case of CONFIG_ARCH_LOWVECTORS=y, defined.  Vectors will be
 * copied to SRAM A1 at address 0x0000:0000
 */

#if !defined(CONFIG_ARCH_LOWVECTORS) && defined(CONFIG_ARCH_ROMPGTABLE)
#  error High vector remap cannot be performed if we are using a ROM page table
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern uint32_t _vector_start; /* Beginning of vector block */
extern uint32_t _vector_end;   /* End+1 of vector block */

static int r528_module_initialize(FAR const char *path,
                                  FAR struct mod_loadinfo_s *loadinfo)
{
#ifdef R528_USE_MODLIB
  return modlib_initialize(path, loadinfo);
#else
  return libelf_initialize(path, loadinfo);
#endif
}

static int r528_module_load(FAR struct mod_loadinfo_s *loadinfo)
{
#ifdef R528_USE_MODLIB
  return modlib_load(loadinfo);
#else
  return libelf_load(loadinfo);
#endif
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* This table describes how to map a set of 1Mb pages to space the physical
 * address space of the r528.
 */
#ifndef CONFIG_ARCH_ROMPGTABLE
static const struct section_mapping_s section_mapping[] =
{
#ifndef ENABLE_L2PGTABLE
  { R528_INTMEM_PSECTION, R528_INTMEM_VSECTION,  /* Includes vectors and page table */
    R528_INTMEM_MMUFLAGS,    R528_INTMEM_NSECTIONS
  },
#endif
  { R528_DSP_PSECTION,    R528_DSP_VSECTION,  /* Includes vectors and page table */
    R528_SYS_MMUFLAGS,    R528_DSP_NSECTIONS,
  },
  { R528_VE_PSECTION,     R528_VE_VSECTION,  /* Includes vectors and page table */
    R528_SYS_MMUFLAGS,    R528_VE_NSECTIONS
  },
  { R528_SP0_PSECTION,    R528_SP0_VSECTION,  /* Includes vectors and page table */
    R528_SYS_MMUFLAGS,    R528_SP0_NSECTIONS
  },
  { R528_SP1_PSECTION,    R528_SP1_VSECTION,  /* Includes vectors and page table */
    R528_SYS_MMUFLAGS,    R528_SP1_NSECTIONS
  },
  { R528_SH0_PSECTION,    R528_SH0_VSECTION,  /* Includes vectors and page table */
    R528_SYS_MMUFLAGS,    R528_SH0_NSECTIONS
  },
  { R528_SH2_PSECTION,  R528_SH2_VSECTION,
    R528_SYS_MMUFLAGS,  R528_SH2_NSECTIONS
  },
  { R528_VIDEO_OUT_PSECTION,  R528_VIDEO_OUT_VSECTION,
    R528_SYS_MMUFLAGS,  R528_VIDEO_OUT_NSECTIONS
  },
  { R528_VIDEO_IN_PSECTION,  R528_VIDEO_IN_VSECTION,
    R528_SYS_MMUFLAGS,       R528_VIDEO_IN_NSECTIONS
  },
  { R528_APBS0_PSECTION,  R528_APBS0_VSECTION,
    R528_SYS_MMUFLAGS,    R528_APBS0_NSECTIONS
  },
  { R528_CPUX_PSECTION,  R528_CPUX_VSECTION,
    R528_SYS_MMUFLAGS,   R528_CPUX_NSECTIONS
  },
  { R528_DDR_MAPPADDR,   R528_DDR_MAPVADDR,
    R528_DDR_MMUFLAGS,   R528_DDR_NSECTIONS
  },
  { R528_DDR_MAPPADDR,   R528_DDR_MAPVADDR2,
    R528_NONCACHE_DDR_MMUFLAGS,   R528_DDR_NSECTIONS
  },
#ifdef CONFIG_AW_RPTUN
  { R528_DSP_DDR_MAPPADDR,   R528_DSP_DDR_MAPVADDR,
    R528_NONCACHE_DDR_MMUFLAGS,   R528_DSP_DDR_NSECTIONS
  },
#endif
  { R528_SAVE_LOG_DDR_MAPPADDR,   R528_SAVE_LOG_DDR_MAPVADDR,
    R528_NONCACHE_DDR_MMUFLAGS,   R528_SAVE_LOG_NSECTIONS
  },
  { R528_DISP_DDR_MAPPADDR,   R528_DISP_DDR_MAPVADDR,
    R528_DDR_MMUFLAGS,   R528_DISP_NSECTIONS
  },
};

#define NMAPPINGS \
  (sizeof(section_mapping) / sizeof(struct section_mapping_s))

#endif
/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: r528_setupmappings
 *
 * Description:
 *   Map all of the initial memory regions defined in section_mapping[]
 *
 ****************************************************************************/
#ifndef CONFIG_ARCH_ROMPGTABLE

#ifdef ENABLE_L2PGTABLE

static inline void r528_setupl2mappings(void)
{
  int i = 0;

  /* you'd better keep only one entry in page_entry_s structure
   * due to the limitations of the mmu_l2_map_page function.
   */
  struct page_entry_s page_entry_0M[] =
  {
    { R528_SRAMA1_PADDR, R528_SRAMA1_VADDR,
      R528_MMU_L2_UDATAFLAGS, R528_SRAM_NPAGES
    },
  };

  struct page_entry_s page_entry_AP_text[] =
  {
    { R528_AP_TEXT_HEAD_PADDR, R528_AP_TEXT_HEAD_VADDR,
      R528_MMU_L2_UDATAFLAGS, R528_AP_TEXT_HEAD_NPAGES
    },
    { R528_AP_TEXT_PADDR, R528_AP_TEXT_VADDR,
      R528_MMU_L2_TEXTFLAGS, R528_AP_TEXT_NPAGES
    },
    { R528_AP_TEXT_TAIL_PADDR, R528_AP_TEXT_TAIL_VADDR,
      R528_MMU_L2_UDATAFLAGS, R528_AP_TEXT_TAIL_NPAGES
    },
  };

  struct page_mapping_s page_mapping[] = {
    { L2PGTABLE_BASE_PADDR_0M,                        /* addr: 0x00000000 - 0x00100000 */
      sizeof(page_entry_0M) / sizeof(struct page_entry_s),
      page_entry_0M
    },
    { L2PGTABLE_BASE_PADDR_AP_TEXT,  /* addr: AP text */
      sizeof(page_entry_AP_text) / sizeof(struct page_entry_s),
      page_entry_AP_text
    },
  };

  struct section_mapping_s section_mapping_l2[] = {
    { L2PGTABLE_BASE_PADDR_0M, R528_INTMEM_VSECTION,  /* addr: 0x00000000 - 0x00100000 */
      MMU_L1_DATAFLAGS, R528_INTMEM_NSECTIONS
    },
    { L2PGTABLE_BASE_PADDR_AP_TEXT, R528_AP_TEXT_HEAD_VADDR,  /* addr: AP text */
      MMU_L1_TEXTFLAGS, R528_AP_TEXT_ALL_NSECTIONS
    },
  };

  struct section_mapping_s section_mapping_dram_xn[] = {
    { R528_AP_DDR_START_PADDR, R528_AP_DDR_START_VADDR,
      R528_DDR_XN_MMUFLAGS, R528_AP_DDR_START_NSECTIONS
    },
    { R528_AP_DDR_END_PADDR, R528_AP_DDR_END_VADDR,
      R528_DDR_XN_MMUFLAGS, R528_AP_DDR_END_NSECTIONS
    },
  };

  /* set l2 pagetable memory to 0 */
  for (i = 0; i < sizeof(section_mapping_l2) / sizeof(struct section_mapping_s);
       i++) {
    memset((void *)section_mapping_l2[i].physbase, 0,
              section_mapping_l2[i].nsections * L2PGTABLE_PER_SIZE);
  }

  /* configure l2 pagetables */
  mmu_l2_map_pages(page_mapping,
              sizeof(page_mapping) / sizeof(struct page_mapping_s));

  /* configure l1 pagetables for l2 pages */
  mmu_l1_map_pages(section_mapping_l2,
              sizeof(section_mapping_l2) / sizeof(struct section_mapping_s));

  /* reconfigure l1 data sections with XN flags */
  mmu_l1_map_regions(section_mapping_dram_xn,
         sizeof(section_mapping_dram_xn) / sizeof(struct section_mapping_s));
}
#else
static inline void r528_setupl2mappings(void)
{
}
#endif

static inline void r528_setupmappings(void)
{

  /* configure l1 pagetables for sections */
  mmu_l1_map_regions(section_mapping, NMAPPINGS);

  /* configure l2 pagetables */
  r528_setupl2mappings();

}
#endif

/****************************************************************************
 * Name: r528_vectorpermissions
 *
 * Description:
 *   Set permissions on the vector mapping.
 *
 ****************************************************************************/

#if !defined(CONFIG_ARCH_ROMPGTABLE) && defined(CONFIG_ARCH_LOWVECTORS) && \
     defined(CONFIG_PAGING)
static void r528_vectorpermissions(uint32_t mmuflags)
{
  /* The PTE for the beginning of ISRAM is at the base of the L2 page table */

  uint32_t pte = mmu_l2_getentry(PG_L2_VECT_VADDR, 0);

  /* Mask out the old MMU flags from the page table entry.
   *
   * The pte might be zero the first time this function is called.
   */

  if (pte == 0)
    {
      pte = PG_VECT_PBASE;
    }
  else
    {
      pte &= PG_L1_PADDRMASK;
    }

  /* Update the page table entry with the MMU flags and save */

  mmu_l2_setentry(PG_L2_VECT_VADDR, pte, 0, mmuflags);
}
#endif

/****************************************************************************
 * Name: r528_vectormapping
 *
 * Description:
 *   Setup a special mapping for the interrupt vectors when (1) the
 *   interrupt vectors are not positioned in ROM, and when (2) the interrupt
 *   vectors are located at the high address, 0xffff0000.  When the
 *   interrupt vectors are located in ROM, we just have to assume that they
 *   were set up correctly;  When vectors  are located in low memory,
 *   0x00000000, the mapping for the ROM memory region will be suppressed.
 *
 ****************************************************************************/

#if !defined(CONFIG_ARCH_ROMPGTABLE) && !defined(CONFIG_ARCH_LOWVECTORS)
static void r528_vectormapping(void)
{
  uint32_t vector_paddr = r528_VECTOR_PADDR & PTE_SMALL_PADDR_MASK;
  uint32_t vector_vaddr = r528_VECTOR_VADDR & PTE_SMALL_PADDR_MASK;
  uint32_t vector_size  = (uint32_t)&_vector_end - (uint32_t)&_vector_start;
  uint32_t end_paddr    = r528_VECTOR_PADDR + vector_size;

  /* REVISIT:  Cannot really assert in this context */

  DEBUGASSERT (vector_size <= VECTOR_TABLE_SIZE);

  /* We want to keep our interrupt vectors and interrupt-related logic in
   * zero-wait state internal SRAM (ISRAM).  The r528 has 128Kb of ISRAM
   * positioned at physical address 0x0300:0000; we need to map this to
   * 0xffff:0000.
   */

  while (vector_paddr < end_paddr)
    {
      mmu_l2_setentry(VECTOR_L2_VBASE,  vector_paddr, vector_vaddr,
                      MMU_L2_VECTORFLAGS);
      vector_paddr += 4096;
      vector_vaddr += 4096;
    }

  /* Now set the level 1 descriptor to refer to the level 2 page table. */

  mmu_l1_setentry(VECTOR_L2_PBASE & PMD_PTE_PADDR_MASK,
                  r528_VECTOR_VADDR & PMD_PTE_PADDR_MASK,
                  MMU_L1_VECTORFLAGS);
}
#else
  /* No vector remap */

#  define r528_vectormapping()
#endif

/****************************************************************************
 * Name: r528_copyvectorblock
 *
 * Description:
 *   Copy the interrupt block to its final destination.  Vectors are already
 *   positioned at the beginning of the text region and only need to be
 *   copied in the case where we are using high vectors.
 *
 ****************************************************************************/

static void r528_copyvectorblock(void)
{
    return;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int direct_printf(const char *fmt, ...)
{
    int ret, i;
    char tmpbuf[512];
    va_list args;

    va_start(args, fmt);
    ret = vsnprintf(tmpbuf, sizeof(tmpbuf), fmt, args);
    va_end(args);
    if (ret < 0 || ret > sizeof(tmpbuf))
        return -EFAULT;

    for (i = 0; i < ret; i++) {
        if (tmpbuf[i] == '\n')
            arm_lowputc('\r');
        arm_lowputc(tmpbuf[i]);
    }

    return 0;
}

/****************************************************************************
 * Name: arm_boot
 *
 * Description:
 *   Complete boot operations started in arm_head.S
 *
 *   This logic will be executing in SDRAM.  This boot logic was started by
 *   the A10 boot logic.  At this point in time, clocking and SDRAM have
 *   already be initialized (they must be because we are executing out of
 *   SDRAM).  So all that must be done here is to:
 *
 *     1) Refine the memory mapping,
 *     2) Configure the serial console, and
 *     3) Perform board-specific initializations.
 *
 ****************************************************************************/
#ifdef CONFIG_SMP
extern void r528_cpu_enable(void);
#endif

uint32_t r528_cpufreq_init(uint32_t freq)
{
#ifdef  CONFIG_DRIVERS_CCMU
	hal_clk_t clk, tmp, pll, div;

	clk = hal_clock_get(HAL_SUNXI_CCU, CLK_CPUX);
	tmp = hal_clock_get(HAL_SUNXI_CCU, CLK_PLL_PERIPH0);
	pll = hal_clock_get(HAL_SUNXI_CCU, CLK_PLL_CPUX);
	div = hal_clock_get(HAL_SUNXI_CCU, CLK_PLL_CPUX_DIV);
	if (!clk || !tmp || !pll || !div)
		return -ENODEV;

	hal_clk_set_parent(clk, tmp);
	hal_clk_set_rate(pll, freq);
	hal_clk_set_rate(div, freq);
	hal_clk_set_parent(clk, div);

	return hal_clk_get_rate(clk);
#endif
	return 0;
}

int r528_gpio_init(void)
{
#ifdef CONFIG_DRIVERS_GPIO
extern int hal_gpio_init(void);
	hal_gpio_init();
#endif
	return 0;
}

#if 0
// move this function to mico_common system.c using vela boardctl to get reset cause
unsigned int get_sw_bootmode_backup(void)
{
#ifndef CHIPPOR_FLAG
#define CHIPPOR_FLAG 0x01
#endif
#ifndef SOFT_RESET_FLAG
#define SOFT_RESET_FLAG 0x02
#endif
#ifndef WDT_TIMEOUT_RESET_FLAG
#define WDT_TIMEOUT_RESET_FLAG 0x04
#endif
	uint32_t reset_flags = r528_read_resetflag();
	reset_flags >>= 16;

#if defined(CONFIG_BOARDCTL_RESET_CAUSE) && !defined(CONFIG_NSH_DISABLE_REBOOT)
	if (BOARDIOC_RESETCAUSE_SYS_CHIPPOR == reset_flags)
		return CHIPPOR_FLAG;
	else if (BOARDIOC_RESETCAUSE_CPU_SOFT == reset_flags)
		return SOFT_RESET_FLAG;
	else if (BOARDIOC_RESETCAUSE_SYS_RWDT == reset_flags)
		return WDT_TIMEOUT_RESET_FLAG;
#endif
	return 0;
}
#endif

int r528_disp_init(void)
{
#ifdef CONFIG_DISP2_SUNXI
#ifdef CONFIG_VIDEO_FB
	int ret = fb_register(0, 0);
	if (ret < 0)
	{
		syslog(LOG_ERR, "Failed to initialize Frame Buffer Driver.\n");
		return ret;
	}
  syslog(LOG_ERR, "succese to initialize Frame Buffer Driver.\n");
#else
	extern int disp_probe(void);
	disp_probe();
#endif
#endif
	return 0;
}

int r528_g2d_init(void)
{
#ifdef CONFIG_DRIVERS_G2D
	g2d_probe();
#endif
	return 0;
}

static int r528_perf_init(void)
{
#ifdef CONFIG_DRIVERS_CCMU
	hal_clk_t clk;
	uint32_t rate;

	clk = hal_clock_get(HAL_SUNXI_CCU, CLK_CPUX);
	rate = hal_clk_get_rate(clk);
	up_perf_init((void *)rate);
#endif
	return 0;
}

#define PROGRESS
void arm_boot(void)
{
#ifndef CONFIG_ARCH_ROMPGTABLE
	r528_setupmappings();
	arm_lowputc('A');

	r528_vectormapping();
	arm_lowputc('B');
#endif

#ifdef CONFIG_SMP
	arm_enable_smp(0);
#endif

	r528_copyvectorblock();

#ifdef CONFIG_ARCH_FPU
	arm_fpuconfig();
#endif

#ifdef CONFIG_BOOT_SDRAM_DATA
	arm_data_initialize();
	arm_lowputc('C');
#endif

#ifdef CONFIG_ARCH_TRUSTZONE_SECURE
	sunxi_spc_set_to_ns();
	sunxi_tzma_set_to_ns();
	sunxi_tzma_set_to_secure(0x20000, 0x4000);
	sunxi_smc_set_to_secure(0x40000000, 0x200000);
#endif

#ifdef CONFIG_SMP
	r528_cpu_enable();
	arm_lowputc('D');
#endif
}

#if defined(CONFIG_X4B_TEE)

static int sec_wdt_timer_isr(int irq, FAR void *context, FAR void *arg)
{
	_alert("Receive the watchdog interrupt: %d\n", irq);
#if 1
	/* disable the SUNXI_R_TMR0 to prevent from receive this irq repeatly */
	hal_timer_irq_disable(SUNXI_R_TMR0);
	up_mdelay(10);

	/* start to dump the REE context */
	struct arm_sm_nsec_ctx *nsec_ctx = arm_sm_get_nsec_ctx();
	_alert("Dump REE registers:\n");
	_alert("R0: %08" PRIx32 " R1: %08" PRIx32
		" R2: %08" PRIx32 " R3: %08" PRIx32
		" R4: %08" PRIx32 " R5: %08" PRIx32 "\n",
		nsec_ctx->r0, nsec_ctx->r1,
		nsec_ctx->r2, nsec_ctx->r3,
		nsec_ctx->r4, nsec_ctx->r5);
	_alert("R6: %08" PRIx32 " R7: %08" PRIx32
		" R8: %08" PRIx32 " R9: %08" PRIx32
		" R10: %08" PRIx32 " R11: %08" PRIx32
		" R12: %08" PRIx32 "\n",
		nsec_ctx->r6, nsec_ctx->r7,
		nsec_ctx->r8, nsec_ctx->r9,
		nsec_ctx->r10, nsec_ctx->r11, nsec_ctx->r12);

	/* the mon_lr is the pc of the non-secure world */
	_alert("SPSR: %08" PRIx32 " PC: %08" PRIx32 "\n",
		nsec_ctx->mon_spsr, nsec_ctx->mon_lr);

	_alert("Dump REE usr mode registers:\n");
	_alert("SP: %08" PRIx32 " LR: %08" PRIx32 "\n",
		nsec_ctx->regs.usr_sp, nsec_ctx->regs.usr_lr);

	_alert("Dump REE irq mode registers:\n");
	_alert("SP: %08" PRIx32 " LR: %08" PRIx32
		" SPSR: %08" PRIx32 "\n",
		nsec_ctx->regs.irq_sp, nsec_ctx->regs.irq_lr,
		nsec_ctx->regs.irq_spsr);
	_alert("Dump REE fiq mode registers:\n");
	_alert("SP: %08" PRIx32 " LR: %08" PRIx32
		" SPSR: %08" PRIx32 "\n",
		nsec_ctx->regs.fiq_sp, nsec_ctx->regs.fiq_lr,
		nsec_ctx->regs.fiq_spsr);
	_alert("Dump REE svc mode registers:\n");
	_alert("SP: %08" PRIx32 " LR: %08" PRIx32
		" SPSR: %08" PRIx32 "\n",
		nsec_ctx->regs.svc_sp, nsec_ctx->regs.svc_lr,
		nsec_ctx->regs.svc_spsr);
	_alert("Dump REE abt mode registers:\n");
	_alert("SP: %08" PRIx32 " LR: %08" PRIx32
		" SPSR: %08" PRIx32 "\n",
		nsec_ctx->regs.abt_sp, nsec_ctx->regs.abt_lr,
		nsec_ctx->regs.abt_spsr);
	_alert("Dump REE und mode registers:\n");
	_alert("SP: %08" PRIx32 " LR: %08" PRIx32
		" SPSR: %08" PRIx32 "\n",
		nsec_ctx->regs.und_sp, nsec_ctx->regs.und_lr,
		nsec_ctx->regs.und_spsr);

	syslog_flush();

	/* force the REE pc as 0, thus to generate a crash in REE, and this
	 * can dump out the whole context of REE
	 */
	nsec_ctx->mon_lr = 0;
#endif
	return OK;
}

#endif

void r528_early_initialize(void)
{
	arm_lowputc('E');
#if defined(CONFIG_ARCH_TRUSTZONE_SECURE)
	up_secure_irq(SUNXI_IRQ_HWSPINLOCK, true);
	up_secure_irq(R528_IRQ_DMA_S, true);
	/* make the watchdog timer as fiq */
	int wdt_irq = SUNXI_IRQ_R_TMR(SUNXI_TMR0);
	up_secure_irq(wdt_irq, true);
	hal_enable_irq(wdt_irq);
	irq_attach(wdt_irq, sec_wdt_timer_isr, NULL);
#endif

	r528_cpufreq_init(1200 * 1000 * 1000);
	r528_gpio_init();
	r528_perf_init();
#ifdef CONFIG_R528_WATCHDOG
	r528_wdt_initialize(CONFIG_WATCHDOG_DEVPATH);
#endif
	arm_lowputc('G');
	syslog_flush();
}

void r528_late_initialize(void)
{
	arm_lowputc('H');
#ifdef CONFIG_ARCH_TRUSTZONE_SECURE
	up_irq_enable();
#endif
	extern void spare_rtos_head_dummy(void);
	spare_rtos_head_dummy();
#ifdef CONFIG_DRIVERS_DMA
	int hal_dma_init(void);
	hal_dma_init();
#endif

#ifdef CONFIG_DRIVERS_HWSPINLOCK
	void hal_hwspinlock_init(void);
	hal_hwspinlock_init();
#ifndef CONFIG_ARCH_TRUSTZONE_SECURE
	void hal_hwspinlock_irq_enable(int);
	hal_hwspinlock_irq_enable(0);
#endif
#endif

#ifdef CONFIG_DRIVERS_PWM
#ifdef CONFIG_LCD_SUPPORT_BOE_1200X1920
  r528_pwm_initialize("/dev/pwm0", 4);
#else
	r528_pwm_initialize("/dev/pwm0", 0);
#endif
#endif

#ifdef CONFIG_DRIVERS_TWI
#ifdef CONFIG_R528_TWI0
  unused_data struct i2c_master_s *i2c_bus0;
  i2c_bus0 = r528_i2c_initialize("/dev/i2c0", 0);
#endif
#ifdef CONFIG_R528_TWI1
  unused_data struct i2c_master_s *i2c_bus1;
  i2c_bus1 = r528_i2c_initialize("/dev/i2c1", 1);
#endif
#ifdef CONFIG_R528_TWI2
  unused_data struct i2c_master_s *i2c_bus2;
  i2c_bus2 = r528_i2c_initialize("/dev/i2c2", 2);
#endif

#ifdef CONFIG_GT911_IIC_TOUCH
  extern int gt911_register(FAR const char *devpath,
                            FAR struct i2c_master_s *dev);
  if (i2c_bus0)
    gt911_register("/dev/input0", i2c_bus0);
#endif

#ifdef CONFIG_GT9271_IIC_TOUCH
  extern int gt9271_register(FAR const char *devpath,
                             FAR struct i2c_master_s *dev);
  if (i2c_bus0)
    gt9271_register("/dev/input0", i2c_bus0);
#endif

#ifdef CONFIG_SENSORS_SHTC3
  extern int shtc3_register(int devno, FAR struct i2c_master_s *i2c);
  if (i2c_bus2) {
    shtc3_register(0, i2c_bus2);
  }
#endif
#ifdef CONFIG_SENSORS_LTR553
  extern int ltr553_register(int devno, FAR struct i2c_master_s *i2c);
  if (i2c_bus2) {
    ltr553_register(0, i2c_bus2);
  }
#endif
#ifdef CONFIG_SENSORS_SGP30_UORB
  extern int sgp30_uorb_register(int devno, FAR struct i2c_master_s *i2c);
  if (i2c_bus2) {
    sgp30_uorb_register(0, i2c_bus2);
  }
#endif
#ifdef CONFIG_SENSORS_BMI160_I2C
  extern int bmi160_register(FAR const char *devpath,
                             FAR struct i2c_master_s *dev);
  if (i2c_bus2)
    bmi160_register(0, i2c_bus2);
#endif

#endif

#ifdef CONFIG_DRIVERS_GPADC
	r528_gpadc_initialize("/dev/adc0", 0);
#endif

#ifdef CONFIG_DRIVERS_LRADC
	r528_button_initialize("/dev/input/event1");
#endif

#ifdef CONFIG_DRIVERS_TPADC
	r528_touchscreen_initialize("/dev/input0");
#endif

#ifdef CONFIG_DRIVERS_MSGBOX
	uint32_t hal_msgbox_init(void);
	hal_msgbox_init();
#endif
#ifdef CONFIG_DRIVERS_NAND_FLASH
  int ret = 0;
	int hal_nand_init(void);
  ret = hal_nand_init();
  if(ret != 0) {
     syslog(LOG_ERR, "Failed to hal_nand_init: %d\n", ret);
     PANIC();
  }
#endif
	/* 111(ota), 2M bytes */
	// ramdisk_register(111, (uint8_t *)0x47E00000, 2 * 1024, 1024, RDFLAG_WRENABLED | RDFLAG_FUNLINK);

	r528_g2d_init();
#ifdef CONFIG_DRIVERS_USB
	int sunxi_usb_init(void);
	//sunxi_usb_init();
#endif

#ifdef CONFIG_AW_RPMSG_MEMBLK
	int rpmsg_memblk_init(void);
	rpmsg_memblk_init();
#endif

#ifdef CONFIG_BOARD_USBDEV_SERIALSTR
	usbdev_serialstr_init();
#endif

#ifdef CONFIG_DRIVERS_USB
#if defined(CONFIG_USBADB) && !defined(CONFIG_BOARDCTL_USBDEVCTRL)
	usbdev_adb_initialize();
#endif
#endif

#ifdef CONFIG_AW_AUDIO_RPMSG_CTRL
	void sunxi_audio_rpmsg_ctrl_init(void);
	sunxi_audio_rpmsg_ctrl_init();
#endif

#ifdef CONFIG_DRIVERS_SOUND
	int sunxi_soundcard_init(void);
	sunxi_soundcard_init();
#endif

#ifdef CONFIG_AW_DRIVERS_AUDIO

#ifdef CONFIG_AW_AUDIO_CODEC
	FAR struct audio_lowerhalf_s *
		sunxi_audio_initialize(bool record, int index);
	audio_register("pcm0c", sunxi_audio_initialize(true, 0));
	audio_register("pcm0p", sunxi_audio_initialize(false, 0));
#endif

#ifdef CONFIG_AW_AUDIO_DMIC
	FAR struct audio_lowerhalf_s *
		sunxi_audio_initialize(bool record, int index);
	audio_register("pcm1c", sunxi_audio_initialize(true, 1));
#endif

#ifdef CONFIG_AW_AUDIO_X4B_DRIVER
	FAR struct audio_lowerhalf_s *
		x4b_record_initialize(int index);
	audio_register("pcm10c", x4b_record_initialize(10));
	audio_register("pcm11c", x4b_record_initialize(11));
	audio_register("pcm12c", x4b_record_initialize(12));
	audio_register("pcm13c", x4b_record_initialize(13));
#endif

#endif

#ifdef CONFIG_AW_RPTUN
	int sunxi_rptun_init(void);
	sunxi_rptun_init();

#endif

#ifdef CONFIG_IOEXPANDER
    struct ioexpander_dev_s *r528_gpio_initialize(void);
    r528_gpio_initialize();
#endif

#ifdef CONFIG_SPI_DRIVER
#if  defined(CONFIG_LCD_SSD1306_SPI_HW) || defined(CONFIG_LCD_ILI9341_HARDWARE_SPI) ||defined(CONFIG_LCD_ST7789_HARDWARE_SPI)
extern struct spi_dev_s *sunxi_spibus_initialize(int port);
    sunxi_spibus_initialize(1);
#endif
#endif



#ifdef CONFIG_DRIVERS_TLSC6X
    tlsc6x_init();
#endif

#ifdef CONFIG_SENSORS_HX3203
    int hx3203_initialize_uorb(void);
    hx3203_initialize_uorb();
#endif
#ifdef CONFIG_IEEE80211_REALTEK_WIFI
  realtek_wlan_bringup();
#endif

#ifdef CONFIG_COMPONENTS_BLUETOOTH
    /* 2026-09-15 暂时关闭蓝牙（用户要求，勿删）：rtk8723FS_initialize 会创建
     * bt_recv/bt_fw 内核线程，bt_recv 空转占满 CPU1（top 50.2%），且蓝牙 H5
     * 一直 bthci open timeout(-110) 不可用。以后要用再把下面两行放开。 */
    /* int rtk8723FS_initialize (uint8_t id); */
    /* rtk8723FS_initialize(0); */
#endif

#ifdef CONFIG_DRIVERS_CE
	syslog(LOG_INFO, "sunxi ce init...\n");
	sunxi_ce_init();
#endif

#ifdef CONFIG_DRIVERS_RC
#if defined(CONFIG_DRIVERS_CIR_RX) || defined(CONFIG_DRIVERS_CIR_TX)
  syslog(LOG_INFO, "r528_rc_initialize...\n");
  extern int r528_rc_initialize(void);
  r528_rc_initialize();
#endif
#endif

#ifdef CONFIG_R528_PROTECT_BROM
  int r528_protect_brom(void);
  r528_protect_brom();
#endif

#ifdef CONFIG_DRIVERS_LEDC
#ifdef CONFIG_LED_RGB_WS2812
    struct ws2812_dev_s *
      r528_ws2812_setup(const char  *path,
                        uint16_t    led_count,
                        bool        has_white);
    r528_ws2812_setup("dev/leds0", 1, false);
#endif
#endif

#ifndef CONFIG_ARCH_TRUSTZONE_SECURE
#ifdef CONFIG_LCD
  // Initialize the LCD board
  extern int board_lcd_initialize(void);
  int lcd_ret = board_lcd_initialize();
  if (lcd_ret < 0)
  {
    syslog(LOG_ERR, "ERROR: board_lcd_initialize() failed: %d\n", lcd_ret);
  }
#ifdef CONFIG_LCD_DEV
     // Register the LCD device
     lcd_ret = lcddev_register(0);
     if (lcd_ret < 0)
     {
         syslog(LOG_ERR, "ERROR: lcddev_register() failed: %d\n", lcd_ret);
     }
#endif /* CONFIG_LCD_DEV */
#else
    r528_disp_init();
#endif
#endif

#ifdef CONFIG_MICRO_TF
    micro_sd_initialize();
#endif

	syslog(LOG_INFO, "r528_late_initialize finish \n");
}

#if defined(CONFIG_BOARDCTL_BOOT_IMAGE) && !defined(CONFIG_NSH_DISABLE_BOOT)
extern struct spare_rtos_head_t  rtos_spare_head;

#ifdef CONFIG_ARCH_TRUSTZONE_SECURE
volatile uint32_t g_ap_entry;
#endif

#ifdef CONFIG_ARMV7A_SMP_BUSY_WAIT
volatile uint32_t *g_smp_busy_wait = (uint32_t *)CONFIG_ARMV7A_SMP_BUSY_WAIT_FLAG_ADDR;
#endif
int board_boot_image(const char *path, uint32_t hdr_size)
{
#ifdef CONFIG_ARCH_TRUSTZONE_SECURE
  up_irq_enable();
#endif
  int ret;

  struct mod_loadinfo_s loadinfo;

  syslog(LOG_INFO, "Loading file: %s hdr_size:%ld\n", path, hdr_size);

  /* Initialize the ELF library to load the program binary. */
  syslog(LOG_INFO, "elf_init...\n");

  ret = r528_module_initialize(path, &loadinfo);
  if (ret != 0)
    {
      syslog(LOG_ERR, "Failed to initialize module image: %d\n", ret);
      syslog_flush();
      return -1;
    }

  /* Load the program binary */
  syslog(LOG_INFO, "elf_load...\n");
  ret = r528_module_load(&loadinfo);
  if (ret != 0)
    {
      syslog(LOG_ERR, "Failed to load module image: %d\n", ret);
      syslog_flush();
    }

#ifdef CONFIG_X4B_TEE
#ifdef CONFIG_DRIVERS_NAND_FLASH
  void hal_nand_exit(void);
  hal_nand_exit();
#endif
#endif

  void hal_dcache_clean_all(void);
  hal_dcache_clean_all();
#ifdef CONFIG_ARCH_TRUSTZONE_SECURE
  up_irq_disable();

  /* reset busy wait status */

#ifdef CONFIG_ARMV7A_SMP_BUSY_WAIT
  *g_smp_busy_wait = 0;
  UP_DSB();
#endif

  g_ap_entry = loadinfo.ehdr.e_entry;
#else
  ((start_t)loadinfo.ehdr.e_entry)();
#endif

  return 0;
}
#endif
