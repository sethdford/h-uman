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

/* ── Quality filter ────────────────────────────────────────────────────── */
/* Phase A1.2 (continued): once PII is scrubbed, the next quality lever
 * is reject-low-signal-examples-before-they-poison-the-adapter. Three
 * cheap signals catch ~all of the common offenders without a model:
 *
 *   1. Length      — too short or pathologically long examples.
 *   2. Entropy     — keyboard mashing, repeated chars, "lorem ipsum"
 *                    placeholder text.
 *   3. Uniqueness  — within-corpus near-duplicates (the same boilerplate
 *                    conversation appearing N times in a session log).
 *
 * Each gate is independent and pure-CPU; callers compose them in
 * whatever order makes sense for their pipeline. */

typedef enum hu_quality_verdict {
    HU_QUALITY_OK = 0,
    HU_QUALITY_REJECT_TOO_SHORT,
    HU_QUALITY_REJECT_TOO_LONG,
    HU_QUALITY_REJECT_LOW_ENTROPY,
    HU_QUALITY_REJECT_LOW_UNIQUE_RATIO,
} hu_quality_verdict_t;

typedef struct hu_quality_thresholds {
    /* Below this many bytes the example is rejected as too short.
     * Default 8 — empty / "ok" / single-emoji turns are skipped. */
    size_t min_chars;
    /* Above this many bytes the example is rejected as too long.
     * Default 16384 — typical chat turn is < 2 KB; a 16 KB blob is
     * either a dump or an artifact and rarely useful for adapter
     * training. */
    size_t max_chars;
    /* Below this many bits of Shannon byte-entropy the example is
     * rejected as low-information. Only applied to texts with at
     * least 32 bytes — short messages legitimately have low entropy.
     * Default 2.5 — "aaaaaaaaaaaaa" (≈0 bits) and "ababab..." (1 bit)
     * are caught, real prose (4-5 bits) sails through. */
    float min_entropy_bits;
    /* Below this fraction of unique bytes / total bytes the example
     * is rejected as repetitive. Default 0.10 — "yyyyyyy" (1/N) is
     * caught, normal prose (~0.20-0.40) is fine. Only applied to
     * texts with at least 32 bytes. */
    float min_unique_ratio;
} hu_quality_thresholds_t;

/* Initialize `out` with conservative defaults that pass typical chat
 * training data and reject obvious garbage. Callers that want stricter
 * gates can adjust individual fields after this call. */
void hu_quality_thresholds_default(hu_quality_thresholds_t *out);

/* Evaluate `text` against `thresholds`. NULL `thresholds` means
 * "use defaults". Returns the first failing verdict, or HU_QUALITY_OK
 * when the text passes every gate. Pure CPU, allocator-free, O(n). */
hu_quality_verdict_t hu_quality_check(const char *text, size_t text_len,
                                      const hu_quality_thresholds_t *thresholds);

/* Stable string for telemetry / logging. Returns "ok" / "too_short" /
 * "too_long" / "low_entropy" / "low_unique_ratio" / "unknown". */
const char *hu_quality_verdict_name(hu_quality_verdict_t v);

/* ── Near-duplicate detector ───────────────────────────────────────────── */
/* Within-run dedup. Stores 64-bit FNV-1a hashes of *normalized* text
 * (lowercased, whitespace-collapsed) in a dynamically-grown sorted
 * array. Lookup is O(log n); insert is O(n) amortized via doubling.
 * Intended scale: 10K–100K conversations per extraction run.
 *
 * Normalization is intentionally aggressive — two conversations that
 * differ only in capitalization or whitespace are treated as the
 * same. SimHash-style fuzzy near-dup (e.g. "hi vs hi!") is a future
 * extension; this module catches the 80% case where copy/paste or
 * automated logging produces byte-identical-after-normalize text. */

typedef struct hu_dedup_set {
    uint64_t *hashes;
    size_t count;
    size_t capacity;
} hu_dedup_set_t;

/* Initialize an empty dedup set. `initial_capacity` is a hint; pass 0
 * to defer allocation until first insert. Returns HU_OK on success. */
hu_error_t hu_dedup_set_init(hu_dedup_set_t *set, size_t initial_capacity);

/* Free internal storage. Safe to call on a zero-initialized set. */
void hu_dedup_set_free(hu_dedup_set_t *set);

/* Single-pass query + insert. Computes the canonical hash of `text`,
 * checks it against the set, and records it if absent.
 *
 * Returns true when `text` is a duplicate of a previously-seen entry
 * (caller should skip it), false otherwise (caller should keep it).
 * On allocation failure the call returns false and the entry is not
 * recorded — the caller still gets to write the example, which is the
 * conservative failure mode. */
bool hu_dedup_set_check_and_add(hu_dedup_set_t *set,
                                const char *text, size_t text_len);

/* Number of distinct entries recorded so far. Useful for telemetry. */
static inline size_t hu_dedup_set_size(const hu_dedup_set_t *set) {
    return set ? set->count : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_TRAINING_DATA_QUALITY_H */
