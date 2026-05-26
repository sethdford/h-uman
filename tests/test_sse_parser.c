/* tests/test_sse_parser.c — pure unit tests for hu_sse_parser_t.
 *
 * No network. No HTTP. No MLX. The parser is byte-in, event-out; these
 * tests push pre-canned SSE-format byte streams and assert the pop
 * order + payloads match the WHATWG-subset contract documented in
 * include/human/util/sse_parser.h.
 *
 * First brick of M3 Bridge B Phase B4 (see
 * docs/plans/2026-05-26-m3-b4-mlx-local-sse/tasks.md::T1). The mlx_local
 * SSE consumer will sit on top of this parser; pinning the byte/event
 * contract here lets future tests of that consumer focus on HTTP +
 * dispatch behavior, not parsing edge cases. */

#include "human/core/allocator.h"
#include "human/util/sse_parser.h"
#include "test_framework.h"

#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Helper — push a NUL-terminated string then pop one event. Many
 * tests do exactly this; keeping it terse avoids 6-line boilerplate
 * in every case.
 * ────────────────────────────────────────────────────────────────── */
static hu_error_t push_and_pop(hu_sse_parser_t *p, const char *stream, char **out,
                               size_t *out_len) {
    hu_error_t e = hu_sse_parser_push(p, stream, strlen(stream));
    if (e != HU_OK)
        return e;
    return hu_sse_parser_pop_event(p, out, out_len);
}

/* ──────────────────────────────────────────────────────────────────
 * Lifecycle — init returns a usable parser; free is NULL-safe.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_init_returns_usable_parser(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_EQ(hu_sse_parser_buffered_bytes(p), (size_t)0);
    hu_sse_parser_free(p);
}

static void test_sse_parser_init_rejects_null_args(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(NULL, &p), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_sse_parser_init(&a, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_sse_parser_free_handles_null(void) {
    /* Must not crash. */
    hu_sse_parser_free(NULL);
    HU_ASSERT_TRUE(true);
}

/* ──────────────────────────────────────────────────────────────────
 * Core happy path — single complete event yields its data payload.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_emits_single_event_per_data_line(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(push_and_pop(p, "data: hello\n\n", &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hello");
    HU_ASSERT_EQ(out_len, (size_t)5);
    a.free(a.ctx, out, out_len + 1);

    /* After popping the only event, the buffer should be empty. */
    HU_ASSERT_EQ(hu_sse_parser_buffered_bytes(p), (size_t)0);

    /* And another pop with no buffered data returns NOT_FOUND. */
    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_ERR_NOT_FOUND);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Multi-line data: concatenation per SSE spec — multiple `data:`
 * lines within ONE event become one payload joined by '\n'.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_handles_multi_data_line_event(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(push_and_pop(p, "data: line1\ndata: line2\ndata: line3\n\n", &out, &out_len),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "line1\nline2\nline3");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Comment lines (starting with ':') must be silently ignored.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_ignores_comment_lines(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    /* Two comment lines around the real data line; output is just "real". */
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(
        push_and_pop(p, ": this is a heartbeat\ndata: real\n: another comment\n\n", &out, &out_len),
        HU_OK);
    HU_ASSERT_STR_EQ(out, "real");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Comment-only events get silently skipped — the next pop returns
 * NOT_FOUND, but the comment-only event's bytes are consumed.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_skips_comment_only_event(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    /* Push a heartbeat-only event followed by a real one. Pop should
     * skip the heartbeat and return the real payload directly. */
    HU_ASSERT_EQ(hu_sse_parser_push(p, ": heartbeat\n\ndata: real\n\n", 26), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "real");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Partial event at buffer edge — push half, get NOT_FOUND, push the
 * rest, get the event. Proves the accumulator survives multi-call
 * arrival (the exact thing libcurl write-callbacks do).
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_drops_partial_event_at_buffer_edge(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    /* Push the first half of "data: hello\n\n" — no terminator yet. */
    HU_ASSERT_EQ(hu_sse_parser_push(p, "data: hel", 9), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(hu_sse_parser_buffered_bytes(p), (size_t)9);

    /* Push the rest — now the terminator arrives. */
    HU_ASSERT_EQ(hu_sse_parser_push(p, "lo\n\n", 4), HU_OK);
    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "hello");
    a.free(a.ctx, out, out_len + 1);
    HU_ASSERT_EQ(hu_sse_parser_buffered_bytes(p), (size_t)0);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Two complete events back-to-back — both pop in order.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_pops_multiple_events_in_order(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    HU_ASSERT_EQ(hu_sse_parser_push(p, "data: first\n\ndata: second\n\ndata: third\n\n", 41),
                 HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "first");
    a.free(a.ctx, out, out_len + 1);

    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "second");
    a.free(a.ctx, out, out_len + 1);

    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "third");
    a.free(a.ctx, out, out_len + 1);

    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_ERR_NOT_FOUND);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * `[DONE]` sentinel — common in OpenAI-streaming-compatible servers.
 * Returned as a normal event with payload exactly "[DONE]". Caller
 * decides end-of-stream semantics; the parser stays dumb.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_returns_done_sentinel_as_data(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(push_and_pop(p, "data: [DONE]\n\n", &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "[DONE]");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * CRLF terminators must work too — many servers emit "\r\n\r\n"
 * instead of "\n\n" depending on platform.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_accepts_crlf_terminators(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(push_and_pop(p, "data: crlf-ok\r\n\r\n", &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "crlf-ok");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Non-`data:` fields (event:, id:, retry:) must be ignored — an
 * event with ONLY those fields gets silently skipped.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_skips_event_with_only_metadata_fields(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    /* Metadata-only event, then a real one. Real one should pop. */
    HU_ASSERT_EQ(hu_sse_parser_push(p, "event: ping\nid: 42\nretry: 1000\n\ndata: hi\n\n", 42),
                 HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_sse_parser_pop_event(p, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "hi");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

/* ──────────────────────────────────────────────────────────────────
 * Optional leading space after "data:" must be stripped exactly once.
 * A second leading space (i.e. "data:  hello") is preserved.
 * ────────────────────────────────────────────────────────────────── */
static void test_sse_parser_strips_one_leading_space_after_data_colon(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_sse_parser_t *p = NULL;
    HU_ASSERT_EQ(hu_sse_parser_init(&a, &p), HU_OK);

    /* Single space — stripped. */
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(push_and_pop(p, "data: a\n\n", &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "a");
    a.free(a.ctx, out, out_len + 1);

    /* Two spaces — first stripped, second preserved. */
    HU_ASSERT_EQ(push_and_pop(p, "data:  b\n\n", &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, " b");
    a.free(a.ctx, out, out_len + 1);

    /* No space — preserved entirely. */
    HU_ASSERT_EQ(push_and_pop(p, "data:c\n\n", &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "c");
    a.free(a.ctx, out, out_len + 1);

    hu_sse_parser_free(p);
}

void run_sse_parser_tests(void);
void run_sse_parser_tests(void) {
    HU_TEST_SUITE("sse_parser");
    HU_RUN_TEST(test_sse_parser_init_returns_usable_parser);
    HU_RUN_TEST(test_sse_parser_init_rejects_null_args);
    HU_RUN_TEST(test_sse_parser_free_handles_null);
    HU_RUN_TEST(test_sse_parser_emits_single_event_per_data_line);
    HU_RUN_TEST(test_sse_parser_handles_multi_data_line_event);
    HU_RUN_TEST(test_sse_parser_ignores_comment_lines);
    HU_RUN_TEST(test_sse_parser_skips_comment_only_event);
    HU_RUN_TEST(test_sse_parser_drops_partial_event_at_buffer_edge);
    HU_RUN_TEST(test_sse_parser_pops_multiple_events_in_order);
    HU_RUN_TEST(test_sse_parser_returns_done_sentinel_as_data);
    HU_RUN_TEST(test_sse_parser_accepts_crlf_terminators);
    HU_RUN_TEST(test_sse_parser_skips_event_with_only_metadata_fields);
    HU_RUN_TEST(test_sse_parser_strips_one_leading_space_after_data_colon);
}
