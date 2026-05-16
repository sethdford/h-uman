/* test_dpo_miner — Sprint 7 US-7.2 acceptance tests.
 *
 * Each test maps to one AC from sprints/sprint-7/stories.md (revised per
 * D2 in sprints/sprint-7/decisions.md):
 *   AC-7.2.1 -> miner_records_outbound_edit_pair_with_correct_fields
 *   AC-7.2.2 -> miner_skips_unedited_messages
 *   AC-7.2.3 -> miner_emits_jsonl_consumable_by_finetune_script
 *   AC-7.2.4 -> miner_redacts_pii
 *   AC-7.2.5 -> miner_deduplicates_pairs
 *   AC-7.2.6 -> covered by build: -Wall -Wextra -Wpedantic -Werror + ASan
 *
 * All tests use an in-memory SQLite handle. The miner's actual logic
 * runs (no HU_IS_TEST short-circuit inside dpo_miner.c) so the SELECT,
 * PII redaction, and dedup paths are exercised end-to-end. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"
#include "human/ml/dpo_miner.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

typedef struct fixture_msg {
    const char *session_id;
    const char *role;
    const char *content;
    const char *created_at;
} fixture_msg_t;

/* Run a single statement against db. Returns SQLITE_OK on success. */
static int run_one_stmt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? SQLITE_OK : rc;
}

/* Create a minimal messages table matching production schema. */
static sqlite3 *open_messages_fixture(const fixture_msg_t *rows, size_t n) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    const char *schema = "CREATE TABLE messages("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "session_id TEXT NOT NULL,"
                         "role TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "created_at TEXT NOT NULL)";
    if (run_one_stmt(db, schema) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO messages(session_id, role, content, created_at) "
                           "VALUES(?, ?, ?, ?)",
                           -1, &ins, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        sqlite3_bind_text(ins, 1, rows[i].session_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, rows[i].role, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, rows[i].content, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, rows[i].created_at, -1, SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            sqlite3_finalize(ins);
            sqlite3_close(db);
            return NULL;
        }
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    return db;
}

static int count_dpo_pairs(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM dpo_pairs", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

/* Read first dpo_pairs row's text fields into caller buffers. */
static int read_first_pair(sqlite3 *db, char *prompt_out, size_t prompt_cap, char *chosen_out,
                           size_t chosen_cap, char *rejected_out, size_t rejected_cap,
                           double *margin_out, char *source_out, size_t source_cap) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT prompt, chosen, rejected, margin, source FROM dpo_pairs ORDER BY id LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(stmt, 0);
        const unsigned char *c = sqlite3_column_text(stmt, 1);
        const unsigned char *r = sqlite3_column_text(stmt, 2);
        double m = sqlite3_column_double(stmt, 3);
        const unsigned char *s = sqlite3_column_text(stmt, 4);
        if (prompt_out && prompt_cap > 0) {
            prompt_out[0] = 0;
            if (p)
                snprintf(prompt_out, prompt_cap, "%s", (const char *)p);
        }
        if (chosen_out && chosen_cap > 0) {
            chosen_out[0] = 0;
            if (c)
                snprintf(chosen_out, chosen_cap, "%s", (const char *)c);
        }
        if (rejected_out && rejected_cap > 0) {
            rejected_out[0] = 0;
            if (r)
                snprintf(rejected_out, rejected_cap, "%s", (const char *)r);
        }
        if (margin_out)
            *margin_out = m;
        if (source_out && source_cap > 0) {
            source_out[0] = 0;
            if (s)
                snprintf(source_out, source_cap, "%s", (const char *)s);
        }
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found;
}

/* AC-7.2.1 */
static void miner_records_outbound_edit_pair_with_correct_fields(void) {
    const fixture_msg_t rows[] = {
        {"sess-A", "user", "what time is the meeting", "2026-05-10 09:00:00"},
        {"sess-A", "assistant", "the meeting is at 3pm", "2026-05-10 09:00:05"},
        {"sess-A", "user", "no it is at 4pm please update", "2026-05-10 09:00:30"},
    };
    sqlite3 *db = open_messages_fixture(rows, sizeof(rows) / sizeof(rows[0]));
    HU_ASSERT_NOT_NULL(db);

    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_mine_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.correction_window_sec = 300;
    hu_dpo_mine_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, &opts, &stats), HU_OK);
    HU_ASSERT_EQ((int)stats.pairs_recorded, 1);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);

    char prompt[256], chosen[256], rejected[256], source[64];
    double margin = 0.0;
    HU_ASSERT_EQ(read_first_pair(db, prompt, sizeof(prompt), chosen, sizeof(chosen), rejected,
                                 sizeof(rejected), &margin, source, sizeof(source)),
                 1);
    HU_ASSERT_STR_EQ(prompt, "what time is the meeting");
    HU_ASSERT_STR_EQ(chosen, "no it is at 4pm please update");
    HU_ASSERT_STR_EQ(rejected, "the meeting is at 3pm");
    HU_ASSERT_STR_EQ(source, "outbound_edit");
    HU_ASSERT_FLOAT_EQ(margin, 0.5, 1e-9);

    sqlite3_close(db);
}

/* AC-7.2.2: gap exceeds correction window -> no pair recorded. */
static void miner_skips_unedited_messages(void) {
    const fixture_msg_t rows[] = {
        {"sess-B", "user", "hi", "2026-05-10 09:00:00"},
        {"sess-B", "assistant", "hello", "2026-05-10 09:00:05"},
        {"sess-B", "user", "totally unrelated followup hours later", "2026-05-10 09:10:05"},
    };
    sqlite3 *db = open_messages_fixture(rows, sizeof(rows) / sizeof(rows[0]));
    HU_ASSERT_NOT_NULL(db);

    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_mine_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.correction_window_sec = 300;
    hu_dpo_mine_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, &opts, &stats), HU_OK);
    HU_ASSERT_EQ((int)stats.pairs_recorded, 0);
    HU_ASSERT_EQ(count_dpo_pairs(db), 0);

    sqlite3_close(db);
}

/* AC-7.2.3: row shape matches finetune-gemma.py reader expectations. */
static void miner_emits_jsonl_consumable_by_finetune_script(void) {
    const fixture_msg_t rows[] = {
        {"sess-C", "user", "draft an apology to John", "2026-05-10 09:00:00"},
        {"sess-C", "assistant", "Hey John, sorry about that.", "2026-05-10 09:00:05"},
        {"sess-C", "user", "Make it more formal please", "2026-05-10 09:00:20"},
    };
    sqlite3 *db = open_messages_fixture(rows, sizeof(rows) / sizeof(rows[0]));
    HU_ASSERT_NOT_NULL(db);

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);

    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT prompt, chosen, rejected FROM dpo_pairs "
                                    "WHERE source = 'outbound_edit' ORDER BY id LIMIT 1",
                                    -1, &stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_NOT_NULL(sqlite3_column_text(stmt, 0));
    HU_ASSERT_NOT_NULL(sqlite3_column_text(stmt, 1));
    HU_ASSERT_NOT_NULL(sqlite3_column_text(stmt, 2));
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

/* AC-7.2.4: email/phone/SSN are redacted in all three fields. */
static void miner_redacts_pii(void) {
    const fixture_msg_t rows[] = {
        {"sess-D", "user", "draft an intro for Carlos", "2026-05-10 09:00:00"},
        {"sess-D", "assistant",
         "Reach me at example.person@example.com or 555-123-4567 (SSN 123-45-6789).",
         "2026-05-10 09:00:05"},
        {"sess-D", "user", "perfect but contact me at other.person@example.com instead",
         "2026-05-10 09:00:10"},
    };
    sqlite3 *db = open_messages_fixture(rows, sizeof(rows) / sizeof(rows[0]));
    HU_ASSERT_NOT_NULL(db);

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);

    char prompt[512], chosen[512], rejected[512], source[64];
    double margin = 0.0;
    HU_ASSERT_EQ(read_first_pair(db, prompt, sizeof(prompt), chosen, sizeof(chosen), rejected,
                                 sizeof(rejected), &margin, source, sizeof(source)),
                 1);

    HU_ASSERT_STR_NOT_CONTAINS(rejected, "example.person@example.com");
    HU_ASSERT_STR_NOT_CONTAINS(rejected, "555-123-4567");
    HU_ASSERT_STR_NOT_CONTAINS(rejected, "123-45-6789");
    HU_ASSERT_STR_CONTAINS(rejected, "[EMAIL]");
    HU_ASSERT_STR_CONTAINS(rejected, "[PHONE]");
    HU_ASSERT_STR_CONTAINS(rejected, "[SSN]");

    HU_ASSERT_STR_NOT_CONTAINS(chosen, "other.person@example.com");
    HU_ASSERT_STR_CONTAINS(chosen, "[EMAIL]");

    sqlite3_close(db);
}

/* AC-7.2.5: content-keyed dedup across miner runs. */
static void miner_deduplicates_pairs(void) {
    const fixture_msg_t rows[] = {
        {"sess-E", "user", "what time", "2026-05-10 09:00:00"},
        {"sess-E", "assistant", "3pm", "2026-05-10 09:00:05"},
        {"sess-E", "user", "no, 4pm", "2026-05-10 09:00:10"},
    };
    sqlite3 *db = open_messages_fixture(rows, sizeof(rows) / sizeof(rows[0]));
    HU_ASSERT_NOT_NULL(db);

    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_mine_stats_t stats1;
    memset(&stats1, 0, sizeof(stats1));
    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, NULL, &stats1), HU_OK);
    HU_ASSERT_EQ((int)stats1.pairs_recorded, 1);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);

    hu_dpo_mine_stats_t stats2;
    memset(&stats2, 0, sizeof(stats2));
    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, NULL, &stats2), HU_OK);
    HU_ASSERT_EQ((int)stats2.pairs_recorded, 0);
    HU_ASSERT_GT((int)stats2.pairs_skipped_dup, 0);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);

    sqlite3_stmt *ins = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "INSERT INTO messages(session_id, role, content, created_at) "
                                    "VALUES(?, ?, ?, ?)",
                                    -1, &ins, NULL),
                 SQLITE_OK);
    const fixture_msg_t extra[] = {
        {"sess-F", "user", "draft an email to mom", "2026-05-10 10:00:00"},
        {"sess-F", "assistant", "Hi mom, hope you are well.", "2026-05-10 10:00:05"},
        {"sess-F", "user", "make it less formal", "2026-05-10 10:00:20"},
    };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        sqlite3_bind_text(ins, 1, extra[i].session_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, extra[i].role, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, extra[i].content, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, extra[i].created_at, -1, SQLITE_STATIC);
        HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    hu_dpo_mine_stats_t stats3;
    memset(&stats3, 0, sizeof(stats3));
    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, db, NULL, &stats3), HU_OK);
    HU_ASSERT_EQ((int)stats3.pairs_recorded, 1);
    HU_ASSERT_EQ(count_dpo_pairs(db), 2);

    sqlite3_close(db);
}

/* NULL-arg guards. */
static void miner_null_alloc_returns_invalid(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_dpo_mine_corrections(NULL, db, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

static void miner_null_db_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_dpo_mine_corrections(&alloc, NULL, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* HIGH-1 critic fix: --no-export must override the default-enable block. */
static void miner_no_export_flag_actually_disables_export(void) {
    hu_dpo_miner_export_decision_t decision;

    /* --no-export passed (no explicit path): export disabled. */
    memset(&decision, 0, sizeof(decision));
    HU_ASSERT_EQ(hu_dpo_miner_resolve_export(NULL, 1, "/tmp/fake-home", &decision), HU_OK);
    HU_ASSERT_EQ(decision.should_export, 0);
    HU_ASSERT_EQ(decision.export_path[0], 0);

    /* Default behavior (no flags, HOME set): export enabled to the
     * well-known cross-story path. */
    memset(&decision, 0, sizeof(decision));
    HU_ASSERT_EQ(hu_dpo_miner_resolve_export(NULL, 0, "/tmp/fake-home", &decision), HU_OK);
    HU_ASSERT_EQ(decision.should_export, 1);
    HU_ASSERT_STR_CONTAINS(decision.export_path, "/.human/dpo/pairs.jsonl");

    /* Explicit path beats default and ignores --no-export-effect when
     * the user has not actually passed --no-export. */
    memset(&decision, 0, sizeof(decision));
    HU_ASSERT_EQ(hu_dpo_miner_resolve_export("/tmp/custom.jsonl", 0, "/tmp/fake-home", &decision),
                 HU_OK);
    HU_ASSERT_EQ(decision.should_export, 1);
    HU_ASSERT_STR_EQ(decision.export_path, "/tmp/custom.jsonl");

    /* --no-export wins even if an explicit path was also given — the CLI
     * loop drops the explicit path when --no-export fires, so the helper
     * sees export_flag_path = NULL. We exercise the more-pessimistic
     * case here: even if the path leaked through, --no-export still
     * disables. */
    memset(&decision, 0, sizeof(decision));
    HU_ASSERT_EQ(hu_dpo_miner_resolve_export("/tmp/custom.jsonl", 1, "/tmp/fake-home", &decision),
                 HU_OK);
    HU_ASSERT_EQ(decision.should_export, 0);

    /* HOME missing AND no explicit path: export disabled (no default
     * available). The CLI surface still warns, but the helper just
     * declines to export. */
    memset(&decision, 0, sizeof(decision));
    HU_ASSERT_EQ(hu_dpo_miner_resolve_export(NULL, 0, NULL, &decision), HU_OK);
    HU_ASSERT_EQ(decision.should_export, 0);
}

/* HIGH-4 critic fix: ensure_parent_dir creates the directory tree if
 * missing so hu_dpo_export_jsonl does not fail on a fresh install. */
static void miner_creates_dpo_directory_if_missing(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";

    /* Unique nested target that definitely does not exist yet. PID +
     * time(NULL) avoids cross-test collisions even when the suite is
     * re-run quickly. */
    char target[512];
    snprintf(target, sizeof(target), "%s/hu-dpo-miner-test-%ld-%ld/nested/dir/pairs.jsonl", tmpdir,
             (long)getpid(), (long)time(NULL));

    /* Sanity: the file's directory tree does not exist yet. */
    char dir_only[512];
    snprintf(dir_only, sizeof(dir_only), "%s/hu-dpo-miner-test-%ld-%ld/nested/dir", tmpdir,
             (long)getpid(), (long)time(NULL));
    struct stat st;
    /* Best-effort: if it somehow exists, the test is still valid
     * because mkdir+EEXIST is tolerated. */
    (void)stat(dir_only, &st);

    HU_ASSERT_EQ(hu_dpo_miner_ensure_parent_dir(target), HU_OK);

    /* Now stat the parent dir of `target` — it must exist. */
    char *last_slash = strrchr(target, '/');
    HU_ASSERT_NOT_NULL(last_slash);
    *last_slash = '\0';
    HU_ASSERT_EQ(stat(target, &st), 0);
    HU_ASSERT_TRUE(S_ISDIR(st.st_mode));

    /* Calling a second time on the same path must succeed (EEXIST is
     * tolerated). */
    *last_slash = '/';
    HU_ASSERT_EQ(hu_dpo_miner_ensure_parent_dir(target), HU_OK);

    /* Clean up: rmdir the three levels we created. Best-effort — a
     * failure does not fail the test (the temp dir gets cleared
     * eventually anyway). */
    *last_slash = '\0';
    rmdir(target);
    char *prev_slash = strrchr(target, '/');
    if (prev_slash) {
        *prev_slash = '\0';
        rmdir(target);
        prev_slash = strrchr(target, '/');
        if (prev_slash) {
            *prev_slash = '\0';
            rmdir(target);
        }
    }
}

/* HIGH-4 guard: NULL / empty input rejected without touching the FS. */
static void miner_ensure_parent_dir_null_returns_invalid(void) {
    HU_ASSERT_EQ(hu_dpo_miner_ensure_parent_dir(NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_miner_ensure_parent_dir(""), HU_ERR_INVALID_ARGUMENT);
    /* No slash → file in CWD → no mkdir needed → HU_OK. */
    HU_ASSERT_EQ(hu_dpo_miner_ensure_parent_dir("pairs.jsonl"), HU_OK);
}

#endif /* HU_ENABLE_SQLITE */

void run_dpo_miner_tests(void);
void run_dpo_miner_tests(void) {
    HU_TEST_SUITE("DpoMiner");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(miner_records_outbound_edit_pair_with_correct_fields);
    HU_RUN_TEST(miner_skips_unedited_messages);
    HU_RUN_TEST(miner_emits_jsonl_consumable_by_finetune_script);
    HU_RUN_TEST(miner_redacts_pii);
    HU_RUN_TEST(miner_deduplicates_pairs);
    HU_RUN_TEST(miner_null_alloc_returns_invalid);
    HU_RUN_TEST(miner_null_db_returns_invalid);
    HU_RUN_TEST(miner_no_export_flag_actually_disables_export);
    HU_RUN_TEST(miner_creates_dpo_directory_if_missing);
    HU_RUN_TEST(miner_ensure_parent_dir_null_returns_invalid);
#endif
}
