/* tests/test_outbound_stats_e2e.c
 *
 * Sprint 60 — E2E proof that the pipeline's per-stage stats counters
 * actually fire when a real outbound message goes through the full
 * pipeline. Pairs with the unit-level test_outbound_stats.c which
 * exercises the record/snapshot contracts in isolation.
 *
 * This file proves the WIRING at src/agent/outbound/pipeline.c:215
 * is live — without it, the counters would always read zero in
 * production no matter how many messages flowed through.
 *
 * Two contracts pinned:
 *   1. Bleed content through HU_OUTBOUND_PATH_PROACTIVE bumps the
 *      crosstalk REJECT counter (and at least one other SEND for
 *      stages that ran before crosstalk).
 *   2. Clean content through the same path bumps every stage's SEND
 *      counter — proves each stage actually executed.
 *
 * Gated on HU_ENABLE_SQLITE because the crosstalk REJECT path needs
 * the SQLite lookup registered to fire.
 */

#include "test_framework.h"

#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/agent/outbound_pipeline.h"
#include "human/agent/outbound_stats.h"
#include "human/core/allocator.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *e2e_stats_db_path(void) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_e2e_stats_proof_%d.db", (int)getpid());
    return path;
}

static void run_sql_or_fail(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    if (err)
        sqlite3_free(err);
}

static sqlite3 *open_e2e_db(void) {
    const char *path = e2e_stats_db_path();
    unlink(path);
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);
    run_sql_or_fail(db, "CREATE TABLE messages("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "session_id TEXT NOT NULL,"
                        "role TEXT NOT NULL,"
                        "content TEXT NOT NULL,"
                        "created_at TEXT DEFAULT(datetime('now')))");
    /* Seed an OTHER contact with a phrase the bleed test will replay. */
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_OTHER', 'assistant', "
                        "'but boy I am just more lonely now than ever', "
                        "datetime('now'))");
    return db;
}

static void close_e2e_db(sqlite3 *db) {
    sqlite3_close(db);
    unlink(e2e_stats_db_path());
}

/* Bleed content through the production proactive pipeline must
 * REGISTER a REJECT in the crosstalk counter. Without the wiring at
 * pipeline.c:215, this would silently fail (verdict still REJECT,
 * but counter stays 0). */
static void test_bleed_run_increments_crosstalk_reject_counter(void) {
    hu_outbound_stats_reset_for_test();
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_e2e_db();
    hu_outbound_crosstalk_register_sqlite(db);

    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(&alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipe), HU_OK);

    const char *bleed = "but boy I am just more lonely now than ever";
    size_t len = strlen(bleed);
    char *content = (char *)alloc.alloc(alloc.ctx, len + 1);
    memcpy(content, bleed, len + 1);

    hu_outbound_message_t msg = {0};
    msg.content = content;
    msg.content_len = len;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_RECIPIENT";
    ctx.recipient_contact_id_len = strlen("+CONTACT_RECIPIENT");
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);
    HU_ASSERT_TRUE(verdict.kind == HU_OUTBOUND_REJECT);

    /* The load-bearing E2E assertion: the crosstalk REJECT counter
     * advanced by exactly 1. Without the wiring at pipeline.c:215
     * (hu_outbound_stats_record call), this would still be 0. */
    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_CROSSTALK][3], 1u); /* REJECT */

    /* Stages BEFORE crosstalk in the proactive config (strip, shape,
     * echo) must have each bumped SEND once — they ran and let the
     * content through. */
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_STRIP][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_SHAPE][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_ECHO][0], 1u);

    /* Stages AFTER crosstalk (persona, moderation) never ran —
     * crosstalk REJECT bubbles up, pipeline short-circuits. */
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_PERSONA][0], 0u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_MODERATION][0], 0u);

    hu_outbound_verdict_clear(&verdict, &alloc);
    if (msg.content)
        alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);
    hu_outbound_crosstalk_unregister_sqlite();
    close_e2e_db(db);
    hu_outbound_stats_reset_for_test();
}

/* Clean content through the same path must bump EVERY stage's SEND
 * counter exactly once — proves every stage actually executed and
 * the wiring fires on the SEND verdict path, not just REJECT. */
static void test_clean_run_increments_every_stage_send_counter(void) {
    hu_outbound_stats_reset_for_test();
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_e2e_db();
    hu_outbound_crosstalk_register_sqlite(db);

    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(&alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipe), HU_OK);

    const char *clean = "hope your week is going well";
    size_t len = strlen(clean);
    char *content = (char *)alloc.alloc(alloc.ctx, len + 1);
    memcpy(content, clean, len + 1);

    hu_outbound_message_t msg = {0};
    msg.content = content;
    msg.content_len = len;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_RECIPIENT";
    ctx.recipient_contact_id_len = strlen("+CONTACT_RECIPIENT");
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);
    HU_ASSERT_TRUE(verdict.kind == HU_OUTBOUND_SEND);

    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);

    /* Every stage in pipeline_configs.c::s_proactive_stages must
     * have bumped its SEND counter exactly once. If any of these is
     * 0, that stage didn't run — wiring regression. */
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_STRIP][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_SHAPE][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_ECHO][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_CROSSTALK][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_PERSONA][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_MODERATION][0], 1u);

    hu_outbound_verdict_clear(&verdict, &alloc);
    if (msg.content)
        alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);
    hu_outbound_crosstalk_unregister_sqlite();
    close_e2e_db(db);
    hu_outbound_stats_reset_for_test();
}

void run_outbound_stats_e2e_tests(void) {
    HU_TEST_SUITE("outbound_stats_e2e");
    HU_RUN_TEST(test_bleed_run_increments_crosstalk_reject_counter);
    HU_RUN_TEST(test_clean_run_increments_every_stage_send_counter);
    hu_outbound_stats_reset_for_test();
}
