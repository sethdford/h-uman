/*
 * Tests for previously untested or lightly tested modules:
 * voice_channel, skill_registry, crontab, update, memory/engines/api.
 * Uses HU_IS_TEST paths (no real network/IO).
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>

#if defined(HU_HAS_SONATA)
#include "human/channel.h"
#include "human/channels/voice_channel.h"
#endif

#include "human/crontab.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/skill_registry.h"
#include "human/update.h"

/* ─── voice_channel (only when built with HU_HAS_SONATA) ──────────────────── */
#if defined(HU_HAS_SONATA)
static void test_voice_channel_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(&alloc, NULL, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "voice");
    hu_channel_voice_destroy(&ch);
}

static void test_voice_channel_create_null_alloc_fails(void) {
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(NULL, NULL, &ch);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_voice_channel_create_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_channel_voice_create(&alloc, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_voice_channel_create_with_config_applies_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_voice_config_t config = {0};
    config.speaker_id = "test-speaker";
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(&alloc, &config, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_voice_destroy(&ch);
}
#endif

/* ─── skill_registry ──────────────────────────────────────────────────────── */
static void test_skill_registry_search_null_alloc_fails(void) {
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(NULL, "query", &entries, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_skill_registry_search_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_search(&alloc, "query", NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_skill_registry_search_mock_returns_valid_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, "email", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    hu_skill_registry_entries_free(&alloc, entries, count);
}

/* ─── crontab ────────────────────────────────────────────────────────────── */
static void test_crontab_get_path_null_alloc_fails(void) {
    char *path = NULL;
    size_t path_len = 0;
    hu_error_t err = hu_crontab_get_path(NULL, &path, &path_len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_crontab_load_nonexistent_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_crontab_entry_t *entries = NULL;
    size_t count = 99;
    hu_error_t err = hu_crontab_load(&alloc, "/nonexistent/crontab.json", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_crontab_entries_free(&alloc, entries, count);
}

static void test_crontab_entries_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_crontab_entries_free(&alloc, NULL, 0);
    hu_crontab_entries_free(&alloc, NULL, 5);
}

/* ─── update ─────────────────────────────────────────────────────────────── */
static void test_update_check_null_buf_fails(void) {
    hu_error_t err = hu_update_check(NULL, 64);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_update_check_zero_size_fails(void) {
    char buf[64];
    hu_error_t err = hu_update_check(buf, 0);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_update_check_mock_returns_version(void) {
    char buf[64];
    hu_error_t err = hu_update_check(buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strlen(buf) > 0);
}

/* ─── memory/engines/api ────────────────────────────────────────────────── */
#if defined(HU_IS_TEST) && HU_IS_TEST
static void test_api_memory_create_null_alloc_returns_null(void) {
    hu_memory_t mem = hu_api_memory_create(NULL, "https://api.example.com", NULL, 5000);
    HU_ASSERT_NULL(mem.ctx);
    HU_ASSERT_NULL(mem.vtable);
}

static void test_api_memory_create_null_base_url_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_api_memory_create(&alloc, NULL, NULL, 5000);
    HU_ASSERT_NULL(mem.ctx);
    HU_ASSERT_NULL(mem.vtable);
}

static void test_api_memory_store_recall_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_api_memory_create(&alloc, "https://api.example.com", "key", 5000);
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "k1", 2, "content", 7, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *out = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "con", 3, 10, NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    if (out) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, count * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_untested_modules_tests(void) {
    HU_TEST_SUITE("Untested modules");

#if defined(HU_HAS_SONATA)
    HU_RUN_TEST(test_voice_channel_create_destroy);
    HU_RUN_TEST(test_voice_channel_create_null_alloc_fails);
    HU_RUN_TEST(test_voice_channel_create_null_out_fails);
    HU_RUN_TEST(test_voice_channel_create_with_config_applies_defaults);
#endif

    HU_RUN_TEST(test_skill_registry_search_null_alloc_fails);
    HU_RUN_TEST(test_skill_registry_search_null_out_fails);
    HU_RUN_TEST(test_skill_registry_search_mock_returns_valid_entries);

    HU_RUN_TEST(test_crontab_get_path_null_alloc_fails);
    HU_RUN_TEST(test_crontab_load_nonexistent_returns_empty);
    HU_RUN_TEST(test_crontab_entries_free_null_safe);

    HU_RUN_TEST(test_update_check_null_buf_fails);
    HU_RUN_TEST(test_update_check_zero_size_fails);
    HU_RUN_TEST(test_update_check_mock_returns_version);

#if defined(HU_IS_TEST) && HU_IS_TEST
    HU_RUN_TEST(test_api_memory_create_null_alloc_returns_null);
    HU_RUN_TEST(test_api_memory_create_null_base_url_returns_null);
    HU_RUN_TEST(test_api_memory_store_recall_mock);
#endif
}
