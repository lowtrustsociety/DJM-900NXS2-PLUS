/* REC-LOG.TXT: the recorder's log on the stick.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "reclog.h"

#define HEADING      "DJM RECORDER LOG\r\n"
#define HEADING_LEN  18u
#define LINE_MAX     64u
#define LAST_NAME    31u

enum {
    L_IDLE = 0,
    L_SCAN,
    L_HEAD,
    L_MAP,
    L_APPEND,
    L_SIZE,
    L_GROW_SCAN,
    L_GROW_COMMIT,
    L_GROW_LINK,
    L_NEW_SCAN,
    L_NEW_COMMIT,
    L_NEW_DIR
};

struct event {
    u16 kind;
    u16 number;
    u32 a;
    u32 b;
};

static struct event queue[RECLOG_EVENTS] = { { 0u, 0u, 0u, 0u } };
static u32 queued = 0u;
static u32 overflow = 0u;
u32 reclog_lines = 0u;
s32 reclog_error = 0;

static struct {
    u32 phase;
    u32 want;
    u32 index;
    u32 cursor;
    u32 offset;
    u32 creating;
    u32 heading;
    u32 note;
    u32 done;
    u32 line_event;
    u32 line_length;
    u32 line_at;
    struct fr_file file;
    struct fr_file grow;
    struct extent grow_extent;
    u8 line[LINE_MAX];
} L = { .phase = L_IDLE };

void reclog_event(u32 kind, u32 number, u32 a, u32 b)
{
    if (queued < RECLOG_EVENTS) {
        queue[queued].kind = (u16)kind;
        queue[queued].number = (u16)number;
        queue[queued].a = a;
        queue[queued].b = b;
        queued++;
    } else {
        overflow++;
    }
}

void reclog_reset(void)
{
    L.phase = L_IDLE;
    L.done = 0u;
    L.note = 0u;
    L.line_length = 0u;
    L.line_at = 0u;
}

static int fail(int error)
{
    reclog_reset();
    reclog_error = error;
    return error;
}


static u8 *put(u8 *p, const char *text)
{
    while (*text)
        *p++ = (u8)*text++;
    return p;
}

static u8 *put_number(u8 *p, u32 value, u32 digits)
{
    u8 buffer[10];
    u32 n = 0u;

    do {
        buffer[n++] = (u8)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u || n < digits);
    while (n != 0u)
        *p++ = buffer[--n];
    return p;
}


static u8 *put_seconds(u8 *p, u32 blocks)
{
    u32 hundredths = (blocks + 20u) / 40u;

    p = put_number(p, hundredths / 100u, 1u);
    *p++ = '.';
    p = put_number(p, hundredths % 100u, 2u);
    return put(p, " S");
}

static void make_line(const struct event *e)
{
    u8 *p = L.line;
    u32 seconds;

    if (e->number != 0u) {
        p = put(p, "REC");
        p = put_number(p, e->number, 4u);
        p = put(p, ".WAV");
    } else if (e->kind != RECLOG_FIXED) {
        p = put(p, "RECORDING");
    }
    switch (e->kind) {
    case RECLOG_LOST:
    case RECLOG_COARSE:
        seconds = e->a / 48000u;
        *p++ = ' ';
        if (seconds >= 3600u) {
            p = put_number(p, seconds / 3600u, 1u);
            *p++ = ':';
        }
        p = put_number(p, seconds / 60u % 60u, 2u);
        *p++ = ':';
        p = put_number(p, seconds % 60u, 2u);
        p = put(p, e->kind == RECLOG_LOST ? " LOST " : " 16 BIT ");
        p = put_seconds(p, e->b);
        break;
    case RECLOG_MORE:
    case RECLOG_COARSE_MORE:
        p = put(p, e->kind == RECLOG_MORE ? " LOST " : " 16 BIT ");
        p = put_number(p, e->a, 1u);
        p = put(p, " MORE TIMES, ");
        p = put_seconds(p, e->b);
        break;
    case RECLOG_REPAIR:
        p = put(p, " NOT CLOSED (POWER OR USB LOST), REPAIRED");
        break;
    case RECLOG_FULL:
        p = put(p, " STOPPED, USB FULL");
        break;
    case RECLOG_REFUSED:
        p = put(p, " START REFUSED, ");
        p = put(p, e->a == RF_REFUSED_FILE ? "WAITING FILE CHANGED"
                 : e->a == RF_REFUSED_STICK ? "NOT THE SAME STICK" : "USB ERROR");
        break;
    case RECLOG_FIXED:
        p = put(p, "DJMREC.TMP CHANGED BY A COMPUTER, REPAIRED");
        break;
    case RECLOG_DEPTH:
        *p++ = ' ';
        p = put_number(p, e->a, 1u);
        p = put(p, " BIT");
        break;
    default:
        p = put(p, " STOPPED, USB ERROR");
        break;
    }
    p = put(p, "\r\n");
    L.line_length = (u32)(p - L.line);
}


static u32 next_line(void)
{
    if (L.line_at < L.line_length)
        return 1u;
    L.line_at = 0u;
    L.line_event = 0u;
    if (L.heading) {
        L.heading = 0u;
        L.line_length = (u32)(put(L.line, HEADING) - L.line);
    } else if (L.done < queued) {
        make_line(&queue[L.done]);
        L.line_event = 1u;
    } else if (overflow != 0u && !L.note) {
        L.note = 1u;
        L.line_length = (u32)(put(L.line, "MORE NOT LISTED\r\n") - L.line);
    } else {
        L.line_length = 0u;
        return 0u;
    }
    return 1u;
}


static void commit(void)
{
    u32 index;

    reclog_lines += L.done;
    for (index = L.done; index < queued; index++) {
        queue[index - L.done].kind = queue[index].kind;
        queue[index - L.done].number = queue[index].number;
        queue[index - L.done].a = queue[index].a;
        queue[index - L.done].b = queue[index].b;
    }
    queued -= L.done;
    L.done = 0u;
    if (L.note && L.line_at == L.line_length) {
        L.note = 0u;
        overflow = 0u;
    }
}


static u32 cluster_bytes(const struct rf *r) { return r->vol.spc * 512u; }

static void scan_begin(struct rf *r)
{
    fr_dirscan_begin(&r->vol, &r->dir);
    r->dir.txt_want = L.want;
    L.phase = L_SCAN;
}


static int next_name(struct rf *r)
{
    L.want = L.index + 1u;
    if (L.want > LAST_NAME)
        return fail(FR_E_DIR);
    scan_begin(r);
    return FR_BUSY;
}


static u32 file_sector(const struct rf *r, u32 index)
{
    u32 spc = r->vol.spc;
    u32 cluster = index / spc;
    u32 e;

    for (e = 0u; e < L.file.extent_count; e++) {
        if (cluster < L.file.extents[e].clusters)
            return L.file.extents[e].first_sector + cluster * spc + index % spc;
        cluster -= L.file.extents[e].clusters;
    }
    return 0u;
}


static u32 chain_cluster(const struct rf *r, u32 n)
{
    return fr_sector_cluster(&r->vol, file_sector(r, n * r->vol.spc));
}

static int plain(u8 c)
{
    return c == '\r' || c == '\n' || c == '\t' || (c >= 0x20u && c < 0x7Fu);
}

static int grow_begin(struct rf *r)
{
    L.grow.extents = &L.grow_extent;
    L.grow.max_extents = 1u;
    L.grow.extent_count = 0u;
    L.grow.clusters = 0u;
    fr_scan_begin(&r->vol, &r->scan, 1u, 1u);
    L.phase = L_GROW_SCAN;
    return FR_BUSY;
}


static int after_commit(struct rf *r)
{
    if (next_line())
        return grow_begin(r);
    L.phase = L_IDLE;
    reclog_error = 0;
    return FR_OK;
}

int reclog_step(struct rf *r)
{
    u8 *b;
    struct extent *e;
    u32 cb = cluster_bytes(r);
    u32 at, n, sector, index;
    int result;

    switch (L.phase) {
    case L_IDLE:
        if (queued == 0u && overflow == 0u)
            return FR_OK;
        L.want = 1u;
        scan_begin(r);
        return FR_BUSY;

    case L_SCAN:
        result = fr_dirscan_step(&r->vol, &r->dir);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(result);
        L.file.extents = r->other.extents;
        L.file.max_extents = r->other.max_extents;
        L.file.extent_count = 0u;
        L.file.clusters = 0u;
        L.creating = 0u;
        L.heading = 0u;
        if (r->dir.txt_index == 0u) {

            for (index = 1u; index <= LAST_NAME && (r->dir.txt_mask >> index & 1u) != 0u; index++)
                ;
            if (index > LAST_NAME || !r->dir.have_free)
                return fail(FR_E_DIR);
            L.index = index;
            fr_log_name(L.file.name, index);
            L.file.attr = (u8)FR_ATTR_ARCHIVE;
            L.file.dir_sector = r->dir.free_sector;
            L.file.dir_index = r->dir.free_index;
            L.file.size = 0u;
            L.creating = 1u;
            L.heading = 1u;
            L.offset = 0u;
            fr_scan_begin(&r->vol, &r->scan, 1u, 1u);
            L.phase = L_NEW_SCAN;
            return FR_BUSY;
        }
        L.index = r->dir.txt_index;
        fr_log_name(L.file.name, L.index);
        L.file.attr = (u8)r->dir.txt_attr;
        L.file.dir_sector = r->dir.txt_sector;
        L.file.dir_index = r->dir.txt_slot;
        L.file.first_cluster = r->dir.txt_first;
        L.file.size = r->dir.txt_size;
        if (L.file.size < HEADING_LEN || (L.file.attr & 0x01u) != 0u ||
            L.file.first_cluster < 2u || L.file.first_cluster >= r->vol.end_cluster)
            return next_name(r);
        L.phase = L_HEAD;
        return FR_BUSY;

    case L_HEAD:
        b = fr_dirbuf();
        if (fr_read(fr_cluster_sector(&r->vol, L.file.first_cluster), b, 1u) != 0)
            return fail(FR_E_IO);
        for (index = 0u; index < HEADING_LEN; index++) {
            if (b[index] != (u8)HEADING[index])
                return next_name(r);
        }
        fr_map_begin(&L.file);
        L.cursor = 0u;
        L.phase = L_MAP;
        return FR_BUSY;

    case L_MAP:
        result = fr_map_step(&r->vol, &L.file, &L.cursor, 0u);
        if (result == FR_BUSY)
            return FR_BUSY;
        if (result == FR_E_CHAIN || result == FR_E_OPEN)
            return next_name(r);
        if (result != FR_OK)
            return fail(result);
        if (L.file.size > L.file.clusters * cb)
            return next_name(r);

        b = fr_dirbuf();
        sector = file_sector(r, (L.file.size - 1u) / 512u);
        if (sector == 0u || fr_read(sector, b, 1u) != 0)
            return fail(FR_E_IO);
        for (index = 0u; index <= (L.file.size - 1u) % 512u; index++) {
            if (!plain(b[index]))
                return next_name(r);
        }


        if (L.file.clusters > (L.file.size + cb - 1u) / cb) {
            result = fr_fat_sync(&r->vol, chain_cluster(r, L.file.clusters - 2u));
            if (result == FR_OK)
                result = fr_fat_sync(&r->vol, chain_cluster(r, L.file.clusters - 1u));
            if (result != FR_OK)
                return fail(result);
        }
        L.offset = L.file.size;
        if (L.offset == L.file.clusters * cb)
            return grow_begin(r);
        L.phase = L_APPEND;
        return FR_BUSY;

    case L_APPEND:
        b = fr_fatbuf();
        r->vol.cached_fat_sector = 0u;
        at = L.offset % 512u;
        sector = file_sector(r, L.offset / 512u);
        if (sector == 0u)
            return fail(FR_E_CHAIN);
        if (at != 0u && fr_read(sector, b, 1u) != 0)
            return fail(FR_E_IO);
        for (n = at; n < 512u && next_line(); n++) {
            b[n] = L.line[L.line_at++];
            if (L.line_at == L.line_length && L.line_event)
                L.done++;
        }
        for (index = n; index < 512u; index++)
            b[index] = 0u;
        if (fr_write(sector, b, 1u) != 0)
            return fail(FR_E_IO);
        L.offset += n - at;
        if (L.offset % cb != 0u && next_line())
            return FR_BUSY;
        if (L.creating) {
            fr_commit_begin(&r->commit, 0u, 0u, L.file.extent_count, FR_EOC, FR_LINK);
            L.phase = L_NEW_COMMIT;
        } else {
            L.phase = L_SIZE;
        }
        return FR_BUSY;

    case L_SIZE:
        L.file.size = L.offset;
        result = fr_dir_write(&L.file, 0, 0);
        if (result != FR_OK)
            return fail(result);
        commit();
        return after_commit(r);

    case L_GROW_SCAN:
        result = fr_scan_step(&r->vol, &r->scan, &L.grow);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(result);
        if (L.grow.clusters == 0u)
            return fail(FR_E_NO_SPACE);
        fr_commit_begin(&r->commit, 0u, 0u, 1u, FR_EOC, FR_LINK);
        L.phase = L_GROW_COMMIT;
        return FR_BUSY;

    case L_GROW_COMMIT:
        result = fr_commit_step(&r->vol, &r->commit, &L.grow);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(result);
        L.phase = L_GROW_LINK;
        return FR_BUSY;

    case L_GROW_LINK:
        e = &L.file.extents[L.file.extent_count - 1u];
        index = L.grow_extent.first_sector == e->first_sector + e->clusters * r->vol.spc;
        if (!index && L.file.extent_count >= L.file.max_extents)
            return fail(FR_E_CHAIN);
        result = fr_fat_set(&r->vol, fr_sector_cluster(&r->vol, e->first_sector) + e->clusters - 1u,
                            FR_EOC, L.grow.first_cluster);
        if (result != FR_OK)
            return fail(result);
        if (index)
            e->clusters++;
        else {
            L.file.extents[L.file.extent_count].first_sector = L.grow_extent.first_sector;
            L.file.extents[L.file.extent_count].clusters = L.grow_extent.clusters;
            L.file.extent_count++;
        }
        L.file.clusters++;
        result = fr_fsinfo_adjust(&r->vol, 1, r->vol.scan_from);
        if (result != FR_OK)
            return fail(result);
        L.phase = L_APPEND;
        return FR_BUSY;

    case L_NEW_SCAN:
        result = fr_scan_step(&r->vol, &r->scan, &L.file);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(result);
        if (L.file.clusters == 0u)
            return fail(FR_E_NO_SPACE);
        L.phase = L_APPEND;
        return FR_BUSY;

    case L_NEW_COMMIT:
        result = fr_commit_step(&r->vol, &r->commit, &L.file);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(result);
        L.phase = L_NEW_DIR;
        return FR_BUSY;

    case L_NEW_DIR:
        L.file.size = L.offset;
        result = fr_dir_write(&L.file, 1, 0);
        if (result == FR_OK)
            result = fr_fsinfo_adjust(&r->vol, 1, r->vol.scan_from);
        if (result != FR_OK)
            return fail(result);
        L.creating = 0u;
        commit();
        return after_commit(r);

    default:
        return fail(FR_E_CHANGED);
    }
}


void reclog_power_on(void)
{
    u32 *w = (u32 *)&L;
    u32 index;

    for (index = 0u; index < sizeof(L) / 4u; index++)
        w[index] = 0u;
    queued = 0u;
    overflow = 0u;
    reclog_lines = 0u;
    reclog_error = 0;
}
