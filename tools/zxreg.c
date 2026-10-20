/*
 * zxreg - read/write 32-bit MMIO words through /dev/mem.
 *
 *   zxreg r <hexaddr> [count]      print "addr = value" lines
 *   zxreg w <hexaddr> <hexval>     write one word
 *   zxreg d <hexbase> <count>      dump only values, comma separated
 *
 * Built static so it can be dropped onto any aarch64 firmware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE 4096UL

static volatile void *map_word(int fd, uint64_t pa, uint64_t *page_off)
{
	uint64_t base = pa & ~(uint64_t)(PAGE - 1);
	void *m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
	if (m == MAP_FAILED)
		return NULL;
	*page_off = pa - base;
	return m;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s r|d <hexaddr> [count] | w <hexaddr> <hexval>\n", argv[0]);
		return 2;
	}

	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}

	if (argv[1][0] == 'w') {
		if (argc < 4)
			return 2;
		uint64_t pa = strtoull(argv[2], NULL, 16);
		uint32_t v = (uint32_t)strtoull(argv[3], NULL, 16);
		uint64_t off;
		volatile void *m = map_word(fd, pa, &off);
		if (!m) {
			perror("mmap");
			return 1;
		}
		*(volatile uint32_t *)((char *)m + off) = v;
		munmap((void *)m, PAGE);
		return 0;
	}

	uint64_t pa = strtoull(argv[2], NULL, 16);
	unsigned long n = (argc > 3) ? strtoul(argv[3], NULL, 0) : 1;
	int compact = (argv[1][0] == 'd');

	for (unsigned long i = 0; i < n; i++) {
		uint64_t a = pa + i * 4;
		uint64_t off;
		volatile void *m = map_word(fd, a, &off);
		if (!m) {
			fprintf(stderr, "mmap fail @%llx\n", (unsigned long long)a);
			return 1;
		}
		uint32_t v = *(volatile uint32_t *)((char *)m + off);
		munmap((void *)m, PAGE);
		if (compact)
			printf("%s%08x", i ? "," : "", v);
		else
			printf("0x%08llx = 0x%08x\n", (unsigned long long)a, v);
	}
	if (compact)
		printf("\n");
	close(fd);
	return 0;
}
