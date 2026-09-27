#ifndef MT6592_FDT_H
#define MT6592_FDT_H

/*
 * mt6592_fdt — flattened device tree, read and write, for the LK slot.
 *
 * The read half has been here since the WiFi work, which only ever needed to
 * pull a couple of constants out of a blob somebody else had built. The write
 * half exists for the SD hand-off: a Linux kernel wants its command line, its
 * initrd extents and its memory node handed to it *in the DTB*, in r2, and
 * every one of those is a fact only the bootloader knows. A card that carries a
 * DTB with a baked-in bootargs is a card that can only ever boot one way.
 *
 * This is not libfdt and does not want to be. libfdt is ~3000 lines and this
 * image has 512 KiB for the panel, the console, the USB gadget and the WiFi
 * transport as well. What a bootloader actually does to a tree is: set a
 * property on a node, creating the node if the tree did not ship with one. So
 * that is the whole write API, and it is implemented the direct way — splice
 * the struct block, fix the header, append to the strings block.
 *
 * Layout the writer requires, which is what dtc emits and what every DTB this
 * board will ever be handed looks like: mem_rsvmap, then struct, then strings,
 * with strings running to the end of the blob. mt6592_fdt_open() checks it and
 * refuses anything else rather than silently re-laying-out a tree.
 *
 * Everything is big-endian on the wire regardless of the CPU, per the spec.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mt6592_fdt_prop {
    const uint8_t* data;
    uint32_t length;
} mt6592_fdt_prop_t;

int mt6592_fdt_is_valid(const void* blob, uint32_t blob_size);

/*
 * Find a property by name ANYWHERE in the tree, first match wins, ignoring
 * node structure entirely. Kept exactly as it was: its callers look for names
 * that appear once in the blobs they read, and changing it would change them.
 * New code that cares which node it is reading wants the path-aware writer
 * below, or should walk the tree itself.
 */
int mt6592_fdt_find_property(const void* blob, uint32_t blob_size, const char* name, mt6592_fdt_prop_t* out_prop);

uint32_t mt6592_fdt_read_u32(const uint8_t* data);

/* ── Writing ── */

enum {
    MT6592_FDT_OK = 0,
    MT6592_FDT_ERR_PARAM = -1,    /* null argument, or a path we cannot parse   */
    MT6592_FDT_ERR_BADBLOB = -2,  /* not an FDT, or its header does not add up  */
    MT6592_FDT_ERR_LAYOUT = -3,   /* blocks are not in the order we can splice  */
    MT6592_FDT_ERR_NOSPACE = -4,  /* the edit does not fit in the buffer        */
    MT6592_FDT_ERR_NOTFOUND = -5, /* a path component does not exist            */
    MT6592_FDT_ERR_TRUNCATED = -6 /* the struct block ends mid-record           */
};

/*
 * A blob plus the room it has to grow into. `capacity` is the size of the
 * buffer, not of the tree: every edit that adds bytes checks against it, so a
 * caller that reads a 40 KiB DTB into a 256 KiB window can set properties
 * without thinking about it, and one that reads it into a tight buffer gets
 * MT6592_FDT_ERR_NOSPACE instead of a corrupted tree.
 */
typedef struct mt6592_fdt {
    uint8_t* blob;
    uint32_t capacity;
} mt6592_fdt_t;

int mt6592_fdt_open(mt6592_fdt_t* fdt, void* blob, uint32_t capacity);

/* Current totalsize, i.e. how many bytes of the buffer the tree occupies. */
uint32_t mt6592_fdt_size(const mt6592_fdt_t* fdt);

/*
 * Set a property, creating both the property and the node if they are absent.
 *
 * `path` is absolute and '/'-separated; "" and "/" both mean the root. Node
 * names match either exactly or up to a unit address, so "/memory" finds
 * "memory@80000000" — which matters because the caller knows what a node is
 * for and rarely knows where the tree's author put it.
 *
 * A replacement of the same length is written in place; anything else splices
 * and moves the tail of the blob, so every offset the caller was holding is
 * stale afterwards. Read what you need, then write.
 */
int mt6592_fdt_setprop(mt6592_fdt_t* fdt, const char* path, const char* name, const void* data, uint32_t len);

int mt6592_fdt_setprop_u32(mt6592_fdt_t* fdt, const char* path, const char* name, uint32_t value);

/* Writes the NUL as part of the value, which is what the spec's string type is. */
int mt6592_fdt_setprop_string(mt6592_fdt_t* fdt, const char* path, const char* name, const char* text);

const char* mt6592_fdt_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif
