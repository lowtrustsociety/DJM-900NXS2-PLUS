/* Simulator test program for pack.c.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../src/pack.h"
extern void sim_write(const char *data, int length);
extern const s32 binary_audio_bin_start[];

static void number(const char *label, u32 v)
{
    char t[11]; int i = 10, n = 0; t[10] = '\n';
    while (label[n]) n++;
    sim_write(label, n);
    do { t[--i] = (char)('0' + v % 10u); v /= 10u; } while (v);
    sim_write(t + i, 11 - i);
}

static u8 packed[96];
static struct pack_state enc = { { 0, 0 }, { 0, 0 } }, dec = { { 0, 0 }, { 0, 0 } };

void harness(void)
{
    u32 b, i, total = 0u, wrong = 0u, largest = 0u, sum = 0u;
    for (b = 0u; b < BLOCKS; b++) {
        const s32 *src = binary_audio_bin_start + b * 24u;
#if MODE == 0
        sum += (u32)src[0];
        (void)i;
#else
        u32 n = pack_block(src, packed, &enc);
        if (n > largest) largest = n;
        total += n;
#if MODE == 2
        {
            u8 back[72];
            u32 m = unpack_block(packed, back, &dec);
            if (m != n) wrong++;
            for (i = 0u; i < 24u; i++)
                if ((s32)((u32)back[i * 3u] << 8 | (u32)back[i * 3u + 1u] << 16 |
                          (u32)back[i * 3u + 2u] << 24) >> 8 != src[i]) wrong++;
        }
#else
        (void)i;
#endif
#endif
    }
    number("bytes ", total);
    number("largest ", largest);
    number("wrong ", wrong);
    number("sum ", sum & 0xFFu);
    (void)packed; (void)enc; (void)dec;
}
