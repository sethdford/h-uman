/* Hard-moment note. Contract: include/human/agent/hard_moment.h. */
#include "human/agent/hard_moment.h"
#include "human/behavior/affect.h"
#include "human/core/log.h"
#include <string.h>

/* Low mood: clearly negative but not agitated. The distress predicate needs
 * arousal > 0.5, which every sadness word in the lexicon sits below. */
#define HU_HARD_MOMENT_LOW_VALENCE     (-0.5f)
#define HU_HARD_MOMENT_LOW_AROUSAL_MAX 0.5f
#define HU_HARD_MOMENT_MAX_UNCERTAINTY 0.85f

/* Written to sit inside the persona's anti-patterns: no support-agent phrases
 * ("I'm here for you", "I understand"), no pep talk, no unsolicited fixing. */
static const char k_note_distress[] =
    "\n\n[Right now they sound stressed or scared. Slow down and meet it: name the "
    "specific thing they said, show you're with them in your own words, and hold off "
    "on advice unless they ask. One gentle question is fine.]";
static const char k_note_low_mood[] =
    "\n\n[Right now they sound down. Warmth first, keep it real and specific, not a "
    "pep talk. Check in on how they're doing with it.]";

hu_hard_moment_kind_t hu_hard_moment_classify(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return HU_HARD_MOMENT_NONE;
    hu_affect_state_t s;
    if (hu_affect_estimate_text(msg, msg_len, &s) != HU_OK)
        return HU_HARD_MOMENT_NONE;
    if (hu_affect_is_distress(&s))
        return HU_HARD_MOMENT_DISTRESS;
    if (s.uncertainty <= HU_HARD_MOMENT_MAX_UNCERTAINTY && s.valence < HU_HARD_MOMENT_LOW_VALENCE &&
        s.arousal <= HU_HARD_MOMENT_LOW_AROUSAL_MAX)
        return HU_HARD_MOMENT_LOW_MOOD;
    return HU_HARD_MOMENT_NONE;
}

const char *hu_hard_moment_note(hu_hard_moment_kind_t kind) {
    switch (kind) {
    case HU_HARD_MOMENT_DISTRESS:
        return k_note_distress;
    case HU_HARD_MOMENT_LOW_MOOD:
        return k_note_low_mood;
    case HU_HARD_MOMENT_NONE:
    default:
        return NULL;
    }
}

const char *hu_hard_moment_kind_name(hu_hard_moment_kind_t kind) {
    switch (kind) {
    case HU_HARD_MOMENT_DISTRESS:
        return "distress";
    case HU_HARD_MOMENT_LOW_MOOD:
        return "low_mood";
    case HU_HARD_MOMENT_NONE:
    default:
        return "none";
    }
}

hu_gate_mode_t hu_hard_moment_mode(void) {
    return hu_gate_mode_from_env("HU_HARD_MOMENT", HU_GATE_OFF);
}

hu_hard_moment_kind_t hu_hard_moment_apply(hu_allocator_t *alloc, hu_gate_mode_t mode,
                                           const char *msg, size_t msg_len, char **prompt,
                                           size_t *prompt_len) {
    if (mode == HU_GATE_OFF || !alloc || !prompt || !*prompt || !prompt_len)
        return HU_HARD_MOMENT_NONE;
    hu_hard_moment_kind_t kind = hu_hard_moment_classify(msg, msg_len);
    if (kind == HU_HARD_MOMENT_NONE)
        return kind;
    if (mode == HU_GATE_SHADOW) {
        /* Kind + length only: shadow telemetry must never copy message text. */
        hu_log_info("hard_moment", NULL, "shadow: would add %s note (msg %zu B)",
                    hu_hard_moment_kind_name(kind), msg_len);
        return kind;
    }
    const char *note = hu_hard_moment_note(kind);
    size_t note_len = strlen(note);
    size_t new_len = *prompt_len + note_len;
    char *grown = (char *)alloc->realloc(alloc->ctx, *prompt, *prompt_len + 1, new_len + 1);
    if (!grown)
        return kind; /* prompt unchanged: the note is an enhancement, never a failure */
    memcpy(grown + *prompt_len, note, note_len + 1);
    *prompt = grown;
    *prompt_len = new_len;
    return kind;
}
