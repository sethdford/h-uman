#include "human/moment.h"

#include "human/core/error.h"
#include "moment_internal.h"

#include <stdio.h>
#include <string.h>

/* ── Internal: relative-time formatter ──────────────────────────────────────
 * "30s", "8m", "3h", "2d" — fits in 8 chars.  Returns chars written. */
static int format_relative(int64_t seconds, char *buf, size_t cap) {
    if (seconds < 0)
        return snprintf(buf, cap, "?");
    if (seconds < 60)
        return snprintf(buf, cap, "%llds", (long long)seconds);
    if (seconds < 3600)
        return snprintf(buf, cap, "%lldm", (long long)(seconds / 60));
    if (seconds < 86400)
        return snprintf(buf, cap, "%lldh", (long long)(seconds / 3600));
    return snprintf(buf, cap, "%lldd", (long long)(seconds / 86400));
}

static const char *phase_label(hu_moment_phase_t p) {
    switch (p) {
    case HU_MOMENT_PHASE_DEEP_NIGHT:
        return "deep night";
    case HU_MOMENT_PHASE_EARLY_MORNING:
        return "early morning";
    case HU_MOMENT_PHASE_MORNING:
        return "morning";
    case HU_MOMENT_PHASE_MIDDAY:
        return "midday";
    case HU_MOMENT_PHASE_AFTERNOON:
        return "afternoon";
    case HU_MOMENT_PHASE_EVENING:
        return "evening";
    case HU_MOMENT_PHASE_NIGHT:
        return "night";
    }
    return "";
}

static const char *open_cue(hu_moment_open_t o) {
    switch (o) {
    case HU_MOMENT_OPEN_NONE:
        return "no greeting";
    case HU_MOMENT_OPEN_ACKNOWLEDGE_GAP:
        return "acknowledge the late-hour gap";
    case HU_MOMENT_OPEN_GREET_MORNING:
        return "greet for a fresh morning";
    case HU_MOMENT_OPEN_GREET_NIGHT:
        return "match the night sign-off";
    case HU_MOMENT_OPEN_RECONNECT:
        return "reconnect warmly after the silence";
    }
    return "";
}

static const char *brevity_cue(hu_moment_brevity_t b) {
    switch (b) {
    case HU_MOMENT_BREVITY_MIRROR:
        return "match their length";
    case HU_MOMENT_BREVITY_TERSE:
        return "very short";
    case HU_MOMENT_BREVITY_SHORT:
        return "short";
    case HU_MOMENT_BREVITY_MEDIUM:
        return "give them room";
    case HU_MOMENT_BREVITY_LONG:
        return "any length";
    }
    return "";
}

static const char *tone_label(hu_moment_tone_t t) {
    switch (t) {
    case HU_MOMENT_TONE_UNKNOWN:
        return "";
    case HU_MOMENT_TONE_TERSE:
        return "terse";
    case HU_MOMENT_TONE_WARM:
        return "warm";
    case HU_MOMENT_TONE_EXCITED:
        return "excited";
    case HU_MOMENT_TONE_QUIET:
        return "quiet";
    case HU_MOMENT_TONE_DISTRESSED:
        return "distressed";
    }
    return "";
}

hu_error_t hu_moment_render_prompt(const hu_moment_t *moment, char *buf, size_t buf_cap,
                                   size_t *out_len) {
    if (moment == NULL || buf == NULL || buf_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Empty moment (no inputs were available) → empty fragment. */
    if (moment->source_flags == 0u) {
        buf[0] = '\0';
        if (out_len)
            *out_len = 0;
        return HU_OK;
    }

    /* Render into a scratch buffer first so we can detect overflow before
     * partial-writing to the caller's buffer. ~256 chars is the design cap. */
    char scratch[512];
    int pos = 0;

    pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "[moment] ");

    /* Phase + time-since label. */
    const char *phase = phase_label(moment->phase_local);
    if (phase[0] != '\0')
        pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "%s your time. ", phase);

    /* When did they last write? */
    if (moment->time_since_their_last_msg_s > 0) {
        char gap[16];
        format_relative(moment->time_since_their_last_msg_s, gap, sizeof gap);
        pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "They wrote %s ago", gap);
        if (moment->their_avg_length_words > 0) {
            const char *t = tone_label(moment->their_recent_tone);
            pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, " — %d words%s%s",
                            moment->their_avg_length_words, t[0] ? ", " : "", t);
        }
        pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, ". ");
    }

    /* Thread state + topic. */
    if (moment->thread_is_continuation) {
        pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "Same thread");
        if (moment->topic_still_open && moment->topic_hint[0] != '\0')
            pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "; topic: \"%.127s\"",
                            moment->topic_hint);
        pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, ". ");
    }

    /* Opener cue + brevity cue. */
    pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "%s. %s.",
                    open_cue(moment->suggested_open), brevity_cue(moment->suggested_brevity));

    if (pos < 0 || (size_t)pos >= sizeof scratch)
        pos = (int)sizeof scratch - 1;

    /* The project has no dedicated buffer-too-small code; INVALID_ARGUMENT
     * is the closest semantic ("your cap is wrong for this input"). */
    if ((size_t)pos + 1 > buf_cap)
        return HU_ERR_INVALID_ARGUMENT;

    memcpy(buf, scratch, (size_t)pos);
    buf[pos] = '\0';
    if (out_len)
        *out_len = (size_t)pos;
    return HU_OK;
}

/* ── hu_moment_render_self_exemplars ─────────────────────────────────────── */

/* Truncate text at 80 chars for the exemplar line so single-line entries stay
 * readable. Returns the actual length written. */
static int render_one_exemplar(const hu_conversation_history_entry_t *e, int64_t now_s, char *buf,
                               size_t cap) {
    char gap[16];
    int64_t age = now_s - e->ts_s;
    if (age < 0)
        age = 0;
    format_relative(age, gap, sizeof gap);

    /* Take the first 80 chars of text (deliberately short — exemplars are
     * style anchors, not full quotes). Bound the snprintf input width
     * explicitly so GCC's -Wformat-truncation doesn't flag the
     * potential 511-byte e->text writing into a 96-byte buffer. */
    char clipped[96];
    int clipped_n = snprintf(clipped, sizeof clipped, "%.80s", e->text);
    if (clipped_n > 80) {
        clipped[80] = '\0';
        /* Trim trailing word break for readability. */
        for (int k = 79; k > 60; k--) {
            if (clipped[k] == ' ') {
                clipped[k] = '\0';
                break;
            }
        }
    }

    return snprintf(buf, cap, "you (%s ago): \"%s\"\n", gap, clipped);
}

/* Two strings considered "consecutive duplicates" for dedupe purposes. */
static bool same_text(const char *a, const char *b) {
    if (a == NULL || b == NULL)
        return false;
    return strcmp(a, b) == 0;
}

hu_error_t hu_moment_render_self_exemplars(const hu_moment_t *moment,
                                           const struct hu_conversation_history_t *history,
                                           size_t max_exemplars, char *buf, size_t buf_cap,
                                           size_t *out_len) {
    if (moment == NULL || history == NULL || buf == NULL || buf_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;

    buf[0] = '\0';
    if (out_len)
        *out_len = 0;

    if (history->count == 0 || max_exemplars == 0)
        return HU_OK;

    /* Walk newest→oldest, pick outbound that isn't a consecutive duplicate.
     * Collect indices into a small fixed array (cap at 16 — design max). */
    size_t indices[16];
    size_t n_indices = 0;
    if (max_exemplars > 16)
        max_exemplars = 16;

    const char *prev_text = NULL;
    for (size_t i = history->count; i-- > 0 && n_indices < max_exemplars;) {
        const hu_conversation_history_entry_t *e = &history->entries[i];
        if (e->outbound) {
            /* Skip system-generated entries — encode "system" by checking
             * for a leading "[system]" marker (simple, no extra fields). */
            if (strncmp(e->text, "[system]", 8) == 0)
                continue;
            /* Skip consecutive duplicates (newest already collected). */
            if (n_indices > 0 && same_text(prev_text, e->text))
                continue;
            indices[n_indices++] = i;
            prev_text = e->text;
        }
    }

    /* Variety sampling: if all picks fall within a 1-hour window AND there
     * are older outbound entries available, swap the second half of the picks
     * for older spread samples. */
    if (n_indices >= 2) {
        int64_t newest_ts = history->entries[indices[0]].ts_s;
        int64_t oldest_pick_ts = history->entries[indices[n_indices - 1]].ts_s;
        if (newest_ts - oldest_pick_ts < 3600 && n_indices > 1) {
            /* Find an older outbound entry to swap in. */
            for (size_t i = indices[n_indices - 1]; i-- > 0;) {
                if (history->entries[i].outbound &&
                    strncmp(history->entries[i].text, "[system]", 8) != 0) {
                    int64_t cand_age = newest_ts - history->entries[i].ts_s;
                    if (cand_age > 3600) {
                        indices[n_indices - 1] = i;
                        break;
                    }
                }
            }
        }
    }

    /* Render in original picked order (most-recent-first). */
    int64_t now_s = moment->composed_at_s;
    size_t pos = 0;
    for (size_t k = 0; k < n_indices; k++) {
        char line[256];
        int n = render_one_exemplar(&history->entries[indices[k]], now_s, line, sizeof line);
        if (n < 0)
            continue;
        if (pos + (size_t)n + 1 > buf_cap)
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(buf + pos, line, (size_t)n);
        pos += (size_t)n;
    }
    buf[pos] = '\0';
    if (out_len)
        *out_len = pos;
    return HU_OK;
}
