#include "test_framework.h"

#include "human/context_engine.h"
#include "human/context_engine_rag.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

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
    HU_ASSERT_EQ(hu_context_engine_rag_create(&s_alloc, &cfg, &engine), HU_OK);
    HU_ASSERT_NOT_NULL(engine.ctx);
    HU_ASSERT_NOT_NULL(engine.vtable);
    HU_ASSERT_STR_EQ(engine.vtable->get_name(engine.ctx), "rag");
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_create_null_args(void) {
    hu_context_engine_t engine;
    HU_ASSERT_EQ(hu_context_engine_rag_create(NULL, NULL, &engine), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_context_engine_rag_create(&s_alloc, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_rag_bootstrap(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    HU_ASSERT_EQ(engine.vtable->bootstrap(engine.ctx, &s_alloc), HU_OK);
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
    HU_ASSERT_EQ(engine.vtable->ingest(engine.ctx, &s_alloc, &msg1), HU_OK);

    hu_context_message_t msg2 = {
        .role = "assistant",
        .role_len = 9,
        .content = "Hi! How can I help?",
        .content_len = 19,
        .timestamp = 1001,
    };
    HU_ASSERT_EQ(engine.vtable->ingest(engine.ctx, &s_alloc, &msg2), HU_OK);

    hu_context_budget_t budget = {
        .max_tokens = 1000,
        .reserved_tokens = 100,
    };
    hu_assembled_context_t assembled;
    HU_ASSERT_EQ(engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled), HU_OK);
    HU_ASSERT_EQ(assembled.messages_count, 2);
    HU_ASSERT_GT(assembled.total_tokens, 0);

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
    HU_ASSERT_LE(assembled.messages_count, 2);
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

    HU_ASSERT_EQ(engine.vtable->compact(engine.ctx, &s_alloc, 20), HU_OK);

    hu_context_budget_t budget = {.max_tokens = 10000, .reserved_tokens = 0};
    hu_assembled_context_t assembled;
    engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled);
    HU_ASSERT_LT(assembled.messages_count, 10);

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
    HU_ASSERT_EQ(engine.vtable->after_turn(engine.ctx, &s_alloc, &user, &asst), HU_OK);

    hu_context_budget_t budget = {.max_tokens = 10000, .reserved_tokens = 0};
    hu_assembled_context_t assembled;
    engine.vtable->assemble(engine.ctx, &s_alloc, &budget, &assembled);
    HU_ASSERT_EQ(assembled.messages_count, 2);

    hu_assembled_context_free(&s_alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_subagent_not_supported(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_config_t cfg = {0};
    hu_context_engine_rag_create(&s_alloc, &cfg, &engine);
    engine.vtable->bootstrap(engine.ctx, &s_alloc);

    hu_context_engine_t sub;
    HU_ASSERT_EQ(engine.vtable->prepare_subagent(engine.ctx, &s_alloc, &sub),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(engine.vtable->merge_subagent(engine.ctx, &s_alloc, &sub),
                 HU_ERR_NOT_SUPPORTED);

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
    HU_ASSERT(assembled.total_tokens <= 10 || assembled.messages_count <= 1);

    hu_assembled_context_free(&s_alloc, &assembled);
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

static void test_rag_default_config(void) {
    hu_context_engine_t engine;
    hu_context_engine_rag_create(&s_alloc, NULL, &engine);
    HU_ASSERT_NOT_NULL(engine.vtable);
    HU_ASSERT_STR_EQ(engine.vtable->get_name(engine.ctx), "rag");
    engine.vtable->deinit(engine.ctx, &s_alloc);
}

void run_context_engine_rag_tests(void) {
    HU_TEST_SUITE("context_engine_rag");
    HU_RUN_TEST(test_rag_create);
    HU_RUN_TEST(test_rag_create_null_args);
    HU_RUN_TEST(test_rag_bootstrap);
    HU_RUN_TEST(test_rag_ingest_and_assemble);
    HU_RUN_TEST(test_rag_recency_limit);
    HU_RUN_TEST(test_rag_compact);
    HU_RUN_TEST(test_rag_after_turn);
    HU_RUN_TEST(test_rag_subagent_not_supported);
    HU_RUN_TEST(test_rag_budget_respects_token_limit);
    HU_RUN_TEST(test_rag_default_config);
}
