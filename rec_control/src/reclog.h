/* REC-LOG.TXT: the recorder's log on the stick.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef REC_RECLOG_H
#define REC_RECLOG_H


#include "recfile.h"

#define RECLOG_LOST    1u
#define RECLOG_MORE    2u
#define RECLOG_REPAIR  3u
#define RECLOG_FULL    4u
#define RECLOG_ERROR   5u
#define RECLOG_COARSE  6u
#define RECLOG_COARSE_MORE 7u
#define RECLOG_REFUSED 8u
#define RECLOG_FIXED   9u
#define RECLOG_DEPTH  10u
#define RECLOG_EVENTS 64u


void reclog_event(u32 kind, u32 number, u32 a, u32 b);


int reclog_step(struct rf *r);
void reclog_reset(void);
void reclog_power_on(void);

extern u32 reclog_lines;
extern s32 reclog_error;

#endif
