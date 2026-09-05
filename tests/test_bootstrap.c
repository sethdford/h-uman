#include "human/bootstrap.h"
#include "human/channels/pwa.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#if HU_HAS_PWA
/* 2026-09-04 audit: bootstrap registered the PWA poll fn but never called
 * the channel's start(), so hu_pwa_channel_poll returned on every tick and
 * ten configured apps produced nothing. The registered channel must be the
 * started one. */
static void bootstrap_starts_the_pwa_channel_it_registers(void) {
    char dir[] = "/tmp/hu_bootstrap_pwa_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
    FILE *f = fopen(cfg_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"default_provider\":\"ollama\",\"channels\":{\"pwa\":{\"apps\":[\"slack\"]}}}", f);
    fclose(f);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_app_bootstrap(&ctx, &alloc, cfg_path, false, true), HU_OK);
    const hu_channel_t *pwa = NULL;
    for (size_t i = 0; i < ctx.channel_count; i++)
        if (ctx.channels[i].poll_fn == hu_pwa_channel_poll)
            pwa = ctx.channels[i].channel;
    HU_ASSERT_NOT_NULL(pwa);
    HU_ASSERT_TRUE(hu_pwa_channel_is_running(pwa));
    hu_app_teardown(&ctx);
    unlink(cfg_path);
    rmdir(dir);
}
#endif

#ifdef HU_ENABLE_SQLITE
/* 2026-09-02..04: with HU_SEMANTIC_RECALL on, bootstrap attached the sqlite
 * engine's semantic index to two BLOCK-SCOPED locals and copied them into the
 * app context afterwards. The engine keeps the addresses it is handed and
 * dereferences them on every indexed store, so the first store after
 * bootstrap read a dead stack slot — 21 ASan aborts of the daemon in
 * semantic_index_row, one per restart. The contract: the engine's attached
 * pointers ARE the app-lifetime objects the context exposes, and a store
 * through the engine after bootstrap returns is safe. */
static void bootstrap_semantic_index_points_at_app_lifetime_embedder(void) {
    char dir[] = "/tmp/hu_bootstrap_sem_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
    FILE *f = fopen(cfg_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"default_provider\":\"ollama\",\"memory\":{\"backend\":\"sqlite\"}}", f);
    fclose(f);
    /* Never the real ~/.human/memory.db; the embed URL is never reached
     * because the test transport is a mock (the index insert fails and is
     * logged, exactly as in test_semantic_recall). */
    setenv("HU_MEMORY_SQLITE_PATH", ":memory:", 1);
    setenv("HU_SEMANTIC_RECALL", "shadow", 1);
    setenv("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749", 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, cfg_path, true, false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx.memory);
    HU_ASSERT_NOT_NULL(ctx.embedder);
    HU_ASSERT_NOT_NULL(ctx.vector_store);

    hu_embedder_t *eng_emb = NULL;
    hu_vector_store_t *eng_vs = NULL;
    hu_sqlite_memory_get_semantic_index(ctx.memory, &eng_emb, &eng_vs);
    /* Attached at all — otherwise the pointer checks below would be vacuous. */
    HU_ASSERT_NOT_NULL(eng_emb);
    HU_ASSERT_NOT_NULL(eng_vs);
    /* ...and attached to the app-lifetime objects, not to a dead temporary. */
    HU_ASSERT_TRUE(eng_emb == (hu_embedder_t *)ctx.embedder);
    HU_ASSERT_TRUE(eng_vs == (hu_vector_store_t *)ctx.vector_store);
    HU_ASSERT_NOT_NULL(eng_emb->vtable);
    HU_ASSERT_NOT_NULL(eng_emb->ctx);

    /* The store that aborted the daemon: index a row AFTER bootstrap returned.
     * Under ASan the pre-fix code dies here with stack-use-after-scope. */
    HU_ASSERT_EQ(
        ctx.memory->vtable->store(ctx.memory->ctx, "user_a:fact", 11, "likes tea", 9, NULL, "", 0),
        HU_OK);

    hu_app_teardown(&ctx);
    unsetenv("HU_SEMANTIC_EMBED_URL");
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_MEMORY_SQLITE_PATH");
    unlink(cfg_path);
    rmdir(dir);
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
    HU_RUN_TEST(bootstrap_semantic_index_points_at_app_lifetime_embedder);
#if HU_HAS_PWA
    HU_RUN_TEST(bootstrap_starts_the_pwa_channel_it_registers);
#endif
#endif
}
