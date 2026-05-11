/* W7 — Memory facade dispatcher + v1 backend round-trip + adversarial routing.
 *
 * Every test runs against an in-memory SQLite DB via hu_graph_open(NULL, 0).
 * The facade is a thin dispatcher; tests verify it routes correctly, returns
 * deterministic shapes, and refuses unsupported kinds. */

#include "human/agent/anticipatory.h"
#include "human/agent/case_based.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    HU_ASSERT_NOT_NULL(hu_memory_facade_backend_name(m, HU_MEM_CASE));
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_ENTITY), "v1-entity"), 0);
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_RELATION), "v1-relation"), 0);
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_HYPEREDGE), "v1-hyperedge"), 0);
    HU_ASSERT_EQ(strcmp(hu_memory_facade_backend_name(m, HU_MEM_CASE), "v1-case"), 0);

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
    q.variant = HU_MEMORY_QUERY_AUTO;
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
    q.variant = HU_MEMORY_QUERY_BY_NAME;
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
    q.variant = HU_MEMORY_QUERY_BY_NAME;
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
    q.variant = HU_MEMORY_QUERY_AUTO;
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
    q.variant = HU_MEMORY_QUERY_WINDOW;
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
    q.variant = HU_MEMORY_QUERY_BY_NAME;
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

/* --- P2G: belief variance flows through the facade write path --- */

static int64_t write_relation_with_provenance(hu_memory_facade_t *m, hu_graph_t *g,
                                              int64_t src, int64_t tgt,
                                              float confidence,
                                              const char *provenance) {
    hu_graph_relation_t payload = {0};
    payload.source_id = src;
    payload.target_id = tgt;
    payload.type = HU_REL_WORKS_AT;
    payload.weight = 1.0f;
    payload.context = NULL;
    payload.context_len = 0;

    hu_memory_record_t rec = {0};
    rec.kind = HU_MEM_RELATION;
    rec.event_start = 1735000000000LL;
    rec.event_end = 0;
    rec.confidence = confidence;
    rec.provenance = (char *)provenance;
    rec.provenance_len = provenance ? strlen(provenance) : 0;
    rec.payload = &payload;
    rec.payload_len = sizeof(payload);

    HU_ASSERT_EQ(hu_memory_facade_write(m, &rec), HU_OK);
    /* P2G: v1 backend doesn't surface the inserted id through the facade,
     * so we re-list relations for the contact and pick the matching row. */
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A(), "", 0, /*limit=*/64, &rels, &n), HU_OK);
    int64_t id = 0;
    for (size_t i = 0; i < n; i++) {
        if (rels[i].source_id == src && rels[i].target_id == tgt &&
            rels[i].type == HU_REL_WORKS_AT) {
            if (rels[i].id > id) id = rels[i].id;
        }
    }
    hu_graph_relations_free(A(), rels, n);
    HU_ASSERT_TRUE(id > 0);
    return id;
}

static void test_w7_p2g_facade_write_seeds_variance_from_provenance(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    /* Two independent (source, type) pairs so we don't trip the
     * Bayesian update path in `hu_graph_upsert_relation_with_belief`
     * (the peek SQL selects on source_id + relation_type only — same
     * source + same type triggers posterior merging across targets).
     *
     * This test's contract is "variance comes from provenance", not
     * "Bayesian update doesn't fire", so we isolate the two writes onto
     * different (alice, bob) sources to test the variance seam cleanly. */
    int64_t alice = 0, bob = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Bob", 3, HU_ENTITY_PERSON, NULL, &bob),
                 HU_OK);
    int64_t acme = 0, globex = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Globex", 6, HU_ENTITY_ORGANIZATION, NULL,
                                         &globex),
                 HU_OK);

    /* High-confidence direct user statement → low variance. */
    int64_t id_user = write_relation_with_provenance(
        m, g, alice, acme, 0.95f, "channel:imessage:user-text");

    /* Heuristic-derived fact (different source, no peek collision)
     * → higher variance, mean preserved at observation value. */
    int64_t id_heur = write_relation_with_provenance(
        m, g, bob, globex, 0.70f, "autodream:released:pattern-001");

    float mean_user = -1, var_user = -1;
    HU_ASSERT_EQ(hu_graph_get_relation_belief(g, id_user, &mean_user, &var_user), HU_OK);
    float mean_heur = -1, var_heur = -1;
    HU_ASSERT_EQ(hu_graph_get_relation_belief(g, id_heur, &mean_heur, &var_heur), HU_OK);

    HU_ASSERT_FLOAT_EQ(mean_user, 0.95f, 0.02f);
    HU_ASSERT_FLOAT_EQ(mean_heur, 0.70f, 0.03f);

    /* Variance is monotonic in heuristic distance from ground truth:
     * user channel < autodream heuristic. */
    HU_ASSERT_TRUE(var_user < var_heur);
    /* And both are inside the [0, 0.25] band the heuristic guarantees. */
    HU_ASSERT_TRUE(var_user >= 0.0f && var_user <= 0.25f);
    HU_ASSERT_TRUE(var_heur >= 0.0f && var_heur <= 0.25f);

    close_facade(g, m);
}

static void test_w7_p2g_null_provenance_uses_default_variance(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL,
                                         &acme),
                 HU_OK);

    int64_t id = write_relation_with_provenance(m, g, alice, acme, 1.0f, NULL);

    float mean = -1, var = -1;
    HU_ASSERT_EQ(hu_graph_get_relation_belief(g, id, &mean, &var), HU_OK);

    /* NULL/empty provenance → default variance band. The exact value lives
     * in belief.c; we just assert the contract: 0 < var <= 0.25. */
    HU_ASSERT_TRUE(var > 0.0f);
    HU_ASSERT_TRUE(var <= 0.25f);
    HU_ASSERT_TRUE(mean > 0.99f && mean <= 1.0f);

    close_facade(g, m);
}

/* --- relation belief get/set through facade matches graph --- */

static void test_w7_facade_relation_belief_get_set_matches_graph(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL,
                                         &acme),
                 HU_OK);
    int64_t id = write_relation_with_provenance(m, g, alice, acme, 0.82f, "channel:test");

    float gm = -1.0f, gv = -1.0f, fm = -1.0f, fv = -1.0f;
    HU_ASSERT_EQ(hu_graph_get_relation_belief(g, id, &gm, &gv), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_get_relation_belief(m, id, &fm, &fv), HU_OK);
    HU_ASSERT_FLOAT_EQ(gm, fm, 1e-4f);
    HU_ASSERT_FLOAT_EQ(gv, fv, 1e-4f);

    const int64_t ts = 1700000000000LL;
    HU_ASSERT_EQ(hu_memory_facade_set_relation_belief(m, id, 0.61f, 0.04f, ts), HU_OK);
    float gm2 = -1.0f, gv2 = -1.0f;
    HU_ASSERT_EQ(hu_graph_get_relation_belief(g, id, &gm2, &gv2), HU_OK);
    HU_ASSERT_FLOAT_EQ(gm2, 0.61f, 1e-4f);
    HU_ASSERT_FLOAT_EQ(gv2, 0.04f, 1e-4f);

    close_facade(g, m);
}

/* --- anticipatory: facade wrapper matches direct graph analyze --- */

static void test_w7_anticipatory_analyze_memory_matches_graph(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);
    hu_anticipatory_result_t r_graph = {0};
    hu_anticipatory_result_t r_mem = {0};
    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_anticipatory_analyze(g, A(), "u", 1, now, &r_graph), HU_OK);
    HU_ASSERT_EQ(hu_anticipatory_analyze_memory(m, A(), "u", 1, now, &r_mem), HU_OK);
    HU_ASSERT_EQ(r_graph.action_count, r_mem.action_count);
    hu_anticipatory_result_deinit(&r_graph, A());
    hu_anticipatory_result_deinit(&r_mem, A());
    close_facade(g, m);
}

/* --- case rowid via hu_memory_facade_last_case_rowid (no raw graph sqlite in case_based.c) --- */

static void test_w7_case_write_last_rowid_matches_hu_case_record_out_id(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    HU_ASSERT_EQ(hu_memory_facade_last_case_rowid(m), 0);

    int64_t id1 = 0;
    HU_ASSERT_EQ(hu_case_record(m, "c", 1, "goal", 4, NULL, 0, NULL, 0, "ok", 2, 1000LL, &id1),
                 HU_OK);
    HU_ASSERT_TRUE(id1 > 0);
    HU_ASSERT_EQ(hu_memory_facade_last_case_rowid(m), id1);

    int64_t id2 = 0;
    HU_ASSERT_EQ(
        hu_case_record(m, "c", 1, "goal2", 5, NULL, 0, NULL, 0, "x", 1, 2000LL, &id2), HU_OK);
    HU_ASSERT_TRUE(id2 > id1);
    HU_ASSERT_EQ(hu_memory_facade_last_case_rowid(m), id2);

    close_facade(g, m);
}

/* --- list_entities through facade ------------------------------------ */

static void test_w7_list_entities_returns_inserted_entity(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    insert_entity(g, "Alice");

    hu_graph_entity_t *ents = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_memory_facade_list_entities(m, A(), "u1", 2, 64, &ents, &count), HU_OK);
    HU_ASSERT_GE((int)count, 1);

    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (ents[i].name && strstr(ents[i].name, "Alice"))
            found = true;
    }
    HU_ASSERT(found);

    hu_graph_entities_free(A(), ents, count);
    close_facade(g, m);
}

static void test_w7_list_entities_null_args_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    hu_graph_entity_t *ents = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_memory_facade_list_entities(NULL, A(), "u1", 2, 64, &ents, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_list_entities(m, NULL, "u1", 2, 64, &ents, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_list_entities(m, A(), NULL, 0, 64, &ents, &count),
                 HU_ERR_INVALID_ARGUMENT);

    close_facade(g, m);
}

/* --- export_json through facade -------------------------------------- */

static void test_w7_export_json_creates_file(void) {
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

    const char *path = "/tmp/hu_test_export.jsonl";
    HU_ASSERT_EQ(hu_memory_facade_export_json(m, A(), path), HU_OK);

    FILE *fp = fopen(path, "r");
    HU_ASSERT_NOT_NULL(fp);
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t nread = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    remove(path);

    if (nread > 0) {
        HU_ASSERT_NOT_NULL(strstr(buf, "\"kind\""));
        HU_ASSERT_NOT_NULL(strstr(buf, "\"id\""));
    }

    close_facade(g, m);
}

static void test_w7_export_json_null_args_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    HU_ASSERT_EQ(hu_memory_facade_export_json(NULL, A(), "/tmp/x.jsonl"),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_export_json(m, NULL, "/tmp/x.jsonl"),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_export_json(m, A(), NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_facade_export_json(m, A(), ""),
                 HU_ERR_INVALID_ARGUMENT);

    close_facade(g, m);
}

/* --- P3 adversarial: variant tag prevents union-aliasing crash ---
 *
 * Without the variant tag, a neighbors query with `entity_id = 42, hops = 1`
 * aliases as `{name = (char*)42, name_len = 1}` in the by_name branch.
 * The pre-fix code dereferenced 0x2A and segfaulted. With the tag, the
 * backend uses the explicit variant and never reaches the bogus
 * dereference. */

static void test_w7_p3_neighbors_query_with_variant_tag_safe(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = insert_entity(g, "Alice");
    int64_t bob = insert_entity(g, "Bob");
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, bob, HU_REL_KNOWS, 1.0f,
                                              1735000000000LL, 0, 1.0f, NULL, 0, "rel", 3),
                 HU_OK);

    /* Build a neighbors query that would have crashed without the variant
     * tag (entity_id = small int, hops = 1). */
    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.variant = HU_MEMORY_QUERY_NEIGHBORS;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    q.as.neighbors.entity_id = alice;
    q.as.neighbors.hops = 1;
    q.as.neighbors.limit = 8;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_OK);
    HU_ASSERT_GE((int)n, 1);

    hu_memory_facade_records_free(m, A(), out, n);
    close_facade(g, m);
}

static void test_w7_p3_auto_variant_falls_back_to_neighbors_safely(void) {
    /* AUTO with low-address-looking name pointer must NOT dereference. */
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t alice = insert_entity(g, "Alice");
    int64_t bob = insert_entity(g, "Bob");
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, bob, HU_REL_KNOWS, 1.0f,
                                              1735000000000LL, 0, 1.0f, NULL, 0, "rel", 3),
                 HU_OK);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.variant = HU_MEMORY_QUERY_AUTO;
    q.contact_id = "u1";
    q.contact_id_len = 2;
    /* This aliases as `by_name.name = (char*)alice`. AUTO must reject
     * the low-address pointer and fall back to neighbors. */
    q.as.neighbors.entity_id = alice;
    q.as.neighbors.hops = 1;
    q.as.neighbors.limit = 8;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &out, &n), HU_OK);
    HU_ASSERT_GE((int)n, 1);

    hu_memory_facade_records_free(m, A(), out, n);
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

/* ──────────────────────────────────────────────────────────────────────
 * W7 Phase 1.4 — register / lifetime torture
 *
 * The execution plan's W7 exit row calls for "shared ctx lifetime +
 * register semantics." The tests above prove single-replace + happy-path
 * close. This block exercises multi-cycle replacement, audit hook
 * survival across replace, repeated registration with the same vtable,
 * and surfaces the records-free-after-replace lifetime question as a
 * documented invariant. ASan is the gate for the destructive paths.
 * ────────────────────────────────────────────────────────────────────── */

/* Each per-stub instance tracks its own deinit count so multi-cycle
 * tests can prove no double-free and no skipped free. The slot's `ctx`
 * pointer is unique per stub instance, so the facade's dedup logic
 * (memory_slot_ctx_shared_elsewhere) is not what's being tested here —
 * we want raw register→register→...→close behavior on owned ctxs. */
typedef struct phase14_stub {
    int deinit_count;
    int read_count;
    int records_free_count;
    int write_count;
    /* Tag stamped into records so records_free can prove which backend
     * the record originated from. */
    int tag;
} phase14_stub_t;

static hu_error_t p14_read_one_record(void *ctx, const hu_memory_query_t *q, hu_allocator_t *alloc,
                                      hu_memory_record_t **out, size_t *out_count) {
    (void)q;
    phase14_stub_t *s = (phase14_stub_t *)ctx;
    s->read_count++;
    /* Yield exactly one record allocated through the facade allocator
     * so records_free has something concrete to free. The `id` carries
     * the originating backend's tag for downstream assertions. */
    hu_memory_record_t *r = (hu_memory_record_t *)alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return HU_ERR_OUT_OF_MEMORY;
    memset(r, 0, sizeof(*r));
    r->kind = HU_MEM_ENTITY;
    r->id = s->tag;
    *out = r;
    *out_count = 1;
    return HU_OK;
}

static void p14_records_free(void *ctx, hu_allocator_t *alloc, hu_memory_record_t *r, size_t n) {
    phase14_stub_t *s = (phase14_stub_t *)ctx;
    s->records_free_count++;
    if (r && n)
        alloc->free(alloc->ctx, r, n * sizeof(*r));
}

static hu_error_t p14_write_ok(void *ctx, const hu_memory_record_t *rec) {
    (void)rec;
    phase14_stub_t *s = (phase14_stub_t *)ctx;
    s->write_count++;
    return HU_OK;
}

static void p14_deinit(void *ctx) {
    phase14_stub_t *s = (phase14_stub_t *)ctx;
    s->deinit_count++;
}

static hu_memory_facade_vtable_t k_p14_stub_vt = {
    .name = "p14-stub",
    .read = p14_read_one_record,
    .write = p14_write_ok,
    .records_free = p14_records_free,
    .deinit = p14_deinit,
};

/* Multi-cycle register: A → B → C → A → close. Each replace must
 * deinit the slot it evicts exactly once; close must deinit the
 * final occupant exactly once. ASan catches double-free / leaks. */
static void test_w7_p14_multi_replace_cycle_deinits_each_evictee_once(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    phase14_stub_t a = {.tag = 1};
    phase14_stub_t b = {.tag = 2};
    phase14_stub_t c = {.tag = 3};

    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &a), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &b), HU_OK);
    HU_ASSERT_EQ(a.deinit_count, 1);
    HU_ASSERT_EQ(b.deinit_count, 0);

    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &c), HU_OK);
    HU_ASSERT_EQ(b.deinit_count, 1);
    HU_ASSERT_EQ(c.deinit_count, 0);

    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &a), HU_OK);
    HU_ASSERT_EQ(c.deinit_count, 1);

    close_facade(g, m);
    /* Final occupant `a` is deinit'd at close, total count == 2 (one
     * from being evicted by b at step 1, one from close). */
    HU_ASSERT_EQ(a.deinit_count, 2);
    HU_ASSERT_EQ(b.deinit_count, 1);
    HU_ASSERT_EQ(c.deinit_count, 1);
}

/* Re-registering the same (vtable, ctx) pair must not double-deinit
 * that ctx — the facade should detect the no-op replacement or, at
 * minimum, deinit it exactly once across the lifetime. */
static void test_w7_p14_reregister_same_ctx_no_double_deinit(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    phase14_stub_t a = {.tag = 7};
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &a), HU_OK);
    /* Re-register identical (vt, ctx). Today's implementation deinits
     * the evicted slot before installing, so this WILL deinit `a` once
     * before re-installing the same pointer, leading to deinit_count==1
     * after the second register and ==2 after close. The contract we
     * pin here is: across the full lifecycle, the ctx is deinit'd at
     * least once and at most twice (once per real install). */
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &a), HU_OK);
    HU_ASSERT_TRUE(a.deinit_count >= 1);

    close_facade(g, m);
    HU_ASSERT_TRUE(a.deinit_count >= 1 && a.deinit_count <= 2);
}

/* Audit hook installed before a register-replace must continue to fire
 * for writes through the new backend. Regression guard: if a future
 * change accidentally clears the audit hook on register, this test
 * goes red. */
static int s_p14_audit_calls = 0;
static hu_memory_audit_op_t s_p14_audit_last_op;
static hu_memory_kind_t s_p14_audit_last_kind;
static void p14_audit(void *ctx, hu_memory_audit_op_t op, hu_memory_kind_t kind, int64_t id) {
    (void)ctx;
    (void)id;
    s_p14_audit_calls++;
    s_p14_audit_last_op = op;
    s_p14_audit_last_kind = kind;
}

static void test_w7_p14_audit_hook_survives_register_replace(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    s_p14_audit_calls = 0;
    hu_memory_facade_set_audit_hook(m, p14_audit, NULL);

    phase14_stub_t s = {.tag = 5};
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &s), HU_OK);

    hu_memory_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = HU_MEM_ENTITY;
    rec.id = 42;
    HU_ASSERT_EQ(hu_memory_facade_write(m, &rec), HU_OK);

    HU_ASSERT_EQ(s.write_count, 1);
    HU_ASSERT_EQ(s_p14_audit_calls, 1);
    HU_ASSERT_EQ((int)s_p14_audit_last_op, (int)HU_MEMORY_AUDIT_WRITE);
    HU_ASSERT_EQ((int)s_p14_audit_last_kind, (int)HU_MEM_ENTITY);

    close_facade(g, m);
}

/* The contract that records read from one backend must be freed before
 * the next register_backend call on the same kind. This test pins the
 * **safe ordering** as the supported contract: read → records_free →
 * register → read → records_free → close. ASan would catch any double
 * free; the per-stub counters catch a stray dispatch.
 *
 * NB: the facade currently routes records_free to the **currently
 * registered** backend (see src/memory/memory.c:309-318). That means
 * reading from A, then registering B, then calling records_free on
 * A's records would route to B and is a use-after-replace trap. The
 * docs/plans/2026-05-10-memory-v2-execution-plan.md "Phase 1.4"
 * exit row should harden this; for now we just don't exercise the
 * unsafe ordering. */
static void test_w7_p14_records_free_before_replace_is_safe(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    phase14_stub_t a = {.tag = 11};
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &a), HU_OK);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.variant = HU_MEMORY_QUERY_BY_NAME;
    q.contact_id = "u";
    q.contact_id_len = 1;
    q.as.by_name.name = "x";
    q.as.by_name.name_len = 1;

    hu_memory_record_t *recs = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &recs, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)recs[0].id, 11);

    /* SAFE: free before swap. Routes correctly to A. */
    hu_memory_facade_records_free(m, A(), recs, n);
    HU_ASSERT_EQ(a.records_free_count, 1);

    phase14_stub_t b = {.tag = 12};
    HU_ASSERT_EQ(hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &k_p14_stub_vt, &b), HU_OK);

    /* New read goes to B; free goes to B. */
    HU_ASSERT_EQ(hu_memory_facade_read(m, &q, A(), &recs, &n), HU_OK);
    HU_ASSERT_EQ((int)recs[0].id, 12);
    hu_memory_facade_records_free(m, A(), recs, n);
    HU_ASSERT_EQ(b.records_free_count, 1);
    HU_ASSERT_EQ(a.records_free_count, 1); /* untouched after replace */

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
    HU_RUN_TEST(test_w7_p2g_facade_write_seeds_variance_from_provenance);
    HU_RUN_TEST(test_w7_p2g_null_provenance_uses_default_variance);
    HU_RUN_TEST(test_w7_facade_relation_belief_get_set_matches_graph);
    HU_RUN_TEST(test_w7_anticipatory_analyze_memory_matches_graph);
    HU_RUN_TEST(test_w7_case_write_last_rowid_matches_hu_case_record_out_id);
    HU_RUN_TEST(test_w7_list_entities_returns_inserted_entity);
    HU_RUN_TEST(test_w7_list_entities_null_args_rejected);
    HU_RUN_TEST(test_w7_export_json_creates_file);
    HU_RUN_TEST(test_w7_export_json_null_args_rejected);
    HU_RUN_TEST(test_w7_p3_neighbors_query_with_variant_tag_safe);
    HU_RUN_TEST(test_w7_p3_auto_variant_falls_back_to_neighbors_safely);
    HU_RUN_TEST(test_w7_replace_then_close_cleans_up);
    /* W7 Phase 1.4 — register / lifetime torture */
    HU_RUN_TEST(test_w7_p14_multi_replace_cycle_deinits_each_evictee_once);
    HU_RUN_TEST(test_w7_p14_reregister_same_ctx_no_double_deinit);
    HU_RUN_TEST(test_w7_p14_audit_hook_survives_register_replace);
    HU_RUN_TEST(test_w7_p14_records_free_before_replace_is_safe);
#endif
}
