#ifndef MT6592_BOOTSTATUS_H
#define MT6592_BOOTSTATUS_H

#include <stdint.h>

#include "mt6592_bootinfo.h"

/*
 * How much of the eMMC console ring is mirrored in RAM. Characters are appended
 * here and only reach storage at a commit point, so this is the amount of
 * output that can be produced between commits without losing the oldest of it.
 * Must be a multiple of 512 and no larger than MT6592_CONSOLE_RING_DATA_SIZE.
 *
 * The default suits the SRAM-resident loader and flash payloads, which log a
 * few dozen lines with autoflush on the whole time. MVII.elf overrides it to
 * the full ring: it is the image with a frame loop, and a frame loop cannot
 * afford the milliseconds an MSDC transaction costs.
 */
#ifndef MT6592_CONSOLE_RING_RAM_BYTES
#define MT6592_CONSOLE_RING_RAM_BYTES 2048u
#endif

enum {
    MT6592_BOOT_STATUS_MAGIC = 0x5342374du, /* M7BS */
    MT6592_BOOT_STATUS_VERSION = 3u,
    MT6592_BOOT_STATUS_SIZE = 512u,
    MT6592_BOOT_STATUS_MESSAGE_SIZE = 320u,

    MT6592_J36_LK_SLOT_OFFSET = 0x01d40000u,
    MT6592_J36_LK_SLOT_SIZE = 0x00200000u,
    MT6592_BOOT_STATUS_OFFSET = MT6592_J36_LK_SLOT_OFFSET + MT6592_J36_LK_SLOT_SIZE - MT6592_BOOT_STATUS_SIZE,

    /* Full runtime console log: a 64 KiB ring in the LK-slot padding directly
     * below the status sector (header sector + data). The message field above
     * only holds the newest ~320 bytes; the ring keeps the whole boot. */
    MT6592_CONSOLE_RING_MAGIC = 0x4c43374du, /* M7CL */
    MT6592_CONSOLE_RING_VERSION = 1u,
    MT6592_CONSOLE_RING_TOTAL = 0x10000u,
    MT6592_CONSOLE_RING_OFFSET = MT6592_J36_LK_SLOT_OFFSET + MT6592_J36_LK_SLOT_SIZE - MT6592_CONSOLE_RING_TOTAL,
    MT6592_CONSOLE_RING_DATA_OFFSET = MT6592_CONSOLE_RING_OFFSET + 512u,
    MT6592_CONSOLE_RING_DATA_SIZE = MT6592_CONSOLE_RING_TOTAL - 512u - MT6592_BOOT_STATUS_SIZE,

    MT6592_BOOT_STATUS_STAGE_FLASH_PENDING = 0x1001u,

    /* 0x12xx: the MVII minimal LK that replaces the stock MediaTek one in the
     * UBOOT slot (mvii_lk_main.c). It commits the sector at every milestone
     * from eMMC init onwards, so a stage in this range is the last thing the LK
     * reached — and the record's message field holds the boot log that led
     * there, because the console mirror autoflushes once storage is up. */
    MT6592_BOOT_STATUS_STAGE_LK_ENTRY = 0x1201u,
    MT6592_BOOT_STATUS_STAGE_LK_POWER_ARMED = 0x1202u,
    MT6592_BOOT_STATUS_STAGE_LK_SCANOUT = 0x1203u,
    MT6592_BOOT_STATUS_STAGE_LK_DISPLAY_FAILED = 0x1204u,
    MT6592_BOOT_STATUS_STAGE_LK_BOOTIMG_FAILED = 0x1205u,
    MT6592_BOOT_STATUS_STAGE_LK_HANDOFF = 0x1206u,
    MT6592_BOOT_STATUS_STAGE_LK_STORAGE_READY = 0x1207u,
    MT6592_BOOT_STATUS_STAGE_LK_BOOTIMG_HEADER = 0x1208u,
    MT6592_BOOT_STATUS_STAGE_LK_KERNEL_LOADED = 0x1209u,
    MT6592_BOOT_STATUS_STAGE_LK_RAMDISK_LOADED = 0x120au,
    MT6592_BOOT_STATUS_STAGE_LK_EXCEPTION = 0x120bu,

    MT6592_BOOT_STATUS_STAGE_STAGE1_ENTRY = 0x1101u,
    MT6592_BOOT_STATUS_STAGE_STAGE1_STAGE2_WRAPPED = 0x1102u,
    MT6592_BOOT_STATUS_STAGE_STAGE1_STAGE2_MISSING = 0x1103u,
    MT6592_BOOT_STATUS_STAGE_STAGE1_HANDOFF = 0x1104u,
    MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_ARMED = 0x1105u,
    MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_IDLE = 0x1106u,
    MT6592_BOOT_STATUS_STAGE_STAGE1_CHARGE_HOLD = 0x1107u,
    MT6592_BOOT_STATUS_STAGE_STAGE2_ENTRY = 0x2001u,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_BOUND = 0x2002u,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_UNBOUND = 0x2003u,
    MT6592_BOOT_STATUS_STAGE_STORAGE_PROBED = 0x2004u,
    MT6592_BOOT_STATUS_STAGE_STORAGE_BEGIN = 0x2005u,
    MT6592_BOOT_STATUS_STAGE_STORAGE_LAYOUT_DONE = 0x2006u,
    MT6592_BOOT_STATUS_STAGE_STORAGE_READY = 0x2007u,
    MT6592_BOOT_STATUS_STAGE_STORAGE_MBR_DONE = 0x2008u,
    MT6592_BOOT_STATUS_STAGE_STORAGE_GPT_DONE = 0x2009u,
    MT6592_BOOT_STATUS_STAGE_STAGE2_HANDOFF_PENDING = 0x200au,
    MT6592_BOOT_STATUS_STAGE_STAGE2_MB2_READY = 0x200bu,
    MT6592_BOOT_STATUS_STAGE_STAGE2_HANDOFF_SPLASH = 0x200cu,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_SCANOUT_BEGIN = 0x200du,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_SCANOUT_STARTED = 0x200eu,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_SCANOUT_FAILED = 0x200fu,
    MT6592_BOOT_STATUS_STAGE_MB2_BUILD_BEGIN = 0x2010u,
    MT6592_BOOT_STATUS_STAGE_MB2_ZEROED = 0x2011u,
    MT6592_BOOT_STATUS_STAGE_MB2_FRAMEBUFFER = 0x2012u,
    MT6592_BOOT_STATUS_STAGE_MB2_BOOT_VOLUME = 0x2013u,
    MT6592_BOOT_STATUS_STAGE_MB2_END = 0x2014u,
    MT6592_BOOT_STATUS_STAGE_MB2_TOTAL = 0x2015u,
    MT6592_BOOT_STATUS_STAGE_MB2_FRAMEBUFFER_HEADER = 0x2016u,
    MT6592_BOOT_STATUS_STAGE_MB2_FRAMEBUFFER_ADDR = 0x2017u,
    MT6592_BOOT_STATUS_STAGE_MB2_FRAMEBUFFER_GEOMETRY = 0x2018u,
    MT6592_BOOT_STATUS_STAGE_MB2_FRAMEBUFFER_FORMAT = 0x2019u,
    MT6592_BOOT_STATUS_STAGE_MB2_FRAMEBUFFER_DONE = 0x201au,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_SAFE_PROFILE = 0x201bu,
    MT6592_BOOT_STATUS_STAGE_DISPLAY_COMMAND_SPLASH = 0x201cu,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_HANDOFF = 0x2101u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_MISSING = 0x2102u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_RETURNED = 0x2103u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_ENTERING = 0x2104u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_ENTERED = 0x2201u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_BOOT_VOLUME = 0x2202u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_HEADLESS = 0x2203u,
    MT6592_BOOT_STATUS_STAGE_RUNTIME_HEARTBEAT = 0x2204u,
    MT6592_BOOT_STATUS_STAGE_BROM_RESET_KEY = 0x2ffeu,
    MT6592_BOOT_STATUS_STAGE_HEARTBEAT = 0x2fffu,

    MT6592_BOOT_STATUS_FLAG_FRAMEBUFFER_BOUND = 1u << 0,
    MT6592_BOOT_STATUS_FLAG_STORAGE_READY = 1u << 1,
    MT6592_BOOT_STATUS_FLAG_BOOT_VOLUME = 1u << 2,
    MT6592_BOOT_STATUS_FLAG_WRITTEN_BY_FLASH = 1u << 3,
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t record_size;
    uint32_t stage;
    uint32_t flags;
    uint32_t sequence;
    uint32_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint32_t entry_r0;
    uint32_t entry_r1;
    uint32_t entry_r2;
    uint32_t entry_r3;
    uint32_t image_base;
    uint32_t storage_count;
    uint32_t boot_volume_present;
    uint32_t boot_volume_serial;
    uint32_t mmsys_cg_con0;
    uint32_t mmsys_cg_con1;
    uint32_t mmsys_lcm_rst_b;
    uint32_t disp_pwm_probe_base;
    uint32_t last_error;
    char message[MT6592_BOOT_STATUS_MESSAGE_SIZE];
    uint32_t bootarg_addr;
    uint32_t bootarg_size;
    uint32_t bootarg_words[6];
    uint32_t pwm_gpio_mode;
    uint32_t bls_en;
    uint32_t bls_con0;
    uint32_t bls_con1;
    uint32_t bls_debug;
    uint32_t disp_pwm_con0;
    uint32_t disp_pwm_con1;
    uint32_t pwm_gpio_data;
    uint32_t pmic_pwrap_status;
    uint32_t pmic_cid;
    uint32_t pmic_ldo_status0;
    uint32_t pmic_ldo_status1;
    uint32_t pmic_vio28_con0;
    uint32_t pmic_vgp1_con7;
    uint32_t pmic_vgp1_con28;
    uint32_t pmic_vio18_con49;
} mt6592_boot_status_t;

void mt6592_bootstatus_init(void);
void mt6592_bootstatus_set_stage(uint32_t stage);
void mt6592_bootstatus_set_message(const char* message);
void mt6592_bootstatus_set_handoff(const mt6592_boot_handoff_t* handoff);
void mt6592_bootstatus_set_image_base(uint32_t image_base);
void mt6592_bootstatus_set_framebuffer(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp,
                                       uint32_t bound);
void mt6592_bootstatus_set_storage(uint32_t count, uint32_t boot_volume_present, uint32_t boot_volume_serial);
void mt6592_bootstatus_set_visible(uint32_t mmsys_cg_con0, uint32_t mmsys_cg_con1, uint32_t lcm_rst_b,
                                   uint32_t disp_pwm_probe_base);
void mt6592_bootstatus_set_visible_extra(uint32_t pwm_gpio_mode, uint32_t bls_en, uint32_t bls_con0,
                                         uint32_t bls_con1, uint32_t bls_debug, uint32_t disp_pwm_con0,
                                         uint32_t disp_pwm_con1, uint32_t pwm_gpio_data);
void mt6592_bootstatus_set_power(uint32_t pwrap_status, uint32_t pmic_cid, uint32_t ldo_status0,
                                 uint32_t ldo_status1, uint32_t vio28_con0, uint32_t vgp1_con7,
                                 uint32_t vgp1_con28, uint32_t vio18_con49);
void mt6592_bootstatus_set_error(uint32_t error);
int mt6592_bootstatus_flush(void);
int mt6592_bootstatus_publish_flash_pending(void);

/* Runtime console mirror: feed serial/stdout characters here and completed
 * lines are appended to the status record's message field (rolling, newest
 * kept) and flushed to eMMC, so `flash -mtk-read-boot-status` doubles as a
 * serial console for the MMU-off runtime. Inert until bootstatus_init ran. */
void mt6592_bootstatus_log_char(char c);
void mt6592_bootstatus_log_text(const char* text);

/*
 * Console autoflush. On during boot, where every milestone should survive a
 * hang. Off once the shell's frame loop is live: an MSDC transaction is
 * milliseconds of stalled CPU, so a log line landing mid-frame freezes the
 * running app, and a chatty frame loop freezes the OS outright. With it off,
 * output still accumulates in the RAM console ring and the message field --
 * only the trip to storage is deferred.
 *
 * Deferred to when? mt6592_bootstatus_console_flush(), and the events that
 * already call mt6592_bootstatus_flush(): panics, traps, and main returning.
 * Those are one-shot, so none of them can stall a frame that has a next one.
 */
void mt6592_bootstatus_console_set_autoflush(int enabled);
int mt6592_bootstatus_console_flush(void);

/*
 * Console mirroring into the ring. On by default; turning it off drops console
 * output on the floor as far as the ring is concerned -- not buffered, not
 * deferred, not recorded.
 *
 * Autoflush is not enough for the one caller that needs this. Anything that reads
 * the ring back and prints it is feeding its own output straight into the thing it
 * is reading: with autoflush off the text still advances the write position, so the
 * first line printed afterwards -- with autoflush back on -- commits a lap's worth
 * of dump transcript over the very bytes the next read wants. A reader has to be
 * able to stop writing entirely, or reading the log is what destroys it.
 *
 * Restore it when done. Anything that leaves it off has silently stopped recording
 * the boot log.
 */
void mt6592_bootstatus_console_set_mirror(int enabled);

#endif
