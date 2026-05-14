#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

static void result_free_on_zeroed_struct_is_safe(void) {
    hu_allocator_t alloc = A();
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    hu_validator_result_free(&alloc, &r);
    HU_ASSERT(r.text == NULL && r.reason == NULL);
}

static void result_free_releases_owned_text_and_reason(void) {
    hu_allocator_t alloc = A();
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    r.decision = HU_VALIDATOR_REWRITE;
    char *text_buf = (char *)alloc.alloc(alloc.ctx, 6);
    HU_ASSERT_NOT_NULL(text_buf);
    memcpy(text_buf, "hello", 6);
    r.text = text_buf;
    r.text_len = 5;
    r.text_owned = true;
    char *reason_buf = (char *)alloc.alloc(alloc.ctx, 3);
    HU_ASSERT_NOT_NULL(reason_buf);
    memcpy(reason_buf, "ok", 3);
    r.reason = reason_buf;
    r.reason_len = 2;
    r.reason_owned = true;
    hu_validator_result_free(&alloc, &r);
    /* No leak under ASan == pass; pointers cleared. */
    HU_ASSERT(r.text == NULL);
    HU_ASSERT(r.reason == NULL);
}

static void deinit_on_zeroed_struct_is_safe(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    memset(&v, 0, sizeof(v));
    hu_output_validator_deinit(&v, &alloc);
    HU_ASSERT(v.ctx == NULL && v.vtable == NULL);
}

static int deinit_call_count = 0;
static void deinit_test_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
    deinit_call_count++;
}
static const char *deinit_test_name(void *ctx) {
    (void)ctx;
    return "test";
}
static hu_error_t deinit_test_validate(void *ctx, hu_allocator_t *alloc,
                                       const hu_validator_context_t *vctx, const char *r, size_t rl,
                                       hu_validator_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)vctx;
    (void)r;
    (void)rl;
    memset(out, 0, sizeof(*out));
    out->decision = HU_VALIDATOR_PASS;
    return HU_OK;
}
static const hu_output_validator_vtable_t deinit_test_vtable = {
    .validate = deinit_test_validate,
    .name = deinit_test_name,
    .deinit = deinit_test_deinit,
};

static void deinit_calls_vtable_deinit_when_present(void) {
    hu_allocator_t alloc = A();
    deinit_call_count = 0;
    hu_output_validator_t v = {.ctx = NULL, .vtable = &deinit_test_vtable};
    hu_output_validator_deinit(&v, &alloc);
    HU_ASSERT_EQ(deinit_call_count, 1);
    HU_ASSERT(v.ctx == NULL && v.vtable == NULL);
}

void run_output_validator_tests(void) {
    HU_TEST_SUITE("output_validator");
    HU_RUN_TEST(result_free_on_zeroed_struct_is_safe);
    HU_RUN_TEST(result_free_releases_owned_text_and_reason);
    HU_RUN_TEST(deinit_on_zeroed_struct_is_safe);
    HU_RUN_TEST(deinit_calls_vtable_deinit_when_present);
}
