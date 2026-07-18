#ifndef HU_AGENT_CONTEXTUAL_BANDIT_H
#define HU_AGENT_CONTEXTUAL_BANDIT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bandit outcome signal (from US-104 signal layer). */
typedef enum {
    HU_BANDIT_REPLY = 0,   /* contact replied: α++ */
    HU_BANDIT_IGNORED = 1, /* contact did not reply: β++ */
    HU_BANDIT_BLOCKED = 2  /* contact blocked/opted out: β += 3 */
} hu_bandit_outcome_t;

/* Per-contact arm state — Beta(α, β) posterior. */
typedef struct hu_contextual_bandit_arm {
    uint64_t contact_handle; /* uint64_t identifier for deduplication */
    double alpha;            /* successes; initialization: 1.0 (weak prior) */
    double beta;             /* failures; initialization: 1.0 (weak prior) */
    uint64_t updates;        /* cumulative update count for diagnostics */
} hu_contextual_bandit_arm_t;

/* Bandit state container. */
typedef struct hu_contextual_bandit {
    hu_allocator_t *alloc;
    hu_contextual_bandit_arm_t *arms; /* fixed array, sized at create */
    size_t capacity;                  /* max number of contacts */
    size_t count;                     /* current number of arms in use */
    double threshold;                 /* decision threshold (default 0.3) */
    uint32_t rng_seed;                /* for deterministic Thompson samples */
} hu_contextual_bandit_t;

/* Create a bandit with capacity for num_contacts. Each arm initializes to
 * Beta(1, 1). Returns HU_OK or HU_ERR_OUT_OF_MEMORY. */
hu_error_t hu_contextual_bandit_create(hu_allocator_t *alloc, size_t capacity,
                                       hu_contextual_bandit_t **out);

/* Destroy and free all bandit state. */
void hu_contextual_bandit_destroy(hu_contextual_bandit_t *bandit);

/* Thompson sample and decide: return true if θ > threshold.
 * If contact_handle is not yet in arms[], initialize a new arm and add it.
 * Deterministic if rng_seed is set (test mode). */
hu_error_t hu_contextual_bandit_decide_send(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                            bool *out_should_send);

/* Update an arm based on outcome. Initializes the arm if not yet present.
 * Returns HU_OK or HU_ERR_OUT_OF_MEMORY (if capacity exhausted). */
hu_error_t hu_contextual_bandit_update(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                       hu_bandit_outcome_t outcome);

/* Serialize bandit state to binary file. Atomic via tmp + fwrite + rename.
 * Returns HU_OK or HU_ERR_IO. */
hu_error_t hu_contextual_bandit_save(hu_contextual_bandit_t *bandit, const char *path);

/* Deserialize bandit state from binary file. Allocates arms array via alloc.
 * Returns HU_OK, HU_ERR_IO, or HU_ERR_OUT_OF_MEMORY. */
hu_error_t hu_contextual_bandit_load(hu_allocator_t *alloc, const char *path,
                                     hu_contextual_bandit_t **out);

/* Retrieve an arm's current state for diagnostics (read-only). */
hu_error_t hu_contextual_bandit_get_arm(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                        hu_contextual_bandit_arm_t *out_arm);

/* Overwrite (or insert) an arm's posterior directly. For the persistence
 * layer — callers must pass already-validated values; no clamping here.
 * Returns HU_OK or HU_ERR_OUT_OF_MEMORY (capacity exhausted). */
hu_error_t hu_contextual_bandit_set_arm(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                        double alpha, double beta, uint64_t updates);

/* Internal: deterministic Beta(α, β) sampler. Public for testing.
 * Uses Marsaglia-Tsang method: sample two Gamma(α,1) and Gamma(β,1)
 * via exponentials, then θ = Γ(α) / (Γ(α) + Γ(β)). */
double hu_contextual_bandit_sample_beta(double alpha, double beta, uint32_t *inout_seed);

#ifdef __cplusplus
}
#endif

#endif
