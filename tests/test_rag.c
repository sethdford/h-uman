#include "human/core/allocator.h"
#include "human/rag.h"
#include "test_framework.h"

static void test_rag_init_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_error_t err = hu_rag_init(&rag, &alloc);
    HU_ASSERT_EQ(err, HU_OK);
    hu_rag_free(&rag);
}

static void test_rag_add_chunks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);
    hu_rag_add_chunk(&rag, "nucleo-f401re", "datasheet.md", "GPIO PA5 is the user LED");
    hu_rag_add_chunk(&rag, NULL, "general.md", "I2C uses SDA and SCL lines");
    HU_ASSERT_EQ(rag.chunk_count, 2u);
    hu_rag_free(&rag);
}

static void test_rag_retrieve_keyword_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);
    hu_rag_add_chunk(&rag, "nucleo-f401re", "ds1.md", "The user LED is connected to GPIO PA5");
    hu_rag_add_chunk(&rag, NULL, "ds2.md", "SPI uses MOSI MISO and SCK pins");
    hu_rag_add_chunk(&rag, NULL, "ds3.md", "The LED blinks when GPIO is high");

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, &alloc, "LED GPIO", 8, NULL, 0, 2, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(results);

    alloc.free(alloc.ctx, (void *)results, count * sizeof(const hu_datasheet_chunk_t *));
    hu_rag_free(&rag);
}

static void test_rag_retrieve_board_boost(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);
    hu_rag_add_chunk(&rag, "nucleo-f401re", "ds1.md", "GPIO configuration");
    hu_rag_add_chunk(&rag, "esp32", "ds2.md", "GPIO configuration");

    const char *boards[] = {"nucleo-f401re"};
    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, &alloc, "GPIO", 4, boards, 1, 2, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(results[0]->board, "nucleo-f401re");

    alloc.free(alloc.ctx, (void *)results, count * sizeof(const hu_datasheet_chunk_t *));
    hu_rag_free(&rag);
}

static void test_rag_retrieve_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, &alloc, "anything", 8, NULL, 0, 5, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_rag_free(&rag);
}

static void test_rag_retrieve_empty_backend_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, &alloc, "query", 5, NULL, 0, 10, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(results);
    hu_rag_free(&rag);
}

static void test_rag_retrieve_malformed_query_whitespace_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);
    hu_rag_add_chunk(&rag, NULL, "a.md", "content");

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, &alloc, "   ", 3, NULL, 0, 5, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_rag_free(&rag);
}

static void test_rag_retrieve_query_zero_len_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);
    hu_rag_add_chunk(&rag, NULL, "a.md", "content");

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, &alloc, "x", 0, NULL, 0, 5, &results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_rag_free(&rag);
}

static void test_rag_init_null_alloc_fails(void) {
    hu_rag_t rag;
    hu_error_t err = hu_rag_init(&rag, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_rag_init_null_rag_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_rag_init(NULL, &alloc);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_rag_retrieve_null_alloc_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_t rag;
    hu_rag_init(&rag, &alloc);
    hu_rag_add_chunk(&rag, NULL, "a.md", "content");

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    hu_error_t err = hu_rag_retrieve(&rag, NULL, "query", 5, NULL, 0, 5, &results, &count);
    HU_ASSERT_NEQ(err, HU_OK);

    hu_rag_free(&rag);
}

void run_rag_tests(void) {
    HU_TEST_SUITE("RAG");
    HU_RUN_TEST(test_rag_init_free);
    HU_RUN_TEST(test_rag_add_chunks);
    HU_RUN_TEST(test_rag_retrieve_keyword_match);
    HU_RUN_TEST(test_rag_retrieve_board_boost);
    HU_RUN_TEST(test_rag_retrieve_empty);
    HU_RUN_TEST(test_rag_retrieve_empty_backend_returns_ok);
    HU_RUN_TEST(test_rag_retrieve_malformed_query_whitespace_only);
    HU_RUN_TEST(test_rag_retrieve_query_zero_len_ok);
    HU_RUN_TEST(test_rag_init_null_alloc_fails);
    HU_RUN_TEST(test_rag_init_null_rag_fails);
    HU_RUN_TEST(test_rag_retrieve_null_alloc_fails);
}
