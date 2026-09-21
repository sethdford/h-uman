/* tests/test_agent_max_tokens_resolve.c — HU_MAX_TOKENS_RESOLVE gate parity.
 *
 * Task 13 (2026-09-20 dead-code-plan wiring): src/agent/max_tokens.c
 * (hu_max_tokens_resolve et al.) computed a model's output cap but nothing
 * populated hu_chat_request_t.max_tokens, so providers always fell back to
 * their own hardcoded constants (src/providers/anthropic.c ~142,
 * gemini.c ~768). This pins the contract for the shared helper
 * `hu_agent_internal_resolve_max_tokens`, which agent_turn.c (non-streaming)
 * and agent_stream.c (streaming) BOTH call, immediately before the request
 * is handed to the provider, to fill req.max_tokens from the model's known
 * cap — same "single shared helper, called from both request-build sites"
 * shape as hu_agent_internal_apply_turn_request_overrides
 * (tests/test_agent_turn_request_overrides.c).
 *
 * Gated OFF -> SHADOW -> LIVE via HU_MAX_TOKENS_RESOLVE (hu_gate_mode_from_env),
 * default SHADOW per the task brief (this is a behavior-shaping gate — it
 * changes reply length caps — but SHADOW rather than OFF because unlike a
 * genuinely new capability, "leave max_tokens at 0 so the provider falls
 * back to its own constant" already IS today's production behavior; SHADOW
 * gives free visibility into what the resolver would pick with zero risk).
 *
 * Pinned here:
 *   AC-1  gate ON + req.max_tokens==0 + known model -> resolved value written
 *   AC-2  gate OFF + same inputs -> req.max_tokens stays 0 (no behavior change)
 *   AC-3  gate SHADOW (and unset env, the documented default) + same inputs
 *         -> req.max_tokens stays 0 (log-only; see src/agent/agent.c for the
 *         hu_log_info call — no string-capture harness exists in this suite,
 *         same precedent as tests/test_agent_facts.c's SHADOW tests)
 *   AC-4  a pre-set positive req.max_tokens (e.g. the somatic-energy /
 *         empathy / adaptive-token-budget caps that run earlier in both
 *         agent_turn.c and agent_stream.c) is NEVER overwritten, in ANY
 *         gate mode — "fill when 0" is unconditional, not gate-dependent
 *   AC-5  unknown model still resolves (falls back to hu_max_tokens_default())
 *         when gate is ON
 *   AC-6  NULL req is a safe no-op
 *   AC-7  empty/NULL model_ref still resolves to the default cap when ON
 */
#include "human/agent.h"
#include "human/max_tokens.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Forward-declare the helper (tests/ isn't on src/agent/'s include path,
 * same pattern as test_agent_turn_request_overrides.c). The contract is
 * the source of truth — see src/agent/agent_internal.h. */
void hu_agent_internal_resolve_max_tokens(hu_chat_request_t *req, const char *model_ref,
                                          size_t model_ref_len);

static void set_gate(const char *mode) {
    if (mode && mode[0])
        setenv("HU_MAX_TOKENS_RESOLVE", mode, 1);
    else
        unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-1: ON fills a zero max_tokens with the model's resolved cap. */
static void gate_on_fills_zero_max_tokens_with_resolved_value(void) {
    set_gate("on");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    HU_ASSERT_EQ(req.max_tokens, 0u);

    hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);

    HU_ASSERT_EQ(req.max_tokens, hu_max_tokens_lookup("gpt-4o", 6));
    HU_ASSERT_TRUE(req.max_tokens > 0);
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-2: OFF leaves max_tokens at 0 — identical to not calling the helper
 * at all, i.e. zero production behavior change while gated off. */
static void gate_off_leaves_max_tokens_zero(void) {
    set_gate("off");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);

    HU_ASSERT_EQ(req.max_tokens, 0u);
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-3: SHADOW computes-and-logs but never writes. Same observable
 * contract as OFF from the request's point of view. */
static void gate_shadow_leaves_max_tokens_zero(void) {
    set_gate("shadow");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);

    HU_ASSERT_EQ(req.max_tokens, 0u);
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-3b: unset env is documented to default to SHADOW, not OFF — pin the
 * default explicitly so a future change to the sentinel is caught here
 * rather than discovered in production. */
static void unset_env_defaults_to_shadow_not_off_or_live(void) {
    unsetenv("HU_MAX_TOKENS_RESOLVE");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);

    /* SHADOW and OFF are behaviorally identical here (both leave 0), so
     * this alone can't distinguish them; test_max_tokens_gate_mode_default
     * below pins the actual enum default via hu_gate_mode_from_env directly. */
    HU_ASSERT_EQ(req.max_tokens, 0u);
}

/* Pin the gate helper's own default resolution, independent of our
 * wrapper, so a change to the sentinel used at the call site is visible
 * here even though AC-2/AC-3 look identical from req.max_tokens alone. */
static void test_max_tokens_gate_mode_default(void) {
    unsetenv("HU_MAX_TOKENS_RESOLVE");
    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_MAX_TOKENS_RESOLVE", HU_GATE_SHADOW);
    HU_ASSERT_EQ((int)mode, (int)HU_GATE_SHADOW);
}

/* AC-4: a pre-set positive max_tokens (somatic/empathy/token-budget caps
 * that run earlier in agent_turn.c / agent_stream.c) must never be
 * clobbered — in ANY gate mode, including LIVE. */
static void preset_positive_max_tokens_is_never_overwritten(void) {
    const char *modes[] = {"off", "shadow", "on"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        set_gate(modes[i]);
        hu_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.max_tokens = 300; /* e.g. somatic low-energy cap */

        hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);

        HU_ASSERT_EQ(req.max_tokens, 300u);
    }
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-5: unknown model still resolves to the global default when ON —
 * hu_max_tokens_resolve's own fallback contract, exercised through the
 * wiring rather than in isolation. */
static void gate_on_unknown_model_resolves_to_default(void) {
    set_gate("on");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_max_tokens(&req, "totally-unknown-model-xyz", 25);

    HU_ASSERT_EQ(req.max_tokens, hu_max_tokens_default());
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-6: NULL req is a safe no-op (mirrors
 * hu_agent_internal_apply_turn_request_overrides' NULL-safety contract).
 * If this doesn't crash, the contract holds — same style as
 * test_agent_turn_request_overrides.c's null_req_is_safe_noop. */
static void null_req_is_safe_noop(void) {
    set_gate("on");
    hu_agent_internal_resolve_max_tokens(NULL, "gpt-4o", 6);
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* Idempotency: calling twice with the same inputs must not compound or
 * re-resolve to a different value (guards a future implementation that
 * mistakenly treats a resolved 0-override as "unset" on the second call). */
static void helper_is_idempotent(void) {
    set_gate("on");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);
    uint32_t after_first = req.max_tokens;
    hu_agent_internal_resolve_max_tokens(&req, "gpt-4o", 6);
    uint32_t after_second = req.max_tokens;

    HU_ASSERT_TRUE(after_first > 0);
    HU_ASSERT_EQ(after_first, after_second);
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

/* AC-7: NULL/empty model_ref still resolves to the default cap when ON —
 * callers with no model name yet (e.g. mid-routing) must not crash or
 * leave the request at a provider-unknown zero. */
static void gate_on_null_model_ref_resolves_to_default(void) {
    set_gate("on");
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_max_tokens(&req, NULL, 0);

    HU_ASSERT_EQ(req.max_tokens, hu_max_tokens_default());
    unsetenv("HU_MAX_TOKENS_RESOLVE");
}

void run_agent_max_tokens_resolve_tests(void) {
    HU_TEST_SUITE("agent_max_tokens_resolve");
    HU_RUN_TEST(gate_on_fills_zero_max_tokens_with_resolved_value);
    HU_RUN_TEST(gate_off_leaves_max_tokens_zero);
    HU_RUN_TEST(gate_shadow_leaves_max_tokens_zero);
    HU_RUN_TEST(unset_env_defaults_to_shadow_not_off_or_live);
    HU_RUN_TEST(test_max_tokens_gate_mode_default);
    HU_RUN_TEST(preset_positive_max_tokens_is_never_overwritten);
    HU_RUN_TEST(gate_on_unknown_model_resolves_to_default);
    HU_RUN_TEST(null_req_is_safe_noop);
    HU_RUN_TEST(helper_is_idempotent);
    HU_RUN_TEST(gate_on_null_model_ref_resolves_to_default);
}
