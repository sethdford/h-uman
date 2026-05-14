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

/* Pass-pass-pass chain returns final text unchanged, no allocation transferred. */
static void chain_execute_all_pass_returns_input_unchanged(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    visit_ctx_t v1 = {0}, v2 = {0};
    hu_output_validator_t ov1 = {.ctx = &v1, .vtable = &visit_vtable};
    hu_output_validator_t ov2 = {.ctx = &v2, .vtable = &visit_vtable};
    hu_output_validator_chain_add(chain, ov1);
    hu_output_validator_chain_add(chain, ov2);
    const char *in = "hello world";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, in, 11, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_PASS);
    HU_ASSERT(cr.final_text == in);
    HU_ASSERT_EQ(cr.final_text_len, 11u);
    HU_ASSERT(!cr.final_text_owned);
    HU_ASSERT_EQ(v1.visited, 1);
    HU_ASSERT_EQ(v2.visited, 1);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Synthetic uppercase-rewriter: turns 'h' -> 'H' etc. */
typedef struct {
    int call_count;
} upper_ctx_t;
static hu_error_t upper_validate(void *vctx, hu_allocator_t *alloc, const hu_validator_context_t *c,
                                 const char *r, size_t rl, hu_validator_result_t *out) {
    (void)c;
    upper_ctx_t *ctx = (upper_ctx_t *)vctx;
    ctx->call_count++;
    char *buf = (char *)alloc->alloc(alloc->ctx, rl + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < rl; i++) {
        char ch = r[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    buf[rl] = '\0';
    memset(out, 0, sizeof(*out));
    out->decision = HU_VALIDATOR_REWRITE;
    out->text = buf;
    out->text_len = rl;
    out->text_owned = true;
    return HU_OK;
}
static const char *upper_name(void *vctx) {
    (void)vctx;
    return "upper";
}
static const hu_output_validator_vtable_t upper_vtable = {
    .validate = upper_validate,
    .name = upper_name,
    .deinit = NULL,
};

static void chain_rewrite_then_pass_returns_rewritten(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    upper_ctx_t u = {0};
    visit_ctx_t v = {0};
    hu_output_validator_chain_add(chain,
                                  (hu_output_validator_t){.ctx = &u, .vtable = &upper_vtable});
    hu_output_validator_chain_add(chain,
                                  (hu_output_validator_t){.ctx = &v, .vtable = &visit_vtable});
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, "hi", 2, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT(cr.final_text_owned);
    HU_ASSERT_EQ(cr.final_text_len, 2u);
    HU_ASSERT(strncmp(cr.final_text, "HI", 2) == 0);
    HU_ASSERT_EQ(cr.rewrite_count, 1u);
    HU_ASSERT_EQ(v.visited, 1);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Reject validator. */
typedef struct {
    const char *msg;
} reject_ctx_t;
static hu_error_t reject_validate(void *vctx, hu_allocator_t *alloc,
                                  const hu_validator_context_t *c, const char *r, size_t rl,
                                  hu_validator_result_t *out) {
    (void)c;
    (void)r;
    (void)rl;
    reject_ctx_t *ctx = (reject_ctx_t *)vctx;
    size_t mlen = strlen(ctx->msg);
    char *reason = (char *)alloc->alloc(alloc->ctx, mlen + 1);
    if (!reason)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(reason, ctx->msg, mlen + 1);
    memset(out, 0, sizeof(*out));
    out->decision = HU_VALIDATOR_REJECT;
    out->reason = reason;
    out->reason_len = mlen;
    out->reason_owned = true;
    return HU_OK;
}
static const char *reject_name(void *vctx) {
    (void)vctx;
    return "reject";
}
static const hu_output_validator_vtable_t reject_vtable = {
    .validate = reject_validate,
    .name = reject_name,
    .deinit = NULL,
};

static void chain_reject_short_circuits(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    reject_ctx_t rc = {.msg = "nope"};
    visit_ctx_t v = {0};
    hu_output_validator_chain_add(chain,
                                  (hu_output_validator_t){.ctx = &rc, .vtable = &reject_vtable});
    hu_output_validator_chain_add(chain,
                                  (hu_output_validator_t){.ctx = &v, .vtable = &visit_vtable});
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, "x", 1, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);
    HU_ASSERT(cr.final_text == NULL);
    HU_ASSERT_EQ(cr.reject_count, 1u);
    HU_ASSERT(cr.reject_reason && strncmp(cr.reject_reason, "nope", 4) == 0);
    HU_ASSERT(cr.reject_reason_owned);
    HU_ASSERT_EQ(v.visited, 0); /* never reached */
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* REWRITE followed by REJECT: intermediate rewrite buffer must be freed. */
static void chain_rewrite_then_reject_frees_intermediate_buffer(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    upper_ctx_t u = {0};
    reject_ctx_t rc = {.msg = "after rewrite"};
    hu_output_validator_chain_add(chain,
                                  (hu_output_validator_t){.ctx = &u, .vtable = &upper_vtable});
    hu_output_validator_chain_add(chain,
                                  (hu_output_validator_t){.ctx = &rc, .vtable = &reject_vtable});
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, "hi", 2, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);
    HU_ASSERT(cr.final_text == NULL);
    /* The intermediate "HI" rewrite buffer must have been freed by the chain
     * before reject; ASan catches a leak here. */
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Execute on an empty chain returns PASS with input unchanged. */
static void chain_execute_empty_returns_pass(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    const char *in = "anything";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, in, 8, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_PASS);
    HU_ASSERT(cr.final_text == in);
    HU_ASSERT(!cr.final_text_owned);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Execute with NULL args returns HU_ERR_INVALID_ARGUMENT. */
static void chain_execute_rejects_null_args(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(NULL, &alloc, NULL, "x", 1, &cr),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, NULL, NULL, "x", 1, &cr),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, NULL, 1, &cr),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, "x", 1, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_output_validator_chain_destroy(chain);
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
    HU_RUN_TEST(chain_execute_all_pass_returns_input_unchanged);
    HU_RUN_TEST(chain_rewrite_then_pass_returns_rewritten);
    HU_RUN_TEST(chain_reject_short_circuits);
    HU_RUN_TEST(chain_rewrite_then_reject_frees_intermediate_buffer);
    HU_RUN_TEST(chain_execute_empty_returns_pass);
    HU_RUN_TEST(chain_execute_rejects_null_args);
}
