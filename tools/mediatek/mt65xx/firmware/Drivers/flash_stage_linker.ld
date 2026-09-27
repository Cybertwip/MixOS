OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    sram (rwx) : ORIGIN = 0x00110000, LENGTH = 128K
}

SECTIONS
{
    __image_base = ORIGIN(sram);
    . = ORIGIN(sram);

    .text : ALIGN(16)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > sram

    .data : ALIGN(16)
    {
        *(.data .data.*)
    } > sram

    .bss (NOLOAD) : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > sram

    /* Referenced by stage1.c's ramdisk scan; unused in the flash payload flow
     * (kept defined so --gc-sections outcomes never decide link success). */
    . = ALIGN(16);
    __image_end = .;

    __stack_top = ORIGIN(sram) + LENGTH(sram) - 0x100;

    ASSERT(__bss_end < (__stack_top - 0x2000), "MVIIFlash payload exceeds MT6592 SRAM budget")

    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
