/* test_replay_turn.c — the real-turn replay harness (include/human/daemon/replay_turn.h).
 *
 * Hermetic: a scripted recording provider (tests/turn_recording_provider.h),
 * a null channel, no persona files, no network, no chat.db, no memory.db.
 *
 * The four contracts the harness exists for:
 *   1. nothing is ever sent (the null channel's outbound counters stay 0);
 *   2. nothing leaves the machine (non-loopback endpoints are refused);
 *   3. a gate's env var reaches the reply request (two arms differ), and the
 *      same arm twice is byte-identical (arms are comparable);
 *   4. the turn never touches the session store (memory.db stays unwritten).
 */
#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/daemon/director_v2.h"
#include "human/daemon/replay_turn.h"
#include "human/memory.h"
#include "human/persona.h"
#include "test_framework.h"
#include "turn_recording_provider.h"

#include <stdlib.h>
#include <string.h>

/* ── helpers ────────────────────────────────────────────────────────── */

static const hu_channel_history_entry_t k_history[] = {
    {.from_me = false, .text = "you still coming saturday", .timestamp = "2026-09-30 18:01:00"},
    {.from_me = true, .text = "yeah def", .timestamp = "2026-09-30 18:03:00"},
};

typedef struct rt_fixture {
    hu_allocator_t alloc;
    trp_t trp;
    hu_replay_provider_t rp;
    hu_agent_t agent;
    bool agent_ok;
} rt_fixture_t;

static bool rt_setup(rt_fixture_t *f, const char *reply) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    trp_init(&f->trp, NULL, 0, reply);
    hu_replay_provider_init(&f->rp, trp_provider(&f->trp), false, "replay-model", true, 0.0);
    f->agent_ok =
        hu_agent_from_config(&f->agent, &f->alloc, hu_replay_provider_as_provider(&f->rp), NULL, 0,
                             NULL, NULL, NULL, NULL, "agent-model", 11, "trp", 3, 0.7, "/tmp", 4, 4,
                             50, false, 2, NULL, 0, NULL, 0, NULL) == HU_OK;
    return f->agent_ok;
}

static void rt_teardown(rt_fixture_t *f) {
    if (f->agent_ok)
        hu_agent_deinit(&f->agent);
    hu_replay_provider_deinit(&f->rp, &f->alloc);
    trp_deinit(&f->trp);
}

static hu_replay_turn_input_t rt_input(const char *inbound) {
    hu_replay_turn_input_t in = {
        .contact_id = "+15550001111",
        .inbound = inbound,
        .inbound_len = strlen(inbound),
        .history = k_history,
        .history_count = sizeof(k_history) / sizeof(k_history[0]),
        .director = true,
        .seed = 42,
    };
    return in;
}

/* ── 2. loopback only ───────────────────────────────────────────────── */

static void replay_url_loopback_accepts_only_loopback_hosts(void) {
    HU_ASSERT_TRUE(hu_replay_url_is_loopback("http://127.0.0.1:8741/v1"));
    HU_ASSERT_TRUE(hu_replay_url_is_loopback("http://localhost:8743"));
    HU_ASSERT_TRUE(hu_replay_url_is_loopback("http://[::1]:8741/v1"));
    HU_ASSERT_TRUE(hu_replay_url_is_loopback("https://127.0.0.1/v1"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("https://api.openai.com/v1"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback(
        "https://aiplatform.googleapis.com/v1/projects/p/locations/global"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("http://10.0.0.5:8741/v1"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("http://localhost.evil.com/v1"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("http://127.0.0.1.nip.io:8741"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("http://evil.com@127.0.0.1:8741"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("http://127.0.0.1@evil.com/"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("ftp://127.0.0.1/"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback("127.0.0.1:8741"));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback(""));
    HU_ASSERT_FALSE(hu_replay_url_is_loopback(NULL));
}

static void replay_provider_create_local_refuses_cloud_endpoint(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p;
    memset(&p, 0xAB, sizeof(p));
    hu_error_t err =
        hu_replay_provider_create_local(&alloc, "mlx_local", "https://api.openai.com/v1", "k", &p);
    HU_ASSERT_EQ(err, HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(p.vtable);
    HU_ASSERT_NULL(p.ctx);

    err =
        hu_replay_provider_create_local(&alloc, "mlx_local", "http://127.0.0.1:8741/v1", NULL, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable);
    if (p.vtable && p.vtable->deinit)
        p.vtable->deinit(p.ctx, &alloc);
}

/* ── 1. nothing is sent ─────────────────────────────────────────────── */

static void replay_channel_counts_every_outbound_entry(void) {
    /* The counter the next test relies on really counts: a direct send and a
     * react each register, and both are refused. */
    hu_replay_channel_t rc;
    hu_replay_channel_init(&rc, NULL, 0);
    hu_channel_t ch = hu_replay_channel_as_channel(&rc);
    HU_ASSERT_EQ(rc.outbound_calls, 0);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imessage");
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "x", 1, "hi", 2, NULL, 0), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "x", 1, 7, HU_REACTION_HEART), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(rc.outbound_calls, 2);
}

static void replay_turn_never_calls_channel_send(void) {
    rt_fixture_t f;
    HU_ASSERT_TRUE(rt_setup(&f, "yeah im around. what time"));
    hu_replay_turn_input_t in = rt_input("hey are you around tonight?");
    hu_replay_turn_result_t r;
    hu_error_t err = hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(r.action, HU_REPLAY_ACTION_TEXT);
    HU_ASSERT_GE(r.bubble_count, 1);
    HU_ASSERT_NOT_NULL(r.text);
    HU_ASSERT_STR_CONTAINS(r.text, "around");
    HU_ASSERT_GE(r.reply_calls, 1);
    HU_ASSERT_EQ(r.channel_outbound_calls, 0);
    hu_replay_turn_result_deinit(&f.alloc, &r);
    rt_teardown(&f);
}

static void replay_turn_director_tapback_skips_the_reply_turn(void) {
    rt_fixture_t f;
    HU_ASSERT_TRUE(rt_setup(&f, "should never be asked"));
    hu_replay_turn_input_t in = rt_input("k"); /* test director: short ack -> tapback */
    hu_replay_turn_result_t r;
    HU_ASSERT_EQ(hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r), HU_OK);
    HU_ASSERT_EQ(r.action, HU_REPLAY_ACTION_TAPBACK);
    HU_ASSERT_TRUE(r.director_valid);
    HU_ASSERT_EQ(r.reply_calls, 0);
    HU_ASSERT_EQ(r.bubble_count, 0);
    HU_ASSERT_EQ(r.channel_outbound_calls, 0);
    hu_replay_turn_result_deinit(&f.alloc, &r);

    /* Director off (an arm knob): the same "k" gets a text turn. */
    in.director = false;
    HU_ASSERT_EQ(hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r), HU_OK);
    HU_ASSERT_EQ(r.action, HU_REPLAY_ACTION_TEXT);
    HU_ASSERT_FALSE(r.director_valid);
    HU_ASSERT_GE(r.reply_calls, 1);
    hu_replay_turn_result_deinit(&f.alloc, &r);
    rt_teardown(&f);
}

/* A replay must never queue a director-v2 SHADOW job: the shadow worker would
 * call the live local endpoint with a replayed turn (#612 review). SHADOW in
 * the environment replays as OFF (v1 only); LIVE stays LIVE, because the
 * cut-over A/B measures HU_DIRECTOR_V2=live through this harness. */
static int s_v2_worker_calls;

static hu_error_t v2_worker_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                                 const char *msg, size_t msg_len, const char *model,
                                 size_t model_len, double temperature, char **out,
                                 size_t *out_len) {
    (void)ctx;
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    s_v2_worker_calls++;
    static const char k_reply[] = "action:text|direction:engage fully";
    *out = (char *)alloc->alloc(alloc->ctx, sizeof(k_reply));
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, k_reply, sizeof(k_reply));
    *out_len = sizeof(k_reply) - 1;
    return HU_OK;
}

static const char *v2_worker_name(void *ctx) {
    (void)ctx;
    return "v2worker";
}

static const hu_provider_vtable_t k_v2_worker_vt = {.chat_with_system = v2_worker_chat,
                                                    .get_name = v2_worker_name};

static void replay_turn_director_v2_shadow_queues_no_shadow_job(void) {
    rt_fixture_t f;
    HU_ASSERT_TRUE(rt_setup(&f, "should never be asked"));
    /* The replay agent's primary is the local endpoint, as in `human replay`. */
    static hu_config_t cfg;
    static hu_provider_entry_t entry;
    memset(&cfg, 0, sizeof(cfg));
    memset(&entry, 0, sizeof(entry));
    entry.name = "trp";
    entry.base_url = "http://127.0.0.1:8741/v1";
    cfg.providers = &entry;
    cfg.providers_len = 1;
    cfg.default_provider = "trp";
    const hu_config_t *saved_cfg = f.agent.config;
    f.agent.config = &cfg;
    HU_ASSERT_NOT_NULL(hu_director_v2_primary_endpoint(&f.agent)); /* shadow would enqueue */
    hu_provider_t worker = {.ctx = NULL, .vtable = &k_v2_worker_vt};
    hu_director_v2_set_worker_provider(&worker);
    s_v2_worker_calls = 0;

    setenv("HU_DIRECTOR_V2", "shadow", 1);
    hu_replay_turn_input_t in = rt_input("k");
    hu_replay_turn_result_t r;
    HU_ASSERT_EQ(hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r), HU_OK);
    HU_ASSERT_TRUE(hu_director_v2_shadow_drain(5000));
    HU_ASSERT_EQ(s_v2_worker_calls, 0); /* no shadow job reached the endpoint */
    HU_ASSERT_TRUE(r.director_valid);
    HU_ASSERT_EQ(r.action, HU_REPLAY_ACTION_TAPBACK); /* v1 decided, as under OFF */
    hu_replay_turn_result_deinit(&f.alloc, &r);

    unsetenv("HU_DIRECTOR_V2");
    hu_director_v2_set_worker_provider(NULL);
    f.agent.config = saved_cfg;
    rt_teardown(&f);
}

/* ── 3. a gate's env reaches the request; arms are deterministic ───── */

#define RT_GATE "HU_MAX_TOKENS_RESOLVE"

static uint64_t rt_reply_fp_with(const char *gate_value, size_t *captured_len) {
    const char *prev = getenv(RT_GATE);
    char saved[64] = "";
    bool had = prev != NULL;
    if (had)
        snprintf(saved, sizeof(saved), "%s", prev);
    setenv(RT_GATE, gate_value, 1);
    rt_fixture_t f;
    uint64_t fp = 0;
    if (rt_setup(&f, "sounds good")) {
        f.rp.capture = true;
        hu_replay_turn_input_t in = rt_input("what are you up to this weekend");
        hu_replay_turn_result_t r;
        if (hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r) == HU_OK) {
            fp = r.reply_fp;
            hu_replay_turn_result_deinit(&f.alloc, &r);
        }
        if (captured_len)
            *captured_len = f.rp.captured_len;
        rt_teardown(&f);
    }
    if (had)
        setenv(RT_GATE, saved, 1);
    else
        unsetenv(RT_GATE);
    return fp;
}

static void replay_turn_gate_env_changes_reply_request(void) {
    size_t cap_off = 0;
    uint64_t off_a = rt_reply_fp_with("off", &cap_off);
    uint64_t off_b = rt_reply_fp_with("off", NULL);
    uint64_t live = rt_reply_fp_with("live", NULL);
    HU_ASSERT_NEQ(off_a, 0);
    HU_ASSERT_GT(cap_off, 0);
    HU_ASSERT_EQ(off_a, off_b); /* same arm twice: byte-identical request */
    HU_ASSERT_NEQ(off_a, live); /* the gate reached the request */
}

static void replay_provider_pins_model_and_temperature(void) {
    rt_fixture_t f;
    HU_ASSERT_TRUE(rt_setup(&f, "ok cool"));
    hu_replay_turn_input_t in = rt_input("did you see the game");
    hu_replay_turn_result_t r;
    HU_ASSERT_EQ(hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r), HU_OK);
    HU_ASSERT_NOT_NULL(f.trp.log);
    HU_ASSERT_STR_CONTAINS(f.trp.log, "model=replay-model temperature=0.000");
    HU_ASSERT_STR_NOT_CONTAINS(f.trp.log, "model=agent-model");
    HU_ASSERT_STR_NOT_CONTAINS(f.trp.log, "temperature=0.700");
    hu_replay_turn_result_deinit(&f.alloc, &r);
    rt_teardown(&f);
}

/* HU_LENGTH_POLICY (#580) lives in the shared reply-budget seam, so the replay
 * measures it: LIVE with the contact's measured lengths changes the reply
 * request; OFF twice is byte-identical. */
static const char k_lp_persona[] =
    "{\"version\":1,\"name\":\"lptest\","
    "\"core\":{\"identity\":\"Seth\",\"traits\":[\"warm\"]},"
    "\"contacts\":{\"+15550001111\":{\"name\":\"Lexi\",\"relationship\":\"friend\","
    "\"reply_chars_p90\":50,\"reply_chars_p50\":21}}}";

static uint64_t rt_length_policy_fp(const char *mode, uint32_t *max_chars) {
    const char *prev = getenv("HU_LENGTH_POLICY");
    char saved[32] = "";
    bool had = prev != NULL;
    if (had)
        snprintf(saved, sizeof(saved), "%s", prev);
    if (mode)
        setenv("HU_LENGTH_POLICY", mode, 1);
    else
        unsetenv("HU_LENGTH_POLICY");
    rt_fixture_t f;
    uint64_t fp = 0;
    if (rt_setup(&f, "lol same")) {
        hu_persona_t *p = (hu_persona_t *)f.alloc.alloc(f.alloc.ctx, sizeof(hu_persona_t));
        memset(p, 0, sizeof(*p));
        if (hu_persona_load_json(&f.alloc, k_lp_persona, strlen(k_lp_persona), p) == HU_OK) {
            f.agent.persona = p; /* owned by the agent from here */
            /* A question: today's rule says "keep it tight" at cap <= 80; the
             * policy, with this contact's lengths, does not tighten a question. */
            hu_replay_turn_input_t in = rt_input("you around later tonight?");
            in.director = false;
            hu_replay_turn_result_t r;
            if (hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r) == HU_OK) {
                fp = r.reply_fp;
                *max_chars = r.max_chars;
                hu_replay_turn_result_deinit(&f.alloc, &r);
            }
        } else {
            f.alloc.free(f.alloc.ctx, p, sizeof(*p));
        }
        rt_teardown(&f);
    }
    if (had)
        setenv("HU_LENGTH_POLICY", saved, 1);
    else
        unsetenv("HU_LENGTH_POLICY");
    return fp;
}

static void replay_turn_length_policy_live_changes_reply_request(void) {
    uint32_t cap_off = 0, cap_off2 = 0, cap_live = 0;
    uint64_t off = rt_length_policy_fp(NULL, &cap_off);
    uint64_t off2 = rt_length_policy_fp("off", &cap_off2);
    uint64_t live = rt_length_policy_fp("live", &cap_live);
    HU_ASSERT_NEQ(off, 0);
    HU_ASSERT_EQ(off, off2); /* unset == off, byte for byte */
    HU_ASSERT_EQ(cap_off, cap_off2);
    HU_ASSERT_NEQ(off, live); /* the gate reached the reply request */
}

/* ── 4. the session store (memory.db) is never touched ──────────────── */

typedef struct spy_store {
    size_t saves;
    size_t loads;
    size_t clears;
} spy_store_t;

static hu_error_t spy_save(void *ctx, const char *sid, size_t sid_len, const char *role,
                           size_t role_len, const char *content, size_t content_len) {
    (void)sid, (void)sid_len, (void)role, (void)role_len, (void)content, (void)content_len;
    ((spy_store_t *)ctx)->saves++;
    return HU_OK;
}
static hu_error_t spy_load(void *ctx, hu_allocator_t *alloc, const char *sid, size_t sid_len,
                           hu_message_entry_t **out, size_t *out_count) {
    (void)alloc, (void)sid, (void)sid_len;
    ((spy_store_t *)ctx)->loads++;
    *out = NULL;
    *out_count = 0;
    return HU_OK;
}
static hu_error_t spy_clear(void *ctx, const char *sid, size_t sid_len) {
    (void)sid, (void)sid_len;
    ((spy_store_t *)ctx)->clears++;
    return HU_OK;
}
static const hu_session_store_vtable_t k_spy_vtable = {
    .save_message = spy_save,
    .load_messages = spy_load,
    .clear_messages = spy_clear,
    .clear_auto_saved = spy_clear,
};

static void replay_turn_detaches_the_session_store(void) {
    rt_fixture_t f;
    HU_ASSERT_TRUE(rt_setup(&f, "lol yes"));
    spy_store_t spy = {0};
    hu_session_store_t store = {.ctx = &spy, .vtable = &k_spy_vtable};
    f.agent.session_store = &store;
    f.agent.auto_save = true;
    hu_replay_turn_input_t in = rt_input("you seen the new trailer");
    hu_replay_turn_result_t r;
    HU_ASSERT_EQ(hu_replay_turn_run(&f.alloc, &f.agent, NULL, &f.rp, &in, &r), HU_OK);
    HU_ASSERT_EQ(r.action, HU_REPLAY_ACTION_TEXT);
    HU_ASSERT_EQ(spy.saves + spy.loads + spy.clears, 0);
    HU_ASSERT_TRUE(f.agent.session_store == &store); /* restored afterwards */
    HU_ASSERT_NULL(f.agent.conversation_context);
    HU_ASSERT_NULL(f.agent.contact_context);
    hu_replay_turn_result_deinit(&f.alloc, &r);
    f.agent.session_store = NULL;
    rt_teardown(&f);
}

void run_replay_turn_tests(void) {
    HU_TEST_SUITE("replay_turn");
    HU_RUN_TEST(replay_url_loopback_accepts_only_loopback_hosts);
    HU_RUN_TEST(replay_provider_create_local_refuses_cloud_endpoint);
    HU_RUN_TEST(replay_channel_counts_every_outbound_entry);
    HU_RUN_TEST(replay_turn_never_calls_channel_send);
    HU_RUN_TEST(replay_turn_director_tapback_skips_the_reply_turn);
    HU_RUN_TEST(replay_turn_director_v2_shadow_queues_no_shadow_job);
    HU_RUN_TEST(replay_turn_gate_env_changes_reply_request);
    HU_RUN_TEST(replay_provider_pins_model_and_temperature);
    HU_RUN_TEST(replay_turn_length_policy_live_changes_reply_request);
    HU_RUN_TEST(replay_turn_detaches_the_session_store);
}
