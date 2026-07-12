/* outbound/pipeline_configs.c — per-path stage selection.
 *
 * Each outbound path gets its own ordered list of stages. The table
 * lives here as static singletons; the pipeline borrows them (does
 * not own them).
 *
 * Stage selection rationale (design.md §"Per-path stage selection"):
 *
 *   reactive   — response_guard already handles most validation;
 *                we add crosstalk so the incident pattern cannot
 *                hit reactive either.
 *   proactive  — full pipeline. This is where Annie/Mindy/Betty
 *                shipped. Highest scrutiny.
 *   f25        — full pipeline + upstream Phase C fix at
 *                daemon_proactive.c:424.
 *   temporal   — strip + shape + crosstalk + persona + moderation.
 *                No echo stage because temporal prompts don't have
 *                the same directive-injection vector.
 *   scheduled  — strip + crosstalk + moderation. Light path; the
 *                content is usually user-authored.
 *   burst      — inherits primary's verdict. The pipeline is not
 *                run for burst sub-sends; they pass-through.
 *
 * Adding a new path: extend hu_outbound_path_t, then add a config
 * row here. That's it.
 */

#include "human/agent/outbound_pipeline.h"

#include <stdlib.h>
#include <string.h>

/* Stage singletons — implemented in their own .c files. Each file
 * exports a `hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_<name>` symbol. */
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_strip;
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_shape;
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_echo;
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_crosstalk;
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_persona;
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_moderation;
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_style_governor;

/* Per-path stage lists. NULL-terminated for ease of static
 * declaration. The build_stages function below converts to the
 * count-and-array form the pipeline expects. */
static hu_outbound_pipeline_stage_t *s_reactive_stages[] = {
    &hu_outbound_pipeline_stage_strip,
    &hu_outbound_pipeline_stage_crosstalk,
    &hu_outbound_pipeline_stage_style_governor,
    NULL,
};

static hu_outbound_pipeline_stage_t *s_proactive_stages[] = {
    &hu_outbound_pipeline_stage_strip,
    &hu_outbound_pipeline_stage_shape,
    &hu_outbound_pipeline_stage_echo,
    &hu_outbound_pipeline_stage_crosstalk,
    &hu_outbound_pipeline_stage_persona,
    &hu_outbound_pipeline_stage_moderation,
    &hu_outbound_pipeline_stage_style_governor,
    NULL,
};

static hu_outbound_pipeline_stage_t *s_f25_stages[] = {
    &hu_outbound_pipeline_stage_strip,
    &hu_outbound_pipeline_stage_shape,
    &hu_outbound_pipeline_stage_echo,
    &hu_outbound_pipeline_stage_crosstalk,
    &hu_outbound_pipeline_stage_persona,
    &hu_outbound_pipeline_stage_moderation,
    &hu_outbound_pipeline_stage_style_governor,
    NULL,
};

static hu_outbound_pipeline_stage_t *s_temporal_stages[] = {
    &hu_outbound_pipeline_stage_strip,          &hu_outbound_pipeline_stage_shape,
    &hu_outbound_pipeline_stage_crosstalk,      &hu_outbound_pipeline_stage_persona,
    &hu_outbound_pipeline_stage_moderation,     &hu_outbound_pipeline_stage_style_governor,
    NULL,
};

static hu_outbound_pipeline_stage_t *s_scheduled_stages[] = {
    &hu_outbound_pipeline_stage_strip,
    &hu_outbound_pipeline_stage_crosstalk,
    &hu_outbound_pipeline_stage_moderation,
    NULL,
};

/* Burst inherits primary verdict — pipeline NOT run. Caller-side
 * convention; pipeline_for_path returns an empty stages list. */
static hu_outbound_pipeline_stage_t *s_burst_stages[] = {
    NULL,
};

static hu_outbound_pipeline_stage_t **stages_for_path(hu_outbound_path_t path, size_t *out_count) {
    hu_outbound_pipeline_stage_t **list = NULL;
    switch (path) {
    case HU_OUTBOUND_PATH_REACTIVE:
        list = s_reactive_stages;
        break;
    case HU_OUTBOUND_PATH_PROACTIVE:
        list = s_proactive_stages;
        break;
    case HU_OUTBOUND_PATH_F25:
        list = s_f25_stages;
        break;
    case HU_OUTBOUND_PATH_TEMPORAL:
        list = s_temporal_stages;
        break;
    case HU_OUTBOUND_PATH_SCHEDULED:
        list = s_scheduled_stages;
        break;
    case HU_OUTBOUND_PATH_BURST:
        list = s_burst_stages;
        break;
    case HU_OUTBOUND_PATH_COUNT:
        list = NULL;
        break;
    }
    if (!list) {
        *out_count = 0;
        return NULL;
    }
    size_t n = 0;
    while (list[n] != NULL)
        n++;
    *out_count = n;
    return list;
}

hu_error_t hu_outbound_pipeline_configs_build_stages(hu_allocator_t *alloc, hu_outbound_path_t path,
                                                     hu_outbound_pipeline_stage_t ***out_stages,
                                                     size_t *out_count) {
    if (!alloc || !out_stages || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_stages = NULL;
    *out_count = 0;

    size_t n = 0;
    hu_outbound_pipeline_stage_t **src = stages_for_path(path, &n);
    if (n == 0) {
        /* Burst / unknown — return empty list, not an error.
         * The pipeline runner handles a zero-stage pipeline as
         * "always SEND". */
        return HU_OK;
    }

    hu_outbound_pipeline_stage_t **arr = (hu_outbound_pipeline_stage_t **)alloc->alloc(alloc->ctx, n * sizeof(*arr));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < n; i++)
        arr[i] = src[i];
    *out_stages = arr;
    *out_count = n;
    return HU_OK;
}
