/* tests/test_moderation_context.c — HU_MODERATION_CONTEXT (SHIELD-004 fix).
 *
 * The bug: SHIELD-004 (src/agent/agent_turn.c, via hu_moderation_shield_apply
 * in src/security/moderation_context.c) replaces the twin's own reply with a
 * canned decline whenever the static keyword list in
 * src/security/moderation.c matches "kill" / "murder" / "violence" — with no
 * idiom exception, so "you killed it!" gets declined. These tests pin: OFF
 * is byte-identical to the pre-fix behaviour (bug still present when the gate
 * is off, by design); LIVE + an IDIOM verdict keeps the reply (the fix);
 * LIVE + REAL, or the judge being unavailable, still declines (fail closed);
 * hate/self_harm paths are untouched by the gate. */

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/core/llm_purpose.h"
#include "human/security/moderation_context.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

/* ── hu_moderation_context_keep_reply: the pure truth table ─────────────── */

static void keep_reply_true_only_for_live_and_idiom(void) {
    const hu_gate_mode_t modes[] = {HU_GATE_OFF, HU_GATE_SHADOW, HU_GATE_LIVE};
    const hu_mod_ctx_verdict_t verdicts[] = {HU_MOD_CTX_ERROR, HU_MOD_CTX_IDIOM, HU_MOD_CTX_REAL};
    for (size_t m = 0; m < 3; m++) {
        for (size_t v = 0; v < 3; v++) {
            bool want = modes[m] == HU_GATE_LIVE && verdicts[v] == HU_MOD_CTX_IDIOM;
            bool got = hu_moderation_context_keep_reply(modes[m], verdicts[v]);
            HU_ASSERT_EQ((int)got, (int)want);
        }
    }
}

/* ── hu_moderation_context_judge: the local-model call ───────────────────── */

typedef struct fake_llm {
    const char *reply; /* NULL = the call fails */
    hu_llm_purpose_t purpose_seen;
    unsigned calls;
} fake_llm_t;

static hu_error_t fake_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                            const char *msg, size_t msg_len, const char *model, size_t model_len,
                            double temperature, char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    fake_llm_t *f = (fake_llm_t *)ctx;
    f->calls++;
    f->purpose_seen = hu_llm_purpose_current();
    if (!f->reply)
        return HU_ERR_INTERNAL;
    size_t len = strlen(f->reply);
    *out = (char *)alloc->alloc(alloc->ctx, len + 1);
    memcpy(*out, f->reply, len + 1);
    *out_len = len;
    return HU_OK;
}

static const hu_provider_vtable_t fake_vtable = {.chat_with_system = fake_chat};

static void judge_null_local_is_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ((int)hu_moderation_context_judge(NULL, &alloc, "m", 1, "hi", 2), HU_MOD_CTX_ERROR);
}

static void judge_vtable_without_chat_with_system_is_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_vtable_t empty = {0};
    hu_provider_t p = {.ctx = NULL, .vtable = &empty};
    HU_ASSERT_EQ((int)hu_moderation_context_judge(&p, &alloc, "m", 1, "hi", 2), HU_MOD_CTX_ERROR);
}

static void judge_empty_reply_is_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fake_llm_t f = {.reply = "IDIOM"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    HU_ASSERT_EQ((int)hu_moderation_context_judge(&p, &alloc, "m", 1, "", 0), HU_MOD_CTX_ERROR);
    HU_ASSERT_EQ(f.calls, 0u); /* no call for an empty reply */
}

static void judge_idiom_reply_parses_idiom(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fake_llm_t f = {.reply = "IDIOM"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    HU_ASSERT_EQ((int)hu_moderation_context_judge(&p, &alloc, "m", 1, "you killed it tonight!", 23),
                 HU_MOD_CTX_IDIOM);
    HU_ASSERT_EQ(f.calls, 1u);
    HU_ASSERT_EQ((int)f.purpose_seen, (int)HU_LLM_PURPOSE_MODERATION_CHECK);
}

static void judge_real_reply_parses_real(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fake_llm_t f = {.reply = "REAL"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    HU_ASSERT_EQ((int)hu_moderation_context_judge(&p, &alloc, "m", 1, "i will kill you", 16),
                 HU_MOD_CTX_REAL);
}

static void judge_unparseable_reply_is_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fake_llm_t f = {.reply = "uh, not sure?"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    HU_ASSERT_EQ((int)hu_moderation_context_judge(&p, &alloc, "m", 1, "you killed it!", 15),
                 HU_MOD_CTX_ERROR);
}

static void judge_transport_failure_is_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fake_llm_t f = {.reply = NULL};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    HU_ASSERT_EQ((int)hu_moderation_context_judge(&p, &alloc, "m", 1, "you killed it!", 15),
                 HU_MOD_CTX_ERROR);
    HU_ASSERT_EQ(f.calls, 1u);
}

/* ── hu_moderation_shield_apply: SHIELD-004 glue (the headline tests) ────── */

static const char *k_env = "HU_MODERATION_CONTEXT";

static void set_env(const char *v) {
    if (v)
        setenv(k_env, v, 1);
    else
        unsetenv(k_env);
}

static char *dup_str(const char *s, size_t *len_out) {
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    memcpy(d, s, n + 1);
    *len_out = n;
    return d;
}

static void shield_off_declines_an_idiomatic_violence_hit(void) {
    set_env(NULL); /* unset -> OFF, the pre-fix (buggy) behaviour */
    hu_moderation_context_set_test_provider(NULL);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("you absolutely killed it at the show tonight!", &len);
    HU_ASSERT_TRUE(hu_moderation_shield_apply(&agent, "how was the show?", 18, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "rather not get into that one");
    alloc.free(alloc.ctx, resp, len + 1);
}

/* THE FIX: LIVE + the judge says IDIOM -> the reply is left exactly as the
 * model wrote it. Pre/post: before the call *response is the idiom reply;
 * after, it is byte-identical — the production symbol (hu_moderation_shield_
 * apply) changed its decision because of the judge's verdict, not a tautology. */
static void shield_live_idiom_keeps_the_reply_unchanged(void) {
    set_env("live");
    fake_llm_t f = {.reply = "IDIOM"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    hu_moderation_context_set_test_provider(&p);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("you absolutely killed it at the show tonight!", &len);
    const char *before = resp;
    HU_ASSERT_FALSE(hu_moderation_shield_apply(&agent, "how was the show?", 18, &resp, &len));
    HU_ASSERT_TRUE(resp == before); /* not reallocated */
    HU_ASSERT_STR_EQ(resp, "you absolutely killed it at the show tonight!");
    HU_ASSERT_EQ(f.calls, 1u);
    alloc.free(alloc.ctx, resp, len + 1);
    hu_moderation_context_set_test_provider(NULL);
}

/* A genuine threat still gets declined under LIVE — the judge's REAL verdict
 * does not unlock a send. */
static void shield_live_real_threat_still_declines(void) {
    set_env("live");
    fake_llm_t f = {.reply = "REAL"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    hu_moderation_context_set_test_provider(&p);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("i will kill you if you do that again", &len);
    HU_ASSERT_TRUE(hu_moderation_shield_apply(&agent, "sorry about that", 16, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "rather not get into that one");
    alloc.free(alloc.ctx, resp, len + 1);
    hu_moderation_context_set_test_provider(NULL);
}

/* Judge unavailable (no local provider) under LIVE -> fail closed, same as OFF. */
static void shield_live_judge_unavailable_fails_closed(void) {
    set_env("live");
    hu_moderation_context_set_test_provider(NULL);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("you killed it out there", &len);
    HU_ASSERT_TRUE(hu_moderation_shield_apply(&agent, "nm", 2, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "rather not get into that one");
    alloc.free(alloc.ctx, resp, len + 1);
}

/* SHADOW runs the judge but never changes the outcome. */
static void shield_shadow_runs_judge_but_keeps_declining(void) {
    set_env("shadow");
    fake_llm_t f = {.reply = "IDIOM"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    hu_moderation_context_set_test_provider(&p);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("you killed it out there", &len);
    HU_ASSERT_TRUE(hu_moderation_shield_apply(&agent, "nm", 2, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "rather not get into that one");
    HU_ASSERT_EQ(f.calls, 1u); /* the judge DID run, just didn't decide */
    alloc.free(alloc.ctx, resp, len + 1);
    hu_moderation_context_set_test_provider(NULL);
}

/* Hate hits are never routed to the context judge, LIVE or not. */
static void shield_live_hate_hit_bypasses_the_judge(void) {
    set_env("live");
    fake_llm_t f = {.reply = "IDIOM"};
    hu_provider_t p = {.ctx = &f, .vtable = &fake_vtable};
    hu_moderation_context_set_test_provider(&p);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("that is hate group rhetoric", &len);
    HU_ASSERT_TRUE(hu_moderation_shield_apply(&agent, "what do you think?", 19, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "i'm gonna pass on this one");
    HU_ASSERT_EQ(f.calls, 0u); /* judge never consulted for hate */
    alloc.free(alloc.ctx, resp, len + 1);
    hu_moderation_context_set_test_provider(NULL);
}

/* A clean reply is left alone regardless of mode. */
static void shield_clean_reply_is_never_touched(void) {
    set_env("live");
    hu_moderation_context_set_test_provider(NULL);
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    size_t len;
    char *resp = dup_str("sounds like a fun trip, what time do you land?", &len);
    const char *before = resp;
    HU_ASSERT_FALSE(hu_moderation_shield_apply(&agent, "heading to the airport", 23, &resp, &len));
    HU_ASSERT_TRUE(resp == before);
    alloc.free(alloc.ctx, resp, len + 1);
}

static void shield_null_args_are_safe(void) {
    HU_ASSERT_FALSE(hu_moderation_shield_apply(NULL, "x", 1, NULL, NULL));
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    HU_ASSERT_FALSE(hu_moderation_shield_apply(&agent, "x", 1, NULL, NULL));
}

void run_moderation_context_tests(void) {
    HU_TEST_SUITE("ModerationContext (HU_MODERATION_CONTEXT)");
    HU_RUN_TEST(keep_reply_true_only_for_live_and_idiom);

    HU_TEST_SUITE("ModerationContext judge");
    HU_RUN_TEST(judge_null_local_is_error);
    HU_RUN_TEST(judge_vtable_without_chat_with_system_is_error);
    HU_RUN_TEST(judge_empty_reply_is_error);
    HU_RUN_TEST(judge_idiom_reply_parses_idiom);
    HU_RUN_TEST(judge_real_reply_parses_real);
    HU_RUN_TEST(judge_unparseable_reply_is_error);
    HU_RUN_TEST(judge_transport_failure_is_error);

    HU_TEST_SUITE("ModerationContext SHIELD-004 glue");
    HU_RUN_TEST(shield_off_declines_an_idiomatic_violence_hit);
    HU_RUN_TEST(shield_live_idiom_keeps_the_reply_unchanged);
    HU_RUN_TEST(shield_live_real_threat_still_declines);
    HU_RUN_TEST(shield_live_judge_unavailable_fails_closed);
    HU_RUN_TEST(shield_shadow_runs_judge_but_keeps_declining);
    HU_RUN_TEST(shield_live_hate_hit_bypasses_the_judge);
    HU_RUN_TEST(shield_clean_reply_is_never_touched);
    HU_RUN_TEST(shield_null_args_are_safe);

    set_env(NULL);
}
