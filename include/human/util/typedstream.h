/* include/human/util/typedstream.h
 *
 * Phase 4 of docs/plans/2026-05-18-imessage-sota.md: structured extraction
 * of the ATTRIBUTE LAYER from an iOS `attributedBody` typedstream blob.
 *
 * The existing src/channels/imessage.c::hu_imessage_extract_attributed_body
 * extracts plain text only (anchored on the 0x01 0x2B text-length-prefixed
 * segment). This module adds the structured-spans layer on top: mentions,
 * one-time-code detection, link spans, iOS 18 text-animation effects, and
 * basic formatting runs.
 *
 * Clean-room implementation. The typedstream format is the legacy
 * NSArchiver binary serialization (pre-NSKeyedArchiver); the algorithm
 * documented in imessage-exporter's typedstream module is the reference,
 * but every byte of the implementation here is original. No GPL code.
 *
 * Strategy: this is NOT a full NSArchiver decoder. We anchor on the
 * well-known marker bytes documented in the typedstream format
 * (`streamtyped` magic, the 0x01 0x2B text segment, the 0x84 / 0x86
 * shared-object markers, and known Cocoa class-name strings) and on the
 * persistent string keys of attribute-run dictionaries
 * (`__kIMMessagePartAttributeName`, `__kIMMentionConfirmedMention`,
 * `__kIMOneTimeCodeAttributeName`, etc.). Range ints are read from int
 * markers (0x82 = signed-int-16 follows, 0x83 = signed-int-32 follows,
 * 0x81 / 0x80 = inline / NULL).
 *
 * The parser is forgiving: malformed blobs produce zero attribute runs
 * but never crash. Plain text always extracts when the 0x01 0x2B anchor
 * is present.
 */

#ifndef HU_UTIL_TYPEDSTREAM_H
#define HU_UTIL_TYPEDSTREAM_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_ATTR_PLAIN = 0,
    HU_ATTR_MENTION,     /* @-mention; detail = canonical handle */
    HU_ATTR_LINK,        /* URL / tel: / mailto:; detail = URL */
    HU_ATTR_OTP_CODE,    /* one-time / 2FA code — DROP from ingest */
    HU_ATTR_TEXT_EFFECT, /* iOS 18 animation; detail = effect name */
    HU_ATTR_BOLD,
    HU_ATTR_ITALIC,
    HU_ATTR_STRIKETHROUGH,
    HU_ATTR_UNDERLINE,
} hu_attribute_kind_t;

typedef struct {
    size_t range_start;  /* byte offset into the plain text */
    size_t range_length; /* bytes */
    hu_attribute_kind_t kind;
    char detail[128]; /* handle, URL, effect name, or empty */
} hu_attribute_run_t;

/* Parse the attribute layer of an attributedBody blob.
 *
 *   text_out       — receives the NUL-terminated plain-text contents
 *   text_cap       — capacity of text_out (must be >= 2)
 *   runs_out       — receives attribute runs ordered by range_start
 *   runs_cap       — capacity of runs_out
 *   runs_count_out — set to the number of runs written
 *
 * Returns:
 *   HU_OK on success (including 0 runs found — a plain message is valid).
 *   HU_ERR_INVALID_ARGUMENT on NULL inputs or malformed/empty blob.
 *   HU_ERR_LIMIT_REACHED if the plain text does not fit in text_cap
 *                        (the codebase's substitute for "buffer overflow"
 *                        — see include/human/core/error.h; no dedicated
 *                        HU_ERR_BUFFER_OVERFLOW exists).
 */
hu_error_t hu_imessage_extract_attribute_runs(const unsigned char *blob, size_t blob_len,
                                              char *text_out, size_t text_cap,
                                              hu_attribute_run_t *runs_out, size_t runs_cap,
                                              size_t *runs_count_out);

/* True if any run in the array indicates this message is an OTP / 2FA
 * code that should NOT be ingested into the personal model. */
bool hu_imessage_runs_contain_otp(const hu_attribute_run_t *runs, size_t count);

/* Return the first mention in the runs (ordered by range_start), or
 * NULL if none. Used by group-chat ingest for social-graph signal. */
const hu_attribute_run_t *hu_imessage_runs_first_mention(const hu_attribute_run_t *runs,
                                                         size_t count);

#ifdef __cplusplus
}
#endif
#endif /* HU_UTIL_TYPEDSTREAM_H */
