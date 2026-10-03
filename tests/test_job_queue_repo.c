/* Exercises hu_job_queue_repo_* in src/memory/repos/job_queue_repo_sqlite.c
 * (durable job queue, docs/plans/2026-10-03-durable-job-queue.md §7): a due
 * job is claimed by exactly one pass, an expired lease is re-claimable but
 * fences out the stale claimer, a row in flight at a crash becomes `unknown`
 * and is never claimed again, a duplicate idempotency key is ignored, and old
 * pending rows expire.
 */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/job_queue_repo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

#define CONTACT "+15550000042"

typedef struct jq_fixture {
    hu_allocator_t alloc; /* the memory keeps a pointer to it */
    hu_memory_t mem;
    sqlite3 *db;
} jq_fixture_t;

static void jq_open(jq_fixture_t *f) {
    f->alloc = hu_system_allocator();
    f->mem = hu_sqlite_memory_create(&f->alloc, ":memory:");
    f->db = hu_sqlite_memory_get_db(&f->mem);
}

static void jq_close(jq_fixture_t *f) {
    f->mem.vtable->deinit(f->mem.ctx);
}

static hu_job_spec_t sched_spec(const char *key, int64_t due_at, const char *text) {
    hu_job_spec_t s;
    memset(&s, 0, sizeof(s));
    s.kind = HU_JOB_KIND_SCHED_SEND;
    s.payload = text;
    s.payload_len = text ? strlen(text) : 0;
    s.contact = CONTACT;
    s.channel = "imessage";
    s.due_at = due_at;
    s.idempotency_key = key;
    return s;
}

static int64_t jq_count_state(sqlite3 *db, const char *state) {
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM jobs WHERE state=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, state, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int64_t jq_attempts(sqlite3 *db, int64_t id) {
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT attempts FROM jobs WHERE id=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void test_job_queue_repo_ensure_schema_is_idempotent(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_NOT_NULL(f.db);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_queue_counts_t c;
    memset(&c, 0xff, sizeof(c));
    HU_ASSERT_EQ(hu_job_queue_repo_counts(f.db, &c), HU_OK);
    HU_ASSERT_EQ(c.pending, 0);
    HU_ASSERT_EQ(c.unknown, 0);
    jq_close(&f);
}

static void test_job_queue_repo_claim_is_exclusive(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:a", 1100, "see you at 6");
    hu_job_spec_t b = sched_spec("sched:b", 5000, "later");
    int64_t ida = 0, idb = 0;
    bool ins = false;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 1000, &ida, &ins), HU_OK);
    HU_ASSERT_TRUE(ins);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &b, 1000, &idb, &ins), HU_OK);
    HU_ASSERT_TRUE(ida > 0 && idb > ida);

    static hu_job_t jobs[4];
    size_t n = 99;
    /* Pre: nothing due yet. */
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1050, 120, jobs, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);

    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, HU_JOB_KIND_SCHED_SEND, 1100, 120, jobs, 4, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(jobs[0].id, ida);
    HU_ASSERT_EQ(jobs[0].lease_until, 1220);
    HU_ASSERT_EQ(jobs[0].payload_len, strlen("see you at 6"));
    HU_ASSERT_TRUE(memcmp(jobs[0].payload, "see you at 6", jobs[0].payload_len) == 0);
    HU_ASSERT_STR_EQ(jobs[0].contact, CONTACT);
    HU_ASSERT_STR_EQ(jobs[0].idempotency_key, "sched:a");
    HU_ASSERT_EQ(jq_count_state(f.db, "claimed"), 1);

    /* A second pass inside the lease must not hand it out again. */
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1150, 120, jobs + 1, 3, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);

    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, ida, jobs[0].lease_until, 1160), HU_OK);
    HU_ASSERT_EQ(jq_attempts(f.db, ida), 1);
    HU_ASSERT_EQ(hu_job_queue_repo_finish(f.db, ida, HU_JOB_STATE_DONE, 0, NULL, 1161), HU_OK);
    HU_ASSERT_EQ(jq_count_state(f.db, "done"), 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "pending"), 1);
    jq_close(&f);
}

static void test_job_queue_repo_expired_lease_is_reclaimed_and_fences_stale_claimer(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:lease", 1000, "hi");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, &id, NULL), HU_OK);
    static hu_job_t first[1], second[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1000, 120, first, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    /* Lease still live at 1120: not re-claimable. */
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1120, 120, second, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    /* Lease expired at 1121: a new pass claims it with a new token. */
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1121, 120, second, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(second[0].id, id);
    HU_ASSERT_EQ(second[0].lease_until, 1241);
    /* The stale claimer can neither send nor release. */
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, id, first[0].lease_until, 1122),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_job_queue_repo_release(f.db, id, first[0].lease_until, 0, 1122),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(jq_attempts(f.db, id), 0);
    /* Past its own lease the current claimer cannot start a send either. */
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, id, second[0].lease_until, 1242),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, id, second[0].lease_until, 1200), HU_OK);
    HU_ASSERT_EQ(jq_attempts(f.db, id), 1);
    jq_close(&f);
}

static void test_job_queue_repo_release_returns_to_pending_without_an_attempt(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:defer", 1000, "x");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, &id, NULL), HU_OK);
    static hu_job_t j[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1000, 120, j, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(hu_job_queue_repo_release(f.db, id, j[0].lease_until, 2000, 1001), HU_OK);
    HU_ASSERT_EQ(jq_count_state(f.db, "pending"), 1);
    HU_ASSERT_EQ(jq_attempts(f.db, id), 0);
    /* Rescheduled: not due at 1500, due at 2000. */
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1500, 120, j, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 2000, 120, j, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    jq_close(&f);
}

static void test_job_queue_repo_sending_becomes_unknown_and_is_never_reclaimed(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:crash", 1000, "in flight");
    hu_job_spec_t b = sched_spec("sched:stale-claim", 1000, "never sent");
    int64_t ida = 0, idb = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, &ida, NULL), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &b, 900, &idb, NULL), HU_OK);
    static hu_job_t j[2];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1000, 120, j, 2, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);
    int64_t lease_a = j[0].id == ida ? j[0].lease_until : j[1].lease_until;
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, ida, lease_a, 1001), HU_OK);
    /* The process dies here: a in `sending`, b still claimed. */

    int64_t unknown = -1, requeued = -1;
    HU_ASSERT_EQ(hu_job_queue_repo_recover_on_start(f.db, 5000, &unknown, &requeued), HU_OK);
    HU_ASSERT_EQ(unknown, 1);
    HU_ASSERT_EQ(requeued, 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "unknown"), 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "sending"), 0);

    /* Every later pass hands out b only; a is never claimed again. */
    for (int64_t now = 5000; now < 5000 + 3 * 200; now += 200) {
        HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, now, 120, j, 2, &n), HU_OK);
        for (size_t i = 0; i < n; i++)
            HU_ASSERT_TRUE(j[i].id != ida);
    }
    /* A second restart finds nothing new in flight. */
    HU_ASSERT_EQ(hu_job_queue_repo_recover_on_start(f.db, 9000, &unknown, &requeued), HU_OK);
    HU_ASSERT_EQ(unknown, 0);
    HU_ASSERT_EQ(jq_count_state(f.db, "unknown"), 1);
    HU_ASSERT_EQ(jq_attempts(f.db, ida), 1);
    jq_close(&f);
}

static void test_job_queue_repo_recover_keeps_a_live_lease(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:live-lease", 1000, "x");
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, NULL, NULL), HU_OK);
    static hu_job_t j[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1000, 120, j, 1, &n), HU_OK);
    int64_t unknown = -1, requeued = -1;
    HU_ASSERT_EQ(hu_job_queue_repo_recover_on_start(f.db, 1060, &unknown, &requeued), HU_OK);
    HU_ASSERT_EQ(unknown, 0);
    HU_ASSERT_EQ(requeued, 0);
    HU_ASSERT_EQ(jq_count_state(f.db, "claimed"), 1);
    jq_close(&f);
}

static void test_job_queue_repo_duplicate_key_is_ignored(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("followup:" CONTACT ":77", 1000, "first");
    hu_job_spec_t again = sched_spec("followup:" CONTACT ":77", 2000, "second");
    int64_t id1 = 0, id2 = 0;
    bool ins = false;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, &id1, &ins), HU_OK);
    HU_ASSERT_TRUE(ins);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &again, 901, &id2, &ins), HU_OK);
    HU_ASSERT_FALSE(ins);
    HU_ASSERT_EQ(id2, id1);
    hu_job_queue_counts_t c;
    HU_ASSERT_EQ(hu_job_queue_repo_counts(f.db, &c), HU_OK);
    HU_ASSERT_EQ(c.pending, 1);
    /* The surviving row is the first one, not overwritten. */
    static hu_job_t j[2];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 3000, 120, j, 2, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(j[0].due_at, 1000);
    HU_ASSERT_TRUE(memcmp(j[0].payload, "first", 5) == 0);
    jq_close(&f);
}

static void test_job_queue_repo_expires_old_pending_rows(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t old = sched_spec("hold:1:100", 1000, "old");
    old.kind = HU_JOB_KIND_INBOUND_HOLD;
    hu_job_spec_t fresh = sched_spec("hold:1:101", 1000, "fresh");
    fresh.kind = HU_JOB_KIND_INBOUND_HOLD;
    hu_job_spec_t other = sched_spec("sched:other", 1000, "other kind");
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &old, 1000, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &fresh, 9000, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &other, 1000, NULL, NULL), HU_OK);
    int64_t expired = -1;
    /* created_at < cutoff is strict: cutoff 1000 leaves the row created at 1000. */
    HU_ASSERT_EQ(
        hu_job_queue_repo_expire_older_than(f.db, HU_JOB_KIND_INBOUND_HOLD, 1000, 11800, &expired),
        HU_OK);
    HU_ASSERT_EQ(expired, 0);
    HU_ASSERT_EQ(
        hu_job_queue_repo_expire_older_than(f.db, HU_JOB_KIND_INBOUND_HOLD, 1001, 11801, &expired),
        HU_OK);
    HU_ASSERT_EQ(expired, 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "expired"), 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "pending"), 2); /* fresh hold + other kind */
    /* An expired row is never claimed, and a hold-kind pass skips the due sched row. */
    static hu_job_t j[4];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, HU_JOB_KIND_INBOUND_HOLD, 20000, 120, j, 4, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(j[0].idempotency_key, "hold:1:101");
    jq_close(&f);
}

static void test_job_queue_repo_finish_and_shadow_transitions(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t s = sched_spec("sched:shadow", 1000, "mirror");
    s.shadow = true;
    hu_job_spec_t p = sched_spec("sched:cancel-me", 1000, "x");
    int64_t ids = 0, idp = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &s, 900, &ids, NULL), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &p, 900, &idp, NULL), HU_OK);
    hu_job_queue_counts_t c;
    HU_ASSERT_EQ(hu_job_queue_repo_counts(f.db, &c), HU_OK);
    HU_ASSERT_EQ(c.shadow, 1);
    HU_ASSERT_EQ(c.pending, 1);
    /* done requires sending; a pending row cannot be marked done. */
    HU_ASSERT_EQ(hu_job_queue_repo_finish(f.db, idp, HU_JOB_STATE_DONE, 0, NULL, 950),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(
        hu_job_queue_repo_finish(f.db, idp, HU_JOB_STATE_CANCELED, 0, "owner replied", 950), HU_OK);
    /* Terminal: cannot be canceled twice, and no unknown state names. */
    HU_ASSERT_EQ(hu_job_queue_repo_finish(f.db, idp, HU_JOB_STATE_CANCELED, 0, NULL, 951),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_job_queue_repo_finish(f.db, idp, "sending", 0, NULL, 951),
                 HU_ERR_INVALID_ARGUMENT);
    /* Shadow rows are never claimed. */
    static hu_job_t j[2];
    size_t n = 9;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 5000, 120, j, 2, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(hu_job_queue_repo_counts(f.db, &c), HU_OK);
    HU_ASSERT_EQ(c.canceled, 1);
    HU_ASSERT_EQ(c.shadow, 1);
    jq_close(&f);
}

static int jq_exec_rc(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* The memory.db handle is shared with other threads. Joining a transaction
 * someone else opened would let their ROLLBACK undo a committed `sending`,
 * so the queue refuses instead. */
static void test_job_queue_repo_refuses_inside_an_open_transaction(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:txn", 1000, "x");
    hu_job_spec_t b = sched_spec("sched:txn-b", 1000, "y");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, &id, NULL), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &b, 900, NULL, NULL), HU_OK);
    static hu_job_t j[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1000, 120, j, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(j[0].id, id);

    /* Another subsystem opens a transaction on the same connection. */
    HU_ASSERT_EQ(jq_exec_rc(f.db, "BEGIN;"), SQLITE_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, id, j[0].lease_until, 1001), HU_ERR_IO_BUSY);
    static hu_job_t k[1];
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1001, 120, k, 1, &n), HU_ERR_IO_BUSY);
    HU_ASSERT_EQ(n, 0);
    int64_t unknown = -1, requeued = -1;
    HU_ASSERT_EQ(hu_job_queue_repo_recover_on_start(f.db, 1001, &unknown, &requeued),
                 HU_ERR_IO_BUSY);
    /* Nothing leaked into their transaction: it is still open, and the row is
     * still claimed with no attempt. */
    HU_ASSERT_EQ(sqlite3_get_autocommit(f.db), 0);
    HU_ASSERT_EQ(jq_exec_rc(f.db, "ROLLBACK;"), SQLITE_OK);
    HU_ASSERT_EQ(jq_count_state(f.db, "claimed"), 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "sending"), 0);
    HU_ASSERT_EQ(jq_attempts(f.db, id), 0);
    /* Once the connection is free the same claim proceeds. */
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, id, j[0].lease_until, 1002), HU_OK);
    HU_ASSERT_EQ(jq_count_state(f.db, "sending"), 1);
    jq_close(&f);
}

static void test_job_queue_repo_finish_is_fenced_for_claimed_rows(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t a = sched_spec("sched:fence-finish", 1000, "x");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &a, 900, &id, NULL), HU_OK);
    static hu_job_t stale[1], fresh[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1000, 120, stale, 1, &n), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1121, 120, fresh, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    /* The stale worker cannot fail, cancel or expire the new claim. */
    HU_ASSERT_EQ(
        hu_job_queue_repo_finish(f.db, id, HU_JOB_STATE_FAILED, stale[0].lease_until, "held", 1122),
        HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(
        hu_job_queue_repo_finish(f.db, id, HU_JOB_STATE_CANCELED, stale[0].lease_until, NULL, 1122),
        HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_job_queue_repo_finish(f.db, id, HU_JOB_STATE_EXPIRED, 0, NULL, 1122),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(jq_count_state(f.db, "claimed"), 1);
    /* The current claimer still owns it. */
    HU_ASSERT_EQ(hu_job_queue_repo_mark_sending(f.db, id, fresh[0].lease_until, 1123), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_finish(f.db, id, HU_JOB_STATE_DONE, 0, NULL, 1124), HU_OK);
    HU_ASSERT_EQ(jq_count_state(f.db, "done"), 1);
    jq_close(&f);
}

static void test_job_queue_repo_expiry_of_sched_sends_uses_due_at(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t later = sched_spec("sched:next-week", 50000, "later");
    hu_job_spec_t overdue = sched_spec("sched:overdue", 2000, "late");
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &later, 1000, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &overdue, 1000, NULL, NULL), HU_OK);
    int64_t expired = -1;
    /* Both were created at 1000; only the one due before the cutoff expires. */
    HU_ASSERT_EQ(
        hu_job_queue_repo_expire_older_than(f.db, HU_JOB_KIND_SCHED_SEND, 10000, 10000, &expired),
        HU_OK);
    HU_ASSERT_EQ(expired, 1);
    HU_ASSERT_EQ(jq_count_state(f.db, "pending"), 1);
    /* No kind means no rule for which clock to read: refused. */
    HU_ASSERT_EQ(hu_job_queue_repo_expire_older_than(f.db, NULL, 99999, 99999, &expired),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(jq_count_state(f.db, "pending"), 1);
    jq_close(&f);
}

static void test_job_queue_repo_rejects_bad_arguments(void) {
    jq_fixture_t f;
    jq_open(&f);
    HU_ASSERT_EQ(hu_job_queue_repo_ensure_schema(f.db), HU_OK);
    hu_job_spec_t bad_kind = sched_spec("k1", 1, "x");
    bad_kind.kind = "cron";
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &bad_kind, 1, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_job_spec_t no_key = sched_spec("", 1, "x");
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &no_key, 1, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    static char big[HU_JOB_PAYLOAD_MAX + 1];
    memset(big, 'a', sizeof(big));
    hu_job_spec_t too_big = sched_spec("k2", 1, NULL);
    too_big.payload = big;
    too_big.payload_len = sizeof(big);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(f.db, &too_big, 1, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_job_queue_repo_enqueue(NULL, &no_key, 1, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    static hu_job_t j[1];
    size_t n = 0;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1, 0, j, 1, &n), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(f.db, NULL, 1, 120, j, 0, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(jq_count_state(f.db, "pending"), 0);
    jq_close(&f);
}

void run_job_queue_repo_tests(void) {
    HU_TEST_SUITE("job queue repo");
    HU_RUN_TEST(test_job_queue_repo_ensure_schema_is_idempotent);
    HU_RUN_TEST(test_job_queue_repo_claim_is_exclusive);
    HU_RUN_TEST(test_job_queue_repo_expired_lease_is_reclaimed_and_fences_stale_claimer);
    HU_RUN_TEST(test_job_queue_repo_release_returns_to_pending_without_an_attempt);
    HU_RUN_TEST(test_job_queue_repo_sending_becomes_unknown_and_is_never_reclaimed);
    HU_RUN_TEST(test_job_queue_repo_recover_keeps_a_live_lease);
    HU_RUN_TEST(test_job_queue_repo_duplicate_key_is_ignored);
    HU_RUN_TEST(test_job_queue_repo_expires_old_pending_rows);
    HU_RUN_TEST(test_job_queue_repo_finish_and_shadow_transitions);
    HU_RUN_TEST(test_job_queue_repo_refuses_inside_an_open_transaction);
    HU_RUN_TEST(test_job_queue_repo_finish_is_fenced_for_claimed_rows);
    HU_RUN_TEST(test_job_queue_repo_expiry_of_sched_sends_uses_due_at);
    HU_RUN_TEST(test_job_queue_repo_rejects_bad_arguments);
}
#else
void run_job_queue_repo_tests(void) {}
#endif
