/* FU-1 integration tests: prove the contact_send_recency module is wired
 * into the daemon's reactive-priority gate.
 *
 * The daemon records HU_SEND_PATH_REACTIVE on every reactive batch reply and
 * the four proactive paths (F25 emotional check-in, scheduler delivery,
 * proactive check-in, photo album) consult
 * hu_daemon_proactive_should_defer() before sending.  This test exercises the
 * predicate at the integration boundary — the same predicate the daemon
 * calls, applied to the same field on hu_agent_t the daemon mutates.
 *
 * Plan reference: docs/plans/2026-05-15-memory-scoping-followups.md FU-1.
 */

#include "human/agent.h"
#include "human/contact_send_recency.h"
#include "test_framework.h"

#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void zero_agent(hu_agent_t *a) {
    memset(a, 0, sizeof(*a));
}

/* ── agent struct holds the recency field (compile-time wiring) ───────────── */

static void agent_struct_embeds_contact_send_recency_field(void) {
    hu_agent_t a;
    zero_agent(&a);
    /* The field exists and is zero-initialised by memset; recording into it
     * and reading back proves it's a live, addressable member — not a stub. */
    hu_contact_send_recency_record(&a.contact_send_recency, "+15551234567", 12, 1700000000LL,
                                   HU_SEND_PATH_REACTIVE);
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&a.contact_send_recency, "+15551234567", 12),
                 1700000000LL);
}

/* ── daemon predicate: defers when reactive fired within window ───────────── */

static void daemon_should_defer_when_reactive_within_window(void) {
    hu_agent_t a;
    zero_agent(&a);

    int64_t reactive_ts = 1700000000LL;
    hu_contact_send_recency_record(&a.contact_send_recency, "mindy", 5, reactive_ts,
                                   HU_SEND_PATH_REACTIVE);

    /* 30s after the reactive send, well inside HU_DAEMON_REACTIVE_GATE_WINDOW_S
     * (60s) — proactive paths must defer. */
    int64_t now = reactive_ts + 30;
    HU_ASSERT_TRUE(hu_daemon_proactive_should_defer(&a.contact_send_recency, "mindy", 5, now));
}

static void daemon_should_not_defer_outside_window(void) {
    hu_agent_t a;
    zero_agent(&a);

    int64_t reactive_ts = 1700000000LL;
    hu_contact_send_recency_record(&a.contact_send_recency, "mindy", 5, reactive_ts,
                                   HU_SEND_PATH_REACTIVE);

    /* 120s after the reactive send, well past the 60s gate — proactive
     * paths must proceed (gate has expired). */
    int64_t now = reactive_ts + 120;
    HU_ASSERT_FALSE(hu_daemon_proactive_should_defer(&a.contact_send_recency, "mindy", 5, now));
}

/* The gate is REACTIVE-only: a proactive record alone must not cause future
 * proactive sends to defer (otherwise photo+morning would lock each other
 * out and the gate would prevent legitimate proactive flows). */
static void daemon_should_not_defer_when_only_proactive_recorded(void) {
    hu_agent_t a;
    zero_agent(&a);

    int64_t proactive_ts = 1700000000LL;
    hu_contact_send_recency_record(&a.contact_send_recency, "mindy", 5, proactive_ts,
                                   HU_SEND_PATH_PROACTIVE);

    int64_t now = proactive_ts + 10;
    HU_ASSERT_FALSE(hu_daemon_proactive_should_defer(&a.contact_send_recency, "mindy", 5, now));
}

static void daemon_should_not_defer_for_unknown_contact(void) {
    hu_agent_t a;
    zero_agent(&a);

    int64_t reactive_ts = 1700000000LL;
    hu_contact_send_recency_record(&a.contact_send_recency, "alice", 5, reactive_ts,
                                   HU_SEND_PATH_REACTIVE);

    /* Different contact — must not defer. */
    int64_t now = reactive_ts + 30;
    HU_ASSERT_FALSE(hu_daemon_proactive_should_defer(&a.contact_send_recency, "bob", 3, now));
}

static void daemon_should_not_defer_for_fresh_agent(void) {
    hu_agent_t a;
    zero_agent(&a);

    /* No reactive sends recorded — predicate must return false so the
     * very first proactive flow on a fresh daemon is not blocked. */
    HU_ASSERT_FALSE(
        hu_daemon_proactive_should_defer(&a.contact_send_recency, "anyone", 6, 1700000000LL));
}

/* The contract the daemon depends on: a reactive RECORD followed by a
 * proactive QUERY at +59s defers, at +60s does NOT defer.  This pins the
 * exact boundary HU_DAEMON_REACTIVE_GATE_WINDOW_S = 60. */
static void daemon_gate_boundary_is_exact_at_window_seconds(void) {
    hu_agent_t a;
    zero_agent(&a);

    int64_t reactive_ts = 1700000000LL;
    hu_contact_send_recency_record(&a.contact_send_recency, "mindy", 5, reactive_ts,
                                   HU_SEND_PATH_REACTIVE);

    HU_ASSERT_TRUE(
        hu_daemon_proactive_should_defer(&a.contact_send_recency, "mindy", 5, reactive_ts + 59));
    /* At exactly window_s the predicate returns false (strict <). */
    HU_ASSERT_FALSE(hu_daemon_proactive_should_defer(
        &a.contact_send_recency, "mindy", 5, reactive_ts + HU_DAEMON_REACTIVE_GATE_WINDOW_S));
}

/* End-to-end flow: simulate the daemon recording a reactive send, then a
 * subsequent proactive path consulting the gate.  This is the integration
 * the daemon performs in src/daemon.c — record on reactive turn end,
 * check on every proactive entry point. */
static void daemon_record_then_check_flow_blocks_proactive_within_window(void) {
    hu_agent_t a;
    zero_agent(&a);

    const char *contact = "+15551234567";
    size_t cid_len = strlen(contact);
    int64_t reactive_send_ts = 1700000000LL;

    /* Daemon does this at the end of the reactive batch loop (~src/daemon.c
     * line 10577): record HU_SEND_PATH_REACTIVE keyed by batch_key. */
    hu_contact_send_recency_record(&a.contact_send_recency, contact, cid_len, reactive_send_ts,
                                   HU_SEND_PATH_REACTIVE);

    /* Daemon does this at every proactive entry point (F25, scheduler,
     * proactive check-in, photo album) — query before sending. */
    int64_t proactive_attempt_ts = reactive_send_ts + 15;
    HU_ASSERT_TRUE(hu_daemon_proactive_should_defer(&a.contact_send_recency, contact, cid_len,
                                                    proactive_attempt_ts));

    /* After the window closes the same proactive path must succeed. */
    int64_t later_ts = reactive_send_ts + HU_DAEMON_REACTIVE_GATE_WINDOW_S + 5;
    HU_ASSERT_FALSE(
        hu_daemon_proactive_should_defer(&a.contact_send_recency, contact, cid_len, later_ts));
}

/* ── runner ──────────────────────────────────────────────────────────────── */

void run_contact_send_recency_daemon_tests(void);

void run_contact_send_recency_daemon_tests(void) {
    HU_TEST_SUITE("Contact Send Recency — Daemon Integration (FU-1)");

    HU_RUN_TEST(agent_struct_embeds_contact_send_recency_field);
    HU_RUN_TEST(daemon_should_defer_when_reactive_within_window);
    HU_RUN_TEST(daemon_should_not_defer_outside_window);
    HU_RUN_TEST(daemon_should_not_defer_when_only_proactive_recorded);
    HU_RUN_TEST(daemon_should_not_defer_for_unknown_contact);
    HU_RUN_TEST(daemon_should_not_defer_for_fresh_agent);
    HU_RUN_TEST(daemon_gate_boundary_is_exact_at_window_seconds);
    HU_RUN_TEST(daemon_record_then_check_flow_blocks_proactive_within_window);
}
