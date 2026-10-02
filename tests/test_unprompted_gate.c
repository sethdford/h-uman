/* tests/test_unprompted_gate.c — the one unprompted-send gate stack
 * (DEF-6/7/9/14, 2026-10-02), driven through the REAL send paths:
 *   - hu_daemon_proactive_gate_and_send   (proposer + follow-up watcher)
 *   - hu_service_run_agent_cron           (the 10:00 cron)
 *   - hu_daemon_sched_deliver             (where the read-no-reply bump lands)
 *   - hu_daemon_f25_checkins_tick         (F25 emotional check-in)
 *   - hu_daemon_photo_share_send          (photo share)
 * plus hu_unprompted_send_check itself for the per-stage truth table.
 *
 * Every send-path test uses a channel that COUNTS sends, so "denied" means
 * the channel was never called, not merely that a function returned false.
 * Assertions are pre/post: precondition asserted, real symbol invoked,
 * postcondition asserted. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent.h"
#include "human/agent/governor.h"
#include "human/agent/proactive_throttle.h"
#include "human/channel.h"
#include "human/context/conversation.h"
#include "human/cron.h"
#include "human/daemon.h"
#include "human/daemon/unprompted_gate.h"
#include "human/daemon/unprompted_sends.h"
#include "human/daemon_contact_optout.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include "human/memory/proactive_decisions_repo.h"
#include "human/persona.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UG_X "+15555550111"
#define UG_Y "+15555550122"

/* ── counting channel ─────────────────────────────────────────────────── */
typedef struct {
    int sends;
    char last_target[64];
    char last_msg[512];
    size_t last_media;
} ug_chan_t;

static ug_chan_t g_ug;

static const char *ug_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static hu_error_t ug_send(void *ctx, const char *target, size_t target_len, const char *message,
                          size_t message_len, const char *const *media, size_t media_count) {
    (void)ctx;
    (void)media;
    g_ug.sends++;
    snprintf(g_ug.last_target, sizeof(g_ug.last_target), "%.*s", (int)target_len,
             target ? target : "");
    snprintf(g_ug.last_msg, sizeof(g_ug.last_msg), "%.*s", (int)message_len,
             message ? message : "");
    g_ug.last_media = media_count;
    return HU_OK;
}

static hu_channel_vtable_t g_ug_vt = {.send = ug_send, .name = ug_name};

static hu_channel_t ug_channel(void) {
    memset(&g_ug, 0, sizeof(g_ug));
    hu_channel_t ch = {.ctx = NULL, .vtable = &g_ug_vt};
    return ch;
}

/* A timestamp at 12:00 LOCAL time, `days` from now: the stack's sleep floor
 * reads the machine's local hour, so a fixed epoch would make the allow-path
 * tests depend on the timezone the suite runs in. */
static int64_t ug_local_noon(int days) {
    time_t t = time(NULL) + (time_t)days * 86400;
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static int32_t ug_tz(int64_t now) {
    time_t t = (time_t)now;
    struct tm tm;
    localtime_r(&t, &tm);
    return (int32_t)tm.tm_gmtoff;
}

static int64_t ug_sent_rows(sqlite3 *db, const char *contact, const char *trigger) {
    int64_t n = -1;
    (void)hu_proactive_decisions_repo_count_since(db, contact, trigger, HU_PROACTIVE_DECISION_SEND,
                                                  0, &n);
    return n;
}

static int ug_last_reason_is(sqlite3 *db, const char *reason) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT reason FROM proactive_decisions ORDER BY id DESC LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *r = sqlite3_column_text(st, 0);
        ok = r && strcmp((const char *)r, reason) == 0;
    }
    sqlite3_finalize(st);
    return ok;
}

static hu_proactive_budget_t ug_budget(void) {
    hu_proactive_budget_t b = {0};
    b.daily_max = 6;
    b.weekly_max = 15;
    b.relationship_multiplier = 1.0;
    b.cool_off_after_unanswered = UINT8_MAX;
    b.cool_off_hours = 72;
    return b;
}

/* Runs the proactive gate chain with a FRESH throttle + budget — i.e. a
 * freshly restarted daemon — against `db`. Returns the channel send count. */
static int ug_proactive_send(hu_memory_t *mem, const char *who, int64_t now) {
    hu_allocator_t alloc = hu_system_allocator();
    struct hu_agent agent = {0};
    agent.memory = mem;
    agent.alloc = &alloc;
    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)who;
    hu_channel_t chan = ug_channel();
    hu_proactive_throttle_t *th =
        (hu_proactive_throttle_t *)alloc.alloc(alloc.ctx, sizeof(hu_proactive_throttle_t));
    hu_proactive_throttle_init(th, &alloc);
    hu_proactive_budget_t budget = ug_budget();
    char response[64] = "hey, you around this weekend?";
    size_t response_len = strlen(response);
    (void)hu_daemon_proactive_gate_and_send(&agent, &alloc, &chan, &cp, "imessage", who,
                                            strlen(who), response, &response_len, now, &budget,
                                            NULL, ug_tz(now), th);
    alloc.free(alloc.ctx, th, sizeof(hu_proactive_throttle_t));
    return g_ug.sends;
}

/* ── RED-on-main tests: these use only APIs that existed before the fix ── */

/* DEF-9: the cap lived in memory, so a restart forgot today's send. */
static void test_proactive_cap_survives_restart(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t now = ug_local_noon(0);
    /* Control: a contact with no prior send IS sent to (the gate is passable). */
    HU_ASSERT_EQ(ug_proactive_send(&mem, UG_Y, now), 1);
    /* The previous process delivered to X an hour ago. */
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, now - 3600, UG_X, "proactive_send",
                                                    HU_PROACTIVE_DECISION_SEND, NULL, 1, NULL),
                 HU_OK);
    /* New process state, same DB: the cap must still hold. */
    HU_ASSERT_EQ(ug_proactive_send(&mem, UG_X, now), 0);
    HU_ASSERT_TRUE(ug_last_reason_is(db, "send_cap"));
    mem.vtable->deinit(mem.ctx);
}

/* DEF-14: opt-out was only consulted by the proposer loop. */
static void test_proactive_gate_denies_opted_out_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t now = ug_local_noon(0);
    HU_ASSERT_TRUE(hu_contact_optout_observe_db(db, UG_X, strlen(UG_X), "please stop texting me",
                                                22, now - 60));
    HU_ASSERT_EQ(ug_proactive_send(&mem, UG_X, now), 0);
    HU_ASSERT_TRUE(ug_last_reason_is(db, "contact_optout"));
    mem.vtable->deinit(mem.ctx);
}

/* DEF-7: the cron sent straight through vtable->send. */
static void test_cron_directed_send_denied_for_opted_out_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_TRUE(hu_contact_optout_observe_db(db, UG_X, strlen(UG_X), "stop texting me", 15,
                                                (int64_t)time(NULL)));
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 8, true);
    HU_ASSERT_NOT_NULL(sched);
    uint64_t id = 0;
    HU_ASSERT_EQ(hu_cron_add_agent_job(sched, &alloc, "* * * * *", "check in", "imessage:" UG_X,
                                       "proactive:x", &id),
                 HU_OK);
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.memory = &mem;
    agent.scheduler = sched;
    hu_channel_t chan = ug_channel();
    hu_service_channel_t chans[1] = {{.channel = &chan}};
    HU_ASSERT_EQ(hu_service_run_agent_cron(&alloc, &agent, chans, 1), HU_OK);
    HU_ASSERT_EQ(g_ug.sends, 0);
    hu_cron_destroy(sched, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* ── the stack itself ─────────────────────────────────────────────────── */

static hu_unprompted_gate_t ug_gate(hu_memory_t *mem, struct hu_agent *agent, int64_t now) {
    static hu_allocator_t alloc;
    alloc = hu_system_allocator();
    agent->memory = mem;
    agent->alloc = &alloc;
    hu_unprompted_gate_t g;
    hu_unprompted_gate_init(&g, &alloc, agent, NULL, NULL, NULL, ug_tz(now), "imessage", UG_X,
                            strlen(UG_X));
    return g;
}

static void test_every_kind_denied_when_opted_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    struct hu_agent agent = {0};
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    const hu_unprompted_kind_t kinds[] = {HU_UNPROMPTED_PROACTIVE, HU_UNPROMPTED_CRON,
                                          HU_UNPROMPTED_BUMP, HU_UNPROMPTED_F25,
                                          HU_UNPROMPTED_PHOTO};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, kinds[i], now, NULL, NULL, false),
                     HU_UNPROMPTED_ALLOW);
    HU_ASSERT_TRUE(hu_contact_optout_observe_db(db, UG_X, strlen(UG_X), "leave me alone", 14, now));
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, kinds[i], now, NULL, NULL, false),
                     HU_UNPROMPTED_DENY_OPTOUT);
    mem.vtable->deinit(mem.ctx);
}

static void test_every_kind_denied_in_quiet_hours(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    struct hu_agent agent = {0};
    int64_t noon = ug_local_noon(0);
    int64_t night = noon + 14 * 3600; /* 02:00 local (DST edges aside) */
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, night);
    const hu_unprompted_kind_t kinds[] = {HU_UNPROMPTED_PROACTIVE, HU_UNPROMPTED_CRON,
                                          HU_UNPROMPTED_BUMP, HU_UNPROMPTED_F25,
                                          HU_UNPROMPTED_PHOTO};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, kinds[i], noon, NULL, NULL, false),
                     HU_UNPROMPTED_ALLOW);
        HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, kinds[i], night, NULL, NULL, false),
                     HU_UNPROMPTED_DENY_QUIET_HOURS);
    }
    mem.vtable->deinit(mem.ctx);
}

static void test_every_kind_denied_over_cap_and_cap_counts_every_kind(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    struct hu_agent agent = {0};
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    const hu_unprompted_kind_t kinds[] = {HU_UNPROMPTED_PROACTIVE, HU_UNPROMPTED_CRON,
                                          HU_UNPROMPTED_BUMP, HU_UNPROMPTED_F25,
                                          HU_UNPROMPTED_PHOTO};
    for (size_t sent_kind = 1; sent_kind < sizeof(kinds) / sizeof(kinds[0]); sent_kind++) {
        sqlite3_exec(db, "DELETE FROM proactive_decisions", NULL, NULL, NULL);
        HU_ASSERT_EQ(
            hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
            HU_UNPROMPTED_ALLOW);
        /* One delivered send of THIS kind 2h ago charges the shared cap. */
        hu_unprompted_record_sent(&g, UG_X, kinds[sent_kind], now - 7200);
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
            HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, kinds[i], now, NULL, NULL, false),
                         HU_UNPROMPTED_DENY_CAP);
    }
    /* Weekly: three sends spread over 6 days (none in the last 24h) cap it. */
    sqlite3_exec(db, "DELETE FROM proactive_decisions", NULL, NULL, NULL);
    for (int d = 2; d <= 6; d += 2)
        HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, now - d * 86400, UG_X, "proactive_send",
                                                        HU_PROACTIVE_DECISION_SEND, NULL, 1, NULL),
                     HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record_inbound(db, UG_X, now - 3600), HU_OK);
    HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_BUMP, now, NULL, NULL, false),
                 HU_UNPROMPTED_DENY_CAP);
    mem.vtable->deinit(mem.ctx);
}

/* DEF-6: Y's reply must not lift X's cool-off; X's own reply must. */
static void test_cooloff_is_per_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    struct hu_agent agent = {0};
    agent.memory = &mem;
    agent.alloc = &alloc;
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_unprompted_state_ensure(db, now - 30 * 86400), HU_OK);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_ALLOW);
    /* Two unanswered unprompted sends to X, outside the 24h cap window. */
    hu_unprompted_record_sent(&g, UG_X, HU_UNPROMPTED_BUMP, now - 3 * 86400);
    hu_unprompted_record_sent(&g, UG_X, HU_UNPROMPTED_F25, now - 2 * 86400);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_DENY_COOLOFF);
    /* Someone ELSE replies. The old global reset lifted X's cool-off here. */
    hu_unprompted_record_inbound(&agent, NULL, UG_Y, strlen(UG_Y), now - 60);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_DENY_COOLOFF);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_Y, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_ALLOW);
    /* X replies: only now is X's count reset. */
    hu_unprompted_record_inbound(&agent, NULL, UG_X, strlen(UG_X), now - 30);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_ALLOW);
    mem.vtable->deinit(mem.ctx);
}

/* Sends from before the ledger existed have no recorded replies, so they
 * must not start a cool-off on deploy day. */
static void test_cooloff_ignores_sends_before_epoch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    struct hu_agent agent = {0};
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    for (int d = 2; d <= 5; d++)
        HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, now - d * 86400 - 3600, UG_X,
                                                        "proactive_send",
                                                        HU_PROACTIVE_DECISION_SEND, NULL, 1, NULL),
                     HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_unprompted_state_ensure(db, now - 86400), HU_OK);
    int64_t n = -1, last = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_unanswered(db, UG_X, now, &n, &last), HU_OK);
    HU_ASSERT_EQ(n, 0);
    /* Weekly cap still sees them (3 in 7 days) — the ceiling is independent. */
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_DENY_CAP);
    mem.vtable->deinit(mem.ctx);
}

static void test_violent_text_blocked_by_stage7_and_clean_text_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    struct hu_agent agent = {0};
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    char ok[] = "hey how was the trip";
    size_t ok_len = strlen(ok);
    HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_CRON, now, ok, &ok_len, true),
                 HU_UNPROMPTED_ALLOW);
    HU_ASSERT_EQ(ok_len, strlen("hey how was the trip"));
    /* The cron used to only LOG this; stage 7's pipeline moderation blocks it. */
    char bad[] = "i will kill him";
    size_t bad_len = strlen(bad);
    HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_CRON, now, bad, &bad_len, true),
                 HU_UNPROMPTED_DENY_SANITIZER);
    mem.vtable->deinit(mem.ctx);
}

/* No second moderation pass after stage 7: the old stage 8 blocked anything
 * the raw self-harm classifier flagged, and its substring match reads
 * "spend it all" as "end it all". Ordinary chat must still go out. */
static void test_spend_it_all_is_not_blocked_at_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    struct hu_agent agent = {0};
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    char msg[] = "did you spend it all at the fair lol";
    size_t len = strlen(msg);
    HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, msg, &len, true),
                 HU_UNPROMPTED_ALLOW);
    HU_ASSERT_EQ(len, strlen("did you spend it all at the fair lol"));
    mem.vtable->deinit(mem.ctx);
}

/* Positive control for the cron: a contact who has NOT opted out, at local
 * noon, IS sent the agent's text — through the validator chain and the stack
 * (the ledger row proves the stack's record path ran), never a raw send. */
static void test_cron_directed_send_allowed_for_consenting_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 8, true);
    HU_ASSERT_NOT_NULL(sched);
    uint64_t id = 0;
    HU_ASSERT_EQ(hu_cron_add_agent_job(sched, &alloc, "* * * * *", "check in", "imessage:" UG_X,
                                       "proactive:x", &id),
                 HU_OK);
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.memory = &mem;
    agent.scheduler = sched;
    hu_channel_t chan = ug_channel();
    hu_service_channel_t chans[1] = {{.channel = &chan}};
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_cron"), 0);
    HU_ASSERT_EQ(hu_service_run_agent_cron_at(&alloc, &agent, chans, 1, (time_t)ug_local_noon(0)),
                 HU_OK);
    HU_ASSERT_EQ(g_ug.sends, 1);
    HU_ASSERT_STR_EQ(g_ug.last_target, UG_X);
    HU_ASSERT_STR_EQ(g_ug.last_msg, "[agent-cron-test]"); /* the HU_IS_TEST agent turn */
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_cron"), 1);
    /* Same job one hour later: the cap the first send charged now holds. */
    HU_ASSERT_EQ(
        hu_service_run_agent_cron_at(&alloc, &agent, chans, 1, (time_t)ug_local_noon(0) + 3600),
        HU_OK);
    HU_ASSERT_EQ(g_ug.sends, 1);
    hu_cron_destroy(sched, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static const char *ug_cli_name(void *ctx) {
    (void)ctx;
    return "cli";
}

/* A cron job with no contact in its channel ("imessage", not
 * "imessage:+1...") used to send with an empty target, which the iMessage
 * channel resolves to its configured default_target — a real person the gate
 * never saw. The recipient cannot be resolved, so the stack denies it; only
 * the owner's own stdout sink (cli) may take undirected output. */
static void test_cron_undirected_denied_except_owner_sink(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 8, true);
    HU_ASSERT_NOT_NULL(sched);
    uint64_t id = 0;
    HU_ASSERT_EQ(
        hu_cron_add_agent_job(sched, &alloc, "* * * * *", "say hi", "imessage", "undirected", &id),
        HU_OK);
    struct hu_agent agent = {0};
    agent.alloc = &alloc;
    agent.memory = &mem;
    agent.scheduler = sched;
    hu_channel_t chan = ug_channel();
    hu_service_channel_t chans[1] = {{.channel = &chan}};
    HU_ASSERT_EQ(hu_service_run_agent_cron_at(&alloc, &agent, chans, 1, (time_t)ug_local_noon(0)),
                 HU_OK);
    HU_ASSERT_EQ(g_ug.sends, 0);
    hu_cron_destroy(sched, &alloc);

    /* Control: the same undirected job on the owner sink IS delivered. */
    sched = hu_cron_create(&alloc, 8, true);
    HU_ASSERT_EQ(
        hu_cron_add_agent_job(sched, &alloc, "* * * * *", "report", "cli", "research-agent", &id),
        HU_OK);
    agent.scheduler = sched;
    hu_channel_vtable_t cli_vt = {.send = ug_send, .name = ug_cli_name};
    hu_channel_t cli = {.ctx = NULL, .vtable = &cli_vt};
    hu_service_channel_t cli_chans[1] = {{.channel = &cli}};
    HU_ASSERT_EQ(
        hu_service_run_agent_cron_at(&alloc, &agent, cli_chans, 1, (time_t)ug_local_noon(0)),
        HU_OK);
    HU_ASSERT_EQ(g_ug.sends, 1);
    HU_ASSERT_STR_EQ(g_ug.last_target, "");
    HU_ASSERT_STR_EQ(g_ug.last_msg, "[agent-cron-test]");
    hu_cron_destroy(sched, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* The owner-sink moderation is blocking now (it only logged), with the
 * outbound pipeline's policy: violence blocks, a self-harm mention and
 * ordinary chat do not. */
static void test_cron_owner_sink_moderation_blocks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_TRUE(hu_daemon_cron_is_owner_sink("cli"));
    HU_ASSERT_FALSE(hu_daemon_cron_is_owner_sink("imessage"));
    HU_ASSERT_FALSE(hu_daemon_cron_is_owner_sink(NULL));
    HU_ASSERT_TRUE(hu_daemon_cron_owner_text_ok(&alloc, "3 new papers today", 18));
    HU_ASSERT_TRUE(hu_daemon_cron_owner_text_ok(&alloc, "did you spend it all at the fair lol",
                                                strlen("did you spend it all at the fair lol")));
    HU_ASSERT_FALSE(hu_daemon_cron_owner_text_ok(&alloc, "i will kill him", 15));
}

/* Owner-scheduled delivery must not stall when unprompted sends spent the
 * global budget: the delivery pass runs before the daemon's budget gate and
 * consults the budget only for unprompted (tagged) entries. */
static void test_owner_scheduled_delivered_with_budget_exhausted(void) {
    char dir[] = "/tmp/hu_ug_state_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    const char *old_state = getenv("HU_STATE_DIR");
    char saved[512];
    snprintf(saved, sizeof(saved), "%s", old_state ? old_state : "");
    setenv("HU_STATE_DIR", dir, 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    struct hu_agent agent = {0};
    agent.memory = &mem;
    agent.alloc = &alloc;
    hu_proactive_budget_t budget = ug_budget();
    int64_t now = ug_local_noon(0);
    while (hu_governor_has_budget(&budget, (uint64_t)now * 1000ULL))
        HU_ASSERT_EQ(hu_governor_record_sent(&budget, (uint64_t)now * 1000ULL), HU_OK);
    hu_daemon_unprompted_set_budget_for_test(&budget);

    hu_channel_t chan = ug_channel();
    hu_service_channel_t chans[1] = {{.channel = &chan}};
    uint64_t due = (uint64_t)(now - 60) * 1000ULL;
    HU_ASSERT_EQ(hu_conversation_schedule_message_kind(UG_X, strlen(UG_X), "imessage", 8,
                                                       "hey no rush", 11, due, HU_UNPROMPTED_BUMP),
                 HU_OK);
    HU_ASSERT_EQ(hu_conversation_schedule_message_on(UG_Y, strlen(UG_Y), "imessage", 8,
                                                     "Dinner at 7.", 12, due),
                 HU_OK);
    /* One entry per channel per pass, as before. */
    hu_daemon_sched_deliver_due(&alloc, &agent, chans, 1, now);
    hu_daemon_sched_deliver_due(&alloc, &agent, chans, 1, now);
    HU_ASSERT_EQ(g_ug.sends, 1);              /* the bump met the exhausted budget */
    HU_ASSERT_STR_EQ(g_ug.last_target, UG_Y); /* the owner's message went out */
    HU_ASSERT_STR_EQ(g_ug.last_msg, "dinner at 7");

    hu_daemon_unprompted_set_budget_for_test(NULL);
    mem.vtable->deinit(mem.ctx);
    char sp[600];
    snprintf(sp, sizeof(sp), "%s/scheduled.json", dir);
    unlink(sp);
    rmdir(dir);
    if (old_state)
        setenv("HU_STATE_DIR", saved, 1);
    else
        unsetenv("HU_STATE_DIR");
}

/* An inbound from a contact's EMAIL address (hu_persona_find_contact's
 * fallback) is the same person as their contact_id for cool-off and opt-out. */
static void test_email_fallback_contact_is_one_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)UG_X;
    cp.email = (char *)"x@example.com";
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.contacts = &cp;
    persona.contacts_count = 1;
    struct hu_agent agent = {0};
    agent.persona = &persona;
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g = ug_gate(&mem, &agent, now);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_unprompted_state_ensure(db, now - 30 * 86400), HU_OK);
    hu_unprompted_record_sent(&g, UG_X, HU_UNPROMPTED_BUMP, now - 3 * 86400);
    hu_unprompted_record_sent(&g, UG_X, HU_UNPROMPTED_F25, now - 2 * 86400);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_DENY_COOLOFF);
    hu_unprompted_record_inbound(&agent, NULL, "x@example.com", strlen("x@example.com"), now - 30);
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_ALLOW);
    /* Opt-out typed from the email address suppresses the contact_id. */
    HU_ASSERT_TRUE(hu_daemon_contact_optout_observe(
        &agent, "x@example.com", strlen("x@example.com"), "stop texting me", 15));
    HU_ASSERT_EQ(
        hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_PROACTIVE, now, NULL, NULL, false),
        HU_UNPROMPTED_DENY_OPTOUT);
    char key[64];
    HU_ASSERT_EQ(hu_unprompted_contact_key(&persona, "x@example.com", 13, key, sizeof(key)),
                 strlen(UG_X));
    HU_ASSERT_STR_EQ(key, UG_X);
    HU_ASSERT_EQ(hu_unprompted_contact_key(&persona, UG_Y, strlen(UG_Y), key, sizeof(key)),
                 strlen(UG_Y)); /* unknown contact: its own key */
    mem.vtable->deinit(mem.ctx);
}

static void test_no_ledger_fails_closed(void) {
    struct hu_agent agent = {0};
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    int64_t now = ug_local_noon(0);
    hu_unprompted_gate_t g;
    hu_unprompted_gate_init(&g, &alloc, &agent, NULL, NULL, NULL, ug_tz(now), "imessage", UG_X,
                            strlen(UG_X));
    HU_ASSERT_NULL(g.db);
    HU_ASSERT_EQ(hu_unprompted_send_check(&g, UG_X, HU_UNPROMPTED_BUMP, now, NULL, NULL, false),
                 HU_UNPROMPTED_DENY_NO_LEDGER);
}

/* ── the other send paths ─────────────────────────────────────────────── */

static void test_sched_bump_gated_but_owner_scheduled_unchanged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    struct hu_agent agent = {0};
    agent.memory = &mem;
    agent.alloc = &alloc;
    int64_t now = ug_local_noon(0);
    hu_channel_t chan = ug_channel();

    /* Allowed bump: delivered and charged to the ledger. */
    char m1[512] = "hey, no rush on that";
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_bump"), 0);
    HU_ASSERT_TRUE(hu_daemon_sched_deliver(&alloc, &agent, &chan, "imessage", UG_X, m1, strlen(m1),
                                           sizeof(m1), HU_UNPROMPTED_BUMP, now));
    HU_ASSERT_EQ(g_ug.sends, 1);
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_bump"), 1);

    /* A second bump the same day is over the cap: never reaches the channel. */
    char m2[512] = "hey, no rush on that";
    HU_ASSERT_FALSE(hu_daemon_sched_deliver(&alloc, &agent, &chan, "imessage", UG_X, m2, strlen(m2),
                                            sizeof(m2), HU_UNPROMPTED_BUMP, now + 3600));
    HU_ASSERT_EQ(g_ug.sends, 1);

    /* Opted out: the bump dies; an OWNER-scheduled message (kind 0) keeps the
     * historical pipeline byte-for-byte and is delivered. */
    HU_ASSERT_TRUE(
        hu_contact_optout_observe_db(db, UG_Y, strlen(UG_Y), "stop texting me", 15, now));
    char m3[512] = "hey, no rush on that";
    HU_ASSERT_FALSE(hu_daemon_sched_deliver(&alloc, &agent, &chan, "imessage", UG_Y, m3, strlen(m3),
                                            sizeof(m3), HU_UNPROMPTED_BUMP, now));
    HU_ASSERT_EQ(g_ug.sends, 1);
    char m4[512] = "Dinner at 7.";
    HU_ASSERT_TRUE(hu_daemon_sched_deliver(&alloc, &agent, &chan, "imessage", UG_Y, m4, strlen(m4),
                                           sizeof(m4), 0, now));
    HU_ASSERT_EQ(g_ug.sends, 2);
    HU_ASSERT_STR_EQ(g_ug.last_msg, "dinner at 7");
    HU_ASSERT_EQ(ug_sent_rows(db, UG_Y, "unprompted_bump"), 0);
    mem.vtable->deinit(mem.ctx);
}

static void test_sched_kind_round_trips_through_scheduled_json(void) {
    char path[] = "/tmp/hu_ug_sched_XXXXXX";
    int fd = mkstemp(path);
    HU_ASSERT_TRUE(fd >= 0);
    close(fd);
    uint64_t at = 1000;
    HU_ASSERT_EQ(hu_conversation_schedule_message_kind(UG_X, strlen(UG_X), "imessage", 8, "bump", 4,
                                                       at, HU_UNPROMPTED_BUMP),
                 HU_OK);
    HU_ASSERT_EQ(hu_conversation_sched_save(path, strlen(path)), HU_OK);
    HU_ASSERT_EQ(hu_conversation_sched_load(path, strlen(path)), HU_OK);
    char who[128], ch[32], msg[512];
    uint8_t kind = 0;
    HU_ASSERT_EQ(hu_conversation_flush_scheduled_kind(at, "imessage", 8, who, sizeof(who), ch,
                                                      sizeof(ch), msg, sizeof(msg), &kind),
                 (size_t)4);
    HU_ASSERT_EQ(kind, HU_UNPROMPTED_BUMP);
    /* Untagged entries round-trip as kind 0 and the file carries no "kind". */
    HU_ASSERT_EQ(
        hu_conversation_schedule_message_on(UG_Y, strlen(UG_Y), "imessage", 8, "owner", 5, at),
        HU_OK);
    HU_ASSERT_EQ(hu_conversation_sched_save(path, strlen(path)), HU_OK);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[1024] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NULL(strstr(buf, "\"kind\""));
    kind = 9;
    HU_ASSERT_EQ(hu_conversation_flush_scheduled_kind(at, "imessage", 8, who, sizeof(who), ch,
                                                      sizeof(ch), msg, sizeof(msg), &kind),
                 (size_t)5);
    HU_ASSERT_EQ(kind, 0);
    unlink(path);
}

static void test_photo_share_gated_by_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    struct hu_agent agent = {0};
    agent.memory = &mem;
    agent.alloc = &alloc;
    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)UG_X;
    int64_t now = ug_local_noon(0);
    hu_channel_t chan = ug_channel();
    const char *media[1] = {"/tmp/photo.jpg"};
    HU_ASSERT_TRUE(hu_daemon_photo_share_send(&alloc, &agent, &chan, &cp, "imessage", UG_X,
                                              strlen(UG_X), media, 1, now));
    HU_ASSERT_EQ(g_ug.sends, 1);
    HU_ASSERT_EQ(g_ug.last_media, (size_t)1);
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_photo"), 1);
    HU_ASSERT_FALSE(hu_daemon_photo_share_send(&alloc, &agent, &chan, &cp, "imessage", UG_X,
                                               strlen(UG_X), media, 1, now + 600));
    HU_ASSERT_EQ(g_ug.sends, 1);
    mem.vtable->deinit(mem.ctx);
}

static void test_f25_tick_gated_by_optout(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, UG_X, strlen(UG_X), "the interview", 13,
                                            "anxious", 7, 0.8f),
                 HU_OK);
    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)UG_X;
    cp.proactive_checkin = true;
    cp.proactive_channel = (char *)"imessage:" UG_X;
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.contacts = &cp;
    persona.contacts_count = 1;
    struct hu_agent agent = {0};
    agent.memory = &mem;
    agent.alloc = &alloc;
    agent.persona = &persona;
    hu_channel_t chan = ug_channel();
    hu_service_channel_t chans[1] = {{.channel = &chan}};
    hu_proactive_context_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    int64_t now = ug_local_noon(4); /* the moment is due by then */

    HU_ASSERT_TRUE(
        hu_contact_optout_observe_db(db, UG_X, strlen(UG_X), "stop texting me", 15, now - 60));
    hu_daemon_f25_checkins_tick(&alloc, &agent, chans, 1, &pctx, now);
    HU_ASSERT_EQ(g_ug.sends, 0);
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_f25"), 0);

    /* Consent restored → the same due moment IS delivered and charged. */
    sqlite3_exec(db, "DELETE FROM contact_suppressions", NULL, NULL, NULL);
    HU_ASSERT_FALSE(hu_contact_optout_is_suppressed_db(db, UG_X));
    hu_daemon_f25_checkins_tick(&alloc, &agent, chans, 1, &pctx, now);
    HU_ASSERT_EQ(g_ug.sends, 1);
    HU_ASSERT_STR_EQ(g_ug.last_msg, "hey how are you doing with the interview?");
    HU_ASSERT_EQ(ug_sent_rows(db, UG_X, "unprompted_f25"), 1);
    mem.vtable->deinit(mem.ctx);
}

void run_unprompted_gate_tests(void) {
    HU_TEST_SUITE("unprompted_gate");
    HU_RUN_TEST(test_proactive_cap_survives_restart);
    HU_RUN_TEST(test_proactive_gate_denies_opted_out_contact);
    HU_RUN_TEST(test_cron_directed_send_denied_for_opted_out_contact);
    HU_RUN_TEST(test_every_kind_denied_when_opted_out);
    HU_RUN_TEST(test_every_kind_denied_in_quiet_hours);
    HU_RUN_TEST(test_every_kind_denied_over_cap_and_cap_counts_every_kind);
    HU_RUN_TEST(test_cooloff_is_per_contact);
    HU_RUN_TEST(test_cooloff_ignores_sends_before_epoch);
    HU_RUN_TEST(test_violent_text_blocked_by_stage7_and_clean_text_passes);
    HU_RUN_TEST(test_spend_it_all_is_not_blocked_at_send);
    HU_RUN_TEST(test_cron_directed_send_allowed_for_consenting_contact);
    HU_RUN_TEST(test_cron_undirected_denied_except_owner_sink);
    HU_RUN_TEST(test_cron_owner_sink_moderation_blocks);
    HU_RUN_TEST(test_owner_scheduled_delivered_with_budget_exhausted);
    HU_RUN_TEST(test_email_fallback_contact_is_one_key);
    HU_RUN_TEST(test_no_ledger_fails_closed);
    HU_RUN_TEST(test_sched_bump_gated_but_owner_scheduled_unchanged);
    HU_RUN_TEST(test_sched_kind_round_trips_through_scheduled_json);
    HU_RUN_TEST(test_photo_share_gated_by_cap);
    HU_RUN_TEST(test_f25_tick_gated_by_optout);
}

#else

void run_unprompted_gate_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
