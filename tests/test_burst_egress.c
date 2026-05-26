/* tests/test_burst_egress.c
 *
 * Sprint 60 — outbound-safety wiring for burst sub-sends.
 *
 * Before this commit, daemon.c sent burst fragments directly via
 * ch->channel->vtable->send, bypassing the outbound pipeline.
 * The crosstalk / persona / shape / moderation stages never ran on
 * any burst fragment. If the LLM hallucinated a cross-contact bleed
 * in any single fragment, it shipped.
 *
 * hu_burst_egress_validate_fragment is the security predicate
 * extracted per security-predicate-extraction.md. It runs each
 * fragment through HU_OUTBOUND_PATH_REACTIVE and returns one of the
 * HU_BURST_EGRESS_* int constants (decoupled from the pipeline enum
 * to sidestep the tag-name collision noted in the header).
 *
 * Gated on HU_ENABLE_SQLITE in CMakeLists.txt — the crosstalk
 * SQLite lookup is SQLite-gated.
 */

#include "test_framework.h"

#include "human/agent/burst_egress.h"
#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/core/allocator.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *test_db_path(void) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_burst_egress_%d.db", (int)getpid());
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
    run_sql_or_fail(db, "CREATE TABLE messages("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "session_id TEXT NOT NULL,"
                        "role TEXT NOT NULL,"
                        "content TEXT NOT NULL,"
                        "created_at TEXT DEFAULT(datetime('now')))");
    return db;
}

static void close_test_db(sqlite3 *db) {
    sqlite3_close(db);
    unlink(test_db_path());
}

/* Clean fragment — no overlap with any seeded contact's content.
 * Helper returns SEND with heap-owned content the caller must free. */
static void test_burst_egress_clean_fragment_sends_with_owned_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_seeded_db();
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_OTHER', 'assistant', 'wholly unrelated message', "
                        "datetime('now'))");
    hu_outbound_crosstalk_register_sqlite(db);

    const char *clean = "yeah on my way";
    char *out = NULL;
    size_t out_len = 0;
    int kind = HU_BURST_EGRESS_REJECT;
    hu_error_t err = hu_burst_egress_validate_fragment(&alloc, "imessage", "+CONTACT_RECIPIENT",
                                                      strlen("+CONTACT_RECIPIENT"), clean,
                                                      strlen(clean), &out, &out_len, &kind);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(kind, HU_BURST_EGRESS_SEND);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_STR_EQ(out, "yeah on my way");

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_outbound_crosstalk_unregister_sqlite();
    close_test_db(db);
}

/* The load-bearing safety contract: a burst fragment that matches a
 * recent message from a DIFFERENT contact triggers REJECT. Before
 * this wiring, daemon.c shipped the bleed straight to the wire. */
static void test_burst_egress_bleed_fragment_rejects(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_seeded_db();
    run_sql_or_fail(db, "INSERT INTO messages(session_id, role, content, created_at) "
                        "VALUES('+CONTACT_MINDY', 'assistant', "
                        "'but boy I am just more lonely now than ever', "
                        "datetime('now'))");
    hu_outbound_crosstalk_register_sqlite(db);

    const char *bleed = "but boy I am just more lonely now than ever";
    char *out = NULL;
    size_t out_len = 0;
    int kind = HU_BURST_EGRESS_SEND;
    hu_error_t err =
        hu_burst_egress_validate_fragment(&alloc, "imessage", "+CONTACT_ANNIE",
                                          strlen("+CONTACT_ANNIE"), bleed, strlen(bleed), &out,
                                          &out_len, &kind);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(kind, HU_BURST_EGRESS_REJECT);
    /* On REJECT, out is freed by the helper. */
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);

    hu_outbound_crosstalk_unregister_sqlite();
    close_test_db(db);
}

/* Channel awareness — slack relaxes some shape rules. Verifies
 * channel_name is plumbed through without crashing. */
static void test_burst_egress_channel_name_plumbed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_outbound_crosstalk_unregister_sqlite();

    const char *clean = "thanks";
    char *out = NULL;
    size_t out_len = 0;
    int kind = HU_BURST_EGRESS_REJECT;
    hu_error_t err = hu_burst_egress_validate_fragment(&alloc, "slack", "+CONTACT_X",
                                                      strlen("+CONTACT_X"), clean, strlen(clean),
                                                      &out, &out_len, &kind);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(kind, HU_BURST_EGRESS_SEND);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "thanks");
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* Argument validation — every NULL required arg returns
 * HU_ERR_INVALID_ARGUMENT without crashing. */
static void test_burst_egress_null_args_return_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    int kind = HU_BURST_EGRESS_SEND;

    HU_ASSERT_EQ(hu_burst_egress_validate_fragment(NULL, "imessage", "+A", 2, "hi", 2, &out,
                                                   &out_len, &kind),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_burst_egress_validate_fragment(&alloc, "imessage", NULL, 0, "hi", 2, &out,
                                                   &out_len, &kind),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_burst_egress_validate_fragment(&alloc, "imessage", "+A", 2, NULL, 0, &out,
                                                   &out_len, &kind),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_burst_egress_validate_fragment(&alloc, "imessage", "+A", 2, "hi", 2, NULL,
                                                   &out_len, &kind),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_burst_egress_validate_fragment(&alloc, "imessage", "+A", 2, "hi", 2, &out,
                                                   NULL, &kind),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_burst_egress_validate_fragment(&alloc, "imessage", "+A", 2, "hi", 2, &out,
                                                   &out_len, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* Empty fragment — pipeline returns SEND (empty content is a no-op
 * the caller can choose to skip). */
static void test_burst_egress_empty_fragment_sends(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_outbound_crosstalk_unregister_sqlite();

    char *out = NULL;
    size_t out_len = 0;
    int kind = HU_BURST_EGRESS_REJECT;
    hu_error_t err = hu_burst_egress_validate_fragment(&alloc, "imessage", "+CONTACT_X",
                                                      strlen("+CONTACT_X"), "", 0, &out, &out_len,
                                                      &kind);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(kind, HU_BURST_EGRESS_SEND);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
}

void run_burst_egress_tests(void) {
    HU_TEST_SUITE("burst_egress");
    HU_RUN_TEST(test_burst_egress_clean_fragment_sends_with_owned_content);
    HU_RUN_TEST(test_burst_egress_bleed_fragment_rejects);
    HU_RUN_TEST(test_burst_egress_channel_name_plumbed);
    HU_RUN_TEST(test_burst_egress_null_args_return_error);
    HU_RUN_TEST(test_burst_egress_empty_fragment_sends);
    hu_outbound_crosstalk_unregister_sqlite();
}
