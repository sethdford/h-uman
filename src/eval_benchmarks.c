#include "human/eval_benchmarks.h"
#include "human/core/string.h"
#include <string.h>

static const char *prefix_for_type(hu_benchmark_type_t type) {
    switch (type) {
    case HU_BENCHMARK_GAIA:
        return "gaia-";
    case HU_BENCHMARK_SWE_BENCH:
        return "swe-bench-";
    case HU_BENCHMARK_TOOL_USE:
        return "tool-use-";
    case HU_BENCHMARK_LIVE_AGENT:
        return "live-agent-";
    case HU_BENCHMARK_APEX:
        return "apex-";
    case HU_BENCHMARK_LONGMEMEVAL:
        return "longmemeval-";
    case HU_BENCHMARK_LOCOMO:
        return "locomo-";
    case HU_BENCHMARK_KNOWU:
        return "knowu-";
    case HU_BENCHMARK_EMPA:
        return "empa-";
    case HU_BENCHMARK_PROAGENTBENCH:
        return "proagentbench-";
    }
    return "";
}

hu_error_t hu_benchmark_load(hu_allocator_t *alloc, hu_benchmark_type_t type, const char *json,
                             size_t json_len, hu_eval_suite_t *out) {
    if (!alloc || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_eval_suite_load_json(alloc, json, json_len, out);
    if (err != HU_OK)
        return err;

    const char *prefix = prefix_for_type(type);
    size_t prefix_len = strlen(prefix);
    const char *orig = out->name ? out->name : "";
    size_t orig_len = strlen(orig);
    size_t total = prefix_len + orig_len + 1;
    char *new_name = alloc->alloc(alloc->ctx, total);
    if (!new_name)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(new_name, prefix, prefix_len);
    memcpy(new_name + prefix_len, orig, orig_len + 1);

    if (out->name)
        alloc->free(alloc->ctx, out->name, orig_len + 1);
    out->name = new_name;
    return HU_OK;
}

const char *hu_benchmark_type_name(hu_benchmark_type_t type) {
    switch (type) {
    case HU_BENCHMARK_GAIA:
        return "gaia";
    case HU_BENCHMARK_SWE_BENCH:
        return "swe-bench";
    case HU_BENCHMARK_TOOL_USE:
        return "tool-use";
    case HU_BENCHMARK_LIVE_AGENT:
        return "live-agent";
    case HU_BENCHMARK_APEX:
        return "apex";
    case HU_BENCHMARK_LONGMEMEVAL:
        return "longmemeval";
    case HU_BENCHMARK_LOCOMO:
        return "locomo";
    case HU_BENCHMARK_KNOWU:
        return "knowu";
    case HU_BENCHMARK_EMPA:
        return "empa";
    case HU_BENCHMARK_PROAGENTBENCH:
        return "proagentbench";
    }
    return "unknown";
}
