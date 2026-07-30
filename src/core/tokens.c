/* src/core/tokens.c — the one place the byte->token ratio lives.
 * Rationale, measurement and the "do not change 4 to 4.5" argument are in
 * include/human/core/tokens.h. */
#include "human/core/tokens.h"

size_t hu_tokens_estimate_len(size_t len) {
    /* Round up: a non-empty run must never estimate to zero tokens, or a budget
     * check can admit unbounded short content and an outcome row can report a
     * real reply as costing nothing. */
    return (len + (HU_TOKENS_BYTES_PER_TOKEN - 1)) / HU_TOKENS_BYTES_PER_TOKEN;
}

size_t hu_tokens_estimate_text(const char *text, size_t len) {
    if (!text)
        return 0;
    return hu_tokens_estimate_len(len);
}
