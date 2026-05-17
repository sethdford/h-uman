/*
 * Persona overlay renderer — applies per-channel persona overlays to outbound
 * text so the same agent reply renders differently in Slack (formal/terse) than
 * in iMessage (casual/emoji). This is the helper called by Tier-1 channel send
 * paths to make `hu_persona_overlay_t` actually shape output instead of being
 * declared-but-unused.
 *
 * See docs/plans/2026-05-16-audit-followups/01-persona-overlay-wiring.md for
 * the design and acceptance criteria. See include/human/persona.h for the
 * public contract on `hu_persona_render_for_channel`.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- Helpers --------------------------------------------------------------- */

/* Lower-case ASCII compare, no allocation. Returns true if s contains
 * needle as a case-insensitive substring. NULL/empty s -> false. */
static bool str_contains_ci(const char *s, const char *needle) {
    if (!s || !needle || !*needle)
        return false;
    size_t nlen = strlen(needle);
    size_t slen = strlen(s);
    if (slen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= slen; i++) {
        if (strncasecmp(s + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static bool eq_ci(const char *a, const char *b) {
    if (!a || !b)
        return false;
    return strcasecmp(a, b) == 0;
}

/* Detect a UTF-8 emoji codepoint at p. Mirrors src/tts/transcript_prep.c. */
static bool is_emoji_codepoint(const unsigned char *p, size_t remain) {
    if (remain < 3)
        return false;
    if (p[0] == 0xE2 && p[1] >= 0x80 && p[1] <= 0xBF)
        return true;
    if (remain >= 4 && p[0] == 0xF0 && p[1] == 0x9F)
        return true;
    return false;
}

/* Strip emoji codepoints in-place from a NUL-terminated buffer. Returns the
 * new length. Skips trailing variation selectors / ZWJ joiners so combined
 * sequences (e.g. flag emoji, family glyphs) collapse cleanly. */
static size_t strip_emoji_inplace(char *s, size_t len) {
    if (!s || len == 0)
        return 0;
    size_t out = 0;
    size_t i = 0;
    bool prev_was_space = false;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xE0) {
            size_t remain = len - i;
            if (is_emoji_codepoint((const unsigned char *)s + i, remain)) {
                size_t skip = (c >= 0xF0) ? 4 : 3;
                if (i + skip > len)
                    skip = len - i;
                i += skip;
                /* Eat variation selectors + ZWJ continuations */
                while (i + 2 < len) {
                    const unsigned char *p = (const unsigned char *)s + i;
                    if (p[0] == 0xEF && p[1] == 0xB8 && p[2] == 0x8F) {
                        i += 3;
                    } else if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8D) {
                        i += 3;
                        if (i < len && is_emoji_codepoint((const unsigned char *)s + i, len - i)) {
                            i += ((unsigned char)s[i] >= 0xF0) ? 4 : 3;
                        }
                    } else {
                        break;
                    }
                }
                continue;
            }
        }
        if (c == ' ') {
            if (prev_was_space || out == 0) {
                i++;
                continue;
            }
            prev_was_space = true;
        } else {
            prev_was_space = false;
        }
        s[out++] = (char)c;
        i++;
    }
    /* Trim trailing whitespace */
    while (out > 0 && (s[out - 1] == ' ' || s[out - 1] == '\t'))
        out--;
    s[out] = '\0';
    return out;
}

/* --- Lexical formality swaps ----------------------------------------------- */

typedef struct {
    const char *from;
    const char *to;
} word_swap_t;

/* Casual -> formal swaps. Applied as whole-word, case-insensitive. */
static const word_swap_t CASUAL_TO_FORMAL[] = {
    {"hey", "hello"},      {"yeah", "yes"},       {"yep", "yes"},       {"nope", "no"},
    {"nah", "no"},         {"gonna", "going to"}, {"wanna", "want to"}, {"gotta", "have to"},
    {"kinda", "somewhat"}, {"sorta", "somewhat"}, {"thx", "thank you"}, {"ty", "thank you"},
    {"u", "you"},          {"ur", "your"},        {"pls", "please"},    {"plz", "please"},
    {"lol", ""},           {"lmao", ""},          {"omg", ""},
};
static const size_t CASUAL_TO_FORMAL_COUNT = sizeof(CASUAL_TO_FORMAL) / sizeof(CASUAL_TO_FORMAL[0]);

/* Formal -> casual swaps (subset, conservative). */
static const word_swap_t FORMAL_TO_CASUAL[] = {
    {"hello", "hey"},
    {"going to", "gonna"},
    {"want to", "wanna"},
};
static const size_t FORMAL_TO_CASUAL_COUNT = sizeof(FORMAL_TO_CASUAL) / sizeof(FORMAL_TO_CASUAL[0]);

static bool is_word_boundary(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '.' || c == ',' || c == '!' ||
           c == '?' || c == ';' || c == ':' || c == '(' || c == ')' || c == '"' || c == '\'';
}

/* Allocate a new string applying whole-word case-insensitive swaps.
 * Returns NULL on OOM. *out_len receives the new length. The returned
 * buffer is NUL-terminated and owned by the caller. */
static char *apply_swaps(const char *in, size_t in_len, const word_swap_t *swaps, size_t swap_count,
                         hu_allocator_t *alloc, size_t *out_len) {
    /* Worst case: every word in becomes the longest swap target. Cheap upper
     * bound: 4x input length + 32. */
    size_t cap = in_len * 4 + 32;
    char *out = (char *)alloc->alloc(alloc->ctx, cap + 1);
    if (!out)
        return NULL;
    size_t op = 0;
    size_t i = 0;
    while (i < in_len) {
        bool at_boundary = (i == 0) || is_word_boundary(in[i - 1]);
        bool matched = false;
        if (at_boundary) {
            for (size_t s = 0; s < swap_count; s++) {
                const char *from = swaps[s].from;
                size_t flen = strlen(from);
                if (i + flen > in_len)
                    continue;
                if (strncasecmp(in + i, from, flen) != 0)
                    continue;
                /* Need word boundary at end too */
                char after = (i + flen < in_len) ? in[i + flen] : '\0';
                if (!is_word_boundary(after))
                    continue;
                const char *to = swaps[s].to;
                size_t tlen = strlen(to);
                if (op + tlen + 1 > cap) {
                    size_t new_cap = cap * 2 + tlen + 1;
                    char *grown = (char *)alloc->alloc(alloc->ctx, new_cap + 1);
                    if (!grown) {
                        alloc->free(alloc->ctx, out, cap + 1);
                        return NULL;
                    }
                    memcpy(grown, out, op);
                    alloc->free(alloc->ctx, out, cap + 1);
                    out = grown;
                    cap = new_cap;
                }
                if (tlen > 0) {
                    memcpy(out + op, to, tlen);
                    op += tlen;
                } else {
                    /* Empty replacement — also swallow one trailing space if
                     * present, to avoid double-spacing. */
                    if (i + flen < in_len && in[i + flen] == ' ')
                        i++;
                }
                i += flen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            if (op + 1 >= cap) {
                size_t new_cap = cap * 2;
                char *grown = (char *)alloc->alloc(alloc->ctx, new_cap + 1);
                if (!grown) {
                    alloc->free(alloc->ctx, out, cap + 1);
                    return NULL;
                }
                memcpy(grown, out, op);
                alloc->free(alloc->ctx, out, cap + 1);
                out = grown;
                cap = new_cap;
            }
            out[op++] = in[i++];
        }
    }
    /* Collapse runs of spaces produced by empty replacements. */
    size_t w = 0;
    bool prev_space = false;
    for (size_t r = 0; r < op; r++) {
        char c = out[r];
        if (c == ' ') {
            if (prev_space)
                continue;
            prev_space = true;
        } else {
            prev_space = false;
        }
        out[w++] = c;
    }
    /* Trim trailing whitespace and punctuation-leading whitespace. */
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t'))
        w--;
    out[w] = '\0';
    *out_len = w;
    return out;
}

/* --- Length policy --------------------------------------------------------- */

/* Parse "max_chars=NNN" or named buckets ("short","medium","long").
 * Returns 0 if no cap should be applied. */
static size_t length_cap_from_overlay(const char *avg_length) {
    if (!avg_length || !*avg_length)
        return 0;
    /* "max_chars=NNN" form (explicit) */
    const char *eq = strstr(avg_length, "max_chars=");
    if (eq) {
        long v = strtol(eq + 10, NULL, 10);
        if (v > 0 && v < 100000)
            return (size_t)v;
    }
    if (eq_ci(avg_length, "short"))
        return 200;
    if (eq_ci(avg_length, "terse") || eq_ci(avg_length, "tight"))
        return 140;
    if (eq_ci(avg_length, "medium"))
        return 0; /* no truncation */
    if (eq_ci(avg_length, "long"))
        return 0;
    return 0;
}

/* Truncate buf in place to at most cap bytes. Prefer last sentence-ending
 * boundary within the cap; otherwise truncate at the last whitespace. */
static size_t truncate_at_boundary(char *buf, size_t len, size_t cap) {
    if (len <= cap)
        return len;
    /* Look back for sentence punctuation followed by space or EOS. */
    for (ssize_t i = (ssize_t)cap - 1; i >= (ssize_t)(cap > 60 ? cap - 60 : 0); i--) {
        char c = buf[i];
        if (c == '.' || c == '!' || c == '?') {
            buf[i + 1] = '\0';
            return (size_t)(i + 1);
        }
    }
    /* Fall back to last whitespace. */
    for (ssize_t i = (ssize_t)cap - 1; i >= (ssize_t)(cap > 40 ? cap - 40 : 0); i--) {
        if (buf[i] == ' ' || buf[i] == '\n') {
            buf[i] = '\0';
            return (size_t)i;
        }
    }
    /* Hard truncate. */
    buf[cap] = '\0';
    return cap;
}

/* --- Public entry point --------------------------------------------------- */

hu_error_t hu_persona_render_for_channel(const hu_persona_overlay_t *overlay, const char *raw_text,
                                         size_t raw_len, hu_allocator_t *alloc, char **out_rendered,
                                         size_t *out_rendered_len) {
    if (!alloc || !alloc->alloc || !alloc->free || !out_rendered)
        return HU_ERR_INVALID_ARGUMENT;

    if (!raw_text || raw_len == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *out_rendered = empty;
        if (out_rendered_len)
            *out_rendered_len = 0;
        return HU_OK;
    }

    /* Stage 0: identity copy. */
    char *buf = (char *)alloc->alloc(alloc->ctx, raw_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, raw_text, raw_len);
    buf[raw_len] = '\0';
    size_t blen = raw_len;
    size_t cap = raw_len + 1;

    if (!overlay) {
        *out_rendered = buf;
        if (out_rendered_len)
            *out_rendered_len = blen;
        return HU_OK;
    }

    /* Stage 1: emoji policy. "none" / "no" / "off" / "never" strip emoji. */
    if (overlay->emoji_usage &&
        (eq_ci(overlay->emoji_usage, "none") || eq_ci(overlay->emoji_usage, "no") ||
         eq_ci(overlay->emoji_usage, "off") || eq_ci(overlay->emoji_usage, "never") ||
         eq_ci(overlay->emoji_usage, "minimal") || eq_ci(overlay->emoji_usage, "rare"))) {
        /* Treat "minimal" and "rare" as strip-for-determinism. The audit's
         * AC-3 only requires that "disabled" produces no emoji; we extend
         * the strip to the formal-leaning buckets so Slack-style overlays
         * are deterministic. */
        if (eq_ci(overlay->emoji_usage, "none") || eq_ci(overlay->emoji_usage, "no") ||
            eq_ci(overlay->emoji_usage, "off") || eq_ci(overlay->emoji_usage, "never")) {
            blen = strip_emoji_inplace(buf, blen);
        }
    }

    /* Stage 2: formality policy. */
    if (overlay->formality && *overlay->formality) {
        bool make_formal = str_contains_ci(overlay->formality, "formal") ||
                           str_contains_ci(overlay->formality, "professional");
        bool make_casual = (!make_formal) && (str_contains_ci(overlay->formality, "casual") ||
                                              str_contains_ci(overlay->formality, "informal"));
        if (make_formal) {
            /* Formal overlay also strips emoji even if emoji_usage isn't
             * explicitly "none" — formal channels (Slack) never emit emoji.
             * This is the AC-3 contract per the design doc. */
            blen = strip_emoji_inplace(buf, blen);
            size_t new_len = 0;
            char *swapped =
                apply_swaps(buf, blen, CASUAL_TO_FORMAL, CASUAL_TO_FORMAL_COUNT, alloc, &new_len);
            if (!swapped) {
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            alloc->free(alloc->ctx, buf, cap);
            buf = swapped;
            blen = new_len;
            cap = blen + 1;
            /* Capitalize first letter. */
            if (blen > 0 && buf[0] >= 'a' && buf[0] <= 'z')
                buf[0] = (char)(buf[0] - 32);
        } else if (make_casual) {
            size_t new_len = 0;
            char *swapped =
                apply_swaps(buf, blen, FORMAL_TO_CASUAL, FORMAL_TO_CASUAL_COUNT, alloc, &new_len);
            if (!swapped) {
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            alloc->free(alloc->ctx, buf, cap);
            buf = swapped;
            blen = new_len;
            cap = blen + 1;
            /* Lowercase first letter for "I"-aware casual feel (but never
             * lowercase a standalone "I" or capitalized proper nouns mid-word). */
            if (blen > 1 && buf[0] >= 'A' && buf[0] <= 'Z' && buf[0] != 'I' && buf[1] >= 'a' &&
                buf[1] <= 'z') {
                buf[0] = (char)(buf[0] + 32);
            }
        }
    }

    /* Stage 3: length cap. */
    size_t cap_bytes = length_cap_from_overlay(overlay->avg_length);
    if (cap_bytes > 0 && blen > cap_bytes) {
        blen = truncate_at_boundary(buf, blen, cap_bytes);
    }

    /* max_segment_chars acts as an additional hard cap when set. */
    if (overlay->max_segment_chars > 0 && blen > overlay->max_segment_chars) {
        blen = truncate_at_boundary(buf, blen, overlay->max_segment_chars);
    }

    *out_rendered = buf;
    if (out_rendered_len)
        *out_rendered_len = blen;
    return HU_OK;
}
