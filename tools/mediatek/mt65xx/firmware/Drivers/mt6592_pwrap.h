/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
#ifndef MT6592_PWRAP_H
#define MT6592_PWRAP_H

/* Distinct non-zero codes so a breadcrumb can show where pwrap init failed. */
enum {
    MT6592_PWRAP_OK = 0,
    MT6592_PWRAP_ERR_ARG = -1,
    MT6592_PWRAP_ERR_TIMEOUT = -2,
    MT6592_PWRAP_ERR_RESET_SPI = -3,
    MT6592_PWRAP_ERR_SISTROBE = -4,
    MT6592_PWRAP_ERR_REGCLK = -5,
    MT6592_PWRAP_ERR_DIO = -6,
    MT6592_PWRAP_ERR_CIPHER = -7,
    MT6592_PWRAP_ERR_WRITE_TEST = -8,
    MT6592_PWRAP_ERR_CRC = -9,
    MT6592_PWRAP_ERR_NOT_INIT = -10,
};

/* Cold-init the MT6592/MT6323 PMIC wrapper so WACS2 reads/writes work.
 * Idempotent enough to call once at stage1 entry. Returns 0 on success. */
int mt6592_pwrap_init(void);

/* TRUE if WACS2 reports INIT_DONE (pwrap usable). */
int mt6592_pwrap_is_ready(void);

#include <stdint.h>

/* 16-bit PMIC register access over WACS2. Fail with
 * MT6592_PWRAP_ERR_NOT_INIT until pwrap reports INIT_DONE. */
int mt6592_pwrap_read(uint32_t adr, uint32_t* rdata);
int mt6592_pwrap_write(uint32_t adr, uint32_t wdata);

#endif
