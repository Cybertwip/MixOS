/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
#ifndef MT6592_DRAM_H
#define MT6592_DRAM_H

#include <stdint.h>

enum {
    MT6592_DRAM_OK = 0,
    MT6592_DRAM_ERR_SELFTEST = -1,
    MT6592_DRAM_ERR_BAD_STEP = -2,
};

enum {
    MT6592_DRAM_STEP_MEMPLL = 1,
    MT6592_DRAM_STEP_REXTDN = 2,
    MT6592_DRAM_STEP_LPDDR2 = 3,
    MT6592_DRAM_STEP_POST = 4,
    MT6592_DRAM_STEP_TEST = 5,
    MT6592_DRAM_STEP_GATING = 6,   /* RX DQS gating window calibration (rank 0) */
};

/* Bring up DRAM using this board's exact MTK_BLOADER_INFO_v13/MTK_BIN EMI
 * record from preloader_j36ultra.bin, then probe only the framebuffer window
 * needed by stage1 video scanout. */
int mt6592_dram_init(void);

/* Same bring-up, split so stage1 can emit a breadcrumb before the dangerous
 * hardware step and the host can see the last reached phase if it hangs. */
int mt6592_dram_init_step(uint32_t step);

/* Standalone DRAM write/read self-test across both ranks. Returns 0 on success;
 * on failure fills *first_bad (address) and *readback (value) when non-NULL. */
int mt6592_dram_selftest(uint32_t* first_bad, uint32_t* readback);

/* Last failure captured by mt6592_dram_init(), without re-running the test. */
int mt6592_dram_last_failure(uint32_t* first_bad, uint32_t* readback);

/* Fault-guarded single-word write/read at an arbitrary DRAM address. Returns 1
 * on match, 0 on mismatch or a caught data abort; *got_out gets the readback
 * (0xdab0da7a on a caught abort). A true bus stall cannot be caught and hangs at
 * the caller -- used to attribute a residual read stall to a specific address. */
int mt6592_dram_verify_rw(uint32_t addr, uint32_t* got_out);

uint32_t mt6592_dram_last_phase(void);

#endif
