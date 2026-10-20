/*
 * mtdpatch — 读整块 → 改 1 字节 → （可选）写回，用于 F7610U mtd2 配置块修改
 *
 * build: aarch64-openwrt-linux-musl-gcc -static -O2 -o mtdpatch mtdpatch.c
 *
 * usage:
 *   mtdpatch <dev> <blockoff> <byteoff> <oldval> <newval> [--dry]
 *     blockoff : 擦除块起始偏移（必须块对齐）
 *     byteoff  : 块内字节偏移
 *     oldval/newval : 十六进制字节，如 30 31
 *   --dry : 只读校验 + 打印差异（不写）
 *   写模式：校验旧值 == oldval → MEMERASE → 整块写回 → 读回全块比对 → 报 OK/差异
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <dev> <blockoff> <byteoff> <oldval> <newval> [--dry]\n", argv[0]);
        return 1;
    }
    const char *dev = argv[1];
    long blockoff = strtol(argv[2], NULL, 0);
    long byteoff  = strtol(argv[3], NULL, 0);
    int oldval = (int)strtol(argv[4], NULL, 16);
    int newval = (int)strtol(argv[5], NULL, 16);
    int dry = (argc > 6 && strcmp(argv[6], "--dry") == 0);

    int fd = open(dev, dry ? O_RDONLY : O_RDWR);
    if (fd < 0) { perror("open"); return 2; }

    struct mtd_info_user mi;
    if (ioctl(fd, MEMGETINFO, &mi) < 0) { perror("MEMGETINFO"); return 2; }
    unsigned es = mi.erasesize;
    if (blockoff % (long)es) { fprintf(stderr, "blockoff not block-aligned (es=%u)\n", es); return 2; }
    if (byteoff < 0 || (unsigned)byteoff >= es) { fprintf(stderr, "byteoff out of block\n"); return 2; }

    unsigned char *buf = malloc(es);
    if (!buf) { perror("malloc"); return 2; }
    if (pread(fd, buf, es, blockoff) != (ssize_t)es) { perror("pread"); return 2; }

    int cur = buf[byteoff];
    printf("dev=%s block=%#lx byteoff=%#lx erasesize=%u\n", dev, blockoff, byteoff, es);
    printf("cur=%02x old=%02x new=%02x\n", cur, oldval, newval);
    printf("head16: ");
    for (int i = 0; i < 16; i++) printf("%02x ", buf[i]);
    printf("\n");
    if (cur != oldval) { fprintf(stderr, "MISMATCH: cur=%02x != old=%02x — abort\n", cur, oldval); return 3; }
    if (dry) { printf("DRY: no write.\n"); close(fd); free(buf); return 0; }

    buf[byteoff] = newval;
    struct erase_info_user ei = { .start = (uint32_t)blockoff, .length = es };
    if (ioctl(fd, MEMERASE, &ei) < 0) { perror("MEMERASE"); return 4; }
    if (pwrite(fd, buf, es, blockoff) != (ssize_t)es) { perror("pwrite"); return 4; }
    fsync(fd);

    unsigned char *rb = malloc(es);
    if (!rb) { perror("malloc2"); return 4; }
    if (pread(fd, rb, es, blockoff) != (ssize_t)es) { perror("pread2"); return 4; }
    if (memcmp(buf, rb, es) == 0) {
        printf("WRITE+VERIFY OK (block %u bytes identical, byte[%#lx]=%02x)\n", es, byteoff, rb[byteoff]);
        close(fd); free(buf); free(rb); return 0;
    }
    int diffs = 0, first = -1;
    for (unsigned i = 0; i < es; i++) if (buf[i] != rb[i]) { diffs++; if (first < 0) first = (int)i; }
    fprintf(stderr, "VERIFY FAIL: %d diffs, first at %#x\n", diffs, first);
    return 5;
}
