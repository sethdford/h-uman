#include "human/moment.h"

#include "human/core/error.h"

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
            pos += snprintf(scratch + pos, sizeof scratch - (size_t)pos, "; topic: \"%s\"",
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
