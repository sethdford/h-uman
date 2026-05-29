/* tests/test_e2e_learning_loop.c — US-107: end-to-end learning-loop proof.
 *
 * Proves the TikTok-style "feels-alive" loop closes end to end, deterministically,
 * without a live model server. Two sub-loops are genuinely exercised:
 *
 *   REWARD loop:   production_outcome (implicit signal)
 *                    -> hu_dpo_collector_mine_pairs_from_outcomes  (US-102)
 *                    -> dpo_pair
 *                    -> hu_reward_model_train                       (US-101)
 *                    -> reward model now RANKS chosen > rejected    (preference learned)
 *
 *   PROACTIVITY loop: proactive_send + outcome signal
 *                    -> hu_proactive_outcomes_process_async         (US-104)
 *                    -> hu_contextual_bandit arm updated            (US-103)
 *
 * The LLM-adapter train+swap legs (US-105/US-106) require a live MLX server and
 * are out of deterministic-test scope; their fallback path (swap to an
 * unreachable server fails gracefully, no crash) is asserted here per AC-107.5.
 * Component-level coverage for those legs lives in test_lora_nightly.c /
 * test_adapter_swap.c.
 *
 * HUML reward inputs are space-separated integer token IDs (US-101 lesson), so
 * the synthetic outcomes use a learnable low="good" / high="bad" token pattern.
 */
#include "human/ml/dpo.h"
#include "test_framework.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Canonical Pattern-1 gate (nested single #ifdefs so the gate-symmetry checker
 * recognizes each flag): the loop spine needs SQLite (mining/proactive tables)
 * AND ML (the reward model). A stub runner below keeps the symbol resolvable in
 * variants where either flag is off. */
#ifdef HU_ENABLE_ML
#ifdef HU_ENABLE_SQLITE
#define HU_E2E_LOOP_BUILT 1
#include "human/agent/contextual_bandit.h"
#include "human/core/allocator.h"
#include "human/ml/mlx_admin.h"
#include "human/ml/reward_model.h"
#include <sqlite3.h>

static sqlite3 *e2e_create_db(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
        return NULL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    if (hu_dpo_collector_create(&alloc, db, 10000, &collector) != HU_OK) {
        sqlite3_close(db);
        return NULL;
    }
    /* Creates dpo_pairs + production_outcomes + proactive_sends. */
    if (hu_dpo_init_tables(&collector) != HU_OK) {
        hu_dpo_collector_deinit(&collector);
        sqlite3_close(db);
        return NULL;
    }
    hu_dpo_collector_deinit(&collector);
    return db;
}

/* Insert a RESOLVED production outcome. A no-reply ("rejected") row passes
 * latency<0 / length<0 / sentiment<0 so those columns bind NULL; the mining
 * code treats NULL reply_latency_s as "no reply". */
static void e2e_insert_outcome(sqlite3 *db, const char *target, const char *prompt,
                               const char *chosen, int reply_latency_s, int reply_length,
                               double reply_sentiment) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO production_outcomes(channel, target, prompt, chosen, "
                                "send_timestamp, reply_latency_s, reply_length, reply_sentiment, "
                                "user_edited, outcome_resolved_at) "
                                "VALUES('imessage', ?, ?, ?, ?, ?, ?, ?, 0, ?)",
                                -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, target, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, prompt, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, chosen, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, now - 200);
    if (reply_latency_s >= 0)
        sqlite3_bind_int(stmt, 5, reply_latency_s);
    else
        sqlite3_bind_null(stmt, 5);
    if (reply_length >= 0)
        sqlite3_bind_int(stmt, 6, reply_length);
    else
        sqlite3_bind_null(stmt, 6);
    if (reply_sentiment >= 0.0)
        sqlite3_bind_double(stmt, 7, reply_sentiment);
    else
        sqlite3_bind_null(stmt, 7);
    sqlite3_bind_int64(stmt, 8, now);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void e2e_insert_proactive(sqlite3 *db, const char *contact, int outcome_type) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO proactive_sends(channel, contact, sent_timestamp, "
                                "outcome_type, outcome_timestamp, processed) "
                                "VALUES('imessage', ?, 100, ?, 200, 0)",
                                -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, contact, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, outcome_type);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static int e2e_count_processed_proactive(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends WHERE processed=1", -1, &stmt,
                           NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

/* Read the single mined pair (prompt/chosen/rejected) into out. */
static int e2e_read_pair(sqlite3 *db, hu_preference_pair_t *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT prompt, chosen, rejected FROM dpo_pairs LIMIT 1", -1, &stmt,
                           NULL) != SQLITE_OK)
        return 0;
    int got = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        const char *c = (const char *)sqlite3_column_text(stmt, 1);
        const char *r = (const char *)sqlite3_column_text(stmt, 2);
        if (p) {
            strncpy(out->prompt, p, sizeof(out->prompt) - 1);
            out->prompt_len = strlen(out->prompt);
        }
        if (c) {
            strncpy(out->chosen, c, sizeof(out->chosen) - 1);
            out->chosen_len = strlen(out->chosen);
        }
        if (r) {
            strncpy(out->rejected, r, sizeof(out->rejected) - 1);
            out->rejected_len = strlen(out->rejected);
        }
        out->margin = 1.0;
        got = 1;
    }
    sqlite3_finalize(stmt);
    return got;
}

/* Train a fresh HUML reward model on `pair` and return the post-training
 * (chosen_score - rejected_score) margin. Deterministic under HU_IS_TEST. */
static double e2e_trained_margin(const hu_preference_pair_t *pair) {
    /* The HUML value head's Xavier init samples global rand() (value_head.c),
     * so pin the seed before each create — same pattern as the convergence
     * test in test_reward_model_train.c — to make the margin reproducible. */
    srand(1234);
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML, .vocab_size = 100, .hidden_dim = 100};
    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);

    hu_reward_model_train_config_t tc = {.max_iters = 20, .learning_rate = 20.0, .log_every = 0};
    hu_reward_model_train_metrics_t m = {0};
    HU_ASSERT_EQ(hu_reward_model_train(&rm, &alloc, pair, 1, &tc, &m), HU_OK);

    double cs = 0.0, rs = 0.0;
    HU_ASSERT_EQ(rm.vtable->score(rm.ctx, &alloc, pair->prompt, pair->prompt_len, pair->chosen,
                                  pair->chosen_len, &cs),
                 HU_OK);
    HU_ASSERT_EQ(rm.vtable->score(rm.ctx, &alloc, pair->prompt, pair->prompt_len, pair->rejected,
                                  pair->rejected_len, &rs),
                 HU_OK);
    rm.vtable->deinit(rm.ctx, &alloc);
    return cs - rs;
}

/* AC-107.1/.2: implicit outcome -> mining -> dpo_pair -> reward model learns. */
static void e2e_reward_loop_closes(void) {
    sqlite3 *db = e2e_create_db();
    HU_ASSERT_NOT_NULL(db);

    /* alice: a reply-with-positive-sentiment message (chosen) and a no-reply
     * message (rejected). Integer-ID tokens so the HUML RM can score them. */
    e2e_insert_outcome(db, "alice", "0 1 2", "1 5", 120, 18, 0.8);   /* chosen */
    e2e_insert_outcome(db, "alice", "0 1 2", "40 41", -1, -1, -1.0); /* rejected (no reply) */

    int written = 0;
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes(db, INT_MAX, &written), HU_OK);
    HU_ASSERT_EQ(written, 1); /* the implicit signal became one preference pair */

    hu_preference_pair_t pair;
    HU_ASSERT(e2e_read_pair(db, &pair) == 1);
    HU_ASSERT(pair.chosen_len > 0 && pair.rejected_len > 0);

    /* The reward model, trained on the MINED pair, must rank chosen above
     * rejected — it learned the preference the implicit signal expressed.
     * This is the reward sub-loop closing end to end. */
    double margin = e2e_trained_margin(&pair);
    HU_ASSERT(margin > 0.0);

    sqlite3_close(db);
}

/* AC-107.3: deterministic — same outcomes -> same mined pair -> same margin. */
static void e2e_reward_loop_deterministic(void) {
    hu_preference_pair_t pair;
    memset(&pair, 0, sizeof(pair));
    strncpy(pair.prompt, "0 1 2", sizeof(pair.prompt) - 1);
    pair.prompt_len = strlen(pair.prompt);
    strncpy(pair.chosen, "1 5", sizeof(pair.chosen) - 1);
    pair.chosen_len = strlen(pair.chosen);
    strncpy(pair.rejected, "40 41", sizeof(pair.rejected) - 1);
    pair.rejected_len = strlen(pair.rejected);
    pair.margin = 1.0;

    double m1 = e2e_trained_margin(&pair);
    double m2 = e2e_trained_margin(&pair);
    HU_ASSERT(fabs(m1 - m2) < 1e-9); /* bit-stable under the fixed test seed */
}

/* AC-107.1/.2: proactive send + implicit outcome -> bandit arm update. */
static void e2e_proactivity_loop_closes(void) {
    sqlite3 *db = e2e_create_db();
    HU_ASSERT_NOT_NULL(db);
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &bandit), HU_OK);
    HU_ASSERT_NOT_NULL(bandit);

    /* Two resolved proactive sends: alice replied (REPLY=0), bob ignored (IGNORED=1). */
    e2e_insert_proactive(db, "alice", 0);
    e2e_insert_proactive(db, "bob", 1);
    HU_ASSERT_EQ(e2e_count_processed_proactive(db), 0);

    HU_ASSERT_EQ(hu_proactive_outcomes_process_async(db, (void *)bandit), HU_OK);

    /* Both outcomes flowed into the bandit and were marked processed -> the
     * proactivity sub-loop closed. */
    HU_ASSERT_EQ(e2e_count_processed_proactive(db), 2);

    hu_contextual_bandit_destroy(bandit);
    sqlite3_close(db);
}

/* AC-107.5: the adapter-swap leg fails GRACEFULLY against an unreachable server
 * (no live MLX in tests) — returns an error, does not crash, loop continues. */
static void e2e_swap_fallback_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_admin_swap_result_t res;
    memset(&res, 0, sizeof(res));
    const char *dead = "http://127.0.0.1:9/v1"; /* discard port — nothing listens */
    const char *apath = "/tmp/hu_e2e_nonexistent_adapter";
    hu_error_t err =
        hu_mlx_admin_swap_adapter(&alloc, dead, strlen(dead), apath, strlen(apath), &res);
    /* Reached here without crashing; swap to a dead endpoint must not succeed. */
    HU_ASSERT(err != HU_OK);
    hu_mlx_admin_swap_result_free(&alloc, &res);
}

void run_e2e_learning_loop_tests(void) {
    HU_TEST_SUITE("e2e_learning_loop");
    HU_RUN_TEST(e2e_reward_loop_closes);
    HU_RUN_TEST(e2e_reward_loop_deterministic);
    HU_RUN_TEST(e2e_proactivity_loop_closes);
    HU_RUN_TEST(e2e_swap_fallback_graceful);
}

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_ENABLE_ML */

#ifndef HU_E2E_LOOP_BUILT
/* Stub keeps run_e2e_learning_loop_tests resolvable when SQLite or ML is off. */
void run_e2e_learning_loop_tests(void) {
    (void)0;
}
#endif
