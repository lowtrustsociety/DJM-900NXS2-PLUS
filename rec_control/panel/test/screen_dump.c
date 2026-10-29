/* Simulator program that draws the panel's screens.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../src/panel.h"

#define SIM_PUTC(c)  (RAM8(0x401) = (u8)(c))
extern void call_stock_frame(void);

static void puthex(u8 v)
{
    static const char hex[] = "0123456789abcdef";
    SIM_PUTC(hex[v >> 4]);
    SIM_PUTC(hex[v & 15]);
}


static void pump(u8 frame)
{
    u8 d, s, i;
    u16 a;

    for (d = 0; d < 2; d++) {
        for (s = 0; s < 31; s++) {
            a = QUEUE_BASE + (u16)d * QUEUE_DISP_SIZE + (u16)s * 9;
            if (!RAM8(a))
                continue;
            SIM_PUTC('Q'); SIM_PUTC(' ');
            puthex(frame); SIM_PUTC(' ');
            puthex(d); SIM_PUTC(' ');
            puthex(s); SIM_PUTC(' ');
            for (i = 1; i < 9; i++)
                puthex(RAM8(a + i));
            SIM_PUTC('\n');
            RAM8(a) = 0;
        }
    }
}

#ifndef FX_TYPE
#define FX_TYPE 3
#endif
#ifndef FRAMES
#define FRAMES 12
#endif

void harness(void)
{
    u16 i;
    u8 f;

    for (i = 0; i < FRAME_LEN; i++) {
        RAM8(RX_BUF_BASE + i) = 0;
        RAM8(RX_BUF_BASE + FRAME_LEN + i) = 0;
    }
    RX_IDX = 0;
    DISP_IMMEDIATE = 0;
    for (f = 0; f < FRAMES; f++) {
        RAM8(RX_BUF_BASE + 0x00) = 0x01;
        RAM8(RX_BUF_BASE + 0x01) = 0xAA;
        RAM8(RX_BUF_BASE + 0x2C) = FX_TYPE;
#ifdef SET
        SET
#endif
        DISP_STAGE = 0;
        call_stock_frame();
        pump(f);
    }
    SIM_PUTC('D'); SIM_PUTC('\n');
}
