/* The link to the main processor: commands in, status out.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "listen.h"

struct listen_log listen_log = { .magic = { 'D', 'J', 'M', 'L', 'S', 'N', '0', '1' } };
struct tunnel_log tunnel_log = { .magic = { 'D', 'J', 'M', 'T', 'U', 'N', '0', '2' } };
static u32 listen_started = 0u;
static u32 last_sequence = 0u;
static u32 last_pipe = 0u;
static u32 status_age = TUNNEL_EVERY - 1u;
static u32 status_pending = 0u;
static u8 status_counter = 0u;
static u8 status[TUNNEL_OUT_BYTES] = { 0u };

static u8 feed_age = 0u;
static u8 feed_pending = 0u;
static u8 feed_asked = 0u;
static u8 feed_off_left = 0u;
static u8 feed_on = 0u;


static void tunnel_in(const u8 *bytes)
{
    struct tunnel_log *t = &tunnel_log;
    u32 index, changed = !listen_started, any = 0u;

    for (index = 0u; index < TUNNEL_IN_BYTES; index++) {
        if (bytes[index] != t->last[index])
            changed = 1u;
        if (bytes[index] != 0u)
            any = 1u;
    }
    t->nonzero += any;


    if ((u8)(bytes[10] ^ bytes[11]) == 0xFFu && bytes[10] != t->last_command) {
        t->last_command = bytes[10];
        t->commands++;
        control_command(bytes[10] & 0x0Fu);
    }
    if (!changed)
        return;
    for (index = 0u; index < TUNNEL_IN_BYTES; index++) {
        if (!listen_started)
            t->first[index] = bytes[index];
        t->last[index] = bytes[index];
        if (t->changes < TUNNEL_HISTORY)
            t->history[t->changes].bytes[index] = bytes[index];
    }
    if (t->changes < TUNNEL_HISTORY)
        t->history[t->changes].packet = listen_log.packets;
    t->changes++;
}


static void tunnel_out(volatile u8 *link_tx, const volatile u8 *link_state)
{

    static const u8 request[FEED_BYTES] = { 0x04u, 0xF0u, 0x00u, 0x40u, 0x04u, 0x05u, 0x44u, 0x4Au,
                                            0x04u, 0x4Du, 0x58u, 0x00u, 0x07u, 0x65u, 0x01u, 0xF7u };
    u32 index, sum = 0u;

    if (++status_age >= TUNNEL_EVERY) {
        status_age = 0u;
        for (index = 0u; index < TUNNEL_OUT_BYTES; index++)
            status[index] = 0u;
        status[0] = TUNNEL_MAGIC;
        status[1] = TUNNEL_FORMAT;
        status[2] = ++status_counter;
        tunnel_status(status);


        if (status[3] == 0u)
            feed_on = 0u;
        if (++feed_age >= (feed_on ? FEED_EVERY_ON : FEED_EVERY)) {
            feed_age = 0u;
            if (status[3] != 0u) {
                feed_pending = 1u;
                feed_off_left = FEED_OFF_TRIES;
            } else if (feed_off_left != 0u) {
                feed_off_left--;
                feed_pending = 2u;
            }
        }
        if (feed_on)
            status[4] |= STATUS_FEED;
        for (index = 0u; index < TUNNEL_OUT_BYTES - 1u; index++)
            sum += status[index];
        status[TUNNEL_OUT_BYTES - 1u] = (u8)~sum;
        status_pending = 1u;
    }
    if (!status_pending && !feed_pending)
        return;
    if (*link_state != 0u) {
        tunnel_log.busy += status_pending;
        return;
    }
    if (status_pending) {
        for (index = 0u; index < TUNNEL_OUT_BYTES; index++) {
            link_tx[(TUNNEL_OUT_AT + index) ^ 1u] = status[index];
            tunnel_log.status[index] = status[index];
        }
        tunnel_log.sent++;
        status_pending = 0u;
    }
    if (feed_pending) {


        for (index = 0u; index < FEED_BYTES; index++)
            link_tx[(FEED_AT + 2u + index) ^ 1u] = request[index];
        if (feed_pending == 2u)
            link_tx[(FEED_AT + 2u + FEED_VALUE) ^ 1u] = 0u;
        link_tx[(FEED_AT + 1u) ^ 1u] = FEED_BYTES;
        link_tx[FEED_AT ^ 1u]++;
        feed_asked = feed_pending;
        feed_pending = 0u;
        if ((listen_log.feed >> 16) != 0xFFFFu)
            listen_log.feed += 0x10000u;
    }
}


static void feed_answer(u32 code)
{
    u32 feed = listen_log.feed;

    if ((feed & 0xFF00u) != 0xFF00u)
        feed += 0x100u;
    listen_log.feed = (feed & ~0xFFu) | ((code + 1u) & 0xFFu);
    if (code != 0u)
        return;
    if (feed_asked == 1u) {
        feed_on = 1u;
    } else {
        feed_on = 0u;
        feed_off_left = 0u;
    }
}

static void note(u32 status, u32 data1, u32 value)
{
    struct listen_log *l = &listen_log;
    u32 index;

    control_message(status, data1, value);
    for (index = 0u; index < l->seen_count; index++) {
        struct listen_seen *s = &l->seen[index];

        if (s->status == status && s->data1 == data1) {
            s->last = (u8)value;
            if (s->count != 255u)
                s->count++;
            break;
        }
    }
    if (index == l->seen_count && l->seen_count < LISTEN_SEEN) {
        struct listen_seen *s = &l->seen[l->seen_count++];

        s->status = (u8)status;
        s->data1 = (u8)data1;
        s->last = (u8)value;
        s->count = 1u;
    }
}

void listen_poll(const volatile u8 *link_rx, volatile u8 *link_tx, const volatile u8 *link_state)
{
    struct listen_log *l = &listen_log;
    static u8 packet[LINK_PACKET_BYTES] = { 0u };
    u32 index, sum, length, pipe, changed;

    l->polls++;
    tunnel_out(link_tx, link_state);
    if (listen_started && ((u32)link_rx[4] << 8 | link_rx[5]) == last_sequence)
        return;
    for (index = 0u; index < LINK_PACKET_BYTES; index++)
        packet[index] = link_rx[index ^ 1u];
    for (index = 0u; index < LINK_PACKET_BYTES; index++) {
        if (packet[index] != link_rx[index ^ 1u]) {
            l->torn++;
            return;
        }
    }


    sum = 0u;
    for (index = 0u; index < LINK_PACKET_BYTES - 3u; index++)
        sum += packet[index];
    sum = (sum & 0xFFu) + (sum >> 8);
    sum = ~(sum + (sum >> 8)) & 0xFFu;
    if (sum != packet[LINK_PACKET_BYTES - 2u]) {
        l->checksum_bad++;
        return;
    }
    sum = (u32)packet[5] << 8 | packet[4];
    if (listen_started)
        l->missed += (sum - last_sequence - 1u) & 0xFFFFu;
    last_sequence = sum;
    l->packets++;

    changed = !listen_started;
    for (index = 0u; index < 24u; index++) {
        if (index != 4u && index != 5u && packet[index] != l->last_head[index])
            changed = 1u;
    }
    for (index = 0u; index < sizeof(l->last_head); index++) {
        if (!listen_started)
            l->first_head[index] = packet[index];
        l->last_head[index] = packet[index];
    }
    if (changed && listen_started)
        l->head_changes++;
    for (index = 0u; index < sizeof(l->tail); index++)
        l->tail[index] = packet[0x66u + index];
    tunnel_in(&packet[TUNNEL_IN_AT]);


    pipe = packet[0x18];
    if (listen_started && pipe != last_pipe) {
        length = packet[0x19] & 0x7Fu;
        if (length > 64u)
            length = 64u;
        if (packet[0x19] & 0x80u)
            l->clock_flags++;
        if (length != 0u) {
            l->pipe_packets++;
            l->pipe_bytes += length;
        }


        for (index = 0u; index + 3u < length; index += 4u) {
            u32 kind = packet[0x1Au + index] & 0x0Fu;

            if (kind - 4u < 4u) {
                if (kind == 7u && packet[0x1Bu + index] == FEED_ANSWER && packet[0x1Du + index] == 0xF7u)
                    feed_answer(packet[0x1Cu + index]);
                continue;
            }
            note(packet[0x1Bu + index], packet[0x1Cu + index], packet[0x1Du + index]);
        }
    }
    last_pipe = pipe;
    listen_started = 1u;
}
