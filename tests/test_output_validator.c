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

#include "human/agent/output_validator_chain.h"

/* Synthetic validator used across chain tests: marks ctx as visited. */
typedef struct {
    int visited;
} visit_ctx_t;
static hu_error_t visit_validate(void *vctx, hu_allocator_t *alloc, const hu_validator_context_t *c,
                                 const char *r, size_t rl, hu_validator_result_t *out) {
    (void)alloc;
    (void)c;
    (void)r;
    (void)rl;
    visit_ctx_t *ctx = (visit_ctx_t *)vctx;
    ctx->visited++;
    memset(out, 0, sizeof(*out));
    out->decision = HU_VALIDATOR_PASS;
    return HU_OK;
}
static const char *visit_name(void *vctx) {
    (void)vctx;
    return "visit";
}
static const hu_output_validator_vtable_t visit_vtable = {
    .validate = visit_validate,
    .name = visit_name,
    .deinit = NULL,
};

static void chain_create_and_destroy_empty(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    HU_ASSERT_NOT_NULL(chain);
    HU_ASSERT_EQ(hu_output_validator_chain_len(chain), 0u);
    hu_output_validator_chain_destroy(chain);
}

static void chain_add_then_len_increments(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    visit_ctx_t v1 = {0}, v2 = {0};
    hu_output_validator_t ov1 = {.ctx = &v1, .vtable = &visit_vtable};
    hu_output_validator_t ov2 = {.ctx = &v2, .vtable = &visit_vtable};
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, ov1), HU_OK);
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, ov2), HU_OK);
    HU_ASSERT_EQ(hu_output_validator_chain_len(chain), 2u);
    hu_output_validator_chain_destroy(chain);
}

static void chain_create_rejects_null_args(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(NULL, &chain), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void chain_add_rejects_invalid_validator(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    hu_output_validator_t bogus = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, bogus), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_output_validator_chain_len(chain), 0u);
    hu_output_validator_chain_destroy(chain);
}

static void chain_destroy_on_null_is_safe(void) {
    hu_output_validator_chain_destroy(NULL);
    /* Did not crash == pass. */
}

void run_output_validator_tests(void) {
    HU_TEST_SUITE("output_validator");
    HU_RUN_TEST(result_free_on_zeroed_struct_is_safe);
    HU_RUN_TEST(result_free_releases_owned_text_and_reason);
    HU_RUN_TEST(deinit_on_zeroed_struct_is_safe);
    HU_RUN_TEST(deinit_calls_vtable_deinit_when_present);
    HU_RUN_TEST(chain_create_and_destroy_empty);
    HU_RUN_TEST(chain_add_then_len_increments);
    HU_RUN_TEST(chain_create_rejects_null_args);
    HU_RUN_TEST(chain_add_rejects_invalid_validator);
    HU_RUN_TEST(chain_destroy_on_null_is_safe);
}
