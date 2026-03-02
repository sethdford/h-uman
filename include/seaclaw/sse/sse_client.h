#ifndef SC_SSE_CLIENT_H
#define SC_SSE_CLIENT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

typedef struct sc_sse_event {
    char *event_type;    /* "message", "error", etc.; default "message" if absent */
    size_t event_type_len;
    char *data;         /* concatenated data fields */
    size_t data_len;
} sc_sse_event_t;

typedef void (*sc_sse_callback_t)(void *ctx, const sc_sse_event_t *event);

sc_error_t sc_sse_connect(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    sc_sse_callback_t callback,
    void *callback_ctx);

#endif
