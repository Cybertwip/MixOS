#pragma once

#include <stdint.h>

#define MT6592_EMMC_BOOT1_PART 0x01u
#define MT6592_EMMC_BOOT2_PART 0x02u
#define MT6592_EMMC_USER_PART 0x08u

typedef struct {
    uint32_t stage;
    uint32_t cmd;
    uint32_t arg;
    uint32_t status;
    uint32_t resp;
    uint32_t cfg;
    uint32_t ps;
    uint32_t sdc_sts;
    uint32_t int_reg;
    uint32_t clk_cfg2;
    uint32_t peri_sta;
    uint32_t pll_con0;
    uint32_t pll_pwr;
    uint32_t gpio_clk;
    uint32_t gpio_cmd;
    uint32_t gpio_dat;
    uint32_t ocr;
    int err;
} mt6592_msdc_diag_t;

enum {
    MT6592_MSDC_OK = 0,
    MT6592_MSDC_ERR_TIMEOUT = -1,
    MT6592_MSDC_ERR_CMD = -2,
    MT6592_MSDC_ERR_DATA = -3,
    MT6592_MSDC_ERR_ALIGN = -4,
    MT6592_MSDC_ERR_UNSUPPORTED = -5,
};

int mt6592_emmc_user_init(void);
int mt6592_emmc_read_part(uint32_t part, uint64_t byte_offset, uint8_t* data, uint32_t size);
int mt6592_emmc_read_user(uint64_t byte_offset, uint8_t* data, uint32_t size);
int mt6592_emmc_write_part(uint32_t part, uint64_t byte_offset, const uint8_t* data, uint32_t size);
int mt6592_emmc_write_user(uint64_t byte_offset, const uint8_t* data, uint32_t size);
int mt6592_emmc_enable_boot_part(uint32_t part, uint32_t boot_ack);
uint64_t mt6592_emmc_user_capacity_sectors(void);
uint32_t mt6592_emmc_last_response(void);
uint32_t mt6592_emmc_ocr(void);
/* Bus mode the driver negotiated and verified at init: 1/4/8 lanes, and the
 * host clock in Hz. Identification always starts 1-bit; these say where it
 * ended up. */
uint32_t mt6592_emmc_bus_width_bits(void);
uint32_t mt6592_emmc_bus_clock_hz(void);
/* bit0 = HS timing engaged, bit1 = data sampled on the alternate edge. */
uint32_t mt6592_emmc_bus_mode(void);
const mt6592_msdc_diag_t* mt6592_msdc_diag(void);
