/* src/util/harmony_filter.c — streaming-safe Harmony channel-marker
 * stripper. See include/human/util/harmony_filter.h for the contract. */

#include "human/util/harmony_filter.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HU_HARMONY_FILTER_INIT_CAP ((size_t)256)
#define HU_HARMONY_FILTER_MAX_CAP  ((size_t)64 * 1024)

/* Max possible marker length: `<|channel|>commentary` = 21 bytes.
 * LOOKAHEAD must exceed that so a `<` near the tail still has enough
 * bytes after it to be resolvable. Round up generously for headroom. */
#define HU_HARMONY_LOOKAHEAD ((size_t)64)

struct hu_harmony_filter {
    hu_allocator_t *alloc;
    char *buf;
    size_t len;
    size_t cap;
};

/* Known Harmony channel values that can follow an unclosed `<|TAG>`. */
static const char *const HU_HARMONY_CHANNEL_VALUES[] = {"thought", "final", "analysis",
                                                        "commentary"};
static const size_t HU_HARMONY_CHANNEL_VALUE_COUNT =
    sizeof(HU_HARMONY_CHANNEL_VALUES) / sizeof(HU_HARMONY_CHANNEL_VALUES[0]);

/* Match the well-formed `<|TAG|>` form. Tag is `[a-zA-Z_]+`. Returns
 * the byte offset AFTER the closing `>`, or 0 if no match starting at
 * `at` (0 is fine as a sentinel because `at >= 2` whenever this is
 * called — we already saw `<|`). */
static size_t match_closed_marker(const char *in, size_t in_len, size_t at) {
    /* in[at-2..at-1] == "<|" already verified by caller */
    size_t scan = at;
    while (scan < in_len) {
        char c = in[scan];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
            scan++;
        else
            break;
    }
    if (scan == at)
        return 0; /* no tag chars */
    if (scan + 1 < in_len && in[scan] == '|' && in[scan + 1] == '>')
        return scan + 2;
    return 0;
}

/* Match the unclosed `<|TAG>VALUE?` leak form. Returns the byte offset
 * after the (optional) trailing channel value, or 0 if no match. */
static size_t match_open_marker(const char *in, size_t in_len, size_t at) {
    size_t scan = at;
    while (scan < in_len) {
        char c = in[scan];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
            scan++;
        else
            break;
    }
    if (scan == at)
        return 0;
    if (scan >= in_len || in[scan] != '>')
        return 0;
    scan++;
    for (size_t k = 0; k < HU_HARMONY_CHANNEL_VALUE_COUNT; k++) {
        size_t cv_len = strlen(HU_HARMONY_CHANNEL_VALUES[k]);
        if (scan + cv_len <= in_len &&
            memcmp(in + scan, HU_HARMONY_CHANNEL_VALUES[k], cv_len) == 0) {
            scan += cv_len;
            break;
        }
    }
    return scan;
}

/* Strip-pass over a complete (no held-back) byte range. Mirrors the
 * non-streaming `strip_harmony` in src/agent/response_guard.c.
 *
 * out must have room for at least in_len + 1 bytes (markers only
 * shrink). Returns the bytes-written count via *out_len. */
static void strip_pass(const char *in, size_t in_len, char *out, size_t *out_len) {
    size_t r = 0;
    size_t w = 0;
    while (r < in_len) {
        if (r + 1 < in_len && in[r] == '<' && in[r + 1] == '|') {
            size_t after = match_closed_marker(in, in_len, r + 2);
            if (after) {
                r = after;
                continue;
            }
            after = match_open_marker(in, in_len, r + 2);
            if (after) {
                r = after;
                continue;
            }
        }
        out[w++] = in[r++];
    }
    *out_len = w;
}

hu_error_t hu_harmony_filter_init(hu_allocator_t *alloc, hu_harmony_filter_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_harmony_filter_t *f = (hu_harmony_filter_t *)alloc->alloc(alloc->ctx, sizeof(*f));
    if (!f)
        return HU_ERR_OUT_OF_MEMORY;
    memset(f, 0, sizeof(*f));
    f->alloc = alloc;
    f->cap = HU_HARMONY_FILTER_INIT_CAP;
    f->buf = (char *)alloc->alloc(alloc->ctx, f->cap);
    if (!f->buf) {
        alloc->free(alloc->ctx, f, sizeof(*f));
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = f;
    return HU_OK;
}

void hu_harmony_filter_free(hu_harmony_filter_t *f) {
    if (!f)
        return;
    if (f->buf)
        f->alloc->free(f->alloc->ctx, f->buf, f->cap);
    f->alloc->free(f->alloc->ctx, f, sizeof(*f));
}

static hu_error_t ensure_cap(hu_harmony_filter_t *f, size_t needed) {
    if (needed <= f->cap)
        return HU_OK;
    if (needed > HU_HARMONY_FILTER_MAX_CAP)
        return HU_ERR_OUT_OF_MEMORY;
    size_t new_cap = f->cap;
    while (new_cap < needed) {
        new_cap *= (size_t)2;
        if (new_cap > HU_HARMONY_FILTER_MAX_CAP)
            new_cap = HU_HARMONY_FILTER_MAX_CAP;
    }
    char *new_buf = (char *)f->alloc->alloc(f->alloc->ctx, new_cap);
    if (!new_buf)
        return HU_ERR_OUT_OF_MEMORY;
    if (f->len > 0)
        memcpy(new_buf, f->buf, f->len);
    f->alloc->free(f->alloc->ctx, f->buf, f->cap);
    f->buf = new_buf;
    f->cap = new_cap;
    return HU_OK;
}

/* Compute the safe-to-emit boundary inside the current accumulator.
 * Bytes [0, safe_boundary) can be passed through strip_pass without
 * risk of an incomplete marker at the tail; bytes [safe_boundary,
 * len) must be held back for the next push or final finish.
 *
 * Rule: if a `<` byte exists within the last LOOKAHEAD bytes, the
 * safe boundary is the position of that `<`. Otherwise safe boundary
 * is the full length. When the accumulator is shorter than LOOKAHEAD
 * AND contains any `<`, we hold the whole thing back to avoid emitting
 * a marker prefix as literal text. */
static size_t safe_boundary(const char *buf, size_t len) {
    /* Hot path: empty buffer. */
    if (len == 0)
        return 0;
    /* Walk the LOOKAHEAD window at the tail backward to find the
     * latest `<`. If one exists, hold from that position onward
     * (next push or finish will resolve whether it's a marker). */
    size_t window_start = (len > HU_HARMONY_LOOKAHEAD) ? len - HU_HARMONY_LOOKAHEAD : 0;
    for (size_t i = len; i > window_start; i--) {
        if (buf[i - 1] == '<')
            return i - 1;
    }
    /* No `<` anywhere in the last LOOKAHEAD bytes — the entire
     * buffer is safe to emit. Any markers starting earlier in the
     * buffer have full lookahead available to strip_pass, and the
     * tail has no marker start at all. */
    return len;
}

/* Build an empty-string output (caller convenience to avoid duplication). */
static hu_error_t emit_empty(hu_harmony_filter_t *f, char **out, size_t *out_len) {
    char *e = (char *)f->alloc->alloc(f->alloc->ctx, (size_t)1);
    if (!e)
        return HU_ERR_OUT_OF_MEMORY;
    e[0] = '\0';
    *out = e;
    *out_len = 0;
    return HU_OK;
}

hu_error_t hu_harmony_filter_push(hu_harmony_filter_t *f, const char *bytes, size_t n, char **out,
                                  size_t *out_len) {
    if (!f || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (n > 0 && bytes) {
        hu_error_t e = ensure_cap(f, f->len + n);
        if (e != HU_OK)
            return e;
        memcpy(f->buf + f->len, bytes, n);
        f->len += n;
    }

    size_t boundary = safe_boundary(f->buf, f->len);
    if (boundary == 0)
        return emit_empty(f, out, out_len);

    /* Strip-pass produces at most `boundary` output bytes (markers
     * only shrink). One extra byte for the NUL terminator. */
    char *result = (char *)f->alloc->alloc(f->alloc->ctx, boundary + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    size_t result_len = 0;
    strip_pass(f->buf, boundary, result, &result_len);
    result[result_len] = '\0';

    /* Slide any held-back tail to the front of the accumulator. */
    size_t remaining = f->len - boundary;
    if (remaining > 0)
        memmove(f->buf, f->buf + boundary, remaining);
    f->len = remaining;

    *out = result;
    *out_len = result_len;
    return HU_OK;
}

hu_error_t hu_harmony_filter_finish(hu_harmony_filter_t *f, char **out, size_t *out_len) {
    if (!f || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (f->len == 0)
        return emit_empty(f, out, out_len);

    /* End-of-stream — no more bytes coming. Run strip_pass over the
     * full remainder; any incomplete marker becomes literal text. */
    char *result = (char *)f->alloc->alloc(f->alloc->ctx, f->len + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    size_t result_len = 0;
    strip_pass(f->buf, f->len, result, &result_len);
    result[result_len] = '\0';
    f->len = 0;

    *out = result;
    *out_len = result_len;
    return HU_OK;
}

size_t hu_harmony_filter_buffered_bytes(const hu_harmony_filter_t *f) {
    return f ? f->len : 0;
}
