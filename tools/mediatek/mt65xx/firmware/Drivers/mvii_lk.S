/*
 * mvii_lk.S — MVII minimal LK entry, running from the UBOOT/LK slot.
 *
 * Modelled on the stage1 entry. Two differences that matter here:
 *
 *  - .bss is zeroed. stage1 is loaded by the stock LK into a region the stock
 *    LK had already cleared; the preloader gives this image raw DRAM, and a
 *    warm reboot leaves the previous boot's data in it. Every "have I done this
 *    yet" flag in the drivers below lives in .bss.
 *  - The data-abort trampoline uses the same mt6592_stage1_abort_flag /
 *    mt6592_stage1_abort_resume pair that lcd_drv.c's fault-guarded probes
 *    expect, but the definitions come from mvii_lk_main.c rather than from
 *    mt6592_dram.c: this LK does not train DRAM (the preloader already did) and
 *    has no reason to link the whole EMI driver for two words.
 */
.section .text.start, "ax", %progbits
.arm
.global _start
.type _start, %function

.extern mt6592_stage1_abort_flag
.extern mt6592_stage1_abort_resume

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
    ldr r0, =mvii_lk_exception_vectors
    mcr p15, 0, r0, c12, c0, 0
    isb

    /* Zero .bss before any C runs. */
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
.Lmvii_lk_bss_zero:
    cmp r0, r1
    strlo r2, [r0], #4
    blo .Lmvii_lk_bss_zero

    /*
     * Re-establish the SVC stack, and note what it is NOT: __stack_top is the
     * abort stack, set above. lcd_drv.c's fault-guarded probes take real data
     * aborts during display bring-up, and the handler below opens with
     * `stmdb sp!, {r0-r12}` — with the two stacks aliased that is 52 bytes
     * written straight through the live C frame of whatever provoked the fault.
     */
    ldr sp, =__stack_top
    sub sp, sp, #0x400
    bic sp, sp, #7
    mov r0, r4
    mov r1, r5
    mov r2, r6
    mov r3, r7
    bl mvii_lk_main

.Lmvii_lk_halt:
    wfi
    b .Lmvii_lk_halt

.size _start, . - _start

/*
 * Hand control to the boot.img kernel exactly the way the stock LK does
 * (lk.full-decompile FUN_81e1955c tail: `(*entry)(0, machtype, tags)`).
 *
 *   r0 = entry, r1 = machtype, r2 = tags  ->  r0 = 0, r1 = machtype, r2 = tags
 *
 * Caches and the MMU are already off (cleared at _start and never re-enabled),
 * so the only thing left to guarantee is that the loaded image is visible to
 * the instruction side: the copies went out as Strongly-Ordered writes, and the
 * i-cache is invalidated once more here for the ARM boot protocol's sake.
 */
.global mvii_lk_jump_to_kernel
.type mvii_lk_jump_to_kernel, %function
mvii_lk_jump_to_kernel:
    mov r3, r0
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0      @ ICIALLU
    mcr p15, 0, r0, c7, c5, 6      @ BPIALL
    dsb
    isb
    bx r3
.size mvii_lk_jump_to_kernel, . - mvii_lk_jump_to_kernel

/*
 * Vectors. Every one of them reports before it halts, which is the point of
 * this block.
 *
 * They used to branch, all but the data abort, to a bare `wfi; b .`. That is
 * the correct thing to do and the worst possible thing to say: a board that
 * takes an unexpected abort in the middle of the boot.img load looks from the
 * outside exactly like a board whose CPU stopped fetching — no log line, no
 * stage change, no reset, just a boot that ends. The J36 spent three rounds of
 * telemetry in that state. An exception that names its vector, its faulting PC
 * and its DFSR/DFAR is a two-minute diagnosis instead.
 *
 * The reporter runs in whatever mode took the exception, so each stub
 * establishes its own SP first — only SVC and ABT get stacks at _start, and
 * UND/IRQ/FIQ would otherwise push onto whatever SP_<mode> reset left behind,
 * turning the report into a second fault. Sharing __stack_top across modes is
 * safe precisely because none of these paths return.
 */
.align 5
.global mvii_lk_exception_vectors
.type mvii_lk_exception_vectors, %function
mvii_lk_exception_vectors:
    b .Lmvii_lk_vec_reset
    b .Lmvii_lk_vec_undef
    b .Lmvii_lk_vec_svc
    b .Lmvii_lk_vec_pabt
    b .Lmvii_lk_data_abort
    b .Lmvii_lk_vec_reserved
    b .Lmvii_lk_vec_irq
    b .Lmvii_lk_vec_fiq

/*
 * Data abort. lcd_drv.c's fault-guarded probes take real aborts on purpose and
 * resume through here, so that path stays first and stays cheap; anything the
 * resume word does not claim falls through to the reporter.
 *
 * Only r0-r3 are saved, and LR is deliberately left alone until the decision is
 * made: the previous version loaded the resume address straight into LR, which
 * destroyed lr_abt — the one register that says where the fault happened —
 * before it had established whether anyone wanted the fault.
 */
.Lmvii_lk_data_abort:
    stmdb sp!, {r0-r3}
    ldr r0, =mt6592_stage1_abort_flag
    mov r1, #1
    str r1, [r0]
    ldr r0, =mt6592_stage1_abort_resume
    ldr r1, [r0]
    cmp r1, #0
    beq .Lmvii_lk_vec_dabt
    mov r2, #0
    str r2, [r0]
    mov lr, r1
    ldmia sp!, {r0-r3}
    subs pc, lr, #0

.Lmvii_lk_vec_dabt:
    ldmia sp!, {r0-r3}
    sub lr, lr, #8              @ lr_abt = faulting instruction + 8
    mov r0, #4
    b .Lmvii_lk_fault

.Lmvii_lk_vec_reset:
    mov r0, #0
    b .Lmvii_lk_fault
.Lmvii_lk_vec_undef:
    sub lr, lr, #4
    mov r0, #1
    b .Lmvii_lk_fault
.Lmvii_lk_vec_svc:
    sub lr, lr, #4
    mov r0, #2
    b .Lmvii_lk_fault
.Lmvii_lk_vec_pabt:
    sub lr, lr, #4
    mov r0, #3
    b .Lmvii_lk_fault
.Lmvii_lk_vec_reserved:
    mov r0, #5
    b .Lmvii_lk_fault
.Lmvii_lk_vec_irq:
    sub lr, lr, #4
    mov r0, #6
    b .Lmvii_lk_fault
.Lmvii_lk_vec_fiq:
    sub lr, lr, #4
    mov r0, #7
    b .Lmvii_lk_fault

/*
 * r0 = vector number, lr = faulting PC. Collect the CP15 fault registers and
 * hand the lot to C:
 *
 *   mvii_lk_report_exception(vec, pc, spsr, dfsr, dfar, ifsr, ifar)
 *
 * Args 5-7 go on the stack under AAPCS; a fourth word is pushed with them only
 * to keep SP 8-byte aligned across the call.
 */
.Lmvii_lk_fault:
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
    bl mvii_lk_report_exception

.Lmvii_lk_unhandled_exception:
    wfi
    b .Lmvii_lk_unhandled_exception
