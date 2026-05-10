/*
 * W12 — Retrieval-planner LLM backend.
 *
 * Sends the goal + a compact world-model digest to the configured provider,
 * asks for a JSON plan matching `hu_retrieval_plan_t`, parses, validates,
 * clamps. Falls back to a deterministic single-step plan when the provider
 * is NULL, the call fails, or the JSON is malformed — so callers can flip
 * backends without worrying about availability.
 *
 * The system prompt locks the schema. The user prompt embeds the goal +
 * a 6-line world-model summary (contact + top entities, top relations,
 * dominant emotion). Temperature is 0.0 because we want determinism, not
 * creativity, in plan emission.
 *
 * Hard caps from `retrieval_planner.h` (steps_count <= 8, total_budget_ms
 * <= 500) are enforced after parse via `clamp_plan` in `retrieval_planner.c`.
 *
 * Layer 4 of the v2 stack; reads layers 0-3 only.
 */
#include "human/agent/retrieval_planner.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/memory/memory.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Ctx ───────────────────────────────────────────────────────────────── */

typedef struct llm_ctx {
    hu_provider_t *provider; /* borrowed; NULL ⇒ fallback to deterministic plan */
    hu_allocator_t *alloc;   /* scratch for prompt + response */
    char *model;             /* optional model override; NULL ⇒ provider default */
    size_t model_len;
} llm_ctx_t;

/* Single static ctx because the backend is stateless. The provider pointer
 * threads through; the alloc is captured at hu_planner_llm() time. */
static llm_ctx_t s_llm_ctx;

/* ── System prompt (locked schema) ─────────────────────────────────────── */

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static const char *const LLM_SYS_PROMPT =
"You are a retrieval planner inside an autonomous memory system. Given a "
"user GOAL and a compact WORLD-MODEL digest, emit a JSON plan describing "
"which memory kinds to retrieve in what order.\n"
"\n"
"Output a single JSON object. Do NOT wrap it in markdown fences. Schema:\n"
"\n"
"{\n"
"  \"total_budget_ms\": <int 0..500>,\n"
"  \"steps\": [\n"
"    {\n"
"      \"kind\": \"entity\" | \"relation\" | \"hyperedge\" | \"reasoning_trace\",\n"
"      \"hops\": <int 0..3>,\n"
"      \"budget_ms\": <int 0..500>,\n"
"      \"verify_after\": <bool>,\n"
"      \"window_from_ms\": <int>,    // 0 = open\n"
"      \"window_to_ms\": <int>,      // 0 = open\n"
"      \"limit\": <int 1..64>\n"
"    }\n"
"  ]\n"
"}\n"
"\n"
"Rules:\n"
"- steps array has 1..8 elements; emit only the steps you actually need.\n"
"- total_budget_ms is the sum across steps; keep it minimal.\n"
"- Prefer 1 step for trivial recall; 2-3 for entity→relation expansion;\n"
"  reserve hops>1 for true multi-hop questions only.\n"
"- Set verify_after=true when the goal asserts a specific fact (who/when/where).\n"
"- Do not invent kinds outside the four listed.\n"
"- Return ONLY the JSON object, nothing else.\n";
#endif /* !HU_IS_TEST */

/* ── World-model digest ────────────────────────────────────────────────── */

#if !(defined(HU_IS_TEST) && HU_IS_TEST)

/* Render a 6-line summary of the world model so the LLM has something to
 * ground the plan in. Bounded to ~512 bytes to keep the prompt small. */
static char *render_wm_digest(hu_allocator_t *alloc, const hu_world_model_t *wm) {
    size_t cap = 1024;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return NULL;
    size_t off = 0;

    int n = snprintf(buf + off, cap - off, "Contact: %.63s\n", wm ? wm->contact_id : "");
    if (n > 0) off += (size_t)n;

    if (wm && wm->entities_count > 0) {
        n = snprintf(buf + off, cap - off, "Top entities: ");
        if (n > 0) off += (size_t)n;
        size_t top = wm->entities_count < 5 ? wm->entities_count : 5;
        for (size_t i = 0; i < top && off + 80 < cap; i++) {
            const hu_graph_entity_t *e = &wm->entities[i];
            n = snprintf(buf + off, cap - off, "%s%.40s",
                         i == 0 ? "" : ", ",
                         e->name ? e->name : "");
            if (n > 0) off += (size_t)n;
        }
        if (off + 2 < cap) { buf[off++] = '\n'; }
    }

    if (wm && wm->relations_count > 0) {
        n = snprintf(buf + off, cap - off, "Top relations: %zu rows\n",
                     wm->relations_count);
        if (n > 0) off += (size_t)n;
    }

    if (wm && wm->dominant_emotion[0]) {
        n = snprintf(buf + off, cap - off, "Emotion: %.31s (v=%.2f a=%.2f)\n",
                     wm->dominant_emotion, wm->valence, wm->arousal);
        if (n > 0) off += (size_t)n;
    }

    if (wm && wm->goals_count > 0) {
        n = snprintf(buf + off, cap - off, "Goals: %zu active\n", wm->goals_count);
        if (n > 0) off += (size_t)n;
    }

    if (wm && wm->recent_topics_count > 0) {
        n = snprintf(buf + off, cap - off, "Recent topics: ");
        if (n > 0) off += (size_t)n;
        size_t top = wm->recent_topics_count < 3 ? wm->recent_topics_count : 3;
        for (size_t i = 0; i < top && off + 80 < cap; i++) {
            n = snprintf(buf + off, cap - off, "%s%.40s",
                         i == 0 ? "" : ", ", wm->recent_topics[i]);
            if (n > 0) off += (size_t)n;
        }
        if (off + 2 < cap) { buf[off++] = '\n'; }
    }

    if (off < cap) buf[off] = '\0';
    else buf[cap - 1] = '\0';
    return buf;
}

/* ── User message: goal + digest ───────────────────────────────────────── */

static char *render_user_msg(hu_allocator_t *alloc, const char *goal, size_t goal_len,
                             const hu_world_model_t *wm) {
    char *digest = render_wm_digest(alloc, wm);
    size_t digest_len = digest ? strlen(digest) : 0;

    size_t cap = goal_len + digest_len + 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        if (digest) alloc->free(alloc->ctx, digest, 1024);
        return NULL;
    }
    int n = snprintf(buf, cap, "GOAL:\n%.*s\n\nWORLD MODEL:\n%s\n",
                     (int)goal_len, goal ? goal : "", digest ? digest : "");
    if (digest) alloc->free(alloc->ctx, digest, 1024);
    if (n <= 0) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    return buf;
}
#endif /* !HU_IS_TEST */

/* ── Kind string → enum ────────────────────────────────────────────────── */

static hu_memory_kind_t parse_kind(const char *s, size_t len) {
    if (!s || len == 0) return HU_MEM_RELATION;
    if (len == 6 && memcmp(s, "entity", 6) == 0) return HU_MEM_ENTITY;
    if (len == 8 && memcmp(s, "relation", 8) == 0) return HU_MEM_RELATION;
    if (len == 9 && memcmp(s, "hyperedge", 9) == 0) return HU_MEM_HYPEREDGE;
    if (len == 16 && memcmp(s, "reasoning_trace", 15) == 0) return HU_MEM_REASONING_TRACE;
    return HU_MEM_RELATION;
}

/* ── JSON → hu_retrieval_plan_t ────────────────────────────────────────── */

static hu_error_t apply_step_from_json(const hu_json_value_t *step,
                                       const hu_world_model_t *wm,
                                       hu_retrieval_step_t *out) {
    memset(out, 0, sizeof(*out));

    /* kind */
    const hu_json_value_t *kind_v = hu_json_object_get(step, "kind");
    if (kind_v && kind_v->type == HU_JSON_STRING) {
        out->kind = parse_kind(kind_v->data.string.ptr, kind_v->data.string.len);
    } else {
        out->kind = HU_MEM_RELATION;
    }

    /* hops, budget_ms, verify_after */
    out->hops = (size_t)hu_json_get_number(step, "hops", 0);
    out->budget_ms = (int)hu_json_get_number(step, "budget_ms", 100);
    out->verify_after = hu_json_get_bool(step, "verify_after", false);

    /* Query window */
    out->query.kind = out->kind;
    if (wm) {
        out->query.contact_id = wm->contact_id;
        size_t cidlen = 0;
        while (cidlen < sizeof(wm->contact_id) && wm->contact_id[cidlen]) cidlen++;
        out->query.contact_id_len = cidlen;
    }

    /* For entity kind, default to "by_name" with NULL name (caller must
     * fill); for relation/hyperedge, use the window. We don't know the
     * entity name from the LLM today — relation-window is the safe path. */
    int64_t from_ms = (int64_t)hu_json_get_number(step, "window_from_ms", 0);
    int64_t to_ms   = (int64_t)hu_json_get_number(step, "window_to_ms", 0);
    int limit       = (int)hu_json_get_number(step, "limit", 16);
    if (to_ms == 0) to_ms = INT64_MAX;
    if (limit <= 0) limit = 16;
    if (limit > 64) limit = 64;

    out->query.variant = HU_MEMORY_QUERY_WINDOW;
    out->query.as.window.from_ts = from_ms;
    out->query.as.window.to_ts = to_ms;
    out->query.as.window.limit = (size_t)limit;

    return HU_OK;
}

static hu_error_t parse_plan_json(hu_allocator_t *alloc, const char *json,
                                  size_t json_len, const hu_world_model_t *wm,
                                  hu_retrieval_plan_t *out) {
    if (!json || json_len == 0) return HU_ERR_INVALID_ARGUMENT;

    /* The provider may wrap output in ```json fences; strip them. */
    while (json_len > 0 && (*json == ' ' || *json == '\n' || *json == '\r' || *json == '\t')) {
        json++; json_len--;
    }
    if (json_len > 7 && memcmp(json, "```json", 7) == 0) {
        json += 7; json_len -= 7;
    } else if (json_len > 3 && memcmp(json, "```", 3) == 0) {
        json += 3; json_len -= 3;
    }
    /* Trim trailing fence. */
    while (json_len > 0 && (json[json_len - 1] == ' ' || json[json_len - 1] == '\n' ||
                            json[json_len - 1] == '\r' || json[json_len - 1] == '\t')) {
        json_len--;
    }
    if (json_len > 3 && memcmp(json + json_len - 3, "```", 3) == 0) {
        json_len -= 3;
    }

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root) hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    out->total_budget_ms = (int)hu_json_get_number(root, "total_budget_ms", 250);

    hu_json_value_t *steps = hu_json_object_get(root, "steps");
    if (!steps || steps->type != HU_JSON_ARRAY || steps->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t n = steps->data.array.len;
    if (n > HU_PLANNER_MAX_STEPS) n = HU_PLANNER_MAX_STEPS;
    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *step = steps->data.array.items[i];
        if (!step || step->type != HU_JSON_OBJECT) continue;
        (void)apply_step_from_json(step, wm, &out->steps[out->steps_count]);
        out->steps_count++;
    }

    hu_json_free(alloc, root);
    if (out->steps_count == 0) return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

/* ── Fallback plan (used when provider is unavailable or returns garbage) ─ */

static void deterministic_fallback(const hu_world_model_t *wm, hu_retrieval_plan_t *out) {
    memset(out, 0, sizeof(*out));
    hu_retrieval_step_t *s = &out->steps[0];
    s->kind = HU_MEM_RELATION;
    s->query.kind = HU_MEM_RELATION;
    s->query.variant = HU_MEMORY_QUERY_WINDOW;
    if (wm) {
        s->query.contact_id = wm->contact_id;
        size_t maxlen = sizeof(wm->contact_id);
        size_t cidlen = 0;
        while (cidlen < maxlen && wm->contact_id[cidlen]) cidlen++;
        s->query.contact_id_len = cidlen;
    }
    s->query.as.window.from_ts = 0;
    s->query.as.window.to_ts = INT64_MAX;
    s->query.as.window.limit = 16;
    s->hops = 1;
    s->budget_ms = 200;
    s->verify_after = true;

    out->steps_count = 1;
    out->total_budget_ms = 250;
}

/* ── plan() ────────────────────────────────────────────────────────────── */

static hu_error_t llm_plan(void *ctx_v, const char *goal, size_t goal_len,
                           const hu_world_model_t *wm, hu_retrieval_plan_t *out) {
    llm_ctx_t *ctx = (llm_ctx_t *)ctx_v;
    if (!out) return HU_ERR_INVALID_ARGUMENT;

    /* Provider missing: deterministic fallback. Same shape, no I/O. */
    if (!ctx || !ctx->provider || !ctx->provider->vtable ||
        !ctx->provider->vtable->chat_with_system) {
        (void)goal; (void)goal_len;
        deterministic_fallback(wm, out);
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Tests must be deterministic and free of provider I/O. */
    (void)goal;
    (void)goal_len;
    deterministic_fallback(wm, out);
    return HU_OK;
#else
    hu_allocator_t *alloc = ctx->alloc;
    if (!alloc) {
        deterministic_fallback(wm, out);
        return HU_OK;
    }

    char *user_msg = render_user_msg(alloc, goal, goal_len, wm);
    if (!user_msg) {
        deterministic_fallback(wm, out);
        return HU_OK;
    }
    size_t user_len = strlen(user_msg);

    char *response = NULL;
    size_t response_len = 0;
    const char *model = ctx->model;
    size_t model_len = ctx->model_len;

    hu_error_t err = ctx->provider->vtable->chat_with_system(
        ctx->provider->ctx, alloc, LLM_SYS_PROMPT, strlen(LLM_SYS_PROMPT),
        user_msg, user_len, model, model_len, /*temperature=*/0.0,
        &response, &response_len);

    alloc->free(alloc->ctx, user_msg, user_len + 1);

    if (err != HU_OK || !response || response_len == 0) {
        if (response) alloc->free(alloc->ctx, response, response_len);
        deterministic_fallback(wm, out);
        return HU_OK;
    }

    err = parse_plan_json(alloc, response, response_len, wm, out);
    alloc->free(alloc->ctx, response, response_len);
    if (err != HU_OK) {
        deterministic_fallback(wm, out);
        return HU_OK;
    }

    return HU_OK;
#endif
}

static void llm_deinit(void *ctx_v) {
    llm_ctx_t *ctx = (llm_ctx_t *)ctx_v;
    if (!ctx) return;
    if (ctx->model && ctx->alloc) {
        ctx->alloc->free(ctx->alloc->ctx, ctx->model, ctx->model_len + 1);
        ctx->model = NULL;
        ctx->model_len = 0;
    }
}

static hu_planner_vtable_t s_llm_vt = {
    .name = "llm",
    .plan = llm_plan,
    .deinit = llm_deinit,
};

hu_error_t hu_planner_llm(hu_provider_t *p, hu_planner_t *out) {
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    s_llm_ctx.provider = p;
    /* If alloc wasn't set via hu_planner_llm_set_alloc, fall through to
     * deterministic fallback when plan() is called. Tests use this path. */
    out->vt = &s_llm_vt;
    out->ctx = &s_llm_ctx;
    return HU_OK;
}

/* Optional configuration: set the allocator the planner uses for prompt /
 * response scratch. Without this, plan() falls back to deterministic mode.
 *
 * Not exposed in the header to keep the surface lean — callers that need
 * full LLM mode wire this through their bootstrap (agent_turn or daemon).
 * Declared `extern` for symbol visibility in tests. */
void hu_planner_llm_configure(hu_allocator_t *alloc, const char *model, size_t model_len) {
    s_llm_ctx.alloc = alloc;
    if (s_llm_ctx.model && s_llm_ctx.alloc) {
        s_llm_ctx.alloc->free(s_llm_ctx.alloc->ctx, s_llm_ctx.model, s_llm_ctx.model_len + 1);
    }
    s_llm_ctx.model = NULL;
    s_llm_ctx.model_len = 0;
    if (model && model_len > 0 && alloc) {
        char *copy = (char *)alloc->alloc(alloc->ctx, model_len + 1);
        if (copy) {
            memcpy(copy, model, model_len);
            copy[model_len] = '\0';
            s_llm_ctx.model = copy;
            s_llm_ctx.model_len = model_len;
        }
    }
}

#if defined(HU_IS_TEST) && HU_IS_TEST
/* Test hook: invoke the JSON parser directly. Not exposed in the public
 * header — declared `extern` in the test source. Returns the same error
 * codes as `plan()`'s internal parse step. */
hu_error_t hu_planner_llm__test_parse_json(hu_allocator_t *alloc, const char *json,
                                           size_t json_len, const hu_world_model_t *wm,
                                           hu_retrieval_plan_t *out) {
    return parse_plan_json(alloc, json, json_len, wm, out);
}
#endif
