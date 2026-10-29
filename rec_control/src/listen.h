/* The link to the main processor: commands in, status out.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef REC_LISTEN_H
#define REC_LISTEN_H


#include "fat.h"

#define LINK_PACKET_BYTES 128u
#define LISTEN_SECTORS      1u


#define LISTEN_SEEN        95u

struct listen_seen {
    u8 status;
    u8 data1;
    u8 last;
    u8 count;
};

struct listen_log {
    u8 magic[8];
    u32 polls;
    u32 packets;
    u32 missed;
    u32 torn;
    u32 checksum_bad;
    u32 pipe_packets;
    u32 pipe_bytes;
    u32 clock_flags;
    u32 seen_count;
    u32 feed;


    u32 head_changes;
    u8 first_head[28];
    u8 last_head[28];
    u8 tail[24];
    struct listen_seen seen[LISTEN_SEEN];
};

extern struct listen_log listen_log;


#define FEED_AT           0x24u
#define FEED_BYTES          16u
#define FEED_VALUE          14u
#define FEED_ANSWER       0x66u
#define FEED_EVERY           4u
#define FEED_EVERY_ON       32u
#define FEED_OFF_TRIES       3u
#define STATUS_FEED       0x10u
#define STATUS_DEEP       0x20u


#define COMMAND_START        1u
#define COMMAND_STOP         2u
#define COMMAND_MARK         3u
#define COMMAND_CUT          4u
#define COMMAND_BITS         5u
#define TUNNEL_SECTORS       4u
#define TUNNEL_IN_BYTES     12u
#define TUNNEL_OUT_BYTES    24u
#define TUNNEL_IN_AT      0x66u
#define TUNNEL_OUT_AT     0x66u
#define TUNNEL_HISTORY     121u
#define TUNNEL_EVERY      1000u
#define TUNNEL_MAGIC      0x52u
#define TUNNEL_FORMAT        3u

struct tunnel_change {
    u32 packet;
    u8 bytes[TUNNEL_IN_BYTES];
};

struct tunnel_log {
    u8 magic[8];
    u32 changes;
    u32 nonzero;
    u32 sent;
    u32 busy;
    u8 first[TUNNEL_IN_BYTES];
    u8 last[TUNNEL_IN_BYTES];
    u8 status[TUNNEL_OUT_BYTES];
    struct tunnel_change history[TUNNEL_HISTORY];
    u32 commands;
    u8 last_command;
    u8 reserved[3];

    u32 watch_passes;
    u32 watch_changes;
    u32 watch_first_address;
    u32 watch_first_pass;
    u32 watch_first_area;
    u32 watch_last_address;
    u32 watch_last_pass;
    u32 watch_last_area;
};

extern struct tunnel_log tunnel_log;


void listen_poll(const volatile u8 *link_rx, volatile u8 *link_tx, const volatile u8 *link_state);

void control_message(u32 status, u32 data1, u32 value);

void tunnel_status(u8 *status);

void control_command(u32 code);

#endif
