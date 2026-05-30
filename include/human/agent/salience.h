#ifndef HU_AGENT_SALIENCE_H
#define HU_AGENT_SALIENCE_H

/* Salience / arbitration layer (coherence — Gap 1).
 *
 * h-uman computes many behavioral "directives" per turn and historically emitted
 * all that fit a buffer ("a committee, not a person"). This layer ranks candidate
 * directives and selects only the most salient few, so the assembled persona reads
 * as one coherent voice. It REUSES the existing arbitrator engine
 * (human/agent/arbitrator.h) — hu_directive_compute_priority + hu_arbitrator_select —
 * and adds three things the engine lacks:
 *
 *   P3  canonicalization : map a directive's source name -> category + never-suppress
 *                          floor (safety / grief / conflict / direct-question are
 *                          REQUIRED and bypass selection).
 *   P2  Seth profile     : per-source weight so ranking reflects what *Seth* foregrounds
 *                          rather than a generic human.
 *   P4  shadow rank      : apply the profile weight to each candidate's priority, run
 *                          the arbitrator, and report kept-vs-suppressed for SHADOW-mode
 *                          logging (observe without changing emitted behavior).
 *
 * Pure and side-effect free (allocator only) so each piece is unit-testable without
 * the turn loop. Lives in src/agent/ alongside the arbitrator; classifies by directive
 * SOURCE strings only (no provider/channel identity) per agent-core-boundary.md.
 */

#include "human/agent/arbitrator.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- P3: canonicalization ------------------------------------------------ */

/* Map a directive source name (e.g. "emotional_checkin", "inside_joke",
 * "shared_reference") to a bitfield of HU_DIRECTIVE_* categories. Uses
 * word-boundary keyword matching (per substring-classifier-pitfalls.md) so
 * "unfriendly" does not match "friend". Returns HU_DIRECTIVE_BEHAVIORAL when no
 * keyword matches (the neutral default). */
uint32_t hu_salience_classify_source(const char *source, size_t source_len);

/* True when a source must NEVER be suppressed regardless of score: emotional
 * crisis (grief, loss, conflict), explicit safety, and direct-question handling.
 * Maps to hu_directive_t.required. */
bool hu_salience_source_is_required(const char *source, size_t source_len);

/* Build a ready-to-rank candidate from a (source, content) pair: fills category
 * (classify), required (floor), token_cost (estimate), and copies source/content.
 * Caller frees via hu_directive_deinit. */
hu_error_t hu_salience_build_candidate(hu_allocator_t *alloc, const char *source, size_t source_len,
                                       const char *content, size_t content_len,
                                       hu_directive_t *out);

/* ---- P2: Seth profile ---------------------------------------------------- */

#define HU_SALIENCE_PROFILE_MAX_WEIGHTS 32

typedef struct hu_salience_source_weight {
    char keyword[40]; /* source substring this weight applies to (word-boundary) */
    double weight;    /* multiplier on the directive's base priority, [0, 4] */
} hu_salience_source_weight_t;

typedef struct hu_salience_profile {
    char name[48]; /* e.g. "seth-default" */
    hu_salience_source_weight_t weights[HU_SALIENCE_PROFILE_MAX_WEIGHTS];
    size_t weight_count;
    double default_weight; /* applied when nothing matches */
} hu_salience_profile_t;

/* Initialize a sane default profile (default_weight 1.0 + a few empirically-shaped
 * weights, e.g. memory/shared-reference up, generic-curiosity down). Never fails. */
void hu_salience_profile_init_default(hu_salience_profile_t *p);

/* Weight for a source: the matching keyword's weight (word-boundary, first match)
 * or the profile default. Returns 1.0 when p is NULL. */
double hu_salience_profile_weight(const hu_salience_profile_t *p, const char *source,
                                  size_t source_len);

/* ---- P4: shadow rank ----------------------------------------------------- */

/* Apply each candidate's Seth-profile weight to its priority (in place), then run
 * the arbitrator to select the top directives within budget. `profile` may be NULL
 * (no modulation). `config` may be NULL (arbitrator defaults). On success `result`
 * holds the selected copies; free via hu_arbitration_result_deinit. */
hu_error_t hu_salience_rank(hu_allocator_t *alloc, hu_directive_t *candidates,
                            size_t candidate_count, const hu_salience_profile_t *profile,
                            const hu_arbitration_config_t *config, hu_arbitration_result_t *result);

/* Build a human-readable one-line shadow summary of a ranking decision, e.g.
 *   "salience(shadow): kept 2/9 [shared_reference,emotional_checkin] suppressed 7
 *    [curiosity,absence,somatic,...]"
 * for operator logs. Allocated; caller frees. Never fails fatally (returns a short
 * fallback on alloc failure). */
char *hu_salience_summarize(hu_allocator_t *alloc, const hu_directive_t *candidates,
                            size_t candidate_count, const hu_arbitration_result_t *result);

#endif /* HU_AGENT_SALIENCE_H */
