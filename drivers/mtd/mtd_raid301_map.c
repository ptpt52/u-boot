/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Address Mapping & Layout Validation Engine
 *
 * Copyright (C) 2026
 */

#include <config.h>
#include <log.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <mtd_raid301.h>
#include "mtd_raid301_internal.h"

/* Greatest Common Divisor (GCD) for Coprime Verification */
static u32 raid301_gcd(u32 a, u32 b)
{
	while (b != 0) {
		u32 temp = b;
		b = a % b;
		a = temp;
	}
	return a;
}

/* Validate Coprime Stride */
bool raid301_validate_stride(u32 stride, u32 member_count)
{
	if (stride == 0 || member_count == 0)
		return false;
	return (raid301_gcd(stride, member_count) == 1);
}

/* Calculate Parity Member Index for a Given Stripe */
u16 raid301_stripe_to_parity_member(u32 stripe_id)
{
	u32 offset = CONFIG_MTD_RAID301_PARITY_OFFSET;
	u32 stride = CONFIG_MTD_RAID301_PARITY_STRIDE;
	u32 count = CONFIG_MTD_RAID301_MEMBER_COUNT;

	u64 raw_idx = (u64)offset + ((u64)stripe_id * stride);
	return (u16)(raw_idx % count);
}

/* Calculate Physical Member Index for Data Index & Stripe */
u16 raid301_data_to_phys_member(u32 stripe_id, u16 data_index)
{
	u16 parity_mem = raid301_stripe_to_parity_member(stripe_id);
	u16 phys_mem = data_index;

	if (phys_mem >= parity_mem)
		phys_mem++;

	return phys_mem;
}

/* 64-bit Overflow-safe Address Calculation */
int raid301_calc_physical_offset(u16 member_id, u32 stripe_id, u64 *out_offset)
{
	u64 member_size = CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES;
	u64 erase_size = CONFIG_MTD_RAID301_ERASE_SIZE_BYTES;
	u64 total_backing_size = (u64)CONFIG_MTD_RAID301_MEMBER_COUNT * member_size;

	if (member_id >= CONFIG_MTD_RAID301_MEMBER_COUNT)
		return -EINVAL;

	if (stripe_id >= RAID301_STRIPE_COUNT)
		return -EINVAL;

	u64 member_base = (u64)member_id * member_size;
	u64 slot_offset = (u64)stripe_id * erase_size;
	u64 phys_offset = member_base + slot_offset;

	if (phys_offset + erase_size > total_backing_size)
		return -EOVERFLOW;

	*out_offset = phys_offset;
	return 0;
}

/* Verify Backing MTD Partition Geometry and Compatibility */
int raid301_validate_backing_mtd(struct mtd_info *master)
{
	if (!master) {
		printf("RAID301 Error: Null master backing MTD device\n");
		return -EINVAL;
	}

	if (master->type != MTD_NORFLASH) {
		printf("RAID301 Error: Backing device '%s' is not MTD_NORFLASH (type: %d)\n",
		       master->name, master->type);
		return -EINVAL;
	}

	if (!master->_read || !master->_write || !master->_erase) {
		printf("RAID301 Error: Backing device '%s' lacks read/write/erase operations\n",
		       master->name);
		return -ENOTSUPP;
	}

	u64 expected_size = (u64)CONFIG_MTD_RAID301_MEMBER_COUNT * CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES;
	if (master->size != expected_size) {
		printf("RAID301 Error: Backing MTD size (%llu) does not match expected size (%llu)\n",
		       master->size, expected_size);
		return -EINVAL;
	}

	if (master->erasesize != CONFIG_MTD_RAID301_ERASE_SIZE_BYTES) {
		printf("RAID301 Error: Backing MTD erasesize (%u) does not match config (%u)\n",
		       master->erasesize, CONFIG_MTD_RAID301_ERASE_SIZE_BYTES);
		return -EINVAL;
	}

	if (!raid301_validate_stride(CONFIG_MTD_RAID301_PARITY_STRIDE, CONFIG_MTD_RAID301_MEMBER_COUNT)) {
		printf("RAID301 Error: Parity stride (%u) and member count (%u) are not coprime!\n",
		       CONFIG_MTD_RAID301_PARITY_STRIDE, CONFIG_MTD_RAID301_MEMBER_COUNT);
		return -EINVAL;
	}

	return 0;
}
