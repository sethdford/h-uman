/* Director v2 (HU_DIRECTOR_V2): intent not length, tapback-vs-text from
 * Seth's learned data, 12 messages of context. Hermetic: the provider is a
 * mock, no channel, no DB. */
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/daemon/common.h"
#include "human/daemon/director.h"
#include "human/daemon/director_v2.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    HU_ASSERT_STR_CONTAINS(buf, "really saying");
    HU_ASSERT_STR_CONTAINS(buf, "ask a follow-up");
    HU_ASSERT_STR_CONTAINS(buf, "share something of his own");
    HU_ASSERT_STR_CONTAINS(buf, "just react");
    HU_ASSERT_STR_CONTAINS(buf, "Never dodge");
    HU_ASSERT_STR_CONTAINS(buf, "How Seth reacts");
    /* No hand-written tapback rule list: the learned facts decide. */
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Never tapback");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "pure acknowledgement");
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

typedef struct {
    const char *reply;
    bool fail;
    int calls;
    size_t sys_len, user_len;
    char user[HU_DIRECTOR_V2_USER_CAP];
} mock_ctx_t;

static hu_error_t mock_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                        size_t sys_len, const char *msg, size_t msg_len,
                                        const char *model, size_t model_len, double temperature,
                                        char **out, size_t *out_len) {
    (void)sys;
    (void)model;
    (void)model_len;
    (void)temperature;
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    m->calls++;
    m->sys_len = sys_len;
    m->user_len = msg_len;
    size_t cp = msg_len < sizeof(m->user) - 1 ? msg_len : sizeof(m->user) - 1;
    memcpy(m->user, msg, cp);
    m->user[cp] = '\0';
    if (m->fail)
        return HU_ERR_INTERNAL;
    *out = hu_strndup(alloc, m->reply, strlen(m->reply));
    *out_len = *out ? strlen(m->reply) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t mock_vtable = {.chat_with_system = mock_chat_with_system};

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
    HU_ASSERT_EQ(m.calls, 1);
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
    return s;
}

static void restore_provider(saved_provider_t s) {
    g_classify_provider = s.prov;
    g_classify_provider_ok = s.ok;
    unsetenv("HU_DIRECTOR_V2");
}

/* Runs decide under `gate` and v1 alone on the same input. */
static void run_both(const char *gate, mock_ctx_t *m, const char *msg, hu_director_result_t *got,
                     hu_director_result_t *v1, bool *got_ok) {
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
    *got_ok = hu_director_v2_decide(&alloc, NULL, NULL, S("+15550000000"), msg, strlen(msg), e, 4,
                                    NULL, got);
    (void)hu_daemon_director_call(&alloc, msg, strlen(msg), e, 4, NULL, v1);
    restore_provider(s);
}

static void director_v2_off_is_byte_identical_to_v1(void) {
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both(NULL, &m, "ok", &got, &v1, &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
    HU_ASSERT_EQ(m.calls, 0); /* v2 never ran */
    run_both("off", &m, "what time is dinner", &got, &v1, &ok);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
    HU_ASSERT_EQ(m.calls, 0);
}

static void director_v2_shadow_computes_v2_but_keeps_v1_decision(void) {
    /* v1's test stub tapbacks "ok"; the mock v2 answers with text. */
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both("shadow", &m, "ok", &got, &v1, &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(m.calls, 1); /* v2 was computed */
    HU_ASSERT_EQ((int)v1.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0); /* ...and v1 still decides */
}

static void director_v2_live_uses_v2_decision(void) {
    mock_ctx_t m = {.reply = "action:text|delay_s:4|direction:engage fully"};
    hu_director_result_t got, v1;
    bool ok = false;
    run_both("live", &m, "ok", &got, &v1, &ok);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(m.calls, 1);
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
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(memcmp(&got, &v1, sizeof(got)), 0);
}

void run_director_v2_tests(void) {
    HU_TEST_SUITE("director_v2");
    HU_RUN_TEST(director_v2_brevity_flags_the_prod_directions);
    HU_RUN_TEST(director_v2_brevity_passes_engaged_directions);
    HU_RUN_TEST(director_v2_system_prompt_has_no_length_instructions);
    HU_RUN_TEST(director_v2_system_prompt_forms_block_stays_in_budget);
    HU_RUN_TEST(director_v2_user_prompt_has_twelve_labelled_messages);
    HU_RUN_TEST(director_v2_user_prompt_stays_in_budget_with_long_messages);
    HU_RUN_TEST(director_v2_call_learned_data_overrides_model_tapback);
    HU_RUN_TEST(director_v2_call_without_data_trusts_the_model);
    HU_RUN_TEST(director_v2_off_is_byte_identical_to_v1);
    HU_RUN_TEST(director_v2_shadow_computes_v2_but_keeps_v1_decision);
    HU_RUN_TEST(director_v2_live_uses_v2_decision);
    HU_RUN_TEST(director_v2_live_falls_back_to_v1_when_v2_fails);
}
