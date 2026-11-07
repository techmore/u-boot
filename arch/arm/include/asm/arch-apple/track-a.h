/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * TEST ONLY: J773g (M4 Mac mini) boot-menu track A instrumentation.
 *
 * Track-A code appends short notes here (DART, ATC PHY, hub GPIO); the board
 * code publishes them, the stdin devices and the USB tree as /chosen
 * properties of the device tree handed to the OS.  Linux reads them back at
 * /proc/device-tree/chosen/u-boot,m4-*.
 */
#ifndef __ASM_ARCH_APPLE_TRACK_A_H
#define __ASM_ARCH_APPLE_TRACK_A_H

/* Identifies the U-Boot variant in the evidence; bumped by each track-A step. */
#define M4_TRACK_A_TAG	"A3"

#if IS_ENABLED(CONFIG_ARCH_APPLE)
void m4_track_a_note(const char *fmt, ...)
	__attribute__ ((format (__printf__, 1, 2)));
#else
static inline void m4_track_a_note(const char *fmt, ...) {}
#endif

#endif
