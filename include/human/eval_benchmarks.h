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
    HU_BENCHMARK_LONGMEMEVAL,
    HU_BENCHMARK_LOCOMO,
    HU_BENCHMARK_KNOWU_BENCH,
    HU_BENCHMARK_EMPA,
    HU_BENCHMARK_PROAGENTBENCH,
} hu_benchmark_type_t;

hu_error_t hu_benchmark_load(hu_allocator_t *alloc, hu_benchmark_type_t type, const char *json,
                             size_t json_len, hu_eval_suite_t *out);

const char *hu_benchmark_type_name(hu_benchmark_type_t type);

#endif
