#ifndef HU_EVAL_PUBLIC_SUITES_H
#define HU_EVAL_PUBLIC_SUITES_H

/* Init #14 — Public benchmark suite expansion (S1 adapter shims).
 *
 * Wires LongMemEval, LoCoMo, KnowU-Bench, EMPA, and ProAgentBench into the
 * existing hu_eval_* runner as deterministic offline smoke gates.  Each
 * benchmark ships a tiny held-out fixture under
 *   tests/fixtures/benchmarks/<name>/smoke.json
 * with synthetic personas only (no PII, no real users).  The synchronous
 * CLI path (`human eval public-benchmark <name>`) loads the fixture, runs
 * it against the configured provider (or the HU_IS_TEST mock), and prints
 * a structured JSON report against a checked-in regression floor.
 *
 * Full-mode runs (`docs/plans/2026-05-11-init-14-public-benchmarks.md`
 * §Phase P4–P6) are deferred to a later sprint; the W16 vtable
 * (`hu_evaluation_t`) remains the home for category-aware scoring once
 * the cloud-judge plumbing lands.  S1 ships shims + floors only.
 *
 * Locked conventions consumed here:
 *   - HU_JOB_KIND_BENCHMARK = 7  (from
 *     docs/plans/2026-05-11-sota-2026-massive-team-program.md, "hu_job_kind_t
 *     enum allocation").  Reserved for the future longitudinal scheduler
 *     path; the synchronous CLI does not use it yet.
 *
 * See also:
 *   - include/human/eval.h          (generic suite runner)
 *   - include/human/eval_benchmarks.h (sister enum dispatch)
 *   - docs/benchmarks/README.md     (how to run each benchmark)
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Locked scheduler ordinal for the benchmark job kind.
 *
 * This is the cross-initiative-locked value from
 * `docs/plans/2026-05-11-sota-2026-massive-team-program.md` §"Locked
 * conventions / hu_job_kind_t enum allocation".  Initiative #10 owns the
 * forthcoming `hu_job_kind_t` reform that will fold this ordinal into the
 * shared scheduler enum.  Init #14's synchronous CLI does not call the
 * scheduler, so we reserve the ordinal as a constant here to prevent
 * future allocators from colliding.
 */
#ifndef HU_JOB_KIND_BENCHMARK
#define HU_JOB_KIND_BENCHMARK 7
#endif

typedef enum {
    HU_PUBLIC_BENCHMARK_LONGMEMEVAL = 0,
    HU_PUBLIC_BENCHMARK_LOCOMO,
    HU_PUBLIC_BENCHMARK_KNOWU,
    HU_PUBLIC_BENCHMARK_EMPA,
    HU_PUBLIC_BENCHMARK_PROAGENTBENCH,
    HU_PUBLIC_BENCHMARK_COUNT
} hu_public_benchmark_t;

typedef struct hu_public_benchmark_result {
    char *name;            /* short id, e.g. "longmemeval"; allocator-owned */
    size_t tasks_run;
    size_t tasks_passed;
    double score;          /* 0.0–1.0 (== pass_rate) */
    double floor;          /* checked-in regression floor */
    bool passed_floor;     /* score >= floor */
    int64_t elapsed_ms;
} hu_public_benchmark_result_t;

/* Stable short name (e.g. "longmemeval"). Returns "" for invalid input. */
const char *hu_public_benchmark_name(hu_public_benchmark_t b);

/* Path to the committed smoke fixture relative to the repo root.
 * NULL for invalid input. */
const char *hu_public_benchmark_fixture_path(hu_public_benchmark_t b);

/* Checked-in regression floor (0.0–1.0). 0.0 for invalid input.
 *
 * Floors are intentionally a small margin below the deterministic
 * pass rate the smoke fixture is constructed to produce, so the gate
 * fires on real regressions but tolerates harmless rounding.
 */
double hu_public_benchmark_floor(hu_public_benchmark_t b);

/* Parse a short name like "longmemeval", "locomo", "knowu", "empa",
 * "proagentbench" into the enum. Returns true on success. */
bool hu_public_benchmark_from_string(const char *name, hu_public_benchmark_t *out);

/* Run a benchmark in smoke mode against `provider` (or the HU_IS_TEST
 * mock if HU_IS_TEST is set).  Reads the committed fixture from disk;
 * see hu_public_benchmark_fixture_path.  Populates *out with the
 * scoring summary including pass-vs-floor.  Caller must free with
 * hu_public_benchmark_result_free.
 *
 * Returns HU_ERR_INVALID_ARGUMENT for unknown enum values or NULL out.
 * Returns HU_ERR_IO if the fixture path cannot be read.  Returns HU_OK
 * even if the regression floor failed — inspect `passed_floor`.
 */
hu_error_t hu_public_benchmark_run_smoke(hu_allocator_t *alloc, hu_public_benchmark_t b,
                                         hu_provider_t *provider, const char *model,
                                         size_t model_len,
                                         hu_public_benchmark_result_t *out);

void hu_public_benchmark_result_free(hu_allocator_t *alloc,
                                     hu_public_benchmark_result_t *r);

/* Render a benchmark result as a stable JSON object on a single line.
 * Caller frees *out_json via alloc->free(*out_json, *out_len + 1). */
hu_error_t hu_public_benchmark_result_to_json(hu_allocator_t *alloc,
                                              const hu_public_benchmark_result_t *r,
                                              char **out_json, size_t *out_len);

/* Write JSON results to `path` atomically (tmp + fwrite + fflush + fsync
 * + rename), the same pattern that test_personal_model_atomic_save.c
 * pins.  Returns HU_ERR_IO if any step fails; the destination is never
 * partially written.  HU_IS_TEST guard: on test builds this writes
 * to a `.tmp` sibling first so adversarial tests can pre-block it.
 */
hu_error_t hu_public_benchmark_publish_results(const char *path, const char *json,
                                               size_t json_len);

/* Scan fixture JSON for known PII anti-patterns (real email TLDs other
 * than example.com, US SSN shapes, etc.).  Returns HU_OK iff the fixture
 * contains only neutral synthetic markers.  HU_ERR_INVALID_ARGUMENT if a
 * disallowed marker is found.  Used by both the loader and a
 * regression-only test in tests/test_eval_public_suites.c.
 */
hu_error_t hu_public_benchmark_check_fixture_privacy(const char *json, size_t json_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_PUBLIC_SUITES_H */
