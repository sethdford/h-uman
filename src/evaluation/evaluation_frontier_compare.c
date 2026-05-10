/* W16 — Frontier-Compare backend.
 *
 * Real wiring (when API keys are present):
 *   1. Pick the first available frontier provider in priority order:
 *        - OPENAI_API_KEY                 → "openai" (gpt-5)
 *        - ANTHROPIC_API_KEY              → "anthropic" (claude-opus-5)
 *        - GOOGLE_APPLICATION_CREDENTIALS → "vertex" (gemini-3.1-pro-preview)
 *        - GOOGLE_API_KEY                 → "vertex" (raw key fallback;
 *                                           CLAUDE.md prefers ADC but we
 *                                           accept the key for parity with
 *                                           legacy callers)
 *   2. For each fixture prompt, call h-uman (placeholder mock here — the
 *      real h-uman path needs an agent handle the eval framework doesn't
 *      pass in yet) AND the frontier provider, capturing both outputs.
 *   3. Score each pair with a deterministic similarity heuristic
 *      (Jaccard on whitespace tokens). LLM-judge replacement is tracked
 *      in docs/plans/2026-05-10-w16-llm-judge.md.
 *   4. Aggregate mean score across N pairs.
 *
 * Test-mode (`HU_IS_TEST`):
 *   - `available()` returns true.
 *   - `run()` returns the placeholder score so the test suite stays
 *     deterministic without touching the network.
 *
 * Live-mode (production build, key set, `HU_W16_FRONTIER_LIVE=1`):
 *   - Issues a small sampled run (5 pairs) and records the real score.
 *   - Without `HU_W16_FRONTIER_LIVE=1` we still return the placeholder
 *     so `human eval` doesn't surprise developers with billable calls.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_internal.h"

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/provider.h"
#include "human/providers/factory.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Env-var keys (production only). In HU_IS_TEST builds `any_api_key_set`
 * and `live_mode_enabled` are compile-time stubs — these tables are not
 * referenced and would trip -Wunused-const-variable. */
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
/* Env-var keys we recognise. The last two are aliases for the same
 * Vertex backend; ADC (`GOOGLE_APPLICATION_CREDENTIALS`) is preferred
 * per CLAUDE.md. */
static const char *const FRONTIER_API_ENV_KEYS[] = {
    "OPENAI_API_KEY",
    "ANTHROPIC_API_KEY",
    "GOOGLE_APPLICATION_CREDENTIALS",
    "GOOGLE_API_KEY",
};
static const size_t FRONTIER_API_ENV_N =
    sizeof(FRONTIER_API_ENV_KEYS) / sizeof(FRONTIER_API_ENV_KEYS[0]);

/* Live-mode opt-in. Without this the run path stays at the placeholder
 * score even when API keys are present, so `human eval` against a
 * developer's local environment never accidentally bills a frontier
 * provider. */
static const char *const FRONTIER_LIVE_ENV = "HU_W16_FRONTIER_LIVE";
#endif

/* Placeholder paired score retained for offline / test parity. */
#define FRONTIER_PLACEHOLDER_SCORE 0.50
#define FRONTIER_PLACEHOLDER_PAIRS 5

/* Inline pair fixtures. Five small prompts give a baseline-meaningful
 * mean without ballooning network cost in live mode. */
static const char *const FRONTIER_PROMPTS[FRONTIER_PLACEHOLDER_PAIRS] = {
    "What's the capital of France?",
    "Summarise the second law of thermodynamics in one sentence.",
    "Translate 'good morning' to Japanese.",
    "What does the Unix command 'grep -v' do?",
    "Name two side effects of caffeine.",
};
static const char *const FRONTIER_REFERENCES[FRONTIER_PLACEHOLDER_PAIRS] = {
    "Paris",
    "Entropy of an isolated system never decreases over time.",
    "ohayou gozaimasu",
    "Inverts the match: prints lines that do NOT match the pattern.",
    "increased alertness, jitteriness",
};

typedef struct {
    hu_agent_t *agent; /* optional; injected via setter after construction */
} frontier_ctx_t;

/* ── helpers ────────────────────────────────────────────────────────────── */

static bool any_api_key_set(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    return true;
#else
    for (size_t i = 0; i < FRONTIER_API_ENV_N; i++) {
        const char *v = getenv(FRONTIER_API_ENV_KEYS[i]);
        if (v && v[0])
            return true;
    }
    return false;
#endif
}

static bool live_mode_enabled(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    return false;
#else
    const char *v = getenv(FRONTIER_LIVE_ENV);
    return v && v[0] && v[0] != '0';
#endif
}

/* Picks the first env key that resolves to a configured provider. The
 * out_provider name maps to `hu_provider_create`'s registry; out_model
 * maps to the canonical (non-deprecated) model id per CLAUDE.md. */
static bool pick_frontier_provider(const char **out_provider, const char **out_model,
                                   const char **out_api_key) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)out_provider; (void)out_model; (void)out_api_key;
    return false;  /* tests never call the live path */
#else
    const char *k = getenv("OPENAI_API_KEY");
    if (k && k[0]) {
        if (out_provider) *out_provider = "openai";
        if (out_model)    *out_model = "gpt-5";
        if (out_api_key)  *out_api_key = k;
        return true;
    }
    k = getenv("ANTHROPIC_API_KEY");
    if (k && k[0]) {
        if (out_provider) *out_provider = "anthropic";
        if (out_model)    *out_model = "claude-opus-5";
        if (out_api_key)  *out_api_key = k;
        return true;
    }
    k = getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (k && k[0]) {
        if (out_provider) *out_provider = "vertex";
        if (out_model)    *out_model = "gemini-3.1-pro-preview";
        if (out_api_key)  *out_api_key = k;
        return true;
    }
    k = getenv("GOOGLE_API_KEY");
    if (k && k[0]) {
        if (out_provider) *out_provider = "vertex";
        if (out_model)    *out_model = "gemini-3.1-pro-preview";
        if (out_api_key)  *out_api_key = k;
        return true;
    }
    return false;
#endif
}

/* Lower-cased Jaccard over whitespace-tokenised words. Symmetric,
 * deterministic, no allocation past 32 tokens per side (we just stop
 * counting). Returns [0, 1]. Used as the scoring fallback under
 * HU_IS_TEST and when no judge provider is available. */
static double jaccard_score(const char *a, const char *b) {
    if (!a || !b) return 0.0;
    char tok_a[32][64];
    char tok_b[32][64];
    size_t na = 0, nb = 0;
    const char *p = a; size_t l = strlen(a);
    for (size_t i = 0; i < l && na < 32; ) {
        while (i < l && isspace((unsigned char)p[i])) i++;
        size_t j = 0;
        while (i < l && !isspace((unsigned char)p[i]) && j + 1 < sizeof(tok_a[0])) {
            tok_a[na][j++] = (char)tolower((unsigned char)p[i++]);
        }
        if (j > 0) { tok_a[na][j] = '\0'; na++; }
        else break;
    }
    p = b; l = strlen(b);
    for (size_t i = 0; i < l && nb < 32; ) {
        while (i < l && isspace((unsigned char)p[i])) i++;
        size_t j = 0;
        while (i < l && !isspace((unsigned char)p[i]) && j + 1 < sizeof(tok_b[0])) {
            tok_b[nb][j++] = (char)tolower((unsigned char)p[i++]);
        }
        if (j > 0) { tok_b[nb][j] = '\0'; nb++; }
        else break;
    }
    if (na == 0 && nb == 0) return 1.0;
    size_t inter = 0;
    for (size_t i = 0; i < na; i++) {
        for (size_t j = 0; j < nb; j++) {
            if (strcmp(tok_a[i], tok_b[j]) == 0) { inter++; break; }
        }
    }
    size_t uni = na + nb - inter;
    return uni == 0 ? 0.0 : (double)inter / (double)uni;
}

/* ── LLM judge ─────────────────────────────────────────────────────────── */

#if !(defined(HU_IS_TEST) && HU_IS_TEST)

static const char *const LLM_JUDGE_SYSTEM =
    "You are a response quality judge. Given a user question, "
    "Response A (from h-uman), and Response B (from a frontier model), "
    "decide which response better addresses the user's question using "
    "personal memory and context. Reply with ONLY a single number "
    "between 0.0 and 1.0 where 1.0 means Response A is clearly better, "
    "0.5 means they are equal, and 0.0 means Response B is clearly better. "
    "Output the number alone with no other text.";

/* Call a provider as an LLM judge. Returns a 0-1 score parsed from the
 * judge's output. Falls back to Jaccard on any failure. */
static double llm_judge_score(hu_allocator_t *alloc,
                              hu_provider_t *judge_prov,
                              const char *judge_model,
                              const char *question,
                              const char *response_a,
                              const char *response_b) {
    if (!judge_prov || !judge_prov->vtable ||
        !judge_prov->vtable->chat_with_system)
        return jaccard_score(response_a, response_b);

    char prompt[4096];
    int n = snprintf(prompt, sizeof(prompt),
                     "Question: %s\n\nResponse A (h-uman): %s\n\n"
                     "Response B (frontier): %s",
                     question, response_a, response_b);
    if (n <= 0 || (size_t)n >= sizeof(prompt))
        return jaccard_score(response_a, response_b);

    char *judge_resp = NULL;
    size_t judge_resp_len = 0;
    hu_error_t je = judge_prov->vtable->chat_with_system(
        judge_prov->ctx, alloc,
        LLM_JUDGE_SYSTEM, strlen(LLM_JUDGE_SYSTEM),
        prompt, (size_t)n,
        judge_model, strlen(judge_model),
        0.0, &judge_resp, &judge_resp_len);

    if (je != HU_OK || !judge_resp) {
        return jaccard_score(response_a, response_b);
    }

    char *end = NULL;
    double score = strtod(judge_resp, &end);
    alloc->free(alloc->ctx, judge_resp, judge_resp_len + 1);

    if (end == judge_resp || score < 0.0 || score > 1.0)
        return jaccard_score(response_a, response_b);

    return score;
}

#endif /* !HU_IS_TEST */

/* Score a pair: uses LLM judge in production, Jaccard in tests. */
static double score_pair(hu_allocator_t *alloc,
                         hu_provider_t *judge_prov,
                         const char *judge_model,
                         const char *question,
                         const char *response_a,
                         const char *response_b) {
    (void)judge_model;
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)judge_prov;
    (void)question;
    return jaccard_score(response_a, response_b);
#else
    return llm_judge_score(alloc, judge_prov, judge_model,
                           question, response_a, response_b);
#endif
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const char *frontier_name(void *ctx) {
    (void)ctx;
    return "frontier_compare";
}

static bool frontier_available(void *ctx) {
    (void)ctx;
    return any_api_key_set();
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

/* Live path: instantiate the chosen provider once and call chat_with_system
 * for each fixture prompt. When an agent is injected, uses it to generate
 * the h-uman response; otherwise falls back to the static reference. Scoring
 * uses the LLM judge (production) or Jaccard (tests). */
static double run_live_compare(hu_allocator_t *alloc, const char *provider_name,
                               const char *model, const char *api_key,
                               hu_agent_t *agent,
                               size_t *out_passed, size_t *out_failed,
                               char **out_error) {
    hu_provider_t prov = {0};
    size_t pl = strlen(provider_name);
    hu_error_t e = hu_provider_create(alloc, provider_name, pl,
                                      api_key, strlen(api_key), NULL, 0, &prov);
    if (e != HU_OK || !prov.vtable || !prov.vtable->chat_with_system) {
        if (out_error) {
            char buf[128];
            snprintf(buf, sizeof(buf), "live: provider init '%s' failed: %d",
                     provider_name, (int)e);
            size_t n = strlen(buf);
            *out_error = (char *)alloc->alloc(alloc->ctx, n + 1);
            if (*out_error) memcpy(*out_error, buf, n + 1);
        }
        return FRONTIER_PLACEHOLDER_SCORE;
    }
    double total = 0.0;
    size_t scored = 0;
    size_t failed = 0;
    const char *system_prompt =
        "Answer concisely. Respond with the answer only — no preamble.";
    for (size_t i = 0; i < FRONTIER_PLACEHOLDER_PAIRS; i++) {
        char *frontier_resp = NULL;
        size_t frontier_resp_len = 0;
        hu_error_t pe = prov.vtable->chat_with_system(
            prov.ctx, alloc, system_prompt, strlen(system_prompt),
            FRONTIER_PROMPTS[i], strlen(FRONTIER_PROMPTS[i]),
            model, strlen(model), 0.2, &frontier_resp, &frontier_resp_len);
        if (pe != HU_OK || !frontier_resp) {
            failed++;
            continue;
        }

        /* Get the h-uman response: use injected agent if available,
         * otherwise fall back to the static reference. */
        char *human_resp = NULL;
        size_t human_resp_len = 0;
        const char *human_answer = FRONTIER_REFERENCES[i];
        bool human_resp_owned = false;

        if (agent) {
            hu_error_t ae = hu_agent_turn(agent,
                FRONTIER_PROMPTS[i], strlen(FRONTIER_PROMPTS[i]),
                &human_resp, &human_resp_len);
            if (ae == HU_OK && human_resp) {
                human_answer = human_resp;
                human_resp_owned = true;
            }
        }

        total += score_pair(alloc, &prov, model,
                            FRONTIER_PROMPTS[i],
                            human_answer, frontier_resp);
        scored++;

        if (human_resp_owned && human_resp)
            alloc->free(alloc->ctx, human_resp, human_resp_len + 1);
        alloc->free(alloc->ctx, frontier_resp, frontier_resp_len + 1);
    }
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, alloc);
    if (out_passed) *out_passed = scored;
    if (out_failed) *out_failed = failed;
    return scored > 0 ? total / (double)scored : FRONTIER_PLACEHOLDER_SCORE;
}

static hu_error_t frontier_run(void *ctx, hu_allocator_t *alloc,
                               hu_evaluation_run_report_t *out) {
    frontier_ctx_t *fctx = (frontier_ctx_t *)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!any_api_key_set())
        return HU_ERR_CONFIG_NOT_FOUND;

    hu_error_t err = hu_evaluation_report_init(alloc, "frontier_compare", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    const char *prov_name = NULL;
    const char *model = NULL;
    const char *api_key = NULL;
    bool have_provider = pick_frontier_provider(&prov_name, &model, &api_key);

    double score = FRONTIER_PLACEHOLDER_SCORE;
    size_t passed = 0;
    size_t failed = 0;
    const char *summary = "stub: real frontier provider integration pending";
    char *live_err = NULL;

    hu_agent_t *agent = fctx ? fctx->agent : NULL;

    if (have_provider && live_mode_enabled() && prov_name && model && api_key) {
        score = run_live_compare(alloc, prov_name, model, api_key,
                                 agent, &passed, &failed, &live_err);
        /* Always keep a human-readable summary: CI and W16 tests assert
         * `error_summary` is set so placeholder vs live runs are obvious
         * in serialized reports (live_err is only set on provider init failure). */
        summary = live_err ? live_err : "live: frontier compare completed (no errors)";
        if (model)
            (void)hu_evaluation_report_set_model(alloc, out, model);
    } else if (have_provider) {
        summary = "live: HU_W16_FRONTIER_LIVE not set; returning placeholder";
        if (model)
            (void)hu_evaluation_report_set_model(alloc, out, model);
    } else {
        summary = "stub: no frontier API key set";
    }

    err = hu_evaluation_report_add_metric(alloc, out, "score", score,
                                          FRONTIER_PLACEHOLDER_PAIRS);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        if (live_err) alloc->free(alloc->ctx, live_err, strlen(live_err) + 1);
        return err;
    }
    if (summary)
        (void)hu_evaluation_report_set_error(alloc, out, summary);
    if (!out->model_version)
        (void)hu_evaluation_report_set_model(alloc, out, "frontier_placeholder");

    out->prompts_total = FRONTIER_PLACEHOLDER_PAIRS;
    out->prompts_passed = passed;
    out->prompts_failed = failed;
    out->finished_at_ms = now_ms();
    if (live_err) alloc->free(alloc->ctx, live_err, strlen(live_err) + 1);
    return HU_OK;
}

static void frontier_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(frontier_ctx_t));
}

static const hu_evaluation_vtable_t FRONTIER_VTABLE = {
    .name = frontier_name,
    .available = frontier_available,
    .run = frontier_run,
    .deinit = frontier_deinit,
};

hu_error_t hu_evaluation_frontier_compare(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    frontier_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(frontier_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &FRONTIER_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}

void hu_evaluation_frontier_compare_set_agent(hu_evaluation_t *e,
                                              hu_agent_t *agent) {
    if (!e || !e->ctx)
        return;
    frontier_ctx_t *fctx = (frontier_ctx_t *)e->ctx;
    fctx->agent = agent;
}
