#ifndef HU_EVAL_BENCHMARKS_H
#define HU_EVAL_BENCHMARKS_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval.h"

typedef enum {
    HU_BENCHMARK_GAIA = 0,
    HU_BENCHMARK_SWE_BENCH,
    HU_BENCHMARK_TOOL_USE,
    HU_BENCHMARK_LIVE_AGENT,
    HU_BENCHMARK_APEX,
    /* Init #14 additions — public benchmark suite expansion.
     * Adapter shims live in src/eval_public_suites.c; see
     * docs/plans/2026-05-11-init-14-public-benchmarks.md §D1. */
    HU_BENCHMARK_LONGMEMEVAL,    /* arXiv:2410.10813 */
    HU_BENCHMARK_LOCOMO,         /* arXiv:2402.17753 + LoCoMo+ refresh */
    HU_BENCHMARK_KNOWU,          /* persona knowledge (pin-before-publish) */
    HU_BENCHMARK_EMPA,           /* arXiv:2603.00552 */
    HU_BENCHMARK_PROAGENTBENCH,  /* expected-utility gated proactivity */
} hu_benchmark_type_t;

hu_error_t hu_benchmark_load(hu_allocator_t *alloc, hu_benchmark_type_t type, const char *json,
                             size_t json_len, hu_eval_suite_t *out);

const char *hu_benchmark_type_name(hu_benchmark_type_t type);

#endif
