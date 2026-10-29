/* FAT32: the file system calls of the recorder.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "fat.h"

#define ENTRIES_PER_SECTOR 128u
#define DIR_ENTRIES         16u
#define MAX_DIR_CLUSTERS  8192u

#define STAMP_DATE 0x5C21u
#define STAMP_TIME 0x0000u

static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static int same(const u8 *a, const u8 *b, u32 count)
{
    while (count-- != 0u) {
        if (*a++ != *b++)
            return 0;
    }
    return 1;
}

u32 fr_cluster_sector(const struct fr_vol *v, u32 cluster)
{
    return v->data_start + (cluster - 2u) * v->spc;
}

u32 fr_sector_cluster(const struct fr_vol *v, u32 sector)
{
    return (sector - v->data_start) / v->spc + 2u;
}

void fr_name(u8 *name, const char *text)
{
    u32 i;
    for (i = 0u; i < 11u; i++)
        name[i] = (u8)text[i];
}


void fr_number_name(u8 *name, u32 number)
{
    fr_name(name, "REC0000 WAV");
    name[3] = (u8)('0' + number / 1000u % 10u);
    name[4] = (u8)('0' + number / 100u % 10u);
    name[5] = (u8)('0' + number / 10u % 10u);
    name[6] = (u8)('0' + number % 10u);
}


volatile u32 fr_free_clusters = FR_FREE_UNKNOWN;


int fr_mount(struct fr_vol *v)
{
    u8 *b = fr_dirbuf();
    u32 spc, reserved, fats, fat_sectors, total, clusters, flags, info;

    fr_free_clusters = FR_FREE_UNKNOWN;
    if (fr_read(0u, b, 1u) != 0)
        return FR_E_IO;
    spc = b[13];
    reserved = le16(&b[14]);
    fats = b[16];
    fat_sectors = le32(&b[36]);
    total = le16(&b[19]) != 0u ? le16(&b[19]) : le32(&b[32]);
    flags = le16(&b[40]);
    if (b[510] != 0x55u || b[511] != 0xAAu || le16(&b[11]) != 512u ||
        spc == 0u || spc > 128u || (spc & (spc - 1u)) != 0u ||
        reserved == 0u || fats == 0u || fats > 2u ||
        le16(&b[17]) != 0u ||
        le16(&b[22]) != 0u ||
        fat_sectors == 0u || fat_sectors > 0x00200000u ||
        le16(&b[42]) != 0u ||
        (flags & 0x0080u) != 0u)
        return FR_E_NOT_FAT32;
    v->spc = spc;
    v->fat_start = reserved;
    v->fat_sectors = fat_sectors;
    v->fat_count = fats;
    v->data_start = reserved + fats * fat_sectors;
    if (total <= v->data_start)
        return FR_E_NOT_FAT32;
    clusters = (total - v->data_start) / spc;
    if (clusters < 65525u || clusters + 2u > fat_sectors * ENTRIES_PER_SECTOR)
        return FR_E_NOT_FAT32;
    v->end_cluster = clusters + 2u;
    v->root_cluster = le32(&b[44]);
    if (v->root_cluster < 2u || v->root_cluster >= v->end_cluster)
        return FR_E_NOT_FAT32;
    v->serial = le32(&b[67]);
    info = le16(&b[48]);
    v->fsinfo_sector = (info != 0u && info < reserved) ? info : 0u;
    v->scan_from = 2u;
    v->cached_fat_sector = 0u;
    if (v->fsinfo_sector != 0u) {
        if (fr_read(v->fsinfo_sector, b, 1u) != 0)
            return FR_E_IO;
        if (le32(&b[0]) == 0x41615252u && le32(&b[484]) == 0x61417272u &&
            le32(&b[508]) == 0xAA550000u) {
            u32 hint = le32(&b[492]), count = le32(&b[488]);
            if (hint >= 2u && hint < v->end_cluster)
                v->scan_from = hint;
            if (count <= clusters)
                fr_free_clusters = count;
        } else {
            v->fsinfo_sector = 0u;
        }
    }
    return FR_OK;
}


static int fat_get(struct fr_vol *v, u32 cluster, u32 *value)
{
    u32 sector = v->fat_start + cluster / ENTRIES_PER_SECTOR;

    if (sector != v->cached_fat_sector) {
        v->cached_fat_sector = 0u;
        if (fr_read(sector, fr_fatbuf(), 1u) != 0)
            return FR_E_IO;
        v->cached_fat_sector = sector;
    }
    *value = le32(&fr_fatbuf()[(cluster % ENTRIES_PER_SECTOR) * 4u]) & 0x0FFFFFFFu;
    return FR_OK;
}


void fr_dirscan_begin(const struct fr_vol *v, struct fr_dirscan *d)
{
    u32 *w = (u32 *)d;
    u32 i;
    for (i = 0u; i < sizeof(*d) / 4u; i++)
        w[i] = 0u;
    d->cluster = v->root_cluster;
}

static int is_rec_name(const u8 *e, u32 *number)
{
    u32 i, n = 0u;
    if (e[0] != 'R' || e[1] != 'E' || e[2] != 'C' || e[7] != ' ' ||
        e[8] != 'W' || e[9] != 'A' || e[10] != 'V')
        return 0;
    for (i = 3u; i < 7u; i++) {
        if (e[i] < '0' || e[i] > '9')
            return 0;
        n = n * 10u + (u32)(e[i] - '0');
    }
    *number = n;
    return 1;
}


void fr_log_name(u8 *name, u32 index)
{
    fr_name(name, "REC-LOG TXT");
    if (index >= 10u) {
        name[5] = 'G';
        name[6] = (u8)('0' + index / 10u);
        name[7] = (u8)('0' + index % 10u);
    } else if (index >= 2u) {
        name[7] = (u8)('0' + index);
    }
}

static u32 log_name_index(const u8 *e)
{
    u8 name[11];
    u32 index;

    if (e[0] != 'R' || e[1] != 'E' || e[2] != 'C' || e[3] != '-' || e[4] != 'L')
        return 0u;
    for (index = 1u; index <= 31u; index++) {
        fr_log_name(name, index);
        if (same(e, name, 11u))
            return index;
    }
    return 0u;
}


int fr_dirscan_step(struct fr_vol *v, struct fr_dirscan *d)
{
    u8 *b = fr_dirbuf();
    u32 sector = fr_cluster_sector(v, d->cluster) + d->sector_in_cluster;
    u32 index, number, next;
    int result;

    if (fr_read(sector, b, 1u) != 0)
        return FR_E_IO;
    for (index = 0u; index < DIR_ENTRIES; index++) {
        const u8 *e = &b[index * 32u];
        u32 first = (le16(&e[20]) << 16) | le16(&e[26]);
        if (e[0] == 0x00u || e[0] == 0xE5u) {
            if (!d->have_free) {
                d->have_free = 1u;
                d->free_sector = sector;
                d->free_index = index;
            }
            if (e[0] == 0x00u)
                return FR_OK;
            continue;
        }
        if ((e[11] & 0x0Fu) == 0x0Fu || (e[11] & 0x18u) != 0u)
            continue;
        if (same(e, (const u8 *)"DJMREC  TMP", 11u)) {
            d->tmp_found = 1u;
            d->tmp_sector = sector;
            d->tmp_index = index;
            d->tmp_first = first;
            d->tmp_size = le32(&e[28]);
        } else if (same(e, (const u8 *)"DJMLOG  BIN", 11u)) {
            if (le32(&e[28]) >= 512u && first >= 2u && first < v->end_cluster) {
                d->log_first = first;
                d->log_sectors = le32(&e[28]) / 512u;
            }
        } else if (is_rec_name(e, &number)) {
            if (number >= d->max_number) {
                d->max_number = number;
                d->rec_found = 1u;
                d->rec_sector = sector;
                d->rec_index = index;
                d->rec_first = first;
                d->rec_size = le32(&e[28]);
            }
        } else if ((number = log_name_index(e)) != 0u) {
            d->txt_mask |= 1u << number;
            if (number >= d->txt_want && (d->txt_index == 0u || number < d->txt_index)) {
                d->txt_index = number;
                d->txt_sector = sector;
                d->txt_slot = index;
                d->txt_first = first;
                d->txt_size = le32(&e[28]);
                d->txt_attr = e[11];
            }
        }
    }
    if (++d->sector_in_cluster < v->spc)
        return FR_BUSY;
    result = fat_get(v, d->cluster, &next);
    if (result != FR_OK)
        return result;
    if (next >= 0x0FFFFFF8u)
        return FR_OK;
    if (next < 2u || next >= v->end_cluster || ++d->clusters_seen > MAX_DIR_CLUSTERS)
        return FR_E_DIR;
    d->cluster = next;
    d->sector_in_cluster = 0u;
    return FR_BUSY;
}

static void build_entry(u8 *e, const struct fr_file *f)
{
    u32 i;
    for (i = 0u; i < 32u; i++)
        e[i] = 0u;
    for (i = 0u; i < 11u; i++)
        e[i] = f->name[i];
    e[11] = f->attr;
    put16(&e[14], STAMP_TIME);
    put16(&e[16], STAMP_DATE);
    put16(&e[18], STAMP_DATE);
    put16(&e[20], f->first_cluster >> 16);
    put16(&e[22], STAMP_TIME);
    put16(&e[24], STAMP_DATE);
    put16(&e[26], f->first_cluster & 0xFFFFu);
    put32(&e[28], f->size);
}


int fr_dir_write(const struct fr_file *f, int create, const u8 *old_name)
{
    u8 *b = fr_dirbuf();
    u8 *e = &b[f->dir_index * 32u];
    u8 entry[32];

    if (f->dir_index >= DIR_ENTRIES || fr_read(f->dir_sector, b, 1u) != 0)
        return f->dir_index >= DIR_ENTRIES ? FR_E_CHANGED : FR_E_IO;
    if (create == FR_DIR_CREATE) {
        if (e[0] != 0x00u && e[0] != 0xE5u)
            return FR_E_CHANGED;
    } else if (!same(e, old_name != 0 ? old_name : f->name, 11u) ||
               ((le16(&e[20]) << 16) | le16(&e[26])) != f->first_cluster) {
        return FR_E_CHANGED;
    }
    build_entry(entry, f);
    if (create == FR_DIR_DELETE)
        entry[0] = 0xE5u;
    {
        u32 i;
        for (i = 0u; i < 32u; i++)
            e[i] = entry[i];
    }
    if (fr_write(f->dir_sector, b, 1u) != 0 || fr_read(f->dir_sector, b, 1u) != 0)
        return FR_E_IO;
    return same(e, entry, 32u) ? FR_OK : FR_E_VERIFY;
}


int fr_dir_check(const struct fr_file *f)
{
    u8 *b = fr_dirbuf();
    u8 entry[32];

    if (f->dir_index >= DIR_ENTRIES)
        return FR_E_CHANGED;
    if (fr_read(f->dir_sector, b, 1u) != 0)
        return FR_E_IO;
    build_entry(entry, f);
    return same(&b[f->dir_index * 32u], entry, 32u) ? FR_OK : FR_E_CHANGED;
}


static int add_run(const struct fr_vol *v, struct fr_file *f, u32 first, u32 length)
{
    u32 sector = fr_cluster_sector(v, first);
    struct extent *last = f->extent_count ? &f->extents[f->extent_count - 1u] : 0;

    if (last != 0 && sector == last->first_sector + last->clusters * v->spc) {
        last->clusters += length;
    } else {
        if (f->extent_count >= f->max_extents)
            return 0;
        f->extents[f->extent_count].first_sector = sector;
        f->extents[f->extent_count].clusters = length;
        f->extent_count++;
    }
    if (f->clusters == 0u)
        f->first_cluster = first;
    f->clusters += length;
    return 1;
}

void fr_map_begin(struct fr_file *f)
{
    f->extent_count = 0u;
    f->clusters = 0u;
}


int fr_map_step(struct fr_vol *v, struct fr_file *f, u32 *cursor, u32 expect_clusters)
{
    u32 budget = 256u;
    u32 cluster = *cursor != 0u ? *cursor : f->first_cluster;
    u32 reads = 0u;
    u32 next;
    int result;

    if (cluster < 2u || cluster >= v->end_cluster)
        return FR_E_CHAIN;
    while (budget-- != 0u) {
        if (v->fat_start + cluster / ENTRIES_PER_SECTOR != v->cached_fat_sector) {
            if (reads != 0u) {
                *cursor = cluster;
                return FR_BUSY;
            }
            reads++;
        }
        result = fat_get(v, cluster, &next);
        if (result != FR_OK)
            return result;
        if (next == 0u) {
            *cursor = cluster;
            return FR_E_OPEN;
        }
        if (!add_run(v, f, cluster, 1u))
            return FR_E_CHAIN;
        if (next >= 0x0FFFFFF8u)
            return expect_clusters == 0u || f->clusters == expect_clusters ? FR_OK : FR_E_CHAIN;
        if (next < 2u || next >= v->end_cluster || f->clusters >= v->end_cluster ||
            (expect_clusters != 0u && f->clusters >= expect_clusters))
            return FR_E_CHAIN;
        cluster = next;
    }
    *cursor = cluster;
    return FR_BUSY;
}


void fr_scan_begin(const struct fr_vol *v, struct fr_scan *s, u32 target, u32 min_run)
{
    s->cluster = (v->scan_from >= 2u && v->scan_from < v->end_cluster) ? v->scan_from : 2u;
    s->remaining = v->end_cluster - 2u;
    s->target = target;
    s->found = 0u;
    s->min_run = min_run ? min_run : 1u;
    s->run_first = 0u;
    s->run_length = 0u;
}


static int close_run(const struct fr_vol *v, struct fr_scan *s, struct fr_file *f)
{
    int ok = 1;
    if (s->run_length >= s->min_run || (s->run_length != 0u && s->found + s->run_length >= s->target)) {
        ok = add_run(v, f, s->run_first, s->run_length);
        if (ok)
            s->found += s->run_length;
    }
    s->run_length = 0u;
    return ok;
}


int fr_scan_step(struct fr_vol *v, struct fr_scan *s, struct fr_file *f)
{
    u8 *b = fr_fatbuf();
    u32 sector, count, cluster, end;

    if (s->found + s->run_length < s->target && s->remaining != 0u) {
        if (s->cluster >= v->end_cluster) {
            if (!close_run(v, s, f))
                s->remaining = 0u;
            s->cluster = 2u;
        }
    }
    if (s->found + s->run_length >= s->target || s->remaining == 0u) {
        close_run(v, s, f);
        v->scan_from = s->cluster < v->end_cluster ? s->cluster : 2u;
        return FR_OK;
    }
    sector = v->fat_start + s->cluster / ENTRIES_PER_SECTOR;
    count = v->fat_start + v->fat_sectors - sector;
    if (count > FR_FAT_BATCH)
        count = FR_FAT_BATCH;
    v->cached_fat_sector = 0u;
    if (fr_read(sector, b, count) != 0)
        return FR_E_IO;
    end = (sector - v->fat_start + count) * ENTRIES_PER_SECTOR;
    if (end > v->end_cluster)
        end = v->end_cluster;
    for (cluster = s->cluster; cluster < end && s->remaining != 0u; cluster++, s->remaining--) {
        u32 value = le32(&b[(cluster - (sector - v->fat_start) * ENTRIES_PER_SECTOR) * 4u]) &
                    0x0FFFFFFFu;
        if (value == 0u) {
            if (s->run_length == 0u)
                s->run_first = cluster;
            s->run_length++;
            if (s->found + s->run_length >= s->target) {
                cluster++;
                s->remaining--;
                break;
            }
        } else if (s->run_length != 0u && !close_run(v, s, f)) {
            s->remaining = 0u;
            break;
        }
    }
    s->cluster = cluster;
    return FR_BUSY;
}


static int write_tables(const struct fr_vol *v, u32 sector, const u8 *b, u32 count)
{
    u32 copy;
    for (copy = 0u; copy < v->fat_count; copy++) {
        if (fr_write(sector + copy * v->fat_sectors, b, count) != 0)
            return FR_E_IO;
    }
    return FR_OK;
}

void fr_commit_begin(struct fr_commit *c, u32 first_extent, u32 offset, u32 last_extent,
                     u32 end_value, u32 mode)
{
    c->extent = first_extent;
    c->offset = offset;
    c->last_extent = last_extent;
    c->end_value = end_value;
    c->mode = mode;
}


static int commit_window(const struct fr_vol *v, struct fr_commit *c, const struct fr_file *f,
                         u8 *b, u32 base, u32 limit, int check)
{
    while (c->extent < c->last_extent) {
        const struct extent *e = &f->extents[c->extent];
        u32 first = fr_sector_cluster(v, e->first_sector);
        u32 end = first + e->clusters;
        u32 cluster = first + c->offset;
        u32 next_first = c->extent + 1u < c->last_extent
            ? fr_sector_cluster(v, f->extents[c->extent + 1u].first_sector) : c->end_value;

        if (cluster < base || cluster >= limit)
            break;
        for (; cluster < end && cluster < limit; cluster++) {
            u8 *p = &b[(cluster - base) * 4u];
            u32 value = le32(p);
            u32 link = cluster + 1u < end ? cluster + 1u : next_first;
            u32 held = value & 0x0FFFFFFFu;
            if (check) {
                if (held != link)
                    return FR_E_VERIFY;
            } else {
                if (held != 0u)
                    return FR_E_CHANGED;
                put32(p, (value & 0xF0000000u) | link);
            }
        }
        c->offset = cluster - first;
        if (cluster < end)
            break;
        c->extent++;
        c->offset = 0u;
    }
    return FR_OK;
}


int fr_commit_step(struct fr_vol *v, struct fr_commit *c, const struct fr_file *f)
{
    u8 *b = fr_fatbuf();
    u32 start_extent = c->extent, start_offset = c->offset;
    u32 done_extent, done_offset;
    u32 cluster, sector, count, base, limit, copy;
    int result;

    if (c->extent >= c->last_extent)
        return FR_OK;
    cluster = fr_sector_cluster(v, f->extents[c->extent].first_sector) + c->offset;
    sector = v->fat_start + cluster / ENTRIES_PER_SECTOR;
    count = v->fat_start + v->fat_sectors - sector;
    if (count > FR_FAT_BATCH)
        count = FR_FAT_BATCH;
    base = (sector - v->fat_start) * ENTRIES_PER_SECTOR;
    limit = base + count * ENTRIES_PER_SECTOR;

    v->cached_fat_sector = 0u;
    if (fr_read(sector, b, count) != 0)
        return FR_E_IO;
    result = commit_window(v, c, f, b, base, limit, 0);
    if (result != FR_OK) {
        c->extent = start_extent;
        c->offset = start_offset;
        return result;
    }
    done_extent = c->extent;
    done_offset = c->offset;
    if (write_tables(v, sector, b, count) != FR_OK)
        return FR_E_IO;
    for (copy = 0u; copy < v->fat_count; copy++) {
        if (fr_read(sector + copy * v->fat_sectors, b, count) != 0)
            return FR_E_IO;
        c->extent = start_extent;
        c->offset = start_offset;
        result = commit_window(v, c, f, b, base, limit, 1);
        if (result != FR_OK || c->extent != done_extent || c->offset != done_offset)
            return FR_E_VERIFY;
    }
    return c->extent >= c->last_extent ? FR_OK : FR_BUSY;
}

int fr_release_step(struct fr_vol *v, struct fr_commit *c, const struct fr_file *f)
{
    u8 *b = fr_fatbuf();
    u8 *back = fr_dirbuf();
    u32 sector = 0u, done = 0u, copy;

    v->cached_fat_sector = 0u;
    for (;;) {
        const struct extent *e = &f->extents[c->extent];
        u32 first = fr_sector_cluster(v, e->first_sector);
        u32 lower = c->extent == c->last_extent ? c->end_value : 0u;
        u32 cluster, link;
        u8 *p;

        if (c->offset == lower) {
            if (c->extent == c->last_extent)
                break;
            c->extent--;
            c->offset = f->extents[c->extent].clusters;
            continue;
        }
        cluster = first + c->offset - 1u;
        if (done == 0u) {
            sector = cluster / ENTRIES_PER_SECTOR;
            if (fr_read(v->fat_start + sector, b, 1u) != 0)
                return FR_E_IO;
        } else if (cluster / ENTRIES_PER_SECTOR != sector) {
            break;
        }
        p = &b[(cluster % ENTRIES_PER_SECTOR) * 4u];


        link = c->offset < e->clusters ? cluster + 1u
             : c->extent + 1u < f->extent_count
               ? fr_sector_cluster(v, f->extents[c->extent + 1u].first_sector) : 0u;
        if (link != 0u && (le32(p) & 0x0FFFFFFFu) != link)
            return FR_E_CHANGED;
        put32(p, le32(p) & 0xF0000000u);
        c->offset--;
        done++;
    }
    if (done == 0u)
        return FR_OK;
    if (write_tables(v, v->fat_start + sector, b, 1u) != FR_OK)
        return FR_E_IO;
    for (copy = 0u; copy < v->fat_count; copy++) {
        if (fr_read(v->fat_start + copy * v->fat_sectors + sector, back, 1u) != 0)
            return FR_E_IO;
        if (!same(back, b, 512u))
            return FR_E_VERIFY;
    }
    return FR_BUSY;
}


int fr_fat_set(struct fr_vol *v, u32 cluster, u32 expected, u32 value)
{
    u8 *b = fr_fatbuf();
    u32 sector = v->fat_start + cluster / ENTRIES_PER_SECTOR;
    u8 *p = &b[(cluster % ENTRIES_PER_SECTOR) * 4u];
    u32 old, copy;

    if (cluster < 2u || cluster >= v->end_cluster)
        return FR_E_CHAIN;
    v->cached_fat_sector = 0u;
    if (fr_read(sector, b, 1u) != 0)
        return FR_E_IO;
    old = le32(p);
    if ((old & 0x0FFFFFFFu) != expected &&
        !(expected >= 0x0FFFFFF8u && (old & 0x0FFFFFFFu) >= 0x0FFFFFF8u))
        return FR_E_CHANGED;
    put32(p, (old & 0xF0000000u) | value);
    if (write_tables(v, sector, b, 1u) != FR_OK)
        return FR_E_IO;
    for (copy = 0u; copy < v->fat_count; copy++) {
        if (fr_read(sector + copy * v->fat_sectors, b, 1u) != 0)
            return FR_E_IO;
        if ((le32(p) & 0x0FFFFFFFu) != value)
            return FR_E_VERIFY;
    }
    return FR_OK;
}

int fr_fat_sync(struct fr_vol *v, u32 cluster)
{
    u8 *b = fr_fatbuf();
    u8 *one = fr_dirbuf();
    u32 last = cluster / ENTRIES_PER_SECTOR;
    u32 first = last >= FR_FAT_BATCH - 1u ? last - (FR_FAT_BATCH - 1u) : 0u;
    u32 copy, i, sector;

    v->cached_fat_sector = 0u;
    if (fr_read(v->fat_start + first, b, last - first + 1u) != 0)
        return FR_E_IO;
    for (copy = 1u; copy < v->fat_count; copy++) {
        for (i = first; i <= last; i++) {
            sector = v->fat_start + copy * v->fat_sectors + i;
            if (fr_read(sector, one, 1u) != 0)
                return FR_E_IO;
            if (!same(one, &b[(i - first) * 512u], 512u) &&
                fr_write(sector, &b[(i - first) * 512u], 1u) != 0)
                return FR_E_IO;
        }
    }
    return FR_OK;
}


int fr_fsinfo_adjust(const struct fr_vol *v, s32 clusters_taken, u32 next_free)
{
    u8 *b = fr_dirbuf();
    u32 free_count;

    if (v->fsinfo_sector == 0u)
        return FR_OK;
    if (fr_read(v->fsinfo_sector, b, 1u) != 0)
        return FR_E_IO;
    if (le32(&b[0]) != 0x41615252u || le32(&b[484]) != 0x61417272u ||
        le32(&b[508]) != 0xAA550000u)
        return FR_OK;
    free_count = le32(&b[488]);
    if (free_count != 0xFFFFFFFFu) {
        if (clusters_taken > 0 && free_count < (u32)clusters_taken)
            free_count = 0u;
        else
            free_count -= (u32)clusters_taken;
        put32(&b[488], free_count);
    }
    fr_free_clusters = free_count <= v->end_cluster - 2u ? free_count : FR_FREE_UNKNOWN;
    if (next_free >= 2u && next_free < v->end_cluster)
        put32(&b[492], next_free);
    if (fr_write(v->fsinfo_sector, b, 1u) != 0)
        return FR_E_IO;
    return FR_OK;
}


u32 fr_split(const struct fr_vol *v, struct fr_file *f, struct fr_file *tail, u32 keep_clusters)
{
    u32 e, seen = 0u, last = 0u, count = 0u;

    tail->extent_count = 0u;
    tail->clusters = 0u;
    tail->first_cluster = 0u;
    if (keep_clusters == 0u || keep_clusters >= f->clusters)
        return 0u;
    for (e = 0u; e < f->extent_count; e++) {
        struct extent *x = &f->extents[e];
        if (seen + x->clusters <= keep_clusters) {
            seen += x->clusters;
            count = e + 1u;
            if (seen == keep_clusters)
                last = fr_sector_cluster(v, x->first_sector) + x->clusters - 1u;
            continue;
        }
        if (seen < keep_clusters) {
            u32 kept = keep_clusters - seen;
            last = fr_sector_cluster(v, x->first_sector) + kept - 1u;
            tail->extents[0].first_sector = x->first_sector + kept * v->spc;
            tail->extents[0].clusters = x->clusters - kept;
            tail->extent_count = 1u;
            seen += x->clusters;
            x->clusters = kept;
            count = e + 1u;
            continue;
        }
        tail->extents[tail->extent_count++] = *x;
    }
    tail->clusters = f->clusters - keep_clusters;
    tail->first_cluster = fr_sector_cluster(v, tail->extents[0].first_sector);
    f->clusters = keep_clusters;
    f->extent_count = count;
    return last;
}


u32 fr_cut_position(const struct fr_vol *v, const struct fr_file *f, u32 keep_clusters,
                    u32 *extent, u32 *offset, u32 *next_cluster)
{
    u32 e, seen = 0u;

    if (keep_clusters == 0u || keep_clusters >= f->clusters)
        return 0u;
    for (e = 0u; e < f->extent_count; e++) {
        const struct extent *x = &f->extents[e];
        u32 first = fr_sector_cluster(v, x->first_sector);
        if (seen + x->clusters > keep_clusters) {
            *extent = e;
            *offset = keep_clusters - seen;
            *next_cluster = first + *offset;
            return *offset != 0u ? *next_cluster - 1u
                : fr_sector_cluster(v, f->extents[e - 1u].first_sector) + f->extents[e - 1u].clusters - 1u;
        }
        seen += x->clusters;
    }
    return 0u;
}
