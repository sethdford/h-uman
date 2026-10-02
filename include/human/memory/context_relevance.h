#ifndef HUMAN_MEMORY_CONTEXT_RELEVANCE_H
#define HUMAN_MEMORY_CONTEXT_RELEVANCE_H

/* Context relevance: one relevance decision in place of the two "casual
 * register" cliffs that kept memory out of short turns.
 *
 * Before this gate a reply turn lost its memory context on two word-count /
 * tier tests, whatever the memory said:
 *   - semantic recall: HU_SEMANTIC_RECALL_REGISTER_GATE=live drops every
 *     semantic hit when the inbound has <= 12 words (85 suppressions, 395
 *     hits dropped, 38 live recalls in 13 days of prod logs);
 *   - graph grounding: HU_GRAPH_GROUNDING=on drops the composed block below
 *     the ANALYTICAL tier (165 of 172 live turns).
 * Most texting is short, so the twin forgot exactly while chatting.
 *
 * Gate: HU_CONTEXT_RELEVANCE=off|shadow|live (default OFF).
 *   OFF    — byte-identical to before: both cliffs apply unchanged.
 *   SHADOW — retrieval runs as before; ONE aggregate line per turn and source
 *            records what the relevance decision WOULD inject, plus the
 *            calibration sample below. Nothing sent changes.
 *   LIVE   — on CASUAL turns only (the turns the cliffs starved), an item is
 *            injected when its relevance clears the threshold, within a small
 *            byte budget instead of zero. Substantive turns keep today's
 *            recall unchanged until the threshold is calibrated.
 *
 * Relevance per source:
 *   semantic — the hit's cosine score (query-to-document, embed_query).
 *   graph    — the composer's query-conditioning, strictly: an entity counts
 *              only when EVERY scoreable word of its name is in the message
 *              (HU_GG_REQUIRE_FULL_NAME) and it is not an EMOTION, so "love
 *              you" or "at work" do not qualify. The contact fallback is not
 *              query-conditioned and stays dropped on casual turns.
 *
 * Calibration. Every non-OFF semantic decision also scores the message's own
 * embedding against HU_CONTEXT_RELEVANCE_NULL_K random stored vectors (dot
 * products only, hu_vector_store_sqlite_vec_sample_scores): what an UNRELATED
 * memory scores against THIS kind of query. Those null scores feed a rolling
 * in-process pool; the threshold is, in order:
 *   1. $HU_CONTEXT_RELEVANCE_MIN_SCORE, when set (config after review);
 *   2. the pool's p95, once it holds >= HU_CONTEXT_RELEVANCE_NULL_MIN scores;
 *   3. HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE (cold start only).
 *
 * Logs carry counts, byte sizes and scores only: never text, keys or ids. */

#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/retrieval.h"
#include "human/memory/semantic_recall.h"
#include "human/memory/vector.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cold-start threshold only: the median DOCUMENT-to-document cosine of the
 * live index (scripts/context_relevance_floor.py, 2026-10-02: 72 vectors,
 * p50 0.458). Live scores are query-to-document, a different distribution,
 * so this is replaced by the null-pool p95 as soon as the pool fills. */
#define HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE 0.46

/* Random stored vectors scored per decision, the pool size and the pool fill
 * needed before its p95 becomes the threshold. */
#define HU_CONTEXT_RELEVANCE_NULL_K    32u
#define HU_CONTEXT_RELEVANCE_NULL_POOL 1024u
#define HU_CONTEXT_RELEVANCE_NULL_MIN  128u

/* Casual-turn budget: two recall hits. Override $HU_CONTEXT_RELEVANCE_CASUAL_BYTES. */
#define HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES (2u * HU_SEMANTIC_RECALL_HIT_MAX_BYTES)

/* 0.05-wide histogram bands over [0, 1]; scores below 0 land in band 0. */
#define HU_CONTEXT_RELEVANCE_HIST_BANDS 20u
/* Top real scores logged per decision, at 0.05 resolution. */
#define HU_CONTEXT_RELEVANCE_TOP_LOGGED 5u

typedef enum hu_context_relevance_threshold_src {
    HU_CR_THRESHOLD_COLD = 0, /* HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE */
    HU_CR_THRESHOLD_NULL,     /* p95 of the null pool */
    HU_CR_THRESHOLD_ENV,      /* $HU_CONTEXT_RELEVANCE_MIN_SCORE */
} hu_context_relevance_threshold_src_t;

typedef struct hu_context_relevance_stats {
    size_t items_considered;   /* candidates after the recall content policy */
    size_t items_passing;      /* candidates at or above the threshold */
    size_t bytes_would_inject; /* bytes the passing items occupy within the budget */
    double top_score;          /* best candidate score (0 when none) */
    uint32_t hist[HU_CONTEXT_RELEVANCE_HIST_BANDS];
} hu_context_relevance_stats_t;

/* $HU_CONTEXT_RELEVANCE=off|shadow|live, default OFF. */
hu_gate_mode_t hu_context_relevance_mode(void);

/* The threshold in force (precedence above); *src (may be NULL) says which.
 * An unparsable or out-of-range env value is ignored. */
double hu_context_relevance_threshold(hu_context_relevance_threshold_src_t *src);
double hu_context_relevance_min_score(void); /* threshold(NULL) */

/* Null pool: add n scores (oldest dropped past the pool size), read its size,
 * clear it (tests, config reload). Thread-safe. */
void hu_context_relevance_null_add(const float *scores, size_t n);
size_t hu_context_relevance_null_count(void);
void hu_context_relevance_null_reset(void);

/* Pure: nearest-rank p95 of v[0, n); 0 when n == 0. Does not reorder v. */
double hu_context_relevance_p95(const float *v, size_t n);

/* $HU_CONTEXT_RELEVANCE_CASUAL_BYTES (> 0), else the default. */
size_t hu_context_relevance_casual_bytes(void);

/* Pure: what the relevance decision would inject from a semantic result —
 * content-policy exclusions skipped, items below min_score skipped, the rest
 * cut to per_hit_bytes and kept in rank order while the cumulative bytes stay
 * within budget_bytes (the same rule as hu_semantic_recall_clamp_result).
 * Never mutates `res`. NULL res / out is a no-op (out zeroed when non-NULL). */
void hu_context_relevance_assess(const hu_retrieval_result_t *res, double min_score,
                                 size_t budget_bytes, size_t per_hit_bytes,
                                 hu_context_relevance_stats_t *out);

/* The semantic-recall decision, called by hu_hybrid_retrieve inside the
 * HU_SEMANTIC_RECALL=live branch. `casual` is the register classifier's
 * verdict. *budget_bytes holds the full recall budget on entry. `store` and
 * `query` (both may be NULL) feed the null-pool calibration sample.
 *   OFF    — returns false; nothing touched, nothing sampled.
 *   SHADOW — samples, logs the would-be decision; returns false.
 *   LIVE   — samples and logs; on a casual turn drops below-threshold hits
 *            from `sem`, lowers *budget_bytes to the casual budget and
 *            returns true (the caller then skips the word-count suppression).
 *            A substantive turn returns false: recall unchanged. */
bool hu_context_relevance_semantic(hu_allocator_t *alloc, hu_retrieval_result_t *sem, bool casual,
                                   size_t *budget_bytes, const hu_vector_store_t *store,
                                   const hu_embedding_t *query);

/* The graph-grounding decision for a turn below the ANALYTICAL tier (the only
 * turns the casual cliff drops). *ctx is the STRICT block (full-name, no
 * EMOTION seeds) and `relevance` its matched-entity count; 0 means nothing
 * in the message names a contact entity.
 *   OFF    — returns false (caller drops, as before).
 *   SHADOW — logs; returns false.
 *   LIVE   — relevance > 0: cuts *ctx in place to the casual budget at a line
 *            boundary, logs, returns true (caller injects). relevance == 0:
 *            logs, returns false. */
bool hu_context_relevance_graph(hu_allocator_t *alloc, char **ctx, size_t *ctx_len,
                                size_t relevance);

/* Pure: byte length to keep from s[0, len) so it is <= max_bytes and ends
 * after a '\n' when one exists in the window (else 0 — a graph block is only
 * useful in whole lines). Returns len when len <= max_bytes. */
size_t hu_context_relevance_line_cut(const char *s, size_t len, size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_CONTEXT_RELEVANCE_H */
