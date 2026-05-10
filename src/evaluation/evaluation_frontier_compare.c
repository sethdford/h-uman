/* W16 — Frontier-Compare backend.
 *
 * STUB. Real wiring will run identical conversation transcripts through
 *   - h-uman with full memory enabled,
 *   - GPT-5 (no memory),
 *   - Gemini-3.1-pro (no memory),
 *   - Claude-Opus-5 (no memory),
 * and score paired outputs with an LLM judge. That requires three API key
 * env vars (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `GOOGLE_API_KEY`), a
 * provider router, and a sampling budget — out of scope for this commit.
 *
 * Behavior of this stub:
 *   - `available()` returns true iff at least one API key env var is set.
 *   - `run()` returns HU_ERR_CONFIG_NOT_FOUND when no key is set.
 *   - With at least one key set, returns a deterministic placeholder score
 *     of 0.50 and emits an `error_summary` documenting that real frontier
 *     calls require integration. Tests pass `HU_IS_TEST` so the path stays
 *     deterministic without touching real APIs.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const FRONTIER_API_ENV_KEYS[] = {
    "OPENAI_API_KEY",
    "ANTHROPIC_API_KEY",
    "GOOGLE_API_KEY",
};
static const size_t FRONTIER_API_ENV_N =
    sizeof(FRONTIER_API_ENV_KEYS) / sizeof(FRONTIER_API_ENV_KEYS[0]);

/* Placeholder paired score. Real wiring will replace this with a per-pair
 * scoring loop; the deterministic constant lets tests assert pair stability. */
#define FRONTIER_PLACEHOLDER_SCORE 0.50
#define FRONTIER_PLACEHOLDER_PAIRS 5

typedef struct {
    int unused;
} frontier_ctx_t;

/* ── helpers ────────────────────────────────────────────────────────────── */

static bool any_api_key_set(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Tests assume the placeholder path is reachable; real key lookup runs in
     * production builds only. */
    return true;
#else
    for (size_t i = 0; i < FRONTIER_API_ENV_N; i++) {
        const char *v = getenv(FRONTIER_API_ENV_KEYS[i]);
        if (v && v[0])
            return true;
    }
    return false;
#endif
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const char *frontier_name(void *ctx) {
    (void)ctx;
    return "frontier_compare";
}

static bool frontier_available(void *ctx) {
    (void)ctx;
    return any_api_key_set();
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

static hu_error_t frontier_run(void *ctx, hu_allocator_t *alloc,
                               hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!any_api_key_set())
        return HU_ERR_CONFIG_NOT_FOUND;

    hu_error_t err = hu_evaluation_report_init(alloc, "frontier_compare", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    err = hu_evaluation_report_add_metric(alloc, out, "score", FRONTIER_PLACEHOLDER_SCORE,
                                          FRONTIER_PLACEHOLDER_PAIRS);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    err = hu_evaluation_report_set_error(
        alloc, out, "stub: real frontier provider integration pending");
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    err = hu_evaluation_report_set_model(alloc, out, "frontier_placeholder");
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }

    out->prompts_total = FRONTIER_PLACEHOLDER_PAIRS;
    out->prompts_passed = 0;
    out->prompts_failed = 0;
    out->finished_at_ms = now_ms();
    return HU_OK;
}

static void frontier_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(frontier_ctx_t));
}

static const hu_evaluation_vtable_t FRONTIER_VTABLE = {
    .name = frontier_name,
    .available = frontier_available,
    .run = frontier_run,
    .deinit = frontier_deinit,
};

hu_error_t hu_evaluation_frontier_compare(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    frontier_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(frontier_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &FRONTIER_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
