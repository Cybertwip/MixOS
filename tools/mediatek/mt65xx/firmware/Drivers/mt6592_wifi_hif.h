#ifndef MT6592_WIFI_HIF_H
#define MT6592_WIFI_HIF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One scan hit, in the shape the scan reporter hands back. This used to be
 * intel_wifi_scan_result, pulled in from the Machine64 driver directory so both
 * sides of the old OS/loader split agreed on the layout. There is no other side
 * any more -- the loader is the only thing that scans -- so the definition lives
 * here, where the only code that fills it in and the only code that reads it
 * both are, and the loader build no longer reaches outside its own directory
 * for a header.
 *
 * ssid is 33 bytes so a maximum-length 32-byte SSID is still NUL-terminated.
 */
typedef struct mt6592_wifi_scan_result {
    char ssid[33];
    int16_t rssi;
    uint8_t encrypted;
} mt6592_wifi_scan_result;

/*
 * What happened to the A-die probe on this boot. Kept because the difference
 * between "the RF front end was configured by its own probe" and "we forged the
 * flag that says it was" is invisible in every other field here, and it is the
 * difference between a receiver that hears beacons and one that does not.
 */
enum
{
    ADIE_PROBE_NOT_RUN = 0,  /* nothing has been attempted yet                */
    ADIE_PROBE_RAN,          /* INIT_CMD 7 posted, and the ROM set the flag   */
    ADIE_PROBE_STUCK,        /* the flag was set and would not clear: forged  */
    ADIE_PROBE_TIMEOUT,      /* command sent, flag never came up: forged      */
    ADIE_PROBE_ROM_SILENT,   /* the ROM stopped answering mid-poll: forged    */
};

/* How a scan stopped. Only the first of these means the firmware looked. */
enum
{
    SCAN_END_NONE = 0,     /* no scan has run, or one is running now         */
    SCAN_END_DONE,         /* EVENT_ID_SCAN_DONE, sequence matched           */
    SCAN_END_OWN_LOST,     /* the HIF would not grant driver ownership again */
    SCAN_END_ABNORMAL,     /* WHISR raised ABNORMAL_INT                      */
    SCAN_END_TIMEOUT,      /* 12 s passed with no SCAN_DONE                  */
};

/*
 * How far the association got. Mirrored out of the driver's private state by
 * mt6592_wifi_hif_get_state() so callers can say which step is stuck.
 *
 * Without this, everything between "associate rc=0" and a working link looked
 * the same from outside: five distinct waits -- for a channel grant, for an
 * authentication reply, for an association reply, and for two halves of the
 * four-way handshake -- all reported as `assoc=no`. The status string names the
 * failure once one is declared, but a machine that is merely still waiting has
 * no failure to name, and that is exactly the state a console needs to show.
 */
enum
{
    WIFI_ASSOC_IDLE = 0,
    WIFI_ASSOC_WAIT_CHANNEL,   /* channel grant from the firmware            */
    WIFI_ASSOC_WAIT_AUTH,      /* open-system authentication reply from the AP */
    WIFI_ASSOC_WAIT_ASSOC,     /* association response from the AP           */
    WIFI_ASSOC_WAIT_EAPOL_M1,  /* WPA2 four-way handshake, message 1         */
    WIFI_ASSOC_WAIT_EAPOL_M3,  /* WPA2 four-way handshake, message 3         */
    WIFI_ASSOC_CONNECTED,
    WIFI_ASSOC_FAILED,
};

/* Traffic classes the HIF keeps a TX page count for. Six, because that is how
 * many bytes nicTxReleaseResource walks and how many WTSR0/WTSR1 carry. */
#define MT6592_WIFI_TX_CLASSES 6u

typedef struct
{
    int hif_ready;
    int driver_own;
    int firmware_loaded;
    int firmware_alive;
    int configured;
    int bss_active;
    int scan_active;
    int auth_active;
    int associated;
    uint32_t assoc_phase;   /* WIFI_ASSOC_*, refreshed by get_state() */
    uint32_t assoc_retries; /* retransmissions of the current step    */
    int data_path_ready;
    int secure;
    /*
     * The firmware's station-record index for the AP, or 0xFE when the record
     * has never been activated. 0xFE is not cosmetic: every unicast data frame
     * built while it holds that value goes out on the not-found path, which is
     * basic rate with no ACK solicited and no hardware retransmission -- and the
     * statistics show precisely that shape (tx_multicast == tx_fragments,
     * tx_retry == 0, ack_failed == -tx_fragments on every sample so far).
     * Surfaced so a run can say whether EVENT_ID_ACTIVATE_STA_REC ever landed.
     */
    uint8_t sta_rec_index;
    /*
     * How many times the four-way handshake had to install the pairwise key
     * without the firmware ever confirming message 4 was transmitted. Nonzero
     * means EVENT_ID_TX_DONE is not being reported for 1X frames, and the key
     * install is back to racing the queue the way it did before.
     */
    uint32_t m4_tx_done_timeouts;
    uint16_t hif_chip_id;
    uint8_t hif_revision;
    uint32_t firmware_size;
    uint32_t firmware_sections;
    uint32_t downloaded_bytes;
    uint32_t scan_result_count;
    uint32_t rx_packets;
    uint32_t rx_events;
    uint32_t rx_management;
    uint32_t rx_data;
    uint32_t tx_data;
    uint32_t dropped_packets;
    /*
     * The 802.11 reason code from the last deauthentication or disassociation
     * the AP addressed to us, and which of the two frames carried it. The
     * failure string on its own cannot tell "the four-way handshake was
     * rejected" (reason 15) from "you stopped talking to me" (reason 4) from
     * "class 3 frame from a nonassociated STA" (reason 7) -- and those want
     * opposite fixes, so discarding the code left the one useful byte on the
     * floor. 0 means nothing has disconnected us.
     */
    uint16_t disconnect_reason;
    uint8_t  disconnect_was_deauth;
    uint32_t last_whisr;
    uint32_t last_wasr;
    uint32_t last_wrplr;
    /*
     * The scan instruments. Everything above records the *last* value of a
     * register that is almost always zero by the time anyone reads it, which is
     * why two sessions of `rx=0 evt=0 wrplr=0` said nothing: they cannot
     * distinguish "the chip never raised anything" from "it raised something and
     * the poll consumed it between prints".
     *
     * whisr_seen and wrplr_seen are the OR of every value the poll has read, so
     * a single interrupt anywhere in the run survives to the dump. poll_calls
     * separates "no packets arrived" from "the poll loop never ran".
     */
    uint32_t whisr_seen;
    uint32_t wrplr_seen;
    uint32_t poll_calls;
    /*
     * Where the station address came from.
     *
     * g_wifi_mac has a hardcoded default, so a query_firmware_mac() that times
     * out is invisible: the driver goes on to *tell* the firmware that fabricated
     * address over CMD_ID_BASIC_CONFIG and every later print shows a plausible
     * MAC. This is the one bit that says whether the firmware has ever answered
     * a query -- which is a completely different search from a scan that runs
     * and hears nothing.
     */
    uint8_t station_mac[6];
    int station_mac_from_firmware;
    uint32_t mac_query_polls;
    /*
     * How many CMD_ID_SET_DOMAIN_INFO commands have gone out, so a scan result
     * can be read against whether the firmware had a channel list at the time.
     * Two per `wifi domain`: the allowed table and the passive table.
     */
    uint32_t domain_cmds_sent;
    uint32_t ps_cmds_sent;
    /*
     * Why the last scan stopped scanning.
     *
     * scan_active is cleared from four places and the console could not tell
     * them apart, so it printed "scan found 0 networks" for all four -- which
     * is a true sentence for one of them and a lie for the other three. Two
     * runs have now been read as "the scan completed and heard nothing" on that
     * basis, and at least one of them cannot have been: it ended with no event
     * of any kind, no abnormal interrupt, and long before the 12 s timeout,
     * which leaves only the ownership handshake.
     */
    uint32_t scan_end_reason; /* mt6592_wifi_scan_end */
    uint32_t driver_own_lost; /* times acquire_driver_own() timed out */
    /*
     * The IDs of the first events to arrive, in order.
     *
     * `evt=1` is not an answer while a CMD_ID_SET_DOMAIN_INFO result and a
     * CMD_ID_SCAN_REQ completion both count as one. The ID says which, and the
     * count says whether anything was missed after the ring filled.
     */
    uint8_t  event_ids[8];
    uint32_t event_ids_used;
    /*
     * The last EVENT_ID_SCAN_DONE, whether or not it was accepted. The handler
     * only acts on one whose payload[0] matches the sequence byte the scan was
     * submitted with, so a mismatch discards the completion silently and the
     * scan runs on to its timeout. These two numbers are the only way to see
     * that happen.
     */
    uint32_t scan_done_events;
    uint8_t  scan_done_seq;
    uint8_t  scan_done_seq_want;
    /*
     * THE REST OF THAT EVENT, which is the only report the firmware ever makes
     * about the sweep it just performed.
     *
     * EVENT_SCAN_DONE is not one byte. Stock's scnEventScanDone -- at 0xc042e938
     * in this device's own kernel, reached by decompressing the zImage and
     * recovering symbols from its __func__ table -- reads four:
     *
     *   payload[0] ucSeqNum
     *   payload[1] ucSparseChannelValid
     *   payload[2] eSparseChannelBand
     *   payload[3] ucSparseChannelNum
     *
     * and when payload[1] is set it stores the other two as "the least busy
     * channel". A radio cannot nominate the quietest channel in a band without
     * having listened on the others, so a set valid bit carrying a plausible
     * 2.4 GHz channel number is POSITIVE EVIDENCE THAT THE RECEIVER RAN -- which
     * is exactly what "SCAN_DONE, 0 networks" has never been able to give. Zero
     * here is the opposite claim: the sweep did not happen. Those are different
     * faults with disjoint fixes and this driver could not tell them apart.
     *
     * payload_len is kept beside them because the fields are only meaningful if
     * the event was long enough to contain them, and raw[] holds the first eight
     * bytes verbatim so that a field mapping that turns out to be off by one is
     * recoverable from the log instead of costing another flash.
     */
    uint8_t  scan_done_sparse_valid;
    uint8_t  scan_done_sparse_band;
    uint8_t  scan_done_sparse_channel;
    uint8_t  scan_done_raw[8];
    uint32_t scan_done_payload_len;
    /*
     * SCAN_REQ submitted to SCAN_DONE received, in milliseconds.
     *
     * A passive 2.4 GHz sweep listens on fourteen channels for a beacon interval
     * apiece and cannot be quick -- stock takes seconds. The two runs on record
     * reported done inside the console's first one-second progress line, and a
     * scan that returns faster than one beacon interval per channel did not
     * visit the channels. This is the cheapest possible test of that, and it
     * costs one subtraction.
     */
    uint32_t scan_elapsed_ms;
    /*
     * THE RECEIVER'S OWN COUNTERS.
     *
     * The sparse-channel report answered the first question -- the sweep runs,
     * fourteen channels at about a beacon interval each -- and left the harder
     * one. "No beacon reached the host" is still two completely different
     * faults: nothing is being demodulated at all, or frames are being
     * demodulated and then not forwarded. Nothing on the host can tell those
     * apart, because both look identical from here: zero packets.
     *
     * The firmware keeps the numbers that can. Stock reaches them through
     * wlanoidQueryStatistics (0xc03cbf70), which is CID 130 sent as a QUERY with
     * an empty body; the answer is handed to nicCmdEventQueryStatistics
     * (0xc03eb9c0), and that function is twelve consecutive ldrd/strd pairs, so
     * the payload layout is not a guess -- it is twelve 64-bit counters at
     * offsets 0, 8, ... 88, copied in order into the NDIS statistics struct:
     *
     *    0  TransmittedFragmentCount        48  RTSFailureCount
     *    8  MulticastTransmittedFrameCount  56  ACKFailureCount
     *   16  FailedCount                     64  FrameDuplicateCount
     *   24  RetryCount                      72  ReceivedFragmentCount
     *   32  MultipleRetryCount              80  MulticastReceivedFrameCount
     *   40  RTSSuccessCount                 88  FCSErrorCount
     *
     * FCSErrorCount is the one that settles it. An FCS error is a frame the
     * radio received, demodulated, and found corrupt -- it cannot be counted by
     * a receiver that is switched off, mistuned, or unplugged from its antenna.
     * Nonzero means the analogue path works and the fault is above it. Zero,
     * across a sweep of a room that has any Wi-Fi in it at all, means nothing is
     * arriving to be decoded, and every host-side theory is moot.
     *
     * ReceivedFragmentCount beside it splits the remaining case: frames counted
     * as received but never forwarded is a filtering fault, not an RF one.
     *
     * Stored as separate halves rather than a 64-bit type because this file is
     * shared with stage1, which has no libc and no 64-bit print.
     */
    uint32_t stats_valid;
    uint8_t  stats_event_id;
    uint32_t stats_payload_len;
    uint32_t stats_polls;
    uint32_t stats_lo[12];
    uint32_t stats_hi[12];
    /*
     * The firmware-start evidence. The driver's own progress prints go to the
     * physical UART, which the soft-boot console does not see, so anything worth
     * reading during a start failure has to live here to reach `wifi fw`.
     *
     * start_event_length is the length the peer reported for the first packet it
     * volunteered after WIFI_START, and start_event holds its leading bytes; the
     * init events are 8 bytes of [len16][EID][seq][status]. Nothing consumes
     * these -- they exist to be printed.
     */
    uint32_t firmware_start_address;
    int adie_probe_state;
    uint32_t adie_probe_polls;
    int adie_probe_stale;    /* the flag was already set on entry and was cleared */
    uint32_t last_wcir;
    uint32_t start_event_length;
    uint8_t start_event[8];
    int start_event_valid;
    /*
     * The device-to-host software mailboxes, D2HRM0R/D2HRM1R. Stock reads mailbox
     * 0 in exactly the situation we are stuck in -- the expired readiness loop at
     * 0xc03c11f8 calls nicGetMailbox(adapter, 0, &v) and prints "Waiting for Ready
     * bit: Timeout, ID=%u" -- so it is the one chip-authored word MediaTek's own
     * author thought worth printing on this failure.
     *
     * Kept in pairs: _at_start is sampled immediately before the readiness poll
     * begins, last_ after it expires. Equal values mean the firmware never wrote a
     * mailbox at all; a change means it lived long enough to say something.
     *
     * wcir_seen records every distinct WCIR value observed during the poll with
     * the iteration it appeared on. A single final reading cannot distinguish a
     * chip that lay inert for 5.13 s from one that booted, dropped POR_INDICATOR
     * and re-entered reset underneath us.
     */
    uint32_t mailbox_at_start_0;
    uint32_t mailbox_at_start_1;
    uint32_t last_mailbox_0;
    uint32_t last_mailbox_1;
    uint32_t wcir_transitions;
    uint32_t wcir_seen[8];
    uint32_t wcir_seen_poll[8];
    /*
     * The connectivity MCU's program counter across WIFI_START, read from
     * CONN_MCU_CONFIG 0x18070160 -- see the derivation at CONSYS_MCU_CPUPCR in
     * mt6592_wifi_hif.c. This is the one thing every other instrument here
     * cannot say: WCIR, the mailboxes and the sweep all describe what the core
     * has *published*, and a core that faults publishes nothing. The PC says
     * whether it is running at all, and if it is not, the address it stopped on.
     *
     * cpupcr_before is four samples taken back to back immediately before the
     * WIFI_START packet goes out. They are the control: the boot ROM is
     * demonstrably alive at that moment (it answers ACCESS_REG), so if these
     * four are already identical the register is not a PC on this part and
     * nothing below it means anything. Read that pair first.
     *
     * During the readiness poll every iteration samples it. cpupcr_changes
     * counts iterations where the value differed from the one before, over
     * cpupcr_samples reads spanning the full 5.13 s:
     *
     *   changes == 0  the core is stopped -- halted, or spinning on a branch to
     *                 self -- and cpupcr_last is the address it stopped at.
     *   changes > 0   the core is executing and the failure is that it never
     *                 asserts WLAN_READY, which is a different bug entirely.
     *
     * cpupcr_trace keeps the first few distinct values with the iteration each
     * appeared on, so a short spin loop can be told from a long one, and
     * cpupcr_min/max bound the address range visited -- enough to say which
     * image the core was in: below 0x60000 is boot ROM, 0x60000..0x73b6c is
     * patch 1_0, 0x6a000..0x6b910 is firmware section 0.
     *
     * Those eight entries are all consumed in the first 80 ms once the firmware
     * runs, because the PC then differs on nearly every sample -- they describe
     * the launch and say nothing about the 5 s of steady state that follows.
     * cpupcr_hist is the part that does: a bounded set of distinct addresses
     * with a hit count each, so the address the core actually sits on comes out
     * on top instead of being buried. Overflow past the table is counted in
     * cpupcr_hist_missed rather than dropped silently -- a histogram that only
     * covers part of the run and does not say so is worse than none.
     */
    uint32_t cpupcr_before[4];
    uint32_t cpupcr_trace[8];
    uint32_t cpupcr_trace_poll[8];
    uint32_t cpupcr_trace_count;
    uint32_t cpupcr_hist[16];
    uint32_t cpupcr_hist_hits[16];
    uint32_t cpupcr_hist_used;
    uint32_t cpupcr_hist_missed;
    /*
     * The same samples again, bucketed to 256 bytes instead of to the exact
     * address. The exact table above earned its place -- it is what named
     * 0x0006f9ce, the patch wait loop -- but it cannot ever cover a whole run:
     * the core's PC differs on nearly every sample, so sixteen distinct
     * addresses are used up almost immediately. The last run reported
     * missed=289 of 598, which is to say more than half the execution went
     * unrecorded, and the half we could not see is exactly the half that is not
     * the wait loop we already understand.
     *
     * Bucketing by pc >> 8 collapses each loop, and each small function, to one
     * entry, so thirty-two of these span the run where sixteen exact addresses
     * span the first eighty milliseconds. The resolution lost is resolution we
     * can recover offline anyway: a 256-byte window is a disassembly away from
     * the instruction, and the images are all dumped.
     */
    uint32_t cpupcr_page[32];
    uint32_t cpupcr_page_hits[32];
    uint32_t cpupcr_page_used;
    uint32_t cpupcr_page_missed;
    uint32_t cpupcr_samples;
    uint32_t cpupcr_changes;
    uint32_t cpupcr_first;
    uint32_t cpupcr_last;
    uint32_t cpupcr_min;
    uint32_t cpupcr_max;
    /*
     * The HIF's per-traffic-class TX page credit -- the accounting stock does in
     * nicTxAcquireResource/nicTxReleaseResource and this driver used to skip.
     *
     * Class 4 is the one to read. Every command and every management/EAPOL frame
     * is TC4, TC4 is the class nicTxCmd maps to WTDR1, and the running firmware
     * grants it exactly four pages. Writing a fifth packet into it without first
     * waiting for a credit is what used to take the board off the USB bus in the
     * middle of a join: an AHB write into a full HIF FIFO never retires, so the
     * CPU stops inside the bus transaction with no exception and no watchdog.
     *
     * tx_credited counts pages the firmware has handed back since the HIF came
     * up. Zero after an attempt means it is not crediting at all, and four is the
     * whole session budget. tx_starved counts sends refused for want of a page --
     * reported failures, not hangs, which is the point of counting them. tx_forced
     * counts the sends let through anyway because nothing had ever been credited
     * and the budget was therefore unproven; see the driver for why that valve
     * exists and why it shuts itself the moment the first credit arrives.
     */
    uint8_t tx_free[MT6592_WIFI_TX_CLASSES];
    uint8_t tx_max[MT6592_WIFI_TX_CLASSES];
    uint32_t tx_credited;
    uint32_t tx_waits;
    uint32_t tx_starved;
    uint32_t tx_forced;
    const char* status;
    const char* blocked;
} mt6592_wifi_hif_state;

/* Bind the integrated CONSYS AHB HIF and acquire driver ownership. */
int mt6592_wifi_hif_bind(void);

/* Validate and download the stock divided WIFI_RAM_CODE_SOC image. */
int mt6592_wifi_hif_load_firmware(const void* data, uint32_t size);

/* The container CRC32, so an edited staged image can be made acceptable again. */
uint32_t mt6592_wifi_hif_crc32(const void* data, uint32_t size);

/* Diagnostic: force the WIFI_START entry point; 0 restores the build default
 * (0x00060000), NOT the image's own -- for that, clear the override below. */
void mt6592_wifi_hif_set_start_address(uint32_t address);

/*
 * INIT_CMD_WIFI_START's u4Override word, the other half of the entry-point
 * question and the one the two references disagree about.
 *
 * Enabled (the shipped J36 kernel's behaviour, and this driver's default) means
 * "enter at the address I am giving you". Cleared (what the vendor source builds,
 * since drv_wlan/mt_wifi/Makefile:5 compiles it -DMT6628 and config.h's MT6628
 * arm sets CFG_OVERRIDE_FW_START_ADDRESS 0) means "enter wherever your own header
 * says", and forces the address on the wire to 0 to match stock's FALSE/0 pair.
 */
void mt6592_wifi_hif_set_start_override(int enable);
int mt6592_wifi_hif_get_start_override(void);

/*
 * Diagnostic: hold the load at "sections downloaded, entry not yet taken" so the
 * connectivity SRAM can be read either side of the jump, then take it.
 */
void mt6592_wifi_hif_set_defer_start(int defer);
int mt6592_wifi_hif_start_firmware(void);

/*
 * The connectivity MCU's program counter, CONN_MCU_CONFIG 0x18070160. This is a
 * plain register read with no dependency on the HIF or on the download having
 * happened, so it is valid at any point in a boot -- including before `wifi dl`
 * and after the boot ROM has stopped answering. Stock's whole post-mortem path
 * (stp_dbg_poll_cpupcr, 0xc03b3e80) is this call in a loop.
 */
uint32_t mt6592_wifi_hif_read_cpupcr(void);

/*
 * Arm a one-shot burst trace of that program counter, taken the instant the next
 * WIFI_START or ACCESS_REG packet is written to the HIF. Sampled in a tight loop
 * with nothing between reads, because the whole of what the ROM does with a
 * command fits inside one 10 ms tick of the readiness poll.
 */
void mt6592_wifi_hif_pc_trace_arm(uint32_t gap_microseconds);
const uint32_t* mt6592_wifi_hif_pc_trace(uint32_t* count);

/*
 * Diagnostic: have the WLAN boot ROM write seed, seed+1, ... at `address`, using
 * the same INIT_CMD_DOWNLOAD_BUF path the real download uses. Tests whether a
 * status==0 ACK actually means the copy landed. Only valid before WIFI_START.
 */
int mt6592_wifi_hif_rom_write(uint32_t address, uint32_t seed, uint32_t bytes);

/*
 * Re-send a slice of the image's own ciphertext to its own destination as a
 * packet of its own. `wifi verify` found that the only nine blocks in the image
 * that do not land are exactly the nine that are partially zero; this sends one
 * of them on its own, which both tests why and, if it works, fixes it. Same
 * download path, same mode word. Only valid before WIFI_START.
 */
int mt6592_wifi_hif_rom_download(uint32_t address, const void* data, uint32_t bytes);

/*
 * Diagnostic: read/write connectivity memory through the boot ROM's
 * INIT_CMD_ACCESS_REG, and ask it whether it has an error queued.
 *
 * These reach where nothing else can. The AP has an aperture onto CONSYS SRAM
 * (AP = CONSYS + 0x17E90000, backed over 0x18070000..0x180bffff) and onto the
 * EMI window (CONSYS 0xf0000000 == AP 0x83100000), and firmware sections 2 and
 * 3 were read back decrypted in the latter. Sections 0 and 1 -- 0x0006a000,
 * which is the WIFI_START entry, and 0x0209f800 -- fall outside both, so no
 * `peek` can confirm they landed. The ROM can.
 *
 * `event_out` receives the raw event, header included, because the body layout
 * is inferred rather than measured. Only valid before WIFI_START.
 */
int mt6592_wifi_hif_rom_access_reg(int write, uint32_t address, uint32_t value, uint8_t* event_out,
                                   uint32_t event_max, uint32_t* event_len);
int mt6592_wifi_hif_rom_query_error(uint8_t* event_out, uint32_t event_max, uint32_t* event_len);

/*
 * Run the MT6625L A-die probe for real, via the ROM's undocumented INIT_CMD
 * type 7. Returns 1 if the probe's own completion flag came up, 0 if it did
 * not, -1 if the ROM stopped answering. Only valid before WIFI_START, and
 * called automatically from the firmware start path -- this entry point exists
 * so the console can run and inspect it on its own.
 */
int mt6592_wifi_hif_adie_probe(void);

/*
 * Tell the firmware which channels the regulatory domain allows.
 *
 * Stock does this from wlanLoadManufactureData() before it ever scans, as a
 * pair of CMD_ID_SET_DOMAIN_INFO commands: the allowed-channel table, then the
 * subset of it that must be listened to passively. We have never sent either,
 * which leaves the firmware's channel list at whatever it powers up with -- and
 * an empty list is a complete, unremarkable explanation for a scan that runs to
 * completion and reports nothing.
 *
 * Not on the start path on purpose. It is a difference from stock, not yet a
 * measured fault, so it stays a thing the console can ask for and the scan can
 * be re-run against. Returns 0 if both commands went out.
 */
int mt6592_wifi_hif_send_domain_info(void);

/* Submit a 2.4 GHz wildcard scan. `active` sends probe requests as well as
 * listening; passive only listens, and so never keys the transmitter. Both take
 * the same path through the HIF, which is what makes the pair a usable test of
 * whether a fault is in the command or in the RF. */
int mt6592_wifi_hif_start_scan(int active);

/*
 * Ask the firmware for its twelve MAC counters -- CID 130, QUERY, empty body,
 * exactly as stock's wlanoidQueryStatistics sends it. Fills stats_* above and
 * returns 0 if an answer arrived.
 *
 * This is a query, not a setting: it changes nothing in the chip and can be run
 * at any point after `wifi fw`, before or after a scan. It exists because
 * FCSErrorCount is the only number either side of this interface that can tell
 * a receiver which heard nothing from a receiver which was never listening.
 */
int mt6592_wifi_hif_query_statistics(void);

/*
 * Read or write one firmware-side register -- CID 194, 8-byte {address, data}.
 *
 * Pass value_in = 0 to read, in which case the answer lands in *value_out. Pass
 * a non-null value_in to write; the write is still acknowledged, and a failure
 * to acknowledge is reported as failure. Either direction returns 0 on success.
 *
 * This is the ONLY register access that survives WIFI_START. `wifi rd` and
 * `wifi dump` go through the boot ROM window and stop working at the handoff,
 * which leaves the running firmware -- where the entire scan problem lives --
 * with no other way to look inside it.
 *
 * Writing pokes a live radio at an address nothing here validates. Reads are
 * safe; treat writes as capable of wedging the chip until proven otherwise.
 */
int mt6592_wifi_hif_mcr(uint32_t address, const uint32_t* value_in, uint32_t* value_out);

/* Poll RX/event FIFOs and advance an active scan without requiring an IRQ. */
int mt6592_wifi_hif_poll(void);

uint32_t mt6592_wifi_hif_get_scan_results(mt6592_wifi_scan_result* out, uint32_t max_results);

/* Associate with an open or WPA2-PSK/CCMP network discovered by the scan path. */
int mt6592_wifi_hif_auth_associate(const char* ssid, const char* password);

/* Drop a running or completed association so the radio stops being claimed.
 * Whoever starts an attempt owns it: nothing else reaps one that is not being
 * polled, so a caller that gives up must say so or it blocks scanning forever.
 * Returns 1 if an association was abandoned, 0 if the driver was already idle. */
int mt6592_wifi_hif_abort_association(void);

/* Diagnostic: put ONE association command on the wire and start nothing.
 * step 0 = UPDATE_STA_RECORD, 1 = CH_PRIVILEGE request, 2 = CH_PRIVILEGE release.
 * `join` sends 0 and 1 back to back, so when the firmware stops answering there
 * is no way to tell which of them it stopped surviving; this sends them apart. */
int mt6592_wifi_hif_assoc_probe(const char* ssid, int step);

/* Ethernet data path exposed to Machine64/lwIP after association. */
int mt6592_wifi_hif_net_ready(void);
int mt6592_wifi_hif_net_send(const void* frame, uint32_t len);
int mt6592_wifi_hif_net_poll_rx(void* frame, uint32_t max_len);
void mt6592_wifi_hif_get_mac(uint8_t mac[6]);

const mt6592_wifi_hif_state* mt6592_wifi_hif_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_WIFI_HIF_H */
