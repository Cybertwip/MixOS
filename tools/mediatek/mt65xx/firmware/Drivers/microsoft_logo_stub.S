/*
 * microsoft_logo_stub.S — zero-length stand-in for the stage1 splash blob.
 *
 * stage1.c references microsoft_logo_bgra_bin_start / _end to paint an embedded
 * BGRA splash. There used to be a real object beside this one that `.incbin`'d
 * the whole 640x480 frame (~1.2 MiB); it fitted only in the boot.img stage1
 * loader, and both that loader and that object are gone.
 *
 * What is left links this: the "blob" is 16 bytes, so (_end - _start) is far
 * smaller than one frame and stage1's paint_raw_bgra() skips the paint outright.
 * The splash a board actually shows comes from the asset slot on the LOGO
 * partition instead, which is a flash of one file rather than a rebuild.
 */
    .section .rodata.microsoft_logo, "a", %progbits
    .align 4
    .global microsoft_logo_bgra_bin_start
microsoft_logo_bgra_bin_start:
    .zero 16
    .global microsoft_logo_bgra_bin_end
microsoft_logo_bgra_bin_end:
