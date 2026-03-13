/* Autonomous experiment loop — core of autoresearch ported to C. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/experiment.h"
#include "human/ml/ml.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ─── Helpers ───────────────────────────────────────────────────────────── */

/* Simple djb2-style hash over experiment config bytes. */
static uint32_t hash_experiment_config(const hu_experiment_config_t *config)
{
    const unsigned char *p = (const unsigned char *)config;
    uint32_t h = 5381u;
    for (size_t i = 0; i < sizeof(hu_experiment_config_t); i++)
        h = ((h << 5) + h) + (unsigned)p[i];
    return h;
}

static const char *status_string(hu_experiment_status_t status)
{
    switch (status) {
    case HU_EXPERIMENT_KEEP:
        return "keep";
    case HU_EXPERIMENT_DISCARD:
        return "discard";
    case HU_EXPERIMENT_CRASH:
        return "crash";
    default:
        return "unknown";
    }
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_experiment_loop(hu_allocator_t *alloc,
                              const hu_experiment_loop_config_t *config,
                              hu_experiment_loop_callback_t callback,
                              void *user_data)
{
    if (!alloc || !config)
        return HU_ERR_INVALID_ARGUMENT;

    if (config->max_iterations < 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Initialize results tracking (no-op for Phase 1 scaffolding). */
    (void)alloc;

    for (int i = 0; i < config->max_iterations; i++) {
        /* TODO: Phase 4 — wire in agent system for full implementation.
         * For now, stub returns immediately. */
        (void)i;
        return HU_ERR_NOT_SUPPORTED;
    }

    /* Call callback with each result if callback is not NULL.
     * Phase 1: no results produced, so callback never invoked. */
    (void)callback;
    (void)user_data;

    return HU_OK;
}

hu_error_t hu_experiment_result_to_tsv(const hu_experiment_result_t *result,
                                       char *buf, size_t buf_size)
{
    if (!result || !buf || buf_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t config_hash = hash_experiment_config(&result->config);
    double peak_memory_gb = result->peak_memory_mb / 1024.0;
    const char *status = status_string(result->status);

    int n = snprintf(buf, buf_size,
                    "%u\t%.6f\t%.1f\t%s\t%s\n",
                    (unsigned)config_hash,
                    result->val_bpb,
                    peak_memory_gb,
                    status,
                    result->description);

    if (n < 0)
        return HU_ERR_INTERNAL;
    if ((size_t)n >= buf_size)
        return HU_ERR_INVALID_ARGUMENT; /* buffer too small */

    return HU_OK;
}
