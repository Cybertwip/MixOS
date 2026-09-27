/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
#ifndef MVII_FAT_H
#define MVII_FAT_H

/*
 * mvii_fat — read-only FAT12/16/32, for code that has a block device and a path
 * and nothing else underneath it.
 *
 * WHY this exists when the tree already contains a FatFs. FatFs lives under
 * Kernel/Kendryte/K210/Dependencies and wants a configuration header, a disk-IO
 * layer, an allocator for its work areas and about six thousand lines of link
 * budget. The LK slot is 512 KiB total, shared with the panel path, the USB
 * gadget and the console, and its linker script asserts on overrun. What LK
 * actually needs is: open one known path, copy it into DRAM, list a directory so
 * an operator can see why the path was wrong. That is this file.
 *
 * The block device is a callback rather than a direct call into the SD driver so
 * the same reader serves the removable card (mt6592_sd_read) and the eMMC user
 * area (mt6592_emmc_read_user) without either one being linked in when it is not
 * wanted. The console mounts both.
 *
 * Sectors are 512 bytes, full stop. Both block backends on this board are
 * 512-byte addressed, so a volume that declares anything else is a volume we
 * could not address correctly anyway -- mount rejects it rather than silently
 * reading the wrong offsets.
 *
 * Not implemented, deliberately: writing, timestamps, UTF-16 beyond ASCII (a
 * non-ASCII character becomes '?'), and the FSInfo hint. Long names ARE
 * implemented and are not optional -- the firmware this was written to fetch is
 * at System/Drivers/MediaTek/WiFi/MT6592/WIFI_RAM_CODE_SOC, and "MediaTek",
 * "WIFI_RAM_CODE_SOC" and "ROMv1_patch_1_1_hdr.bin" are every one of them a
 * long name. A short-name-only reader finds none of that path.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MVII_FAT_OK = 0,
    MVII_FAT_ERR_IO = -1,        /* the block callback failed                  */
    MVII_FAT_ERR_NOFS = -2,      /* nothing on the device parses as FAT        */
    MVII_FAT_ERR_NOTFOUND = -3,  /* a path component does not exist            */
    MVII_FAT_ERR_NOTDIR = -4,    /* a path component exists but is a file      */
    MVII_FAT_ERR_ISDIR = -5,     /* asked to read a directory as a file        */
    MVII_FAT_ERR_TOOBIG = -6,    /* file is larger than the caller's buffer    */
    MVII_FAT_ERR_PARAM = -7,     /* null argument, or a path we cannot parse   */
    MVII_FAT_ERR_CHAIN = -8      /* cluster chain is short, looped or invalid  */
};

/* Read `count` 512-byte sectors starting at `lba`. Returns 0 on success. */
typedef int (*mvii_fat_read_fn)(void* ctx, uint64_t lba, uint32_t count, void* buffer);

/* Called after each cluster of a file read. A 257 KiB firmware image off a slow
 * card outlives any watchdog worth arming, so the caller needs a place to kick
 * it and to show that something is still happening. */
typedef void (*mvii_fat_progress_fn)(void* ctx, uint32_t done, uint32_t total);

typedef struct {
    mvii_fat_read_fn read;
    void* read_ctx;
    mvii_fat_progress_fn progress;
    void* progress_ctx;

    uint64_t volume_lba;         /* first sector of the volume itself          */
    uint64_t fat_lba;            /* first sector of FAT #0                     */
    uint64_t root_lba;           /* FAT12/16 fixed root area; 0 on FAT32       */
    uint64_t data_lba;           /* sector of cluster 2                        */

    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_sectors;
    uint32_t num_fats;
    uint32_t root_entries;       /* FAT12/16 only                              */
    uint32_t root_sectors;       /* FAT12/16 fixed root area length            */
    uint32_t root_cluster;       /* FAT32 only                                 */
    uint32_t cluster_count;      /* data clusters, i.e. highest valid is +1    */
    uint32_t eoc;                /* chain values >= this end the chain         */
    uint32_t total_sectors;

    uint8_t type;                /* 12, 16 or 32                               */
    char label[12];              /* from the BPB, may be blank                 */
    char source[12];             /* "superfloppy", "mbr0".."mbr3", "gpt0"...   */
} mvii_fat_volume;

typedef struct {
    char name[64];               /* long name when present, else 8.3           */
    uint32_t first_cluster;
    uint32_t size;
    uint8_t attr;
    uint8_t is_dir;
} mvii_fat_dirent;

/* Return non-zero to stop the walk early. */
typedef int (*mvii_fat_list_fn)(void* ctx, const mvii_fat_dirent* entry);

/*
 * Find the first FAT volume the device offers, trying, in order: the whole
 * device as one volume (superfloppy, which is how a lot of cameras and phones
 * format a card), the four MBR primaries, then GPT if the MBR is protective.
 *
 * "First that parses" rather than "first with a FAT partition type" on purpose:
 * partition type bytes are frequently wrong on cards that have been reformatted
 * in place, and a BPB either parses or it does not. `v->source` records which
 * candidate won so the operator can see it.
 */
int mvii_fat_mount(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx);

/*
 * The same search, restricted to a volume carrying a particular BPB label
 * (case-insensitive). "First that parses" is the wrong answer on a card that
 * holds more than one FAT volume — a dArkOS card has a 100 MiB BOOT partition
 * and a several-gigabyte EASYROMS one, and only one of them has a kernel on it.
 * Returns MVII_FAT_ERR_NOFS when nothing carries that label, so a caller can
 * fall back to mvii_fat_mount() for a card that was made by hand.
 */
int mvii_fat_mount_labeled(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx, const char* label);

/* Mount one specific volume start, for when the search above picked wrong. */
int mvii_fat_mount_at(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx, uint64_t lba);

void mvii_fat_set_progress(mvii_fat_volume* v, mvii_fat_progress_fn fn, void* ctx);

/* '/' and '\' both separate; leading and repeated separators are ignored; an
 * empty path is the root directory. Matching is case-insensitive. */
int mvii_fat_stat(mvii_fat_volume* v, const char* path, mvii_fat_dirent* out);

int mvii_fat_list(mvii_fat_volume* v, const char* path, mvii_fat_list_fn cb, void* ctx);

/*
 * Copy a whole file into `buffer`. On MVII_FAT_ERR_TOOBIG the file's real size
 * is still written to *out_len, so a caller with a fixed scratch can say how
 * much it was short by instead of just "no".
 */
int mvii_fat_read_file(mvii_fat_volume* v, const char* path,
                       void* buffer, uint32_t buffer_size, uint32_t* out_len);

const char* mvii_fat_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* MVII_FAT_H */
