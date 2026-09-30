/* tests/test_daemon_prospective_time.c
 *
 * The proactive tick's time-cued producers (src/daemon/daemon_prospective_time.c),
 * moved out of hu_service_run unchanged: the F20 commitment follow-up lines
 * and the proposer's due_followups section. Task 9 adds the
 * HU_PROSPECTIVE_TIME gate on top of these. */
#include "test_framework.h"

#include "human/agent.h"
#include "human/daemon/prospective_time.h"
#include <stdlib.h>
#include <string.h>

/* F18: every gate test starts and ends with both gates unset. */
static void t_env_clear(void) {
    unsetenv("HU_PROSPECTIVE_TIME");
    unsetenv("HU_PROSPECTIVE");
}

static void time_producers_need_memory_and_a_contact(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent)); /* no memory */
    char *ctx = (char *)&agent;
    size_t len = 9;
    int64_t ids[3];
    size_t n = 9;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, "+15550000001", 100, &ctx, &len, ids, &n);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_EQ(n, (size_t)0);
    char buf[64] = "stale";
    int64_t listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, "+15550000001",
                                                     100, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(listed, (int64_t)-1);
    t_env_clear();
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/superhuman.h"

static void commitment_ctx_lists_this_contacts_due_commitments(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000001", 12,
                                                "call the dentist", 16, "me", 2, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000002", 12,
                                                "return the drill", 16, "me", 2, 1000),
                 HU_OK);
    char *ctx = NULL;
    size_t len = 0;
    int64_t ids[3];
    size_t n = 0;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, "+15550000001", 5000, &ctx, &len, ids, &n);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_EQ(ctx, "COMMITMENT FOLLOW-UP: call the dentist was due. Ask if it happened: "
                          "'hey did you ever call the dentist?'\n");
    HU_ASSERT_EQ(len, strlen(ctx));
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_TRUE(ids[0] > 0);
    alloc.free(alloc.ctx, ctx, len + 1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

static void due_followups_lists_one_line_for_this_contact(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000002", 12,
                                                         "their trip", 10, 500, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000001", 12,
                                                         "the job interview", 17, 1000, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000001", 12,
                                                         "the move", 8, 2000, NULL, 0),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    size_t n = hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, "+15550000001",
                                                   5000, buf, sizeof(buf), &listed);
    HU_ASSERT_STR_EQ(buf, "- the job interview (due 4000s ago)\n"); /* oldest due, one line */
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_TRUE(listed > 0);
    listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, "+15550000003",
                                                     5000, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_EQ(listed, (int64_t)-1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* ── HU_PROSPECTIVE_TIME (task 9) ─────────────────────────────────────── */
#include "human/agent/contextual_proactive.h"
#include "human/core/log.h"
#include "human/daemon/prospective.h"
#include "human/memory/prospective_policy.h"
#include <sqlite3.h>
#include <stdio.h>
#include <unistd.h>

#define TA   "+15550000001"
#define TB   "+15550000002"
#define TNOW ((int64_t)1790000000)
#define TDAY ((int64_t)86400)

typedef struct tmock {
    int calls;
    int saw_history;   /* a judge request carried the channel's history line */
    const char *reply; /* the judge's answer; NULL = "fire" */
    int lease_calls;   /* requests that judged "call about the lease" */
    int drill_calls;   /* requests that judged "return the drill" */
} tmock_t;

static bool t_req_has(const hu_chat_request_t *req, const char *needle) {
    for (size_t i = 0; req && i < req->messages_count; i++)
        if (req->messages[i].content &&
            memmem(req->messages[i].content, req->messages[i].content_len, needle, strlen(needle)))
            return true;
    return false;
}

static hu_error_t tmock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                             const char *model, size_t model_len, double temperature,
                             hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    tmock_t *m = (tmock_t *)ctx;
    m->calls++;
    m->saw_history += t_req_has(req, "them: we signed the lease");
    m->lease_calls += t_req_has(req, "call about the lease");
    m->drill_calls += t_req_has(req, "return the drill");
    memset(out, 0, sizeof(*out));
    const char *r = m->reply ? m->reply : "fire";
    size_t rl = strlen(r);
    char *c = (char *)alloc->alloc(alloc->ctx, rl + 1);
    memcpy(c, r, rl + 1);
    out->content = c;
    out->content_len = rl;
    return HU_OK;
}

static hu_provider_vtable_t s_tmock_vt;

static void agent_with_mock(hu_agent_t *agent, hu_allocator_t *alloc, hu_memory_t *mem,
                            tmock_t *m) {
    memset(agent, 0, sizeof(*agent));
    memset(&s_tmock_vt, 0, sizeof(s_tmock_vt));
    s_tmock_vt.chat = tmock_chat;
    agent->alloc = alloc;
    agent->memory = mem;
    agent->provider.ctx = m;
    agent->provider.vtable = &s_tmock_vt;
}

static int64_t t_count(hu_memory_t *mem, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(hu_sqlite_memory_get_db(mem), sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Every row of the three stores the time path can touch, one line each: a
 * before/after snapshot of ONE database proves a call wrote nothing. */
static void t_dump(hu_memory_t *mem, char *buf, size_t cap) {
    static const char *const q[] = {"SELECT * FROM prospective_memories ORDER BY rowid",
                                    "SELECT * FROM commitments ORDER BY rowid",
                                    "SELECT * FROM delayed_followups ORDER BY rowid"};
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        sqlite3_stmt *st = NULL;
        HU_ASSERT_EQ(sqlite3_prepare_v2(hu_sqlite_memory_get_db(mem), q[i], -1, &st, NULL),
                     SQLITE_OK);
        while (sqlite3_step(st) == SQLITE_ROW) {
            for (int c = 0; c < sqlite3_column_count(st); c++) {
                const unsigned char *t = sqlite3_column_text(st, c);
                int w = snprintf(buf + pos, cap - pos, "%s|", t ? (const char *)t : "NULL");
                HU_ASSERT_TRUE(w > 0 && pos + (size_t)w < cap);
                pos += (size_t)w;
            }
            HU_ASSERT_TRUE(pos + 1 < cap);
            buf[pos++] = '\n';
            buf[pos] = '\0';
        }
        sqlite3_finalize(st);
    }
}

typedef struct t_call {
    hu_allocator_t *alloc;
    hu_agent_t *agent;
    int64_t now;
    char out[640];
    size_t n;
    int64_t listed;
    char *ctx;
    size_t ctx_len;
    size_t ids_n;
} t_call_t;

/* One proactive tick for TA as hu_service_run_proactive_checkins drives it:
 * commitment ctx, due follow-ups, then the after-send hook. */
static void t_tick(void *p) {
    t_call_t *c = (t_call_t *)p;
    int64_t ids[3];
    c->ctx = NULL;
    c->ctx_len = 0;
    c->ids_n = 0;
    hu_daemon_prospective_commitment_ctx(c->alloc, c->agent, TA, c->now, &c->ctx, &c->ctx_len, ids,
                                         &c->ids_n);
    c->listed = -1;
    c->n = hu_daemon_prospective_due_followups(c->alloc, c->agent, NULL, NULL, 0, TA, c->now,
                                               c->out, sizeof(c->out), &c->listed);
    static const char sent[] = "hey did you ever call about the lease?";
    hu_daemon_prospective_time_after_send(c->agent, TA, sent, sizeof(sent) - 1, c->now + 60);
}

/* Run `fn(arg)` with INFO logging captured from stderr into buf. */
static void t_capture(void (*fn)(void *), void *arg, char *buf, size_t cap) {
    FILE *tmp = tmpfile();
    HU_ASSERT_NOT_NULL(tmp);
    fflush(stderr);
    int saved = dup(fileno(stderr));
    HU_ASSERT_TRUE(saved >= 0);
    HU_ASSERT_TRUE(dup2(fileno(tmp), fileno(stderr)) >= 0);
    hu_log_level_set_for_test((int)HU_LOG_LEVEL_INFO);
    fn(arg);
    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    hu_log_level_set_for_test(-1);
    rewind(tmp);
    size_t n = fread(buf, 1, cap - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);
}

/* A's dated promise and its F20 follow-up twin, due an hour before `now`. */
static void t_seed_lease(hu_memory_t *mem, hu_allocator_t *alloc, int64_t now) {
    HU_ASSERT_EQ(hu_superhuman_commitment_store(mem, alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, now - 3600),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(mem, alloc, TA, 12, "call about the lease",
                                                         20, now - 3600, "me", 2),
                 HU_OK);
}

/* F11: OFF is the legacy tick, byte for byte: same outputs as a run with the
 * env unset, no judge call, no row changed, and nothing logged but the
 * one-time off banner. */
static void time_off_changes_nothing(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent_a;
    static hu_agent_t agent_b;
    agent_with_mock(&agent_a, &alloc, &ma, &m);
    agent_with_mock(&agent_b, &alloc, &mb, &m);
    t_seed_lease(&ma, &alloc, TNOW);
    t_seed_lease(&mb, &alloc, TNOW);
    static char before[8192];
    static char after[8192];
    t_dump(&ma, before, sizeof(before));
    static t_call_t off;
    static t_call_t unset;
    memset(&off, 0, sizeof(off));
    memset(&unset, 0, sizeof(unset));
    off.alloc = unset.alloc = &alloc;
    off.agent = &agent_a;
    unset.agent = &agent_b;
    off.now = unset.now = TNOW;
    setenv("HU_PROSPECTIVE_TIME", "off", 1);
    static char log[8192];
    t_capture(t_tick, &off, log, sizeof(log));
    unsetenv("HU_PROSPECTIVE_TIME");
    t_tick(&unset);
    t_dump(&ma, after, sizeof(after));
    HU_ASSERT_STR_EQ(after, before); /* no row changed */
    HU_ASSERT_EQ(m.calls, 0);        /* never judged */
    HU_ASSERT_TRUE(off.n > 0);
    HU_ASSERT_EQ(off.n, unset.n);
    HU_ASSERT_STR_EQ(off.out, unset.out);
    HU_ASSERT_STR_EQ(off.out, "- call about the lease (due 3600s ago)\n");
    HU_ASSERT_TRUE(off.listed > 0);
    HU_ASSERT_EQ(off.listed, unset.listed);
    HU_ASSERT_NOT_NULL(off.ctx);
    HU_ASSERT_NOT_NULL(unset.ctx);
    HU_ASSERT_STR_EQ(off.ctx, unset.ctx);
    HU_ASSERT_EQ(off.ids_n, (size_t)1);
    HU_ASSERT_EQ(off.ids_n, unset.ids_n);
    /* Logged: the off banner at most once (it is once per process), and
     * no other line at all. */
    const char *banner = hu_prospective_gate_banner(HU_GATE_OFF, true);
    size_t lines = 0;
    for (const char *p = log; *p;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > 0) {
            lines++;
            HU_ASSERT_TRUE(memmem(p, ll, banner, strlen(banner)) != NULL);
        }
        p += ll + (nl ? 1 : 0);
    }
    HU_ASSERT_TRUE(lines <= 1);
    alloc.free(alloc.ctx, off.ctx, off.ctx_len + 1);
    alloc.free(alloc.ctx, unset.ctx, unset.ctx_len + 1);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
    t_env_clear();
}

/* The legacy read took the 3 oldest due commitments GLOBALLY, then filtered
 * by contact: another contact's backlog hid A's due item. LIVE reads A's own
 * due set. */
static void time_live_uses_the_per_contact_due_set(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "return the drill", 16, "me",
                                                2, TNOW - 9000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "send the photos", 15, "me",
                                                2, TNOW - 8000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "book the cabin", 14, "me", 2,
                                                TNOW - 7000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, TNOW - 3600),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    size_t n = hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                   sizeof(buf), &listed);
    HU_ASSERT_STR_EQ(buf, "- call about the lease\n");
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_EQ(listed, (int64_t)-1); /* the legacy mark-sent stays out of LIVE */
    HU_ASSERT_EQ(m.calls, 1);
    char *ctx = NULL;
    size_t cl = 0;
    int64_t ids[3];
    size_t idn = 0;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, TA, TNOW, &ctx, &cl, ids, &idn);
    HU_ASSERT_NULL(ctx); /* LIVE: covered by the due set, never twice */
    HU_ASSERT_EQ(idn, (size_t)0);
    /* B's backlog is untouched by A's pass */
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE contact_id='" TB
                               "' AND status='pending' AND surfaced_at IS NULL"),
                 (int64_t)3);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

static void time_live_caps_one_per_contact_per_day(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, TNOW - 3600),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "return the drill", 16, "me",
                                                2, TNOW - 1800),
                 HU_OK);
    /* LIVE: no COMMITMENT FOLLOW-UP lines, though the legacy read would
     * list both of these (A has no backlog here to hide them). */
    char *ctx = NULL;
    size_t cl = 0;
    int64_t ids[3];
    size_t idn = 9;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, TA, TNOW, &ctx, &cl, ids, &idn);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(cl, (size_t)0);
    HU_ASSERT_EQ(idn, (size_t)0);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                       sizeof(buf), &listed) > 0);
    HU_ASSERT_STR_EQ(buf, "- call about the lease\n"); /* one line, not two */
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW + 600,
                                                     buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(listed, (int64_t)-1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

static void time_after_send_marks_done_and_retires_the_ledger(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    t_seed_lease(&mem, &alloc, TNOW);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                       sizeof(buf), &listed) > 0);
    HU_ASSERT_EQ(listed, (int64_t)-1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)1);
    static const char sent[] = "hey did you ever call about the lease?";
    /* OFF and SHADOW: the hook settles nothing, even with a surfaced row
     * and a text that carries it. */
    static char before[8192];
    static char after[8192];
    t_dump(&mem, before, sizeof(before));
    unsetenv("HU_PROSPECTIVE_TIME");
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 30);
    setenv("HU_PROSPECTIVE_TIME", "off", 1);
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 40);
    setenv("HU_PROSPECTIVE_TIME", "shadow", 1);
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 50);
    t_dump(&mem, after, sizeof(after));
    HU_ASSERT_STR_EQ(after, before);
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 60);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE "
                               "cue_kind='time' AND status='done' AND outcome='used'"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM commitments WHERE status='followed_up'"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM delayed_followups WHERE sent=1"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* Ownership ruling (b): a delivery to B never settles A's surfaced item,
 * even when B's text would carry A's action. */
static void time_after_send_is_owner_scoped(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    t_seed_lease(&mem, &alloc, TNOW);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                       sizeof(buf), &listed) > 0);
    static const char sent[] = "hey did you ever call about the lease?";
    hu_daemon_prospective_time_after_send(&agent, TB, sent, sizeof(sent) - 1, TNOW + 60);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE contact_id='" TA
                               "' AND status='surfaced' AND attempts=0"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM commitments WHERE status='pending'"),
                 (int64_t)1);
    /* A's own delivery that does not carry it: an attempt, not done. */
    static const char other[] = "hey hows the new job going";
    hu_daemon_prospective_time_after_send(&agent, TA, other, sizeof(other) - 1, TNOW + 120);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE contact_id='" TA
                               "' AND status='pending' AND attempts=1"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM commitments WHERE status='pending'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* F4: a promise the CONTACT made renders as a question about them, from the
 * action Task 8 stored — never Seth's first person. */
static void time_live_renders_a_contact_promise_in_third_person(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "I'll send the photos", 20,
                                                "them", 4, TNOW - 3600),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    size_t n = hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                   sizeof(buf), &listed);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "- ask if they still need to send the photos\n");
    HU_ASSERT_NULL(strstr(buf, "I will"));
    HU_ASSERT_NULL(strstr(buf, "I'll"));
    HU_ASSERT_NULL(strstr(buf, "I "));
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* Parked M4 from task 8: an action queued with its relative day baked in
 * ("(in 3 days)") is stale by the time it is due, so the due line drops it.
 * The stored action is unchanged. (Dated-moment frames no longer carry one
 * into the store at all: see time_mirror_stores_a_situation_frames_topic.) */
static void time_live_drops_a_stale_relative_day(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    static const char topic[] = "ask how the move went (in 3 days)";
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, TA, 12, topic,
                                                         sizeof(topic) - 1, TNOW - 3600, NULL, 0),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                       sizeof(buf), &listed) > 0);
    HU_ASSERT_STR_EQ(buf, "- ask how the move went\n");
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE action="
                               "'ask how the move went (in 3 days)'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* Build the frame exactly as the dated-moment path does, so a format change
 * there fails here instead of silently mirroring the wrapper again. */
static size_t t_frame(const char *topic, int64_t send_at, int64_t now, char *out, size_t cap) {
    hu_contextual_proactive_decision_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.topic, sizeof(d.topic), "%s", topic);
    d.send_at_ms = send_at * 1000;
    d.confidence = 0.8;
    size_t n = hu_contextual_proactive_situation_frame(&d, now, out, cap);
    HU_ASSERT_TRUE(n > 0);
    return n;
}

/* Fix round 1 (i): the time mirror of a dated-moment frame is its topic —
 * no stale "(tomorrow)", no "confidence 0.80" — while the ledger row keeps
 * the frame, so the OFF output is unchanged. */
static void time_mirror_stores_a_situation_frames_topic(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    char frame[320];
    size_t fl = t_frame("the job interview", TNOW + TDAY + 60, TNOW, frame, sizeof(frame));
    HU_ASSERT_STR_EQ(frame, "they mentioned the job interview (tomorrow); confidence 0.80");
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, TA, 12, frame, fl,
                                                         TNOW + TDAY + 60, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time' "
                               "AND action='the job interview'"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories"), (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM delayed_followups WHERE topic="
                               "'they mentioned the job interview (tomorrow); confidence 0.80'"),
                 (int64_t)1);
    /* OFF: the legacy line still quotes the ledger's frame, byte for byte */
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA,
                                                       TNOW + TDAY + 120, buf, sizeof(buf),
                                                       &listed) > 0);
    HU_ASSERT_STR_EQ(buf, "- they mentioned the job interview (tomorrow); confidence 0.80 "
                          "(due 60s ago)\n");
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* Two detections of the same moment on different days carry different
 * relative words; their topics are equal, so they are ONE open intention. */
static void time_mirror_collapses_frames_of_the_same_topic(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    char f1[320];
    char f2[320];
    size_t l1 = t_frame("the job interview", TNOW + TDAY + 60, TNOW, f1, sizeof(f1));
    size_t l2 = t_frame("the job interview", TNOW + 2 * TDAY + 60, TNOW, f2, sizeof(f2));
    HU_ASSERT_STR_CONTAINS(f1, "(tomorrow)");
    HU_ASSERT_STR_CONTAINS(f2, "(in 2 days)");
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, TA, 12, f1, l1,
                                                         TNOW + TDAY + 60, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, TA, 12, f2, l2,
                                                         TNOW + 2 * TDAY + 60, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM delayed_followups"), (int64_t)2);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time' "
                               "AND status IN ('pending','surfaced')"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* The rowid-keyed twin retire (task 8 round 3) still finds the ledger row
 * of a topic-mirrored frame: a delivered reply about the topic marks the
 * intention done and the delayed follow-up sent. */
static void time_topic_mirrored_frame_retires_its_ledger_row(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    char frame[320];
    size_t fl = t_frame("the job interview", TNOW - 3600, TNOW - 2 * TDAY, frame, sizeof(frame));
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, TA, 12, frame, fl,
                                                         TNOW - 3600, NULL, 0),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                       sizeof(buf), &listed) > 0);
    HU_ASSERT_STR_EQ(buf, "- the job interview\n");
    static const char sent[] = "hey how did the job interview go?";
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 60);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' "
                               "AND outcome='used'"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM delayed_followups WHERE sent=1"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

static hu_error_t t_history(void *ctx, hu_allocator_t *alloc, const char *contact_id,
                            size_t contact_id_len, size_t limit, hu_channel_history_entry_t **out,
                            size_t *out_count) {
    (void)ctx;
    (void)contact_id;
    (void)contact_id_len;
    (void)limit;
    hu_channel_history_entry_t *e =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, sizeof(*e));
    memset(e, 0, sizeof(*e));
    snprintf(e->text, sizeof(e->text), "we signed the lease");
    *out = e;
    *out_count = 1;
    return HU_OK;
}

/* The fire-time check sees the send channel's history (the proactive tick
 * has no conversation loaded). */
static void time_live_judges_with_the_channel_history(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    t_seed_lease(&mem, &alloc, TNOW);
    static hu_channel_vtable_t cvt;
    memset(&cvt, 0, sizeof(cvt));
    cvt.load_conversation_history = t_history;
    hu_channel_t ch = {.ctx = NULL, .vtable = &cvt};
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, &ch, TA, 12, TA, TNOW, buf,
                                                       sizeof(buf), &listed) > 0);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(m.saw_history, 1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

typedef struct t_shadow {
    hu_allocator_t *alloc;
    hu_agent_t *agent;
    int64_t now;
    char out[640];
    size_t n;
    int64_t listed;
} t_shadow_t;

static void t_shadow_due(void *p) {
    t_shadow_t *s = (t_shadow_t *)p;
    s->listed = -1;
    s->n = hu_daemon_prospective_due_followups(s->alloc, s->agent, NULL, NULL, 0, TA, s->now,
                                               s->out, sizeof(s->out), &s->listed);
}

/* SHADOW: the legacy output, byte for byte (twin store run OFF), plus ONE
 * read-only v2 pass per contact per day — judged, logged with the
 * would-send ids, and not a single row written. */
static void time_shadow_keeps_legacy_output_and_store(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent_a;
    static hu_agent_t agent_b;
    agent_with_mock(&agent_a, &alloc, &ma, &m);
    agent_with_mock(&agent_b, &alloc, &mb, &m);
    /* a day no other test uses: the once-per-day slot is process-wide */
    const int64_t now = TNOW + 10 * TDAY + 3600;
    t_seed_lease(&ma, &alloc, now);
    t_seed_lease(&mb, &alloc, now);
    static t_shadow_t off;
    static t_shadow_t sh;
    memset(&off, 0, sizeof(off));
    memset(&sh, 0, sizeof(sh));
    off.alloc = sh.alloc = &alloc;
    off.agent = &agent_a;
    sh.agent = &agent_b;
    off.now = sh.now = now;
    t_shadow_due(&off);
    static char before[8192];
    static char after[8192];
    t_dump(&mb, before, sizeof(before));
    setenv("HU_PROSPECTIVE_TIME", "shadow", 1);
    static char log[8192];
    t_capture(t_shadow_due, &sh, log, sizeof(log));
    t_dump(&mb, after, sizeof(after));
    HU_ASSERT_STR_EQ(after, before); /* read-only */
    HU_ASSERT_TRUE(off.n > 0);
    HU_ASSERT_EQ(off.n, sh.n);
    HU_ASSERT_STR_EQ(off.out, sh.out);
    HU_ASSERT_EQ(off.listed, sh.listed); /* legacy mark-sent unchanged in SHADOW */
    HU_ASSERT_TRUE(sh.listed > 0);
    HU_ASSERT_EQ(m.calls, 1); /* SHADOW judged once */
    int64_t id = t_count(&mb, "SELECT id FROM prospective_memories WHERE cue_kind='time'");
    HU_ASSERT_STR_CONTAINS(log, "prospective time shadow: candidates=1 fire=1 resolved=0 "
                                "cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 "
                                "capped=0 write_err=0\n");
    char item[96];
    snprintf(item, sizeof(item), "prospective time shadow item: id=%lld verdict=fire\n",
             (long long)id);
    HU_ASSERT_STR_CONTAINS(log, item);
    /* the same contact again the same day: legacy output, no second pass */
    t_capture(t_shadow_due, &sh, log, sizeof(log));
    HU_ASSERT_STR_EQ(off.out, sh.out);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_NULL(strstr(log, "prospective time shadow:"));
    /* the next local day: one more pass */
    sh.now = now + TDAY;
    t_shadow_due(&sh);
    HU_ASSERT_EQ(m.calls, 2);
    /* SHADOW never settles anything after a send */
    static const char sent[] = "hey did you ever call about the lease?";
    hu_daemon_prospective_time_after_send(&agent_b, TA, sent, sizeof(sent) - 1, now + 60);
    t_dump(&mb, after, sizeof(after));
    HU_ASSERT_STR_EQ(after, before);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
    t_env_clear();
}
/* Fix round 1 (ii): SHADOW takes a contact's daily slot only when the pass
 * had something to judge. An item due at 15:00 whose contact first ticks at
 * 09:00 is judged at the 15:30 tick, as LIVE would — not a day late. */
static void time_shadow_slot_waits_for_something_due(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    /* a day no other test uses: the slot table is process-wide */
    const int64_t day = hu_prospective_local_day_start(TNOW + 20 * TDAY);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, day + 15 * 3600),
                 HU_OK);
    setenv("HU_PROSPECTIVE_TIME", "shadow", 1);
    static t_shadow_t sh;
    memset(&sh, 0, sizeof(sh));
    sh.alloc = &alloc;
    sh.agent = &agent;
    sh.now = day + 9 * 3600; /* 09:00: nothing due yet */
    static char log[8192];
    t_capture(t_shadow_due, &sh, log, sizeof(log));
    HU_ASSERT_EQ(m.calls, 0);
    HU_ASSERT_NULL(strstr(log, "prospective time shadow:"));
    sh.now = day + 15 * 3600 + 1800; /* 15:30 the same day */
    t_capture(t_shadow_due, &sh, log, sizeof(log));
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_STR_CONTAINS(log, "prospective time shadow: candidates=1 fire=1 ");
    sh.now = day + 16 * 3600; /* judged once today: the slot is now taken */
    t_capture(t_shadow_due, &sh, log, sizeof(log));
    HU_ASSERT_EQ(m.calls, 1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* Known gap 1: a counting history loader, the channel seam pm_time_history
 * already reads through. */
static hu_error_t t_history_counted(void *ctx, hu_allocator_t *alloc, const char *contact_id,
                                    size_t contact_id_len, size_t limit,
                                    hu_channel_history_entry_t **out, size_t *out_count) {
    (*(int *)ctx)++;
    return t_history(NULL, alloc, contact_id, contact_id_len, limit, out, out_count);
}

static hu_channel_vtable_t s_counted_cvt;

static hu_channel_t t_counted_channel(int *loads) {
    memset(&s_counted_cvt, 0, sizeof(s_counted_cvt));
    s_counted_cvt.load_conversation_history = t_history_counted;
    hu_channel_t ch = {.ctx = loads, .vtable = &s_counted_cvt};
    return ch;
}

/* A contact with nothing due -- only a future row, and another contact's due
 * backlog -- costs no chat history read on any tick, in SHADOW or LIVE: the
 * pending-due count comes first. */
static void time_nothing_due_loads_no_history(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    /* days no other test uses: the SHADOW slot table is process-wide */
    const int64_t day = hu_prospective_local_day_start(TNOW + 50 * TDAY);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, day + 20 * 3600),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "return the drill", 16, "me",
                                                2, day + 3600),
                 HU_OK);
    int loads = 0;
    hu_channel_t ch = t_counted_channel(&loads);
    char buf[640];
    int64_t listed = -1;
    static const char *const modes[] = {"shadow", "live"};
    for (size_t i = 0; i < 2; i++) {
        setenv("HU_PROSPECTIVE_TIME", modes[i], 1);
        for (int tick = 0; tick < 3; tick++) /* 09:00, 09:30, 10:00: TA has nothing due */
            (void)hu_daemon_prospective_due_followups(&alloc, &agent, &ch, TA, 12, TA,
                                                      day + 9 * 3600 + tick * 1800, buf,
                                                      sizeof(buf), &listed);
    }
    HU_ASSERT_EQ(loads, 0);
    HU_ASSERT_EQ(m.calls, 0);
    /* the positive control: once TA's row is due, LIVE reads the history */
    (void)hu_daemon_prospective_due_followups(&alloc, &agent, &ch, TA, 12, TA, day + 21 * 3600, buf,
                                              sizeof(buf), &listed);
    HU_ASSERT_EQ(loads, 1);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(m.saw_history, 1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

typedef struct t_shadow_ch {
    t_shadow_t s;
    hu_channel_t *ch;
} t_shadow_ch_t;

static void t_shadow_due_ch(void *p) {
    t_shadow_ch_t *c = (t_shadow_ch_t *)p;
    c->s.listed = -1;
    c->s.n =
        hu_daemon_prospective_due_followups(c->s.alloc, c->s.agent, c->ch, TA, 12, TA, c->s.now,
                                            c->s.out, sizeof(c->s.out), &c->s.listed);
}

/* With a row due, SHADOW still loads the history once and logs exactly the
 * counts it logged before the pending-due check existed. */
static void time_shadow_due_loads_history_once(void) {
    t_env_clear();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    const int64_t now = TNOW + 51 * TDAY + 3600; /* a day no other test uses */
    t_seed_lease(&mem, &alloc, now);
    int loads = 0;
    hu_channel_t ch = t_counted_channel(&loads);
    static t_shadow_ch_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.s.alloc = &alloc;
    sc.s.agent = &agent;
    sc.s.now = now;
    sc.ch = &ch;
    setenv("HU_PROSPECTIVE_TIME", "shadow", 1);
    static char log[8192];
    t_capture(t_shadow_due_ch, &sc, log, sizeof(log));
    HU_ASSERT_EQ(loads, 1);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(m.saw_history, 1);
    HU_ASSERT_STR_CONTAINS(log, "prospective time shadow: candidates=1 fire=1 resolved=0 "
                                "cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 "
                                "capped=0 write_err=0\n");
    HU_ASSERT_STR_EQ(sc.s.out, "- call about the lease (due 3600s ago)\n"); /* legacy output */
    sc.s.now = now + 1800; /* the same day: slot taken, no second read */
    t_capture(t_shadow_due_ch, &sc, log, sizeof(log));
    HU_ASSERT_EQ(loads, 1);
    HU_ASSERT_EQ(m.calls, 1);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}

/* Known gap 3: LIVE remembers a not_now for the rest of the local day. The
 * same intention is judged once per day; the next day it is judged again; a
 * different intention of the same contact the same day still is. */
static void time_live_not_now_is_judged_once_a_day(void) {
    t_env_clear();
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    m.reply = "not_now";
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    /* days no other test uses: the memo is process-wide */
    const int64_t day = hu_prospective_local_day_start(TNOW + 40 * TDAY);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, day + 8 * 3600),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA,
                                                     day + 9 * 3600, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_EQ(m.lease_calls, 1);
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA,
                                                     day + 10 * 3600, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_EQ(m.lease_calls, 1); /* the second tick of the day: not re-judged */
    HU_ASSERT_EQ(m.calls, 1);
    /* a different intention of the same contact, the same day, is judged */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "return the drill", 16, "me",
                                                2, day + 10 * 3600),
                 HU_OK);
    (void)hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, day + 11 * 3600,
                                              buf, sizeof(buf), &listed);
    HU_ASSERT_EQ(m.drill_calls, 1);
    HU_ASSERT_EQ(m.lease_calls, 1);
    HU_ASSERT_EQ(m.calls, 2);
    /* still pending: a not_now leaves the intention for another day */
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time' "
                               "AND status='pending' AND surfaced_at IS NULL"),
                 (int64_t)2);
    /* the next local day: both are judged again, once each */
    (void)hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA,
                                              day + TDAY + 9 * 3600, buf, sizeof(buf), &listed);
    HU_ASSERT_EQ(m.lease_calls, 2);
    HU_ASSERT_EQ(m.drill_calls, 2);
    (void)hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA,
                                              day + TDAY + 10 * 3600, buf, sizeof(buf), &listed);
    HU_ASSERT_EQ(m.calls, 4);
    mem.vtable->deinit(mem.ctx);
    t_env_clear();
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_prospective_time_tests(void) {
    HU_TEST_SUITE("daemon prospective time");
    HU_RUN_TEST(time_producers_need_memory_and_a_contact);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(commitment_ctx_lists_this_contacts_due_commitments);
    HU_RUN_TEST(due_followups_lists_one_line_for_this_contact);
    HU_RUN_TEST(time_off_changes_nothing);
    HU_RUN_TEST(time_live_uses_the_per_contact_due_set);
    HU_RUN_TEST(time_live_caps_one_per_contact_per_day);
    HU_RUN_TEST(time_after_send_marks_done_and_retires_the_ledger);
    HU_RUN_TEST(time_after_send_is_owner_scoped);
    HU_RUN_TEST(time_live_renders_a_contact_promise_in_third_person);
    HU_RUN_TEST(time_live_drops_a_stale_relative_day);
    HU_RUN_TEST(time_live_judges_with_the_channel_history);
    HU_RUN_TEST(time_shadow_keeps_legacy_output_and_store);
    HU_RUN_TEST(time_mirror_stores_a_situation_frames_topic);
    HU_RUN_TEST(time_mirror_collapses_frames_of_the_same_topic);
    HU_RUN_TEST(time_topic_mirrored_frame_retires_its_ledger_row);
    HU_RUN_TEST(time_shadow_slot_waits_for_something_due);
    HU_RUN_TEST(time_nothing_due_loads_no_history);
    HU_RUN_TEST(time_shadow_due_loads_history_once);
    HU_RUN_TEST(time_live_not_now_is_judged_once_a_day);
#endif
}
