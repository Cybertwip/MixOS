/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
/* See mvii_fat.h for what this is and why it is not FatFs. */

#include "mvii_fat.h"

#define SECTOR_BYTES 512u
#define DIRENT_BYTES 32u
#define DIRENTS_PER_SECTOR (SECTOR_BYTES / DIRENT_BYTES)

#define ATTR_READ_ONLY 0x01u
#define ATTR_HIDDEN    0x02u
#define ATTR_SYSTEM    0x04u
#define ATTR_VOLUME_ID 0x08u
#define ATTR_DIRECTORY 0x10u
#define ATTR_LFN       0x0fu

/*
 * Long-name assembly area. Sequence numbers run 1..20 and each entry carries 13
 * UTF-16 units, so the highest byte a well-formed chain can address is
 * (20-1)*13+12 = 259 -- hence 272 and not 256, which is the size everyone
 * reaches for first and which a maximum-length name overruns by four bytes.
 */
#define LFN_MAX_SEQ 20u
#define LFN_CHARS   272u

/*
 * Two 512-byte staging areas, both file-static.
 *
 * They are static rather than stack because LK's stack is 4 KiB below
 * __stack_top and the console already nests several frames deep by the time it
 * gets here; two sector buffers plus a 272-byte name buffer on top of that is
 * how a directory walk turns into a silent stack overflow into .bss.
 *
 * g_fat is a one-sector cache of the FAT itself, which is the read that would
 * otherwise dominate: without it, walking a 257 KiB file's chain re-reads the
 * same FAT sector once per cluster. `g_fat_owner` keys the cache on the block
 * device so mounting the SD card after the eMMC does not serve the eMMC's FAT.
 */
static uint8_t g_sec[SECTOR_BYTES];
static uint8_t g_fat[SECTOR_BYTES];
static uint64_t g_fat_lba;
static const void* g_fat_owner;
static int g_fat_valid;

static char g_lfn[LFN_CHARS];

/* ── little-endian field access ── */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t* p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void mem_copy(void* dst, const void* src, uint32_t n)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n-- != 0u) *d++ = *s++;
}

static void mem_zero(void* dst, uint32_t n)
{
    uint8_t* d = (uint8_t*)dst;
    while (n-- != 0u) *d++ = 0u;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static void str_copy(char* dst, const char* src, uint32_t cap)
{
    uint32_t i = 0u;
    if (cap == 0u) return;
    while (src[i] != '\0' && i + 1u < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

/* ── block access ── */

static int dev_read(mvii_fat_volume* v, uint64_t lba, uint32_t count, void* buf)
{
    if (v->read == 0) return MVII_FAT_ERR_PARAM;
    if (v->read(v->read_ctx, lba, count, buf) != 0) return MVII_FAT_ERR_IO;
    return MVII_FAT_OK;
}

static int fat_sector(mvii_fat_volume* v, uint64_t lba)
{
    if (g_fat_valid && g_fat_owner == v->read_ctx && g_fat_lba == lba) return MVII_FAT_OK;
    g_fat_valid = 0;
    if (dev_read(v, lba, 1u, g_fat) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
    g_fat_lba = lba;
    g_fat_owner = v->read_ctx;
    g_fat_valid = 1;
    return MVII_FAT_OK;
}

/* ── cluster chain ── */

static int cluster_valid(const mvii_fat_volume* v, uint32_t cl)
{
    return (cl >= 2u) && (cl < v->cluster_count + 2u) && (cl < v->eoc);
}

static uint64_t cluster_lba(const mvii_fat_volume* v, uint32_t cl)
{
    return v->data_lba + (uint64_t)(cl - 2u) * v->sectors_per_cluster;
}

static int fat_next(mvii_fat_volume* v, uint32_t cl, uint32_t* next)
{
    uint32_t byte;
    uint32_t off;
    uint32_t val;

    if (v->type == 32u) {
        byte = cl * 4u;
        off = byte % SECTOR_BYTES;
        if (fat_sector(v, v->fat_lba + byte / SECTOR_BYTES) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
        val = rd32(&g_fat[off]) & 0x0fffffffu;
    } else if (v->type == 16u) {
        byte = cl * 2u;
        off = byte % SECTOR_BYTES;
        if (fat_sector(v, v->fat_lba + byte / SECTOR_BYTES) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
        val = rd16(&g_fat[off]);
    } else {
        /* FAT12: 1.5 bytes per entry, and the pair straddles a sector boundary
         * for one entry in every 341. Read the halves separately rather than
         * assuming they are adjacent in the cache. */
        uint32_t lo;
        uint32_t hi;
        byte = cl + (cl >> 1);
        off = byte % SECTOR_BYTES;
        if (fat_sector(v, v->fat_lba + byte / SECTOR_BYTES) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
        lo = g_fat[off];
        if (off == SECTOR_BYTES - 1u) {
            if (fat_sector(v, v->fat_lba + byte / SECTOR_BYTES + 1u) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
            hi = g_fat[0];
        } else {
            hi = g_fat[off + 1u];
        }
        val = (hi << 8) | lo;
        val = ((cl & 1u) != 0u) ? (val >> 4) : (val & 0x0fffu);
    }

    *next = val;
    return MVII_FAT_OK;
}

/* ── short and long names ── */

static uint8_t sfn_checksum(const uint8_t* name11)
{
    uint8_t sum = 0u;
    uint32_t i;
    for (i = 0u; i < 11u; ++i) {
        sum = (uint8_t)((((sum & 1u) != 0u) ? 0x80u : 0u) + (uint32_t)(sum >> 1) + (uint32_t)name11[i]);
    }
    return sum;
}

/* 11 packed bytes -> "NAME.EXT". The two NT case bits are honoured, which is the
 * difference between printing "SYSTEM" and printing "System" for a name that
 * needed no long entry at all. */
static void sfn_to_string(const uint8_t* e, char* out)
{
    const uint8_t nt = e[12];
    uint32_t n = 0u;
    uint32_t i;

    for (i = 0u; i < 8u && e[i] != ' '; ++i) {
        char c = (char)e[i];
        /* 0x05 in byte 0 is an escaped 0xE5, which is otherwise the free mark. */
        if (i == 0u && e[0] == 0x05u) c = (char)0xe5;
        if ((nt & 0x08u) != 0u) c = lower(c);
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8u; i < 11u && e[i] != ' '; ++i) {
            char c = (char)e[i];
            if ((nt & 0x10u) != 0u) c = lower(c);
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

/* Compare against one path component, which is not NUL-terminated. */
static int name_eq_n(const char* a, const char* b, uint32_t blen)
{
    uint32_t i;
    for (i = 0u; i < blen; ++i) {
        if (a[i] == '\0') return 0;
        if (lower(a[i]) != lower(b[i])) return 0;
    }
    return a[blen] == '\0';
}

/* ── directory walk ──
 *
 * One function serves lookup and listing because the two differ only in what
 * happens at a matching entry, and a shared walker cannot drift out of sync with
 * itself. `want`/`want_len` non-null means lookup; `cb` non-null means listing.
 *
 * Directories are addressed by cluster, with 0 meaning "the root". On FAT32 that
 * redirects to BPB_RootClus; on FAT12/16 the root is a fixed extent outside the
 * data area with no chain at all, which is the one case the cluster walk below
 * cannot express and so is handled as its own loop.
 */

typedef struct {
    /* Long-name accumulation, carried across sector and cluster boundaries
     * because a name's entries are free to straddle either. */
    int active;
    uint32_t expect;
    uint8_t checksum;
} lfn_state;

static void lfn_reset(lfn_state* s)
{
    s->active = 0;
    s->expect = 0u;
    s->checksum = 0u;
}

static void lfn_absorb(lfn_state* s, const uint8_t* e)
{
    /* The 13 UTF-16 units of one LFN entry, at their three non-contiguous
     * offsets. Anyone who writes this loop as a single range gets the checksum
     * and the attribute byte in the middle of the name. */
    static const uint8_t slots[13] = {1u, 3u, 5u, 7u, 9u, 14u, 16u, 18u, 20u, 22u, 24u, 28u, 30u};
    const uint32_t ord = (uint32_t)e[0] & 0x3fu;
    uint32_t base;
    uint32_t i;

    if ((e[0] & 0x40u) != 0u) {
        /* First entry encountered is the LAST chunk of the name; it starts a
         * fresh chain and declares the checksum every other chunk must match. */
        mem_zero(g_lfn, LFN_CHARS);
        s->active = 1;
        s->expect = ord;
        s->checksum = e[13];
    }

    if (s->active == 0 || ord == 0u || ord > LFN_MAX_SEQ || ord != s->expect || e[13] != s->checksum) {
        lfn_reset(s);
        return;
    }
    s->expect = ord - 1u;

    base = (ord - 1u) * 13u;
    for (i = 0u; i < 13u; ++i) {
        const uint16_t u = rd16(&e[slots[i]]);
        if (u == 0x0000u || u == 0xffffu) break;
        /* Positions are absolute, so a truncated chunk simply leaves the zeros
         * the reset already wrote -- no separate length to track. */
        g_lfn[base + i] = (u < 0x80u) ? (char)u : '?';
    }
}

/* Non-zero when g_lfn holds a complete name for the short entry at `e`. */
static int lfn_complete(const lfn_state* s, const uint8_t* e)
{
    return s->active != 0 && s->expect == 0u && sfn_checksum(e) == s->checksum && g_lfn[0] != '\0';
}

static void dirent_fill(mvii_fat_dirent* out, const uint8_t* e, const char* name)
{
    str_copy(out->name, name, sizeof out->name);
    out->attr = e[11];
    out->is_dir = ((e[11] & ATTR_DIRECTORY) != 0u) ? 1u : 0u;
    out->size = rd32(&e[28]);
    out->first_cluster = ((uint32_t)rd16(&e[20]) << 16) | (uint32_t)rd16(&e[26]);
}

/* Returns MVII_FAT_OK on a lookup hit or a completed listing, ERR_NOTFOUND when
 * a lookup ran off the end, or a negative I/O error. */
static int dir_walk(mvii_fat_volume* v, uint32_t dir_cluster,
                    const char* want, uint32_t want_len,
                    mvii_fat_dirent* out, mvii_fat_list_fn cb, void* cbctx)
{
    lfn_state lfn;
    uint64_t lba;
    uint32_t sectors_left;
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0u;
    int fixed_root = 0;

    lfn_reset(&lfn);

    if (cluster == 0u) {
        if (v->type == 32u) {
            cluster = v->root_cluster;
        } else {
            fixed_root = 1;
            lba = v->root_lba;
            sectors_left = v->root_sectors;
        }
    }

    for (;;) {
        uint32_t s;

        if (!fixed_root) {
            if (!cluster_valid(v, cluster)) return MVII_FAT_ERR_NOTFOUND;
            /* A chain that outlives the volume's own cluster count is a loop;
             * bail rather than walk it forever with the watchdog armed. */
            if (++guard > v->cluster_count + 2u) return MVII_FAT_ERR_CHAIN;
            lba = cluster_lba(v, cluster);
            sectors_left = v->sectors_per_cluster;
        }

        for (s = 0u; s < sectors_left; ++s) {
            uint32_t i;
            if (dev_read(v, lba + s, 1u, g_sec) != MVII_FAT_OK) return MVII_FAT_ERR_IO;

            for (i = 0u; i < DIRENTS_PER_SECTOR; ++i) {
                const uint8_t* e = &g_sec[i * DIRENT_BYTES];
                char sfn[13];
                const char* name;
                mvii_fat_dirent ent;

                if (e[0] == 0x00u) return (cb != 0) ? MVII_FAT_OK : MVII_FAT_ERR_NOTFOUND;
                if (e[0] == 0xe5u) { lfn_reset(&lfn); continue; }
                if ((e[11] & 0x3fu) == ATTR_LFN) { lfn_absorb(&lfn, e); continue; }
                if ((e[11] & ATTR_VOLUME_ID) != 0u) { lfn_reset(&lfn); continue; }

                sfn_to_string(e, sfn);
                name = lfn_complete(&lfn, e) ? g_lfn : sfn;

                if (want != 0) {
                    /* Both spellings are accepted: an operator who typed the
                     * mangled 8.3 form they saw in a hex dump gets the file. */
                    if (name_eq_n(name, want, want_len) || name_eq_n(sfn, want, want_len)) {
                        dirent_fill(out, e, name);
                        return MVII_FAT_OK;
                    }
                } else if (cb != 0) {
                    dirent_fill(&ent, e, name);
                    if (cb(cbctx, &ent) != 0) return MVII_FAT_OK;
                }
                lfn_reset(&lfn);
            }
        }

        if (fixed_root) return (cb != 0) ? MVII_FAT_OK : MVII_FAT_ERR_NOTFOUND;
        if (fat_next(v, cluster, &cluster) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
    }
}

/* ── path resolution ── */

/* Advance past separators and return the length of the next component. */
static uint32_t path_component(const char** p)
{
    const char* s = *p;
    uint32_t n = 0u;
    while (*s == '/' || *s == '\\') ++s;
    *p = s;
    while (s[n] != '\0' && s[n] != '/' && s[n] != '\\') ++n;
    return n;
}

static int resolve(mvii_fat_volume* v, const char* path, mvii_fat_dirent* out)
{
    const char* p = path;
    uint32_t cluster = 0u;
    int have = 0;

    /* The root has no directory entry to describe it, so synthesise one. */
    str_copy(out->name, "/", sizeof out->name);
    out->attr = ATTR_DIRECTORY;
    out->is_dir = 1u;
    out->size = 0u;
    out->first_cluster = 0u;

    for (;;) {
        uint32_t len = path_component(&p);
        int rc;

        if (len == 0u) break;

        if (have != 0 && out->is_dir == 0u) return MVII_FAT_ERR_NOTDIR;

        rc = dir_walk(v, cluster, p, len, out, 0, 0);
        if (rc != MVII_FAT_OK) return rc;

        /* "." and ".." in a subdirectory carry cluster 0 for the root. Our
         * convention already spells the root as 0, so this needs no fixup --
         * but a *file* with cluster 0 is an empty file, and reading it must not
         * fall through to the root directory's contents. */
        cluster = out->first_cluster;
        have = 1;
        p += len;
    }

    return MVII_FAT_OK;
}

/* ── mount ── */

static int parse_bpb(mvii_fat_volume* v, const uint8_t* s, uint64_t lba)
{
    uint32_t bps;
    uint32_t spc;
    uint32_t reserved;
    uint32_t fats;
    uint32_t root_entries;
    uint32_t fat_sz;
    uint32_t total;
    uint32_t root_sectors;
    uint32_t first_data;
    uint32_t clusters;
    uint32_t i;

    if (s[510] != 0x55u || s[511] != 0xaau) return MVII_FAT_ERR_NOFS;

    /* The jump instruction. This is the check that keeps an MBR from being
     * mistaken for a superfloppy BPB: an MBR's first bytes are bootstrap code
     * that only coincidentally starts with EB/E9. */
    if (!(s[0] == 0xebu || s[0] == 0xe9u)) return MVII_FAT_ERR_NOFS;

    bps = rd16(&s[11]);
    spc = s[13];
    reserved = rd16(&s[14]);
    fats = s[16];
    root_entries = rd16(&s[17]);
    fat_sz = rd16(&s[22]);
    total = rd16(&s[19]);

    if (bps != SECTOR_BYTES) return MVII_FAT_ERR_NOFS;
    if (spc == 0u || (spc & (spc - 1u)) != 0u || spc > 128u) return MVII_FAT_ERR_NOFS;
    if (reserved == 0u) return MVII_FAT_ERR_NOFS;
    if (fats == 0u || fats > 2u) return MVII_FAT_ERR_NOFS;

    if (fat_sz == 0u) fat_sz = rd32(&s[36]);
    if (total == 0u) total = rd32(&s[32]);
    if (fat_sz == 0u || total == 0u) return MVII_FAT_ERR_NOFS;

    root_sectors = ((root_entries * DIRENT_BYTES) + (SECTOR_BYTES - 1u)) / SECTOR_BYTES;
    first_data = reserved + fats * fat_sz + root_sectors;
    if (total <= first_data) return MVII_FAT_ERR_NOFS;
    clusters = (total - first_data) / spc;
    if (clusters == 0u) return MVII_FAT_ERR_NOFS;

    v->volume_lba = lba;
    v->sectors_per_cluster = spc;
    v->cluster_bytes = spc * SECTOR_BYTES;
    v->fat_sectors = fat_sz;
    v->num_fats = fats;
    v->root_entries = root_entries;
    v->root_sectors = root_sectors;
    v->cluster_count = clusters;
    v->total_sectors = total;
    v->fat_lba = lba + reserved;
    v->root_lba = v->fat_lba + (uint64_t)fats * fat_sz;
    v->data_lba = v->root_lba + root_sectors;

    /* The cluster count is the definition of the FAT width -- not the "FAT32"
     * text in the BPB, which is a comment and is wrong on plenty of cards. */
    if (clusters < 4085u) {
        v->type = 12u;
        v->eoc = 0x0ff8u;
    } else if (clusters < 65525u) {
        v->type = 16u;
        v->eoc = 0xfff8u;
    } else {
        v->type = 32u;
        v->eoc = 0x0ffffff8u;
    }

    if (v->type == 32u) {
        if (root_entries != 0u || rd16(&s[22]) != 0u) return MVII_FAT_ERR_NOFS;
        v->root_cluster = rd32(&s[44]);
        if (v->root_cluster < 2u || v->root_cluster >= clusters + 2u) return MVII_FAT_ERR_NOFS;
        v->root_lba = 0u;
        v->root_sectors = 0u;
    } else {
        if (root_entries == 0u) return MVII_FAT_ERR_NOFS;
        v->root_cluster = 0u;
    }

    {
        const uint8_t* lbl = (v->type == 32u) ? &s[71] : &s[43];
        uint32_t n = 11u;
        while (n > 0u && lbl[n - 1u] == ' ') --n;
        for (i = 0u; i < n; ++i) v->label[i] = (char)lbl[i];
        v->label[n] = '\0';
    }

    return MVII_FAT_OK;
}

static void source_tag(mvii_fat_volume* v, const char* prefix, int index)
{
    uint32_t n = 0u;
    while (prefix[n] != '\0' && n + 2u < sizeof v->source) { v->source[n] = prefix[n]; ++n; }
    if (index >= 0) {
        if (index >= 10 && n + 2u < sizeof v->source) v->source[n++] = (char)('0' + (index / 10));
        v->source[n++] = (char)('0' + (index % 10));
    }
    v->source[n] = '\0';
}

int mvii_fat_mount_at(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx, uint64_t lba)
{
    int rc;

    if (v == 0 || read == 0) return MVII_FAT_ERR_PARAM;
    mem_zero(v, sizeof *v);
    v->read = read;
    v->read_ctx = ctx;
    g_fat_valid = 0;

    if (read(ctx, lba, 1u, g_sec) != 0) return MVII_FAT_ERR_IO;
    rc = parse_bpb(v, g_sec, lba);
    if (rc == MVII_FAT_OK) source_tag(v, "lba", -1);
    return rc;
}

/* Case-insensitive whole-string compare against a mounted volume's label. A
 * NULL filter matches everything, which is what makes one search serve both
 * mvii_fat_mount() and mvii_fat_mount_labeled(). */
static int label_eq(const char* label, const char* want)
{
    uint32_t i = 0u;

    if (want == 0) return 1;
    while (label[i] != '\0' && want[i] != '\0') {
        if (lower(label[i]) != lower(want[i])) return 0;
        ++i;
    }
    return label[i] == want[i];
}

static int mount_search(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx, const char* want_label)
{
    uint8_t mbr[SECTOR_BYTES];
    int gpt = 0;
    int i;

    if (v == 0 || read == 0) return MVII_FAT_ERR_PARAM;
    g_fat_valid = 0;

    if (read(ctx, 0u, 1u, mbr) != 0) return MVII_FAT_ERR_IO;

    mem_zero(v, sizeof *v);
    v->read = read;
    v->read_ctx = ctx;
    if (parse_bpb(v, mbr, 0u) == MVII_FAT_OK && label_eq(v->label, want_label)) {
        source_tag(v, "superfloppy", -1);
        return MVII_FAT_OK;
    }

    if (mbr[510] != 0x55u || mbr[511] != 0xaau) return MVII_FAT_ERR_NOFS;

    for (i = 0; i < 4; ++i) {
        const uint8_t* e = &mbr[446 + i * 16];
        const uint8_t type = e[4];
        const uint32_t first = rd32(&e[8]);
        const uint32_t count = rd32(&e[12]);

        if (e[0] != 0x00u && e[0] != 0x80u) continue; /* not a partition table  */
        if (type == 0xeeu) { gpt = 1; continue; }     /* protective MBR         */
        if (type == 0x00u || type == 0x05u || type == 0x0fu) continue;
        if (first == 0u || count == 0u) continue;

        mem_zero(v, sizeof *v);
        v->read = read;
        v->read_ctx = ctx;
        if (read(ctx, first, 1u, g_sec) != 0) continue;
        if (parse_bpb(v, g_sec, first) == MVII_FAT_OK && label_eq(v->label, want_label)) {
            source_tag(v, "mbr", i);
            return MVII_FAT_OK;
        }
    }

    if (gpt != 0) {
        uint8_t hdr[SECTOR_BYTES];
        if (read(ctx, 1u, 1u, hdr) == 0 &&
            hdr[0] == 'E' && hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == ' ' &&
            hdr[4] == 'P' && hdr[5] == 'A' && hdr[6] == 'R' && hdr[7] == 'T') {
            const uint64_t entries = rd64(&hdr[72]);
            uint32_t count = rd32(&hdr[80]);
            const uint32_t esz = rd32(&hdr[84]);
            uint32_t n;

            if (esz >= 128u && esz <= SECTOR_BYTES && count <= 128u) {
                for (n = 0u; n < count; ++n) {
                    const uint32_t per_sector = SECTOR_BYTES / esz;
                    const uint64_t sec = entries + (n / per_sector);
                    const uint8_t* e;
                    uint64_t first;

                    if (read(ctx, sec, 1u, g_sec) != 0) break;
                    e = &g_sec[(n % per_sector) * esz];
                    first = rd64(&e[32]);
                    if (first == 0u) continue;
                    /* g_sec is about to be reused for the candidate's BPB, so
                     * the entry has to be consumed before the next read. */
                    mem_zero(v, sizeof *v);
                    v->read = read;
                    v->read_ctx = ctx;
                    if (read(ctx, first, 1u, g_sec) != 0) continue;
                    if (parse_bpb(v, g_sec, first) == MVII_FAT_OK && label_eq(v->label, want_label)) {
                        source_tag(v, "gpt", (int)n);
                        return MVII_FAT_OK;
                    }
                }
            }
        }
    }

    mem_zero(v, sizeof *v);
    return MVII_FAT_ERR_NOFS;
}

int mvii_fat_mount(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx)
{
    return mount_search(v, read, ctx, 0);
}

int mvii_fat_mount_labeled(mvii_fat_volume* v, mvii_fat_read_fn read, void* ctx, const char* label)
{
    if (label == 0 || label[0] == '\0') return MVII_FAT_ERR_PARAM;
    return mount_search(v, read, ctx, label);
}

void mvii_fat_set_progress(mvii_fat_volume* v, mvii_fat_progress_fn fn, void* ctx)
{
    if (v == 0) return;
    v->progress = fn;
    v->progress_ctx = ctx;
}

/* ── public lookup / list / read ── */

int mvii_fat_stat(mvii_fat_volume* v, const char* path, mvii_fat_dirent* out)
{
    if (v == 0 || v->read == 0 || path == 0 || out == 0) return MVII_FAT_ERR_PARAM;
    return resolve(v, path, out);
}

int mvii_fat_list(mvii_fat_volume* v, const char* path, mvii_fat_list_fn cb, void* ctx)
{
    mvii_fat_dirent dir;
    int rc;

    if (v == 0 || v->read == 0 || path == 0 || cb == 0) return MVII_FAT_ERR_PARAM;
    rc = resolve(v, path, &dir);
    if (rc != MVII_FAT_OK) return rc;
    if (dir.is_dir == 0u) return MVII_FAT_ERR_NOTDIR;
    return dir_walk(v, dir.first_cluster, 0, 0u, 0, cb, ctx);
}

int mvii_fat_read_file(mvii_fat_volume* v, const char* path,
                       void* buffer, uint32_t buffer_size, uint32_t* out_len)
{
    mvii_fat_dirent f;
    uint8_t* dst = (uint8_t*)buffer;
    uint32_t remaining;
    uint32_t done = 0u;
    uint32_t cluster;
    uint32_t guard = 0u;
    int rc;

    if (v == 0 || v->read == 0 || path == 0 || buffer == 0) return MVII_FAT_ERR_PARAM;

    rc = resolve(v, path, &f);
    if (rc != MVII_FAT_OK) return rc;
    if (f.is_dir != 0u) return MVII_FAT_ERR_ISDIR;

    if (out_len != 0) *out_len = f.size;
    if (f.size > buffer_size) return MVII_FAT_ERR_TOOBIG;
    if (f.size == 0u) return MVII_FAT_OK;

    remaining = f.size;
    cluster = f.first_cluster;

    while (remaining != 0u) {
        uint64_t lba;
        uint32_t s;

        if (!cluster_valid(v, cluster)) return MVII_FAT_ERR_CHAIN;
        if (++guard > v->cluster_count + 2u) return MVII_FAT_ERR_CHAIN;

        lba = cluster_lba(v, cluster);
        for (s = 0u; s < v->sectors_per_cluster && remaining != 0u; ++s) {
            if (remaining >= SECTOR_BYTES) {
                /* Batch the whole-sector part of this cluster into one transfer:
                 * a 257 KiB image is 514 sectors, and one command per sector is
                 * the difference between a second and most of a minute. */
                uint32_t n = v->sectors_per_cluster - s;
                if (n > remaining / SECTOR_BYTES) n = remaining / SECTOR_BYTES;
                if (dev_read(v, lba + s, n, dst + done) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
                done += n * SECTOR_BYTES;
                remaining -= n * SECTOR_BYTES;
                s += n - 1u;
            } else {
                /* Final partial sector: bounce it so we never write past the
                 * file's last byte into the caller's buffer. */
                if (dev_read(v, lba + s, 1u, g_sec) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
                mem_copy(dst + done, g_sec, remaining);
                done += remaining;
                remaining = 0u;
            }
        }

        if (v->progress != 0) v->progress(v->progress_ctx, done, f.size);
        if (remaining == 0u) break;
        if (fat_next(v, cluster, &cluster) != MVII_FAT_OK) return MVII_FAT_ERR_IO;
    }

    return MVII_FAT_OK;
}

const char* mvii_fat_strerror(int rc)
{
    switch (rc) {
        case MVII_FAT_OK:            return "ok";
        case MVII_FAT_ERR_IO:        return "block read failed";
        case MVII_FAT_ERR_NOFS:      return "no FAT volume found";
        case MVII_FAT_ERR_NOTFOUND:  return "no such file or directory";
        case MVII_FAT_ERR_NOTDIR:    return "path component is not a directory";
        case MVII_FAT_ERR_ISDIR:     return "that is a directory";
        case MVII_FAT_ERR_TOOBIG:    return "file is larger than the scratch buffer";
        case MVII_FAT_ERR_PARAM:     return "bad argument";
        case MVII_FAT_ERR_CHAIN:     return "cluster chain is broken";
        default:                     return "unknown error";
    }
}
