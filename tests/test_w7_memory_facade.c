/* W7 — Memory facade dispatcher + v1 backend round-trip + adversarial routing.
 *
 * Every test runs against an in-memory SQLite DB via hu_graph_open(NULL, 0).
 * The facade is a thin dispatcher; tests verify it routes correctly, returns
 * deterministic shapes, and refuses unsupported kinds. */

#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_facade(hu_graph_t **g, hu_memory_facade_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
}

static void close_facade(hu_graph_t *g, hu_memory_facade_t *m) {
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* --- backend registration / introspection --------------------------- */

static void test_w7_open_registers_v1_for_entity_and_relation(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    HU_ASSERT_NOT_NULL(hu_memory_facade_backend_name(m, HU_MEM_ENTITY));
    HU_ASSERT_NOT_NULL(hu_memory_facade_backend_name(m, HU_MEM_RELATION));
    HU_ASSERT_NOT_NULL(hu_memory_facade_backend_name(m, HU_MEM_HYPEREDGE));
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_ENTITY), "v1-entity"), 0);
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_RELATION), "v1-relation"), 0);
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_HYPEREDGE), "v1-hyperedge"), 0);

    /* Kinds without a backend yet (KV_CACHE, REASONING_TRACE, BLOB) still
     * return NULL until W10 lands the neural-memory backends. */
    HU_ASSERT_NULL(hu_memory_facade_backend_name(m, HU_MEM_KV_CACHE));
    HU_ASSERT_NULL(hu_memory_facade_backend_name(m, HU_MEM_REASONING_TRACE));

    HU_ASSERT_EQ(hu_memory_facade_graph_handle(m), g);

    close_facade(g, m);
}

static void test_w7_unsupported_kind_returns_not_supported(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    /* KV_CACHE has no backend yet (W10 not landed) — NOT_SUPPORTED is the
     * honest response. */
    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_KV_CACHE;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_ERR_NOT_SUPPORTED);

    close_facade(g, m);
}

static void test_w7_invalid_args_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    HU_ASSERT_EQ(hu_memory_facade_read(NULL, NULL, A(), NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_write(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_register_backend(NULL, HU_MEM_ENTITY, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_KIND_MAX, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    close_facade(g, m);
}

/* --- entity round-trip ---------------------------------------------- */

static int64_t insert_entity(hu_graph_t *g, const char *name) {
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, name, strlen(name), HU_ENTITY_PERSON,
                                         NULL, &id),
                 HU_OK);
    return id;
}

static void test_w7_entity_read_by_name_routes_to_v1(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = insert_entity(g, "Alice");
    HU_ASSERT_GT(alice, 0);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    q.as.by_name.name = "Alice";
    q.as.by_name.name_len = 5;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(out[0].kind, HU_MEM_ENTITY);
    HU_ASSERT_EQ(out[0].id, alice);
    HU_ASSERT_NOT_NULL(out[0].payload);

    /* Payload is hu_graph_entity_t. */
    hu_graph_entity_t *e = (hu_graph_entity_t *)out[0].payload;
    HU_ASSERT_EQ(e->id, alice);

    hu_memory_facade_records_free(m, A(), out, n);
    close_facade(g, m);
}

static void test_w7_entity_read_unknown_returns_zero(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    q.as.by_name.name = "Nobody";
    q.as.by_name.name_len = 6;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    /* find_entity returns HU_ERR_NOT_FOUND when the name doesn't exist; we
     * surface that honestly via the facade rather than masking as 0 results. */
    hu_error_t err = hu_memory_facade_read(m, &q, A(), &out, &n);
    HU_ASSERT(err == HU_ERR_NOT_FOUND || err == HU_OK);
    HU_ASSERT_EQ(n, 0);

    close_facade(g, m);
}

/* --- relation list round-trip --------------------------------------- */

static void test_w7_relation_list_routes_to_v1(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = insert_entity(g, "Alice");
    int64_t acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION,
                                         NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                           NULL, 0),
                 HU_OK);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_RELATION;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    /* leave window unset; backend defaults to list_relations top-K */

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(out[0].kind, HU_MEM_RELATION);
    HU_ASSERT_GT(out[0].id, 0);
    /* Bitemporal/W1 fields are populated. */
    HU_ASSERT_GT(out[0].event_start, 0);
    HU_ASSERT_EQ(out[0].event_end, 0);
    HU_ASSERT_FLOAT_EQ(out[0].confidence, 1.0f, 1e-3);

    hu_graph_relation_t *r = (hu_graph_relation_t *)out[0].payload;
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->source_id, alice);
    HU_ASSERT_EQ(r->target_id, acme);

    hu_memory_facade_records_free(m, A(), out, n);
    close_facade(g, m);
}

static void test_w7_relation_window_query_routes(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = insert_entity(g, "Alice");
    int64_t acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION,
                                         NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                              1735000000000LL, 0, 1.0f, NULL, 0,
                                              "test", 4),
                 HU_OK);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_RELATION;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    q.as.window.from_ts = 1734000000000LL;
    q.as.window.to_ts   = 1736000000000LL;
    q.as.window.limit = 16;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(out[0].kind, HU_MEM_RELATION);

    hu_memory_facade_records_free(m, A(), out, n);
    close_facade(g, m);
}

/* --- backend swap (architectural test) ------------------------------ */

static int s_stub_read_calls = 0;
static hu_error_t stub_read(void *ctx, const hu_memory_query_t *q, hu_allocator_t *alloc,
                             hu_memory_record_t **out, size_t *out_count) {
    (void)ctx; (void)q; (void)alloc;
    s_stub_read_calls++;
    *out = NULL;
    *out_count = 0;
    return HU_OK;
}
static void stub_records_free(void *ctx, hu_allocator_t *alloc,
                               hu_memory_record_t *r, size_t n) {
    (void)ctx; (void)alloc; (void)r; (void)n;
}
static void stub_deinit(void *ctx) { (void)ctx; }

static void test_w7_register_backend_replaces_existing(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    static hu_memory_facade_vtable_t stub_vt = {
        .name = "stub",
        .read = stub_read,
        .records_free = stub_records_free,
        .deinit = stub_deinit,
    };
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &stub_vt, NULL), HU_OK);
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_ENTITY), "stub"), 0);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    q.as.by_name.name = "Alice";
    q.as.by_name.name_len = 5;

    s_stub_read_calls = 0;
    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_OK);
    HU_ASSERT_EQ(s_stub_read_calls, 1);

    close_facade(g, m);
}

/* Replacing only the entity slot must not free the shared v1 ctx while
 * relation/hyperedge backends still reference it (regression: flaky NULL
 * hu_memory_facade_backend_name for HU_MEM_HYPEREDGE after partial swap). */
static void test_w7_stub_entity_slot_keeps_hyperedge_backend(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    static hu_memory_facade_vtable_t stub_vt = {
        .name = "stub",
        .read = stub_read,
        .records_free = stub_records_free,
        .deinit = stub_deinit,
    };
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &stub_vt, NULL), HU_OK);

    HU_ASSERT_NOT_NULL(hu_memory_facade_backend_name(m, HU_MEM_HYPEREDGE));
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_HYPEREDGE), "v1-hyperedge"), 0);
    HU_ASSERT_NOT_NULL(hu_memory_facade_backend_name(m, HU_MEM_RELATION));
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_RELATION), "v1-relation"), 0);

    close_facade(g, m);
}

/* --- P2C: memory_facade_routes table persistence -------------------- */

static void test_w7_routes_persisted_after_open(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    /* Open populates routes for the v1 backend kinds. We expect the
     * kind→backend_name mapping to round-trip through SQLite. */
    char *entity_route = hu_memory_facade_route_lookup(m, HU_MEM_ENTITY, A());
    char *relation_route = hu_memory_facade_route_lookup(m, HU_MEM_RELATION, A());
    char *hyperedge_route = hu_memory_facade_route_lookup(m, HU_MEM_HYPEREDGE, A());

    HU_ASSERT_NOT_NULL(entity_route);
    HU_ASSERT_NOT_NULL(relation_route);
    HU_ASSERT_NOT_NULL(hyperedge_route);
    HU_ASSERT_EQ(strcmp(entity_route, "v1-entity"), 0);
    HU_ASSERT_EQ(strcmp(relation_route, "v1-relation"), 0);
    HU_ASSERT_EQ(strcmp(hyperedge_route, "v1-hyperedge"), 0);

    A()->free(A()->ctx, entity_route, strlen(entity_route) + 1);
    A()->free(A()->ctx, relation_route, strlen(relation_route) + 1);
    A()->free(A()->ctx, hyperedge_route, strlen(hyperedge_route) + 1);

    /* Kinds without a backend bind nothing. */
    char *kv_route = hu_memory_facade_route_lookup(m, HU_MEM_KV_CACHE, A());
    HU_ASSERT_NULL(kv_route);

    close_facade(g, m);
}

static void test_w7_routes_replaced_on_register(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    static hu_memory_facade_vtable_t stub_vt = {
        .name = "stub",
        .read = stub_read,
        .records_free = stub_records_free,
        .deinit = stub_deinit,
    };
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &stub_vt, NULL), HU_OK);

    char *route = hu_memory_facade_route_lookup(m, HU_MEM_ENTITY, A());
    HU_ASSERT_NOT_NULL(route);
    HU_ASSERT_EQ(strcmp(route, "stub"), 0);
    A()->free(A()->ctx, route, strlen(route) + 1);

    close_facade(g, m);
}

/* --- adversarial: replacing entity backend doesn't crash on close --- */

static void test_w7_replace_then_close_cleans_up(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    static hu_memory_facade_vtable_t stub_vt = {
        .name = "stub",
        .read = stub_read,
        .records_free = stub_records_free,
        .deinit = stub_deinit,
    };
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &stub_vt, NULL), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_RELATION, &stub_vt, NULL), HU_OK);
    /* If close mishandled the now-orphan v1 ctx, ASan flags a leak. */
    close_facade(g, m);
}

#endif /* HU_ENABLE_SQLITE */

void run_w7_memory_facade_tests(void) {
    HU_TEST_SUITE("W7 memory facade - dispatcher + v1 backend");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w7_open_registers_v1_for_entity_and_relation);
    HU_RUN_TEST(test_w7_unsupported_kind_returns_not_supported);
    HU_RUN_TEST(test_w7_invalid_args_rejected);
    HU_RUN_TEST(test_w7_entity_read_by_name_routes_to_v1);
    HU_RUN_TEST(test_w7_entity_read_unknown_returns_zero);
    HU_RUN_TEST(test_w7_relation_list_routes_to_v1);
    HU_RUN_TEST(test_w7_relation_window_query_routes);
    HU_RUN_TEST(test_w7_register_backend_replaces_existing);
    HU_RUN_TEST(test_w7_stub_entity_slot_keeps_hyperedge_backend);
    HU_RUN_TEST(test_w7_routes_persisted_after_open);
    HU_RUN_TEST(test_w7_routes_replaced_on_register);
    HU_RUN_TEST(test_w7_replace_then_close_cleans_up);
#endif
}
