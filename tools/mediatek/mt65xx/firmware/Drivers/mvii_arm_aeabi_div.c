/*
 * mvii_arm_aeabi_div.c — freestanding 64-bit division helpers for images that
 * do not link compiler-rt builtins (MVIISlot.elf, non-FULL_OS MVII.elf).
 *
 * armv7 (Cortex-A7) has hardware udiv/sdiv for 32-bit, so the compiler only
 * emits libcalls for 64-bit division: __aeabi_uldivmod / __aeabi_ldivmod.
 * First hit: mt6592_timer_microseconds() (ticks/13) once the charger service
 * was enabled in MVIISlot.elf; the AFE audio driver's pacing math has the same
 * dependency when linked.
 *
 * In the FULL_OS MVII.elf these object-file definitions simply preempt the
 * compiler-rt archive members (archives are only searched for still-undefined
 * symbols), so linking this file everywhere is safe.
 *
 * AEABI contract: n in r0:r1, d in r2:r3 → quotient in r0:r1, remainder in
 * r2:r3. A C function cannot return the {quot, rem} pair in r0-r3 under
 * AAPCS, so the entry points are small naked wrappers around plain-C cores
 * (the same shape compiler-rt's aeabi_uldivmod.S uses around __udivmoddi4).
 */

#include <stdint.h>

/* Shift-subtract long division. Bounded 64 iterations, no libcalls, no
 * lookup tables — bring-up-friendly and fast enough for the timer path
 * (a few hundred calls per second). Division by zero returns quot=0 rem=0
 * (freestanding image: no trap plumbing worth wiring for it). */
uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t* rem_out)
{
    uint64_t quot = 0u;
    uint64_t rem = 0u;

    if (den != 0u) {
        for (int i = 63; i >= 0; --i) {
            rem = (rem << 1) | ((num >> i) & 1u);
            if (rem >= den) {
                rem -= den;
                quot |= (uint64_t)1u << i;
            }
        }
    }
    if (rem_out) *rem_out = rem;
    return quot;
}

/* Signed core on top of the unsigned one (C99/AAPCS truncated division:
 * remainder takes the sign of the dividend). */
int64_t __divmoddi4(int64_t num, int64_t den, int64_t* rem_out)
{
    const int neg_num = num < 0;
    const int neg_den = den < 0;
    uint64_t unum = neg_num ? (uint64_t)0u - (uint64_t)num : (uint64_t)num;
    uint64_t uden = neg_den ? (uint64_t)0u - (uint64_t)den : (uint64_t)den;
    uint64_t urem = 0u;
    uint64_t uquot = __udivmoddi4(unum, uden, &urem);
    int64_t quot = (neg_num != neg_den) ? -(int64_t)uquot : (int64_t)uquot;
    int64_t rem = neg_num ? -(int64_t)urem : (int64_t)urem;
    if (rem_out) *rem_out = rem;
    return quot;
}

/* Plain libgcc-style entry points some codegen paths use directly. */
uint64_t __udivdi3(uint64_t num, uint64_t den) { return __udivmoddi4(num, den, 0); }
uint64_t __umoddi3(uint64_t num, uint64_t den)
{
    uint64_t rem = 0u;
    (void)__udivmoddi4(num, den, &rem);
    return rem;
}
int64_t __divdi3(int64_t num, int64_t den) { return __divmoddi4(num, den, 0); }
int64_t __moddi3(int64_t num, int64_t den)
{
    int64_t rem = 0;
    (void)__divmoddi4(num, den, &rem);
    return rem;
}

#if defined(__arm__)

/* AEABI wrappers. Stack layout: 8 bytes for the outgoing rem-pointer argument
 * slot (+ alignment), 8 bytes for the remainder itself; sp stays 8-byte
 * aligned across the public interface (push {r11, lr} = 8 bytes, sub #16). */
__attribute__((naked)) void __aeabi_uldivmod(void)
{
    __asm__ volatile(
        "push {r11, lr}\n"
        "sub  sp, sp, #16\n"
        "add  r12, sp, #8\n"
        "str  r12, [sp]\n"        /* 5th arg: &remainder */
        "bl   __udivmoddi4\n"
        "ldr  r2, [sp, #8]\n"     /* remainder → r2:r3 */
        "ldr  r3, [sp, #12]\n"
        "add  sp, sp, #16\n"
        "pop  {r11, pc}\n");
}

__attribute__((naked)) void __aeabi_ldivmod(void)
{
    __asm__ volatile(
        "push {r11, lr}\n"
        "sub  sp, sp, #16\n"
        "add  r12, sp, #8\n"
        "str  r12, [sp]\n"
        "bl   __divmoddi4\n"
        "ldr  r2, [sp, #8]\n"
        "ldr  r3, [sp, #12]\n"
        "add  sp, sp, #16\n"
        "pop  {r11, pc}\n");
}

#endif /* __arm__ */
