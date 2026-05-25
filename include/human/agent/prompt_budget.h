#ifndef HU_AGENT_PROMPT_BUDGET_H
#define HU_AGENT_PROMPT_BUDGET_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Prompt-budget instrumentation — per-field byte accounting for the system
 * prompt builder.
 *
 * See docs/plans/2026-05-25-director-compression/{requirements,design,tasks}.md.
 *
 * Phase 1 ships MEASUREMENT only:
 *   - hu_prompt_field_stat_t — per-field byte count snapshot from a single
 *     hu_prompt_build_system call.
 *   - hu_prompt_budget_t — accumulator that observes stats across many
 *     calls and identifies DEAD fields (mean bytes < threshold).
 *   - Config block (hu_prompt_budget_config_t) gates the future trim
 *     behavior. Phase 1's gate is OFF by default; Phase 2 (Task 4) wires
 *     the actual trim. */

/* Total number of named context fields the builder tracks. Indexed by
 * hu_prompt_field_t below. Keep in sync with the wrapper macro in
 * src/agent/prompt.c — increment this when adding a tracked field. */
#define HU_PROMPT_FIELD_COUNT 27

/* Stable field indices. Used by both the appender call sites and the
 * test harness; the names are exposed via hu_prompt_field_name(). */
typedef enum hu_prompt_field {
    HU_PROMPT_FIELD_MEMORY_CONTEXT = 0,
    HU_PROMPT_FIELD_PERSONAL_MODEL_CONTEXT,
    HU_PROMPT_FIELD_MOMENT_CONTEXT,
    HU_PROMPT_FIELD_SELF_EXEMPLARS_CONTEXT,
    HU_PROMPT_FIELD_WORLD_MODEL_CONTEXT,
    HU_PROMPT_FIELD_RELATIONAL_EPISODE_CONTEXT,
    HU_PROMPT_FIELD_INSTRUCTION_CONTEXT,
    HU_PROMPT_FIELD_STM_CONTEXT,
    HU_PROMPT_FIELD_CONTACT_CONTEXT,
    HU_PROMPT_FIELD_CONVERSATION_CONTEXT,
    HU_PROMPT_FIELD_AWARENESS_CONTEXT,
    HU_PROMPT_FIELD_OUTCOME_CONTEXT,
    HU_PROMPT_FIELD_INTELLIGENCE_CONTEXT,
    HU_PROMPT_FIELD_SKILLS_CONTEXT,
    HU_PROMPT_FIELD_EMOTIONAL_CONTEXT,
    HU_PROMPT_FIELD_COMMITMENT_CONTEXT,
    HU_PROMPT_FIELD_PATTERN_CONTEXT,
    HU_PROMPT_FIELD_ADAPTIVE_PERSONA_CONTEXT,
    HU_PROMPT_FIELD_PROACTIVE_CONTEXT,
    HU_PROMPT_FIELD_SUPERHUMAN_CONTEXT,
    HU_PROMPT_FIELD_PERSONA_PROMPT,
    HU_PROMPT_FIELD_CUSTOM_INSTRUCTIONS,
    HU_PROMPT_FIELD_PREFERENCES,
    HU_PROMPT_FIELD_TONE_HINT,
    HU_PROMPT_FIELD_SOMATIC_CONTEXT,
    HU_PROMPT_FIELD_RUPTURE_CONTEXT,
    HU_PROMPT_FIELD_VOICE_MATURITY_DIRECTIVE,
} hu_prompt_field_t;

/* Per-field stats from ONE prompt-build call. The stats array passed to
 * hu_prompt_build_system_with_stats is indexed by hu_prompt_field_t. */
typedef struct hu_prompt_field_stat {
    const char *name;         /* static string; borrowed, do not free */
    size_t bytes_contributed; /* bytes this field added to the prompt */
} hu_prompt_field_stat_t;

/* Stable display name for a field index (returns NULL for out-of-range). */
const char *hu_prompt_field_name(hu_prompt_field_t field);

/* Opaque accumulator that observes per-field stats across many turns
 * and flags fields that consistently contribute < threshold bytes. */
typedef struct hu_prompt_budget hu_prompt_budget_t;

hu_error_t hu_prompt_budget_init(hu_allocator_t *alloc, hu_prompt_budget_t **out);

void hu_prompt_budget_free(hu_prompt_budget_t *b);

/* Record one turn's worth of stats. The stats array is borrowed; the
 * budget accumulates running totals. count must be HU_PROMPT_FIELD_COUNT
 * (or fewer — only the leading `count` entries are read). */
void hu_prompt_budget_observe(hu_prompt_budget_t *b, const hu_prompt_field_stat_t *stats,
                              size_t count);

/* Returns the number of observations recorded. */
size_t hu_prompt_budget_observation_count(const hu_prompt_budget_t *b);

/* Pure decision predicate: is `field` DEAD?
 *
 * Returns true iff:
 *   1. observation_count >= min_sample_count (we have enough data)
 *   2. mean bytes for this field < min_bytes_threshold
 *
 * Returns false on NULL budget, out-of-range field, or insufficient
 * observations — so an UNINITIALIZED field never accidentally trims as
 * DEAD. */
bool hu_prompt_budget_field_is_dead(const hu_prompt_budget_t *b, hu_prompt_field_t field,
                                    size_t min_bytes_threshold, size_t min_sample_count);

/* Snapshot CURRENT per-field mean bytes into out_array. Returns the
 * number of entries populated (capped at array_cap). Order matches
 * hu_prompt_field_t indices for entries 0..min(count, array_cap). */
size_t hu_prompt_budget_snapshot(const hu_prompt_budget_t *b, hu_prompt_field_stat_t *out_array,
                                 size_t array_cap);

#endif /* HU_AGENT_PROMPT_BUDGET_H */
