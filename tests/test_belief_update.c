/* ─────────────────────────────────────────────────────────────────────────
 * test_belief_update.c
 *
 * Pins the pure conviction-loop decision faculty (hu_belief_update_decide)
 * plus its evidence-cue and conviction-mapping helpers.
 *
 * Spec: docs/plans/2026-05-29-conviction-loop/  (ACs 2, 3, 4)
 *
 * The faculty decides whether a held opinion should STRENGTHEN / WEAKEN /
 * FLIP / stay NO_CHANGE given facts about the current turn. The load-bearing
 * invariants:
 *   - No stance or no evidence  -> NO_CHANGE (nothing to update)
 *   - Reassertion (no new evidence) -> NO_CHANGE  (anti-sycophancy, AC-2)
 *   - Agreeing evidence -> STRENGTHEN
 *   - Contradicting evidence: FLIP only if conviction is not strong;
 *     a strong conviction WEAKENS first (erode before snap)
 *   - Per-conversation cap reached -> NO_CHANGE  (AC-4)
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/agent/belief_update.h"
#include "test_framework.h"

#include <string.h>

/* Convenience: a baseline "ripe for update" fact set; tests tweak one field. */
static hu_belief_facts_t base_facts(void) {
    hu_belief_facts_t f;
    f.stance_exists = true;
    f.has_new_evidence = true;
    f.is_reassertion = false;
    f.evidence_contradicts = false;
    f.current_conviction = 0.5;
    f.changes_this_convo = 0;
    return f;
}

/* ── Refusal cases ──────────────────────────────────────────────────────── */

static void belief_null_is_no_change(void) {
    HU_ASSERT_EQ((int)hu_belief_update_decide(NULL), (int)HU_BELIEF_NO_CHANGE);
}

static void belief_no_stance_is_no_change(void) {
    hu_belief_facts_t f = base_facts();
    f.stance_exists = false;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_NO_CHANGE);
}

static void belief_no_evidence_is_no_change(void) {
    hu_belief_facts_t f = base_facts();
    f.has_new_evidence = false;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_NO_CHANGE);
}

/* AC-2: pure reassertion (even when flagged as "evidence") must NOT update.
 * Reassertion VETOES evidence — this is the anti-sycophancy spine. */
static void belief_reassertion_vetoes_update(void) {
    hu_belief_facts_t f = base_facts();
    f.is_reassertion = true;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_NO_CHANGE);

    /* Even contradicting evidence cannot flip on a reassertion. */
    f.evidence_contradicts = true;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_NO_CHANGE);
}

/* ── Positive update cases ──────────────────────────────────────────────── */

static void belief_agreeing_evidence_strengthens(void) {
    hu_belief_facts_t f = base_facts();
    f.evidence_contradicts = false;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_STRENGTHEN);
}

static void belief_weak_conviction_flips_on_contradiction(void) {
    hu_belief_facts_t f = base_facts();
    f.evidence_contradicts = true;
    f.current_conviction = 0.5; /* <= ceil -> flips */
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_FLIP);
}

static void belief_strong_conviction_weakens_not_flips(void) {
    hu_belief_facts_t f = base_facts();
    f.evidence_contradicts = true;
    f.current_conviction = 0.9; /* > ceil -> erode first */
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_WEAKEN);
}

/* Boundary: exactly at the ceiling weakens (ceil is inclusive of "strong"). */
static void belief_at_flip_ceiling_weakens(void) {
    hu_belief_facts_t f = base_facts();
    f.evidence_contradicts = true;
    f.current_conviction = HU_BELIEF_FLIP_CONVICTION_CEIL;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_WEAKEN);
}

/* AC-4: per-conversation cap suppresses further changes. */
static void belief_convo_cap_suppresses(void) {
    hu_belief_facts_t f = base_facts();
    f.changes_this_convo = HU_BELIEF_MAX_CHANGES_PER_CONVO; /* at cap */
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_NO_CHANGE);

    /* Just under the cap still updates. */
    f.changes_this_convo = HU_BELIEF_MAX_CHANGES_PER_CONVO - 1;
    HU_ASSERT_EQ((int)hu_belief_update_decide(&f), (int)HU_BELIEF_STRENGTHEN);
}

/* ── Evidence-cue helper ────────────────────────────────────────────────── */

static void evidence_cue_detects_argument_markers(void) {
    HU_ASSERT_TRUE(hu_belief_msg_has_evidence_cue("because the data shows otherwise", 31));
    HU_ASSERT_TRUE(hu_belief_msg_has_evidence_cue("actually, a study found the opposite", 36));
    HU_ASSERT_TRUE(hu_belief_msg_has_evidence_cue("turns out the source was wrong", 30));
}

static void evidence_cue_ignores_bare_assertion(void) {
    HU_ASSERT_FALSE(hu_belief_msg_has_evidence_cue("no you're wrong", 15));
    HU_ASSERT_FALSE(hu_belief_msg_has_evidence_cue("i still think so", 16));
    HU_ASSERT_FALSE(hu_belief_msg_has_evidence_cue(NULL, 0));
    HU_ASSERT_FALSE(hu_belief_msg_has_evidence_cue("", 0));
}

/* Word-boundary: "factory" must NOT match the "fact" cue. */
static void evidence_cue_respects_word_boundary(void) {
    HU_ASSERT_FALSE(hu_belief_msg_has_evidence_cue("the factory closed", 18));
}

/* ── Conviction mapping ─────────────────────────────────────────────────── */

static void conviction_map_strengthen_raises_capped(void) {
    HU_ASSERT_FLOAT_EQ(hu_belief_conviction_for(HU_BELIEF_STRENGTHEN, 0.5), 0.7, 1e-9);
    HU_ASSERT_FLOAT_EQ(hu_belief_conviction_for(HU_BELIEF_STRENGTHEN, 0.95), 1.0, 1e-9);
}

static void conviction_map_weaken_lowers_floored(void) {
    HU_ASSERT_FLOAT_EQ(hu_belief_conviction_for(HU_BELIEF_WEAKEN, 0.5), 0.3, 1e-9);
    HU_ASSERT_FLOAT_EQ(hu_belief_conviction_for(HU_BELIEF_WEAKEN, 0.1), 0.0, 1e-9);
}

static void conviction_map_flip_is_fresh_moderate(void) {
    HU_ASSERT_FLOAT_EQ(hu_belief_conviction_for(HU_BELIEF_FLIP, 0.9), 0.55, 1e-9);
}

static void conviction_map_no_change_is_identity(void) {
    HU_ASSERT_FLOAT_EQ(hu_belief_conviction_for(HU_BELIEF_NO_CHANGE, 0.42), 0.42, 1e-9);
}

/* ── Integration: evaluator against an in-memory opinion store ──────────────
 * These exercise hu_belief_update_evaluate_turn end-to-end (decide + persist).
 * SQLite-gated; a stub keeps the symbols resolvable when SQLite is off. */
#ifdef HU_ENABLE_SQLITE

#include "human/behavior/pressure_history.h"
#include "human/behavior/trust.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/evolved_opinions.h"
#include <time.h>

/* Seed a held opinion the agent already believes. */
static sqlite3 *seed_opinion(hu_memory_t *mem, const char *topic, const char *stance,
                             double conviction) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    hu_evolved_opinions_ensure_table(db);
    hu_opinion_history_ensure_table(db);
    hu_evolved_opinion_upsert(db, topic, strlen(topic), stance, strlen(stance), conviction,
                              (int64_t)1000);
    return db;
}

/* AC-1: genuine contradicting evidence on a held topic flips the stance and
 * records exactly one opinion_history row with a non-empty reason. */
static void belief_eval_flip_records_one_history_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = seed_opinion(&mem, "remote work", "is overrated", 0.5);

    const char *msg = "I disagree — actually, the data shows remote work boosts output";
    char *dir = NULL;
    size_t dlen = 0;
    bool changed = false;
    HU_ASSERT_EQ(hu_belief_update_evaluate_turn(&alloc, db, NULL, msg, strlen(msg), 0,
                                                (int64_t)2000, &dir, &dlen, &changed),
                 HU_OK);
    HU_ASSERT_TRUE(changed);

    hu_opinion_history_entry_t *hist = NULL;
    size_t hcount = 0;
    HU_ASSERT_EQ(hu_evolved_opinion_history(&alloc, db, "remote work", 11, &hist, &hcount), HU_OK);
    HU_ASSERT_EQ(hcount, 1u);
    HU_ASSERT_NOT_NULL(hist[0].change_reason);
    HU_ASSERT_GT(hist[0].change_reason_len, 0);
    hu_opinion_history_free(&alloc, hist, hcount);
    if (dir)
        alloc.free(alloc.ctx, dir, dlen + 1);
    mem.vtable->deinit(mem.ctx);
}

/* AC-2: a reassertion of a claim already pushed back on does NOT update —
 * the anti-sycophancy veto holds even with an evidence-flavored message. */
static void belief_eval_reassertion_does_not_update(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = seed_opinion(&mem, "remote work", "is overrated", 0.5);

    const char *msg = "I disagree — actually the data shows remote work is better";

    hu_pressure_history_t ph;
    hu_pressure_history_init(&ph);
    /* The agent already pushed back on this exact claim last turn. */
    hu_pressure_history_observe(&ph, 1, msg, strlen(msg), HU_TRUST_PUSH_BACK);

    char *dir = NULL;
    size_t dlen = 0;
    bool changed = true;
    HU_ASSERT_EQ(hu_belief_update_evaluate_turn(&alloc, db, &ph, msg, strlen(msg), 0, (int64_t)2000,
                                                &dir, &dlen, &changed),
                 HU_OK);
    HU_ASSERT_FALSE(changed);

    hu_opinion_history_entry_t *hist = NULL;
    size_t hcount = 0;
    HU_ASSERT_EQ(hu_evolved_opinion_history(&alloc, db, "remote work", 11, &hist, &hcount), HU_OK);
    HU_ASSERT_EQ(hcount, 0u);
    hu_opinion_history_free(&alloc, hist, hcount);
    if (dir)
        alloc.free(alloc.ctx, dir, dlen + 1);
    mem.vtable->deinit(mem.ctx);
}

/* AC-4: once the per-conversation change cap is reached, no further update. */
static void belief_eval_convo_cap_blocks_update(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = seed_opinion(&mem, "remote work", "is overrated", 0.5);

    const char *msg = "I disagree — actually, the data shows remote work boosts output";
    char *dir = NULL;
    size_t dlen = 0;
    bool changed = true;
    HU_ASSERT_EQ(hu_belief_update_evaluate_turn(&alloc, db, NULL, msg, strlen(msg),
                                                HU_BELIEF_MAX_CHANGES_PER_CONVO, (int64_t)2000,
                                                &dir, &dlen, &changed),
                 HU_OK);
    HU_ASSERT_FALSE(changed);
    if (dir)
        alloc.free(alloc.ctx, dir, dlen + 1);
    mem.vtable->deinit(mem.ctx);
}

/* No evidence cue → no update, even if the topic is referenced. */
static void belief_eval_bare_assertion_does_not_update(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = seed_opinion(&mem, "remote work", "is overrated", 0.5);

    const char *msg = "no, remote work is great, you're wrong";
    char *dir = NULL;
    size_t dlen = 0;
    bool changed = true;
    HU_ASSERT_EQ(hu_belief_update_evaluate_turn(&alloc, db, NULL, msg, strlen(msg), 0,
                                                (int64_t)2000, &dir, &dlen, &changed),
                 HU_OK);
    HU_ASSERT_FALSE(changed);
    if (dir)
        alloc.free(alloc.ctx, dir, dlen + 1);
    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

void run_belief_update_tests(void);
void run_belief_update_tests(void) {
    HU_TEST_SUITE("belief_update");

    HU_RUN_TEST(belief_null_is_no_change);
    HU_RUN_TEST(belief_no_stance_is_no_change);
    HU_RUN_TEST(belief_no_evidence_is_no_change);
    HU_RUN_TEST(belief_reassertion_vetoes_update);
    HU_RUN_TEST(belief_agreeing_evidence_strengthens);
    HU_RUN_TEST(belief_weak_conviction_flips_on_contradiction);
    HU_RUN_TEST(belief_strong_conviction_weakens_not_flips);
    HU_RUN_TEST(belief_at_flip_ceiling_weakens);
    HU_RUN_TEST(belief_convo_cap_suppresses);

    HU_RUN_TEST(evidence_cue_detects_argument_markers);
    HU_RUN_TEST(evidence_cue_ignores_bare_assertion);
    HU_RUN_TEST(evidence_cue_respects_word_boundary);

    HU_RUN_TEST(conviction_map_strengthen_raises_capped);
    HU_RUN_TEST(conviction_map_weaken_lowers_floored);
    HU_RUN_TEST(conviction_map_flip_is_fresh_moderate);
    HU_RUN_TEST(conviction_map_no_change_is_identity);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(belief_eval_flip_records_one_history_row);
    HU_RUN_TEST(belief_eval_reassertion_does_not_update);
    HU_RUN_TEST(belief_eval_convo_cap_blocks_update);
    HU_RUN_TEST(belief_eval_bare_assertion_does_not_update);
#endif
}
