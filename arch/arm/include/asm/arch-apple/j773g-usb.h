/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * TEST ONLY: J773g (M4 Mac mini) boot-menu track A.  The one USB path U-Boot
 * may drive on this machine: the front ATC2 dwc3 at 0x412280000, its
 * USB2-only ATC PHY, its two dart-usb2 instances and the ATC2_USB power
 * domain, with exactly the references the U1 DT gives them.  Every other
 * dwc3, ATC PHY or DART node refuses to bind on J773g, whichever compatible
 * (t8132, t8103, snps,dwc3, ...) matched it.  Pure libfdt, so the same code
 * is unit-tested on the host against the U1 DTB and mutated copies.
 */
#ifndef __ASM_ARCH_APPLE_J773G_USB_H
#define __ASM_ARCH_APPLE_J773G_USB_H

enum j773g_usb_kind {
	J773G_USB_DWC3,
	J773G_USB_ATCPHY,
	J773G_USB_DART,
};

#define J773G_ATC2_DWC3		0x412280000ULL
#define J773G_ATC2_DWC3_SIZE	0xcd00ULL
#define J773G_ATC2_USB2PHY	0x412a90000ULL
#define J773G_ATC2_PIPEHANDLER	0x412a84000ULL
#define J773G_ATC_BANK_SIZE	0x4000ULL
#define J773G_DART_USB2_0	0x412f00000ULL
#define J773G_DART_USB2_1	0x412f80000ULL
#define J773G_DART_SIZE		0xc000ULL
#define J773G_PMGR_ATC2		0x388280000ULL
#define J773G_PS_ATC2_USB	0xe0
#define J773G_PHY_TYPE_USB2	3

/*
 * Return 0 if @node may bind as @kind, else a negative error; *why (if not
 * NULL) names the first check that failed.  For a DART, *sid (if not NULL)
 * gets the stream the controller uses on it.
 */
int j773g_usb_node_allowed(const void *fdt, int node, enum j773g_usb_kind kind,
			   int *sid, const char **why);

/* Front hub setup (track A3): nothing the policy admits may probe unless OK. */
enum j773g_usb_hub {
	J773G_USB_HUB_PENDING,
	J773G_USB_HUB_OK,
	J773G_USB_HUB_BLOCKED,
};

/*
 * The single gate: 0 if @node may probe, -EPERM if the policy admits it (as a
 * dwc3, ATC PHY or DART, whatever its name or path) and @hub is not OK,
 * -EINVAL if the node cannot be looked up (callers refuse then too).
 */
int j773g_usb_probe_gate(const void *fdt, int node, enum j773g_usb_hub hub);

/* The two front-hub lines, in the order A3 drives them. */
#define J773G_HUB_LINES		2
#define J773G_HUB_FLASH_OE_PIN	101
#define J773G_HUB_FLASH_OE_NAME	"usb3hub-flash-oe"
#define J773G_HUB_RESET_PIN	198
#define J773G_HUB_RESET_NAME	"usb3hub-reset"

/*
 * Check every gpio-hog under the GPIO controller @pctl (fail closed): each must
 * parse completely (#gpio-cells 2; gpios = exactly one specifier; exactly one
 * of input / output-low / output-high; line-name, if any, one string).  A hog
 * naming a hub pin or a hub line must be exactly that line, active-high,
 * output-high, and appear once; both hub lines must be present.  0 and @pins
 * (flash-OE, reset) on success, else -EINVAL and *why.
 */
int j773g_hub_hogs_check(const void *fdt, int pctl, uint32_t pins[J773G_HUB_LINES],
			 const char **why);

#ifndef J773G_POLICY_HOST
enum j773g_usb_hub j773g_usb_hub_state(void);
#endif

#endif
