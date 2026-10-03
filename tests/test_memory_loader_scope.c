/* Contact scope for recalled memories (src/agent/memory_loader.c,
 * src/memory/retrieval/namespace.c). On 2026-10-01 a contact's commitments
 * ("I'll send the completed copy to the HOA") and the global
 * hierarchical_* summaries were recallable while texting anyone else: the
 * loader passed no scope, and semantic hits carry no session at all. */
#include "test_framework.h"

#include "human/memory/retrieval.h"

#include <string.h>

static void session_scope_keeps_own_and_global_rows(void) {
    HU_ASSERT_TRUE(hu_retrieval_session_in_scope("", 0, "+15550000001", 12));
    HU_ASSERT_TRUE(hu_retrieval_session_in_scope(NULL, 0, "+15550000001", 12));
    HU_ASSERT_TRUE(hu_retrieval_session_in_scope("+15550000001", 12, "+15550000001", 12));
    HU_ASSERT_FALSE(hu_retrieval_session_in_scope("+15550000002", 12, "+15550000001", 12));
    /* a prefix is not the same contact */
    HU_ASSERT_FALSE(hu_retrieval_session_in_scope("+1555000000", 11, "+15550000001", 12));
    /* no contact to scope to (CLI, tests): everything is in scope */
    HU_ASSERT_TRUE(hu_retrieval_session_in_scope("+15550000002", 12, NULL, 0));
}

#ifdef HU_ENABLE_SQLITE
#include "human/agent/memory_loader.h"
#include "human/core/allocator.h"
#include "human/memory/engines.h"

static void store_for(hu_memory_t *m, const char *key, const char *text, const char *who) {
    hu_error_t err = m->vtable->store(m->ctx, key, strlen(key), text, strlen(text), NULL, who,
                                      who ? strlen(who) : 0);
    HU_ASSERT_EQ(err, HU_OK);
}

static void loader_recall_skips_other_contacts_memories(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    store_for(&mem, "commitment:a1", "I'll send the HOA form tomorrow", "+15550000001");
    store_for(&mem, "commitment:b1", "I'll send the lease form tomorrow", "+15550000002");
    store_for(&mem, "core:g1", "Seth sends every form by email", NULL);
    hu_retrieval_engine_t eng = hu_retrieval_create(&a, &mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, &eng, 8, 4096), HU_OK);
    const char q[] = "send the form";
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, q, strlen(q), "+15550000001", 12, &ctx, &ctx_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "HOA"));
    HU_ASSERT_NOT_NULL(strstr(ctx, "by email"));
    HU_ASSERT_NULL(strstr(ctx, "lease")); /* the other contact's promise */
    a.free(a.ctx, ctx, ctx_len + 1);
    eng.vtable->deinit(eng.ctx, &a);
    mem.vtable->deinit(mem.ctx);
}

/* One 7,680-char row filled the whole recall budget on 2026-10-02, so no
 * other memory about the contact reached the reply. */
static void loader_one_long_memory_cannot_fill_recall(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    static char big[7700];
    size_t n = 0;
    while (n + 20 < sizeof(big)) {
        memcpy(big + n, "weekend plans boat ", 19);
        n += 19;
    }
    big[n] = '\0';
    store_for(&mem, "core:weekend plans", big, NULL);
    store_for(&mem, "core:w1", "Seth takes the boat out most weekend mornings", NULL);
    hu_retrieval_engine_t eng = hu_retrieval_create(&a, &mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, &eng, 8, 4096), HU_OK);
    const char q[] = "weekend plans boat";
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, q, strlen(q), NULL, 0, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "most weekend mornings"));
    HU_ASSERT_TRUE(ctx_len < 2500);
    a.free(a.ctx, ctx, ctx_len + 1);
    eng.vtable->deinit(eng.ctx, &a);
    mem.vtable->deinit(mem.ctx);
}

/* Experience rows are global (no contact) turn logs: inbound text + the
 * daemon's reply. Keyword recall put other people's messages and rating
 * prompts into a reply prompt on 2026-10-02; the semantic path already
 * excludes them (hu_semantic_recall_hit_is_excluded). */
static void loader_recall_skips_experience_scaffold(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    store_for(&mem, "experience:Hi Seth - did yo",
              "Task: Hi Seth - did you get the boat fixed?\nActions: agent_turn\nOutcome: yep",
              NULL);
    store_for(&mem, "core:b1", "Seth keeps the boat at the marina", NULL);
    hu_retrieval_engine_t eng = hu_retrieval_create(&a, &mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, &eng, 8, 4096), HU_OK);
    const char q[] = "boat fixed";
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, q, strlen(q), "+15550000001", 12, &ctx, &ctx_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "marina"));
    HU_ASSERT_NULL(strstr(ctx, "did you get the boat"));
    a.free(a.ctx, ctx, ctx_len + 1);
    eng.vtable->deinit(eng.ctx, &a);
    mem.vtable->deinit(mem.ctx);
}

static void session_of_reads_the_owner_back_by_key(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    store_for(&mem, "commitment:a1", "x", "+15550000001");
    store_for(&mem, "core:g1", "y", NULL);
    char owner[64];
    HU_ASSERT_TRUE(hu_sqlite_memory_session_of(&mem, "commitment:a1", 13, owner, sizeof(owner)));
    HU_ASSERT_STR_EQ(owner, "+15550000001");
    HU_ASSERT_TRUE(hu_sqlite_memory_session_of(&mem, "core:g1", 7, owner, sizeof(owner)));
    HU_ASSERT_STR_EQ(owner, "");
    HU_ASSERT_FALSE(hu_sqlite_memory_session_of(&mem, "gone", 4, owner, sizeof(owner)));
    mem.vtable->deinit(mem.ctx);
}

/* DEF-17: strategy_outcomes.success was `count > 0` written only inside
 * `if (count > 0)` — 4,850 of 4,850 prod rows are success=1, and recommend()
 * feeds its own pick back (SEMANTIC: 2,433 rows, all KEYWORD). With
 * HU_STRATEGY_SIGNAL=live the loader neither writes that tautology nor lets
 * the all-success history steer retrieval. */
#include "human/memory/retrieval/strategy_learner.h"

static int strategy_rows(sqlite3 *db, int *successes) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    *successes = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*), COALESCE(SUM(success),0) FROM strategy_outcomes",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
        *successes = sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
    return n;
}

static void strategy_load_once(hu_memory_t *mem, hu_retrieval_engine_t *eng) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, mem, eng, 8, 4096), HU_OK);
    const char q[] = "send the form";
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, q, strlen(q), "+15550000001", 12, &ctx, &ctx_len),
                 HU_OK);
    if (ctx)
        a.free(a.ctx, ctx, ctx_len + 1);
}

static void strategy_signal_live_writes_no_tautological_success(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    store_for(&mem, "commitment:a1", "I'll send the HOA form tomorrow", "+15550000001");
    hu_retrieval_engine_t eng = hu_retrieval_create(&a, &mem);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    /* OFF (today): a successful recall writes one success=1 row. */
    hu_strategy_signal_set_mode_for_test(HU_GATE_OFF);
    strategy_load_once(&mem, &eng);
    int wins = 0;
    HU_ASSERT_EQ(strategy_rows(db, &wins), 1);
    HU_ASSERT_EQ(wins, 1);

    /* LIVE: the same recall writes nothing. */
    hu_strategy_signal_set_mode_for_test(HU_GATE_LIVE);
    strategy_load_once(&mem, &eng);
    HU_ASSERT_EQ(strategy_rows(db, &wins), 1);

    /* SHADOW: unchanged from OFF (still writes), only logs. */
    hu_strategy_signal_set_mode_for_test(HU_GATE_SHADOW);
    strategy_load_once(&mem, &eng);
    HU_ASSERT_EQ(strategy_rows(db, &wins), 2);

    hu_strategy_signal_set_mode_for_test(-1);
    eng.vtable->deinit(eng.ctx, &a);
    mem.vtable->deinit(mem.ctx);
}

/* The all-success history must not steer retrieval once LIVE: recommend()
 * returns the neutral HYBRID (no override) instead of the arbitrary
 * all-success winner. OFF keeps today's pick. */
static void strategy_signal_live_ignores_all_success_history(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_allocator_t a = hu_system_allocator();
    hu_strategy_learner_t sl;
    HU_ASSERT_EQ(hu_strategy_learner_create(&a, db, &sl), HU_OK);
    HU_ASSERT_EQ(hu_strategy_learner_init_tables(&sl), HU_OK);
    for (int i = 0; i < 20; i++)
        HU_ASSERT_EQ(
            hu_strategy_learner_record(&sl, HU_QCAT_SEMANTIC, HU_RSTRAT_KEYWORD, true, 1000 + i),
            HU_OK);
    hu_strategy_signal_set_mode_for_test(HU_GATE_OFF);
    HU_ASSERT_EQ(hu_strategy_learner_recommend_gated(&sl, HU_QCAT_SEMANTIC), HU_RSTRAT_KEYWORD);
    hu_strategy_signal_set_mode_for_test(HU_GATE_SHADOW);
    HU_ASSERT_EQ(hu_strategy_learner_recommend_gated(&sl, HU_QCAT_SEMANTIC), HU_RSTRAT_KEYWORD);
    hu_strategy_signal_set_mode_for_test(HU_GATE_LIVE);
    HU_ASSERT_EQ(hu_strategy_learner_recommend_gated(&sl, HU_QCAT_SEMANTIC), HU_RSTRAT_HYBRID);
    hu_strategy_signal_set_mode_for_test(-1);
    hu_strategy_learner_deinit(&sl);
    sqlite3_close(db);
}
#endif

void run_memory_loader_scope_tests(void) {
    HU_TEST_SUITE("memory loader contact scope");
    HU_RUN_TEST(session_scope_keeps_own_and_global_rows);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(loader_recall_skips_other_contacts_memories);
    HU_RUN_TEST(loader_one_long_memory_cannot_fill_recall);
    HU_RUN_TEST(loader_recall_skips_experience_scaffold);
    HU_RUN_TEST(session_of_reads_the_owner_back_by_key);
    HU_RUN_TEST(strategy_signal_live_writes_no_tautological_success);
    HU_RUN_TEST(strategy_signal_live_ignores_all_success_history);
#endif
}
