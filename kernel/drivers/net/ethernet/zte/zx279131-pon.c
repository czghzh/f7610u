// SPDX-License-Identifier: GPL-2.0
/*
 * ZTE ZX279131 PON SerDes bring-up.
 *
 * The PON front-end is a separate block from the packet engine:
 *
 *   0x10e00000  sys_ctrl    (mode / reset release for the PON clock domain)
 *   0x10e10000  top_crm     (PON clock gate, reference clock, SerDes PLL)
 *   0x10e30000  ncs_pll     (byte-wide PLL trim file; leave alone)
 *   0x10e40000  serdes      (the actual SerDes register file)
 *
 * The sequence below was recovered from the vendor platform module
 * (plat-zxylzb_9131.ko: ref_clk_set / reg_def_set / serdes_mode_set, reached
 * through zx_pon_clk_reset_init) and then verified against a running stock
 * F7610U: after it runs, every SerDes register reads back byte-identical to
 * the vendor's live unit and 0x10e40068 bit4 ("serdes_rx_ready", the flag the
 * vendor GPON driver polls) comes up.
 *
 * Two traps worth remembering:
 *   - pon_serdes_base is 0x10e40000, NOT 0x10e30000.  The vendor DTS lists
 *     seven reg ranges for the PON node and of_iomap() index 4 is the SerDes.
 *   - 0x10e30000 is a byte-wide register file; 32-bit writes there only keep
 *     the low byte.  Writing the SerDes table there wedges the bus.
 */

#include <linux/bitops.h>
#include <crypto/hash.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define PON_SYSCTRL_BASE	0x10e00000
#define PON_TOP_CRM_BASE	0x10e10000
#define PON_SERDES_BASE		0x10e40000
#define PON_MAC_BASE		0x17000000
#define PON_BLOCK_SIZE		0x1000

/*
 * The MAC register file is split into 0xc0000-spaced windows on top of
 * 0x17000000, exactly like the vendor's xmac_vir_addr_map():
 *
 *   win 0  0x17000000   GPON MAC core (id, work mode, timers)
 *   win 3  0x171c0000   MPCP / TC layer
 *   win 4  0x17200000   interrupt status, GEM/queues
 *   win 6  0x17240000   reset block (0x04 = PON reset, write-only-ish)
 *
 * Window 4 is where xedpm_isr_handler() reads its status words, so it needs
 * its own mapping (0x200a04 is far outside the 4 KiB window 0 mapping).
 */
#define PON_MAC_WIN4_BASE	0x17200000
#define PON_MAC_WIN4_SIZE	0x2000

/*
 * 中断状态对（来自 zte_xgpon.ko: fi_Interrupt_PonMAC_Handle）
 *
 *   Hal_ReadXmacReg(0x16003) -> enable   落在 0x17000000 + 0x16003*4 = 0x1705800c
 *   Hal_ReadXmacReg(0x16004) -> status   落在 0x17058010
 *   event = enable & status             （vendor 就是这么算的）
 *   处理完把 event 写回 status 寄存器（write-1-clear）
 *
 * 0x800281 那对（我们早先当成窗口 4 的 0xa04/0xa08）在 GPON 路径上用不到，
 * 原厂活体读数一直是 0。
 */
#define MAC_IRQ_EN		0x16003
#define MAC_IRQ_STAT		0x16004

/* 仅用于兼容旧 sysfs 输出，保留读窗口 4 */
#define MAC_IRQ_STAT0		0xa04
#define MAC_IRQ_STAT1		0xa08
#define MAC_IRQ_INFO		0x9a8

#define MAC_WORK_MODE		0x018	/* 3 = GPON; 4 is the ROM default */

#define CRM_CLK_GATE		0x04c	/* bit24: PON clock, bit8: block enable */
#define CRM_REF_CLK		0x040	/* bits[8:3]: reference clock select  */
#define CRM_PLL			0x038	/* SerDes PLL: bits[23:16], [15:6], b27, b31 */
#define CRM_AUX			0x0fc

#define SERDES_PLL_WORD		0x058
#define SERDES_STATUS		0x068

#define REF_CLK_GPON		0x00001944	/* ref_clk_set(7): field 0x28 */
#define PLL_WORD_GPON		0x0828142a

/* reg_def_set() + serdes_mode_set(mode 7), captured from a live vendor unit
 * running GPON. */
static const u32 pon_serdes_tbl[24] = {
	0xe009fef3, 0x400019ff, 0x00001dd0, 0x00003034,
	0x00000000, 0x118f2000, 0x00950231, 0x01a1e014,
	0x48000002, 0x09570087, 0x00080000, 0x00000000,
	0x01212000, 0x00793d46, 0x00000000, 0x00000000,
	0x003c0000, 0xea00a013, 0x101038ca, 0x0005a088,
	0x33333333, 0x33333333, 0x03e23333, 0x00000000,
};

static void __iomem *pon_crm;
static void __iomem *pon_ser;
static void __iomem *pon_sys;
static void __iomem *pon_mac;
static void __iomem *pon_mac_c;		/* 0x170c0000 */
static void __iomem *pon_mac_i;		/* 0x17140000 (中断模块) */
static void __iomem *pon_mac_win4;

/*
 * GPON MAC 的已知良好配置。
 *
 * 来源：把原厂（4.1.25 + zte_xgpon）活体上 86 个 HAL 访问过的寄存器全读了一遍，
 * 和我们的镜像逐项对比，只有 28 个不一致；下面这张表是其中属于「配置」的那部分，
 * 每条都是在那台跑着的原厂机上验证过能写进去、且写完中断线能拉起来的。
 *
 * 三个分组：
 *   0x170000xx / 0x170400xx  MAC 顶层的模式与门控
 *   0x17058xxx               XMAC 收发控制（bit0 of 0x58400 = 下行/RX 使能）
 *   0x17059xxx               GPON profile 与上行参数
 *   0x170c0028 / 0x1714xxxx  中断模块与 GEM 统计使能
 *
 * val/mask 语义：value = (old & ~mask) | (val & mask)
 */
struct pon_mac_reg {
	u32 addr;
	u32 val;
	u32 mask;
};

static const struct pon_mac_reg pon_mac_tbl[] = {
	/* --- MAC 顶层 --- */
	{ 0x17000020, 0x00000320, 0xffffffff },
	{ 0x17000044, 0xffffff0f, 0xffffffff },
	{ 0x17040014, 0x00000002, 0xffffffff },

	/* --- GPON profile --- */
	{ 0x17059008, 0x00000008, 0xffffffff },
	{ 0x17059400, 0x00000003, 0x00000003 },
	{ 0x17059404, 0x00010000, 0xffffffff },
	{ 0x1705940c, 0x0001cde3, 0xffffffff },
	{ 0x17059428, 0x000000dd, 0xffffffff },

	/* --- 上行 / 下行 --- */
	/* 原厂 O5 实测 0x16：bit1/2/4 置位，**bit0 必须为 0**（我们之前是 0x17） */
	{ 0x17059800, 0x00000016, 0x0000001f },
	{ 0x17059910, 0x00001b01, 0xffffffff },
	{ 0x1705a27c, 0x00000001, 0xffffffff },
	{ 0x17058414, 0x00000002, 0xffffffff },
	{ 0x1705842c, 0x00080000, 0x00080000 },
	{ 0x17058430, 0x000007ff, 0x000007ff },
	/* bit0 = 下行接收使能，bit5 = Hw_Init 里置的那个位 */
	{ 0x17058400, 0x00000021, 0x00000021 },

	/* --- 中断 --- */
	{ 0x1705800c, 0x000009be, 0x000009be },	/* 事件使能 */
	{ 0x170c0028, 0x00000001, 0xffffffff },
	{ 0x17140100, 0x00000001, 0xffffffff },
	{ 0x17140300, 0x00000001, 0xffffffff },
};

static void __iomem *pon_mac_map(u32 addr)
{
	if (addr >= 0x17000000 && addr < 0x17060000)
		return pon_mac ? pon_mac + (addr - 0x17000000) : NULL;
	if (addr >= 0x170c0000 && addr < 0x170d0000)
		return pon_mac_c ? pon_mac_c + (addr - 0x170c0000) : NULL;
	if (addr >= 0x17140000 && addr < 0x17150000)
		return pon_mac_i ? pon_mac_i + (addr - 0x17140000) : NULL;
	return NULL;
}

static int pon_mac_rmw(u32 addr, u32 val, u32 mask)
{
	void __iomem *p = pon_mac_map(addr);
	u32 old;

	if (!p)
		return -EINVAL;
	old = readl(p);
	writel((old & ~mask) | (val & mask), p);
	return 0;
}

static bool pon_init_done;
static u32 pon_last_status;

/*
 * PLOAM 消息队列（fi_Configure_Interrupt_Read_Ploam_Msg）：
 *
 *   轮询 0x16147 (0x1705851c) 的 bit0，最多 16 次
 *   从 0x16148 (0x17058520) 连读，每字要 rev（字节序翻转）
 *   向 0x16147 写 0        <-- 这一步才真正让中断线落下
 *
 * 长度是 **12 个字 = 48 字节**（厂商循环从 0x16148 到 0x16153），
 * 发送侧 fi_Configure_Ponmac_Send_Ploam_Msg 也是 48 字节、12 字，
 * 所以 48 是这个硬件的报文粒度，不是 G.984 的 13 字节。
 *
 * 不取消息就会一直 level 触发，实测会把整机打进中断风暴。
 */
#define XMAC_PLOAM_CTRL		0x1705851c
#define XMAC_PLOAM_DATA		0x17058520
#define PLOAM_WORDS		12
#define PLOAM_LEN		(PLOAM_WORDS * 4)
#define PLOAM_RING		8

static u8 pon_ploam_ring[PLOAM_RING][PLOAM_LEN];
static unsigned int pon_ploam_ring_idx;
static u8 pon_ploam[PLOAM_LEN];		/* 最近一条 */
static u32 pon_ploam_n;

/*
 * 下行状态广播帧的三个关键字节（48 字节帧内偏移）：
 *
 *   off 3   序号，每 0.5 秒 +1
 *   off 15  注册状态：原厂 O5 = 0x08，未注册 = 0x00
 *   off 16  授权规模：原厂 O5 = 0x1f（31 个时隙），未注册 = 0x04 / 0x08
 *
 * 这是 OLT 直接告诉我们的状态，比 MPCP 侧任何寄存器都可靠。
 * 存最近 8 条，便于观察它从 0x00 跳到 0x08 的那一刻。
 */
#define PLOAM_REG_HIST	8
static u8 pon_seq_h[PLOAM_REG_HIST];
static u8 pon_regflag_h[PLOAM_REG_HIST];
static u8 pon_grant_h[PLOAM_REG_HIST];
static unsigned int pon_reg_h_idx;
static u8 pon_seq, pon_regflag, pon_grant;

/* 0x17058424 是 125us 帧计数器（实测 ~8000/s），原厂 ISR 在 bit2 分支里读它 */
static u32 pon_frame_cnt;

/* GPON 激活状态机（实现见文件末尾），下行报文类型统计挂在 drain 里 */
static void gpon_note_msg(const u8 *m);
static void gpon_init_work(void);

static int pon_ploam_drain(void)
{
	void __iomem *ctl = pon_mac_map(XMAC_PLOAM_CTRL);
	void __iomem *data = pon_mac_map(XMAC_PLOAM_DATA);
	u32 *w;
	int i, tries;

	if (!ctl || !data)
		return -EINVAL;

	for (tries = 0; tries < 16; tries++) {
		if (readl(ctl) & BIT(0))
			break;
		udelay(2);
	}

	w = (u32 *)pon_ploam_ring[pon_ploam_ring_idx];
	for (i = 0; i < PLOAM_WORDS; i++)
		w[i] = swab32(readl(data + i * 4));

	writel(0, ctl);

	memcpy(pon_ploam, w, PLOAM_LEN);
	pon_ploam_ring_idx = (pon_ploam_ring_idx + 1) % PLOAM_RING;
	pon_ploam_n++;
	pon_frame_cnt = readl(pon_mac_map(0x17058424));

	/*
	 * w[] 已经 swab 过，在小端上 w[n] 的内存字节就是帧内第 4n..4n+3，
	 * 且是网络字节序（大端）：
	 *     m[4n+0] = w[n] 的 **最低** 字节，m[4n+3] = w[n] 的最高字节。
	 * 所以"帧内第 k 字节"不是统一用 >>24，取值方向取决于 k%4。
	 */
	pon_seq = (w[0] >> 24) & 0xff;	/* off 3  */
	pon_regflag = (w[3] >> 24) & 0xff;	/* off 15 */
	pon_grant = w[4] & 0xff;		/* off 16 */
	pon_seq_h[pon_reg_h_idx] = pon_seq;
	pon_regflag_h[pon_reg_h_idx] = pon_regflag;
	pon_grant_h[pon_reg_h_idx] = pon_grant;
	pon_reg_h_idx = (pon_reg_h_idx + 1) % PLOAM_REG_HIST;

	gpon_note_msg(pon_ploam);

	return 0;
}

/*
 * 上行 PLOAM/MPCP 发送通路（fi_Configure_Ponmac_Send_Ploam_Msg）
 *
 *   轮询 0x16642 (0x17059908) 直到 0（FIFO 空）
 *   把 48 字节按 12 个字写进 0x66c2..(0x17059b08..)，每字要 rev
 *   写 0x16642 = 1  <-- 这一下就真的往光纤上发了
 *   再轮询回 0
 *
 * 报文 48 字节 = 40 字节正文 + 8 字节 AES-CMAC（Drv_PloamuMic_Generate_Activation
 * 对 "0x02 + 前 40 字节" 做 CMAC，结果写回 +40）。
 * 发送侧格式里 MsgID 在第 [2] 字节。
 *
 * ⚠ 上行是时分共享介质，乱发会撞到同 PON 口其他 ONU 的时隙。
 *   所以这里拆成两步：pon_send 只排队，pon_fire 才触发。
 */
#define XMAC_UP_TX_STAT		0x17059908
#define XMAC_UP_TX_FIFO		0x17059b08
#define UP_MSG_LEN		48
#define UP_MSG_WORDS		(UP_MSG_LEN / 4)

static u8 pon_up_msg[UP_MSG_LEN];
static bool pon_up_queued;
static u32 pon_up_sent;

static int pon_up_wait_empty(void)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (!readl(pon_mac_map(XMAC_UP_TX_STAT)))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

/* 只把报文压进硬件 FIFO，不触发发送 */
static int pon_up_load(const u8 *msg)
{
	void __iomem *fifo = pon_mac_map(XMAC_UP_TX_FIFO);
	int i;

	if (!fifo)
		return -EINVAL;
	if (pon_up_wait_empty())
		return -ETIMEDOUT;

	for (i = 0; i < UP_MSG_WORDS; i++) {
		u32 v;

		memcpy(&v, msg + i * 4, 4);
		writel(swab32(v), fifo + i * 4);
	}
	pon_up_queued = true;
	return 0;
}

static int pon_up_fire(void)
{
	if (!pon_up_queued)
		return -EINVAL;
	writel(1, pon_mac_map(XMAC_UP_TX_STAT));
	pon_up_queued = false;
	pon_up_sent++;
	return 0;
}

static int hex2bin_u8(const char *s, u8 *out, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		unsigned int v;

		if (sscanf(s + i * 2, "%2x", &v) != 1)
			return -EINVAL;
		out[i] = v;
	}
	return 0;
}

static ssize_t pon_send_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	u8 tmp[UP_MSG_LEN];
	char *s;
	int ret;

	s = skip_spaces(buf);
	if (strlen(s) < UP_MSG_LEN * 2)
		return -EINVAL;
	ret = hex2bin_u8(s, tmp, UP_MSG_LEN);
	if (ret)
		return ret;
	ret = pon_up_load(tmp);
	if (ret)
		return ret;
	memcpy(pon_up_msg, tmp, UP_MSG_LEN);
	return count;
}
static DEVICE_ATTR_WO(pon_send);

static ssize_t pon_fire_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int ret = pon_up_fire();

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(pon_fire);

static ssize_t pon_send_state_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int i, n;

	n = sysfs_emit(buf, "queued=%d sent=%u stat=%#010x\n",
		       pon_up_queued, pon_up_sent,
		       pon_mac_map(XMAC_UP_TX_STAT) ?
		       readl(pon_mac_map(XMAC_UP_TX_STAT)) : 0);
	for (i = 0; i < UP_MSG_LEN; i++)
		n += sysfs_emit_at(buf, n, "%02x", pon_up_msg[i]);
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RO(pon_send_state);

/*
 * 两张表，长度以原厂 O5 快照 (work/o5_regs.json) 为准，不能凭猜：
 *   0x17058040..0x170583fc : 240 项，首项 0x04444444，其余 0x09999999
 *   0x17059200..0x170593f8 : 127 项，全 0x00000009（0x170593fc 保持 0）
 * 我们读出来是 0x4bef / 0x1 的默认值 ⇒ 这两张表从来没配过。
 *
 * 厂商 fi_Configure_Hw_Init 只写 0x16010=0x4444、0x16011=0x9999，
 * 但实测读出 0x04444444 / 0x09999999 ⇒ 写一次会铺开一整片。
 *
 * ★ 早先这里按 256 项写，第二张表越界 512 字节，把 0x17059400 起的
 *   ONU 状态 / SN / profile 全刷成 0x9，整块 TX 状态被毁。长度必须卡死。
 *
 * probe 阶段自动执行（pon_allocmap 节点仍保留，可用于手动重放）。
 */
#define PON_ALLOCMAP1	0x17058040
#define PON_ALLOCMAP1_N	240
#define PON_ALLOCMAP2	0x17059200
#define PON_ALLOCMAP2_N	127

static int pon_allocmap_apply(void)
{
	void __iomem *p0 = pon_mac_map(PON_ALLOCMAP1);
	void __iomem *p1 = pon_mac_map(PON_ALLOCMAP2);
	int i;

	if (!p0 || !p1)
		return -EINVAL;

	writel(0x04444444, p0);
	for (i = 1; i < PON_ALLOCMAP1_N; i++)
		writel(0x09999999, p0 + i * 4);

	for (i = 0; i < PON_ALLOCMAP2_N; i++)
		writel(0x00000009, p1 + i * 4);

	pr_info("zx279131-pon: allocmap applied (%d+%d)\n",
		PON_ALLOCMAP1_N, PON_ALLOCMAP2_N);
	return 0;
}

static ssize_t pon_allocmap_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int v;
	int ret;

	if (kstrtouint(skip_spaces(buf), 10, &v))
		return -EINVAL;
	if (v != 1)
		return -EINVAL;
	ret = pon_allocmap_apply();
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(pon_allocmap);

/*
 * GPON MAC 中断：enable & status，处理完把 event 写回 status（W1C）。
 *
 * 目前只真正处理 bit2（PLOAM 到达），其余位按厂商做法写回状态寄存器应答，
 * 不做事件分派 —— MPCP/PLOAM 状态机还没写。
 *
 * 风暴保护做在中断里（不能指望有人去读 sysfs）：同一个 jiffy 里超过 200 次
 * 就认定电平没清掉，直接关线。
 */
static int pon_irq = -1;
static unsigned long pon_irq_count;
static u32 pon_irq_s0;			/* 最近一次 enable 寄存器 */
static u32 pon_irq_s1;			/* 最近一次 status 寄存器 */
static u32 pon_irq_ev;			/* 最近一次 event mask */
static u32 pon_irq_ev_or;		/* 累计见过的 event 位 */
static unsigned int pon_irq_storm;
static unsigned long pon_irq_j;		/* 上一个 jiffy */
static unsigned int pon_irq_j_n;	/* 同一 jiffy 内的次数 */

static irqreturn_t pon_irq_handler(int irq, void *data)
{
	u32 en, st, ev;

	if (!pon_mac)
		return IRQ_NONE;

	/* 同一 jiffy 内反复触发 = 电平没清掉，先关线再报 */
	if (jiffies == pon_irq_j) {
		if (++pon_irq_j_n > 200 && !pon_irq_storm) {
			pon_irq_storm = 1;
			disable_irq_nosync(irq);
			pr_err("zx279131-pon: irq storm, line %d disabled (en=%#x st=%#x)\n",
			       irq, pon_irq_s0, pon_irq_s1);
			return IRQ_HANDLED;
		}
	} else {
		pon_irq_j = jiffies;
		pon_irq_j_n = 1;
	}

	en = readl(pon_mac_map(0x1705800c));
	st = readl(pon_mac_map(0x17058010));
	ev = en & st;

	pon_irq_count++;
	pon_irq_s0 = en;
	pon_irq_s1 = st;
	pon_irq_ev = ev;
	pon_irq_ev_or |= ev;

	if (ev & BIT(2))
		pon_ploam_drain();

	/* 其余事件按厂商做法逐个写回状态位应答 */
	if (ev & ~BIT(2))
		writel(ev & ~BIT(2), pon_mac_map(0x17058010));

	if (pon_irq_count <= 3 || !(pon_irq_count & 0xfff))
		pr_info("zx279131-pon: irq %d #%lu en=%#010x st=%#010x ev=%#010x ploam=%u\n",
			irq, pon_irq_count, en, st, ev, pon_ploam_n);

	return IRQ_HANDLED;
}

static void zx279131_pon_irq_setup(void)
{
	struct device_node *np;
	int irq;

	np = of_find_compatible_node(NULL, NULL, "zte,zx279131-pon");
	if (!np) {
		pr_warn("zx279131-pon: no device tree node, irq not requested\n");
		return;
	}

	irq = irq_of_parse_and_map(np, 0);
	of_node_put(np);
	if (!irq) {
		pr_warn("zx279131-pon: node has no usable interrupt\n");
		return;
	}

	if (request_irq(irq, pon_irq_handler, 0, "zx279131-pon", NULL)) {
		pr_warn("zx279131-pon: request_irq(%d) failed\n", irq);
		return;
	}

	pon_irq = irq;
	pr_info("zx279131-pon: listening on irq %d\n", irq);
}

/*
 * Run the vendor bring-up.  Idempotent: the vendor runs it on every probe.
 * Returns the final SerDes status word (0x10e40068).
 */
u32 zx279131_pon_serdes_init(void)
{
	int i;
	u32 st;

	if (!pon_crm || !pon_ser || !pon_sys)
		return 0;

	/* 0. the ROM leaves work_mode at 4; the vendor probe selects GPON (3) */
	if (pon_mac)
		writel(3, pon_mac + MAC_WORK_MODE);

	/* 1. bounce the PON clock so the block starts from a known state */
	writel(0x00fffeff, pon_crm + CRM_CLK_GATE);
	msleep(20);
	writel(0x01fffeff, pon_crm + CRM_CLK_GATE);
	msleep(20);

	/* 2. reference clock: ref_clk_set(7) selects the GPON divider */
	writel(REF_CLK_GPON, pon_crm + CRM_REF_CLK);
	msleep(20);

	/* 3. SerDes register file (reg_def_set + serdes_mode_set) */
	for (i = 0; i < ARRAY_SIZE(pon_serdes_tbl); i++)
		writel(pon_serdes_tbl[i], pon_ser + i * 4);

	/* 4. let the block out of reset and enable its aux clock */
	writel(0x01ffffff, pon_crm + CRM_CLK_GATE);
	/* NOTE: keep 0x59 here.  Writing the stock 0x00DFF3FF at this point
	 * makes the board fail to bring up the network after bootm
	 * (v9.35 experiment) -- the stock value is presumably only valid
	 * after its own clock tree is up. */
	writel(0x00000059, pon_crm + CRM_AUX);

	/* 5. SerDes PLL word, then the PLL reset pulse.
	 * Stock reads back 0x03E23333 (bit31/29/28 clear); writing
	 * 0xB3E23333 leaves those bits set. */
	writel(0x03e23333, pon_ser + SERDES_PLL_WORD);
	msleep(20);

	writel(PLL_WORD_GPON, pon_crm + CRM_PLL);
	msleep(20);
	writel(PLL_WORD_GPON, pon_crm + CRM_PLL);
	msleep(20);
	writel(PLL_WORD_GPON | 0x80000000, pon_crm + CRM_PLL);	/* assert */
	msleep(20);
	writel(PLL_WORD_GPON, pon_crm + CRM_PLL);		/* release */
	msleep(50);

	/* 6. PON domain clock/reset select */
	writel(0x00002000, pon_sys + 0x010);
	writel(0x0000007a, pon_sys + 0x018);
	writel(0x0000009f, pon_sys + 0x020);
	msleep(50);

	st = readl(pon_ser + SERDES_STATUS);
	pon_last_status = st;
	pon_init_done = true;
	return st;
}
EXPORT_SYMBOL_GPL(zx279131_pon_serdes_init);

/*
 * 把 MAC 侧配到原厂的已知良好状态。必须在 SerDes 起来之后跑：
 * 0x17058400 的 bit0（下行接收使能）打开后，MAC 才会开始往中断模块报事件。
 * 返回最终的 0x17058400。
 */
/*
 * ★ 2026-10-19 深夜与原厂逐项对照（PON-PLAN §91）：
 *   原厂运行稳定后 GPIO bank3 DATA = 0x08，我们（含 U-Boot 默认）是 0x0b，
 *   bit0/bit1 多高两位。这两个脚是 PON 子块的硬件使能脚，DATA 直写不生效，
 *   只能用 CLR 清。同时 SYSCTRL 0x10e0004c 原厂 0xB5、我们 0x94（少 bit0/bit5）。
 *   都对齐到原厂值（原厂值来自 telnet devmem2 实测）。
 */
static void zx279131_pon_board_align(void)
{
	void __iomem *gpio, *sys, *mux;
	u32 v;

	gpio = ioremap(0x10d10000, 0x100);
	if (gpio) {
		writel(0x3, gpio + 0xc0 + 0x1c);	/* bank3 CLR: bit0/bit1 */
		pr_info("zx279131-pon: gpio bank3 dir=%#010x data=%#010x\n",
			readl(gpio + 0xc0), readl(gpio + 0xc0 + 0x14));
		iounmap(gpio);
	}

	sys = ioremap(0x10e00040, 0x10);
	if (sys) {
		v = readl(sys + 0x0c);
		writel(v | BIT(0) | BIT(5), sys + 0x0c);
		pr_info("zx279131-pon: sysctrl 4c %#010x -> %#010x\n",
			v, readl(sys + 0x0c));
		iounmap(sys);
	}

	/*
	 * ★ 原厂 zte_xgpon.ko 的 fi_Configure_Init 里有这一段（串口日志
	 *   "DEbug: GPON PIN_MUX_REG3:(0xfc)"）：
	 *     v = pin_mux[3]; v &= ~0x9; v |= 0x8; pin_mux[3] = v;
	 * 即 0x10e2000c 由 U-Boot 的 0xFD 变成 GPON 用的 0xFC。
	 * 实测写它不会断网，就照原厂做了。
	 */
	mux = ioremap(0x10e20000, 0x20);
	if (mux) {
		v = readl(mux + 0x0c);
		writel((v & ~0x9u) | 0x8u, mux + 0x0c);
		pr_info("zx279131-pon: pin_mux3 %#010x -> %#010x\n",
			v, readl(mux + 0x0c));
		iounmap(mux);
	}
}

u32 zx279131_pon_mac_init(void)
{
	size_t i;
	int n = 0;

	zx279131_pon_board_align();

	for (i = 0; i < ARRAY_SIZE(pon_mac_tbl); i++) {
		if (!pon_mac_rmw(pon_mac_tbl[i].addr, pon_mac_tbl[i].val,
				 pon_mac_tbl[i].mask))
			n++;
	}

	/*
	 * T-CONT 描述符。厂商 fi_Configure_Gem_Create_Tcont 写
	 *   idx 0x1650f / 0x16510  ->  0x1705943c / 0x17059440
	 *   值 = (alloc & 0x3fff) | 0x4000   （bit14 = valid）
	 * 原厂 O1 与 O5 快照都是 0x4009 / 0x4409（alloc 9；tcont1 另带 bit10），
	 * 我们这两个字一直是 0 —— 硬件不知道 alloc 9 归属哪个 T-CONT，
	 * 上行授权到了也落不到队列上。这里按原厂实测值补齐。
	 */
	pon_mac_rmw(0x1705943c, 0x00004009, 0xffffffff);
	pon_mac_rmw(0x17059440, 0x00004409, 0xffffffff);

	/*
	 * 0x17059844：bit0 = SN MIC 就绪（setSnMIC 置）、bit1 = Ack MIC 就绪
	 * （setAckMIC 置）。原厂 O1/O5 快照都是 3，我们一直只有 1。
	 */
	pon_mac_rmw(0x17059844, 0x00000003, 0x00000003);

	pr_info("zx279131-pon: mac init %d/%zu regs, xmac_ctrl=%#010x irq_en=%#010x gpon=%#010x tcont=%#010x/%#010x\n",
		n, ARRAY_SIZE(pon_mac_tbl),
		readl(pon_mac_map(0x17058400)),
		readl(pon_mac_map(0x1705800c)),
		readl(pon_mac_map(0x17059800)),
		readl(pon_mac_map(0x1705943c)),
		readl(pon_mac_map(0x17059440)));

	return readl(pon_mac_map(0x17058400));
}

/*
 * Burst profile for the PLOAMu (registration) burst.
 *
 * Taken from a live stock dump captured in O1: all four profile banks are
 * identical there, and both the +0x08 and the +0x10 word carry a copy of the
 * delimiter pattern.  In O5 the data profiles differ and those two words read
 * zero.  Writing the O5 values (what we used to do) leaves the registration
 * burst with 0xad4cc30f / 0x00000000 as its delimiter, so the OLT cannot lock
 * onto the burst header -- which matches the symptom exactly: the MAC looks
 * healthy locally and nothing ever registers.
 */
static void zx279131_pon_apply_reg_profile(void)
{
	/*
	 * 原厂 O5 实测四个 bank：
	 *   bank0/1 {00090404, aa, 0, delim, 0}   <- 由 OLT 的 Upstream_Overhead 实时写
	 *   bank2/3 {001f0408, aa, aa, delim, 0}  <- Set_Default_Profile 的静态默认档
	 * 这里复刻的是静态默认档（bank2/3）；+0x10 恒为 0，写 a56679e0 会让
	 * 突发尾部多出 4 字节，OLT 的突发接收器锁不上定界符。
	 */
	static const u32 prof[5] = {
		0x001f0408, 0xaaaaaaaa, 0xaaaaaaaa, 0xa56679e0, 0x00000000
	};
	int p, k;

	if (!pon_mac)
		return;

	for (p = 0; p < 4; p++)
		for (k = 0; k < 5; k++)
			writel(prof[k], pon_mac + 0x59880 + p * 0x14 + k * 4);
}

/*
 * Registration mode, mirroring fi_set_ONU_activation_state(1) plus
 * fi_Configure_Ponmac_Set_Upstream_Enable(0) as observed on a live unit:
 *   0x1705947c = 0x43ff0000  discovery window for Alloc-ID 0x3ff
 *   0x17059800 = 0x15        bit4 | bit0, bit1 clear  (PLOAMu only, no data)
 *   0x17059400 = 0x03ff0001  O1
 */
void zx279131_pon_enter_registration(void)
{
	void __iomem *c;
	u32 v;

	if (!pon_mac)
		return;

	zx279131_pon_apply_reg_profile();

	/* 原厂 O1 实测是 0x03ff0000（不是 0x43ff0000 —— bit30 是以前记错的） */
	writel(0x03ff0000, pon_mac + 0x5947c);

	c = pon_mac + 0x59800;
	v = readl(c);
	v |= BIT(4) | BIT(0);
	v &= ~(BIT(1) | BIT(3));
	writel(v, c);

	writel(0x03ff0001, pon_mac + 0x59400);

	pr_info("zx279131-pon: reg mode 5947c=%#010x 59800=%#010x 59400=%#010x\n",
		readl(pon_mac + 0x5947c), readl(c), readl(pon_mac + 0x59400));
}

/* Back to data mode: Set_Upstream_Enable(1) -> bit1 set / bit0 clear, and
 * drop the discovery-window bit. */
void zx279131_pon_enter_data(void)
{
	void __iomem *c;
	u32 v;

	if (!pon_mac)
		return;

	c = pon_mac + 0x59800;
	v = readl(c);
	v |= BIT(4) | BIT(1);
	v &= ~(BIT(0) | BIT(3));
	writel(v, c);

	v = readl(pon_mac + 0x5947c);
	writel(v & ~BIT(30), pon_mac + 0x5947c);

	pr_info("zx279131-pon: data mode 59800=%#010x 5947c=%#010x\n",
		readl(c), readl(pon_mac + 0x5947c));
}

/*
 * Faithful replay of fi_Configure_Hw_Init() as recovered from zte_xgpon.ko.
 * We never implemented this: the driver only loaded a 19-entry register
 * table, so every one of these one-shot hardware enables stayed at reset.
 * Kept as an explicit action so it can be tried on a running box without
 * risking the working RX path at probe time.
 *
 * Byte accesses below mirror the original (strb/ldrb on a stack word), i.e.
 * "byte0" is bits[7:0], "byte1" is bits[15:8], "byte2" is bits[23:16].
 */
/*
 * ★ 偏移有两套写法，别混：
 *   - 形如 0x18000 / 0x18400 / 0x1842c / 0x18004 的是「字下标 ×4」，
 *     必须落到 0x17058xxx，即 base(0x17000000) + 0x40000 + off。
 *     旧代码直接 base+off，写到了 0x17018xxx（那一带读出来是芯片 ID
 *     0x27913100），PON MAC 全局使能从来没被置过。
 *   - 形如 0x59800 / 0x59404 / 0x59008 的已经是绝对偏移。
 */
void zx279131_pon_run_hw_init(void)
{
	void __iomem *p;
	u32 v;

	if (!pon_mac)
		return;

#define PON_SETB(a, m)	do { p = pon_mac + (a); v = readl(p); writel(v | (m), p); } while (0)
#define PON_CLRB(a, m)	do { p = pon_mac + (a); v = readl(p); writel(v & ~(m), p); } while (0)

	PON_SETB(0x58000, 0x0000001f);	/* idx 0x6000  */
	/*
	 * 厂商这里 __const_udelay(0x418958) × 20 ≈ 20 ms。我们原来只等 2 ms：
	 * 实测 probe 阶段紧接着写 0x1705842c 会被吞掉（dmesg 的 hw_init replay
	 * 一直是 1842c=0），而开机 2 分钟后再跑同一条序列就正常留住 0x80000。
	 * 所以复位后必须等够，并且把这一步留到最后再补一次。
	 */
	msleep(20);

	PON_SETB(0x5842c, 0x00080000);	/* idx 0x610b  */

	/* fi_Configure_Ponmac_Set_Upstream_Enable(0) */
	p = pon_mac + 0x59800;
	v = readl(p);
	v &= ~BIT(1);
	writel(v, p);
	v = readl(p);
	writel(v | BIT(0), p);
	v = readl(p);
	writel(v & ~BIT(3), p);

	PON_SETB(0x58400, 0x00000020);	/* idx 0x6100, byte0 */
	PON_SETB(0x59800, 0x00000010);	/* idx 0x6600, byte0 */
	PON_SETB(0x5a27c, 0x00000001);	/* idx 0x689f, byte0 */
	PON_CLRB(0x00068, 0x0000000c);	/* idx 0x1a,   byte0: clear b2/b3 */
	PON_CLRB(0x00024, 0x00010000);	/* idx 0x9,    byte2: clear b16 */
	PON_SETB(0x58004, 0x00000001);	/* idx 0x6001  */

	if (pon_mac_i) {		/* idx 0x50040 / 0x500c0 */
		writel(1, pon_mac_i + 0x100);
		writel(1, pon_mac_i + 0x300);
	}

	PON_SETB(0x59404, 0x00010000);	/* idx 0x6501, byte2 |= 1 */
	PON_CLRB(0x59404, 0x00020000);	/*             byte2 &= ~2 */

	p = pon_mac + 0x59008;		/* idx 0x6402: bits[29:16] = 8 */
	v = readl(p);
	writel((v & 0xff00ffff) | (8u << 16), p);

	if (pon_mac_c)			/* idx 0x3000a */
		writel(readl(pon_mac_c + 0x28) | 1u, pon_mac_c + 0x28);

	PON_SETB(0x598d4, 0x00000001);	/* idx 0x6635 */

	/* idx 0x6644: byte1 = 0x1b, byte0 = 1 -> 0x00001b01 */
	p = pon_mac + 0x59910;
	v = readl(p);
	v = (v & ~0x0000ff00u) | (0x1bu << 8);
	v = (v & ~0x000000ffu) | 0x01u;
	writel(v, p);

	PON_SETB(0x59404, 0x00000004);	/* idx 0x6501, byte0 |= 4 */

	/*
	 * 厂商 Hw_Init 还有一步 GMAC：Hal_ReadGmacReg(0x1000) 后置 bit16 写回，
	 * 即 0x17044000。我们从来没配过 GMAC 窗口；用 zx_peek 读 0x17040000 区
	 * 返回的是 0xfc25fc25/0x3d5d3d5d 这类垃圾（原厂读 0x2），说明该窗口在
	 * 我们这边根本没有正常映射/上电。这里单独 ioremap 一次，至少把读数打出来。
	 */
	{
		void __iomem *g = ioremap(0x17040000, 0x40000);

		if (g) {
			u32 g0 = readl(g + 0x4000);

			pr_info("zx279131-pon: gmac 17044000 before=%#010x\n", g0);
			writel(g0 | 0x00010000, g + 0x4000);
			pr_info("zx279131-pon: gmac 17044000 after=%#010x\n",
				readl(g + 0x4000));
			iounmap(g);
		} else {
			pr_warn("zx279131-pon: gmac ioremap failed\n");
		}
	}

	/* 这两张表不能在这里铺：原厂 O1 快照是 0xffffffff，只有到 O5
	 * 才是 0x04444444/0x09999999。早早在 O1 铺会破坏注册前的状态。
	 * 现在只在 O5（或手动 echo 1 > pon_allocmap）时铺。 */

	/*
	 * 复位后这一位必须最后再补一次：probe 阶段紧跟复位的写入会被吞
	 * （replay 打印 1842c=0），等整个序列跑完再写就稳定留住 0x80000。
	 */
	PON_SETB(0x5842c, 0x00080000);
	pr_info("zx279131-pon: hwinit tail 1842c=%#010x 18400=%#010x\n",
		readl(pon_mac + 0x5842c), readl(pon_mac + 0x58400));

#undef PON_SETB
#undef PON_CLRB

	pr_info("zx279131-pon: hw_init replay 18000=%#010x 18004=%#010x 18400=%#010x 1842c=%#010x 59404=%#010x 59008=%#010x 59910=%#010x\n",
		readl(pon_mac + 0x58000), readl(pon_mac + 0x58004),
		readl(pon_mac + 0x58400), readl(pon_mac + 0x5842c),
		readl(pon_mac + 0x59404), readl(pon_mac + 0x59008),
		readl(pon_mac + 0x59910));
}

/* Called from the packet-engine probe once, before the interface goes up. */
void zx279131_pon_init(void)
{
	u32 st;

	pon_crm = ioremap(PON_TOP_CRM_BASE, PON_BLOCK_SIZE);
	pon_ser = ioremap(PON_SERDES_BASE, PON_BLOCK_SIZE);
	pon_sys = ioremap(PON_SYSCTRL_BASE, PON_BLOCK_SIZE);
	pon_mac = ioremap(PON_MAC_BASE, 0x60000);
	pon_mac_c = ioremap(0x170c0000, PON_BLOCK_SIZE);
	pon_mac_i = ioremap(0x17140000, PON_BLOCK_SIZE);
	pon_mac_win4 = ioremap(PON_MAC_WIN4_BASE, PON_MAC_WIN4_SIZE);
	if (!pon_crm || !pon_ser || !pon_sys) {
		pr_err("zx279131-pon: ioremap failed\n");
		return;
	}

	st = zx279131_pon_serdes_init();
	pr_info("zx279131-pon: serdes status %#010x (rx_ready=%u pll_lock=%u)\n",
		st, !!(st & BIT(4)), !!(st & BIT(10)));

	/*
	 * ★ 厂商 fi_Configure_Hw_Init 的第一步：往 0x17058000 写 bit0-4。
	 * 这是 MAC 子块的复位（write-1，硬件自己清零 —— 写完读回是 0）。
	 * 以前只在 `echo step hwinit > pon_gpon` 里跑，probe 从不执行，于是
	 * MAC 的 TX 子块一直停在复位态：FIFO(0x17059b08) 写得进、0x17059908
	 * 写 1 永不清零、0x17059a58/5c 恒 0 —— 一条 burst 都没发出去过。
	 * 必须在 mac_init 之前跑，让它随后重新配置。
	 */
	zx279131_pon_run_hw_init();

	zx279131_pon_mac_init();

	zx279131_pon_apply_reg_profile();

	/*
	 * Registration ID.  Stock has it in the MAC from power-on (O1),
	 * so it is local configuration, not something the OLT grants.
	 * 0x17059854..5c is the ASCII "f663702212"; 4c/50 is the
	 * encoded form stock computes from it.
	 */
	if (pon_mac) {
		writel(0x4342f052, pon_mac + 0x5984c);
		writel(0xac5dca31, pon_mac + 0x59850);
		writel(0x66363633, pon_mac + 0x59854);
		writel(0x37303232, pon_mac + 0x59858);
		writel(0x31320000, pon_mac + 0x5985c);
	}

	/*
	 * 这两张表原厂 O1 是 0xffffffff，O5 才铺成 0x04444444/0x09999999。
	 * probe 阶段就铺会破坏注册前的 MAC 状态（v9.46/v9.47 的错误），
	 * 所以这里不再自动铺；到 O5 再铺，或手动 echo 1 > pon_allocmap。
	 */

	zx279131_pon_irq_setup();
	gpon_init_work();
}

static ssize_t pon_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	u32 st = pon_init_done ? readl(pon_ser + SERDES_STATUS) : 0;
	u32 s60 = pon_init_done ? readl(pon_ser + 0x060) : 0;
	u32 s70 = pon_init_done ? readl(pon_ser + 0x070) : 0;
	u32 wm = pon_mac ? readl(pon_mac + MAC_WORK_MODE) : 0;
	u32 xc = pon_mac_map(0x17058400) ? readl(pon_mac_map(0x17058400)) : 0;
	u32 ie = pon_mac_map(0x1705800c) ? readl(pon_mac_map(0x1705800c)) : 0;
	u32 is = pon_mac_map(0x17058010) ? readl(pon_mac_map(0x17058010)) : 0;
	u32 up = pon_mac_map(0x17059800) ? readl(pon_mac_map(0x17059800)) : 0;
	u32 tx = pon_mac_map(0x17059908) ? readl(pon_mac_map(0x17059908)) : 0;
	u32 g0 = pon_mac_map(0x17059a58) ? readl(pon_mac_map(0x17059a58)) : 0;
	u32 g1 = pon_mac_map(0x17059a5c) ? readl(pon_mac_map(0x17059a5c)) : 0;
	u32 bp = pon_mac_map(0x17059980) ? readl(pon_mac_map(0x17059980)) : 0;

	return sysfs_emit(buf,
			  "status=%#010x rx_ready=%u pll_lock=%u sig_detect=%u\n"
			  "work_mode=%#x\n"
			  "0x060=%#010x 0x070=%#010x\n"
			  "xmac=0x17058400=%#010x rx_en=%u up=0x17059800=%#010x\n"
			  "txstat=0x17059908=%#010x burst=0x17059980=%#010x\n"
			  "grant58=%#010x grant5c=%#010x\n"
			  "irq_en=0x1705800c=%#010x irq_st=0x17058010=%#010x\n"
			  "irq=%d count=%lu ev=%#010x ev_or=%#010x storm=%u ploam=%u\n"
			  "olt_reg=%#04x\n"
			  "grant=%#04x\n"
			  "seq=%u\n",
			  st, !!(st & BIT(4)), !!(st & BIT(10)), !!(st & BIT(8)),
			  wm, s60, s70,
			  xc, !!(xc & BIT(0)), up,
			  tx, bp, g0, g1,
			  ie, is,
			  pon_irq, pon_irq_count, pon_irq_ev, pon_irq_ev_or,
			  pon_irq_storm, pon_ploam_n,
			  pon_regflag, pon_grant, pon_seq);
}

/* 最近一条 PLOAM 消息（48 字节，寄存器原始字节序已翻正） */
static ssize_t pon_ploam_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int i, n = 0;

	n += sysfs_emit(buf, "count=%u frame_cnt=%#010x\n", pon_ploam_n, pon_frame_cnt);
	for (i = 0; i < PLOAM_LEN; i++)
		n += sysfs_emit_at(buf, n, "%02x%s", pon_ploam[i],
				   (i % 16 == 15) ? "\n" : " ");
	return n;
}
static DEVICE_ATTR_RO(pon_ploam);

/* 最近 8 条，按时间顺序（老的在前） */
static ssize_t pon_ploam_all_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int i, j, n = 0;

	n += sysfs_emit(buf, "count=%u\n", pon_ploam_n);
	n += sysfs_emit(buf, "history seq/regflag/grant:");
	for (i = 0; i < PLOAM_REG_HIST; i++) {
		unsigned int k = (pon_reg_h_idx + i) % PLOAM_REG_HIST;

		n += sysfs_emit_at(buf, n, " %02x/%02x/%02x",
				   pon_seq_h[k], pon_regflag_h[k],
				   pon_grant_h[k]);
	}
	n += sysfs_emit_at(buf, n, "\n");
	for (i = 0; i < PLOAM_RING; i++) {
		unsigned int k = (pon_ploam_ring_idx + i) % PLOAM_RING;
		const u8 *m = pon_ploam_ring[k];

		n += sysfs_emit_at(buf, n, "#%d ", i);
		for (j = 0; j < PLOAM_LEN; j++)
			n += sysfs_emit_at(buf, n, "%02x", m[j]);
		n += sysfs_emit_at(buf, n, "   seq=%02x regflag=%02x grant=%02x\n",
				   m[3], m[15], m[16]);
	}
	return n;
}
static DEVICE_ATTR_RO(pon_ploam_all);

/*
 * Passive sampling of the MAC interrupt status words.  This only reads; it
 * never registers the IRQ and never writes the status back.  Its purpose is
 * to answer one question without touching the hardware state: does the GPON
 * MAC generate events at all on this board?
 */
static ssize_t pon_irq_sample_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	int i, n0 = 0, n1 = 0, ninfo = 0;
	u32 or0 = 0, or1 = 0, last0 = 0, last1 = 0, info = 0;

	if (!pon_mac_win4)
		return sysfs_emit(buf, "window 4 not mapped\n");

	for (i = 0; i < 40; i++) {
		u32 s0 = readl(pon_mac_win4 + MAC_IRQ_STAT0);
		u32 s1 = readl(pon_mac_win4 + MAC_IRQ_STAT1);

		last0 = s0;
		last1 = s1;
		if (s0) {
			n0++;
			or0 |= s0;
		}
		if (s1) {
			n1++;
			or1 |= s1;
		}
		if (s0 || s1) {
			info = readl(pon_mac_win4 + MAC_IRQ_INFO);
			ninfo++;
		}
		msleep(50);
	}

	return sysfs_emit(buf,
			  "samples=40\n"
			  "stat0(0x%04x): nonzero=%d or=%#010x last=%#010x\n"
			  "stat1(0x%04x): nonzero=%d or=%#010x last=%#010x\n"
			  "info (0x%04x): sampled=%d last=%#010x\n",
			  MAC_IRQ_STAT0, n0, or0, last0,
			  MAC_IRQ_STAT1, n1, or1, last1,
			  MAC_IRQ_INFO, ninfo, info);
}
static DEVICE_ATTR_RO(pon_irq_sample);

static ssize_t pon_irq_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
			  "irq=%d count=%lu en=%#010x st=%#010x ev=%#010x ev_or=%#010x storm=%u\n",
			  pon_irq, pon_irq_count, pon_irq_s0, pon_irq_s1,
			  pon_irq_ev, pon_irq_ev_or, pon_irq_storm);
}
static DEVICE_ATTR_RO(pon_irq);

static ssize_t pon_reinit_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	u32 st = zx279131_pon_serdes_init();

	dev_info(dev, "reinit -> %#010x (rx_ready=%u)\n", st, !!(st & BIT(4)));
	return count;
}

/* ======================================================================
 * GPON activation: the O2 -> O3 serial-number handshake
 *
 * Reverse-engineered from the vendor zte_xgpon.ko (see PON-PLAN §76/§77):
 *
 *   Set_Sn       0x170598d8/0x170598dc  SN, big-endian, *not* swab'd
 *   setSnMIC     -> 0x170598f8/0x170598fc  AES-CMAC of the 40-byte body
 *   OnuState     0x17059400 bits[4:0]   O1/O2/O3/O4/O5 activation state
 *   Send_Sr      0x170599bc  b0 SN-tx enable, b2:1 onu_id, b3 trigger
 *
 * The hardware only transmits in **O2**.  In O3 the trigger bit never
 * clears: that is what kept the hand-rolled pokes stuck for hours.
 *
 * Frame the hardware builds for us (48 B = 40 body + 8 MIC):
 *   [0]=03 [1]=ff [2]=01(Serial_Number) [3]=00
 *   [4..11]=SN  [12..15]=random delay  [16..39]=0  [40..47]=MIC
 * MIC = AES-CMAC(key = 0x55 x 16, 0x02 || body[0..39]) then one bit in the
 * MIC is flipped according to the zero-bit parity of the body.
 * ====================================================================== */
#define XMAC_ONU_STATE		0x17059400
#define XMAC_SN_LO		0x170598d8
#define XMAC_SN_HI		0x170598dc
#define XMAC_SN_MIC_LO		0x170598f8
#define XMAC_SN_MIC_HI		0x170598fc
#define XMAC_SN_CTRL		0x170599bc

#define GPON_ST_O1		1
#define GPON_ST_O2		2
#define GPON_ST_O3		3
#define GPON_ST_O5		5
#define GPON_ONU_ID		0	/* 未注册时 SN 突发的 ONU-ID 字段应为 0 */
#define GPON_STATE_MASK		0x1f
#define GPON_O2_SETTLE_MS	80	/* O2 needs this long before firing */
#define GPON_O3_SETTLE_MS	40

#define GPON_MSGID_MAX		16

/* Stock F7610U O5 live values (work/o5_regs.json).  SKYW + a8d40ba7 is the
 * SN the OLT actually accepted; ZTEGD09D561 from mtd12 is the box serial,
 * not the 8-byte GPON SN. */
static u8 gpon_sn[8] = { 'S', 'K', 'Y', 'W', 0xa8, 0xd4, 0x0b, 0xa7 };
static u8 gpon_pw[10] = { 'f', '6', '6', '3', '7', '0', '2', '2', '1', '2' };

static DEFINE_MUTEX(gpon_lock);

struct gpon_stat {
	bool	armed;		/* user allowed us to transmit */
	bool	auto_mic;	/* compute MIC in kernel */
	u32	period_ms;
	u32	remaining;	/* 0 = run until stopped */
	u32	tries;
	u32	tx_ok;		/* trigger bit cleared => hardware sent */
	u32	tx_timeout;
	u32	msgid_hist[GPON_MSGID_MAX];
	u32	msgid_other;
	u8	last_msgid;
	u32	last_state;
	u32	last_ctrl;
	int	last_mic_err;
	u32	rt_msg;		/* Ranging_Time (msgid 04) received */
	u32	ack_tx;		/* upstream ACK (msgid 09) actually queued */
	u32	o5_entered;	/* successful O4 -> O5 transitions */
};

static struct gpon_stat gpon;
static struct delayed_work gpon_work;
static struct work_struct gpon_sn_work;	/* fires on every Profile PLOAMd */
static struct delayed_work gpon_sn_timer;	/* O1->O3 SN replay, 2 s period */
static unsigned long gpon_sn_last;
static unsigned long sn_burst_last;
static unsigned int sn_burst_try;
static unsigned int sn_burst_ok;
static unsigned int sn_rounds;
static struct crypto_shash *gpon_tfm;		/* "cmac(aes)", allocated at probe */
static struct workqueue_struct *gpon_wq;

static void gpon_sn_auto(struct work_struct *w);

static void __iomem *pon_mac_w(u32 addr);

/*
 * fi_Configure_Ponmac_Set_Profile(), decoded from the vendor module and
 * checked against a live capture of the OLT's Upstream_Overhead message
 * (msgid 0x01, see work/pon_ploam_all):
 *
 *   m[4]      profile bank index (0..3)
 *   m[6]      delimiter length in bytes
 *   m[7..14]  delimiter bit pattern
 *   m[15]     preamble length in bytes
 *   m[16]     bits[23:16] of the burst control word
 *   m[17..24] preamble bit pattern
 *
 * The OLT alternates between two patterns every 0.5 s (0xad4cc30f / 0xa56679e0
 * with preamble 0xaaaaaaaa), so stock rewrites the bank on *every* message.
 * We used to freeze the reset default (0x00190808 / 0xa56679e0) at probe time,
 * which means at best every second burst carried a delimiter the OLT was not
 * expecting.
 */
static void gpon_apply_profile_msg(const u8 *m)
{
	void __iomem *p;
	u32 w[5], mask;
	int idx = m[4] & 3;
	int i;

	if (!pon_mac)
		return;

	w[0] = ((u32)(m[16] & 0x1f) << 16) |
	       ((u32)(m[6] & 0xf) << 8) | (u32)(m[15] & 0xf);
	for (i = 0; i < 2; i++) {
		w[1 + i] = ((u32)m[17 + i * 4] << 24) |
			   ((u32)m[18 + i * 4] << 16) |
			   ((u32)m[19 + i * 4] << 8) |
			   (u32)m[20 + i * 4];
		w[3 + i] = ((u32)m[7 + i * 4] << 24) |
			   ((u32)m[8 + i * 4] << 16) |
			   ((u32)m[9 + i * 4] << 8) |
			   (u32)m[10 + i * 4];
	}

	p = pon_mac + 0x59880 + idx * 0x14;
	/*
	 * 反汇编 fi_Configure_Ponmac_Set_Profile（ko 0x48b0..0x4b18）：
	 *   ctl      = (m[16]&0x1f)<<16 | (m[6]&0xf)<<8 | (m[15]&0xf)
	 *   plen     = m[15] & 0xf   preamble 字节数
	 *   dlen     = m[6]  & 0xf   delimiter 字节数
	 *   memcpy(buf+0, m+17, plen);  memcpy(buf+8, m+7, dlen);
	 *   缓冲区先 stp xzr 清零 ⇒ 长度不足 8 时第二字必须是 0，不能照抄 m[21..24]。
	 *   base+0x00=ctl  +0x04/+0x08=preamble  +0x0c/+0x10=delimiter（都按大端落字）。
	 *
	 * 实测 OLT 报文：m[15]=8、m[6]=4、m[17..24]=AA×8、m[7..10]=A5 66 79 E0
	 * ⇒ 该档应写 {ctl, aaaaaaaa, aaaaaaaa, a56679e0, 0}；而 4 字节档
	 *   {ctl, aaaaaaaa, 0, delim, 0}，正是原厂 bank0/bank1 的形态。
	 */
	{
		u32 pre[2] = { 0, 0 }, del[2] = { 0, 0 };
		u8 *pb = (u8 *)pre, *db = (u8 *)del;
		int plen = m[15] & 0xf, dlen = m[6] & 0xf;

		for (i = 0; i < plen && i < 8; i++)
			pb[i] = m[17 + i];
		for (i = 0; i < dlen && i < 8; i++)
			db[i] = m[7 + i];

		writel(w[0], p);
		writel(be32_to_cpu(pre[0]), p + 4);
		writel(be32_to_cpu(pre[1]), p + 8);
		writel(be32_to_cpu(del[0]), p + 0xc);
		writel(be32_to_cpu(del[1]), p + 0x10);
	}

	/* 0x17059428: bit 2*p marks the bank live, bit 2*p+1 mirrors m[5]. */
	mask = 1u << (idx * 2);
	if (m[5] & 1)
		mask |= 1u << (idx * 2 + 1);
	p = pon_mac_w(0x17059428);
	if (p)
		writel(readl(p) | mask, p);
}

/*
 * fi_Configure_Ponmac_Send_Ploam_Msg(): 48 bytes go into the PLOAMu FIFO at
 * 0x17059b08 as big-endian words, then bit0 of 0x17059908 starts the burst.
 */
static void gpon_fifo_send(const u8 frame[48])
{
	void __iomem *fifo = pon_mac_w(0x17059b08);
	void __iomem *ctl = pon_mac_w(0x17059908);
	int i;

	if (!fifo || !ctl)
		return;
	for (i = 0; i < 12; i++)
		writel(((u32)frame[i * 4] << 24) |
		       ((u32)frame[i * 4 + 1] << 16) |
		       ((u32)frame[i * 4 + 2] << 8) |
		       (u32)frame[i * 4 + 3], fifo + i * 4);
	writel(1, ctl);
}

/*
 * fi_Configure_Ponmac_Set_Eqd() (zte_xgpon.ko @0x5b70), instruction for
 * instruction (PON-PLAN §96.6 / §101).
 *
 * The vendor's "mode 1" branch (@0x5c48) is the one that runs while the ONU
 * is ranging, and it is a straight fixed-point transform of the raw
 * equalisation delay the OLT put in the message:
 *
 *   w0  = E << 2
 *   w5  = w0 >> 3
 *   x2  = (w0 * 0x1af83441) >> 49
 *   x3  = (w5 * 0x1af83441) >> 46
 *   low = w5 - 0x25f80 * x3
 *   out = low | (x2 & 0x1f) << 18 | ((E & 1) << 2) << 23
 *
 * Verified: E = 0x00082c03 (the Ranging_Time the OLT actually sent us,
 * PON-PLAN §96.5) encodes to 0x0205b681, which is the value §96.6 already
 * recorded from the vendor path.
 *
 * flags (the message's m[4]) bit0 = 1 means "replace with the value from
 * the message"; bit0 = 0 means "add/subtract it to/from the current EqD"
 * (bit1 selects which).
 */
static u32 gpon_encode_eqd(u32 eqd, u32 flags, u32 cur)
{
	u64 x2, x3;
	u32 low;

	if (!(flags & 1)) {
		if (flags & 2)
			cur = cur - eqd;
		else
			cur = cur + eqd;
		eqd = cur;
	}

	x2 = ((u64)(eqd << 2) * 0x1af83441ULL) >> 49;
	x3 = ((u64)((eqd << 2) >> 3) * 0x1af83441ULL) >> 46;
	low = (u32)(((eqd << 2) >> 3) - 0x25f80u * (u32)x3);

	return (low & 0x3ffff) |
	       ((u32)(x2 & 0x1f) << 18) |
	       (((eqd & 1) << 2) << 23);
}

/*
 * send_ack_msg() (zte_xgpon.ko @0xd838) - the upstream PLOAM ACK the vendor
 * sends after every Ranging_Time.  PON-PLAN §101.2.
 *
 *   buf[0..1] = the 16 bit field the vendor keeps at pon_base+0x264b.  In
 *                every live capture that is the destination ONU-ID in
 *                big endian order; for the ACK we echo our own ONU-ID.
 *   buf[2]    = 0x09  (the ACK message type)
 *   buf[3]    = the m[3] byte of the message we are answering, echoed back
 *   buf[4]    = 0
 *   buf[5..39]= 0
 *   buf[40..47]= AES-CMAC(key = 0x55 x 16, 0x02 || buf[0..39])
 *
 * The vendor uses Drv_PloamuMic_Generate_Activation() here (no parity bit
 * flip), and it feeds the frame straight into
 * fi_Configure_Ponmac_Send_Ploam_Msg() - the same software FIFO path we
 * already have in gpon_fifo_send().
 */
static void gpon_send_ack(u8 echo_byte, u16 onu_id)
{
	u8 frame[48], key[16], msg[41], mac[16];
	struct shash_desc *desc;
	int ret;

	if (!gpon_tfm)
		return;

	memset(frame, 0, sizeof(frame));
	frame[0] = (u8)(onu_id >> 8);
	frame[1] = (u8)(onu_id & 0xff);
	frame[2] = 0x09;		/* ACK */
	frame[3] = echo_byte;

	memset(key, 0x55, sizeof(key));
	msg[0] = 0x02;
	memcpy(msg + 1, frame, 40);

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(gpon_tfm),
		       GFP_KERNEL);
	if (!desc)
		return;
	desc->tfm = gpon_tfm;
	ret = crypto_shash_setkey(gpon_tfm, key, sizeof(key));
	if (!ret)
		ret = crypto_shash_digest(desc, msg, sizeof(msg), mac);
	kfree(desc);
	if (ret)
		return;

	memcpy(frame + 40, mac, 8);

	gpon_fifo_send(frame);
	gpon.ack_tx++;
	pr_info("zx279131-pon: ploam ack sent (echo=%#04x id=%u)\n",
		echo_byte, onu_id);
}

/* Whole 48-byte Serial_Number frame: 40-byte body + 8-byte MIC. */
static int gpon_build_sn_frame(u8 frame[48])
{
	u8 key[16], msg[41], mac[16];
	struct shash_desc *desc;
	void __iomem *pd = pon_mac_w(0x1705940c);
	u32 delay = 0;
	int ret;

	if (!gpon_tfm)
		return -ENOENT;

	memset(frame, 0, 48);
	frame[0] = 0x03;
	frame[1] = 0xff;
	frame[2] = 0x01;		/* msgid: Serial_Number_ONU */
	frame[3] = 0x00;
	memcpy(frame + 4, gpon_sn, 8);
	if (pd) {
		delay = readl(pd);
		frame[12] = delay >> 24;
		frame[13] = delay >> 16;
		frame[14] = delay >> 8;
		frame[15] = delay;
	}

	memset(key, 0x55, sizeof(key));
	msg[0] = 0x02;
	memcpy(msg + 1, frame, 40);

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(gpon_tfm),
		       GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->tfm = gpon_tfm;
	ret = crypto_shash_setkey(gpon_tfm, key, sizeof(key));
	if (!ret)
		ret = crypto_shash_digest(desc, msg, sizeof(msg), mac);
	kfree(desc);
	if (ret)
		return ret;
	memcpy(frame + 40, mac, 8);
	return 0;
}

/*
 * p_o4_ranging_time_msg() (zte_xgpon.ko @0xf488) - PON-PLAN §101.1.
 *
 * The OLT sends msgid 0x04 (Ranging_Time) with the equalisation delay in
 * m[5..8] (big endian) and the flags in m[4].  The vendor answers with
 *   1. fi_Configure_Ponmac_Set_Eqd()  -> 0x17059410
 *   2. send_ack_msg()                 -> the upstream ACK (msgid 0x09)
 *   3. fi_set_ONU_activation_state(5) -> O5
 * We skipped steps 2 and 3 for hours, which is why the OLT always went
 * silent ~70 s after the Ranging_Time.
 *
 * The ACK needs an AES-CMAC, and crypto_shash must not run in hard IRQ
 * context, so the message is copied out and the real work happens in a
 * workqueue (same trick the SN path uses).
 */
static struct work_struct gpon_rt_work;
static u8 gpon_rt_msg[48];
static bool gpon_rt_pending;

static void gpon_rt_worker(struct work_struct *w)
{
	void __iomem *st;
	void __iomem *eqd_reg;
	u32 state, cur_eqd, enc;
	u16 dest_id, onu_id;
	u8 flags, echo;
	const u8 *m = gpon_rt_msg;

	eqd_reg = pon_mac_w(0x17059410);
	st = pon_mac_w(XMAC_ONU_STATE);
	if (!st || !eqd_reg)
		return;

	state = readl(st) & GPON_STATE_MASK;
	if (state != 4)
		return;		/* the vendor only reacts while in O4 */

	/* m[0..1] = destination ONU-ID, big endian.  0x3ff = broadcast. */
	dest_id = (u16)(((u16)m[0] << 8) | m[1]);
	if (dest_id == 0x3ff)
		return;

	flags = m[4];
	cur_eqd = readl(eqd_reg);
	enc = gpon_encode_eqd(((u32)m[5] << 24) | ((u32)m[6] << 16) |
			      ((u32)m[7] << 8) | m[8], flags, cur_eqd);
	writel(enc, eqd_reg);

	/*
	 * Our own ONU-ID lives in bits[25:16] of the state word; the vendor
	 * keeps it in a separate database field.  Use the same source.
	 */
	onu_id = (u16)((readl(st) >> 16) & 0x3ff);
	echo = m[3];

	gpon_send_ack(echo, onu_id);

	pr_info("zx279131-pon: ranging time dest=%u flags=%#04x eqd=%#010x -> enc=%#010x state=%u\n",
		dest_id, flags, ((u32)m[5] << 24) | ((u32)m[6] << 16) |
		((u32)m[7] << 8) | m[8], enc, state);

	/*
	 * Step 3: move to O5.  The vendor's fi_set_ONU_activation_state(5)
	 * also pokes 0x17000024 bit16 (XMAC 0x24) - PON-PLAN §96.2.
	 */
	writel(((u32)onu_id << 16) | 5, st);
	if (pon_mac_w(0x17000024)) {
		void __iomem *p24 = pon_mac_w(0x17000024);

		writel(readl(p24) | BIT(16), p24);
	}
	gpon.o5_entered++;
	pr_info("zx279131-pon: activation state -> O5 (id=%u)\n", onu_id);
}

/* Queue the Ranging_Time handling; called from the PLOAM IRQ handler. */
static void gpon_queue_ranging_time(const u8 *m)
{
	gpon.rt_msg++;
	memcpy(gpon_rt_msg, m, sizeof(gpon_rt_msg));
	gpon_rt_pending = true;
	schedule_work(&gpon_rt_work);
}

static void gpon_note_msg(const u8 *m)
{
	u8 id = m[2];

	if (id < GPON_MSGID_MAX)
		gpon.msgid_hist[id]++;
	else
		gpon.msgid_other++;
	gpon.last_msgid = id;

	/*
	 * Stock calls setSnMIC() from p_o2_3_profile_msg, i.e. once per
	 * Profile: rebuild the frame MIC from the current random delay and
	 * kick 0x17059844.  Doing the same here is the closest we can get
	 * to the vendor's timing.  Done in a work item because CMAC must
	 * not be computed in hardirq context.
	 */
	if (id == 0x01 && time_after(jiffies, gpon_sn_last + HZ / 2)) {
		gpon_sn_last = jiffies;
		gpon_apply_profile_msg(m);
		schedule_work(&gpon_sn_work);
		return;
	}

	/* Ranging_Time: answer it, otherwise the OLT deactivates us. */
	if (id == 0x04)
		gpon_queue_ranging_time(m);
}

static void __iomem *pon_mac_w(u32 addr)
{
	return pon_mac_map(addr);
}

static void gpon_write_be(u32 addr, const u8 *b)
{
	__be32 v;

	memcpy(&v, b, 4);
	writel(be32_to_cpu(v), pon_mac_w(addr));
}

/*
 * MIC = CMAC(AES, key = 0x55 x 16, 0x02 || body[40]) with one bit flipped
 * by the zero-bit parity of the body.  Both quirks come straight from
 * Drv_PloamuMic_fake in zte_xgpon.ko.
 */
static int gpon_compute_mic(const u8 *sn, u8 out[8])
{
	struct shash_desc *desc;
	u8 key[16], body[40], msg[41], mac[16];
	void __iomem *pd = pon_mac_w(0x1705940c);
	u32 delay;
	int ret;

	memset(key, 0x55, sizeof(key));
	memset(body, 0, sizeof(body));
	body[0] = 0x03;
	body[1] = 0xff;
	body[2] = 0x01;
	body[3] = 0x00;
	memcpy(body + 4, sn, 8);
	/*
	 * body[12..15] must carry the random delay the MAC puts into the frame
	 * it assembles by itself.  fi_Configure_Ponmac_Set_Random_Delay mirrors
	 * that value into 0x1705940c; reading it here is what makes our MIC
	 * match the one stock writes to 0x170598f8/fc (verified byte for byte
	 * against a live O5 unit).
	 */
	if (pd) {
		delay = readl(pd);
		body[12] = delay >> 24;
		body[13] = delay >> 16;
		body[14] = delay >> 8;
		body[15] = delay;
	}

	msg[0] = 0x02;
	memcpy(msg + 1, body, sizeof(body));

	/* The tfm is allocated once at probe time.  Allocating it from a
	 * work item would hit request_module() and wedge the workqueue. */
	if (!gpon_tfm)
		return -ENOENT;

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(gpon_tfm),
		       GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->tfm = gpon_tfm;

	ret = crypto_shash_setkey(gpon_tfm, key, sizeof(key));
	if (!ret)
		ret = crypto_shash_digest(desc, msg, sizeof(msg), mac);

	kfree(desc);
	/*
	 * cmac(aes) always emits 16 bytes.  Callers only have room for the
	 * truncated 8-byte MIC that goes to 0x170598f8/fc, so copy it here
	 * instead of letting the digest spill over the caller's stack frame
	 * (that overflow panicked the board inside gpon_sn_auto).
	 */
	if (!ret)
		memcpy(out, mac, 8);
	/* Stock keeps the raw CMAC — the zero-bit parity flip belongs to the
	 * GPON path (Drv_PloamuMic_fake), not to XG-PON.  Verified: live O5
	 * 0x170598f8/fc == plain CMAC of the 41-byte message. */
	return ret;
}

/* Kick off an upstream SN burst: stock does read-modify-write of bit0 on
 * 0x17059844 (XMAC 0x6611) at the end of every setSnMIC(). */
static void gpon_sn_fire(void)
{
	void __iomem *p = pon_mac_w(0x17059844);
	void __iomem *h = pon_mac_w(0x170598f4);
	u32 v;

	if (!p)
		return;

	/* setSnMIC() stamps 0x03 into the top byte of the header word when the
	 * MAC is in mode 1 (O1) and 0x00 while ranging.  We always left it 0. */
	if (h)
		writel((readl(h) & 0x00ffffff) | 0x03000000, h);

	v = readl(p);
	writel(v | 1u, p);
	pr_info("zx279131-pon: sn fire %#010x -> %#010x (hdr %#010x)\n",
		v, readl(p), h ? readl(h) : 0);
}

/*
 * ★★ 2026-10-19 深夜实测定论（PON-PLAN §91）：
 *   0x17059908 bit0 是 PLOAMu FIFO 的“请求未被消费”标志。在 O3 原地反复
 *   写 1 会被硬件直接拒绝（写完立刻弹回 1，看起来像“永久卡死”）；
 *   正确顺序是
 *       O1 → 清 0x17059908 → 逐字灌 12 字到 0x17059b08 → 切 O3
 *   这时硬件才把 FIFO 锁存进突发引擎，0x17059908 会保持 0。
 *   实测 5 轮全部受理；OLT 仍只回 01，所以改成驱动里周期重放。
 */
static void gpon_fifo_burst_o1(const u8 frame[48])
{
	void __iomem *st = pon_mac_w(XMAC_ONU_STATE);
	void __iomem *txs = pon_mac_w(XMAC_UP_TX_STAT);
	void __iomem *fifo = pon_mac_w(0x17059b08);
	u32 v;
	int i, consumed = 0;

	if (!st || !txs || !fifo)
		return;

	/* 1) 回 O1：硬件只在这个状态下接受装帧 */
	v = readl(st);
	writel((v & ~GPON_STATE_MASK) | GPON_ST_O1, st);
	udelay(300);

	/* 2) 清掉上次残留的请求位 */
	writel(0, txs);

	/* 3) 12 个大端字灌进 PLOAMu FIFO */
	for (i = 0; i < 12; i++)
		writel(((u32)frame[i * 4] << 24) |
		       ((u32)frame[i * 4 + 1] << 16) |
		       ((u32)frame[i * 4 + 2] << 8) |
		       (u32)frame[i * 4 + 3], fifo + i * 4);

	/* 4) 切 O3：硬件在这里锁存 FIFO 并起突发 */
	writel((readl(st) & ~GPON_STATE_MASK) | GPON_ST_O3, st);

	/* 5) 20 ms 内看请求位有没有被消费 */
	for (i = 0; i < 50; i++) {
		if (!(readl(txs) & 1)) {
			consumed = 1;
			break;
		}
		usleep_range(200, 400);
	}

	sn_burst_last = jiffies;
	sn_burst_try++;
	if (consumed)
		sn_burst_ok++;
	if (!(sn_burst_try % 10))
		pr_info("zx279131-pon: sn burst #%u consumed=%d txs=%#010x state=%#010x\n",
			sn_burst_try, consumed, readl(txs), readl(st));
}

static void gpon_sn_auto(struct work_struct *w)
{
	u8 frame[48];
	void __iomem *st;

	if (!pon_mac || !gpon_tfm)
		return;

	/*
	 * v9.61：只在 O1/O2_3 武装 SN 引擎。厂商 p_o4_profile_msg /
	 * p_o5_profile_msg 只 apply profile、从不调 setSnMIC；在 O4/O5 继续
	 * 重武装会把 header 反复写回 0x03（PON-PLAN §95.5：mode>3 时 header
	 * 字节应为 0），并覆盖手工 O4/O5 寄存器控制。
	 */
	st = pon_mac_w(XMAC_ONU_STATE);
	if (st) {
		u32 stv = readl(st) & GPON_STATE_MASK;

		if (stv != GPON_ST_O1 && stv != GPON_ST_O3)
			return;
	}

	/* 2 秒一轮，别把状态机刷得太凶 */
	if (sn_burst_last && time_before(jiffies, sn_burst_last + 2 * HZ))
		return;

	if (gpon_build_sn_frame(frame))
		return;

	/*
	 * ★ v9.60：照抄厂商 setSnMIC()（zte_xgpon.ko 0xe058，由
	 * p_o2_3_profile_msg 每收到一个 Profile 消息调一次）的完整序列：
	 *   1) SN     -> 0x170598d8/dc（大端）
	 *   2) MIC    -> 0x170598f8/fc = CMAC(0x55*16, 02||body)
	 *   3) header -> 0x170598f4 顶字节打 0x03（mode 1 时）
	 *   4) 0x17059844 bit0 = "SN MIC 就绪"，硬件 SN 突发引擎的武装位
	 * 之前三个版本都漏了 3) 和 4)：MIC 写得再好，bit0 不置位硬件就不发。
	 * （原厂 O5 活体 0x17059844=3，我们长期只有 2。）
	 *
	 * ★ 同时删掉 gpon_fifo_burst_o1()：那个函数每 2 秒强制
	 * 0x17059400 O1->O3，会把硬件状态机反复打回 O1，SN 引擎永远 arm 不了。
	 * 厂商在 O2_3 稳态只刷新 MIC/bit0，从不碰状态寄存器——FIFO 那条路
	 * （fi_Configure_Ponmac_Send_Ploam_Msg）只用于 O5 的软件重注册。
	 */
	gpon_write_be(XMAC_SN_LO, gpon_sn);
	gpon_write_be(XMAC_SN_HI, gpon_sn + 4);
	gpon_write_be(XMAC_SN_MIC_LO, frame + 40);
	gpon_write_be(XMAC_SN_MIC_HI, frame + 44);

	{
		void __iomem *h = pon_mac_w(0x170598f4);

		if (h)
			writel((readl(h) & 0x00ffffff) | 0x03000000, h);
	}

	{
		void __iomem *r = pon_mac_w(0x17059844);

		if (r)
			writel(readl(r) | 1u, r);	/* bit0 = SN MIC ready */
	}

	sn_burst_last = jiffies;
	sn_burst_try++;
	if (!(sn_burst_try % 20))
		pr_info("zx279131-pon: sn mic refresh #%u state=%#010x ready=%#010x hdr=%#010x\n",
			sn_burst_try, readl(st),
			readl(pon_mac_w(0x17059844)),
			readl(pon_mac_w(0x170598f4)));
}

/* 周期重放：不依赖收到 0x01 才动作，10 分钟（300 轮）后自停。 */
static void gpon_sn_periodic(struct work_struct *w)
{
	if (!gpon_wq)
		return;

	if (sn_rounds++ < 300)
		schedule_work(&gpon_sn_work);

	queue_delayed_work(gpon_wq, &gpon_sn_timer, msecs_to_jiffies(2000));
}

/*
 * Step-by-step bring-up helper: run exactly one phase and printk around it,
 * so a hardware reset can be pinned to a single register write.
 *   echo step sn|mic|o2|fire|o3 > pon_gpon
 */
static int gpon_step_one(const char *name)
{
	void __iomem *st = pon_mac_w(XMAC_ONU_STATE);
	void __iomem *ctl = pon_mac_w(XMAC_SN_CTRL);
	u32 v;
	int i, sent = 0;

	if (!st || !ctl)
		return -EINVAL;

	pr_info("zx279131-pon: step '%s' begin state=%#010x ctl=%#010x\n",
		name, readl(st), readl(ctl));

	if (!strcmp(name, "sn")) {
		gpon_write_be(XMAC_SN_LO, gpon_sn);
		gpon_write_be(XMAC_SN_HI, gpon_sn + 4);
		pr_info("zx279131-pon: step sn -> %#010x %#010x\n",
			readl(pon_mac_w(XMAC_SN_LO)),
			readl(pon_mac_w(XMAC_SN_HI)));
	} else if (!strcmp(name, "mic")) {
		u8 mic[8];
		int rc;

		memset(mic, 0, sizeof(mic));
		rc = gpon_compute_mic(gpon_sn, mic);
		pr_info("zx279131-pon: step mic rc=%d %8phN\n", rc, mic);
		if (!rc) {
			gpon_write_be(XMAC_SN_MIC_LO, mic);
			gpon_write_be(XMAC_SN_MIC_HI, mic + 4);
			pr_info("zx279131-pon: step mic -> %#010x %#010x\n",
				readl(pon_mac_w(XMAC_SN_MIC_LO)),
				readl(pon_mac_w(XMAC_SN_MIC_HI)));
			gpon_sn_fire();
		}
	} else if (!strcmp(name, "hwinit")) {
		zx279131_pon_run_hw_init();
	} else if (!strcmp(name, "reg")) {
		zx279131_pon_enter_registration();
	} else if (!strcmp(name, "data")) {
		zx279131_pon_enter_data();
	} else if (!strcmp(name, "o2")) {
		v = readl(st);
		writel((v & ~GPON_STATE_MASK) | GPON_ST_O2, st);
		pr_info("zx279131-pon: step o2 %#010x -> %#010x\n", v, readl(st));
	} else if (!strcmp(name, "o3")) {
		v = readl(st);
		writel((v & ~GPON_STATE_MASK) | GPON_ST_O3, st);
		pr_info("zx279131-pon: step o3 %#010x -> %#010x\n", v, readl(st));
	} else if (!strcmp(name, "fire")) {
		v = readl(ctl);
		writel(v | BIT(0), ctl);
		v = readl(ctl);
		writel((v & ~(3u << 1)) | (GPON_ONU_ID << 1) | BIT(3), ctl);
		pr_info("zx279131-pon: step fire ctl=%#010x\n", readl(ctl));
		for (i = 0; i < 250; i++) {
			if (!(readl(ctl) & BIT(3))) {
				sent = 1;
				break;
			}
			usleep_range(200, 400);
		}
		pr_info("zx279131-pon: step fire %s ctl=%#010x\n",
			sent ? "SENT" : "timeout", readl(ctl));
	} else {
		return -EINVAL;
	}

	pr_info("zx279131-pon: step '%s' done\n", name);
	return 0;
}

/* One transmission attempt.  Returns 0 when the hardware actually sent. */
static int gpon_send_sn(void)
{
	void __iomem *st = pon_mac_w(XMAC_ONU_STATE);
	void __iomem *ctl = pon_mac_w(XMAC_SN_CTRL);
	u32 cur, v;
	int i, sent = 0;

	if (!st || !ctl)
		return -EINVAL;

	pr_debug("zx279131-pon: gpon#%u enter sn=%8phN state=%#010x ctl=%#010x mic=%d\n",
		gpon.tries, gpon_sn, readl(st), readl(ctl), gpon.auto_mic);

	/* SN: big-endian words, no byte swap */
	gpon_write_be(XMAC_SN_LO, gpon_sn);
	gpon_write_be(XMAC_SN_HI, gpon_sn + 4);
	pr_debug("zx279131-pon: gpon#%u sn written %#010x %#010x\n", gpon.tries,
		readl(pon_mac_w(XMAC_SN_LO)), readl(pon_mac_w(XMAC_SN_HI)));

	if (gpon.auto_mic) {
		u8 mic[8];
		int rc = gpon_compute_mic(gpon_sn, mic);

		gpon.last_mic_err = rc;
		if (!rc) {
			gpon_write_be(XMAC_SN_MIC_LO, mic);
			gpon_write_be(XMAC_SN_MIC_HI, mic + 4);
			pr_debug("zx279131-pon: gpon#%u mic %8phN -> %#010x %#010x\n",
				gpon.tries, mic,
				readl(pon_mac_w(XMAC_SN_MIC_LO)),
				readl(pon_mac_w(XMAC_SN_MIC_HI)));
		} else {
			pr_warn("zx279131-pon: gpon#%u mic failed %d\n",
				gpon.tries, rc);
		}
	}

	/* O3 -> O2: only O2 gets an upstream slot for the SN burst.
	 * The state change needs a real settling time -- firing 2 ms after
	 * the switch wedges the MAC (observed as a full board hang). */
	cur = readl(st);
	writel((cur & ~GPON_STATE_MASK) | GPON_ST_O2, st);
	pr_debug("zx279131-pon: gpon#%u o2 %#010x -> %#010x\n", gpon.tries,
		 cur, readl(st));
	msleep(GPON_O2_SETTLE_MS);

	/* read-modify-write: never clear bits we do not understand */
	v = readl(ctl);
	writel(v | BIT(0), ctl);			/* SN tx enable */
	v = readl(ctl);
	writel((v & ~(3u << 1)) | (GPON_ONU_ID << 1) | BIT(3), ctl);
	pr_debug("zx279131-pon: gpon#%u fired ctl=%#010x\n", gpon.tries, readl(ctl));

	for (i = 0; i < 250; i++) {
		if (!(readl(ctl) & BIT(3))) {
			sent = 1;
			break;
		}
		usleep_range(200, 400);
	}

	v = readl(ctl);
	if (!sent)
		writel(v & ~BIT(3), ctl);		/* drop the stuck request */

	/* back to O3 (idle, SN already out) */
	writel((readl(st) & ~GPON_STATE_MASK) | GPON_ST_O3, st);
	msleep(GPON_O3_SETTLE_MS);

	gpon.last_state = readl(st);
	gpon.last_ctrl = readl(ctl);

	pr_debug("zx279131-pon: gpon#%u %s ctl=%#010x state=%#010x\n", gpon.tries,
		sent ? "SENT" : "timeout", gpon.last_ctrl, gpon.last_state);

	return sent ? 0 : -ETIMEDOUT;
}

static void gpon_tick(struct work_struct *w)
{
	bool again = false;

	mutex_lock(&gpon_lock);
	if (!gpon.armed) {
		mutex_unlock(&gpon_lock);
		return;
	}

	gpon.tries++;
	if (gpon_send_sn())
		gpon.tx_timeout++;
	else
		gpon.tx_ok++;

	if (gpon.remaining) {
		if (--gpon.remaining == 0)
			gpon.armed = false;
	}
	again = gpon.armed;
	mutex_unlock(&gpon_lock);

	if (again && gpon_wq)
		queue_delayed_work(gpon_wq, &gpon_work,
				   msecs_to_jiffies(gpon.period_ms));
}

static void gpon_kick(void)
{
	if (gpon_wq && !delayed_work_pending(&gpon_work))
		queue_delayed_work(gpon_wq, &gpon_work, 0);
}

static void gpon_init_work(void)
{
	gpon.auto_mic = true;
	gpon.period_ms = 2000;

	gpon_tfm = crypto_alloc_shash("cmac(aes)", 0, 0);
	if (IS_ERR(gpon_tfm)) {
		pr_warn("zx279131-pon: cmac(aes) unavailable (%ld), MIC stays as-is\n",
			PTR_ERR(gpon_tfm));
		gpon_tfm = NULL;
	} else {
		pr_info("zx279131-pon: cmac(aes) ready, SN MIC computed in kernel\n");
	}

	gpon_wq = alloc_workqueue("pon-gpon", WQ_UNBOUND, 1);
	if (!gpon_wq)
		pr_err("zx279131-pon: cannot alloc gpon workqueue\n");

	INIT_DELAYED_WORK(&gpon_work, gpon_tick);
	INIT_WORK(&gpon_sn_work, gpon_sn_auto);
	INIT_WORK(&gpon_rt_work, gpon_rt_worker);
	INIT_DELAYED_WORK(&gpon_sn_timer, gpon_sn_periodic);

	/*
	 * v9.60：照厂商 p_o1_clear_los_or_lof_event（0xcfa0）做一次
	 * O1 -> O2_3 跳变。厂商是 LOS/LOF 告警清除时由软件写状态寄存器完成
	 * 这个跳变的，跳完再 setSnMIC + AllocId_Set，硬件 SN 突发引擎才
	 * arm 起来。bit0/bit3 语义（fi_set_ONU_activation_state）：
	 *   bits[4:0]   = 激活状态（1=O1, 3=O2_3, 4=O4, 5=O5）
	 *   bits[25:16] = ONU-ID，0x3ff = 未注册
	 * 只改 bits[4:0]，ONU-ID 字段保持 0x3ff。
	 */
	{
		void __iomem *st = pon_mac_w(XMAC_ONU_STATE);

		if (st) {
			u32 v = readl(st);

			writel((v & ~GPON_STATE_MASK) | GPON_ST_O1, st);
			msleep(50);
			v = readl(st);
			writel((v & ~GPON_STATE_MASK) | GPON_ST_O3, st);
			pr_info("zx279131-pon: enter O2_3, state=%#010x\n",
				readl(st));
		}
	}

	/* v9.55: 开机 20 秒后开始 SN MIC 周期刷新，每 2 秒一轮，300 轮自停 */
	if (gpon_wq)
		queue_delayed_work(gpon_wq, &gpon_sn_timer,
				   msecs_to_jiffies(20000));
}

static ssize_t pon_gpon_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	char cmd[64], arg[40];
	int n;

	n = sscanf(buf, "%15s %39s", cmd, arg);

	if (!strcmp(cmd, "sn")) {
		size_t len;

		if (n < 2)
			return -EINVAL;
		len = strlen(arg);
		if (len == 16) {			/* hex form */
			int i;

			for (i = 0; i < 8; i++) {
				unsigned int b;

				if (sscanf(arg + i * 2, "%2x", &b) != 1)
					return -EINVAL;
				gpon_sn[i] = b;
			}
		} else if (len == 8) {			/* ASCII form */
			memcpy(gpon_sn, arg, 8);
		} else {
			return -EINVAL;
		}
	} else if (!strcmp(cmd, "password")) {
		size_t len;

		if (n < 2)
			return -EINVAL;
		len = strlen(arg);
		memset(gpon_pw, 0, sizeof(gpon_pw));
		memcpy(gpon_pw, arg, min(len, sizeof(gpon_pw)));
	} else if (!strcmp(cmd, "mic")) {
		if (n < 2)
			return -EINVAL;
		gpon.auto_mic = !strcmp(arg, "auto");
	} else if (!strcmp(cmd, "period")) {
		unsigned int v;

		if (n < 2 || kstrtouint(arg, 10, &v))
			return -EINVAL;
		gpon.period_ms = clamp_val(v, 200, 60000);
	} else if (!strcmp(cmd, "burst")) {
		if (n > 1 && !strcmp(arg, "off")) {
			sn_rounds = 1000000;
			cancel_delayed_work_sync(&gpon_sn_timer);
			pr_info("zx279131-pon: sn periodic stopped\n");
		} else {
			u8 f[48];
			void __iomem *c1, *c2, *c3, *c4;
			u32 s1, s2, s3, s4, e1, e2, e3, e4;

			if (gpon_build_sn_frame(f))
				return -ENOENT;
			gpon_write_be(XMAC_SN_LO, gpon_sn);
			gpon_write_be(XMAC_SN_HI, gpon_sn + 4);
			gpon_write_be(XMAC_SN_MIC_LO, f + 40);
			gpon_write_be(XMAC_SN_MIC_HI, f + 44);

			c1 = pon_mac_w(0x17059998);
			c2 = pon_mac_w(0x17059990);
			c3 = pon_mac_w(XMAC_SN_CTRL);
			c4 = pon_mac_w(XMAC_UP_TX_STAT);
			s1 = c1 ? readl(c1) : 0;
			s2 = c2 ? readl(c2) : 0;
			s3 = c3 ? readl(c3) : 0;
			s4 = c4 ? readl(c4) : 0;

			mutex_lock(&gpon_lock);
			gpon_fifo_burst_o1(f);
			usleep_range(3000, 4000);
			mutex_unlock(&gpon_lock);

			e1 = c1 ? readl(c1) : 0;
			e2 = c2 ? readl(c2) : 0;
			e3 = c3 ? readl(c3) : 0;
			e4 = c4 ? readl(c4) : 0;
			pr_info("zx279131-pon: burst cnt %#010x->%#010x (%+d) stat %#010x->%#010x sn %#010x->%#010x txs %#010x->%#010x\n",
				s1, e1, (int)(e1 - s1), s2, e2, s3, e3, s4, e4);
		}
	} else if (!strcmp(cmd, "run")) {
		unsigned int v = 1, i;
		int rc = 0;

		/* synchronous: the caller's context runs the whole attempt,
		 * no workqueue involved */
		if (n > 1 && kstrtouint(arg, 10, &v))
			return -EINVAL;
		if (v > 100)
			v = 100;
		mutex_lock(&gpon_lock);
		for (i = 0; i < v; i++) {
			if (i)
				msleep(200);
			gpon.tries++;
			rc = gpon_send_sn();
			if (rc)
				gpon.tx_timeout++;
			else
				gpon.tx_ok++;
		}
		mutex_unlock(&gpon_lock);
	} else if (!strcmp(cmd, "start")) {
		unsigned int v = 0;

		if (n > 1 && kstrtouint(arg, 10, &v))
			return -EINVAL;
		mutex_lock(&gpon_lock);
		gpon.armed = true;
		gpon.remaining = v;		/* 0 = until stopped */
		mutex_unlock(&gpon_lock);
		gpon_kick();
	} else if (!strcmp(cmd, "stop")) {
		mutex_lock(&gpon_lock);
		gpon.armed = false;
		mutex_unlock(&gpon_lock);
		cancel_delayed_work_sync(&gpon_work);
	} else if (!strcmp(cmd, "step")) {
		int rc;

		if (n < 2)
			return -EINVAL;
		rc = gpon_step_one(arg);
		if (rc)
			return rc;
	} else if (!strcmp(cmd, "clear")) {
		mutex_lock(&gpon_lock);
		memset(gpon.msgid_hist, 0, sizeof(gpon.msgid_hist));
		gpon.msgid_other = 0;
		gpon.tries = gpon.tx_ok = gpon.tx_timeout = 0;
		mutex_unlock(&gpon_lock);
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t pon_gpon_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int i, n = 0;

	n += sysfs_emit(buf, "sn=");
	for (i = 0; i < 8; i++)
		n += sysfs_emit_at(buf, n, "%02x", gpon_sn[i]);
	n += sysfs_emit_at(buf, n, " (");
	for (i = 0; i < 8; i++)
		n += sysfs_emit_at(buf, n, "%c",
				   (gpon_sn[i] >= 32 && gpon_sn[i] < 127) ?
				   gpon_sn[i] : '.');
	n += sysfs_emit_at(buf, n, ") password=");
	for (i = 0; i < 10; i++)
		n += sysfs_emit_at(buf, n, "%c",
				   (gpon_pw[i] >= 32 && gpon_pw[i] < 127) ?
				   gpon_pw[i] : '.');
	n += sysfs_emit_at(buf, n, " mic=%s period=%ums\n",
			   gpon.auto_mic ? "auto" : "manual", gpon.period_ms);
	return n;
}
static DEVICE_ATTR_RW(pon_gpon);

static ssize_t pon_gpon_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	int i, n = 0;

	n += sysfs_emit(buf,
			"armed=%d remaining=%u tries=%u tx_ok=%u tx_timeout=%u\n"
			"last_state=%#010x last_ctrl=%#010x mic_err=%d\n"
			"last_msgid=%#04x other=%u\n",
			gpon.armed, gpon.remaining, gpon.tries, gpon.tx_ok,
			gpon.tx_timeout, gpon.last_state, gpon.last_ctrl,
			gpon.last_mic_err, gpon.last_msgid, gpon.msgid_other);
	n += sysfs_emit_at(buf, n, "msgid_hist:");
	for (i = 0; i < GPON_MSGID_MAX; i++)
		n += sysfs_emit_at(buf, n, " %02x:%u", i, gpon.msgid_hist[i]);
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RO(pon_gpon_state);

/* Read a PON MAC word, tolerating an unmapped window. */
static u32 pon_mac_rd(u32 addr)
{
	void __iomem *p = pon_mac_w(addr);

	return p ? readl(p) : 0;
}

/*
 * pon_onu - flat key=value snapshot of the GPON activation state.
 *
 * This node is the contract between the driver and luci-app-pon.  The user
 * space helper /usr/sbin/ponctl greps these keys and turns them into the
 * JSON snapshot the web UI consumes (schema_version 1, "line"/"registration"/
 * "counters" objects), so **keep the names stable** - append new keys at the
 * end instead of renaming existing ones.
 *
 * Register sources (all established by measurement, see PON-PLAN):
 *   state     0x17059400 bits[4:0]   O1/O2/O3/O4/O5/O6/O7
 *   onu_id    0x17059400 bits[25:16] 0x3ff = not assigned
 *   sn_ready  0x17059844             bit0 SN/MIC armed, bit1 engine idle
 *   sn_ctl    0x170599bc             bit0 = SN transmit enable
 *   up_en     0x17059800             bit1 = upstream transmit enable
 *   tx_bursts 0x17059988             upstream burst attempts
 *   tx_errors 0x17059998             upstream burst errors
 *   eqd       0x17059410             equalisation delay (encoded)
 *   unreg     0x1705947c             bit22 = hardware still sees us unregistered
 */
static ssize_t pon_onu_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	static const char * const onames[8] = {
		"unknown", "O1", "O2", "O3", "O4", "O5", "O6", "O7"
	};
	u32 st = pon_mac_rd(XMAC_ONU_STATE);
	u32 snr = pon_mac_rd(0x17059844);
	u32 snc = pon_mac_rd(XMAC_SN_CTRL);
	u32 up = pon_mac_rd(0x17059800);
	u32 txb = pon_mac_rd(0x17059988);
	u32 txe = pon_mac_rd(0x17059998);
	u32 eqd = pon_mac_rd(0x17059410);
	u32 nreg = pon_mac_rd(0x1705947c);
	u32 state = st & GPON_STATE_MASK;
	u32 onu_id = (st >> 16) & 0x3ff;
	u32 ploamd = 0;
	int i, n = 0;

	for (i = 0; i < GPON_MSGID_MAX; i++)
		ploamd += gpon.msgid_hist[i];

	n += sysfs_emit(buf,
		"state=%s\n"
		"state_raw=%#010x\n"
		"onu_id=%u\n"
		"onu_id_valid=%u\n"
		"unregistered=%u\n"
		"sn_ready=%u\n"
		"sn_ctl=%#010x\n"
		"up_en=%u\n"
		"tx_bursts=%u\n"
		"tx_errors=%u\n"
		"eqd=%#010x\n",
		state < ARRAY_SIZE(onames) ? onames[state] : "unknown",
		st, onu_id, onu_id != 0x3ff && onu_id != 0,
		!!(nreg & BIT(22)),
		(snr & 3), snc, !!(up & BIT(1)),
		txb, txe, eqd);

	n += sysfs_emit_at(buf, n,
		"ploamd=%u\n"
		"ploamu=%u\n"
		"ranging=%u\n"
		"acks=%u\n"
		"o5=%u\n"
		"last_msgid=%#04x\n"
		"armed=%u\n",
		ploamd, gpon.tx_ok, gpon.rt_msg, gpon.ack_tx, gpon.o5_entered,
		gpon.last_msgid, gpon.armed);

	return n;
}
static DEVICE_ATTR_RO(pon_onu);

/*
 * BWmap record RAM.  Mirrors zte_xgpon.ko: fi_configure_Set_Bwmap_Record_Mode
 * (XMAC 0x16501 = 0x17059404) and PrintBwmapRecord
 * (XMAC 0x16505/6/7/8 = 0x17059414/8/1c/20).
 *
 * ★ 记录使能是 0x17059404 的 **bit16**，不是 bit0。bit16=1 时数据口恒 0，
 *   必须清 bit16 才能读 512 条；读完再置回，否则记录会停。
 *   （bit0 是另一路使能，厂商 Hw_Init 自己会置，这里不动它。）
 *
 *   echo rec  > pon_bwmap    # start recording (bit16=1)
 *   echo stop > pon_bwmap    # stop
 *   echo dump > pon_bwmap    # walk 512 entries, print non-empty ones to dmesg
 *
 * This is the only way to see which Alloc-IDs the OLT actually hands out; a
 * discovery grant (Alloc-ID 0x3ff) is what lets an unregistered ONU transmit.
 */
static ssize_t pon_bwmap_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	char cmd[16];
	u32 v, w0, w1;
	int i, n;

	if (!pon_mac)
		return -ENODEV;
	if (sscanf(buf, "%15s", cmd) != 1)
		return -EINVAL;

	if (!strcmp(cmd, "rec")) {
		v = readl(pon_mac + 0x59404);
		writel(v | 0x00010000u, pon_mac + 0x59404);
		dev_info(dev, "bwmap rec on, ctl=%#010x\n",
			 readl(pon_mac + 0x59404));
	} else if (!strcmp(cmd, "stop")) {
		v = readl(pon_mac + 0x59404);
		writel(v & ~0x00010000u, pon_mac + 0x59404);
		dev_info(dev, "bwmap rec off, ctl=%#010x\n",
			 readl(pon_mac + 0x59404));
	} else if (!strcmp(cmd, "dump")) {
		v = readl(pon_mac + 0x59404);
		writel(v & ~0x00010000u, pon_mac + 0x59404);
		for (i = 0; i < 512; i++) {
			writel(0x08000000u | (u32)i, pon_mac + 0x59414);
			for (n = 0; n < 200; n++) {
				if (readl(pon_mac + 0x59418))
					break;
				udelay(5);
			}
			w0 = readl(pon_mac + 0x5941c);
			w1 = readl(pon_mac + 0x59420);
			if (!w0 && !w1)
				continue;
			dev_info(dev,
				 "bwmap[%3d] w0=%08x w1=%08x alloc=%u f=%02x\n",
				 i, w0, w1, w1 & 0x3fffu, (w1 >> 14) & 0xffu);
		}
		writel(v, pon_mac + 0x59404);
		dev_info(dev, "bwmap dump done, ctl restored %#010x\n",
			 readl(pon_mac + 0x59404));
	} else {
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_WO(pon_bwmap);

/*
 * Kernel-side register access.  Userspace /dev/mem mmap is dangerous on this
 * part: reading an unmapped address hangs the bus and takes the whole board
 * down (no watchdog recovery).  Going through ioremap/readl keeps the access
 * inside the kernel's abort handler, and the address is range-checked first.
 *
 *   echo 0x17000020        > pon_reg    # read
 *   echo 0x17000020 0x320  > pon_reg    # write, then read back
 *   cat pon_reg
 */
#define PON_MAC_END	0x17250000UL

static unsigned long pon_reg_addr;
static u32 pon_reg_last;

static bool pon_addr_ok(unsigned long addr)
{
	if (addr >= PON_MAC_BASE && addr < PON_MAC_END)
		return true;
	if (addr >= PON_SYSCTRL_BASE && addr < PON_TOP_CRM_BASE + PON_BLOCK_SIZE)
		return true;
	/* SerDes file: needed to compare/tune the PMA words against stock */
	if (addr >= PON_SERDES_BASE && addr < PON_SERDES_BASE + PON_BLOCK_SIZE)
		return true;
	if (addr >= 0x10d10000 && addr < 0x10d10100)
		return true;
	return false;
}

static ssize_t pon_reg_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned long addr, val = 0;
	void __iomem *p;
	int n;

	n = sscanf(buf, "%lx %lx", &addr, &val);
	if (n < 1)
		return -EINVAL;
	if ((addr & 3) || !pon_addr_ok(addr))
		return -EINVAL;

	p = ioremap(addr, 4);
	if (!p)
		return -ENOMEM;

	if (n == 2)
		writel((u32)val, p);
	pon_reg_last = readl(p);
	pon_reg_addr = addr;
	iounmap(p);

	return count;
}

static ssize_t pon_reg_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%#010lx = %#010x\n", pon_reg_addr, pon_reg_last);
}
static DEVICE_ATTR_RW(pon_reg);

static DEVICE_ATTR_RO(pon_status);
static DEVICE_ATTR_WO(pon_reinit);

static struct attribute *pon_attrs[] = {
	&dev_attr_pon_status.attr,
	&dev_attr_pon_ploam_all.attr,
	&dev_attr_pon_send.attr,
	&dev_attr_pon_fire.attr,
	&dev_attr_pon_send_state.attr,
	&dev_attr_pon_allocmap.attr,
	&dev_attr_pon_reinit.attr,
	&dev_attr_pon_irq_sample.attr,
	&dev_attr_pon_irq.attr,
	&dev_attr_pon_ploam.attr,
	&dev_attr_pon_reg.attr,
	&dev_attr_pon_gpon.attr,
	&dev_attr_pon_gpon_state.attr,
	&dev_attr_pon_onu.attr,
	&dev_attr_pon_bwmap.attr,
	NULL,
};

static const struct attribute_group pon_attr_group = {
	.attrs = pon_attrs,
};

int zx279131_pon_sysfs_add(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &pon_attr_group);
}

void zx279131_pon_sysfs_del(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &pon_attr_group);
}
