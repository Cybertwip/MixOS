// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 CONSYS Wi-Fi: the platform driver.
 *
 * Maps the windows, waits for the PMIC, and runs the bring-up: power the
 * connectivity subsystem, open the BTIF link, put both ROM patches down it, then
 * push MediaTek's WLAN firmware down the AHB HIF and start it.
 *
 * A mirror of PowerEngine/OS/MVII's mt6592_wifi.c, which is where the same steps
 * are sequenced for the bootstrap.  What is different here is the waiting: MVII
 * owns the machine and can assume the PMIC is up and the images are in hand, and
 * a Linux driver can assume neither, so the PMIC dependency is a probe deferral
 * and the images come from request_firmware().
 *
 *
 * ── WHAT THIS BUILD DOES AND DOES NOT GET YOU ──
 *
 * It gets the connectivity MCU powered, clocked, talking, running a patched image
 * with its radio configured, and then WIFI_RAM_CODE_SOC downloaded and executing
 * -- WLAN_READY asserted.
 *
 * It does NOT get a network interface.  WLAN_READY means the firmware is running,
 * not that anything can be sent through it: scanning, association, key management
 * and the data path are a further layer on top of that transport and are not in
 * this build.  There is deliberately no netdev registered and no wiphy, because a
 * driver that offers an interface it cannot carry traffic on is worse than one
 * that says plainly where it stopped -- so the last line this driver logs is which
 * stage it reached, every time, success or not.
 */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/stdarg.h>
#include <linux/workqueue.h>

#include "j36_mt6592_pmic.h"
#include "j36_mt6592_wifi.h"

/*
 * The two ROM patches, off this device's own /system/etc/firmware.
 *
 * They are named here in file order and sorted below by the sequence byte in
 * their own headers, because the two do not agree: ROMv1_patch_1_1_hdr.bin
 * carries sequence 1 and ROMv1_patch_1_0_hdr.bin carries sequence 2.  Reading the
 * order off the filenames puts them down backwards, and the peer rejects an
 * out-of-order patch by going quiet rather than by saying so.
 *
 * WIFI_RAM_CODE_SOC is the third blob in that directory and is the WLAN firmware
 * itself, which goes down the AHB HIF once the MCU is patched.  It has no
 * extension, which is worth saying out loud because it is not an oversight and a
 * *.bin staging glob silently omitted it from the image for a while.
 */
static const char * const j36_wifi_patch_names[] = {
	"mediatek/mt6592/ROMv1_patch_1_1_hdr.bin",
	"mediatek/mt6592/ROMv1_patch_1_0_hdr.bin",
};

#define J36_WIFI_RAM_CODE_NAME	"mediatek/mt6592/WIFI_RAM_CODE_SOC"

MODULE_FIRMWARE("mediatek/mt6592/ROMv1_patch_1_1_hdr.bin");
MODULE_FIRMWARE("mediatek/mt6592/ROMv1_patch_1_0_hdr.bin");
MODULE_FIRMWARE(J36_WIFI_RAM_CODE_NAME);

struct j36_wifi_patch {
	const struct firmware *fw;
	u8 sequence;
};

/*
 * The bring-up state, plus the images, plus nothing else.
 *
 * struct j36_wifi is the part the other translation units share; the image set is
 * main.c's alone, and how many patches there are is a property of this file's
 * table rather than of the hardware.  Keeping it out here means the header does
 * not have to name a count that only this file can know.
 */
struct j36_wifi_device {
	struct j36_wifi w;
	struct j36_wifi_patch patch[ARRAY_SIZE(j36_wifi_patch_names)];
	const struct firmware *wlan_fw;
	bool patches_ready;
};

/*
 * One reason, recorded last-writer-wins, and always logged.
 *
 * Last-wins is the useful direction: the low-level helper that noticed the
 * failure names the operation ("no usable answer to opcode 0x08") and its caller
 * names the stage ("patch-address setup failed"), and it is the stage that tells
 * you where to look.  Both lines go to dmesg -- MVII suppresses the repeat
 * because its log is a 115200 baud UART, which is not a constraint here.
 */
void j36_wifi_fail(struct j36_wifi *w, const char *blocked, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	dev_err(w->dev, "%pV [%s]\n", &vaf, blocked);
	va_end(args);

	w->blocked = blocked;
}

/* ── the firmware images ─────────────────────────────────────────────────────*/

/*
 * READ AT PROBE, NOT AT BRING-UP, and the reason is where this driver is loaded
 * from rather than anything about the hardware.
 *
 * The module is insmodded by the initramfs /init, which then switch_roots.  The
 * firmware loader searches the filesystem that is mounted NOW, so a
 * request_firmware() from the work item is a race against a root filesystem
 * changing underneath it -- and one that would resolve differently on a fast
 * boot than on a slow one.  Doing it here makes insmod itself the synchronisation
 * point: it does not return until both images are in kernel memory, so /init can
 * carry on and take the world with it.
 *
 * Failing is not fatal to probe.  Everything up to the link probe still runs and
 * still reports, and "the MCU answered on BTIF but there was no patch to send it"
 * is a materially different diagnosis from "the MCU never answered" -- which is
 * the whole reason the bring-up is staged.
 */
static void j36_wifi_request_images(struct j36_wifi_device *jd)
{
	struct j36_wifi *w = &jd->w;
	unsigned int i, j;

	/*
	 * The WLAN image first, and before any early return below, because the two
	 * failures are independent and the switch_root deadline applies to both.  A
	 * missing ROM patch must not be able to skip the read of an image that is
	 * there, or the log would blame the patch for a stage that never ran.
	 */
	if (firmware_request_nowarn(&jd->wlan_fw, J36_WIFI_RAM_CODE_NAME, w->dev))
		jd->wlan_fw = NULL;

	for (i = 0; i < ARRAY_SIZE(jd->patch); i++) {
		/* _nowarn: a missing patch is reported below in terms of what it
		 * costs, and the loader's own "Direct firmware load failed"
		 * describes the mechanism rather than the consequence. */
		if (firmware_request_nowarn(&jd->patch[i].fw,
					    j36_wifi_patch_names[i], w->dev)) {
			j36_wifi_fail(w, "rom-patch-missing",
				      "%s is not in the firmware search path",
				      j36_wifi_patch_names[i]);
			return;
		}
		if (jd->patch[i].fw->size <= J36_WMT_PATCH_HEADER_SIZE) {
			j36_wifi_fail(w, "rom-patch-truncated",
				      "%s is %zu bytes, shorter than its own header",
				      j36_wifi_patch_names[i],
				      jd->patch[i].fw->size);
			return;
		}
		jd->patch[i].sequence =
			jd->patch[i].fw->data[J36_WMT_PATCH_METADATA_OFFSET] & 0x0f;
	}

	/* Insertion sort over two entries.  Written as a sort rather than a swap
	 * so that a three-patch set, which the header's four-bit count field can
	 * express, does not need this rewritten. */
	for (i = 1; i < ARRAY_SIZE(jd->patch); i++) {
		struct j36_wifi_patch held = jd->patch[i];

		for (j = i; j && jd->patch[j - 1].sequence > held.sequence; j--)
			jd->patch[j] = jd->patch[j - 1];
		jd->patch[j] = held;
	}

	jd->patches_ready = true;
}

static void j36_wifi_release_images(void *data)
{
	struct j36_wifi_device *jd = data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(jd->patch); i++) {
		release_firmware(jd->patch[i].fw);
		jd->patch[i].fw = NULL;
	}
	release_firmware(jd->wlan_fw);
	jd->wlan_fw = NULL;
	jd->patches_ready = false;
}

/* ── the bring-up ────────────────────────────────────────────────────────────*/

/*
 * Everything, in order, on a workqueue.
 *
 * This is seconds of work -- a hundred patch fragments, each with a two-second
 * ceiling on its answer -- so it is on system_long_wq rather than the default one,
 * and it is off the probe path entirely.  A radio that fails to come up must not
 * be able to hold up the rest of the boot, and on this board the rest of the boot
 * includes the panel.
 */
static void j36_wifi_bring_up(struct work_struct *work)
{
	struct j36_wifi *w = container_of(work, struct j36_wifi, bring_up);
	struct j36_wifi_device *jd = container_of(w, struct j36_wifi_device, w);
	unsigned int i;

	mutex_lock(&w->lock);

	if (j36_wifi_consys_bind(w))
		goto out;
	if (j36_wifi_wmt_probe_link(w))
		goto out;
	if (!jd->patches_ready) {
		/* blocked already names which image and why, from probe. */
		dev_warn(w->dev,
			 "the connectivity MCU is talking but there is no ROM patch to send it\n");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(jd->patch); i++)
		if (j36_wifi_wmt_load_patch(w, jd->patch[i].fw->data,
					    jd->patch[i].fw->size))
			goto out;

	/*
	 * Stage 3.  Not fatal to the stages already reported: a patched MCU with no
	 * WLAN firmware is a real, useful, and precisely diagnosable state, and the
	 * trace below prints either way.
	 */
	if (!jd->wlan_fw) {
		j36_wifi_fail(w, "wlan-firmware-missing",
			      "%s is not in the firmware search path",
			      J36_WIFI_RAM_CODE_NAME);
		goto out;
	}
	j36_wifi_hif_load_firmware(w, jd->wlan_fw->data, jd->wlan_fw->size);

out:
	/*
	 * The one line that is always printed, whatever happened.  Even the
	 * success case names what is missing -- because a log that stops at "ok"
	 * invites the reading that there is a network interface somewhere.
	 */
	if (w->firmware_alive)
		dev_info(w->dev,
			 "WLAN firmware running: chip 0x%08x, %u ROM patches, RF %s, %u bytes downloaded, WLAN_READY -- there is no netdev, cfg80211 is not in this build\n",
			 w->chip_id, w->patch_count,
			 w->calibrated ? "calibrated" : "uncalibrated",
			 w->hif_stats.downloaded_bytes);
	else if (w->ready)
		dev_warn(w->dev,
			 "connectivity MCU up: chip 0x%08x, %u ROM patches, RF %s -- but the WLAN firmware did not start, stopped at [%s]\n",
			 w->chip_id, w->patch_count,
			 w->calibrated ? "calibrated" : "uncalibrated",
			 w->blocked ? w->blocked : "unknown");
	else
		dev_warn(w->dev, "Wi-Fi bring-up stopped at [%s]\n",
			 w->blocked ? w->blocked : "unknown");
	j36_wifi_wmt_trace(w, "bring-up");
	j36_wifi_hif_trace(w);

	mutex_unlock(&w->lock);
}

/* ── probe ───────────────────────────────────────────────────────────────────*/

/*
 * Same helper and same reasoning as every other driver on this board: the region
 * is NOT claimed.  infracfg, pericfg and the SPM are syscons that several drivers
 * here reach into for one documented bit each, and a request_mem_region() from any
 * one of them would lock the others out of blocks none of them owns.
 */
static void __iomem *j36_iomap_phandle(struct device *dev, const char *property)
{
	struct device_node *node;
	struct resource resource;
	void __iomem *base;
	int ret;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return ERR_PTR(-EINVAL);

	ret = of_address_to_resource(node, 0, &resource);
	of_node_put(node);
	if (ret)
		return ERR_PTR(ret);

	base = devm_ioremap(dev, resource.start, resource_size(&resource));
	if (!base)
		return ERR_PTR(-ENOMEM);
	return base;
}

/*
 * The CONSYS EMI aperture.
 *
 * A no-map reserved-memory region, which is what makes the ioremap below legal:
 * ARM32 refuses to ioremap memory that is in the kernel's linear map, precisely so
 * two mappings of one page cannot disagree about cacheability, and a no-map region
 * was never put in it.  Write-combining rather than uncached because the only
 * traffic is a 343 KiB memset at bind and, later, firmware images; there is
 * nothing here that a write buffer can reorder into a wrong answer.
 */
static int j36_wifi_map_emi(struct device *dev, struct j36_wifi *w)
{
	struct device_node *node;
	struct resource resource;
	int ret;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return dev_err_probe(dev, -EINVAL,
				     "no memory-region for the CONSYS EMI window\n");

	ret = of_address_to_resource(node, 0, &resource);
	of_node_put(node);
	if (ret)
		return dev_err_probe(dev, ret, "resolve the CONSYS EMI window\n");

	/* The aperture's base goes into a twelve-bit field of 1 MiB units, so an
	 * unaligned region would be silently truncated down to the megabyte and
	 * the subsystem would be reading somebody else's memory. */
	if (!IS_ALIGNED(resource.start, SZ_1M) ||
	    resource_size(&resource) < J36_CONSYS_EMI_SHARE_OFFSET +
				       J36_CONSYS_EMI_SHARE_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "the CONSYS EMI window at %pa must be 1 MiB aligned and at least 0x%x bytes\n",
				     &resource.start,
				     J36_CONSYS_EMI_SHARE_OFFSET +
					     J36_CONSYS_EMI_SHARE_SIZE);

	w->emi = devm_ioremap_wc(dev, resource.start, resource_size(&resource));
	if (!w->emi)
		return dev_err_probe(dev, -ENOMEM,
				     "map the CONSYS EMI window at %pa\n",
				     &resource.start);
	w->emi_phys = resource.start;
	w->emi_size = resource_size(&resource);
	return 0;
}

static void j36_wifi_cancel_bring_up(void *data)
{
	struct j36_wifi *w = data;

	cancel_work_sync(&w->bring_up);
}

static int j36_wifi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct j36_wifi_device *jd;
	struct j36_wifi *w;
	u32 probe_read;
	int ret;

	/*
	 * The rails this driver needs are on the MT6323, behind a PMIC wrapper
	 * with exactly one owner, and that owner is a separate module.  The symbol
	 * resolves as soon as that module is loaded; what this read establishes is
	 * that its DEVICE has bound, which is a different thing and the thing that
	 * matters.  Deferring is the whole answer: the driver core retries when the
	 * PMIC binds, and if it never does, this stays on the deferred list with a
	 * reason rather than raising a radio on rails nobody brought up.
	 */
	ret = j36_pmic_pwrap_read(J36_PMIC_DIGLDO_CON11, &probe_read);
	if (ret == -ENODEV)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for j36_mt6592_pmic to bind\n");
	if (ret)
		return dev_err_probe(dev, ret, "the PMIC wrapper did not answer\n");

	jd = devm_kzalloc(dev, sizeof(*jd), GFP_KERNEL);
	if (!jd)
		return -ENOMEM;
	w = &jd->w;
	w->dev = dev;
	w->blocked = "not-started";
	mutex_init(&w->lock);

	/* Ours: the BTIF link, the connectivity MCU's config block, and the WLAN
	 * AHB HIF the firmware download runs over. */
	w->btif = devm_platform_ioremap_resource_byname(pdev, "btif");
	if (IS_ERR(w->btif))
		return dev_err_probe(dev, PTR_ERR(w->btif), "map BTIF\n");

	w->consys = devm_platform_ioremap_resource_byname(pdev, "conn-mcu");
	if (IS_ERR(w->consys))
		return dev_err_probe(dev, PTR_ERR(w->consys),
				     "map the connectivity MCU config block\n");

	w->hif = devm_platform_ioremap_resource_byname(pdev, "hif");
	if (IS_ERR(w->hif))
		return dev_err_probe(dev, PTR_ERR(w->hif), "map the WLAN AHB HIF\n");

	/* Borrowed: the three shared SoC blocks. */
	w->infracfg = j36_iomap_phandle(dev, "j36,infracfg-controller");
	if (IS_ERR(w->infracfg))
		return dev_err_probe(dev, PTR_ERR(w->infracfg), "map INFRACFG\n");

	w->pericfg = j36_iomap_phandle(dev, "j36,pericfg-controller");
	if (IS_ERR(w->pericfg))
		return dev_err_probe(dev, PTR_ERR(w->pericfg), "map PERICFG\n");

	w->spm = j36_iomap_phandle(dev, "j36,spm-controller");
	if (IS_ERR(w->spm))
		return dev_err_probe(dev, PTR_ERR(w->spm), "map the SPM\n");

	ret = j36_wifi_map_emi(dev, w);
	if (ret)
		return ret;

	/* stp_core.c:327-331's opening values.  The 7 is stock's opening ack, not
	 * a sentinel: it is what the first frame carries in its low three bits. */
	w->last_rx_sequence = 7;
	w->last_ack = U32_MAX;

	/* Registered BEFORE the work item's own action, so it runs AFTER it on the
	 * way out: devm unwinds in reverse, and the images must outlive the last
	 * fragment that reads them. */
	ret = devm_add_action_or_reset(dev, j36_wifi_release_images, jd);
	if (ret)
		return ret;
	j36_wifi_request_images(jd);

	INIT_WORK(&w->bring_up, j36_wifi_bring_up);
	ret = devm_add_action_or_reset(dev, j36_wifi_cancel_bring_up, w);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, jd);

	dev_info(dev, "MT6592 CONSYS Wi-Fi: starting bring-up\n");
	queue_work(system_long_wq, &w->bring_up);
	return 0;
}

/*
 * There is deliberately no remove-time power-down.
 *
 * Bringing the subsystem back down means undoing the MTCMOS sequence in reverse
 * with the bus protection re-asserted first, and getting that wrong on a domain
 * the AP shares a bus with does not fail politely -- it stalls the bus until the
 * watchdog resets the board.  MVII never had to write that path because it never
 * unloads, and this driver is not going to acquire an unverified version of it to
 * make rmmod look tidier.  What remove does do is stop the work item, which is the
 * part that would actually corrupt something if it kept running.
 */

static const struct of_device_id j36_wifi_of_match[] = {
	{ .compatible = "j36,j36-ultra-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, j36_wifi_of_match);

static struct platform_driver j36_wifi_driver = {
	.probe = j36_wifi_probe,
	.driver = {
		.name = "j36-mt6592-wifi",
		.of_match_table = j36_wifi_of_match,
	},
};
module_platform_driver(j36_wifi_driver);

MODULE_DESCRIPTION("J36 Ultra MT6592 CONSYS Wi-Fi bring-up (power, BTIF/STP/WMT, ROM patches, WLAN firmware)");
MODULE_AUTHOR("MixOS / PowerEngine integration");
MODULE_LICENSE("GPL");
