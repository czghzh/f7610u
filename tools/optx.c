/* optx.c — read/enable the optical module TX laser (A2 page @ i2c 0x51).
 * Mirrors the vendor hw_power_optx_set() from bspdriver.ko 0x5d40:
 *   write 0x7f = 6; read 0x6e; clear bit6 (TX_DISABLE) -> laser on
 *   (set bit6 -> laser off)
 * Usage: optx [read|on|off] [bus] [addr]
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

static int fd;

static int rd(int bus, int addr, int reg)
{
	fd = open(bus == 0 ? "/dev/i2c-0" : "/dev/i2c-1", O_RDWR);
	if (fd < 0) { perror("open"); return -1; }
	if (ioctl(fd, I2C_SLAVE, addr) < 0) { perror("ioctl"); return -1; }
	unsigned char r = reg;
	if (write(fd, &r, 1) != 1) { perror("w reg"); close(fd); return -1; }
	unsigned char v;
	if (read(fd, &v, 1) != 1) { perror("r val"); close(fd); return -1; }
	close(fd);
	return v;
}

static int wr(int bus, int addr, int reg, int val)
{
	fd = open(bus == 0 ? "/dev/i2c-0" : "/dev/i2c-1", O_RDWR);
	if (fd < 0) { perror("open"); return -1; }
	if (ioctl(fd, I2C_SLAVE, addr) < 0) { perror("ioctl"); return -1; }
	unsigned char b[2] = { reg, val };
	if (write(fd, b, 2) != 2) { perror("wr"); close(fd); return -1; }
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "read";
	int bus = argc > 2 ? atoi(argv[2]) : 0;
	int addr = argc > 3 ? (int)strtol(argv[3], NULL, 0) : 0x51;

	if (!strcmp(cmd, "read")) {
		printf("A2(0x%02x) bus%d DDM area:\n", addr, bus);
		for (int r = 0x60; r <= 0x7f; r += 8) {
			printf("  %02x:", r);
			for (int i = 0; i < 8; i++) {
				int v = rd(bus, addr, r + i);
				printf(" %02x", v < 0 ? 0xff : v);
			}
			printf("\n");
		}
		int v = rd(bus, addr, 0x6e);
		printf("=> 0x6e=0x%02x  TX_DISABLE(bit6)=%d  %s\n",
		       v < 0 ? -1 : v, v < 0 ? -1 : ((v >> 6) & 1),
		       (v >= 0 && !(v & 0x40)) ? "LASER ON" : "LASER OFF");
	} else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
		int on = !strcmp(cmd, "on");
		/* vendor: write 0x7f=6 first (module sequence register) */
		wr(bus, addr, 0x7f, 6);
		int v = rd(bus, addr, 0x6e);
		if (v < 0) { printf("read 0x6e failed\n"); return 1; }
		printf("before: 0x6e=0x%02x\n", v);
		v = on ? (v & ~0x40) : (v | 0x40);
		if (wr(bus, addr, 0x6e, v)) return 1;
		usleep(10000);
		int v2 = rd(bus, addr, 0x6e);
		printf("after : 0x6e=0x%02x (readback 0x%02x) %s\n",
		       v, v2 < 0 ? -1 : v2, on ? "LASER ENABLED" : "LASER DISABLED");
	} else {
		printf("usage: optx [read|on|off] [bus] [addr]\n");
		return 1;
	}
	return 0;
}
