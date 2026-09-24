/*
* Copyright (c) 2019-2025 Allwinner Technology Co., Ltd. ALL rights reserved.
*
* Allwinner is a trademark of Allwinner Technology Co.,Ltd., registered in
* the the people's Republic of China and other countries.
* All Allwinner Technology Co.,Ltd. trademarks are used with permission.
*
* DISCLAIMER
* THIRD PARTY LICENCES MAY BE REQUIRED TO IMPLEMENT THE SOLUTION/PRODUCT.
* IF YOU NEED TO INTEGRATE THIRD PARTY’S TECHNOLOGY (SONY, DTS, DOLBY, AVS OR MPEGLA, ETC.)
* IN ALLWINNERS’SDK OR PRODUCTS, YOU SHALL BE SOLELY RESPONSIBLE TO OBTAIN
* ALL APPROPRIATELY REQUIRED THIRD PARTY LICENCES.
* ALLWINNER SHALL HAVE NO WARRANTY, INDEMNITY OR OTHER OBLIGATIONS WITH RESPECT TO MATTERS
* COVERED UNDER ANY REQUIRED THIRD PARTY LICENSE.
* YOU ARE SOLELY RESPONSIBLE FOR YOUR USAGE OF THIRD PARTY’S TECHNOLOGY.
*
*
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hal_dma.h>
#include <hal_clk.h>
#include <hal_gpio.h>
#include <hal_timer.h>
#include <hal_reset.h>
#include <sunxi_hal_timer.h>
#include <sunxi_hal_efuse.h>
#include <sound/snd_core.h>
#include <sound/snd_io.h>
#include <sound/snd_dma.h>
#include <sound/snd_pcm.h>
#include <sound/dma_wrap.h>
#include <sound/pcm_common.h>

#include "sun8iw20-codec.h"

struct snd_codec sunxi_audiocodec;

enum {
	PB_AUDIO_ROUTE_LINEOUT_SPK = 0,
	PB_AUDIO_ROUTE_HEADPHONE_SPK,
	PB_AUDIO_ROUTE_LINEOUT,
	PB_AUDIO_ROUTE_HEADPHONE,
	PB_AUDIO_ROUTE_LO_HP,
	PB_AUDIO_ROUTE_LO_HP_SPK,
	PB_AUDIO_ROUTE_MAX,
};

enum {
	ADC_AUDIO_ROUTE_ON = 0x1,
	ADC_AUDIO_ROUTE_MIC = 0x2,
	ADC_AUDIO_ROUTE_LINEIN = 0x4,
	ADC_AUDIO_ROUTE_FMIN = 0x8,
};

#ifndef CONFIG_BOARD_CONFIG
static struct sunxi_codec_param default_param = {
	/* digital_vol 保持 0x0（最大）：直接写 DAC_DPC.DVOL 会绕过
	 * sunxi_set_data_invert 的反转语义（寄存器=63-音量），0x2C 实测导致
	 * 整机无声（ok-20260806-31 教训）。音量 30% 改由 dm_sound_write
	 * 软件衰减实现（可靠可控）。 */
	.digital_vol		= 0x0,
	.hpout_vol		= 0x3,
	.lineout_vol		= 0x1a,
	.mic1gain		= 0x1f,
	.mic2gain		= 0x1f,
	.mic3gain		= 0x1f,
	.lineingain		= 0x0,
#if defined(CONFIG_ARCH_BOARD_R528S3_EVB4) || defined(CONFIG_ARCH_BOARD_R528S3_GEMINI_S1)
	.gpio_spk		= GPIOD(17),
#elif CONFIG_ARCH_BOARD_R528S3_X4B
	.gpio_spk		= GPIOB(2),
#else
	.gpio_spk		= GPIOB(2),
#endif
	.pa_msleep_time 	= 20,
	.pa_level		= GPIO_DATA_HIGH,
	.adcdrc_cfg     	= 0,
	.adchpf_cfg     	= 1,
	.dacdrc_cfg     	= 0,
	.dachpf_cfg     	= 0,
	.pb_audio_route 	= PB_AUDIO_ROUTE_LO_HP_SPK,
	.rx_sync_en     	= false,
	.rx_sync_ctl		= false,
	.hp_detect_used		= false,
};
#else
#include "g_board_config.h"
static struct sunxi_codec_param default_param = BOARD_CONFIG_CODEC_PARAM;
#endif

struct sample_rate {
	unsigned int samplerate;
	unsigned int rate_bit;
};

static const struct sample_rate sample_rate_conv[] = {
	{44100, 0},
	{48000, 0},
	{8000, 5},
	{32000, 1},
	{22050, 2},
	{24000, 2},
	{16000, 3},
	{11025, 4},
	{12000, 4},
	{192000, 6},
	{96000, 7},
	{88200, 7}, /* audio spec do not supply 88.2k */
};

static int sunxi_get_adc_ch(struct snd_codec *codec)
{
	uint32_t reg_val;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *codec_param = &sunxi_codec->param;

	codec_param->adc1_flag = 0;
	reg_val = snd_codec_read(codec, SUNXI_ADC1_ANA_CTL);

	if (reg_val & (1 << MIC_PGA_EN)) {
		codec_param->adc1_flag = ADC_AUDIO_ROUTE_MIC;
	} else if (reg_val & (1 << FMINL_EN)) {
		codec_param->adc1_flag = ADC_AUDIO_ROUTE_FMIN;
	} else if (reg_val & (1 << LINEINL_EN)) {
		codec_param->adc1_flag = ADC_AUDIO_ROUTE_FMIN;
	} else if (codec_param->adc1_flag != 0) {
		codec_param->adc1_flag |= ADC_AUDIO_ROUTE_ON;
	}

	codec_param->adc2_flag = 0;
	reg_val = snd_codec_read(codec, SUNXI_ADC2_ANA_CTL);

	if (reg_val & (1 << MIC_PGA_EN)) {
		codec_param->adc2_flag = ADC_AUDIO_ROUTE_MIC;
	} else if (reg_val & (1 << FMINR_EN)) {
		codec_param->adc2_flag = ADC_AUDIO_ROUTE_FMIN;
	} else if (reg_val & (1 << LINEINR_EN)) {
		codec_param->adc2_flag = ADC_AUDIO_ROUTE_FMIN;
	} else if (codec_param->adc2_flag != 0) {
		codec_param->adc2_flag |= ADC_AUDIO_ROUTE_ON;
	}

	codec_param->adc3_flag = 0;
	reg_val = snd_codec_read(codec, SUNXI_ADC3_ANA_CTL);

	if (reg_val & (1 << MIC_PGA_EN)) {
		codec_param->adc3_flag = ADC_AUDIO_ROUTE_MIC;
	} else if (codec_param->adc3_flag != 0) {
		codec_param->adc3_flag |= ADC_AUDIO_ROUTE_ON;
	}

	if (codec_param->adc1_flag || codec_param->adc2_flag ||
		codec_param->adc3_flag)
		return 0;

	return -1;
}

#ifdef SUNXI_CODEC_DAP_ENABLE
static void adcdrc_config(struct snd_codec *codec)
{
	/* Left peak filter attack time */
	snd_codec_write(codec, SUNXI_ADC_DRC_LPFHAT, (0x000B77BF >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LPFLAT, 0x000B77BF & 0xFFFF);
	/* Right peak filter attack time */
	snd_codec_write(codec, SUNXI_ADC_DRC_RPFHAT, (0x000B77BF >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_RPFLAT, 0x000B77BF & 0xFFFF);
	/* Left peak filter release time */
	snd_codec_write(codec, SUNXI_ADC_DRC_LPFHRT, (0x00FFE1F8 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LPFLRT, 0x00FFE1F8 & 0xFFFF);
	/* Right peak filter release time */
	snd_codec_write(codec, SUNXI_ADC_DRC_RPFHRT, (0x00FFE1F8 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_RPFLRT, 0x00FFE1F8 & 0xFFFF);

	/* Left RMS filter attack time */
	snd_codec_write(codec, SUNXI_ADC_DRC_LPFHAT, (0x00012BAF >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LPFLAT, 0x00012BAF & 0xFFFF);
	/* Right RMS filter attack time */
	snd_codec_write(codec, SUNXI_ADC_DRC_RPFHAT, (0x00012BAF >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_RPFLAT, 0x00012BAF & 0xFFFF);

	/* smooth filter attack time */
	snd_codec_write(codec, SUNXI_ADC_DRC_SFHAT, (0x00017665 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_SFLAT, 0x00017665 & 0xFFFF);
	/* gain smooth filter release time */
	snd_codec_write(codec, SUNXI_ADC_DRC_SFHRT, (0x00000F04 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_SFLRT, 0x00000F04 & 0xFFFF);

	/* OPL */
	snd_codec_write(codec, SUNXI_ADC_DRC_HOPL, (0xFBD8FBA7 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LOPL, 0xFBD8FBA7 & 0xFFFF);
	/* OPC */
	snd_codec_write(codec, SUNXI_ADC_DRC_HOPC, (0xF95B2C3F >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LOPC, 0xF95B2C3F & 0xFFFF);
	/* OPE */
	snd_codec_write(codec, SUNXI_ADC_DRC_HOPE, (0xF45F8D6E >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LOPE, 0xF45F8D6E & 0xFFFF);
	/* LT */
	snd_codec_write(codec, SUNXI_ADC_DRC_HLT, (0x01A934F0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LLT, 0x01A934F0 & 0xFFFF);
	/* CT */
	snd_codec_write(codec, SUNXI_ADC_DRC_HCT, (0x06A4D3C0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LCT, 0x06A4D3C0 & 0xFFFF);
	/* ET */
	snd_codec_write(codec, SUNXI_ADC_DRC_HET, (0x0BA07291 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LET, 0x0BA07291 & 0xFFFF);
	/* Ki */
	snd_codec_write(codec, SUNXI_ADC_DRC_HKI, (0x00051EB8 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LKI, 0x00051EB8 & 0xFFFF);
	/* Kc */
	snd_codec_write(codec, SUNXI_ADC_DRC_HKC, (0x00800000 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LKC, 0x00800000 & 0xFFFF);
	/* Kn */
	snd_codec_write(codec, SUNXI_ADC_DRC_HKN, (0x01000000 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LKN, 0x01000000 & 0xFFFF);
	/* Ke */
	snd_codec_write(codec, SUNXI_ADC_DRC_HKE, (0x0000F45F >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LKE, 0x0000F45F & 0xFFFF);
}

static void adcdrc_enable(struct snd_codec *codec, bool on)
{
	if (on) {
		snd_codec_update_bits(codec, SUNXI_ADC_DAP_CTL,
			(0x1 << ADC_DRC0_EN | 0x1 << ADC_DRC1_EN),
			(0x1 << ADC_DRC0_EN | 0x1 << ADC_DRC1_EN));
	} else {
		snd_codec_update_bits(codec, SUNXI_ADC_DAP_CTL,
			(0x1 << ADC_DRC0_EN | 0x1 << ADC_DRC1_EN),
			(0x0 << ADC_DRC0_EN | 0x0 << ADC_DRC1_EN));
	}
}

static void adchpf_config(struct snd_codec *codec)
{
	/* HPF */
	snd_codec_write(codec, SUNXI_ADC_DRC_HHPFC, (0xFFFAC1 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_ADC_DRC_LHPFC, 0xFFFAC1 & 0xFFFF);
}

static void adchpf_enable(struct snd_codec *codec, bool on)
{
	if (on) {
		snd_codec_update_bits(codec, SUNXI_ADC_DAP_CTL,
			(0x1 << ADC_HPF0_EN | 0x1 << ADC_HPF1_EN),
			(0x1 << ADC_HPF0_EN | 0x1 << ADC_HPF1_EN));
	} else {
		snd_codec_update_bits(codec, SUNXI_ADC_DAP_CTL,
			(0x1 << ADC_HPF0_EN | 0x1 << ADC_HPF1_EN),
			(0x0 << ADC_HPF0_EN | 0x0 << ADC_HPF1_EN));
	}
}

static void dacdrc_config(struct snd_codec *codec)
{
	/* Left peak filter attack time */
	snd_codec_write(codec, SUNXI_DAC_DRC_LPFHAT, (0x000B77BF >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LPFLAT, 0x000B77BF & 0xFFFF);
	/* Right peak filter attack time */
	snd_codec_write(codec, SUNXI_DAC_DRC_RPFHAT, (0x000B77F0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_RPFLAT, 0x000B77F0 & 0xFFFF);

	/* Left peak filter release time */
	snd_codec_write(codec, SUNXI_DAC_DRC_LPFHRT, (0x00FFE1F8 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LPFLRT, 0x00FFE1F8 & 0xFFFF);
	/* Right peak filter release time */
	snd_codec_write(codec, SUNXI_DAC_DRC_RPFHRT, (0x00FFE1F8 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_RPFLRT, 0x00FFE1F8 & 0xFFFF);

	/* Left RMS filter attack time */
	snd_codec_write(codec, SUNXI_DAC_DRC_LRMSHAT, (0x00012BB0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LRMSLAT, 0x00012BB0 & 0xFFFF);
	/* Right RMS filter attack time */
	snd_codec_write(codec, SUNXI_DAC_DRC_RRMSHAT, (0x00012BB0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_RRMSLAT, 0x00012BB0 & 0xFFFF);

	/* smooth filter attack time */
	snd_codec_write(codec, SUNXI_DAC_DRC_SFHAT, (0x00017665 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_SFLAT, 0x00017665 & 0xFFFF);
	/* gain smooth filter release time */
	snd_codec_write(codec, SUNXI_DAC_DRC_SFHRT, (0x00000F04 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_SFLRT, 0x00000F04 & 0xFFFF);

	/* OPL */
	snd_codec_write(codec, SUNXI_DAC_DRC_HOPL, (0xFF641741 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LOPL, 0xFF641741 & 0xFFFF);
	/* OPC */
	snd_codec_write(codec, SUNXI_DAC_DRC_HOPC, (0xF9E8E88C >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LOPC, 0xF9E8E88C & 0xFFFF);
	/* OPE */
	snd_codec_write(codec, SUNXI_DAC_DRC_HOPE, (0xF5DE3D14 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LOPE, 0xF5DE3D14 & 0xFFFF);
	/* LT */
	snd_codec_write(codec, SUNXI_DAC_DRC_HLT, (0x0336110B >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LLT, 0x0336110B & 0xFFFF);
	/* CT */
	snd_codec_write(codec, SUNXI_DAC_DRC_HCT, (0x08BF6C28 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LCT, 0x08BF6C28 & 0xFFFF);
	/* ET */
	snd_codec_write(codec, SUNXI_DAC_DRC_HET, (0x0C9F9255 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LET, 0x0C9F9255 & 0xFFFF);
	/* Ki */
	snd_codec_write(codec, SUNXI_DAC_DRC_HKI, (0x001A7B96 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LKI, 0x001A7B96 & 0xFFFF);
	/* Kc */
	snd_codec_write(codec, SUNXI_DAC_DRC_HKC, (0x00FD70A5 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LKC, 0x00FD70A5 & 0xFFFF);
	/* Kn */
	snd_codec_write(codec, SUNXI_DAC_DRC_HKN, (0x010AF8B0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LKN, 0x010AF8B0 & 0xFFFF);
	/* Ke */
	snd_codec_write(codec, SUNXI_DAC_DRC_HKE, (0x06286BA0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LKE, 0x06286BA0 & 0xFFFF);
	/* MXG */
	snd_codec_write(codec, SUNXI_DAC_DRC_MXGHS, (0x035269E0 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_MXGLS, 0x035269E0 & 0xFFFF);
	/* MNG */
	snd_codec_write(codec, SUNXI_DAC_DRC_MNGHS, (0xF95B2C3F >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_MNGLS, 0xF95B2C3F & 0xFFFF);
	/* EPS */
	snd_codec_write(codec, SUNXI_DAC_DRC_EPSHC, (0x00025600 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_EPSLC, 0x00025600 & 0xFFFF);
}

static void dacdrc_enable(struct snd_codec *codec, bool on)
{
	if (on) {
		/* detect noise when ET enable */
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_NOISE_DET_EN),
			(0x1 << DAC_DRC_NOISE_DET_EN));

		/* 0x0:RMS filter; 0x1:Peak filter */
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_SIGNAL_SEL),
			(0x1 << DAC_DRC_SIGNAL_SEL));

		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_GAIN_MAX_EN),
			(0x1 << DAC_DRC_GAIN_MAX_EN));

		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_GAIN_MIN_EN),
			(0x1 << DAC_DRC_GAIN_MIN_EN));

		/* delay function enable */
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_DELAY_BUF_EN),
			(0x1 << DAC_DRC_DELAY_BUF_EN));

		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_LT_EN),
			(0x1 << DAC_DRC_LT_EN));
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_ET_EN),
			(0x1 << DAC_DRC_ET_EN));

		snd_codec_update_bits(codec, SUNXI_DAC_DAP_CTL,
			(0x1 << DDAP_DRC_EN),
			(0x1 << DDAP_DRC_EN));
	} else {
		snd_codec_update_bits(codec, SUNXI_DAC_DAP_CTL,
			(0x1 << DDAP_DRC_EN),
			(0x0 << DDAP_DRC_EN));

		/* detect noise when ET enable */
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_NOISE_DET_EN),
			(0x0 << DAC_DRC_NOISE_DET_EN));

		/* 0x0:RMS filter; 0x1:Peak filter */
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_SIGNAL_SEL),
			(0x0 << DAC_DRC_SIGNAL_SEL));

		/* delay function enable */
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_DELAY_BUF_EN),
			(0x0 << DAC_DRC_DELAY_BUF_EN));

		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_GAIN_MAX_EN),
			(0x0 << DAC_DRC_GAIN_MAX_EN));
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_GAIN_MIN_EN),
			(0x0 << DAC_DRC_GAIN_MIN_EN));

		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_LT_EN),
			(0x0 << DAC_DRC_LT_EN));
		snd_codec_update_bits(codec, SUNXI_DAC_DRC_CTRL,
			(0x1 << DAC_DRC_ET_EN),
			(0x0 << DAC_DRC_ET_EN));
	}
}

static void dachpf_config(struct snd_codec *codec)
{
	/* HPF */
	snd_codec_write(codec, SUNXI_DAC_DRC_HHPFC, (0xFFFAC1 >> 16) & 0xFFFF);
	snd_codec_write(codec, SUNXI_DAC_DRC_LHPFC, 0xFFFAC1 & 0xFFFF);
}

static void dachpf_enable(struct snd_codec *codec, bool on)
{
	if (on) {
		snd_codec_update_bits(codec, SUNXI_DAC_DAP_CTL,
			(0x1 << DDAP_HPF_EN),
			(0x1 << DDAP_HPF_EN));
	} else {
		snd_codec_update_bits(codec, SUNXI_DAC_DAP_CTL,
			(0x1 << DDAP_HPF_EN),
			(0x0 << DDAP_HPF_EN));
	}
}
#endif

static const char * const lineout_mux[] = {
			"DAC_SINGLE", "DAC_DIFFER"};

static const char * const micin_select[] = {
			"MIC_DIFFER", "MIC_SINGLE"};

static const char *sunxi_switch_text[] = {"Off", "On"};

static const char * const pb_audio_route_text[] = {
			"lo+spk", "hp+spk", "lineout", "headphone",
			"lo+hp", "lo+hp+spk"};

static int sunxi_pb_audio_route_get_data(struct snd_kcontrol *kcontrol,
				struct snd_ctl_info *info)
{
	struct snd_codec *codec = kcontrol->private_data;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	snd_kcontrol_to_snd_ctl_info(kcontrol, info, param->pb_audio_route);
	return 0;
}

static int sunxi_pb_audio_route_set_data(struct snd_kcontrol *kcontrol,
						unsigned long value)
{
	struct snd_codec *codec = kcontrol->private_data;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	if (value >= PB_AUDIO_ROUTE_MAX)
		return -1;

	param->pb_audio_route = value;
	return 0;
}

static inline int sunxi_get_data_invert(struct snd_kcontrol *kcontrol,
		struct snd_ctl_info *info)
{
	struct snd_codec *codec = kcontrol->private_data;

	int value = 0;
	value = snd_codec_read(codec, kcontrol->reg);
	value = (kcontrol->max & value >> kcontrol->shift);

	info->value = kcontrol->max - value;
	info->id = kcontrol->id;
	info->type = kcontrol->type;
	info->name = kcontrol->name;
	info->min = kcontrol->min;
	info->max = kcontrol->max;
	info->count = kcontrol->count;
	info->items = kcontrol->items;
	info->texts = kcontrol->texts;

	return 0;
}

static inline int sunxi_set_data_invert(struct snd_kcontrol *kcontrol,
		unsigned long value)
{
	struct snd_codec *codec = kcontrol->private_data;

	if (value > kcontrol->max || value < kcontrol->min)
		return -1;

	value = kcontrol->max - value;
	snd_codec_update_bits(codec, kcontrol->reg,
			(kcontrol->max << kcontrol->shift), (value << kcontrol->shift));
	return 0;
}

static void sunxi_rx_sync_enable(void *data, bool enable)
{
	struct snd_codec *codec = data;

	if (enable) {
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				      1 << RX_SYNC_EN_STA, 1 << RX_SYNC_EN_STA);
	} else {
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				      1 << RX_SYNC_EN_STA, 0 << RX_SYNC_EN_STA);
	}

}

static int sunxi_get_rx_sync_mode(struct snd_kcontrol *kcontrol, struct snd_ctl_info *info)
{
	unsigned long value = 0;

	snd_print("\n");
	if (kcontrol->type != SND_CTL_ELEM_TYPE_ENUMERATED) {
		snd_err("invalid kcontrol type = %d.\n", kcontrol->type);
		return -EINVAL;
	}

	if (kcontrol->private_data_type == SND_MODULE_CODEC) {
		struct snd_codec *codec = kcontrol->private_data;
		struct sunxi_codec_info *sunxi_codec = codec->private_data;

		value = sunxi_codec->param.rx_sync_ctl;
	} else {
		snd_err("invalid kcontrol data type = %d.\n", kcontrol->private_data_type);
		return -EINVAL;
	}

	snd_kcontrol_to_snd_ctl_info(kcontrol, info, value);

	return 0;
}

static int sunxi_set_rx_sync_mode(struct snd_kcontrol *kcontrol, unsigned long value)
{
	struct snd_codec *codec = kcontrol->private_data;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	snd_print("\n");
	if (kcontrol->type != SND_CTL_ELEM_TYPE_ENUMERATED) {
		snd_err("invalid kcontrol type = %d.\n", kcontrol->type);
		return -EINVAL;
	}

	if (value >= kcontrol->items) {
		snd_err("invalid kcontrol items = %ld.\n", value);
		return -EINVAL;
	}

	if (kcontrol->private_data_type != SND_MODULE_CODEC) {
		snd_err("invalid kcontrol data type = %d.\n", kcontrol->private_data_type);
		return 0;
	}

	if (value) {
		if (param->rx_sync_ctl) {
			return 0;
		}
		param->rx_sync_ctl = true;
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC, 1 <<  RX_SYNC_EN, 1 << RX_SYNC_EN);
		sunxi_rx_sync_startup(param->rx_sync_domain, param->rx_sync_id, (void *)codec,
				      sunxi_rx_sync_enable);
	} else {
		if (!param->rx_sync_ctl) {
			return 0;
		}
		sunxi_rx_sync_shutdown(param->rx_sync_domain, param->rx_sync_id);
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC, 1 <<  RX_SYNC_EN, 0 << RX_SYNC_EN);
		param->rx_sync_ctl = false;
	}

	snd_info("mask:0x%x, items:%d, value:0x%x\n", kcontrol->mask, kcontrol->items, value);

	return 0;
}

static struct snd_kcontrol sunxi_codec_controls[] = {
	SND_CTL_ENUM((const char*)"tx hub mode",
		     ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
		     SUNXI_DAC_DPC, DAC_HUB_EN),
	SND_CTL_ENUM_EXT("rx sync mode",
			 ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			 SND_CTL_ENUM_AUTO_MASK,
			 sunxi_get_rx_sync_mode,
			 sunxi_set_rx_sync_mode),

	/* global digital volume */
	SND_CTL_KCONTROL_EXT_REG((const char*)"digital volume",
			SUNXI_DAC_DPC, DVOL, 0x3F,
			sunxi_get_data_invert, sunxi_set_data_invert),

	SND_CTL_ENUM((const char*)"LINEOUTL Output Select",
			ARRAY_SIZE(lineout_mux), lineout_mux,
			SUNXI_DAC_ANA_CTL, LINEOUTLDIFFEN),
	SND_CTL_ENUM((const char*)"LINEOUTR Output Select",
			ARRAY_SIZE(lineout_mux), lineout_mux,
			SUNXI_DAC_ANA_CTL, LINEOUTRDIFFEN),
	/* DAC digital volume */
	SND_CTL_ENUM((const char*)"DAC digital volume switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_DAC_VOL_CTL, DAC_VOL_SEL),
	SND_CTL_KCONTROL((const char*)"DACL digital volume",
			SUNXI_DAC_VOL_CTL, DAC_VOL_L, 0xFF),
	SND_CTL_KCONTROL((const char*)"DACR digital volume",
			SUNXI_DAC_VOL_CTL, DAC_VOL_R, 0xFF),

	/* analog volume */
	SND_CTL_KCONTROL((const char*)"MIC1 gain volume",
			SUNXI_ADC1_ANA_CTL, ADC_PGA_GAIN_CTL, 0x1F),
	SND_CTL_KCONTROL((const char*)"MIC2 gain volume",
			SUNXI_ADC2_ANA_CTL, ADC_PGA_GAIN_CTL, 0x1F),
	SND_CTL_KCONTROL((const char*)"MIC3 gain volume",
			SUNXI_ADC3_ANA_CTL, ADC_PGA_GAIN_CTL, 0x1F),
	SND_CTL_KCONTROL((const char*)"FM Left gain volume",
			SUNXI_ADC1_ANA_CTL, FMINL_GAIN, 0x1),
	SND_CTL_KCONTROL((const char*)"FM Right gain volume",
			SUNXI_ADC2_ANA_CTL, FMINR_GAIN, 0x1),
	SND_CTL_KCONTROL((const char*)"Line Right gain volume",
			SUNXI_ADC2_ANA_CTL, LINEINL_GAIN, 0x1),
	SND_CTL_KCONTROL((const char*)"Line Left gain volume",
			SUNXI_ADC2_ANA_CTL, LINEINR_GAIN, 0x1),
	SND_CTL_KCONTROL_EXT_REG((const char*)"HPOUT gain volume",
			SUNXI_HP_ANA_CTL, HP_GAIN, 0x7,
			sunxi_get_data_invert, sunxi_set_data_invert),
	SND_CTL_KCONTROL((const char*)"LINEOUT volume",
			SUNXI_DAC_ANA_CTL, LINEOUT_VOL, 0x1f),

	/* ADC digital volume */
	SND_CTL_ENUM((const char*)"ADC1_2 digital volume switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC_DIG_CTL, ADC1_2_VOL_EN),
	SND_CTL_ENUM((const char*)"ADC3 digital volume switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC_DIG_CTL, ADC3_VOL_EN),
	SND_CTL_KCONTROL((const char*)"ADC1 digital volume",
			SUNXI_ADC_VOL_CTL1, ADC1_VOL, 0xFF),
	SND_CTL_KCONTROL((const char*)"ADC2 digital volume",
			SUNXI_ADC_VOL_CTL1, ADC2_VOL, 0xFF),
	SND_CTL_KCONTROL((const char*)"ADC3 digital volume",
			SUNXI_ADC_VOL_CTL1, ADC3_VOL, 0xFF),

	/* mic input select */
	SND_CTL_ENUM((const char*)"MIC1 input select",
			ARRAY_SIZE(micin_select), micin_select,
			SUNXI_ADC1_ANA_CTL, MIC_SIN_EN),
	SND_CTL_ENUM((const char*)"MIC2 input select",
			ARRAY_SIZE(micin_select), micin_select,
			SUNXI_ADC2_ANA_CTL, MIC_SIN_EN),
	SND_CTL_ENUM((const char*)"MIC3 input select",
			ARRAY_SIZE(micin_select), micin_select,
			SUNXI_ADC3_ANA_CTL, MIC_SIN_EN),
	/* ADC1 */
	SND_CTL_ENUM((const char*)"MIC1 input switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC1_ANA_CTL, MIC_PGA_EN),
	SND_CTL_ENUM((const char*)"FM input L switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC1_ANA_CTL, FMINL_EN),
	SND_CTL_ENUM((const char*)"LINE input L switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC1_ANA_CTL, LINEINL_EN),
	/* ADC2 */
	SND_CTL_ENUM((const char*)"MIC2 input switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC2_ANA_CTL, MIC_PGA_EN),
	SND_CTL_ENUM((const char*)"FM input R switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC2_ANA_CTL, FMINR_EN),
	SND_CTL_ENUM((const char*)"LINE input R switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC2_ANA_CTL, LINEINR_EN),
	/* ADC3 */
	SND_CTL_ENUM((const char*)"MIC3 input switch",
			ARRAY_SIZE(sunxi_switch_text), sunxi_switch_text,
			SUNXI_ADC3_ANA_CTL, MIC_PGA_EN),

	SND_CTL_ENUM_EXT((const char *)"Playback audio route",
			ARRAY_SIZE(pb_audio_route_text), pb_audio_route_text,
			SND_CTL_ENUM_AUTO_MASK,
			sunxi_pb_audio_route_get_data, sunxi_pb_audio_route_set_data),
};

/* A-E 五时点诊断（2026-08-08 切歌无声排查）：在 codec_init 结束后、
 * 每次播放 dapm on 之前、dapm off 之后打点，对比首播 vs 切歌的
 * 模拟寄存器建立过程差异（A=init 后, B/D=第1/2次 on 前, C=off 后）。 */
static void sunxi_codec_out_dump(struct snd_codec *codec, const char *tag);

static void sunxi_codec_init(struct snd_codec *codec)
{
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;
	struct alg_cfg_reg_domain domain_1bdrc = {
		.reg_base = (void *)SUNXI_CODEC_BASE_ADDR,
		.reg_min = SUNXI_DAC_DAP_CTL,
		.reg_max = SUNXI_DAC_DRC_HPFLGAIN,
	};

	/* get chp version */
	sunxi_codec->chip_ver = hal_efuse_get_chip_ver();

	/* In order to ensure that the ADC sampling is normal,
	 * the A chip SOC needs to always open HPLDO and RMC_EN
	 */
	if (sunxi_codec->chip_ver == CHIP_VER_A) {
		snd_codec_update_bits(codec, SUNXI_POWER_ANA_CTL,
			(0x1<<HPLDO_EN), (0x1<<HPLDO_EN));
		snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
			(0x1<<RMCEN), (0x1<<RMCEN));
	} else {
		snd_codec_update_bits(codec, SUNXI_POWER_ANA_CTL,
			(0x1<<HPLDO_EN), (0x0<<HPLDO_EN));
		snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
			(0x1<<RMCEN), (0x0<<RMCEN));
	}

	/* Disable HPF(high passed filter) */
	snd_codec_update_bits(codec, SUNXI_DAC_DPC,
			(1 << HPF_EN), (0x0 << HPF_EN));

	if (param->rx_sync_en) {
		/* disabled ADCFDT */
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(0x1 << ADCDFEN), (0x0 << ADCDFEN));
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(0x3 << ADCFDT), (0x2 << ADCFDT));
		/* RX_SYNC_EN */
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC, 1 << RX_SYNC_EN, 0 << RX_SYNC_EN);
	} else {
		/* Enable ADCFDT to overcome niose at the beginning */
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(0x1 << ADCDFEN), (0x1 << ADCDFEN));
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(0x3 << ADCFDT), (0x2 << ADCFDT));
	}

	/* init the mic pga and vol params */
	snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
			0x1F << LINEOUT_VOL,
			param->lineout_vol << LINEOUT_VOL);
	snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
			0x7 << HP_GAIN,
			param->hpout_vol << HP_GAIN);

	/* DAC_VOL_SEL enable */
	snd_codec_update_bits(codec, SUNXI_DAC_VOL_CTL,
			(0x1 << DAC_VOL_SEL), (0x1 << DAC_VOL_SEL));
	snd_codec_update_bits(codec, SUNXI_DAC_DPC,
			0x3F << DVOL,
			param->digital_vol << DVOL);

	snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
			(0x1 << LINEOUTLDIFFEN) | (0x1 << LINEOUTRDIFFEN),
			(0x1 << LINEOUTLDIFFEN) | (0x1 << LINEOUTRDIFFEN));

	snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
			0x1F << ADC_PGA_GAIN_CTL,
			param->mic1gain << ADC_PGA_GAIN_CTL);

	snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
			0x1F << ADC_PGA_GAIN_CTL,
			param->mic2gain << ADC_PGA_GAIN_CTL);

	snd_codec_update_bits(codec, SUNXI_ADC3_ANA_CTL,
			0x1F << ADC_PGA_GAIN_CTL,
			param->mic3gain << ADC_PGA_GAIN_CTL);

#ifdef SUNXI_CODEC_DAP_ENABLE
	if (param->dacdrc_cfg || param->dachpf_cfg) {
		snd_codec_update_bits(codec, SUNXI_DAC_DAP_CTL,
				(0x1 << DDAP_EN), (0x1 << DDAP_EN));
	}

	if (param->adcdrc_cfg || param->adchpf_cfg) {
		snd_codec_update_bits(codec, SUNXI_ADC_DAP_CTL,
				(0x1 << ADC_DAP0_EN | 0x1 << ADC_DAP1_EN),
				(0x1 << ADC_DAP0_EN | 0x1 << ADC_DAP1_EN));
	}

	if (param->adcdrc_cfg) {
		adcdrc_config(codec);
		adcdrc_enable(codec, 1);
	}
	if (param->adchpf_cfg) {
		adchpf_config(codec);
		adchpf_enable(codec, 1);
	}
	if (param->dacdrc_cfg) {
		dacdrc_config(codec);
		dacdrc_enable(codec, 1);
	}
	if (param->dachpf_cfg) {
		dachpf_config(codec);
		dachpf_enable(codec, 1);
	}
#endif

	sunxi_alg_cfg_domain_set(&domain_1bdrc, SUNXI_ALG_CFG_DOMAIN_1BDRC);

	/* A-E 五时点诊断（2026-08-08）：A 点 = codec_init 结束后，
	 * 记录冷启动初始寄存器状态，作为 B/C/D/E 的对比基线。 */
	sunxi_codec_out_dump(codec, "INIT_DONE");
}

/* 诊断（2026-08-07 切歌无声排查）：打印模拟输出链路关键寄存器实值 +
 * 功放 GPIO 电平。仅挂在 open/prepare/trigger 级低频路径，绝不挂在
 * pointer 等高频查询上（上一轮日志洪水死机教训）。
 * 2026-08-23 降噪（review-8）：P73~P113 已闭环，默认关闭；排障时
 * 改 #if 0 → #if 1 重新启用。 */
static void sunxi_codec_out_dump(struct snd_codec *codec, const char *tag)
{
#if 0
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;
	gpio_data_t pa = 0;
	static uint32_t last_cnt = 0;
	uint32_t fifos, cnt, delta;

	if (param->gpio_spk > 0)
		hal_gpio_get_data(param->gpio_spk, &pa);
	fifos = snd_codec_read(codec, SUNXI_DAC_FIFOS);
	cnt = snd_codec_read(codec, SUNXI_DAC_CNT);
	/* DAC_CNT 为只读 TX 帧计数：trigger START/STOP 两次调用间的增量
	 * = 本次播放实际进入 DAC 的帧数。增量≈播放帧数 → 数字侧在消费，
	 * 无声锁定模拟/物理；增量≈0 → 数据根本没进 DAC（数字侧问题）。
	 * TX_EMPTY(bit23)=1 表示 DAC TX FIFO 为空（无数据在出）。 */
	delta = (cnt >= last_cnt) ? (cnt - last_cnt) : cnt;
	last_cnt = cnt;
	/* 切歌无声排查（2026-08-07）：增益/静音类寄存器此前从未 dump——
	 * DAC_VOL_CTL(0x04) 音量、DAC_DG(0x28) 数字增益、DAC_DAP_CTL(0xF0)、
	 * DAC_DRC_CTRL(0x108) 动态范围控制。若切歌路径把增益改静音/衰减，
	 * 数字层（writei/CNT）完全正常但实际无声，与全部日志现象吻合。 */
	printf("[codec] %s: DPC=0x%x DAC_ANA=0x%x HP_ANA=0x%x "
	       "DAC_REG=0x%x DAC_FIFOC=0x%x FIFOS=0x%x(TXE=%d) "
	       "CNT=%u(delta=%u) PA=%d VOL=0x%x DG=0x%x DAP=0x%x DRC=0x%x "
	       "RAMP=0x%x BIAS=0x%x POWER=0x%x\n",
	       tag,
	       snd_codec_read(codec, SUNXI_DAC_DPC),
	       snd_codec_read(codec, SUNXI_DAC_ANA_CTL),
	       snd_codec_read(codec, SUNXI_HP_ANA_CTL),
	       snd_codec_read(codec, SUNXI_DAC_REG),
	       snd_codec_read(codec, SUNXI_DAC_FIFOC),
	       fifos, (fifos >> TX_EMPTY) & 1,
	       cnt, delta,
	       pa,
	       snd_codec_read(codec, SUNXI_DAC_VOL_CTL),
	       snd_codec_read(codec, SUNXI_DAC_DG),
	       snd_codec_read(codec, SUNXI_DAC_DAP_CTL),
	       snd_codec_read(codec, SUNXI_DAC_DRC_CTRL),
	       snd_codec_read(codec, SUNXI_RAMP_ANA_CTL),
	       snd_codec_read(codec, SUNXI_BIAS_ANA_CTL),
	       snd_codec_read(codec, SUNXI_POWER_ANA_CTL));
#else
	(void)codec;
	(void)tag;
#endif
}

static void sunxi_codec_playback_hp_route(struct snd_codec *codec, int spk, int onoff)
{
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	if (onoff) {
		if (sunxi_codec->chip_ver == CHIP_VER_A) {
			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
					(0x1<<HPFB_BUF_EN) | (0x1<<HPFB_IN_EN),
					(0x1<<HPFB_BUF_EN) | (0x1<<HPFB_IN_EN));
			/* ramp out enable */
			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
					(0x1<<RAMP_OUT_EN) | (0x1<<RSWITCH),
					(0x1<<RAMP_OUT_EN) | (0x1<<RSWITCH));
			/* digital DAC enable */
			snd_codec_update_bits(codec, SUNXI_DAC_DPC,
					(0x1<<EN_DAC), (0x1<<EN_DAC));
			hal_msleep(5);
			/* analog DAC enable */
			snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
					(0x1<<DACLEN) | (0x1<<DACREN),
					(0x1<<DACLEN) | (0x1<<DACREN));
			/* HEADPONEOUT */
			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
				(0x1<<HP_DRVEN) | (0x1<<HP_DRVOUTEN),
				(0x1<<HP_DRVEN) | (0x1<<HP_DRVOUTEN));
			/* Playback on */
			snd_codec_update_bits(codec, SUNXI_POWER_ANA_CTL,
				(0x1<<HPLDO_EN), (0x1<<HPLDO_EN));
		} else {
			/* digital DAC enable */
			snd_codec_update_bits(codec, SUNXI_DAC_DPC,
					(0x1<<EN_DAC), (0x1<<EN_DAC));
			snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
					(0x1<<RDEN), (0x1<<RDEN));
		}
		if (spk) {
			hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
			hal_gpio_set_data(param->gpio_spk, param->pa_level);
			hal_msleep(param->pa_msleep_time);
		}
	} else {
		/* Playback off */
		if (sunxi_codec->chip_ver == CHIP_VER_A) {
			if (spk) {
				hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
				hal_gpio_set_data(param->gpio_spk, !param->pa_level);
			}
			/* HEADPONE_EN */
			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
					(0x1<<HP_DRVEN), (0x0<<HP_DRVEN));
			/* power off */
			/* note: HPLDO is always open to avoid KEY_ADC sampling fail */
			/* snd_codec_update_bits(codec, SUNXI_POWER_ANA_CTL, */
			/* 		(0x1<<HPLDO_EN), (0x0<<HPLDO_EN)); */
			hal_msleep(30);
			/* HEADPONE_OUT */
			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
					(0x1<<HP_DRVOUTEN), (0x0<<HP_DRVOUTEN));
			/* analog DAC */
			snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
					(0x1<<DACLEN) | (0x1<<DACREN),
					(0x0<<DACLEN) | (0x0<<DACREN));
			/* digital DAC */
			snd_codec_update_bits(codec, SUNXI_DAC_DPC,
					(0x1<<EN_DAC), (0x0<<EN_DAC));

			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
					(0x1<<RAMP_OUT_EN) | (0x1<<RSWITCH),
					(0x0<<RAMP_OUT_EN) | (0x0<<RSWITCH));

			snd_codec_update_bits(codec, SUNXI_HP_ANA_CTL,
					(0x1<<HPFB_BUF_EN) | (0x1<<HPFB_IN_EN),
					(0x0<<HPFB_BUF_EN) | (0x0<<HPFB_IN_EN));
		} else {
			/* 2026-08-31 pop 修复（P140+P141）：先静音 → 延时 → 再关 PA/DAC。
			 * P140 只调 DACLMUTE 与 DACLEN 的顺序；P141 补充：PA（功放 GPIO）
			 * 关断也必须在静音之后——否则功放掉电瞬间 DAC 仍在输出 → POP。 */
			snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
					(0x1<<DACLMUTE) | (0x1<<DACRMUTE),
					(0x0<<DACLMUTE) | (0x0<<DACRMUTE));
			hal_msleep(5);
			if (spk) {
				hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
				hal_gpio_set_data(param->gpio_spk, !param->pa_level);
			}
			/* analog DAC */
			snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
					(0x1<<DACLEN) | (0x1<<DACREN),
					(0x0<<DACLEN) | (0x0<<DACREN));
			/* digital DAC enable */
			snd_codec_update_bits(codec, SUNXI_DAC_DPC,
					(0x1<<EN_DAC), (0x0<<EN_DAC));
			snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
					(0x1<<RDEN), (0x0<<RDEN));
		}
	}
}

static void sunxi_codec_playback_lineout_route(struct snd_codec *codec, int spk, int onoff)
{
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	if (onoff) {
		/* digital DAC enable */
		snd_codec_update_bits(codec, SUNXI_DAC_DPC,
				(0x1<<EN_DAC), (0x1<<EN_DAC));
		hal_msleep(5);
		/* analog DAC enable */
		snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
				(0x1<<DACLEN) | (0x1<<DACREN),
				(0x1<<DACLEN) | (0x1<<DACREN));
		snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
				(0x1<<DACLMUTE) | (0x1<<DACRMUTE),
				(0x1<<DACLMUTE) | (0x1<<DACRMUTE));
		if (sunxi_codec->chip_ver != CHIP_VER_A) {
			uint32_t reg_val;
			/* Patch 2（2026-08-08 手册 0x31C bit30）：RAMP_RISE_INT 是
			 * R/W1C 中断位——写 1 清除。ON 前清掉上一次 Ramp Rise 完成
			 * 的 pending 中断，避免旧状态干扰本次 bit30 判断。 */
			snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
					0x1<<30, 0x1<<30);
			reg_val = snd_codec_read(codec, SUNXI_RAMP_ANA_CTL);
			if(!(reg_val & (0x1 << RDEN)))
				snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
						0x1<<RDEN, 0x1<<RDEN);
		}
		snd_codec_update_bits(codec, SUNXI_DAC_REG,
				(0x1<<LINEOUTL_EN) | (0x1<<LINEOUTR_EN),
				(0x1<<LINEOUTL_EN) | (0x1<<LINEOUTR_EN));
		if (spk) {
			hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
			hal_gpio_set_data(param->gpio_spk, param->pa_level);
			hal_msleep(param->pa_msleep_time);
		}
	} else {
		/* 2026-08-31 pop 修复（P140+P141）：先静音 → 延时 → 再关 PA/DAC。
		 * P140 只调 DACLMUTE 与 DACLEN 的顺序；P141 补充：PA（功放 GPIO）
		 * 关断也必须在静音之后——否则功放掉电瞬间 DAC 仍在输出 → POP。 */
		snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
				(0x1<<DACLMUTE) | (0x1<<DACRMUTE),
				(0x0<<DACLMUTE) | (0x0<<DACRMUTE));
		hal_msleep(5);
		if (spk) {
			hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
			hal_gpio_set_data(param->gpio_spk, !param->pa_level);
		}
		/* analog DAC */
		snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
				(0x1<<DACLEN) | (0x1<<DACREN),
				(0x0<<DACLEN) | (0x0<<DACREN));
		/* digital DAC enable */
		snd_codec_update_bits(codec, SUNXI_DAC_DPC,
				(0x1<<EN_DAC), (0x0<<EN_DAC));
		snd_codec_update_bits(codec, SUNXI_DAC_REG,
				(0x1<<LINEOUTL_EN) | (0x1<<LINEOUTR_EN),
				(0x0<<LINEOUTL_EN) | (0x0<<LINEOUTR_EN));
		if (sunxi_codec->chip_ver != CHIP_VER_A) {
			uint32_t reg_val;
			reg_val = snd_codec_read(codec, SUNXI_RAMP_ANA_CTL);
			/* 切歌无声修复（2026-08-08）：上游此分支条件写反——
			 * if(!(reg_val & RDEN)) 只在 RDEN 已为 0 时才写 0，等于永不清
			 * 除 RAMP 使能 → 首播 close 后 RDEN 残留 1，切歌第二次 open
			 * 时 RAMP 状态机认为偏置已建立而跳过逐级建立 → DAC 模拟输出
			 * 为 0 → 无声（与"首播有声、切歌无声、寄存器全一致"完全吻合）。
			 * 修正为 RDEN 置位时才清零（ON 分支首播路径保持不动）。 */
			if(reg_val & (0x1 << RDEN))
				snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
						0x1<<RDEN, 0x0<<RDEN);
			/* Patch 1（2026-08-08 手册 0x31C）：切歌无声排查——首播
			 * RAMP=0x40180001（Rise 完成）而切歌 0x180001（从未 Rise）。
			 * 手册只证明 RD_EN=1 触发 Rise 流程，未证明 RD_EN=0 会让
			 * Ramp FSM 回到初始态。为确保下次播放重新 Rise，close 时
			 * 显式复位：先清旧 Rise/Fall pending 中断（bit30/bit28
			 * 是 R/W1C，写 1 清除，避免旧 pending 与新 Rise 状态混杂），
			 * 再对 Ramp FSM 软复位（bit24 置 1 → 延时 → 清 0）。
			 * RD_EN disable does not guarantee Ramp FSM returns to
			 * initial state; reset it explicitly before next playback. */
			snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
					0x1<<30 | 0x1<<28, 0x1<<30 | 0x1<<28);
			snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
					0x1<<24, 0x1<<24);
			hal_udelay(10);
			snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL,
					0x1<<24, 0x0<<24);
		}
	}

}

static int sunxi_codec_dapm_control(struct snd_pcm_substream *substream,
				struct snd_dai *dai, int onoff)
{
	struct snd_codec *codec = dai->component;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;
	/* A-E 五时点诊断（2026-08-08）：播放 open/close 序号，区分
	 * 首播（#1）与切歌（#2）的 dapm on/off 打点。
	 * 2026-08-23 降噪（review-8）：打点 #if 0 关闭，序号变量一并关闭。 */
#if 0
	static int dapm_on_seq = 0;
	static int dapm_off_seq = 0;
#endif

	if (substream->dapm_state == onoff)
		return 0;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/*
		 * Playback:
		 * Playback --> DAC --> DAC_DIFFER --> LINEOUT/HPOUT + (External Speaker)
		 */
		/* A-E 五时点诊断（2026-08-08）：B/D 点 = 第 1/2 次播放
		 * dapm on 执行前，记录 route 建立前的寄存器基线（首播 vs
		 * 切歌对比点）。route 函数（lineout/hp_route）会改这些寄存器。
		 * 2026-08-23 降噪（review-8）：已闭环默认关闭，排障改 #if 1。 */
	#if 0
		 if (onoff) {
		  dapm_on_seq++;
		  printf("[codec] ===== PRE_ON#%d (第%d次播放 dapm on 前) =====\n",
		         dapm_on_seq, dapm_on_seq);
		  sunxi_codec_out_dump(codec, "PRE_ON");
		 } else {
		  dapm_off_seq++;
		 }
	#endif

		switch (param->pb_audio_route) {
		case PB_AUDIO_ROUTE_LINEOUT_SPK:
			sunxi_codec_playback_lineout_route(codec, 1, onoff);
			break;
		case PB_AUDIO_ROUTE_HEADPHONE_SPK:
			sunxi_codec_playback_hp_route(codec, 1, onoff);
			break;
		case PB_AUDIO_ROUTE_LINEOUT:
			sunxi_codec_playback_lineout_route(codec, 0, onoff);
			break;
		case PB_AUDIO_ROUTE_HEADPHONE:
			sunxi_codec_playback_hp_route(codec, 0, onoff);
			break;
		case PB_AUDIO_ROUTE_LO_HP:
			sunxi_codec_playback_lineout_route(codec, 0, onoff);
			sunxi_codec_playback_hp_route(codec, 0, onoff);
			break;
		case PB_AUDIO_ROUTE_LO_HP_SPK:
			sunxi_codec_playback_lineout_route(codec, onoff ? 0 : 1, onoff);
			sunxi_codec_playback_hp_route(codec, onoff ? 1 : 0, onoff);
			break;
		}
	} else {
		/*
		 * Capture:
		 * Capture1 <-- ADC1 <-- Input Mixer <-- LINEINL PGA <-- LINEINL
		 * Capture1 <-- ADC1 <-- Input Mixer <-- FMINL PGA <-- FMINL
		 *
		 * Capture2 <-- ADC2 <-- Input Mixer <-- LINEINR PGA <-- LINEINR
		 * Capture2 <-- ADC2 <-- Input Mixer <-- FMINR PGA <-- FMINR
		 *
		 * Capture3 <-- ADC3 <-- Input Mixer <-- MIC3 PGA <-- MIC3
		 */
		unsigned int channels = 0;
		channels = substream->runtime->channels;

		snd_print("channels = %u\n", channels);
		snd_print("adc flag:%d,%d,%d\n",
			param->adc1_flag, param->adc2_flag, param->adc3_flag);
		if (onoff) {
			if ((param->adc1_flag & ADC_AUDIO_ROUTE_MIC) ||
				(param->adc2_flag & ADC_AUDIO_ROUTE_MIC) ||
				(param->adc3_flag & ADC_AUDIO_ROUTE_MIC)) {
				snd_codec_update_bits(codec, SUNXI_MICBIAS_REG,
						      0x1<<MMICBIASEN,
						      0x1<<MMICBIASEN);
			}
			/* Capture on */
			hal_msleep(100);
			/* digital ADC enable */
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
					(0x1<<EN_AD), (0x1<<EN_AD));

			if (channels > 3 || channels < 1) {
				snd_err("unknown channels:%u\n", channels);
				return -EINVAL;
			}

			if (param->adc1_flag == ADC_AUDIO_ROUTE_MIC) {
				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1<<ADC_EN,
						      0x1<<ADC_EN);
			} else if (param->adc1_flag == ADC_AUDIO_ROUTE_LINEIN ||
					param->adc1_flag == ADC_AUDIO_ROUTE_FMIN) {

				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1F<<ADC_PGA_GAIN_CTL,
						      0x00<<ADC_PGA_GAIN_CTL);

				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1<<MIC_SIN_EN,
						      0x1<<MIC_SIN_EN);

				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1<<ADC_EN,
						      0x1<<ADC_EN);
			}

			if (param->adc2_flag == ADC_AUDIO_ROUTE_MIC) {
				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1<<ADC_EN,
						      0x1<<ADC_EN);
			} else if (param->adc2_flag == ADC_AUDIO_ROUTE_LINEIN ||
					param->adc2_flag == ADC_AUDIO_ROUTE_FMIN) {

				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1F<<ADC_PGA_GAIN_CTL,
						      0x00<<ADC_PGA_GAIN_CTL);

				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1<<MIC_SIN_EN,
						      0x1<<MIC_SIN_EN);

				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1<<ADC_EN,
						      0x1<<ADC_EN);
			}

			if (param->adc3_flag) {
				snd_codec_update_bits(codec, SUNXI_ADC3_ANA_CTL,
						      0x1<<ADC_EN,
						      0x1<<ADC_EN);
			}
		} else {
			/* Capture off */
			if (channels > 3 || channels < 1) {
				snd_err("unknown channels:%u\n", channels);
				return -EINVAL;
			}

			/* 2026-09-11 pop 修复（对齐 P140 DAC 关断顺序）：原顺序在此处
			 * 先断 MICBIAS，随后才关 ADC——偏置已失而 ADC 仍在采样，
			 * 输入瞬跳产生"麦克风关闭 POP/CLICK"。改为先关 ADC 通路、
			 * 延时后再断 MICBIAS（见本分支末尾）。 */

			if (param->adc1_flag == ADC_AUDIO_ROUTE_MIC) {
				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1<<ADC_EN,
						      0x0<<ADC_EN);
			} else if (param->adc1_flag == ADC_AUDIO_ROUTE_LINEIN ||
					param->adc1_flag == ADC_AUDIO_ROUTE_FMIN) {

				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1F<<ADC_PGA_GAIN_CTL,
						      param->mic1gain<<ADC_PGA_GAIN_CTL);

				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1<<MIC_SIN_EN,
						      0x0<<MIC_SIN_EN);

				snd_codec_update_bits(codec, SUNXI_ADC1_ANA_CTL,
						      0x1<<ADC_EN,
						      0x0<<ADC_EN);
			}

			if (param->adc2_flag == ADC_AUDIO_ROUTE_MIC) {
				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1<<ADC_EN,
						      0x0<<ADC_EN);
			} else if (param->adc2_flag == ADC_AUDIO_ROUTE_LINEIN ||
					param->adc2_flag == ADC_AUDIO_ROUTE_FMIN) {

				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1F<<ADC_PGA_GAIN_CTL,
						      param->mic2gain<<ADC_PGA_GAIN_CTL);

				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1<<MIC_SIN_EN,
						      0x0<<MIC_SIN_EN);

				snd_codec_update_bits(codec, SUNXI_ADC2_ANA_CTL,
						      0x1<<ADC_EN,
						      0x0<<ADC_EN);
			}

			if (param->adc3_flag) {
				snd_codec_update_bits(codec, SUNXI_ADC3_ANA_CTL,
						      0x1<<ADC_EN,
						      0x0<<ADC_EN);
			}
			/* 2026-09-11 pop 修复（P140 同款思路）：ADC 通路已关，
			 * 等模拟输出归零后再断 MICBIAS 与数字 ADC，避免采样瞬跳。 */
			hal_msleep(5);
			if ((param->adc1_flag & ADC_AUDIO_ROUTE_MIC) ||
				(param->adc2_flag & ADC_AUDIO_ROUTE_MIC) ||
				(param->adc3_flag & ADC_AUDIO_ROUTE_MIC)) {
				snd_codec_update_bits(codec, SUNXI_MICBIAS_REG,
						      0x1<<MMICBIASEN,
						      0x0<<MMICBIASEN);
			}
			/* digital ADC enable */
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
					(0x1<<EN_AD), (0x0<<EN_AD));
		}
	}
	/* A-E 五时点诊断（2026-08-08）：C 点 = dapm off 之后（POST_OFF），
	 * E 点 = dapm on 之后（POST_ON，播放已开始）。带播放序号，首播 #1
	 * vs 切歌 #2 直接对比。
	 * 2026-08-23 降噪（review-8）：已闭环默认关闭，排障改 #if 1。 */
#if 0
	sunxi_codec_out_dump(codec, onoff ? "dapm on" : "dapm off");
	printf("[codec] ===== %s#%d (第%d次播放 dapm %s 后) =====\n",
	       onoff ? "POST_ON" : "POST_OFF",
	       onoff ? dapm_on_seq : dapm_off_seq,
	       onoff ? dapm_on_seq : dapm_off_seq,
	       onoff ? "on" : "off");
	sunxi_codec_out_dump(codec, onoff ? "POST_ON" : "POST_OFF");
#endif
	substream->dapm_state = onoff;
	return 0;
}

static int sunxi_codec_startup(struct snd_pcm_substream *substream,
		struct snd_dai *dai)
{
	snd_print("\n");

	return 0;
}


static int sunxi_codec_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_dai *dai)
{
	struct snd_codec *codec = dai->component;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *codec_param = &sunxi_codec->param;
	int i = 0;

	snd_print("\n");
	switch (params_format(params)) {
	case	SND_PCM_FORMAT_S16_LE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
				(3 << FIFO_MODE), (3 << FIFO_MODE));
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
				(1 << TX_SAMPLE_BITS), (0 << TX_SAMPLE_BITS));
		} else {
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << RX_FIFO_MODE), (1 << RX_FIFO_MODE));
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << RX_SAMPLE_BITS), (0 << RX_SAMPLE_BITS));
		}
		break;
	case	SND_PCM_FORMAT_S24_LE:
	case	SND_PCM_FORMAT_S32_LE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
				(3 << FIFO_MODE), (0 << FIFO_MODE));
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
				(1 << TX_SAMPLE_BITS), (1 << TX_SAMPLE_BITS));
		} else {
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << RX_FIFO_MODE), (0 << RX_FIFO_MODE));
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << RX_SAMPLE_BITS), (1 << RX_SAMPLE_BITS));
		}
		break;
	default:
		break;
	}

	for (i = 0; i < ARRAY_SIZE(sample_rate_conv); i++) {
		if (sample_rate_conv[i].samplerate == params_rate(params)) {
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
				snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
					(0x7 << DAC_FS),
					(sample_rate_conv[i].rate_bit << DAC_FS));
			} else {
				if (sample_rate_conv[i].samplerate > 48000)
					return -EINVAL;
				snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
					(0x7 << ADC_FS),
					(sample_rate_conv[i].rate_bit<<ADC_FS));
			}
		}
	}

	/* reset the adchpf func setting for different sampling */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (codec_param->adchpf_cfg) {
			if (params_rate(params) == 16000) {

				snd_codec_write(codec, SUNXI_ADC_DRC_HHPFC,
						(0x00F623A5 >> 16) & 0xFFFF);

				snd_codec_write(codec, SUNXI_ADC_DRC_LHPFC,
							0x00F623A5 & 0xFFFF);

			} else if (params_rate(params) == 44100) {

				snd_codec_write(codec, SUNXI_ADC_DRC_HHPFC,
						(0x00FC60DB >> 16) & 0xFFFF);

				snd_codec_write(codec, SUNXI_ADC_DRC_LHPFC,
							0x00FC60DB & 0xFFFF);
			} else {
				snd_codec_write(codec, SUNXI_ADC_DRC_HHPFC,
						(0x00FCABB3 >> 16) & 0xFFFF);

				snd_codec_write(codec, SUNXI_ADC_DRC_LHPFC,
							0x00FCABB3 & 0xFFFF);
			}
		}
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		switch (params_channels(params)) {
		case 1:
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
					(1<<DAC_MONO_EN), 1<<DAC_MONO_EN);
			break;
		case 2:
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
					(1<<DAC_MONO_EN), (0<<DAC_MONO_EN));
			break;
		default:
			snd_err("cannot support the channels:%u.\n",
				params_channels(params));
			return -EINVAL;
		}
	} else {
		if (sunxi_get_adc_ch(codec) < 0) {
			snd_err("capture only support 1~3 channel\n");
			return -EINVAL;
		}
		if (codec_param->adc1_flag)
			snd_codec_update_bits(codec, SUNXI_ADC_DIG_CTL,
					      0x1<<ADC1_CHANNEL_EN,
					      0x1<<ADC1_CHANNEL_EN);
		else
			snd_codec_update_bits(codec, SUNXI_ADC_DIG_CTL,
					      0x1<<ADC1_CHANNEL_EN,
					      0x0<<ADC1_CHANNEL_EN);
		if (codec_param->adc2_flag)
			snd_codec_update_bits(codec, SUNXI_ADC_DIG_CTL,
					      0x1<<ADC2_CHANNEL_EN,
					      0x1<<ADC2_CHANNEL_EN);
		else
			snd_codec_update_bits(codec, SUNXI_ADC_DIG_CTL,
					      0x1<<ADC2_CHANNEL_EN,
					      0x0<<ADC2_CHANNEL_EN);
		if (codec_param->adc3_flag)
			snd_codec_update_bits(codec, SUNXI_ADC_DIG_CTL,
					      0x1<<ADC3_CHANNEL_EN,
					      0x1<<ADC3_CHANNEL_EN);
		else
			snd_codec_update_bits(codec, SUNXI_ADC_DIG_CTL,
					      0x1<<ADC3_CHANNEL_EN,
					      0x0<<ADC3_CHANNEL_EN);
	}

	return 0;
}

static int sunxi_codec_set_sysclk(struct snd_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_codec *codec = dai->component;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK) {
		hal_clock_disable(sunxi_codec->moduleclk);
		if (freq == 24576000) {
			hal_clk_set_parent(sunxi_codec->moduleclk, sunxi_codec->pllclk1);
		} else {
			hal_clk_set_parent(sunxi_codec->moduleclk, sunxi_codec->pllclk);
		}
		hal_clk_set_rate(sunxi_codec->moduleclk, freq);
		hal_msleep(50);
		hal_clock_enable(sunxi_codec->moduleclk);
	}

	if (dir == SNDRV_PCM_STREAM_CAPTURE) {
		if (freq == 24576000) {
			hal_clk_set_parent(sunxi_codec->moduleclk1, sunxi_codec->pllclk1);
		} else {
			hal_clk_set_parent(sunxi_codec->moduleclk1, sunxi_codec->pllclk);
		}
		hal_clk_set_rate(sunxi_codec->moduleclk1, freq);
	}

	return 0;
}

static void sunxi_codec_shutdown(struct snd_pcm_substream *substream, struct snd_dai *dai)
{
	return ;
}

static int sunxi_codec_prepare(struct snd_pcm_substream *substream,
		struct snd_dai *dai)
{
	struct snd_codec *codec = dai->component;

	snd_print("\n");

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
			(1 << FIFO_FLUSH), (1 << FIFO_FLUSH));
		snd_codec_write(codec, SUNXI_DAC_FIFOS,
			(1 << DAC_TXE_INT | 1 << DAC_TXU_INT | 1 << DAC_TXO_INT));
		snd_codec_write(codec, SUNXI_DAC_CNT, 0);
	} else {
		snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << ADC_FIFO_FLUSH), (1 << ADC_FIFO_FLUSH));
		snd_codec_write(codec, SUNXI_ADC_FIFOS,
				(1 << ADC_RXA_INT | 1 << ADC_RXO_INT));
		snd_codec_write(codec, SUNXI_ADC_CNT, 0);
	}

	return 0;
}

static int sunxi_codec_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_dai *dai)
{
	struct snd_codec *codec = dai->component;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	const char *cmd_name;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		cmd_name = "START";
		break;
	case SNDRV_PCM_TRIGGER_RESUME:
		cmd_name = "RESUME";
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		cmd_name = "PAUSE_RELEASE";
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		cmd_name = "STOP";
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		cmd_name = "SUSPEND";
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		cmd_name = "PAUSE_PUSH";
		break;
	default:
		cmd_name = "UNKNOWN";
		break;
	}

	snd_print("\n");
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
				(1 << DAC_DRQ_EN), (1 << DAC_DRQ_EN));
		}
		else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << ADC_DRQ_EN), (1 << ADC_DRQ_EN));
			if (sunxi_codec->param.rx_sync_en && sunxi_codec->param.rx_sync_ctl)
				sunxi_rx_sync_control(sunxi_codec->param.rx_sync_domain,
						      sunxi_codec->param.rx_sync_id, 1);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			snd_codec_update_bits(codec, SUNXI_DAC_FIFOC,
				(1 << DAC_DRQ_EN), (0 << DAC_DRQ_EN));
		}
		else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
				(1 << ADC_DRQ_EN), (0 << ADC_DRQ_EN));
			if (sunxi_codec->param.rx_sync_en && sunxi_codec->param.rx_sync_ctl)
				sunxi_rx_sync_control(sunxi_codec->param.rx_sync_domain,
						      sunxi_codec->param.rx_sync_id, 0);
		}
		break;
	default:
		return -EINVAL;
	}

	/* 切歌无声排查（2026-08-07 证据点③）：带命令名打点，让暂停续播
	 * （PAUSE_PUSH→PAUSE_RELEASE）与切歌（STOP→…→START）的 trigger
	 * 序列在日志里可直接对比，配合 dapm on/off + PA + FIFOS/CNT
	 * 构成四打点，一次刷机拿到两种操作的完整差异。 */
	sunxi_codec_out_dump(codec, cmd_name);
	return 0;
}

static struct snd_dai_ops sun8iw20_codec_dai_ops = {
	.startup	= sunxi_codec_startup,
	.hw_params	= sunxi_codec_hw_params,
	.shutdown	= sunxi_codec_shutdown,
	.set_sysclk	= sunxi_codec_set_sysclk,
	.trigger	= sunxi_codec_trigger,
	.prepare	= sunxi_codec_prepare,
	.dapm_control   = sunxi_codec_dapm_control,
};

static struct snd_dai sun8iw20_codec_dai[] = {
	{
		.name	= "sun8iw20codec",
		.playback = {
			.stream_name = "Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates	= SNDRV_PCM_RATE_8000_192000
				| SNDRV_PCM_RATE_KNOT,
			.formats = SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE,
			.rate_min       = 8000,
			.rate_max       = 192000,
		},
		.capture = {
			.stream_name = "Capture",
			.channels_min = 1,
			.channels_max = 3,
			.rates = SNDRV_PCM_RATE_8000_48000
				| SNDRV_PCM_RATE_KNOT,
			.formats = SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE,
			.rate_min       = 8000,
			.rate_max       = 48000,
		},
		.ops = &sun8iw20_codec_dai_ops,
	},
};

static void sunxi_jack_detect(void *param)
{
	struct sunxi_codec_info *sunxi_codec = param;
	struct sunxi_codec_param *codec_param = &sunxi_codec->param;
	gpio_data_t gpio_status;

	if (codec_param->gpio_spk > 0) {
		hal_gpio_set_direction(codec_param->gpio_spk, GPIO_MUXSEL_OUT);
		hal_gpio_get_data(SUNXI_CODEC_JACK_DET, &gpio_status);
		if (gpio_status) {
			/* jack plug out */
			hal_gpio_set_data(codec_param->gpio_spk, codec_param->pa_level);
		} else {
			/* jack plug in */
			hal_gpio_set_data(codec_param->gpio_spk, !codec_param->pa_level);
		}
	}
}

static hal_irqreturn_t sunxi_jack_irq_handler(void *ptr)
{
	struct sunxi_codec_info *sunxi_codec = ptr;

	hal_timer_set_oneshot(SUNXI_TMR0, 300000, sunxi_jack_detect, sunxi_codec);

	return 0;
}

static int sun8iw20_codec_probe(struct snd_codec *codec)
{
	struct sunxi_codec_info *sunxi_codec = NULL;
	struct reset_control *reset;
	hal_clk_status_t ret;

	if (!codec->codec_dai)
		return -1;

	sunxi_codec = snd_malloc(sizeof(struct sunxi_codec_info));
	if (!sunxi_codec) {
		snd_err("no memory\n");
		return -ENOMEM;
	}

	codec->private_data = (void *)sunxi_codec;

	snd_print("codec para init\n");
	/* get codec para from board config? */
	sunxi_codec->param = default_param;

	if (sunxi_codec->param.rx_sync_en) {
		sunxi_codec->param.rx_sync_domain = RX_SYNC_SYS_DOMAIN;
		sunxi_codec->param.rx_sync_id =
			sunxi_rx_sync_probe(sunxi_codec->param.rx_sync_domain);
		if (sunxi_codec->param.rx_sync_id < 0) {
			snd_err("sunxi_rx_sync_probe failed");
			return -EINVAL;
		}
		snd_info("sunxi_rx_sync_probe successful. domain=%d, id=%d",
			 sunxi_codec->param.rx_sync_domain,
			 sunxi_codec->param.rx_sync_id);
	}

	codec->codec_base_addr = (void *)SUNXI_CODEC_BASE_ADDR;
	codec->codec_dai->component = codec;

	reset = hal_reset_control_get(HAL_SUNXI_RESET, RST_BUS_AUDIO_CODEC);
	hal_reset_control_deassert(reset);
	hal_reset_control_put(reset);

	sunxi_codec->busclk = hal_clock_get(HAL_SUNXI_CCU, CLK_BUS_AUDIO_CODEC);
	sunxi_codec->moduleclk = hal_clock_get(HAL_SUNXI_CCU, CLK_AUDIO_DAC);
	sunxi_codec->moduleclk1 = hal_clock_get(HAL_SUNXI_CCU, CLK_AUDIO_ADC);
	sunxi_codec->pllclk = hal_clock_get(HAL_SUNXI_CCU, CLK_PLL_AUDIO0);
	sunxi_codec->pllclk1 = hal_clock_get(HAL_SUNXI_CCU, CLK_PLL_AUDIO1_DIV5);

	hal_clk_set_parent(sunxi_codec->moduleclk, sunxi_codec->pllclk);
	hal_clk_set_parent(sunxi_codec->moduleclk1, sunxi_codec->pllclk);

	hal_clk_set_rate(sunxi_codec->pllclk, 22579200);
	hal_clk_set_rate(sunxi_codec->pllclk1, 614400000);

	ret = hal_clock_enable(sunxi_codec->pllclk);
	if (ret != HAL_CLK_STATUS_OK)
	    snd_err("pllclk clock enable failed.\n");
	ret = hal_clock_enable(sunxi_codec->pllclk1);
	if (ret != HAL_CLK_STATUS_OK)
	    snd_err("pllclk1 clock enable failed.\n");
	ret = hal_clock_enable(sunxi_codec->busclk);
	if (ret != HAL_CLK_STATUS_OK)
	    snd_err("busclk clock enable failed.\n");
	ret = hal_clock_enable(sunxi_codec->moduleclk);
	if (ret != HAL_CLK_STATUS_OK)
	    snd_err("moduleclk clock enable failed.\n");
	ret = hal_clock_enable(sunxi_codec->moduleclk1);
	if (ret != HAL_CLK_STATUS_OK)
	    snd_err("moduleclk1 clock enable failed.\n");

	sunxi_codec_init(codec);
#ifdef SUNXI_ADC_DAUDIO_SYNC
	adc_daudio_sync_codec = codec;
#endif

	if (sunxi_codec->param.hp_detect_used) {
		hal_timer_init(SUNXI_TMR0);
		if (hal_gpio_to_irq(SUNXI_CODEC_JACK_DET, &sunxi_codec->irq) < 0)
			snd_err("sunxi jack gpio to irq err.\n");
		if (hal_gpio_irq_request(sunxi_codec->irq, sunxi_jack_irq_handler, IRQ_TYPE_EDGE_FALLING, sunxi_codec) < 0)
			snd_err("sunxi jack irq request err.\n");
		sunxi_jack_detect(sunxi_codec);
		if (hal_gpio_irq_enable(sunxi_codec->irq) < 0)
			snd_err("sunxi jack irq enable err.\n");
	}

	return 0;
}

static int sun8iw20_codec_remove(struct snd_codec *codec)
{
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	if (sunxi_codec->param.rx_sync_en) {
		sunxi_rx_sync_remove(sunxi_codec->param.rx_sync_domain);
	}

	if (param->adcdrc_cfg)
		adcdrc_enable(codec, 0);
	if (param->adchpf_cfg)
		adchpf_enable(codec, 0);
	if (param->dacdrc_cfg)
		dacdrc_enable(codec, 0);
	if (param->dachpf_cfg)
		dachpf_enable(codec, 0);
	hal_clock_disable(sunxi_codec->moduleclk);
	hal_clock_disable(sunxi_codec->moduleclk1);
	hal_clock_disable(sunxi_codec->pllclk);
	hal_clock_disable(sunxi_codec->pllclk1);

	hal_gpio_irq_disable(sunxi_codec->irq);
	hal_gpio_irq_free(sunxi_codec->irq);
	if(sunxi_codec->param.hp_detect_used){
		hal_timer_uninit(SUNXI_TMR0);
	}
	snd_free(sunxi_codec);
	codec->private_data = NULL;

	return 0;
}

struct snd_codec sunxi_audiocodec = {
	.name		= "audiocodec",
	.codec_dai	= sun8iw20_codec_dai,
	.codec_dai_num  = ARRAY_SIZE(sun8iw20_codec_dai),
	.private_data	= NULL,
	.probe		= sun8iw20_codec_probe,
	.remove		= sun8iw20_codec_remove,
	.controls       = sunxi_codec_controls,
	.num_controls   = ARRAY_SIZE(sunxi_codec_controls),
};
