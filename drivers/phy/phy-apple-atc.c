// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 Mark Kettenis <kettenis@openbsd.org>
 */

#include <dm.h>
#include <log.h>
#include <dm/device-internal.h>
#include <generic-phy.h>
#include <reset-uclass.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/arch/j773g-usb.h>
#include <asm/arch/track-a.h>

/*
 * TEST ONLY (J773g boot-menu track A): the T8132 ATC PHY is matched only for
 * the front ATC2 port of J773g, in its USB2-only two-bank form
 * ("usb2phy", "pipehandler"); see apple_atcphy_reset_bind().
 */

#define PIPEHANDLER_AON_GEN			0x1c
#define  PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN	BIT(4)
#define  PIPEHANDLER_AON_GEN_DWC3_RESET_N		BIT(0)

DECLARE_GLOBAL_DATA_PTR;

struct apple_atcphy_priv {
	void __iomem *usb2phy;
	void __iomem *pipehandler;
	bool t8132;
};

static const struct phy_ops apple_atcphy_ops = {
};

static struct driver apple_atcphy_driver = {
	.name = "apple-atcphy",
	.id = UCLASS_PHY,
	.ops = &apple_atcphy_ops,
};

static int apple_atcphy_reset_of_xlate(struct reset_ctl *reset_ctl,
				       struct ofnode_phandle_args *args)
{
	if (args->args_count != 0)
		return -EINVAL;

	return 0;
}

/*
 * T8132: nothing here brings the controller out of reset yet.  Let the dwc3
 * probe go on only if something before U-Boot already did, so xHCI never
 * reads registers of a core that is held in reset (read-only check).
 */
static int apple_atcphy_reset_deassert(struct reset_ctl *reset_ctl)
{
	struct apple_atcphy_priv *priv = dev_get_priv(reset_ctl->dev);
	u32 aon;

	if (!priv->t8132)
		return 0;

	aon = readl(priv->pipehandler + PIPEHANDLER_AON_GEN);
	m4_track_a_note("atc2 aon_gen=%x", aon);
	if (!(aon & PIPEHANDLER_AON_GEN_DWC3_RESET_N) ||
	    (aon & PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN))
		return -EIO;

	return 0;
}

static const struct reset_ops apple_atcphy_reset_ops = {
	.of_xlate = apple_atcphy_reset_of_xlate,
	.rst_deassert = apple_atcphy_reset_deassert,
};

/*
 * On J773g only the front ATC2 USB2-only PHY may bind, whichever compatible
 * matched (asm/arch/j773g-usb.h).  Elsewhere a node bound only through the
 * new apple,t8132-atcphy entry is refused; other nodes bind as before.
 */
#define ATCPHY_T8132	1

static int apple_atcphy_reset_bind(struct udevice *dev)
{
	const char *why;

	if (of_machine_is_compatible("apple,j773g")) {
		if (j773g_usb_node_allowed(gd->fdt_blob, dev_of_offset(dev),
					   J773G_USB_ATCPHY, NULL, &why)) {
			log_debug("%s: refused: %s\n", dev->name, why);
			return -ENODEV;
		}
		return 0;
	}
	if (dev_get_driver_data(dev) == ATCPHY_T8132 &&
	    !device_is_compatible(dev, "apple,t6000-atcphy") &&
	    !device_is_compatible(dev, "apple,t8103-atcphy") &&
	    !device_is_compatible(dev, "apple,t8122-atcphy") &&
	    !device_is_compatible(dev, "apple,t8140-atcphy"))
		return -ENODEV;

	return 0;
}

static int apple_atcphy_reset_probe(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	struct udevice *child;

	if (device_is_compatible(dev, "apple,t8132-atcphy")) {
		priv->t8132 = true;
		priv->usb2phy = dev_read_addr_name_ptr(dev, "usb2phy");
		priv->pipehandler = dev_read_addr_name_ptr(dev, "pipehandler");
		if (!priv->usb2phy || !priv->pipehandler)
			return -EINVAL;
	}

	device_bind(dev, &apple_atcphy_driver, "apple-atcphy", NULL,
		    dev_ofnode(dev), &child);

	return 0;
}

static const struct udevice_id apple_atcphy_ids[] = {
	{ .compatible = "apple,t6000-atcphy" },
	{ .compatible = "apple,t8103-atcphy" },
	{ .compatible = "apple,t8122-atcphy" },
	{ .compatible = "apple,t8132-atcphy", .data = ATCPHY_T8132 },
	{ }
};

U_BOOT_DRIVER(apple_atcphy_reset) = {
	.name = "apple-atcphy-reset",
	.id = UCLASS_RESET,
	.of_match = apple_atcphy_ids,
	.ops = &apple_atcphy_reset_ops,
	.bind = apple_atcphy_reset_bind,
	.probe = apple_atcphy_reset_probe,
	.priv_auto = sizeof(struct apple_atcphy_priv),
};
