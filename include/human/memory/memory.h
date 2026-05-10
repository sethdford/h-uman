#ifndef HU_MEMORY_FACADE_H
#define HU_MEMORY_FACADE_H

/* W7 Memory Facade — single read/write/erase surface.
 *
 * Every consumer that wants a memory entry goes through `hu_memory_facade_t`.
 * Backends are vtables registered per `hu_memory_kind_t`. The default open()
 * registers a v1 backend that wraps existing graph/persona/cross_edges/case/
 * quarantine APIs, so callers can migrate one site at a time without behavior
 * drift.
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

/* Kind-specific query payloads (tagged-union; caller must set `kind` then
 * fill the matching struct). Keep this <= 64 bytes to discourage drift. */
typedef struct hu_memory_query {
    hu_memory_kind_t kind;
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
            int64_t id; /* generic id-based fetch */
        } by_id;
    } as;
} hu_memory_query_t;

/* Universal record carried back from read() and into write(). Kind-specific
 * payload sits in the opaque `payload` blob; consumers cast based on `kind`.
 * The non-payload fields (id, provenance, event window) are honored by every
 * backend so callers can reason about provenance without casting. */
typedef struct hu_memory_record {
    hu_memory_kind_t kind;
    int64_t id;
    char *provenance;       /* nullable; owned-by-record when read returns it */
    size_t provenance_len;
    int64_t event_start;
    int64_t event_end;
    float confidence;       /* 0.0-1.0; 1.0 default. W8 will replace with hu_belief_t. */
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

/* Lifecycle. `hu_memory_facade_open` registers the v1 backend for every kind v1
 * supports today (entity, relation, persona_delta, case, cross_edge,
 * quarantine). Other kinds return HU_ERR_NOT_SUPPORTED until a backend is
 * registered for them. */
hu_error_t hu_memory_facade_open(hu_allocator_t *alloc, hu_graph_t *graph, hu_memory_facade_t **out);
void hu_memory_facade_close(hu_memory_facade_t *m, hu_allocator_t *alloc);

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
