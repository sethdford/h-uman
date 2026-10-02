/* test_contact_stage.c — DEF-16: the relationship stage is per contact,
 * derived from that contact's interaction data, not an agent-wide counter.
 * Gated by HU_REL_STAGE_DERIVED (off | shadow | live). */
#include "human/persona/contact_stage.h"

#include "human/persona/relationship.h"
#include "test_framework.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE
#include "human/agent.h"
#include "human/agent/contact_stage_turn.h"
#include "human/agent/frontier_persist.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/contact_stage_repo.h"
#include <sqlite3.h>
#endif

static const hu_contact_stage_norms_t k_norms = {
    .median_inbound = 186.0, .median_days = 20.0, .median_reply_ratio = 0.0};

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
    HU_ASSERT_TRUE(hu_contact_stage_prior("unclose", NULL) < 0.0f); /* whole words only */
    HU_ASSERT_TRUE(hu_contact_stage_prior(NULL, NULL) < 0.0f);
    HU_ASSERT_TRUE(hu_contact_stage_prior(NULL, "intimate") >= 0.80f);
    HU_ASSERT_TRUE(hu_contact_stage_prior("close", "intimate") < 0.80f);
}

/* Fixture numbers are the shape of prod's session store (counts only). */
static void test_derive_discriminates_by_interaction(void) {
    hu_contact_stage_signals_t heavy = {.inbound = 684, .active_days = 54, .seth_replies = -1};
    hu_contact_stage_signals_t mid = {.inbound = 152, .active_days = 13, .seth_replies = -1};
    hu_contact_stage_signals_t light = {.inbound = 12, .active_days = 3, .seth_replies = -1};
    hu_contact_stage_signals_t none = {0, 0, -1};
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&heavy, &k_norms, -1.0f, NULL), (int)HU_REL_TRUSTED);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&mid, &k_norms, -1.0f, NULL), (int)HU_REL_FAMILIAR);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&light, &k_norms, -1.0f, NULL), (int)HU_REL_NEW);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&none, &k_norms, -1.0f, NULL), (int)HU_REL_NEW);
}

static void test_derive_prior_dominates_until_evidence_accumulates(void) {
    float intimate = hu_contact_stage_prior("intimate", NULL);
    hu_contact_stage_signals_t none = {0, 0, -1};
    hu_contact_stage_signals_t heavy = {.inbound = 684, .active_days = 54, .seth_replies = 600};
    hu_contact_stage_signals_t ignored = {.inbound = 900, .active_days = 2, .seth_replies = 0};
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&none, &k_norms, intimate, NULL), (int)HU_REL_DEEP);
    HU_ASSERT_EQ((int)hu_contact_stage_derive(&heavy, &k_norms, intimate, NULL), (int)HU_REL_DEEP);
    /* evidence can pull a declared layer down: many messages Seth never
     * answers, on two days, are not intimacy */
    float q = 0.0f;
    hu_relationship_stage_t st = hu_contact_stage_derive(&ignored, &k_norms, intimate, &q);
    HU_ASSERT_TRUE(st < HU_REL_DEEP);
    HU_ASSERT_TRUE(q < intimate);
}

/* Reciprocity is Seth's own replies against the owner's median reply ratio;
 * unknown is neutral. */
static void test_reciprocity_uses_seth_replies(void) {
    hu_contact_stage_norms_t n = {
        .median_inbound = 100, .median_days = 10, .median_reply_ratio = 0.5};
    hu_contact_stage_signals_t answered = {.inbound = 100, .active_days = 10, .seth_replies = 60};
    hu_contact_stage_signals_t unknown = {.inbound = 100, .active_days = 10, .seth_replies = -1};
    hu_contact_stage_signals_t ignored = {.inbound = 100, .active_days = 10, .seth_replies = 0};
    float a = hu_contact_stage_evidence(&answered, &n);
    float u = hu_contact_stage_evidence(&unknown, &n);
    float i = hu_contact_stage_evidence(&ignored, &n);
    HU_ASSERT_TRUE(a > u && u > i);
    HU_ASSERT_TRUE(fabsf(a - 0.5f) < 1e-4f); /* (0.5 + 0.5) / 2 * (0.75 + 0.25) */
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

/* n messages FROM `who` over `days` distinct days, plus `twin` assistant
 * rows (the twin's replies, which must never count). */
static void seed(sqlite3 *db, const char *who, int n, int days, int twin) {
    char sql[256];
    for (int i = 0; i < n + twin; i++) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO messages(session_id, role, content, created_at) VALUES "
                 "('%s', '%s', 'x', datetime('2026-06-01', '+%d days'))",
                 who, i < n ? "user" : "assistant", i % (days > 0 ? days : 1));
        exec_ok(db, sql);
    }
}

typedef struct {
    hu_allocator_t alloc;
    hu_memory_t mem;
    sqlite3 *db;
    hu_agent_t *agent;
} cs_fx_t;

static void fx_open(cs_fx_t *fx) {
    fx->alloc = hu_system_allocator();
    fx->mem = hu_sqlite_memory_create(&fx->alloc, ":memory:");
    HU_ASSERT_NOT_NULL(fx->mem.vtable);
    fx->db = hu_sqlite_memory_get_db(&fx->mem);
    /* five light contacts make the norms data-derived (median of 6) */
    seed(fx->db, "+15550000010", 20, 4, 3);
    seed(fx->db, "+15550000011", 25, 5, 3);
    seed(fx->db, "+15550000012", 30, 6, 3);
    seed(fx->db, "+15550000013", 20, 4, 3);
    seed(fx->db, "+15550000014", 5, 1, 300);    /* few messages, many twin replies */
    seed(fx->db, "+15550000099", 400, 60, 200); /* the heavy contact */
    fx->agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(fx->agent);
    fx->agent->alloc = &fx->alloc;
    fx->agent->memory = &fx->mem;
    hu_contact_stage_cache_reset();
}

static void fx_close(cs_fx_t *fx) {
    free(fx->agent);
    fx->mem.vtable->deinit(fx->mem.ctx);
    unsetenv("HU_REL_STAGE_DERIVED");
    hu_contact_stage_cache_reset();
}

static uint32_t derived_rows(sqlite3 *db) {
    uint32_t c[4] = {0};
    HU_ASSERT_EQ(hu_contact_stage_repo_derived_counts(db, c), HU_OK);
    return c[0] + c[1] + c[2] + c[3];
}

static void test_repo_counts_only_the_contacts_own_messages(void) {
    cs_fx_t fx;
    fx_open(&fx);
    uint32_t in = 0, days = 0;
    HU_ASSERT_EQ(hu_contact_stage_repo_contact_counts(fx.db, "+15550000014", 12, &in, &days),
                 HU_OK);
    HU_ASSERT_EQ(in, 5u); /* 300 twin replies ignored */
    HU_ASSERT_EQ(days, 1u);
    HU_ASSERT_EQ(hu_contact_stage_repo_contact_counts(fx.db, "+15550000777", 12, &in, &days),
                 HU_OK);
    HU_ASSERT_EQ(in, 0u);
    fx_close(&fx);
}

static void test_gate_off_changes_and_writes_nothing(void) {
    cs_fx_t fx;
    fx_open(&fx);
    unsetenv("HU_REL_STAGE_DERIVED");
    fx.agent->relationship.stage = HU_REL_TRUSTED;
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(fx.agent, "+15550000014", 12), (int)HU_REL_TRUSTED);
    HU_ASSERT_FALSE(fx.agent->relationship.derived);
    HU_ASSERT_EQ(derived_rows(fx.db), 0u);
    fx_close(&fx);
}

/* SHADOW derives and logs; the agent and the database are untouched. */
static void test_shadow_writes_nothing(void) {
    cs_fx_t fx;
    fx_open(&fx);
    HU_ASSERT_EQ(
        hu_frontier_persist_save_relationship(fx.db, "+15550000014", 12, HU_REL_TRUSTED, 9, 120),
        HU_OK);
    setenv("HU_REL_STAGE_DERIVED", "shadow", 1);
    fx.agent->relationship.stage = HU_REL_TRUSTED;
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(fx.agent, "+15550000014", 12), (int)HU_REL_TRUSTED);
    HU_ASSERT_FALSE(fx.agent->relationship.derived);
    HU_ASSERT_EQ(derived_rows(fx.db), 0u);
    int rs = 0, rsc = 0, rt = 0;
    HU_ASSERT_EQ(hu_frontier_persist_load_relationship(fx.db, "+15550000014", 12, &rs, &rsc, &rt),
                 HU_OK);
    HU_ASSERT_EQ(rs, (int)HU_REL_TRUSTED);
    HU_ASSERT_EQ(rt, 120);
    fx_close(&fx);
}

/* Headline: two contacts on ONE agent keep their own stages. The old
 * agent-wide state gave the second contact whatever the first had, and every
 * turn on any contact raised it. LIVE never touches the old counters. */
static void test_live_keeps_stages_per_contact(void) {
    cs_fx_t fx;
    fx_open(&fx);
    setenv("HU_REL_STAGE_DERIVED", "live", 1);
    fx.agent->relationship.session_count = 7;
    fx.agent->relationship.total_turns = 33;
    hu_relationship_stage_t heavy = hu_contact_stage_refresh(fx.agent, "+15550000099", 12);
    HU_ASSERT_TRUE(heavy >= HU_REL_TRUSTED);
    HU_ASSERT_TRUE(fx.agent->relationship.derived);
    HU_ASSERT_EQ(fx.agent->relationship.session_count, 7u); /* not redefined */
    HU_ASSERT_EQ(fx.agent->relationship.total_turns, 33u);  /* not overwritten */
    for (int t = 0; t < 250; t++)
        hu_relationship_update(&fx.agent->relationship, 1);

    HU_ASSERT_EQ((int)hu_contact_stage_refresh(fx.agent, "+15550000014", 12), (int)HU_REL_NEW);
    for (int t = 0; t < 250; t++) /* turns never escalate a derived stage */
        hu_relationship_update(&fx.agent->relationship, 1);
    HU_ASSERT_EQ((int)fx.agent->relationship.stage, (int)HU_REL_NEW);

    HU_ASSERT_EQ((int)hu_contact_stage_refresh(fx.agent, "+15550000099", 12), (int)heavy);
    /* an unknown contact starts NEW, not at the previous contact's stage */
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(fx.agent, "+15550000777", 12), (int)HU_REL_NEW);
    /* the derived rows live in their own table */
    int st = -1;
    HU_ASSERT_EQ(hu_contact_stage_repo_get_derived(fx.db, "+15550000099", 12, &st, NULL, NULL),
                 HU_OK);
    HU_ASSERT_EQ(st, (int)heavy);
    HU_ASSERT_EQ(derived_rows(fx.db), 3u);
    fx_close(&fx);
}

/* A failed derivation must not leave the previous contact's stage. */
static void test_live_failure_resets_to_prior(void) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);
    setenv("HU_REL_STAGE_DERIVED", "live", 1);
    agent->relationship.stage = HU_REL_DEEP; /* the previous contact's */
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(agent, "+15550000099", 12), (int)HU_REL_NEW);
    HU_ASSERT_TRUE(agent->relationship.derived);
    setenv("HU_REL_STAGE_DERIVED", "shadow", 1); /* shadow: still untouched */
    agent->relationship.stage = HU_REL_DEEP;
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(agent, "+15550000099", 12), (int)HU_REL_DEEP);
    unsetenv("HU_REL_STAGE_DERIVED");
    HU_ASSERT_EQ((int)hu_contact_stage_refresh(NULL, "x", 1), (int)HU_REL_NEW);
    free(agent);
}

/* Norms are cached for the TTL: twenty heavy contacts arriving later move the
 * median only after it expires. */
static void test_norms_cache_ttl(void) {
    cs_fx_t fx;
    fx_open(&fx);
    setenv("HU_REL_STAGE_DERIVED", "live", 1);
    seed(fx.db, "+15550000050", 60, 12, 0); /* the probe */
    const int64_t t0 = 1790000000;
    hu_relationship_stage_t before = hu_contact_stage_refresh_at(fx.agent, "+15550000050", 12, t0);
    HU_ASSERT_TRUE(before >= HU_REL_FAMILIAR);
    for (int c = 0; c < 20; c++) {
        char who[16];
        snprintf(who, sizeof(who), "+1555000%04d", 2000 + c);
        seed(fx.db, who, 900, 80, 0);
    }
    HU_ASSERT_EQ((int)hu_contact_stage_refresh_at(fx.agent, "+15550000050", 12, t0 + 60),
                 (int)before); /* cached norms */
    hu_relationship_stage_t after = hu_contact_stage_refresh_at(
        fx.agent, "+15550000050", 12, t0 + HU_CONTACT_STAGE_NORMS_TTL_S + 1);
    HU_ASSERT_TRUE(after < before); /* recomputed: the median contact is now far heavier */
    fx_close(&fx);
}

/* Seth's own replies come from the learned-style profile, not from the
 * session store's assistant rows (the twin). */
static void test_live_reads_seth_replies_from_learned_style(void) {
    char dir[] = "/tmp/hu_cs_persona_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/testp.learned-style.json", dir);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"schema\":\"learned-style/v1\",\"contacts\":{"
          "\"+15550000010\":{\"overall\":{\"n\":10}},"
          "\"+15550000011\":{\"overall\":{\"n\":12}},"
          "\"+15550000012\":{\"overall\":{\"n\":15}},"
          "\"+15550000099\":{\"overall\":{\"n\":300}}}}",
          f);
    fclose(f);
    setenv("HU_PERSONA_DIR", dir, 1);
    cs_fx_t fx;
    fx_open(&fx);
    fx.agent->persona_name = (char *)"testp";
    fx.agent->persona_name_len = 5;
    setenv("HU_REL_STAGE_DERIVED", "live", 1);
    (void)hu_contact_stage_refresh(fx.agent, "+15550000099", 12);
    (void)hu_contact_stage_refresh(fx.agent, "+15550000014", 12);
    int64_t replies = -2;
    double q_known = 0.0;
    HU_ASSERT_EQ(
        hu_contact_stage_repo_get_derived(fx.db, "+15550000099", 12, NULL, &q_known, &replies),
        HU_OK);
    HU_ASSERT_EQ(replies, (int64_t)300);
    HU_ASSERT_EQ(hu_contact_stage_repo_get_derived(fx.db, "+15550000014", 12, NULL, NULL, &replies),
                 HU_OK);
    HU_ASSERT_EQ(replies, (int64_t)-1); /* not in the profile: unknown, not the twin's 300 */
    fx_close(&fx);
    unsetenv("HU_PERSONA_DIR");
    unlink(path);
    rmdir(dir);
}

#else
static void test_repo_counts_only_the_contacts_own_messages(void) {
    (void)0;
}
static void test_gate_off_changes_and_writes_nothing(void) {
    (void)0;
}
static void test_shadow_writes_nothing(void) {
    (void)0;
}
static void test_live_keeps_stages_per_contact(void) {
    (void)0;
}
static void test_live_failure_resets_to_prior(void) {
    (void)0;
}
static void test_norms_cache_ttl(void) {
    (void)0;
}
static void test_live_reads_seth_replies_from_learned_style(void) {
    (void)0;
}
#endif

void run_contact_stage_tests(void) {
    HU_TEST_SUITE("contact_stage");
    HU_RUN_TEST(test_prior_maps_dunbar_layers_to_band_midpoints);
    HU_RUN_TEST(test_derive_discriminates_by_interaction);
    HU_RUN_TEST(test_derive_prior_dominates_until_evidence_accumulates);
    HU_RUN_TEST(test_reciprocity_uses_seth_replies);
    HU_RUN_TEST(test_relationship_update_does_not_raise_a_derived_stage);
    HU_RUN_TEST(test_repo_counts_only_the_contacts_own_messages);
    HU_RUN_TEST(test_gate_off_changes_and_writes_nothing);
    HU_RUN_TEST(test_shadow_writes_nothing);
    HU_RUN_TEST(test_live_keeps_stages_per_contact);
    HU_RUN_TEST(test_live_failure_resets_to_prior);
    HU_RUN_TEST(test_norms_cache_ttl);
    HU_RUN_TEST(test_live_reads_seth_replies_from_learned_style);
}
