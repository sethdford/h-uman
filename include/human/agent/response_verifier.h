#ifndef HU_AGENT_RESPONSE_VERIFIER_H
#define HU_AGENT_RESPONSE_VERIFIER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
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
 *   - HU_VERIFY_OFF    Log only; never modify the draft.
 *   - HU_VERIFY_SOFT   Prepend a hedge to flagged claims; attach provenance.
 *   - HU_VERIFY_STRICT Rewrite the draft via corrective-RAG. */

typedef enum hu_verify_mode {
    HU_VERIFY_OFF = 0,
    HU_VERIFY_SOFT = 1,
    HU_VERIFY_STRICT = 2,
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
} hu_verifier_config_t;

typedef struct hu_verifier_report {
    size_t claims_extracted;
    size_t claims_supported;
    size_t claims_flagged;
    bool draft_modified;        /* true when SOFT/STRICT made changes */
    char modified_draft[2048];  /* populated when draft_modified is true */
    hu_verifier_claim_t claims[16];
} hu_verifier_report_t;

hu_verifier_config_t hu_verifier_default_config(void);

/* Run synchronous verification over a draft. The caller passes the draft text
 * plus a contact_id to scope the graph queries. Returns HU_OK and fills
 * out_report on success. The graph is NEVER modified by this call. */
hu_error_t hu_response_verify(hu_allocator_t *alloc, hu_graph_t *graph, const char *contact_id,
                              size_t contact_id_len, const char *draft, size_t draft_len,
                              const hu_verifier_config_t *cfg, hu_verifier_report_t *out_report);

/* Pure helper: render a relation's bitemporal attribution into a 1-line
 * receipt string. Used by the verifier and by the response renderer. */
void hu_provenance_render(const hu_graph_relation_t *rel, char *buf, size_t cap);

#endif /* HU_AGENT_RESPONSE_VERIFIER_H */
