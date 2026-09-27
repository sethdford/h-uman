/* Pins hu_daemon_proactive_record_decline — the attribution added 2026-09-20 so
 * a proactive check-in suppressed BEFORE any send attempt leaves a row saying
 * which gate suppressed it.
 *
 * Why this matters: scripts/eval_when_to_speak.py measures the FIR/MIR
 * calibration of the proactive policy and reported fir_dropped_pre_send=89
 * against 10 delivered sends. Those 89 FIRED proposals died in gates between
 * daemon.c:1573 and :1666 with no row naming the gate — visible only in the
 * service log, invisible to the metric. Without attribution the eval cannot
 * separate "the policy correctly stayed quiet" from "a rate-limiter ate it".
 *
 * Assertions are deliberately NON-VACUOUS (see tests-that-pin-bugs.md): each
 * asserts the row's FIELD VALUES, not merely that a row exists — a recorder
 * that wrote decision='send' or dropped the reason would pass a count check. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent.h"
#include "human/channel.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/proactive_decisions_repo.h"
#include <sqlite3.h>
#include <string.h>

/* Reads back the single row's text columns so assertions can be about VALUES. */
static int decline_row_matches(sqlite3 *db, const char *contact, const char *trigger,
                               const char *decision, const char *reason, int sent) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT contact, trigger, decision, reason, sent "
                      "FROM proactive_decisions ORDER BY id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *c = sqlite3_column_text(st, 0);
        const unsigned char *t = sqlite3_column_text(st, 1);
        const unsigned char *d = sqlite3_column_text(st, 2);
        const unsigned char *r = sqlite3_column_text(st, 3);
        int s = sqlite3_column_int(st, 4);
        ok = c && t && d && r && strcmp((const char *)c, contact) == 0 &&
             strcmp((const char *)t, trigger) == 0 && strcmp((const char *)d, decision) == 0 &&
             strcmp((const char *)r, reason) == 0 && s == sent;
    }
    sqlite3_finalize(st);
    return ok;
}

static int64_t decline_row_count(sqlite3 *db) {
    int64_t n = -1;
    (void)hu_proactive_decisions_repo_count(db, &n);
    return n;
}

static void test_record_decline_attributes_the_suppressing_gate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.memory = &mem;

    /* Precondition: the table is empty, so a post-assert cannot pass vacuously. */
    HU_ASSERT_EQ(decline_row_count(db), 0);

    hu_daemon_proactive_record_decline(&agent, "+15555550100", "rate_limited", 1789000000);

    HU_ASSERT_EQ(decline_row_count(db), 1);
    /* The row must say WHICH gate, as a decline, on the send-outcome trigger,
     * and must NOT claim delivery. */
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550100", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "rate_limited",
                                       /*sent=*/0));
    mem.vtable->deinit(mem.ctx);
}

static void test_record_decline_distinguishes_two_gates(void) {
    /* Two different gates must produce two distinguishable rows — a recorder
     * that hardcoded one reason would pass the single-row test above. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.memory = &mem;

    hu_daemon_proactive_record_decline(&agent, "+15555550100", "sanitize_refused", 1789000001);
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550100", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "sanitize_refused", 0));

    hu_daemon_proactive_record_decline(&agent, "+15555550199", "send_cap", 1789000002);
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550199", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "send_cap", 0));

    HU_ASSERT_EQ(decline_row_count(db), 2);
    mem.vtable->deinit(mem.ctx);
}

static void test_record_decline_writes_nothing_on_null_inputs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.memory = &mem;

    hu_daemon_proactive_record_decline(NULL, "+1555", "rate_limited", 1789000000);
    hu_daemon_proactive_record_decline(&agent, NULL, "rate_limited", 1789000000);
    hu_daemon_proactive_record_decline(&agent, "+1555", NULL, 1789000000);

    /* Telemetry must never invent a row it cannot describe. */
    HU_ASSERT_EQ(decline_row_count(db), 0);
    mem.vtable->deinit(mem.ctx);
}

/* Drives the carved gate chain itself, not just the recorder. The proactive
 * tick is compiled out under HU_IS_TEST (daemon_housekeeping.c:73), so nothing
 * else in the suite reaches hu_daemon_proactive_gate_and_send. The LLM "SKIP"
 * path is fully determined: it short-circuits before the channel, governor and
 * throttle are touched, so those can be inert. A build that dropped the
 * recorder call, wrote the length back wrong, or reported sent=true would
 * fail this. */
static void test_gate_and_send_llm_skip_records_reason_and_sends_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(decline_row_count(db), 0);

    struct hu_agent agent = {0};
    agent.memory = &mem;
    hu_contact_profile_t cp = {0};
    cp.contact_id = "+15555550100";
    hu_channel_vtable_t vt = {0}; /* .send NULL — must never be reached */
    hu_channel_t chan = {.ctx = (void *)"imessage", .vtable = &vt};
    hu_proactive_budget_t budget = {0};
    char response[8] = "SKIP";
    size_t response_len = 4;

    bool sent = hu_daemon_proactive_gate_and_send(
        &agent, &alloc, &chan, &cp, "imessage", "+15555550100", 12, response, &response_len,
        1789000000, &budget, /*ar_cfg=*/NULL, /*tz_offset_s=*/0, /*throttle=*/NULL);

    HU_ASSERT_FALSE(sent);
    HU_ASSERT_EQ(response_len, 4); /* written back, unchanged: nothing mutated it */
    HU_ASSERT_EQ(decline_row_count(db), 1);
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550100", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "llm_skip", 0));
    mem.vtable->deinit(mem.ctx);
}

/* The circuit breaker, driven through the real gate chain rather than called
 * directly — this is the wiring proof. The draft is a REAL message, not "SKIP",
 * so the llm_skip short-circuit cannot be what stops it: only the breaker can.
 * The channel vtable has .send = NULL, so if the breaker failed to fire and the
 * chain reached the send, the contract below (no send, one send_circuit_open
 * row) would not hold.
 *
 * Pins the 2026-09-22 failure: 5 undelivered sends to a contact must stop the
 * proposer, and a delivery must let it resume. */
static void test_gate_and_send_open_circuit_skips_and_attributes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    const char *who = "+15555550177";
    /* Five sends that never reached the contact — the shape of an unreachable
     * address, not a policy decline. */
    for (int i = 1; i <= 5; i++)
        HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, 1789000000 + i, who, "proactive_send",
                                                        HU_PROACTIVE_DECISION_DECLINE,
                                                        "send_failed", 0, NULL),
                     HU_OK);
    int64_t before = decline_row_count(db);

    struct hu_agent agent = {0};
    agent.memory = &mem;
    hu_contact_profile_t cp = {0};
    cp.contact_id = who;
    hu_channel_vtable_t vt = {0}; /* .send NULL — reaching it would be the bug */
    hu_channel_t chan = {.ctx = (void *)"imessage", .vtable = &vt};
    hu_proactive_budget_t budget = {0};
    char response[64] = "hey, you around this weekend?";
    size_t response_len = 29;

    bool sent =
        hu_daemon_proactive_gate_and_send(&agent, &alloc, &chan, &cp, "imessage", who, 12, response,
                                          &response_len, 1789000010, &budget, NULL, 0, NULL);

    HU_ASSERT_FALSE(sent);
    HU_ASSERT_EQ(decline_row_count(db), before + 1);
    HU_ASSERT_TRUE(decline_row_matches(db, who, "proactive_send", HU_PROACTIVE_DECISION_DECLINE,
                                       "send_circuit_open", 0));

    /* And it is not a permanent ban: once the contact actually receives one,
     * the breaker closes and the next proposal is gated by policy again
     * (governor, with a zeroed budget) rather than by the circuit. */
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, 1789000020, who, "proactive_send",
                                                    HU_PROACTIVE_DECISION_SEND, NULL, 1, NULL),
                 HU_OK);
    response_len = 29;
    (void)hu_daemon_proactive_gate_and_send(&agent, &alloc, &chan, &cp, "imessage", who, 12,
                                            response, &response_len, 1789000030, &budget, NULL, 0,
                                            NULL);
    HU_ASSERT_TRUE(!decline_row_matches(db, who, "proactive_send", HU_PROACTIVE_DECISION_DECLINE,
                                        "send_circuit_open", 1789000030));
    mem.vtable->deinit(mem.ctx);
}

/* The vtable check is the one condition in the chain that is NOT a policy
 * gate: a channel with no send entry point cannot deliver, but nothing
 * DECIDED against the proposal. Recording a decline there would teach
 * eval_when_to_speak.py that a gate fired when none did, so the contract is
 * sent=false with NO proactive_decisions row and the draft left untouched
 * (the mutating validator/complexity block is inside the vtable branch).
 *
 * Reaching the check means gates 1-3 must all PASS, which pins their pass
 * conditions too. The load-bearing one is the budget: a zeroed
 * hu_proactive_budget_t is EXHAUSTED (governor.c: weekly_used 0 < weekly_max
 * 0 is false), so it would fail on the governor gate and write a
 * "governor_gated" row — the row-count assert below catches that
 * misconfiguration rather than passing sent=false for the wrong reason.
 * ar_cfg=NULL is "quiet hours opted out"; the boundary repo creates its own
 * schema so a fresh memory has no boundary; an empty recency ring has
 * nothing recent. */
static void test_gate_and_send_missing_send_vtable_is_not_a_policy_drop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(decline_row_count(db), 0);

    struct hu_agent agent = {0};
    agent.memory = &mem;
    hu_contact_profile_t cp = {0};
    cp.contact_id = "+15555550100";
    hu_channel_vtable_t vt = {0}; /* .send NULL — the condition under test */
    hu_channel_t chan = {.ctx = (void *)"imessage", .vtable = &vt};
    hu_proactive_budget_t budget = {0};
    budget.daily_max = 3;
    budget.weekly_max = 10;
    budget.relationship_multiplier = 1.0;
    char response[16] = "hey there";
    size_t response_len = 9;

    bool sent = hu_daemon_proactive_gate_and_send(
        &agent, &alloc, &chan, &cp, "imessage", "+15555550100", 12, response, &response_len,
        1789000000, &budget, /*ar_cfg=*/NULL, /*tz_offset_s=*/0, /*throttle=*/NULL);

    HU_ASSERT_FALSE(sent);
    HU_ASSERT_EQ(response_len, 9);
    HU_ASSERT_TRUE(memcmp(response, "hey there", 9) == 0);
    /* No gate fired, so no gate may be blamed. */
    HU_ASSERT_EQ(decline_row_count(db), 0);
    mem.vtable->deinit(mem.ctx);
}

void run_daemon_proactive_decline_tests(void) {
    HU_TEST_SUITE("daemon_proactive_decline");
    HU_RUN_TEST(test_record_decline_attributes_the_suppressing_gate);
    HU_RUN_TEST(test_record_decline_distinguishes_two_gates);
    HU_RUN_TEST(test_record_decline_writes_nothing_on_null_inputs);
    HU_RUN_TEST(test_gate_and_send_llm_skip_records_reason_and_sends_nothing);
    HU_RUN_TEST(test_gate_and_send_missing_send_vtable_is_not_a_policy_drop);
    HU_RUN_TEST(test_gate_and_send_open_circuit_skips_and_attributes);
}

#else

void run_daemon_proactive_decline_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
