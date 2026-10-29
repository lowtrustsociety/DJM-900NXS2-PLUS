/* Simulator harness for the menu.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "panel.h"
#include "../src/recstat.h"
#include "../src/menu.h"

#define SIM_PUTC(c)  (RAM8(0x401) = (u8)(c))
#define STEP         RAM8(0x43E0)
#define TX           0x3000
extern void call_hook(void);
extern void call_init(void);
extern void call_parse(void);
extern void call_builder(u16 packet);

static u16 frame_number;
static u8 counter;
static u8 open, item, last_d;
static u8 st_state, st_marks, st_file, st_hour, st_min, st_sec;
static u8 st_reason, st_cut;
static u8 st_left, st_left_sec;
static u8 st_rest_h, st_rest_m, st_rest_s;
static u8 st_flags;
static u16 st_space;
static u8 st_none;
static u8 st_frozen;
static u8 quiet;
static u8 combo;

static void puthex(u8 v)
{
    static const char hex[] = "0123456789abcdef";
    SIM_PUTC(hex[v >> 4]);
    SIM_PUTC(hex[v & 15]);
}

static void pump(void)
{
    u8 d, s, i;
    u16 a, p, k;

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
                u16 bytes;
                p = RAM16(a + 1);
                bytes = (u16)(2 + (u16)(RAM8(p) >> 3) * RAM8(p + 1));
                SIM_PUTC(' ');
                for (k = 0; k < bytes; k++)
                    puthex(RAM8(p + k));
            }
            SIM_PUTC('\n');
            RAM8(a) = 0;
        }
    }
}


static void builder_inputs(void)
{
    u16 a;

    for (a = 0x0F96; a < 0x0FD8; a += 2) {
        RAM8(a) = (u8)(a + frame_number);
        RAM8(a + 1) = (u8)(a & 3);
    }
    for (a = 0x1054; a < 0x105B; a++)
        RAM8(a) = (u8)(0x11 * (a & 7));
    for (a = 0x1DD0; a < 0x1DDD; a++)
        RAM8(a) = (u8)a;
    RAM16(0x1060) = 0;
    RAM16(0x0FFA) = 0;
    RAM8(0x1DC9) = 1;
    RAM8(0x1DBE) = 0;
    RAM8(0x1D88) = 0;
    RAM8(0x1D89) = 0x80;
    RAM8(0x1D8A) = 0x80;
}


static void exchange(u8 d, u8 b)
{
    static const u8 picture[13] = { 0, 1, 2, 3, 5, 6, 4, 7, 8, 9, 10, 11, 0x45 };
    u8 i, passes, sum = 0, pressed, status[STATUS_BYTES];
    u16 base = RX_BUF_BASE;


    if (combo)
        b |= 0x02;
    RAM8(KEYS_D_ADDR) = d;
    RAM8(KEYS_B_ADDR) = (u8)(0x60 | b);
    RAM8(0x105C) = (u8)(combo ? 0x55 : 0x15);
    DISP_STAGE = 0;
    call_hook();


    builder_inputs();
    RAM8(KEYS_D_ADDR) = d;
    RAM8(KEYS_B_ADDR) = (u8)(0x60 | b);
    for (i = 0; i < FRAME_LEN; i++)
        RAM8(TX + i) = 0xEE;
    for (i = 0; i <= 6; i++) {
        STEP = i;
        call_builder(TX);
    }
    if (!quiet) {
        SIM_PUTC('X'); SIM_PUTC(' ');
        puthex((u8)(frame_number >> 8)); puthex((u8)frame_number); SIM_PUTC(' ');
        for (i = 0; i < FRAME_LEN; i++)
            puthex(RAM8(TX + i));
        SIM_PUTC(' ');
        puthex(RAM8(KEYS_B_ADDR)); puthex(RAM8(0x105C)); puthex(RAM8(KEYS_D_ADDR));
        SIM_PUTC('\n');
    }


    pressed = RAM8(TX + 0x0D) & (u8)~last_d;
    last_d = RAM8(TX + 0x0D);
    if (pressed & 0x04) {
        open = (u8)!open;
        item = 1;
    } else if (open) {
        if ((pressed & 0x01) && item > 1)
            item--;
        if ((pressed & 0x02) && item < 12)
            item++;
    }


    for (i = 0; i < FRAME_LEN; i++)
        RAM8(base + i) = 0;
    RAM8(base + 0x00) = 0x01;
    RAM8(base + 0x01) = 0xAA;
    if (open) {
        RAM8(base + 0x2C) = 0x80;
        RAM8(base + 0x38) = picture[item];
        RAM8(base + 0x3A) = (u8)(item > 1);
        RAM8(base + 0x3B) = (u8)(item < 12);
        RAM8(base + 0x3C) = 1;
    } else {
        RAM8(base + 0x2C) = 3;
        RAM8(base + 0x2D) = 1;
        RAM8(base + 0x2F) = 0x05;
        RAM8(base + 0x30) = 4;
    }
    if (frame_number % 25 == 0 && !st_frozen)
        counter++;
    for (i = 0; i < STATUS_BYTES; i++)
        status[i] = 0;
    status[0] = STATUS_MAGIC;
    status[1] = STATUS_FORMAT;
    status[2] = counter;
    status[3] = st_state;
    status[4] = st_flags;
    status[5] = st_hour;
    status[6] = st_min;
    status[7] = st_sec;
    status[8] = st_marks;
    status[9] = (u8)(st_space >> 8);
    status[10] = (u8)st_space;
    status[15] = st_file;
    status[16] = st_reason;
    status[17] = st_cut;
    status[18] = st_left;
    status[19] = st_left_sec;
    status[20] = st_rest_h;
    status[21] = st_rest_m;
    status[22] = st_rest_s;
    for (i = 0; i < STATUS_BYTES - 1; i++)
        sum += status[i];
    status[STATUS_BYTES - 1] = (u8)~sum;
    if (!st_none) {
        for (i = 0; i < STATUS_BYTES; i++)
            RAM8(base + STATUS_AT + i) = status[i];
    }


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

static void tag(u8 number)
{
    SIM_PUTC('T'); SIM_PUTC(' ');
    puthex(number); SIM_PUTC(' ');
    puthex((u8)(frame_number >> 8)); puthex((u8)frame_number);
    SIM_PUTC('\n');
}


static void press(u8 number, u8 d, u8 b)
{
    u8 i;

    for (i = 0; i < 8; i++)
        exchange(d, b);
    for (i = 0; i < 12; i++)
        exchange(0, 0);
    tag(number);
}

static void idle(u8 number, u8 frames)
{
    u8 i;

    for (i = 0; i < frames; i++)
        exchange(0, 0);
    tag(number);
}

#define QUANTIZE 0x04, 0
#define PREV     KEY_PREV, 0
#define NEXT     KEY_NEXT, 0
#define TAP      KEY_ENTER, 0
#define AUTOTAP  0, KEY_BACK

void harness(void)
{
    u16 n;

    frame_number = 0;
    counter = 0;
    open = 0;
    item = 1;
    last_d = 0;
    st_state = 1;
    st_marks = 0;
    st_file = 0;
    st_hour = 0;
    st_min = 0;
    st_sec = 0;
    st_reason = 0;
    st_flags = 0;
    st_cut = 0;
    st_left = 0;
    st_left_sec = 0;
    st_rest_h = 3;
    st_rest_m = 25;
    st_rest_s = 10;
    st_space = 35;
    st_none = 0;
    st_frozen = 0;
    quiet = 0;
    combo = 0;
    call_init();
    DISP_IMMEDIATE = 0;

#ifdef SCENARIO_UPDATE


    combo = 1;
    for (n = 0; n < 40; n++)
        exchange(0, 0);
    combo = 0;
    quiet = 1;
    for (n = frame_number; n < 400; n++)
        exchange(0, 0);
    quiet = 0;
    press(1, QUANTIZE);
    press(2, PREV);
    press(3, NEXT);
    press(4, TAP);
    press(5, QUANTIZE);
    idle(6, 40);
    SIM_PUTC('D'); SIM_PUTC('\n');
    return;
#endif

#ifdef SCENARIO_BOOT


    st_none = 1;
    quiet = 1;
    for (n = frame_number; n < 320; n++)
        exchange(0, 0);
    quiet = 0;
    press(0x20, QUANTIZE);
    press(0x21, PREV);
    press(1, TAP);
    idle(2, 25);
    idle(3, 25);
    idle(4, 25);
    press(0x22, TAP);
    press(5, NEXT);
    press(0x23, NEXT);
    press(21, PREV);
    press(0x24, PREV);
    quiet = 1;
    for (n = frame_number; n < 3600; n++)
        exchange(0, 0);
    quiet = 0;
    idle(6, 25);
    idle(7, 25);
    idle(8, 25);
    idle(9, 25);
    quiet = 1;
    for (n = frame_number; n < 3800; n++)
        exchange(0, 0);
    quiet = 0;
    idle(10, 25);
    idle(11, 25);
    idle(12, 25);
    idle(13, 25);
    st_none = 0; st_state = 0; st_reason = 1;
    idle(14, 25);
    idle(15, 25);
    idle(16, 25);
    idle(17, 25);
    st_state = 1; st_reason = 0;
    idle(18, 30);
    press(19, AUTOTAP);
    press(20, QUANTIZE);
    SIM_PUTC('D'); SIM_PUTC('\n');
    return;
#endif


    idle(1, 5);
    press(2, QUANTIZE);
    quiet = 1;
    for (n = frame_number; n < 60; n++)
        exchange(0, 0);
    quiet = 0;
    tag(8);
    press(9, PREV);
    press(0x50, NEXT);
    press(3, NEXT);
    press(4, PREV);
    press(0x51, QUANTIZE);

    press(0xE0, QUANTIZE);
    press(10, PREV);
    press(11, PREV);
    press(12, AUTOTAP);
    press(13, TAP);
    press(14, PREV);
    press(15, TAP);
    st_state = 2; st_min = 0; st_sec = 12; st_marks = 0; st_file = 1;
    idle(16, 30);
    press(17, NEXT);
    press(18, TAP);
    st_marks = 1; st_sec = 15;
    idle(19, 30);
    press(20, TAP);
    st_marks = 2; st_sec = 17;
    idle(21, 30);
    press(22, NEXT);
    press(0xF0, NEXT);
    press(23, NEXT);
    st_rest_s = 9;
    idle(0xB1, 30);
    press(0xB3, NEXT);
    press(0xB4, TAP);
    press(0xB7, PREV);
    press(0xF1, PREV);
    press(24, TAP);
    st_file = 2; st_marks = 0; st_min = 1; st_sec = 5;
    idle(25, 30);
    press(26, AUTOTAP);
    press(27, TAP);
    press(28, PREV);
    press(29, PREV);
    press(30, TAP);
    st_state = 3;
    idle(31, 30);
    st_state = 1; st_min = 0; st_sec = 0; st_file = 0;
    idle(32, 30);
    press(33, AUTOTAP);
    press(34, NEXT);
    press(35, NEXT);
    press(36, NEXT);
    press(37, PREV);
    press(38, PREV);
    press(39, PREV);
    press(40, QUANTIZE);
    press(41, NEXT);
    press(0xE1, QUANTIZE);
    press(42, PREV);
    press(43, TAP);


    press(44, AUTOTAP);
    for (n = 0; n < 60; n++)
        exchange(KEY_NEXT, 0);
    tag(45);
    idle(46, 12);
    press(47, QUANTIZE);


    st_state = 2; st_hour = 2; st_min = 3; st_sec = 44; st_marks = 4; st_file = 1;
    idle(0x4F, 30);
    press(0xE2, QUANTIZE);
    press(0x52, PREV);
    press(0x53, TAP);
    press(0x54, NEXT);
    press(0x55, QUANTIZE);


    st_state = 0; st_reason = 2; st_hour = 0; st_min = 0; st_sec = 0; st_marks = 0; st_file = 0;
    st_rest_h = 255;
    press(0xE3, QUANTIZE);
    press(0x60, PREV);
    press(0x61, TAP);
    press(0x56, PREV);
    press(0xF2, NEXT);
    press(0x57, NEXT);
    press(0x58, TAP);
    press(0x59, AUTOTAP);
    press(0xE4, TAP);
    press(0xF3, PREV);
    press(0xEA, PREV);
    st_reason = 3;
    idle(0x62, 30);
    st_reason = 4;
    idle(0x63, 30);
    st_reason = 5;
    idle(0x64, 30);
    st_reason = 6;
    idle(0x65, 30);
    st_reason = 1;
    idle(0x66, 25);
    idle(0x67, 25);
    idle(0x68, 25);
    idle(0x69, 25);
    st_reason = 2;
    press(0x6A, AUTOTAP);
    press(0xE5, NEXT);
    press(0x6B, PREV);
    st_state = 1; st_reason = 0; st_rest_h = 3;
    idle(0x6C, 30);
    press(0x6D, TAP);
    press(0x6E, NEXT);
    press(0xB8, PREV);
    st_state = 0; st_reason = 2;
    idle(0x6F, 30);
    st_state = 1; st_reason = 0;
    idle(0x70, 30);
    idle(0x71, 20);
    press(0x72, TAP);
    st_state = 2; st_sec = 7; st_file = 1; st_cut = 1;
    idle(0x73, 30);
    press(0x74, NEXT);
    st_marks = 100;
    idle(0x75, 30);
    press(0x76, TAP);
    st_marks = 5;
    press(0x77, NEXT);
    press(0x78, TAP);
    st_cut = 2;
    idle(0x79, 30);
    press(0x7A, TAP);
    st_cut = 0;
    idle(0x7B, 30);
    press(0x7C, TAP);
    st_state = 3; st_cut = 0;
    idle(0x7D, 30);
    press(0x7E, TAP);
    st_state = 1; st_sec = 0; st_marks = 0; st_file = 0;
    idle(0x7F, 30);
    st_frozen = 1;
    quiet = 1;
    for (n = 0; n < 330; n++)
        exchange(0, 0);
    quiet = 0;
    tag(0x80);
    st_frozen = 0;
    idle(0x81, 30);
    press(0x82, QUANTIZE);


    st_state = 2; st_flags = 4; st_hour = 0; st_min = 12; st_sec = 34; st_marks = 7; st_file = 1; st_cut = 0;
    idle(0x8F, 30);
    press(0xE6, QUANTIZE);
    press(0x90, PREV);
    press(0x91, TAP);
    press(0x92, NEXT);
    st_hour = 1; st_min = 12; st_marks = 12;
    idle(0x93, 30);
    st_flags = 0;
    idle(0x94, 30);
    press(0x95, QUANTIZE);


    st_state = 2; st_flags = 0; st_hour = 1; st_min = 31; st_sec = 2; st_marks = 12; st_file = 1;
    st_left = 30; st_left_sec = 58;
    idle(0x9F, 30);
    press(0xE7, QUANTIZE);
    press(0xA0, PREV);
    press(0xA1, TAP);
    press(0xA2, PREV);
    press(0xA8, NEXT);
    st_flags = 4; st_left = 1; st_left_sec = 7;
    idle(0xA3, 30);
    press(0xA4, NEXT);
    st_flags = 0; st_left = 0; st_left_sec = 0;
    idle(0xA5, 30);
    st_left = 61;
    idle(0xA6, 30);
    st_left = 60; st_left_sec = 59;
    idle(0xA9, 30);
    st_left = 0;
    press(0xA7, QUANTIZE);


    st_state = 1; st_flags = 0; st_hour = 0; st_min = 0; st_sec = 0; st_marks = 0; st_file = 0;
    st_rest_h = 3; st_rest_m = 25; st_rest_s = 10;
    idle(0xC0, 30);
    press(0xEB, QUANTIZE);
    press(0xC1, PREV);
    press(0xC2, TAP);
    press(0xF4, NEXT);
    press(0xC3, NEXT);
    st_rest_h = 124; st_rest_m = 0; st_rest_s = 0; st_space = 1285;
    idle(0xCB, 30);
    st_rest_h = 255; st_rest_m = 0; st_rest_s = 0; st_space = 0;
    idle(0xCD, 30);
    st_rest_h = 0; st_rest_m = 60; st_rest_s = 0; st_space = 0x8000u | 1036u;
    idle(0xCF, 30);
    st_rest_h = 0; st_rest_m = 57; st_rest_s = 52; st_space = 0x8000u | 999u;
    idle(0xD0, 30);
    st_rest_h = 0; st_rest_m = 57; st_rest_s = 53; st_space = 10;
    idle(0xD1, 30);
    st_rest_h = 0; st_rest_m = 0; st_rest_s = 41; st_space = 0x8000u | 11u;
    idle(0xD2, 30);
    st_state = 0; st_reason = 2; st_rest_h = 255; st_space = 0;
    idle(0xD3, 30);
    press(0xF5, PREV);
    press(0xD4, PREV);
    press(0xF6, NEXT);
    press(0xE8, NEXT);
    st_state = 1; st_reason = 0; st_rest_h = 1; st_rest_m = 2; st_rest_s = 3; st_space = 10;
    idle(0xD5, 30);
    press(0xF7, PREV);
    press(0xD6, PREV);
    press(0xF8, NEXT);
    press(0xD7, NEXT);
    press(0xD8, AUTOTAP);
    press(0xD9, TAP);
    press(0xDA, TAP);
    press(0xDB, QUANTIZE);
    press(0xDC, QUANTIZE);
    press(0xE9, PREV);
    press(0xDD, TAP);
    press(0xDE, QUANTIZE);


    press(0x40, QUANTIZE);
    press(0x41, PREV);
    press(0x42, TAP);
    press(0x43, PREV);
    press(0x44, TAP);
    st_flags = 0x20;
    idle(0x45, 100);
    idle(0x46, 100);
    idle(0x47, 100);
    idle(0x48, 100);
    press(0x49, TAP);
    st_flags = 0;
    idle(0x4A, 30);
    press(0x30, TAP);
    st_flags = 0x20; st_state = 2; st_sec = 3; st_file = 1;
    idle(0x31, 100);
    idle(0x32, 100);
    idle(0x33, 100);
    idle(0x34, 100);
    press(0x4B, TAP);
    st_state = 3;
    idle(0x4C, 30);
    press(0x4D, TAP);
    st_state = 1; st_flags = 0; st_sec = 0; st_file = 0;
    idle(0x4E, 30);
    press(0x35, QUANTIZE);
    SIM_PUTC('D'); SIM_PUTC('\n');
}
