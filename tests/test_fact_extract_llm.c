/* test_fact_extract_llm — coverage for the LLM-based fact extractor.
 *
 * Uses a recording stub provider that returns canned JSON responses.
 * Unlike test_persona_fidelity_judge.c (where eval_judge's HU_IS_TEST
 * bypass swallows the provider call), this extractor invokes
 * provider->chat_with_system DIRECTLY — so the stub actually runs and
 * we can verify both the parser and the prompt formatting.
 *
 * Coverage:
 *   - Happy path: well-formed {"facts":[...]} → facts populated
 *   - Bare array: [...] (no top-level object) accepted
 *   - Prose-wrapped JSON: "Here you go:\n{...}" parsed correctly
 *   - Markdown fence: "```json\n{...}\n```" parsed correctly
 *   - Empty array: HU_OK with zero facts (NOT an error)
 *   - Malformed JSON: HU_OK with zero facts (soft fail, regex fast-path covers)
 *   - Provider error: error propagates
 *   - NULL args: rejected
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/fact_extract_llm.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

/* ── Recording stub provider ───────────────────────────────────────── */

typedef struct rec_ctx {
    const char *canned;
    size_t canned_len;
    hu_error_t canned_err; /* if non-OK, returned without populating out */
    char *last_user_msg;
    int call_count;
} rec_ctx_t;

static char *dup_str(hu_allocator_t *a, const char *s, size_t n) {
    char *p = (char *)a->alloc(a->ctx, n + 1);
    if (!p)
        return NULL;
    if (s && n > 0)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static hu_error_t rec_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
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

static hu_error_t rec_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                       size_t sys_len, const char *msg, size_t msg_len,
                                       const char *model, size_t model_len, double temperature,
                                       char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    rec_ctx_t *c = (rec_ctx_t *)ctx;
    c->call_count++;
    if (c->last_user_msg) {
        alloc->free(alloc->ctx, c->last_user_msg, strlen(c->last_user_msg) + 1);
        c->last_user_msg = NULL;
    }
    c->last_user_msg = dup_str(alloc, msg, msg_len);
    if (c->canned_err != HU_OK) {
        *out = NULL;
        *out_len = 0;
        return c->canned_err;
    }
    *out = dup_str(alloc, c->canned, c->canned_len);
    *out_len = c->canned_len;
    return HU_OK;
}

static bool rec_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *rec_get_name(void *ctx) {
    (void)ctx;
    return "rec";
}

static void rec_deinit(void *ctx, hu_allocator_t *alloc) {
    rec_ctx_t *c = (rec_ctx_t *)ctx;
    if (!c)
        return;
    if (c->last_user_msg) {
        alloc->free(alloc->ctx, c->last_user_msg, strlen(c->last_user_msg) + 1);
        c->last_user_msg = NULL;
    }
}

static const hu_provider_vtable_t rec_vtable = {
    .chat = rec_chat,
    .chat_with_system = rec_chat_with_system,
    .supports_native_tools = rec_supports_native_tools,
    .get_name = rec_get_name,
    .deinit = rec_deinit,
};

static hu_provider_t mk_stub(rec_ctx_t *c, const char *canned, size_t canned_len, hu_error_t err) {
    memset(c, 0, sizeof(*c));
    c->canned = canned;
    c->canned_len = canned_len;
    c->canned_err = err;
    hu_provider_t p = {.ctx = c, .vtable = &rec_vtable};
    return p;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static void llm_extract_parses_well_formed_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned = "{\"facts\":["
                         " {\"subject\":\"user\",\"predicate\":\"likes\",\"object\":\"rock "
                         "climbing\",\"confidence\":0.9},"
                         " {\"subject\":\"user\",\"predicate\":\"lives_in\",\"object\":"
                         "\"Portland\",\"confidence\":0.85}"
                         "]}";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "Rock climbing is my passion.", 28,
                                     1700000000LL, &r),
                 HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 2L);
    HU_ASSERT_STR_EQ(r.facts[0].predicate, "likes");
    HU_ASSERT_STR_EQ(r.facts[0].object, "rock climbing");
    HU_ASSERT_TRUE(r.facts[0].confidence > 0.8f);
    HU_ASSERT_EQ((long)r.facts[0].last_seen_at, 1700000000L);
    HU_ASSERT_STR_EQ(r.facts[1].predicate, "lives_in");
    HU_ASSERT_EQ((long)r.propositional_count, 2L);
    HU_ASSERT_EQ(rc.call_count, 1);
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_accepts_bare_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned = "[{\"subject\":\"user\",\"predicate\":\"works_at\",\"object\":\"Acme\","
                         "\"confidence\":0.95}]";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(
        hu_fact_extract_llm(&alloc, &stub, "m", 1, "I work at Acme.", 15, 1700000000LL, &r), HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 1L);
    HU_ASSERT_STR_EQ(r.facts[0].object, "Acme");
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_strips_prose_preamble(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Real-world: Gemini sometimes prepends "Here you go:" */
    const char *canned =
        "Here you go:\n{\"facts\":[{\"subject\":\"user\",\"predicate\":\"likes\",\"object\":"
        "\"tea\",\"confidence\":0.8}]}\n";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "I enjoy tea.", 12, 1700000000LL, &r),
                 HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 1L);
    HU_ASSERT_STR_EQ(r.facts[0].object, "tea");
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_strips_markdown_fence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned =
        "```json\n{\"facts\":[{\"subject\":\"user\",\"predicate\":\"owns\",\"object\":\"a "
        "kayak\",\"confidence\":0.7}]}\n```";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "Got a kayak last weekend.", 25,
                                     1700000000LL, &r),
                 HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 1L);
    HU_ASSERT_STR_EQ(r.facts[0].predicate, "owns");
    HU_ASSERT_STR_EQ(r.facts[0].object, "a kayak");
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_empty_array_returns_zero_facts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned = "{\"facts\":[]}";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "ok", 2, 0, &r), HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 0L);
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_malformed_json_soft_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* No closing brace — invalid JSON */
    const char *canned = "{\"facts\": [{\"predicate\":\"likes\",\"object\":\"x\"";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "msg", 3, 0, &r), HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 0L);
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_provider_error_propagates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, "", 0, HU_ERR_PROVIDER_RATE_LIMITED);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "msg", 3, 0, &r),
                 HU_ERR_PROVIDER_RATE_LIMITED);
    stub.vtable->deinit(stub.ctx, &alloc);
}

static void llm_extract_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, "{}", 2, HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(NULL, &stub, "m", 1, "msg", 3, 0, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, NULL, "m", 1, "msg", 3, 0, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, NULL, 0, 0, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "msg", 3, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    stub.vtable->deinit(stub.ctx, &alloc);
}

/* Subject defaults to "user" when omitted in the JSON (common LLM
 * shorthand for personal facts). */
static void llm_extract_defaults_subject_to_user(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned = "{\"facts\":[{\"predicate\":\"hates\",\"object\":\"meetings\","
                         "\"confidence\":0.9}]}";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1, "Meetings drain me.", 18, 0, &r),
                 HU_OK);
    HU_ASSERT_EQ((long)r.fact_count, 1L);
    HU_ASSERT_STR_EQ(r.facts[0].subject, "user");
    stub.vtable->deinit(stub.ctx, &alloc);
}

/* Validates the prompt sent to the provider mentions the source text —
 * a regression here means the LLM is being asked to extract facts
 * from a template with no actual input. */
static void llm_extract_passes_source_text_into_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned = "{\"facts\":[]}";
    rec_ctx_t rc;
    hu_provider_t stub = mk_stub(&rc, canned, strlen(canned), HU_OK);
    hu_fact_extract_result_t r;
    HU_ASSERT_EQ(hu_fact_extract_llm(&alloc, &stub, "m", 1,
                                     "Rock climbing is my passion these days", 38, 0, &r),
                 HU_OK);
    HU_ASSERT_NOT_NULL(rc.last_user_msg);
    HU_ASSERT_TRUE(strstr(rc.last_user_msg, "Rock climbing is my passion") != NULL);
    stub.vtable->deinit(stub.ctx, &alloc);
}

void run_fact_extract_llm_tests(void) {
    HU_TEST_SUITE("fact_extract_llm");
    HU_RUN_TEST(llm_extract_parses_well_formed_object);
    HU_RUN_TEST(llm_extract_accepts_bare_array);
    HU_RUN_TEST(llm_extract_strips_prose_preamble);
    HU_RUN_TEST(llm_extract_strips_markdown_fence);
    HU_RUN_TEST(llm_extract_empty_array_returns_zero_facts);
    HU_RUN_TEST(llm_extract_malformed_json_soft_fails);
    HU_RUN_TEST(llm_extract_provider_error_propagates);
    HU_RUN_TEST(llm_extract_rejects_null_args);
    HU_RUN_TEST(llm_extract_defaults_subject_to_user);
    HU_RUN_TEST(llm_extract_passes_source_text_into_prompt);
}
