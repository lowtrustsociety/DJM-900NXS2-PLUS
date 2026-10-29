/* The recording files: the waiting file, names, headers, closing.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "recfile.h"

static const char tmp_name[] = "DJMREC  TMP";

static u32 cluster_bytes(const struct rf *r) { return r->vol.spc * 512u; }

static int fail(struct rf *r, int error)
{
    r->last_error = error;
    r->error_phase = r->phase;
    r->phase = RF_IDLE;
    return error;
}

static void copy_bytes(u8 *destination, const u8 *source, u32 count)
{
    while (count-- != 0u)
        *destination++ = *source++;
}

static int equal_bytes(const u8 *a, const u8 *b, u32 count)
{
    while (count-- != 0u) {
        if (*a++ != *b++)
            return 0;
    }
    return 1;
}

static void empty_file(struct fr_file *f)
{
    f->extent_count = 0u;
    f->clusters = 0u;
    f->first_cluster = 0u;
    f->size = 0u;
}


static void drop_tail(struct fr_file *f, u32 clusters)
{
    while (clusters != 0u && f->extent_count != 0u) {
        struct extent *e = &f->extents[f->extent_count - 1u];
        u32 n = e->clusters < clusters ? e->clusters : clusters;
        e->clusters -= n;
        f->clusters -= n;
        clusters -= n;
        if (e->clusters == 0u)
            f->extent_count--;
    }
}

static u32 last_cluster(const struct rf *r, const struct fr_file *f)
{
    const struct extent *e = &f->extents[f->extent_count - 1u];
    return fr_sector_cluster(&r->vol, e->first_sector) + e->clusters - 1u;
}


static u32 chain_cluster(const struct rf *r, const struct fr_file *f, u32 index)
{
    u32 e;
    for (e = 0u; e < f->extent_count; e++) {
        if (index < f->extents[e].clusters)
            return fr_sector_cluster(&r->vol, f->extents[e].first_sector) + index;
        index -= f->extents[e].clusters;
    }
    return 0u;
}


static int write_checked(u32 sector, const u8 *data)
{
    u8 *b = fr_dirbuf();
    if (fr_write(sector, data, 1u) != 0 || fr_read(sector, b, 1u) != 0)
        return FR_E_IO;
    return equal_bytes(b, data, 512u) ? FR_OK : FR_E_VERIFY;
}


static u32 reserve_clusters(const struct rf *r)
{
    return (r->margin_bytes + cluster_bytes(r) - 1u) / cluster_bytes(r);
}

static void set_waiting(struct fr_file *f, u32 bytes_per_cluster)
{
    fr_name(f->name, tmp_name);
    f->attr = (u8)(FR_ATTR_HIDDEN | FR_ATTR_ARCHIVE);
    f->size = f->clusters * bytes_per_cluster;
}


static int begin_space(struct rf *r, u32 adding)
{
    u32 cap = r->cap_bytes / cluster_bytes(r);
    u32 min_run = 65536u / cluster_bytes(r);
    struct fr_file *f = adding ? &r->other : &r->cur;
    u32 target;

    r->adding = adding;
    empty_file(f);
    if (adding) {
        if (r->cur.clusters >= cap || r->cur.extent_count >= r->cur.max_extents) {
            r->phase = RF_READY;
            return FR_OK;
        }
        target = cap - r->cur.clusters;
        r->other.max_extents = r->cur.max_extents - r->cur.extent_count;
    } else {
        target = cap;
    }
    fr_scan_begin(&r->vol, &r->scan, target + reserve_clusters(r), min_run);
    r->phase = RF_SCAN;
    return FR_BUSY;
}


static int waiting_ready(struct rf *r)
{
    r->named = 0u;
    if (!r->no_refill && (r->cur.size < r->refill_below_bytes || r->cur.size < r->min_bytes))
        return begin_space(r, 1u);
    r->phase = RF_READY;
    return FR_OK;
}

void rf_begin_prepare(struct rf *r)
{
    r->phase = RF_MOUNT;
    r->adding = 0u;
    r->recovering = 0u;
    r->named = 0u;
    r->next = RF_NEXT_OFF;
    r->tmp_pending = 0u;
    r->reusing = 0u;
    r->repaired = 0u;
    r->entry_fixed = 0u;
    r->refused = 0u;
    r->last_error = 0;
}

static void swap_files(struct rf *r)
{
    u8 swap[sizeof(struct fr_file)];
    copy_bytes(swap, (const u8 *)&r->cur, sizeof(swap));
    copy_bytes((u8 *)&r->cur, (const u8 *)&r->other, sizeof(swap));
    copy_bytes((u8 *)&r->other, swap, sizeof(swap));
}


static int take_up_waiting(struct rf *r)
{
    r->cur.dir_sector = r->dir.tmp_sector;
    r->cur.dir_index = r->dir.tmp_index;
    r->cur.first_cluster = r->dir.tmp_first;
    r->cur.size = r->dir.tmp_size;
    fr_name(r->cur.name, tmp_name);
    r->cur.attr = (u8)(FR_ATTR_HIDDEN | FR_ATTR_ARCHIVE);
    fr_map_begin(&r->cur);
    r->cursor = 0u;
    r->tmp_pending = 0u;
    r->phase = RF_TMP_MAP;
    return FR_BUSY;
}


void rf_next_begin(struct rf *r)
{
    if (r->phase != RF_READY || !r->named) {
        r->next = RF_NEXT_NONE;
        return;
    }
    empty_file(&r->other);
    r->other.max_extents = r->cur.max_extents;
    fr_dirscan_begin(&r->vol, &r->dir);
    r->next = RF_NEXT_SLOT;
}

static int next_none(struct rf *r, int error)
{
    r->last_error = error;
    r->error_phase = 0x100u + r->next;


    if (r->next == RF_NEXT_COMMIT || r->next == RF_NEXT_FSINFO)
        r->tmp_pending = 1u;
    r->next = RF_NEXT_NONE;
    return error;
}


int rf_next_step(struct rf *r)
{
    u32 cb = cluster_bytes(r);
    int result;

    switch (r->next) {
    case RF_NEXT_SLOT:
        result = fr_dirscan_step(&r->vol, &r->dir);
        if (result == FR_BUSY)
            return FR_BUSY;
        if (result != FR_OK)
            return next_none(r, result);
        if (!r->dir.have_free || r->dir.tmp_found)
            return next_none(r, FR_E_DIR);
        if (r->dir.rec_found && r->dir.max_number + 1u > r->next_number)
            r->next_number = r->dir.max_number + 1u;
        fr_scan_begin(&r->vol, &r->scan, r->cap_bytes / cb + reserve_clusters(r), 65536u / cb);
        r->next = RF_NEXT_SCAN;
        return FR_BUSY;
    case RF_NEXT_SCAN:
        result = fr_scan_step(&r->vol, &r->scan, &r->other);
        if (result == FR_BUSY)
            return FR_BUSY;
        if (result != FR_OK)
            return next_none(r, result);
        drop_tail(&r->other, reserve_clusters(r));
        if (r->other.clusters == 0u || r->other.clusters < r->min_bytes / cb)
            return next_none(r, FR_E_NO_SPACE);
        r->next = RF_NEXT_HEAD;
        return FR_BUSY;
    case RF_NEXT_HEAD:
        result = write_checked(r->other.extents[0].first_sector, rf_idle_header(r));
        if (result != FR_OK)
            return next_none(r, result);
        r->next = RF_NEXT_DIR;
        return FR_BUSY;
    case RF_NEXT_DIR:
        set_waiting(&r->other, cb);
        r->other.dir_sector = r->dir.free_sector;
        r->other.dir_index = r->dir.free_index;
        result = fr_dir_write(&r->other, FR_DIR_CREATE, 0);
        if (result != FR_OK)
            return next_none(r, result);
        r->dir.tmp_sector = r->other.dir_sector;
        r->dir.tmp_index = r->other.dir_index;
        r->dir.tmp_first = r->other.first_cluster;
        r->dir.tmp_size = r->other.size;
        r->made++;
        fr_commit_begin(&r->commit, 0u, 0u, r->other.extent_count, FR_EOC, FR_LINK);
        r->next = RF_NEXT_COMMIT;
        return FR_BUSY;
    case RF_NEXT_COMMIT:
        result = fr_commit_step(&r->vol, &r->commit, &r->other);
        if (result == FR_BUSY)
            return FR_BUSY;
        if (result != FR_OK)
            return next_none(r, result);
        r->next = RF_NEXT_FSINFO;
        return FR_BUSY;
    case RF_NEXT_FSINFO:
        result = fr_fsinfo_adjust(&r->vol, (s32)r->other.clusters, r->vol.scan_from);
        if (result != FR_OK)
            return next_none(r, result);
        r->next = RF_NEXT_READY;
        return FR_OK;
    case RF_NEXT_READY:
        return FR_OK;
    default:
        return r->last_error != 0 ? r->last_error : FR_E_NO_SPACE;
    }
}


int rf_switch(struct rf *r)
{
    u8 *b = fr_dirbuf();
    int result;

    if (r->phase != RF_READY || r->next != RF_NEXT_READY)
        return FR_E_CHANGED;
    result = fr_dir_check(&r->other);
    if (result != FR_OK)
        return result;
    if (fr_read(r->other.extents[0].first_sector, b, 1u) != 0)
        return FR_E_IO;
    if (!equal_bytes(b, rf_idle_header(r), 512u))
        return FR_E_CHANGED;
    swap_files(r);
    r->named = 0u;
    r->next = RF_NEXT_OFF;
    r->switched++;
    return FR_OK;
}


void rf_begin_finish(struct rf *r, u32 file_bytes)
{
    r->keep_bytes = file_bytes;
    r->recovering = 0u;
    r->phase = RF_CUT_DIR;
}


int rf_revalidate(struct rf *r)
{
    u8 *b = fr_dirbuf();
    int result;

    r->refused = RF_REFUSED_READ;
    if (r->phase != RF_READY)
        return FR_E_CHANGED;
    if (fr_read(0u, b, 1u) != 0)
        return FR_E_IO;
    r->refused = RF_REFUSED_STICK;
    if (b[510] != 0x55u || b[13] != r->vol.spc ||
        ((u32)b[14] | ((u32)b[15] << 8)) != r->vol.fat_start ||
        ((u32)b[67] | ((u32)b[68] << 8) | ((u32)b[69] << 16) | ((u32)b[70] << 24)) != r->vol.serial)
        return FR_E_CHANGED;
    r->refused = RF_REFUSED_FILE;
    result = fr_dir_check(&r->cur);
    if (result != FR_OK) {
        if (result != FR_E_CHANGED)
            r->refused = RF_REFUSED_READ;
        return result;
    }
    if (fr_read(r->cur.extents[0].first_sector, b, 1u) != 0) {
        r->refused = RF_REFUSED_READ;
        return FR_E_IO;
    }
    if (!equal_bytes(b, rf_idle_header(r), 512u))
        return FR_E_CHANGED;
    r->refused = 0u;
    return FR_OK;
}


int rf_commit_name(struct rf *r)
{
    u8 old[11];
    int result;

    if (r->named)
        return FR_OK;
    if (r->next_number > 9999u)
        return FR_E_DIR;
    copy_bytes(old, r->cur.name, 11u);
    fr_number_name(r->cur.name, r->next_number);
    r->cur.attr = (u8)FR_ATTR_ARCHIVE;
    result = fr_dir_write(&r->cur, 0, old);
    if (result != FR_OK) {
        copy_bytes(r->cur.name, old, 11u);
        r->cur.attr = (u8)(FR_ATTR_HIDDEN | FR_ATTR_ARCHIVE);
        return result;
    }
    r->named = 1u;
    r->next_number++;
    return FR_OK;
}


static u32 closed_bytes(const u8 *header, u32 kind, u32 data_bytes, u32 limit)
{
    u32 audio_end = 512u + data_bytes;
    u32 total = rf_header_file_bytes(header);

    if (kind == RF_HEADER_COMPLETE && total > audio_end && total - audio_end <= RF_TAIL_MAX &&
        total <= limit)
        return total;
    return audio_end;
}

int rf_step(struct rf *r)
{
    u8 *b = fr_dirbuf();
    struct fr_file *f;
    u32 kind, data_bytes, keep, e, cb;
    int result;

    switch (r->phase) {
    case RF_MOUNT:
        result = fr_mount(&r->vol);
        if (result != FR_OK)
            return fail(r, result);
        fr_dirscan_begin(&r->vol, &r->dir);
        r->phase = RF_DIR;
        return FR_BUSY;

    case RF_DIR:
        result = fr_dirscan_step(&r->vol, &r->dir);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(r, result);
        r->next_number = r->dir.rec_found ? r->dir.max_number + 1u : 1u;
        r->tmp_pending = r->dir.tmp_found;
        if (r->dir.rec_found && r->dir.rec_first >= 2u && r->dir.rec_first < r->vol.end_cluster) {
            r->phase = RF_RECOVER_HEAD;
            return FR_BUSY;
        }
        return r->tmp_pending ? take_up_waiting(r) : begin_space(r, 0u);


    case RF_RECOVER_HEAD:
        if (fr_read(fr_cluster_sector(&r->vol, r->dir.rec_first), b, 1u) != 0)
            return fail(r, FR_E_IO);
        kind = rf_header_kind(b, &data_bytes);


        r->reusing = !r->tmp_pending &&
                     (kind == RF_HEADER_IDLE || (kind == RF_HEADER_RUNNING && data_bytes == 0u));
        if (kind == RF_HEADER_NONE || (kind == RF_HEADER_IDLE && !r->reusing) ||
            data_bytes > 0xFFFFFDFFu || 512u + data_bytes > r->dir.rec_size)
            return r->tmp_pending ? take_up_waiting(r) : begin_space(r, 0u);
        r->header_running = kind == RF_HEADER_RUNNING;
        r->cur.dir_sector = r->dir.rec_sector;
        r->cur.dir_index = r->dir.rec_index;
        r->cur.first_cluster = r->dir.rec_first;
        r->cur.size = r->dir.rec_size;
        fr_number_name(r->cur.name, r->dir.max_number);
        r->cur.attr = (u8)FR_ATTR_ARCHIVE;
        r->keep_bytes = closed_bytes(b, kind, data_bytes, r->dir.rec_size);
        fr_map_begin(&r->cur);
        r->cursor = 0u;
        r->phase = RF_RECOVER_MAP;
        return FR_BUSY;

    case RF_RECOVER_MAP:
        result = fr_map_step(&r->vol, &r->cur, &r->cursor, 0u);
        if (result == FR_BUSY)
            return FR_BUSY;
        if (result == FR_E_OPEN && r->cur.clusters != 0u) {


            result = fr_fat_set(&r->vol, last_cluster(r, &r->cur), r->cursor, FR_EOC);
        } else if (result == FR_E_CHAIN || result == FR_E_OPEN) {
            return r->tmp_pending ? take_up_waiting(r) : begin_space(r, 0u);
        }
        if (result != FR_OK)
            return fail(r, result);
        cb = cluster_bytes(r);
        if (r->reusing && r->cur.size == r->cur.clusters * cb) {


            u8 old[11];
            result = write_checked(r->cur.extents[0].first_sector, rf_idle_header(r));
            if (result != FR_OK)
                return fail(r, result);
            copy_bytes(old, r->cur.name, 11u);
            set_waiting(&r->cur, cb);
            result = fr_dir_write(&r->cur, 0, old);
            if (result != FR_OK)
                return fail(r, result);
            r->next_number = r->dir.max_number;
            r->reused++;
            return waiting_ready(r);
        }
        if (r->reusing && !r->header_running)
            return begin_space(r, 0u);
        keep = (r->keep_bytes + cb - 1u) / cb;
        if (r->cur.clusters == keep) {

            result = fr_fat_sync(&r->vol, last_cluster(r, &r->cur));
            if (result != FR_OK)
                return fail(r, result);
        }
        if (r->cur.clusters < keep ||
            (r->cur.clusters == keep && r->cur.size == r->keep_bytes && !r->header_running))
            return r->tmp_pending ? take_up_waiting(r) : begin_space(r, 0u);
        r->recovering = 1u;
        r->named = 1u;
        r->recovered++;
        if (r->header_running)
            r->repaired = r->dir.max_number;
        r->phase = r->header_running ? RF_RECOVER_CUT : RF_CUT_DIR;
        return FR_BUSY;

    case RF_RECOVER_CUT:
        if (fr_read(r->cur.extents[0].first_sector, b, 1u) != 0)
            return fail(r, FR_E_IO);
        if (rf_header_kind(b, &data_bytes) != RF_HEADER_RUNNING)
            return fail(r, FR_E_CHANGED);
        rf_close_header(b);


        r->keep_bytes += rf_recover_tail(r, b);
        if (fr_write(r->cur.extents[0].first_sector, b, 1u) != 0)
            return fail(r, FR_E_IO);
        r->phase = RF_CUT_DIR;
        return FR_BUSY;


    case RF_TMP_MAP:
        result = fr_map_step(&r->vol, &r->cur, &r->cursor, 0u);
        if (result == FR_E_OPEN) {


            if (r->cur.clusters == 0u) {
                result = fr_dir_write(&r->cur, FR_DIR_DELETE, 0);
                if (result != FR_OK)
                    return fail(r, result);
                r->dir.have_free = 1u;
                r->dir.free_sector = r->cur.dir_sector;
                r->dir.free_index = r->cur.dir_index;
                return begin_space(r, 0u);
            }
            e = last_cluster(r, &r->cur);
            result = fr_fat_set(&r->vol, e, r->cursor, FR_EOC);
            if (result == FR_OK)
                result = fr_fat_sync(&r->vol, e);
            if (result != FR_OK)
                return fail(r, result);
            set_waiting(&r->cur, cluster_bytes(r));
            result = fr_dir_write(&r->cur, FR_DIR_UPDATE, 0);
            if (result != FR_OK)
                return fail(r, result);
            r->phase = RF_TMP_HEAD;
            return FR_BUSY;
        }
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(r, result);


        result = fr_fat_sync(&r->vol, last_cluster(r, &r->cur));
        if (result != FR_OK)
            return fail(r, result);
        if (r->cur.size > r->cur.clusters * cluster_bytes(r))
            return fail(r, FR_E_CHAIN);
        r->phase = RF_TMP_HEAD;
        return FR_BUSY;

    case RF_TMP_HEAD:
        if (fr_read(r->cur.extents[0].first_sector, b, 1u) != 0)
            return fail(r, FR_E_IO);
        kind = rf_header_kind(b, &data_bytes);
        if (kind == RF_HEADER_NONE)
            return fail(r, FR_E_CHANGED);
        if (kind == RF_HEADER_COMPLETE && data_bytes != 0u &&
            512u + data_bytes <= r->cur.clusters * cluster_bytes(r)) {


            r->keep_bytes = closed_bytes(b, kind, data_bytes, r->cur.clusters * cluster_bytes(r));
            r->named = 0u;
            r->recovered++;
            r->phase = RF_CUT_DIR;
            return FR_BUSY;
        }
        if (!equal_bytes(b, rf_idle_header(r), 512u)) {
            result = write_checked(r->cur.extents[0].first_sector, rf_idle_header(r));
            if (result != FR_OK)
                return fail(r, result);
        }
        cb = cluster_bytes(r);
        if (r->cur.size != r->cur.clusters * cb) {


            u32 old = r->cur.size / cb;
            if (r->cur.size % cb != 0u || old == 0u)
                return fail(r, FR_E_CHAIN);
            result = fr_fat_set(&r->vol, chain_cluster(r, &r->cur, old - 1u),
                                chain_cluster(r, &r->cur, old), chain_cluster(r, &r->cur, old));
            if (result != FR_OK)
                return fail(r, result);
            set_waiting(&r->cur, cb);
            result = fr_dir_write(&r->cur, 0, 0);
            if (result != FR_OK)
                return fail(r, result);
        }


        result = fr_dir_check(&r->cur);
        if (result == FR_E_CHANGED) {
            result = fr_dir_write(&r->cur, FR_DIR_UPDATE, 0);
            r->entry_fixed = 1u;
        }
        if (result != FR_OK)
            return fail(r, result);
        r->reused++;
        return waiting_ready(r);

    case RF_SCAN:
        f = r->adding ? &r->other : &r->cur;
        result = fr_scan_step(&r->vol, &r->scan, f);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(r, result);
        cb = cluster_bytes(r);
        drop_tail(f, reserve_clusters(r));
        if (r->adding) {
            if (f->clusters == 0u) {
                r->phase = RF_READY;
                return FR_OK;
            }
            r->phase = RF_LINK;
            return FR_BUSY;
        }
        if (f->clusters < r->min_bytes / cb || f->clusters == 0u)
            return fail(r, FR_E_NO_SPACE);
        if (!r->dir.have_free)
            return fail(r, FR_E_DIR);
        r->phase = RF_NEW_HEAD;
        return FR_BUSY;

    case RF_NEW_HEAD:
        result = write_checked(r->cur.extents[0].first_sector, rf_idle_header(r));
        if (result != FR_OK)
            return fail(r, result);
        r->phase = RF_NEW_DIR;
        return FR_BUSY;


    case RF_NEW_DIR:
        r->named = 0u;
        set_waiting(&r->cur, cluster_bytes(r));
        r->cur.dir_sector = r->dir.free_sector;
        r->cur.dir_index = r->dir.free_index;
        result = fr_dir_write(&r->cur, FR_DIR_CREATE, 0);
        if (result != FR_OK)
            return fail(r, result);
        r->made++;
        fr_commit_begin(&r->commit, 0u, 0u, r->cur.extent_count, FR_EOC, FR_LINK);
        r->phase = RF_COMMIT;
        return FR_BUSY;

    case RF_LINK:
        result = fr_fat_set(&r->vol, last_cluster(r, &r->cur), FR_EOC, r->other.first_cluster);
        if (result != FR_OK)
            return fail(r, result);
        fr_commit_begin(&r->commit, 0u, 0u, r->other.extent_count, FR_EOC, FR_LINK);
        r->phase = RF_COMMIT;
        return FR_BUSY;

    case RF_COMMIT:
        result = fr_commit_step(&r->vol, &r->commit, r->adding ? &r->other : &r->cur);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(r, result);
        r->phase = r->adding ? RF_ADDED : RF_FSINFO;
        return FR_BUSY;

    case RF_ADDED:
        for (e = 0u; e < r->other.extent_count; e++) {
            struct extent *tail = &r->cur.extents[r->cur.extent_count - 1u];
            if (r->other.extents[e].first_sector ==
                tail->first_sector + tail->clusters * r->vol.spc)
                tail->clusters += r->other.extents[e].clusters;
            else
                r->cur.extents[r->cur.extent_count++] = r->other.extents[e];
        }
        r->cur.clusters += r->other.clusters;
        r->cur.size = r->cur.clusters * cluster_bytes(r);
        result = fr_dir_write(&r->cur, 0, 0);
        if (result != FR_OK)
            return fail(r, result);
        r->other.max_extents = r->cur.max_extents;
        r->refilled++;
        r->phase = RF_FSINFO;
        return FR_BUSY;

    case RF_FSINFO:
        result = fr_fsinfo_adjust(&r->vol, (s32)(r->adding ? r->other.clusters : r->cur.clusters),
                                  r->vol.scan_from);
        if (result != FR_OK)
            return fail(r, result);
        r->phase = RF_READY;
        return FR_OK;


    case RF_CUT_DIR:
        cb = cluster_bytes(r);
        keep = (r->keep_bytes + cb - 1u) / cb;
        if (r->next == RF_NEXT_READY || r->tmp_pending) {

            r->cut_last = fr_cut_position(&r->vol, &r->cur, keep, &r->cut_extent,
                                          &r->cut_offset, &r->cut_next);
        } else {
            r->other.max_extents = r->cur.max_extents;
            r->cut_last = fr_split(&r->vol, &r->cur, &r->other, keep);
        }
        {
            u8 old[11];
            copy_bytes(old, r->cur.name, 11u);
            if (!r->named) {
                if (r->next_number > 9999u)
                    return fail(r, FR_E_DIR);
                fr_number_name(r->cur.name, r->next_number++);
                r->cur.attr = (u8)FR_ATTR_ARCHIVE;
            }
            r->cur.size = r->keep_bytes;
            result = fr_dir_write(&r->cur, 0, old);
            if (result != FR_OK)
                return fail(r, result);
        }
        r->cuts++;
        if (r->next == RF_NEXT_READY || r->tmp_pending) {
            r->phase = RF_FREE_INFO;
            if (r->cut_last != 0u) {


                r->phase = r->tmp_pending && r->cut_next == r->dir.tmp_first ? RF_FREE_FAT : RF_FREE_TAIL;
                fr_commit_begin(&r->commit, r->cur.extent_count - 1u,
                                r->cur.extents[r->cur.extent_count - 1u].clusters,
                                r->cut_extent, r->cut_offset, 0u);
            }
            return FR_BUSY;
        }
        fr_dirscan_begin(&r->vol, &r->dir);
        r->phase = RF_TAIL_SLOT;
        return FR_BUSY;


    case RF_FREE_TAIL:
        result = fr_release_step(&r->vol, &r->commit, &r->cur);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(r, result);
        cb = cluster_bytes(r);
        result = fr_fsinfo_adjust(&r->vol, -(s32)(r->cur.clusters - (r->keep_bytes + cb - 1u) / cb),
                                  r->vol.scan_from);
        if (result != FR_OK)
            return fail(r, result);
        r->phase = RF_FREE_FAT;
        return FR_BUSY;

    case RF_FREE_FAT:
        result = fr_fat_set(&r->vol, r->cut_last, r->cut_next, FR_EOC);
        if (result != FR_OK)
            return fail(r, result);
        r->phase = RF_FREE_INFO;
        return FR_BUSY;

    case RF_FREE_INFO:
        if (r->next == RF_NEXT_READY) {
            swap_files(r);
            r->next = RF_NEXT_OFF;
            return waiting_ready(r);
        }
        return take_up_waiting(r);

    case RF_TAIL_SLOT:
        result = fr_dirscan_step(&r->vol, &r->dir);
        if (result != FR_OK)
            return result == FR_BUSY ? FR_BUSY : fail(r, result);
        if (r->dir.rec_found && r->dir.max_number + 1u > r->next_number)
            r->next_number = r->dir.max_number + 1u;
        if (r->cut_last == 0u)
            return begin_space(r, 0u);
        if (!r->dir.have_free)
            return fail(r, FR_E_DIR);
        r->phase = RF_TAIL_HEAD;
        return FR_BUSY;

    case RF_TAIL_HEAD:
        result = write_checked(r->other.extents[0].first_sector, rf_idle_header(r));
        if (result != FR_OK)
            return fail(r, result);
        r->phase = RF_TAIL_DIR;
        return FR_BUSY;

    case RF_TAIL_DIR:
        set_waiting(&r->other, cluster_bytes(r));
        r->other.dir_sector = r->dir.free_sector;
        r->other.dir_index = r->dir.free_index;
        result = fr_dir_write(&r->other, 1, 0);
        if (result != FR_OK)
            return fail(r, result);
        r->phase = RF_CUT_FAT;
        return FR_BUSY;

    case RF_CUT_FAT:
        result = fr_fat_set(&r->vol, r->cut_last, r->other.first_cluster, FR_EOC);
        if (result != FR_OK)
            return fail(r, result);
        swap_files(r);
        return waiting_ready(r);

    case RF_READY:
        return FR_OK;
    default:
        return r->last_error != 0 ? r->last_error : FR_E_CHANGED;
    }
}
