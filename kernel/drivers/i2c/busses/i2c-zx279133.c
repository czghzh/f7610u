// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C bus driver for the ZTE ZX279131 / ZX279133 "zx_i2c" controller.
 *
 * The register layout below was recovered from the vendor 4.1.25 kernel
 * image (kallsyms + objdump), not from a datasheet:
 *
 *   zx_i2c_read_data   @ 0xffffffc0004248d8
 *   zx_i2c_write_data  @ 0xffffffc0004249d8
 *   zx_i2c_wait        @ 0xffffffc000424690
 *   zx_i2c_init_ctr    @ 0xffffffc000424f28
 *   zx_i2c_xfer        @ 0xffffffc0004245d0
 *
 * Register map (offsets from the controller base, all 32-bit):
 *   0x04 CMD       0x01 idle, 0x41 write, 0x51 read (7-bit), 0x53 read (10-bit)
 *   0x0c ADDR_HI   (addr >> 7) & 7, zero for 7-bit addressing
 *   0x10 ADDR_LO   addr & 0x7f
 *   0x14 SCL_DIV   SCL divider
 *   0x1c INT_EN_A  bit7 must be set during a transfer
 *   0x20 INT_EN_B  bit7 must be set during a transfer
 *   0x24 DATA      byte FIFO (same register for tx and rx)
 *   0x28 STAT      bit1 nak, bit2/3 error, bit6 done
 *   0x2c TX_STAT   bit0 = tx fifo empty
 *   0x30 RX_STAT   bit1 = rx data available
 *
 * The controller has no usable interrupt handler in the vendor driver either:
 * it polls STAT with a ~10us step, and so do we.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ZX_I2C_CMD		0x04
#define ZX_I2C_ADDR_HI		0x0c
#define ZX_I2C_ADDR_LO		0x10
#define ZX_I2C_SCL_DIV		0x14
#define ZX_I2C_INT_EN_A		0x1c
#define ZX_I2C_INT_EN_B		0x20
#define ZX_I2C_DATA		0x24
#define ZX_I2C_STAT		0x28
#define ZX_I2C_TX_STAT		0x2c
#define ZX_I2C_RX_STAT		0x30

#define ZX_CMD_IDLE		0x01
#define ZX_CMD_WRITE		0x41
#define ZX_CMD_READ		0x51
#define ZX_CMD_READ_TEN		0x53

/*
 * STAT is a latched status register: bit1 = NAK, bit2/bit3 = protocol errors,
 * bit6 = shifter still busy.  Writing 0x80 back acknowledges and clears it.
 * (Verified against the stock zx_i2c_wait() disassembly.)
 */
#define ZX_ST_NAK		BIT(1)
#define ZX_ST_ERR2		BIT(2)
#define ZX_ST_ERR3		BIT(3)
#define ZX_ST_BUSY		BIT(6)
#define ZX_ST_CLEAR		0x80

/* TX fifo: bit0 = full, bit1 = empty.  RX fifo: bit1 = empty. */
#define ZX_TX_FULL		BIT(0)
#define ZX_TX_EMPTY		BIT(1)
#define ZX_RX_EMPTY		BIT(1)

/* the vendor driver polls with __const_udelay(0xa7c6) == udelay(10) */
#define ZX_POLL_DELAY_US	10

/* the vendor driver uses 3000 x ~10us for a whole message ... */
#define ZX_XFER_LOOPS		3000
/* ... and 200 x ~10us per byte on the fifo handshakes */
#define ZX_FIFO_LOOPS		200
/* fifo depth: the vendor driver splits anything above 32 bytes */
#define ZX_FIFO_DEPTH		32

struct zx_i2c {
	void __iomem *base;
	struct i2c_adapter adap;
};

static inline u32 zx_rd(struct zx_i2c *i2c, u32 reg)
{
	return readl(i2c->base + reg);
}

static inline void zx_wr(struct zx_i2c *i2c, u32 reg, u32 val)
{
	writel(val, i2c->base + reg);
}

/*
 * Wait for the transfer to finish.  Mirrors the stock zx_i2c_wait(): poll
 * STAT every 43us, bail out on the latched error bits, and only declare
 * success once the fifo handshake lines up (a byte arrived for a read, the
 * tx fifo drained for a write) and the shifter is no longer busy.
 */
static int zx_wait_done(struct zx_i2c *i2c, bool for_read)
{
	int i, ret = -ETIMEDOUT;

	for (i = 0; i < ZX_XFER_LOOPS; i++) {
		u32 st;

		udelay(ZX_POLL_DELAY_US);
		st = zx_rd(i2c, ZX_I2C_STAT);

		if (st & ZX_ST_NAK) {
			ret = -ENXIO;
			break;
		}
		if (st & (ZX_ST_ERR2 | ZX_ST_ERR3)) {
			ret = -EIO;
			break;
		}
		if (for_read) {
			if (zx_rd(i2c, ZX_I2C_RX_STAT) & ZX_RX_EMPTY)
				continue;
		} else {
			if (!(zx_rd(i2c, ZX_I2C_TX_STAT) & ZX_TX_EMPTY))
				continue;
		}
		if (st & ZX_ST_BUSY)
			continue;

		ret = 0;
		break;
	}

	zx_wr(i2c, ZX_I2C_STAT, ZX_ST_CLEAR);
	return ret;
}

static void zx_set_addr(struct zx_i2c *i2c, u16 addr)
{
	zx_wr(i2c, ZX_I2C_ADDR_LO, addr & 0x7f);
	zx_wr(i2c, ZX_I2C_ADDR_HI, (addr >> 7) & 0x7);
}

static int zx_read_bytes(struct zx_i2c *i2c, u16 addr, u8 *buf, u32 len)
{
	int ret, i;

	zx_set_addr(i2c, addr);
	zx_wr(i2c, ZX_I2C_CMD,
	      ((addr >> 7) & 0x7) ? ZX_CMD_READ_TEN : ZX_CMD_READ);

	ret = zx_wait_done(i2c, true);
	if (ret)
		goto out;

	for (i = 0; i < len; i++) {
		int t = ZX_FIFO_LOOPS;

		while (zx_rd(i2c, ZX_I2C_RX_STAT) & ZX_RX_EMPTY) {
			if (!--t) {
				dev_warn_ratelimited(&i2c->adap.dev,
						     "rx fifo empty at byte %d\n", i);
				break;
			}
			udelay(ZX_POLL_DELAY_US);
		}
		buf[i] = zx_rd(i2c, ZX_I2C_DATA) & 0xff;
	}

out:
	zx_wr(i2c, ZX_I2C_CMD, ZX_CMD_IDLE);
	return ret;
}

static int zx_write_bytes(struct zx_i2c *i2c, u16 addr, const u8 *buf, u32 len)
{
	u32 done = 0, i;
	int ret;

	zx_set_addr(i2c, addr);

	/* first chunk goes into the fifo before the command is armed */
	for (i = 0; i < len && i < ZX_FIFO_DEPTH; i++)
		zx_wr(i2c, ZX_I2C_DATA, buf[i]);
	done = i;

	zx_wr(i2c, ZX_I2C_CMD, ZX_CMD_WRITE);

	/* the rest is pushed as the fifo drains */
	while (done < len) {
		int t = ZX_FIFO_LOOPS;

		while (zx_rd(i2c, ZX_I2C_TX_STAT) & ZX_TX_FULL) {
			if (!--t)
				break;
			udelay(ZX_POLL_DELAY_US);
		}
		zx_wr(i2c, ZX_I2C_DATA, buf[done++]);
	}

	ret = zx_wait_done(i2c, false);
	zx_wr(i2c, ZX_I2C_CMD, ZX_CMD_IDLE);
	return ret;
}

static int zx_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct zx_i2c *i2c = i2c_get_adapdata(adap);
	int i, ret;

	/* the vendor driver enables the block's interrupt gate around a xfer */
	zx_wr(i2c, ZX_I2C_INT_EN_A, zx_rd(i2c, ZX_I2C_INT_EN_A) | BIT(7));
	zx_wr(i2c, ZX_I2C_INT_EN_B, zx_rd(i2c, ZX_I2C_INT_EN_B) | BIT(7));

	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];

		if (m->flags & I2C_M_RD)
			ret = zx_read_bytes(i2c, m->addr, m->buf, m->len);
		else
			ret = zx_write_bytes(i2c, m->addr, m->buf, m->len);

		if (ret)
			return ret;
	}

	return num;
}

static u32 zx_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm zx_i2c_algo = {
	.master_xfer	= zx_i2c_xfer,
	.functionality	= zx_i2c_func,
};

/*
 * SCL = input_clock / (4 * (div + 1)), so aim at 100 kHz.  The stock firmware
 * lands on 249 with the SoC's 100 MHz i2c clock; keep that as the fallback in
 * case the clock provider is not modelled in this tree.
 */
#define ZX_I2C_SCL_FALLBACK_DIV	249

static u32 zx_i2c_scl_div(struct platform_device *pdev)
{
	unsigned long rate = 0;
	struct clk *clk;
	u32 div;

	clk = devm_clk_get(&pdev->dev, "i2cclk");
	if (!IS_ERR_OR_NULL(clk)) {
		clk_prepare_enable(clk);
		rate = clk_get_rate(clk);
	}

	if (!rate)
		return ZX_I2C_SCL_FALLBACK_DIV;

	div = rate / (4 * 100000);
	if (div < 2)
		div = 2;

	dev_info(&pdev->dev, "i2c clk %lu Hz -> SCL_DIV %u\n", rate, div - 1);
	return div - 1;
}

static int zx_i2c_probe(struct platform_device *pdev)
{
	struct zx_i2c *i2c;
	int ret;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	/*
	 * Reproduce what zx_i2c_init_ctr() leaves behind.  The block is already
	 * clocked and reset by the bootloader by the time we get here; touching
	 * the clock/reset providers from here only risks fighting the firmware.
	 *
	 * SCL_DIV must not be hardcoded: the vendor computes it from the input
	 * clock, and the stock firmware ends up with 249 (~100 kHz).  Running
	 * the bus at divider 2 (~8 MHz) makes every real device NAK, which is
	 * exactly why the optical module EEPROM was never detected.
	 */
	zx_wr(i2c, ZX_I2C_CMD, ZX_CMD_IDLE);
	zx_wr(i2c, ZX_I2C_SCL_DIV, zx_i2c_scl_div(pdev));
	zx_wr(i2c, ZX_I2C_INT_EN_A, 0x1f);
	zx_wr(i2c, ZX_I2C_INT_EN_B, 0x1f);

	i2c_set_adapdata(&i2c->adap, i2c);
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &zx_i2c_algo;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = pdev->dev.of_node;
	strscpy(i2c->adap.name, "zx_i2c", sizeof(i2c->adap.name));

	platform_set_drvdata(pdev, i2c);

	ret = i2c_add_adapter(&i2c->adap);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "ZX I2C bus registered (%s)\n", i2c->adap.name);
	return 0;
}

static void zx_i2c_remove(struct platform_device *pdev)
{
	struct zx_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
}

static const struct of_device_id zx_i2c_of_match[] = {
	{ .compatible = "zte,zx-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx_i2c_of_match);

static struct platform_driver zx_i2c_driver = {
	.probe	= zx_i2c_probe,
	.remove	= zx_i2c_remove,
	.driver	= {
		.name		= "zx_i2c",
		.of_match_table	= zx_i2c_of_match,
	},
};
module_platform_driver(zx_i2c_driver);

MODULE_DESCRIPTION("ZTE ZX279131/ZX279133 I2C bus controller");
MODULE_LICENSE("GPL");
