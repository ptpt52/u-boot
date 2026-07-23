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
#include <linux/string.h>
#include <mtd_raid301.h>
#include "../drivers/mtd/mtd_raid301_internal.h"

static int do_raid301_attach(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const char *master_name = NULL;
	struct mtd_info *mtd;

	if (argc >= 2)
		master_name = argv[1];

	mtd = mtd_raid301_attach(master_name);
	if (!mtd)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

static int do_raid301_detach(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (mtd_raid301_detach() != 0)
		return CMD_RET_FAILURE;
	return CMD_RET_SUCCESS;
}

static int do_raid301_format(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const char *master_name;
	struct mtd_info *master;
	int ret;

	if (argc < 3) {
		printf("Usage: raid301 format <backing_mtd> --confirm\n");
		return CMD_RET_USAGE;
	}

	master_name = argv[1];
	if (strcmp(argv[2], "--confirm") != 0) {
		printf("RAID301 Error: Destructive format requires '--confirm' flag.\n");
		return CMD_RET_FAILURE;
	}

	mtd_probe_devices();
	master = get_mtd_device_nm(master_name);
	if (IS_ERR_OR_NULL(master)) {
		printf("RAID301 Format Error: Could not find MTD device '%s'\n", master_name);
		return CMD_RET_FAILURE;
	}

	/* Must detach existing virtual device prior to format */
	mtd_raid301_detach();

	ret = raid301_format_device(master);
	put_mtd_device(master);

	if (ret != 0)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

static int do_raid301_info(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mtd_info *mtd = mtd_raid301_get_dev();

	if (!mtd) {
		printf("RAID301: Device not attached. Use 'raid301 attach [mtd_name]' first.\n");
		return CMD_RET_FAILURE;
	}

	printf("\n--- MTD RAID301 Storage Status ---\n");
	printf("Virtual MTD Name   : %s\n", mtd->name);
	printf("Logical Size       : %llu Bytes (~%llu MiB)\n", mtd->size, mtd->size / (1024 * 1024));
	printf("Logical Payload    : %u Bytes\n", mtd->erasesize);
	printf("Layout Hash        : 0x%08x\n", mtd_raid301_calc_layout_hash());
	printf("----------------------------------\n\n");

	return CMD_RET_SUCCESS;
}

static int do_raid301_scrub(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mtd_info *mtd = mtd_raid301_get_dev();
	bool repair = false;

	if (!mtd) {
		printf("RAID301: Device not attached.\n");
		return CMD_RET_FAILURE;
	}

	if (argc >= 2 && strcmp(argv[1], "--repair") == 0)
		repair = true;

	struct mtd_raid301_dev *dev = container_of(mtd, struct mtd_raid301_dev, mtd);
	if (raid301_scrub_stripes(dev->master, repair) != 0)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_raid301_sub[] = {
	U_BOOT_CMD_MKENT(attach, 2, 0, do_raid301_attach, "", ""),
	U_BOOT_CMD_MKENT(detach, 1, 0, do_raid301_detach, "", ""),
	U_BOOT_CMD_MKENT(format, 3, 0, do_raid301_format, "", ""),
	U_BOOT_CMD_MKENT(scrub, 2, 0, do_raid301_scrub, "", ""),
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
	"30+1 XOR RAID5 MTD Management",
	"attach [backing_mtd]         - Attach RAID301 to backing MTD\n"
	"raid301 detach               - Detach RAID301 virtual device\n"
	"raid301 format <mtd> --confirm - Format backing MTD partition for RAID301\n"
	"raid301 info                 - Show RAID301 status and superblock info"
);
