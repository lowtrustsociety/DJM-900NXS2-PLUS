/* Addresses and RAM of the stock panel firmware.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef PANEL_H
#define PANEL_H

#ifndef __ASSEMBLER__


#ifndef __m32c__
#define __far
#endif

typedef unsigned char u8;
typedef unsigned short u16;

#define RAM8(a)  (*(volatile u8 *)(a))
#define RAM16(a) (*(volatile u16 *)(a))


#define FRAME_LEN        0x60
#define TX_BUF_BASE      0x1A00
#define RX_BUF_BASE      0x1AC0
#define TX_IDX           RAM8(0x1DC3)
#define RX_IDX           RAM8(0x1DC2)


#define KEYS_0B          RAM8(0x105B)
#define KEYS_0C          RAM8(0x105C)
#define UPDATE_COMBO_HELD() ((KEYS_0B & 0x02) && (KEYS_0C & 0x40))


#define TXF_TYPE_NORMAL  0x63
#define TXF_KEYS_0B      0x0B
#define TXF_KEYS_0C      0x0C
#define TXF_KEYS_0D      0x0D
#define TXF_XPAD         0x50
#define TXF_CSUM         0x5E


#define DISP_STAGE       RAM8(0x1F44)
#define DISP_IMMEDIATE   RAM8(0x1F45)
#define DISP_DIRTY       RAM8(0x1F4B)
#define DISP_PART        RAM8(0x1F4D)
#define DISP_MODE        RAM8(0x1F67)


#define QUEUE_BASE       0x17D2
#define QUEUE_DISP_SIZE  0x117

#endif


#define MOD_FB_REC_ADDR  0x2000
#define MOD_FB2_REC_ADDR 0x2610
#define MOD_SAFETY_ADDR  0x2C20
#define MOD_SAVED_SP     0x2C28
#define MOD_DIAG_ADDR    0x2C30
#define MOD_BYPASS_ADDR  0x3300
#define MOD_AUTOOFF_ADDR 0x3340
#define MOD_STACK_LOW    0x3400
#define MOD_STACK_TOP    0x3FF0


#define LCD_W 192
#define LCD_H 64

#ifndef __ASSEMBLER__

void mod_init(void);
u8 mod_armed(void);
void diag_init(void);
u8 hook_render(void);
u8 mod_bypass_ok(void);
void bypass_init(void);
u8 tx_pre(u8 type);
void tx_restore(u8 masked);
void autooff_init(void);
void autooff_tick(u8 armed);


#define BYPASS_INJECT    RAM8(MOD_BYPASS_ADDR + 15)
#endif

#endif
