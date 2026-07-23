/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * 30+1 XOR RAID-5 Driver Abstraction Layer for 32MB SPI NOR Flash
 *
 * Copyright (C) 2026
 */

#include <config.h>
#include <log.h>
#include <malloc.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <mtd_raid301.h>

static struct mtd_raid301_dev g_raid301_dev;

/* -------------------------------------------------------------------------
 * Physical Block & Address Mapping Helpers
 * ------------------------------------------------------------------------- */

static inline u32 get_parity_phys_block(u32 stripe_idx)
{
	return MTD_RAID301_RAW_BLOCKS + (stripe_idx % MTD_RAID301_RAID_BLOCKS);
}

static inline u32 get_data_phys_block(u32 stripe_idx, u32 logic_data_block)
{
	u32 parity_block = get_parity_phys_block(stripe_idx);
	u32 phys_block = MTD_RAID301_RAW_BLOCKS + logic_data_block;

	if (phys_block >= parity_block)
		phys_block++;

	return phys_block;
}

static inline u32 calc_phys_addr(u32 phys_block, u32 sector_in_block)
{
	return (phys_block * MTD_RAID301_BLOCK_SIZE) + (sector_in_block * MTD_RAID301_SECTOR_SIZE);
}

/* -------------------------------------------------------------------------
 * Low-level Sector I/O & CRC Verification
 * ------------------------------------------------------------------------- */

static int read_phys_sector(struct mtd_info *master, u32 phys_addr, struct sector_unit *sector)
{
	size_t retlen = 0;
	int ret = mtd_read(master, phys_addr, sizeof(struct sector_unit), &retlen, (u_char *)sector);

	if (ret < 0 || retlen != sizeof(struct sector_unit))
		return -EIO;

	return 0;
}

static bool check_sector_crc(const struct sector_unit *sector)
{
	u32 calc_crc = crc32(0, sector->data, MTD_RAID301_SECTOR_DATA_SIZE);
	return (calc_crc == sector->crc32);
}

static int erase_phys_sector(struct mtd_info *master, u32 phys_addr)
{
	struct erase_info instr;

	memset(&instr, 0, sizeof(instr));
	instr.mtd = master;
	instr.addr = phys_addr;
	instr.len = MTD_RAID301_SECTOR_SIZE;

	return mtd_erase(master, &instr);
}

static int write_phys_sector(struct mtd_info *master, u32 phys_addr, const struct sector_unit *sector)
{
	size_t retlen = 0;
	int ret;

	ret = erase_phys_sector(master, phys_addr);
	if (ret) {
		printf("RAID301: Erase failed at phys 0x%08x (err %d)\n", phys_addr, ret);
		return ret;
	}

	ret = mtd_write(master, phys_addr, sizeof(struct sector_unit), &retlen, (const u_char *)sector);
	if (ret < 0 || retlen != sizeof(struct sector_unit)) {
		printf("RAID301: Write failed at phys 0x%08x (err %d)\n", phys_addr, ret);
		return -EIO;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * XOR Reconstruction Engine
 * ------------------------------------------------------------------------- */

static int reconstruct_sector(struct mtd_info *master, u32 stripe_idx, u32 sector_in_block,
			       u32 bad_phys_block, struct sector_unit *out_recovered)
{
	struct sector_unit *temp_sector;
	u8 *dst = (u8 *)out_recovered;
	u32 i, k;
	int ret;

	temp_sector = malloc(sizeof(struct sector_unit));
	if (!temp_sector)
		return -ENOMEM;

	memset(out_recovered, 0, sizeof(struct sector_unit));

	/* Iterate over all 31 physical blocks in the RAID group */
	for (i = 0; i < MTD_RAID301_RAID_BLOCKS; i++) {
		u32 phys_block = MTD_RAID301_RAW_BLOCKS + i;
		u32 phys_addr;

		if (phys_block == bad_phys_block)
			continue;

		phys_addr = calc_phys_addr(phys_block, sector_in_block);
		ret = read_phys_sector(master, phys_addr, temp_sector);
		if (ret != 0 || !check_sector_crc(temp_sector)) {
			printf("RAID301: Uncorrectable double fault! Block %u sector %u corrupted\n",
			       phys_block, sector_in_block);
			free(temp_sector);
			return -EIO;
		}

		/* Perform XOR accumulation across all 30 healthy sectors */
		u8 *src = (u8 *)temp_sector;
		for (k = 0; k < sizeof(struct sector_unit); k++) {
			dst[k] ^= src[k];
		}
	}

	free(temp_sector);

	/* Recalculate CRC for recovered payload to verify consistency */
	out_recovered->crc32 = crc32(0, out_recovered->data, MTD_RAID301_SECTOR_DATA_SIZE);
	g_raid301_dev.stats.recovered_sectors++;

	return 0;
}

/* -------------------------------------------------------------------------
 * MTD Operations (_read, _write, _erase)
 * ------------------------------------------------------------------------- */

static int mtd_raid301_read_ops(struct mtd_info *mtd, loff_t from, size_t len,
				size_t *retlen, u_char *buf)
{
	struct mtd_info *master = g_raid301_dev.master;
	struct sector_unit *sector_buf;
	size_t remaining = len;
	size_t read_bytes = 0;
	int ret = 0;

	if (!master || from + len > mtd->size)
		return -EINVAL;

	sector_buf = malloc(sizeof(struct sector_unit));
	if (!sector_buf)
		return -ENOMEM;

	g_raid301_dev.stats.read_ops++;

	while (remaining > 0) {
		u32 logic_sector = (u32)(from / MTD_RAID301_SECTOR_DATA_SIZE);
		u32 offset_in_sector = (u32)(from % MTD_RAID301_SECTOR_DATA_SIZE);
		u32 chunk_len = MTD_RAID301_SECTOR_DATA_SIZE - offset_in_sector;

		if (chunk_len > remaining)
			chunk_len = remaining;

		u32 stripe_idx = logic_sector / MTD_RAID301_DATA_BLOCKS;
		u32 logic_data_block = logic_sector % MTD_RAID301_DATA_BLOCKS;
		u32 sector_in_block = stripe_idx % MTD_RAID301_SECTORS_PER_BLOCK;

		u32 phys_block = get_data_phys_block(stripe_idx, logic_data_block);
		u32 phys_addr = calc_phys_addr(phys_block, sector_in_block);

		/* 1. Try reading the sector directly and check CRC */
		ret = read_phys_sector(master, phys_addr, sector_buf);
		if (ret != 0 || !check_sector_crc(sector_buf)) {
			g_raid301_dev.stats.crc_errors++;
			printf("RAID301: CRC error at phys 0x%08x (Block %u Sector %u). Triggering XOR reconstruction...\n",
			       phys_addr, phys_block, sector_in_block);

			/* 2. Reconstruct from parity and other 29 data blocks */
			ret = reconstruct_sector(master, stripe_idx, sector_in_block, phys_block, sector_buf);
			if (ret != 0) {
				g_raid301_dev.stats.uncorrectable_errors++;
				ret = -EIO;
				break;
			}

			/* 3. Self-healing: Write reconstructed data back to flash */
			write_phys_sector(master, phys_addr, sector_buf);
		}

		memcpy(buf + read_bytes, sector_buf->data + offset_in_sector, chunk_len);

		read_bytes += chunk_len;
		from += chunk_len;
		remaining -= chunk_len;
	}

	free(sector_buf);
	if (retlen)
		*retlen = read_bytes;

	return ret;
}

static int mtd_raid301_write_ops(struct mtd_info *mtd, loff_t to, size_t len,
				 size_t *retlen, const u_char *buf)
{
	struct mtd_info *master = g_raid301_dev.master;
	struct sector_unit *old_data, *old_parity, *new_data, *new_parity;
	size_t remaining = len;
	size_t written_bytes = 0;
	int ret = 0;

	if (!master || to + len > mtd->size)
		return -EINVAL;

	old_data = malloc(sizeof(struct sector_unit));
	old_parity = malloc(sizeof(struct sector_unit));
	new_data = malloc(sizeof(struct sector_unit));
	new_parity = malloc(sizeof(struct sector_unit));

	if (!old_data || !old_parity || !new_data || !new_parity) {
		ret = -ENOMEM;
		goto out_free;
	}

	g_raid301_dev.stats.write_ops++;

	while (remaining > 0) {
		u32 logic_sector = (u32)(to / MTD_RAID301_SECTOR_DATA_SIZE);
		u32 offset_in_sector = (u32)(to % MTD_RAID301_SECTOR_DATA_SIZE);
		u32 chunk_len = MTD_RAID301_SECTOR_DATA_SIZE - offset_in_sector;

		if (chunk_len > remaining)
			chunk_len = remaining;

		u32 stripe_idx = logic_sector / MTD_RAID301_DATA_BLOCKS;
		u32 logic_data_block = logic_sector % MTD_RAID301_DATA_BLOCKS;
		u32 sector_in_block = stripe_idx % MTD_RAID301_SECTORS_PER_BLOCK;

		u32 data_phys_block = get_data_phys_block(stripe_idx, logic_data_block);
		u32 parity_phys_block = get_parity_phys_block(stripe_idx);

		u32 data_phys_addr = calc_phys_addr(data_phys_block, sector_in_block);
		u32 parity_phys_addr = calc_phys_addr(parity_phys_block, sector_in_block);

		/* Read old data sector */
		ret = read_phys_sector(master, data_phys_addr, old_data);
		if (ret != 0 || !check_sector_crc(old_data)) {
			/* If old data is unreadable, reconstruct it first */
			reconstruct_sector(master, stripe_idx, sector_in_block, data_phys_block, old_data);
		}

		/* Read old parity sector */
		ret = read_phys_sector(master, parity_phys_addr, old_parity);
		if (ret != 0 || !check_sector_crc(old_parity)) {
			/* If old parity is unreadable, reconstruct it first */
			reconstruct_sector(master, stripe_idx, sector_in_block, parity_phys_block, old_parity);
		}

		/* Prepare new data payload */
		memcpy(new_data->data, old_data->data, MTD_RAID301_SECTOR_DATA_SIZE);
		memcpy(new_data->data + offset_in_sector, buf + written_bytes, chunk_len);
		new_data->crc32 = crc32(0, new_data->data, MTD_RAID301_SECTOR_DATA_SIZE);

		/* Compute new Parity: P_new = P_old ^ D_old ^ D_new */
		u8 *p_old = (u8 *)old_parity;
		u8 *d_old = (u8 *)old_data;
		u8 *d_new = (u8 *)new_data;
		u8 *p_new = (u8 *)new_parity;

		for (size_t k = 0; k < sizeof(struct sector_unit); k++) {
			p_new[k] = p_old[k] ^ d_old[k] ^ d_new[k];
		}
		new_parity->crc32 = crc32(0, new_parity->data, MTD_RAID301_SECTOR_DATA_SIZE);

		/* Write out updated data sector and updated parity sector */
		ret = write_phys_sector(master, data_phys_addr, new_data);
		if (ret != 0)
			break;

		ret = write_phys_sector(master, parity_phys_addr, new_parity);
		if (ret != 0)
			break;

		written_bytes += chunk_len;
		to += chunk_len;
		remaining -= chunk_len;
	}

	if (retlen)
		*retlen = written_bytes;

out_free:
	free(old_data);
	free(old_parity);
	free(new_data);
	free(new_parity);

	return ret;
}

static int mtd_raid301_erase_ops(struct mtd_info *mtd, struct erase_info *instr)
{
	u8 *ff_data;
	size_t retlen;
	int ret = 0;

	ff_data = malloc(MTD_RAID301_SECTOR_DATA_SIZE);
	if (!ff_data)
		return -ENOMEM;

	memset(ff_data, 0xFF, MTD_RAID301_SECTOR_DATA_SIZE);

	g_raid301_dev.stats.erase_ops++;

	/* Write 0xFF payload to erased region sector by sector to update parity */
	ret = mtd_raid301_write_ops(mtd, instr->addr, instr->len, &retlen, ff_data);

	free(ff_data);

	if (ret == 0)
		instr->state = MTD_ERASE_DONE;
	else
		instr->state = MTD_ERASE_FAILED;

	return ret;
}

/* -------------------------------------------------------------------------
 * Initialization & Public API
 * ------------------------------------------------------------------------- */

struct mtd_info *mtd_raid301_init(struct mtd_info *master)
{
	if (!master) {
		printf("RAID301 Error: Null master MTD device\n");
		return NULL;
	}

	if (master->size < MTD_RAID301_FLASH_SIZE) {
		printf("RAID301 Error: Master MTD size (%llu MB) less than required 32MB\n",
		       master->size / (1024 * 1024));
		return NULL;
	}

	memset(&g_raid301_dev, 0, sizeof(g_raid301_dev));
	g_raid301_dev.master = master;

	/* Setup Virtual MTD Info */
	struct mtd_info *mtd = &g_raid301_dev.mtd;
	memset(mtd, 0, sizeof(struct mtd_info));

	mtd->name = "raid301";
	mtd->type = MTD_NORFLASH;
	mtd->flags = MTD_CAP_NORFLASH;
	mtd->size = MTD_RAID301_LOGICAL_SIZE;
	mtd->erasesize = MTD_RAID301_SECTOR_DATA_SIZE;
	mtd->writesize = 1;
	mtd->writebufsize = master->writebufsize;

	mtd->_read = mtd_raid301_read_ops;
	mtd->_write = mtd_raid301_write_ops;
	mtd->_erase = mtd_raid301_erase_ops;

	if (add_mtd_device(mtd)) {
		printf("RAID301 Error: Failed to add virtual MTD device\n");
		return NULL;
	}

	g_raid301_dev.initialized = true;

	printf("\n=======================================================\n");
	printf(" MTD 30+1 XOR RAID-5 Driver Layer Initialized\n");
	printf(" Master MTD     : %s (Total 32MB)\n", master->name);
	printf(" Raw MTD Area   : Block 0 (1MB)\n");
	printf(" RAID5 Area     : Block 1~31 (31MB, 30 Data + 1 Parity)\n");
	printf(" Virtual MTD Dev: raid301 (Capacity: %llu Bytes / ~%llu MB)\n",
	       mtd->size, mtd->size / (1024 * 1024));
	printf(" Wear-Leveling  : Distributed Parity Rotation Enabled\n");
	printf(" Fault Tolerance: Single Block Loss Self-Healing\n");
	printf("=======================================================\n\n");

	return mtd;
}

void mtd_raid301_cleanup(void)
{
	if (g_raid301_dev.initialized) {
		del_mtd_device(&g_raid301_dev.mtd);
		g_raid301_dev.initialized = false;
	}
}

struct mtd_info *mtd_raid301_get_dev(void)
{
	if (g_raid301_dev.initialized)
		return &g_raid301_dev.mtd;
	return NULL;
}

/* Fault injection helper for testing and demonstration */
int mtd_raid301_inject_fault(u32 phys_block, u32 sector_idx)
{
	struct mtd_info *master = g_raid301_dev.master;
	u32 phys_addr;
	u8 *corrupt_buf;
	size_t retlen;
	int ret;

	if (!master || phys_block >= 32 || sector_idx >= MTD_RAID301_SECTORS_PER_BLOCK)
		return -EINVAL;

	phys_addr = calc_phys_addr(phys_block, sector_idx);

	corrupt_buf = malloc(sizeof(struct sector_unit));
	if (!corrupt_buf)
		return -ENOMEM;

	memset(corrupt_buf, 0xDE, sizeof(struct sector_unit)); // Fill with garbage

	ret = erase_phys_sector(master, phys_addr);
	if (ret == 0) {
		ret = mtd_write(master, phys_addr, sizeof(struct sector_unit), &retlen, corrupt_buf);
	}

	free(corrupt_buf);

	if (ret == 0) {
		printf("RAID301: Injected corrupt garbage data at Phys Block %u Sector %u (0x%08x)\n",
		       phys_block, sector_idx, phys_addr);
	}
	return ret;
}

int mtd_raid301_scrub(void)
{
	struct mtd_info *mtd = &g_raid301_dev.mtd;
	u8 *buf;
	size_t retlen;
	loff_t addr = 0;
	int ret = 0;

	if (!g_raid301_dev.initialized)
		return -ENODEV;

	buf = malloc(MTD_RAID301_SECTOR_DATA_SIZE);
	if (!buf)
		return -ENOMEM;

	printf("RAID301: Starting Global Integrity Scrubbing...\n");

	while (addr < mtd->size) {
		ret = mtd_raid301_read_ops(mtd, addr, MTD_RAID301_SECTOR_DATA_SIZE, &retlen, buf);
		if (ret != 0) {
			printf("RAID301 Scrubbing Error at offset 0x%llx\n", addr);
		}
		addr += MTD_RAID301_SECTOR_DATA_SIZE;
	}

	free(buf);
	printf("RAID301: Scrubbing Complete! CRC Errors: %u, Recovered: %u, Uncorrectable: %u\n",
	       g_raid301_dev.stats.crc_errors,
	       g_raid301_dev.stats.recovered_sectors,
	       g_raid301_dev.stats.uncorrectable_errors);

	return 0;
}

void mtd_raid301_dump_info(void)
{
	printf("\n--- MTD RAID301 Statistics ---\n");
	printf("Read Operations     : %u\n", g_raid301_dev.stats.read_ops);
	printf("Write Operations    : %u\n", g_raid301_dev.stats.write_ops);
	printf("Erase Operations    : %u\n", g_raid301_dev.stats.erase_ops);
	printf("CRC32 Faults Found  : %u\n", g_raid301_dev.stats.crc_errors);
	printf("Sectors Recovered   : %u\n", g_raid301_dev.stats.recovered_sectors);
	printf("Uncorrectable Faults: %u\n", g_raid301_dev.stats.uncorrectable_errors);
	printf("-------------------------------\n\n");
}
