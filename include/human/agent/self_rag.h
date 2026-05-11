#ifndef HU_AGENT_SELF_RAG_H
#define HU_AGENT_SELF_RAG_H

/* W11 — Inline Self-RAG with abstention.
 *
 * Moves verification *into* generation. The agent's draft is decomposed into
 * atomic noun-phrase claims; each claim is scored against the W7 memory
 * facade (`hu_memory_facade_t`) and the W9 world model (`hu_world_model_t`); the
 * support score is carried as a W8 belief posterior (`hu_belief_t`). When
 * the draft as a whole carries too little evidence, the verifier returns
 * an explicit ABSTAINED outcome with a deterministic refusal template.
 *
 * Three pluggable backends sit behind a single vtable:
 *   - heuristic: wraps v1's `hu_response_verify` for parity / fallback.
 *   - atomic:    decomposes the draft into noun-phrase atomic claims and
 *                verifies each individually.
 *   - inline:    parses provider control tokens (<retrieve>, <critique>,
 *                <refuse>) emitted mid-stream. The real provider streaming
 *                wiring is out of scope for this commit; a deterministic
 *                placeholder parses the same protocol from a finished draft
 *                so the rest of the pipeline can be built and tested.
 *
 * Layer 6 of the v2 stack (see docs/plans/2026-05-10-w11-inline-self-rag.md).
 */

#include "human/agent/response_verifier.h" /* hu_verify_mode_t, receipts */
#include "human/agent/world_model.h"       /* hu_world_model_t */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/belief.h"           /* hu_belief_t, hu_provenance_atom_t */
#include "human/memory/memory.h"           /* hu_memory_facade_t */
#include "human/provider.h"                /* hu_provider_t, hu_stream_callback_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Outcome of a single verification pass. SUPPORTED = no changes needed.
 * HEDGED = SOFT mode prepended hedges to flagged claims. REWRITTEN = STRICT
 * mode rewrote the draft (corrective-RAG path). ABSTAINED = the agent chose
 * to refuse rather than emit an unsupported claim; `refusal_text` is set. */
typedef enum hu_self_rag_outcome {
    HU_SELF_RAG_SUPPORTED = 0,
    HU_SELF_RAG_HEDGED = 1,
    HU_SELF_RAG_REWRITTEN = 2,
    HU_SELF_RAG_ABSTAINED = 3,
} hu_self_rag_outcome_t;

/* Deterministic refusal categories. Templates are emitted by
 * hu_self_rag_render_refusal so callers can localize without forking
 * the verifier. */
typedef enum hu_refusal_reason {
    HU_REFUSAL_UNKNOWN_FACT = 0,
    HU_REFUSAL_POLICY = 1,
    HU_REFUSAL_NEGATIVE_MEMORY_MATCH = 2,
    HU_REFUSAL_LOW_CONFIDENCE = 3,
} hu_refusal_reason_t;

/* A single noun-phrase atomic claim extracted from a draft.
 *
 * NOTE on field types: the original spec proposed `char span_start[32]` /
 * `char span_end[32]` for the byte offsets. That's an obvious type error —
 * spans are integer offsets into the draft buffer, not strings. We use
 * int64_t which matches every other byte-offset field in the codebase
 * (e.g. hu_graph_relation_t event_start). */
typedef struct hu_atomic_claim {
    char text[160];               /* the atomic claim, truncated for the report */
    int64_t span_start;           /* byte offset in draft (inclusive) */
    int64_t span_end;             /* byte offset in draft (exclusive) */
    hu_belief_t support;          /* W8 posterior over "is this claim true?" */
    hu_provenance_atom_t prov;    /* primary supporting source; zero when none */
    bool fabricated;              /* true when no evidence + low support variance */
} hu_atomic_claim_t;

/* Caller's request to the verifier. `wm` may be NULL — backends that don't
 * use the world model ignore it. `mode` follows the v1 verifier semantics
 * with the new HU_VERIFY_INLINE value reserved for the inline backend. */
typedef struct hu_self_rag_request {
    hu_world_model_t *wm;         /* optional W9 view of the contact */
    const char *contact_id;       /* nullable; used by the heuristic backend */
    size_t contact_id_len;
    const char *draft;
    size_t draft_len;
    hu_verify_mode_t mode;        /* OFF/SOFT/STRICT/INLINE */
    float abstain_threshold;      /* default 0.3 — below this, refuse */
    int64_t now_ms;               /* 0 = use OS clock */
} hu_self_rag_request_t;

/* The verifier's response. `modified_draft` and `refusal_text` are inline
 * buffers so the response is self-contained and trivial to free (it isn't:
 * everything is on the caller's stack frame). `claims_count` <= 32. */
typedef struct hu_self_rag_response {
    hu_self_rag_outcome_t outcome;
    char modified_draft[2048];
    bool draft_modified;
    hu_atomic_claim_t claims[32];
    size_t claims_count;
    char refusal_text[256];       /* populated when outcome == ABSTAINED */
} hu_self_rag_response_t;

/* Backend vtable. The verifier is synchronous and side-effect-free with
 * respect to memory state (it reads, never writes). */
typedef struct hu_self_rag_vtable {
    const char *name;
    hu_error_t (*verify)(void *ctx, hu_allocator_t *alloc,
                         const hu_self_rag_request_t *req,
                         hu_self_rag_response_t *resp);
    void (*deinit)(void *ctx);
} hu_self_rag_vtable_t;

typedef struct hu_self_rag {
    hu_self_rag_vtable_t *vt;
    void *ctx;
} hu_self_rag_t;

/* Construct the heuristic backend (v1 parity). Wraps `hu_response_verify`. */
hu_error_t hu_self_rag_heuristic(hu_memory_facade_t *m, hu_self_rag_t *out);

/* Construct the atomic backend. Decomposes drafts into noun-phrase atomic
 * claims and verifies each against `hu_memory_facade_t`. `embedder` may be NULL —
 * the deterministic decomposer in this commit does not require it; the
 * parameter is reserved for the future LLM-driven decomposer. */
hu_error_t hu_self_rag_atomic(hu_memory_facade_t *m, hu_provider_t *embedder,
                              hu_self_rag_t *out);

/* Construct the inline backend. Parses control tokens emitted by a
 * provider that supports them. `chat` may be NULL in this commit —
 * provider streaming integration is a follow-up. The deterministic
 * placeholder parses the protocol from the supplied draft string so the
 * rest of the pipeline can be built and tested. */
hu_error_t hu_self_rag_inline(hu_memory_facade_t *m, hu_provider_t *chat,
                              hu_self_rag_t *out);

/* Dispatch to the bound backend. `alloc` is used for any temporary
 * allocations the backend needs (e.g. wrapping the v1 verifier). */
hu_error_t hu_self_rag_verify(hu_self_rag_t *r, hu_allocator_t *alloc,
                              const hu_self_rag_request_t *req,
                              hu_self_rag_response_t *resp);

/* Free backend-owned context. Safe with a zero-initialized struct. */
void hu_self_rag_close(hu_self_rag_t *r);

/* Render the deterministic refusal template for `reason` into `buf`.
 * Always null-terminates if cap > 0. Public so tests and the channel
 * renderer can share the same source-of-truth strings. */
void hu_self_rag_render_refusal(hu_refusal_reason_t reason, char *buf, size_t cap);

/* ── Inline streaming self-RAG ────────────────────────────────────────────
 *
 * Stream filter that intercepts control tokens (<retrieve>, <critique>,
 * <refuse>) during generation rather than post-hoc. Wraps the original
 * provider stream callback and buffers partial tokens across chunk
 * boundaries.
 *
 * Enable with HU_SELF_RAG_STREAMING=1 env var (default: disabled).
 * ──────────────────────────────────────────────────────────────────────── */

#define HU_SELF_RAG_TOKEN_BUF_SIZE 64

typedef struct hu_self_rag_stream_ctx {
    hu_stream_callback_t original_cb;
    void *original_ctx;
    char token_buf[HU_SELF_RAG_TOKEN_BUF_SIZE];
    size_t token_buf_len;
    hu_memory_facade_t *memory;
    hu_allocator_t *alloc;
    bool retrieval_triggered;
    bool critique_triggered;
    bool refuse_triggered;
} hu_self_rag_stream_ctx_t;

/* Initialize a stream wrapper context. After calling this, pass
 * hu_self_rag_stream_callback as the provider's stream callback with
 * `ctx` as the callback context. */
hu_error_t hu_self_rag_stream_wrap(hu_self_rag_stream_ctx_t *ctx,
                                    hu_stream_callback_t original_cb,
                                    void *original_ctx,
                                    hu_memory_facade_t *memory,
                                    hu_allocator_t *alloc);

/* Stream callback that detects and strips control tokens, forwarding
 * clean content to the original callback. */
bool hu_self_rag_stream_callback(void *ctx, const hu_stream_chunk_t *chunk);

/* Flush any remaining buffered bytes to the original callback. Call
 * after the stream completes to ensure no partial text is lost. */
void hu_self_rag_stream_flush(hu_self_rag_stream_ctx_t *ctx);

/* Append the W11 control-token directive to a system prompt. The
 * directive tells the model that during streaming it may emit:
 *
 *   <retrieve>QUERY</retrieve>   — request a mid-stream memory probe
 *                                  for QUERY (caller side scores
 *                                  support; if low and STRICT, the
 *                                  abstention path runs).
 *   <critique>CLAIM</critique>   — flag a factual claim for self-RAG
 *                                  verification.
 *   <refuse>REASON</refuse>      — abort generation; the agent
 *                                  swaps the response for a policy-
 *                                  appropriate refusal template.
 *
 * On success, `*system_prompt` is reallocated to include the directive
 * and `*system_prompt_len` is updated. On failure (OOM) the inputs are
 * left unchanged and HU_ERR_OUT_OF_MEMORY is returned.
 *
 * The directive is intentionally short (one paragraph) to keep the
 * system-prompt cost low; the parser handles tag boundaries even if
 * the model wraps the tokens in code fences or mid-sentence. */
hu_error_t hu_self_rag_stream_directive_append(hu_allocator_t *alloc,
                                                char **system_prompt,
                                                size_t *system_prompt_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_SELF_RAG_H */
