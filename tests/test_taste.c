/* ─────────────────────────────────────────────────────────────────────────
 * test_taste.c
 *
 * Pins independent taste (A2): the pure expression/stability/drift predicates,
 * the SQLite store, the INDEPENDENCE-from-Seth invariant (AC-2), and the
 * honesty contract on the rendered directive (AC-6).
 *
 * Spec: docs/plans/2026-05-29-independent-taste/
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/persona/taste.h"
#include "test_framework.h"

#include <string.h>

/* ── Expression predicate (AC-3) ────────────────────────────────────────── */

static hu_taste_express_facts_t base_express(void) {
    hu_taste_express_facts_t f;
    f.topic_relevant = true;
    f.already_expressed_recently = false;
    f.strength = 0.7;
    f.turns_since_last_taste = 5;
    return f;
}

static void taste_express_relevant_strong_surfaces(void) {
    hu_taste_express_facts_t f = base_express();
    HU_ASSERT_EQ((int)hu_taste_express_decide(&f), (int)HU_TASTE_EXPRESS);
}

static void taste_express_irrelevant_holds(void) {
    hu_taste_express_facts_t f = base_express();
    f.topic_relevant = false;
    HU_ASSERT_EQ((int)hu_taste_express_decide(&f), (int)HU_TASTE_HOLD);
}

static void taste_express_weak_holds(void) {
    hu_taste_express_facts_t f = base_express();
    f.strength = 0.2; /* below min */
    HU_ASSERT_EQ((int)hu_taste_express_decide(&f), (int)HU_TASTE_HOLD);
}

static void taste_express_anti_harp_holds(void) {
    hu_taste_express_facts_t f = base_express();
    f.already_expressed_recently = true;
    HU_ASSERT_EQ((int)hu_taste_express_decide(&f), (int)HU_TASTE_HOLD);
}

static void taste_express_null_holds(void) {
    HU_ASSERT_EQ((int)hu_taste_express_decide(NULL), (int)HU_TASTE_HOLD);
}

/* ── Stability (AC-4): disagreement does not revise taste ───────────────── */

static void taste_disagreement_does_not_revise(void) {
    HU_ASSERT_FALSE(hu_taste_should_revise(true, false)); /* user disagrees only */
    HU_ASSERT_FALSE(hu_taste_should_revise(true, false));
}

static void taste_own_experience_revises(void) {
    HU_ASSERT_TRUE(hu_taste_should_revise(false, true)); /* accumulated experience */
    HU_ASSERT_TRUE(hu_taste_should_revise(true, true));  /* experience wins over disagreement */
}

/* ── Drift (AC-5): bounded, coherent ────────────────────────────────────── */

static void taste_drift_bounded_per_step(void) {
    double up = hu_taste_drift_step(0.5, true);
    HU_ASSERT_TRUE(up > 0.5);
    HU_ASSERT_TRUE(up - 0.5 <= HU_TASTE_DRIFT_MAX_STEP + 1e-9);
    double down = hu_taste_drift_step(0.5, false);
    HU_ASSERT_TRUE(down < 0.5);
    HU_ASSERT_TRUE(0.5 - down <= HU_TASTE_DRIFT_MAX_STEP + 1e-9);
}

static void taste_drift_clamps(void) {
    HU_ASSERT_FLOAT_EQ(hu_taste_drift_step(1.0, true), 1.0, 1e-9);
    HU_ASSERT_FLOAT_EQ(hu_taste_drift_step(0.0, false), 0.0, 1e-9);
}

/* Many steps in one direction converge but never overshoot the bound. */
static void taste_drift_no_whiplash(void) {
    double s = 0.5;
    for (int i = 0; i < 50; i++)
        s = hu_taste_drift_step(s, true);
    HU_ASSERT_TRUE(s <= 1.0 + 1e-9);
    HU_ASSERT_TRUE(s >= 0.9); /* converged upward */
}

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/evolved_opinions.h"

/* ── Store round-trip (AC-1) ────────────────────────────────────────────── */

static void taste_store_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_taste_ensure_table(db), HU_OK);

    hu_taste_pref_t p = {0};
    char d[] = "music", s[] = "ambient music";
    p.domain = d;
    p.domain_len = strlen(d);
    p.subject = s;
    p.subject_len = strlen(s);
    p.valence = HU_TASTE_LIKE;
    p.strength = 0.8;
    p.reinforced = 3;
    HU_ASSERT_EQ(hu_taste_upsert(db, &p, 100), HU_OK);

    hu_taste_pref_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_taste_get(&alloc, db, 0.0, 16, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(out[0].subject, "ambient music");
    HU_ASSERT_EQ((int)out[0].valence, (int)HU_TASTE_LIKE);
    hu_taste_free(&alloc, out, n);
    mem.vtable->deinit(mem.ctx);
}

/* ── AC-2 isolation: the Seth-facing opinion path does NOT write taste ──── */

static void taste_isolated_from_seth_opinion_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_taste_ensure_table(db), HU_OK);

    /* Exercise the Seth-facing evolved-opinions store (a proxy for the mirror
     * path). It must NOT bleed into the agent's own taste store. */
    hu_evolved_opinions_ensure_table(db);
    hu_evolved_opinion_upsert(db, "remote work", 11, "is great", 8, 0.7, 100);

    hu_taste_pref_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_taste_get(&alloc, db, 0.0, 16, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 0u); /* taste store untouched by the Seth path */
    hu_taste_free(&alloc, out, n);
    mem.vtable->deinit(mem.ctx);
}

/* Starter seed is independent + present. */
static void taste_starter_seed_loads(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_taste_ensure_table(db), HU_OK);

    size_t seeded = 0;
    HU_ASSERT_EQ(hu_taste_seed_starter(db, 100, &seeded), HU_OK);
    HU_ASSERT_TRUE(seeded >= 6);

    hu_taste_pref_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_taste_get(&alloc, db, 0.0, 64, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, seeded);
    hu_taste_free(&alloc, out, n);
    mem.vtable->deinit(mem.ctx);
}

/* ── AC-6 honesty: directive leaks taste without claiming sentience ─────── */

static void taste_directive_honest_and_relevant(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_taste_ensure_table(db), HU_OK);

    hu_taste_pref_t p = {0};
    char d[] = "music", s[] = "ambient music";
    p.domain = d;
    p.domain_len = strlen(d);
    p.subject = s;
    p.subject_len = strlen(s);
    p.valence = HU_TASTE_LIKE;
    p.strength = 0.8;
    HU_ASSERT_EQ(hu_taste_upsert(db, &p, 100), HU_OK);

    /* Relevant message -> directive present, honest. */
    const char *msg = "what kind of ambient music do you recommend?";
    size_t dl = 0;
    char *dir = hu_taste_turn_directive(&alloc, db, msg, strlen(msg), &dl);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(dl > 0);
    /* Honesty contract: no sentience/affect claims. */
    HU_ASSERT_STR_NOT_CONTAINS(dir, "I feel");
    HU_ASSERT_STR_NOT_CONTAINS(dir, "I'm conscious");
    HU_ASSERT_STR_NOT_CONTAINS(dir, "sentient");
    alloc.free(alloc.ctx, dir, dl + 1);

    /* Irrelevant message -> no directive. */
    const char *msg2 = "what's the weather tomorrow?";
    size_t dl2 = 0;
    char *dir2 = hu_taste_turn_directive(&alloc, db, msg2, strlen(msg2), &dl2);
    HU_ASSERT_NULL(dir2);
    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

void run_taste_tests(void);
void run_taste_tests(void) {
    HU_TEST_SUITE("taste");
    HU_RUN_TEST(taste_express_relevant_strong_surfaces);
    HU_RUN_TEST(taste_express_irrelevant_holds);
    HU_RUN_TEST(taste_express_weak_holds);
    HU_RUN_TEST(taste_express_anti_harp_holds);
    HU_RUN_TEST(taste_express_null_holds);
    HU_RUN_TEST(taste_disagreement_does_not_revise);
    HU_RUN_TEST(taste_own_experience_revises);
    HU_RUN_TEST(taste_drift_bounded_per_step);
    HU_RUN_TEST(taste_drift_clamps);
    HU_RUN_TEST(taste_drift_no_whiplash);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(taste_store_roundtrip);
    HU_RUN_TEST(taste_isolated_from_seth_opinion_path);
    HU_RUN_TEST(taste_starter_seed_loads);
    HU_RUN_TEST(taste_directive_honest_and_relevant);
#endif
}
