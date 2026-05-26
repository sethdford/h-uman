/* tests/test_harmony_filter.c — streaming-safe Harmony channel-marker
 * filter contract tests.
 *
 * Pins the byte-level behavior the M3 B4 T4 mlx_local SSE consumer
 * needs: push raw bytes (possibly with mid-marker chunk boundaries),
 * get clean text back; partial markers held back; final flush drains
 * the accumulator through one last strip pass. See
 * include/human/util/harmony_filter.h for the full contract.
 *
 * No network. No HTTP. No SSE. Pure utility tests.
 *
 * Future T4 wiring (compatible_stream_chat → filter → user callback)
 * is gated separately on cfg.mlx_local.streaming_enabled; these tests
 * pin the FILTER ITSELF so the wiring can rely on a known-good
 * primitive. */

#include "human/core/allocator.h"
#include "human/util/harmony_filter.h"
#include "test_framework.h"

#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Helper — push a NUL-terminated string and assert the cleaned
 * output matches. Frees the output. Most tests do this exactly.
 * ────────────────────────────────────────────────────────────────── */
static void push_expect(hu_harmony_filter_t *f, const char *in, const char *expected_out) {
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_push(f, in, strlen(in), &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, expected_out);
    HU_ASSERT_EQ(out_len, strlen(expected_out));
    /* Free with the system allocator (matches what init uses). */
    hu_allocator_t a = hu_system_allocator();
    a.free(a.ctx, out, out_len + 1);
}

static void finish_expect(hu_harmony_filter_t *f, const char *expected_out) {
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_finish(f, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, expected_out);
    HU_ASSERT_EQ(out_len, strlen(expected_out));
    hu_allocator_t a = hu_system_allocator();
    a.free(a.ctx, out, out_len + 1);
}

/* ──────────────────────────────────────────────────────────────────
 * Lifecycle — init returns a usable filter; free is NULL-safe.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_init_returns_usable_filter(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);
    HU_ASSERT_NOT_NULL(f);
    HU_ASSERT_EQ(hu_harmony_filter_buffered_bytes(f), (size_t)0);
    hu_harmony_filter_free(f);
}

static void test_harmony_filter_init_rejects_null_args(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(NULL, &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_harmony_filter_free_handles_null(void) {
    hu_harmony_filter_free(NULL); /* must not crash */
    HU_ASSERT_TRUE(true);
}

/* ──────────────────────────────────────────────────────────────────
 * Clean text passes through unchanged — no `<` in the input, no
 * hold-back, full chunk emitted as-is.
 *
 * Special case: a string ending in a clean trailing `\n` is the
 * common SSE shape; should still emit fully.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_clean_text_passes_through(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    /* Long enough that no hold-back kicks in (no `<` byte). */
    push_expect(f, "hello there, friend — how's it going today?",
                "hello there, friend — how's it going today?");

    /* Empty buffer, nothing to finish. */
    finish_expect(f, "");

    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Well-formed `<|channel|>` marker in the middle of a single chunk
 * — stripped entirely; surrounding text intact.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_strips_closed_marker_in_one_chunk(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    /* Push long-enough text so the marker is well within the safe
     * zone (well clear of the lookahead window). The trailing
     * "padding..." ensures hold-back doesn't trigger on the marker. */
    push_expect(f,
                "before <|channel|> after"
                " — more padding text so the lookahead window clears the marker.",
                "before  after"
                " — more padding text so the lookahead window clears the marker.");
    finish_expect(f, "");

    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Unclosed leak shape `<|channel>thought` — production-observed.
 * Stripped via match_open_marker.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_strips_open_leak_shape(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    push_expect(f, "hi <|channel>thought there — more text to clear the lookahead window for emit.",
                "hi  there — more text to clear the lookahead window for emit.");
    finish_expect(f, "");

    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Marker split across two chunks. First chunk holds back the partial
 * marker; second chunk completes it; both chunks combined emit a
 * clean composite.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_handles_marker_split_across_chunks(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    /* First chunk ends mid-marker. The "<|" plus partial tag is
     * within the lookahead zone — must be held back. The plain text
     * "before " comes before the `<` so it CAN be emitted, but the
     * accumulator-shorter-than-lookahead branch holds everything.
     * Either behavior is acceptable as long as the final composite
     * is correct; we don't pin the intermediate split shape. */
    char *out1 = NULL;
    size_t out1_len = 0;
    HU_ASSERT_EQ(
        hu_harmony_filter_push(f, "before <|chan", strlen("before <|chan"), &out1, &out1_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out1);

    /* Second chunk closes the marker + emits cleartext. The composite
     * across all callbacks must equal the cleaned single-chunk
     * equivalent. */
    char *out2 = NULL;
    size_t out2_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_push(f, "nel|> after — more padding to clear lookahead.",
                                        strlen("nel|> after — more padding to clear lookahead."),
                                        &out2, &out2_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out2);

    /* Drain any remaining tail. */
    char *out3 = NULL;
    size_t out3_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_finish(f, &out3, &out3_len), HU_OK);
    HU_ASSERT_NOT_NULL(out3);

    /* Concatenate all three outputs and assert the composite. */
    size_t total_len = out1_len + out2_len + out3_len;
    char *composite = (char *)a.alloc(a.ctx, total_len + 1);
    HU_ASSERT_NOT_NULL(composite);
    memcpy(composite, out1, out1_len);
    memcpy(composite + out1_len, out2, out2_len);
    memcpy(composite + out1_len + out2_len, out3, out3_len);
    composite[total_len] = '\0';
    HU_ASSERT_STR_EQ(composite, "before  after — more padding to clear lookahead.");

    a.free(a.ctx, out1, out1_len + 1);
    a.free(a.ctx, out2, out2_len + 1);
    a.free(a.ctx, out3, out3_len + 1);
    a.free(a.ctx, composite, total_len + 1);
    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Trailing partial marker held back; finish() drains it through one
 * last strip pass (incomplete marker emits as literal text — exactly
 * what the non-streaming strip_harmony does at EOF).
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_trailing_partial_drains_on_finish(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    /* Short chunk ends mid-marker. Lookahead finds the `<` at
     * position 4, so push emits "say " (4 safe bytes) and holds
     * "<|cha" back. */
    char *out1 = NULL;
    size_t out1_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_push(f, "say <|cha", strlen("say <|cha"), &out1, &out1_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out1);
    HU_ASSERT_STR_EQ(out1, "say ");
    HU_ASSERT_EQ(hu_harmony_filter_buffered_bytes(f), (size_t)5); /* "<|cha" */
    a.free(a.ctx, out1, out1_len + 1);

    /* No more bytes ever arrive. Finish must drain "<|cha" through
     * one last strip pass — since `<|cha` never closes, strip_pass
     * emits the `<` as literal text. */
    char *out2 = NULL;
    size_t out2_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_finish(f, &out2, &out2_len), HU_OK);
    HU_ASSERT_NOT_NULL(out2);
    HU_ASSERT_STR_EQ(out2, "<|cha");
    a.free(a.ctx, out2, out2_len + 1);

    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Multiple markers in sequence — all stripped.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_strips_multiple_markers(void) {
    /* Three markers in a row. The LAST marker may fall within the
     * 64-byte lookahead zone (depending on trailing pad length), so
     * the test composes push + finish outputs to validate the
     * end-to-end stripped result. Same shape as the split-chunk
     * test — proves the streaming-safe contract: all bytes EITHER
     * arrive cleaned in some push, OR drain on finish. */
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    const char *input = "<|channel|>before<|message|>middle<|return|>"
                        " trailing pad text to clear the lookahead boundary.";
    const char *expected = "beforemiddle"
                           " trailing pad text to clear the lookahead boundary.";

    char *out1 = NULL;
    size_t out1_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_push(f, input, strlen(input), &out1, &out1_len), HU_OK);
    HU_ASSERT_NOT_NULL(out1);

    char *out2 = NULL;
    size_t out2_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_finish(f, &out2, &out2_len), HU_OK);
    HU_ASSERT_NOT_NULL(out2);

    size_t total_len = out1_len + out2_len;
    char *composite = (char *)a.alloc(a.ctx, total_len + 1);
    HU_ASSERT_NOT_NULL(composite);
    memcpy(composite, out1, out1_len);
    memcpy(composite + out1_len, out2, out2_len);
    composite[total_len] = '\0';
    HU_ASSERT_STR_EQ(composite, expected);

    a.free(a.ctx, out1, out1_len + 1);
    a.free(a.ctx, out2, out2_len + 1);
    a.free(a.ctx, composite, total_len + 1);
    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Non-marker `<|` followed by non-tag characters — emitted literally.
 * (Defensive: don't strip too aggressively.)
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_preserves_non_marker_pipe(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    /* `<|123` — `<|` then digit, not a valid tag start. The strip
     * pass falls through and emits `<` as literal. The follow-on `|`
     * is also emitted as the loop continues from the next byte. */
    push_expect(f, "before <|123 after — padding to clear lookahead window from the marker.",
                "before <|123 after — padding to clear lookahead window from the marker.");
    finish_expect(f, "");

    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * NULL bytes input + push of zero length — no-op, returns OK.
 * ────────────────────────────────────────────────────────────────── */
static void test_harmony_filter_zero_byte_push_is_noop(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_harmony_filter_push(f, NULL, 0, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, (size_t)0);
    a.free(a.ctx, out, out_len + 1);

    HU_ASSERT_EQ(hu_harmony_filter_push(f, "x", 0, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, (size_t)0);
    a.free(a.ctx, out, out_len + 1);

    hu_harmony_filter_free(f);
}

/* ──────────────────────────────────────────────────────────────────
 * Callback wrapper tests — isolate the wrapper from any provider /
 * SSE / HTTP code. Synthetic recorder callback captures every chunk
 * the wrapper dispatches.
 * ────────────────────────────────────────────────────────────────── */

typedef struct chunk_record {
    hu_stream_chunk_type_t type;
    char delta_copy[256];
    size_t delta_len;
    bool is_final;
    bool seen_tool_name;
} chunk_record_t;

typedef struct recorder_ctx {
    chunk_record_t records[16];
    size_t count;
    bool return_value;
} recorder_ctx_t;

static bool recorder_cb(void *ctx, const hu_stream_chunk_t *chunk) {
    recorder_ctx_t *r = (recorder_ctx_t *)ctx;
    if (!chunk || r->count >= 16)
        return r->return_value;
    chunk_record_t *rec = &r->records[r->count++];
    rec->type = chunk->type;
    rec->delta_len = chunk->delta_len < sizeof(rec->delta_copy) - 1 ? chunk->delta_len
                                                                    : sizeof(rec->delta_copy) - 1;
    if (chunk->delta && rec->delta_len > 0)
        memcpy(rec->delta_copy, chunk->delta, rec->delta_len);
    rec->delta_copy[rec->delta_len] = '\0';
    rec->is_final = chunk->is_final;
    rec->seen_tool_name = (chunk->tool_name != NULL);
    return r->return_value;
}

static void test_harmony_wrap_filters_content_delta_with_marker(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);
    recorder_ctx_t rec = {.return_value = true};
    hu_harmony_callback_wrap_t wrap = {
        .inner = recorder_cb, .inner_ctx = &rec, .filter = f, .alloc = &a};

    const char *delta = "before <|channel|> after — padding text to clear the lookahead window.";
    hu_stream_chunk_t in = {.type = HU_STREAM_CONTENT, .delta = delta, .delta_len = strlen(delta)};
    bool keep = hu_harmony_callback_wrap_fn(&wrap, &in);
    HU_ASSERT_TRUE(keep);
    HU_ASSERT(rec.count >= 1);
    HU_ASSERT_EQ(rec.records[0].type, HU_STREAM_CONTENT);
    HU_ASSERT_NULL(strstr(rec.records[0].delta_copy, "<|channel|>"));
    HU_ASSERT_NOT_NULL(strstr(rec.records[0].delta_copy, "before"));
    HU_ASSERT_NOT_NULL(strstr(rec.records[0].delta_copy, "after"));

    hu_harmony_filter_free(f);
}

static void test_harmony_wrap_passes_through_tool_chunks_unchanged(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);
    recorder_ctx_t rec = {.return_value = true};
    hu_harmony_callback_wrap_t wrap = {
        .inner = recorder_cb, .inner_ctx = &rec, .filter = f, .alloc = &a};

    hu_stream_chunk_t in = {.type = HU_STREAM_TOOL_START,
                            .tool_name = "shell",
                            .tool_name_len = 5,
                            .tool_call_id = "call_001",
                            .tool_call_id_len = 8,
                            .tool_index = 0};
    bool keep = hu_harmony_callback_wrap_fn(&wrap, &in);
    HU_ASSERT_TRUE(keep);
    HU_ASSERT_EQ(rec.count, (size_t)1);
    HU_ASSERT_EQ(rec.records[0].type, HU_STREAM_TOOL_START);
    HU_ASSERT_TRUE(rec.records[0].seen_tool_name);

    hu_harmony_filter_free(f);
}

static void test_harmony_wrap_holds_back_partial_marker_silently(void) {
    /* When the entire chunk is held back, wrapper MUST NOT fire inner. */
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);
    recorder_ctx_t rec = {.return_value = true};
    hu_harmony_callback_wrap_t wrap = {
        .inner = recorder_cb, .inner_ctx = &rec, .filter = f, .alloc = &a};

    const char *delta = "<|cha";
    hu_stream_chunk_t in = {.type = HU_STREAM_CONTENT, .delta = delta, .delta_len = strlen(delta)};
    bool keep = hu_harmony_callback_wrap_fn(&wrap, &in);
    HU_ASSERT_TRUE(keep);
    HU_ASSERT_EQ(rec.count, (size_t)0);
    HU_ASSERT_EQ(hu_harmony_filter_buffered_bytes(f), (size_t)5);

    hu_harmony_filter_free(f);
}

static void test_harmony_wrap_inner_stop_signal_propagates(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_harmony_filter_t *f = NULL;
    HU_ASSERT_EQ(hu_harmony_filter_init(&a, &f), HU_OK);
    recorder_ctx_t rec = {.return_value = false}; /* signal stop */
    hu_harmony_callback_wrap_t wrap = {
        .inner = recorder_cb, .inner_ctx = &rec, .filter = f, .alloc = &a};

    const char *delta = "clean text long enough to push past the lookahead window so it emits.";
    hu_stream_chunk_t in = {.type = HU_STREAM_CONTENT, .delta = delta, .delta_len = strlen(delta)};
    bool keep = hu_harmony_callback_wrap_fn(&wrap, &in);
    HU_ASSERT_FALSE(keep);

    hu_harmony_filter_free(f);
}

void run_harmony_filter_tests(void);
void run_harmony_filter_tests(void) {
    HU_TEST_SUITE("harmony_filter");
    HU_RUN_TEST(test_harmony_filter_init_returns_usable_filter);
    HU_RUN_TEST(test_harmony_filter_init_rejects_null_args);
    HU_RUN_TEST(test_harmony_filter_free_handles_null);
    HU_RUN_TEST(test_harmony_filter_clean_text_passes_through);
    HU_RUN_TEST(test_harmony_filter_strips_closed_marker_in_one_chunk);
    HU_RUN_TEST(test_harmony_filter_strips_open_leak_shape);
    HU_RUN_TEST(test_harmony_filter_handles_marker_split_across_chunks);
    HU_RUN_TEST(test_harmony_filter_trailing_partial_drains_on_finish);
    HU_RUN_TEST(test_harmony_filter_strips_multiple_markers);
    HU_RUN_TEST(test_harmony_filter_preserves_non_marker_pipe);
    HU_RUN_TEST(test_harmony_filter_zero_byte_push_is_noop);
    /* T4-part2: callback wrapper. */
    HU_RUN_TEST(test_harmony_wrap_filters_content_delta_with_marker);
    HU_RUN_TEST(test_harmony_wrap_passes_through_tool_chunks_unchanged);
    HU_RUN_TEST(test_harmony_wrap_holds_back_partial_marker_silently);
    HU_RUN_TEST(test_harmony_wrap_inner_stop_signal_propagates);
}
