/*
 * main.c
 * SD card emulation
 *
 * Copyright (C) 2017 Alexander Graf <agraf@suse.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "qemu/osdep.h"
#include "qemu-common.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "uio.h"
#include "gpio.h"
#include "fast.h"
#include "crc16.h"
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/time.h>

#define SECTOR_SIZE 512

static void *sdcard_map;
static void *sdctl_map;

int fd;

static void benchmark_sdctl(void)
{
    void *sdctl_map;
    int i;
    struct timeval tv_before;
    struct timeval tv_after;
    uint64_t usec_before;
    uint64_t usec_after;

    gpio_set_reset(0);
    sdctl_map = uio_map(3);

    gettimeofday(&tv_before, NULL);
    for (i = 0; i < 1000000; i++) {
        readl((uint32_t*)sdctl_map);
        readl((uint32_t*)(sdctl_map + 4));
    }
    gettimeofday(&tv_after, NULL);

    usec_before = tv_before.tv_sec * 1000000ULL + tv_before.tv_usec;
    usec_after = tv_after.tv_sec * 1000000ULL + tv_after.tv_usec;
    printf("Benchmarked SDCTL: %lld usec for 2000000 reads\n", (long long)(usec_after - usec_before));
}

static int open_file(const char *filename)
{
    fd = open(filename, O_RDWR);
    if (fd <= 0) {
        printf("Unable to open '%s'\n", filename);
        return -1;
    }
    return 0;
}

static int get_size(volatile struct fast_queue_elem *el)
{
    uint64_t size;
    int r;

    r = ioctl(fd, BLKGETSIZE64, &size);
    if (r) {
        struct stat st;

        /* BLKGETSIZE64 not available, use fstat */
        r = fstat(fd, &st);
        if (r) {
            return r;
        }
        size = st.st_size;
    }

    *(uint64_t*)el->ptr = size;
    return 0;
}

static void read_sector(uint64_t sector, uint32_t *ptr)
{
    char buf[SECTOR_SIZE + 8];
    int r, fr = 0, i;
    uint16_t crc16 = 0;

    printf("Reading sector %lld\n", (long long)sector);

    lseek(fd, sector * SECTOR_SIZE, SEEK_SET);
    while (fr != SECTOR_SIZE) {
        r = read(fd, &buf[fr], SECTOR_SIZE - fr);
        if (r <= 0) {
            printf("Read error: %s\n", strerror(errno));
            return;
        }
        fr += r;
    }

    if (0) {
        crc16 = crc16ccitt_xmodem((uint8_t *)buf, SECTOR_SIZE);
        buf[SECTOR_SIZE] = crc16 >> 8;
        buf[SECTOR_SIZE + 1] = crc16 & 0xff;
    }
    printf("Writing sector data to %p (crc16 = %x)\n", ptr, crc16);
    for (i = 0; i < (sizeof(buf) / 4); i++) {
        uint32_t *buf32 = (uint32_t *)buf;
        writel(buf32[i], &ptr[i]);
    }
    //memcpy(ptr, buf, sizeof(buf));
}

static void write_sector(uint64_t sector, uint32_t *ptr)
{
    char buf[SECTOR_SIZE];
    int r, fr = 0, i;

    printf("Writing sector %lld\n", (long long)sector);

    for (i = 0; i < (sizeof(buf) / 4); i++) {
        uint32_t *buf32 = (uint32_t *)buf;
        buf32[i] = readl(&ptr[i]);
    }

    lseek(fd, sector * SECTOR_SIZE, SEEK_SET);
    while (fr != SECTOR_SIZE) {
        r = write(fd, &buf[fr], SECTOR_SIZE - fr);
        if (r <= 0) {
            printf("Write error: %s\n", strerror(errno));
            return;
        }
        fr += r;
    }
}

static void axi_bench(void)
{
    int i;
    struct timeval tv_before;
    struct timeval tv_after;
    uint64_t usec_before;
    uint64_t usec_after;

    gettimeofday(&tv_before, NULL);
    for (i = 0; i < 100000; i++) {
        writel(0xdeadbeef, sdcard_map);
    }
    gettimeofday(&tv_after, NULL);

    usec_before = tv_before.tv_sec * 1000000ULL + tv_before.tv_usec;
    usec_after = tv_after.tv_sec * 1000000ULL + tv_after.tv_usec;
    printf("Benchmarked AXI: %lld usec for 100000 reads\n", (long long)(usec_after - usec_before));
}

int main(int argc, char **argv)
{
    if (uio_init()) {
        printf("Failed to open UIO device\n");
        return 1;
    }

    if (gpio_init()) {
        printf("Failed to initialize GPIO device\n");
        return 1;
    }

    if (!(sdcard_map = uio_map(0))) {
        return 1;
    }

    if (!(sdctl_map = uio_map(1))) {
        return 1;
    }

    axi_bench();
    benchmark_sdctl();

    if (fast_init())
        return 1;

    if (open_file(argv[1])) {
        printf("Unable to open file '%s'\n", argv[1]);
        return 1;
    }

    printf("Starting SD Card emulation ...\n");

    while (1) {
        volatile struct fast_queue_elem *el = fast_next();

        if (!el) {
            usleep(1000);
            continue;
        }

//        printf("New CMD: %02x (%c) len=%d\n", msg->cmd, msg->cmd, r);
        switch (el->cmd) {
        case SDCARD_MSG_DBG: {
            printf("[dbg] %s", (char*)el->ptr);
            break;
        }
        case SDCARD_MSG_DBG_INT: {
            printf("[dbg] %s%#"PRIx64"\n", (char*)el->ptr, el->extra);
            break;
        }
        case SDCARD_MSG_GET_SIZE:
            get_size(el);
            break;
        case SDCARD_MSG_READ_SECTOR: {
            read_sector(el->extra, el->ptr);
            break;
        }
        case SDCARD_MSG_WRITE_SECTOR: {
            write_sector(el->extra, el->ptr);
            break;
        }
        default:
            printf("XXX Unknown CMD %x (%c)\n", el->cmd, el->cmd);
            break;
        }

        fast_done(el);
    }
}
