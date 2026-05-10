/* W11 — Self-RAG dispatcher + heuristic backend + refusal templates.
 *
 * Three backends share the hu_self_rag_vtable_t surface; this file holds the
 * dispatch helpers (hu_self_rag_verify, hu_self_rag_close), the deterministic
 * refusal-template renderer, and the heuristic backend that wraps v1's
 * `hu_response_verify`. Atomic and inline backends live in their own files.
 *
 * KISS notes:
 *   - The response is a fixed-size struct; no heap allocations are owned by
 *     the response itself. Backends use stack/temporary buffers internally.
 *   - The heuristic backend stores only `hu_memory_facade_t *`. The graph handle
 *     is fetched on each call; this matches W9/W10 patterns and avoids
 *     storing pointers that might be invalidated by the facade between
 *     verify() calls.
 */

#include "human/agent/self_rag.h"

#include "human/agent/response_verifier.h"
#include "human/core/error.h"
#include "human/memory/belief.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Refusal templates ────────────────────────────────────────────────────
 * These strings are the source of truth shared between the verifier and the
 * channel renderer. Tests assert exact equality so they cannot drift
 * silently. */

void hu_self_rag_render_refusal(hu_refusal_reason_t reason, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    const char *s;
    switch (reason) {
    case HU_REFUSAL_POLICY:
        s = "This is something I shouldn't say without more confidence.";
        break;
    case HU_REFUSAL_NEGATIVE_MEMORY_MATCH:
        s = "Based on what I know, I'd rather not weigh in here.";
        break;
    case HU_REFUSAL_LOW_CONFIDENCE:
        s = "I don't have enough memory to confirm this.";
        break;
    case HU_REFUSAL_UNKNOWN_FACT:
    default:
        s = "I don't have memory backing this. Want to tell me?";
        break;
    }
    snprintf(buf, cap, "%s", s);
}

/* ── Dispatch ─────────────────────────────────────────────────────────── */

hu_error_t hu_self_rag_verify(hu_self_rag_t *r, hu_allocator_t *alloc,
                              const hu_self_rag_request_t *req,
                              hu_self_rag_response_t *resp) {
    if (!r || !r->vt || !r->vt->verify || !req || !resp)
        return HU_ERR_INVALID_ARGUMENT;
    memset(resp, 0, sizeof(*resp));
    return r->vt->verify(r->ctx, alloc, req, resp);
}

void hu_self_rag_close(hu_self_rag_t *r) {
    if (!r || !r->vt)
        return;
    if (r->vt->deinit)
        r->vt->deinit(r->ctx);
    r->vt = NULL;
    r->ctx = NULL;
}

/* ── Heuristic backend ──────────────────────────────────────────────────
 * Wraps the v1 verifier verbatim. We do not duplicate its claim-extraction
 * or scoring logic; we translate its `hu_verifier_report_t` into the new
 * `hu_self_rag_response_t` shape. This is the fallback backend and is the
 * default until a provider opts in to inline mode. */

typedef struct heuristic_ctx {
    hu_memory_facade_t *m;
} heuristic_ctx_t;

/* Map a verifier_claim's score into a W8 belief posterior. We treat the
 * heuristic score as an observation with moderate prior weight: variance
 * follows the Bernoulli-style mean*(1-mean) used by hu_belief_init. */
static hu_belief_t belief_from_score(float score, const char *source, int64_t now) {
    /* Clamp into [0,1] defensively before handing to the belief layer. */
    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    return hu_belief_init(score, source, now);
}

static void prov_atom_from_receipt(const hu_provenance_receipt_t *r,
                                   hu_provenance_atom_t *out) {
    memset(out, 0, sizeof(*out));
    if (!r) return;
    snprintf(out->source, sizeof(out->source), "%s",
             r->source[0] ? r->source : "memory");
    out->observed_at = r->observed_at_ms;
    out->weight = r->confidence;
}

static hu_error_t heuristic_verify(void *vctx, hu_allocator_t *alloc,
                                    const hu_self_rag_request_t *req,
                                    hu_self_rag_response_t *resp) {
    heuristic_ctx_t *ctx = (heuristic_ctx_t *)vctx;
    if (!ctx || !alloc || !req || !resp)
        return HU_ERR_INVALID_ARGUMENT;
    if (!req->draft || req->draft_len == 0) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        return HU_OK;
    }

    /* OFF mode short-circuits to SUPPORTED with no work. Mirrors v1. */
    if (req->mode == HU_VERIFY_OFF) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        return HU_OK;
    }

    hu_verifier_config_t cfg = hu_verifier_default_config();
    /* The heuristic backend understands SOFT/STRICT; INLINE mode is mapped
     * to SOFT (heuristic can't do inline; falls back gracefully). */
    cfg.mode = (req->mode == HU_VERIFY_INLINE || req->mode == HU_VERIFY_SOFT)
                   ? HU_VERIFY_SOFT
                   : HU_VERIFY_STRICT;
    cfg.now_ms = req->now_ms;

    hu_verifier_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err = hu_response_verify(alloc, ctx->m, req->contact_id,
                                         req->contact_id_len, req->draft,
                                         req->draft_len, &cfg, &report);
    if (err != HU_OK)
        return err;

    /* Translate verifier claims → atomic claims. The heuristic backend has
     * no span tracking, so spans are zero. */
    size_t n = report.claims_extracted;
    if (n > 32) n = 32;
    resp->claims_count = n;
    int64_t now = req->now_ms;
    size_t flagged = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_verifier_claim_t *v = &report.claims[i];
        hu_atomic_claim_t *c = &resp->claims[i];
        memset(c, 0, sizeof(*c));
        snprintf(c->text, sizeof(c->text), "%s", v->text);
        c->span_start = 0;
        c->span_end = 0;
        c->support = belief_from_score(v->score, "heuristic", now);
        prov_atom_from_receipt(&v->receipt, &c->prov);
        c->fabricated = !v->supported && v->score < 0.2f;
        if (!v->supported) flagged++;
    }

    /* Outcome decision. The order matters:
     *   1. ABSTAINED beats HEDGED when the unsupported ratio crosses
     *      `abstain_threshold` (≥0.5 of claims unsupported by default).
     *   2. HEDGED when SOFT mode mutated the draft.
     *   3. REWRITTEN when STRICT mode mutated the draft.
     *   4. SUPPORTED otherwise. */
    float thr = req->abstain_threshold > 0.0f ? req->abstain_threshold : 0.5f;
    float ratio = n > 0 ? (float)flagged / (float)n : 0.0f;
    if (n > 0 && ratio >= thr) {
        resp->outcome = HU_SELF_RAG_ABSTAINED;
        hu_self_rag_render_refusal(HU_REFUSAL_UNKNOWN_FACT, resp->refusal_text,
                                    sizeof(resp->refusal_text));
        return HU_OK;
    }

    if (report.draft_modified) {
        snprintf(resp->modified_draft, sizeof(resp->modified_draft), "%s",
                 report.modified_draft);
        resp->draft_modified = true;
        resp->outcome = (cfg.mode == HU_VERIFY_STRICT) ? HU_SELF_RAG_REWRITTEN
                                                       : HU_SELF_RAG_HEDGED;
    } else {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
    }
    return HU_OK;
}

static void heuristic_deinit(void *vctx) {
    heuristic_ctx_t *ctx = (heuristic_ctx_t *)vctx;
    if (!ctx) return;
    free(ctx);
}

static hu_self_rag_vtable_t heuristic_vt = {
    .name = "heuristic",
    .verify = heuristic_verify,
    .deinit = heuristic_deinit,
};

hu_error_t hu_self_rag_heuristic(hu_memory_facade_t *m, hu_self_rag_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    heuristic_ctx_t *ctx = (heuristic_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->m = m;
    out->vt = &heuristic_vt;
    out->ctx = ctx;
    return HU_OK;
}
