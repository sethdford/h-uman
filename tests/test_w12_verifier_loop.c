/* W12 P0 #2 — planner verifier loop end-to-end.
 *
 * Proves that hu_planner_execute, when given a non-NULL self_rag and
 * a step with verify_after=true, drops sub-threshold records and
 * aborts the rest of the plan when the abstain ratio crosses 0.5.
 * Lives in its own file so concurrent edits to test_w12_planner.c
 * don't clobber these proofs. */

#include "human/agent/retrieval_planner.h"
#include "human/agent/self_rag.h"
#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A_(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static int64_t add_entity_(hu_graph_t *g, const char *cid, const char *name,
                           hu_entity_type_t t) {
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), name, strlen(name), t,
                                         NULL, &id),
                 HU_OK);
    return id;
}

/* Filter: 50% drop ratio (one of two records below 0.3). The high
 * record survives. The low record is excluded from out[]. */
static void test_w12_verifier_loop_drops_unsupported_facts(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_graph_open(A_(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A_(), g, &m), HU_OK);
    hu_world_model_invalidate(NULL, 0);

    int64_t alice = add_entity_(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t acme  = add_entity_(g, "u1", "Acme",  HU_ENTITY_ORGANIZATION);
    int64_t init  = add_entity_(g, "u1", "Initech", HU_ENTITY_ORGANIZATION);

    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT,
                                              1.0f, 0, 0, 0.95f, NULL, 0, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, init, HU_REL_LIVES_IN,
                                              1.0f, 0, 0, 0.10f, NULL, 0, NULL, 0),
                 HU_OK);

    hu_retrieval_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.steps_count = 1;
    plan.total_budget_ms = 0;
    plan.steps[0].kind = HU_MEM_RELATION;
    plan.steps[0].query.kind = HU_MEM_RELATION;
    plan.steps[0].query.contact_id = "u1";
    plan.steps[0].query.contact_id_len = 2;
    plan.steps[0].verify_after = true;
    plan.steps[0].budget_ms = 50;

    hu_self_rag_t self_rag;
    memset(&self_rag, 0, sizeof(self_rag));
    HU_ASSERT_EQ(hu_self_rag_heuristic(m, &self_rag), HU_OK);

    /* Baseline: NULL verifier returns both records. */
    hu_memory_record_t *out_b = NULL; size_t n_b = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A_(), &out_b, &n_b), HU_OK);
    HU_ASSERT_EQ((int)n_b, 2);
    bool saw_high = false, saw_low = false;
    for (size_t i = 0; i < n_b; i++) {
        if (out_b[i].confidence > 0.9f) saw_high = true;
        if (out_b[i].confidence > 0.0f && out_b[i].confidence < 0.2f) saw_low = true;
    }
    HU_ASSERT(saw_high);
    HU_ASSERT(saw_low);
    hu_planner_records_free(A_(), out_b, n_b);

    /* With verifier: only the 0.95 record survives. */
    hu_memory_record_t *out = NULL; size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, &self_rag, &plan, A_(), &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT(out[0].confidence > 0.3f);
    hu_planner_records_free(A_(), out, n);

    hu_self_rag_close(&self_rag);
    hu_world_model_invalidate(NULL, 0);
    hu_memory_facade_close(m, A_());
    hu_graph_close(g, A_());
}

/* Abort: 75% drop ratio in step 0 → step 1 must NOT execute. We use
 * two contacts so dedupe doesn't mask the difference. */
static void test_w12_verifier_loop_aborts_when_step_abstains(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_graph_open(A_(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A_(), g, &m), HU_OK);
    hu_world_model_invalidate(NULL, 0);

    int64_t alice = add_entity_(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t e1 = add_entity_(g, "u1", "Org1", HU_ENTITY_ORGANIZATION);
    int64_t e2 = add_entity_(g, "u1", "Org2", HU_ENTITY_ORGANIZATION);
    int64_t e3 = add_entity_(g, "u1", "Org3", HU_ENTITY_ORGANIZATION);
    int64_t e4 = add_entity_(g, "u1", "Org4", HU_ENTITY_ORGANIZATION);

    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, e1, HU_REL_WORKS_AT,
                                              1.0f, 0, 0, 0.05f, NULL, 0, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, e2, HU_REL_LIVES_IN,
                                              1.0f, 0, 0, 0.10f, NULL, 0, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, e3, HU_REL_KNOWS,
                                              1.0f, 0, 0, 0.15f, NULL, 0, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, e4, HU_REL_INTERESTED_IN,
                                              1.0f, 0, 0, 0.95f, NULL, 0, NULL, 0),
                 HU_OK);

    /* Independent contact u2 with one solid relation. Step 1 reads
     * from u2; if the abort doesn't trigger we'll see this record in
     * the output. */
    int64_t bob = add_entity_(g, "u2", "Bob",  HU_ENTITY_PERSON);
    int64_t cob = add_entity_(g, "u2", "Coby", HU_ENTITY_PERSON);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u2", 2, bob, cob, HU_REL_KNOWS,
                                              1.0f, 0, 0, 0.95f, NULL, 0, NULL, 0),
                 HU_OK);

    hu_retrieval_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.steps_count = 2;
    plan.total_budget_ms = 0;
    plan.steps[0].kind = HU_MEM_RELATION;
    plan.steps[0].query.kind = HU_MEM_RELATION;
    plan.steps[0].query.contact_id = "u1";
    plan.steps[0].query.contact_id_len = 2;
    plan.steps[0].verify_after = true;
    plan.steps[0].budget_ms = 50;
    plan.steps[1].kind = HU_MEM_RELATION;
    plan.steps[1].query.kind = HU_MEM_RELATION;
    plan.steps[1].query.contact_id = "u2";
    plan.steps[1].query.contact_id_len = 2;
    plan.steps[1].verify_after = false;
    plan.steps[1].budget_ms = 50;

    hu_self_rag_t self_rag;
    memset(&self_rag, 0, sizeof(self_rag));
    HU_ASSERT_EQ(hu_self_rag_heuristic(m, &self_rag), HU_OK);

    /* Baseline: NULL verifier — both steps run. */
    hu_memory_record_t *out_b = NULL; size_t n_b = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A_(), &out_b, &n_b), HU_OK);
    HU_ASSERT_EQ((int)n_b, 5);
    hu_planner_records_free(A_(), out_b, n_b);

    /* With verifier: step 0 abstains (75% > 50%), step 1 does NOT
     * run, only the 1 surviving u1 relation is returned. */
    hu_memory_record_t *out = NULL; size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, &self_rag, &plan, A_(), &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    hu_planner_records_free(A_(), out, n);

    hu_self_rag_close(&self_rag);
    hu_world_model_invalidate(NULL, 0);
    hu_memory_facade_close(m, A_());
    hu_graph_close(g, A_());
}

#endif /* HU_ENABLE_SQLITE */

void run_w12_verifier_loop_tests(void) {
    HU_TEST_SUITE("W12 verifier loop (P0 #2)");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w12_verifier_loop_drops_unsupported_facts);
    HU_RUN_TEST(test_w12_verifier_loop_aborts_when_step_abstains);
#endif
}
