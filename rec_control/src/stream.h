/* The recorder: capture, ring, writing, markers, files.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef CONTINUOUS_RECORDER_H
#define CONTINUOUS_RECORDER_H

#include "recfile.h"
#include "pack.h"
#include "listen.h"


#define PAGE_BYTES            23040u
#define PAGE_SECTORS             45u
#define PAGE_BLOCKS             320u
#define PAGE_BLOCKS_16          480u


#define RING_PAGE_BLOCKS       1280u


#define COARSE_FROM_NUM           3u
#define COARSE_FROM_DEN           4u
#define COARSE_END_PAGES          2u
#define LOW_PAGES                 3u
#define STAGE_PAGES               3u
#define BASE_PAGES                6u
#define EXTRA_PAGES              30u
#define EXTRA_REGIONS             3u
#define POOL_BLOCKS               3u
#define POOL_BLOCK_BYTES      69120u
#define POOL_BLOCK_PAGES          3u
#define POOL_PAGES                9u
#define MAX_PAGES                45u
#define LOW_BUFFER_BYTES        9728u
#define LOW_AUDIT_BYTES      101888u
#define STAGE_AUDIT_BYTES     69120u
#define AUDIT_BYTES          (LOW_AUDIT_BYTES + STAGE_AUDIT_BYTES)
#define EXTRA_BYTES          691200u
#define PAGE_WORDS             5760u
#define SECTOR_BYTES            512u
#define SLOW_LOG_ENTRIES         20u
#define SLOW_LOG_US          300000u

#define MAP_BLOCK_BYTES       16384u
#define MAX_EXTENTS            1024u
#define MAX_BLOCK_SECTORS        32u


#define SAMPLE_BYTES              3u
#define FRAME_BYTES               6u
#define BLOCK_BYTES              72u
#define BYTE_RATE            288000u


#ifndef DEPTH_AT_POWER_ON
#define DEPTH_AT_POWER_ON         0u
#endif


#ifndef SILENCE_BLOCKS
#define SILENCE_BLOCKS        80000u
#endif


#ifndef MIN_KEEP_WORDS
#define MIN_KEEP_WORDS      5760000u
#endif
#ifndef HEADER_UPDATE_US
#define HEADER_UPDATE_US   15000000u
#endif

#ifndef FILE_AUDIO_PAGES
#define FILE_AUDIO_PAGES        90000u
#endif


#ifndef FILE_CAP_BYTES
#define FILE_CAP_BYTES     (SECTOR_BYTES + FILE_AUDIO_PAGES * PAGE_BYTES + 65536u)
#endif
#ifndef FILE_MIN_BYTES
#define FILE_MIN_BYTES       10485760u
#endif


#ifndef CUT_LEAST_BYTES
#define CUT_LEAST_BYTES      17280000u
#endif


#ifndef FILE_REFILL_BYTES
#define FILE_REFILL_BYTES   FILE_CAP_BYTES
#endif
#ifndef FILE_MARGIN_BYTES
#define FILE_MARGIN_BYTES     5242880u
#endif


#ifndef NEXT_AHEAD_WORDS
#define NEXT_AHEAD_WORDS    351360000u
#endif


#ifndef COUNTDOWN_WORDS
#define COUNTDOWN_WORDS     345600000u
#endif


#ifndef BURST_PAGES
#define BURST_PAGES                 1u
#endif
#define BURST_FREE_PAGES            9u
#define PAGE_US                 80000u


#ifndef CONTROL_START_KEY
#define CONTROL_START_KEY 0u
#endif


#ifndef CONTROL_EVERY_MESSAGE
#define CONTROL_EVERY_MESSAGE 1u
#endif
#ifndef CONTROL_STOP_KEY
#define CONTROL_STOP_KEY 0u
#endif
#ifndef CONTROL_MARK_KEY
#define CONTROL_MARK_KEY 0u
#endif


#ifndef CONTROL_CUT_KEY
#define CONTROL_CUT_KEY 0u
#endif
#ifndef AUTO_START
#define AUTO_START 1u
#endif


#define MAX_MARKS            100u
#define CUE_RESERVE_BYTES   4096u


#define LOST_MARKS            16u


#define COARSE_MARKS           8u


#define CUE_POINT_BYTES       24u
#define CUE_LABEL_BYTES       18u
#define NAMED_MARKS          (LOST_MARKS + COARSE_MARKS)
#define ALL_MARKS            (MAX_MARKS + NAMED_MARKS)
#define CUE_MAX_BYTES        (12u + ALL_MARKS * CUE_POINT_BYTES + 12u + NAMED_MARKS * CUE_LABEL_BYTES)


#define SHEET_COUNT_AT          8u
#define SHEET_POSITIONS_AT     12u
#define SHEET_SUM_AT          508u
#define HOLD_BLOCKS 8000u


#define START_WAIT_BLOCKS 240000u


#define REASON_INIT        0u
#define REASON_NO_USB      1u
#define REASON_NOT_FAT32   2u
#define REASON_FULL        3u
#define REASON_ERROR       4u
#define REASON_OTHER       5u

#define CUT_OK             0u
#define CUT_WAIT           1u
#define CUT_LOW            2u


#define STICK_POLL_US    1000000u
#define STICK_SETTLE_US  1000000u
#define STICK_AGAIN_US    100000u


#define ERROR_SHOW_US    2000000u

#define STATUS_OVERRUN            0x00000001u
#define STATUS_INVALID_BLOCK      0x00000002u
#define STATUS_DIRECT_WRITE       0x00000004u
#define STATUS_FS_IO              0x00000008u
#define STATUS_NOT_FAT32          0x00000010u
#define STATUS_HEADER_FAILED      0x00000020u
#define STATUS_MEMORY_DIRTY       0x00000040u
#define STATUS_FS_VERIFY          0x00000080u
#define STATUS_MAP_MISMATCH       0x00000100u
#define STATUS_REVALIDATION       0x00000200u
#define STATUS_NO_SPACE           0x00000400u
#define STATUS_FS_DIR             0x00000800u
#define STATUS_NAME_FAILED        0x00001000u
#define STATUS_BOUNDARY           0x00002000u
#define STATUS_ALLOC_FAILED       0x00004000u
#define STATUS_RELEASE_FAILED     0x00008000u
#define STATUS_FS_CHANGED         0x00010000u
#define STATUS_GEOMETRY           0x00020000u
#define STATUS_FS_CHAIN           0x00040000u
#define STATUS_EXTRA_CHANGED      0x00080000u
#define STATUS_SMALL_RING         0x00100000u
#define STATUS_GUARD_CHANGED      0x00200000u
#define STATUS_WATCH_CHANGED      0x00400000u
#define STATUS_COARSE             0x00800000u

#define CAPTURE_IDLE   0u
#define CAPTURE_ARMED  1u
#define CAPTURE_ACTIVE 2u
#define CAPTURE_DONE   3u

#define END_FULL     1u
#define END_SILENCE  2u
#define END_INVALID  3u
#define END_MANUAL   4u

#define TAKE_NONE      0u
#define TAKE_RECORDING 1u
#define TAKE_COMPLETE  2u


#define EXTRA_PENDING  0u
#define EXTRA_ACCEPTED 1u
#define EXTRA_BAD      2u
#define EXTRA_STATE_MASK 0xFFu
#define EXTRA_OUT_SHIFT   8u

#define REPORT_SCHEMA 53u


#define EVENT_ARM      1u
#define EVENT_START    2u
#define EVENT_KEEP     3u
#define EVENT_DISCARD  4u
#define EVENT_ABORT    5u
#define EVENT_FAIL     6u
#define EVENT_READY    7u
#define EVENT_SWITCH   8u
#define EVENT_CUT      9u
#define EVENT_SPLIT   10u

#ifndef SIM
#define REG32(address) (*(volatile u32 *)(address))
#define DMA3_RDAR       REG32(0xFFFE1134u)
#define DMA3_RDMATCR    REG32(0xFFFE1138u)
#define SEND_BUFFER_0   0xFFF8715Cu
#define SEND_BUFFER_1   0xFFF8721Cu
#define LOW_BUFFER      ((volatile u8 *)0x1C040000u)
#define LINK_RX         ((const volatile u8 *)0xFFF86EA8u)
#define LINK_TX         ((volatile u8 *)0xFFF86F28u)
#define LINK_STATE      ((const volatile u8 *)0xFFF86FA8u)
#define STAGE_BUFFER    ((volatile u8 *)0x1C120000u)


#define EXTRA_REGIONS_INIT { { (volatile u8 *)0x1C0AD800u, 20u }, \
                             { (volatile u8 *)0x1C130E00u, 2u }, \
                             { (volatile u8 *)0x1C077000u, 8u } }
#define LOW_ADDRESS     0x1C040000u
#define STAGE_ADDRESS   0x1C120000u


extern const volatile u8 vars_end[];
#define GUARD_START     ((const volatile u32 *)vars_end)
#define GUARD_END       ((const volatile u32 *)0x1C05B000u)
#define ORIGINAL_SEND_ISR         ((void (*)(void))0xFFF84EA6u)
#define STOCK_BACKGROUND_CALLBACK ((void (*)(void))0x1C0095E0u)
#define STOCK_TIME_US             ((u32 (*)(void))0xFFF84F88u)
#define STOCK_BLOCK_READ          ((int (*)(void *, u32, void *, u16))0x1C01397Au)
#define STOCK_BLOCK_WRITE         ((int (*)(void *, u32, const void *, u16))0x1C0139FCu)
#define STOCK_POOL_ALLOC          ((void *(*)(u32))0x1C028EF8u)
#define STOCK_POOL_RELEASE        ((int (*)(void *, u32))0x1C028F0Au)


#define STOCK_VOLUME_LIST         (*(void * volatile *)0x1C0ABB2Cu)
#define STOCK_FIND_VOLUME         ((int (*)(int, void **))0x1C0138A4u)
#define STOCK_MEDIA_CHECK         ((int (*)(void *))0x1C0138CEu)
#define FS_SECTORS_PER_CLUSTER(f) (*(volatile u16 *)((u8 *)(f) + 4u))
#define FS_RESERVED_SECTORS(f)    (*(volatile u16 *)((u8 *)(f) + 6u))
#define FS_FAT_COUNT(f)           (*(volatile u16 *)((u8 *)(f) + 8u))
#define FS_FAT_SECTORS(f)         (*(volatile u32 *)((u8 *)(f) + 12u))
#define FS_TYPE(f)                (*(volatile u8 *)((u8 *)(f) + 16u))
#define SIM_EVENT(code)           ((void)0)
#else
extern volatile u32 sim_dma3_rdar;
extern volatile u32 sim_dma3_rdmatcr;
extern volatile u32 sim_send_buffer_0[48];
extern volatile u32 sim_send_buffer_1[48];
extern volatile u8 sim_low_buffer[LOW_AUDIT_BYTES];
extern volatile u8 sim_link_rx[LINK_PACKET_BYTES];
extern volatile u8 sim_link_tx[LINK_PACKET_BYTES];
extern volatile u8 sim_link_state;
#define LINK_RX         sim_link_rx
#define LINK_TX         sim_link_tx
#define LINK_STATE      (&sim_link_state)
extern volatile u8 sim_stage_buffer[STAGE_AUDIT_BYTES];
extern volatile u8 sim_extra_buffer[EXTRA_BYTES];
#define SIM_GUARD_WORDS 64u
extern volatile u32 sim_guard[SIM_GUARD_WORDS];
#define EXTRA_REGIONS_INIT { { sim_extra_buffer, 20u }, \
                             { sim_extra_buffer + 20u * PAGE_BYTES, 2u }, \
                             { sim_extra_buffer + 22u * PAGE_BYTES, 8u } }
#define GUARD_START     ((const volatile u32 *)sim_guard)
#define GUARD_END       ((const volatile u32 *)sim_guard + SIM_GUARD_WORDS)

#define DMA3_RDAR       sim_dma3_rdar
#define DMA3_RDMATCR    sim_dma3_rdmatcr
#define SEND_BUFFER_0   ((u32)&sim_send_buffer_0[0])
#define SEND_BUFFER_1   ((u32)&sim_send_buffer_1[0])
#define LOW_BUFFER      sim_low_buffer
#define STAGE_BUFFER    sim_stage_buffer
#define LOW_ADDRESS     0x1C040000u
#define STAGE_ADDRESS   0x1C120000u

void sim_original_send_isr(void);
void sim_stock_background_callback(void);
u32 sim_time_us(void);
int sim_block_read(void *fs, u32 sector, void *data, u16 count);
int sim_block_write(void *fs, u32 sector, const void *data, u16 count);
void *sim_volume_list(void);
int sim_find_volume(int letter, void **fs);
int sim_media_check(void *fs);
u16 sim_fs_sectors_per_cluster(void *fs);
u16 sim_fs_reserved_sectors(void *fs);
u16 sim_fs_fat_count(void *fs);
u32 sim_fs_fat_sectors(void *fs);
u8 sim_fs_type(void *fs);
void *sim_pool_alloc(u32 size);
int sim_pool_release(void *block, u32 size);
void sim_event(u32 code);

#define ORIGINAL_SEND_ISR         sim_original_send_isr
#define STOCK_BACKGROUND_CALLBACK sim_stock_background_callback
#define STOCK_TIME_US             sim_time_us
#define STOCK_BLOCK_READ          sim_block_read
#define STOCK_BLOCK_WRITE         sim_block_write
#define STOCK_VOLUME_LIST         sim_volume_list()
#define STOCK_FIND_VOLUME         sim_find_volume
#define STOCK_MEDIA_CHECK         sim_media_check
#define FS_SECTORS_PER_CLUSTER(f) sim_fs_sectors_per_cluster(f)
#define FS_RESERVED_SECTORS(f)    sim_fs_reserved_sectors(f)
#define FS_FAT_COUNT(f)           sim_fs_fat_count(f)
#define FS_FAT_SECTORS(f)         sim_fs_fat_sectors(f)
#define FS_TYPE(f)                sim_fs_type(f)
#define STOCK_POOL_ALLOC          sim_pool_alloc
#define STOCK_POOL_RELEASE        sim_pool_release
#define SIM_EVENT(code)           sim_event(code)

u32 stream_sim_phase(void);
u32 stream_sim_status_flags(void);
u32 stream_sim_captured_blocks(void);
u32 stream_sim_capture_state(void);
u32 stream_sim_coarse(void);
u32 stream_sim_deep(void);
void stream_sim_reboot(void);
#endif

enum recorder_phase {
    PHASE_VERIFY_MEMORY = 0,
    PHASE_SETTLE,
    PHASE_SNAPSHOT,
    PHASE_PREPARE,
    PHASE_LOG,
    PHASE_AUDIT,
    PHASE_ARMED,
    PHASE_STREAM,
    PHASE_CLOSE,
    PHASE_FAILED
};


struct session_state {
    u32 phase;
    u32 status_flags;
    u32 takes_started;
    u32 takes_kept;
    u32 takes_discarded;
    u32 takes_failed;
    u32 prepare_attempts;
    u32 files_made;
    u32 files_reused;
    u32 files_refilled;
    u32 files_recovered;
    u32 files_cut;
    u32 next_number;
    s32 fs_error;
    u32 fs_error_phase;
    u32 fs_phase;
    u32 sectors_per_cluster;
    u32 fat_start;
    u32 fat_sectors;
    u32 data_start;
    u32 end_cluster;
    u32 first_sector;
    u32 file_bytes;
    u32 extent_count;
    u32 map_sum;
    u32 prepare_duration_us;
    u32 sector_reads;
    u32 sector_writes;
    u32 ring_base_pages;
    u32 extra_state;
    u32 extra_passes;
    u32 first_bad_address;
    u32 first_bad_value;
    u32 direct_read_errors;
    u32 direct_write_errors;
    u32 first_release_error;
    u32 log_writes;
    u32 armed_us;
    u32 next_state;
    u32 files_switched;
    u32 text_lines;
    s32 text_error;
};

struct take_state {
    u32 state;
    u32 flags;
    u32 actual_words;
    u32 file_offset_bytes;
    u32 write_count;
    u32 data_write_min_us;
    u32 data_write_max_us;
    u32 total_data_write_us;
    u32 max_write_offset_bytes;
    u32 slow_writes[3];
    u32 slow_count;
    u32 slow_log[SLOW_LOG_ENTRIES];
    u32 direct_block_writes;
    u32 header_writes;
    u32 header_write_max_us;
    u32 service_gap_max_us;
    u32 start_duration_us;
    u32 pool_pages;
    u32 number;
    u32 name_duration_us;
    u32 chain_index;
    u32 continued;
    u32 blocks_before;

    u32 dropped_before;
    u32 gaps_before;
    u32 sheet_stamp;
};

struct capture_state {
    u32 state;
    u32 ring_pages;
    u32 target_words;
    u32 produced_words;
    u32 captured_blocks;
    u32 invalid_blocks;
    u32 current_page_bytes;
    u32 pages_produced;
    u32 maximum_pending_bytes;
    u32 dropped_blocks;
    u32 gap_count;
    u32 in_gap;
    u32 coarse;

    u32 silent_blocks;
    u32 end_reason;
    u32 current_page_blocks;
    u32 maximum_pending_blocks;
    u32 by_hand;
    u32 marks_set;
    u32 cue_marks;
    u32 cue_lost;
};

#define REPORT_OFFSET          64u
#define REPORT_BYTES           (sizeof(struct session_state) + sizeof(struct take_state) + \
                                sizeof(struct capture_state))
#define REPORT_END_OFFSET      (REPORT_OFFSET + REPORT_BYTES)

void recorder_service(void);
void stream_capture_isr(void);

#endif
