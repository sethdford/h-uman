/*
 * tests/test_provider_steering_dispatch.c — SOTA-2026 init-01 S1.
 *
 * The vtable contract: cloud providers (NULL `apply_steering`) get
 * HU_ERR_NOT_SUPPORTED; on-device providers (mocked here) receive the vector
 * by-value and can reject oversize dims at the boundary. The helper enforces
 * bounds before dispatching so we don't have to repeat them in every provider.
 *
 * Determinism: the mock provider is a static struct — no allocations, no I/O.
 */

#include "human/provider.h"
#include "human/persona/steering.h"
#include "human/core/allocator.h"

#include "test_framework.h"

#include <string.h>

/* ── Mock provider state ─────────────────────────────────────────────────── */

typedef struct mock_steering_ctx {
    /* Captures the last `apply_steering` call so the test can assert on it. */
    float captured[HU_STEERING_VEC_DIM];
    size_t captured_dim;
    int call_count;
    int reset_count;
} mock_steering_ctx_t;

static hu_error_t mock_apply_steering(void *ctx, const float *vec, size_t dim) {
    mock_steering_ctx_t *m = (mock_steering_ctx_t *)ctx;
    m->call_count++;
    if (vec == NULL && dim == 0) {
        m->reset_count++;
        for (size_t i = 0; i < HU_STEERING_VEC_DIM; i++)
            m->captured[i] = 0.0f;
        m->captured_dim = 0;
        return HU_OK;
    }
    if (dim > HU_STEERING_VEC_MAX_DIM)
        return HU_ERR_INVALID_ARGUMENT;
    size_t copy = dim < HU_STEERING_VEC_DIM ? dim : HU_STEERING_VEC_DIM;
    for (size_t i = 0; i < copy; i++)
        m->captured[i] = vec[i];
    m->captured_dim = dim;
    return HU_OK;
}

static const hu_provider_vtable_t mock_with_steering = {
    /* required (unused — never called in this dispatch test) */
    .chat_with_system = NULL,
    .chat = NULL,
    .supports_native_tools = NULL,
    .get_name = NULL,
    .deinit = NULL,
    /* the slot under test */
    .apply_steering = mock_apply_steering,
};

static const hu_provider_vtable_t mock_without_steering = {
    .chat_with_system = NULL,
    .chat = NULL,
    .supports_native_tools = NULL,
    .get_name = NULL,
    .deinit = NULL,
    .apply_steering = NULL,
};

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_provider_apply_steering_null_vtable_returns_not_supported(void) {
    /* The cloud-provider shape: vtable present but apply_steering slot NULL. */
    mock_steering_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_provider_t p = {.ctx = &ctx, .vtable = &mock_without_steering};
    float vec[HU_STEERING_VEC_DIM] = {0};
    vec[HU_STEERING_AXIS_WARMTH] = 0.4f;

    HU_ASSERT_EQ(hu_provider_apply_steering(&p, vec, HU_STEERING_VEC_DIM),
                 HU_ERR_NOT_SUPPORTED);
    /* Provider mock was never invoked. */
    HU_ASSERT_EQ((long)ctx.call_count, 0L);
}

static void test_provider_apply_steering_rejects_null_provider(void) {
    float vec[HU_STEERING_VEC_DIM] = {0};
    HU_ASSERT_EQ(hu_provider_apply_steering(NULL, vec, HU_STEERING_VEC_DIM),
                 HU_ERR_INVALID_ARGUMENT);

    hu_provider_t p = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_EQ(hu_provider_apply_steering(&p, vec, HU_STEERING_VEC_DIM),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_provider_apply_steering_rejects_oversize_dim(void) {
    mock_steering_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_provider_t p = {.ctx = &ctx, .vtable = &mock_with_steering};
    float buf[HU_STEERING_VEC_MAX_DIM + 8] = {0};
    HU_ASSERT_EQ(hu_provider_apply_steering(&p, buf, HU_STEERING_VEC_MAX_DIM + 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((long)ctx.call_count, 0L);
}

static void test_provider_apply_steering_rejects_null_vec_nonzero_dim(void) {
    mock_steering_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_provider_t p = {.ctx = &ctx, .vtable = &mock_with_steering};
    HU_ASSERT_EQ(hu_provider_apply_steering(&p, NULL, HU_STEERING_VEC_DIM),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((long)ctx.call_count, 0L);
}

static void test_provider_apply_steering_records_vector(void) {
    mock_steering_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_provider_t p = {.ctx = &ctx, .vtable = &mock_with_steering};
    float vec[HU_STEERING_VEC_DIM] = {0};
    vec[HU_STEERING_AXIS_WARMTH] = 0.7f;
    vec[HU_STEERING_AXIS_HEDGING] = -0.5f;

    HU_ASSERT_EQ(hu_provider_apply_steering(&p, vec, HU_STEERING_VEC_DIM), HU_OK);
    HU_ASSERT_EQ((long)ctx.call_count, 1L);
    HU_ASSERT_EQ((long)ctx.captured_dim, (long)HU_STEERING_VEC_DIM);
    HU_ASSERT_FLOAT_EQ(ctx.captured[HU_STEERING_AXIS_WARMTH], 0.7f, 1e-6);
    HU_ASSERT_FLOAT_EQ(ctx.captured[HU_STEERING_AXIS_HEDGING], -0.5f, 1e-6);
}

static void test_provider_apply_steering_reset_clears_vec(void) {
    mock_steering_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_provider_t p = {.ctx = &ctx, .vtable = &mock_with_steering};
    float vec[HU_STEERING_VEC_DIM] = {0};
    vec[HU_STEERING_AXIS_WARMTH] = 0.9f;

    HU_ASSERT_EQ(hu_provider_apply_steering(&p, vec, HU_STEERING_VEC_DIM), HU_OK);
    HU_ASSERT_TRUE(ctx.captured[HU_STEERING_AXIS_WARMTH] > 0.5f);

    /* Reset form: NULL vec, dim 0. */
    HU_ASSERT_EQ(hu_provider_apply_steering(&p, NULL, 0), HU_OK);
    HU_ASSERT_EQ((long)ctx.reset_count, 1L);
    HU_ASSERT_FLOAT_EQ(ctx.captured[HU_STEERING_AXIS_WARMTH], 0.0f, 1e-6);
    HU_ASSERT_EQ((long)ctx.captured_dim, 0L);
}

static void test_provider_apply_steering_cloud_fallback_to_directive(void) {
    /* End-to-end: a cloud-shaped provider returns NOT_SUPPORTED, the caller
     * renders a prompt-side directive instead. This is the production path. */
    hu_provider_t cloud = {.ctx = NULL, .vtable = &mock_without_steering};
    float vec[HU_STEERING_VEC_DIM] = {0};
    vec[HU_STEERING_AXIS_WARMTH] = 0.5f;
    vec[HU_STEERING_AXIS_FORMALITY] = -0.4f;

    HU_ASSERT_EQ(hu_provider_apply_steering(&cloud, vec, HU_STEERING_VEC_DIM),
                 HU_ERR_NOT_SUPPORTED);

    hu_allocator_t alloc = hu_system_allocator();
    char *directive = NULL;
    size_t directive_len = 0;
    HU_ASSERT_EQ(hu_persona_steering_directive(&alloc, vec, HU_STEERING_VEC_DIM, 1.0f, &directive,
                                               &directive_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(directive);
    HU_ASSERT_STR_CONTAINS(directive, "## Persona steering");

    alloc.free(alloc.ctx, directive, directive_len + 1);
}

void run_provider_steering_dispatch_tests(void) {
    HU_TEST_SUITE("provider_steering_dispatch");
    HU_RUN_TEST(test_provider_apply_steering_null_vtable_returns_not_supported);
    HU_RUN_TEST(test_provider_apply_steering_rejects_null_provider);
    HU_RUN_TEST(test_provider_apply_steering_rejects_oversize_dim);
    HU_RUN_TEST(test_provider_apply_steering_rejects_null_vec_nonzero_dim);
    HU_RUN_TEST(test_provider_apply_steering_records_vector);
    HU_RUN_TEST(test_provider_apply_steering_reset_clears_vec);
    HU_RUN_TEST(test_provider_apply_steering_cloud_fallback_to_directive);
}
