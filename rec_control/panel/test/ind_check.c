/* Simulator harness for the status indicator.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "panel.h"
#include "../src/recstat.h"

#define SIM_PUTC(c)  (RAM8(0x401) = (u8)(c))
extern void call_hook(void);
extern void call_init(void);
extern void call_parse(void);

static u16 frame_number;
static u8 counter;
static u8 why;
static u8 left, left_sec;

static void puts_(const char *s)
{
    while (*s)
        SIM_PUTC(*s++);
}

static void puthex(u8 v)
{
    static const char hex[] = "0123456789abcdef";
    SIM_PUTC(hex[v >> 4]);
    SIM_PUTC(hex[v & 15]);
}

static void pump(void)
{
    u8 d, s, i;
    u16 a, p;

    for (d = 0; d < 2; d++) {
        for (s = 0; s < 31; s++) {
            a = QUEUE_BASE + (u16)d * QUEUE_DISP_SIZE + (u16)s * 9;
            if (!RAM8(a))
                continue;
            SIM_PUTC('Q'); SIM_PUTC(' ');
            puthex((u8)(frame_number >> 8)); puthex((u8)frame_number); SIM_PUTC(' ');
            puthex(d); SIM_PUTC(' ');
            puthex(s); SIM_PUTC(' ');
            for (i = 1; i < 9; i++)
                puthex(RAM8(a + i));
            if (RAM16(a + 3) == 0) {
                p = RAM16(a + 1);
                SIM_PUTC(' ');
                for (i = 0; i < 2 + IND_BYTES; i++)
                    puthex(RAM8(p + i));
            }
            SIM_PUTC('\n');
            RAM8(a) = 0;
        }
    }
}


static void frame(u8 mode, const u8 *status)
{
    u8 i, passes;
    u16 base = RX_BUF_BASE;

    for (i = 0; i < FRAME_LEN; i++)
        RAM8(base + i) = 0;
    RAM8(base + 0x00) = 0x01;
    RAM8(base + 0x01) = 0xAA;
    RAM8(base + 0x2C) = mode;
    RAM8(base + 0x2D) = 1;
    RAM8(base + 0x2E) = 0x00;
    RAM8(base + 0x2F) = 0x05;
    RAM8(base + 0x30) = 4;
    RAM8(base + 0x34) = (u8)(frame_number >> 3);
    if (status)
        for (i = 0; i < STATUS_BYTES; i++)
            RAM8(base + STATUS_AT + i) = status[i];
    DISP_STAGE = 0;
    call_parse();
    for (passes = 0; passes < 6 && DISP_STAGE == 1; passes++)
        call_hook();


    if (DISP_STAGE == 2) {
        u8 s, any = 0;
        for (s = 0; s < 31; s++)
            any |= RAM8(QUEUE_BASE + (u16)s * 9);
        if (!any)
            DISP_STAGE = 3;
    }
    call_hook();
    pump();
    DISP_STAGE = 0;
    call_hook();
    frame_number++;
}

static void status_make(u8 *s, u8 state, u8 flags, u8 h, u8 m, u8 sec)
{
    u8 i, sum = 0;

    for (i = 0; i < STATUS_BYTES; i++)
        s[i] = 0;
    s[0] = STATUS_MAGIC;
    s[1] = STATUS_FORMAT;
    s[2] = counter;
    s[3] = state;
    s[4] = flags;
    s[5] = h;
    s[6] = m;
    s[7] = sec;
    s[8] = 3;
    s[10] = 15;
    s[16] = why;
    s[18] = left;
    s[19] = left_sec;
    for (i = 0; i < STATUS_BYTES - 1; i++)
        sum += s[i];
    s[STATUS_BYTES - 1] = (u8)~sum;
}


static void run(const char *tag, u16 n, u8 mode, u8 live, u8 spoil,
                u8 state, u8 flags, u8 h, u8 m, u8 sec)
{
    u8 s[STATUS_BYTES];
    u16 i;

    puts_("T ");
    puts_(tag);
    SIM_PUTC(' ');
    puthex((u8)(frame_number >> 8)); puthex((u8)frame_number);
    SIM_PUTC('\n');
    for (i = 0; i < n; i++) {
        if (live && i % 25 == 0)
            counter++;
        status_make(s, state, flags, h, m, sec);
        if (spoil == 1)
            s[STATUS_BYTES - 1] ^= 0x10;
        if (spoil == 2)
            s[1] = 1;
        frame(mode, spoil == 3 ? (const u8 *)0 : s);
    }
}

static void dump_ram(void)
{
    u16 a;

    puts_("RAM ");
    for (a = 0x0404; a < 0x5400; a++) {
        if (a >= 0x2000 && a < 0x4400)
            a = 0x4400;
        if (a >= 0x4E00 && a < 0x4F00)
            a = 0x4F00;
        puthex(RAM8(a));
    }
    SIM_PUTC('\n');
}

void harness(void)
{
    frame_number = 0;
    counter = 0;
#ifdef NO_INIT

    DISP_IMMEDIATE = 0;
    run("noinit", 1700, 3, 1, 0, 2, 0, 0, 1, 2);
#else
    call_init();
    DISP_IMMEDIATE = 0;
    run("window", 29, 3, 1, 0, 2, 0, 0, 1, 2);
    run("ready", 60, 3, 1, 0, 1, 0, 0, 0, 0);
    run("rec7", 60, 3, 1, 0, 2, 0, 0, 0, 7);
    run("rec0959", 60, 3, 1, 0, 2, 0, 0, 9, 59);
    run("rec5959", 30, 3, 1, 0, 2, 0, 0, 59, 59);
    run("rec1h", 30, 3, 1, 0, 2, 0, 1, 0, 0);
    run("rec12h", 30, 3, 1, 0, 2, 0, 12, 34, 56);
    run("warn", 30, 3, 1, 0, 2, 4, 12, 34, 57);
    run("badsum", 30, 3, 1, 1, 2, 0, 1, 1, 1);
    run("badformat", 30, 3, 1, 2, 2, 0, 1, 1, 1);
    run("back", 30, 3, 1, 0, 2, 4, 12, 34, 58);
    run("stale", 320, 3, 0, 0, 2, 4, 12, 34, 58);
    run("stillstale", 320, 3, 0, 0, 2, 4, 12, 34, 58);
    run("again", 30, 3, 1, 0, 2, 0, 0, 0, 1);
    run("utility", 40, 0x80, 1, 0, 2, 0, 0, 0, 2);
    run("normal", 40, 3, 1, 0, 2, 0, 0, 0, 3);
    run("normal2", 40, 3, 1, 0, 3, 0, 0, 0, 5);
    run("notready", 40, 3, 1, 0, 0, 0, 0, 0, 0);
    run("nostatus", 40, 3, 0, 3, 0, 0, 0, 0, 0);
    run("ready2", 40, 3, 1, 0, 1, 0, 0, 0, 0);
    run("fx", 40, 7, 1, 0, 1, 0, 0, 0, 0);
    run("logo", 40, 0xF8, 1, 0, 2, 0, 0, 0, 9);
    run("afterlogo", 40, 7, 1, 0, 2, 0, 0, 0, 10);
    run("quiet", 40, 7, 0, 3, 0, 0, 0, 0, 0);
    run("quiet2", 300, 7, 0, 3, 0, 0, 0, 0, 0);
    run("utility2", 10, 0x80, 0, 3, 0, 0, 0, 0, 0);
    run("normal3", 10, 7, 0, 3, 0, 0, 0, 0, 0);


    why = 4;
    run("full", 40, 7, 1, 0, 0, 8, 0, 0, 0);
    why = 5;
    run("error", 40, 7, 1, 0, 0, 8, 0, 0, 0);
    why = 2;
    run("nousb", 40, 7, 1, 0, 0, 8, 0, 0, 0);
    why = 1;
    run("looking", 40, 7, 1, 0, 0, 8, 0, 0, 0);
    why = 0;
    run("readyagain", 40, 7, 1, 0, 1, 8, 0, 0, 0);
    run("newtake", 40, 7, 1, 0, 2, 0, 0, 0, 1);
    why = 4;
    run("fullnoflag", 40, 7, 1, 0, 0, 0, 0, 0, 0);
    why = 0;
    run("ready3", 40, 7, 1, 0, 1, 0, 0, 0, 0);


    run("blink", 210, 7, 1, 0, 2, 0, 0, 0, 2);
    left = 30; left_sec = 58;
    run("count", 110, 7, 1, 0, 2, 0, 1, 31, 2);
    left = 60; left_sec = 59;
    run("count59", 40, 7, 1, 0, 2, 0, 1, 1, 0);
    left = 1; left_sec = 7;
    run("count7", 40, 7, 1, 0, 2, 4, 1, 59, 53);
    left = 61;
    run("countbad", 30, 7, 1, 0, 2, 0, 0, 0, 1);
    left = 0; left_sec = 0;
    run("countoff", 40, 7, 1, 0, 2, 0, 1, 59, 55);

    run("text", 20, 0xFF, 1, 0, 2, 0, 0, 0, 4);
    run("aftertext", 80, 7, 1, 0, 2, 0, 0, 0, 30);
    run("utility3", 20, 0x80, 1, 0, 2, 0, 0, 0, 31);
    run("normal4", 40, 7, 1, 0, 1, 0, 0, 0, 0);
#endif
    dump_ram();
    puts_("DONE\n");
}
