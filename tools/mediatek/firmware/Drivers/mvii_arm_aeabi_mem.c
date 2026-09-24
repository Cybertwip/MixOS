/*
 * mvii_arm_aeabi_mem.c — the AEABI block-move helpers, for images that do not
 * link compiler-rt builtins.
 *
 * Companion to mvii_arm_aeabi_div.c and there for the same reason. Every driver
 * in this tree hand-rolls its own copy loop precisely so that a bare image needs
 * no libc, and for years that was enough. It stops being enough the moment a
 * whole struct is assigned: `*out = g_scan_results[i]` is not a loop the author
 * wrote, it is a 36-byte block move the compiler lowers to __aeabi_memcpy, and
 * there is nothing at the C level to point at. First hit was
 * mt6592_wifi_hif_get_scan_results() when the HIF was linked into MVIILK.elf.
 *
 * In an image that DOES link compiler-rt these definitions simply preempt the
 * archive members (an archive is only searched for still-undefined symbols), so
 * linking this file everywhere would be safe. It is currently linked only where
 * the link actually failed without it.
 *
 * The AEABI shapes are not the C ones and the differences bite:
 *   - the memcpy/memmove/memset entry points return void, not dest;
 *   - __aeabi_memset takes (dest, n, c) -- size before the fill byte, the
 *     reverse of C's memset;
 *   - the 4/8 suffixes promise the pointers are 4/8-byte aligned. We honour
 *     that as a fast path rather than a requirement, so a caller that lies
 *     still gets a correct copy.
 *
 * The loops write through volatile pointers. That is not about hardware: at -O3
 * a plain byte loop inside a function named like a block move is exactly the
 * pattern the optimiser rewrites into a call to memcpy, and the call it emits
 * would be to this function. Volatile is the portable way to say "emit the loop
 * I wrote".
 */

#include <stdint.h>
#include <stddef.h>

static void copy_forward(void* dest, const void* src, size_t n)
{
    volatile uint8_t* d = (volatile uint8_t*)dest;
    const volatile uint8_t* s = (const volatile uint8_t*)src;

    /* Word-at-a-time when both ends agree on alignment, which is the common
     * case and the only one worth special-casing at this size. */
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        volatile uint32_t* dw = (volatile uint32_t*)dest;
        const volatile uint32_t* sw = (const volatile uint32_t*)src;
        while (n >= 4u) {
            *dw++ = *sw++;
            n -= 4u;
        }
        d = (volatile uint8_t*)dw;
        s = (const volatile uint8_t*)sw;
    }
    while (n-- != 0u) *d++ = *s++;
}

static void copy_backward(void* dest, const void* src, size_t n)
{
    volatile uint8_t* d = (volatile uint8_t*)dest + n;
    const volatile uint8_t* s = (const volatile uint8_t*)src + n;
    while (n-- != 0u) *--d = *--s;
}

static void fill_bytes(void* dest, size_t n, uint8_t c)
{
    volatile uint8_t* d = (volatile uint8_t*)dest;
    while (n-- != 0u) *d++ = c;
}

void __aeabi_memcpy(void* dest, const void* src, size_t n) { copy_forward(dest, src, n); }
void __aeabi_memcpy4(void* dest, const void* src, size_t n) { copy_forward(dest, src, n); }
void __aeabi_memcpy8(void* dest, const void* src, size_t n) { copy_forward(dest, src, n); }

void __aeabi_memmove(void* dest, const void* src, size_t n)
{
    if ((uintptr_t)dest > (uintptr_t)src && (uintptr_t)dest < (uintptr_t)src + n) {
        copy_backward(dest, src, n);
    } else {
        copy_forward(dest, src, n);
    }
}
void __aeabi_memmove4(void* dest, const void* src, size_t n) { __aeabi_memmove(dest, src, n); }
void __aeabi_memmove8(void* dest, const void* src, size_t n) { __aeabi_memmove(dest, src, n); }

void __aeabi_memset(void* dest, size_t n, int c) { fill_bytes(dest, n, (uint8_t)c); }
void __aeabi_memset4(void* dest, size_t n, int c) { fill_bytes(dest, n, (uint8_t)c); }
void __aeabi_memset8(void* dest, size_t n, int c) { fill_bytes(dest, n, (uint8_t)c); }

void __aeabi_memclr(void* dest, size_t n) { fill_bytes(dest, n, 0u); }
void __aeabi_memclr4(void* dest, size_t n) { fill_bytes(dest, n, 0u); }
void __aeabi_memclr8(void* dest, size_t n) { fill_bytes(dest, n, 0u); }
