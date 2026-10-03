/* Director v2 (HU_DIRECTOR_V2): intent not length, tapback-vs-text from
 * Seth's learned data, 12 messages of context. Hermetic: the provider is a
 * mock, no channel, no DB. */
#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/daemon/common.h"
#include "human/daemon/director.h"
#include "human/daemon/director_v2.h"
#include "human/persona.h"
#include "human/providers/compatible.h"
#include "test_framework.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define S(lit) (lit), (sizeof(lit) - 1)

/* ── brevity helper ─────────────────────────────────────────────────── */

static void director_v2_brevity_flags_the_prod_directions(void) {
    static const char *const brief[] = {
        "Acknowledge the sickness briefly, keep it low-pressure, one line",
        "Keep it light and non-committal, admit he hasn't really thought about it yet",
        "Laugh it off",
        "Tease them back, just a few words",
        "Just acknowledge the hard news, 5 words, don't fix it",
        "Agree, don't over-explain",
        "He's busy, one-word reply when he gets back",
        "Reassure them with a simple, confident one-liner.",
    };
    for (size_t i = 0; i < sizeof(brief) / sizeof(brief[0]); i++)
        HU_ASSERT_TRUE(hu_director_brevity_directive(brief[i], strlen(brief[i])));
    HU_ASSERT_EQ(hu_director_directive_flags(S("Greet back, one line")), HU_DIRECTIVE_LENGTH);
    HU_ASSERT_EQ(hu_director_directive_flags(S("Laugh it off")), HU_DIRECTIVE_DEFLECT);
    HU_ASSERT_EQ(hu_director_directive_flags(S("keep it light, one word")),
                 HU_DIRECTIVE_LENGTH | HU_DIRECTIVE_DEFLECT);
}

static void director_v2_brevity_passes_engaged_directions(void) {
    static const char *const deep[] = {
        "She's sick and worried about him too ; engage fully, ask how bad it is ; her news",
        "Make him laugh",
        "He thinks something broke ; take it seriously, ask what it said ; nothing",
        "Share something of his own about the trip ; the trip in the thread",
        "Someone online said it shortly after",
    };
    for (size_t i = 0; i < sizeof(deep) / sizeof(deep[0]); i++)
        HU_ASSERT_FALSE(hu_director_brevity_directive(deep[i], strlen(deep[i])));
    HU_ASSERT_FALSE(hu_director_brevity_directive(NULL, 3));
    HU_ASSERT_FALSE(hu_director_brevity_directive(S("")));
}

/* ── prompt construction ────────────────────────────────────────────── */

static void director_v2_system_prompt_has_no_length_instructions(void) {
    static char buf[8192];
    unsetenv("HU_DIRECTOR_FORMS");
    size_t n = hu_director_v2_system_prompt(buf, sizeof(buf));
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_LE(n, (size_t)HU_DIRECTOR_V2_SYSTEM_CAP);
    HU_ASSERT_EQ((hu_director_directive_flags(buf, n) & HU_DIRECTIVE_LENGTH), 0u);
    HU_ASSERT_STR_CONTAINS(buf, "really mean or ask");
    HU_ASSERT_STR_CONTAINS(buf, "ask a follow-up");
    HU_ASSERT_STR_CONTAINS(buf, "share something of his own");
    HU_ASSERT_STR_CONTAINS(buf, "just react");
    HU_ASSERT_STR_CONTAINS(buf, "Never dodge");
    HU_ASSERT_STR_CONTAINS(buf, "How Seth replies");
    HU_ASSERT_STR_CONTAINS(buf, "Always write the direction");
    /* No fixed behaviour rules: no tapback word list, no silence rule, no
     * delay ranges, no topic list, and no examples to copy. */
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Never tapback");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "pure acknowledgement");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "unanswered");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "2-8");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "15-60");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "illness");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Examples");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "action:text|");
    /* v1, for contrast, asks for length: the thing v2 removes. */
    static char v1[16384];
    size_t n1 = hu_daemon_director_system_prompt(v1, sizeof(v1));
    HU_ASSERT_NEQ((hu_director_directive_flags(v1, n1) & HU_DIRECTIVE_LENGTH), 0u);
}

static void director_v2_system_prompt_forms_block_stays_in_budget(void) {
    static char off[8192], live[8192];
    unsetenv("HU_DIRECTOR_FORMS");
    size_t n_off = hu_director_v2_system_prompt(off, sizeof(off));
    setenv("HU_DIRECTOR_FORMS", "live", 1);
    size_t n_live = hu_director_v2_system_prompt(live, sizeof(live));
    unsetenv("HU_DIRECTOR_FORMS");
    HU_ASSERT_GT(n_live, n_off);
    HU_ASSERT_LE(n_live, (size_t)HU_DIRECTOR_V2_SYSTEM_CAP);
    HU_ASSERT_STR_CONTAINS(live, "action:voice");
    HU_ASSERT_EQ((hu_director_directive_flags(live, n_live) & HU_DIRECTIVE_LENGTH), 0u);
    HU_ASSERT_EQ(hu_director_v2_system_prompt(live, 64), 0u); /* does not fit */
}

static size_t count_occurrences(const char *hay, const char *needle) {
    size_t c = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
        c++;
    return c;
}

static void fill_entries(hu_channel_history_entry_t *e, size_t n, size_t text_len) {
    memset(e, 0, n * sizeof(*e));
    for (size_t i = 0; i < n; i++) {
        e[i].from_me = (i % 2) == 1;
        int w = snprintf(e[i].text, sizeof(e[i].text), "msg%02zu ", i);
        size_t at = (size_t)w;
        while (at < text_len && at + 1 < sizeof(e[i].text))
            e[i].text[at++] = 'x';
        e[i].text[at] = '\0';
    }
}

static void director_v2_user_prompt_has_twelve_labelled_messages(void) {
    hu_channel_history_entry_t e[15];
    fill_entries(e, 15, 20);
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.name = "Maureen";
    cp.relationship = "mom";
    cp.dunbar_layer = "intimate";
    char buf[HU_DIRECTOR_V2_USER_CAP];
    size_t n = hu_director_v2_user_prompt(buf, sizeof(buf), &cp, e, 15, S("how are you doing"),
                                          NULL, "How Seth reacts, measured: 3%.");
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_EQ(count_occurrences(buf, "\nSeth: ") + count_occurrences(buf, "\nThem: "), 12u);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "msg02 "); /* the 13th-newest is dropped */
    HU_ASSERT_STR_CONTAINS(buf, "Seth: msg03 ");
    HU_ASSERT_STR_CONTAINS(buf, "Them: msg14 ");
    HU_ASSERT_STR_CONTAINS(buf, "Contact: mom; dunbar layer: intimate");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Maureen");
    HU_ASSERT_STR_CONTAINS(buf, "How Seth reacts, measured: 3%.\nThread");
    HU_ASSERT_STR_CONTAINS(buf, "how are you doing");
    HU_ASSERT_EQ((hu_director_directive_flags(buf, n) & HU_DIRECTIVE_LENGTH), 0u);
}

static const char k_long_facts[] =
    "How Seth reacts, measured from his own texts: with them, when they tell him something he "
    "replies with only a reaction 100% of the time (n=4000000000); his reactions: heart 100%, "
    "haha 100%, thumbs_up 100%, emphasis 100%, thumbs_down 100%, question 100%; after his "
    "reaction-only replies they went quiet or pushed for a real answer 100% of the time "
    "(n=4000000000).";

static void director_v2_user_prompt_stays_in_budget_with_long_messages(void) {
    hu_channel_history_entry_t e[12];
    fill_entries(e, 12, 511);
    char big[1200];
    memset(big, 'y', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    static char buf[16384];
    size_t n = hu_director_v2_user_prompt(buf, sizeof(buf), NULL, e, 12, big, strlen(big),
                                          "This turn: voice memo available", k_long_facts);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_LE(n, (size_t)HU_DIRECTOR_V2_USER_CAP);
    HU_ASSERT_EQ(strlen(buf), n);
    HU_ASSERT_EQ(count_occurrences(buf, "\nSeth: ") + count_occurrences(buf, "\nThem: "), 12u);
    HU_ASSERT_STR_CONTAINS(buf, "This turn: voice memo available");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Contact:");
    HU_ASSERT_STR_CONTAINS(buf, "How Seth reacts");
}

/* ── the call and the gate, against a mock provider ─────────────────── */

/* At every field's maximum the new message and the situation line still
 * fit; the history gives way, oldest message first. */
static void director_v2_user_prompt_drops_oldest_history_first(void) {
    hu_channel_history_entry_t e[12];
    fill_entries(e, 12, 511);
    char rel[64], dun[64], big[1200], sit[240];
    memset(rel, 'r', sizeof(rel) - 1);
    rel[sizeof(rel) - 1] = '\0';
    memset(dun, 'd', sizeof(dun) - 1);
    dun[sizeof(dun) - 1] = '\0';
    memset(big, 'y', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memset(sit, 's', sizeof(sit) - 1);
    memcpy(sit, "This turn:", 10);
    sit[sizeof(sit) - 1] = '\0';
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.relationship = rel;
    cp.dunbar_layer = dun;
    static char buf[16384];
    size_t n = hu_director_v2_user_prompt(buf, sizeof(buf), &cp, e, 12, big, strlen(big), sit,
                                          k_long_facts);
    HU_ASSERT_LE(n, (size_t)HU_DIRECTOR_V2_USER_CAP);
    HU_ASSERT_STR_CONTAINS(buf, "This turn:");
    HU_ASSERT_STR_CONTAINS(buf, "New message from them:\nyyyy");
    HU_ASSERT_STR_CONTAINS(buf, "Seth: msg11 ");     /* the newest message kept */
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Them: msg00 "); /* the oldest dropped */
    HU_ASSERT_LT(count_occurrences(buf, "\nSeth: ") + count_occurrences(buf, "\nThem: "), 12u);
}

typedef struct {
    const char *reply;
    bool fail;
    atomic_int calls;
    atomic_bool block;    /* hold the call until released (or 3 s) */
    atomic_bool entered;  /* the call has started */
    atomic_bool finished; /* the call has returned */
    int hold_ms;          /* with block: hold this long (0 = up to 3 s) */
    size_t sys_len, user_len;
    char user[HU_DIRECTOR_V2_USER_CAP];
} mock_ctx_t;

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static hu_error_t mock_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                        size_t sys_len, const char *msg, size_t msg_len,
                                        const char *model, size_t model_len, double temperature,
                                        char **out, size_t *out_len) {
    (void)sys;
    (void)model;
    (void)model_len;
    (void)temperature;
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    atomic_fetch_add(&m->calls, 1);
    atomic_store(&m->entered, true);
    int hold = m->hold_ms > 0 ? m->hold_ms : 3000;
    for (int waited = 0; atomic_load(&m->block) && waited < hold; waited += 5)
        sleep_ms(5);
    m->sys_len = sys_len;
    m->user_len = msg_len;
    size_t cp = msg_len < sizeof(m->user) - 1 ? msg_len : sizeof(m->user) - 1;
    memcpy(m->user, msg, cp);
    m->user[cp] = '\0';
    atomic_store(&m->finished, true);
    if (m->fail)
        return HU_ERR_INTERNAL;
    *out = hu_strndup(alloc, m->reply, strlen(m->reply));
    *out_len = *out ? strlen(m->reply) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const char *mock_get_name(void *ctx) {
    (void)ctx;
    return "mockdir";
}

static const hu_provider_vtable_t mock_vtable = {.chat_with_system = mock_chat_with_system,
                                                 .get_name = mock_get_name};

/* An agent whose primary provider ("mockdir") is on `url`. */
static hu_agent_t g_agent;
static hu_config_t g_cfg;
static hu_provider_entry_t g_entry;

static hu_agent_t *agent_with_director_at(const char *url) {
    memset(&g_agent, 0, sizeof(g_agent));
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_entry, 0, sizeof(g_entry));
    g_entry.name = "mockdir";
    g_entry.base_url = (char *)url;
    g_cfg.providers = &g_entry;
    g_cfg.providers_len = 1;
    g_cfg.default_provider = "mockdir";
    g_agent.config = &g_cfg;
    return &g_agent;
}

/* Seth's learned profile for this contact: when they tell him something he
 * replies with only a reaction 2% of the time; the lower quartile of all his
 * cells is 0.10. Data, not a rule, turns the model's tapback into text. */
static hu_tapback_profile_t low_rate_profile(void) {
    hu_tapback_profile_t tp;
    memset(&tp, 0, sizeof(tp));
    tp.found = true;
    tp.level = "bucket";
    tp.rate = 0.02f;
    tp.n = 50;
    tp.cutoff_found = true;
    tp.cutoff = 0.10f;
    tp.cells = 8;
    return tp;
}

static void director_v2_call_learned_data_overrides_model_tapback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_ctx_t m = {.reply = "action:tapback|reaction:heart"};
    hu_provider_t p = {.ctx = &m, .vtable = &mock_vtable};
    hu_channel_history_entry_t e[3];
    fill_entries(e, 3, 10);
    hu_tapback_profile_t tp = low_rate_profile();
    hu_director_result_t r;
    hu_tapback_src_t src = HU_TAPBACK_SRC_NODATA;
    size_t bytes = 0;
    const char msg[] = "Dad and I both got covid. Feeling pretty rough. How are you doing";
    HU_ASSERT_TRUE(hu_director_v2_call(&alloc, &p, S("m"), NULL, e, 3, msg, sizeof(msg) - 1, NULL,
                                       &tp, "How Seth reacts: 2%.", &r, &src, &bytes));
    HU_ASSERT_EQ(atomic_load(&m.calls), 1);
    HU_ASSERT_EQ((int)src, (int)HU_TAPBACK_SRC_LEARNED);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
    HU_ASSERT_EQ(bytes, m.sys_len + m.user_len);
    HU_ASSERT_STR_CONTAINS(m.user, "How Seth reacts: 2%.");
    HU_ASSERT_STR_CONTAINS(m.user, "Them: msg00 ");
    HU_ASSERT_STR_CONTAINS(m.user, "Seth: msg01 ");
}

/* No learned data: the same covid message, the same model tapback, kept. No
 * word list stands in for the missing data. */
static void director_v2_call_without_data_trusts_the_model(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_ctx_t m = {.reply = "action:tapback|reaction:heart"};
    hu_provider_t p = {.ctx = &m, .vtable = &mock_vtable};
    hu_director_result_t r;
    hu_tapback_src_t src = HU_TAPBACK_SRC_LEARNED;
    const char msg[] = "Seth your AI is messed up?";
    HU_ASSERT_TRUE(hu_director_v2_call(&alloc, &p, S("m"), NULL, NULL, 0, msg, sizeof(msg) - 1,
                                       NULL, NULL, NULL, &r, &src, NULL));
    HU_ASSERT_EQ((int)src, (int)HU_TAPBACK_SRC_NODATA);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)r.reaction, (int)HU_REACTION_HEART);
}

/* The decide tests swap the daemon's classify provider for the mock. */
typedef struct {
    hu_provider_t prov;
    bool ok;
} saved_provider_t;

static saved_provider_t install_mock(mock_ctx_t *m) {
    saved_provider_t s = {g_classify_provider, g_classify_provider_ok};
    g_classify_provider.ctx = m;
    g_classify_provider.vtable = &mock_vtable;
    g_classify_provider_ok = true;
    hu_provider_t w = {.ctx = m, .vtable = &mock_vtable}; /* the shadow worker's own */
    hu_director_v2_set_worker_provider(&w);
    return s;
}

static void restore_provider(saved_provider_t s) {
    g_classify_provider = s.prov;
    g_classify_provider_ok = s.ok;
    hu_director_v2_set_worker_provider(NULL);
    unsetenv("HU_DIRECTOR_V2");
}

/* Runs decide under `gate` and v1 alone on the same input. */
static void run_both_as(hu_agent_t *agent, const char *gate, mock_ctx_t *m, const char *msg,
                        hu_director_result_t *got, hu_director_result_t *v1, bool *got_ok) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t e[4];
    fill_entries(e, 4, 12);
    saved_provider_t s = install_mock(m);
    if (gate)
        setenv("HU_DIRECTOR_V2", gate, 1);
    else
        unsetenv("HU_DIRECTOR_V2");
    memset(got, 0xA5, sizeof(*got));
    memset(v1, 0xA5, sizeof(*v1));
    *got_ok = hu_director_v2_decide(&alloc, agent, NULL, S("+15550000000"), msg, strlen(msg), e, 4,
                                    NULL, got);
    (void)hu_daemon_director_call(&alloc, msg, strlen(msg), e, 4, NULL, v1);
    HU_ASSERT_TRUE(hu_director_v2_shadow_drain(5000)); /* no worker outlives the mock */
    restore_provider(s);
}

static void run_both(const char *gate, mock_ctx_t *m, const char *msg, hu_director_result_t *got,
                     hu_director_result_t *v1, bool *got_ok) {
    run_both_as(NULL, gate, m, msg, got, v1, got_ok);
}

static void director_v2_off_is_byte_identical_to_v1(void) {
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both(NULL, &m, "ok", &got, &v1, &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
    HU_ASSERT_EQ(atomic_load(&m.calls), 0); /* v2 never ran */
    run_both("off", &m, "what time is dinner", &got, &v1, &ok);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
    HU_ASSERT_EQ(atomic_load(&m.calls), 0);
}

/* SHADOW on a loopback director: v1 decides, v2 runs later on a worker. */
static void director_v2_shadow_computes_v2_but_keeps_v1_decision(void) {
    /* v1's test stub tapbacks "ok"; the mock v2 answers with text. */
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both_as(agent_with_director_at("http://127.0.0.1:8741/v1"), "shadow", &m, "ok", &got, &v1,
                &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(atomic_load(&m.calls), 1); /* v2 was computed */
    HU_ASSERT_EQ((int)v1.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0); /* ...and v1 still decides */
}

/* The reply path never waits on v2: the director provider is held for up to
 * 3 s, and decide still returns v1's decision at once. */
static void director_v2_shadow_adds_no_latency_to_the_decision(void) {
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    atomic_store(&m.block, true);
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t e[4];
    fill_entries(e, 4, 12);
    saved_provider_t s = install_mock(&m);
    setenv("HU_DIRECTOR_V2", "shadow", 1);
    hu_director_result_t got, v1;
    long t0 = now_ms();
    bool ok = hu_director_v2_decide(&alloc, agent_with_director_at("http://localhost:8741"), NULL,
                                    S("+15550000000"), S("ok"), e, 4, NULL, &got);
    long elapsed = now_ms() - t0;
    bool released_before_return = !atomic_load(&m.block);
    atomic_store(&m.block, false);
    bool drained = hu_director_v2_shadow_drain(5000);
    (void)hu_daemon_director_call(&alloc, S("ok"), e, 4, NULL, &v1);
    restore_provider(s);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_FALSE(released_before_return);
    HU_ASSERT_LT(elapsed, 1000L); /* inline v2 would take the full 3 s hold */
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
    HU_ASSERT_TRUE(drained);
    HU_ASSERT_EQ(atomic_load(&m.calls), 1); /* ...and v2 did run, off the path */
}

/* Until #587 moves the director local, a cloud director never sees the
 * thread for a thrown-away shadow result. */
static void director_v2_shadow_skips_a_nonlocal_director(void) {
    static const char *const urls[] = {NULL, "https://us-central1-aiplatform.googleapis.com",
                                       "http://localhost.example.com"};
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        mock_ctx_t m = {.reply = "action:text|direction:engage fully"};
        hu_director_result_t got, v1;
        bool ok = false;
        run_both_as(agent_with_director_at(urls[i]), "shadow", &m, "ok", &got, &v1, &ok);
        HU_ASSERT_TRUE(ok);
        HU_ASSERT_EQ(atomic_load(&m.calls), 0);
        HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
    }
    mock_ctx_t m = {.reply = "action:text|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both("shadow", &m, "ok", &got, &v1, &ok); /* no agent, no config: unknown */
    HU_ASSERT_EQ(atomic_load(&m.calls), 0);
}

/* Real host parsing: a prefix match let "http://127.0.0.1:8080@evil.com/"
 * through, and its host is evil.com. */
static void compatible_url_is_loopback_parses_the_host(void) {
    static const char *const local[] = {
        "http://127.0.0.1:8741/v1", "http://localhost",     "https://localhost:443/x",
        "HTTP://LOCALHOST/",        "http://127.255.0.9:1", "http://127.0.0.1",
        "http://[::1]:8741/v1",     "http://[::1]",         "http://user@127.0.0.1/v1",
    };
    static const char *const remote[] = {
        "http://127.0.0.1:8080@evil.com/",
        "http://localhost@evil.com",
        "http://127.0.0.1.evil.com/",
        "http://localhost.evil.com",
        "http://127.0.0.256/",
        "http://127.0.0/",
        "http://127.0.0.1.1/",
        "http://1270.0.0.1/",
        "http://0127.0.0.1/",
        "127.0.0.1:8741",
        "ftp://127.0.0.1/",
        "http://[::1].evil.com/",
        "http://[::2]/",
        "http://127.0.0.1:80x/",
        "http://127.0.0.1:/",
        "https://aiplatform.googleapis.com/v1",
        "http://evil.com/?q=127.0.0.1",
        "http://evil.com#@127.0.0.1",
        "http://128.0.0.1/",
        "",
    };
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++)
        HU_ASSERT_TRUE(hu_compatible_url_is_loopback(local[i], strlen(local[i])));
    for (size_t i = 0; i < sizeof(remote) / sizeof(remote[0]); i++)
        HU_ASSERT_FALSE(hu_compatible_url_is_loopback(remote[i], strlen(remote[i])));
    HU_ASSERT_FALSE(hu_compatible_url_is_loopback(NULL, 4));
}

/* The worker's provider is its own: a plain compatible provider on the
 * primary's loopback endpoint, never the shared (reliable, fallback) one. */
static void director_v2_worker_provider_is_plain_and_local_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ((int)hu_director_v2_worker_provider_create(&alloc, "http://127.0.0.1:8741/v1", &p),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_STR_EQ(p.vtable->get_name(p.ctx), "compatible");
    p.vtable->deinit(p.ctx, &alloc);
    hu_provider_t q;
    memset(&q, 0, sizeof(q));
    HU_ASSERT_EQ((int)hu_director_v2_worker_provider_create(
                     &alloc, "https://aiplatform.googleapis.com/v1", &q),
                 (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(q.vtable);
    HU_ASSERT_EQ(
        (int)hu_director_v2_worker_provider_create(&alloc, "http://127.0.0.1:1@evil.com", &q),
        (int)HU_ERR_NOT_SUPPORTED);
}

static void director_v2_primary_endpoint_follows_the_config(void) {
    hu_agent_t *ag = agent_with_director_at("http://127.0.0.1:8741/v1");
    HU_ASSERT_STR_EQ(hu_director_v2_primary_endpoint(ag), "http://127.0.0.1:8741/v1");
    /* "reliable" wraps the primary named in reliability.primary_provider. */
    g_cfg.default_provider = "reliable";
    g_cfg.reliability.primary_provider = "mockdir";
    HU_ASSERT_STR_EQ(hu_director_v2_primary_endpoint(ag), "http://127.0.0.1:8741/v1");
    g_cfg.reliability.primary_provider = "other";
    HU_ASSERT_NULL(hu_director_v2_primary_endpoint(ag));
    HU_ASSERT_NULL(hu_director_v2_primary_endpoint(NULL));
}

/* The shadow worker never calls the shared director provider or the agent's
 * provider, whatever they are: only its own. */
static void director_v2_shadow_worker_uses_only_its_own_provider(void) {
    mock_ctx_t shared = {.reply = "action:text|direction:x"};
    mock_ctx_t agents = {.reply = "action:text|direction:x"};
    mock_ctx_t own = {.reply = "action:text|direction:engage fully"};
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t e[4];
    fill_entries(e, 4, 12);
    saved_provider_t s = install_mock(&shared);
    hu_provider_t w = {.ctx = &own, .vtable = &mock_vtable};
    hu_director_v2_set_worker_provider(&w);
    hu_agent_t *ag = agent_with_director_at("http://127.0.0.1:8741/v1");
    ag->provider.ctx = &agents;
    ag->provider.vtable = &mock_vtable;
    setenv("HU_DIRECTOR_V2", "shadow", 1);
    hu_director_result_t got;
    bool ok = hu_director_v2_decide(&alloc, ag, NULL, S("+15550000000"), S("ok"), e, 4, NULL, &got);
    bool drained = hu_director_v2_shadow_drain(5000);
    restore_provider(s);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(drained);
    HU_ASSERT_EQ(atomic_load(&own.calls), 1);
    HU_ASSERT_EQ(atomic_load(&shared.calls), 0);
    HU_ASSERT_EQ(atomic_load(&agents.calls), 0);
}

/* Shutdown waits for a running worker before its inputs go away, refuses new
 * jobs afterwards, and reports a worker it could not wait out. */
static void director_v2_shutdown_waits_for_the_worker(void) {
    mock_ctx_t m = {.reply = "action:text|direction:engage fully", .hold_ms = 300};
    atomic_store(&m.block, true);
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t e[4];
    fill_entries(e, 4, 12);
    saved_provider_t s = install_mock(&m);
    setenv("HU_DIRECTOR_V2", "shadow", 1);
    hu_agent_t *ag = agent_with_director_at("http://127.0.0.1:8741/v1");
    hu_director_result_t got;
    (void)hu_director_v2_decide(&alloc, ag, NULL, S("+15550000000"), S("ok"), e, 4, NULL, &got);
    bool finished_before = atomic_load(&m.finished);
    bool drained = hu_director_v2_shutdown(5000);
    bool finished_after = atomic_load(&m.finished);
    /* Closed: a new turn runs v1 only. */
    (void)hu_director_v2_decide(&alloc, ag, NULL, S("+15550000000"), S("ok"), e, 4, NULL, &got);
    bool idle = hu_director_v2_shadow_drain(1000);
    int calls = atomic_load(&m.calls);
    /* A worker that outlives the timeout is reported, not abandoned silently. */
    mock_ctx_t stuck = {.reply = "action:text|direction:x", .hold_ms = 3000};
    atomic_store(&stuck.block, true);
    hu_provider_t sw = {.ctx = &stuck, .vtable = &mock_vtable};
    hu_director_v2_set_worker_provider(&sw); /* reopens (test seam) */
    (void)hu_director_v2_decide(&alloc, ag, NULL, S("+15550000000"), S("ok"), e, 4, NULL, &got);
    for (int waited = 0; !atomic_load(&stuck.entered) && waited < 3000; waited += 5)
        sleep_ms(5);
    bool timed_out = !hu_director_v2_shutdown(50);
    atomic_store(&stuck.block, false);
    bool drained2 = hu_director_v2_shadow_drain(5000);
    restore_provider(s);
    HU_ASSERT_FALSE(finished_before);
    HU_ASSERT_TRUE(drained);
    HU_ASSERT_TRUE(finished_after);
    HU_ASSERT_TRUE(idle);
    HU_ASSERT_EQ(calls, 1); /* the post-shutdown turn started no worker */
    HU_ASSERT_TRUE(atomic_load(&stuck.entered));
    HU_ASSERT_TRUE(timed_out);
    HU_ASSERT_TRUE(drained2);
}

static void director_v2_live_uses_v2_decision(void) {
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both("live", &m, "ok", &got, &v1, &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(atomic_load(&m.calls), 1);
    HU_ASSERT_EQ((int)v1.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)got.action, (int)DIR_TEXT);
    HU_ASSERT_STR_EQ(got.direction, "engage fully");
}

static void director_v2_live_falls_back_to_v1_when_v2_fails(void) {
    mock_ctx_t m = {.fail = true};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both("live", &m, "ok", &got, &v1, &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(atomic_load(&m.calls), 1);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
}

/* Replays pin the gate: SHADOW runs as OFF (no shadow job), OFF and LIVE
 * pass through (the cut-over A/B arms set HU_DIRECTOR_V2=off|live). */
static void director_v2_replay_mode_pins_shadow_to_off(void) {
    unsetenv("HU_DIRECTOR_V2");
    HU_ASSERT_EQ((int)hu_director_v2_replay_mode(), (int)HU_GATE_OFF);
    setenv("HU_DIRECTOR_V2", "shadow", 1);
    HU_ASSERT_EQ((int)hu_gate_mode_from_env("HU_DIRECTOR_V2", HU_GATE_OFF), (int)HU_GATE_SHADOW);
    HU_ASSERT_EQ((int)hu_director_v2_replay_mode(), (int)HU_GATE_OFF);
    setenv("HU_DIRECTOR_V2", "live", 1);
    HU_ASSERT_EQ((int)hu_director_v2_replay_mode(), (int)HU_GATE_LIVE);
    setenv("HU_DIRECTOR_V2", "off", 1);
    HU_ASSERT_EQ((int)hu_director_v2_replay_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_DIRECTOR_V2");
}

void run_director_v2_tests(void) {
    HU_TEST_SUITE("director_v2");
    HU_RUN_TEST(director_v2_brevity_flags_the_prod_directions);
    HU_RUN_TEST(director_v2_brevity_passes_engaged_directions);
    HU_RUN_TEST(director_v2_system_prompt_has_no_length_instructions);
    HU_RUN_TEST(director_v2_system_prompt_forms_block_stays_in_budget);
    HU_RUN_TEST(director_v2_user_prompt_has_twelve_labelled_messages);
    HU_RUN_TEST(director_v2_user_prompt_stays_in_budget_with_long_messages);
    HU_RUN_TEST(director_v2_user_prompt_drops_oldest_history_first);
    HU_RUN_TEST(director_v2_call_learned_data_overrides_model_tapback);
    HU_RUN_TEST(director_v2_call_without_data_trusts_the_model);
    HU_RUN_TEST(director_v2_off_is_byte_identical_to_v1);
    HU_RUN_TEST(director_v2_shadow_computes_v2_but_keeps_v1_decision);
    HU_RUN_TEST(director_v2_shadow_adds_no_latency_to_the_decision);
    HU_RUN_TEST(director_v2_shadow_skips_a_nonlocal_director);
    HU_RUN_TEST(compatible_url_is_loopback_parses_the_host);
    HU_RUN_TEST(director_v2_worker_provider_is_plain_and_local_only);
    HU_RUN_TEST(director_v2_primary_endpoint_follows_the_config);
    HU_RUN_TEST(director_v2_shadow_worker_uses_only_its_own_provider);
    HU_RUN_TEST(director_v2_shutdown_waits_for_the_worker);
    HU_RUN_TEST(director_v2_live_uses_v2_decision);
    HU_RUN_TEST(director_v2_live_falls_back_to_v1_when_v2_fails);
    HU_RUN_TEST(director_v2_replay_mode_pins_shadow_to_off);
}
