#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static int fd;

static uint32_t rd(uint32_t pa, int *ok) {
    long pg = pa & ~0xfffUL;
    unsigned off = pa & 0xfff;
    void *m = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, pg);
    if (m == MAP_FAILED) { *ok = 0; return 0; }
    uint32_t v = *(volatile uint32_t *)((char *)m + off);
    munmap(m, 0x1000);
    *ok = 1;
    return v;
}

static int wr(uint32_t pa, uint32_t val) {
    long pg = pa & ~0xfffUL;
    unsigned off = pa & 0xfff;
    void *m = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
    if (m == MAP_FAILED) return -1;
    *(volatile uint32_t *)((char *)m + off) = val;
    munmap(m, 0x1000);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <hexaddr> [count]\n", argv[0]);
        printf("       %s <hexaddr>=<hexval>\n", argv[0]);
        return 1;
    }
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    char *eq = strchr(argv[1], '=');
    if (eq) {
        *eq = 0;
        uint32_t pa = strtoul(argv[1], 0, 16);
        uint32_t v  = strtoul(eq + 1, 0, 16);
        if (wr(pa, v)) { perror("mmap w"); return 1; }
        printf("wrote 0x%08x = 0x%08x\n", pa, v);
        return 0;
    }
    uint32_t pa = strtoul(argv[1], 0, 16);
    int n = (argc > 2) ? atoi(argv[2]) : 1;
    for (int i = 0; i < n; i++) {
        int ok;
        uint32_t v = rd(pa + i * 4, &ok);
        if (!ok) { printf("0x%08x = <mmap fail>\n", pa + i * 4); continue; }
        printf("0x%08x = 0x%08x\n", pa + i * 4, v);
    }
    return 0;
}
