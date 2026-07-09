/* test_personal_model_llm_extract — the LLM fact-extraction FALLBACK wired
 * into hu_personal_model_ingest.
 *
 * The regex fast-path (hu_fact_extract) matches ~43 first-person prefixes and
 * finds nothing in casual/indirect iMessage text ("Did you not get the
 * email?"). This is why the production graph held 67 relations over 1796
 * messages. The fix: when the regex pass returns ZERO facts and the message is
 * substantive, fall back to an injected LLM extractor (hu_fact_extract_llm),
 * gated OFF -> SHADOW -> LIVE by HU_LLM_FACT_EXTRACT.
 *
 * These tests pin the WIRING contract (not the extractor itself, which
 * test_fact_extract_llm.c already covers) using a recording mock provider:
 *   - LIVE + regex-empty + substantive  -> LLM called, fact merged
 *   - regex HIT                          -> LLM NOT called (short-circuit)
 *   - SHADOW                             -> LLM called, fact NOT merged
 *   - OFF / unset                        -> LLM NOT called
 *   - no provider injected               -> LLM NOT called
 *   - too-short message                  -> LLM NOT called
 *
 * References production symbol hu_personal_model_set_llm_extractor +
 * hu_personal_model_ingest.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* ── Recording mock provider (counts calls, returns canned JSON) ───────── */

typedef struct pm_rec_ctx {
    const char *canned;
    size_t canned_len;
    int call_count;
} pm_rec_ctx_t;

static char *pm_dup(hu_allocator_t *a, const char *s, size_t n) {
    char *p = (char *)a->alloc(a->ctx, n + 1);
    if (!p)
        return NULL;
    if (s && n > 0)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static hu_error_t pm_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
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

static hu_error_t pm_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                      size_t sys_len, const char *msg, size_t msg_len,
                                      const char *model, size_t model_len, double temperature,
                                      char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    pm_rec_ctx_t *c = (pm_rec_ctx_t *)ctx;
    c->call_count++;
    *out = pm_dup(alloc, c->canned, c->canned_len);
    *out_len = c->canned_len;
    return HU_OK;
}

static bool pm_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *pm_get_name(void *ctx) {
    (void)ctx;
    return "pm_rec";
}

static const hu_provider_vtable_t pm_vtable = {
    .chat = pm_chat,
    .chat_with_system = pm_chat_with_system,
    .supports_native_tools = pm_supports_native_tools,
    .get_name = pm_get_name,
};

/* Canned: one fact the regex extractor could NEVER produce from the casual
 * message below — proves the fact came from the LLM fallback. */
static const char *pm_canned_json =
    "{\"facts\":[{\"subject\":\"user\",\"predicate\":\"asked_about\","
    "\"object\":\"the email\",\"confidence\":0.8}]}";

static const char *pm_casual_msg =
    "Did you not get the email I sent yesterday?"; /* regex finds nothing */

/* Reset DI + env so each test is isolated (file-scope statics persist). */
static void pm_reset_fallback(void) {
    unsetenv("HU_LLM_FACT_EXTRACT");
    hu_personal_model_set_llm_extractor(NULL, NULL, NULL, 0);
}

static bool model_has_object(const hu_personal_model_t *m, const char *obj) {
    for (size_t i = 0; i < m->fact_count; i++) {
        if (strstr(m->facts[i].object, obj) != NULL)
            return true;
    }
    return false;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void llm_fallback_extracts_when_regex_empty_and_live(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pm_rec_ctx_t rc = {.canned = pm_canned_json, .canned_len = strlen(pm_canned_json), .call_count = 0};
    hu_provider_t prov = {.ctx = &rc, .vtable = &pm_vtable};

    pm_reset_fallback();
    setenv("HU_LLM_FACT_EXTRACT", "on", 1);
    hu_personal_model_set_llm_extractor(&alloc, &prov, "m", 1);

    hu_personal_model_t model;
    memset(&model, 0, sizeof(model));
    HU_ASSERT_EQ(hu_personal_model_ingest(&model, pm_casual_msg, strlen(pm_casual_msg), true, 1700000000LL, NULL),
                 HU_OK);

    HU_ASSERT_EQ(rc.call_count, 1);                       /* LLM fallback fired */
    HU_ASSERT_TRUE(model_has_object(&model, "the email")); /* and the fact was merged */
    pm_reset_fallback();
}

static void llm_fallback_short_circuits_when_regex_hits(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pm_rec_ctx_t rc = {.canned = pm_canned_json, .canned_len = strlen(pm_canned_json), .call_count = 0};
    hu_provider_t prov = {.ctx = &rc, .vtable = &pm_vtable};

    pm_reset_fallback();
    setenv("HU_LLM_FACT_EXTRACT", "on", 1);
    hu_personal_model_set_llm_extractor(&alloc, &prov, "m", 1);

    hu_personal_model_t model;
    memset(&model, 0, sizeof(model));
    /* "i like " is a regex marker — fast-path produces a fact, so the
     * expensive LLM fallback must NOT run. */
    const char *hit = "i like climbing on the weekends";
    HU_ASSERT_EQ(hu_personal_model_ingest(&model, hit, strlen(hit), true, 1700000000LL, NULL),
                 HU_OK);

    HU_ASSERT_EQ(rc.call_count, 0); /* short-circuited */
    HU_ASSERT_TRUE(model.fact_count >= 1);
    pm_reset_fallback();
}

static void llm_fallback_shadow_does_not_merge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pm_rec_ctx_t rc = {.canned = pm_canned_json, .canned_len = strlen(pm_canned_json), .call_count = 0};
    hu_provider_t prov = {.ctx = &rc, .vtable = &pm_vtable};

    pm_reset_fallback();
    setenv("HU_LLM_FACT_EXTRACT", "shadow", 1);
    hu_personal_model_set_llm_extractor(&alloc, &prov, "m", 1);

    hu_personal_model_t model;
    memset(&model, 0, sizeof(model));
    HU_ASSERT_EQ(hu_personal_model_ingest(&model, pm_casual_msg, strlen(pm_casual_msg), true, 1700000000LL, NULL),
                 HU_OK);

    HU_ASSERT_EQ(rc.call_count, 1);                        /* ran in shadow */
    HU_ASSERT_FALSE(model_has_object(&model, "the email")); /* but did NOT merge */
    pm_reset_fallback();
}

static void llm_fallback_off_does_not_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pm_rec_ctx_t rc = {.canned = pm_canned_json, .canned_len = strlen(pm_canned_json), .call_count = 0};
    hu_provider_t prov = {.ctx = &rc, .vtable = &pm_vtable};

    pm_reset_fallback(); /* env unset => OFF */
    hu_personal_model_set_llm_extractor(&alloc, &prov, "m", 1);

    hu_personal_model_t model;
    memset(&model, 0, sizeof(model));
    HU_ASSERT_EQ(hu_personal_model_ingest(&model, pm_casual_msg, strlen(pm_casual_msg), true, 1700000000LL, NULL),
                 HU_OK);

    HU_ASSERT_EQ(rc.call_count, 0);
    HU_ASSERT_FALSE(model_has_object(&model, "the email"));
    pm_reset_fallback();
}

static void llm_fallback_skips_when_no_provider(void) {
    pm_reset_fallback();
    setenv("HU_LLM_FACT_EXTRACT", "on", 1); /* gate ON but no provider injected */

    hu_personal_model_t model;
    memset(&model, 0, sizeof(model));
    HU_ASSERT_EQ(hu_personal_model_ingest(&model, pm_casual_msg, strlen(pm_casual_msg), true, 1700000000LL, NULL),
                 HU_OK);

    HU_ASSERT_FALSE(model_has_object(&model, "the email")); /* no crash, no fact */
    pm_reset_fallback();
}

static void llm_fallback_skips_short_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pm_rec_ctx_t rc = {.canned = pm_canned_json, .canned_len = strlen(pm_canned_json), .call_count = 0};
    hu_provider_t prov = {.ctx = &rc, .vtable = &pm_vtable};

    pm_reset_fallback();
    setenv("HU_LLM_FACT_EXTRACT", "on", 1);
    hu_personal_model_set_llm_extractor(&alloc, &prov, "m", 1);

    hu_personal_model_t model;
    memset(&model, 0, sizeof(model));
    const char *tiny = "C?"; /* regex-empty but too short to be worth an LLM call */
    HU_ASSERT_EQ(hu_personal_model_ingest(&model, tiny, strlen(tiny), true, 1700000000LL, NULL),
                 HU_OK);

    HU_ASSERT_EQ(rc.call_count, 0);
    pm_reset_fallback();
}

void run_personal_model_llm_extract_tests(void) {
    HU_TEST_SUITE("personal_model_llm_extract");
    HU_RUN_TEST(llm_fallback_extracts_when_regex_empty_and_live);
    HU_RUN_TEST(llm_fallback_short_circuits_when_regex_hits);
    HU_RUN_TEST(llm_fallback_shadow_does_not_merge);
    HU_RUN_TEST(llm_fallback_off_does_not_call);
    HU_RUN_TEST(llm_fallback_skips_when_no_provider);
    HU_RUN_TEST(llm_fallback_skips_short_messages);
}
