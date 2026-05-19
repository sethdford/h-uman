/* include/human/util/bplist.h
 *
 * Apple binary plist (bplist00) parser. Phase 3 keystone dependency for
 * docs/plans/2026-05-18-imessage-sota.md — used to extract audio-message
 * transcripts, edit-history chains, and balloon-payload fields from the
 * blobs Apple stores in chat.db.
 *
 * Clean-room implementation of the publicly documented bplist00 format.
 * No CoreFoundation, no libplist, no external dependencies beyond libc.
 *
 * The parser is READ-ONLY (no writer). It validates the magic, the
 * trailer, and every offset it dereferences; malformed input is
 * rejected with HU_ERR_INVALID_ARGUMENT rather than dereferenced.
 *
 * Lifetime: hu_bplist_parse copies the input blob into an internal
 * buffer, so callers may free their input immediately. All pointers
 * returned by accessors point into that internal buffer and remain
 * valid until hu_bplist_free.
 */

#ifndef HU_UTIL_BPLIST_H
#define HU_UTIL_BPLIST_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_BPLIST_NULL = 0,
    HU_BPLIST_BOOL,
    HU_BPLIST_INT,
    HU_BPLIST_REAL,
    HU_BPLIST_DATE,
    HU_BPLIST_DATA,
    HU_BPLIST_STRING,
    HU_BPLIST_ARRAY,
    HU_BPLIST_DICT,
    HU_BPLIST_UID,
} hu_bplist_kind_t;

typedef struct hu_bplist hu_bplist_t; /* opaque */

/* Parse a bplist00 blob. Validates the "bplist00" magic, reads the
 * 32-byte trailer, and indexes the object offset table. Returns HU_OK
 * on success, HU_ERR_INVALID_ARGUMENT on malformed input (truncated,
 * wrong magic, offset out-of-bounds, invalid trailer). */
hu_error_t hu_bplist_parse(const unsigned char *blob, size_t len, hu_bplist_t **out);

/* Free a parsed plist and its internal buffer. NULL-safe. */
void hu_bplist_free(hu_bplist_t *p);

/* Index of the root (top) object. */
size_t hu_bplist_root(const hu_bplist_t *p);

/* Kind tag for the object at `idx`. Returns HU_BPLIST_NULL for
 * out-of-range or invalid indices. */
hu_bplist_kind_t hu_bplist_kind(const hu_bplist_t *p, size_t idx);

/* Type-specific accessors. Each returns the documented "zero" value if
 * the index is out of range or the object is the wrong type. */
bool hu_bplist_get_bool(const hu_bplist_t *p, size_t idx);
int64_t hu_bplist_get_int(const hu_bplist_t *p, size_t idx);
double hu_bplist_get_real(const hu_bplist_t *p, size_t idx);

/* Copy the string at `idx` (ASCII or UTF-16 source, always UTF-8 out)
 * into `out`. NUL-terminates if cap > 0. Returns the number of bytes
 * written excluding the NUL terminator, or 0 on type mismatch /
 * truncation-to-zero. */
size_t hu_bplist_get_string(const hu_bplist_t *p, size_t idx, char *out, size_t cap);

/* Return a pointer to the raw bytes of the data object at `idx`,
 * writing the length to *out_len. The pointer is valid until
 * hu_bplist_free. Returns NULL on type mismatch. */
const unsigned char *hu_bplist_get_data(const hu_bplist_t *p, size_t idx, size_t *out_len);

/* Array: element count and per-element child indices. */
size_t hu_bplist_array_count(const hu_bplist_t *p, size_t idx);
size_t hu_bplist_array_at(const hu_bplist_t *p, size_t idx, size_t i);

/* Dict: number of (key,value) pairs and key-string lookup. Returns
 * SIZE_MAX from hu_bplist_dict_lookup if the key is not present. */
size_t hu_bplist_dict_count(const hu_bplist_t *p, size_t idx);
size_t hu_bplist_dict_lookup(const hu_bplist_t *p, size_t idx, const char *key);

/* High-level: walk a NULL-terminated path of dict keys (and numeric
 * indices for arrays — pass the decimal string, e.g. "0") from the
 * root and copy the resulting STRING value into `out`. Returns bytes
 * written or 0 on any failure (missing key, type mismatch, OOB).
 *
 * Example: const char *path[] = {"ec", "0", "t", NULL}; */
size_t hu_bplist_get_string_at_path(const hu_bplist_t *p, const char *const *path, char *out,
                                    size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_UTIL_BPLIST_H */
