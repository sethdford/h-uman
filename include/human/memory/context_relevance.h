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
 *            records what the relevance decision WOULD inject
 *            (items_considered, items_passing, bytes_would_inject, top_score,
 *            a score histogram). Nothing sent changes.
 *   LIVE   — the cliffs are replaced: an item is injected when its relevance
 *            clears the threshold, whatever the word count or tier. Casual
 *            turns keep a SMALL byte budget instead of zero.
 *
 * Relevance per source:
 *   semantic — the hit's cosine score from the retrieval scorer. Threshold:
 *              $HU_CONTEXT_RELEVANCE_MIN_SCORE, else
 *              HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE (see below).
 *   graph    — the composer's own query-conditioning: a block counts as
 *              relevant when the message named one of the contact's entities
 *              (matched_entities > 0) or an owner fact by full name. The
 *              contact-anchored fallback is not query-conditioned, so on a
 *              casual turn it stays dropped.
 *
 * Logs carry counts, byte sizes and scores only: never text, keys or ids. */

#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/retrieval.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default semantic threshold, derived from data rather than picked: the
 * MEDIAN cosine similarity between two arbitrary memories in the live index
 * (scripts/context_relevance_floor.py on ~/.human/memory.db, 2026-10-02:
 * 72 vectors, 2,556 pairs; p50 0.458, p75 0.695, p95 0.798). A hit must be at
 * least as close to the message as an unrelated pair of memories are to each
 * other. No labelled data exists yet on which recalled items Seth's real
 * replies referenced, so this is the scorer's own floor; the SHADOW histogram
 * is what a learned threshold will be fitted on (docs/guides/
 * context-relevance.md). Override with $HU_CONTEXT_RELEVANCE_MIN_SCORE. */
#define HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE 0.46

/* Default casual-turn budget: two recall hits at HU_SEMANTIC_RECALL_HIT_MAX_BYTES.
 * Override with $HU_CONTEXT_RELEVANCE_CASUAL_BYTES. */
#define HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES 480u

/* Histogram bands over the score: <0.2, [0.2,0.4), [0.4,0.6), [0.6,0.8), >=0.8. */
#define HU_CONTEXT_RELEVANCE_HIST_BANDS 5u

typedef struct hu_context_relevance_stats {
    size_t items_considered;   /* candidates after the recall content policy */
    size_t items_passing;      /* candidates at or above the threshold */
    size_t bytes_would_inject; /* bytes the passing items occupy within the budget */
    double top_score;          /* best candidate score (0 when none) */
    uint32_t hist[HU_CONTEXT_RELEVANCE_HIST_BANDS];
} hu_context_relevance_stats_t;

/* $HU_CONTEXT_RELEVANCE=off|shadow|live, default OFF. */
hu_gate_mode_t hu_context_relevance_mode(void);

/* $HU_CONTEXT_RELEVANCE_MIN_SCORE in [-1, 1], else the default. Unparsable
 * values fail closed to the default. */
double hu_context_relevance_min_score(void);

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
 * verdict (it now only sizes the budget). *budget_bytes holds the full recall
 * budget on entry.
 *   OFF    — returns false; nothing touched.
 *   SHADOW — logs the would-be decision; returns false; nothing touched.
 *   LIVE   — drops below-threshold hits from `sem`, lowers *budget_bytes to
 *            the casual budget on casual turns, logs, returns true: the caller
 *            must then skip the word-count suppression. */
bool hu_context_relevance_semantic(hu_allocator_t *alloc, hu_retrieval_result_t *sem, bool casual,
                                   size_t *budget_bytes);

/* The graph-grounding decision for a turn below the ANALYTICAL tier (the only
 * turns the casual cliff drops). `relevance` is matched_entities plus 1 when
 * owner facts were matched by full name; 0 means the block is not
 * query-conditioned (empty, or the contact fallback only).
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
