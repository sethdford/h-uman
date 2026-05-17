/* test_daemon_e2e_validator.c — End-to-end validator chain integration test.
 *
 * AC coverage (US-6):
 *   AC-6.1  Mock provider returns JORDAN_LEAK_F1 verbatim on the first (primary)
 *           call; test does NOT inline-reconstruct the validator chain — it
 *           exercises persona->outbound_chain (US-4 cached chain) via the
 *           production path in hu_agent_turn (src/agent/agent_turn.c:5585-5665).
 *   AC-6.2  The value returned by hu_agent_turn does NOT contain the
 *           JORDAN_LEAK_F1 payload; the validator chain intercepted it before
 *           it could reach the transport layer (the REJECT triggered a retry
 *           whose response was clean).
 *   AC-6.3  Deletion-check evidence is documented in the PR description.
 *           Manual experiment: comment out hu_output_validator_chain_execute at
 *           agent_turn.c:5605 → e2e_leak_blocked_by_validator_chain fails because
 *           the chain never runs, no REJECT fires, no retry occurs, and
 *           JORDAN_LEAK_F1 flows through unchecked to the response_out, causing
 *           the HU_ASSERT_NULL(strstr(response, F1_FRAGMENT_A)) assertion to fail.
 *   AC-6.4  Suite name: "daemon_e2e_validator".
 *
 * Production path exercised (primary test):
 *   hu_agent_turn (agent_turn.c:5585-5665)
 *     1. Reads persona->outbound_chain  [US-4 cached chain, built once at load]
 *     2. Calls hu_output_validator_chain_execute with the mock's JORDAN_LEAK_F1
 *     3. persona_narrator_validator REJECTs → triggers hu_response_guard_retry_slim
 *     4. Retry mock returns a clean reply → passes response_guard_check → recovered
 *     5. agent_turn returns the clean retry reply (no F1 fragments)
 *
 * The test does NOT touch daemon_stream_event_cb (guarded by #ifndef HU_IS_TEST).
 * That path is a distinct production arm (daemon bus delivery);
 * this test covers the agent_turn arm, which is the primary path exercised
 * when a message arrives via iMessage or any channel that drives agent_turn.
 *
 * US-10 compliance: this file references hu_output_validator_chain_execute,
 * a non-static symbol exported from src/agent/output_validator_chain.c.
 */

#include "human/agent.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

/* ── JORDAN_LEAK_F1 fixture ─────────────────────────────────────────────── */

static const char *JORDAN_LEAK_F1 =
    "Wait, looking at the history, the AI has been slipping into "
    "\"How can I help you today?\" which is a massive AI tell and "
    "explicitly forbidden by the persona instructions. I need to snap "
    "back into Seth.\n\n"
    "Seth is chill, playful, and romantic with Jordan.\n"
    "If she says \"Oh nice!\", he should probably keep it light or ask a follow-up";

/* Key substrings from F1 that must NOT appear in any outbound message. */
static const char *F1_FRAGMENT_A = "Wait, looking at the history";
static const char *F1_FRAGMENT_B = "Seth is chill";

/* In-character reply used as the retry response (clean, no validator violations). */
static const char *CLEAN_REPLY = "wait, you got the package already? that was fast";

/* ── Two-phase mock provider ─────────────────────────────────────────────
 * Call 0 (primary turn): returns JORDAN_LEAK_F1 — simulates a persona leak
 *   from the frontier model. The validator chain REJECTs this.
 * Call 1 (retry slim): returns CLEAN_REPLY — simulates the repair request
 *   succeeding. This passes response_guard_check, is returned as final content.
 *
 * Assertion: the final content does NOT contain F1 fragments.
 * If hu_output_validator_chain_execute were deleted from agent_turn.c, call 0
 * would not be rejected, no retry would fire, and JORDAN_LEAK_F1 would flow
 * through to response_out — causing the assertions below to FAIL (AC-6.3).
 * ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    uint32_t call_count; /* incremented on each chat() call */
} two_phase_ctx_t;

static hu_error_t two_phase_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                 const char *model, size_t model_len, double temperature,
                                 hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    two_phase_ctx_t *c = (two_phase_ctx_t *)ctx;
    const char *resp = (c->call_count == 0) ? JORDAN_LEAK_F1 : CLEAN_REPLY;
    c->call_count++;
    out->content = hu_strndup(alloc, resp, strlen(resp));
    out->content_len = out->content ? strlen(resp) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->usage.prompt_tokens = 10;
    out->usage.completion_tokens = 20;
    out->usage.total_tokens = 30;
    out->model = NULL;
    out->model_len = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t two_phase_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                             const char *system_prompt, size_t system_prompt_len,
                                             const char *message, size_t message_len,
                                             const char *model, size_t model_len,
                                             double temperature, char **out, size_t *out_len) {
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    two_phase_ctx_t *c = (two_phase_ctx_t *)ctx;
    const char *resp = (c->call_count == 0) ? JORDAN_LEAK_F1 : CLEAN_REPLY;
    c->call_count++;
    *out = hu_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static bool two_phase_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *two_phase_get_name(void *ctx) {
    return ((two_phase_ctx_t *)ctx)->name;
}
static void two_phase_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t two_phase_vtable = {
    .chat_with_system = two_phase_chat_with_system,
    .chat = two_phase_chat,
    .supports_native_tools = two_phase_supports_native_tools,
    .get_name = two_phase_get_name,
    .deinit = two_phase_deinit,
};

/* ── Persona JSON with name "Seth" (activates persona_narrator validator) ─── */

static const char SETH_PERSONA_JSON[] =
    "{"
    "  \"name\": \"Seth\","
    "  \"identity\": \"Seth Ford — a real person using h-uman to send messages.\","
    "  \"traits\": [\"chill\", \"playful\", \"direct\"],"
    "  \"communication_rules\": [\"never break character\", \"no AI phrases\"]"
    "}";

/* ── Helper: load Seth persona and attach to agent ───────────────────────── */

static hu_persona_t *setup_seth_persona(hu_allocator_t *alloc) {
    hu_persona_t *persona = (hu_persona_t *)alloc->alloc(alloc->ctx, sizeof(hu_persona_t));
    if (!persona)
        return NULL;
    memset(persona, 0, sizeof(hu_persona_t));
    hu_error_t perr =
        hu_persona_load_json(alloc, SETH_PERSONA_JSON, sizeof(SETH_PERSONA_JSON) - 1, persona);
    if (perr != HU_OK) {
        alloc->free(alloc->ctx, persona, sizeof(hu_persona_t));
        return NULL;
    }
    return persona;
}

static void attach_persona_to_agent(hu_agent_t *agent, hu_allocator_t *alloc,
                                    hu_persona_t *persona) {
    if (agent->persona) {
        hu_persona_deinit(alloc, agent->persona);
        alloc->free(alloc->ctx, agent->persona, sizeof(hu_persona_t));
    }
    agent->persona = persona; /* transfer ownership; hu_agent_deinit will free */
    agent->active_channel = "imessage";
    agent->active_channel_len = 8;
}

/* ── AC-6.1 sub-check: outbound_chain is cached on the persona ────────────
 *
 * Directly exercises hu_output_validator_chain_execute on the cached chain
 * to confirm it REJECTs JORDAN_LEAK_F1.  This is the production function
 * that agent_turn.c calls; calling it here satisfies the US-10 symbol
 * reference requirement. */

static void e2e_outbound_chain_is_cached_and_rejects_f1(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t perr =
        hu_persona_load_json(&alloc, SETH_PERSONA_JSON, sizeof(SETH_PERSONA_JSON) - 1, &p);
    HU_ASSERT_EQ(perr, HU_OK);

    /* AC-6.1: outbound_chain non-NULL after load (US-4 cache). */
    HU_ASSERT_NOT_NULL(p.outbound_chain);

    /* Chain must reject JORDAN_LEAK_F1 — confirms it is the real production
     * chain with persona_narrator validator keyed on "Seth". */
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    hu_validator_context_t vctx = {0};
    vctx.persona_name = "Seth";
    vctx.persona_name_len = 4;
    /* hu_output_validator_chain_execute is the production function also called
     * at agent_turn.c:5605; referencing it here satisfies US-10. */
    hu_error_t cerr = hu_output_validator_chain_execute(
        p.outbound_chain, &alloc, &vctx, JORDAN_LEAK_F1, strlen(JORDAN_LEAK_F1), &cr);
    HU_ASSERT_EQ(cerr, HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);
    hu_chain_result_free(&alloc, &cr);

    hu_persona_deinit(&alloc, &p);
}

/* ── AC-6.1 + AC-6.2: leak blocked E2E through agent_turn ──────────────────
 *
 * Flow:
 *   call 0 → JORDAN_LEAK_F1 → hu_output_validator_chain_execute → REJECT
 *   call 1 (retry slim) → CLEAN_REPLY → response_guard_check → HU_GUARD_OK → returned
 *   final response must NOT contain F1 fragments.
 *
 * AC-6.3 deletion proof: if the hu_output_validator_chain_execute call at
 * agent_turn.c:5605 is removed, the REJECT never fires, the retry never
 * occurs, and JORDAN_LEAK_F1 flows directly to response_out — the
 * HU_ASSERT_NULL assertions below FAIL. */

static void e2e_leak_blocked_by_validator_chain(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_persona_t *persona = setup_seth_persona(&alloc);
    HU_ASSERT_NOT_NULL(persona);
    /* AC-6.1: chain must be non-NULL (US-4 caching). */
    HU_ASSERT_NOT_NULL(persona->outbound_chain);

    two_phase_ctx_t mock_ctx = {.name = "two_phase", .call_count = 0};
    hu_provider_t prov = {.ctx = &mock_ctx, .vtable = &two_phase_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t aerr =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(aerr, HU_OK);

    attach_persona_to_agent(&agent, &alloc, persona);

    char *response = NULL;
    size_t response_len = 0;
    /* hu_agent_turn:
     *   1. provider.chat() → call 0 → JORDAN_LEAK_F1
     *   2. hu_output_validator_chain_execute(persona->outbound_chain, ...) → REJECT
     *   3. hu_response_guard_retry_slim → provider.chat() → call 1 → CLEAN_REPLY
     *   4. response_guard_check(CLEAN_REPLY) → HU_GUARD_OK → returned */
    hu_error_t terr = hu_agent_turn(&agent, "hey what's up", 13, &response, &response_len);

    HU_ASSERT_EQ(terr, HU_OK);
    HU_ASSERT_NOT_NULL(response);

    /* AC-6.2: response must NOT contain any fragment of JORDAN_LEAK_F1.
     * The validator chain intercepted the leak before it reached the transport. */
    HU_ASSERT_NULL(strstr(response, F1_FRAGMENT_A));
    HU_ASSERT_NULL(strstr(response, F1_FRAGMENT_B));

    /* The provider was called exactly twice: once for the primary turn,
     * once for the retry slim.  This confirms the chain REJECT fired. */
    HU_ASSERT_EQ((unsigned)mock_ctx.call_count, 2u);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent); /* frees agent.persona → hu_persona_deinit → destroys chain */
}

/* ── Sanity: clean reply passes through without modification ─────────────── */

typedef struct {
    const char *name;
} clean_ctx_t;

static hu_error_t clean_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                             const char *model, size_t model_len, double temperature,
                             hu_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    out->content = hu_strndup(alloc, CLEAN_REPLY, strlen(CLEAN_REPLY));
    out->content_len = out->content ? strlen(CLEAN_REPLY) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->usage.prompt_tokens = 5;
    out->usage.completion_tokens = 10;
    out->usage.total_tokens = 15;
    out->model = NULL;
    out->model_len = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t clean_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                         const char *system_prompt, size_t system_prompt_len,
                                         const char *message, size_t message_len, const char *model,
                                         size_t model_len, double temperature, char **out,
                                         size_t *out_len) {
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    *out = hu_strndup(alloc, CLEAN_REPLY, strlen(CLEAN_REPLY));
    *out_len = *out ? strlen(CLEAN_REPLY) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const char *clean_get_name(void *ctx) {
    return ((clean_ctx_t *)ctx)->name;
}
static void clean_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}
static bool clean_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const hu_provider_vtable_t clean_vtable = {
    .chat_with_system = clean_chat_with_system,
    .chat = clean_chat,
    .supports_native_tools = clean_supports_native_tools,
    .get_name = clean_get_name,
    .deinit = clean_deinit,
};

static void e2e_clean_reply_passes_through_validator_chain(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_persona_t *persona = setup_seth_persona(&alloc);
    HU_ASSERT_NOT_NULL(persona);
    HU_ASSERT_NOT_NULL(persona->outbound_chain);

    clean_ctx_t mock_ctx = {.name = "clean_mock"};
    hu_provider_t prov = {.ctx = &mock_ctx, .vtable = &clean_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t aerr =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(aerr, HU_OK);

    attach_persona_to_agent(&agent, &alloc, persona);

    char *response = NULL;
    size_t response_len = 0;
    hu_error_t terr = hu_agent_turn(&agent, "did it arrive?", 14, &response, &response_len);

    HU_ASSERT_EQ(terr, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    /* Clean reply must pass through the chain unchanged. */
    HU_ASSERT(response_len > 0);
    /* Verify the clean content actually arrived (not stripped/rejected). */
    HU_ASSERT_NOT_NULL(strstr(response, "package"));

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

/* ── Registration ────────────────────────────────────────────────────────── */

void run_daemon_e2e_validator_tests(void) {
    HU_TEST_SUITE("daemon_e2e_validator");
    HU_RUN_TEST(e2e_outbound_chain_is_cached_and_rejects_f1);
    HU_RUN_TEST(e2e_leak_blocked_by_validator_chain);
    HU_RUN_TEST(e2e_clean_reply_passes_through_validator_chain);
}
