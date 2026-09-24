#include "mt6592_msdc.h"

#include "mt6592_delay.h"
#include "mt6592_pmic.h"

#include <stdint.h>

extern void minos_machine64_io_yield(void) __attribute__((weak));

static void msdc_cooperative_yield(void) {
    if (&minos_machine64_io_yield) minos_machine64_io_yield();
}

#define BIT(n) (1u << (n))

enum {
    MSDC0_BASE = 0x11230000u,
    INFRA_BASE = 0x10000000u,
    PERICFG_BASE = 0x10003000u,
    GPIO_BASE = 0x10005000u,
    APMIXEDSYS_BASE = 0x10209000u,

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
    SDC_ADV_CFG0 = 0x064u,
    EMMC_IOCON = 0x07cu,
    MSDC_PAD_CTL0 = 0x0e0u,
    MSDC_PAD_CTL1 = 0x0e4u,
    MSDC_PAD_CTL2 = 0x0e8u,
    MSDC_PATCH_BIT0 = 0x0b0u,
    MSDC_PATCH_BIT1 = 0x0b4u,
    MSDC_PATCH_BIT2 = 0x0b8u,
    MSDC_PAD_TUNE = 0x0ecu,
    MSDC_PAD_TUNE0 = 0x0f0u,
    MSDC_PAD_TUNE1 = 0x0f4u,
    MSDC_DAT_RDDLY0 = 0x0f8u,
    MSDC_DAT_RDDLY1 = 0x0fcu,
    MSDC_HW_DBG = 0x100u,
    MSDC_VERSION = 0x104u,
    EMMC50_CFG0 = 0x208u,

    CLK_CFG_2 = 0x0060u,
    PERI_PDN0_CLR = 0x0010u,
    PERI_PDN0_STA = 0x0018u,
    MSDCPLL_CON0 = 0x0240u,
    MSDCPLL_PWR_CON0 = 0x024cu,
    MSDC0_GPIO_CLK = 0x0c00u,
    MSDC0_GPIO_CMD = 0x0c10u,
    MSDC0_GPIO_DAT = 0x0c20u,
    MSDC0_GPIO_PAD = 0x0c30u,
    MSDC0_GPIO_MODE15 = 0x03f0u,
    MSDC0_GPIO_MODE16 = 0x0400u,

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

    /* Sampling edges. DSPL moves the data window by half a bus clock, which is
     * the one knob that decides whether 50 MHz reads come back intact on a
     * board whose pad delays we cannot look up; the bus tuner tries both. */
    MSDC_IOCON_RSPL = BIT(1),
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

    CLK_CFG_2_MSDC30_0_SEL_MASK = 0x07000000u,
    CLK_CFG_2_MSDC30_0_PDN = BIT(31),
    PERI_PDN0_MSDC30_0 = BIT(12),
    PLL_PWR_ON = BIT(0),
    PLL_ISO_EN = BIT(1),
    PLL_EN = BIT(0),
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

    SDC_CFG_BUSWIDTH_MASK = 3u << 16,

    SDC_STS_SDCBUSY = BIT(0),
    SDC_STS_CMDBUSY = BIT(1),

    SDC_CMD_WR = BIT(13),
    SDC_CMD_STOP = BIT(14),

    MSDC_FIFO_SIZE = 128u,
    MSDC_SRC_CLK_HZ = 200000000u,
    EMMC_RCA = 1u,
    /* Protocol constant, not a tunable: the SDC_CMD block-length field is 12
     * bits, so anything at or above 4096 truncates (4 MiB truncates to zero)
     * and CMD16 rejects it outright. Transfer size is bought with bus width and
     * clock -- see the bus tuner -- and with the burst counts below. */
    EMMC_BLOCK_SIZE = 512u,
    /* Largest run of blocks pushed in one open-ended CMD25 burst. Keep this
     * matched to the 64 KiB feed DATA chunk so flashing does not degrade into
     * tiny 4 KiB storage transfers. */
    MSDC_MAX_BURST_BLOCKS = 128u,
    /* Reads are issued from the cooperative OS runtime, so the burst is kept
     * short enough that one CMD18 is a few hundred microseconds and the
     * scheduler gets a turn between commands. At the negotiated 8-bit/50 MHz
     * this is ~650us of bus time; at the old 1-bit/12.5 MHz even the previous
     * 16-block burst was over 5ms, i.e. longer than a whole scheduler slice. */
    MSDC_READ_BURST_BLOCKS = 64u,
    /* The flash payload has no scheduler to starve, so it streams in the
     * largest bursts the PIO path sustains. */
    MSDC_READ_BURST_BLOCKS_STANDALONE = 128u,
    EMMC_EXT_CSD_SEC_COUNT = 212u,
    EMMC_EXT_CSD_PARTITION_CONFIG = 179u,
    EMMC_EXT_CSD_BUS_WIDTH = 183u,
    EMMC_EXT_CSD_HS_TIMING = 185u,
    EMMC_EXT_CSD_DEVICE_TYPE = 196u,
    EMMC_EXT_CSD_PARTITION_ACCESS_MASK = 0x07u,
    EMMC_EXT_CSD_BOOT_PARTITION_ENABLE_MASK = 0x38u,
    EMMC_EXT_CSD_BOOT_ACK = BIT(6),
    EMMC_SWITCH_ACCESS_WRITE_BYTE = 3u,
    EMMC_PARTITION_ACCESS_USER = 0u,
    EMMC_PARTITION_ACCESS_BOOT1 = 1u,
    EMMC_PARTITION_ACCESS_BOOT2 = 2u,

    /* DEVICE_TYPE advertises which clocks the part tolerates in HS timing. */
    EMMC_DEVICE_TYPE_HS_26 = BIT(0),
    EMMC_DEVICE_TYPE_HS_52 = BIT(1),
    /* BUS_WIDTH byte values, which are also the MSDC SDC_CFG field encoding. */
    EMMC_BUS_WIDTH_1BIT = 0u,
    EMMC_BUS_WIDTH_4BIT = 1u,
    EMMC_BUS_WIDTH_8BIT = 2u,

    /* Card identification has to run slow and single-ended; everything after it
     * is negotiated. msdc_set_clock() divides 200 MHz by 4*div, so these land on
     * 50 / 25 / 12.5 MHz -- each inside its mode's ceiling. */
    MSDC_CLOCK_ID_HZ = 260000u,
    MSDC_CLOCK_SAFE_HZ = 13000000u,
    MSDC_CLOCK_LEGACY_HZ = 26000000u,
    MSDC_CLOCK_HS_HZ = 52000000u,

    MSDC_DIAG_STAGE_HW_INIT = 1u,
    MSDC_DIAG_STAGE_SET_CLOCK = 2u,
    MSDC_DIAG_STAGE_CMD_READY = 3u,
    MSDC_DIAG_STAGE_CMD = 4u,
    MSDC_DIAG_STAGE_DATA = 5u,
    MSDC_DIAG_STAGE_OCR = 6u,
    MSDC_DIAG_STAGE_READY = 7u,
    MSDC_DIAG_STAGE_WRITE = 8u,
    MSDC_DIAG_STAGE_READ = 9u,
    MSDC_DIAG_STAGE_SWITCH = 10u,
    MSDC_DIAG_STAGE_TUNE = 11u,
};

static const uint32_t k_cmd_ints = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO |
    MSDC_INT_ACMDRDY | MSDC_INT_ACMDCRCERR | MSDC_INT_ACMDTMO;
static const uint32_t k_data_ints =
    MSDC_INT_XFER_COMPL | MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR;

static uint32_t g_last_resp;
static uint32_t g_ocr;
static uint32_t g_high_capacity;
static int g_initialized;
static int g_last_write;
static uint32_t g_active_part = 0xffffffffu;
static uint8_t g_ext_csd[EMMC_BLOCK_SIZE];
/* EXT_CSD as read at the conservative 1-bit identification speed. Every bus
 * mode the tuner tries is validated by re-reading EXT_CSD over the new bus and
 * comparing against this. */
static uint8_t g_ext_csd_ref[EMMC_BLOCK_SIZE];
static uint32_t g_bus_width_code = EMMC_BUS_WIDTH_1BIT;
static uint32_t g_bus_width_bits = 1u;
static uint32_t g_bus_clock_hz;
static uint32_t g_bus_hs;
static uint32_t g_bus_edge;
static mt6592_msdc_diag_t g_diag;

static volatile uint32_t* reg32(uint32_t offset) {
    return (volatile uint32_t*)(uintptr_t)(MSDC0_BASE + offset);
}

static volatile uint8_t* reg8(uint32_t offset) {
    return (volatile uint8_t*)(uintptr_t)(MSDC0_BASE + offset);
}

static volatile uint32_t* abs_reg32(uint32_t addr) {
    return (volatile uint32_t*)(uintptr_t)addr;
}

static uint32_t read32(uint32_t offset) {
    return *reg32(offset);
}

static void write32(uint32_t offset, uint32_t value) {
    *reg32(offset) = value;
}

static uint32_t read32_abs(uint32_t addr) {
    return *abs_reg32(addr);
}

static void write32_abs(uint32_t addr, uint32_t value) {
    *abs_reg32(addr) = value;
}

static void mt6592_watchdog_disable(void) {
    write32_abs(0x10007000u, 0x22000000u);
    write32_abs(0x10000500u, 0x22000000u);
}

static uint32_t bit_shift(uint32_t mask) {
    uint32_t shift = 0;
    while (shift < 32u && ((mask >> shift) & 1u) == 0) {
        ++shift;
    }
    return shift;
}

static void field_write_abs(uint32_t addr, uint32_t mask, uint32_t value) {
    uint32_t shift = bit_shift(mask);
    uint32_t reg = read32_abs(addr);
    reg &= ~mask;
    reg |= (value << shift) & mask;
    write32_abs(addr, reg);
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

static void delay_cycles(uint32_t cycles) {
    /* GPT4-anchored so the tuned settle times survive the stage2 MMU/cache
     * enable (a raw spin would run ~30-50x faster cached). */
    mt6592_delay_cycles(cycles);
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

static void msdc_fifo_clear(void) {
    set_bits(MSDC_FIFOCS, MSDC_FIFOCS_CLR);
    (void)wait_clear(MSDC_FIFOCS, MSDC_FIFOCS_CLR, 1000000u);
}

static void msdc_reset(void) {
    set_bits(MSDC_CFG, MSDC_CFG_RST);
    (void)wait_clear(MSDC_CFG, MSDC_CFG_RST, 1000000u);
    msdc_fifo_clear();
    write32(MSDC_INT, read32(MSDC_INT));
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

static void diag_capture(uint32_t stage, int err, uint32_t cmd, uint32_t arg, uint32_t status) {
    g_diag.stage = stage;
    g_diag.cmd = cmd;
    g_diag.arg = arg;
    g_diag.status = status;
    g_diag.resp = g_last_resp;
    g_diag.cfg = read32(MSDC_CFG);
    g_diag.ps = read32(MSDC_PS);
    g_diag.sdc_sts = read32(SDC_STS);
    g_diag.int_reg = read32(MSDC_INT);
    g_diag.clk_cfg2 = read32_abs(INFRA_BASE + CLK_CFG_2);
    g_diag.peri_sta = read32_abs(PERICFG_BASE + PERI_PDN0_STA);
    g_diag.pll_con0 = read32_abs(APMIXEDSYS_BASE + MSDCPLL_CON0);
    g_diag.pll_pwr = read32_abs(APMIXEDSYS_BASE + MSDCPLL_PWR_CON0);
    g_diag.gpio_clk = read32_abs(GPIO_BASE + MSDC0_GPIO_CLK);
    g_diag.gpio_cmd = read32_abs(GPIO_BASE + MSDC0_GPIO_CMD);
    g_diag.gpio_dat = read32_abs(GPIO_BASE + MSDC0_GPIO_DAT);
    g_diag.ocr = g_ocr;
    g_diag.err = err;
}

static void mt6592_msdc_prepare_clocks_and_pads(void) {
    uint32_t clk_cfg2;
    uint32_t gpio_clk = GPIO_BASE + MSDC0_GPIO_CLK;
    uint32_t gpio_cmd = GPIO_BASE + MSDC0_GPIO_CMD;
    uint32_t gpio_dat = GPIO_BASE + MSDC0_GPIO_DAT;
    uint32_t gpio_pad = GPIO_BASE + MSDC0_GPIO_PAD;

    set_bits_abs(APMIXEDSYS_BASE + MSDCPLL_PWR_CON0, PLL_PWR_ON);
    delay_cycles(1000u);
    clear_bits_abs(APMIXEDSYS_BASE + MSDCPLL_PWR_CON0, PLL_ISO_EN);
    set_bits_abs(APMIXEDSYS_BASE + MSDCPLL_CON0, PLL_EN);
    delay_cycles(20000u);

    clk_cfg2 = read32_abs(INFRA_BASE + CLK_CFG_2);
    clk_cfg2 &= ~(CLK_CFG_2_MSDC30_0_SEL_MASK | CLK_CFG_2_MSDC30_0_PDN);
    write32_abs(INFRA_BASE + CLK_CFG_2, clk_cfg2);
    write32_abs(PERICFG_BASE + PERI_PDN0_CLR, PERI_PDN0_MSDC30_0);
    delay_cycles(1000u);

    write32_abs(GPIO_BASE + MSDC0_GPIO_MODE15, 0x11111100u);
    write32_abs(GPIO_BASE + MSDC0_GPIO_MODE16, 0x11111111u);

    field_write_abs(gpio_clk, GPIO_MSDC_DRVN_MASK, 4u);
    field_write_abs(gpio_cmd, GPIO_MSDC_DRVN_MASK, 2u);
    field_write_abs(gpio_dat, GPIO_MSDC_DRVN_MASK, 2u);
    set_bits_abs(gpio_clk, GPIO_SMT | GPIO_IES);
    set_bits_abs(gpio_cmd, GPIO_SMT | GPIO_IES);
    set_bits_abs(gpio_dat, GPIO_SMT | GPIO_IES);
    field_write_abs(gpio_cmd, GPIO_R0 | GPIO_R1 | GPIO_PUPD, GPIO_R1);
    field_write_abs(gpio_dat, GPIO_R0 | GPIO_R1 | GPIO_PUPD, GPIO_R1);
    field_write_abs(gpio_pad, 0x000003ffu, 0u);
    delay_cycles(1000u);
}

/* The SDC_CFG bus-width field uses the same encoding as the EXT_CSD BUS_WIDTH
 * byte (0 = 1 bit, 1 = 4 bits, 2 = 8 bits), so one value drives both halves. */
static void msdc_set_host_bus_width(uint32_t width_code) {
    uint32_t v = read32(SDC_CFG);
    v &= ~SDC_CFG_BUSWIDTH_MASK;
    v |= (width_code << 16) & SDC_CFG_BUSWIDTH_MASK;
    write32(SDC_CFG, v);
}

static void msdc_set_sample_edge(uint32_t dspl) {
    uint32_t v = read32(MSDC_IOCON) & ~MSDC_IOCON_DSPL;
    if (dspl) v |= MSDC_IOCON_DSPL;
    write32(MSDC_IOCON, v);
}

static int msdc_set_clock(uint32_t hz) {
    uint32_t mode = 0;
    uint32_t div = 0;
    uint32_t cfg;

    if (hz == 0) {
        clear_bits(MSDC_CFG, MSDC_CFG_CKPDN);
        return MT6592_MSDC_OK;
    }

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

    if (wait_set(MSDC_CFG, MSDC_CFG_CKSTB, 1000000u) != MT6592_MSDC_OK) {
        diag_capture(MSDC_DIAG_STAGE_SET_CLOCK, MT6592_MSDC_ERR_TIMEOUT, 0, hz, read32(MSDC_CFG));
        return MT6592_MSDC_ERR_TIMEOUT;
    }
    return MT6592_MSDC_OK;
}

static int msdc_init_hw(void) {
    mt6592_msdc_prepare_clocks_and_pads();

    write32(EMMC_IOCON, 1u);
    delay_cycles(1000u);
    write32(EMMC_IOCON, 0u);

    set_bits(MSDC_CFG, MSDC_CFG_MODE | MSDC_CFG_PIO);
    msdc_reset();

    clear_bits(MSDC_PS, BIT(0));
    write32(MSDC_INT, read32(MSDC_INT));
    write32(MSDC_INTEN, 0x0001ff7bu);

    write32(MSDC_IOCON, 0u);
    write32(MSDC_DAT_RDDLY0, 0u);
    write32(MSDC_DAT_RDDLY1, 0u);
    write32(MSDC_HW_DBG, 0u);
    write32(MSDC_VERSION, 0u);
    write32(MSDC_PAD_TUNE, 0u);
    write32(MSDC_PAD_TUNE0, 0u);
    write32(MSDC_PAD_TUNE1, 0u);

    write32(MSDC_PATCH_BIT0, 0x403c0006u);
    write32(MSDC_PATCH_BIT1, 0xffe24340u);
    write32(MSDC_PATCH_BIT2, 0x3489180du);
    write32(EMMC50_CFG0, read32(EMMC50_CFG0));

    write32(SDC_CFG, 0x000a0000u);
    write32(SDC_CFG, 0x00080000u);
    msdc_set_host_bus_width(EMMC_BUS_WIDTH_1BIT);
    write32(SDC_CFG, 0u);
    return msdc_set_clock(MSDC_CLOCK_ID_HZ);
}

static int msdc_wait_cmd_ready(int check_data) {
    if (wait_clear(SDC_STS, SDC_STS_CMDBUSY, 20000000u) != MT6592_MSDC_OK) {
        diag_capture(MSDC_DIAG_STAGE_CMD_READY, MT6592_MSDC_ERR_TIMEOUT, 0, 0, read32(SDC_STS));
        msdc_reset();
        return MT6592_MSDC_ERR_TIMEOUT;
    }
    if (check_data || g_last_write) {
        if (wait_set(MSDC_PS, MSDC_PS_DAT0, 20000000u) != MT6592_MSDC_OK) {
            diag_capture(MSDC_DIAG_STAGE_CMD_READY, MT6592_MSDC_ERR_TIMEOUT, 0, 0, read32(MSDC_PS));
            msdc_reset();
            return MT6592_MSDC_ERR_TIMEOUT;
        }
    }
    return MT6592_MSDC_OK;
}

static uint32_t build_raw_cmd(uint32_t opcode, uint32_t resp, uint32_t blocks, uint32_t block_size, int write) {
    uint32_t dtype = 0;
    uint32_t raw = (opcode & 0x3fu) | ((resp & 0x7u) << 7);
    if (blocks != 0 || opcode == 16u) {
        dtype = blocks > 1 ? 2u : 1u;
        raw |= (block_size & 0xfffu) << 16;
        if (blocks != 0) {
            raw |= dtype << 11;
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
    uint32_t guard = 50000000u;
    while (guard--) {
        uint32_t status = read32(MSDC_INT);
        if ((guard & 0xfffffu) == 0) mt6592_watchdog_disable();
        if (status != 0) write32(MSDC_INT, status);
        status &= k_data_ints;

        if (status & (MSDC_INT_DATCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR)) return MT6592_MSDC_ERR_DATA;
        if (status & MSDC_INT_DATTMO) return MT6592_MSDC_ERR_TIMEOUT;
        if (status & MSDC_INT_XFER_COMPL) return size == 0 ? MT6592_MSDC_OK : MT6592_MSDC_ERR_DATA;

        /* Keep refilling while the FIFO has room instead of going back around
         * the interrupt poll per 128-byte chunk -- see pio_read(). */
        while (size != 0) {
            uint32_t chunk = min_u32(size, MSDC_FIFO_SIZE);
            if ((MSDC_FIFO_SIZE - fifo_tx_count()) < chunk) break;
            fifo_write(ptr, chunk);
            ptr += chunk;
            size -= chunk;
        }
    }
    return MT6592_MSDC_ERR_TIMEOUT;
}

static int pio_read(uint8_t* ptr, uint32_t size) {
    uint32_t guard = 50000000u;
    while (guard--) {
        uint32_t status = read32(MSDC_INT);
        if ((guard & 0xfffffu) == 0) mt6592_watchdog_disable();
        if (status != 0) write32(MSDC_INT, status);
        status &= k_data_ints;

        if (status & (MSDC_INT_DATCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR)) return MT6592_MSDC_ERR_DATA;
        if (status & MSDC_INT_DATTMO) return MT6592_MSDC_ERR_TIMEOUT;

        /* Drain everything the FIFO is holding before going back to the
         * interrupt register. At 8-bit/50 MHz a 128-byte chunk arrives every
         * ~2.4us, which is the same order as one MMIO round trip on this
         * peripheral bus, so re-reading MSDC_INT per chunk roughly doubled the
         * register traffic on the critical path. XFER_COMPL is still evaluated
         * from the `status` sampled *before* the drain, so a completion that
         * races the last chunk cannot make us return with data left behind. */
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

static int msdc_cmd(uint32_t opcode, uint32_t arg, uint32_t resp, uint8_t* data, uint32_t blocks, int write) {
    uint32_t status;
    uint32_t raw;
    uint32_t guard;
    int err;

    err = msdc_wait_cmd_ready(blocks != 0 || resp == 7u);
    if (err != MT6592_MSDC_OK) return err;

    if (fifo_tx_count() != 0 || fifo_rx_count() != 0) {
        msdc_reset();
        msdc_fifo_clear();
    }

    write32(MSDC_INT, read32(MSDC_INT));
    write32(SDC_BLK_NUM, blocks);
    raw = build_raw_cmd(opcode, resp, blocks, EMMC_BLOCK_SIZE, write);
    write32(SDC_ARG, arg);
    write32(SDC_CMD, raw);

    status = 0;
    guard = 20000000u;
    while (guard--) {
        status = read32(MSDC_INT);
        if (status & k_cmd_ints) break;
    }
    if (guard == 0) status = MSDC_INT_CMDTMO;

    if (resp == 2u) {
        g_last_resp = read32(SDC_RESP3);
    } else if (resp != 0u) {
        g_last_resp = read32(SDC_RESP0);
    } else {
        g_last_resp = 0;
    }

    write32(MSDC_INT, status & k_cmd_ints);

    if ((status & MSDC_INT_CMDRDY) == 0) {
        diag_capture(MSDC_DIAG_STAGE_CMD, (status & MSDC_INT_CMDTMO) ? MT6592_MSDC_ERR_TIMEOUT : MT6592_MSDC_ERR_CMD,
                     opcode, arg, status);
        msdc_reset();
        return (status & MSDC_INT_CMDTMO) ? MT6592_MSDC_ERR_TIMEOUT : MT6592_MSDC_ERR_CMD;
    }
    if ((status & MSDC_INT_RSPCRCERR) != 0 && resp != 3u) {
        diag_capture(MSDC_DIAG_STAGE_CMD, MT6592_MSDC_ERR_CMD, opcode, arg, status);
        msdc_reset();
        return MT6592_MSDC_ERR_CMD;
    }

    if (blocks != 0) {
        err = write ? pio_write(data, blocks * EMMC_BLOCK_SIZE) : pio_read(data, blocks * EMMC_BLOCK_SIZE);
        if (err != MT6592_MSDC_OK) {
            diag_capture(MSDC_DIAG_STAGE_DATA, err, opcode, arg, read32(MSDC_INT));
            msdc_reset();
            msdc_fifo_clear();
            return err;
        }
    }

    g_last_write = write;
    return MT6592_MSDC_OK;
}

static int emmc_cmd(uint32_t opcode, uint32_t arg, uint32_t resp) {
    return msdc_cmd(opcode, arg, resp, 0, 0, 0);
}

static int emmc_wait_ready(void) {
    for (uint32_t i = 0; i < 20000u; ++i) {
        int err = emmc_cmd(13u, EMMC_RCA << 16, 1u);
        uint32_t state;
        if ((i & 0x3fu) == 0) mt6592_watchdog_disable();
        if (err != MT6592_MSDC_OK) {
            diag_capture(MSDC_DIAG_STAGE_READY, err, 13u, EMMC_RCA << 16, read32(MSDC_INT));
            return err;
        }
        state = (g_last_resp >> 9) & 0xfu;
        if ((g_last_resp & BIT(8)) != 0 && state != 7u) return MT6592_MSDC_OK;
        delay_cycles(2000u);
    }
    diag_capture(MSDC_DIAG_STAGE_READY, MT6592_MSDC_ERR_TIMEOUT, 13u, EMMC_RCA << 16, read32(MSDC_INT));
    return MT6592_MSDC_ERR_TIMEOUT;
}

static int emmc_part_access(uint32_t part, uint32_t* access) {
    if (part == 0u || part == MT6592_EMMC_USER_PART) {
        *access = EMMC_PARTITION_ACCESS_USER;
        return MT6592_MSDC_OK;
    }
    if (part == MT6592_EMMC_BOOT1_PART) {
        *access = EMMC_PARTITION_ACCESS_BOOT1;
        return MT6592_MSDC_OK;
    }
    if (part == MT6592_EMMC_BOOT2_PART) {
        *access = EMMC_PARTITION_ACCESS_BOOT2;
        return MT6592_MSDC_OK;
    }
    return MT6592_MSDC_ERR_UNSUPPORTED;
}

static int emmc_boot_enable_bits(uint32_t part, uint32_t* bits) {
    if (part == MT6592_EMMC_BOOT1_PART) {
        *bits = EMMC_PARTITION_ACCESS_BOOT1 << 3;
        return MT6592_MSDC_OK;
    }
    if (part == MT6592_EMMC_BOOT2_PART) {
        *bits = EMMC_PARTITION_ACCESS_BOOT2 << 3;
        return MT6592_MSDC_OK;
    }
    if (part == 0u || part == MT6592_EMMC_USER_PART) {
        *bits = 0u;
        return MT6592_MSDC_OK;
    }
    return MT6592_MSDC_ERR_UNSUPPORTED;
}

static int emmc_read_ext_csd(void) {
    int err = msdc_cmd(8u, 0u, 1u, g_ext_csd, 1u, 0);
    if (err != MT6592_MSDC_OK) {
        diag_capture(MSDC_DIAG_STAGE_SWITCH, err, 8u, 0u, read32(MSDC_INT));
        return err;
    }
    return MT6592_MSDC_OK;
}

/* CMD6 WRITE_BYTE against one EXT_CSD index. R1b, so the device signals busy on
 * DAT0 afterwards; poll it out with CMD13 before anything else is issued. */
static int emmc_switch_byte(uint32_t index, uint32_t value) {
    uint32_t arg = (EMMC_SWITCH_ACCESS_WRITE_BYTE << 24) | ((index & 0xffu) << 16) | ((value & 0xffu) << 8);
    int err = emmc_cmd(6u, arg, 1u);
    if (err != MT6592_MSDC_OK) {
        diag_capture(MSDC_DIAG_STAGE_SWITCH, err, 6u, arg, read32(MSDC_INT));
        return err;
    }
    err = emmc_wait_ready();
    if (err != MT6592_MSDC_OK) {
        diag_capture(MSDC_DIAG_STAGE_SWITCH, err, 6u, arg, read32(MSDC_INT));
        return err;
    }
    return MT6592_MSDC_OK;
}

static int emmc_write_partition_config(uint32_t value) {
    int err = emmc_switch_byte(EMMC_EXT_CSD_PARTITION_CONFIG, value);
    if (err != MT6592_MSDC_OK) return err;
    /* Keep the tuner's reference image in step with the device: it is compared
     * byte-for-byte, and PARTITION_CONFIG is the one field the driver itself
     * rewrites after the reference is taken. */
    g_ext_csd_ref[EMMC_EXT_CSD_PARTITION_CONFIG] = (uint8_t)value;
    g_active_part = value & EMMC_EXT_CSD_PARTITION_ACCESS_MASK;
    return MT6592_MSDC_OK;
}

static int emmc_switch_part(uint32_t part) {
    uint32_t access;
    uint32_t value;
    int err;

    err = emmc_part_access(part, &access);
    if (err != MT6592_MSDC_OK) return err;
    if (g_active_part == access) return MT6592_MSDC_OK;

    err = emmc_read_ext_csd();
    if (err != MT6592_MSDC_OK) return err;

    value = (uint32_t)g_ext_csd[EMMC_EXT_CSD_PARTITION_CONFIG];
    value = (value & ~EMMC_EXT_CSD_PARTITION_ACCESS_MASK) | access;
    return emmc_write_partition_config(value);
}

/* ---- bus mode negotiation ------------------------------------------------
 *
 * Identification has to happen at 1 bit / 260 kHz, but leaving the bus there is
 * what made the eMMC feel wedged: 1 lane at 12.5 MHz is ~1.5 MB/s, so a single
 * multi-megabyte read is seconds of wall time with the whole cooperative
 * scheduler parked behind it. eMMC 4.x parts on this board are wired 8 lanes
 * wide (the pinmux writes MODE15/MODE16 for all of DAT0..7) and accept 52 MHz
 * once HS_TIMING is set -- about 33x the bandwidth.
 *
 * Nothing here is taken on trust. Pad delays for this board are not documented
 * anywhere we can read, so each candidate mode is *proved* by reading EXT_CSD
 * back over it and comparing all 512 bytes against a copy taken at the slow,
 * known-good identification settings. A mis-sampled data window shows up either
 * as a CRC error or as garbled bytes, and both are caught. Modes are tried best
 * first; the first one that verifies wins, and if every one fails the driver
 * ends up exactly where it used to be. */

static int emmc_bus_verify(uint32_t width_code, uint32_t hs_timing) {
    int err = emmc_read_ext_csd();
    if (err != MT6592_MSDC_OK) return err;
    if (g_ext_csd[EMMC_EXT_CSD_BUS_WIDTH] != (uint8_t)width_code) return MT6592_MSDC_ERR_DATA;
    if (g_ext_csd[EMMC_EXT_CSD_HS_TIMING] != (uint8_t)hs_timing) return MT6592_MSDC_ERR_DATA;
    for (uint32_t i = 0; i < EMMC_BLOCK_SIZE; ++i) {
        if (i == EMMC_EXT_CSD_BUS_WIDTH || i == EMMC_EXT_CSD_HS_TIMING) continue;
        if (g_ext_csd[i] != g_ext_csd_ref[i]) return MT6592_MSDC_ERR_DATA;
    }
    return MT6592_MSDC_OK;
}

/* Put both halves of the link back to the settings identification ran at. The
 * device side goes first and over CMD6/CMD13, which ride the command line and
 * so are unaffected by however wrong the data lanes currently are. */
static void emmc_bus_restore_safe(void) {
    (void)msdc_set_clock(MSDC_CLOCK_SAFE_HZ);
    msdc_set_sample_edge(0u);
    if (g_bus_width_code != EMMC_BUS_WIDTH_1BIT) {
        if (emmc_switch_byte(EMMC_EXT_CSD_BUS_WIDTH, EMMC_BUS_WIDTH_1BIT) == MT6592_MSDC_OK) {
            g_bus_width_code = EMMC_BUS_WIDTH_1BIT;
        }
    }
    msdc_set_host_bus_width(EMMC_BUS_WIDTH_1BIT);
    if (g_bus_hs != 0u) {
        if (emmc_switch_byte(EMMC_EXT_CSD_HS_TIMING, 0u) == MT6592_MSDC_OK) g_bus_hs = 0u;
    }
    g_bus_width_bits = 1u;
    g_bus_clock_hz = MSDC_CLOCK_SAFE_HZ;
}

static int emmc_bus_try(uint32_t width_code, uint32_t width_bits, uint32_t hz, uint32_t hs_timing,
                        uint32_t dspl) {
    int err;

    /* HS timing goes in at the *old* clock: the device has to be told it is
     * about to be driven fast before it actually is. */
    if (hs_timing != g_bus_hs) {
        err = emmc_switch_byte(EMMC_EXT_CSD_HS_TIMING, hs_timing);
        if (err != MT6592_MSDC_OK) return err;
        g_bus_hs = hs_timing;
    }
    err = msdc_set_clock(hz);
    if (err != MT6592_MSDC_OK) return err;

    /* Width on the device, then immediately on the host. No data command may be
     * issued while the two disagree -- it would be latched off the wrong lanes. */
    if (width_code != g_bus_width_code) {
        err = emmc_switch_byte(EMMC_EXT_CSD_BUS_WIDTH, width_code);
        if (err != MT6592_MSDC_OK) return err;
        g_bus_width_code = width_code;
    }
    msdc_set_host_bus_width(width_code);
    msdc_set_sample_edge(dspl);

    err = emmc_bus_verify(width_code, hs_timing);
    if (err != MT6592_MSDC_OK) return err;

    g_bus_width_bits = width_bits;
    g_bus_clock_hz = hz;
    return MT6592_MSDC_OK;
}

static void emmc_tune_bus(void) {
    /* Ordered by lanes x clock, best first. HS timing is required above 26 MHz;
     * below it the legacy interface is one fewer thing to get wrong. */
    static const struct {
        uint32_t width_code;
        uint32_t width_bits;
        uint32_t hz;
        uint32_t hs;
    } k_ladder[] = {
        {EMMC_BUS_WIDTH_8BIT, 8u, MSDC_CLOCK_HS_HZ,     1u},  /* 8 x 50   MHz */
        {EMMC_BUS_WIDTH_4BIT, 4u, MSDC_CLOCK_HS_HZ,     1u},  /* 4 x 50   MHz */
        {EMMC_BUS_WIDTH_8BIT, 8u, MSDC_CLOCK_LEGACY_HZ, 0u},  /* 8 x 25   MHz */
        {EMMC_BUS_WIDTH_4BIT, 4u, MSDC_CLOCK_LEGACY_HZ, 0u},  /* 4 x 25   MHz */
        {EMMC_BUS_WIDTH_8BIT, 8u, MSDC_CLOCK_SAFE_HZ,   0u},  /* 8 x 12.5 MHz */
        {EMMC_BUS_WIDTH_4BIT, 4u, MSDC_CLOCK_SAFE_HZ,   0u},  /* 4 x 12.5 MHz */
        {EMMC_BUS_WIDTH_1BIT, 1u, MSDC_CLOCK_LEGACY_HZ, 0u},  /* 1 x 25   MHz */
    };
    const uint32_t device_type = g_ext_csd_ref[EMMC_EXT_CSD_DEVICE_TYPE];
    const uint32_t hs_ok = (device_type & (EMMC_DEVICE_TYPE_HS_26 | EMMC_DEVICE_TYPE_HS_52)) != 0u;
    const uint32_t hs52_ok = (device_type & EMMC_DEVICE_TYPE_HS_52) != 0u;

    for (uint32_t i = 0; i < (sizeof(k_ladder) / sizeof(k_ladder[0])); ++i) {
        if (k_ladder[i].hs != 0u && !hs_ok) continue;
        if (k_ladder[i].hz > MSDC_CLOCK_LEGACY_HZ && !hs52_ok) continue;
        /* Both data sample edges. This is the only tuning axis available
         * without a per-board pad-delay table, and it is what decides whether
         * the 50 MHz rungs are reachable at all. */
        for (uint32_t dspl = 0; dspl < 2u; ++dspl) {
            if (emmc_bus_try(k_ladder[i].width_code, k_ladder[i].width_bits, k_ladder[i].hz,
                             k_ladder[i].hs, dspl) == MT6592_MSDC_OK) {
                g_bus_edge = dspl;
                return;
            }
            emmc_bus_restore_safe();
        }
    }

    /* Nothing verified: the link is back on the identification settings, which
     * are the ones this driver shipped with. Prove that much still works before
     * telling the storage layer the device is usable. */
    emmc_bus_restore_safe();
    g_bus_edge = 0u;
    if (emmc_bus_verify(EMMC_BUS_WIDTH_1BIT, 0u) != MT6592_MSDC_OK) {
        diag_capture(MSDC_DIAG_STAGE_TUNE, MT6592_MSDC_ERR_DATA, 8u, 0u, read32(MSDC_INT));
    }
}

int mt6592_emmc_user_init(void) {
    int err;
    uint32_t ocr_arg = 0x40018000u;

    if (g_initialized) return MT6592_MSDC_OK;

    g_last_write = 0;
    g_active_part = 0xffffffffu;
    err = msdc_init_hw();
    if (err != MT6592_MSDC_OK) return err;

    (void)emmc_cmd(0u, 0u, 0u);
    (void)emmc_cmd(1u, 0u, 3u);
    (void)emmc_cmd(0u, 0u, 0u);
    for (uint32_t i = 0; i < 200u; ++i) {
        err = emmc_cmd(1u, ocr_arg, 3u);
        if (err == MT6592_MSDC_OK && (g_last_resp & BIT(31)) != 0) {
            g_ocr = g_last_resp;
            break;
        }
        delay_cycles(50000u);
    }
    if ((g_ocr & BIT(31)) == 0) {
        diag_capture(MSDC_DIAG_STAGE_OCR, MT6592_MSDC_ERR_TIMEOUT, 1u, ocr_arg, read32(MSDC_INT));
        return MT6592_MSDC_ERR_TIMEOUT;
    }
    g_high_capacity = (g_ocr & BIT(30)) != 0;

    err = emmc_cmd(2u, 0u, 2u);
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_cmd(3u, EMMC_RCA << 16, 1u);
    if (err != MT6592_MSDC_OK) return err;
    (void)emmc_cmd(9u, EMMC_RCA << 16, 2u);
    err = emmc_cmd(7u, EMMC_RCA << 16, 7u);
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_wait_ready();
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_cmd(16u, EMMC_BLOCK_SIZE, 1u);
    if (err != MT6592_MSDC_OK) return err;

    /* Off the 260 kHz identification clock, but still 1-bit: these are the
     * settings the reference EXT_CSD below is taken at, and the ones the bus
     * tuner falls back to if nothing faster verifies. */
    (void)msdc_set_clock(MSDC_CLOCK_SAFE_HZ);
    g_bus_clock_hz = MSDC_CLOCK_SAFE_HZ;
    g_bus_width_code = EMMC_BUS_WIDTH_1BIT;
    g_bus_width_bits = 1u;
    g_bus_hs = 0u;
    err = emmc_wait_ready();
    if (err != MT6592_MSDC_OK) return err;

    g_initialized = 1;
    g_active_part = EMMC_PARTITION_ACCESS_USER;

    err = emmc_read_ext_csd();
    if (err == MT6592_MSDC_OK) {
        for (uint32_t i = 0; i < EMMC_BLOCK_SIZE; ++i) g_ext_csd_ref[i] = g_ext_csd[i];
        emmc_tune_bus();
    }
    return MT6592_MSDC_OK;
}

int mt6592_emmc_read_part(uint32_t part, uint64_t byte_offset, uint8_t* data, uint32_t size) {
    /* Open the input path before the transfer, not after: with no battery
     * fitted a bulk eMMC burst is one of the loads that collapsed the rail. */
    if (&mt6592_pmic_power_hold) mt6592_pmic_power_hold();
    int err;
    uint64_t block;

    if ((byte_offset & (uint64_t)(EMMC_BLOCK_SIZE - 1u)) != 0 || (size & (EMMC_BLOCK_SIZE - 1u)) != 0) {
        return MT6592_MSDC_ERR_ALIGN;
    }
    err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_switch_part(part);
    if (err != MT6592_MSDC_OK) return err;

    block = byte_offset >> 9;
    for (uint32_t pos = 0; pos < size;) {
        uint32_t blocks = (size - pos) / EMMC_BLOCK_SIZE;
        uint32_t opcode;
        uint32_t arg;

        /* One CMD18 cannot be preempted, so the burst is capped by whether
         * there is a cooperative scheduler above us to starve. */
        const uint32_t burst_cap =
            (&minos_machine64_io_yield) ? MSDC_READ_BURST_BLOCKS : MSDC_READ_BURST_BLOCKS_STANDALONE;
        if (blocks > burst_cap) blocks = burst_cap;
        mt6592_watchdog_disable();
        if (g_high_capacity) {
            arg = (uint32_t)(block + (uint64_t)(pos >> 9));
        } else {
            arg = (uint32_t)(byte_offset + (uint64_t)pos);
        }

        /* CMD17 for a lone block, otherwise an open-ended CMD18 burst that is
         * terminated by CMD12 — the mirror of the CMD25 write path below. */
        opcode = blocks > 1u ? 18u : 17u;
        err = msdc_cmd(opcode, arg, 1u, data + pos, blocks, 0);
        if (err == MT6592_MSDC_OK && blocks > 1u) {
            err = emmc_cmd(12u, 0u, 1u);
        }
        if (err != MT6592_MSDC_OK) {
            diag_capture(MSDC_DIAG_STAGE_READ, err, opcode, arg, read32(MSDC_INT));
            return err;
        }
        pos += blocks * EMMC_BLOCK_SIZE;
        if (pos < size) msdc_cooperative_yield();
    }
    return MT6592_MSDC_OK;
}

int mt6592_emmc_read_user(uint64_t byte_offset, uint8_t* data, uint32_t size) {
    return mt6592_emmc_read_part(MT6592_EMMC_USER_PART, byte_offset, data, size);
}

int mt6592_emmc_write_part(uint32_t part, uint64_t byte_offset, const uint8_t* data, uint32_t size) {
    /* Open the input path before the transfer, not after: with no battery
     * fitted a bulk eMMC burst is one of the loads that collapsed the rail. */
    if (&mt6592_pmic_power_hold) mt6592_pmic_power_hold();
    int err;
    uint64_t block;

    if ((byte_offset & (uint64_t)(EMMC_BLOCK_SIZE - 1u)) != 0 || (size & (EMMC_BLOCK_SIZE - 1u)) != 0) {
        return MT6592_MSDC_ERR_ALIGN;
    }
    err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_switch_part(part);
    if (err != MT6592_MSDC_OK) return err;

    block = byte_offset >> 9;
    for (uint32_t pos = 0; pos < size;) {
        uint32_t blocks = (size - pos) / EMMC_BLOCK_SIZE;
        uint32_t opcode;
        uint32_t arg;

        if (blocks > MSDC_MAX_BURST_BLOCKS) blocks = MSDC_MAX_BURST_BLOCKS;
        mt6592_watchdog_disable();
        if (g_high_capacity) {
            arg = (uint32_t)(block + (uint64_t)(pos >> 9));
        } else {
            arg = (uint32_t)(byte_offset + (uint64_t)pos);
        }

        /* CMD24 for a lone block, otherwise an open-ended CMD25 burst. */
        opcode = blocks > 1u ? 25u : 24u;
        err = msdc_cmd(opcode, arg, 1u, (uint8_t*)(uintptr_t)(data + pos), blocks, 1);
        if (err != MT6592_MSDC_OK) {
            diag_capture(MSDC_DIAG_STAGE_WRITE, err, opcode, arg, read32(MSDC_INT));
            return err;
        }
        if (blocks > 1u) {
            /* Terminate the open-ended multi-block write before polling busy. */
            err = emmc_cmd(12u, 0u, 1u);
            if (err != MT6592_MSDC_OK) {
                diag_capture(MSDC_DIAG_STAGE_WRITE, err, 12u, arg, read32(MSDC_INT));
                return err;
            }
        }
        err = emmc_wait_ready();
        if (err != MT6592_MSDC_OK) return err;
        pos += blocks * EMMC_BLOCK_SIZE;
        if (pos < size) msdc_cooperative_yield();
    }
    return MT6592_MSDC_OK;
}

int mt6592_emmc_write_user(uint64_t byte_offset, const uint8_t* data, uint32_t size) {
    return mt6592_emmc_write_part(MT6592_EMMC_USER_PART, byte_offset, data, size);
}

int mt6592_emmc_enable_boot_part(uint32_t part, uint32_t boot_ack) {
    uint32_t boot_bits;
    uint32_t value;
    int err;

    err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_boot_enable_bits(part, &boot_bits);
    if (err != MT6592_MSDC_OK) return err;
    err = emmc_read_ext_csd();
    if (err != MT6592_MSDC_OK) return err;

    value = (uint32_t)g_ext_csd[EMMC_EXT_CSD_PARTITION_CONFIG];
    value &= ~(EMMC_EXT_CSD_BOOT_PARTITION_ENABLE_MASK | EMMC_EXT_CSD_BOOT_ACK);
    value |= boot_bits;
    if (boot_ack != 0u && boot_bits != 0u) value |= EMMC_EXT_CSD_BOOT_ACK;
    return emmc_write_partition_config(value);
}

uint64_t mt6592_emmc_user_capacity_sectors(void) {
    int err = mt6592_emmc_user_init();
    if (err != MT6592_MSDC_OK) return 0;
    err = emmc_read_ext_csd();
    if (err != MT6592_MSDC_OK) return 0;
    return (uint64_t)g_ext_csd[EMMC_EXT_CSD_SEC_COUNT] |
        ((uint64_t)g_ext_csd[EMMC_EXT_CSD_SEC_COUNT + 1u] << 8) |
        ((uint64_t)g_ext_csd[EMMC_EXT_CSD_SEC_COUNT + 2u] << 16) |
        ((uint64_t)g_ext_csd[EMMC_EXT_CSD_SEC_COUNT + 3u] << 24);
}

uint32_t mt6592_emmc_last_response(void) {
    return g_last_resp;
}

uint32_t mt6592_emmc_ocr(void) {
    return g_ocr;
}

uint32_t mt6592_emmc_bus_width_bits(void) {
    return g_bus_width_bits;
}

uint32_t mt6592_emmc_bus_clock_hz(void) {
    return g_bus_clock_hz;
}

uint32_t mt6592_emmc_bus_mode(void) {
    return (g_bus_hs != 0u ? 1u : 0u) | (g_bus_edge != 0u ? 2u : 0u);
}

const mt6592_msdc_diag_t* mt6592_msdc_diag(void) {
    return &g_diag;
}
