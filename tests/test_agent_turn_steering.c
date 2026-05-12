/*
 * tests/test_agent_turn_steering.c — SOTA-2026 init-01 follow-up (c).
 *
 * Pins the wiring in `src/agent/agent_turn.c` that injects the persona
 * activation-steering directive on verifier-rejected retry attempts.
 *
 *   - first attempt is byte-identical to the pre-steering baseline
 *     (retry_attempt == 0 → no projection, no directive, no provider
 *     dispatch). This is the load-bearing determinism contract.
 *   - retry attempt 1+ projects persona + personal_model into the
 *     abstract trait-coefficient vector and renders a directive the
 *     prompt builder injects before the imperfect-delivery block.
 *   - cloud-shaped providers (NULL `apply_steering` vtable slot) get
 *     HU_ERR_NOT_SUPPORTED back from the dispatch helper — the turn
 *     MUST keep going on the prompt-side directive alone.
 *   - persona projections that fall below HU_STEERING_DIRECTIVE_FLOOR
 *     produce no directive even on retry (the threshold gate is in the
 *     directive renderer).
 *   - same persona + same retry attempt → byte-identical directive
 *     suffix across runs (no clock reads, no RNG perturb the projection
 *     when the personal_model is empty).
 *
 * The mock provider captures `msgs[0].content` (the system prompt) on
 * each `chat()` call so we can grep for "## Persona steering". Personas
 * are built on the same shape as `tests/test_persona_steering.c` uses,
 * so the projected vector clears the directive floor on retry.
 *
 * HU_IS_TEST gates the per-turn `hu_personal_model_ingest` in agent_turn,
 * so the user message never seeds the personal_model and the steering
 * vector stays freshness-independent — load-bearing for the determinism
 * test below.
 */

#include "human/agent.h"
#include "human/agent/reflection.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/persona/steering.h"
#include "human/provider.h"

#include "test_framework.h"

#include <stdbool.h>
#include <string.h>

/* ── Mock provider ──────────────────────────────────────────────── */

#define STEERING_TEST_MAX_CALLS 4
#define STEERING_TEST_PROMPT_BUF (16 * 1024)

typedef struct steering_turn_provider_ctx {
    int calls;
    int apply_steering_calls;
    char captured[STEERING_TEST_MAX_CALLS][STEERING_TEST_PROMPT_BUF];
    size_t captured_lens[STEERING_TEST_MAX_CALLS];
    bool has_steering[STEERING_TEST_MAX_CALLS];
    /* Per-call canned responses; NULL → fall back to default GOOD payload. */
    const char *responses[STEERING_TEST_MAX_CALLS];
} steering_turn_provider_ctx_t;

static const char *steering_turn_provider_name(void *ctx) {
    (void)ctx;
    return "steering_turn_mock";
}

static bool steering_turn_provider_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static void steering_turn_provider_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static void steering_capture_system_prompt(steering_turn_provider_ctx_t *ctx,
                                           const hu_chat_request_t *request, int idx) {
    if (idx < 0 || idx >= STEERING_TEST_MAX_CALLS)
        return;
    if (!request || request->messages_count == 0)
        return;
    if (request->messages[0].role != HU_ROLE_SYSTEM)
        return;
    const hu_chat_message_t *sys = &request->messages[0];
    size_t copy = sys->content_len < (sizeof(ctx->captured[idx]) - 1)
                      ? sys->content_len
                      : (sizeof(ctx->captured[idx]) - 1);
    if (sys->content && copy > 0)
        memcpy(ctx->captured[idx], sys->content, copy);
    ctx->captured[idx][copy] = '\0';
    ctx->captured_lens[idx] = copy;
    ctx->has_steering[idx] =
        strstr(ctx->captured[idx], "## Persona steering") != NULL;
}

static hu_error_t steering_turn_provider_chat(void *ctx, hu_allocator_t *alloc,
                                              const hu_chat_request_t *request,
                                              const char *model, size_t model_len,
                                              double temperature,
                                              hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    steering_turn_provider_ctx_t *r = (steering_turn_provider_ctx_t *)ctx;
    int idx = r->calls;
    r->calls++;
    steering_capture_system_prompt(r, request, idx);

    const char *text = NULL;
    if (idx >= 0 && idx < STEERING_TEST_MAX_CALLS && r->responses[idx])
        text = r->responses[idx];
    if (!text)
        text = "i had a wonderful day, thanks for asking — looking forward to more";

    out->content = hu_strndup(alloc, text, strlen(text));
    out->content_len = out->content ? strlen(text) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t steering_turn_provider_apply_steering(void *ctx, const float *vec,
                                                        size_t dim) {
    steering_turn_provider_ctx_t *r = (steering_turn_provider_ctx_t *)ctx;
    (void)vec;
    (void)dim;
    r->apply_steering_calls++;
    return HU_OK;
}

/* Cloud-shape: NO apply_steering slot. */
static const hu_provider_vtable_t steering_turn_cloud_vtable = {
    .chat = steering_turn_provider_chat,
    .supports_native_tools = steering_turn_provider_supports_native_tools,
    .get_name = steering_turn_provider_name,
    .deinit = steering_turn_provider_deinit,
    /* apply_steering intentionally NULL — mirrors every cloud provider
     * in the codebase today (openai, anthropic, gemini, …). */
};

/* On-device-shape: apply_steering implemented. Used to confirm the
 * provider-side dispatch fires alongside the prompt-side directive. */
static const hu_provider_vtable_t steering_turn_local_vtable = {
    .chat = steering_turn_provider_chat,
    .supports_native_tools = steering_turn_provider_supports_native_tools,
    .get_name = steering_turn_provider_name,
    .deinit = steering_turn_provider_deinit,
    .apply_steering = steering_turn_provider_apply_steering,
};

/* ── Persona builders ──────────────────────────────────────────── */

/* Persona that projects to a strong (above-floor) steering vector. Same
 * shape as `steering_build_warm_persona` in test_persona_steering.c so
 * the rendered directive header "## Persona steering" appears
 * deterministically on retry. */
static void steering_build_strong_persona(hu_persona_t *p, hu_persona_overlay_t *overlay) {
    memset(p, 0, sizeof(*p));
    memset(overlay, 0, sizeof(*overlay));
    p->emotional_range.ceiling = (char *)"warm and tender";
    p->humor.frequency = (char *)"frequent";
    p->conflict_style.confrontation_comfort = (char *)"direct";
    overlay->formality = (char *)"casual";
    p->overlays = overlay;
    p->overlays_count = 1;
}

/* Persona below the directive floor: leave fields empty so the projection
 * stays at 0.0 and no axis crosses HU_STEERING_DIRECTIVE_FLOOR. */
static void steering_build_weak_persona(hu_persona_t *p) {
    memset(p, 0, sizeof(*p));
}

/* ── Agent helpers ─────────────────────────────────────────────── */

static hu_error_t steering_init_test_agent(hu_agent_t *agent, hu_allocator_t *alloc,
                                           hu_provider_t provider) {
    return hu_agent_from_config(agent, alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                "test-model", 10, "steering_turn_mock", 18, 0.7, "/tmp", 4,
                                5, 50, false, 1, NULL, 0, NULL, 0, NULL);
}

/* Persona is heap-owned in production; for tests we point at a stack
 * persona and NULL it back out before deinit so hu_agent_deinit's
 * persona_deinit/free path is a no-op. */
static void steering_attach_test_persona(hu_agent_t *agent, hu_persona_t *persona) {
    agent->persona = persona;
}

static void steering_detach_test_persona(hu_agent_t *agent) {
    agent->persona = NULL;
}

/* Skip the second-opinion LLM critique on ACCEPTABLE responses. With it
 * left enabled, an extra chat() call would land between calls 1 and 2 and
 * scramble our captured-prompt index. The reflection retry path itself
 * (driven by the heuristic NEEDS_RETRY return for short responses) is
 * unaffected. */
static void steering_disable_llm_reflection(hu_agent_t *agent) {
    agent->reflection.use_llm = false;
}

/* ── Tests ─────────────────────────────────────────────────────── */

static void test_agent_turn_first_attempt_has_no_steering_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    steering_turn_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.responses[0] = "doing well today, thanks for asking — quiet morning so far";

    hu_provider_t provider = {.ctx = &pctx, .vtable = &steering_turn_local_vtable};
    hu_agent_t agent;
    HU_ASSERT_EQ(steering_init_test_agent(&agent, &alloc, provider), HU_OK);
    steering_disable_llm_reflection(&agent);

    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    steering_build_strong_persona(&persona, &overlay);
    steering_attach_test_persona(&agent, &persona);

    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = hu_agent_turn(&agent, "tell me about your day",
                                   strlen("tell me about your day"), &response,
                                   &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(pctx.calls, 1);
    /* Determinism contract: first attempt MUST be unsteered. */
    HU_ASSERT_FALSE(pctx.has_steering[0]);
    HU_ASSERT_STR_NOT_CONTAINS(pctx.captured[0], "## Persona steering");
    /* Critic H1: one apply_steering call expected — the entry-point
     * RESET with (NULL, 0). No retry happened, so no inject call. */
    HU_ASSERT_EQ(pctx.apply_steering_calls, 1);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    steering_detach_test_persona(&agent);
    hu_agent_deinit(&agent);
}

static void test_agent_turn_retry_attempt_1_injects_steering_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    steering_turn_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.responses[0] = "no";  /* < 10 chars → heuristic NEEDS_RETRY */
    pctx.responses[1] = "yes, doing great today, thanks for asking";

    hu_provider_t provider = {.ctx = &pctx, .vtable = &steering_turn_local_vtable};
    hu_agent_t agent;
    HU_ASSERT_EQ(steering_init_test_agent(&agent, &alloc, provider), HU_OK);
    steering_disable_llm_reflection(&agent);

    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    steering_build_strong_persona(&persona, &overlay);
    steering_attach_test_persona(&agent, &persona);

    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = hu_agent_turn(&agent, "tell me about your day",
                                   strlen("tell me about your day"), &response,
                                   &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(pctx.calls, 2);
    HU_ASSERT_FALSE(pctx.has_steering[0]);
    HU_ASSERT_TRUE(pctx.has_steering[1]);
    HU_ASSERT_STR_CONTAINS(pctx.captured[1], "## Persona steering");
    /* Provider-side activation steering also dispatched on retry. With
     * Critic H1 in effect, the entry-point reset adds one call before
     * the retry inject — so the expected total is 2 (reset + inject). */
    HU_ASSERT_EQ(pctx.apply_steering_calls, 2);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    steering_detach_test_persona(&agent);
    hu_agent_deinit(&agent);
}

static void test_agent_turn_retry_below_threshold_skips_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    steering_turn_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.responses[0] = "no";
    pctx.responses[1] = "yes, doing great today, thanks for asking";

    hu_provider_t provider = {.ctx = &pctx, .vtable = &steering_turn_local_vtable};
    hu_agent_t agent;
    HU_ASSERT_EQ(steering_init_test_agent(&agent, &alloc, provider), HU_OK);
    steering_disable_llm_reflection(&agent);

    hu_persona_t persona;
    steering_build_weak_persona(&persona);
    steering_attach_test_persona(&agent, &persona);

    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = hu_agent_turn(&agent, "tell me about your day",
                                   strlen("tell me about your day"), &response,
                                   &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(pctx.calls, 2);
    /* First attempt unsteered. */
    HU_ASSERT_FALSE(pctx.has_steering[0]);
    /* Retry attempt: directive renderer returns NULL because no axis
     * crosses HU_STEERING_DIRECTIVE_FLOOR — the prompt MUST stay clean
     * even though we're past the first attempt. */
    HU_ASSERT_FALSE(pctx.has_steering[1]);
    HU_ASSERT_STR_NOT_CONTAINS(pctx.captured[1], "## Persona steering");

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    steering_detach_test_persona(&agent);
    hu_agent_deinit(&agent);
}

static void
test_agent_turn_cloud_provider_apply_steering_returns_not_supported_does_not_fail_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    steering_turn_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.responses[0] = "no";
    pctx.responses[1] = "yes, doing great today, thanks for asking";

    /* Cloud-shaped vtable: apply_steering slot NULL → helper returns
     * HU_ERR_NOT_SUPPORTED. The turn must still complete cleanly and the
     * prompt-side directive still has to land for fallback parity. */
    hu_provider_t provider = {.ctx = &pctx, .vtable = &steering_turn_cloud_vtable};
    hu_agent_t agent;
    HU_ASSERT_EQ(steering_init_test_agent(&agent, &alloc, provider), HU_OK);
    steering_disable_llm_reflection(&agent);

    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    steering_build_strong_persona(&persona, &overlay);
    steering_attach_test_persona(&agent, &persona);

    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = hu_agent_turn(&agent, "tell me about your day",
                                   strlen("tell me about your day"), &response,
                                   &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_EQ(pctx.calls, 2);
    HU_ASSERT_FALSE(pctx.has_steering[0]);
    HU_ASSERT_TRUE(pctx.has_steering[1]);
    HU_ASSERT_STR_CONTAINS(pctx.captured[1], "## Persona steering");
    /* Cloud vtable has NULL apply_steering — the helper returned
     * HU_ERR_NOT_SUPPORTED before any provider code ran. */
    HU_ASSERT_EQ(pctx.apply_steering_calls, 0);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    steering_detach_test_persona(&agent);
    hu_agent_deinit(&agent);
}

static void test_agent_turn_retry_steering_is_deterministic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    steering_build_strong_persona(&persona, &overlay);

    /* Run twice with identical persona + (empty) personal_model. The
     * "## Persona steering" SUFFIX of the retry-attempt captured prompt
     * must be byte-identical across runs — that's the load-bearing
     * determinism contract for the steering boost-renderer pair. */
    char captured_runs[2][STEERING_TEST_PROMPT_BUF];
    size_t captured_run_lens[2] = {0, 0};

    for (int run = 0; run < 2; run++) {
        steering_turn_provider_ctx_t pctx;
        memset(&pctx, 0, sizeof(pctx));
        pctx.responses[0] = "no";
        pctx.responses[1] = "yes, doing great today, thanks for asking";

        hu_provider_t provider = {.ctx = &pctx, .vtable = &steering_turn_cloud_vtable};
        hu_agent_t agent;
        HU_ASSERT_EQ(steering_init_test_agent(&agent, &alloc, provider), HU_OK);
        steering_disable_llm_reflection(&agent);
        steering_attach_test_persona(&agent, &persona);

        char *response = NULL;
        size_t response_len = 0;
        hu_error_t err = hu_agent_turn(&agent, "tell me about your day",
                                       strlen("tell me about your day"), &response,
                                       &response_len);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_EQ(pctx.calls, 2);
        HU_ASSERT_TRUE(pctx.has_steering[1]);

        const char *suffix = strstr(pctx.captured[1], "## Persona steering");
        HU_ASSERT_NOT_NULL(suffix);
        size_t suffix_len = strlen(suffix);
        HU_ASSERT(suffix_len < sizeof(captured_runs[run]));
        memcpy(captured_runs[run], suffix, suffix_len);
        captured_runs[run][suffix_len] = '\0';
        captured_run_lens[run] = suffix_len;

        if (response)
            alloc.free(alloc.ctx, response, response_len + 1);
        steering_detach_test_persona(&agent);
        hu_agent_deinit(&agent);
    }

    HU_ASSERT_EQ(captured_run_lens[0], captured_run_lens[1]);
    HU_ASSERT_EQ(memcmp(captured_runs[0], captured_runs[1], captured_run_lens[0]), 0);
}

/* S1.5 critic H1 / CI-3: at the top of every `hu_agent_turn`, the agent
 * MUST call `hu_provider_apply_steering(provider, NULL, 0)` so any
 * steering vector left over from a previous turn's retry path is
 * cleared. Without this reset, the first (unsteered) attempt of turn
 * N inherits turn N-1's last-retry vector — silently violating the
 * "first attempt is byte-identical to baseline" contract on any
 * provider that implements `apply_steering`.
 *
 * The mock counts `apply_steering_calls`. A single turn that does NOT
 * retry must still increment the counter exactly once (the entry reset).
 * The provider helper swallows (NULL, 0) into a real vtable call, so
 * the mock sees it. */
static void test_agent_turn_resets_apply_steering_on_entry(void) {
    hu_allocator_t alloc = hu_system_allocator();
    steering_turn_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.responses[0] = "doing well today, thanks for asking — quiet morning so far";

    hu_provider_t provider = {.ctx = &pctx, .vtable = &steering_turn_local_vtable};
    hu_agent_t agent;
    HU_ASSERT_EQ(steering_init_test_agent(&agent, &alloc, provider), HU_OK);
    steering_disable_llm_reflection(&agent);

    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    steering_build_strong_persona(&persona, &overlay);
    steering_attach_test_persona(&agent, &persona);

    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = hu_agent_turn(&agent, "tell me about your day",
                                   strlen("tell me about your day"), &response,
                                   &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(pctx.calls, 1);
    /* First attempt unsteered (existing contract). */
    HU_ASSERT_FALSE(pctx.has_steering[0]);
    /* Critic H1 contract: even on a single-attempt turn, the entry-point
     * reset fired — apply_steering was called exactly once with (NULL, 0).
     * On a retry, this count would be 2 (entry reset + retry-1 inject). */
    HU_ASSERT_EQ(pctx.apply_steering_calls, 1);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    steering_detach_test_persona(&agent);
    hu_agent_deinit(&agent);
}

void run_agent_turn_steering_tests(void) {
    HU_TEST_SUITE("agent_turn_steering");
    HU_RUN_TEST(test_agent_turn_first_attempt_has_no_steering_directive);
    HU_RUN_TEST(test_agent_turn_retry_attempt_1_injects_steering_directive);
    HU_RUN_TEST(test_agent_turn_retry_below_threshold_skips_directive);
    HU_RUN_TEST(
        test_agent_turn_cloud_provider_apply_steering_returns_not_supported_does_not_fail_turn);
    HU_RUN_TEST(test_agent_turn_retry_steering_is_deterministic);
    HU_RUN_TEST(test_agent_turn_resets_apply_steering_on_entry);
}
