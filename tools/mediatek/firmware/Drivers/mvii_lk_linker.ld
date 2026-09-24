/*
 * mvii_lk_linker.ld — MVII minimal LK, UBOOT/LK slot image.
 *
 * The preloader reads the MTK header at the top of the UBOOT slot (eMMC USER
 * 0x1d40000) and copies `size` bytes to 0x81e00000 before branching there, so
 * this image is linked exactly where the stock MediaTek LK used to run.
 *
 * 512K of budget against a 2 MiB slot. The slot's last 64 KiB is the console
 * ring + boot-status sector (mt6592_bootstatus.h), and nothing else in the slot
 * is claimed, so the only real constraint is that the image stays clear of the
 * addresses it loads into: the boot.img kernel at 0x80008000 (8 MiB reserved by
 * the stage1 link script), the ramdisk at 0x84000000, and the LK framebuffer canvas
 * at 0x82700000. 0x81e00000 + 512K = 0x81e80000 clears all three.
 *
 * One thing does NOT come out of that budget: the Mali job buffers, which get
 * their own DRAM region below. They are scratch the GPU walks, they are four
 * times the size of the whole image, and putting them in .bss would mean the
 * bootloader could not afford a GPU at all.
 */
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    ram (rwx) : ORIGIN = 0x81e00000, LENGTH = 512K

    /*
     * Mali job buffers, out of line. mt6592_gpu_offload.c's PLB heap, tile
     * heap, PLBU array, tile list and staging arrays come to ~1.9 MiB — four
     * times the whole image budget above — and they are pure scratch the GP and
     * PP walk, never anything the slot has to carry. So they get their own
     * NOLOAD region instead of .bss, and the budget ASSERT below keeps meaning
     * what it says.
     *
     * 0x82d00000 is the DRAM hole between the LK's off-screen compose canvas
     * (0x82b00000, ending 0x82c2c000) and mvii_debug_console's WLAN firmware
     * scratch at 0x83000000, rounded up to the next megabyte so the 4 KiB
     * alignment the GP tile heap needs is free. 3 MiB reaches exactly the
     * firmware scratch and no further; overrunning it is a link error, not a
     * boot that scribbles on the connectivity firmware. It is also above
     * stage1's RAMDISK_SCAN_END (0x82600000), so the ramdisk magic scan cannot
     * mistake a half-built tile list for an image. Keep the map in
     * mvii_assets.h in step with any change here.
     */
    gpuram (rw) : ORIGIN = 0x82d00000, LENGTH = 3M
}

SECTIONS
{
    __image_base = ORIGIN(ram);
    . = ORIGIN(ram);

    .text : ALIGN(16)
    {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > ram

    .data : ALIGN(16)
    {
        *(.data .data.*)
    } > ram

    /* Zeroed by _start: the preloader hands us DRAM that a previous boot wrote,
     * and objcopy -O binary does not carry NOLOAD sections into the slot image. */
    .bss (NOLOAD) : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > ram

    . = ALIGN(2048);
    __image_end = .;

    . = ALIGN(16);
    __stack_top = ORIGIN(ram) + LENGTH(ram) - 0x1000;

    ASSERT(__bss_end < (__stack_top - 0x2000), "MVII minimal LK exceeds its UBOOT-slot budget")

    /* Placed last on purpose. An output section with a >region assignment moves
     * the location counter into that region, so anything computed off `.'
     * -- __image_end, __stack_top -- has to be settled before this point.
     *
     * Zeroed by mt6592_gpu_offload.c's probe rather than by _start: it is not in
     * .bss and it is not in the image, so the only code that knows it exists is
     * the code that has to clear it. Empty, with the two symbols equal, in any
     * image where --gc-sections dropped the GPU driver -- which is every release
     * bootloader, since nothing outside the debug console calls into it. */
    .gpubss (NOLOAD) : ALIGN(4096)
    {
        __gpu_bss_start = .;
        *(.gpubss .gpubss.*)
        . = ALIGN(4);
        __gpu_bss_end = .;
    } > gpuram

    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
