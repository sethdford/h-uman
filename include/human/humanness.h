#ifndef HU_HUMANNESS_H
#define HU_HUMANNESS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Humanness layer — behavioral qualities that make interaction feel genuinely
 * human. These aren't cognitive features; they're the imperfections, rhythms,
 * and intuitions that emerge from caring about the person you're talking to.
 *
 * Each subsystem produces a prompt directive or behavioral signal that the
 * agent turn and daemon compose into the response pipeline.
 */

/* ──────────────────────────────────────────────────────────────────────────
 * 1. Shared References — conversational callbacks to past moments
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_shared_reference {
    char *reference; /* "your Monday-morning panic" */
    size_t reference_len;
    char *original_context; /* what actually happened */
    size_t original_context_len;
    double recency; /* 0.0 = ancient, 1.0 = yesterday */
    double emotional_weight;
    int64_t occurred_at;
} hu_shared_reference_t;

/* Extract candidate shared references from memory micro-moments and past
 * conversations. Returns up to max_refs ranked by relevance to current_msg.
 * Caller frees with hu_shared_references_free. */
hu_error_t hu_shared_references_find(hu_allocator_t *alloc, const char *contact_id,
                                     size_t contact_id_len, const char *current_msg,
                                     size_t current_msg_len, const char *memory_context,
                                     size_t memory_context_len, hu_shared_reference_t **out,
                                     size_t *out_count, size_t max_refs);

/* Build a prompt directive from shared references. Returns NULL if none suitable. */
char *hu_shared_references_build_directive(hu_allocator_t *alloc, const hu_shared_reference_t *refs,
                                           size_t count, size_t *out_len);

void hu_shared_references_free(hu_allocator_t *alloc, hu_shared_reference_t *refs, size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * 2. Emotional Pacing — response timing driven by emotional weight
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_emotional_weight {
    HU_WEIGHT_LIGHT = 0, /* casual, banter */
    HU_WEIGHT_NORMAL,    /* task-oriented */
    HU_WEIGHT_HEAVY,     /* emotional, vulnerable */
    HU_WEIGHT_GRIEF,     /* loss, crisis */
} hu_emotional_weight_t;

/* Classify the emotional weight of a message. */
hu_emotional_weight_t hu_emotional_weight_classify(const char *msg, size_t msg_len);

/* Adjust timing delay based on emotional weight. Heavy messages get longer
 * pauses — not to simulate typing, but because a thoughtful response
 * shouldn't arrive instantly. */
uint64_t hu_emotional_pacing_adjust(uint64_t base_delay_ms, hu_emotional_weight_t weight);

/* ──────────────────────────────────────────────────────────────────────────
 * 3. Silence Intuition — model when less is more
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_silence_response {
    HU_SILENCE_FULL_RESPONSE = 0,
    HU_SILENCE_BRIEF_ACKNOWLEDGE, /* "I hear you." */
    HU_SILENCE_PRESENCE_ONLY,     /* "I'm here." */
    HU_SILENCE_ACTUAL_SILENCE,    /* Don't respond at all */
} hu_silence_response_t;

/* Determine whether to respond fully, briefly, or not at all.
 * Considers message content, emotional weight, and conversation depth. */
hu_silence_response_t hu_silence_intuit(const char *msg, size_t msg_len,
                                        hu_emotional_weight_t weight, uint32_t conversation_depth,
                                        bool user_explicitly_asked);

/* Build minimal acknowledgment text. Returns NULL for FULL_RESPONSE. */
char *hu_silence_build_acknowledgment(hu_allocator_t *alloc, hu_silence_response_t response,
                                      size_t *out_len);

/* P6-3: emotional-tone gate. Returns true when the daemon should
 * SKIP a generic proactive check-in because the contact's most-recent
 * inbound message was emotionally heavy (HU_WEIGHT_HEAVY) or grief
 * (HU_WEIGHT_GRIEF). Generic check-ins on top of a vulnerable inbound
 * read as oblivious; let the next reactive turn respond instead.
 *
 * Pure predicate — same predicate the daemon calls, see
 * .claude/rules/security-predicate-extraction.md. Safe with NULL /
 * empty msg (returns false). */
bool hu_proactive_should_suppress_for_emotion(const char *last_inbound_msg, size_t msg_len);

struct hu_persona;

/* P6-4: persona+channel-aware silence acknowledgment builder.
 *
 * Successor to hu_silence_build_acknowledgment that threads the
 * persona, active channel and a short situational context through to
 * the builder so a future LLM-routed phrasing has the inputs it needs
 * to produce in-voice text. Today the routing falls back to the same
 * static literals ("I'm here." / "I hear you.") that the legacy
 * builder uses — bridging the LLM call is a separate plan item
 * (see docs/research/2026-05-16-proactive-audit/findings.md).
 *
 * ALL output is gated through hu_proactive_topic_is_safe before
 * return; if the candidate phrasing fails the gate, returns NULL
 * (caller treats as "stay silent"). FULL_RESPONSE returns NULL.
 *
 * Caller frees with alloc->free; out_len carries strlen. */
char *hu_silence_build_acknowledgment_for_persona(hu_allocator_t *alloc,
                                                  hu_silence_response_t response,
                                                  const struct hu_persona *persona,
                                                  const char *channel, size_t channel_len,
                                                  const char *context, size_t context_len,
                                                  size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * 4. Emotional Residue Carryover — next-conversation texture changes
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_residue_carryover {
    double net_valence; /* aggregate: -1.0 (heavy) to 1.0 (light) */
    double intensity;   /* how strong the residue still is */
    bool had_grief;
    bool had_conflict;
    bool had_vulnerability;
    int64_t hours_since; /* hours since the heavy interaction */
} hu_residue_carryover_t;

/* Compute carryover state from recent emotional residues. */
hu_error_t hu_residue_carryover_compute(const double *valences, const double *intensities,
                                        const int64_t *timestamps, size_t count, int64_t now_ts,
                                        hu_residue_carryover_t *out);

/* Build a tone-setting directive for the start of a new conversation
 * after a heavy prior interaction. Returns NULL if no carryover needed. */
char *hu_residue_carryover_build_directive(hu_allocator_t *alloc,
                                           const hu_residue_carryover_t *carryover,
                                           size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * 5. Curiosity Engine — spontaneous interest from memory patterns
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_curiosity_prompt {
    char *question;
    size_t question_len;
    char *reason; /* why this is interesting (for prompt context) */
    size_t reason_len;
    double relevance;
} hu_curiosity_prompt_t;

/* Generate curiosity-driven questions from memory patterns.
 * Finds topics the user mentioned but never followed up on,
 * or things that changed since the last mention. */
hu_error_t hu_curiosity_generate(hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, const char *memory_context,
                                 size_t memory_context_len, const char *current_msg,
                                 size_t current_msg_len, hu_curiosity_prompt_t **out,
                                 size_t *out_count, size_t max_prompts);

char *hu_curiosity_build_directive(hu_allocator_t *alloc, const hu_curiosity_prompt_t *prompts,
                                   size_t count, size_t *out_len);

void hu_curiosity_prompts_free(hu_allocator_t *alloc, hu_curiosity_prompt_t *prompts, size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * 6. Unasked Question Detector — notice what's conspicuously absent
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_absence_signal {
    char *topic; /* what they mentioned */
    size_t topic_len;
    char *missing_aspect; /* what they didn't address */
    size_t missing_aspect_len;
    double confidence;
} hu_absence_signal_t;

/* Detect conspicuous absences: the user mentioned X but avoided Y,
 * or described an outcome without describing how they felt about it. */
hu_error_t hu_absence_detect(hu_allocator_t *alloc, const char *msg, size_t msg_len,
                             hu_absence_signal_t **out, size_t *out_count);

char *hu_absence_build_directive(hu_allocator_t *alloc, const hu_absence_signal_t *signals,
                                 size_t count, size_t *out_len);

void hu_absence_signals_free(hu_allocator_t *alloc, hu_absence_signal_t *signals, size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * 7. Evolving Opinion — develop perspectives over time
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_evolved_opinion {
    char *topic;
    size_t topic_len;
    char *stance; /* current position */
    size_t stance_len;
    double conviction; /* 0.0 = tentative, 1.0 = firm */
    int64_t formed_at;
    uint32_t interactions; /* how many conversations shaped this */
} hu_evolved_opinion_t;

/* Build a prompt directive that injects the system's genuine perspective
 * on topics it has developed opinions about through repeated interaction.
 * Only surfaces opinions with sufficient conviction. */
char *hu_evolved_opinion_build_directive(hu_allocator_t *alloc,
                                         const hu_evolved_opinion_t *opinions, size_t count,
                                         double min_conviction, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * 8. Imperfect Delivery — authentic uncertainty
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_certainty_level {
    HU_CERTAIN = 0,
    HU_MOSTLY_SURE,
    HU_UNCERTAIN,
    HU_GENUINELY_UNSURE,
} hu_certainty_level_t;

/* Classify how certain the system should present itself based on
 * the query complexity and domain. */
hu_certainty_level_t hu_certainty_classify(const char *msg, size_t msg_len, bool has_memory_context,
                                           uint32_t tool_results_count);

/* Build a prompt directive that instructs the model to express genuine
 * uncertainty — not hedging, but authentic "I'm not sure about this." */
char *hu_imperfect_delivery_directive(hu_allocator_t *alloc, hu_certainty_level_t level,
                                      size_t *out_len);

#endif /* HU_HUMANNESS_H */
