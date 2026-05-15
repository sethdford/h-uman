/*
 * Sprint 6 US-19: Post-generation case/punctuation mirroring.
 *
 * Enforces partner style on Seth's outbound text AFTER generation and BEFORE
 * channel send, making mirroring deterministic rather than advisory.
 */

#include "human/persona/style_mirror.h"
#include <ctype.h>
#include <string.h>

/* Return true if c is an ASCII lowercase letter */
static bool is_lower_alpha(char c) {
    return c >= 'a' && c <= 'z';
}

/* Return true if c is an ASCII uppercase letter */
static bool is_upper_alpha(char c) {
    return c >= 'A' && c <= 'Z';
}

/* Lowercase a single ASCII uppercase letter in-place. */
static void lowercase_char(char *c) {
    if (is_upper_alpha(*c))
        *c = (char)(*c + 32);
}

/*
 * Count the length of the word starting at buf[pos] (up to buf_len).
 * A word ends at whitespace, '.', '?', '!', or NUL.
 */
static size_t word_len_at(const char *buf, size_t buf_len, size_t pos) {
    size_t len = 0;
    while (pos + len < buf_len) {
        char c = buf[pos + len];
        if (c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '.' || c == '?' || c == '!')
            break;
        len++;
    }
    return len;
}

/*
 * Apply sentence-start lowercasing to buf.
 * Only lowercases the first letter of a sentence-start word if that word
 * is 1-3 characters long, which conservatively avoids proper nouns (4+ chars).
 *
 * Sentence starts: position 0, or one character after '. ', '? ', '! '
 * followed immediately by an uppercase letter.
 */
static size_t apply_lowercase_sentence_starts(char *buf, size_t len) {
    size_t edits = 0;

    /* Position 0 */
    if (len > 0 && is_upper_alpha(buf[0])) {
        size_t wlen = word_len_at(buf, len, 0);
        if (wlen >= 1 && wlen <= 3) {
            lowercase_char(&buf[0]);
            edits++;
        }
    }

    /* After ". ", "? ", "! " */
    for (size_t i = 1; i + 1 < len; i++) {
        char prev = buf[i - 1];
        char sep = buf[i];
        char next = buf[i + 1];
        if ((prev == '.' || prev == '?' || prev == '!') && sep == ' ' && is_upper_alpha(next)) {
            size_t wlen = word_len_at(buf, len, i + 1);
            if (wlen >= 1 && wlen <= 3) {
                lowercase_char(&buf[i + 1]);
                edits++;
            }
        }
    }

    return edits;
}

/*
 * Strip a single trailing '.' from buf (not '?' or '!').
 * Ignores trailing whitespace when scanning for the period.
 * Returns 1 if a period was stripped, 0 otherwise.
 */
static int strip_trailing_period(char *buf, size_t *len) {
    if (*len == 0)
        return 0;

    /* Find last non-whitespace character */
    size_t end = *len;
    while (end > 0 && (buf[end - 1] == ' ' || buf[end - 1] == '\t' || buf[end - 1] == '\n'))
        end--;

    if (end > 0 && buf[end - 1] == '.') {
        buf[end - 1] = '\0';
        *len = end - 1;
        return 1;
    }
    return 0;
}

hu_error_t hu_style_mirror_apply(char *buf, size_t *inout_len, const char *const *partner_recent,
                                 size_t n_partner_recent, hu_style_mirror_report_t *report) {
    if (!buf || !inout_len)
        return HU_ERR_INVALID_ARGUMENT;

    if (report) {
        report->lowercased_applied = false;
        report->periods_stripped = false;
        report->edits = 0;
    }

    /* Need at least 2 partner messages to establish a pattern */
    if (!partner_recent || n_partner_recent < 2)
        return HU_OK;

    /* Compute lowercase_ratio and no_period_ratio */
    size_t lowercase_count = 0;
    size_t no_period_count = 0;

    for (size_t i = 0; i < n_partner_recent; i++) {
        const char *m = partner_recent[i];
        if (!m)
            continue;

        /* Does it start with a lowercase letter? */
        if (is_lower_alpha(m[0]))
            lowercase_count++;

        /* Does it NOT end with '.' (after trimming whitespace)? */
        size_t mlen = strlen(m);
        size_t end = mlen;
        while (end > 0 && (m[end - 1] == ' ' || m[end - 1] == '\t' || m[end - 1] == '\n'))
            end--;
        if (end == 0 || m[end - 1] != '.')
            no_period_count++;
    }

    float lowercase_ratio = (float)lowercase_count / (float)n_partner_recent;
    float no_period_ratio = (float)no_period_count / (float)n_partner_recent;

    size_t len = *inout_len;

    /* Apply sentence-start lowercasing when >= 70% of partner messages start lowercase */
    if (lowercase_ratio >= 0.7f) {
        size_t edits = apply_lowercase_sentence_starts(buf, len);
        if (report && edits > 0) {
            report->lowercased_applied = true;
            report->edits += edits;
        }
    }

    /* Strip trailing period when >= 70% of partner messages skip end-of-sentence periods */
    if (no_period_ratio >= 0.7f) {
        if (strip_trailing_period(buf, &len)) {
            *inout_len = len;
            if (report) {
                report->periods_stripped = true;
                report->edits++;
            }
        }
    }

    return HU_OK;
}
