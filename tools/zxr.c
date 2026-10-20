/*
 * zxr - ZX279131 register / raw-i2c helper (needs CONFIG_DEVMEM)
 *
 *   zxr rd   <pa> [count]          read 32-bit words
 *   zxr wr   <pa> <val>            write one 32-bit word
 *   zxr dump <pa> <count>          read words, 4 per line
 *   zxr i2c  <a7> <off> <len>      talk to i2c0 through the controller regs
 *
 * The i2c path inside the kernel driver is fine now, but this lets us watch
 * the controller state machine directly without rebuilding anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define I2C_BASE	0x14f03000UL
#define I2C_CMD		0x04
#define I2C_ADDR_HI	0x0c
#define I2C_ADDR_LO	0x10
#define I2C_SCL_DIV	0x14
#define I2C_DATA	0x24
#define I2C_STAT	0x28
#define I2C_TX_STAT	0x2c
#define I2C_RX_STAT	0x30

#define CMD_IDLE	0x01
#define CMD_WRITE	0x41
#define CMD_READ	0x51

#define ST_NAK		(1u << 1)
#define ST_ERR2		(1u << 2)
#define ST_ERR3		(1u << 3)
#define ST_BUSY		(1u << 6)
#define ST_CLEAR	0x80

static int fd;

static uint32_t *map(uint32_t pa)
{
	long pg = pa & ~0xfffUL;
	void *m = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
	if (m == MAP_FAILED)
		return NULL;
	return (uint32_t *)((char *)m + (pa & 0xfff));
}

static uint32_t rd(uint32_t pa)
{
	uint32_t *p = map(pa);
	uint32_t v;
	if (!p)
		return 0xffffffff;
	v = *p;
	munmap((void *)((char *)p - (pa & 0xfff)), 0x1000);
	return v;
}

static int wr(uint32_t pa, uint32_t val)
{
	uint32_t *p = map(pa);
	uint32_t rb;

	if (!p)
		return -1;
	*p = val;
	__sync_synchronize();
	asm volatile("dsb sy" ::: "memory");
	rb = *p;
	munmap((void *)((char *)p - (pa & 0xfff)), 0x1000);
	if (rb != val)
		fprintf(stderr, "  ! readback in same mapping: 0x%08x\n", rb);
	return 0;
}

/* poll STAT like the vendor driver: NAK / error latched, bit6 = busy */
static int wait_done(int for_read)
{
	int i;

	for (i = 0; i < 3000; i++) {
		uint32_t st;

		usleep(10);
		st = rd(I2C_BASE + I2C_STAT);
		if (st & ST_NAK)  { wr(I2C_BASE + I2C_STAT, ST_CLEAR); return -2; }
		if (st & ST_ERR2) { wr(I2C_BASE + I2C_STAT, ST_CLEAR); return -4; }
		if (st & ST_ERR3) { wr(I2C_BASE + I2C_STAT, ST_CLEAR); return -1; }
		if (for_read) {
			if (rd(I2C_BASE + I2C_RX_STAT) & 2)
				continue;
		} else {
			if (!(rd(I2C_BASE + I2C_TX_STAT) & 2))
				continue;
		}
		if (st & ST_BUSY)
			continue;
		wr(I2C_BASE + I2C_STAT, ST_CLEAR);
		return 0;
	}
	wr(I2C_BASE + I2C_STAT, ST_CLEAR);
	return -5;
}

static int i2c_read(unsigned a7, unsigned off, unsigned len, unsigned char *buf)
{
	unsigned i;
	int ret;

	wr(I2C_BASE + I2C_ADDR_LO, a7 & 0x7f);
	wr(I2C_BASE + I2C_ADDR_HI, 0);

	/* set the byte pointer */
	wr(I2C_BASE + I2C_DATA, off);
	wr(I2C_BASE + I2C_CMD, CMD_WRITE);
	ret = wait_done(0);
	wr(I2C_BASE + I2C_CMD, CMD_IDLE);
	if (ret) {
		fprintf(stderr, "offset write failed: %d\n", ret);
		return ret;
	}

	wr(I2C_BASE + I2C_CMD, CMD_READ);
	ret = wait_done(1);
	if (ret) {
		fprintf(stderr, "read command failed: %d\n", ret);
		wr(I2C_BASE + I2C_CMD, CMD_IDLE);
		return ret;
	}

	for (i = 0; i < len; i++) {
		int t = 200;

		while (rd(I2C_BASE + I2C_RX_STAT) & 2) {
			if (!--t) {
				fprintf(stderr, "rx empty at %u\n", i);
				break;
			}
			usleep(10);
		}
		buf[i] = rd(I2C_BASE + I2C_DATA) & 0xff;
	}

	wr(I2C_BASE + I2C_CMD, CMD_IDLE);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned a7, off, len, i, n;
	unsigned char buf[512];

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s rd <pa> [count]\n"
			"       %s wr <pa> <val>\n"
			"       %s dump <pa> <count>\n"
			"       %s i2c <a7> <off> <len>\n", argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }

	if (!strcmp(argv[1], "rd")) {
		uint32_t pa = strtoul(argv[2], 0, 16);
		n = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 1;
		for (i = 0; i < n; i++)
			printf("0x%08x = 0x%08x\n", pa + i * 4, rd(pa + i * 4));
		return 0;
	}
	if (!strcmp(argv[1], "wr")) {
		uint32_t pa = strtoul(argv[2], 0, 16);
		uint32_t v  = strtoul(argv[3], 0, 16);
		if (wr(pa, v)) { perror("mmap"); return 1; }
		printf("0x%08x <= 0x%08x (now 0x%08x)\n", pa, v, rd(pa));
		return 0;
	}
	if (!strcmp(argv[1], "dump")) {
		uint32_t pa = strtoul(argv[2], 0, 16);
		n = (unsigned)strtoul(argv[3], 0, 0);
		for (i = 0; i < n; i++) {
			if (!(i % 4))
				printf("%08x:", pa + i * 4);
			printf(" %08x", rd(pa + i * 4));
			if ((i % 4) == 3 || i == n - 1)
				printf("\n");
		}
		return 0;
	}
	if (!strcmp(argv[1], "i2c")) {
		int ret;
		if (argc < 5) { fprintf(stderr, "i2c <a7> <off> <len>\n"); return 1; }
		a7  = (unsigned)strtoul(argv[2], 0, 0);
		off = (unsigned)strtoul(argv[3], 0, 0);
		len = (unsigned)strtoul(argv[4], 0, 0);
		if (len > sizeof(buf)) len = sizeof(buf);
		ret = i2c_read(a7, off, len, buf);
		if (ret) return 1;
		for (i = 0; i < len; i++) {
			if (!(i % 16))
				printf("  %02x:", off + i);
			printf(" %02x", buf[i]);
			if ((i % 16) == 15 || i == len - 1)
				printf("\n");
		}
		return 0;
	}

	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 1;
}
