/* tests/test_agent_stop_sequences.c — HU_STOP_SEQUENCES gate parity.
 *
 * Task 14 (2026-09-20 dead-code-plan wiring): src/agent/stop_sequence_registry.c
 * (hu_stop_sequence_registry_lookup) has held per-provider default stop
 * sequences since Task 19/20 of the 2026-05-14 output-validator-chain plan,
 * and src/providers/anthropic.c:382 (and every other provider) already sends
 * `request->stop_sequences` when the request carries them — but nothing ever
 * populated the field. This pins the contract for the shared helper
 * `hu_agent_internal_resolve_stop_sequences`, which agent_turn.c
 * (non-streaming) and agent_stream.c (streaming) BOTH call, immediately
 * after `hu_agent_internal_resolve_max_tokens` (Task 13) at each
 * request-build site — same "single shared helper, called from both sites"
 * shape as hu_agent_internal_apply_turn_request_overrides
 * (tests/test_agent_turn_request_overrides.c) and
 * hu_agent_internal_resolve_max_tokens (tests/test_agent_max_tokens_resolve.c).
 *
 * Gated OFF -> SHADOW -> LIVE via HU_STOP_SEQUENCES (hu_gate_mode_from_env),
 * default SHADOW: like Task 13, "leave stop_sequences at NULL/0 so the
 * provider sends none" already IS today's production behavior, so SHADOW
 * gives free visibility (throttled log) into what the resolver would set
 * before flipping ON.
 *
 * Signature note: the helper takes `provider_name` (a plain NUL-terminated
 * string, like every other use of that value at both call sites) and an
 * `agent` pointer for channel context (`agent->active_channel`/`_len`)
 * instead of four separate string+length pairs. agent_turn.c sits exactly
 * at the file-size ceiling ratchet (.claude/rules/file-size-ceiling.md), so
 * its call site must be a true one-line addition; folding the length
 * computation and channel plumbing into the helper keeps both call sites a
 * bare one-liner. Registry lookup itself stays provider-only
 * (include/human/agent/stop_sequence_registry.h is explicit: "Per-channel
 * stop sequences are not implemented; provider-only lookup") — `agent` is
 * used only for the SHADOW log-throttle key/message, never passed into the
 * registry call, and src/agent/ never interprets the channel string, same
 * as every other place agent->active_channel already flows through
 * agent_turn.c/agent_stream.c untouched (persona overlays, memory loader
 * context, etc.).
 *
 * Pinned here:
 *   AC-1  gate ON + empty req.stop_sequences + known provider "anthropic"
 *         -> registry's sequences written into req.stop_sequences/_count,
 *         return HU_STOP_SEQUENCES_RESOLVE_APPLIED
 *   AC-2  gate OFF + same inputs -> req.stop_sequences stays empty (no
 *         behavior change), return HU_STOP_SEQUENCES_RESOLVE_NOOP
 *   AC-3  gate SHADOW (documented default) + same inputs -> req.stop_sequences
 *         stays empty (log-only; see src/agent/agent.c for the hu_log_info
 *         call — no string-capture harness exists in this suite, same
 *         precedent as tests/test_agent_max_tokens_resolve.c's AC-3)
 *   AC-3b unset env is documented to default to SHADOW, not OFF or LIVE
 *   AC-3c pins the gate helper's own default resolution directly
 *         (hu_gate_mode_from_env), independent of our wrapper
 *   AC-4  a pre-set non-empty req.stop_sequences (some earlier request-shaping
 *         step) is NEVER overwritten, in ANY gate mode including LIVE —
 *         "fill when empty" is unconditional, not gate-dependent
 *   AC-5  unknown provider -> ON -> NOOP; the registry has no defaults for
 *         it and the helper must not write a bogus empty-but-non-NULL array
 *   AC-6  NULL req is a safe no-op, returns HU_STOP_SEQUENCES_RESOLVE_NOOP
 *   AC-6b helper is idempotent — a second ON call after the first APPLIED
 *         sees stop_sequences already populated and returns NOOP without
 *         changing the pointer/count
 *   AC-7  NULL/empty provider_name -> ON -> NOOP (registry returns count 0
 *         for a NULL provider; nothing to apply)
 *   AC-8  SHADOW log-throttle, keyed on (provider, channel): first SHADOW
 *         call for (anthropic, imessage) -> SHADOW_LOGGED; a second call for
 *         the SAME (provider, channel) pair -> SHADOW_THROTTLED; the first
 *         call for the SAME provider but a DIFFERENT channel (anthropic,
 *         telegram) -> SHADOW_LOGGED (the throttle key is the pair, not the
 *         provider alone — a per-channel gap in visibility would defeat the
 *         point of SHADOW). Also pins a NULL `agent` behaving like an empty
 *         channel ("(none)").
 *   AC-9  literal brief contract: gate ON -> request carries the registry's
 *         sequences for (anthropic, imessage); gate OFF -> empty.
 */
#include "human/agent.h"
#include "human/agent/stop_sequence_registry.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* Forward-declare the helper + its result enum + the test-only throttle
 * reset (tests/ isn't on src/agent/'s include path, same pattern as
 * test_agent_max_tokens_resolve.c). The contract is the source of truth —
 * see src/agent/agent_internal.h. */
typedef enum hu_stop_sequences_resolve_result {
    HU_STOP_SEQUENCES_RESOLVE_NOOP = 0,
    HU_STOP_SEQUENCES_RESOLVE_APPLIED,
    HU_STOP_SEQUENCES_RESOLVE_SHADOW_LOGGED,
    HU_STOP_SEQUENCES_RESOLVE_SHADOW_THROTTLED,
} hu_stop_sequences_resolve_result_t;

hu_stop_sequences_resolve_result_t
hu_agent_internal_resolve_stop_sequences(hu_chat_request_t *req, const char *provider_name,
                                         const hu_agent_t *agent);
void hu_agent_internal_resolve_stop_sequences_reset_for_test(void);

static void set_gate(const char *mode) {
    if (mode && mode[0])
        setenv("HU_STOP_SEQUENCES", mode, 1);
    else
        unsetenv("HU_STOP_SEQUENCES");
}

/* Builds a zeroed agent with only active_channel/_len set — no provider
 * vtable needed, since the helper never calls into agent->provider; it only
 * reads the plain active_channel field, same as agent_turn.c/agent_stream.c
 * do today for persona overlays etc. */
static void set_channel(hu_agent_t *agent, const char *channel, size_t channel_len) {
    memset(agent, 0, sizeof(*agent));
    agent->active_channel = channel;
    agent->active_channel_len = channel_len;
}

/* AC-1: ON fills an empty stop_sequences with the provider's registry
 * defaults and reports APPLIED. */
static void gate_on_fills_empty_stop_sequences_with_registry_value(void) {
    set_gate("on");
    hu_agent_t agent;
    set_channel(&agent, "imessage", 8);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    HU_ASSERT(req.stop_sequences == NULL);
    HU_ASSERT_EQ(req.stop_sequences_count, 0u);

    hu_stop_sequences_resolve_result_t result =
        hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);

    HU_ASSERT_EQ((int)result, (int)HU_STOP_SEQUENCES_RESOLVE_APPLIED);
    HU_ASSERT(req.stop_sequences != NULL);
    HU_ASSERT_TRUE(req.stop_sequences_count > 0);

    const char *const *expect_seqs = NULL;
    size_t expect_count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("anthropic", 9, &expect_seqs, &expect_count),
                 HU_OK);
    HU_ASSERT_EQ(req.stop_sequences_count, expect_count);
    HU_ASSERT(req.stop_sequences == expect_seqs);
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-2: OFF leaves stop_sequences empty — identical to not calling the
 * helper at all — and reports NOOP. */
static void gate_off_leaves_stop_sequences_empty(void) {
    set_gate("off");
    hu_agent_t agent;
    set_channel(&agent, "imessage", 8);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_stop_sequences_resolve_result_t result =
        hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);

    HU_ASSERT_EQ((int)result, (int)HU_STOP_SEQUENCES_RESOLVE_NOOP);
    HU_ASSERT(req.stop_sequences == NULL);
    HU_ASSERT_EQ(req.stop_sequences_count, 0u);
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-3: SHADOW computes-and-logs but never writes. */
static void gate_shadow_leaves_stop_sequences_empty(void) {
    set_gate("shadow");
    hu_agent_internal_resolve_stop_sequences_reset_for_test();
    hu_agent_t agent;
    set_channel(&agent, "shadow-empty-check", 18);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);

    HU_ASSERT(req.stop_sequences == NULL);
    HU_ASSERT_EQ(req.stop_sequences_count, 0u);
    hu_agent_internal_resolve_stop_sequences_reset_for_test();
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-3b: unset env is documented to default to SHADOW, not OFF. */
static void unset_env_defaults_to_shadow_not_off_or_live(void) {
    unsetenv("HU_STOP_SEQUENCES");
    hu_agent_internal_resolve_stop_sequences_reset_for_test();
    hu_agent_t agent;
    set_channel(&agent, "unset-env-check", 15);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);

    HU_ASSERT(req.stop_sequences == NULL);
    HU_ASSERT_EQ(req.stop_sequences_count, 0u);
    hu_agent_internal_resolve_stop_sequences_reset_for_test();
}

/* AC-3c: pin the gate helper's own default resolution directly. */
static void test_stop_sequences_gate_mode_default(void) {
    unsetenv("HU_STOP_SEQUENCES");
    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_STOP_SEQUENCES", HU_GATE_SHADOW);
    HU_ASSERT_EQ((int)mode, (int)HU_GATE_SHADOW);
}

/* AC-4: a pre-set non-empty stop_sequences must never be clobbered — in ANY
 * gate mode, including LIVE. */
static void preset_stop_sequences_are_never_overwritten(void) {
    static const char *const preset[] = {"###STOP###"};
    const char *modes[] = {"off", "shadow", "on"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        set_gate(modes[i]);
        hu_agent_t agent;
        set_channel(&agent, "preset-check", 12);
        hu_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.stop_sequences = preset;
        req.stop_sequences_count = 1;

        hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);

        HU_ASSERT(req.stop_sequences == preset);
        HU_ASSERT_EQ(req.stop_sequences_count, 1u);
    }
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-5: unknown provider -> ON -> NOOP; the registry has no defaults so
 * nothing should be written. */
static void gate_on_unknown_provider_is_noop(void) {
    set_gate("on");
    hu_agent_t agent;
    set_channel(&agent, "imessage", 8);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_stop_sequences_resolve_result_t result =
        hu_agent_internal_resolve_stop_sequences(&req, "totally-unknown-provider", &agent);

    HU_ASSERT_EQ((int)result, (int)HU_STOP_SEQUENCES_RESOLVE_NOOP);
    HU_ASSERT(req.stop_sequences == NULL);
    HU_ASSERT_EQ(req.stop_sequences_count, 0u);
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-6: NULL req is a safe no-op. */
static void null_req_is_safe_noop(void) {
    set_gate("on");
    hu_agent_t agent;
    set_channel(&agent, "imessage", 8);
    hu_stop_sequences_resolve_result_t result =
        hu_agent_internal_resolve_stop_sequences(NULL, "anthropic", &agent);
    HU_ASSERT_EQ((int)result, (int)HU_STOP_SEQUENCES_RESOLVE_NOOP);
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-6b: idempotency — a second ON call sees stop_sequences already
 * populated (by the first call) and reports NOOP without changing it. */
static void helper_is_idempotent(void) {
    set_gate("on");
    hu_agent_t agent;
    set_channel(&agent, "idempotent-check", 16);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_stop_sequences_resolve_result_t first =
        hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);
    HU_ASSERT_EQ((int)first, (int)HU_STOP_SEQUENCES_RESOLVE_APPLIED);
    const char *const *after_first = req.stop_sequences;
    size_t count_after_first = req.stop_sequences_count;

    hu_stop_sequences_resolve_result_t second =
        hu_agent_internal_resolve_stop_sequences(&req, "anthropic", &agent);

    HU_ASSERT_EQ((int)second, (int)HU_STOP_SEQUENCES_RESOLVE_NOOP);
    HU_ASSERT(req.stop_sequences == after_first);
    HU_ASSERT_EQ(req.stop_sequences_count, count_after_first);
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-7: NULL/empty provider_name -> ON -> NOOP. */
static void gate_on_null_provider_name_is_noop(void) {
    set_gate("on");
    hu_agent_t agent;
    set_channel(&agent, "imessage", 8);
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_resolve_stop_sequences(&req, NULL, &agent);

    HU_ASSERT(req.stop_sequences == NULL);
    HU_ASSERT_EQ(req.stop_sequences_count, 0u);
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-8: SHADOW log-throttle keyed on (provider, channel). Uses
 * provider/channel strings unique to this test plus an explicit reset, so
 * the result is deterministic regardless of what ran earlier in this
 * process — the throttle table is process-global static state, not reset
 * between tests by the framework. Also pins that a NULL `agent` (no channel
 * context) behaves like an empty channel, normalized to "(none)". */
static void shadow_log_throttled_per_distinct_provider_channel_pair(void) {
    set_gate("shadow");
    hu_agent_internal_resolve_stop_sequences_reset_for_test();

    hu_agent_t agent_imsg;
    set_channel(&agent_imsg, "task14-throttle-imsg", 20);
    hu_chat_request_t req1;
    memset(&req1, 0, sizeof(req1));
    hu_stop_sequences_resolve_result_t first =
        hu_agent_internal_resolve_stop_sequences(&req1, "anthropic", &agent_imsg);
    HU_ASSERT_EQ((int)first, (int)HU_STOP_SEQUENCES_RESOLVE_SHADOW_LOGGED);
    HU_ASSERT(req1.stop_sequences == NULL); /* SHADOW never writes, throttle or not */

    hu_chat_request_t req2;
    memset(&req2, 0, sizeof(req2));
    hu_stop_sequences_resolve_result_t second =
        hu_agent_internal_resolve_stop_sequences(&req2, "anthropic", &agent_imsg);
    HU_ASSERT_EQ((int)second, (int)HU_STOP_SEQUENCES_RESOLVE_SHADOW_THROTTLED);
    HU_ASSERT(req2.stop_sequences == NULL);

    /* Same provider, DIFFERENT channel — the (provider, channel) pair is a
     * distinct throttle key, so this must still log. */
    hu_agent_t agent_tg;
    set_channel(&agent_tg, "task14-throttle-telegram", 24);
    hu_chat_request_t req3;
    memset(&req3, 0, sizeof(req3));
    hu_stop_sequences_resolve_result_t third =
        hu_agent_internal_resolve_stop_sequences(&req3, "anthropic", &agent_tg);
    HU_ASSERT_EQ((int)third, (int)HU_STOP_SEQUENCES_RESOLVE_SHADOW_LOGGED);
    HU_ASSERT(req3.stop_sequences == NULL);

    /* NULL agent (no channel context) normalizes to "(none)" and still logs
     * on first sight of that key. */
    hu_chat_request_t req4;
    memset(&req4, 0, sizeof(req4));
    hu_stop_sequences_resolve_result_t fourth =
        hu_agent_internal_resolve_stop_sequences(&req4, "anthropic", NULL);
    HU_ASSERT_EQ((int)fourth, (int)HU_STOP_SEQUENCES_RESOLVE_SHADOW_LOGGED);
    HU_ASSERT(req4.stop_sequences == NULL);

    hu_agent_internal_resolve_stop_sequences_reset_for_test();
    unsetenv("HU_STOP_SEQUENCES");
}

/* AC-9: literal brief contract — gate ON -> request carries the registry's
 * sequences for (anthropic, imessage); gate OFF -> empty. */
static void brief_contract_on_carries_off_is_empty(void) {
    hu_agent_t agent;
    set_channel(&agent, "imessage", 8);

    set_gate("on");
    hu_chat_request_t req_on;
    memset(&req_on, 0, sizeof(req_on));
    hu_agent_internal_resolve_stop_sequences(&req_on, "anthropic", &agent);
    HU_ASSERT(req_on.stop_sequences != NULL);
    HU_ASSERT_TRUE(req_on.stop_sequences_count > 0);

    set_gate("off");
    hu_chat_request_t req_off;
    memset(&req_off, 0, sizeof(req_off));
    hu_agent_internal_resolve_stop_sequences(&req_off, "anthropic", &agent);
    HU_ASSERT(req_off.stop_sequences == NULL);
    HU_ASSERT_EQ(req_off.stop_sequences_count, 0u);
    unsetenv("HU_STOP_SEQUENCES");
}

void run_agent_stop_sequences_resolve_tests(void) {
    HU_TEST_SUITE("agent_stop_sequences_resolve");
    HU_RUN_TEST(gate_on_fills_empty_stop_sequences_with_registry_value);
    HU_RUN_TEST(gate_off_leaves_stop_sequences_empty);
    HU_RUN_TEST(gate_shadow_leaves_stop_sequences_empty);
    HU_RUN_TEST(unset_env_defaults_to_shadow_not_off_or_live);
    HU_RUN_TEST(test_stop_sequences_gate_mode_default);
    HU_RUN_TEST(preset_stop_sequences_are_never_overwritten);
    HU_RUN_TEST(gate_on_unknown_provider_is_noop);
    HU_RUN_TEST(null_req_is_safe_noop);
    HU_RUN_TEST(helper_is_idempotent);
    HU_RUN_TEST(gate_on_null_provider_name_is_noop);
    HU_RUN_TEST(shadow_log_throttled_per_distinct_provider_channel_pair);
    HU_RUN_TEST(brief_contract_on_carries_off_is_empty);
}
