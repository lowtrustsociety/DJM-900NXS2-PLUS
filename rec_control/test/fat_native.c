/* Host test program for fat.c.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/recfile.h"

#define MAX_EXTENTS 1024u
#define RATE 192000u

static int image = -1;
static long write_limit = -1, writes_done, reads_done, steps_done;
static u32 ahead_bytes;
static int no_audio;
static u8 dirbuf[512], fatbuf[FR_FAT_BATCH * 512u], header[512], idle[512];
static struct extent map_a[MAX_EXTENTS], map_b[MAX_EXTENTS];
static struct rf r;

u8 *fr_dirbuf(void) { return dirbuf; }
u8 *fr_fatbuf(void) { return fatbuf; }

int fr_read(u32 sector, u8 *buffer, u32 count)
{
    reads_done++;
    return pread(image, buffer, (size_t)count * 512u, (off_t)sector * 512) ==
           (ssize_t)((size_t)count * 512u) ? 0 : -1;
}

int fr_write(u32 sector, const u8 *buffer, u32 count)
{
    if (write_limit >= 0 && writes_done >= write_limit) {
        printf("POWER CUT before sector write %ld (sector %u, %u sectors)\n",
               writes_done + 1, sector, count);
        fflush(stdout);
        exit(3);
    }
    writes_done++;
    return pwrite(image, buffer, (size_t)count * 512u, (off_t)sector * 512) ==
           (ssize_t)((size_t)count * 512u) ? 0 : -1;
}


static int raw_write(u32 sector, const u8 *buffer)
{
    return pwrite(image, buffer, 512u, (off_t)sector * 512) == 512 ? 0 : -1;
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void build_header(u8 *h, u32 state, u32 data_bytes)
{
    memset(h, 0, 512);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 504u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16u);
    h[20] = 1; h[22] = 2;
    put32(h + 24, 48000u);
    put32(h + 28, RATE);
    h[32] = 4; h[34] = 16;
    memcpy(h + 36, "JUNK", 4);
    put32(h + 40, 460u);
    memcpy(h + 44, "DJMREC43", 8);
    put32(h + 60, state);
    memcpy(h + 504, "data", 4);
    put32(h + 508, data_bytes);
}

const u8 *rf_idle_header(const struct rf *x) { (void)x; build_header(idle, 0u, 0u); return idle; }

u32 rf_header_kind(const u8 *s, u32 *data_bytes)
{
    if (memcmp(s, "RIFF", 4) || memcmp(s + 8, "WAVE", 4) || memcmp(s + 44, "DJMREC43", 8))
        return RF_HEADER_NONE;
    *data_bytes = get32(s + 508);
    switch (get32(s + 60)) {
    case 0u: return RF_HEADER_IDLE;
    case 1u: return RF_HEADER_RUNNING;
    default: return RF_HEADER_COMPLETE;
    }
}

void rf_close_header(u8 *s) { put32(s + 60, 2u); }
u32 rf_header_file_bytes(const u8 *s) { return get32(s + 4) + 8u; }
u32 rf_recover_tail(struct rf *x, u8 *header) { (void)x; (void)header; return 0u; }

static const char *error_name(int e)
{
    switch (e) {
    case FR_E_IO: return "IO";
    case FR_E_NOT_FAT32: return "NOT_FAT32";
    case FR_E_DIR: return "DIR";
    case FR_E_NO_SPACE: return "NO_SPACE";
    case FR_E_CHAIN: return "CHAIN";
    case FR_E_CHANGED: return "CHANGED";
    case FR_E_VERIFY: return "VERIFY";
    default: return "?";
    }
}

static int run_steps(const char *what)
{
    int result;
    while ((result = rf_step(&r)) == FR_BUSY)
        steps_done++;
    if (result != FR_OK) {
        printf("%s: ERROR %s in phase %u\n", what, error_name(result), r.error_phase);
        return result;
    }
    printf("%s: waiting file %.8s.%.3s, %u clusters (%u bytes) in %u pieces, first cluster %u; "
           "made %u reused %u refilled %u recovered %u cuts %u switched %u; next number %u\n",
           what, r.cur.name, r.cur.name + 8, r.cur.clusters, r.cur.size, r.cur.extent_count,
           r.cur.first_cluster, r.made, r.reused, r.refilled, r.recovered, r.cuts, r.switched,
           r.next_number);
    return FR_OK;
}


static u32 file_sector(u32 index)
{
    u32 cluster = index / r.vol.spc, e;
    for (e = 0u; e < r.cur.extent_count; e++) {
        if (cluster < r.cur.extents[e].clusters)
            return r.cur.extents[e].first_sector + cluster * r.vol.spc + index % r.vol.spc;
        cluster -= r.cur.extents[e].clusters;
    }
    return 0u;
}


static int take(char kind, u32 seconds)
{
    u32 total = (u32)((unsigned long long)seconds * RATE / 512u), n, tag, file_pos = 0u, i;
    u8 sector[512];
    int result = rf_revalidate(&r);

    if (result != FR_OK) {
        printf("take: revalidation failed: %s\n", error_name(result));
        return result;
    }
    tag = r.next_number;
    build_header(header, 1u, 0u);
    if (raw_write(file_sector(0u), header)) return FR_E_IO;
    for (n = 0u; n < total; n++) {
        if (512u + (file_pos + 1u) * 512u > r.cur.size) {
            if (r.next != RF_NEXT_READY)
                break;
            build_header(header, 2u, file_pos * 512u);
            if (raw_write(file_sector(0u), header)) return FR_E_IO;
            printf("file %u full after %u s, on into the next\n", tag, file_pos * 512u / RATE);
            result = rf_switch(&r);
            if (result == FR_OK) {
                tag = r.next_number;
                result = rf_commit_name(&r);
            }
            if (result != FR_OK) {
                printf("take: file change failed: %s\n", error_name(result));
                return result;
            }
            build_header(header, 1u, 0u);
            if (raw_write(file_sector(0u), header)) return FR_E_IO;
            file_pos = 0u;
        }
        if (!no_audio) {
            for (i = 0u; i < 512u; i += 8u) {
                put32(sector + i, tag);
                put32(sector + i + 4u, file_pos * 512u + i);
            }
            if (raw_write(file_sector(1u + file_pos), sector)) return FR_E_IO;
        } else if (file_sector(1u + file_pos) == 0u) {
            printf("take: no sector for file position %u\n", file_pos);
            return FR_E_CHAIN;
        }
        file_pos++;
        if (n == 60u * RATE / 512u && kind != 's' && !r.named) {
            result = rf_commit_name(&r);
            if (result != FR_OK) {
                printf("take: naming failed: %s\n", error_name(result));
                return result;
            }
        }
        if (file_pos % (15u * RATE / 512u) == 0u) {
            build_header(header, 1u, file_pos * 512u);
            if (raw_write(file_sector(0u), header)) return FR_E_IO;
        }
        if (r.named && r.next == RF_NEXT_OFF && ahead_bytes != 0u &&
            r.cur.size - 512u - file_pos * 512u < ahead_bytes)
            rf_next_begin(&r);
        if (r.next > RF_NEXT_OFF && r.next < RF_NEXT_READY && n % 16u == 0u) {
            result = rf_next_step(&r);
            if (result != FR_BUSY)
                printf("next file: %s\n", result == FR_OK ? "ready" : error_name(result));
        }
    }
    if (kind == 'c') {
        printf("cut: power lost %u s into a take (file %.8s.%.3s, next file state %u)\n",
               seconds, r.cur.name, r.cur.name + 8, r.next);
        fflush(stdout);
        exit(0);
    }
    if (kind == 's') {
        if (raw_write(file_sector(0u), rf_idle_header(&r))) return FR_E_IO;
        printf("short: %u s recorded and dropped\n", seconds);
        return FR_OK;
    }
    while (r.next > RF_NEXT_OFF && r.next < RF_NEXT_READY && rf_next_step(&r) == FR_BUSY)
        ;
    build_header(header, 2u, file_pos * 512u);
    if (raw_write(file_sector(0u), header)) return FR_E_IO;
    rf_begin_finish(&r, 512u + file_pos * 512u);
    printf("take: ended in file %u after %u s there\n", tag, file_pos * 512u / RATE);
    return run_steps("after take");
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: fat_native IMAGE [options] COMMAND...\n");
        return 2;
    }
    image = open(argv[1], O_RDWR);
    if (image < 0) { perror(argv[1]); return 2; }
    r.cap_bytes = 2073666048u;
    r.min_bytes = 64u << 20;
    r.refill_below_bytes = 345600000u;
    r.margin_bytes = 32u << 20;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--cap")) r.cap_bytes = (u32)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--min")) r.min_bytes = (u32)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--refill")) r.refill_below_bytes = (u32)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--margin")) r.margin_bytes = (u32)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--limit")) write_limit = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--ahead")) ahead_bytes = (u32)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--noaudio")) no_audio = 1;
        else if (!strcmp(argv[i], "boot")) {
            u32 cap = r.cap_bytes, min = r.min_bytes, refill = r.refill_below_bytes, margin = r.margin_bytes;
            memset(&r, 0, sizeof(r));
            r.cap_bytes = cap; r.min_bytes = min; r.refill_below_bytes = refill; r.margin_bytes = margin;
            r.cur.extents = map_a; r.cur.max_extents = MAX_EXTENTS;
            r.other.extents = map_b; r.other.max_extents = MAX_EXTENTS;
            rf_begin_prepare(&r);
            if (run_steps("boot") != FR_OK) break;
        } else if (strchr("tsc", argv[i][0]) && strchr(argv[i], ':')) {
            if (take(argv[i][0], (u32)strtoul(strchr(argv[i], ':') + 1, 0, 0)) != FR_OK) break;
        } else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    printf("io: %ld reads, %ld writes, %ld steps\n", reads_done, writes_done, steps_done);
    return i < argc ? 1 : 0;
}
