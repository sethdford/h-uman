#include "human/context_engine.h"
#include "human/context_engine_rag.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define RUN(fn)           \
    do {                  \
        s_tests_run++;    \
        fn();             \
        s_tests_passed++; \
    } while (0)

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t s_alloc = {.alloc = test_alloc, .free = test_free};

static void test_rag_create(void) {
    hu_context_engine_t engine;
    memset(&engine, 0, sizeof(engine));
    hu_context_engine_rag_config_t cfg = {0};
    cfg.max_recency_messages = 10;
    assert(hu_context_engine_rag_create(&s_alloc, &cfg, &engine) == HU_OK);
    assert(engine.ctx != NULL);
    assert(engine.vtable != NULL);
    assert(strcmp(engine.vtable->get_name(engine.ctx), "rag") == 0);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_create_null_args(void) {
    hu_context_engine_t engine;
    assert(hu_context_engine_rag_create(NULL, NULL, &engine) == HU_ERR_INVALID_ARGUMENT);
    assert(hu_context_engine_rag_create(&s_alloc, NULL, NULL) == HU_ERR_INVALID_ARGUMENT);
}

static void test_rag_bootstrap(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    assert(engine.vtable->bootstrap(engine.ctx, &s_alloc) == HU_OK);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_ingest_and_assemble(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    cfg.max_recency_messages = 5;
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    hu_context_message_t msg1 = {
        .role = "user",
        .role_len = 4,
        .content = "Hello there",
        .content_len = 11,
        .timestamp = 1000,
    };
    assert(engine.vtable->ingest(engine.ctx, &s_alloc, &msg1) == HU_OK);

    hu_context_message_t msg2 = {
        .role = "assistant",
        .role_len = 9,
        .content = "Hi! How can I help?",
        .content_len = 19,
        .timestamp = 1001,
    };
    assert(engine.vtable->ingest(engine.ctx, &s_alloc, &msg2) == HU_OK);

    hu_context_budget_t budget = {
        .max_tokens = 1000,
        .reserved_tokens = 100,
    };
    hu_assembled_context_t assembled;
    assert(engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled) == HU_OK);
    assert(assembled.messages_count == 2);
    assert(assembled.total_tokens > 0);

    hu_assembled_context_free(&s_alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_recency_limit(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    cfg.max_recency_messages = 2;
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    for (int i = 0; i < 5; i++) {
        char content[32];
        snprintf(content, sizeof(content), "Message %d", i);
        hu_context_message_t msg = {
            .role = "user",
            .role_len = 4,
            .content = content,
            .content_len = strlen(content),
            .timestamp = 1000 + i,
        };
        engine.vtable->ingest(engine.ctx, &s_alloc, &msg);
    }

    hu_context_budget_t budget = {.max_tokens = 10000, .reserved_tokens = 0};
    hu_assembled_context_t assembled;
    engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled);
    assert(assembled.messages_count <= 2);
    hu_assembled_context_free(&s_alloc, &assembled);

    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_compact(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    for (int i = 0; i < 10; i++) {
        char content[64];
        snprintf(content, sizeof(content), "A longer message number %d with some content", i);
        hu_context_message_t msg = {
            .role = "user",
            .role_len = 4,
            .content = content,
            .content_len = strlen(content),
            .timestamp = 1000 + i,
        };
        engine.vtable->ingest(engine.ctx, &s_alloc, &msg);
    }

    assert(engine.vtable->compact(engine.ctx, &s_alloc, 20) == HU_OK);

    hu_context_budget_t budget = {.max_tokens = 10000, .reserved_tokens = 0};
    hu_assembled_context_t assembled;
    engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled);
    assert(assembled.messages_count < 10);

    hu_assembled_context_free(&s_alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_after_turn(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    hu_context_message_t user = {
        .role = "user",
        .role_len = 4,
        .content = "What is AI?",
        .content_len = 11,
        .timestamp = 2000,
    };
    hu_context_message_t asst = {
        .role = "assistant",
        .role_len = 9,
        .content = "AI is artificial intelligence.",
        .content_len = 30,
        .timestamp = 2001,
    };
    assert(engine.vtable->after_turn(engine.ctx, &s_alloc, &user, &asst) == HU_OK);

    hu_context_budget_t budget = {.max_tokens = 10000, .reserved_tokens = 0};
    hu_assembled_context_t assembled;
    engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled);
    assert(assembled.messages_count == 2);

    hu_assembled_context_free(&s_alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_subagent_not_supported(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    hu_context_engine_t sub;
    assert(engine.vtable->prepare_subagent(engine.ctx, &s_alloc, &sub) == HU_ERR_NOT_SUPPORTED);
    assert(engine.vtable->merge_subagent(engine.ctx, &s_alloc, &sub) == HU_ERR_NOT_SUPPORTED);

    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_budget_respects_token_limit(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    cfg.max_recency_messages = 100;
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    for (int i = 0; i < 20; i++) {
        char content[128];
        snprintf(content, sizeof(content),
                 "This is a moderately long message number %d with enough text to use tokens", i);
        hu_context_message_t msg = {
            .role = "user",
            .role_len = 4,
            .content = content,
            .content_len = strlen(content),
            .timestamp = 1000 + i,
        };
        engine.vtable->ingest(engine.ctx, &s_alloc, &msg);
    }

    hu_context_budget_t budget = {.max_tokens = 10, .reserved_tokens = 0};
    hu_assembled_context_t assembled;
    engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled);
    assert(assembled.total_tokens <= 10 || assembled.messages_count <= 1);

    hu_assembled_context_free(&s_alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_default_config(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_create(&s_alloc, NULL, &engine);
    assert(engine.vtable != NULL);
    assert(strcmp(engine.vtable->get_name(engine.ctx), "rag") == 0);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

#ifdef HU_ENABLE_SQLITE
/* rag_assemble frees every field of every recalled entry. It used to free
 * only key and content, leaking id/category/timestamp/session_id/source/
 * provenance per recalled memory on every assemble. */
static void test_rag_assemble_frees_every_recalled_field(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&sys, ":memory:");
    assert(mem.vtable != NULL);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CUSTOM};
    cat.data.custom.name = "notes";
    cat.data.custom.name_len = 5;
    const char *content = "the sailboat is docked at slip fourteen";
    assert(mem.vtable->store(mem.ctx, "boat", 4, content, strlen(content), &cat, "s1", 2) == HU_OK);

    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    cfg.memory = &mem;
    cfg.rag_min_relevance = 0.0001;
    assert(hu_context_engine_rag_create(&alloc, &cfg, &engine) == HU_OK);
    engine.vtable->bootstrap(engine.ctx, &alloc);
    hu_context_message_t msg = {.role = "user",
                                .role_len = 4,
                                .content = "where is the sailboat docked",
                                .content_len = 28,
                                .timestamp = 1000};
    assert(engine.vtable->ingest(engine.ctx, &alloc, &msg) == HU_OK);
    hu_context_budget_t budget = {.max_tokens = 4000, .reserved_tokens = 100};
    hu_assembled_context_t assembled;
    assert(engine.vtable->assemble(engine.ctx, &alloc, &budget, &assembled) == HU_OK);
    assert(assembled.injected_context != NULL); /* recall really ran */
    hu_assembled_context_free(&alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &alloc);
    size_t leaks = hu_tracking_allocator_leaks(ta);
    hu_tracking_allocator_destroy(ta);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    if (leaks != 0)
        fprintf(stderr, "  rag assemble leaked %zu allocation(s)\n", leaks);
    assert(leaks == 0);
}
#endif

int run_context_engine_rag_tests(void) {
    s_tests_run = 0;
    s_tests_passed = 0;

    RUN(test_rag_create);
    RUN(test_rag_create_null_args);
    RUN(test_rag_bootstrap);
    RUN(test_rag_ingest_and_assemble);
    RUN(test_rag_recency_limit);
    RUN(test_rag_compact);
    RUN(test_rag_after_turn);
    RUN(test_rag_subagent_not_supported);
    RUN(test_rag_budget_respects_token_limit);
    RUN(test_rag_default_config);
#ifdef HU_ENABLE_SQLITE
    RUN(test_rag_assemble_frees_every_recalled_field);
#endif

    printf("  context_engine_rag: %d/%d passed\n", s_tests_passed, s_tests_run);
    return s_tests_run - s_tests_passed;
}
