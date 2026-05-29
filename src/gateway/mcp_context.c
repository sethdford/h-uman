#include "human/mcp_context.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

void hu_mcp_timeout_budget_init(hu_mcp_timeout_budget_t *budget, uint32_t total_ms,
                                uint32_t per_call_default_ms) {
    if (!budget)
        return;
    memset(budget, 0, sizeof(*budget));
    budget->total_budget_ms = total_ms;
    budget->remaining_ms = total_ms;
    budget->per_call_default_ms = per_call_default_ms;
    budget->per_call_min_ms = 500;
}

uint32_t hu_mcp_timeout_budget_allocate(hu_mcp_timeout_budget_t *budget) {
    if (!budget || budget->remaining_ms == 0)
        return 0;

    uint32_t timeout;
    if (budget->call_count == 0 || budget->avg_latency_ms < 1.0) {
        timeout = budget->per_call_default_ms;
    } else {
        /* Adaptive: 2x average latency, with headroom for variance */
        timeout = (uint32_t)(budget->avg_latency_ms * 2.0);
        if (timeout < budget->per_call_min_ms)
            timeout = budget->per_call_min_ms;

        /* If we've seen timeouts, give more headroom */
        if (budget->timeout_count > 0)
            timeout = (uint32_t)(timeout * 1.5);
    }

    if (timeout > budget->remaining_ms)
        timeout = budget->remaining_ms;

    return timeout;
}

void hu_mcp_timeout_budget_record(hu_mcp_timeout_budget_t *budget, uint32_t elapsed_ms,
                                  bool timed_out) {
    if (!budget)
        return;

    if (elapsed_ms > budget->remaining_ms)
        budget->remaining_ms = 0;
    else
        budget->remaining_ms -= elapsed_ms;

    budget->call_count++;
    if (timed_out)
        budget->timeout_count++;

    if (!timed_out) {
        double n = (double)budget->call_count;
        budget->avg_latency_ms = budget->avg_latency_ms * ((n - 1.0) / n) + (double)elapsed_ms / n;
    }
}

bool hu_mcp_timeout_budget_has_remaining(const hu_mcp_timeout_budget_t *budget) {
    if (!budget)
        return false;
    return budget->remaining_ms >= budget->per_call_min_ms;
}

void hu_mcp_error_classify(const char *raw_error, size_t error_len,
                           hu_mcp_structured_error_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!raw_error || error_len == 0) {
        out->category = HU_MCP_ERR_NONE;
        return;
    }

    if (hu_str_contains_ci_cstr(raw_error, error_len, "timeout") ||
        hu_str_contains_ci_cstr(raw_error, error_len, "timed out") ||
        hu_str_contains_ci_cstr(raw_error, error_len, "deadline")) {
        out->category = HU_MCP_ERR_TIMEOUT;
        out->retryable = true;
        out->retry_after_ms = 1000;
    } else if (hu_str_contains_ci_cstr(raw_error, error_len, "unauthorized") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "forbidden") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "auth")) {
        out->category = HU_MCP_ERR_AUTH;
        out->retryable = false;
    } else if (hu_str_contains_ci_cstr(raw_error, error_len, "not found") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "404")) {
        out->category = HU_MCP_ERR_NOT_FOUND;
        out->retryable = false;
    } else if (hu_str_contains_ci_cstr(raw_error, error_len, "rate limit") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "429") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "too many")) {
        out->category = HU_MCP_ERR_RATE_LIMITED;
        out->retryable = true;
        out->retry_after_ms = 5000;
    } else if (hu_str_contains_ci_cstr(raw_error, error_len, "500") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "internal") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "server error")) {
        out->category = HU_MCP_ERR_SERVER;
        out->retryable = true;
        out->retry_after_ms = 2000;
    } else if (hu_str_contains_ci_cstr(raw_error, error_len, "invalid") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "bad request") ||
               hu_str_contains_ci_cstr(raw_error, error_len, "400")) {
        out->category = HU_MCP_ERR_INVALID_INPUT;
        out->retryable = false;
    } else {
        out->category = HU_MCP_ERR_TRANSIENT;
        out->retryable = true;
        out->retry_after_ms = 1000;
    }

    size_t copy_len = error_len < sizeof(out->message) - 1 ? error_len : sizeof(out->message) - 1;
    memcpy(out->message, raw_error, copy_len);
    out->message[copy_len] = '\0';
}
