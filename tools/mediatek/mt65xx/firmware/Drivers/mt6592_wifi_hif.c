/*
 * mt6592_wifi_hif.c — minimal freestanding CONSYS_6592 WLAN HIF runtime.
 *
 * This ports the hardware-facing pieces of MediaTek's stock conn_soc/mt_wifi
 * AHB driver that MVII needs for discovery:
 *   - AHB HIF ownership and PIO FIFO access at 0x180f0000
 *   - divided/encrypted WIFI_RAM_CODE_SOC download with per-chunk ACKs
 *   - normal firmware command framing, AIS activation, and wildcard scan
 *   - polling RX0/RX1 and parsing Beacon/Probe Response frames into the
 *     mt6592_wifi_scan_result records
 *   - channel grant, authentication/association, WPA2-PSK/CCMP EAPOL handling
 *   - firmware station/BSS/key synchronization and Ethernet-format PIO TX/RX
 *
 * It intentionally does not pull Linux cfg80211/netdev/WMT modules into MVII.
 */

#include "mt6592_wifi_hif.h"
#include "mt6592_wifi_crypto.h"
#include "mt6592_wifi_sdio.h" /* the VCN33_WIFI rail HifAhbProbe raises; see the bind */

#include "mt6592_bootstatus.h"
#include "mt6592_delay.h"
#include "mt6592_pmic.h"
#include "mt6592_timer.h"
#include "mt6592_uart.h"

#include <stddef.h>
#include <stdint.h>

extern void minos_machine64_io_yield(void) __attribute__((weak));

/*
 * Kick whatever watchdog the caller armed around us.
 *
 * Weak and usually absent: under the OS nothing arms one here. The LK console
 * DOES -- `wifi join' arms 30 s across the whole association -- and it cannot
 * kick it from its own poll loop, because the long stretch is before that loop
 * ever runs: key derivation is 8192 HMAC-SHA1 with no I/O in it at all, and
 * minos_machine64_io_yield is itself null in LK, so the yield below was a call
 * to nothing for the entire derivation. A join that outran the watchdog would
 * reset the board, and a reset is exactly what "the board vanished off USB the
 * moment I typed `wifi join'" looks like from the host.
 *
 * Whether it did is a separate question the step markers in cmd_wifi_join now
 * answer. This is here so the answer cannot be "the timer we set ourselves".
 */
void mvii_console_watchdog_kick(void) __attribute__((weak));

static void wifi_cooperative_yield(void)
{
    if (&minos_machine64_io_yield)
    {
        minos_machine64_io_yield();
    }
    if (&mvii_console_watchdog_kick)
    {
        mvii_console_watchdog_kick();
    }
}

#define BIT(n) (1u << (n))

#ifndef MVII_MT6592_WIFI_ENABLE_TRANSPORT
#    define MVII_MT6592_WIFI_ENABLE_TRANSPORT 0
#endif

enum
{
    HIF_BASE = 0x180f0000u,

    /*
     * CONN_MCU_CONFIG: the connectivity MCU's own control page, and the only
     * window there is onto what that core is actually doing.
     *
     * CONSYS_MCU_CPUPCR is the connectivity MCU's program counter. Stock reads
     * it and nothing else -- wmt_plat_read_cpupcr (0xc03bbc24) is three
     * instructions:
     *
     *     mov  r3, #0
     *     movt r3, #0xf807          ; r3 = 0xf8070000
     *     ldr  r0, [r3, #0x160]
     *
     * 0xf807xxxx is the kernel's fixed IO virtual address; the same function's
     * neighbour mtk_wcn_consys_hw_reg_ctrl (0xc03ba160) reads CONSYS_EMI_MAP at
     * 0xf0001310, which we already know is physical 0x10001310, so the mapping
     * is VA = PA + 0xe0000000 and 0xf8070160 is physical 0x18070160. The same
     * function pins the base a second way by polling 0xf8070008 for the chip ID
     * against 0x6572/0x6582/0x6592 -- that is CONSYS_CHIP_ID at 0x18070008,
     * which this board already answers with 0x6592.
     *
     * The whole of stock's post-mortem tooling is this one register sampled in a
     * loop: stp_dbg_poll_cpupcr (0xc03b3e80) calls wmt_plat_read_cpupcr into a
     * 512-entry ring with osal_sleep_ms between reads, and wmt_plat_read_dmaregs
     * next door (0xc03bbc40) is a stub that returns 0. So there is no richer
     * instrument to go looking for -- this is the instrument.
     */
    CONSYS_MCU_BASE    = 0x18070000u,
    CONSYS_MCU_CHIP_ID = CONSYS_MCU_BASE + 0x0008u,
    CONSYS_MCU_CPUPCR  = CONSYS_MCU_BASE + 0x0160u,

    MCR_WCIR   = 0x0000u,
    MCR_WHLPCR = 0x0004u,
    MCR_WHCR   = 0x000cu,
    MCR_WHISR  = 0x0010u,
    MCR_WHIER  = 0x0014u,
    MCR_WASR   = 0x0018u,
    MCR_WTSR0  = 0x0020u,
    MCR_WTSR1  = 0x0024u,
    MCR_WTDR0  = 0x0028u,
    MCR_WTDR1  = 0x002cu,
    MCR_WRDR0  = 0x0030u,
    MCR_WRDR1  = 0x0034u,
    /*
     * The software mailboxes: host-to-device at 0x38/0x3c, device-to-host at
     * 0x40/0x44. Offsets read out of nicPutMailbox (0xc03d91dc: `mov r1,#56` at
     * 0xc03d9220, `mov r1,#60` at 0xc03d92b4) and nicGetMailbox (0xc03d9334:
     * `mov r1,#64` at 0xc03d9378, `mov r1,#68` at 0xc03d940c). All four sit
     * inside the 0x5c-byte HIF page stock ioremaps at 0xc041034c, so they are
     * ours to read.
     *
     * These matter because D2HRM0R is the one thing stock bothers to read when
     * the readiness poll gives up: at 0xc03c11f8 the expired loop calls
     * nicGetMailbox(adapter, 0, &v) and prints "Waiting for Ready bit: Timeout,
     * ID=%u". It is the only chip-authored word anyone at MediaTek thought worth
     * printing on precisely the failure we have, and we have never looked at it.
     */
    MCR_H2DSM0R = 0x0038u,
    MCR_H2DSM1R = 0x003cu,
    MCR_D2HRM0R = 0x0040u,
    MCR_D2HRM1R = 0x0044u,
    MCR_WRPLR  = 0x0050u,
    MCR_HSTCR  = 0x0058u,

    WCIR_WLAN_READY     = BIT(21),
    WCIR_REVISION_SHIFT = 16u,
    WCIR_REVISION_MASK  = 0x000f0000u,
    WCIR_CHIP_ID_MASK   = 0x0000ffffu,

    WHLPCR_DRIVER_OWN_REQ = BIT(9),
    WHLPCR_IS_DRIVER_OWN  = BIT(8),
    WHLPCR_INT_EN_CLR     = BIT(1),

    WHCR_RX_ENHANCE_MODE_EN  = BIT(16),
    WHCR_MAX_HIF_RX_LEN_MASK = 0x000000f0u,
    WHCR_MAILBOX_READ_CLEAR  = BIT(2),
    WHCR_INT_WRITE_1_CLEAR   = BIT(1),

    WHISR_ABNORMAL_INT = BIT(3),
    WHISR_RX1_DONE_INT = BIT(2),
    WHISR_RX0_DONE_INT = BIT(1),
    WHISR_TX_DONE_INT  = BIT(0),
    WHIER_DEFAULT      = 0xffffff0fu,

    HSTCR_BURST_4_DW   = 1u,
    HSTCR_BURST_SHIFT  = 24u,
    HSTCR_TARGET_SHIFT = 20u,
    HSTCR_COUNT_MASK   = 0x000ffffcu,

    HIF_TARGET_TXD0 = 0u,
    HIF_TARGET_TXD1 = 1u,
    HIF_TARGET_RXD0 = 2u,
    HIF_TARGET_RXD1 = 3u,

    HIF_RX_PACKET_TYPE_MASK       = 0x3u,
    HIF_RX_PACKET_TYPE_DATA       = 0u,
    HIF_RX_PACKET_TYPE_EVENT      = 1u,
    HIF_RX_PACKET_TYPE_MANAGEMENT = 3u,
    HIF_RX_HEADER_SIZE            = 12u,

    /*
     * HIF_RX_HW_APPENDED_LEN (hif_rx.h:120). The length WRPLR reports is the
     * packet's own length; the hardware queues one extra DW of status behind it,
     * and the read has to take that DW too:
     *
     *     nicRxEnhanceReadBuffer (nic_rx.c:2660)
     *         HAL_READ_RX_PORT(prAdapter, u4DataPort,
     *                          ALIGN_4(u2RxLength + HIF_RX_HW_APPENDED_LEN), ...)
     *
     * and nicRxReceiveRFBs reads that word back at ALIGN_4(u2PacketLen) to look
     * at RX_STATUS_TEST_MORE_FLAG. So the drain length is ALIGN_4(len + 4), not
     * ALIGN_4(len) -- and for a length that is already a multiple of four those
     * differ by a whole DW. Leaving that DW in the FIFO shifts every subsequent
     * packet by four bytes, which is not a corrupt frame but a permanently
     * misframed port.
     *
     * The logical packet length stays what WRPLR said: the appended DW is drained
     * and ignored, exactly as stock does outside its one debug print.
     */
    HIF_RX_HW_APPENDED_LEN = 4u,

    INIT_CMD_DOWNLOAD_BUF    = 1u,
    INIT_CMD_WIFI_START      = 2u,
    INIT_EVENT_CMD_RESULT    = 1u,
    INIT_DOWNLOAD_ENCRYPTION = BIT(0),
    /*
     * DL_MODE_RESET_SEC_IV in MediaTek's naming. Tried, measured, and NOT set --
     * do not put it back without new evidence, because both halves of the case
     * for it turned out to be wrong.
     *
     * The argument was that OpenBSD's mwx(4) never separates it from the
     * encryption bit (mwx_mcu_gen_dl_mode: `ret |= (DL_MODE_ENCRYPT |
     * DL_MODE_RESET_SEC_IV)`), and that a stale IV in a chaining cipher would
     * corrupt exactly one 16-byte block per section -- the first -- which is the
     * only damage the read-back ever showed, and which for section 0 is the
     * WIFI_START entry point itself.
     *
     * The cipher does not chain. Section 1's ciphertext holds an unbroken run of
     * 991 identical blocks over a run of zero plaintext; chaining makes every
     * block of such a run different. It is 16-byte ECB, so there is no IV to be
     * stale and no way for the previous section to reach the next one.
     *
     * Setting it changed nothing and broke something. `wifi dump 6a000 8` came
     * back byte-identical with the bit and without it -- 6a000046 4c0e0058
     * 0000004a 00469e80 both times -- so it moved no plaintext. But a first
     * download of a clean boot went from no pending error to
     * QUERY_PENDING_ERROR = 0x0006a000, section 0's own destination: the ROM
     * objects to the mode word, and names the section it objected to.
     */
    INIT_DOWNLOAD_ACK        = BIT(31),

    /*
     * The other half of the boot ROM's INIT protocol, which this driver has
     * never used and stock's wlan driver was built without.
     *
     * We are blind in exactly the place that matters. Of the image's four
     * sections, the two that go to EMI (s2 -> 0xf0020000, s3 -> 0xf0063000) were
     * read back in DRAM at AP 0x83120000 and 0x83163000, decrypted, landing to
     * the page, bit-identical across two boots. The two that do not (s0 ->
     * 0x0006a000, which is also the WIFI_START entry, and s1 -> 0x0209f800) are
     * MCU-internal: no AP aperture reaches them, so no `peek`, `sweep` or `find`
     * can ever say whether they arrived. Both sweeps either side of `wifi go`
     * are flat -- CONSYS SRAM and the whole 1 MiB EMI window -- so the firmware
     * executes nothing, and the only untested link left in the chain is whether
     * the code it is supposed to execute is there at all.
     *
     * ACCESS_REG is the ROM answering a read for us, so it needs no working
     * firmware and no AP aperture. QUERY_PENDING_ERROR asks the ROM whether it
     * already knows something is wrong. Both are single-shot like every other
     * INIT command: valid only before WIFI_START.
     *
     * The layouts below are the MediaTek INIT_CMD/INIT_EVENT structures, not
     * something read out of this kernel -- it has no ACCESS_REG symbols to read.
     * That is why both callers hand the raw event back to the console verbatim:
     * if a field is off by four bytes the bytes still say so.
     */
    INIT_CMD_ACCESS_REG          = 3u,
    INIT_CMD_QUERY_PENDING_ERROR = 4u,
    INIT_EVENT_ACCESS_REG        = 2u,
    INIT_EVENT_PENDING_ERROR     = 3u,

    /*
     * THE COMMAND THAT RUNS THE A-DIE PROBE. Not in any header; read straight
     * off the ROM's own INIT_CMD dispatcher at 0x0000d9a8, which is the same
     * switch that routes the four commands above:
     *
     *   lbi   r2, [r8 + #4]          ; the command type byte, our packet[4]
     *   beq   r2, #4  -> 0xdaea      ; QUERY_PENDING_ERROR
     *   slti45 r2, #5 / beqzs8 0xd9cc
     *   beq   r2, #2  -> 0xda60      ; WIFI_START  (sets byte[gp+4377] at 0xda9a)
     *   slti45 r2, #3 / beqzs8 0xdaa4 ; ACCESS_REG
     *   bne   r2, #1  -> default
     *   j     0xd9e2                 ; DOWNLOAD_BUF
     *  0xd9cc:
     *   beq   r2, #6  -> 0xdb1e      ; -> 0x211c
     *   slti45 r2, #6 / bnez ta, 0xdb0e ; type 5 -> 0x2154
     *   bne   r2, #7  -> default
     *   j     0xdb2e                 ; TYPE 7
     *
     * and type 7's arm is:
     *
     *   0000db2e:  lwi  r0, [r8 + #8]      ; our packet[8..11]
     *   0000db32:  addi r1, r8, #0xc       ; our packet[12..]
     *   0000db38:  jal  0x20d0             ; cos_api_t.c, copies the payload out
     *   0000db3c:  movi55 r0, #2           ; task 2 = wifi_task
     *   0000db3e:  movi   r1, #46          ; message 46
     *   0000db42:  movi55 r2, #3           ; parameter 3
     *   0000db44:  jal    0xd364           ; post()
     *
     * The post is UNCONDITIONAL -- it is the instruction after the call, with no
     * branch over it -- so the payload only decides what 0x20d0 copies, not
     * whether the message goes. And message 46 parameter 3 is precisely what
     * wifi_task's handler at 0x0000db90 tests for before calling 0x15118, the
     * A-die probe sequence. See arm_adie_probe_flag() for the other end of it.
     */
    INIT_CMD_ADIE_PROBE          = 7u,
    INIT_ADIE_PROBE_PACKET_SIZE  = 20u,

    /*
     * ── EVERY CID AND EID BELOW IS NOW CONFIRMED FROM SOURCE, NOT DISASSEMBLY ──
     *
     * The derivations recorded against the individual constants below were read
     * out of this device's own kernel binary, one `bl wlanSendSetQueryCmd` at a
     * time. That work stands, but it was attribution work, and attribution is the
     * part that went wrong once already (see CMD_ID_UPDATE_STA_RECORD). The
     * reference tree carries the enums outright:
     *
     *   ENUM_CMD_ID_T   (include/nic_cmd_event.h:669-769)
     *   ENUM_EVENT_ID_T (include/nic_cmd_event.h:771-830)
     *
     * and every value this driver uses matches them:
     *
     *   0x06 POWER_SAVE_MODE      0x08 ADD_REMOVE_KEY     0x13 SET_DOMAIN_INFO
     *   0x15 BSS_ACTIVATE_CTRL    0x16 SET_BSS_INFO       0x17 UPDATE_STA_RECORD
     *   0x18 REMOVE_STA_RECORD    0x1a INDICATE_PM_BSS_CONNECTED
     *   0x1e SCAN_REQ             0x20 CH_PRIVILEGE       0x82 GET_STATISTICS
     *   0xc1 BASIC_CONFIG         0xc2 ACCESS_REG
     *
     *   0x01 CMD_RESULT   0x04 SCAN_RESULT   0x09 BASIC_CONFIG
     *   0x15 SCAN_DONE    0x18 CH_PRIVILEGE  0x1b BSS_BEACON_TIMEOUT
     *
     * Two of those are worth calling out because they are the pair the join fault
     * turned on: 0x17 really is UPDATE and 0x18 really is REMOVE, so the re-read
     * that moved the join off 0x18 landed on the right number for the right
     * reason. Anything added here from now on should be taken from those two enums
     * first and only checked against the binary, rather than the other way round.
     */

    /*
     * CID 6, POWER_SAVE_MODE -- the last command on stock's adapter-start path
     * that this driver has never sent, and now the only one.
     *
     * Read off nicConfigPowerSaveProfile, which begins at 0xc03dad?? and ends at
     * its literal pool 0xc03daf30. The command build is unambiguous:
     *
     *   c03dae44  movw r8,#0x2090 ; movt r8,#1     r8 = 0x12090
     *   c03dae48  movw lr,#0x2091 ; movt lr,#1     lr = 0x12091
     *   c03dae3c  add  r6, r5, r4, lsl #2          r6 = prAdapter + idx*4
     *   c03dae80  strb r4, [r6, r8]                [0] = ucNetTypeIndex
     *   c03dae88  strb r7, [r6, lr]                [1] = ucPsProfile
     *   c03dae68  mov  ip, #4                      u4SetQueryInfoLen = 4
     *   c03dae84  mov  r1, #6                      CID = 6
     *   c03dae9c  bl   0xc03cae9c                  wlanSendSetQueryCmd
     *
     * so the payload is {ucNetTypeIndex, ucPsProfile, 0, 0} and stock passes
     * Param_PowerModeCAM (0) for AIS at adapter start -- constantly awake.
     *
     * IT IS NOT THE ASSOCIATION FAULT. This comment used to argue that it was --
     * that a BSS the firmware believes is asleep is not one it hands the radio
     * to, which would explain a CH_PRIVILEGE that is neither granted nor refused.
     * The real cause was found since and is written up at CMD_ID_UPDATE_STA_RECORD
     * below: the join's first command was the firmware's DELETE-station-record,
     * sent under the wrong CID. Sending CAM here remains right, and remains what
     * stock sends, so it stays -- as correctness, not as a suspect.
     */
    CMD_ID_POWER_SAVE_MODE         = 0x06u,
    CMD_ID_ADD_REMOVE_KEY          = 0x08u,
    CMD_ID_SET_DOMAIN_INFO         = 0x13u,
    CMD_ID_BSS_ACTIVATE_CTRL       = 0x15u,
    CMD_ID_SET_BSS_INFO            = 0x16u,
    /*
     * ── 0x17. IT WAS 0x18, AND 0x18 IS THE COMMAND THAT DELETES THE RECORD. ──
     *
     * This is the association fault, and it is worth writing down in full because
     * the previous derivation was careful, cited an address, and was still wrong.
     *
     * That derivation said: the CID-24 call sits at 0xc041ec00, cnmStaRecAlloc
     * starts at 0xc041e9c8, the next symbol is at 0xc041ede0, therefore 0xc041ec00
     * is inside cnmStaRecAlloc, therefore 24 is UPDATE_STA_RECORD. The premise is
     * false: cnmStaRecAlloc's next symbol is not 0xc041ede0, it is cnmStaRecFree at
     * 0xc041eb1c. 0xc041ec00 is inside cnmStaRecFree.
     *
     * Re-measured by sweeping EVERY `bl wlanSendSetQueryCmd` in this device's own
     * kernel and attributing each to the symbol whose range actually contains it:
     *
     *   CID 23 (0x17)  cnmStaRecChangeState @0xc041f1a4   40-byte payload
     *   CID 24 (0x18)  cnmStaRecFree        @0xc041ec00    8-byte payload
     *
     * and cnmStaRecFree's 8 bytes are {ucStaRecIndex, pad, aucMacAddr[6]} -- byte
     * 0 from prStaRec[8], the MAC written as a word at +2 and a halfword at +6.
     *
     * So every `wifi join` opened by telling the firmware to DELETE station record
     * 0 of the AIS network, with the target AP's BSSID attached, using a 40-byte
     * body where 8 were expected. That is not a command the firmware ignores; it
     * is one it obeys. It is the whole shape of the symptom: the join is never
     * answered, and from that moment the firmware stops answering scans and
     * statistics queries that worked minutes earlier, because the station table
     * the AIS network was standing on is gone.
     *
     * The 40-byte body was never the problem -- it is byte-for-byte the layout
     * cnmStaRecChangeState builds for CID 23: index at 0, eStaType at 1 (0x41,
     * STA_TYPE_LEGACY_AP, confirmed as the r1 immediate into
     * bssCreateStaRecFromBssDesc at 0xc0411c98), MAC at 2, and eStaState at 20.
     * The right structure was being sent under the destructor's number.
     */
    CMD_ID_UPDATE_STA_RECORD       = 0x17u,
    /*
     * Named so it cannot be reached for by accident again. Nothing sends it: a
     * station record this driver never allocated by name is not one it has any
     * business freeing, and the AP-side teardown it would be for is handled by
     * the AP's own inactivity timer. It exists as a label on a landmine.
     */
    CMD_ID_REMOVE_STA_RECORD       = 0x18u,
    CMD_ID_INDICATE_PM_BSS_CONNECTED = 0x1au,
    CMD_ID_SCAN_REQ                = 0x1eu,
    CMD_ID_CH_PRIVILEGE            = 0x20u,
    /*
     * CMD_ID_GET_STATISTICS. Read off wlanoidQueryStatistics (0xc03cbf70):
     * `mov r1, #130` is the CID, r2 = 0 selects QUERY over SET, and both the
     * info length and the info buffer it hands wlanSendSetQueryCmd are zero --
     * so the command is a bare header with no body at all.
     */
    CMD_ID_GET_STATISTICS          = 0x82u,
    /*
     * CID 194, MCR: the firmware's own register window, and the only one that
     * survives WIFI_START. See mt6592_wifi_hif_mcr() for the payload derivation.
     */
    CMD_ID_ACCESS_REG              = 0xc2u,
    MCR_PAYLOAD_SIZE               = 8u,
    CMD_ID_BASIC_CONFIG            = 0xc1u,
    EVENT_ID_CMD_RESULT            = 0x01u,
    EVENT_ID_SCAN_RESULT           = 0x04u,
    EVENT_ID_BASIC_CONFIG          = 0x09u,
    /*
     * EID 0x13, EVENT_ID_ACTIVATE_STA_REC_T, "(Unsolicited)"
     * (include/nic_cmd_event.h:789). It is the answer to an UPDATE_STA_RECORD
     * that asked for one, and it is the firmware saying the record is live.
     *
     *     EVENT_ACTIVATE_STA_REC_T (nic_cmd_event.h:1197-1204), 12 bytes:
     *         0  aucMacAddr[6]
     *         6  ucStaRecIdx
     *         7  ucNetworkTypeIndex
     *         8  fgIsQoS
     *         9  fgIsAP
     *         10 aucReserved[2]
     *
     * and stock checks the MAC and the state before acting on it
     * (cnmStaRecHandleEventPkt, mgmt/cnm_mem.c:1234-1252), which is what
     * handle_activate_sta_rec() below does.
     */
    EVENT_ID_ACTIVATE_STA_REC      = 0x13u,
    EVENT_ID_SCAN_DONE             = 0x15u,
    EVENT_ID_TX_DONE               = 0x17u,
    EVENT_ID_CH_PRIVILEGE          = 0x18u,
    EVENT_ID_BSS_BEACON_TIMEOUT    = 0x1bu,

    /*
     * The three 802.11 association states as the firmware numbers them, which
     * is one less than their names (include/nic/mac.h:259-261):
     *
     *     #define STA_STATE_1  0   // Accept Class 1 frames
     *     #define STA_STATE_2  1   // Accept Class 1 & 2 frames
     *     #define STA_STATE_3  2   // Accept Class 1,2 & 3 frames
     */
    STA_STATE_1 = 0u,
    STA_STATE_2 = 1u,
    STA_STATE_3 = 2u,

    /*
     * Station-record indices. This driver keeps one record, index 0, for the AP
     * it is joined to; the other two values are the firmware's reserved ones:
     *
     *     #define STA_REC_INDEX_BMCAST     0xFF
     *     #define STA_REC_INDEX_NOT_FOUND  0xFE
     *
     * (include/mgmt/cnm_mem.h:501-502; que_mgt.h:301 repeats the BMCAST one.)
     * NOT_FOUND is not an error code -- it is what stock puts on a unicast data
     * frame whose station record is not yet valid, and the firmware handles it
     * (qmDetermineStaRecIndex, que_mgt.c:1539-1541).
     */
    STA_RECORD_INDEX          = 0u,
    STA_REC_INDEX_NOT_FOUND   = 0xfeu,
    STA_REC_INDEX_BMCAST      = 0xffu,

    HIF_TX_PACKET_TYPE_CMD   = 1u,
    HIF_TX_PACKET_TYPE_SHIFT = 6u,
    HIF_TX_RESOURCE_SHIFT    = 2u,
    HIF_TX_COMMAND_RESOURCE  = 4u,

    /* ENUM_SCAN_TYPE_T. Passive listens for beacons and never transmits;
     * active also sends probe requests, which is the first thing in this whole
     * bring-up that keys the transmitter. Both fill the same command byte at
     * payload offset 2 -- confirmed against stock's scnSendScanReq, which
     * builds the identical 110-byte layout. */
    SCAN_TYPE_PASSIVE  = 0u,
    SCAN_TYPE_ACTIVE   = 1u,
    SCAN_SSID_WILDCARD = BIT(0),
    SCAN_CHANNEL_2G4   = 1u,
    NETWORK_TYPE_AIS   = 0u,

    MTK_WIFI_SIGNATURE         = 0x574b544du,
    MAX_FIRMWARE_SECTIONS      = 16u,
    FIRMWARE_CHUNK_SIZE        = 2048u,

    /*
     * CFG_FW_START_ADDRESS -- the entry point WIFI_START is given.
     *
     * A compile-time constant in stock, not anything read out of the image, and
     * both sources agree on the value:
     *
     *   config.h:1316-1319, the MT6628 arm (drv_wlan/mt_wifi/wlan/Makefile:5
     *   compiles this driver with -DMT6628, so that is the arm the SOC takes):
     *       #define CFG_FW_LOAD_ADDRESS   0x00060000
     *       #define CFG_FW_START_ADDRESS  0x00060000
     *
     *   and the J36 kernel itself, in wlanProbe:
     *       c03f02a4  mov r3, #0x60000
     *       c03f02b0  str r3, [r7, #0x370]   ; prRegInfo->u4StartAddress  (+12)
     *       c03f02b4  str r3, [r7, #0x374]   ; prRegInfo->u4LoadAddress   (+16)
     *   with r7+0x364 the prRegInfo that the memset above it clears for
     *   0x2e4 = sizeof(REG_INFO_T) bytes.
     *
     * This driver used to send section 0's destination instead, which for
     * WIFI_RAM_CODE_SOC is 0x0006a000 -- 0x2800 past the entry, in the middle of
     * the first section. That is a jump into the wrong place, and "the firmware
     * executes nothing" is exactly what it would look like. 0x00060000 is also
     * where the WMT layer put ROMv1_patch_1_0_hdr.bin (see the patch-address note
     * in mt6592_wifi_wmt.c), so the entry veneer is resident before the WLAN
     * image is downloaded at all -- which is why no section targets it.
     */
    FIRMWARE_START_ADDRESS     = 0x00060000u,
    INIT_DOWNLOAD_HEADER_SIZE  = 24u,
    INIT_START_PACKET_SIZE     = 16u,
    INIT_ACCESS_REG_PACKET_SIZE  = 20u,
    INIT_QUERY_ERROR_PACKET_SIZE = 8u,
    NORMAL_COMMAND_HEADER_SIZE = 8u,
    HIF_DATA_HEADER_SIZE       = 16u,
    SCAN_COMMAND_PAYLOAD_SIZE  = 110u,

    TX_BUFFER_SIZE          = 2176u,
    RX_BUFFER_SIZE          = 4096u,
    MAX_SCAN_RESULTS        = 32u,
    MAX_RX_PACKETS_PER_POLL = 32u,
    RX_ETHERNET_QUEUE_DEPTH = 8u,
    MAX_ETHERNET_FRAME_SIZE = 1600u,
    MAX_ASSOC_FRAME_SIZE    = 256u,
    MAX_RSN_IE_SIZE         = 64u,

    DRIVER_OWN_TIMEOUT_US     = 250000u,
    INIT_ACK_TIMEOUT_US       = 1000000u,
    FIRMWARE_READY_TIMEOUT_US = 6000000u,
    SCAN_TIMEOUT_US           = 12000000u,
    CHANNEL_TIMEOUT_US        = 1500000u,
    AUTH_TIMEOUT_US           = 1000000u,
    ASSOC_TIMEOUT_US          = 1500000u,
    EAPOL_TIMEOUT_US          = 12000000u,
    /*
     * How long to wait for the firmware to confirm message 4 was transmitted
     * before installing the pairwise key regardless. Short, because the AP is
     * already counting: hostapd's default dot11RSNAConfigPairwiseUpdateTimeOut
     * is one second per try, so a stall longer than this has already cost a
     * retransmission and waiting further only risks the whole handshake.
     */
    M4_TX_DONE_TIMEOUT_US     = 300000u,
};

static mt6592_wifi_hif_state g_state = {
    .status  = "MediaTek WLAN HIF not bound",
    .blocked = "mt6592-wifi:hif-not-bound",
};

static uint32_t g_tx_words[TX_BUFFER_SIZE / sizeof(uint32_t)];
static uint32_t g_rx_words[(RX_BUFFER_SIZE + sizeof(uint32_t) - 1u) / sizeof(uint32_t)];

/*
 * Diagnostic override for the WIFI_START entry point; 0 means "use
 * FIRMWARE_START_ADDRESS", which is the constant stock passes and what every
 * non-experimental boot wants. It exists so an alternative entry (a section
 * destination, say) can be tried from the console without a rebuild-and-reflash
 * round trip.
 */
static uint32_t g_start_address_override;

/*
 * The u4Override word of INIT_CMD_WIFI_START, which is a separate question from
 * the address and until now was not askable at all -- send_init_start hardcoded
 * a 1.
 *
 * It has to be askable because the two sources of truth disagree about it, and
 * the disagreement is not the usual kind. wlanConfigWifiFunc's callers are all
 * spelled
 *
 *     #if CFG_OVERRIDE_FW_START_ADDRESS
 *         wlanConfigWifiFunc(prAdapter, TRUE, kalGetFwStartAddress(...));
 *     #else
 *         wlanConfigWifiFunc(prAdapter, FALSE, 0);
 *     #endif
 *
 * (wlan_lib.c:1414-1423 and nic_pwr_mgt.c:545-553, the only four call sites),
 * and drv_wlan/mt_wifi/Makefile:5 compiles the driver `-DLINUX -DMT6628', which
 * selects the config.h:1315-1319 arm -- CFG_OVERRIDE_FW_START_ADDRESS 0. So the
 * vendor tree builds the FALSE/0 arm, and the argument is not "address 0", it is
 * "no address: run the entry the image declares".
 *
 * The J36 kernel's own wlanAdapterStart takes the other arm (mov r1,#1 at
 * c03c0f9c, then ldr r2,[r3,#12]), which is why this driver shipped with 1. That
 * is normally decisive -- the kernel is what the chip was shipped with -- but it
 * is decisive about a kernel driving a *Linux-initialised* chip, and the thing
 * we cannot reproduce is precisely the initialisation. With override=1 the
 * firmware has never asserted WLAN_READY, and the address we pass with it,
 * 0x00060000, is inside the range the WMT ROM patch is already executing from
 * (the pre-START PC samples read 0x00066382), so "jump here" may well be a jump
 * into live WMT code rather than into the WLAN image.
 *
 * Hence a knob rather than a flip: this is a hypothesis with hardware on the
 * other end, and the console can now put either value on the wire.
 */
static uint32_t g_start_override_word = 1u;

/*
 * Diagnostic: stop the load after the sections are down and before the entry
 * jump, so the console can read connectivity SRAM either side of it.
 *
 * The firmware currently takes the jump and then says nothing at all -- no
 * event, no WLAN_READY -- and "it never executed an instruction" and "it ran and
 * fell over" need different fixes. Connectivity SRAM is AP-visible (section 1's
 * destination 0x0209f800 reads back at 0x1809f800), so a page-checksum sweep
 * before and after WIFI_START separates the two.
 */
static int g_defer_start;

typedef enum
{
    WIFI_SECURITY_OPEN = 0,
    WIFI_SECURITY_WPA2_PSK_CCMP = 1,
    WIFI_SECURITY_UNSUPPORTED = 2,
} wifi_security_mode;

typedef struct
{
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t dtim_period;
    uint8_t rcpi;
    uint16_t capability;
    uint16_t beacon_interval;
    uint16_t operational_rates;
    uint16_t basic_rates;
    uint8_t rates[16];
    uint8_t rate_count;
    uint8_t rsn_ie[MAX_RSN_IE_SIZE];
    uint8_t rsn_ie_len;
    wifi_security_mode security;
} wifi_bss_profile;

typedef enum
{
    ASSOC_IDLE = 0,
    ASSOC_WAIT_CHANNEL,
    ASSOC_WAIT_AUTH,
    ASSOC_WAIT_ASSOC,
    ASSOC_WAIT_EAPOL_M1,
    ASSOC_WAIT_EAPOL_M3,
    ASSOC_CONNECTED,
    ASSOC_FAILED,
} wifi_assoc_phase;

/* mt6592_wifi_hif_get_state() casts this enum straight into assoc_phase, so the
 * public WIFI_ASSOC_* list has to stay in step with it. Checked here rather than
 * trusted, because the two lists are in different files and nothing else would
 * notice a value inserted in the middle of one of them. */
_Static_assert((int)ASSOC_WAIT_CHANNEL == (int)WIFI_ASSOC_WAIT_CHANNEL, "assoc phase enums diverged");
_Static_assert((int)ASSOC_WAIT_AUTH == (int)WIFI_ASSOC_WAIT_AUTH, "assoc phase enums diverged");
_Static_assert((int)ASSOC_WAIT_ASSOC == (int)WIFI_ASSOC_WAIT_ASSOC, "assoc phase enums diverged");
_Static_assert((int)ASSOC_WAIT_EAPOL_M1 == (int)WIFI_ASSOC_WAIT_EAPOL_M1, "assoc phase enums diverged");
_Static_assert((int)ASSOC_WAIT_EAPOL_M3 == (int)WIFI_ASSOC_WAIT_EAPOL_M3, "assoc phase enums diverged");
_Static_assert((int)ASSOC_CONNECTED == (int)WIFI_ASSOC_CONNECTED, "assoc phase enums diverged");
_Static_assert((int)ASSOC_FAILED == (int)WIFI_ASSOC_FAILED, "assoc phase enums diverged");

static mt6592_wifi_scan_result g_scan_results[MAX_SCAN_RESULTS];
static wifi_bss_profile g_scan_profiles[MAX_SCAN_RESULTS];
static uint8_t g_cmd_sequence;
static uint8_t g_scan_sequence;
static uint8_t g_tx_sequence;
static uint64_t g_scan_deadline_us;
static uint64_t g_scan_started_us;

static wifi_assoc_phase g_assoc_phase;
static wifi_bss_profile g_assoc_profile;
static char g_assoc_password[65];
static uint8_t g_wifi_mac[6] = {0x68u, 0x52u, 0xd6u, 0x05u, 0x7cu, 0x28u};
static uint8_t g_channel_token;
/*
 * Is the firmware currently owing us a channel? The token alone cannot say: it
 * survives the release so a late grant event can still be matched against the
 * request it answers, which means "token != 0" is "we asked once", not "the
 * radio is still pinned". Releasing twice is worse than not tracking it -- the
 * second release carries a token the firmware has already retired.
 */
static uint8_t g_channel_held;
static uint8_t g_assoc_retries;
static uint16_t g_assoc_aid;
static uint64_t g_assoc_deadline_us;
/*
 * prStaRec->fgIsValid, as one index instead of a flag.
 *
 * Stock will not put a station-record index on a unicast data frame until the
 * firmware has confirmed the record. qmDetermineStaRecIndex (nic/que_mgt.c:1511-1540)
 * walks the records looking for `fgIsAp && fgIsValid`, and if it finds none:
 *
 *     //4 <4> No STA found, Not BMCAST --> Indicate NOT_FOUND to FW
 *     prMsduInfo->ucStaRecIndex = STA_REC_INDEX_NOT_FOUND;
 *
 * fgIsValid is set in exactly one place, qmActivateStaRec (que_mgt.c:800), which
 * is reached from exactly one place, cnmStaRecHandleEventPkt on
 * EVENT_ID_ACTIVATE_STA_REC_T. So this holds STA_REC_INDEX_NOT_FOUND from reset
 * until that event arrives and STA_RECORD_INDEX afterwards, and the data path
 * degrades the way stock's does rather than naming a record the firmware has not
 * finished installing.
 */
static uint8_t g_sta_rec_index = STA_REC_INDEX_NOT_FOUND;

/*
 * The tail of the four-way handshake, deferred until message 4 is off the chip.
 *
 * Installing the pairwise key is what ends the handshake, and it must not
 * happen while message 4 is still queued in the firmware: the AP does not
 * install its own pairwise key until message 4 arrives, so a message 4 that
 * gets CCMP-encrypted on the way out is unreadable and the AP times the
 * handshake out -- reason 15, which is exactly what the board reports.
 *
 * Writing the frame to WTDR1 is not transmission. The command that installs the
 * key goes out on the same port and can still be executed by the firmware ahead
 * of a data frame that is only queued, so the ordering has to be enforced
 * against the TX-status event rather than against the order of the writes.
 *
 * g_m4_tx_seq is the ucPacketSeqNo tagged on message 4, and zero when nothing is
 * pending. The group key travels with it because message 3 carried it and the
 * buffer it was unwrapped into is a local that does not survive the return.
 */
static uint8_t  g_last_tx_sequence;
static uint8_t  g_m4_tx_seq;
static uint64_t g_m4_deadline_us;
static uint8_t  g_m4_gtk[192];
static uint32_t g_m4_gtk_len;
static uint32_t g_m4_tx_done_timeouts;
static void complete_four_way(void);
/*
 * Stock's `if (!prAisBssInfo->ucDTIMPeriod)` guard (mgmt/scan.c:2488), which is
 * both "we do not know the schedule yet" and "we have not announced it yet" in
 * one test because learning it is what announces it. Kept as its own flag rather
 * than reused from g_assoc_profile.dtim_period, since the scan writes that field
 * before the association exists.
 */
static uint8_t g_pm_connected_sent;
static uint8_t g_pmk[32];
static uint8_t g_snonce[32];
static uint8_t g_anonce[32];
static uint8_t g_ptk[64];
static uint64_t g_eapol_replay_counter;
static uint8_t g_rx_ethernet[RX_ETHERNET_QUEUE_DEPTH][MAX_ETHERNET_FRAME_SIZE];
static uint16_t g_rx_ethernet_len[RX_ETHERNET_QUEUE_DEPTH];
static uint8_t g_rx_ethernet_head;
static uint8_t g_rx_ethernet_tail;

static uint8_t* tx_buffer(void)
{
    return (uint8_t*)g_tx_words;
}

static uint8_t* rx_buffer(void)
{
    return (uint8_t*)g_rx_words;
}

static uint16_t read_le16(const uint8_t* data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le16(uint8_t* data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint16_t read_be16(const uint8_t* data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint64_t read_be64(const uint8_t* data)
{
    uint64_t value = 0u;
    for (uint32_t i = 0; i < 8u; ++i) value = (value << 8u) | data[i];
    return value;
}

static void write_be16(uint8_t* data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be64(uint8_t* data, uint64_t value)
{
    for (uint32_t i = 0; i < 8u; ++i) data[7u - i] = (uint8_t)(value >> (i * 8u));
}

static uint32_t string_length(const char* text, uint32_t limit)
{
    uint32_t length = 0u;
    if (!text) return 0u;
    while (length < limit && text[length] != '\0') ++length;
    return length;
}

static void copy_string(char* dst, uint32_t capacity, const char* src)
{
    uint32_t i = 0u;
    if (!dst || capacity == 0u) return;
    if (src)
    {
        while (i + 1u < capacity && src[i] != '\0')
        {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static int same_string(const char* a, const char* b)
{
    if (!a || !b) return 0;
    uint32_t i = 0u;
    while (a[i] != '\0' && b[i] != '\0')
    {
        if (a[i] != b[i]) return 0;
        ++i;
    }
    return a[i] == b[i];
}

static int same_mac(const uint8_t a[6], const uint8_t b[6])
{
    for (uint32_t i = 0; i < 6u; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

static int multicast_mac(const uint8_t mac[6])
{
    return (mac[0] & 1u) != 0u;
}

static void zero_bytes(uint8_t* data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        data[i] = 0u;
    }
}

static void copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        dst[i] = src[i];
    }
}

static uint32_t align4(uint32_t value)
{
    return (value + 3u) & ~3u;
}

static uint32_t mmio_read(uint32_t offset)
{
    return *(volatile uint32_t*)(uintptr_t)(HIF_BASE + offset);
}

static void mmio_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(uintptr_t)(HIF_BASE + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* The connectivity MCU's program counter. See CONSYS_MCU_CPUPCR above. */
uint32_t mt6592_wifi_hif_read_cpupcr(void)
{
    return *(volatile uint32_t*)(uintptr_t)CONSYS_MCU_CPUPCR;
}

/*
 * A burst trace of that program counter, fired the instant a command packet is
 * written to the HIF.
 *
 * The 10 ms readiness poll is far too coarse for this. It caught the ROM's
 * command handler once, on its very first sample -- @0:0x000028f8 -- and by the
 * next sample 10 ms later the core was already back in its idle loop at
 * 0x00066382. So the whole of what the ROM does with WIFI_START happens inside
 * one poll tick, and the poll can only tell us that it happened, not what it was.
 *
 * This samples in a tight loop with nothing in between, so the spacing is one
 * bus read -- fast enough to sit inside that handler. It has to be armed
 * explicitly and fires once, because the download alone writes 129 packets and
 * tracing all of them would say nothing.
 *
 * The point is comparative. ACCESS_REG works: the ROM answers it, and it goes
 * out through the same write_port() on the same port. Trace that, trace
 * WIFI_START, and diff the addresses. What is common is the HIF receive path;
 * what is unique to WIFI_START is the dispatch path, and the address it stops
 * at is where the ROM decides not to jump.
 */
enum { PC_TRACE_MAX = 2048u };
static uint32_t g_pc_trace[PC_TRACE_MAX];
static uint32_t g_pc_trace_count;
static uint32_t g_pc_trace_gap;
static int g_pc_trace_armed;

/*
 * gap_microseconds trades resolution for reach. At 0 the samples are one bus
 * read apart, which covers roughly half a millisecond -- the tightest look at
 * the handler, and the right first try, since the poll caught the ROM still
 * inside it on the sample immediately after the write. If that trace comes back
 * as nothing but the idle address the handler had not started yet, and the same
 * buffer at gap=2 covers four milliseconds instead. Widening it costs a command,
 * not a rebuild.
 */
void mt6592_wifi_hif_pc_trace_arm(uint32_t gap_microseconds)
{
    g_pc_trace_count = 0u;
    g_pc_trace_gap   = gap_microseconds;
    g_pc_trace_armed = 1;
}

static void pc_trace_capture(void)
{
    if (!g_pc_trace_armed) return;
    g_pc_trace_armed = 0;
    for (uint32_t i = 0u; i < PC_TRACE_MAX; ++i)
    {
        g_pc_trace[i] = *(volatile uint32_t*)(uintptr_t)CONSYS_MCU_CPUPCR;
        if (g_pc_trace_gap != 0u)
        {
            mt6592_delay_cycles(g_pc_trace_gap * (uint32_t)MT6592_DELAY_LEGACY_CYCLES_PER_US);
        }
    }
    g_pc_trace_count = PC_TRACE_MAX;
}

const uint32_t* mt6592_wifi_hif_pc_trace(uint32_t* count)
{
    *count = g_pc_trace_count;
    return g_pc_trace;
}

static void delay_us(uint32_t microseconds)
{
    mt6592_delay_cycles(microseconds * (uint32_t)MT6592_DELAY_LEGACY_CYCLES_PER_US);
}

static uint8_t next_command_sequence(void)
{
    ++g_cmd_sequence;
    if (g_cmd_sequence == 0u)
    {
        ++g_cmd_sequence;
    }
    return g_cmd_sequence;
}

static void set_failure(const char* status, const char* blocked)
{
    const int changed = g_state.blocked != blocked;
    g_state.status    = status;
    g_state.blocked   = blocked;
    if (changed)
    {
        mt6592_uart_puts("  wifi: ");
        mt6592_uart_puts(status ? status : "driver failure");
        mt6592_uart_puts(" [");
        mt6592_uart_puts(blocked ? blocked : "unknown");
        mt6592_uart_puts("]\n");
        mt6592_bootstatus_log_text("wifi: ");
        mt6592_bootstatus_log_text(status ? status : "driver failure");
        mt6592_bootstatus_log_text(" [");
        mt6592_bootstatus_log_text(blocked ? blocked : "unknown");
        mt6592_bootstatus_log_text("]\n");
    }
}

/* Defined with the other command builders ~1200 lines down; the failure path
 * that has to send them is here. */
static int  send_bss_disconnect(void);
static int  send_sta_record_remove(void);
static int  send_bss_reactivate(void);
static void release_channel_if_held(void);

/*
 * A failed association has to be unwound in the *firmware*, not just in these
 * variables. This used to clear the driver's state and stop, which left the
 * firmware holding everything the join had built: a BSS still in media state
 * CONNECTED, a station record still in STA_STATE_3, and an outstanding channel
 * privilege. Nothing ever took those back, so:
 *
 *   - the next join asked for a channel privilege while the firmware still owed
 *     one for the previous attempt, and sat out its grant timeout instead;
 *   - the firmware went on spending TC4 pages retransmitting to an AP nobody was
 *     servicing, and TC4 is four pages deep, so once they were all in flight for
 *     a dead link every later command -- including every `wifi scan' -- was
 *     refused for want of a page.
 *
 * Which is the "one WPA2 timeout wedges the radio until reboot" behaviour. The
 * EAPOL timeout is what fails; this is what made the failure permanent, and why
 * `wifi fw' did not clear it -- nothing here was ever the firmware's fault.
 *
 * It is the same leak mt6592_wifi_hif_abort_association() was written to fix for
 * `wifi drop', and this path could not benefit from that fix: it cleared
 * g_state.associated first, so by the time `wifi drop' ran, that function's own
 * `if (g_state.associated)' guard was false and it skipped the teardown it
 * exists to perform.
 *
 * Hence the ordering below -- unwind the firmware while the state describing
 * what to unwind is still true, and only then forget it. send_bss_disconnect()
 * in particular is matched against the live g_assoc_profile; see its note.
 *
 * No acquire_driver_own() here: every caller is already inside the poll loop or
 * the join path, both of which hold ownership. The one caller that does not --
 * the ownership-lost failure itself -- would have its commands refused, which is
 * the correct outcome and is why the results are discarded.
 */
static void set_assoc_failure(const char* status, const char* blocked)
{
    if (g_state.associated)
    {
        (void)send_bss_disconnect();
        (void)send_sta_record_remove();
        /* And drop the firmware's BSS context, or its receive filter keeps
         * swallowing this BSSID's beacons and the AP we just lost is missing
         * from every subsequent scan. See send_bss_reactivate(). */
        (void)send_bss_reactivate();
    }
    release_channel_if_held();

    g_assoc_phase = ASSOC_FAILED;
    g_state.auth_active = 0;
    g_state.associated = 0;
    g_state.data_path_ready = 0;
    /* qmDeactivateStaRec's `prStaRec->fgIsValid = FALSE` (que_mgt.c:855), which
     * stock runs on any transition out of STA_STATE_3 (cnmStaRecChangeState,
     * mgmt/cnm_mem.c:1070-1074). The next join has to earn its record again. */
    g_sta_rec_index = STA_REC_INDEX_NOT_FOUND;
    /* Nothing left to order the key install against, and leaving the tag set
     * would let a stale TX-status event from the dead association drive
     * complete_four_way() into the next one. */
    g_m4_tx_seq = 0u;
    g_pm_connected_sent = 0u;
    g_state.secure = 0;
    set_failure(status, blocked);
}

static void reset_association(void)
{
    g_assoc_phase = ASSOC_IDLE;
    g_assoc_retries = 0u;
    g_assoc_deadline_us = 0u;
    g_eapol_replay_counter = 0u;
    g_state.auth_active = 0;
    g_state.associated = 0;
    g_state.data_path_ready = 0;
    g_sta_rec_index = STA_REC_INDEX_NOT_FOUND; /* see set_assoc_failure */
    g_pm_connected_sent = 0u;
    g_state.secure = 0;
    g_rx_ethernet_head = 0u;
    g_rx_ethernet_tail = 0u;
    zero_bytes((uint8_t*)&g_assoc_profile, sizeof(g_assoc_profile));
    zero_bytes((uint8_t*)g_assoc_password, sizeof(g_assoc_password));
    zero_bytes(g_pmk, sizeof(g_pmk));
    zero_bytes(g_snonce, sizeof(g_snonce));
    zero_bytes(g_anonce, sizeof(g_anonce));
    zero_bytes(g_ptk, sizeof(g_ptk));
}

static void generate_snonce(void)
{
    uint8_t seed[32];
    uint8_t digest[20];
    for (uint32_t i = 0; i < sizeof(seed); ++i)
        seed[i] = (uint8_t)(mmio_read(MCR_WCIR) >> ((i & 3u) * 8u));
    const uint64_t now = mt6592_timer_microseconds();
    for (uint32_t i = 0; i < 8u; ++i) seed[i] ^= (uint8_t)(now >> (i * 8u));
    copy_bytes(seed + 8u, g_wifi_mac, sizeof(g_wifi_mac));
    for (uint8_t block = 0u; block < 2u; ++block)
    {
        seed[31] ^= block;
        mt6592_wifi_hmac_sha1(g_pmk, sizeof(g_pmk), seed, sizeof(seed), digest);
        const uint32_t offset = (uint32_t)block * 20u;
        uint32_t take = sizeof(g_snonce) - offset;
        if (take > sizeof(digest)) take = sizeof(digest);
        copy_bytes(g_snonce + offset, digest, take);
    }
}

static int supported_chip_id(uint16_t chip_id)
{
    return chip_id == 0x6572u || chip_id == 0x6582u || chip_id == 0x6592u;
}

static void configure_transfer(uint32_t target, uint32_t size)
{
    const uint32_t count = align4(size) & HSTCR_COUNT_MASK;

    /*
     * Stock AHB HIF workaround: touch a non-data register between transfers.
     * HifAhbDmaEnhanceModeConf (0xc0410100) does *two* dummy reads, WHIER at 0x14
     * and then HSTCR itself at 0x58, each followed by a dsb, before composing the
     * new HSTCR word. We only ever did the first. On a posted AHB bus the second
     * read is what guarantees the previous descriptor's write has retired before
     * the next one overwrites it, so omitting it is not cosmetic.
     */
    (void)mmio_read(MCR_WHIER);
    (void)mmio_read(MCR_HSTCR);
    mmio_write(MCR_HSTCR, (HSTCR_BURST_4_DW << HSTCR_BURST_SHIFT) | (target << HSTCR_TARGET_SHIFT) | count);
}

static void write_port(uint32_t port, uint32_t target, const uint8_t* data, uint32_t size)
{
    const uint32_t words = align4(size) / 4u;

    configure_transfer(target, size);
    for (uint32_t i = 0; i < words; ++i)
    {
        const uint32_t offset = i * 4u;
        uint32_t value        = 0u;
        for (uint32_t byte = 0; byte < 4u && offset + byte < size; ++byte)
        {
            value |= (uint32_t)data[offset + byte] << (byte * 8u);
        }
        mmio_write(port, value);
    }
}

static int read_port(uint32_t port, uint32_t target, uint8_t* data, uint32_t size, uint32_t capacity)
{
    const uint32_t words = align4(size) / 4u;
    const int fits       = size <= capacity;

    configure_transfer(target, size);
    for (uint32_t i = 0; i < words; ++i)
    {
        const uint32_t value  = mmio_read(port);
        const uint32_t offset = i * 4u;
        if (offset < capacity)
        {
            for (uint32_t byte = 0; byte < 4u && offset + byte < capacity; ++byte)
            {
                data[offset + byte] = (uint8_t)(value >> (byte * 8u));
            }
        }
    }
    return fits ? 0 : -1;
}

/*
 * ── TX PAGE ACCOUNTING: THE THING THIS DRIVER NEVER DID ──
 *
 * WTSR0 and WTSR1 are not status words to be glanced at and dropped. They are
 * six bytes of *freed page counts*, one per traffic class, and the driver is
 * required to hold a running credit against them. This file read both registers
 * and threw both values away, which is why `wifi join` takes the board off the
 * USB bus: with no credit, the fifth command in a row is written into a HIF FIFO
 * that has no room for it, the AHB write never retires, and the CPU stops inside
 * a bus transaction -- no exception, no watchdog, no more USB.
 *
 * Everything below is read out of the stock kernel, not inferred:
 *
 *   nicTxAcquireResource(adapter, ucTC)   0xc03dd5a0
 *       reads [adapter+0x72b8+tc]; zero -> returns 0xc000009a
 *       (RESOURCE_NOT_ENOUGH); otherwise decrements by exactly ONE and returns 0.
 *       One page per packet, whatever the packet's length.
 *
 *   nicTxReleaseResource(adapter, pucFreeCount)   0xc03dd694
 *       adds pucFreeCount[tc] to [adapter+0x72b8+tc] for six classes and clamps
 *       each against the per-class maximum at [adapter+0x72be+tc].
 *
 *   nicTxPollingResource(adapter, ucTC)   0xc03dd96c
 *       up to 255 rounds of: kalDevRegRead(0x20) and kalDevRegRead(0x24) into one
 *       8-byte local, nicTxReleaseResource(adapter, that local), sleep 50 ms,
 *       retry. That is what makes WTSR0/WTSR1 the six freed counts -- WTSR0 is
 *       TC0..TC3 little-endian, WTSR1 is TC4 and TC5.
 *
 *   nicTxResetResource(adapter)   0xc03ddbc0     run-time table below
 *   nicTxInitResetResource(...)   0xc03dfcb4     download table below
 *   nicTxCmd(...)                 0xc03ddfa0     port = (ucTC == 4) ? 0x2c : 0x28
 *
 * That last line is the one that makes this the join bug rather than a tidiness
 * complaint: TC4 is the class every command and every management/EAPOL frame
 * uses here, TC4 is the class that maps to WTDR1, and TC4 has FOUR pages. A scan
 * spends one. A join spends CH_PRIVILEGE, UPDATE_STA_RECORD, AUTH, ASSOC,
 * SET_BSS_INFO, PM_CONNECTED and EAPOL-M2 back to back, and the class is dry
 * long before the last of them. It also explains the note already in this file
 * that a `wifi scan` run *after* a failed join has twice killed the link: the
 * class was still dry from the join.
 */
enum
{
    /* Six, from the header, so the console's arrays cannot drift from ours. */
    HIF_TX_CLASSES = MT6592_WIFI_TX_CLASSES,

    /*
     * Stock waits 255 x 50 ms = 12.75 s for a page. That is a sane number for a
     * kernel with a scheduler behind it and a terrible one for a console the
     * operator is watching: a firmware that has stopped crediting would look
     * exactly like a hang. 200 x 1 ms = 200 ms is thousands of times longer than
     * the microseconds a live firmware needs, and when it does expire the command
     * fails with a name instead of parking the CPU on the bus.
     */
    TX_RESOURCE_POLL_ROUNDS      = 200u,
    TX_RESOURCE_POLL_INTERVAL_US = 1000u,
};

/* nicTxResetResource, 0xc03ddbc0 -- what the classes are worth once the
 * firmware is running. Free counts land at +0x72b8, maxima at +0x72be. */
static const uint8_t kTxPagesRuntime[HIF_TX_CLASSES] = { 1u, 20u, 1u, 1u, 4u, 1u };

/* nicTxInitResetResource, 0xc03dfcb4 -- during download only TC0 exists, with
 * eight pages (`strb r1(#8),[r3,#0xb8]` and the same to +0xbe, zeros elsewhere). */
static const uint8_t kTxPagesDownload[HIF_TX_CLASSES] = { 8u, 0u, 0u, 0u, 0u, 0u };

/* The counts live in g_state so `wifi` can print them with everything else it
 * prints; the header documents each field. */
static void tx_resource_reset(const uint8_t* table)
{
    for (uint32_t tc = 0u; tc < HIF_TX_CLASSES; ++tc)
    {
        g_state.tx_free[tc] = table[tc];
        g_state.tx_max[tc]  = table[tc];
    }
}

/*
 * One round of nicTxPollingResource's body: read both status registers as the
 * six freed counts they are, credit each class, clamp against its maximum.
 *
 * This replaces drain_tx_status() everywhere, including the sites that only ever
 * wanted the FIFO emptied -- the reads are the same two reads, and a read that
 * discards its value is a credit thrown on the floor, because these registers
 * clear when read.
 */
static void tx_resource_release(void)
{
    const uint32_t status0 = mmio_read(MCR_WTSR0);
    const uint32_t status1 = mmio_read(MCR_WTSR1);
    const uint8_t freed[HIF_TX_CLASSES] = {
        (uint8_t)(status0 & 0xffu),
        (uint8_t)((status0 >> 8u) & 0xffu),
        (uint8_t)((status0 >> 16u) & 0xffu),
        (uint8_t)((status0 >> 24u) & 0xffu),
        (uint8_t)(status1 & 0xffu),
        (uint8_t)((status1 >> 8u) & 0xffu),
    };

    for (uint32_t tc = 0u; tc < HIF_TX_CLASSES; ++tc)
    {
        uint32_t credit = (uint32_t)g_state.tx_free[tc] + (uint32_t)freed[tc];
        if (credit > (uint32_t)g_state.tx_max[tc]) credit = (uint32_t)g_state.tx_max[tc];
        g_state.tx_credited += freed[tc];
        g_state.tx_free[tc] = (uint8_t)credit;
    }
}

/*
 * nicTxAcquireResource with stock's caller folded in: take a page, and if the
 * class is dry, poll for one the way nicTxPollingResource does.
 *
 * Nothing writes a packet to a TX port without coming through here first. A
 * failure is a refused send, which the caller reports; the one thing it is not
 * is a write into a full FIFO.
 */
static int tx_resource_acquire(uint32_t tc)
{
    if (tc >= HIF_TX_CLASSES) return -1;

    if (g_state.tx_free[tc] != 0u)
    {
        --g_state.tx_free[tc];
        return 0;
    }

    ++g_state.tx_waits;
    for (uint32_t round = 0u; round < TX_RESOURCE_POLL_ROUNDS; ++round)
    {
        tx_resource_release();
        if (g_state.tx_free[tc] != 0u)
        {
            --g_state.tx_free[tc];
            return 0;
        }
        delay_us(TX_RESOURCE_POLL_INTERVAL_US);
        if ((round & 0x0fu) == 0x0fu) wifi_cooperative_yield();
    }

    ++g_state.tx_starved;

    /*
     * The one thing here that is not stock, and the reason it is here.
     *
     * If the firmware has never credited a single page in this session, then the
     * table above is a budget nothing has confirmed -- and refusing a send on the
     * strength of it would break `wifi scan`, which works today, on the strength
     * of a model that has never been checked against this chip. So let it through
     * and count it. The first credit that ever arrives shuts the valve for good,
     * and `wifi` prints both numbers, so one run says which world we are in:
     * credited>0 means the accounting is real and starve counts are real refusals;
     * credited=0 with forced>0 means WTSR is not reporting and the four-page
     * window was never a budget in the first place.
     */
    if (g_state.tx_credited == 0u)
    {
        ++g_state.tx_forced;
        return 0;
    }

    set_failure("MediaTek WLAN firmware freed no TX page for this traffic class",
                "mt6592-wifi:tx-resource-starved");
    return -1;
}

static int acquire_driver_own(void)
{
    const uint64_t deadline = mt6592_timer_microseconds() + DRIVER_OWN_TIMEOUT_US;
    uint32_t polls = 0u;

    if (mmio_read(MCR_WHLPCR) & WHLPCR_IS_DRIVER_OWN)
    {
        g_state.driver_own = 1;
        return 0;
    }

    mmio_write(MCR_WHLPCR, WHLPCR_DRIVER_OWN_REQ);
    while (mt6592_timer_microseconds() < deadline)
    {
        if (mmio_read(MCR_WHLPCR) & WHLPCR_IS_DRIVER_OWN)
        {
            g_state.driver_own = 1;
            return 0;
        }
        delay_us(8u);
        if ((++polls & 0x7fu) == 0u) wifi_cooperative_yield();
    }

    g_state.driver_own = 0;
    set_failure("MediaTek WLAN HIF did not grant driver ownership", "mt6592-wifi:hif-driver-own-timeout");
    return -1;
}

static int configure_hif(void)
{
    uint32_t wcir          = mmio_read(MCR_WCIR);
    const uint16_t chip_id = (uint16_t)(wcir & WCIR_CHIP_ID_MASK);

    g_state.hif_chip_id  = chip_id;
    g_state.hif_revision = (uint8_t)((wcir & WCIR_REVISION_MASK) >> WCIR_REVISION_SHIFT);
    if (!supported_chip_id(chip_id))
    {
        set_failure("MediaTek WLAN AHB HIF returned an unexpected chip ID", "mt6592-wifi:hif-chip-id-invalid");
        return -1;
    }

    if (acquire_driver_own() != 0)
    {
        return -1;
    }

    mmio_write(MCR_WHLPCR, WHLPCR_INT_EN_CLR);

    /*
     * Stock's whole HIF programming, in stock's order: nicSDIOInit (0xc03d8744)
     * does one read-modify-write of WHCR clearing bit 16 and bits [7:4] and
     * nothing else, and nicInitializeAdapter (0xc03d90b4) then writes WHIER.
     * nicMCRInit and nicHifInit are both empty on this SOC.
     *
     * MVII used to also clear bits 1 and 2 and set MAX_HIF_RX_LEN_NUM to 1.
     * Neither is anything stock does, and with the firmware refusing to come up
     * after a byte-identical download there is no room for embellishment here:
     * bits stock preserves are preserved, and the RX length count stays 0.
     */
    uint32_t whcr = mmio_read(MCR_WHCR);
    whcr &= ~(WHCR_RX_ENHANCE_MODE_EN | WHCR_MAX_HIF_RX_LEN_MASK);
    mmio_write(MCR_WHCR, whcr);

    mmio_write(MCR_WHIER, WHIER_DEFAULT);

    /*
     * The download table, before anything is credited against it. Stock's
     * nicTxInitResetResource runs at the same point in the same order: the HIF is
     * up, the firmware is not, and the only class that exists is TC0 with eight
     * pages. The run-time table is installed later, where WLAN_READY is seen.
     */
    tx_resource_reset(kTxPagesDownload);

    g_state.last_whisr = mmio_read(MCR_WHISR);
    tx_resource_release();
    g_state.last_wrplr = mmio_read(MCR_WRPLR);
    g_state.hif_ready  = 1;
    g_state.status     = "MediaTek WLAN AHB HIF ready";
    g_state.blocked    = 0;
    return 0;
}

static uint32_t crc32(const uint8_t* data, uint32_t size);

/*
 * The container's own CRC32, exposed so the console can repair it after editing
 * the staged image. Same polynomial the loader checks with, which is the whole
 * point -- a second implementation would only be a second thing to get wrong.
 */
uint32_t mt6592_wifi_hif_crc32(const void* data, uint32_t size)
{
    return crc32((const uint8_t*)data, size);
}

static uint32_t crc32(const uint8_t* data, uint32_t size)
{
    uint32_t crc = 0xffffffffu;

    for (uint32_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8u; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return ~crc;
}

static int read_next_packet(uint8_t* port_out, uint32_t* length_out)
{
    const uint32_t lengths = mmio_read(MCR_WRPLR);
    const uint32_t rx0     = lengths & 0xffffu;
    const uint32_t rx1     = lengths >> 16;

    g_state.last_wrplr = lengths;
    g_state.wrplr_seen |= lengths;
    if (rx0 != 0u)
    {
        *port_out   = 0u;
        *length_out = rx0;
        return 1;
    }
    if (rx1 != 0u)
    {
        *port_out   = 1u;
        *length_out = rx1;
        return 1;
    }
    return 0;
}

static int receive_packet(uint8_t port, uint32_t length)
{
    /* See HIF_RX_HW_APPENDED_LEN: the port holds one DW of hardware status behind
     * the packet, and the transfer has to take it or the port stays misframed. */
    const uint32_t drain = align4(length + HIF_RX_HW_APPENDED_LEN);

    if (port == 0u)
    {
        return read_port(MCR_WRDR0, HIF_TARGET_RXD0, rx_buffer(), drain, RX_BUFFER_SIZE);
    }
    return read_port(MCR_WRDR1, HIF_TARGET_RXD1, rx_buffer(), drain, RX_BUFFER_SIZE);
}

static int wait_init_ack(uint8_t expected_sequence)
{
    const uint64_t deadline = mt6592_timer_microseconds() + INIT_ACK_TIMEOUT_US;
    uint32_t polls = 0u;

    while (mt6592_timer_microseconds() < deadline)
    {
        uint8_t port;
        uint32_t length;
        if (!read_next_packet(&port, &length))
        {
            delay_us(50u);
            if ((++polls & 0x3fu) == 0u) wifi_cooperative_yield();
            continue;
        }
        if (receive_packet(port, length) != 0 || length < 8u)
        {
            ++g_state.dropped_packets;
            continue;
        }

        const uint8_t* packet = rx_buffer();
        if (packet[2] != INIT_EVENT_CMD_RESULT || packet[3] != expected_sequence)
        {
            ++g_state.dropped_packets;
            continue;
        }
        if (packet[4] != 0u)
        {
            set_failure("MediaTek WLAN firmware rejected a download chunk", "mt6592-wifi:firmware-download-rejected");
            return -1;
        }
        tx_resource_release();
        return 0;
    }

    set_failure("MediaTek WLAN firmware download ACK timed out", "mt6592-wifi:firmware-download-ack-timeout");
    return -1;
}

/*
 * wait_init_ack() for an event that carries a body.
 *
 * Deliberately a separate function rather than a generalisation of the ACK
 * waiter: that one is on the download path, it is the reason 0x40350 bytes now
 * land correctly, and nothing diagnostic is worth destabilising it. The polling
 * and drop accounting below are copied from it verbatim.
 *
 * The whole event is handed back, header included, because the layout of the
 * body is the one thing here that is not measured.
 */
static int wait_init_event(uint8_t expected_event, uint8_t expected_sequence, uint8_t* event_out,
                           uint32_t event_max, uint32_t* event_len)
{
    const uint64_t deadline = mt6592_timer_microseconds() + INIT_ACK_TIMEOUT_US;
    uint32_t polls = 0u;
    /*
     * Keep the first packet that arrives but does not match, because a dropped
     * packet is not the same fact as no packet and the two used to be reported
     * identically. `wifi wr` is the case that exposed it: the write to CONSYS
     * 0x001f0000 reported "no ACCESS_REG answer", yet the very next `wifi rd`
     * of that address returned 0xa5a5a5a5 and AP 0x18080000 agreed. The write
     * had landed; only the acknowledgement was a shape this matcher rejects.
     * Capturing it turns a timeout into the bytes that explain the timeout.
     */
    uint8_t unmatched[16];
    uint32_t unmatched_len = 0u;

    if (event_len != 0) { *event_len = 0u; }

    while (mt6592_timer_microseconds() < deadline)
    {
        uint8_t port;
        uint32_t length;
        if (!read_next_packet(&port, &length))
        {
            delay_us(50u);
            if ((++polls & 0x3fu) == 0u) wifi_cooperative_yield();
            continue;
        }
        if (receive_packet(port, length) != 0 || length < 8u)
        {
            ++g_state.dropped_packets;
            continue;
        }

        const uint8_t* packet = rx_buffer();
        if (packet[2] != expected_event || packet[3] != expected_sequence)
        {
            if (unmatched_len == 0u)
            {
                unmatched_len = (length < sizeof unmatched) ? length : (uint32_t)sizeof unmatched;
                for (uint32_t i = 0; i < unmatched_len; ++i) { unmatched[i] = packet[i]; }
            }
            ++g_state.dropped_packets;
            continue;
        }

        if (event_out != 0 && event_max != 0u)
        {
            const uint32_t copy = (length < event_max) ? length : event_max;
            for (uint32_t i = 0; i < copy; ++i) { event_out[i] = packet[i]; }
            if (event_len != 0) { *event_len = copy; }
        }
        tx_resource_release();
        return 0;
    }

    /* Answered, but not with the event asked for. Hand the bytes back so the
     * caller can print them; 1 rather than 0 so nothing mistakes them for the
     * requested event's body. */
    if (unmatched_len != 0u && event_out != 0 && event_max != 0u)
    {
        const uint32_t copy = (unmatched_len < event_max) ? unmatched_len : event_max;
        for (uint32_t i = 0; i < copy; ++i) { event_out[i] = unmatched[i]; }
        if (event_len != 0) { *event_len = copy; }
        return 1;
    }
    return -1;
}

static int send_init_download(uint32_t address, const uint8_t* data, uint32_t size)
{
    uint8_t* packet              = tx_buffer();
    const uint32_t packet_size   = INIT_DOWNLOAD_HEADER_SIZE + size;
    const uint32_t transfer_size = align4(packet_size);
    const uint8_t sequence       = next_command_sequence();

    if (transfer_size > TX_BUFFER_SIZE)
    {
        set_failure("MediaTek WLAN firmware chunk exceeds the HIF buffer", "mt6592-wifi:firmware-chunk-too-large");
        return -1;
    }

    zero_bytes(packet, transfer_size);
    write_le16(packet + 0u, (uint16_t)transfer_size);
    packet[4] = INIT_CMD_DOWNLOAD_BUF;
    packet[5] = sequence;
    write_le32(packet + 8u, address);
    write_le32(packet + 12u, size);
    write_le32(packet + 16u, crc32(data, size));
    write_le32(packet + 20u, INIT_DOWNLOAD_ENCRYPTION | INIT_DOWNLOAD_ACK);
    copy_bytes(packet + INIT_DOWNLOAD_HEADER_SIZE, data, size);

    /*
     * No acquire here, deliberately, and this is the one place in the file where
     * that is a decision rather than an oversight.
     *
     * Stock does gate it -- nicTxInitResetResource gives the download its own TC0
     * with eight pages -- but this path already puts 0x40350 bytes into the chip
     * without a stall, because wait_init_ack() below blocks on the ROM's per-chunk
     * ACK and one outstanding chunk cannot overrun eight pages. The credits are
     * still collected: that ACK wait releases into the same table. Gating a path
     * that demonstrably works would only buy the chance that the ROM does not
     * report freed pages the way the firmware does, and lose the download to it.
     */
    write_port(MCR_WTDR0, HIF_TARGET_TXD0, packet, transfer_size);
    return wait_init_ack(sequence);
}

/*
 * INIT_CMD_ID_WIFI_START -- the "now run it" command that follows the download.
 *
 * The 16-byte layout is INIT_HIF_TX_HEADER_T + INIT_CMD_WIFI_START
 * (nic_init_cmd_event.h:98-121): u2TxByteCount at 0, ucEtherTypeOffset at 2,
 * ucCSflags at 3, ucCID at 4, ucSeqNum at 5, then u4Override at 8 and u4Address
 * at 12. wlanConfigWifiFunc (wlan_lib.c:3512, 0xc03be6e4 in the J36 kernel)
 * fills it and hands it to nicTxInitCmd on TC0 with no ACK wait -- the answer to
 * WIFI_START is WCIR's ready bit, not an init event.
 *
 * BOTH ARGUMENTS COME FROM THE BUILD, NOT FROM THE IMAGE:
 *
 *     #if CFG_OVERRIDE_FW_START_ADDRESS
 *         wlanConfigWifiFunc(prAdapter, TRUE, prRegInfo->u4StartAddress);
 *     #else
 *         wlanConfigWifiFunc(prAdapter, FALSE, 0);
 *     #endif
 *
 * and u4Override is `(fgEnable == TRUE ? 1 : 0)`. The reference tree's config.h
 * sets CFG_OVERRIDE_FW_START_ADDRESS to 0 for MT6628 -- and Makefile:5 compiles
 * this driver as MT6628 -- so the vendor source builds the FALSE/0 arm. The J36
 * build does not; its wlanAdapterStart takes the other arm:
 *
 *     c03c0f8c  movw r0,#0x5110 / movt r0,#0xc0a0   "<wifi> send Wi-Fi Start"
 *     c03c0f94  bl   0xc0819274                    printk
 *     c03c0f9c  mov  r1, #1                        fgEnable = TRUE
 *     c03c0fa4  ldr  r2, [r3, #12]                 prRegInfo->u4StartAddress
 *     c03c0fa8  bl   0xc03be6e4                    wlanConfigWifiFunc
 *
 * so override is 1 and the address is REG_INFO_T +12, which wlanProbe set to the
 * constant 0x00060000 and never touched again. Which of the two we put on the
 * wire is g_start_override_word / g_start_address_override; see the note on
 * those for why this one disagreement gets a knob instead of a verdict.
 */
static int send_init_start(uint32_t start_address)
{
    uint8_t* packet        = tx_buffer();
    const uint8_t sequence = next_command_sequence();

    zero_bytes(packet, INIT_START_PACKET_SIZE);
    write_le16(packet + 0u, INIT_START_PACKET_SIZE);
    packet[4] = INIT_CMD_WIFI_START;
    packet[5] = sequence;
    write_le32(packet + 8u, g_start_override_word); /* u4Override */
    write_le32(packet + 12u, start_address);

    /*
     * Baseline the device-to-host mailboxes while the boot ROM still owns the
     * chip. Whatever they read here is the ROM's leftover, so any later change is
     * necessarily the downloaded firmware's own writing -- which is the only
     * evidence available that it executed at all.
     */
    g_state.mailbox_at_start_0 = mmio_read(MCR_D2HRM0R);
    g_state.mailbox_at_start_1 = mmio_read(MCR_D2HRM1R);

    /*
     * The control sample, taken back to back with nothing between the reads. At
     * this instant the boot ROM is provably executing -- it has just ACKed every
     * download chunk and answers ACCESS_REG -- so these four values are what a
     * *running* core looks like in this register. If they are all equal, the
     * post-jump trace proves nothing and the register is not what stock says it
     * is; if they differ, a frozen trace afterwards is a stopped core.
     */
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        g_state.cpupcr_before[i] = mt6592_wifi_hif_read_cpupcr();
    }

    write_port(MCR_WTDR0, HIF_TARGET_TXD0, packet, INIT_START_PACKET_SIZE);
    pc_trace_capture(); /* armed by `wifi trace`; the tightest sample we can take */
    tx_resource_release();
    return 0;
}

/*
 * Watch for anything the peer volunteers between WIFI_START and WLAN_READY.
 *
 * Stock does not look: its readiness poll (0xc03c0fec) reads WCIR and nothing
 * else. Stock also gets a firmware that comes up. If ours refuses the start it
 * can only say so as an init event on RX0, and this is the only window in which
 * that event exists -- so keep the first one for the `wifi` dump, and drain the
 * rest so a backed-up FIFO cannot pass for silence.
 */
static void capture_start_event(void)
{
    uint8_t port;
    uint32_t length;

    for (uint32_t guard = 0u; guard < 8u; ++guard)
    {
        if (!read_next_packet(&port, &length)) return;
        if (receive_packet(port, length) != 0)
        {
            ++g_state.dropped_packets;
            return;
        }
        if (g_state.start_event_valid)
        {
            ++g_state.rx_events;
            continue;
        }

        uint32_t copy = length;
        if (copy > sizeof(g_state.start_event)) copy = sizeof(g_state.start_event);
        zero_bytes(g_state.start_event, sizeof(g_state.start_event));
        copy_bytes(g_state.start_event, rx_buffer(), copy);
        g_state.start_event_length = length;
        g_state.start_event_valid  = 1;
    }
}

static int wait_firmware_ready(void)
{
    const uint64_t deadline = mt6592_timer_microseconds() + FIRMWARE_READY_TIMEOUT_US;
    uint32_t polls = 0u;

    /*
     * Sample WCIR and the device-to-host mailboxes on every distinct value, not
     * just at the end. Only the final reading was ever kept, which cannot tell a
     * chip that sat inert for 5.13 s from one that came up, changed its mind and
     * went back down -- a POR_INDICATOR transition inside the window would say
     * the WLAN subsystem re-booted underneath us, and we would never have seen it.
     */
    g_state.wcir_transitions = 0u;
    g_state.last_wcir        = ~mmio_read(MCR_WCIR); /* force the first sample to count */

    /*
     * Sample the connectivity MCU's PC on every iteration alongside WCIR. This
     * is the same loop stock's stp_dbg_poll_cpupcr runs, folded into the poll we
     * are already spending 5.13 s on rather than run separately: 513 reads of
     * 0x18070160 at 10 ms, which is the entire window in which the firmware
     * either boots or dies.
     */
    g_state.cpupcr_trace_count = 0u;
    g_state.cpupcr_hist_used   = 0u;
    g_state.cpupcr_hist_missed = 0u;
    g_state.cpupcr_page_used   = 0u;
    g_state.cpupcr_page_missed = 0u;
    g_state.cpupcr_samples     = 0u;
    g_state.cpupcr_changes     = 0u;
    g_state.cpupcr_first       = mt6592_wifi_hif_read_cpupcr();
    g_state.cpupcr_last        = g_state.cpupcr_first;
    g_state.cpupcr_min         = g_state.cpupcr_first;
    g_state.cpupcr_max         = g_state.cpupcr_first;

    while (mt6592_timer_microseconds() < deadline)
    {
        const uint32_t wcir = mmio_read(MCR_WCIR);
        const uint32_t pc   = mt6592_wifi_hif_read_cpupcr();

        ++g_state.cpupcr_samples;
        if (pc < g_state.cpupcr_min) g_state.cpupcr_min = pc;
        if (pc > g_state.cpupcr_max) g_state.cpupcr_max = pc;
        {
            const uint32_t slots = (uint32_t)(sizeof g_state.cpupcr_hist / sizeof g_state.cpupcr_hist[0]);
            uint32_t h = 0u;
            while (h < g_state.cpupcr_hist_used && g_state.cpupcr_hist[h] != pc) ++h;
            if (h < g_state.cpupcr_hist_used)
            {
                ++g_state.cpupcr_hist_hits[h];
            }
            else if (g_state.cpupcr_hist_used < slots)
            {
                g_state.cpupcr_hist[h]      = pc;
                g_state.cpupcr_hist_hits[h] = 1u;
                g_state.cpupcr_hist_used    = h + 1u;
            }
            else
            {
                ++g_state.cpupcr_hist_missed;
            }
        }
        {
            /* The same sample bucketed to 256 bytes; see cpupcr_page in the
             * header for why the exact table above cannot cover the run. */
            const uint32_t page  = pc >> 8;
            const uint32_t slots = (uint32_t)(sizeof g_state.cpupcr_page / sizeof g_state.cpupcr_page[0]);
            uint32_t p = 0u;
            while (p < g_state.cpupcr_page_used && g_state.cpupcr_page[p] != page) ++p;
            if (p < g_state.cpupcr_page_used)
            {
                ++g_state.cpupcr_page_hits[p];
            }
            else if (g_state.cpupcr_page_used < slots)
            {
                g_state.cpupcr_page[p]      = page;
                g_state.cpupcr_page_hits[p] = 1u;
                g_state.cpupcr_page_used    = p + 1u;
            }
            else
            {
                ++g_state.cpupcr_page_missed;
            }
        }
        if (pc != g_state.cpupcr_last)
        {
            ++g_state.cpupcr_changes;
            if (g_state.cpupcr_trace_count
                < (uint32_t)(sizeof g_state.cpupcr_trace / sizeof g_state.cpupcr_trace[0]))
            {
                g_state.cpupcr_trace[g_state.cpupcr_trace_count]      = pc;
                g_state.cpupcr_trace_poll[g_state.cpupcr_trace_count] = polls;
                ++g_state.cpupcr_trace_count;
            }
        }
        g_state.cpupcr_last = pc;

        if (wcir != g_state.last_wcir)
        {
            if (g_state.wcir_transitions < (uint32_t)(sizeof g_state.wcir_seen / sizeof g_state.wcir_seen[0]))
            {
                g_state.wcir_seen[g_state.wcir_transitions] = wcir;
                g_state.wcir_seen_poll[g_state.wcir_transitions] = polls;
            }
            ++g_state.wcir_transitions;
        }
        g_state.last_wcir = wcir;
        if (wcir & WCIR_WLAN_READY)
        {
            g_state.firmware_alive = 1;
            /*
             * WLAN_READY is the moment the download table stops being true and
             * stock's run-time one starts: TC0=1, TC1=20, TC2=1, TC3=1, TC4=4,
             * TC5=1. It goes here rather than in start_firmware() because this is
             * the single place in the file where firmware_alive is raised, so no
             * path can reach the command senders with the eight-page download
             * table still installed -- which would hand TC4 four pages it does
             * not have and put us straight back on the bus stall.
             */
            tx_resource_reset(kTxPagesRuntime);
            return 0;
        }
        capture_start_event();
        mt6592_pmic_charger_service();
        delay_us(10000u);
        if ((++polls & 0x07u) == 0u) wifi_cooperative_yield();
    }

    /*
     * Resample the status registers before giving up. They are otherwise only
     * written where they happen to be read -- last_wrplr in particular is set by
     * read_next_packet() *before* the packet it found is consumed, so after a
     * clean download it still holds the length of the final ACK. Left stale it
     * reads as "8 bytes pending, never collected", which is a packet that does
     * not exist and a lead worth an afternoon.
     *
     * The mailboxes are here because this is exactly what stock does on this
     * exact failure: the expired loop calls nicGetMailbox(adapter, 0, &v) at
     * 0xc03c11f8 and prints "Waiting for Ready bit: Timeout, ID=%u". Comparing
     * against the pre-poll sample also answers a question the single reading
     * cannot -- whether the firmware wrote a mailbox at any point before dying.
     */
    g_state.last_whisr     = mmio_read(MCR_WHISR);
    g_state.last_wasr      = mmio_read(MCR_WASR);
    g_state.last_wrplr     = mmio_read(MCR_WRPLR);
    g_state.last_mailbox_0 = mmio_read(MCR_D2HRM0R);
    g_state.last_mailbox_1 = mmio_read(MCR_D2HRM1R);
    set_failure("MediaTek WLAN firmware did not assert WLAN_READY", "mt6592-wifi:firmware-ready-timeout");
    return -1;
}

static int send_normal_command(uint8_t command_id, const uint8_t* payload, uint32_t payload_size)
{
    uint8_t* packet              = tx_buffer();
    const uint32_t packet_size   = NORMAL_COMMAND_HEADER_SIZE + payload_size;
    const uint32_t transfer_size = align4(packet_size);

    if (transfer_size > TX_BUFFER_SIZE || transfer_size > 0x0fffu)
    {
        return -1;
    }

    zero_bytes(packet, transfer_size);
    write_le16(packet + 0u, (uint16_t)transfer_size);
    packet[3] = (uint8_t)((HIF_TX_COMMAND_RESOURCE << HIF_TX_RESOURCE_SHIFT) |
                          (HIF_TX_PACKET_TYPE_CMD << HIF_TX_PACKET_TYPE_SHIFT));
    packet[4] = command_id;
    packet[5] = 1u; /* set */
    packet[6] = next_command_sequence();
    if (payload && payload_size)
    {
        copy_bytes(packet + NORMAL_COMMAND_HEADER_SIZE, payload, payload_size);
    }

    /* Every command in this driver is a TC4 packet, and TC4 owns four pages.
     * Seven of these go out back to back on a join, so this is the acquire that
     * the crash was the absence of. */
    if (tx_resource_acquire(HIF_TX_COMMAND_RESOURCE) != 0) return -1;
    write_port(MCR_WTDR1, HIF_TARGET_TXD1, packet, transfer_size);
    return 0;
}

static int send_hif_frame(const uint8_t* frame,
                          uint32_t frame_len,
                          uint8_t packet_type,
                          uint8_t sta_index,
                          int is_80211,
                          int is_1x,
                          int basic_rate)
{
    uint8_t* packet = tx_buffer();
    const uint32_t packet_size = HIF_DATA_HEADER_SIZE + frame_len;
    const uint32_t transfer_size = align4(packet_size);
    /*
     * ucStaRecIndex first, because the traffic class is derived from it and not
     * the other way round. qmDetermineStaRecIndex (nic/que_mgt.c:1501-1541), in
     * its three cases and in this order:
     *
     *     if (IS_BMCAST_MAC_ADDR(aucEthDestAddr))  -> STA_REC_INDEX_BMCAST
     *     else if (an AP record with fgIsValid)    -> that record's index
     *     else                                     -> STA_REC_INDEX_NOT_FOUND
     *
     * The group-addressed test is on the *Ethernet* DA, which is why it only
     * applies to the !is_80211 callers. The fgIsValid half is g_sta_rec_index.
     */
    const uint8_t record = (!is_80211 && frame_len >= 6u && multicast_mac(frame))
                               ? STA_REC_INDEX_BMCAST
                               : sta_index;
    /*
     * The traffic class, which stock derives in qmEnqueueTxPackets
     * (nic/que_mgt.c:1279-1392) and NOT from anything in the frame's own header:
     *
     *     ucTC = TC1_INDEX;                            // the initial value, 1279
     *     switch (ucStaRecIndex) {
     *       case STA_REC_INDEX_BMCAST:  ucTC = TC5_INDEX;   // 1303-1305
     *       case STA_REC_INDEX_NOT_FOUND: ucTC = TC5_INDEX; // 1321
     *       default: ... if (!prStaRec->fgIsQoS) ucTC = TC1_INDEX;  // 1389
     *     }
     *
     * so a group-addressed data frame is TC5, not TC4, and so is a frame sent
     * before the firmware has validated the station record. Management frames are
     * the one class forced from elsewhere -- nicTxEnqueueMsdu (nic_tx.c:2362-2366),
     * "MMPDU: force stick to TC4" -- and 802.1X data never reaches this path at
     * all in stock: wlanHardStartXmit turns it into a COMMAND_TYPE_SECURITY_FRAME
     * (wlan_lib.c:4111) that goes out through the command queue on TC4
     * (wlan_lib.c:2234), which is why it takes TC4's port here too.
     *
     * This used to hand group-addressed data to TC4, which is both the wrong page
     * pool (TC4 has 4 pages and is the command class; TC5 has 1 and is the
     * broadcast class) and the wrong port, since the port follows the class.
     */
    const uint8_t resource =
        (packet_type == 3u || is_1x)
            ? 4u
            : ((record == STA_REC_INDEX_BMCAST || record == STA_REC_INDEX_NOT_FOUND) ? 5u : 1u);
    if (!frame || frame_len == 0u || transfer_size > TX_BUFFER_SIZE || packet_size > 0x0fffu) return -1;

    zero_bytes(packet, transfer_size);
    write_le16(packet + 0u, (uint16_t)packet_size);
    packet[2] = (uint8_t)(((HIF_DATA_HEADER_SIZE + (is_80211 ? 24u : 12u)) >> 1u) & 0xffu);
    packet[3] = (uint8_t)((resource << HIF_TX_RESOURCE_SHIFT) |
                          ((packet_type & 0x3u) << HIF_TX_PACKET_TYPE_SHIFT));
    packet[4] = (uint8_t)(is_80211 ? 24u : 14u);
    packet[5] = (uint8_t)((NETWORK_TYPE_AIS << 4u) | (is_1x ? BIT(6) : 0u) | (is_80211 ? BIT(7) : 0u));
    packet[10] = record;
    packet[11] = BIT(5); /* burst end */
    /*
     * ucPacketSeqNo and HIF_TX_HDR_NEED_ACK move together. Stock writes both or
     * neither, in the same if/else, three times over (nic_tx.c:1469-1474,
     * 1701-1702, 1755-1760):
     *
     *     if (prMsduInfo->pfTxDoneHandler) {
     *         rHwTxHeader.ucPacketSeqNo = prMsduInfo->ucTxSeqNum;
     *         rHwTxHeader.ucAck_BIP_BasicRate = HIF_TX_HDR_NEED_ACK;
     *     } else {
     *         rHwTxHeader.ucPacketSeqNo = 0;
     *         rHwTxHeader.ucAck_BIP_BasicRate = 0;
     *     }
     *
     * The sequence number only means anything as the tag on the TX-status event
     * that NEED_ACK asks for, so a nonzero one without the bit -- which is what
     * this used to send on every data frame -- is a number the firmware has no
     * reason to report and nothing here would match.
     */
    /*
     * 1X frames ask for the TX-status event too, and g_last_tx_sequence carries
     * the tag out to the caller.
     *
     * The four-way handshake needs to know when message 4 has actually been
     * transmitted, not merely when it was written to WTDR1: the pairwise key
     * must not be installed while message 4 is still sitting in the firmware's
     * TX queue, or the firmware CCMP-encrypts a frame the AP can only read in
     * the clear. See the EVENT_ID_TX_DONE handler and complete_four_way().
     */
    const int need_ack = (packet_type == 3u) || is_1x;
    if (need_ack)
    {
        ++g_tx_sequence;
        if (g_tx_sequence == 0u) ++g_tx_sequence;
        packet[12] = g_tx_sequence;
        packet[13] = (uint8_t)(BIT(0) | (basic_rate ? BIT(2) : 0u));
        g_last_tx_sequence = g_tx_sequence;
    }
    else
    {
        packet[13] = (uint8_t)(basic_rate ? BIT(2) : 0u);
        g_last_tx_sequence = 0u;
    }
    copy_bytes(packet + HIF_DATA_HEADER_SIZE, frame, frame_len);

    /* The resource nibble in packet[3] IS the traffic class, and the port follows
     * it: nicTxCmd and nicTxMsduInfoList both pick the port with `ucTC ==
     * TC4_INDEX ? 1 : 0` (nic_tx.c:1664, 2252), i.e. WTDR1 for TC4 and WTDR0 for
     * every other class. Acquire against the class the frame declares, not against
     * whichever one is convenient. */
    if (tx_resource_acquire(resource) != 0) return -1;
    if (resource == 4u)
        write_port(MCR_WTDR1, HIF_TARGET_TXD1, packet, transfer_size);
    else
        write_port(MCR_WTDR0, HIF_TARGET_TXD0, packet, transfer_size);
    return 0;
}

static uint16_t rate_bit(uint8_t rate)
{
    switch (rate & 0x7fu)
    {
    case 2u: return BIT(0);
    case 4u: return BIT(1);
    case 11u: return BIT(2);
    case 22u: return BIT(3);
    case 44u: return BIT(4);
    case 66u: return BIT(5);
    case 12u: return BIT(6);
    case 18u: return BIT(7);
    case 24u: return BIT(8);
    case 36u: return BIT(9);
    case 48u: return BIT(10);
    case 72u: return BIT(11);
    case 96u: return BIT(12);
    case 108u: return BIT(13);
    default: return 0u;
    }
}

/*
 * assoc.c:793-805, one number written to two places:
 *
 *     if (prStaRec->ucDTIMPeriod)
 *         u2ListenInterval = prStaRec->ucDTIMPeriod * DEFAULT_LISTEN_INTERVAL_BY_DTIM_PERIOD;
 *     else
 *         u2ListenInterval = DEFAULT_LISTEN_INTERVAL;
 *     prStaRec->u2ListenInterval = u2ListenInterval;
 *     WLAN_SET_FIELD_16(&prAssocFrame->u2ListenInterval, u2ListenInterval);
 *
 * with DEFAULT_LISTEN_INTERVAL_BY_DTIM_PERIOD = 2 and DEFAULT_LISTEN_INTERVAL = 10
 * (include/nic/mac.h:225-226). The last two lines are the point: the interval in
 * the station record IS the interval the association request advertised. This
 * used to send dtim*2 in the frame and a flat 10 in the record, which tells the
 * AP one wake-up schedule and the firmware another.
 */
static uint16_t listen_interval(void)
{
    return g_assoc_profile.dtim_period ? (uint16_t)g_assoc_profile.dtim_period * 2u : 10u;
}

/*
 * CMD_UPDATE_STA_RECORD_T, 40 bytes (include/nic_cmd_event.h:1347-1375), filled
 * by cnmStaSendUpdateCmd (mgmt/cnm_mem.c:1176-1266). Every offset below is that
 * struct's, and every constant is now traced:
 *
 *   0  ucIndex             our one record is 0
 *   1  ucStaType           STA_TYPE_LEGACY_AP = STA_TYPE_LEGACY_MASK|STA_TYPE_AP_MASK
 *                          = BIT(STA_TYPE_LEGACY_INDEX=0) | BIT(STA_ROLE_AP_INDEX=6)
 *                          = 0x41  (wlan_def.h:531, 538-544, 747-752, 771)
 *   2  aucMacAddr[6]       the AP
 *   8  u2AssocId           0 until the association response gives one
 *   10 u2ListenInterval    see listen_interval()
 *   12 ucNetTypeIndex      NETWORK_TYPE_AIS_INDEX = 0
 *   13 ucDesiredPhyTypeSet PHY_TYPE_SET_802_11BGN = BIT(HR_DSSS=0)|BIT(ERP=1)|BIT(HT=4)
 *                          = 0x13  (wlan_def.h:555-563, 306-308)
 *   14 u2DesiredNonHTRateSet
 *   16 u2BSSBasicRateSet
 *   18 ucIsQoS             0 -- see below
 *   19 ucIsUapsdSupported  0, follows ucIsQoS
 *   20 ucStaState          STA_STATE_1=0, STA_STATE_2=1, STA_STATE_3=2
 *                          (include/nic/mac.h:259-261), so the 0/1/2 this is
 *                          called with are the three 802.11 association states.
 *   21 ucMcsSet            0xff = MCS 0..7
 *   22 ucSupMcs32          0
 *   23 ucAmpduParam        0 = exponent 0, density 0
 *   24 u2HtCapInfo         0 = 20 MHz, long GI, no STBC: the conservative HT
 *   26 u2HtExtendedCap     0
 *   28 u4TxBeamformingCap  0
 *   32 ucAselCap           0
 *   33 ucRCPI              the beacon's, as the scan measured it
 *   34 ucNeedResp          see below
 *   35 ucUapsdAc           0 = ucBmpTriggerAC | (ucBmpDeliveryAC << 4), both 0
 *   36 ucUapsdSp           0
 *   37 aucReserved[3]      cnmMemAlloc'd and never zeroed in stock: garbage
 *
 * ucIsQoS is 0 on purpose and it is 0 consistently: this driver sends no WMM IE,
 * so it declares a non-QoS peer here, a non-QBSS in SET_BSS_INFO, and takes the
 * `if (!prStaRec->fgIsQoS) ucTC = TC1_INDEX` arm of qmEnqueueTxPackets
 * (que_mgt.c:1389) for its data. Stock reaches the same three settings whenever
 * the AP has no WMM IE, and forces fgIsQoS = FALSE outright on the adhoc path
 * (ais_fsm.c:3208). What would be wrong is claiming QoS in one of the three.
 *
 * ucNeedResp is the gate on the whole data path, and it was 0:
 *
 *     cnmStaRecChangeState (mgmt/cnm_mem.c:1043-1090)
 *         fgNeedResp = FALSE;
 *         if (ucNewState == STA_STATE_3) {
 *             secFsmEventStart(...);
 *             if (ucNewState != prStaRec->ucStaState) fgNeedResp = TRUE;
 *         }
 *         ...
 *         cnmStaSendUpdateCmd(prAdapter, prStaRec, fgNeedResp);
 *
 * and cnmStaSendUpdateCmd hands cnmStaRecHandleEventPkt in as the done handler
 * only when fgNeedResp, which on EVENT_ID_ACTIVATE_STA_REC_T calls
 * qmActivateStaRec -> prStaRec->fgIsValid = TRUE (que_mgt.c:800). fgIsValid is
 * what qmDetermineStaRecIndex (que_mgt.c:1511-1540) requires before it will put
 * this record's index on a data frame. So: no ucNeedResp, no event, no valid
 * record, and every unicast data frame is a STA_REC_INDEX_NOT_FOUND frame. See
 * g_sta_rec_index.
 */
static int send_sta_record(uint8_t sta_state)
{
    uint8_t command[40];
    zero_bytes(command, sizeof(command));
    command[0] = STA_RECORD_INDEX;
    command[1] = 0x41u; /* STA_TYPE_LEGACY_AP */
    copy_bytes(command + 2u, g_assoc_profile.bssid, 6u);
    write_le16(command + 8u, g_assoc_aid);
    write_le16(command + 10u, listen_interval());
    command[12] = NETWORK_TYPE_AIS;
    command[13] = 0x13u; /* PHY_TYPE_SET_802_11BGN */
    write_le16(command + 14u, g_assoc_profile.operational_rates ? g_assoc_profile.operational_rates : 0x3fcfu);
    write_le16(command + 16u, g_assoc_profile.basic_rates ? g_assoc_profile.basic_rates : 0x044fu);
    command[20] = sta_state;
    command[21] = 0xffu; /* MCS 0..7 */
    command[33] = g_assoc_profile.rcpi;
    command[34] = (uint8_t)(sta_state == STA_STATE_3 ? 1u : 0u);
    return send_normal_command(CMD_ID_UPDATE_STA_RECORD, command, sizeof(command));
}

/*
 * CMD_SET_BSS_INFO, 80 bytes, and the 80 is not a round number chosen here: it is
 * a 64-byte body with a 16-byte CMD_SET_BSS_RLM_PARAM_T welded onto the end
 * (include/nic_cmd_event.h:1305-1345). nicUpdateBss (nic/nic.c:2172-2280) fills
 * the first part and hands the tail to rlmFillSyncCmdParam (mgmt/rlm.c:1512-1548).
 * Offsets and constants, all now traced:
 *
 *   0  ucNetTypeIndex        NETWORK_TYPE_AIS_INDEX = 0
 *   1  ucConnectionState     PARAM_MEDIA_STATE_CONNECTED = 1, DISCONNECTED = 0
 *   2  ucCurrentOPMode       OP_MODE_INFRASTRUCTURE = 0 (wlan_def.h:573-580)
 *   3  ucSSIDLen
 *   4  aucSSID[32]
 *   36 aucBSSID[6]
 *   42 ucIsQBSS              0 -- the non-QoS choice, see send_sta_record
 *   43 ucReserved1
 *   44 u2OperationalRateSet
 *   46 u2BSSBasicRateSet
 *   48 ucStaRecIdxOfAP       our record, or STA_REC_INDEX_NOT_FOUND -- see below
 *   49 ucReserved2
 *   50 ucReserved3
 *   51 ucNonHTBasicPhyType   PHY_TYPE_ERP_INDEX = 1. bss.c:644-646 picks ERP for
 *                            any BSS whose non-HT PHY set has PHY_TYPE_BIT_ERP,
 *                            which every 2.4 GHz g/n AP does; the OFDM and
 *                            HR_DSSS arms are the 5 GHz and b-only cases.
 *   52 ucAuthMode            AUTH_MODE_OPEN = 0, AUTH_MODE_WPA2_PSK = 7
 *                            (include/wlan_oid.h:296-306)
 *   53 ucEncStatus           ENUM_ENCRYPTION_DISABLED = 1,
 *                            ENUM_ENCRYPTION3_ENABLED = 6,
 *                            ENUM_ENCRYPTION3_KEY_ABSENT = 7
 *                            (include/wlan_oid.h:309-323 -- note the aliasing:
 *                            WEP_ENABLED and ENCRYPTION1_ENABLED are both 0, so
 *                            "disabled" is 1, not 0.) KEY_ABSENT until the
 *                            handshake installs the pairwise key, ENABLED after,
 *                            which is what key_ready selects.
 *   54 ucPhyTypeSet          PHY_TYPE_SET_802_11BGN = 0x13, as in the STA record
 *   55 aucOwnMac[6]
 *   61 fgWapiMode            0: stock writes (UINT_8)FALSE unconditionally first
 *                            and only WAPI builds overwrite it (nic.c:2207)
 *   62 fgIsApMode            0: P2P GO only (nic.c:2237)
 *   63 aucRsv[1]
 *   -- CMD_SET_BSS_RLM_PARAM_T from here, rlmFillSyncCmdParam --
 *   64 ucNetTypeIndex        the same index again, this time the RLM copy
 *   65 ucRfBand              BAND_2G4 = 1 (BAND_NULL is 0; wlan_def.h:590-595)
 *   66 ucPrimaryChannel
 *   67 ucRfSco               CHNL_EXT_SCN = 0 (wlan_def.h:582-585): 20 MHz, no
 *                            secondary channel, which is what ucHtOpInfo1 = 0
 *                            below says too
 *   68 ucErpProtectMode      0
 *   69 ucHtProtectMode       0
 *   70 ucGfOperationMode     0
 *   71 ucTxRifsMode          0
 *   72 u2HtOpInfo3           0
 *   74 u2HtOpInfo2           0
 *   76 ucHtOpInfo1           0
 *   77 ucUseShortPreamble    the AP's CAP_INFO_SHORT_PREAMBLE = BIT(5)
 *   78 ucUseShortSlotTime    the AP's CAP_INFO_SHORT_SLOT_TIME = BIT(10)
 *                            (include/nic/mac.h:554, 559). Both are read straight
 *                            off the beacon's capability field in stock too:
 *                            fgIsShortPreambleAllowed = (u2CapInfo &
 *                            CAP_INFO_SHORT_PREAMBLE) at ais_fsm.c:3521-3527 and
 *                            fgUseShortPreamble = fgIsShortPreambleAllowed at
 *                            rlm.c:948; fgUseShortSlotTime likewise at rlm.c:1349.
 *   79 ucCheckId             0x72, and the struct member's own comment says
 *                            "Fixed value: 0x72" -- rlmFillSyncCmdParam:1536
 *                            assigns the literal. It is the firmware's guard that
 *                            the 16-byte tail arrived intact, so the last byte of
 *                            an 80-byte body is the one that must not be padding.
 *
 * The four protection-mode bytes at 68-71 and the three HT operation words at
 * 72-76 are left zero deliberately: stock computes them from the BSS's ERP and HT
 * Operation IEs, and this driver associates as a 20 MHz non-QoS station, so the
 * settings that pair with that are all-zero -- no protection, no RIFS, no
 * greenfield, no secondary channel. They cost throughput near legacy traffic;
 * they cannot cost the association.
 */
static int send_bss_info(int key_ready)
{
    uint8_t command[80];
    const uint32_t ssid_len = string_length(g_assoc_profile.ssid, 32u);
    zero_bytes(command, sizeof(command));
    command[0] = NETWORK_TYPE_AIS;
    command[1] = 1u; /* PARAM_MEDIA_STATE_CONNECTED */
    command[2] = 0u; /* OP_MODE_INFRASTRUCTURE */
    command[3] = (uint8_t)ssid_len;
    copy_bytes(command + 4u, (const uint8_t*)g_assoc_profile.ssid, ssid_len);
    copy_bytes(command + 36u, g_assoc_profile.bssid, 6u);
    write_le16(command + 44u, g_assoc_profile.operational_rates ? g_assoc_profile.operational_rates : 0x3fcfu);
    write_le16(command + 46u, g_assoc_profile.basic_rates ? g_assoc_profile.basic_rates : 0x044fu);
    command[48] = STA_RECORD_INDEX;
    command[51] = 1u; /* PHY_TYPE_ERP_INDEX */
    command[52] = g_assoc_profile.security == WIFI_SECURITY_WPA2_PSK_CCMP ? 7u : 0u;
    command[53] = g_assoc_profile.security == WIFI_SECURITY_WPA2_PSK_CCMP ? (key_ready ? 6u : 7u) : 1u;
    command[54] = 0x13u; /* PHY_TYPE_SET_802_11BGN */
    copy_bytes(command + 55u, g_wifi_mac, 6u);
    command[64] = NETWORK_TYPE_AIS;
    command[65] = 1u; /* BAND_2G4 */
    command[66] = g_assoc_profile.channel ? g_assoc_profile.channel : 1u;
    command[77] = (g_assoc_profile.capability & BIT(5)) ? 1u : 0u;
    command[78] = (g_assoc_profile.capability & BIT(10)) ? 1u : 0u;
    command[79] = 0x72u; /* ucCheckId, "Fixed value: 0x72" */
    return send_normal_command(CMD_ID_SET_BSS_INFO, command, sizeof(command));
}

/*
 * The same SET_BSS_INFO the join sends, with the one field that matters
 * inverted: media state DISCONNECTED. Everything else is deliberately still the
 * live profile -- the firmware matches the command against the BSS it is
 * holding, and a disconnect for a BSS described differently from the one it was
 * given is a disconnect for nothing. So this runs BEFORE reset_association().
 */
static int send_bss_disconnect(void)
{
    uint8_t command[80];
    zero_bytes(command, sizeof(command));
    command[0] = NETWORK_TYPE_AIS;
    command[1] = 0u; /* PARAM_MEDIA_STATE_DISCONNECTED, against 1 to connect */
    command[2] = 0u; /* OP_MODE_INFRASTRUCTURE */
    copy_bytes(command + 36u, g_assoc_profile.bssid, 6u);
    /*
     * NOT 0. ucStaRecIdxOfAP names the record the firmware should associate with
     * this BSS, and nicUpdateBss's else-arm (nic/nic.c:2266-2268) is reached
     * exactly when there is no AP record to name:
     *
     *     else {
     *         rCmdSetBssInfo.ucStaRecIdxOfAP = STA_REC_INDEX_NOT_FOUND;
     *     }
     *
     * A disconnect that still claims record 0 is the AP is a disconnect that
     * leaves the firmware holding the association it was told to drop. The 0 here
     * was invisible because it was never written -- it was the zero_bytes.
     */
    command[48] = STA_REC_INDEX_NOT_FOUND;
    copy_bytes(command + 55u, g_wifi_mac, 6u);
    command[64] = NETWORK_TYPE_AIS;
    command[65] = 1u; /* BAND_2G4 */
    command[66] = g_assoc_profile.channel ? g_assoc_profile.channel : 1u;
    /* ucCheckId again: rlmFillSyncCmdParam writes the literal every time it runs,
     * and nicUpdateBss calls it for a disconnect exactly as for a connect. */
    command[79] = 0x72u;
    return send_normal_command(CMD_ID_SET_BSS_INFO, command, sizeof(command));
}

/*
 * CID 0x18, the one this driver spent a while sending by accident in place of
 * 0x17 -- see CMD_ID_UPDATE_STA_RECORD. Here it is what is actually wanted:
 * forget the peer. Without it the firmware keeps a station record pointing at
 * an AP the driver has stopped tracking, and keeps spending TC4 pages on it.
 */
static int send_sta_record_remove(void)
{
    uint8_t command[8];
    zero_bytes(command, sizeof(command));
    command[0] = NETWORK_TYPE_AIS;
    copy_bytes(command + 1u, g_assoc_profile.bssid, 6u);
    return send_normal_command(CMD_ID_REMOVE_STA_RECORD, command, sizeof(command));
}

/*
 * CMD_BSS_ACTIVATE_CTRL, 4 bytes {ucNetTypeIndex, ucActive, reserved[2]}.
 *
 * Stock calls nicDeactivateNetwork() (nic.c:2124, ucActive = 0) from
 * ais_fsm.c:1954 -- on the transition to idle, immediately before entering
 * AIS_STATE_SCAN -- and pairs it with nicActivateNetwork() when the next join
 * starts. SET_BSS_INFO(DISCONNECTED) alone does not do it: the BSSID stays in
 * the firmware's BSS context, so the receive filter keeps claiming beacons for
 * that BSSID against a BSS that is no longer being serviced, and they never
 * reach the scan result list. That is exactly what the log shows -- IZZI-1CA3
 * at -46 dBm is absent from all three scans taken after its deauth, while a
 * scan taken while associated to a *different* AP finds it at -47.
 *
 * Deactivate then immediately reactivate: the firmware drops and rebuilds the
 * per-BSS context, which is what clears the stale binding, and the AIS network
 * stays live so nothing else in this driver has to track an inactive BSS.
 */
static int send_bss_reactivate(void)
{
    uint8_t command[4];
    zero_bytes(command, sizeof(command));
    command[0] = NETWORK_TYPE_AIS;
    command[1] = 0u;
    if (send_normal_command(CMD_ID_BSS_ACTIVATE_CTRL, command, sizeof(command)) != 0) {
        return -1;
    }
    command[1] = 1u;
    return send_normal_command(CMD_ID_BSS_ACTIVATE_CTRL, command, sizeof(command));
}

/*
 * CMD_INDICATE_PM_BSS_CONNECTED, 12 bytes (include/nic_cmd_event.h:1389-1399),
 * filled by nicPmIndicateBssConnected (nic/nic.c:2359-2414):
 *
 *   0  ucNetTypeIndex        NETWORK_TYPE_AIS_INDEX = 0
 *   1  ucDtimPeriod          prBssInfo->ucDTIMPeriod
 *   2  u2AssocId
 *   4  u2BeaconInterval
 *   6  u2AtimWindow          0 for infrastructure (an IBSS field)
 *   8  fgIsUapsdConnection   prStaRecOfAP->fgIsUapsdSupported, so 0 here
 *   9  ucBmpDeliveryAC       0, UAPSD only
 *   10 ucBmpTriggerAC        0, UAPSD only
 *   11 aucReserved[1]        stock never zeroes the struct and never writes this
 *
 * WHEN, not just what. There is no DTIM period in an association response, and
 * stock does not invent one -- aisUpdateBssInfoForJOIN (mgmt/ais_fsm.c:3569-3583)
 * ends with:
 *
 *     // NOTE: Defer ucDTIMPeriod updating to when beacon is received after connection
 *     prAisBssInfo->ucDTIMPeriod = 0;
 *     ...
 *     nicUpdateBss(prAdapter, NETWORK_TYPE_AIS_INDEX);
 *     //4 <4.4> *DEFER OPERATION* nicPmIndicateBssConnected() will be invoked
 *     //inside scanProcessBeaconAndProbeResp() after 1st beacon is received
 *
 * and the deferred half (mgmt/scan.c:2486-2498) is guarded on all four of:
 *
 *     if ((!prAisBssInfo->ucDTIMPeriod) &&                       // once only
 *         EQUAL_MAC_ADDR(prBssDesc->aucBSSID, prAisBssInfo->aucBSSID) &&
 *         (prAisBssInfo->eCurrentOPMode == OP_MODE_INFRASTRUCTURE) &&
 *         ((prWlanBeaconFrame->u2FrameCtrl & MASK_FRAME_TYPE) == MAC_FRAME_BEACON)) {
 *         prAisBssInfo->ucDTIMPeriod = prBssDesc->ucDTIMPeriod;
 *         nicPmIndicateBssConnected(prAdapter, NETWORK_TYPE_AIS_INDEX);
 *     }
 *
 * -- a beacon, not a probe response, because only a beacon carries a TIM element
 * to read the DTIM count out of. This used to send `dtim ? dtim : 1` and
 * `beacon_interval ? beacon_interval : 100` at the end of the join: two numbers
 * the AP never said, put into the one command that tells the firmware how long to
 * keep the receiver off. A wrong DTIM there is missed multicast and missed
 * buffered traffic, silently. So the fabrications are gone and the caller is the
 * beacon path; there is no power save until the AP has told us its schedule.
 */
static int send_pm_connected(void)
{
    uint8_t command[12];
    if (g_assoc_profile.dtim_period == 0u || g_assoc_profile.beacon_interval == 0u) return -1;
    zero_bytes(command, sizeof(command));
    command[0] = NETWORK_TYPE_AIS;
    command[1] = g_assoc_profile.dtim_period;
    write_le16(command + 2u, g_assoc_aid);
    write_le16(command + 4u, g_assoc_profile.beacon_interval);
    return send_normal_command(CMD_ID_INDICATE_PM_BSS_CONNECTED, command, sizeof(command));
}

/*
 * CMD_802_11_KEY, 64 bytes (include/nic_cmd_event.h:924-937), filled by
 * wlanoidSetAddKey (common/wlan_oid.c:3183-3240):
 *
 *   0  ucAddRemove         1 = add
 *   1  ucTxKey             u4KeyIndex & IS_TRANSMIT_KEY  (BIT(31))
 *   2  ucKeyType           u4KeyIndex & IS_UNICAST_KEY   (BIT(30))
 *   3  ucIsAuthenticator   u4KeyIndex & IS_AUTHENTICATOR (BIT(28)); we are the
 *                          supplicant, so 0 -- privacy.h:115-117
 *   4  aucPeerAddr[6]      the PARAM_KEY BSSID
 *   10 ucNetType           0, "AIS", written as a literal by stock
 *   11 ucAlgorithmId       CIPHER_SUITE_CCMP = 4 (include/mgmt/privacy.h:123),
 *                          which is what a 16-byte key with eAuthMode >= WPA
 *                          resolves to; below that it would be WEP128
 *   12 ucKeyId             u4KeyIndex & 0xff
 *   13 ucKeyLen            the key length, 16 for CCMP
 *   16 aucKeyMaterial[32]
 *   48 aucKeyRsc[16]       see below
 *
 * The two flags and the peer address are not independent -- they are three
 * readings of the one thing cfg80211 passes, and mtk_cfg80211_add_key
 * (os/linux/gl_cfg80211.c:201-227) derives all three from whether there is a
 * mac_addr:
 *
 *   pairwise: mac_addr is the AP, so arBSSID = BSSID and both BIT(31) and
 *             BIT(30) go on -- ucTxKey = ucKeyType = 1. wlanoidSetAddKey then
 *             *rejects* a pairwise key whose id is not 0 or whose address is
 *             broadcast (wlan_oid.c:3116-3123), which is why the PTK call site
 *             passes key id 0 and the BSSID.
 *   group:    mac_addr is NULL, so arBSSID = ff:ff:ff:ff:ff:ff and neither bit
 *             is set -- ucTxKey = ucKeyType = 0. Stock even leaves the reason
 *             in a commented-out line: "Enable BIT 31 will make tx use bc key
 *             id, should use pairwise key id 0".
 *
 * aucKeyRsc IS LEFT ZERO. This used to copy the eight bytes of EAPOL Key RSC
 * into it, and stock's CCMP path never writes that field at all -- the only
 * assignment to aucKeyRsc in the whole tree is in the WAPI variant of
 * wlanoidSetAddKey (wlan_oid.c:9035), which copies 16 bytes of WPI PN for
 * CIPHER_SUITE_WPI. So the field's format is unverifiable from here, and the
 * failure mode of guessing it wrong is not "no replay protection", it is a
 * silent broadcast blackhole: hand the firmware a byte-swapped starting PN and
 * every group frame the AP sends afterwards looks like a replay. Zero is what
 * the driver this firmware shipped with sends.
 */
static int install_ccmp_key(const uint8_t peer[6], uint8_t key_id, int pairwise,
                            const uint8_t key[16])
{
    uint8_t command[64];
    zero_bytes(command, sizeof(command));
    command[0] = 1u; /* add */
    command[1] = pairwise ? 1u : 0u;
    command[2] = pairwise ? 1u : 0u;
    command[3] = 0u;
    copy_bytes(command + 4u, peer, 6u);
    command[10] = NETWORK_TYPE_AIS;
    command[11] = 4u; /* CIPHER_SUITE_CCMP */
    command[12] = key_id;
    command[13] = 16u;
    copy_bytes(command + 16u, key, 16u);
    return send_normal_command(CMD_ID_ADD_REMOVE_KEY, command, sizeof(command));
}

static int send_channel_request(void)
{
    uint8_t command[20];
    zero_bytes(command, sizeof(command));
    ++g_channel_token;
    if (g_channel_token == 0u) ++g_channel_token;
    command[0] = NETWORK_TYPE_AIS;
    command[1] = g_channel_token;
    command[2] = 0u; /* request */
    command[3] = g_assoc_profile.channel ? g_assoc_profile.channel : 1u;
    command[4] = 0u;
    command[5] = 1u; /* BAND_2G4 */
    command[6] = 0u; /* CH_REQ_TYPE_JOIN */
    /*
     * 2000, not the 5000 that used to be here. Stock's join request is built at
     * 0xc0413c64 in aisFsmSteps and every field is now accounted for against it:
     *
     *   c0413c94  mov r6, #2000          u4MaxInterval, in milliseconds
     *   c0413cb0  str lr, [ip,#24]       eReqType  = 0  (CH_REQ_TYPE_JOIN)
     *   c0413cb4  str r6, [ip,#28]       u4MaxInterval
     *   c0413cac  strb r5, [ip,#13]      ucTokenID = ++ucSeqNumOfChReq, from 1
     *   c0413cc0  strb lr, [ip,#14]      ucPrimaryChannel from prBssDesc[78]
     *   c0413ccc/cd8                     eRfSco, eRfBand from prBssDesc[80]/[84]
     *   c0413ce4/cec                     aucBSSID, 4 + 2
     *
     * The 5000 was invented. It is very unlikely to be why the firmware says
     * nothing -- this is how long the grant is held before it lapses, not
     * whether it is issued -- but "a number stock does not send" is not
     * something to leave in the one command that is being ignored.
     *
     * CONFIRMED FROM SOURCE. CMD_CH_PRIVILEGE_T (include/nic_cmd_event.h) is
     * {ucNetTypeIndex, ucTokenID, ucAction, ucPrimaryChannel, ucRfSco, ucRfBand,
     * ucReqType, ucReserved, u4MaxInterval, aucBSSID[6], aucReserved[2]} -- 20
     * bytes, and cnmChMngrRequestPrivilege (mgmt/cnm.c:329-338) fills that exact
     * set and sends sizeof(CMD_CH_PRIVILEGE_T). The interval is a named constant,
     * AIS_JOIN_CH_REQUEST_INTERVAL = 2000 (include/mgmt/ais_fsm.h:259), used at
     * mgmt/ais_fsm.c:2376; the token is `++prAisFsmInfo->ucSeqNumOfChReq` from a
     * counter zeroed at init (ais_fsm.c:1150, 2374), i.e. first request is 1, which
     * is what the wrap above keeps true. CH_REQ_TYPE_JOIN = 0 (mgmt/cnm.h:121-126),
     * BAND_2G4 = 1 and CHNL_EXT_SCN = 0 (nic/wlan_def.h:582-595).
     */
    write_le32(command + 8u, 2000u);
    copy_bytes(command + 12u, g_assoc_profile.bssid, 6u);
    const int rc = send_normal_command(CMD_ID_CH_PRIVILEGE, command, sizeof(command));
    /* Held from the moment the request is on the wire, not from the grant event:
     * a request the firmware acted on but never reported back is still a request
     * that has to be released, and that is the case worth being right about. */
    if (rc == 0) g_channel_held = 1u;
    return rc;
}

/*
 * THE OTHER HALF OF A PAIR THIS DRIVER HAS ONLY EVER SENT ONE HALF OF.
 *
 * CH_PRIVILEGE is a request/release protocol: byte 2 selects which, and the
 * token in byte 1 says which outstanding request is being talked about. Every
 * join asks the firmware to hand the radio over to one channel for a join, and
 * until now nothing ever gave it back -- not on timeout, not on failure, not
 * ever. The firmware was left owing the channel to an attempt that no longer
 * exists.
 *
 * That is the shape of what the console shows: two scans work, one `wifi join`
 * times out, and from then on every scan is submitted cleanly and answered by
 * nothing at all -- rx frozen, no SCAN_DONE, GET_STATISTICS unanswered. A scan
 * has to go off-channel, and a firmware holding a channel grant for a pending
 * join will not.
 *
 * Whether releasing is enough to recover a firmware that has already stopped
 * answering is a separate question and this does not assume it is. What is not
 * in question is that asking for something and never releasing it is wrong, and
 * that the symptom is exactly the symptom that mistake produces.
 */
static int send_channel_abort(void)
{
    uint8_t command[20];
    /*
     * THREE FIELDS, NOT NINE. cnmChMngrAbortPrivilege (mgmt/cnm.c:409-411) writes
     * exactly:
     *
     *     prCmdBody->ucNetTypeIndex = prMsgChAbort->ucNetTypeIndex;
     *     prCmdBody->ucTokenID      = prMsgChAbort->ucTokenID;
     *     prCmdBody->ucAction       = CMD_CH_ACTION_ABORT;
     *
     * into a body it got from cnmMemAlloc and never zeroed, and then sends the
     * whole sizeof(CMD_CH_PRIVILEGE_T). So bytes 3..19 of an abort are literally
     * whatever was in that heap slot: the firmware cannot be reading them, and the
     * token in byte 1 is what identifies the grant being given back. Channel, band,
     * request type and BSSID used to be filled in here to "match the request",
     * which sounds careful and is the same class of invention as the 5000 ms above
     * -- a value stock does not send, in the command being investigated.
     *
     * CMD_CH_ACTION_REQ = 0 and CMD_CH_ACTION_ABORT = 1
     * (include/nic_cmd_event.h:662-663), which is where byte 2's two values come
     * from in both directions.
     */
    zero_bytes(command, sizeof(command));
    command[0] = NETWORK_TYPE_AIS;
    command[1] = g_channel_token; /* the token the request went out under */
    command[2] = 1u;              /* CMD_CH_ACTION_ABORT, against 0 for request */
    return send_normal_command(CMD_ID_CH_PRIVILEGE, command, sizeof(command));
}

/*
 * GIVE THE CHANNEL BACK THE MOMENT THE JOIN IS OVER, WHICH IS NOT THE SAME
 * MOMENT THE JOIN FAILS.
 *
 * The release above was only ever reached from the abandon path, so a join that
 * SUCCEEDED left the grant outstanding for the whole life of the link. That is
 * measurably what breaks `wifi scan` while associated: the console shows the
 * scan submitted cleanly, then twelve seconds in which rx does not advance by a
 * single frame, no SCAN_DONE, no events at all -- and a scan cannot happen
 * without leaving the operating channel, which is exactly what an outstanding
 * CH_REQ_TYPE_JOIN grant forbids. Every command after that is answered by
 * nothing, TC4 never gets a page credited back, and the next scan is refused
 * with tx-resource-starved: the starvation is the symptom, the pinned channel
 * is the cause.
 *
 * Stock does not hold it either. aisFsmSteps calls aisFsmReleaseCh() on the way
 * into AIS_STATE_NORMAL_TR -- the grant covers the JOIN exchange and nothing
 * after it, because once SET_BSS_INFO has landed the firmware owns the BSS and
 * schedules its own channel time.
 *
 * Idempotent by g_channel_held, because the abandon path releases too and a
 * release against a token the firmware has already retired is its own bug.
 */
static void release_channel_if_held(void)
{
    if (!g_channel_held) return;
    g_channel_held = 0u;
    (void)send_channel_abort();
}

static int send_auth_request(void)
{
    uint8_t frame[30];
    zero_bytes(frame, sizeof(frame));
    write_le16(frame + 0u, 0x00b0u);
    copy_bytes(frame + 4u, g_assoc_profile.bssid, 6u);
    copy_bytes(frame + 10u, g_wifi_mac, 6u);
    copy_bytes(frame + 16u, g_assoc_profile.bssid, 6u);
    write_le16(frame + 24u, 0u);
    write_le16(frame + 26u, 1u);
    write_le16(frame + 28u, 0u);
    return send_hif_frame(frame, sizeof(frame), 3u, 0u, 1, 0, 1);
}

static uint32_t append_assoc_rsn(uint8_t* out, uint32_t capacity)
{
    static const uint8_t rsn[] = {
        48u, 20u, 1u, 0u,
        0x00u, 0x0fu, 0xacu, 0x04u,
        1u, 0u, 0x00u, 0x0fu, 0xacu, 0x04u,
        1u, 0u, 0x00u, 0x0fu, 0xacu, 0x02u,
        0u, 0u,
    };
    if (capacity < sizeof(rsn)) return 0u;
    copy_bytes(out, rsn, sizeof(rsn));
    return sizeof(rsn);
}

static int send_assoc_request(void)
{
    uint8_t frame[MAX_ASSOC_FRAME_SIZE];
    uint32_t length = 28u;
    const uint32_t ssid_len = string_length(g_assoc_profile.ssid, 32u);
    zero_bytes(frame, sizeof(frame));
    write_le16(frame + 0u, 0x0000u);
    copy_bytes(frame + 4u, g_assoc_profile.bssid, 6u);
    copy_bytes(frame + 10u, g_wifi_mac, 6u);
    copy_bytes(frame + 16u, g_assoc_profile.bssid, 6u);
    uint16_t capability = BIT(0);
    if (g_assoc_profile.capability & BIT(5)) capability |= BIT(5);
    if (g_assoc_profile.capability & BIT(10)) capability |= BIT(10);
    if (g_assoc_profile.security != WIFI_SECURITY_OPEN) capability |= BIT(4);
    write_le16(frame + 24u, capability);
    write_le16(frame + 26u, g_assoc_profile.dtim_period ? (uint16_t)g_assoc_profile.dtim_period * 2u : 10u);

    frame[length++] = 0u;
    frame[length++] = (uint8_t)ssid_len;
    copy_bytes(frame + length, (const uint8_t*)g_assoc_profile.ssid, ssid_len);
    length += ssid_len;

    uint32_t rates = g_assoc_profile.rate_count;
    if (rates == 0u)
    {
        static const uint8_t defaults[] = {0x82u, 0x84u, 0x8bu, 0x96u, 0x0cu, 0x12u, 0x18u, 0x24u, 0x30u, 0x48u, 0x60u, 0x6cu};
        rates = sizeof(defaults);
        copy_bytes(g_assoc_profile.rates, defaults, rates);
    }
    const uint32_t first_rates = rates > 8u ? 8u : rates;
    frame[length++] = 1u;
    frame[length++] = (uint8_t)first_rates;
    copy_bytes(frame + length, g_assoc_profile.rates, first_rates);
    length += first_rates;
    if (rates > first_rates)
    {
        frame[length++] = 50u;
        frame[length++] = (uint8_t)(rates - first_rates);
        copy_bytes(frame + length, g_assoc_profile.rates + first_rates, rates - first_rates);
        length += rates - first_rates;
    }
    if (g_assoc_profile.security == WIFI_SECURITY_WPA2_PSK_CCMP)
    {
        const uint32_t rsn_len = append_assoc_rsn(frame + length, sizeof(frame) - length);
        if (rsn_len == 0u) return -1;
        length += rsn_len;
    }
    return send_hif_frame(frame, length, 3u, 0u, 1, 0, 1);
}

static int valid_station_mac(const uint8_t mac[6])
{
    uint8_t any = 0u;
    uint8_t all_ff = 0xffu;
    for (uint32_t i = 0; i < 6u; ++i)
    {
        any |= mac[i];
        all_ff &= mac[i];
    }
    return any != 0u && all_ff != 0xffu && !multicast_mac(mac);
}

static void query_firmware_mac(void)
{
    uint8_t* packet = tx_buffer();
    const uint8_t sequence = next_command_sequence();
    const uint32_t packet_size = NORMAL_COMMAND_HEADER_SIZE + 12u;
    zero_bytes(packet, align4(packet_size));
    write_le16(packet + 0u, (uint16_t)align4(packet_size));
    packet[3] = (uint8_t)((HIF_TX_COMMAND_RESOURCE << HIF_TX_RESOURCE_SHIFT) |
                          (HIF_TX_PACKET_TYPE_CMD << HIF_TX_PACKET_TYPE_SHIFT));
    packet[4] = CMD_ID_BASIC_CONFIG;
    packet[5] = 0u; /* query */
    packet[6] = sequence;
    if (tx_resource_acquire(HIF_TX_COMMAND_RESOURCE) != 0) return;
    write_port(MCR_WTDR1, HIF_TARGET_TXD1, packet, align4(packet_size));

    const uint64_t deadline = mt6592_timer_microseconds() + INIT_ACK_TIMEOUT_US;
    uint32_t polls          = 0u;
    while (mt6592_timer_microseconds() < deadline)
    {
        uint8_t port;
        uint32_t length;
        if (!read_next_packet(&port, &length))
        {
            delay_us(100u);
            /* The one wait in this file that did not hand the CPU back. A
             * firmware that never answers the MAC query holds it for the full
             * second, and on a cooperative kernel a second not given back is a
             * second the compositor does not run. Same cadence as the other
             * bounded waits here, and taken only with nothing to read, so no
             * packet is left half-collected across the switch. */
            if ((++polls & 0x3fu) == 0u) wifi_cooperative_yield();
            continue;
        }
        if (receive_packet(port, length) != 0 || length < 20u) continue;
        const uint8_t* event = rx_buffer();
        if ((read_le16(event + 2u) & HIF_RX_PACKET_TYPE_MASK) != HIF_RX_PACKET_TYPE_EVENT ||
            event[4] != EVENT_ID_BASIC_CONFIG || event[5] != sequence)
            continue;
        if (valid_station_mac(event + 8u))
        {
            copy_bytes(g_wifi_mac, event + 8u, 6u);
            g_state.station_mac_from_firmware = 1;
        }
        g_state.mac_query_polls = polls;
        copy_bytes(g_state.station_mac, g_wifi_mac, 6u);
        return;
    }
    /*
     * Timed out. Leave station_mac_from_firmware clear and publish the default
     * anyway, so `wifi fw` shows the address the firmware is about to be told
     * next to the fact that the firmware never named one itself.
     */
    g_state.mac_query_polls = polls;
    copy_bytes(g_state.station_mac, g_wifi_mac, 6u);
}

/*
 * CMD_ID_SET_DOMAIN_INFO -- the channel list the firmware is allowed to use.
 *
 * Layout read off rlmDomainSendCmd (0xc0424188) and the tail call it makes to
 * rlmDomainPassiveScanSendCmd (0xc0423f3c). Both build the same 56-byte
 * CMD_SET_DOMAIN_INFO_T and send it under the same CID 19, differing only in
 * one halfword and in which table they draw the subbands from. The struct is
 * CMD_SET_DOMAIN_INFO_T (include/nic_cmd_event.h:1245-1253) over six
 * CMD_SUBBAND_INFO (1235-1242), which gives every offset below by name:
 *
 *   [0..1]  u2CountryCode, little-endian, ('E' << 8) | 'U'
 *   [2..3]  0 = the channels that are allowed at all
 *           1 = the subset of those that must be listened to passively
 *   [4..51] six 8-byte subbands, five used bytes each and three zero:
 *           { ucRegClass, ucBand, ucChannelSpan, ucFirstChannelNum, ucNumChannels }
 *   [52]    uc2G4Bandwidth   [53] uc5GBandwidth   [54..55] zero
 *
 * The stock loop copies only the first two bytes of a subband whose ucBand is
 * outside {1 = 2.4 GHz, 2 = 5 GHz} and leaves the rest zero, which is also how
 * it terminates a short list: ucNumChannels == 0 ends it. Source says the same
 * thing with the enum spelled out (mgmt/rlm_domain.c:603-614):
 *
 *     prCmd->rSubBand[i].ucRegClass = prSubBand->ucRegClass;
 *     prCmd->rSubBand[i].ucBand     = prSubBand->ucBand;
 *     if (prSubBand->ucBand != BAND_NULL && prSubBand->ucBand < BAND_NUM) {
 *         ... ucChannelSpan, ucFirstChannelNum, ucNumChannels
 *     }
 *
 * -- BAND_NULL/BAND_2G4/BAND_5G/BAND_NUM being 0/1/2/3 (include/nic/wlan_def.h:
 * 591), so "< BAND_NUM and not NULL" is exactly the {1, 2} test below.
 *
 * The one place the reference tree and the shipped kernel part company is that
 * halfword: this revision of rlmDomainSendCmd writes `prCmd->u2Reserved = 0`
 * (rlm_domain.c:597) and has no passive-scan sender at all. The J36 kernel is
 * the later driver that added one and gave the field a meaning. The shipped
 * binary is what the firmware on this board was built against, so the selector
 * stays; the rest of the struct is unchanged between the two.
 */
enum
{
    DOMAIN_COMMAND_PAYLOAD_SIZE = 56u,
    DOMAIN_TABLE_ALLOWED        = 0u,
    DOMAIN_TABLE_PASSIVE        = 1u,
    DOMAIN_COUNTRY_EU           = 0x4555u, /* 'E' << 8 | 'U' */
    DOMAIN_SUBBAND_COUNT        = 6u,
};

/*
 * The domain the stock driver falls back to when the NVRAM country code matches
 * none of its 22 groups -- entry 20 of the table at 0xc0b6f97c, the one
 * rlmDomainGetDomainInfo loads from its literal pool at 0xc0423d28. Channels
 * 1..13 on 2.4 GHz, which is the band we scan, plus the four 5 GHz subbands it
 * carries. Sent verbatim rather than trimmed to 2.4 GHz: the point of this
 * command is to stop differing from stock, and every group in that table except
 * the two most restrictive ones names the 5 GHz subbands too.
 */
static const uint8_t g_domain_allowed[DOMAIN_SUBBAND_COUNT][5] = {
    {  81u, 1u, 1u,   1u, 13u }, /* 2.4 GHz, channels 1..13 */
    { 115u, 2u, 4u,  36u,  4u }, /* 5 GHz, 36..48           */
    { 118u, 2u, 4u,  52u,  4u }, /* 5 GHz, 52..64           */
    { 121u, 2u, 4u, 100u, 12u }, /* 5 GHz, 100..144         */
    { 125u, 2u, 4u, 149u,  7u }, /* 5 GHz, 149..173         */
    {   0u, 0u, 0u,   0u,  0u },
};

/*
 * The passive-scan table, which is empty. Not an omission: the stock table at
 * 0xc0b6fd44 has exactly two entries and the default one is all zeros, so the
 * second command really does say "no channel is passive-only". Only country
 * "UD" fills anything in, and then only channel 12.
 */
static const uint8_t g_domain_passive[DOMAIN_SUBBAND_COUNT][5] = {
    { 0u, 0u, 0u, 0u, 0u }, { 0u, 0u, 0u, 0u, 0u }, { 0u, 0u, 0u, 0u, 0u },
    { 0u, 0u, 0u, 0u, 0u }, { 0u, 0u, 0u, 0u, 0u }, { 0u, 0u, 0u, 0u, 0u },
};

static int send_domain_table(uint16_t table, const uint8_t subbands[DOMAIN_SUBBAND_COUNT][5])
{
    uint8_t payload[DOMAIN_COMMAND_PAYLOAD_SIZE];

    zero_bytes(payload, sizeof(payload));
    write_le16(payload + 0u, DOMAIN_COUNTRY_EU);
    write_le16(payload + 2u, table);
    for (uint32_t i = 0; i < DOMAIN_SUBBAND_COUNT; ++i)
    {
        uint8_t* slot = payload + 4u + (i * 8u);
        slot[0] = subbands[i][0];
        slot[1] = subbands[i][1];
        if (subbands[i][1] != 1u && subbands[i][1] != 2u) continue;
        slot[2] = subbands[i][2];
        slot[3] = subbands[i][3];
        slot[4] = subbands[i][4];
    }
    /* [52]/[53] are the fixed-bandwidth overrides. Stock forwards two adapter
     * bytes -- rConnSettings.uc2G4BandwidthMode and uc5GBandwidthMode
     * (rlm_domain.c:598-601, declared include/nic/adapter.h:716 as "20/40M or
     * 20M only") -- that are zero unless someone has pinned a channel width,
     * and we have no such override, so zero here is the faithful value and not
     * a placeholder standing in for one we could not work out. */
    return send_normal_command(CMD_ID_SET_DOMAIN_INFO, payload, sizeof(payload));
}

int mt6592_wifi_hif_send_domain_info(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_alive || !g_state.configured)
    {
        set_failure("MediaTek WLAN domain info needs the firmware running first",
                    "mt6592-wifi:domain-firmware-not-ready");
        return -1;
    }
    if (acquire_driver_own() != 0) return -1;

    if (send_domain_table(DOMAIN_TABLE_ALLOWED, g_domain_allowed) != 0)
    {
        set_failure("MediaTek WLAN allowed-channel command failed", "mt6592-wifi:domain-allowed-submit-failed");
        return -1;
    }
    ++g_state.domain_cmds_sent;
    delay_us(2000u);

    if (send_domain_table(DOMAIN_TABLE_PASSIVE, g_domain_passive) != 0)
    {
        set_failure("MediaTek WLAN passive-channel command failed", "mt6592-wifi:domain-passive-submit-failed");
        return -1;
    }
    ++g_state.domain_cmds_sent;
    return 0;
#else
    return -1;
#endif
}

/*
 * WHAT STOCK SENDS HERE THAT WE DO NOT -- enumerated, not guessed.
 *
 * wlanAdapterStart (0xc03c0bb4) makes exactly this sequence of calls between the
 * firmware download and the first scan, and only four of them talk to the chip:
 *
 *   nicConfigPowerSaveProfile      CMD_ID_POWER_SAVE_MODE
 *   wlanLoadManufactureData        CIDs 49, 61, 54, 59, 60 + rlmDomainSendCmd(0x13)
 *   wlanQueryNicCapability         a query, answered into the driver's own struct
 *   wlanUpdateNetworkAddress       CMD_ID_BASIC_CONFIG (0xc1) -- we send this
 *
 * plus nicActivateNetwork (0xc03da544), which is exactly the four-byte
 * BSS_ACTIVATE_CTRL below: {ucNetTypeIndex, 1, pad, pad}, CID 21, length 4.
 *
 * The manufacture-data CIDs were the leading suspect for the deaf receiver and
 * they should not be any more. Disassembling wlanLoadManufactureData (0xc03bffb0)
 * shows all five carrying TX power and band-edge tables -- 49 is the 144-byte
 * power table, 54 the 4-byte CCK/HT20/HT40 band edges, 59/60/61 the rest, and the
 * one non-command in the middle is nicUpdateTxPower. Every one of them is
 * transmit-side. A PASSIVE SCAN NEVER KEYS THE TRANSMITTER, so none of them can
 * explain a passive scan that hears nothing, and sending them would prove
 * nothing about the symptom we have. They stay relevant to the other symptom --
 * the board dying the instant an ACTIVE scan starts, which is the one case where
 * transmitting into an unconfigured front end is exactly what happens.
 *
 * That leaves POWER_SAVE_MODE as the only untried command on the stock path, and
 * "the radio never woke up" as the hypothesis to test before adding any of them;
 * see the sparse-channel report in the SCAN_DONE handler.
 */
static int configure_firmware_runtime(void)
{
    uint8_t basic_config[12];
    uint8_t activate[4];
    uint8_t ps_profile[4];

    query_firmware_mac();

    /*
     * First, because it is first in wlanAdapterStart: put the AIS network into
     * CAM. Non-fatal -- stock does not check the return either, and a firmware
     * that refuses this still scans.
     *
     * CMD_PS_PROFILE_T, 4 bytes (include/nic_cmd_event.h:1144-1148):
     * {ucNetTypeIndex, ucPsProfile, aucReserved[2]}, sent by
     * nicConfigPowerSaveProfile (nic/nic.c:2454-2488) with fgEnCmdEvent FALSE
     * at start, so no response to wait for.
     *
     * THE PROFILE BYTE IS A DELIBERATE DIVERGENCE, so it is spelled out. Stock
     * sends 2 here: gl_init.c:2940 sets u4PowerMode = CFG_INIT_POWER_SAVE_PROF
     * unconditionally, and config.h:1211 defines that as ENUM_PSP_FAST_SWITCH,
     * which is 2 (include/wlan_oid.h:582-585). We send 0, which is the same
     * number under either of the two parallel enums the field is written
     * through -- Param_PowerModeCAM (wlan_oid.h:542) and
     * ENUM_PSP_CONTINUOUS_ACTIVE (wlan_oid.h:582) -- and it is not invented:
     * it is what stock itself sends from the #else arm of wlanAdapterStart
     * (common/wlan_lib.c:1775) when CFG_SUPPORT_PWR_MGT is off.
     *
     * Off is what we are. Fast-switch hands the firmware the right to sleep
     * between DTIMs, and the driver on this side has no power-save state
     * machine to keep in step with it -- the one PS command it sends is the
     * PM_BSS_CONNECTED that follows the first real beacon. A radio that sleeps
     * on a schedule nobody here is tracking loses packets silently, which is
     * the failure this whole pass is meant to stop.
     */
    zero_bytes(ps_profile, sizeof(ps_profile));
    ps_profile[0] = NETWORK_TYPE_AIS;
    ps_profile[1] = 0u; /* Param_PowerModeCAM: constantly awake */
    if (send_normal_command(CMD_ID_POWER_SAVE_MODE, ps_profile, sizeof(ps_profile)) == 0)
    {
        ++g_state.ps_cmds_sent;
    }

    /*
     * CMD_BASIC_CONFIG, 12 bytes (include/nic_cmd_event.h:951-963):
     *
     *   0  rMyMacAddr[6]
     *   6  ucNative80211      0
     *   7  aucReserved[1]
     *   8  u2RxChecksum       bit0 IP, bit1 UDP, bit2 TCP
     *   10 u2TxChecksum       same bits
     *
     * wlanUpdateNetworkAddress (common/wlan_lib.c:3983-3987) writes all four
     * fields explicitly, and the two checksum halfwords are assigned 0 before
     * anything can set a bit in them -- the bits only go on under
     * CFG_TCP_IP_CHKSUM_OFFLOAD, from prAdapter->u4CSUMFlags. Nothing here asks
     * the firmware to compute a checksum, so zero is the value and not a gap:
     * offload left off means the host stack keeps doing what it already does.
     */
    zero_bytes(basic_config, sizeof(basic_config));
    copy_bytes(basic_config, g_wifi_mac, sizeof(g_wifi_mac));
    if (send_normal_command(CMD_ID_BASIC_CONFIG, basic_config, sizeof(basic_config)) != 0)
    {
        set_failure("MediaTek WLAN basic configuration command failed", "mt6592-wifi:basic-config-submit-failed");
        return -1;
    }

    /*
     * CMD_BSS_ACTIVATE_CTRL, 4 bytes (include/nic_cmd_event.h:1299-1303):
     * {ucNetTypeIndex, ucActive, aucReserved[2]}, filled by nicActivateNetwork
     * (nic/nic.c:2090-2091) with ucActive hard-coded to 1. Stock leaves the two
     * reserved bytes as whatever was on its stack; we zero them.
     */
    zero_bytes(activate, sizeof(activate));
    activate[0] = NETWORK_TYPE_AIS;
    activate[1] = 1u;
    if (send_normal_command(CMD_ID_BSS_ACTIVATE_CTRL, activate, sizeof(activate)) != 0)
    {
        set_failure("MediaTek WLAN AIS activation command failed", "mt6592-wifi:ais-activate-submit-failed");
        return -1;
    }

    g_state.configured = 1;
    g_state.bss_active = 1;

    /*
     * The channel list, sent HERE because this is where stock sends it:
     * wlanAdapterStart calls rlmDomainSendCmd during adapter start, long before
     * anything can scan or associate. Carrying it as a console-only command was
     * the bug, and hardware named it.
     *
     * MEASURED 2026-08-09: with a firmware that was demonstrably alive -- MAC
     * read back from it, nine networks found, fw_alive=yes -- `wifi step` sent
     * UPDATE_STA_RECORD, CH_PRIVILEGE request and CH_PRIVILEGE release and
     * rx/mgmt/evt did not move by one across any of the three, twice in a row.
     * Not a wedge: a `wifi scan` after them still swept and still found nine.
     * The firmware accepted all three and did nothing with any of them, and the
     * state dump read `domain_cmds=0`.
     *
     * The measurement stands; the reading of it that used to be here does not.
     * It concluded "empty domain table", on the argument that the firmware's
     * channel manager mirrors rlmDomainIsLegalChannel and silently drops a
     * request for a channel it considers illegal. That is a real mechanism and
     * this command is genuinely sent in the wrong place before, so the fix below
     * is kept -- but it is not what was happening, and the run above says so if
     * you read step 0 correctly. Step 0 was UPDATE_STA_RECORD under CID 0x18,
     * which is REMOVE_STA_RECORD (see the CID block near the top of this file),
     * so every one of those runs deleted the AIS station record before asking for
     * a channel for it. Scanning survived for the reason it always survives: the
     * firmware sweeps off its own list and consults neither the domain nor the
     * station table to do it.
     *
     * Not fatal if the submit fails. The scan path provably works without this
     * command, so a domain command that cannot be sent should cost association,
     * not the radio -- but say so on the status line rather than reporting a
     * transport that is only three quarters up.
     */
    if (mt6592_wifi_hif_send_domain_info() != 0)
    {
        g_state.status = "MediaTek WLAN firmware alive; scan ready, channel list refused";
        return 0;
    }

    g_state.status     = "MediaTek WLAN firmware alive; active scan transport ready";
    g_state.blocked    = 0;
    return 0;
}

static int profile_supports_wpa2_psk_ccmp(const uint8_t* rsn, uint32_t len)
{
    if (!rsn || len < 18u || read_le16(rsn) != 1u) return 0;
    if (rsn[2] != 0x00u || rsn[3] != 0x0fu || rsn[4] != 0xacu || rsn[5] != 0x04u) return 0;
    uint32_t offset = 6u;
    if (offset + 2u > len) return 0;
    const uint16_t pair_count = read_le16(rsn + offset);
    offset += 2u;
    int ccmp = 0;
    for (uint16_t i = 0; i < pair_count; ++i)
    {
        if (offset + 4u > len) return 0;
        if (rsn[offset] == 0x00u && rsn[offset + 1u] == 0x0fu &&
            rsn[offset + 2u] == 0xacu && rsn[offset + 3u] == 0x04u) ccmp = 1;
        offset += 4u;
    }
    if (offset + 2u > len) return 0;
    const uint16_t akm_count = read_le16(rsn + offset);
    offset += 2u;
    int psk = 0;
    for (uint16_t i = 0; i < akm_count; ++i)
    {
        if (offset + 4u > len) return 0;
        if (rsn[offset] == 0x00u && rsn[offset + 1u] == 0x0fu &&
            rsn[offset + 2u] == 0xacu && rsn[offset + 3u] == 0x02u) psk = 1;
        offset += 4u;
    }
    return ccmp && psk;
}

/*
 * ELEM_ID_TIM = 5, and the DTIM *period* is its second octet -- the first is the
 * DTIM count, which counts down and is 0 in every DTIM beacon, so reading body[0]
 * would give the schedule as zero exactly when it matters. Same offset
 * parse_bss_ies uses for the same element; this is the one field of it that a
 * connected station still needs, so it is worth reading without building a whole
 * profile out of an associated AP's beacon.
 */
static uint8_t beacon_dtim_period(const uint8_t* ies, uint32_t ies_len)
{
    uint32_t offset = 0u;
    while (offset + 2u <= ies_len)
    {
        const uint8_t id = ies[offset];
        const uint8_t len = ies[offset + 1u];
        offset += 2u;
        if (offset + len > ies_len) break;
        if (id == 5u && len >= 2u) return ies[offset + 1u];
        offset += len;
    }
    return 0u;
}

static int parse_bss_ies(const uint8_t* ies, uint32_t ies_len, wifi_bss_profile* profile)
{
    uint32_t offset = 0u;
    int have_ssid = 0;
    int privacy = (profile->capability & BIT(4)) != 0u;
    profile->security = privacy ? WIFI_SECURITY_UNSUPPORTED : WIFI_SECURITY_OPEN;
    while (offset + 2u <= ies_len)
    {
        const uint8_t id = ies[offset];
        const uint8_t len = ies[offset + 1u];
        offset += 2u;
        if (offset + len > ies_len) break;
        const uint8_t* body = ies + offset;
        if (id == 0u && !have_ssid && len <= 32u)
        {
            for (uint32_t i = 0; i < len; ++i) profile->ssid[i] = (char)body[i];
            profile->ssid[len] = '\0';
            have_ssid = len != 0u;
        }
        else if ((id == 1u || id == 50u) && len != 0u)
        {
            for (uint32_t i = 0; i < len; ++i)
            {
                const uint16_t bit = rate_bit(body[i]);
                profile->operational_rates |= bit;
                if (body[i] & 0x80u) profile->basic_rates |= bit;
                if (profile->rate_count < sizeof(profile->rates))
                    profile->rates[profile->rate_count++] = body[i];
            }
        }
        else if (id == 3u && len >= 1u)
        {
            profile->channel = body[0];
        }
        else if (id == 5u && len >= 2u)
        {
            profile->dtim_period = body[1];
        }
        else if (id == 48u)
        {
            privacy = 1;
            if (len + 2u <= sizeof(profile->rsn_ie))
            {
                profile->rsn_ie[0] = id;
                profile->rsn_ie[1] = len;
                copy_bytes(profile->rsn_ie + 2u, body, len);
                profile->rsn_ie_len = (uint8_t)(len + 2u);
            }
            profile->security = profile_supports_wpa2_psk_ccmp(body, len)
                ? WIFI_SECURITY_WPA2_PSK_CCMP
                : WIFI_SECURITY_UNSUPPORTED;
        }
        else if (id == 221u && len >= 4u && body[0] == 0x00u && body[1] == 0x50u &&
                 body[2] == 0xf2u && body[3] == 0x01u)
        {
            privacy = 1;
            if (profile->security != WIFI_SECURITY_WPA2_PSK_CCMP)
                profile->security = WIFI_SECURITY_UNSUPPORTED;
        }
        offset += len;
    }
    if (privacy && profile->security == WIFI_SECURITY_OPEN) profile->security = WIFI_SECURITY_UNSUPPORTED;
    return have_ssid;
}

static void store_scan_result(const wifi_bss_profile* profile, int rssi)
{
    uint32_t slot = MAX_SCAN_RESULTS;
    if (!profile || profile->ssid[0] == '\0') return;

    for (uint32_t i = 0; i < g_state.scan_result_count; ++i)
    {
        if (same_mac(g_scan_profiles[i].bssid, profile->bssid) || same_string(g_scan_profiles[i].ssid, profile->ssid))
        {
            slot = i;
            break;
        }
    }
    if (slot == MAX_SCAN_RESULTS)
    {
        if (g_state.scan_result_count < MAX_SCAN_RESULTS)
            slot = g_state.scan_result_count++;
        else
        {
            int weakest_rssi = g_scan_results[0].rssi;
            uint32_t weakest = 0u;
            for (uint32_t i = 1; i < MAX_SCAN_RESULTS; ++i)
            {
                if (g_scan_results[i].rssi < weakest_rssi)
                {
                    weakest_rssi = g_scan_results[i].rssi;
                    weakest = i;
                }
            }
            if (rssi <= weakest_rssi) return;
            slot = weakest;
        }
    }
    if (g_scan_results[slot].ssid[0] != '\0' && rssi < g_scan_results[slot].rssi) return;

    if (rssi < -127) rssi = -127;
    if (rssi > 0) rssi = 0;
    g_scan_profiles[slot] = *profile;
    zero_bytes((uint8_t*)&g_scan_results[slot], sizeof(g_scan_results[slot]));
    copy_string(g_scan_results[slot].ssid, sizeof(g_scan_results[slot].ssid), profile->ssid);
    g_scan_results[slot].rssi = (int16_t)rssi;
    g_scan_results[slot].encrypted = profile->security == WIFI_SECURITY_OPEN ? 0u : 1u;
}

static void association_auth_response(const uint8_t* frame, uint32_t frame_len)
{
    if (g_assoc_phase != ASSOC_WAIT_AUTH || frame_len < 30u || !same_mac(frame + 10u, g_assoc_profile.bssid)) return;
    if (read_le16(frame + 24u) != 0u || read_le16(frame + 26u) != 2u || read_le16(frame + 28u) != 0u)
    {
        set_assoc_failure("MediaTek Wi-Fi open-system authentication was rejected", "mt6592-wifi:auth-rejected");
        return;
    }
    /*
     * NO STATE_2 SYNC HERE. It looked like the obvious third of a three-state
     * progression and it is a command stock never sends -- cnmStaRecChangeState
     * (mgmt/cnm_mem.c:1049-1062) opens by returning before the send:
     *
     *     // Do nothing when following state transitions happen,
     *     // other 6 conditions should be sync to FW, including 1-->1, 3-->3
     *     if ((ucNewState == STA_STATE_2 && prStaRec->ucStaState != STA_STATE_3) ||
     *         (ucNewState == STA_STATE_1 && prStaRec->ucStaState == STA_STATE_2)) {
     *         prStaRec->ucStaState = ucNewState;
     *         return;
     *     }
     *
     * The state after authentication is STA_STATE_1, so `1 --> 2` takes the first
     * arm and the firmware is never told. Note what the comment does say is sent:
     * 1-->1, which is the join's opening STA_STATE_1 update, and 3-->3. Only the
     * two transitions above are silent, and they are silent because class-2 frame
     * acceptance changes nothing the firmware holds -- an association request goes
     * out as a management frame on TC4 either way.
     */
    if (send_assoc_request() != 0)
    {
        set_assoc_failure("MediaTek Wi-Fi association request could not be submitted", "mt6592-wifi:assoc-submit-failed");
        return;
    }
    g_assoc_phase = ASSOC_WAIT_ASSOC;
    g_assoc_retries = 0u;
    g_assoc_deadline_us = mt6592_timer_microseconds() + ASSOC_TIMEOUT_US;
    g_state.status = "MediaTek Wi-Fi authenticated; association in progress";
    g_state.blocked = 0;
}

/*
 * THE END OF A JOIN, for both kinds of network, in one place.
 *
 * "Associated" and "connected" were the same instant for an open network and
 * two very different instants for a WPA2 one, and the three things that have to
 * happen at the *end* were being done at the start. They are, in order:
 *
 *   1. Announce power save. Only now -- see the note in association_response()
 *      -- and only if a beacon has already given us the AP's DTIM period, which
 *      it usually has, because the scan that found this BSS read it out of the
 *      TIM element. If it has not, indicate_pm_from_beacon() picks it up off the
 *      next beacon instead, which is where stock does it from in every case. See
 *      send_pm_connected().
 *   2. Give the channel back, so a later `wifi scan` can leave the operating
 *      channel. See release_channel_if_held().
 *   3. Open the data path.
 *
 * Order matters between 1 and 2 only in that both are commands and TC4 is four
 * pages deep; neither depends on the other's answer.
 */
static void association_completed(int secure)
{
    g_pm_connected_sent = (send_pm_connected() == 0) ? 1u : 0u;
    release_channel_if_held();
    g_assoc_phase = ASSOC_CONNECTED;
    g_state.auth_active = 0;
    g_state.associated = 1;
    g_state.data_path_ready = 1;
    g_state.secure = secure ? 1 : 0;
    g_state.blocked = 0;
}

static void association_response(const uint8_t* frame, uint32_t frame_len)
{
    if (g_assoc_phase != ASSOC_WAIT_ASSOC || frame_len < 30u || !same_mac(frame + 10u, g_assoc_profile.bssid)) return;
    const uint16_t status = read_le16(frame + 26u);
    if (status != 0u)
    {
        set_assoc_failure("MediaTek Wi-Fi association was rejected", "mt6592-wifi:assoc-rejected");
        return;
    }
    g_assoc_aid = read_le16(frame + 28u) & 0x3fffu;
    /*
     * SET_BSS_INFO first, then STA_STATE_3 -- this pair used to be sent the other
     * way round, which is the reverse of stock. aisFsmStateAbort's successful
     * infra-JOIN branch (mgmt/ais_fsm.c:2872-2893) runs
     * aisChangeMediaState(CONNECTED), then aisUpdateBssInfoForJOIN (which is what
     * emits SET_BSS_INFO, via nicUpdateBss), and only then
     * cnmStaRecChangeState(..., STA_STATE_3).
     *
     * The order is not cosmetic. STA_STATE_3 is "accept class 3 frames", and
     * class 3 frames are meaningful only within a BSS -- so it promotes a record
     * whose BSS the firmware has not yet been told is connected. The station
     * record itself already exists on the firmware side either way: the join
     * opens with send_sta_record(STA_STATE_1) before the auth request (see the
     * authentication path), which is also why stock can reference
     * ucStaRecIdxOfAP from inside SET_BSS_INFO at nic.c:2247.
     *
     * Symptom this matches: after the assoc response, RX stopped completely --
     * not "data frames were filtered", but WRPLR flat at 0 for every port and
     * every poll, with TC4 credits never returned. That is the shape of a
     * firmware that stopped servicing the link, and it is why the four-way
     * handshake never saw message 1.
     */
    if (send_bss_info(0) != 0 || send_sta_record(STA_STATE_3) != 0)
    {
        set_assoc_failure("MediaTek firmware rejected connected BSS configuration", "mt6592-wifi:bss-config-failed");
        return;
    }
    g_state.associated = 1;
    g_assoc_retries = 0u;
    if (g_assoc_profile.security == WIFI_SECURITY_OPEN)
    {
        association_completed(0);
        g_state.status = "MediaTek Wi-Fi associated; open-network Ethernet path ready";
    }
    else
    {
        /*
         * INDICATE_PM_BSS_CONNECTED is NOT sent here, and that is the whole
         * difference between the network this driver joins and the network it
         * does not.
         *
         * It is what puts the station into DTIM power save. Announce that before
         * the 802.1X port is open and the AP is entitled to buffer EAPOL-M1
         * against the TIM instead of transmitting it -- a station that has said
         * it sleeps gets its unicast traffic held for it. Nothing here wakes for
         * a TIM bit, so M1 is buffered, never fetched, and the handshake runs out
         * its twelve seconds having heard nothing. An OPEN network has no
         * handshake to lose, which is exactly why open joins and WPA2 does not.
         *
         * Stock has the same ordering for the same reason: nicPmIndicateBssConnected
         * is driven from the connected *indication*, after aisUpdateBssInfoForJOIN
         * and after the port is authorised, not from the association response.
         *
         * So the radio stays awake for the four-way, and association_completed()
         * sends it once the keys are in.
         */
        g_assoc_phase = ASSOC_WAIT_EAPOL_M1;
        g_assoc_deadline_us = mt6592_timer_microseconds() + EAPOL_TIMEOUT_US;
        g_state.status = "MediaTek Wi-Fi associated; WPA2 four-way handshake in progress";
        g_state.blocked = 0;
    }
}

static void process_management_packet(const uint8_t* packet, uint32_t length)
{
    if (length < HIF_RX_HEADER_SIZE) return;
    const uint16_t packet_len = read_le16(packet + 0u);
    const uint32_t header_offset = packet[4] & 0x3u;
    uint32_t usable_len = packet_len;
    if (usable_len > length) usable_len = length;
    if (HIF_RX_HEADER_SIZE + header_offset + 24u > usable_len)
    {
        ++g_state.dropped_packets;
        return;
    }

    const uint8_t* frame = packet + HIF_RX_HEADER_SIZE + header_offset;
    const uint32_t frame_len = usable_len - HIF_RX_HEADER_SIZE - header_offset;
    const uint16_t subtype = read_le16(frame) & 0x00f0u;
    if (same_mac(frame + 4u, g_wifi_mac))
    {
        if (subtype == 0x00b0u) association_auth_response(frame, frame_len);
        else if (subtype == 0x0010u) association_response(frame, frame_len);
        else if ((subtype == 0x00a0u || subtype == 0x00c0u) && same_mac(frame + 10u, g_assoc_profile.bssid))
        {
            /* Reason code is the first field of the frame body, at 24. The
             * dispatch above already guarantees 24 bytes; the code needs 26. */
            g_state.disconnect_reason     = frame_len >= 26u ? read_le16(frame + 24u) : 0u;
            g_state.disconnect_was_deauth = subtype == 0x00c0u ? 1u : 0u;
            mt6592_uart_puts(subtype == 0x00c0u ? "  wifi: deauthenticated by the AP, reason="
                                                : "  wifi: disassociated by the AP, reason=");
            mt6592_uart_put_hex32(g_state.disconnect_reason);
            mt6592_uart_puts("\n");
            set_assoc_failure("MediaTek Wi-Fi link was disconnected by the access point", "mt6592-wifi:ap-disconnect");
        }
    }

    /*
     * The deferred half of scanProcessBeaconAndProbeResp (mgmt/scan.c:2486-2498),
     * with stock's four guards intact: not announced yet, our BSSID, we are
     * connected as a station, and this is a BEACON -- 0x0080, not a probe response,
     * because the DTIM period lives in the TIM element and only beacons carry one.
     * See send_pm_connected() for why the alternative is not to guess.
     */
    if (!g_pm_connected_sent && subtype == 0x0080u && g_assoc_phase == ASSOC_CONNECTED &&
        frame_len >= 36u && same_mac(frame + 16u, g_assoc_profile.bssid))
    {
        const uint16_t interval = read_le16(frame + 32u);
        const uint8_t dtim = beacon_dtim_period(frame + 36u, frame_len - 36u);
        if (dtim != 0u && interval != 0u)
        {
            g_assoc_profile.dtim_period = dtim;
            g_assoc_profile.beacon_interval = interval;
            g_pm_connected_sent = (send_pm_connected() == 0) ? 1u : 0u;
        }
    }

    /*
     * Recorded whenever one arrives, not only while a sweep is running.
     *
     * This used to be gated on g_state.scan_active, which threw away precisely
     * the beacons the driver receives best: while associated and idle the radio
     * sits on the AP's channel and hears its beacon every ~100 ms, and every one
     * of them was discarded. The next `wifi scan' then started from an empty
     * table and had to re-hear that AP inside whatever dwell the sweep gave its
     * channel -- so the one network guaranteed to be in range, the one we were
     * joined to, was the one most likely to be missing from the results.
     *
     * Stock keeps its BSS descriptor list the same way: scanProcessBeaconAndProbeResp
     * adds to it on receipt, and a scan is what makes the radio go looking, not
     * what makes it allowed to remember.
     */
    if ((subtype != 0x0080u && subtype != 0x0050u) || frame_len < 36u) return;
    wifi_bss_profile profile;
    zero_bytes((uint8_t*)&profile, sizeof(profile));
    copy_bytes(profile.bssid, frame + 16u, 6u);
    profile.beacon_interval = read_le16(frame + 32u);
    profile.capability = read_le16(frame + 34u);
    profile.channel = packet[10];
    profile.rcpi = packet[9];
    if (!parse_bss_ies(frame + 36u, frame_len - 36u, &profile)) return;
    const int rssi = ((int)packet[9] / 2) - 110;
    store_scan_result(&profile, rssi);
}

static void process_legacy_scan_result(const uint8_t* payload, uint32_t payload_len)
{
    if (!g_state.scan_active || payload_len < 58u) return;
    wifi_bss_profile profile;
    zero_bytes((uint8_t*)&profile, sizeof(profile));
    copy_bytes(profile.bssid, payload + 22u, 6u);
    profile.capability = read_le16(payload + 54u);
    const uint32_t freq_khz = read_le32(payload + 8u);
    if (freq_khz >= 2412000u && freq_khz <= 2484000u)
        profile.channel = freq_khz == 2484000u ? 14u : (uint8_t)((freq_khz - 2407000u) / 5000u);
    if (!parse_bss_ies(payload + 56u, payload_len - 56u, &profile)) return;
    store_scan_result(&profile, (int32_t)read_le32(payload + 0u));
}

static void enqueue_ethernet_frame(const uint8_t* frame, uint32_t len)
{
    if (!frame || len < 14u || len > MAX_ETHERNET_FRAME_SIZE) return;
    const uint8_t next = (uint8_t)((g_rx_ethernet_head + 1u) % RX_ETHERNET_QUEUE_DEPTH);
    if (next == g_rx_ethernet_tail)
        g_rx_ethernet_tail = (uint8_t)((g_rx_ethernet_tail + 1u) % RX_ETHERNET_QUEUE_DEPTH);
    copy_bytes(g_rx_ethernet[g_rx_ethernet_head], frame, len);
    g_rx_ethernet_len[g_rx_ethernet_head] = (uint16_t)len;
    g_rx_ethernet_head = next;
}

static int secure_equal(const uint8_t* a, const uint8_t* b, uint32_t size)
{
    uint8_t diff = 0u;
    for (uint32_t i = 0; i < size; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0u;
}

static int send_eapol_key_response(uint8_t eapol_version,
                                   uint8_t descriptor_type,
                                   uint16_t key_info,
                                   uint16_t key_length,
                                   uint64_t replay_counter,
                                   const uint8_t* nonce,
                                   const uint8_t* key_data,
                                   uint16_t key_data_len)
{
    uint8_t frame[256];
    const uint16_t key_body_len = (uint16_t)(95u + key_data_len);
    const uint32_t frame_len = 14u + 4u + key_body_len;
    if (frame_len > sizeof(frame)) return -1;
    zero_bytes(frame, sizeof(frame));
    copy_bytes(frame + 0u, g_assoc_profile.bssid, 6u);
    copy_bytes(frame + 6u, g_wifi_mac, 6u);
    frame[12] = 0x88u;
    frame[13] = 0x8eu;
    uint8_t* eapol = frame + 14u;
    eapol[0] = eapol_version ? eapol_version : 2u;
    eapol[1] = 3u;
    write_be16(eapol + 2u, key_body_len);
    uint8_t* key = eapol + 4u;
    key[0] = descriptor_type;
    write_be16(key + 1u, key_info);
    write_be16(key + 3u, key_length);
    write_be64(key + 5u, replay_counter);
    if (nonce) copy_bytes(key + 13u, nonce, 32u);
    write_be16(key + 93u, key_data_len);
    if (key_data && key_data_len) copy_bytes(key + 95u, key_data, key_data_len);

    uint8_t mic[20];
    mt6592_wifi_hmac_sha1(g_ptk, 16u, eapol, 4u + key_body_len, mic);
    copy_bytes(key + 77u, mic, 16u);
    return send_hif_frame(frame, frame_len, 0u, g_sta_rec_index, 0, 1, 0);
}

static int verify_eapol_mic(const uint8_t* eapol, uint32_t eapol_len, const uint8_t expected[16])
{
    uint8_t copy[256];
    uint8_t mic[20];
    if (!eapol || eapol_len > sizeof(copy) || eapol_len < 4u + 95u) return 0;
    copy_bytes(copy, eapol, eapol_len);
    zero_bytes(copy + 4u + 77u, 16u);
    mt6592_wifi_hmac_sha1(g_ptk, 16u, copy, eapol_len, mic);
    return secure_equal(mic, expected, 16u);
}

static int install_gtk_from_key_data(const uint8_t* data, uint32_t len)
{
    static const uint8_t broadcast[6] = {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu};
    uint32_t offset = 0u;
    while (offset + 2u <= len)
    {
        const uint8_t id = data[offset];
        const uint8_t ie_len = data[offset + 1u];
        offset += 2u;
        if (offset + ie_len > len) break;
        if (id == 0xddu && ie_len >= 22u && data[offset] == 0x00u && data[offset + 1u] == 0x0fu &&
            data[offset + 2u] == 0xacu && data[offset + 3u] == 0x01u)
        {
            const uint8_t key_id = data[offset + 4u] & 0x03u;
            const uint32_t gtk_len = ie_len - 6u;
            if (gtk_len >= 16u && install_ccmp_key(broadcast, key_id, 0, data + offset + 6u) == 0) return 0;
        }
        offset += ie_len;
        while (offset < len && data[offset] == 0u) ++offset;
    }
    return -1;
}

/*
 * The half of the four-way handshake that must not run until message 4 is on
 * the air: install the pairwise key, then the group key, then tell the firmware
 * the BSS is connected. Driven either by the TX-status event that names message
 * 4 or, if that never arrives, by the deadline in check_m4_tx_done().
 */
static void complete_four_way(void)
{
    g_m4_tx_seq = 0u;
    if (install_ccmp_key(g_assoc_profile.bssid, 0u, 1, g_ptk + 32u) != 0)
    {
        set_assoc_failure("MediaTek firmware rejected the WPA2 pairwise key", "mt6592-wifi:ptk-install-failed");
        return;
    }
    if (install_gtk_from_key_data(g_m4_gtk, g_m4_gtk_len) != 0)
    {
        set_assoc_failure("MediaTek WPA2 group key could not be installed", "mt6592-wifi:gtk-install-failed");
        return;
    }
    (void)send_bss_info(1);
    association_completed(1);
    g_state.status = "MediaTek Wi-Fi WPA2-PSK/CCMP link ready; Ethernet data path online";
}

/*
 * Called from the poll loop. If the firmware never reports message 4 as
 * transmitted, install anyway rather than hanging the join: that is no worse
 * than the behaviour this replaced, and the distinct status line says which of
 * the two paths completed the handshake -- which is also the measurement that
 * says whether the TX-status event is reported for 1X frames at all.
 */
static void check_m4_tx_done(void)
{
    if (g_m4_tx_seq == 0u) return;
    if (mt6592_timer_microseconds() < g_m4_deadline_us) return;
    ++g_m4_tx_done_timeouts;
    complete_four_way();
    if (g_state.associated)
        g_state.status = "MediaTek Wi-Fi WPA2-PSK/CCMP link ready; message 4 was never confirmed transmitted";
}

static int process_eapol_key(const uint8_t* frame, uint32_t frame_len)
{
    if (frame_len < 14u + 4u + 95u || frame[12] != 0x88u || frame[13] != 0x8eu) return 0;
    const uint8_t* eapol = frame + 14u;
    if (eapol[1] != 3u) return 0;
    const uint16_t body_len = read_be16(eapol + 2u);
    if ((uint32_t)body_len + 18u > frame_len || body_len < 95u) return 1;
    const uint8_t* key = eapol + 4u;
    const uint16_t key_info = read_be16(key + 1u);
    const uint16_t descriptor_version = key_info & 0x7u;
    const uint64_t replay = read_be64(key + 5u);
    const int pairwise = (key_info & BIT(3)) != 0u;
    const int install = (key_info & BIT(6)) != 0u;
    const int ack = (key_info & BIT(7)) != 0u;
    const int mic_set = (key_info & BIT(8)) != 0u;
    const int secure = (key_info & BIT(9)) != 0u;
    const int encrypted_data = (key_info & BIT(12)) != 0u;
    if (descriptor_version != 2u || !ack) return 1;

    if (!pairwise && g_assoc_phase == ASSOC_CONNECTED && mic_set && secure)
    {
        if (replay <= g_eapol_replay_counter || !verify_eapol_mic(eapol, 4u + body_len, key + 77u)) return 1;
        const uint16_t key_data_len = read_be16(key + 93u);
        if (95u + key_data_len > body_len) return 1;
        uint8_t plain[192];
        int plain_len = -1;
        if (encrypted_data)
        {
            plain_len = mt6592_wifi_aes_unwrap(g_ptk + 16u, key + 95u, key_data_len, plain, sizeof(plain));
        }
        else if (key_data_len <= sizeof(plain))
        {
            copy_bytes(plain, key + 95u, key_data_len);
            plain_len = (int)key_data_len;
        }
        if (plain_len >= 0 && install_gtk_from_key_data(plain, (uint32_t)plain_len) == 0)
        {
            g_eapol_replay_counter = replay;
            const uint16_t response_info = (uint16_t)(descriptor_version | BIT(8) | BIT(9));
            (void)send_eapol_key_response(eapol[0], key[0], response_info, read_be16(key + 3u), replay,
                                          0, 0, 0u);
            g_state.status = "MediaTek Wi-Fi WPA2 group key refreshed";
        }
        return 1;
    }
    if (!pairwise) return 1;

    /*
     * Message 1, including every retransmission of it.
     *
     * The ASSOC_WAIT_EAPOL_M3 arm used to be a separate block that only replied
     * when `replay == g_eapol_replay_counter'. That can never match: hostapd's
     * wpa_send_eapol() bumps the replay counter on every transmission, retries
     * included, so a retried M1 always arrives with a strictly higher counter
     * and the old guard dropped it silently. Same defect, same cause, as the M3
     * retransmission guard below.
     *
     * Deriving the PTK afresh on each M1 rather than short-circuiting the retry
     * is what wpa_supplicant does (wpa_supplicant_process_1_of_4 runs the full
     * derivation unconditionally), and it is the only version that is also
     * correct when the AP gives up and restarts the handshake with a new ANonce.
     * Safe here because the PTK is not installed until M3 is accepted.
     */
    if ((g_assoc_phase == ASSOC_WAIT_EAPOL_M1 ||
         (g_assoc_phase == ASSOC_WAIT_EAPOL_M3 && replay >= g_eapol_replay_counter)) && !mic_set)
    {
        copy_bytes(g_anonce, key + 13u, sizeof(g_anonce));
        g_eapol_replay_counter = replay;
        mt6592_wifi_wpa_prf_512(g_pmk, g_wifi_mac, g_assoc_profile.bssid, g_snonce, g_anonce, g_ptk);
        uint8_t rsn[24];
        const uint32_t rsn_len = append_assoc_rsn(rsn, sizeof(rsn));
        const uint16_t response_info = (uint16_t)(descriptor_version | BIT(3) | BIT(8));
        if (send_eapol_key_response(eapol[0], key[0], response_info, read_be16(key + 3u), replay,
                                    g_snonce, rsn, (uint16_t)rsn_len) != 0)
        {
            set_assoc_failure("MediaTek WPA2 message 2 could not be transmitted", "mt6592-wifi:eapol-m2-submit-failed");
            return 1;
        }
        g_assoc_phase = ASSOC_WAIT_EAPOL_M3;
        g_assoc_deadline_us = mt6592_timer_microseconds() + EAPOL_TIMEOUT_US;
        g_state.status = "MediaTek WPA2 message 2 sent; waiting for pairwise/group keys";
        return 1;
    }

    if (g_assoc_phase == ASSOC_WAIT_EAPOL_M3 && mic_set && install && secure)
    {
        if (replay < g_eapol_replay_counter || !verify_eapol_mic(eapol, 4u + body_len, key + 77u))
        {
            set_assoc_failure("MediaTek WPA2 message 3 MIC/replay validation failed", "mt6592-wifi:eapol-m3-invalid");
            return 1;
        }
        g_eapol_replay_counter = replay;

        /*
         * Validate all of message 3 before installing any of it. Both the MIC
         * checked above and the unwrap below use keys this driver derived
         * itself -- the KCK at g_ptk+0 and the KEK at g_ptk+16 -- so neither
         * needs the firmware to be holding a key, and the whole message can be
         * proven good while the hardware is still in the clear.
         */
        const uint16_t key_data_len = read_be16(key + 93u);
        if (95u + key_data_len > body_len)
        {
            set_assoc_failure("MediaTek WPA2 message 3 key data was truncated", "mt6592-wifi:eapol-key-data-truncated");
            return 1;
        }
        uint8_t plain[192];
        int plain_len;
        if (encrypted_data)
            plain_len = mt6592_wifi_aes_unwrap(g_ptk + 16u, key + 95u, key_data_len, plain, sizeof(plain));
        else
        {
            if (key_data_len > sizeof(plain)) plain_len = -1;
            else
            {
                copy_bytes(plain, key + 95u, key_data_len);
                plain_len = key_data_len;
            }
        }
        if (plain_len < 0)
        {
            set_assoc_failure("MediaTek WPA2 message 3 key data could not be decrypted",
                              "mt6592-wifi:eapol-key-data-undecryptable");
            return 1;
        }

        /*
         * Message 4 goes out BEFORE either key is installed, and that ordering
         * is the whole handshake.
         *
         * The authenticator installs its pairwise key when message 4 arrives,
         * not when message 3 leaves -- so for the duration of message 4 the AP
         * still has no PTK and can only read plaintext. Installing ours first
         * makes the firmware encrypt message 4 with a key the other end has not
         * got, the AP sees nothing it can decrypt, retransmits message 3 until
         * it gives up, and deauthenticates with reason 15, 4-Way Handshake
         * Timeout. That is exactly what this board was getting, and the fault
         * was invisible from here because every step on our side had succeeded:
         * M1 arrived, M2 was accepted (the AP would not have sent M3 otherwise,
         * which is also what proves the PTK derivation and MIC are right), M3
         * verified, and the driver reported a healthy link to an AP that had
         * already stopped believing in it.
         *
         * IEEE 802.11-2016 12.7.6.4 puts the send first, and OpenBSD's
         * supplicant does too -- ieee80211_pae_input.c sends msg 4 and only then
         * installs the PTK, with "..authenticator will retry" on the failure
         * path. Install order after that is pairwise then group.
         *
         * "Before" has to mean transmitted, not written. Sending first and
         * installing immediately afterwards was already tried and the board
         * still lost the link: on the first join after boot TC4 has no credits
         * yet, message 4 sits in the queue, and the firmware executes the key
         * command that follows it -- tx_fragments counted three frames for a
         * handshake that sent four. The second join, with credits already warm,
         * transmitted all four and stayed up. So the install now waits for the
         * TX-status event that names this frame.
         */
        const uint16_t response_info = (uint16_t)(descriptor_version | BIT(3) | BIT(8) | BIT(9));
        if (send_eapol_key_response(eapol[0], key[0], response_info, read_be16(key + 3u), replay,
                                    0, 0, 0u) != 0)
        {
            set_assoc_failure("MediaTek WPA2 message 4 could not be transmitted", "mt6592-wifi:eapol-m4-submit-failed");
            return 1;
        }

        if ((uint32_t)plain_len > sizeof(g_m4_gtk))
        {
            set_assoc_failure("MediaTek WPA2 group key was larger than the driver's buffer",
                              "mt6592-wifi:gtk-too-large");
            return 1;
        }
        copy_bytes(g_m4_gtk, plain, (uint32_t)plain_len);
        g_m4_gtk_len    = (uint32_t)plain_len;
        g_m4_tx_seq     = g_last_tx_sequence;
        g_m4_deadline_us = mt6592_timer_microseconds() + M4_TX_DONE_TIMEOUT_US;
        if (g_m4_tx_seq == 0u)
        {
            /* No tag to wait on, so there is nothing to wait for. Complete the
             * way this used to, rather than stalling on an event that can never
             * be matched. */
            complete_four_way();
            return 1;
        }
        g_state.status = "MediaTek WPA2 message 4 sent; waiting for the firmware to transmit it";
        return 1;
    }

    /*
     * M3 retransmitted after the handshake completed: the AP never saw our M4.
     * Answer it -- with the counter it actually carries, not ours.
     *
     * This guard used to demand `replay == g_eapol_replay_counter', and that is
     * why the link kept dying with reason 15. hostapd increments the replay
     * counter on every transmission including retries (wpa_send_eapol), so a
     * retried M3 always carries a counter one or more above the M3 we accepted,
     * and `==' rejected all four of them. The full log shows it exactly: six
     * EAPOL frames in, two out. It keeps a ring of the last
     * RSNA_MAX_EAPOL_RETRIES counters and accepts an M4 echoing any of them, so
     * a late reply is still a good reply -- one lost M4 should cost a round
     * trip, not the association.
     *
     * MIC-verified against the installed PTK before anything is sent, so a
     * forged or genuinely stale M3 still cannot draw a response.
     */
    if (g_assoc_phase == ASSOC_CONNECTED && mic_set && install && secure &&
        replay >= g_eapol_replay_counter && verify_eapol_mic(eapol, 4u + body_len, key + 77u))
    {
        g_eapol_replay_counter = replay;
        const uint16_t response_info = (uint16_t)(descriptor_version | BIT(3) | BIT(8) | BIT(9));
        (void)send_eapol_key_response(eapol[0], key[0], response_info, read_be16(key + 3u), replay,
                                      0, 0, 0u);
    }
    return 1;
}

static void process_ethernet_frame(const uint8_t* frame, uint32_t frame_len)
{
    if (!frame || frame_len < 14u) return;
    if (frame[12] == 0x88u && frame[13] == 0x8eu && (g_state.auth_active || g_state.associated))
    {
        (void)process_eapol_key(frame, frame_len);
        return;
    }
    if (g_state.data_path_ready) enqueue_ethernet_frame(frame, frame_len);
}

static void process_data_packet(const uint8_t* packet, uint32_t length)
{
    if (length < HIF_RX_HEADER_SIZE) return;
    uint32_t packet_len = read_le16(packet + 0u);
    const uint32_t offset = packet[4] & 0x3u;
    if (packet_len > length) packet_len = length;
    if (HIF_RX_HEADER_SIZE + offset >= packet_len) return;
    const uint8_t* payload = packet + HIF_RX_HEADER_SIZE + offset;
    uint32_t payload_len = packet_len - HIF_RX_HEADER_SIZE - offset;
    ++g_state.rx_data;

    if ((packet[5] & BIT(0)) == 0u)
    {
        process_ethernet_frame(payload, payload_len);
        return;
    }

    uint32_t header_len = (packet[4] >> 2u) & 0x3fu;
    if (header_len < 24u) header_len = 24u;
    if (payload_len < header_len + 8u) return;
    const uint16_t fc = read_le16(payload);
    const int to_ds = (fc & 0x0100u) != 0u;
    const int from_ds = (fc & 0x0200u) != 0u;
    if (to_ds && from_ds) return;
    const uint8_t* llc = payload + header_len;
    if (llc[0] != 0xaau || llc[1] != 0xaau || llc[2] != 0x03u) return;
    const uint32_t body_len = payload_len - header_len - 8u;
    if (body_len + 14u > MAX_ETHERNET_FRAME_SIZE) return;
    uint8_t ethernet[MAX_ETHERNET_FRAME_SIZE];
    const uint8_t* dest = from_ds ? payload + 4u : (to_ds ? payload + 16u : payload + 4u);
    const uint8_t* src = from_ds ? payload + 16u : payload + 10u;
    copy_bytes(ethernet, dest, 6u);
    copy_bytes(ethernet + 6u, src, 6u);
    ethernet[12] = llc[6];
    ethernet[13] = llc[7];
    copy_bytes(ethernet + 14u, llc + 8u, body_len);
    process_ethernet_frame(ethernet, body_len + 14u);
}

static void association_timeout_pump(void)
{
    if (g_assoc_phase == ASSOC_IDLE || g_assoc_phase == ASSOC_CONNECTED || g_assoc_phase == ASSOC_FAILED) return;
    if (mt6592_timer_microseconds() < g_assoc_deadline_us) return;
    if (g_assoc_phase == ASSOC_WAIT_CHANNEL && g_assoc_retries++ < 2u && send_channel_request() == 0)
    {
        g_assoc_deadline_us = mt6592_timer_microseconds() + CHANNEL_TIMEOUT_US;
        return;
    }
    if (g_assoc_phase == ASSOC_WAIT_AUTH && g_assoc_retries++ < 2u && send_auth_request() == 0)
    {
        g_assoc_deadline_us = mt6592_timer_microseconds() + AUTH_TIMEOUT_US;
        return;
    }
    if (g_assoc_phase == ASSOC_WAIT_ASSOC && g_assoc_retries++ < 2u && send_assoc_request() == 0)
    {
        g_assoc_deadline_us = mt6592_timer_microseconds() + ASSOC_TIMEOUT_US;
        return;
    }
    if (g_assoc_phase == ASSOC_WAIT_EAPOL_M1 || g_assoc_phase == ASSOC_WAIT_EAPOL_M3)
        set_assoc_failure("MediaTek WPA2 four-way handshake timed out", "mt6592-wifi:eapol-timeout");
    else
        set_assoc_failure("MediaTek Wi-Fi authentication/association timed out", "mt6592-wifi:association-timeout");
}

static void process_event_packet(const uint8_t* packet, uint32_t length)
{
    if (length < 8u)
    {
        ++g_state.dropped_packets;
        return;
    }

    const uint8_t event_id     = packet[4];
    const uint8_t* payload     = packet + 8u;
    const uint32_t payload_len = length - 8u;
    ++g_state.rx_events;
    /* Keep the first few IDs rather than the last: what a scan does at the
     * start is what is in question, and a late flood must not push it out. */
    if (g_state.event_ids_used < (uint32_t)(sizeof(g_state.event_ids)))
    {
        g_state.event_ids[g_state.event_ids_used++] = event_id;
    }

    /*
     * EVENT_TX_DONE_T (include/nic_cmd_event.h:1290-1297): ucPacketSeq at 0,
     * ucStatus at 1, u2SequenceNumber at 2. The tag is the ucPacketSeqNo this
     * driver wrote into the HIF TX header, so it names one frame exactly.
     *
     * Only message 4 is waited on. Status is deliberately ignored: a failed
     * transmission still means the frame is out of the queue, which is the only
     * thing the key install has to be ordered against, and treating a bad status
     * as a reason to stall would hang the join on the one case where the AP is
     * about to retransmit message 3 anyway.
     */
    if (event_id == EVENT_ID_TX_DONE && payload_len >= 1u && g_m4_tx_seq != 0u &&
        payload[0] == g_m4_tx_seq)
    {
        complete_four_way();
        return;
    }

    if (event_id == EVENT_ID_SCAN_DONE && payload_len >= 1u)
    {
        /* Recorded before the match, because the interesting failure is the one
         * where SCAN_DONE did arrive and this test threw it away: a sequence
         * mismatch would leave the scan running to its 12 s timeout with the
         * proof of completion already in hand and discarded. */
        g_state.scan_done_seq     = payload[0];
        g_state.scan_done_seq_want = g_scan_sequence;
        ++g_state.scan_done_events;
        /* The rest of the event, recorded on the same terms: whatever arrived,
         * before anything decides whether to believe it. See the field comments
         * in the header -- payload[1..3] are the firmware's own account of the
         * sweep, and they are the difference between "listened, heard nothing"
         * and "never listened". */
        g_state.scan_done_payload_len   = payload_len;
        g_state.scan_done_sparse_valid  = (payload_len >= 2u) ? payload[1] : 0u;
        g_state.scan_done_sparse_band   = (payload_len >= 3u) ? payload[2] : 0u;
        g_state.scan_done_sparse_channel= (payload_len >= 4u) ? payload[3] : 0u;
        {
            uint32_t i;
            const uint32_t n = payload_len < 8u ? payload_len : 8u;
            for (i = 0u; i < 8u; ++i) g_state.scan_done_raw[i] = (i < n) ? payload[i] : 0u;
        }
        if (g_state.scan_active && payload[0] == g_scan_sequence)
        {
            g_state.scan_elapsed_ms = (uint32_t)((mt6592_timer_microseconds() - g_scan_started_us) / 1000u);
            g_state.scan_end_reason = SCAN_END_DONE;
            g_state.scan_active = 0;
            g_state.status  = g_state.scan_result_count ? "MediaTek Wi-Fi scan complete; nearby networks discovered"
                                                        : "MediaTek Wi-Fi scan complete; no beacon responses received";
            g_state.blocked = 0;
        }
    }
    else if (event_id == EVENT_ID_SCAN_RESULT)
    {
        process_legacy_scan_result(payload, payload_len);
    }
    else if (event_id == EVENT_ID_CH_PRIVILEGE && payload_len >= 12u)
    {
        if (g_assoc_phase == ASSOC_WAIT_CHANNEL && payload[0] == NETWORK_TYPE_AIS &&
            payload[1] == g_channel_token && payload[2] == 0u)
        {
            /*
             * Station record first, then the authentication frame -- stock's own
             * order, and both halves of aisFsmStateInit_JOIN (0xc0411c70): it
             * calls bssCreateStaRecFromBssDesc with eStaType 0x41, then
             * cnmStaRecChangeState with the record's state still 0, and only then
             * posts the message that makes saaFsmSteps transmit AUTH.
             *
             * State 0 is STA_STATE_1: cnmStaRecChangeState's own dispatch accepts
             * 0, 1 and 2 (0xc041f008..0xc041f01c), and the join-init call site at
             * 0xc0411cb0 reaches it precisely when the fresh record's eStaState is
             * still zero. So the three states this driver already sends -- 0 at
             * join, 1 after authentication, 2 after association -- are the right
             * three numbers. Only the command they travelled under was wrong.
             */
            if (send_sta_record(STA_STATE_1) != 0 || send_auth_request() != 0)
            {
                set_assoc_failure("MediaTek authentication frame could not be submitted",
                                  "mt6592-wifi:auth-submit-failed");
            }
            else
            {
                g_assoc_phase = ASSOC_WAIT_AUTH;
                g_assoc_retries = 0u;
                g_assoc_deadline_us = mt6592_timer_microseconds() + AUTH_TIMEOUT_US;
                g_state.status = "MediaTek Wi-Fi channel granted; authenticating";
                g_state.blocked = 0;
            }
        }
    }
    else if (event_id == EVENT_ID_ACTIVATE_STA_REC && payload_len >= 8u)
    {
        /*
         * qmActivateStaRec, gated exactly as stock gates it. cnmStaRecHandleEventPkt
         * (mgmt/cnm_mem.c:1234-1252) does not take the firmware's word for which
         * record this is -- it looks the index up, then checks the state and the
         * MAC before activating:
         *
         *     prStaRec = cnmGetStaRecByIndex(prAdapter, prEventContent->ucStaRecIdx);
         *     if (prStaRec && prStaRec->ucStaState == STA_STATE_3 &&
         *         !kalMemCmp(&prStaRec->aucMacAddr[0], &prEventContent->aucMacAddr[0],
         *                     MAC_ADDR_LEN)) {
         *         qmActivateStaRec(prAdapter, prStaRec);
         *     }
         *
         * "ucStaState == STA_STATE_3" is g_state.associated here, since STATE_3 is
         * what the association response sends and nothing else sets it.
         */
        if (payload[6] == STA_RECORD_INDEX && g_state.associated &&
            same_mac(payload, g_assoc_profile.bssid))
        {
            g_sta_rec_index = STA_RECORD_INDEX;
        }
    }
    else if (event_id == EVENT_ID_BSS_BEACON_TIMEOUT && g_state.associated)
    {
        set_assoc_failure("MediaTek Wi-Fi beacon timeout", "mt6592-wifi:beacon-timeout");
    }
    else if (event_id == EVENT_ID_CMD_RESULT)
    {
        /* Set commands are normally fire-and-forget; consume any result event. */
    }
}

static void process_rx_packet(const uint8_t* packet, uint32_t length)
{
    uint16_t packet_type;

    ++g_state.rx_packets;
    if (length < 4u)
    {
        ++g_state.dropped_packets;
        return;
    }

    packet_type = read_le16(packet + 2u) & HIF_RX_PACKET_TYPE_MASK;
    if (packet_type == HIF_RX_PACKET_TYPE_EVENT)
    {
        process_event_packet(packet, length);
    }
    else if (packet_type == HIF_RX_PACKET_TYPE_MANAGEMENT)
    {
        ++g_state.rx_management;
        process_management_packet(packet, length);
    }
    else if (packet_type == HIF_RX_PACKET_TYPE_DATA)
    {
        process_data_packet(packet, length);
    }
}

/*
 * CID 130, QUERY, empty body -- the firmware's own MAC counters.
 *
 * Matching is on the SEQUENCE BYTE ALONE, not on an event ID, and that is
 * deliberate. Stock does the same: nicRxProcessEventPacket (0xc03e223c) jumps
 * through a 254-entry table indexed by EID-1, and 220 of those entries -- the
 * statistics answer among them -- land on one shared default at 0xc03e2684 that
 * finds the outstanding command by its sequence number and calls that command's
 * done handler. The event ID of a query response is a firmware detail nobody
 * here has a table for (CMD 0xc1 answers with EID 0x09, so it is not even the
 * CID), and inventing one would only be a way to reject the right packet.
 * Whatever ID does arrive is recorded instead, which is how the table gets
 * filled in for free.
 *
 * Anything else that turns up while waiting is handed to the normal RX path
 * rather than dropped -- a beacon arriving during this window is exactly the
 * event we have spent the session looking for, and discarding it to keep the
 * loop simple would be discarding the answer.
 */
int mt6592_wifi_hif_query_statistics(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_alive)
    {
        return -1;
    }
    if (acquire_driver_own() != 0)
    {
        ++g_state.driver_own_lost;
        return -1;
    }

    {
        uint8_t* packet            = tx_buffer();
        const uint8_t sequence     = next_command_sequence();
        const uint32_t packet_size = align4(NORMAL_COMMAND_HEADER_SIZE);

        g_state.stats_valid       = 0u;
        g_state.stats_event_id    = 0u;
        g_state.stats_payload_len = 0u;
        g_state.stats_polls       = 0u;
        for (uint32_t i = 0u; i < 12u; ++i) { g_state.stats_lo[i] = 0u; g_state.stats_hi[i] = 0u; }

        zero_bytes(packet, packet_size);
        write_le16(packet + 0u, (uint16_t)packet_size);
        packet[3] = (uint8_t)((HIF_TX_COMMAND_RESOURCE << HIF_TX_RESOURCE_SHIFT) |
                              (HIF_TX_PACKET_TYPE_CMD << HIF_TX_PACKET_TYPE_SHIFT));
        packet[4] = CMD_ID_GET_STATISTICS;
        packet[5] = 0u; /* query, not set */
        packet[6] = sequence;
        if (tx_resource_acquire(HIF_TX_COMMAND_RESOURCE) != 0) return -1;
        write_port(MCR_WTDR1, HIF_TARGET_TXD1, packet, packet_size);

        const uint64_t deadline = mt6592_timer_microseconds() + INIT_ACK_TIMEOUT_US;
        uint32_t polls          = 0u;
        while (mt6592_timer_microseconds() < deadline)
        {
            uint8_t port;
            uint32_t length;
            if (!read_next_packet(&port, &length))
            {
                delay_us(100u);
                if ((++polls & 0x3fu) == 0u) wifi_cooperative_yield();
                continue;
            }
            if (receive_packet(port, length) != 0 || length < 8u)
            {
                ++g_state.dropped_packets;
                continue;
            }

            const uint8_t* event = rx_buffer();
            if ((read_le16(event + 2u) & HIF_RX_PACKET_TYPE_MASK) != HIF_RX_PACKET_TYPE_EVENT ||
                event[5] != sequence)
            {
                process_rx_packet(rx_buffer(), length);
                continue;
            }

            g_state.stats_event_id    = event[4];
            g_state.stats_payload_len = (length > 8u) ? (length - 8u) : 0u;
            g_state.stats_polls       = polls;
            {
                const uint8_t* payload = event + 8u;
                for (uint32_t i = 0u; i < 12u; ++i)
                {
                    const uint32_t off = i * 8u;
                    if (off + 8u > g_state.stats_payload_len) break;
                    g_state.stats_lo[i] = read_le32(payload + off);
                    g_state.stats_hi[i] = read_le32(payload + off + 4u);
                }
            }
            g_state.stats_valid = 1u;
            return 0;
        }
        g_state.stats_polls = polls;
    }
    return -1;
#else
    return -1;
#endif
}

/*
 * CID 194 -- read and write a firmware-side register, through the firmware.
 *
 * WHY THIS EXISTS. Once WIFI_START runs, `wifi rd` and `wifi dump` stop working:
 * they go through the boot ROM's register window, and the handoff takes that
 * window away. From that moment the running firmware is opaque, which is exactly
 * the wrong time to lose visibility -- the scan problem lives entirely after
 * start, in a receiver that demodulates (rx_fcs_errors climbs) but never lands a
 * frame (rx_fragments stays 0). MCR is the one path into the live chip that
 * survives, because the firmware itself services it.
 *
 * PAYLOAD, read off wlanoidQueryMcrRead (~0xc03cc8b0): `mov r1, #194` is the
 * CID, r2 selects 0 QUERY / 1 SET, r3 = 1 fgNeedResp, and the stack arguments
 * are fgIsOid = 1, done handler 0xc03eb248, timeout handler 0xc03ed46c, length
 * 8, buffer {u4McrOffset, u4McrData}. The error path's `mov r3,#8; str r3,[r5]`
 * confirms the 8: LE32 address then LE32 data, both directions.
 *
 * A WRITE IS A WRITE. This pokes a live radio; a wrong address can wedge the
 * firmware and cost a power cycle. It is a debugging instrument, deliberately
 * not called from any driver path.
 */
int mt6592_wifi_hif_mcr(uint32_t address, const uint32_t* value_in, uint32_t* value_out)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_alive)
    {
        return -1;
    }
    if (acquire_driver_own() != 0)
    {
        ++g_state.driver_own_lost;
        return -1;
    }

    {
        uint8_t* packet            = tx_buffer();
        const uint8_t sequence     = next_command_sequence();
        const uint32_t packet_size = align4(NORMAL_COMMAND_HEADER_SIZE + MCR_PAYLOAD_SIZE);

        zero_bytes(packet, packet_size);
        write_le16(packet + 0u, (uint16_t)packet_size);
        packet[3] = (uint8_t)((HIF_TX_COMMAND_RESOURCE << HIF_TX_RESOURCE_SHIFT) |
                              (HIF_TX_PACKET_TYPE_CMD << HIF_TX_PACKET_TYPE_SHIFT));
        packet[4] = CMD_ID_ACCESS_REG;
        packet[5] = (value_in != 0) ? 1u : 0u; /* set : query */
        packet[6] = sequence;
        write_le32(packet + NORMAL_COMMAND_HEADER_SIZE + 0u, address);
        write_le32(packet + NORMAL_COMMAND_HEADER_SIZE + 4u,
                   (value_in != 0) ? *value_in : 0u);

        if (tx_resource_acquire(HIF_TX_COMMAND_RESOURCE) != 0) return -1;
        write_port(MCR_WTDR1, HIF_TARGET_TXD1, packet, packet_size);

        /* A write still asks for a response -- stock passes fgNeedResp = 1 for
         * both directions -- so both cases wait, and a write that is never
         * acknowledged is reported as a failure rather than as success. */
        const uint64_t deadline = mt6592_timer_microseconds() + INIT_ACK_TIMEOUT_US;
        uint32_t polls          = 0u;
        while (mt6592_timer_microseconds() < deadline)
        {
            uint8_t port;
            uint32_t length;
            if (!read_next_packet(&port, &length))
            {
                delay_us(100u);
                if ((++polls & 0x3fu) == 0u) wifi_cooperative_yield();
                continue;
            }
            if (receive_packet(port, length) != 0 || length < 8u)
            {
                ++g_state.dropped_packets;
                continue;
            }

            const uint8_t* event = rx_buffer();
            /* Sequence byte alone, for the reason spelled out over
             * mt6592_wifi_hif_query_statistics(): the event ID of a query
             * response is a firmware detail we have no table for, and anything
             * that is not ours -- a beacon above all -- belongs in the RX path,
             * not in the bin. */
            if ((read_le16(event + 2u) & HIF_RX_PACKET_TYPE_MASK) != HIF_RX_PACKET_TYPE_EVENT ||
                event[5] != sequence)
            {
                process_rx_packet(rx_buffer(), length);
                continue;
            }

            if (value_out)
            {
                /* The response echoes the same {offset, data} pair, so the data
                 * is at payload+4. Short answers report zero rather than read
                 * off the end of the buffer. */
                *value_out = (length >= NORMAL_COMMAND_HEADER_SIZE + MCR_PAYLOAD_SIZE)
                                 ? read_le32(event + NORMAL_COMMAND_HEADER_SIZE + 4u)
                                 : 0u;
            }
            return 0;
        }
    }
    return -1;
#else
    (void)address;
    (void)value_in;
    (void)value_out;
    return -1;
#endif
}

int mt6592_wifi_hif_bind(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (g_state.hif_ready)
    {
        return acquire_driver_own();
    }

    mt6592_uart_puts("  wifi: binding CONSYS WLAN AHB HIF @ 0x180f0000\n");

    /*
     * The WiFi TX PA rail, raised here and never lowered, because this function
     * is HifAhbProbe() and that is what HifAhbProbe() does first:
     *
     *     /-* power on WiFi TX PA 3.3V and HIF GDMA clock *-/
     *     hwPowerOn(MT6323_POWER_LDO_VCN33_WIFI, VOL_3300, "WLAN");
     *     upmu_set_vcn33_on_ctrl_wifi(1);   /-* switch to HW mode *-/
     *     ... then pfWlanProbe(), i.e. the whole download / WIFI_START /
     *         WLAN_READY sequence, with the rail already up
     *
     * (ahb.c:1823-1826; the matching drop is in HifAhbRemove at :1868-1871,
     * after pfWlanRemove.) So on stock the rail is up for the entire life of the
     * WLAN driver, not just for a moment.
     *
     * This driver used to believe otherwise. The note in mt6592_wifi_sdio.c
     * searched conn_soc for WIFI_PALDO, found only wmt_ic_soc.c:766/:779
     * bracketing the RF calibration script, and concluded that 3.3 V was a
     * calibration-time rail -- so mt6592_wifi_wmt.c raises it for the calibration
     * frame and drops it again. That reading was right about the WMT layer and
     * wrong about the chip: the WLAN driver raises the same regulator under its
     * own name, MT6323_POWER_LDO_VCN33_WIFI, which no WIFI_PALDO search finds.
     * Both are true at once and stock does both.
     *
     * What it cost: the firmware downloaded and started with its transmit PA
     * unpowered, ran, and reported failure rather than readiness -- WCIR bit 21
     * never set, and mailbox 0 (which wlan_lib.c:1596-1601 reads for exactly this
     * timeout) carrying ID 300.
     *
     * Failing to raise it is not fatal here. A board that cannot bring this rail
     * up has a PMIC problem the WLAN bring-up cannot fix and should not hide, and
     * the firmware's own error path is a better report than a refused bind.
     */
    if (mt6592_wifi_sdio_set_wifi_rail(1) != 0)
    {
        mt6592_uart_puts("  wifi: WARNING VCN33_WIFI would not come up; the TX PA is unpowered\n");
    }

    if (configure_hif() != 0)
    {
        return -1;
    }
    mt6592_uart_puts("  wifi: WLAN HIF chip id=");
    mt6592_uart_put_hex32(g_state.hif_chip_id);
    mt6592_uart_puts(" driver-owned\n");
    return 0;
#else
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
/*
 * Everything stock does between the last download chunk and a usable adapter:
 * INIT_CMD_WIFI_START at the image's own entry, then poll WCIR for WLAN_READY.
 * Split out of the loader only so the deferred-start diagnostic can reach it.
 */
/*
 * The boot ROM's global pointer, read out of its own reset code at ROM 0x26a:
 *
 *     0000026a:  47d02097  sethi r29, #0x2097     ; gp  = 0x02097000
 *     0000026e:  59de8324  ori   r29, r29, #0x324 ; gp |= 0x324
 *
 * (ORI's immediate is 15 bits, not 20 -- reading it as 0x8324 puts gp 0x8000
 * too high and every offset below lands in nothing.) Confirmed against the live
 * chip: word[gp-28128] reads 0x0006a000, the entry address the ROM defaults to,
 * and word[gp-28124] reads 1, the mode value that means "serving INIT commands".
 */
#define CONSYS_ROM_GP 0x02097324u

/*
 * Release the ROM's firmware handoff.
 *
 * WIFI_START does everything it should: the CID 2 arm at ROM 0xda60 stores the
 * entry address to word[gp-28128] and calls the mode setter at 0xd4a8 with 2.
 * Mode 2 is what makes the ROM run the image -- the dispatcher re-reads it at
 * 0xd81a and mode 2 falls through 0xd8ca to 0xdc16:
 *
 *     0000dc18:  bne    r0, r2(2), 0xdc46
 *     0000dc1e:  jal    0x15188                   ; prepare the hardware
 *     0000dc22:  lwi.gp r0, [gp + #-28128]        ; the entry address
 *     0000dc26:  jral5  r0                        ; run the firmware
 *
 * The catch is the path back to that re-read. WIFI_START also sets
 * byte[gp+4377] = 1 at 0xda9a, and the post-command tail only reaches 0xd81a
 * through 0xdc06:
 *
 *     0000dbd6:  lbi.gp r1, [gp + #4377]
 *     0000dbda:  beqz38 r1, 0xdc06                ; flag clear -> re-read mode
 *     0000dbdc:  lbi.gp r1, [gp + #-14007]
 *     0000dbe0:  beqz38 r1, 0xdbee                ; -> enqueue and go back to rx
 *     0000dbe2:  lbi.gp r1, [gp + #-17736]
 *     0000dbe6:  bnez38 r1, 0xdc06
 *     0000dbe8:  lbi.gp r1, [gp + #-14006]
 *     0000dbec:  bnez38 r1, 0xdc06
 *
 * so with all three of those bytes zero -- which is what the chip reads back on
 * our boot -- WIFI_START's own flag steers the tail to 0xdbee, which enqueues a
 * deferred item via 0x1a3c(item, 5) and returns to receive(). The ROM then sits
 * in that receive forever: only WIFI_START reaches 0xdbd6 at all (it is the one
 * command that leaves r8 non-zero, tested at 0xdbd2), so nothing later gets
 * another look at the mode. Mode 2 is set and never acted on.
 *
 * Setting byte[gp-14007] and byte[gp-14006] before WIFI_START takes the tail
 * down the 0xdbe8 -> 0xdc06 arm instead. Measured, on the same boot that had
 * never moved before: the connectivity PC went from a fixed 0x00066382
 * (`standby` in the WMT patch's idle) to 597 changes in 598 samples, was sampled
 * at 0x0006a0aa inside the downloaded section 0, and the device-to-host
 * mailboxes went from zero to 2c 01 49 4e / 49 54 00 00 -- 0x012c and "INIT".
 *
 * Both bytes live in one word, and that word reads back zero, so a single write
 * sets them without disturbing a neighbour.
 */
#define CONSYS_ROM_HANDOFF_GATE    (CONSYS_ROM_GP - 14008u) /* 0x02093c6c */
#define CONSYS_ROM_HANDOFF_RELEASE 0x00010100u              /* bytes +1 and +2 */

static void release_rom_handoff(void)
{
    uint8_t event[32];
    uint32_t event_len = 0u;

    /* A write is acknowledged in a different shape than a read, so rc > 0 is the
     * normal answer here and is not a failure -- see rom_access_reg(). */
    (void)mt6592_wifi_hif_rom_access_reg(1, CONSYS_ROM_HANDOFF_GATE, CONSYS_ROM_HANDOFF_RELEASE,
                                         event, sizeof event, &event_len);
}

/*
 * Declare the A-die RF front end probed, so the firmware will run.
 *
 * The chain, traced end to end and then confirmed on hardware:
 *
 *   1. Firmware 0xf004720c calls ROM table slot 0x14E9C -> 0x150d8 and ASSERTs
 *      at wifi/mgmt/mt6582/rlm_phy.c:4209 if it returns 0. That assert is what
 *      the EMI log showed on every boot for weeks.
 *   2. 0x150d8 returns the byte at CONSYS 0x02090614 (addi.gp r0, gp, #-27920).
 *   3. The ONLY writer of that byte is ROM 0x1506e, the last instruction of the
 *      A-die MT6625L probe at 0x14fdc.
 *   4. 0x14fdc is straight-line: no branches, no early return. So a zero there
 *      does not mean "the probe ran and failed", it means the probe was never
 *      entered. It is entered from 0x15118, which the ROM reaches only when
 *      wifi_task.c gets message 46 with parameter 3 -- and nothing in our
 *      bring-up posts that message. WMT FUNC_CTRL(3, on) was the obvious
 *      candidate and is measured NOT to be it: the chip answers "accepted" and
 *      this byte stays zero.
 *
 * Writing it is therefore not a guess about what the firmware wants; it is the
 * one bit whose absence the firmware names in its own log. Measured on the same
 * boot that had never got past the assert: WIFI_START rc=0, WLAN_READY set,
 * the connectivity PC moving through 0xf00268b2 / 0x0006a0aa / 0x0006143e, and
 * the EMI log free of rlm_phy.c for the first time.
 *
 * The flag is one byte, but a word is written: bytes +1..+3 read back zero both
 * before and after, so there is no neighbour to preserve, and ACCESS_REG is a
 * word interface.
 *
 * What this does NOT do is configure the A-die. The probe body reads MT6625L
 * registers over the ROM's SPI helper and stashes what it finds; skipping it
 * and asserting the result leaves those values at their reset state. `wifi
 * adie` reads the A-die chip id over WMT and gets 0x6627 back, so the part is
 * alive and answering.
 *
 * THAT THREAD HAS NOW BEEN PULLED, and it led back here. Forcing this byte does
 * not merely skip the probe as a side effect -- it makes the probe impossible,
 * because the sequence at 0x15118 opens with
 *
 *   0001511e:  lbi.gp r7, [gp + #-27920]
 *   00015122:  bnez38 r7, 0x1514a          ; already probed -> return
 *
 * so a forced 1 is a permanent "no". Which fits both symptoms exactly: a
 * receiver whose front end is at reset values hears no beacons (passive scan
 * completes with mgmt=0), and a transmitter in the same state collapses the
 * rail the moment a probe request keys it (active scan detaches the board).
 *
 * So this is now the FALLBACK, not the mechanism -- see run_adie_probe(), which
 * makes the ROM do the probe for real and lets it set this byte itself. This is
 * kept for the case where that fails, because a forced flag at least gets the
 * firmware running, which is where we were.
 */
#define CONSYS_ROM_ADIE_PROBED (CONSYS_ROM_GP - 27920u) /* 0x02090614 */

static void arm_adie_probe_flag(void)
{
    uint8_t event[32];
    uint32_t event_len = 0u;

    (void)mt6592_wifi_hif_rom_access_reg(1, CONSYS_ROM_ADIE_PROBED, 1u, event, sizeof event, &event_len);
}

/* The inverse, and the reason run_adie_probe() can be trusted twice in a row:
 * the probe refuses to run while this byte is set, so anything that set it --
 * an earlier call, or the forge below from a previous boot of this driver --
 * has to be taken back before asking. */
static void clear_adie_probe_flag(void)
{
    uint8_t event[32];
    uint32_t event_len = 0u;

    (void)mt6592_wifi_hif_rom_access_reg(1, CONSYS_ROM_ADIE_PROBED, 0u, event, sizeof event, &event_len);
}

/* Read the probe flag back through the ROM. -1 if the ROM did not answer, or
 * answered for a different address -- the events have slipped at that point and
 * any word we read out is a plausible lie. */
static int read_adie_probe_flag(void)
{
    uint8_t event[32];
    uint32_t event_len = 0u;

    if (mt6592_wifi_hif_rom_access_reg(0, CONSYS_ROM_ADIE_PROBED, 0u, event, sizeof event, &event_len) != 0 ||
        event_len < 12u)
    {
        return -1;
    }
    if (read_le32(event + 4u) != (uint32_t)CONSYS_ROM_ADIE_PROBED)
    {
        return -1;
    }
    return (int)(read_le32(event + 8u) & 0xffu);
}

/*
 * Have the ROM run the real MT6625L probe, instead of asserting its result.
 *
 * One INIT_CMD of type 7 posts wifi_task message 46 parameter 3 (the derivation
 * is at INIT_CMD_ADIE_PROBE), whose handler at 0xdb90 calls 0x15118:
 *
 *   0x15130  store 0x80000001 to 0x601200e4   ; power/clock the SPI path
 *   0x15136  jal 0x1164 (1)
 *   0x1513a  jal 0x15080                      ; two setup steps
 *   0x1513e  jal 0x1509c
 *   0x15142  jal 0x14fdc                      ; the probe body
 *   0x15146  restore 0x601200e4
 *
 * and the probe body's last instruction, 0x1506e, is the write of the flag we
 * used to forge. So the flag reading back as 1 here is not our own write coming
 * back -- we did not write it -- it is the probe reporting that it ran, on the
 * exact byte the firmware later asserts on.
 *
 * The message is handled asynchronously by wifi_task, and the probe's SPI
 * helper at 0x14f38 spins up to 32000 iterations per transfer, so this polls
 * rather than assuming. Returns 1 if the probe ran, 0 if it did not, -1 if the
 * ROM stopped answering.
 */
enum {
    ADIE_PROBE_POLL_INTERVAL_US = 20000u,
    ADIE_PROBE_POLL_LIMIT       = 100u, /* 2 s */
};

static int run_adie_probe(void)
{
    uint8_t* packet = tx_buffer();
    uint32_t i;
    int flag;

    if (mt6592_wifi_hif_bind() != 0)
    {
        return -1;
    }

    /* A flag that is already set is not good news, it is the thing standing in
     * the way: 0x15122 returns out of the probe on exactly this byte. It reads
     * set here whenever an earlier call in this session probed, and -- far more
     * likely on a board that has been running the previous builds -- whenever
     * the old unconditional forge left it that way and the CONSYS was never
     * power-cycled since. Those two cases are the same byte with the same value,
     * so believing it would mean reporting somebody else's success. Take it back
     * instead, and probe from a known state.
     *
     * Re-probing when the probe genuinely did run is safe: 0x14fdc is the
     * boot-time initialisation path, it only reads the A-die over SPI and
     * rebuilds its own table from what it reads. */
    flag = read_adie_probe_flag();
    if (flag < 0)
    {
        g_state.adie_probe_state = ADIE_PROBE_ROM_SILENT;
        return -1;
    }
    if (flag > 0)
    {
        g_state.adie_probe_stale = 1;
        clear_adie_probe_flag();
        flag = read_adie_probe_flag();
        if (flag != 0)
        {
            /* The write did not take. The gate stays shut, so the probe cannot
             * run at all -- do not send a command that would only be discarded
             * and then poll a byte that was never going to change. */
            g_state.adie_probe_state = ADIE_PROBE_STUCK;
            return -1;
        }
    }

    zero_bytes(packet, INIT_ADIE_PROBE_PACKET_SIZE);
    write_le16(packet + 0u, INIT_ADIE_PROBE_PACKET_SIZE);
    packet[4] = INIT_CMD_ADIE_PROBE;
    packet[5] = next_command_sequence();
    /* packet[8] stays 0. It is the only payload byte 0x20d0 branches on, and 0
     * takes the shorter of its two arms -- one buffer copy instead of two. The
     * post of message 46 is after the call either way. */

    write_port(MCR_WTDR0, HIF_TARGET_TXD0, packet, INIT_ADIE_PROBE_PACKET_SIZE);
    tx_resource_release();

    /* Deliberately NOT waiting on an init event. Type 7 is not one of the four
     * commands whose reply shape is known, and the thing worth waiting for is
     * not an acknowledgement of the command anyway -- it is the flag, which is
     * written by the probe itself several calls deeper. */
    for (i = 0u; i < ADIE_PROBE_POLL_LIMIT; ++i)
    {
        delay_us(ADIE_PROBE_POLL_INTERVAL_US);
        flag = read_adie_probe_flag();
        if (flag > 0)
        {
            g_state.adie_probe_state = ADIE_PROBE_RAN;
            g_state.adie_probe_polls = i + 1u;
            return 1;
        }
        if (flag < 0)
        {
            g_state.adie_probe_state = ADIE_PROBE_ROM_SILENT;
            g_state.adie_probe_polls = i + 1u;
            return -1;
        }
    }

    g_state.adie_probe_state = ADIE_PROBE_TIMEOUT;
    g_state.adie_probe_polls = ADIE_PROBE_POLL_LIMIT;
    return 0;
}

static int start_firmware(void)
{
    const uint32_t start_address = g_state.firmware_start_address;

    /*
     * The real probe first, and the forgery only if it did not happen.
     *
     * Order matters and is the opposite of what it was: the probe sequence
     * refuses to run once the flag is set, so forcing it first -- which is what
     * this function used to do -- guaranteed the RF front end stayed at reset
     * values. Now the flag is left alone until the ROM has had its chance at it,
     * and the fallback exists only so that a probe that fails still leaves us
     * with a firmware that boots, which is where this was before.
     */
    if (run_adie_probe() <= 0)
    {
        arm_adie_probe_flag();
    }

    /* Both of these are ROM writes and the ROM stops answering the moment
     * WIFI_START is dispatched, so this is the last window in which either can
     * be done at all. */
    release_rom_handoff();

    if (send_init_start(start_address) != 0 || wait_firmware_ready() != 0)
    {
        return -1;
    }

    g_state.last_whisr = mmio_read(MCR_WHISR);
    tx_resource_release();
    if (configure_firmware_runtime() != 0)
    {
        return -1;
    }

    mt6592_uart_puts("  wifi: firmware WLAN_READY; scan command path online\n");
    return 0;
}
#endif

int mt6592_wifi_hif_adie_probe(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    /* Every step of this runs through INIT_CMD, which only the boot ROM answers.
     * Once WIFI_START has been taken the ROM command handler is gone, so this
     * would poll a flag nobody is going to write. Belongs between `wifi dl` and
     * `wifi go`. */
    if (g_state.firmware_alive)
    {
        set_failure("MediaTek A-die probe must run before WIFI_START",
                    "mt6592-wifi:adie-probe-after-start");
        return -1;
    }
    return run_adie_probe();
#else
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

int mt6592_wifi_hif_load_firmware(const void* data, uint32_t size)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    const uint8_t* image = (const uint8_t*)data;

    if (g_state.firmware_alive)
    {
        return 0;
    }
    if (!image || size < 16u)
    {
        set_failure("MediaTek WIFI_RAM_CODE_SOC is missing", "mt6592-wifi:firmware-missing");
        return -1;
    }
    if (mt6592_wifi_hif_bind() != 0)
    {
        return -1;
    }

    const uint32_t signature     = read_le32(image + 0u);
    const uint32_t expected_crc  = read_le32(image + 4u);
    const uint32_t section_count = read_le32(image + 8u);
    if (signature != MTK_WIFI_SIGNATURE || section_count == 0u || section_count > MAX_FIRMWARE_SECTIONS)
    {
        set_failure("MediaTek WIFI_RAM_CODE_SOC divided-image header is invalid",
                    "mt6592-wifi:firmware-header-invalid");
        return -1;
    }
    const uint32_t header_size = 16u + section_count * 16u;
    if (header_size > size)
    {
        set_failure("MediaTek WIFI_RAM_CODE_SOC divided-image header is truncated",
                    "mt6592-wifi:firmware-header-truncated");
        return -1;
    }
    if (crc32(image + 8u, size - 8u) != expected_crc)
    {
        set_failure("MediaTek WIFI_RAM_CODE_SOC header CRC does not match", "mt6592-wifi:firmware-header-crc-mismatch");
        return -1;
    }

    g_state.firmware_size       = size;
    g_state.firmware_sections   = section_count;
    g_state.downloaded_bytes    = 0u;
    g_state.start_event_valid   = 0;
    g_state.start_event_length  = 0u;
    zero_bytes(g_state.start_event, sizeof(g_state.start_event));
    g_state.status            = "Downloading MediaTek WIFI_RAM_CODE_SOC";
    g_state.blocked           = "mt6592-wifi:firmware-download-in-progress";
    mt6592_uart_puts("  wifi: WIFI_RAM_CODE_SOC divided image sections=");
    mt6592_uart_put_hex32(section_count);
    mt6592_uart_puts(" bytes=");
    mt6592_uart_put_hex32(size);
    mt6592_uart_puts("\n");

    /*
     * Section 0's destination, captured below on the first pass -- NOT the
     * compile-time FIRMWARE_START_ADDRESS, even though stock's source passes
     * prRegInfo->u4StartAddress and that constant is what wlanProbe puts there.
     *
     * This driver sent section 0's destination up to commit b5403d48f1, which
     * scanned and associated. "Begin 1:1 wifi" (7cbd86195f) replaced it with
     * 0x00060000 on the strength of the disassembly, and WLAN_READY has not been
     * asserted since. Two of the three changes that arrived in that window are
     * now reverted; this is the second.
     *
     * The reference is not being ignored, it is being outranked. 0x00060000 is
     * where the WMT layer already put ROMv1_patch_1_0_hdr.bin, and the pre-START
     * program counter samples read 0x00066382 -- inside that patch. On a stock
     * boot the AP has torn far more down between the patch and the WLAN
     * download than this LK does, so "the entry veneer is resident at 0x60000"
     * may simply not be true here, while section 0's own base always is.
     *
     * Both are one console command away: `wifi start 60000' for the stock
     * constant, `wifi start image' for override=0. See send_init_start().
     */
    uint32_t start_address = 0u;

    for (uint32_t section = 0; section < section_count; ++section)
    {
        const uint8_t* entry        = image + 16u + section * 16u;
        const uint32_t file_offset  = read_le32(entry + 0u);
        const uint32_t section_size = read_le32(entry + 8u);
        const uint32_t destination  = read_le32(entry + 12u);
        if (section_size == 0u || file_offset > size || section_size > size - file_offset)
        {
            set_failure("MediaTek WIFI_RAM_CODE_SOC section range is invalid",
                        "mt6592-wifi:firmware-section-range-invalid");
            return -1;
        }
        if (section == 0u)
        {
            start_address = destination;
        }

        mt6592_uart_puts("  wifi: firmware section dst=");
        mt6592_uart_put_hex32(destination);
        mt6592_uart_puts(" len=");
        mt6592_uart_put_hex32(section_size);
        mt6592_uart_puts("\n");

        for (uint32_t offset = 0; offset < section_size; offset += FIRMWARE_CHUNK_SIZE)
        {
            uint32_t chunk = section_size - offset;
            if (chunk > FIRMWARE_CHUNK_SIZE)
            {
                chunk = FIRMWARE_CHUNK_SIZE;
            }
            if (send_init_download(destination + offset, image + file_offset + offset, chunk) != 0)
            {
                return -1;
            }
            g_state.downloaded_bytes += chunk;
            wifi_cooperative_yield();
            if ((offset & 0x3fffu) == 0u)
            {
                mt6592_pmic_charger_service();
            }
        }
    }

    g_state.firmware_loaded = 1;
    if (g_start_address_override != 0u)
    {
        start_address = g_start_address_override;
    }
    /* With override cleared the address field is not an address -- stock passes a
     * literal 0 alongside FALSE, and the firmware is meant to enter at whatever
     * its own header declares. Carrying 0x00060000 in the word would be sending a
     * third combination that no build produces. */
    if (g_start_override_word == 0u)
    {
        start_address = 0u;
    }
    g_state.firmware_start_address = start_address;
    mt6592_uart_puts("  wifi: firmware start override=");
    mt6592_uart_put_hex32(g_start_override_word);
    mt6592_uart_puts(" address=");
    mt6592_uart_put_hex32(start_address);
    mt6592_uart_puts("\n");
    if (g_defer_start)
    {
        g_state.status  = "MediaTek WLAN firmware downloaded; WIFI_START deferred";
        g_state.blocked = "mt6592-wifi:firmware-start-deferred";
        mt6592_uart_puts("  wifi: WIFI_START deferred; `wifi go` sends it\n");
        return 0;
    }
    return start_firmware();
#else
    (void)data;
    (void)size;
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

/*
 * `active` selects probe requests over pure listening.
 *
 * It is a parameter rather than a constant because SCAN_REQ is the first
 * command in this bring-up that makes the radio transmit, and the board dies
 * the instant it is submitted -- silently, with no CPU exception and no
 * watchdog reset, twice, at two different points in the caller. A passive scan
 * exercises the identical command path, the identical HIF writes and the
 * identical receive path, and differs only in whether the transmitter keys. So
 * running one answers "is it the command, or is it the RF?" without needing the
 * board to survive long enough to report anything.
 *
 * It is also not merely a probe: a passive scan still hears every beacon on the
 * band, so if it works, listing networks works.
 */
int mt6592_wifi_hif_start_scan(int active)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    uint8_t scan[SCAN_COMMAND_PAYLOAD_SIZE];

    if (!g_state.firmware_alive || !g_state.configured)
    {
        set_failure("MediaTek Wi-Fi scan needs WIFI_RAM_CODE_SOC first", "mt6592-wifi:scan-firmware-not-ready");
        return -1;
    }
    if (g_assoc_phase == ASSOC_FAILED) reset_association();
    /*
     * SCANNING WHILE CONNECTED IS THE NORMAL CASE, not an exception.
     *
     * A station that cannot look around while it is associated cannot let anyone
     * change networks, and cannot notice that they have walked out of range of
     * the one they are on -- so the radio is only useful for the first network it
     * ever joins. The firmware handles the off-channel excursion itself: it is
     * the same SCAN_REQ, and a sweep is short next to any AP's inactivity timer.
     * Nothing below touches the association -- g_assoc_profile is a private copy
     * taken at join time, so wiping the scan result table cannot disturb the link
     * this station is currently on.
     *
     * The one thing genuinely worth refusing is a scan submitted on top of an
     * association attempt that is STILL RUNNING, because both want the channel.
     * That is a narrower test than auth_active, and the difference is not
     * academic: auth_active stays set until something reaps the attempt, and the
     * reaper (association_timeout_pump) only runs inside poll(). An attempt that
     * nobody is polling therefore stays "active" forever and blocks every future
     * scan until a power cycle. That is what put "scan-association-active" on the
     * console permanently after one join.
     *
     * So ask whether the attempt is live rather than whether one was ever
     * started: a deadline still in the future means something is driving it and
     * the refusal is real; a deadline already past means it was abandoned, and an
     * abandoned attempt has no claim on the radio.
     */
    if (g_state.auth_active && g_assoc_phase != ASSOC_CONNECTED)
    {
        if (mt6592_timer_microseconds() < g_assoc_deadline_us)
        {
            set_failure("MediaTek scan deferred while association is active", "mt6592-wifi:scan-association-active");
            return -1;
        }
        reset_association();
    }
    if (acquire_driver_own() != 0)
    {
        return -1;
    }

    /*
     * Carry the associated BSS across the wipe. A sweep legitimately starts from
     * an empty table so that APs which have gone away stop being listed, but the
     * one BSS we can vouch for is the one we are currently joined to -- dropping
     * it and hoping the sweep re-hears it inside one channel dwell is how the
     * joined network kept disappearing from the list it should always head.
     */
    wifi_bss_profile carried;
    int carried_rssi  = 0;
    int carried_valid = 0;
    if (g_state.associated)
    {
        for (uint32_t i = 0; i < g_state.scan_result_count; ++i)
        {
            if (same_mac(g_scan_profiles[i].bssid, g_assoc_profile.bssid))
            {
                carried       = g_scan_profiles[i];
                carried_rssi  = g_scan_results[i].rssi;
                carried_valid = 1;
                break;
            }
        }
    }

    zero_bytes((uint8_t*)g_scan_results, sizeof(g_scan_results));
    zero_bytes((uint8_t*)g_scan_profiles, sizeof(g_scan_profiles));
    g_state.scan_result_count = 0u;
    if (carried_valid) store_scan_result(&carried, carried_rssi);
    /* Per-scan, unlike rx_packets and friends: `evt=1` across a whole session
     * cannot say which scan produced it, and that ambiguity is what made the
     * one event after `wifi domain` unreadable. */
    g_state.scan_end_reason = SCAN_END_NONE;
    g_state.event_ids_used  = 0u;
    /* Per-scan for the same reason: a sparse-channel report left over from the
     * previous run would read as this run's evidence. */
    g_state.scan_done_payload_len    = 0u;
    g_state.scan_done_sparse_valid   = 0u;
    g_state.scan_done_sparse_band    = 0u;
    g_state.scan_done_sparse_channel = 0u;
    g_state.scan_elapsed_ms          = 0u;
    zero_bytes(g_state.scan_done_raw, sizeof(g_state.scan_done_raw));
    ++g_scan_sequence;
    if (g_scan_sequence == 0u)
    {
        ++g_scan_sequence;
    }

    zero_bytes(scan, sizeof(scan));
    /*
     * CMD_SCAN_REQ_T, and these offsets are now READ OFF THE STOCK BINARY rather
     * than inferred from a header nobody here has. scnSendScanReq lives at
     * 0xc042d560 in this device's own kernel; it zeroes 710 bytes, fills them
     * from prAdapter->rWifiVar.rScanInfo.rScanParam, and sends 110 + u2IELen:
     *
     *    0      ucSeqNum              <- rScanParam+0x218
     *    1      ucNetworkType         <- rScanParam+0x04
     *    2      ucScanType            <- rScanParam+0x00
     *    3      ucSSIDType            <- rScanParam+0x08
     *    4      ucSSIDLength          <- rScanParam+0x0a  (only when ucSSIDNum==1)
     *    5      reserved
     *    6-7    u2ChannelMinDwellTime <- never written by anything in stock
     *    8-39   aucSSID[32]           <- rScanParam+0x0e
     *    40-41  u2ChannelDwellTime    <- only written for the P2P network index
     *    42     ucChannelType         <- rScanParam+0xa0
     *    43     ucChannelListNum      <- only written when ucChannelType==SPECIFIED
     *    44-107 arChannelList[32]     <- 2 bytes each, likewise SPECIFIED-only
     *    108-109 u2IELen
     *    110+   aucIE[600]
     *
     * So 42 really is the channel-type byte and 108 really is the IE length, the
     * two that a plausible mis-guess would put at 40 and 110; the 110-byte length
     * is stock's own zero-IE case; and leaving 43 and the channel list zero while
     * asking for SCAN_CHANNEL_2G4 is byte-for-byte what stock emits for a 2.4 GHz
     * wildcard scan. This layout is not a suspect any more.
     *
     * SINCE CONFIRMED FROM SOURCE, field for field: CMD_SCAN_REQ_T is declared at
     * include/nic_cmd_event.h:1434-1449 with exactly those members in exactly that
     * order, CHANNEL_INFO_T (1429) is the two bytes {ucBand, ucChannelNum} that
     * make the list 64 bytes wide, and scnSendScanReq (mgmt/scan_fsm.c:457-467)
     * sends `OFFSET_OF(CMD_SCAN_REQ, aucIE) + u2IELen` -- which is 110 for the
     * zero-IE case, arrived at independently of the disassembly above. The three
     * constants below are source too: SCAN_TYPE_PASSIVE_SCAN=0 /
     * SCAN_TYPE_ACTIVE_SCAN=1 (mgmt/scan.h:317-321), SCAN_CHANNEL_2G4=1
     * (mgmt/scan.h:329-336), SCAN_REQ_SSID_WILDCARD=BIT(0) (mgmt/scan.h:302).
     * u2ChannelMinDwellTime appears once in the whole tree -- its own declaration
     * -- so leaving 6-7 zero is not an omission, it is what stock sends.
     */
    scan[0]  = g_scan_sequence;
    scan[1]  = NETWORK_TYPE_AIS;
    scan[2]  = (uint8_t)(active ? SCAN_TYPE_ACTIVE : SCAN_TYPE_PASSIVE);
    scan[3]  = SCAN_SSID_WILDCARD;
    scan[42] = SCAN_CHANNEL_2G4;
    write_le16(scan + 108u, 0u);

    if (send_normal_command(CMD_ID_SCAN_REQ, scan, sizeof(scan)) != 0)
    {
        set_failure("MediaTek Wi-Fi scan command could not be submitted", "mt6592-wifi:scan-command-submit-failed");
        return -1;
    }

    g_state.scan_active = 1;
    g_scan_started_us   = mt6592_timer_microseconds();
    g_scan_deadline_us  = g_scan_started_us + SCAN_TIMEOUT_US;
    g_state.status      = "MediaTek Wi-Fi active scan in progress";
    g_state.blocked     = 0;
    return 0;
#else
    (void)active;
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

int mt6592_wifi_hif_poll(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_alive)
    {
        return -1;
    }
    if (acquire_driver_own() != 0)
    {
        ++g_state.driver_own_lost;
        if (g_state.scan_active) g_state.scan_end_reason = SCAN_END_OWN_LOST;
        g_state.scan_active = 0;
        if (g_state.auth_active)
            set_assoc_failure("MediaTek WLAN ownership was lost during association", "mt6592-wifi:association-driver-own-lost");
        return -1;
    }

    ++g_state.poll_calls;
    g_state.last_whisr = mmio_read(MCR_WHISR);
    g_state.whisr_seen |= g_state.last_whisr;
    if (g_state.last_whisr & WHISR_ABNORMAL_INT)
    {
        g_state.last_wasr   = mmio_read(MCR_WASR);
        if (g_state.scan_active) g_state.scan_end_reason = SCAN_END_ABNORMAL;
        g_state.scan_active = 0;
        if (g_state.auth_active)
            set_assoc_failure("MediaTek WLAN HIF failed during association", "mt6592-wifi:hif-abnormal-interrupt");
        else
            set_failure("MediaTek WLAN HIF reported an abnormal FIFO condition", "mt6592-wifi:hif-abnormal-interrupt");
        return -1;
    }
    if (g_state.last_whisr & WHISR_TX_DONE_INT)
    {
        tx_resource_release();
    }
    check_m4_tx_done();

    for (uint32_t processed = 0; processed < MAX_RX_PACKETS_PER_POLL; ++processed)
    {
        uint8_t port;
        uint32_t length;
        if (!read_next_packet(&port, &length))
        {
            break;
        }
        if (receive_packet(port, length) != 0)
        {
            ++g_state.dropped_packets;
            continue;
        }
        process_rx_packet(rx_buffer(), length);
    }

    association_timeout_pump();

    if (g_state.scan_active && mt6592_timer_microseconds() >= g_scan_deadline_us)
    {
        g_state.scan_end_reason = SCAN_END_TIMEOUT;
        g_state.scan_active = 0;
        g_state.status  = g_state.scan_result_count ? "MediaTek Wi-Fi scan timed out after collecting nearby networks"
                                                    : "MediaTek Wi-Fi scan timed out before receiving beacon responses";
        g_state.blocked = "mt6592-wifi:scan-timeout";
        return -1;
    }
    if (g_assoc_phase == ASSOC_FAILED) return -1;
    return 0;
#else
    return -1;
#endif
}

uint32_t mt6592_wifi_hif_get_scan_results(mt6592_wifi_scan_result* out, uint32_t max_results)
{
    uint32_t count = g_state.scan_result_count;

    if (!out || max_results == 0u)
    {
        return count;
    }
    if (count > max_results)
    {
        count = max_results;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        out[i] = g_scan_results[i];
    }

    /* Strongest networks first for the Settings list. */
    for (uint32_t i = 1; i < count; ++i)
    {
        const mt6592_wifi_scan_result value = out[i];
        uint32_t j                         = i;
        while (j > 0u && out[j - 1u].rssi < value.rssi)
        {
            out[j] = out[j - 1u];
            --j;
        }
        out[j] = value;
    }
    return count;
}

int mt6592_wifi_hif_auth_associate(const char* ssid, const char* password)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_alive || !g_state.configured || !ssid || ssid[0] == '\0')
    {
        set_assoc_failure("MediaTek association needs firmware and a scanned SSID",
                          "mt6592-wifi:association-prerequisite-missing");
        return -1;
    }
    int selected = -1;
    int best_rssi = -128;
    for (uint32_t i = 0; i < g_state.scan_result_count; ++i)
    {
        if (same_string(g_scan_profiles[i].ssid, ssid) && g_scan_results[i].rssi >= best_rssi)
        {
            selected = (int)i;
            best_rssi = g_scan_results[i].rssi;
        }
    }
    if (selected < 0)
    {
        set_assoc_failure("MediaTek association target is not in the latest scan results",
                          "mt6592-wifi:ssid-not-scanned");
        return -1;
    }
    if (g_scan_profiles[selected].security == WIFI_SECURITY_UNSUPPORTED)
    {
        set_assoc_failure("MediaTek driver supports open and WPA2-PSK/CCMP networks; selected security is unsupported",
                          "mt6592-wifi:security-mode-unsupported");
        return -1;
    }

    reset_association();
    g_assoc_profile = g_scan_profiles[selected];
    copy_string(g_assoc_password, sizeof(g_assoc_password), password ? password : "");
    if (g_assoc_profile.security == WIFI_SECURITY_WPA2_PSK_CCMP)
    {
        if (mt6592_wifi_pbkdf2_sha1(g_assoc_password,
                                    (const uint8_t*)g_assoc_profile.ssid,
                                    string_length(g_assoc_profile.ssid, 32u),
                                    g_pmk,
                                    wifi_cooperative_yield) != 0)
        {
            set_assoc_failure("MediaTek WPA2 password must contain 8-63 characters",
                              "mt6592-wifi:wpa2-password-invalid");
            return -1;
        }
        generate_snonce();
    }
    /*
     * ONE COMMAND, AND IT IS THE CHANNEL REQUEST.
     *
     * The station record used to go out here, ahead of it. Stock does not: its
     * AIS_STATE_REQ_CHANNEL_JOIN handler is 0xc0413c64..0xc0413cfc and the whole
     * of it builds one MSG_CH_REQ and posts it. The station record is created and
     * pushed to the firmware in aisFsmStateInit_JOIN (0xc0411c70), which only runs
     * once the grant has come back -- so on this radio a station record before a
     * channel grant is a record for a link the firmware has not yet agreed to.
     *
     * That ordering mattered less than the CID it was sent under, but it is the
     * same mistake twice: a command borrowed from a later step. See the grant
     * handler in process_event_packet(), which is where it now lives.
     */
    if (acquire_driver_own() != 0 || send_channel_request() != 0)
    {
        set_assoc_failure("MediaTek firmware could not start the association state machine",
                          "mt6592-wifi:association-start-failed");
        return -1;
    }

    g_assoc_phase = ASSOC_WAIT_CHANNEL;
    g_assoc_retries = 0u;
    g_assoc_aid = 0u;
    g_assoc_deadline_us = mt6592_timer_microseconds() + CHANNEL_TIMEOUT_US;
    g_state.auth_active = 1;
    g_state.associated = 0;
    g_state.data_path_ready = 0;
    g_sta_rec_index = STA_REC_INDEX_NOT_FOUND; /* see set_assoc_failure */
    g_pm_connected_sent = 0u;
    g_state.secure = 0;
    g_state.status = "MediaTek Wi-Fi requesting the target channel";
    g_state.blocked = 0;
    return 0;
#else
    (void)ssid;
    (void)password;
    return -1;
#endif
}

/*
 * ABANDON A HALF-FINISHED ASSOCIATION ON PURPOSE.
 *
 * An attempt that nobody polls never times out, because the only thing that
 * reaps a stalled step is association_timeout_pump() and it runs exclusively
 * inside poll(). So the caller that started the attempt is the only party who
 * knows when it has stopped caring, and it has to say so out loud -- otherwise
 * auth_active stays set, the radio stays claimed on behalf of a state machine
 * nobody is turning, and every later scan is refused for an attempt that has in
 * every practical sense already ended.
 *
 * That is the whole of what a wrong passphrase looked like from the console: not
 * "the key is wrong", which is a fine thing to be told, but "scanning is off
 * now", which is not recoverable and not explained. A failed join must cost
 * nothing beyond the join.
 *
 * Nothing is said to the AP. A half-associated station that never completed the
 * handshake has no session for the AP to tear down, and one that did will be
 * dropped on the AP's own inactivity timer. The FIRMWARE, on the other hand, has
 * to be told, because the join took the channel away from it -- see
 * send_channel_abort(). Releasing before reset_association() matters: the release
 * carries the channel and BSSID out of g_assoc_profile, which the reset wipes.
 *
 * A drop from CONNECTED is a different thing from a drop from half-way, and the
 * console showed it: `wifi drop` printed "the radio is free" and the very next
 * scan was refused, because the only state that had actually changed was ours.
 * The firmware still had a live BSS, a station record and a power-save
 * announcement for a link the driver had already forgotten, so it went on
 * spending TC4 on an AP nobody was talking to. Tearing that down is three
 * commands and they are the same three, in reverse, that made the link: media
 * state disconnected, then the station record removed.
 *
 * Returns 1 if there was something to abandon, 0 if it was already idle.
 */
int mt6592_wifi_hif_abort_association(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (g_assoc_phase == ASSOC_IDLE && !g_state.auth_active && !g_state.associated)
    {
        return 0;
    }
    if (acquire_driver_own() == 0)
    {
        if (g_state.associated)
        {
            (void)send_bss_disconnect();
            (void)send_sta_record_remove();
            (void)send_bss_reactivate();
        }
        release_channel_if_held();
    }
    reset_association();
    /* Set directly rather than through set_failure(): reset_association() leaves
     * status/blocked alone, and the stale pair is usually the association's own
     * progress line plus whatever refusal it caused -- both of which would keep
     * reading as live after the thing they describe is gone. */
    g_state.status  = "MediaTek Wi-Fi association abandoned; the radio is free";
    g_state.blocked = 0;
    return 1;
#else
    return 0;
#endif
}

/*
 * SEND EXACTLY ONE ASSOCIATION COMMAND AND THEN STOP.
 *
 * `wifi join` used to send UPDATE_STA_RECORD and CH_PRIVILEGE back to back, and
 * the console showed the firmware going silent somewhere across that pair -- rx
 * frozen from the moment they went out, and no answer to anything afterwards.
 * Two commands, one silence, and no way to tell which one did it, because they
 * were never sent apart. So this sends them apart.
 *
 * It answered better than it was meant to. The station record was going out
 * under CID 0x18, which is not UPDATE but REMOVE -- see CMD_ID_UPDATE_STA_RECORD
 * -- and the join no longer sends anything before the channel request at all.
 * Keep the probe anyway: it is the only way to put one association command on
 * the wire with nothing else moving, and the next unexplained silence will want
 * exactly that again.
 *
 * step 0 UPDATE_STA_RECORD (now 0x17), 1 CH_PRIVILEGE request, 2 release.
 */
int mt6592_wifi_hif_assoc_probe(const char* ssid, int step)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_alive || !g_state.configured)
    {
        set_failure("MediaTek association probe needs WIFI_RAM_CODE_SOC first",
                    "mt6592-wifi:probe-firmware-not-ready");
        return -1;
    }
    if (ssid && ssid[0] != '\0')
    {
        int selected  = -1;
        int best_rssi = -128;
        for (uint32_t i = 0; i < g_state.scan_result_count; ++i)
        {
            if (same_string(g_scan_profiles[i].ssid, ssid) && g_scan_results[i].rssi >= best_rssi)
            {
                selected  = (int)i;
                best_rssi = g_scan_results[i].rssi;
            }
        }
        if (selected < 0)
        {
            set_failure("MediaTek association probe target is not in the latest scan results",
                        "mt6592-wifi:ssid-not-scanned");
            return -1;
        }
        g_assoc_profile = g_scan_profiles[selected];
    }
    if (g_assoc_profile.ssid[0] == '\0')
    {
        set_failure("MediaTek association probe has no target; name one", "mt6592-wifi:probe-no-target");
        return -1;
    }
    if (acquire_driver_own() != 0) return -1;
    if (step == 0) return send_sta_record(STA_STATE_1);
    if (step == 1) return send_channel_request();
    if (step == 2) return send_channel_abort();
    return -1;
#else
    (void)ssid;
    (void)step;
    return -1;
#endif
}

int mt6592_wifi_hif_net_ready(void)
{
    return g_state.associated && g_state.data_path_ready;
}

int mt6592_wifi_hif_net_send(const void* frame, uint32_t len)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!mt6592_wifi_hif_net_ready() || !frame || len < 14u || len > MAX_ETHERNET_FRAME_SIZE) return -1;
    if (acquire_driver_own() != 0) return -1;
    const uint8_t* bytes = (const uint8_t*)frame;
    const int is_1x = bytes[12] == 0x88u && bytes[13] == 0x8eu;
    if (send_hif_frame(bytes, len, 0u, g_sta_rec_index, 0, is_1x, 0) != 0) return -1;
    ++g_state.tx_data;
    return (int)len;
#else
    (void)frame;
    (void)len;
    return -1;
#endif
}

int mt6592_wifi_hif_net_poll_rx(void* frame, uint32_t max_len)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!frame || max_len == 0u) return 0;
    (void)mt6592_wifi_hif_poll();
    if (g_rx_ethernet_tail == g_rx_ethernet_head) return 0;
    uint32_t len = g_rx_ethernet_len[g_rx_ethernet_tail];
    if (len > max_len) len = max_len;
    copy_bytes((uint8_t*)frame, g_rx_ethernet[g_rx_ethernet_tail], len);
    g_rx_ethernet_tail = (uint8_t)((g_rx_ethernet_tail + 1u) % RX_ETHERNET_QUEUE_DEPTH);
    return (int)len;
#else
    (void)frame;
    (void)max_len;
    return 0;
#endif
}

void mt6592_wifi_hif_get_mac(uint8_t mac[6])
{
    if (!mac) return;
    copy_bytes(mac, g_wifi_mac, 6u);
}

void mt6592_wifi_hif_set_start_address(uint32_t address)
{
    g_start_address_override = address;
}

void mt6592_wifi_hif_set_start_override(int enable)
{
    g_start_override_word = enable ? 1u : 0u;
}

int mt6592_wifi_hif_get_start_override(void)
{
    return (int)g_start_override_word;
}

void mt6592_wifi_hif_set_defer_start(int defer)
{
    g_defer_start = defer ? 1 : 0;
}

int mt6592_wifi_hif_start_firmware(void)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (!g_state.firmware_loaded)
    {
        set_failure("MediaTek WLAN firmware has not been downloaded", "mt6592-wifi:firmware-not-loaded");
        return -1;
    }
    return start_firmware();
#else
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

/*
 * Ask the WLAN boot ROM to write a recognisable marker at an arbitrary address.
 *
 * Every hypothesis left standing rests on an assumption nobody has tested: that
 * a download ACK with status==0 means the ROM's copy actually landed. It may
 * not. The ROM CRC-checks the packet it received -- that is what the status byte
 * reports -- and the copy into the destination happens afterwards, on a bus the
 * ROM does not check. If a sub-domain behind 0xf0020000 is unclocked, every
 * chunk would ACK exactly as we see and 90.6% of the firmware would quietly go
 * nowhere.
 *
 * This makes that testable. It sends one INIT_CMD_DOWNLOAD_BUF of `bytes` filled
 * with seed, seed+1, seed+2, ... to `address`, through the identical path the
 * real download uses, and returns whether the ROM ACKed. Two experiments follow:
 *
 *   1. Mark an address the ROM cannot possibly own -- 0xdeadb000. If that ACKs
 *      status==0, the ACK is proven to carry no information about the write, and
 *      the silent-drop hypothesis becomes the leading explanation for everything.
 *   2. Mark a real section destination and go looking for the pattern with
 *      `find`. Where it turns up establishes the AP<->CONSYS address mapping by
 *      measurement instead of by the single 0x0209f800 data point we extrapolate
 *      from today.
 *
 * The ROM's INIT handler is single-shot: this only works before WIFI_START.
 */
/*
 * Re-send a slice of the image's own ciphertext, on its own, to its own
 * destination -- the same INIT_CMD_DOWNLOAD_BUF path, the same 0x80000001 mode
 * word, just one small packet instead of a 2 KiB one.
 *
 * `wifi verify` found nine blocks that did not land, and they are exactly the
 * nine blocks in the whole 0x403a0-byte image that are PARTIALLY zero -- a
 * pointer or two in an otherwise-empty sixteen bytes. Every fully-dense block
 * landed (12764 of them checked) and every fully-zero block landed (2291
 * checked). Nine of nine, and nothing else: that is a rule, not a fault rate.
 *
 * The cipher is a byte-local substitution with a period-16 position key, so
 * ct[i] == E(0)[i] <=> pt[i] == 0 holds byte by byte and needs no key, and a
 * sixteen-byte packet at offset 0 decrypts exactly as those same sixteen bytes
 * did inside the big transfer. That makes this a clean experiment: if the block
 * lands when sent alone, the loss is in how it was carried, and re-sending is
 * also the fix. If it still does not land, the ROM is refusing that content or
 * that address and re-sending will never help.
 */
int mt6592_wifi_hif_rom_download(uint32_t address, const void* data, uint32_t bytes)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    if (mt6592_wifi_hif_bind() != 0)
    {
        return -1;
    }
    if (data == 0 || bytes == 0u)
    {
        return -1;
    }
    if (send_init_download(address, (const uint8_t*)data, align4(bytes)) != 0)
    {
        return -1;
    }
    g_state.status  = "MediaTek WLAN boot ROM accepted a re-sent block";
    g_state.blocked = 0;
    return 0;
#else
    (void)address;
    (void)data;
    (void)bytes;
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

int mt6592_wifi_hif_rom_write(uint32_t address, uint32_t seed, uint32_t bytes)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    static uint32_t marker[64];
    uint32_t i;

    if (mt6592_wifi_hif_bind() != 0)
    {
        return -1;
    }
    if (bytes == 0u || bytes > sizeof(marker))
    {
        bytes = sizeof(marker);
    }
    bytes = align4(bytes);
    for (i = 0; i < bytes / 4u; ++i)
    {
        marker[i] = seed + i;
    }
    if (send_init_download(address, (const uint8_t*)marker, bytes) != 0)
    {
        return -1;
    }
    g_state.status  = "MediaTek WLAN boot ROM accepted a marker write";
    g_state.blocked = 0;
    return 0;
#else
    (void)address;
    (void)seed;
    (void)bytes;
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

/*
 * Read or write connectivity memory through the boot ROM -- the only instrument
 * that reaches the parts of the chip the AP has no window onto.
 *
 * `wifi mark` proved a download ACK means the copy landed, but only for
 * addresses the AP can see afterwards. The two sections that decide whether the
 * firmware can run at all, s0 at CONSYS 0x0006a000 (the entry point) and s1 at
 * 0x0209f800, are not among them. This closes that: it asks the ROM to read
 * back one word, and the ROM lives on the far side of the same bus the ROM's
 * own copy used.
 *
 * A write goes out with ucSetQuery=1 and no decryption -- unlike `wifi mark`,
 * which reuses INIT_CMD_DOWNLOAD_BUF and therefore hands the payload to the
 * ROM's decryptor, so it can prove a page changed but never what it changed to.
 */
int mt6592_wifi_hif_rom_access_reg(int write, uint32_t address, uint32_t value, uint8_t* event_out,
                                   uint32_t event_max, uint32_t* event_len)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    uint8_t* packet        = tx_buffer();
    const uint8_t sequence = next_command_sequence();

    if (mt6592_wifi_hif_bind() != 0)
    {
        return -1;
    }

    zero_bytes(packet, INIT_ACCESS_REG_PACKET_SIZE);
    write_le16(packet + 0u, INIT_ACCESS_REG_PACKET_SIZE);
    packet[4] = INIT_CMD_ACCESS_REG;
    packet[5] = sequence;
    write_le32(packet + 8u, write ? 1u : 0u); /* ucSetQuery + 3 reserved bytes */
    write_le32(packet + 12u, address);
    write_le32(packet + 16u, write ? value : 0u);

    write_port(MCR_WTDR0, HIF_TARGET_TXD0, packet, INIT_ACCESS_REG_PACKET_SIZE);
    /* The control trace: this command is answered, so whatever the ROM's receive
     * path looks like, it looks like this. Same port, same write_port(). */
    pc_trace_capture();
    tx_resource_release();

    const int rc = wait_init_event(INIT_EVENT_ACCESS_REG, sequence, event_out, event_max, event_len);
    if (rc > 0)
    {
        /* A write is acknowledged in some other shape than the read's ACCESS_REG
         * event, and the write itself lands regardless -- measured. Report the
         * mismatch rather than a timeout, and do not call it a failure. */
        g_state.status  = "MediaTek WLAN boot ROM answered with a different event";
        g_state.blocked = 0;
        return 1;
    }
    if (rc != 0)
    {
        set_failure("MediaTek WLAN boot ROM did not answer ACCESS_REG",
                    "mt6592-wifi:rom-access-reg-timeout");
        return -1;
    }
    g_state.status  = "MediaTek WLAN boot ROM answered ACCESS_REG";
    g_state.blocked = 0;
    return 0;
#else
    (void)write;
    (void)address;
    (void)value;
    (void)event_out;
    (void)event_max;
    (void)event_len;
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

/*
 * Ask the boot ROM whether it has an error queued.
 *
 * Every one of the 130 download chunks ACKed with status==0, but that status is
 * the ROM's verdict on the packet it received, not on what it then did with it.
 * If the ROM noticed anything afterwards -- a bad destination, a failed
 * decrypt, a bus error on the copy -- this is where it would be waiting to say
 * so, and nothing has ever asked.
 */
int mt6592_wifi_hif_rom_query_error(uint8_t* event_out, uint32_t event_max, uint32_t* event_len)
{
#if MVII_MT6592_WIFI_ENABLE_TRANSPORT
    uint8_t* packet        = tx_buffer();
    const uint8_t sequence = next_command_sequence();

    if (mt6592_wifi_hif_bind() != 0)
    {
        return -1;
    }

    zero_bytes(packet, INIT_QUERY_ERROR_PACKET_SIZE);
    write_le16(packet + 0u, INIT_QUERY_ERROR_PACKET_SIZE);
    packet[4] = INIT_CMD_QUERY_PENDING_ERROR;
    packet[5] = sequence;

    write_port(MCR_WTDR0, HIF_TARGET_TXD0, packet, INIT_QUERY_ERROR_PACKET_SIZE);
    tx_resource_release();

    const int rc = wait_init_event(INIT_EVENT_PENDING_ERROR, sequence, event_out, event_max, event_len);
    if (rc > 0)
    {
        g_state.status  = "MediaTek WLAN boot ROM answered with a different event";
        g_state.blocked = 0;
        return 1;
    }
    if (rc != 0)
    {
        set_failure("MediaTek WLAN boot ROM did not answer QUERY_PENDING_ERROR",
                    "mt6592-wifi:rom-query-error-timeout");
        return -1;
    }
    g_state.status  = "MediaTek WLAN boot ROM answered QUERY_PENDING_ERROR";
    g_state.blocked = 0;
    return 0;
#else
    (void)event_out;
    (void)event_max;
    (void)event_len;
    set_failure("MediaTek WLAN transport disabled at build time", "mt6592-wifi:transport-disabled");
    return -1;
#endif
}

const mt6592_wifi_hif_state* mt6592_wifi_hif_get_state(void)
{
    /* Mirrored here rather than at each of the ten sites that assign
     * g_assoc_phase, because a mirror that is refreshed on read cannot go stale
     * and a mirror that is refreshed on write can. The enum values in the header
     * are declared to match this one's order. */
    g_state.assoc_phase   = (uint32_t)g_assoc_phase;
    g_state.assoc_retries = g_assoc_retries;
    g_state.sta_rec_index = g_sta_rec_index;
    g_state.m4_tx_done_timeouts = g_m4_tx_done_timeouts;
    return &g_state;
}
