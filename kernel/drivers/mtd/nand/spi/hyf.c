// SPDX-License-Identifier: GPL-2.0
/*
 * HeYangTek (南京扬贺扬) SPI NAND chips.
 *
 * The HYF2GQ4UAACAE (2Gb, 2048+64, 14-bit on-die ECC) found on the
 * ZTE F7610U (ZX279131) reports manufacturer id 0xc9 / device id 0x52.
 * The vendor U-Boot prints these as "manuid=c9,52" and stores
 * { 0xc9, 0x52, 0x00, 0x00 } in its chip table.
 *
 * The same part is also supported by the Rockchip rkflash driver:
 *   { 0xC9, 0x52, 0x00, 4, 0x40, 1, 2048, 0x4C, 19, 0xE, 1,
 *     { 0x04, 0x24, 0xFF, 0xFF }, &sfc_nand_get_ecc_status0 },
 *
 * The ID stream of this chip repeats ("c9 52 c9 52 ...") and which byte
 * the controller sees first depends on its dummy-byte handling.  To be
 * robust, the same chip is registered for all three read-id methods the
 * spinand core tries.
 */
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_HEYANGTEK		0xc9

static SPINAND_OP_VARIANTS(read_cache_variants,
		SPINAND_PAGE_READ_FROM_CACHE_FAST_1S_1S_1S_OP(0, 1, NULL, 0, 0),
		SPINAND_PAGE_READ_FROM_CACHE_1S_1S_1S_OP(0, 1, NULL, 0, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
		SPINAND_PROG_LOAD_1S_1S_1S_OP(true, 0, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
		SPINAND_PROG_LOAD_1S_1S_1S_OP(false, 0, NULL, 0));

/*
 * On-die ECC layout: not documented for this part; use the common
 * "ECC data in the upper half of OOB" shape of 2K-page domestic parts.
 */
static int hyf_ooblayout_ecc(struct mtd_info *mtd, int section,
			     struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = mtd->oobsize / 2;
	region->length = mtd->oobsize / 2;

	return 0;
}

static int hyf_ooblayout_free(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = 2;
	region->length = mtd->oobsize / 2 - 2;

	return 0;
}

static const struct mtd_ooblayout_ops hyf_ooblayout = {
	.ecc = hyf_ooblayout_ecc,
	.free = hyf_ooblayout_free,
};

static int hyf_ecc_get_status(struct spinand_device *spinand, u8 status)
{
	/*
	 * The 14-bit engine's status encoding is undocumented; assume the
	 * common 2-bit "corrected / uncorrectable" scheme at [5:4].
	 */
	switch (status & (3 << 4)) {
	case 0 << 4:
		return 0;
	case 1 << 4:
		return 1;	/* errors corrected */
	case 2 << 4:
		return -EBADMSG;	/* uncorrectable */
	case 3 << 4:
		return 2;	/* max corrected */
	}

	return -EINVAL;
}

#define HYF_CHIP(__method)						\
	SPINAND_INFO("HYF2GQ4UAACAE",					\
		     SPINAND_ID(__method, 0x52),			\
		     NAND_MEMORG(1, 2048, 64, 64, 2048, 40, 1, 1, 1),	\
		     NAND_ECCREQ(14, 512),				\
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,	\
					      &write_cache_variants,	\
					      &update_cache_variants),	\
		     0,							\
		     SPINAND_ECCINFO(&hyf_ooblayout, hyf_ecc_get_status))

static const struct spinand_info hyf_spinand_table[] = {
	HYF_CHIP(SPINAND_READID_METHOD_OPCODE),
	HYF_CHIP(SPINAND_READID_METHOD_OPCODE_ADDR),
	HYF_CHIP(SPINAND_READID_METHOD_OPCODE_DUMMY),
};

static const struct spinand_manufacturer_ops hyf_spinand_manuf_ops = {
};

const struct spinand_manufacturer heyangtek_spinand_manufacturer = {
	.id = SPINAND_MFR_HEYANGTEK,
	.name = "HeYangTek",
	.chips = hyf_spinand_table,
	.nchips = ARRAY_SIZE(hyf_spinand_table),
	.ops = &hyf_spinand_manuf_ops,
};
