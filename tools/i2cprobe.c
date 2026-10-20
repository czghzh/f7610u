/*
 * i2cprobe.c - 通过 /dev/i2c-N 做 SFF-8472 探测
 *
 *   i2cprobe <bus> scan
 *   i2cprobe <bus> read <addr7> <reg> <len>
 *   i2cprobe <bus> dump <addr7>            # A0/A2 页常用区域
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static int rd_reg(int fd, unsigned char a7, unsigned char reg,
		  unsigned char *buf, int len)
{
	struct i2c_msg m[2];
	struct i2c_rdwr_ioctl_data d;
	unsigned char r = reg;

	m[0].addr = a7; m[0].flags = 0;
	m[0].len = 1;   m[0].buf = &r;
	m[1].addr = a7; m[1].flags = I2C_M_RD;
	m[1].len = len; m[1].buf = buf;
	d.msgs = m; d.nmsgs = 2;

	if (ioctl(fd, I2C_RDWR, &d) < 0)
		return -errno;
	return len;
}

static int wr_reg(int fd, unsigned char a7, unsigned char reg, unsigned char val)
{
	struct i2c_msg m;
	struct i2c_rdwr_ioctl_data d;
	unsigned char b[2] = { reg, val };

	m.addr = a7; m.flags = 0; m.len = 2; m.buf = b;
	d.msgs = &m; d.nmsgs = 1;

	if (ioctl(fd, I2C_RDWR, &d) < 0)
		return -errno;
	return 0;
}

static void hexdump(const unsigned char *p, int n, unsigned base)
{
	int i, j;
	for (i = 0; i < n; i += 16) {
		printf("  %02x:", base + i);
		for (j = 0; j < 16 && i + j < n; j++)
			printf(" %02x", p[i + j]);
		printf("\n");
	}
}

/* SFF-8472 常用字段 */
static void sff_summary(const unsigned char *a0, const unsigned char *a2)
{
	int temp, vcc, txb, txp, rxp, br;

	printf("  vendor  : %.16s\n", a0 + 20);
	printf("  partnum : %.16s\n", a0 + 40);
	printf("  serial  : %.16s\n", a0 + 68);
	printf("  date    : %.8s\n", a0 + 84);

	if (!a2)
		return;

	temp = (short)((a2[96] << 8) | a2[97]);
	vcc  = (a2[98] << 8) | a2[99];
	txb  = (a2[100] << 8) | a2[101];
	txp  = (a2[102] << 8) | a2[103];
	rxp  = (a2[104] << 8) | a2[105];
	br   = (a2[106] << 8) | a2[107];

	printf("  temp    : %.2f C\n", temp / 256.0);
	printf("  vcc     : %.3f V\n", vcc * 0.0001);
	printf("  tx_bias : %.2f mA\n", txb * 0.002);
	printf("  tx_pwr  : %.2f dBm  (raw 0x%04x)\n",
	       txp ? 10.0 * log10(txp / 10000.0) : 0.0, txp);
	printf("  rx_pwr  : %.2f dBm  (raw 0x%04x)\n",
	       rxp ? 10.0 * log10(rxp / 10000.0) : 0.0, rxp);
	printf("  br_nom  : %u MBd\n", br * 100);
	printf("  dmi_type: 0x%02x (0x68=DDM internal cal)\n", a2[92]);
}

int main(int argc, char **argv)
{
	unsigned char a0[256], a2[256];
	char path[32];
	int bus, fd, i, n, found = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <bus> scan|read|dump ...\n", argv[0]);
		return 1;
	}
	bus = atoi(argv[1]);
	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (!strcmp(argv[2], "scan")) {
		printf("scanning bus %d ...\n", bus);
		for (i = 0x03; i <= 0x77; i++) {
			unsigned char z[8];
			n = rd_reg(fd, i, 0x00, z, 1);
			if (n == 1) {
				printf("  0x%02x ACK\n", i);
				found++;
			}
		}
		printf("found %d device(s)\n", found);
		close(fd);
		return 0;
	}

	if (!strcmp(argv[2], "read")) {
		unsigned char a7, reg;
		int len, off = 0;
		if (argc < 6) { fprintf(stderr, "read <a7> <reg> <len>\n"); return 1; }
		a7  = (unsigned char)strtoul(argv[3], 0, 0);
		reg = (unsigned char)strtoul(argv[4], 0, 0);
		len = (int)strtoul(argv[5], 0, 0);
		if (len > 256) len = 256;
		{
			unsigned char b[256];
			while (off < len) {
				int c = len - off > 128 ? 128 : len - off;
				n = rd_reg(fd, a7, reg + off, b + off, c);
				if (n < 0) {
					printf("ERR reg %#x: %s\n", reg + off, strerror(-n));
					return 1;
				}
				off += n;
			}
			hexdump(b, len, reg);
		}
		close(fd);
		return 0;
	}

	if (!strcmp(argv[2], "dump")) {
		unsigned char a7 = (unsigned char)strtoul(argv[3], 0, 0);
		memset(a0, 0xff, sizeof(a0));
		memset(a2, 0xff, sizeof(a2));

		printf("== page A0 (0x%02x) ==\n", a7);
		n = rd_reg(fd, a7, 0x00, a0, 128);
		if (n < 0) {
			printf("  ERR: %s\n", strerror(-n));
			close(fd);
			return 1;
		}
		printf("  ok, 128 bytes @0x00\n");

		/* A2 页要求 7bit 地址 +1 */
		printf("== page A2 (0x%02x) ==\n", a7 + 1);
		n = rd_reg(fd, a7 + 1, 0x00, a2, 128);
		if (n < 0) {
			printf("  ERR: %s\n", strerror(-n));
			sff_summary(a0, NULL);
			hexdump(a0, 128, 0);
			close(fd);
			return 0;
		}
		printf("  ok, 128 bytes @0x00\n");
		if (rd_reg(fd, a7 + 1, 0x60, a2 + 96, 32) < 0)
			printf("  warn: DDM 0x60-0x7f read failed\n");

		printf("== summary ==\n");
		sff_summary(a0, a2);
		printf("== A0 raw ==\n");
		hexdump(a0, 128, 0);
		printf("== A2 0x60..0x7f (DDM) ==\n");
		hexdump(a2 + 96, 32, 0x60);
		close(fd);
		return 0;
	}

	/* 写测试（仅诊断用） */
	if (!strcmp(argv[2], "wr")) {
		unsigned char a7 = (unsigned char)strtoul(argv[3], 0, 0);
		unsigned char reg = (unsigned char)strtoul(argv[4], 0, 0);
		unsigned char val = (unsigned char)strtoul(argv[5], 0, 0);
		n = wr_reg(fd, a7, reg, val);
		printf("wr 0x%02x[0x%02x]=0x%02x -> %d (%s)\n", a7, reg, val, n,
		       n ? strerror(-n) : "ok");
		close(fd);
		return n ? 1 : 0;
	}

	fprintf(stderr, "unknown cmd %s\n", argv[2]);
	close(fd);
	return 1;
}
