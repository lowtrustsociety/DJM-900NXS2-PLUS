/* Lossless packing of the audio in the ring.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "pack.h"

u32 pack_block(const s32 *in, u8 *out, struct pack_state *state)
{
    s32 residual[24];
    u8 *p = out + 2;
    u32 width[2];
    u32 channel, index, acc, count;

    for (channel = 0u; channel < 2u; channel++) {
        s32 x1 = state->x1[channel], x2 = state->x2[channel];
        u32 fold = 0u, any = 0u, w;

        for (index = channel; index < 24u; index += 2u) {
            s32 x = in[index];
            s32 d = x - 2 * x1 + x2;

            x2 = x1;
            x1 = x;
            residual[index] = d;
            fold |= (u32)(d ^ (d >> 31));
            any |= (u32)d;
        }
        state->x1[channel] = x1;
        state->x2[channel] = x2;
        if (any == 0u) {
            w = 0u;
        } else if (fold >= 0x400000u) {
            w = PACK_PLAIN;
        } else {
            w = 1u;
            if (fold >= 0x1000u) {
                fold >>= 12;
                w += 12u;
            }
            if (fold >= 0x40u) {
                fold >>= 6;
                w += 6u;
            }
            while (fold != 0u) {
                fold >>= 1;
                w++;
            }
        }
        width[channel] = w;
    }
    out[0] = (u8)width[0];
    out[1] = (u8)width[1];
    for (channel = 0u; channel < 2u; channel++) {
        u32 w = width[channel];

        if (w == PACK_PLAIN) {
            for (index = channel; index < 24u; index += 2u) {
                *p++ = (u8)((u32)in[index] >> 16);
                *p++ = (u8)((u32)in[index] >> 8);
                *p++ = (u8)in[index];
            }
        } else if (w != 0u) {
            u32 mask = (1u << w) - 1u;


            acc = 0u;
            count = 0u;
            for (index = channel; index < 24u; index += 2u) {
                acc = acc << w | ((u32)residual[index] & mask);
                count += w;
                if (count >= 8u) {
                    count -= 8u;
                    *p++ = (u8)(acc >> count);
                    if (count >= 8u) {
                        count -= 8u;
                        *p++ = (u8)(acc >> count);
                        if (count >= 8u) {
                            count -= 8u;
                            *p++ = (u8)(acc >> count);
                        }
                    }
                }
            }
            if (count != 0u)
                *p++ = (u8)(acc << (8u - count));
        }
    }
    return (u32)(p - out);
}

u32 unpack_block(const u8 *in, u8 *out, struct pack_state *state)
{
    const u8 *p = in + 2;
    u32 channel, index;

    for (channel = 0u; channel < 2u; channel++) {
        u32 w = in[channel];
        s32 x1 = state->x1[channel], x2 = state->x2[channel];
        u32 acc = 0u, count = 0u;

        for (index = channel; index < 24u; index += 2u) {
            s32 x;

            if (w == PACK_PLAIN) {
                x = (s32)(((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8)) >> 8;
                p += 3;
            } else {
                s32 d = 0;

                if (w != 0u) {
                    while (count < w) {
                        acc = acc << 8 | *p++;
                        count += 8u;
                    }
                    count -= w;
                    d = (s32)(acc << (32u - w - count)) >> (32u - w);
                }
                x = d + 2 * x1 - x2;
            }
            x2 = x1;
            x1 = x;
            out[index * 3u] = (u8)x;
            out[index * 3u + 1u] = (u8)((u32)x >> 8);
            out[index * 3u + 2u] = (u8)((u32)x >> 16);
        }
        state->x1[channel] = x1;
        state->x2[channel] = x2;
    }
    return (u32)(p - in);
}
