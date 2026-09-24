/*
 * mt6592_wifi_sdio.c — integrated MT6592 CONSYS WLAN power/probe stage.
 *
 * The filename is kept to avoid disturbing the existing source lists. The
 * sequence follows the stock J36 MT6592 WMT and MTCMOS sources: MT6323 VCN
 * rails, CONSYS MTCMOS, INFRA_CONNMCU clock, then the CONSYS chip-ID probe.
 */

#include "mt6592_wifi_sdio.h"

#include "mt6592_bootstatus.h"
#include "mt6592_delay.h"
#include "mt6592_pwrap.h"
#include "mt6592_uart.h"

#include <stdint.h>

#define BIT(n) (1u << (n))

#ifndef MVII_MT6592_WIFI_ENABLE_TRANSPORT
#    define MVII_MT6592_WIFI_ENABLE_TRANSPORT 0
#endif

/*
 * The register map, all of it from MediaTek's own MT6592 platform sources
 * rather than from a datasheet or a read of the running chip.
 *
 * The offsets are in mediatek/platform/mt6592/kernel/core/include/mach/, and the
 * bases there are the kernel's virtual ones (INFRA_BASE 0xF0000000,
 * INFRACFG_AO_BASE 0xF0001000, SPM_BASE 0xF0006000, CONN_MCU_CONFIG_BASE
 * 0xF8070000 -- mt_reg_base.h:14,17,32,408). We run before any MMU mapping, so
 * the physical aliases below are those with the 0xF0/0xF8 nibble replaced by
 * 0x10/0x18, which is the identity this platform's ioremap sets up.
 *
 *     INFRA_PDN_CLR/STA        mt_clkmgr.h:69-70
 *     TOPAXI_PROT_EN/STA1      mt_clkmgr.h:72-73
 *     SPM_CONN_PWR_CON         mt_spm.h:41
 *     SPM_PWR_STATUS/_S        mt_spm.h:102-103
 *     CONSYS_CHIP_ID           mtk_wcn_consys_hw.h:70
 *
 * CONSYS_EMI_MAP is the one that looks wrong and is not: the connectivity
 * header calls it TOPCKGEN_BASE + 0x1310 (mtk_wcn_consys_hw.h:120) and defines
 * TOPCKGEN_BASE as INFRA_BASE (:50), so it is 0xF0001310 -- the same register
 * this file reaches as INFRACFG_AO_BASE + 0x0310. Two names, one address.
 *
 * The power-control bits are mt_spm_mtcmos.c:774-795 verbatim, and the one
 * renamed here is CONN_SRAM_PDN, which stock calls MD_SRAM_PDN (:786) -- the
 * same bit 8 of the same CONN register, under a name inherited from the modem
 * block it was copied from.
 *
 * INFRA_CONNMCU_GATE is derived rather than quoted, because stock never writes
 * it directly: it calls enable_clock(MT_CG_INFRA_CONNMCU) and the clock manager
 * resolves that. MT_CG_INFRA_CONNMCU is 44 (mt_clkmgr.h:166) and the CG_INFRA
 * group starts at 32 (:157, the first INFRA id), so the bit is 44 - 32 = 12.
 * That group's descriptor (mt_clkmgr.c:2369-2375) names INFRA_PDN_CLR as its
 * "enable" address and INFRA_PDN_STA as its status, and its mask 0x0091BFE3 has
 * bit 12 set -- which is why enabling is a write of the bit to _CLR and the
 * check is for a zero in _STA.
 */
enum
{
    INFRACFG_AO_BASE     = 0x10001000u,
    SPM_BASE             = 0x10006000u,
    CONN_MCU_CONFIG_BASE = 0x18070000u,

    INFRA_PDN_CLR    = INFRACFG_AO_BASE + 0x0044u,
    INFRA_PDN_STA    = INFRACFG_AO_BASE + 0x0048u,
    TOPAXI_PROT_EN   = INFRACFG_AO_BASE + 0x0220u,
    TOPAXI_PROT_STA1 = INFRACFG_AO_BASE + 0x0228u,
    CONSYS_EMI_MAP   = INFRACFG_AO_BASE + 0x0310u,
    SPM_CONN_PWR_CON = SPM_BASE + 0x0280u,
    SPM_PWR_STATUS   = SPM_BASE + 0x060cu,
    SPM_PWR_STATUS_S = SPM_BASE + 0x0610u,
    CONSYS_CHIP_ID   = CONN_MCU_CONFIG_BASE + 0x0008u,

    /*
     * The four MT6323 registers the connectivity rails live in, from
     * mediatek/platform/mt6592/kernel/core/include/mach/upmu_hw.h:234-249, with
     * PMIC_REG_BASE dropped -- pwrap addresses the die directly.
     *
     * Which bit is in which register is not guessable from the field names,
     * because VCN33 is split across two: BT's enable and mux are in CON16
     * alongside the shared VOSEL, and WiFi's are in CON17. The accessors settle
     * it -- each one names its register in its own body
     * (drivers/power/upmu_common.c):
     *
     *     upmu_set_rg_vcn33_vosel        ANALDO_CON16  [3:2]   :11192
     *     upmu_set_vcn33_on_ctrl_bt      ANALDO_CON16  bit 5    :11220
     *     upmu_set_rg_vcn33_en_bt        ANALDO_CON16  bit 7    :11246
     *     upmu_set_rg_vcn33_en_wifi      ANALDO_CON17  bit 12   :11285
     *     upmu_set_vcn33_on_ctrl_wifi    ANALDO_CON17  bit 14   :11298
     *     upmu_set_rg_vcn28_en           ANALDO_CON19  bit 12   :11405
     *     upmu_set_vcn28_on_ctrl         ANALDO_CON19  bit 14   :11418
     *     upmu_set_vcn_1v8_lp_mode_set   DIGLDO_CON11  bit 1    :12317
     *     upmu_set_rg_vcn_1v8_en         DIGLDO_CON11  bit 14   :12359
     *
     * and the shifts are upmu_hw.h:2086-2121 and :2250-2257. Two of those names
     * would mislead if taken at face value: VCN33_ON_CTRL_BT is bit 5 while
     * VCN33_ON_CTRL_WIFI is bit 14, and VCN_1V8's low-power bit is 1 while its
     * enable is 14 -- neither pair is symmetric.
     */
    PMIC_ANALDO_CON16 = 0x0416u,
    PMIC_ANALDO_CON17 = 0x0418u,
    PMIC_ANALDO_CON19 = 0x041cu,
    PMIC_DIGLDO_CON11 = 0x0512u,

    CONN_PWR_STA_MASK  = BIT(1),
    CONN_PROT_MASK     = 0x0104u,
    INFRA_CONNMCU_GATE = BIT(12),
    PWR_RST_B          = BIT(0),
    PWR_ISO            = BIT(1),
    PWR_ON             = BIT(2),
    PWR_ON_S           = BIT(3),
    PWR_CLK_DIS        = BIT(4),
    CONN_SRAM_PDN      = BIT(8),

    PMIC_VCN18_LP_MODE      = BIT(1),
    PMIC_VCN18_ENABLE       = BIT(14),
    PMIC_VCN28_ENABLE       = BIT(12),
    PMIC_VCN28_ON_CTRL      = BIT(14),
    PMIC_VCN33_BT_ON_CTRL   = BIT(5),
    PMIC_VCN33_BT_ENABLE    = BIT(7),
    PMIC_VCN33_WIFI_ENABLE  = BIT(12),
    PMIC_VCN33_WIFI_ON_CTRL = BIT(14),

    /*
     * ANALDO_CON16[3:2] is RG_VCN33_VOSEL -- ONE selector for ONE regulator, even
     * though VCN33 has two independent enable/mux paths (BT in 0x416, WiFi in
     * 0x418). PMIC_RG_VCN33_VOSEL_MASK/SHIFT in upmu_hw.h are 0x3 and 2, and
     * dct_pmic_VCN33_sel() maps VOL_3300 -> 0 while VOL_DEFAULT and VOL_3600 both
     * map to 3 (pmic.c:2117). hwPowerOn(VCN33_BT|VCN33_WIFI, VOL_3300) is what
     * mtk_wcn_consys_hw_{bt,wifi}_paldo_ctrl() asks for, so 0 is the only correct
     * code here -- leaving the field at its reset value runs the connectivity
     * front end at 3.6 V.
     */
    PMIC_VCN33_VOSEL_MASK = 0x3u << 2,
    PMIC_VCN33_VOSEL_3300 = 0x0u << 2,

    /* CONSYS_EMI_MAPPING: [11:0] is the share window's DRAM base in 1 MiB
     * units -- twelve bits is exactly 4 GiB of reach -- and bit 12 enables it. */
    CONSYS_EMI_BASE_MASK = 0x00000fffu,
    CONSYS_EMI_ENABLE    = BIT(12),

    /* The half of the aperture stock ioremaps and zeroes: base+0x80000 for
     * 0x55c00 bytes. Both literals are read straight out of
     * mtk_wcn_consys_hw_init() (0xc03baca8). */
    CONSYS_EMI_SHARE_OFFSET = 0x00080000u,
    CONSYS_EMI_SHARE_SIZE   = 0x00055c00u,

    POLL_LIMIT      = 200000u,
    CHIP_ID_RETRIES = 10u,
};

static mt6592_wifi_sdio_state g_state = {
    .blocked = "mt6592-wifi:consys-not-probed",
};
static int g_bound;

static void trace_stage(const char* text)
{
    mt6592_uart_puts(text);
    mt6592_bootstatus_log_text(text);
}

static uint32_t mmio_read(uint32_t address)
{
    return *(volatile uint32_t*)(uintptr_t)address;
}

static void mmio_write(uint32_t address, uint32_t value)
{
    *(volatile uint32_t*)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void mmio_set(uint32_t address, uint32_t mask)
{
    mmio_write(address, mmio_read(address) | mask);
}

static void mmio_clear(uint32_t address, uint32_t mask)
{
    mmio_write(address, mmio_read(address) & ~mask);
}

static void delay_us(uint32_t microseconds)
{
    mt6592_delay_cycles(microseconds * MT6592_DELAY_LEGACY_CYCLES_PER_US);
}

static int pmic_update(uint32_t address, uint32_t clear_mask, uint32_t set_mask)
{
    uint32_t value;
    int rc = mt6592_pwrap_read(address, &value);
    if (rc != MT6592_PWRAP_OK)
    {
        g_state.last_err = rc;
        return -1;
    }
    value = (value & ~clear_mask) | set_mask;
    rc    = mt6592_pwrap_write(address, value);
    if (rc != MT6592_PWRAP_OK)
    {
        g_state.last_err = rc;
        return -1;
    }
    return 0;
}

static int rails_on(void)
{
    int rc = mt6592_pwrap_init();
    if (rc != MT6592_PWRAP_OK)
    {
        g_state.last_err = rc;
        return -1;
    }

    /* VCN_1V8 is a fixed digital LDO -- hwPowerOn(VCN_1V8, VOL_DEFAULT) selects no
     * voltage -- so this is exactly the two bits stock writes: lp_mode_set(0) then
     * enable. mtk_wcn_consys_hw_reg_ctrl() (mtk_wcn_consys_hw.c:215). */
    if (pmic_update(PMIC_DIGLDO_CON11, PMIC_VCN18_LP_MODE, PMIC_VCN18_ENABLE) != 0)
    {
        return -1;
    }
    delay_us(150u);

    /*
     * VCN28 follows co_clock_flag, and it is NOT symmetric:
     *
     *     if (co_clock_en)  upmu_set_vcn28_on_ctrl(0);          <- and nothing else
     *     else            { upmu_set_vcn28_on_ctrl(1);
     *                       hwPowerOn(MT6323_POWER_LDO_VCN28, VOL_DEFAULT); }
     *
     * (mtk_wcn_consys_hw.c:223-236.) In co-clock mode the 2.8 V rail is left in SW
     * control and left OFF here; it is FM and GPS that later raise it, through
     * mtk_wcn_consys_hw_vcn28_ctrl(). J36's /etc/firmware/WMT_SOC.cfg says
     * co_clock_flag=1, so the co-clock branch is the one this board takes.
     *
     * An earlier revision of this function took the other branch -- ON_CTRL=1 plus
     * the enable bit -- which put VCN28 under hardware control on a board whose
     * connectivity clock is shared. WLAN does not need VCN28 on either path.
     */
    if (MT6592_WIFI_CO_CLOCK_FLAG != 0)
    {
        if (pmic_update(PMIC_ANALDO_CON19, PMIC_VCN28_ON_CTRL, 0u) != 0)
        {
            return -1;
        }
    }
    else if (pmic_update(PMIC_ANALDO_CON19, 0u, PMIC_VCN28_ON_CTRL | PMIC_VCN28_ENABLE) != 0)
    {
        return -1;
    }

    g_state.rails_programmed = 1;
    return 0;
}

static int wait_mask(uint32_t address, uint32_t mask, uint32_t expected)
{
    for (uint32_t i = 0; i < POLL_LIMIT; ++i)
    {
        if ((mmio_read(address) & mask) == expected)
        {
            return 0;
        }
    }
    return -1;
}

/*
 * The connectivity MTCMOS domain, in stock's order.
 *
 * mtk_wcn_consys_hw_reg_ctrl() does not open-code this -- it calls
 * conn_power_on() (mtk_wcn_consys_hw.c:276), which lands in
 * spm_mtcmos_ctrl_connsys(STA_POWER_ON) at
 * mediatek/platform/mt6592/kernel/core/mt_spm_mtcmos.c:1380-1406. That is nine
 * operations and they are the nine below, in this order:
 *
 *     CONN_PWR_CON |= PWR_ON
 *     CONN_PWR_CON |= PWR_ON_S
 *     spin until (PWR_STATUS & CONN) && (PWR_STATUS_S & CONN)
 *     CONN_PWR_CON &= ~PWR_CLK_DIS
 *     CONN_PWR_CON &= ~PWR_ISO
 *     CONN_PWR_CON |= PWR_RST_B
 *     CONN_PWR_CON &= ~MD_SRAM_PDN
 *     TOPAXI_PROT_EN &= ~CONN_PROT_MASK
 *     spin until (TOPAXI_PROT_STA1 & CONN_PROT_MASK) == 0
 *
 * The order is not decorative. The bus protection comes off last, after reset is
 * released, so nothing can issue a transaction into a domain that is still
 * isolated; and the two ack polls are unbounded spins in stock. Ours are bounded
 * by POLL_LIMIT because a bootstrap that hangs here has no console left to say
 * so, and the same file shows MediaTek reaching the same conclusion in the
 * power-down path, where the equivalent wait carries a `if (count > 1000) break`
 * (:1354-1357).
 *
 * The two acks are polled separately here and jointly there; for the rising edge
 * that is the same wait, since both have to be set before either loop ends.
 */
static int mtcmos_on(void)
{
    mmio_set(SPM_CONN_PWR_CON, PWR_ON);
    mmio_set(SPM_CONN_PWR_CON, PWR_ON_S);
    if (wait_mask(SPM_PWR_STATUS, CONN_PWR_STA_MASK, CONN_PWR_STA_MASK) != 0 ||
        wait_mask(SPM_PWR_STATUS_S, CONN_PWR_STA_MASK, CONN_PWR_STA_MASK) != 0)
    {
        g_state.pwr_status = mmio_read(SPM_PWR_STATUS);
        return -1;
    }

    mmio_clear(SPM_CONN_PWR_CON, PWR_CLK_DIS);
    mmio_clear(SPM_CONN_PWR_CON, PWR_ISO);
    mmio_set(SPM_CONN_PWR_CON, PWR_RST_B);
    mmio_clear(SPM_CONN_PWR_CON, CONN_SRAM_PDN);
    mmio_clear(TOPAXI_PROT_EN, CONN_PROT_MASK);
    if (wait_mask(TOPAXI_PROT_STA1, CONN_PROT_MASK, 0u) != 0)
    {
        return -1;
    }

    g_state.pwr_status   = mmio_read(SPM_PWR_STATUS);
    g_state.mtcmos_ready = 1;
    return 0;
}

static int infra_clock_on(void)
{
    mmio_write(INFRA_PDN_CLR, INFRA_CONNMCU_GATE);
    if (wait_mask(INFRA_PDN_STA, INFRA_CONNMCU_GATE, 0u) != 0)
    {
        return -1;
    }
    g_state.infra_clock_ready = 1;
    return 0;
}

/*
 * Point the connectivity subsystem's EMI aperture at real DRAM.
 *
 * Stock does this in mtk_wcn_consys_hw_init() (0xc03baca8) at probe, long
 * before any firmware moves:
 *
 *     gConEmiPhyBase = arm_memblock_steal(0x100000, 0x100000);
 *     emi_mpu_set_region_protection(base + 0x80000, base + 0x100000, 5, 0xa28);
 *     r2 = *CONSYS_EMI_MAPPING; r2 |= 0x1000; r2 |= gConEmiPhyBase >> 20;
 *     *CONSYS_EMI_MAPPING = r2; dsb;
 *     pEmiVirtBase = ioremap_nocache(base + 0x80000, 0x55c00);
 *     memset(pEmiVirtBase, 0, 0x55c00);
 *
 * Source agrees line for line -- mtk_wcn_consys_hw_init()
 * (mediatek/platform/mt6592/kernel/drivers/wmt/mtk_wcn_consys_hw.c:650-696) and
 * mtk_wcn_consys_memory_reserve() (:637-648) -- and names the two literals the
 * disassembly only gave as numbers: CONSYS_EMI_AP_PHY_OFFSET 0x80000 and
 * CONSYS_EMI_MEM_SIZE 343*KBYTE, which is 0x55c00 exactly
 * (mtk_wcn_consys_hw.h:109, :112). It also settles the register, in a comment
 * rather than a macro: "consys to ap emi remapping register:10001310" (:671).
 *
 * ONE DELIBERATE DIFFERENCE, in emi_share_window() below. Stock ORs its value in
 * (:675, `CONSYS_REG_READ(...) | addrPhy`), which only lands on the intended
 * base because the field reads zero from cold. We clear the twelve base bits
 * first and then set ours, so a second call, or a bootloader that got there
 * first, cannot leave the aperture pointing at the bitwise union of two
 * addresses. Stock's own base computation masks with 0xFFF00000 before the
 * shift (:672), so twelve bits is its number too.
 *
 * -- a 1 MiB, 1 MiB-aligned steal. The base is ours to choose, and 0x83100000
 * sits in the same gap as the firmware staging buffer: above the LK framebuffer
 * and the 512 KiB scratch at 0x83000000, below the ramdisk at 0x84000000, and
 * above stage1's ramdisk scan window.
 *
 * WHAT THIS APERTURE IS, measured: it is exactly where 90.6% of the firmware
 * goes. CONSYS 0xf0000000 == AP 0x83100000, so of the four sections:
 *
 *     s2  dest 0xf0020000  len 0x2ebc0  ->  AP 0x83120000..0x8314ebc0
 *     s3  dest 0xf0063000  len 0x0b6d0  ->  AP 0x83163000..0x8316e6d0
 *
 * Both were read back in DRAM after a download, decrypted. s2's dense span ends
 * between AP 0x8314e000 and 0x8314f000, which is 0x8314ebc0 to the page; s3
 * reads 32 KiB of zeros then 16 KiB of data, which is precisely the plaintext
 * its ciphertext implies (entropy 5.449, the 8-byte block c5f91fb483e9d660
 * repeating -- ECB over mostly-zero plaintext). AP 0x83120000 does not match the
 * staged ciphertext at the same section offset, so the boot ROM decrypts on the
 * way in.
 *
 * An earlier revision of this comment claimed the opposite -- "this aperture is
 * not where the firmware image goes" -- on the grounds that section 1 had been
 * read back correctly decrypted at AP 0x1809f800. That reading was wrong. AP
 * 0x1809f800 is ring slot 15's staging buffer (slot N at 0x18090000 + N*0x1010;
 * slot 15's descriptor at 0x1809f0f0 holds own-buffer CONSYS 0x0020f100, which
 * is AP 0x1809f100 under AP = CONSYS + 0x17E90000 -- the hardware naming its own
 * address). What was read there was leftover section-2 ciphertext in transit.
 * CONSYS 0x02000000 is NOT AP 0x18000000.
 *
 * The consequence is that the download is not the fault. The sections arrive,
 * and they arrive decrypted; the failure is downstream of that.
 *
 * The upper part of the window is a different thing again. Stock only ever
 * ioremaps base + 0x80000 for 0x55c00, and only ever reads a coredump out of it,
 * so it is shared memory for the *running* firmware sitting above the 512 KiB
 * the image loads into. That is why the memset matters and the MPU call does
 * not: an EMI MPU region left unconfigured is unprotected, and configuring one
 * can only ever take access away, whereas zeroing the span turns it into an
 * instrument -- anything non-zero there afterwards was written by connectivity
 * firmware that was executing.
 */
static void emi_share_zero(void)
{
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)(MT6592_CONSYS_EMI_PHYS_BASE + CONSYS_EMI_SHARE_OFFSET);
    uint32_t i;

    for (i = 0; i < CONSYS_EMI_SHARE_SIZE / 4u; ++i)
    {
        p[i] = 0u;
    }
}

static int emi_share_window(void)
{
    const uint32_t desired = (mmio_read(CONSYS_EMI_MAP) & ~(CONSYS_EMI_BASE_MASK | CONSYS_EMI_ENABLE)) |
                             ((MT6592_CONSYS_EMI_PHYS_BASE >> 20) & CONSYS_EMI_BASE_MASK) | CONSYS_EMI_ENABLE;

    mmio_write(CONSYS_EMI_MAP, desired);
    g_state.emi_mapping = mmio_read(CONSYS_EMI_MAP);
    if (g_state.emi_mapping != desired)
    {
        return -1;
    }
    emi_share_zero();
    return 0;
}

static int known_chip_id(uint32_t id)
{
    return id == 0x6592u || id == 0x6582u || id == 0x6572u;
}

/*
 * Ten reads of CONN_MCU_CONFIG_BASE+0x08, 20 ms apart, against the three
 * PLATFORM_SOC_CHIP values -- mtk_wcn_consys_hw_reg_ctrl()'s retry loop, and the
 * first thing in the bring-up that the connectivity subsystem itself has to answer.
 *
 * Stock compares the whole 32-bit word (mtk_wcn_consys_hw.c:~300); the register
 * reads as the bare chip ID with no revision bits above it, so masking to 16 bits
 * only widens what is accepted. The full compare is the primary test here, with a
 * low-half match kept as a logged fallback: it is the difference between "this is
 * an MT6592" and "something at that address has 0x6592 in it", and a bring-up log
 * that says which one it saw is worth more than one that silently accepts both.
 *
 * TWO DELIBERATE DEPARTURES from stock, both about failing loudly:
 *
 *   - Stock logs "CONSYS chip id mismatch" and carries on regardless, because in
 *     the kernel the subsystem is already known to exist from the platform data.
 *     Here the probe is the only evidence there is, so a timeout is fatal and
 *     names itself in g_state.blocked.
 *   - The settle before the first read is 10 us, matching the udelay(10) stock
 *     puts between conn_power_on() and the clock enable -- see bind(), where that
 *     delay now sits in the same place stock has it.
 */
static int chip_probe(void)
{
    for (uint32_t i = 0; i < CHIP_ID_RETRIES; ++i)
    {
        g_state.chip_id = mmio_read(CONSYS_CHIP_ID);
        if (known_chip_id(g_state.chip_id))
        {
            g_state.consys_responds = 1;
            return 0;
        }
        delay_us(20000u);
    }

    if (known_chip_id(g_state.chip_id & 0xffffu))
    {
        mt6592_uart_puts("  wifi: CONSYS chip id has unexpected high half: ");
        mt6592_uart_put_hex32(g_state.chip_id);
        mt6592_uart_puts("\n");
        g_state.consys_responds = 1;
        return 0;
    }
    return -1;
}

/*
 * The two VCN33 PALDOs, in the order and with the register writes stock uses.
 *
 * mtk_wcn_consys_hw_bt_paldo_ctrl() / _wifi_paldo_ctrl()
 * (mtk_wcn_consys_hw.c:468, :491) are both three steps in this order:
 *
 *     on:   hwPowerOn(VCN33_x, VOL_3300)   -> VOSEL := 0, then the enable bit
 *           upmu_set_vcn33_on_ctrl_x(1)    -> hand the rail to HW control
 *     off:  upmu_set_vcn33_on_ctrl_x(0)    -> take it back to SW control
 *           hwPowerDown(VCN33_x)           -> then clear the enable bit
 *
 * The order is the point: ON_CTRL hands the rail's enable over to a hardware
 * strobe, so it must be raised only once the LDO is already up and lowered before
 * the LDO comes down -- otherwise the rail is momentarily enabled by nobody.
 * Writing ON_CTRL and the enable bit in one PMIC transaction, as an earlier
 * revision did, cannot express that.
 *
 * VOSEL is shared, so it is written on the way up by whichever path goes up
 * first, and deliberately left alone on the way down (hwPowerDown does not touch
 * it either) -- clearing it would reset the other path's voltage.
 */
static int set_vcn33_rail(uint32_t reg, uint32_t enable_bit, uint32_t on_ctrl_bit, int enable)
{
    if (mt6592_pwrap_init() != MT6592_PWRAP_OK)
    {
        return -1;
    }

    if (enable)
    {
        if (pmic_update(PMIC_ANALDO_CON16, PMIC_VCN33_VOSEL_MASK, PMIC_VCN33_VOSEL_3300) != 0 ||
            pmic_update(reg, 0u, enable_bit) != 0 ||
            pmic_update(reg, 0u, on_ctrl_bit) != 0)
        {
            return -1;
        }
        return 0;
    }

    if (pmic_update(reg, on_ctrl_bit, 0u) != 0 || pmic_update(reg, enable_bit, 0u) != 0)
    {
        return -1;
    }
    return 0;
}

int mt6592_wifi_sdio_set_bt_rail(int enable)
{
    return set_vcn33_rail(PMIC_ANALDO_CON16, PMIC_VCN33_BT_ENABLE, PMIC_VCN33_BT_ON_CTRL, enable);
}

int mt6592_wifi_sdio_set_wifi_rail(int enable)
{
    int rc = set_vcn33_rail(PMIC_ANALDO_CON17, PMIC_VCN33_WIFI_ENABLE, PMIC_VCN33_WIFI_ON_CTRL, enable);
    if (rc == 0)
    {
        g_state.wifi_rail_ready = enable ? 1 : 0;
    }
    return rc;
}

#if MVII_MT6592_WIFI_ENABLE_TRANSPORT

int mt6592_wifi_sdio_bind(void)
{
    if (g_bound && g_state.consys_responds)
    {
        return 0;
    }
    g_bound = 1;

    trace_stage("wifi: CONSYS_6592 power/probe begin\n");

    /* Before the subsystem is powered, so the aperture is never live and wrong.
     * INFRACFG_AO is always on, so this can be done from a cold start. */
    if (emi_share_window() != 0)
    {
        g_state.blocked = "mt6592-wifi:consys-emi-mapping-failed";
        return -1;
    }
    trace_stage("wifi: CONSYS EMI share window mapped\n");

    if (rails_on() != 0)
    {
        g_state.blocked = "mt6592-wifi:vcn18-vcn28-pwrap-failed";
        return -1;
    }
    trace_stage("wifi: VCN18/VCN28 rails ready\n");

    if (mtcmos_on() != 0)
    {
        g_state.blocked = "mt6592-wifi:consys-mtcmos-timeout";
        return -1;
    }
    trace_stage("wifi: CONSYS MTCMOS ready\n");

    /* conn_power_on(); udelay(10); enable_clock(MT_CG_INFRA_CONNMCU). The settle
     * belongs between the power domain coming up and its clock being ungated --
     * not after the clock, where an earlier revision had it. */
    delay_us(10u);

    if (infra_clock_on() != 0)
    {
        g_state.blocked = "mt6592-wifi:infra-connmcu-clock-timeout";
        return -1;
    }
    trace_stage("wifi: INFRA CONNMCU clock ready\n");

    if (chip_probe() != 0)
    {
        g_state.blocked = "mt6592-wifi:consys-chip-id-timeout";
        mt6592_uart_puts("  wifi: unexpected CONSYS chip id=");
        mt6592_uart_put_hex32(g_state.chip_id);
        mt6592_uart_puts("\n");
        return -1;
    }

    trace_stage("wifi: CONSYS chip ID ready\n");

    /*
     * msleep(5) closes mtk_wcn_consys_hw_reg_ctrl()'s power-on path, before
     * anything speaks to the subsystem. Keep it, so the first BTIF frame cannot
     * race the clock settling.
     *
     * VCN33_WIFI is deliberately NOT raised here, but it IS raised twice later.
     * Searching conn_soc for WIFI_PALDO finds exactly one pair of users --
     * wmt_ic_soc.c:766 and :779, bracketing the RF calibration script -- and
     * wmt_func.c's wifi_on path (:681) does not touch it at all; it only calls
     * the WLAN driver's probe. mt6592_wifi_wmt.c reproduces that bracket.
     *
     * That search is not the whole story, and reading it as one cost this driver
     * a bring-up. The WLAN driver's probe raises the same regulator under a name
     * no WIFI_PALDO search matches: HifAhbProbe does
     * hwPowerOn(MT6323_POWER_LDO_VCN33_WIFI, VOL_3300, "WLAN") immediately
     * before pfWlanProbe (ahb.c:1823-1826) and only drops it in HifAhbRemove.
     * So the 3.3 V rail is a calibration-time rail AND the transmit PA supply
     * for the life of the WLAN driver; mt6592_wifi_hif_bind() is where the
     * second lifetime starts, for the same reason it starts in HifAhbProbe.
     *
     * The one step of stock's power-on this function does not reproduce is the
     * second half of mtk_wcn_consys_hw_pwr_on(): after reg_ctrl it calls
     * mtk_wcn_consys_hw_gpio_ctrl(1) (mtk_wcn_consys_hw.c:426, body at :366-393).
     * Nothing in there is for WLAN. It initialises PIN_GPS_SYNC, PIN_GPS_LNA and
     * PIN_I2S_GRP -- GPS and BT audio, and J36's WMT_SOC.cfg turns the GPS LNA off
     * anyway (wmt_gps_lna_pin=0, wmt_gps_lna_enable=0) -- and then registers the
     * BGF external interrupt only to disable it two lines later
     * (PIN_STA_INIT then PIN_STA_EINT_DIS, :386-389). So the state it leaves
     * behind for the connectivity interrupt is "off", which is where a polled
     * driver needs it, and is where it already is on a cold boot.
     */
    delay_us(5000u);

    g_state.blocked = "mt6592-wifi:firmware-not-loaded";
    mt6592_uart_puts("  wifi: CONSYS chip id=");
    mt6592_uart_put_hex32(g_state.chip_id);
    mt6592_uart_puts("\n");
    return 0;
}

#else

int mt6592_wifi_sdio_bind(void)
{
    g_bound         = 1;
    g_state.blocked = "mt6592-wifi:transport-disabled";
    return -1;
}

#endif

const mt6592_wifi_sdio_state* mt6592_wifi_sdio_get_state(void)
{
    return &g_state;
}
