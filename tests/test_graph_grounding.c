#include "human/agent/autodream.h"
#include "human/agent/graph_grounding.h"
#include "human/agent/scheduler.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Pure retrieval-scoring predicates (no DB) ──────────────────────────── */

/* Word-boundary matching pins the substring-classifier-pitfalls contract:
 * an entity named "formal" must NOT be selected by the message "that was
 * so informal" — the exact opposite-intent overlap class that rule
 * documents. */
static void test_ground_match_count_respects_word_boundaries(void) {
    const char *msg1 = "that was so informal lol";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg1, strlen(msg1), "formal", 6), 0);
    const char *msg2 = "lukewarm at best";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg2, strlen(msg2), "warm", 4), 0);
    const char *msg3 = "such warm friend energy";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg3, strlen(msg3), "warm", 4), 1);
    /* Case-insensitive across boundaries, multi-word entity names count
     * per-word: "Alice" matches one of the two words of "Alice Friend". */
    const char *msg4 = "did ALICE text you back";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg4, strlen(msg4), "Alice Friend", 12),
                 1);
}

static void test_ground_name_word_count_skips_stopwords_and_short_words(void) {
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count("the marina", 10), 1);
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count("Bo", 2), 0);
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count(NULL, 0), 0);
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count("Alice Friend", 12), 2);
}

static void test_ground_score_zero_without_match_and_coverage_dominates(void) {
    /* No lexical overlap -> 0.0: empty injection is the VALID outcome for an
     * irrelevant message (never fall back to generic filler). */
    HU_ASSERT_TRUE(hu_graph_ground_score(0, 2, 100, 1000, 2000) == 0.0);
    HU_ASSERT_TRUE(hu_graph_ground_score(1, 0, 100, 1000, 2000) == 0.0);
    /* Full-name coverage outranks half coverage at identical stats. */
    double full = hu_graph_ground_score(2, 2, 5, 1000, 2000);
    double half = hu_graph_ground_score(1, 2, 5, 1000, 2000);
    HU_ASSERT_TRUE(full > half);
    /* Mention + recency boosts are bounded: a half-coverage entity with
     * maxed boosts cannot outrank a full-coverage entity with the same
     * boosts (coverage is the dominant term). */
    double half_boosted = hu_graph_ground_score(1, 2, 1000000, 2000, 2000);
    double full_plain = hu_graph_ground_score(2, 2, 1000000, 2000, 2000);
    HU_ASSERT_TRUE(full_plain > half_boosted);
}

static void test_ground_fingerprint_varies_with_content(void) {
    const char *a = "- sailboat (topic)";
    const char *b = "- guitar (topic)";
    HU_ASSERT_TRUE(hu_graph_ground_fingerprint(a, strlen(a)) !=
                   hu_graph_ground_fingerprint(b, strlen(b)));
    HU_ASSERT_EQ((int)hu_graph_ground_fingerprint(NULL, 0), 0);
    HU_ASSERT_EQ((int)hu_graph_ground_fingerprint(a, 0), 0);
}

/* ── Query-conditioned composition (SQLite graph store) ─────────────────── */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void seed_run(sqlite3 *db, const char *sql) {
    char *emsg = NULL;
    (void)sqlite3_exec(db, sql, NULL, NULL, &emsg);
    if (emsg)
        sqlite3_free(emsg);
}

/* Two DISJOINT entity clusters for "alice" so different conversations
 * compose different context: (sailboat -> marina) and (guitar -> teacher).
 * Plus a "formal" entity to pin the word-boundary contract at compose
 * level, and a "bob"-scoped entity that must never leak into alice's
 * context. */
static void seed_alice_graph(hu_graph_t *graph) {
    int64_t boat = 0, marina = 0, guitar = 0, teacher = 0, formal = 0, bobs = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "sailboat", 8, HU_ENTITY_TOPIC, NULL, &boat),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "marina", 6, HU_ENTITY_PLACE, NULL, &marina),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "guitar", 6, HU_ENTITY_TOPIC, NULL, &guitar),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "teacher", 7, HU_ENTITY_PERSON, NULL, &teacher),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "formal", 6, HU_ENTITY_TOPIC, NULL, &formal),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "bob", 3, "sailboat", 8, HU_ENTITY_TOPIC, NULL, &bobs),
        HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(graph, "alice", 5, boat, marina, HU_REL_RELATED_TO, 1.0f,
                                          "docked at slip 14 since spring", 30),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(graph, "alice", 5, guitar, teacher, HU_REL_RELATED_TO,
                                          1.0f, "fingerstyle lessons every tuesday", 33),
                 HU_OK);
}

typedef struct gg_fixture {
    hu_allocator_t alloc;
    hu_graph_t *graph;
    hu_w7_facade_t *facade;
    hu_memory_loader_t loader;
} gg_fixture_t;

static void gg_fixture_open(gg_fixture_t *fx) {
    fx->alloc = hu_system_allocator();
    fx->graph = NULL;
    HU_ASSERT_EQ(hu_graph_open(&fx->alloc, ":memory:", strlen(":memory:"), &fx->graph), HU_OK);
    seed_alice_graph(fx->graph);
    fx->facade = NULL;
    HU_ASSERT_EQ(hu_w7_facade_open(fx->graph, &fx->alloc, &fx->facade), HU_OK);
    hu_memory_loader_init(&fx->loader, &fx->alloc, NULL, NULL, 10, 4000);
    hu_memory_loader_set_facade(&fx->loader, fx->facade);
}

static void gg_fixture_close(gg_fixture_t *fx) {
    hu_w7_facade_close(fx->facade, &fx->alloc);
    hu_graph_close(fx->graph, &fx->alloc);
}

/* Relevant entity in the message -> that entity's graph content (its name,
 * its 1-hop relation, and the relation's context text) is selected. */
static void test_compose_selects_relevant_entity_content(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(matched >= 1);
    HU_ASSERT_TRUE(strstr(out, "sailboat") != NULL);
    HU_ASSERT_TRUE(strstr(out, "docked at slip 14") != NULL); /* relation context text */
    HU_ASSERT_TRUE(strstr(out, "guitar") == NULL);            /* disjoint cluster stays out */
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Irrelevant message -> EMPTY injection. Empty is VALID and better than
 * the pre-2026-07 behavior (same generic community summaries every turn). */
static void test_compose_irrelevant_message_returns_empty(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "wanna grab tacos tonight";
    char *out = (char *)0x1;
    size_t out_len = 99, matched = 99;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    HU_ASSERT_EQ((int)matched, 0);
    gg_fixture_close(&fx);
}

/* Compose-level pin of the word-boundary contract: entity "formal" exists,
 * but the message "that was so informal" must not seed it. */
static void test_compose_word_boundary_prevents_false_seed(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "that was so informal";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)matched, 0);
    gg_fixture_close(&fx);
}

/* max_chars is a hard cap on the composed block (the block additionally
 * participates in the HU_PROMPT_TRIM graph span downstream). */
static void test_compose_respects_budget_cap(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 48, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(out_len <= 48);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* DONE-(b) synthetic multi-conversation demo: two different incoming
 * messages against the SAME contact graph compose DIFFERENT content with
 * DIFFERENT relevance fingerprints — the measurable inverse of the old
 * failure signature (274 shadow events collapsing to 5 distinct sizes of
 * identical generic summaries). */
static void test_compose_varies_with_conversation(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg_a = "hows the sailboat coming along";
    const char *msg_b = "hows guitar practice going";
    char *out_a = NULL, *out_b = NULL;
    size_t len_a = 0, len_b = 0, matched_a = 0, matched_b = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg_a, strlen(msg_a), 0, &out_a,
                                         &len_a, &matched_a),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg_b, strlen(msg_b), 0, &out_b,
                                         &len_b, &matched_b),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out_a);
    HU_ASSERT_NOT_NULL(out_b);
    HU_ASSERT_TRUE(matched_a >= 1);
    HU_ASSERT_TRUE(matched_b >= 1);
    HU_ASSERT_TRUE(strstr(out_a, "sailboat") != NULL);
    HU_ASSERT_TRUE(strstr(out_a, "guitar") == NULL);
    HU_ASSERT_TRUE(strstr(out_b, "guitar") != NULL);
    HU_ASSERT_TRUE(strstr(out_b, "sailboat") == NULL);
    HU_ASSERT_TRUE(strcmp(out_a, out_b) != 0);
    HU_ASSERT_TRUE(hu_graph_ground_fingerprint(out_a, len_a) !=
                   hu_graph_ground_fingerprint(out_b, len_b));
    fx.alloc.free(fx.alloc.ctx, out_a, len_a + 1);
    fx.alloc.free(fx.alloc.ctx, out_b, len_b + 1);
    gg_fixture_close(&fx);
}

/* Contact scoping: bob's graph knows "sailboat" too, but bob has no
 * relations and alice's rows must not leak into bob's composition. */
static void test_compose_scopes_to_contact(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "bob", 3, msg, strlen(msg), 0, &out, &out_len,
                                         &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out); /* bob's own sailboat entity matches... */
    HU_ASSERT_TRUE(strstr(out, "docked at slip 14") == NULL); /* ...alice's relation doesn't */
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Fail-open: loader without a facade (no graph wired) -> empty, HU_OK. */
static void test_compose_no_graph_is_failopen(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_loader_t loader;
    hu_memory_loader_init(&loader, &alloc, NULL, NULL, 10, 4000);
    const char *msg = "hows the sailboat";
    char *out = (char *)0x1;
    size_t out_len = 99, matched = 99;
    HU_ASSERT_EQ(
        hu_graph_ground_compose(&loader, "alice", 5, msg, strlen(msg), 0, &out, &out_len, &matched),
        HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    HU_ASSERT_EQ((int)matched, 0);
}
#endif

static void test_graph_grounding_mode_parse(void) {
    /* Default SHADOW as of 2026-05-31: the grounding A/B (n=30, ON-win-rate 43.3%,
     * CI [27.4,60.8]) did NOT substantiate injection, so unset => SHADOW until a
     * measurement clears 50%. See src/agent/graph_grounding.c +
     * docs/research/2026-05-31-graphrag-grounding-ab.md. The 2026-07 query-
     * conditioned read-path rebuild deliberately did NOT change gate semantics:
     * promotion past shadow stays human-gated on a fresh blind A/B. */
    unsetenv("HU_GRAPH_GROUNDING");
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_SHADOW);
    setenv("HU_GRAPH_GROUNDING", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_SHADOW);
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_ON);
    /* Explicit off (disable) override. */
    setenv("HU_GRAPH_GROUNDING", "off", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    /* Unknown values disable (fail-safe to off, not silently on). */
    setenv("HU_GRAPH_GROUNDING", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    unsetenv("HU_GRAPH_GROUNDING");
}

#ifdef HU_ENABLE_SQLITE

/* AC-1.1: Verify that autodream writes community_summaries after a runner invocation */
static void test_autodream_tick_populates_community_summaries_for_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *graph = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, ":memory:", strlen(":memory:"), &graph), HU_OK);

    sqlite3 *gdb = hu_graph_sqlite_connection(graph);

    /* Seed entities table with contact_id and community_id columns,
     * and relationships to give the community real structure */
    const char *create_entities_sql = "CREATE TABLE IF NOT EXISTS entities ("
                                      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                      "  name TEXT NOT NULL,"
                                      "  entity_type TEXT NOT NULL DEFAULT 'unknown',"
                                      "  contact_id TEXT,"
                                      "  community_id INTEGER,"
                                      "  mention_count INTEGER DEFAULT 1)";
    seed_run(gdb, create_entities_sql);

    const char *create_relationships_sql = "CREATE TABLE IF NOT EXISTS relations ("
                                           "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                           "  source_id INTEGER,"
                                           "  target_id INTEGER,"
                                           "  contact_id TEXT,"
                                           "  relationship_type TEXT DEFAULT 'knows',"
                                           "  weight REAL DEFAULT 1.0,"
                                           "  context TEXT,"
                                           "  event_start INTEGER,"
                                           "  event_end INTEGER DEFAULT 0,"
                                           "  last_seen INTEGER)";
    seed_run(gdb, create_relationships_sql);

    /* Seed entities for a test contact with community_id set (this is what
     * the community summarizer reads to generate summaries) */
    const char *seed_entities_sql =
        "INSERT INTO entities (name, entity_type, contact_id, community_id, mention_count) VALUES"
        "  ('Alice Friend', 'person', 'TestContact', 1, 5),"
        "  ('Bob Colleague', 'person', 'TestContact', 1, 3),"
        "  ('Carol Neighbor', 'person', 'TestContact', 1, 2),"
        "  ('David Other', 'person', 'TestContact', 2, 4),"
        "  ('Eve Another', 'person', 'TestContact', 2, 2)";
    seed_run(gdb, seed_entities_sql);

    /* Seed relationships between entities in the same community so the
     * summarizer has graph structure to count. Relations join with entities
     * via source_id = e.id to count live edges per community. */
    const char *seed_relations_sql = "INSERT INTO relations (source_id, target_id, contact_id, "
                                     "context, event_start, event_end) VALUES"
                                     "  (1, 2, 'TestContact', 'work_together', 1000, 0),"
                                     "  (2, 3, 'TestContact', 'neighborhood', 2000, 0),"
                                     "  (4, 5, 'TestContact', 'friends_group', 3000, 0)";
    seed_run(gdb, seed_relations_sql);

    /* Assert the table is empty before the runner.
     * The summarizer's ensure_autodream_schema will create community_summaries on first call. */
    sqlite3_stmt *pre_check = NULL;
    const char *count_sql =
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='community_summaries'";
    HU_ASSERT_EQ(sqlite3_prepare_v2(gdb, count_sql, -1, &pre_check, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(pre_check), SQLITE_ROW);
    int table_exists_pre = sqlite3_column_int(pre_check, 0);
    sqlite3_finalize(pre_check);
    /* Table should not exist yet before calling the summarizer */
    HU_ASSERT_EQ(table_exists_pre, 0);

    /* Invoke the real production summarizer for community 1 */
    hu_error_t runner_result = hu_autodream_summarize_community(
        &alloc, graph, "TestContact", strlen("TestContact"), 1, (int64_t)time(NULL) * 1000);
    HU_ASSERT_EQ(runner_result, HU_OK);

    /* After runner completes, query the table for inserted rows */
    const char *count_summaries_sql =
        "SELECT COUNT(*) FROM community_summaries WHERE contact_id = 'TestContact'";
    sqlite3_stmt *post_check = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(gdb, count_summaries_sql, -1, &post_check, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(post_check), SQLITE_ROW);
    int post_count = sqlite3_column_int(post_check, 0);
    sqlite3_finalize(post_check);

    /* AC-1.1 contract: after autodream summarizer invocation, table MUST have >= 1 row
     * for the contact. The runner was called with real seeded entities and community_id,
     * so it must have written at least one community_summaries row. */
    HU_ASSERT_TRUE(post_count >= 1);

    hu_graph_close(graph, &alloc);
}

#endif

/* AC-1.3: Compliance test that the gate comment exists in source. Checks the
 * comment is PRESENT (not at a hardcoded line — that pinned line 1471 and broke
 * whenever code was inserted above it; presence is the real contract). */
static void test_gate_comment_exists_at_agent_turn_1471(void) {
    FILE *f = fopen("src/agent/agent_turn.c", "r");
    HU_ASSERT_NOT_NULL(f);

    char buf[512];
    bool found_comment = false;
    /* Search the whole file: the gate comment PRESENCE is the contract; its
     * line shifts whenever agent_turn.c is edited (the ToM helper), so do not
     * pin a line range. */
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "GraphRAG activation gated") != NULL) {
            found_comment = true;
            break;
        }
    }
    fclose(f);
    HU_ASSERT_TRUE(found_comment);
}

/* Regression (GraphRAG calibration audit 2026-05-31): the Self-RAG
 * memory-relevance !should_use branch must NOT free graph_ctx.
 * hu_srag_verify_relevance scores memory_ctx ONLY; coupling graph_ctx's lifetime
 * to that verdict silently defeated GraphRAG grounding whenever flat memory was
 * judged irrelevant. Pins the decoupling: between the "Self-RAG: verify
 * relevance" comment and the "behavior_memory_ctx_nonempty" line that follows
 * the block, no graph_ctx free may appear. (Source-presence style, like the
 * gate-comment test above — fails on the pre-fix code that freed graph_ctx.) */
static void test_srag_memory_miss_does_not_free_graph_ctx(void) {
    FILE *f = fopen("src/agent/agent_turn.c", "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[512];
    bool in_block = false;
    bool freed_graph_in_block = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (!in_block) {
            if (strstr(buf, "Self-RAG: verify relevance") != NULL)
                in_block = true;
            continue;
        }
        /* The statement immediately following the Self-RAG block. */
        if (strstr(buf, "behavior_memory_ctx_nonempty") != NULL)
            break;
        if (strstr(buf, "graph_ctx") != NULL && strstr(buf, "free") != NULL)
            freed_graph_in_block = true;
    }
    fclose(f);
    HU_ASSERT_FALSE(freed_graph_in_block);
}

void run_graph_grounding_tests(void) {
    HU_TEST_SUITE("GraphRAG grounding");
    HU_RUN_TEST(test_graph_grounding_mode_parse);
    HU_RUN_TEST(test_gate_comment_exists_at_agent_turn_1471);
    HU_RUN_TEST(test_srag_memory_miss_does_not_free_graph_ctx);
    HU_RUN_TEST(test_ground_match_count_respects_word_boundaries);
    HU_RUN_TEST(test_ground_name_word_count_skips_stopwords_and_short_words);
    HU_RUN_TEST(test_ground_score_zero_without_match_and_coverage_dominates);
    HU_RUN_TEST(test_ground_fingerprint_varies_with_content);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_compose_selects_relevant_entity_content);
    HU_RUN_TEST(test_compose_irrelevant_message_returns_empty);
    HU_RUN_TEST(test_compose_word_boundary_prevents_false_seed);
    HU_RUN_TEST(test_compose_respects_budget_cap);
    HU_RUN_TEST(test_compose_varies_with_conversation);
    HU_RUN_TEST(test_compose_scopes_to_contact);
    HU_RUN_TEST(test_compose_no_graph_is_failopen);
    HU_RUN_TEST(test_autodream_tick_populates_community_summaries_for_contact);
#endif
}
