#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "human/moment.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "human/core/error.h"

/* hu_conversation_history_t lives in the internal header so moment_render.c
 * can iterate entries without a public accessor explosion. */
#include "moment_internal.h"

/* Returns seconds elapsed since ts_s, clamped to 0 on clock skew.
   Returns -1 when ts_s is negative (timestamp not available). */
static int64_t compute_delta_s(int64_t ts_s, int64_t now_s) {
    if (ts_s < 0)
        return -1;
    int64_t d = now_s - ts_s;
    return d < 0 ? 0 : d;
}

/* Maps (hour, minute) in local time to a hu_moment_phase_t.
   Boundaries match the enum comments in moment.h. */
static hu_moment_phase_t hour_to_phase(int hour, int min) {
    int t = hour * 60 + min;
    if (t < 5 * 60 + 30)
        return HU_MOMENT_PHASE_DEEP_NIGHT;
    if (t < 7 * 60 + 30)
        return HU_MOMENT_PHASE_EARLY_MORNING;
    if (t < 11 * 60)
        return HU_MOMENT_PHASE_MORNING;
    if (t < 14 * 60)
        return HU_MOMENT_PHASE_MIDDAY;
    if (t < 17 * 60 + 30)
        return HU_MOMENT_PHASE_AFTERNOON;
    if (t < 21 * 60 + 30)
        return HU_MOMENT_PHASE_EVENING;
    /* 21:30–24:00 */ return HU_MOMENT_PHASE_NIGHT;
}

/* Converts a Unix timestamp to a phase using the given IANA timezone name.
 * When tz is NULL, the process-local timezone is used.
 *
 * THREAD SAFETY: This function temporarily modifies the TZ environment
 * variable when tz != NULL. It restores it before returning, but the
 * swap is NOT atomic. Callers must ensure single-threaded access (the
 * agent-turn caller is single-threaded). Do not call concurrently.
 *
 * BUFFER SIZE: saved_tz uses 256 bytes, safely larger than any real IANA
 * timezone name (longest known: ~40 chars). */
static hu_moment_phase_t phase_for_tz(int64_t now_s, const char *tz) {
    time_t t = (time_t)now_s;
    struct tm tm_out;

    if (tz != NULL) {
        const char *saved = getenv("TZ");
        char saved_copy[256];
        saved_copy[0] = '\0';
        if (saved)
            snprintf(saved_copy, sizeof saved_copy, "%s", saved);

        setenv("TZ", tz, 1);
        tzset();
        localtime_r(&t, &tm_out);

        /* Restore: if TZ was set before, restore it; otherwise remove it. */
        if (saved)
            setenv("TZ", saved_copy, 1);
        else
            unsetenv("TZ");
        tzset();
    } else {
        localtime_r(&t, &tm_out);
    }

    return hour_to_phase(tm_out.tm_hour, tm_out.tm_min);
}

/* Continuation window: messages within 30 minutes are considered a thread. */
#define HU_CONTINUATION_WINDOW_S ((int64_t)1800)

/* Minimum word count for a message to be "substantive" (worth extracting a topic hint). */
#define HU_TOPIC_MIN_WORDS 3

/* Maximum words to copy into topic_hint. */
#define HU_TOPIC_HINT_MAX_WORDS 6

/* Copy up to HU_TOPIC_HINT_MAX_WORDS words (or out_cap-1 chars) from text into
 * out_buf, lower-cased and space-separated. Trailing NUL is always written.
 * Returns the number of words copied. */
static int extract_topic_hint(const char *text, char *out_buf, size_t out_cap) {
    if (!text || !out_buf || out_cap == 0)
        return 0;
    out_buf[0] = '\0';

    size_t pos = 0; /* write position in out_buf */
    int words = 0;
    const char *p = text;

    while (*p && words < HU_TOPIC_HINT_MAX_WORDS) {
        /* skip whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;

        /* add space separator between words */
        if (words > 0 && pos + 1 < out_cap) {
            out_buf[pos++] = ' ';
        }

        /* copy one word, lower-cased */
        while (*p && !isspace((unsigned char)*p)) {
            if (pos + 1 < out_cap)
                out_buf[pos++] = (char)tolower((unsigned char)*p);
            p++;
        }
        words++;
    }
    if (pos < out_cap)
        out_buf[pos] = '\0';
    else
        out_buf[out_cap - 1] = '\0';

    return words;
}

/* Count words in a NUL-terminated string. */
static int count_words(const char *text) {
    if (!text)
        return 0;
    int n = 0;
    const char *p = text;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p) {
            n++;
            while (*p && !isspace((unsigned char)*p))
                p++;
        }
    }
    return n;
}

/* Returns true if text has no uppercase letters (ignores non-alpha chars). */
static bool is_all_lowercase(const char *text) {
    if (!text)
        return false;
    for (const char *p = text; *p; p++) {
        if (isupper((unsigned char)*p))
            return false;
    }
    return true;
}

/* Decode one UTF-8 codepoint from *p, advancing *p past the sequence.
 * Returns the codepoint, or 0xFFFD on invalid input. */
static uint32_t utf8_decode(const char **p) {
    unsigned char c = (unsigned char)**p;
    uint32_t cp;
    int extra;
    if (c < 0x80) {
        cp = c;
        extra = 0;
    } else if (c < 0xC0) {
        /* continuation byte without a lead — invalid */
        cp = 0xFFFD;
        extra = 0;
    } else if (c < 0xE0) {
        cp = c & 0x1F;
        extra = 1;
    } else if (c < 0xF0) {
        cp = c & 0x0F;
        extra = 2;
    } else {
        cp = c & 0x07;
        extra = 3;
    }
    (*p)++;
    for (int i = 0; i < extra; i++) {
        unsigned char cont = (unsigned char)**p;
        if ((cont & 0xC0) != 0x80) {
            /* not a continuation byte — invalid sequence */
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
        (*p)++;
    }
    return cp;
}

/* Returns true if text contains any codepoint in the emoji ranges:
 *   U+1F000..U+1FAFF  (pictographs, emoticons, transport, etc.)
 *   U+2600..U+27BF    (misc symbols + dingbats) */
static bool contains_emoji_codepoint(const char *text) {
    if (!text)
        return false;
    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF))
            return true;
    }
    return false;
}

/* Returns true if the last non-whitespace character is sentence-ending punctuation. */
static bool ends_with_punctuation(const char *text) {
    if (!text || !*text)
        return false;
    const char *last = text + strlen(text) - 1;
    while (last > text && isspace((unsigned char)*last))
        last--;
    char c = *last;
    return c == '.' || c == '?' || c == '!' || c == ';' || c == ':' || c == ',';
}

/* Negative-affect keywords for DISTRESSED tone detection.
 * Checked as case-insensitive substrings. */
static const char *const k_distressed_markers[] = {
    "ugh",   "tired", "can't",         "cant",  "don't know", "dont know", "hate",
    "sucks", "awful", "nothing works", "never", "anymore",    NULL};

/* Infer tone from the last inbound message text. */
static hu_moment_tone_t infer_tone(const char *text, int word_count) {
    if (word_count < 4)
        return HU_MOMENT_TONE_TERSE;

    /* Case-insensitive substring search using a lower-cased copy (stack buffer).
     * Cap at 511 chars to avoid large stack allocations. */
    char lower[512];
    size_t len = strlen(text);
    if (len >= sizeof lower)
        len = sizeof lower - 1;
    for (size_t i = 0; i < len; i++)
        lower[i] = (char)tolower((unsigned char)text[i]);
    lower[len] = '\0';

    for (int i = 0; k_distressed_markers[i]; i++) {
        if (strstr(lower, k_distressed_markers[i]))
            return HU_MOMENT_TONE_DISTRESSED;
    }

    /* Excited: contains '!' */
    if (strchr(text, '!'))
        return HU_MOMENT_TONE_EXCITED;

    return HU_MOMENT_TONE_WARM;
}

/* Lowercase substring search: returns true iff needle (already lowercase) is
 * found inside text after lower-casing text. Cap at 511 chars for the
 * lowercased buffer; messages beyond that are searched only over their first
 * 511 chars (good enough for opener detection, which scans short phrases). */
static bool text_contains_phrase_ci(const char *text, const char *needle_lower) {
    if (!text || !needle_lower)
        return false;
    char lower[512];
    size_t len = strlen(text);
    if (len >= sizeof lower)
        len = sizeof lower - 1;
    for (size_t i = 0; i < len; i++)
        lower[i] = (char)tolower((unsigned char)text[i]);
    lower[len] = '\0';
    return strstr(lower, needle_lower) != NULL;
}

/* Decide the suggested opener for the response based on the current moment.
 * Top-down: first matching rule wins. The order encodes overrides:
 *   continuation > long silence > unusual hour > phase-based greets > none. */
static hu_moment_open_t decide_open(const hu_moment_t *m, const char *last_inbound_text) {
    /* 1. Mid-thread continuation: no greeting under any circumstance. */
    if (m->thread_is_continuation)
        return HU_MOMENT_OPEN_NONE;

    /* 2. Long silence (> 3 days on either side): reconnect rather than greet. */
    const int64_t three_days_s = (int64_t)3 * 86400;
    if (m->time_since_their_last_msg_s > three_days_s ||
        m->time_since_our_last_msg_s > three_days_s)
        return HU_MOMENT_OPEN_RECONNECT;

    /* 3. It's an unusual hour for the contact (typically DEEP_NIGHT in their tz).
     * Acknowledge the gap — never apply a morning greet under this branch. This
     * is the "no good morning at 3am" regression guard. */
    if (m->it_is_unusual_hour_for_them)
        return HU_MOMENT_OPEN_ACKNOWLEDGE_GAP;

    /* 4. Fresh-day morning: greet only if we haven't talked recently. */
    const int64_t fresh_day_silence_s = (int64_t)8 * 3600;
    if ((m->phase_local == HU_MOMENT_PHASE_EARLY_MORNING ||
         m->phase_local == HU_MOMENT_PHASE_MORNING) &&
        (m->time_since_their_last_msg_s < 0 ||
         m->time_since_their_last_msg_s > fresh_day_silence_s))
        return HU_MOMENT_OPEN_GREET_MORNING;

    /* 5. Night, and the inbound mentions sleeping or tomorrow: match the
     * "good night" register. */
    if (m->phase_local == HU_MOMENT_PHASE_NIGHT && last_inbound_text != NULL) {
        if (text_contains_phrase_ci(last_inbound_text, "night") ||
            text_contains_phrase_ci(last_inbound_text, "sleep") ||
            text_contains_phrase_ci(last_inbound_text, "tomorrow") ||
            text_contains_phrase_ci(last_inbound_text, "talk later"))
            return HU_MOMENT_OPEN_GREET_NIGHT;
    }

    /* 6. Default: no greeting. */
    return HU_MOMENT_OPEN_NONE;
}

/* Decide a closing-style hint. Mostly NONE; emits GREET_NIGHT only when phase
 * is NIGHT and the conversation tone is warm/excited (we want to wrap warmly
 * rather than coldly). Re-uses the OPEN enum per spec (the close vocabulary
 * is a subset of the open vocabulary). */
static hu_moment_open_t decide_close(const hu_moment_t *m) {
    if (m->phase_local == HU_MOMENT_PHASE_NIGHT && (m->their_recent_tone == HU_MOMENT_TONE_WARM ||
                                                    m->their_recent_tone == HU_MOMENT_TONE_EXCITED))
        return HU_MOMENT_OPEN_GREET_NIGHT;
    return HU_MOMENT_OPEN_NONE;
}

/* Decide the response-length budget. Defaults to MIRROR (match the contact's
 * recent length). Special cases override:
 *   - TERSE when the last message was very short AND the rolling average is
 *     also short (signals casual ping rhythm)
 *   - MEDIUM when the contact is distressed (they need room to feel heard;
 *     don't dump bullet lists but don't be one-word either)
 *   - LONG never set by default — reserved for overlay opt-in (future) */
static hu_moment_brevity_t decide_brevity(const hu_moment_t *m) {
    if (m->their_recent_tone == HU_MOMENT_TONE_DISTRESSED)
        return HU_MOMENT_BREVITY_MEDIUM;
    if (m->their_recent_tone == HU_MOMENT_TONE_TERSE && m->their_avg_length_words <= 4)
        return HU_MOMENT_BREVITY_TERSE;
    if (m->their_avg_length_words > 0 && m->their_avg_length_words <= 12)
        return HU_MOMENT_BREVITY_SHORT;
    return HU_MOMENT_BREVITY_MIRROR;
}

/* Compute "next 8am in the contact's timezone" as a Unix timestamp. Uses the
 * TZ env-swap pattern (same caveats as phase_for_tz: not thread-safe). Falls
 * back to local TZ when tz is NULL. */
static int64_t next_8am_local_s(int64_t now_s, const char *tz) {
    time_t t = (time_t)now_s;
    struct tm tm_now;
    char saved_copy[256] = {0};
    const char *saved = NULL;

    if (tz != NULL) {
        saved = getenv("TZ");
        if (saved)
            snprintf(saved_copy, sizeof saved_copy, "%s", saved);
        setenv("TZ", tz, 1);
        tzset();
    }
    localtime_r(&t, &tm_now);

    /* Build a tm for 08:00 the same local day. */
    struct tm tm_8am = tm_now;
    tm_8am.tm_hour = 8;
    tm_8am.tm_min = 0;
    tm_8am.tm_sec = 0;
    tm_8am.tm_isdst = -1; /* let mktime resolve DST */
    time_t target = mktime(&tm_8am);

    /* If we're already past 08:00 local today, target tomorrow. */
    if (target <= t) {
        tm_8am.tm_mday += 1;
        tm_8am.tm_isdst = -1;
        target = mktime(&tm_8am);
    }

    if (tz != NULL) {
        if (saved)
            setenv("TZ", saved_copy, 1);
        else
            unsetenv("TZ");
        tzset();
    }
    return (int64_t)target;
}

/* Decide whether to defer the outbound. Returns 0 if "send now" is fine,
 * else a future Unix timestamp (next 08:00 in the contact's tz) when:
 *   - it's DEEP_NIGHT for them, AND
 *   - this is NOT a continuation thread, AND
 *   - the inbound did NOT signal night (e.g. "good night") — replying to a
 *     night sign-off immediately is fine; spontaneously messaging at 3am is not. */
static int64_t decide_defer_send(const hu_moment_t *m, const char *last_inbound_text, int64_t now_s,
                                 const char *contact_tz) {
    if (m->phase_theirs != HU_MOMENT_PHASE_DEEP_NIGHT)
        return 0;
    if (m->thread_is_continuation)
        return 0;
    if (last_inbound_text != NULL && (text_contains_phrase_ci(last_inbound_text, "night") ||
                                      text_contains_phrase_ci(last_inbound_text, "sleep") ||
                                      text_contains_phrase_ci(last_inbound_text, "tomorrow")))
        return 0;
    return next_8am_local_s(now_s, contact_tz);
}

/* Public agent-bridging wrapper.
 *
 * STATUS: stub. The plan assumed accessors like `hu_agent_persona`,
 * `hu_persona_overlay_for_channel`, `hu_agent_recent_history`,
 * `hu_contact_tz` would exist; an audit-verify-before-allege pass (per
 * ~/.claude/rules/audit-verify-before-allege.md) found none of these
 * symbols in the codebase, and there is no `hu_contact_t` type at all —
 * only `hu_contact_profile`, `hu_contact_baseline`, etc. The real bridge
 * to the daemon's reactive path lives in Phase 3 Task 3.2 (agent_turn.c
 * integration), where the call site has full context to pick the right
 * existing accessors (`hu_contact_send_recency_last_ts`, history loaded
 * via `load_conversation_history` in daemon.c, persona from the agent's
 * already-loaded state, etc.).
 *
 * Until Phase 3 lands, callers should use `hu_moment_compose_from_inputs`
 * directly. That entry point is the actual contract; this wrapper exists
 * only to satisfy the public header signature. */
hu_error_t hu_moment_compose(const struct hu_agent_t *agent, const struct hu_contact_t *contact,
                             const char *channel_id, int64_t now_s, hu_moment_t *out) {
    (void)agent;
    (void)contact;
    (void)channel_id;
    (void)now_s;
    if (out == NULL)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_moment_compose_from_inputs(const struct hu_persona_t *persona,
                                         const struct hu_persona_overlay_t *overlay,
                                         const struct hu_conversation_history_t *history,
                                         int64_t last_their_ts_s, int64_t last_our_ts_s,
                                         const char *contact_tz, int64_t now_s, hu_moment_t *out) {
    /* persona/overlay are reserved for future per-contact / per-channel
     * decision inputs; for now we only record their presence as provenance. */
    if (out == NULL)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    /* enum zero-values are correct defaults; only the time deltas need -1 overrides. */
    out->time_since_their_last_msg_s = compute_delta_s(last_their_ts_s, now_s);
    out->time_since_our_last_msg_s = compute_delta_s(last_our_ts_s, now_s);
    out->composed_at_s = now_s;
    if (last_their_ts_s >= 0)
        out->source_flags |= HU_MOMENT_SRC_LAST_THEIR_TS;
    if (last_our_ts_s >= 0)
        out->source_flags |= HU_MOMENT_SRC_LAST_OUR_TS;
    if (persona != NULL)
        out->source_flags |= HU_MOMENT_SRC_PERSONA;
    if (overlay != NULL)
        out->source_flags |= HU_MOMENT_SRC_OVERLAY;

    /* Phase fields: local and contact's timezone. */
    out->phase_local = phase_for_tz(now_s, NULL);
    if (contact_tz != NULL && contact_tz[0] != '\0') {
        out->phase_theirs = phase_for_tz(now_s, contact_tz);
        out->source_flags |= HU_MOMENT_SRC_CONTACT_TZ;
    } else {
        out->phase_theirs = out->phase_local;
    }
    out->it_is_unusual_hour_for_them = (out->phase_theirs == HU_MOMENT_PHASE_DEEP_NIGHT);

    /* Hoisted so the decision tree (below) can read the last inbound's text. */
    const char *last_inbound_text = NULL;

    /* Thread continuation + topic state from history. */
    if (history != NULL && history->count > 0) {
        out->source_flags |= HU_MOMENT_SRC_HISTORY;

        /* Walk backwards to find the most-recent inbound entry. */
        const hu_conversation_history_entry_t *last_inbound = NULL;
        for (size_t i = history->count; i-- > 0;) {
            if (!history->entries[i].outbound) {
                last_inbound = &history->entries[i];
                break;
            }
        }

        if (last_inbound != NULL) {
            last_inbound_text = last_inbound->text;
            int64_t age_s = now_s - last_inbound->ts_s;
            if (age_s < 0)
                age_s = 0;

            /* thread_is_continuation: last inbound is within the window. */
            out->thread_is_continuation = (age_s < HU_CONTINUATION_WINDOW_S);

            /* topic_still_open: recent inbound AND substantive message text. */
            int last_wc = count_words(last_inbound->text);
            if (out->thread_is_continuation && last_wc >= HU_TOPIC_MIN_WORDS) {
                out->topic_still_open = true;
                extract_topic_hint(last_inbound->text, out->topic_hint, sizeof out->topic_hint);
            }

            /* Style inference from the most recent inbound message. */
            out->they_use_lowercase = is_all_lowercase(last_inbound->text);
            out->they_use_emoji = contains_emoji_codepoint(last_inbound->text);
            out->they_use_punctuation_eol = ends_with_punctuation(last_inbound->text);
            out->their_recent_tone = infer_tone(last_inbound->text, last_wc);
        }

        /* Length stats across inbound entries (last up to 5). */
        int inbound_counts[5];
        size_t inbound_n = 0;
        for (size_t i = history->count; i-- > 0 && inbound_n < 5;) {
            if (!history->entries[i].outbound) {
                inbound_counts[inbound_n++] = count_words(history->entries[i].text);
            }
        }
        if (inbound_n > 0) {
            int sum = 0;
            int max = 0;
            for (size_t i = 0; i < inbound_n; i++) {
                sum += inbound_counts[i];
                if (inbound_counts[i] > max)
                    max = inbound_counts[i];
            }
            /* Round-to-nearest average. */
            out->their_avg_length_words = (int)((sum * 10 / (int)inbound_n + 5) / 10);
            /* With N<=5, p90 is essentially the max. */
            out->their_p90_length_words = max;
        }
    }

    /* ── suggested_open decision tree ──
     * Top-down — first matching rule wins. Order matters; comments call
     * out the override relationships (continuation overrides everything;
     * unusual-hour overrides phase-based greets). */
    out->suggested_open = decide_open(out, last_inbound_text);
    out->suggested_close = decide_close(out);
    out->suggested_brevity = decide_brevity(out);
    out->defer_send_until_s = decide_defer_send(out, last_inbound_text, now_s, contact_tz);

    return HU_OK;
}

/* ── Public history constructor/destructor ──────────────────────────────── */

struct hu_conversation_history_t *hu_moment_history_create(size_t count, const int64_t *ts_s,
                                                           const bool *outbound,
                                                           const char *const *text) {
    struct hu_conversation_history_t *h = malloc(sizeof(struct hu_conversation_history_t));
    if (!h)
        return NULL;
    h->count = count;
    h->entries = NULL;

    if (count == 0)
        return h;

    h->entries = calloc(count, sizeof(hu_conversation_history_entry_t));
    if (!h->entries) {
        free(h);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        h->entries[i].ts_s = ts_s ? ts_s[i] : 0;
        h->entries[i].outbound = outbound ? outbound[i] : false;
        if (text && text[i]) {
            strncpy(h->entries[i].text, text[i], HU_HISTORY_TEXT_CAP - 1);
            h->entries[i].text[HU_HISTORY_TEXT_CAP - 1] = '\0';
        }
    }
    return h;
}

/* ── Downstream pure predicates (Task 1.10) ──────────────────────────────── */

bool hu_moment_should_defer_send(const hu_moment_t *m) {
    if (m == NULL)
        return false;
    return m->defer_send_until_s > 0;
}

bool hu_moment_should_trigger_followup(const hu_moment_t *m, int64_t silence_threshold_s) {
    if (m == NULL)
        return false;
    if (m->time_since_their_last_msg_s < 0 || m->time_since_their_last_msg_s < silence_threshold_s)
        return false;
    if (m->time_since_our_last_msg_s < 0 || m->time_since_our_last_msg_s < silence_threshold_s)
        return false;
    if (!m->topic_still_open)
        return false;
    if (m->it_is_unusual_hour_for_them)
        return false;
    return true;
}

int hu_moment_brevity_cap_words(const hu_moment_t *m) {
    if (m == NULL)
        return 0;
    switch (m->suggested_brevity) {
    case HU_MOMENT_BREVITY_TERSE:
        return 8;
    case HU_MOMENT_BREVITY_SHORT:
        return 25;
    case HU_MOMENT_BREVITY_MEDIUM:
        return 60;
    case HU_MOMENT_BREVITY_LONG:
        return 0; /* 0 = no cap */
    case HU_MOMENT_BREVITY_MIRROR:
        /* Mirror the contact's average length with 30% slack. */
        if (m->their_avg_length_words > 0)
            return (m->their_avg_length_words * 13 + 5) / 10;
        return 25; /* fallback when no inbound history */
    }
    return 25;
}

void hu_moment_history_free(struct hu_conversation_history_t *h) {
    if (!h)
        return;
    free(h->entries);
    free(h);
}
