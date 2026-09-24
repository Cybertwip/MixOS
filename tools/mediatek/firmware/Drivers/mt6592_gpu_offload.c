/*
 * mt6592_gpu_offload.c — bare-metal Mali-450 MP4 shader pipeline.
 *
 * This is a real Utgard hardware path. It powers and clocks the MT6592 MFG
 * domain, disables the per-core Mali MMUs for the kernel's identity-mapped
 * physical buffers, submits a PLBU job for a pre-transformed fullscreen quad,
 * and then runs the linked texture-sampling fragment shader on PP0. The PP
 * writeback unit writes either RGBA8 or RGB565 directly to the destination.
 *
 * The shader and render-state constants below were produced locally from the
 * stock J36 libMali.so. The stock fixed-function shader generator produced a
 * StageSamplerNormal0 fragment shader, and the stock shader linker rewrote the
 * matching vertex varying location. The PLBU-only quad path does not execute a
 * vertex shader: it supplies pre-transformed position and TexCoord0 varying
 * buffers exactly like libMali's _mali200_draw_quad path.
 *
 * CPU conversion is deliberately not hidden in this backend. If the hardware
 * probe, self-test, alignment constraints, or a bounded GP/PP job fails, these
 * functions return -1 and the existing caller performs its explicit CPU
 * fallback. GPU capabilities are advertised only after the shader self-test
 * has rendered and read back correctly.
 *
 * ── WHAT MOVING THIS INTO LK COST ──
 *
 * This file was written against a kernel that had a libc, a heap, an MMU and a
 * float ABI. The bootloader has none of those, so three things here are not the
 * way you would write them for a hosted target:
 *
 *   - No <string.h>. The LK links no libc at all; mvii_arm_aeabi_mem.c supplies
 *     the __aeabi_* block-move entry points the compiler emits for struct
 *     assignment, and nothing else. The mem_* helpers below are the local
 *     stand-ins, and -ffreestanding -fno-builtin is what stops the optimiser
 *     rewriting them back into calls to the libc names that are not there.
 *
 *   - The big job buffers live in .gpubss, not .bss. They come to ~1.9 MiB and
 *     the LK image links into a 512 KiB window at 0x81e00000 whose linker script
 *     asserts on overrun. mvii_lk_linker.ld gives .gpubss its own NOLOAD region
 *     in the DRAM hole below the WLAN firmware scratch instead. _start does not
 *     zero it -- it is not in .bss -- so gpu_dram_zero_once() does.
 *
 *   - Float. The rest of LK is -mfloat-abi=soft with no compiler-rt, so a single
 *     __aeabi_fsub here would fail the link. This one translation unit is built
 *     -mfloat-abi=softfp -mfpu=neon-vfpv4 (same calling convention, so it links
 *     against soft-float objects unchanged) and turns the VFP on itself in
 *     vfp_enable(), which probe_once() calls before anything touches a float.
 *     Nothing before LK has enabled cp10/cp11, and the Mali needs IEEE-754 bit
 *     patterns for its vertex and texcoord arrays regardless.
 *
 *   - The job guard is a plain flag, not __sync_lock_test_and_set spun on. See
 *     gpu_lock_take(): there is one core running and no scheduler, so the spin
 *     could only ever wait on itself, and LDREX/STREX against the
 *     Strongly-Ordered memory an MMU-off core sees is not something to rely on.
 */

#include "mvii_gpu_offload.h"

#include "mt6592_bootstatus.h"
#include "mt6592_timer.h"
#include "mt6592_uart.h"

#include <stddef.h>
#include <stdint.h>

#define BIT(n) (1u << (n))

/* The job buffers the Mali reads and writes, placed out of the LK's 512 KiB
 * link window by mvii_lk_linker.ld. Everything else here is small enough to
 * stay in ordinary .bss. */
#define MALI_DRAM __attribute__((section(".gpubss")))

#ifndef MVII_MT6592_MALI_SHADER_PIPELINE
#    define MVII_MT6592_MALI_SHADER_PIPELINE 1
#endif

enum
{
    SPM_BASE         = 0x10006000u,
    SPM_MFG_PWR_CON  = SPM_BASE + 0x0214u,
    SPM_PWR_STATUS   = SPM_BASE + 0x060cu,
    SPM_PWR_STATUS_S = SPM_BASE + 0x0610u,
    MFG_PWR_STA_MASK = BIT(4),
    PWR_RST_B        = BIT(0),
    PWR_ISO          = BIT(1),
    PWR_ON           = BIT(2),
    PWR_ON_S         = BIT(3),
    PWR_CLK_DIS      = BIT(4),
    SRAM_PDN         = 0x0f00u,
    MFG_SRAM_ACK     = BIT(12),

    MFG_CONFIG_BASE   = 0x13000000u,
    MFG_CG_CLR        = MFG_CONFIG_BASE + 0x0008u,
    MALI_BASE         = 0x13040000u,
    MALI_GP_BASE      = MALI_BASE + 0x00000u,
    MALI_L2_GP_BASE   = MALI_BASE + 0x10000u,
    MALI_L2_PP_BASE   = MALI_BASE + 0x01000u,
    MALI_GP_MMU_BASE  = MALI_BASE + 0x03000u,
    MALI_PP0_MMU_BASE = MALI_BASE + 0x04000u,
    MALI_PP0_BASE     = MALI_BASE + 0x08000u,

    DISPSYS_BASE         = 0x14000000u,
    DISP_CG_CLR0         = DISPSYS_BASE + 0x0108u,
    DISP_SMI_COMMON_GATE = BIT(0),

    MALI_MMU_STATUS    = 0x0004u,
    MALI_MMU_COMMAND   = 0x0008u,
    MALI_MMU_INT_CLEAR = 0x0018u,
    MALI_MMU_INT_MASK  = 0x001cu,
    MALI_MMU_DISABLE   = 0x0001u,
    MALI_MMU_PAGING    = BIT(0),

    MALI_GP_VS_START        = 0x0000u,
    MALI_GP_VS_END          = 0x0004u,
    MALI_GP_PLBU_START      = 0x0008u,
    MALI_GP_PLBU_END        = 0x000cu,
    MALI_GP_HEAP_START      = 0x0010u,
    MALI_GP_HEAP_END        = 0x0014u,
    MALI_GP_COMMAND         = 0x0020u,
    MALI_GP_INT_RAWSTAT     = 0x0024u,
    MALI_GP_INT_CLEAR       = 0x0028u,
    MALI_GP_INT_MASK        = 0x002cu,
    MALI_GP_STATUS          = 0x0068u,
    MALI_GP_VERSION         = 0x006cu,
    MALI_GP_CMD_START_PLBU  = BIT(1),
    MALI_GP_CMD_UPDATE_HEAP = BIT(4),
    MALI_GP_CMD_SOFT_RESET  = BIT(10),
    MALI_GP_IRQ_PLBU_DONE   = BIT(1),
    MALI_GP_IRQ_PLBU_OOM    = BIT(2),
    MALI_GP_IRQ_RESET_DONE  = BIT(19),
    MALI_GP_IRQ_ERRORS = BIT(5) | BIT(6) | BIT(9) | BIT(10) | BIT(11) | BIT(13) | BIT(14) | BIT(20) | BIT(21) | BIT(22),
    MALI_GP_STATUS_ACTIVE = BIT(1) | BIT(3),

    MALI_PP_FRAME_BASE       = 0x0000u,
    MALI_PP_WB0_BASE         = 0x0100u,
    MALI_PP_VERSION          = 0x1000u,
    MALI_PP_STATUS           = 0x1008u,
    MALI_PP_CONTROL          = 0x100cu,
    MALI_PP_INT_RAWSTAT      = 0x1020u,
    MALI_PP_INT_CLEAR        = 0x1024u,
    MALI_PP_INT_MASK         = 0x1028u,
    MALI_PP_START_RENDERING  = BIT(6),
    MALI_PP_SOFT_RESET       = BIT(7),
    MALI_PP_IRQ_END_OF_FRAME = BIT(0),
    MALI_PP_IRQ_RESET_DONE   = BIT(12),
    MALI_PP_IRQ_ERRORS       = BIT(2) | BIT(3) | BIT(4) | BIT(8) | BIT(9) | BIT(10) | BIT(11),
    MALI_PP_STATUS_ACTIVE    = BIT(0),

    MALI_L2_STATUS       = 0x0008u,
    MALI_L2_COMMAND      = 0x0010u,
    MALI_L2_MAX_READS    = 0x0018u,
    MALI_L2_ENABLE       = 0x001cu,
    MALI_L2_CLEAR_ALL    = 0x0001u,
    MALI_L2_COMMAND_BUSY = BIT(0),

    MALI450_GP_PRODUCT = 0x0d07u,
    MALI450_PP_PRODUCT = 0xcf07u,

    MALI_MAX_WIDTH  = 1280u,
    MALI_MAX_HEIGHT = 720u,
    /* Destination extents are bounded by the tile-list and PLB heap sized
     * above; the source is only bounded by what the texture descriptor can
     * encode — thirteen-bit extents and a fifteen-bit byte stride. */
    MALI_TEXTURE_MAX_EXTENT = 8191u,
    MALI_TEXTURE_MAX_PITCH  = 0x7fffu,
    MALI_TILE_SIZE          = 16u,
    MALI_MAX_TILES_X     = (MALI_MAX_WIDTH + MALI_TILE_SIZE - 1u) / MALI_TILE_SIZE,
    MALI_MAX_TILES_Y     = (MALI_MAX_HEIGHT + MALI_TILE_SIZE - 1u) / MALI_TILE_SIZE,
    MALI_MAX_TILE_COUNT  = MALI_MAX_TILES_X * MALI_MAX_TILES_Y,
    MALI_PLB_TILE_BYTES  = 0x200u,
    MALI_PLB_HEAP_BYTES  = MALI_MAX_TILE_COUNT * MALI_PLB_TILE_BYTES,
    MALI_TILE_LIST_WORDS = MALI_MAX_TILE_COUNT * 4u + 4u,
    MALI_PLBU_WORDS      = 128u,
    /* Sized for the largest self-test surface: the downscale test's 48 rows at
     * its 64-pixel stride. */
    MALI_SELFTEST_PIXELS = 3072u,

    /* Indices reach the hardware as bytes -- that is what the primitive-setup
     * word this driver has proven on this board encodes -- so one batch cannot
     * name more than 256 vertices. A caller with more geometry splits it, and
     * 256 triangles per submission already amortises the job overhead many
     * times over. */
    MALI_BATCH_MAX_VERTICES  = 256u,
    MALI_BATCH_MAX_TRIANGLES = 256u,
    MALI_BATCH_MAX_INDICES   = MALI_BATCH_MAX_TRIANGLES * 3u,

    /* Utgard primitive types. The blit path draws mode 15, the rectangle
     * primitive that takes three named corners and implies the fourth; a
     * rasteriser's output is an ordinary triangle list. */
    MALI_PRIM_TRIANGLES = 4u,
    MALI_PRIM_RECTANGLE = 15u,

    /* Primitive setup words. The rectangle the blit draws is the tile-reload
     * primitive, whose setup is a bare 0x200 emitted after the render state
     * bind. An ordinary indexed triangle list is a different primitive: its
     * setup carries an extra bit and is emitted before the bind. Only one of
     * those two shapes is right for this silicon, and the boot self-test is
     * what finds out which. */
    MALI_SETUP_RECTANGLE = 0x00000200u,
    MALI_SETUP_TRIANGLES = 0x00002200u,

    /* The PLBU allocator carves overflow blocks out of a growable tile heap
     * that must not overlap the primary per-tile PLB blocks; sharing one
     * region makes the GP scribble over the lists the PP is about to walk. */
    MALI_GP_TILE_HEAP_BYTES = 64u * 1024u,

    MALI_POLL_LIMIT       = 12000000u,
    MALI_POWER_POLL_LIMIT = 1200000u,
};

/* Utgard tile-reload program: load.v $1 0.xy, texld 0, mov.v0 $0 ^tex_sampler,
 * sync, stop. This is the same two-instruction program the Utgard blit path
 * uses, so it consumes exactly one vec2 varying and one sampler and writes no
 * uniforms. The low five bits of word zero are the first instruction's length,
 * which the render state repeats in RSW[9]. */
static const uint32_t g_fragment_shader[] __attribute__((aligned(64))) = {
    0x000005e6u, 0xf1003c20u, 0x00000000u, 0x39001000u, 0x00000e4eu, 0x000007cfu, 0x00000000u, 0x00000000u,
};

/* Frame render state used by the PP tile initialisation pass. It is separate
 * from the per-primitive render state and always references the clear program. */
static const uint32_t g_clear_shader[] __attribute__((aligned(64))) = {
    0x00020425u, 0x0000000cu, 0x01e007cfu, 0xb0000000u, 0x000005f5u, 0x00000000u, 0x00000000u, 0x00000000u,
};

static uint8_t g_plb_heap[MALI_PLB_HEAP_BYTES] MALI_DRAM __attribute__((aligned(64)));
static uint8_t g_gp_tile_heap[MALI_GP_TILE_HEAP_BYTES] MALI_DRAM __attribute__((aligned(4096)));
/* The GP and PP consume different tile streams. The GP PLBU array is a flat
 * list of PLB heap addresses, while the PP tile list contains four-word
 * coordinate/address descriptors. Pointing the GP at the PP descriptors makes
 * the GP complete without filling g_plb_heap; PP then reports INVALID_PLIST. */
static uint32_t g_plbu_array[MALI_MAX_TILE_COUNT] MALI_DRAM __attribute__((aligned(64)));
static uint32_t g_tile_list[MALI_TILE_LIST_WORDS] MALI_DRAM __attribute__((aligned(64)));
static uint32_t g_plbu_commands[MALI_PLBU_WORDS] __attribute__((aligned(64)));
static uint8_t g_quad_indices[96] __attribute__((aligned(64)));
static float g_quad_positions[12] __attribute__((aligned(64)));
static float g_quad_texcoords[8] __attribute__((aligned(64)));
static uint32_t g_texture_descriptor[16] __attribute__((aligned(64)));
/* The render state's "textures address" points at a list of descriptor
 * addresses, not at a descriptor. One sampler means one live entry. */
static uint32_t g_texture_list[16] __attribute__((aligned(64)));
static uint32_t g_render_state[16] __attribute__((aligned(64)));
static uint32_t g_frame_render_state[16] __attribute__((aligned(64)));
static uint32_t g_selftest_src[MALI_SELFTEST_PIXELS] MALI_DRAM __attribute__((aligned(64)));
static uint32_t g_selftest_dst[MALI_SELFTEST_PIXELS] MALI_DRAM __attribute__((aligned(64)));
static uint16_t g_selftest_dst565[MALI_SELFTEST_PIXELS] MALI_DRAM __attribute__((aligned(64)));
/* Triangle-list staging. The vertex array is vec4 positions at the fixed
 * sixteen-byte stride the PLBU's indexed-dest command implies, and the varying
 * array is vec2 texel coordinates at the eight-byte stride the render state
 * declares -- the same two layouts the quad path uses, just longer. */
static float   g_batch_positions[MALI_BATCH_MAX_VERTICES * 4u] MALI_DRAM __attribute__((aligned(64)));
static float   g_batch_varyings[MALI_BATCH_MAX_VERTICES * 2u] MALI_DRAM __attribute__((aligned(64)));
static uint8_t g_batch_indices[MALI_BATCH_MAX_INDICES] MALI_DRAM __attribute__((aligned(64)));
/* The batch draws from its own render state rather than sharing the blit's.
 * The two differ only in which varying array they name, which is precisely the
 * field a shared buffer would make impossible to tell apart from a stale one
 * when a batch comes out sampling the previous blit's texture coordinates. */
static uint32_t g_batch_render_state[16] __attribute__((aligned(64)));
/* Second descriptor/state pair, for the rectangle that reloads the destination
 * into the tile buffer ahead of a batch that must not lose uncovered pixels.
 * It samples the destination itself, so it cannot share the source's. */
static uint32_t g_reload_texture_descriptor[16] __attribute__((aligned(64)));
static uint32_t g_reload_texture_list[16] __attribute__((aligned(64)));
static uint32_t g_reload_render_state[16] __attribute__((aligned(64)));

static volatile int g_gpu_lock;

/* The kernel this driver came from could re-enter these entry points from
 * another core, so the guard around a job used to be __sync_lock_test_and_set
 * spun on until it cleared. In LK it is a plain flag, for two reasons.
 *
 * The spin is wrong here: one core is running, the secondaries are still in
 * reset, interrupts are off and there is no scheduler, so a taken lock can only
 * have been taken by this same call chain and waiting for it is a hang. Failing
 * the job is what the whole API already does when it cannot help -- every one of
 * these functions returns -1 and the caller does the work itself.
 *
 * And the atomic is worse than useless: __sync_lock_test_and_set lowers to
 * LDREX/STREX, the MMU is off so every access on this core is Strongly-Ordered,
 * and the ARM ARM leaves exclusives to Strongly-Ordered memory CONSTRAINED
 * UNPREDICTABLE. That buys a possible abort, or an exclusive monitor that never
 * clears, in exchange for mutual exclusion against nothing at all. */
static int gpu_lock_take(void)
{
    if (g_gpu_lock != 0)
        return -1;
    g_gpu_lock = 1;
    return 0;
}

static void gpu_lock_drop(void)
{
    g_gpu_lock = 0;
}
static int g_probe_done;
static int g_hardware_ready;
static int g_pipeline_ready;
static int g_pipeline_disabled;
/* Set only after the corresponding boot self-test rendered and read back
 * correctly; the compositor asks for these through the reported caps. */
static int g_rgb565_ready;
static int g_scaled_ready;
static int g_triangle_ready;
/* Whether destination pixels no triangle covers survive a batch. The PP
 * initialises each tile before the polygon list runs, so this is a property of
 * the frame render state rather than something to assume either way; the boot
 * self-test decides it. */
static int g_triangle_preserve;
/* How the triangle-list draw encodes its primitive setup, chosen by the boot
 * self-test out of the candidates the rectangle path does not answer for. */
static uint32_t g_batch_setup_value = MALI_SETUP_TRIANGLES;
static uint32_t g_batch_setup_first = 1u;
static uint32_t g_wb_rb_swap;
static uint32_t g_gp_version;
static uint32_t g_pp_version;
static uint32_t g_dispatch_count;
static uint32_t g_dispatch_failures;
static uint32_t g_last_gp_irq;
static uint32_t g_last_gp_status;
static uint32_t g_last_pp_irq;
static uint32_t g_last_pp_status;
static uint32_t g_last_heap_used;
static uint32_t g_last_plb_word0;
static uint32_t g_last_plb_word1;
static const char* g_status = "Mali-450 shader pipeline has not been probed";

static uint32_t read32(uint32_t address)
{
    return *(volatile uint32_t*)(uintptr_t)address;
}

static void write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t*)(uintptr_t)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Local stand-ins for the three libc entry points this file used to call. The
 * loops write through volatile pointers for the reason mvii_arm_aeabi_mem.c
 * gives: a plain byte loop in a function shaped like a block move is exactly
 * what the optimiser turns back into a call to the name that is missing. */
static void mem_fill(void* dest, uint32_t bytes, uint8_t value)
{
    volatile uint8_t* d = (volatile uint8_t*)dest;
    if (((uintptr_t)d & 3u) == 0u)
    {
        const uint32_t   word = (uint32_t)value * 0x01010101u;
        volatile uint32_t* dw = (volatile uint32_t*)dest;
        while (bytes >= 4u)
        {
            *dw++ = word;
            bytes -= 4u;
        }
        d = (volatile uint8_t*)dw;
    }
    while (bytes-- != 0u)
        *d++ = value;
}

static void mem_zero(void* dest, uint32_t bytes)
{
    mem_fill(dest, bytes, 0u);
}

static void mem_copy(void* dest, const void* src, uint32_t bytes)
{
    volatile uint8_t*       d = (volatile uint8_t*)dest;
    const volatile uint8_t* s = (const volatile uint8_t*)src;
    if (((((uintptr_t)d) | ((uintptr_t)s)) & 3u) == 0u)
    {
        volatile uint32_t*       dw = (volatile uint32_t*)dest;
        const volatile uint32_t* sw = (const volatile uint32_t*)src;
        while (bytes >= 4u)
        {
            *dw++ = *sw++;
            bytes -= 4u;
        }
        d = (volatile uint8_t*)dw;
        s = (const volatile uint8_t*)sw;
    }
    while (bytes-- != 0u)
        *d++ = *s++;
}

static void copy_string(char* dst, uint32_t dst_size, const char* src)
{
    uint32_t i;

    if (!dst || dst_size == 0u)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    for (i = 0u; i + 1u < dst_size && src[i] != '\0'; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void trace(const char* text)
{
    mt6592_uart_puts("  gpu: ");
    mt6592_uart_puts(text);
    mt6592_uart_puts("\n");
    mt6592_bootstatus_log_text("gpu: ");
    mt6592_bootstatus_log_text(text);
    mt6592_bootstatus_log_text("\n");
}

static void bootstatus_hex32(uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    char out[11];
    out[0] = '0';
    out[1] = 'x';
    for (uint32_t i = 0; i < 8u; ++i)
        out[2u + i] = hex[(value >> ((7u - i) * 4u)) & 0x0fu];
    out[10] = '\0';
    mt6592_bootstatus_log_text(out);
}

static void trace_text(const char* text)
{
    mt6592_uart_puts(text);
    mt6592_bootstatus_log_text(text);
}

static void trace_field(const char* label, uint32_t value)
{
    mt6592_uart_puts(label);
    mt6592_uart_put_hex32(value);
    mt6592_bootstatus_log_text(label);
    bootstatus_hex32(value);
}

static void trace_job_failure(const char* phase)
{
    mt6592_uart_puts("  gpu: ");
    mt6592_bootstatus_log_text("gpu: ");
    trace_text(phase);
    trace_field(" gp_irq=", g_last_gp_irq);
    trace_field(" gp_status=", g_last_gp_status);
    trace_field(" pp_irq=", g_last_pp_irq);
    trace_field(" pp_status=", g_last_pp_status);
    trace_field(" heap_used=", g_last_heap_used);
    trace_field(" plb=", g_last_plb_word0);
    trace_field(",", g_last_plb_word1);
    trace_text("\n");
}

static int wait_mask(uint32_t address, uint32_t mask, uint32_t expected, uint32_t limit)
{
    for (uint32_t i = 0; i < limit; ++i)
    {
        if ((read32(address) & mask) == expected)
            return 0;
    }
    return -1;
}

static void clean_dcache_range(const void* ptr, size_t bytes)
{
    if (!ptr || bytes == 0u)
        return;
    uintptr_t line      = (uintptr_t)ptr & ~(uintptr_t)63u;
    const uintptr_t end = ((uintptr_t)ptr + bytes + 63u) & ~(uintptr_t)63u;
    for (; line < end; line += 64u)
    {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(line) : "memory"); /* DCCMVAC */
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void clean_invalidate_dcache_range(void* ptr, size_t bytes)
{
    if (!ptr || bytes == 0u)
        return;
    uintptr_t line      = (uintptr_t)ptr & ~(uintptr_t)63u;
    const uintptr_t end = ((uintptr_t)ptr + bytes + 63u) & ~(uintptr_t)63u;
    for (; line < end; line += 64u)
    {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(line) : "memory"); /* DCCIMVAC */
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void invalidate_dcache_range(void* ptr, size_t bytes)
{
    if (!ptr || bytes == 0u)
        return;
    uintptr_t line      = (uintptr_t)ptr & ~(uintptr_t)63u;
    const uintptr_t end = ((uintptr_t)ptr + bytes + 63u) & ~(uintptr_t)63u;
    for (; line < end; line += 64u)
    {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(line) : "memory"); /* DCIMVAC */
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* A destination that is a sub-rectangle of a bigger surface only owns
 * row_bytes out of every pitch bytes, and the gap between rows can be most of
 * the surface: a 128-wide sprite in a 1280-wide framebuffer touches one tenth
 * of the rows it spans. Maintaining the whole span would then cost ten times
 * the cache operations the job actually needs, which on a small blit is more
 * work than the blit. Rounding each row out to whole lines makes neighbouring
 * rows' line operations overlap, which is harmless -- they are idempotent. */
static void clean_invalidate_dcache_rect(void* base, uint32_t pitch, uint32_t row_bytes, uint32_t rows)
{
    if (pitch == row_bytes)
    {
        clean_invalidate_dcache_range(base, (size_t)pitch * rows);
        return;
    }
    for (uint32_t row = 0; row < rows; ++row)
    {
        clean_invalidate_dcache_range((uint8_t*)base + (size_t)row * pitch, row_bytes);
    }
}

static void invalidate_dcache_rect(void* base, uint32_t pitch, uint32_t row_bytes, uint32_t rows)
{
    if (pitch == row_bytes)
    {
        invalidate_dcache_range(base, (size_t)pitch * rows);
        return;
    }
    for (uint32_t row = 0; row < rows; ++row)
    {
        invalidate_dcache_range((uint8_t*)base + (size_t)row * pitch, row_bytes);
    }
}

/* The pre-job pass over a destination the job overwrites completely and never
 * reads back.
 *
 * Clean+invalidate is what a destination needs in general, and the reload path
 * below still needs it -- there the frame is a texture as well as a target, so
 * the CPU's version of it has to reach memory. But the writeback unit fills
 * every pixel of the frame it is given whether a primitive covered it or not,
 * so on a job with no reload the whole destination is about to be replaced, and
 * cleaning it first spends a full surface of DRAM writes on pixels with nothing
 * left to say. On the compositor's 960x720 target that is 1.4 MB a frame thrown
 * away immediately after being written.
 *
 * Only the two cache lines straddling the ends of the range hold bytes that
 * belong to somebody else, so those keep their writeback and everything between
 * them is dropped. Kept to the contiguous case on purpose: with a pitch wider
 * than the rows, the gaps belong to a neighbour and adjacent rows can share a
 * line, so one row's discard could drop bytes another row's clean had just
 * written back -- the row loop's operations are only idempotent while they are
 * all the same operation. */
static void discard_dcache_range(void* ptr, size_t bytes)
{
    const uintptr_t begin = (uintptr_t)ptr;
    const uintptr_t end   = begin + bytes;
    const uintptr_t first = begin & ~(uintptr_t)63u;
    const uintptr_t last  = (end - 1u) & ~(uintptr_t)63u;
    uintptr_t line;

    /* A range inside one line is partial at both ends and has no interior to
     * save anything on. */
    if (first == last)
    {
        clean_invalidate_dcache_range(ptr, bytes);
        return;
    }
    if (begin != first)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(first) : "memory"); /* DCCIMVAC */
    else
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(first) : "memory"); /* DCIMVAC */
    for (line = first + 64u; line < last; line += 64u)
    {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(line) : "memory"); /* DCIMVAC */
    }
    if (end != last + 64u)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(last) : "memory"); /* DCCIMVAC */
    else
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(last) : "memory"); /* DCIMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

static void discard_dcache_rect(void* base, uint32_t pitch, uint32_t row_bytes, uint32_t rows)
{
    uint32_t row;

    if (!base || rows == 0u || row_bytes == 0u)
        return;
    if (pitch == row_bytes)
    {
        discard_dcache_range(base, (size_t)pitch * rows);
        return;
    }
    /* Strided, so each row keeps its own ends -- but only while the gap to the
     * next row is wide enough that the two cannot share a line. Below that a
     * neighbour's bytes ride in this row's last line, and discarding would drop
     * what the previous row's writeback had just saved; the row loop's
     * operations are idempotent only while they are all the same operation. */
    if (pitch < row_bytes || pitch - row_bytes < 64u)
    {
        clean_invalidate_dcache_rect(base, pitch, row_bytes, rows);
        return;
    }
    for (row = 0; row < rows; ++row)
    {
        discard_dcache_range((uint8_t*)base + (size_t)row * pitch, row_bytes);
    }
}

static int mfg_power_on(void)
{
    uint32_t value = read32(SPM_MFG_PWR_CON);
    if ((read32(SPM_PWR_STATUS) & MFG_PWR_STA_MASK) == 0u || (read32(SPM_PWR_STATUS_S) & MFG_PWR_STA_MASK) == 0u)
    {
        write32(SPM_MFG_PWR_CON, value | PWR_ON);
        write32(SPM_MFG_PWR_CON, read32(SPM_MFG_PWR_CON) | PWR_ON_S);
        if (wait_mask(SPM_PWR_STATUS, MFG_PWR_STA_MASK, MFG_PWR_STA_MASK, MALI_POWER_POLL_LIMIT) != 0 ||
            wait_mask(SPM_PWR_STATUS_S, MFG_PWR_STA_MASK, MFG_PWR_STA_MASK, MALI_POWER_POLL_LIMIT) != 0)
        {
            return -1;
        }
        value = read32(SPM_MFG_PWR_CON);
        value &= ~(PWR_CLK_DIS | PWR_ISO);
        value |= PWR_RST_B;
        write32(SPM_MFG_PWR_CON, value);
        write32(SPM_MFG_PWR_CON, read32(SPM_MFG_PWR_CON) & ~SRAM_PDN);
        if (wait_mask(SPM_MFG_PWR_CON, MFG_SRAM_ACK, 0u, MALI_POWER_POLL_LIMIT) != 0)
            return -1;
    }

    /* Stock MT6592 enables both SMI common and the MFG G3D clock. The display
     * path already owns SMI common, so this is additive and idempotent. */
    write32(DISP_CG_CLR0, DISP_SMI_COMMON_GATE);
    write32(MFG_CG_CLR, BIT(0));
    return 0;
}

static int mmu_disable(uint32_t base)
{
    write32(base + MALI_MMU_INT_MASK, 0u);
    write32(base + MALI_MMU_INT_CLEAR, 0xffffffffu);
    write32(base + MALI_MMU_COMMAND, MALI_MMU_DISABLE);
    return wait_mask(base + MALI_MMU_STATUS, MALI_MMU_PAGING, 0u, MALI_POWER_POLL_LIMIT);
}

static int gp_reset(void)
{
    write32(MALI_GP_BASE + MALI_GP_INT_MASK, 0u);
    write32(MALI_GP_BASE + MALI_GP_INT_CLEAR, 0xffffffffu);
    write32(MALI_GP_BASE + MALI_GP_COMMAND, MALI_GP_CMD_SOFT_RESET);
    for (uint32_t i = 0; i < MALI_POWER_POLL_LIMIT; ++i)
    {
        if (read32(MALI_GP_BASE + MALI_GP_INT_RAWSTAT) & MALI_GP_IRQ_RESET_DONE)
        {
            write32(MALI_GP_BASE + MALI_GP_INT_CLEAR, 0xffffffffu);
            return 0;
        }
    }
    return -1;
}

static int pp_reset(void)
{
    write32(MALI_PP0_BASE + MALI_PP_INT_MASK, 0u);
    write32(MALI_PP0_BASE + MALI_PP_INT_RAWSTAT, 0xffffffffu);
    write32(MALI_PP0_BASE + MALI_PP_CONTROL, MALI_PP_SOFT_RESET);
    for (uint32_t i = 0; i < MALI_POWER_POLL_LIMIT; ++i)
    {
        const uint32_t raw = read32(MALI_PP0_BASE + MALI_PP_INT_RAWSTAT);
        if ((read32(MALI_PP0_BASE + MALI_PP_STATUS) & MALI_PP_STATUS_ACTIVE) == 0u && (raw & MALI_PP_IRQ_RESET_DONE))
        {
            write32(MALI_PP0_BASE + MALI_PP_INT_CLEAR, 0xffffffffu);
            return 0;
        }
    }
    return -1;
}

static void l2_prepare(uint32_t base)
{
    write32(base + MALI_L2_MAX_READS, 0x1cu);
    write32(base + MALI_L2_ENABLE, 0x3u);
    write32(base + MALI_L2_COMMAND, MALI_L2_CLEAR_ALL);
    (void)wait_mask(base + MALI_L2_STATUS, MALI_L2_COMMAND_BUSY, 0u, MALI_POWER_POLL_LIMIT);
}

static int mali_probe_hardware(void)
{
#if !MVII_MT6592_MALI_SHADER_PIPELINE
    return -1;
#else
    if (mfg_power_on() != 0)
    {
        g_status = "MT6592 MFG power domain did not become ready";
        return -1;
    }

    g_gp_version = read32(MALI_GP_BASE + MALI_GP_VERSION);
    g_pp_version = read32(MALI_PP0_BASE + MALI_PP_VERSION);
    if ((g_gp_version >> 16) != MALI450_GP_PRODUCT || (g_pp_version >> 16) != MALI450_PP_PRODUCT)
    {
        g_status = "MT6592 MFG is powered but Mali-450 GP/PP version probe failed";
        return -1;
    }

    if (mmu_disable(MALI_GP_MMU_BASE) != 0 || mmu_disable(MALI_PP0_MMU_BASE) != 0 || gp_reset() != 0 || pp_reset() != 0)
    {
        g_status = "Mali-450 reset or identity-address MMU setup failed";
        return -1;
    }

    l2_prepare(MALI_L2_GP_BASE);
    l2_prepare(MALI_L2_PP_BASE);
    g_status = "Mali-450 MP4 GP/PP hardware ready; validating texture shader";
    return 0;
#endif
}

/* UINT32_MAX is the overflow marker and has to survive being fed back in, now
 * that a stream is assembled from several calls rather than one straight run:
 * without the first test, cursor + 2 wraps back into range and the next pair
 * would be written past the end of the array. */
static uint32_t emit_pair(uint32_t cursor, uint32_t value, uint32_t command)
{
    if (cursor == UINT32_MAX || cursor + 2u > MALI_PLBU_WORDS)
        return UINT32_MAX;
    g_plbu_commands[cursor++] = value;
    g_plbu_commands[cursor++] = command;
    return cursor;
}

static uint32_t float_bits(float value)
{
    union
    {
        float f;
        uint32_t u;
    } convert;
    convert.f = value;
    return convert.u;
}

/* Frame setup and viewport: everything a PLBU-only job states once, before any
 * primitive. The encodings are the Utgard blit path's, unchanged. */
static uint32_t emit_plbu_prologue(uint32_t width, uint32_t height, uint32_t tile_count)
{
    const uint32_t tiles_x         = (width + 15u) >> 4;
    const uint32_t tiles_y         = (height + 15u) >> 4;
    const uint32_t tile_dimensions = (tiles_y - 1u) * 0x100u | (tiles_x - 1u) * 0x01000000u;
    uint32_t cursor                = 0u;

    cursor = emit_pair(cursor, 0x00000200u, 0x1000010bu);
    cursor = emit_pair(cursor, 0u, 0x1000010cu); /* no tile grouping shifts */
    cursor = emit_pair(cursor, tile_dimensions, 0x10000109u);
    cursor = emit_pair(cursor, tiles_x & 0xffu, 0x30000000u);
    /* Block count is encoded as count-1 with bit zero forced set. A single-tile
     * job therefore encodes 0x28000001, not 0x28000000. */
    cursor = emit_pair(cursor, (uint32_t)(uintptr_t)g_plbu_array, 0x28000000u | ((tile_count - 1u) | 1u));

    cursor = emit_pair(cursor, 0u, 0x10000107u);
    cursor = emit_pair(cursor, float_bits((float)width), 0x10000108u);
    cursor = emit_pair(cursor, 0u, 0x10000105u);
    cursor = emit_pair(cursor, float_bits((float)height), 0x10000106u);
    return cursor;
}

/* One indexed draw: bind the render state and its vertex array, scissor to the
 * whole frame, then name the index array and the primitive.
 *
 * The scissor is deliberately not a parameter. The frame this driver submits is
 * always exactly the rectangle the caller asked to touch, because everything
 * inside the frame gets written back whether a primitive covered it or not --
 * so a scissor narrower than the frame would not protect the pixels outside it,
 * it would only stop them being drawn before they were overwritten anyway.
 *
 * Emitting more than one of these is what puts a reload rectangle in front of a
 * batch: primitives land in the tile buffer in list order and the blend is a
 * plain replace, so a later triangle wins over the reloaded pixel underneath. */
static uint32_t emit_plbu_draw(uint32_t cursor,
                               uint32_t width,
                               uint32_t height,
                               uint32_t vertex_address,
                               uint32_t rsw_address,
                               uint32_t index_address,
                               uint32_t primitive_mode,
                               uint32_t index_count,
                               uint32_t setup_value,
                               uint32_t setup_first)
{
    if (setup_first)
        cursor = emit_pair(cursor, setup_value, 0x1000010bu);
    cursor = emit_pair(cursor, rsw_address, 0x80000000u | (vertex_address >> 4));
    cursor = emit_pair(cursor, (height - 1u) << 15, 0x70000000u | ((width - 1u) << 13));
    if (!setup_first)
        cursor = emit_pair(cursor, setup_value, 0x1000010bu);
    cursor = emit_pair(cursor, 0u, 0x1000010au);
    cursor = emit_pair(cursor, index_address, 0x10000101u);
    cursor = emit_pair(cursor, vertex_address, 0x10000100u);
    /* Draw elements: the value word carries the index count's low byte in its
     * top byte and the first index in the rest, the command word the primitive
     * mode and the count's high bits. */
    cursor = emit_pair(cursor, index_count << 24, 0x00200000u | (primitive_mode << 16) | (index_count >> 8));
    return cursor;
}

static uint32_t emit_plbu_end(uint32_t cursor)
{
    return emit_pair(cursor, 0u, 0x50000000u);
}

/* The blit's whole command stream: one rectangle primitive over the surface.
 * Mode 15 takes the three named corners and implies the fourth. Setup word
 * 0x200 after the render state bind is the reload rectangle's encoding, which
 * is what this primitive is. */
static uint32_t build_plbu_commands(uint32_t width,
                                    uint32_t height,
                                    uint32_t tile_count,
                                    uint32_t vertex_address,
                                    uint32_t rsw_address)
{
    uint32_t cursor = emit_plbu_prologue(width, height, tile_count);
    cursor = emit_plbu_draw(cursor, width, height, vertex_address, rsw_address, (uint32_t)(uintptr_t)g_quad_indices,
                            MALI_PRIM_RECTANGLE, 3u, MALI_SETUP_RECTANGLE, 0u);
    return emit_plbu_end(cursor);
}

static void build_tile_list(uint32_t width, uint32_t height)
{
    const uint32_t tiles_x = (width + 15u) >> 4;
    const uint32_t tiles_y = (height + 15u) >> 4;
    uint32_t out           = 0u;
    uint32_t tile          = 0u;

    for (uint32_t y = 0; y < tiles_y; ++y)
    {
        for (uint32_t x = 0; x < tiles_x; ++x, ++tile)
        {
            const uint32_t heap = (uint32_t)(uintptr_t)(g_plb_heap + tile * MALI_PLB_TILE_BYTES);
            g_plbu_array[tile]  = heap;
            g_tile_list[out++]  = 0u;
            g_tile_list[out++]  = 0xb8000000u | (y << 8) | x;
            g_tile_list[out++]  = ((heap >> 3) & 0x1ffffffcu) | 0xe0000002u;
            g_tile_list[out++]  = 0xb0000000u;
        }
    }
    /* The terminator is a full four-word slot; the PP prefetches past the
     * 0xbc000000 opcode and must not read uninitialised memory. */
    g_tile_list[out++] = 0u;
    g_tile_list[out++] = 0xbc000000u;
    g_tile_list[out++] = 0u;
    g_tile_list[out++] = 0u;
}

static void build_texture_descriptor_into(uint32_t* descriptor,
                                          uint32_t* list,
                                          const uint8_t* src,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t pitch)
{
    const uint32_t address = (uint32_t)(uintptr_t)src;
    mem_zero(descriptor, 16u * sizeof(uint32_t));

    /* Pixel format 3 (RGBA8888) maps to Mali texel format 22. Linear layout,
     * nearest sampling, clamp to edge, unnormalised coordinates, one level.
     * Word 0 also carries the byte stride, enabled by "has stride" in word 2. */
    descriptor[0] = 0x00000016u | ((pitch & 0x7fffu) << 16);
    descriptor[1] = 0x00000480u;
    descriptor[2] = 0x00093900u | ((width & 0x3ffu) << 22);
    descriptor[3] = 0x00010000u | ((width >> 10) & 0x7u) | ((height & 0x1fffu) << 3);
    descriptor[6] = ((address >> 6) & 0x3u) << 30;
    descriptor[7] = (address >> 8) & 0x00ffffffu;

    mem_zero(list, 16u * sizeof(uint32_t));
    list[0] = (uint32_t)(uintptr_t)descriptor;
}

static void build_texture_descriptor(const uint8_t* src, uint32_t width, uint32_t height, uint32_t pitch)
{
    build_texture_descriptor_into(g_texture_descriptor, g_texture_list, src, width, height, pitch);
}

/* One per-primitive render state. Which texture it samples and where its
 * varyings live are the only things that vary between the blit's rectangle and
 * a rasteriser batch, so they are arguments; everything else is fixed by the
 * one fragment program this driver has. */
static void build_render_state_into(uint32_t* rsw, uint32_t texture_list_address, uint32_t varying_address)
{
    /* Blend add, src one / dst zero, depth+stencil+alpha compare always,
     * viewport near 0.0 far 1.0, colour mask 0xf, sample mask 0xf. Word two's
     * "unknown" nibble is zero, which is the blit rather than draw variant.
     * Word 10 declares varying zero as vec2 fp32; word 13 pairs the resulting
     * eight-byte varying stride with a single sampler. The reload program
     * reads no uniforms, so no uniform block is advertised. */
    static const uint32_t baseline[16] = {
        0x00000000u, 0x00000000u, 0xf03b1ad2u, 0x0000000eu, 0xffff0000u, 0x00000007u, 0x00000007u, 0x00000000u,
        0x0000f007u, 0x00000000u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00004021u, 0x00000000u, 0x00000000u,
    };

    mem_copy(rsw, baseline, 16u * sizeof(uint32_t));
    rsw[9]  = (uint32_t)(uintptr_t)g_fragment_shader | (g_fragment_shader[0] & 0x1fu);
    rsw[12] = texture_list_address;
    rsw[15] = varying_address;
}

/* The PP frame render state drives tile initialisation and is distinct from the
 * per-primitive state referenced by the PLBU command list. It clears, which is
 * why a job that must not lose uncovered pixels has to reload them itself. */
static void build_frame_render_state(void)
{
    mem_zero(g_frame_render_state, sizeof(g_frame_render_state));
    g_frame_render_state[8]  = 0x0000f008u;
    g_frame_render_state[9]  = (uint32_t)(uintptr_t)g_clear_shader;
    g_frame_render_state[13] = 0x00000100u;
}

static void build_render_state(void)
{
    build_render_state_into(g_render_state, (uint32_t)(uintptr_t)g_texture_list,
                            (uint32_t)(uintptr_t)g_quad_texcoords);
    build_frame_render_state();
}

static void build_quad(uint32_t dst_width, uint32_t dst_height, uint32_t src_width, uint32_t src_height)
{
    mem_zero(g_quad_indices, sizeof(g_quad_indices));
    g_quad_indices[0] = 0u;
    g_quad_indices[1] = 1u;
    g_quad_indices[2] = 2u;

    /* Primitive mode 0xf takes the three named corners of a rectangle and
     * implies the fourth, so the middle vertex is the shared corner. This is
     * the vertex order and winding the Utgard tile-reload path uses. */
    g_quad_positions[0]  = (float)dst_width;
    g_quad_positions[1]  = 0.0f;
    g_quad_positions[2]  = 0.0f;
    g_quad_positions[3]  = 1.0f;
    g_quad_positions[4]  = 0.0f;
    g_quad_positions[5]  = 0.0f;
    g_quad_positions[6]  = 0.0f;
    g_quad_positions[7]  = 1.0f;
    g_quad_positions[8]  = 0.0f;
    g_quad_positions[9]  = (float)dst_height;
    g_quad_positions[10] = 0.0f;
    g_quad_positions[11] = 1.0f;

    /* One vec2 varying per vertex. The texture descriptor selects unnormalised
     * coordinates, so it carries texel coordinates rather than the 0..1 range.
     * Giving the varying the source extent while the position carries the
     * destination extent makes the rasteriser's own linear interpolation do the
     * nearest-neighbour rescale: fragment (x+0.5, y+0.5) samples texel
     * ((x+0.5)*src/dst, (y+0.5)*src/dst). When the two extents match this is the
     * identity mapping, so the unscaled path is unchanged.
     * The trailing pair pads the array out to the sixteen-byte alignment the
     * render state's varying address encoding requires. */
    g_quad_texcoords[0] = (float)src_width;
    g_quad_texcoords[1] = 0.0f;
    g_quad_texcoords[2] = 0.0f;
    g_quad_texcoords[3] = 0.0f;
    g_quad_texcoords[4] = 0.0f;
    g_quad_texcoords[5] = (float)src_height;
    g_quad_texcoords[6] = 0.0f;
    g_quad_texcoords[7] = 0.0f;
}

static int run_gp(uint32_t command_words)
{
    const uint32_t start = (uint32_t)(uintptr_t)g_plbu_commands;
    const uint32_t heap  = (uint32_t)(uintptr_t)g_gp_tile_heap;
    write32(MALI_GP_BASE + MALI_GP_INT_MASK, 0u);
    write32(MALI_GP_BASE + MALI_GP_INT_CLEAR, 0xffffffffu);
    write32(MALI_GP_BASE + MALI_GP_VS_START, 0u);
    write32(MALI_GP_BASE + MALI_GP_VS_END, 0u);
    write32(MALI_GP_BASE + MALI_GP_PLBU_START, start);
    write32(MALI_GP_BASE + MALI_GP_PLBU_END, start + command_words * sizeof(uint32_t));
    write32(MALI_GP_BASE + MALI_GP_HEAP_START, heap);
    write32(MALI_GP_BASE + MALI_GP_HEAP_END, heap + MALI_GP_TILE_HEAP_BYTES);
    write32(MALI_GP_BASE + MALI_GP_COMMAND, MALI_GP_CMD_UPDATE_HEAP);
    write32(MALI_GP_BASE + MALI_GP_COMMAND, MALI_GP_CMD_START_PLBU);

    for (uint32_t i = 0; i < MALI_POLL_LIMIT; ++i)
    {
        const uint32_t raw = read32(MALI_GP_BASE + MALI_GP_INT_RAWSTAT);
        g_last_gp_irq      = raw;
        g_last_gp_status   = read32(MALI_GP_BASE + MALI_GP_STATUS);
        if (raw & (MALI_GP_IRQ_ERRORS | MALI_GP_IRQ_PLBU_OOM))
        {
            write32(MALI_GP_BASE + MALI_GP_INT_CLEAR, 0xffffffffu);
            return -1;
        }
        if (raw & MALI_GP_IRQ_PLBU_DONE)
        {
            write32(MALI_GP_BASE + MALI_GP_INT_CLEAR, 0xffffffffu);
            return 0;
        }
    }
    g_last_gp_irq    = read32(MALI_GP_BASE + MALI_GP_INT_RAWSTAT);
    g_last_gp_status = read32(MALI_GP_BASE + MALI_GP_STATUS);
    return -1;
}

static void write_pp_frame_registers(uint32_t width, uint32_t height)
{
    uint32_t frame[23] = {0};
    frame[0]           = (uint32_t)(uintptr_t)g_tile_list; /* PLBU array address */
    frame[1]           = (uint32_t)(uintptr_t)g_frame_render_state;
    frame[2]           = 0u;               /* vertex address, unused by the PP */
    frame[3]           = 2u;               /* early Z */
    frame[10]          = width - 1u;       /* bounding box right, encoded minus one */
    frame[11]          = height - 1u;      /* bounding box bottom, encoded minus one */
    frame[16]          = 1u;               /* origin X */
    frame[17]          = height * 2u - 1u; /* origin Y, encoded minus one */
    frame[18]          = 0x77u;            /* subpixel specifier */
    frame[19]          = 1u;               /* tiebreak mode */
    frame[20]          = 0u;               /* polygon tile amounts, no shifts */
    /* scale fragcoord/derivatives plus flip dithering/fragcoord/derivatives */
    frame[21] = 0x00000e0cu;
    frame[22] = 0x8888u; /* tilebuffer channel layout, 8 bits per channel */

    for (uint32_t i = 0; i < 23u; ++i)
    {
        if (i == 14u || i == 15u)
            continue;
        write32(MALI_PP0_BASE + MALI_PP_FRAME_BASE + i * 4u, frame[i]);
    }
}

static void write_pp_writeback(uint8_t* dst, uint32_t width, uint32_t pitch, uint32_t pixel_format)
{
    uint32_t wb[12] = {0};
    /* Writeback source type: one selects depth/stencil, two selects the colour
     * tile buffer. Asking for depth here makes the PP report end-of-frame
     * without ever writing a colour pixel. */
    wb[0] = 2u;
    wb[1] = (uint32_t)(uintptr_t)dst;
    wb[2] = pixel_format;
    wb[3] = 0u;
    wb[4] = 0u; /* linear */
    wb[5] = pitch >> 3;
    /* Word six is the writeback block's MRT control. Its low bits select extra
     * render targets -- which is why setting bit zero to mean "swap" did
     * nothing at all -- and bit two is the red/blue swap. Utgard names the
     * packed 16-bit writeback format B5G6R5 while the shell framebuffer is
     * R5G6B5, so one of them is needed; which one is decided by the boot
     * self-test rather than assumed here. */
    wb[6] = (pixel_format == 0u) ? g_wb_rb_swap : 0u;
    (void)width;
    for (uint32_t i = 0; i < 12u; ++i)
    {
        write32(MALI_PP0_BASE + MALI_PP_WB0_BASE + i * 4u, wb[i]);
    }
}

static int run_pp(uint8_t* dst, uint32_t width, uint32_t height, uint32_t dst_pitch, uint32_t pixel_format)
{
    write32(MALI_PP0_BASE + MALI_PP_INT_MASK, 0u);
    write32(MALI_PP0_BASE + MALI_PP_INT_CLEAR, 0xffffffffu);
    write_pp_frame_registers(width, height);
    write_pp_writeback(dst, width, dst_pitch, pixel_format);
    write32(MALI_PP0_BASE + MALI_PP_CONTROL, MALI_PP_START_RENDERING);

    for (uint32_t i = 0; i < MALI_POLL_LIMIT; ++i)
    {
        const uint32_t raw = read32(MALI_PP0_BASE + MALI_PP_INT_RAWSTAT);
        g_last_pp_irq      = raw;
        g_last_pp_status   = read32(MALI_PP0_BASE + MALI_PP_STATUS);
        if (raw & MALI_PP_IRQ_ERRORS)
        {
            write32(MALI_PP0_BASE + MALI_PP_INT_CLEAR, 0xffffffffu);
            return -1;
        }
        if (raw & MALI_PP_IRQ_END_OF_FRAME)
        {
            write32(MALI_PP0_BASE + MALI_PP_INT_CLEAR, 0xffffffffu);
            return 0;
        }
    }
    g_last_pp_irq    = read32(MALI_PP0_BASE + MALI_PP_INT_RAWSTAT);
    g_last_pp_status = read32(MALI_PP0_BASE + MALI_PP_STATUS);
    return -1;
}

static int shader_blit_scaled(const uint8_t* src,
                              uint8_t* dst,
                              uint32_t src_width,
                              uint32_t src_height,
                              uint32_t dst_width,
                              uint32_t dst_height,
                              uint32_t src_pitch,
                              uint32_t dst_pitch,
                              uint32_t dst_pixel_format)
{
    const uint32_t tiles_x    = (dst_width + 15u) >> 4;
    const uint32_t tiles_y    = (dst_height + 15u) >> 4;
    const uint32_t tile_count = tiles_x * tiles_y;
    const uint32_t heap_bytes = tile_count * MALI_PLB_TILE_BYTES;
    /* Only these bytes of each destination row belong to the job; on a
     * sub-rectangle the rest of the row is somebody else's pixels. */
    const uint32_t dst_row_bytes = dst_width * (dst_pixel_format == 0u ? 2u : 4u);
    uint32_t command_words;

    const uintptr_t src_begin = (uintptr_t)src;
    const uintptr_t src_end   = src_begin + (size_t)src_pitch * src_height;
    const uintptr_t dst_begin = (uintptr_t)dst;
    /* The last row stops at its own width, so a destination sitting at the
     * bottom of its surface does not make this run past the allocation. */
    const uintptr_t dst_end = dst_begin + (size_t)dst_pitch * (dst_height - 1u) + dst_row_bytes;
    if (!g_hardware_ready || g_pipeline_disabled || !src || !dst || dst_width == 0u || dst_height == 0u ||
        src_width == 0u || src_height == 0u || dst_width > MALI_MAX_WIDTH || dst_height > MALI_MAX_HEIGHT ||
        tile_count > MALI_MAX_TILE_COUNT ||
        /* The descriptor's extent fields are thirteen bits wide and its byte
         * stride fifteen, which also bounds how far a texel fetch can reach. */
        src_width > MALI_TEXTURE_MAX_EXTENT || src_height > MALI_TEXTURE_MAX_EXTENT ||
        src_pitch < src_width * 4u || src_pitch > MALI_TEXTURE_MAX_PITCH ||
        /* A tightly packed source is the long-proven case and stays allowed at
         * any width; anything wider than its rows only gets fetched correctly
         * when each row starts on a cache line, so require that of a real
         * stride rather than trusting an arbitrary one. */
        (src_pitch != src_width * 4u && (src_pitch & 63u) != 0u) ||
        dst_pitch < dst_width * (dst_pixel_format == 0u ? 2u : 4u) ||
        /* The texture descriptor drops the low six bits of the base address
         * (word six carries bits 7:6 and word seven bits 31:8), so a source
         * that is not 64-byte aligned would be sampled from the wrong place.
         * The writeback address has no such encoding: it is a plain 32-bit
         * register, and eight bytes is the granularity the unit's pitch field
         * already works in. Requiring 64 there too would have ruled out every
         * sub-rectangle destination -- a sprite blitted into a framebuffer
         * starts at an arbitrary pixel -- which is exactly the case this path
         * exists to serve. Cache maintenance is unaffected: the pre-pass is a
         * clean+invalidate, so the partial lines at either end are written back
         * before they are dropped, and nothing writes them again until the
         * synchronous job has finished. */
        ((uintptr_t)src & 63u) != 0u || ((uintptr_t)dst & 7u) != 0u || (dst_pitch & 7u) != 0u ||
        (src_begin < dst_end && dst_begin < src_end))
    {
        return -1;
    }

    if (gpu_lock_take() != 0)
        return -1;

    mem_zero(g_plb_heap, heap_bytes);
    build_tile_list(dst_width, dst_height);
    build_quad(dst_width, dst_height, src_width, src_height);
    build_texture_descriptor(src, src_width, src_height, src_pitch);
    build_render_state();
    command_words = build_plbu_commands(dst_width, dst_height, tile_count, (uint32_t)(uintptr_t)g_quad_positions,
                                        (uint32_t)(uintptr_t)g_render_state);
    if (command_words == UINT32_MAX)
    {
        gpu_lock_drop();
        return -1;
    }

    /* Only the used part of the last row matters, and on a strided source that
     * is a large fraction: the compositor's 640-pixel window sits in a
     * 1280-pixel-pitch surface, so cleaning whole rows would clean twice the
     * memory the texture unit ever reads. */
    clean_dcache_range(src, (size_t)src_pitch * (src_height - 1u) + (size_t)src_width * 4u);
    /* Nothing here samples the destination -- the source is a separate surface
     * and the overlap check above guarantees it -- so the old contents go. */
    discard_dcache_rect(dst, dst_pitch, dst_row_bytes, dst_height);
    clean_dcache_range(g_fragment_shader, sizeof(g_fragment_shader));
    clean_dcache_range(g_clear_shader, sizeof(g_clear_shader));
    clean_dcache_range(g_texture_descriptor, sizeof(g_texture_descriptor));
    clean_dcache_range(g_texture_list, sizeof(g_texture_list));
    clean_dcache_range(g_render_state, sizeof(g_render_state));
    clean_dcache_range(g_frame_render_state, sizeof(g_frame_render_state));
    clean_dcache_range(g_quad_indices, sizeof(g_quad_indices));
    clean_dcache_range(g_quad_positions, sizeof(g_quad_positions));
    clean_dcache_range(g_quad_texcoords, sizeof(g_quad_texcoords));
    clean_dcache_range(g_plbu_array, (size_t)tile_count * sizeof(g_plbu_array[0]));
    clean_dcache_range(g_tile_list, (size_t)(tile_count * 4u + 4u) * sizeof(uint32_t));
    clean_dcache_range(g_plbu_commands, (size_t)command_words * sizeof(uint32_t));
    /* The GP writes the polygon lists and the overflow heap, and the PP reads
     * them back. Drop the CPU copies entirely so no stale line is written back
     * over GP output between the two jobs. */
    clean_invalidate_dcache_range(g_plb_heap, heap_bytes);
    clean_invalidate_dcache_range(g_gp_tile_heap, sizeof(g_gp_tile_heap));
    l2_prepare(MALI_L2_GP_BASE);
    l2_prepare(MALI_L2_PP_BASE);

    int rc = run_gp(command_words);
    if (rc == 0)
    {
        /* How far the PLBU allocator advanced, and the head of the first
         * polygon list block, tell apart "the GP binned nothing" from "the PP
         * mis-parsed a list the GP really wrote". */
        g_last_heap_used = read32(MALI_GP_BASE + MALI_GP_HEAP_START) - (uint32_t)(uintptr_t)g_gp_tile_heap;
        invalidate_dcache_range(g_plb_heap, 64u);
        g_last_plb_word0 = ((const volatile uint32_t*)g_plb_heap)[0];
        g_last_plb_word1 = ((const volatile uint32_t*)g_plb_heap)[1];

        /* The PP reads the polygon lists through its own L2, which must not
         * serve lines cached before the GP filled them. */
        l2_prepare(MALI_L2_PP_BASE);
        rc = run_pp(dst, dst_width, dst_height, dst_pitch, dst_pixel_format);
    }
    if (rc == 0)
    {
        invalidate_dcache_rect(dst, dst_pitch, dst_row_bytes, dst_height);
        ++g_dispatch_count;
    }
    else
    {
        ++g_dispatch_failures;
        if (g_pipeline_ready)
        {
            g_pipeline_ready    = 0;
            g_pipeline_disabled = 1;
            g_status            = "Mali-450 shader dispatch failed; hardware path disabled for this boot";
            trace_job_failure("runtime shader dispatch failed");
        }
    }
    gpu_lock_drop();
    return rc;
}

static int shader_blit(const uint8_t* src,
                       uint8_t* dst,
                       uint32_t width,
                       uint32_t height,
                       uint32_t src_pitch,
                       uint32_t dst_pitch,
                       uint32_t dst_pixel_format)
{
    return shader_blit_scaled(src, dst, width, height, width, height, src_pitch, dst_pitch, dst_pixel_format);
}

/* Geometry arrives from a rasteriser, so it can hold whatever a vertex shader
 * and a clipper produced -- including values a divide by a near-zero w turned
 * into an infinity. The PLBU has no opinion about that and will happily bin a
 * primitive whose bounding box covers the whole address space, so bound it here
 * instead. The limit is far outside any surface this driver accepts, which
 * leaves ordinary offscreen geometry to the clipper where it belongs. */
static int coordinate_usable(float value)
{
    const uint32_t bits = float_bits(value);
    if ((bits & 0x7f800000u) == 0x7f800000u) /* NaN or infinity */
        return 0;
    return value >= -65536.0f && value <= 65536.0f;
}

/* Copy one job's geometry into the layouts the hardware reads: vec4 positions at
 * the sixteen-byte stride the PLBU's indexed-dest command implies, vec2 texel
 * coordinates at the eight-byte stride the render state declares, and byte
 * indices. Positions are rebased on the frame origin, because the frame this
 * driver submits is the caller's rectangle rather than the whole surface.
 *
 * Returns the index count, or zero for geometry that cannot be drawn -- an
 * index naming a vertex the job did not supply, or a coordinate the PLBU should
 * not be handed. */
static uint32_t build_triangle_batch(const struct gpu_triangle_job* job, float origin_x, float origin_y)
{
    const uint32_t vertices = job->vertex_count;

    for (uint32_t v = 0; v < vertices; ++v)
    {
        const float x = job->vertices[v].x - origin_x;
        const float y = job->vertices[v].y - origin_y;
        const float u = job->vertices[v].u;
        const float t = job->vertices[v].v;
        if (!coordinate_usable(x) || !coordinate_usable(y) || !coordinate_usable(u) || !coordinate_usable(t))
            return 0u;
        g_batch_positions[v * 4u + 0u] = x;
        g_batch_positions[v * 4u + 1u] = y;
        g_batch_positions[v * 4u + 2u] = 0.0f;
        g_batch_positions[v * 4u + 3u] = 1.0f;
        g_batch_varyings[v * 2u + 0u]  = u;
        g_batch_varyings[v * 2u + 1u]  = t;
    }
    /* The varying array is fetched in sixteen-byte units, so an odd vertex count
     * has to leave a defined pair behind the last one rather than whatever the
     * previous batch put there. */
    if (vertices & 1u)
    {
        g_batch_varyings[vertices * 2u + 0u] = 0.0f;
        g_batch_varyings[vertices * 2u + 1u] = 0.0f;
    }

    for (uint32_t i = 0; i < job->index_count; ++i)
    {
        if (job->indices[i] >= vertices)
            return 0u;
        g_batch_indices[i] = (uint8_t)job->indices[i];
    }
    return job->index_count;
}

/* Draw an indexed triangle list into a rectangle of a destination surface.
 *
 * The frame submitted to the PP is exactly the caller's rectangle. That is not
 * an optimisation: the writeback unit writes every pixel of the frame whether a
 * primitive covered it or not, so a frame any larger would destroy pixels the
 * caller never mentioned. The rectangle's first pixel is therefore the
 * writeback base address, and the vertices are rebased onto it to match.
 *
 * Pixels inside the rectangle that no triangle covers are the interesting case.
 * The PP initialises each tile from the frame render state, which clears, so
 * they are lost unless something puts them back. When the caller needs them
 * kept, this prepends a rectangle primitive that samples the destination itself
 * -- the same textured draw the blit path has always done, with an identity
 * mapping -- and the triangles then land on top of it. */
static int shader_draw_triangles(const struct gpu_triangle_job* job)
{
    uint32_t bpp;
    uint32_t preserve;
    uint32_t frame_w;
    uint32_t frame_h;
    uint32_t tile_count;
    uint32_t heap_bytes;
    uint32_t frame_row_bytes;
    uint32_t index_count;
    uint32_t command_words;
    uint32_t cursor;
    uint8_t* frame_base;
    uintptr_t tex_begin;
    uintptr_t tex_end;
    uintptr_t frame_begin;
    uintptr_t frame_end;
    int rc;

    if (!g_hardware_ready || g_pipeline_disabled || !job)
        return -1;
    if (!job->vertices || !job->indices || !job->texture_rgba8 || !job->dst)
        return -1;
    if (job->dst_format != 0u && job->dst_format != 3u)
        return -1;
    if (job->vertex_count < 3u || job->vertex_count > MALI_BATCH_MAX_VERTICES)
        return -1;
    if (job->index_count < 3u || job->index_count > MALI_BATCH_MAX_INDICES || (job->index_count % 3u) != 0u)
        return -1;
    if (job->scissor_max_x <= job->scissor_min_x || job->scissor_max_y <= job->scissor_min_y ||
        job->scissor_max_x > job->dst_width || job->scissor_max_y > job->dst_height)
    {
        return -1;
    }

    bpp             = (job->dst_format == 0u) ? 2u : 4u;
    preserve        = (job->flags & GPU_TRIANGLE_FLAG_OVERWRITE_TARGET) ? 0u : 1u;
    frame_w         = job->scissor_max_x - job->scissor_min_x;
    frame_h         = job->scissor_max_y - job->scissor_min_y;
    frame_row_bytes = frame_w * bpp;
    tile_count      = ((frame_w + 15u) >> 4) * ((frame_h + 15u) >> 4);
    heap_bytes      = tile_count * MALI_PLB_TILE_BYTES;

    if (frame_w > MALI_MAX_WIDTH || frame_h > MALI_MAX_HEIGHT || tile_count > MALI_MAX_TILE_COUNT)
        return -1;
    /* Texture constraints are the blit path's, for the same reasons: the
     * descriptor's extents are thirteen bits and its stride fifteen, its base
     * address drops the low six bits, and a stride wider than its own rows is
     * only fetched correctly when each row starts on a cache line. */
    if (job->texture_width == 0u || job->texture_height == 0u || job->texture_width > MALI_TEXTURE_MAX_EXTENT ||
        job->texture_height > MALI_TEXTURE_MAX_EXTENT || job->texture_pitch_bytes < job->texture_width * 4u ||
        job->texture_pitch_bytes > MALI_TEXTURE_MAX_PITCH ||
        (job->texture_pitch_bytes != job->texture_width * 4u && (job->texture_pitch_bytes & 63u) != 0u) ||
        ((uintptr_t)job->texture_rgba8 & 63u) != 0u)
    {
        return -1;
    }
    /* Eight bytes is the granularity the writeback unit's pitch field works in,
     * and it applies to the frame's first pixel rather than the surface's, so a
     * rectangle starting at an odd column is refused rather than nudged. */
    if (job->dst_pitch_bytes < job->dst_width * bpp || (job->dst_pitch_bytes & 7u) != 0u ||
        ((uintptr_t)job->dst & 7u) != 0u || ((job->scissor_min_x * bpp) & 7u) != 0u)
    {
        return -1;
    }

    frame_base = (uint8_t*)job->dst + (size_t)job->scissor_min_y * job->dst_pitch_bytes +
                 (size_t)job->scissor_min_x * bpp;
    frame_begin = (uintptr_t)frame_base;
    frame_end   = frame_begin + (size_t)job->dst_pitch_bytes * (frame_h - 1u) + frame_row_bytes;
    tex_begin   = (uintptr_t)job->texture_rgba8;
    tex_end     = tex_begin + (size_t)job->texture_pitch_bytes * job->texture_height;
    if (tex_begin < frame_end && frame_begin < tex_end)
        return -1;

    if (preserve)
    {
        /* Reloading makes the destination a texture as well as a target, so it
         * has to satisfy the descriptor's rules too -- and RGBA8 is the only
         * texel format this driver's descriptor writes. The sixty-four byte
         * requirements are not the descriptor's alone: one tile row is exactly
         * one cache line of RGBA8 pixels, and only a frame whose rows start on a
         * line keeps one tile's writeback clear of the next tile's texture
         * read. */
        if (job->dst_format != 3u || (frame_begin & 63u) != 0u || (job->dst_pitch_bytes & 63u) != 0u ||
            job->dst_pitch_bytes > MALI_TEXTURE_MAX_PITCH || frame_w > MALI_TEXTURE_MAX_EXTENT ||
            frame_h > MALI_TEXTURE_MAX_EXTENT)
        {
            return -1;
        }
    }

    if (gpu_lock_take() != 0)
        return -1;

    index_count = build_triangle_batch(job, (float)job->scissor_min_x, (float)job->scissor_min_y);
    if (index_count == 0u)
    {
        gpu_lock_drop();
        return -1;
    }

    mem_zero(g_plb_heap, heap_bytes);
    build_tile_list(frame_w, frame_h);
    build_texture_descriptor(job->texture_rgba8, job->texture_width, job->texture_height, job->texture_pitch_bytes);
    build_render_state_into(g_batch_render_state, (uint32_t)(uintptr_t)g_texture_list,
                            (uint32_t)(uintptr_t)g_batch_varyings);
    build_frame_render_state();

    cursor = emit_plbu_prologue(frame_w, frame_h, tile_count);
    if (preserve)
    {
        build_texture_descriptor_into(g_reload_texture_descriptor, g_reload_texture_list, frame_base, frame_w,
                                      frame_h, job->dst_pitch_bytes);
        build_render_state_into(g_reload_render_state, (uint32_t)(uintptr_t)g_reload_texture_list,
                                (uint32_t)(uintptr_t)g_quad_texcoords);
        build_quad(frame_w, frame_h, frame_w, frame_h);
        cursor = emit_plbu_draw(cursor, frame_w, frame_h, (uint32_t)(uintptr_t)g_quad_positions,
                                (uint32_t)(uintptr_t)g_reload_render_state, (uint32_t)(uintptr_t)g_quad_indices,
                                MALI_PRIM_RECTANGLE, 3u, MALI_SETUP_RECTANGLE, 0u);
    }
    cursor        = emit_plbu_draw(cursor, frame_w, frame_h, (uint32_t)(uintptr_t)g_batch_positions,
                                   (uint32_t)(uintptr_t)g_batch_render_state, (uint32_t)(uintptr_t)g_batch_indices,
                                   MALI_PRIM_TRIANGLES, index_count, g_batch_setup_value, g_batch_setup_first);
    command_words = emit_plbu_end(cursor);
    if (command_words == UINT32_MAX)
    {
        gpu_lock_drop();
        return -1;
    }

    clean_dcache_range(job->texture_rgba8, (size_t)job->texture_pitch_bytes * (job->texture_height - 1u) +
                                               (size_t)job->texture_width * 4u);
    /* With a reload the frame is a texture too and the CPU's copy of it has to
     * reach memory; without one every pixel is overwritten and none is read. */
    if (preserve)
        clean_invalidate_dcache_rect(frame_base, job->dst_pitch_bytes, frame_row_bytes, frame_h);
    else
        discard_dcache_rect(frame_base, job->dst_pitch_bytes, frame_row_bytes, frame_h);
    clean_dcache_range(g_fragment_shader, sizeof(g_fragment_shader));
    clean_dcache_range(g_clear_shader, sizeof(g_clear_shader));
    clean_dcache_range(g_texture_descriptor, sizeof(g_texture_descriptor));
    clean_dcache_range(g_texture_list, sizeof(g_texture_list));
    clean_dcache_range(g_batch_render_state, sizeof(g_batch_render_state));
    clean_dcache_range(g_frame_render_state, sizeof(g_frame_render_state));
    clean_dcache_range(g_batch_positions, (size_t)job->vertex_count * 4u * sizeof(float));
    clean_dcache_range(g_batch_varyings, (size_t)((job->vertex_count + 1u) & ~1u) * 2u * sizeof(float));
    clean_dcache_range(g_batch_indices, index_count);
    if (preserve)
    {
        clean_dcache_range(g_reload_texture_descriptor, sizeof(g_reload_texture_descriptor));
        clean_dcache_range(g_reload_texture_list, sizeof(g_reload_texture_list));
        clean_dcache_range(g_reload_render_state, sizeof(g_reload_render_state));
        clean_dcache_range(g_quad_indices, sizeof(g_quad_indices));
        clean_dcache_range(g_quad_positions, sizeof(g_quad_positions));
        clean_dcache_range(g_quad_texcoords, sizeof(g_quad_texcoords));
    }
    clean_dcache_range(g_plbu_array, (size_t)tile_count * sizeof(g_plbu_array[0]));
    clean_dcache_range(g_tile_list, (size_t)(tile_count * 4u + 4u) * sizeof(uint32_t));
    clean_dcache_range(g_plbu_commands, (size_t)command_words * sizeof(uint32_t));
    clean_invalidate_dcache_range(g_plb_heap, heap_bytes);
    clean_invalidate_dcache_range(g_gp_tile_heap, sizeof(g_gp_tile_heap));
    l2_prepare(MALI_L2_GP_BASE);
    l2_prepare(MALI_L2_PP_BASE);

    rc = run_gp(command_words);
    if (rc == 0)
    {
        g_last_heap_used = read32(MALI_GP_BASE + MALI_GP_HEAP_START) - (uint32_t)(uintptr_t)g_gp_tile_heap;
        invalidate_dcache_range(g_plb_heap, 64u);
        g_last_plb_word0 = ((const volatile uint32_t*)g_plb_heap)[0];
        g_last_plb_word1 = ((const volatile uint32_t*)g_plb_heap)[1];
        l2_prepare(MALI_L2_PP_BASE);
        rc = run_pp(frame_base, frame_w, frame_h, job->dst_pitch_bytes, job->dst_format);
    }
    if (rc == 0)
    {
        invalidate_dcache_rect(frame_base, job->dst_pitch_bytes, frame_row_bytes, frame_h);
        ++g_dispatch_count;
    }
    else
    {
        ++g_dispatch_failures;
        if (g_pipeline_ready)
        {
            g_pipeline_ready    = 0;
            g_pipeline_disabled = 1;
            g_status            = "Mali-450 shader dispatch failed; hardware path disabled for this boot";
            trace_job_failure("runtime shader dispatch failed");
        }
    }
    gpu_lock_drop();
    return rc;
}

/* `want` is passed rather than read from the source at `index`: under a scaling
 * blit the expected texel does not live at the destination's index. */
static void trace_selftest_mismatch(uint32_t width, uint32_t height, uint32_t index, uint32_t want)
{
    mt6592_uart_puts("  gpu: ");
    mt6592_bootstatus_log_text("gpu: ");
    trace_text("shader self-test readback mismatch");
    trace_field(" size=", (width << 16) | height);
    trace_field(" i=", index);
    trace_field(" want=", want);
    trace_field(" got=", g_selftest_dst[index]);
    trace_field(" dst=", g_selftest_dst[0]);
    trace_field(",", g_selftest_dst[1]);
    trace_field(",", g_selftest_dst[2]);
    trace_text("\n");
}

static int run_selftest(uint32_t width, uint32_t height)
{
    const uint32_t pixels = width * height;
    const uint32_t pitch  = width * 4u;

    for (uint32_t i = 0; i < pixels; ++i)
    {
        g_selftest_src[i] =
            0xff000000u | (((i * 17u) & 0xffu) << 16) | (((255u - i * 11u) & 0xffu) << 8) | ((i * 7u) & 0xffu);
        g_selftest_dst[i] = 0u;
    }
    clean_dcache_range(g_selftest_src, pixels * sizeof(uint32_t));
    clean_invalidate_dcache_range(g_selftest_dst, pixels * sizeof(uint32_t));
    if (shader_blit((const uint8_t*)g_selftest_src, (uint8_t*)g_selftest_dst, width, height, pitch, pitch, 3u) != 0)
    {
        trace_job_failure("shader self-test job failed");
        return -1;
    }
    invalidate_dcache_range(g_selftest_dst, pixels * sizeof(uint32_t));
    for (uint32_t i = 0; i < pixels; ++i)
    {
        if (g_selftest_dst[i] != g_selftest_src[i])
        {
            trace_selftest_mismatch(width, height, i, g_selftest_src[i]);
            return -1;
        }
    }
    return 0;
}

static int validate_pipeline(void)
{
    /* One exactly tile-sized blit, then one that only covers a corner of its
     * tile, so a bounding-box or writeback-clipping defect stays telling apart
     * from a sampling or writeback-format defect. */
    if (run_selftest(16u, 16u) != 0)
        return -1;
    return run_selftest(4u, 4u);
}

static uint16_t cpu_rgba8_to_rgb565(uint32_t rgba)
{
    const uint32_t r = rgba & 0xffu;
    const uint32_t g = (rgba >> 8) & 0xffu;
    const uint32_t b = (rgba >> 16) & 0xffu;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Bit-exactness is the wrong bar for a writeback unit. The PP converts 8-bit
 * colour to 5/6/5 in hardware and may round to nearest or dither where the CPU
 * reference simply truncates, so an exact comparison rejects a writeback that
 * is not merely correct but slightly better than the reference. That is how
 * this board ended up reporting "RGB565 unavailable" and pushing the whole
 * compositor blit back onto the CPU. Allow one step of slack per channel --
 * enough for rounding and ordered dither, far too little to let a swapped,
 * shifted or 5551-packed result through. */
static int rgb565_close_enough(uint16_t got, uint16_t want)
{
    const int dr = (int)((got >> 11) & 0x1fu) - (int)((want >> 11) & 0x1fu);
    const int dg = (int)((got >> 5) & 0x3fu) - (int)((want >> 5) & 0x3fu);
    const int db = (int)(got & 0x1fu) - (int)(want & 0x1fu);
    return (dr >= -1 && dr <= 1) && (dg >= -1 && dg <= 1) && (db >= -1 && db <= 1);
}

/* The compositor's 565 pixels are R5G6B5. Utgard calls its packed 16-bit
 * writeback format B5G6R5 and has a red/blue swap bit, so run the same blit
 * both ways once at boot and keep whichever setting reproduces the CPU
 * conversion. A colour-swapped desktop is not something to discover by eye.
 *
 * The two candidates are the writeback MRT word's two meaningful states, not
 * zero and one: the swap lives in bit two, and bit zero asks for a second
 * render target instead. Writing one is why this board first reported red and
 * blue transposed for both settings and pushed the whole compositor blit back
 * onto the CPU. */
static int run_selftest_rgb565(uint32_t width, uint32_t height)
{
    static const uint32_t candidates[] = {0x00000000u, 0x00000004u};
    const uint32_t pixels = width * height;
    const uint32_t src_pitch = width * 4u;
    const uint32_t dst_pitch = width * 2u;

    for (uint32_t attempt = 0; attempt < sizeof(candidates) / sizeof(candidates[0]); ++attempt)
    {
        const uint32_t swap = candidates[attempt];
        uint32_t mismatch = 0u;
        uint32_t bad_index = 0u;

        g_wb_rb_swap = swap;
        for (uint32_t i = 0; i < pixels; ++i)
        {
            /* Distinct per-channel ramps: a red/blue swap has to change the
             * result for every pixel rather than only for grey ones. */
            g_selftest_src[i] = 0xff000000u | (((i * 3u + 32u) & 0xffu) << 16) | (((i * 5u + 8u) & 0xffu) << 8) |
                                ((i * 9u + 128u) & 0xffu);
            g_selftest_dst565[i] = 0u;
        }
        clean_dcache_range(g_selftest_src, pixels * sizeof(uint32_t));
        clean_invalidate_dcache_range(g_selftest_dst565, pixels * sizeof(uint16_t));
        if (shader_blit((const uint8_t*)g_selftest_src, (uint8_t*)g_selftest_dst565, width, height, src_pitch,
                        dst_pitch, 0u) != 0)
        {
            trace_job_failure("RGB565 writeback self-test job failed");
            break;
        }
        invalidate_dcache_range(g_selftest_dst565, pixels * sizeof(uint16_t));
        for (uint32_t i = 0; i < pixels; ++i)
        {
            if (!rgb565_close_enough(g_selftest_dst565[i], cpu_rgba8_to_rgb565(g_selftest_src[i])))
            {
                mismatch = 1u;
                bad_index = i;
                break;
            }
        }
        if (!mismatch)
            return 0;
        /* Print what the hardware actually produced. Without this the failure is
         * a dead end -- "unavailable" says nothing about whether the encoding is
         * swapped, shifted, a different packing, or simply not written at all,
         * and each of those wants a different fix. */
        mt6592_uart_puts("  gpu: ");
        mt6592_bootstatus_log_text("gpu: ");
        trace_text("RGB565 writeback mismatch");
        trace_field(" swap=", swap);
        trace_field(" index=", bad_index);
        trace_field(" src=", g_selftest_src[bad_index]);
        trace_field(" got=", g_selftest_dst565[bad_index]);
        trace_field(" want=", cpu_rgba8_to_rgb565(g_selftest_src[bad_index]));
        trace_text("\n");
    }

    g_wb_rb_swap = 0u;
    return -1;
}

/* Nearest sampling at fragment centres: destination pixel (x, y) must read
 * source texel (floor((x+0.5)*src_w/dst_w), floor((y+0.5)*src_h/dst_h)). A
 * scale that silently did not apply reads (x, y) instead, and an off-by-one in
 * the varying setup shifts the whole image; both show up here.
 *
 * The ratios below are deliberately not 2:1. At exactly two the sample point
 * lands on a texel boundary, where either neighbour is a defensible answer and
 * a passing or failing test would say more about float rounding than about the
 * pipeline. */
static int run_selftest_scale_ratio(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                                    uint32_t src_pitch)
{
    const uint32_t src_row = src_pitch / 4u;

    mem_zero(g_selftest_src, sizeof(g_selftest_src));
    for (uint32_t y = 0; y < src_h; ++y)
    {
        for (uint32_t x = 0; x < src_w; ++x)
        {
            g_selftest_src[y * src_row + x] = 0xff000000u | (y << 16) | (x << 8) | ((x ^ y) & 0xffu);
        }
    }
    for (uint32_t i = 0; i < dst_w * dst_h; ++i)
        g_selftest_dst[i] = 0u;

    clean_dcache_range(g_selftest_src, (size_t)src_pitch * src_h);
    clean_invalidate_dcache_range(g_selftest_dst, (size_t)dst_w * dst_h * sizeof(uint32_t));
    if (shader_blit_scaled((const uint8_t*)g_selftest_src, (uint8_t*)g_selftest_dst, src_w, src_h, dst_w, dst_h,
                           src_pitch, dst_w * 4u, 3u) != 0)
    {
        return -1;
    }
    invalidate_dcache_range(g_selftest_dst, (size_t)dst_w * dst_h * sizeof(uint32_t));
    for (uint32_t y = 0; y < dst_h; ++y)
    {
        const uint32_t sy = ((y * 2u + 1u) * src_h) / (dst_h * 2u);
        for (uint32_t x = 0; x < dst_w; ++x)
        {
            const uint32_t sx   = ((x * 2u + 1u) * src_w) / (dst_w * 2u);
            const uint32_t want = g_selftest_src[sy * src_row + sx];
            if (g_selftest_dst[y * dst_w + x] != want)
            {
                trace_selftest_mismatch(dst_w, dst_h, y * dst_w + x, want);
                return -1;
            }
        }
    }
    return 0;
}

static int run_selftest_scaled(void)
{
    /* 3:1 down, on a source whose rows are wider than its pixels, which is the
     * shape the compositor hands over. Then 1:2 up, because a window larger
     * than the app's own surface takes the opposite path. */
    if (run_selftest_scale_ratio(48u, 48u, 16u, 16u, 256u) != 0)
        return -1;
    return run_selftest_scale_ratio(16u, 16u, 32u, 32u, 128u);
}

/* Draw a `covered` x `covered` square out of a `width` x `height` destination as
 * two triangles sampling the texture one to one, and check what came back. With
 * `preserve` set the pixels outside the square have to still hold the marker
 * they were filled with, which is the only way to find out whether the reload
 * rectangle really does put the destination back into the tile buffer -- the PP
 * clears each tile first, and no register says whether the reload beat it. */
static int run_selftest_triangles(uint32_t width, uint32_t height, uint32_t covered, uint32_t preserve)
{
    struct gpu_triangle_vertex vertices[4];
    uint16_t indices[6];
    struct gpu_triangle_job job;
    const uint32_t pixels = width * height;
    const uint32_t marker = 0xff5a5a5au;
    const float extent    = (float)covered;

    for (uint32_t i = 0; i < pixels; ++i)
    {
        g_selftest_src[i] = 0xff000000u | (((i * 13u + 7u) & 0xffu) << 16) | (((i * 5u + 3u) & 0xffu) << 8) |
                            ((i * 3u + 11u) & 0xffu);
        g_selftest_dst[i] = marker;
    }

    /* Two triangles round the same four corners, the second sharing the first's
     * diagonal. If the shared edge were filled twice or not at all it would show
     * up as a mismatch along it. */
    vertices[0].x = 0.0f;   vertices[0].y = 0.0f;   vertices[0].u = 0.0f;   vertices[0].v = 0.0f;
    vertices[1].x = extent; vertices[1].y = 0.0f;   vertices[1].u = extent; vertices[1].v = 0.0f;
    vertices[2].x = extent; vertices[2].y = extent; vertices[2].u = extent; vertices[2].v = extent;
    vertices[3].x = 0.0f;   vertices[3].y = extent; vertices[3].u = 0.0f;   vertices[3].v = extent;
    indices[0] = 0u; indices[1] = 1u; indices[2] = 2u;
    indices[3] = 0u; indices[4] = 2u; indices[5] = 3u;

    mem_zero(&job, sizeof(job));
    job.vertices            = vertices;
    job.vertex_count        = 4u;
    job.indices             = indices;
    job.index_count         = 6u;
    job.texture_rgba8       = (const uint8_t*)g_selftest_src;
    job.texture_width       = width;
    job.texture_height      = height;
    job.texture_pitch_bytes = width * 4u;
    job.dst                 = g_selftest_dst;
    job.dst_width           = width;
    job.dst_height          = height;
    job.dst_pitch_bytes     = width * 4u;
    job.dst_format          = 3u;
    job.scissor_max_x       = width;
    job.scissor_max_y       = height;
    job.flags               = preserve ? 0u : GPU_TRIANGLE_FLAG_OVERWRITE_TARGET;

    clean_dcache_range(g_selftest_src, pixels * sizeof(uint32_t));
    clean_invalidate_dcache_range(g_selftest_dst, pixels * sizeof(uint32_t));
    if (shader_draw_triangles(&job) != 0)
    {
        trace_job_failure(preserve ? "triangle reload self-test job failed" : "triangle-list self-test job failed");
        return -1;
    }
    invalidate_dcache_range(g_selftest_dst, pixels * sizeof(uint32_t));

    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t i = y * width + x;
            if (x < covered && y < covered)
            {
                if (g_selftest_dst[i] != g_selftest_src[i])
                {
                    trace_selftest_mismatch(width, height, i, g_selftest_src[i]);
                    return -1;
                }
            }
            /* Outside the square only a preserving job promises anything; an
             * overwriting one is allowed to have left the clear behind. */
            else if (preserve && g_selftest_dst[i] != marker)
            {
                trace_selftest_mismatch(width, height, i, marker);
                return -1;
            }
        }
    }
    return 0;
}

/* What the hardware was told, and what it had to read to obey. A batch that
 * comes back drawing the right geometry out of the wrong texture coordinates
 * has either been handed a render state naming the blit's varying array or has
 * ignored the render state bind altogether, and these four words are what tells
 * those two apart. The render state is re-read from memory rather than from the
 * CPU's copy of it, because the question is what the PP could see. */
static void trace_triangle_encoding(void)
{
    const volatile uint32_t* rsw = (const volatile uint32_t*)g_batch_render_state;
    const volatile uint32_t* bv  = (const volatile uint32_t*)g_batch_varyings;
    const volatile uint32_t* qv  = (const volatile uint32_t*)g_quad_texcoords;

    invalidate_dcache_range(g_batch_render_state, sizeof(g_batch_render_state));
    invalidate_dcache_range(g_batch_varyings, 4u * sizeof(float));
    invalidate_dcache_range(g_quad_texcoords, 4u * sizeof(float));
    mt6592_uart_puts("  gpu: ");
    mt6592_bootstatus_log_text("gpu: ");
    trace_text("triangle encoding rejected");
    trace_field(" setup=", g_batch_setup_value);
    trace_field(" first=", g_batch_setup_first);
    trace_field(" rsw=", (uint32_t)(uintptr_t)g_batch_render_state);
    trace_field(" rsw15=", rsw[15]);
    trace_field(" varyings=", (uint32_t)(uintptr_t)g_batch_varyings);
    trace_field(" quaduv=", (uint32_t)(uintptr_t)g_quad_texcoords);
    trace_text("\n  gpu: ");
    mt6592_bootstatus_log_text("gpu: ");
    /* Whether the array the render state names actually holds what was staged.
     * Floats as raw bits: 0.0f is 0x00000000, 16.0f is 0x41800000, 32.0f is
     * 0x42000000 -- enough to read off which of the two arrays the PP sampled
     * without needing a formatter. rsw10/rsw13 come along because a right
     * address read at the wrong stride looks exactly like a wrong address. */
    trace_text("triangle encoding state");
    trace_field(" rsw10=", rsw[10]);
    trace_field(" rsw13=", rsw[13]);
    trace_field(" bv=", bv[0]);
    trace_field(",", bv[1]);
    trace_field(",", bv[2]);
    trace_field(",", bv[3]);
    trace_field(" qv=", qv[0]);
    trace_field(",", qv[1]);
    trace_field(",", qv[2]);
    trace_field(",", qv[3]);
    trace_text("\n");
}

/* The rectangle the blit path draws answers for the whole pipeline except one
 * thing: how an ordinary indexed triangle list is encoded. The candidates
 * differ in the primitive setup word and in whether it is emitted before or
 * after the render state bind, and getting it wrong does not fail the job --
 * this board drew the right geometry while sampling the previous rectangle's
 * texture coordinates, which no status register reports. So try them, keep the
 * first that reproduces the reference image, and say what happened to the rest.
 * The reload rectangle's own encoding is included last: if it turns out this
 * silicon wants the same shape for both, that is worth finding out rather than
 * ruling out. */
static int run_selftest_triangle_encodings(void)
{
    static const uint32_t setup[] = {MALI_SETUP_TRIANGLES, MALI_SETUP_TRIANGLES, MALI_SETUP_RECTANGLE,
                                     MALI_SETUP_RECTANGLE};
    static const uint32_t first[] = {1u, 0u, 1u, 0u};

    for (uint32_t attempt = 0; attempt < sizeof(setup) / sizeof(setup[0]); ++attempt)
    {
        g_batch_setup_value = setup[attempt];
        g_batch_setup_first = first[attempt];
        if (run_selftest_triangles(32u, 32u, 32u, 0u) == 0)
            return 0;
        trace_triangle_encoding();
        if (g_pipeline_disabled)
            break;
    }
    return -1;
}

/* The .gpubss region mvii_lk_linker.ld carves out of DRAM for the job buffers.
 * _start zeroes .bss and nothing else, and objcopy -O binary does not carry a
 * NOLOAD section into the slot image, so on entry this region still holds
 * whatever the previous boot left in DRAM. */
extern char __gpu_bss_start[];
extern char __gpu_bss_end[];

static void gpu_dram_zero(void)
{
    const uintptr_t start = (uintptr_t)__gpu_bss_start;
    const uintptr_t end   = (uintptr_t)__gpu_bss_end;

    if (end > start)
        mem_zero((void*)start, (uint32_t)(end - start));
}

/* Grant cp10/cp11 and set FPEXC.EN. This file is the only one in LK built with
 * a floating-point unit, and the preloader hands the core over with the VFP
 * disabled, so without this the first vertex coordinate touched here is an
 * undefined-instruction abort rather than a float. Idempotent, and harmless if
 * something upstream ever starts doing it first. */
static void vfp_enable(void)
{
    uint32_t cpacr;

    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
    cpacr |= (0xfu << 20); /* full access, both coprocessors, PL0 and PL1 */
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 2" ::"r"(cpacr));
    __asm__ volatile("isb sy" ::: "memory");
    __asm__ volatile("vmsr fpexc, %0" ::"r"(0x40000000u));
    __asm__ volatile("isb sy" ::: "memory");
}

static void probe_once(void)
{
    if (g_probe_done)
        return;
    g_probe_done = 1;
    /* Both before the first trace(): the self-tests below run float code and
     * read buffers this is the only thing that ever initialises. */
    vfp_enable();
    gpu_dram_zero();
    trace("Mali-450 MP4 shader probe begin");
    if (mali_probe_hardware() != 0)
    {
        trace(g_status);
        return;
    }
    g_hardware_ready = 1;
    if (validate_pipeline() != 0)
    {
        g_pipeline_disabled = 1;
        g_status            = "Mali-450 hardware answered, but the bounded texture-shader self-test failed";
        trace(g_status);
        return;
    }
    /* Qualify the two compositor extras before the pipeline is marked ready, on
     * purpose: a job that fails at the hardware level disables the pipeline for
     * the rest of the boot, and only once g_pipeline_ready is set. Running them
     * first means an unsupported writeback format or an unhappy rescale costs
     * its own capability and nothing else. */
    g_rgb565_ready   = (run_selftest_rgb565(16u, 16u) == 0);
    g_scaled_ready   = (run_selftest_scaled() == 0);
    g_triangle_ready = (run_selftest_triangle_encodings() == 0);
    /* Preservation is a strictly stronger claim, so it is only worth asking
     * about once the plain batch is known to draw. */
    if (g_triangle_ready)
        g_triangle_preserve = (run_selftest_triangles(32u, 32u, 16u, 1u) == 0);
    g_pipeline_ready = 1;

    if (g_rgb565_ready && g_scaled_ready)
    {
        g_status = "Mali-450 MP4 texture shader pipeline validated; scaled RGB565 writeback active";
    }
    else if (g_rgb565_ready)
    {
        g_status = "Mali-450 MP4 texture shader pipeline validated; RGB565 writeback active, scaling unavailable";
    }
    else
    {
        g_status = "Mali-450 MP4 texture shader pipeline validated; GP/PP writeback active, RGB565 unavailable";
    }
    trace(g_status);
    if (g_rgb565_ready && g_wb_rb_swap)
    {
        trace("Mali-450 RGB565 writeback needs the red/blue swap; enabled");
    }
    /* Reported separately from g_status, which the shell only has ninety-six
     * characters for. Which of the two triangle capabilities came up decides
     * whether a software rasteriser can offload at all and whether it has to be
     * redrawing its whole rectangle to do so, so it is worth naming. */
    if (g_triangle_ready)
    {
        mt6592_uart_puts("  gpu: ");
        mt6592_bootstatus_log_text("gpu: ");
        trace_text(g_triangle_preserve ? "Mali-450 triangle lists validated, uncovered pixels preserved"
                                       : "Mali-450 triangle lists validated, whole-rectangle redraws only");
        trace_field(" setup=", g_batch_setup_value);
        trace_field(" first=", g_batch_setup_first);
        trace_text("\n");
    }
    else
    {
        trace("Mali-450 triangle lists unavailable; rasterisers stay in software");
    }
}

int mvii_gpu_offload_get_info(struct gpu_device_info* out)
{
    if (!out)
        return -1;
    probe_once();
    mem_zero(out, sizeof(*out));
    out->abi_version = GPU_ACCELERATOR_ABI_VERSION;
    out->present     = 1;
    out->vendor      = 0x14c3u;
    out->device      = 0x6592u;
    out->caps        = GPU_CAP_PRESENT | GPU_CAP_MEDIATEK | GPU_CAP_ZERO_COPY_RGBA8 | GPU_CAP_COMPUTE_ABI |
                       GPU_CAP_CPU_VECTOR_FALLBACK;
    if (g_pipeline_ready)
    {
        out->caps |= GPU_CAP_SUBMIT_NOOP | GPU_CAP_COMPUTE_RGBA8_COPY | GPU_CAP_SHADER_PIPELINE;
        out->queue_ready = 1;
    }
    /* Advertised off the self-tests, not off "the pipeline came up": the 565
     * writeback and the rescale each have their own way of being wrong. */
    if (g_rgb565_ready)
        out->caps |= GPU_CAP_COMPUTE_RGBA8_TO_RGB565;
    if (g_scaled_ready)
        out->caps |= GPU_CAP_SCALED_BLIT;
    if (g_triangle_ready)
        out->caps |= GPU_CAP_TRIANGLE_LIST;
    if (g_triangle_preserve)
        out->caps |= GPU_CAP_TRIANGLE_PRESERVE;
    copy_string(out->driver_name, sizeof(out->driver_name),
                g_pipeline_ready ? "mt6592-mali450-shader" : "mt6592-mali450-probe");
    copy_string(out->status, sizeof(out->status), g_status);
    return 0;
}

int mvii_gpu_offload_submit_noop(void)
{
    probe_once();
    return g_pipeline_ready ? 0 : -1;
}

int mvii_gpu_offload_rgba8_to_rgb565_2d(const uint8_t* src_rgba8,
                                        uint16_t* dst_rgb565,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t src_pitch_bytes,
                                        uint32_t dst_pitch_bytes)
{
    probe_once();
    if (!g_pipeline_ready || !g_rgb565_ready)
        return -1;
    return shader_blit(src_rgba8, (uint8_t*)dst_rgb565, width, height, src_pitch_bytes, dst_pitch_bytes, 0u);
}

int mvii_gpu_offload_rgba8_to_rgb565_scaled(const uint8_t* src_rgba8,
                                            uint32_t src_width,
                                            uint32_t src_height,
                                            uint32_t src_pitch_bytes,
                                            uint16_t* dst_rgb565,
                                            uint32_t dst_width,
                                            uint32_t dst_height,
                                            uint32_t dst_pitch_bytes)
{
    probe_once();
    if (!g_pipeline_ready || !g_rgb565_ready || !g_scaled_ready)
        return -1;
    return shader_blit_scaled(src_rgba8, (uint8_t*)dst_rgb565, src_width, src_height, dst_width, dst_height,
                              src_pitch_bytes, dst_pitch_bytes, 0u);
}

int mvii_gpu_offload_rgba8_blit_scaled(const uint8_t* src_rgba8,
                                       uint32_t src_width,
                                       uint32_t src_height,
                                       uint32_t src_pitch_bytes,
                                       uint8_t* dst_rgba8,
                                       uint32_t dst_width,
                                       uint32_t dst_height,
                                       uint32_t dst_pitch_bytes)
{
    probe_once();
    /* Same gate as the RGB565 rescale: the scaling self-test is what proves the
     * varying setup interpolates the way this path assumes, and it runs with an
     * RGBA8 destination, so passing it qualifies exactly this job shape. */
    if (!g_pipeline_ready || !g_scaled_ready)
        return -1;
    return shader_blit_scaled(src_rgba8, dst_rgba8, src_width, src_height, dst_width, dst_height, src_pitch_bytes,
                              dst_pitch_bytes, 3u);
}

int mvii_gpu_offload_draw_triangles(const struct gpu_triangle_job* job)
{
    probe_once();
    if (!g_pipeline_ready || !g_triangle_ready || !job)
        return -1;
    /* An RGB565 target goes through the same writeback conversion the 565 blit
     * does, so it needs the same qualification -- and the red/blue swap the
     * self-test settled on. */
    if (job->dst_format == 0u && !g_rgb565_ready)
        return -1;
    if ((job->flags & GPU_TRIANGLE_FLAG_OVERWRITE_TARGET) == 0u && !g_triangle_preserve)
        return -1;
    return shader_draw_triangles(job);
}

int mvii_gpu_offload_rgba8_copy_2d(const uint8_t* src_rgba8,
                                   uint8_t* dst_rgba8,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t src_pitch_bytes,
                                   uint32_t dst_pitch_bytes)
{
    probe_once();
    if (!g_pipeline_ready)
        return -1;
    return shader_blit(src_rgba8, dst_rgba8, width, height, src_pitch_bytes, dst_pitch_bytes, 3u);
}

int mvii_gpu_offload_rgba8_to_rgb565(const uint8_t* src_rgba8, uint16_t* dst_rgb565, uint32_t pixel_count)
{
    if (pixel_count == 0u || pixel_count > MALI_MAX_WIDTH)
        return -1;
    return mvii_gpu_offload_rgba8_to_rgb565_2d(src_rgba8, dst_rgb565, pixel_count, 1u, pixel_count * 4u,
                                               pixel_count * 2u);
}

int mvii_gpu_offload_rgba8_copy(const uint8_t* src_rgba8, uint8_t* dst_rgba8, uint32_t pixel_count)
{
    if (pixel_count == 0u || pixel_count > MALI_MAX_WIDTH)
        return -1;
    return mvii_gpu_offload_rgba8_copy_2d(src_rgba8, dst_rgba8, pixel_count, 1u, pixel_count * 4u, pixel_count * 4u);
}
