/* W9 — World model: build, cache hit/miss, invalidation, negative memory.
 * All tests run on in-memory SQLite. */

#include "human/agent/world_model.h"
#include "human/agent/goals.h"
#include "human/core/allocator.h"
#include "human/memory/emotional_residue.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef HU_ENABLE_SQLITE

extern struct sqlite3 *hu_graph__db_handle(hu_graph_t *g);

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_facade_(hu_graph_t **g, hu_memory_facade_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    /* Ensure no stale cache entries leak across tests. */
    hu_world_model_invalidate(NULL, 0);
}

static void close_facade_(hu_graph_t *g, hu_memory_facade_t *m) {
    hu_world_model_invalidate(NULL, 0);
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

static void seed_one_relation_(hu_graph_t *g, const char *cid) {
    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), "Alice", 5,
                                          HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), "Acme", 4,
                                          HU_ENTITY_ORGANIZATION, NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(g, cid, strlen(cid), alice, acme,
                                            HU_REL_WORKS_AT, 1.0f, NULL, 0),
                 HU_OK);
}

/* --- build --- */

static void test_w9_build_returns_entities_and_relations(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u1");

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u1", 2, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(strcmp(wm->contact_id, "u1"), 0);
    HU_ASSERT_GT(wm->built_at, 0);
    HU_ASSERT_GT(wm->valid_until, wm->built_at);
    HU_ASSERT_EQ(wm->entities_count, 2u);
    HU_ASSERT_EQ(wm->relations_count, 1u);
    /* P2D ToM: user_thinks_we_are = top entity name, user_expects_we_can
     * is empty (no W12 planner yet), user_expects_we_cannot is empty
     * (no negatives seeded). */
    HU_ASSERT_EQ(strcmp(wm->dominant_emotion, "neutral"), 0);
    HU_ASSERT_TRUE(wm->tom.user_thinks_we_are[0] != '\0');
    HU_ASSERT_EQ(wm->tom.user_expects_we_can[0], '\0');
    HU_ASSERT_EQ(wm->tom.user_expects_we_cannot[0], '\0');
    /* Recent topics derived from entity names. */
    HU_ASSERT_EQ(wm->recent_topics_count, 2u);
    /* Goals table doesn't exist yet in this DB, so 0 goals. */
    HU_ASSERT_EQ(wm->goals_count, 0u);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w9_build_with_no_data_returns_empty_snapshot(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-empty", 7, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(wm->entities_count, 0u);
    HU_ASSERT_EQ(wm->relations_count, 0u);
    HU_ASSERT_EQ(wm->negatives_count, 0u);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* --- cache --- */

static void test_w9_load_cache_hit_within_ttl(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-cached");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-cached", 8, 1000LL, &wm1), HU_OK);
    HU_ASSERT_EQ(wm1->entities_count, 2u);

    /* Cache hit within TTL — no intervening writes. P2 #7 wires
     * upsert_entity → hu_world_model_invalidate, so the original
     * "insert here and observe staleness" pattern no longer holds.
     * The pure-read cache hit still must return the same 2-entity
     * snapshot. */
    hu_world_model_t *wm2 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-cached", 8, 2000LL, &wm2), HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 2u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

static void test_w9_load_cache_miss_after_invalidation(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-inv");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-inv", 5, 1000LL, &wm1), HU_OK);
    HU_ASSERT_EQ(wm1->entities_count, 2u);

    int64_t carol = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-inv", 5, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol),
                 HU_OK);

    /* Invalidate; next load should rebuild and see Carol. */
    hu_world_model_invalidate("u-inv", 5);

    hu_world_model_t *wm2 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-inv", 5, 2000LL, &wm2), HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 3u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

/* P2 #7 — write→invalidate contract: a graph upsert MUST invalidate the
 * cache automatically without callers having to remember the explicit
 * hu_world_model_invalidate(). This proves the new contract. */
static void test_w9_upsert_auto_invalidates_cache(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-auto");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-auto", 6, 1000LL, &wm1), HU_OK);
    HU_ASSERT_EQ(wm1->entities_count, 2u);

    int64_t carol = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-auto", 6, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol),
                 HU_OK);

    /* No explicit invalidate. The next load must still rebuild because
     * upsert_entity wired the invalidate hook. */
    hu_world_model_t *wm2 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-auto", 6, 2000LL, &wm2), HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 3u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

static void test_w9_load_cache_expires_after_ttl(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-ttl");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-ttl", 5, 1000LL, &wm1), HU_OK);

    int64_t carol = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-ttl", 5, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol),
                 HU_OK);

    /* TTL is 60s by default; jump 120s ahead. */
    hu_world_model_t *wm2 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-ttl", 5,
                                       1000LL + 120 * 1000, &wm2),
                 HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 3u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

/* --- negative memory --- */

static void test_w9_negative_memory_round_trip(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    hu_negative_memory_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.text, "do not bring up the project failure");
    strcpy(nm.scope, "topic");
    strcpy(nm.reason, "user said it was painful");
    nm.belief = hu_belief_init(0.95f, "user-explicit", 1735690000000LL);
    nm.created_at = 1735690000000LL;

    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u-neg", 5, &nm, &id), HU_OK);
    HU_ASSERT_GT(id, 0);

    hu_negative_memory_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_negative_memory_list(g, A(), "u-neg", 5, 32, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(out[0].id, id);
    HU_ASSERT_EQ(strcmp(out[0].text, "do not bring up the project failure"), 0);
    HU_ASSERT_EQ(strcmp(out[0].scope, "topic"), 0);
    HU_ASSERT_FLOAT_EQ(out[0].belief.mean, 0.95f, 1e-3);

    hu_negative_memory_free(A(), out, n);
    close_facade_(g, m);
}

static void test_w9_negative_memory_appears_in_world_model(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-neg2");

    hu_negative_memory_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.text, "no jokes about Acme's CEO");
    strcpy(nm.scope, "topic");
    nm.belief = hu_belief_init(0.99f, "user-explicit", 1735690000000LL);
    nm.created_at = 1735690000000LL;
    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u-neg2", 6, &nm, &id), HU_OK);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-neg2", 6, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(wm->negatives_count, 1u);
    HU_ASSERT_EQ(strcmp(wm->negatives[0].text, "no jokes about Acme's CEO"), 0);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* --- goals in world model --- */

static void test_w9_goals_appear_in_world_model(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    /* Seed the goals table and insert an active goal. */
    struct sqlite3 *db = hu_graph__db_handle(g);
    HU_ASSERT_NOT_NULL(db);
    hu_goal_engine_t ge;
    HU_ASSERT_EQ(hu_goal_engine_create(A(), db, &ge), HU_OK);
    HU_ASSERT_EQ(hu_goal_init_tables(&ge), HU_OK);
    int64_t gid = 0;
    HU_ASSERT_EQ(hu_goal_create(&ge, "learn rust", 10, 0.8, 0, 0,
                                 1735690000LL, &gid),
                 HU_OK);
    HU_ASSERT_GT(gid, 0);
    hu_goal_engine_deinit(&ge);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-goals", 7, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(wm->goals_count, 1u);
    HU_ASSERT_EQ(strcmp(wm->goals[0].text, "learn rust"), 0);
    HU_ASSERT_FLOAT_EQ(wm->goals[0].salience, 0.8f, 1e-3);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* --- P2D: emotion + theory-of-mind populated from v1 sources --- */

/* Helper: ensure the emotional_residue table exists on this graph DB.
 * The W7 SQLite engine creates it on `human ml`/agent paths, but the
 * minimal in-memory graph in this test doesn't run that bootstrap. */
static void ensure_emotional_residue_table_(struct sqlite3 *db) {
    static const char *kCreate =
        "CREATE TABLE IF NOT EXISTS emotional_residue("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "episode_id INTEGER,"
        "contact_id TEXT NOT NULL,"
        "valence REAL NOT NULL,"
        "intensity REAL NOT NULL,"
        "decay_rate REAL DEFAULT 0.1,"
        "created_at INTEGER NOT NULL)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, kCreate, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, 0);
    rc = sqlite3_step(stmt);
    HU_ASSERT_TRUE(rc == 101 /* SQLITE_DONE */);
    sqlite3_finalize(stmt);
}

static void test_w9_emotion_populated_from_distress_residue(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    struct sqlite3 *db = hu_graph__db_handle(g);
    HU_ASSERT_NOT_NULL(db);
    ensure_emotional_residue_table_(db);

    /* Two residues: one strong distress, one mild positive. Weighted by
     * intensity, the distress dominates (valence < -0.5 -> distressed). */
    int64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_emotional_residue_add(db, 0, "u-emo", 5,
                                          -0.9, 0.9, 0.05, &id1), HU_OK);
    HU_ASSERT_EQ(hu_emotional_residue_add(db, 0, "u-emo", 5,
                                          0.3, 0.2, 0.05, &id2), HU_OK);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-emo", 5,
                                       1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_TRUE(wm->valence < -0.4f);
    HU_ASSERT_TRUE(wm->arousal >= 0.85f);
    HU_ASSERT_EQ(strcmp(wm->dominant_emotion, "distressed"), 0);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w9_emotion_populated_from_joy_residue(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    struct sqlite3 *db = hu_graph__db_handle(g);
    ensure_emotional_residue_table_(db);

    int64_t id1 = 0;
    HU_ASSERT_EQ(hu_emotional_residue_add(db, 0, "u-joy", 5,
                                          0.85, 0.7, 0.05, &id1), HU_OK);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-joy", 5,
                                       1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_TRUE(wm->valence > 0.5f);
    HU_ASSERT_EQ(strcmp(wm->dominant_emotion, "joy"), 0);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w9_tom_synthesized_from_negatives_and_entities(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-tom");

    /* Add two negative memories. ToM `user_expects_we_cannot` should
     * '; '-join their text. `user_thinks_we_are` should be the top
     * entity (Alice was inserted first). */
    hu_negative_memory_t nm1 = {0};
    strcpy(nm1.text, "no political opinions");
    strcpy(nm1.scope, "topic");
    nm1.belief = hu_belief_init(0.9f, "user-explicit", 1735690000000LL);
    nm1.created_at = 1735690000000LL;
    int64_t id1 = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u-tom", 5, &nm1, &id1), HU_OK);

    hu_negative_memory_t nm2 = {0};
    strcpy(nm2.text, "no jokes about my brother");
    strcpy(nm2.scope, "topic");
    nm2.belief = hu_belief_init(0.85f, "user-explicit", 1735690000000LL);
    nm2.created_at = 1735690000000LL;
    int64_t id2 = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u-tom", 5, &nm2, &id2), HU_OK);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-tom", 5,
                                       1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);

    /* user_expects_we_cannot has both negatives ';'-joined. */
    HU_ASSERT_TRUE(strstr(wm->tom.user_expects_we_cannot, "no political opinions") != NULL);
    HU_ASSERT_TRUE(strstr(wm->tom.user_expects_we_cannot, "no jokes about my brother") != NULL);
    HU_ASSERT_TRUE(strstr(wm->tom.user_expects_we_cannot, "; ") != NULL);

    /* user_thinks_we_are = top entity name (one of the seeded entities). */
    HU_ASSERT_TRUE(wm->tom.user_thinks_we_are[0] != '\0');

    /* Confidence rises with corroboration (2 negatives + 1 entity = 3 signals,
     * capped at 0.7). */
    HU_ASSERT_FLOAT_EQ(wm->tom.confidence.mean, 0.7f, 1e-3);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* --- adversarial --- */

static void test_w9_invalid_args_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(NULL, A(), "u", 1, 0, &wm), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_model_build(m, NULL, "u", 1, 0, &wm), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_model_build(m, A(), NULL, 0, 0, &wm), HU_ERR_INVALID_ARGUMENT);

    hu_negative_memory_t nm;
    memset(&nm, 0, sizeof(nm));
    /* empty text rejected */
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u", 1, &nm, NULL), HU_ERR_INVALID_ARGUMENT);

    /* contact_id longer than 64 bytes is rejected at load. */
    char long_cid[80];
    memset(long_cid, 'x', 79);
    long_cid[79] = '\0';
    HU_ASSERT_EQ(hu_world_model_load(m, A(), long_cid, 79, 0, &wm),
                 HU_ERR_INVALID_ARGUMENT);

    close_facade_(g, m);
}

void run_w9_world_model_tests(void) {
    HU_TEST_SUITE("W9 world model - per-contact unified snapshot");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w9_build_returns_entities_and_relations);
    HU_RUN_TEST(test_w9_build_with_no_data_returns_empty_snapshot);
    HU_RUN_TEST(test_w9_load_cache_hit_within_ttl);
    HU_RUN_TEST(test_w9_load_cache_miss_after_invalidation);
    HU_RUN_TEST(test_w9_upsert_auto_invalidates_cache);
    HU_RUN_TEST(test_w9_load_cache_expires_after_ttl);
    HU_RUN_TEST(test_w9_negative_memory_round_trip);
    HU_RUN_TEST(test_w9_negative_memory_appears_in_world_model);
    HU_RUN_TEST(test_w9_goals_appear_in_world_model);
    HU_RUN_TEST(test_w9_emotion_populated_from_distress_residue);
    HU_RUN_TEST(test_w9_emotion_populated_from_joy_residue);
    HU_RUN_TEST(test_w9_tom_synthesized_from_negatives_and_entities);
    HU_RUN_TEST(test_w9_invalid_args_rejected);
#endif
}

#else /* !HU_ENABLE_SQLITE */

void run_w9_world_model_tests(void) {}

#endif
