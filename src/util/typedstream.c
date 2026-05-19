/* src/util/typedstream.c
 *
 * Phase 4 of docs/plans/2026-05-18-imessage-sota.md.
 *
 * Clean-room attribute-run extractor for iOS attributedBody typedstream
 * blobs. See include/human/util/typedstream.h for the contract.
 *
 * What this parser does:
 *   1. Anchors on the 0x01 0x2B text-length-prefixed segment (same as
 *      hu_imessage_extract_attributed_body) and extracts the plain text.
 *   2. Scans the tail of the blob (everything after the text segment)
 *      for occurrences of well-known attribute key strings:
 *        __kIMMessagePartAttributeName     (run delimiter / part index)
 *        __kIMMentionConfirmedMention      (mention)
 *        __kIMLinkAttributeName            (link span)
 *        __kIMOneTimeCodeAttributeName     (OTP/2FA span)
 *        __kIMTextEffectAttributeName      (iOS 18 animation)
 *        __kIMBoldAttributeName            (bold formatting)
 *        __kIMItalicAttributeName          (italic)
 *        __kIMStrikethroughAttributeName   (strikethrough)
 *        __kIMUnderlineAttributeName       (underline)
 *   3. For each attribute occurrence, walks back ~32 bytes to find the
 *      nearest preceding range pair (two ints) — that is the (start,
 *      length) of the run in the plain text.
 *   4. For mention/link/effect runs, walks forward from the key string
 *      to find the associated value (a string for handle/URL, an int for
 *      effect code).
 *
 * The forward / backward windows are bounded so a malformed blob cannot
 * cause unbounded reads. All offsets are bounds-checked. */

#include "human/util/typedstream.h"

#include <stdint.h>
#include <string.h>

/* Bound how far back/forward we will look around an attribute key. The
 * typedstream encoding places the range ints immediately before the
 * attribute dict and the value immediately after the key; in practice
 * the gap is < 32 bytes. We use 64 as a generous bound. */
#define HU_TS_SCAN_RADIUS 64

/* iOS 18 text-effect codes. Mapping documented in the public iOS 18
 * Messages "text animations" surface. Values 1..8 confirmed by multiple
 * community trackers; we store them as effect names so the personal
 * model can render them as English. */
static const char *text_effect_name(int code) {
    switch (code) {
    case 1:
        return "big";
    case 2:
        return "small";
    case 3:
        return "shake";
    case 4:
        return "nod";
    case 5:
        return "explode";
    case 6:
        return "ripple";
    case 7:
        return "bloom";
    case 8:
        return "jitter";
    default:
        return NULL;
    }
}

/* Decode the length / body-start of a 0x01 0x2B text segment. Returns
 * true with *text_len + *text_start set on success; false otherwise.
 *
 * Mirrors the on-the-wire form used by hu_imessage_extract_attributed_body
 * but does not modify out buffers. */
static bool decode_text_segment(const unsigned char *blob, size_t blob_len, size_t marker_at,
                                size_t *out_text_start, size_t *out_text_len) {
    if (marker_at + 3 >= blob_len)
        return false;
    size_t text_len = 0;
    size_t text_start = 0;
    unsigned char lb = blob[marker_at + 2];
    if (lb < 0x80) {
        text_len = lb;
        text_start = marker_at + 3;
    } else {
        size_t len_bytes = lb & 0x7F;
        if (len_bytes == 0 || len_bytes > 4 || marker_at + 3 + len_bytes > blob_len)
            return false;
        for (size_t b = 0; b < len_bytes; b++)
            text_len |= (size_t)blob[marker_at + 3 + b] << (8 * b);
        text_start = marker_at + 3 + len_bytes;
    }
    if (text_start + text_len > blob_len)
        return false;
    *out_text_start = text_start;
    *out_text_len = text_len;
    return true;
}

/* Find the 0x01 0x2B text-segment marker. Returns SIZE_MAX if absent. */
static size_t find_text_marker(const unsigned char *blob, size_t blob_len) {
    if (blob_len < 4)
        return SIZE_MAX;
    for (size_t i = 0; i + 3 < blob_len; i++) {
        if (blob[i] == 0x01 && blob[i + 1] == 0x2B)
            return i;
    }
    return SIZE_MAX;
}

/* Find the first occurrence of `needle` (NUL-terminated) inside `hay`
 * within [start, hay_len). Returns SIZE_MAX if not found.
 *
 * `needle` must NOT be empty. */
static size_t find_substring(const unsigned char *hay, size_t hay_len, size_t start,
                             const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || start >= hay_len || hay_len - start < nl)
        return SIZE_MAX;
    for (size_t i = start; i + nl <= hay_len; i++) {
        if (hay[i] == (unsigned char)needle[0] && memcmp(hay + i, needle, nl) == 0)
            return i;
    }
    return SIZE_MAX;
}

/* Try to read an int that lives at offset `at` in the blob. The
 * typedstream integer encodings we expect to encounter:
 *   0x81 nn nn               — unsigned 16-bit BE inline  (some variants)
 *   0x82 nn nn               — signed   16-bit LE
 *   0x83 nn nn nn nn         — signed   32-bit LE
 *   else                     — inline byte (treat as small unsigned)
 *
 * Returns true with *out_val + *out_consumed on success; false on
 * overflow. */
static bool read_int_marker(const unsigned char *blob, size_t blob_len, size_t at, int64_t *out_val,
                            size_t *out_consumed) {
    if (at >= blob_len)
        return false;
    unsigned char m = blob[at];
    if (m == 0x81) {
        if (at + 3 > blob_len)
            return false;
        /* 16-bit; some serializers use BE, some LE. Empirically iOS
         * uses LE for typedstream ints. */
        uint16_t v = (uint16_t)blob[at + 1] | ((uint16_t)blob[at + 2] << 8);
        *out_val = (int64_t)v;
        *out_consumed = 3;
        return true;
    }
    if (m == 0x82) {
        if (at + 3 > blob_len)
            return false;
        int16_t v = (int16_t)((uint16_t)blob[at + 1] | ((uint16_t)blob[at + 2] << 8));
        *out_val = (int64_t)v;
        *out_consumed = 3;
        return true;
    }
    if (m == 0x83) {
        if (at + 5 > blob_len)
            return false;
        uint32_t v = (uint32_t)blob[at + 1] | ((uint32_t)blob[at + 2] << 8) |
                     ((uint32_t)blob[at + 3] << 16) | ((uint32_t)blob[at + 4] << 24);
        *out_val = (int64_t)(int32_t)v;
        *out_consumed = 5;
        return true;
    }
    /* Inline small byte — typedstream often stores 0..127 directly. */
    if (m < 0x80) {
        *out_val = (int64_t)m;
        *out_consumed = 1;
        return true;
    }
    return false;
}

/* Read a STRICT int marker — only 0x81/0x82/0x83 are accepted. Inline
 * bytes < 0x80 are NOT accepted (those false-trigger inside text bodies
 * and length-prefixed strings). */
static bool read_strict_int_marker(const unsigned char *blob, size_t blob_len, size_t at,
                                   int64_t *out_val, size_t *out_consumed) {
    if (at >= blob_len)
        return false;
    unsigned char m = blob[at];
    if (m != 0x81 && m != 0x82 && m != 0x83)
        return false;
    return read_int_marker(blob, blob_len, at, out_val, out_consumed);
}

/* Scan back from `key_at` up to HU_TS_SCAN_RADIUS bytes looking for two
 * consecutive STRICT int markers (0x81/0x82/0x83 only). Returns true
 * with *out_start and *out_length on success. The CLOSEST-to-key pair
 * wins (last match wins in the forward iteration). */
static bool find_preceding_range(const unsigned char *blob, size_t blob_len, size_t key_at,
                                 size_t *out_start, size_t *out_length) {
    size_t lo = (key_at > HU_TS_SCAN_RADIUS) ? key_at - HU_TS_SCAN_RADIUS : 0;
    bool found = false;
    int64_t best_start = 0, best_length = 0;
    for (size_t i = lo; i + 1 < key_at; i++) {
        int64_t a = 0, b = 0;
        size_t na = 0, nb = 0;
        if (!read_strict_int_marker(blob, blob_len, i, &a, &na))
            continue;
        if (i + na >= key_at)
            continue;
        if (!read_strict_int_marker(blob, blob_len, i + na, &b, &nb))
            continue;
        if (a < 0 || b <= 0)
            continue;
        best_start = a;
        best_length = b;
        found = true;
    }
    if (!found)
        return false;
    *out_start = (size_t)best_start;
    *out_length = (size_t)best_length;
    return true;
}

/* Scan forward from `after` up to HU_TS_SCAN_RADIUS bytes looking for a
 * length-prefixed ASCII / UTF-8 string. A typedstream string is encoded
 * as a length byte (< 0x80 for inline length) followed by the bytes.
 * Some strings have a 0x2B-style length-int prefix; we accept either.
 *
 * Writes up to `cap-1` bytes to `out`, NUL-terminates, and returns true
 * if anything was extracted. */
static bool find_following_string(const unsigned char *blob, size_t blob_len, size_t after,
                                  char *out, size_t cap) {
    if (cap < 2)
        return false;
    size_t hi = after + HU_TS_SCAN_RADIUS;
    if (hi > blob_len)
        hi = blob_len;
    /* Heuristic: find the first byte sequence that looks like a length
     * (1..127) followed by that many printable bytes. */
    for (size_t i = after; i + 1 < hi; i++) {
        unsigned char lb = blob[i];
        /* Some encodings use 0x2B as a sentinel before the length byte;
         * skip it. */
        if (lb == 0x2B) {
            if (i + 2 >= hi)
                continue;
            unsigned char real_len = blob[i + 1];
            if (real_len == 0 || real_len >= 0x80)
                continue;
            if (i + 2 + real_len > hi)
                continue;
            /* Verify printability of at least the first byte. */
            unsigned char c0 = blob[i + 2];
            if (c0 < 0x20 || c0 >= 0x7F)
                continue;
            size_t n = real_len;
            if (n >= cap)
                n = cap - 1;
            memcpy(out, blob + i + 2, n);
            out[n] = '\0';
            return true;
        }
        if (lb == 0 || lb >= 0x80)
            continue;
        if (i + 1 + lb > hi)
            continue;
        unsigned char c0 = blob[i + 1];
        if (c0 < 0x20 || c0 >= 0x7F)
            continue;
        /* Reject obvious non-strings: if the candidate "string" starts
         * with two underscores it is probably another key, not a value. */
        if (c0 == '_' && i + 2 < hi && blob[i + 2] == '_')
            continue;
        size_t n = lb;
        if (n >= cap)
            n = cap - 1;
        memcpy(out, blob + i + 1, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

/* Try to read an int value (effect code) that follows the key string.
 * Forward window: HU_TS_SCAN_RADIUS bytes. */
static bool find_following_int(const unsigned char *blob, size_t blob_len, size_t after,
                               int64_t *out_val) {
    size_t hi = after + HU_TS_SCAN_RADIUS;
    if (hi > blob_len)
        hi = blob_len;
    for (size_t i = after; i < hi; i++) {
        unsigned char m = blob[i];
        if (m == 0x81 || m == 0x82 || m == 0x83) {
            size_t consumed = 0;
            if (read_int_marker(blob, hi, i, out_val, &consumed))
                return true;
        }
    }
    /* Fall back: a single small inline byte right after the key. */
    if (after < hi) {
        unsigned char m = blob[after];
        if (m > 0 && m < 0x80) {
            *out_val = (int64_t)m;
            return true;
        }
    }
    return false;
}

/* Append a run to the output array if room remains. */
static void emit_run(hu_attribute_run_t *runs_out, size_t runs_cap, size_t *count,
                     size_t range_start, size_t range_length, hu_attribute_kind_t kind,
                     const char *detail) {
    if (*count >= runs_cap)
        return;
    hu_attribute_run_t *r = &runs_out[*count];
    r->range_start = range_start;
    r->range_length = range_length;
    r->kind = kind;
    if (detail && detail[0]) {
        size_t n = strlen(detail);
        if (n >= sizeof(r->detail))
            n = sizeof(r->detail) - 1;
        memcpy(r->detail, detail, n);
        r->detail[n] = '\0';
    } else {
        r->detail[0] = '\0';
    }
    (*count)++;
}

/* Insertion-sort runs by range_start. */
static void sort_runs(hu_attribute_run_t *runs, size_t count) {
    for (size_t i = 1; i < count; i++) {
        hu_attribute_run_t tmp = runs[i];
        size_t j = i;
        while (j > 0 && runs[j - 1].range_start > tmp.range_start) {
            runs[j] = runs[j - 1];
            j--;
        }
        runs[j] = tmp;
    }
}

/* Look for one specific attribute key in [search_from, blob_len). For
 * each occurrence, derive a run and emit it. */
static void scan_key(const unsigned char *blob, size_t blob_len, size_t search_from,
                     const char *key, hu_attribute_kind_t kind, bool extract_string,
                     bool extract_effect_int, hu_attribute_run_t *runs_out, size_t runs_cap,
                     size_t *count) {
    size_t pos = search_from;
    size_t klen = strlen(key);
    while (pos < blob_len) {
        size_t hit = find_substring(blob, blob_len, pos, key);
        if (hit == SIZE_MAX)
            break;
        size_t range_start = 0, range_length = 0;
        bool have_range = find_preceding_range(blob, blob_len, hit, &range_start, &range_length);
        char detail[128];
        detail[0] = '\0';
        if (extract_string) {
            (void)find_following_string(blob, blob_len, hit + klen, detail, sizeof(detail));
        } else if (extract_effect_int) {
            int64_t code = 0;
            if (find_following_int(blob, blob_len, hit + klen, &code)) {
                const char *name = text_effect_name((int)code);
                if (name) {
                    size_t n = strlen(name);
                    memcpy(detail, name, n);
                    detail[n] = '\0';
                }
            }
        }
        if (have_range)
            emit_run(runs_out, runs_cap, count, range_start, range_length, kind, detail);
        else
            emit_run(runs_out, runs_cap, count, 0, 0, kind, detail);
        pos = hit + klen;
    }
}

hu_error_t hu_imessage_extract_attribute_runs(const unsigned char *blob, size_t blob_len,
                                              char *text_out, size_t text_cap,
                                              hu_attribute_run_t *runs_out, size_t runs_cap,
                                              size_t *runs_count_out) {
    if (!blob || blob_len < 4 || !text_out || text_cap < 2 || !runs_count_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (runs_cap > 0 && !runs_out)
        return HU_ERR_INVALID_ARGUMENT;

    text_out[0] = '\0';
    *runs_count_out = 0;

    /* Step 1: locate and extract the plain-text segment. */
    size_t marker_at = find_text_marker(blob, blob_len);
    if (marker_at == SIZE_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    size_t text_start = 0, text_len = 0;
    if (!decode_text_segment(blob, blob_len, marker_at, &text_start, &text_len))
        return HU_ERR_INVALID_ARGUMENT;

    if (text_len + 1 > text_cap)
        return HU_ERR_LIMIT_REACHED;

    memcpy(text_out, blob + text_start, text_len);
    text_out[text_len] = '\0';

    /* Step 2: scan the tail for attribute keys. */
    size_t tail_start = text_start + text_len;
    if (tail_start >= blob_len || runs_cap == 0)
        return HU_OK;

    size_t count = 0;

    scan_key(blob, blob_len, tail_start, "__kIMMentionConfirmedMention", HU_ATTR_MENTION,
             /*str=*/true, /*eff=*/false, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMLinkAttributeName", HU_ATTR_LINK,
             /*str=*/true, /*eff=*/false, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMOneTimeCodeAttributeName", HU_ATTR_OTP_CODE,
             /*str=*/false, /*eff=*/false, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMTextEffectAttributeName", HU_ATTR_TEXT_EFFECT,
             /*str=*/false, /*eff=*/true, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMBoldAttributeName", HU_ATTR_BOLD,
             /*str=*/false, /*eff=*/false, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMItalicAttributeName", HU_ATTR_ITALIC,
             /*str=*/false, /*eff=*/false, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMStrikethroughAttributeName", HU_ATTR_STRIKETHROUGH,
             /*str=*/false, /*eff=*/false, runs_out, runs_cap, &count);
    scan_key(blob, blob_len, tail_start, "__kIMUnderlineAttributeName", HU_ATTR_UNDERLINE,
             /*str=*/false, /*eff=*/false, runs_out, runs_cap, &count);

    sort_runs(runs_out, count);
    *runs_count_out = count;
    return HU_OK;
}

bool hu_imessage_runs_contain_otp(const hu_attribute_run_t *runs, size_t count) {
    if (!runs)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (runs[i].kind == HU_ATTR_OTP_CODE)
            return true;
    }
    return false;
}

const hu_attribute_run_t *hu_imessage_runs_first_mention(const hu_attribute_run_t *runs,
                                                         size_t count) {
    if (!runs)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (runs[i].kind == HU_ATTR_MENTION)
            return &runs[i];
    }
    return NULL;
}
