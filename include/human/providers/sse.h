#ifndef HU_PROVIDERS_SSE_H
#define HU_PROVIDERS_SSE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* SSE parser — feed raw bytes, get events via callback.
 * Used by OpenAI/Anthropic streaming providers. */

typedef struct hu_sse_parser {
    char *buffer;
    size_t buf_len;
    size_t buf_cap;
    hu_allocator_t *alloc;
} hu_sse_parser_t;

typedef void (*hu_sse_event_cb)(const char *event_type, size_t event_type_len, const char *data,
                                size_t data_len, void *userdata);

hu_error_t hu_sse_parser_init(hu_sse_parser_t *p, hu_allocator_t *alloc);
void hu_sse_parser_deinit(hu_sse_parser_t *p);

/* Feed raw bytes. Calls callback for each complete event. */
hu_error_t hu_sse_parser_feed(hu_sse_parser_t *p, const char *bytes, size_t len,
                              hu_sse_event_cb callback, void *userdata);

/* ── Line-level parsing (for tests and simple use) ── */
typedef enum hu_sse_line_result_tag {
    HU_SSE_DELTA,
    HU_SSE_DONE,
    HU_SSE_SKIP,
} hu_sse_line_result_tag_t;

typedef struct hu_sse_line_result {
    hu_sse_line_result_tag_t tag;
    char *delta;
    size_t delta_len;
} hu_sse_line_result_t;

hu_error_t hu_sse_parse_line(hu_allocator_t *alloc, const char *line, size_t line_len,
                             hu_sse_line_result_t *out);

char *hu_sse_extract_delta_content(hu_allocator_t *alloc, const char *json_str, size_t json_len);

#endif /* HU_PROVIDERS_SSE_H */
