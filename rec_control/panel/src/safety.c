/* Power-on interlock: stays out of the way of the update mode.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "panel.h"
#include "recstat.h"
#include "menu.h"

#define ARM_FRAMES 30
#define MAGIC1 0x5AFE
#define MAGIC2 0xA11C

struct safety {
    u16 magic1, magic2;
    u16 frames;
    u8 last_stage;
    u8 disarmed;
};
#define SF ((struct safety *)MOD_SAFETY_ADDR)


void mod_init(void)
{
    SF->magic1 = 0;
    SF->frames = 0;
    SF->last_stage = 0xFF;
    SF->disarmed = 0;
    recstat_init();
    menu_init();
    SF->magic2 = MAGIC2;
    SF->magic1 = MAGIC1;
}


u8 mod_armed(void)
{
    u8 stage;

    if (SF->magic1 != MAGIC1 || SF->magic2 != MAGIC2)
        return 0;
    stage = DISP_STAGE;
    if (SF->frames < ARM_FRAMES && UPDATE_COMBO_HELD())
        SF->disarmed = 1;
    if (stage == 1 && SF->last_stage != 1) {
        if (SF->frames < ARM_FRAMES)
            SF->frames++;
        if (DISP_MODE == 3)
            SF->disarmed = 1;
    }
    SF->last_stage = stage;
    return !SF->disarmed && SF->frames >= ARM_FRAMES;
}


u8 mod_ok_irq(void)
{
    return SF->magic1 == MAGIC1 && SF->magic2 == MAGIC2 &&
           !SF->disarmed && SF->frames >= ARM_FRAMES;
}
