/* HU_PROPOSER_CONTEXT — per-contact proposer context (src/daemon/daemon_proposer_context.c).
 *
 * The per-contact proactive proposer never saw the thread with the person it
 * was deciding to text; 2026-10-01 prod: 111 of 200 per-contact decisions
 * declined, most for "no trigger / no context". These tests pin:
 *   - the thread renderer (speakers, relative times, days since contact, budget);
 *   - the builder fills contact / conversation / memory within 3 KB;
 *   - OFF is byte-identical (renderer golden + the decide() call shape);
 *   - SHADOW never changes the production decision input, runs the enriched
 *     prompt at most once per contact per cycle, and only on a local provider;
 *   - LIVE feeds the enriched prompt to the local provider, and falls back to
 *     today's prompt — never the enriched one — when the local call fails;
 *   - the local-provider resolver refuses cloud providers and an open circuit. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/log_redact.h"
#include "human/daemon/proposer_context.h"
#include "human/persona.h"
#include "human/providers/compatible.h"
#include "human/providers/reliable.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_insights_repo.h"
#include "human/memory/engines.h"
#endif

#define T_NOW ((int64_t)1790000000) /* 2026-09-21, fixed so tests are deterministic */

static const char k_contact[] = "+15550001111";

/* ── fixtures ─────────────────────────────────────────────────────────── */

static void stamp(hu_channel_history_entry_t *e, int64_t when, bool from_me, const char *text) {
    memset(e, 0, sizeof(*e));
    e->from_me = from_me;
    snprintf(e->text, sizeof(e->text), "%s", text);
    time_t t = (time_t)when;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M", &tmv);
}

/* Oldest first, as load_conversation_history returns them. */
static size_t make_thread(hu_channel_history_entry_t *e, int64_t now) {
    stamp(&e[0], now - 3 * 86400 - 600, false, "did the roof guy ever show up");
    stamp(&e[1], now - 3 * 86400 - 300, true, "not yet, thursday supposedly");
    stamp(&e[2], now - 2 * 3600 - 30, false, "ok lmk\nhow it goes");
    return 3;
}

static void fake_contact(hu_contact_profile_t *cp) {
    memset(cp, 0, sizeof(*cp));
    cp->contact_id = (char *)k_contact;
    cp->name = "Dana";
    cp->relationship = "sister";
    cp->relationship_type = "family";
    cp->dunbar_layer = "1";
    cp->identity = "younger sister, nurse in tampa";
    cp->context = "renovating her house this fall";
}

/* Scripted fake provider: answers `reply` and keeps the last user message. */
typedef struct fake_llm {
    int calls;
    const char *reply;  /* NULL = empty answer */
    const char *reply2; /* if set, the answer from the 2nd call on */
    char last_msg[16384];
    char first_msg[16384];
} fake_llm_t;

static hu_error_t fake_llm_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                                const char *msg, size_t msg_len, const char *model,
                                size_t model_len, double temperature, char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    fake_llm_t *f = (fake_llm_t *)ctx;
    f->calls++;
    size_t n = msg_len < sizeof(f->last_msg) - 1 ? msg_len : sizeof(f->last_msg) - 1;
    memcpy(f->last_msg, msg, n);
    f->last_msg[n] = '\0';
    if (f->calls == 1)
        memcpy(f->first_msg, f->last_msg, n + 1);
    *out = NULL;
    *out_len = 0;
    const char *reply = (f->calls > 1 && f->reply2) ? f->reply2 : f->reply;
    if (!reply)
        return HU_OK;
    size_t rl = strlen(reply);
    char *b = (char *)alloc->alloc(alloc->ctx, rl + 1);
    if (!b)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(b, reply, rl + 1);
    *out = b;
    *out_len = rl;
    return HU_OK;
}

static const hu_provider_vtable_t fake_llm_vtable = {.chat_with_system = fake_llm_chat};

static hu_error_t always_fail(void *ctx, hu_allocator_t *alloc, const char *s, size_t sl,
                              const char *m, size_t ml, const char *mo, size_t mol, double t,
                              char **out, size_t *out_len) {
    (void)ctx, (void)alloc, (void)s, (void)sl, (void)m, (void)ml, (void)mo, (void)mol, (void)t;
    *out = NULL;
    *out_len = 0;
    return HU_ERR_IO;
}
static const hu_provider_vtable_t failing_vtable = {.chat_with_system = always_fail};

/* Fake production tick: records what the proposer was asked and returns a
 * scripted result per call. */
static struct {
    int calls;
    void *provider_ctx[4];
    char msg[4][16384];
    bool defer_row[4];
    hu_init_proposer_result_t script[4];
} g_tick;

static hu_error_t fake_tick(const struct hu_initiative_config *cfg,
                            const struct hu_autoresponder_config *ar_cfg, int32_t tz,
                            struct hu_proactive_budget *budget, const struct hu_agent *agent,
                            struct hu_provider *provider, hu_allocator_t *alloc,
                            const hu_proactive_compose_inputs_t *inputs, int64_t last_inbound,
                            int64_t now, int64_t *last_tick, uint64_t *tick_id,
                            hu_init_proposer_result_t *out_result,
                            hu_init_decision_t *out_decision) {
    (void)cfg;
    (void)ar_cfg;
    (void)tz;
    (void)budget;
    (void)agent;
    (void)alloc;
    (void)last_tick;
    (void)tick_id;
    int i = g_tick.calls < 4 ? g_tick.calls : 3;
    g_tick.provider_ctx[i] = provider ? provider->ctx : NULL;
    g_tick.defer_row[i] = inputs->defer_llm_failure_row;
    hu_init_proposer_build_propose_user_message_ex(inputs, now, last_inbound, g_tick.msg[i],
                                                   sizeof(g_tick.msg[i]));
    g_tick.calls++;
    *out_result = g_tick.script[i];
    if (out_decision)
        memset(out_decision, 0, sizeof(*out_decision));
    return HU_OK;
}

static void tick_reset(hu_init_proposer_result_t a, hu_init_proposer_result_t b) {
    memset(&g_tick, 0, sizeof(g_tick));
    g_tick.script[0] = a;
    g_tick.script[1] = b;
    hu_proposer_context_set_tick_fn_for_test(fake_tick);
    hu_proposer_context_reset_rate_limit_for_test();
}

static void plain_inputs(hu_proactive_compose_inputs_t *in) {
    memset(in, 0, sizeof(*in));
    in->contact_id = k_contact;
    in->contact_id_len = sizeof(k_contact) - 1;
    in->channel_name = "imessage";
    in->channel_name_len = 8;
    in->situation_context = "It has been 3 days since you talked.";
    in->situation_context_len = strlen(in->situation_context);
}

/* Today's rendering of plain_inputs — the bytes OFF must keep sending. */
static const char k_plain_golden[] = "Context as of unix=1790000000; last_inbound=0\n"
                                     "\n--- channel ---\nimessage"
                                     "\n--- contact ---\n+15550001111"
                                     "\n--- situation ---\nIt has been 3 days since you talked."
                                     "\n\nShould h-uman send Seth a message right now?";

/* Run one decide() for k_contact at `now`, with the thread captured. */
static void run_decide(hu_proposer_context_t *pc, hu_gate_mode_t mode, hu_provider_t *local,
                       hu_provider_t *reliable, int64_t now, hu_init_proposer_result_t *out) {
    hu_allocator_t a = hu_system_allocator();
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, now);
    hu_proposer_context_begin_with_local(pc, mode, local);
    hu_proposer_context_capture_thread(pc, e, n, now);
    hu_proactive_compose_inputs_t in;
    plain_inputs(&in);
    hu_initiative_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_init_decision_t d;
    hu_proposer_context_decide(pc, &cfg, NULL, 0, NULL, NULL, reliable, &a, &cp, &in, now, out, &d);
}

/* ── 1. the thread renderer ───────────────────────────────────────────── */

static void render_thread_labels_speakers_times_and_days_since(void) {
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, T_NOW);
    char buf[HU_PROPOSER_CTX_THREAD_MAX];
    int64_t days_last = 99, days_in = 99;
    size_t len =
        hu_proposer_context_render_thread(e, n, T_NOW, buf, sizeof(buf), &days_last, &days_in);
    HU_ASSERT_GT(len, (size_t)0);
    HU_ASSERT_EQ(strlen(buf), len);
    HU_ASSERT_STR_CONTAINS(buf, "[3d ago] them: did the roof guy ever show up");
    HU_ASSERT_STR_CONTAINS(buf, "[3d ago] Seth: not yet, thursday supposedly");
    /* the embedded newline is flattened so one message stays one line */
    HU_ASSERT_STR_CONTAINS(buf, "[2h ago] them: ok lmk how it goes");
    HU_ASSERT_EQ(days_last, (int64_t)0);
    HU_ASSERT_EQ(days_in, (int64_t)0);

    /* only Seth's message, 5 days old: days since inbound is unknown */
    size_t one = 1;
    stamp(&e[0], T_NOW - 5 * 86400 - 60, true, "happy birthday!!");
    len = hu_proposer_context_render_thread(e, one, T_NOW, buf, sizeof(buf), &days_last, &days_in);
    HU_ASSERT_STR_CONTAINS(buf, "[5d ago] Seth: happy birthday!!");
    HU_ASSERT_EQ(days_last, (int64_t)5);
    HU_ASSERT_EQ(days_in, (int64_t)-1);
}

static void render_thread_keeps_newest_lines_within_cap(void) {
    hu_channel_history_entry_t e[15];
    for (size_t i = 0; i < 15; i++) {
        char t[512];
        memset(t, 'a' + (int)(i % 26), sizeof(t) - 1);
        t[sizeof(t) - 1] = '\0';
        stamp(&e[i], T_NOW - (int64_t)(15 - i) * 3600, i % 2 == 0, t);
    }
    char buf[HU_PROPOSER_CTX_THREAD_MAX];
    int64_t dl, di;
    size_t len = hu_proposer_context_render_thread(e, 15, T_NOW, buf, sizeof(buf), &dl, &di);
    HU_ASSERT_LT(len, sizeof(buf));
    /* each message capped at HU_PROPOSER_CTX_LINE_MAX bytes of text */
    HU_ASSERT_STR_NOT_CONTAINS(buf,
                               "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
                               "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
                               "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
                               "oooooooooooooooo");
    /* the newest message (index 14, 'o') is present; the oldest ('a') is not */
    HU_ASSERT_STR_CONTAINS(buf, "[1h ago] Seth: oooo");
    HU_ASSERT_STR_NOT_CONTAINS(buf, ": aaaa");
}

static void render_thread_empty_writes_nothing_and_unknown_days(void) {
    char buf[64] = "x";
    int64_t dl = 7, di = 7;
    HU_ASSERT_EQ(hu_proposer_context_render_thread(NULL, 0, T_NOW, buf, sizeof(buf), &dl, &di),
                 (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
    HU_ASSERT_EQ(dl, (int64_t)-1);
    HU_ASSERT_EQ(di, (int64_t)-1);
}

/* ── 2. the builder ───────────────────────────────────────────────────── */

static fake_llm_t g_local_llm;
static hu_provider_t local_provider(const char *reply) {
    memset(&g_local_llm, 0, sizeof(g_local_llm));
    g_local_llm.reply = reply;
    hu_provider_t p = {.ctx = &g_local_llm, .vtable = &fake_llm_vtable};
    return p;
}

#ifdef HU_ENABLE_SQLITE
static void build_fills_contact_conversation_memory_within_budget(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "thread",
                                         "waiting on a roofer for the leak over the kitchen", 0.8,
                                         1767225600000LL, "test", NULL),
                 HU_OK);
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, T_NOW);
    hu_provider_t local = local_provider(NULL);

    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_SHADOW, &local);
    HU_ASSERT_EQ(pc.block_len, (size_t)0); /* pre: nothing yet */
    hu_proposer_context_capture_thread(&pc, e, n, T_NOW);
    hu_proposer_context_build(&pc, &a, &mem, &cp, NULL);

    HU_ASSERT_GT(pc.contact_len, (size_t)0);
    HU_ASSERT_GT(pc.thread_len, (size_t)0);
    HU_ASSERT_GT(pc.memory_len, (size_t)0);
    HU_ASSERT_GT(pc.block_len, (size_t)0);
    HU_ASSERT_LE(pc.block_len, (size_t)HU_PROPOSER_CTX_BUDGET);
    HU_ASSERT_EQ(strlen(pc.block), pc.block_len);
    HU_ASSERT_STR_CONTAINS(pc.block, "--- contact profile ---");
    HU_ASSERT_STR_CONTAINS(pc.block, "renovating her house");
    HU_ASSERT_STR_CONTAINS(pc.block, "--- recent thread (last contact 0 days ago) ---");
    HU_ASSERT_STR_CONTAINS(pc.block, "them: did the roof guy");
    HU_ASSERT_STR_CONTAINS(pc.block, "--- what you remember about them ---");
    HU_ASSERT_STR_CONTAINS(pc.block, "leak over the kitchen");
    mem.vtable->deinit(mem.ctx);
}

static void build_holds_the_budget_with_oversized_inputs(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    for (int i = 0; i < 20; i++) {
        char ins[200];
        snprintf(ins, sizeof(ins), "insight %d %0150d", i, 0);
        HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact", ins, 0.9,
                                             1767225600000LL + i, "test", NULL),
                     HU_OK);
    }
    hu_contact_profile_t cp;
    fake_contact(&cp);
    char big[4096];
    memset(big, 'z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    cp.dynamic = big;
    hu_channel_history_entry_t e[15];
    for (size_t i = 0; i < 15; i++) {
        char t[511];
        memset(t, 'q', sizeof(t) - 1);
        t[sizeof(t) - 1] = '\0';
        stamp(&e[i], T_NOW - (int64_t)(15 - i) * 600, i % 2 == 1, t);
    }
    hu_provider_t local = local_provider(NULL);
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_LIVE, &local);
    hu_proposer_context_capture_thread(&pc, e, 15, T_NOW);
    hu_proposer_context_build(&pc, &a, &mem, &cp, NULL);
    HU_ASSERT_GT(pc.contact_len, (size_t)0);
    HU_ASSERT_GT(pc.thread_len, (size_t)0);
    HU_ASSERT_GT(pc.memory_len, (size_t)0);
    HU_ASSERT_LE(pc.block_len, (size_t)HU_PROPOSER_CTX_BUDGET);
    HU_ASSERT_LT(pc.contact_len, (size_t)HU_PROPOSER_CTX_CONTACT_MAX);
    HU_ASSERT_LT(pc.memory_len, (size_t)HU_PROPOSER_CTX_MEMORY_MAX);
    mem.vtable->deinit(mem.ctx);
}

static bool reject_cross(const char *s, size_t n) {
    for (size_t i = 0; i + 5 <= n; i++)
        if (memcmp(s + i, "cross", 5) == 0)
            return false;
    return true;
}

static void build_drops_insights_the_safety_predicate_rejects(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact",
                                         "something cross-contact", 0.9, 1767225600000LL, "test",
                                         NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact",
                                         "starts night shifts in november", 0.9, 1767225500000LL,
                                         "test", NULL),
                 HU_OK);
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_provider_t local = local_provider(NULL);
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_SHADOW, &local);
    hu_proposer_context_build(&pc, &a, &mem, &cp, reject_cross);
    /* per insight: the unsafe line goes, the safe one stays */
    HU_ASSERT_STR_NOT_CONTAINS(pc.block, "cross-contact");
    HU_ASSERT_STR_CONTAINS(pc.memory, "night shifts");
    HU_ASSERT_GT(pc.contact_len, (size_t)0); /* the rest still builds */
    mem.vtable->deinit(mem.ctx);
}
static void insights_are_chosen_by_the_thread_like_the_reactive_path(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    /* oldest: the one the thread is about; four newer, unrelated */
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "thread",
                                         "the roof guy keeps rescheduling", 0.9, 1767225600000LL,
                                         "test", NULL),
                 HU_OK);
    const char *other[] = {"loves pickleball", "adopted a beagle", "learning italian",
                           "hates cilantro"};
    for (int i = 0; i < 4; i++)
        HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact", other[i],
                                             0.9, 1767225600000LL + 1000 * (i + 1), "test", NULL),
                     HU_OK);
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, T_NOW);
    hu_provider_t local = local_provider(NULL);
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_SHADOW, &local);
    hu_proposer_context_capture_thread(&pc, e, n, T_NOW);
    hu_proposer_context_build(&pc, &a, &mem, &cp, NULL);
    /* 3 items, and the thread-matching one is picked despite being oldest */
    HU_ASSERT_STR_CONTAINS(pc.memory, "roof guy keeps rescheduling");
    size_t lines = 0;
    for (size_t i = 0; i < pc.memory_len; i++)
        lines += pc.memory[i] == '\n';
    HU_ASSERT_EQ(lines, (size_t)3);
    mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

static void thread_is_never_captured_without_a_local_provider(void) {
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, T_NOW);
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_allocator_t a = hu_system_allocator();
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_LIVE, NULL);
    hu_proposer_context_capture_thread(&pc, e, n, T_NOW);
    hu_proposer_context_build(&pc, &a, NULL, &cp, NULL);
    HU_ASSERT_EQ(pc.thread_len, (size_t)0);
    HU_ASSERT_EQ(pc.block_len, (size_t)0);
    HU_ASSERT_STR_NOT_CONTAINS(pc.block, "roof guy");
}

static void nothing_is_captured_when_off(void) {
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, T_NOW);
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_allocator_t a = hu_system_allocator();
    hu_provider_t local = local_provider(NULL);
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_OFF, &local);
    hu_proposer_context_capture_thread(&pc, e, n, T_NOW);
    hu_proposer_context_build(&pc, &a, NULL, &cp, NULL);
    HU_ASSERT_EQ(pc.thread_len + pc.contact_len + pc.block_len, (size_t)0);
}

/* ── 3. OFF is byte-identical ─────────────────────────────────────────── */

static void user_message_without_block_matches_todays_bytes(void) {
    hu_proactive_compose_inputs_t in;
    plain_inputs(&in);
    char buf[2048];
    size_t n = hu_init_proposer_build_propose_user_message_ex(&in, T_NOW, 0, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, k_plain_golden);
    HU_ASSERT_EQ(n, sizeof(k_plain_golden) - 1);
    /* and the block, when set, lands before the situation */
    in.proposer_context = "\n--- recent thread ---\n[1h ago] them: hi\n";
    in.proposer_context_len = strlen(in.proposer_context);
    hu_init_proposer_build_propose_user_message_ex(&in, T_NOW, 0, buf, sizeof(buf));
    HU_ASSERT_STR_CONTAINS(buf, "+15550001111\n--- recent thread ---\n[1h ago] them: hi\n\n--- "
                                "situation ---");
}

static void decide_off_is_exactly_todays_call(void) {
    hu_provider_t local = local_provider("{\"should_propose\":true,\"confidence\":0.9}");
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_OFF, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_tick.calls, 1);
    HU_ASSERT_TRUE(g_tick.provider_ctx[0] == (void *)&rel_llm);
    HU_ASSERT_STR_EQ(g_tick.msg[0], k_plain_golden);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_NEGATIVE);
    HU_ASSERT_EQ(g_local_llm.calls, 0);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_NONE);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

/* ── 4. SHADOW ────────────────────────────────────────────────────────── */

static void shadow_keeps_the_decision_input_and_runs_local_once_per_cycle(void) {
    /* control (today's prompt) declines; the enriched prompt proposes */
    hu_provider_t local =
        local_provider("{\"should_propose\":false,\"confidence\":0.2,\"reason\":\"nothing new\"}");
    g_local_llm.reply2 = "{\"should_propose\":true,\"confidence\":0.91,\"draft\":\"did the "
                         "roofer ever make it out\"}";
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_NEGATIVE);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_SHADOW, &local, &reliable, T_NOW, &r);

    /* production: today's bytes, today's provider, today's verdict */
    HU_ASSERT_EQ(g_tick.calls, 1);
    HU_ASSERT_TRUE(g_tick.provider_ctx[0] == (void *)&rel_llm);
    HU_ASSERT_STR_EQ(g_tick.msg[0], k_plain_golden);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_NEGATIVE); /* shadow said propose; ignored */
    /* shadow: control + enriched, both on the local provider */
    HU_ASSERT_EQ(g_local_llm.calls, 2);
    HU_ASSERT_STR_EQ(g_local_llm.first_msg, k_plain_golden); /* control = today's prompt */
    HU_ASSERT_STR_CONTAINS(g_local_llm.last_msg, "them: did the roof guy");
    HU_ASSERT_EQ(rel_llm.calls, 0);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_OK);
    HU_ASSERT_EQ(pc.shadow_ctrl_result, (int)HU_INIT_RESULT_NEGATIVE);
    HU_ASSERT_EQ(pc.shadow_rich_result, (int)HU_INIT_RESULT_FIRED);

    /* same contact, same cycle: rate-limited, no further GPU calls */
    g_tick.calls = 0;
    run_decide(&pc, HU_GATE_SHADOW, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_local_llm.calls, 2);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_RATE_LIMITED);
    HU_ASSERT_EQ(g_tick.calls, 1); /* production still ran */
    /* next cycle: runs again */
    run_decide(&pc, HU_GATE_SHADOW, &local, &reliable, T_NOW + 1800, &r);
    HU_ASSERT_EQ(g_local_llm.calls, 4);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

static void shadow_skips_when_production_never_reached_the_model(void) {
    hu_provider_t local = local_provider("{\"should_propose\":true,\"confidence\":0.9}");
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_GATED_QUIET, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_SHADOW, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_GATED_QUIET);
    HU_ASSERT_EQ(g_local_llm.calls, 0);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_NOT_REACHED);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

/* Review fix: a raw should_propose under the threshold is not a would-propose. */
static void shadow_scores_both_prompts_through_the_production_threshold(void) {
    hu_provider_t local = local_provider(
        "{\"should_propose\":true,\"confidence\":0.95,\"draft\":\"how did the roofer go\"}");
    g_local_llm.reply2 =
        "{\"should_propose\":true,\"confidence\":0.5,\"draft\":\"did the roofer show up\"}";
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_SHADOW, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_OK);
    HU_ASSERT_EQ(pc.shadow_ctrl_result, (int)HU_INIT_RESULT_FIRED);
    HU_ASSERT_EQ(pc.shadow_rich_result, (int)HU_INIT_RESULT_LOW_CONFIDENCE); /* 0.5 < 0.85 */
    HU_ASSERT_FLOAT_EQ(pc.shadow_rich_confidence, 0.5, 1e-9);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

/* Review fix: the circuit is re-checked just before the local call. */
static void shadow_rechecks_the_circuit_before_calling_local(void) {
    hu_allocator_t a = hu_system_allocator();
    int dummy_p = 0, dummy_f = 0;
    hu_provider_t prim = {.ctx = &dummy_p, .vtable = &failing_vtable};
    hu_provider_t fb = {.ctx = &dummy_f, .vtable = &failing_vtable};
    hu_reliable_provider_entry_t extras[1] = {{.name = "fb", .name_len = 2, .provider = fb}};
    hu_provider_t rel = {0};
    HU_ASSERT_EQ(hu_reliable_create_ex(&a, prim, 0, 50, extras, 1, NULL, 0, &rel), HU_OK);
    hu_reliable_set_circuit(&rel, 1, 300);
    char *o = NULL;
    size_t ol = 0;
    (void)rel.vtable->chat_with_system(rel.ctx, &a, "s", 1, "m", 1, "x", 1, 0.1, &o, &ol);

    hu_provider_t local = local_provider("{\"should_propose\":true,\"confidence\":0.9}");
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_SKIP);
    hu_contact_profile_t cp;
    fake_contact(&cp);
    hu_channel_history_entry_t e[3];
    size_t n = make_thread(e, T_NOW);
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_SHADOW, &local);
    hu_proposer_context_capture_thread(&pc, e, n, T_NOW);
    pc.via = &rel; /* taken from a wrapper whose circuit has since opened */
    hu_proactive_compose_inputs_t in;
    plain_inputs(&in);
    hu_initiative_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_init_decision_t d;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    hu_proposer_context_decide(&pc, &cfg, NULL, 0, NULL, NULL, &reliable, &a, &cp, &in, T_NOW, &r,
                               &d);
    HU_ASSERT_EQ(g_local_llm.calls, 0);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_LOCAL_UNAVAILABLE);
    rel.vtable->deinit(rel.ctx, &a);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

static void shadow_without_local_provider_sends_the_thread_nowhere(void) {
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_SHADOW, NULL, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_tick.calls, 1);
    HU_ASSERT_STR_EQ(g_tick.msg[0], k_plain_golden);
    HU_ASSERT_EQ(rel_llm.calls, 0); /* never retried on the fallback chain */
    HU_ASSERT_EQ(pc.thread_len, (size_t)0);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_LOCAL_UNAVAILABLE);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

static void shadow_local_empty_answer_is_local_unavailable(void) {
    hu_provider_t local = local_provider(NULL); /* the "</think>"-only reply, stripped */
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_SHADOW, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_local_llm.calls, 1);
    HU_ASSERT_EQ(rel_llm.calls, 0);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_LOCAL_UNAVAILABLE);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_NEGATIVE);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

/* ── 5. LIVE ──────────────────────────────────────────────────────────── */

static void live_feeds_the_enriched_prompt_to_the_local_provider(void) {
    hu_provider_t local = local_provider(NULL);
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_FIRED, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_LIVE, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_tick.calls, 1);
    HU_ASSERT_TRUE(g_tick.provider_ctx[0] == (void *)&g_local_llm);
    HU_ASSERT_STR_CONTAINS(g_tick.msg[0], "them: did the roof guy");
    HU_ASSERT_STR_CONTAINS(g_tick.msg[0], "--- situation ---"); /* today's material kept */
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_FIRED);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_OK);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

static void live_local_failure_falls_back_to_todays_prompt_not_the_thread(void) {
    hu_provider_t local = local_provider(NULL);
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_LLM_ERROR, HU_INIT_RESULT_NEGATIVE);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_LIVE, &local, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_tick.calls, 2);
    HU_ASSERT_TRUE(g_tick.provider_ctx[0] == (void *)&g_local_llm);
    HU_ASSERT_TRUE(g_tick.provider_ctx[1] == (void *)&rel_llm);
    HU_ASSERT_STR_EQ(g_tick.msg[1], k_plain_golden); /* the retry carries no thread */
    /* one decision row: the failed local call records none, the fallback does */
    HU_ASSERT_TRUE(g_tick.defer_row[0]);
    HU_ASSERT_FALSE(g_tick.defer_row[1]);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_NEGATIVE);
    HU_ASSERT_EQ((int)pc.outcome, (int)HU_PROPOSER_CTX_LOCAL_UNAVAILABLE);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

static void live_without_local_provider_is_todays_call(void) {
    fake_llm_t rel_llm = {0};
    hu_provider_t reliable = {.ctx = &rel_llm, .vtable = &fake_llm_vtable};
    tick_reset(HU_INIT_RESULT_NEGATIVE, HU_INIT_RESULT_SKIP);
    hu_proposer_context_t pc;
    hu_init_proposer_result_t r = HU_INIT_RESULT_SKIP;
    run_decide(&pc, HU_GATE_LIVE, NULL, &reliable, T_NOW, &r);
    HU_ASSERT_EQ(g_tick.calls, 1);
    HU_ASSERT_TRUE(g_tick.provider_ctx[0] == (void *)&rel_llm);
    HU_ASSERT_STR_EQ(g_tick.msg[0], k_plain_golden);
    hu_proposer_context_set_tick_fn_for_test(NULL);
}

/* ── 6. gate + local-provider resolution ──────────────────────────────── */

static void mode_defaults_off_and_parses_shadow(void) {
    const char *prev = getenv("HU_PROPOSER_CONTEXT");
    char saved[32] = {0};
    if (prev)
        snprintf(saved, sizeof(saved), "%s", prev);
    hu_proposer_context_set_mode_for_test(-1);
    unsetenv("HU_PROPOSER_CONTEXT");
    HU_ASSERT_EQ((int)hu_proposer_context_mode(), (int)HU_GATE_OFF);
    setenv("HU_PROPOSER_CONTEXT", "shadow", 1);
    HU_ASSERT_EQ((int)hu_proposer_context_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_PROPOSER_CONTEXT", "live", 1);
    HU_ASSERT_EQ((int)hu_proposer_context_mode(), (int)HU_GATE_LIVE);
    if (prev)
        setenv("HU_PROPOSER_CONTEXT", saved, 1);
    else
        unsetenv("HU_PROPOSER_CONTEXT");
}

static void local_provider_accepts_loopback_and_refuses_cloud(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_provider_t loop = {0}, cloud = {0}, out = {0};
    const char *lu = "http://127.0.0.1:8741/v1", *cu = "https://api.example.com/v1";
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, lu, strlen(lu), &loop), HU_OK);
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, cu, strlen(cu), &cloud), HU_OK);
    HU_ASSERT_TRUE(hu_proposer_context_local_provider(&loop, &out));
    HU_ASSERT_TRUE(out.ctx == loop.ctx);
    HU_ASSERT_FALSE(hu_proposer_context_local_provider(&cloud, &out));
    HU_ASSERT_FALSE(hu_proposer_context_local_provider(NULL, &out));
    fake_llm_t f = {0};
    hu_provider_t notcompat = {.ctx = &f, .vtable = &fake_llm_vtable};
    HU_ASSERT_FALSE(hu_proposer_context_local_provider(&notcompat, &out));
    loop.vtable->deinit(loop.ctx, &a);
    cloud.vtable->deinit(cloud.ctx, &a);
}

static void local_provider_unwraps_reliable_primary_only(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_provider_t loop = {0}, cloud = {0}, rel = {0}, out = {0};
    const char *lu = "http://127.0.0.1:8741/v1", *cu = "https://api.example.com/v1";
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, lu, strlen(lu), &loop), HU_OK);
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, cu, strlen(cu), &cloud), HU_OK);
    hu_reliable_provider_entry_t extras[1] = {{.name = "cloud", .name_len = 5, .provider = cloud}};
    HU_ASSERT_EQ(hu_reliable_create_ex(&a, loop, 1, 50, extras, 1, NULL, 0, &rel), HU_OK);
    HU_ASSERT_TRUE(hu_proposer_context_local_provider(&rel, &out));
    HU_ASSERT_TRUE(out.ctx == loop.ctx); /* the primary, never the cloud extra */
    rel.vtable->deinit(rel.ctx, &a);

    /* cloud primary + loopback extra: the fallback is not a local pin */
    hu_provider_t loop2 = {0}, cloud2 = {0}, rel2 = {0};
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, lu, strlen(lu), &loop2), HU_OK);
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, cu, strlen(cu), &cloud2), HU_OK);
    hu_reliable_provider_entry_t extras2[1] = {{.name = "mlx", .name_len = 3, .provider = loop2}};
    HU_ASSERT_EQ(hu_reliable_create_ex(&a, cloud2, 1, 50, extras2, 1, NULL, 0, &rel2), HU_OK);
    HU_ASSERT_FALSE(hu_proposer_context_local_provider(&rel2, &out));
    rel2.vtable->deinit(rel2.ctx, &a);
}

static void reliable_primary_is_unavailable_while_the_circuit_is_open(void) {
    hu_allocator_t a = hu_system_allocator();
    int dummy_p = 0, dummy_f = 0;
    hu_provider_t prim = {.ctx = &dummy_p, .vtable = &failing_vtable};
    hu_provider_t fb = {.ctx = &dummy_f, .vtable = &failing_vtable};
    hu_reliable_provider_entry_t extras[1] = {{.name = "fb", .name_len = 2, .provider = fb}};
    hu_provider_t rel = {0}, out = {0};
    HU_ASSERT_EQ(hu_reliable_create_ex(&a, prim, 0, 50, extras, 1, NULL, 0, &rel), HU_OK);
    hu_reliable_set_circuit(&rel, 1, 300);
    HU_ASSERT_EQ(hu_reliable_primary(&rel, &out), HU_OK); /* pre: closed */
    HU_ASSERT_TRUE(out.ctx == (void *)&dummy_p);
    char *r = NULL;
    size_t rl = 0;
    (void)rel.vtable->chat_with_system(rel.ctx, &a, "s", 1, "m", 1, "x", 1, 0.1, &r, &rl);
    HU_ASSERT_EQ(hu_reliable_primary(&rel, &out), HU_ERR_PROVIDER_UNAVAILABLE);
    HU_ASSERT_EQ(hu_reliable_primary(&prim, &out), HU_ERR_INVALID_ARGUMENT); /* not a wrapper */
    rel.vtable->deinit(rel.ctx, &a);
}

/* Review fix: a context-enriched verdict never logs the reason text. */
static void verdict_log_drops_reason_text_when_the_block_was_used(void) {
    hu_proactive_compose_inputs_t in;
    plain_inputs(&in);
    hu_init_decision_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.skip_reason, sizeof(d.skip_reason), "she said the roof guy flaked again");
    d.skip_reason_len = strlen(d.skip_reason);
    char line[512];
    hu_log_content_set_for_test(1); /* HU_LOG_CONTENT=1: the debug-only text line */
    hu_init_proposer_format_ex_verdict(&in, &d, 9, 100, line, sizeof(line));
    HU_ASSERT_STR_CONTAINS(line, "reason=she said the roof guy");
    hu_log_content_set_for_test(0); /* default: length only, even without the block */
    hu_init_proposer_format_ex_verdict(&in, &d, 9, 100, line, sizeof(line));
    HU_ASSERT_STR_NOT_CONTAINS(line, "roof");
    HU_ASSERT_STR_CONTAINS(line, "reason=<34 chars>");
    hu_log_content_set_for_test(1);
    in.proposer_context = "\n--- recent thread ---\n[1h ago] them: the roof guy flaked\n";
    in.proposer_context_len = strlen(in.proposer_context);
    hu_init_proposer_format_ex_verdict(&in, &d, 9, 100, line, sizeof(line));
    HU_ASSERT_STR_NOT_CONTAINS(line, "roof");
    HU_ASSERT_STR_CONTAINS(line, "reason_len=34");
    hu_log_content_set_for_test(-1);
}

static void reliable_reports_which_provider_served(void) {
    hu_allocator_t a = hu_system_allocator();
    int dp = 0;
    hu_provider_t prim = {.ctx = &dp, .vtable = &failing_vtable};
    fake_llm_t fb_llm = {0};
    fb_llm.reply = "ok";
    hu_provider_t fb = {.ctx = &fb_llm, .vtable = &fake_llm_vtable};
    hu_reliable_provider_entry_t extras[1] = {{.name = "fb", .name_len = 2, .provider = fb}};
    hu_provider_t rel = {0};
    HU_ASSERT_EQ(hu_reliable_create_ex(&a, prim, 0, 50, extras, 1, NULL, 0, &rel), HU_OK);
    HU_ASSERT_EQ(hu_reliable_last_served(&rel), -1); /* pre: nothing served */
    char *o = NULL;
    size_t ol = 0;
    HU_ASSERT_EQ(rel.vtable->chat_with_system(rel.ctx, &a, "s", 1, "m", 1, "x", 1, 0.1, &o, &ol),
                 HU_OK);
    HU_ASSERT_EQ(hu_reliable_last_served(&rel), 1); /* the fallback answered */
    a.free(a.ctx, o, ol + 1);
    HU_ASSERT_EQ(hu_reliable_last_served(&prim), -1); /* not a wrapper */
    rel.vtable->deinit(rel.ctx, &a);
}

void run_daemon_proposer_context_tests(void) {
    HU_TEST_SUITE("daemon_proposer_context");
    HU_RUN_TEST(render_thread_labels_speakers_times_and_days_since);
    HU_RUN_TEST(render_thread_keeps_newest_lines_within_cap);
    HU_RUN_TEST(render_thread_empty_writes_nothing_and_unknown_days);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(build_fills_contact_conversation_memory_within_budget);
    HU_RUN_TEST(build_holds_the_budget_with_oversized_inputs);
    HU_RUN_TEST(build_drops_insights_the_safety_predicate_rejects);
    HU_RUN_TEST(insights_are_chosen_by_the_thread_like_the_reactive_path);
#endif
    HU_RUN_TEST(thread_is_never_captured_without_a_local_provider);
    HU_RUN_TEST(nothing_is_captured_when_off);
    HU_RUN_TEST(user_message_without_block_matches_todays_bytes);
    HU_RUN_TEST(decide_off_is_exactly_todays_call);
    HU_RUN_TEST(shadow_keeps_the_decision_input_and_runs_local_once_per_cycle);
    HU_RUN_TEST(shadow_skips_when_production_never_reached_the_model);
    HU_RUN_TEST(shadow_scores_both_prompts_through_the_production_threshold);
    HU_RUN_TEST(shadow_rechecks_the_circuit_before_calling_local);
    HU_RUN_TEST(shadow_without_local_provider_sends_the_thread_nowhere);
    HU_RUN_TEST(shadow_local_empty_answer_is_local_unavailable);
    HU_RUN_TEST(live_feeds_the_enriched_prompt_to_the_local_provider);
    HU_RUN_TEST(live_local_failure_falls_back_to_todays_prompt_not_the_thread);
    HU_RUN_TEST(live_without_local_provider_is_todays_call);
    HU_RUN_TEST(mode_defaults_off_and_parses_shadow);
    HU_RUN_TEST(local_provider_accepts_loopback_and_refuses_cloud);
    HU_RUN_TEST(local_provider_unwraps_reliable_primary_only);
    HU_RUN_TEST(reliable_primary_is_unavailable_while_the_circuit_is_open);
    HU_RUN_TEST(verdict_log_drops_reason_text_when_the_block_was_used);
    HU_RUN_TEST(reliable_reports_which_provider_served);
}
