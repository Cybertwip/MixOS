#ifndef MT6592_WIFI_SDIO_H
#define MT6592_WIFI_SDIO_H

/*
 * Historical filename retained for build compatibility. J36 Ultra does not
 * use an external MT66xx SDIO combo chip: its stock configuration names the
 * integrated CONSYS_6592 WLAN block. This stage powers and probes that block.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int rails_programmed;
    int mtcmos_ready;
    int infra_clock_ready;
    int consys_responds;
    /* Whether VCN33_WIFI is up right now -- not whether it ever came up.
     *
     * Two things raise it and they overlap: mt6592_wifi_wmt.c brackets the RF
     * calibration frame with it (wmt_ic_soc.c:762-782, spelled WIFI_PALDO), and
     * mt6592_wifi_hif_bind() raises it for good (ahb.c:1823-1826, spelled
     * MT6323_POWER_LDO_VCN33_WIFI). So after a successful bring-up this reads 1
     * -- it is the transmit PA supply and stock holds it for the life of the
     * WLAN driver. A 0 here on a board that claims a running radio means the
     * calibration bracket dropped it and the bind never put it back. */
    int wifi_rail_ready;
    uint32_t chip_id;
    uint32_t pwr_status;
    /* CONSYS_EMI_MAPPING as it reads back after being programmed: the enable bit
     * plus the DRAM base of the share window in 1 MiB units. */
    uint32_t emi_mapping;
    int last_err;
    const char* blocked;
} mt6592_wifi_sdio_state;

/*
 * The DRAM window the connectivity subsystem reaches through its own aperture.
 * Exposed because the WLAN firmware image places one of its sections in there,
 * so whoever stages the image needs to be able to look at where it landed.
 */
#define MT6592_CONSYS_EMI_PHYS_BASE 0x83100000u
#define MT6592_CONSYS_EMI_PHYS_SIZE 0x00100000u

/*
 * co_clock_flag, straight out of the device's own
 * /etc/firmware/WMT_SOC.cfg (extracted from the J36 system image):
 *
 *     coex_wmt_ant_mode=1
 *     wmt_gps_lna_pin=0
 *     wmt_gps_lna_enable=0
 *     co_clock_flag=1
 *
 * The stock driver reads that file at probe and threads the value through the
 * whole bring-up: mtk_wcn_consys_hw_reg_ctrl(on, co_clock_en) branches on it for
 * VCN28, wmt_plat_init() picks an oscillator setup from it, and wmt_ic_soc.c sends
 * the CO_CLOCK command only when it is set. There is no file to read this early,
 * so the device's value is the constant -- one definition, so the branches cannot
 * disagree with each other.
 */
#define MT6592_WIFI_CO_CLOCK_FLAG 1

/* Deferred, bounded CONSYS power-on and chip-ID probe. */
int mt6592_wifi_sdio_bind(void);

/*
 * The two VCN33 connectivity PALDOs. Both are calibration-time rails on this
 * platform -- see the note at the end of mt6592_wifi_sdio_bind() -- so the WMT
 * layer raises them around the RF-calibration frame and drops them again.
 */
int mt6592_wifi_sdio_set_bt_rail(int enable);
int mt6592_wifi_sdio_set_wifi_rail(int enable);

const mt6592_wifi_sdio_state* mt6592_wifi_sdio_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_WIFI_SDIO_H */
