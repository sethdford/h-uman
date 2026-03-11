#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/feeds/processor.h"
#include "human/memory.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

static void feed_processor_store_item_get_recent_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_feed_processor_t proc = {.alloc = &alloc, .db = db};
    hu_feed_item_stored_t item = {0};
    snprintf(item.source, sizeof(item.source), "facebook");
    snprintf(item.contact_id, sizeof(item.contact_id), "user_123");
    snprintf(item.content_type, sizeof(item.content_type), "post");
    snprintf(item.content, sizeof(item.content), "Hello world");
    item.content_len = 11;
    snprintf(item.url, sizeof(item.url), "https://example.com/post/1");
    item.ingested_at = 1700000000;

    hu_error_t err = hu_feed_processor_store_item(&proc, &item);
    HU_ASSERT_EQ(err, HU_OK);

    hu_feed_item_stored_t *out = NULL;
    size_t count = 0;
    err = hu_feed_processor_get_recent(&alloc, db, "facebook", 8, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(out[0].source, "facebook");
    HU_ASSERT_STR_EQ(out[0].contact_id, "user_123");
    HU_ASSERT_STR_EQ(out[0].content_type, "post");
    HU_ASSERT_STR_EQ(out[0].content, "Hello world");
    HU_ASSERT_STR_EQ(out[0].url, "https://example.com/post/1");
    HU_ASSERT_EQ(out[0].ingested_at, 1700000000);

    hu_feed_items_free(&alloc, out, count);
    mem.vtable->deinit(mem.ctx);
}

static void feed_processor_get_for_contact_scoped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_feed_processor_t proc = {.alloc = &alloc, .db = db};

    hu_feed_item_stored_t item1 = {0};
    snprintf(item1.source, sizeof(item1.source), "instagram");
    snprintf(item1.contact_id, sizeof(item1.contact_id), "user_a");
    snprintf(item1.content_type, sizeof(item1.content_type), "post");
    snprintf(item1.content, sizeof(item1.content), "Post from A");
    item1.content_len = 12;
    item1.ingested_at = 1700000001;
    HU_ASSERT_EQ(hu_feed_processor_store_item(&proc, &item1), HU_OK);

    hu_feed_item_stored_t item2 = {0};
    snprintf(item2.source, sizeof(item2.source), "instagram");
    snprintf(item2.contact_id, sizeof(item2.contact_id), "user_b");
    snprintf(item2.content_type, sizeof(item2.content_type), "post");
    snprintf(item2.content, sizeof(item2.content), "Post from B");
    item2.content_len = 12;
    item2.ingested_at = 1700000002;
    HU_ASSERT_EQ(hu_feed_processor_store_item(&proc, &item2), HU_OK);

    hu_feed_item_stored_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_feed_processor_get_for_contact(&alloc, db, "user_a", 6, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(out[0].contact_id, "user_a");
    HU_ASSERT_STR_EQ(out[0].content, "Post from A");

    hu_feed_items_free(&alloc, out, count);
    mem.vtable->deinit(mem.ctx);
}

static void feed_processor_empty_count_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_feed_item_stored_t *out = NULL;
    size_t count = 99;
    hu_error_t err = hu_feed_processor_get_recent(&alloc, db, "facebook", 8, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(count, 0u);

    out = NULL;
    count = 99;
    err = hu_feed_processor_get_for_contact(&alloc, db, "user_x", 6, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}

void run_feed_processor_tests(void) {
    HU_TEST_SUITE("feed_processor");
    HU_RUN_TEST(feed_processor_store_item_get_recent_found);
    HU_RUN_TEST(feed_processor_get_for_contact_scoped);
    HU_RUN_TEST(feed_processor_empty_count_zero);
}

#else

void run_feed_processor_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
