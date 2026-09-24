#include "mt6592_bootstatus.h"
#include "mt6592_msdc.h"

static mt6592_boot_status_t g_status;
static uint8_t g_status_sector[MT6592_BOOT_STATUS_SIZE];
/* Non-zero while an eMMC write is in flight, so a log line emitted by the
 * storage driver cannot start a nested flush. Used by the console mirror
 * further down; declared here because mt6592_bootstatus_flush() holds it. */
static uint32_t g_log_flush_busy;

// Static compilation assertion checking structure bounds
typedef char mt6592_boot_status_size_check[(sizeof(mt6592_boot_status_t) <= MT6592_BOOT_STATUS_SIZE) ? 1 : -1];

static void zero_bytes(void* ptr, uint32_t len) {
    uint8_t* p = (uint8_t*)ptr;
    while (len--) *p++ = 0;
}

static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t len) {
    while (len--) *dst++ = *src++;
}

static void copy_message(char* dst, uint32_t dst_len, const char* src) {
    uint32_t i = 0;
    if (dst_len == 0u) return;
    while (src && src[i] && i + 1u < dst_len) {
        dst[i] = src[i];
        ++i;
    }
    while (i < dst_len) dst[i++] = 0;
}

static int readable_handoff_pointer(uint32_t addr, uint32_t size) {
    if ((addr & 3u) != 0u || size < 4u) return 0;
    if (addr >= 0x10000000u && addr < 0x12000000u && size <= 0x02000000u - (addr - 0x10000000u)) return 1;
    if (addr >= 0x80000000u && addr < 0xc0000000u && size <= 0x40000000u - (addr - 0x80000000u)) return 1;
    return 0;
}

void mt6592_bootstatus_init(void) {
    zero_bytes(&g_status, sizeof(g_status));
    g_status.magic = MT6592_BOOT_STATUS_MAGIC;         // This assigns 0x5342374d ("MB7S")
    g_status.version = MT6592_BOOT_STATUS_VERSION;
    g_status.record_size = sizeof(g_status);
}

void mt6592_bootstatus_set_stage(uint32_t stage) {
    g_status.stage = stage;
}

void mt6592_bootstatus_set_message(const char* message) {
    copy_message(g_status.message, MT6592_BOOT_STATUS_MESSAGE_SIZE, message);
}

void mt6592_bootstatus_set_handoff(const mt6592_boot_handoff_t* handoff) {
    uint32_t fb_addr = 0;
    uint32_t fb_width = 0;
    uint32_t fb_height = 0;
    uint32_t fb_pitch = 0;
    uint32_t fb_bpp = 0;
    
    if (!mt6592_bootinfo_is_valid(handoff)) return;
    
    g_status.entry_r0 = handoff->entry_r0;
    g_status.entry_r1 = handoff->entry_r1;
    g_status.entry_r2 = handoff->entry_r2;
    g_status.entry_r3 = handoff->entry_r3;
    g_status.bootarg_addr = handoff->entry_r1;
    g_status.bootarg_size = handoff->entry_r2;
    
    if (readable_handoff_pointer(handoff->entry_r1, handoff->entry_r2)) {
        volatile const uint32_t* words = (volatile const uint32_t*)(uintptr_t)handoff->entry_r1;
        uint32_t count = handoff->entry_r2 / 4u;
        if (count > 6u) count = 6u;
        for (uint32_t i = 0; i < count; ++i) g_status.bootarg_words[i] = words[i];
    }
    
    if (mt6592_bootinfo_framebuffer(handoff, &fb_addr, &fb_width, &fb_height, &fb_pitch, &fb_bpp)) {
        mt6592_bootstatus_set_framebuffer(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp, 0u);
    }
}

void mt6592_bootstatus_set_image_base(uint32_t image_base) {
    g_status.image_base = image_base;
}

void mt6592_bootstatus_set_framebuffer(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp,
                                       uint32_t bound) {
    g_status.fb_addr = addr;
    g_status.fb_width = width;
    g_status.fb_height = height;
    g_status.fb_pitch = pitch;
    g_status.fb_bpp = bpp;
    if (bound) {
        g_status.flags |= MT6592_BOOT_STATUS_FLAG_FRAMEBUFFER_BOUND;
    } else {
        g_status.flags &= ~MT6592_BOOT_STATUS_FLAG_FRAMEBUFFER_BOUND;
    }
}

void mt6592_bootstatus_set_storage(uint32_t count, uint32_t boot_volume_present, uint32_t boot_volume_serial) {
    g_status.storage_count = count;
    g_status.boot_volume_present = boot_volume_present;
    g_status.boot_volume_serial = boot_volume_serial;
    if (count != 0u) g_status.flags |= MT6592_BOOT_STATUS_FLAG_STORAGE_READY;
    if (boot_volume_present != 0u) {
        g_status.flags |= MT6592_BOOT_STATUS_FLAG_BOOT_VOLUME;
    } else {
        g_status.flags &= ~MT6592_BOOT_STATUS_FLAG_BOOT_VOLUME;
    }
}

void mt6592_bootstatus_set_visible(uint32_t mmsys_cg_con0, uint32_t mmsys_cg_con1, uint32_t lcm_rst_b,
                                   uint32_t disp_pwm_probe_base) {
    g_status.mmsys_cg_con0 = mmsys_cg_con0;
    g_status.mmsys_cg_con1 = mmsys_cg_con1;
    g_status.mmsys_lcm_rst_b = lcm_rst_b;
    g_status.disp_pwm_probe_base = disp_pwm_probe_base;
}

void mt6592_bootstatus_set_visible_extra(uint32_t pwm_gpio_mode, uint32_t bls_en, uint32_t bls_con0,
                                         uint32_t bls_con1, uint32_t bls_debug, uint32_t disp_pwm_con0,
                                         uint32_t disp_pwm_con1, uint32_t pwm_gpio_data) {
    g_status.pwm_gpio_mode = pwm_gpio_mode;
    g_status.bls_en = bls_en;
    g_status.bls_con0 = bls_con0;
    g_status.bls_con1 = bls_con1;
    g_status.bls_debug = bls_debug;
    g_status.disp_pwm_con0 = disp_pwm_con0;
    g_status.disp_pwm_con1 = disp_pwm_con1;
    g_status.pwm_gpio_data = pwm_gpio_data;
}

void mt6592_bootstatus_set_power(uint32_t pwrap_status, uint32_t pmic_cid, uint32_t ldo_status0,
                                 uint32_t ldo_status1, uint32_t vio28_con0, uint32_t vgp1_con7,
                                 uint32_t vgp1_con28, uint32_t vio18_con49) {
    g_status.pmic_pwrap_status = pwrap_status;
    g_status.pmic_cid = pmic_cid;
    g_status.pmic_ldo_status0 = ldo_status0;
    g_status.pmic_ldo_status1 = ldo_status1;
    g_status.pmic_vio28_con0 = vio28_con0;
    g_status.pmic_vgp1_con7 = vgp1_con7;
    g_status.pmic_vgp1_con28 = vgp1_con28;
    g_status.pmic_vio18_con49 = vio18_con49;
}

void mt6592_bootstatus_set_error(uint32_t error) {
    g_status.last_error = error;
}

static void console_ring_commit(void);

int mt6592_bootstatus_flush(void) {
    /* Held across the whole write: should the storage driver itself log, the
     * line must not come straight back here and start a nested flush. */
    const uint32_t was_busy = g_log_flush_busy;
    int rc;

    zero_bytes(g_status_sector, sizeof(g_status_sector));
    copy_bytes(g_status_sector, (const uint8_t*)&g_status, sizeof(g_status));

    g_log_flush_busy = 1u;
    // Write out tracking sector to MMC
    rc = mt6592_emmc_write_user(MT6592_BOOT_STATUS_OFFSET, g_status_sector, sizeof(g_status_sector));
    /* The ring holds everything the 320-byte message field could not; a caller
     * that wants the status on eMMC always wants the log that led to it, and
     * every existing caller is a boot milestone, a panic or a trap. */
    console_ring_commit();
    g_log_flush_busy = was_busy;
    return rc;
}

/* ---- runtime console mirror --------------------------------------------- */

static char g_log_line[160];
static uint32_t g_log_line_len;
static uint32_t g_log_line_count;
/* When clear, completed lines stop reaching eMMC and only accumulate in RAM.
 * See mt6592_bootstatus_console_set_autoflush() in the header for why. */
static uint32_t g_autoflush = 1u;
/* When clear, console output is not recorded in the ring at all -- not even in RAM.
 * See mt6592_bootstatus_console_set_mirror() in the header. */
static uint32_t g_mirror = 1u;

#ifdef MVII_ARMV7_FULL_OS
/* Full console ring on eMMC (see header constants). Only the full-OS image
 * carries it — the small loader/flash payloads have tight RAM budgets and
 * never feed the mirror anyway.
 *
 * The ring is mirrored in RAM in its entirety and eMMC only ever sees it at a
 * commit point. An MSDC transaction is milliseconds of stalled CPU; doing one
 * per 512 characters of output, as this did when the ring was a single sector
 * streamed straight to storage, freezes whatever is running. Sixty-three KiB of
 * .bss buys the ability to log freely from code that must not block. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t data_size;
    uint32_t write_pos; /* total bytes ever written; position = write_pos % data_size */
} console_ring_header_t;

typedef char console_ring_ram_size_check[((MT6592_CONSOLE_RING_RAM_BYTES % 512u) == 0u &&
                                          MT6592_CONSOLE_RING_RAM_BYTES <= MT6592_CONSOLE_RING_DATA_SIZE &&
                                          MT6592_CONSOLE_RING_RAM_BYTES > 0u)
                                             ? 1
                                             : -1];

static uint8_t g_ring_data[MT6592_CONSOLE_RING_RAM_BYTES];
static uint32_t g_ring_pos;     /* total bytes ever written */
static uint32_t g_ring_flushed; /* how much of that eMMC has seen */

static void console_ring_flush_header(void) {
    uint8_t sector[512];
    console_ring_header_t hdr;
    hdr.magic = MT6592_CONSOLE_RING_MAGIC;
    hdr.version = MT6592_CONSOLE_RING_VERSION;
    hdr.data_size = MT6592_CONSOLE_RING_DATA_SIZE;
    hdr.write_pos = g_ring_pos;
    zero_bytes(sector, sizeof(sector));
    copy_bytes(sector, (const uint8_t*)&hdr, sizeof(hdr));
    (void)mt6592_emmc_write_user(MT6592_CONSOLE_RING_OFFSET, sector, sizeof(sector));
}

/* Write back every 512-byte block the RAM ring has touched since the last
 * commit, oldest first, so the newest copy of any block lands last. Output
 * produced since the last commit that exceeds the RAM mirror has already been
 * overwritten, so clamp rather than replay laps that no longer exist.
 *
 * The two rings can differ in size, hence the separate offsets: the position
 * within the RAM mirror is not the position within the eMMC ring. */
static void console_ring_flush_data(void) {
    uint32_t pending = g_ring_pos - g_ring_flushed;
    uint32_t start;

    if (pending == 0u) return;
    if (pending > MT6592_CONSOLE_RING_RAM_BYTES) pending = MT6592_CONSOLE_RING_RAM_BYTES;
    start = g_ring_pos - pending;
    start -= start % 512u; /* the partly filled block at the low end still needs writing */

    while (start < g_ring_pos) {
        const uint32_t emmc_off = start % MT6592_CONSOLE_RING_DATA_SIZE;
        const uint32_t ram_off = start % MT6592_CONSOLE_RING_RAM_BYTES;
        (void)mt6592_emmc_write_user(MT6592_CONSOLE_RING_DATA_OFFSET + emmc_off, &g_ring_data[ram_off], 512u);
        start += 512u;
    }
    g_ring_flushed = g_ring_pos;
}

static void console_ring_putc(char c) {
    /* Mirror off: record nothing, so g_ring_pos does not advance and no later
     * commit can be provoked. This is what lets a reader print the ring without
     * consuming it -- see mt6592_bootstatus_console_set_mirror(). */
    if (!g_mirror) return;
    /* About to lap the mirror. With autoflush on this would silently drop boot
     * output that storage has not seen, which is the one thing the boot log
     * must not do, so pay for the commit now. With it off the ring is doing its
     * job: the oldest line goes and no frame is stalled. */
    if (g_autoflush && !g_log_flush_busy && (g_ring_pos - g_ring_flushed) >= MT6592_CONSOLE_RING_RAM_BYTES) {
        console_ring_commit();
    }
    g_ring_data[g_ring_pos % MT6592_CONSOLE_RING_RAM_BYTES] = (uint8_t)c;
    ++g_ring_pos;
}

static void console_ring_commit(void) {
    console_ring_flush_data();
    console_ring_flush_header();
}
#else
static void console_ring_putc(char c) {
    (void)c;
}
static void console_ring_commit(void) {
}
#endif

static uint32_t message_len(void) {
    uint32_t n = 0;
    while (n < MT6592_BOOT_STATUS_MESSAGE_SIZE && g_status.message[n]) ++n;
    return n;
}

/* Append one line to the message field, separated by " | ". When the field is
 * full, the oldest bytes are shifted out so the newest lines always survive. */
static void message_append_line(const char* line) {
    uint32_t add = 0;
    while (line[add]) ++add;
    if (add == 0u) return;
    if (add > MT6592_BOOT_STATUS_MESSAGE_SIZE - 1u) {
        line += add - (MT6592_BOOT_STATUS_MESSAGE_SIZE - 1u);
        add = MT6592_BOOT_STATUS_MESSAGE_SIZE - 1u;
    }
    uint32_t cur = message_len();
    uint32_t sep = cur != 0u ? 3u : 0u;
    if (cur + sep + add + 1u > MT6592_BOOT_STATUS_MESSAGE_SIZE) {
        uint32_t drop = cur + sep + add + 1u - MT6592_BOOT_STATUS_MESSAGE_SIZE;
        if (drop >= cur) {
            cur = 0u;
            sep = 0u;
        } else {
            for (uint32_t i = 0; i + drop < cur; ++i) g_status.message[i] = g_status.message[i + drop];
            cur -= drop;
        }
    }
    if (sep) {
        g_status.message[cur++] = ' ';
        g_status.message[cur++] = '|';
        g_status.message[cur++] = ' ';
    }
    for (uint32_t i = 0; i < add; ++i) g_status.message[cur++] = line[i];
    while (cur < MT6592_BOOT_STATUS_MESSAGE_SIZE) g_status.message[cur++] = 0;
}

/* 0x12xx is the MVII minimal LK's milestone range (see mt6592_bootstatus.h). */
static int stage_is_bootloader(uint32_t stage) {
    return stage >= 0x1200u && stage <= 0x12ffu;
}

static void log_commit_line(void) {
    if (g_log_line_len == 0u) return;
    g_log_line[g_log_line_len] = 0;
    g_log_line_len = 0u;
    message_append_line(g_log_line);
    ++g_log_line_count;
    g_status.sequence = g_log_line_count;
    /*
     * The heartbeat is the runtime's, so do not stamp it over a bootloader's
     * stage. The minimal LK sets an explicit 0x12xx milestone and then keeps
     * logging; with an unconditional overwrite here, the record read back off a
     * board that died in the LK said "MVII ARM runtime heartbeat" — naming a
     * stage the board never reached, and discarding the one it did. A caller
     * that has claimed a stage outside the runtime's own range owns the field
     * until it says otherwise.
     */
    if (!stage_is_bootloader(g_status.stage)) {
        g_status.stage = g_log_line_count == 1u ? MT6592_BOOT_STATUS_STAGE_RUNTIME_ENTERED
                                                : MT6592_BOOT_STATUS_STAGE_RUNTIME_HEARTBEAT;
    }
    if (!g_autoflush) return; /* runtime: RAM only, committed at a safe point */
    if (g_log_flush_busy) return;
    g_log_flush_busy = 1u;
    /* Every line early in boot; after that, sample so a chatty boot does not
     * turn every line into an eMMC write. */
    if (g_log_line_count <= 512u || (g_log_line_count & 15u) == 0u) {
        (void)mt6592_bootstatus_flush();
    }
    g_log_flush_busy = 0u;
}

void mt6592_bootstatus_console_set_autoflush(int enabled) {
    g_autoflush = enabled ? 1u : 0u;
}

void mt6592_bootstatus_console_set_mirror(int enabled) {
    g_mirror = enabled ? 1u : 0u;
}

int mt6592_bootstatus_console_flush(void) {
    if (g_status.magic != MT6592_BOOT_STATUS_MAGIC) return -1;
    if (g_log_flush_busy) return -1; /* already inside a flush; nothing to add */
    return mt6592_bootstatus_flush();
}

void mt6592_bootstatus_log_char(char c) {
    if (g_status.magic != MT6592_BOOT_STATUS_MAGIC) return;
    if (c == '\r') return;
    if (c == '\n') {
        console_ring_putc('\n');
        log_commit_line();
        return;
    }
    if (c < 0x20 || c > 0x7e) c = '.';
    console_ring_putc(c);
    g_log_line[g_log_line_len++] = c;
    if (g_log_line_len >= sizeof(g_log_line) - 1u) log_commit_line();
}

void mt6592_bootstatus_log_text(const char* text) {
    if (!text) return;
    while (*text) mt6592_bootstatus_log_char(*text++);
}

int mt6592_bootstatus_publish_flash_pending(void) {
    mt6592_bootstatus_init();
    mt6592_bootstatus_set_stage(MT6592_BOOT_STATUS_STAGE_FLASH_PENDING);
    mt6592_bootstatus_set_message("LK image flashed; waiting for MVII LK");
    g_status.flags |= MT6592_BOOT_STATUS_FLAG_WRITTEN_BY_FLASH;
    
    // FIX: Force flush to flash block storage so it doesn't get lost on reset/handoff
    return mt6592_bootstatus_flush();
}
