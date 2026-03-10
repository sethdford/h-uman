#ifndef HU_SSE_CLIENT_H
#define HU_SSE_CLIENT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_sse_event {
    char *event_type; /* "message", "error", etc.; default "message" if absent */
    size_t event_type_len;
    char *data; /* concatenated data fields */
    size_t data_len;
} hu_sse_event_t;

typedef void (*hu_sse_callback_t)(void *ctx, const hu_sse_event_t *event);

hu_error_t hu_sse_connect(hu_allocator_t *alloc, const char *url, const char *auth_header,
                          const char *extra_headers, hu_sse_callback_t callback,
                          void *callback_ctx);

#endif
