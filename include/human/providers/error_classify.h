#ifndef HU_ERROR_CLASSIFY_H
#define HU_ERROR_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_api_error_kind {
    HU_API_ERROR_RATE_LIMITED,
    HU_API_ERROR_CONTEXT_EXHAUSTED,
    HU_API_ERROR_VISION_UNSUPPORTED,
    HU_API_ERROR_OTHER,
} hu_api_error_kind_t;

/* Check if error message indicates non-retryable 4xx (except 429/408) */
bool hu_error_is_non_retryable(const char *msg, size_t msg_len);

/* Check if error message indicates context window exhaustion */
bool hu_error_is_context_exhausted(const char *msg, size_t msg_len);

/* Check if error message indicates rate-limit (429) */
bool hu_error_is_rate_limited(const char *msg, size_t msg_len);

/* Parse Retry-After value from error message, returns milliseconds or 0 if not found */
uint64_t hu_error_parse_retry_after_ms(const char *msg, size_t msg_len);

/* Map API error kind to semantic classification for text-based checks */
bool hu_error_is_rate_limited_text(const char *text, size_t text_len);
bool hu_error_is_context_exhausted_text(const char *text, size_t text_len);
bool hu_error_is_vision_unsupported_text(const char *text, size_t text_len);

#endif /* HU_ERROR_CLASSIFY_H */
