#include "human/context/voice_intent.h"
#include "human/core/string.h"
#include "human/util/typedstream.h"

#include <ctype.h>

static bool any_word(const char *s, size_t n, const char *const *words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (hu_str_contains_word_ci_n(s, n, words[i]))
            return true;
    return false;
}

/* Quick "where/when" questions answer faster as text; a memo that says
 * "seven" is worse than the text "7". */
static bool is_logistics(const char *s, size_t n) {
    static const char *const w[] = {"what time", "where", "when", "address"};
    return any_word(s, n, w, sizeof(w) / sizeof(w[0]));
}

static bool is_heartfelt(const char *s, size_t n) {
    static const char *const w[] = {"love",     "miss",     "proud",       "sorry",      "worried",
                                    "sad",      "upset",    "crying",      "lonely",     "scared",
                                    "grateful", "congrats", "heartbroken", "passed away"};
    return any_word(s, n, w, sizeof(w) / sizeof(w[0]));
}

/* Ends with '?' and long enough to be more than "you coming?". */
static bool is_real_question(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n == 0 || s[n - 1] != '?')
        return false;
    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < n; i++) {
        bool w = !isspace((unsigned char)s[i]);
        if (w && !in_word)
            words++;
        in_word = w;
    }
    return words >= 8;
}

#define DECIDE(d, why)           \
    do {                         \
        if (out_reason)          \
            *out_reason = (why); \
        return (d);              \
    } while (0)

hu_voice_decision_t hu_voice_intent_decide(const hu_voice_intent_facts_t *f,
                                           const char **out_reason) {
    if (!f || !f->has_voice_id)
        DECIDE(HU_VOICE_SEND_TEXT, "no_voice_id");
    if (!f->cfg || !f->cfg->enabled)
        DECIDE(HU_VOICE_SEND_TEXT, "disabled");
    const char *s = f->inbound ? f->inbound : "";
    size_t n = f->inbound ? f->inbound_len : 0;
    /* Answering audio with audio is reciprocity, so spacing does not apply. */
    if (hu_text_has_audio_transcription(s, n))
        DECIDE(HU_VOICE_SEND_VOICE, "they_sent_audio");
    if (is_logistics(s, n))
        DECIDE(HU_VOICE_SEND_TEXT, "logistics");
    if (f->min_gap_sec > 0 && f->secs_since_last_memo >= 0 &&
        f->secs_since_last_memo < (int64_t)f->min_gap_sec)
        DECIDE(HU_VOICE_SEND_TEXT, "spacing");
    if (is_heartfelt(s, n))
        DECIDE(HU_VOICE_SEND_VOICE, "heartfelt");
    if (is_real_question(s, n))
        DECIDE(HU_VOICE_SEND_VOICE, "question_worth_talking");
    DECIDE(HU_VOICE_SEND_TEXT, "no_trigger");
}
