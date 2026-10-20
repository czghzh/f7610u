/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ZTE ZX279131 packet engine / switch / MDIO register definitions.
 *
 * Fully reverse-engineered from the vendor 4.1.25 driver
 * (plat_zxylzb_9131.ko, not stripped, all functions decoded):
 *   pp_dma_init / pp_bmu_init / pp_bmu_alloc_bp / pp_bmu_free_bp /
 *   pp_queue_init / pp_net_tx / pp_data_raw_send / soft_insert_tx_1desc /
 *   pp_check_tx_done_nolock / switch_smac_* / smac_* / phy_process /
 *   register_phy_int / tm_pp_init     and mii_read/mii_write from the
 *   5.4 sibling kernel (kernel-2b5.elf).
 *
 * Cross-checked against live register snapshots of a running F7610U and
 * against the SR1010 vendor DTS.  See NETWORK-DESIGN.md / work/v3dis/.
 */
#ifndef __ZX279131_REGS_H
#define __ZX279131_REGS_H

/* Physical windows */
#define ZX279131_PP_PA		0x18000000
#define ZX279131_PP_SIZE	0x1000000	/* 16 MiB */
#define ZX279131_SW_PA		0x1a000000
#define ZX279131_SW_SIZE	0x1000000	/* 16 MiB */
#define ZX279131_IDM_PA		0x1a008000
#define ZX279131_IDM_SIZE	0x2000
#define ZX279131_MDIO_PA	0x14f01000
#define ZX279131_MDIO_SIZE	0x1000

/* ------------------------------------------------------------------ */
/* PP: DMA engine window (PP + 0x18000)                                */
/* ------------------------------------------------------------------ */
#define PP_DMA_CTRL0		0x18000	/* rw: clr bit23, set [19:16]=0xF */
#define PP_DMA_MAGIC04		0x18004	/* = 127 */
#define PP_DMA_QW0		0x18020	/* = 64 */
#define PP_DMA_QW1		0x18024	/* = 64 */
#define PP_DMA_MODE28		0x18028	/* = 1 */
#define PP_DMA_MODE2C		0x1802c	/* = 1 */
#define PP_DMA_TIMEOUT		0x18030	/* = 20000 */
#define PP_DMA_DESC_CFG		0x18034	/* = 128 */
#define PP_DMA_BURST		0x1803c	/* = 0x00400040 */

/* TX ring A ("channel 0", used by the vendor pon/type-2 paths) */
#define PP_DMA_M40		0x18040	/* DMA misc window 0x40..0x4c */
#define PP_DMA_M44		0x18044
#define PP_DMA_M48		0x18048
#define PP_DMA_TXFREE		0x1804c	/* dma_tx_free: poll bit21 clear, then write */
#define PP_INT_STATUS		0x100	/* bits[1:0] pending (_zx_pp_int) */
#define PP_INT_MASK		0x104	/* pp_int_init writes 0xffffffff */
#define PP_DMA_RINGA_BASE	0x18050
#define PP_DMA_RINGA_DOOR	0x18054	/* w: descriptor count */
#define PP_DMA_RINGA_DONE	0x18058	/* r: completed count, low 16 bits */
/* TX ring B ("channel 1", used by the vendor sw netdev, type 1) */
#define PP_DMA_RINGB_BASE	0x18060
#define PP_DMA_RINGB_DOOR	0x18064
#define PP_DMA_RINGB_DONE	0x18068
/* Unprogrammed by any vendor code we decoded; on a live stock box 0x1801806c
 * reads 0x179f179f (a 16/16 pair that grows with traffic) - used here purely
 * as a "is the engine still consuming descriptors" probe. */
#define PP_DMA_RINGA_MISC	0x1805c
#define PP_DMA_RINGB_MISC	0x1806c
#define PP_DMA_M38		0x18038

/* U-Boot-only PP pair.  Bootloader function 0x840252a4 writes
 *   0x19002040 = 0x0fa0000c   and   0x19002044 = 0x3e80000c
 * through its 0x19000000 alias window.  Anchoring that alias on the one
 * register we can cross-check (ring-B misc, which still held U-Boot's
 * TFTP counter 0xd5c2d5c2 at 0x1801806c) puts them at 0x18012050 /
 * 0x18012054 (A); the ring-A anchor instead gives 0x18013040 /
 * 0x18013044 (B).  Neither the vendor kernel nor this driver has ever
 * written either address.  Both candidates are programmed behind the
 * `ubq` module parameter (default on) so one boot tests both. */
#define PP_UBQ_A0		0x12050
#define PP_UBQ_A1		0x12054
#define PP_UBQ_B0		0x13040
#define PP_UBQ_B1		0x13044

/* ------------------------------------------------------------------ */
/* PP: BMU (PP + 0x10000)                                              */
/* ------------------------------------------------------------------ */
#define PP_BMU_CTRL		0x00	/* 1 = enable */
#define PP_BMU_CFG_A		0x04	/* = 0x0104c040 */
#define PP_BMU_CFG_B		0x08	/* = 0x0104c040 */
#define PP_BMU_ALLOC_RESULT	0x0c	/* bit31 valid | bp index */
#define PP_BMU_FREE		0x10	/* w: (pool << 16) | bp */
#define PP_BMU_ALLOC_REQ	0x14	/* w: pool | 1; poll bit0 until clear */
#define PP_BMU_POOLCNT_TX	0x48	/* (tx_bp_count << 16) */
#define PP_BMU_POOLCNT_RX	0x4c	/* (512 << 16) */
#define PP_BMU_CFG_C		0x58	/* = 0x17f */
#define PP_BMU_CFG_D		0x5c	/* = 0xf */
#define PP_BMU_FIFO_STATUS	0xdc	/* bits[8:3] = free-queue occupancy */

#define PP_BMU_MAX_BP		12287
#define ZX279131_BP_FIFO_ENTRIES	12288	/* table 0: TX pool  (bp x 2432) */
#define ZX279131_BP_TABLE_ENTRIES	512	/* table 1: RX pool  (bp x 9728) */
#define ZX279131_TX_BP_STRIDE	2432
#define ZX279131_RX_BP_STRIDE	9728
#define ZX279131_TX_BP_MAX	12288

/* ------------------------------------------------------------------ */
/* PP: pool / ring configuration registers (PP + offset)               */
/* ------------------------------------------------------------------ */
#define PP_POOL_TBL0		0x0e8	/* BP fifo table 0 base (phys) */
#define PP_POOL_TBL1		0x0ec	/* BP fifo table 1 base (phys) */
#define PP_POOL_RXRING		0x0f0	/* RX descriptor rings base (16 x 64K) */
#define PP_POOL_TX		0x0f4	/* TX buffer pool base (bp x 2432) */
#define PP_POOL_RX		0x0f8	/* RX buffer pool base (bp x 9728) */
#define PP_POOL_STRIDES		0x0fc	/* = 0x26000980 (9728 / 2432) */
#define PP_POOL_MISC		0x2d8	/* misc table base (phys) */

/* RX rings: 16 rings of 4096 descriptors x 16 B = 64 KiB each, located
 * at PP_POOL_RXRING + ring * 0x10000; ring = queue + batch*8. */
#define ZX279131_RX_RING_SIZE	4096
#define ZX279131_RX_DESC_SIZE	16
#define ZX279131_RX_RING_BYTES	(ZX279131_RX_RING_SIZE * ZX279131_RX_DESC_SIZE)
#define ZX279131_NUM_RX_QUEUES	8
#define ZX279131_RX_RINGS_TOTAL	(16 * ZX279131_RX_RING_BYTES)	/* 1 MiB */

/* RX descriptor release (after the driver consumed descriptors) */
#define PP_RXREL_TRIG		0xc064	/* w: 1 = trigger */
#define PP_RXREL_CMD		0xc068	/* w: queue | mode<<3 | count<<4 | batch<<14 */

/* RX queue counters (PP + 0x18100), 8 queues x u32 */
#define PP_RX_QCNT_BASE		0x18100
#define PP_RX_QCNT(q)		(PP_RX_QCNT_BASE + (q) * 4)

/* ------------------------------------------------------------------ */
/* TX descriptor (32 bytes, 1024 per ring)                             */
/* ------------------------------------------------------------------ */
struct zx279131_tx_desc {
	__le16 ctrl;		/* +0: 0x8000 */
	__le16 f2;		/* +2 */
	__le32 f4;		/* +4: 0xa0000000 / 0x80000000 (sw), 0 (pon) */
	__le16 f8;		/* +8: queue */
	__le16 bp_idx;		/* +10: BP index */
	__le32 w12;		/* +12: 0x20000021 | tag<<2 | bit14 | len<<15 */
	__le32 ch;		/* +16: channel flag (0 = ring A, 1 = ring B) */
	__le32 f20;		/* +20 */
	__le32 attr;		/* +24: 4 */
	__le32 f28;		/* +28 */
};

#define ZX279131_TX_DESC_SIZE	32
#define ZX279131_TX_RING_SIZE	1024
#define ZX279131_TX_DESC_CTRL	0x8000
#define ZX279131_TX_W12_BASE_SW	0x20000005	/* type-1 sw tagged: byte12 bits[6:2]=tag=1 */
#define ZX279131_TX_W12_BASE_A	0x20000023	/* pon / type-2 path */
#define ZX279131_TX_W12_BASE_B	0x20000021	/* sw / type-1 path  */
#define ZX279131_TX_W12_BIT14	0x00004000	/* byte13 bits[6:1] = 32 */
#define ZX279131_TX_F4_SW	0xa0000000
#define ZX279131_TX_F4_SW2	0x80000000
#define ZX279131_TX_ATTR24	4

/* ------------------------------------------------------------------ */
/* RX descriptor (16 bytes, 4096 per ring, 16 rings)                   */
/* ------------------------------------------------------------------ */
/* Field layout comes from pp_net_rx.isra.2 (lsl #4 stepping, wrap test
 * `cmp w0,#0x1,lsl #12`) and was then confirmed field-by-field against a
 * live ring: every decoded (bp,len) pair was found by reading the physical
 * memory it points at, and the frame contents matched (PORT-NOTES 56).
 *
 * Where the payload lives depends on status bit6 - the disassembly picks
 * between two different pools/constants at exactly that bit:
 *   bit6 = 0, single BP : data = PP_POOL_TX + 0x20 + bp * 2432
 *                         (measured; this is the SAME pool TX uses)
 *   bit6 = 1, multi  BP : data = PP_POOL_RX       + bp * 9728
 * PP_POOL_STRIDES = 0x26000980 encodes both strides. */
struct zx279131_rx_desc {
	__le32 f0;		/* +0 : tags; (f0 & 0xc1) == 0xc1 => gro-able */
	u8  port;		/* +4 : bits[7:3] = source port */
	u8  f5;
	__le16 f6;
	__le32 f8;		/* +8 : [31:18]=len [17:2]=bp [1:0]=flags */
	u8  f12;
	u8  f13;
	u8  f14;
	u8  status;		/* +15: bit6 = multi-BP (jumbo) frame */
} __packed;

#define ZX279131_RX_DESC_LEN(d) \
	((le32_to_cpu((d)->f8) >> 18) & 0x3fff)
#define ZX279131_RX_DESC_BP(d) \
	((le32_to_cpu((d)->f8) >> 2) & 0xffff)
#define ZX279131_RX_DESC_PORT(d) (((d)->port >> 3) & 0x1f)
#define ZX279131_RX_DESC_MULTI(d) (((d)->status >> 6) & 1)

/* ------------------------------------------------------------------ */
/* SW: SMAC (per-port blocks at SW + 0x40000 * (port + 1))             */
/* ------------------------------------------------------------------ */
#define SW_SMAC_BLOCK(p)	(0x40000 * ((p) + 1))
#define SW_SMAC_MODE(p)		(0x60 + (p) * 4)	/* bits[25:23] */
#define SW_SMAC_PORTMODE_BIT	0x01000000		/* bit24 */
#define SW_SMAC_MODE_MASK	0x03800000
#define SW_SMAC_MAINCTL		0x000		/* = 0x70e203 */
#define SW_SMAC_MISC8		0x008		/* = 0x80000001 */
#define SW_SMAC_MISCE0		0x0e0		/* = 0x00011200 */
#define SW_SMAC_MISCD00		0xd00		/* |= 2 */
#define SW_SMAC_MISCD30		0xd30		/* |= 0x20 */
#define SW_SMAC_ENABLE_BITS	0x3		/* set = enable port */

#define SW_SMAC_RESET_REG	0x2c0004	/* bit per port, pulse */
#define SW_SOPC_HALFMODE	0x283f0		/* bit per port */
#define SW_SMAC_PORTUP(p)	(0x28000 + (0xa5 + (p)) * 4)	/* bit0 = up */
#define SW_SMAC_PORTACK(p)	(0x28000 + (0xaa + (p)) * 4)	/* write 1 */

/* SMAC speed/duplex bits in the per-port block */
#define SW_SMAC_SPD_BIT15	0x8000
#define SW_SMAC_SPD_BIT14	0x4000
#define SW_SMAC_SPD_BIT13	0x2000

/* ------------------------------------------------------------------ */
/* MDIO (mii_read / mii_write from the 5.4 sibling kernel)             */
/* ------------------------------------------------------------------ */
#define ZX_MDIO_DATA_WR		0x04
#define ZX_MDIO_DATA_RD		0x08
#define ZX_MDIO_STATUS		0x10	/* poll != 0 = done; w0 = clear */
#define ZX_MDIO_CTRL		0x14	/* cmd: 0x5800|reg|phy<<5 (rd) */
#define ZX_MDIO_CLR		0xffffbfff	/* clears bit14 */
#define ZX_MDIO_CMD_READ	0x5800
#define ZX_MDIO_CMD_WRITE	0x5400

#define ZX279131_GPHY_ADDR0	10
#define ZX279131_NUM_GPHY	4

/* GPHY registers (vendor usage) */
#define GPHY_REG_SPEC_CTRL	0x10	/* = 0xb640 */
#define GPHY_REG_SPEC_STAT	0x11	/* = 0x18 / 0x1c (strap) */
#define GPHY_REG_INT_EN		0x18	/* = 5 */
#define GPHY_REG_STATUS26	0x1a	/* bit6 = link, bits[9:8] = speed, bit7 = fd */

/* ------------------------------------------------------------------ */
/* IRQ numbers (DT SPI ids)                                            */
/* ------------------------------------------------------------------ */
#define ZX279131_IRQ_PP		39
#define ZX279131_IRQ_SW		40
#define ZX279131_IRQ_GPHY0	79

/* ------------------------------------------------------------------ */
/* SW switch-fabric subsystem registers (spa/brg/sqmg/sipc/red/cla)    */
/* ------------------------------------------------------------------ */
#define SW_CTRL04		0x04		/* = 0xfffeffff (tm_switch_init) */
#define SW_CTRL48		0x48		/* = 0 */
#define SW_SPA_BASE		0x14000		/* 3 x 0xffffffff */
#define SW_SIPC			0xc000		/* = 0x20000031 */
#define SW_BRG_BASE		0x1c000		/* switch_brg_init */
#define SW_BRG_PORTISOL(p)	(0x1c3c0 + (p) * 4)	/* RMW clear mask */

/* SQMG command interface (sqmg_set_cfg_ram), SW + 0x20000 */
#define SW_SQMG_CMD		0x20014		/* q | mode<<22 */
#define SW_SQMG_STATUS		0x20018		/* poll bit0 */
#define SW_SQMG_CFG0		0x2001c
#define SW_SQMG_CFG1		0x20020
#define SW_SQMG_CFG2		0x20024

/* PP RED command interface (red_set_queue_cfg / pp_red_init) */
#define PP_RED_STATUS		0xc018		/* poll bit0 */
#define PP_RED_CMD		0xc014		/* q | mode<<22 */
#define PP_RED_CFG0		0xc01c
#define PP_RED_CFG1		0xc020
#define PP_RED_CFG2		0xc024
#define PP_RED_CFG3		0xc028

/* PP classifier (pp_cla_init: writes 0x3812 / 4).
 * Decoded from vendor virtual 0xFFFFFFD018004048 via the driver alias
 * rule virtual = physical + 0xFFFFFFD000000000  ->  physical 0x18004048.
 * NOTE: an earlier revision used 0x40048 (0x18040048) - that address is
 * never touched by any vendor code; the real CLA stayed at reset 0. */
#define PP_CLA_0		0x4048
#define PP_CLA_1		0x404c
/* v8.0: the two PP-tail writes U-Boot performs and this driver never did.
 * Recovered by disassembling U-Boot's PP init (entry 0xbc00 -> bl 0xbc70):
 *   [0x18004050] = 0    [0x1800408c] = 1
 * Neither address occurs anywhere else in this driver, nor in the vendor
 * plat/switch modules enumerated so far.  0x408c taking the value 1, right
 * after the CLA pair, looks like an enable/latch bit. */
#define PP_CLA_2		0x4050	/* U-Boot 0; STOCK RUNTIME 0x7FFF479E (v8.8c) */
#define PP_CLA_EN		0x408c	/* U-Boot 1; STOCK RUNTIME 0x372C       */

#endif /* __ZX279131_REGS_H */
