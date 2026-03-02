#ifndef SC_ERROR_CLASSIFY_H
#define SC_ERROR_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_api_error_kind {
    SC_API_ERROR_RATE_LIMITED,
    SC_API_ERROR_CONTEXT_EXHAUSTED,
    SC_API_ERROR_VISION_UNSUPPORTED,
    SC_API_ERROR_OTHER,
} sc_api_error_kind_t;

/* Check if error message indicates non-retryable 4xx (except 429/408) */
bool sc_error_is_non_retryable(const char *msg, size_t msg_len);

/* Check if error message indicates context window exhaustion */
bool sc_error_is_context_exhausted(const char *msg, size_t msg_len);

/* Check if error message indicates rate-limit (429) */
bool sc_error_is_rate_limited(const char *msg, size_t msg_len);

/* Parse Retry-After value from error message, returns milliseconds or 0 if not found */
uint64_t sc_error_parse_retry_after_ms(const char *msg, size_t msg_len);

/* Map API error kind to semantic classification for text-based checks */
bool sc_error_is_rate_limited_text(const char *text, size_t text_len);
bool sc_error_is_context_exhausted_text(const char *text, size_t text_len);
bool sc_error_is_vision_unsupported_text(const char *text, size_t text_len);

#endif /* SC_ERROR_CLASSIFY_H */
