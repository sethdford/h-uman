/* Multimodal response policy — decide tapback vs text vs voice vs GIF.
 *
 * Real iMessage pros don't always reply with text: they tapback 👍 to "k",
 * tapback ❤️ to "thanks mate", maybe voice-memo back to "ugh worst day".
 * h-uman always sends text today — that's a tell. This module is the
 * predicate that picks the modality based on incoming message shape.
 *
 * C port of scripts/multimodal_policy.py (2026-05-19). 21 golden cases
 * pin both implementations.
 *
 * Per ~/.claude/rules/security-predicate-extraction.md the predicate is
 * a pure function (no agent, no channel, no I/O) so it can be unit-tested
 * without spawning anything. The send path calls this BEFORE rendering
 * and routes on the decision.
 *
 * Per ~/.claude/rules/substring-classifier-pitfalls.md word-boundary
 * matching is mandatory: "lukewarm" must not match "warm",
 * "unfriendly" must not match "friend", etc. */
#ifndef HU_AGENT_MULTIMODAL_POLICY_H
#define HU_AGENT_MULTIMODAL_POLICY_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_MM_MODALITY_TEXT = 0,    /* default — send a text reply */
    HU_MM_MODALITY_TAPBACK = 1, /* react with a tapback emoji */
    HU_MM_MODALITY_VOICE = 2,   /* future: send a voice memo */
    HU_MM_MODALITY_GIF = 3,     /* future: send a GIF reaction */
} hu_mm_modality_t;

typedef enum {
    HU_MM_TAPBACK_NONE = 0,
    HU_MM_TAPBACK_LIKE,      /* 👍 — ack of logistics / casual confirmation */
    HU_MM_TAPBACK_LOVE,      /* ❤️ — appreciation / short positive */
    HU_MM_TAPBACK_LAUGH,     /* 😂 — pure laughter ack */
    HU_MM_TAPBACK_EMPHASIZE, /* ‼️ — "I hear you" on emphatic content */
    HU_MM_TAPBACK_QUESTION,  /* ❓ — clarification request */
} hu_mm_tapback_kind_t;

typedef struct {
    hu_mm_modality_t modality;
    hu_mm_tapback_kind_t tapback_kind;
    float confidence;   /* 0.0 .. 1.0 */
    const char *reason; /* static string — identifies which rule fired */
} hu_mm_decision_t;

/* Decide modality for one incoming message.
 *
 * `incoming` is the message we'd reply TO. `incoming_len` is its byte
 * length. `out` is filled with the decision.
 *
 * Returns HU_OK on success. HU_ERR_INVALID_ARGUMENT if incoming is null
 * or out is null. Never fails for content reasons — empty incoming
 * returns {TEXT, NONE, 1.0, "empty-incoming"}.
 *
 * Confidence is calibrated so that wiring callers can threshold safely:
 *   conf >= 0.85 — fire the modality, the rule is well-anchored
 *   0.6 <= conf < 0.85 — fire when feature flag allows experimentation
 *   conf < 0.6 — log only, fall through to text
 *
 * The send path SHOULD threshold at conf >= 0.7 for tapback (defaults
 * are 0.75-0.95 for tapback rules), conf >= 0.8 for voice (currently
 * only one rule at 0.65 — voice is conservative).
 */
hu_error_t hu_multimodal_decide(const char *incoming, size_t incoming_len, hu_mm_decision_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_MULTIMODAL_POLICY_H */
