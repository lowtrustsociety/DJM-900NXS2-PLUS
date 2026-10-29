/* The recorder's status as the panel receives it.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "panel.h"
#include "recstat.h"
#include "menu.h"

#define DISP_TYPE RAM8(0x1F4F)


#define G_COLON  10
#define G_R      11
#define G_E      12
#define G_A      13
#define G_D      14
#define G_Y      15
#define G_RING   16
#define G_DOT    17
#define G_SQUARE 18
#define G_WARN   19
#define G_H      20
#define G_M      21
#define G_F      22
#define G_U      23
#define G_L      24
#define G_O      25
#define G_N      26
#define G_S      27
#define G_B      28
#define G_MINUS  29
#define G_T      30
#define G_I      31
#define G_CAP_M  32
#define G_P      33
#define G_V      34
#define G_C      35
#define G_G      36
#define G_POINT  37
#define G_SPACE  0xFE
#define G_NONE   0xFF

#define CAUSE_NONE   0
#define CAUSE_FULL   1
#define CAUSE_ERROR  2
#define CAUSE_NO_USB 3
#define CAUSE_OTHER  4
static const __far u8 cause_text[3][6] = {
    { G_F, G_U, G_L, G_L, G_NONE, G_NONE },
    { G_E, G_R, G_R, G_O, G_R, G_NONE },
    { G_N, G_O, G_NONE, G_U, G_S, G_B },
};
static const __far u8 font[38][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00},
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    {0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00},
    {0x00, 0x0E, 0x1F, 0x1F, 0x1F, 0x0E, 0x00},
    {0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00},
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11},
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18},
};


#define WORD_CHARS 10
static const __far u8 about_words[3][WORD_CHARS] = {
    { G_T, G_I, G_CAP_M, G_E, G_SPACE, G_NONE },
    { G_S, G_P, G_A, G_C, G_E, G_SPACE, G_NONE },
    { G_P, G_L, G_U, G_S, G_SPACE, G_V, REC_V0, G_POINT, REC_V1, REC_V2 },
};


void recstat_init(void)
{
    u8 i;

    ST->last_stage = 0xFF;
    ST->pending = 0;
    ST->seen = 0;
    ST->fresh = 0;
    ST->counter = 0;
    ST->age = 0;
    ST->kind = KIND_NONE;
    ST->hours = 0;
    ST->minutes = 0;
    ST->seconds = 0;
    ST->warn = 0;
    ST->marks = 0;
    ST->file = 0;
    ST->reason = 0;
    ST->cut = 0;
    ST->stopped = 0;
    ST->left = 0;
    ST->left_seconds = 0;
    ST->rest_hours = REST_UNKNOWN;
    ST->rest_minutes = 0;
    ST->rest_seconds = 0;
    ST->uptime = 0;
    for (i = 0; i < WANT; i++)
        ST->shown[i] = 0;
    ST->normal = 0;
    ST->buffer = 0;
}


static void glyph_at(u8 *rows, u8 x, u8 g, u8 stride)
{
    const __far u8 *bits = font[g];
    u8 at = (u8)(x >> 3), shift = (u8)(x & 7), r, high, low;
    u16 word;

    for (r = 0; r < 7; r++) {
        word = (u16)bits[r] << 11;
        word >>= shift;
        high = (u8)(word >> 8);
        low = (u8)word;
        rows[at] |= high;
        if (low)
            rows[at + 1] |= low;
        rows += stride;
    }
}
#define glyph(rows, x, g) glyph_at(rows, x, g, IND_COLS)

static u8 two(u8 *rows, u8 x, u8 value)
{
    u8 tens = 0;

    while (value >= 10) {
        value -= 10;
        tens++;
    }
    glyph(rows, x, tens);
    glyph(rows, (u8)(x + 6), value);
    return (u8)(x + 12);
}


static void compose(u8 *record, const u8 *want)
{
    u8 *rows = record + 2;
    u8 i, x, hours = ST->hours;
    volatile u8 g;

    record[0] = IND_COLS * 8;
    record[1] = IND_ROWS;
    for (i = 0; i < IND_BYTES; i++)
        rows[i] = 0;
    if (want[4])
        glyph(rows, IND_X0, G_WARN);
    if (want[0] == KIND_NONE) {

        if (want[5] >= CAUSE_FULL && want[5] <= CAUSE_NO_USB) {
            for (i = 0; i < 6; i++) {
                g = cause_text[want[5] - 1][i];
                if (g != G_NONE)
                    glyph(rows, (u8)(IND_X0 + 7 + 6 * i), g);
            }
        }
    } else if (want[0] == KIND_READY) {
        if (!want[4])
            glyph(rows, IND_X0, G_RING);
        glyph(rows, IND_X0 + 7, G_R);
        glyph(rows, IND_X0 + 13, G_E);
        glyph(rows, IND_X0 + 19, G_A);
        glyph(rows, IND_X0 + 25, G_D);
        glyph(rows, IND_X0 + 31, G_Y);
    } else {
        if (!want[4] && !want[6])
            glyph(rows, IND_X0, want[0] == KIND_REC ? G_DOT : G_SQUARE);
        x = IND_X0 + 7;
        if (want[7]) {

            glyph(rows, x, G_MINUS);
            x = two(rows, (u8)(x + 6), want[2]);
            glyph(rows, x, G_COLON);
            (void)two(rows, (u8)(x + 3), want[3]);
        } else if (hours) {

            if (hours > 9)
                hours = 9;
            glyph(rows, x, hours);
            glyph(rows, (u8)(x + 6), G_H);
            x = two(rows, (u8)(x + 12), ST->minutes);
            glyph(rows, x, G_M);
        } else {
            x = two(rows, x, ST->minutes);
            glyph(rows, x, G_COLON);
            (void)two(rows, (u8)(x + 3), ST->seconds);
        }
    }
}


static u8 decimal(u8 *out, u16 value, u8 least)
{
    u16 step = 10000;
    u8 n = 0, d;

    while (step) {
        d = 0;
        while (value >= step) {
            value -= step;
            d++;
        }
        if (d || n || step < 10 || (step == 10 && least >= 2) || (step == 100 && least >= 3))
            out[n++] = d;
        step = step == 10000 ? 1000 : step == 1000 ? 100 : step == 100 ? 10 : step == 10 ? 1 : 0;
    }
    return n;
}


static u8 word(u8 *out, u8 which)
{
    u8 n = 0;
    volatile u8 g;

    while (n < WORD_CHARS) {
        g = about_words[which][n];
        if (g == G_NONE)
            break;
        out[n++] = g;
    }
    return n;
}


static u8 line(u8 *rows, u8 x, u8 row, const u8 *g, u8 n)
{
    u8 i, w = 0, c;

    for (i = 0; i < n; i++) {
        c = g[i];
        if (rows && c != G_SPACE)
            glyph_at(rows + (u16)row * ABOUT_COLS, (u8)(x + w), c, ABOUT_COLS);
        w = (u8)(w + (c == G_SPACE ? 4 : c == G_COLON || c == G_POINT ? 3 : 6));
    }
    return (u8)(w - 1);
}

void about_compose(u8 *record)
{
    u8 *rows = record + 2;
    u8 t[20], n, h = ST->rest_hours, known = (u8)(ST->fresh && ST->rest_hours != REST_UNKNOWN);
    u16 i, whole;

    record[0] = ABOUT_COLS * 8;
    record[1] = ABOUT_ROWS;
    for (i = 0; i < ABOUT_BYTES; i++)
        rows[i] = 0;


    n = word(t, 0);
    if (!known) {
        t[n++] = G_MINUS;
        t[n++] = G_MINUS;
    } else {
        n += decimal(t + n, h, 2);
        t[n++] = G_COLON;
        n += decimal(t + n, ST->rest_minutes, 2);
        t[n++] = G_COLON;
        n += decimal(t + n, ST->rest_seconds, 2);
    }
    (void)line(rows, 4, 2, t, n);


    n = word(t, 1);
    if (!known) {
        t[n++] = G_MINUS;
        t[n++] = G_MINUS;
    } else {


        whole = ST->space;
        if (!(whole & 0x8000u)) {
            n += decimal(t + n, whole, 2);
            t[n] = t[n - 1];
            t[n - 1] = G_POINT;
            n++;
            t[n++] = G_SPACE;
            t[n++] = G_G;
        } else {
            n += decimal(t + n, (u16)(whole & 0x7FFFu), 1);
            t[n++] = G_SPACE;
            t[n++] = G_CAP_M;
        }
        t[n++] = G_B;
    }
    (void)line(rows, (u8)(188 - line(0, 0, 0, t, n)), 2, t, n);


    n = word(t, 2);
    (void)line(rows, (u8)(188 - line(0, 0, 0, t, n)), 22, t, n);
}


static void sample(void)
{
    u8 status[STATUS_BYTES];
    const u8 *source;
    u8 index = RX_IDX, i, sum = 0, kind;

    source = (const u8 *)(RX_BUF_BASE + STATUS_AT);
    if (index)
        source += FRAME_LEN;

    if (source[0] != STATUS_MAGIC || (ST->seen && source[2] == ST->counter))
        goto nothing_new;
    for (i = 0; i < STATUS_BYTES; i++)
        status[i] = source[i];
    for (i = 0; i < STATUS_BYTES - 1; i++)
        sum += status[i];
    if (index == RX_IDX && status[0] == STATUS_MAGIC && status[1] == STATUS_FORMAT &&
        status[STATUS_BYTES - 1] == (u8)~sum && status[6] < 60 && status[7] < 60 &&
        status[18] <= LEFT_MAX && status[19] < 60 &&
        (status[20] == REST_UNKNOWN || (status[21] < 60 && status[22] < 60)) &&
        (!ST->seen || status[2] != ST->counter)) {
        ST->counter = status[2];
        ST->age = 0;
        ST->seen = 1;
        ST->fresh = 1;
        kind = status[3];
        ST->kind = kind <= KIND_CLOSING ? kind : KIND_NONE;
        ST->hours = status[5];
        ST->minutes = status[6];
        ST->seconds = status[7];
        ST->warn = (u8)((status[4] & 4) ? 1 : 0);
        ST->stopped = (u8)((status[4] & 8) ? 1 : 0);
        ST->deep = (u8)((status[4] & 0x20) ? 1 : 0);
        ST->space = (u16)((u16)status[9] << 8 | status[10]);
        ST->marks = status[8];
        ST->file = status[15];
        ST->reason = status[16];
        ST->cut = status[17];
        ST->left = status[18];
        ST->left_seconds = status[19];
        ST->rest_hours = status[20];
        ST->rest_minutes = status[21];
        ST->rest_seconds = status[22];
        return;
    }
nothing_new:


    if (ST->fresh && ++ST->age > STALE_FRAMES)
        ST->fresh = 0;
}

static void queue(u16 pointer_low, u16 pointer_high)
{
    u16 base = QUEUE_BASE + IND_SLOT * 9;

    RAM16(base + 1) = pointer_low;
    RAM16(base + 3) = pointer_high;
    RAM8(base + 5) = IND_COL0;
    RAM8(base + 6) = IND_ROW0;
    RAM8(base + 7) = IND_COL0 + IND_COLS - 1;
    RAM8(base + 8) = IND_ROW0 + IND_ROWS - 1;
    RAM8(base) = 1;
}


static u8 repainted(void)
{
    u16 a = QUEUE_BASE;
    u8 slot;

    for (slot = 0; slot < IND_SLOT; slot++, a += 9) {
        if (RAM8(a) && RAM8(a + 5) <= IND_COL0 + IND_COLS - 1 && RAM8(a + 7) >= IND_COL0 &&
            RAM8(a + 6) <= IND_ROW0 + IND_ROWS - 1 && RAM8(a + 8) >= IND_ROW0)
            return 1;
    }
    return 0;
}


static void rendered(u8 queue_visible)
{
    u8 want[WANT], i, same = 1, force, wiped;
    u8 *record;

    want[0] = ST->fresh ? ST->kind : KIND_NONE;
    want[1] = want[2] = want[3] = want[4] = want[5] = want[6] = want[7] = 0;
    if (want[0] != KIND_NONE)
        want[4] = ST->warn;
    if (ST->fresh && ST->stopped) {

        volatile u8 reason = ST->reason;

        want[4] = 1;
        if (want[0] == KIND_NONE) {
            want[5] = CAUSE_OTHER;
            if (reason == REASON_FULL)
                want[5] = CAUSE_FULL;
            if (reason == REASON_ERROR)
                want[5] = CAUSE_ERROR;
            if (reason == REASON_NO_USB)
                want[5] = CAUSE_NO_USB;
        }
    }
    if (want[0] == KIND_REC || want[0] == KIND_CLOSING) {
        want[1] = ST->hours > 9 ? 9 : ST->hours;
        want[2] = ST->minutes;
        want[3] = ST->hours ? 0 : ST->seconds;
    }
    if (want[0] == KIND_REC) {


        want[6] = (u8)(!want[4] && (ST->counter & 2) ? 1 : 0);
        if (ST->left) {
            want[1] = 0;
            want[2] = (u8)(ST->left - 1);
            want[3] = ST->left_seconds;
            want[7] = 1;
        }
    }


    if (DISP_MODE != 1 || DISP_IMMEDIATE != 0 || DISP_TYPE >= 0x80) {
        ST->normal = 0;
        menu_rendered(queue_visible);
        return;
    }
    menu_rendered(queue_visible);
    force = (u8)!ST->normal;
    ST->normal = 1;
    wiped = (u8)(force || (queue_visible && repainted()));
    for (i = 0; i < WANT; i++) {
        if (wiped)
            ST->shown[i] = 0;
        if (want[i] != ST->shown[i])
            same = 0;
        ST->shown[i] = want[i];
    }
    if (same && !force)
        return;
    if (want[0] == KIND_NONE && !want[5]) {
        queue(0xFFFF, 0xFFFF);
        return;
    }


    record = ST->picture[ST->buffer];
    ST->buffer ^= 1;
    compose(record, want);
    queue((u16)record, 0);
}


u8 hook_render(void)
{
    u8 stage;

    if (!mod_armed())
        return 0;
    menu_keys();
    stage = DISP_STAGE;
    if (stage == 1) {
        if (ST->last_stage != 1) {
            if (ST->uptime != 0xFFFF)
                ST->uptime++;
            sample();
            menu_frame();
            ST->pending = 1;
        }
    } else if (ST->pending) {
        ST->pending = 0;
        rendered((u8)(stage == 2));
    }
    ST->last_stage = stage;
    return 0;
}
