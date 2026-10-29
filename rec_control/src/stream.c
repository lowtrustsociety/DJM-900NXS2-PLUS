/* The recorder: capture, ring, writing, markers, files.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "stream.h"
#include "reclog.h"

#define SETTLE_US             5000000u
#define RETRY_ABSENT_US       5000000u
#define RETRY_UNUSABLE_US    30000000u
#define MAX_WRITES_PER_SERVICE      8u
#define MAX_FAILED_TAKES            3u
#define JUNK_PAYLOAD_BYTES        460u
#define VERIFY_CHUNK_WORDS         64u
#define EXTRA_STEP_WORDS          864u
#define MIN_SENTINEL         0xFFFFFFFFu
#define AUDIO_TRIGGER_THRESHOLD  25000
#define FS_TYPE_FAT32            0x20u
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")


#define HEADER_BUFFER ((u8 *)LOW_BUFFER)
#define IDLE_BUFFER   (HEADER_BUFFER + SECTOR_BYTES)
#define DIR_BUFFER    (HEADER_BUFFER + 2u * SECTOR_BYTES)
#define FAT_BUFFER    (HEADER_BUFFER + 3u * SECTOR_BYTES)
#define LOW_PAGE_BASE (LOW_BUFFER + LOW_BUFFER_BYTES)
#define OUT_PAGE      (LOW_PAGE_BASE + LOW_PAGES * PAGE_BYTES)

_Static_assert(REPORT_END_OFFSET <= 504u, "report must end before the data chunk header");
_Static_assert((3u + FR_FAT_BATCH) * SECTOR_BYTES == LOW_BUFFER_BYTES &&
               LOW_BUFFER_BYTES + (LOW_PAGES + 1u) * PAGE_BYTES == LOW_AUDIT_BYTES, "low region");
_Static_assert(STAGE_PAGES * PAGE_BYTES == STAGE_AUDIT_BYTES, "stage region");
_Static_assert(EXTRA_PAGES * PAGE_BYTES == EXTRA_BYTES, "extra region");
_Static_assert(BASE_PAGES + EXTRA_PAGES + POOL_PAGES == MAX_PAGES, "ring");
_Static_assert(sizeof(struct listen_log) == LISTEN_SECTORS * SECTOR_BYTES, "listen record");
_Static_assert(sizeof(struct tunnel_log) == TUNNEL_SECTORS * SECTOR_BYTES, "tunnel record");
_Static_assert(CUE_RESERVE_BYTES <= RF_TAIL_MAX, "what follows the audio");
_Static_assert(CUE_MAX_BYTES + SECTOR_BYTES <= CUE_RESERVE_BYTES, "the markers never reach the file's last sector");
_Static_assert(SHEET_POSITIONS_AT + 4u * ALL_MARKS <= SHEET_SUM_AT, "the saved markers fit a sector");
_Static_assert(4096u + 4u * ALL_MARKS <= 4608u && 4608u + SECTOR_BYTES <= FR_FAT_BATCH * SECTOR_BYTES &&
               SECTOR_BYTES + CUE_MAX_BYTES <= 4096u, "work areas in the table buffer");
_Static_assert(MAX_MARKS <= 0xFFFFu && LOST_MARKS <= 0xFFu && COARSE_MARKS <= 0xFFu, "the sheet's counts");
_Static_assert(2u * MAX_EXTENTS * sizeof(struct extent) == MAP_BLOCK_BYTES, "map block");
_Static_assert(PAGE_SECTORS * SECTOR_BYTES == PAGE_BYTES && PAGE_BLOCKS * BLOCK_BYTES == PAGE_BYTES &&
               PAGE_WORDS * 4u == PAGE_BYTES && PAGE_BLOCKS * 250u == PAGE_US &&
               PAGE_BLOCKS_16 * 48u == PAGE_BYTES, "page");
_Static_assert(BLOCK_BYTES == 24u * SAMPLE_BYTES && FRAME_BYTES == 2u * SAMPLE_BYTES &&
               BYTE_RATE == 48000u * FRAME_BYTES && PACK_MAX_BYTES >= BLOCK_BYTES, "sample size");
_Static_assert(FILE_AUDIO_PAGES <= 0x7FF00000u / PAGE_BYTES && FILE_CAP_BYTES < 0x80000000u, "file under 2 GB");
#ifndef SIM
_Static_assert(NEXT_AHEAD_WORDS > COUNTDOWN_WORDS, "the next file is tried before the countdown can begin");
#endif


struct work_state {
    void *fs;
    void *map_block;
    void *pool_blocks[POOL_BLOCKS];
    u32 file_sectors;
    u32 capacity_pages;
    u32 geometry_checked;
    u32 prepare_started_us;
    u32 audit_page;
    u32 audit_word;
    u32 audit_sum;
    u32 audit_mismatch;
    u32 extra_sums[EXTRA_PAGES];
    u32 after_snapshot;
    u32 verify_index;
    u32 settle_seen;
    u32 settle_started_us;
    u32 wait_from_us;
    u32 wait_us;
    u32 last_header_us;
    u32 last_service_us;
    u32 service_seen;
    u32 failed_in_row;
    u32 log_key;
    u32 next_tried;
    u32 bursting;
    u32 out_blocks;
    u32 last_length;
    u32 cue_bytes;
    u32 cutting;
    u32 cut_wanted;
    u32 read_left;
    u32 read_coarse;
    u32 out_coarse;
    u32 read_offset;
    struct pack_state unpack;
    u32 close_bytes;
    u32 poll_seen;
    u32 poll_from_us;
    u32 stick_present;
    u32 retry;
    u32 gone_seen;
    u32 cut_to_next;
    u32 header_has_audio;
    u32 error_from_us;
    s32 fail_error;
    u32 fail_logged;
};

static volatile struct capture_state capture = { 0u };
static volatile u32 pages_consumed = 0u;
static volatile u32 page_lengths[MAX_PAGES] = { 0u };
static volatile u32 page_blocks[MAX_PAGES] = { 0u };
static volatile u32 blocks_consumed = 0u;
static volatile u8 page_coarse[MAX_PAGES] = { 0u };
static volatile u32 coarse_now = 0u;
static u32 dither_state = 0u;
static struct pack_state capture_pack = { { 0, 0 }, { 0, 0 } };


static s32 capture_samples[24] = { 0 };
static u8 unpacked[BLOCK_BYTES] = { 0u };
static volatile u8 *page_buffers[MAX_PAGES] = { 0 };


static volatile u32 deep = DEPTH_AT_POWER_ON;


static u32 sample_bytes(void)
{
    return deep ? SAMPLE_BYTES : 2u;
}

static u32 file_page_blocks(void)
{
    return deep ? PAGE_BLOCKS : PAGE_BLOCKS_16;
}
static struct session_state session = { .phase = PHASE_VERIFY_MEMORY };
static struct take_state take = { 0u };
static struct work_state work = { 0 };
static const u32 next_ahead_words = NEXT_AHEAD_WORDS;
static const u32 countdown_words = COUNTDOWN_WORDS;
static const u32 burst_pages = BURST_PAGES;
static const u32 auto_start = AUTO_START;
static const u32 start_key = CONTROL_START_KEY;
static const u32 stop_key = CONTROL_STOP_KEY;
static const u32 mark_key = CONTROL_MARK_KEY;
static const u32 cut_key = CONTROL_CUT_KEY;
static const u32 every_message = CONTROL_EVERY_MESSAGE;
static volatile u32 control_cut = 0u;

static volatile u32 status_reason = REASON_INIT;
static volatile u32 status_closing = 0u;
static volatile u32 cut_hint = CUT_WAIT;
static volatile u32 marks[MAX_MARKS] = { 0u };
static volatile u32 mark_count = 0u;


struct lost_place {
    u32 frame;
    u32 blocks;
};
static volatile struct lost_place lost[LOST_MARKS] = { { 0u, 0u } };
static volatile u32 lost_count = 0u;
static volatile u32 lost_open = 0u;
static volatile u32 lost_more = 0u;
static volatile u32 lost_more_blocks = 0u;


static volatile struct lost_place coarse[COARSE_MARKS] = { { 0u, 0u } };
static volatile u32 coarse_count = 0u;
static volatile u32 coarse_open = 0u;
static volatile u32 coarse_more = 0u;
static volatile u32 coarse_more_blocks = 0u;


static volatile u32 coarse_more_last = 0u;
static volatile u32 coarse_more_frame = 0u;
static volatile u32 coarse_more_run = 0u;
static volatile u32 sheet_dirty = 0u;
static volatile u32 status_stopped = 0u;


static volatile u32 status_free = FR_FREE_UNKNOWN;
static u32 write_cue(u32 last);
static volatile u32 control_start = 0u;
static volatile u32 control_stop = 0u;
static volatile u32 control_hold = 0u;
static volatile u32 control_quiet = 0u;
static const u32 file_audio_pages = FILE_AUDIO_PAGES;
static struct rf files = { .cap_bytes = FILE_CAP_BYTES, .min_bytes = FILE_MIN_BYTES,
                           .refill_below_bytes = FILE_REFILL_BYTES,
                           .margin_bytes = FILE_MARGIN_BYTES };

#ifndef SIM
static u32 irq_lock(void)
{
    u32 saved;
    u32 masked;

    __asm__ __volatile__("stc sr,%0" : "=r"(saved) :: "memory");
    masked = saved | 0xF0u;
    __asm__ __volatile__("ldc %0,sr" :: "r"(masked) : "memory");
    return saved;
}

static void irq_restore(u32 saved)
{
    __asm__ __volatile__("ldc %0,sr" :: "r"(saved) : "memory");
}
#else
static u32 irq_lock(void) { return 0u; }
static void irq_restore(u32 saved) { (void)saved; }
#endif

static void *pool_alloc(u32 bytes)
{
    u32 saved = irq_lock();
    void *result = STOCK_POOL_ALLOC(bytes);
    irq_restore(saved);
    return result;
}

static int pool_release(void *block, u32 bytes)
{
    u32 saved = irq_lock();
    int result = STOCK_POOL_RELEASE(block, bytes);
    irq_restore(saved);
    return result;
}

static int pool_address_valid(void *block, u32 bytes)
{
#ifdef SIM
    (void)bytes;
    return block != 0;
#else
    u32 address = (u32)block;
    return address >= 0x1C140000u && address < 0x1C180000u &&
           bytes <= 0x1C180000u - address && (address & 3u) == 0u;
#endif
}

static void put_le16(u8 *destination, u16 value)
{
    destination[0] = (u8)value;
    destination[1] = (u8)(value >> 8);
}

static void put_le32(u8 *destination, u32 value)
{
    destination[0] = (u8)value;
    destination[1] = (u8)(value >> 8);
    destination[2] = (u8)(value >> 16);
    destination[3] = (u8)(value >> 24);
}

static u32 get_le32(const u8 *source)
{
    return (u32)source[0] | ((u32)source[1] << 8) |
           ((u32)source[2] << 16) | ((u32)source[3] << 24);
}

static void copy_text(u8 *destination, const char *source, u32 count)
{
    while (count-- != 0u)
        *destination++ = (u8)*source++;
}

static int equal_text(const u8 *source, const char *text, u32 count)
{
    while (count-- != 0u) {
        if (*source++ != (u8)*text++)
            return 0;
    }
    return 1;
}

static u32 elapsed(u32 now, u32 since)
{
    return (u32)(now - since);
}


u8 *fr_dirbuf(void) { return DIR_BUFFER; }
u8 *fr_fatbuf(void) { return FAT_BUFFER; }

static void *find_stick(void);


static u32 stick_usable(void)
{
    if (work.fs == 0 || find_stick() != work.fs) {
        work.fs = 0;
        return 0u;
    }
    return 1u;
}


static void bookkeeping_failed(void)
{
    if (session.phase != PHASE_STREAM)
        work.fs = 0;
}

int fr_read(u32 sector, u8 *buffer, u32 count)
{
    session.sector_reads++;
    if (!stick_usable() || STOCK_BLOCK_READ(work.fs, sector, buffer, (u16)count) != 0) {
        session.direct_read_errors++;
        bookkeeping_failed();
        return -1;
    }
    return 0;
}

int fr_write(u32 sector, const u8 *buffer, u32 count)
{
    session.sector_writes++;
    if (!stick_usable() || STOCK_BLOCK_WRITE(work.fs, sector, buffer, (u16)count) != 0) {
        session.direct_write_errors++;
        bookkeeping_failed();
        return -1;
    }
    return 0;
}


static void wav_frame(u8 *h, u32 data_bytes)
{
    u32 index;

    for (index = 0u; index < SECTOR_BYTES; index++)
        h[index] = 0u;
    copy_text(&h[0], "RIFF", 4u);
    put_le32(&h[4], SECTOR_BYTES - 8u + data_bytes);
    copy_text(&h[8], "WAVEfmt ", 8u);
    put_le32(&h[16], 16u);
    put_le16(&h[20], 1u);
    put_le16(&h[22], 2u);
    put_le32(&h[24], 48000u);
    put_le32(&h[28], BYTE_RATE);
    put_le16(&h[32], FRAME_BYTES);
    put_le16(&h[34], 8u * SAMPLE_BYTES);
    copy_text(&h[36], "JUNK", 4u);
    put_le32(&h[40], JUNK_PAYLOAD_BYTES);
    copy_text(&h[44], "DJMREC44", 8u);
    put_le32(&h[52], REPORT_SCHEMA);
    copy_text(&h[504], "data", 4u);
    put_le32(&h[508], data_bytes);
}


const u8 *rf_idle_header(const struct rf *r)
{
    (void)r;
    wav_frame(IDLE_BUFFER, 0u);
    return IDLE_BUFFER;
}

u32 rf_header_kind(const u8 *s, u32 *data_bytes)
{
    if (!equal_text(s, "RIFF", 4u) || !equal_text(&s[8], "WAVE", 4u) ||
        !equal_text(&s[44], "DJMREC44", 8u))
        return RF_HEADER_NONE;
    *data_bytes = get_le32(&s[508]);
    switch (get_le32(&s[60])) {
    case TAKE_NONE: return RF_HEADER_IDLE;
    case TAKE_RECORDING: return RF_HEADER_RUNNING;
    default: return RF_HEADER_COMPLETE;
    }
}

void rf_close_header(u8 *s)
{
    put_le32(&s[60], TAKE_COMPLETE);
}

u32 rf_header_file_bytes(const u8 *s)
{
    return get_le32(&s[4]) + 8u;
}

static u32 map_checksum(void)
{
    const struct fr_file *f = &files.cur;
    u32 sum = 0x9E3779B9u;
    u32 index;
    for (index = 0u; index < f->extent_count; index++) {
        sum = (sum << 5 | sum >> 27) ^ f->extents[index].first_sector;
        sum = (sum << 7 | sum >> 25) + f->extents[index].clusters;
    }
    return sum ^ f->size;
}

static u32 take_flags(void)
{
    u32 flags = take.flags;
    if (capture.dropped_blocks != take.dropped_before)
        flags |= STATUS_OVERRUN;
    if (capture.end_reason == END_INVALID)
        flags |= STATUS_INVALID_BLOCK;
    return flags;
}

static u8 *dump_words(u8 *destination, const volatile u32 *source, u32 bytes)
{
    u32 count = bytes / 4u;
    while (count-- != 0u) {
        put_le32(destination, *source++);
        destination += 4;
    }
    return destination;
}


static void guard_check(void)
{
    const volatile u32 *word;

    for (word = GUARD_START; word < GUARD_END; word++) {
        if (*word != 0xFFFFFFFFu) {
            if (!(session.status_flags & (STATUS_GUARD_CHANGED | STATUS_MEMORY_DIRTY))) {
                session.first_bad_address = (u32)word;
                session.first_bad_value = *word;
            }
            session.status_flags |= STATUS_GUARD_CHANGED;
            return;
        }
    }
}

static void build_report(u32 wav)
{
    u8 *h = HEADER_BUFFER;
    u8 *p;
    u32 bytes = sample_bytes();
    u32 data_bytes = take.actual_words * bytes;
    u32 index;

    guard_check();
    for (index = 0u; index < SECTOR_BYTES; index++)
        h[index] = 0u;
    if (wav) {
        copy_text(&h[0], "RIFF", 4u);
        put_le32(&h[4], SECTOR_BYTES - 8u + data_bytes +
                        (take.state == TAKE_COMPLETE ? work.cue_bytes : 0u));
        copy_text(&h[8], "WAVEfmt ", 8u);
        put_le32(&h[16], 16u);
        put_le16(&h[20], 1u);
        put_le16(&h[22], 2u);
        put_le32(&h[24], 48000u);
        put_le32(&h[28], 96000u * bytes);
        put_le16(&h[32], (u16)(2u * bytes));
        put_le16(&h[34], (u16)(8u * bytes));
        copy_text(&h[36], "JUNK", 4u);
        put_le32(&h[40], JUNK_PAYLOAD_BYTES);
        copy_text(&h[504], "data", 4u);
        put_le32(&h[508], data_bytes);
    } else {
        copy_text(&h[0], "DJMLOG01", 8u);
    }
    copy_text(&h[44], "DJMREC44", 8u);
    put_le32(&h[52], REPORT_SCHEMA);
    put_le32(&h[56], take_flags());
    put_le32(&h[60], take.state);
    p = dump_words(&h[REPORT_OFFSET], (const volatile u32 *)&session, sizeof(session));
    p = dump_words(p, (const volatile u32 *)&take, sizeof(take));
    dump_words(p, (const volatile u32 *)&capture, sizeof(capture));
}

static void note_release(int result)
{
    if (result != 0) {
        if (session.first_release_error == 0u)
            session.first_release_error = (u32)result;
        session.status_flags |= STATUS_RELEASE_FAILED;
    }
}


static void release_pool_pages(void)
{
    u32 index;

    for (index = 0u; index < POOL_BLOCKS; index++) {
        if (work.pool_blocks[index] != 0) {
            note_release(pool_release(work.pool_blocks[index], POOL_BLOCK_BYTES));
            work.pool_blocks[index] = 0;
        }
    }
}

static void release_map(void)
{
    if (work.map_block != 0) {
        note_release(pool_release(work.map_block, MAP_BLOCK_BYTES));
        work.map_block = 0;
    }
    files.phase = RF_IDLE;
    files.next = RF_NEXT_OFF;
}


static void write_log(void)
{
    if (files.dir.log_first == 0u || !stick_usable())
        return;
    build_report(0u);
    if (STOCK_BLOCK_WRITE(work.fs, fr_cluster_sector(&files.vol, files.dir.log_first),
                          HEADER_BUFFER, 1u) != 0) {
        work.fs = 0;
        return;
    }
    session.log_writes++;


    if (files.dir.log_sectors >= 1u + LISTEN_SECTORS && files.vol.spc >= 1u + LISTEN_SECTORS)
        (void)STOCK_BLOCK_WRITE(work.fs, fr_cluster_sector(&files.vol, files.dir.log_first) + 1u,
                                (u8 *)&listen_log, LISTEN_SECTORS);

    if (files.dir.log_sectors >= 1u + LISTEN_SECTORS + TUNNEL_SECTORS &&
        files.vol.spc >= 1u + LISTEN_SECTORS + TUNNEL_SECTORS)
        (void)STOCK_BLOCK_WRITE(work.fs, fr_cluster_sector(&files.vol, files.dir.log_first) + 1u +
                                LISTEN_SECTORS, (u8 *)&tunnel_log, TUNNEL_SECTORS);
}


static void log_once(u32 key)
{
    key ^= session.status_flags ^ (session.files_cut << 8);
    if (key != work.log_key) {
        work.log_key = key;
        write_log();
    }
}


static void *find_stick(void)
{
    void *fs = 0;

    if (STOCK_VOLUME_LIST == 0 || STOCK_FIND_VOLUME('C', &fs) != 0 || fs == 0 ||
        STOCK_MEDIA_CHECK(fs) != 0)
        return 0;
    return fs;
}

static void begin_prepare(u32 now, u32 delay_us)
{
    work.wait_from_us = now;
    work.wait_us = delay_us;
    files.phase = RF_IDLE;
    reclog_reset();
    session.phase = PHASE_PREPARE;
}

static u32 fs_flag(int error)
{
    switch (error) {
    case FR_E_IO: return STATUS_FS_IO;
    case FR_E_NOT_FAT32: return STATUS_NOT_FAT32;
    case FR_E_DIR: return STATUS_FS_DIR;
    case FR_E_NO_SPACE: return STATUS_NO_SPACE;
    case FR_E_CHAIN: case FR_E_OPEN: return STATUS_FS_CHAIN;
    case FR_E_CHANGED: return STATUS_FS_CHANGED;
    default: return STATUS_FS_VERIFY;
    }
}

static void report_files(void)
{
    session.files_made = files.made;
    session.files_reused = files.reused;
    session.files_refilled = files.refilled;
    session.files_recovered = files.recovered;
    session.files_cut = files.cuts;
    session.next_number = files.next_number;
    session.next_state = files.next;
    session.files_switched = files.switched;
    session.fs_phase = files.phase;
    session.sectors_per_cluster = files.vol.spc;
    session.fat_start = files.vol.fat_start;
    session.fat_sectors = files.vol.fat_sectors;
    session.data_start = files.vol.data_start;
    session.end_cluster = files.vol.end_cluster;
}


static void files_failed(u32 now, int error)
{
    session.fs_error = error;
    session.fs_error_phase = files.error_phase;
    session.status_flags |= fs_flag(error);
    status_reason = error == FR_E_NOT_FAT32 ? REASON_NOT_FAT32
                  : error == FR_E_NO_SPACE ? REASON_FULL : REASON_ERROR;
    work.error_from_us = now;
    work.retry = 1u;


    if (capture.state == CAPTURE_DONE)
        capture.state = CAPTURE_IDLE;
    status_closing = 0u;
    report_files();


    if (error == FR_E_NO_SPACE && work.map_block != 0 && !work.fail_logged) {
        work.fail_logged = 1u;
        work.fail_error = error;
        session.phase = PHASE_LOG;
        return;
    }
    work.fail_logged = 0u;
    log_once(0x20000000u ^ (u32)error);
    release_map();
    begin_prepare(now, error == FR_E_IO ? RETRY_ABSENT_US : RETRY_UNUSABLE_US);
}


static u32 file_capacity_pages(u32 size)
{
    u32 pages = size > SECTOR_BYTES + CUE_RESERVE_BYTES
        ? (size - SECTOR_BYTES - CUE_RESERVE_BYTES) / PAGE_BYTES : 0u;

    return pages > file_audio_pages ? file_audio_pages : pages;
}


static u32 file_capacity_words(u32 size)
{
    return file_capacity_pages(size) * file_page_blocks() * 24u;
}


static u32 capacity_words(void)
{
    return work.capacity_pages * file_page_blocks() * 24u;
}


static void use_file(void)
{
    const struct fr_file *f = &files.cur;

    report_files();
    session.first_sector = f->extents[0].first_sector;
    session.file_bytes = f->size;
    session.extent_count = f->extent_count;
    session.map_sum = map_checksum();
    work.file_sectors = f->size / SECTOR_BYTES;
    work.capacity_pages = file_capacity_pages(f->size);
}


static void files_ready(u32 now)
{
    use_file();
    session.prepare_duration_us = now - work.prepare_started_us;
    status_reason = REASON_INIT;
    work.retry = 0u;
    if (files.repaired != 0u) {
        reclog_event(RECLOG_REPAIR, files.repaired, 0u, 0u);
        files.repaired = 0u;
    }
    if (files.entry_fixed != 0u) {
        reclog_event(RECLOG_FIXED, 0u, 0u, 0u);
        files.entry_fixed = 0u;
    }
    SIM_EVENT(EVENT_READY);
    session.phase = PHASE_LOG;
}


static void files_step(u32 now)
{
    int result = rf_step(&files);

    session.fs_phase = files.phase;
    if (!work.geometry_checked && files.phase > RF_MOUNT) {


        work.geometry_checked = 1u;
        if (FS_TYPE(work.fs) != FS_TYPE_FAT32 ||
            FS_SECTORS_PER_CLUSTER(work.fs) != files.vol.spc ||
            FS_RESERVED_SECTORS(work.fs) != files.vol.fat_start ||
            FS_FAT_COUNT(work.fs) != files.vol.fat_count ||
            FS_FAT_SECTORS(work.fs) != files.vol.fat_sectors) {
            session.status_flags |= STATUS_GEOMETRY;
            files_failed(now, FR_E_NOT_FAT32);
            return;
        }
    }
    if (result == FR_BUSY)
        return;
    if (result != FR_OK) {
        files_failed(now, result);
        return;
    }
    if (files.cur.size < SECTOR_BYTES + PAGE_BYTES) {
        files_failed(now, FR_E_NO_SPACE);
        return;
    }
    files_ready(now);
}


static void prepare(u32 now)
{
    void *fs;
    void *block;

    if (files.phase != RF_IDLE) {
        files_step(now);
        return;
    }


    if (work.poll_seen && elapsed(now, work.poll_from_us) < STICK_POLL_US)
        return;
    work.poll_seen = 1u;
    work.poll_from_us = now;
    fs = find_stick();
    if (fs == 0) {
        work.stick_present = 0u;
        work.retry = 0u;
        if (status_reason != REASON_ERROR || elapsed(now, work.error_from_us) >= ERROR_SHOW_US)
            status_reason = REASON_NO_USB;
        return;
    }
    if (!work.stick_present) {
        work.stick_present = 1u;
        work.retry = 0u;
        status_reason = REASON_INIT;
        work.wait_from_us = now;
        work.wait_us = STICK_SETTLE_US;
        return;
    }
    if (elapsed(now, work.wait_from_us) < work.wait_us)
        return;
    work.wait_from_us = now;
    work.wait_us = RETRY_ABSENT_US;
    session.prepare_attempts++;
    if (!work.retry)
        status_reason = REASON_INIT;
    work.fs = fs;
    if (work.map_block == 0) {
        block = pool_alloc(MAP_BLOCK_BYTES);
        if (block == 0 || !pool_address_valid(block, MAP_BLOCK_BYTES)) {
            if (block != 0)
                note_release(pool_release(block, MAP_BLOCK_BYTES));
            session.status_flags |= STATUS_ALLOC_FAILED;
            status_reason = REASON_OTHER;
            return;
        }
        work.map_block = block;
    }
    files.cur.extents = (struct extent *)work.map_block;
    files.cur.max_extents = MAX_EXTENTS;
    files.other.extents = (struct extent *)work.map_block + MAX_EXTENTS;
    files.other.max_extents = MAX_EXTENTS;
    work.geometry_checked = 0u;
    work.prepare_started_us = now;
    rf_begin_prepare(&files);
}


static u32 audit_read_word(u32 index)
{
    if (index < LOW_AUDIT_BYTES / 4u)
        return ((const volatile u32 *)LOW_BUFFER)[index];
    return ((const volatile u32 *)STAGE_BUFFER)[index - LOW_AUDIT_BYTES / 4u];
}


static void verify_memory_chunk(void)
{
    u32 count;
    u32 value;
    u32 low;

    for (count = 0u; count < VERIFY_CHUNK_WORDS &&
         work.verify_index < AUDIT_BYTES / 4u; count++) {
        low = work.verify_index < LOW_AUDIT_BYTES / 4u;
        value = audit_read_word(work.verify_index);
        if (value != (low ? 0xFFFFFFFFu : 0u)) {
            session.first_bad_address = low
                ? LOW_ADDRESS + work.verify_index * 4u
                : STAGE_ADDRESS + (work.verify_index - LOW_AUDIT_BYTES / 4u) * 4u;
            session.first_bad_value = value;
            session.status_flags |= STATUS_MEMORY_DIRTY;
            session.phase = PHASE_FAILED;
            return;
        }
        work.verify_index++;
    }
    if (work.verify_index == AUDIT_BYTES / 4u)
        session.phase = PHASE_SETTLE;
}


struct extra_region { volatile u8 *start; u32 pages; };
static const struct extra_region extra_regions[EXTRA_REGIONS] = EXTRA_REGIONS_INIT;


static volatile u8 *extra_page(u32 index, u32 *region)
{
    u32 r = 0u;

    while (index >= extra_regions[r].pages)
        index -= extra_regions[r++].pages;
    *region = r;
    return extra_regions[r].start + index * PAGE_BYTES;
}


static u32 extra_state(void) { return session.extra_state & EXTRA_STATE_MASK; }
static u32 extra_out(void) { return session.extra_state >> EXTRA_OUT_SHIFT; }

static void audit_begin(void)
{
    work.audit_page = 0u;
    work.audit_word = 0u;
    work.audit_sum = 0u;
    work.audit_mismatch = 0u;
}


static int audit_step(u32 verify)
{
    const volatile u32 *words;
    u32 count = EXTRA_STEP_WORDS;
    u32 sum = work.audit_sum, word = work.audit_word, region;

    while (work.audit_page < EXTRA_PAGES) {
        words = (const volatile u32 *)extra_page(work.audit_page, &region);
        if ((extra_out() >> region & 1u) == 0u) {
            while (word < PAGE_WORDS && count != 0u) {
                sum = (sum << 1 | sum >> 31) ^ words[word++];
                count--;
            }
            if (word < PAGE_WORDS) {
                work.audit_sum = sum;
                work.audit_word = word;
                return 0;
            }
            if (!verify)
                work.extra_sums[work.audit_page] = sum;
            else if (work.extra_sums[work.audit_page] != sum)
                work.audit_mismatch |= 1u << region;
        }
        sum = 0u;
        word = 0u;
        work.audit_page++;
    }
    work.audit_sum = 0u;
    work.audit_word = 0u;
    return 1;
}


static u32 audit_verdict(void)
{
    u32 out = extra_out() | work.audit_mismatch;
    u32 changed = work.audit_mismatch != 0u;

    if (changed)
        session.status_flags |= STATUS_EXTRA_CHANGED;
    else
        session.extra_passes++;
    session.extra_state = out << EXTRA_OUT_SHIFT |
                          (out == (1u << EXTRA_REGIONS) - 1u ? EXTRA_BAD : EXTRA_ACCEPTED);
    return changed;
}

static void enter_snapshot(u32 after)
{
    work.after_snapshot = after;
    if (extra_state() != EXTRA_BAD)
        session.extra_state = (session.extra_state & ~EXTRA_STATE_MASK) | EXTRA_PENDING;
    audit_begin();
    session.phase = PHASE_SNAPSHOT;
}

static void snapshot(void)
{
    if (extra_state() == EXTRA_BAD || audit_step(0u)) {
        audit_begin();
        session.phase = work.after_snapshot;
    }
}


static void settle(u32 now)
{
    if (!work.settle_seen) {
        work.settle_seen = 1u;
        work.settle_started_us = now;
        return;
    }
    if (elapsed(now, work.settle_started_us) >= SETTLE_US) {
        work.stick_present = 1u;
        begin_prepare(now, 0u);
        enter_snapshot(PHASE_PREPARE);
    }
}


static u32 file_run(u32 index, u32 *sector)
{
    const struct fr_file *f = &files.cur;
    u32 spc = files.vol.spc;
    u32 cluster = index / spc;
    u32 e;

    for (e = 0u; e < f->extent_count; e++) {
        if (cluster < f->extents[e].clusters) {
            *sector = f->extents[e].first_sector + cluster * spc + index % spc;
            return (f->extents[e].clusters - cluster) * spc - index % spc;
        }
        cluster -= f->extents[e].clusters;
    }
    return 0u;
}


static u32 file_write(u32 index, const volatile u8 *data, u32 count)
{
    u32 sector = 0u;

    if (work.map_block == 0 || files.cur.extent_count == 0u ||
        map_checksum() != session.map_sum)
        return STATUS_MAP_MISMATCH;
    if (count == 0u || count > MAX_BLOCK_SECTORS || index >= work.file_sectors ||
        count > work.file_sectors - index || file_run(index, &sector) < count)
        return STATUS_BOUNDARY;
    if (!stick_usable() || STOCK_BLOCK_WRITE(work.fs, sector, (const void *)data, (u16)count) != 0) {
        session.direct_write_errors++;
        work.fs = 0;
        return STATUS_DIRECT_WRITE;
    }
    return 0u;
}

static u32 write_header(void)
{
    u32 start = STOCK_TIME_US();
    u32 result;
    u32 duration;

    take.header_writes++;
    build_report(1u);
    result = file_write(0u, HEADER_BUFFER, 1u);
    duration = STOCK_TIME_US() - start;
    if (duration > take.header_write_max_us)
        take.header_write_max_us = duration;
    return result == STATUS_DIRECT_WRITE ? STATUS_HEADER_FAILED : result;
}


static void set_base_ring(void)
{
    volatile u8 *page;
    u32 index, region, pages = BASE_PAGES;

    for (index = 0u; index < LOW_PAGES; index++)
        page_buffers[index] = LOW_PAGE_BASE + index * PAGE_BYTES;
    for (index = 0u; index < STAGE_PAGES; index++)
        page_buffers[LOW_PAGES + index] = STAGE_BUFFER + index * PAGE_BYTES;
    if (extra_state() == EXTRA_ACCEPTED) {
        for (index = 0u; index < EXTRA_PAGES; index++) {
            page = extra_page(index, &region);
            if ((extra_out() >> region & 1u) == 0u)
                page_buffers[pages++] = page;
        }
    }
    session.ring_base_pages = pages;
}

static void arm(u32 now)
{
    volatile u32 *c = (volatile u32 *)&capture;
    u32 *t = (u32 *)&take;
    u32 index, saved;

    capture.state = CAPTURE_IDLE;
    for (index = 1u; index < sizeof(capture) / 4u; index++)
        c[index] = 0u;
    for (index = 0u; index < sizeof(take) / 4u; index++)
        t[index] = 0u;
    take.data_write_min_us = MIN_SENTINEL;
    pages_consumed = 0u;
    blocks_consumed = 0u;
    mark_count = 0u;
    lost_count = 0u;
    lost_open = 0u;
    lost_more = 0u;
    lost_more_blocks = 0u;
    coarse_now = 0u;
    coarse_count = 0u;
    coarse_open = 0u;
    coarse_more = 0u;
    coarse_more_blocks = 0u;
    coarse_more_last = 0u;
    coarse_more_run = 0u;
    dither_state = 0u;
    sheet_dirty = 0u;
    control_cut = 0u;
    work.cutting = 0u;
    work.cut_wanted = 0u;
    work.cue_bytes = 0u;
    work.last_length = 0u;
    work.service_seen = 0u;
    work.gone_seen = 0u;
    work.bursting = 0u;
    work.out_blocks = 0u;
    work.out_coarse = 0u;
    work.read_left = 0u;
    set_base_ring();
    capture.ring_pages = session.ring_base_pages;
#if defined(SIM) && defined(SIM_START_BLOCKS)


    take.blocks_before = SIM_START_BLOCKS;
    capture.captured_blocks = SIM_START_BLOCKS;
    blocks_consumed = SIM_START_BLOCKS;
    capture.produced_words = SIM_START_BLOCKS * 24u;
#endif
    session.armed_us = now;
    if (files.next == RF_NEXT_NONE)
        files.next = RF_NEXT_OFF;
    audit_begin();
    session.phase = PHASE_ARMED;
    SIM_EVENT(EVENT_ARM);
    COMPILER_BARRIER();


    saved = irq_lock();
    capture.target_words = capture.produced_words + capacity_words();
    capture.state = CAPTURE_ARMED;
    status_closing = 0u;
    irq_restore(saved);
}


static void audit(u32 now)
{
    if (extra_state() != EXTRA_BAD) {
        if (!audit_step(1u))
            return;
        (void)audit_verdict();
    }
    arm(now);
}


static void armed_audit(void)
{
    u32 saved;

    if (extra_state() != EXTRA_ACCEPTED || !audit_step(1u))
        return;
    if (audit_verdict()) {
        saved = irq_lock();
        if (capture.state == CAPTURE_ARMED) {
            set_base_ring();
            capture.ring_pages = session.ring_base_pages;
        }
        irq_restore(saved);
    }
    audit_begin();
}


static void add_pool_pages(void)
{
    u32 index;
    u32 page;
    u32 saved;
    void *block;

    for (index = 0u; index < POOL_BLOCKS; index++) {
        block = pool_alloc(POOL_BLOCK_BYTES);
        if (block == 0 || !pool_address_valid(block, POOL_BLOCK_BYTES)) {
            if (block != 0)
                note_release(pool_release(block, POOL_BLOCK_BYTES));
            release_pool_pages();
            take.flags |= STATUS_ALLOC_FAILED | STATUS_SMALL_RING;
            return;
        }
        work.pool_blocks[index] = block;
    }
    for (index = 0u; index < POOL_PAGES; index++) {
        page = session.ring_base_pages + index;
        page_buffers[page] = (volatile u8 *)work.pool_blocks[index / POOL_BLOCK_PAGES] +
                             (index % POOL_BLOCK_PAGES) * PAGE_BYTES;
    }
    COMPILER_BARRIER();
    saved = irq_lock();
    if (capture.pages_produced < capture.ring_pages) {
        capture.ring_pages += POOL_PAGES;
        take.pool_pages = POOL_PAGES;
    }
    irq_restore(saved);
    if (take.pool_pages == 0u) {
        release_pool_pages();
        take.flags |= STATUS_SMALL_RING;
    }
}


static void name_take(void);

#define EVENT_REFUSED 0x80u

static void take_failed(u32 now, u32 flag, u32 event)
{
    u32 refused;

    capture.state = CAPTURE_IDLE;
    status_closing = 0u;
    status_stopped = 1u;


    if (event == EVENT_FAIL)
        reclog_event(RECLOG_ERROR, files.named && work.header_has_audio ? take.number : 0u, 0u, 0u);


    status_reason = event == EVENT_FAIL || event == EVENT_REFUSED ? REASON_ERROR : REASON_INIT;
    work.error_from_us = now;
    work.retry = event == EVENT_FAIL || event == EVENT_REFUSED;
    refused = event == EVENT_REFUSED;
    if (refused)
        event = EVENT_ABORT;
    take.flags |= flag;
    session.status_flags |= take_flags();
    release_pool_pages();
    release_map();
    (void)event;
    SIM_EVENT(event);
    if (event == EVENT_FAIL) {
        session.takes_failed++;
        work.failed_in_row++;
    }
    if ((flag & (STATUS_MAP_MISMATCH | STATUS_BOUNDARY)) != 0u ||
        work.failed_in_row >= MAX_FAILED_TAKES) {

        status_reason = work.failed_in_row >= MAX_FAILED_TAKES ? REASON_ERROR : REASON_OTHER;
        session.phase = PHASE_FAILED;
        return;
    }
    begin_prepare(now, refused ? ERROR_SHOW_US : RETRY_ABSENT_US);
    enter_snapshot(PHASE_PREPARE);
}


static void start_take(u32 now)
{
    void *fs;
    u32 result;

    session.takes_started++;
    cut_hint = CUT_WAIT;
    status_stopped = 0u;
    add_pool_pages();
    if (work.map_block == 0 || map_checksum() != session.map_sum) {
        take_failed(now, STATUS_MAP_MISMATCH, EVENT_FAIL);
        return;
    }
    fs = find_stick();
    if (fs == 0) {
        take_failed(now, STATUS_REVALIDATION, EVENT_ABORT);
        return;
    }
    work.fs = fs;
    if (rf_revalidate(&files) != FR_OK) {


        reclog_event(RECLOG_REFUSED, 0u, files.refused, 0u);
        take_failed(now, STATUS_REVALIDATION, EVENT_REFUSED);
        return;
    }
    take.state = TAKE_RECORDING;
    take.sheet_stamp = (now ^ session.first_sector * 0x9E3779B1u) | 1u;
    work.header_has_audio = 0u;
    result = write_header();
    if (result != 0u) {
        take_failed(now, result, EVENT_FAIL);
        return;
    }
    SIM_EVENT(EVENT_START);


    if ((capture.by_hand & 1u) != 0u)
        name_take();
    take.file_offset_bytes = SECTOR_BYTES;
    take.start_duration_us = STOCK_TIME_US() - now;
    work.last_header_us = now;
    work.next_tried = 0u;
    session.phase = PHASE_STREAM;
}

static void update_pending(void)
{
    u32 pending = (capture.pages_produced - pages_consumed) * PAGE_BYTES;
    pending += capture.current_page_bytes;
    if (pending > capture.maximum_pending_bytes)
        capture.maximum_pending_bytes = pending;
    pending = capture.captured_blocks - blocks_consumed;
    if (pending > capture.maximum_pending_blocks)
        capture.maximum_pending_blocks = pending;
}

static void publish_current_page(void)
{
    u32 page;
    if (capture.current_page_bytes == 0u)
        return;
    page = capture.pages_produced % capture.ring_pages;
    page_lengths[page] = capture.current_page_bytes;
    page_blocks[page] = capture.current_page_blocks;
    page_coarse[page] = (u8)coarse_now;
    COMPILER_BARRIER();
    capture.pages_produced++;
    capture.current_page_bytes = 0u;
    capture.current_page_blocks = 0u;
}

static void stop_capture(u32 reason)
{
    capture.end_reason = reason;
    publish_current_page();
    update_pending();
    COMPILER_BARRIER();
    capture.state = CAPTURE_DONE;
}


static u32 continue_take(u32 now, u32 chain_index, u32 blocks_before, u32 pool_pages)
{
    u32 *t = (u32 *)&take;
    u32 result;
    u32 index;

    use_file();
    for (index = 0u; index < sizeof(take) / 4u; index++)
        t[index] = 0u;
    take.state = TAKE_RECORDING;
    take.data_write_min_us = MIN_SENTINEL;
    take.pool_pages = pool_pages;
    take.chain_index = chain_index;
    take.blocks_before = blocks_before;
    take.dropped_before = capture.dropped_blocks;
    take.gaps_before = capture.gap_count;
    take.sheet_stamp = (now ^ session.first_sector * 0x9E3779B1u ^ chain_index) | 1u;
    work.header_has_audio = 0u;
    name_take();
    result = write_header();
    if (result != 0u)
        return result;
    take.file_offset_bytes = SECTOR_BYTES;
    work.last_header_us = now;
    work.next_tried = 0u;
    return 0u;
}


static u32 switch_file(u32 now)
{
    u32 pool_pages = take.pool_pages;
    u32 chain_index = take.chain_index + 1u;
    u32 blocks_before = take.blocks_before + take.actual_words / 24u;
    u32 result;

    if (files.next != RF_NEXT_READY)
        return STATUS_BOUNDARY;
    result = write_cue(0u);
    if (result != 0u)
        return result;
    take.state = TAKE_COMPLETE;
    take.continued = 1u;
    session.takes_kept++;
    session.status_flags |= take_flags();
    result = write_header();
    if (result != 0u) {
        session.takes_kept--;
        return result;
    }
    SIM_EVENT(EVENT_SWITCH);
    work.cue_bytes = 0u;
    work.failed_in_row = 0u;
    if (rf_switch(&files) != FR_OK)
        return STATUS_FS_CHANGED;
    return continue_take(now, chain_index, blocks_before, pool_pages);
}


static u32 next_found_no_room(void)
{
    return files.next == RF_NEXT_NONE && files.last_error == FR_E_NO_SPACE &&
           files.error_phase == 0x100u + RF_NEXT_SCAN && !files.tmp_pending;
}


static u32 cut_allowed(void)
{
    u32 cluster = files.vol.spc * SECTOR_BYTES;
    u32 keep = SECTOR_BYTES + take.actual_words * sample_bytes() + CUE_RESERVE_BYTES;
    u32 least = files.min_bytes > 4u * PAGE_BYTES ? files.min_bytes : 4u * PAGE_BYTES;

    if (take.actual_words == 0u)
        return 0u;
    if (files.next == RF_NEXT_READY)
        return 1u;
    if (files.next != RF_NEXT_OFF) {
        if (!next_found_no_room())
            return 0u;
        if (least < CUT_LEAST_BYTES)
            least = CUT_LEAST_BYTES;
    }
    return files.cur.size > keep && files.cur.size - keep >= least + 2u * cluster + CUE_RESERVE_BYTES;
}


static u32 cut_begin(u32 now)
{
    u32 result = write_cue(0u);

    if (result != 0u)
        return result;
    SIM_EVENT(EVENT_CUT);
    take.state = TAKE_COMPLETE;
    take.continued = 1u;
    session.takes_kept++;
    session.status_flags |= take_flags();
    if (!files.named)
        take.number = files.next_number;
    result = write_header();
    if (result != 0u) {
        session.takes_kept--;
        return result;
    }
    SIM_EVENT(EVENT_SPLIT);
    files.no_refill = 1u;
    work.cut_to_next = files.next == RF_NEXT_READY;
    rf_begin_finish(&files, SECTOR_BYTES + take.actual_words * sample_bytes() + work.cue_bytes);
    work.cue_bytes = 0u;
    work.cutting = 1u;
    work.prepare_started_us = now;
    return 0u;
}


static void cut_step(u32 now)
{
    u32 chain_index = take.chain_index + 1u;
    u32 blocks_before = take.blocks_before + take.actual_words / 24u;
    u32 saved, result;
    int step = rf_step(&files);

    session.fs_phase = files.phase;
    if (step == FR_BUSY)
        return;
    files.no_refill = 0u;
    work.cutting = 0u;


    if (step == FR_OK && work.cut_to_next)
        step = rf_revalidate(&files);
    if (step != FR_OK || files.cur.size < SECTOR_BYTES + CUE_RESERVE_BYTES + 2u * PAGE_BYTES) {
        session.fs_error = step;
        session.fs_error_phase = files.error_phase;
        take_failed(now, STATUS_FS_CHANGED, EVENT_FAIL);
        return;
    }
    result = continue_take(now, chain_index, blocks_before, take.pool_pages);
    if (result != 0u) {
        take_failed(now, result, EVENT_FAIL);
        return;
    }
    saved = irq_lock();


    capture.target_words = blocks_before * 24u + capacity_words();
    irq_restore(saved);
}


static u32 fill_out_page(void)
{
    u32 page, n;
    u32 blocks = file_page_blocks();
    volatile u8 *sample;

    while (work.out_blocks < blocks) {
        COMPILER_BARRIER();
        if (capture.pages_produced == pages_consumed)
            break;
        page = pages_consumed % capture.ring_pages;
        if (work.read_left == 0u) {
            work.read_left = page_blocks[page];
            work.read_coarse = page_coarse[page];
            work.read_offset = 0u;
            work.unpack.x1[0] = work.unpack.x1[1] = 0;
            work.unpack.x2[0] = work.unpack.x2[1] = 0;
            if (work.read_left == 0u || page_lengths[page] > PAGE_BYTES)
                return STATUS_BOUNDARY;
        }
        if (!deep) {


            work.read_offset += unpack_block((const u8 *)page_buffers[page] + work.read_offset,
                                             unpacked, &work.unpack);
            sample = OUT_PAGE + work.out_blocks * 48u;
            for (n = 0u; n < 24u; n++) {
                sample[2u * n] = unpacked[3u * n];
                sample[2u * n + 1u] = unpacked[3u * n + 1u];
            }
        } else {
            work.read_offset += unpack_block((const u8 *)page_buffers[page] + work.read_offset,
                                             (u8 *)OUT_PAGE + work.out_blocks * BLOCK_BYTES,
                                             &work.unpack);
            if (work.read_coarse) {


                sample = OUT_PAGE + work.out_blocks * BLOCK_BYTES;
                for (n = 0u; n < 24u; n++, sample += SAMPLE_BYTES) {
                    sample[2] = sample[1];
                    sample[1] = sample[0];
                    sample[0] = 0u;
                }
                work.out_coarse = 1u;
            }
        }
        work.out_blocks++;
        if (--work.read_left == 0u) {
            if (work.read_offset != page_lengths[page])
                return STATUS_BOUNDARY;
            blocks_consumed += page_blocks[page];
            COMPILER_BARRIER();
            pages_consumed++;
        }
    }
    return 0u;
}


static u32 write_out_page(u32 now)
{
    volatile u8 *out = OUT_PAGE;
    u32 length;
    u32 padded;
    u32 index;
    u32 offset;
    u32 remaining;
    u32 chunk;
    u32 run;
    u32 sector;
    u32 result;
    u32 start;
    u32 duration;

    if (take.file_offset_bytes == SECTOR_BYTES + work.capacity_pages * PAGE_BYTES) {
        result = switch_file(now);
        if (result != 0u)
            return result;
    }
    length = work.out_blocks * 24u * sample_bytes();
    padded = (length + 511u) & ~511u;
    if (length == 0u || length > PAGE_BYTES ||
        (take.file_offset_bytes & 511u) != 0u)
        return STATUS_BOUNDARY;
    for (index = length; index < padded; index++)
        out[index] = 0u;
    index = take.file_offset_bytes / SECTOR_BYTES;
    offset = 0u;
    remaining = padded / SECTOR_BYTES;
    start = STOCK_TIME_US();
    while (remaining != 0u) {
        run = file_run(index, &sector);
        chunk = remaining < MAX_BLOCK_SECTORS ? remaining : MAX_BLOCK_SECTORS;
        if (chunk > run)
            chunk = run;
        result = file_write(index, out + offset, chunk);
        if (result != 0u)
            return result;
        take.direct_block_writes++;
        index += chunk;
        offset += chunk * SECTOR_BYTES;
        remaining -= chunk;
    }
    duration = STOCK_TIME_US() - start;
    take.total_data_write_us += duration;
    take.write_count++;
    if (duration < take.data_write_min_us)
        take.data_write_min_us = duration;
    if (duration > take.data_write_max_us) {
        take.data_write_max_us = duration;
        take.max_write_offset_bytes = take.file_offset_bytes;
    }
    if (duration > SLOW_LOG_US && take.slow_count < SLOW_LOG_ENTRIES) {
        index = duration / 1000u;
        take.slow_log[take.slow_count++] =
            (take.write_count - 1u) << 16 | (index > 0xFFFFu ? 0xFFFFu : index);
    }
    if (duration > 250000u) {
        take.slow_writes[0]++;
        if (duration > 500000u) {
            take.slow_writes[1]++;
            if (duration > 1000000u)
                take.slow_writes[2]++;
        }
    }
    if (work.out_coarse) {
        take.flags |= STATUS_COARSE;
        work.out_coarse = 0u;
    }
    take.actual_words += length / sample_bytes();
    take.file_offset_bytes += length;
    work.last_length = length;
    work.out_blocks = 0u;
    return 0u;
}


static u32 build_tail(u8 *out, const u32 *const *lists, u32 *used, u32 frames)
{
    u32 ids[NAMED_MARKS];
    u8 kinds[NAMED_MARKS];
    u32 at[3];
    u32 count = 0u, named = 0u, index, k, best, position;
    u8 *p = out + 12u;

    at[0] = at[1] = at[2] = 0u;
    for (;;) {
        position = 0xFFFFFFFFu;
        best = 0u;
        for (k = 0u; k < 3u; k++) {
            if (at[k] < used[k] && lists[k][at[k]] < position) {
                position = lists[k][at[k]];
                best = k;
            }
        }
        if (position >= frames)
            break;
        count++;
        put_le32(p, count);
        put_le32(p + 4, position);
        copy_text(p + 8, "data", 4u);
        put_le32(p + 12, 0u);
        put_le32(p + 16, 0u);
        put_le32(p + 20, position);
        p += CUE_POINT_BYTES;
        if (best != 0u) {
            ids[named] = count;
            kinds[named] = (u8)best;
            named++;
        }
        at[best]++;
    }
    used[0] = at[0];
    used[1] = at[1];
    used[2] = at[2];
    if (count == 0u)
        return 0u;
    copy_text(out, "cue ", 4u);
    put_le32(out + 4, 4u + CUE_POINT_BYTES * count);
    put_le32(out + 8, count);
    if (named != 0u) {
        copy_text(p, "LIST", 4u);
        put_le32(p + 4, 4u + CUE_LABEL_BYTES * named);
        copy_text(p + 8, "adtl", 4u);
        p += 12;
        for (index = 0u; index < named; index++, p += CUE_LABEL_BYTES) {
            copy_text(p, "labl", 4u);
            put_le32(p + 8, ids[index]);
            if (kinds[index] == 1u) {
                put_le32(p + 4, 9u);
                copy_text(p + 12, "LOST", 4u);
                p[16] = 0u;
            } else {
                put_le32(p + 4, 10u);
                copy_text(p + 12, "16BIT", 5u);
            }
            p[17] = 0u;
        }
    }
    return (u32)(p - out);
}


static u32 frame_in_file(u32 frame, u32 first)
{
    u32 distance = frame - first;

    return (s32)distance < 0 ? 0u : distance;
}


static u32 write_cue(u32 last)
{
    u8 *scratch = fr_fatbuf();
    u32 *user = (u32 *)(void *)(scratch + 4096u);
    u32 *places = user + MAX_MARKS;
    u32 *stretches = places + LOST_MARKS;
    const u32 *lists[3];
    u32 used[3];
    u32 first_frame = take.blocks_before * 12u;
    u32 frames = take.actual_words / 2u;
    u32 audio_end = SECTOR_BYTES + take.actual_words * sample_bytes();
    u32 tail = audio_end % SECTOR_BYTES;
    u32 number = files.named ? take.number : files.next_number;
    u32 saved, index, listed_lost, listed_coarse, bytes, sectors, done, run, chunk, sector, result;
    u32 carry = 0u, carry_blocks = 0u, inside, next_more = 0u, next_blocks = 0u;

    work.cue_bytes = 0u;
    capture.cue_marks = 0u;
    capture.cue_lost = 0u;
    if (mark_count == 0u && lost_count == 0u && lost_more == 0u && coarse_count == 0u && coarse_more == 0u &&
        coarse_more_blocks == 0u)
        return 0u;
    files.vol.cached_fat_sector = 0u;
    lists[0] = user;
    lists[1] = places;
    lists[2] = stretches;
    saved = irq_lock();
    used[0] = mark_count;
    used[1] = lost_count;
    used[2] = coarse_count;
    for (index = 0u; index < used[0]; index++)
        user[index] = frame_in_file(marks[index], first_frame);
    for (index = 0u; index < used[1]; index++)
        places[index] = frame_in_file(lost[index].frame, first_frame);
    for (index = 0u; index < used[2]; index++)
        stretches[index] = frame_in_file(coarse[index].frame, first_frame);
    irq_restore(saved);
    bytes = build_tail(scratch + tail, lists, used, frames);
    listed_lost = used[1];
    listed_coarse = used[2];
    saved = irq_lock();
    if (!last && coarse_more_last) {


        inside = first_frame + frames;
        if ((s32)(coarse_more_frame - inside) >= 0) {
            next_blocks = coarse_more_run;
            coarse_more--;
        } else {
            inside = (inside - coarse_more_frame) / 12u;
            next_blocks = coarse_more_run > inside ? coarse_more_run - inside : 0u;
        }
        coarse_more_blocks -= next_blocks;
        next_more = next_blocks != 0u || (coarse_now && !coarse_open) ? 1u : 0u;
    }
    if (!last && used[2] != 0u) {


        index = used[2] - 1u;
        inside = (first_frame + frames - coarse[index].frame) / 12u;
        if (coarse[index].blocks > inside || (coarse_now && coarse_open && used[2] == coarse_count)) {
            carry = 1u;
            if (coarse[index].blocks > inside) {
                carry_blocks = coarse[index].blocks - inside;
                coarse[index].blocks = inside;
            }
        }
    }
    for (index = 0u; index < used[1]; index++)
        reclog_event(RECLOG_LOST, number, places[index], lost[index].blocks);
    for (index = 0u; index < used[2]; index++)
        reclog_event(RECLOG_COARSE, number, stretches[index], coarse[index].blocks);
    if (last) {


        for (index = used[1]; index < lost_count; index++)
            reclog_event(RECLOG_LOST, number, frames, lost[index].blocks);
        used[1] = lost_count;
        for (index = used[2]; index < coarse_count; index++)
            reclog_event(RECLOG_COARSE, number, frames, coarse[index].blocks);
        used[2] = coarse_count;
    }
    for (index = used[0]; index < mark_count; index++)
        marks[index - used[0]] = marks[index];
    mark_count -= used[0];
    for (index = used[1]; index < lost_count; index++) {
        lost[index - used[1]].frame = lost[index].frame;
        lost[index - used[1]].blocks = lost[index].blocks;
    }
    lost_count -= used[1];
    for (index = used[2]; index < coarse_count; index++) {
        coarse[index - used[2]].frame = coarse[index].frame;
        coarse[index - used[2]].blocks = coarse[index].blocks;
    }
    coarse_count -= used[2];
    if (carry) {


        if (coarse_count < COARSE_MARKS) {
            for (index = coarse_count; index != 0u; index--) {
                coarse[index].frame = coarse[index - 1u].frame;
                coarse[index].blocks = coarse[index - 1u].blocks;
            }
            coarse[0].frame = first_frame + frames;
            coarse[0].blocks = carry_blocks;
            coarse_count++;
            sheet_dirty = 1u;
        } else {
            next_more++;
            next_blocks += carry_blocks;
            coarse_open = 0u;
        }
    } else if (coarse_count == 0u) {
        coarse_open = 0u;
    }
    if (lost_more != 0u) {
        reclog_event(RECLOG_MORE, number, lost_more, lost_more_blocks);
        lost_more = 0u;
        lost_more_blocks = 0u;
    }
    if (coarse_more != 0u || coarse_more_blocks != 0u)
        reclog_event(RECLOG_COARSE_MORE, number, coarse_more != 0u ? coarse_more : 1u, coarse_more_blocks);
    coarse_more = next_more;
    coarse_more_blocks = next_blocks;
    irq_restore(saved);
    if (bytes == 0u)
        return 0u;
    if (audio_end + bytes > files.cur.size || (tail != 0u && tail > work.last_length))
        return 0u;
    for (index = 0u; index < tail; index++)
        scratch[index] = OUT_PAGE[work.last_length - tail + index];
    for (index = tail + bytes; (index & (SECTOR_BYTES - 1u)) != 0u; index++)
        scratch[index] = 0u;
    sectors = index / SECTOR_BYTES;
    index = audio_end / SECTOR_BYTES;
    for (done = 0u; done < sectors; done += chunk) {
        run = file_run(index + done, &sector);
        chunk = sectors - done < run ? sectors - done : run;
        result = file_write(index + done, scratch + done * SECTOR_BYTES, chunk);
        if (result != 0u)
            return result;
    }
    work.cue_bytes = bytes;
    capture.cue_marks = used[0];
    capture.cue_lost = listed_lost | listed_coarse << 16;
    return 0u;
}

static u32 sheet_sum(const u8 *sheet)
{
    u32 sum = 0u, index;

    for (index = 0u; index < SHEET_SUM_AT; index++)
        sum = (sum << 1 | sum >> 31) + sheet[index];
    return sum;
}


static u32 save_sheet(void)
{
    u8 *sheet = fr_fatbuf();
    u32 first_frame = take.blocks_before * 12u;
    u32 saved, index, count;
    u8 *p = sheet + SHEET_POSITIONS_AT;

    files.vol.cached_fat_sector = 0u;
    for (index = 0u; index < SECTOR_BYTES; index++)
        sheet[index] = 0u;
    copy_text(sheet, "DJMK", 4u);
    put_le32(sheet + 4, take.sheet_stamp);
    saved = irq_lock();
    sheet_dirty = 0u;
    count = mark_count;
    put_le16(sheet + SHEET_COUNT_AT, (u16)count);
    for (index = 0u; index < count; index++, p += 4)
        put_le32(p, frame_in_file(marks[index], first_frame));
    count = lost_count;
    sheet[SHEET_COUNT_AT + 2u] = (u8)count;
    for (index = 0u; index < count; index++, p += 4)
        put_le32(p, frame_in_file(lost[index].frame, first_frame));
    count = coarse_count;
    sheet[SHEET_COUNT_AT + 3u] = (u8)count;
    for (index = 0u; index < count; index++, p += 4)
        put_le32(p, frame_in_file(coarse[index].frame, first_frame));
    irq_restore(saved);
    put_le32(sheet + SHEET_SUM_AT, sheet_sum(sheet));
    return file_write(work.file_sectors - 1u, sheet, 1u);
}


u32 rf_recover_tail(struct rf *r, u8 *header)
{
    u8 *scratch = fr_fatbuf();
    u32 *user = (u32 *)(void *)(scratch + 4096u);
    u32 *places = user + MAX_MARKS;
    u32 *stretches = places + LOST_MARKS;
    const u32 *lists[3];
    u32 used[3];
    u8 *sheet = scratch + 4608u;
    u32 data_bytes = get_le32(&header[508]);
    u32 frame_bytes = (u32)header[32] | (u32)header[33] << 8;
    u32 stamp = get_le32(&header[REPORT_OFFSET + sizeof(struct session_state) +
                                 __builtin_offsetof(struct take_state, sheet_stamp)]);
    u32 sectors = r->cur.clusters * r->vol.spc;
    u32 sector = 0u, index, bytes, first, count, done, run, chunk;

    r->vol.cached_fat_sector = 0u;
    if (r != &files || (data_bytes & (SECTOR_BYTES - 1u)) != 0u || stamp == 0u || sectors < 2u ||
        frame_bytes == 0u ||
        file_run(sectors - 1u, &sector) == 0u || fr_read(sector, sheet, 1u) != 0)
        return 0u;
    used[0] = (u32)sheet[SHEET_COUNT_AT] | (u32)sheet[SHEET_COUNT_AT + 1u] << 8;
    used[1] = sheet[SHEET_COUNT_AT + 2u];
    used[2] = sheet[SHEET_COUNT_AT + 3u];
    if (!equal_text(sheet, "DJMK", 4u) || get_le32(&sheet[4]) != stamp ||
        get_le32(&sheet[SHEET_SUM_AT]) != sheet_sum(sheet) ||
        used[0] > MAX_MARKS || used[1] > LOST_MARKS || used[2] > COARSE_MARKS)
        return 0u;
    for (index = 0u; index < used[0] + used[1] + used[2]; index++)
        user[index] = get_le32(&sheet[SHEET_POSITIONS_AT + 4u * index]);


    for (index = used[2]; index-- != 0u; )
        stretches[index] = user[used[0] + used[1] + index];
    for (index = used[1]; index-- != 0u; )
        places[index] = user[used[0] + index];
    lists[0] = user;
    lists[1] = places;
    lists[2] = stretches;
    bytes = build_tail(scratch, lists, used, data_bytes / frame_bytes);
    if (bytes == 0u)
        return 0u;
    for (index = bytes; (index & (SECTOR_BYTES - 1u)) != 0u; index++)
        scratch[index] = 0u;
    count = index / SECTOR_BYTES;
    first = (SECTOR_BYTES + data_bytes) / SECTOR_BYTES;
    if (first + count >= sectors)
        return 0u;
    for (done = 0u; done < count; done += chunk) {
        run = file_run(first + done, &sector);
        chunk = count - done < run ? count - done : run;
        if (chunk == 0u || fr_write(sector, scratch + done * SECTOR_BYTES, chunk) != 0)
            return 0u;
    }
    put_le32(&header[4], SECTOR_BYTES - 8u + data_bytes + bytes);
    return bytes;
}


static u32 ring_empty(void)
{
    return capture.pages_produced == pages_consumed && work.read_left == 0u;
}


static void finish_take(u32 now)
{
    u32 result;

    status_closing = 1u;
    release_pool_pages();
    if (capture.end_reason == END_SILENCE && !files.named &&
        capture.produced_words < MIN_KEEP_WORDS) {

        result = file_write(0u, rf_idle_header(&files), 1u);
        if (result != 0u) {
            take_failed(now, result, EVENT_FAIL);
            return;
        }
        session.takes_discarded++;
        SIM_EVENT(EVENT_DISCARD);
        enter_snapshot(PHASE_AUDIT);
        return;
    }
    result = write_cue(1u);
    if (result != 0u) {
        take_failed(now, result, EVENT_FAIL);
        return;
    }
    take.state = TAKE_COMPLETE;
    session.takes_kept++;
    session.status_flags |= take_flags();
    if (!files.named)
        take.number = files.next_number;
    result = write_header();
    if (result != 0u) {
        session.takes_kept--;
        take_failed(now, result, EVENT_FAIL);
        return;
    }
    SIM_EVENT(EVENT_KEEP);


    if (capture.end_reason == END_FULL || capture.end_reason == END_INVALID) {
        status_stopped = 1u;
        if (capture.end_reason == END_FULL)
            reclog_event(files.last_error == FR_E_IO || files.last_error == FR_E_VERIFY
                         ? RECLOG_ERROR : RECLOG_FULL, take.number, 0u, 0u);
    }
    work.failed_in_row = 0u;
    work.prepare_started_us = now;
    work.close_bytes = SECTOR_BYTES + take.actual_words * sample_bytes() + work.cue_bytes;
    enter_snapshot(PHASE_CLOSE);
}


static void close_files(u32 now)
{
    if (files.next > RF_NEXT_OFF && files.next < RF_NEXT_READY) {
        rf_next_step(&files);
        return;
    }
    if (work.close_bytes != 0u) {
        rf_begin_finish(&files, work.close_bytes);
        work.close_bytes = 0u;
    }
    files_step(now);
}


static void next_step(void)
{
    u32 saved;
    int result = rf_next_step(&files);

    session.next_state = files.next;
    if (result == FR_BUSY)
        return;
    if (result != FR_OK) {
        session.fs_error = result;
        session.fs_error_phase = files.error_phase;
        return;
    }
    session.files_made = files.made;
    saved = irq_lock();
    if (capture.state == CAPTURE_ACTIVE)
        capture.target_words += file_capacity_words(files.other.size);
    status_free = fr_free_clusters;
    irq_restore(saved);
}


static void name_take(void)
{
    u32 start = STOCK_TIME_US();
    u32 number = files.next_number;

    if (rf_commit_name(&files) == FR_OK) {
        take.number = number;
        session.next_number = files.next_number;


        if (take.chain_index == 0u)
            reclog_event(RECLOG_DEPTH, number, deep ? 24u : 16u, 0u);
    } else {
        take.flags |= STATUS_NAME_FAILED;
    }
    take.name_duration_us = (STOCK_TIME_US() - start) | 1u;
}


static u32 burst_due(void)
{
    if (capture.state == CAPTURE_DONE)
        return 1u;
    if (capture.captured_blocks - blocks_consumed + work.out_blocks >= burst_pages * file_page_blocks())
        return 1u;
    return capture.pages_produced - pages_consumed + BURST_FREE_PAGES >= capture.ring_pages;
}

static void stream(u32 now)
{
    u32 writes = 0u;
    u32 result;
    u32 gap;

    if (work.service_seen) {
        gap = now - work.last_service_us;
        if (gap > take.service_gap_max_us)
            take.service_gap_max_us = gap;
    }
    work.service_seen = 1u;
    work.last_service_us = now;

    if (work.cutting || work.cut_wanted || control_cut || take.actual_words == 0u)
        cut_hint = CUT_WAIT;
    else if (files.next != RF_NEXT_OFF && files.next != RF_NEXT_READY && !next_found_no_room())
        cut_hint = files.next == RF_NEXT_NONE ? CUT_LOW : CUT_WAIT;
    else
        cut_hint = cut_allowed() ? CUT_OK : CUT_LOW;
    if (work.cutting) {
        cut_step(now);
        return;
    }
    if (control_cut) {
        control_cut = 0u;
        work.cut_wanted = 1u;
    }


    if (burst_pages > 1u && !work.bursting) {
        if (!burst_due() && !work.cut_wanted)
            goto bookkeeping;
        work.bursting = 1u;
    }
    for (;;) {
        result = fill_out_page();
        if (result == 0u) {
            if (work.out_blocks == 0u)
                break;

            if (work.out_blocks < file_page_blocks() &&
                !(capture.state == CAPTURE_DONE && ring_empty()))
                break;
            if (writes == MAX_WRITES_PER_SERVICE)
                return;
            result = write_out_page(now);
            writes++;
        }
        if (result != 0u) {
            take_failed(now, result, EVENT_FAIL);
            return;
        }
    }
    work.bursting = 0u;
    if (capture.state == CAPTURE_DONE && ring_empty() && work.out_blocks == 0u) {
        finish_take(now);
        return;
    }
    if (work.cut_wanted) {

        work.cut_wanted = 0u;
        if (capture.state == CAPTURE_ACTIVE && cut_allowed()) {
            result = cut_begin(now);
            if (result != 0u)
                take_failed(now, result, EVENT_FAIL);
            return;
        }
    }


    if (elapsed(now, work.last_header_us) + (burst_pages - 1u) * file_page_blocks() * 250u >= HEADER_UPDATE_US) {
        work.last_header_us = now;
        result = sheet_dirty ? save_sheet() : 0u;
        if (result == 0u)
            result = write_header();
        if (result != 0u) {
            take_failed(now, result, EVENT_FAIL);
            return;
        }
        work.header_has_audio = take.actual_words != 0u;
    }
bookkeeping:

    if (burst_pages < 2u && capture.pages_produced - pages_consumed > 1u)
        return;
    if (!files.named && take.name_duration_us == 0u && take.actual_words >= MIN_KEEP_WORDS) {
        name_take();
    } else if (files.named && files.next == RF_NEXT_OFF && !work.next_tried &&
               capacity_words() - take.actual_words < next_ahead_words) {
        work.next_tried = 1u;
        rf_next_begin(&files);
    } else if (files.next > RF_NEXT_OFF && files.next < RF_NEXT_READY) {
        next_step();
    }
}


static u32 stick_gone(u32 now)
{
    u32 saved, armed;

    if (elapsed(now, work.poll_from_us) < STICK_POLL_US)
        return 0u;
    work.poll_from_us = now;
    if (find_stick() != 0) {
        work.gone_seen = 0u;
        return 0u;
    }


    if (!work.gone_seen) {
        work.gone_seen = 1u;
        work.poll_from_us = now - (STICK_POLL_US - STICK_AGAIN_US);
        return 0u;
    }
    work.gone_seen = 0u;
    saved = irq_lock();
    armed = capture.state == CAPTURE_ARMED;
    if (armed)
        capture.state = CAPTURE_IDLE;
    irq_restore(saved);
    if (!armed)
        return 0u;
    release_map();
    work.stick_present = 0u;
    work.retry = 0u;
    status_reason = REASON_NO_USB;
    begin_prepare(now, 0u);
    enter_snapshot(PHASE_PREPARE);
    return 1u;
}

__attribute__((section(".text.entry")))
void recorder_service(void)
{
    u32 now;

    STOCK_BACKGROUND_CALLBACK();
    now = STOCK_TIME_US();
    status_free = fr_free_clusters;

    switch (session.phase) {
    case PHASE_VERIFY_MEMORY: verify_memory_chunk(); return;
    case PHASE_SETTLE: settle(now); return;
    case PHASE_SNAPSHOT: snapshot(); return;
    case PHASE_PREPARE: prepare(now); return;
    case PHASE_CLOSE: close_files(now); return;
    case PHASE_LOG:


        if (reclog_step(&files) == FR_BUSY)
            return;
        session.text_lines = reclog_lines;
        session.text_error = reclog_error;
        if (work.fail_error != 0) {
            int error = work.fail_error;

            work.fail_error = 0;
            session.phase = PHASE_PREPARE;
            files_failed(now, error);
            return;
        }
        log_once(0x40000000u ^ reclog_lines);
        session.phase = PHASE_AUDIT;
        return;
    case PHASE_AUDIT: audit(now); return;
    case PHASE_ARMED:
        if (capture.state != CAPTURE_ARMED)
            start_take(now);
        else if (!stick_gone(now))
            armed_audit();
        return;
    case PHASE_STREAM: stream(now); return;
    default: return;
    }
}


void tunnel_status(u8 *status)
{
    u32 state = capture.state;


    u32 before = take.blocks_before;
    u32 blocks = capture.captured_blocks > before ? capture.captured_blocks - before : 0u;
    u32 seconds = (state == CAPTURE_ACTIVE || state == CAPTURE_DONE) ? blocks / 4000u : 0u;
    u32 flags = session.status_flags;


    u32 marks = mark_count;


    if (state == CAPTURE_IDLE && status_closing)
        state = CAPTURE_DONE;
    status[3] = (u8)state;
    status[4] = (u8)((capture.by_hand & 1u) | (control_start != 0u ? 2u : 0u) |
                     ((state == CAPTURE_ACTIVE || state == CAPTURE_DONE) &&
                      capture.dropped_blocks != take.dropped_before ? 4u : 0u) |
                     (status_stopped ? 8u : 0u) | (deep ? STATUS_DEEP : 0u));
    status[5] = (u8)(seconds / 3600u > 255u ? 255u : seconds / 3600u);
    status[6] = (u8)(seconds / 60u % 60u);
    status[7] = (u8)(seconds % 60u);
    status[8] = (u8)(marks > 255u ? 255u : marks);
    status[11] = (u8)(flags >> 24);
    status[12] = (u8)(flags >> 16);
    status[13] = (u8)(flags >> 8);
    status[14] = (u8)flags;
    status[15] = (u8)(state == CAPTURE_ACTIVE || state == CAPTURE_DONE
                      ? (take.chain_index > 254u ? 255u : take.chain_index + 1u) : 0u);
    status[16] = (u8)(state == CAPTURE_IDLE ? status_reason + 1u : 0u);
    status[17] = (u8)(state == CAPTURE_ACTIVE ? cut_hint : 0u);


    if (capture.state == CAPTURE_ACTIVE && (files.next == RF_NEXT_NONE || files.next == RF_NEXT_READY)) {
        u32 left = capture.target_words - capture.produced_words;

        if (left < countdown_words) {
            left /= 96000u;
            status[18] = (u8)(left / 60u + 1u);
            status[19] = (u8)(left % 60u);
        }
    }


    status[20] = 255u;
    if ((capture.state == CAPTURE_ARMED || capture.state == CAPTURE_ACTIVE) && status_free != FR_FREE_UNKNOWN) {
        u32 sectors = status_free * files.vol.spc, margin = files.margin_bytes / SECTOR_BYTES;


        u32 left = capture.target_words - capture.produced_words;
        u32 mb;

        left = left < 0x80000000u ? left : 0u;
        mb = left / 1000u * sample_bytes() / 1000u;
        left /= 96000u;
        sectors = sectors > margin && sectors - margin >= files.min_bytes / SECTOR_BYTES ? sectors - margin : 0u;
        if (deep)
            left += sectors / 1125u * 2u + sectors % 1125u * 2u / 1125u;
        else
            left += sectors / 375u;
        mb += sectors / 15625u * 8u + sectors % 15625u * 8u / 15625u;
        mb = mb >= 1000u ? (mb / 100u > 0x7FFFu ? 0x7FFFu : mb / 100u) : mb | 0x8000u;
        status[9] = (u8)(mb >> 8);
        status[10] = (u8)mb;
        status[20] = (u8)(left / 3600u > 254u ? 254u : left / 3600u);
        status[21] = (u8)(left / 60u % 60u);
        status[22] = (u8)(left % 60u);
    }
}


void control_command(u32 code)
{
    if (code == COMMAND_MARK) {
        if (capture.state == CAPTURE_ACTIVE) {
            capture.marks_set++;
            if (mark_count < MAX_MARKS) {
                marks[mark_count++] = capture.captured_blocks * 12u;
                sheet_dirty = 1u;
            }
        }
    } else if (code == COMMAND_CUT) {
        if (capture.state == CAPTURE_ACTIVE)
            control_cut = 1u;
    } else if (code == COMMAND_START) {
        if (capture.state != CAPTURE_ACTIVE)
            control_start = START_WAIT_BLOCKS;
    } else if (code == COMMAND_STOP) {
        control_stop = 1u;
    } else if (code == COMMAND_BITS) {


        if ((capture.state == CAPTURE_IDLE || capture.state == CAPTURE_ARMED) && !status_closing) {
            deep ^= 1u;
            if (capture.state == CAPTURE_ARMED)
                capture.target_words = capture.produced_words + capacity_words();
        }
    }
}


void control_message(u32 status, u32 data1, u32 value)
{
    u32 key = status << 8 | data1;

    if ((value == 0u && !every_message) || key == 0u)
        return;
    if (key == mark_key) {
        if (capture.state == CAPTURE_ACTIVE) {
            capture.marks_set++;
            if (mark_count < MAX_MARKS) {
                marks[mark_count++] = capture.captured_blocks * 12u;
                sheet_dirty = 1u;
            }
        }
    } else if (key == cut_key) {
        if (capture.state == CAPTURE_ACTIVE)
            control_cut = 1u;
    } else if (key == start_key && !(key == stop_key && capture.state == CAPTURE_ACTIVE)) {
        control_start = START_WAIT_BLOCKS;
    } else if (key == stop_key) {
        control_stop = 1u;
    }
}

static int address_valid(u32 address)
{
    return address == SEND_BUFFER_0 || address == SEND_BUFFER_1;
}

void stream_capture_isr(void)
{
    u32 address;
    u32 count;
    u32 state;
    u32 page;
    u32 index;
    u32 word;
    u32 loud;
    s32 sample;
    volatile u32 *source;

    ORIGINAL_SEND_ISR();
    listen_poll(LINK_RX, LINK_TX, LINK_STATE);
    state = capture.state;
    if (state == CAPTURE_ACTIVE || state == CAPTURE_DONE)
        control_start = 0u;
    else if (state != CAPTURE_ARMED && control_start != 0u)
        control_start--;
    if (state != CAPTURE_ACTIVE)
        control_stop = 0u;
    if (state != CAPTURE_ARMED && state != CAPTURE_ACTIVE)
        return;
    count = DMA3_RDMATCR & 0x00FFFFFFu;
    address = DMA3_RDAR;
    if (count != 24u || !address_valid(address)) {
        capture.invalid_blocks++;
        if (state == CAPTURE_ACTIVE)
            stop_capture(END_INVALID);
        return;
    }
    source = (volatile u32 *)address;
    if (state == CAPTURE_ARMED) {
        loud = 0u;
        for (index = 0u; index < 24u; index++) {
            sample = (s32)source[index] >> 8;
            if (sample > AUDIO_TRIGGER_THRESHOLD ||
                sample < -AUDIO_TRIGGER_THRESHOLD) {
                loud = 1u;
                break;
            }
        }
        if (control_hold) {
            if (loud)
                control_quiet = 0u;
            else if (++control_quiet >= HOLD_BLOCKS)
                control_hold = 0u;
        }
        if (control_start) {
            control_start = 0u;
            control_hold = 0u;
            capture.by_hand = 1u;
        } else if (!(auto_start && loud && !control_hold)) {
            return;
        }
        capture.state = CAPTURE_ACTIVE;
    }
    if (capture.current_page_bytes + PACK_MAX_BYTES > PAGE_BYTES ||
        capture.current_page_blocks >= RING_PAGE_BLOCKS)
        publish_current_page();
    if (capture.pages_produced - pages_consumed >= capture.ring_pages) {


        capture.dropped_blocks++;
        if (!capture.in_gap) {
            capture.in_gap = 1u;
            capture.gap_count++;
            if (lost_count < LOST_MARKS) {
                lost[lost_count].frame = capture.captured_blocks * 12u;
                lost[lost_count].blocks = 1u;
                lost_count++;
                lost_open = 1u;
                sheet_dirty = 1u;
            } else {
                lost_open = 0u;
                lost_more++;
                lost_more_blocks++;
            }
        } else if (lost_open) {
            lost[lost_count - 1u].blocks++;
        } else {
            lost_more_blocks++;
        }
        return;
    }
    capture.in_gap = 0u;


    page = capture.pages_produced % capture.ring_pages;
    if (capture.current_page_bytes == 0u) {
        u32 waiting = capture.pages_produced - pages_consumed;

        capture_pack.x1[0] = capture_pack.x1[1] = 0;
        capture_pack.x2[0] = capture_pack.x2[1] = 0;
        if (!deep) {
            coarse_now = 1u;
        } else if (!coarse_now) {
            if (waiting * COARSE_FROM_DEN >= capture.ring_pages * COARSE_FROM_NUM) {

                coarse_now = 1u;
                if (capture.coarse < 0xFF000000u)
                    capture.coarse += 0x01000000u;
                if (coarse_count < COARSE_MARKS) {
                    coarse[coarse_count].frame = capture.captured_blocks * 12u;
                    coarse[coarse_count].blocks = 0u;
                    coarse_count++;
                    coarse_open = 1u;
                    sheet_dirty = 1u;
                } else {
                    coarse_open = 0u;
                    coarse_more++;
                    coarse_more_last = 1u;
                    coarse_more_frame = capture.captured_blocks * 12u;
                    coarse_more_run = 0u;
                }
            }
        } else if (waiting <= COARSE_END_PAGES) {
            coarse_now = 0u;
        }
    }


    word = dither_state;
    loud = 0u;
    for (index = 0u; index < 24u; index++) {
        sample = (s32)source[index] >> 8;
        if (sample > AUDIO_TRIGGER_THRESHOLD || sample < -AUDIO_TRIGGER_THRESHOLD)
            loud = 1u;
        if (coarse_now) {
            word = word * 1664525u + 1013904223u;
            sample += 128 + (s32)((word >> 16) & 0xFFu) - (s32)(word >> 24);
            sample >>= 8;
            if (sample > 32767)
                sample = 32767;
            if (sample < -32768)
                sample = -32768;
        }
        capture_samples[index] = sample;
    }
    dither_state = word;
    if (coarse_now && deep) {
        if ((capture.coarse & 0x00FFFFFFu) != 0x00FFFFFFu)
            capture.coarse++;
        if (coarse_open) {
            coarse[coarse_count - 1u].blocks++;
        } else {
            coarse_more_blocks++;
            coarse_more_run++;
        }
    }
    capture.current_page_bytes +=
        pack_block(capture_samples, (u8 *)page_buffers[page] + capture.current_page_bytes, &capture_pack);
    capture.current_page_blocks++;
    capture.produced_words += 24u;
    capture.captured_blocks++;
    update_pending();
    if (capture.produced_words == capture.target_words) {
        stop_capture(END_FULL);
    } else if (control_stop) {
        control_stop = 0u;
        control_hold = 1u;
        control_quiet = 0u;
        capture.by_hand |= 2u;
        stop_capture(END_MANUAL);
    } else if (loud) {
        capture.silent_blocks = 0u;
    } else if (++capture.silent_blocks >= SILENCE_BLOCKS && !(capture.by_hand & 1u)) {
        stop_capture(END_SILENCE);
    }
}

#ifdef SIM
u32 stream_sim_status_flags(void) { return session.status_flags; }
u32 stream_sim_phase(void) { return session.phase; }
u32 stream_sim_captured_blocks(void) { return capture.captured_blocks; }
u32 stream_sim_capture_state(void) { return capture.state; }
u32 stream_sim_coarse(void) { return coarse_now; }
u32 stream_sim_deep(void) { return deep; }


void stream_sim_reboot(void)
{
    u32 index;
    volatile u32 *c = (volatile u32 *)&capture;
    u32 *p;

    for (index = 0u; index < sizeof(capture) / 4u; index++)
        c[index] = 0u;
    for (p = (u32 *)&session, index = 0u; index < sizeof(session) / 4u; index++)
        p[index] = 0u;
    for (p = (u32 *)&take, index = 0u; index < sizeof(take) / 4u; index++)
        p[index] = 0u;
    for (p = (u32 *)&work, index = 0u; index < sizeof(work) / 4u; index++)
        p[index] = 0u;
    for (p = (u32 *)&files, index = 0u; index < sizeof(files) / 4u; index++)
        p[index] = 0u;
    for (index = 0u; index < MAX_PAGES; index++) {
        page_lengths[index] = 0u;
        page_blocks[index] = 0u;
        page_buffers[index] = 0;
    }
    pages_consumed = 0u;
    blocks_consumed = 0u;
    status_reason = REASON_INIT;
    status_closing = 0u;
    status_stopped = 0u;
    cut_hint = CUT_WAIT;
    mark_count = 0u;
    lost_count = 0u;
    lost_open = 0u;
    lost_more = 0u;
    lost_more_blocks = 0u;
    coarse_now = 0u;
    coarse_count = 0u;
    coarse_open = 0u;
    coarse_more = 0u;
    coarse_more_blocks = 0u;
    coarse_more_last = 0u;
    coarse_more_run = 0u;
    dither_state = 0u;
    sheet_dirty = 0u;
    deep = DEPTH_AT_POWER_ON;
    reclog_power_on();
    session.phase = PHASE_VERIFY_MEMORY;
    files.cap_bytes = FILE_CAP_BYTES;
    files.min_bytes = FILE_MIN_BYTES;
    files.refill_below_bytes = FILE_REFILL_BYTES;
    files.margin_bytes = FILE_MARGIN_BYTES;
}
#endif
