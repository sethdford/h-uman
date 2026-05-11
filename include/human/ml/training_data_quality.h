#ifndef HU_ML_TRAINING_DATA_QUALITY_H
#define HU_ML_TRAINING_DATA_QUALITY_H

/* Training-data PII redaction + quality filter.
 *
 * Phase A1.2 of the SOTA roadmap. Conversation history that flows into
 * the LoRA training pipeline can carry email addresses, phone numbers,
 * credit-card-shaped digits, IP addresses, SSNs, and secret tokens.
 * Even when the training run itself stays on-device, the resulting
 * adapter weights *memorize* whatever's in the corpus — so PII has to
 * be scrubbed before it reaches a JSONL training file, not after.
 *
 * The redactor is a deterministic scanner — no regex library — that
 * walks the input once and writes the output in a single pass. Each
 * pattern is replaced with a fixed token (e.g. "[EMAIL]") and
 * accounted for in `hu_pii_stats_t` so callers can log redaction
 * counts and the test suite can assert detection rates.
 *
 * Patterns covered:
 *   - Email addresses          → [EMAIL]
 *   - US-style phone numbers   → [PHONE]
 *   - SSN (NNN-NN-NNNN)        → [SSN]
 *   - Credit-card-shaped digit groups (16 digits with optional separators)
 *                              → [CC]
 *   - IPv4 dotted-quad         → [IP]
 *   - Long opaque token strings preceded by api_key= / token: / etc.
 *                              → [SECRET]
 *
 * The redactor is deliberately conservative: it never returns more
 * bytes than it received, and it only matches well-anchored patterns
 * to keep false-positive rate low (e.g. a 16-digit ISBN inside a long
 * paragraph could trip the CC matcher; we accept that as the cost of
 * privacy-by-default).
 *
 * All functions are pure C, allocator-free, side-effect-free, and
 * safe to call from any thread. */

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_pii_stats {
    uint32_t emails;
    uint32_t phones;
    uint32_t ssns;
    uint32_t credit_cards;
    uint32_t ips;
    uint32_t secrets;
} hu_pii_stats_t;

/* Redact PII from `text` (length `text_len`) into `out` (capacity
 * `out_cap`). Output is NUL-terminated when there's room. On return:
 *   - `*out_len` (when non-NULL) is the number of bytes written
 *     before the NUL.
 *   - `*stats`   (when non-NULL) holds per-pattern counts.
 *
 * Truncation is silent — when the output would exceed `out_cap - 1`,
 * the redactor stops writing at the byte before the NUL and reports
 * the truncated size via `*out_len`. Callers that care about
 * round-trip fidelity must size `out_cap` to at least
 * `text_len + 16` (the longest replacement token is shorter than
 * every pattern it replaces, but `[EMAIL]` is one byte longer than
 * the minimal email "a@b.co"; +16 covers the worst case for any
 * realistic chat-message-sized input).
 *
 * Returns:
 *   HU_OK                   — redaction completed (possibly truncated).
 *   HU_ERR_INVALID_ARGUMENT — NULL text/out, or out_cap == 0. */
hu_error_t hu_pii_redact(const char *text, size_t text_len,
                         char *out, size_t out_cap, size_t *out_len,
                         hu_pii_stats_t *stats);

/* Convenience: returns true when `hu_pii_redact` would emit at least
 * one redaction marker on `text`. Pure scan, no buffer needed.
 *
 * Useful for telemetry-free quality gates that want to refuse to
 * write a training example carrying PII without re-scanning it. */
bool hu_pii_contains_pii(const char *text, size_t text_len);

/* Total redactions across all categories — convenience for tests. */
static inline uint32_t hu_pii_total(const hu_pii_stats_t *s) {
    if (!s) return 0;
    return s->emails + s->phones + s->ssns + s->credit_cards + s->ips + s->secrets;
}

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_TRAINING_DATA_QUALITY_H */
