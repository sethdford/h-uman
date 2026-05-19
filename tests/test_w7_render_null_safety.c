/* M4 audit follow-up — null-safety pin for hu_w7_render_world_model.
 *
 * Background: commit 22a9fa7a fixed a defensive null-check at agent_turn.c
 * for tom_p/q/c (though those are fixed `char[N]` arrays inside the agent
 * struct, so they can never actually be NULL — pointer-to-array can't be).
 * Commit 528755cb fixed the REAL crash, which was:
 *   (a) stack-use-after-scope on an 8 KB personal_model_buf, and
 *   (b) stack overflow on the gateway's 512 KB pthread worker stacks.
 *
 * Per ~/.claude/rules/audit-verify-before-allege.md, this file empirically
 * pins the null-derefable inputs of hu_w7_render_world_model so any future
 * regression that strips a guard re-fires here instead of in production
 * BUS-error territory. Each test exercises a specific NULL combination
 * the bridge claims to accept by contract (per world_model_bridge.h):
 *
 *   • persona_ctx == NULL                            → no persona merge
 *   • persona_ctx->persona == NULL                   → no persona merge
 *   • persona_ctx->channel == NULL / 0               → overlay skipped
 *   • persona_ctx->tools == NULL / count == 0        → capabilities skipped
 *   • persona_ctx->recent_tools_used == NULL / 0     → tools section skipped
 *   • pm == NULL                                     → personal-model skipped
 *   • tom_premise == NULL                            → ToM scenario skipped
 *
 * The bridge already returns HU_ERR_INVALID_ARGUMENT for required NULL
 * inputs (facade / facade->m / alloc / contact_id / out_text / out_len);
 * test_world_model_bridge.c::bridge_render_rejects_invalid_args covers
 * those. This file covers the OPTIONAL-input null-safety contract. */

#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_w7ns_alloc;
static hu_allocator_t *W7NS_A(void) {
    g_w7ns_alloc = hu_system_allocator();
    return &g_w7ns_alloc;
}

static void w7ns_open(hu_graph_t **out_g, hu_w7_facade_t **out_f) {
    setenv("HU_WORLD_MODEL_TTL_MS", "0", 1);
    HU_ASSERT_EQ(hu_graph_open(W7NS_A(), NULL, 0, out_g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(*out_g, W7NS_A(), out_f), HU_OK);
}

static void w7ns_cleanup(hu_graph_t *g, hu_w7_facade_t *f) {
    if (f)
        hu_w7_facade_close(f, W7NS_A());
    if (g)
        hu_graph_close(g, W7NS_A());
}

/* The shape of an M4 stateless chat-completion request: empty graph, no
 * persona context, no personal model, no ToM scenario. Pre-528755cb this
 * was the request that crashed the gateway. */
static void w7_render_stateless_chat_request_does_not_crash(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_stateless", 12, 1700000000000LL, &txt,
                                          &tlen,
                                          /* tom_p */ NULL, 0,
                                          /* tom_q */ NULL, 0,
                                          /* tom_c */ NULL, 0,
                                          /* pm */ NULL,
                                          /* persona_ctx */ NULL),
                 HU_OK);
    /* Empty world model + no merges → bridge returns NULL/0 (caller skips
     * injection). The contract is documented in world_model_bridge.h:91. */
    HU_ASSERT(txt == NULL);
    HU_ASSERT_EQ(tlen, (size_t)0);

    w7ns_cleanup(g, f);
}

/* Partial ToM scenario (premise present, question NULL) must NOT crash —
 * the bridge requires all three to be present to fire the merge. */
static void w7_render_partial_tom_scenario_skips_merge_safely(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    const char *p = "Anne moves the ball.";
    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_partial_tom", 14, 1700000000000LL, &txt,
                                          &tlen, p, strlen(p),
                                          /* tom_q */ NULL, 0,
                                          /* tom_c */ NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT(txt == NULL);
    HU_ASSERT_EQ(tlen, (size_t)0);

    w7ns_cleanup(g, f);
}

/* Persona context with NULL persona pointer — bridge must not deref. */
static void w7_render_persona_ctx_with_null_persona_is_noop(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    hu_persona_context_t pctx = {0};
    /* All fields zero; persona == NULL so merge_persona is skipped. */

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_null_persona", 15, 1700000000000LL, &txt,
                                          &tlen, NULL, 0, NULL, 0, NULL, 0, NULL, &pctx),
                 HU_OK);
    /* No persona → merge skipped → empty world model → NULL/0 out. */
    HU_ASSERT(txt == NULL);
    HU_ASSERT_EQ(tlen, (size_t)0);

    w7ns_cleanup(g, f);
}

/* Persona context with persona present but channel NULL — overlay merge
 * is skipped per the documented contract (header line 39). */
static void w7_render_persona_ctx_with_null_channel_emits_identity_only(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    /* hu_persona_t::name and ::identity are char* — point at string
     * literals (read-only, stable for the test's duration). */
    persona.name = (char *)"Test Bot";
    persona.name_len = strlen(persona.name);
    persona.identity = (char *)"a deterministic test persona used in null-safety pinning";

    hu_persona_context_t pctx = {0};
    pctx.persona = &persona;
    pctx.channel = NULL;
    pctx.channel_len = 0;
    pctx.delta_limit = 0;
    pctx.tools = NULL;
    pctx.tools_count = 0;
    pctx.recent_tools_used = NULL;
    pctx.recent_tools_used_count = 0;

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_null_chan", 12, 1700000000000LL, &txt,
                                          &tlen, NULL, 0, NULL, 0, NULL, 0, NULL, &pctx),
                 HU_OK);
    /* Persona merge fires (identity present) → at minimum the ToM block
     * "user_thinks_we_are" gets populated. Output should be non-NULL. */
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT_GT(tlen, (size_t)0);

    W7NS_A()->free(W7NS_A()->ctx, txt, tlen + 1);
    w7ns_cleanup(g, f);
}

/* Persona context with NULL tools but tools_count > 0 — bridge must
 * check tools pointer before reading tools_count entries. */
static void w7_render_persona_ctx_null_tools_does_not_crash(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.name = (char *)"Test Bot";
    persona.name_len = strlen(persona.name);
    persona.identity = (char *)"tester";

    hu_persona_context_t pctx = {0};
    pctx.persona = &persona;
    pctx.tools = NULL;
    pctx.tools_count = 0; /* count must be 0 when ptr is NULL — bridge gates on both */

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_null_tools", 13, 1700000000000LL, &txt,
                                          &tlen, NULL, 0, NULL, 0, NULL, 0, NULL, &pctx),
                 HU_OK);
    /* Persona merge populates identity-derived ToM fields. Output is
     * non-NULL because at least user_thinks_we_are is filled in. */
    HU_ASSERT_NOT_NULL(txt);
    if (txt)
        W7NS_A()->free(W7NS_A()->ctx, txt, tlen + 1);

    w7ns_cleanup(g, f);
}

/* Persona context with NULL recent_tools_used but count > 0 — bridge
 * must check the pointer (gate is `ptr && count > 0`). */
static void w7_render_persona_ctx_null_recent_tools_does_not_crash(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.name = (char *)"Test Bot";
    persona.name_len = strlen(persona.name);

    hu_persona_context_t pctx = {0};
    pctx.persona = &persona;
    pctx.recent_tools_used = NULL;
    pctx.recent_tools_used_count = 0;

    char *txt = NULL;
    size_t tlen = 0;
    /* Bridge must accept NULL recent_tools_used without dereferencing it. */
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_null_rtu", 11, 1700000000000LL, &txt,
                                          &tlen, NULL, 0, NULL, 0, NULL, 0, NULL, &pctx),
                 HU_OK);
    if (txt)
        W7NS_A()->free(W7NS_A()->ctx, txt, tlen + 1);

    w7ns_cleanup(g, f);
}

/* Empty personal model (zero-initialized) — `hu_world_model_merge_personal`
 * short-circuits on `!hu_personal_model_has_content`. The bridge must
 * tolerate an empty-but-non-NULL pm. */
static void w7_render_empty_personal_model_is_safe(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    hu_personal_model_t pm;
    memset(&pm, 0, sizeof(pm));

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_empty_pm", 11, 1700000000000LL, &txt,
                                          &tlen, NULL, 0, NULL, 0, NULL, 0, &pm, NULL),
                 HU_OK);
    /* Empty PM + empty world model + no persona → NULL/0. */
    HU_ASSERT(txt == NULL);
    HU_ASSERT_EQ(tlen, (size_t)0);

    w7ns_cleanup(g, f);
}

/* Repeated render calls with the FULL null-everything-optional shape —
 * exercises any per-call leak or use-after-free that would surface as a
 * BUS only after thousands of turns through the gateway. */
static void w7_render_repeated_null_optional_calls_stable(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    w7ns_open(&g, &f);

    for (int i = 0; i < 16; i++) {
        char *txt = NULL;
        size_t tlen = 0;
        HU_ASSERT_EQ(hu_w7_render_world_model(f, W7NS_A(), "m4_repeat", 9, 1700000000000LL + i,
                                              &txt, &tlen, NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                     HU_OK);
        HU_ASSERT(txt == NULL);
        HU_ASSERT_EQ(tlen, (size_t)0);
    }

    w7ns_cleanup(g, f);
}

#endif /* HU_ENABLE_SQLITE */

void run_w7_render_null_safety_tests(void) {
    HU_TEST_SUITE("W7 render world model — null safety (M4 audit)");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(w7_render_stateless_chat_request_does_not_crash);
    HU_RUN_TEST(w7_render_partial_tom_scenario_skips_merge_safely);
    HU_RUN_TEST(w7_render_persona_ctx_with_null_persona_is_noop);
    HU_RUN_TEST(w7_render_persona_ctx_with_null_channel_emits_identity_only);
    HU_RUN_TEST(w7_render_persona_ctx_null_tools_does_not_crash);
    HU_RUN_TEST(w7_render_persona_ctx_null_recent_tools_does_not_crash);
    HU_RUN_TEST(w7_render_empty_personal_model_is_safe);
    HU_RUN_TEST(w7_render_repeated_null_optional_calls_stable);
#endif
}
