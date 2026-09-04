#include "human/bootstrap.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static void bootstrap_null_ctx_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_app_bootstrap(NULL, &alloc, NULL, false, false), HU_ERR_INVALID_ARGUMENT);
}

static void bootstrap_null_alloc_returns_error(void) {
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_app_bootstrap(&ctx, NULL, NULL, false, false), HU_ERR_INVALID_ARGUMENT);
}

static void teardown_null_is_safe(void) {
    hu_app_teardown(NULL);
}

static void teardown_zero_ctx_is_safe(void) {
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_app_teardown(&ctx);
}

static void bootstrap_minimal_no_agent_no_channels(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, false, false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx.alloc);
    HU_ASSERT_NOT_NULL(ctx.cfg);
    HU_ASSERT_NOT_NULL(ctx.tools);
    HU_ASSERT_TRUE(ctx.tools_count > 0);
    HU_ASSERT_TRUE(ctx.channel_count == 0);
    HU_ASSERT_FALSE(ctx.agent_ok);
    hu_app_teardown(&ctx);
}

static void bootstrap_with_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, true, false);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(ctx.provider);
        HU_ASSERT_NOT_NULL(ctx.memory);
        HU_ASSERT_TRUE(ctx.provider_ok);
        hu_app_teardown(&ctx);
    }
}

#ifdef HU_ENABLE_SQLITE
/* 2026-09-04 prod abort (asan.log.{1400,30452,85605}): bootstrap attached the
 * semantic index to two BLOCK-SCOPED locals, copied the structs into the
 * lifetime-owned bi-> fields, and let the block end. The sqlite engine kept
 * the dead addresses; every later store() read reused stack in
 * semantic_index_row. The engine must borrow the pair that outlives it. */
static void bootstrap_semantic_index_borrows_the_lifetime_owned_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    setenv("HU_SEMANTIC_RECALL", "shadow", 1);
    setenv("HU_MEMORY_SQLITE_PATH", ":memory:", 1); /* never the live store */
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, true, false);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_MEMORY_SQLITE_PATH");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx.memory);
    HU_ASSERT_NOT_NULL(ctx.embedder);
    HU_ASSERT_NOT_NULL(ctx.vector_store);
    struct hu_embedder *emb = NULL;
    struct hu_vector_store *vs = NULL;
    hu_sqlite_memory_get_semantic_index(ctx.memory, &emb, &vs);
    HU_ASSERT_NOT_NULL(emb); /* SHADOW attached the real pair */
    HU_ASSERT_NOT_NULL(vs);
    /* The engine borrows the pair the app context owns for its whole life —
     * not a copy that died with an inner block. */
    HU_ASSERT(emb == (struct hu_embedder *)ctx.embedder);
    HU_ASSERT(vs == (struct hu_vector_store *)ctx.vector_store);
    /* And a store through the engine after bootstrap must be safe (ASan). */
    HU_ASSERT_EQ(ctx.memory->vtable->store(ctx.memory->ctx, "k", 1, "hello", 5, NULL, "", 0),
                 HU_OK);
    hu_app_teardown(&ctx);
}
#endif

void run_bootstrap_tests(void) {
    HU_TEST_SUITE("Bootstrap");

    HU_RUN_TEST(bootstrap_null_ctx_returns_error);
    HU_RUN_TEST(bootstrap_null_alloc_returns_error);
    HU_RUN_TEST(teardown_null_is_safe);
    HU_RUN_TEST(teardown_zero_ctx_is_safe);
    HU_RUN_TEST(bootstrap_minimal_no_agent_no_channels);
    HU_RUN_TEST(bootstrap_with_agent);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(bootstrap_semantic_index_borrows_the_lifetime_owned_pair);
#endif
}
