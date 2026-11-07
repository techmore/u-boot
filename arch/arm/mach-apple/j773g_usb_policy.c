// SPDX-License-Identifier: GPL-2.0+
/*
 * TEST ONLY: J773g boot-menu track A, the front-USB binding policy.
 * See asm/arch/j773g-usb.h.
 */

#ifdef J773G_POLICY_HOST
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <libfdt.h>
#include "j773g-usb.h"
#else
#include <errno.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <asm/arch/j773g-usb.h>
#endif

#define REFUSE(msg) do { if (why) *why = (msg); return -ENODEV; } while (0)

static uint32_t cell(const void *p, int i)
{
	return fdt32_to_cpu(((const fdt32_t *)p)[i]);
}

/* reg entry @i of a node under a 2/2-cell parent */
static int reg2(const void *fdt, int node, int i, uint64_t *addr, uint64_t *size)
{
	int parent = fdt_parent_offset(fdt, node), len;
	const void *reg;

	if (parent < 0 || fdt_address_cells(fdt, parent) != 2 ||
	    fdt_size_cells(fdt, parent) != 2)
		return -EINVAL;
	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || len % 16 || (i + 1) * 16 > len)
		return -EINVAL;
	*addr = ((uint64_t)cell(reg, 4 * i) << 32) | cell(reg, 4 * i + 1);
	*size = ((uint64_t)cell(reg, 4 * i + 2) << 32) | cell(reg, 4 * i + 3);
	return 0;
}

static int nregs(const void *fdt, int node)
{
	int len;

	return fdt_getprop(fdt, node, "reg", &len) ? len / 16 : 0;
}

static bool reg_is(const void *fdt, int node, int i, uint64_t addr, uint64_t size)
{
	uint64_t a, s;

	return !reg2(fdt, node, i, &a, &s) && a == addr && s == size;
}

static bool named_reg_is(const void *fdt, int node, const char *name,
			 uint64_t addr, uint64_t size)
{
	int i = fdt_stringlist_search(fdt, node, "reg-names", name);

	return i >= 0 && reg_is(fdt, node, i, addr, size);
}

/* The node a phandle cell points to, or -1 */
static int by_phandle(const void *fdt, uint32_t ph)
{
	return ph ? fdt_node_offset_by_phandle(fdt, ph) : -1;
}

static bool is_atc2_pd(const void *fdt, int pd)
{
	int parent = pd >= 0 ? fdt_parent_offset(fdt, pd) : -1, len;
	const void *reg;

	if (parent < 0 || fdt_address_cells(fdt, parent) != 1)
		return false;
	reg = fdt_getprop(fdt, pd, "reg", &len);
	return reg && len >= 4 && cell(reg, 0) == J773G_PS_ATC2_USB &&
	       nregs(fdt, parent) >= 1 &&
	       reg_is(fdt, parent, 0, J773G_PMGR_ATC2, 0x4000);
}

/* power-domains = <&atc2_usb> and nothing else */
static bool pd_ok(const void *fdt, int node)
{
	int len;
	const void *p = fdt_getprop(fdt, node, "power-domains", &len);

	return p && len == 4 && is_atc2_pd(fdt, by_phandle(fdt, cell(p, 0)));
}

static bool is_atc2_phy(const void *fdt, int node)
{
	return node >= 0 && nregs(fdt, node) == 2 &&
	       named_reg_is(fdt, node, "usb2phy", J773G_ATC2_USB2PHY, J773G_ATC_BANK_SIZE) &&
	       named_reg_is(fdt, node, "pipehandler", J773G_ATC2_PIPEHANDLER,
			    J773G_ATC_BANK_SIZE) &&
	       pd_ok(fdt, node);
}

/* dart-usb2 instance @idx (0 or 1), or -1 */
static int dart_index(const void *fdt, int node)
{
	if (node < 0 || nregs(fdt, node) != 1 || !pd_ok(fdt, node))
		return -1;
	if (reg_is(fdt, node, 0, J773G_DART_USB2_0, J773G_DART_SIZE))
		return 0;
	if (reg_is(fdt, node, 0, J773G_DART_USB2_1, J773G_DART_SIZE))
		return 1;
	return -1;
}

static int dwc3_allowed(const void *fdt, int node, const char **why)
{
	const void *p;
	const char *s;
	int len;

	if (!reg_is(fdt, node, 0, J773G_ATC2_DWC3, J773G_ATC2_DWC3_SIZE))
		REFUSE("dwc3: not the ATC2 controller");
	s = fdt_getprop(fdt, node, "dr_mode", &len);
	if (!s || strcmp(s, "host"))
		REFUSE("dwc3: dr_mode is not host");
	/* iommus = <&dart-usb2[0] 0>, <&dart-usb2[1] 1> */
	p = fdt_getprop(fdt, node, "iommus", &len);
	if (!p || len != 16 ||
	    dart_index(fdt, by_phandle(fdt, cell(p, 0))) != 0 || cell(p, 1) != 0 ||
	    dart_index(fdt, by_phandle(fdt, cell(p, 2))) != 1 || cell(p, 3) != 1)
		REFUSE("dwc3: iommus are not dart-usb2 sid 0 and 1");
	/* phys = <&atc2-phy PHY_TYPE_USB2> */
	p = fdt_getprop(fdt, node, "phys", &len);
	if (!p || len != 8 || !is_atc2_phy(fdt, by_phandle(fdt, cell(p, 0))) ||
	    cell(p, 1) != J773G_PHY_TYPE_USB2)
		REFUSE("dwc3: phys is not the ATC2 USB2 PHY");
	/* resets = <&atc2-phy> */
	p = fdt_getprop(fdt, node, "resets", &len);
	if (!p || len != 4 || !is_atc2_phy(fdt, by_phandle(fdt, cell(p, 0))))
		REFUSE("dwc3: resets is not the ATC2 PHY");
	if (!pd_ok(fdt, node))
		REFUSE("dwc3: power-domains is not atc2_usb");

	return 0;
}

int j773g_usb_node_allowed(const void *fdt, int node, enum j773g_usb_kind kind,
			   int *sid, const char **why)
{
	int idx;

	if (why)
		*why = NULL;
	if (!fdt || node < 0)
		REFUSE("no node");

	switch (kind) {
	case J773G_USB_DWC3:
		return dwc3_allowed(fdt, node, why);
	case J773G_USB_ATCPHY:
		if (!is_atc2_phy(fdt, node))
			REFUSE("atcphy: not the ATC2 USB2-only PHY");
		return 0;
	case J773G_USB_DART:
		idx = dart_index(fdt, node);
		if (idx < 0)
			REFUSE("dart: not a dart-usb2 instance");
		if (sid)
			*sid = idx;	/* dart-usb2[0] carries sid 0, [1] sid 1 */
		return 0;
	}

	REFUSE("unknown kind");
}
