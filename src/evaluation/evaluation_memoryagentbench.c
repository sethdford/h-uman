/* W16 — MemoryAgentBench backend.
 *
 * STUB. The real harness — see paper arxiv:2503.06745 — drives multiple
 * agents across a shared memory and scores coordination quality. It is a
 * later-phase deliverable because it requires:
 *   1. A multi-agent orchestrator built on `src/agent/spawn.c`.
 *   2. A shared `hu_memory_t` instance routed through the W7 facade.
 *   3. Real conversation transcripts (gated under license review).
 *
 * For now this backend reports a deterministic score against a tiny
 * three-scenario inline set. `available()` returns true so CI can include
 * the suite end-to-end and the regression gate has a stable baseline.
 *
 * Replace the stub by:
 *   1. Loading scenarios from `eval_suites/memoryagentbench/<name>.json`.
 *   2. Spawning agent threads via the existing spawn API.
 *   3. Recording shared-memory reads/writes with provenance.
 *   4. Scoring per the paper's coordination rubric.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *scenario;
    /* score per scenario in [0.0, 1.0]; deterministic placeholder until the
     * real harness lands. */
    double placeholder_score;
} mab_scenario_t;

static const mab_scenario_t MAB_SCENARIOS[] = {
    {"two-agent-handoff", 0.62},
    {"three-agent-debate", 0.55},
    {"shared-doc-edit", 0.71},
};

static const size_t MAB_N = sizeof(MAB_SCENARIOS) / sizeof(MAB_SCENARIOS[0]);

typedef struct {
    int unused;
} mab_ctx_t;

static const char *mab_name(void *ctx) {
    (void)ctx;
    return "memoryagentbench";
}

static bool mab_available(void *ctx) {
    (void)ctx;
    /* Stub backend is always "available"; documented above. */
    return true;
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

static hu_error_t mab_run(void *ctx, hu_allocator_t *alloc, hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, "memoryagentbench", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    double sum = 0.0;
    for (size_t i = 0; i < MAB_N; i++)
        sum += MAB_SCENARIOS[i].placeholder_score;
    double mean = MAB_N == 0 ? 0.0 : sum / (double)MAB_N;

    err = hu_evaluation_report_add_metric(alloc, out, "score", mean, MAB_N);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    err = hu_evaluation_report_set_error(alloc, out,
                                         "stub: real MemoryAgentBench harness pending");
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }

    out->prompts_total = MAB_N;
    out->prompts_passed = 0;
    out->prompts_failed = 0;
    out->finished_at_ms = now_ms();
    return HU_OK;
}

static void mab_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(mab_ctx_t));
}

static const hu_evaluation_vtable_t MAB_VTABLE = {
    .name = mab_name,
    .available = mab_available,
    .run = mab_run,
    .deinit = mab_deinit,
};

hu_error_t hu_evaluation_memoryagentbench(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    mab_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(mab_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &MAB_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
