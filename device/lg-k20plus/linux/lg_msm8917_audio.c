// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus audio machine: "lv517-snd-card".
 *
 * WHAT IT IS. The ASoC machine that wires mainline LPASS (lpass-cpu, the
 * apq8016 data the whole 8916 family shares) to the board codec: the
 * PMI8950's internal WCD codec over MI2S. Playback and capture DAIs are the
 * LPASS low-power MI2S ports; the speaker/headset muxing is DAPM.
 *
 * WHY IT CANNOT PROBE YET. lpass-cpu needs MI2S bit/OSR clocks with real
 * rate control, and those come from the LPASS clock controller behind
 * gcc-msm8917 -- which does not exist in mainline. The DTS therefore keeps
 * the sound node disabled, and this driver waits for it. USB audio covers
 * v1 (a headset on the OTG port works today); this machine is the speaker
 * path and is written now so the LPASSCC port has a consumer waiting.
 *
 * Machine shape from msm8917-lge-sound.dtsi (model "msm8952-snd-card").
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

static struct snd_soc_dai_link lv517_links[] = {
	{
		.name = "Primary MI2S",
		.stream_name = "Primary",
		.cpu_dai_name = "Primary MI2S",
		.codec_dai_name = "lge-lv517-codec-dai",
		.codec_name = "lge-lv517-codec",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBS_CFS,
		/* The codec half is the PMI8950's internal WCD block, which
		 * has no mainline driver yet -- the names above are the
		 * reservation its port will register under. */
	},
};

static struct snd_soc_card lv517_card = {
	.name = "lv517-snd-card",
	.owner = THIS_MODULE,
	.dai_link = lv517_links,
	.num_links = ARRAY_SIZE(lv517_links),
};

static int lv517_audio_probe(struct platform_device *pdev)
{
	lv517_card.dev = &pdev->dev;
	return devm_snd_soc_register_card(&pdev->dev, &lv517_card);
}

static const struct of_device_id lv517_audio_match[] = {
	{ .compatible = "lge,lv517-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, lv517_audio_match);

static struct platform_driver lv517_audio_driver = {
	.probe = lv517_audio_probe,
	.driver = {
		.name = "lg-lv517-sound",
		.of_match_table = lv517_audio_match,
	},
};
module_platform_driver(lv517_audio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus audio machine");
