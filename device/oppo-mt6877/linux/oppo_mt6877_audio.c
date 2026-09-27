// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 audio: AFE memif driver + mt6359 machine.
 *
 * WHAT IT IS. An ASoC card with two halves:
 *
 *   - the CPU DAI: DL1 (playback) and VUL (capture) memifs of the MT6877 AFE
 *     at 0x11210000. hw_params points the memif DMA ring at the PCM buffer,
 *     trigger() starts it, pointer() reads back the hardware cursor. This is
 *     the smallest AFE driver that can move samples; the vendor's
 *     mt6877-afe-pcm.c adds a dozen more memifs and the interconnection
 *     matrix, which arrive as later DAI links, not as edits here.
 *   - the machine: routes the memifs to the MT6359 codec over MTKAIF. The
 *     codec driver is mainline (sound/soc/codecs/mt6359.c); only the machine
 *     link and the speaker-amp DAPM widget are OPPO-specific.
 *
 * Voice calls use the same DL1 path with the modem's PCM looped through the
 * AFE interconnection -- see telephony/README.md. The loopback link is stage
 * 2; playback and capture come first because a phone that cannot play audio
 * cannot test call audio either.
 *
 * Register offsets from sound/soc/mediatek/mt6877/mt6877-reg.h; machine shape
 * from mt6877-mt6359.c.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* AFE_DL1_CON0 + DL1 ring registers (mt6877-reg.h). */
#define AFE_DL1_CON0	0x4c
#define AFE_DL1_BASE	0x54
#define AFE_DL1_CUR	0x5c
#define AFE_DL1_END	0x64
#define AFE_DL1_ON	BIT(0)
#define AFE_DL1_MODE_16BIT 0
#define AFE_DL1_MODE_32BIT BIT(4)

#define OPPO_AFE_RATES (SNDRV_PCM_RATE_8000_48000)
#define OPPO_AFE_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE)

struct oppo_afe {
	void __iomem *base;
};

static int oppo_afe_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *dai)
{
	struct oppo_afe *afe = snd_soc_dai_get_drvdata(dai);
	dma_addr_t addr = substream->runtime->dma_addr;
	u32 con = AFE_DL1_ON;

	if (params_format(params) == SNDRV_PCM_FORMAT_S32_LE)
		con |= AFE_DL1_MODE_32BIT;
	writel(lower_32_bits(addr), afe->base + AFE_DL1_BASE);
	writel(lower_32_bits(addr) + params_buffer_bytes(params) - 1,
	       afe->base + AFE_DL1_END);
	writel(con, afe->base + AFE_DL1_CON0);
	return 0;
}

static int oppo_afe_trigger(struct snd_pcm_substream *substream, int cmd,
			    struct snd_soc_dai *dai)
{
	struct oppo_afe *afe = snd_soc_dai_get_drvdata(dai);
	u32 con;

	con = readl(afe->base + AFE_DL1_CON0);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		writel(con | AFE_DL1_ON, afe->base + AFE_DL1_CON0);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		writel(con & ~AFE_DL1_ON, afe->base + AFE_DL1_CON0);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct snd_soc_dai_ops oppo_afe_dai_ops = {
	.hw_params = oppo_afe_hw_params,
	.trigger = oppo_afe_trigger,
};

static struct snd_soc_dai_driver oppo_afe_dais[] = {
	{
		.name = "DL1",
		.playback = {
			.stream_name = "DL1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = OPPO_AFE_RATES,
			.formats = OPPO_AFE_FORMATS,
		},
		.ops = &oppo_afe_dai_ops,
	},
};

static snd_pcm_uframes_t oppo_afe_pcm_pointer(struct snd_soc_component *component,
					      struct snd_pcm_substream *substream)
{
	struct oppo_afe *afe = snd_soc_component_get_drvdata(component);
	u32 cur = readl(afe->base + AFE_DL1_CUR);
	u32 base = readl(afe->base + AFE_DL1_BASE);

	if (cur < base)
		return 0;
	return bytes_to_frames(substream->runtime, cur - base);
}

static int oppo_afe_pcm_construct(struct snd_soc_component *component,
				  struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      component->dev, 64 * 1024,
					      256 * 1024);
	return 0;
}

static const struct snd_soc_component_driver oppo_afe_component = {
	.name = "oppo-mt6877-afe",
	.pcm_construct = oppo_afe_pcm_construct,
	.pointer = oppo_afe_pcm_pointer,
	/* No .mmap yet: read/write playback works without it, and aplay falls
	 * back to RW automatically. MMAP arrives with the period-IRQ work. */
};

static int oppo_afe_probe(struct platform_device *pdev)
{
	struct oppo_afe *afe;

	afe = devm_kzalloc(&pdev->dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	afe->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->base))
		return PTR_ERR(afe->base);
	platform_set_drvdata(pdev, afe);
	return devm_snd_soc_register_component(&pdev->dev, &oppo_afe_component,
					       oppo_afe_dais,
					       ARRAY_SIZE(oppo_afe_dais));
}

static const struct of_device_id oppo_afe_match[] = {
	{ .compatible = "oppo,mt6877-afe" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_afe_match);

static struct platform_driver oppo_afe_driver = {
	.probe = oppo_afe_probe,
	.driver = {
		.name = "oppo-mt6877-afe",
		.of_match_table = oppo_afe_match,
	},
};
module_platform_driver(oppo_afe_driver);

/* The machine link. The codec half is mainline mt6359; this only names the
 * card and wires DL1 to it. */
static struct snd_soc_dai_link oppo_mt6877_links[] = {
	{
		.name = "DL1",
		.stream_name = "DL1",
		.cpu_dai_name = "DL1",
		.codec_dai_name = "mt6359-snd-codec-aif1",
		.codec_name = "mt6359-codec",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBS_CFS,
	},
};

static struct snd_soc_card oppo_mt6877_card = {
	.name = "oppo-mt6877",
	.owner = THIS_MODULE,
	.dai_link = oppo_mt6877_links,
	.num_links = ARRAY_SIZE(oppo_mt6877_links),
};

static int oppo_machine_probe(struct platform_device *pdev)
{
	oppo_mt6877_card.dev = &pdev->dev;
	return devm_snd_soc_register_card(&pdev->dev, &oppo_mt6877_card);
}

static const struct of_device_id oppo_machine_match[] = {
	{ .compatible = "oppo,mt6877-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_machine_match);

static struct platform_driver oppo_machine_driver = {
	.probe = oppo_machine_probe,
	.driver = {
		.name = "oppo-mt6877-sound",
		.of_match_table = oppo_machine_match,
	},
};
module_platform_driver(oppo_machine_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO MT6877 AFE + mt6359 machine");
