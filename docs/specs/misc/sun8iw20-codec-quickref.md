# R528 sun8iw20 codec 源码速查（第二次 open 无声问题检查用）

> 工程：openvela (R528) — nsh 配置
> 文件：`vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c`（当前基线 ok-20260808-8，即 -5 状态）
> 头文件：`sun8iw20-codec.h`（同目录）
> 说明：本文件只放源码与写点索引，不含分析。行号对应 sun8iw20-codec.c 当前版本。

---

## 1. sunxi_codec_init()（行 695-805）完整代码

```c
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
}
```

### 1.1 关注的寄存器初始化位置标注

| 寄存器 | init 中位置 | 操作 |
|--------|------------|------|
| **POWER_ANA_CTL** | 行 712-713（chip_ver==A）/ 717-718（else） | 仅置/清 HPLDO_EN(bit30) |
| **RAMP_ANA_CTL** | 行 714-715（chip_ver==A）/ 719-720（else） | 仅置/清 RMCEN(bit1) |
| **BIAS_ANA_CTL** | **init 中完全不操作** | — |
| **HP_ANA_CTL** | 行 747-749 | 仅写 HP_GAIN(bit28-30, 3bit) |
| **DAC_ANA_CTL** | 行 744-746（LINEOUT_VOL）、758-760（LINEOUTL/RDIFFEN） | 音量 + 差分使能 |
| **DAC_DPC** | 行 724-725（HPF_EN 清）、754-756（DVOL） | 滤波 + 数字音量 |
| **DAC_FIFOC** | **init 中不操作**（在 hw_params/prepare/trigger 里设） | — |

> 注：DRC/HPF 配置函数（adcdrc_config 等）由 `param->dacdrc_cfg/adcdrc_cfg` 等开关控制，默认参数 `BOARD_CONFIG_CODEC_PARAM` 中 dacdrc_cfg=0/dachpf_cfg=0/adcdrc_cfg=0/adchpf_cfg=1（见 default_param，行 93-95），adchpf 默认开启。

---

## 2. 播放采样率配置路径（PCM open → hw_params → set_sysclk）

### 2.1 调用链

```
snd_pcm_open() →  [tiny-alsa snd_vela_pcm_hw_params]  (app 侧 / apps/luncher_dm/deskmate_ui.c)
   ↓
sunxi_codec_hw_params(substream, params, dai)     sun8iw20-codec.c:1206
   ├─ 按格式设 DAC_FIFOC: FIFO_MODE(bit24-25), TX_SAMPLE_BITS(bit5)     行 1215-1245
   ├─ 按采样率设 DAC_FIFOC: DAC_FS(bit29-31, 3bit)                       行 1247-1261
   └─ 按声道设 DAC_FIFOC: DAC_MONO_EN(bit6)                             行 1291-1305
   ↓
sunxi_codec_set_sysclk(dai, clk_id, freq, dir)    sun8iw20-codec.c:1340  （由上层 PCM hw_params 调用）
   └─ 播放方向: 关 moduleclk → 选 PLL 父时钟 → 设频率 → msleep(50) → 开时钟
   ↑ 时钟频率来源：上层按采样率给 24576000（48k 系列）或 22579200（44.1k 系列）
```

### 2.2 sample_rate 转换表（行 106-124）

```c
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
```

### 2.3 sunxi_codec_hw_params() 完整代码（行 1206-1338）

```c
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
```

### 2.4 sunxi_codec_set_sysclk() 完整代码（行 1340-1368）—— MCLK/PLL 时钟切换

```c
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
```

> 时钟要点：**播放方向会 disable → 换 PLL 父时钟（24576000 用 pllclk1，其余用 pllclk）→ set_rate → msleep(50) → enable**。44.1k 系列用 22579200Hz（pllclk），48k 系列用 24576000Hz（pllclk1）。

---

## 3. sunxi_codec_dapm_control() 完整代码（行 1009-1195）

### 3.1 dapm_control 主函数

```c
static int sunxi_codec_dapm_control(struct snd_pcm_substream *substream,
				struct snd_dai *dai, int onoff)
{
	struct snd_codec *codec = dai->component;
	struct sunxi_codec_info *sunxi_codec = codec->private_data;
	struct sunxi_codec_param *param = &sunxi_codec->param;

	if (substream->dapm_state == onoff)
		return 0;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/*
		 * Playback:
		 * Playback --> DAC --> DAC_DIFFER --> LINEOUT/HPOUT + (External Speaker)
		 */

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

			if ((param->adc1_flag & ADC_AUDIO_ROUTE_MIC) ||
				(param->adc2_flag & ADC_AUDIO_ROUTE_MIC) ||
				(param->adc3_flag & ADC_AUDIO_ROUTE_MIC)) {
				snd_codec_update_bits(codec, SUNXI_MICBIAS_REG,
					      0x1<<MMICBIASEN,
					      0x0<<MMICBIASEN);
			}

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
			/* digital ADC enable */
			snd_codec_update_bits(codec, SUNXI_ADC_FIFOC,
					(0x1<<EN_AD), (0x0<<EN_AD));
		}
	}
	sunxi_codec_out_dump(codec, onoff ? "dapm on" : "dapm off");
	substream->dapm_state = onoff;
	return 0;
}
```

### 3.2 sunxi_codec_playback_hp_route() 完整代码（行 854-940）

```c
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
		if (spk) {
			hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
			hal_gpio_set_data(param->gpio_spk, !param->pa_level);
		}
		/* Playback off */
		if (sunxi_codec->chip_ver == CHIP_VER_A) {
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
```

### 3.3 sunxi_codec_playback_lineout_route() 完整代码（行 942-1007）

```c
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
		if (spk) {
			hal_gpio_set_direction(param->gpio_spk, GPIO_MUXSEL_OUT);
			hal_gpio_set_data(param->gpio_spk, !param->pa_level);
		}
		/* analog DAC */
		snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
				(0x1<<DACLEN) | (0x1<<DACREN),
				(0x0<<DACLEN) | (0x0<<DACREN));
		snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
				(0x1<<DACLMUTE) | (0x1<<DACRMUTE),
				(0x0<<DACLMUTE) | (0x0<<DACRMUTE));
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
		}
	}

}
```

---

## 4. 涉及寄存器的全部写操作（grep 结果，行号 = sun8iw20-codec.c 当前版）

### 4.1 DAC_ANA_CTL（0x310）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 606 | sunxi_controls 表（LINEOUTLDIFFEN 开关） | 读改 |
| 609 | sunxi_controls 表（LINEOUTRDIFFEN 开关） | 读改 |
| 638 | sunxi_controls 表（LINEOUT volume 0x1f） | 读改 |
| 744 | **sunxi_codec_init** | LINEOUT_VOL(bit0-4) ← param->lineout_vol |
| 758 | **sunxi_codec_init** | LINEOUTLDIFFEN(6)/LINEOUTRDIFFEN(5) 置 1 |
| 838 | out_dump 读 | 打印 |
| 873 | hp_route ON（chip_ver==A） | DACLEN(15)/DACREN(14) 置 1 |
| 914 | hp_route OFF（chip_ver==A） | DACLEN/DACREN 清 0 |
| 930 | hp_route OFF（else） | DACLEN/DACREN 清 0 |
| 953 | **lineout_route ON** | DACLEN/DACREN 置 1 |
| 956 | **lineout_route ON** | DACLMUTE(12)/DACRMUTE(10) 置 1 |
| 980 | **lineout_route OFF** | DACLEN/DACREN 清 0 |
| 983 | **lineout_route OFF** | DACLMUTE/DACRMUTE 清 0 |

### 4.2 RAMP_ANA_CTL（0x31c）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 714 | **sunxi_codec_init**（chip_ver==A） | RMCEN(bit1) 置 1 |
| 719 | **sunxi_codec_init**（else） | RMCEN(bit1) 清 0 |
| 849 | out_dump 读 | 打印 |
| 887 | hp_route ON（else） | RDEN(bit0) 置 1 |
| 936 | hp_route OFF（else） | RDEN(bit0) 清 0 |
| 961/963 | **lineout_route ON**（chip_ver!=A） | 读 + RDEN 未置则置 1 |
| 994/1002 | **lineout_route OFF**（chip_ver!=A） | 读 + RDEN 已置则清 0（**-5 修复点**） |

> **注意：全驱动对 RAMP_ANA_CTL 只操作 bit0(RDEN)/bit1(RMCEN)，从未操作 bit30**。bit30=0x40000000 只出现在 out_dump 的读取打印中。

### 4.3 BIAS_ANA_CTL（0x320）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 850 | out_dump 读 | 打印 |

> **全驱动对 BIAS_ANA_CTL 只有读取，没有任何写操作**。AC_BIASDATA(bit0) 定义存在但从未写入。

### 4.4 POWER_ANA_CTL（0x348）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 712 | **sunxi_codec_init**（chip_ver==A） | HPLDO_EN(bit30) 置 1 |
| 717 | **sunxi_codec_init**（else） | HPLDO_EN(bit30) 清 0 |
| 851 | out_dump 读 | 打印 |
| 881 | hp_route ON（chip_ver==A） | HPLDO_EN 置 1 |
| 907 | hp_route OFF（注释掉） | — |

### 4.5 HP_ANA_CTL（0x340）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 635 | sunxi_controls 表（HP_GAIN 0x7） | 读改 |
| 747 | **sunxi_codec_init** | HP_GAIN(bit28-30) ← param->hpout_vol |
| 839 | out_dump 读 | 打印 |
| 861 | hp_route ON（chip_ver==A） | HPFB_BUF_EN(31)/HPFB_IN_EN(17) 置 1 |
| 865 | hp_route ON（chip_ver==A） | RAMP_OUT_EN(15)/RSWITCH(19) 置 1 |
| 877 | hp_route ON（chip_ver==A） | HP_DRVEN(21)/HP_DRVOUTEN(20) 置 1 |
| 903 | hp_route OFF（chip_ver==A） | HP_DRVEN 清 0 |
| 911 | hp_route OFF（chip_ver==A） | HP_DRVOUTEN 清 0 |
| 921 | hp_route OFF（chip_ver==A） | RAMP_OUT_EN/RSWITCH 清 0 |
| 925 | hp_route OFF（chip_ver==A） | HPFB_BUF_EN/HPFB_IN_EN 清 0 |

### 4.6 DAC_DPC（0x00）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 592 | sunxi_controls 表（DAC_HUB_EN） | 读改 |
| 601 | sunxi_controls 表（DVOL 0x3F） | 读改 |
| 724 | **sunxi_codec_init** | HPF_EN(bit18) 清 0 |
| 754 | **sunxi_codec_init** | DVOL(bit12-17) ← param->digital_vol |
| 869 | hp_route ON（chip_ver==A） | EN_DAC(bit31) 置 1 |
| 885 | hp_route ON（else） | EN_DAC 置 1 |
| 918 | hp_route OFF（chip_ver==A） | EN_DAC 清 0 |
| 934 | hp_route OFF（else） | EN_DAC 清 0 |
| 949 | **lineout_route ON** | EN_DAC 置 1 |
| 987 | **lineout_route OFF** | EN_DAC 清 0 |

### 4.7 DAC_FIFOC（0x10）

| 行 | 位置/函数 | 操作 |
|----|-----------|------|
| 1218/1220 | **hw_params**（S16_LE） | FIFO_MODE(24-25)=3, TX_SAMPLE_BITS(5)=0 |
| 1232/1234 | **hw_params**（S24/S32） | FIFO_MODE=0, TX_SAMPLE_BITS=1 |
| 1250 | **hw_params** | DAC_FS(bit29-31, 3bit) ← rate_bit（44.1k/48k 都是 0） |
| 1294/1298 | **hw_params** | DAC_MONO_EN(bit6) 按声道 |
| 1383 | **prepare** | FIFO_FLUSH(bit0) 置 1 |
| 1436 | **trigger START/RESUME/PAUSE_RELEASE** | DAC_DRQ_EN(bit4) 置 1 |
| 1451 | **trigger STOP/SUSPEND/PAUSE_PUSH** | DAC_DRQ_EN 清 0 |

---

## 5. sunxi_codec_out_dump() 实现（行 810-852）

```c
static void sunxi_codec_out_dump(struct snd_codec *codec, const char *tag)
{
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
}
```

> 当前 dump 挂载点（触发时机）：`sunxi_codec_dapm_control()` 末尾（"dapm on"/"dapm off"）+ `sunxi_codec_trigger()` 末尾（START/STOP/PAUSE_PUSH/PAUSE_RELEASE 等 cmd_name）。**init 结束时、dapm on 执行前没有打点**——A-E 五时点 dump 的改造见下一节（源码改动）。


