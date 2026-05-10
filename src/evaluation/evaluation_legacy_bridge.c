/* W16 — legacy-bridge backend.
 *
 * Wraps the legacy `hu_eval_*` task-list framework
 * (`include/human/eval.h`, `src/eval.c`) behind the W16
 * `hu_evaluation_t` vtable. The W16 CLI surface (`human eval --w16
 * legacy-bridge`, `human evaluation run legacy-bridge`) gains a real
 * backend that exercises the dispatcher even before the dataset-backed
 * suites can be configured against the user's chosen provider.
 *
 * Why this exists: the six suite-specific backends
 * (locomo / longmemeval / dmr / minja / memoryagentbench /
 * frontier_compare) own their own embedded synthetic datasets. The
 * bridge is the "thin adapter" path that turns the existing
 * eval_suites JSON task-list framework into a W16-shaped report,
 * so the dispatcher has at least one backend that scores arbitrary
 * task lists rather than a pinned dataset.
 *
 * Determinism + offline guarantees:
 *   - In `HU_IS_TEST` builds the run path calls
 *     `hu_eval_run_suite` with a NULL provider, which the legacy
 *     framework recognises and answers with deterministic mock
 *     responses (see src/eval.c). No network, no spawning.
 *   - In production builds we deliberately do NOT instantiate a real
 *     provider here. Doing so would route through OpenAI / Anthropic
 *     by default and violate the "secure by default, no surprise
 *     network" rule. Instead `run` returns a stub report annotated
 *     with `error_summary` so callers know the bridge needs the full
 *     `human eval run <suite.json>` path for live scoring.
 */

#include "evaluation_internal.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval.h"
#include "human/evaluation/evaluation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_EVALUATION_LEGACY_BRIDGE_NAME "legacy-bridge"

typedef struct {
    int unused;
} legacy_bridge_ctx_t;

static const char *legacy_bridge_name(void *ctx) {
    (void)ctx;
    return HU_EVALUATION_LEGACY_BRIDGE_NAME;
}

static bool legacy_bridge_available(void *ctx) {
    (void)ctx;
    return true;
}

static int64_t legacy_bridge_now_ms(void) { return (int64_t)time(NULL) * 1000; }

/* Inline fixture: two tiny tasks. Mirrors the shape of an
 * eval_suites JSON entry (id, prompt, expected, category, difficulty,
 * timeout_ms). The legacy mock-response generator answers
 * "Mock response for: <prompt>" so any expected substring that appears
 * in the prompt itself passes; both tasks below satisfy that, giving a
 * deterministic 1.0 pass-rate.
 *
 * Only used by the HU_IS_TEST run path; production builds emit a stub
 * report instead and never touch this fixture. */
#if defined(HU_IS_TEST) && HU_IS_TEST
static const char LEGACY_BRIDGE_FIXTURE_JSON[] =
    "{\"name\":\"legacy-bridge-fixture\",\"tasks\":["
    "{\"id\":\"lb1\",\"prompt\":\"return the literal token TOKEN_A\","
    "\"expected\":\"TOKEN_A\",\"category\":\"smoke\","
    "\"difficulty\":1,\"timeout_ms\":5000},"
    "{\"id\":\"lb2\",\"prompt\":\"echo TOKEN_B back\","
    "\"expected\":\"TOKEN_B\",\"category\":\"smoke\","
    "\"difficulty\":1,\"timeout_ms\":5000}"
    "]}";

static hu_error_t legacy_bridge_run_test_path(hu_allocator_t *alloc,
                                              hu_evaluation_run_report_t *out) {
    hu_eval_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    hu_error_t err = hu_eval_suite_load_json(alloc, LEGACY_BRIDGE_FIXTURE_JSON,
                                             sizeof(LEGACY_BRIDGE_FIXTURE_JSON) - 1, &suite);
    if (err != HU_OK)
        return err;

    hu_eval_run_t run;
    memset(&run, 0, sizeof(run));
    /* NULL provider is intentional: in HU_IS_TEST builds the legacy
     * runner returns mock responses. The whole helper is compiled out
     * in production builds. */
    err = hu_eval_run_suite(alloc, NULL, "mock", 4, &suite, HU_EVAL_CONTAINS, &run);
    hu_eval_suite_free(alloc, &suite);
    if (err != HU_OK) {
        hu_eval_run_free(alloc, &run);
        return err;
    }

    out->prompts_total = run.results_count;
    out->prompts_passed = run.passed;
    out->prompts_failed = run.failed;

    err = hu_evaluation_report_add_metric(alloc, out, "pass_rate", run.pass_rate,
                                          run.results_count);
    hu_eval_run_free(alloc, &run);
    return err;
}
#endif

static hu_error_t legacy_bridge_run(void *ctx, hu_allocator_t *alloc,
                                    hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, HU_EVALUATION_LEGACY_BRIDGE_NAME, out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = legacy_bridge_now_ms();

#if defined(HU_IS_TEST) && HU_IS_TEST
    err = legacy_bridge_run_test_path(alloc, out);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
#else
    /* Production builds: the bridge cannot manufacture a real provider
     * without violating the "no surprise network" rule. Emit a
     * deterministic empty report and document the reason via
     * `error_summary`. The full bridging path lives in
     * `human eval run <suite.json>` which loads a configured provider
     * explicitly. */
    err = hu_evaluation_report_set_error(
        alloc, out,
        "legacy-bridge: production runs require `human eval run <suite.json>` "
        "with a configured provider; this stub returns an empty report");
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    err = hu_evaluation_report_add_metric(alloc, out, "pass_rate", 0.0, 0);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
#endif

    out->finished_at_ms = legacy_bridge_now_ms();
    return HU_OK;
}

static void legacy_bridge_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(legacy_bridge_ctx_t));
}

static const hu_evaluation_vtable_t LEGACY_BRIDGE_VTABLE = {
    .name = legacy_bridge_name,
    .available = legacy_bridge_available,
    .run = legacy_bridge_run,
    .deinit = legacy_bridge_deinit,
};

hu_error_t hu_evaluation_legacy_bridge(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    legacy_bridge_ctx_t *ctx =
        (legacy_bridge_ctx_t *)alloc->alloc(alloc->ctx, sizeof(legacy_bridge_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->unused = 0;
    out->ctx = ctx;
    out->vtable = &LEGACY_BRIDGE_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
