/* W11 — Inline self-RAG backend (deterministic placeholder).
 *
 * Protocol (kept verbatim with the spec so a future provider integration can
 * adopt it without revising the parser):
 *
 *   <retrieve>QUERY</retrieve>      Mid-stream memory fetch hint. The
 *                                   placeholder records QUERY as a "lookup"
 *                                   atomic claim and strips the tags from
 *                                   the visible output.
 *   <critique>CLAIM</critique>      Marks CLAIM for verification. The
 *                                   placeholder records CLAIM as an atomic
 *                                   claim, scores it 0 (unknown) since we
 *                                   skip the graph round-trip in the
 *                                   placeholder, and strips the tags.
 *   <refuse>REASON</refuse>         Forces an immediate ABSTAINED outcome.
 *                                   REASON populates `refusal_text` (caller
 *                                   may localize). All subsequent control
 *                                   tokens after the first <refuse> are
 *                                   ignored, and the visible output is
 *                                   suppressed.
 *
 * The real implementation will register a provider stream callback that
 * intercepts these tokens as they arrive; this commit ships the
 * deterministic post-hoc parser so the rest of the pipeline (channel
 * rendering, eval harness, A/B tests) can be built and tested against
 * stable inputs.
 */

#include "human/agent/self_rag.h"

#include "human/agent/response_verifier.h"
#include "human/core/error.h"
#include "human/memory/belief.h"
#include "human/memory/corrective_rag.h"
#include "human/memory/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct inline_ctx {
    hu_memory_facade_t *m;
    /* `chat` is stored for the future provider streaming path. The
     * deterministic placeholder does not call into it. */
} inline_ctx_t;

/* Find the next opening tag that matches one of the known control names
 * starting at `from`. Returns the offset of the leading '<' or SIZE_MAX. On
 * a match, *tag_kind is set to one of "retrieve" / "critique" / "refuse"
 * and *open_end is the offset just past the closing '>' of the open tag. */
static size_t find_next_open_tag(const char *s, size_t len, size_t from,
                                  const char **tag_kind, size_t *open_end) {
    static const char *const k_names[] = {"retrieve", "critique", "refuse",
                                           NULL};
    size_t best = (size_t)-1;
    const char *best_name = NULL;
    size_t best_end = 0;
    for (size_t k = 0; k_names[k]; k++) {
        const char *n = k_names[k];
        size_t nl = strlen(n);
        size_t pl = nl + 2; /* '<' + name + '>' */
        if (len < pl || from > len - pl) continue;
        for (size_t i = from; i + pl <= len; i++) {
            if (s[i] != '<') continue;
            if (memcmp(s + i + 1, n, nl) != 0) continue;
            if (s[i + 1 + nl] != '>') continue;
            if (i < best) {
                best = i;
                best_name = n;
                best_end = i + pl;
            }
            break;
        }
    }
    if (tag_kind) *tag_kind = best_name;
    if (open_end) *open_end = best_end;
    return best;
}

/* Find the closing tag </name> starting at `from`. Returns the offset of
 * the leading '<' or SIZE_MAX if not found. */
static size_t find_close_tag(const char *s, size_t len, size_t from,
                              const char *name) {
    size_t nl = strlen(name);
    size_t pl = nl + 3; /* '<','/',name,'>' */
    if (len < pl || from > len - pl) return (size_t)-1;
    for (size_t i = from; i + pl <= len; i++) {
        if (s[i] != '<' || s[i + 1] != '/') continue;
        if (memcmp(s + i + 2, name, nl) != 0) continue;
        if (s[i + 2 + nl] != '>') continue;
        return i;
    }
    return (size_t)-1;
}

/* Append `src[0..len)` to dst, respecting the dst capacity. Returns the
 * number of bytes actually appended. */
static size_t append_clip(char *dst, size_t cap, size_t off, const char *src,
                           size_t len) {
    if (off >= cap) return 0;
    size_t avail = cap - 1 - off; /* leave room for nul */
    size_t copy = len < avail ? len : avail;
    if (copy > 0) memcpy(dst + off, src, copy);
    dst[off + copy] = '\0';
    return copy;
}

/* Capture text into a fixed-size atomic_claim text buffer. */
static void capture_claim_text(hu_atomic_claim_t *c, const char *src,
                                size_t len, int64_t base_off) {
    memset(c, 0, sizeof(*c));
    size_t copy = len < sizeof(c->text) - 1 ? len : sizeof(c->text) - 1;
    if (copy > 0) memcpy(c->text, src, copy);
    c->text[copy] = '\0';
    c->span_start = base_off;
    c->span_end = base_off + (int64_t)len;
}

/* W9 single-load helper — does the world-model snapshot carry an entity
 * whose name appears in the claim text?
 *
 * Case-insensitive substring match. Cheap (O(N * M) where N is wm
 * entities and M is claim length, both bounded). Used as a *supplementary*
 * support signal — it can only LIFT a weak SQL score, never lower it. The
 * W9 promise is "every consumer reads from the unified world model"; this
 * threads that read through the inline verifier without removing the SQL
 * fallback (which still owns relation-level grounding the WM doesn't
 * carry — see `world_model.c` where context/provenance is stripped). */
static bool inline_wm_entity_in_claim(const hu_world_model_t *wm,
                                       const char *claim, size_t claim_len) {
    if (!wm || !claim || claim_len == 0)
        return false;
    for (size_t i = 0; i < wm->entities_count; i++) {
        const hu_graph_entity_t *e = &wm->entities[i];
        if (!e->name)
            continue;
        size_t nl = strlen(e->name);
        if (nl < 2 || nl > claim_len)
            continue;
        for (size_t j = 0; j + nl <= claim_len; j++) {
            if (strncasecmp(claim + j, e->name, nl) == 0)
                return true;
        }
    }
    return false;
}

/* W11 negative-memory check — does the world-model's negative-memory list
 * contain an item whose text appears as a substring in the claim?
 *
 * Case-insensitive substring match mirroring inline_wm_entity_in_claim.
 * Returns the matching negative_memory item (for provenance tagging) or
 * NULL. When a match is found, the critique score is forced to 0 and
 * fabricated is set — the caller can then trigger NEGATIVE_MEMORY_MATCH
 * refusal in STRICT mode. */
static const hu_negative_memory_t *inline_wm_negative_match(
    const hu_world_model_t *wm, const char *claim, size_t claim_len) {
    if (!wm || !claim || claim_len == 0 || !wm->negatives)
        return NULL;
    for (size_t i = 0; i < wm->negatives_count; i++) {
        const hu_negative_memory_t *nm = &wm->negatives[i];
        size_t nl = strlen(nm->text);
        if (nl < 2 || nl > claim_len)
            continue;
        for (size_t j = 0; j + nl <= claim_len; j++) {
            if (strncasecmp(claim + j, nm->text, nl) == 0)
                return nm;
        }
    }
    return NULL;
}

/* Real scoring for `<critique>` claims.
 *
 * Wraps the claim into a single-sentence "draft" and runs it through the v1
 * verifier (`hu_response_verify`) configured with `max_claims=1`, which is
 * the same pattern the atomic backend uses. The resulting token-overlap
 * score becomes the support belief mean; sub-threshold scores flip
 * `c->fabricated` so a downstream caller can hedge or abstain.
 *
 * When the request supplies a W9 world-model snapshot AND the claim text
 * mentions one of the loaded entities, we bump weak scores to a soft floor
 * of 0.5. This is the "single-load" win: the verifier consumes the
 * already-loaded WM artifact instead of issuing a second round-trip for
 * entity discovery. The bump is monotone — never lowers the SQL score —
 * so existing supported claims remain supported.
 *
 * W11 negative-memory gate: after the v1 score, check whether any of the
 * world model's negative-memory items match the claim text. If so, force
 * score=0 / fabricated=true and tag the source as "inline-negative-mem".
 * This blocks the claim from being emitted in STRICT mode.
 *
 * Side note on `c->prov.source`: callers downstream (the channel renderer,
 * routing tests) rely on `prov.source` carrying the **tag kind** so they
 * can distinguish "this came from a <critique>" vs "this came from a
 * <retrieve>". We preserve that contract and stash the receipt source in
 * the belief's first provenance atom (the belief is the new home for
 * "where did the score come from"). */
static void inline_score_critique(hu_allocator_t *alloc, hu_memory_facade_t *m,
                                   const char *contact_id, size_t cid_len,
                                   int64_t now,
                                   const hu_world_model_t *wm,
                                   hu_atomic_claim_t *c) {
    if (!alloc || !m || !c || !c->text[0]) {
        c->support = hu_belief_init(0.0f, "inline-no-memory", now);
        c->fabricated = true;
        return;
    }

    char sentence[200];
    int n = snprintf(sentence, sizeof(sentence), "%s.", c->text);
    if (n <= 0 || (size_t)n >= sizeof(sentence)) {
        c->support = hu_belief_init(0.0f, "inline-no-claim", now);
        c->fabricated = true;
        return;
    }

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    cfg.confidence_threshold = 0.6f;
    cfg.max_claims = 1;

    hu_verifier_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err = hu_response_verify(alloc, m, contact_id, cid_len,
                                          sentence, (size_t)n, &cfg, &report);
    if (err != HU_OK || report.claims_extracted == 0) {
        c->support = hu_belief_init(0.0f, "inline-no-claim", now);
        c->fabricated = true;
        return;
    }

    float score = report.claims[0].score;

    /* W9 single-load: when the unified world model is supplied AND its
     * loaded entity set covers the claim, lift weak scores to a 0.5
     * floor. Strong SQL scores are preserved. */
    bool wm_lifted = false;
    if (wm && score < 0.5f && inline_wm_entity_in_claim(wm, c->text, strlen(c->text))) {
        score = 0.5f;
        wm_lifted = true;
    }

    /* W11: negative-memory gate. If the claim matches a world-model
     * negative-memory item, override the score to 0 and mark fabricated
     * regardless of the SQL/WM result. This is a hard veto. */
    const hu_negative_memory_t *neg_match =
        inline_wm_negative_match(wm, c->text, strlen(c->text));
    if (neg_match) {
        score = 0.0f;
        c->support = hu_belief_init(0.0f, "inline-negative-mem", now);
        c->fabricated = true;
        if (c->support.prov_count < 4) {
            hu_provenance_atom_t *p = &c->support.prov[c->support.prov_count++];
            snprintf(p->source, sizeof(p->source), "neg:%s", neg_match->scope);
            p->observed_at = neg_match->created_at;
            p->weight = 0.0f;
        }
        return;
    }

    c->support = hu_belief_init(score, wm_lifted ? "inline-wm-lift" : "inline-graph", now);
    c->fabricated = score < cfg.confidence_threshold;

    if (report.claims[0].receipt.source[0] && c->support.prov_count < 4) {
        hu_provenance_atom_t *p = &c->support.prov[c->support.prov_count++];
        snprintf(p->source, sizeof(p->source), "%s",
                 report.claims[0].receipt.source);
        p->observed_at = report.claims[0].receipt.observed_at_ms;
        p->weight = report.claims[0].receipt.confidence;
    }
}

/* Real scoring for `<retrieve>` claims.
 *
 * `<retrieve>QUERY</retrieve>` represents the model's request "memory,
 * tell me about QUERY." Unlike a `<critique>` (factual claim being
 * verified), this is a probe: the support score reflects "does memory
 * have anything actually relevant to QUERY for this contact."
 *
 * Implementation is grade-aware. We pull up to 16 relations for the
 * contact, then run each relation's `context` text through
 * `hu_crag_grade_document` (the same token-overlap grader the atomic
 * backend's STRICT-mode corrective-RAG path uses). Records grading
 * RELEVANT contribute their full `score` to the support sum; AMBIGUOUS
 * records contribute half; IRRELEVANT records contribute zero. The sum
 * is saturated at 1.0.
 *
 * Why grade-aware rather than the prior count-only heuristic: a contact
 * with 16 unrelated relations would have scored ~1.0 under the old code
 * even when the QUERY had nothing in memory. Now an unrelated retrieve
 * scores 0.0 (claim fabricated → STRICT mode can abstain) while a
 * specific retrieve with a single matching relation can score >= 0.5.
 *
 * Cost: O(N) grading calls for N <= 16, each is bounded token-overlap
 * scoring (no SQL, no allocation per grade). Empirically ~50-200 µs
 * for the full sweep on typical contacts. */
static void inline_score_retrieve(hu_allocator_t *alloc, hu_memory_facade_t *m,
                                   const char *contact_id, size_t cid_len,
                                   const char *query, int64_t now,
                                   hu_atomic_claim_t *c) {
    if (!alloc || !m || !contact_id || cid_len == 0) {
        c->support = hu_belief_init(0.0f, "inline-no-memory", now);
        c->fabricated = true;
        return;
    }

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_RELATION;
    q.variant = HU_MEMORY_QUERY_AUTO;
    q.contact_id = contact_id;
    q.contact_id_len = cid_len;
    q.as.window.limit = 16;

    hu_memory_record_t *recs = NULL;
    size_t n = 0;
    hu_error_t err = hu_memory_facade_read(m, &q, alloc, &recs, &n);
    if (err != HU_OK) {
        if (recs)
            hu_memory_facade_records_free(m, alloc, recs, n);
        c->support = hu_belief_init(0.0f, "inline-facade-err", now);
        c->fabricated = true;
        return;
    }

    float score = 0.0f;
    size_t relevant_count = 0;
    if (query && query[0] && n > 0) {
        size_t query_len = strlen(query);
        for (size_t i = 0; i < n; i++) {
            const hu_memory_relation_row_t *r =
                (const hu_memory_relation_row_t *)recs[i].payload;
            if (!r || !r->context || r->context_len == 0)
                continue;
            hu_rag_graded_doc_t grade = {0};
            if (hu_crag_grade_document(alloc, query, query_len,
                                        r->context, r->context_len,
                                        &grade) != HU_OK)
                continue;
            /* RELEVANT carries full weight, AMBIGUOUS half, IRRELEVANT zero.
             * The grader's `score` is already in [0, 1], so a single strong
             * relevant match can saturate. */
            float contrib = 0.0f;
            if (grade.relevance == HU_RAG_RELEVANT) {
                contrib = (float)grade.score;
                relevant_count++;
            } else if (grade.relevance == HU_RAG_AMBIGUOUS) {
                contrib = (float)grade.score * 0.5f;
            }
            score += contrib;
            if (score >= 1.0f) {
                score = 1.0f;
                break;
            }
        }
    } else if (n > 0) {
        /* Query was empty (e.g. `<retrieve></retrieve>`) — fall back to
         * the prior presence signal so empty-tag retrieves don't
         * immediately abstain. Saturates at 5 records → 1.0. */
        score = n >= 5 ? 1.0f : (float)n / 5.0f;
    }

    c->support = hu_belief_init(score,
                                 relevant_count > 0 ? "inline-probe-graded"
                                                    : "inline-probe", now);
    c->fabricated = (score < 0.2f);

    if (recs)
        hu_memory_facade_records_free(m, alloc, recs, n);
}

static hu_error_t inline_verify(void *vctx, hu_allocator_t *alloc,
                                 const hu_self_rag_request_t *req,
                                 hu_self_rag_response_t *resp) {
    inline_ctx_t *ctx = (inline_ctx_t *)vctx;
    if (!ctx || !alloc || !req || !resp)
        return HU_ERR_INVALID_ARGUMENT;
    if (!req->draft || req->draft_len == 0) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        return HU_OK;
    }
    if (req->mode == HU_VERIFY_OFF) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        snprintf(resp->modified_draft, sizeof(resp->modified_draft), "%.*s",
                 (int)req->draft_len, req->draft);
        return HU_OK;
    }

    /* Walk the draft, building the cleaned output and capturing claims. */
    char visible[2048];
    visible[0] = '\0';
    size_t vis_off = 0;
    size_t i = 0;
    bool refused = false;
    int64_t now = req->now_ms;

    while (i < req->draft_len) {
        const char *kind = NULL;
        size_t open_end = 0;
        size_t open_at = find_next_open_tag(req->draft, req->draft_len, i,
                                             &kind, &open_end);
        if (open_at == (size_t)-1) {
            /* No more tags; emit the rest verbatim. */
            vis_off += append_clip(visible, sizeof(visible), vis_off,
                                    req->draft + i, req->draft_len - i);
            break;
        }
        /* Emit text up to the tag. */
        vis_off += append_clip(visible, sizeof(visible), vis_off,
                                req->draft + i, open_at - i);

        size_t close_at = find_close_tag(req->draft, req->draft_len,
                                          open_end, kind);
        if (close_at == (size_t)-1) {
            /* Malformed: unterminated tag. Treat the rest as visible text
             * (tags included) and stop. This is the graceful-degrade path. */
            vis_off += append_clip(visible, sizeof(visible), vis_off,
                                    req->draft + open_at,
                                    req->draft_len - open_at);
            break;
        }

        const char *content_start = req->draft + open_end;
        size_t content_len = close_at - open_end;
        size_t close_end = close_at + strlen(kind) + 3; /* </name> */

        if (strcmp(kind, "refuse") == 0) {
            refused = true;
            if (content_len > 0) {
                size_t copy = content_len < sizeof(resp->refusal_text) - 1
                                  ? content_len
                                  : sizeof(resp->refusal_text) - 1;
                memcpy(resp->refusal_text, content_start, copy);
                resp->refusal_text[copy] = '\0';
            } else {
                hu_self_rag_render_refusal(HU_REFUSAL_POLICY,
                                            resp->refusal_text,
                                            sizeof(resp->refusal_text));
            }
            /* Stop processing — refusal terminates the stream. */
            break;
        }

        if (resp->claims_count <
            sizeof(resp->claims) / sizeof(resp->claims[0])) {
            hu_atomic_claim_t *c = &resp->claims[resp->claims_count++];
            capture_claim_text(c, content_start, content_len,
                                (int64_t)open_at);

            /* `prov.source` carries the **tag kind** so downstream routing
             * (channel renderer, tests) can distinguish critique vs
             * retrieve provenance. The numeric weight is replaced below
             * with the actual support score so it stops lying. */
            snprintf(c->prov.source, sizeof(c->prov.source), "%s", kind);
            c->prov.observed_at = now;

            /* Real memory-backed scoring per tag kind. Replaces the prior
             * hardcoded `hu_belief_init(0.0f, kind, now)` placeholder so
             * the abstention path (STRICT mode) can decide on real
             * evidence rather than always reading "no support." */
            if (strcmp(kind, "critique") == 0) {
                inline_score_critique(alloc, ctx->m, req->contact_id,
                                       req->contact_id_len, now,
                                       req->wm, c);
            } else if (strcmp(kind, "retrieve") == 0) {
                inline_score_retrieve(alloc, ctx->m, req->contact_id,
                                       req->contact_id_len, c->text, now, c);
            } else {
                c->support = hu_belief_init(0.0f, kind, now);
                c->fabricated = false;
            }
            c->prov.weight = c->support.mean;
        }

        /* Critique tags: their content is the model's claim — emit it as
         * visible text (the receipt would be appended by the channel
         * renderer). Retrieve tags' content is the query — never emit.
         * This matches the spec's "control tokens stripped; receipts
         * inserted" behavior at a deterministic level. */
        if (strcmp(kind, "critique") == 0) {
            vis_off += append_clip(visible, sizeof(visible), vis_off,
                                    content_start, content_len);
        }

        i = close_end;
    }

    if (refused) {
        resp->outcome = HU_SELF_RAG_ABSTAINED;
        /* Visible output is suppressed on refusal. modified_draft is set to
         * the refusal_text so the channel renderer can pick it up
         * uniformly. */
        snprintf(resp->modified_draft, sizeof(resp->modified_draft), "%s",
                 resp->refusal_text);
        resp->draft_modified = true;
        return HU_OK;
    }

    /* STRICT-mode score-based abstention.
     *
     * Only triggers when the caller has explicitly asked for STRICT
     * verification AND a `abstain_threshold` is set (>0). When the
     * fabricated-claim ratio crosses the threshold, render the
     * deterministic LOW_CONFIDENCE refusal template — same behavior
     * the atomic backend uses, just here driven by inline-tagged
     * claims rather than a noun-phrase decomposer.
     *
     * INLINE / SOFT / OFF callers fall through to the existing
     * tag-stripping outcome below; existing tests that pass mode
     * = HU_VERIFY_INLINE keep their semantics. */
    if (req->mode == HU_VERIFY_STRICT && req->abstain_threshold > 0.0f &&
        resp->claims_count > 0) {
        size_t fabricated = 0;
        bool has_negative_mem = false;
        for (size_t k = 0; k < resp->claims_count; k++) {
            if (resp->claims[k].fabricated) {
                fabricated++;
                if (resp->claims[k].support.prov_count > 0 &&
                    strncmp(resp->claims[k].support.prov[0].source,
                            "inline-negative-mem", 19) == 0)
                    has_negative_mem = true;
            }
        }
        float ratio = (float)fabricated / (float)resp->claims_count;
        if (ratio >= req->abstain_threshold) {
            hu_refusal_reason_t reason = has_negative_mem
                ? HU_REFUSAL_NEGATIVE_MEMORY_MATCH
                : HU_REFUSAL_LOW_CONFIDENCE;
            hu_self_rag_render_refusal(reason,
                                       resp->refusal_text,
                                       sizeof(resp->refusal_text));
            resp->outcome = HU_SELF_RAG_ABSTAINED;
            snprintf(resp->modified_draft, sizeof(resp->modified_draft), "%s",
                     resp->refusal_text);
            resp->draft_modified = true;
            return HU_OK;
        }
    }

    /* The inline parser always populates `modified_draft` with the visible
     * (control-token-stripped) output so the channel renderer has a single
     * field to read. `draft_modified` only flips when the bytes actually
     * differ from the input — that's the signal "I changed something". */
    snprintf(resp->modified_draft, sizeof(resp->modified_draft), "%s",
             visible);
    if (vis_off != req->draft_len ||
        memcmp(visible, req->draft, vis_off) != 0) {
        resp->draft_modified = true;
        resp->outcome = HU_SELF_RAG_HEDGED;
    } else {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
    }
    return HU_OK;
}

static void inline_deinit(void *vctx) {
    inline_ctx_t *ctx = (inline_ctx_t *)vctx;
    if (!ctx) return;
    free(ctx);
}

static hu_self_rag_vtable_t inline_vt = {
    .name = "inline",
    .verify = inline_verify,
    .deinit = inline_deinit,
};

/* ── Streaming self-RAG filter ─────────────────────────────────────────── */

/* Control token patterns we scan for in the stream. */
static const char *const k_stream_tags[] = {"<retrieve>", "<critique>", "<refuse>", NULL};
static const size_t k_stream_tag_lens[] = {10, 10, 8};

/* Forward buffered bytes as a content chunk to the original callback.
 * Returns the original callback's return value (true = continue). */
static bool flush_buffer_to_original(hu_self_rag_stream_ctx_t *ctx,
                                      const char *data, size_t len) {
    if (!ctx->original_cb || len == 0)
        return true;
    hu_stream_chunk_t fwd;
    memset(&fwd, 0, sizeof(fwd));
    fwd.type = HU_STREAM_CONTENT;
    fwd.delta = data;
    fwd.delta_len = len;
    return ctx->original_cb(ctx->original_ctx, &fwd);
}

hu_error_t hu_self_rag_stream_wrap(hu_self_rag_stream_ctx_t *ctx,
                                    hu_stream_callback_t original_cb,
                                    void *original_ctx,
                                    hu_memory_facade_t *memory,
                                    hu_allocator_t *alloc) {
    if (!ctx)
        return HU_ERR_INVALID_ARGUMENT;
    memset(ctx, 0, sizeof(*ctx));
    ctx->original_cb = original_cb;
    ctx->original_ctx = original_ctx;
    ctx->memory = memory;
    ctx->alloc = alloc;
    return HU_OK;
}

/* Check if the token buffer starts with a prefix of any control tag.
 * Returns the tag index (0=retrieve, 1=critique, 2=refuse) or -1. */
static int match_tag_prefix(const char *buf, size_t len) {
    for (int i = 0; k_stream_tags[i]; i++) {
        size_t tl = k_stream_tag_lens[i];
        size_t cmp = len < tl ? len : tl;
        if (cmp > 0 && memcmp(buf, k_stream_tags[i], cmp) == 0)
            return i;
    }
    return -1;
}

/* Check if the buffer contains a complete control tag starting at position 0. */
static int match_complete_tag(const char *buf, size_t len) {
    for (int i = 0; k_stream_tags[i]; i++) {
        size_t tl = k_stream_tag_lens[i];
        if (len >= tl && memcmp(buf, k_stream_tags[i], tl) == 0)
            return i;
    }
    return -1;
}

bool hu_self_rag_stream_callback(void *vctx, const hu_stream_chunk_t *chunk) {
    hu_self_rag_stream_ctx_t *ctx = (hu_self_rag_stream_ctx_t *)vctx;
    if (!ctx || !chunk)
        return false;

    /* Non-content chunks and final markers pass through unchanged. */
    if (chunk->type != HU_STREAM_CONTENT || chunk->is_final) {
        if (ctx->original_cb)
            return ctx->original_cb(ctx->original_ctx, chunk);
        return true;
    }

    if (!chunk->delta || chunk->delta_len == 0)
        return true;

    /* If refuse already triggered, suppress all further content. */
    if (ctx->refuse_triggered)
        return true;

    /* Process each byte of the incoming delta. We scan for '<' which may
     * begin a control tag, buffer potential tag characters, and flush
     * non-tag content to the original callback. */
    const char *data = chunk->delta;
    size_t data_len = chunk->delta_len;
    size_t i = 0;

    while (i < data_len) {
        if (ctx->refuse_triggered)
            return true;

        /* If we're mid-buffer (accumulating a potential tag), keep feeding. */
        if (ctx->token_buf_len > 0) {
            /* Add one byte at a time to the buffer. */
            if (ctx->token_buf_len < HU_SELF_RAG_TOKEN_BUF_SIZE - 1) {
                ctx->token_buf[ctx->token_buf_len++] = data[i];
                ctx->token_buf[ctx->token_buf_len] = '\0';
                i++;
            } else {
                /* Buffer full without a match — flush it all as normal text. */
                if (!flush_buffer_to_original(ctx, ctx->token_buf, ctx->token_buf_len))
                    return false;
                ctx->token_buf_len = 0;
                continue;
            }

            /* Check if we still have a valid prefix. */
            int tag_idx = match_tag_prefix(ctx->token_buf, ctx->token_buf_len);
            if (tag_idx < 0) {
                /* No longer a prefix of any tag — flush buffer as content. */
                if (!flush_buffer_to_original(ctx, ctx->token_buf, ctx->token_buf_len))
                    return false;
                ctx->token_buf_len = 0;
                continue;
            }

            /* Check for complete tag match. */
            int complete = match_complete_tag(ctx->token_buf, ctx->token_buf_len);
            if (complete >= 0) {
                size_t tag_len = k_stream_tag_lens[complete];
                /* Tag matched — set the flag, strip the tag, and flush any
                 * trailing bytes that were buffered after the tag. */
                switch (complete) {
                case 0: ctx->retrieval_triggered = true; break;
                case 1: ctx->critique_triggered = true; break;
                case 2: ctx->refuse_triggered = true; break;
                }

                if (ctx->refuse_triggered) {
                    ctx->token_buf_len = 0;
                    return true;
                }

                /* If buffer has bytes beyond the tag, flush them. */
                size_t remaining = ctx->token_buf_len - tag_len;
                if (remaining > 0) {
                    memmove(ctx->token_buf, ctx->token_buf + tag_len, remaining);
                    ctx->token_buf_len = remaining;
                } else {
                    ctx->token_buf_len = 0;
                }
            }
            /* Otherwise keep accumulating — it's a valid prefix but incomplete. */
            continue;
        }

        /* Not in the middle of buffering — scan for '<'. */
        if (data[i] == '<') {
            ctx->token_buf[0] = '<';
            ctx->token_buf_len = 1;
            ctx->token_buf[1] = '\0';
            i++;
            continue;
        }

        /* Normal character — find the next '<' and flush everything before it. */
        size_t start = i;
        while (i < data_len && data[i] != '<')
            i++;
        if (!flush_buffer_to_original(ctx, data + start, i - start))
            return false;
    }

    return true;
}

void hu_self_rag_stream_flush(hu_self_rag_stream_ctx_t *ctx) {
    if (!ctx || ctx->token_buf_len == 0)
        return;
    /* Flush any remaining partial buffer as normal content. This handles
     * the case where the stream ends mid-tag (e.g. "<retr" with no
     * closing "ieve>"). Safe-by-default: pass through unchanged. */
    flush_buffer_to_original(ctx, ctx->token_buf, ctx->token_buf_len);
    ctx->token_buf_len = 0;
}

/* W11 streaming control-token directive — kept terse to minimise the
 * system-prompt budget impact (~430 bytes). The exact tag spellings
 * match the parser in hu_self_rag_stream_callback; do not edit one
 * without the other. */
static const char *const SELF_RAG_STREAM_DIRECTIVE =
    "\n[Self-RAG streaming protocol]\n"
    "During this response you MAY emit any of these control tokens at "
    "the start of a sentence to request runtime help. They are stripped "
    "from the user-visible output and do not count toward length budgets.\n"
    "  <retrieve>QUERY</retrieve>   — probe memory for QUERY before continuing.\n"
    "  <critique>CLAIM</critique>   — flag CLAIM for support scoring; the\n"
    "                                  runtime may abstain if support is low.\n"
    "  <refuse>REASON</refuse>      — abort this response; the runtime will\n"
    "                                  substitute a policy refusal.\n"
    "Use them sparingly. Omit them entirely for well-grounded answers.\n";

hu_error_t hu_self_rag_stream_directive_append(hu_allocator_t *alloc,
                                                char **system_prompt,
                                                size_t *system_prompt_len) {
    if (!alloc || !system_prompt || !system_prompt_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t add_len = strlen(SELF_RAG_STREAM_DIRECTIVE);
    size_t old_len = *system_prompt_len;
    size_t new_len = old_len + add_len;

    /* realloc grows in place if possible. Old buffer is +1 for the
     * trailing NUL; new buffer is +1 for the same. */
    char *new_buf = (char *)alloc->realloc(alloc->ctx, *system_prompt,
                                            old_len + 1, new_len + 1);
    if (!new_buf)
        return HU_ERR_OUT_OF_MEMORY;

    memcpy(new_buf + old_len, SELF_RAG_STREAM_DIRECTIVE, add_len);
    new_buf[new_len] = '\0';
    *system_prompt = new_buf;
    *system_prompt_len = new_len;
    return HU_OK;
}

hu_error_t hu_self_rag_inline(hu_memory_facade_t *m, hu_provider_t *chat,
                               hu_self_rag_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)chat;
    inline_ctx_t *ctx = (inline_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->m = m;
    out->vt = &inline_vt;
    out->ctx = ctx;
    return HU_OK;
}
