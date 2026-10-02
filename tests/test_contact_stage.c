/* test_contact_stage.c — DEF-16: the relationship stage is per contact,
 * derived from that contact's interaction data, not an agent-wide counter. */
#include "human/persona/contact_stage.h"

#include "human/persona/relationship.h"
#include "test_framework.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/agent.h"
#include "human/agent/contact_stage_turn.h"
#include "human/agent/frontier_persist.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/contact_stage_repo.h"
#include <sqlite3.h>
#endif

static const hu_contact_stage_norms_t k_norms = {.median_msgs = 292.0, .median_days = 20.0};

static void test_prior_maps_dunbar_layers_to_band_midpoints(void) {
    HU_ASSERT_TRUE(hu_contact_stage_prior("intimate", NULL) >= 0.80f);
    HU_ASSERT_EQ((int)hu_relationship_stage_from_quality(hu_contact_stage_prior("close", NULL)),
                 (int)HU_REL_TRUSTED);
    HU_ASSERT_EQ((int)hu_relationship_stage_from_quality(hu_contact_stage_prior("Active", NULL)),
                 (int)HU_REL_FAMILIAR);
    HU_ASSERT_EQ((int)hu_relationship_stage_from_quality(hu_contact_stage_prior("150", NULL)),
                 (int)HU_REL_NEW);
    HU_ASSERT_EQ((int)hu_relationship_stage_from_quality(hu_contact_stage_prior("2", NULL)),
                 (int)HU_REL_TRUSTED);
    /* whole words only: "unclose" is not "close" */
    HU_ASSERT_TRUE(hu_contact_stage_prior("unclose", NULL) < 0.0f);
    HU_ASSERT_TRUE(hu_contact_stage_prior(NULL, NULL) < 0.0f);
    /* relationship_stage is the fallback when the layer gives nothing */
    HU_ASSERT_TRUE(hu_contact_stage_prior(NULL, "intimate") >= 0.80f);
    HU_ASSERT_TRUE(hu_contact_stage_prior("close", "intimate") < 0.80f);
}

/* Fixture numbers are the shape of prod's session store (counts only). */
static void test_derive_discriminates_by_interaction(void) {
    hu_contact_stage_signals_t heavy = {.inbound = 684, .outbound = 263, .active_days = 54};
    hu_contact_stage_signals_t mid = {.inbound = 152, .outbound = 64, .active_days = 13};
    hu_contact_stage_signals_t light = {.inbound = 29, .outbound = 10, .active_days = 11};
    hu_contact_stage_signals_t none = {0, 0, 0};
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&heavy, &k_norms, -1.0f, NULL), (int)HU_REL_TRUSTED);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&mid, &k_norms, -1.0f, NULL), (int)HU_REL_FAMILIAR);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&light, &k_norms, -1.0f, NULL), (int)HU_REL_NEW);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&none, &k_norms, -1.0f, NULL), (int)HU_REL_NEW);
}

static void test_derive_prior_dominates_until_evidence_accumulates(void) {
    float intimate = hu_contact_stage_prior("intimate", NULL);
    hu_contact_stage_signals_t none = {0, 0, 0};
    hu_contact_stage_signals_t heavy = {.inbound = 684, .outbound = 263, .active_days = 54};
    hu_contact_stage_signals_t one_sided = {.inbound = 900, .outbound = 0, .active_days = 2};
    /* declared family with no history yet: the prior alone */
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&none, &k_norms, intimate, NULL), (int)HU_REL_DEEP);
    /* declared family + heavy reciprocal history: still DEEP */
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&heavy, &k_norms, intimate, NULL), (int)HU_REL_DEEP);
    /* evidence can pull a declared layer down: many one-sided messages on
     * two days are not intimacy */
    float q = 0.0f;
    hu_relationship_stage_t st = hu_contact_stage_derive(&one_sided, &k_norms, intimate, &q);
    HU_ASSERT_TRUE(st < HU_REL_DEEP);
    HU_ASSERT_TRUE(q < intimate);
}

static void test_evidence_is_not_a_turn_count(void) {
    /* Same message count; reciprocity and regularity change the evidence. */
    hu_contact_stage_signals_t burst = {.inbound = 300, .outbound = 0, .active_days = 1};
    hu_contact_stage_signals_t steady = {.inbound = 150, .outbound = 150, .active_days = 40};
    HU_ASSERT_TRUE(hu_contact_stage_evidence(&steady, &k_norms) >
                   hu_contact_stage_evidence(&burst, &k_norms) + 0.2f);
}

static void test_relationship_update_does_not_raise_a_derived_stage(void) {
    hu_relationship_state_t legacy = {0};
    hu_relationship_update(&legacy, 250);
    HU_ASSERT_EQ((int)legacy.stage, (int)HU_REL_DEEP); /* CLI fallback unchanged */
    hu_relationship_state_t derived = {.stage = HU_REL_FAMILIAR, .derived = true};
    hu_relationship_update(&derived, 250);
    HU_ASSERT_EQ((int)derived.stage, (int)HU_REL_FAMILIAR);
    HU_ASSERT_EQ(derived.total_turns, 250u);
}

#ifdef HU_ENABLE_SQLITE

static void exec_ok(sqlite3 *db, const char *sql) {
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

/* n messages for `who` spread over `days` distinct days; every `out_every`th
 * message is outbound (0 = none). */
static void seed(sqlite3 *db, const char *who, int n, int days, int out_every) {
    char sql[256];
    for (int i = 0; i < n; i++) {
        bool out = out_every > 0 && (i % out_every) == 0;
        snprintf(sql, sizeof(sql),
                 "INSERT INTO messages(session_id, role, content, created_at) VALUES "
                 "('%s', '%s', 'x', datetime('2026-06-01', '+%d days'))",
                 who, out ? "assistant" : "user", i % (days > 0 ? days : 1));
        exec_ok(db, sql);
    }
}

typedef struct {
    int rows;
    uint32_t in_total, out_total, days_max;
} repo_acc_t;

static void acc_row(void *ctx, const char *c, size_t cl, uint32_t in, uint32_t out, uint32_t d) {
    (void)c;
    (void)cl;
    repo_acc_t *a = (repo_acc_t *)ctx;
    a->rows++;
    a->in_total += in;
    a->out_total += out;
    if (d > a->days_max)
        a->days_max = d;
}

static void test_repo_counts_per_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed(db, "+15550000001", 40, 10, 4);
    seed(db, "+15550000002", 6, 2, 0);
    repo_acc_t acc = {0};
    HU_ASSERT_EQ(hu_contact_stage_repo_each_contact(db, acc_row, &acc), HU_OK);
    HU_ASSERT_EQ(acc.rows, 2);
    HU_ASSERT_EQ(acc.out_total, 10u);
    HU_ASSERT_EQ(acc.in_total, 36u);
    HU_ASSERT_EQ(acc.days_max, 10u);
    uint32_t counts[4] = {9, 9, 9, 9};
    HU_ASSERT_EQ(hu_contact_stage_repo_persisted_counts(db, counts), HU_OK);
    HU_ASSERT_EQ(counts[0] + counts[1] + counts[2] + counts[3], 0u); /* no table yet */
    mem.vtable->deinit(mem.ctx);
}

/* Headline: two contacts on ONE agent keep their own stages. The old
 * agent-wide state gave the second contact whatever the first had, and
 * every turn on any contact raised it. */
static void test_refresh_keeps_stages_per_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    /* five light contacts make the norms data-derived (median of 6) */
    seed(db, "+15550000010", 20, 4, 3);
    seed(db, "+15550000011", 25, 5, 3);
    seed(db, "+15550000012", 30, 6, 3);
    seed(db, "+15550000013", 20, 4, 3);
    seed(db, "+15550000014", 5, 1, 3);
    seed(db, "+15550000099", 400, 60, 2); /* the heavy, reciprocal contact */
    /* both rows persisted by the old agent-wide counter at the same stage */
    HU_ASSERT_EQ(
        hu_frontier_persist_save_relationship(db, "+15550000099", 12, HU_REL_TRUSTED, 9, 120),
        HU_OK);
    HU_ASSERT_EQ(
        hu_frontier_persist_save_relationship(db, "+15550000014", 12, HU_REL_TRUSTED, 9, 120),
        HU_OK);

    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);
    agent->alloc = &alloc;
    agent->memory = &mem;
    HU_ASSERT_EQ((int)agent->relationship.stage, (int)HU_REL_NEW);

    hu_relationship_stage_t heavy = hu_contact_stage_refresh(agent, "+15550000099", 12);
    HU_ASSERT_TRUE(heavy >= HU_REL_TRUSTED);
    HU_ASSERT_TRUE(agent->relationship.derived);
    HU_ASSERT_EQ(agent->relationship.total_turns, 400u);
    for (int t = 0; t < 250; t++) /* 250 turns with the heavy contact */
        hu_relationship_update(&agent->relationship, 1);

    hu_relationship_stage_t light = hu_contact_stage_refresh(agent, "+15550000014", 12);
    HU_ASSERT_EQ((int)light, (int)HU_REL_NEW);
    HU_ASSERT_EQ((int)agent->relationship.stage, (int)HU_REL_NEW);
    for (int t = 0; t < 250; t++) /* turns never escalate a derived stage */
        hu_relationship_update(&agent->relationship, 1);
    HU_ASSERT_EQ((int)agent->relationship.stage, (int)HU_REL_NEW);

    /* back to the heavy contact: its own stage; both persisted rows migrated */
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(agent, "+15550000099", 12), (int)heavy);
    uint32_t counts[4] = {0};
    HU_ASSERT_EQ(hu_contact_stage_repo_persisted_counts(db, counts), HU_OK);
    HU_ASSERT_EQ(counts[HU_REL_NEW], 1u);
    HU_ASSERT_EQ(counts[(int)heavy], 1u);

    /* an unknown contact starts NEW, not at the previous contact's stage */
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(agent, "+15550000777", 12), (int)HU_REL_NEW);
    free(agent);
    mem.vtable->deinit(mem.ctx);
}

static void test_refresh_without_memory_changes_nothing(void) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);
    agent->relationship.stage = HU_REL_TRUSTED;
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(agent, "+15550000099", 12), (int)HU_REL_TRUSTED);
    HU_ASSERT_FALSE(agent->relationship.derived);
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(NULL, "x", 1), (int)HU_REL_NEW);
    free(agent);
}

#else
static void test_repo_counts_per_contact(void) {
    (void)0;
}
static void test_refresh_keeps_stages_per_contact(void) {
    (void)0;
}
static void test_refresh_without_memory_changes_nothing(void) {
    (void)0;
}
#endif

void run_contact_stage_tests(void) {
    HU_TEST_SUITE("contact_stage");
    HU_RUN_TEST(test_prior_maps_dunbar_layers_to_band_midpoints);
    HU_RUN_TEST(test_derive_discriminates_by_interaction);
    HU_RUN_TEST(test_derive_prior_dominates_until_evidence_accumulates);
    HU_RUN_TEST(test_evidence_is_not_a_turn_count);
    HU_RUN_TEST(test_relationship_update_does_not_raise_a_derived_stage);
    HU_RUN_TEST(test_repo_counts_per_contact);
    HU_RUN_TEST(test_refresh_keeps_stages_per_contact);
    HU_RUN_TEST(test_refresh_without_memory_changes_nothing);
}
