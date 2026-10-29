/* FAT32: the file system calls of the recorder.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef SELFMADE_FAT_H
#define SELFMADE_FAT_H


typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;

#define FR_OK             0
#define FR_BUSY           1
#define FR_E_IO          -1
#define FR_E_NOT_FAT32   -2
#define FR_E_DIR         -3
#define FR_E_NO_SPACE    -4
#define FR_E_CHAIN       -5
#define FR_E_CHANGED     -6
#define FR_E_VERIFY      -7
#define FR_E_OPEN        -8

#define FR_FAT_BATCH     16u
#define FR_EOC           0x0FFFFFFFu
#define FR_ATTR_HIDDEN   0x02u
#define FR_ATTR_ARCHIVE  0x20u

struct extent {
    u32 first_sector;
    u32 clusters;
};

struct fr_vol {
    u32 spc;
    u32 fat_start;
    u32 fat_sectors;
    u32 fat_count;
    u32 data_start;
    u32 end_cluster;
    u32 root_cluster;
    u32 fsinfo_sector;
    u32 serial;
    u32 scan_from;
    u32 cached_fat_sector;
};


struct fr_file {
    u32 dir_sector;
    u32 dir_index;
    u32 first_cluster;
    u32 clusters;
    u32 size;
    u32 extent_count;
    u32 max_extents;
    struct extent *extents;
    u8 name[11];
    u8 attr;
};


struct fr_dirscan {
    u32 cluster;
    u32 sector_in_cluster;
    u32 clusters_seen;
    u32 have_free;
    u32 free_sector;
    u32 free_index;
    u32 max_number;
    u32 tmp_found;
    u32 tmp_sector;
    u32 tmp_index;
    u32 tmp_first;
    u32 tmp_size;
    u32 rec_found;
    u32 rec_sector;
    u32 rec_index;
    u32 rec_first;
    u32 rec_size;
    u32 log_first;
    u32 log_sectors;


    u32 txt_want;
    u32 txt_mask;
    u32 txt_index;
    u32 txt_sector;
    u32 txt_slot;
    u32 txt_first;
    u32 txt_size;
    u32 txt_attr;
};


struct fr_scan {
    u32 cluster;
    u32 remaining;
    u32 target;
    u32 found;
    u32 min_run;
    u32 run_first;
    u32 run_length;
};


#define FR_LINK 0u
struct fr_commit {
    u32 extent;
    u32 offset;
    u32 last_extent;
    u32 end_value;
    u32 mode;
};


int fr_read(u32 sector, u8 *buffer, u32 count);
int fr_write(u32 sector, const u8 *buffer, u32 count);


extern u8 *fr_dirbuf(void);
extern u8 *fr_fatbuf(void);

u32 fr_cluster_sector(const struct fr_vol *v, u32 cluster);
u32 fr_sector_cluster(const struct fr_vol *v, u32 sector);

int fr_mount(struct fr_vol *v);
void fr_dirscan_begin(const struct fr_vol *v, struct fr_dirscan *d);
int fr_dirscan_step(struct fr_vol *v, struct fr_dirscan *d);

void fr_map_begin(struct fr_file *f);


int fr_map_step(struct fr_vol *v, struct fr_file *f, u32 *cursor, u32 expect_clusters);

void fr_scan_begin(const struct fr_vol *v, struct fr_scan *s, u32 target, u32 min_run);
int fr_scan_step(struct fr_vol *v, struct fr_scan *s, struct fr_file *f);

void fr_commit_begin(struct fr_commit *c, u32 first_extent, u32 offset, u32 last_extent,
                     u32 end_value, u32 mode);
int fr_commit_step(struct fr_vol *v, struct fr_commit *c, const struct fr_file *f);


int fr_release_step(struct fr_vol *v, struct fr_commit *c, const struct fr_file *f);
int fr_fat_set(struct fr_vol *v, u32 cluster, u32 expected, u32 value);


int fr_fat_sync(struct fr_vol *v, u32 cluster);
#define FR_DIR_UPDATE 0
#define FR_DIR_CREATE 1
#define FR_DIR_DELETE 2
int fr_dir_write(const struct fr_file *f, int mode, const u8 *old_name);
int fr_dir_check(const struct fr_file *f);
int fr_fsinfo_adjust(const struct fr_vol *v, s32 clusters_taken, u32 next_free);


#define FR_FREE_UNKNOWN 0xFFFFFFFFu
extern volatile u32 fr_free_clusters;

void fr_name(u8 *name, const char *text);
void fr_number_name(u8 *name, u32 number);
void fr_log_name(u8 *name, u32 index);
u32 fr_split(const struct fr_vol *v, struct fr_file *f, struct fr_file *tail, u32 keep_clusters);
u32 fr_cut_position(const struct fr_vol *v, const struct fr_file *f, u32 keep_clusters,
                    u32 *extent, u32 *offset, u32 *next_cluster);

#endif
