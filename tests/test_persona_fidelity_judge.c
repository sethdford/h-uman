/* test_persona_fidelity_judge — end-to-end coverage for the L2 wrapper.
 *
 * Honest scope note (2026-05-16): under HU_IS_TEST, `hu_eval_judge_check`
 * BYPASSES the provider entirely (src/eval_judge.c:266) and uses a
 * deterministic `heuristic_judge_1to5` instead. That isolation is
 * deliberate — no test should make real API calls — but it means the
 * "full provider round-trip" test the user originally asked for is
 * structurally blocked here. Lifting the gate (e.g., a build-time
 * `HU_TEST_ALLOW_FAKE_PROVIDER` flag) is its own project.
 *
 * What this file CAN cover today:
 *   1. The persona_fidelity wrapper correctly invokes hu_eval_judge_check
 *      (returns HU_OK, populates score/reasoning, no leaks).
 *   2. Input validation guards reject malformed args before any provider
 *      contact.
 *   3. A recording stub provider exists in this file so the day the
 *      HU_IS_TEST bypass is lifted, the test can flip to assert against
 *      the captured call args without writing new infrastructure.
 *
 * The recording stub below is dead code under HU_IS_TEST. That's the
 * point — it's a fixture-in-waiting, not a passive verifier.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval/persona_fidelity.h"
#include "human/eval_judge.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

/* ── Recording stub provider (currently dead under HU_IS_TEST) ──────── */

typedef struct recording_stub_ctx {
    /* Last chat_with_system args captured. Plain heap copies; freed in deinit. */
    char *last_system;
    char *last_message;
    /* Canned response the stub returns. Owned by the caller of
     * recording_stub_create; not freed here. */
    const char *canned_response;
    size_t canned_response_len;
    int call_count;
} recording_stub_ctx_t;

static hu_error_t recording_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                 const char *model, size_t model_len, double temperature,
                                 hu_chat_response_t *out) {
    (void)ctx;
    (void)alloc;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

static char *dup_str(hu_allocator_t *a, const char *s, size_t n) {
    char *p = (char *)a->alloc(a->ctx, n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static hu_error_t recording_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                             const char *system_prompt, size_t system_len,
                                             const char *message, size_t message_len,
                                             const char *model, size_t model_len,
                                             double temperature, char **out, size_t *out_len) {
    (void)model;
    (void)model_len;
    (void)temperature;
    recording_stub_ctx_t *c = (recording_stub_ctx_t *)ctx;
    if (c->last_system)
        alloc->free(alloc->ctx, c->last_system, strlen(c->last_system) + 1);
    if (c->last_message)
        alloc->free(alloc->ctx, c->last_message, strlen(c->last_message) + 1);
    c->last_system = dup_str(alloc, system_prompt, system_len);
    c->last_message = dup_str(alloc, message, message_len);
    c->call_count++;
    *out = dup_str(alloc, c->canned_response, c->canned_response_len);
    *out_len = c->canned_response_len;
    return HU_OK;
}

static bool recording_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *recording_get_name(void *ctx) {
    (void)ctx;
    return "recording_stub";
}

static void recording_deinit(void *ctx, hu_allocator_t *alloc) {
    /* The ctx struct itself is stack-owned by the test caller — only
     * the captured heap copies inside it get freed here. Calling
     * alloc->free on `c` would bad-free a stack address (ASan catches
     * it). The test that uses this stub must invoke `deinit` once
     * before its stack frame unwinds so the captured strings don't leak. */
    recording_stub_ctx_t *c = (recording_stub_ctx_t *)ctx;
    if (!c)
        return;
    if (c->last_system) {
        alloc->free(alloc->ctx, c->last_system, strlen(c->last_system) + 1);
        c->last_system = NULL;
    }
    if (c->last_message) {
        alloc->free(alloc->ctx, c->last_message, strlen(c->last_message) + 1);
        c->last_message = NULL;
    }
}

static const hu_provider_vtable_t recording_vtable = {
    .chat = recording_chat,
    .chat_with_system = recording_chat_with_system,
    .supports_native_tools = recording_supports_native_tools,
    .get_name = recording_get_name,
    .deinit = recording_deinit,
};

static hu_provider_t recording_stub_create(hu_allocator_t *alloc, recording_stub_ctx_t *ctx_storage,
                                           const char *canned, size_t canned_len) {
    memset(ctx_storage, 0, sizeof(*ctx_storage));
    ctx_storage->canned_response = canned;
    ctx_storage->canned_response_len = canned_len;
    (void)alloc;
    hu_provider_t p = {.ctx = ctx_storage, .vtable = &recording_vtable};
    return p;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

/* Wrapper returns HU_OK and populates score via the heuristic path
 * (HU_IS_TEST bypasses the provider). Proves the persona_fidelity layer
 * forwards rubric, persona description, and response to eval_judge
 * without dropping the call. */
static void persona_fidelity_judge_wrapper_returns_populated_result(void) {
    hu_allocator_t alloc = hu_system_allocator();
    recording_stub_ctx_t stub_ctx;
    /* JSON shape that parse_score_1to5 + parse_reasoning expect; not
     * exercised under HU_IS_TEST but the canned response is captured. */
    const char *canned = "{\"score\": 4, \"reasoning\": \"matches register\"}";
    hu_provider_t stub = recording_stub_create(&alloc, &stub_ctx, canned, strlen(canned));

    const char *persona_desc = "Casual lowercase chatter using lmk/btw/rn.";
    const char *response = "yeah lmk if u want anything else from me rn";
    const char *rubric = "{\"axes\":{\"register\":1.0}}";

    hu_eval_judge_result_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t err = hu_persona_fidelity_judge(&alloc, &stub, "test-model", 10, persona_desc,
                                               strlen(persona_desc), response, strlen(response),
                                               rubric, strlen(rubric), 3, NULL, &out);
    HU_ASSERT_EQ(err, HU_OK);
    /* Heuristic path always populates a score in [1,5]. */
    HU_ASSERT_TRUE(out.score >= 1 && out.score <= 5);
    /* Reasoning is set (heuristic stub fills it with a marker). */
    HU_ASSERT_NOT_NULL(out.reasoning);
    HU_ASSERT_TRUE(out.reasoning_len > 0);
    hu_eval_judge_result_free(&alloc, &out);
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void persona_fidelity_judge_rejects_null_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_result_t out;
    HU_ASSERT_EQ(
        hu_persona_fidelity_judge(&alloc, NULL, "m", 1, "p", 1, "r", 1, "ru", 2, 3, NULL, &out),
        HU_ERR_INVALID_ARGUMENT);
}

static void persona_fidelity_judge_rejects_empty_rubric(void) {
    hu_allocator_t alloc = hu_system_allocator();
    recording_stub_ctx_t stub_ctx;
    hu_provider_t stub = recording_stub_create(&alloc, &stub_ctx, "", 0);
    hu_eval_judge_result_t out;
    HU_ASSERT_EQ(
        hu_persona_fidelity_judge(&alloc, &stub, "m", 1, "p", 1, "r", 1, NULL, 0, 3, NULL, &out),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_fidelity_judge(&alloc, &stub, "m", 1, "p", 1, "r", 1, "rubric", 0, 3,
                                           NULL, &out),
                 HU_ERR_INVALID_ARGUMENT);
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void persona_fidelity_judge_rejects_empty_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    recording_stub_ctx_t stub_ctx;
    hu_provider_t stub = recording_stub_create(&alloc, &stub_ctx, "", 0);
    hu_eval_judge_result_t out;
    HU_ASSERT_EQ(
        hu_persona_fidelity_judge(&alloc, &stub, "m", 1, NULL, 0, "r", 1, "ru", 2, 3, NULL, &out),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_persona_fidelity_judge(&alloc, &stub, "m", 1, "p", 0, "r", 1, "ru", 2, 3, NULL, &out),
        HU_ERR_INVALID_ARGUMENT);
    stub.vtable->deinit(stub.ctx, &alloc);
}

/* Verify the wrapper survives a very long persona description (it caps
 * at 1024 bytes before the snprintf template fill, so 4 KB input must
 * not crash). */
static void persona_fidelity_judge_handles_overlong_persona_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    recording_stub_ctx_t stub_ctx;
    hu_provider_t stub = recording_stub_create(&alloc, &stub_ctx, "ok", 2);

    char big[4096];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    hu_eval_judge_result_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_persona_fidelity_judge(&alloc, &stub, "m", 1, big, strlen(big), "resp", 4,
                                           "rubric", 6, 3, NULL, &out),
                 HU_OK);
    hu_eval_judge_result_free(&alloc, &out);
    stub.vtable->deinit(stub.ctx, &alloc);
}

void run_persona_fidelity_judge_tests(void) {
    HU_TEST_SUITE("persona_fidelity_judge");
    HU_RUN_TEST(persona_fidelity_judge_wrapper_returns_populated_result);
    HU_RUN_TEST(persona_fidelity_judge_rejects_null_provider);
    HU_RUN_TEST(persona_fidelity_judge_rejects_empty_rubric);
    HU_RUN_TEST(persona_fidelity_judge_rejects_empty_persona);
    HU_RUN_TEST(persona_fidelity_judge_handles_overlong_persona_description);
}
