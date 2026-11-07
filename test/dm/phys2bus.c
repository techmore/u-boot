// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2020 Nicolas Saenz Julienne <nsaenzjulienne@suse.de>
 */

#include <dm.h>
#include <mapmem.h>
#include <phys2bus.h>
#include <dm/device.h>
#include <dm/ofnode.h>
#include <dm/root.h>
#include <dm/test.h>
#include <dm/uclass-internal.h>
#include <test/ut.h>

static int dm_test_phys_to_bus(struct unit_test_state *uts)
{
	struct udevice *dev, *pdev;
	ofnode psave, nsave;
	ofnode node;

	node = ofnode_path("/mmio-bus@0");
	ut_assert(ofnode_valid(node));
	ut_assertok(uclass_get_device_by_ofnode(UCLASS_TEST_BUS, node, &dev));
	/* In this case it should be transparent, no dma-ranges in parent bus */
	ut_asserteq_addr((void*)0xfffffULL, (void*)dev_phys_to_bus(dev, 0xfffff));
	ut_asserteq_addr((void*)0xfffffULL, (void*)(ulong)dev_bus_to_phys(dev, 0xfffff));

	node = ofnode_path("/mmio-bus@0/subnode@0");
	ut_assert(ofnode_valid(node));
	ut_assertok(uclass_get_device_by_ofnode(UCLASS_TEST_FDT, node, &dev));
	ut_asserteq_addr((void*)0x1003ffffULL, (void*)dev_phys_to_bus(dev, 0x3ffff));
	ut_asserteq_addr((void*)0x3ffffULL, (void*)(ulong)dev_bus_to_phys(dev, 0x1003ffff));

#if CONFIG_IS_ENABLED(OF_REAL)
	/* dma-ranges are on the grandparent bus node,
	 *  OF nodes neither here nor at parent
	 */
	node = ofnode_path("/mmio-bus@0/bus@7");
	ut_assert(ofnode_valid(node));
	ut_assertok(uclass_get_device_by_ofnode(UCLASS_TEST_BUS, node, &pdev));

	/* pretend the devices were discovered dynamically and have no OF node */
	psave = pdev->node_;
	pdev->node_.np = NULL;
	node = ofnode_path("/mmio-bus@0/bus@7/subnode@8");
	ut_assert(ofnode_valid(node));
	nsave = dev->node_;
	dev->node_.np = NULL;
	ut_assertok(uclass_get_device_by_ofnode(UCLASS_TEST_FDT, node, &dev));

	ut_asserteq_addr((void *)0x1003ffffULL, (void *)dev_phys_to_bus(dev, 0x3ffff));
	ut_asserteq_addr((void *)0x3ffffULL, (void *)(ulong)dev_bus_to_phys(dev, 0x1003ffff));

	pdev->node_ = psave;
	dev->node_ = nsave;
#endif
	return 0;
}
DM_TEST(dm_test_phys_to_bus, UTF_SCAN_PDATA | UTF_SCAN_FDT);
