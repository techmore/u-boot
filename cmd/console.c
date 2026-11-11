// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

/*
 * Boot support
 */
#include <command.h>
#include <env.h>
#include <iomux.h>
#include <mapmem.h>
#include <membuf.h>
#include <stdio_dev.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

extern void _do_coninfo (void);
static int do_coninfo(struct cmd_tbl *cmd, int flag, int argc,
		      char *const argv[])
{
	int l;
	struct list_head *list = stdio_get_list();
	struct list_head *pos;
	struct stdio_dev *dev;

	/* Scan for valid output and input devices */

	puts("List of available devices\n");

	list_for_each(pos, list) {
		dev = list_entry(pos, struct stdio_dev, list);

		printf("|-- %s (%s%s)\n",
		       dev->name,
		       (dev->flags & DEV_FLAGS_INPUT) ? "I" : "",
		       (dev->flags & DEV_FLAGS_OUTPUT) ? "O" : "");

		for (l = 0; l < MAX_FILES; l++) {
			if (CONFIG_IS_ENABLED(CONSOLE_MUX)) {
				if (iomux_match_device(console_devices[l],
						       cd_count[l], dev) >= 0)
					printf("|   |-- %s\n", stdio_names[l]);
			} else {
				if (stdio_devices[l] == dev)
					printf("|   |-- %s\n", stdio_names[l]);
			}

		}
	}
	return 0;
}

/***************************************************/

U_BOOT_CMD(
	coninfo,	3,	1,	do_coninfo,
	"print console devices and information",
	""
);

#ifdef CONFIG_CONSOLE_RECORD
/* TEST ONLY: copy the recorded console to RAM for a subsequent fatwrite. */
static int do_consoledump(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	ulong addr;
	void *dst;
	int len;

	if (argc != 2)
		return CMD_RET_USAGE;

	addr = hextoul(argv[1], NULL);
	if (!membuf_makecontig(&gd->console_out))
		return CMD_RET_FAILURE;
	len = membuf_avail(&gd->console_out);
	dst = map_sysmem(addr, len);
	memcpy(dst, gd->console_out.start, len);
	unmap_sysmem(dst);
	env_set_hex("filesize", len);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	consoledump, 2, 0, do_consoledump,
	"TEST ONLY: copy recorded console to memory and set filesize",
	"addr"
);
#endif
