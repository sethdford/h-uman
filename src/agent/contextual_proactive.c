/* Contextual (context-driven) proactive outreach — detect a future-dated event
 * in a message, freeze a post-event "how'd it go?" from the real topic, and
 * decide when it should fire. See include/human/agent/contextual_proactive.h.
 *
 * Everything here is pure w.r.t. the clock (resolve/decide take now_ts) so the
 * temporal grammar is testable without the daemon. The activation gate and the
 * actual scheduling/send live in the daemon caller, not here. */
#include "human/agent/contextual_proactive.h"
#include "human/context/event_extract.h"
#include "human/core/string.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── small case-insensitive helpers ───────────────────────────────────────── */

static bool ci_prefix(const char *s, size_t s_len, const char *p) {
    size_t p_len = strlen(p);
    if (s_len < p_len)
        return false;
    for (size_t i = 0; i < p_len; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)p[i]))
            return false;
    }
    return true;
}

static bool ci_equals(const char *s, size_t s_len, const char *lit) {
    return s_len == strlen(lit) && ci_prefix(s, s_len, lit);
}

/* ── activation gate ──────────────────────────────────────────────────────── */

hu_contextual_proactive_mode_t hu_contextual_proactive_mode_from_str(const char *s) {
    if (!s || !s[0])
        return HU_CONTEXTUAL_PROACTIVE_OFF;
    size_t n = strlen(s);
    /* Precedence ON > SHADOW > OFF. */
    if (ci_equals(s, n, "on") || ci_equals(s, n, "live") || ci_equals(s, n, "1") ||
        ci_equals(s, n, "true"))
        return HU_CONTEXTUAL_PROACTIVE_ON;
    if (ci_equals(s, n, "shadow"))
        return HU_CONTEXTUAL_PROACTIVE_SHADOW;
    return HU_CONTEXTUAL_PROACTIVE_OFF;
}

hu_contextual_proactive_mode_t hu_contextual_proactive_mode(void) {
    return hu_contextual_proactive_mode_from_str(getenv("HU_PROACTIVE_CONTEXTUAL"));
}

const char *hu_contextual_proactive_mode_str(hu_contextual_proactive_mode_t mode) {
    switch (mode) {
    case HU_CONTEXTUAL_PROACTIVE_SHADOW:
        return "shadow";
    case HU_CONTEXTUAL_PROACTIVE_ON:
        return "on";
    case HU_CONTEXTUAL_PROACTIVE_OFF:
    default:
        return "off";
    }
}

/* ── temporal resolution ──────────────────────────────────────────────────── */

/* Compose the absolute epoch (seconds) at local SEND_HOUR on the day reached by
 * (base local date) + offset_days. mktime normalizes mday/mon overflow and DST. */
static int64_t at_send_hour(const struct tm *base_local, int offset_days) {
    struct tm ev = *base_local;
    ev.tm_hour = HU_CONTEXTUAL_PROACTIVE_SEND_HOUR;
    ev.tm_min = 0;
    ev.tm_sec = 0;
    ev.tm_isdst = -1; /* let libc resolve DST for the target date */
    ev.tm_mday += offset_days;
    time_t t = mktime(&ev);
    return (t == (time_t)-1) ? 0 : (int64_t)t;
}

/* Index 0=Sunday..6=Saturday for a weekday word, or -1. Accepts full names. */
static int weekday_index(const char *s, size_t len) {
    static const char *const day_names[7] = {"sunday",   "monday", "tuesday", "wednesday",
                                             "thursday", "friday", "saturday"};
    for (int d = 0; d < 7; d++) {
        if (ci_equals(s, len, day_names[d]))
            return d;
    }
    return -1;
}

static int month_index(const char *s, size_t len) {
    static const char *const month_names[12] = {"january",   "february", "march",    "april",
                                                "may",       "june",     "july",     "august",
                                                "september", "october",  "november", "december"};
    for (int m = 0; m < 12; m++) {
        if (ci_prefix(s, len, month_names[m]) && strlen(month_names[m]) <= len)
            return m;
    }
    return -1;
}

/* Parse the first run of ASCII digits at s into [1,31], or 0 if none/invalid. */
static int parse_day_of_month(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && !isdigit((unsigned char)s[i]))
        i++;
    if (i >= len)
        return 0;
    int day = 0;
    while (i < len && isdigit((unsigned char)s[i])) {
        day = day * 10 + (s[i] - '0');
        i++;
        if (day > 99)
            return 0;
    }
    return (day >= 1 && day <= 31) ? day : 0;
}

int64_t hu_contextual_proactive_resolve_send_at(const char *temporal_ref, size_t len,
                                                int64_t now_ts) {
    if (!temporal_ref || len == 0)
        return 0;

    /* Trim surrounding whitespace. */
    while (len > 0 && (unsigned char)temporal_ref[0] <= 32) {
        temporal_ref++;
        len--;
    }
    while (len > 0 && (unsigned char)temporal_ref[len - 1] <= 32)
        len--;
    if (len == 0)
        return 0;

    time_t now = (time_t)now_ts;
    struct tm base;
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (localtime_s(&base, &now) != 0)
        return 0;
#else
    if (!localtime_r(&now, &base))
        return 0;
#endif

    /* Past or too-vague references: never fire a "how'd it go". */
    if (ci_prefix(temporal_ref, len, "yesterday") || ci_prefix(temporal_ref, len, "last ") ||
        ci_equals(temporal_ref, len, "this week") || ci_equals(temporal_ref, len, "this month") ||
        ci_equals(temporal_ref, len, "next month"))
        return 0;

    int64_t send_at = 0;

    if (ci_equals(temporal_ref, len, "today")) {
        send_at = at_send_hour(&base, 0);
    } else if (ci_equals(temporal_ref, len, "tomorrow")) {
        send_at = at_send_hour(&base, 1);
    } else if (ci_equals(temporal_ref, len, "next week")) {
        send_at = at_send_hour(&base, 7);
    } else if (ci_prefix(temporal_ref, len, "in ")) {
        /* "in N days" / "in N day" / "in N weeks" / "in N week" */
        int n = parse_day_of_month(temporal_ref, len); /* reused digit scanner */
        if (n <= 0)
            return 0;
        bool weeks = hu_str_contains_ci_cstr(temporal_ref, len, "week");
        send_at = at_send_hour(&base, weeks ? n * 7 : n);
    } else {
        /* Weekday name, with optional "this "/"next " prefix. */
        const char *body = temporal_ref;
        size_t body_len = len;
        bool next_week = false;
        if (ci_prefix(temporal_ref, len, "next ")) {
            next_week = true;
            body += 5;
            body_len -= 5;
        } else if (ci_prefix(temporal_ref, len, "this ")) {
            body += 5;
            body_len -= 5;
        }
        int wd = weekday_index(body, body_len);
        if (wd >= 0) {
            int days_until = (wd - base.tm_wday + 7) % 7; /* 0..6, 0 == today */
            if (next_week)
                days_until += 7; /* "next Friday" => next week's Friday */
            send_at = at_send_hour(&base, days_until);
        } else {
            /* Absolute calendar date: "March 15th", "the 23rd", "the 15th". */
            int mon = month_index(temporal_ref, len);
            int day = parse_day_of_month(temporal_ref, len);
            if (day <= 0)
                return 0;
            struct tm ev = base;
            ev.tm_hour = HU_CONTEXTUAL_PROACTIVE_SEND_HOUR;
            ev.tm_min = 0;
            ev.tm_sec = 0;
            ev.tm_isdst = -1;
            ev.tm_mday = day;
            if (mon >= 0) {
                /* Month named: that month this year, or next year if already past. */
                ev.tm_mon = mon;
                time_t t = mktime(&ev);
                send_at = (t == (time_t)-1) ? 0 : (int64_t)t;
                if (send_at != 0 && send_at <= now_ts) {
                    ev.tm_year += 1;
                    t = mktime(&ev);
                    send_at = (t == (time_t)-1) ? 0 : (int64_t)t;
                }
            } else {
                /* Bare day-of-month: this month, or next month if already past. */
                time_t t = mktime(&ev);
                send_at = (t == (time_t)-1) ? 0 : (int64_t)t;
                if (send_at != 0 && send_at <= now_ts) {
                    ev.tm_mon += 1; /* mktime normalizes Dec->Jan rollover */
                    t = mktime(&ev);
                    send_at = (t == (time_t)-1) ? 0 : (int64_t)t;
                }
            }
            /* Absolute-date branch already accounts for "past" by rolling
             * forward; fall through to the final guard for safety. */
            return (send_at > now_ts) ? send_at : 0;
        }
    }

    /* Relative/weekday branch: reject anything that landed in the past
     * (e.g. an event "today" when it's already past SEND_HOUR). */
    return (send_at > now_ts) ? send_at : 0;
}

/* ── topic normalization + message ────────────────────────────────────────── */

size_t hu_contextual_proactive_normalize_topic(const char *topic, size_t len, char *out,
                                               size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!topic || len == 0)
        return 0;

    /* Trim surrounding whitespace. */
    while (len > 0 && (unsigned char)topic[0] <= 32) {
        topic++;
        len--;
    }
    while (len > 0 && (unsigned char)topic[len - 1] <= 32)
        len--;

    /* Iteratively strip leading filler. Longest forms first so "i have a "
     * wins over "i have ". */
    static const char *const filler[] = {
        "i have a ", "i have an ", "i've got a ", "i've got an ", "we have a ", "we have an ",
        "i have ",   "i've got ",  "we have ",    "got a ",       "got an ",    "got ",
        "have a ",   "have an ",   "have ",       "my ",          "the ",       "a ",
        "an ",       "i ",         NULL};
    bool changed = true;
    while (changed && len > 0) {
        changed = false;
        for (size_t f = 0; filler[f]; f++) {
            size_t fl = strlen(filler[f]);
            if (len >= fl && ci_prefix(topic, len, filler[f])) {
                topic += fl;
                len -= fl;
                changed = true;
                break;
            }
        }
    }

    /* Trim trailing punctuation/space. */
    while (len > 0) {
        char c = topic[len - 1];
        if ((unsigned char)c <= 32 || c == '.' || c == ',' || c == '!' || c == '?' || c == ';' ||
            c == ':')
            len--;
        else
            break;
    }
    if (len == 0)
        return 0;

    size_t copy = (len < cap - 1) ? len : cap - 1;
    memcpy(out, topic, copy);
    out[copy] = '\0';
    return copy;
}

/* ── topic-quality gate ───────────────────────────────────────────────────── */

/* Words that mark a topic as a clause rather than a noun phrase. Matched with
 * word boundaries (hu_str_contains_word_ci_n) per
 * .claude/rules/substring-classifier-pitfalls.md — "im" must not fire inside
 * "swimming", "will" must not fire inside "goodwill". Apostrophes are word
 * boundaries, so "I'll" is caught by "i". */
static const char *const topic_clause_words[] = {
    /* pronouns */
    "i", "im", "ive", "ill", "id", "me", "you", "your", "youre", "we", "weve", "hes", "shes",
    "theyre",
    /* auxiliaries / copulas */
    "is", "are", "was", "were", "will", "be", "been", "being", "am", "dont", "cant", "wont",
    /* clause verbs seen in real extractor output */
    "going", "gonna", "get", "got", "went", "think", "thinking", "know", "need", "want", "try",
    "trying", "come",
    /* question words + discourse markers (2026-07-21 leak wave: "What's" splits
     * at the apostrophe into "what"+"s", which passed the original list and
     * scheduled "how'd the What's go?"; "So" and "How ya feeling" likewise). */
    "what", "whats", "how", "when", "where", "why", "who", "whos", "so", "well", "oh", "hey", "ya",
    "it", "its", "this", "that", "feeling",
    /* interjections */
    "okay", "ok", "yeah", "yep", "sure", "thanks", NULL};

bool hu_contextual_proactive_topic_is_sendable(const char *topic, size_t len) {
    if (!topic || len == 0 || len > 48)
        return false;

    /* Sentence punctuation ANYWHERE means the "topic" is a clause. The
     * normalizer already strips trailing punctuation, so any survivor is
     * internal ("It will be tomorrow. Im working"). */
    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < len; i++) {
        char c = topic[i];
        if (c == '.' || c == '!' || c == '?' || c == ';' || c == ':' || c == ',' || c == '\n')
            return false;
        bool sp = ((unsigned char)c <= 32);
        if (!sp && !in_word) {
            words++;
            in_word = true;
        } else if (sp) {
            in_word = false;
        }
    }

    /* Real event topics are 1-4 words ("interview", "dentist appointment",
     * "parent teacher conference"). Longer means the extractor grabbed prose. */
    if (words == 0 || words > 4)
        return false;

    /* Apostrophe-FOLDED copy for the clause-word scan.
     *
     * Every contraction in topic_clause_words is spelled without an apostrophe
     * ("dont", "cant", "wont", "hes", "shes", "theyre", "youre", "im", ...)
     * while hu_str_contains_word_ci_n treats an apostrophe as a word boundary.
     * So real text "don't" tokenized to "don" + "t" and matched NEITHER, and the
     * entry that exists to catch it never fired. Those entries only ever worked
     * by accident, when the bare prefix was independently listed ("I'm" caught
     * by "i", "what's" by "what", "it's" by "it"); "don't", "won't", "he's",
     * "she's" and "they're" had no such backstop and passed the gate.
     *
     * That is how "hey how are you doing with don't understand provide?" reached
     * Seth's sister on 2026-07-26 15:05 — she replied "Turn AI off". Folding the
     * apostrophe out makes "don't" read as "dont" and the existing list work as
     * written. Same class as ~/.claude/rules/substring-classifier-pitfalls.md:
     * the tokenizer and the keyword list disagreed about word shape. */
    char folded[49]; /* len is <= 48, checked above */
    size_t folded_len = 0;
    for (size_t i = 0; i < len; i++) {
        char c = topic[i];
        if (c == '\'' || (unsigned char)c == 0x92) /* ASCII and CP-1252 curly */
            continue;
        folded[folded_len++] = c;
    }
    /* UTF-8 right single quote (U+2019 = E2 80 99) — drop the 3-byte sequence. */
    size_t clean_len = 0;
    for (size_t i = 0; i < folded_len;) {
        if (i + 2 < folded_len && (unsigned char)folded[i] == 0xE2 &&
            (unsigned char)folded[i + 1] == 0x80 && (unsigned char)folded[i + 2] == 0x99) {
            i += 3;
            continue;
        }
        folded[clean_len++] = folded[i++];
    }
    folded_len = clean_len;

    for (size_t w = 0; topic_clause_words[w]; w++) {
        if (hu_str_contains_word_ci_n(topic, len, topic_clause_words[w]) ||
            hu_str_contains_word_ci_n(folded, folded_len, topic_clause_words[w]))
            return false;
    }
    return true;
}

size_t hu_contextual_proactive_build_message(const char *topic, size_t len, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!topic || len == 0)
        return 0; /* never fabricate a topicless contextual proactive */

    int n = snprintf(out, cap, "how'd the %.*s go?", (int)len, topic);
    if (n <= 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

/* ── decision aggregator ──────────────────────────────────────────────────── */

hu_error_t hu_contextual_proactive_decide(hu_allocator_t *alloc, const char *inbound, size_t len,
                                          int64_t now_ts, hu_contextual_proactive_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!inbound || len == 0)
        return HU_OK;

    hu_event_extract_result_t events;
    memset(&events, 0, sizeof(events));
    hu_error_t err = hu_event_extract(alloc, inbound, len, &events);
    if (err != HU_OK)
        return err;

    for (size_t i = 0; i < events.event_count && out->count < HU_CONTEXTUAL_PROACTIVE_MAX; i++) {
        const hu_extracted_event_t *ev = &events.events[i];
        if (ev->confidence < HU_CONTEXTUAL_PROACTIVE_MIN_CONFIDENCE)
            continue;
        if (!ev->description || ev->description_len == 0 || !ev->temporal_ref ||
            ev->temporal_ref_len == 0)
            continue;

        hu_contextual_proactive_decision_t cand;
        memset(&cand, 0, sizeof(cand));

        size_t tlen = hu_contextual_proactive_normalize_topic(ev->description, ev->description_len,
                                                              cand.topic, sizeof(cand.topic));
        if (tlen == 0)
            continue;

        /* Topic-quality gate (2026-07-18 audit): the extractor can hand back a
         * whole clause as a "description"; splicing that into the template
         * produced real sends like "how'd the It will be tomorrow. Im working
         * go?". Only short noun-phrase topics survive — skipping a follow-up
         * costs nothing, sending garbage costs trust. */
        if (!hu_contextual_proactive_topic_is_sendable(cand.topic, tlen))
            continue;

        int64_t send_at =
            hu_contextual_proactive_resolve_send_at(ev->temporal_ref, ev->temporal_ref_len, now_ts);
        if (send_at <= now_ts || send_at == 0)
            continue;

        size_t mlen = hu_contextual_proactive_build_message(cand.topic, tlen, cand.message,
                                                            sizeof(cand.message));
        if (mlen == 0)
            continue;

        /* De-duplicate by topic so two mentions in one burst don't double-fire. */
        bool dup = false;
        for (size_t j = 0; j < out->count; j++) {
            if (ci_equals(out->items[j].topic, strlen(out->items[j].topic), cand.topic)) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        cand.send_at_ms = send_at * 1000;
        cand.confidence = ev->confidence;
        out->items[out->count++] = cand;
    }

    hu_event_extract_result_deinit(&events, alloc);
    return HU_OK;
}

/* ── SHADOW metric capture ────────────────────────────────────────────────── */

size_t hu_contextual_proactive_shadow_summary(const hu_contextual_proactive_result_t *res,
                                              const char *contact_id, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!res || res->count == 0)
        return 0;

    int pos = snprintf(out, cap, "contextual_proactive(shadow): contact=%s decided %zu [",
                       contact_id ? contact_id : "?", res->count);
    for (size_t i = 0; i < res->count && pos > 0 && (size_t)pos < cap; i++) {
        pos += snprintf(out + pos, cap - (size_t)pos, "%s%s@%lld(%.2f)", i ? "," : "",
                        res->items[i].topic, (long long)(res->items[i].send_at_ms / 1000),
                        res->items[i].confidence);
    }
    if (pos > 0 && (size_t)pos < cap)
        (void)snprintf(out + pos, cap - (size_t)pos, "]");
    return strlen(out); /* snprintf always NUL-terminates; report actual length */
}
