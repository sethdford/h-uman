/* W11 — Atomic-claim decomposer + verifier backend.
 *
 * Splits a draft into noun-phrase atomic claims using a deterministic
 * preposition-aware splitter (no LLM, no embedder), then runs each claim
 * through the v1 graph-token-overlap verifier. The atomic backend is
 * provider-agnostic: it does not require the chat provider to emit any
 * special control tokens.
 *
 * Decomposition example:
 *   "Alice works at Acme since 2024 in NYC."
 *     stem = "Alice works"
 *     prep splits = " at ", " since ", " in "
 *     atomic claims: "Alice works at Acme",
 *                    "Alice works since 2024",
 *                    "Alice works in NYC"
 *
 * If ≥ `abstain_threshold` (default 0.5) of the atomic claims fall below the
 * support floor, the backend returns HU_SELF_RAG_ABSTAINED with the
 * deterministic refusal template. Otherwise it composes a hedged or
 * supported response.
 */

#include "human/agent/self_rag.h"

#include "human/memory/corrective_rag.h"
#include "human/memory/graph.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The closed list of decomposition prepositions. Order matters only for
 * cosmetics; the splitter is left-to-right. The trailing/leading spaces are
 * intentional — we split on whole-word matches only. */
static const char *const k_preps[] = {
    " at ", " in ", " on ", " since ", " from ", " to ",
    " for ", " with ", " by ", " of ", " about ", " near ",
    NULL,
};

typedef struct atomic_ctx {
    hu_memory_facade_t *m;
} atomic_ctx_t;

/* Trim leading/trailing ASCII whitespace and write into dst. dst_cap must be
 * large enough for the trimmed result + nul. */
static void trim_into(const char *src, size_t len, char *dst, size_t dst_cap) {
    if (dst_cap == 0) return;
    while (len > 0 && isspace((unsigned char)src[0])) { src++; len--; }
    while (len > 0 && isspace((unsigned char)src[len - 1])) { len--; }
    size_t copy = len < dst_cap - 1 ? len : dst_cap - 1;
    if (copy > 0)
        memcpy(dst, src, copy);
    dst[copy] = '\0';
}

/* Find the next sentence break starting at `from`. Returns the offset of the
 * terminator and sets *is_question. End-of-buffer counts as a terminator. */
static size_t next_sentence_end(const char *s, size_t len, size_t from,
                                 bool *is_question) {
    *is_question = false;
    for (size_t i = from; i < len; i++) {
        if (s[i] == '.' || s[i] == '!') return i;
        if (s[i] == '?') { *is_question = true; return i; }
    }
    return len;
}

/* Count alphabetic tokens (>= 1 char) in [s, s+len). */
static size_t alpha_word_count(const char *s, size_t len) {
    size_t n = 0;
    bool in_word = false;
    for (size_t i = 0; i < len; i++) {
        bool a = isalpha((unsigned char)s[i]) != 0;
        if (a && !in_word) { n++; in_word = true; }
        else if (!a) in_word = false;
    }
    return n;
}

/* Find the next preposition match in [s, s+len) starting at `from`. Returns
 * the byte offset of the leading space, the matching preposition string in
 * *out_prep, and its length in *out_prep_len. Returns SIZE_MAX if none. */
static size_t find_next_prep(const char *s, size_t len, size_t from,
                              const char **out_prep, size_t *out_prep_len) {
    size_t best_off = (size_t)-1;
    const char *best_prep = NULL;
    size_t best_len = 0;
    for (size_t k = 0; k_preps[k]; k++) {
        const char *p = k_preps[k];
        size_t pl = strlen(p);
        /* Manual case-insensitive substring search starting at `from`. */
        if (len < pl || from > len - pl) continue;
        for (size_t i = from; i + pl <= len; i++) {
            bool match = true;
            for (size_t j = 0; j < pl; j++) {
                char a = (char)tolower((unsigned char)s[i + j]);
                char b = (char)tolower((unsigned char)p[j]);
                if (a != b) { match = false; break; }
            }
            if (match) {
                if (i < best_off) {
                    best_off = i;
                    best_prep = p;
                    best_len = pl;
                }
                break;
            }
        }
    }
    if (out_prep) *out_prep = best_prep;
    if (out_prep_len) *out_prep_len = best_len;
    return best_off;
}

/* Decompose `sentence` (no terminator) into atomic claims and append to
 * out[]. Returns number written. The stem is the substring before the first
 * preposition; each subsequent preposition+segment becomes "stem prep
 * segment". With zero prepositions the whole sentence is emitted. */
static size_t decompose_sentence(const char *sentence, size_t len,
                                 hu_atomic_claim_t *out, size_t cap,
                                 size_t base_offset) {
    if (cap == 0) return 0;
    /* Skip empty / whitespace-only / very short sentences. */
    if (alpha_word_count(sentence, len) < 3) return 0;

    /* Find prepositions across the sentence. */
    size_t prep_offsets[16];
    const char *prep_strs[16];
    size_t prep_lens[16];
    size_t n_preps = 0;
    size_t scan = 0;
    while (n_preps < 16) {
        const char *p = NULL;
        size_t pl = 0;
        size_t off = find_next_prep(sentence, len, scan, &p, &pl);
        if (off == (size_t)-1) break;
        prep_offsets[n_preps] = off;
        prep_strs[n_preps] = p;
        prep_lens[n_preps] = pl;
        n_preps++;
        scan = off + pl;
    }

    /* No prepositions → one claim covering the whole sentence. */
    if (n_preps == 0) {
        hu_atomic_claim_t *c = &out[0];
        memset(c, 0, sizeof(*c));
        trim_into(sentence, len, c->text, sizeof(c->text));
        c->span_start = (int64_t)base_offset;
        c->span_end = (int64_t)(base_offset + len);
        return c->text[0] ? 1 : 0;
    }

    /* Stem = sentence up to first preposition; bail if it has no real verb
     * content (heuristic: < 2 alpha words). Falls back to the whole sentence
     * as one claim — matches v1 behavior. */
    size_t stem_len = prep_offsets[0];
    if (alpha_word_count(sentence, stem_len) < 2) {
        hu_atomic_claim_t *c = &out[0];
        memset(c, 0, sizeof(*c));
        trim_into(sentence, len, c->text, sizeof(c->text));
        c->span_start = (int64_t)base_offset;
        c->span_end = (int64_t)(base_offset + len);
        return c->text[0] ? 1 : 0;
    }

    char stem[160];
    trim_into(sentence, stem_len, stem, sizeof(stem));

    size_t out_n = 0;
    for (size_t i = 0; i < n_preps && out_n < cap; i++) {
        size_t seg_start = prep_offsets[i] + prep_lens[i];
        size_t seg_end = (i + 1 < n_preps) ? prep_offsets[i + 1] : len;
        /* Trim the preposition surface — we capture it without the trailing
         * space so the rendered claim reads naturally. */
        const char *prep = prep_strs[i];
        size_t pl = prep_lens[i];
        char prep_trim[16] = {0};
        size_t pi = 0;
        for (size_t j = 0; j < pl && pi < sizeof(prep_trim) - 1; j++) {
            if (!isspace((unsigned char)prep[j])) prep_trim[pi++] = prep[j];
        }
        prep_trim[pi] = '\0';

        char segment[160];
        trim_into(sentence + seg_start, seg_end - seg_start, segment,
                  sizeof(segment));
        if (segment[0] == '\0') continue;

        hu_atomic_claim_t *c = &out[out_n];
        memset(c, 0, sizeof(*c));
        snprintf(c->text, sizeof(c->text), "%s %s %s", stem, prep_trim,
                 segment);
        c->span_start = (int64_t)base_offset;
        c->span_end = (int64_t)(base_offset + len);
        out_n++;
    }
    /* If everything came up empty (all segments whitespace), still emit the
     * whole sentence so the verifier can score it. */
    if (out_n == 0) {
        hu_atomic_claim_t *c = &out[0];
        memset(c, 0, sizeof(*c));
        trim_into(sentence, len, c->text, sizeof(c->text));
        c->span_start = (int64_t)base_offset;
        c->span_end = (int64_t)(base_offset + len);
        return c->text[0] ? 1 : 0;
    }
    return out_n;
}

/* Decompose a full draft into atomic claims, walking sentence by sentence. */
static size_t decompose_draft(const char *draft, size_t draft_len,
                              hu_atomic_claim_t *out, size_t cap) {
    size_t out_n = 0;
    size_t i = 0;
    while (i < draft_len && out_n < cap) {
        bool is_q = false;
        size_t end = next_sentence_end(draft, draft_len, i, &is_q);
        size_t len = end - i;
        if (!is_q && len > 0) {
            out_n += decompose_sentence(draft + i, len, out + out_n,
                                         cap - out_n, i);
        }
        i = end + 1; /* skip terminator */
    }
    return out_n;
}

/* P2F — Corrective-RAG via the W7 facade.
 *
 * For a fabricated claim under STRICT mode, look in the contact's relation
 * graph for any edge whose `context` text grades RELEVANT against the
 * claim. The grading reuses `hu_crag_grade_document` (token-overlap
 * scoring) — the same logic legacy CRAG uses for its production grader.
 *
 * The bridge lives entirely in the W11 layer: we go from the W7 facade
 * down to its graph handle (downward call, layer-OK), pull relations,
 * grade, and pick the best one. We do NOT touch the legacy
 * `hu_legacy_memory_t` at all, so this works in non-test builds.
 *
 * On success, writes the correction into `out_correction` (caller-owned
 * buffer, NUL-terminated) and returns true. On no-match, returns false
 * and leaves the buffer untouched. Never throws or allocates beyond what
 * `hu_graph_list_relations` requires (which is freed before return). */
static bool retrieve_correction_via_facade(hu_allocator_t *alloc,
                                           hu_memory_facade_t *m,
                                           const char *contact_id,
                                           size_t contact_id_len,
                                           const char *claim, size_t claim_len,
                                           char *out_correction,
                                           size_t out_cap) {
    if (!alloc || !m || !claim || claim_len == 0 || !out_correction || out_cap == 0)
        return false;
    if (!contact_id || contact_id_len == 0)
        return false;

    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return false;

    /* Cap at 64 relations: corrections come from the most-weighted edges,
     * and grading every edge in the graph is wasteful. */
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    hu_error_t err = hu_graph_list_relations(g, alloc, contact_id, contact_id_len,
                                             64, &rels, &n);
    if (err != HU_OK || !rels || n == 0) {
        if (rels) hu_graph_relations_free(alloc, rels, n);
        return false;
    }

    int best_idx = -1;
    double best_score = 0.0;
    for (size_t i = 0; i < n; i++) {
        const char *doc = rels[i].context;
        size_t doc_len = rels[i].context_len;
        if (!doc || doc_len == 0)
            continue;
        hu_rag_graded_doc_t grade = {0};
        if (hu_crag_grade_document(alloc, claim, claim_len, doc, doc_len,
                                   &grade) != HU_OK)
            continue;
        if (grade.relevance == HU_RAG_RELEVANT && grade.score > best_score) {
            best_score = grade.score;
            best_idx = (int)i;
        }
    }

    bool ok = false;
    if (best_idx >= 0) {
        size_t copy = rels[best_idx].context_len;
        if (copy >= out_cap) copy = out_cap - 1;
        memcpy(out_correction, rels[best_idx].context, copy);
        out_correction[copy] = '\0';
        ok = true;
    }
    hu_graph_relations_free(alloc, rels, n);
    return ok;
}

/* Score a single atomic claim against the graph by reusing v1's verifier on
 * a single-sentence "draft". Returns the supporting receipt + score in
 * *out_supported / *out_score / *out_receipt. Failures map to score = 0. */
static void score_atomic_claim(hu_allocator_t *alloc, hu_memory_facade_t *m,
                                const char *contact_id, size_t cid_len,
                                const hu_atomic_claim_t *claim,
                                float *out_score,
                                hu_provenance_receipt_t *out_receipt) {
    *out_score = 0.0f;
    memset(out_receipt, 0, sizeof(*out_receipt));
    /* Wrap the claim text into a sentence so v1 extracts it as 1 claim. */
    char sentence[200];
    snprintf(sentence, sizeof(sentence), "%s.", claim->text);

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    cfg.confidence_threshold = 0.6f;
    cfg.max_claims = 1;

    hu_verifier_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err = hu_response_verify(alloc, m, contact_id, cid_len,
                                         sentence, strlen(sentence), &cfg,
                                         &report);
    if (err != HU_OK || report.claims_extracted == 0) return;
    *out_score = report.claims[0].score;
    *out_receipt = report.claims[0].receipt;
}

static hu_error_t atomic_verify(void *vctx, hu_allocator_t *alloc,
                                 const hu_self_rag_request_t *req,
                                 hu_self_rag_response_t *resp) {
    atomic_ctx_t *ctx = (atomic_ctx_t *)vctx;
    if (!ctx || !alloc || !req || !resp)
        return HU_ERR_INVALID_ARGUMENT;
    if (!req->draft || req->draft_len == 0) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        return HU_OK;
    }
    if (req->mode == HU_VERIFY_OFF) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        return HU_OK;
    }

    /* 1. Decompose. */
    size_t n = decompose_draft(req->draft, req->draft_len, resp->claims,
                                sizeof(resp->claims) / sizeof(resp->claims[0]));
    resp->claims_count = n;
    if (n == 0) {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
        return HU_OK;
    }

    /* 2. Score each atomic claim. */
    int64_t now = req->now_ms;
    size_t flagged = 0;
    /* Default abstain threshold matches the spec (0.5 of claims unsupported
     * = abstain). The request can override per-call. */
    float abstain = req->abstain_threshold > 0.0f ? req->abstain_threshold
                                                   : 0.5f;
    /* The per-claim support floor: claim is "fabricated" when score is
     * below this. Mirrors v1's confidence_threshold default. */
    const float per_claim_floor = 0.6f;

    for (size_t i = 0; i < n; i++) {
        hu_atomic_claim_t *c = &resp->claims[i];
        float score = 0.0f;
        hu_provenance_receipt_t rcpt;
        memset(&rcpt, 0, sizeof(rcpt));
        if (ctx->m) {
            score_atomic_claim(alloc, ctx->m, req->contact_id, req->contact_id_len,
                                c, &score, &rcpt);
        }
        c->support = hu_belief_init(score, "atomic-graph", now);
        memset(&c->prov, 0, sizeof(c->prov));
        if (rcpt.source[0]) {
            snprintf(c->prov.source, sizeof(c->prov.source), "%s", rcpt.source);
            c->prov.observed_at = rcpt.observed_at_ms;
            c->prov.weight = rcpt.confidence;
        }
        c->fabricated = score < per_claim_floor;
        if (c->fabricated) flagged++;
    }

    /* 3. Decide outcome. */
    float ratio = (float)flagged / (float)n;
    if (ratio >= abstain) {
        resp->outcome = HU_SELF_RAG_ABSTAINED;
        hu_self_rag_render_refusal(HU_REFUSAL_UNKNOWN_FACT,
                                    resp->refusal_text,
                                    sizeof(resp->refusal_text));
        return HU_OK;
    }

    /* 4. SOFT/STRICT/INLINE: rebuild the draft with hedges around fabricated
     * claims, supported claims emitted verbatim. We rebuild claim-by-claim
     * rather than sentence-by-sentence so atomic granularity carries
     * through. STRICT attempts corrective-RAG retrieval for flagged claims
     * before dropping them; SOFT/INLINE prepend a hedge. */
    char rebuilt[2048];
    size_t off = 0;
    bool any_modified = false;
    rebuilt[0] = '\0';
    for (size_t i = 0; i < n && off < sizeof(rebuilt) - 256; i++) {
        const hu_atomic_claim_t *c = &resp->claims[i];
        int w;
        if (c->fabricated) {
            if (req->mode == HU_VERIFY_STRICT) {
                /* P2F — Corrective-RAG via W7 facade. Bypasses the legacy
                 * `hu_crag_retrieve` (which expects v1 `hu_legacy_memory_t`)
                 * by going graph-direct through the W7 facade. Works in
                 * both test and production builds. */
                if (ctx->m) {
                    char correction[1024];
                    if (retrieve_correction_via_facade(
                            alloc, ctx->m, req->contact_id, req->contact_id_len,
                            c->text, strlen(c->text),
                            correction, sizeof(correction))) {
                        w = snprintf(rebuilt + off, sizeof(rebuilt) - off,
                                     "%s%s.",
                                     off == 0 ? "" : " ", correction);
                        if (w > 0) off += (size_t)w;
                        any_modified = true;
                        continue;
                    }
                }
                any_modified = true;
                continue;
            }
            w = snprintf(rebuilt + off, sizeof(rebuilt) - off,
                         "%sI'm not 100%% sure but %s.",
                         off == 0 ? "" : " ", c->text);
            any_modified = true;
        } else {
            w = snprintf(rebuilt + off, sizeof(rebuilt) - off, "%s%s.",
                         off == 0 ? "" : " ", c->text);
        }
        if (w > 0) off += (size_t)w;
    }
    if (any_modified) {
        snprintf(resp->modified_draft, sizeof(resp->modified_draft), "%s",
                 rebuilt);
        resp->draft_modified = true;
        resp->outcome = (req->mode == HU_VERIFY_STRICT)
                            ? HU_SELF_RAG_REWRITTEN
                            : HU_SELF_RAG_HEDGED;
    } else {
        resp->outcome = HU_SELF_RAG_SUPPORTED;
    }
    return HU_OK;
}

static void atomic_deinit(void *vctx) {
    atomic_ctx_t *ctx = (atomic_ctx_t *)vctx;
    if (!ctx) return;
    free(ctx);
}

static hu_self_rag_vtable_t atomic_vt = {
    .name = "atomic",
    .verify = atomic_verify,
    .deinit = atomic_deinit,
};

hu_error_t hu_self_rag_atomic(hu_memory_facade_t *m, hu_provider_t *embedder,
                               hu_self_rag_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    /* `embedder` is reserved for the future LLM-driven decomposer. The
     * deterministic splitter in this commit does not use it. */
    (void)embedder;
    atomic_ctx_t *ctx = (atomic_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->m = m;
    out->vt = &atomic_vt;
    out->ctx = ctx;
    return HU_OK;
}
