/* W9 wire bridge (FIX 12). See world_model_bridge.h for the rationale.
 *
 * This TU INTENTIONALLY does NOT include `human/agent.h` or `human/memory.h`
 * (legacy). It is the ONE place where W7 + W9 headers are visible. Adding an
 * include of either legacy header here will reintroduce the type collision
 * the bridge exists to dodge. */

#include "human/agent/world_model_bridge.h"
#include "human/agent/belief_reverify_runner.h"
#include "human/agent/kv_prewarm_runner.h"
#include "human/agent/lora_runner.h"
#include "human/agent/training_data_runner.h"
#include "human/agent/retrieval_planner.h"
#include "human/agent/scheduler.h"
#include "human/agent/self_rag.h"
#include "human/agent/world_model.h"
#include "human/memory/memory.h"
#include "human/provider.h"
#include "human/memory/belief.h"
#include "human/security/audit_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hu_w7_facade {
    hu_memory_facade_t *m;
};

hu_error_t hu_w7_facade_open(struct hu_graph *graph, hu_allocator_t *alloc, hu_w7_facade_t **out) {
    if (!graph || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    hu_w7_facade_t *f = (hu_w7_facade_t *)alloc->alloc(alloc->ctx, sizeof(*f));
    if (!f)
        return HU_ERR_OUT_OF_MEMORY;
    f->m = NULL;
    hu_error_t e = hu_memory_facade_open_on_graph(alloc, graph, &f->m);
    if (e != HU_OK) {
        alloc->free(alloc->ctx, f, sizeof(*f));
        return e;
    }
    *out = f;
    return HU_OK;
}

hu_memory_facade_t *hu_w7_facade_memory_handle(hu_w7_facade_t *facade) {
    return facade ? facade->m : NULL;
}

/* W15 — bridge callback: memory facade audit hook → SQLite audit log. */
static void facade_audit_bridge(void *ctx, hu_memory_audit_op_t op,
                                hu_memory_kind_t kind, int64_t id) {
    hu_audit_log_t *log = (hu_audit_log_t *)ctx;
    if (!log)
        return;
    hu_audit_log_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.operation = (op == HU_MEMORY_AUDIT_WRITE) ? HU_AUDIT_OP_WRITE : HU_AUDIT_OP_ERASE;
    ev.kind = kind;
    ev.target_id = id;
    ev.actor = "facade";
    ev.occurred_at = (int64_t)time(NULL) * 1000;
    (void)hu_audit_log_append(log, &ev);
}

hu_error_t hu_w7_audit_log_open(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                const char *db_path, const char *contact_id,
                                hu_audit_log_t **out) {
    if (!facade || !facade->m || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    hu_audit_log_t *log = NULL;
    hu_error_t e = hu_audit_log_open(alloc, db_path, contact_id, &log);
    if (e != HU_OK)
        return e;
    hu_memory_facade_set_audit_hook(facade->m, facade_audit_bridge, log);
    *out = log;
    return HU_OK;
}

void hu_w7_audit_log_close(hu_audit_log_t *log, hu_allocator_t *alloc) {
    if (log)
        hu_audit_log_close(log, alloc);
}

void hu_w7_facade_close(hu_w7_facade_t *facade, hu_allocator_t *alloc) {
    if (!facade)
        return;
    if (facade->m)
        hu_memory_facade_close(facade->m, alloc);
    if (alloc)
        alloc->free(alloc->ctx, facade, sizeof(*facade));
}

/* Append `s` (length `n`) to a growing buffer. Returns false on OOM. */
static bool buf_append(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap, const char *s,
                       size_t n) {
    if (n == 0)
        return true;
    if (*len + n + 1 > *cap) {
        size_t newcap = *cap == 0 ? 256 : *cap * 2;
        while (newcap < *len + n + 1)
            newcap *= 2;
        char *nb = (char *)alloc->alloc(alloc->ctx, newcap);
        if (!nb)
            return false;
        if (*buf) {
            memcpy(nb, *buf, *len);
            alloc->free(alloc->ctx, *buf, *cap);
        }
        *buf = nb;
        *cap = newcap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static bool buf_appendf(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                        const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    if ((size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    return buf_append(alloc, buf, len, cap, tmp, (size_t)n);
}

static const char *wm_entity_name_by_id(const hu_world_model_t *wm, int64_t id) {
    for (size_t i = 0; i < wm->entities_count; i++) {
        if (wm->entities[i].id == id && wm->entities[i].name)
            return wm->entities[i].name;
    }
    return NULL;
}

hu_error_t hu_w7_render_world_model(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    int64_t now_ms, char **out_text, size_t *out_len) {
    if (out_text)
        *out_text = NULL;
    if (out_len)
        *out_len = 0;
    if (!facade || !facade->m || !alloc || !contact_id || contact_id_len == 0 || !out_text ||
        !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (now_ms == 0)
        now_ms = (int64_t)time(NULL) * 1000;

    hu_world_model_t *wm = NULL;
    hu_error_t e =
        hu_world_model_load(facade->m, alloc, contact_id, contact_id_len, now_ms, &wm);
    if (e != HU_OK || !wm)
        return e == HU_OK ? HU_OK : e;

    /* If everything is empty, return NULL/0 -- callers skip injection.
     *
     * As of P2D the W9 builder synthesizes `dominant_emotion`, `valence`, and
     * `arousal` from the F76 emotional-residue table (`hu_emotional_residue_
     * get_active`), so non-trivial values are real signal. We still treat
     * the post-default "neutral" bucket as "no signal" because the builder
     * falls through to it when no active residues exist for this contact. */
    bool emo_signal = wm->dominant_emotion[0] != '\0' &&
                      strcmp(wm->dominant_emotion, "neutral") != 0;
    bool tom_signal = (wm->tom.user_thinks_we_are[0] &&
                       strcmp(wm->tom.user_thinks_we_are, "unknown") != 0) ||
                      (wm->tom.user_expects_we_can[0] &&
                       strcmp(wm->tom.user_expects_we_can, "unknown") != 0) ||
                      (wm->tom.user_expects_we_cannot[0] &&
                       strcmp(wm->tom.user_expects_we_cannot, "unknown") != 0);
    bool any = wm->entities_count > 0 || wm->relations_count > 0 || wm->goals_count > 0 ||
               wm->negatives_count > 0 || wm->recent_topics_count > 0 || emo_signal ||
               tom_signal;
    if (!any) {
        hu_world_model_free(alloc, wm);
        return HU_OK;
    }

    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    bool ok = true;

    ok = ok && buf_append(alloc, &buf, &blen, &bcap, "## What I know about this conversation\n",
                          strlen("## What I know about this conversation\n"));

    if (wm->entities_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Known entities:\n", 16);
        size_t ent_cap = wm->entities_count > 8 ? 8 : wm->entities_count;
        for (size_t i = 0; i < ent_cap; i++) {
            const char *name = wm->entities[i].name ? wm->entities[i].name : "?";
            const char *etype = hu_entity_type_to_string(wm->entities[i].type);
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s (%s)\n", name, etype);
        }
    }
    if (wm->relations_count > 0 && wm->entities_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Key relationships:\n", 19);
        size_t rel_cap = wm->relations_count > 6 ? 6 : wm->relations_count;
        for (size_t i = 0; i < rel_cap; i++) {
            const char *src = wm_entity_name_by_id(wm, wm->relations[i].source_id);
            const char *tgt = wm_entity_name_by_id(wm, wm->relations[i].target_id);
            const char *rtype = hu_relation_type_to_string(wm->relations[i].type);
            if (src && tgt)
                ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s %s %s\n", src, rtype, tgt);
        }
    }

    if (wm->goals_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Active goals:\n", 14);
        for (size_t i = 0; i < wm->goals_count && i < 8; i++) {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s\n", wm->goals[i].text);
        }
    }
    if (wm->negatives_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Avoid:\n", 7);
        size_t shown = wm->negatives_count > 6 ? 6 : wm->negatives_count;
        for (size_t i = 0; i < shown; i++) {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s%s%s\n", wm->negatives[i].text,
                                   wm->negatives[i].reason[0] ? " — " : "",
                                   wm->negatives[i].reason[0] ? wm->negatives[i].reason : "");
        }
    }
    if (tom_signal) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Theory of mind (what they think of me):\n",
                              strlen("Theory of mind (what they think of me):\n"));
        if (wm->tom.user_thinks_we_are[0] &&
            strcmp(wm->tom.user_thinks_we_are, "unknown") != 0)
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- They see me as: %s\n",
                                   wm->tom.user_thinks_we_are);
        if (wm->tom.user_expects_we_can[0] &&
            strcmp(wm->tom.user_expects_we_can, "unknown") != 0)
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- They expect I can: %s\n",
                                   wm->tom.user_expects_we_can);
        if (wm->tom.user_expects_we_cannot[0] &&
            strcmp(wm->tom.user_expects_we_cannot, "unknown") != 0)
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- They expect I cannot: %s\n",
                                   wm->tom.user_expects_we_cannot);
    }
    if (wm->recent_topics_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Recent topics: ", 15);
        for (size_t i = 0; i < wm->recent_topics_count && i < 10; i++) {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "%s%s", i == 0 ? "" : ", ",
                                   wm->recent_topics[i]);
        }
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "\n", 1);
    }
    if (emo_signal) {
        ok = ok && buf_appendf(alloc, &buf, &blen, &bcap,
                               "Recent emotional tone: %s (arousal %.2f, valence %.2f)\n",
                               wm->dominant_emotion, (double)wm->arousal, (double)wm->valence);
    }

    hu_world_model_free(alloc, wm);

    if (!ok) {
        if (buf)
            alloc->free(alloc->ctx, buf, bcap);
        return HU_ERR_OUT_OF_MEMORY;
    }

    *out_text = buf;
    *out_len = blen;
    return HU_OK;
}

/* W11 self-RAG bridge internal implementation.
 *
 * Prefers the atomic backend for SOFT/STRICT/INLINE modes — it decomposes
 * the draft into noun-phrase atomic claims and verifies each against the
 * memory graph, providing finer-grained abstention. Falls back to the
 * heuristic backend if atomic construction fails (defensive). When
 * `provider` is non-NULL and mode is STRICT, the atomic backend uses the
 * provider for real LLM-backed claim verification. */
static hu_error_t self_rag_verify_impl(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                       hu_provider_t *provider,
                                       const char *contact_id, size_t contact_id_len,
                                       const char *draft, size_t draft_len, int mode,
                                       int64_t now_ms,
                                       hu_w11_outcome_t *out_outcome, size_t *out_claims_total,
                                       size_t *out_claims_flagged, char **out_modified,
                                       size_t *out_modified_len) {
    if (out_outcome)
        *out_outcome = HU_W11_OUTCOME_SUPPORTED;
    if (out_claims_total)
        *out_claims_total = 0;
    if (out_claims_flagged)
        *out_claims_flagged = 0;
    if (out_modified)
        *out_modified = NULL;
    if (out_modified_len)
        *out_modified_len = 0;
    if (!facade || !facade->m || !alloc || !contact_id || contact_id_len == 0 || !draft ||
        draft_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (now_ms == 0)
        now_ms = (int64_t)time(NULL) * 1000;

    /* OFF: no-op fast path. */
    if (mode == HU_VERIFY_OFF) {
        if (out_outcome)
            *out_outcome = HU_W11_OUTCOME_SUPPORTED;
        return HU_OK;
    }

    /* Prefer atomic backend (noun-phrase decomposition, per-claim scoring).
     * Fall back to heuristic if construction fails. The inline backend is
     * reserved for providers that emit control tokens mid-stream.
     * Pass the provider through so the atomic backend can use it for
     * real LLM-backed claim verification in STRICT mode. */
    hu_self_rag_t r = {0};
    hu_error_t e = hu_self_rag_atomic(facade->m, provider, &r);
    if (e != HU_OK)
        e = hu_self_rag_heuristic(facade->m, &r);
    if (e != HU_OK)
        return e;

    hu_world_model_t *wm = NULL;
    /* Best-effort world-model load. The verifier handles wm == NULL. */
    (void)hu_world_model_load(facade->m, alloc, contact_id, contact_id_len, now_ms, &wm);

    hu_self_rag_request_t req = {
        .wm = wm,
        .contact_id = contact_id,
        .contact_id_len = contact_id_len,
        .draft = draft,
        .draft_len = draft_len,
        .mode = (hu_verify_mode_t)mode,
        .abstain_threshold = 0.3f,
        .now_ms = now_ms,
    };
    hu_self_rag_response_t resp;
    memset(&resp, 0, sizeof(resp));
    e = hu_self_rag_verify(&r, alloc, &req, &resp);

    if (wm)
        hu_world_model_free(alloc, wm);

    if (e != HU_OK) {
        hu_self_rag_close(&r);
        return e;
    }

    if (out_outcome) {
        switch (resp.outcome) {
        case HU_SELF_RAG_SUPPORTED:
            *out_outcome = HU_W11_OUTCOME_SUPPORTED;
            break;
        case HU_SELF_RAG_HEDGED:
            *out_outcome = HU_W11_OUTCOME_HEDGED;
            break;
        case HU_SELF_RAG_REWRITTEN:
            *out_outcome = HU_W11_OUTCOME_REWRITTEN;
            break;
        case HU_SELF_RAG_ABSTAINED:
            *out_outcome = HU_W11_OUTCOME_ABSTAINED;
            break;
        }
    }
    if (resp.outcome == HU_SELF_RAG_ABSTAINED && contact_id && contact_id_len > 0) {
        hu_negative_memory_t nm;
        memset(&nm, 0, sizeof(nm));
        size_t draft_cap = sizeof(nm.text) - 10;
        if (draft_len > draft_cap) draft_len = draft_cap;
        snprintf(nm.text, sizeof(nm.text), "Refused: %.*s",
                 (int)draft_len, draft);
        snprintf(nm.scope, sizeof(nm.scope), "topic");
        snprintf(nm.reason, sizeof(nm.reason), "self-rag abstention");
        nm.belief = hu_belief_init(0.6f, "self-rag", now_ms);
        nm.created_at = now_ms;
        int64_t nm_id = 0;
        (void)hu_negative_memory_add_facade(facade->m, contact_id, contact_id_len, &nm, &nm_id);
    }

    if (out_claims_total)
        *out_claims_total = resp.claims_count;
    if (out_claims_flagged) {
        size_t flagged = 0;
        for (size_t i = 0; i < resp.claims_count; i++) {
            /* Reuse the abstain threshold to mark "low support" claims. */
            if (resp.claims[i].support.mean < req.abstain_threshold)
                flagged++;
        }
        *out_claims_flagged = flagged;
    }

    /* Update relation beliefs based on verification outcome. Supported
     * claims reinforce confidence; unsupported claims weaken it. We walk
     * relations for the contact and apply a single Bayesian update per
     * relation so the posterior evolves with each verification pass. */
    if (resp.claims_count > 0 && contact_id && contact_id_len > 0 && facade->m) {
        float obs = (resp.outcome == HU_SELF_RAG_SUPPORTED) ? 0.9f : 0.3f;
        const char *src = (resp.outcome == HU_SELF_RAG_SUPPORTED) ? "self-rag-confirm"
                                                                    : "self-rag-disconfirm";
        hu_memory_query_t bq;
        memset(&bq, 0, sizeof(bq));
        bq.kind = HU_MEM_RELATION;
        bq.contact_id = contact_id;
        bq.contact_id_len = contact_id_len;
        bq.variant = HU_MEMORY_QUERY_AUTO;
        bq.as.window.limit = 16;
        hu_memory_record_t *brecs = NULL;
        size_t bn = 0;
        if (hu_memory_facade_read(facade->m, &bq, alloc, &brecs, &bn) == HU_OK && bn > 0) {
            for (size_t bi = 0; bi < bn; bi++) {
                const hu_memory_relation_row_t *br =
                    (const hu_memory_relation_row_t *)brecs[bi].payload;
                if (!br)
                    continue;
                float rm = 0.0f, rv = 0.0f;
                if (hu_memory_facade_get_relation_belief(facade->m, br->id, &rm, &rv) != HU_OK)
                    continue;
                hu_belief_t bp = {.mean = rm, .variance = rv};
                hu_belief_t bu = hu_belief_update(&bp, obs, src, now_ms);
                hu_memory_facade_set_relation_belief(facade->m, br->id, bu.mean, bu.variance, now_ms);
            }
            hu_memory_facade_records_free(facade->m, alloc, brecs, bn);
        }
    }

    /* Surface either the rewritten draft (HEDGED/REWRITTEN) or the
     * deterministic refusal template (ABSTAINED) through `out_modified`.
     * The agent loop's replacement path treats both as "swap the response
     * with this string" — semantically distinct (a hedge is the same
     * answer with a caveat; a refusal is a different answer entirely)
     * but mechanically the same buffer transfer. We unify them here so
     * `agent_turn.c` doesn't need a third branch.
     *
     * Heuristic backend leaves `resp.refusal_text` empty on ABSTAINED;
     * we render the deterministic UNKNOWN_FACT template in that case so
     * the user always sees an honest "I don't know" instead of the
     * unverified draft. */
    if (out_modified && out_modified_len) {
        const char *src = NULL;
        size_t src_len = 0;
        if (resp.outcome == HU_SELF_RAG_ABSTAINED) {
            char tmpl[256];
            if (resp.refusal_text[0] != '\0') {
                src = resp.refusal_text;
                src_len = strnlen(resp.refusal_text, sizeof(resp.refusal_text));
            } else {
                hu_self_rag_render_refusal(HU_REFUSAL_UNKNOWN_FACT, tmpl, sizeof(tmpl));
                src = tmpl;
                src_len = strnlen(tmpl, sizeof(tmpl));
            }
            if (src_len > 0) {
                char *copy = (char *)alloc->alloc(alloc->ctx, src_len + 1);
                if (copy) {
                    memcpy(copy, src, src_len);
                    copy[src_len] = '\0';
                    *out_modified = copy;
                    *out_modified_len = src_len;
                }
            }
        } else if (resp.draft_modified) {
            src_len = strnlen(resp.modified_draft, sizeof(resp.modified_draft));
            char *copy = (char *)alloc->alloc(alloc->ctx, src_len + 1);
            if (copy) {
                memcpy(copy, resp.modified_draft, src_len);
                copy[src_len] = '\0';
                *out_modified = copy;
                *out_modified_len = src_len;
            }
        }
    }
    hu_self_rag_close(&r);
    return HU_OK;
}

/* Public entrypoint: no provider. Preserves the historical API. */
hu_error_t hu_w11_self_rag_verify(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                  const char *contact_id, size_t contact_id_len,
                                  const char *draft, size_t draft_len, int mode, int64_t now_ms,
                                  hu_w11_outcome_t *out_outcome, size_t *out_claims_total,
                                  size_t *out_claims_flagged, char **out_modified,
                                  size_t *out_modified_len) {
    return self_rag_verify_impl(facade, alloc, NULL,
                                contact_id, contact_id_len,
                                draft, draft_len, mode, now_ms,
                                out_outcome, out_claims_total,
                                out_claims_flagged, out_modified,
                                out_modified_len);
}

/* Public entrypoint: with provider for LLM-backed claim verification. */
hu_error_t hu_w11_self_rag_verify_with_provider(
    hu_w7_facade_t *facade, hu_allocator_t *alloc,
    hu_provider_t *provider,
    const char *contact_id, size_t contact_id_len,
    const char *draft, size_t draft_len, int mode, int64_t now_ms,
    hu_w11_outcome_t *out_outcome, size_t *out_claims_total,
    size_t *out_claims_flagged, char **out_modified,
    size_t *out_modified_len) {
    return self_rag_verify_impl(facade, alloc, provider,
                                contact_id, contact_id_len,
                                draft, draft_len, mode, now_ms,
                                out_outcome, out_claims_total,
                                out_claims_flagged, out_modified,
                                out_modified_len);
}

/* ── W12 goal-conditioned planner recall bridge ───────────────────────────
 *
 * Loads the contact's world model, plans via the goal-conditioned (PageRank)
 * backend, then executes through hu_planner_execute so verify_after flags
 * are honored. The executor returns payload-stripped summaries; a second
 * read pass re-fetches with payloads, filtering to the verified set, and
 * formats results into a markdown text block for prompt injection. */

/* Build a planner using the best available backend.
 *
 * Order: LLM (if `provider` non-NULL and HU_IS_TEST unset) → goal-conditioned
 * (PageRank-seeded over the contact's graph) → heuristic (deterministic
 * goal-verb scan). Returns HU_OK on any successful selection and writes the
 * selected backend name to *out_backend (borrowed string literal).
 *
 * On non-OK return `*out` is unchanged and the caller MUST NOT call
 * hu_planner_close. */
static hu_error_t select_planner_backend(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                         hu_provider_t *provider,
                                         const char *model, size_t model_len,
                                         hu_planner_t *out,
                                         const char **out_backend) {
    *out_backend = "none";

    /* LLM backend: requires a provider and (in production) a working
     * chat_with_system entrypoint. Under HU_IS_TEST the LLM planner
     * forces the deterministic fallback, but we still take this branch
     * so the configure path is exercised. */
    if (provider && provider->vtable && provider->vtable->chat_with_system) {
        if (hu_planner_llm(provider, out) == HU_OK) {
            hu_planner_llm_configure(alloc, model, model_len);
            *out_backend = "llm";
            return HU_OK;
        }
    }

    /* Goal-conditioned (HippoRAG-style) backend: needs the facade. */
    if (hu_planner_goal_conditioned(m, alloc, out) == HU_OK) {
        *out_backend = "goal-conditioned";
        return HU_OK;
    }

    /* Heuristic: always works. */
    if (hu_planner_heuristic(out) == HU_OK) {
        *out_backend = "heuristic";
        return HU_OK;
    }
    return HU_ERR_NOT_SUPPORTED;
}

static hu_error_t planner_recall_impl(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                      hu_provider_t *provider,
                                      const char *model, size_t model_len,
                                      const char *contact_id, size_t contact_id_len,
                                      const char *query, size_t query_len,
                                      size_t limit, size_t max_chars,
                                      char **out_text, size_t *out_len) {
    if (!facade || !facade->m || !alloc || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

    if (limit == 0) limit = 5;
    if (max_chars == 0) max_chars = 4000;

    /* Load the world model so the goal-conditioned and LLM backends can
     * both ground their plan in the contact's entity graph + state. */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    hu_world_model_t *wm = NULL;
    if (contact_id && contact_id_len > 0)
        (void)hu_world_model_load(facade->m, alloc, contact_id, contact_id_len, now_ms, &wm);

    hu_planner_t planner;
    memset(&planner, 0, sizeof(planner));
    const char *backend_name = "none";
    hu_error_t err = select_planner_backend(facade->m, alloc, provider,
                                            model, model_len, &planner,
                                            &backend_name);
    if (err != HU_OK) {
        if (wm) hu_world_model_free(alloc, wm);
        return err;
    }
    (void)backend_name; /* available for future telemetry */

    hu_retrieval_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    err = hu_planner_plan(&planner, query, query_len, wm, &plan);

    if (wm)
        hu_world_model_free(alloc, wm);

    if (err != HU_OK) {
        hu_planner_close(&planner);
        return err;
    }

    /* Stamp contact_id on each step for scoped reads. */
    for (size_t i = 0; i < plan.steps_count; i++) {
        if (contact_id && contact_id_len > 0) {
            plan.steps[i].query.contact_id = contact_id;
            plan.steps[i].query.contact_id_len = contact_id_len;
        }
    }

    /* Route through hu_planner_execute so verify_after flags fire the W8
     * confidence filter. The heuristic self-RAG handle enables the filter
     * without requiring an embedder. Records come back without payloads;
     * we re-fetch below. */
    hu_self_rag_t rag;
    memset(&rag, 0, sizeof(rag));
    hu_self_rag_t *rag_ptr = NULL;
    if (hu_self_rag_heuristic(facade->m, &rag) == HU_OK)
        rag_ptr = &rag;

    hu_memory_record_t *verified = NULL;
    size_t verified_count = 0;
    err = hu_planner_execute(facade->m, rag_ptr, &plan, alloc, &verified, &verified_count);

    if (rag_ptr)
        hu_self_rag_close(rag_ptr);
    hu_planner_close(&planner);

    if (err != HU_OK && verified_count == 0)
        return err;
    if (verified_count == 0) {
        hu_planner_records_free(alloc, verified, 0);
        return HU_OK;
    }

    /* Re-read plan steps with payloads, filtering to the verified set.
     * hu_planner_execute strips payloads by design; we need entity names
     * and relation context for prompt rendering. */
    char *buf = NULL;
    size_t buf_len = 0;
    size_t buf_cap = 0;

    size_t total_records = 0;

    for (size_t i = 0; i < plan.steps_count && total_records < limit && buf_len < max_chars; i++) {
        hu_retrieval_step_t *step = &plan.steps[i];
        hu_memory_record_t *recs = NULL;
        size_t n = 0;
        hu_error_t re = hu_memory_facade_read(facade->m, &step->query, alloc, &recs, &n);
        if (re != HU_OK || n == 0) continue;

        for (size_t j = 0; j < n && total_records < limit && buf_len < max_chars; j++) {
            hu_memory_record_t *rec = &recs[j];

            /* Only include records that passed verification. */
            bool in_verified = false;
            for (size_t k = 0; k < verified_count; k++) {
                if (verified[k].id == rec->id && verified[k].kind == rec->kind) {
                    in_verified = true;
                    break;
                }
            }
            if (!in_verified) continue;

            const char *name = NULL;
            size_t name_len = 0;
            const char *detail = NULL;
            size_t detail_len = 0;

            if (rec->kind == HU_MEM_ENTITY && rec->payload) {
                hu_memory_entity_row_t *ent = (hu_memory_entity_row_t *)rec->payload;
                name = ent->name;
                name_len = ent->name_len;
                detail = ent->metadata_json;
                detail_len = detail ? strlen(detail) : 0;
            } else if (rec->kind == HU_MEM_RELATION && rec->payload) {
                hu_memory_relation_row_t *rel = (hu_memory_relation_row_t *)rec->payload;
                name = "relation";
                name_len = 8;
                detail = rel->context;
                detail_len = rel->context_len;
            } else {
                continue;
            }

            if (!name) name = "memory";
            if (!name_len) name_len = 6;

            size_t overhead = 15 + name_len + 2;
            size_t block_len = overhead + detail_len;
            if (buf_len + block_len > max_chars) {
                size_t remain = max_chars - buf_len;
                if (remain <= overhead) break;
                detail_len = remain - overhead;
                block_len = remain;
            }

            size_t needed = buf_len + block_len + 1;
            if (needed > buf_cap) {
                size_t newcap = buf_cap == 0 ? 512 : buf_cap * 2;
                if (newcap < needed) newcap = needed;
                void *p = alloc->realloc(alloc->ctx, buf, buf_cap, newcap);
                if (!p) {
                    hu_memory_facade_records_free(facade->m, alloc, recs, n);
                    if (buf) alloc->free(alloc->ctx, buf, buf_cap);
                    hu_planner_records_free(alloc, verified, verified_count);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                buf = p;
                buf_cap = newcap;
            }

            memcpy(buf + buf_len, "### Memory: ", 12);
            buf_len += 12;
            memcpy(buf + buf_len, name, name_len);
            buf_len += name_len;
            buf[buf_len++] = '\n';
            if (detail && detail_len > 0) {
                memcpy(buf + buf_len, detail, detail_len);
                buf_len += detail_len;
            }
            buf[buf_len++] = '\n';
            buf[buf_len++] = '\n';

            total_records++;
        }
        hu_memory_facade_records_free(facade->m, alloc, recs, n);
    }

    hu_planner_records_free(alloc, verified, verified_count);

    if (buf && buf_len > 0) {
        buf[buf_len] = '\0';
        *out_text = buf;
        *out_len = buf_len;
    } else {
        if (buf) alloc->free(alloc->ctx, buf, buf_cap);
    }
    return HU_OK;
}

/* Public entrypoint: no provider, no LLM. Preserves the historical API. */
hu_error_t hu_w12_planner_recall(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                 const char *contact_id, size_t contact_id_len,
                                 const char *query, size_t query_len,
                                 size_t limit, size_t max_chars,
                                 char **out_text, size_t *out_len) {
    return planner_recall_impl(facade, alloc, /*provider=*/NULL, NULL, 0,
                                contact_id, contact_id_len, query, query_len,
                                limit, max_chars, out_text, out_len);
}

/* Public entrypoint: routes through the LLM backend when a provider is
 * available. Falls through to goal-conditioned / heuristic on any failure
 * or under HU_IS_TEST. */
hu_error_t hu_w12_planner_recall_with_provider(
    hu_w7_facade_t *facade, hu_allocator_t *alloc,
    struct hu_provider *provider, const char *model, size_t model_len,
    const char *contact_id, size_t contact_id_len,
    const char *query, size_t query_len,
    size_t limit, size_t max_chars,
    char **out_text, size_t *out_len) {
    return planner_recall_impl(facade, alloc, provider, model, model_len,
                                contact_id, contact_id_len, query, query_len,
                                limit, max_chars, out_text, out_len);
}

/* ── W14 sleep-time compute scheduler bridge (FIX 13) ─────────────────────
 *
 * Owns a `hu_scheduler_t *` and a borrowed reference to the facade's
 * `hu_memory_facade_t *`. The scheduler does NOT take ownership of the memory
 * handle (it just uses it for SQLite + dispatch). On close we destroy
 * the scheduler before the facade so the per-tick SQL handle stays
 * valid through the last tick.
 *
 * The counterfactual-rehearsal runner is registered at open() so the
 * daemon can enqueue jobs without knowing which kind to register. Other
 * runners (AUTODREAM_*, KV_CACHE_*, LORA_TRAINING) stay as the no-op
 * defaults the scheduler installs at open() — they will be wired in
 * follow-up commits as their dependencies (W13 adapter loading, W10
 * eviction policy) land. */

struct hu_w14_scheduler {
    hu_scheduler_t *s;
};

hu_error_t hu_w14_scheduler_open(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                 hu_w14_scheduler_t **out_sched) {
    if (out_sched)
        *out_sched = NULL;
    if (!facade || !facade->m || !alloc || !out_sched)
        return HU_ERR_INVALID_ARGUMENT;

    hu_w14_scheduler_t *w = (hu_w14_scheduler_t *)alloc->alloc(alloc->ctx, sizeof(*w));
    if (!w)
        return HU_ERR_OUT_OF_MEMORY;
    w->s = NULL;
    hu_error_t e = hu_scheduler_open(alloc, facade->m, &w->s);
    if (e != HU_OK) {
        alloc->free(alloc->ctx, w, sizeof(*w));
        return e;
    }
    /* Wire the runners that this commit ships. The KV-cache + LoRA +
     * belief-reverify runners need caller-provided context (cache
     * handle, learner, etc.) so they're registered separately via the
     * bridge helpers below. The counterfactual + autodream runners are
     * context-free and always available. */
    (void)hu_scheduler_register_runner(w->s, HU_JOB_COUNTERFACTUAL_REHEARSAL,
                                       hu_counterfactual_rehearsal_runner, NULL);
    /* AutoDream: same C function handles all three kinds; spec->kind
     * inside the runner picks which phase fires. */
    (void)hu_scheduler_register_runner(w->s, HU_JOB_AUTODREAM_QUARANTINE,
                                       hu_autodream_runner, NULL);
    (void)hu_scheduler_register_runner(w->s, HU_JOB_AUTODREAM_COMMUNITY,
                                       hu_autodream_runner, NULL);
    (void)hu_scheduler_register_runner(w->s, HU_JOB_AUTODREAM_DECAY,
                                       hu_autodream_runner, NULL);
    /* Belief reverification: pure DB-side, no caller context needed,
     * defaults are sane (30 day age, 64 rows/tick). Daemon overrides
     * via hu_w14_scheduler_register_belief_reverify if it wants to
     * pin a contact filter or surface counters. */
    (void)hu_scheduler_register_runner(w->s, HU_JOB_BELIEF_REVERIFICATION,
                                       hu_belief_reverify_runner, NULL);
    /* KV cache + LoRA training: stay as no-ops until the daemon binds
     * them via the helpers below. */
    *out_sched = w;
    return HU_OK;
}

hu_error_t hu_w14_scheduler_register_lora_runner(hu_w14_scheduler_t *s,
                                                 hu_lora_runner_ctx_t *ctx) {
    if (!s || !s->s || !ctx)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_scheduler_register_runner(s->s, HU_JOB_LORA_TRAINING,
                                        hu_lora_training_runner, ctx);
}

hu_error_t hu_w14_scheduler_register_kv_prewarm_runner(hu_w14_scheduler_t *s,
                                                       hu_kv_cache_manager_t *mgr) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e1 = hu_scheduler_register_runner(s->s, HU_JOB_KV_CACHE_EVICTION,
                                                 hu_kv_prewarm_runner, mgr);
    if (e1 != HU_OK)
        return e1;
    return hu_scheduler_register_runner(s->s, HU_JOB_KV_CACHE_WARMING,
                                        hu_kv_prewarm_runner, mgr);
}

hu_error_t hu_w14_scheduler_register_belief_reverify(hu_w14_scheduler_t *s,
                                                     hu_belief_reverify_ctx_t *ctx) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_scheduler_register_runner(s->s, HU_JOB_BELIEF_REVERIFICATION,
                                        hu_belief_reverify_runner, ctx);
}

/* Allow the LoRA training runner to fire its KV-warm follow-up through
 * the same bridge handle the daemon owns. Without this, the runner has
 * no scheduler reference (it's a `hu_scheduler_t *`, not the bridge
 * type). Daemon callers wire `ctx->scheduler` to the unwrapped pointer
 * via this helper. */
hu_scheduler_t *hu_w14_scheduler_inner(hu_w14_scheduler_t *s) {
    return s ? s->s : NULL;
}

void hu_w14_scheduler_close(hu_w14_scheduler_t *s, hu_allocator_t *alloc) {
    if (!s)
        return;
    if (s->s)
        hu_scheduler_close(s->s, alloc);
    if (alloc)
        alloc->free(alloc->ctx, s, sizeof(*s));
}

hu_error_t hu_w14_scheduler_tick(hu_w14_scheduler_t *s, int64_t now_ms) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    if (now_ms == 0)
        now_ms = (int64_t)time(NULL) * 1000;
    return hu_scheduler_tick(s->s, now_ms);
}

hu_error_t hu_w14_scheduler_enqueue_autodream(hu_w14_scheduler_t *s, int64_t now_ms,
                                              int budget_ms) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    /* AutoDream is global (no contact_id). Each phase becomes its own
     * job so the scheduler can pace them and so a single phase failure
     * doesn't poison the others. Quarantine review is the most
     * sensitive (it can drop facts) so it runs at higher priority. */
    static const struct {
        hu_job_kind_t kind;
        int priority;
    } phases[] = {
        {HU_JOB_AUTODREAM_QUARANTINE, 1},
        {HU_JOB_AUTODREAM_COMMUNITY, 0},
        {HU_JOB_AUTODREAM_DECAY, 0},
    };
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        hu_job_spec_t job;
        memset(&job, 0, sizeof(job));
        job.kind = phases[i].kind;
        job.priority = phases[i].priority;
        job.budget_ms = budget_ms > 0 ? budget_ms : 60000; /* 1 min per phase default */
        job.requires_idle = false;
        job.requires_ac_power = false;
        job.earliest_at = now_ms; /* 0 = ASAP */
        hu_error_t e = hu_scheduler_enqueue(s->s, &job);
        if (e != HU_OK)
            return e;
    }
    return HU_OK;
}

hu_error_t hu_w14_scheduler_enqueue_persona_evolver(hu_w14_scheduler_t *s,
                                                    int64_t now_ms,
                                                    int budget_ms) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    hu_job_spec_t job;
    memset(&job, 0, sizeof(job));
    job.kind = HU_JOB_PERSONA_EVOLVER;
    job.priority = 0;
    job.budget_ms = budget_ms > 0 ? budget_ms : 120000;
    job.requires_idle = true;
    job.requires_ac_power = false;
    job.earliest_at = now_ms;
    return hu_scheduler_enqueue(s->s, &job);
}

hu_error_t hu_w14_scheduler_enqueue_lora(hu_w14_scheduler_t *s, int64_t now_ms,
                                         int budget_ms) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    hu_job_spec_t job;
    memset(&job, 0, sizeof(job));
    job.kind = HU_JOB_LORA_TRAINING;
    job.priority = 0;
    job.budget_ms = budget_ms > 0 ? budget_ms : 300000;
    job.requires_idle = true;
    job.requires_ac_power = false;
    job.earliest_at = now_ms;
    return hu_scheduler_enqueue(s->s, &job);
}

hu_error_t hu_w14_scheduler_register_training_data_runner(hu_w14_scheduler_t *s,
                                                          hu_training_data_runner_ctx_t *ctx) {
    if (!s || !s->s || !ctx)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_scheduler_register_runner(s->s, HU_JOB_TRAINING_DATA_EXTRACT,
                                        hu_training_data_runner, ctx);
}

hu_error_t hu_w14_scheduler_enqueue_training_data_extract(hu_w14_scheduler_t *s,
                                                          int64_t now_ms,
                                                          int budget_ms) {
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    hu_job_spec_t job;
    memset(&job, 0, sizeof(job));
    job.kind = HU_JOB_TRAINING_DATA_EXTRACT;
    job.priority = 0;
    job.budget_ms = budget_ms > 0 ? budget_ms : 120000;
    job.requires_idle = true;
    job.requires_ac_power = false;
    job.earliest_at = now_ms;
    return hu_scheduler_enqueue(s->s, &job);
}

hu_error_t hu_w14_scheduler_enqueue_counterfactual(hu_w14_scheduler_t *s,
                                                   const char *contact_id,
                                                   size_t contact_id_len,
                                                   int budget_ms) {
    if (!s || !s->s || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_job_spec_t job;
    memset(&job, 0, sizeof(job));
    job.kind = HU_JOB_COUNTERFACTUAL_REHEARSAL;
    job.contact_id = contact_id;
    job.contact_id_len = contact_id_len;
    job.priority = 0;
    job.budget_ms = budget_ms > 0 ? budget_ms : 50;
    /* Idle-only and not battery-gated: counterfactual rehearsal is light
     * enough to run on AC- or battery-power; readers can override later
     * by enqueuing their own spec directly through the bridge if needed. */
    job.requires_idle = false;
    job.requires_ac_power = false;
    return hu_scheduler_enqueue(s->s, &job);
}

hu_error_t hu_w14_scheduler_status(hu_w14_scheduler_t *s, size_t *out_jobs_pending,
                                   size_t *out_jobs_completed_today, int *out_battery_pct,
                                   int *out_on_ac_power) {
    if (out_jobs_pending)
        *out_jobs_pending = 0;
    if (out_jobs_completed_today)
        *out_jobs_completed_today = 0;
    if (out_battery_pct)
        *out_battery_pct = -1;
    if (out_on_ac_power)
        *out_on_ac_power = 1;
    if (!s || !s->s)
        return HU_ERR_INVALID_ARGUMENT;
    hu_scheduler_status_t st;
    memset(&st, 0, sizeof(st));
    hu_error_t e = hu_scheduler_status(s->s, &st);
    if (e != HU_OK)
        return e;
    if (out_jobs_pending)
        *out_jobs_pending = st.jobs_pending;
    if (out_jobs_completed_today)
        *out_jobs_completed_today = st.jobs_completed_today;
    if (out_battery_pct)
        *out_battery_pct = st.battery_pct;
    if (out_on_ac_power)
        *out_on_ac_power = st.on_ac_power ? 1 : 0;
    return HU_OK;
}

bool hu_w14_scheduler_status_path(char *out_path, size_t cap) {
    if (!out_path || cap == 0)
        return false;
    const char *home = getenv("HOME");
    if (!home || !*home)
        return false;
    int n = snprintf(out_path, cap, "%s/.human/scheduler.status", home);
    return n > 0 && (size_t)n < cap;
}

hu_error_t hu_w14_scheduler_status_save(hu_w14_scheduler_t *s) {
    if (!s)
        return HU_ERR_INVALID_ARGUMENT;
    char path[512];
    if (!hu_w14_scheduler_status_path(path, sizeof(path)))
        return HU_OK; /* HOME unset — best effort no-op like imessage */

    size_t pending = 0, completed = 0;
    int battery = -1, on_ac = 1;
    hu_error_t e = hu_w14_scheduler_status(s, &pending, &completed, &battery, &on_ac);
    if (e != HU_OK)
        return e;

    /* Atomic write via tempfile + rename so concurrent readers (`human ml
     * status`, `human doctor scheduler`) never see a partial document. Both
     * tools parse via `hu_scheduler_status_parse_json` (any key order).
     * Shape matches other ~/.human status JSON files for grep/jq. */
    char tmp[600];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp))
        return HU_OK;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return HU_OK;
    int64_t now = (int64_t)time(NULL);
    fprintf(f,
            "{\n"
            "  \"jobs_pending\": %zu,\n"
            "  \"jobs_completed_today\": %zu,\n"
            "  \"battery_pct\": %d,\n"
            "  \"on_ac_power\": %s,\n"
            "  \"updated_epoch\": %lld\n"
            "}\n",
            pending, completed, battery, on_ac ? "true" : "false", (long long)now);
    fclose(f);
    if (rename(tmp, path) != 0)
        (void)remove(tmp);
    return HU_OK;
}
