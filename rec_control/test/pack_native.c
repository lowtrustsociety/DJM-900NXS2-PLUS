/* Host test program for pack.c.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pack.h"

static unsigned long long plain, packed;
static unsigned widths[32], largest, wrong;

static void run(const s32 *samples, size_t blocks, unsigned page_blocks)
{
    struct pack_state enc = { {0, 0}, {0, 0} }, dec = { {0, 0}, {0, 0} };
    u8 buffer[96], back[72];
    size_t b;
    unsigned i;

    for (b = 0; b < blocks; b++) {
        const s32 *in = samples + b * 24;
        u32 n, m;

        if (page_blocks && b % page_blocks == 0) {
            memset(&enc, 0, sizeof enc);
            memset(&dec, 0, sizeof dec);
        }
        memset(buffer, 0xA5, sizeof buffer);
        n = pack_block(in, buffer, &enc);
        if (n > PACK_MAX_BYTES || buffer[n] != 0xA5 || buffer[0] > 31 || buffer[1] > 31) { wrong++; continue; }
        if (n > largest) largest = n;
        widths[buffer[0]]++; widths[buffer[1]]++;
        m = unpack_block(buffer, back, &dec);
        if (m != n) wrong++;
        for (i = 0; i < 24; i++) {
            s32 v = (s32)((u32)back[i * 3] << 8 | (u32)back[i * 3 + 1] << 16 | (u32)back[i * 3 + 2] << 24) >> 8;
            if (v != in[i]) wrong++;
        }
        plain += 72; packed += n;
    }
}

int main(int argc, char **argv)
{
    static s32 generated[24 * 4096];
    unsigned shift, i, seed = 1;


    for (shift = 0; shift <= 24; shift++) {
        for (i = 0; i < 24 * 4096; i++) {
            seed = seed * 1664525u + 1013904223u;
            generated[i] = shift == 0 ? 0 : (s32)((int)(seed >> 8) - 8388608) >> (24 - shift);
            if (i % 5000 == 0) generated[i] = 8388607;
            if (i % 7000 == 0) generated[i] = -8388608;
        }
        run(generated, 4096, shift % 3 == 0 ? 0 : 311);
    }
    if (argc > 2) {

        FILE *f = fopen(argv[1], "rb");
        unsigned bytes = (unsigned)atoi(argv[2]);
        static unsigned char data[311 * 72];
        static s32 samples[311 * 24];
        size_t got, count, k;
        if (!f) { perror(argv[1]); return 2; }
        if (bytes != 2 && bytes != 3) return 2;
        plain = packed = 0;
        while ((got = fread(data, 1, 311 * 24 * bytes, f)) >= 24 * bytes) {
            count = got / bytes / 24 * 24;
            for (k = 0; k < count; k++) {
                const unsigned char *s = data + k * bytes;
                if (bytes == 3) {
                    samples[k] = (s32)((u32)s[0] << 8 | (u32)s[1] << 16 | (u32)s[2] << 24) >> 8;
                } else {
                    seed = seed * 1664525u + 1013904223u;
                    samples[k] = ((s32)((u32)s[0] << 16 | (u32)s[1] << 24) >> 8) | (s32)(seed >> 24);
                }
            }
            run(samples, count / 24, 311);
        }
        fclose(f);
        printf("file: %.3f of plain\n", (double)packed / (double)plain);
    }
    printf("widths used:");
    for (i = 0; i < 32; i++) if (i < 24 || i == PACK_PLAIN) printf(" %u:%u", i, widths[i]);
    printf("\nlargest block %u bytes, wrong %u\n", largest, wrong);
    return wrong != 0;
}
