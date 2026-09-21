/* tests/test_daemon_follow_up_watcher.c
 *
 * Unit tests for the follow-up watcher daemon tick
 * (src/daemon/daemon_follow_up_watcher.c). The tick polls for INBOUND
 * messages the user never replied to and proposes circadian-aware
 * follow-ups; it is config-gated via hu_follow_up_watcher_config_t.enabled
 * and env-gated via HU_FOLLOW_UP_WATCHER (off|shadow|on, default shadow).
 *
 * These tests exercise the tick's deterministic control-flow contract
 * directly — no daemon, no chat.db, no network, no spawning. The chat.db
 * query is injected via hu_daemon_follow_up_watcher_set_finder, because
 * hu_imessage_find_inbound_unreplied is compiled out under HU_IS_TEST
 * (src/channels/imessage.c:3235) and so cannot be driven by a fixture
 * from inside the test binary.
 *
 * Contract under test:
 *   - NULL cfg / last_poll / watermark -> HU_ERR_INVALID_ARGUMENT
 *   - enabled=false                    -> HU_OK, no watermark advance
 *   - enabled=true, first poll         -> HU_OK, watermark advances to now
 *   - enabled=true, within interval    -> HU_OK, watermark NOT re-advanced
 *   - HU_FOLLOW_UP_WATCHER=off         -> finder never called
 *   - =shadow: only the aged unreplied candidate reaches
 *     hu_follow_up_should_send_now; channel send count stays 0; the
 *     production throttle budget is NOT consumed; one proactive_decisions
 *     row lands with trigger='follow_up', decision='send', sent=0
 *   - =on without a text source        -> still no send (degrades to shadow)
 *   - =on with a text source           -> exactly one send via the channel
 */

#include "test_framework.h"

#include "human/agent.h"
#include "human/agent/proactive_throttle.h"
#include "human/channel.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/daemon.h"
#include "human/persona.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/proactive_decisions_repo.h"
#include <sqlite3.h>
#endif

/* The reset helper is declared only under #if HU_IS_TEST in the source
 * with no public header; tests link against the test build (HU_IS_TEST=1)
 * so the symbol resolves. Declared extern here to call it. */
extern void hu_daemon_follow_up_watcher_reset_warn_guards_for_test(void);

/* Probe counters. The tick increments them unconditionally (plain statics,
 * not a behavior fork); only the accessors are test-only, matching the
 * reset-helper precedent above. */
extern unsigned hu_daemon_follow_up_watcher_send_now_calls_for_test(void);
extern unsigned hu_daemon_follow_up_watcher_proposals_for_test(void);
extern void hu_daemon_follow_up_watcher_reset_counters_for_test(void);

/* NULL cfg is rejected with HU_ERR_INVALID_ARGUMENT and never crashes. */
static void test_follow_up_watcher_null_cfg_returns_invalid_argument(void) {
    int64_t last_poll = 0;
    int64_t watermark = 0;
    hu_error_t err = hu_daemon_tick_follow_up_watcher(NULL, 1000, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

/* NULL in/out pointers are rejected too. */
static void test_follow_up_watcher_null_outptrs_returns_invalid_argument(void) {
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t watermark = 0;
    HU_ASSERT_EQ((int)hu_daemon_tick_follow_up_watcher(&cfg, 1000, NULL, &watermark, NULL, NULL,
                                                       NULL, 0, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
    int64_t last_poll = 0;
    HU_ASSERT_EQ((int)hu_daemon_tick_follow_up_watcher(&cfg, 1000, &last_poll, NULL, NULL, NULL,
                                                       NULL, 0, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

/* Disabled config is a clean no-op: HU_OK, watermark untouched. */
static void test_follow_up_watcher_disabled_is_noop(void) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_follow_up_watcher_config_t cfg = {.enabled = false, .interval_seconds = 300};
    int64_t last_poll = 0;
    int64_t watermark = 0;
    hu_error_t err = hu_daemon_tick_follow_up_watcher(&cfg, 1000, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)watermark, 0); /* disabled path must not advance */
    HU_ASSERT_EQ((int)last_poll, 0);
}

/* Enabled + first poll (last_poll==0) does the work: advances watermark
 * and last_poll to now_unix. */
static void test_follow_up_watcher_enabled_first_poll_advances_watermark(void) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0;
    int64_t watermark = 0;
    hu_error_t err = hu_daemon_tick_follow_up_watcher(&cfg, 5000, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)watermark, 5000);
    HU_ASSERT_EQ((int)last_poll, 5000);
}

/* Enabled but called again within the interval: the interval gate
 * short-circuits, so the watermark is NOT re-advanced. */
static void test_follow_up_watcher_within_interval_does_not_readvance(void) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 5000;
    int64_t watermark = 5000;
    /* now is only 100s after last_poll (< 300s interval). */
    hu_error_t err = hu_daemon_tick_follow_up_watcher(&cfg, 5100, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)watermark, 5000); /* unchanged */
    HU_ASSERT_EQ((int)last_poll, 5000); /* unchanged */
}

/* ──────────────────────────────────────────────────────────────────────────
 * Fixture: three contacts, one fake finder, one counting channel.
 *
 * C1 has an unreplied inbound OLDER than HU_FOLLOW_UP_WATCHER_MIN_AGE_MS
 *    -> the only candidate that may reach hu_follow_up_should_send_now
 * C2 has already been replied to (finder reports msg_id 0)
 * C3 has an unreplied inbound NEWER than the threshold
 * ────────────────────────────────────────────────────────────────────────── */

#define FUW_C1 "+15555550101" /* unreplied, aged   -> candidate */
#define FUW_C2 "+15555550102" /* replied           -> filtered by the finder */
#define FUW_C3 "+15555550103" /* unreplied, recent -> filtered by the age gate */

/* now_unix used by every fixture test; ms form is what the finder answers in. */
#define FUW_NOW_UNIX 1789000000LL
#define FUW_NOW_MS   ((uint64_t)FUW_NOW_UNIX * 1000ULL)

static unsigned g_fuw_finder_calls;

static hu_error_t fuw_fake_finder(void *ctx, const char *contact_id, size_t contact_id_len,
                                  int64_t *out_msg_id, uint64_t *out_inbound_at_ms) {
    (void)ctx;
    (void)contact_id_len;
    g_fuw_finder_calls++;
    *out_msg_id = 0;
    *out_inbound_at_ms = 0;
    if (!contact_id)
        return HU_OK;
    if (strcmp(contact_id, FUW_C1) == 0) {
        /* 12h old — comfortably past the 6h threshold. */
        *out_msg_id = 11;
        *out_inbound_at_ms = FUW_NOW_MS - (12ULL * 3600ULL * 1000ULL);
        return HU_OK;
    }
    if (strcmp(contact_id, FUW_C2) == 0)
        return HU_OK; /* seth already replied: no unreplied inbound */
    if (strcmp(contact_id, FUW_C3) == 0) {
        /* 1h old — inside the threshold, must not become a proposal. */
        *out_msg_id = 33;
        *out_inbound_at_ms = FUW_NOW_MS - (1ULL * 3600ULL * 1000ULL);
        return HU_OK;
    }
    return HU_OK;
}

static unsigned g_fuw_send_calls;
static char g_fuw_last_sent[256];

static hu_error_t fuw_mock_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    g_fuw_send_calls++;
    size_t n =
        message_len < sizeof(g_fuw_last_sent) - 1 ? message_len : sizeof(g_fuw_last_sent) - 1;
    if (message)
        memcpy(g_fuw_last_sent, message, n);
    g_fuw_last_sent[message ? n : 0] = '\0';
    return HU_OK;
}

static const char *fuw_mock_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static hu_error_t fuw_text_source(void *ctx, const char *contact_id, uint64_t age_ms, char *out,
                                  size_t cap) {
    (void)ctx;
    (void)contact_id;
    (void)age_ms;
    if (!out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    snprintf(out, cap, "sorry for the slow reply");
    return HU_OK;
}

/* Contacts must have a follow-up-worthy warmth tier, the same rule the live
 * outbound scheduler uses (src/daemon/daemon_followup_sched.c:77). */
static hu_contact_profile_t g_fuw_contacts[3];
static hu_persona_t g_fuw_persona;

static void fuw_build_persona(void) {
    memset(g_fuw_contacts, 0, sizeof(g_fuw_contacts));
    g_fuw_contacts[0].contact_id = (char *)FUW_C1;
    g_fuw_contacts[0].warmth_level = (char *)"close";
    g_fuw_contacts[1].contact_id = (char *)FUW_C2;
    g_fuw_contacts[1].warmth_level = (char *)"close";
    g_fuw_contacts[2].contact_id = (char *)FUW_C3;
    g_fuw_contacts[2].warmth_level = (char *)"close";
    memset(&g_fuw_persona, 0, sizeof(g_fuw_persona));
    g_fuw_persona.contacts = g_fuw_contacts;
    g_fuw_persona.contacts_count = 3;
}

static hu_channel_vtable_t g_fuw_vtable;
static hu_channel_t g_fuw_channel;
static hu_service_channel_t g_fuw_service_channel;

static void fuw_build_channel(void) {
    memset(&g_fuw_vtable, 0, sizeof(g_fuw_vtable));
    g_fuw_vtable.send = fuw_mock_send;
    g_fuw_vtable.name = fuw_mock_name;
    memset(&g_fuw_channel, 0, sizeof(g_fuw_channel));
    g_fuw_channel.vtable = &g_fuw_vtable;
    memset(&g_fuw_service_channel, 0, sizeof(g_fuw_service_channel));
    g_fuw_service_channel.channel = &g_fuw_channel;
}

/* Reset every piece of cross-test state the watcher touches. */
static void fuw_reset(const char *gate_value) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_daemon_follow_up_watcher_reset_counters_for_test();
    hu_daemon_follow_up_watcher_set_finder(fuw_fake_finder, NULL);
    hu_daemon_follow_up_watcher_set_text_source(NULL, NULL);
    g_fuw_finder_calls = 0;
    g_fuw_send_calls = 0;
    g_fuw_last_sent[0] = '\0';
    fuw_build_persona();
    fuw_build_channel();
    if (gate_value)
        setenv("HU_FOLLOW_UP_WATCHER", gate_value, 1);
    else
        unsetenv("HU_FOLLOW_UP_WATCHER");
    /* Reachability pre-filter must not be left on by a neighbouring suite. */
    unsetenv("HU_PROACTIVE_REACHABILITY");
}

static void fuw_teardown(void) {
    hu_daemon_follow_up_watcher_set_finder(NULL, NULL);
    hu_daemon_follow_up_watcher_set_text_source(NULL, NULL);
    unsetenv("HU_FOLLOW_UP_WATCHER");
}

/* OFF must return before any chat.db work: the finder is never called. */
static void test_follow_up_watcher_off_never_calls_finder(void) {
    fuw_reset("off");
    hu_allocator_t alloc = hu_system_allocator();
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.persona = &g_fuw_persona;
    hu_proactive_throttle_t throttle;
    hu_proactive_throttle_init(&throttle, &alloc);

    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0, watermark = 0;
    HU_ASSERT_EQ((int)hu_daemon_tick_follow_up_watcher(&cfg, FUW_NOW_UNIX, &last_poll, &watermark,
                                                       &agent, NULL, &g_fuw_service_channel, 1,
                                                       &throttle),
                 (int)HU_OK);
    HU_ASSERT_EQ(g_fuw_finder_calls, 0u);
    HU_ASSERT_EQ(hu_daemon_follow_up_watcher_send_now_calls_for_test(), 0u);
    HU_ASSERT_EQ(g_fuw_send_calls, 0u);
    fuw_teardown();
}

/* SHADOW: the finder sees all three contacts, but exactly ONE of them —
 * the aged unreplied inbound — reaches hu_follow_up_should_send_now. */
static void test_follow_up_watcher_shadow_filters_to_one_candidate(void) {
    fuw_reset("shadow");
    hu_allocator_t alloc = hu_system_allocator();
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.persona = &g_fuw_persona;
    hu_proactive_throttle_t throttle;
    hu_proactive_throttle_init(&throttle, &alloc);

    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0, watermark = 0;
    HU_ASSERT_EQ((int)hu_daemon_tick_follow_up_watcher(&cfg, FUW_NOW_UNIX, &last_poll, &watermark,
                                                       &agent, NULL, &g_fuw_service_channel, 1,
                                                       &throttle),
                 (int)HU_OK);

    /* All three contacts were queried — otherwise "exactly one" could be an
     * artifact of the loop stopping early rather than of the filters. */
    HU_ASSERT_EQ(g_fuw_finder_calls, 3u);
    HU_ASSERT_EQ(hu_daemon_follow_up_watcher_send_now_calls_for_test(), 1u);
    HU_ASSERT_EQ(hu_daemon_follow_up_watcher_proposals_for_test(), 1u);
    /* The whole point of shadow: nothing reaches a real person. */
    HU_ASSERT_EQ(g_fuw_send_calls, 0u);
    HU_ASSERT_EQ((int)watermark, (int)FUW_NOW_UNIX);
    fuw_teardown();
}

/* SHADOW must not spend the production throttle's per-contact budget:
 * hu_follow_up_should_send_now calls hu_proactive_throttle_record_send,
 * which counts toward the SAME daily/weekly caps real proactive check-ins
 * use. A shadow mode that consumed them would silently suppress live sends. */
static void test_follow_up_watcher_shadow_does_not_consume_throttle_budget(void) {
    fuw_reset("shadow");
    hu_allocator_t alloc = hu_system_allocator();
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.persona = &g_fuw_persona;
    hu_proactive_throttle_t throttle;
    hu_proactive_throttle_init(&throttle, &alloc);

    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0, watermark = 0;
    (void)hu_daemon_tick_follow_up_watcher(&cfg, FUW_NOW_UNIX, &last_poll, &watermark, &agent, NULL,
                                           &g_fuw_service_channel, 1, &throttle);
    HU_ASSERT_EQ(hu_daemon_follow_up_watcher_send_now_calls_for_test(), 1u);

    /* Pre/post contract: the production ledger still has C1's first send
     * available. If shadow had charged it, this would return false. */
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&throttle, FUW_C1, "proactive", FUW_NOW_MS));
    fuw_teardown();
}

/* ON without a text source degrades to shadow rather than inventing copy. */
static void test_follow_up_watcher_on_without_text_source_does_not_send(void) {
    fuw_reset("on");
    hu_allocator_t alloc = hu_system_allocator();
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.persona = &g_fuw_persona;
    hu_proactive_throttle_t throttle;
    hu_proactive_throttle_init(&throttle, &alloc);

    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0, watermark = 0;
    (void)hu_daemon_tick_follow_up_watcher(&cfg, FUW_NOW_UNIX, &last_poll, &watermark, &agent, NULL,
                                           &g_fuw_service_channel, 1, &throttle);
    HU_ASSERT_EQ(hu_daemon_follow_up_watcher_send_now_calls_for_test(), 1u);
    HU_ASSERT_EQ(g_fuw_send_calls, 0u);
    fuw_teardown();
}

/* ON with a text source sends exactly once, to the one real candidate.
 * This is what proves the send path is actually wired rather than dead:
 * the same tick that sends 0 in shadow sends 1 here. */
static void test_follow_up_watcher_on_with_text_source_sends_once(void) {
    fuw_reset("on");
    hu_daemon_follow_up_watcher_set_text_source(fuw_text_source, NULL);
    hu_allocator_t alloc = hu_system_allocator();
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.persona = &g_fuw_persona;
    hu_proactive_throttle_t throttle;
    hu_proactive_throttle_init(&throttle, &alloc);

    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0, watermark = 0;
    (void)hu_daemon_tick_follow_up_watcher(&cfg, FUW_NOW_UNIX, &last_poll, &watermark, &agent, NULL,
                                           &g_fuw_service_channel, 1, &throttle);
    HU_ASSERT_EQ(g_fuw_send_calls, 1u);
    HU_ASSERT_STR_EQ(g_fuw_last_sent, "sorry for the slow reply");
    fuw_teardown();
}

#ifdef HU_ENABLE_SQLITE
/* SHADOW writes the decision row the When2Speak eval reads: the row must
 * say trigger='follow_up', decision='send', sent=0 — asserted by VALUE, so
 * a writer that mislabelled the trigger or claimed delivery would fail. */
static void test_follow_up_watcher_shadow_writes_decision_row(void) {
    fuw_reset("shadow");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.persona = &g_fuw_persona;
    agent.memory = &mem;

    /* Precondition, so the post-assert cannot pass vacuously. */
    int64_t before = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_count(db, &before), HU_OK);
    HU_ASSERT_EQ((int)before, 0);

    hu_proactive_throttle_t throttle;
    hu_proactive_throttle_init(&throttle, &alloc);
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0, watermark = 0;
    (void)hu_daemon_tick_follow_up_watcher(&cfg, FUW_NOW_UNIX, &last_poll, &watermark, &agent, NULL,
                                           &g_fuw_service_channel, 1, &throttle);

    int64_t after = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_count(db, &after), HU_OK);
    HU_ASSERT_EQ((int)after, 1); /* one row for the one candidate, not three */

    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT contact, trigger, decision, sent, ts FROM "
                                    "proactive_decisions ORDER BY id DESC LIMIT 1",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), FUW_C1);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "follow_up");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 2), HU_PROACTIVE_DECISION_SEND);
    HU_ASSERT_EQ(sqlite3_column_int(st, 3), 0); /* shadow never claims delivery */
    /* ts is SECONDS, not ms — a ms value here would be ~1e12 and would
     * silently break scripts/eval_when_to_speak.py's time bucketing. */
    HU_ASSERT_EQ((long long)sqlite3_column_int64(st, 4), (long long)FUW_NOW_UNIX);
    sqlite3_finalize(st);

    mem.vtable->deinit(mem.ctx);
    fuw_teardown();
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_follow_up_watcher_tests(void) {
    HU_TEST_SUITE("daemon_follow_up_watcher");
    HU_RUN_TEST(test_follow_up_watcher_null_cfg_returns_invalid_argument);
    HU_RUN_TEST(test_follow_up_watcher_null_outptrs_returns_invalid_argument);
    HU_RUN_TEST(test_follow_up_watcher_disabled_is_noop);
    HU_RUN_TEST(test_follow_up_watcher_enabled_first_poll_advances_watermark);
    HU_RUN_TEST(test_follow_up_watcher_within_interval_does_not_readvance);
    HU_RUN_TEST(test_follow_up_watcher_off_never_calls_finder);
    HU_RUN_TEST(test_follow_up_watcher_shadow_filters_to_one_candidate);
    HU_RUN_TEST(test_follow_up_watcher_shadow_does_not_consume_throttle_budget);
    HU_RUN_TEST(test_follow_up_watcher_on_without_text_source_does_not_send);
    HU_RUN_TEST(test_follow_up_watcher_on_with_text_source_sends_once);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_follow_up_watcher_shadow_writes_decision_row);
#endif
}
