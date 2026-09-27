.section .text.start, "ax", %progbits
.arm
.global _start
.type _start, %function

.extern mt6592_stage1_abort_flag
.extern mt6592_stage1_abort_resume

_start:
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
    ldr r0, =mt6592_stage1_exception_vectors
    mcr p15, 0, r0, c12, c0, 0
    isb
    ldr sp, =__stack_top
    bl mvii_arm_flash_stage_main

.Lflash_stage_halt:
    wfi
    b .Lflash_stage_halt

.size _start, . - _start

.align 5
.global mt6592_stage1_exception_vectors
.type mt6592_stage1_exception_vectors, %function
mt6592_stage1_exception_vectors:
    b .Lflash_stage_unhandled_exception
    b .Lflash_stage_unhandled_exception
    b .Lflash_stage_unhandled_exception
    b .Lflash_stage_unhandled_exception
    b .Lflash_stage_data_abort
    b .Lflash_stage_unhandled_exception
    b .Lflash_stage_unhandled_exception
    b .Lflash_stage_unhandled_exception

.Lflash_stage_data_abort:
    stmdb sp!, {r0-r12}
    ldr r0, =mt6592_stage1_abort_flag
    mov r1, #1
    str r1, [r0]
    ldr r0, =mt6592_stage1_abort_resume
    ldr lr, [r0]
    cmp lr, #0
    beq .Lflash_stage_unhandled_exception
    mov r2, #0
    str r2, [r0]
    ldmia sp!, {r0-r12}
    subs pc, lr, #0

.Lflash_stage_unhandled_exception:
    wfi
    b .Lflash_stage_unhandled_exception
