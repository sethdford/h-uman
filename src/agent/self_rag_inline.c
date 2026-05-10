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

#include "human/core/error.h"
#include "human/memory/belief.h"
#include "human/memory/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct inline_ctx {
    hu_memory_t *m;
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
            /* Both retrieve and critique start as unknown-support beliefs;
             * a future provider integration replaces these with real
             * scores from hu_memory_read / atomic verifier calls. The
             * primary provenance source is the control-token name itself
             * so callers can route the claim downstream by tag. */
            c->support = hu_belief_init(0.0f, kind, now);
            snprintf(c->prov.source, sizeof(c->prov.source), "%s", kind);
            c->prov.observed_at = now;
            c->prov.weight = 0.0f;
            c->fabricated = false;
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

hu_error_t hu_self_rag_inline(hu_memory_t *m, hu_provider_t *chat,
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
