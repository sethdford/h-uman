#include "human/agent/superhuman.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>
#endif

static hu_error_t mock_build_a(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    (void)ctx;
    *out = (char *)alloc->alloc(alloc->ctx, 6);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, "ctx_a", 5);
    (*out)[5] = '\0';
    *out_len = 5;
    return HU_OK;
}

static hu_error_t mock_build_b(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    (void)ctx;
    *out = (char *)alloc->alloc(alloc->ctx, 6);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, "ctx_b", 5);
    (*out)[5] = '\0';
    *out_len = 5;
    return HU_OK;
}

static void superhuman_register_and_count(void) {
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);
    HU_ASSERT_EQ(reg.count, 0u);

    hu_superhuman_service_t svc_a = {
        .name = "service_a",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_a), HU_OK);
    HU_ASSERT_EQ(reg.count, 1u);

    hu_superhuman_service_t svc_b = {
        .name = "service_b",
        .build_context = mock_build_b,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_b), HU_OK);
    HU_ASSERT_EQ(reg.count, 2u);
}

static void superhuman_build_context_calls_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    hu_superhuman_service_t svc_a = {
        .name = "service_a",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_a), HU_OK);

    hu_superhuman_service_t svc_b = {
        .name = "service_b",
        .build_context = mock_build_b,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_b), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_superhuman_build_context(&reg, &alloc, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "### Superhuman Insights") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "#### service_a") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "ctx_a") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "#### service_b") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "ctx_b") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
}

static void superhuman_register_at_max(void) {
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    static char names[HU_SUPERHUMAN_MAX_SERVICES][16];
    hu_superhuman_service_t svc = {
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };

    for (size_t i = 0; i < HU_SUPERHUMAN_MAX_SERVICES; i++) {
        int n = snprintf(names[i], sizeof(names[i]), "svc_%zu", i);
        svc.name = names[i];
        (void)n;
        HU_ASSERT_EQ(hu_superhuman_register(&reg, svc), HU_OK);
    }

    svc.name = "extra";
    hu_error_t err = hu_superhuman_register(&reg, svc);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);
}

static void superhuman_observe_null_text_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    hu_superhuman_service_t svc = {
        .name = "svc",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc), HU_OK);

    hu_error_t err = hu_superhuman_observe_all(&reg, &alloc, NULL, 0, "user", 4);
    HU_ASSERT_EQ(err, HU_OK);
}

static void superhuman_build_context_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_error_t err = hu_superhuman_build_context(&reg, &alloc, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, 0u);
}

#ifdef HU_ENABLE_SQLITE
static void superhuman_phase3_tables_exist(void) {
    static const char *const expected[] = {
        "inside_jokes", "commitments", "temporal_patterns", "delayed_followups",
        "avoidance_patterns", "topic_baselines", "micro_moments", "growth_milestones",
        "pattern_observations",
    };
    static const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1,
                                &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    for (size_t i = 0; i < expected_count; i++) {
        bool found = false;
        sqlite3_reset(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 0);
            if (name && strcmp(name, expected[i]) == 0) {
                found = true;
                break;
            }
        }
        HU_ASSERT_TRUE(found);
    }

    sqlite3_finalize(stmt);
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_superhuman_tests(void) {
    HU_TEST_SUITE("superhuman");
    HU_RUN_TEST(superhuman_register_and_count);
    HU_RUN_TEST(superhuman_build_context_calls_all);
    HU_RUN_TEST(superhuman_register_at_max);
    HU_RUN_TEST(superhuman_observe_null_text_ok);
    HU_RUN_TEST(superhuman_build_context_empty);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(superhuman_phase3_tables_exist);
#endif
}
