#ifndef HU_MEMORY_FACADE_H
#define HU_MEMORY_FACADE_H

/* W7 Memory Facade — single read/write/erase surface.
 *
 * Every consumer that wants a memory entry goes through `hu_memory_facade_t`.
 * Backends are vtables registered per `hu_memory_kind_t`. The default open()
 * registers a v1 backend for entity, relation, hyperedge, and (when SQLite is
 * enabled) case kinds, so callers can migrate one site at a time without
 * behavior drift.
 *
 * Layer contract: this is layer 1 of the v2 stack (see
 * docs/plans/2026-05-10-memory-v2-roadmap-overview.md). It dispatches; it does
 * not own storage. Backends own storage.
 *
 * Naming: `hu_memory_facade_*` / `hu_memory_facade_t` are distinct from legacy
 * vector-store `hu_memory_t` in `human/memory.h` (Phase 0 collision fix).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HU_MEM_RELATION read: set `q.as.by_id.id` to this sentinel and optional
 * `q.as.by_id.limit` (default 64) to fetch open-interval relations
 * (`event_end = 0`) ordered by `last_seen` with endpoint entity names
 * populated for response-path verification. */
#define HU_MEMORY_REL_VERIFIER_SCAN ((int64_t)-1)

/* Payload for HU_MEM_CASE records (read returns owned strings; write may use
 * borrowed pointers that must outlive hu_memory_facade_write). */
typedef struct hu_memory_case_payload {
    char *goal_verb;
    size_t goal_verb_len;
    int64_t *anchor_entity_ids;
    size_t anchor_count;
    char *plan_text;
    size_t plan_text_len;
    char *outcome;
    size_t outcome_len;
    int64_t happened_at;
} hu_memory_case_payload_t;

/* v1 backends store entity/relation rows in graph layout; facade reads may
 * expose the same structs as `payload` or via list_entities. Prefer these
 * aliases in W7-first agent code over repeating `hu_graph_*` at cast sites. */
typedef hu_graph_entity_t hu_memory_entity_row_t;
typedef hu_graph_relation_t hu_memory_relation_row_t;

/* Discriminator for queries / records. Each kind maps to one backend at a time
 * (registered via hu_memory_facade_register_backend). New kinds extend this enum at
 * the bottom; values are stable for on-disk routing tables. */
typedef enum hu_memory_kind {
    HU_MEM_ENTITY = 0,
    HU_MEM_RELATION = 1,
    HU_MEM_HYPEREDGE = 2,        /* W8 */
    HU_MEM_PERSONA_DELTA = 3,    /* v1 W5 */
    HU_MEM_CASE = 4,             /* v1 W3 */
    HU_MEM_CROSS_EDGE = 5,       /* v1 W3 */
    HU_MEM_QUARANTINE = 6,       /* v1 W1 */
    HU_MEM_KV_CACHE = 7,         /* W10 */
    HU_MEM_REASONING_TRACE = 8,  /* W10 */
    HU_MEM_BLOB = 9,             /* W10 multimodal */
    HU_MEM_KIND_MAX
} hu_memory_kind_t;

/* Sub-discriminator for the `as` union below. Entity queries can ask for
 * neighbors (graph traversal) or by_name (canonical lookup); the two share
 * union storage so we need an explicit tag to tell them apart — the union
 * members alias (e.g. neighbors.entity_id and by_name.name both occupy
 * bytes 0..7). Default is HU_MEMORY_QUERY_AUTO for backward compat: the
 * backend infers the variant from non-zero fields, which is risky but
 * matches pre-tag behavior. New callers should set this explicitly. */
typedef enum hu_memory_query_variant {
    HU_MEMORY_QUERY_AUTO      = 0, /* legacy heuristic; backend infers */
    HU_MEMORY_QUERY_BY_NAME   = 1,
    HU_MEMORY_QUERY_NEIGHBORS = 2,
    HU_MEMORY_QUERY_WINDOW    = 3,
    HU_MEMORY_QUERY_BY_ID     = 4,
    HU_MEMORY_QUERY_KV        = 5,
    HU_MEMORY_QUERY_CASE      = 6,
} hu_memory_query_variant_t;

/* Kind-specific query payloads (tagged-union; caller must set `kind` then
 * fill the matching struct). Keep this <= 64 bytes to discourage drift. */
typedef struct hu_memory_query {
    hu_memory_kind_t kind;
    hu_memory_query_variant_t variant; /* picks the union variant below */
    const char *contact_id;
    size_t contact_id_len;
    union {
        struct {
            int64_t entity_id;
            size_t hops;
            size_t limit;
        } neighbors;
        struct {
            const char *name;
            size_t name_len;
        } by_name;
        struct {
            int64_t from_ts;
            int64_t to_ts;
            size_t limit;
        } window;
        struct {
            const char *prompt_hash;
            size_t hash_len;
            const char *model_version;
            size_t model_version_len;
        } kv;
        struct {
            const char *goal_verb;
            size_t goal_len;
            const int64_t *anchors;
            size_t anchors_len;
            size_t limit;
        } cases;
        struct {
            int64_t id; /* generic id-based fetch; see HU_MEMORY_REL_VERIFIER_SCAN */
            size_t limit; /* used with HU_MEM_RELATION + HU_MEMORY_REL_VERIFIER_SCAN */
        } by_id;
    } as;
} hu_memory_query_t;

/* Universal record carried back from read() and into write(). Kind-specific
 * payload sits in the opaque `payload` blob; consumers cast based on `kind`.
 * The non-payload fields (id, provenance, event window, contact scope) are
 * honored by every backend so callers can reason about provenance without
 * casting. */
typedef struct hu_memory_record {
    hu_memory_kind_t kind;
    int64_t id;
    /* Optional contact scope. Backends MUST honor this when present — the v1
     * graph backend, for example, uses it as the SQL `contact_id` column.
     * Length is the byte count without a trailing NUL; pass NULL/0 for
     * unscoped writes (e.g. global hyperedges). */
    const char *contact_id;
    size_t contact_id_len;
    char *provenance;       /* nullable; owned-by-record when read returns it */
    size_t provenance_len;
    int64_t event_start;
    int64_t event_end;
    float confidence;       /* 0.0-1.0; 1.0 default. W8 mean estimate. */
    /* P2G — W8 Bayesian belief variance. 0.0 == fully certain. Backends MUST
     * honor this when present. If `confidence_variance < 0`, the backend
     * derives a default from provenance via
     * `hu_belief_initial_variance_for_provenance`. */
    float confidence_variance;
    void *payload;          /* kind-specific struct; cast via `kind`. */
    size_t payload_len;
} hu_memory_record_t;

/* Backend vtable. Implementors should not free `payload` themselves — the
 * facade calls `records_free` when the caller is done. */
typedef struct hu_memory_facade_vtable {
    const char *name;
    hu_error_t (*read)(void *ctx, const hu_memory_query_t *q, hu_allocator_t *alloc,
                       hu_memory_record_t **out, size_t *out_count);
    hu_error_t (*write)(void *ctx, const hu_memory_record_t *rec);
    hu_error_t (*erase)(void *ctx, hu_memory_kind_t kind, int64_t id);
    hu_error_t (*erase_by_provenance)(void *ctx, const char *substring, size_t len);
    void (*records_free)(void *ctx, hu_allocator_t *alloc, hu_memory_record_t *r, size_t n);
    void (*deinit)(void *ctx);
} hu_memory_facade_vtable_t;

typedef struct hu_memory_facade hu_memory_facade_t;

/* Lifecycle. `hu_memory_facade_open` registers the v1 backend for entity,
 * relation, hyperedge, and (when SQLite is enabled) case_records. Other kinds
 * return HU_ERR_NOT_SUPPORTED until a backend is registered for them. */
hu_error_t hu_memory_facade_open(hu_allocator_t *alloc, hu_graph_t *graph, hu_memory_facade_t **out);
hu_error_t hu_memory_facade_open_on_graph(hu_allocator_t *alloc, struct hu_graph *graph,
                                          hu_memory_facade_t **out);
void hu_memory_facade_close(hu_memory_facade_t *m, hu_allocator_t *alloc);

/* W15 — generic audit hook for write/erase ops. The facade invokes this
 * callback after a successful backend write or erase so that upper layers
 * (e.g. security/audit_log) can record the event without introducing a
 * cross-layer dependency from memory (L1) into security (L6). */
typedef enum hu_memory_audit_op {
    HU_MEMORY_AUDIT_WRITE = 0,
    HU_MEMORY_AUDIT_ERASE = 1,
} hu_memory_audit_op_t;

typedef void (*hu_memory_audit_fn)(void *ctx, hu_memory_audit_op_t op,
                                   hu_memory_kind_t kind, int64_t id);

void hu_memory_facade_set_audit_hook(hu_memory_facade_t *m,
                                     hu_memory_audit_fn fn, void *ctx);

/* Backend registration. Replaces an existing backend for `kind` if present.
 * The previous backend's deinit() is called. Caller retains ownership of `vt`
 * (must outlive the facade); facade owns `ctx` after registration and calls
 * `deinit` on it at close. */
hu_error_t hu_memory_facade_register_backend(hu_memory_facade_t *m, hu_memory_kind_t kind,
                                             hu_memory_facade_vtable_t *vt, void *ctx);

/* Dispatching API. */
hu_error_t hu_memory_facade_read(hu_memory_facade_t *m, const hu_memory_query_t *q, hu_allocator_t *alloc,
                                hu_memory_record_t **out, size_t *out_count);
hu_error_t hu_memory_facade_write(hu_memory_facade_t *m, const hu_memory_record_t *rec);

/* After a successful `hu_memory_facade_write` with `rec->kind == HU_MEM_CASE`, returns
 * `sqlite3_last_insert_rowid()` for the graph connection (0 if unavailable). The facade
 * clears this to 0 at the start of every `hu_memory_facade_write` call, then refreshes it
 * only when the case write succeeds — best-effort; do not rely across concurrent writers. */
int64_t hu_memory_facade_last_case_rowid(const hu_memory_facade_t *m);

hu_error_t hu_memory_facade_erase(hu_memory_facade_t *m, hu_memory_kind_t kind, int64_t id);

/* Cross-backend purge by provenance substring. Distinct from the v1 helper
 * `hu_memory_erase_by_provenance(hu_graph_t*, ...)` in erasure.h: that walks
 * the graph only; this fans out across every registered backend whose
 * vtable implements `erase_by_provenance`. */
hu_error_t hu_memory_facade_purge_by_provenance(hu_memory_facade_t *m, const char *substring, size_t len);

/* Free a record array previously returned by `hu_memory_facade_read`. Routes back to
 * the originating backend's `records_free`. Calling with `n == 0` is a no-op. */
void hu_memory_facade_records_free(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                   hu_memory_record_t *r, size_t n);

/* Introspection: name of the backend currently bound to `kind`, or NULL if
 * none. Returned pointer is owned by the facade; do not free. */
const char *hu_memory_facade_backend_name(hu_memory_facade_t *m, hu_memory_kind_t kind);

/* W7 P2C — read the persisted (kind -> backend_name) edge from the
 * memory_facade_routes table. Returns a freshly allocated string the
 * caller must free via the same `alloc`, or NULL if the row does not
 * exist (or the SQLite layer is disabled). Used by operators and
 * upgrade-time route-mismatch detection. */
char *hu_memory_facade_route_lookup(hu_memory_facade_t *m, hu_memory_kind_t kind,
                                    hu_allocator_t *alloc);

/* Underlying graph handle for the v1 backend. Exposed so existing callers can
 * migrate incrementally without losing access to graph-only APIs (community
 * detection, Leiden, etc). New code should prefer the facade. */
hu_graph_t *hu_memory_facade_graph_handle(hu_memory_facade_t *m);

/* Open / close a v1 on-disk graph (delegates to `hu_graph_open` / `hu_graph_close`).
 * Surfaces that only hold `struct hu_graph *` can use these instead of spelling
 * `hu_graph_*` at every call site. */
hu_error_t hu_memory_v1_graph_open(hu_allocator_t *alloc, const char *db_path, size_t db_path_len,
                                   struct hu_graph **out);
void hu_memory_v1_graph_close(struct hu_graph *g, hu_allocator_t *alloc);

/* Delegates to `hu_graph_upsert_relation_with_belief` for graph-only promotion paths. */
hu_error_t hu_memory_v1_upsert_relation_with_belief(
    struct hu_graph *g, const char *contact_id, size_t contact_id_len,
    int64_t source_id, int64_t target_id, hu_relation_type_t type,
    float weight, int64_t event_start, int64_t event_end,
    float belief_mean, float belief_variance,
    const char *context, size_t context_len,
    const char *provenance, size_t provenance_len,
    int64_t *out_id);

#ifdef HU_ENABLE_SQLITE
/* Shared SQLite connection backing the v1 graph (scheduler DDL, counterfactual
 * replays, etc.). Returns NULL if the facade has no graph or SQLite is
 * unavailable. Prefer `hu_memory_facade_read` / `write` for memory rows. */
struct sqlite3;
struct sqlite3 *hu_memory_facade_sqlite_db(hu_memory_facade_t *m);

/* Same connection as `hu_graph_sqlite_connection` for a v1-opened graph.
 * Lets modules that only need raw SQL avoid spelling `hu_graph_sqlite_*`
 * at every call site when a facade handle is not available. */
struct sqlite3 *hu_memory_sqlite_from_graph(struct hu_graph *g);
#endif

/* List all entities for a contact through the facade. Convenience wrapper
 * that delegates to the underlying graph handle. Callers should prefer this
 * over hu_memory_facade_graph_handle + hu_graph_list_entities directly so
 * the facade remains the single entry point. Free results with
 * hu_memory_facade_free_listed_entities. */
hu_error_t hu_memory_facade_list_entities(hu_memory_facade_t *m,
                                          hu_allocator_t *alloc,
                                          const char *contact_id,
                                          size_t cid_len,
                                          size_t limit,
                                          hu_graph_entity_t **out,
                                          size_t *out_count);

/* Frees arrays returned by hu_memory_facade_list_entities (delegates to the
 * graph helper; `m` is reserved for future invariant checks). */
void hu_memory_facade_free_listed_entities(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                           hu_memory_entity_row_t *entities, size_t count);

/* Markdown digests over `temporal_events` / `causal_links` for the given
 * contact scope. Thin wrappers on the v1 graph connection; prefer these over
 * `hu_memory_facade_graph_handle` + `hu_graph_query_*` in migrating call sites.
 * Output buffers are allocator-owned (same contract as the underlying graph
 * helpers). */
hu_error_t hu_memory_facade_query_temporal(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                            const char *contact_id, size_t contact_id_len,
                                            int64_t from_ts, int64_t to_ts, size_t limit,
                                            char **out, size_t *out_len);
hu_error_t hu_memory_facade_query_causal(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        int64_t entity_id, size_t max_results, char **out,
                                        size_t *out_len);

/* W8 / W14 — read or UPDATE-by-id belief columns on an existing relation row.
 * Delegates to `hu_graph_get_relation_belief` / `hu_graph_set_relation_belief`.
 * Prefer over `hu_memory_facade_graph_handle` + graph calls in migrating runners. */
hu_error_t hu_memory_facade_get_relation_belief(hu_memory_facade_t *m, int64_t relation_id,
                                                float *out_mean, float *out_variance);
hu_error_t hu_memory_facade_set_relation_belief(hu_memory_facade_t *m, int64_t relation_id,
                                                float mean, float variance,
                                                int64_t last_seen_now_ms);

/* W15 GDPR data-portability: export all memory records across every registered
 * kind to a JSON-Lines file at `output_path`. Each line is a self-contained
 * JSON object with { "kind", "id", "provenance", "confidence", "payload_len" }.
 * File I/O is guarded: under HU_IS_TEST the function writes to the provided
 * path (callers should use /tmp). Returns HU_ERR_IO on write failure. */
hu_error_t hu_memory_facade_export_json(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                        const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* HU_MEMORY_FACADE_H */
