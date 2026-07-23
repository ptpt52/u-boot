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

static int do_raid301_init(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const char *master_name = "nor0";
	struct mtd_info *master, *raid_mtd;

	if (argc >= 2)
		master_name = argv[1];

	mtd_probe_devices();
	master = get_mtd_device_nm(master_name);
	if (IS_ERR_OR_NULL(master)) {
		printf("RAID301: Could not find MTD device '%s'\n", master_name);
		printf("Available MTD devices:\n");
		struct mtd_info *mtd;
		mtd_for_each_device(mtd) {
			printf("  - %s (Size: %llu MB)\n", mtd->name, mtd->size / (1024 * 1024));
		}
		return CMD_RET_FAILURE;
	}

	raid_mtd = mtd_raid301_init(master);
	if (!raid_mtd)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

static int do_raid301_read(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mtd_info *mtd = mtd_raid301_get_dev();
	ulong addr, offset, len;
	void *buf;
	size_t retlen;
	int ret;

	if (!mtd) {
		printf("RAID301: Device not initialized. Run 'raid301 init' first.\n");
		return CMD_RET_FAILURE;
	}

	if (argc < 4)
		return CMD_RET_USAGE;

	addr = hextoul(argv[1], NULL);
	offset = hextoul(argv[2], NULL);
	len = hextoul(argv[3], NULL);

	buf = map_sysmem(addr, len);

	printf("RAID301: Reading 0x%lx bytes from logic offset 0x%lx to RAM 0x%lx...\n", len, offset, addr);
	ret = mtd->_read(mtd, offset, len, &retlen, buf);
	unmap_sysmem(buf);

	if (ret != 0) {
		printf("RAID301: Read failed with error %d\n", ret);
		return CMD_RET_FAILURE;
	}

	printf("RAID301: Successfully read 0x%zx bytes.\n", retlen);
	return CMD_RET_SUCCESS;
}

static int do_raid301_write(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mtd_info *mtd = mtd_raid301_get_dev();
	ulong addr, offset, len;
	void *buf;
	size_t retlen;
	int ret;

	if (!mtd) {
		printf("RAID301: Device not initialized. Run 'raid301 init' first.\n");
		return CMD_RET_FAILURE;
	}

	if (argc < 4)
		return CMD_RET_USAGE;

	addr = hextoul(argv[1], NULL);
	offset = hextoul(argv[2], NULL);
	len = hextoul(argv[3], NULL);

	buf = map_sysmem(addr, len);

	printf("RAID301: Writing 0x%lx bytes from RAM 0x%lx to logic offset 0x%lx...\n", len, addr, offset);
	ret = mtd->_write(mtd, offset, len, &retlen, buf);
	unmap_sysmem(buf);

	if (ret != 0) {
		printf("RAID301: Write failed with error %d\n", ret);
		return CMD_RET_FAILURE;
	}

	printf("RAID301: Successfully wrote 0x%zx bytes.\n", retlen);
	return CMD_RET_SUCCESS;
}

static int do_raid301_inject(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	u32 phys_block, sector_idx;

	if (argc < 3)
		return CMD_RET_USAGE;

	phys_block = dectoul(argv[1], NULL);
	sector_idx = dectoul(argv[2], NULL);

	if (mtd_raid301_inject_fault(phys_block, sector_idx) != 0) {
		printf("RAID301: Fault injection failed!\n");
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

static int do_raid301_scrub(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (mtd_raid301_scrub() != 0)
		return CMD_RET_FAILURE;
	return CMD_RET_SUCCESS;
}

static int do_raid301_info(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mtd_info *mtd = mtd_raid301_get_dev();

	if (!mtd) {
		printf("RAID301: Device not initialized. Run 'raid301 init' first.\n");
		return CMD_RET_FAILURE;
	}

	mtd_raid301_dump_info();
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_raid301_sub[] = {
	U_BOOT_CMD_MKENT(init, 2, 0, do_raid301_init, "", ""),
	U_BOOT_CMD_MKENT(read, 4, 0, do_raid301_read, "", ""),
	U_BOOT_CMD_MKENT(write, 4, 0, do_raid301_write, "", ""),
	U_BOOT_CMD_MKENT(inject_fault, 3, 0, do_raid301_inject, "", ""),
	U_BOOT_CMD_MKENT(scrub, 1, 0, do_raid301_scrub, "", ""),
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
	"init [master_mtd_name] - Initialize 30+1 RAID5 device\n"
	"raid301 info                     - Show statistics and device info\n"
	"raid301 read <addr> <off> <len>  - Read logic offset from raid301 to RAM\n"
	"raid301 write <addr> <off> <len> - Write RAM buffer to raid301 logic offset\n"
	"raid301 inject_fault <blk> <sec> - Inject corruption fault for testing\n"
	"raid301 scrub                    - Perform global integrity check & self-healing"
);
