#ifndef MT6592_WIFI_WMT_H
#define MT6592_WIFI_WMT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MediaTek's ENUM_WMTDRV_TYPE_T, the subsystem index FUNC_CTRL takes. Only the
 * WLAN one is used here; the rest are listed because `wifi func` sweeps them and
 * a named pad is worth more than a number when something powers off.
 */
enum
{
    WMT_SUBSYSTEM_BT   = 0u,
    WMT_SUBSYSTEM_FM   = 1u,
    WMT_SUBSYSTEM_GPS  = 2u,
    WMT_SUBSYSTEM_WIFI = 3u,
    WMT_SUBSYSTEM_WMT  = 4u
};

typedef struct
{
    int btif_ready;
    int calibrated;
    int ready;
    /* Did the connectivity MCU accept FUNC_CTRL(wifi, on)? Nothing downstream of
     * WIFI_START can work without it, and the download completes either way, so
     * this is the difference between "the firmware crashed" and "the firmware
     * was never allowed to start". */
    int wifi_function_on;
    uint8_t patch_count;
    uint8_t patch_mask;
    uint32_t patch_bytes;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t last_btif_lsr;
    /* Wall-clock cost of pushing one STP frame into the BTIF FIFO. A frame the
     * peer never answers is a different fault depending on whether it took
     * microseconds or long enough for the peer's parser to time out. */
    uint32_t last_tx_us;
    uint32_t max_tx_us;
    /* Size and opcode of the last WMT event the peer produced. "The peer said
     * nothing" and "the peer said something unexpected" are different faults
     * with different fixes, and until these were exported the only way to tell
     * them apart was a UART trace nobody had a cable for. */
    uint32_t last_event_size;
    uint32_t last_event_opcode;
    const char* status;
    const char* blocked;
} mt6592_wifi_wmt_state;

/* Bring the BTIF link up and ask the connectivity MCU to identify its STP
 * capability. This is the first exchange of the stock bootstrap and the only one
 * that needs no firmware image, so it answers "is the peer alive at all?" on its
 * own. Safe to call repeatedly. */
int mt6592_wifi_wmt_probe_link(void);

/* Download one stock ROMv1_patch_*_hdr.bin image. Patch sequence/address are
 * read from the four launcher metadata bytes at file offset 24. */
int mt6592_wifi_wmt_load_patch(const void* data, uint32_t size);

/* WMT opcode 0x06 FUNC_CTRL: turn one connectivity subsystem on or off. The
 * bootstrap already turns WLAN on at the end of the patch load; this is here so
 * the subsystem index can be swept from the console, because the index is the
 * one value in that command with no measurement behind it. */
int mt6592_wifi_wmt_func_ctrl(uint8_t subsystem, int on);

/*
 * Send a WMT frame this driver does not otherwise know about and return the
 * event bytes. The console exposes it as `wifi wmt <bytes...>`, which turns
 * every command in the stock kernel's tables into something answerable on a
 * booted board instead of something that needs a new image. Returns 0 when the
 * peer answered with status 0; the event is copied out either way, because on a
 * probe the failure bytes are the result.
 */
int mt6592_wifi_wmt_raw_command(const uint8_t* command, uint32_t command_size, uint8_t* event_out,
                                uint32_t event_max, uint32_t* event_len);

const mt6592_wifi_wmt_state* mt6592_wifi_wmt_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_WIFI_WMT_H */
