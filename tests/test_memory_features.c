#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/connections.h"
#include "human/memory/consolidation.h"
#include "human/memory/inbox.h"
#include "human/memory/ingest.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

/* ── Feature 1: Source citations ─────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
static void test_store_ex_with_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_NOT_NULL(mem.vtable->store_ex);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_memory_store_opts_t opts = {
        .source = "file:///notes.txt", .source_len = 17, .importance = -1.0};
    hu_error_t err = mem.vtable->store_ex(mem.ctx, "k1", 2, "content", 7, &cat, NULL, 0, &opts);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k1", 2, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_NOT_NULL(entry.source);
    HU_ASSERT_EQ(entry.source_len, 17);
    HU_ASSERT_TRUE(memcmp(entry.source, "file:///notes.txt", 17) == 0);
    hu_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}

static void test_store_ex_null_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable->store_ex);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_memory_store_opts_t opts = {.source = NULL, .source_len = 0, .importance = -1.0};
    hu_error_t err = mem.vtable->store_ex(mem.ctx, "k2", 2, "val", 3, &cat, NULL, 0, &opts);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k2", 2, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(entry.source_len, 0);
    hu_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}

static void test_store_with_source_helper(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_error_t err = hu_memory_store_with_source(&mem, "k3", 2, "data", 4, NULL, NULL, 0,
                                                 "http://example.com", 18);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "k3", 2, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_NOT_NULL(entry.source);
    HU_ASSERT_TRUE(memcmp(entry.source, "http://example.com", 18) == 0);
    hu_memory_entry_free_fields(&alloc, &entry);
    mem.vtable->deinit(mem.ctx);
}

static void test_source_in_recall_results(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_memory_store_with_source(&mem, "doc", 3, "important document", 18, NULL, NULL, 0,
                                "file:///doc.pdf", 15);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err =
        mem.vtable->recall(mem.ctx, &alloc, "document", 8, 5, NULL, 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count > 0);
    HU_ASSERT_NOT_NULL(entries[0].source);
    HU_ASSERT_TRUE(memcmp(entries[0].source, "file:///doc.pdf", 15) == 0);

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}
#endif

#ifdef HU_ENABLE_SQLITE
static void contact_memory_store_and_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    const char *contact_a = "user_a";
    size_t contact_a_len = 6;
    hu_error_t err = hu_memory_store_for_contact(&mem, contact_a, contact_a_len, "pref", 4,
                                                 "likes dark mode", 15, NULL, "", 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = hu_memory_recall_for_contact(&mem, &alloc, NULL, NULL, contact_a, contact_a_len, "dark",
                                       4, 5, "", 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(entries[0].content && memcmp(entries[0].content, "likes dark mode", 15) == 0);

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}

static void contact_memory_cross_contact_isolation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    const char *contact_a = "user_a";
    const char *contact_b = "user_b";
    size_t len_a = 6, len_b = 6;

    HU_ASSERT_EQ(hu_memory_store_for_contact(&mem, contact_a, len_a, "key_a", 5,
                                             "alpha likes coffee", 18, NULL, "", 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_memory_store_for_contact(&mem, contact_b, len_b, "key_b", 5,
                                             "bravo prefers tea", 17, NULL, "", 0),
                 HU_OK);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_memory_recall_for_contact(&mem, &alloc, NULL, NULL, contact_a, len_a,
                                                  "coffee", 6, 5, "", 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(memcmp(entries[0].content, "alpha likes coffee", 18) == 0);
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));

    entries = NULL;
    count = 0;
    err = hu_memory_recall_for_contact(&mem, &alloc, NULL, NULL, contact_a, len_a, "tea", 3, 5, "",
                                       0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    if (entries) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &entries[i]);
        alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

/* US-3.1: when both embedder AND vector_store are provided, the contact recall
 * routes through hu_hybrid_retrieve. The contact-prefix filter must still apply
 * (entries from other contacts must be excluded). */
static void contact_memory_recall_routes_through_hybrid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_embedder_t embedder = hu_embedder_local_create(&alloc);
    hu_vector_store_t vstore = hu_vector_store_mem_create(&alloc);
    HU_ASSERT_NOT_NULL(embedder.vtable);
    HU_ASSERT_NOT_NULL(vstore.vtable);

    /* Store memories for two contacts; index each under its prefixed storage key. */
    const char *contact_a = "alice";
    size_t len_a = 5;
    const char *contact_b = "bob";
    size_t len_b = 3;
    HU_ASSERT_EQ(hu_memory_store_for_contact(&mem, contact_a, len_a, "pref", 4,
                                             "alice loves espresso", 20, NULL, "", 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_memory_store_for_contact(&mem, contact_b, len_b, "pref", 4,
                                             "bob prefers matcha tea", 22, NULL, "", 0),
                 HU_OK);

    /* Index the same content in the vector store under the SAME prefixed keys. */
    const char *alice_key = "contact:alice:pref";
    size_t alice_key_len = strlen(alice_key);
    const char *bob_key = "contact:bob:pref";
    size_t bob_key_len = strlen(bob_key);
    const char *alice_content = "alice loves espresso";
    size_t alice_content_len = strlen(alice_content);
    const char *bob_content = "bob prefers matcha tea";
    size_t bob_content_len = strlen(bob_content);

    hu_embedding_t emb_a = {0}, emb_b = {0};
    HU_ASSERT_EQ(
        embedder.vtable->embed(embedder.ctx, &alloc, alice_content, alice_content_len, &emb_a),
        HU_OK);
    HU_ASSERT_EQ(embedder.vtable->embed(embedder.ctx, &alloc, bob_content, bob_content_len, &emb_b),
                 HU_OK);
    HU_ASSERT_EQ(vstore.vtable->insert(vstore.ctx, &alloc, alice_key, alice_key_len, &emb_a,
                                       alice_content, alice_content_len),
                 HU_OK);
    HU_ASSERT_EQ(vstore.vtable->insert(vstore.ctx, &alloc, bob_key, bob_key_len, &emb_b,
                                       bob_content, bob_content_len),
                 HU_OK);
    hu_embedding_free(&alloc, &emb_a);
    hu_embedding_free(&alloc, &emb_b);

    /* Query for alice's preference using the hybrid path. The contact-prefix filter
     * must drop bob's entry even though it could surface from the semantic search. */
    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_memory_recall_for_contact(&mem, &alloc, &embedder, &vstore, contact_a,
                                                  len_a, "espresso", 8, 5, "", 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    for (size_t i = 0; i < count; i++) {
        /* Every returned entry must be in alice's namespace. */
        HU_ASSERT_TRUE(entries[i].key && entries[i].key_len >= alice_key_len);
        HU_ASSERT_TRUE(memcmp(entries[i].key, "contact:alice:", 14) == 0);
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    }
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));

    if (embedder.vtable && embedder.vtable->deinit)
        embedder.vtable->deinit(embedder.ctx, &alloc);
    if (vstore.vtable && vstore.vtable->deinit)
        vstore.vtable->deinit(vstore.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* US-3.1 AC-3.1.5 error propagation: when the hybrid path is engaged
 * (embedder + vector_store both non-NULL) and the embedder fails (here
 * by returning HU_ERR_NOT_SUPPORTED from `embed`), `hu_memory_recall_for_contact`
 * must propagate that error to the caller — NOT silently fall back to keyword
 * mode and return HU_OK. */
static hu_error_t stub_embed_returns_not_supported(void *ctx, hu_allocator_t *alloc,
                                                   const char *text, size_t text_len,
                                                   hu_embedding_t *out) {
    (void)ctx;
    (void)alloc;
    (void)text;
    (void)text_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

static size_t stub_embed_dimensions(void *ctx) {
    (void)ctx;
    return HU_EMBEDDING_DIM;
}

static void stub_embed_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static void contact_memory_recall_propagates_embedder_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    /* Seed at least one memory so the keyword side has a row to return — the
     * failure must come from the embedder, not from an empty backend. */
    HU_ASSERT_EQ(hu_memory_store_for_contact(&mem, "alice", 5, "pref", 4, "alice loves espresso",
                                             20, NULL, "", 0),
                 HU_OK);

    /* Stubbed embedder whose `embed` vtable returns HU_ERR_NOT_SUPPORTED. */
    static const hu_embedder_vtable_t stub_vtable = {
        .embed = stub_embed_returns_not_supported,
        .embed_batch = NULL,
        .dimensions = stub_embed_dimensions,
        .deinit = stub_embed_deinit,
    };
    hu_embedder_t failing_embedder = {.ctx = NULL, .vtable = &stub_vtable};
    hu_vector_store_t vstore = hu_vector_store_mem_create(&alloc);
    HU_ASSERT_NOT_NULL(vstore.vtable);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_memory_recall_for_contact(&mem, &alloc, &failing_embedder, &vstore, "alice",
                                                  5, "espresso", 8, 5, "", 0, &entries, &count);
    /* The embedder failure must surface — recall MUST return HU_ERR_NOT_SUPPORTED
     * (not HU_OK with a silent fallback) and MUST NOT leak partial results. */
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(entries);

    if (vstore.vtable && vstore.vtable->deinit)
        vstore.vtable->deinit(vstore.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_store_with_source_fallback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_TRUE(mem.vtable->store_ex == NULL);

    hu_error_t err =
        hu_memory_store_with_source(&mem, "k", 1, "v", 1, NULL, NULL, 0, "http://test.com", 15);
    HU_ASSERT_EQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

/* ── Feature 2: Connection discovery ─────────────────────────────────── */

static void test_connections_build_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_entry_t entries[2];
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
    hu_error_t err = hu_connections_build_prompt(&alloc, entries, 2, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "Memory 0") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "AI agents") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Memory 1") != NULL);
    alloc.free(alloc.ctx, prompt, HU_CONN_PROMPT_CAP);
}

static void test_connections_parse_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"connections\":[{\"a\":0,\"b\":1,\"relationship\":\"both about cost\","
                       "\"strength\":0.8}],\"insights\":[{\"text\":\"Cost and scale are linked\","
                       "\"related\":[0,1]}]}";
    size_t json_len = strlen(json);

    hu_connection_result_t result;
    hu_error_t err = hu_connections_parse(&alloc, json, json_len, 2, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.connection_count, 1);
    HU_ASSERT_EQ(result.connections[0].memory_a_idx, 0);
    HU_ASSERT_EQ(result.connections[0].memory_b_idx, 1);
    HU_ASSERT_NOT_NULL(result.connections[0].relationship);
    HU_ASSERT_EQ(result.insight_count, 1);
    HU_ASSERT_NOT_NULL(result.insights[0].text);
    HU_ASSERT_TRUE(strstr(result.insights[0].text, "Cost") != NULL);
    hu_connection_result_deinit(&result, &alloc);
}

static void test_connections_parse_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_connection_result_t result;
    hu_error_t err = hu_connections_parse(&alloc, "{}", 2, 0, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.connection_count, 0);
    HU_ASSERT_EQ(result.insight_count, 0);
    hu_connection_result_deinit(&result, &alloc);
}

#ifdef HU_ENABLE_SQLITE
static void test_connections_store_insights(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_memory_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].key = "a";
    entries[0].key_len = 1;
    entries[0].content = "content a";
    entries[0].content_len = 9;
    entries[1].key = "b";
    entries[1].key_len = 1;
    entries[1].content = "content b";
    entries[1].content_len = 9;

    hu_connection_result_t result;
    memset(&result, 0, sizeof(result));
    result.insight_count = 1;
    result.insights[0].text = (char *)"Test insight about a and b";
    result.insights[0].text_len = 26;

    hu_error_t err = hu_connections_store_insights(&alloc, &mem, &result, entries, 2);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_TRUE(count >= 1);

    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Feature 3: Multimodal ingestion ─────────────────────────────────── */

static void test_ingest_detect_type_text(void) {
    HU_ASSERT_EQ(hu_ingest_detect_type("notes.txt", 9), HU_INGEST_TEXT);
    HU_ASSERT_EQ(hu_ingest_detect_type("readme.md", 9), HU_INGEST_TEXT);
    HU_ASSERT_EQ(hu_ingest_detect_type("config.json", 11), HU_INGEST_TEXT);
    HU_ASSERT_EQ(hu_ingest_detect_type("data.csv", 8), HU_INGEST_TEXT);
    HU_ASSERT_EQ(hu_ingest_detect_type("code.py", 7), HU_INGEST_TEXT);
}

static void test_ingest_detect_type_image(void) {
    HU_ASSERT_EQ(hu_ingest_detect_type("photo.png", 9), HU_INGEST_IMAGE);
    HU_ASSERT_EQ(hu_ingest_detect_type("photo.jpg", 9), HU_INGEST_IMAGE);
    HU_ASSERT_EQ(hu_ingest_detect_type("photo.jpeg", 10), HU_INGEST_IMAGE);
    HU_ASSERT_EQ(hu_ingest_detect_type("icon.svg", 8), HU_INGEST_IMAGE);
}

static void test_ingest_detect_type_audio(void) {
    HU_ASSERT_EQ(hu_ingest_detect_type("song.mp3", 8), HU_INGEST_AUDIO);
    HU_ASSERT_EQ(hu_ingest_detect_type("clip.wav", 8), HU_INGEST_AUDIO);
    HU_ASSERT_EQ(hu_ingest_detect_type("voice.ogg", 9), HU_INGEST_AUDIO);
}

static void test_ingest_detect_type_video(void) {
    HU_ASSERT_EQ(hu_ingest_detect_type("movie.mp4", 9), HU_INGEST_VIDEO);
    HU_ASSERT_EQ(hu_ingest_detect_type("clip.webm", 9), HU_INGEST_VIDEO);
    HU_ASSERT_EQ(hu_ingest_detect_type("record.mov", 10), HU_INGEST_VIDEO);
}

static void test_ingest_detect_type_pdf(void) {
    HU_ASSERT_EQ(hu_ingest_detect_type("report.pdf", 10), HU_INGEST_PDF);
}

static void test_ingest_detect_type_unknown(void) {
    HU_ASSERT_EQ(hu_ingest_detect_type("data.xyz", 8), HU_INGEST_UNKNOWN);
    HU_ASSERT_EQ(hu_ingest_detect_type("noext", 5), HU_INGEST_UNKNOWN);
}

static void test_ingest_build_extract_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err = hu_ingest_build_extract_prompt(&alloc, "photo.png", 9, HU_INGEST_IMAGE,
                                                    &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "photo.png") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_ingest_with_provider_text_fallback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_error_t err = hu_ingest_file_with_provider(&alloc, &mem, NULL, "notes.txt", 9, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    mem.vtable->deinit(mem.ctx);
}

static void test_ingest_with_provider_binary_no_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_error_t err = hu_ingest_file_with_provider(&alloc, &mem, NULL, "photo.png", 9, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    mem.vtable->deinit(mem.ctx);
}

static void test_ingest_with_provider_null_args(void) {
    HU_ASSERT_EQ(hu_ingest_file_with_provider(NULL, NULL, NULL, NULL, 0, NULL, 0),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Feature 4: Inbox watcher ────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
static void test_inbox_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_inbox_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    hu_error_t err = hu_inbox_init(&watcher, &alloc, &mem, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(watcher.alloc);
    HU_ASSERT_NOT_NULL(watcher.memory);

    hu_inbox_deinit(&watcher);
    mem.vtable->deinit(mem.ctx);
}

static void test_inbox_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_inbox_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    hu_inbox_init(&watcher, &alloc, &mem, NULL, 0);

    size_t processed = 99;
    hu_error_t err = hu_inbox_poll(&watcher, &processed);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(processed, 0);

    hu_inbox_deinit(&watcher);
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Feature 5: Insight category ─────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
static void test_insight_category_store_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_INSIGHT};
    hu_error_t err = mem.vtable->store(
        mem.ctx, "insight:1", 9, "Cross-cutting insight about user behavior", 41, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_TRUE(strstr(entries[0].content, "Cross-cutting") != NULL);

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Audit fix regression tests ──────────────────────────────────────── */

static void test_connections_parse_markdown_wrapped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "```json\n{\"connections\":[],\"insights\":[{\"text\":\"wrapped insight\","
                       "\"related\":[]}]}\n```";
    size_t json_len = strlen(json);

    hu_connection_result_t result;
    hu_error_t err = hu_connections_parse(&alloc, json, json_len, 0, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.insight_count, 1);
    HU_ASSERT_NOT_NULL(result.insights[0].text);
    HU_ASSERT_TRUE(strstr(result.insights[0].text, "wrapped") != NULL);
    hu_connection_result_deinit(&result, &alloc);
}

static void test_connections_parse_out_of_bounds_indices(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"connections\":[{\"a\":5,\"b\":10,\"relationship\":\"oob\","
                       "\"strength\":0.5}],\"insights\":[]}";
    size_t json_len = strlen(json);

    hu_connection_result_t result;
    hu_error_t err = hu_connections_parse(&alloc, json, json_len, 3, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.connection_count, 0);
    hu_connection_result_deinit(&result, &alloc);
}

static void test_consolidation_timestamp_compare(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.decay_days = 0;
    hu_error_t err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);
    mem.vtable->deinit(mem.ctx);
}

#ifdef HU_ENABLE_SQLITE
static void test_consolidation_iso_decay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    mem.vtable->store(mem.ctx, "recent", 6, "recent data", 11, NULL, NULL, 0);
    mem.vtable->store(mem.ctx, "old", 3, "old data", 8, NULL, NULL, 0);

    size_t before = 0;
    mem.vtable->count(mem.ctx, &before);
    HU_ASSERT_TRUE(before >= 2);

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.decay_days = 1;
    hu_error_t err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t after = 0;
    mem.vtable->count(mem.ctx, &after);
    HU_ASSERT_TRUE(after <= before);

    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_inbox_rejects_dotdot(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_inbox_watcher_t watcher;
    memset(&watcher, 0, sizeof(watcher));
    hu_error_t err = hu_inbox_init(&watcher, &alloc, &mem, "/tmp/sc-test-inbox", 18);
    HU_ASSERT_EQ(err, HU_OK);
    hu_inbox_deinit(&watcher);
    mem.vtable->deinit(mem.ctx);
}

#ifdef HU_ENABLE_SQLITE
static void test_consolidation_dedup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    mem.vtable->store(mem.ctx, "a", 1, "the quick brown fox jumps over the lazy dog", 44, NULL,
                      NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "the quick brown fox jumps over the lazy dog", 44, NULL,
                      NULL, 0);
    mem.vtable->store(mem.ctx, "c", 1, "something completely different and unique", 40, NULL, NULL,
                      0);

    size_t before = 0;
    mem.vtable->count(mem.ctx, &before);
    HU_ASSERT_EQ(before, 3);

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.decay_days = 0;
    config.dedup_threshold = 80;
    hu_error_t err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t after = 0;
    mem.vtable->count(mem.ctx, &after);
    HU_ASSERT_TRUE(after < before);
    HU_ASSERT_TRUE(after >= 2);

    mem.vtable->deinit(mem.ctx);
}

static void test_consolidation_max_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    for (int i = 0; i < 10; i++) {
        char key[16];
        char content[64];
        snprintf(key, sizeof(key), "k%d", i);
        snprintf(content, sizeof(content), "unique content number %d for testing", i);
        mem.vtable->store(mem.ctx, key, strlen(key), content, strlen(content), NULL, NULL, 0);
    }

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.decay_days = 0;
    config.max_entries = 5;
    hu_error_t err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t after = 0;
    mem.vtable->count(mem.ctx, &after);
    HU_ASSERT_TRUE(after <= 5);

    mem.vtable->deinit(mem.ctx);
}

static void test_connection_pipeline_end_to_end(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_memory_entry_t entries[3];
    memset(entries, 0, sizeof(entries));
    entries[0].key = "project-deadline";
    entries[0].key_len = 16;
    entries[0].content = "Main project deadline is end of March";
    entries[0].content_len = 37;
    entries[1].key = "meeting-notes";
    entries[1].key_len = 13;
    entries[1].content = "Team wants to ship before March deadline";
    entries[1].content_len = 40;
    entries[2].key = "user-preference";
    entries[2].key_len = 15;
    entries[2].content = "User prefers dark mode for late-night work";
    entries[2].content_len = 43;

    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err = hu_connections_build_prompt(&alloc, entries, 3, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strstr(prompt, "Memory 0") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Memory 2") != NULL);
    alloc.free(alloc.ctx, prompt, HU_CONN_PROMPT_CAP);

    const char *mock_response =
        "{\"connections\":[{\"a\":0,\"b\":1,\"relationship\":\"both about March deadline\","
        "\"strength\":0.9}],\"insights\":[{\"text\":\"Project deadline and team shipping "
        "goal are tightly coupled\",\"related\":[0,1]}]}";
    hu_connection_result_t result;
    err = hu_connections_parse(&alloc, mock_response, strlen(mock_response), 3, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.connection_count, 1);
    HU_ASSERT_EQ(result.insight_count, 1);

    err = hu_connections_store_insights(&alloc, &mem, &result, entries, 3);
    HU_ASSERT_EQ(err, HU_OK);
    hu_connection_result_deinit(&result, &alloc);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_TRUE(count >= 1);

    hu_memory_entry_t *listed = NULL;
    size_t listed_count = 0;
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_INSIGHT};
    err = mem.vtable->list(mem.ctx, &alloc, &cat, NULL, 0, &listed, &listed_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(listed_count, 1);
    HU_ASSERT_TRUE(strstr(listed[0].content, "tightly coupled") != NULL);
    HU_ASSERT_NOT_NULL(listed[0].source);
    HU_ASSERT_TRUE(memcmp(listed[0].source, "connection_discovery", 20) == 0);

    for (size_t i = 0; i < listed_count; i++)
        hu_memory_entry_free_fields(&alloc, &listed[i]);
    alloc.free(alloc.ctx, listed, listed_count * sizeof(hu_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}

static void test_ingest_file_with_provider_unknown_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_error_t err = hu_ingest_file_with_provider(&alloc, &mem, NULL, "data.xyz", 8, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_similarity_score_basics(void) {
    HU_ASSERT_EQ(hu_similarity_score(NULL, 0, NULL, 0), 0);
    HU_ASSERT_EQ(hu_similarity_score("", 0, "", 0), 100);
    HU_ASSERT_EQ(hu_similarity_score("hello world", 11, "hello world", 11), 100);
    uint32_t partial = hu_similarity_score("hello world", 11, "hello there", 11);
    HU_ASSERT_TRUE(partial > 0 && partial < 100);
    HU_ASSERT_EQ(hu_similarity_score("abc", 3, "xyz", 3), 0);
}

/* ── Test runner ─────────────────────────────────────────────────────── */

void run_memory_features_tests(void) {
    HU_TEST_SUITE("memory_features — contact-scoped memory");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(contact_memory_store_and_recall);
    HU_RUN_TEST(contact_memory_cross_contact_isolation);
    HU_RUN_TEST(contact_memory_recall_routes_through_hybrid);
    HU_RUN_TEST(contact_memory_recall_propagates_embedder_error);
#endif

    HU_TEST_SUITE("memory_features — source citations");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_store_ex_with_source);
    HU_RUN_TEST(test_store_ex_null_source);
    HU_RUN_TEST(test_store_with_source_helper);
    HU_RUN_TEST(test_source_in_recall_results);
#endif
    HU_RUN_TEST(test_store_with_source_fallback);

    HU_TEST_SUITE("memory_features — connections");
    HU_RUN_TEST(test_connections_build_prompt);
    HU_RUN_TEST(test_connections_parse_valid);
    HU_RUN_TEST(test_connections_parse_empty);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_connections_store_insights);
#endif

    HU_TEST_SUITE("memory_features — ingestion");
    HU_RUN_TEST(test_ingest_detect_type_text);
    HU_RUN_TEST(test_ingest_detect_type_image);
    HU_RUN_TEST(test_ingest_detect_type_audio);
    HU_RUN_TEST(test_ingest_detect_type_video);
    HU_RUN_TEST(test_ingest_detect_type_pdf);
    HU_RUN_TEST(test_ingest_detect_type_unknown);
    HU_RUN_TEST(test_ingest_build_extract_prompt);
    HU_RUN_TEST(test_ingest_with_provider_text_fallback);
    HU_RUN_TEST(test_ingest_with_provider_binary_no_provider);
    HU_RUN_TEST(test_ingest_with_provider_null_args);

    HU_TEST_SUITE("memory_features — inbox");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_inbox_init_deinit);
    HU_RUN_TEST(test_inbox_poll_test_mode);
#endif

    HU_TEST_SUITE("memory_features — insight category");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_insight_category_store_recall);
#endif

    HU_TEST_SUITE("memory_features — audit fixes");
    HU_RUN_TEST(test_connections_parse_markdown_wrapped);
    HU_RUN_TEST(test_connections_parse_out_of_bounds_indices);
    HU_RUN_TEST(test_consolidation_timestamp_compare);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_consolidation_iso_decay);
#endif
    HU_RUN_TEST(test_inbox_rejects_dotdot);

    HU_TEST_SUITE("memory_features — integration hardening");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_consolidation_dedup);
    HU_RUN_TEST(test_consolidation_max_entries);
    HU_RUN_TEST(test_connection_pipeline_end_to_end);
    HU_RUN_TEST(test_ingest_file_with_provider_unknown_type);
#endif
    HU_RUN_TEST(test_similarity_score_basics);
}
