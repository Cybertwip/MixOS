#include "mt6592_fdt.h"

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u
#define FDT_HEADER_SIZE 40u

/* Header word offsets, spec order. */
#define HDR_TOTALSIZE 4u
#define HDR_STRUCT_OFF 8u
#define HDR_STRINGS_OFF 12u
#define HDR_VERSION 20u
#define HDR_STRINGS_SIZE 32u
#define HDR_STRUCT_SIZE 36u

/* size_dt_struct became a header field in v17. Below that we cannot know where
 * the struct block ends without walking it, and a writer that has to trust a
 * walk to bound its own splices is a writer that corrupts malformed input. */
#define FDT_MIN_WRITE_VERSION 17u

typedef struct fdt_view {
    const uint8_t* base;
    uint32_t total_size;
    uint32_t struct_off;
    uint32_t strings_off;
    uint32_t struct_size;
    uint32_t strings_size;
} fdt_view_t;

uint32_t mt6592_fdt_read_u32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint32_t align4(uint32_t value) {
    return (value + 3u) & ~3u;
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int range_ok(uint32_t off, uint32_t len, uint32_t total) {
    if (off > total) return 0;
    if (len > total - off) return 0;
    return 1;
}

static int parse_header(const void* blob, uint32_t blob_size, fdt_view_t* view) {
    const uint8_t* base = (const uint8_t*)blob;
    uint32_t total_size;
    uint32_t struct_off;
    uint32_t strings_off;
    uint32_t struct_size;
    uint32_t strings_size;

    if (!blob || !view) return 0;
    if (blob_size && blob_size < FDT_HEADER_SIZE) return 0;
    if (mt6592_fdt_read_u32(base) != FDT_MAGIC) return 0;

    total_size = mt6592_fdt_read_u32(base + 4u);
    struct_off = mt6592_fdt_read_u32(base + 8u);
    strings_off = mt6592_fdt_read_u32(base + 12u);
    strings_size = mt6592_fdt_read_u32(base + 32u);
    struct_size = mt6592_fdt_read_u32(base + 36u);

    if (total_size < FDT_HEADER_SIZE) return 0;
    if (blob_size && total_size > blob_size) return 0;
    if (!range_ok(struct_off, struct_size, total_size)) return 0;
    if (!range_ok(strings_off, strings_size, total_size)) return 0;

    view->base = base;
    view->total_size = total_size;
    view->struct_off = struct_off;
    view->strings_off = strings_off;
    view->struct_size = struct_size;
    view->strings_size = strings_size;
    return 1;
}

static const char* string_at(const fdt_view_t* view, uint32_t name_off) {
    const char* s;
    uint32_t i;

    if (!view || name_off >= view->strings_size) return 0;
    s = (const char*)(view->base + view->strings_off + name_off);
    for (i = name_off; i < view->strings_size; ++i) {
        if (view->base[view->strings_off + i] == 0) return s;
    }
    return 0;
}

int mt6592_fdt_is_valid(const void* blob, uint32_t blob_size) {
    fdt_view_t view;
    return parse_header(blob, blob_size, &view);
}

int mt6592_fdt_find_property(const void* blob, uint32_t blob_size, const char* name, mt6592_fdt_prop_t* out_prop) {
    fdt_view_t view;
    uint32_t pos;
    uint32_t end;

    if (!name || !out_prop) return 0;
    out_prop->data = 0;
    out_prop->length = 0;
    if (!parse_header(blob, blob_size, &view)) return 0;

    pos = view.struct_off;
    end = view.struct_off + view.struct_size;
    while (range_ok(pos, 4u, end)) {
        uint32_t token = mt6592_fdt_read_u32(view.base + pos);
        pos += 4u;

        if (token == FDT_BEGIN_NODE) {
            while (pos < end && view.base[pos] != 0) ++pos;
            if (pos >= end) return 0;
            pos = align4(pos + 1u);
        } else if (token == FDT_PROP) {
            uint32_t len;
            uint32_t name_off;
            const char* prop_name;

            if (!range_ok(pos, 8u, end)) return 0;
            len = mt6592_fdt_read_u32(view.base + pos);
            name_off = mt6592_fdt_read_u32(view.base + pos + 4u);
            pos += 8u;
            if (!range_ok(pos, len, end)) return 0;

            prop_name = string_at(&view, name_off);
            if (prop_name && str_eq(prop_name, name)) {
                out_prop->data = view.base + pos;
                out_prop->length = len;
                return 1;
            }
            pos = align4(pos + len);
        } else if (token == FDT_END_NODE || token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            return 0;
        } else {
            return 0;
        }
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Writing
 *
 * Everything below works on absolute byte offsets into the blob and splices
 * bytes in and out of it. Two rules make that safe and they are worth stating
 * once rather than re-deriving at every call site:
 *
 *   1. The strings block is last. Interning a name therefore grows the blob at
 *      totalsize and moves nothing, so a name offset obtained before a struct
 *      edit is still correct after it. Every path here interns first.
 *   2. A struct-block splice moves the strings block, so off_dt_strings has to
 *      move with it. struct_splice() is the only way to edit the struct block
 *      and it is the only place that knows this.
 *
 * mt6592_fdt_open() enforces rule 1 up front; nothing below re-checks it.
 * ══════════════════════════════════════════════════════════════════════════ */

/* No <string.h> in this image, and the aeabi helpers that are linked here are
 * an implementation detail of the compiler rather than an API. Ten lines of
 * loop cost nothing and cannot be optimised into a call to themselves. */
static void blob_move(uint8_t* dst, const uint8_t* src, uint32_t n) {
    uint32_t i;

    if (dst == src || n == 0u) return;
    if (dst < src) {
        for (i = 0u; i < n; ++i) dst[i] = src[i];
    } else {
        for (i = n; i > 0u; --i) dst[i - 1u] = src[i - 1u];
    }
}

static void blob_zero(uint8_t* dst, uint32_t n) {
    uint32_t i;

    for (i = 0u; i < n; ++i) dst[i] = 0u;
}

static uint32_t str_len(const char* s) {
    uint32_t n = 0u;

    while (s[n] != 0) ++n;
    return n;
}

static void write_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t hdr_get(const mt6592_fdt_t* f, uint32_t off) {
    return mt6592_fdt_read_u32(f->blob + off);
}

static void hdr_set(mt6592_fdt_t* f, uint32_t off, uint32_t v) {
    write_u32(f->blob + off, v);
}

static uint32_t struct_end(const mt6592_fdt_t* f) {
    return hdr_get(f, HDR_STRUCT_OFF) + hdr_get(f, HDR_STRUCT_SIZE);
}

/* Is there a whole token at `pos` inside the struct block? */
static int tok_ok(const mt6592_fdt_t* f, uint32_t pos) {
    const uint32_t end = struct_end(f);

    return pos >= hdr_get(f, HDR_STRUCT_OFF) && pos + 4u <= end && pos + 4u > pos;
}

static uint32_t tok_at(const mt6592_fdt_t* f, uint32_t pos) {
    return mt6592_fdt_read_u32(f->blob + pos);
}

int mt6592_fdt_open(mt6592_fdt_t* fdt, void* blob, uint32_t capacity) {
    fdt_view_t view;
    uint32_t version;

    if (!fdt || !blob) return MT6592_FDT_ERR_PARAM;
    fdt->blob = (uint8_t*)blob;
    fdt->capacity = capacity;

    if (!parse_header(blob, capacity, &view)) return MT6592_FDT_ERR_BADBLOB;

    version = mt6592_fdt_read_u32((const uint8_t*)blob + HDR_VERSION);
    if (version < FDT_MIN_WRITE_VERSION) return MT6592_FDT_ERR_LAYOUT;

    /* The one layout this writer can edit: struct, then strings, strings ending
     * exactly at the end of the tree. */
    if (view.struct_off < FDT_HEADER_SIZE) return MT6592_FDT_ERR_LAYOUT;
    if (view.struct_off + view.struct_size > view.strings_off) return MT6592_FDT_ERR_LAYOUT;
    if (view.strings_off + view.strings_size != view.total_size) return MT6592_FDT_ERR_LAYOUT;
    if (view.total_size > capacity) return MT6592_FDT_ERR_NOSPACE;

    return MT6592_FDT_OK;
}

uint32_t mt6592_fdt_size(const mt6592_fdt_t* fdt) {
    if (!fdt || !fdt->blob) return 0u;
    return hdr_get(fdt, HDR_TOTALSIZE);
}

/*
 * Insert `delta` zero bytes at `at`, or remove `-delta` bytes starting there.
 * Updates totalsize and nothing else — the caller owns the block sizes because
 * only the caller knows which block it just edited.
 */
static int blob_splice(mt6592_fdt_t* f, uint32_t at, int32_t delta) {
    uint32_t total = hdr_get(f, HDR_TOTALSIZE);

    if (delta == 0) return MT6592_FDT_OK;
    if (at > total) return MT6592_FDT_ERR_PARAM;

    if (delta > 0) {
        const uint32_t grow = (uint32_t)delta;

        if (grow > f->capacity - total) return MT6592_FDT_ERR_NOSPACE;
        blob_move(f->blob + at + grow, f->blob + at, total - at);
        blob_zero(f->blob + at, grow);
        total += grow;
    } else {
        const uint32_t shrink = (uint32_t)(-delta);

        if (shrink > total - at) return MT6592_FDT_ERR_PARAM;
        blob_move(f->blob + at, f->blob + at + shrink, total - at - shrink);
        total -= shrink;
    }

    hdr_set(f, HDR_TOTALSIZE, total);
    return MT6592_FDT_OK;
}

static int struct_splice(mt6592_fdt_t* f, uint32_t at, int32_t delta) {
    const int rc = blob_splice(f, at, delta);

    if (rc != MT6592_FDT_OK) return rc;
    hdr_set(f, HDR_STRUCT_SIZE, (uint32_t)((int32_t)hdr_get(f, HDR_STRUCT_SIZE) + delta));
    hdr_set(f, HDR_STRINGS_OFF, (uint32_t)((int32_t)hdr_get(f, HDR_STRINGS_OFF) + delta));
    return MT6592_FDT_OK;
}

/* The strings block is a bag of NUL-terminated names addressed by offset, with
 * no index and no ordering. Reuse an existing entry when the name is already
 * there — not to save the bytes, but because dtc's own trees have exactly one
 * entry per distinct name and a tree that is edited a dozen times should not
 * end up looking different from one that was compiled that way. */
static int strings_intern(mt6592_fdt_t* f, const char* name, uint32_t* out_off) {
    const uint32_t soff = hdr_get(f, HDR_STRINGS_OFF);
    const uint32_t ssize = hdr_get(f, HDR_STRINGS_SIZE);
    const uint32_t want = str_len(name);
    uint32_t i = 0u;
    uint32_t at;
    int rc;

    while (i < ssize) {
        const char* cand = (const char*)(f->blob + soff + i);
        uint32_t l = 0u;

        while (i + l < ssize && cand[l] != 0) ++l;
        if (i + l >= ssize) break; /* unterminated tail: stop trusting it */
        if (l == want && str_eq(cand, name)) {
            *out_off = i;
            return MT6592_FDT_OK;
        }
        i += l + 1u;
    }

    at = soff + ssize; /* == totalsize, by the layout rule open() enforced */
    rc = blob_splice(f, at, (int32_t)(want + 1u));
    if (rc != MT6592_FDT_OK) return rc;
    for (i = 0u; i < want; ++i) f->blob[at + i] = (uint8_t)name[i];
    f->blob[at + want] = 0u;
    hdr_set(f, HDR_STRINGS_SIZE, ssize + want + 1u);
    *out_off = ssize;
    return MT6592_FDT_OK;
}

static const char* strings_at(const mt6592_fdt_t* f, uint32_t name_off) {
    const uint32_t soff = hdr_get(f, HDR_STRINGS_OFF);
    const uint32_t ssize = hdr_get(f, HDR_STRINGS_SIZE);
    uint32_t i;

    if (name_off >= ssize) return 0;
    for (i = name_off; i < ssize; ++i) {
        if (f->blob[soff + i] == 0u) return (const char*)(f->blob + soff + name_off);
    }
    return 0;
}

/* Offset of the first token inside a node, given the offset of its
 * FDT_BEGIN_NODE. Returns 0 when the name runs off the end of the block. */
static uint32_t node_body(const mt6592_fdt_t* f, uint32_t node_tok) {
    const uint32_t end = struct_end(f);
    uint32_t pos = node_tok + 4u;

    while (pos < end && f->blob[pos] != 0u) ++pos;
    if (pos >= end) return 0u;
    return align4(pos + 1u);
}

/* Advance past a whole node, subtree and all. */
static int node_skip(const mt6592_fdt_t* f, uint32_t node_tok, uint32_t* out_next) {
    uint32_t pos = node_tok;
    uint32_t depth = 0u;

    while (tok_ok(f, pos)) {
        const uint32_t token = tok_at(f, pos);

        if (token == FDT_BEGIN_NODE) {
            const uint32_t body = node_body(f, pos);

            if (body == 0u) return MT6592_FDT_ERR_TRUNCATED;
            ++depth;
            pos = body;
        } else if (token == FDT_END_NODE) {
            pos += 4u;
            if (--depth == 0u) {
                *out_next = pos;
                return MT6592_FDT_OK;
            }
        } else if (token == FDT_PROP) {
            uint32_t len;

            if (!tok_ok(f, pos + 8u)) return MT6592_FDT_ERR_TRUNCATED;
            len = mt6592_fdt_read_u32(f->blob + pos + 4u);
            pos = align4(pos + 12u + len);
        } else if (token == FDT_NOP) {
            pos += 4u;
        } else {
            return MT6592_FDT_ERR_TRUNCATED;
        }
    }
    return MT6592_FDT_ERR_TRUNCATED;
}

/* "memory" matches "memory@80000000". A caller knows what a node is *for* and
 * almost never knows what unit address the tree's author gave it. */
static int name_matches(const char* node_name, const char* want, uint32_t want_len) {
    uint32_t i;

    for (i = 0u; i < want_len; ++i) {
        if (node_name[i] == 0 || node_name[i] != want[i]) return 0;
    }
    return node_name[want_len] == 0 || node_name[want_len] == '@';
}

/*
 * Walk one node's children. With `want` non-null, stops at the first match and
 * reports its FDT_BEGIN_NODE offset; either way it reports the offset of the
 * parent's own FDT_END_NODE, which is where a new child has to be inserted so
 * that it lands after the parent's properties rather than in front of them.
 */
static int node_children(const mt6592_fdt_t* f, uint32_t body, const char* want, uint32_t want_len,
                         uint32_t* out_child, uint32_t* out_end) {
    uint32_t pos = body;

    if (out_child) *out_child = 0u;

    while (tok_ok(f, pos)) {
        const uint32_t token = tok_at(f, pos);

        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)(f->blob + pos + 4u);
            uint32_t next;
            int rc;

            if (want && name_matches(name, want, want_len)) {
                if (out_child) *out_child = pos;
                return MT6592_FDT_OK;
            }
            rc = node_skip(f, pos, &next);
            if (rc != MT6592_FDT_OK) return rc;
            pos = next;
        } else if (token == FDT_PROP) {
            uint32_t len;

            if (!tok_ok(f, pos + 8u)) return MT6592_FDT_ERR_TRUNCATED;
            len = mt6592_fdt_read_u32(f->blob + pos + 4u);
            pos = align4(pos + 12u + len);
        } else if (token == FDT_NOP) {
            pos += 4u;
        } else if (token == FDT_END_NODE) {
            if (out_end) *out_end = pos;
            return want ? MT6592_FDT_ERR_NOTFOUND : MT6592_FDT_OK;
        } else {
            return MT6592_FDT_ERR_TRUNCATED;
        }
    }
    return MT6592_FDT_ERR_TRUNCATED;
}

/* Insert an empty child node just before the parent's FDT_END_NODE. */
static int node_add(mt6592_fdt_t* f, uint32_t parent_body, const char* name, uint32_t name_len,
                    uint32_t* out_body) {
    uint32_t end = 0u;
    uint32_t namebytes;
    uint32_t bytes;
    uint32_t i;
    int rc;

    rc = node_children(f, parent_body, 0, 0u, 0, &end);
    if (rc != MT6592_FDT_OK) return rc;
    if (end == 0u) return MT6592_FDT_ERR_TRUNCATED;

    namebytes = align4(name_len + 1u);
    bytes = 4u + namebytes + 4u;
    rc = struct_splice(f, end, (int32_t)bytes);
    if (rc != MT6592_FDT_OK) return rc;

    write_u32(f->blob + end, FDT_BEGIN_NODE);
    for (i = 0u; i < name_len; ++i) f->blob[end + 4u + i] = (uint8_t)name[i];
    blob_zero(f->blob + end + 4u + name_len, namebytes - name_len);
    write_u32(f->blob + end + 4u + namebytes, FDT_END_NODE);

    *out_body = end + 4u + namebytes;
    return MT6592_FDT_OK;
}

/*
 * Resolve an absolute path to the node's body offset, optionally creating the
 * nodes that are missing. Creation is not speculative generality: a stock
 * distribution DTB frequently has no /chosen at all, and a bootloader whose
 * whole job is to hand over a command line cannot fail on that.
 */
static int node_path(mt6592_fdt_t* f, const char* path, int create, uint32_t* out_body) {
    uint32_t root_tok = hdr_get(f, HDR_STRUCT_OFF);
    uint32_t body;
    const char* p = path ? path : "";

    while (tok_ok(f, root_tok) && tok_at(f, root_tok) == FDT_NOP) root_tok += 4u;
    if (!tok_ok(f, root_tok) || tok_at(f, root_tok) != FDT_BEGIN_NODE) return MT6592_FDT_ERR_BADBLOB;

    body = node_body(f, root_tok);
    if (body == 0u) return MT6592_FDT_ERR_TRUNCATED;

    for (;;) {
        uint32_t len = 0u;
        uint32_t child = 0u;
        int rc;

        while (*p == '/') ++p;
        if (*p == 0) break;
        while (p[len] != 0 && p[len] != '/') ++len;

        rc = node_children(f, body, p, len, &child, 0);
        if (rc == MT6592_FDT_ERR_NOTFOUND) {
            if (!create) return MT6592_FDT_ERR_NOTFOUND;
            rc = node_add(f, body, p, len, &body);
            if (rc != MT6592_FDT_OK) return rc;
        } else if (rc != MT6592_FDT_OK) {
            return rc;
        } else {
            body = node_body(f, child);
            if (body == 0u) return MT6592_FDT_ERR_TRUNCATED;
        }
        p += len;
    }

    *out_body = body;
    return MT6592_FDT_OK;
}

/* A node's own properties run from its body up to its first child or its end. */
static int prop_find(const mt6592_fdt_t* f, uint32_t body, const char* name, uint32_t* out_tok,
                     uint32_t* out_len) {
    uint32_t pos = body;

    while (tok_ok(f, pos)) {
        const uint32_t token = tok_at(f, pos);
        uint32_t len;
        uint32_t name_off;
        const char* prop_name;

        if (token == FDT_NOP) {
            pos += 4u;
            continue;
        }
        if (token != FDT_PROP) return MT6592_FDT_ERR_NOTFOUND;

        if (!tok_ok(f, pos + 8u)) return MT6592_FDT_ERR_TRUNCATED;
        len = mt6592_fdt_read_u32(f->blob + pos + 4u);
        name_off = mt6592_fdt_read_u32(f->blob + pos + 8u);
        if (pos + 12u + len > struct_end(f)) return MT6592_FDT_ERR_TRUNCATED;

        prop_name = strings_at(f, name_off);
        if (prop_name && str_eq(prop_name, name)) {
            *out_tok = pos;
            *out_len = len;
            return MT6592_FDT_OK;
        }
        pos = align4(pos + 12u + len);
    }
    return MT6592_FDT_ERR_TRUNCATED;
}

int mt6592_fdt_setprop(mt6592_fdt_t* fdt, const char* path, const char* name, const void* data, uint32_t len) {
    const uint8_t* src = (const uint8_t*)data;
    uint32_t name_off = 0u;
    uint32_t body = 0u;
    uint32_t tok = 0u;
    uint32_t oldlen = 0u;
    uint32_t i;
    int rc;

    if (!fdt || !fdt->blob || !name || (!data && len != 0u)) return MT6592_FDT_ERR_PARAM;

    /* Intern first, always. It grows the blob past the end of everything, so
     * the offset it returns survives every splice the rest of this does. */
    rc = strings_intern(fdt, name, &name_off);
    if (rc != MT6592_FDT_OK) return rc;

    rc = node_path(fdt, path, 1, &body);
    if (rc != MT6592_FDT_OK) return rc;

    rc = prop_find(fdt, body, name, &tok, &oldlen);
    if (rc == MT6592_FDT_OK) {
        const uint32_t oldpad = align4(oldlen);
        const uint32_t newpad = align4(len);
        uint32_t data_off = tok + 12u;

        if (newpad != oldpad) {
            const uint32_t at = data_off + (newpad > oldpad ? oldpad : newpad);

            rc = struct_splice(fdt, at, (int32_t)newpad - (int32_t)oldpad);
            if (rc != MT6592_FDT_OK) return rc;
        }
        write_u32(fdt->blob + tok + 4u, len);
        for (i = 0u; i < len; ++i) fdt->blob[data_off + i] = src[i];
        blob_zero(fdt->blob + data_off + len, newpad - len);
        return MT6592_FDT_OK;
    }
    if (rc != MT6592_FDT_ERR_NOTFOUND) return rc;

    /* New property, at the front of the node's body: properties must precede
     * subnodes, and the front is the only place that is unconditionally before
     * every subnode without having to find where they start. */
    {
        const uint32_t newpad = align4(len);
        const uint32_t bytes = 12u + newpad;

        rc = struct_splice(fdt, body, (int32_t)bytes);
        if (rc != MT6592_FDT_OK) return rc;

        write_u32(fdt->blob + body, FDT_PROP);
        write_u32(fdt->blob + body + 4u, len);
        write_u32(fdt->blob + body + 8u, name_off);
        for (i = 0u; i < len; ++i) fdt->blob[body + 12u + i] = src[i];
        blob_zero(fdt->blob + body + 12u + len, newpad - len);
    }
    return MT6592_FDT_OK;
}

int mt6592_fdt_setprop_u32(mt6592_fdt_t* fdt, const char* path, const char* name, uint32_t value) {
    uint8_t be[4];

    write_u32(be, value);
    return mt6592_fdt_setprop(fdt, path, name, be, 4u);
}

int mt6592_fdt_setprop_string(mt6592_fdt_t* fdt, const char* path, const char* name, const char* text) {
    if (!text) return MT6592_FDT_ERR_PARAM;
    return mt6592_fdt_setprop(fdt, path, name, text, str_len(text) + 1u);
}

const char* mt6592_fdt_strerror(int rc) {
    switch (rc) {
        case MT6592_FDT_OK: return "ok";
        case MT6592_FDT_ERR_PARAM: return "bad argument";
        case MT6592_FDT_ERR_BADBLOB: return "not a device tree";
        case MT6592_FDT_ERR_LAYOUT: return "device tree layout is not editable";
        case MT6592_FDT_ERR_NOSPACE: return "device tree buffer is full";
        case MT6592_FDT_ERR_NOTFOUND: return "no such node or property";
        case MT6592_FDT_ERR_TRUNCATED: return "device tree is truncated";
        default: return "unknown error";
    }
}
