/*
 * mt6592_msdc_sd.c — MT6592 MSDC1 microSD block driver (J36 Ultra).
 *
 * Mirrors the working MSDC0 eMMC PIO driver (mt6592_msdc.c) on the MSDC1
 * instance, with the SD-card init sequence instead of the eMMC one:
 *
 *   CMD0 (idle) -> CMD8 (voltage check, detects v2/SDHC) ->
 *   ACMD41 loop (OCR, HCS) -> CMD2 (CID) -> CMD3 (RCA) -> CMD9 (CSD, capacity)
 *   -> CMD7 (select) -> ACMD6 (4-bit bus) -> CMD16 (512B) -> 13 MHz.
 *
 * Card-absent behaviour: every command is terminated by the controller's own
 * response-timeout interrupt (MSDC_INT_CMDTMO fires after ~64 SD clocks), so a
 * probe with no card inserted costs a handful of sub-millisecond timeouts and
 * returns cleanly. mvii_storage_rescan() polls this for hot-plug.
 *
 * Register-layout notes (single place to adjust after the first hardware run,
 * diagnostics for all of them are printed to the serial/console ring):
 *   - MSDC1_BASE 0x11240000: confirmed by stock LK base table.
 *   - PERI PDN bit 13 for MSDC30_1 (bit 12 is MSDC30_0, used by the eMMC
 *     driver; MTK numbers MSDC gates consecutively).
 *   - CLK_CFG_3 low byte for the msdc30_1 mux (msdc30_0 sits in CLK_CFG_2
 *     bits [26:24] + PDN 31; MTK packs four muxes per CLK_CFG register, so
 *     msdc30_1 is the first slot of the next register).
 *   - GPIO mode register 0x410 for the six MSDC1 pads (MSDC0 pads occupy
 *     0x3f0/0x400; pads are numbered consecutively) and IO-config blocks
 *     0xc40/0xc50/0xc60 (again directly after MSDC0's 0xc00..0xc30).
 */

#include "mt6592_msdc_sd.h"
#include "mt6592_delay.h"

#include <stdint.h>

extern void minos_machine64_io_yield(void) __attribute__((weak));

static void sd_cooperative_yield(void) {
    if (&minos_machine64_io_yield) minos_machine64_io_yield();
}

#include "mt6592_msdc.h"   /* MT6592_MSDC_* error codes */
#include "mt6592_uart.h"
#include "mt6592_pmic.h"

#define BIT(n) (1u << (n))

enum {
    MSDC1_BASE = 0x11240000u,
    INFRA_BASE = 0x10000000u,
    PERICFG_BASE = 0x10003000u,
    GPIO_BASE = 0x10005000u,
    APMIXEDSYS_BASE = 0x10209000u,

    /* MSDC register offsets (same block layout as MSDC0). */
    MSDC_CFG = 0x000u,
    MSDC_IOCON = 0x004u,
    MSDC_PS = 0x008u,
    MSDC_INT = 0x00cu,
    MSDC_INTEN = 0x010u,
    MSDC_FIFOCS = 0x014u,
    MSDC_TXDATA = 0x018u,
    MSDC_RXDATA = 0x01cu,
    SDC_CFG = 0x030u,
    SDC_CMD = 0x034u,
    SDC_ARG = 0x038u,
    SDC_STS = 0x03cu,
    SDC_RESP0 = 0x040u,
    SDC_RESP1 = 0x044u,
    SDC_RESP2 = 0x048u,
    SDC_RESP3 = 0x04cu,
    SDC_BLK_NUM = 0x050u,
    MSDC_PATCH_BIT0 = 0x0b0u,
    MSDC_PATCH_BIT1 = 0x0b4u,
    MSDC_PATCH_BIT2 = 0x0b8u,
    MSDC_PAD_TUNE = 0x0ecu,
    MSDC_DAT_RDDLY0 = 0x0f8u,
    MSDC_DAT_RDDLY1 = 0x0fcu,

    MSDC_CFG_MODE = BIT(0),
    MSDC_CFG_CKPDN = BIT(1),
    MSDC_CFG_RST = BIT(2),
    MSDC_CFG_PIO = BIT(3),
    MSDC_CFG_CKSTB = BIT(7),
    MSDC_CFG_CLK_SRC_EXT = BIT(25),
    MSDC_CFG_CKDIV_EXT_MASK = 0x000fff00u,
    MSDC_CFG_CKMOD_EXT_MASK = 0x00300000u,
    MSDC_CFG_HS400_CK_MODE_EXT = BIT(22),

    MSDC_PS_DAT0 = BIT(16),

    /* DSPL moves the data sampling point by half a bus clock. With no board pad
     * delay table to consult, it is the only knob that decides whether 50 MHz
     * reads land intact; sd_tune_bus() tries both settings. */
    MSDC_IOCON_DSPL = BIT(2),

    MSDC_INT_ACMDRDY = BIT(3),
    MSDC_INT_ACMDTMO = BIT(4),
    MSDC_INT_ACMDCRCERR = BIT(5),
    MSDC_INT_CMDRDY = BIT(8),
    MSDC_INT_CMDTMO = BIT(9),
    MSDC_INT_RSPCRCERR = BIT(10),
    MSDC_INT_XFER_COMPL = BIT(12),
    MSDC_INT_DATTMO = BIT(14),
    MSDC_INT_DATCRCERR = BIT(15),
    MSDC_INT_BDCSERR = BIT(17),
    MSDC_INT_GPDCSERR = BIT(18),

    MSDC_FIFOCS_CLR = BIT(31),

    SDC_CFG_BUSWIDTH_MASK = 3u << 16,
    SDC_CFG_BUSWIDTH_4BIT = 1u << 16,

    SDC_STS_SDCBUSY = BIT(0),
    SDC_STS_CMDBUSY = BIT(1),

    SDC_CMD_WR = BIT(13),
    SDC_CMD_STOP = BIT(14),

    MSDC_FIFO_SIZE = 128u,
    MSDC_SRC_CLK_HZ = 200000000u,
    /* Protocol constant, not a tunable. The SDC_CMD block-length field is 12
     * bits wide, so anything at or above 4096 truncates -- 4 MiB truncates to
     * zero -- and CMD16 rejects it besides; the whole block layer above also
     * addresses in 512-byte sectors. Transfer rate is bought with bus width and
     * clock (see sd_tune_bus) and with the burst counts below, never here. */
    SD_BLOCK_SIZE = 512u,
    SD_HS_CLOCK_HZ = 50000000u,
    SD_RUNTIME_CLOCK_HZ = 25000000u,
    SD_FALLBACK_CLOCK_HZ = 13000000u,
    /* Runtime reads run as CMD18 bursts, mirroring the CMD25 write path.
     * One CMD17 per sector paid a full command setup + response poll +
     * XFER_COMPL round trip for every 512 bytes, which is what made opening a
     * multi-megabyte .virtua feel like the shell had wedged. The FIFO still
     * cannot be preempted mid-command, so the burst is kept small enough that
     * one command is a few hundred microseconds and sd_cooperative_yield()
     * runs between bursts — Explorer and the pointer stay responsive. At the
     * negotiated 4-bit/50 MHz, 32 blocks is ~650us of bus time. */
    SD_READ_BURST_BLOCKS = 32u,
    SD_WRITE_BURST_BLOCKS = 8u,
    /* CMD6 SWITCH_FUNC answers with a 64-byte status block, not a sector. */
    SD_SWITCH_STATUS_BYTES = 64u,

    /* --- MSDC1-specific clock / pinmux plumbing (board-verify) ----------- */
    PERI_PDN0_CLR = 0x0010u,
    PERI_PDN0_MSDC30_1 = BIT(13),
    CLK_CFG_3 = 0x0070u,
    CLK_CFG_3_MSDC30_1_SEL_MASK = 0x00000007u,
    CLK_CFG_3_MSDC30_1_PDN = BIT(7),
    MSDCPLL_CON0 = 0x0240u,
    MSDCPLL_PWR_CON0 = 0x024cu,
    PLL_PWR_ON = BIT(0),
    PLL_ISO_EN = BIT(1),
    PLL_EN = BIT(0),

    MSDC1_GPIO_MODE17 = 0x0410u,       /* six pads, 4-bit fields, mode 1 */
    MSDC1_GPIO_MODE17_MASK = 0x00ffffffu,
    MSDC1_GPIO_MODE17_VALUE = 0x00111111u,
    MSDC1_GPIO_CLK = 0x0c40u,
    MSDC1_GPIO_CMD = 0x0c50u,
    MSDC1_GPIO_DAT = 0x0c60u,
    GPIO_MSDC_E2 = BIT(8),
    GPIO_MSDC_E4 = BIT(9),
    GPIO_MSDC_E8 = BIT(10),
    GPIO_MSDC_DRVN_MASK = GPIO_MSDC_E2 | GPIO_MSDC_E4 | GPIO_MSDC_E8,
    GPIO_SR = BIT(12),
    GPIO_SMT = BIT(13),
    GPIO_IES = BIT(14),
    GPIO_R0 = BIT(0),
    GPIO_R1 = BIT(1),
    GPIO_PUPD = BIT(2),

    /* Bounded spin caps. Command completion is normally signalled by the
     * hardware CMDRDY/CMDTMO interrupt bits long before these run out; the
     * caps only bound truly wedged hardware. */
    SD_CMD_POLL_CAP = 1000000u,
    SD_DATA_POLL_CAP = 4000000u,
    SD_ACMD41_TRIES = 1000u,           /* ~1 s at 260 kHz including delays */
};

static const uint32_t k_cmd_ints = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO |
    MSDC_INT_ACMDRDY | MSDC_INT_ACMDCRCERR | MSDC_INT_ACMDTMO;
static const uint32_t k_data_ints =
    MSDC_INT_XFER_COMPL | MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR;

static int g_present;
static int g_hw_ready;
static uint32_t g_rca;              /* published RCA in [31:16] position */
static uint32_t g_high_capacity;    /* SDHC/SDXC: block addressing */
static uint64_t g_capacity_sectors;
static uint32_t g_resp[4];
static int g_absent_logged;         /* rate-limit "no card" logging */

/* ---- low-level register access ------------------------------------------ */

static volatile uint32_t* reg32(uint32_t offset) {
    return (volatile uint32_t*)(uintptr_t)(MSDC1_BASE + offset);
}

static volatile uint8_t* reg8(uint32_t offset) {
    return (volatile uint8_t*)(uintptr_t)(MSDC1_BASE + offset);
}

static uint32_t read32(uint32_t offset) {
    return *reg32(offset);
}

static void write32(uint32_t offset, uint32_t value) {
    *reg32(offset) = value;
}

static uint32_t read32_abs(uint32_t addr) {
    return *(volatile uint32_t*)(uintptr_t)addr;
}

static void write32_abs(uint32_t addr, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)addr = value;
}

static void set_bits_abs(uint32_t addr, uint32_t bits) {
    write32_abs(addr, read32_abs(addr) | bits);
}

static void clear_bits_abs(uint32_t addr, uint32_t bits) {
    write32_abs(addr, read32_abs(addr) & ~bits);
}

static void set_bits(uint32_t offset, uint32_t bits) {
    write32(offset, read32(offset) | bits);
}

static void clear_bits(uint32_t offset, uint32_t bits) {
    write32(offset, read32(offset) & ~bits);
}

static uint32_t bit_shift(uint32_t mask) {
    uint32_t shift = 0;
    while (shift < 32u && ((mask >> shift) & 1u) == 0) ++shift;
    return shift;
}

static void field_write_abs(uint32_t addr, uint32_t mask, uint32_t value) {
    uint32_t shift = bit_shift(mask);
    uint32_t reg = read32_abs(addr);
    reg &= ~mask;
    reg |= (value << shift) & mask;
    write32_abs(addr, reg);
}

static void delay_cycles(uint32_t cycles) {
    /* GPT4-anchored so the tuned settle times survive the stage2 MMU/cache
     * enable (a raw spin would run ~30-50x faster cached). */
    mt6592_delay_cycles(cycles);
}

static void watchdog_disable(void) {
    write32_abs(0x10007000u, 0x22000000u);
    write32_abs(0x10000500u, 0x22000000u);
}

static int wait_clear(uint32_t offset, uint32_t mask, uint32_t loops) {
    while (loops--) {
        if ((read32(offset) & mask) == 0) return MT6592_MSDC_OK;
    }
    return MT6592_MSDC_ERR_TIMEOUT;
}

static int wait_set(uint32_t offset, uint32_t mask, uint32_t loops) {
    while (loops--) {
        if ((read32(offset) & mask) != 0) return MT6592_MSDC_OK;
    }
    return MT6592_MSDC_ERR_TIMEOUT;
}

static uint32_t fifo_rx_count(void) {
    return read32(MSDC_FIFOCS) & 0xffu;
}

static uint32_t fifo_tx_count(void) {
    return (read32(MSDC_FIFOCS) >> 16) & 0xffu;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static void sd_fifo_clear(void) {
    set_bits(MSDC_FIFOCS, MSDC_FIFOCS_CLR);
    (void)wait_clear(MSDC_FIFOCS, MSDC_FIFOCS_CLR, 1000000u);
}

static void sd_reset(void) {
    set_bits(MSDC_CFG, MSDC_CFG_RST);
    (void)wait_clear(MSDC_CFG, MSDC_CFG_RST, 1000000u);
    sd_fifo_clear();
    write32(MSDC_INT, read32(MSDC_INT));
}

/* ---- clocks + pads ------------------------------------------------------- */

static void sd_prepare_clocks_and_pads(void) {
    uint32_t clk_cfg3;

    /* MSDCPLL: shared with MSDC0; powering it on twice is harmless. */
    set_bits_abs(APMIXEDSYS_BASE + MSDCPLL_PWR_CON0, PLL_PWR_ON);
    delay_cycles(1000u);
    clear_bits_abs(APMIXEDSYS_BASE + MSDCPLL_PWR_CON0, PLL_ISO_EN);
    set_bits_abs(APMIXEDSYS_BASE + MSDCPLL_CON0, PLL_EN);
    delay_cycles(20000u);

    /* msdc30_1 mux: clear select + PDN, matching what the working MSDC0 path
     * does with CLK_CFG_2 for msdc30_0. */
    clk_cfg3 = read32_abs(INFRA_BASE + CLK_CFG_3);
    clk_cfg3 &= ~(CLK_CFG_3_MSDC30_1_SEL_MASK | CLK_CFG_3_MSDC30_1_PDN);
    write32_abs(INFRA_BASE + CLK_CFG_3, clk_cfg3);
    write32_abs(PERICFG_BASE + PERI_PDN0_CLR, PERI_PDN0_MSDC30_1);
    delay_cycles(1000u);

    /* Pinmux: six MSDC1 pads (CLK, CMD, DAT0..3) to msdc mode. */
    field_write_abs(GPIO_BASE + MSDC1_GPIO_MODE17, MSDC1_GPIO_MODE17_MASK,
                    MSDC1_GPIO_MODE17_VALUE);

    /* IO cells: drive strength, schmitt trigger + input enable, and pull-ups
     * on CMD/DAT (SD requires them; CLK stays pull-free). */
    field_write_abs(GPIO_BASE + MSDC1_GPIO_CLK, GPIO_MSDC_DRVN_MASK, 4u);
    field_write_abs(GPIO_BASE + MSDC1_GPIO_CMD, GPIO_MSDC_DRVN_MASK, 2u);
    field_write_abs(GPIO_BASE + MSDC1_GPIO_DAT, GPIO_MSDC_DRVN_MASK, 2u);
    set_bits_abs(GPIO_BASE + MSDC1_GPIO_CLK, GPIO_SMT | GPIO_IES);
    set_bits_abs(GPIO_BASE + MSDC1_GPIO_CMD, GPIO_SMT | GPIO_IES);
    set_bits_abs(GPIO_BASE + MSDC1_GPIO_DAT, GPIO_SMT | GPIO_IES);
    field_write_abs(GPIO_BASE + MSDC1_GPIO_CMD, GPIO_R0 | GPIO_R1 | GPIO_PUPD, GPIO_R1);
    field_write_abs(GPIO_BASE + MSDC1_GPIO_DAT, GPIO_R0 | GPIO_R1 | GPIO_PUPD, GPIO_R1);
    delay_cycles(1000u);
}

static void sd_set_bus_width(uint32_t width_bits) {
    uint32_t v = read32(SDC_CFG);
    v &= ~SDC_CFG_BUSWIDTH_MASK;
    if (width_bits == 4u) v |= SDC_CFG_BUSWIDTH_4BIT;
    write32(SDC_CFG, v);
}

static int sd_set_clock(uint32_t hz) {
    uint32_t mode = 0;
    uint32_t div = 0;
    uint32_t cfg;

    if (hz >= MSDC_SRC_CLK_HZ) {
        mode = 1;
        div = 0;
    } else if (hz >= (MSDC_SRC_CLK_HZ >> 1)) {
        mode = 0;
        div = 0;
    } else {
        uint32_t denom = hz << 2;
        div = (MSDC_SRC_CLK_HZ + denom - 1u) / denom;
        if (div == 0) div = 1;
        if (div > 0xfffu) div = 0xfffu;
    }

    cfg = read32(MSDC_CFG);
    cfg &= ~(MSDC_CFG_CKPDN | MSDC_CFG_CKDIV_EXT_MASK | MSDC_CFG_CKMOD_EXT_MASK | MSDC_CFG_HS400_CK_MODE_EXT);
    cfg |= MSDC_CFG_MODE | MSDC_CFG_PIO | MSDC_CFG_CLK_SRC_EXT | ((mode & 0x3u) << 20) | ((div & 0xfffu) << 8);
    write32(MSDC_CFG, cfg);

    return wait_set(MSDC_CFG, MSDC_CFG_CKSTB, 1000000u);
}

static int sd_init_hw(void) {
    if (g_hw_ready) return MT6592_MSDC_OK;

    sd_prepare_clocks_and_pads();

    set_bits(MSDC_CFG, MSDC_CFG_MODE | MSDC_CFG_PIO);
    sd_reset();

    clear_bits(MSDC_PS, BIT(0));
    write32(MSDC_INT, read32(MSDC_INT));
    write32(MSDC_INTEN, 0x0001ff7bu);

    write32(MSDC_IOCON, 0u);
    write32(MSDC_DAT_RDDLY0, 0u);
    write32(MSDC_DAT_RDDLY1, 0u);
    write32(MSDC_PAD_TUNE, 0u);

    /* Same controller quirk values that make MSDC0 stable on this SoC. */
    write32(MSDC_PATCH_BIT0, 0x403c0006u);
    write32(MSDC_PATCH_BIT1, 0xffe24340u);
    write32(MSDC_PATCH_BIT2, 0x3489180du);

    write32(SDC_CFG, 0u);
    sd_set_bus_width(1u);

    if (sd_set_clock(260000u) != MT6592_MSDC_OK) return MT6592_MSDC_ERR_TIMEOUT;
    g_hw_ready = 1;
    return MT6592_MSDC_OK;
}

/* ---- command engine ------------------------------------------------------ */

static int sd_wait_cmd_ready(int check_data) {
    if (wait_clear(SDC_STS, SDC_STS_CMDBUSY, SD_CMD_POLL_CAP) != MT6592_MSDC_OK) {
        sd_reset();
        return MT6592_MSDC_ERR_TIMEOUT;
    }
    if (check_data) {
        if (wait_set(MSDC_PS, MSDC_PS_DAT0, SD_CMD_POLL_CAP) != MT6592_MSDC_OK) {
            sd_reset();
            return MT6592_MSDC_ERR_TIMEOUT;
        }
    }
    return MT6592_MSDC_OK;
}

static uint32_t build_raw_cmd(uint32_t opcode, uint32_t resp, uint32_t blocks, uint32_t block_size,
                              int write) {
    uint32_t raw = (opcode & 0x3fu) | ((resp & 0x7u) << 7);
    if (blocks != 0 || opcode == 16u) {
        raw |= (block_size & 0xfffu) << 16;
        if (blocks != 0) {
            raw |= (blocks > 1 ? 2u : 1u) << 11;
            if (write) raw |= SDC_CMD_WR;
        }
    }
    if (opcode == 12u) raw |= SDC_CMD_STOP;
    return raw;
}

static void fifo_write(const uint8_t* ptr, uint32_t size) {
    while ((((uintptr_t)ptr) & 3u) != 0 && size != 0) {
        *reg8(MSDC_TXDATA) = *ptr++;
        --size;
    }
    while (size >= 4u) {
        write32(MSDC_TXDATA, *(const uint32_t*)ptr);
        ptr += 4;
        size -= 4;
    }
    while (size != 0) {
        *reg8(MSDC_TXDATA) = *ptr++;
        --size;
    }
}

static void fifo_read(uint8_t* ptr, uint32_t size) {
    while ((((uintptr_t)ptr) & 3u) != 0 && size != 0) {
        *ptr++ = *reg8(MSDC_RXDATA);
        --size;
    }
    while (size >= 4u) {
        *(uint32_t*)ptr = read32(MSDC_RXDATA);
        ptr += 4;
        size -= 4;
    }
    while (size != 0) {
        *ptr++ = *reg8(MSDC_RXDATA);
        --size;
    }
}

static int pio_write(const uint8_t* ptr, uint32_t size) {
    uint32_t guard = SD_DATA_POLL_CAP;
    while (guard--) {
        uint32_t status = read32(MSDC_INT);
        if ((guard & 0xfffffu) == 0) watchdog_disable();
        if (status != 0) write32(MSDC_INT, status);
        status &= k_data_ints;

        if (status & (MSDC_INT_DATCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR)) return MT6592_MSDC_ERR_DATA;
        if (status & MSDC_INT_DATTMO) return MT6592_MSDC_ERR_TIMEOUT;
        if (status & MSDC_INT_XFER_COMPL) return size == 0 ? MT6592_MSDC_OK : MT6592_MSDC_ERR_DATA;

        if (size != 0) {
            uint32_t chunk = min_u32(size, MSDC_FIFO_SIZE);
            if ((MSDC_FIFO_SIZE - fifo_tx_count()) >= chunk) {
                fifo_write(ptr, chunk);
                ptr += chunk;
                size -= chunk;
            }
        }
    }
    return MT6592_MSDC_ERR_TIMEOUT;
}

static int pio_read(uint8_t* ptr, uint32_t size) {
    uint32_t guard = SD_DATA_POLL_CAP;
    while (guard--) {
        uint32_t status = read32(MSDC_INT);
        if ((guard & 0xfffffu) == 0) watchdog_disable();
        if (status != 0) write32(MSDC_INT, status);
        status &= k_data_ints;

        if (status & (MSDC_INT_DATCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR)) return MT6592_MSDC_ERR_DATA;
        if (status & MSDC_INT_DATTMO) return MT6592_MSDC_ERR_TIMEOUT;

        /* Drain everything the FIFO holds before going back to the interrupt
         * register: at 4-bit/50 MHz a 128-byte chunk arrives in ~5us, the same
         * order as an MMIO round trip on this bus. XFER_COMPL is still judged
         * from the `status` sampled before the drain, so a completion racing
         * the final chunk cannot make us return with data left behind. */
        while (size != 0) {
            uint32_t chunk = min_u32(size, MSDC_FIFO_SIZE);
            if (fifo_rx_count() < chunk) break;
            fifo_read(ptr, chunk);
            ptr += chunk;
            size -= chunk;
        }

        if (status & MSDC_INT_XFER_COMPL) return size == 0 ? MT6592_MSDC_OK : MT6592_MSDC_ERR_DATA;
    }
    return MT6592_MSDC_ERR_TIMEOUT;
}

/* resp encoding matches the MSDC SDC_CMD field: 1=R1, 2=R2, 3=R3 (no CRC),
 * 7=R1b. R6/R7 travel as R1-length responses.
 *
 * block_size is a parameter rather than SD_BLOCK_SIZE because CMD6
 * (SWITCH_FUNC) returns a 64-byte status block, not a sector. */
static int sd_cmd_sized(uint32_t opcode, uint32_t arg, uint32_t resp, uint8_t* data, uint32_t blocks,
                        uint32_t block_size, int write) {
    uint32_t status;
    uint32_t guard;
    int err;

    err = sd_wait_cmd_ready(blocks != 0 || resp == 7u);
    if (err != MT6592_MSDC_OK) return err;

    if (fifo_tx_count() != 0 || fifo_rx_count() != 0) {
        sd_reset();
        sd_fifo_clear();
    }

    write32(MSDC_INT, read32(MSDC_INT));
    write32(SDC_BLK_NUM, blocks);
    write32(SDC_ARG, arg);
    write32(SDC_CMD, build_raw_cmd(opcode, resp, blocks, block_size, write));

    status = 0;
    guard = SD_CMD_POLL_CAP;
    while (guard--) {
        status = read32(MSDC_INT);
        if (status & k_cmd_ints) break;
    }
    if (guard == 0) status = MSDC_INT_CMDTMO;

    g_resp[0] = read32(SDC_RESP0);
    g_resp[1] = read32(SDC_RESP1);
    g_resp[2] = read32(SDC_RESP2);
    g_resp[3] = read32(SDC_RESP3);

    write32(MSDC_INT, status & k_cmd_ints);

    if ((status & MSDC_INT_CMDRDY) == 0) {
        sd_reset();
        return (status & MSDC_INT_CMDTMO) ? MT6592_MSDC_ERR_TIMEOUT : MT6592_MSDC_ERR_CMD;
    }
    if ((status & MSDC_INT_RSPCRCERR) != 0 && resp != 3u) {
        sd_reset();
        return MT6592_MSDC_ERR_CMD;
    }

    if (blocks != 0) {
        err = write ? pio_write(data, blocks * block_size) : pio_read(data, blocks * block_size);
        if (err != MT6592_MSDC_OK) {
            sd_reset();
            sd_fifo_clear();
            return err;
        }
    }
    return MT6592_MSDC_OK;
}

static int sd_cmd(uint32_t opcode, uint32_t arg, uint32_t resp, uint8_t* data, uint32_t blocks, int write) {
    return sd_cmd_sized(opcode, arg, resp, data, blocks, SD_BLOCK_SIZE, write);
}

static int sd_app_cmd(uint32_t opcode, uint32_t arg, uint32_t resp) {
    int err = sd_cmd(55u, g_rca, 1u, 0, 0, 0);
    if (err != MT6592_MSDC_OK) return err;
    return sd_cmd(opcode, arg, resp, 0, 0, 0);
}

static int sd_wait_ready(void) {
    for (uint32_t i = 0; i < 20000u; ++i) {
        uint32_t state;
        int err = sd_cmd(13u, g_rca, 1u, 0, 0, 0);
        if ((i & 0x3fu) == 0) watchdog_disable();
        if (err != MT6592_MSDC_OK) return err;
        state = (g_resp[0] >> 9) & 0xfu;
        if ((g_resp[0] & BIT(8)) != 0 && state == 4u) return MT6592_MSDC_OK; /* tran */
        if ((g_resp[0] & BIT(8)) != 0 && state != 7u && state != 5u && state != 6u) return MT6592_MSDC_OK;
        delay_cycles(2000u);
    }
    return MT6592_MSDC_ERR_TIMEOUT;
}

/* ---- capacity ------------------------------------------------------------ */

/* The MSDC drops the R2 CRC byte: RESP0..3 hold CSD[127:8] left-justified the
 * same way the eMMC driver sees CID/CSD. Extract a bit field from that layout:
 * bit index is the standard CSD bit number (8..127). */
static uint32_t csd_bits(uint32_t start, uint32_t count) {
    /* Flatten RESP3..RESP0 into a 128-bit big-endian view: RESP3 = CSD[127:96],
     * RESP2 = CSD[95:64], RESP1 = CSD[63:32], RESP0 = CSD[31:0] (controller
     * convention; if capacity logs look wrong on hardware, this is the first
     * place to flip). */
    uint64_t value = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bit = start + count - 1u - i;
        uint32_t word = bit >> 5;          /* 0..3 → RESP0..RESP3 */
        uint32_t off = bit & 31u;
        value = (value << 1) | ((g_resp[word] >> off) & 1u);
    }
    return (uint32_t)value;
}

static uint64_t sd_capacity_from_csd(void) {
    uint32_t structure = csd_bits(126u, 2u);
    if (structure >= 1u) {
        /* CSD v2 (SDHC/SDXC): capacity = (C_SIZE + 1) * 512 KiB. */
        uint32_t c_size = csd_bits(48u, 22u);
        return ((uint64_t)c_size + 1ull) * 1024ull;
    }
    /* CSD v1: capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN. */
    {
        uint32_t c_size = csd_bits(62u, 12u);
        uint32_t c_size_mult = csd_bits(47u, 3u);
        uint32_t read_bl_len = csd_bits(80u, 4u);
        uint64_t bytes = ((uint64_t)c_size + 1ull) << (c_size_mult + 2u + read_bl_len);
        return bytes >> 9;
    }
}

/* ---- bus mode negotiation -------------------------------------------------
 *
 * Identification runs 1-bit at 260 kHz and the driver used to settle at 4-bit /
 * 25 MHz. Every card that answers CMD8 also supports the SDR25 "high speed"
 * access mode, which doubles the clock to 50 MHz -- so 4 lanes x 50 MHz, about
 * 25 MB/s, versus 12.5 MB/s before.
 *
 * As on the eMMC side, none of this is taken on trust: the board's pad delays
 * are not documented anywhere we can read, so each candidate is proved by
 * reading LBA 0 back over it and comparing against a copy taken at the slow,
 * known-good settings. The first mode that verifies wins; if none does, the
 * driver ends up exactly where it used to be. */

static uint8_t g_probe_ref[SD_BLOCK_SIZE];   /* LBA 0 as read at the safe rate */
static uint8_t g_probe_chk[SD_BLOCK_SIZE];
static uint32_t g_bus_width_bits = 1u;
static uint32_t g_bus_clock_hz;
static uint32_t g_bus_hs;

static void sd_set_sample_edge(uint32_t dspl) {
    uint32_t v = read32(MSDC_IOCON) & ~MSDC_IOCON_DSPL;
    if (dspl) v |= MSDC_IOCON_DSPL;
    write32(MSDC_IOCON, v);
}

/* LBA 0 addresses as 0 whether the card is byte- or block-addressed, so this
 * needs no capacity branch. */
static int sd_read_lba0(uint8_t* out) {
    return sd_cmd(17u, 0u, 1u, out, 1u, 0);
}

/* CMD6 SWITCH_FUNC. mode 0 queries what group 1 (access mode) supports without
 * changing anything; mode 1 commits. Either way the card answers with 64 bytes:
 * [12..13] is the group-1 support bitmap, and the low nibble of [16] is the
 * function actually selected. */
static int sd_switch_func(uint32_t mode, uint32_t access_mode, uint8_t status[SD_SWITCH_STATUS_BYTES]) {
    const uint32_t arg = (mode ? 0x80000000u : 0u) | 0x00fffff0u | (access_mode & 0xfu);
    return sd_cmd_sized(6u, arg, 1u, status, 1u, SD_SWITCH_STATUS_BYTES, 0);
}

static int sd_high_speed_supported(void) {
    uint8_t status[SD_SWITCH_STATUS_BYTES];
    if (sd_switch_func(0u, 1u, status) != MT6592_MSDC_OK) return 0;
    /* Group 1 support bitmap, big endian; bit 1 is SDR25/high speed. */
    return ((((uint32_t)status[12] << 8) | status[13]) & 0x0002u) != 0u;
}

static int sd_select_access_mode(uint32_t access_mode) {
    uint8_t status[SD_SWITCH_STATUS_BYTES];
    int err = sd_switch_func(1u, access_mode, status);
    if (err != MT6592_MSDC_OK) return err;
    if ((status[16] & 0x0fu) != access_mode) return MT6592_MSDC_ERR_UNSUPPORTED;
    /* The card needs 8 clocks to change its timing before the next command. */
    delay_cycles(2000u);
    return MT6592_MSDC_OK;
}

static int sd_bus_verify(void) {
    int err = sd_read_lba0(g_probe_chk);
    if (err != MT6592_MSDC_OK) return err;
    for (uint32_t i = 0; i < SD_BLOCK_SIZE; ++i) {
        if (g_probe_chk[i] != g_probe_ref[i]) return MT6592_MSDC_ERR_DATA;
    }
    return MT6592_MSDC_OK;
}

static int sd_bus_try(uint32_t width_bits, uint32_t hz, uint32_t hs, uint32_t dspl) {
    int err;

    /* Access mode on the card first, at the old clock: it has to know it is
     * about to be driven fast before it actually is. */
    if (hs != g_bus_hs) {
        err = sd_select_access_mode(hs ? 1u : 0u);
        if (err != MT6592_MSDC_OK) return err;
        g_bus_hs = hs;
    }
    /* ACMD6 changes the card's lane count; the host must follow immediately or
     * the next data command latches off the wrong lines. */
    if (width_bits != g_bus_width_bits) {
        err = sd_app_cmd(6u, width_bits == 4u ? 2u : 0u, 1u);
        if (err != MT6592_MSDC_OK) return err;
        g_bus_width_bits = width_bits;
    }
    sd_set_bus_width(width_bits);
    err = sd_set_clock(hz);
    if (err != MT6592_MSDC_OK) return err;
    sd_set_sample_edge(dspl);

    err = sd_bus_verify();
    if (err != MT6592_MSDC_OK) return err;
    g_bus_clock_hz = hz;
    return MT6592_MSDC_OK;
}

static void sd_bus_restore_safe(void) {
    (void)sd_set_clock(SD_FALLBACK_CLOCK_HZ);
    sd_set_sample_edge(0u);
    if (g_bus_hs != 0u) {
        if (sd_select_access_mode(0u) == MT6592_MSDC_OK) g_bus_hs = 0u;
    }
    if (g_bus_width_bits != 1u) {
        if (sd_app_cmd(6u, 0u, 1u) == MT6592_MSDC_OK) g_bus_width_bits = 1u;
    }
    sd_set_bus_width(1u);
    g_bus_width_bits = 1u;
    g_bus_clock_hz = SD_FALLBACK_CLOCK_HZ;
}

static int sd_tune_bus(void) {
    /* Ordered by lanes x clock, best first. */
    static const struct { uint32_t width_bits; uint32_t hz; uint32_t hs; } k_ladder[] = {
        {4u, SD_HS_CLOCK_HZ,      1u},  /* 4 x 50   MHz */
        {4u, SD_RUNTIME_CLOCK_HZ, 0u},  /* 4 x 25   MHz */
        {1u, SD_HS_CLOCK_HZ,      1u},  /* 1 x 50   MHz */
        {1u, SD_RUNTIME_CLOCK_HZ, 0u},  /* 1 x 25   MHz */
        {1u, SD_FALLBACK_CLOCK_HZ, 0u}, /* 1 x 12.5 MHz */
    };
    const int hs_ok = sd_high_speed_supported();
    int err;

    /* Reference read at the settings identification just finished on. If even
     * this fails the card is not usable at any speed. */
    (void)sd_set_clock(SD_FALLBACK_CLOCK_HZ);
    sd_set_bus_width(1u);
    sd_set_sample_edge(0u);
    g_bus_width_bits = 1u;
    g_bus_clock_hz = SD_FALLBACK_CLOCK_HZ;
    g_bus_hs = 0u;
    err = sd_read_lba0(g_probe_ref);
    if (err != MT6592_MSDC_OK) return err;

    for (uint32_t i = 0; i < (sizeof(k_ladder) / sizeof(k_ladder[0])); ++i) {
        if (k_ladder[i].hs != 0u && !hs_ok) continue;
        for (uint32_t dspl = 0; dspl < 2u; ++dspl) {
            if (sd_bus_try(k_ladder[i].width_bits, k_ladder[i].hz, k_ladder[i].hs, dspl) == MT6592_MSDC_OK) {
                mt6592_uart_puts("  sd: bus ");
                mt6592_uart_put_dec(g_bus_width_bits);
                mt6592_uart_puts("-bit ");
                mt6592_uart_put_dec(g_bus_clock_hz / 1000000u);
                mt6592_uart_puts("MHz hs=");
                mt6592_uart_put_dec(g_bus_hs);
                mt6592_uart_puts(" edge=");
                mt6592_uart_put_dec(dspl);
                mt6592_uart_puts("\n");
                return MT6592_MSDC_OK;
            }
            sd_bus_restore_safe();
        }
    }

    /* Back on the identification settings; prove they still read before telling
     * the storage layer the card is usable. */
    sd_bus_restore_safe();
    return sd_bus_verify();
}

/* ---- public API ----------------------------------------------------------- */

void mt6592_sd_forget(void) {
    g_present = 0;
    g_rca = 0;
    g_capacity_sectors = 0;
}

static int sd_card_init(void) {
    int err;
    uint32_t ocr = 0;
    int v2 = 0;

    (void)sd_set_clock(260000u);
    sd_set_bus_width(1u);

    /* CMD0: no response — always "succeeds"; it resets any half-initialised
     * card back to idle. */
    (void)sd_cmd(0u, 0u, 0u, 0, 0, 0);
    delay_cycles(20000u);

    /* CMD8: only v2 cards answer. A response timeout here with no card
     * inserted is the fast-exit path for hot-plug polling. */
    err = sd_cmd(8u, 0x000001aau, 1u, 0, 0, 0);
    if (err == MT6592_MSDC_OK) {
        if ((g_resp[0] & 0xffu) != 0xaau) return MT6592_MSDC_ERR_CMD;
        v2 = 1;
    } else if (err != MT6592_MSDC_ERR_TIMEOUT) {
        return err;
    }

    /* ACMD41 until powered up. Without a card, CMD55 times out on the first
     * lap and we bail immediately. */
    g_rca = 0;
    for (uint32_t i = 0; i < SD_ACMD41_TRIES; ++i) {
        uint32_t arg = 0x00ff8000u | (v2 ? 0x40000000u : 0u);
        err = sd_app_cmd(41u, arg, 3u);
        if (err != MT6592_MSDC_OK) return err; /* first lap = fast no-card fail */
        ocr = g_resp[0];
        if (ocr & BIT(31)) break;
        if ((i & 0x3fu) == 0) watchdog_disable();
        delay_cycles(30000u);
    }
    if ((ocr & BIT(31)) == 0) return MT6592_MSDC_ERR_TIMEOUT;
    g_high_capacity = (ocr & BIT(30)) != 0u;

    err = sd_cmd(2u, 0u, 2u, 0, 0, 0);           /* CID */
    if (err != MT6592_MSDC_OK) return err;
    err = sd_cmd(3u, 0u, 1u, 0, 0, 0);           /* R6: published RCA */
    if (err != MT6592_MSDC_OK) return err;
    g_rca = g_resp[0] & 0xffff0000u;

    err = sd_cmd(9u, g_rca, 2u, 0, 0, 0);        /* CSD → capacity */
    if (err != MT6592_MSDC_OK) return err;
    g_capacity_sectors = sd_capacity_from_csd();

    err = sd_cmd(7u, g_rca, 7u, 0, 0, 0);        /* select */
    if (err != MT6592_MSDC_OK) return err;
    err = sd_wait_ready();
    if (err != MT6592_MSDC_OK) return err;

    err = sd_cmd(16u, SD_BLOCK_SIZE, 1u, 0, 0, 0);
    if (err != MT6592_MSDC_OK) return err;

    /* Lane count, access mode and clock are all negotiated together and each
     * candidate is verified against a reference read -- see sd_tune_bus(). */
    return sd_tune_bus();
}

int mt6592_sd_probe(void) {
    /* Card power plus a transfer burst is the reported shutdown trigger when
     * the battery is out; raise the input limit before drawing it. */
    if (&mt6592_pmic_power_hold) mt6592_pmic_power_hold();
    int err;

    if (g_present) return MT6592_MSDC_OK;

    err = sd_init_hw();
    if (err != MT6592_MSDC_OK) return err;

    err = sd_card_init();
    if (err != MT6592_MSDC_OK) {
        mt6592_sd_forget();
        if (!g_absent_logged) {
            mt6592_uart_puts("  sd: no card on MSDC1 (probe err=");
            mt6592_uart_put_dec((uint32_t)(-err));
            mt6592_uart_puts("); hot-plug rescan continues\n");
            g_absent_logged = 1;
        }
        return err;
    }

    g_present = 1;
    g_absent_logged = 0;
    mt6592_uart_puts("  sd: MSDC1 card ready rca=");
    mt6592_uart_put_hex32(g_rca);
    mt6592_uart_puts(g_high_capacity ? " sdhc" : " sdsc");
    mt6592_uart_puts(" sectors=");
    mt6592_uart_put_dec(g_capacity_sectors);
    mt6592_uart_puts("\n");
    return MT6592_MSDC_OK;
}

int mt6592_sd_present(void) {
    return g_present;
}

uint64_t mt6592_sd_capacity_sectors(void) {
    return g_present ? g_capacity_sectors : 0;
}

static uint32_t sd_block_arg(uint64_t lba) {
    return g_high_capacity ? (uint32_t)lba : (uint32_t)(lba << 9);
}

int mt6592_sd_read(uint64_t lba, uint32_t nsectors, void* buffer) {
    /* Card power plus a transfer burst is the reported shutdown trigger when
     * the battery is out; raise the input limit before drawing it. */
    if (&mt6592_pmic_power_hold) mt6592_pmic_power_hold();
    uint8_t* out = (uint8_t*)buffer;
    if (!g_present || !buffer || nsectors == 0) return MT6592_MSDC_ERR_UNSUPPORTED;

    while (nsectors != 0) {
        uint32_t blocks = nsectors > SD_READ_BURST_BLOCKS ? SD_READ_BURST_BLOCKS : nsectors;
        uint32_t opcode = blocks > 1u ? 18u : 17u;
        int err;

        watchdog_disable();
        err = sd_cmd(opcode, sd_block_arg(lba), 1u, out, blocks, 0);
        if (err == MT6592_MSDC_OK && blocks > 1u) {
            err = sd_cmd(12u, 0u, 7u, 0, 0, 0);
        }
        if (err != MT6592_MSDC_OK) {
            /* Surprise removal shows up as timeouts: drop the card so the
             * hot-plug rescan re-probes instead of hammering a dead slot. */
            mt6592_sd_forget();
            return err;
        }
        lba += blocks;
        out += blocks * SD_BLOCK_SIZE;
        nsectors -= blocks;
        if (nsectors != 0) sd_cooperative_yield();
    }
    return MT6592_MSDC_OK;
}

int mt6592_sd_write(uint64_t lba, uint32_t nsectors, const void* buffer) {
    /* Card power plus a transfer burst is the reported shutdown trigger when
     * the battery is out; raise the input limit before drawing it. */
    if (&mt6592_pmic_power_hold) mt6592_pmic_power_hold();
    const uint8_t* in = (const uint8_t*)buffer;
    static int first_write_logged;
    if (!g_present || !buffer || nsectors == 0) return MT6592_MSDC_ERR_UNSUPPORTED;

    /* First write through this driver on real hardware is unproven bring-up
     * territory: leave a ring breadcrumb before touching the card so a wedge
     * or crash here is attributable from `flash -mtk-read-boot-status`. */
    if (!first_write_logged) {
        first_write_logged = 1;
        mt6592_uart_puts("  sd: first write lba=");
        mt6592_uart_put_dec((uint32_t)lba);
        mt6592_uart_puts(" n=");
        mt6592_uart_put_dec(nsectors);
        mt6592_uart_puts("\n");
    }

    while (nsectors != 0) {
        uint32_t blocks = nsectors > SD_WRITE_BURST_BLOCKS ? SD_WRITE_BURST_BLOCKS : nsectors;
        uint32_t opcode = blocks > 1u ? 25u : 24u;
        int err;

        watchdog_disable();
        err = sd_cmd(opcode, sd_block_arg(lba), 1u, (uint8_t*)(uintptr_t)in, blocks, 1);
        if (err == MT6592_MSDC_OK && blocks > 1u) {
            err = sd_cmd(12u, 0u, 7u, 0, 0, 0);
        }
        if (err == MT6592_MSDC_OK) {
            err = sd_wait_ready();
        }
        if (err != MT6592_MSDC_OK) {
            mt6592_uart_puts("  sd: write failed err=");
            mt6592_uart_put_dec((uint32_t)(-err));
            mt6592_uart_puts(" lba=");
            mt6592_uart_put_dec((uint32_t)lba);
            mt6592_uart_puts("\n");
            mt6592_sd_forget();
            return err;
        }
        lba += blocks;
        in += blocks * SD_BLOCK_SIZE;
        nsectors -= blocks;
        if (nsectors != 0) sd_cooperative_yield();
    }
    return MT6592_MSDC_OK;
}
