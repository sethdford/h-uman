#ifndef SC_CONTEXT_CONVERSATION_H
#define SC_CONTEXT_CONVERSATION_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Build a complete conversation awareness context string from channel history.
 * Includes: conversation thread, emotional analysis, verbosity mirroring,
 * conversation phase, time-of-day triggers, detected user states.
 * Caller owns returned string. Returns NULL if no entries. */
char *sc_conversation_build_awareness(sc_allocator_t *alloc,
                                      const sc_channel_history_entry_t *entries, size_t count,
                                      size_t *out_len);

/* Conversation quality score (0-100). Evaluates a draft response for:
 * brevity, validation/reflection, repair/rephrase, and follow-up.
 * Returns a quality score and writes guidance into *feedback (caller owns). */
typedef struct sc_quality_score {
    int total;
    int brevity;
    int validation;
    int warmth;
    int naturalness;
    bool needs_revision;
} sc_quality_score_t;

sc_quality_score_t sc_conversation_evaluate_quality(const char *response, size_t response_len,
                                                    const sc_channel_history_entry_t *entries,
                                                    size_t count, uint32_t max_chars);

/* Honesty guardrail: detect "did you do X?" questions and inject honest context.
 * Returns a context string if an honesty injection is needed, NULL otherwise.
 * Caller owns returned string. */
char *sc_conversation_honesty_check(sc_allocator_t *alloc, const char *message, size_t message_len);

/* Narrative arc phase for the conversation.
 * Adds structure/climax/intervention guidance to the awareness buffer. */
typedef enum sc_narrative_phase {
    SC_NARRATIVE_OPENING = 0,
    SC_NARRATIVE_BUILDING,
    SC_NARRATIVE_APPROACHING_CLIMAX,
    SC_NARRATIVE_PEAK,
    SC_NARRATIVE_RELEASE,
    SC_NARRATIVE_CLOSING,
} sc_narrative_phase_t;

sc_narrative_phase_t sc_conversation_detect_narrative(const sc_channel_history_entry_t *entries,
                                                      size_t count);

/* Engagement level detection */
typedef enum sc_engagement_level {
    SC_ENGAGEMENT_HIGH = 0,
    SC_ENGAGEMENT_MODERATE,
    SC_ENGAGEMENT_LOW,
    SC_ENGAGEMENT_DISTRACTED,
} sc_engagement_level_t;

sc_engagement_level_t sc_conversation_detect_engagement(const sc_channel_history_entry_t *entries,
                                                        size_t count);

/* Emotional valence from conversation (-1.0 to 1.0, negative=distressed, positive=happy) */
typedef struct sc_emotional_state {
    float valence;
    float intensity;
    bool concerning;
    const char *dominant_emotion;
} sc_emotional_state_t;

sc_emotional_state_t sc_conversation_detect_emotion(const sc_channel_history_entry_t *entries,
                                                    size_t count);

/* ── Multi-message splitting ──────────────────────────────────────────── */

/* Split a single response into multiple message fragments for natural delivery.
 * Analyzes sentence boundaries, conjunctions, and interjections to break
 * a response into the kind of rapid-fire fragments real humans send.
 * Returns fragment count. Each fragment.text is a separately allocated copy;
 * caller must free each fragment.text via alloc->free(ctx, text, text_len+1).
 * Returns 0 on error or if response is empty/NULL. */
typedef struct sc_message_fragment {
    char *text;
    size_t text_len;
    uint32_t delay_ms; /* suggested inter-message delay before this fragment */
} sc_message_fragment_t;

size_t sc_conversation_split_response(sc_allocator_t *alloc, const char *response,
                                      size_t response_len, sc_message_fragment_t *fragments,
                                      size_t max_fragments);

/* ── Texting style analysis ───────────────────────────────────────────── */

/* Analyze the other person's texting style from conversation history.
 * Produces a style context string for the prompt with capitalization,
 * punctuation, fragmentation, and abbreviation patterns.
 * Caller owns returned string. Returns NULL if insufficient data. */
char *sc_conversation_analyze_style(sc_allocator_t *alloc,
                                    const sc_channel_history_entry_t *entries, size_t count,
                                    size_t *out_len);

/* ── Response action classification ───────────────────────────────────── */

typedef enum sc_response_action {
    SC_RESPONSE_FULL = 0,  /* generate full response */
    SC_RESPONSE_BRIEF = 1, /* ultra-short: "yeah", "lol", "nice" */
    SC_RESPONSE_SKIP = 2,  /* don't respond at all */
    SC_RESPONSE_DELAY = 3, /* full response but with extra delay */
} sc_response_action_t;

/* Classify how to respond, factoring in conversation context. */
sc_response_action_t sc_conversation_classify_response(const char *msg, size_t msg_len,
                                                       const sc_channel_history_entry_t *entries,
                                                       size_t entry_count,
                                                       uint32_t *delay_extra_ms);

#endif /* SC_CONTEXT_CONVERSATION_H */
