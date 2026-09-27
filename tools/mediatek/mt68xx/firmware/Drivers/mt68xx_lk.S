/*
 * mt68xx_lk.S -- mt68xx minimal LK entry, running from the UBOOT/LK slot.
 *
 * Derived from tools/mediatek/firmware/Drivers/mvii_lk.S. Same contract:
 * the preloader loads this image to MT68XX_MEMBASE and branches here with
 * its boot arguments in r0-r3. Same sequence: mask interrupts, raise the
 * abort and SVC stacks, take the MMU and caches off (LK runs
 * Strongly-Ordered throughout), point VBAR at the reporting vectors, zero
 * .bss (the preloader hands over DRAM a previous boot wrote), then call
 * into C with the preloader's arguments intact.
 *
 * Two deliberate omissions against the j36 original:
 *  - No jump_to_kernel. The j36 helper jumps a 32-bit kernel through ATAGs;
 *    these phones boot 64-bit kernels through a DTB, which needs an
 *    AArch32->AArch64 switch instead. LK-BRINGUP step 5 writes that helper
 *    here; until then there is nothing to jump to and no stub pretending.
 *  - The data-abort resume path is kept (two words in main.c claim it) even
 *    though no fault-guarded probe exists yet. Display bring-up will take
 *    real aborts on purpose, and the resume slot has to predate it.
 */
.section .text.start, "ax", %progbits
.arm
.global _start
.type _start, %function

.extern mt68xx_stage1_abort_flag
.extern mt68xx_stage1_abort_resume

_start:
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, r3
    cpsid if
    ldr r0, =__stack_top
    bic r0, r0, #7
    cps #0x17
    mov sp, r0
    sub r0, r0, #0x400
    cps #0x13
    mov sp, r0
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #0x00000001
    bic r0, r0, #0x00000004
    bic r0, r0, #0x00000800
    bic r0, r0, #0x00001000
    mcr p15, 0, r0, c1, c0, 0
    isb
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c10, 4
    dsb
    isb
    ldr r0, =mt68xx_lk_exception_vectors
    mcr p15, 0, r0, c12, c0, 0
    isb

    /* Zero .bss before any C runs. */
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
.Lmt68xx_lk_bss_zero:
    cmp r0, r1
    strlo r2, [r0], #4
    blo .Lmt68xx_lk_bss_zero

    /*
     * Re-establish the SVC stack, and note what it is NOT: __stack_top is
     * the abort stack, set above. The handler below opens with
     * `stmdb sp!, {r0-r12}` -- with the two stacks aliased that is 52 bytes
     * written straight through the live C frame of whatever provoked the
     * fault.
     */
    ldr sp, =__stack_top
    sub sp, sp, #0x400
    bic sp, sp, #7
    mov r0, r4
    mov r1, r5
    mov r2, r6
    mov r3, r7
    bl mt68xx_lk_main

.Lmt68xx_lk_halt:
    wfi
    b .Lmt68xx_lk_halt

.size _start, . - _start

/*
 * Vectors. Every one of them reports before it halts, which is the point of
 * this block: an exception that names its vector, its faulting PC and its
 * DFSR/DFAR is a two-minute diagnosis instead of a boot that just ends.
 *
 * The reporter runs in whatever mode took the exception, so each stub
 * establishes its own SP first -- only SVC and ABT get stacks at _start, and
 * UND/IRQ/FIQ would otherwise push onto whatever SP_<mode> reset left behind,
 * turning the report into a second fault. Sharing __stack_top across modes is
 * safe precisely because none of these paths return.
 */
.align 5
.global mt68xx_lk_exception_vectors
.type mt68xx_lk_exception_vectors, %function
mt68xx_lk_exception_vectors:
    b .Lmt68xx_lk_vec_reset
    b .Lmt68xx_lk_vec_undef
    b .Lmt68xx_lk_vec_svc
    b .Lmt68xx_lk_vec_pabt
    b .Lmt68xx_lk_data_abort
    b .Lmt68xx_lk_vec_reserved
    b .Lmt68xx_lk_vec_irq
    b .Lmt68xx_lk_vec_fiq

/*
 * Data abort. The resume word below lets a future fault-guarded probe take a
 * real abort on purpose and continue; anything the resume word does not claim
 * falls through to the reporter.
 *
 * Only r0-r3 are saved, and LR is deliberately left alone until the decision
 * is made: loading the resume address straight into LR would destroy lr_abt --
 * the one register that says where the fault happened -- before establishing
 * whether anyone wanted the fault.
 */
.Lmt68xx_lk_data_abort:
    stmdb sp!, {r0-r3}
    ldr r0, =mt68xx_stage1_abort_flag
    mov r1, #1
    str r1, [r0]
    ldr r0, =mt68xx_stage1_abort_resume
    ldr r1, [r0]
    cmp r1, #0
    beq .Lmt68xx_lk_vec_dabt
    mov r2, #0
    str r2, [r0]
    mov lr, r1
    ldmia sp!, {r0-r3}
    subs pc, lr, #0

.Lmt68xx_lk_vec_dabt:
    ldmia sp!, {r0-r3}
    sub lr, lr, #8              @ lr_abt = faulting instruction + 8
    mov r0, #4
    b .Lmt68xx_lk_fault

.Lmt68xx_lk_vec_reset:
    mov r0, #0
    b .Lmt68xx_lk_fault
.Lmt68xx_lk_vec_undef:
    sub lr, lr, #4
    mov r0, #1
    b .Lmt68xx_lk_fault
.Lmt68xx_lk_vec_svc:
    sub lr, lr, #4
    mov r0, #2
    b .Lmt68xx_lk_fault
.Lmt68xx_lk_vec_pabt:
    sub lr, lr, #4
    mov r0, #3
    b .Lmt68xx_lk_fault
.Lmt68xx_lk_vec_reserved:
    mov r0, #5
    b .Lmt68xx_lk_fault
.Lmt68xx_lk_vec_irq:
    sub lr, lr, #4
    mov r0, #6
    b .Lmt68xx_lk_fault
.Lmt68xx_lk_vec_fiq:
    sub lr, lr, #4
    mov r0, #7
    b .Lmt68xx_lk_fault

/*
 * r0 = vector number, lr = faulting PC. Collect the CP15 fault registers and
 * hand the lot to C:
 *
 *   mt68xx_lk_report_exception(vec, pc, spsr, dfsr, dfar, ifsr, ifar)
 *
 * Args 5-7 go on the stack under AAPCS; a fourth word is pushed with them only
 * to keep SP 8-byte aligned across the call.
 */
.Lmt68xx_lk_fault:
    ldr sp, =__stack_top
    bic sp, sp, #7
    mov r1, lr
    mrs r2, spsr
    mrc p15, 0, r3, c5, c0, 0   @ DFSR
    mrc p15, 0, r4, c6, c0, 0   @ DFAR
    mrc p15, 0, r5, c5, c0, 1   @ IFSR
    mrc p15, 0, r6, c6, c0, 2   @ IFAR
    mov r7, #0
    stmdb sp!, {r4-r7}
    bl mt68xx_lk_report_exception

.Lmt68xx_lk_unhandled_exception:
    wfi
    b .Lmt68xx_lk_unhandled_exception
