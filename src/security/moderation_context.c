/* src/security/moderation_context.c — HU_MODERATION_CONTEXT.
 *
 * See include/human/security/moderation_context.h for the gate semantics and
 * why a REGENERATE path is not used here. Decision table pattern follows
 * src/daemon/daemon_commitment_guard.c (detect() + a pure decision function);
 * local-provider resolution reuses hu_proposer_context_local_provider, the
 * same loopback-only resolver the commitment guard uses. */

#include "human/security/moderation_context.h"

#include "human/agent.h"
#include "human/core/gate_mode.h"
#include "human/core/llm_purpose.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon/proposer_context.h"
#include "human/security/moderation.h"
#include "human/security/self_harm.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_MOD_CTX_REPLY_MAX 1200 /* bytes of the reply sent to the judge */

static const char k_judge_system[] =
    "You judge ONE message, written in someone's own voice, that they are about to send to "
    "a friend or family member. Does it threaten, incite, or endorse real-world violence "
    "against an actual person — or is it idiom, hyperbole, sarcasm, talk about sports, games, "
    "movies, news, or a past event? Reply with exactly one word and nothing else: IDIOM or "
    "REAL.";

hu_gate_mode_t hu_moderation_context_mode(void) {
    return hu_gate_mode_from_env("HU_MODERATION_CONTEXT", HU_GATE_OFF);
}

bool hu_moderation_context_keep_reply(hu_gate_mode_t mode, hu_mod_ctx_verdict_t verdict) {
    return mode == HU_GATE_LIVE && verdict == HU_MOD_CTX_IDIOM;
}

/* Exactly one of "idiom" / "real" (whole word, case-insensitive) decides the
 * verdict; anything else (empty, both, neither) is unparseable -> ERROR. */
static hu_mod_ctx_verdict_t mod_parse_verdict(const char *raw, size_t raw_len) {
    if (!raw || raw_len == 0)
        return HU_MOD_CTX_ERROR;
    bool idiom = hu_str_contains_word_ci_n(raw, raw_len, "idiom");
    bool real = hu_str_contains_word_ci_n(raw, raw_len, "real");
    if (idiom && !real)
        return HU_MOD_CTX_IDIOM;
    if (real && !idiom)
        return HU_MOD_CTX_REAL;
    return HU_MOD_CTX_ERROR;
}

hu_mod_ctx_verdict_t hu_moderation_context_judge(const hu_provider_t *local, hu_allocator_t *alloc,
                                                 const char *model, size_t model_len,
                                                 const char *reply, size_t reply_len) {
    if (!local || !local->vtable || !local->vtable->chat_with_system || !alloc || !reply ||
        reply_len == 0)
        return HU_MOD_CTX_ERROR;
    size_t n = reply_len > HU_MOD_CTX_REPLY_MAX ? HU_MOD_CTX_REPLY_MAX : reply_len;
    char msg[HU_MOD_CTX_REPLY_MAX + 32];
    size_t pos = (size_t)snprintf(msg, sizeof(msg), "Message:\n%.*s", (int)n, reply);
    if (pos == 0 || pos >= sizeof(msg))
        return HU_MOD_CTX_ERROR;

    hu_llm_purpose_t prev = hu_llm_purpose_set_if_untagged(HU_LLM_PURPOSE_MODERATION_CHECK);
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = local->vtable->chat_with_system(local->ctx, alloc, k_judge_system,
                                                     sizeof(k_judge_system) - 1, msg, pos, model,
                                                     model_len, 0.0, &raw, &raw_len);
    (void)hu_llm_purpose_set(prev);
    if (err != HU_OK) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_len + 1);
        return HU_MOD_CTX_ERROR;
    }
    hu_mod_ctx_verdict_t v = mod_parse_verdict(raw, raw_len);
    alloc->free(alloc->ctx, raw, raw_len + 1);
    return v;
}

static const char *mod_ctx_verdict_name(hu_mod_ctx_verdict_t v) {
    return v == HU_MOD_CTX_IDIOM ? "idiom" : v == HU_MOD_CTX_REAL ? "real" : "error";
}

static int64_t mod_ctx_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef HU_IS_TEST
static hu_provider_t g_test_provider;
static bool g_test_provider_set;

void hu_moderation_context_set_test_provider(const hu_provider_t *p) {
    if (!p) {
        memset(&g_test_provider, 0, sizeof(g_test_provider));
        g_test_provider_set = false;
        return;
    }
    g_test_provider = *p;
    g_test_provider_set = true;
}
#endif

/* Runs the judge (when the gate is on) and returns whether the violence-only
 * hit should keep the reply. Logs the shadow/live aggregate line. SHADOW
 * never changes the return value (always false — today's behaviour). */
static bool mod_ctx_gate(hu_agent_t *agent, const char *reply, size_t reply_len) {
    hu_gate_mode_t mode = hu_moderation_context_mode();
    if (mode == HU_GATE_OFF)
        return false;

    hu_provider_t local = {0};
    bool have_local;
#ifdef HU_IS_TEST
    have_local = g_test_provider_set;
    if (have_local)
        local = g_test_provider;
#else
    have_local = agent && hu_proposer_context_local_provider(&agent->provider, &local);
#endif
    int64_t t0 = mod_ctx_mono_ms();
    hu_mod_ctx_verdict_t verdict =
        have_local ? hu_moderation_context_judge(&local, agent->alloc, agent->model_name,
                                                 agent->model_name_len, reply, reply_len)
                   : HU_MOD_CTX_ERROR;
    int64_t ms = mod_ctx_mono_ms() - t0;
    hu_log_info("moderation_context", NULL, "[moderation_context %s] verdict=%s ms=%lld",
                mode == HU_GATE_LIVE ? "live" : "shadow", mod_ctx_verdict_name(verdict),
                (long long)ms);
    return hu_moderation_context_keep_reply(mode, verdict);
}

bool hu_moderation_shield_apply(struct hu_agent *agent, const char *inbound, size_t inbound_len,
                                char **response, size_t *response_len) {
    if (!agent || !agent->alloc || !response || !*response || !response_len || *response_len == 0)
        return false;

    hu_moderation_result_t mod_result;
    memset(&mod_result, 0, sizeof(mod_result));
    if (hu_moderation_check(agent->alloc, *response, *response_len, &mod_result) != HU_OK ||
        !mod_result.flagged)
        return false;

    hu_log_info("agent_turn", NULL,
                "moderation flagged response: violence=%.2f self_harm=%.2f hate=%.2f",
                mod_result.violence_score, mod_result.self_harm_score, mod_result.hate_score);

    /* HU_MODERATION_CONTEXT: a violence-only hit gets a second, context-aware
     * opinion before the canned decline fires. Hate and self_harm are
     * untouched (owner ruling — see the header). */
    bool keep = (mod_result.violence && !mod_result.hate)
                    ? mod_ctx_gate(agent, *response, *response_len)
                    : false;

    if (!(mod_result.violence || mod_result.hate) || keep)
        return false;

    if (mod_result.violence && !keep)
        hu_log_warn("agent_turn", NULL,
                    "CRITICAL FIX 2026-05-26: violence flagged (score=%.2f); replacing unsafe "
                    "LLM output with safe canned response. Prior code path prepended the "
                    "internal [SAFETY] directive text TO the outgoing message, which sent the "
                    "directive verbatim to the recipient. Directive was intended for "
                    "system-prompt regenerate, not user-facing reply. Band-aid: replace with "
                    "safe decline; HU_MODERATION_CONTEXT=live keeps idiomatic hits unchanged.",
                    mod_result.violence_score);
    if (mod_result.hate)
        hu_log_info("agent_turn", NULL,
                    "hate speech flagged (score=%.2f); replacing with safe decline",
                    mod_result.hate_score);

    size_t safe_len = 0;
    const char *decline = hu_self_harm_decline_or_floor(
        inbound, inbound_len,
        mod_result.violence ? "rather not get into that one" : "i'm gonna pass on this one",
        &safe_len);
    char *safe = (char *)agent->alloc->alloc(agent->alloc->ctx, safe_len + 1);
    if (!safe)
        return false;
    memcpy(safe, decline, safe_len);
    safe[safe_len] = '\0';
    agent->alloc->free(agent->alloc->ctx, *response, *response_len + 1);
    *response = safe;
    *response_len = safe_len;
    return true;
}
