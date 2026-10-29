/* Lossless packing of the audio in the ring.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef CONTINUOUS_PACK_H
#define CONTINUOUS_PACK_H


#include "fat.h"

#define PACK_PLAIN      31u
#define PACK_MAX_BYTES  74u

struct pack_state {
    s32 x1[2];
    s32 x2[2];
};


u32 pack_block(const s32 *in, u8 *out, struct pack_state *state);


u32 unpack_block(const u8 *in, u8 *out, struct pack_state *state);

#endif
