/* The recording files: the waiting file, names, headers, closing.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef SELFMADE_RECFILE_H
#define SELFMADE_RECFILE_H

#include "fat.h"


#define RF_HEADER_NONE      0u
#define RF_HEADER_IDLE      1u
#define RF_HEADER_RUNNING   2u
#define RF_HEADER_COMPLETE  3u

enum rf_phase {
    RF_IDLE = 0,
    RF_MOUNT,
    RF_DIR,
    RF_RECOVER_HEAD,
    RF_RECOVER_MAP,
    RF_RECOVER_CUT,
    RF_TMP_MAP,
    RF_TMP_HEAD,
    RF_SCAN,
    RF_NEW_HEAD,
    RF_NEW_DIR,
    RF_LINK,
    RF_COMMIT,
    RF_ADDED,
    RF_FSINFO,
    RF_CUT_DIR,
    RF_TAIL_SLOT,
    RF_TAIL_HEAD,
    RF_TAIL_DIR,
    RF_CUT_FAT,
    RF_FREE_TAIL,
    RF_FREE_FAT,
    RF_FREE_INFO,
    RF_READY
};


enum rf_next {
    RF_NEXT_OFF = 0,
    RF_NEXT_SLOT,
    RF_NEXT_SCAN,
    RF_NEXT_HEAD,
    RF_NEXT_DIR,
    RF_NEXT_COMMIT,
    RF_NEXT_FSINFO,
    RF_NEXT_READY,
    RF_NEXT_NONE
};

struct rf {
    struct fr_vol vol;
    struct fr_file cur;
    struct fr_file other;
    struct fr_dirscan dir;
    struct fr_scan scan;
    struct fr_commit commit;
    u32 phase;
    u32 cursor;
    u32 next_number;
    u32 keep_bytes;
    u32 cut_last;
    u32 cut_next;
    u32 cut_extent;
    u32 cut_offset;
    u32 next;
    u32 tmp_pending;
    u32 switched;
    u32 header_running;
    u32 adding;
    u32 recovering;
    u32 named;
    u32 no_refill;
    u32 reusing;
    u32 repaired;

    u32 cap_bytes;
    u32 min_bytes;
    u32 refill_below_bytes;
    u32 margin_bytes;

    u32 made;
    u32 reused;
    u32 refilled;
    u32 recovered;
    u32 cuts;
    s32 last_error;
    u32 error_phase;
    u32 entry_fixed;
    u32 refused;
};


#define RF_REFUSED_READ     1u
#define RF_REFUSED_STICK    2u
#define RF_REFUSED_FILE     3u


const u8 *rf_idle_header(const struct rf *r);
u32 rf_header_kind(const u8 *sector, u32 *data_bytes);
void rf_close_header(u8 *sector);


#define RF_TAIL_MAX 4096u
u32 rf_header_file_bytes(const u8 *sector);


u32 rf_recover_tail(struct rf *r, u8 *header);

void rf_begin_prepare(struct rf *r);
void rf_begin_finish(struct rf *r, u32 file_bytes);
int rf_step(struct rf *r);
int rf_revalidate(struct rf *r);
int rf_commit_name(struct rf *r);
void rf_next_begin(struct rf *r);
int rf_next_step(struct rf *r);
int rf_switch(struct rf *r);

#endif
