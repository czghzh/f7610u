/*
 * mtdtool — minimal MTD character-device tool for F7610U recovery / flashing
 *
 * build (host, ImmortalWrt musl toolchain):
 *   aarch64-openwrt-linux-musl-gcc -static -O2 -o mtdtool mtdtool.c
 *
 * usage:
 *   mtdtool info  <dev>
 *   mtdtool erase <dev> <off> <len>            erase range (block aligned)
 *   mtdtool write <dev> <off> <file>           erase + stream file into device
 *   mtdtool read  <dev> <off> <len> <file>     dump range to file
 *
 * off/len accept decimal or 0x-hex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <mtd/mtd-user.h>

#define CHUNK (128 * 1024)

static uint32_t round_up(uint32_t v, uint32_t a) {
    return (v + a - 1) / a * a;
}

static int erase_range(int fd, uint32_t off, uint32_t len) {
    struct mtd_info_user mi;
    if (ioctl(fd, MEMGETINFO, &mi)) { perror("MEMGETINFO"); return -1; }
    if (off % mi.erasesize || len % mi.erasesize) {
        fprintf(stderr, "erase not block-aligned (erasesize=0x%x)\n", mi.erasesize);
        return -1;
    }
    uint32_t n = len / mi.erasesize;
    fprintf(stderr, "erase: %u x 0x%x @ 0x%x ...\n", n, mi.erasesize, off);
    for (uint32_t i = 0; i < n; i++) {
        struct erase_info_user ei;
        ei.start  = off + i * mi.erasesize;
        ei.length = mi.erasesize;
        /* 坏块：先查 MEMGETBADBLOCK，是坏块就直接跳过（与 U-Boot nand erase 语义一致） */
        long long bad = (long long)ei.start;
        if (ioctl(fd, MEMGETBADBLOCK, &bad) > 0) {
            fprintf(stderr, "erase: skip BAD block @0x%x\n", ei.start);
            continue;
        }
        if (ioctl(fd, MEMERASE, &ei)) {
            if (errno == EIO) {
                fprintf(stderr, "erase: EIO, treat as bad block, skip @0x%x\n", ei.start);
                continue;
            }
            fprintf(stderr, "MEMERASE @0x%x failed: %s\n", ei.start, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { goto usage_err; }
    const char *cmd = argv[1];
    const char *dev = argv[2];

    if (!strcmp(cmd, "info")) {
        int fd = open(dev, O_RDONLY);
        if (fd < 0) { perror("open"); return 1; }
        struct mtd_info_user mi;
        if (ioctl(fd, MEMGETINFO, &mi)) { perror("MEMGETINFO"); return 1; }
        printf("type=0x%x flags=0x%x size=0x%x erasesize=0x%x writesize=0x%x oobsize=0x%x\n",
               mi.type, mi.flags, mi.size, mi.erasesize, mi.writesize, mi.oobsize);
        return 0;
    }

    if (argc < 5) { goto usage_err; }
    uint32_t off = (uint32_t)strtoul(argv[3], NULL, 0);

    if (!strcmp(cmd, "erase")) {
        uint32_t len = (uint32_t)strtoul(argv[4], NULL, 0);
        int fd = open(dev, O_RDWR);
        if (fd < 0) { perror("open"); return 1; }
        if (erase_range(fd, off, len)) return 1;
        fprintf(stderr, "erase OK\n");
        return 0;
    }

    if (!strcmp(cmd, "write")) {
        const char *path = argv[4];
        struct stat st;
        if (stat(path, &st)) { perror("stat"); return 1; }
        uint32_t fsz = (uint32_t)st.st_size;
        int fd = open(dev, O_RDWR);
        if (fd < 0) { perror("open"); return 1; }
        struct mtd_info_user mi;
        if (ioctl(fd, MEMGETINFO, &mi)) { perror("MEMGETINFO"); return 1; }
        uint32_t blk = mi.erasesize;

        /* 1) 统计需要擦除的物理块数（含坏块：坏块跳过但仍占 flash 空间） */
        uint32_t nblk = 0, cover = 0, fpos = off, badcnt = 0;
        while (cover < fsz) {
            long long bpos = (long long)fpos;
            int bad = ioctl(fd, MEMGETBADBLOCK, &bpos);
            if (bad <= 0) cover += blk;
            else { badcnt++; fprintf(stderr, "write: BAD block @0x%x (skip)\n", fpos); }
            fpos += blk;
            nblk++;
        }
        fprintf(stderr, "write: %u bytes, %u blocks (%u bad) @0x%x\n", fsz, nblk, badcnt, off);
        if (erase_range(fd, off, nblk * blk)) return 1;

        /* 2) 逐块写入：坏块跳过（flash 位置前进，文件数据不消耗） */
        int fin = open(path, O_RDONLY);
        if (fin < 0) { perror("open src"); return 1; }
        static unsigned char buf[CHUNK];
        uint32_t done = 0;
        fpos = off;
        while (done < fsz) {
            long long bpos = (long long)fpos;
            if (ioctl(fd, MEMGETBADBLOCK, &bpos) > 0) { fpos += blk; continue; }
            uint32_t want = fsz - done;
            if (want > blk) want = blk;
            uint32_t got = 0;
            while (got < want) {
                ssize_t r = read(fin, buf, want - got);
                if (r <= 0) { perror("read src"); return 1; }
                got += r;
            }
            if (lseek(fd, fpos, SEEK_SET) < 0) { perror("lseek"); return 1; }
            uint32_t w = 0;
            while (w < want) {
                ssize_t k = write(fd, buf + w, want - w);
                if (k <= 0) { perror("write mtd"); return 1; }
                w += k;
            }
            done += want;
            fpos += blk;
        }
        fprintf(stderr, "write OK: %u bytes @0x%x -> %s\n", fsz, off, dev);
        return 0;
    }

    if (!strcmp(cmd, "read")) {
        if (argc < 6) { goto usage_err; }
        uint32_t len = (uint32_t)strtoul(argv[4], NULL, 0);
        const char *path = argv[5];
        int fd = open(dev, O_RDONLY);
        if (fd < 0) { perror("open"); return 1; }
        if (lseek(fd, off, SEEK_SET) < 0) { perror("lseek"); return 1; }
        int fout = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fout < 0) { perror("open dst"); return 1; }
        static unsigned char buf[CHUNK];
        uint32_t done = 0;
        while (done < len) {
            uint32_t want = (len - done) > CHUNK ? CHUNK : (len - done);
            ssize_t r = read(fd, buf, want);
            if (r <= 0) { perror("read mtd"); return 1; }
            ssize_t w = 0;
            while (w < r) {
                ssize_t k = write(fout, buf + w, r - w);
                if (k <= 0) { perror("write dst"); return 1; }
                w += k;
            }
            done += r;
        }
        fprintf(stderr, "read OK: %u bytes @0x%x -> %s\n", len, off, path);
        return 0;
    }

usage_err:
    fprintf(stderr,
        "usage:\n"
        "  %s info  <dev>\n"
        "  %s erase <dev> <off> <len>\n"
        "  %s write <dev> <off> <file>\n"
        "  %s read  <dev> <off> <len> <file>\n", argv[0], argv[0], argv[0], argv[0]);
    return 1;
}
