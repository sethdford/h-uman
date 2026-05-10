#ifndef HU_NEURAL_MEMORY_H
#define HU_NEURAL_MEMORY_H

/* W10 — Neural Memory Tier: KV-cache reuse, reasoning-trace memory, multimodal blobs.
 *
 * Three new memory kinds (HU_MEM_KV_CACHE, HU_MEM_REASONING_TRACE, HU_MEM_BLOB)
 * backed by SQLite tables.  All functions accept a hu_memory_facade_t facade and derive
 * the SQLite handle internally — callers never touch SQLite directly.
 *
 * Eviction (W14) and **provider short-circuit** (skip LLM when `blob` holds a
 * replayable assistant payload) are not implemented yet. `agent_turn.c` only
 * probes prior rows and stores `prompt_token_count` metadata after a successful
 * chat — see comments there.
 *
 * Gate: functions compile unconditionally; schema creation and SQL execution are
 * guarded by HU_ENABLE_SQLITE inside neural_memory.c.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/belief.h"
#include "human/memory/memory.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── KV-cache entry ───────────────────────────────────────────────────────── */

typedef struct hu_kv_cache_entry {
    char    prompt_hash[64];     /* sha256 hex of the prompt prefix */
    char    model_version[64];   /* invalidates when model upgrades */
    int64_t prompt_token_count;
    void   *blob;                /* opaque KV bytes; provider-specific */
    size_t  blob_len;
    int64_t created_at;          /* unix seconds; 0 → set to now on put */
} hu_kv_cache_entry_t;

/* ── Reasoning trace ──────────────────────────────────────────────────────── */

typedef struct hu_reasoning_trace {
    int64_t  id;
    char     goal_verb[64];
    int64_t *anchor_entity_ids;  /* alloc-owned; freed by hu_reasoning_traces_free */
    size_t   anchors_count;
    char    *cot_text;           /* chain-of-thought; alloc-owned */
    size_t   cot_len;
    char    *outcome;            /* nullable; alloc-owned */
    int64_t  recorded_at;
    hu_belief_t belief;          /* W8: how confident the trace was correct */
} hu_reasoning_trace_t;

/* ── Multimodal blob ──────────────────────────────────────────────────────── */

typedef struct hu_memory_blob {
    int64_t id;
    char    mime_type[64];       /* "image/png", "audio/wav", ... */
    void   *bytes;               /* alloc-owned */
    size_t  bytes_len;
    char   *caption;             /* lazy text description; nullable; alloc-owned */
    size_t  caption_len;
    int64_t created_at;
} hu_memory_blob_t;

/* ── KV-cache API ─────────────────────────────────────────────────────────── */

/* Look up a cached KV entry by (prompt_hash, model_version).
 * On HU_OK, *out is heap-allocated; caller must free with hu_kv_cache_entry_free. */
hu_error_t hu_kv_cache_get(hu_memory_facade_t *m, const char *prompt_hash,
                            const char *model_version,
                            hu_allocator_t *alloc, hu_kv_cache_entry_t **out);

/* Insert or replace a KV entry.  UNIQUE(prompt_hash, model_version) is
 * enforced: a duplicate put overwrites the previous blob. */
hu_error_t hu_kv_cache_put(hu_memory_facade_t *m, const hu_kv_cache_entry_t *entry);

/* Delete every KV entry for the given model_version.
 * W14 eviction hook: call when a model upgrade is detected. */
hu_error_t hu_kv_cache_invalidate_for_model(hu_memory_facade_t *m,
                                              const char *model_version);

/* Free an entry returned by hu_kv_cache_get. */
void hu_kv_cache_entry_free(hu_allocator_t *alloc, hu_kv_cache_entry_t *e);

/* ── Reasoning-trace API ──────────────────────────────────────────────────── */

/* Persist a reasoning trace.  *out_id receives the new row id. */
hu_error_t hu_reasoning_trace_record(hu_memory_facade_t *m, const char *contact_id,
                                      size_t cid_len,
                                      const hu_reasoning_trace_t *trace,
                                      int64_t *out_id);

/* Retrieve reasoning traces matching contact_id + goal_verb, optionally
 * filtered to those that share at least one anchor with the query set.
 * At most `limit` results are returned (0 → no limit, use 1000).
 * On HU_OK, *out is heap-allocated; caller frees with hu_reasoning_traces_free. */
hu_error_t hu_reasoning_trace_recall(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                      const char *contact_id, size_t cid_len,
                                      const char *goal_verb, size_t goal_len,
                                      const int64_t *anchors, size_t anchors_count,
                                      size_t limit,
                                      hu_reasoning_trace_t **out, size_t *out_count);

/* Free an array of traces returned by hu_reasoning_trace_recall. */
void hu_reasoning_traces_free(hu_allocator_t *alloc,
                               hu_reasoning_trace_t *traces, size_t n);

/* ── Multimodal blob API ──────────────────────────────────────────────────── */

/* Store a blob.  *out_id receives the new row id.
 * Returns HU_ERR_INVALID_ARGUMENT if blob->bytes_len > 100 MB. */
hu_error_t hu_memory_blob_put(hu_memory_facade_t *m, const char *contact_id,
                               size_t cid_len, const hu_memory_blob_t *blob,
                               int64_t *out_id);

/* Retrieve a blob by id.
 * On HU_OK, *out is heap-allocated; caller frees with hu_memory_blob_free. */
hu_error_t hu_memory_blob_get(hu_memory_facade_t *m, hu_allocator_t *alloc,
                               int64_t blob_id, hu_memory_blob_t **out);

/* Free a blob returned by hu_memory_blob_get. */
void hu_memory_blob_free(hu_allocator_t *alloc, hu_memory_blob_t *blob);

#ifdef __cplusplus
}
#endif

#endif /* HU_NEURAL_MEMORY_H */
