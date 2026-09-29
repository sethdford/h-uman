#include "human/context/voice_intent.h"
#include "human/core/string.h"
#include "human/util/typedstream.h"

#include <ctype.h>
#include <stdlib.h>

static bool any_word(const char *s, size_t n, const char *const *words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (hu_str_contains_word_ci_n(s, n, words[i]))
            return true;
    return false;
}

static size_t count_words(const char *s, size_t n) {
    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < n; i++) {
        bool w = !isspace((unsigned char)s[i]);
        if (w && !in_word)
            words++;
        in_word = w;
    }
    return words;
}

static bool ends_with_question(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    return n > 0 && s[n - 1] == '?';
}

/* A short "what time / where / when" question answers faster as text; a memo
 * that says "seven" is worse than the text "7". Only a short question counts:
 * "I cried when I heard" is not logistics (review I4). */
static bool is_logistics(const char *s, size_t n) {
    static const char *const w[] = {"what time", "where", "when", "address"};
    return ends_with_question(s, n) && count_words(s, n) <= 12 &&
           any_word(s, n, w, sizeof(w) / sizeof(w[0]));
}

/* Phrases, not bare "love"/"sorry": "I'd love to" and "sorry running late"
 * are everyday texts, not moments (review I4). */
static bool is_heartfelt(const char *s, size_t n) {
    static const char *const w[] = {
        "love you",    "miss you",    "i miss",      "proud of",        "so proud", "so sorry",
        "i'm sorry",   "im sorry",    "sorry about", "worried",         "scared",   "lonely",
        "heartbroken", "passed away", "congrats",    "congratulations", "crying",   "cried",
        "upset",       "sad",         "grateful"};
    return any_word(s, n, w, sizeof(w) / sizeof(w[0]));
}

/* Ends with '?' and long enough to be more than "you coming?". */
static bool is_real_question(const char *s, size_t n) {
    return ends_with_question(s, n) && count_words(s, n) >= 8;
}

bool hu_voice_intent_memo_shaped(const char *text, size_t len) {
    if (!text)
        return false;
    size_t words = count_words(text, len);
    return words >= 12 && words <= 110;
}

uint32_t hu_voice_intent_parse_gap(const char *env) {
    if (!env || !env[0])
        return 10800u;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 7 * 86400)
        return 10800u;
    return (uint32_t)v;
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
    if (f->min_gap_sec > 0 && f->secs_since_last_memo >= 0 &&
        f->secs_since_last_memo < (int64_t)f->min_gap_sec)
        DECIDE(HU_VOICE_SEND_TEXT, "spacing");
    if (is_heartfelt(s, n))
        DECIDE(HU_VOICE_SEND_VOICE, "heartfelt");
    if (is_logistics(s, n))
        DECIDE(HU_VOICE_SEND_TEXT, "logistics");
    if (is_real_question(s, n))
        DECIDE(HU_VOICE_SEND_VOICE, "question_worth_talking");
    DECIDE(HU_VOICE_SEND_TEXT, "no_trigger");
}
