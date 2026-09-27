#ifndef MVII_EXTERNAL_POWER_H
#define MVII_EXTERNAL_POWER_H
#include "mt6592_pwrap.h"
#include "j36_external_power.h"

#ifndef MVII_WITHOUT_BATTERY
#define MVII_WITHOUT_BATTERY 0
#endif

static inline int external_power_read(void *ctx, unsigned int reg, unsigned int *value) {
    (void)ctx;
    return mt6592_pwrap_read(reg, value);
}
static inline int external_power_write(void *ctx, unsigned int reg, unsigned int value) {
    (void)ctx;
    return mt6592_pwrap_write(reg, value);
}
static inline int external_power_hold(void) {
    if (!mt6592_pwrap_is_ready()) return -1;
    return j36_external_power_hold(0, external_power_read, external_power_write);
}
#endif
