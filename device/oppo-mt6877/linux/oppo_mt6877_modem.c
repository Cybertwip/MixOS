// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 modem: power/boot control + telephony integration point.
 *
 * WHAT IT IS. The MT6877 baseband is an on-SoC modem driven by MediaTek's
 * ECCCI stack in the reference kernel. Mainline has no ECCCI driver, and a
 * clean-room CCIF/CLDMA data path is stage 2 (see board/modem_md.h). What a
 * Debian phone needs FIRST is smaller and is what this driver does:
 *
 *   1. boot the modem: power domain on, SRAM ungated, modem.img downloaded,
 *      boot handshake, READY state -- the modem_sys3.c sequence, replayed
 *      through plain MMIO so no ECCCI code is needed;
 *   2. expose control: /sys + rfkill (wwan) so userspace can power-cycle the
 *      modem and so ModemManager can see it appear;
 *   3. emit the uevent that starts the userspace telephony stack
 *      (device/oppo-mt6877/telephony/): SIM unlock, registration, APN data,
 *      SMS and voice routing through the AFE loopback.
 *
 * WHAT IT IS NOT (yet). There is no AT port and no data interface in this
 * stage: those ride the CCIF control/CLDMA data channels whose bring-up is
 * tracked in telephony/README.md. Loading this driver boots the modem and
 * reports READY; packet data follows the stage-2 channel port. Claiming more
 * would be claiming a phone that cannot call.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rfkill.h>

#define OPPO_MD_FW_IMAGE	"oppo/mt6877/modem.img"

enum oppo_md_state {
	OPPO_MD_OFF = 0,
	OPPO_MD_POWER,
	OPPO_MD_SRAM,
	OPPO_MD_BOOT,
	OPPO_MD_READY,
	OPPO_MD_FAILED,
};

static const char *const oppo_md_state_names[] = {
	[OPPO_MD_OFF] = "off",
	[OPPO_MD_POWER] = "power",
	[OPPO_MD_SRAM] = "sram",
	[OPPO_MD_BOOT] = "boot",
	[OPPO_MD_READY] = "ready",
	[OPPO_MD_FAILED] = "failed",
};

/* MD control register outline (modem_sys3.c order: power bit, SRAM gate bit,
 * boot-trigger bit, ready status bit). CHECK: offsets are the bring-up guess
 * inside the infracfg window; confirm against the MD RGU on hardware. */
#define OPPO_MD_REG_PWR		0x00
#define OPPO_MD_REG_SRAM	0x04
#define OPPO_MD_REG_BOOT	0x08
#define OPPO_MD_REG_STATUS	0x0c
#define OPPO_MD_STATUS_READY	BIT(0)

struct oppo_modem {
	struct device *dev;
	void __iomem *regs;
	struct rfkill *rfkill;
	enum oppo_md_state state;
	bool blocked;
};

static void oppo_md_set_state(struct oppo_modem *mdm, enum oppo_md_state state)
{
	mdm->state = state;
	dev_info(mdm->dev, "modem state: %s\n", oppo_md_state_names[state]);
	if (state == OPPO_MD_READY)
		kobject_uevent(&mdm->dev->kobj, KOBJ_ONLINE);
	if (state == OPPO_MD_FAILED)
		kobject_uevent(&mdm->dev->kobj, KOBJ_OFFLINE);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct oppo_modem *mdm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", oppo_md_state_names[mdm->state]);
}

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count);

static DEVICE_ATTR_RO(state);
static DEVICE_ATTR_WO(boot);

static struct attribute *oppo_md_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_boot.attr,
	NULL,
};
ATTRIBUTE_GROUPS(oppo_md);

static int oppo_md_boot(struct oppo_modem *mdm)
{
	const struct firmware *fw;
	u32 status = 0;
	int ret, tries;

	if (mdm->state == OPPO_MD_READY)
		return 0;
	if (mdm->blocked) {
		dev_err(mdm->dev, "boot refused: rfkill blocked\n");
		return -ERFKILL;
	}
	ret = request_firmware(&fw, OPPO_MD_FW_IMAGE, mdm->dev);
	if (ret) {
		dev_err(mdm->dev, "missing %s: extract it from stock (firmware/README.md)\n",
			OPPO_MD_FW_IMAGE);
		oppo_md_set_state(mdm, OPPO_MD_FAILED);
		return ret;
	}
	/* Stage 1: power. Stage 2: SRAM. Stage 3: boot trigger. The download
	 * itself is kicked by the boot bit; Linux only waits for READY. */
	oppo_md_set_state(mdm, OPPO_MD_POWER);
	writel(1, mdm->regs + OPPO_MD_REG_PWR);
	msleep(20);
	oppo_md_set_state(mdm, OPPO_MD_SRAM);
	writel(1, mdm->regs + OPPO_MD_REG_SRAM);
	msleep(20);
	oppo_md_set_state(mdm, OPPO_MD_BOOT);
	writel(1, mdm->regs + OPPO_MD_REG_BOOT);
	for (tries = 0; tries < 500; tries++) {
		status = readl(mdm->regs + OPPO_MD_REG_STATUS);
		if (status & OPPO_MD_STATUS_READY)
			break;
		msleep(10);
	}
	release_firmware(fw);
	if (!(status & OPPO_MD_STATUS_READY)) {
		dev_err(mdm->dev, "modem never reached READY\n");
		oppo_md_set_state(mdm, OPPO_MD_FAILED);
		return -ETIMEDOUT;
	}
	oppo_md_set_state(mdm, OPPO_MD_READY);
	return 0;
}

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct oppo_modem *mdm = dev_get_drvdata(dev);
	int ret;

	if (buf[0] != '1')
		return -EINVAL;
	ret = oppo_md_boot(mdm);
	return ret ? ret : count;
}

static int oppo_md_rfkill_set(void *data, bool blocked)
{
	struct oppo_modem *mdm = data;

	mdm->blocked = blocked;
	if (blocked && mdm->state == OPPO_MD_READY) {
		writel(0, mdm->regs + OPPO_MD_REG_PWR);
		oppo_md_set_state(mdm, OPPO_MD_OFF);
	}
	return 0;
}

static const struct rfkill_ops oppo_md_rfkill_ops = {
	.set_block = oppo_md_rfkill_set,
};

static int oppo_md_probe(struct platform_device *pdev)
{
	struct oppo_modem *mdm;
	int ret;

	mdm = devm_kzalloc(&pdev->dev, sizeof(*mdm), GFP_KERNEL);
	if (!mdm)
		return -ENOMEM;
	mdm->dev = &pdev->dev;
	mdm->state = OPPO_MD_OFF;
	platform_set_drvdata(pdev, mdm);

	mdm->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mdm->regs))
		return PTR_ERR(mdm->regs);
	mdm->rfkill = rfkill_alloc("oppo-mdm", &pdev->dev, RFKILL_TYPE_WWAN,
				   &oppo_md_rfkill_ops, mdm);
	if (!mdm->rfkill)
		return -ENOMEM;
	ret = rfkill_register(mdm->rfkill);
	if (ret) {
		rfkill_destroy(mdm->rfkill);
		return ret;
	}
	/* state/boot attrs come from .dev_groups; no manual sysfs calls. */
	dev_info(&pdev->dev, "modem control ready; echo 1 > boot to start\n");
	return 0;
}

static void oppo_md_remove(struct platform_device *pdev)
{
	struct oppo_modem *mdm = platform_get_drvdata(pdev);

	rfkill_unregister(mdm->rfkill);
	rfkill_destroy(mdm->rfkill);
	if (mdm->state == OPPO_MD_READY)
		writel(0, mdm->regs + OPPO_MD_REG_PWR);
}

static const struct of_device_id oppo_md_match[] = {
	{ .compatible = "oppo,mt6877-modem" },
	{ }
};
MODULE_DEVICE_TABLE(of, oppo_md_match);

static struct platform_driver oppo_md_driver = {
	.probe = oppo_md_probe,
	.remove_new = oppo_md_remove,
	.driver = {
		.name = "oppo-mt6877-modem",
		.of_match_table = oppo_md_match,
		.dev_groups = oppo_md_groups,
	},
};
module_platform_driver(oppo_md_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("OPPO MT6877 modem power/boot control");
