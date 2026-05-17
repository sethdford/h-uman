#ifndef HU_AGENT_RESPONSE_VERIFIER_H
#define HU_AGENT_RESPONSE_VERIFIER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* W4 — Response verifier with provenance receipts.
 *
 * Wraps the existing self_rag / verify_claim logic in a synchronous check
 * that runs on the response path. Each factual claim in a draft response is
 * scored against the memory graph; sub-threshold claims are flagged with a
 * suggested hedge or rewrite, and supporting provenance is collected so the
 * channel layer can render attribution like:
 *
 *   "You start work at 9 [from iMessage, Mon 2026-05-09 14:22]."
 *
 * Verification policy (mode):
 *   - HU_VERIFY_OFF       Skip entirely; report stays zeroed.
 *   - HU_VERIFY_TELEMETRY Extract + score every claim but NEVER modify the
 *                         draft. Used by default on the response path so we
 *                         get hallucination metrics without changing the
 *                         user-visible reply.
 *   - HU_VERIFY_SOFT      Prepend a hedge to flagged claims; attach provenance.
 *   - HU_VERIFY_STRICT    Rewrite the draft via corrective-RAG. */

typedef enum hu_verify_mode {
    HU_VERIFY_OFF = 0,
    HU_VERIFY_SOFT = 1,
    HU_VERIFY_STRICT = 2,
    HU_VERIFY_TELEMETRY = 3,
    HU_VERIFY_INLINE = 4, /* W11: provider emits control tokens during generation */
} hu_verify_mode_t;

typedef struct hu_provenance_receipt {
    int64_t graph_relation_id;  /* source row in relations table; 0 = synthetic */
    char source[64];            /* short label: "imessage", "user", "feed-web", ... */
    int64_t observed_at_ms;     /* unix ms when the supporting fact was observed */
    int64_t event_start_ms;     /* bitemporal: when the fact became true */
    int64_t event_end_ms;       /* 0 = still true */
    float confidence;           /* 0..1 */
    char rendered[160];         /* human-readable receipt string */
} hu_provenance_receipt_t;

typedef struct hu_verifier_claim {
    char text[256];             /* the claim text, truncated for the report */
    float score;                /* combined verifier score; 1.0 = strongly supported */
    bool supported;             /* true if score >= threshold */
    hu_provenance_receipt_t receipt; /* set when supported; rendered = "" otherwise */
    char suggested_hedge[160];  /* set when !supported; e.g. "I'm not 100% sure but" */
} hu_verifier_claim_t;

typedef struct hu_verifier_config {
    hu_verify_mode_t mode;
    float confidence_threshold; /* default 0.6 */
    size_t max_claims;          /* cap per response; default 16 */
    int64_t now_ms;             /* 0 = use OS clock */
    float abstain_threshold;    /* fraction of flagged claims that triggers
                                 * ABSTAIN; default 0.5 (set 0 = use default) */
} hu_verifier_config_t;

/* Outcome of a v1 verification pass. Mirrors hu_self_rag_outcome_t for
 * callers that go through the v1 verifier directly (e.g. telemetry). */
typedef enum hu_verifier_outcome {
    HU_VERIFY_RESULT_SUPPORTED = 0,
    HU_VERIFY_RESULT_HEDGED    = 1,
    HU_VERIFY_RESULT_REWRITTEN = 2,
    HU_VERIFY_RESULT_ABSTAIN   = 3,
} hu_verifier_outcome_t;

typedef struct hu_verifier_report {
    hu_verifier_outcome_t outcome;
    size_t claims_extracted;
    size_t claims_supported;
    size_t claims_flagged;
    bool draft_modified;        /* true when SOFT/STRICT made changes */
    char modified_draft[2048];  /* populated when draft_modified is true */
    char refusal_text[256];     /* populated when outcome == ABSTAIN */
    hu_verifier_claim_t claims[16];
} hu_verifier_report_t;

hu_verifier_config_t hu_verifier_default_config(void);

/* Run synchronous verification over a draft. The caller passes the draft text
 * plus a contact_id to scope queries. Returns HU_OK and fills out_report on
 * success. Memory is NEVER modified by this call.
 *
 * `memory` is the W7 facade; pass NULL for graph-less telemetry (every claim
 * scores unsupported). */
hu_error_t hu_response_verify(hu_allocator_t *alloc, hu_memory_facade_t *memory, const char *contact_id,
                              size_t contact_id_len, const char *draft, size_t draft_len,
                              const hu_verifier_config_t *cfg, hu_verifier_report_t *out_report);

/* sprint-2c Story A — verify against both the W7 facade AND a loaded world
 * model. When `wm` is non-NULL the verifier also walks `wm->negatives` and
 * shapes the outcome by the negative's `source` tag:
 *
 *   USER_EXPLICIT    (`[hard]`)    → ABSTAIN, refusal cites the user's prior request
 *   SYSTEM_POLICY    (`[policy]`)  → ABSTAIN, refusal cites the safety policy
 *   SELF_RAG_ABSTAIN (`[soft]`)    → HEDGED,  hedge cites prior low confidence
 *   AUTO_EXTRACT     (`[confirm]`) → HEDGED,  hedge asks the user to re-confirm
 *
 * Strictness lattice (ABSTAIN > HEDGED > SUPPORTED) — when multiple negatives
 * match a draft, the strictest wins.
 *
 * When `wm` is NULL the behavior is byte-identical to `hu_response_verify`.
 *
 * `wm` is BORROWED — caller owns lifetime and must keep it alive across this
 * call. Forward-declared so callers that don't need this entry point don't
 * pay the world_model.h include cost. */
struct hu_world_model;
hu_error_t hu_response_verify_against_world_model(
    hu_allocator_t *alloc, hu_memory_facade_t *memory,
    const struct hu_world_model *wm,
    const char *contact_id, size_t contact_id_len,
    const char *draft, size_t draft_len,
    const hu_verifier_config_t *cfg, hu_verifier_report_t *out_report);

/* sprint-2c Story A — scan one claim against `wm->negatives` and return the
 * strictest implied outcome (ABSTAIN > HEDGED > SUPPORTED).
 *
 * Matcher: tokenize the negative into ≥5-char non-stopword lowercase tokens
 * (the denominator), then count how many appear as substrings of the
 * lowercased claim. Hit when `hits ≥ 0.3 × negative_tokens` — catches a
 * single topic-keyword tripwire in a short negative ("never discuss the
 * merger" hit by "The merger talks are going well") while keeping benign
 * claims with zero topic-word overlap below threshold.
 *
 * When a match exists, the matching negative's refusal text is written into
 * `out_refusal` (ABSTAIN class) or `out_hedge` (HEDGED class). Either buffer
 * may be NULL to skip rendering. When `out_policy_hit` is non-NULL and the
 * strictest match was a `HU_NEGATIVE_SOURCE_SYSTEM_POLICY` negative,
 * `*out_policy_hit` is set to `true` so callers can emit audit-log entries.
 *
 * `wm == NULL` or `wm->negatives_count == 0` returns SUPPORTED with no
 * writes to the output buffers.
 *
 * Shared by `response_verifier.c` and `self_rag_atomic.c` so both backends
 * use the same matcher and source-tag mapping. */
hu_verifier_outcome_t hu_negatives_scan_claim(const struct hu_world_model *wm,
                                              const char *claim,
                                              char *out_refusal, size_t refusal_cap,
                                              char *out_hedge, size_t hedge_cap,
                                              bool *out_policy_hit);

/* Pure helper: render a relation's bitemporal attribution into a 1-line
 * receipt string. Used by the verifier and by the response renderer. */
void hu_provenance_render(const hu_memory_relation_row_t *rel, char *buf, size_t cap);

#endif /* HU_AGENT_RESPONSE_VERIFIER_H */
