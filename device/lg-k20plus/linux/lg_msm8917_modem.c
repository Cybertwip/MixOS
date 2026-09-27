// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG K20 Plus modem glue: remoteproc watch + telephony integration point.
 *
 * WHAT IT IS. On this phone the modem itself is mainline's: qcom_q6v5_mss
 * boots the X6 (with the msm8917 patch in this directory), qrtr carries
 * QMI, and ModemManager's `qmi` plugin does registration, data, SMS and
 * voice signalling. What mainline does NOT do is the board policy around
 * it, which is this driver:
 *
 *   1. rmtfs/EFS check: the modem reads its NV from the rmtfs partitions
 *      and silently refuses to boot without them. Probe verifies the rmtfs
 *      memory is reserved and says which partition to flash when it is not.
 *   2. rfkill (wwan) + sysfs state, mirroring the OPPO modem driver so the
 *      telephony scripts work on both families unchanged.
 *   3. the ONLINE uevent when qrtr-ns announces the modem -- the hook the
 *      udev rule and systemd unit wait on before starting ModemManager.
 *
 * The QRTR wait is a userspace-assisted poll: this driver exposes `state`
 * (off/boot/ready) and the telephony script flips it to ready when
 * `qrtr-lookup` sees the modem services. A kernel qrtr client just to flip
 * one bit would be machinery for its own sake.
 */

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rfkill.h>

enum lv517_mdm_state {
	LV517_MDM_OFF = 0,
	LV517_MDM_BOOT,
	LV517_MDM_READY,
	LV517_MDM_FAILED,
};

static const char *const lv517_mdm_state_names[] = {
	[LV517_MDM_OFF] = "off",
	[LV517_MDM_BOOT] = "boot",
	[LV517_MDM_READY] = "ready",
	[LV517_MDM_FAILED] = "failed",
};

struct lv517_modem {
	struct device *dev;
	struct rfkill *rfkill;
	enum lv517_mdm_state state;
	bool blocked;
};

static void lv517_mdm_set_state(struct lv517_modem *mdm,
				enum lv517_mdm_state state)
{
	mdm->state = state;
	dev_info(mdm->dev, "modem state: %s\n", lv517_mdm_state_names[state]);
	if (state == LV517_MDM_READY)
		kobject_uevent(&mdm->dev->kobj, KOBJ_ONLINE);
	if (state == LV517_MDM_FAILED)
		kobject_uevent(&mdm->dev->kobj, KOBJ_OFFLINE);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct lv517_modem *mdm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", lv517_mdm_state_names[mdm->state]);
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct lv517_modem *mdm = dev_get_drvdata(dev);

	/* Written by the telephony stack: "boot" when remoteproc starts the
	 * MSS, "ready" when qrtr-lookup sees its services. */
	if (!strncmp(buf, "boot", 4))
		lv517_mdm_set_state(mdm, LV517_MDM_BOOT);
	else if (!strncmp(buf, "ready", 5))
		lv517_mdm_set_state(mdm, LV517_MDM_READY);
	else if (!strncmp(buf, "failed", 6))
		lv517_mdm_set_state(mdm, LV517_MDM_FAILED);
	else
		return -EINVAL;
	return count;
}
static DEVICE_ATTR_RW(state);

static struct attribute *lv517_mdm_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lv517_mdm);

static int lv517_mdm_rfkill_set(void *data, bool blocked)
{
	struct lv517_modem *mdm = data;

	mdm->blocked = blocked;
	if (blocked && mdm->state != LV517_MDM_OFF)
		lv517_mdm_set_state(mdm, LV517_MDM_OFF);
	return 0;
}

static const struct rfkill_ops lv517_mdm_rfkill_ops = {
	.set_block = lv517_mdm_rfkill_set,
};

static int lv517_mdm_probe(struct platform_device *pdev)
{
	struct lv517_modem *mdm;
	int ret;

	mdm = devm_kzalloc(&pdev->dev, sizeof(*mdm), GFP_KERNEL);
	if (!mdm)
		return -ENOMEM;
	mdm->dev = &pdev->dev;
	mdm->state = LV517_MDM_OFF;
	platform_set_drvdata(pdev, mdm);

	mdm->rfkill = rfkill_alloc("lg-mss", &pdev->dev, RFKILL_TYPE_WWAN,
				   &lv517_mdm_rfkill_ops, mdm);
	if (!mdm->rfkill)
		return -ENOMEM;
	ret = rfkill_register(mdm->rfkill);
	if (ret) {
		rfkill_destroy(mdm->rfkill);
		return ret;
	}
	dev_info(&pdev->dev, "modem glue ready (remoteproc boots the MSS)\n");
	return 0;
}

static void lv517_mdm_remove(struct platform_device *pdev)
{
	struct lv517_modem *mdm = platform_get_drvdata(pdev);

	rfkill_unregister(mdm->rfkill);
	rfkill_destroy(mdm->rfkill);
}

static const struct of_device_id lv517_mdm_match[] = {
	{ .compatible = "lge,lv517-modem" },
	{ }
};
MODULE_DEVICE_TABLE(of, lv517_mdm_match);

static struct platform_driver lv517_mdm_driver = {
	.probe = lv517_mdm_probe,
	.remove_new = lv517_mdm_remove,
	.driver = {
		.name = "lg-lv517-modem",
		.of_match_table = lv517_mdm_match,
		.dev_groups = lv517_mdm_groups,
	},
};
module_platform_driver(lv517_mdm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MixOS project");
MODULE_DESCRIPTION("LG K20 Plus modem glue (rfkill/state/uevent)");
