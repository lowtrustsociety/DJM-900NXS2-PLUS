/* The REC entry of the UTILITY menu.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef MENU_H
#define MENU_H


#define MOD_MENU_ADDR    0x2100
#define MOD_MENU_END     0x2150


#define WORD_ADDR        0x2820
#define WORD_STEP        0x01C0
#define WORD_COLS        24
#define WORD_ROWS        18
#define WORD_ROW0        35


#define LOW_ROW0         11
#define TEXT_MAX         16


#define OWN_ADDR         0x2C40

#define KEYS_B_ADDR      0x105B
#define KEYS_D_ADDR      0x105D
#define KEY_PREV         0x01
#define KEY_NEXT         0x02
#define KEY_ENTER        0x08
#define KEY_TAP7         0x80
#define KEY_BACK         0x08
#define NAV_D            (KEY_PREV | KEY_NEXT | KEY_ENTER | KEY_TAP7)
#define NAV_B            KEY_BACK

#define COMMAND_START    1
#define COMMAND_STOP     2
#define COMMAND_MARK     3
#define COMMAND_CUT      4
#define COMMAND_BITS     5
#define TXF_COMMAND      0x0E
#define TX_SUM_ADDR      0x0F2C


#define UT_ITEM_SHOWN    RAM8(0x1F5B)
#define UT_BEFORE        RAM8(0x1F5E)
#define UT_LEVEL         RAM8(0x1F63)

#define MENU_OFF         0
#define MENU_ENTRY       1
#define MENU_LIST        2


#define PAGE_START       0
#define PAGE_MARKER      1
#define PAGE_CUT         2
#define PAGE_BITS        3
#define PAGE_ABOUT       4
#define PAGE_LAST        PAGE_ABOUT


#define HINT_ENTER       1
#define HINT_BACK        2
#define HINT_BACK_OK     3


#define PIC_LEFT_LOW     0xE522u
#define PIC_RIGHT_LOW    0xE706u
#define PIC_HINT1_LOW    0xE9ACu
#define PIC_HINT3_LOW    0xEB30u
#define PIC_HINT4_LOW    0xECB4u
#define PIC_HIGH         0x000Du

#ifndef __ASSEMBLER__
void menu_init(void);
void menu_keys(void);
void menu_frame(void);
void menu_rendered(u8 queue_visible);
u8 tx_pre(u8 step);
void tx_post(u8 flags, u16 frame);
u8 far_byte(u16 low, u16 high);
#endif

#endif
