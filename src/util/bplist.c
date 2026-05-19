/* src/util/bplist.c
 *
 * Apple binary plist (bplist00) parser. See include/human/util/bplist.h
 * for contract. Clean-room implementation of the publicly documented
 * binary property-list v0 format.
 *
 * Format summary (read-only, bplist00):
 *   header   : 8 bytes "bplist00"
 *   objects  : variable-length, each starting with a marker byte:
 *                upper nibble = type tag, lower nibble = info
 *              type tags: 0=null/bool/fill, 1=int, 2=real, 3=date,
 *                         4=data, 5=ASCII string, 6=UTF-16BE string,
 *                         8=UID, A=array, C=set, D=dict
 *   trailer  : last 32 bytes
 *                bytes [0..5] = unused
 *                byte  [6]    = offset_int_size (1, 2, 4, or 8)
 *                byte  [7]    = object_ref_size (1, 2, 4, or 8)
 *                bytes [8..15]  = num_objects (big-endian u64)
 *                bytes [16..23] = top_object (big-endian u64)
 *                bytes [24..31] = offset_table_offset (big-endian u64)
 *   offset table : num_objects entries of offset_int_size bytes each
 *                  (big-endian); entry i is the file offset of object i.
 *   array body   : count-prefixed list of object_ref_size-byte indices
 *   dict body    : count-prefixed list of key refs followed by value
 *                  refs (same count, same width)
 */

#include "human/util/bplist.h"

#include <stdlib.h>
#include <string.h>

struct hu_bplist {
    unsigned char *buf; /* owned copy of input */
    size_t buf_len;     /* total length of buf */
    size_t num_objects; /* trailer-reported count */
    size_t top_object;  /* trailer-reported root index */
    size_t offset_table_off;
    unsigned char offset_size; /* bytes per offset-table entry */
    unsigned char ref_size;    /* bytes per object reference */
};

/* ── Endian helpers (big-endian unsigned reads) ───────────────────── */

static uint64_t read_be_u64(const unsigned char *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v = (v << 8) | (uint64_t)p[i];
    return v;
}

/* Read the file offset of object `i` from the offset table. Returns
 * SIZE_MAX on out-of-range or oversized offset (defense against
 * malicious blobs). */
static size_t object_offset(const hu_bplist_t *p, size_t i) {
    if (!p || i >= p->num_objects)
        return SIZE_MAX;
    size_t pos = p->offset_table_off + i * p->offset_size;
    if (pos + p->offset_size > p->buf_len)
        return SIZE_MAX;
    uint64_t off = read_be_u64(p->buf + pos, p->offset_size);
    if (off >= p->buf_len)
        return SIZE_MAX;
    return (size_t)off;
}

/* Read the marker byte for object `i`. Returns 0xFF (an invalid marker)
 * if the index is OOB. */
static unsigned char object_marker(const hu_bplist_t *p, size_t i) {
    size_t off = object_offset(p, i);
    if (off == SIZE_MAX)
        return 0xFF;
    return p->buf[off];
}

/* For variable-length objects, decode the (length, body-start) pair
 * given the marker offset. The marker's low nibble is either the
 * length directly OR 0xF, in which case the next bytes form an int
 * object whose value is the length.
 *
 * Returns true and writes *out_len + *out_body_off on success.
 * Returns false if the encoding overflows the buffer. */
static bool decode_length(const hu_bplist_t *p, size_t marker_off, size_t *out_len,
                          size_t *out_body_off) {
    if (marker_off >= p->buf_len)
        return false;
    unsigned char low = p->buf[marker_off] & 0x0F;
    if (low < 0x0F) {
        *out_len = low;
        *out_body_off = marker_off + 1;
        return true;
    }
    /* low == 0x0F: follow-up int. Next byte is an int marker (0x1n)
     * whose low nibble encodes 2^n bytes of length. */
    if (marker_off + 1 >= p->buf_len)
        return false;
    unsigned char int_marker = p->buf[marker_off + 1];
    if ((int_marker & 0xF0) != 0x10)
        return false;
    size_t nbytes = (size_t)1u << (int_marker & 0x0F);
    if (nbytes > 8 || marker_off + 2 + nbytes > p->buf_len)
        return false;
    uint64_t v = read_be_u64(p->buf + marker_off + 2, nbytes);
    if (v > (uint64_t)(p->buf_len - (marker_off + 2 + nbytes)))
        return false;
    *out_len = (size_t)v;
    *out_body_off = marker_off + 2 + nbytes;
    return true;
}

/* Read an object_ref_size-byte ref at `pos`. Returns SIZE_MAX on OOB. */
static size_t read_ref(const hu_bplist_t *p, size_t pos) {
    if (pos + p->ref_size > p->buf_len)
        return SIZE_MAX;
    uint64_t v = read_be_u64(p->buf + pos, p->ref_size);
    if (v >= p->num_objects)
        return SIZE_MAX;
    return (size_t)v;
}

/* ── Public API ───────────────────────────────────────────────────── */

hu_error_t hu_bplist_parse(const unsigned char *blob, size_t len, hu_bplist_t **out) {
    if (!blob || !out || len < 8 + 32)
        return HU_ERR_INVALID_ARGUMENT;
    if (memcmp(blob, "bplist00", 8) != 0)
        return HU_ERR_INVALID_ARGUMENT;

    const unsigned char *trailer = blob + len - 32;
    unsigned char offset_size = trailer[6];
    unsigned char ref_size = trailer[7];
    if (offset_size != 1 && offset_size != 2 && offset_size != 4 && offset_size != 8)
        return HU_ERR_INVALID_ARGUMENT;
    if (ref_size != 1 && ref_size != 2 && ref_size != 4 && ref_size != 8)
        return HU_ERR_INVALID_ARGUMENT;
    uint64_t num_objects = read_be_u64(trailer + 8, 8);
    uint64_t top_object = read_be_u64(trailer + 16, 8);
    uint64_t off_tbl = read_be_u64(trailer + 24, 8);

    /* Bound num_objects so we can multiply without overflow on 32-bit
     * size_t and so we never trust an arbitrarily huge claim. */
    if (num_objects == 0 || num_objects > (uint64_t)len)
        return HU_ERR_INVALID_ARGUMENT;
    if (top_object >= num_objects)
        return HU_ERR_INVALID_ARGUMENT;
    if (off_tbl < 8 || off_tbl + num_objects * offset_size > len - 32)
        return HU_ERR_INVALID_ARGUMENT;

    hu_bplist_t *p = (hu_bplist_t *)calloc(1, sizeof(*p));
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;
    p->buf = (unsigned char *)malloc(len);
    if (!p->buf) {
        free(p);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(p->buf, blob, len);
    p->buf_len = len;
    p->num_objects = (size_t)num_objects;
    p->top_object = (size_t)top_object;
    p->offset_table_off = (size_t)off_tbl;
    p->offset_size = offset_size;
    p->ref_size = ref_size;

    *out = p;
    return HU_OK;
}

void hu_bplist_free(hu_bplist_t *p) {
    if (!p)
        return;
    free(p->buf);
    free(p);
}

size_t hu_bplist_root(const hu_bplist_t *p) {
    return p ? p->top_object : 0;
}

hu_bplist_kind_t hu_bplist_kind(const hu_bplist_t *p, size_t idx) {
    if (!p)
        return HU_BPLIST_NULL;
    unsigned char m = object_marker(p, idx);
    if (m == 0xFF)
        return HU_BPLIST_NULL;
    unsigned char hi = m & 0xF0;
    unsigned char lo = m & 0x0F;
    switch (hi) {
    case 0x00:
        if (lo == 0x08 || lo == 0x09)
            return HU_BPLIST_BOOL;
        return HU_BPLIST_NULL;
    case 0x10:
        return HU_BPLIST_INT;
    case 0x20:
        return HU_BPLIST_REAL;
    case 0x30:
        return HU_BPLIST_DATE;
    case 0x40:
        return HU_BPLIST_DATA;
    case 0x50:
        return HU_BPLIST_STRING; /* ASCII */
    case 0x60:
        return HU_BPLIST_STRING; /* UTF-16BE */
    case 0x80:
        return HU_BPLIST_UID;
    case 0xA0:
        return HU_BPLIST_ARRAY;
    case 0xD0:
        return HU_BPLIST_DICT;
    default:
        return HU_BPLIST_NULL;
    }
}

bool hu_bplist_get_bool(const hu_bplist_t *p, size_t idx) {
    unsigned char m = object_marker(p, idx);
    return m == 0x09;
}

int64_t hu_bplist_get_int(const hu_bplist_t *p, size_t idx) {
    if (!p)
        return 0;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return 0;
    unsigned char m = p->buf[off];
    if ((m & 0xF0) != 0x10)
        return 0;
    size_t nbytes = (size_t)1u << (m & 0x0F);
    if (nbytes == 0 || nbytes > 8 || off + 1 + nbytes > p->buf_len)
        return 0;
    /* Per spec: ints of 1/2/4 bytes are unsigned; 8-byte ints are
     * signed two's-complement. Sign-extend the 8-byte case. */
    uint64_t v = read_be_u64(p->buf + off + 1, nbytes);
    if (nbytes == 8)
        return (int64_t)v;
    return (int64_t)v;
}

double hu_bplist_get_real(const hu_bplist_t *p, size_t idx) {
    if (!p)
        return 0.0;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return 0.0;
    unsigned char m = p->buf[off];
    if ((m & 0xF0) != 0x20)
        return 0.0;
    size_t nbytes = (size_t)1u << (m & 0x0F);
    if (off + 1 + nbytes > p->buf_len)
        return 0.0;
    if (nbytes == 4) {
        uint32_t bits = (uint32_t)read_be_u64(p->buf + off + 1, 4);
        float f;
        memcpy(&f, &bits, 4);
        return (double)f;
    }
    if (nbytes == 8) {
        uint64_t bits = read_be_u64(p->buf + off + 1, 8);
        double d;
        memcpy(&d, &bits, 8);
        return d;
    }
    return 0.0;
}

/* Append one UTF-8 codepoint (1-4 bytes) to out. Returns bytes written
 * (0 if cap exhausted). */
static size_t utf8_encode(uint32_t cp, char *out, size_t cap) {
    if (cp < 0x80) {
        if (cap < 1)
            return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2)
            return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3)
            return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cap < 4)
        return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t hu_bplist_get_string(const hu_bplist_t *p, size_t idx, char *out, size_t cap) {
    if (!p || !out || cap == 0)
        return 0;
    out[0] = '\0';
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return 0;
    unsigned char m = p->buf[off];
    unsigned char hi = m & 0xF0;
    if (hi != 0x50 && hi != 0x60)
        return 0;
    size_t nchars = 0;
    size_t body = 0;
    if (!decode_length(p, off, &nchars, &body))
        return 0;
    if (hi == 0x50) {
        /* ASCII (treated as Latin-1; high-bit bytes are unlikely but
         * we widen them to UTF-8 multibytes defensively). */
        if (body + nchars > p->buf_len)
            return 0;
        size_t w = 0;
        for (size_t i = 0; i < nchars && w + 1 < cap; i++) {
            unsigned char c = p->buf[body + i];
            if (c < 0x80) {
                out[w++] = (char)c;
            } else {
                size_t n = utf8_encode((uint32_t)c, out + w, cap - 1 - w);
                if (n == 0)
                    break;
                w += n;
            }
        }
        out[w] = '\0';
        return w;
    }
    /* UTF-16 big-endian; nchars is u16 count, not byte count. */
    if (body + nchars * 2 > p->buf_len)
        return 0;
    size_t w = 0;
    for (size_t i = 0; i < nchars && w + 1 < cap;) {
        uint32_t u = ((uint32_t)p->buf[body + i * 2] << 8) | (uint32_t)p->buf[body + i * 2 + 1];
        i++;
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i < nchars) {
            uint32_t low =
                ((uint32_t)p->buf[body + i * 2] << 8) | (uint32_t)p->buf[body + i * 2 + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                i++;
            } else {
                cp = 0xFFFD; /* lone high surrogate */
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            cp = 0xFFFD; /* lone low surrogate */
        } else {
            cp = u;
        }
        size_t n = utf8_encode(cp, out + w, cap - 1 - w);
        if (n == 0)
            break;
        w += n;
    }
    out[w] = '\0';
    return w;
}

const unsigned char *hu_bplist_get_data(const hu_bplist_t *p, size_t idx, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!p)
        return NULL;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return NULL;
    if ((p->buf[off] & 0xF0) != 0x40)
        return NULL;
    size_t n = 0, body = 0;
    if (!decode_length(p, off, &n, &body))
        return NULL;
    if (body + n > p->buf_len)
        return NULL;
    if (out_len)
        *out_len = n;
    return p->buf + body;
}

size_t hu_bplist_array_count(const hu_bplist_t *p, size_t idx) {
    if (!p)
        return 0;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return 0;
    if ((p->buf[off] & 0xF0) != 0xA0)
        return 0;
    size_t n = 0, body = 0;
    if (!decode_length(p, off, &n, &body))
        return 0;
    if (body + n * p->ref_size > p->buf_len)
        return 0;
    return n;
}

size_t hu_bplist_array_at(const hu_bplist_t *p, size_t idx, size_t i) {
    if (!p)
        return SIZE_MAX;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return SIZE_MAX;
    if ((p->buf[off] & 0xF0) != 0xA0)
        return SIZE_MAX;
    size_t n = 0, body = 0;
    if (!decode_length(p, off, &n, &body))
        return SIZE_MAX;
    if (i >= n)
        return SIZE_MAX;
    return read_ref(p, body + i * p->ref_size);
}

size_t hu_bplist_dict_count(const hu_bplist_t *p, size_t idx) {
    if (!p)
        return 0;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return 0;
    if ((p->buf[off] & 0xF0) != 0xD0)
        return 0;
    size_t n = 0, body = 0;
    if (!decode_length(p, off, &n, &body))
        return 0;
    if (body + n * 2 * p->ref_size > p->buf_len)
        return 0;
    return n;
}

/* Compare a stored string object's bytes to a NUL-terminated UTF-8
 * needle. Returns true on byte-exact match. We render the candidate
 * into a small stack buffer (capped at 256 bytes) — dict keys in
 * practice are short identifiers. */
static bool dict_key_matches(const hu_bplist_t *p, size_t key_idx, const char *needle) {
    char buf[256];
    size_t n = hu_bplist_get_string(p, key_idx, buf, sizeof(buf));
    if (n == 0)
        return false;
    return strcmp(buf, needle) == 0;
}

size_t hu_bplist_dict_lookup(const hu_bplist_t *p, size_t idx, const char *key) {
    if (!p || !key)
        return SIZE_MAX;
    size_t off = object_offset(p, idx);
    if (off == SIZE_MAX)
        return SIZE_MAX;
    if ((p->buf[off] & 0xF0) != 0xD0)
        return SIZE_MAX;
    size_t n = 0, body = 0;
    if (!decode_length(p, off, &n, &body))
        return SIZE_MAX;
    if (body + n * 2 * p->ref_size > p->buf_len)
        return SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        size_t k = read_ref(p, body + i * p->ref_size);
        if (k == SIZE_MAX)
            continue;
        if (dict_key_matches(p, k, key))
            return read_ref(p, body + n * p->ref_size + i * p->ref_size);
    }
    return SIZE_MAX;
}

/* Path walker: numeric segment ("0", "12") indexes into an array,
 * everything else is a dict key. */
size_t hu_bplist_get_string_at_path(const hu_bplist_t *p, const char *const *path, char *out,
                                    size_t cap) {
    if (!p || !path || !out || cap == 0)
        return 0;
    out[0] = '\0';
    size_t cur = hu_bplist_root(p);
    for (size_t i = 0; path[i] != NULL; i++) {
        const char *seg = path[i];
        hu_bplist_kind_t k = hu_bplist_kind(p, cur);
        if (k == HU_BPLIST_ARRAY) {
            /* Numeric segment */
            size_t v = 0;
            bool ok = (seg[0] != '\0');
            for (const char *c = seg; *c; c++) {
                if (*c < '0' || *c > '9') {
                    ok = false;
                    break;
                }
                v = v * 10 + (size_t)(*c - '0');
            }
            if (!ok)
                return 0;
            cur = hu_bplist_array_at(p, cur, v);
        } else if (k == HU_BPLIST_DICT) {
            cur = hu_bplist_dict_lookup(p, cur, seg);
        } else {
            return 0;
        }
        if (cur == SIZE_MAX)
            return 0;
    }
    if (hu_bplist_kind(p, cur) != HU_BPLIST_STRING)
        return 0;
    return hu_bplist_get_string(p, cur, out, cap);
}
