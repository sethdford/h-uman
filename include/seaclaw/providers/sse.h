#ifndef SC_PROVIDERS_SSE_H
#define SC_PROVIDERS_SSE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* SSE parser — feed raw bytes, get events via callback.
 * Used by OpenAI/Anthropic streaming providers. */

typedef struct sc_sse_parser {
    char *buffer;
    size_t buf_len;
    size_t buf_cap;
    sc_allocator_t *alloc;
} sc_sse_parser_t;

typedef void (*sc_sse_event_cb)(const char *event_type, size_t event_type_len,
    const char *data, size_t data_len, void *userdata);

sc_error_t sc_sse_parser_init(sc_sse_parser_t *p, sc_allocator_t *alloc);
void sc_sse_parser_deinit(sc_sse_parser_t *p);

/* Feed raw bytes. Calls callback for each complete event. */
sc_error_t sc_sse_parser_feed(sc_sse_parser_t *p,
    const char *bytes, size_t len,
    sc_sse_event_cb callback, void *userdata);

/* ── Line-level parsing (for tests and simple use) ── */
typedef enum sc_sse_line_result_tag {
    SC_SSE_DELTA,
    SC_SSE_DONE,
    SC_SSE_SKIP,
} sc_sse_line_result_tag_t;

typedef struct sc_sse_line_result {
    sc_sse_line_result_tag_t tag;
    char *delta;
    size_t delta_len;
} sc_sse_line_result_t;

sc_error_t sc_sse_parse_line(sc_allocator_t *alloc, const char *line, size_t line_len,
    sc_sse_line_result_t *out);

char *sc_sse_extract_delta_content(sc_allocator_t *alloc, const char *json_str, size_t json_len);

#endif /* SC_PROVIDERS_SSE_H */
