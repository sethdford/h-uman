/*
 * Humanness layer — the imperfections, rhythms, and intuitions that make
 * interaction feel like talking to someone who cares.
 */

#define _GNU_SOURCE
#include "human/humanness.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static bool contains_any(const char *msg, size_t msg_len, const char *const *phrases,
                         size_t phrase_count) {
    for (size_t i = 0; i < phrase_count; i++) {
        if (memmem(msg, msg_len, phrases[i], strlen(phrases[i])))
            return true;
    }
    return false;
}

/* ── 1. Shared References ────────────────────────────────────────────────── */

hu_error_t hu_shared_references_find(hu_allocator_t *alloc, const char *contact_id,
                                     size_t contact_id_len, const char *current_msg,
                                     size_t current_msg_len, const char *memory_context,
                                     size_t memory_context_len, hu_shared_reference_t **out,
                                     size_t *out_count, size_t max_refs) {
    (void)contact_id;
    (void)contact_id_len;
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;
    if (!memory_context || memory_context_len == 0 || !current_msg || current_msg_len == 0)
        return HU_OK;
    if (max_refs == 0)
        max_refs = 3;

    /*
     * Scan memory context for recurring themes that overlap with the
     * current message. A "shared reference" is a past moment that
     * resonates with what the user is saying now.
     */
    static const char *temporal_markers[] = {
        "yesterday",     "last week",       "remember when", "that time",
        "you mentioned", "we talked about", "you said",
    };
    size_t marker_count = sizeof(temporal_markers) / sizeof(temporal_markers[0]);

    size_t found = 0;
    hu_shared_reference_t *refs =
        (hu_shared_reference_t *)alloc->alloc(alloc->ctx, sizeof(hu_shared_reference_t) * max_refs);
    if (!refs)
        return HU_ERR_OUT_OF_MEMORY;
    memset(refs, 0, sizeof(hu_shared_reference_t) * max_refs);

    /* Find sentences in memory that contain temporal markers */
    for (size_t m = 0; m < marker_count && found < max_refs; m++) {
        const char *marker = temporal_markers[m];
        size_t mlen = strlen(marker);
        const char *hit = memmem(memory_context, memory_context_len, marker, mlen);
        if (!hit)
            continue;

        /* Extract the surrounding sentence */
        const char *start = hit;
        while (start > memory_context && *(start - 1) != '.' && *(start - 1) != '\n')
            start--;
        const char *end = hit + mlen;
        while (end < memory_context + memory_context_len && *end != '.' && *end != '\n')
            end++;
        size_t ref_len = (size_t)(end - start);
        if (ref_len < 10 || ref_len > 256)
            continue;

        /* Check if current message has any word overlap with this reference */
        bool has_overlap = false;
        for (size_t ci = 0; ci + 4 <= current_msg_len; ci++) {
            if (current_msg[ci] == ' ' || ci == 0) {
                size_t wstart = (current_msg[ci] == ' ') ? ci + 1 : ci;
                size_t wend = wstart;
                while (wend < current_msg_len && current_msg[wend] != ' ')
                    wend++;
                size_t wlen = wend - wstart;
                if (wlen >= 4 && memmem(start, ref_len, current_msg + wstart, wlen)) {
                    has_overlap = true;
                    break;
                }
            }
        }
        if (!has_overlap)
            continue;

        refs[found].reference = hu_strndup(alloc, start, ref_len);
        refs[found].reference_len = ref_len;
        refs[found].original_context = hu_strndup(alloc, start, ref_len);
        refs[found].original_context_len = ref_len;
        refs[found].recency = 0.5;
        refs[found].emotional_weight = 0.5;
        refs[found].occurred_at = 0;
        found++;
    }

    if (found == 0) {
        alloc->free(alloc->ctx, refs, sizeof(hu_shared_reference_t) * max_refs);
        return HU_OK;
    }

    /* Shrink allocation to actual count for clean accounting in free */
    if (found < max_refs) {
        hu_shared_reference_t *shrunk = (hu_shared_reference_t *)alloc->alloc(
            alloc->ctx, sizeof(hu_shared_reference_t) * found);
        if (shrunk) {
            memcpy(shrunk, refs, sizeof(hu_shared_reference_t) * found);
            alloc->free(alloc->ctx, refs, sizeof(hu_shared_reference_t) * max_refs);
            refs = shrunk;
        }
    }

    *out = refs;
    *out_count = found;
    return HU_OK;
}

char *hu_shared_references_build_directive(hu_allocator_t *alloc, const hu_shared_reference_t *refs,
                                           size_t count, size_t *out_len) {
    if (!alloc || !refs || count == 0)
        return NULL;

    char buf[2048];
    size_t pos =
        hu_buf_appendf(buf, sizeof(buf), 0,
                       "You share history with this person. "
                       "If naturally relevant, weave in brief callbacks to past moments — "
                       "not as explicit reminders, but as the shorthand that develops between "
                       "people who know each other. Possible references:\n");

    for (size_t i = 0; i < count && pos < sizeof(buf) - 200; i++) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- \"%.*s\"\n",
                             (int)(refs[i].reference_len > 120 ? 120 : refs[i].reference_len),
                             refs[i].reference);
    }
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "Use these sparingly. A single natural callback is better than "
                         "multiple forced references.");

    size_t len = pos;
    char *result = hu_strndup(alloc, buf, len);
    if (out_len)
        *out_len = len;
    return result;
}

void hu_shared_references_free(hu_allocator_t *alloc, hu_shared_reference_t *refs, size_t count) {
    if (!alloc || !refs)
        return;
    for (size_t i = 0; i < count; i++) {
        if (refs[i].reference)
            alloc->free(alloc->ctx, refs[i].reference, refs[i].reference_len + 1);
        if (refs[i].original_context)
            alloc->free(alloc->ctx, refs[i].original_context, refs[i].original_context_len + 1);
    }
    /* We allocated max_refs initially but only count is valid — free full block */
    alloc->free(alloc->ctx, refs, sizeof(hu_shared_reference_t) * count);
}

/* ── 2. Emotional Pacing ─────────────────────────────────────────────────── */

hu_emotional_weight_t hu_emotional_weight_classify(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return HU_WEIGHT_LIGHT;

    static const char *grief_phrases[] = {
        "died", "passed away", "lost my", "funeral", "cancer", "suicide", "terminal", "miscarriage",
    };
    static const char *heavy_phrases[] = {
        "depressed",     "anxious",       "panic",       "scared",     "lonely",
        "heartbroken",   "devastated",    "overwhelmed", "can't cope", "don't know what to do",
        "breaking down", "falling apart", "hurting",     "struggling", "help me",
    };
    static const char *normal_phrases[] = {
        "how do i", "can you",  "what is", "explain", "write",
        "create",   "generate", "update",  "fix",     "build",
    };

    if (contains_any(msg, msg_len, grief_phrases, sizeof(grief_phrases) / sizeof(grief_phrases[0])))
        return HU_WEIGHT_GRIEF;
    if (contains_any(msg, msg_len, heavy_phrases, sizeof(heavy_phrases) / sizeof(heavy_phrases[0])))
        return HU_WEIGHT_HEAVY;
    if (contains_any(msg, msg_len, normal_phrases,
                     sizeof(normal_phrases) / sizeof(normal_phrases[0])))
        return HU_WEIGHT_NORMAL;
    return HU_WEIGHT_LIGHT;
}

uint64_t hu_emotional_pacing_adjust(uint64_t base_delay_ms, hu_emotional_weight_t weight) {
    switch (weight) {
    case HU_WEIGHT_LIGHT:
        return base_delay_ms;
    case HU_WEIGHT_NORMAL:
        return base_delay_ms;
    case HU_WEIGHT_HEAVY:
        return base_delay_ms + 3000; /* 3s pause — taking it in */
    case HU_WEIGHT_GRIEF:
        return base_delay_ms + 6000; /* 6s pause — sitting with it */
    }
    return base_delay_ms;
}

/* ── 3. Silence Intuition ────────────────────────────────────────────────── */

hu_silence_response_t hu_silence_intuit(const char *msg, size_t msg_len,
                                        hu_emotional_weight_t weight, uint32_t conversation_depth,
                                        bool user_explicitly_asked) {
    if (user_explicitly_asked)
        return HU_SILENCE_FULL_RESPONSE;

    if (!msg || msg_len == 0)
        return HU_SILENCE_ACTUAL_SILENCE;

    /* Grief with no explicit question — just be present */
    if (weight == HU_WEIGHT_GRIEF) {
        bool has_question = false;
        for (size_t i = 0; i < msg_len; i++) {
            if (msg[i] == '?') {
                has_question = true;
                break;
            }
        }
        if (!has_question)
            return HU_SILENCE_PRESENCE_ONLY;
    }

    /* Heavy emotion, short message, no question — brief acknowledgment */
    if (weight == HU_WEIGHT_HEAVY && msg_len < 80) {
        bool has_question = false;
        for (size_t i = 0; i < msg_len; i++) {
            if (msg[i] == '?') {
                has_question = true;
                break;
            }
        }
        if (!has_question)
            return HU_SILENCE_BRIEF_ACKNOWLEDGE;
    }

    /* Very short messages deep in conversation — might be just venting */
    static const char *vent_phrases[] = {
        "ugh", "sigh", "whatever", "i guess", "idk", "meh", "same", "mood", "yep", "nope",
    };
    if (msg_len < 20 && conversation_depth > 5 &&
        contains_any(msg, msg_len, vent_phrases, sizeof(vent_phrases) / sizeof(vent_phrases[0])))
        return HU_SILENCE_BRIEF_ACKNOWLEDGE;

    return HU_SILENCE_FULL_RESPONSE;
}

bool hu_proactive_should_suppress_for_emotion(const char *last_inbound_msg, size_t msg_len) {
    /* No inbound history → let other gates decide; do not suppress. */
    if (!last_inbound_msg || msg_len == 0)
        return false;
    hu_emotional_weight_t w = hu_emotional_weight_classify(last_inbound_msg, msg_len);
    return w == HU_WEIGHT_HEAVY || w == HU_WEIGHT_GRIEF;
}

char *hu_silence_build_acknowledgment(hu_allocator_t *alloc, hu_silence_response_t response,
                                      size_t *out_len) {
    if (response == HU_SILENCE_FULL_RESPONSE)
        return NULL;

    const char *text;
    switch (response) {
    case HU_SILENCE_PRESENCE_ONLY:
        text = "I'm here.";
        break;
    case HU_SILENCE_BRIEF_ACKNOWLEDGE:
        text = "I hear you.";
        break;
    case HU_SILENCE_ACTUAL_SILENCE:
        return NULL;
    default:
        return NULL;
    }

    size_t len = strlen(text);
    char *result = hu_strndup(alloc, text, len);
    if (out_len)
        *out_len = len;
    return result;
}

/* ── 4. Emotional Residue Carryover ──────────────────────────────────────── */

hu_error_t hu_residue_carryover_compute(const double *valences, const double *intensities,
                                        const int64_t *timestamps, size_t count, int64_t now_ts,
                                        hu_residue_carryover_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (!valences || !intensities || !timestamps || count == 0)
        return HU_OK;

    double total_valence = 0;
    double total_intensity = 0;
    int64_t most_recent = 0;

    for (size_t i = 0; i < count; i++) {
        int64_t age_hours = (now_ts - timestamps[i]) / 3600;
        if (age_hours > 72)
            continue; /* Only carry forward 3 days */

        double decay = 1.0 / (1.0 + (double)age_hours / 12.0);
        total_valence += valences[i] * decay;
        total_intensity += intensities[i] * decay;

        if (valences[i] < -0.5 && intensities[i] > 0.6)
            out->had_grief = true;
        if (valences[i] < -0.3 && valences[i] > -0.7)
            out->had_conflict = true;
        if (intensities[i] > 0.7)
            out->had_vulnerability = true;
        if (timestamps[i] > most_recent)
            most_recent = timestamps[i];
    }

    out->net_valence = total_valence / (double)count;
    out->intensity = total_intensity / (double)count;
    if (out->net_valence < -1.0)
        out->net_valence = -1.0;
    if (out->net_valence > 1.0)
        out->net_valence = 1.0;
    if (out->intensity > 1.0)
        out->intensity = 1.0;
    out->hours_since = most_recent > 0 ? (now_ts - most_recent) / 3600 : 0;

    return HU_OK;
}

char *hu_residue_carryover_build_directive(hu_allocator_t *alloc, const hu_residue_carryover_t *c,
                                           size_t *out_len) {
    if (!alloc || !c)
        return NULL;

    /* No carryover needed if residue is light or ancient */
    if (c->intensity < 0.2 || c->hours_since > 48)
        return NULL;

    char buf[1024];
    int pos = 0;

    if (c->had_grief) {
        pos = snprintf(buf, sizeof(buf),
                       "Your last conversation with this person involved grief or loss. "
                       "Open gently — a lighter touch, a quiet check-in. Don't pretend "
                       "the previous conversation didn't happen, but don't force them to "
                       "revisit it either. Something like acknowledging \"yesterday was a lot\" "
                       "without being clinical about it.");
    } else if (c->had_vulnerability) {
        pos = snprintf(buf, sizeof(buf),
                       "This person was vulnerable with you recently (%.0f hours ago). "
                       "Start warmer than usual. They took a risk by opening up — "
                       "honor that by being a little gentler in your opening.",
                       (double)c->hours_since);
    } else if (c->had_conflict) {
        pos = snprintf(buf, sizeof(buf),
                       "There was tension in your last conversation. Don't pretend it "
                       "didn't happen, but don't lead with it. Be steady and warm — "
                       "the kind of person who doesn't hold grudges.");
    } else if (c->net_valence < -0.3) {
        pos = snprintf(buf, sizeof(buf),
                       "Recent conversations have been heavy. Be a little lighter "
                       "today if the moment allows it — but don't force cheerfulness. "
                       "Read their tone and match it.");
    } else {
        return NULL;
    }

    size_t len = (size_t)pos;
    char *result = hu_strndup(alloc, buf, len);
    if (out_len)
        *out_len = len;
    return result;
}

/* ── 5. Curiosity Engine ─────────────────────────────────────────────────── */

hu_error_t hu_curiosity_generate(hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, const char *memory_context,
                                 size_t memory_context_len, const char *current_msg,
                                 size_t current_msg_len, hu_curiosity_prompt_t **out,
                                 size_t *out_count, size_t max_prompts) {
    (void)contact_id;
    (void)contact_id_len;
    (void)current_msg;
    (void)current_msg_len;
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;
    if (!memory_context || memory_context_len == 0)
        return HU_OK;
    if (max_prompts == 0)
        max_prompts = 2;

    /*
     * Look for topics the user mentioned that could prompt genuine
     * curiosity — things with follow-up potential.
     */
    static const char *curiosity_triggers[] = {
        "learning",   "starting", "trying", "planning", "thinking about", "considering",
        "working on", "new job",  "moving", "trip",     "project",
    };
    size_t trigger_count = sizeof(curiosity_triggers) / sizeof(curiosity_triggers[0]);

    hu_curiosity_prompt_t *prompts = (hu_curiosity_prompt_t *)alloc->alloc(
        alloc->ctx, sizeof(hu_curiosity_prompt_t) * max_prompts);
    if (!prompts)
        return HU_ERR_OUT_OF_MEMORY;
    memset(prompts, 0, sizeof(hu_curiosity_prompt_t) * max_prompts);

    size_t found = 0;
    for (size_t t = 0; t < trigger_count && found < max_prompts; t++) {
        const char *trigger = curiosity_triggers[t];
        size_t tlen = strlen(trigger);
        const char *hit = memmem(memory_context, memory_context_len, trigger, tlen);
        if (!hit)
            continue;

        /* Extract surrounding context */
        const char *start = hit;
        while (start > memory_context && *(start - 1) != '.' && *(start - 1) != '\n')
            start--;
        const char *end = hit + tlen;
        while (end < memory_context + memory_context_len && *end != '.' && *end != '\n')
            end++;
        size_t ctx_len = (size_t)(end - start);
        if (ctx_len < 8 || ctx_len > 200)
            continue;

        char question[256];
        int qlen = snprintf(question, sizeof(question), "How is the %.*s going?",
                            (int)(ctx_len > 60 ? 60 : ctx_len), start);
        if (qlen <= 0)
            continue;

        prompts[found].question = hu_strndup(alloc, question, (size_t)qlen);
        prompts[found].question_len = (size_t)qlen;
        prompts[found].reason = hu_strndup(alloc, start, ctx_len);
        prompts[found].reason_len = ctx_len;
        prompts[found].relevance = 0.6;
        found++;
    }

    if (found == 0) {
        alloc->free(alloc->ctx, prompts, sizeof(hu_curiosity_prompt_t) * max_prompts);
        return HU_OK;
    }

    if (found < max_prompts) {
        hu_curiosity_prompt_t *shrunk = (hu_curiosity_prompt_t *)alloc->alloc(
            alloc->ctx, sizeof(hu_curiosity_prompt_t) * found);
        if (shrunk) {
            memcpy(shrunk, prompts, sizeof(hu_curiosity_prompt_t) * found);
            alloc->free(alloc->ctx, prompts, sizeof(hu_curiosity_prompt_t) * max_prompts);
            prompts = shrunk;
        }
    }

    *out = prompts;
    *out_count = found;
    return HU_OK;
}

char *hu_curiosity_build_directive(hu_allocator_t *alloc, const hu_curiosity_prompt_t *prompts,
                                   size_t count, size_t *out_len) {
    if (!alloc || !prompts || count == 0)
        return NULL;

    char buf[1024];
    size_t pos = hu_buf_appendf(buf, sizeof(buf), 0,
                                "You're genuinely curious about something from their past. "
                                "If there's a natural opening, ask — not as a scripted check-in "
                                "but because you actually want to know:\n");

    for (size_t i = 0; i < count && pos < sizeof(buf) - 200; i++) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- %.*s\n", (int)prompts[i].question_len,
                             prompts[i].question);
    }
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "Only ask if the conversation naturally leads there. Never force it.");

    size_t len = pos;
    char *result = hu_strndup(alloc, buf, len);
    if (out_len)
        *out_len = len;
    return result;
}

void hu_curiosity_prompts_free(hu_allocator_t *alloc, hu_curiosity_prompt_t *prompts,
                               size_t count) {
    if (!alloc || !prompts)
        return;
    for (size_t i = 0; i < count; i++) {
        if (prompts[i].question)
            alloc->free(alloc->ctx, prompts[i].question, prompts[i].question_len + 1);
        if (prompts[i].reason)
            alloc->free(alloc->ctx, prompts[i].reason, prompts[i].reason_len + 1);
    }
    alloc->free(alloc->ctx, prompts, sizeof(hu_curiosity_prompt_t) * count);
}

/* ── 6. Unasked Question Detector ────────────────────────────────────────── */

typedef struct {
    const char *topic_signal;
    const char *missing_signal;
    const char *missing_description;
} absence_pattern_t;

static const absence_pattern_t s_absence_patterns[] = {
    {"presentation", "felt", "how they felt about the presentation"},
    {"interview", "felt", "how they felt about the interview"},
    {"meeting", "think", "what they thought about the meeting"},
    {"diagnosis", "feel", "how they're feeling about the diagnosis"},
    {"breakup", "doing", "how they're doing after the breakup"},
    {"promotion", "feel", "how they feel about the promotion"},
    {"moved", "like", "how they like the new place"},
    {"started", "going", "how it's going"},
    {"result", "feel", "how they feel about the result"},
    {"decision", "feel", "how they feel about the decision"},
};

hu_error_t hu_absence_detect(hu_allocator_t *alloc, const char *msg, size_t msg_len,
                             hu_absence_signal_t **out, size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;
    if (!msg || msg_len < 15)
        return HU_OK;

    size_t pattern_count = sizeof(s_absence_patterns) / sizeof(s_absence_patterns[0]);
    hu_absence_signal_t *signals = NULL;
    size_t found = 0;
    size_t cap = 3;

    for (size_t p = 0; p < pattern_count; p++) {
        const char *topic = s_absence_patterns[p].topic_signal;
        const char *missing = s_absence_patterns[p].missing_signal;

        bool has_topic = memmem(msg, msg_len, topic, strlen(topic)) != NULL;
        bool has_missing = memmem(msg, msg_len, missing, strlen(missing)) != NULL;

        if (has_topic && !has_missing) {
            if (!signals) {
                signals = (hu_absence_signal_t *)alloc->alloc(alloc->ctx,
                                                              sizeof(hu_absence_signal_t) * cap);
                if (!signals)
                    return HU_ERR_OUT_OF_MEMORY;
                memset(signals, 0, sizeof(hu_absence_signal_t) * cap);
            }
            if (found >= cap)
                break;

            signals[found].topic = hu_strndup(alloc, topic, strlen(topic));
            signals[found].topic_len = strlen(topic);
            signals[found].missing_aspect =
                hu_strndup(alloc, s_absence_patterns[p].missing_description,
                           strlen(s_absence_patterns[p].missing_description));
            signals[found].missing_aspect_len = strlen(s_absence_patterns[p].missing_description);
            signals[found].confidence = 0.6;
            found++;
        }
    }

    *out = signals;
    *out_count = found;
    return HU_OK;
}

char *hu_absence_build_directive(hu_allocator_t *alloc, const hu_absence_signal_t *signals,
                                 size_t count, size_t *out_len) {
    if (!alloc || !signals || count == 0)
        return NULL;

    char buf[1024];
    size_t pos = hu_buf_appendf(buf, sizeof(buf), 0,
                                "Notice what they didn't say. They described an outcome but "
                                "didn't share how they feel about it. If appropriate, gently "
                                "ask about the emotional side — not clinically, but the way "
                                "a close friend would notice the gap:\n");

    for (size_t i = 0; i < count && pos < sizeof(buf) - 200; i++) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos,
                             "- They mentioned \"%.*s\" but didn't address %.*s\n",
                             (int)signals[i].topic_len, signals[i].topic,
                             (int)signals[i].missing_aspect_len, signals[i].missing_aspect);
    }

    size_t len = pos;
    char *result = hu_strndup(alloc, buf, len);
    if (out_len)
        *out_len = len;
    return result;
}

void hu_absence_signals_free(hu_allocator_t *alloc, hu_absence_signal_t *signals, size_t count) {
    if (!alloc || !signals)
        return;
    for (size_t i = 0; i < count; i++) {
        if (signals[i].topic)
            alloc->free(alloc->ctx, signals[i].topic, signals[i].topic_len + 1);
        if (signals[i].missing_aspect)
            alloc->free(alloc->ctx, signals[i].missing_aspect, signals[i].missing_aspect_len + 1);
    }
    alloc->free(alloc->ctx, signals, sizeof(hu_absence_signal_t) * count);
}

/* ── 7. Evolving Opinion ─────────────────────────────────────────────────── */

char *hu_evolved_opinion_build_directive(hu_allocator_t *alloc,
                                         const hu_evolved_opinion_t *opinions, size_t count,
                                         double min_conviction, size_t *out_len) {
    if (!alloc || !opinions || count == 0)
        return NULL;

    size_t qualifying = 0;
    for (size_t i = 0; i < count; i++) {
        if (opinions[i].conviction >= min_conviction)
            qualifying++;
    }
    if (qualifying == 0)
        return NULL;

    char buf[2048];
    size_t pos =
        hu_buf_appendf(buf, sizeof(buf), 0,
                       "Over time, you've developed genuine perspectives on certain topics "
                       "through repeated conversation. These aren't reflexive agreement — "
                       "they're positions you've arrived at through experience. "
                       "Share them when relevant, even if the user might disagree:\n");

    for (size_t i = 0; i < count && pos < sizeof(buf) - 300; i++) {
        if (opinions[i].conviction < min_conviction)
            continue;
        const char *firmness = opinions[i].conviction > 0.8   ? "firmly"
                               : opinions[i].conviction > 0.5 ? "moderately"
                                                              : "tentatively";
        pos =
            hu_buf_appendf(buf, sizeof(buf), pos,
                           "- On \"%.*s\": you %s believe \"%.*s\" (shaped by %u conversations)\n",
                           (int)opinions[i].topic_len, opinions[i].topic, firmness,
                           (int)(opinions[i].stance_len > 100 ? 100 : opinions[i].stance_len),
                           opinions[i].stance, opinions[i].interactions);
    }
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "Express these naturally — not as pronouncements, but as a thoughtful "
                         "person sharing their honest take. Be open to being persuaded otherwise.");

    size_t len = pos;
    char *result = hu_strndup(alloc, buf, len);
    if (out_len)
        *out_len = len;
    return result;
}

/* ── 8. Imperfect Delivery ───────────────────────────────────────────────── */

hu_certainty_level_t hu_certainty_classify(const char *msg, size_t msg_len, bool has_memory_context,
                                           uint32_t tool_results_count) {
    if (!msg || msg_len == 0)
        return HU_CERTAIN;

    /* Tool results increase certainty */
    if (tool_results_count > 0)
        return HU_CERTAIN;

    /* Memory context increases certainty */
    if (has_memory_context)
        return HU_MOSTLY_SURE;

    static const char *uncertain_domains[] = {
        "should i",    "what do you think", "opinion",         "would you",
        "best way to", "which is better",   "how do you feel", "advice",
    };
    if (contains_any(msg, msg_len, uncertain_domains,
                     sizeof(uncertain_domains) / sizeof(uncertain_domains[0])))
        return HU_UNCERTAIN;

    static const char *unsure_domains[] = {
        "meaning of life", "future", "predict", "will i", "what happens when", "philosophy",
    };
    if (contains_any(msg, msg_len, unsure_domains,
                     sizeof(unsure_domains) / sizeof(unsure_domains[0])))
        return HU_GENUINELY_UNSURE;

    return HU_MOSTLY_SURE;
}

char *hu_imperfect_delivery_directive(hu_allocator_t *alloc, hu_certainty_level_t level,
                                      size_t *out_len) {
    if (!alloc)
        return NULL;

    const char *text;
    switch (level) {
    case HU_CERTAIN:
        return NULL;
    case HU_MOSTLY_SURE:
        text = "You're fairly confident in your response, but leave room for "
               "nuance. Don't hedge excessively — just be honest about what "
               "you know vs what you're reasoning through.";
        break;
    case HU_UNCERTAIN:
        text = "Be genuinely transparent about uncertainty here. Don't fake "
               "confidence. It's okay to say \"I think...\" or \"my sense is...\" "
               "or even \"I'm not sure, but here's how I'd think about it.\" "
               "Authentic uncertainty is more trustworthy than false precision.";
        break;
    case HU_GENUINELY_UNSURE:
        text = "You genuinely don't know the answer to this — and that's okay. "
               "Say so honestly. Share what you can reason through, but don't "
               "pretend to have certainty you don't have. \"Honestly, I don't know\" "
               "followed by thoughtful exploration is more human than a confident "
               "wrong answer.";
        break;
    default:
        return NULL;
    }

    size_t len = strlen(text);
    char *result = hu_strndup(alloc, text, len);
    if (out_len)
        *out_len = len;
    return result;
}
