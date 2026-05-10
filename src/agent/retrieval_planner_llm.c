/*
 * W12 — Retrieval-planner LLM backend (PLACEHOLDER).
 *
 * The eventual contract: send the goal + a compact world-model digest to
 * the configured provider; ask for a JSON plan matching `hu_retrieval_plan_t`
 * (steps_count, kind, query payload, hops, budget); validate; clamp; emit.
 *
 * THIS COMMIT does NOT implement the round-trip. The backend captures the
 * provider pointer (so callers can wire it now) and returns a deterministic
 * single-step plan: list relations in an open window, with verification on.
 * That guarantees we ship without an LLM dependency for tests, and lets the
 * follow-up commit replace the body without touching call sites.
 *
 * Why ship the stub now: callers that flip from heuristic to llm should not
 * break; the caps (steps_count <= 8, total_budget_ms <= 500) are enforced
 * regardless of backend, so the stub plan is always valid input to
 * hu_planner_execute.
 */
#include "human/agent/retrieval_planner.h"

#include "human/core/error.h"
#include "human/memory/memory.h"

#include <stdint.h>
#include <string.h>

typedef struct llm_ctx {
    hu_provider_t *provider; /* captured but not invoked in this commit */
} llm_ctx_t;

/* Single static ctx — the backend is stateless, the provider pointer simply
 * threads through. We don't allocate per-instance to keep the API zero-cost
 * even when callers spin up the planner per turn. */
static llm_ctx_t s_llm_ctx;

static hu_error_t llm_plan(void *ctx, const char *goal, size_t goal_len,
                           const hu_world_model_t *wm, hu_retrieval_plan_t *out) {
    (void)ctx;
    (void)goal;
    (void)goal_len;
    /* TODO(W12-llm): emit a JSON plan via the captured provider; for now
     * return a deterministic single-step plan that is always valid. */
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_retrieval_step_t *s = &out->steps[0];
    s->kind = HU_MEM_RELATION;
    s->query.kind = HU_MEM_RELATION;
    if (wm) {
        s->query.contact_id = wm->contact_id;
        size_t maxlen = sizeof(wm->contact_id);
        size_t cidlen = 0;
        while (cidlen < maxlen && wm->contact_id[cidlen]) cidlen++;
        s->query.contact_id_len = cidlen;
    }
    s->query.as.window.from_ts = 0;
    s->query.as.window.to_ts   = INT64_MAX;
    s->query.as.window.limit   = 16;
    s->hops = 1;
    s->budget_ms = 200;
    s->verify_after = true;

    out->steps_count = 1;
    out->total_budget_ms = 250;
    return HU_OK;
}

static hu_planner_vtable_t s_llm_vt = {
    .name = "llm-stub",
    .plan = llm_plan,
    .deinit = NULL,
};

hu_error_t hu_planner_llm(hu_provider_t *p, hu_planner_t *out) {
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    s_llm_ctx.provider = p; /* captured for the future implementation */
    out->vt = &s_llm_vt;
    out->ctx = &s_llm_ctx;
    return HU_OK;
}
