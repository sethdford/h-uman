#ifndef HU_BELIEF_H
#define HU_BELIEF_H

/* W8 — Belief layer: Bayesian posterior tracking for memory confidence.
 *
 * hu_belief_t replaces the scalar `confidence: float` used by W1-W7.
 * It tracks (mean, variance) via a Welford-style online update so the agent
 * can distinguish "confident with low variance" from "confident with high
 * variance (contested)". No external dependencies; pure deterministic math.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single contributing observation attributed to a named source. */
typedef struct hu_provenance_atom {
    char source[64];    /* "imessage", "user-explicit", "feed-web", ... */
    int64_t observed_at;
    float weight;       /* this source's contribution to the posterior */
} hu_provenance_atom_t;

/* Belief posterior: mean + variance over n observations.
 * mean ∈ [0, 1]: point estimate of truth probability.
 * variance ∈ [0, 0.25]: uncertainty (max for uniform prior = 0.25).
 * prov[]: up to 4 supporting sources (ring-buffer, oldest overwritten). */
typedef struct hu_belief {
    float mean;
    float variance;
    hu_provenance_atom_t prov[4];
    uint8_t prov_count;
    int64_t last_updated;
} hu_belief_t;

/* Create a fresh belief from a single observation.
 * Initial variance = mean*(1-mean) (Beta(1,1) with one data point). */
hu_belief_t hu_belief_init(float mean, const char *source, int64_t now);

/* Welford-style online posterior update.
 * Corroborating observations (|observation - prior.mean| < 0.5) shrink variance.
 * Contradicting observations (|observation - prior.mean| >= 0.5) grow variance. */
hu_belief_t hu_belief_update(const hu_belief_t *prior, float observation,
                              const char *source, int64_t now);

/* Inverse-variance pooling of two independent beliefs.
 * Combined mean = weighted average by precision (1/variance).
 * Combined variance = harmonic mean of variances / 2.
 * Degenerate case (both variances zero): simple average. */
hu_belief_t hu_belief_combine(const hu_belief_t *a, const hu_belief_t *b);

/* Returns true when the two beliefs differ by more than sigma_threshold
 * standard deviations of their combined spread.
 * Useful for flagging paraphrase contradictions before LLM-judge. */
bool hu_belief_significantly_disagrees(const hu_belief_t *a, const hu_belief_t *b,
                                        float sigma_threshold);

/* Semantic conflict classification between two text descriptions.
 * Uses deterministic heuristics (substring/negation) as fallback
 * when no LLM provider is available (HU_IS_TEST, no model). */
typedef enum hu_belief_conflict {
    HU_BELIEF_CONFLICT_NONE = 0,
    HU_BELIEF_CONFLICT_PARAPHRASE,
    HU_BELIEF_CONFLICT_CONTRADICT,
} hu_belief_conflict_t;

/* Compare two text descriptions for semantic equivalence/contradiction
 * without requiring exact string match.
 * Returns PARAPHRASE when >60% word overlap, CONTRADICT when negation
 * patterns are detected against a similar root, NONE otherwise. */
hu_belief_conflict_t hu_belief_semantic_conflict(
    const char *text_a, size_t len_a,
    const char *text_b, size_t len_b);

#ifdef __cplusplus
}
#endif

#endif /* HU_BELIEF_H */
