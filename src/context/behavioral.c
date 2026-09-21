/* Behavioral polish: F9 double-text, F12 bookends, F28 mirroring, F54 timezone */
#include "human/context/behavioral.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LCG_MUL 1103515245u
#define LCG_ADD 12345u

static const void *hu_memmem(const void *haystack, size_t haystacklen, const void *needle,
                             size_t needlelen) {
    if (needlelen == 0)
        return haystack;
    if (needlelen > haystacklen)
        return NULL;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    size_t limit = haystacklen - needlelen;
    for (size_t i = 0; i <= limit; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return h + i;
    }
    return NULL;
}

static uint32_t lcg_next(uint32_t *state) {
    *state = *state * LCG_MUL + LCG_ADD;
    return *state;
}

/* --- F9: Double-text --- */
static const hu_double_text_config_t hu_double_text_default = {
    .probability = 0.15,
    .min_gap_seconds = 30,
    .max_gap_seconds = 300,
    .only_close_friends = true,
};

bool hu_should_double_text(double closeness, uint64_t last_msg_ms, uint64_t now_ms,
                           const hu_double_text_config_t *config, uint32_t seed) {
    const hu_double_text_config_t *c = config ? config : &hu_double_text_default;

    if (c->only_close_friends && closeness < 0.6)
        return false;

    uint64_t gap_ms = (last_msg_ms <= now_ms) ? (now_ms - last_msg_ms) : 0;
    uint32_t gap_sec = (uint32_t)(gap_ms / 1000u);
    if (gap_sec < c->min_gap_seconds || gap_sec > c->max_gap_seconds)
        return false;

    uint32_t s = seed;
    double roll = (double)(lcg_next(&s) >> 16) / 65536.0;
    return roll < c->probability;
}

hu_error_t hu_double_text_build_prompt(hu_allocator_t *alloc, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    const char *msg =
        "[DOUBLE-TEXT OK] You just sent a message. You can add a follow-up thought if natural.";
    char *dup = hu_strndup(alloc, msg, strlen(msg));
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out = dup;
    *out_len = strlen(msg);
    return HU_OK;
}

/* --- F12: Bookends --- */
hu_bookend_type_t hu_bookend_check(uint32_t hour, bool contact_is_close, bool already_sent_today,
                                   uint32_t seed) {
    (void)seed;
    if (!contact_is_close || already_sent_today)
        return HU_BOOKEND_NONE;

    if (hour >= 6 && hour <= 9)
        return HU_BOOKEND_MORNING;
    if (hour >= 21 && hour < 23)
        return HU_BOOKEND_EVENING;
    if (hour >= 23 || hour < 6)
        return HU_BOOKEND_GOODNIGHT;

    return HU_BOOKEND_NONE;
}

const char *hu_bookend_type_str(hu_bookend_type_t t) {
    switch (t) {
    case HU_BOOKEND_NONE:
        return "none";
    case HU_BOOKEND_MORNING:
        return "morning";
    case HU_BOOKEND_EVENING:
        return "evening";
    case HU_BOOKEND_GOODNIGHT:
        return "goodnight";
    default:
        return "none";
    }
}

hu_error_t hu_bookend_build_prompt(hu_allocator_t *alloc, hu_bookend_type_t type, char **out,
                                   size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *msg;
    switch (type) {
    case HU_BOOKEND_MORNING:
        msg = "[BOOKEND: morning] Start the day with a casual greeting.";
        break;
    case HU_BOOKEND_EVENING:
        msg = "[BOOKEND: evening] Wind down with a brief check-in.";
        break;
    case HU_BOOKEND_GOODNIGHT:
        msg = "[BOOKEND: goodnight] Say goodnight if appropriate.";
        break;
    default:
        return HU_ERR_INVALID_ARGUMENT;
    }

    char *dup = hu_strndup(alloc, msg, strlen(msg));
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out = dup;
    *out_len = strlen(msg);
    return HU_OK;
}

/* --- F28: Linguistic Mirroring --- */
static bool is_emoji_char(unsigned char c) {
    return c > 127;
}

static bool contains_abbrev(const char *msg, size_t len) {
    if (len < 2)
        return false;
    /* "u " " ur " " rn" */
    for (size_t i = 0; i + 2 <= len; i++) {
        if (msg[i] == 'u' && (i + 1 >= len || msg[i + 1] == ' ' || msg[i + 1] == '\0'))
            return true;
        if (i + 3 <= len && msg[i] == ' ' && msg[i + 1] == 'u' && msg[i + 2] == 'r')
            return true;
        if (i + 3 <= len && msg[i] == ' ' && msg[i + 1] == 'r' && msg[i + 2] == 'n')
            return true;
        if (i + 2 <= len && msg[i] == 'u' && msg[i + 1] == 'r')
            return true;
        if (i + 2 <= len && msg[i] == 'r' && msg[i + 1] == 'n')
            return true;
    }
    return false;
}

static bool is_all_lowercase(const char *msg, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (isalpha((unsigned char)msg[i]) && !islower((unsigned char)msg[i]))
            return false;
    }
    return true;
}

static size_t count_char(const char *msg, size_t len, char ch) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        if (msg[i] == ch)
            n++;
    return n;
}

static bool has_emoji(const char *msg, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (is_emoji_char((unsigned char)msg[i]))
            return true;
    }
    return false;
}

hu_error_t hu_mirror_analyze(const char *const *messages, const size_t *msg_lens, size_t count,
                             hu_mirror_analysis_t *out) {
    if (!messages || !msg_lens || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (count == 0)
        return HU_OK;

    size_t lowercase_count = 0;
    size_t abbrev_count = 0;
    size_t exclaim_count = 0;
    size_t ellipsis_count = 0;
    size_t emoji_count = 0;
    uint64_t total_len = 0;

    for (size_t i = 0; i < count; i++) {
        const char *m = messages[i];
        size_t len = msg_lens[i];
        if (!m)
            continue;

        total_len += len;
        if (len > 0 && is_all_lowercase(m, len))
            lowercase_count++;
        if (contains_abbrev(m, len))
            abbrev_count++;
        if (count_char(m, len, '!') >= 2)
            exclaim_count++;
        if (len >= 3) {
            for (size_t j = 0; j + 3 <= len; j++) {
                if (m[j] == '.' && m[j + 1] == '.' && m[j + 2] == '.') {
                    ellipsis_count++;
                    break;
                }
            }
        }
        if (has_emoji(m, len))
            emoji_count++;
    }

    out->uses_lowercase = (lowercase_count * 2 >= count);
    out->uses_abbreviations = (abbrev_count > 0);
    out->uses_exclamation = (exclaim_count * 2 >= count);
    out->uses_ellipsis = (ellipsis_count * 2 >= count);
    out->uses_emoji = (emoji_count * 2 >= count);
    out->avg_msg_length = (count > 0) ? (double)total_len / (double)count : 0.0;

    return HU_OK;
}

hu_error_t hu_mirror_build_directive(hu_allocator_t *alloc, const hu_mirror_analysis_t *analysis,
                                     char **out, size_t *out_len) {
    if (!alloc || !analysis || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[512];
    size_t pos = 0;
    const char *prefix = "[LINGUISTIC MIRROR]: ";
    size_t prefix_len = strlen(prefix);
    if (pos + prefix_len < sizeof(buf)) {
        memcpy(buf + pos, prefix, prefix_len + 1);
        pos += prefix_len;
    }

    int first = 1;
    if (analysis->uses_lowercase) {
        if (!first)
            pos = hu_buf_appendf(buf, sizeof(buf), pos, " ");
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "They type in lowercase — match their style.");
        first = 0;
    }
    if (analysis->uses_abbreviations) {
        if (!first)
            pos = hu_buf_appendf(buf, sizeof(buf), pos, " ");
        pos = hu_buf_appendf(buf, sizeof(buf), pos,
                             "They use abbreviations — ok to use 'u' and 'rn'.");
        first = 0;
    }
    if (analysis->avg_msg_length > 0.0) {
        int n = (int)(analysis->avg_msg_length + 0.5);
        if (n < 1)
            n = 1;
        if (!first)
            pos = hu_buf_appendf(buf, sizeof(buf), pos, " ");
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "Keep messages around %d chars.", n);
        first = 0;
    }
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    if (pos == prefix_len) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    char *dup = hu_strndup(alloc, buf, pos);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out = dup;
    *out_len = pos;
    return HU_OK;
}

/* --- F28b: Mirror identity bounds --- */

/* Check if a directive sentence mentions any avoided vocab word.
 * A sentence like "ok to use 'u' and 'rn'" conflicts if "u" or "rn" is avoided. */
static bool sentence_conflicts_with_avoided(const char *sentence, size_t len,
                                            const char *const *avoided, size_t avoided_count) {
    if (!avoided || avoided_count == 0)
        return false;
    /* Check each avoided word against the sentence */
    for (size_t i = 0; i < avoided_count; i++) {
        if (!avoided[i])
            continue;
        size_t wlen = strlen(avoided[i]);
        if (wlen == 0)
            continue;
        /* Search for the avoided word in the sentence */
        for (size_t j = 0; j + wlen <= len; j++) {
            if (strncasecmp(sentence + j, avoided[i], wlen) == 0) {
                /* Check word boundaries */
                bool left_ok = (j == 0 || !isalnum((unsigned char)sentence[j - 1]));
                bool right_ok = (j + wlen >= len || !isalnum((unsigned char)sentence[j + wlen]));
                if (left_ok && right_ok)
                    return true;
            }
        }
    }
    return false;
}

/* Check if a directive sentence conflicts with character invariants.
 * Invariants like "never uses abbreviations" should block abbreviation mirroring. */
static bool sentence_conflicts_with_invariants(const char *sentence, size_t len,
                                               const char *const *invariants,
                                               size_t invariant_count) {
    if (!invariants || invariant_count == 0)
        return false;

    /* Map: if sentence mentions "abbreviations" and an invariant says "never...abbreviation" */
    bool sentence_has_abbrev = (len >= 6 && (hu_memmem(sentence, len, "abbrev", 6) != NULL ||
                                             hu_memmem(sentence, len, "Abbrev", 6) != NULL));
    bool sentence_has_lowercase = (len >= 9 && (hu_memmem(sentence, len, "lowercase", 9) != NULL ||
                                                hu_memmem(sentence, len, "Lowercase", 9) != NULL));

    for (size_t i = 0; i < invariant_count; i++) {
        if (!invariants[i])
            continue;
        size_t ilen = strlen(invariants[i]);
        bool inv_has_never = (hu_memmem(invariants[i], ilen, "never", 5) != NULL ||
                              hu_memmem(invariants[i], ilen, "Never", 5) != NULL);
        bool inv_has_no = (hu_memmem(invariants[i], ilen, "no ", 3) != NULL ||
                           hu_memmem(invariants[i], ilen, "No ", 3) != NULL);
        if (!inv_has_never && !inv_has_no)
            continue;

        if (sentence_has_abbrev && hu_memmem(invariants[i], ilen, "abbrev", 6) != NULL)
            return true;
        if (sentence_has_lowercase && hu_memmem(invariants[i], ilen, "lowercase", 9) != NULL)
            return true;
    }
    return false;
}

hu_error_t hu_mirror_check_identity_bounds(hu_allocator_t *alloc, const char *directive,
                                           size_t directive_len, const struct hu_persona *persona,
                                           char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    /* No directive or no persona — pass through */
    if (!directive || directive_len == 0) {
        return HU_OK;
    }
    if (!persona) {
        char *dup = hu_strndup(alloc, directive, directive_len);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        *out = dup;
        *out_len = directive_len;
        return HU_OK;
    }

    /* Build filtered output: process sentence by sentence (split on ". ") */
    char buf[512];
    size_t pos = 0;

    /* Copy the prefix "[LINGUISTIC MIRROR]: " if present */
    const char *prefix = "[LINGUISTIC MIRROR]: ";
    size_t prefix_len = strlen(prefix);
    const char *body = directive;
    size_t body_len = directive_len;

    if (directive_len >= prefix_len && memcmp(directive, prefix, prefix_len) == 0) {
        memcpy(buf, prefix, prefix_len);
        pos = prefix_len;
        body = directive + prefix_len;
        body_len = directive_len - prefix_len;
    }

    /* Walk sentences separated by ". " or end of string */
    const char *p = body;
    const char *end = body + body_len;
    bool first = true;

    while (p < end) {
        /* Find end of current sentence */
        const char *dot = (const char *)hu_memmem(p, (size_t)(end - p), ". ", 2);
        size_t slen;
        const char *next;
        if (dot) {
            slen = (size_t)(dot - p) + 1; /* include the '.' */
            next = dot + 2;               /* skip ". " */
        } else {
            slen = (size_t)(end - p);
            next = end;
        }

        /* Check if this sentence conflicts with identity */
        bool conflicts = false;

        if (persona->avoided_vocab_count > 0) {
            conflicts = sentence_conflicts_with_avoided(
                p, slen, (const char *const *)persona->avoided_vocab, persona->avoided_vocab_count);
        }

        if (!conflicts && persona->character_invariants_count > 0) {
            conflicts = sentence_conflicts_with_invariants(
                p, slen, (const char *const *)persona->character_invariants,
                persona->character_invariants_count);
        }

        if (!conflicts) {
            if (!first && pos < sizeof(buf) - 1) {
                buf[pos++] = ' ';
            }
            size_t copy = slen;
            if (pos + copy >= sizeof(buf))
                copy = sizeof(buf) - pos - 1;
            memcpy(buf + pos, p, copy);
            pos += copy;
            first = false;
        }

        p = next;
    }

    /* If everything was filtered, return NULL */
    if (pos == prefix_len || pos == 0) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    buf[pos] = '\0';
    char *dup = hu_strndup(alloc, buf, pos);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out = dup;
    *out_len = pos;
    return HU_OK;
}

/* --- F28c: Identity conflict check (boolean) --- */

bool hu_mirror_conflicts_with_identity(const hu_mirror_analysis_t *analysis,
                                       const struct hu_persona *persona) {
    if (!analysis || !persona)
        return false;

    const hu_persona_t *p = (const hu_persona_t *)persona;

    /* Check abbreviations against avoided vocab and character invariants */
    if (analysis->uses_abbreviations) {
        /* Check avoided vocab for abbreviation-related words */
        for (size_t i = 0; i < p->avoided_vocab_count; i++) {
            if (!p->avoided_vocab[i])
                continue;
            /* If any avoided word is "u", "ur", "rn", abbreviations conflict */
            if (strcmp(p->avoided_vocab[i], "u") == 0 || strcmp(p->avoided_vocab[i], "ur") == 0 ||
                strcmp(p->avoided_vocab[i], "rn") == 0)
                return true;
        }
        /* Check character invariants for "never...abbreviation" patterns */
        for (size_t i = 0; i < p->character_invariants_count; i++) {
            if (!p->character_invariants[i])
                continue;
            size_t ilen = strlen(p->character_invariants[i]);
            bool has_never = (hu_memmem(p->character_invariants[i], ilen, "never", 5) != NULL ||
                              hu_memmem(p->character_invariants[i], ilen, "Never", 5) != NULL);
            bool has_no = (hu_memmem(p->character_invariants[i], ilen, "no ", 3) != NULL ||
                           hu_memmem(p->character_invariants[i], ilen, "No ", 3) != NULL);
            if ((has_never || has_no) &&
                hu_memmem(p->character_invariants[i], ilen, "abbrev", 6) != NULL)
                return true;
        }
    }

    /* Check lowercase against character invariants */
    if (analysis->uses_lowercase) {
        for (size_t i = 0; i < p->character_invariants_count; i++) {
            if (!p->character_invariants[i])
                continue;
            size_t ilen = strlen(p->character_invariants[i]);
            bool has_never = (hu_memmem(p->character_invariants[i], ilen, "never", 5) != NULL ||
                              hu_memmem(p->character_invariants[i], ilen, "Never", 5) != NULL);
            bool has_no = (hu_memmem(p->character_invariants[i], ilen, "no ", 3) != NULL ||
                           hu_memmem(p->character_invariants[i], ilen, "No ", 3) != NULL);
            if ((has_never || has_no) &&
                hu_memmem(p->character_invariants[i], ilen, "lowercase", 9) != NULL)
                return true;
        }
    }

    return false;
}

/* --- F28d: Combined build+filter --- */

hu_error_t hu_mirror_build_directive_persona(hu_allocator_t *alloc,
                                             const hu_mirror_analysis_t *analysis,
                                             const struct hu_persona *persona, char **out,
                                             size_t *out_len) {
    if (!alloc || !analysis || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    /* Build the raw directive */
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = hu_mirror_build_directive(alloc, analysis, &raw, &raw_len);
    if (err != HU_OK)
        return err;

    /* No directive generated — nothing to filter */
    if (!raw || raw_len == 0)
        return HU_OK;

    /* No persona — return raw */
    if (!persona) {
        *out = raw;
        *out_len = raw_len;
        return HU_OK;
    }

    /* Filter through identity bounds */
    err = hu_mirror_check_identity_bounds(alloc, raw, raw_len, persona, out, out_len);
    alloc->free(alloc->ctx, raw, raw_len + 1);
    return err;
}

/* --- F54: Timezone --- */
hu_timezone_info_t hu_timezone_compute(int offset_hours, uint64_t utc_now_ms) {
    hu_timezone_info_t tz = {
        .offset_hours = offset_hours,
        .local_hour = 0,
        .is_sleeping_hours = false,
        .is_work_hours = false,
    };

    uint64_t sec = utc_now_ms / 1000u;
    uint32_t utc_hour = (uint32_t)((sec / 3600u) % 24u);
    int local = (int)utc_hour + offset_hours;
    while (local < 0)
        local += 24;
    while (local >= 24)
        local -= 24;
    tz.local_hour = (uint32_t)local;
    tz.is_sleeping_hours = (tz.local_hour >= 23 || tz.local_hour < 7);
    tz.is_work_hours = (tz.local_hour >= 9 && tz.local_hour <= 17);

    return tz;
}

hu_error_t hu_timezone_build_directive(hu_allocator_t *alloc, const hu_timezone_info_t *tz,
                                       const char *contact_name, size_t name_len, char **out,
                                       size_t *out_len) {
    if (!alloc || !tz || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    if (!tz->is_sleeping_hours)
        return HU_OK;

    const char *name = (contact_name && name_len > 0) ? contact_name : "contact";
    size_t nlen = (contact_name && name_len > 0) ? name_len : 7;

    uint32_t h12 = tz->local_hour % 12;
    if (h12 == 0)
        h12 = 12;
    const char *ampm = (tz->local_hour < 12) ? "am" : "pm";

    char *dup = hu_sprintf(alloc,
                           "[TIMEZONE: %.*s is likely sleeping (%u%s their time). "
                           "Do not send proactive messages.]",
                           (int)nlen, name, (unsigned)h12, ampm);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;

    *out = dup;
    *out_len = strlen(dup);
    return HU_OK;
}
