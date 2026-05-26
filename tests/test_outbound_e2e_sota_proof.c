/* tests/test_outbound_e2e_sota_proof.c
 *
 * End-to-end SOTA proof for the Sprint 59 + Sprint 60 outbound safety
 * pipeline. The other test files isolate concerns:
 *   - tests/test_outbound_crosstalk.c          — Jaccard predicate only
 *   - tests/test_outbound_crosstalk_sqlite.c   — SQLite lookup + register
 *   - tests/test_outbound_corpus_regression.c  — pipeline w/ fake_lookup
 *
 * This file proves the FULL production stack works together:
 *   1. File-based SQLite db (not :memory:) — mirrors deployment
 *   2. Production registration via hu_outbound_crosstalk_register_sqlite
 *      (not a hand-rolled fake lookup)
 *   3. Full pipeline via hu_outbound_pipeline_for_path / _run — every
 *      stage in pipeline_configs.c for HU_OUTBOUND_PATH_PROACTIVE runs
 *   4. Annie/Mindy/Betty replay shape: insert verbatim content from
 *      Mindy's row, then send the same content as a proactive to Annie.
 *      The crosstalk stage must REJECT it.
 *   5. Clean novel content sent to the same recipient must SEND.
 *   6. Unregister cleanly so the static callback never sees a freed db.
 *
 * If the pipeline ever loses the SQLite wiring or a future refactor
 * skips the crosstalk stage on the proactive path, this test fails
 * with `verdict.kind != REJECT`. That's the load-bearing assertion.
 *
 * Gated on HU_ENABLE_SQLITE in CMakeLists.txt (the registration helper
 * is SQLite-gated).
 */

#include "test_framework.h"

#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Use a file-based db (not :memory:) so the test exercises the same
 * SQLite path the daemon uses — disk i/o, journal mode, fsync. The
 * cost is ~10ms of setup. */
static const char *test_db_path(void) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_e2e_sota_proof_%d.db", (int)getpid());
    return path;
}

static void run_sql_or_fail(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    if (err)
        sqlite3_free(err);
}

static sqlite3 *open_seeded_db(void) {
    const char *path = test_db_path();
    unlink(path);
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);

    /* Match the production schema in src/memory/engines/sqlite.c:59. */
    run_sql_or_fail(db, "CREATE TABLE messages("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "session_id TEXT NOT NULL,"
                        "role TEXT NOT NULL,"
                        "content TEXT NOT NULL,"
                        "created_at TEXT DEFAULT(datetime('now')))");
    run_sql_or_fail(db, "CREATE INDEX idx_messages_session ON messages(session_id)");

    /* Seed three contacts. Mindy's row carries the verbatim phrase that
     * caused the original Annie/Mindy/Betty bleed (per
     * docs/plans/2026-05-26-sprint-59-outbound-safety/incident-corpus.md). */
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_MINDY', 'assistant', "
                        "'but boy I am just more lonely now than ever', "
                        "datetime('now', '-2 hours'))");
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_BETTY', 'assistant', "
                        "'thinking of you and how that meeting went today', "
                        "datetime('now', '-1 hours'))");
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_ANNIE', 'assistant', "
                        "'how did the bike ride go this weekend', "
                        "datetime('now', '-3 hours'))");
    return db;
}

static void close_test_db(sqlite3 *db) {
    sqlite3_close(db);
    unlink(test_db_path());
}

/* The defining E2E proof: the Annie/Mindy/Betty bleed shape, run
 * through the full PROACTIVE pipeline with the production SQLite
 * lookup registered. */
static void test_e2e_proactive_pipeline_rejects_crosscontact_bleed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_seeded_db();
    hu_outbound_crosstalk_register_sqlite(db);

    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(&alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipe), HU_OK);
    HU_ASSERT_NOT_NULL(pipe);

    /* Replay: agent is about to send a proactive to Annie, content is
     * a verbatim copy of what was sent to Mindy. The pipeline MUST stop
     * it. */
    const char *bleed_content = "but boy I am just more lonely now than ever";
    size_t blen = strlen(bleed_content);
    char *content = (char *)alloc.alloc(alloc.ctx, blen + 1);
    HU_ASSERT_NOT_NULL(content);
    memcpy(content, bleed_content, blen + 1);

    hu_outbound_message_t msg = {0};
    msg.content = content;
    msg.content_len = blen;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_ANNIE";
    ctx.recipient_contact_id_len = strlen("+CONTACT_ANNIE");
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);

    /* SOTA acceptance: REJECT with the cross-contact reason. A future
     * regression that bypasses crosstalk surfaces here as either SEND
     * or a different reason. */
    HU_ASSERT_TRUE(verdict.kind == HU_OUTBOUND_REJECT);
    HU_ASSERT_NOT_NULL(verdict.reason);
    HU_ASSERT_TRUE(strstr(verdict.reason, "crosstalk_other_contact") != NULL);

    hu_outbound_verdict_clear(&verdict, &alloc);
    if (msg.content)
        alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);

    hu_outbound_crosstalk_unregister_sqlite();
    close_test_db(db);
}

/* Counterpart: a clean novel proactive to the same recipient must
 * SEND. Without this, a false-positive crosstalk filter could
 * silently block all legitimate sends. */
static void test_e2e_proactive_pipeline_sends_clean_novel_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_seeded_db();
    hu_outbound_crosstalk_register_sqlite(db);

    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(&alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipe), HU_OK);

    /* Novel content with no Jaccard overlap against any seeded row.
     * Short, single-phrase, generic — should pass shape + echo + persona
     * + crosstalk + moderation. */
    const char *clean = "hope your week is going well";
    size_t clen = strlen(clean);
    char *content = (char *)alloc.alloc(alloc.ctx, clen + 1);
    HU_ASSERT_NOT_NULL(content);
    memcpy(content, clean, clen + 1);

    hu_outbound_message_t msg = {0};
    msg.content = content;
    msg.content_len = clen;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_ANNIE";
    ctx.recipient_contact_id_len = strlen("+CONTACT_ANNIE");
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);

    /* SEND is the only acceptable outcome — any REJECT/REGENERATE here
     * is a false-positive regression. */
    HU_ASSERT_TRUE(verdict.kind == HU_OUTBOUND_SEND);

    hu_outbound_verdict_clear(&verdict, &alloc);
    if (msg.content)
        alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);

    hu_outbound_crosstalk_unregister_sqlite();
    close_test_db(db);
}

/* Degraded-mode contract: when no SQLite lookup is registered, the
 * pipeline must still SEND clean content. The cross-contact check
 * falls to the metadata-pattern check only; other stages still
 * enforce shape / echo / persona / moderation. This proves the
 * degraded path doesn't accidentally promote to REJECT. */
static void test_e2e_proactive_pipeline_sends_in_degraded_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Defensive — clear any lookup another test left registered. */
    hu_outbound_crosstalk_unregister_sqlite();

    hu_outbound_pipeline_t *pipe = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(&alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipe), HU_OK);

    const char *clean = "wanted to say hi";
    size_t clen = strlen(clean);
    char *content = (char *)alloc.alloc(alloc.ctx, clen + 1);
    memcpy(content, clean, clen + 1);

    hu_outbound_message_t msg = {0};
    msg.content = content;
    msg.content_len = clen;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = &alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.recipient_contact_id = "+CONTACT_ANNIE";
    ctx.recipient_contact_id_len = strlen("+CONTACT_ANNIE");
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipe, &msg, &ctx, &verdict), HU_OK);
    HU_ASSERT_TRUE(verdict.kind == HU_OUTBOUND_SEND);

    hu_outbound_verdict_clear(&verdict, &alloc);
    if (msg.content)
        alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipe);
}

void run_outbound_e2e_sota_proof_tests(void) {
    HU_TEST_SUITE("outbound_e2e_sota_proof");
    HU_RUN_TEST(test_e2e_proactive_pipeline_rejects_crosscontact_bleed);
    HU_RUN_TEST(test_e2e_proactive_pipeline_sends_clean_novel_content);
    HU_RUN_TEST(test_e2e_proactive_pipeline_sends_in_degraded_mode);
    /* Defensive — leave global static lookup state clean. */
    hu_outbound_crosstalk_unregister_sqlite();
}
