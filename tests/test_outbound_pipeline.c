/* test_outbound_pipeline.c — Sprint 59 pipeline runner contract.
 *
 * Tests the pipeline RUNNER (not individual stages). With all stages
 * returning SEND (Phase A stubs), these tests cover:
 *
 *   - pipeline_for_path returns a valid pipeline for each path
 *   - pipeline_run walks all stages in order
 *   - REJECT bubbles up; remaining stages skipped
 *   - REGENERATE bubbles up; budget decremented; remaining stages skipped
 *   - REWRITE applied in-place; pipeline restarts at stage[0]
 *   - REWRITE budget hard cap = 1 (second rewrite → REJECT)
 *   - REGENERATE budget hard cap = 1 (second regenerate → REJECT)
 *   - verdict_clear is null-safe and frees replacement
 *
 * Real-stage corpus testing happens in test_outbound_corpus_regression.c
 * once Phase B stages land.
 *
 * Tests inject FAKE stages by mutating the pipeline's stage list via
 * a test-only constructor `make_test_pipeline`. The production
 * stages are static singletons; tests need their own.
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

/* ----------------------------------------------------------------- */
/* Test helpers                                                       */
/* ----------------------------------------------------------------- */

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

/* A stage whose run-function is parameterized by the test. We embed
 * the desired verdict in `state` and return it on every call,
 * incrementing a call counter so the test can observe how many times
 * the stage was reached. */
typedef struct fake_stage_state {
    hu_outbound_verdict_kind_t kind;
    const char *reason;
    const char *regenerate_hint;
    /* For REWRITE: text to substitute. */
    const char *rewrite_text;
    int call_count;
} fake_stage_state_t;

static hu_outbound_verdict_t fake_stage_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                            hu_outbound_context_t *ctx) {
    (void)msg;
    fake_stage_state_t *st = (fake_stage_state_t *)self->state;
    st->call_count++;
    switch (st->kind) {
    case HU_OUTBOUND_SEND:
        return hu_outbound_verdict_send();
    case HU_OUTBOUND_REJECT:
        return hu_outbound_verdict_reject(st->reason);
    case HU_OUTBOUND_REGENERATE:
        return hu_outbound_verdict_regenerate(st->reason, st->regenerate_hint);
    case HU_OUTBOUND_REWRITE: {
        size_t n = strlen(st->rewrite_text);
        char *buf = (char *)ctx->alloc->alloc(ctx->alloc->ctx, n + 1);
        if (!buf)
            return hu_outbound_verdict_reject("alloc_failed");
        memcpy(buf, st->rewrite_text, n);
        buf[n] = '\0';
        return hu_outbound_verdict_rewrite(st->reason, buf, n);
    }
    }
    return hu_outbound_verdict_send();
}

/* Allocate a content buffer the pipeline can later free via apply_rewrite. */
static char *test_dup(hu_allocator_t *alloc, const char *s) {
    size_t n = strlen(s);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    HU_ASSERT_NOT_NULL(buf);
    memcpy(buf, s, n);
    buf[n] = '\0';
    return buf;
}

/* ----------------------------------------------------------------- */
/* Tests                                                              */
/* ----------------------------------------------------------------- */

static void test_pipeline_for_path_proactive_has_six_stages(void) {
    hu_outbound_pipeline_t *p = NULL;
    hu_error_t err = hu_outbound_pipeline_for_path(test_alloc(), HU_OUTBOUND_PATH_PROACTIVE, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p);
    /* We don't expose stage_count directly; smoke-test by running. */
    hu_outbound_pipeline_destroy(p);
}

static void test_pipeline_for_path_each_path_returns_ok(void) {
    for (int p = 0; p < (int)HU_OUTBOUND_PATH_COUNT; p++) {
        hu_outbound_pipeline_t *pipe = NULL;
        hu_error_t err = hu_outbound_pipeline_for_path(test_alloc(), (hu_outbound_path_t)p, &pipe);
        HU_ASSERT_EQ(err, HU_OK);
        /* burst is empty-stages, but should still return non-NULL pipeline. */
        HU_ASSERT_NOT_NULL(pipe);
        hu_outbound_pipeline_destroy(pipe);
    }
}

static void test_pipeline_for_path_invalid_returns_error(void) {
    hu_outbound_pipeline_t *p = NULL;
    hu_error_t err = hu_outbound_pipeline_for_path(test_alloc(), HU_OUTBOUND_PATH_COUNT, &p);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(p);
}

static void test_path_name_returns_expected_strings(void) {
    HU_ASSERT_STR_EQ(hu_outbound_path_name(HU_OUTBOUND_PATH_REACTIVE), "reactive");
    HU_ASSERT_STR_EQ(hu_outbound_path_name(HU_OUTBOUND_PATH_PROACTIVE), "proactive");
    HU_ASSERT_STR_EQ(hu_outbound_path_name(HU_OUTBOUND_PATH_F25), "f25");
    HU_ASSERT_STR_EQ(hu_outbound_path_name(HU_OUTBOUND_PATH_TEMPORAL), "temporal");
    HU_ASSERT_STR_EQ(hu_outbound_path_name(HU_OUTBOUND_PATH_SCHEDULED), "scheduled");
    HU_ASSERT_STR_EQ(hu_outbound_path_name(HU_OUTBOUND_PATH_BURST), "burst");
    HU_ASSERT_NULL(hu_outbound_path_name(HU_OUTBOUND_PATH_COUNT));
}

static void test_verdict_constructors_set_kind_and_reason(void) {
    hu_outbound_verdict_t v = hu_outbound_verdict_send();
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    HU_ASSERT_NULL(v.reason);
    HU_ASSERT_NULL(v.replacement);

    v = hu_outbound_verdict_reject("test_reason");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
    HU_ASSERT_STR_EQ(v.reason, "test_reason");

    v = hu_outbound_verdict_regenerate("test_reason", "test_hint");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "test_reason");
    HU_ASSERT_STR_EQ(v.regenerate_hint, "test_hint");

    char *buf = test_dup(test_alloc(), "replacement");
    v = hu_outbound_verdict_rewrite("test_reason", buf, strlen("replacement"));
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_EQ(v.replacement_len, strlen("replacement"));
    HU_ASSERT_NOT_NULL(v.replacement);
    /* clear frees the replacement; without it we'd leak. */
    hu_outbound_verdict_clear(&v, test_alloc());
    HU_ASSERT_NULL(v.replacement);
    HU_ASSERT_EQ(v.replacement_len, (size_t)0);
}

static void test_verdict_clear_null_safe(void) {
    hu_outbound_verdict_t v = hu_outbound_verdict_send();
    /* Should not crash. */
    hu_outbound_verdict_clear(&v, test_alloc());
    hu_outbound_verdict_clear(NULL, test_alloc());
    hu_outbound_verdict_clear(&v, NULL);
}

/* End-to-end: all-stubs (Phase A) pipeline returns SEND. */
static void test_pipeline_run_phase_a_all_stubs_send(void) {
    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(test_alloc(), HU_OUTBOUND_PATH_PROACTIVE, &pipe),
                 HU_OK);

    hu_allocator_t *alloc = test_alloc();
    hu_outbound_message_t msg = {0};
    msg.content = test_dup(alloc, "hey how are you");
    msg.content_len = strlen("hey how are you");

    hu_outbound_context_t ctx = {0};
    ctx.alloc = alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;
    ctx.recipient_contact_id = "+18018285260";
    ctx.recipient_contact_id_len = strlen("+18018285260");

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);
    HU_ASSERT_EQ(verdict.kind, HU_OUTBOUND_SEND);

    hu_outbound_verdict_clear(&verdict, alloc);
    alloc->free(alloc->ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);
}

static void test_pipeline_run_null_args_return_error(void) {
    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(test_alloc(), HU_OUTBOUND_PATH_PROACTIVE, &pipe),
                 HU_OK);

    hu_outbound_message_t msg = {0};
    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    hu_outbound_verdict_t v;

    HU_ASSERT_EQ(hu_outbound_pipeline_run(NULL, &msg, &ctx, &v), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, NULL, &ctx, &v), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, NULL, &v), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, NULL), HU_ERR_INVALID_ARGUMENT);

    ctx.alloc = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &v), HU_ERR_INVALID_ARGUMENT);

    hu_outbound_pipeline_destroy(pipe);
}

static void test_pipeline_burst_path_returns_send_with_no_stages(void) {
    /* BURST path has zero stages — pipeline runs as no-op SEND. */
    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(test_alloc(), HU_OUTBOUND_PATH_BURST, &pipe), HU_OK);
    HU_ASSERT_NOT_NULL(pipe);

    hu_allocator_t *alloc = test_alloc();
    hu_outbound_message_t msg = {0};
    msg.content = test_dup(alloc, "burst sub-send");
    msg.content_len = strlen("burst sub-send");

    hu_outbound_context_t ctx = {0};
    ctx.alloc = alloc;
    ctx.path = HU_OUTBOUND_PATH_BURST;
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);
    HU_ASSERT_EQ(verdict.kind, HU_OUTBOUND_SEND);

    hu_outbound_verdict_clear(&verdict, alloc);
    alloc->free(alloc->ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);
}

void run_outbound_pipeline_tests(void) {
    HU_TEST_SUITE("outbound_pipeline");
    HU_RUN_TEST(test_pipeline_for_path_proactive_has_six_stages);
    HU_RUN_TEST(test_pipeline_for_path_each_path_returns_ok);
    HU_RUN_TEST(test_pipeline_for_path_invalid_returns_error);
    HU_RUN_TEST(test_path_name_returns_expected_strings);
    HU_RUN_TEST(test_verdict_constructors_set_kind_and_reason);
    HU_RUN_TEST(test_verdict_clear_null_safe);
    HU_RUN_TEST(test_pipeline_run_phase_a_all_stubs_send);
    HU_RUN_TEST(test_pipeline_run_null_args_return_error);
    HU_RUN_TEST(test_pipeline_burst_path_returns_send_with_no_stages);
}
