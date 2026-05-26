/* tests/test_agent_turn_request_overrides.c — Per-turn request overrides parity.
 *
 * G11 (2026-05-26): pins the contract for
 * `hu_agent_internal_apply_turn_request_overrides`, the shared helper that
 * agent_turn.c (non-streaming) and agent_stream.c (streaming) BOTH call to
 * apply per-turn request overrides set by the daemon dispatcher.
 *
 * The bug class this guards against (G5 = 2026-05-26 ../docs notes):
 * agent_stream.c declared `hu_chat_request_t req` with memset-zero, set
 * messages/model/tools/etc., and never assigned `req.thinking_budget`
 * from `agent->turn_thinking_budget`. Effect: every streaming turn ran
 * with `thinkingBudget=0` even when the daemon routed the turn to
 * ANALYTICAL / DEEP / EXPERT tier — degrading reasoning quality on every
 * Web-UI / streaming surface. Same F2-family root cause as
 * `gemini_3x_thinking_gotcha.md`.
 *
 * Without centralizing the override into a single helper called from
 * both call sites, the only test coverage was "trust the source code
 * review for parity drift". This file pins:
 *
 *   AC-1  positive turn_thinking_budget overrides req.thinking_budget
 *   AC-2  zero turn_thinking_budget leaves req.thinking_budget unchanged
 *   AC-3  negative turn_thinking_budget treated as "no override"
 *   AC-4  NULL agent is a safe no-op
 *   AC-5  NULL req is a safe no-op
 *   AC-6  helper is idempotent (calling twice produces same result)
 *   AC-7  helper does not clobber a pre-set req.thinking_budget when
 *         agent->turn_thinking_budget is zero (drift guard for callers
 *         that set the budget by another route, e.g. cognition_budget)
 *
 * Spec references: G5 fix (commit b40e9d7b) +
 *   ~/.claude/memory/.../gemini_3x_thinking_gotcha.md
 *   .claude/rules/security-predicate-extraction.md (the extraction pattern)
 */
#include "human/agent.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

/* Forward-declare the helper (tests/ isn't on src/agent/'s include path,
 * same as test_agent_turn_transport.c). The contract is the source of
 * truth — see src/agent/agent_internal.h. */
void hu_agent_internal_apply_turn_request_overrides(const hu_agent_t *agent,
                                                    hu_chat_request_t *req);

/* AC-1: positive turn_thinking_budget overrides the request's value.
 * This is the load-bearing path the G5 bug walked past — without it,
 * tier-routed thinking is silently disabled on every streaming turn. */
static void positive_turn_thinking_budget_overrides_request(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.turn_thinking_budget = 4096;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    /* Pre-condition: req starts with no thinking budget (the zero-init
     * default both call sites use before invoking the helper). */
    HU_ASSERT_EQ(req.thinking_budget, 0);

    hu_agent_internal_apply_turn_request_overrides(&agent, &req);

    HU_ASSERT_EQ(req.thinking_budget, 4096);
}

/* AC-2: zero turn_thinking_budget is the "no override" sentinel
 * (include/human/agent.h:297 "0 = no thinking config"). Helper must
 * NOT zero-out a request whose budget was set by another route. */
static void zero_turn_thinking_budget_leaves_request_unchanged(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.turn_thinking_budget = 0;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.thinking_budget = 2048; /* set by some other code path */

    hu_agent_internal_apply_turn_request_overrides(&agent, &req);

    HU_ASSERT_EQ(req.thinking_budget, 2048);
}

/* AC-3: negative turn_thinking_budget treated as "no override".
 * Defensive: the field is `int` and a caller could conceivably stage
 * -1 as a "clear" signal. The helper's `> 0` guard rejects that path. */
static void negative_turn_thinking_budget_is_not_applied(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.turn_thinking_budget = -1;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.thinking_budget = 512;

    hu_agent_internal_apply_turn_request_overrides(&agent, &req);

    HU_ASSERT_EQ(req.thinking_budget, 512);
}

/* AC-4: NULL agent → no-op. The helper sits in a hot path called once
 * per turn; making it NULL-safe means callers don't need defensive
 * null-checks of their own. */
static void null_agent_is_safe_noop(void) {
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.thinking_budget = 1024;

    hu_agent_internal_apply_turn_request_overrides(NULL, &req);

    HU_ASSERT_EQ(req.thinking_budget, 1024);
}

/* AC-5: NULL req → no-op. Symmetric NULL-safety contract. */
static void null_req_is_safe_noop(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.turn_thinking_budget = 8192;

    /* If this doesn't crash, the contract holds. */
    hu_agent_internal_apply_turn_request_overrides(&agent, NULL);
}

/* AC-6: helper is idempotent — calling twice with the same inputs
 * produces the same result. Guards against any future implementation
 * that might compound or decrement the budget. */
static void helper_is_idempotent(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.turn_thinking_budget = 1024;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));

    hu_agent_internal_apply_turn_request_overrides(&agent, &req);
    int after_first = req.thinking_budget;
    hu_agent_internal_apply_turn_request_overrides(&agent, &req);
    int after_second = req.thinking_budget;

    HU_ASSERT_EQ(after_first, 1024);
    HU_ASSERT_EQ(after_second, 1024);
    HU_ASSERT_EQ(after_first, after_second);
}

/* AC-7: helper does NOT clobber a pre-set req.thinking_budget when
 * agent->turn_thinking_budget is zero. agent_turn.c's cognition_budget
 * branch at line 4956-4957 raises thinking_budget to 2048 for planning
 * mode AFTER the helper runs — but other code paths could legitimately
 * pre-set the budget before calling. The helper must respect that. */
static void helper_does_not_clobber_preset_budget_when_no_override(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    /* turn_thinking_budget = 0 = no override */

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.thinking_budget = 16384; /* deep-tier pre-set by some caller */

    hu_agent_internal_apply_turn_request_overrides(&agent, &req);

    HU_ASSERT_EQ(req.thinking_budget, 16384);
}

void run_agent_turn_request_overrides_tests(void) {
    HU_TEST_SUITE("agent_turn_request_overrides");
    HU_RUN_TEST(positive_turn_thinking_budget_overrides_request);
    HU_RUN_TEST(zero_turn_thinking_budget_leaves_request_unchanged);
    HU_RUN_TEST(negative_turn_thinking_budget_is_not_applied);
    HU_RUN_TEST(null_agent_is_safe_noop);
    HU_RUN_TEST(null_req_is_safe_noop);
    HU_RUN_TEST(helper_is_idempotent);
    HU_RUN_TEST(helper_does_not_clobber_preset_budget_when_no_override);
}
