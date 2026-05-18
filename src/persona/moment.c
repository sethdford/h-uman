#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "human/moment.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "human/core/error.h"

/* ── hu_conversation_history_t definition ──────────────────────────────────
 * The type is forward-declared in moment.h as an opaque struct. This TU
 * owns the concrete definition. Tests access it via make_history / free_history
 * helpers defined in the test file. */

#define HU_HISTORY_TEXT_CAP 512

typedef struct hu_conversation_history_entry {
    int64_t ts_s;                   /* Unix timestamp, seconds */
    bool outbound;                  /* true = sent by us; false = inbound */
    char text[HU_HISTORY_TEXT_CAP]; /* message text, NUL-terminated */
} hu_conversation_history_entry_t;

struct hu_conversation_history_t {
    size_t count;
    hu_conversation_history_entry_t *entries; /* heap-allocated array[count] */
};

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

hu_error_t hu_moment_compose_from_inputs(const struct hu_persona_t *persona,
                                         const struct hu_persona_overlay_t *overlay,
                                         const struct hu_conversation_history_t *history,
                                         int64_t last_their_ts_s, int64_t last_our_ts_s,
                                         const char *contact_tz, int64_t now_s, hu_moment_t *out) {
    (void)persona;
    (void)overlay;
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

    /* Phase fields: local and contact's timezone. */
    out->phase_local = phase_for_tz(now_s, NULL);
    if (contact_tz != NULL && contact_tz[0] != '\0') {
        out->phase_theirs = phase_for_tz(now_s, contact_tz);
        out->source_flags |= HU_MOMENT_SRC_CONTACT_TZ;
    } else {
        out->phase_theirs = out->phase_local;
    }
    out->it_is_unusual_hour_for_them = (out->phase_theirs == HU_MOMENT_PHASE_DEEP_NIGHT);

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
            int64_t age_s = now_s - last_inbound->ts_s;
            if (age_s < 0)
                age_s = 0;

            /* thread_is_continuation: last inbound is within the window. */
            out->thread_is_continuation = (age_s < HU_CONTINUATION_WINDOW_S);

            /* topic_still_open: recent inbound AND substantive message text. */
            int wc = count_words(last_inbound->text);
            if (out->thread_is_continuation && wc >= HU_TOPIC_MIN_WORDS) {
                out->topic_still_open = true;
                extract_topic_hint(last_inbound->text, out->topic_hint, sizeof out->topic_hint);
            }
        }
    }

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

void hu_moment_history_free(struct hu_conversation_history_t *h) {
    if (!h)
        return;
    free(h->entries);
    free(h);
}
