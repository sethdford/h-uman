/* Life-event lifecycle — see include/human/persona/life_events.h for the
 * cycle-4 evidence that motivated this module and the do-not-assert contract
 * it enforces. */

#include "human/persona/life_events.h"

#include <stdio.h>
#include <string.h>

/* ── State vocabulary ─────────────────────────────────────────────────── */

/* Closed vocabulary. `alias` lets a persona author write the natural spelling
 * without us reaching for substring matching (see header for why exact-match
 * is the correct matcher here and word-boundary matching is not). */
static const struct {
    const char *token;
    hu_life_event_state_t state;
} state_tokens[] = {
    {"pending", HU_LIFE_EVENT_STATE_PENDING},
    {"upcoming", HU_LIFE_EVENT_STATE_PENDING},
    {"planned", HU_LIFE_EVENT_STATE_PENDING},
    {"in_progress", HU_LIFE_EVENT_STATE_IN_PROGRESS},
    {"in-progress", HU_LIFE_EVENT_STATE_IN_PROGRESS},
    {"underway", HU_LIFE_EVENT_STATE_IN_PROGRESS},
    {"completed", HU_LIFE_EVENT_STATE_COMPLETED},
    {"done", HU_LIFE_EVENT_STATE_COMPLETED},
    {"cancelled", HU_LIFE_EVENT_STATE_CANCELLED},
    {"canceled", HU_LIFE_EVENT_STATE_CANCELLED},
    {"unknown", HU_LIFE_EVENT_STATE_UNKNOWN},
};

static bool token_equals_ci(const char *s, size_t len, const char *token) {
    size_t tlen = strlen(token);
    if (len != tlen)
        return false;
    for (size_t i = 0; i < len; i++) {
        char a = s[i];
        char b = token[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
    }
    return true;
}

hu_life_event_state_t hu_life_event_state_from_string(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_LIFE_EVENT_STATE_UNKNOWN;
    /* Trim surrounding whitespace so " completed " parses, but nothing else —
     * any interior token mismatch still falls through to UNKNOWN. */
    while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
        s++;
        len--;
    }
    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    if (len == 0)
        return HU_LIFE_EVENT_STATE_UNKNOWN;

    for (size_t i = 0; i < sizeof(state_tokens) / sizeof(state_tokens[0]); i++) {
        if (token_equals_ci(s, len, state_tokens[i].token))
            return state_tokens[i].state;
    }
    return HU_LIFE_EVENT_STATE_UNKNOWN;
}

const char *hu_life_event_state_str(hu_life_event_state_t state) {
    switch (state) {
    case HU_LIFE_EVENT_STATE_PENDING:
        return "pending";
    case HU_LIFE_EVENT_STATE_IN_PROGRESS:
        return "in progress";
    case HU_LIFE_EVENT_STATE_COMPLETED:
        return "completed";
    case HU_LIFE_EVENT_STATE_CANCELLED:
        return "cancelled";
    case HU_LIFE_EVENT_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

/* ── Date parsing ─────────────────────────────────────────────────────── */

/* Days from 1970-01-01 for a civil date (Howard Hinnant's days_from_civil).
 * Pure integer arithmetic — no mktime/timegm, so the result does not depend on
 * the host timezone and tests are deterministic on every machine. */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                  /* [0, 399] */
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

int64_t hu_life_event_parse_date(const char *s, size_t len) {
    if (!s || len < 10)
        return 0;
    /* Strict "YYYY-MM-DD"; anything else is treated as unset rather than
     * guessed at. A misparsed date would silently shift a staleness verdict. */
    for (size_t i = 0; i < 10; i++) {
        bool want_digit = (i != 4 && i != 7);
        if (want_digit) {
            if (s[i] < '0' || s[i] > '9')
                return 0;
        } else if (s[i] != '-') {
            return 0;
        }
    }
    int64_t y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int64_t m = (s[5] - '0') * 10 + (s[6] - '0');
    int64_t d = (s[8] - '0') * 10 + (s[9] - '0');
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970)
        return 0;
    return days_from_civil(y, m, d) * 86400;
}

/* ── The pure predicate ───────────────────────────────────────────────── */

hu_life_event_state_t hu_life_event_effective_state(const hu_life_event_t *ev, int64_t now_ts) {
    if (!ev)
        return HU_LIFE_EVENT_STATE_UNKNOWN;

    /* Terminal states are observations, not predictions — nothing about the
     * passage of time makes a completed move un-complete. Trust them. */
    if (ev->state == HU_LIFE_EVENT_STATE_COMPLETED || ev->state == HU_LIFE_EVENT_STATE_CANCELLED)
        return ev->state;

    /* Open-ended (no expected_date): there is no resolution point to be past,
     * so the declared state stands. An open-ended `pending` stays pending. */
    if (ev->expected_date <= 0)
        return ev->state;

    /* Still before the expected resolution — the declared state is current. */
    if (now_ts < ev->expected_date)
        return ev->state;

    /* Past the expected resolution. The question is whether the declared state
     * is an OBSERVATION made after that point, or a PREDICTION made before it.
     *
     * as_of >= expected_date means someone confirmed the state after the event
     * was due — e.g. "still in_progress, checked yesterday". That is real
     * information; keep it.
     *
     * as_of < expected_date (or absent) means the state predates the moment it
     * was supposed to resolve. We know the date passed; we do NOT know what
     * happened. This is exactly the cycle-4 failure: "moving on the 23rd"
     * declared on the 20th, read on the 27th, and completed into "done moving
     * all settled in now". The honest answer is UNKNOWN. */
    if (ev->as_of >= ev->expected_date)
        return ev->state;

    return HU_LIFE_EVENT_STATE_UNKNOWN;
}

bool hu_life_event_must_not_assert_completion(const hu_life_event_t *ev, int64_t now_ts) {
    hu_life_event_state_t eff = hu_life_event_effective_state(ev, now_ts);
    /* COMPLETED and CANCELLED are confirmed resolutions — the model may state
     * them. Everything else lacks a confirmed resolution, so asserting one
     * would be the fabrication this module exists to prevent. */
    return eff != HU_LIFE_EVENT_STATE_COMPLETED && eff != HU_LIFE_EVENT_STATE_CANCELLED;
}

/* ── Gate ─────────────────────────────────────────────────────────────── */

hu_gate_mode_t hu_life_events_gate(void) {
    /* HU_LIFE_EVENTS activation gated on the cycle-5 human blind A/B rating
     * sheet: do not flip to default-ON without a measurement showing the
     * hedging behavior is judged more human than the current confident-wrong
     * completions. Per .claude/rules/feature-gate-requires-measurement.md a
     * green test suite is NOT that measurement. */
    return hu_gate_mode_from_env("HU_LIFE_EVENTS", HU_GATE_OFF);
}

/* ── Directive rendering ──────────────────────────────────────────────── */

/* Appends to a fixed caller buffer, tracking position. Truncates rather than
 * overflowing. Returns false once the buffer is full so callers stop early. */
static bool append_str(char *out, size_t cap, size_t *pos, const char *s) {
    if (!s || *pos + 1 >= cap)
        return false;
    size_t rem = cap - *pos;
    int w = snprintf(out + *pos, rem, "%s", s);
    if (w < 0)
        return false;
    size_t written = ((size_t)w < rem) ? (size_t)w : rem - 1;
    *pos += written;
    return written == (size_t)w;
}

hu_error_t hu_life_events_build_directive(const hu_life_event_t *events, size_t count,
                                          int64_t now_ts, char *out, size_t cap, size_t *out_len) {
    if (!out || cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    out[0] = '\0';
    *out_len = 0;
    /* No events declared -> emit nothing. A persona without a `life_events`
     * array produces a byte-identical prompt to today's. */
    if (!events || count == 0)
        return HU_OK;

    size_t pos = 0;
    size_t rendered = 0;

    for (size_t i = 0; i < count; i++) {
        const hu_life_event_t *ev = &events[i];
        if (ev->description[0] == '\0')
            continue;

        if (rendered == 0 &&
            !append_str(out, cap, &pos, "\n--- Where your life is right now ---\n"))
            break;
        rendered++;

        hu_life_event_state_t eff = hu_life_event_effective_state(ev, now_ts);
        /* `[status: <label>]` is a single unambiguous token rather than prose.
         * It keeps the state machine-checkable — the contract test asserts the
         * ABSENCE of "[status: completed]", which a keyword search for "done"
         * or "completed" could not do, since the guidance sentence below
         * legitimately contains both words. Bracketed tokens are already the
         * idiom in this prompt surface (cf. "- [FOLLOW_UP] ... (95%)"). */
        if (!append_str(out, cap, &pos, "- ") || !append_str(out, cap, &pos, ev->description) ||
            !append_str(out, cap, &pos, " [status: ") ||
            !append_str(out, cap, &pos, hu_life_event_state_str(eff)) ||
            !append_str(out, cap, &pos, "]"))
            break;

        if (hu_life_event_must_not_assert_completion(ev, now_ts)) {
            /* THE CONTRACT. Per-event so it cannot be separated from the event
             * it governs by prompt trimming, and phrased as a concrete
             * behavior ("ask") rather than a prohibition alone — a bare "do not
             * say X" leaves the model no alternative move, and it will invent
             * one. */
            if (!append_str(out, cap, &pos,
                            ". You do NOT know whether this has finished. Do not say it is done, "
                            "finished, over, or settled, and do not invent how it turned out. If "
                            "it comes up, hedge or ask about it."))
                break;
        }
        if (!append_str(out, cap, &pos, "\n"))
            break;
    }

    if (rendered > 0 && pos > 0)
        (void)append_str(out, cap, &pos,
                         "Never upgrade one of these to finished just because its date has "
                         "passed. Saying \"i don't know yet\" or asking is always better than "
                         "guessing that it worked out.\n");

    out[pos < cap ? pos : cap - 1] = '\0';
    *out_len = pos < cap ? pos : cap - 1;
    return HU_OK;
}
