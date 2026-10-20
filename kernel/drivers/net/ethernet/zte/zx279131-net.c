// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279131 packet-engine Ethernet driver (F7610U / zn515xg family).
 *
 * v3: full register-level port of the vendor 4.1.25 "sw" datapath.
 * Every sequence below is strapped to disassembly of the vendor driver
 * (plat_zxylzb_9131.ko, not stripped): pp_dma_init, pp_bmu_init/enable,
 * pp_queue_init, pp_data_raw_send, pp_net_tx (type-1 "sw" path),
 * soft_insert_tx_1desc, pp_check_tx_done_nolock, switch_smac_* / smac_*,
 * phy_process, and mii_read/mii_write.  See work/v3dis/ + NETWORK-DESIGN.md.
 *
 * TX path used here mirrors the vendor "sw" netdev (type 1):
 *   BP alloc -> copy into TX pool -> 32B descriptor -> ring B doorbell.
 * TX completion is observed through the hardware done counter
 * (PP+0x8068) which is logged; the BPs are recycled through the BMU.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "zx279131-regs.h"

/* PON SerDes bring-up lives in zx279131-pon.c (separate register block). */
void zx279131_pon_init(void);
int zx279131_pon_sysfs_add(struct device *dev);
void zx279131_pon_sysfs_del(struct device *dev);

#define DRV_NAME	"zx279131-net"
#define DRV_VERSION	"0.8.7"

/* v8.6 experiment: U-Boot parks its TX rings at 0x80a10000 (A) / 0x80a18000
 * (B) in low DDR, and D1 reserved 0x80000000-0x80B00000 no-map so nothing
 * else touches that region.  `lowring` re-points our rings there.
 *
 * memremap(MEMREMAP_WC), NOT ioremap(): plain ioremap() yields Device/nGnRnE
 * semantics, on which arm64 faults (FSC=0x21 alignment fault) as soon as the
 * compiler coalesces descriptor stores into a wide store - which it does.
 * For a non-RAM region memremap(WC) routes to
 * ioremap_prot(pgprot_writecombine(PAGE_KERNEL)), i.e. Normal Non-cacheable,
 * which is what a DMA descriptor ring needs and matches the semantics of the
 * default dma_alloc_attrs(coherent) path (no cache maintenance required). */
#define ZX_LOWRING_A_PA		0x80a10000
#define ZX_LOWRING_B_PA		0x80a18000
#define ZX_LOWRING_BYTES	(ZX279131_TX_RING_SIZE * ZX279131_TX_DESC_SIZE)

#define ZX279131_NAPI_WEIGHT	512

static int tx_mode = 1;		/* 1/2: sw path (ring B); 3: pon path (ring A) */
module_param(tx_mode, int, 0644);
MODULE_PARM_DESC(tx_mode, "TX descriptor flavour: 1=sw(0xa0000000) 2=sw(0x80000000) 3=pon(ring A)");

static int link_poll_ms = 1000;
module_param(link_poll_ms, int, 0644);
MODULE_PARM_DESC(link_poll_ms, "GPHY link poll interval in ms");

static int phy11 = 0x18;
module_param(phy11, int, 0644);
MODULE_PARM_DESC(phy11, "GPHY register 0x11 value (0x18/0x1c, efuse strap)");

static int selftest = 1;
module_param(selftest, int, 0644);
MODULE_PARM_DESC(selftest, "run the probe-time TX self-test (40 injected frames)");

/* PP_DMA_BURST.  Vendor kernel pp_dma_init() writes 0x00400040; U-Boot -
 * the only known-good PP TX on this SoC - writes 0x00200020 (see
 * PORT-NOTES 23.2).  Exposed so both can be A/B tested without a rebuild:
 *   zx279131.burst=0x200020 */
static int burst = 0x00400040;
module_param(burst, int, 0644);
MODULE_PARM_DESC(burst, "PP_DMA_BURST value (default 0x00400040 vendor, 0x00200020 = U-Boot)");

/* Which DMA/BMU config register set to program.  Our driver mirrors the
 * vendor kernel's pp_dma_init()/pp_bmu_init(); U-Boot - the only proven
 * working PP TX on this board, it shipped 28 MB over ring B during TFTP -
 * leaves a DIFFERENT set behind (uboot1..4 dump).  The two disagree on
 * MODE28 (0x1000 vs 1), DESC_CFG (1 vs 128), MAGIC04 (0 vs 127),
 * QW0/QW1 (1/1 vs 64/64), TIMEOUT (0x1000 vs 20000), BURST
 * (0x00200020 vs 0x00400040) and BMU CFG_C/D (0x40/0x10 vs 0x17f/0xf).
 * Exposed so the two can be A/B tested in a single boot, with the
 * `retrig` sysfs node re-applying the choice at runtime:
 *   zx279131.cfgmode=1      -> U-Boot residue
 *   echo 1 > .../retrig     -> re-apply + re-run the self-test   */
static int cfgmode = 0;
module_param(cfgmode, int, 0644);
MODULE_PARM_DESC(cfgmode, "DMA/BMU config set: 0=vendor kernel (default), 1=U-Boot residue");

/* v7.2: the PP TX-done interrupt turns out to be LOAD-BEARING, not just a
 * notification.  Measured on the stock firmware (PORT-NOTES 28.2/28.3):
 *   - 0x18000104 reads 0xfffffffc (mask bits[1:0] clear = our TX-done A/B
 *     sources enabled);
 *   - one PP interrupt per TX frame (3009 IRQ for 3009 frames);
 *   - writing 0xffffffff to 0x18000104 (mask everything) stops TX dead -
 *     the box stops answering on the wire and needs a power cycle.
 * That is exactly why v5.1..v7.0 all stalled after "10" frames: probe
 * pinned the mask to 0xffffffff.  We now replay the vendor protocol
 * (zx_pp_int -> pp_net_int -> NAPI): mask on entry so the level line
 * drops, reclaim, then re-enable.  ppirq=0 restores poll-only. */
static int ppirq = 1;
module_param(ppirq, int, 0644);
MODULE_PARM_DESC(ppirq, "1 = drive TX-done via the PP interrupt (vendor protocol, default), 0 = mask and poll");

/* U-Boot writes a PP register pair (0x19002040/44 = 0x0fa0000c /
 * 0x3e80000c) through its 0x19000000 alias window.  We worked the alias
 * offset out from ring-B misc (0x1801806c still held U-Boot's TFTP
 * counter), which lands the pair at 0x18012050/0x18012054; the ring-A
 * anchor instead gives 0x18013040/0x18013044.  Neither the vendor kernel
 * nor this driver has ever written either address, and U-Boot's own TX -
 * the only proven-working PP TX on this board - runs with them set.
 * The boot dump shows whether U-Boot's write survived; then both
 * candidate pairs are programmed so one boot tests both.
 *   zx279131.ubq=0   -> do nothing (leave the region untouched) */
static int ubq = 1;
module_param(ubq, int, 0644);
MODULE_PARM_DESC(ubq, "program U-Boot's extra PP pair at both alias-derived offsets");

/* U-Boot's switch-core release (bootloader function 0x84029690) forces
 * 0x1a000008 and 0x1a00000c to all-ones after a 2 ms delay.  Neither the
 * vendor plat module nor switch.ko ever writes them, and our probe reads
 * back the reset defaults (0xe001e0 / 0x0) - i.e. whatever the switch's
 * port matrix is meant to be, the stock kernel installs it through the
 * switch.ko ACL/stream-select rules instead, which we do not have.
 * U-Boot's own TX - the only proven-working PP TX on this board - runs
 * with these set.  Exposed so both can be A/B tested in one boot:
 *   zx279131.swcfg=0   -> vendor-kernel baseline (default 1) */
static int swcfg = 0;
module_param(swcfg, int, 0644);
MODULE_PARM_DESC(swcfg, "write U-Boot's 0x1a000008/0c = ~0 (0 = leave stock defaults)");

/* The stock kernel's switch port-mode registers (probed with devmem2 on the
 * vendor firmware, probe-net1/probe-stock-dma6):
 *   0x1a000060 (port 0) = 0x01201000     0x1a000078 (port 6) = 0x00A01000
 * Ports 0..3 use mode field bits[25:23]=2 (bit24); port 6 - the CPU/PP
 * port - uses mode 1 (bit23).  This driver only ever programmed 0..3, so
 * port 6 is the one switch-port config stock has and we never wrote.
 * The write is skipped when the register already holds the stock value. */
static int swport = 1;
module_param(swport, int, 0644);
MODULE_PARM_DESC(swport, "program switch port 6 (CPU/PP port) mode = 0xA01000");

/* v8.0: U-Boot's PP register footprint.  Disassembling U-Boot's PP init
 * (backup/mtd1-uboot.bin, entry 0xbc00 -> bl 0xbc70) turned up the last
 * places where its PP setup still differs from ours:
 *   [0x18004050] = 0   [0x1800408c] = 1  -> already the reset values
 *                                           (zx_regs CLA.008C reads 1), so
 *                                           writing them is a no-op that
 *                                           only makes the footprint equal
 *   CLA pair: U-Boot 0x33092 / 2   vs   stock kernel 0x3812 / 4
 *   BMU POOLCNT: U-Boot 2048 / 512 vs   ours tx_bp_count / table entries
 * The last two have never been A/B tested.  All three are switchable so a
 * single boot (via `retrig`) can sweep them. */
static int ubtail = 1;
module_param(ubtail, int, 0644);
MODULE_PARM_DESC(ubtail, "apply the PP-tail CLA block (0x4050/0x408c = cla2/claen)");

static int ubcla = 0;
module_param(ubcla, int, 0644);
MODULE_PARM_DESC(ubcla, "use U-Boot's CLA pair 0x33092/2 instead of stock 0x3812/4");

static int ubpool = 0;
module_param(ubpool, int, 0644);
MODULE_PARM_DESC(ubpool, "use U-Boot's BMU POOLCNT 2048/512 instead of ours");

/* v8.8c: CLA control block, copied from the STOCK firmware's *runtime* state.
 *
 * U-Boot leaves 0x4050 = 0; this driver used to keep it that way (the "ubtail"
 * no-op).  Reading the running stock firmware (see PORT-NOTES 60.6) shows it
 * does NOT stay 0 there: 0x18004050 = 0x7FFF479E, and that value decodes with
 * the vendor's own field layout (xtm `cla_set_config`, reg id 3, mask 0xf87f)
 * to exactly the fields its own `cat gclacfg` prints:
 *
 *   bit15 MAC Req Ctrl = 0 Pass-Through   bit14 Dn Broadcast = 1 Register
 *   bit13 Dn Multicast = 0 Acl            bit12 Dn Unicast   = 0 Acl
 *   bit11 Up Unicast   = 0 Acl            bit6  Trap Cfg     = 0 (trap pkts
 *                                                               not in flow)
 *   bit5:4 Dslite Del  = 1              bits 16..29 = 0x3FFF (all set)
 *
 * The bits16..29 word is the interesting one: 14 consecutive enables that are
 * all-ones on stock and zero here.  Trap-Reason 0x48..0x55 is exactly 14 codes
 * and holds the ones that make *host* traffic reach the stack --
 * 0x51 ZX_TM_TRAP_L3_LOCAL, 0x53 ZX_TM_TRAP_TCP_CTRL, 0x54
 * ZX_TM_TRAP_LOOK_UP_MISS -- while ARP (0x11) and ICMPv4 (0x1e), which DO work
 * on this driver, sit outside that window.  That is the working hypothesis for
 * "ICMP/ARP pass, TCP/UDP never arrive".
 *
 * Both registers are params so one boot can sweep them (no reflash):
 *   echo 0 > /sys/module/zx279131_net/parameters/cla2 && echo 1 > $P/retrig
 */
static uint cla2 = 0x7FFF479E;
module_param(cla2, uint, 0644);
MODULE_PARM_DESC(cla2, "0x18004050 value (stock runtime = 0x7FFF479E; U-Boot = 0)");

static uint claen = 1;
module_param(claen, uint, 0644);
MODULE_PARM_DESC(claen, "0x1800408c value (stock runtime = 0x372C; U-Boot = 1)");

/* v8.3: SW-init stage mask - "did we do too much?".
 *
 * U-Boot pushes 19088 TFTP frames through this very engine having programmed
 * far LESS of the switch than this driver does: BRG + SPA + SQMG's 0x20040/
 * 0x20044 pair + SMAC, and nothing else (PORT-NOTES 42.3).  We additionally
 * run the full SQMG config sweep, SIPC and RED/CLA.  If one of those extra
 * stages is what closes the PP -> switch data path, clearing its bit should
 * bring TX back.
 *
 * Bit mask so a single boot can sweep the combinations at runtime:
 *   bit0 SPA      bit1 BRG      bit2 SQMG (full)      bit3 SIPC
 * Default 0xf = current behaviour.  Re-applied by the `retrig` node, so the
 * sweep costs no reflash:
 *   echo 6 > /sys/module/zx279131_net/parameters/swmsk && echo 1 > $P/retrig
 * (RED is still governed by `redn`, the CLA pair by `ubtail`/`ubcla`.) */
static u32 swmsk = 0xf;
module_param(swmsk, uint, 0644);
MODULE_PARM_DESC(swmsk, "SW init stages: bit0 SPA, bit1 BRG, bit2 SQMG, bit3 SIPC (default 0xf)");

/* v8.5: TX descriptor w12 (type/tag/length word) base-bit override.
 *
 * The U-Boot ring B that D1's reserved-memory kept alive shows, for its
 * 46-byte TFTP ACK frames:
 *     w12 = 0x20172001   (46 << 15 = 0x170000, so base bits = 0x20002001)
 * while this driver writes, for its 64-byte selftest frames:
 *     w12 = 0x20204005   (BASE_SW 0x20000005 | BIT14 | 64 << 15)
 * i.e. we set BIT14 (and BIT2) where U-Boot sets BIT13.  U-Boot's TX path
 * demonstrably completes (54722 descriptors), ours wedges at comp=10.
 *
 * Let the base bits be swept at runtime; the length field is always OR-ed
 * in from the real frame size.
 *   echo 0x20002001 > .../w12base    # U-Boot's base bits
 *   echo 0x20004005 > .../w12base    # our current base bits
 *   echo 0           > .../w12base    # back to the per-tx_mode default
 * Then: echo 1 > .../retrig          # re-runs the self-test
 */
static u32 w12base;
module_param(w12base, uint, 0644);
MODULE_PARM_DESC(w12base, "override TX desc w12 base bits (0 = per-tx_mode default)");

/* v8.1: U-Boot's PP init (objdump 0xba10) programs the RED block with exactly
 * nine (queue, ram) commands - q=0..7 with ram 0, then q=8 with ram 6 - and
 * each one is preceded by a successful STATUS-bit0 poll.  This driver used to
 * issue 264 + 9 of them, and on the live board U-Boot's RED block reads back
 * CMD=0x01800008 / STATUS=0x1 / CFG0=0x00800400 while ours reads
 * CMD=0 / STATUS=0xa / CFG0=0xc, i.e. none of our writes stick.  redn=1
 * replays U-Boot's known-good 9-command sequence; redn=0 keeps the legacy
 * 264+9 so both can be compared inside one boot. */
static int redn = 1;
module_param(redn, int, 0644);
MODULE_PARM_DESC(redn, "RED init: 1 = U-Boot's 8+1 sequence (default), 0 = legacy 264+9");

/* v8.6: place the TX rings at U-Boot's low-DDR addresses (0x80a10000 /
 * 0x80a18000) instead of a kernel-allocated coherent buffer.  See
 * ZX_LOWRING_* above.  Requires the D1 DTS reserved-memory(no-map) covering
 * 0x80000000-0x80B00000 and the D1 .its kernel load address (0x8C000000). */
static int lowring;
module_param(lowring, int, 0644);
MODULE_PARM_DESC(lowring, "1 = TX rings via memremap(WC) at U-Boot's low-DDR addresses");

/* v8.7: U-Boot's TX path places the frame data 0x10 bytes into each pool
 * slot (objdump 0x8400c1bc: pool + idx*2048 + 0x10), while this driver uses
 * 0x20.  Expose the offset so the two can be A/B-compared: U-Boot's own
 * submit path is the only known-good reference on this silicon. */
static uint bufoff = 0x20;
module_param(bufoff, uint, 0644);
MODULE_PARM_DESC(bufoff, "frame data offset inside a TX pool slot (default 0x20, U-Boot uses 0x10)");

/* v8.8: minimal RX consumption.
 *
 * The hardware side has always worked - RXQ0 climbs as soon as traffic
 * arrives - but this driver never read a single RX descriptor, so the
 * stack saw zero packets (PORT-NOTES 55).  Where a received frame actually
 * lands was not guessable from the source alone: it was pinned down by
 * reading descriptor fields off the live box and then peeking the physical
 * memory they point at (PORT-NOTES 56):
 *
 *   single-BP frame : PP_POOL_TX + 0x20 + bp * 2432   <- the TX pool
 *   multi-BP (jumbo): PP_POOL_RX       + bp * 9728
 *
 * So RX and TX share one buffer pool, exactly as PP_POOL_STRIDES'
 * 0x0980/0x2600 pair and the two branches of pp_net_rx.isra.2 imply.
 *
 * Every knob below is a module_param so this can be disabled or
 * re-pointed at runtime, without a reflash. */
static int rxen = 1;
module_param(rxen, int, 0644);
MODULE_PARM_DESC(rxen, "1 = consume RX descriptors and hand the frames to the stack");

static uint rxoff = 0x20;
module_param(rxoff, uint, 0644);
MODULE_PARM_DESC(rxoff, "frame data offset inside an RX pool slot (measured: 0x20)");

static uint rxstride = 2432;
module_param(rxstride, uint, 0644);
MODULE_PARM_DESC(rxstride, "RX slot stride for single-BP frames (measured: 2432, the TX stride)");

static int rxpoll_ms = 2;
module_param(rxpoll_ms, int, 0644);
MODULE_PARM_DESC(rxpoll_ms, "RX poll interval in ms (0 = only poll from the TX-done work)");

static uint rxport = 0xff;
module_param(rxport, uint, 0644);
MODULE_PARM_DESC(rxport, "bitmask of descriptor source ports to accept (default 0xff = all)");

static int rxfreen = 1;
module_param(rxfreen, int, 0644);
MODULE_PARM_DESC(rxfreen, "1 = return each consumed BP to the BMU (0 = leak it, for A/B testing)");

static int rxrel;
module_param(rxrel, int, 0644);
MODULE_PARM_DESC(rxrel, "1 = also release descriptors via PP_RXREL_* (unused in v8.8)");

static uint rxbudget = 64;
module_param(rxbudget, uint, 0644);
MODULE_PARM_DESC(rxbudget, "max RX descriptors consumed per poll");

struct zx279131_priv {
	struct net_device	*ndev;
	struct device		*dev;

	void __iomem		*pp;
	void __iomem		*sw;
	void __iomem		*mdio;
	void __iomem		*idm;

	/* TX rings (32 KiB each, 1024 x 32 B, coherent) */
	struct zx279131_tx_desc	*tx_ring_a;
	dma_addr_t		tx_ring_a_dma;
	struct zx279131_tx_desc	*tx_ring_b;
	dma_addr_t		tx_ring_b_dma;

	/* BMU tables + pools (all coherent) */
	void			*bmu_area;	/* tbl0 + tbl1 + misc */
	dma_addr_t		bmu_area_dma;
	u16			*bp_tbl0;
	dma_addr_t		bp_tbl0_dma;
	u16			*bp_tbl1;
	dma_addr_t		bp_tbl1_dma;
	void			*rx_rings;
	dma_addr_t		rx_rings_dma;
	void			*tx_pool;
	dma_addr_t		tx_pool_dma;
	void			*rx_pool;
	dma_addr_t		rx_pool_dma;
	u32			tx_bp_count;

	/* ring bookkeeping (we submit on ring B or A per tx_mode) */
	struct zx279131_tx_desc	*tx_ring;
	dma_addr_t		tx_ring_dma;
	u32			tx_submit_idx;
	u32			tx_submitted;
	u32			tx_completed;
	u32			dbg_done_last;
	int			cur_mode;
	u16			bp_history[ZX279131_TX_RING_SIZE];
	u32			doorbell_off;
	u32			done_off;

	/* debug */
	u32			dbg_tx_pkts;
	u32			dbg_bp_alloc_fail;
	u32			dbg_done_stuck;
	bool			port_up;

	/* link state per GPHY (vendor g_phy state array) */
	int			phy_state[ZX279131_NUM_GPHY];
	bool			port_acked[ZX279131_NUM_GPHY];
	struct delayed_work	link_work;

	/* v7.2: PP TX-done interrupt, vendor zx_pp_int/pp_net_int protocol */
	int			pp_irq;
	struct delayed_work	pp_work;
	u32			irq_count;
	u32			irq_stale;
	bool			irq_gave_up;

	/* v8.8: RX consumption (ring 0) */
	struct delayed_work	rx_work;
	u32			rx_read_total;	/* monotonic consumer cursor */
	u32			rx_prod_last;	/* last producer count seen */
	u32			dbg_rx_pkts;
	u32			dbg_rx_bytes;
	u32			dbg_rx_bad;
	u32			dbg_rx_multi;
	u32			dbg_rx_skb_fail;
	u32			dbg_rx_resync;
	u32			dbg_rx_port[ZX279131_NUM_RX_QUEUES];
};

/* ------------------------------------------------------------------ */
/* low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static inline void zx_wr(struct zx279131_priv *priv, u32 off, u32 val)
{
	iowrite32(val, priv->pp + off);
}

static inline u32 zx_rd(struct zx279131_priv *priv, u32 off)
{
	return ioread32(priv->pp + off);
}

static inline void sw_wr(struct zx279131_priv *priv, u32 off, u32 val)
{
	iowrite32(val, priv->sw + off);
}

static inline u32 sw_rd(struct zx279131_priv *priv, u32 off)
{
	return ioread32(priv->sw + off);
}

/* BMU window is PP + 0x10000 */
static inline void bmu_wr(struct zx279131_priv *priv, u32 off, u32 val)
{
	iowrite32(val, priv->pp + 0x10000 + off);
}

static inline u32 bmu_rd(struct zx279131_priv *priv, u32 off)
{
	return ioread32(priv->pp + 0x10000 + off);
}

/* ------------------------------------------------------------------ */
/* MDIO (mii_read / mii_write port)                                    */
/* ------------------------------------------------------------------ */

static int zx_mdio_read(struct zx279131_priv *priv, u32 phy, u32 reg)
{
	void __iomem *base = priv->mdio;
	u32 v;
	int i;

	v = readl(base + ZX_MDIO_CTRL);
	writel(v & ZX_MDIO_CLR, base + ZX_MDIO_CTRL);
	writel(0, base + ZX_MDIO_STATUS);
	writel(ZX_MDIO_CMD_READ | (reg & 0x1f) | ((phy & 0x1f) << 5),
	       base + ZX_MDIO_CTRL);

	for (i = 0; i < 1000; i++) {
		if (readl(base + ZX_MDIO_STATUS))
			break;
		udelay(10);
	}
	writel(0, base + ZX_MDIO_STATUS);
	v = readl(base + ZX_MDIO_CTRL);
	writel(v & ZX_MDIO_CLR, base + ZX_MDIO_CTRL);
	return readl(base + ZX_MDIO_DATA_RD) & 0xffff;
}

static void zx_mdio_write(struct zx279131_priv *priv, u32 phy, u32 reg, u16 val)
{
	void __iomem *base = priv->mdio;
	u32 v;
	int i;

	v = readl(base + ZX_MDIO_CTRL);
	writel(v & ZX_MDIO_CLR, base + ZX_MDIO_CTRL);
	writel(0, base + ZX_MDIO_STATUS);
	writel(val, base + ZX_MDIO_DATA_WR);
	writel(ZX_MDIO_CMD_WRITE | (reg & 0x1f) | ((phy & 0x1f) << 5),
	       base + ZX_MDIO_CTRL);

	for (i = 0; i < 1000; i++) {
		if (readl(base + ZX_MDIO_STATUS))
			break;
		udelay(10);
	}
	writel(0, base + ZX_MDIO_STATUS);
	v = readl(base + ZX_MDIO_CTRL);
	writel(v & ZX_MDIO_CLR, base + ZX_MDIO_CTRL);
}

/* ------------------------------------------------------------------ */
/* SMAC (switch MAC per port)                                          */
/* ------------------------------------------------------------------ */

static void zx_smac_mode(struct zx279131_priv *priv, int port)
{
	u32 off = SW_SMAC_MODE(port);
	u32 v = sw_rd(priv, off);

	v &= ~SW_SMAC_MODE_MASK;
	v |= SW_SMAC_PORTMODE_BIT;
	sw_wr(priv, off, v);
}

static void zx_smac_reset(struct zx279131_priv *priv, u32 mask)
{
	u32 v = sw_rd(priv, SW_SMAC_RESET_REG);

	sw_wr(priv, SW_SMAC_RESET_REG, v & ~mask);
	udelay(1000);
	sw_wr(priv, SW_SMAC_RESET_REG, mask | v);
	udelay(500);
}

/* value = 0 -> default 0x70e203 (flag 0 path); otherwise the given word */
static void zx_smac_init(struct zx279131_priv *priv, int port, u32 value)
{
	u32 base = SW_SMAC_BLOCK(port);
	u32 w6, v;

	if (value == 0) {
		value = 0x0070e203;
		w6 = SW_SMAC_SPD_BIT13;		/* flag==0 -> 0x2000 */
	} else {
		w6 = value & SW_SMAC_SPD_BIT13;
	}

	sw_wr(priv, base + SW_SMAC_MAINCTL, value);
	sw_wr(priv, base + SW_SMAC_MISC8, 0x80000001);
	sw_wr(priv, base + SW_SMAC_MISCE0, 0x00011200);
	v = sw_rd(priv, base + SW_SMAC_MISCD00);
	sw_wr(priv, base + SW_SMAC_MISCD00, v | 2);
	v = sw_rd(priv, base + SW_SMAC_MISCD30);
	sw_wr(priv, base + SW_SMAC_MISCD30, v | 0x20);

	/* sopc_set_half_mode(port, w6): nonzero clears the per-port bit */
	v = sw_rd(priv, SW_SOPC_HALFMODE);
	if (w6)
		sw_wr(priv, SW_SOPC_HALFMODE, v & ~(1u << port));
	else
		sw_wr(priv, SW_SOPC_HALFMODE, v | (1u << port));
}

static void zx_smac_enable(struct zx279131_priv *priv, int port)
{
	u32 off = SW_SMAC_BLOCK(port);
	u32 v = sw_rd(priv, off);

	sw_wr(priv, off, v | SW_SMAC_ENABLE_BITS);
}

static void zx_smac_disable(struct zx279131_priv *priv, int port)
{
	u32 off = SW_SMAC_BLOCK(port);
	u32 v = sw_rd(priv, off);

	sw_wr(priv, off, v & ~SW_SMAC_ENABLE_BITS);
}

/* port speed/duplex, exactly mirroring switch_smac_config_speed_duplex() */
static void zx_smac_speed_duplex(struct zx279131_priv *priv, int port,
				 int speed, int duplex)
{
	u32 off = SW_SMAC_BLOCK(port);
	u32 old = sw_rd(priv, off);
	u32 bit13 = old & SW_SMAC_SPD_BIT13;
	u32 new;

	if (speed == 2) {
		new = (old & ~SW_SMAC_SPD_BIT15) | SW_SMAC_SPD_BIT13;
	} else {
		if (duplex)
			new = old | (SW_SMAC_SPD_BIT15 | SW_SMAC_SPD_BIT13);
		else
			new = (old & ~SW_SMAC_SPD_BIT13) | SW_SMAC_SPD_BIT15;
		if (speed == 1)
			new &= ~SW_SMAC_SPD_BIT14;
		else
			new |= SW_SMAC_SPD_BIT14;
	}

	if ((new & SW_SMAC_SPD_BIT13) != bit13) {
		u32 half = new & SW_SMAC_SPD_BIT13;
		u32 v = sw_rd(priv, SW_SOPC_HALFMODE);

		/* sopc_set_half_mode: nonzero value clears, zero sets */
		if (half)
			sw_wr(priv, SW_SOPC_HALFMODE, v & ~(1u << port));
		else
			sw_wr(priv, SW_SOPC_HALFMODE, v | (1u << port));
		zx_smac_reset(priv, 1u << port);
		zx_smac_init(priv, port, new);
	} else {
		sw_wr(priv, off, new);
	}
}

/* wait for the port-up handshake bit, then acknowledge it */
static void zx_smac_port_up_wait(struct zx279131_priv *priv, int port,
				 bool patient)
{
	int i, limit = patient ? 1000 : 200;

	for (i = 0; i < limit; i++) {
		if (sw_rd(priv, SW_SMAC_PORTUP(port)) & 1) {
			sw_wr(priv, SW_SMAC_PORTACK(port), 1);
			priv->port_acked[port] = true;
			return;
		}
		if (patient)
			msleep(1);
		else
			udelay(200);
	}
}

static void zx_sqmg_set_cfg(struct zx279131_priv *priv, u32 q,
			    const u32 *cfg, u32 mode)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (sw_rd(priv, SW_SQMG_STATUS) & 1)
			break;
		cpu_relax();
	}
	sw_wr(priv, SW_SQMG_CMD, (q & 0xffff) | (mode << 22));
	sw_wr(priv, SW_SQMG_CFG0, cfg[0]);
	sw_wr(priv, SW_SQMG_CFG1, cfg[1]);
	sw_wr(priv, SW_SQMG_CFG2, cfg[2]);
}

/* v8.1: U-Boot's RED writer (objdump 0xba10) polls STATUS bit0 *before* every
 * command and gives up with an error print if it never comes up - i.e. a
 * command issued while the block is not idle is meaningless.  The old code
 * spun 200 times and then wrote anyway, silently, so 273 such writes could all
 * be going nowhere.  Keep the counter so a real failure is visible. */
static unsigned int zx_red_notready;

static void zx_red_set_cfg(struct zx279131_priv *priv, u32 q,
			   const u32 *cfg, u32 mode)
{
	int i;

	/* wait for idle (STATUS bit0) before issuing the command */
	for (i = 0; i < 1000; i++) {
		if (zx_rd(priv, PP_RED_STATUS) & 1)
			break;
		cpu_relax();
	}
	if (!(zx_rd(priv, PP_RED_STATUS) & 1) && zx_red_notready++ < 4)
		dev_warn(priv->dev,
			 "red: q=%u ram=%u issued while NOT ready, status=%#x\n",
			 q, mode, zx_rd(priv, PP_RED_STATUS));

	zx_wr(priv, PP_RED_CMD, (q & 0xffff) | (mode << 22));
	zx_wr(priv, PP_RED_CFG0, cfg[0]);
	zx_wr(priv, PP_RED_CFG1, cfg[1]);
	zx_wr(priv, PP_RED_CFG2, cfg[2]);
	zx_wr(priv, PP_RED_CFG3, cfg[3]);

	/* ... and wait for the block to go idle again (U-Boot loops straight
	 * back into the poll, so the wait is part of the known-good sequence). */
	for (i = 0; i < 1000; i++) {
		if (zx_rd(priv, PP_RED_STATUS) & 1)
			break;
		cpu_relax();
	}
}

/* v8.0: write the PP-tail block recovered from U-Boot's PP init sequence
 * (objdump of backup/mtd1-uboot.bin; the caller at 0xbc14 does
 *   CLA = 0x33092 / 2, then 0x18004050 = 0 and 0x1800408c = 1).
 * Called from fabric_init at probe time and from the `retrig` node, so
 * both settings can be compared inside one boot.  v8.8c replaces U-Boot's
 * 0x4050=0 / 0x408c=1 with the values the *stock* firmware runs with (see the
 * cla2/claen params above); both are switchable at runtime. */
static void zx279131_ub_tail(struct zx279131_priv *priv)
{
	u32 b50, b8c;

	/* CLA pair: stock kernel 0x3812/4, U-Boot 0x33092/2 */
	zx_wr(priv, PP_CLA_0, ubcla ? 0x33092 : 0x3812);
	zx_wr(priv, PP_CLA_1, ubcla ? 2 : 4);

	if (!ubtail)
		return;

	b50 = zx_rd(priv, PP_CLA_2);
	b8c = zx_rd(priv, PP_CLA_EN);
	zx_wr(priv, PP_CLA_2, cla2);
	zx_wr(priv, PP_CLA_EN, claen);
	dev_info(priv->dev,
		 "ubtail: cla=%#x/%#x 4050 %#x->%#x 408c %#x->%#x\n",
		 zx_rd(priv, PP_CLA_0), zx_rd(priv, PP_CLA_1),
		 b50, zx_rd(priv, PP_CLA_2), b8c, zx_rd(priv, PP_CLA_EN));
}

/* v8.2: M48 / INT_STATUS staged snapshot.
 *
 * D1 (2026-10-17) proved the bootloader's engine state really does survive
 * into probe: the uboot2/ppring2 dumps read PP_DMA_M48 = 0x8 (bit3) before
 * this driver writes anything, while a wedged TX shows INT_STATUS stuck at
 * bit1.  §38.18 had read those two as "hardware-driven symptom bits"; D1
 * showed the good signature is present up front and therefore gets *clear*
 * later.  Snapshot both - plus the TXFREE and ring-B pair they sit next to -
 * at every init stage so one boot shows which write flips 0x8 -> 0x2.
 * See PORT-NOTES 40.5 / 40.7. */
static void zx279131_m48_snap(struct zx279131_priv *priv, const char *tag)
{
	dev_info(priv->dev,
		 "m48[%s]: 040=%#x 044=%#x 048=%#x 04c=%#x 100=%#x 104=%#x rB=%#x/%#x\n",
		 tag,
		 zx_rd(priv, PP_DMA_M40), zx_rd(priv, PP_DMA_M44),
		 zx_rd(priv, PP_DMA_M48), zx_rd(priv, PP_DMA_TXFREE),
		 zx_rd(priv, PP_INT_STATUS), zx_rd(priv, PP_INT_MASK),
		 zx_rd(priv, PP_DMA_RINGB_BASE), zx_rd(priv, PP_DMA_RINGB_MISC));
}

/* Switch-fabric bring-up: switch_spa/brg/sqmg/sipc_init + pp_red/cla_init
 * + the tm_switch_init tail, replayed from the vendor disassembly. */
static void zx279131_fabric_init(struct zx279131_priv *priv)
{
	static const u32 sqmg_cfg[4] = { 0x80, 0, 0, 0 };
	static const u32 red_cfg[4] = { 0x00800400, 0, 0, 0 };
	u32 i;

	/* U-Boot's switch-core release, before anything else is programmed
	 * (bootloader 0x84029690).  The stock kernel reaches the same state
	 * through switch.ko rules; we have neither, and the reset defaults
	 * (0xe001e0 / 0x0) are what our probe reads back. */
	if (swcfg) {
		u32 b4 = sw_rd(priv, 0x0008);
		u32 bc = sw_rd(priv, 0x000c);

		sw_wr(priv, 0x0008, 0xffffffff);
		sw_wr(priv, 0x000c, 0xffffffff);
		dev_info(priv->dev,
			 "swcfg: 0x1a000008 %#x->%#x  0x1a00000c %#x->%#x\n",
			 b4, sw_rd(priv, 0x0008), bc, sw_rd(priv, 0x000c));
	}

	/* switch_spa_init */
	if (swmsk & 0x1) {
		for (i = 0; i < 3; i++)
			sw_wr(priv, SW_SPA_BASE + i * 4, 0xffffffff);
	} else {
		dev_info(priv->dev, "swmsk: SPA init skipped\n");
	}

	/* switch_brg_init */
	if (swmsk & 0x2) {
		sw_wr(priv, SW_BRG_BASE + 0x004, 0x002003ff);
		sw_wr(priv, SW_BRG_BASE + 0x340, 0x155557ff);
		sw_wr(priv, SW_BRG_BASE + 0x344, 24);
		sw_wr(priv, SW_BRG_BASE + 0x380, 0x319);
		sw_wr(priv, SW_BRG_BASE + 0x63c, 0xaaaaaaaa);
		sw_wr(priv, SW_BRG_BASE + 0x1c0, 0x3ff);
		sw_wr(priv, SW_BRG_BASE + 0x1c4, 0x00055555);
		sw_wr(priv, SW_BRG_BASE + 0x188, 0x000ff654);
		sw_wr(priv, SW_BRG_BASE + 0x2c0, 0xffffffff);
		sw_wr(priv, SW_BRG_BASE + 0x300, 0x00ffffff);
		sw_wr(priv, SW_BRG_BASE + 0x304, 24);
		sw_wr(priv, SW_BRG_BASE + 0x050, 0xfffffffa);
		sw_wr(priv, SW_BRG_PORTISOL(8),
		      sw_rd(priv, SW_BRG_PORTISOL(8)) & ~0x200u);
		sw_wr(priv, SW_BRG_PORTISOL(9),
		      sw_rd(priv, SW_BRG_PORTISOL(9)) & ~0x100u);
		sw_wr(priv, SW_BRG_BASE + 0x014, 0x01000000);
		sw_wr(priv, SW_BRG_BASE + 0x01c, 0x00078001);
		sw_wr(priv, SW_BRG_BASE + 0x008, 0x00260000);
	} else {
		dev_info(priv->dev, "swmsk: BRG init skipped\n");
	}

	/* switch_sqmg_init */
	if (swmsk & 0x4) {
		for (i = 0; i < 0x50; i++)
			zx_sqmg_set_cfg(priv, i, sqmg_cfg, 2);
		sw_wr(priv, 0x20044, 0x400);
		sw_wr(priv, 0x20040, 0x400);
		for (i = 0; i < 0x50; i++)
			zx_sqmg_set_cfg(priv, i, sqmg_cfg, 0);
		sw_wr(priv, 0x2006c, 0x800);
		sw_wr(priv, 0x20034, 1);
		sw_wr(priv, 0x20038, 1);
	} else {
		dev_info(priv->dev, "swmsk: SQMG init skipped\n");
	}

	/* switch_sipc_init */
	if (swmsk & 0x8)
		sw_wr(priv, SW_SIPC, 0x20000031);
	else
		dev_info(priv->dev, "swmsk: SIPC init skipped\n");

	/* pp_red_init - v8.1: redn=1 replays U-Boot's 9-command sequence */
	if (redn) {
		for (i = 0; i < 8; i++)
			zx_red_set_cfg(priv, i, red_cfg, 0);
		zx_red_set_cfg(priv, 8, red_cfg, 6);
	} else {
		for (i = 0; i < 0x108; i++)
			zx_red_set_cfg(priv, i, red_cfg, 0);
		for (i = 0; i < 9; i++)
			zx_red_set_cfg(priv, i, red_cfg, 6);
	}
	zx_wr(priv, 0xc004, 8);
	zx_wr(priv, 0xc060, 7);

	dev_info(priv->dev,
		 "red: redn=%d status=%#x cmd=%#x cfg0=%#x notready=%u swmsk=%#x\n",
		 redn, zx_rd(priv, PP_RED_STATUS), zx_rd(priv, PP_RED_CMD),
		 zx_rd(priv, PP_RED_CFG0), zx_red_notready, swmsk);

	/* pp_cla_init + v8.0 U-Boot PP-tail (CLA pair + 0x4050/0x408c) */
	zx279131_ub_tail(priv);

	/* tm_switch_init tail */
	sw_wr(priv, SW_CTRL04, 0xfffeffff);
	sw_wr(priv, SW_CTRL48, 0);

	dev_info(priv->dev,
		 "fabric: sw04=%#x sw48=%#x brg04=%#x sqmg14=%#x sipc=%#x red=%#x cla=%#x/%#x idm=%#x\n",
		 sw_rd(priv, 0x04), sw_rd(priv, 0x48), sw_rd(priv, 0x1c004),
		 sw_rd(priv, 0x20014), sw_rd(priv, 0xc000),
		 zx_rd(priv, 0xc004), zx_rd(priv, PP_CLA_0),
		 zx_rd(priv, PP_CLA_1),
		 priv->idm ? readl(priv->idm) : 0);
}

/* ------------------------------------------------------------------ */
/* DMA / BMU init                                                      */
/* ------------------------------------------------------------------ */

static void zx279131_dma_init(struct zx279131_priv *priv)
{
	u32 v;

	/* dma_cfg_byteorder(1) */
	v = zx_rd(priv, PP_DMA_CTRL0);
	v &= 0xfff0ffff;
	v |= 0x000f0000;
	v &= 0xff7fffff;
	zx_wr(priv, PP_DMA_CTRL0, v);

	/* exactly the pp_dma_init() write order; cfgmode=1 replays U-Boot's
	 * residue instead (see the cfgmode comment above) */
	if (cfgmode == 1) {
		zx_wr(priv, PP_DMA_MODE28, 0x1000);
		zx_wr(priv, PP_DMA_MODE2C, 0x1);
		zx_wr(priv, PP_DMA_TIMEOUT, 0x1000);
		zx_wr(priv, PP_DMA_DESC_CFG, 0x1);
		zx_wr(priv, PP_DMA_MAGIC04, 0x0);
		zx_wr(priv, PP_DMA_QW0, 0x1);
		zx_wr(priv, PP_DMA_QW1, 0x1);
	} else {
		zx_wr(priv, PP_DMA_MODE28, 1);
		zx_wr(priv, PP_DMA_MODE2C, 1);
		zx_wr(priv, PP_DMA_TIMEOUT, 20000);
		zx_wr(priv, PP_DMA_DESC_CFG, 128);
		zx_wr(priv, PP_DMA_MAGIC04, 127);
		zx_wr(priv, PP_DMA_QW0, 64);
		zx_wr(priv, PP_DMA_QW1, 64);
	}

	zx_wr(priv, PP_DMA_RINGA_BASE, lower_32_bits(priv->tx_ring_a_dma));
	zx_wr(priv, PP_DMA_RINGB_BASE, lower_32_bits(priv->tx_ring_b_dma));

	/* vendor pp_dma_init writes the burst register last */
	zx_wr(priv, PP_DMA_BURST, cfgmode == 1 ? 0x00200020u : (u32)burst);

	/* U-Boot's extra PP pair (see the ubq comment above).  Dump the
	 * neighbourhood first - if U-Boot's write survived, the values show
	 * up and pin the alias; then program both candidate pairs. */
	if (ubq) {
		dev_info(priv->dev,
			 "ubq A: 12048=%#x 1204c=%#x 12050=%#x 12054=%#x 12058=%#x 1205c=%#x\n",
			 zx_rd(priv, 0x12048), zx_rd(priv, 0x1204c),
			 zx_rd(priv, 0x12050), zx_rd(priv, 0x12054),
			 zx_rd(priv, 0x12058), zx_rd(priv, 0x1205c));
		dev_info(priv->dev,
			 "ubq B: 13038=%#x 1303c=%#x 13040=%#x 13044=%#x 13048=%#x 1304c=%#x\n",
			 zx_rd(priv, 0x13038), zx_rd(priv, 0x1303c),
			 zx_rd(priv, 0x13040), zx_rd(priv, 0x13044),
			 zx_rd(priv, 0x13048), zx_rd(priv, 0x1304c));
		zx_wr(priv, PP_UBQ_A0, 0x0fa0000c);
		zx_wr(priv, PP_UBQ_A1, 0x3e80000c);
		zx_wr(priv, PP_UBQ_B0, 0x0fa0000c);
		zx_wr(priv, PP_UBQ_B1, 0x3e80000c);
		dev_info(priv->dev,
			 "ubq applied: 12050=%#x 12054=%#x 13040=%#x 13044=%#x\n",
			 zx_rd(priv, PP_UBQ_A0), zx_rd(priv, PP_UBQ_A1),
			 zx_rd(priv, PP_UBQ_B0), zx_rd(priv, PP_UBQ_B1));
	}

	/* Clean 0/0 ring indices.  U-Boot's TFTP drives these exact rings
	 * (its own init prints "dma_init:TX_UP_DESC_DEPTH = 1024"), so a
	 * tftp+bootm boot leaves a non-zero producer/consumer pair behind
	 * (we measured 0xd5c1/0xd5c1 = 54721 = the 512-byte TFTP block
	 * count).  A stock NAND boot leaves them 0.  Vendor pp_dma_init
	 * never touches them; we reset them so our run matches a stock
	 * boot and the 0x1801806c dump reads as (comp<<16)|sub. */
	zx_wr(priv, PP_DMA_RINGA_MISC, 0);
	zx_wr(priv, PP_DMA_RINGB_MISC, 0);

	dev_info(priv->dev,
		 "dma: ctrl0=%#x ringA=%#x ringB=%#x mode=%u/%u cfg=%u burst=%#x idx=%#x/%#x cfgmode=%d\n",
		 zx_rd(priv, PP_DMA_CTRL0),
		 zx_rd(priv, PP_DMA_RINGA_BASE), zx_rd(priv, PP_DMA_RINGB_BASE),
		 zx_rd(priv, PP_DMA_MODE28), zx_rd(priv, PP_DMA_MODE2C),
		 zx_rd(priv, PP_DMA_DESC_CFG), zx_rd(priv, PP_DMA_BURST),
		 zx_rd(priv, PP_DMA_RINGA_MISC), zx_rd(priv, PP_DMA_RINGB_MISC),
		 cfgmode);
}

static void *zx_alloc_coherent(struct zx279131_priv *priv, size_t size,
			       dma_addr_t *dma)
{
	return dma_alloc_attrs(priv->dev, size, dma, GFP_KERNEL,
			       DMA_ATTR_NO_WARN);
}

/* BMU config-register block.  Register-only (no allocations), so it is
 * safe to re-apply at runtime - that is what the `retrig` sysfs node uses
 * for in-shell A/B testing.  Strides are deliberately NOT cfgmode-switched:
 * our TX buffer layout is fixed at 2432 B/bp (vendor pp_data_raw_send),
 * whereas U-Boot leaves 2048, which would only make sense with its own
 * layout. */
static void zx279131_bmu_cfg(struct zx279131_priv *priv)
{
	bmu_wr(priv, PP_BMU_CFG_A, 0x0104c040);
	bmu_wr(priv, PP_BMU_CFG_B, 0x0104c040);
	bmu_wr(priv, PP_BMU_POOLCNT_TX,
	       (ubpool ? 2048u : priv->tx_bp_count) << 16);
	bmu_wr(priv, PP_BMU_POOLCNT_RX,
	       (ubpool ? 512u : (u32)ZX279131_BP_TABLE_ENTRIES) << 16);
	bmu_wr(priv, PP_BMU_CFG_C, cfgmode == 1 ? 0x40 : 0x17f);
	bmu_wr(priv, PP_BMU_CFG_D, cfgmode == 1 ? 0x10 : 0xf);
}

static int zx279131_bmu_init(struct zx279131_priv *priv)
{
	static const u32 tx_slots[] = { 12288, 4096, 2048, 512 };
	static const u32 rx_slots[] = { 512, 256, 128 };
	unsigned int i;

	/* one coherent block: table0 (24 KiB) + table1 (1 KiB) + misc 64 KiB */
	priv->bmu_area = zx_alloc_coherent(priv, 0x30000, &priv->bmu_area_dma);
	if (!priv->bmu_area)
		return -ENOMEM;
	priv->bp_tbl0 = priv->bmu_area;
	priv->bp_tbl0_dma = priv->bmu_area_dma;
	priv->bp_tbl1 = (u16 *)((u8 *)priv->bmu_area + 0x10000);
	priv->bp_tbl1_dma = priv->bmu_area_dma + 0x10000;

	/* RX pool first: grab the smaller block while memory is fresh */
	priv->rx_pool = NULL;
	for (i = 0; i < ARRAY_SIZE(rx_slots); i++) {
		priv->rx_pool = zx_alloc_coherent(priv,
						  (size_t)rx_slots[i] * 9728,
						  &priv->rx_pool_dma);
		if (priv->rx_pool)
			break;
	}

	/* TX pool: as large as the coherent allocator allows */
	priv->tx_pool = NULL;
	for (i = 0; i < ARRAY_SIZE(tx_slots); i++) {
		priv->tx_pool = zx_alloc_coherent(priv,
						  (size_t)tx_slots[i] * 2432,
						  &priv->tx_pool_dma);
		if (priv->tx_pool) {
			priv->tx_bp_count = tx_slots[i];
			break;
		}
	}
	if (!priv->tx_pool)
		return -ENOMEM;

	/* RX descriptor rings: 16 x 64 KiB = 1 MiB */
	priv->rx_rings = zx_alloc_coherent(priv, ZX279131_RX_RINGS_TOTAL,
					   &priv->rx_rings_dma);

	for (i = 0; i < priv->tx_bp_count; i++)
		priv->bp_tbl0[i] = (u16)__swab16(i & 0xffff);
	for (i = 0; i < ZX279131_BP_TABLE_ENTRIES; i++)
		priv->bp_tbl1[i] = (u16)__swab16(i & 0xffff);

	/* pool / ring bases (vendor pp_bmu_init + tm_pp_init) */
	zx_wr(priv, PP_POOL_TBL0, lower_32_bits(priv->bp_tbl0_dma));
	zx_wr(priv, PP_POOL_TBL1, lower_32_bits(priv->bp_tbl1_dma));
	if (priv->rx_rings)
		zx_wr(priv, PP_POOL_RXRING, lower_32_bits(priv->rx_rings_dma));
	zx_wr(priv, PP_POOL_TX, lower_32_bits(priv->tx_pool_dma));
	if (priv->rx_pool)
		zx_wr(priv, PP_POOL_RX, lower_32_bits(priv->rx_pool_dma));
	zx_wr(priv, PP_POOL_STRIDES, 0x26000980);
	zx_wr(priv, PP_POOL_MISC, lower_32_bits(priv->bmu_area_dma + 0x20000));

	/* BMU control block */
	zx279131_bmu_cfg(priv);

	dev_info(priv->dev,
		 "bmu: tbl0=%pad tbl1=%pad rxring=%pad txpool=%pad (%u bp) rxpool=%pad cfg=%#x\n",
		 &priv->bp_tbl0_dma, &priv->bp_tbl1_dma, &priv->rx_rings_dma,
		 &priv->tx_pool_dma, priv->tx_bp_count, &priv->rx_pool_dma,
		 bmu_rd(priv, PP_BMU_CFG_A));
	return 0;
}

static int zx279131_bmu_alloc(struct zx279131_priv *priv, u32 *bp_out)
{
	u32 req = 0, res;
	int i;

	bmu_wr(priv, PP_BMU_ALLOC_REQ, 1);	/* pool 0 (TX) */
	for (i = 0; i < 2000; i++) {
		req = bmu_rd(priv, PP_BMU_ALLOC_REQ);
		if (!(req & 1))
			break;
	}
	if (req & 1)
		return -EBUSY;

	res = bmu_rd(priv, PP_BMU_ALLOC_RESULT);
	if (!(res & 0x80000000))
		return -ENOSPC;
	*bp_out = res & 0xffff;
	return 0;
}

static void zx279131_bmu_free(struct zx279131_priv *priv, u32 pool, u32 bp)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if ((bmu_rd(priv, PP_BMU_FIFO_STATUS) >> 3) & 0x3f)
			break;
		cpu_relax();
	}
	bmu_wr(priv, PP_BMU_FREE, (pool << 16) | (bp & 0xffff));
}

/* ------------------------------------------------------------------ */
/* TX reclaim (hardware done counter, pp_check_tx_done_nolock port)    */
/* ------------------------------------------------------------------ */

static void zx279131_tx_reclaim(struct zx279131_priv *priv)
{
	/* 0x18018068 returns the number of descriptors completed since
	 * the previous read and clears (vendor pp_check_tx_done_nolock
	 * derives its in-flight count from exactly this).  It must be
	 * read ONCE per pass - every extra read steals completions. */
	u32 dreg = (tx_mode == 3) ? PP_DMA_RINGA_DONE
				  : PP_DMA_RINGB_DONE;
	u16 delta = zx_rd(priv, dreg) & 0xffff;

	priv->dbg_done_last = delta;
	if (!delta || delta > ZX279131_TX_RING_SIZE)
		return;

	/* v6.3: do NOT release the BP here.  The BMU recycles a TX BP in
	 * hardware the moment the engine consumes its descriptor.  Proof
	 * from the vendor module: pp_bmu_free_bp() has exactly five call
	 * sites - pp_gso_upload_disable() and the RX path
	 * (pp_net_rx_isra_2) - and NONE on the TX-completion path;
	 * pp_timer_func() only decrements the in-flight counter through
	 * pp_check_tx_done_nolock().  A software free here therefore
	 * pushes duplicate BP indices into the BMU free FIFO, overflows it
	 * and stalls the engine (measured: TX stops dead after exactly 10
	 * frames, on both rings, fresh boot, 40 identical descriptors). */
	priv->tx_completed += delta;
	if (priv->tx_completed > priv->tx_submitted)
		priv->tx_completed = priv->tx_submitted;

	/* the xmit path stops the queue when the ring is full; without a
	 * wake here the only other caller (link_work) runs at poll rate
	 * and TX can stall permanently. */
	if (netif_queue_stopped(priv->ndev) &&
	    (u32)(priv->tx_submitted - priv->tx_completed) <
	    ZX279131_TX_RING_SIZE)
		netif_wake_queue(priv->ndev);
}

/* ------------------------------------------------------------------ */
/* TX self-test (diagnostic, once at probe, before any up/down cycle)   */
/* ------------------------------------------------------------------ */
/* Answers two open questions in one boot:
 *  - does the engine keep completing past the ~10 frames preinit sends?
 *  - do injected frames reach the wire?  (dst ff:ff:ff:ff:ff:ff, ethertype
 *    0x88b5: every host accepts a broadcast, so a plain `tcpdump -i ethX
 *    -e -n ether proto 0x88b5` sees it without promiscuous mode; the
 *    ethertype is not IP, so nothing replies)
 * The descriptor fill below mirrors zx279131_ndo_xmit() exactly on purpose -
 * if you change one, change the other.
 */
#define ZX279131_SELFTEST_FRAMES	40

static void zx279131_tx_selftest(struct zx279131_priv *priv)
{
	struct net_device *ndev = priv->ndev;
	u8 frame[64];
	int i;

	memset(frame, 0, sizeof(frame));
	memset(frame, 0xff, ETH_ALEN);		/* dst = broadcast */
	memcpy(frame + 6, ndev->dev_addr, ETH_ALEN);
	frame[12] = 0x88;			/* ethertype 0x88b5 */
	frame[13] = 0xb5;
	for (i = 14; i < (int)sizeof(frame); i++)
		frame[i] = (u8)i;

	dev_info(priv->dev,
		 "selftest: begin ring=%c idx=%u sub=%u comp=%u done=%#x rA6c=%#x rB6c=%#x\n",
		 tx_mode == 3 ? 'A' : 'B', priv->tx_submit_idx,
		 priv->tx_submitted, priv->tx_completed,
		 zx_rd(priv, priv->done_off),
		 zx_rd(priv, PP_DMA_RINGA_MISC), zx_rd(priv, PP_DMA_RINGB_MISC));

	for (i = 0; i < ZX279131_SELFTEST_FRAMES; i++) {
		struct zx279131_tx_desc *desc;
		void *buf;
		u32 bp, w12;
		int err;

		err = zx279131_bmu_alloc(priv, &bp);
		if (err || bp >= priv->tx_bp_count) {
			dev_warn(priv->dev,
				 "selftest: bp alloc failed at frame %d (%d)\n",
				 i, err);
			break;
		}

		buf = (u8 *)priv->tx_pool + 0x20 +
		      (size_t)bp * ZX279131_TX_BP_STRIDE;
		memcpy(buf, frame, sizeof(frame));

		if (w12base)
			w12 = w12base;
		else if (tx_mode == 1)
			w12 = ZX279131_TX_W12_BASE_SW | ZX279131_TX_W12_BIT14;
		else if (tx_mode == 3)
			w12 = ZX279131_TX_W12_BASE_A | ZX279131_TX_W12_BIT14;
		else
			w12 = ZX279131_TX_W12_BASE_B | ZX279131_TX_W12_BIT14;
		w12 |= ((u32)sizeof(frame) & 0x3fff) << 15;

		desc = &(tx_mode == 3 ? priv->tx_ring_a : priv->tx_ring_b)
			[priv->tx_submit_idx];
		desc->ctrl = cpu_to_le16(ZX279131_TX_DESC_CTRL);
		desc->f2 = 0;
		desc->f4 = cpu_to_le32(tx_mode == 1 ? ZX279131_TX_F4_SW :
				       tx_mode == 2 ? ZX279131_TX_F4_SW2 : 0);
		desc->f8 = 0;
		desc->bp_idx = cpu_to_le16(bp);
		desc->w12 = cpu_to_le32(w12);
		desc->ch = cpu_to_le32(tx_mode == 3 ? 0 : 1);
		desc->f20 = 0;
		desc->attr = cpu_to_le32(ZX279131_TX_ATTR24);
		desc->f28 = 0;

		priv->bp_history[priv->tx_submit_idx] = bp;
		priv->tx_submit_idx = (priv->tx_submit_idx + 1) %
				      ZX279131_TX_RING_SIZE;
		priv->tx_submitted++;

		iowrite32(1, priv->pp + (tx_mode == 3 ? PP_DMA_RINGA_DOOR :
					 PP_DMA_RINGB_DOOR));

		if (i % 4 == 3) {
			msleep(20);
			zx279131_tx_reclaim(priv);
			dev_info(priv->dev,
				 "selftest: %2d sent comp=%u done=%#x rA6c=%#x rB6c=%#x\n",
				 i + 1, priv->tx_completed,
				 zx_rd(priv, priv->done_off),
				 zx_rd(priv, PP_DMA_RINGA_MISC),
				 zx_rd(priv, PP_DMA_RINGB_MISC));
		}
	}

	msleep(60);
	zx279131_tx_reclaim(priv);
	dev_info(priv->dev, "selftest: end sub=%u comp=%u of %d frames\n",
		 priv->tx_submitted, priv->tx_completed,
		 ZX279131_SELFTEST_FRAMES);
}

/* ------------------------------------------------------------------ */
/* GPHY / link                                                         */
/* ------------------------------------------------------------------ */

static const char *zx_speed_name(int code)
{
	switch (code) {
	case 0: return "10M";
	case 1: return "100M";
	case 2: return "1000M";
	default: return "?";
	}
}

static void zx279131_link_scan(struct zx279131_priv *priv, bool initial)
{
	int port;

	for (port = 0; port < ZX279131_NUM_GPHY; port++) {
		u32 phy = ZX279131_GPHY_ADDR0 + port;
		u32 r26;
		bool link;
		u32 nspd;
		int code, duplex;

		r26 = zx_mdio_read(priv, phy, GPHY_REG_STATUS26);
		r26 = zx_mdio_read(priv, phy, GPHY_REG_STATUS26);
		link = !!(r26 & 0x40);

		if (!link) {
			if (priv->phy_state[port] != -1) {
				priv->phy_state[port] = -1;
				dev_info(priv->dev,
					 "gphy%d: link down (r26=%#x)\n",
					 port, r26);
				zx_smac_disable(priv, port);
			}
			continue;
		}

		nspd = r26 & 0x380;
		if (nspd == priv->phy_state[port]) {
			if (!priv->port_acked[port])
				zx_smac_port_up_wait(priv, port, false);
			zx_smac_enable(priv, port);
			continue;
		}

		code = nspd >> 8;
		if (code == 3)
			continue;
		duplex = (nspd >> 7) & 1;

		dev_info(priv->dev,
			 "gphy%d: link up %s %s (r26=%#x)%s\n",
			 port, zx_speed_name(code), duplex ? "full" : "half",
			 r26, initial ? " [boot]" : "");

		zx_smac_speed_duplex(priv, port, code, duplex);
		zx_smac_enable(priv, port);
		priv->phy_state[port] = nspd;
		if (!priv->port_acked[port])
			zx_smac_port_up_wait(priv, port, false);

		if (code == 0)
			udelay(500);
	}
}

static void zx279131_link_work(struct work_struct *work)
{
	struct zx279131_priv *priv =
		container_of(to_delayed_work(work), struct zx279131_priv,
			     link_work);
	u32 before;

	zx279131_link_scan(priv, false);

	before = priv->tx_completed;
	if (priv->cur_mode != tx_mode) {
		priv->cur_mode = tx_mode;
		priv->tx_submit_idx = 0;
		priv->tx_submitted = 0;
		priv->tx_completed = 0;
		priv->dbg_done_stuck = 0xffffffff;
	}

	zx279131_tx_reclaim(priv);
	if (priv->tx_completed != before)
		dev_info(priv->dev,
			 "tx: hw done +%u (submitted=%u completed=%u last_bp=%u)\n",
			 priv->tx_completed - before, priv->tx_submitted,
			 priv->tx_completed,
			 priv->bp_history[(priv->tx_completed - 1) %
					  ZX279131_TX_RING_SIZE]);

	if (priv->tx_submitted != priv->dbg_done_stuck) {
		struct zx279131_tx_desc *ld;

		priv->dbg_done_stuck = priv->tx_submitted;
		ld = &priv->tx_ring[(priv->tx_submit_idx +
				     ZX279131_TX_RING_SIZE - 1) %
				    ZX279131_TX_RING_SIZE];
		dev_info(priv->dev,
			 "tx: sub=%u comp=%u done_delta=%#x txfree=%#x istat=%#x imask=%#x last=%08x %08x %08x %08x\n",
			 priv->tx_submitted, priv->tx_completed,
			 priv->dbg_done_last,
			 zx_rd(priv, PP_DMA_TXFREE),
			 zx_rd(priv, PP_INT_STATUS),
			 zx_rd(priv, PP_INT_MASK),
			 le32_to_cpu(ld->ctrl), le32_to_cpu(ld->f4),
			 le32_to_cpu(ld->w12), le32_to_cpu(ld->ch));
	}

	if (link_poll_ms > 0)
		schedule_delayed_work(&priv->link_work,
				      msecs_to_jiffies(link_poll_ms));
}

/* ------------------------------------------------------------------ */
/* RX consumption (v8.8)                                               */
/* ------------------------------------------------------------------ */
/* Just enough for ping: ring 0 (queue 0) and single-BP frames only.
 *
 * Deliberately NOT done in v8.8 - multi-BP/jumbo reassembly, GRO, the
 * kfifo skb pool, and the PP_RXREL descriptor hand-back.  Descriptors are
 * consumed in order and simply forgotten; the ring holds 4096 of them, so
 * a few thousand frames can arrive before the engine would need them
 * returned.  (RXQ0 is a producer count, so the software index below is a
 * plain monotonic cursor - nothing else moves it.)
 *
 * The BP *is* returned to the BMU: the pool only has tx_bp_count entries
 * and broadcast traffic alone would drain it in a second.  It is returned
 * only after the payload has been copied out, because the engine may hand
 * that same BP to the very next frame the moment it becomes free. */
static int zx279131_rx_poll(struct zx279131_priv *priv)
{
	struct net_device *ndev = priv->ndev;
	const struct zx279131_rx_desc *ring;
	u32 prod, avail, done = 0;
	u8 *pool;

	if (!rxen || !priv->rx_rings || !priv->tx_pool || !priv->port_up)
		return 0;

	ring = priv->rx_rings;			/* ring 0 == queue 0 */
	pool = priv->tx_pool;

	/* PP_RX_QCNT(0) is a free-running producer *count*, not a ring index:
	 * it climbs straight past the ring size (measured 5016 on a 4096-entry
	 * ring, so the engine wraps by itself and never needs PP_RXREL to keep
	 * producing).  The consumer cursor therefore has to be monotonic too
	 * and masked only where it addresses the ring - subtracting a masked
	 * index from the counter silently re-delivers frames once the ring
	 * wraps (measured: 61888 packets delivered for 5016 produced, peer
	 * logging +2356 duplicates). */
	prod = zx_rd(priv, PP_RX_QCNT(0));
	priv->rx_prod_last = prod;

	avail = prod - priv->rx_read_total;
	if (avail > ZX279131_RX_RING_SIZE) {
		/* Fell behind by more than the ring holds: those entries have
		 * already been overwritten, so resync onto the newest
		 * RING_SIZE ones instead of re-reading stale descriptors. */
		priv->rx_read_total = prod - ZX279131_RX_RING_SIZE;
		priv->dbg_rx_resync++;
		avail = ZX279131_RX_RING_SIZE;
	}
	if (avail > rxbudget)
		avail = rxbudget;

	while (done < avail) {
		u32 idx = priv->rx_read_total &
			  (ZX279131_RX_RING_SIZE - 1);
		const struct zx279131_rx_desc *d = &ring[idx];
		u32 len = ZX279131_RX_DESC_LEN(d);
		u32 bp = ZX279131_RX_DESC_BP(d);
		u32 port = ZX279131_RX_DESC_PORT(d);
		struct sk_buff *skb;
		u8 *src;

		priv->rx_read_total++;
		done++;

		if (ZX279131_RX_DESC_MULTI(d)) {
			priv->dbg_rx_multi++;
			continue;
		}
		if (port < ARRAY_SIZE(priv->dbg_rx_port))
			priv->dbg_rx_port[port]++;

		if (!(rxport & (1u << (port & 31))) || len < ETH_HLEN ||
		    len > rxstride - rxoff || bp >= priv->tx_bp_count) {
			priv->dbg_rx_bad++;
			if (rxfreen)
				zx279131_bmu_free(priv, 0, bp);
			continue;
		}

		src = pool + rxoff + (size_t)bp * rxstride;
		skb = netdev_alloc_skb(ndev, len + NET_IP_ALIGN);
		if (!skb) {
			priv->dbg_rx_skb_fail++;
			ndev->stats.rx_dropped++;
			if (rxfreen)
				zx279131_bmu_free(priv, 0, bp);
			continue;
		}
		skb_reserve(skb, NET_IP_ALIGN);
		skb_put_data(skb, src, len);

		if (rxfreen)
			zx279131_bmu_free(priv, 0, bp);

		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		priv->dbg_rx_pkts++;
		priv->dbg_rx_bytes += len;
		netif_rx(skb);
	}

	return done;
}

static void zx279131_rx_work(struct work_struct *work)
{
	struct zx279131_priv *priv =
		container_of(to_delayed_work(work), struct zx279131_priv,
			     rx_work);
	int got = zx279131_rx_poll(priv);

	if (got && priv->dbg_rx_pkts <= 4)
		dev_info(priv->dev,
			 "rx: +%d pkt=%u bad=%u multi=%u resync=%u prod=%u idx=%u\n",
			 got, priv->dbg_rx_pkts, priv->dbg_rx_bad,
			 priv->dbg_rx_multi, priv->dbg_rx_resync,
			 priv->rx_prod_last,
			 priv->rx_read_total & (ZX279131_RX_RING_SIZE - 1));

	if (rxpoll_ms > 0 && priv->port_up)
		schedule_delayed_work(&priv->rx_work,
				      msecs_to_jiffies(rxpoll_ms));
}

/* ------------------------------------------------------------------ */
/* PP TX-done interrupt (vendor zx_pp_int / pp_net_int, PORT-NOTES 28.4) */
/* ------------------------------------------------------------------ */

static void zx279131_pp_work(struct work_struct *work)
{
	struct zx279131_priv *priv =
		container_of(to_delayed_work(work), struct zx279131_priv,
			     pp_work);
	u32 before = priv->tx_completed;

	zx279131_tx_reclaim(priv);

	/* v8.8: the same wakeup that drains TX also drains what RX has
	 * queued up - it is free and it shortens the RX latency whenever
	 * the box is also transmitting. */
	zx279131_rx_poll(priv);

	if (priv->tx_completed == before)
		priv->irq_stale++;
	else
		priv->irq_stale = 0;

	/* Safety net.  PP_INT_STATUS bit1 is asserted on this part from the
	 * very first open (v5.1 logs: istat=0x2 steady); re-enabling blindly
	 * is what livelocked the box in the first v6 attempt.  Each round
	 * here is at most one IRQ plus one workqueue run, so it cannot storm
	 * the CPU - but if it never makes progress we simply stop re-arming
	 * and fall back to the old polled behaviour. */
	if (priv->irq_stale > 4) {
		if (!priv->irq_gave_up) {
			priv->irq_gave_up = true;
			dev_warn(priv->dev,
				 "pp irq: %u wakeups without TX progress, falling back to polling\n",
				 priv->irq_stale);
		}
		return;			/* leave the sources masked */
	}

	/* vendor pp_net_int_enable: clear mask bits[1:0] (TX-done A/B) */
	zx_wr(priv, PP_INT_MASK, zx_rd(priv, PP_INT_MASK) & ~0x3u);
}

static irqreturn_t zx279131_pp_irq(int irq, void *dev_id)
{
	struct zx279131_priv *priv = dev_id;
	u32 st = zx_rd(priv, PP_INT_STATUS);
	u32 mk = zx_rd(priv, PP_INT_MASK);
	u32 pending = (st & 0x3u) & ~mk;

	/* vendor zx_pp_int: mask unconditionally (this is what drops the
	 * level-triggered line), handle only if pending, and always report
	 * HANDLED. */
	zx_wr(priv, PP_INT_MASK, mk | 0x7u);

	if (!pending)
		return IRQ_HANDLED;

	priv->irq_count++;
	schedule_delayed_work(&priv->pp_work, 0);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* net_device_ops                                                      */
/* ------------------------------------------------------------------ */

static int zx279131_ndo_open(struct net_device *ndev)
{
	struct zx279131_priv *priv = netdev_priv(ndev);

	priv->port_up = true;
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	schedule_delayed_work(&priv->link_work, 0);

	/* v8.8: start draining RX */
	if (rxen && rxpoll_ms > 0)
		schedule_delayed_work(&priv->rx_work,
				      msecs_to_jiffies(rxpoll_ms));

	/* vendor pp_net_int_enable: clear mask bits[1:0] (TX-done A/B) */
	if (priv->pp_irq >= 0)
		zx_wr(priv, PP_INT_MASK, zx_rd(priv, PP_INT_MASK) & ~0x3u);

	dev_info(priv->dev, "%s: open (pp_irq=%d, rxen=%d)\n", ndev->name,
		 priv->pp_irq, rxen);
	return 0;
}

static int zx279131_ndo_stop(struct net_device *ndev)
{
	struct zx279131_priv *priv = netdev_priv(ndev);

	cancel_delayed_work_sync(&priv->link_work);
	cancel_delayed_work_sync(&priv->pp_work);
	cancel_delayed_work_sync(&priv->rx_work);
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	priv->port_up = false;

	/* vendor pp_net_stop -> pp_net_int_disable */
	zx_wr(priv, PP_INT_MASK, zx_rd(priv, PP_INT_MASK) | 0x7u);

	dev_info(priv->dev, "%s: stop\n", ndev->name);
	return 0;
}

static netdev_tx_t zx279131_ndo_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct zx279131_priv *priv = netdev_priv(ndev);
	struct zx279131_tx_desc *desc;
	void *buf;
	u32 bp, w12, desc_len;
	int err;

	if (!priv->port_up || skb->len > 0x3fff) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	zx279131_tx_reclaim(priv);
	if ((u32)(priv->tx_submitted - priv->tx_completed) >=
	    ZX279131_TX_RING_SIZE) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	err = zx279131_bmu_alloc(priv, &bp);
	if (err || bp >= priv->tx_bp_count) {
		if (!err)
			zx279131_bmu_free(priv, 0, bp);
		priv->dbg_bp_alloc_fail++;
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	/* data starts `bufoff` bytes into the pool slot (0x20 default,
	 * U-Boot's own TX path uses 0x10 - see the bufoff parameter) */
	buf = (u8 *)priv->tx_pool + bufoff + (size_t)bp * ZX279131_TX_BP_STRIDE;
	memcpy(buf, skb->data, skb->len);

	/* ring B pads short frames to 33 bytes (vendor pp_data_raw_send) */
	desc_len = skb->len;
	if (tx_mode != 3 && desc_len <= 32) {
		memset((u8 *)buf + desc_len, 0, 33 - desc_len);
		desc_len = 33;
	}

	if (tx_mode == 1)
		w12 = ZX279131_TX_W12_BASE_SW;	/* tagged sw desc */
	else if (tx_mode == 3)
		w12 = ZX279131_TX_W12_BASE_A;	/* type-2 (ring A) */
	else
		w12 = ZX279131_TX_W12_BASE_B;	/* untagged sw desc */
	w12 |= ZX279131_TX_W12_BIT14;
	w12 |= ((u32)desc_len & 0x3fff) << 15;

	desc = &(tx_mode == 3 ? priv->tx_ring_a : priv->tx_ring_b)
		[priv->tx_submit_idx];
	desc->ctrl = cpu_to_le16(ZX279131_TX_DESC_CTRL);
	desc->f2 = 0;
	desc->f4 = cpu_to_le32(tx_mode == 1 ? ZX279131_TX_F4_SW :
			       tx_mode == 2 ? ZX279131_TX_F4_SW2 : 0);
	desc->f8 = 0;
	desc->bp_idx = cpu_to_le16(bp);
	desc->w12 = cpu_to_le32(w12);
	desc->ch = cpu_to_le32(tx_mode == 3 ? 0 : 1);
	desc->f20 = 0;
	desc->attr = cpu_to_le32(ZX279131_TX_ATTR24);
	desc->f28 = 0;

	priv->bp_history[priv->tx_submit_idx] = bp;
	priv->tx_submit_idx = (priv->tx_submit_idx + 1) %
			      ZX279131_TX_RING_SIZE;
	priv->tx_submitted++;
	priv->dbg_tx_pkts++;
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_consume_skb_any(skb);

	/* ring the doorbell with the descriptor count */
	iowrite32(1, priv->pp + (tx_mode == 3 ? PP_DMA_RINGA_DOOR
					      : PP_DMA_RINGB_DOOR));
	return NETDEV_TX_OK;
}

static void zx279131_ndo_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct zx279131_priv *priv = netdev_priv(ndev);

	zx279131_tx_reclaim(priv);
	netif_wake_queue(ndev);
	dev_warn(priv->dev, "%s: tx timeout (submitted=%u done=%u)\n",
		 ndev->name, priv->tx_submitted, priv->tx_completed);
}

static void zx279131_ndo_get_stats(struct net_device *ndev,
				   struct rtnl_link_stats64 *stats)
{
	stats->tx_packets = ndev->stats.tx_packets;
	stats->tx_bytes = ndev->stats.tx_bytes;
}

static const struct net_device_ops zx279131_netdev_ops = {
	.ndo_open		= zx279131_ndo_open,
	.ndo_stop		= zx279131_ndo_stop,
	.ndo_start_xmit		= zx279131_ndo_xmit,
	.ndo_tx_timeout		= zx279131_ndo_timeout,
	.ndo_get_stats64	= zx279131_ndo_get_stats,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                      */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* sysfs register dump (works without /dev/mem)                        */
/* ------------------------------------------------------------------ */

static ssize_t zx_regs_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	int n = 0, q;

	if (!priv || !priv->pp)
		return -ENODEV;

#define ZXREG(name, val) \
	n += scnprintf(buf + n, PAGE_SIZE - n, "%-14s %#010x\n", name, (u32)(val))

	ZXREG("PP.DMA_CTRL0", zx_rd(priv, PP_DMA_CTRL0));
	ZXREG("PP.DMA_M04", zx_rd(priv, PP_DMA_MAGIC04));
	ZXREG("PP.DMA_QW0", zx_rd(priv, PP_DMA_QW0));
	ZXREG("PP.DMA_QW1", zx_rd(priv, PP_DMA_QW1));
	ZXREG("PP.DMA_M28", zx_rd(priv, PP_DMA_MODE28));
	ZXREG("PP.DMA_M2C", zx_rd(priv, PP_DMA_MODE2C));
	ZXREG("PP.DMA_TO", zx_rd(priv, PP_DMA_TIMEOUT));
	ZXREG("PP.DMA_CFG", zx_rd(priv, PP_DMA_DESC_CFG));
	ZXREG("PP.DMA_BURST", zx_rd(priv, PP_DMA_BURST));
	ZXREG("PP.DMA_M40", zx_rd(priv, PP_DMA_M40));
	ZXREG("PP.DMA_M44", zx_rd(priv, PP_DMA_M44));
	ZXREG("PP.DMA_M48", zx_rd(priv, PP_DMA_M48));
	ZXREG("PP.DMA_TXFREE", zx_rd(priv, PP_DMA_TXFREE));
	ZXREG("PP.INT_ST", zx_rd(priv, PP_INT_STATUS));
	ZXREG("PP.INT_MASK", zx_rd(priv, PP_INT_MASK));
	ZXREG("PP.RINGA", zx_rd(priv, PP_DMA_RINGA_BASE));
	ZXREG("PP.DONEA", zx_rd(priv, PP_DMA_RINGA_DONE));
	ZXREG("PP.RINGB", zx_rd(priv, PP_DMA_RINGB_BASE));
	ZXREG("PP.DONEB", zx_rd(priv, PP_DMA_RINGB_DONE));
	ZXREG("PP.DMA_M38", zx_rd(priv, PP_DMA_M38));
	ZXREG("PP.RINGA_6C", zx_rd(priv, PP_DMA_RINGA_MISC));
	ZXREG("PP.RINGB_6C", zx_rd(priv, PP_DMA_RINGB_MISC));
	for (q = 0; q < ZX279131_NUM_RX_QUEUES; q++)
		ZXREG("PP.RXQ", zx_rd(priv, PP_RX_QCNT(q)));
	ZXREG("BMU.CTRL", bmu_rd(priv, PP_BMU_CTRL));
	ZXREG("BMU.CFGA", bmu_rd(priv, PP_BMU_CFG_A));
	ZXREG("BMU.CFGB", bmu_rd(priv, PP_BMU_CFG_B));
	ZXREG("BMU.RESULT", bmu_rd(priv, PP_BMU_ALLOC_RESULT));
	ZXREG("BMU.REQ", bmu_rd(priv, PP_BMU_ALLOC_REQ));
	ZXREG("BMU.CNT_TX", bmu_rd(priv, PP_BMU_POOLCNT_TX));
	ZXREG("BMU.CNT_RX", bmu_rd(priv, PP_BMU_POOLCNT_RX));
	ZXREG("BMU.STAT", bmu_rd(priv, PP_BMU_FIFO_STATUS));
	ZXREG("PP.TBL0", zx_rd(priv, PP_POOL_TBL0));
	ZXREG("PP.TBL1", zx_rd(priv, PP_POOL_TBL1));
	ZXREG("PP.RXRING", zx_rd(priv, PP_POOL_RXRING));
	ZXREG("PP.TXPOOL", zx_rd(priv, PP_POOL_TX));
	ZXREG("PP.RXPOOL", zx_rd(priv, PP_POOL_RX));
	ZXREG("PP.STRIDES", zx_rd(priv, PP_POOL_STRIDES));
	ZXREG("PP.MISC", zx_rd(priv, PP_POOL_MISC));
	ZXREG("SMAC.P0", sw_rd(priv, SW_SMAC_BLOCK(0)));
	ZXREG("SMAC.P1", sw_rd(priv, SW_SMAC_BLOCK(1)));
	ZXREG("SMAC.P2", sw_rd(priv, SW_SMAC_BLOCK(2)));
	ZXREG("SMAC.P3", sw_rd(priv, SW_SMAC_BLOCK(3)));
	ZXREG("SMAC.MODE", sw_rd(priv, SW_SMAC_MODE(0)));
	ZXREG("SMAC.SOPC", sw_rd(priv, SW_SOPC_HALFMODE));
	ZXREG("SMAC.RESET", sw_rd(priv, SW_SMAC_RESET_REG));
	for (q = 0; q < ZX279131_NUM_GPHY; q++)
		ZXREG("SMAC.UP", sw_rd(priv, SW_SMAC_PORTUP(q)));
	ZXREG("SW.04", sw_rd(priv, SW_CTRL04));
	ZXREG("SW.48", sw_rd(priv, SW_CTRL48));
	ZXREG("SW.BRG04", sw_rd(priv, SW_BRG_BASE + 4));
	ZXREG("SW.SQMG14", sw_rd(priv, SW_SQMG_CMD));
	ZXREG("SW.SIPC", sw_rd(priv, SW_SIPC));
	/* v6.4: U-Boot-vs-us comparison block (read-only telemetry).
	 * U-Boot is the only known-good PP/switch reference on this silicon
	 * (its TFTP pushes 54721 frames through the very same PP rings);
	 * each entry below is a register U-Boot programs with a value this
	 * driver does not write, or does not touch at all. */
	ZXREG("SW.0000", sw_rd(priv, 0x0000));
	ZXREG("SW.0004", sw_rd(priv, 0x0004));
	ZXREG("SW.0008", sw_rd(priv, 0x0008));
	ZXREG("SW.000C", sw_rd(priv, 0x000c));
	ZXREG("SW.01E8", sw_rd(priv, 0x01e8));
	for (q = 0; q < ZX279131_NUM_GPHY; q++)
		ZXREG("SW.PMODE", sw_rd(priv, SW_SMAC_MODE(q)));
	ZXREG("SPA.0000", sw_rd(priv, 0x14000));
	ZXREG("SPA.0004", sw_rd(priv, 0x14004));
	ZXREG("SPA.0008", sw_rd(priv, 0x14008));
	for (q = 0; q < ZX279131_NUM_GPHY; q++)
		ZXREG("SPA.ALLOC", sw_rd(priv, 0x140d0 + q * 4));
	ZXREG("BRG.0340", sw_rd(priv, SW_BRG_BASE + 0x340));
	ZXREG("BRG.0344", sw_rd(priv, SW_BRG_BASE + 0x344));
	ZXREG("BRG.0380", sw_rd(priv, SW_BRG_BASE + 0x380));
	ZXREG("BRG.0188", sw_rd(priv, SW_BRG_BASE + 0x188));
	ZXREG("BRG.0304", sw_rd(priv, SW_BRG_BASE + 0x304));
	ZXREG("BRG.03DC", sw_rd(priv, SW_BRG_BASE + 0x3dc));
	ZXREG("BRG.03E0", sw_rd(priv, SW_BRG_BASE + 0x3e0));
	ZXREG("BRG.001C", sw_rd(priv, SW_BRG_BASE + 0x01c));
	ZXREG("CLA.0048", zx_rd(priv, PP_CLA_0));
	ZXREG("CLA.004C", zx_rd(priv, PP_CLA_1));
	ZXREG("CLA.008C", zx_rd(priv, PP_CLA_0 + 0x44));
	ZXREG("BMU.F58", bmu_rd(priv, 0x58));
	ZXREG("BMU.F5C", bmu_rd(priv, 0x5c));
	ZXREG("BMU.FE8", bmu_rd(priv, 0xe8));
	ZXREG("BMU.FEC", bmu_rd(priv, 0xec));
	ZXREG("BMU.FF4", bmu_rd(priv, 0xf4));
	ZXREG("BMU.FF8", bmu_rd(priv, 0xf8));
	ZXREG("BMU.FFC", bmu_rd(priv, 0xfc));
	ZXREG("IDM0", priv->idm ? readl(priv->idm) : 0);
	ZXREG("MDIO.00", readl(priv->mdio));
	ZXREG("MDIO.10", readl(priv->mdio + ZX_MDIO_STATUS));
	ZXREG("MDIO.14", readl(priv->mdio + ZX_MDIO_CTRL));
	n += scnprintf(buf + n, PAGE_SIZE - n, "COUNTS sub=%u comp=%u drop_bp=%u\n",
		       priv->tx_submitted, priv->tx_completed,
		       priv->dbg_bp_alloc_fail);
#undef ZXREG
	return n;
}
/* v6.9: raw full-window dump, one register per line, so it can be diffed
 * line-by-line against the stock-firmware devmem2 probes we already hold
 * (0x18000000..0x180000fc and 0x1a000000..0x1a0000fc were both probed on
 * the vendor firmware).  Read-only, no side effects. */
static ssize_t zx_raw_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	int n = 0, off;

	if (!priv || !priv->pp)
		return -ENODEV;

	for (off = 0; off <= 0xfc; off += 4)
		n += scnprintf(buf + n, PAGE_SIZE - n, "PP%03x %08x\n",
			       off, zx_rd(priv, off));
	for (off = 0; off <= 0xfc; off += 4)
		n += scnprintf(buf + n, PAGE_SIZE - n, "SW%03x %08x\n",
			       off, sw_rd(priv, off));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SW200 %08x\n", sw_rd(priv, 0x200));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SW230 %08x\n", sw_rd(priv, 0x230));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SW400 %08x\n", sw_rd(priv, 0x400));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SW1000 %08x\n", sw_rd(priv, 0x1000));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SW10000 %08x\n",
		       sw_rd(priv, 0x10000));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SW8000 %08x\n", sw_rd(priv, 0x8000));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SPA000 %08x\n", sw_rd(priv, 0x14000));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SPA004 %08x\n", sw_rd(priv, 0x14004));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SPA008 %08x\n", sw_rd(priv, 0x14008));
	n += scnprintf(buf + n, PAGE_SIZE - n, "SPA0D0 %08x\n", sw_rd(priv, 0x140d0));
	n += scnprintf(buf + n, PAGE_SIZE - n, "IDM000 %08x\n",
		       priv->idm ? readl(priv->idm) : 0);
	n += scnprintf(buf + n, PAGE_SIZE - n, "DONEA %08x DONEB %08x\n",
		       zx_rd(priv, PP_DMA_RINGA_DONE),
		       zx_rd(priv, PP_DMA_RINGB_DONE));
	return n;
}
static DEVICE_ATTR_RO(zx_raw);

/* v7.0: descriptor-level view.  The engine stops after exactly 10 frames
 * (v6.3..v6.9, both rings); dumping the ring shows how far the producer
 * got and whether the hardware touched anything in the descriptors. */
static ssize_t zx_desc_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	const u32 *p;
	const struct zx279131_rx_desc *rd;
	int n = 0, i, q;
	struct zx279131_tx_desc *ring =
		tx_mode == 3 ? priv->tx_ring_a : priv->tx_ring_b;

	if (!priv || !priv->pp || !ring)
		return -ENODEV;

	for (i = 0; i < 16; i++) {
		p = (const u32 *)&ring[i];
		n += scnprintf(buf + n, PAGE_SIZE - n,
			       "TX%02d %08x %08x %08x %08x %08x %08x %08x %08x\n",
			       i, le32_to_cpu(p[0]), le32_to_cpu(p[1]),
			       le32_to_cpu(p[2]), le32_to_cpu(p[3]),
			       le32_to_cpu(p[4]), le32_to_cpu(p[5]),
			       le32_to_cpu(p[6]), le32_to_cpu(p[7]));
	}
	if (priv->rx_rings) {
		rd = priv->rx_rings;
		for (i = 0; i < 8; i++) {
			p = (const u32 *)&rd[i];
			n += scnprintf(buf + n, PAGE_SIZE - n,
				       "RX%02d %08x %08x %08x %08x bp=%u len=%u port=%u%s\n",
				       i, le32_to_cpu(p[0]), le32_to_cpu(p[1]),
				       le32_to_cpu(p[2]), le32_to_cpu(p[3]),
				       ZX279131_RX_DESC_BP(&rd[i]),
				       ZX279131_RX_DESC_LEN(&rd[i]),
				       ZX279131_RX_DESC_PORT(&rd[i]),
				       ZX279131_RX_DESC_MULTI(&rd[i]) ?
				       " multi" : "");
		}
	}
	for (q = 0; q < 8; q++)
		n += scnprintf(buf + n, PAGE_SIZE - n, "RXQ%d %08x\n", q,
			       zx_rd(priv, PP_RX_QCNT(q)));
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "sub=%u comp=%u submit_idx=%u\n",
		       priv->tx_submitted, priv->tx_completed,
		       priv->tx_submit_idx);
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "rx pkt=%u bytes=%u bad=%u multi=%u skbfail=%u resync=%u idx=%u prod=%u\n",
		       priv->dbg_rx_pkts, priv->dbg_rx_bytes,
		       priv->dbg_rx_bad, priv->dbg_rx_multi,
		       priv->dbg_rx_skb_fail, priv->dbg_rx_resync,
		       priv->rx_read_total & (ZX279131_RX_RING_SIZE - 1),
		       priv->rx_prod_last);
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "rx port0..7 %u %u %u %u %u %u %u %u\n",
		       priv->dbg_rx_port[0], priv->dbg_rx_port[1],
		       priv->dbg_rx_port[2], priv->dbg_rx_port[3],
		       priv->dbg_rx_port[4], priv->dbg_rx_port[5],
		       priv->dbg_rx_port[6], priv->dbg_rx_port[7]);
	return n;
}
static DEVICE_ATTR_RO(zx_desc);

/* v7.0: apply the stock firmware's PP-top values.  Only reachable by
 * writing this file, never at probe time - PP+0 may well be a clock/reset
 * control, so it must be opt-in and reversible by a reboot (we boot over
 * TFTP, nothing is written to NAND). */
static ssize_t ppq_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	u32 b0 = zx_rd(priv, 0x000), b8 = zx_rd(priv, 0x008), b10 = zx_rd(priv, 0x010);
	u32 b40 = zx_rd(priv, 0x040);
	int i;

	dev_info(priv->dev,
		 "ppq: before 000=%#x 008=%#x 010=%#x 040=%#x\n",
		 b0, b8, b10, b40);
	zx_wr(priv, 0x000, 0x7e00140);
	zx_wr(priv, 0x008, 0x9cf8);
	for (i = 0; i < 7; i++)
		zx_wr(priv, 0x010 + i * 8, 0xd8e8);
	dev_info(priv->dev,
		 "ppq: after  000=%#x 008=%#x 010=%#x 040=%#x\n",
		 zx_rd(priv, 0x000), zx_rd(priv, 0x008),
		 zx_rd(priv, 0x010), zx_rd(priv, 0x040));
	zx279131_tx_selftest(priv);
	return count;
}
static DEVICE_ATTR_WO(ppq);

static DEVICE_ATTR_RO(zx_regs);

/* ------------------------------------------------------------------ */
/* v7.3: the switch windows this driver has never compared             */
/*                                                                     */
/* Every software register we do program matches the stock firmware.   */
/* What was never checked is the per-port SMAC block - stock carries   */
/* link-status words at 0xB0/0xD0..0xE4 that we never write (0xE0 even */
/* differs: stock 0x11000, we write 0x11200) - and the CPU port's      */
/* (port 6) up/ack handshake at 0x282a4/0x282b8, which the vendor's    */
/* smac_init() handles explicitly for port 6 and this driver does not. */
/* If the switch judges the egress port "not ready" from those, frames */
/* queue up, never drain, and the PP stops after a fixed number of     */
/* descriptors - exactly the symptom we are chasing.                   */
/* 8 words per line keeps each file far below one page.                */
/* ------------------------------------------------------------------ */

static int zx_dump_words(struct zx279131_priv *priv, char *buf, int n,
			 const char *tag, bool is_sw, u32 off)
{
	int i;

	if (n > PAGE_SIZE - 200)
		return n;
	n += scnprintf(buf + n, PAGE_SIZE - n, "%s %05x:", tag, off);
	for (i = 0; i < 8; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, " %08x",
			       is_sw ? sw_rd(priv, off + i * 4) :
				       zx_rd(priv, off + i * 4));
	return n + scnprintf(buf + n, PAGE_SIZE - n, "\n");
}

#define ZX_DUMP(priv, n, tag, is_sw, off) \
	((n) = zx_dump_words((priv), (buf), (n), (tag), (is_sw), (off)))

static ssize_t zx_raw2_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	u32 off;
	int n = 0;

	if (!priv || !priv->pp)
		return -ENODEV;

	/* PP DMA block (incl. ring door/done/index) and BMU block */
	ZX_DUMP(priv, n, "DMA", false, 0x18000);
	ZX_DUMP(priv, n, "DMA", false, 0x18020);
	ZX_DUMP(priv, n, "DMA", false, 0x18040);
	ZX_DUMP(priv, n, "DMA", false, 0x18060);
	ZX_DUMP(priv, n, "BMU", false, 0x10000);
	ZX_DUMP(priv, n, "BMU", false, 0x10040);
	ZX_DUMP(priv, n, "BMU", false, 0x100dc);
	/* switch 0x200..0x2fc (driver only knows 0x230) */
	for (off = 0x200; off <= 0x2e0; off += 0x20)
		ZX_DUMP(priv, n, "SW", true, off);
	ZX_DUMP(priv, n, "SPA", true, 0x14000);
	ZX_DUMP(priv, n, "SPA", true, 0x140d0);
	ZX_DUMP(priv, n, "BRG", true, 0x1c000);
	ZX_DUMP(priv, n, "BRG", true, 0x1c180);
	ZX_DUMP(priv, n, "BRG", true, 0x1c300);
	ZX_DUMP(priv, n, "BRG", true, 0x1c340);
	ZX_DUMP(priv, n, "BRG", true, 0x1c3d0);
	ZX_DUMP(priv, n, "BRG", true, 0x1c640);
	ZX_DUMP(priv, n, "BRG", true, 0x1c778);
	ZX_DUMP(priv, n, "BRG", true, 0x1c7b8);
	ZX_DUMP(priv, n, "SQMG", true, 0x20000);
	ZX_DUMP(priv, n, "SQMG", true, 0x20040);
	ZX_DUMP(priv, n, "SIPC", true, 0xc000);
	/* port up/ack handshake words (index 0xa5..0xb1), half-mode, reset */
	ZX_DUMP(priv, n, "PORT", true, 0x28290);
	ZX_DUMP(priv, n, "PORT", true, 0x282b0);
	ZX_DUMP(priv, n, "HALF", true, 0x283f0);
	ZX_DUMP(priv, n, "SRST", true, 0x2c0004);
	ZX_DUMP(priv, n, "IDM", true, 0x8000);
	ZX_DUMP(priv, n, "IDM", true, 0x8040);
	return n;
}
static DEVICE_ATTR_RO(zx_raw2);

static ssize_t zx_raw3_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	static const u8 ports[] = { 0, 1, 2, 3, 6 };
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	char tag[16];
	unsigned int i;
	int n = 0;

	if (!priv || !priv->sw)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		u32 base = SW_SMAC_BLOCK(ports[i]);

		snprintf(tag, sizeof(tag), "SMB%d", ports[i]);
		ZX_DUMP(priv, n, tag, true, base);
		ZX_DUMP(priv, n, tag, true, base + 0xb0);
		ZX_DUMP(priv, n, tag, true, base + 0xd0);
		ZX_DUMP(priv, n, tag, true, base + 0xd00);
		ZX_DUMP(priv, n, tag, true, base + 0xd20);
	}
	return n;
}
static DEVICE_ATTR_RO(zx_raw3);

/* v7.3: the vendor's smac_init() waits for the up-handshake of every port it
 * configures and then acknowledges it - including port 6, the CPU port our
 * injected frames leave through (its words are 0x282a4 / 0x282b8, i.e. the
 * same pair the code uses for "port 6").  This driver only ever runs the
 * handshake for ports 0..3, so the CPU port may still be waiting.  Stock
 * shows UP4=0/ACK4=1 (completed) with UP5..UP7 pending, i.e. stock does
 * finish it.  Opt-in per write so the state is never changed silently. */
static ssize_t cpuport_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	u32 up0 = sw_rd(priv, 0x282a4), ack0 = sw_rd(priv, 0x282b8);
	int i;

	for (i = 0; i < 400; i++) {
		if (sw_rd(priv, 0x282a4) & 1)
			break;
		udelay(500);
	}
	sw_wr(priv, 0x282b8, 1);
	dev_info(priv->dev,
		 "cpuport: UP4 %#x->%#x (bit0 after %d polls) ACK4 %#x->%#x\n",
		 up0, sw_rd(priv, 0x282a4), i, ack0, sw_rd(priv, 0x282b8));
	zx279131_tx_selftest(priv);
	return count;
}
static DEVICE_ATTR_WO(cpuport);

/* v7.3: enable the CPU port's SMAC block the way switch_smac_init() enables
 * the GPHY ones (block |= 2).  Kept separate from cpuport so the log can
 * tell which of the two made a difference. */
static ssize_t cpuen_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	u32 off = SW_SMAC_BLOCK(6);
	u32 old = sw_rd(priv, off);

	sw_wr(priv, off, old | 2);
	dev_info(priv->dev,
		 "cpuen: SMAC6 MAIN %#x->%#x (misc8 %#x d00 %#x e0 %#x)\n",
		 old, sw_rd(priv, off), sw_rd(priv, off + 8),
		 sw_rd(priv, off + 0xd00), sw_rd(priv, off + 0xe0));
	zx279131_tx_selftest(priv);
	return count;
}
static DEVICE_ATTR_WO(cpuen);

/* v7.3: stock's SMAC link-status words for port 0 (the port the host sits
 * on): 0xB0=0x320 0xD0=0x20c 0xD4=0x3e80000 (=1000<<12) 0xDC=0x63
 * 0xE0=0x11000 0xE4=0x8.  We write 0xE0=0x11200 at init and never touch the
 * others; if the switch derives "port ready" from these, egress never drains
 * and the PP stalls.  Opt-in, values printed before and after. */
static ssize_t smacstat_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	static const struct { u32 off, val; } w[] = {
		{ 0xb0, 0x320 }, { 0xd0, 0x20c }, { 0xd4, 0x3e80000 },
		{ 0xdc, 0x63 }, { 0xe0, 0x11000 }, { 0xe4, 0x8 },
	};
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	u32 base = SW_SMAC_BLOCK(0);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(w); i++)
		dev_info(priv->dev, "smacstat: %#x %#x -> %#x\n",
			 base + w[i].off, sw_rd(priv, base + w[i].off),
			 w[i].val);
	for (i = 0; i < ARRAY_SIZE(w); i++)
		sw_wr(priv, base + w[i].off, w[i].val);
	for (i = 0; i < ARRAY_SIZE(w); i++)
		dev_info(priv->dev, "smacstat: %#x now %#x\n",
			 base + w[i].off, sw_rd(priv, base + w[i].off));
	zx279131_tx_selftest(priv);
	return count;
}
static DEVICE_ATTR_WO(smacstat);

/* ------------------------------------------------------------------ */
/* on-demand TX trigger                                                */
/*                                                                     */
/* The probe-time self-test fires ~1 s after power-on, long before the  */
/* operator can start a capture, so every "did the frames reach the     */
/* wire?" test so far was inconclusive.  This lets the frames be sent   */
/* on demand from the shell:                                           */
/*                                                                     */
/*   echo 40 > /sys/devices/platform/18000000.pp/tx_test               */
/*                                                                     */
/* Run tcpdump first, then write the file - the capture then provably   */
/* covers the frames.  Frames are broadcast ethertype 0x88b5 (non-IP,   */
/* so nothing on the LAN answers).                                      */
/* ------------------------------------------------------------------ */

static void zx279131_tx_inject(struct zx279131_priv *priv, unsigned int n)
{
	struct net_device *ndev = priv->ndev;
	unsigned int i, before = priv->tx_completed;
	u8 frame[64];

	memset(frame, 0, sizeof(frame));
	memset(frame, 0xff, ETH_ALEN);		/* dst = broadcast */
	memcpy(frame + 6, ndev->dev_addr, ETH_ALEN);
	frame[12] = 0x88;			/* ethertype 0x88b5 */
	frame[13] = 0xb5;

	for (i = 0; i < n; i++) {
		struct sk_buff *skb = alloc_skb(sizeof(frame) + NET_IP_ALIGN,
						GFP_KERNEL);

		if (!skb)
			break;
		skb->dev = ndev;
		skb_reserve(skb, NET_IP_ALIGN);
		memcpy(skb_put(skb, sizeof(frame)), frame, sizeof(frame));
		skb->protocol = htons(0x88b5);
		dev_queue_xmit(skb);
	}

	msleep(30);
	zx279131_tx_reclaim(priv);
	dev_info(priv->dev, "tx_test: %u injected, hw comp %u -> %u\n",
		 i, before, priv->tx_completed);
}

static ssize_t tx_test_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);
	struct net_device *ndev = priv->ndev;
	int val;

	if (kstrtoint(buf, 0, &val) || val < 0)
		return -EINVAL;
	if (!netif_running(ndev))
		return -ENETDOWN;

	zx279131_tx_inject(priv, min_t(int, val, 256));
	return count;
}

static ssize_t tx_test_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "sub=%u comp=%u inflight=%u\n",
			 priv->tx_submitted, priv->tx_completed,
			 priv->tx_submitted - priv->tx_completed);
}

static DEVICE_ATTR_RW(tx_test);

/* Runtime re-apply of the DMA/BMU config + re-run of the TX self-test.
 * Lets both cfgmode sets be compared inside one boot (no reflash):
 *   echo 1 > /sys/devices/platform/soc/18000000.pp/retrig          */
static ssize_t retrig_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct zx279131_priv *priv = dev_get_drvdata(dev);

	dev_info(priv->dev,
		 "retrig: cfgmode=%d ubtail=%d ubcla=%d ubpool=%d swmsk=%#x, then SW re-init + TX self-test\n",
		 cfgmode, ubtail, ubcla, ubpool, swmsk);
	zx279131_dma_init(priv);
	zx279131_bmu_cfg(priv);
	zx279131_ub_tail(priv);
	zx279131_fabric_init(priv);
	zx279131_tx_selftest(priv);
	return count;
}
static DEVICE_ATTR_WO(retrig);

/* v8.4: generic physical peek/poke for bring-up debugging.
 *
 * Why not /dev/mem: this rootfs has no busybox devmem applet, and plain
 * read()/write() on /dev/mem cannot reach MMIO on arm64 anyway --
 * valid_phys_addr_range() only admits memblock RAM (returns -EFAULT for
 * anything else), while only the mmap() path merely bounds the address.
 * These two attributes give the same capability without /dev/mem:
 *
 *   echo 10e1004c > /sys/devices/platform/soc/18000000.pp/zx_peek
 *   echo 10e10000 16 > .../zx_peek          # 16 consecutive words
 *   echo 10e1004c ff80ff > .../zx_poke
 *
 * Addresses/values are HEX (0x prefix optional).  Results go to dev_info,
 * so they appear on the serial console.  RAM ranges are mapped with
 * memremap(), everything else with ioremap().  An address backed by
 * nothing raises a bus fault -> kernel oops, so only touch registers a
 * datasheet or the U-Boot disassembly vouches for.
 */
static int zx_phys_rw(u64 pa, u32 *val, bool is_write)
{
	unsigned long off = (unsigned long)pa & ~PAGE_MASK;
	void __iomem *m;

	if (pfn_valid(pa >> PAGE_SHIFT))
		m = (void __iomem *)memremap(pa & PAGE_MASK, PAGE_SIZE,
					     MEMREMAP_WB);
	else
		m = ioremap(pa & PAGE_MASK, PAGE_SIZE);
	if (!m)
		return -ENOMEM;

	if (is_write)
		writel(*val, m + off);
	else
		*val = readl(m + off);

	if (pfn_valid(pa >> PAGE_SHIFT))
		memunmap((void *)m);
	else
		iounmap(m);
	return 0;
}

static ssize_t zx_peek_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "echo <hex phys_addr> [count] > zx_peek\n"
			 "reads count (default 1, max 64) consecutive 32-bit words,\n"
			 "printed to the kernel log / serial console.\n");
}

static ssize_t zx_peek_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	u64 pa;
	unsigned int i, n = 1;

	if (sscanf(buf, "%llx %u", &pa, &n) < 1)
		return -EINVAL;
	if (n < 1 || n > 64)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		u32 v;
		int ret = zx_phys_rw(pa + i * 4, &v, false);

		if (ret) {
			dev_info(dev, "peek %#010llx: %d\n", pa + i * 4, ret);
			break;
		}
		dev_info(dev, "peek %#010llx = %#010x\n", pa + i * 4, v);
	}
	return count;
}
static DEVICE_ATTR_RW(zx_peek);

static ssize_t zx_poke_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "echo <hex phys_addr> <hex value> > zx_poke\n"
			 "writes one 32-bit word.\n");
}

static ssize_t zx_poke_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	u64 pa, val;
	u32 v;
	int ret;

	if (sscanf(buf, "%llx %llx", &pa, &val) != 2)
		return -EINVAL;

	v = (u32)val;
	ret = zx_phys_rw(pa, &v, true);
	if (ret)
		return ret;

	dev_info(dev, "poke %#010llx = %#010x\n", pa, v);
	return count;
}
static DEVICE_ATTR_RW(zx_poke);

/* v8.5: runtime access to the w12 base-bit override (rationale at its
 * definition near swmsk).  Write a new value, then `echo 1 > retrig` to
 * re-run the self-test with it. */
static ssize_t w12base_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%#x\n", w12base);
}

static ssize_t w12base_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	u32 v;

	if (kstrtou32(buf, 0, &v))
		return -EINVAL;
	w12base = v;
	dev_info(dev, "w12base = %#x (0 = per-tx_mode default)\n", w12base);
	return count;
}
static DEVICE_ATTR_RW(w12base);

static int zx279131_probe(struct platform_device *pdev)
{
	struct zx279131_priv *priv;
	struct net_device *ndev;
	int err, i;

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	priv = netdev_priv(ndev);
	priv->dev = &pdev->dev;
	priv->ndev = ndev;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	platform_set_drvdata(pdev, priv);
	priv->cur_mode = tx_mode;
	for (i = 0; i < ZX279131_NUM_GPHY; i++)
		priv->phy_state[i] = -1;

	priv->pp = devm_ioremap(&pdev->dev, ZX279131_PP_PA, ZX279131_PP_SIZE);
	priv->sw = devm_ioremap(&pdev->dev, ZX279131_SW_PA, ZX279131_SW_SIZE);
	priv->mdio = devm_ioremap(&pdev->dev, ZX279131_MDIO_PA,
				  ZX279131_MDIO_SIZE);
	priv->idm = devm_ioremap(&pdev->dev, ZX279131_IDM_PA,
				 ZX279131_IDM_SIZE);
	if (!priv->pp || !priv->sw || !priv->mdio) {
		err = -ENOMEM;
		goto err_free;
	}

	dev_info(&pdev->dev, "probe: pp[0]=%#x sw0=%#x swmain=%#x mdio[0]=%#x\n",
		 zx_rd(priv, 0), readl(priv->sw), readl(priv->sw + 0x100000),
		 readl(priv->mdio));

	/* Capture whatever the bootloader left in the PP/BMU before we touch
	 * anything.  U-Boot's own TX demonstrably works (its TFTP ACKs reached
	 * the host over this very engine), so this residue is a known-good
	 * reference for every register we are about to overwrite.  The DONE
	 * registers are deliberately not read here: they are read-clear. */
	dev_info(&pdev->dev,
		 "uboot1: ctrl0=%#x m04=%#x qw=%#x/%#x m28=%#x/%#x tmo=%#x cfg=%#x brst=%#x\n",
		 zx_rd(priv, PP_DMA_CTRL0), zx_rd(priv, PP_DMA_MAGIC04),
		 zx_rd(priv, PP_DMA_QW0), zx_rd(priv, PP_DMA_QW1),
		 zx_rd(priv, PP_DMA_MODE28), zx_rd(priv, PP_DMA_MODE2C),
		 zx_rd(priv, PP_DMA_TIMEOUT), zx_rd(priv, PP_DMA_DESC_CFG),
		 zx_rd(priv, PP_DMA_BURST));
	dev_info(&pdev->dev,
		 "uboot2: rA=%#x/%#x rB=%#x/%#x m40=%#x/%#x/%#x/%#x\n",
		 zx_rd(priv, PP_DMA_RINGA_BASE), zx_rd(priv, PP_DMA_RINGA_MISC),
		 zx_rd(priv, PP_DMA_RINGB_BASE), zx_rd(priv, PP_DMA_RINGB_MISC),
		 zx_rd(priv, PP_DMA_M40), zx_rd(priv, PP_DMA_M44),
		 zx_rd(priv, PP_DMA_M48), zx_rd(priv, PP_DMA_TXFREE));
	dev_info(&pdev->dev,
		 "uboot3: bmu ctrl=%#x cfg=%#x/%#x cnt=%#x/%#x c=%#x/%#x fifo=%#x\n",
		 bmu_rd(priv, PP_BMU_CTRL), bmu_rd(priv, PP_BMU_CFG_A),
		 bmu_rd(priv, PP_BMU_CFG_B), bmu_rd(priv, PP_BMU_POOLCNT_TX),
		 bmu_rd(priv, PP_BMU_POOLCNT_RX), bmu_rd(priv, PP_BMU_CFG_C),
		 bmu_rd(priv, PP_BMU_CFG_D), bmu_rd(priv, PP_BMU_FIFO_STATUS));
	dev_info(&pdev->dev,
		 "uboot4: pool e8=%#x ec=%#x f0=%#x f4=%#x f8=%#x fc=%#x\n",
		 zx_rd(priv, PP_POOL_TBL0), zx_rd(priv, PP_POOL_TBL1),
		 zx_rd(priv, PP_POOL_RXRING), zx_rd(priv, PP_POOL_TX),
		 zx_rd(priv, PP_POOL_RX), zx_rd(priv, PP_POOL_STRIDES));

	/* v7.2: the one PP window never compared against stock.  The stock
	 * firmware (NAND boot, running) reads
	 *   008=0x2001  010=0xffffffff  018=0x1  038=0x10001
	 * while our driver never writes any of them (PORT-NOTES 28.5). */
	dev_info(&pdev->dev,
		 "ppring1: 000=%#x 004=%#x 008=%#x 00c=%#x 010=%#x 014=%#x 018=%#x 01c=%#x\n",
		 zx_rd(priv, 0x18000), zx_rd(priv, 0x18004),
		 zx_rd(priv, 0x18008), zx_rd(priv, 0x1800c),
		 zx_rd(priv, 0x18010), zx_rd(priv, 0x18014),
		 zx_rd(priv, 0x18018), zx_rd(priv, 0x1801c));
	dev_info(&pdev->dev,
		 "ppring2: 020=%#x 024=%#x 028=%#x 02c=%#x 030=%#x 034=%#x 038=%#x 03c=%#x 040=%#x 044=%#x 048=%#x 04c=%#x\n",
		 zx_rd(priv, 0x18020), zx_rd(priv, 0x18024),
		 zx_rd(priv, 0x18028), zx_rd(priv, 0x1802c),
		 zx_rd(priv, 0x18030), zx_rd(priv, 0x18034),
		 zx_rd(priv, 0x18038), zx_rd(priv, 0x1803c),
		 zx_rd(priv, 0x18040), zx_rd(priv, 0x18044),
		 zx_rd(priv, 0x18048), zx_rd(priv, 0x1804c));

	/* v7.0: the PP top block 0x18000000..0x18000040.  The stock firmware
	 * reads 0x7e00140 / 0x9cf8 / 0xd8e8 x7 there while we read
	 * 0x08000140 / 0x41 / 0 - and neither the vendor plat module nor
	 * switch.ko writes any PP register below 0x4048 (all 25 PP-base
	 * accesses are 0x4048/0x4328/0x8200/0xc0xx).  Dumping it here, before
	 * we touch anything, tells us whether those values are bootloader
	 * residue (writable) or hardware state (not configurable). */
	dev_info(&pdev->dev,
		 "pptop: 000=%#x 004=%#x 008=%#x 00c=%#x 010=%#x 018=%#x 020=%#x 028=%#x\n",
		 zx_rd(priv, 0x000), zx_rd(priv, 0x004), zx_rd(priv, 0x008),
		 zx_rd(priv, 0x00c), zx_rd(priv, 0x010), zx_rd(priv, 0x018),
		 zx_rd(priv, 0x020), zx_rd(priv, 0x028));
	dev_info(&pdev->dev,
		 "pptop: 030=%#x 038=%#x 040=%#x 0a0=%#x 0a8=%#x 0b0=%#x 0b8=%#x 0e0=%#x 0e4=%#x\n",
		 zx_rd(priv, 0x030), zx_rd(priv, 0x038), zx_rd(priv, 0x040),
		 zx_rd(priv, 0x0a0), zx_rd(priv, 0x0a8), zx_rd(priv, 0x0b0),
		 zx_rd(priv, 0x0b8), zx_rd(priv, 0x0e0), zx_rd(priv, 0x0e4));

	/* TX rings A and B: 32 KiB each */
	if (lowring) {
		/* v8.6: same physical addresses U-Boot uses, inside the D1
		 * no-map reservation.  devm_memremap() ties the mapping to
		 * the device lifetime, so there is nothing to undo on the
		 * error or remove paths. */
		priv->tx_ring_a_dma = ZX_LOWRING_A_PA;
		priv->tx_ring_b_dma = ZX_LOWRING_B_PA;
		priv->tx_ring_a = (struct zx279131_tx_desc *)
			devm_memremap(&pdev->dev, ZX_LOWRING_A_PA,
				      ZX_LOWRING_BYTES, MEMREMAP_WC);
		priv->tx_ring_b = (struct zx279131_tx_desc *)
			devm_memremap(&pdev->dev, ZX_LOWRING_B_PA,
				      ZX_LOWRING_BYTES, MEMREMAP_WC);
		if (priv->tx_ring_a && priv->tx_ring_b) {
			/* scrub U-Boot's stale descriptors */
			memset(priv->tx_ring_a, 0, ZX_LOWRING_BYTES);
			memset(priv->tx_ring_b, 0, ZX_LOWRING_BYTES);
		}
	} else {
		priv->tx_ring_a = zx_alloc_coherent(priv,
				ZX279131_TX_RING_SIZE * ZX279131_TX_DESC_SIZE,
				&priv->tx_ring_a_dma);
		priv->tx_ring_b = zx_alloc_coherent(priv,
				ZX279131_TX_RING_SIZE * ZX279131_TX_DESC_SIZE,
				&priv->tx_ring_b_dma);
	}
	if (!priv->tx_ring_a || !priv->tx_ring_b) {
		err = -ENOMEM;
		goto err_free;
	}
	dev_info(&pdev->dev, "rings: A=%pa B=%pa lowring=%d\n",
		 &priv->tx_ring_a_dma, &priv->tx_ring_b_dma, lowring);

	if (tx_mode == 3) {
		priv->tx_ring = priv->tx_ring_a;
		priv->tx_ring_dma = priv->tx_ring_a_dma;
		priv->doorbell_off = PP_DMA_RINGA_DOOR;
		priv->done_off = PP_DMA_RINGA_DONE;
	} else {
		priv->tx_ring = priv->tx_ring_b;
		priv->tx_ring_dma = priv->tx_ring_b_dma;
		priv->doorbell_off = PP_DMA_RINGB_DOOR;
		priv->done_off = PP_DMA_RINGB_DONE;
	}

	/* v8.1 staged RED snapshot: U-Boot hands the RED block over with
	 * STATUS bit0 = 1 (verified with md at the U-Boot prompt), but by the
	 * time this driver's pp_red_init runs bit0 is 0 and none of our RED
	 * writes stick.  Print the block at each stage so we can see which
	 * init step - if any - is what kills it. */
	dev_info(&pdev->dev,
		 "red0: status=%#x cmd=%#x cfg0=%#x (before any PP write)\n",
		 zx_rd(priv, PP_RED_STATUS), zx_rd(priv, PP_RED_CMD),
		 zx_rd(priv, PP_RED_CFG0));

	zx279131_dma_init(priv);
	zx279131_m48_snap(priv, "dma");

	/* the done register is read-clear; reclaim() is its only reader */

	err = zx279131_bmu_init(priv);
	if (err)
		goto err_free;

	/* pp_bmu_enable() */
	bmu_wr(priv, PP_BMU_CTRL, 1);

	/* BMU self test: one alloc + immediate free */
	{
		u32 bp = 0xffff;

		err = zx279131_bmu_alloc(priv, &bp);
		dev_info(&pdev->dev, "bmu selftest: alloc -> %s bp=%u\n",
			 err ? "FAIL" : "ok", bp);
		if (!err)
			zx279131_bmu_free(priv, 0, bp);
	}

	/* v8.1 staged RED snapshot (see red0 above): after dma_init + bmu_init
	 * + BMU self-test, but before fabric_init writes anything. */
	dev_info(&pdev->dev,
		 "red1: status=%#x cmd=%#x cfg0=%#x (before fabric_init)\n",
		 zx_rd(priv, PP_RED_STATUS), zx_rd(priv, PP_RED_CMD),
		 zx_rd(priv, PP_RED_CFG0));
	zx279131_m48_snap(priv, "bmu");

	/* switch fabric (spa/brg/sqmg/sipc/red/cla) before SMAC */
	zx279131_fabric_init(priv);
	zx279131_m48_snap(priv, "fab");

	/* --- switch MAC init for the 4 GPHY ports --- */
	sw_wr(priv, SW_SOPC_HALFMODE, 0);
	for (i = 0; i < ZX279131_NUM_GPHY; i++) {
		zx_smac_mode(priv, i);
		zx_smac_reset(priv, 1u << i);
		zx_smac_init(priv, i, 0);
		zx_smac_port_up_wait(priv, i, true);
	}
	dev_info(&pdev->dev,
		 "smac: blocks %#x %#x %#x %#x mode=%#x sopc=%#x up=%d%d%d%d\n",
		 sw_rd(priv, SW_SMAC_BLOCK(0)), sw_rd(priv, SW_SMAC_BLOCK(1)),
		 sw_rd(priv, SW_SMAC_BLOCK(2)), sw_rd(priv, SW_SMAC_BLOCK(3)),
		 sw_rd(priv, SW_SMAC_MODE(0) & ~3), sw_rd(priv, SW_SOPC_HALFMODE),
		 priv->port_acked[0], priv->port_acked[1],
		 priv->port_acked[2], priv->port_acked[3]);
	zx279131_m48_snap(priv, "smac");

	/* v6.9: port mode registers 0..7 in one line (stock reference in
	 * the comment above swport) + the one entry stock has and we do not:
	 * port 6 (CPU/PP port) = 0xA01000.  Written only when it differs. */
	dev_info(&pdev->dev,
		 "swmode before: p0=%#x p1=%#x p2=%#x p3=%#x p4=%#x p5=%#x p6=%#x p7=%#x\n",
		 sw_rd(priv, SW_SMAC_MODE(0)), sw_rd(priv, SW_SMAC_MODE(1)),
		 sw_rd(priv, SW_SMAC_MODE(2)), sw_rd(priv, SW_SMAC_MODE(3)),
		 sw_rd(priv, SW_SMAC_MODE(4)), sw_rd(priv, SW_SMAC_MODE(5)),
		 sw_rd(priv, SW_SMAC_MODE(6)), sw_rd(priv, SW_SMAC_MODE(7)));
	if (swport) {
		u32 off = SW_SMAC_MODE(6);
		u32 v = sw_rd(priv, off);

		if ((v & ~3u) != 0x00a01000u) {
			sw_wr(priv, off, (v & 3u) | 0x00a01000u);
			dev_info(&pdev->dev,
				 "swmode p6: %#x -> %#x (stock value)\n",
				 v, sw_rd(priv, off));
		}
	}
	dev_info(&pdev->dev,
		 "swmode after:  p0=%#x p4=%#x p6=%#x sw0008=%#x sw000c=%#x\n",
		 sw_rd(priv, SW_SMAC_MODE(0)), sw_rd(priv, SW_SMAC_MODE(4)),
		 sw_rd(priv, SW_SMAC_MODE(6)),
		 sw_rd(priv, 0x0008), sw_rd(priv, 0x000c));

	/* v6.4 U-Boot comparison, in the boot log so one boot tells all.
	 * U-Boot's values for the same registers are in PORT-NOTES §23.2. */
	dev_info(&pdev->dev,
		 "cmp1: sw0000=%#x sw0008=%#x sw000c=%#x sw01e8=%#x pmode=%#x/%#x/%#x/%#x\n",
		 sw_rd(priv, 0x0000), sw_rd(priv, 0x0008), sw_rd(priv, 0x000c),
		 sw_rd(priv, 0x01e8),
		 sw_rd(priv, SW_SMAC_MODE(0)), sw_rd(priv, SW_SMAC_MODE(1)),
		 sw_rd(priv, SW_SMAC_MODE(2)), sw_rd(priv, SW_SMAC_MODE(3)));
	dev_info(&pdev->dev,
		 "cmp2: spa0=%#x alloc=%#x/%#x/%#x/%#x brg340=%#x brg344=%#x brg380=%#x brg188=%#x brg3dc=%#x brg3e0=%#x\n",
		 sw_rd(priv, 0x14000),
		 sw_rd(priv, 0x140d0), sw_rd(priv, 0x140d4),
		 sw_rd(priv, 0x140d8), sw_rd(priv, 0x140dc),
		 sw_rd(priv, SW_BRG_BASE + 0x340), sw_rd(priv, SW_BRG_BASE + 0x344),
		 sw_rd(priv, SW_BRG_BASE + 0x380), sw_rd(priv, SW_BRG_BASE + 0x188),
		 sw_rd(priv, SW_BRG_BASE + 0x3dc), sw_rd(priv, SW_BRG_BASE + 0x3e0));

	/* --- GPHY bring-up via MDIO --- */
	for (i = 0; i < ZX279131_NUM_GPHY; i++) {
		u32 phy = ZX279131_GPHY_ADDR0 + i;

		zx_mdio_write(priv, phy, GPHY_REG_SPEC_CTRL, 0xb640);
		zx_mdio_write(priv, phy, GPHY_REG_SPEC_STAT, phy11);
		zx_mdio_write(priv, phy, GPHY_REG_INT_EN, 5);
	}
	dev_info(&pdev->dev,
		 "gphy: addr %d-%d cfg done; r11=%#x r26: %#x %#x %#x %#x\n",
		 ZX279131_GPHY_ADDR0, ZX279131_GPHY_ADDR0 + 3,
		 zx_mdio_read(priv, ZX279131_GPHY_ADDR0, GPHY_REG_SPEC_STAT),
		 zx_mdio_read(priv, 10, GPHY_REG_STATUS26),
		 zx_mdio_read(priv, 11, GPHY_REG_STATUS26),
		 zx_mdio_read(priv, 12, GPHY_REG_STATUS26),
		 zx_mdio_read(priv, 13, GPHY_REG_STATUS26));
	zx279131_m48_snap(priv, "gphy");

	/* vendor pp_int_init: mask every PP source.  Unlike v6 (which then
	 * unmasked and livelocked because the handler never re-masked), we
	 * take the IRQ here and only clear bits[1:0] from ndo_open, and the
	 * handler masks again on entry - exactly the vendor protocol. */
	zx_wr(priv, PP_INT_MASK, 0xffffffff);
	zx279131_m48_snap(priv, "imask");
	priv->pp_irq = -1;
	if (ppirq) {
		int irq = platform_get_irq(pdev, 0);

		if (irq >= 0) {
			err = request_irq(irq, zx279131_pp_irq, 0,
					  "zx279131-pp", priv);
			if (err) {
				dev_warn(&pdev->dev,
					 "pp irq %d request failed: %d, polling only\n",
					 irq, err);
			} else {
				priv->pp_irq = irq;
			}
		} else {
			dev_warn(&pdev->dev,
				 "no pp irq in DT (%d), polling only\n", irq);
		}
	}

	ndev->netdev_ops = &zx279131_netdev_ops;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->min_mtu = ETH_ZLEN;
	ndev->max_mtu = 1500;

	err = of_get_ethdev_address(pdev->dev.of_node, ndev);
	if (err)
		eth_hw_addr_set(ndev, "\x00\xd0\xd0\x81\x82\x83"); /* vendor sw MAC */

	INIT_DELAYED_WORK(&priv->link_work, zx279131_link_work);
	INIT_DELAYED_WORK(&priv->pp_work, zx279131_pp_work);
	INIT_DELAYED_WORK(&priv->rx_work, zx279131_rx_work);

	/* v8.8: the engine reads TX data at +0x20 inside a pool slot (and RX
	 * lands at +0x20 too).  bootargs used to carry zx279131.bufoff=16,
	 * which silently threw every frame away - warn instead of repeating
	 * that debugging session. */
	if (bufoff != 0x20)
		dev_warn(&pdev->dev,
			 "bufoff=%u (expected 0x20): frames are built at +%u but the engine reads +0x20, TX/RX will not work\n",
			 bufoff, bufoff);

	err = register_netdev(ndev);
	if (err)
		goto err_free;

	/* initial link pass (vendor runs phy_process right after init) */
	zx279131_link_scan(priv, true);
	zx279131_m48_snap(priv, "link");

	/* Bring the PON SerDes front-end up.  Separate register block, cannot
	 * disturb the packet engine; see zx279131-pon.c for the provenance. */
	zx279131_pon_init();
	zx279131_pon_sysfs_add(&pdev->dev);

	/* diagnostic: inject frames before anything can stop the interface */
	if (selftest)
		zx279131_tx_selftest(priv);

	if (device_create_file(&pdev->dev, &dev_attr_zx_regs))
		dev_warn(&pdev->dev, "zx_regs sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_tx_test))
		dev_warn(&pdev->dev, "tx_test sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_retrig))
		dev_warn(&pdev->dev, "retrig sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_zx_raw))
		dev_warn(&pdev->dev, "zx_raw sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_zx_desc))
		dev_warn(&pdev->dev, "zx_desc sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_ppq))
		dev_warn(&pdev->dev, "ppq sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_zx_raw2))
		dev_warn(&pdev->dev, "zx_raw2 sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_zx_raw3))
		dev_warn(&pdev->dev, "zx_raw3 sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_cpuport))
		dev_warn(&pdev->dev, "cpuport sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_cpuen))
		dev_warn(&pdev->dev, "cpuen sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_smacstat))
		dev_warn(&pdev->dev, "smacstat sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_zx_peek))
		dev_warn(&pdev->dev, "zx_peek sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_zx_poke))
		dev_warn(&pdev->dev, "zx_poke sysfs file not created\n");
	if (device_create_file(&pdev->dev, &dev_attr_w12base))
		dev_warn(&pdev->dev, "w12base sysfs file not created\n");

	dev_info(&pdev->dev,
		 "v8.8c ready: %s tx_mode=%d door=%#x done=%#x burst=%#x swcfg=%d cfgmode=%d swport=%d pp_irq=%d\n"
		 "  ub   : ubtail=%d ubcla=%d ubpool=%d redn=%d swmsk=%#x w12base=%#x lowring=%d bufoff=%#x\n"
		 "  cla  : cla2=%#x claen=%#x (4050/408c; stock runtime 7fff479e/372c)\n"
		 "  rx   : rxen=%d rxoff=%#x rxstride=%u rxpoll=%dms rxport=%#x rxfreen=%d rxrel=%d budget=%u\n"
		 "  diag: %s/zx_regs /zx_raw /zx_raw2 /zx_raw3 /zx_desc\n"
		 "  peek: zx_peek <hex addr> [count]    poke: zx_poke <hex addr> <hex val>\n"
		 "  run : tx_test  retrig  ppq  cpuport  cpuen  smacstat  w12base\n",
		 ndev->name, tx_mode, priv->doorbell_off, priv->done_off,
		 (u32)burst, swcfg, cfgmode, swport, priv->pp_irq,
		 ubtail, ubcla, ubpool, redn, swmsk, w12base, lowring, bufoff,
		 cla2, claen,
		 rxen, rxoff, rxstride, rxpoll_ms, rxport, rxfreen, rxrel,
		 rxbudget, dev_name(&pdev->dev));
	return 0;

err_free:
	if (priv->pp_irq >= 0)
		free_irq(priv->pp_irq, priv);
	free_netdev(ndev);
	return err;
}

static void zx279131_remove(struct platform_device *pdev)
{
	struct zx279131_priv *priv = platform_get_drvdata(pdev);
	struct net_device *ndev = priv->ndev;

	cancel_delayed_work_sync(&priv->link_work);
	cancel_delayed_work_sync(&priv->pp_work);
	if (priv->pp_irq >= 0) {
		zx_wr(priv, PP_INT_MASK, 0xffffffff);
		free_irq(priv->pp_irq, priv);
	}
	device_remove_file(&pdev->dev, &dev_attr_zx_regs);
	device_remove_file(&pdev->dev, &dev_attr_zx_raw);
	device_remove_file(&pdev->dev, &dev_attr_zx_desc);
	device_remove_file(&pdev->dev, &dev_attr_ppq);
	device_remove_file(&pdev->dev, &dev_attr_tx_test);
	device_remove_file(&pdev->dev, &dev_attr_retrig);
	device_remove_file(&pdev->dev, &dev_attr_zx_raw2);
	device_remove_file(&pdev->dev, &dev_attr_zx_raw3);
	device_remove_file(&pdev->dev, &dev_attr_cpuport);
	device_remove_file(&pdev->dev, &dev_attr_cpuen);
	device_remove_file(&pdev->dev, &dev_attr_smacstat);
	device_remove_file(&pdev->dev, &dev_attr_zx_peek);
	device_remove_file(&pdev->dev, &dev_attr_zx_poke);
	device_remove_file(&pdev->dev, &dev_attr_w12base);
	zx279131_pon_sysfs_del(&pdev->dev);
	unregister_netdev(ndev);
	free_netdev(ndev);
}

static const struct of_device_id zx279131_of_match[] = {
	{ .compatible = "zte,zx279131-pp" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx279131_of_match);

static struct platform_driver zx279131_driver = {
	.probe		= zx279131_probe,
	.remove		= zx279131_remove,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= zx279131_of_match,
	},
};

module_platform_driver(zx279131_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("F7610U port");
MODULE_DESCRIPTION("ZTE ZX279131 packet-engine Ethernet driver (v7)");
MODULE_VERSION(DRV_VERSION);
