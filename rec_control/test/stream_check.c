/* Simulator harness for the recorder.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include "../src/stream.h"

extern void sim_write(const char *data, int length);

volatile u32 sim_dma3_rdar;
volatile u32 sim_dma3_rdmatcr;
volatile u32 sim_send_buffer_0[48];
volatile u32 sim_send_buffer_1[48];
volatile u8 sim_low_buffer[LOW_AUDIT_BYTES];
volatile u8 sim_link_rx[LINK_PACKET_BYTES];
volatile u8 sim_link_tx[LINK_PACKET_BYTES];
volatile u8 sim_link_state;
volatile u32 sim_guard[SIM_GUARD_WORDS];
volatile u8 sim_stage_buffer[STAGE_AUDIT_BYTES];
volatile u8 sim_extra_buffer[EXTRA_BYTES];
static volatile u8 sim_pool_buffer[POOL_BLOCKS][POOL_BLOCK_BYTES];
static volatile u8 sim_map_buffer[MAP_BLOCK_BYTES];


struct segment { u32 blocks; u32 audio; };
#ifndef SCHEDULE
#define SCHEDULE { { 40000u, 0u }, { 600000u, 1u }, { 200000u, 0u } }
#endif
#define ACT_REBOOT        1u
#define ACT_STICK_OUT     2u
#define ACT_STICK_IN      3u
#define ACT_SWAP          4u
#define ACT_CORRUPT_MAP   5u
#define ACT_DIRTY_EXTRA   6u
#define ACT_DMA_COUNT     7u
#define ACT_MIDI          8u
#define ACT_COMMAND       9u
#define ACT_FEED_REFUSE  10u
#define ACT_NOTE         11u
#define ACT_GUARD        12u
#define ACT_TOUCH        13u

struct event { u32 at; u32 action; u32 arg; };
#ifndef EVENTS
#define EVENTS { { 0xFFFFFFFFu, 0u, 0u } }
#endif
struct stall { u32 write; u32 blocks; };
#ifndef STALLS
#define STALLS { { 0xFFFFFFFFu, 0u } }
#endif
#ifndef STICK_INITIAL
#define STICK_INITIAL 1u
#endif


#ifndef USER_LAYOUT
#define USER_LAYOUT 0u
#endif
#ifndef LOG_FILE
#define LOG_FILE 1u
#endif
#ifndef WRITE_FAIL_AT
#define WRITE_FAIL_AT 0xFFFFFFFFu
#endif
#ifndef META_CUT_AT
#define META_CUT_AT 0xFFFFFFFFu
#endif
#ifndef META_FAIL_AT
#define META_FAIL_AT 0xFFFFFFFFu
#endif
#ifndef POOL_FAIL_AT
#define POOL_FAIL_AT 0xFFFFFFFFu
#endif
#ifndef MAP_ALLOC_FAILS
#define MAP_ALLOC_FAILS 0u
#endif
#ifndef RELEASE_FAIL_AT
#define RELEASE_FAIL_AT 0xFFFFFFFFu
#endif
#ifndef GEOMETRY_LIE
#define GEOMETRY_LIE 0u
#endif


#ifndef TEXT_LAYOUT
#define TEXT_LAYOUT 0u
#endif

static const struct segment schedule[] = SCHEDULE;
static const struct event events[] = EVENTS;
static const struct stall stalls[] = STALLS;
static const u32 map_alloc_fails = MAP_ALLOC_FAILS;


#define SPC             32u
#define CLUSTERS        65600u
#define END_CLUSTER     (CLUSTERS + 2u)
#define RESERVED        32u
#define FAT_SECTORS     513u
#define FAT_COUNT       2u
#define DATA_START      (RESERVED + FAT_COUNT * FAT_SECTORS)
#define TOTAL_SECTORS   (DATA_START + CLUSTERS * SPC)
#define ROOT_CLUSTER    2u
#define ROOT_SECTOR     DATA_START
#define MAX_HEADERS     200u
#define MAX_TAKES       16u

static u8 boot[512], fsinfo[512];
static u8 fat[FAT_COUNT][FAT_SECTORS * 512u];
static u8 root[SPC * 512u];
static struct { u32 sector; u8 data[512]; } headers[MAX_HEADERS];
static u32 header_count;
static u8 log_sector[512];
static u8 listen_sectors[LISTEN_SECTORS * 512u];
static u32 listen_writes;
static u8 tunnel_sectors[TUNNEL_SECTORS * 512u];
static u32 tunnel_writes;
static u32 log_first_sector;
static u32 chain[CLUSTERS];
static u32 chain_length;


struct take_record {
    u32 kind;
    u32 first_sector;
    u32 stored;
    u32 audio_bytes;
    u32 take_audio;
    u32 sectors;
    u32 dropped;
    u32 fifo_left;
    u32 headers;
    u32 header_bytes;
    u32 cue_marks;
    u32 cue_position[4];
    u32 cue_lost;
    u32 cue_coarse;
    u32 cue_coarse_first;
    u32 coarse_blocks;
    u32 coarse_first;
    u32 bits;
};

static void out(const char *s);
static void field(const char *name, u32 value);
static struct take_record takes[MAX_TAKES];
static u32 take_count;
static u32 stick_present = STICK_INITIAL;
static u32 power_lost;
static int fake_fs;

static u32 now_us;
static u32 isr_calls, original_calls, current_value;
static u32 schedule_index, schedule_end, schedule_total;
static u32 event_index;
static u32 block_reads, block_writes, meta_writes, log_writes, idle_header_writes;
static u32 alloc_calls, release_calls, map_allocs;
static u32 pool_used[POOL_BLOCKS], map_used;
static u32 data_ok = 1u, bounds_ok = 1u, header_ok = 1u;
static const u32 audio_page_limit = FILE_AUDIO_PAGES;
static u32 max_write_sectors, max_read_sectors;
static u32 first_io_us = 0xFFFFFFFFu, first_alloc_us = 0xFFFFFFFFu;
static u32 arms, starts, keeps, discards, aborts, fails, reboots, readies, cuts, closed_headers;
static u32 switches;
static u16 fifo[65536];
static u32 fifo_head, fifo_tail;

static u32 take_stored, take_first, take_last;
static u32 take_audio_bytes, take_writes;
static u32 file_audio_bytes, take_sectors, take_headers, take_file = 0u;
static u32 take_header_bytes;
static u32 check_call, check_low, check_coarse, check_state;
static u8 last_audio_sector[512];
static u32 file_cue_bytes, file_cue_marks, file_cue_position[4], cue_ok = 1u;
static u32 file_cue_lost;
static u32 file_cue_coarse, file_cue_coarse_first = 0xFFFFFFFFu;
static u32 file_coarse_blocks, file_coarse_first = 0xFFFFFFFFu;
static u32 coarse_stored;
static u32 file_bits;
static u32 cut_phase = 0xFFFFFFFFu;
static u32 sheet_writes, text_writes, sheet_ok = 1u, text_ok = 1u;
static u32 dead_calls;
static u32 recover_tail_bytes, recover_tails;
static u32 cut_pending, splits;

static void put_le16(u8 *data, u32 offset, u32 value)
{
    data[offset] = (u8)value;
    data[offset + 1u] = (u8)(value >> 8);
}

static void put_le32(u8 *data, u32 offset, u32 value)
{
    data[offset] = (u8)value;
    data[offset + 1u] = (u8)(value >> 8);
    data[offset + 2u] = (u8)(value >> 16);
    data[offset + 3u] = (u8)(value >> 24);
}

static u32 le16(const u8 *data, u32 offset)
{
    return (u32)data[offset] | ((u32)data[offset + 1u] << 8);
}

static u32 le32(const u8 *data, u32 offset)
{
    return (u32)data[offset] | ((u32)data[offset + 1u] << 8) |
           ((u32)data[offset + 2u] << 16) | ((u32)data[offset + 3u] << 24);
}

static int text(const u8 *data, const char *other, u32 count)
{
    u32 index;
    for (index = 0u; index < count; index++) {
        if (data[index] != (u8)other[index])
            return 0;
    }
    return 1;
}

static void copy_bytes(u8 *destination, const u8 *source, u32 count)
{
    u32 index;
    for (index = 0u; index < count; index++)
        destination[index] = source[index];
}

static u32 fat_get(u32 cluster) { return le32(fat[0], cluster * 4u) & 0x0FFFFFFFu; }

static void fat_put(u32 cluster, u32 value)
{
    put_le32(fat[0], cluster * 4u, value);
    put_le32(fat[1], cluster * 4u, value);
}

static void dir_entry(u32 index, const char *name, u32 attr, u32 first, u32 size)
{
    u8 *e = &root[index * 32u];
    copy_bytes(e, (const u8 *)name, 11u);
    e[11] = (u8)attr;
    put_le16(e, 20u, first >> 16);
    put_le16(e, 26u, first & 0xFFFFu);
    put_le32(e, 28u, size);
}


static u32 chain_runs(u32 first, u32 run, u32 gap, u32 runs)
{
    u32 r, c, cluster = first, count = 0u;
    for (r = 0u; r < runs; r++) {
        for (c = 0u; c < run; c++) {
            u32 last = c + 1u == run;
            u32 next = !last ? cluster + 1u : (r + 1u < runs ? cluster + 1u + gap : 0x0FFFFFFFu);
            fat_put(cluster, next);
            cluster = next;
            count++;
        }
    }
    return count;
}

static u8 *stored_header(u32 sector, int create);

static void make_volume(void)
{
    u32 used = 1u, entries = 0u, n;

    boot[0] = 0xEBu; boot[1] = 0x58u; boot[2] = 0x90u;
    copy_bytes(&boot[3], (const u8 *)"MSDOS5.0", 8u);
    put_le16(boot, 24u, 32u);
    put_le16(boot, 26u, 255u);
    boot[64] = 0x80u;
    put_le16(boot, 11u, 512u);
    boot[13] = (u8)SPC;
    put_le16(boot, 14u, RESERVED);
    boot[16] = (u8)FAT_COUNT;
    boot[21] = 0xF8u;
    put_le32(boot, 32u, TOTAL_SECTORS);
    put_le32(boot, 36u, FAT_SECTORS);
    put_le32(boot, 44u, ROOT_CLUSTER);
    put_le16(boot, 48u, 1u);
    put_le16(boot, 50u, 6u);
    boot[66] = 0x29u;
    put_le32(boot, 67u, 0x1D2C3B4Au);
    copy_bytes(&boot[71], (const u8 *)"TESTVOL    FAT32   ", 19u);
    boot[510] = 0x55u;
    boot[511] = 0xAAu;
#if USER_LAYOUT == 5u
    put_le16(boot, 17u, 512u);
    put_le16(boot, 22u, 250u);
#endif
    fat_put(0u, 0x0FFFFFF8u);
    fat_put(1u, 0x0FFFFFFFu);
    fat_put(ROOT_CLUSTER, 0x0FFFFFFFu);
    dir_entry(entries++, "TESTVOL    ", 0x08u, 0u, 0u);
#if USER_LAYOUT == 1u
    n = chain_runs(3u, 20000u, 0u, 1u);
    dir_entry(entries++, "MUSIC   DAT", 0x20u, 3u, n * SPC * 512u);
    used += n;
#elif USER_LAYOUT == 2u
    n = chain_runs(3u, 10u, 10u, 1100u);
    dir_entry(entries++, "MUSIC   DAT", 0x20u, 3u, n * SPC * 512u);
    used += n;
#elif USER_LAYOUT == 3u
    n = chain_runs(3u, CLUSTERS - 1u - 2000u, 0u, 1u);
    dir_entry(entries++, "MUSIC   DAT", 0x20u, 3u, n * SPC * 512u - 100u);
    used += n;
#elif USER_LAYOUT == 4u
    n = chain_runs(3u, 4u, 0u, 1u);
    dir_entry(entries++, "DJMREC  TMP", 0x20u, 3u, n * SPC * 512u);
    used += n;
#else
    n = chain_runs(3u, 0u, 0u, 0u);
#endif
    (void)n;
#if LOG_FILE
    {
        u32 cluster = CLUSTERS - 5u;
        fat_put(cluster, 0x0FFFFFFFu);

        dir_entry(entries++, "DJMLOG  BIN", 0x20u, cluster,
                  LOG_FILE == 2u ? 512u : LOG_FILE == 3u ? 2048u : 4096u);
        log_first_sector = DATA_START + (cluster - 2u) * SPC;
        used++;
    }
#endif
#if TEXT_LAYOUT
    {
        static const char ours[] = "DJM RECORDER LOG\r\nREC0001.WAV STOPPED, USB FULL\r\n";
        static const char other[] = "Shopping list\r\nmilk\r\n";
        u32 cluster = CLUSTERS - 40u, size, i, k, files_ = TEXT_LAYOUT == 4u ? 9u : 1u;
        for (k = 0u; k < files_; k++, cluster++) {
            u8 *d = stored_header(DATA_START + (cluster - 2u) * SPC, 1);
            const char *text_ = (TEXT_LAYOUT == 1u || TEXT_LAYOUT == 4u) ? other : ours;
            static const char base[12] = "REC-LOG TXT";
            char name[12];
            for (i = 0u; i < 12u; i++)
                name[i] = base[i];
            for (size = 0u; text_[size]; size++)
                d[size] = (u8)text_[size];
            if (TEXT_LAYOUT == 6u)
                d[size++] = 0x07u;
            if (TEXT_LAYOUT == 7u)
                size = 0u;
            if (TEXT_LAYOUT == 3u) {
                for (i = 0u; i < SPC; i++) {
                    u8 *x = i ? stored_header(DATA_START + (cluster - 2u) * SPC + i, 1) : d;
                    u32 j, from = i ? 0u : 18u;
                    for (j = from; j < 512u; j++)
                        x[j] = (u8)((j % 32u == 30u) ? '\r' : (j % 32u == 31u) ? '\n' : 'x');
                }
                size = SPC * 512u;
            }
            if (k != 0u)
                name[7] = (char)('1' + k);
            if (TEXT_LAYOUT != 7u)
                fat_put(cluster, 0x0FFFFFFFu);
            dir_entry(entries++, name, TEXT_LAYOUT == 5u ? 0x21u : 0x20u, TEXT_LAYOUT == 7u ? 0u : cluster, size);
            if (TEXT_LAYOUT != 7u)
                used++;
        }
    }
#endif
    put_le32(fsinfo, 0u, 0x41615252u);
    put_le32(fsinfo, 484u, 0x61417272u);
    put_le32(fsinfo, 488u, CLUSTERS - used);
    put_le32(fsinfo, 492u, 2u);
    put_le32(fsinfo, 508u, 0xAA550000u);
}


#ifndef AUDIO_NOISE
#define AUDIO_NOISE 1u
#endif
static const u32 audio_noise = AUDIO_NOISE;


static s32 audio_sample(u32 value, u32 index)
{
    u32 x;
    if (audio_noise == 0u)
        return (s32)((value << 8) | index);
    if (audio_noise == 2u) {


        u32 t = (value * 12u + index / 2u) % 400u;
        s32 wave = (s32)(t < 200u ? t : 400u - t) * 9000 - 900000;
        x = value * 2654435761u ^ (index * 40503u + 12345u);
        x ^= x >> 15;
        x *= 2246822519u;
        return wave + (s32)((x >> 8) & ((index & 1u) ? 0xFFFFu : 0x1FFFu));
    }
    x = value * 2654435761u ^ (index * 40503u + 12345u);
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (s32)(x << 8) >> 8;
}

static u32 audio_now(void)
{
    while (isr_calls >= schedule_end &&
           schedule_index + 1u < sizeof(schedule) / sizeof(schedule[0])) {
        schedule_index++;
        schedule_end += schedule[schedule_index].blocks;
    }
    return isr_calls < schedule_end ? schedule[schedule_index].audio : 0u;
}


static u8 link_tx_sent[LINK_PACKET_BYTES];
static u32 link_tx_torn, status_seen, status_bad, status_steps, status_last_counter;
static u32 status_state_seen, status_most_seconds, status_most_marks, status_flags_seen, status_most_files;
static u32 status_file2_first = 0xFFFFu, status_file2_most;
static u32 status_file2_marks_first = 0xFFFFu;


#define VIEW_MAX 96u
static struct { u32 block, state, reason, cut; } views[VIEW_MAX];
static u32 view_count, view_lost, view_last = 0xFFFFFFFFu;


#define COUNT_MAX 200u
static struct { u32 block, left; } counts[COUNT_MAX];
static u32 count_count, count_lost, count_last, count_bad;


static u32 left_count_diff, left_ready = 0xFFFFFFFFu, left_sum_min = 0xFFFFFFFFu, left_sum_max, left_seen;
static u32 space_seen, space_bad, space_first = 0xFFFFFFFFu;


static u32 main_pipe_last, main_pipe_known, main_feed, main_refuses, main_answer, main_note;
static u32 feed_on_requests, feed_off_requests, feed_bad, feed_answers, feed_refusals, feed_changes;
static void main_takes_pipe(const u8 *sent, u32 state_known, u32 state)
{
    static const u8 want[12] = { 0xF0u, 0x00u, 0x40u, 0x05u, 0x44u, 0x4Au, 0x4Du, 0x58u, 0x00u, 0x65u, 0x00u, 0xF7u };
    u8 message[12];
    u32 counter = sent[FEED_AT ^ 1u], count = sent[(FEED_AT + 1u) ^ 1u] & 0x7Fu, i, j, n = 0u;

    if (counter == main_pipe_last)
        return;
    if (main_pipe_known && counter != ((main_pipe_last + 1u) & 0xFFu))
        feed_bad++;
    main_pipe_last = counter;
    main_pipe_known = 1u;
    if (count != FEED_BYTES) {
        feed_bad++;
        return;
    }
    for (i = 0u; i < count; i += 4u) {
        u32 kind = sent[(FEED_AT + 2u + i) ^ 1u];
        if (kind != (i + 4u == count ? 0x07u : 0x04u)) {
            feed_bad++;
            return;
        }
        for (j = 1u; j < 4u; j++)
            message[n++] = sent[(FEED_AT + 2u + i + j) ^ 1u];
    }
    for (i = 0u; i < 12u; i++) {
        if (i != 10u && message[i] != want[i]) {
            feed_bad++;
            return;
        }
    }
    if (message[10] == 1u) {
        feed_on_requests++;
        if (state_known && state == 0u)
            feed_bad++;
        if (main_refuses) {
            main_answer = 0x0Eu + 1u;
            feed_refusals++;
        } else {
            if (main_feed)
                main_answer = 1u;
            else
                feed_changes++;
            main_feed = 1u;
        }
    } else if (message[10] == 0u) {
        feed_off_requests++;
        if (state_known && state != 0u)
            feed_bad++;
        if (!main_feed)
            main_answer = 1u;
        else
            feed_changes++;
        main_feed = 0u;
    } else {
        feed_bad++;
    }
}

static void link_tx_service(void)
{
    u32 i, sum = 0u;
    u8 status[TUNNEL_OUT_BYTES];

    if (isr_calls % 8u == 6u) {
        for (i = 0u; i < LINK_PACKET_BYTES; i++)
            link_tx_sent[i] = sim_link_tx[i];
        sim_link_state = 1u;
    } else if (isr_calls % 8u == 4u && sim_link_state == 1u) {
        for (i = 0u; i < LINK_PACKET_BYTES; i++) {
            if (link_tx_sent[i] != sim_link_tx[i]) {
                link_tx_torn++;
                break;
            }
        }
        for (i = 0u; i < TUNNEL_OUT_BYTES; i++)
            status[i] = link_tx_sent[(TUNNEL_OUT_AT + i) ^ 1u];
        for (i = 0u; i < TUNNEL_OUT_BYTES - 1u; i++)
            sum += status[i];
        main_takes_pipe(link_tx_sent, status[0] == TUNNEL_MAGIC && status[TUNNEL_OUT_BYTES - 1u] == (u8)~sum,
                        status[3]);
        if (status[0] != 0u || status[1] != 0u) {
            if (status[0] != TUNNEL_MAGIC || status[1] != TUNNEL_FORMAT ||
                status[TUNNEL_OUT_BYTES - 1u] != (u8)~sum) {
                status_bad++;
            } else {
                u32 seconds = (u32)status[5] * 3600u + (u32)status[6] * 60u + status[7];
                if (status[6] > 59u || status[7] > 59u)
                    status_bad++;
                if (status_seen != 0u && status[2] != status_last_counter) {
                    status_steps++;
                    if (status[2] != (u8)(status_last_counter + 1u))
                        status_bad++;
                }
                status_last_counter = status[2];
                status_seen++;
                status_state_seen |= 1u << (status[3] & 7u);
                if (((u32)status[3] << 16 | (u32)status[16] << 8 | status[17]) != view_last) {
                    view_last = (u32)status[3] << 16 | (u32)status[16] << 8 | status[17];
                    if (view_count < VIEW_MAX) {
                        views[view_count].block = isr_calls;
                        views[view_count].state = status[3];
                        views[view_count].reason = status[16];
                        views[view_count].cut = status[17];
                        view_count++;
                    } else {
                        view_lost++;
                    }
                }
                {
                    u32 left = status[18] != 0u ? ((u32)status[18] - 1u) * 60u + status[19] + 1u : 0u;
                    if (status[18] > 30u || status[19] > 59u || (status[18] == 0u && status[19] != 0u) ||
                        (status[18] != 0u && status[3] != 2u) ||
                        (status[20] == 255u ? status[21] != 0u || status[22] != 0u
                                            : status[3] == 0u || status[3] == 3u || status[21] > 59u || status[22] > 59u))
                        count_bad++;
                    if (status[20] != 255u) {
                        u32 time_left = (u32)status[20] * 3600u + (u32)status[21] * 60u + status[22];
                        u32 v = (u32)status[9] << 8 | status[10];
                        u32 expect = time_left * ((status[4] & STATUS_DEEP) ? 288u : 192u) / 1000u;
                        u32 lo = v & 0x8000u ? v & 0x7FFFu : v * 100u;
                        u32 hi = v & 0x8000u ? v & 0x7FFFu : v * 100u + 99u;


                        if (expect + 2u < lo || hi + 2u < expect || ((v & 0x8000u) != 0u) != (hi < 1000u))
                            space_bad++;
                        if (space_first == 0xFFFFFFFFu)
                            space_first = v;
                        space_seen++;
                        left_seen++;
                        if (status[3] == 1u && left_ready == 0xFFFFFFFFu)
                            left_ready = time_left;
                        if (status[3] == 2u) {
                            u32 sum_ = time_left + isr_calls / 4000u;
                            if (sum_ < left_sum_min)
                                left_sum_min = sum_;
                            if (sum_ > left_sum_max)
                                left_sum_max = sum_;
                        }
                        if (left != 0u) {
                            u32 d = time_left > left - 1u ? time_left - (left - 1u) : (left - 1u) - time_left;
                            if (d > left_count_diff)
                                left_count_diff = d;
                        }
                    }
                    if (left != count_last) {
                        count_last = left;
                        if (count_count < COUNT_MAX) {
                            counts[count_count].block = isr_calls;
                            counts[count_count].left = left;
                            count_count++;
                        } else {
                            count_lost++;
                        }
                    }
                }
                status_flags_seen |= status[4];
                if (seconds > status_most_seconds)
                    status_most_seconds = seconds;
                if (status[8] > status_most_marks)
                    status_most_marks = status[8];
                if (status[15] > status_most_files)
                    status_most_files = status[15];
                if (status[15] == 2u && status[3] == 2u) {
                    if (status_file2_first == 0xFFFFu) {
                        status_file2_first = seconds;
                        status_file2_marks_first = status[8];
                    }
                    if (seconds > status_file2_most)
                        status_file2_most = seconds;
                }
            }
        }
        sim_link_state = 0u;
    }
}

void sim_original_send_isr(void)
{
    volatile u32 *buffer;
    u32 index;

    link_tx_service();
    original_calls++;
    buffer = (original_calls & 1u) ? sim_send_buffer_1 : sim_send_buffer_0;
    current_value = audio_now() ? 128u + original_calls % 32000u : 0u;
    for (index = 0u; index < sim_dma3_rdmatcr && index < 48u; index++)
        buffer[index] = current_value ? (u32)audio_sample(current_value, index) << 8 : 0u;
    sim_dma3_rdar = (u32)buffer;
}


static u32 link_sequence, link_counter, link_midi, link_late_pending, link_command;
static u8 link_late[LINK_PACKET_BYTES];
static void link_packet(void)
{
    static const u8 first[8] = { 0x0Bu, 0xB0u, 0x11u, 0x40u, 0x09u, 0x90u, 0x4Eu, 0x7Fu };
    static const u8 second[4] = { 0x0Bu, 0xB0u, 0x11u, 0x41u };
    static u8 p[LINK_PACKET_BYTES];
    const u8 *data = 0;
    u32 i, sum = 0u, n = 0u;

    for (i = 0u; i < LINK_PACKET_BYTES; i++)
        p[i] = 0u;
    link_sequence++;
    p[0] = 1u;
    p[4] = (u8)link_sequence;
    p[5] = (u8)(link_sequence >> 8);
    if (isr_calls == 8000u) { data = first; n = 8u; }
    if (isr_calls == 12000u) { data = second; n = 4u; }
    if (n == 0u && link_midi != 0u) {
        static u8 event[4];
        event[0] = (u8)((link_midi >> 20) & 0x0Fu);
        event[1] = (u8)(link_midi >> 16);
        event[2] = (u8)(link_midi >> 8);
        event[3] = (u8)link_midi;
        data = event;
        n = 4u;
        link_midi = 0u;
    }
    if (n == 0u && (main_answer != 0u || main_note != 0u)) {


        static u8 text[20] = { 0x04u, 0xF0u, 0x00u, 0x40u, 0x04u, 0x05u, 0x50u, 0x44u, 0x04u, 0x4Au, 0x58u, 0x00u };
        if (main_answer != 0u) {
            text[12] = 0x07u; text[13] = 0x66u; text[14] = (u8)(main_answer - 1u); text[15] = 0xF7u;
            n = 16u;
            main_answer = 0u;
            feed_answers++;
        } else {
            text[12] = 0x04u; text[13] = 0x67u; text[14] = (u8)main_note; text[15] = 0x00u;
            text[16] = 0x05u; text[17] = 0xF7u; text[18] = 0x00u; text[19] = 0x00u;
            n = 20u;
            main_note = 0u;
        }
        data = text;
    }
    if (n != 0u || isr_calls == 16000u)
        link_counter++;
    p[0x18] = (u8)link_counter;
    p[0x19] = (u8)n;
    if (isr_calls == 16000u)
        p[0x19] |= 0x80u;
    for (i = 0u; i < n; i++)
        p[0x1Au + i] = data[i];
    for (i = 0u; i < 12u; i++)
        p[0x5Au + i] = (u8)(i % 10u);


    {
        static const u8 panel[TUNNEL_IN_BYTES] = { 0x08u, 0x04u, 0x02u, 0x02u, 0x04u, 0x00u, 0x14u,
                                                   0x60u, 0x00u, 0x00u, 0x00u, 0x00u };
        for (i = 0u; i < TUNNEL_IN_BYTES; i++)
            p[TUNNEL_IN_AT + i] = panel[i];
        if (isr_calls >= 20000u && isr_calls < 20800u)
            p[TUNNEL_IN_AT + 8u] |= 0x40u;
        if (isr_calls >= 24000u && isr_calls < 24400u)
            p[TUNNEL_IN_AT + 7u] |= 0x02u;

        if (link_command != 0u) {
            p[TUNNEL_IN_AT + 10u] = (u8)link_command;
            p[TUNNEL_IN_AT + 11u] = (u8)~link_command;
        }
    }
    for (i = 0u; i < LINK_PACKET_BYTES - 2u; i++)
        sum += p[i];
    sum = (sum & 0xFFu) + (sum >> 8);
    p[LINK_PACKET_BYTES - 2u] = (u8)~(sum + (sum >> 8));


    for (i = 0u; i < 32u; i++)
        sim_link_rx[i] = p[i ^ 1u];
    for (i = 32u; i < LINK_PACKET_BYTES; i++)
        link_late[i] = p[i ^ 1u];
    link_late_pending = 1u;
}

static void link_packet_rest(void)
{
    u32 i;
    if (!link_late_pending)
        return;
    for (i = 32u; i < LINK_PACKET_BYTES; i++)
        sim_link_rx[i] = link_late[i];
    link_late_pending = 0u;
}


static void inject(u32 blocks)
{
    u32 index, before;
    for (index = 0u; index < blocks; index++) {
        now_us += 250u;
        if (isr_calls % 8u == 0u)
            link_packet();
        else if (isr_calls % 8u == 3u)
            link_packet_rest();
        before = stream_sim_captured_blocks();
        stream_capture_isr();
        if (stream_sim_captured_blocks() != before) {

            fifo[fifo_head++ & 0xFFFFu] = (u16)(current_value | (stream_sim_coarse() ? 0x8000u : 0u));
            if (stream_sim_coarse() && stream_sim_deep())
                coarse_stored++;
            if (take_stored++ == 0u)
                take_first = isr_calls;
            take_last = isr_calls;
            if (fifo_head - fifo_tail > 65536u)
                data_ok = 0u;
        }
        isr_calls++;
    }
}

void sim_stock_background_callback(void) { now_us += 100u; }
u32 sim_time_us(void) { return now_us; }


void *sim_pool_alloc(u32 size)
{
    u32 index;
    alloc_calls++;
    if (first_alloc_us == 0xFFFFFFFFu)
        first_alloc_us = now_us;
    if (alloc_calls == POOL_FAIL_AT)
        return 0;
    if (size == MAP_BLOCK_BYTES) {
        if (map_allocs++ < map_alloc_fails || map_used)
            return 0;
        map_used = 1u;
        return (void *)sim_map_buffer;
    }
    if (size != POOL_BLOCK_BYTES)
        return 0;
    for (index = 0u; index < POOL_BLOCKS; index++) {
        if (!pool_used[index]) {
            pool_used[index] = 1u;
            return (void *)sim_pool_buffer[index];
        }
    }
    return 0;
}

int sim_pool_release(void *block, u32 size)
{
    u32 index;
    release_calls++;
    if (block == (void *)sim_map_buffer) {
        if (size != MAP_BLOCK_BYTES || !map_used)
            return -3;
        map_used = 0u;
        return release_calls == RELEASE_FAIL_AT ? -9 : 0;
    }
    if (size != POOL_BLOCK_BYTES)
        return -1;
    for (index = 0u; index < POOL_BLOCKS; index++) {
        if ((void *)sim_pool_buffer[index] == block && pool_used[index]) {
            pool_used[index] = 0u;
            return release_calls == RELEASE_FAIL_AT ? -9 : 0;
        }
    }
    return -2;
}


void *sim_volume_list(void) { return &fake_fs; }

int sim_find_volume(int letter, void **fs)
{
    if (letter != 'C')
        return -12;
    *fs = &fake_fs;
    return 0;
}

int sim_media_check(void *fs) { return fs == &fake_fs && stick_present ? 0 : -19; }

u16 sim_fs_sectors_per_cluster(void *fs) { return fs == &fake_fs ? SPC + GEOMETRY_LIE : 0u; }
u16 sim_fs_reserved_sectors(void *fs) { return fs == &fake_fs ? RESERVED : 0u; }
u16 sim_fs_fat_count(void *fs) { return fs == &fake_fs ? FAT_COUNT : 0u; }
u32 sim_fs_fat_sectors(void *fs) { return fs == &fake_fs ? FAT_SECTORS : 0u; }
u8 sim_fs_type(void *fs) { return fs == &fake_fs ? (USER_LAYOUT == 5u ? 0x10u : 0x20u) : 0u; }


static u8 *meta_sector(u32 sector)
{
    if (sector == 0u)
        return boot;
    if (sector == 1u)
        return fsinfo;
    if (sector >= RESERVED && sector < RESERVED + FAT_SECTORS)
        return &fat[0][(sector - RESERVED) * 512u];
    if (sector >= RESERVED + FAT_SECTORS && sector < DATA_START)
        return &fat[1][(sector - RESERVED - FAT_SECTORS) * 512u];
    if (sector >= ROOT_SECTOR && sector < ROOT_SECTOR + SPC)
        return &root[(sector - ROOT_SECTOR) * 512u];
    return 0;
}

static u8 *stored_header(u32 sector, int create)
{
    u32 index;
    for (index = 0u; index < header_count; index++) {
        if (headers[index].sector == sector)
            return headers[index].data;
    }
    if (!create || header_count == MAX_HEADERS)
        return 0;
    headers[header_count].sector = sector;
    return headers[header_count++].data;
}

int sim_block_read(void *fs, u32 sector, void *data, u16 count)
{
    u8 *d = (u8 *)data;
    u32 n, i;

    block_reads++;
    if (first_io_us == 0xFFFFFFFFu)
        first_io_us = now_us;
    inject(2u + count / 4u);
    if (!stick_present)
        dead_calls++;
    if (!stick_present || fs != &fake_fs || count == 0u || count > FR_FAT_BATCH ||
        sector + count > TOTAL_SECTORS)
        return -1;
    if (count > max_read_sectors)
        max_read_sectors = count;
    for (n = 0u; n < count; n++, d += 512u) {
        const u8 *source = meta_sector(sector + n);
        if (source == 0)
            source = sector + n == log_first_sector ? log_sector : stored_header(sector + n, 0);
        if (source != 0) {
            copy_bytes(d, source, 512u);
        } else {
            for (i = 0u; i < 512u; i++)
                d[i] = 0xA5u;
        }
    }
    return 0;
}


static u32 sample_size(void)
{
    return stream_sim_deep() ? 3u : 2u;
}


static void inspect_audio(const u8 *data, u32 bytes)
{
    u32 index, in_block;
    u32 sb = sample_size(), block_bytes = 24u * sb;
    s32 sample;
    for (index = 0u; index < bytes; index++) {
        if (take_audio_bytes >= take_stored * block_bytes) {
            if (data[index] != 0u)
                data_ok = 0u;
            continue;
        }
        in_block = take_audio_bytes++ % block_bytes;
        file_audio_bytes++;
        if (in_block == 0u) {
            if (fifo_tail == fifo_head) {
                data_ok = 0u;
                return;
            }
            check_call = fifo[fifo_tail++ & 0xFFFFu];
            check_coarse = check_call >> 15;
            check_call &= 0x7FFFu;
            if (check_coarse && sb == 3u) {
                if (file_coarse_blocks++ == 0u)
                    file_coarse_first = (file_audio_bytes - 1u) / FRAME_BYTES;
            }
            if (!check_coarse && sb == 2u)
                data_ok = 0u;
        }
        if (in_block % sb == 0u) {
            sample = check_call ? audio_sample(check_call, in_block / sb) : 0;
            if (check_coarse) {
                check_state = check_state * 1664525u + 1013904223u;
                sample += 128 + (s32)((check_state >> 16) & 0xFFu) - (s32)(check_state >> 24);
                sample >>= 8;
                if (sample > 32767)
                    sample = 32767;
                if (sample < -32768)
                    sample = -32768;
                if (sb == 3u)
                    sample = (s32)((u32)sample << 8);
            }
            check_low = (u32)sample;
        }
        if (data[index] != (u8)(check_low >> (8u * (in_block % sb))))
            data_ok = 0u;
    }
}


static void inspect_header(const u8 *h)
{
    u32 data_bytes = le32(h, 508u);
    u32 pages = (chain_length * SPC * 512u - 512u - CUE_RESERVE_BYTES) / PAGE_BYTES;
    u32 capacity = (pages > audio_page_limit ? audio_page_limit : pages) * PAGE_BYTES;
    u32 cue = le32(h, 60u) == TAKE_COMPLETE ? file_cue_bytes : 0u;
    u32 sb = sample_size();
    if (!text(h + 8u, "WAVEfmt ", 8u) || !text(h + 36u, "JUNK", 4u) ||
        !text(h + 44u, "DJMREC44", 8u) || !text(h + 504u, "data", 4u) ||
        le32(h, 16u) != 16u || le16(h, 20u) != 1u || le16(h, 22u) != 2u ||
        le32(h, 24u) != 48000u || le32(h, 28u) != 96000u * sb || le16(h, 32u) != 2u * sb ||
        le16(h, 34u) != 8u * sb || le32(h, 40u) != 460u || le32(h, 52u) != REPORT_SCHEMA ||
        le32(h, 4u) != 504u + data_bytes + cue || REPORT_END_OFFSET > 504u ||
        data_bytes > file_audio_bytes || data_bytes % (2u * sb) != 0u ||
        (le32(h, 60u) == TAKE_COMPLETE &&
         (data_bytes != file_audio_bytes ||
          (data_bytes != capacity && take_audio_bytes != take_stored * 24u * sb &&
           !(cut_pending && data_bytes % PAGE_BYTES == 0u)))))
        header_ok = 0u;
    take_header_bytes = data_bytes;
}


static void follow_chain(u32 first_sector)
{
    u32 cluster = (first_sector - DATA_START) / SPC + 2u;
    chain_length = 0u;
    while (cluster >= 2u && cluster < END_CLUSTER && chain_length < CLUSTERS) {
        chain[chain_length++] = cluster;
        cluster = fat_get(cluster);
    }
}


static int in_file(u32 sector, u32 index, u32 count)
{
    u32 n;
    for (n = 0u; n < count; n++) {
        u32 at = index + n;
        if (at / SPC >= chain_length ||
            sector + n != DATA_START + (chain[at / SPC] - 2u) * SPC + at % SPC)
            return 0;
    }
    return 1;
}

int sim_block_write(void *fs, u32 sector, const void *data, u16 count)
{
    const u8 *d = (const u8 *)data;
    u8 *meta;
    u32 delay = 12u, i, n;

    if (first_io_us == 0xFFFFFFFFu)
        first_io_us = now_us;
    if (!stick_present || power_lost) {
        if (!power_lost)
            dead_calls++;
        inject(8u);
        return power_lost ? 0 : -1;
    }
    if (fs != &fake_fs || count == 0u || count > MAX_BLOCK_SECTORS ||
        sector + count > TOTAL_SECTORS) {
        bounds_ok = 0u;
        return -1;
    }
    if (log_first_sector != 0u && sector == log_first_sector + 1u && count == LISTEN_SECTORS &&
        LOG_FILE != 2u) {
        copy_bytes(listen_sectors, d, LISTEN_SECTORS * 512u);
        listen_writes++;
        return 0;
    }
    if (log_first_sector != 0u && sector == log_first_sector + 1u + LISTEN_SECTORS &&
        count == TUNNEL_SECTORS && LOG_FILE == 1u) {
        copy_bytes(tunnel_sectors, d, TUNNEL_SECTORS * 512u);
        tunnel_writes++;
        return 0;
    }
    meta = meta_sector(sector);
    if (meta != 0 || sector == log_first_sector) {

        if (sector == 0u || (meta != 0 && meta_sector(sector + count - 1u) == 0)) {
            bounds_ok = 0u;
            return -1;
        }
        if (sector == log_first_sector) {
            copy_bytes(log_sector, d, 512u);
            log_writes++;
        } else {
#ifdef TRACE_META
            out("W ");
            field("index", meta_writes);
            field("sector", sector);
            field("count", count);
            field("isr", isr_calls);
            out("\n");
#endif
            if (meta_writes == META_CUT_AT) {
                power_lost = 1u;
                cuts++;
                cut_phase = stream_sim_phase();
                meta_writes++;
                return 0;
            }
            if (meta_writes == META_FAIL_AT) {
                meta_writes++;
                inject(8u);
                return -1;
            }
            for (n = 0u; n < count; n++)
                copy_bytes(meta_sector(sector + n), d + n * 512u, 512u);
            meta_writes++;
        }
        block_writes++;
        inject(delay);
        return 0;
    }
    if (take_writes == WRITE_FAIL_AT) {
        take_writes++;
        inject(8u);
        return -1;
    }
    if (count == 1u && text(d, "RIFF", 4u) && text(d + 44u, "DJMREC44", 8u) &&
        (sector - DATA_START) % SPC == 0u) {
        u32 state = le32(d, 60u);
        u8 *slot = stored_header(sector, 1);
        if (slot == 0) {
            bounds_ok = 0u;
            return -1;
        }
        if (state == TAKE_NONE) {
            idle_header_writes++;
        } else if (state == TAKE_COMPLETE && take_stored == 0u && take_headers == 0u &&
                   stream_sim_phase() != PHASE_STREAM) {


            u32 i2;
            for (i2 = 0u; i2 < 512u; i2++) {
                if (slot[i2] != d[i2] && (i2 < 60u || i2 > 63u) && !(recover_tail_bytes != 0u && i2 >= 4u && i2 < 8u))
                    header_ok = 0u;
            }
            if (le32(slot, 60u) != TAKE_RECORDING ||
                le32(d, 4u) != 504u + le32(d, 508u) + recover_tail_bytes)
                header_ok = 0u;
            recover_tail_bytes = 0u;
            closed_headers++;
        } else {
            if (take_headers == 0u) {
                take_file = sector;
                follow_chain(sector);
                file_bits = le16(d, 34u);
            }
            if (take_file != sector)
                bounds_ok = 0u;
            inspect_header(d);
            take_headers++;
        }
        copy_bytes(slot, d, 512u);
    } else {
        u32 tail = file_audio_bytes % 512u;
        if (take_file != 0u && count == 1u && text(d, "DJMK", 4u)) {

            u32 n_marks = le16(d, SHEET_COUNT_AT), n_lost = d[SHEET_COUNT_AT + 2u], k, sum = 0u;
            u32 n_coarse = d[SHEET_COUNT_AT + 3u];
            u8 *slot = stored_header(sector, 1);
            for (k = 0u; k < SHEET_SUM_AT; k++)
                sum = (sum << 1 | sum >> 31) + d[k];
            if (!in_file(sector, chain_length * SPC - 1u, 1u) || slot == 0 || n_marks > MAX_MARKS ||
                n_lost > LOST_MARKS || n_coarse > COARSE_MARKS || le32(d, SHEET_SUM_AT) != sum || le32(d, 4u) == 0u)
                sheet_ok = 0u;
            for (k = 0u; k + 1u < n_marks && sheet_ok; k++) {
                if (le32(d, SHEET_POSITIONS_AT + 4u * k) > le32(d, SHEET_POSITIONS_AT + 4u * k + 4u))
                    sheet_ok = 0u;
            }
            if (slot != 0)
                copy_bytes(slot, d, 512u);
            sheet_writes++;
            block_writes++;
            inject(9u);
            return 0;
        }
        if (text(d + (take_file != 0u ? tail : 0u), "cue ", 4u) &&
            (take_file == 0u || in_file(sector, 1u + take_sectors - (tail ? 1u : 0u), count))) {


            u32 off = take_file != 0u ? tail : 0u;
            u32 n = le32(d, off + 8u), at = off + 12u, k, previous = 0u, lost_n = 0u, bytes;
            u32 named = 0u, coarse_n = 0u, coarse_first = 0xFFFFFFFFu, points = at;
            if (le32(d, off + 4u) != 4u + 24u * n || n == 0u || n > ALL_MARKS ||
                (take_file != 0u && file_cue_bytes != 0u))
                cue_ok = 0u;
            for (k = 0u; k < off; k++) {
                if (d[k] != last_audio_sector[k])
                    cue_ok = 0u;
            }
            for (k = 0u; k < n && cue_ok; k++, at += 24u) {
                u32 position = le32(d, at + 4u);
                if (le32(d, at) != k + 1u || !text(d + at + 8u, "data", 4u) || le32(d, at + 12u) != 0u ||
                    le32(d, at + 16u) != 0u || le32(d, at + 20u) != position ||
                    (take_file != 0u && position >= file_audio_bytes / (2u * sample_size())) || position < previous)
                    cue_ok = 0u;
                previous = position;
                if (k < 4u)
                    file_cue_position[k] = position;
            }
            if (cue_ok && at + 12u <= (u32)count * 512u && text(d + at, "LIST", 4u)) {
                u32 last_id = 0u;
                named = (le32(d, at + 4u) - 4u) / CUE_LABEL_BYTES;
                if (!text(d + at + 8u, "adtl", 4u) || named == 0u || named > NAMED_MARKS || named > n ||
                    le32(d, at + 4u) != 4u + CUE_LABEL_BYTES * named)
                    cue_ok = 0u;
                at += 12u;
                for (k = 0u; k < named && cue_ok; k++, at += CUE_LABEL_BYTES) {
                    u32 id = le32(d, at + 8u);
                    if (!text(d + at, "labl", 4u) || id <= last_id || id > n || d[at + 17u] != 0u)
                        cue_ok = 0u;
                    else if (le32(d, at + 4u) == 9u && text(d + at + 12u, "LOST", 4u) && d[at + 16u] == 0u)
                        lost_n++;
                    else if (le32(d, at + 4u) == 10u && text(d + at + 12u, "16BIT", 5u)) {
                        if (coarse_n++ == 0u)
                            coarse_first = le32(d, points + 24u * (id - 1u) + 4u);
                    } else
                        cue_ok = 0u;
                    last_id = id;
                }
                if (lost_n > LOST_MARKS || coarse_n > COARSE_MARKS)
                    cue_ok = 0u;
            }
            bytes = at - off;
            if ((off + bytes + 511u) / 512u != count)
                cue_ok = 0u;
            for (; at < (u32)count * 512u; at++) {
                if (d[at] != 0u)
                    cue_ok = 0u;
            }
            for (k = 0u; k < count; k++) {
                u8 *slot = stored_header(sector + k, 1);
                if (slot != 0)
                    copy_bytes(slot, d + k * 512u, 512u);
                else
                    cue_ok = 0u;
            }
            if (take_file != 0u) {
                file_cue_bytes = bytes;
                file_cue_marks = n;
                file_cue_lost = lost_n;
                file_cue_coarse = coarse_n;
                file_cue_coarse_first = coarse_first;
            } else {
                recover_tail_bytes = bytes;
                recover_tails++;
            }
            block_writes++;
            inject(8u + count);
            return 0;
        }
        if (take_file == 0u && count == 1u) {


            u32 cluster = (sector - DATA_START) / SPC + 2u, k, zero = 0u, mine = fat_get(cluster) == 0u, e, c, guard;
            u8 *slot = stored_header(sector, 1);
            for (k = 0u; k < 512u; k++) {
                if (d[k] == 0u)
                    zero = 1u;
                else if (zero || !(d[k] == '\r' || d[k] == '\n' || (d[k] >= 0x20u && d[k] < 0x7Fu)))
                    text_ok = 0u;
            }
            for (e = 0u; e < SPC * 16u && !mine; e++) {
                const u8 *x = &root[e * 32u];
                if (!text(x, "REC-L", 5u) || x[0] == 0xE5u)
                    continue;
                c = (le16(x, 20u) << 16) | le16(x, 26u);
                for (guard = 0u; c >= 2u && c < END_CLUSTER && guard < CLUSTERS; guard++, c = fat_get(c)) {
                    if (c == cluster)
                        mine = 1u;
                }
            }
            if (!mine || slot == 0)
                text_ok = 0u;
            if (slot != 0)
                copy_bytes(slot, d, 512u);
            text_writes++;
            block_writes++;
            inject(9u);
            return 0;
        }

        if (take_file == 0u || !in_file(sector, 1u + take_sectors, count)) {
            bounds_ok = 0u;
            return -1;
        }
        inspect_audio(d, (u32)count * 512u);
        copy_bytes(last_audio_sector, d + ((u32)count - 1u) * 512u, 512u);
        take_sectors += count;
        delay = 8u + count;
    }
    for (i = 0u; i < sizeof(stalls) / sizeof(stalls[0]); i++) {
        if (stalls[i].write == take_writes)
            delay = stalls[i].blocks;
    }
    take_writes++;
    block_writes++;
    if (count > max_write_sectors)
        max_write_sectors = count;
    inject(delay);
    return 0;
}


static void close_file(u32 kind)
{
    struct take_record *t;
    if (take_count < MAX_TAKES) {
        t = &takes[take_count++];
        t->kind = kind;
        t->first_sector = take_file;
        t->stored = take_stored;
        t->audio_bytes = file_audio_bytes;
        t->take_audio = take_audio_bytes;
        t->sectors = take_sectors;
        t->dropped = take_stored ? take_last - take_first + 1u - take_stored : 0u;
        t->fifo_left = fifo_head - fifo_tail;
        t->headers = take_headers;
        t->header_bytes = take_header_bytes;
        t->cue_marks = file_cue_marks;
        t->cue_lost = file_cue_lost;
        t->cue_coarse = file_cue_coarse;
        t->cue_coarse_first = file_cue_coarse_first;
        t->coarse_blocks = file_coarse_blocks;
        t->coarse_first = file_coarse_first;
        t->bits = file_bits;
        t->cue_position[0] = file_cue_position[0];
        t->cue_position[1] = file_cue_position[1];
        t->cue_position[2] = file_cue_position[2];
        t->cue_position[3] = file_cue_position[3];
    }
    file_cue_bytes = 0u;
    file_cue_marks = 0u;
    file_cue_lost = 0u;
    file_cue_coarse = 0u;
    file_cue_coarse_first = 0xFFFFFFFFu;
    file_coarse_blocks = 0u;
    file_coarse_first = 0xFFFFFFFFu;
    file_audio_bytes = 0u;
    take_sectors = 0u;
    take_headers = 0u;
    take_header_bytes = 0u;
    take_file = 0u;
}

static void close_take(u32 kind)
{
    close_file(kind);
    fifo_tail = fifo_head;
    take_stored = 0u;
    take_audio_bytes = 0u;
    take_writes = 0u;
    check_state = 0u;
}

void sim_event(u32 code)
{
    switch (code) {
    case EVENT_ARM:
        arms++;
        if (fifo_head != fifo_tail || take_stored != 0u)
            data_ok = 0u;
        break;
    case EVENT_START: starts++; break;
    case EVENT_KEEP: keeps++; close_take(code); break;
    case EVENT_DISCARD: discards++; close_take(code); break;
    case EVENT_ABORT: aborts++; close_take(code); break;
    case EVENT_FAIL: fails++; close_take(code); break;
    case EVENT_READY: readies++; break;
    case EVENT_SWITCH: switches++; close_file(code); break;
    case EVENT_CUT: cut_pending = 1u; break;
    case EVENT_SPLIT: splits++; cut_pending = 0u; close_file(code); break;
    default: break;
    }
}

static void power_on(void)
{
    u32 index;
    for (index = 0u; index < LOW_AUDIT_BYTES; index++)
        sim_low_buffer[index] = 0xFFu;
    for (index = 0u; index < STAGE_AUDIT_BYTES; index++)
        sim_stage_buffer[index] = 0u;
    for (index = 0u; index < SIM_GUARD_WORDS; index++)
        sim_guard[index] = 0xFFFFFFFFu;
    for (index = 0u; index < POOL_BLOCKS; index++)
        pool_used[index] = 0u;
    map_used = 0u;
    power_lost = 0u;
    sim_dma3_rdmatcr = 24u;
#ifdef TEST_DIRTY
    sim_stage_buffer[STAGE_AUDIT_BYTES - 5u] = 1u;
#endif
}

static void reboot(void)
{
    if (take_stored != 0u || take_headers != 0u)
        close_take(0u);
    reboots++;
    power_on();
    stream_sim_reboot();


    main_feed = 0u;
    main_answer = 0u;
    main_note = 0u;
    main_pipe_known = 0u;
}

static const u32 extra_ends[3] = { 20u, 22u, 30u };

static void touch_waiting_file(void)
{
    u32 e;
    for (e = 0u; e < SPC * 512u / 32u; e++) {
        u8 *x = &root[e * 32u];
        if (text(x, "DJMREC  TMP", 11u)) {
            put_le16(x, 22u, 0x7CE3u);
            put_le16(x, 24u, 0x5B4Du);
        }
    }
}

static void run_events(void)
{
    if (power_lost)
        reboot();
    while (event_index < sizeof(events) / sizeof(events[0]) &&
           isr_calls >= events[event_index].at) {

        if (events[event_index].action == ACT_DIRTY_EXTRA && (events[event_index].arg & 0xFFu) != 0u &&
            stream_sim_phase() != (events[event_index].arg & 0xFFu))
            return;
        switch (events[event_index].action) {
        case ACT_REBOOT: reboot(); break;
        case ACT_STICK_OUT: stick_present = 0u; break;
        case ACT_STICK_IN: stick_present = 1u; break;
        case ACT_SWAP: put_le32(boot, 67u, le32(boot, 67u) + 1u); break;
        case ACT_CORRUPT_MAP: sim_map_buffer[3] ^= 0x10u; break;
        case ACT_DIRTY_EXTRA:
            sim_extra_buffer[extra_ends[(events[event_index].arg >> 8) % 3u] * PAGE_BYTES - 5000u] ^= 0x5Au;
            break;
        case ACT_DMA_COUNT: sim_dma3_rdmatcr = events[event_index].arg; break;
        case ACT_MIDI: link_midi = events[event_index].arg | 0x80000000u; break;
        case ACT_FEED_REFUSE: main_refuses = events[event_index].arg; break;
        case ACT_NOTE: main_note = events[event_index].arg; break;
        case ACT_GUARD: sim_guard[SIM_GUARD_WORDS - 3u] = 0x12345678u; break;
        case ACT_TOUCH: touch_waiting_file(); break;
        case ACT_COMMAND:
            link_command = ((((link_command >> 4) % 15u) + 1u) << 4) | (events[event_index].arg & 0x0Fu);
            break;
        default: break;
        }
        event_index++;
    }
}


static void out(const char *s)
{
    int length = 0;
    while (s[length])
        length++;
    sim_write(s, length);
}

static void field(const char *name, u32 value)
{
    char buffer[9];
    int index;
    out(name);
    out("=");
    for (index = 7; index >= 0; index--, value >>= 4)
        buffer[index] = "0123456789abcdef"[value & 15u];
    buffer[8] = ' ';
    sim_write(buffer, 9);
}

static void hex_bytes(const u8 *data, u32 count)
{
    static char line[1024];
    u32 index;
    for (index = 0u; index < count; index++) {
        line[index * 2u] = "0123456789abcdef"[data[index] >> 4];
        line[index * 2u + 1u] = "0123456789abcdef"[data[index] & 15u];
    }
    sim_write(line, (int)(count * 2u));
}


static void dump_sector(const char *tag, u32 sector, const u8 *data)
{
    u32 index;
    for (index = 0u; index < 512u && data[index] == 0u; index++)
        ;
    if (index == 512u)
        return;
    out(tag);
    out(" ");
    field("sector", sector);
    hex_bytes(data, 512u);
    out("\n");
}

static void dump_volume(const char *tag)
{
    u32 sector;
    dump_sector(tag, 0u, boot);
    dump_sector(tag, 1u, fsinfo);
    for (sector = RESERVED; sector < DATA_START + SPC; sector++)
        dump_sector(tag, sector, meta_sector(sector));
    for (sector = 0u; sector < header_count; sector++)
        dump_sector(tag, headers[sector].sector, headers[sector].data);
    dump_sector(tag, log_first_sector ? log_first_sector : TOTAL_SECTORS - 1u, log_sector);
    if (log_first_sector != 0u) {
        for (sector = 0u; sector < LISTEN_SECTORS; sector++)
            dump_sector(tag, log_first_sector + 1u + sector, listen_sectors + sector * 512u);
        for (sector = 0u; sector < TUNNEL_SECTORS; sector++)
            dump_sector(tag, log_first_sector + 1u + LISTEN_SECTORS + sector,
                        tunnel_sectors + sector * 512u);
    }
}

void harness(void)
{
    u32 index, leaks = 0u;

    make_volume();
    out("G ");
    field("total_sectors", TOTAL_SECTORS);
    out("\n");
    dump_volume("B");
    for (index = 0u; index < EXTRA_BYTES; index += 97u)
        sim_extra_buffer[index] = (u8)(index * 7u + 1u);
    for (index = 0u; index < sizeof(schedule) / sizeof(schedule[0]); index++)
        schedule_total += schedule[index].blocks;
    schedule_end = schedule[0].blocks;
    power_on();


    while (isr_calls < schedule_total) {
        run_events();
        inject(4u);
        recorder_service();
    }
    if (take_stored != 0u || take_headers != 0u)
        close_take(0u);
    for (index = 0u; index < POOL_BLOCKS; index++)
        leaks += pool_used[index];
    leaks += map_used;

    out("R ");
    field("block_reads", block_reads);
    field("block_writes", block_writes);
    field("meta_writes", meta_writes);
    field("idle_header_writes", idle_header_writes);
    field("log_writes", log_writes);
    field("cue_ok", cue_ok && sheet_ok && text_ok);
    field("sheet_writes", sheet_writes);
    field("text_writes", text_writes);
    field("recover_tails", recover_tails);
    field("dead_calls", dead_calls);
    field("splits", splits);
    field("listen_writes", listen_writes);
    field("tunnel_writes", tunnel_writes);
    field("link_tx_torn", link_tx_torn);
    field("status_seen", status_seen);
    field("status_bad", status_bad);
    field("status_steps", status_steps);
    field("status_states", status_state_seen);
    field("status_flags", status_flags_seen);
    field("feed_on", feed_on_requests);
    field("feed_off", feed_off_requests);
    field("feed_bad", feed_bad);
    field("feed_answers", feed_answers);
    field("feed_refusals", feed_refusals);
    field("feed_changes", feed_changes);
    field("feed_now", main_feed);
    field("status_seconds", status_most_seconds);
    field("status_marks", status_most_marks);
    field("status_files", status_most_files);
    field("status_file2_first", status_file2_first);
    field("status_file2_most", status_file2_most);
    field("status_file2_marks_first", status_file2_marks_first);
    field("alloc", alloc_calls);
    field("release", release_calls);
    field("leaks", leaks);
    field("data_ok", data_ok);
    field("bounds_ok", bounds_ok);
    field("header_ok", header_ok);
    field("max_write_sectors", max_write_sectors);
    field("max_read_sectors", max_read_sectors);
    field("first_io_us", first_io_us);
    field("first_alloc_us", first_alloc_us);
    field("arms", arms);
    field("starts", starts);
    field("keeps", keeps);
    field("discards", discards);
    field("aborts", aborts);
    field("fails", fails);
    field("reboots", reboots);
    field("readies", readies);
    field("cuts", cuts);
    field("closed_headers", closed_headers);
    field("switches", switches);
    field("status", stream_sim_status_flags());
    field("phase", stream_sim_phase());
    field("capture", stream_sim_capture_state());
    out("\n");
    for (index = 0u; index < take_count; index++) {
        out("T ");
        field("kind", takes[index].kind);
        field("first_sector", takes[index].first_sector);
        field("stored", takes[index].stored);
        field("audio_bytes", takes[index].audio_bytes);
        field("take_audio", takes[index].take_audio);
        field("sectors", takes[index].sectors);
        field("dropped", takes[index].dropped);
        field("fifo_left", takes[index].fifo_left);
        field("headers", takes[index].headers);
        field("header_bytes", takes[index].header_bytes);
        field("cue_marks", takes[index].cue_marks);
        field("cue_lost", takes[index].cue_lost);
        field("cue_coarse", takes[index].cue_coarse);
        field("cue_coarse_first", takes[index].cue_coarse_first);
        field("coarse_blocks", takes[index].coarse_blocks);
        field("coarse_first", takes[index].coarse_first);
        field("bits", takes[index].bits);
        field("cue0", takes[index].cue_position[0]);
        field("cue1", takes[index].cue_position[1]);
        field("cue2", takes[index].cue_position[2]);
        field("cue3", takes[index].cue_position[3]);
        out("\n");
    }
    for (index = 0u; index < view_count; index++) {
        out("V ");
        field("block", views[index].block);
        field("state", views[index].state);
        field("reason", views[index].reason);
        field("cut", views[index].cut);
        out("\n");
    }
    for (index = 0u; index < count_count; index++) {
        out("C ");
        field("block", counts[index].block);
        field("left", counts[index].left);
        out("\n");
    }
    out("W ");
    field("view_lost", view_lost);
    field("count_lost", count_lost);
    field("count_bad", count_bad);
    field("left_seen", left_seen);
    field("left_ready", left_ready);
    field("left_count_diff", left_count_diff);
    field("left_sum_min", left_sum_min);
    field("left_sum_max", left_sum_max);
    field("cut_phase", cut_phase);
    field("space_seen", space_seen);
    field("space_bad", space_bad);
    field("space_first", space_first);
    field("coarse_stored", coarse_stored);
    out("\n");
    dump_volume("A");
    out("DONE\n");
}
