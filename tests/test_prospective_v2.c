/* tests/test_prospective_v2.c
 *
 * hu_prospective_v2_run / _after_delivery (src/memory/prospective_v2.c): the
 * PIS-style pass over the typed store with a SCRIPTED judge (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.3,
 * §4.5). Pins: a cued intention is decided once and surfaced with the soft
 * directive; already_resolved / cancel settle it and it never refires;
 * not_now, parse failures and model errors stay silent and pending; SHADOW
 * writes and renders nothing; group and self-chat are never judged; at most
 * three Decide calls per turn; time cues fire within their grace window, one
 * per contact per day; done only after a delivered reply carries the action;
 * an undelivered surfacing counts as an attempt. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/prospective_v2.h"
#include "human/memory/superhuman.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C1  "+15550000001"
#define NOW ((int64_t)1790000000)

typedef struct script {
    const char *const *replies;
    size_t n;
    size_t calls;
    hu_error_t err;
    char last_user[4096];
} script_t;

static hu_error_t scripted_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                                 size_t system_len, const char *user, size_t user_len, char **out,
                                 size_t *out_len) {
    script_t *s = (script_t *)ctx;
    (void)system;
    (void)system_len;
    size_t cl = user_len < sizeof(s->last_user) - 1 ? user_len : sizeof(s->last_user) - 1;
    memcpy(s->last_user, user, cl);
    s->last_user[cl] = '\0';
    s->calls++;
    if (s->err != HU_OK)
        return s->err;
    const char *r = s->calls <= s->n ? s->replies[s->calls - 1] : "not_now";
    size_t rl = strlen(r);
    char *b = (char *)alloc->alloc(alloc->ctx, rl + 1);
    memcpy(b, r, rl + 1);
    *out = b;
    *out_len = rl;
    return HU_OK;
}

static hu_prospective_judge_t judge_of(script_t *s, const char *const *replies, size_t n) {
    memset(s, 0, sizeof(*s));
    s->replies = replies;
    s->n = n;
    hu_prospective_judge_t j = {.fn = scripted_judge, .ctx = s};
    return j;
}

static void seed_kw_exp(sqlite3 *db, const char *cue, const char *action, int64_t created,
                        int64_t expires) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','%s','%s','" C1 "',%lld,0,%lld)",
             cue, action, (long long)expires, (long long)created);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void seed_kw(sqlite3 *db, const char *cue, const char *action) {
    seed_kw_exp(db, cue, action, NOW - 86400, 0);
}

static void seed_time(sqlite3 *db, const char *action, int64_t due, const char *key) {
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(
                     db, C1, strlen(C1), action, strlen(action), due, HU_PROSPECTIVE_TIME_GRACE_S,
                     HU_PM_SOURCE_PROMISE_KEEPER, key, HU_PM_PENDING, NOW - 86400, NULL),
                 HU_OK);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static hu_prospective_turn_t turn_for(const char *inbound, int64_t now) {
    hu_prospective_turn_t t;
    memset(&t, 0, sizeof(t));
    t.contact = C1;
    t.contact_len = strlen(C1);
    t.inbound = inbound;
    t.inbound_len = inbound ? strlen(inbound) : 0;
    t.history = "them: going to that new taco place friday\nme: nice let me know\n";
    t.history_len = strlen(t.history);
    t.now = now;
    t.day_start = now - 3600;
    return t;
}

static void v2_clean_positive_surfaces_a_soft_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw(db, "tacos", "ask how the new taco place was");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("ok the taco place was packed", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1); /* one intention, one Decide call */
    HU_ASSERT_EQ(c.candidates, (size_t)1);
    HU_ASSERT_EQ(c.fire, (size_t)1);
    HU_ASSERT_STR_EQ(d, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ask how "
                        "the new taco place was]");
    HU_ASSERT_EQ(dl, strlen(d));
    alloc.free(alloc.ctx, d, dl + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced' "
                           "AND fired=0 AND surfaced_at=1790000000"),
                 (int64_t)2); /* both keyword rows of the intention */
    HU_ASSERT_EQ(c.item_count, (size_t)1);
    HU_ASSERT_EQ(c.items[0].verdict, HU_PM_VERDICT_FIRE);
    HU_ASSERT_TRUE(c.items[0].judge_ok);
    HU_ASSERT_EQ(c.fire_action_count, (size_t)1);
    HU_ASSERT_STR_EQ(c.fire_actions[0], "ask how the new taco place was");
    mem.vtable->deinit(mem.ctx);
}

static void v2_already_resolved_is_done_and_never_refires(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"already_resolved"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("taco place was great btw", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.resolved, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' AND "
                           "fired=1 AND outcome='suppressed'"),
                 (int64_t)1);
    /* the same cue later: nothing is judged again (TriggerBench "always remind") */
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_EQ(c.candidates, (size_t)0);
    HU_ASSERT_NULL(d);
    mem.vtable->deinit(mem.ctx);
}

static void v2_cancel_retires_the_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"cancel"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("the taco place closed down, forget it", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.cancel, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='canceled' "
                           "AND fired=2"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_not_now_parse_fail_and_judge_error_stay_pending_and_silent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw_exp(db, "alpha", "ask about alpha", NOW - 20, 0);
    seed_kw_exp(db, "beta", "ask about beta", NOW - 30, 0);
    static const char *const r[] = {"not_now", "lol yeah def"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 2);
    hu_prospective_turn_t t = turn_for("alpha and beta", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.not_now, (size_t)1);
    HU_ASSERT_EQ(c.parse_fail, (size_t)1);
    s.err = HU_ERR_PROVIDER_RESPONSE; /* the model is down */
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.judge_err, (size_t)2);
    HU_ASSERT_EQ(c.items[0].judge_ok, false);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0 AND surfaced_at IS NULL"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_shadow_judges_but_writes_and_renders_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw_exp(db, "stale", "an expired one", NOW - 90000, NOW - 5);
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("taco place tonight?", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, false, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_EQ(c.fire, (size_t)1);
    HU_ASSERT_EQ(c.expired, (size_t)1); /* counted, not written */
    HU_ASSERT_STR_EQ(c.fire_actions[0], "ask how the new taco place was");
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(dl, (size_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0 AND surfaced_at IS NULL"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_group_and_self_chat_are_never_judged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw_exp(db, "stale", "an expired one", NOW - 90000, NOW - 5);
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for("taco place and stale", NOW);
    t.is_group = true;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)0);
    HU_ASSERT_EQ(c.candidates + c.expired, (size_t)0);
    t.is_group = false;
    t.is_self = true;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)0);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending'"),
                 (int64_t)2); /* nothing written, not even the expiry */
    mem.vtable->deinit(mem.ctx);
}

static void v2_judges_at_most_three_intentions_per_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw_exp(db, "alpha", "ask about alpha", NOW - 10, 0);
    seed_kw_exp(db, "beta", "ask about beta", NOW - 20, 0);
    seed_kw_exp(db, "gamma", "ask about gamma", NOW - 30, 0);
    seed_kw_exp(db, "delta", "ask about delta", NOW - 40, 0);
    seed_kw_exp(db, "epsilon", "ask about epsilon", NOW - 50, 0);
    static const char *const r[] = {"fire", "fire", "fire", "fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 5);
    hu_prospective_turn_t t = turn_for("alpha beta gamma delta epsilon", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)3);
    HU_ASSERT_EQ(c.candidates, (size_t)3);
    HU_ASSERT_EQ(c.fire, (size_t)3);
    HU_ASSERT_STR_CONTAINS(d, "ask about alpha | ask about beta | ask about gamma]");
    alloc.free(alloc.ctx, d, dl + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending'"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_time_due_item_renders_due_list_and_caps_one_per_day(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_time(db, "call about the lease", NOW - 3600, "commitment:1");
    seed_time(db, "return the drill", NOW - 1800, "followup:2");
    seed_time(db, "not due yet", NOW + 3600, "followup:3");
    static const char *const r[] = {"fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 2);
    hu_prospective_turn_t t = turn_for(NULL, NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_STR_EQ(d, "- call about the lease\n"); /* oldest due first */
    HU_ASSERT_EQ(c.capped, (size_t)1);
    alloc.free(alloc.ctx, d, dl + 1);
    /* later the same day, nothing delivered: still capped — one per contact per day */
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.capped, (size_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_time_past_grace_expires_without_judging(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                              "created_at) VALUES('" C1 "','call about the lease','me',1,"
                              "'pending',1)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    seed_time(db, "call about the lease", NOW - 4 * 86400, "commitment:1");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for(NULL, NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)0);
    HU_ASSERT_EQ(c.expired, (size_t)1);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='expired'"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_after_delivery_used_is_done_ignored_retries_then_expires(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw(db, "lasagna", "send the lasagna recipe");
    static const char *const r[] = {"fire", "fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 3);
    hu_prospective_counts_t c;
    hu_prospective_delivery_counts_t dc;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for("taco place, then lasagna night", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    alloc.free(alloc.ctx, d, dl + 1);
    static const char reply[] = "wait how was the taco place??";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, C1, strlen(C1),
                                                  reply, sizeof(reply) - 1, NOW + 60, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.surfaced, (size_t)2);
    HU_ASSERT_EQ(dc.used, (size_t)1);
    HU_ASSERT_EQ(dc.ignored, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE action LIKE 'ask how%' "
                           "AND status='done' AND fired=1 AND outcome='used'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE action LIKE 'send%' "
                           "AND status='pending' AND outcome='ignored'"),
                 (int64_t)1);
    /* surfaced a second time and still not carried: expired, never a third time */
    hu_prospective_turn_t t2 = turn_for("lasagna tonight?", NOW + 3600);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t2, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(c.fire, (size_t)1);
    alloc.free(alloc.ctx, d, dl + 1);
    static const char reply2[] = "sounds good";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, C1, strlen(C1),
                                                  reply2, sizeof(reply2) - 1, NOW + 3660, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.expired, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE action LIKE 'send%'"),
                 (int64_t)3);
    mem.vtable->deinit(mem.ctx);
}

static void v2_undelivered_surfacing_is_reclaimed_as_an_attempt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for("taco place?", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    alloc.free(alloc.ctx, d, dl + 1);
    /* the reply went out as a voice memo: no delivered-text hook ran */
    hu_prospective_turn_t t2 = turn_for("how are you", NOW + 600);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t2, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE status='pending' AND "
                           "outcome='ignored'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_time_done_retires_ledger_twins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                              "created_at) VALUES('" C1 "','call about the lease','me',1,"
                              "'pending',1);INSERT INTO delayed_followups(contact_id,topic,"
                              "scheduled_at,sent) VALUES('" C1 "','call about the lease',1,0)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    seed_time(db, "call about the lease", NOW - 3600, "commitment:1");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    hu_prospective_delivery_counts_t dc;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for(NULL, NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    alloc.free(alloc.ctx, d, dl + 1);
    static const char sent[] = "hey did you ever call about the lease?";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_TIME, C1, strlen(C1), sent,
                                                  sizeof(sent) - 1, NOW + 60, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.used, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='followed_up'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_judge_sees_history_intention_and_cue(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"not_now"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    hu_prospective_turn_t t = turn_for("the taco place!!", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, NULL, NULL),
                 HU_OK);
    HU_ASSERT_STR_CONTAINS(s.last_user, "them: going to that new taco place friday");
    HU_ASSERT_STR_CONTAINS(s.last_user, "intention: ask how the new taco place was");
    HU_ASSERT_STR_CONTAINS(s.last_user, "cue: they just mentioned \"taco place\"");
    mem.vtable->deinit(mem.ctx);
}

static void v2_rejects_invalid_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_prospective_counts_t c;
    hu_prospective_turn_t t = turn_for("x", NOW);
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, NULL, HU_PM_CUE_KEYWORD, &t, NULL, true, &c, NULL, NULL),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_AFTER_EVENT, &t, NULL, true, &c, NULL, NULL),
        HU_ERR_INVALID_ARGUMENT);
    t.contact_len = 0;
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, NULL, true, &c, NULL, NULL),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, NULL, 0, "x", 1, NOW, NULL),
        HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

/* --- fix round 1 (review of task-6): I1 --- */

/* I1: the only pre-existing apply=false test used fire + expire, so removing
 * the apply guard on the resolved write, the canceled write, or the reclaim
 * step would go unnoticed for the 7 days SHADOW runs against the real DB.
 * This test seeds an already_resolved verdict, a cancel verdict, AND a
 * pre-existing surfaced row (attempts=0) in one SHADOW pass and asserts none
 * of the three writes happened. */
static void v2_shadow_never_writes_resolved_cancel_or_reclaim(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was"); /* inserted first: lower id */
    seed_kw(db, "lasagna", "send the lasagna recipe");           /* inserted second: higher id */
    seed_kw(db, "sushi", "ask about the sushi place");
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "UPDATE prospective_memories SET status='surfaced', fired=0, "
                              "surfaced_at=1789999000 WHERE trigger_value='sushi'",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    /* hu_prospective_repo_list orders keyword rows newest-first (created_at
     * DESC, id DESC): with equal created_at, lasagna (inserted second, higher
     * id) is judged before taco. */
    static const char *const r[] = {"cancel", "already_resolved"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 2);
    hu_prospective_turn_t t = turn_for("taco place and lasagna talk, plus sushi later", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, false, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(c.resolved, (size_t)1);
    HU_ASSERT_EQ(c.cancel, (size_t)1);
    HU_ASSERT_NULL(d);
    /* the resolved item (taco): still pending, fired=0, outcome untouched */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE trigger_value='taco "
                           "place' AND status='pending' AND fired=0 AND outcome IS NULL"),
                 (int64_t)1);
    /* the canceled item (lasagna): still pending too */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE trigger_value="
                           "'lasagna' AND status='pending' AND fired=0 AND outcome IS NULL"),
                 (int64_t)1);
    /* the earlier-surfaced item (sushi): SHADOW's reclaim step must not touch it */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE trigger_value="
                           "'sushi' AND status='surfaced' AND attempts=0 AND outcome IS NULL"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* --- fix round 1: I2 --- */

/* I2a: pm_surface must write the surfaced transition FIRST and only render
 * items whose write actually changed a row. A trigger that RAISEs on the
 * UPDATE simulates a real backend failure: the item must not be rendered
 * and must stay pending (untouched) for a later retry. */
static void v2_surface_write_failure_is_not_rendered_and_stays_pending(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "widget", "FAILWRITE ask about the widget");
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TRIGGER pm_fail_write BEFORE UPDATE ON prospective_memories "
                              "WHEN OLD.action LIKE 'FAILWRITE%' BEGIN SELECT RAISE(ABORT, "
                              "'simulated write failure'); END",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("the widget again", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(c.fire, (size_t)1);      /* the judge did say fire */
    HU_ASSERT_EQ(c.write_err, (size_t)1); /* but the surfaced write failed: counted, not silent */
    HU_ASSERT_EQ(c.judge_err, (size_t)0); /* the judge itself answered fine (task 7 ruling 1) */
    HU_ASSERT_NULL(d);                    /* not rendered */
    HU_ASSERT_EQ(dl, (size_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE action LIKE "
                           "'FAILWRITE%' AND status='pending' AND fired=0 AND attempts=0"),
                 (int64_t)1); /* untouched: the aborted UPDATE changed nothing */
    mem.vtable->deinit(mem.ctx);
}

/* I2b: after_delivery must count an outcome only when its transition
 * actually changed a row, and must propagate a real backend error instead
 * of returning HU_OK with fabricated counts. */
static void v2_after_delivery_write_failure_is_not_counted_and_propagates_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "gadget", "FAILWRITE ask about the gadget");
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "UPDATE prospective_memories SET status='surfaced', fired=0, "
                              "surfaced_at=1789999000 WHERE action LIKE 'FAILWRITE%'",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TRIGGER pm_fail_write BEFORE UPDATE ON prospective_memories "
                              "WHEN OLD.action LIKE 'FAILWRITE%' BEGIN SELECT RAISE(ABORT, "
                              "'simulated write failure'); END",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    hu_prospective_delivery_counts_t dc;
    static const char reply[] = "how's the gadget going";
    hu_error_t err = hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, C1, strlen(C1),
                                                      reply, sizeof(reply) - 1, NOW, &dc);
    HU_ASSERT_TRUE(err != HU_OK); /* a real SQLite error, not a swallowed HU_OK */
    HU_ASSERT_EQ(dc.surfaced, (size_t)0);
    HU_ASSERT_EQ(dc.used, (size_t)0);
    HU_ASSERT_EQ(dc.ignored, (size_t)0);
    HU_ASSERT_EQ(dc.expired, (size_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE action LIKE "
                           "'FAILWRITE%' AND status='surfaced' AND attempts=0"),
                 (int64_t)1); /* untouched */
    mem.vtable->deinit(mem.ctx);
}

/* --- fix round 1: minor --- a judge that returns HU_OK with a NULL or
 * empty answer must fail toward silence (parse_fail), never fire. */
static hu_error_t null_output_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                                    size_t system_len, const char *user, size_t user_len,
                                    char **out, size_t *out_len) {
    (void)ctx;
    (void)alloc;
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    *out = NULL;
    *out_len = 0;
    return HU_OK;
}

static void v2_judge_ok_with_null_or_empty_output_is_parse_fail(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "kite", "ask about the kite");
    hu_prospective_judge_t jn = {.fn = null_output_judge, .ctx = NULL};
    hu_prospective_turn_t t = turn_for("flying the kite", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &jn, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.parse_fail, (size_t)1);
    HU_ASSERT_EQ(c.fire, (size_t)0);
    HU_ASSERT_TRUE(c.items[0].judge_ok); /* the judge call itself succeeded */
    HU_ASSERT_EQ(c.items[0].verdict, HU_PM_VERDICT_PARSE_FAIL);

    seed_kw(db, "balloon", "ask about the balloon");
    static const char *const empty_r[] = {""};
    script_t s;
    hu_prospective_judge_t je = judge_of(&s, empty_r, 1);
    hu_prospective_turn_t t2 = turn_for("flying the balloon", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t2, &je, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.parse_fail, (size_t)1);
    HU_ASSERT_EQ(c.fire, (size_t)0);
    mem.vtable->deinit(mem.ctx);
}

/* --- task 7 review, fix round 1 --- */

/* A keyword row whose action is `len` bytes: "<head>" followed by 'y'. */
static void seed_kw_long(sqlite3 *db, const char *cue, const char *head, size_t len) {
    char action[600];
    size_t hl = strlen(head);
    HU_ASSERT_TRUE(len < sizeof(action) && hl < len);
    memcpy(action, head, hl);
    memset(action + hl, 'y', len - hl);
    action[len] = '\0';
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "INSERT INTO prospective_memories(trigger_type,trigger_value,"
                                    "action,contact_id,expires_at,fired,created_at) "
                                    "VALUES('keyword',?1,?2,'" C1 "',0,0,?3)",
                                    -1, &st, NULL),
                 SQLITE_OK);
    sqlite3_bind_text(st, 1, cue, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, NOW - 86400);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

static void q_text(sqlite3 *db, const char *sql, char *buf, size_t cap) {
    sqlite3_stmt *st = NULL;
    buf[0] = '\0';
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        snprintf(buf, cap, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

/* Two fired ~500-byte actions do not both fit the 1024-byte directive. Only
 * the one actually rendered is marked surfaced; the other stays pending with
 * its attempts untouched, so the undelivered-attempt settle cannot charge it
 * an attempt for a reminder that was never shown. */
static void v2_surfaces_only_the_items_the_directive_rendered(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw_long(db, "alpha", "ask about alpha ", 500);
    seed_kw_long(db, "beta", "ask about beta ", 500);
    static const char *const r[] = {"fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 2);
    hu_prospective_turn_t t = turn_for("alpha and beta", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(c.fire, (size_t)2);
    HU_ASSERT_EQ(c.write_err, (size_t)0);
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_EQ(dl, strlen(d));
    HU_ASSERT_NULL(strstr(d, " | ")); /* one item, no dangling separator */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "attempts=0 AND surfaced_at IS NULL"),
                 (int64_t)1);
    char shown[600];
    char hidden[600];
    q_text(db, "SELECT action FROM prospective_memories WHERE status='surfaced'", shown,
           sizeof(shown));
    q_text(db, "SELECT action FROM prospective_memories WHERE status='pending'", hidden,
           sizeof(hidden));
    HU_ASSERT_NOT_NULL(strstr(d, shown));
    HU_ASSERT_NULL(strstr(d, hidden));
    alloc.free(alloc.ctx, d, dl + 1);
    /* the reply never came: only the shown one is charged an attempt */
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, C1, strlen(C1),
                                                  NULL, 0, NOW + 60, NULL),
                 HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE attempts=1"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE attempts=0 AND "
                           "surfaced_at IS NULL"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* An allocator that fails its Nth allocation (0 = never) and counts calls. */
typedef struct failing_alloc {
    size_t calls;
    size_t fail_at;
} failing_alloc_t;

static void *fa_alloc(void *ctx, size_t size) {
    failing_alloc_t *f = (failing_alloc_t *)ctx;
    f->calls++;
    if (f->fail_at && f->calls == f->fail_at)
        return NULL;
    return malloc(size);
}

static void *fa_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void fa_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

/* The directive allocation is the pass's last allocation. When it fails,
 * nothing may be left surfaced: every row stays pending, never shown. */
static void v2_directive_alloc_failure_leaves_every_row_pending(void) {
    hu_allocator_t sys = hu_system_allocator();
    static const char *const r[] = {"fire"};
    /* 1. count the allocations of a successful pass */
    failing_alloc_t f = {0, 0};
    hu_allocator_t fa = {.ctx = &f, .alloc = fa_alloc, .realloc = fa_realloc, .free = fa_free};
    hu_memory_t m1 = hu_sqlite_memory_create(&sys, ":memory:");
    sqlite3 *db1 = hu_sqlite_memory_get_db(&m1);
    seed_kw(db1, "taco place", "ask how the new taco place was");
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("the taco place!!", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&fa, db1, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NOT_NULL(d);
    fa.free(fa.ctx, d, dl + 1);
    size_t total = f.calls;
    HU_ASSERT_TRUE(total > 0);
    m1.vtable->deinit(m1.ctx);
    /* 2. the same pass with its last allocation failing */
    f.calls = 0;
    f.fail_at = total;
    hu_memory_t m2 = hu_sqlite_memory_create(&sys, ":memory:");
    sqlite3 *db2 = hu_sqlite_memory_get_db(&m2);
    seed_kw(db2, "taco place", "ask how the new taco place was");
    j = judge_of(&s, r, 1);
    d = NULL;
    dl = 7;
    HU_ASSERT_EQ(hu_prospective_v2_run(&fa, db2, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(dl, (size_t)0);
    HU_ASSERT_EQ(q_int(db2, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                            "fired=0 AND attempts=0 AND surfaced_at IS NULL"),
                 (int64_t)1);
    m2.vtable->deinit(m2.ctx);
}

/* ── Task 10: the one-time backfill ──────────────────────────────────── */

#define C2 "+15550000002"

static void v2_backfill_imports_expires_reanchors_and_dedupes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    /* Raw ledger rows (the Task 8 writers would already mirror them). */
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('" C1 "','call about the lease','me',%lld,'pending',1),"
             "('" C1 "','old promise','me',%lld,'pending',1),"
             "('" C1 "','future thing','me',%lld,'pending',1),"
             "('" C1 "','undated','me',0,'pending',1),"
             "('" C1 "','already done','me',%lld,'followed_up',1);"
             "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
             "('" C1 "','call about the lease',%lld,0),('" C1 "','sent one',%lld,1)",
             (long long)(NOW - 2 * 86400), (long long)(NOW - 20 * 86400), (long long)(NOW + 86400),
             (long long)(NOW - 86400), (long long)(NOW - 2 * 86400), (long long)(NOW - 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, false, &b), HU_OK);
    HU_ASSERT_EQ(b.commitments_seen, (size_t)3); /* dated + pending only */
    HU_ASSERT_EQ(b.followups_seen, (size_t)1);   /* unsent only */
    HU_ASSERT_EQ(b.imported_pending, (size_t)2);
    HU_ASSERT_EQ(b.imported_expired, (size_t)1);
    HU_ASSERT_EQ(b.reanchored, (size_t)1);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)1); /* the lease follow-up is the same intention */
    HU_ASSERT_EQ(b.skipped_unsafe, (size_t)0);
    HU_ASSERT_EQ(b.ledger_retired, (size_t)1); /* 'old promise', the expired import */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)0); /* dry run rolled back */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='pending'"),
                 (int64_t)4); /* ... the ledger retire too */

    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired, (size_t)3);
    HU_ASSERT_EQ(b.ledger_retired, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT status='expired' AND followed_up_at=1790000000 FROM "
                           "commitments WHERE description='old promise'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT due_at FROM prospective_memories WHERE "
                           "action='call about the lease'"),
                 NOW); /* re-anchored: one grace window from the backfill */
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE action='old promise'"),
                 (int64_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT due_at FROM prospective_memories WHERE action='future thing'"),
                 NOW + 86400);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "trigger_value='commitment:1' AND source='promise_keeper'"),
                 (int64_t)1); /* keyed by the ledger row's own id */

    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired, (size_t)0); /* idempotent */
    /* Known gap 5: the retired 'old promise' is no longer a pending ledger
     * row, so the re-run does not see it at all (was 3 seen / 4 skipped). */
    HU_ASSERT_EQ(b.commitments_seen, (size_t)2);
    HU_ASSERT_EQ(b.followups_seen, (size_t)1);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)3);
    HU_ASSERT_EQ(b.ledger_retired, (size_t)0);
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, NULL, NOW, true, &b), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, 0, true, &b), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

/* Every time row, one line each, in key order: what "identical" compares. */
static void time_rows_snapshot(sqlite3 *db, char *out, size_t cap) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(
                     db,
                     "SELECT trigger_type||'|'||trigger_value||'|'||action||'|'||contact_id||'|'||"
                     "source||'|'||status||'|'||fired||'|'||due_at||'|'||expires_at "
                     "FROM prospective_memories WHERE cue_kind='time' ORDER BY trigger_value",
                     -1, &st, NULL),
                 SQLITE_OK);
    size_t n = 0;
    out[0] = '\0';
    while (sqlite3_step(st) == SQLITE_ROW)
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s\n",
                              (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    HU_ASSERT_TRUE(n < cap);
}

/* Ruling 1: the backfill writes the SAME rows the live writers write for the
 * same ledger items -- owner verbatim, a contact's promise rephrased (and its
 * paired follow-up collapsed into it), an unsafe contact promise skipped, a
 * dated-moment frame as its topic. Mirror live, snapshot, drop the mirrors,
 * backfill at the same clock, snapshot again: byte-equal. */
static void v2_backfill_rows_are_identical_to_the_live_mirror(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t now = (int64_t)time(NULL);
    static const char lease[] = "call about the lease";
    static const char land[] = "text you when I land";
    static const char frame[] =
        "they mentioned the dentist appointment (tomorrow); confidence 0.80";
    static const char kids[] = "pick up the kids";
    size_t c1 = strlen(C1);
    size_t c2 = strlen(C2);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, C1, c1, lease, sizeof(lease) - 1,
                                                "me", 2, now + 2 * 86400),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(
                     &mem, &alloc, C1, c1, lease, sizeof(lease) - 1, now + 2 * 86400, "me", 2),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, C2, c2, land, sizeof(land) - 1,
                                                "them", 4, now + 3 * 86400),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(
                     &mem, &alloc, C2, c2, land, sizeof(land) - 1, now + 3 * 86400, "them", 4),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, C1, c1, frame,
                                                         sizeof(frame) - 1, now + 86400, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_superhuman_commitment_store(&mem, &alloc, C2, c2, "to ", 3, "them", 4, now + 4 * 86400),
        HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(
                     &mem, &alloc, C1, c1, kids, sizeof(kids) - 1, now + 5 * 86400, NULL, 0),
                 HU_OK);
    char live[4096];
    time_rows_snapshot(db, live, sizeof(live));
    HU_ASSERT_STR_CONTAINS(live, "|ask if they still need to text you when they land|");
    HU_ASSERT_STR_CONTAINS(live, "|the dentist appointment|");
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)4);

    HU_ASSERT_EQ(sqlite3_exec(db, "DELETE FROM prospective_memories WHERE cue_kind='time'", NULL,
                              NULL, NULL),
                 SQLITE_OK);
    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, now, true, &b), HU_OK);
    HU_ASSERT_EQ(b.commitments_seen, (size_t)3);
    HU_ASSERT_EQ(b.followups_seen, (size_t)4);
    HU_ASSERT_EQ(b.imported_pending, (size_t)4);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)2); /* both paired follow-ups */
    HU_ASSERT_EQ(b.skipped_unsafe, (size_t)1);   /* "to " */
    HU_ASSERT_EQ(b.commitments_seen + b.followups_seen,
                 b.imported_pending + b.imported_expired + b.skipped_existing + b.skipped_unsafe);
    char back[4096];
    time_rows_snapshot(db, back, sizeof(back));
    HU_ASSERT_STR_EQ(back, live);
    mem.vtable->deinit(mem.ctx);
}

/* F4 + F1 + the rowid-keyed retire, on rows only the backfill wrote: a
 * contact's overdue promise is re-anchored in third person (never quoted
 * first person), its paired follow-up collapses into it, a re-run at a LATER
 * clock adds nothing, and settling the backfilled row retires its own ledger
 * row and twin by id. */
static void v2_backfill_contact_promises_rerun_later_and_retire_by_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('" C2 "','text you when I land','them',%lld,'pending',1),"
             "('" C2 "','to ','them',%lld,'pending',1);"
             "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
             "('" C2 "','text you when I land',%lld,0),"
             "('" C1 "','they mentioned the dentist appointment (tomorrow); confidence 0.80',"
             "%lld,0)",
             (long long)(NOW - 2 * 86400), (long long)(NOW + 86400), (long long)(NOW - 2 * 86400),
             (long long)(NOW + 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.commitments_seen, (size_t)2);
    HU_ASSERT_EQ(b.followups_seen, (size_t)2);
    HU_ASSERT_EQ(b.imported_pending, (size_t)2);
    HU_ASSERT_EQ(b.reanchored, (size_t)1);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)1);
    HU_ASSERT_EQ(b.skipped_unsafe, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "action LIKE '%when I land%' OR action='to '"),
                 (int64_t)0); /* never the contact's words as the owner's */
    HU_ASSERT_EQ(q_int(db, "SELECT due_at FROM prospective_memories WHERE "
                           "trigger_value='commitment:1' AND "
                           "action='ask if they still need to text you when they land'"),
                 NOW);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "trigger_value='followup:2' AND action='the dentist appointment' AND "
                           "source='followup'"),
                 (int64_t)1);

    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW + 5 * 86400, false, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired, (size_t)0);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)3);
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW + 5 * 86400, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired, (size_t)0); /* F1: later clock */
    HU_ASSERT_EQ(b.skipped_existing, (size_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)2);

    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING, C2, strlen(C2),
                                          &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &items[0], HU_PM_DONE, NOW), HU_OK);
    hu_prospective_repo_free(&alloc, items, n);
    HU_ASSERT_EQ(q_int(db, "SELECT status='followed_up' FROM commitments WHERE id=1"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups WHERE id=1"), (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups WHERE id=2"), (int64_t)0);
    mem.vtable->deinit(mem.ctx);
}

/* Known gap 5: an item imported `expired` also retires its ledger rows in
 * the same transaction -- agent_turn.c and proactive.c read the ledger
 * whatever the gates say. A 20-day-overdue commitment and its F20 follow-up
 * both end retired; a 5-day-overdue pair is re-anchored pending and its
 * ledger stays pending; another contact's expired item retires only its
 * own row. A dry run changes nothing. A database a pre-fix backfill already
 * wrote (expired time row, ledger still pending) is repaired on re-run via
 * skipped_existing. */
static void v2_backfill_expired_import_retires_its_ledger_twins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('" C1 "','renew the passport','me',%lld,'pending',1),"
             "('" C1 "','book the vet','me',%lld,'pending',1),"
             "('" C2 "','fix the fence','me',%lld,'pending',1);"
             "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
             "('" C1 "','renew the passport',%lld,0),('" C1 "','book the vet',%lld,0)",
             (long long)(NOW - 20 * 86400), (long long)(NOW - 5 * 86400),
             (long long)(NOW - 30 * 86400), (long long)(NOW - 20 * 86400),
             (long long)(NOW - 5 * 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    static const char k_ledger[] =
        "SELECT (SELECT group_concat(status) FROM (SELECT status FROM commitments ORDER BY id)) "
        "|| '/' || (SELECT group_concat(sent) FROM (SELECT sent FROM delayed_followups ORDER BY "
        "id))";
    char led[128];
    q_text(db, k_ledger, led, sizeof(led));
    HU_ASSERT_STR_EQ(led, "pending,pending,pending/0,0");

    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, false, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_expired,
                 (size_t)3); /* passport x2 (an expired row blocks none), fence */
    HU_ASSERT_EQ(b.ledger_retired, (size_t)3);
    q_text(db, k_ledger, led, sizeof(led));
    HU_ASSERT_STR_EQ(led, "pending,pending,pending/0,0"); /* dry run: rolled back */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories"), (int64_t)0);

    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.ledger_retired, (size_t)3);
    q_text(db, k_ledger, led, sizeof(led));
    HU_ASSERT_STR_EQ(led, "expired,pending,expired/1,0");
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "action='renew the passport' AND status<>'expired'"),
                 (int64_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT status='pending' AND due_at=1790000000 FROM "
                           "prospective_memories WHERE action='book the vet'"),
                 (int64_t)1); /* 5 days overdue: re-anchored, its ledger untouched */

    /* re-run: the retired rows are no longer seen; nothing else changes */
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.commitments_seen, (size_t)1);
    HU_ASSERT_EQ(b.followups_seen, (size_t)1);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)2);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired + b.ledger_retired, (size_t)0);

    /* the pre-fix state: expired time row written, ledger left pending */
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "UPDATE commitments SET status='pending', followed_up_at=NULL "
                              "WHERE description='renew the passport'",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)3);
    HU_ASSERT_EQ(b.imported_expired, (size_t)0);
    HU_ASSERT_EQ(b.ledger_retired, (size_t)1);
    q_text(db, k_ledger, led, sizeof(led));
    HU_ASSERT_STR_EQ(led, "expired,pending,expired/1,0");
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 1 minor: an expired import whose contact is too long for the
 * deferred-retire slot is not retired -- and is counted, never silent. */
static void v2_backfill_counts_an_unretirable_long_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    char contact[301];
    memset(contact, '7', 300);
    contact[0] = '+';
    contact[300] = '\0';
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('%s','renew the passport','me',%lld,'pending',1)",
             contact, (long long)(NOW - 20 * 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_expired, (size_t)1);
    HU_ASSERT_EQ(b.ledger_retired, (size_t)0);
    HU_ASSERT_EQ(b.ledger_unretired, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='pending'"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* The boundary is "more than 14 days": exactly 14 days overdue re-anchors. */
static void v2_backfill_fourteen_day_boundary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('" C1 "','at the edge','me',%lld,'pending',1),"
             "('" C1 "','just past','me',%lld,'pending',1)",
             (long long)(NOW - HU_PROSPECTIVE_BACKFILL_EXPIRE_S),
             (long long)(NOW - HU_PROSPECTIVE_BACKFILL_EXPIRE_S - 1));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending, (size_t)1);
    HU_ASSERT_EQ(b.imported_expired, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT status='pending' AND due_at="
                           "1790000000"
                           " FROM prospective_memories WHERE action='at the edge'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT status='expired' FROM prospective_memories WHERE "
                           "action='just past'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* Fix round 1, M3: a failure partway through the walk -- the second of three
 * upserts aborts (a test-only trigger; no production seam) after the first
 * already inserted -- rolls the WHOLE run back: zero time rows, zeroed
 * counts, the error surfaced, and no transaction left open (a clean re-run
 * after the fault is gone imports all three). */
static void v2_backfill_failure_midway_writes_nothing_and_zeroes_counts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('" C1 "','first thing','me',%lld,'pending',1),"
             "('" C1 "','boom','me',%lld,'pending',1),"
             "('" C1 "','third thing','me',%lld,'pending',1);"
             "CREATE TRIGGER t_boom BEFORE INSERT ON prospective_memories "
             "WHEN NEW.action = 'boom' BEGIN SELECT RAISE(ABORT, 'injected'); END;",
             (long long)(NOW + 86400), (long long)(NOW + 2 * 86400), (long long)(NOW + 3 * 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    hu_prospective_backfill_counts_t zero;
    memset(&zero, 0, sizeof(zero));
    for (int write = 1; write >= 0; write--) {
        hu_prospective_backfill_counts_t b;
        memset(&b, 0xAB, sizeof(b));
        HU_ASSERT_NEQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, write != 0, &b), HU_OK);
        HU_ASSERT_EQ(memcmp(&b, &zero, sizeof(b)), 0);
        HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                     (int64_t)0); /* 'first thing' was inserted, then rolled back */
        HU_ASSERT_TRUE(sqlite3_get_autocommit(db) != 0); /* no transaction left open */
    }

    HU_ASSERT_EQ(sqlite3_exec(db, "DROP TRIGGER t_boom", NULL, NULL, NULL), SQLITE_OK);
    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending, (size_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)3);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_v2_tests(void) {
    HU_TEST_SUITE("prospective v2");
    HU_RUN_TEST(v2_clean_positive_surfaces_a_soft_directive);
    HU_RUN_TEST(v2_already_resolved_is_done_and_never_refires);
    HU_RUN_TEST(v2_cancel_retires_the_intention);
    HU_RUN_TEST(v2_not_now_parse_fail_and_judge_error_stay_pending_and_silent);
    HU_RUN_TEST(v2_shadow_judges_but_writes_and_renders_nothing);
    HU_RUN_TEST(v2_group_and_self_chat_are_never_judged);
    HU_RUN_TEST(v2_judges_at_most_three_intentions_per_turn);
    HU_RUN_TEST(v2_time_due_item_renders_due_list_and_caps_one_per_day);
    HU_RUN_TEST(v2_time_past_grace_expires_without_judging);
    HU_RUN_TEST(v2_after_delivery_used_is_done_ignored_retries_then_expires);
    HU_RUN_TEST(v2_undelivered_surfacing_is_reclaimed_as_an_attempt);
    HU_RUN_TEST(v2_time_done_retires_ledger_twins);
    HU_RUN_TEST(v2_judge_sees_history_intention_and_cue);
    HU_RUN_TEST(v2_rejects_invalid_arguments);
    HU_RUN_TEST(v2_shadow_never_writes_resolved_cancel_or_reclaim);
    HU_RUN_TEST(v2_surface_write_failure_is_not_rendered_and_stays_pending);
    HU_RUN_TEST(v2_after_delivery_write_failure_is_not_counted_and_propagates_error);
    HU_RUN_TEST(v2_judge_ok_with_null_or_empty_output_is_parse_fail);
    HU_RUN_TEST(v2_surfaces_only_the_items_the_directive_rendered);
    HU_RUN_TEST(v2_directive_alloc_failure_leaves_every_row_pending);
    HU_RUN_TEST(v2_backfill_imports_expires_reanchors_and_dedupes);
    HU_RUN_TEST(v2_backfill_rows_are_identical_to_the_live_mirror);
    HU_RUN_TEST(v2_backfill_contact_promises_rerun_later_and_retire_by_id);
    HU_RUN_TEST(v2_backfill_expired_import_retires_its_ledger_twins);
    HU_RUN_TEST(v2_backfill_counts_an_unretirable_long_contact);
    HU_RUN_TEST(v2_backfill_fourteen_day_boundary);
    HU_RUN_TEST(v2_backfill_failure_midway_writes_nothing_and_zeroes_counts);
}

#else

void run_prospective_v2_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
