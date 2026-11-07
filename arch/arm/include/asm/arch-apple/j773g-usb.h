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

#endif
