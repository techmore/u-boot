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
#include <linux/bitfield.h>
#include <linux/delay.h>

/*
 * TEST ONLY (J773g boot-menu track A): the T8132 ATC PHY is matched only for
 * the front ATC2 port of J773g, in its USB2-only two-bank form
 * ("usb2phy", "pipehandler"); see apple_atcphy_reset_bind().
 */

/* Register layout and sequences from Linux drivers/phy/apple/atc.c (U1 0714d188). */
#define PIPEHANDLER_MUX_CTRL			0x0c
#define  PIPEHANDLER_MUX_CTRL_CLK		GENMASK(5, 3)
#define  PIPEHANDLER_MUX_CTRL_DATA		GENMASK(2, 0)
#define  PIPEHANDLER_MUX_CTRL_CLK_OFF		0
#define  PIPEHANDLER_MUX_CTRL_CLK_DUMMY		4
#define  PIPEHANDLER_MUX_CTRL_DATA_DUMMY	2
#define PIPEHANDLER_AON_GEN			0x1c
#define  PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN	BIT(4)
#define  PIPEHANDLER_AON_GEN_DWC3_RESET_N		BIT(0)
#define PIPEHANDLER_T8132_USB2_CONFIG		0x44
#define  PIPEHANDLER_T8132_USB2_CONFIG_EN		BIT(2)

#define USB2PHY_USBCTL				0x00
#define  USB2PHY_USBCTL_USB_MODE		GENMASK(2, 0)
#define  USB2PHY_USBCTL_RUN			2
#define  USB2PHY_USBCTL_ISOLATION		4
#define USB2PHY_CTL				0x04
#define  USB2PHY_CTL_RESET			BIT(0)
#define  USB2PHY_CTL_PORT_RESET			BIT(1)
#define  USB2PHY_CTL_APB_RESET_N		BIT(2)
#define  USB2PHY_CTL_SIDDQ			BIT(3)
#define USB2PHY_SIG				0x08
#define  USB2PHY_SIG_VBUS_FORCE			GENMASK(3, 0)
/* ADT tunable_USB2PHY_HOST 0x7003 at 0x8; Linux U1 patch 0001 (0019) */
#define  USB2PHY_SIG_HOST			GENMASK(14, 12)
#define USB2PHY_MISCTUNE			0x1c
#define  USB2PHY_MISCTUNE_APBCLK_GATE_OFF	BIT(29)
#define  USB2PHY_MISCTUNE_REFCLK_GATE_OFF	BIT(30)

DECLARE_GLOBAL_DATA_PTR;

struct apple_atcphy_priv {
	void __iomem *usb2phy;
	void __iomem *pipehandler;
	bool t8132;
};

static void atc_mask32(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

#define atc_set32(reg, set)	atc_mask32(reg, 0, set)
#define atc_clear32(reg, clr)	atc_mask32(reg, clr, 0)

static void atc_t8132_dwc3_reset_assert(struct apple_atcphy_priv *priv)
{
	atc_clear32(priv->pipehandler + PIPEHANDLER_AON_GEN,
		    PIPEHANDLER_AON_GEN_DWC3_RESET_N);
	atc_set32(priv->pipehandler + PIPEHANDLER_AON_GEN,
		  PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
}

static void atc_t8132_usb2_power_off(struct apple_atcphy_priv *priv)
{
	void __iomem *u = priv->usb2phy;

	atc_mask32(u + USB2PHY_USBCTL, USB2PHY_USBCTL_USB_MODE,
		   USB2PHY_USBCTL_ISOLATION);
	udelay(10);
	atc_set32(u + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
	udelay(10);
	atc_set32(u + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
	udelay(10);
	atc_set32(u + USB2PHY_CTL, USB2PHY_CTL_RESET);
	udelay(10);
	atc_clear32(u + USB2PHY_CTL, USB2PHY_CTL_APB_RESET_N);
	udelay(10);
	atc_set32(u + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_APBCLK_GATE_OFF);
	atc_set32(u + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_REFCLK_GATE_OFF);
}

static void atc_t8132_usb2_power_on(struct apple_atcphy_priv *priv)
{
	void __iomem *u = priv->usb2phy;

	atc_set32(u + USB2PHY_SIG, USB2PHY_SIG_VBUS_FORCE);
	udelay(10);
	atc_clear32(u + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
	udelay(10);
	atc_clear32(u + USB2PHY_CTL, USB2PHY_CTL_RESET);
	udelay(10);
	atc_clear32(u + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
	udelay(10);
	atc_set32(u + USB2PHY_CTL, USB2PHY_CTL_APB_RESET_N);
	udelay(10);
	atc_clear32(u + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_APBCLK_GATE_OFF);
	atc_clear32(u + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_REFCLK_GATE_OFF);
	writel(USB2PHY_USBCTL_RUN, u + USB2PHY_USBCTL);
}

static void atc_t8132_mux_dummy(struct apple_atcphy_priv *priv)
{
	void __iomem *mux = priv->pipehandler + PIPEHANDLER_MUX_CTRL;

	atc_mask32(mux, PIPEHANDLER_MUX_CTRL_CLK,
		   FIELD_PREP(PIPEHANDLER_MUX_CTRL_CLK, PIPEHANDLER_MUX_CTRL_CLK_OFF));
	udelay(10);
	atc_mask32(mux, PIPEHANDLER_MUX_CTRL_DATA,
		   FIELD_PREP(PIPEHANDLER_MUX_CTRL_DATA, PIPEHANDLER_MUX_CTRL_DATA_DUMMY));
	udelay(10);
	atc_mask32(mux, PIPEHANDLER_MUX_CTRL_CLK,
		   FIELD_PREP(PIPEHANDLER_MUX_CTRL_CLK, PIPEHANDLER_MUX_CTRL_CLK_DUMMY));
	udelay(10);
}

static struct apple_atcphy_priv *atc_phy_priv(struct phy *phy)
{
	struct apple_atcphy_priv *priv = dev_get_priv(phy->dev->parent);

	return priv->t8132 ? priv : NULL;
}

static int apple_atcphy_init(struct phy *phy)
{
	struct apple_atcphy_priv *priv = atc_phy_priv(phy);

	/* Linux atcphy_usb2_init() for a fixed-hub port; the role was latched at probe. */
	if (priv) {
		atc_set32(priv->usb2phy + USB2PHY_SIG, USB2PHY_SIG_HOST);
		atc_t8132_usb2_power_on(priv);
		m4_track_a_note("atc2 init sig=%x ctl=%x usbctl=%x",
				readl(priv->usb2phy + USB2PHY_SIG),
				readl(priv->usb2phy + USB2PHY_CTL),
				readl(priv->usb2phy + USB2PHY_USBCTL));
	}

	return 0;
}

/*
 * No .exit, as in Linux: at OS handoff xhci_dwc3_remove() halts xHCI, then
 * reset_release_bulk() asserts the reset, which holds dwc3 in reset and
 * clamped and then powers USB2 off.  That is the state the Linux driver sets
 * up first at its own probe.
 */
static const struct phy_ops apple_atcphy_ops = {
	.init = apple_atcphy_init,
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

static int apple_atcphy_reset_assert(struct reset_ctl *reset_ctl)
{
	struct apple_atcphy_priv *priv = dev_get_priv(reset_ctl->dev);

	if (priv->t8132) {
		atc_t8132_dwc3_reset_assert(priv);
		atc_t8132_usb2_power_off(priv);
	}

	return 0;
}

/*
 * T8132: release the controller (the USB2 PHY was brought up at probe).  The
 * read-back makes sure xHCI never reads a core that is still held in reset.
 */
static int apple_atcphy_reset_deassert(struct reset_ctl *reset_ctl)
{
	struct apple_atcphy_priv *priv = dev_get_priv(reset_ctl->dev);
	u32 aon;

	if (!priv->t8132)
		return 0;

	atc_t8132_usb2_power_on(priv);
	atc_clear32(priv->pipehandler + PIPEHANDLER_AON_GEN,
		    PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
	atc_set32(priv->pipehandler + PIPEHANDLER_AON_GEN,
		  PIPEHANDLER_AON_GEN_DWC3_RESET_N);

	aon = readl(priv->pipehandler + PIPEHANDLER_AON_GEN);
	m4_track_a_note("atc2 aon_gen=%x", aon);
	if (!(aon & PIPEHANDLER_AON_GEN_DWC3_RESET_N) ||
	    (aon & PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN))
		return -EIO;

	return 0;
}

static const struct reset_ops apple_atcphy_reset_ops = {
	.of_xlate = apple_atcphy_reset_of_xlate,
	.rst_assert = apple_atcphy_reset_assert,
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

		/*
		 * As Linux atc.c probes a fixed-hub USB2-only port: hold dwc3
		 * in reset, clear what the bootloader left in the USB2 PHY,
		 * give the controller its dummy PIPE clock, select host while
		 * the USB2 PHY is stopped (T8132 latches the role then) and
		 * power USB2 up.  dwc3 leaves reset at deassert.
		 */
		atc_t8132_dwc3_reset_assert(priv);
		atc_t8132_usb2_power_off(priv);
		atc_set32(priv->pipehandler + PIPEHANDLER_T8132_USB2_CONFIG,
			  PIPEHANDLER_T8132_USB2_CONFIG_EN);
		atc_t8132_mux_dummy(priv);
		atc_set32(priv->usb2phy + USB2PHY_SIG, USB2PHY_SIG_HOST);
		atc_t8132_usb2_power_on(priv);
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
