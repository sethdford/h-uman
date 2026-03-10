#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/promotion.h"
#include "human/memory/stm.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void promotion_importance_higher_for_frequent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    hu_stm_record_turn(&buf, "user", 4, "Hello", 5, 1000);
    hu_stm_turn_add_entity(&buf, 0, "Alice", 5, "person", 6, 1);

    hu_stm_record_turn(&buf, "user", 4, "Alice said hi", 13, 1001);
    hu_stm_turn_add_entity(&buf, 1, "Alice", 5, "person", 6, 2);

    hu_stm_record_turn(&buf, "user", 4, "Alice again", 11, 1002);
    hu_stm_turn_add_entity(&buf, 2, "Alice", 5, "person", 6, 1);

    hu_stm_record_turn(&buf, "user", 4, "Bob once", 8, 1003);
    hu_stm_turn_add_entity(&buf, 3, "Bob", 3, "person", 6, 1);

    hu_stm_entity_t alice = {.name = "Alice",
                             .name_len = 5,
                             .type = "person",
                             .type_len = 6,
                             .mention_count = 4,
                             .importance = 0.0};
    hu_stm_entity_t bob = {.name = "Bob",
                           .name_len = 3,
                           .type = "person",
                           .type_len = 6,
                           .mention_count = 1,
                           .importance = 0.0};

    double imp_alice = hu_promotion_entity_importance(&alice, &buf);
    double imp_bob = hu_promotion_entity_importance(&bob, &buf);

    HU_ASSERT_TRUE(imp_alice > imp_bob);

    hu_stm_deinit(&buf);
}

static void promotion_skips_low_importance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    hu_stm_record_turn(&buf, "user", 4, "X", 1, 1000);
    hu_stm_turn_add_entity(&buf, 0, "X", 1, "topic", 5, 1);

    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_promotion_config_t config = HU_PROMOTION_DEFAULTS;
    config.min_mention_count = 2;
    config.min_importance = 0.5;

    hu_error_t err = hu_promotion_run(&alloc, &buf, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "entity:X", 8, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_promotes_qualifying_entities(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    hu_stm_record_turn(&buf, "user", 4, "Entity0", 7, 1000);
    hu_stm_turn_add_entity(&buf, 0, "Entity0", 7, "person", 6, 2);

    hu_stm_record_turn(&buf, "user", 4, "Entity0 again", 14, 1001);
    hu_stm_turn_add_entity(&buf, 1, "Entity0", 7, "person", 6, 2);

    hu_stm_record_turn(&buf, "user", 4, "Entity1", 7, 1002);
    hu_stm_turn_add_entity(&buf, 2, "Entity1", 7, "person", 6, 2);

    hu_stm_record_turn(&buf, "user", 4, "Entity1 too", 11, 1003);
    hu_stm_turn_add_entity(&buf, 3, "Entity1", 7, "person", 6, 2);

    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_promotion_config_t config = HU_PROMOTION_DEFAULTS;
    config.min_mention_count = 1;
    config.min_importance = 0.0;

    hu_error_t err = hu_promotion_run(&alloc, &buf, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_emotions_stores_high_intensity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess_emotions", 14);

    hu_stm_record_turn(&buf, "user", 4, "I'm so happy!", 13, 1234567890);
    hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.8);

    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_error_t err = hu_promotion_run_emotions(&alloc, &buf, &mem, "user_a", 5);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);

    hu_memory_entry_t *recall_out = NULL;
    size_t recall_count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "joy", 3, 10, NULL, 0, &recall_out, &recall_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(recall_count >= 1);

    bool found_joy = false;
    for (size_t i = 0; i < recall_count; i++) {
        if (recall_out[i].content && strstr(recall_out[i].content, "0.80") != NULL) {
            found_joy = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_joy);

    for (size_t i = 0; i < recall_count; i++)
        hu_memory_entry_free_fields(&alloc, &recall_out[i]);
    alloc.free(alloc.ctx, recall_out, recall_count * sizeof(hu_memory_entry_t));

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_emotions_skips_low_intensity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess_low", 8);

    hu_stm_record_turn(&buf, "user", 4, "Meh", 3, 1000);
    hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.2);

    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_error_t err = hu_promotion_run_emotions(&alloc, &buf, &mem, "user_a", 5);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "emotion:user_a:1000:joy", 22, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(found);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_run_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_promotion_config_t config = HU_PROMOTION_DEFAULTS;

    HU_ASSERT_EQ(hu_promotion_run(NULL, &buf, &mem, &config), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_promotion_run(&alloc, NULL, &mem, &config), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_promotion_run(&alloc, &buf, NULL, &config), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_promotion_run(&alloc, &buf, &mem, NULL), HU_ERR_INVALID_ARGUMENT);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_run_empty_memory_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);
    /* No turns recorded — buffer is empty */
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_promotion_config_t config = HU_PROMOTION_DEFAULTS;

    hu_error_t err = hu_promotion_run(&alloc, &buf, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_run_emotions_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);

    HU_ASSERT_EQ(hu_promotion_run_emotions(NULL, &buf, &mem, "user_a", 5),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_promotion_run_emotions(&alloc, NULL, &mem, "user_a", 5),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_promotion_run_emotions(&alloc, &buf, NULL, "user_a", 5),
                 HU_ERR_INVALID_ARGUMENT);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

static void promotion_entity_importance_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    hu_stm_record_turn(&buf, "user", 4, "Alice", 5, 1000);
    hu_stm_turn_add_entity(&buf, 0, "Alice", 5, "person", 6, 3);

    hu_stm_entity_t alice = {.name = "Alice",
                             .name_len = 5,
                             .type = "person",
                             .type_len = 6,
                             .mention_count = 3,
                             .importance = 0.0};

    double imp = hu_promotion_entity_importance(&alice, &buf);
    HU_ASSERT_TRUE(imp >= 0.0 && imp <= 1.0);
    HU_ASSERT_TRUE(imp > 0.0);

    hu_stm_deinit(&buf);
}

static void promotion_emotions_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);

    hu_error_t err = hu_promotion_run_emotions(&alloc, &buf, NULL, "user_a", 5);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_stm_deinit(&buf);
}

static void promotion_respects_max_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    for (int i = 0; i < 10; i++) {
        char content[32];
        int n = snprintf(content, sizeof(content), "Entity%d", i);
        hu_stm_record_turn(&buf, "user", 4, content, (size_t)n, (uint64_t)(1000 + i));
        hu_stm_turn_add_entity(&buf, (size_t)i, content, (size_t)n, "person", 6, 2);
    }

    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_promotion_config_t config = HU_PROMOTION_DEFAULTS;
    config.min_mention_count = 1;
    config.min_importance = 0.0;
    config.max_entities = 3;

    hu_error_t err = hu_promotion_run(&alloc, &buf, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count <= 3);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&buf);
}

void run_promotion_tests(void) {
    HU_TEST_SUITE("promotion");
    HU_RUN_TEST(promotion_importance_higher_for_frequent);
    HU_RUN_TEST(promotion_skips_low_importance);
    HU_RUN_TEST(promotion_promotes_qualifying_entities);
    HU_RUN_TEST(promotion_respects_max_cap);
    HU_RUN_TEST(promotion_run_null_args_returns_error);
    HU_RUN_TEST(promotion_run_empty_memory_succeeds);
    HU_RUN_TEST(promotion_run_emotions_null_args_returns_error);
    HU_RUN_TEST(promotion_entity_importance_basic);
    HU_RUN_TEST(promotion_emotions_stores_high_intensity);
    HU_RUN_TEST(promotion_emotions_skips_low_intensity);
    HU_RUN_TEST(promotion_emotions_null_args);
}
