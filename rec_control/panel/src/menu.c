/* The REC entry of the UTILITY menu.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "panel.h"
#include "recstat.h"
#include "menu.h"

#define D_NAME   0x01
#define D_LEAVE  0x02

struct menu {
    u8 state;
    u8 index;
    u8 utility;
    volatile u8 mask;
    volatile u8 hold_d;
    volatile u8 hold_b;
    u8 keys_d, keys_b;
    volatile u8 command;
    u8 dirty;
    u8 arrows;
    u8 hint;
    u8 left_shown;
    u8 blink;
    u8 seconds;
    u8 word;
    u8 word_buffer;
    u8 lower[TEXT_MAX];
    u8 lower_n;
    volatile u8 tx_d, tx_b;
    volatile u8 tx_busy;
    u8 about_on;
    u8 about_buffer;
    u8 about_key[3];
};
#define M ((struct menu *)MOD_MENU_ADDR)
_Static_assert(sizeof(struct menu) <= MOD_MENU_END - MOD_MENU_ADDR, "the menu's state runs into the next variable");


#define T_READY     0
#define T_SAVING    1
#define T_NOT_READY 2
#define T_INIT      3
#define T_NO_USB    4
#define T_USB_FULL  5
#define T_USB_ERROR 6
#define T_WAIT      7
#define T_LOW_SPACE 8
#define T_MAX_100   9
#define T_START     10
#define T_STOP      15
#define T_REC       16
#define T_16BIT     17
#define T_EXPER     19
#define T_NONE      0xFF
#define TEXT_CHARS  12
static const __far char texts[20][TEXT_CHARS] = {
    "READY", "SAVING", "NOT READY", "INIT", "NO USB", "USB FULL", "USB ERROR", "WAIT",
    "LOW SPACE", "MAX 100", "START", "MARKER", "CUT", "BIT DEPTH", "ABOUT", "STOP", "REC",
    "16 BIT", "24 BIT", {'E','X','P','E','R','I','M','E','N','T','A','L'},
};


#define GLYPH_BYTES 5
#define G_A        10
#define G_I        17
#define G_Y        30
#define G_MINUS    31
#define G_X        32
#define G_BANG     33
#define G_COLON    34
#define G_H        35
#define G_NONE     0xFF
#define FONT_MAP   (36 * GLYPH_BYTES)
#define FONT_PAIRS (FONT_MAP + 26)
#define PAIRS      4
static const __far u8 font[FONT_PAIRS + 3 * PAIRS] = {
    0xA2, 0xA6, 0x2E,  74, 11,
    0xEE, 0x44, 0x5E,  92,  6,
    0xB0, 0x48, 0x2E,  90, 11,
    0x72, 0x4C, 0x2E,  90, 11,
    0x34, 0x50, 0x2E,  90, 11,
    0xF6, 0x53, 0x2E,  90, 11,
    0xB8, 0x57, 0x2E,  90, 11,
    0x7A, 0x5B, 0x2E,  90, 11,
    0x3C, 0x5F, 0x2E,  90, 11,
    0xFE, 0x62, 0x2E,  90, 11,
    0xC6, 0xF9, 0x0D,  37, 16,
    0x88, 0xFD, 0x0D,  24, 13,
    0x1C, 0x23, 0x0E,  97, 15,
    0x80, 0xEE, 0x0D, 100, 14,
    0xC6, 0xF9, 0x0D,  87, 11,
    0x5C, 0x9B, 0x0E,  93, 11,
    0x0E, 0x81, 0x0E,  79, 16,
    0x80, 0xEE, 0x0D,  92,  2,
    0x1C, 0x23, 0x0E, 115, 13,
    0x1C, 0x23, 0x0E,  64, 11,
    0xC6, 0xF9, 0x0D,  16, 17,
    0xE0, 0xA2, 0x0E,  78, 14,
    0xC6, 0xF9, 0x0D, 128, 16,
    0xA8, 0x39, 0x0E,  15, 12,
    0xC6, 0xF9, 0x0D, 103, 14,
    0xC6, 0xF9, 0x0D,  56, 12,
    0xC6, 0xF9, 0x0D,  70, 14,
    0xC6, 0xF9, 0x0D, 147, 14,
    0x42, 0xF2, 0x0D, 117, 16,
    0x90, 0x0C, 0x0E, 100, 23,
    0x1E, 0x9F, 0x0E,  72, 16,
    0x2E, 0xBD, 0x0E,  61,  8,
    (u8)OWN_ADDR, (u8)(OWN_ADDR >> 8), 0x00,  0, 16,
    (u8)OWN_ADDR, (u8)(OWN_ADDR >> 8), 0x00, 16,  2,
    (u8)OWN_ADDR, (u8)(OWN_ADDR >> 8), 0x20, 24,  3,
    0xA0, 0x2A, 0x0E,  98, 14,
    G_A, G_A + 1, G_A + 2, G_A + 3, G_A + 4, G_A + 5, G_A + 6, G_H, G_I, G_NONE, G_I + 1,
    G_I + 2, G_I + 3, G_I + 4, G_I + 5, G_I + 6, G_NONE, G_I + 7, G_I + 8, G_I + 9, G_I + 10,
    G_I + 11, G_I + 12, G_X, G_Y, G_NONE,

    G_A + 10, G_A, 1,
    G_I + 8, G_I + 9, 0xFF,
    G_I + 9, G_A, 0xFE,
    G_I + 9, G_I + 5, 0xFF,
};
#define SPACE_PIXELS 8
#define DIGIT_PIXELS 16

#define BLINK_FRAMES 100
#define BLINK_ON      60
#define MARKS_MAX    100

static void glyph_row(u8 *row, u8 at, u8 g, u8 r);


void __attribute__((section(".romtext"))) menu_init(void)
{
    u8 i, *row;
    u16 j;

    M->state = MENU_OFF;
    M->index = 0;
    M->utility = 0;
    M->mask = 0;
    M->hold_d = 0;
    M->hold_b = 0;
    M->keys_d = 0;
    M->keys_b = 0;
    M->command = 0;
    M->dirty = 0;
    M->arrows = 0xFF;
    M->hint = 0;
    M->left_shown = 0;
    M->blink = 0;
    M->seconds = 0;
    M->word = 0xFF;
    M->word_buffer = 0;
    M->lower_n = 0;
    M->tx_busy = 0;
    M->about_on = 0;
    M->about_buffer = 0;


    for (j = 0; j < WORD_COLS * WORD_ROWS; j++)
        RAM8(OWN_ADDR + j) = 0;
    for (i = 0; i < WORD_ROWS; i++) {
        row = (u8 *)(OWN_ADDR + (u16)i * WORD_COLS);
        glyph_row(row, 0, G_Y, (u8)(i < 8 ? i : i < 10 ? 9 : 17 - i));
        if (i < 12 || i >= 15)
            glyph_row(row, 16, G_I, i);
        if ((i >= 4 && i < 7) || (i >= 11 && i < 14))
            row[3] = 0xE0;
    }
}


static u8 blocked(void)
{
    u8 reason;

    if (!ST->fresh)
        return (u8)(!ST->seen && ST->uptime < INIT_FRAMES ? T_INIT : T_NOT_READY);
    if (ST->kind != KIND_NONE)
        return T_NONE;
    reason = ST->reason;
    if (reason == REASON_INIT)
        return T_INIT;
    if (reason == REASON_NO_USB)
        return T_NO_USB;
    if (reason == REASON_FULL)
        return T_USB_FULL;
    if (reason == REASON_ERROR || reason == REASON_NOT_FAT32)
        return T_USB_ERROR;
    return T_NOT_READY;
}

static u8 recording(void)
{
    return (u8)(ST->fresh && ST->kind == KIND_REC);
}


static void show_entry(void)
{
    M->state = MENU_ENTRY;
    M->mask = 1;
    M->arrows = 0xFF;
    M->hint = HINT_ENTER;
}

static void back_to_main(void)
{


    M->hold_d |= M->keys_d;
    M->hold_b |= M->keys_b;
    M->mask = 0;
    M->state = MENU_OFF;
    M->dirty |= D_LEAVE;
    M->left_shown = 0;
    M->about_on = 0;
    UT_ITEM_SHOWN = 0;
}


static u8 tap_command(void)
{
    u8 rec = recording();

    if (M->index == PAGE_START) {
        if (rec)
            return COMMAND_STOP;
        if (ST->fresh && ST->kind == KIND_READY)
            return COMMAND_START;
    } else if (rec && M->index == PAGE_MARKER) {
        if (ST->marks < MARKS_MAX)
            return COMMAND_MARK;
    } else if (rec && M->index == PAGE_CUT) {
        if (ST->cut == CUT_OK)
            return COMMAND_CUT;
    } else if (M->index == PAGE_BITS) {


        if (ST->fresh && ST->kind <= KIND_READY)
            return COMMAND_BITS;
    }
    return 0;
}

static void send(u8 code)
{
    u8 sequence = (u8)((M->command >> 4) + 1);

    if (sequence > 15)
        sequence = 1;
    M->command = (u8)(sequence << 4 | code);
}


void menu_keys(void)
{
    u8 d = RAM8(KEYS_D_ADDR) & NAV_D, b = RAM8(KEYS_B_ADDR) & NAV_B;
    u8 pressed_d = d & (u8)~M->keys_d, pressed_b = b & (u8)~M->keys_b;

    M->hold_d &= d;
    M->hold_b &= b;
    M->keys_d = d;
    M->keys_b = b;
    if (!M->utility)
        return;
    if (M->state == MENU_OFF) {


        if ((pressed_d & KEY_PREV) && UT_LEVEL == 1 && UT_BEFORE == 0) {
            show_entry();
            M->dirty |= D_NAME;
        }
    } else if (M->state == MENU_ENTRY) {
        if (pressed_d & KEY_NEXT)
            back_to_main();
        else if (pressed_d & (KEY_ENTER | KEY_TAP7)) {
            M->state = MENU_LIST;
            if (!recording() && M->index <= PAGE_CUT)
                M->index = PAGE_START;
        }
    } else {


        u8 command;

        if (!recording() && (M->index == PAGE_MARKER || M->index == PAGE_CUT))
            M->index = PAGE_START;
        if (pressed_d & KEY_PREV) {
            if (M->index == PAGE_BITS && !recording())
                M->index = PAGE_START;
            else if (M->index)
                M->index--;
        } else if (pressed_d & KEY_NEXT) {
            if (M->index == PAGE_START && !recording())
                M->index = PAGE_BITS;
            else if (M->index < PAGE_LAST)
                M->index++;
        } else if (pressed_d & (KEY_ENTER | KEY_TAP7)) {
            command = tap_command();
            if (command)
                send(command);
        } else if (pressed_b & KEY_BACK) {
            M->state = MENU_ENTRY;
        }
    }
}


void menu_frame(void)
{
    u8 open = (u8)(DISP_MODE == 2 && DISP_IMMEDIATE == 0);

    if (++M->blink >= BLINK_FRAMES) {
        M->blink = 0;
        M->seconds++;
    }
    if (open && !M->utility) {


        M->utility = 1;
        M->left_shown = 0;
    } else if (!open && M->utility) {
        M->hold_d |= M->keys_d;
        M->hold_b |= M->keys_b;
        M->mask = 0;
        M->state = MENU_OFF;
        M->utility = 0;
        M->dirty = 0;
        M->about_on = 0;
    }
}


static void put(u8 display, u8 slot, u16 low, u16 high, u8 c0, u8 r0, u8 c1, u8 r1)
{
    u16 base = QUEUE_BASE + (u16)slot * 9;

    if (display)
        base += QUEUE_DISP_SIZE;
    RAM16(base + 1) = low;
    RAM16(base + 3) = high;
    RAM8(base + 5) = c0;
    RAM8(base + 6) = r0;
    RAM8(base + 7) = c1;
    RAM8(base + 8) = r1;
    RAM8(base) = 1;
}

static u8 queued(u8 display, u8 slot)
{
    u16 base = QUEUE_BASE + (u16)slot * 9;

    if (display)
        base += QUEUE_DISP_SIZE;
    return RAM8(base);
}


static u8 wiped(u8 display)
{
    u16 base = QUEUE_BASE;

    if (display)
        base += QUEUE_DISP_SIZE;
    return (u8)(RAM8(base) && RAM8(base + 5) == 0 && RAM8(base + 6) == 0 &&
                RAM8(base + 7) == LCD_W / 8 - 1 && RAM8(base + 8) == LCD_H - 1);
}

static u8 __attribute__((noinline)) font_byte(u8 i)
{
    volatile u8 v = font[i];


    return v;
}


static void __attribute__((noinline)) glyph_row(u8 *row, u8 at, u8 g, u8 r)
{
    u8 i = (u8)(g * GLYPH_BYTES), high, first, width, x, sx, column = 0xFF, bits = 0;
    u16 low, address, dx;

    low = (u16)(font_byte(i) | (u16)font_byte((u8)(i + 1)) << 8);
    high = (u8)(font_byte((u8)(i + 2)) & 15);
    first = font_byte((u8)(i + 3));
    width = font_byte((u8)(i + 4));
    address = (u16)(low + (u16)r * WORD_COLS);
    if (address < low)
        high++;
    low = address;
    for (x = 0; x < width; x++) {
        sx = (u8)(first + x);
        if ((u8)(sx >> 3) != column) {
            column = (u8)(sx >> 3);
            address = (u16)(low + column);
            bits = far_byte(address, (u16)(address < low ? high + 1 : high));
        }
        dx = (u16)at + x;
        if ((bits & (u8)(0x80 >> (sx & 7))) && dx < WORD_COLS * 8)
            row[dx >> 3] |= (u8)(0x80 >> (dx & 7));
    }
}

static u8 glyph_of(u8 c)
{
    if (c >= '0' && c <= '9')
        return (u8)(c - '0');
    if (c >= 'A' && c <= 'Z')
        return font_byte((u8)(FONT_MAP + c - 'A'));
    if (c == '-')
        return G_MINUS;
    if (c == '!')
        return G_BANG;
    if (c == ':')
        return G_COLON;
    return G_NONE;
}


static void __attribute__((noinline)) compose(u8 *record, const u8 *text, u8 n)
{
    u8 *rows = record + 2;
    u8 k, g, p, prev, lead, width, pass, r;
    u16 x, end = 0;

    record[0] = WORD_COLS * 8;
    record[1] = WORD_ROWS;
    for (x = 0; x < WORD_COLS * WORD_ROWS; x++)
        rows[x] = 0;
    for (pass = 0; pass < 2; pass++) {
        x = pass && end < WORD_COLS * 8 ? (u16)((WORD_COLS * 8 + 1 - end) >> 1) : 0;
        prev = G_NONE;
        for (k = 0; k < n; k++) {
            g = glyph_of(text[k]);
            if (g == G_NONE) {
                x += SPACE_PIXELS;
                prev = G_NONE;
                continue;
            }
            for (p = 0; p < PAIRS; p++) {
                if (font_byte((u8)(FONT_PAIRS + 3 * p)) == prev && font_byte((u8)(FONT_PAIRS + 3 * p + 1)) == g)
                    x += (u16)(signed char)font_byte((u8)(FONT_PAIRS + 3 * p + 2));
            }
            lead = (u8)(font_byte((u8)(g * GLYPH_BYTES + 2)) >> 4);
            width = font_byte((u8)(g * GLYPH_BYTES + 4));
            if (pass) {
                for (r = 0; r < WORD_ROWS; r++)
                    glyph_row(rows + (u16)r * WORD_COLS, (u8)(x + lead), g, r);
            } else {
                end = x + lead + width;
            }
            x += g < 10 ? DIGIT_PIXELS : lead + width + 3;
            prev = g;
        }
    }
}


static u8 text_copy(u8 *out, u8 t)
{
    const __far char *from = texts[t];
    u8 n = 0;
    volatile u8 c;

    while (n < TEXT_CHARS) {
        c = (u8)from[n];
        if (!c)
            break;
        out[n++] = c;
    }
    return n;
}


static u8 __attribute__((section(".romtext"))) number(u8 *out, u8 value, u8 two)
{
    u8 tens = 0;

    while (value >= 10) {
        value -= 10;
        tens++;
    }
    if (tens > 9)
        tens = 9;
    if (tens || two) {
        out[0] = (u8)('0' + tens);
        out[1] = (u8)('0' + value);
        return 2;
    }
    out[0] = (u8)('0' + value);
    return 1;
}


static u8 status_line(u8 *text)
{
    u8 n = 0, message = blocked();
    volatile u8 page = M->index;


    if (page == PAGE_BITS && ST->fresh) {
        message = (u8)(T_16BIT + ST->deep);
        if (ST->deep && (M->seconds & 2))
            message = T_EXPER;
        return text_copy(text, message);
    }
    if (message != T_NONE) {
        if (message == T_INIT && M->blink >= BLINK_ON)
            return 0;
        return text_copy(text, message);
    }
    if (ST->kind == KIND_READY)
        return text_copy(text, T_READY);
    if (ST->kind == KIND_CLOSING)
        return text_copy(text, T_SAVING);

    if (page == PAGE_MARKER && ST->marks >= MARKS_MAX)
        return text_copy(text, T_MAX_100);
    if (page == PAGE_CUT && ST->cut != CUT_OK)
        return text_copy(text, ST->cut == CUT_WAIT ? T_WAIT : T_LOW_SPACE);
    if (ST->warn)
        text[n++] = '!';
    if (ST->left) {


        text[n++] = '-';
        n += number(text + n, (u8)(ST->left - 1), 1);
        text[n++] = ':';
        n += number(text + n, ST->left_seconds, 1);
    } else {


        n += number(text + n, ST->hours, 1);
        text[n++] = ':';
        n += number(text + n, ST->minutes, 1);
        text[n++] = ':';
        n += number(text + n, ST->seconds, 1);
    }
    if (page == PAGE_MARKER || page == PAGE_CUT) {

        text[n++] = ' ';
        text[n++] = (u8)(page == PAGE_MARKER ? 'M' : 'F');
        n += number(text + n, page == PAGE_MARKER ? ST->marks : ST->file, 0);
    }
    return n;
}

static void about(void);
static void line(void);


void menu_rendered(u8 queue_visible)
{
    u8 want, n, text[TEXT_CHARS], *record;

    if (!M->utility)
        return;
    if (M->state == MENU_OFF) {
        if (M->dirty & D_LEAVE) {
            M->dirty = 0;
            put(1, 15, 0xFFFF, 0xFFFF, 0, 0, LCD_W / 8 - 1, 39);
            M->lower_n = 0;
            if (M->hint != HINT_ENTER) {
                M->hint = HINT_ENTER;
                put(1, 18, PIC_HINT1_LOW, PIC_HIGH, 0, 52, LCD_W / 8 - 1, 59);
            }
        }


        if (UT_LEVEL == 1 && UT_BEFORE == 0) {
            if (!M->left_shown || (queue_visible && (queued(0, 17) || wiped(0)))) {
                put(0, 17, PIC_LEFT_LOW, PIC_HIGH, 14, 0, 23, 23);
                M->left_shown = 1;
            }
        } else {
            M->left_shown = 0;
        }
        return;
    }


    if (queue_visible && wiped(0)) {
        if (queued(0, 16))
            RAM8(QUEUE_BASE + 16 * 9) = 0;
        M->word = 0xFF;
        M->arrows = 0xFF;
        M->dirty &= (u8)~D_NAME;
    } else if (M->dirty & D_NAME) {
        M->dirty &= (u8)~D_NAME;
        put(0, 19, 0xFFFF, 0xFFFF, 0, 24, LCD_W / 8 - 1, LCD_H - 1);
        M->word = 0xFF;
    }
    if (queued(1, 0)) {
        M->lower_n = 0;
        M->hint = HINT_ENTER;
        M->about_on = 0;
    }


    want = T_REC;
    if (M->state == MENU_LIST)
        want = (u8)(M->index == PAGE_START && recording() ? T_STOP : T_START + M->index);
    if (want != M->word) {
        record = (u8 *)(WORD_ADDR + (M->word_buffer ? WORD_STEP : 0));
        M->word_buffer ^= 1;
        M->word = want;
        n = text_copy(text, want);
        compose(record, text, n);
        put(0, 20, (u16)record, 0, 0, WORD_ROW0, WORD_COLS - 1, WORD_ROW0 + WORD_ROWS - 1);
    }

    want = 2;
    if (M->state == MENU_LIST)
        want = (u8)((M->index ? 1 : 0) | (M->index < PAGE_LAST ? 2 : 0));
    if (want != M->arrows) {
        M->arrows = want;
        if (want & 1)
            put(0, 17, PIC_LEFT_LOW, PIC_HIGH, 14, 0, 23, 23);
        else
            put(0, 17, 0xFFFF, 0xFFFF, 14, 0, 23, 23);
        if (want & 2)
            put(0, 18, PIC_RIGHT_LOW, PIC_HIGH, 0, 0, 9, 23);
        else
            put(0, 18, 0xFFFF, 0xFFFF, 0, 0, 9, 23);
    }

    if (M->state == MENU_LIST && M->index == PAGE_ABOUT) {
        about();
    } else {
        if (M->about_on) {
            M->about_on = 0;
            put(1, 15, 0xFFFF, 0xFFFF, 0, ABOUT_ROW0, LCD_W / 8 - 1, ABOUT_ROW0 + ABOUT_ROWS - 1);
            M->lower_n = 0;
        }
        line();
    }


    want = HINT_ENTER;
    if (M->state == MENU_LIST)
        want = (u8)(tap_command() ? HINT_BACK_OK : HINT_BACK);
    if (want != M->hint) {
        M->hint = want;
        put(1, 18, want == HINT_BACK_OK ? PIC_HINT3_LOW : want == HINT_BACK ? PIC_HINT4_LOW : PIC_HINT1_LOW,
            PIC_HIGH, 0, 52, LCD_W / 8 - 1, 59);
    }
}


static void about(void)
{
    u8 i, key[3], same, *record;

    key[0] = ST->fresh ? ST->rest_hours : REST_UNKNOWN;
    key[1] = ST->rest_minutes;
    key[2] = ST->rest_seconds;
    same = M->about_on;
    for (i = 0; i < 3; i++) {
        if (key[i] != M->about_key[i])
            same = 0;
        M->about_key[i] = key[i];
    }
    M->about_on = 1;
    M->lower_n = 0;
    if (same)
        return;
    record = (u8 *)(ABOUT_ADDR + (M->about_buffer ? ABOUT_STEP : 0));
    M->about_buffer ^= 1;
    about_compose(record);
    put(1, 19, (u16)record, 0, 0, ABOUT_ROW0, LCD_W / 8 - 1, ABOUT_ROW0 + ABOUT_ROWS - 1);
}


static void line(void)
{
    u8 text[TEXT_MAX], n = 0, i, same, *record;

    if (M->state == MENU_LIST)
        n = status_line(text);
    same = (u8)(n == M->lower_n);
    for (i = 0; i < n; i++) {
        if (text[i] != M->lower[i])
            same = 0;
        M->lower[i] = text[i];
    }
    M->lower_n = n;
    if (same)
        return;
    record = (u8 *)(ABOUT_ADDR + (M->about_buffer ? ABOUT_STEP : 0));
    M->about_buffer ^= 1;
    compose(record, text, n);
    put(1, 19, (u16)record, 0, 0, LOW_ROW0, LCD_W / 8 - 1, LOW_ROW0 + WORD_ROWS - 1);
}


u8 tx_pre(u8 step)
{
    u8 flags, mask_d, mask_b;

    if (step != 0 && step != TXF_TYPE_NORMAL)
        return 0;
    if (!mod_ok_irq())
        return 0;
    flags = (u8)(step == 0 ? 2 : 0);
    if (M->tx_busy)
        return flags;
    if (M->mask) {
        mask_d = NAV_D;
        mask_b = NAV_B;
    } else {
        mask_d = M->hold_d;
        mask_b = M->hold_b;
    }
    if (mask_d | mask_b) {
        M->tx_d = RAM8(KEYS_D_ADDR);
        M->tx_b = RAM8(KEYS_B_ADDR);
        RAM8(KEYS_D_ADDR) = M->tx_d & (u8)~mask_d;
        RAM8(KEYS_B_ADDR) = M->tx_b & (u8)~mask_b;
        M->tx_busy = 1;
        flags |= 1;
    }
    return flags;
}


void tx_post(u8 flags, u16 frame)
{
    u8 command;
    u16 sum;

    if (flags & 1) {
        RAM8(KEYS_D_ADDR) = M->tx_d;
        RAM8(KEYS_B_ADDR) = M->tx_b;
        M->tx_busy = 0;
    }
    command = M->command;
    if ((flags & 2) && command) {
        RAM8(frame + TXF_COMMAND) = command;
        RAM8(frame + TXF_COMMAND + 1) = (u8)~command;
        sum = RAM16(TX_SUM_ADDR);
        RAM16(TX_SUM_ADDR) = sum + 0xFF;
        if (sum > 0xFF00)
            RAM16(TX_SUM_ADDR + 2) = RAM16(TX_SUM_ADDR + 2) + 1;
    }
}
