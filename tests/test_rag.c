#include "test_framework.h"
#include "seaclaw/rag.h"
#include "seaclaw/core/allocator.h"

static void test_rag_init_free(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_error_t err = sc_rag_init(&rag, &alloc);
    SC_ASSERT_EQ(err, SC_OK);
    sc_rag_free(&rag);
}

static void test_rag_add_chunks(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);
    sc_rag_add_chunk(&rag, "nucleo-f401re", "datasheet.md", "GPIO PA5 is the user LED");
    sc_rag_add_chunk(&rag, NULL, "general.md", "I2C uses SDA and SCL lines");
    SC_ASSERT_EQ(rag.chunk_count, 2u);
    sc_rag_free(&rag);
}

static void test_rag_retrieve_keyword_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);
    sc_rag_add_chunk(&rag, "nucleo-f401re", "ds1.md", "The user LED is connected to GPIO PA5");
    sc_rag_add_chunk(&rag, NULL, "ds2.md", "SPI uses MOSI MISO and SCK pins");
    sc_rag_add_chunk(&rag, NULL, "ds3.md", "The LED blinks when GPIO is high");

    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, &alloc, "LED GPIO", 8, NULL, 0, 2, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_NOT_NULL(results);

    alloc.free(alloc.ctx, (void *)results, count * sizeof(const sc_datasheet_chunk_t *));
    sc_rag_free(&rag);
}

static void test_rag_retrieve_board_boost(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);
    sc_rag_add_chunk(&rag, "nucleo-f401re", "ds1.md", "GPIO configuration");
    sc_rag_add_chunk(&rag, "esp32", "ds2.md", "GPIO configuration");

    const char *boards[] = {"nucleo-f401re"};
    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, &alloc, "GPIO", 4, boards, 1, 2, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_STR_EQ(results[0]->board, "nucleo-f401re");

    alloc.free(alloc.ctx, (void *)results, count * sizeof(const sc_datasheet_chunk_t *));
    sc_rag_free(&rag);
}

static void test_rag_retrieve_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);

    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, &alloc, "anything", 8, NULL, 0, 5, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_rag_free(&rag);
}

static void test_rag_retrieve_empty_backend_returns_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);

    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, &alloc, "query", 5, NULL, 0, 10, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    SC_ASSERT_NULL(results);
    sc_rag_free(&rag);
}

static void test_rag_retrieve_malformed_query_whitespace_only(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);
    sc_rag_add_chunk(&rag, NULL, "a.md", "content");

    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, &alloc, "   ", 3, NULL, 0, 5, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_rag_free(&rag);
}

static void test_rag_retrieve_query_zero_len_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);
    sc_rag_add_chunk(&rag, NULL, "a.md", "content");

    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, &alloc, "x", 0, NULL, 0, 5, &results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_rag_free(&rag);
}

static void test_rag_init_null_alloc_fails(void) {
    sc_rag_t rag;
    sc_error_t err = sc_rag_init(&rag, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_rag_init_null_rag_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_rag_init(NULL, &alloc);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_rag_retrieve_null_alloc_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rag_t rag;
    sc_rag_init(&rag, &alloc);
    sc_rag_add_chunk(&rag, NULL, "a.md", "content");

    const sc_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    sc_error_t err = sc_rag_retrieve(&rag, NULL, "query", 5, NULL, 0, 5, &results, &count);
    SC_ASSERT_NEQ(err, SC_OK);

    sc_rag_free(&rag);
}

void run_rag_tests(void) {
    SC_TEST_SUITE("RAG");
    SC_RUN_TEST(test_rag_init_free);
    SC_RUN_TEST(test_rag_add_chunks);
    SC_RUN_TEST(test_rag_retrieve_keyword_match);
    SC_RUN_TEST(test_rag_retrieve_board_boost);
    SC_RUN_TEST(test_rag_retrieve_empty);
    SC_RUN_TEST(test_rag_retrieve_empty_backend_returns_ok);
    SC_RUN_TEST(test_rag_retrieve_malformed_query_whitespace_only);
    SC_RUN_TEST(test_rag_retrieve_query_zero_len_ok);
    SC_RUN_TEST(test_rag_init_null_alloc_fails);
    SC_RUN_TEST(test_rag_init_null_rag_fails);
    SC_RUN_TEST(test_rag_retrieve_null_alloc_fails);
}
