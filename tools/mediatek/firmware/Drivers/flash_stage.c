#include <stdint.h>

#include "backlight.h"
#include "lcd_drv.h"
#include "mt6592_bootstatus.h"
#include "mt6592_dbgflag.h"
#include "mt6592_disp_hw.h"
#include "mt6592_led.h"
#include "mt6592_msdc.h"
#include "mt6592_pwrap.h"
#include "mt6592_uart.h"
#include "mt6592_wifi_sdio.h"
#include "panel_bringup.h"
#include "stage1.h"

enum {
    FRAME_HEADER_SIZE = 20u,
    FRAME_VERSION = 1u,
    FRAME_HELLO = 0x0001u,
    FRAME_START = 0x0002u,
    FRAME_DATA = 0x0003u,
    FRAME_FINISH = 0x0004u,
    FRAME_READ = 0x0005u,
    FRAME_REBOOT = 0x0006u,
    FRAME_RUN_STAGE1 = 0x0007u,
    FRAME_COMMAND = 0x0008u,
    FRAME_ACK = 0x8001u,
    FRAME_DONE = 0x8002u,
    FRAME_LOG = 0x8003u,
    FRAME_PROGRESS = 0x8004u,
    FRAME_READ_DATA = 0x8005u,
    FRAME_STAGE1_STATUS = 0x8006u,
    FRAME_ERROR = 0x80ffu,
    MAX_FEED_CHUNK = 64u * 1024u,
    MAX_FRAME_PAYLOAD = MAX_FEED_CHUNK + 16u,
    EMMC_BLOCK_SIZE = 512u,
};

enum {
    FLASH_OK = 0,
    FLASH_ERR_FRAME = 0x1001u,
    FLASH_ERR_START = 0x1002u,
    FLASH_ERR_SEQUENCE = 0x1003u,
    FLASH_ERR_MSDC = 0x1004u,
    FLASH_ERR_UNSUPPORTED = 0x1005u,
    FLASH_ERR_BOOT = 0x1006u,
    FEED_FLAG_BOOT_AFTER_FLASH = 1u << 0, /* deprecated: rejected so flashing never mirrors into DRAM */
    FEED_FLAG_REBOOT_AFTER_FLASH = 1u << 1,
    FEED_FLAG_ENABLE_BOOT1 = 1u << 2,
    FEED_FLAG_SPARSE_STREAM = 1u << 3,
    FEED_FLAG_BOOT_STATUS = 1u << 4,
};

typedef int (*usb_data_fn)(void*, uint32_t);
typedef int (*usb_flush_fn)(void);
typedef void (*usb_response_fn)(int, int, int);

extern uint8_t __bss_start;
extern uint8_t __bss_end;

static uint8_t g_frame_payload[MAX_FRAME_PAYLOAD];
static usb_data_fn g_usb_get;
static usb_data_fn g_usb_put;
static usb_flush_fn g_usb_flush;
static uint64_t g_target_start;
static uint64_t g_image_size;
static uint64_t g_transfer_len;
static uint64_t g_next_stream_offset;
static uint32_t g_emmc_part;
static uint32_t g_start_flags;
static uint32_t g_started;

/* Set once the display MTCMOS domain and MMSYS gates are up. Re-running the
 * "panel" command in the same session skips re-gating them: it is dead time,
 * and cycling a domain the DSI host is already sitting in is a variable this
 * console exists to remove. */
static uint32_t g_panel_clocks_done;

static void mt6592_watchdog_disable(void) {
    *(volatile uint32_t*)(uintptr_t)0x10007000u = 0x22000000u;
    *(volatile uint32_t*)(uintptr_t)0x10000500u = 0x22000000u;
}

static void mt6592_clear_usbdl_flag(void) {
    *(volatile uint32_t*)(uintptr_t)0x10002050u = 0xad98u;
    *(volatile uint32_t*)(uintptr_t)0x10002058u = 0u;
    *(volatile uint32_t*)(uintptr_t)0x10002050u = 0u;
    *(volatile uint32_t*)(uintptr_t)0x10002030u = 0u;
}

static void mt6592_set_brom_usbdl_flag(void) {
    volatile uint32_t* misc_lock = (volatile uint32_t*)(uintptr_t)0x10002050u;
    volatile uint32_t* reset_ctl = (volatile uint32_t*)(uintptr_t)0x10002058u;
    volatile uint32_t* usbdl = (volatile uint32_t*)(uintptr_t)0x10002030u;
    *misc_lock = 0xad98u;
    *reset_ctl = *reset_ctl | 1u;
    *misc_lock = 0u;
    *usbdl = 0x444cfffdu;
}

static void mt6592_watchdog_reboot(void) {
    volatile uint32_t* wdt = (volatile uint32_t*)(uintptr_t)0x10007000u;
    wdt[8u / 4u] = 0x1971u;
    wdt[0u / 4u] = 0x22000014u;
    wdt[0x14u / 4u] = 0x1209u;
    for (;;) {
    }
}

static void memzero(void* ptr, uint32_t len) {
    uint8_t* p = (uint8_t*)ptr;
    while (len--) *p++ = 0;
}

static uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static uint32_t command_copy(char* out, uint32_t out_len, const uint8_t* data, uint32_t len) {
    uint32_t n = 0;
    if (out_len == 0u) return 0u;
    while (n + 1u < out_len && n < len) {
        char c = (char)data[n];
        if (c == 0 || c == '\r' || c == '\n') break;
        out[n] = lower_ascii(c);
        ++n;
    }
    while (n != 0u && (out[n - 1u] == ' ' || out[n - 1u] == '\t')) --n;
    out[n] = 0;
    return n;
}

static int bytes_equal_text(const uint8_t* data, uint32_t len, const char* text) {
    uint32_t n = str_len(text);
    if (!data || len != n) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (data[i] != (uint8_t)text[i]) return 0;
    }
    return 1;
}

static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t len) {
    while (len--) *dst++ = *src++;
}

static uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t* p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t* p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t crc32_ieee(const uint8_t* data, uint32_t len) {
    static const uint32_t table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 0x0fu];
        crc = (crc >> 4) ^ table[crc & 0x0fu];
    }
    return ~crc;
}

static void usb_init(void) {
    volatile uint32_t* usbdl = (volatile uint32_t*)(uintptr_t)0x0000a564u;
    uint32_t control = usbdl[0];
    if (control) {
        *(volatile uint32_t*)(uintptr_t)(control + 8u) = usbdl[2];
    }
    g_usb_get = (usb_data_fn)(uintptr_t)usbdl[1];
    g_usb_put = (usb_data_fn)(uintptr_t)usbdl[2];
    g_usb_flush = (usb_flush_fn)(uintptr_t)usbdl[3];

    ((usb_response_fn)(uintptr_t)0x0000535du)(1, 0, 1);
}

/* Re-establish the BROM usbdl VCOM callbacks after heavy display clock/DSI
 * changes in the live stage1 path. Some boards drop the "configured" state
 * on the host after MMSYS/DSI programming unless we poke the response hook
 * again.
 *
 * We refresh the function pointers from the BROM vector table (still valid
 * in SRAM) and re-ack. This is the 1:1 equivalent of what early DA code does
 * after clock setup in the reference flows. */
static void usb_reinit_for_vcom(void) {
    volatile uint32_t* usbdl = (volatile uint32_t*)(uintptr_t)0x0000a564u;
    uint32_t control = usbdl[0];
    if (control) {
        *(volatile uint32_t*)(uintptr_t)(control + 8u) = usbdl[2];
    }
    g_usb_get = (usb_data_fn)(uintptr_t)usbdl[1];
    g_usb_put = (usb_data_fn)(uintptr_t)usbdl[2];
    g_usb_flush = (usb_flush_fn)(uintptr_t)usbdl[3];

    /* Re-ack to keep the download session configured on the host. */
    ((usb_response_fn)(uintptr_t)0x0000535du)(1, 0, 1);
}

static void usb_read_exact(void* data, uint32_t len) {
    if (len == 0) return;
    (void)g_usb_get(data, len);
}

static void usb_write_exact(const void* data, uint32_t len) {
    if (len != 0) {
        (void)g_usb_put((void*)(uintptr_t)data, len);
    }
    (void)g_usb_flush();
}

static void send_frame(uint16_t type, uint32_t seq, const uint8_t* payload, uint32_t payload_len) {
    uint8_t header[FRAME_HEADER_SIZE];
    header[0] = 'M';
    header[1] = '7';
    header[2] = 'F';
    header[3] = '1';
    put_le16(header + 4, FRAME_VERSION);
    put_le16(header + 6, type);
    put_le32(header + 8, seq);
    put_le32(header + 12, payload_len);
    put_le32(header + 16, crc32_ieee(payload, payload_len));
    usb_write_exact(header, FRAME_HEADER_SIZE);
    usb_write_exact(payload, payload_len);
}

static void send_status(uint16_t type, uint32_t seq, uint32_t status, const char* message) {
    uint8_t payload[132];
    uint32_t msg_len = str_len(message);
    if (msg_len > sizeof(payload) - 5u) msg_len = sizeof(payload) - 5u;
    put_le32(payload, status);
    copy_bytes(payload + 4, (const uint8_t*)message, msg_len);
    payload[4 + msg_len] = 0;
    send_frame(type, seq, payload, 5u + msg_len);
}

static void send_log(const char* message) {
    send_frame(FRAME_LOG, 0, (const uint8_t*)message, str_len(message));
}

static void send_hello(uint32_t seq) {
    uint8_t payload[48];
    const char name[] = "MVIIFlash MT6592 USER PIO";
    put_le32(payload, MAX_FEED_CHUNK);
    put_le32(payload + 4, EMMC_BLOCK_SIZE);
    put_le32(payload + 8, 0);
    copy_bytes(payload + 12, (const uint8_t*)name, sizeof(name));
    send_frame(FRAME_HELLO, seq, payload, 12u + (uint32_t)sizeof(name));
}

static void send_progress(uint64_t done, uint64_t total) {
    uint8_t payload[16];
    put_le64(payload, done);
    put_le64(payload + 8, total);
    send_frame(FRAME_PROGRESS, 0, payload, sizeof(payload));
}

static char* append_text(char* out, char* end, const char* text) {
    while (out + 1 < end && text && *text) {
        *out++ = *text++;
    }
    return out;
}

static char* append_hex32(char* out, char* end, uint32_t value) {
    static const char hex[] = "0123456789abcdef";
    out = append_text(out, end, "0x");
    for (int shift = 28; shift >= 0 && out + 1 < end; shift -= 4) {
        *out++ = hex[(value >> (uint32_t)shift) & 0xfu];
    }
    return out;
}

static char* append_field_hex32(char* out, char* end, const char* name, uint32_t value) {
    out = append_text(out, end, name);
    out = append_text(out, end, "=");
    return append_hex32(out, end, value);
}

static void send_probe_line(char* line, char* out) {
    if (out < line) out = line;
    if (out >= line + 256u) out = line + 255u;
    *out = 0;
    send_log(line);
}

static volatile uint32_t* mmio32(uint32_t addr) {
    return (volatile uint32_t*)(uintptr_t)addr;
}

static uint32_t read32(uint32_t addr) {
    return *mmio32(addr);
}

static uint32_t gpio_group_addr(uint32_t base, uint32_t group) {
    return MTK_GPIO_BASE + base + group * MTK_GPIO_STRIDE;
}

struct frame {
    uint16_t type;
    uint32_t seq;
    uint32_t len;
};

static int read_frame(struct frame* out) {
    static const uint8_t magic[4] = {'M', '7', 'F', '1'};
    uint8_t b = 0;
    uint8_t header[FRAME_HEADER_SIZE];
    uint32_t matched = 0;
    uint32_t len;
    uint32_t want_crc;
    uint32_t got_crc;

    mt6592_watchdog_disable();
    for (;;) {
        mt6592_watchdog_disable();
        usb_read_exact(&b, 1);
        if (b == magic[matched]) {
            ++matched;
            if (matched == 4u) break;
        } else {
            matched = (b == magic[0]) ? 1u : 0u;
        }
    }
    header[0] = magic[0];
    header[1] = magic[1];
    header[2] = magic[2];
    header[3] = magic[3];
    mt6592_watchdog_disable();
    usb_read_exact(header + 4, FRAME_HEADER_SIZE - 4u);

    if (get_le16(header + 4) != FRAME_VERSION) return -1;
    out->type = get_le16(header + 6);
    out->seq = get_le32(header + 8);
    len = get_le32(header + 12);
    want_crc = get_le32(header + 16);
    if (len > MAX_FRAME_PAYLOAD) return -2;
    if (len != 0) {
        mt6592_watchdog_disable();
        usb_read_exact(g_frame_payload, len);
    }
    mt6592_watchdog_disable();
    got_crc = crc32_ieee(g_frame_payload, len);
    if (got_crc != want_crc) return -3;
    out->len = len;
    return 0;
}

static void report_msdc_error(uint32_t seq, int err, const char* prefix) {
    char message[256];
    char* out = message;
    char* end = message + sizeof(message);
    const mt6592_msdc_diag_t* diag = mt6592_msdc_diag();

    out = append_text(out, end, prefix);
    out = append_text(out, end, " err=");
    out = append_hex32(out, end, (uint32_t)err);
    out = append_text(out, end, " st=");
    out = append_hex32(out, end, diag->stage);
    out = append_text(out, end, " cmd=");
    out = append_hex32(out, end, diag->cmd);
    out = append_text(out, end, " arg=");
    out = append_hex32(out, end, diag->arg);
    out = append_text(out, end, " int=");
    out = append_hex32(out, end, diag->status);
    out = append_text(out, end, " resp=");
    out = append_hex32(out, end, diag->resp);
    out = append_text(out, end, " cfg=");
    out = append_hex32(out, end, diag->cfg);
    out = append_text(out, end, " clk=");
    out = append_hex32(out, end, diag->clk_cfg2);
    out = append_text(out, end, "/");
    out = append_hex32(out, end, diag->peri_sta);
    out = append_text(out, end, " pll=");
    out = append_hex32(out, end, diag->pll_con0);
    out = append_text(out, end, "/");
    out = append_hex32(out, end, diag->pll_pwr);
    out = append_text(out, end, " gpio=");
    out = append_hex32(out, end, diag->gpio_clk);
    out = append_text(out, end, "/");
    out = append_hex32(out, end, diag->gpio_cmd);
    out = append_text(out, end, "/");
    out = append_hex32(out, end, diag->gpio_dat);
    if (out >= end) out = end - 1;
    *out = 0;

    mt6592_uart_puts(message);
    mt6592_uart_puts("\n");
    send_log(message);
    send_status(FRAME_ERROR, seq, FLASH_ERR_MSDC, message);
}

static int feed_part_is_supported(uint32_t part) {
    return part == MT6592_EMMC_USER_PART || part == MT6592_EMMC_BOOT1_PART || part == MT6592_EMMC_BOOT2_PART ||
           part == 0u;
}

static void handle_start(const struct frame* f) {
    uint32_t part;
    uint32_t block_size;
    uint32_t chunk_size;
    int err;

    if (f->len < 72u) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_START, "START payload is truncated");
        return;
    }

    g_target_start = get_le64(g_frame_payload);
    g_image_size = get_le64(g_frame_payload + 8);
    g_transfer_len = get_le64(g_frame_payload + 16);
    part = get_le32(g_frame_payload + 24);
    block_size = get_le32(g_frame_payload + 28);
    chunk_size = get_le32(g_frame_payload + 32);
    g_start_flags = get_le32(g_frame_payload + 36);

    if ((g_start_flags & FEED_FLAG_BOOT_AFTER_FLASH) != 0) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_UNSUPPORTED,
                    "DRAM handoff during flashing is disabled");
        return;
    }
    if (!feed_part_is_supported(part)) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_UNSUPPORTED, "MVIIFlash supports EMMC USER/BOOT1/BOOT2 only");
        return;
    }
    if (block_size != EMMC_BLOCK_SIZE || chunk_size < EMMC_BLOCK_SIZE || chunk_size > MAX_FEED_CHUNK ||
        (chunk_size & (EMMC_BLOCK_SIZE - 1u)) != 0) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_START, "START block/chunk geometry is invalid");
        return;
    }
    if ((g_target_start & (uint64_t)(EMMC_BLOCK_SIZE - 1u)) != 0 ||
        (g_transfer_len & (uint64_t)(EMMC_BLOCK_SIZE - 1u)) != 0) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_START, "START offset/length must be 512-byte aligned");
        return;
    }
    if (g_image_size == 0 || g_image_size > g_transfer_len) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_START, "START image length is invalid");
        return;
    }

    mt6592_watchdog_disable();
    send_log("initializing MT6592 MSDC0/eMMC");
    err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) {
        report_msdc_error(f->seq, err, "MSDC/eMMC init failed");
        return;
    }

    g_next_stream_offset = 0;
    g_emmc_part = part;
    g_started = 1;
    send_status(FRAME_ACK, f->seq, FLASH_OK, "eMMC ready");
}

static void handle_data(const struct frame* f) {
    uint64_t target;
    uint64_t stream;
    uint32_t data_len;
    uint32_t sparse;
    int err;

    if (!g_started) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_SEQUENCE, "DATA before START");
        return;
    }
    if (f->len < 16u) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_FRAME, "DATA payload is truncated");
        return;
    }

    target = get_le64(g_frame_payload);
    stream = get_le64(g_frame_payload + 8);
    data_len = f->len - 16u;
    sparse = (g_start_flags & FEED_FLAG_SPARSE_STREAM) != 0;

    if ((!sparse && stream != g_next_stream_offset) || (sparse && stream < g_next_stream_offset) ||
        target != g_target_start + stream || stream + data_len > g_transfer_len) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_SEQUENCE, "DATA offset sequence mismatch");
        return;
    }
    if ((data_len & (EMMC_BLOCK_SIZE - 1u)) != 0) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_FRAME, "DATA length must be 512-byte aligned");
        return;
    }

    mt6592_watchdog_disable();
    if (data_len != 0u) {
        err = mt6592_emmc_write_part(g_emmc_part, target, g_frame_payload + 16u, data_len);
        if (err != MT6592_MSDC_OK) {
            report_msdc_error(f->seq, err, "eMMC block write failed");
            return;
        }
    }
    mt6592_watchdog_disable();

    if (sparse) {
        g_next_stream_offset = stream + data_len;
    } else {
        g_next_stream_offset += data_len;
    }
    send_progress(g_next_stream_offset, g_transfer_len);
    send_status(FRAME_ACK, f->seq, FLASH_OK, data_len == 0u ? "skipped" : "written");
}

static int should_write_boot_status_for_reboot(void) {
    return (g_start_flags & FEED_FLAG_BOOT_STATUS) != 0u && g_emmc_part == MT6592_EMMC_USER_PART;
}

static void handle_finish(const struct frame* f) {
    uint64_t expected;
    int err;
    if (f->len < 8u) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_FRAME, "FINISH payload is truncated");
        return;
    }
    expected = get_le64(g_frame_payload);
    if (!g_started || expected != g_transfer_len || g_next_stream_offset != g_transfer_len) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_SEQUENCE, "FINISH length mismatch");
        return;
    }
    if ((g_start_flags & FEED_FLAG_ENABLE_BOOT1) != 0) {
        send_log("enabling eMMC BOOT1 hardware boot partition");
        err = mt6592_emmc_enable_boot_part(MT6592_EMMC_BOOT1_PART, 1u);
        if (err != MT6592_MSDC_OK) {
            report_msdc_error(f->seq, err, "eMMC BOOT1 boot-enable failed");
            return;
        }
        send_log("eMMC BOOT1 boot partition enabled");
    }
    if ((g_start_flags & FEED_FLAG_REBOOT_AFTER_FLASH) != 0) {
        if (should_write_boot_status_for_reboot()) {
            send_log("writing MVII boot-status probe sector");
            err = mt6592_bootstatus_publish_flash_pending();
            if (err != MT6592_MSDC_OK) {
                send_log("MVII boot-status probe sector write failed; continuing reboot");
            }
        }
        send_log("clearing BROM USB-download reset flag");
        mt6592_clear_usbdl_flag();
        send_status(FRAME_DONE, f->seq, FLASH_OK, "flash complete; rebooting");
        for (volatile uint32_t i = 0; i < 1000000u; ++i) {
        }
        mt6592_watchdog_reboot();
        return;
    }
    send_status(FRAME_DONE, f->seq, FLASH_OK, "flash complete");
}

static void handle_read(const struct frame* f) {
    uint64_t offset;
    uint32_t length;
    uint32_t part;
    uint32_t block_size;
    int err;

    if (f->len < 20u) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_FRAME, "READ payload is truncated");
        return;
    }

    offset = get_le64(g_frame_payload);
    length = get_le32(g_frame_payload + 8u);
    part = get_le32(g_frame_payload + 12u);
    block_size = get_le32(g_frame_payload + 16u);
    if (!feed_part_is_supported(part)) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_UNSUPPORTED, "MVIIFlash supports EMMC USER/BOOT1/BOOT2 only");
        return;
    }
    if (block_size != EMMC_BLOCK_SIZE || length == 0u || length > MAX_FRAME_PAYLOAD - 12u ||
        (length & (EMMC_BLOCK_SIZE - 1u)) != 0 || (offset & (uint64_t)(EMMC_BLOCK_SIZE - 1u)) != 0) {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_FRAME, "READ offset/length geometry is invalid");
        return;
    }

    mt6592_watchdog_disable();
    send_log("initializing MT6592 MSDC0/eMMC for read");
    err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) {
        report_msdc_error(f->seq, err, "MSDC/eMMC init failed");
        return;
    }

    put_le64(g_frame_payload, offset);
    put_le32(g_frame_payload + 8u, length);
    err = mt6592_emmc_read_part(part, offset, g_frame_payload + 12u, length);
    if (err != MT6592_MSDC_OK) {
        report_msdc_error(f->seq, err, "eMMC block read failed");
        return;
    }
    send_frame(FRAME_READ_DATA, f->seq, g_frame_payload, 12u + length);
}

static void handle_reboot(const struct frame* f) {
    if (bytes_equal_text(g_frame_payload, f->len, "host reconnect via preloader")) {
        send_log("host requested payload reset through preloader");
        mt6592_clear_usbdl_flag();
        send_status(FRAME_DONE, f->seq, FLASH_OK, "rebooting through preloader");
        for (volatile uint32_t i = 0; i < 1000000u; ++i) {
        }
        mt6592_watchdog_reboot();
        return;
    }

    send_log("host requested payload reset to BROM");
    mt6592_set_brom_usbdl_flag();
    send_status(FRAME_DONE, f->seq, FLASH_OK, "rebooting to BROM");
    for (volatile uint32_t i = 0; i < 1000000u; ++i) {
    }
    mt6592_watchdog_reboot();
}

/*
 * Two commands, and that is the whole supported surface.
 *
 * The BROM is not a debug environment on this board. Its USB transport is the
 * BROM's own usbdl callback table, and it stops answering the moment a clock it
 * relies on is reprogrammed -- proven twice here, first by an AFE_DAC_CON0 write
 * and then by the `panel` command dropping the link mid-run. Anything that
 * actually exercises hardware therefore cannot be trusted to report its own
 * result from BROM.
 *
 * So the BROM's job is now the one thing it does reliably: write a sector and
 * reset. `debug mode` arms the one-shot flag that makes the LK start its live
 * console instead of booting on, and `boot` just resets. Every diagnostic that
 * used to be a menu entry here now lives in that console (`flash -dbg`), running
 * on a board with real clocks, real DRAM, and a USB device of our own.
 *
 * The legacy verbs are still wired up below rather than deleted: they are the
 * fallback if the console's gadget ever fails to enumerate, and a fallback that
 * has been removed is not a fallback. They are simply not advertised.
 */
static void handle_command_help(void) {
    send_log("commands: debug mode, boot");
    send_log("  debug mode = arm the one-shot debug flag in eMMC, then reboot. The LK finds the flag");
    send_log("               and serves a live console on the OTG port instead of booting the kernel.");
    send_log("               Wait for the board to come back up, then run: flash -dbg");
    send_log("  boot       = reboot straight into a normal boot");
    send_log("BROM is only reliable for eMMC writes and resets on this board, so nothing else is");
    send_log("offered here; the live console is where hardware gets poked. `flash -dbg` for commands.");
}

/* Arm the debug flag and reset. eMMC first: the flag is worthless if the write
 * silently fails, so the failure is reported and the reboot is skipped -- a
 * board that reboots into a normal boot when it was asked for a console is
 * confusing, but a board that says why is not. */
static void command_debug_mode(uint32_t seq) {
    int err;

    mt6592_watchdog_disable();
    send_log("debug mode: initializing eMMC to arm the debug flag");
    err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) {
        report_msdc_error(seq, err, "MSDC/eMMC init failed; debug flag not armed");
        return;
    }
    if (mt6592_dbgflag_arm(MT6592_DBG_MODE_CONSOLE) != 0) {
        send_status(FRAME_ERROR, seq, FLASH_ERR_MSDC, "debug flag write failed; not rebooting");
        return;
    }
    send_log("debug mode: flag armed; rebooting through the preloader");
    send_log("debug mode: when the board is back up, run `flash -dbg` to attach the live console");
    mt6592_clear_usbdl_flag();
    send_status(FRAME_DONE, seq, FLASH_OK, "debug flag armed; rebooting");
    for (volatile uint32_t i = 0; i < 1000000u; ++i) {
    }
    mt6592_watchdog_reboot();
}

/* The other half of the pair, and it has to undo what the first half arms.
 *
 * The debug flag is one-shot but it is only consumed by an LK that actually
 * runs, so a `debug mode` that never got as far as booting -- a flash that went
 * wrong, a power cycle back into BROM -- leaves it sitting armed. Rebooting on
 * top of that would land in the console, which is the opposite of what the
 * command says. So overwrite it with NORMAL.
 *
 * Best-effort, unlike `debug mode`: there the flag *is* the command and a failed
 * write must not reboot, whereas here the reboot is the command and a board that
 * cannot reach eMMC should still be able to boot. */
static void command_boot_now(uint32_t seq) {
    mt6592_watchdog_disable();
    send_log("boot: clearing any armed debug flag so this really is a normal boot");
    if (mt6592_emmc_user_init() != MT6592_MSDC_OK) {
        send_log("boot: eMMC init failed; booting anyway (a stale debug flag would still be honoured)");
    } else if (mt6592_dbgflag_arm(MT6592_DBG_MODE_NORMAL) != 0) {
        send_log("boot: debug flag write failed; booting anyway (a stale flag would still be honoured)");
    }
    send_log("boot: rebooting through the preloader into a normal boot");
    mt6592_clear_usbdl_flag();
    send_status(FRAME_DONE, seq, FLASH_OK, "rebooting");
    for (volatile uint32_t i = 0; i < 1000000u; ++i) {
    }
    mt6592_watchdog_reboot();
}

/* ── Live register/display console ──
 * Reading the eMMC boot-status ring costs a BROM handshake, and that handshake
 * resets the board: every log so far has been a snapshot of the instant the run
 * was interrupted. These commands run against a board that stays up, so a
 * register change can be tried and seen in seconds. */

static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

/* Parse a hex value, with or without an 0x prefix. Advances *sp past it.
 * Returns 0 if there were no hex digits at all. */
static int parse_hex32(const char** sp, uint32_t* out) {
    const char* s = skip_spaces(*sp);
    uint32_t v = 0u;
    uint32_t digits = 0u;

    if (s[0] == '0' && s[1] == 'x') s += 2;
    for (;;) {
        char c = *s;
        uint32_t d;

        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10u;
        } else {
            break;
        }
        v = (v << 4) | d;
        ++digits;
        ++s;
    }
    if (digits == 0u) return 0;
    *sp = s;
    *out = v;
    return 1;
}

/* Match "<verb>" or "<verb> <args...>"; on match, *rest points at the args. */
static int command_verb(const char* cmd, const char* verb, const char** rest) {
    uint32_t n = str_len(verb);

    for (uint32_t i = 0; i < n; ++i) {
        if (cmd[i] != verb[i]) return 0;
    }
    if (cmd[n] == 0) {
        *rest = cmd + n;
        return 1;
    }
    if (cmd[n] != ' ' && cmd[n] != '\t') return 0;
    *rest = skip_spaces(cmd + n);
    return 1;
}

/* Trace sink for the shared bring-up: one M7F1 log frame per step. */
static void payload_panel_log(void* ctx, const char* label, int has_value, uint32_t value) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    (void)ctx;
    mt6592_watchdog_disable();
    out = append_text(out, end, label);
    if (has_value) out = append_hex32(out, end, value);
    send_probe_line(line, out);
}

static void command_peek(const char* args) {
    char line[256];
    uint32_t addr = 0u;
    uint32_t words = 1u;

    if (!parse_hex32(&args, &addr)) {
        send_log("peek: usage peek <hex-addr> [word-count]");
        return;
    }
    (void)parse_hex32(&args, &words);
    if (words == 0u) words = 1u;
    if (words > 64u) words = 64u;
    addr &= ~3u;

    for (uint32_t i = 0; i < words;) {
        char* out = line;
        char* end = line + sizeof(line);
        uint32_t on_line = 0u;

        out = append_hex32(out, end, addr + i * 4u);
        out = append_text(out, end, ":");
        while (on_line < 4u && i < words) {
            uint32_t v = 0u;

            out = append_text(out, end, " ");
            if (mt6592_mmio_safe_read32(addr + i * 4u, &v)) {
                out = append_hex32(out, end, v);
            } else {
                out = append_text(out, end, "<abort>");
            }
            ++on_line;
            ++i;
        }
        mt6592_watchdog_disable();
        send_probe_line(line, out);
    }
}

static void command_poke(const char* args) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);
    uint32_t addr = 0u;
    uint32_t value = 0u;
    uint32_t before = 0u;
    uint32_t after = 0u;
    int had_before;

    if (!parse_hex32(&args, &addr) || !parse_hex32(&args, &value)) {
        send_log("poke: usage poke <hex-addr> <hex-value>");
        return;
    }
    addr &= ~3u;

    had_before = mt6592_mmio_safe_read32(addr, &before);
    mt6592_watchdog_disable();
    if (!mt6592_mmio_safe_write32(addr, value)) {
        out = append_field_hex32(out, end, "poke aborted at ", addr);
        send_probe_line(line, out);
        return;
    }
    (void)mt6592_mmio_safe_read32(addr, &after);

    /* Report the readback, not the value written: on this SoC plenty of display
     * registers are write-1-to-clear, reserved-bit-masked, or shadowed until a
     * DDP commit, and "it took" is a different claim from "it was written". */
    out = append_text(out, end, "poke ");
    out = append_hex32(out, end, addr);
    out = append_text(out, end, " was=");
    if (had_before) {
        out = append_hex32(out, end, before);
    } else {
        out = append_text(out, end, "<abort>");
    }
    out = append_text(out, end, " wrote=");
    out = append_hex32(out, end, value);
    out = append_text(out, end, " now=");
    out = append_hex32(out, end, after);
    send_probe_line(line, out);
}

static void command_panel(const char* args) {
    mt6592_panel_bringup_cfg_t cfg;
    uint32_t argb = 0u;
    int rc;

    mt6592_panel_bringup_defaults(&cfg);

    /* Nothing has trained the memory controller in BROM mode, so the DRAM
     * framebuffer at 0x82700000 is not readable. Drive the OVL ROI background
     * instead: full-screen solid colour, no DRAM traffic at all. */
    cfg.fb_addr = 0u;
    cfg.bg_color = parse_hex32(&args, &argb) ? argb : 0xff0000ffu;
    cfg.skip_clocks = g_panel_clocks_done;

    mt6592_watchdog_disable();
    usb_reinit_for_vcom(); /* before heavy display work */

    rc = mt6592_panel_bringup(&cfg, payload_panel_log, 0);

    /* The bring-up reprograms clocks and starts the video path; re-arm the BROM
     * USB callbacks so the VCOM bridge survives to report the result. */
    usb_reinit_for_vcom();
    g_panel_clocks_done = 1u;

    if (rc == -1) {
        send_log("panel: JD9365 program refused; DSI registers dumped above");
    } else if (rc == -2) {
        send_log("panel: DDP scanout setup failed");
    } else {
        send_log("panel: bring-up complete; the panel should now be showing the solid colour");
    }
    mt6592_panel_dump(payload_panel_log, 0);
}

static void command_dsi(void) {
    mt6592_watchdog_disable();
    mt6592_panel_dump(payload_panel_log, 0);
}

static void command_backlight(const char* args) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);
    uint32_t pct = 100u;

    (void)parse_hex32(&args, &pct);
    if (pct > 100u) pct = 100u;
    mt6592_watchdog_disable();
    mt6592_backlight_reassert(pct);
    out = append_field_hex32(out, end, "backlight pct", pct);
    send_probe_line(line, out);
}

/* ── PMIC ──
 * The pwrap wrapper is the only way to reach the MT6323, and it must be
 * INIT_DONE before a transfer means anything. stage1 normally does this; the
 * console may be the first thing to want it. */
static int payload_pwrap_ready(void) {
    if (mt6592_pwrap_is_ready()) return 1;
    return mt6592_pwrap_init() == MT6592_PWRAP_OK || mt6592_pwrap_is_ready();
}

static void command_pmic_read(const char* args) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);
    uint32_t reg = 0u;
    uint32_t count = 1u;
    uint32_t v = 0u;

    if (!parse_hex32(&args, &reg)) {
        send_log("pmicr: usage pmicr <hex-reg> [count]   (MT6323 registers are 16-bit, addresses step by 2)");
        return;
    }
    (void)parse_hex32(&args, &count);
    if (count == 0u) count = 1u;
    if (count > 32u) count = 32u;

    mt6592_watchdog_disable();
    if (!payload_pwrap_ready()) {
        send_log("pmicr: pwrap is not INIT_DONE; no PMIC access");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = reg + i * 2u;

        out = line;
        out = append_hex32(out, end, r);
        out = append_text(out, end, ": ");
        if (mt6592_pwrap_read(r, &v) == 0) {
            out = append_hex32(out, end, v);
        } else {
            out = append_text(out, end, "<pwrap-error>");
        }
        mt6592_watchdog_disable();
        send_probe_line(line, out);
    }
}

static void command_pmic_write(const char* args) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);
    uint32_t reg = 0u;
    uint32_t value = 0u;
    uint32_t before = 0u;
    uint32_t after = 0u;

    if (!parse_hex32(&args, &reg) || !parse_hex32(&args, &value)) {
        send_log("pmicw: usage pmicw <hex-reg> <hex-value>");
        return;
    }

    mt6592_watchdog_disable();
    if (!payload_pwrap_ready()) {
        send_log("pmicw: pwrap is not INIT_DONE; no PMIC access");
        return;
    }

    (void)mt6592_pwrap_read(reg, &before);
    if (mt6592_pwrap_write(reg, value) != 0) {
        send_log("pmicw: pwrap write failed");
        return;
    }
    /* Read back rather than echo: PMIC registers have reserved and hardware-
     * driven fields, so what sticks is the only interesting answer. */
    (void)mt6592_pwrap_read(reg, &after);

    out = append_text(out, end, "pmicw ");
    out = append_field_hex32(out, end, "reg", reg);
    out = append_text(out, end, " was=");
    out = append_hex32(out, end, before);
    out = append_text(out, end, " wrote=");
    out = append_hex32(out, end, value);
    out = append_text(out, end, " now=");
    out = append_hex32(out, end, after);
    send_probe_line(line, out);
}

/* ── Centre LED ──
 * The board lights every colour at power-on because the MT6323 leaves its ISINK
 * channels enabled out of reset and nothing in the stock LK ever writes them.
 * Which channel is which colour is a board routing fact no artefact in
 * Reference/ records, so this command exists to settle it by looking at the
 * board: `led 1 0 0` lights channel MVII_MT6592_LED_CH_RED and whatever colour
 * appears IS that channel. */
static void command_led(const char* args) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);
    uint32_t r = 0u;
    uint32_t g = 0u;
    uint32_t b = 0u;
    int rc;

    if (!parse_hex32(&args, &r)) {
        send_log("led: usage led <r> <g> <b>   (each 0 or 1; `led 0 0 0` turns the centre LED off)");
        send_log("led: the three arguments drive ISINK channels red/green/blue as currently mapped;");
        send_log("     if the colour that lights disagrees, rebuild with -DMVII_MT6592_LED_CH_RED=<n> etc.");
        return;
    }
    (void)parse_hex32(&args, &g);
    (void)parse_hex32(&args, &b);

    mt6592_watchdog_disable();
    if (!payload_pwrap_ready()) {
        send_log("led: pwrap is not INIT_DONE; no PMIC access");
        return;
    }

    rc = mt6592_led_set_channels(r, g, b);
    out = append_text(out, end, "led ");
    out = append_field_hex32(out, end, "r", r != 0u);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "g", g != 0u);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "b", b != 0u);
    out = append_text(out, end, rc == 0 ? " ok" : " pwrap-error");
    send_probe_line(line, out);
}

static void command_log_display(void) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    out = append_text(out, end, "display ");
    out = append_field_hex32(out, end, "dsi_start", read32(MTK_DSI0_BASE + MTK_DSI_START));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mode", read32(MTK_DSI0_BASE + MTK_DSI_MODE_CTRL));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "int", read32(MTK_DSI0_BASE + MTK_DSI_INTSTA));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "txrx", read32(MTK_DSI0_BASE + MTK_DSI_TXRX_CTRL));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "ps", read32(MTK_DSI0_BASE + MTK_DSI_PSCTRL));
    send_probe_line(line, out);

    out = line;
    out = append_text(out, end, "ddp ");
    out = append_field_hex32(out, end, "mout", read32(MTK_MMSYS_BASE + MTK_MMSYS_OVL0_MOUT_EN));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "out", read32(MTK_MMSYS_BASE + MTK_MMSYS_DISP_OUT_SEL));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "ovl_en", read32(MTK_OVL0_BASE + MTK_OVL_EN));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "ovl_src", read32(MTK_OVL0_BASE + MTK_OVL_SRC_CON));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "rdma", read32(MTK_RDMA0_BASE + MTK_RDMA_GLOBAL_CON));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mutex", read32(MTK_MUTEX_BASE + MTK_MUTEX0_MOD));
    send_probe_line(line, out);

    out = line;
    out = append_text(out, end, "framebuffer addr=");
    out = append_hex32(out, end, MT6592_J36_SOFTWARE_FB_ADDR);
    out = append_text(out, end, " 640x480 pitch=0x00000a00 bpp=32; LK handoff path is BGRA/XRGB8888");
    send_probe_line(line, out);
}

static void command_log_keys(void) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    out = append_text(out, end, "keys ");
    out = append_field_hex32(out, end, "kpd_sta", read32(MTK_KPD_BASE + MTK_KPD_STA));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mem1", read32(MTK_KPD_BASE + MTK_KPD_MEM1));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mem2", read32(MTK_KPD_BASE + MTK_KPD_MEM2));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mem3", read32(MTK_KPD_BASE + MTK_KPD_MEM3));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mem4", read32(MTK_KPD_BASE + MTK_KPD_MEM4));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mem5", read32(MTK_KPD_BASE + MTK_KPD_MEM5));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "sel", read32(MTK_KPD_BASE + MTK_KPD_SEL));
    send_probe_line(line, out);

    out = line;
    out = append_text(out, end, "gpio-in ");
    out = append_field_hex32(out, end, "g0", read32(gpio_group_addr(MTK_GPIO_DIN_BASE, 0u)));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "g5", read32(gpio_group_addr(MTK_GPIO_DIN_BASE, 5u)));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "g7", read32(gpio_group_addr(MTK_GPIO_DIN_BASE, 7u)));
    out = append_text(out, end, "  watch deltas while holding buttons; GPIO90 is backlight/PWM noise");
    send_probe_line(line, out);

    send_log("keymap: MVII DPAD uses raw GPIO DIN U/D/L/R=22/35/8/21 without pinmux writes");
    send_log("keymap: KPD raw face A/B/X/Y=9/11/12/10  L1/R1/L2/R2=2/4/3/5");
    send_log("keymap: KPD raw START/SELECT/MODE=30/29/35  POWER=PMIC");
    send_log("keymap: Android Vendor_2454_Product_6500.kl lists translated Linux keycodes, not raw KPD bits");
}

static void command_log_sd(void) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    out = append_text(out, end, "msdc0/eMMC ");
    out = append_field_hex32(out, end, "cfg", read32(MTK_MSDC0_BASE + MTK_MSDC_CFG));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "ps", read32(MTK_MSDC0_BASE + MTK_MSDC_PS));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "int", read32(MTK_MSDC0_BASE + MTK_MSDC_INT));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "sts", read32(MTK_MSDC0_BASE + MTK_MSDC_SDC_STS));
    send_probe_line(line, out);

    out = line;
    out = append_text(out, end, "msdc1 candidate WiFi/SDIO ");
    out = append_field_hex32(out, end, "cfg", read32(MTK_MSDC1_BASE + MTK_MSDC_CFG));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "ps", read32(MTK_MSDC1_BASE + MTK_MSDC_PS));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "int", read32(MTK_MSDC1_BASE + MTK_MSDC_INT));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "sts", read32(MTK_MSDC1_BASE + MTK_MSDC_SDC_STS));
    send_probe_line(line, out);

    out = line;
    out = append_text(out, end, "msdc2 candidate external SD ");
    out = append_field_hex32(out, end, "cfg", read32(MTK_MSDC2_BASE + MTK_MSDC_CFG));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "ps", read32(MTK_MSDC2_BASE + MTK_MSDC_PS));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "int", read32(MTK_MSDC2_BASE + MTK_MSDC_INT));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "sts", read32(MTK_MSDC2_BASE + MTK_MSDC_SDC_STS));
    send_probe_line(line, out);
}

static void command_log_usb(void) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    out = append_text(out, end, "usb0 ");
    out = append_field_hex32(out, end, "faddr", read32(MTK_USB0_BASE + 0x0000u));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "devctl", read32(MTK_USB0_BASE + MTK_USB_DEVCTL));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "power", read32(MTK_USB0_BASE + MTK_USB_POWER));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "intrtx", read32(MTK_USB0_BASE + MTK_USB_INTRTX));
    send_probe_line(line, out);
}

/* ── Wi-Fi ──
 *
 * What this console can and cannot do for Wi-Fi, stated plainly because the
 * boundary is structural rather than a matter of effort.
 *
 * CAN: the CONSYS power-up. MT6323 VCN18/VCN28/VCN33 rails over pwrap, the
 * CONSYS MTCMOS domain, the INFRA_CONNMCU clock gate, the TOPAXI bus-protect
 * handshake, and then reading the chip ID out of CONN_MCU_CONFIG. That is the
 * board-specific half of Wi-Fi bring-up, it is the half most likely to be
 * wrong, and it needs nothing but registers — so it runs here, live, against a
 * board that stays up, which is the whole point of this console.
 *
 * CANNOT: scan and connect. Those need the WMT patch and WIFI_RAM_CODE_SOC
 * downloaded into the CONSYS, and the firmware is ~hundreds of KB sitting in a
 * filesystem on the eMMC. This payload is 128 KB of SRAM with no DRAM (the
 * memory controller is untrained in BROM mode) and no filesystem driver. There
 * is nowhere to put the firmware and nothing to read it with.
 *
 * The end-to-end path already exists where those things do:
 * Runtime/WifiSystem.cpp loads both blobs from
 * System/Drivers/MediaTek/WiFi/MT6592 and drives scan/associate through
 * mt6592_wifi_hif.c. So `wifi bind` here is the fast loop for the power-up
 * stage, and the runtime is where the association is proven — see the wifi
 * lines in `-mtk-read-boot-status`.
 */
static void command_wifi_bind(void) {
    const mt6592_wifi_sdio_state* st;
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);
    int rc;

    mt6592_watchdog_disable();
    if (!payload_pwrap_ready()) {
        send_log("wifi: pwrap is not INIT_DONE; the VCN rails cannot be programmed");
        return;
    }

    /* CONSYS power-up is a sharp load step and the rails come from the same
     * PMIC the BROM USB link depends on. Re-arm the VCOM bridge on both sides,
     * the way the panel command does. */
    usb_reinit_for_vcom();
    rc = mt6592_wifi_sdio_bind();
    usb_reinit_for_vcom();

    st = mt6592_wifi_sdio_get_state();

    out = append_text(out, end, "wifi ");
    out = append_field_hex32(out, end, "rails", (uint32_t)st->rails_programmed);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "mtcmos", (uint32_t)st->mtcmos_ready);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "infra_ck", (uint32_t)st->infra_clock_ready);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "responds", (uint32_t)st->consys_responds);
    send_probe_line(line, out);

    out = line;
    out = append_text(out, end, "wifi ");
    out = append_field_hex32(out, end, "chip_id", st->chip_id);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "pwr_status", st->pwr_status);
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "last_err", (uint32_t)st->last_err);
    send_probe_line(line, out);

    if (st->blocked && st->blocked[0]) {
        out = line;
        out = append_text(out, end, "wifi blocked=");
        out = append_text(out, end, st->blocked);
        send_probe_line(line, out);
    }

    if (rc == 0 && st->consys_responds) {
        send_log("wifi: CONSYS is powered and answering; chip_id above is the silicon ID this board really has");
        send_log("wifi: next stage (WMT patch + WIFI_RAM_CODE_SOC, then scan/associate) runs in the OS —");
        send_log("      no DRAM and no filesystem here to hold a firmware image. See Runtime/WifiSystem.cpp.");
    } else {
        send_log("wifi: CONSYS did not come up; the fields above say which stage stopped");
    }
}

static void command_log_wifi(const char* args) {
    if (command_verb(args, "bind", &args) || command_verb(args, "up", &args)) {
        command_wifi_bind();
        return;
    }

    send_log("wifi: Android build.prop says mediatek.wlan.chip=MTK_CONNSYS_MT6592");
    send_log("wifi: extracted firmware set includes WMT_SOC.cfg, WIFI_RAM_CODE_SOC, and mt6627 assets");
    send_log("wifi: board/schematic banner still points at MT6625-class combo; run `wifi bind` for the real silicon ID");
    send_log("wifi bind = live CONSYS power-up (VCN rails, MTCMOS, INFRA clock, chip-ID probe)");
    command_log_sd();
}

static void command_log_audio(void) {
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    out = append_text(out, end, "audio afe ");
    out = append_field_hex32(out, end, "top", read32(MTK_AUDIO_AFE_BASE + MTK_AFE_AUDIO_TOP_CON0));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "dac", read32(MTK_AUDIO_AFE_BASE + MTK_AFE_DAC_CON0));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "i2s", read32(MTK_AUDIO_AFE_BASE + MTK_AFE_I2S_CON));
    out = append_text(out, end, " ");
    out = append_field_hex32(out, end, "conn0", read32(MTK_AUDIO_AFE_BASE + MTK_AFE_CONN0));
    send_probe_line(line, out);
    send_log("audio: AFE base is visible; PMIC codec/smart-PA path is not armed yet");
}

static void command_speaker_noise(void) {
    uint32_t dac_before = read32(MTK_AUDIO_AFE_BASE + MTK_AFE_DAC_CON0);
    char line[256];
    char* out = line;
    char* end = line + sizeof(line);

    command_log_audio();
    send_log("speaker-noise: probe-only; prior AFE_DAC_CON0 write killed the live USB loop");
    send_log("speaker-noise: next step is PMIC codec/smart-PA routing from LK/kernel tables before any audio writes");

    out = append_text(out, end, "speaker-noise skipped ");
    out = append_field_hex32(out, end, "dac_before", dac_before);
    send_probe_line(line, out);
}

static void command_probe_all(void) {
    command_log_display();
    command_log_keys();
    command_log_usb();
    command_log_wifi("");
    command_log_audio();
}

static void handle_command(const struct frame* f) {
    char command[64];
    const char* args = "";
    uint32_t len = command_copy(command, sizeof(command), g_frame_payload, f->len);

    mt6592_watchdog_disable();
    if (len == 0u || bytes_equal_text((const uint8_t*)command, str_len(command), "help") ||
        bytes_equal_text((const uint8_t*)command, str_len(command), "menu")) {
        handle_command_help();
    } else if (command_verb(command, "debug", &args)) {
        /* Neither of these returns; both reset the SoC after sending DONE. */
        command_debug_mode(f->seq);
        return;
    } else if (command_verb(command, "boot", &args)) {
        command_boot_now(f->seq);
        return;
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "probe") ||
               bytes_equal_text((const uint8_t*)command, str_len(command), "all")) {
        command_probe_all();
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "keys")) {
        command_log_keys();
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "display")) {
        command_log_display();
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "sd")) {
        command_log_sd();
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "usb")) {
        command_log_usb();
    } else if (command_verb(command, "wifi", &args)) {
        command_log_wifi(args);
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "audio")) {
        command_log_audio();
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "speaker-noise")) {
        command_speaker_noise();
    } else if (command_verb(command, "peek", &args)) {
        command_peek(args);
    } else if (command_verb(command, "poke", &args)) {
        command_poke(args);
    } else if (command_verb(command, "panel", &args)) {
        command_panel(args);
    } else if (command_verb(command, "bl", &args)) {
        command_backlight(args);
    } else if (command_verb(command, "pmicr", &args)) {
        command_pmic_read(args);
    } else if (command_verb(command, "pmicw", &args)) {
        command_pmic_write(args);
    } else if (command_verb(command, "led", &args)) {
        command_led(args);
    } else if (bytes_equal_text((const uint8_t*)command, str_len(command), "dsi")) {
        command_dsi();
    } else {
        send_status(FRAME_ERROR, f->seq, FLASH_ERR_UNSUPPORTED, "unknown command; try help");
        return;
    }
    send_status(FRAME_DONE, f->seq, FLASH_OK, "command complete; payload still alive");
}

static uint32_t g_stage1_stream_seq;

/* Breadcrumb sink that streams stage1 progress to the host over the live VCOM
 * instead of writing eMMC, so we never drop the bridge or depend on eMMC writes.
 * Each call ships the full 512-byte status record as one M7F1 frame. */
static int stage1_usb_sink(void* ctx, const uint8_t* sector, uint32_t size, uint32_t sequence) {
    (void)ctx;
    (void)sequence;
    mt6592_watchdog_disable();
    send_frame(FRAME_STAGE1_STATUS, g_stage1_stream_seq, sector, size);
    return 0;
}

/* Run the stage1 display bring-up in-process. The payload command loop resumes
 * afterwards, so the host keeps the same connection for further reads/runs. */
static void handle_run_stage1(const struct frame* f) {
    g_stage1_stream_seq = f->seq;
    send_log("running stage1 display bring-up in-payload; VCOM stays up");
    mt6592_watchdog_disable();
    mvii_arm_stage1_set_sink(stage1_usb_sink, 0);
    usb_reinit_for_vcom(); /* before heavy display work */
    mvii_arm_stage1_run(0u, 0u, 0u, 0u);
    /* After the full OVL/RDMA/DSI bring-up (which reprograms clocks and
     * starts the video path), re-init the BROM USB callbacks. This keeps
     * the VCOM bridge "configured" on the host side for the remaining
     * status frames and the final DONE. */
    usb_reinit_for_vcom();
    mvii_arm_stage1_set_sink(0, 0);
    send_status(FRAME_DONE, f->seq, FLASH_OK, "stage1 run complete; payload still alive");
}

void mvii_arm_flash_stage_main(void) {
    struct frame f;

    memzero(&__bss_start, (uint32_t)(&__bss_end - &__bss_start));
    mt6592_watchdog_disable();
    mt6592_uart_init();
    mt6592_uart_puts("\nMVIIFlash MT6592 native payload\n");
    usb_init();
    send_hello(0);

    for (;;) {
        int err = read_frame(&f);
        if (err != 0) {
            send_status(FRAME_ERROR, 0, FLASH_ERR_FRAME, "bad M7F1 frame");
            continue;
        }

        switch (f.type) {
            case FRAME_HELLO:
                send_hello(f.seq);
                break;
            case FRAME_START:
                handle_start(&f);
                break;
            case FRAME_DATA:
                handle_data(&f);
                break;
            case FRAME_FINISH:
                handle_finish(&f);
                break;
            case FRAME_READ:
                handle_read(&f);
                break;
            case FRAME_REBOOT:
                handle_reboot(&f);
                break;
            case FRAME_RUN_STAGE1:
                handle_run_stage1(&f);
                break;
            case FRAME_COMMAND:
                handle_command(&f);
                break;
            default:
                send_status(FRAME_ERROR, f.seq, FLASH_ERR_FRAME, "unknown M7F1 frame");
                break;
        }
    }
}
