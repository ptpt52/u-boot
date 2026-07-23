/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * U-Boot CLI Command for 30+1 XOR RAID-5 Driver Layer
 *
 * Copyright (C) 2026
 */

#include <config.h>
#include <command.h>
#include <mapmem.h>
#include <mtd.h>
#include <vsprintf.h>
#include <mtd_raid301.h>

static int do_raid301_info(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mtd_info *mtd = mtd_raid301_get_dev();

	if (!mtd) {
		printf("RAID301: Device not initialized.\n");
		return CMD_RET_FAILURE;
	}
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_raid301_sub[] = {
	U_BOOT_CMD_MKENT(info, 1, 0, do_raid301_info, "", ""),
};

static int do_raid301(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct cmd_tbl *cp;

	if (argc < 2)
		return CMD_RET_USAGE;

	cp = find_cmd_tbl(argv[1], cmd_raid301_sub, ARRAY_SIZE(cmd_raid301_sub));
	if (cp)
		return cp->cmd(cmdtp, flag, argc - 1, argv + 1);

	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	raid301, 5, 1, do_raid301,
	"30+1 XOR RAID5 MTD Abstraction Management",
	"info - Show device information and status"
);
