/* src/agent/prompt_budget.c
 *
 * Per-field byte accounting + DEAD-field detection for the system prompt
 * builder. See docs/plans/2026-05-25-director-compression/.
 *
 * Pure functions over a file-scope struct — testable in isolation. Wrap-
 * up call sites in src/agent/prompt.c populate the stats array; this
 * module just accumulates and decides. */

#include "human/agent/prompt_budget.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Per-field running totals. File-scope struct (NOT anonymous) so pointers
 * and assignments compose cleanly across functions — the worker's earlier
 * attempt at this used three differently-scoped anonymous structs and
 * got -Wincompatible-pointer-types from clang. */
typedef struct prompt_field_accumulator {
    const char *name;           /* borrowed from stat->name (static string) */
    uint64_t total_bytes;       /* sum of all observed bytes for this field */
    uint64_t observation_count; /* count of observations including zero-byte */
    uint64_t non_empty_count;   /* observations where bytes_contributed > 0 */
} prompt_field_accumulator_t;

struct hu_prompt_budget {
    hu_allocator_t *alloc;
    prompt_field_accumulator_t fields[HU_PROMPT_FIELD_COUNT];
    size_t observation_count; /* total turns observed (across all fields) */
};

/* Stable display names. Indexed by hu_prompt_field_t. The trailing
 * sentinel keeps array bounds explicit so adding a new field forces an
 * update of HU_PROMPT_FIELD_COUNT (build break catches the omission). */
static const char *const s_field_names[HU_PROMPT_FIELD_COUNT] = {
    [HU_PROMPT_FIELD_MEMORY_CONTEXT] = "memory_context",
    [HU_PROMPT_FIELD_PERSONAL_MODEL_CONTEXT] = "personal_model_context",
    [HU_PROMPT_FIELD_MOMENT_CONTEXT] = "moment_context",
    [HU_PROMPT_FIELD_SELF_EXEMPLARS_CONTEXT] = "self_exemplars_context",
    [HU_PROMPT_FIELD_WORLD_MODEL_CONTEXT] = "world_model_context",
    [HU_PROMPT_FIELD_RELATIONAL_EPISODE_CONTEXT] = "relational_episode_context",
    [HU_PROMPT_FIELD_INSTRUCTION_CONTEXT] = "instruction_context",
    [HU_PROMPT_FIELD_STM_CONTEXT] = "stm_context",
    [HU_PROMPT_FIELD_CONTACT_CONTEXT] = "contact_context",
    [HU_PROMPT_FIELD_CONVERSATION_CONTEXT] = "conversation_context",
    [HU_PROMPT_FIELD_AWARENESS_CONTEXT] = "awareness_context",
    [HU_PROMPT_FIELD_OUTCOME_CONTEXT] = "outcome_context",
    [HU_PROMPT_FIELD_INTELLIGENCE_CONTEXT] = "intelligence_context",
    [HU_PROMPT_FIELD_SKILLS_CONTEXT] = "skills_context",
    [HU_PROMPT_FIELD_EMOTIONAL_CONTEXT] = "emotional_context",
    [HU_PROMPT_FIELD_COMMITMENT_CONTEXT] = "commitment_context",
    [HU_PROMPT_FIELD_PATTERN_CONTEXT] = "pattern_context",
    [HU_PROMPT_FIELD_ADAPTIVE_PERSONA_CONTEXT] = "adaptive_persona_context",
    [HU_PROMPT_FIELD_PROACTIVE_CONTEXT] = "proactive_context",
    [HU_PROMPT_FIELD_SUPERHUMAN_CONTEXT] = "superhuman_context",
    [HU_PROMPT_FIELD_PERSONA_PROMPT] = "persona_prompt",
    [HU_PROMPT_FIELD_CUSTOM_INSTRUCTIONS] = "custom_instructions",
    [HU_PROMPT_FIELD_PREFERENCES] = "preferences",
    [HU_PROMPT_FIELD_TONE_HINT] = "tone_hint",
    [HU_PROMPT_FIELD_SOMATIC_CONTEXT] = "somatic_context",
    [HU_PROMPT_FIELD_RUPTURE_CONTEXT] = "rupture_context",
    [HU_PROMPT_FIELD_VOICE_MATURITY_DIRECTIVE] = "voice_maturity_directive",
};

const char *hu_prompt_field_name(hu_prompt_field_t field) {
    if ((int)field < 0 || (int)field >= HU_PROMPT_FIELD_COUNT)
        return NULL;
    return s_field_names[field];
}

hu_error_t hu_prompt_budget_init(hu_allocator_t *alloc, hu_prompt_budget_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_prompt_budget_t *b = (hu_prompt_budget_t *)alloc->alloc(alloc->ctx, sizeof(*b));
    if (!b)
        return HU_ERR_OUT_OF_MEMORY;
    memset(b, 0, sizeof(*b));
    b->alloc = alloc;
    /* Pre-populate field name pointers from the static table so even
     * a budget that has never observed a turn can still report names. */
    for (size_t i = 0; i < HU_PROMPT_FIELD_COUNT; i++) {
        b->fields[i].name = s_field_names[i];
    }
    *out = b;
    return HU_OK;
}

void hu_prompt_budget_free(hu_prompt_budget_t *b) {
    if (!b)
        return;
    hu_allocator_t *alloc = b->alloc;
    if (alloc)
        alloc->free(alloc->ctx, b, sizeof(*b));
}

void hu_prompt_budget_observe(hu_prompt_budget_t *b, const hu_prompt_field_stat_t *stats,
                              size_t count) {
    if (!b || !stats || count == 0)
        return;
    size_t n = count < HU_PROMPT_FIELD_COUNT ? count : HU_PROMPT_FIELD_COUNT;
    for (size_t i = 0; i < n; i++) {
        prompt_field_accumulator_t *f = &b->fields[i];
        /* Adopt the name pointer if the stat carries one (in case the
         * caller used a different static string than our default). */
        if (stats[i].name)
            f->name = stats[i].name;
        f->observation_count++;
        f->total_bytes += (uint64_t)stats[i].bytes_contributed;
        if (stats[i].bytes_contributed > 0)
            f->non_empty_count++;
    }
    b->observation_count++;
}

size_t hu_prompt_budget_observation_count(const hu_prompt_budget_t *b) {
    if (!b)
        return 0;
    return b->observation_count;
}

bool hu_prompt_budget_field_is_dead(const hu_prompt_budget_t *b, hu_prompt_field_t field,
                                    size_t min_bytes_threshold, size_t min_sample_count) {
    if (!b)
        return false;
    if ((int)field < 0 || (int)field >= HU_PROMPT_FIELD_COUNT)
        return false;
    const prompt_field_accumulator_t *f = &b->fields[field];
    /* Need enough observations to make a confident claim. Until then
     * the field cannot be DEAD — telemetric value: a brand-new field
     * is "unknown," not "dead." */
    if (f->observation_count < (uint64_t)min_sample_count)
        return false;
    uint64_t mean = f->total_bytes / f->observation_count;
    return mean < (uint64_t)min_bytes_threshold;
}

size_t hu_prompt_budget_snapshot(const hu_prompt_budget_t *b, hu_prompt_field_stat_t *out_array,
                                 size_t array_cap) {
    if (!b || !out_array || array_cap == 0)
        return 0;
    size_t n = array_cap < HU_PROMPT_FIELD_COUNT ? array_cap : HU_PROMPT_FIELD_COUNT;
    for (size_t i = 0; i < n; i++) {
        const prompt_field_accumulator_t *f = &b->fields[i];
        out_array[i].name = f->name;
        /* Snapshot reports the MEAN bytes per observation — that's
         * what dead-field detection compares against the threshold. */
        out_array[i].bytes_contributed =
            f->observation_count > 0 ? (size_t)(f->total_bytes / f->observation_count) : 0;
    }
    return n;
}
