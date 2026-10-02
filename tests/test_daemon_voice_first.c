/* Voice-first memos (spec 2026-09-28): the daemon decides voice before the
 * turn and, LIVE and on the family list, has the turn write a memo. */
#include "human/agent.h"
#include "human/context/voice_triggers.h"
#include "human/core/allocator.h"
#include "human/daemon/reactive_turn.h"
#include "human/daemon/voice_first.h"
#include "human/persona.h"
#include "test_framework.h"
#if defined(HU_ENABLE_SQLITE)
#include "human/memory.h"
#include "human/memory/proactive_decisions_repo.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *ctx;
    size_t ctx_len;
    uint32_t max_chars;
    hu_daemon_voice_first_t vf;
} vf_run_t;

static bool g_group;
static bool g_force;
static hu_memory_t *g_mem;                 /* NULL: no decision log */
static hu_contact_profile_t *g_contact;    /* the sender's persona profile, or NULL */
static const hu_reactive_turn_ctx_t *g_rt; /* channel history, or NULL */
static uint32_t g_max_chars = 200;

#if defined(HU_ENABLE_SQLITE)
/* A failed assert returns early and can leave these pointing at a dead frame;
 * every test that sets them starts from a clean slate. */
static void vf_reset(void) {
    g_mem = NULL;
    g_contact = NULL;
    g_rt = NULL;
    g_max_chars = 200;
}
#endif

static void run(const char *mode, const char *allow, const char *inbound, vf_run_t *r) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    snprintf(persona.voice.voice_id, sizeof(persona.voice.voice_id), "test-voice");
    persona.voice_messages.enabled = true;
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.persona = &persona;
    agent.memory = g_mem;
    if (g_contact) {
        persona.contacts = g_contact;
        persona.contacts_count = 1;
    }
    if (mode)
        setenv("HU_VOICE_FIRST", mode, 1);
    else
        unsetenv("HU_VOICE_FIRST");
    if (allow)
        setenv("HU_VOICE_DELIVERY_ONLY", allow, 1);
    else
        unsetenv("HU_VOICE_DELIVERY_ONLY");
    const char *base = "[earlier] they asked about the lake";
    r->ctx_len = strlen(base);
    r->ctx = alloc.alloc(alloc.ctx, r->ctx_len + 1);
    memcpy(r->ctx, base, r->ctx_len + 1);
    r->max_chars = g_max_chars;
    hu_daemon_voice_first_prepare(&alloc, &agent, "+15550000001", 12, g_group, g_force, inbound,
                                  strlen(inbound), &r->ctx, &r->ctx_len, &r->max_chars, g_rt,
                                  &r->vf);
    unsetenv("HU_VOICE_FIRST");
    unsetenv("HU_VOICE_DELIVERY_ONLY");
}

static void done(vf_run_t *r) {
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, r->ctx, r->ctx_len + 1);
}

static void test_voice_first_off_changes_nothing(void) {
    vf_run_t r;
    run(NULL, "+15550000001", "I'm so proud of you", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ(r.max_chars, 200);
    HU_ASSERT_STR_EQ(r.ctx, "[earlier] they asked about the lake");
    done(&r);
}

static void test_voice_first_live_writes_a_memo_for_family(void) {
    vf_run_t r;
    run("live", "+15550000009,+15550000001", "I'm so proud of you", &r);
    HU_ASSERT_TRUE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "heartfelt");
    HU_ASSERT_EQ(r.max_chars, HU_VOICE_FIRST_MEMO_MAX_CHARS);
    HU_ASSERT_TRUE(strncmp(r.ctx, "VOICE MEMO:", 11) == 0);
    HU_ASSERT_STR_CONTAINS(r.ctx, "[earlier] they asked about the lake");
    HU_ASSERT_STR_CONTAINS(r.ctx, "wonderful day"); /* named as what NOT to say */
    done(&r);
}

static void test_voice_first_live_leaves_others_as_text(void) {
    vf_run_t r;
    run("live", "+15550000009", "I'm so proud of you", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ((int)r.vf.decision, (int)HU_VOICE_SEND_VOICE); /* decided, not eligible */
    HU_ASSERT_EQ(r.max_chars, 200);
    done(&r);
    run("live", NULL, "I'm so proud of you", &r); /* no list: nobody */
    HU_ASSERT_FALSE(r.vf.memo);
    done(&r);
}

static void test_voice_first_shadow_decides_but_writes_text(void) {
    vf_run_t r;
    run("shadow", "+15550000001", "[Audio transcription: miss you guys]", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ((int)r.vf.decision, (int)HU_VOICE_SEND_VOICE);
    HU_ASSERT_STR_EQ(r.vf.reason, "they_sent_audio");
    HU_ASSERT_STR_EQ(r.ctx, "[earlier] they asked about the lake");
    done(&r);
}

static void test_voice_first_live_without_a_trigger_stays_text(void) {
    vf_run_t r;
    run("live", "+15550000001", "ok sounds good", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_EQ(r.max_chars, 200);
    done(&r);
}

static void test_voice_first_stands_down_in_group_chats(void) {
    vf_run_t r;
    g_group = true;
    run("live", "+15550000001", "I'm so proud of you", &r);
    g_group = false;
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "group");
    HU_ASSERT_EQ(r.max_chars, 200);
    done(&r);
}

/* What the director is told: voice is on the table only where voice-first
 * LIVE would actually send one (family list, not a group, a voice set up). */
static void test_voice_first_available_for_the_director(void) {
    static hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    snprintf(persona.voice.voice_id, sizeof(persona.voice.voice_id), "test-voice");
    persona.voice_messages.enabled = true;
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.persona = &persona;
    setenv("HU_VOICE_DELIVERY_ONLY", "+15550000001", 1);
    setenv("HU_VOICE_FIRST", "live", 1);
    HU_ASSERT_TRUE(hu_daemon_voice_first_available(&agent, "+15550000001", 12, false));
    HU_ASSERT_FALSE(hu_daemon_voice_first_available(&agent, "+15550000001", 12, true));
    HU_ASSERT_FALSE(hu_daemon_voice_first_available(&agent, "+15550000002", 12, false));
    setenv("HU_VOICE_FIRST", "shadow", 1);
    HU_ASSERT_FALSE(hu_daemon_voice_first_available(&agent, "+15550000001", 12, false));
    unsetenv("HU_VOICE_FIRST");
    unsetenv("HU_VOICE_DELIVERY_ONLY");
}

/* #voice from Seth's own number: a memo even without a trigger or inside the gap. */

static void test_voice_first_forced_for_a_self_test(void) {
    vf_run_t r;
    g_force = true;
    run("live", "+15550000001", "ok sounds good", &r); /* no trigger */
    g_force = false;
    HU_ASSERT_TRUE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "self_test");
    done(&r);
}

/* The director's cue ("keep it light and brief") beat the memo directive in
 * the live test (2026-09-29 06:09). On a memo turn the cue keeps its objective
 * and gains an explicit memo length that comes first. */
static void test_voice_first_rewrites_the_directors_cue(void) {
    char d[512] = "Laugh it off, acknowledge the loop, keep it light and brief";
    hu_daemon_voice_first_direction(d, sizeof(d));
    HU_ASSERT_TRUE(strncmp(d, "Voice memo", 10) == 0);
    HU_ASSERT_STR_CONTAINS(d, "a few connected thoughts");
    HU_ASSERT_STR_CONTAINS(d, "Laugh it off");
    char empty[512] = "";
    hu_daemon_voice_first_direction(empty, sizeof(empty));
    HU_ASSERT_STR_CONTAINS(empty, "a few connected thoughts");
}

#if defined(HU_ENABLE_SQLITE)
/* A decision-log row as the daemon writes it: trigger, reason, sent, `ago` s back. */
static void log_row(hu_memory_t *mem, const char *trigger, const char *decision, const char *reason,
                    int sent, int64_t ago) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, (int64_t)time(NULL) - ago, "+15550000001",
                                                    trigger, decision, reason, sent, NULL),
                 HU_OK);
}

static int64_t count_rows(hu_memory_t *mem, const char *where) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM proactive_decisions WHERE %s", where);
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Bug 2026-10-01: all 18 "spacing" decisions in 14 days of production were on
 * the owner's own thread, each inside 3 h of a #voice self-test memo. A
 * self-test is not a memo the contact heard; it must not start the gap. A real
 * memo still must. */
static void test_voice_first_self_test_does_not_start_the_spacing_gap(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    g_mem = &mem;
    log_row(&mem, "voice_reply", "send", hu_daemon_voice_first_reply_reason(HU_VOICE_FIRST_FORCED),
            1, 600);
    vf_run_t r;
    run("shadow", "+15550000001", "I'm so proud of you", &r);
    HU_ASSERT_STR_EQ(r.vf.reason, "heartfelt");
    done(&r);
    /* A memo the contact actually got (voice-first or the post-hoc classifier). */
    log_row(&mem, "voice_reply", "send", hu_daemon_voice_first_reply_reason(HU_VOICE_FIRST_MEMO), 1,
            300);
    run("shadow", "+15550000001", "I'm so proud of you", &r);
    HU_ASSERT_STR_EQ(r.vf.reason, "spacing");
    done(&r);
    g_mem = NULL;
    mem.vtable->deinit(mem.ctx);
}

static const char k_story[] = "We went to the lake this morning with the kids. Ella caught her "
                              "first fish and screamed.";

/* HU_VOICE_TRIGGERS_V2 unset or off: the decision, context and length budget
 * are exactly what voice-first decides today, and nothing new is logged. */
static void test_voice_triggers_v2_off_is_byte_identical(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    g_max_chars = 200;
    vf_run_t unset, off;
    unsetenv("HU_VOICE_TRIGGERS_V2");
    run("live", "+15550000001", k_story, &unset);
    setenv("HU_VOICE_TRIGGERS_V2", "off", 1);
    run("live", "+15550000001", k_story, &off);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    HU_ASSERT_STR_EQ(unset.vf.reason, "no_trigger");
    HU_ASSERT_FALSE(unset.vf.memo);
    HU_ASSERT_EQ(unset.max_chars, 200);
    HU_ASSERT_STR_EQ(unset.ctx, "[earlier] they asked about the lake");
    HU_ASSERT_STR_EQ(off.vf.reason, unset.vf.reason);
    HU_ASSERT_EQ((int)off.vf.decision, (int)unset.vf.decision);
    HU_ASSERT_EQ((int)off.vf.memo, (int)unset.vf.memo);
    HU_ASSERT_EQ(off.max_chars, unset.max_chars);
    HU_ASSERT_EQ(off.ctx_len, unset.ctx_len);
    HU_ASSERT_TRUE(memcmp(off.ctx, unset.ctx, unset.ctx_len) == 0);
    HU_ASSERT_EQ(count_rows(&mem, "trigger LIKE 'voice_v2%'"), 0);
    done(&unset);
    done(&off);
    g_mem = NULL;
    mem.vtable->deinit(mem.ctx);
}

/* SHADOW evaluates v2 and records what it would do, but the turn is unchanged. */
static void test_voice_triggers_v2_shadow_does_not_change_the_decision(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    g_max_chars = 120;
    setenv("HU_VOICE_TRIGGERS_V2", "shadow", 1);
    vf_run_t r;
    run("live", "+15550000001", k_story, &r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_EQ((int)r.vf.decision, (int)HU_VOICE_SEND_TEXT);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ(r.max_chars, 120);
    HU_ASSERT_STR_EQ(r.ctx, "[earlier] they asked about the lake");
    /* ...and it did evaluate: the would-voice row is there. */
    HU_ASSERT_EQ(count_rows(&mem, "trigger = 'voice_v2_shadow' AND decision = 'send' AND "
                                  "reason = 'story_inbound' AND sent = 0"),
                 1);
    HU_ASSERT_EQ(count_rows(&mem, "trigger = 'voice_v2'"), 0); /* the LIVE ledger is untouched */
    done(&r);
    /* A turn the base rules already decided is left to them: no v2 row. */
    setenv("HU_VOICE_TRIGGERS_V2", "shadow", 1);
    run("live", "+15550000001", "I'm so proud of you", &r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    HU_ASSERT_STR_EQ(r.vf.reason, "heartfelt");
    HU_ASSERT_EQ(count_rows(&mem, "trigger = 'voice_v2_shadow'"), 1);
    done(&r);
    g_max_chars = 200;
    g_mem = NULL;
    mem.vtable->deinit(mem.ctx);
}

/* LIVE lets a v2 reason choose voice: on the family list the turn writes a memo. */
static void test_voice_triggers_v2_live_writes_a_memo_for_a_story(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    g_max_chars = 120;
    setenv("HU_VOICE_TRIGGERS_V2", "live", 1);
    vf_run_t r;
    run("live", "+15550000001", k_story, &r);
    HU_ASSERT_TRUE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "story_inbound");
    HU_ASSERT_EQ(r.max_chars, HU_VOICE_FIRST_MEMO_MAX_CHARS);
    done(&r);
    HU_ASSERT_EQ(count_rows(&mem, "trigger = 'voice_v2' AND decision = 'send'"), 1);
    /* Off the family list no memo is written, so nothing is booked to the cap. */
    run("live", "+15550000009", k_story, &r);
    HU_ASSERT_STR_EQ(r.vf.reason, "story_inbound");
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ(count_rows(&mem, "trigger = 'voice_v2' AND decision = 'send'"), 1);
    done(&r);
    /* The existing spacing rule still sits on top. */
    log_row(&mem, "voice_reply", "send", "voice_first", 1, 60);
    run("live", "+15550000001", k_story, &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "spacing");
    done(&r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    g_max_chars = 200;
    g_mem = NULL;
    mem.vtable->deinit(mem.ctx);
}

/* Two v2 memos to one contact this week: the third story stays a text. */
static void test_voice_triggers_v2_live_weekly_cap_per_contact(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    g_max_chars = 120;
    log_row(&mem, "voice_v2", "send", "story_inbound", 0, 2 * 86400);
    log_row(&mem, "voice_v2", "send", "late_evening_warmth", 0, 5 * 86400);
    log_row(&mem, "voice_v2", "send", "story_inbound", 0, 9 * 86400); /* last week */
    setenv("HU_VOICE_TRIGGERS_V2", "live", 1);
    vf_run_t r;
    run("live", "+15550000001", k_story, &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_EQ(count_rows(&mem, "trigger = 'voice_v2' AND reason = 'weekly_cap'"), 1);
    done(&r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    g_max_chars = 200;
    g_mem = NULL;
    mem.vtable->deinit(mem.ctx);
}

/* Review of #576: for a family contact whose measured reply p90 is 240, the
 * planned budget sits at the channel ceiling on every turn, so a budget-based
 * memo_length_reply fired on "ok". A one-word reply is never a memo moment. */
static void test_voice_triggers_v2_does_not_saturate_on_a_short_message(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.contact_id = "+15550000001";
    cp.relationship_type = "family";
    cp.reply_chars_p90 = 240;
    g_contact = &cp;
    g_max_chars = 240;
    setenv("HU_VOICE_TRIGGERS_V2", "live", 1);
    vf_run_t r;
    run("live", "+15550000001", "ok", &r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ(count_rows(&mem, "trigger LIKE 'voice_v2%' AND decision = 'send'"), 0);
    done(&r);
    vf_reset();
    mem.vtable->deinit(mem.ctx);
}

/* Review of #576: a week of SHADOW would-voices must not use up the LIVE cap,
 * or the first LIVE week starts at "weekly_cap" for everyone. */
static void test_voice_triggers_v2_shadow_does_not_consume_the_live_cap(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    g_max_chars = 120;
    vf_run_t r;
    setenv("HU_VOICE_TRIGGERS_V2", "shadow", 1);
    for (int i = 0; i < 3; i++) {
        run("live", "+15550000001", k_story, &r);
        done(&r);
    }
    setenv("HU_VOICE_TRIGGERS_V2", "live", 1);
    run("live", "+15550000001", k_story, &r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    HU_ASSERT_STR_EQ(r.vf.reason, "story_inbound");
    HU_ASSERT_TRUE(r.vf.memo);
    done(&r);
    vf_reset();
    mem.vtable->deinit(mem.ctx);
}

/* The weekly cap cannot be read (no decision log): treat it as reached. */
static void test_voice_triggers_v2_cap_fails_closed_without_a_log(void) {
    vf_reset();
    g_max_chars = 120;
    setenv("HU_VOICE_TRIGGERS_V2", "live", 1);
    vf_run_t r;
    run("live", "+15550000001", k_story, &r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_FALSE(r.vf.memo);
    done(&r);
}
#endif

/* "YYYY-MM-DD HH:MM:SS" local, `ago` seconds before now — chat.db's history format. */
static void stamp(char *out, size_t cap, int64_t ago) {
    time_t t = time(NULL) - (time_t)ago;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void test_voice_first_secs_since_owner_reply(void) {
    hu_channel_history_entry_t h[3];
    memset(h, 0, sizeof(h));
    h[0].from_me = true;
    stamp(h[0].timestamp, sizeof(h[0].timestamp), 5 * 86400);
    h[1].from_me = true;
    stamp(h[1].timestamp, sizeof(h[1].timestamp), 4 * 86400);
    h[2].from_me = false; /* their message now */
    stamp(h[2].timestamp, sizeof(h[2].timestamp), 10);
    int64_t now = (int64_t)time(NULL);
    int64_t s = hu_daemon_voice_first_secs_since_owner_reply(h, 3, now);
    HU_ASSERT_TRUE(s >= 4 * 86400 - 5 && s <= 4 * 86400 + 5); /* the NEWEST reply */
    HU_ASSERT_EQ(hu_daemon_voice_first_secs_since_owner_reply(h + 2, 1, now), -1); /* none */
    HU_ASSERT_EQ(hu_daemon_voice_first_secs_since_owner_reply(NULL, 0, now), -1);
    snprintf(h[1].timestamp, sizeof(h[1].timestamp), "yesterday-ish");
    s = hu_daemon_voice_first_secs_since_owner_reply(h, 2, now); /* unparseable: skip it */
    HU_ASSERT_TRUE(s >= 5 * 86400 - 5 && s <= 5 * 86400 + 5);
}

#if defined(HU_ENABLE_SQLITE)
/* A close contact, first reply in 4 days: v2 LIVE picks long_gap_reconnect
 * from the turn's own channel history. A contact who is not close does not. */
static void test_voice_triggers_v2_live_reconnects_with_a_close_contact(void) {
    vf_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    g_mem = &mem;
    hu_channel_history_entry_t h[2];
    memset(h, 0, sizeof(h));
    h[0].from_me = true;
    stamp(h[0].timestamp, sizeof(h[0].timestamp), 4 * 86400);
    stamp(h[1].timestamp, sizeof(h[1].timestamp), 30);
    hu_reactive_turn_ctx_t rt;
    memset(&rt, 0, sizeof(rt));
    rt.history_entries = h;
    rt.history_count = 2;
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.contact_id = "+15550000001";
    cp.dunbar_layer = "intimate";
    g_rt = &rt;
    g_contact = &cp;
    g_max_chars = 120;
    setenv("HU_VOICE_TRIGGERS_V2", "live", 1);
    vf_run_t r;
    run("live", "+15550000001", "hey", &r);
    HU_ASSERT_STR_EQ(r.vf.reason, "long_gap_reconnect");
    HU_ASSERT_TRUE(r.vf.memo);
    done(&r);
    cp.dunbar_layer = "active";
    run("live", "+15550000001", "hey", &r);
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_FALSE(r.vf.memo);
    done(&r);
    unsetenv("HU_VOICE_TRIGGERS_V2");
    vf_reset();
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_daemon_voice_first_tests(void) {
    HU_TEST_SUITE("daemon voice-first memos");
    HU_RUN_TEST(test_voice_first_off_changes_nothing);
    HU_RUN_TEST(test_voice_first_rewrites_the_directors_cue);
    HU_RUN_TEST(test_voice_first_forced_for_a_self_test);
    HU_RUN_TEST(test_voice_first_available_for_the_director);
    HU_RUN_TEST(test_voice_first_stands_down_in_group_chats);
    HU_RUN_TEST(test_voice_first_live_writes_a_memo_for_family);
    HU_RUN_TEST(test_voice_first_live_leaves_others_as_text);
    HU_RUN_TEST(test_voice_first_shadow_decides_but_writes_text);
    HU_RUN_TEST(test_voice_first_live_without_a_trigger_stays_text);
    HU_RUN_TEST(test_voice_first_secs_since_owner_reply);
#if defined(HU_ENABLE_SQLITE)
    HU_RUN_TEST(test_voice_triggers_v2_live_reconnects_with_a_close_contact);
    HU_RUN_TEST(test_voice_first_self_test_does_not_start_the_spacing_gap);
    HU_RUN_TEST(test_voice_triggers_v2_off_is_byte_identical);
    HU_RUN_TEST(test_voice_triggers_v2_shadow_does_not_change_the_decision);
    HU_RUN_TEST(test_voice_triggers_v2_live_writes_a_memo_for_a_story);
    HU_RUN_TEST(test_voice_triggers_v2_live_weekly_cap_per_contact);
    HU_RUN_TEST(test_voice_triggers_v2_does_not_saturate_on_a_short_message);
    HU_RUN_TEST(test_voice_triggers_v2_shadow_does_not_consume_the_live_cap);
    HU_RUN_TEST(test_voice_triggers_v2_cap_fails_closed_without_a_log);
#endif
}
