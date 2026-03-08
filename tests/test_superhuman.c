#include "seaclaw/agent/superhuman.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static sc_error_t mock_build_a(void *ctx, sc_allocator_t *alloc, char **out, size_t *out_len) {
    (void)ctx;
    *out = (char *)alloc->alloc(alloc->ctx, 6);
    if (!*out)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(*out, "ctx_a", 5);
    (*out)[5] = '\0';
    *out_len = 5;
    return SC_OK;
}

static sc_error_t mock_build_b(void *ctx, sc_allocator_t *alloc, char **out, size_t *out_len) {
    (void)ctx;
    *out = (char *)alloc->alloc(alloc->ctx, 6);
    if (!*out)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(*out, "ctx_b", 5);
    (*out)[5] = '\0';
    *out_len = 5;
    return SC_OK;
}

static void superhuman_register_and_count(void) {
    sc_superhuman_registry_t reg;
    SC_ASSERT_EQ(sc_superhuman_registry_init(&reg), SC_OK);
    SC_ASSERT_EQ(reg.count, 0u);

    sc_superhuman_service_t svc_a = {
        .name = "service_a",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    SC_ASSERT_EQ(sc_superhuman_register(&reg, svc_a), SC_OK);
    SC_ASSERT_EQ(reg.count, 1u);

    sc_superhuman_service_t svc_b = {
        .name = "service_b",
        .build_context = mock_build_b,
        .observe = NULL,
        .ctx = NULL,
    };
    SC_ASSERT_EQ(sc_superhuman_register(&reg, svc_b), SC_OK);
    SC_ASSERT_EQ(reg.count, 2u);
}

static void superhuman_build_context_calls_all(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_superhuman_registry_t reg;
    SC_ASSERT_EQ(sc_superhuman_registry_init(&reg), SC_OK);

    sc_superhuman_service_t svc_a = {
        .name = "service_a",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    SC_ASSERT_EQ(sc_superhuman_register(&reg, svc_a), SC_OK);

    sc_superhuman_service_t svc_b = {
        .name = "service_b",
        .build_context = mock_build_b,
        .observe = NULL,
        .ctx = NULL,
    };
    SC_ASSERT_EQ(sc_superhuman_register(&reg, svc_b), SC_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    SC_ASSERT_EQ(sc_superhuman_build_context(&reg, &alloc, &ctx, &ctx_len), SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(ctx_len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "### Superhuman Insights") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "#### service_a") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "ctx_a") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "#### service_b") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "ctx_b") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
}

void run_superhuman_tests(void) {
    SC_TEST_SUITE("superhuman");
    SC_RUN_TEST(superhuman_register_and_count);
    SC_RUN_TEST(superhuman_build_context_calls_all);
}
