/* Training-data PII redaction — Phase A1.2 of the SOTA roadmap.
 *
 * Single-pass scanner over UTF-8 text. Each pattern attempts to match
 * starting at the current offset. A match writes the replacement token
 * into the output buffer and advances past the matched span. Non-match
 * copies one byte and advances. Truncation is silent — caller checks
 * `*out_len` against expected bounds.
 *
 * Pattern detection is deliberately conservative: we anchor on
 * non-alphanumeric boundaries on both sides to keep false-positive
 * rate low. The cost is a handful of edge cases (e.g. an email that
 * butts up against a letter without space) where the redactor backs
 * off rather than risk redacting non-PII. For training-data hygiene
 * this is the right trade-off — the eval criterion is "< 0.1% PII
 * leak rate", not "< 0.1% false-redaction rate". */

#include "human/ml/training_data_quality.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static bool is_word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

/* Boundary check: byte at offset `idx` is at the start of a word when
 * `idx == 0` or the preceding byte is a non-word character. Used to
 * anchor patterns so we don't match "user" inside "useragent" — a
 * narrower equivalent of \b in regex. */
static bool at_word_boundary_start(const char *text, size_t text_len, size_t idx) {
    (void)text_len;
    if (idx == 0)
        return true;
    return !is_word_char((unsigned char)text[idx - 1]);
}

/* Boundary check at end: byte after the matched span is past the end
 * of the buffer or a non-word character. */
static bool at_word_boundary_end(const char *text, size_t text_len, size_t end) {
    if (end >= text_len)
        return true;
    return !is_word_char((unsigned char)text[end]);
}

/* Append `s` (length `len`) into `out` at `*pos`, respecting `cap - 1`
 * for the NUL. Returns true when the full string fit. */
static bool append_n(char *out, size_t cap, size_t *pos, const char *s, size_t len) {
    if (cap == 0)
        return false;
    size_t avail = (cap > *pos + 1) ? (cap - 1 - *pos) : 0;
    size_t to_write = len > avail ? avail : len;
    if (to_write > 0) {
        memcpy(out + *pos, s, to_write);
        *pos += to_write;
    }
    return to_write == len;
}

static bool append_byte(char *out, size_t cap, size_t *pos, char c) {
    if (cap == 0 || *pos + 1 >= cap)
        return false;
    out[(*pos)++] = c;
    return true;
}

/* ── Pattern matchers ───────────────────────────────────────────────── */

/* Email: localpart "@" domain "." tld
 *   localpart: [A-Za-z0-9._%+-]+
 *   domain:    [A-Za-z0-9.-]+
 *   tld:       [A-Za-z]{2,}
 * Anchored at word boundary on both sides. */
static size_t match_email(const char *text, size_t text_len, size_t idx) {
    if (!at_word_boundary_start(text, text_len, idx))
        return 0;
    size_t i = idx;
    /* Localpart — must start with alnum to keep "..@..." style spam
     * out of the matcher. */
    if (i >= text_len)
        return 0;
    unsigned char c0 = (unsigned char)text[i];
    if (!isalnum(c0))
        return 0;
    while (i < text_len) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-')
            i++;
        else
            break;
    }
    if (i >= text_len || text[i] != '@' || i == idx)
        return 0;
    size_t at_pos = i++;
    (void)at_pos;
    /* Domain — at least one alnum, then any of [a-z0-9.-]. */
    size_t dom_start = i;
    while (i < text_len) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c) || c == '.' || c == '-')
            i++;
        else
            break;
    }
    if (i == dom_start)
        return 0;
    /* Need a "." somewhere in the domain followed by ≥2 alpha for the TLD. */
    const char *dot = NULL;
    for (size_t j = i - 1; j > dom_start; j--) {
        if (text[j] == '.') {
            dot = &text[j];
            break;
        }
    }
    if (!dot)
        return 0;
    size_t tld_start = (size_t)(dot - text) + 1;
    if (i - tld_start < 2)
        return 0;
    for (size_t j = tld_start; j < i; j++) {
        if (!isalpha((unsigned char)text[j]))
            return 0;
    }
    if (!at_word_boundary_end(text, text_len, i))
        return 0;
    return i - idx;
}

/* SSN: NNN-NN-NNNN — exact 11-byte form, word-anchored. */
static size_t match_ssn(const char *text, size_t text_len, size_t idx) {
    if (!at_word_boundary_start(text, text_len, idx))
        return 0;
    if (idx + 11 > text_len)
        return 0;
    const char *p = text + idx;
    for (int k = 0; k < 3; k++)
        if (!isdigit((unsigned char)p[k])) return 0;
    if (p[3] != '-') return 0;
    for (int k = 4; k < 6; k++)
        if (!isdigit((unsigned char)p[k])) return 0;
    if (p[6] != '-') return 0;
    for (int k = 7; k < 11; k++)
        if (!isdigit((unsigned char)p[k])) return 0;
    if (!at_word_boundary_end(text, text_len, idx + 11))
        return 0;
    return 11;
}

/* Credit card: 16 digits with optional ' ' or '-' separators after each
 * group of 4. Word-anchored. Examples accepted:
 *   1234567812345678
 *   1234-5678-1234-5678
 *   1234 5678 1234 5678
 * Not Luhn-checked — the goal is "looks like a CC", not validity. */
static size_t match_cc(const char *text, size_t text_len, size_t idx) {
    if (!at_word_boundary_start(text, text_len, idx))
        return 0;
    size_t i = idx;
    int digits = 0;
    char sep = '\0';
    while (i < text_len && digits < 16) {
        unsigned char c = (unsigned char)text[i];
        if (isdigit(c)) {
            i++;
            digits++;
            if (digits % 4 == 0 && digits < 16) {
                if (i < text_len && (text[i] == '-' || text[i] == ' ')) {
                    if (sep == '\0')
                        sep = text[i];
                    else if (text[i] != sep)
                        return 0;
                    i++;
                }
            }
        } else {
            return 0;
        }
    }
    if (digits != 16)
        return 0;
    if (!at_word_boundary_end(text, text_len, i))
        return 0;
    return i - idx;
}

/* IPv4 dotted-quad: each octet 1–3 digits with value 0–255. Anchored. */
static size_t match_ipv4(const char *text, size_t text_len, size_t idx) {
    if (!at_word_boundary_start(text, text_len, idx))
        return 0;
    size_t i = idx;
    for (int oct = 0; oct < 4; oct++) {
        size_t start = i;
        int val = 0;
        int len = 0;
        while (i < text_len && isdigit((unsigned char)text[i]) && len < 3) {
            val = val * 10 + (text[i] - '0');
            i++;
            len++;
        }
        if (len == 0 || val > 255)
            return 0;
        /* Reject leading zeros (e.g. "01.02.03.04") — too noisy a pattern. */
        if (len > 1 && text[start] == '0')
            return 0;
        if (oct < 3) {
            if (i >= text_len || text[i] != '.')
                return 0;
            i++;
        }
    }
    if (!at_word_boundary_end(text, text_len, i))
        return 0;
    return i - idx;
}

/* US-style phone: tolerates these forms (all 10 digits + optional country
 * code "1"):
 *   (NNN) NNN-NNNN
 *   NNN-NNN-NNNN
 *   NNN.NNN.NNNN
 *   +1 NNN NNN NNNN
 * To keep false positives low we require at least one separator and
 * exactly 10 digits in the body. */
static size_t match_phone(const char *text, size_t text_len, size_t idx) {
    if (!at_word_boundary_start(text, text_len, idx))
        return 0;
    size_t i = idx;
    bool has_sep = false;

    /* Optional +1 prefix. */
    if (i < text_len && text[i] == '+') {
        i++;
        if (i >= text_len || text[i] != '1')
            return 0;
        i++;
        if (i < text_len && (text[i] == ' ' || text[i] == '-')) {
            i++;
            has_sep = true;
        }
    }
    /* Optional opening paren. */
    bool have_paren = false;
    if (i < text_len && text[i] == '(') {
        have_paren = true;
        i++;
    }
    /* Three digits. */
    for (int k = 0; k < 3; k++) {
        if (i >= text_len || !isdigit((unsigned char)text[i]))
            return 0;
        i++;
    }
    if (have_paren) {
        if (i >= text_len || text[i] != ')')
            return 0;
        i++;
        has_sep = true;
        if (i < text_len && text[i] == ' ') i++;
    } else if (i < text_len && (text[i] == '-' || text[i] == '.' || text[i] == ' ')) {
        i++;
        has_sep = true;
    } else {
        return 0;
    }
    /* Three digits. */
    for (int k = 0; k < 3; k++) {
        if (i >= text_len || !isdigit((unsigned char)text[i]))
            return 0;
        i++;
    }
    if (i < text_len && (text[i] == '-' || text[i] == '.' || text[i] == ' ')) {
        i++;
        has_sep = true;
    } else {
        return 0;
    }
    /* Four digits. */
    for (int k = 0; k < 4; k++) {
        if (i >= text_len || !isdigit((unsigned char)text[i]))
            return 0;
        i++;
    }
    if (!has_sep)
        return 0;
    if (!at_word_boundary_end(text, text_len, i))
        return 0;
    return i - idx;
}

/* Secret key: keyword "api_key" / "apikey" / "token" / "secret" /
 * "password" / "pwd" followed by [:= ]+ then a long opaque string of
 * length ≥ 16 of [A-Za-z0-9_\-+/=].
 *
 * We anchor on the keyword (case-insensitive) so we don't redact every
 * 16-char alphanumeric blob in conversation. */
static const char *kSecretKeywords[] = {
    "api_key", "apikey", "api-key",
    "secret", "password", "passwd", "pwd",
    "token", "auth", "bearer",
};

static size_t ci_starts_with(const char *text, size_t text_len, size_t idx, const char *kw) {
    size_t klen = strlen(kw);
    if (idx + klen > text_len)
        return 0;
    for (size_t k = 0; k < klen; k++) {
        unsigned char a = (unsigned char)text[idx + k];
        unsigned char b = (unsigned char)kw[k];
        if (tolower(a) != tolower(b))
            return 0;
    }
    return klen;
}

static size_t match_secret(const char *text, size_t text_len, size_t idx) {
    if (!at_word_boundary_start(text, text_len, idx))
        return 0;
    size_t kw_len = 0;
    for (size_t k = 0; k < sizeof(kSecretKeywords) / sizeof(kSecretKeywords[0]); k++) {
        size_t hit = ci_starts_with(text, text_len, idx, kSecretKeywords[k]);
        if (hit > 0) {
            kw_len = hit;
            break;
        }
    }
    if (kw_len == 0)
        return 0;
    size_t i = idx + kw_len;
    /* Need a separator: ':', '=', ' ', or any of those mixed. */
    bool seen_sep = false;
    while (i < text_len) {
        char c = text[i];
        if (c == ':' || c == '=' || c == ' ' || c == '\t') {
            seen_sep = true;
            i++;
        } else {
            break;
        }
    }
    if (!seen_sep)
        return 0;
    /* Optional opening quote. */
    char quote = '\0';
    if (i < text_len && (text[i] == '"' || text[i] == '\'')) {
        quote = text[i];
        i++;
    }
    /* Opaque token: ≥ 16 chars from [A-Za-z0-9_\-+/=.]. */
    size_t tok_start = i;
    while (i < text_len) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c) || c == '_' || c == '-' || c == '+' || c == '/' || c == '=' || c == '.')
            i++;
        else
            break;
    }
    if (i - tok_start < 16)
        return 0;
    if (quote != '\0' && i < text_len && text[i] == quote)
        i++;
    /* Don't bother with end-of-word check — secrets are followed by
     * arbitrary content and we want to absorb the full opaque blob. */
    return i - idx;
}

/* ── Public API ─────────────────────────────────────────────────────── */

bool hu_pii_contains_pii(const char *text, size_t text_len) {
    if (!text || text_len == 0)
        return false;
    for (size_t i = 0; i < text_len;) {
        size_t m;
        m = match_email(text, text_len, i); if (m) return true;
        m = match_ssn(text, text_len, i);   if (m) return true;
        m = match_cc(text, text_len, i);    if (m) return true;
        m = match_ipv4(text, text_len, i);  if (m) return true;
        m = match_phone(text, text_len, i); if (m) return true;
        m = match_secret(text, text_len, i); if (m) return true;
        i++;
    }
    return false;
}

hu_error_t hu_pii_redact(const char *text, size_t text_len,
                         char *out, size_t out_cap, size_t *out_len,
                         hu_pii_stats_t *stats) {
    if (!text || !out || out_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_pii_stats_t local;
    memset(&local, 0, sizeof(local));

    size_t pos = 0;
    size_t i = 0;
    while (i < text_len) {
        size_t m;
        if ((m = match_email(text, text_len, i)) > 0) {
            local.emails++;
            (void)append_n(out, out_cap, &pos, "[EMAIL]", 7);
            i += m;
            continue;
        }
        if ((m = match_ssn(text, text_len, i)) > 0) {
            local.ssns++;
            (void)append_n(out, out_cap, &pos, "[SSN]", 5);
            i += m;
            continue;
        }
        /* Phone before CC: "555-555-5555" matches phone, "5555-5555-5555-5555"
         * matches CC. Phone is shorter so check first. */
        if ((m = match_phone(text, text_len, i)) > 0) {
            local.phones++;
            (void)append_n(out, out_cap, &pos, "[PHONE]", 7);
            i += m;
            continue;
        }
        if ((m = match_cc(text, text_len, i)) > 0) {
            local.credit_cards++;
            (void)append_n(out, out_cap, &pos, "[CC]", 4);
            i += m;
            continue;
        }
        if ((m = match_ipv4(text, text_len, i)) > 0) {
            local.ips++;
            (void)append_n(out, out_cap, &pos, "[IP]", 4);
            i += m;
            continue;
        }
        if ((m = match_secret(text, text_len, i)) > 0) {
            local.secrets++;
            (void)append_n(out, out_cap, &pos, "[SECRET]", 8);
            i += m;
            continue;
        }
        (void)append_byte(out, out_cap, &pos, text[i]);
        i++;
    }
    if (pos < out_cap)
        out[pos] = '\0';
    else
        out[out_cap - 1] = '\0';

    if (out_len)
        *out_len = pos;
    if (stats)
        *stats = local;
    return HU_OK;
}
