#include "human/context/voice_triggers.h"
#include "human/context/voice_intent.h"
#include "human/core/string.h"

#include <stdlib.h>
#include <strings.h>

/* Past-tense and feeling words that make two sentences a story rather than a
 * plan. Whole words only ("washed" is not "was", "this morning" is a phrase,
 * bare "morning" is not). */
static bool has_narrative_marker(const char *s, size_t n) {
    static const char *const w[] = {
        "was",       "were",       "had",          "went",    "got",      "told",
        "said",      "felt",       "saw",          "came",    "happened", "found",
        "yesterday", "last night", "this morning", "nervous", "excited",  "stressed",
        "exhausted", "frustrated", "annoyed",      "happy",   "amazing",  "awful",
        "terrible",  "rough",      "hard day",     "crazy",   "relieved", "overwhelmed"};
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++)
        if (hu_str_contains_word_ci_n(s, n, w[i]))
            return true;
    return false;
}

/* Sentences of at least HU_VOICE_V2_STORY_SENTENCE_MIN_WORDS words, split on
 * . ! ? and newlines ("lol." and "ok!" are not sentences). */
static size_t count_sentences(const char *s, size_t n) {
    size_t sentences = 0, start = 0;
    for (size_t i = 0; i <= n; i++) {
        bool end = i == n || s[i] == '.' || s[i] == '!' || s[i] == '?' || s[i] == '\n';
        if (!end)
            continue;
        if (hu_voice_intent_word_count(s + start, i - start) >=
            HU_VOICE_V2_STORY_SENTENCE_MIN_WORDS)
            sentences++;
        start = i + 1;
    }
    return sentences;
}

static bool has_link(const char *s, size_t n) {
    return hu_str_contains_ci_cstr(s, n, "http://") || hu_str_contains_ci_cstr(s, n, "https://") ||
           hu_str_contains_ci_cstr(s, n, "www.");
}

bool hu_voice_v2_story_inbound(const char *s, size_t n) {
    if (!s || n == 0)
        return false;
    if (n >= HU_VOICE_V2_STORY_LONG_CHARS && !has_link(s, n))
        return true;
    return n >= HU_VOICE_V2_STORY_NARRATIVE_CHARS &&
           count_sentences(s, n) >= HU_VOICE_V2_STORY_MIN_SENTENCES && has_narrative_marker(s, n);
}

bool hu_voice_v2_memo_length_reply(uint32_t planned_reply_chars) {
    return planned_reply_chars >= HU_VOICE_V2_MEMO_PLANNED_CHARS;
}

bool hu_voice_v2_late_evening_warmth(int local_minute, bool close_contact, const char *s,
                                     size_t n) {
    if (!close_contact || local_minute < HU_VOICE_V2_EVENING_START_MIN ||
        local_minute > HU_VOICE_V2_EVENING_END_MIN || !s)
        return false;
    return hu_voice_intent_word_count(s, n) >= HU_VOICE_V2_EVENING_MIN_WORDS &&
           !hu_voice_intent_is_logistics(s, n);
}

bool hu_voice_v2_long_gap_reconnect(int64_t secs_since_owner_reply, bool close_contact) {
    return close_contact && secs_since_owner_reply >= HU_VOICE_V2_RECONNECT_GAP_SEC;
}

static bool value_is(const char *v, const char *want) {
    return v && strcasecmp(v, want) == 0;
}

bool hu_voice_v2_close_contact(const hu_contact_profile_t *cp) {
    if (!cp)
        return false;
    return value_is(cp->dunbar_layer, "intimate") || value_is(cp->dunbar_layer, "close") ||
           value_is(cp->relationship_type, "family") || value_is(cp->relationship_type, "romantic");
}

uint32_t hu_voice_v2_parse_weekly_cap(const char *env) {
    if (!env || !env[0])
        return HU_VOICE_V2_WEEKLY_CAP_DEFAULT;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 14)
        return HU_VOICE_V2_WEEKLY_CAP_DEFAULT;
    return (uint32_t)v;
}

static const char *v2_reason(const hu_voice_v2_facts_t *f) {
    if (hu_voice_v2_story_inbound(f->inbound, f->inbound_len))
        return "story_inbound";
    if (hu_voice_v2_long_gap_reconnect(f->secs_since_owner_reply, f->close_contact))
        return "long_gap_reconnect";
    if (hu_voice_v2_late_evening_warmth(f->local_minute, f->close_contact, f->inbound,
                                        f->inbound_len))
        return "late_evening_warmth";
    if (hu_voice_v2_memo_length_reply(f->planned_reply_chars))
        return "memo_length_reply";
    return NULL;
}

bool hu_voice_v2_decide(const hu_voice_v2_facts_t *f, const char **out_reason) {
    const char *why = f ? v2_reason(f) : NULL;
    bool voice = why != NULL;
    if (voice && f->v2_memos_this_week >= f->weekly_cap) {
        why = "weekly_cap";
        voice = false;
    }
    if (out_reason)
        *out_reason = why ? why : "none";
    return voice;
}
