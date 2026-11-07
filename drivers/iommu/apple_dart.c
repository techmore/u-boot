// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <cpu_func.h>
#include <dm.h>
#include <iommu.h>
#include <log.h>
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>
#include <lmb.h>
#include <malloc.h>
#include <memalign.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/arch/j773g-usb.h>
#include <asm/arch/track-a.h>

#define DART_PARAMS2		0x0004
#define  DART_PARAMS2_BYPASS_SUPPORT	BIT(0)

#define DART_T8020_TLB_CMD		0x0020
#define  DART_T8020_TLB_CMD_FLUSH		BIT(20)
#define  DART_T8020_TLB_CMD_BUSY		BIT(2)
#define DART_T8020_TLB_SIDMASK		0x0034
#define DART_T8020_ERROR		0x0040
#define DART_T8020_ERROR_ADDR_LO	0x0050
#define DART_T8020_ERROR_ADDR_HI	0x0054
#define DART_T8020_CONFIG		0x0060
#define  DART_T8020_CONFIG_LOCK			BIT(15)
#define DART_T8020_SID_ENABLE		0x00fc
#define DART_T8020_TCR_BASE		0x0100
#define  DART_T8020_TCR_TRANSLATE_ENABLE	BIT(7)
#define  DART_T8020_TCR_BYPASS_DART		BIT(8)
#define  DART_T8020_TCR_BYPASS_DAPF		BIT(12)
#define DART_T8020_TTBR_BASE		0x0200
#define  DART_T8020_TTBR_VALID			BIT(31)
#define DART_T8020_BUSY_TIMEOUT_US	100

#define DART_T8110_PARAMS3		0x0008
#define  DART_T8110_PARAMS3_VERSION		GENMASK(15, 0)
#define DART_T8110_PARAMS4		0x000c
#define  DART_T8110_PARAMS4_NSID_MASK		(0x1ff << 0)
/*
 * The TLB command takes an operation in bits 10:8 and a stream in bits 7:0
 * (Linux apple-dart.c, m1n1 dart.c).  Both only ever use FLUSH_SID, per
 * stream, with the v2 flag on DART v2.2+; so does this driver.
 */
#define DART_T8110_TLB_CMD		0x0080
#define  DART_T8110_TLB_CMD_BUSY		BIT(31)
#define  DART_T8110_TLB_CMD_V2			BIT(15)
#define  DART_T8110_TLB_CMD_OP			GENMASK(10, 8)
#define  DART_T8110_TLB_CMD_OP_FLUSH_SID	1
#define  DART_T8110_TLB_CMD_STREAM		GENMASK(7, 0)
#define DART_T8110_BUSY_TIMEOUT_US	2000
#define DART_T8110_ERROR		0x0100
#define DART_T8110_ERROR_MASK		0x0104
#define DART_T8110_ERROR_ADDR_LO	0x0170
#define DART_T8110_ERROR_ADDR_HI	0x0174
#define DART_T8110_PROTECT		0x0200
#define  DART_T8110_PROTECT_TTBR_TCR		BIT(0)
#define DART_T8110_SID_ENABLE_BASE	0x0c00
#define DART_T8110_TCR_BASE		0x1000
#define  DART_T8110_TCR_BYPASS_DAPF		BIT(2)
#define  DART_T8110_TCR_BYPASS_DART		BIT(1)
#define  DART_T8110_TCR_TRANSLATE_ENABLE	BIT(0)
#define DART_T8110_TTBR_BASE		0x1400
#define  DART_T8110_TTBR_VALID			BIT(0)

#define DART_MAX_SID		256

#define DART_SID_ENABLE(priv, idx) \
	((priv)->sid_enable_base + 4 * (idx))
#define DART_TCR(priv, sid)	((priv)->tcr_base + 4 * (sid))
#define DART_TTBR(priv, sid, idx)	\
	((priv)->ttbr_base + 4 * (priv)->nttbr * (sid) + 4 * (idx))
#define  DART_TTBR_SHIFT	12

#define DART_PAGE_SIZE		SZ_16K
#define DART_PAGE_MASK		(DART_PAGE_SIZE - 1)

#define DART_L1_TABLE		0x3
#define DART_L2_INVAL		0
#define DART_L2_VALID		BIT(0)
#define DART_L2_FULL_PAGE	BIT(1)
#define DART_L2_START(addr)	((((addr) & DART_PAGE_MASK) >> 2) << 52)
#define DART_L2_END(addr)	((((addr) & DART_PAGE_MASK) >> 2) << 40)

/*
 * TEST ONLY (J773g boot-menu track A): on the J773g U-Boot may drive
 * only the two DARTs of the front ATC2 USB controller, each with the 0xc000
 * window the ADT pmap-io-ranges give it and the atc2_usb power domain (the
 * shared J773g USB policy, asm/arch/j773g-usb.h), and only for the stream
 * the controller's "iommus" names.  A TLB invalidation that does not finish
 * on these two is not contained (a stale translation could alias reused
 * memory), so it stops the boot: see apple_dart_fail().  Every other DART on the machine (rear ATC
 * ports, display, PCIe, ...) refuses to bind, so no DT can make U-Boot touch
 * it.  The pair shares one set of page tables: the controller is connected to
 * both, while the IOMMU uclass maps through the last one only.
 */
DECLARE_GLOBAL_DATA_PTR;

struct apple_dart_policy {
	phys_addr_t base;
	fdt_size_t size;
	int sid;
};

static const struct apple_dart_policy apple_dart_t8132_allowed[] = {
	{ 0x412f00000, 0xc000, 0 },	/* dart-usb2 reg[0], usb@412280000 */
	{ 0x412f80000, 0xc000, 1 },	/* dart-usb2 reg[1], usb@412280000 */
};

struct apple_dart_tables {
	u64 *l1, *l2;
	int nl1;
	struct lmb io_lmb;
	int users;
};

struct apple_dart_priv {
	void *base;
	struct apple_dart_tables *tbl;
	struct apple_dart_tables own;
	struct apple_dart_priv *peer;
	const struct apple_dart_policy *policy;
	int bypass, shift;

	dma_addr_t dvabase;
	dma_addr_t dvaend;

	int nsid;
	int nttbr;
	int sid_enable_base;
	int tcr_base;
	u32 tcr_translate_enable;
	u32 tcr_bypass;
	int ttbr_base;
	u32 ttbr_valid;
	bool t8110, v2;
	u32 owned[DART_MAX_SID / 32];
	int err;
};

static struct apple_dart_tables apple_dart_t8132_tables;
static struct apple_dart_priv *apple_dart_t8132_darts[2];

/* J773g only: other T8132 machines (J713) use DARTs for their own handoffs. */
static bool apple_dart_t8132_machine(void)
{
	return ofnode_device_is_compatible(ofnode_root(), "apple,j773g");
}

static const struct apple_dart_policy *apple_dart_lookup(struct udevice *dev)
{
	const char *why;
	int sid;

	if (j773g_usb_node_allowed(gd->fdt_blob, dev_of_offset(dev),
				   J773G_USB_DART, &sid, &why)) {
		log_debug("%s: refused: %s\n", dev->name, why);
		return NULL;
	}

	return &apple_dart_t8132_allowed[sid];
}

/*
 * A failed invalidation leaves translations the DART may still use.  On the
 * J773g USB DARTs nothing may run on after that: no mapping, no IOVA or
 * memory reuse, no OS handoff.  panic() resets the machine (PANIC_HANG is
 * off; the watchdog is the reset path) before Linux.  Other DARTs keep the
 * old behaviour: the error is logged, latched and returned.
 */
static void apple_dart_fail(struct apple_dart_priv *priv, const char *what, int ret)
{
	if (!priv->err)
		priv->err = ret;
	m4_track_a_note("dart %p %s failed %d", priv->base, what, ret);
	if (priv->policy)
		panic("apple_dart %p: %s failed (%d); stopping before DMA memory is reused or handed on\n",
		      priv->base, what, ret);
	log_err("apple_dart %p: %s failed (%d)\n", priv->base, what, ret);
}

static bool apple_dart_owns(struct apple_dart_priv *priv, int sid)
{
	return priv->owned[sid / 32] & BIT(sid % 32);
}

static int apple_dart_t8020_flush_tlb(struct apple_dart_priv *priv)
{
	u32 val;

	writel(priv->owned[0], priv->base + DART_T8020_TLB_SIDMASK);
	writel(DART_T8020_TLB_CMD_FLUSH, priv->base + DART_T8020_TLB_CMD);
	return readl_poll_timeout(priv->base + DART_T8020_TLB_CMD, val,
				  !(val & DART_T8020_TLB_CMD_BUSY),
				  DART_T8020_BUSY_TIMEOUT_US);
}

static int apple_dart_t8110_flush_sid(struct apple_dart_priv *priv, int sid)
{
	u32 cmd, val;
	int ret;

	cmd = FIELD_PREP(DART_T8110_TLB_CMD_OP, DART_T8110_TLB_CMD_OP_FLUSH_SID) |
	      FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid);
	if (priv->v2)
		cmd |= DART_T8110_TLB_CMD_V2;

	ret = readl_poll_timeout(priv->base + DART_T8110_TLB_CMD, val,
				 !(val & DART_T8110_TLB_CMD_BUSY),
				 DART_T8110_BUSY_TIMEOUT_US);
	if (ret)
		return ret;
	writel(cmd, priv->base + DART_T8110_TLB_CMD);
	return readl_poll_timeout(priv->base + DART_T8110_TLB_CMD, val,
				  !(val & DART_T8110_TLB_CMD_BUSY),
				  DART_T8110_BUSY_TIMEOUT_US);
}

/* Invalidate the streams this driver owns; a timeout is latched in priv->err. */
static int apple_dart_flush_one(struct apple_dart_priv *priv)
{
	int sid, ret = 0;

	dsb();

	if (!priv->t8110) {
		ret = apple_dart_t8020_flush_tlb(priv);
	} else {
		for (sid = 0; sid < priv->nsid && !ret; sid++) {
			if (apple_dart_owns(priv, sid))
				ret = apple_dart_t8110_flush_sid(priv, sid);
		}
	}

	if (ret)
		apple_dart_fail(priv, "TLB flush", ret);

	return ret;
}

static int apple_dart_flush_tlb(struct apple_dart_priv *priv)
{
	int ret = apple_dart_flush_one(priv);

	if (priv->peer) {
		int pret = apple_dart_flush_one(priv->peer);

		ret = ret ?: pret;
	}

	return ret;
}

static dma_addr_t apple_dart_map(struct udevice *dev, void *addr, size_t size)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	struct apple_dart_tables *tbl = priv->tbl;
	phys_addr_t paddr, dva;
	phys_size_t psize, off;
	int i, idx;

	if (priv->bypass)
		return (phys_addr_t)addr;

	paddr = ALIGN_DOWN((phys_addr_t)addr, DART_PAGE_SIZE);
	off = (phys_addr_t)addr - paddr;
	psize = ALIGN(size + off, DART_PAGE_SIZE);

	dva = io_lmb_alloc(&tbl->io_lmb, psize, DART_PAGE_SIZE);
	if (!dva) {
		/* DVA 0 is never mapped: a DMA to it faults in the DART. */
		apple_dart_fail(priv, "IOVA allocation", -ENOMEM);
		return 0;
	}

	idx = dva / DART_PAGE_SIZE;
	for (i = 0; i < psize / DART_PAGE_SIZE; i++) {
		tbl->l2[idx + i] = (paddr  >> priv->shift) | DART_L2_VALID |
			DART_L2_START(0LL) | DART_L2_END(~0LL);
		paddr += DART_PAGE_SIZE;
	}
	flush_dcache_range((unsigned long)&tbl->l2[idx],
			   (unsigned long)&tbl->l2[idx + i]);
	/* A timeout is logged and latched; this API has no error return. */
	apple_dart_flush_tlb(priv);

	return dva + off;
}

static void apple_dart_unmap(struct udevice *dev, dma_addr_t addr, size_t size)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	struct apple_dart_tables *tbl = priv->tbl;
	phys_addr_t dva;
	phys_size_t psize;
	int i, idx;

	if (priv->bypass)
		return;

	dva = ALIGN_DOWN(addr, DART_PAGE_SIZE);
	psize = size + (addr - dva);
	psize = ALIGN(psize, DART_PAGE_SIZE);

	idx = dva / DART_PAGE_SIZE;
	for (i = 0; i < psize / DART_PAGE_SIZE; i++)
		tbl->l2[idx + i] = DART_L2_INVAL;
	flush_dcache_range((unsigned long)&tbl->l2[idx],
			   (unsigned long)&tbl->l2[idx + i]);
	/* Only a completed invalidation lets the IOVA be reused. */
	if (apple_dart_flush_tlb(priv))
		return;

	io_lmb_free(&tbl->io_lmb, dva, psize);
}

/*
 * T8132: take one stream for a consumer.  A stream that is not in its reset
 * state (translation or bypass on, or a valid TTBR) belongs to firmware and is
 * left alone: the consumer's probe fails instead.
 */
static int apple_dart_own_sid(struct apple_dart_priv *priv, int sid)
{
	struct apple_dart_tables *tbl = priv->tbl;
	phys_addr_t addr = (phys_addr_t)tbl->l1;
	u32 tcr, ttbr;
	int i, ret;

	if (sid < 0 || sid >= priv->nsid || sid >= DART_MAX_SID)
		return -EINVAL;
	if (apple_dart_owns(priv, sid))
		return 0;
	if (!tbl->l1)
		return -EINVAL;

	tcr = readl(priv->base + DART_TCR(priv, sid));
	ttbr = readl(priv->base + DART_TTBR(priv, sid, 0));
	if (tcr || (ttbr & priv->ttbr_valid)) {
		log_err("%s: stream %d has firmware state (tcr %x ttbr %x)\n",
			__func__, sid, tcr, ttbr);
		m4_track_a_note("dart %p sid%d busy tcr=%x ttbr=%x",
				priv->base, sid, tcr, ttbr);
		return -EBUSY;
	}

	for (i = 0; i < tbl->nl1; i++) {
		writel(addr >> DART_TTBR_SHIFT | priv->ttbr_valid,
		       priv->base + DART_TTBR(priv, sid, i));
		addr += DART_PAGE_SIZE;
	}
	priv->owned[sid / 32] |= BIT(sid % 32);

	/* A timeout does not return on the J773g USB DARTs (apple_dart_fail). */
	ret = apple_dart_flush_one(priv);
	if (ret)
		return ret;

	/* T8110 stream enables are write-one-to-set. */
	writel(BIT(sid % 32), priv->base + DART_SID_ENABLE(priv, sid / 32));
	writel(priv->tcr_translate_enable, priv->base + DART_TCR(priv, sid));
	m4_track_a_note("dart %p sid%d on v2=%d", priv->base, sid, priv->v2);

	return 0;
}

static int apple_dart_connect(struct udevice *dev)
{
	struct udevice *iommu = dev->iommu;
	struct apple_dart_priv *priv = dev_get_priv(iommu);
	struct ofnode_phandle_args args;
	int i, count, ret;

	if (!priv->policy)
		return 0;	/* everything was set up at probe */

	count = dev_count_phandle_with_args(dev, "iommus", "#iommu-cells", 0);
	for (i = 0; i < count; i++) {
		ret = dev_read_phandle_with_args(dev, "iommus", "#iommu-cells",
						 0, i, &args);
		if (ret)
			return ret;
		if (!ofnode_equal(args.node, dev_ofnode(iommu)))
			continue;
		if (args.args_count != 1 || args.args[0] != priv->policy->sid) {
			log_err("%s: %s: stream not allowed by the T8132 policy\n",
				__func__, dev->name);
			return -EPERM;
		}
		ret = apple_dart_own_sid(priv, args.args[0]);
		if (ret)
			return ret;
	}

	return 0;
}

static struct iommu_ops apple_dart_ops = {
	.connect = apple_dart_connect,
	.map = apple_dart_map,
	.unmap = apple_dart_unmap,
};

static int apple_dart_alloc_tables(struct apple_dart_priv *priv)
{
	struct apple_dart_tables *tbl = priv->tbl;
	phys_addr_t l2;
	int ntte, nl2, i, ret;

	/* Count a user only once the tables exist (shared by the T8132 pair). */
	if (tbl->users) {
		tbl->users++;
		return 0;
	}

	ntte = DIV_ROUND_UP(priv->dvaend, DART_PAGE_SIZE);
	nl2 = DIV_ROUND_UP(ntte, DART_PAGE_SIZE / sizeof(u64));
	tbl->nl1 = DIV_ROUND_UP(nl2, DART_PAGE_SIZE / sizeof(u64));
	if (tbl->nl1 > priv->nttbr)
		return -EINVAL;

	ret = io_lmb_setup(&tbl->io_lmb);
	if (ret)
		return ret;
	if (io_lmb_add(&tbl->io_lmb, priv->dvabase,
		       priv->dvaend - priv->dvabase)) {
		io_lmb_teardown(&tbl->io_lmb);
		return -EINVAL;
	}

	tbl->l2 = memalign(DART_PAGE_SIZE, nl2 * DART_PAGE_SIZE);
	tbl->l1 = memalign(DART_PAGE_SIZE, tbl->nl1 * DART_PAGE_SIZE);
	if (!tbl->l1 || !tbl->l2) {
		free(tbl->l1);
		free(tbl->l2);
		tbl->l1 = NULL;
		tbl->l2 = NULL;
		io_lmb_teardown(&tbl->io_lmb);
		return -ENOMEM;
	}
	memset(tbl->l2, 0, nl2 * DART_PAGE_SIZE);
	flush_dcache_range((unsigned long)tbl->l2,
			   (unsigned long)tbl->l2 + nl2 * DART_PAGE_SIZE);

	memset(tbl->l1, 0, tbl->nl1 * DART_PAGE_SIZE);
	l2 = (phys_addr_t)tbl->l2;
	for (i = 0; i < nl2; i++) {
		tbl->l1[i] = (l2 >> priv->shift) | DART_L1_TABLE;
		l2 += DART_PAGE_SIZE;
	}
	flush_dcache_range((unsigned long)tbl->l1,
			   (unsigned long)tbl->l1 + tbl->nl1 * DART_PAGE_SIZE);

	tbl->users = 1;
	return 0;
}

static int apple_dart_bind(struct udevice *dev)
{
	if (apple_dart_t8132_machine() && !apple_dart_lookup(dev))
		return -ENODEV;	/* refuse: not on the T8132 allowlist */

	return 0;
}

static int apple_dart_probe(struct udevice *dev)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	dma_addr_t addr;
	int ret, sid, i;
	u32 params2, params4;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	if (device_is_compatible(dev, "apple,t8110-dart")) {
		priv->t8110 = true;
		params4 = readl(priv->base + DART_T8110_PARAMS4);
		priv->nsid = params4 & DART_T8110_PARAMS4_NSID_MASK;
		priv->nttbr = 1;
		priv->sid_enable_base = DART_T8110_SID_ENABLE_BASE;
		priv->tcr_base = DART_T8110_TCR_BASE;
		priv->tcr_translate_enable = DART_T8110_TCR_TRANSLATE_ENABLE;
		priv->tcr_bypass =
		    DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART;
		priv->ttbr_base = DART_T8110_TTBR_BASE;
		priv->ttbr_valid = DART_T8110_TTBR_VALID;
		priv->v2 = FIELD_GET(DART_T8110_PARAMS3_VERSION,
				     readl(priv->base + DART_T8110_PARAMS3)) >= 0x0202;

		/* Firmware locked TTBR/TCR: never write them. */
		if (readl(priv->base + DART_T8110_PROTECT) &
		    DART_T8110_PROTECT_TTBR_TCR) {
			log_err("%s: %s is locked\n", __func__, dev->name);
			m4_track_a_note("dart %p locked", priv->base);
			return -EACCES;
		}
	} else {
		priv->nsid = 16;
		priv->nttbr = 4;
		priv->sid_enable_base = DART_T8020_SID_ENABLE;
		priv->tcr_base = DART_T8020_TCR_BASE;
		priv->tcr_translate_enable = DART_T8020_TCR_TRANSLATE_ENABLE;
		priv->tcr_bypass =
		    DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART;
		priv->ttbr_base = DART_T8020_TTBR_BASE;
		priv->ttbr_valid = DART_T8020_TTBR_VALID;
	}
	if (priv->nsid <= 0 || priv->nsid > DART_MAX_SID)
		return -EINVAL;

	if (device_is_compatible(dev, "apple,t6000-dart") ||
	    device_is_compatible(dev, "apple,t8110-dart"))
		priv->shift = 4;

	priv->dvabase = DART_PAGE_SIZE;
	priv->dvaend = SZ_4G - DART_PAGE_SIZE;

	if (apple_dart_t8132_machine()) {
		/* Translation only, shared tables, streams taken at connect. */
		priv->policy = apple_dart_lookup(dev);
		if (!priv->policy || !priv->t8110)
			return -EPERM;
		priv->tbl = &apple_dart_t8132_tables;
		ret = apple_dart_alloc_tables(priv);
		if (ret)
			return ret;
		for (i = 0; i < ARRAY_SIZE(apple_dart_t8132_darts); i++) {
			struct apple_dart_priv *other = apple_dart_t8132_darts[i];

			if (other && other != priv) {
				priv->peer = other;
				other->peer = priv;
			}
		}
		apple_dart_t8132_darts[priv->policy->sid] = priv;
		m4_track_a_note("dart %p probe nsid=%d v2=%d", priv->base,
				priv->nsid, priv->v2);
		return 0;
	}

	/* Earlier SoCs: own every stream, as before. */
	priv->tbl = &priv->own;
	for (sid = 0; sid < priv->nsid; sid++)
		priv->owned[sid / 32] |= BIT(sid % 32);

	/* Disable translations. */
	for (sid = 0; sid < priv->nsid; sid++)
		writel(0, priv->base + DART_TCR(priv, sid));

	/* Remove page tables. */
	for (sid = 0; sid < priv->nsid; sid++) {
		for (i = 0; i < priv->nttbr; i++)
			writel(0, priv->base + DART_TTBR(priv, sid, i));
	}
	ret = apple_dart_flush_one(priv);
	if (ret)
		return ret;

	params2 = readl(priv->base + DART_PARAMS2);
	if (params2 & DART_PARAMS2_BYPASS_SUPPORT) {
		for (sid = 0; sid < priv->nsid; sid++) {
			writel(priv->tcr_bypass,
			       priv->base + DART_TCR(priv, sid));
		}
		priv->bypass = 1;
		return 0;
	}

	ret = apple_dart_alloc_tables(priv);
	if (ret)
		return ret;

	/* Install page tables. */
	for (sid = 0; sid < priv->nsid; sid++) {
		addr = (phys_addr_t)priv->tbl->l1;
		for (i = 0; i < priv->tbl->nl1; i++) {
			writel(addr >> DART_TTBR_SHIFT | priv->ttbr_valid,
			       priv->base + DART_TTBR(priv, sid, i));
			addr += DART_PAGE_SIZE;
		}
	}
	ret = apple_dart_flush_one(priv);
	if (ret)
		return ret;

	/* Enable all streams. */
	for (i = 0; i < DIV_ROUND_UP(priv->nsid, 32); i++)
		writel(~0, priv->base + DART_SID_ENABLE(priv, i));

	/* Enable translations. */
	for (sid = 0; sid < priv->nsid; sid++) {
		writel(priv->tcr_translate_enable,
		       priv->base + DART_TCR(priv, sid));
	}

	return 0;
}

/*
 * OS handoff (DM_FLAG_OS_PREPARE, vital, so after DMA masters were removed):
 * turn off translation and drop the tables of the streams this driver owns,
 * nothing else.  The next owner resets the DART anyway.
 */
static int apple_dart_remove(struct udevice *dev)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	int sid, i, ret;

	/* Writes to a DART locked since probe would be ignored: tables stay live. */
	if (priv->policy && (readl(priv->base + DART_T8110_PROTECT) &
			     DART_T8110_PROTECT_TTBR_TCR))
		apple_dart_fail(priv, "teardown (DART locked)", -EACCES);

	for (sid = 0; sid < priv->nsid; sid++) {
		if (apple_dart_owns(priv, sid))
			writel(0, priv->base + DART_TCR(priv, sid));
	}
	for (sid = 0; sid < priv->nsid; sid++) {
		if (!apple_dart_owns(priv, sid))
			continue;
		for (i = 0; i < priv->nttbr; i++)
			writel(0, priv->base + DART_TTBR(priv, sid, i));
	}
	ret = apple_dart_flush_one(priv);
	if (ret)
		return ret;	/* keep ownership and tables: nothing proved them unused */
	memset(priv->owned, 0, sizeof(priv->owned));

	if (priv->peer)
		priv->peer->peer = NULL;
	if (priv->policy)
		apple_dart_t8132_darts[priv->policy->sid] = NULL;
	if (priv->tbl && priv->tbl->users && !--priv->tbl->users)
		io_lmb_teardown(&priv->tbl->io_lmb);

	return 0;
}

static const struct udevice_id apple_dart_ids[] = {
	{ .compatible = "apple,t8103-dart" },
	{ .compatible = "apple,t6000-dart" },
	{ .compatible = "apple,t8110-dart" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_dart) = {
	.name = "apple_dart",
	.id = UCLASS_IOMMU,
	.of_match = apple_dart_ids,
	.priv_auto = sizeof(struct apple_dart_priv),
	.ops = &apple_dart_ops,
	.bind = apple_dart_bind,
	.probe = apple_dart_probe,
	.remove = apple_dart_remove,
	.flags	= DM_FLAG_OS_PREPARE | DM_FLAG_VITAL
};
