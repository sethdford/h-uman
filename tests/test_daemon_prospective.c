/* tests/test_daemon_prospective.c
 *
 * HU_PROSPECTIVE in the reactive path (src/daemon/daemon_prospective.c; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.4).
 * OFF is byte-identical to the legacy directive and never calls the judge;
 * SHADOW returns the legacy directive and changes nothing the legacy path
 * would not; LIVE returns the soft directive and leaves `fired` alone until a
 * delivered reply carries the action; a judge failure is silence. Twin
 * in-memory stores prove the byte-identity. */
#include "test_framework.h"

#include "human/channel.h"
#include "human/daemon/prospective.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* History rendering is pure (no SQLite). */
static void history_render_keeps_last_twenty_lines_oldest_first(void) {
    hu_channel_history_entry_t e[25];
    memset(e, 0, sizeof(e));
    for (int i = 0; i < 25; i++) {
        e[i].from_me = (i % 2) == 1;
        snprintf(e[i].text, sizeof(e[i].text), "line %02d", i);
    }
    char buf[2048];
    size_t n = hu_daemon_prospective_history_render(e, 25, buf, sizeof(buf));
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_NULL(strstr(buf, "line 04"));
    HU_ASSERT_TRUE(strncmp(buf, "me: line 05\n", 12) == 0);
    HU_ASSERT_TRUE(n >= 14 && strcmp(buf + n - 14, "them: line 24\n") == 0);
    HU_ASSERT_EQ(hu_daemon_prospective_history_render(NULL, 0, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
}

static void history_render_keeps_the_newest_lines_that_fit(void) {
    hu_channel_history_entry_t e[3];
    memset(e, 0, sizeof(e));
    memset(e[0].text, 'x', 40); /* oldest, too big to keep */
    e[1].from_me = true;
    snprintf(e[1].text, sizeof(e[1].text), "yes");
    snprintf(e[2].text, sizeof(e[2].text), "ok go");
    char buf[32];
    size_t n = hu_daemon_prospective_history_render(e, 3, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "me: yes\nthem: ok go\n");
    HU_ASSERT_EQ(n, (size_t)20);
}

#ifdef HU_ENABLE_SQLITE
#include "human/agent.h"
#include "human/core/log.h"
#include "human/memory.h"
#include "human/memory/prospective.h"
#include "human/persona.h"
#include <sqlite3.h>
#include <time.h>
#include <unistd.h>

#define C1  "+15550000001"
#define C2  "+15550000002"
#define NOW ((int64_t)1790000000)

typedef struct fixed {
    const char *reply;
    int calls;
    hu_error_t err;
} fixed_t;

static hu_error_t fixed_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                              size_t system_len, const char *user, size_t user_len, char **out,
                              size_t *out_len) {
    fixed_t *f = (fixed_t *)ctx;
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    f->calls++;
    if (f->err != HU_OK)
        return f->err;
    size_t n = strlen(f->reply);
    char *b = (char *)alloc->alloc(alloc->ctx, n + 1);
    memcpy(b, f->reply, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static void seed_kw(sqlite3 *db, const char *cue, const char *action) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','%s','%s','" C1 "',0,0,%lld)",
             cue, action, (long long)(NOW - 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

/* A legacy keyword row with no owner: every contact's keyword read sees it. */
static void seed_kw_ownerless(sqlite3 *db, const char *cue, const char *action) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','%s','%s',NULL,0,0,%lld)",
             cue, action, (long long)(NOW - 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void seed_pair(sqlite3 *db) {
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw(db, "lasagna", "send the lasagna recipe");
}

static void dump(sqlite3 *db, char *buf, size_t cap) {
    sqlite3_stmt *st = NULL;
    size_t pos = 0;
    buf[0] = '\0';
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT id, fired, status, attempts, "
                                    "COALESCE(surfaced_at, 0), COALESCE(outcome, '') "
                                    "FROM prospective_memories ORDER BY id",
                                    -1, &st, NULL),
                 SQLITE_OK);
    while (sqlite3_step(st) == SQLITE_ROW && pos + 96 < cap) {
        int w = snprintf(buf + pos, cap - pos, "%lld/%d/%s/%d/%lld/%s;",
                         (long long)sqlite3_column_int64(st, 0), sqlite3_column_int(st, 1),
                         (const char *)sqlite3_column_text(st, 2), sqlite3_column_int(st, 3),
                         (long long)sqlite3_column_int64(st, 4),
                         (const char *)sqlite3_column_text(st, 5));
        if (w > 0)
            pos += (size_t)w;
    }
    sqlite3_finalize(st);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static hu_prospective_turn_t turn_for(const char *text) {
    hu_prospective_turn_t t;
    memset(&t, 0, sizeof(t));
    t.contact = C1;
    t.contact_len = strlen(C1);
    t.inbound = text;
    t.inbound_len = strlen(text);
    t.now = NOW;
    t.day_start = NOW - 3600;
    return t;
}

/* Hermetic env for the tests that reach the production entry points. */
static char g_tmp_home[256];

static void env_enter(void) {
    unsetenv("HU_PROSPECTIVE");
    snprintf(g_tmp_home, sizeof(g_tmp_home), "/tmp/hu_test_dpm_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_tmp_home));
    setenv("HOME", g_tmp_home, 1);
    setenv("HU_STATE_DIR", g_tmp_home, 1);
}

static void env_leave(const char *old_home) {
    unsetenv("HU_PROSPECTIVE");
    unsetenv("HU_STATE_DIR");
    if (old_home)
        setenv("HOME", old_home, 1);
    (void)rmdir(g_tmp_home);
}

static void directive_off_is_byte_identical_to_legacy_and_never_judges(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *a = hu_sqlite_memory_get_db(&ma);
    sqlite3 *b = hu_sqlite_memory_get_db(&mb);
    seed_pair(a);
    seed_pair(b);
    static const char msg[] = "taco place and lasagna";
    size_t la = 0;
    size_t lb = 0;
    char *da =
        hu_prospective_directive_build(&alloc, a, msg, sizeof(msg) - 1, C1, strlen(C1), NOW, &la);
    fixed_t f = {.reply = "fire"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for(msg);
    char *dbuf = hu_daemon_prospective_directive(&alloc, b, HU_GATE_OFF, &t, &j, &lb);
    HU_ASSERT_NOT_NULL(da);
    HU_ASSERT_NOT_NULL(dbuf);
    HU_ASSERT_EQ(la, lb);
    HU_ASSERT_TRUE(memcmp(da, dbuf, la) == 0);
    HU_ASSERT_EQ(f.calls, 0);
    char sa[1024];
    char sb[1024];
    dump(a, sa, sizeof(sa));
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb);
    /* delivery in OFF touches nothing */
    hu_daemon_prospective_on_delivered(&alloc, b, HU_GATE_OFF, C1, strlen(C1),
                                       "how was the taco place", 22, NOW + 60);
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb);
    alloc.free(alloc.ctx, da, la + 1);
    alloc.free(alloc.ctx, dbuf, lb + 1);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
}

static void directive_shadow_returns_legacy_and_writes_nothing_extra(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *a = hu_sqlite_memory_get_db(&ma);
    sqlite3 *b = hu_sqlite_memory_get_db(&mb);
    seed_pair(a);
    seed_pair(b);
    static const char msg[] = "taco place and lasagna";
    size_t la = 0;
    size_t lb = 0;
    char *da =
        hu_prospective_directive_build(&alloc, a, msg, sizeof(msg) - 1, C1, strlen(C1), NOW, &la);
    fixed_t f = {.reply = "already_resolved"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for(msg);
    char *dbuf = hu_daemon_prospective_directive(&alloc, b, HU_GATE_SHADOW, &t, &j, &lb);
    HU_ASSERT_EQ(f.calls, 2); /* Filter + Decide ran on both cued intentions */
    HU_ASSERT_EQ(la, lb);
    HU_ASSERT_TRUE(memcmp(da, dbuf, la) == 0);
    char sa[1024];
    char sb[1024];
    dump(a, sa, sizeof(sa));
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb); /* the judge said resolved; SHADOW wrote none of it */
    /* delivery in SHADOW only logs uptake: the store is untouched */
    hu_daemon_prospective_on_delivered(&alloc, b, HU_GATE_SHADOW, C1, strlen(C1),
                                       "how was the taco place", 22, NOW + 60);
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb);
    alloc.free(alloc.ctx, da, la + 1);
    alloc.free(alloc.ctx, dbuf, lb + 1);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
}

/* SHADOW calls the model only for intentions the turn's text cued, and never
 * more than HU_PROSPECTIVE_JUDGE_CAP times per turn. */
static void directive_shadow_judges_only_cued_turns_up_to_the_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_kw(db, "alpha", "ask about alpha");
    seed_kw(db, "beta", "ask about beta");
    seed_kw(db, "gamma", "ask about gamma");
    seed_kw(db, "delta", "ask about delta");
    fixed_t f = {.reply = "not_now"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for("nothing to see here");
    size_t len = 0;
    char *d = hu_daemon_prospective_directive(&alloc, db, HU_GATE_SHADOW, &t, &j, &len);
    HU_ASSERT_NULL(d); /* the legacy builder found no cue either */
    HU_ASSERT_EQ(f.calls, 0);
    t = turn_for("alpha beta gamma delta");
    d = hu_daemon_prospective_directive(&alloc, db, HU_GATE_SHADOW, &t, &j, &len);
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_EQ(f.calls, (int)HU_PROSPECTIVE_JUDGE_CAP);
    alloc.free(alloc.ctx, d, len + 1);
    m.vtable->deinit(m.ctx);
}

static void directive_live_returns_soft_directive_and_settles_on_delivery(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_pair(db);
    fixed_t f = {.reply = "fire"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for("the taco place was packed");
    size_t len = 0;
    char *d = hu_daemon_prospective_directive(&alloc, db, HU_GATE_LIVE, &t, &j, &len);
    HU_ASSERT_STR_EQ(d, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ask how "
                        "the new taco place was]");
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE fired=1"),
                 (int64_t)0); /* LIVE never marks fired at render time */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)1);
    hu_daemon_prospective_on_delivered(&alloc, db, HU_GATE_LIVE, C1, strlen(C1),
                                       "wait how was the taco place", 27, NOW + 60);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' AND "
                           "fired=1 AND outcome='used'"),
                 (int64_t)1);
    m.vtable->deinit(m.ctx);
}

static void directive_live_judge_failure_is_silent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_pair(db);
    fixed_t f = {.reply = "fire", .err = HU_ERR_PROVIDER_RESPONSE};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for("the taco place was packed");
    size_t len = 7;
    HU_ASSERT_NULL(hu_daemon_prospective_directive(&alloc, db, HU_GATE_LIVE, &t, &j, &len));
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_EQ(f.calls, 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0"),
                 (int64_t)2);
    m.vtable->deinit(m.ctx);
}

/* Ruling 2 (per-contact ordering): an owner-less legacy keyword row that
 * contact A's LIVE pass surfaced is settled only by A's pass — A's delivery,
 * or A's "did not land" settle before another contact's pass. B's delivery
 * must never mark it used against B's reply. */
static void live_ownerless_row_is_settled_only_by_the_pass_that_surfaced_it(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_kw_ownerless(db, "pizza", "ask about the pizza place");
    fixed_t f = {.reply = "fire"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t ta = turn_for("pizza tonight?"); /* contact A = C1 */
    size_t len = 0;
    char *d = hu_daemon_prospective_directive(&alloc, db, HU_GATE_LIVE, &ta, &j, &len);
    HU_ASSERT_NOT_NULL(d);
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)1);
    /* B's delivered reply carries the action's terms, but B never surfaced it */
    hu_daemon_prospective_on_delivered(&alloc, db, HU_GATE_LIVE, C2, strlen(C2),
                                       "how was the pizza place", 23, NOW + 30);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced' AND "
                           "attempts=0 AND outcome IS NULL"),
                 (int64_t)1);
    /* B's next pass: A's attempt did not land, so A's pass settles it first
     * (attempt 1, back to pending) — B's pass never reclaims it for B. */
    hu_prospective_turn_t tb = turn_for("hello there");
    tb.contact = C2;
    tb.contact_len = strlen(C2);
    tb.now = NOW + 60;
    HU_ASSERT_NULL(hu_daemon_prospective_directive(&alloc, db, HU_GATE_LIVE, &tb, &j, &len));
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "attempts=1 AND outcome='ignored'"),
                 (int64_t)1);
    /* and a late A delivery no longer has anything to settle */
    hu_daemon_prospective_on_delivered(&alloc, db, HU_GATE_LIVE, C1, strlen(C1),
                                       "how was the pizza place", 23, NOW + 90);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "attempts=1"),
                 (int64_t)1);
    m.vtable->deinit(m.ctx);
}

typedef struct mock_prov {
    int calls;
    int thinking_budget;
    uint32_t max_tokens;
    double temperature;
    char model[32];
} mock_prov_t;

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    mock_prov_t *m = (mock_prov_t *)ctx;
    m->calls++;
    m->thinking_budget = req->thinking_budget;
    m->max_tokens = req->max_tokens;
    m->temperature = temperature;
    snprintf(m->model, sizeof(m->model), "%.*s", (int)model_len, model);
    memset(out, 0, sizeof(*out));
    char *c = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(c, "fire", 5);
    out->content = c;
    out->content_len = 4;
    return HU_OK;
}

static hu_error_t mock_chat_fail(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                 const char *model, size_t model_len, double temperature,
                                 hu_chat_response_t *out) {
    mock_prov_t *m = (mock_prov_t *)ctx;
    (void)alloc;
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)out;
    m->calls++;
    return HU_ERR_PROVIDER_RESPONSE;
}

static void provider_judge_sends_a_short_thinking_off_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_prov_t m;
    memset(&m, 0, sizeof(m));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    hu_provider_t p = {.ctx = &m, .vtable = &vt};
    hu_daemon_prospective_judge_ctx_t jc = {.provider = &p, .model = "glm", .model_len = 3};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_daemon_prospective_provider_judge(&jc, &alloc, "s", 1, "u", 1, &out, &len),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "fire");
    HU_ASSERT_EQ(m.thinking_budget, 0);
    HU_ASSERT_EQ(m.max_tokens, (uint32_t)16);
    HU_ASSERT_TRUE(m.temperature == 0.0);
    HU_ASSERT_STR_EQ(m.model, "glm");
    alloc.free(alloc.ctx, out, len + 1);
    hu_daemon_prospective_judge_ctx_t none = {.provider = NULL};
    HU_ASSERT_EQ(hu_daemon_prospective_provider_judge(&none, &alloc, "s", 1, "u", 1, &out, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

/* The production entry points read HU_PROSPECTIVE and use the agent's own
 * provider — here a mock, so no network. */
static void reactive_and_delivered_entries_follow_the_env_gate(void) {
    const char *old_home_env = getenv("HOME");
    char old_home[512];
    snprintf(old_home, sizeof(old_home), "%s", old_home_env ? old_home_env : "");
    env_enter();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_kw(db, "alpha", "ask about alpha");
    seed_kw(db, "beta", "ask about beta");
    mock_prov_t mp;
    memset(&mp, 0, sizeof(mp));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.memory = &m;
    agent.provider.ctx = &mp;
    agent.provider.vtable = &vt;

    setenv("HU_PROSPECTIVE", "live", 1);
    size_t len = 0;
    char *d = hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "alpha!", 6, NULL,
                                             0, false, &len);
    HU_ASSERT_STR_CONTAINS(d, "If it fits naturally, you could bring up: ask about alpha");
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(mp.calls, 1);
    /* a group thread never reaches the judge */
    HU_ASSERT_NULL(hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "beta!", 5,
                                                  NULL, 0, true, &len));
    HU_ASSERT_EQ(mp.calls, 1);
    hu_daemon_prospective_delivered(&agent, C1, strlen(C1), "so how was alpha", 16);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done'"),
                 (int64_t)1);

    /* LIVE with a provider error: judge_ok=false -> not surfaced, no directive */
    vt.chat = mock_chat_fail;
    HU_ASSERT_NULL(hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "beta!", 5,
                                                  NULL, 0, false, &len));
    HU_ASSERT_EQ(mp.calls, 2);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending'"),
                 (int64_t)1);
    vt.chat = mock_chat;

    unsetenv("HU_PROSPECTIVE"); /* OFF: the legacy directive, no judge call */
    d = hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "beta!", 5, NULL, 0,
                                       false, &len);
    HU_ASSERT_STR_CONTAINS(d, "[PROSPECTIVE MEMORY: Remember to: ask about beta");
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(mp.calls, 2);
    m.vtable->deinit(m.ctx);
    env_leave(old_home_env ? old_home : NULL);
}

/* Ruling 3: with HU_PROSPECTIVE unset, the production entry point and the
 * pre-change call (hu_prospective_directive_build) produce the same bytes
 * and the same store on twin DBs; no model call; delivery writes nothing. */
static void reactive_off_twin_db_matches_the_pre_change_call(void) {
    const char *old_home_env = getenv("HOME");
    char old_home[512];
    snprintf(old_home, sizeof(old_home), "%s", old_home_env ? old_home_env : "");
    env_enter();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *a = hu_sqlite_memory_get_db(&ma);
    sqlite3 *b = hu_sqlite_memory_get_db(&mb);
    seed_pair(a);
    seed_pair(b);
    seed_kw_ownerless(a, "pizza", "ask about the pizza place");
    seed_kw_ownerless(b, "pizza", "ask about the pizza place");
    mock_prov_t mp;
    memset(&mp, 0, sizeof(mp));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.memory = &mb;
    agent.provider.ctx = &mp;
    agent.provider.vtable = &vt;
    static const char *const msgs[] = {"taco place and pizza", "lasagna?", "nothing"};
    for (size_t i = 0; i < 3; i++) {
        size_t n = strlen(msgs[i]);
        size_t la = 0;
        size_t lb = 0;
        char *da = hu_prospective_directive_build(&alloc, a, msgs[i], n, C1, strlen(C1),
                                                  (int64_t)time(NULL), &la);
        char *db = hu_daemon_prospective_reactive(&alloc, &agent, b, C1, strlen(C1), msgs[i], n,
                                                  NULL, 0, false, &lb);
        HU_ASSERT_EQ(la, lb);
        HU_ASSERT_TRUE((da == NULL) == (db == NULL));
        if (da && db)
            HU_ASSERT_TRUE(memcmp(da, db, la) == 0);
        if (da)
            alloc.free(alloc.ctx, da, la + 1);
        if (db)
            alloc.free(alloc.ctx, db, lb + 1);
        hu_daemon_prospective_delivered(&agent, C1, strlen(C1), "taco place lasagna pizza", 24);
    }
    char sa[1024];
    char sb[1024];
    dump(a, sa, sizeof(sa));
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb);
    HU_ASSERT_EQ(mp.calls, 0);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
    env_leave(old_home_env ? old_home : NULL);
}

/* Self-chat (a persona contact with relationship "test") is never judged. */
static void reactive_live_self_chat_is_never_judged(void) {
    const char *old_home_env = getenv("HOME");
    char old_home[512];
    snprintf(old_home, sizeof(old_home), "%s", old_home_env ? old_home_env : "");
    env_enter();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_kw(db, "alpha", "ask about alpha");
    static hu_contact_profile_t contacts[1];
    static hu_persona_t persona;
    memset(contacts, 0, sizeof(contacts));
    contacts[0].contact_id = C1;
    contacts[0].name = "Seth Ford";
    contacts[0].relationship = "test";
    memset(&persona, 0, sizeof(persona));
    persona.contacts = contacts;
    persona.contacts_count = 1;
    mock_prov_t mp;
    memset(&mp, 0, sizeof(mp));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.memory = &m;
    agent.persona = &persona;
    agent.provider.ctx = &mp;
    agent.provider.vtable = &vt;
    setenv("HU_PROSPECTIVE", "live", 1);
    size_t len = 0;
    HU_ASSERT_NULL(hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "alpha!", 6,
                                                  NULL, 0, false, &len));
    HU_ASSERT_EQ(mp.calls, 0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0"),
                 (int64_t)1);
    m.vtable->deinit(m.ctx);
    env_leave(old_home_env ? old_home : NULL);
}

/* Ruling 1: write_err is the LAST field of the daemon counts line. */
static void log_counts_line_ends_with_write_err(void) {
    hu_prospective_counts_t c;
    memset(&c, 0, sizeof(c));
    c.candidates = 1;
    c.fire = 1;
    c.write_err = 1;
    c.item_count = 1;
    c.items[0].id = 7;
    c.items[0].verdict = HU_PM_VERDICT_FIRE;
    c.items[0].judge_ok = true;
    FILE *tmp = tmpfile();
    HU_ASSERT_NOT_NULL(tmp);
    fflush(stderr);
    int saved = dup(fileno(stderr));
    HU_ASSERT_TRUE(saved >= 0);
    HU_ASSERT_TRUE(dup2(fileno(tmp), fileno(stderr)) >= 0);
    hu_log_level_set_for_test((int)HU_LOG_LEVEL_INFO);
    hu_daemon_prospective_log_counts("shadow", &c);
    hu_prospective_counts_t none;
    memset(&none, 0, sizeof(none));
    hu_daemon_prospective_log_counts("shadow", &none); /* nothing to say: silent */
    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    hu_log_level_set_for_test(-1);
    char buf[1024];
    rewind(tmp);
    size_t n = fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);
    HU_ASSERT_STR_CONTAINS(buf, "prospective shadow: candidates=1 fire=1 resolved=0 cancel=0 "
                                "not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 "
                                "write_err=1\n");
    HU_ASSERT_STR_CONTAINS(buf, "prospective shadow item: id=7 verdict=fire\n");
    const char *first = strstr(buf, "prospective shadow:");
    HU_ASSERT_NULL(strstr(first + 1, "prospective shadow:"));
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_prospective_tests(void) {
    HU_TEST_SUITE("daemon prospective");
    HU_RUN_TEST(history_render_keeps_last_twenty_lines_oldest_first);
    HU_RUN_TEST(history_render_keeps_the_newest_lines_that_fit);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(directive_off_is_byte_identical_to_legacy_and_never_judges);
    HU_RUN_TEST(directive_shadow_returns_legacy_and_writes_nothing_extra);
    HU_RUN_TEST(directive_shadow_judges_only_cued_turns_up_to_the_cap);
    HU_RUN_TEST(directive_live_returns_soft_directive_and_settles_on_delivery);
    HU_RUN_TEST(directive_live_judge_failure_is_silent);
    HU_RUN_TEST(live_ownerless_row_is_settled_only_by_the_pass_that_surfaced_it);
    HU_RUN_TEST(provider_judge_sends_a_short_thinking_off_request);
    HU_RUN_TEST(reactive_and_delivered_entries_follow_the_env_gate);
    HU_RUN_TEST(reactive_off_twin_db_matches_the_pre_change_call);
    HU_RUN_TEST(reactive_live_self_chat_is_never_judged);
    HU_RUN_TEST(log_counts_line_ends_with_write_err);
#endif
}
