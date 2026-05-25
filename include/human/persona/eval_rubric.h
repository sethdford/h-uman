/* include/human/persona/eval_rubric.h
 *
 * Pure scoring predicates for A-loop blind eval framework.
 * US-48-1: Validate autoresponder persona fidelity via rubric.
 *
 * Three independent dimensions (tone, length, formality) each scored 0-10.
 * No provider calls — text-only deterministic analysis.
 *
 * Contracting:
 *   All functions accept NUL-terminated strings.
 *   All functions return int 0-10 (higher = better match).
 *   Functions are pure (no I/O, no state, no allocation).
 *
 * Test seams:
 *   hu_eval_rubric_hash_for_blind_order — deterministic shuffle helper
 */

#ifndef HUMAN_PERSONA_EVAL_RUBRIC_H
#define HUMAN_PERSONA_EVAL_RUBRIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tone alignment predicate.
 *
 * Compares incoming message tone to two reply candidates (baseline vs
 * persona-aware). Scores the persona reply relative to baseline on how
 * well it mirrors the incoming tone.
 *
 * Heuristics:
 *   - Exclamation count (excited incoming → excited reply preferred)
 *   - Question count (interrogative incoming → responsive replies preferred)
 *   - Emoji count (emoji-heavy incoming → emoji presence preferred)
 *   - Sentiment polarity (positive incoming → positive reply preferred)
 *
 * Args:
 *   incoming       — message the user received (establishes expected tone)
 *   reply_baseline — response without persona (baseline LLM output)
 *   reply_persona  — response with persona overlay (candidate A-loop output)
 *
 * Returns:
 *   int 0-10 where 10 = persona reply perfectly mirrors incoming tone,
 *   5 = neutral / both match equally poorly, 0 = persona mismatches badly.
 */
int hu_eval_rubric_tone_match(const char *incoming, const char *reply_baseline,
                              const char *reply_persona);

/* Length alignment predicate.
 *
 * Scores how well the reply length matches the incoming message.
 * Heuristic: very short incoming → expect short reply; long incoming
 * → expect proportionally longer reply; massive mismatch is penalized.
 *
 * Args:
 *   incoming       — message the user received (establishes expected length)
 *   reply_baseline — response without persona
 *   reply_persona  — response with persona overlay
 *
 * Returns:
 *   int 0-10 where 10 = persona reply is much closer to expected length
 *   than baseline, 5 = both equally off-target, 0 = persona way worse.
 */
int hu_eval_rubric_length_match(const char *incoming, const char *reply_baseline,
                                const char *reply_persona);

/* Formality alignment predicate.
 *
 * Scores how well reply register (formal/casual/professional) matches
 * incoming. Uses word-boundary substring matching (not naive substring;
 * avoids false-positives like "unfriendly" matching "friendly").
 *
 * Formal keywords: "professional", "formal", "proper", "official", "business"
 * Casual keywords: "casual", "chill", "laid-back", "relaxed", "informal"
 *
 * Args:
 *   incoming       — message the user received (establishes formality tier)
 *   reply_baseline — response without persona
 *   reply_persona  — response with persona overlay
 *
 * Returns:
 *   int 0-10 where 10 = persona reply matches incoming formality tier,
 *   5 = neutral (no strong signal either way), 0 = persona is inverted.
 */
int hu_eval_rubric_formality_match(const char *incoming, const char *reply_baseline,
                                   const char *reply_persona);

/* Deterministic hash for blind-eval shuffle.
 *
 * Produces a 64-bit hash of two responses to shuffle eval order without
 * revealing identity. Same inputs always produce same output (deterministic
 * for reproducible shuffles).
 *
 * Args:
 *   response_a — first arm text
 *   response_b — second arm text
 *   seed       — random seed for shuffle order (supplied by test harness)
 *
 * Returns:
 *   uint64_t hash suitable for use as a sort key or random number.
 */
uint64_t hu_eval_rubric_hash_for_blind_order(const char *response_a, const char *response_b,
                                             uint32_t seed);

/* JSON output serializer for per-contact eval results.
 *
 * Serializes a list of contact scores into JSON format for AC-1.5 output.
 * Produces: { "results": [ { "contact": "alice", "score": 7.5 }, ... ] }
 *
 * Args:
 *   contacts    — array of contact handle strings (NULL-terminated)
 *   scores      — array of corresponding scores (0.0-10.0)
 *   count       — number of contacts/scores
 *   buf         — output buffer (caller-allocated)
 *   buflen      — size of output buffer
 *
 * Returns:
 *   Number of characters written (not including NUL terminator), or negative
 *   on error (e.g., buffer too small). Check result >= 0 and result < buflen.
 */
int hu_eval_rubric_json_per_contact(const char **contacts, const double *scores, int count,
                                    char *buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_PERSONA_EVAL_RUBRIC_H */
