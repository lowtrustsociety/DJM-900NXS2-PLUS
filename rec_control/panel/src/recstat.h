/* The recorder's status as the panel receives it.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef RECSTAT_H
#define RECSTAT_H


#define STATUS_AT        0x44
#define STATUS_BYTES       24
#define STATUS_MAGIC     0x52
#define STATUS_FORMAT       3
#define STALE_FRAMES      300

#define IND_SLOT           30
#define IND_COL0            0
#define IND_COLS            7
#define IND_ROW0           22
#define IND_ROWS            8
#define IND_BYTES   (IND_COLS * IND_ROWS)


#define IND_X0             13

#define MOD_REC_ADDR     0x2000


#define ABOUT_ADDR       0x2200
#define ABOUT_STEP       0x0310
#define ABOUT_COLS           24
#define ABOUT_ROW0            8
#define ABOUT_ROWS           32
#define ABOUT_BYTES  (ABOUT_COLS * ABOUT_ROWS)

#define LEFT_MAX          60
#define REST_UNKNOWN     255
#define WANT               8

#define KIND_NONE    0
#define KIND_READY   1
#define KIND_REC     2
#define KIND_CLOSING 3

#define REASON_INIT      1
#define REASON_NO_USB    2
#define REASON_NOT_FAT32 3
#define REASON_FULL      4
#define REASON_ERROR     5
#define CUT_OK           0
#define CUT_WAIT         1
#define CUT_LOW          2


#define INIT_FRAMES   3700

#ifndef __ASSEMBLER__
struct recstat {
    u8 last_stage;
    u8 pending;
    u8 seen;
    u8 fresh;
    u8 counter;
    u16 age;
    u8 kind, hours, minutes, seconds, warn;
    u8 marks, file;
    u8 reason, cut;
    u8 stopped;
    u8 deep;
    u16 space;
    u8 left, left_seconds;
    u8 rest_hours, rest_minutes, rest_seconds;

    u16 uptime;
    u8 shown[WANT];

    u8 normal;
    u8 buffer;
    u8 picture[2][2 + IND_BYTES];
};
#define ST ((struct recstat *)MOD_REC_ADDR)

void recstat_init(void);
u8 mod_ok_irq(void);
void about_compose(u8 *record);
#endif

#endif
