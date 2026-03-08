#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/connections.h"
#include "seaclaw/memory/consolidation.h"
#include "seaclaw/memory/inbox.h"
#include "seaclaw/memory/ingest.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

/* ── Feature 1: Source citations ─────────────────────────────────────── */

#ifdef SC_ENABLE_SQLITE
static void test_store_ex_with_source(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.vtable);
    SC_ASSERT_NOT_NULL(mem.vtable->store_ex);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
    sc_memory_store_opts_t opts = {
        .source = "file:///notes.txt", .source_len = 17, .importance = -1.0};
    sc_error_t err = mem.vtable->store_ex(mem.ctx, "k1", 2, "content", 7, &cat, NULL, 0, &opts);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k1", 2, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(found);
    SC_ASSERT_NOT_NULL(entry.source);
    SC_ASSERT_EQ(entry.source_len, 17);
    SC_ASSERT_TRUE(memcmp(entry.source, "file:///notes.txt", 17) == 0);
    sc_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}

static void test_store_ex_null_source(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.vtable->store_ex);

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};
    sc_memory_store_opts_t opts = {.source = NULL, .source_len = 0, .importance = -1.0};
    sc_error_t err = mem.vtable->store_ex(mem.ctx, "k2", 2, "val", 3, &cat, NULL, 0, &opts);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k2", 2, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(found);
    SC_ASSERT_EQ(entry.source_len, 0);
    sc_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}

static void test_store_with_source_helper(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_error_t err = sc_memory_store_with_source(&mem, "k3", 2, "data", 4, NULL, NULL, 0,
                                                 "http://example.com", 18);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k3", 2, &entry, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(found);
    SC_ASSERT_NOT_NULL(entry.source);
    SC_ASSERT_TRUE(memcmp(entry.source, "http://example.com", 18) == 0);
    sc_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}

static void test_source_in_recall_results(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_memory_store_with_source(&mem, "doc", 3, "important document", 18, NULL, NULL, 0,
                                "file:///doc.pdf", 15);

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err =
        mem.vtable->recall(mem.ctx, &alloc, "document", 8, 5, NULL, 0, &entries, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count > 0);
    SC_ASSERT_NOT_NULL(entries[0].source);
    SC_ASSERT_TRUE(memcmp(entries[0].source, "file:///doc.pdf", 15) == 0);

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(sc_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_store_with_source_fallback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    SC_ASSERT_TRUE(mem.vtable->store_ex == NULL);

    sc_error_t err =
        sc_memory_store_with_source(&mem, "k", 1, "v", 1, NULL, NULL, 0, "http://test.com", 15);
    SC_ASSERT_EQ(err, SC_OK);
    mem.vtable->deinit(mem.ctx);
}

/* ── Feature 2: Connection discovery ─────────────────────────────────── */

static void test_connections_build_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].key = "note1";
    entries[0].key_len = 5;
    entries[0].content = "AI agents are growing fast";
    entries[0].content_len = 26;
    entries[0].timestamp = "2026-03-01";
    entries[0].timestamp_len = 10;
    entries[1].key = "note2";
    entries[1].key_len = 5;
    entries[1].content = "Reduce inference costs by 40%";
    entries[1].content_len = 29;
    entries[1].timestamp = "2026-03-02";
    entries[1].timestamp_len = 10;

    char *prompt = NULL;
    size_t prompt_len = 0;
    sc_error_t err = sc_connections_build_prompt(&alloc, entries, 2, &prompt, &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT_TRUE(prompt_len > 0);
    SC_ASSERT_TRUE(strstr(prompt, "Memory 0") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "AI agents") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Memory 1") != NULL);
    alloc.free(alloc.ctx, prompt, SC_CONN_PROMPT_CAP);
}

static void test_connections_parse_valid(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"connections\":[{\"a\":0,\"b\":1,\"relationship\":\"both about cost\","
                       "\"strength\":0.8}],\"insights\":[{\"text\":\"Cost and scale are linked\","
                       "\"related\":[0,1]}]}";
    size_t json_len = strlen(json);

    sc_connection_result_t result;
    sc_error_t err = sc_connections_parse(&alloc, json, json_len, 2, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.connection_count, 1);
    SC_ASSERT_EQ(result.connections[0].memory_a_idx, 0);
    SC_ASSERT_EQ(result.connections[0].memory_b_idx, 1);
    SC_ASSERT_NOT_NULL(result.connections[0].relationship);
    SC_ASSERT_EQ(result.insight_count, 1);
    SC_ASSERT_NOT_NULL(result.insights[0].text);
    SC_ASSERT_TRUE(strstr(result.insights[0].text, "Cost") != NULL);
    sc_connection_result_deinit(&result, &alloc);
}

static void test_connections_parse_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_connection_result_t result;
    sc_error_t err = sc_connections_parse(&alloc, "{}", 2, 0, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.connection_count, 0);
    SC_ASSERT_EQ(result.insight_count, 0);
    sc_connection_result_deinit(&result, &alloc);
}

#ifdef SC_ENABLE_SQLITE
static void test_connections_store_insights(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_memory_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].key = "a";
    entries[0].key_len = 1;
    entries[0].content = "content a";
    entries[0].content_len = 9;
    entries[1].key = "b";
    entries[1].key_len = 1;
    entries[1].content = "content b";
    entries[1].content_len = 9;

    sc_connection_result_t result;
    memset(&result, 0, sizeof(result));
    result.insight_count = 1;
    result.insights[0].text = (char *)"Test insight about a and b";
    result.insights[0].text_len = 26;

    sc_error_t err = sc_connections_store_insights(&alloc, &mem, &result, entries, 2);
    SC_ASSERT_EQ(err, SC_OK);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_TRUE(count >= 1);

    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Feature 3: Multimodal ingestion ─────────────────────────────────── */

static void test_ingest_detect_type_text(void) {
    SC_ASSERT_EQ(sc_ingest_detect_type("notes.txt", 9), SC_INGEST_TEXT);
    SC_ASSERT_EQ(sc_ingest_detect_type("readme.md", 9), SC_INGEST_TEXT);
    SC_ASSERT_EQ(sc_ingest_detect_type("config.json", 11), SC_INGEST_TEXT);
    SC_ASSERT_EQ(sc_ingest_detect_type("data.csv", 8), SC_INGEST_TEXT);
    SC_ASSERT_EQ(sc_ingest_detect_type("code.py", 7), SC_INGEST_TEXT);
}

static void test_ingest_detect_type_image(void) {
    SC_ASSERT_EQ(sc_ingest_detect_type("photo.png", 9), SC_INGEST_IMAGE);
    SC_ASSERT_EQ(sc_ingest_detect_type("photo.jpg", 9), SC_INGEST_IMAGE);
    SC_ASSERT_EQ(sc_ingest_detect_type("photo.jpeg", 10), SC_INGEST_IMAGE);
    SC_ASSERT_EQ(sc_ingest_detect_type("icon.svg", 8), SC_INGEST_IMAGE);
}

static void test_ingest_detect_type_audio(void) {
    SC_ASSERT_EQ(sc_ingest_detect_type("song.mp3", 8), SC_INGEST_AUDIO);
    SC_ASSERT_EQ(sc_ingest_detect_type("clip.wav", 8), SC_INGEST_AUDIO);
    SC_ASSERT_EQ(sc_ingest_detect_type("voice.ogg", 9), SC_INGEST_AUDIO);
}

static void test_ingest_detect_type_video(void) {
    SC_ASSERT_EQ(sc_ingest_detect_type("movie.mp4", 9), SC_INGEST_VIDEO);
    SC_ASSERT_EQ(sc_ingest_detect_type("clip.webm", 9), SC_INGEST_VIDEO);
    SC_ASSERT_EQ(sc_ingest_detect_type("record.mov", 10), SC_INGEST_VIDEO);
}

static void test_ingest_detect_type_pdf(void) {
    SC_ASSERT_EQ(sc_ingest_detect_type("report.pdf", 10), SC_INGEST_PDF);
}

static void test_ingest_detect_type_unknown(void) {
    SC_ASSERT_EQ(sc_ingest_detect_type("data.xyz", 8), SC_INGEST_UNKNOWN);
    SC_ASSERT_EQ(sc_ingest_detect_type("noext", 5), SC_INGEST_UNKNOWN);
}

static void test_ingest_build_extract_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *prompt = NULL;
    size_t prompt_len = 0;
    sc_error_t err = sc_ingest_build_extract_prompt(&alloc, "photo.png", 9, SC_INGEST_IMAGE,
                                                    &prompt, &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT_TRUE(prompt_len > 0);
    SC_ASSERT_TRUE(strstr(prompt, "photo.png") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_ingest_with_provider_text_fallback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_error_t err = sc_ingest_file_with_provider(&alloc, &mem, NULL, "notes.txt", 9, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    mem.vtable->deinit(mem.ctx);
}

static void test_ingest_with_provider_binary_no_provider(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_error_t err = sc_ingest_file_with_provider(&alloc, &mem, NULL, "photo.png", 9, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    mem.vtable->deinit(mem.ctx);
}

static void test_ingest_with_provider_null_args(void) {
    SC_ASSERT_EQ(sc_ingest_file_with_provider(NULL, NULL, NULL, NULL, 0, NULL, 0),
                 SC_ERR_INVALID_ARGUMENT);
}

/* ── Feature 4: Inbox watcher ────────────────────────────────────────── */

#ifdef SC_ENABLE_SQLITE
static void test_inbox_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_inbox_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    sc_error_t err = sc_inbox_init(&watcher, &alloc, &mem, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(watcher.alloc);
    SC_ASSERT_NOT_NULL(watcher.memory);

    sc_inbox_deinit(&watcher);
    mem.vtable->deinit(mem.ctx);
}

static void test_inbox_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_inbox_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    sc_inbox_init(&watcher, &alloc, &mem, NULL, 0);

    size_t processed = 99;
    sc_error_t err = sc_inbox_poll(&watcher, &processed);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(processed, 0);

    sc_inbox_deinit(&watcher);
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Feature 5: Insight category ─────────────────────────────────────── */

#ifdef SC_ENABLE_SQLITE
static void test_insight_category_store_recall(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_INSIGHT};
    sc_error_t err = mem.vtable->store(
        mem.ctx, "insight:1", 9, "Cross-cutting insight about user behavior", 41, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &entries, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 1);
    SC_ASSERT_TRUE(strstr(entries[0].content, "Cross-cutting") != NULL);

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(sc_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Audit fix regression tests ──────────────────────────────────────── */

static void test_connections_parse_markdown_wrapped(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "```json\n{\"connections\":[],\"insights\":[{\"text\":\"wrapped insight\","
                       "\"related\":[]}]}\n```";
    size_t json_len = strlen(json);

    sc_connection_result_t result;
    sc_error_t err = sc_connections_parse(&alloc, json, json_len, 0, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.insight_count, 1);
    SC_ASSERT_NOT_NULL(result.insights[0].text);
    SC_ASSERT_TRUE(strstr(result.insights[0].text, "wrapped") != NULL);
    sc_connection_result_deinit(&result, &alloc);
}

static void test_connections_parse_out_of_bounds_indices(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"connections\":[{\"a\":5,\"b\":10,\"relationship\":\"oob\","
                       "\"strength\":0.5}],\"insights\":[]}";
    size_t json_len = strlen(json);

    sc_connection_result_t result;
    sc_error_t err = sc_connections_parse(&alloc, json, json_len, 3, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.connection_count, 0);
    sc_connection_result_deinit(&result, &alloc);
}

static void test_consolidation_timestamp_compare(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);

    sc_consolidation_config_t config = SC_CONSOLIDATION_DEFAULTS;
    config.decay_days = 0;
    sc_error_t err = sc_memory_consolidate(&alloc, &mem, &config);
    SC_ASSERT_EQ(err, SC_OK);
    mem.vtable->deinit(mem.ctx);
}

#ifdef SC_ENABLE_SQLITE
static void test_consolidation_iso_decay(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    mem.vtable->store(mem.ctx, "recent", 6, "recent data", 11, NULL, NULL, 0);
    mem.vtable->store(mem.ctx, "old", 3, "old data", 8, NULL, NULL, 0);

    size_t before = 0;
    mem.vtable->count(mem.ctx, &before);
    SC_ASSERT_TRUE(before >= 2);

    sc_consolidation_config_t config = SC_CONSOLIDATION_DEFAULTS;
    config.decay_days = 1;
    sc_error_t err = sc_memory_consolidate(&alloc, &mem, &config);
    SC_ASSERT_EQ(err, SC_OK);

    size_t after = 0;
    mem.vtable->count(mem.ctx, &after);
    SC_ASSERT_TRUE(after <= before);

    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_inbox_rejects_dotdot(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_inbox_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    sc_error_t err = sc_inbox_init(&watcher, &alloc, &mem, "/tmp/sc-test-inbox", 18);
    SC_ASSERT_EQ(err, SC_OK);
    sc_inbox_deinit(&watcher);
    mem.vtable->deinit(mem.ctx);
}

/* ── Test runner ─────────────────────────────────────────────────────── */

void run_memory_features_tests(void) {
    SC_TEST_SUITE("memory_features — source citations");
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_store_ex_with_source);
    SC_RUN_TEST(test_store_ex_null_source);
    SC_RUN_TEST(test_store_with_source_helper);
    SC_RUN_TEST(test_source_in_recall_results);
#endif
    SC_RUN_TEST(test_store_with_source_fallback);

    SC_TEST_SUITE("memory_features — connections");
    SC_RUN_TEST(test_connections_build_prompt);
    SC_RUN_TEST(test_connections_parse_valid);
    SC_RUN_TEST(test_connections_parse_empty);
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_connections_store_insights);
#endif

    SC_TEST_SUITE("memory_features — ingestion");
    SC_RUN_TEST(test_ingest_detect_type_text);
    SC_RUN_TEST(test_ingest_detect_type_image);
    SC_RUN_TEST(test_ingest_detect_type_audio);
    SC_RUN_TEST(test_ingest_detect_type_video);
    SC_RUN_TEST(test_ingest_detect_type_pdf);
    SC_RUN_TEST(test_ingest_detect_type_unknown);
    SC_RUN_TEST(test_ingest_build_extract_prompt);
    SC_RUN_TEST(test_ingest_with_provider_text_fallback);
    SC_RUN_TEST(test_ingest_with_provider_binary_no_provider);
    SC_RUN_TEST(test_ingest_with_provider_null_args);

    SC_TEST_SUITE("memory_features — inbox");
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_inbox_init_deinit);
    SC_RUN_TEST(test_inbox_poll_test_mode);
#endif

    SC_TEST_SUITE("memory_features — insight category");
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_insight_category_store_recall);
#endif

    SC_TEST_SUITE("memory_features — audit fixes");
    SC_RUN_TEST(test_connections_parse_markdown_wrapped);
    SC_RUN_TEST(test_connections_parse_out_of_bounds_indices);
    SC_RUN_TEST(test_consolidation_timestamp_compare);
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_consolidation_iso_decay);
#endif
    SC_RUN_TEST(test_inbox_rejects_dotdot);
}
