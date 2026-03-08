#include "seaclaw/core/allocator.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/memory/promotion.h"
#include "seaclaw/memory/stm.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void promotion_importance_higher_for_frequent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "sess", 4);

    sc_stm_record_turn(&buf, "user", 4, "Hello", 5, 1000);
    sc_stm_turn_add_entity(&buf, 0, "Alice", 5, "person", 6, 1);

    sc_stm_record_turn(&buf, "user", 4, "Alice said hi", 13, 1001);
    sc_stm_turn_add_entity(&buf, 1, "Alice", 5, "person", 6, 2);

    sc_stm_record_turn(&buf, "user", 4, "Alice again", 11, 1002);
    sc_stm_turn_add_entity(&buf, 2, "Alice", 5, "person", 6, 1);

    sc_stm_record_turn(&buf, "user", 4, "Bob once", 8, 1003);
    sc_stm_turn_add_entity(&buf, 3, "Bob", 3, "person", 6, 1);

    sc_stm_entity_t alice = {.name = "Alice",
                             .name_len = 5,
                             .type = "person",
                             .type_len = 6,
                             .mention_count = 4,
                             .importance = 0.0};
    sc_stm_entity_t bob = {.name = "Bob",
                           .name_len = 3,
                           .type = "person",
                           .type_len = 6,
                           .mention_count = 1,
                           .importance = 0.0};

    double imp_alice = sc_promotion_entity_importance(&alice, &buf);
    double imp_bob = sc_promotion_entity_importance(&bob, &buf);

    SC_ASSERT_TRUE(imp_alice > imp_bob);

    sc_stm_deinit(&buf);
}

static void promotion_skips_low_importance(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "sess", 4);

    sc_stm_record_turn(&buf, "user", 4, "X", 1, 1000);
    sc_stm_turn_add_entity(&buf, 0, "X", 1, "topic", 5, 1);

    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_promotion_config_t config = SC_PROMOTION_DEFAULTS;
    config.min_mention_count = 2;
    config.min_importance = 0.5;

    sc_error_t err = sc_promotion_run(&alloc, &buf, &mem, &config);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "entity:X", 8, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_FALSE(found);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    sc_stm_deinit(&buf);
}

static void promotion_promotes_qualifying_entities(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "sess", 4);

    sc_stm_record_turn(&buf, "user", 4, "Entity0", 7, 1000);
    sc_stm_turn_add_entity(&buf, 0, "Entity0", 7, "person", 6, 2);

    sc_stm_record_turn(&buf, "user", 4, "Entity0 again", 14, 1001);
    sc_stm_turn_add_entity(&buf, 1, "Entity0", 7, "person", 6, 2);

    sc_stm_record_turn(&buf, "user", 4, "Entity1", 7, 1002);
    sc_stm_turn_add_entity(&buf, 2, "Entity1", 7, "person", 6, 2);

    sc_stm_record_turn(&buf, "user", 4, "Entity1 too", 11, 1003);
    sc_stm_turn_add_entity(&buf, 3, "Entity1", 7, "person", 6, 2);

    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);
    sc_promotion_config_t config = SC_PROMOTION_DEFAULTS;
    config.min_mention_count = 1;
    config.min_importance = 0.0;

    sc_error_t err = sc_promotion_run(&alloc, &buf, &mem, &config);
    SC_ASSERT_EQ(err, SC_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    sc_stm_deinit(&buf);
}

static void promotion_respects_max_cap(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "sess", 4);

    for (int i = 0; i < 10; i++) {
        char content[32];
        int n = snprintf(content, sizeof(content), "Entity%d", i);
        sc_stm_record_turn(&buf, "user", 4, content, (size_t)n, (uint64_t)(1000 + i));
        sc_stm_turn_add_entity(&buf, (size_t)i, content, (size_t)n, "person", 6, 2);
    }

    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_promotion_config_t config = SC_PROMOTION_DEFAULTS;
    config.min_mention_count = 1;
    config.min_importance = 0.0;
    config.max_entities = 3;

    sc_error_t err = sc_promotion_run(&alloc, &buf, &mem, &config);
    SC_ASSERT_EQ(err, SC_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count <= 3);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    sc_stm_deinit(&buf);
}

void run_promotion_tests(void) {
    SC_TEST_SUITE("promotion");
    SC_RUN_TEST(promotion_importance_higher_for_frequent);
    SC_RUN_TEST(promotion_skips_low_importance);
    SC_RUN_TEST(promotion_promotes_qualifying_entities);
    SC_RUN_TEST(promotion_respects_max_cap);
}
