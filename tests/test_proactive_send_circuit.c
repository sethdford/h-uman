/* Exercises hu_proactive_decisions_repo_consecutive_send_failures in
 * src/memory/repos/proactive_decisions_repo_sqlite.c — the read the proactive
 * circuit breaker gates on.
 *
 * @covers-none — same basename-stripping collision documented in
 * tests/test_proactive_decisions_repo.c ("proactive_decisions_repo" ->
 * "proactive" -> the unrelated src/agent/proactive.c). Production file named above.
 *
 * WHY THIS EXISTS (measured 2026-09-22): the proactive proposer fired 124
 * check-ins at +1801xxx8303 over 30 days and delivered ZERO — that contact is
 * RCS/Android and the send path forces iMessage. A failed send deliberately
 * does not record send-recency (a message nobody received must not suppress a
 * later real one), so nothing ever suppressed the retry: ~10 attempts/day,
 * forever, burning 91% of all proactive capacity on an address that cannot
 * receive. The breaker gives a permanently-failing contact the negative
 * feedback that a successful send's recency record would otherwise provide.
 */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/proactive_decisions_repo.h"
#include "test_framework.h"
#include <sqlite3.h>

#define OUTCOME "proactive_send"
#define WHO     "+15551230001"

/* Shared fixture: an in-memory store with the decision schema ready. Extracted
 * because seven near-identical open/ensure-schema preambles tripped the clone
 * ratchet — and the duplication was real, not an artifact. */
static sqlite3 *circuit_fixture_open(hu_allocator_t *alloc, hu_memory_t *mem) {
    *alloc = hu_system_allocator();
    *mem = hu_sqlite_memory_create(alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem->ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);
    return db;
}

static void fail_row(sqlite3 *db, int64_t ts, const char *who) {
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(
                     db, ts, who, OUTCOME, HU_PROACTIVE_DECISION_DECLINE, "send_failed", 0, NULL),
                 HU_OK);
}
static void ok_row(sqlite3 *db, int64_t ts, const char *who) {
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, ts, who, OUTCOME,
                                                    HU_PROACTIVE_DECISION_SEND, NULL, 1, NULL),
                 HU_OK);
}

static void test_circuit_counts_consecutive_failures(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);

    int64_t n = -1;
    /* Pre: a contact we have never tried has an intact circuit. */
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, WHO, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);

    fail_row(db, 1000, WHO);
    fail_row(db, 2000, WHO);
    fail_row(db, 3000, WHO);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, WHO, &n), HU_OK);
    HU_ASSERT_EQ(n, 3);

    mem.vtable->deinit(mem.ctx);
}

static void test_circuit_resets_after_a_delivery(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);

    int64_t n = -1;
    fail_row(db, 1000, WHO);
    fail_row(db, 2000, WHO);
    /* A delivery closes the circuit: the contact is reachable again. */
    ok_row(db, 3000, WHO);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, WHO, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);

    /* Only failures SINCE that delivery count — the older two stay discounted. */
    fail_row(db, 4000, WHO);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, WHO, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);

    mem.vtable->deinit(mem.ctx);
}

static void test_circuit_is_per_contact(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);

    int64_t n = -1;
    fail_row(db, 1000, WHO);
    fail_row(db, 2000, WHO);
    /* One unreachable contact must not open the circuit for everyone else —
     * this is the whole failure mode: 91% of capacity spent on one address. */
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, "+15559990002", &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 0);

    mem.vtable->deinit(mem.ctx);
}

static void test_circuit_ignores_non_delivery_declines(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);

    int64_t n = -1;
    /* send_cap is a rate limit working correctly, and llm_skip is the model
     * choosing silence. Neither is evidence the contact is unreachable, so
     * neither may trip the breaker. Measured 2026-09-22: 11 of the 31 outcome
     * declines were send_cap — counting those would open circuits on healthy
     * contacts purely for being popular. */
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(
                     db, 1000, WHO, OUTCOME, HU_PROACTIVE_DECISION_DECLINE, "send_cap", 0, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(
                     db, 2000, WHO, OUTCOME, HU_PROACTIVE_DECISION_DECLINE, "llm_skip", 0, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, WHO, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);

    mem.vtable->deinit(mem.ctx);
}

static void test_circuit_rejects_bad_args(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);
    int64_t n = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(NULL, WHO, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, NULL, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_consecutive_send_failures(db, WHO, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

static void test_circuit_opens_only_at_the_threshold(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);

    const int64_t DAY = 86400;
    /* Below threshold: a couple of failures must NOT silence a contact —
     * transient channel errors happen and are not evidence of unreachability. */
    fail_row(db, 1000, WHO);
    HU_ASSERT_TRUE(!hu_proactive_send_circuit_is_open(db, WHO, 1000 + 60));
    fail_row(db, 2000, WHO);
    HU_ASSERT_TRUE(!hu_proactive_send_circuit_is_open(db, WHO, 2000 + 60));

    /* At the threshold (5 undelivered), the circuit opens. */
    fail_row(db, 3000, WHO);
    fail_row(db, 4000, WHO);
    fail_row(db, 5000, WHO);
    HU_ASSERT_TRUE(hu_proactive_send_circuit_is_open(db, WHO, 5000 + 60));

    /* HALF-OPEN: once the cooldown has elapsed since the last failure, exactly
     * one probe is allowed through. Without this the circuit wedges shut
     * forever — a delivery can never reset it if we never attempt one, so
     * fixing the contact's routing later would not bring them back. */
    HU_ASSERT_TRUE(!hu_proactive_send_circuit_is_open(db, WHO, 5000 + DAY + 1));

    /* A failed probe re-opens it for another full cooldown window. */
    fail_row(db, 5000 + DAY + 2, WHO);
    HU_ASSERT_TRUE(hu_proactive_send_circuit_is_open(db, WHO, 5000 + DAY + 60));

    /* A delivery closes it outright, cooldown irrelevant. */
    ok_row(db, 5000 + DAY + 100, WHO);
    HU_ASSERT_TRUE(!hu_proactive_send_circuit_is_open(db, WHO, 5000 + DAY + 101));

    mem.vtable->deinit(mem.ctx);
}

static void test_circuit_open_is_per_contact(void) {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db = circuit_fixture_open(&alloc, &mem);

    for (int i = 1; i <= 5; i++)
        fail_row(db, 1000 * i, WHO);
    HU_ASSERT_TRUE(hu_proactive_send_circuit_is_open(db, WHO, 6000));
    /* The reachable contacts keep their capacity. */
    HU_ASSERT_TRUE(!hu_proactive_send_circuit_is_open(db, "+15559990002", 6000));

    mem.vtable->deinit(mem.ctx);
}

void run_proactive_send_circuit_tests(void) {
    HU_RUN_TEST(test_circuit_counts_consecutive_failures);
    HU_RUN_TEST(test_circuit_resets_after_a_delivery);
    HU_RUN_TEST(test_circuit_is_per_contact);
    HU_RUN_TEST(test_circuit_ignores_non_delivery_declines);
    HU_RUN_TEST(test_circuit_rejects_bad_args);
    HU_RUN_TEST(test_circuit_opens_only_at_the_threshold);
    HU_RUN_TEST(test_circuit_open_is_per_contact);
}
#else
void run_proactive_send_circuit_tests(void) {}
#endif
