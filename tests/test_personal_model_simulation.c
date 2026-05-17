/*
 * Sprint 4 — M2 measurement bundle: multi-turn behavioral simulation.
 *
 * The existing 60+ tests in test_personal_model.c probe single
 * functions: one ingest, one fact, one decay. None of them drive the
 * model through a realistic conversation.
 *
 * This file ships four integration tests that prove the M2 capability
 * actually behaves the way CLAUDE.md claims when used over many turns:
 *
 *   B1 — 50-turn deterministic simulation harness.
 *        Drives ingest 50 times, asserts checkpoint state at turns
 *        1, 10, 25, 50.
 *
 *   B2 — Drift / time-travel regression.
 *        After B1's simulation, advance simulated time by one + two +
 *        four fact half-lives and assert effective scores halve, then
 *        floor.
 *
 *   B3 — 1000-turn invariant stress test.
 *        Random-but-deterministic message stream; asserts bounded
 *        state at every 100-turn checkpoint (no array overflow,
 *        scores in [0,1], prompt fits in cap, NUL-terminated).
 *
 *   B4 — Save/load round-trip AFTER long simulation.
 *        The existing save/load test starts from a fresh model. This
 *        one runs B1, saves, reloads, runs another B1, saves, reloads
 *        — and asserts both round-trips preserve all observable state.
 *
 * Invocation:
 *   ./build/human_tests --filter=simulation
 */

#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Fixture: 50 turns over 14 days ───────────────────────────────── */

typedef struct sim_turn {
    const char *text;
    int from_user; /* 1 = user, 0 = agent reply */
    int day_offset;
    int hour;
} sim_turn_t;

/* Realistic-looking chat fixture. The patterns are deliberately
 * chosen to match `src/memory/fact_extract.c`'s pattern table
 * ("i work at ", "i live in ", "i love ", "i hate ", "my name is ",
 * "i'm working on ", "i need to ", etc.) so we can make precise
 * subject/predicate/object assertions.
 *
 * Distribution: 30 user turns + 20 agent turns. Style cues: most user
 * messages are lowercase / chat-style with abbreviations to give the
 * style EWMA a clear casual fingerprint. */
static const sim_turn_t k_simulation_50turns[] = {
    /* Day 0 — first session */
    {"hi! my name is alex", 1, 0, 9},
    {"nice to meet you alex", 0, 0, 9},
    {"i work at initech", 1, 0, 9},
    {"got it — what brings you here today?", 0, 0, 9},
    {"i'm working on shipping the deck this quarter", 1, 0, 9},

    /* Day 1 — preferences emerge */
    {"good morning", 0, 1, 8},
    {"u up? i love coffee in the morning", 1, 1, 8},
    {"absolutely, hot or cold brew?", 0, 1, 8},
    {"i prefer pour over tbh", 1, 1, 8},
    {"i hate mornings without coffee", 1, 1, 9},

    /* Day 2 — identity reinforced + topic introduced */
    {"i live in portland", 1, 2, 19},
    {"the rain too?", 0, 2, 19},
    {"lol i love hiking even in the rain", 1, 2, 19},
    {"i'm a hiker", 1, 2, 20},
    {"any goals for this week alex?", 0, 2, 20},
    {"i need to finalize the deck slides by friday", 1, 2, 20},

    /* Day 3 — repetition lifts confidence on existing facts */
    {"hey", 1, 3, 10},
    {"hey alex, how's the deck coming?", 0, 3, 10},
    {"slow but i'm working on the architecture diagrams", 1, 3, 10},
    {"how about hiking this weekend?", 0, 3, 10},
    {"i love hiking, always have", 1, 3, 10},

    /* Day 4 — dislikes accumulate (avoid-line trigger) */
    {"i don't like meetings before 10am", 1, 4, 9},
    {"noted", 0, 4, 9},
    {"i hate small talk in standups too", 1, 4, 11},
    {"that's fair", 0, 4, 11},

    /* Day 5 — abbreviation cluster (style fingerprint) */
    {"btw the deck got delayed", 1, 5, 14},
    {"oh no", 0, 5, 14},
    {"ty for asking. lmk if u see any sample decks", 1, 5, 15},

    /* Day 6 — second mention of portland (topic count++) */
    {"rn looking for coffee shops in portland", 1, 6, 8},
    {"any favorites?", 0, 6, 8},
    {"i love coffee from stumptown", 1, 6, 8},

    /* Day 7 — week mark */
    {"hi again", 1, 7, 9},
    {"welcome back!", 0, 7, 9},
    {"my name is still alex 😅", 1, 7, 9},

    /* Day 8-10 — goal progression */
    {"i'm working on the deck final cut", 1, 8, 13},
    {"i need to ship by tomorrow", 1, 9, 18},
    {"i shipped it 🎉", 1, 10, 21},
    {"congrats!", 0, 10, 21},

    /* Day 11-12 — new topic emerges (running) */
    {"i went for a run today", 1, 11, 7},
    {"how far?", 0, 11, 7},
    {"i love running in forest park", 1, 11, 7},
    {"i'm trying to do 5k three times a week", 1, 12, 7},

    /* Day 13 — coffee returns (topic mention_count grows) */
    {"i love coffee", 1, 13, 9},
    {"again with the coffee :)", 0, 13, 9},
    {"my favorite is still pour over", 1, 13, 9},

    /* Day 14 — final session */
    {"hey", 1, 14, 10},
    {"hi alex", 0, 14, 10},
    {"i'm working on a new project at initech", 1, 14, 10},
    {"i need to draft the spec by next week", 1, 14, 11},
    {"good luck", 0, 14, 11},
};
static const size_t k_simulation_50turns_count =
    sizeof(k_simulation_50turns) / sizeof(k_simulation_50turns[0]);

/* Synthetic clock: pick a fixed wall-clock anchor far enough in the
 * past that adding 14*86400 still lands well before "now", but recent
 * enough that timestamps look realistic in any debug logs. */
#define SIM_T0 1700000000LL /* 2023-11-14 22:13:20 UTC */

static int64_t sim_turn_timestamp(const sim_turn_t *t) {
    return SIM_T0 + (int64_t)t->day_offset * 86400LL + (int64_t)t->hour * 3600LL;
}

/* Run the fixture from the start through `up_to` turns (inclusive
 * count, so up_to=10 means turns 1..10). Returns the timestamp of
 * the final turn applied. */
static int64_t sim_run_through(hu_personal_model_t *model, size_t up_to) {
    if (up_to > k_simulation_50turns_count)
        up_to = k_simulation_50turns_count;
    int64_t last_ts = SIM_T0;
    for (size_t i = 0; i < up_to; i++) {
        const sim_turn_t *t = &k_simulation_50turns[i];
        int64_t ts = sim_turn_timestamp(t);
        last_ts = ts;
        hu_error_t err = hu_personal_model_ingest(model, t->text, strlen(t->text),
                                                  (bool)t->from_user, ts, /*prov=*/NULL);
        HU_ASSERT_EQ(err, HU_OK);
    }
    return last_ts;
}

/* Helper: count user turns up to a given checkpoint. */
static size_t sim_user_turns_through(size_t up_to) {
    if (up_to > k_simulation_50turns_count)
        up_to = k_simulation_50turns_count;
    size_t n = 0;
    for (size_t i = 0; i < up_to; i++)
        if (k_simulation_50turns[i].from_user)
            n++;
    return n;
}

/* Helper: find a fact whose predicate contains `pred` and object
 * contains `obj` (case-insensitive). Returns NULL on miss. */
static const hu_heuristic_fact_t *sim_find_fact(const hu_personal_model_t *model,
                                                const char *pred, const char *obj) {
    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];
        const char *p = f->predicate;
        const char *o = f->object;
        bool pred_ok = (pred == NULL) || (strstr(p, pred) != NULL);
        bool obj_ok = (obj == NULL) || (strstr(o, obj) != NULL);
        if (pred_ok && obj_ok)
            return f;
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────── */
/*  Story B1 — 50-turn deterministic simulation harness                */
/* ─────────────────────────────────────────────────────────────────── */

static void simulation_b1_fixture_is_well_formed(void) {
    /* Sanity-check the fixture itself before testing the model:
     * deterministic counts let later assertions reason about
     * sample_count without re-counting. */
    HU_ASSERT_EQ((long)k_simulation_50turns_count, 50L);
    size_t user = 0, agent = 0;
    for (size_t i = 0; i < k_simulation_50turns_count; i++) {
        if (k_simulation_50turns[i].from_user)
            user++;
        else
            agent++;
        /* timestamps must be monotonic non-decreasing */
        if (i > 0) {
            int64_t a = sim_turn_timestamp(&k_simulation_50turns[i - 1]);
            int64_t b = sim_turn_timestamp(&k_simulation_50turns[i]);
            HU_ASSERT_TRUE(b >= a);
        }
    }
    /* Don't pin exact counts — they shift if the fixture is tweaked.
     * The contract that matters: total is 50, neither side empty. */
    HU_ASSERT_EQ((long)(user + agent), 50L);
    HU_ASSERT_TRUE(user > agent); /* user-driven simulation */
    HU_ASSERT_TRUE(agent >= 5);   /* but agent turns exist for realism */
}

static void simulation_b1_turn_1_initial_state(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, 1);

    /* After turn 1 ("hi! my name is alex"): one user message, one
     * fact extracted (my name is alex), one style sample. */
    HU_ASSERT_EQ((unsigned)m.interaction_count, 1U);
    HU_ASSERT_EQ((unsigned)m.style.sample_count, 1U);
    HU_ASSERT_TRUE(m.fact_count >= 1U);
    const hu_heuristic_fact_t *name = sim_find_fact(&m, "my name is", "alex");
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(name->confidence >= 0.85f); /* 0.9 in pattern table */
}

static void simulation_b1_turn_10_accumulating(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, 10);

    /* By turn 10 we expect: name, work, working-on (deck), love
     * (coffee), prefer (pour over), hate (mornings ~ "mornings"). */
    HU_ASSERT_EQ((unsigned)m.interaction_count, 10U);
    HU_ASSERT_EQ((unsigned)m.style.sample_count,
                 (unsigned)sim_user_turns_through(10));

    HU_ASSERT_NOT_NULL(sim_find_fact(&m, "my name is", "alex"));
    HU_ASSERT_NOT_NULL(sim_find_fact(&m, "i work at", "initech"));
    HU_ASSERT_NOT_NULL(sim_find_fact(&m, "i'm working on", NULL));
    HU_ASSERT_NOT_NULL(sim_find_fact(&m, "i love", "coffee"));
    HU_ASSERT_NOT_NULL(sim_find_fact(&m, "i hate", NULL));
    HU_ASSERT_TRUE(m.fact_count >= 4U);

    /* Topic accumulation kicked in (facts insert bumps topics). */
    HU_ASSERT_TRUE(m.topic_count >= 1U);
}

static void simulation_b1_turn_25_style_emerges(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, 25);

    HU_ASSERT_EQ((unsigned)m.interaction_count, 25U);
    HU_ASSERT_EQ((unsigned)m.style.sample_count,
                 (unsigned)sim_user_turns_through(25));

    /* The fixture deliberately uses lowercase + chat abbreviations
     * for many user turns. After ~15 user samples the EWMA-tracked
     * style fingerprint should reflect that. */
    HU_ASSERT_TRUE(m.style.sample_count >= 15U);
    HU_ASSERT_TRUE(m.style.lowercase_ratio >= 0.4f);
    HU_ASSERT_TRUE(m.style.last_observed_at > 0);

    /* Topic accumulation: every fact insert and every fact-dedup
     * path calls bump_topic(nf->object), so the model has at least
     * as many topics as it has distinct object strings observed.
     * By turn 25 we expect a healthy population of topics. We
     * deliberately don't assert mention_count >= 2 — that would
     * require two extracted facts to share an object STRING, which
     * the fixture's natural-language phrasing rarely produces;
     * dedup is checked at the fact level, not the topic level. */
    HU_ASSERT_TRUE(m.topic_count >= 5U);

    /* Reinforcement: the predicate-key dedup path (fact_key_dup
     * matches subject+predicate only) means T7's "i love" fact
     * gets visited again at T20 ("i love hiking, always have").
     * The dup path lifts the existing fact's confidence via EWMA;
     * since both observations had confidence 0.8, the lifted value
     * is still ~0.8 (no change). What we CAN check is that the
     * fact's last_seen_at moved forward past T7's wall clock. */
    const hu_heuristic_fact_t *love = sim_find_fact(&m, "i love", NULL);
    HU_ASSERT_NOT_NULL(love);
    HU_ASSERT_TRUE(love->last_seen_at > SIM_T0 + 1LL * 86400LL);
}

static void simulation_b1_turn_50_terminal_state(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t last_ts = sim_run_through(&m, k_simulation_50turns_count);

    /* Terminal-state contract. */
    HU_ASSERT_EQ((unsigned)m.interaction_count, 50U);
    HU_ASSERT_EQ((unsigned)m.style.sample_count,
                 (unsigned)sim_user_turns_through(50));
    HU_ASSERT_EQ((long long)m.style.last_observed_at, (long long)last_ts);
    HU_ASSERT_EQ((long long)m.updated_at, (long long)last_ts);
    HU_ASSERT_EQ((long long)m.created_at, (long long)(SIM_T0 + 9LL * 3600LL));

    /* Bounded state: never exceeds slot caps. */
    HU_ASSERT_TRUE(m.fact_count <= (size_t)HU_PM_MAX_FACTS);
    HU_ASSERT_TRUE(m.topic_count <= (size_t)HU_PM_MAX_TOPICS);

    /* Identity fact survived 50 turns. */
    const hu_heuristic_fact_t *name = sim_find_fact(&m, "my name is", "alex");
    HU_ASSERT_NOT_NULL(name);

    /* "i work at initech" fact still present (single observation;
     * the day-14 turn uses "i'm working on" / different predicate,
     * which is treated as a separate prescriptive fact, not a
     * refresh of "i work at"). */
    const hu_heuristic_fact_t *work = sim_find_fact(&m, "i work at", "initech");
    HU_ASSERT_NOT_NULL(work);

    /* The "i love" predicate, on the other hand, is hit multiple
     * times across the fixture (T7, T20, T30, T42). Each subsequent
     * hit dedups via subject+predicate match and refreshes
     * last_seen_at. Verify the refresh ran — last_seen_at on the
     * single "i love" fact should be from the last love-mention,
     * not the first. */
    const hu_heuristic_fact_t *love = sim_find_fact(&m, "i love", NULL);
    HU_ASSERT_NOT_NULL(love);
    /* T42 is on day 13 ("i love coffee"). last_seen_at should be at
     * or after that timestamp. */
    HU_ASSERT_TRUE(love->last_seen_at >= SIM_T0 + 13LL * 86400LL);
}

static void simulation_b1_turn_50_prompt_contract(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, k_simulation_50turns_count);

    char buf[8192];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(n < sizeof(buf));
    HU_ASSERT_EQ((int)buf[n], 0);
    /* The standard header always present in non-empty prompts. */
    HU_ASSERT_STR_CONTAINS(buf, "[Personal Context]");
}

/* ─────────────────────────────────────────────────────────────────── */
/*  Story B2 — drift / time-travel regression                          */
/* ─────────────────────────────────────────────────────────────────── */

static void simulation_b2_fact_decay_halves_at_sim_end_plus_one_half_life(void) {
    /* Run B1, pluck a fact, then check effective_confidence at
     * last_seen_at + 90d. The existing test_personal_model.c proves
     * this for synthetic stamping; this version proves the same
     * primitive holds when called on a fact that was actually
     * accumulated through hu_personal_model_ingest. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, k_simulation_50turns_count);

    const hu_heuristic_fact_t *love = sim_find_fact(&m, "i love", NULL);
    HU_ASSERT_NOT_NULL(love);
    HU_ASSERT_TRUE(love->last_seen_at > 0);

    float raw = love->confidence;
    HU_ASSERT_TRUE(raw > 0.0f);

    /* At last_seen_at: no decay. */
    float now0 = hu_heuristic_fact_effective_confidence(love, love->last_seen_at);
    HU_ASSERT_FLOAT_EQ(now0, raw, 0.001f);

    /* +1 half-life: ~0.5 × raw. (decay is approximated via the
     * lookup table in fact_extract.c, so loosen tolerance to ±15%
     * of the raw value.) */
    int64_t t1 = love->last_seen_at + HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    float e1 = hu_heuristic_fact_effective_confidence(love, t1);
    HU_ASSERT_TRUE(e1 > 0.40f * raw);
    HU_ASSERT_TRUE(e1 < 0.60f * raw);

    /* +2 half-lives: ~0.25 × raw. */
    int64_t t2 = love->last_seen_at + 2 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    float e2 = hu_heuristic_fact_effective_confidence(love, t2);
    HU_ASSERT_TRUE(e2 > 0.18f * raw);
    HU_ASSERT_TRUE(e2 < 0.32f * raw);

    /* +20 half-lives: floored. */
    int64_t t_far = love->last_seen_at + 20 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    float e_far = hu_heuristic_fact_effective_confidence(love, t_far);
    HU_ASSERT_TRUE(e_far >= 0.0f);
    HU_ASSERT_TRUE(e_far < 0.01f * raw);
}

static void simulation_b2_topic_decay_halves_at_sim_end_plus_one_half_life(void) {
    /* Topics use a 60-day half-life. Find any topic with a stamped
     * last_mentioned and verify decay shape. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, k_simulation_50turns_count);

    HU_ASSERT_TRUE(m.topic_count > 0);
    const hu_personal_topic_t *t = NULL;
    for (size_t i = 0; i < m.topic_count; i++) {
        if (m.topics[i].last_mentioned > 0 && m.topics[i].interest_score > 0.0f) {
            t = &m.topics[i];
            break;
        }
    }
    HU_ASSERT_NOT_NULL(t);

    float raw = t->interest_score;
    int64_t base = t->last_mentioned;

    float at_zero = hu_personal_topic_effective_score(t, base);
    HU_ASSERT_FLOAT_EQ(at_zero, raw, 0.001f);

    int64_t one_hl = base + HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC;
    float at_one_hl = hu_personal_topic_effective_score(t, one_hl);
    HU_ASSERT_TRUE(at_one_hl > 0.40f * raw);
    HU_ASSERT_TRUE(at_one_hl < 0.60f * raw);
}

static void simulation_b2_style_freshness_decays_after_long_silence(void) {
    /* Style freshness has a 180-day half-life. After 360 days of
     * silence, freshness should be ≤ 0.30 (two half-lives → 0.25). */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    sim_run_through(&m, k_simulation_50turns_count);

    HU_ASSERT_TRUE(m.style.last_observed_at > 0);
    float fresh_now =
        hu_personal_communication_style_freshness(&m.style, m.style.last_observed_at);
    HU_ASSERT_FLOAT_EQ(fresh_now, 1.0f, 0.001f);

    int64_t two_hl = m.style.last_observed_at + 2 * HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;
    float fresh_2hl = hu_personal_communication_style_freshness(&m.style, two_hl);
    HU_ASSERT_TRUE(fresh_2hl > 0.18f);
    HU_ASSERT_TRUE(fresh_2hl < 0.32f);
}

static void simulation_b2_apply_decay_prunes_after_long_drift(void) {
    /* Run B1, then call apply_decay with a `now` two years past the
     * end of the simulation. The lowest-confidence facts and topics
     * fall below HU_PM_FORGET_FLOOR and get pruned. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t sim_end = sim_run_through(&m, k_simulation_50turns_count);

    size_t facts_before = m.fact_count;
    size_t topics_before = m.topic_count;
    HU_ASSERT_TRUE(facts_before > 0);
    HU_ASSERT_TRUE(topics_before > 0);

    /* +730 days ≈ 8 fact half-lives, 12 topic half-lives, 4 style
     * half-lives. Effective scores well below HU_PM_FORGET_FLOOR. */
    int64_t far_future = sim_end + 730LL * 86400LL;
    size_t pruned = hu_personal_model_apply_decay(&m, far_future);

    HU_ASSERT_TRUE(pruned > 0);
    HU_ASSERT_TRUE(m.fact_count < facts_before);
    HU_ASSERT_TRUE(m.topic_count < topics_before);
}

static void simulation_b2_prompt_shrinks_after_long_drift(void) {
    /* The prompt builder uses model->updated_at as `now`. After
     * advancing updated_at two years, the rebuilt prompt should
     * either shrink (decayed signal dropped) or, at worst, stay
     * the same length but differ in content. We assert the
     * stronger property: it must shrink, since the fixture
     * accumulates enough signal that pruning some of it is the
     * expected outcome. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t sim_end = sim_run_through(&m, k_simulation_50turns_count);

    char buf_now[8192];
    size_t n_now = hu_personal_model_build_prompt(&m, buf_now, sizeof(buf_now));
    HU_ASSERT_GT((long)n_now, 0L);

    /* Time-travel: bump updated_at by 730 days and re-build. */
    m.updated_at = sim_end + 730LL * 86400LL;

    char buf_far[8192];
    size_t n_far = hu_personal_model_build_prompt(&m, buf_far, sizeof(buf_far));
    HU_ASSERT_GT((long)n_far, 0L);

    /* The header [Personal Context] and the basic style summary
     * survive in both — but the body should shrink as decayed
     * signal gets dropped from the prompt. Assert at least
     * 100 bytes of difference (a single avoid-line or topic line). */
    HU_ASSERT_TRUE(n_far + 100 <= n_now);
}

/* ─────────────────────────────────────────────────────────────────── */
/*  Story B3 — 1000-turn invariant stress test                         */
/* ─────────────────────────────────────────────────────────────────── */

/* A small pool of message templates that each match exactly one
 * fact_extract pattern when filled. The {OBJ} slot gets substituted
 * with one of a fixed set of objects; the result is deterministic
 * for any fixed PRNG seed. */
static const char *k_b3_templates[] = {
    "i love {OBJ}",
    "i hate {OBJ}",
    "i prefer {OBJ}",
    "i live in {OBJ}",
    "i work at {OBJ}",
    "my favorite is {OBJ}",
    "i'm working on {OBJ}",
    "i need to {OBJ}",
    "i'm trying to {OBJ}",
    "u up?",                     /* doesn't match — exits early path */
    "ok",                        /* short message — style sample */
    "BTW THAT'S COOL",           /* uppercase — drives lowercase_ratio down */
    "lol",                       /* abbreviation */
    "ty",                        /* abbreviation */
    "rn busy",                   /* abbreviation cluster */
};
static const size_t k_b3_template_count =
    sizeof(k_b3_templates) / sizeof(k_b3_templates[0]);

static const char *k_b3_objects[] = {
    "coffee", "tea", "hiking", "running", "cooking", "reading",
    "portland", "seattle", "san francisco", "new york",
    "initech", "globex", "soylent",
    "the deck", "the launch", "the migration", "the runbook",
    "ship today", "ship tomorrow", "draft the spec",
};
static const size_t k_b3_object_count =
    sizeof(k_b3_objects) / sizeof(k_b3_objects[0]);

static void b3_render_message(unsigned int *seed, char *out, size_t cap) {
    /* rand_r is BSD/POSIX, available on macOS + Linux glibc. The
     * test framework already uses libc-portable code; rand_r keeps
     * the PRNG state under our control and away from any global
     * srand() the suite might also touch. */
    const char *tpl = k_b3_templates[rand_r(seed) % k_b3_template_count];
    const char *obj = k_b3_objects[rand_r(seed) % k_b3_object_count];

    const char *slot = strstr(tpl, "{OBJ}");
    if (!slot) {
        strncpy(out, tpl, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    size_t prefix_len = (size_t)(slot - tpl);
    if (prefix_len >= cap)
        prefix_len = cap - 1;
    memcpy(out, tpl, prefix_len);
    size_t pos = prefix_len;
    size_t obj_len = strlen(obj);
    if (pos + obj_len >= cap)
        obj_len = cap - 1 - pos;
    memcpy(out + pos, obj, obj_len);
    pos += obj_len;
    const char *suffix = slot + 5; /* skip "{OBJ}" */
    size_t suffix_len = strlen(suffix);
    if (pos + suffix_len >= cap)
        suffix_len = cap - 1 - pos;
    memcpy(out + pos, suffix, suffix_len);
    pos += suffix_len;
    out[pos] = '\0';
}

static void b3_assert_invariants(const hu_personal_model_t *m, int64_t now) {
    /* Slot caps: ingest must never overflow the fixed-size arrays. */
    HU_ASSERT_TRUE(m->fact_count <= (size_t)HU_PM_MAX_FACTS);
    HU_ASSERT_TRUE(m->topic_count <= (size_t)HU_PM_MAX_TOPICS);
    HU_ASSERT_TRUE(m->goal_count <= (size_t)HU_PM_MAX_GOALS);

    /* Per-fact bounds. */
    for (size_t i = 0; i < m->fact_count; i++) {
        const hu_heuristic_fact_t *f = &m->facts[i];
        HU_ASSERT_TRUE(f->confidence >= 0.0f);
        HU_ASSERT_TRUE(f->confidence <= 1.0f);
        float eff = hu_heuristic_fact_effective_confidence(f, now);
        HU_ASSERT_TRUE(eff >= 0.0f);
        HU_ASSERT_TRUE(eff <= 1.0f + 0.001f); /* float epsilon slack */
    }

    /* Per-topic bounds. */
    for (size_t i = 0; i < m->topic_count; i++) {
        const hu_personal_topic_t *t = &m->topics[i];
        HU_ASSERT_TRUE(t->interest_score >= 0.0f);
        HU_ASSERT_TRUE(t->interest_score <= 1.0f + 0.001f);
        float eff = hu_personal_topic_effective_score(t, now);
        HU_ASSERT_TRUE(eff >= 0.0f);
        HU_ASSERT_TRUE(eff <= 1.0f + 0.001f);
    }

    /* Per-goal bounds (priority is 0 for inactive — covered). */
    for (size_t i = 0; i < m->goal_count; i++) {
        const hu_personal_goal_t *g = &m->goals[i];
        HU_ASSERT_TRUE(g->progress >= 0.0f);
        HU_ASSERT_TRUE(g->progress <= 1.0f + 0.001f);
        float eff = hu_personal_goal_effective_priority(g, now);
        HU_ASSERT_TRUE(eff >= 0.0f);
        HU_ASSERT_TRUE(eff <= 1.0f + 0.001f);
    }

    /* Style axes. */
    HU_ASSERT_TRUE(m->style.formality >= 0.0f && m->style.formality <= 1.0f + 0.001f);
    HU_ASSERT_TRUE(m->style.verbosity >= 0.0f && m->style.verbosity <= 1.0f + 0.001f);
    HU_ASSERT_TRUE(m->style.emoji_frequency >= 0.0f &&
                   m->style.emoji_frequency <= 1.0f + 0.001f);
    HU_ASSERT_TRUE(m->style.humor_receptivity >= 0.0f &&
                   m->style.humor_receptivity <= 1.0f + 0.001f);
    HU_ASSERT_TRUE(m->style.lowercase_ratio >= 0.0f &&
                   m->style.lowercase_ratio <= 1.0f + 0.001f);
    HU_ASSERT_TRUE(m->style.abbreviation_ratio >= 0.0f &&
                   m->style.abbreviation_ratio <= 1.0f + 0.001f);

    /* Build prompt within a 4 KB cap returns NUL-terminated, non-zero. */
    char buf[4096];
    size_t n = hu_personal_model_build_prompt(m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(n < sizeof(buf));
    HU_ASSERT_EQ((int)buf[n], 0);
}

static void simulation_b3_thousand_turn_invariants(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    unsigned int seed = 42; /* deterministic */
    char msg[256];
    /* Spread 1000 turns over 60 simulated days at random hours. */
    for (size_t turn = 1; turn <= 1000; turn++) {
        b3_render_message(&seed, msg, sizeof(msg));
        int day = (int)((turn - 1) % 60);
        int hour = (int)(rand_r(&seed) % 24);
        int64_t ts = SIM_T0 + (int64_t)day * 86400LL + (int64_t)hour * 3600LL;
        bool from_user = (rand_r(&seed) % 4) != 0; /* ~75% user turns */
        hu_error_t err =
            hu_personal_model_ingest(&m, msg, strlen(msg), from_user, ts, /*prov=*/NULL);
        HU_ASSERT_EQ(err, HU_OK);

        if (turn % 100 == 0)
            b3_assert_invariants(&m, ts);
    }

    /* After 1000 turns, the model should be at or near saturation. */
    HU_ASSERT_TRUE(m.fact_count > 0);
    HU_ASSERT_TRUE(m.topic_count > 0);
    HU_ASSERT_EQ((unsigned)m.interaction_count, 1000U);
}

static void simulation_b3_thousand_turn_save_load_after_stress(void) {
    /* Same stress run, then save → load → invariants on the loaded
     * model. Pins that the binary save format survives a saturated
     * model end-to-end. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    unsigned int seed = 1337;
    char msg[256];
    int64_t last_ts = SIM_T0;
    for (size_t turn = 1; turn <= 1000; turn++) {
        b3_render_message(&seed, msg, sizeof(msg));
        int day = (int)((turn - 1) % 60);
        int hour = (int)(rand_r(&seed) % 24);
        last_ts = SIM_T0 + (int64_t)day * 86400LL + (int64_t)hour * 3600LL;
        bool from_user = (rand_r(&seed) % 4) != 0;
        HU_ASSERT_EQ(
            hu_personal_model_ingest(&m, msg, strlen(msg), from_user, last_ts, /*prov=*/NULL),
            HU_OK);
    }

    char path[256];
    snprintf(path, sizeof(path), "/tmp/sprint4_b3_pm_%d.bin", getpid());
    HU_ASSERT_EQ(hu_personal_model_save(&m, path), HU_OK);

    hu_personal_model_t loaded;
    hu_personal_model_init(&loaded);
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);

    HU_ASSERT_EQ((unsigned)loaded.interaction_count,
                 (unsigned)m.interaction_count);
    HU_ASSERT_EQ((unsigned)loaded.fact_count, (unsigned)m.fact_count);
    HU_ASSERT_EQ((unsigned)loaded.topic_count, (unsigned)m.topic_count);
    HU_ASSERT_EQ((unsigned)loaded.style.sample_count,
                 (unsigned)m.style.sample_count);

    b3_assert_invariants(&loaded, last_ts);

    unlink(path);
}

/* ─────────────────────────────────────────────────────────────────── */
/*  Story B4 — Save/load round-trip AFTER long simulation              */
/* ─────────────────────────────────────────────────────────────────── */

/* Compare two models at the public-field level. Used by both B4
 * cycles. Asserts every counter, every fact identity, and every
 * style axis matches byte-for-byte (within float epsilon). */
static void b4_assert_models_equivalent(const hu_personal_model_t *a,
                                        const hu_personal_model_t *b) {
    HU_ASSERT_EQ((unsigned)a->interaction_count, (unsigned)b->interaction_count);
    HU_ASSERT_EQ((unsigned)a->fact_count, (unsigned)b->fact_count);
    HU_ASSERT_EQ((unsigned)a->topic_count, (unsigned)b->topic_count);
    HU_ASSERT_EQ((unsigned)a->goal_count, (unsigned)b->goal_count);
    HU_ASSERT_EQ((unsigned)a->style.sample_count, (unsigned)b->style.sample_count);
    HU_ASSERT_EQ((long long)a->created_at, (long long)b->created_at);
    HU_ASSERT_EQ((long long)a->updated_at, (long long)b->updated_at);
    HU_ASSERT_EQ((long long)a->style.last_observed_at,
                 (long long)b->style.last_observed_at);
    HU_ASSERT_EQ((unsigned)a->version, (unsigned)b->version);

    for (size_t i = 0; i < a->fact_count; i++) {
        const hu_heuristic_fact_t *fa = &a->facts[i];
        const hu_heuristic_fact_t *fb = &b->facts[i];
        HU_ASSERT_STR_EQ(fa->subject, fb->subject);
        HU_ASSERT_STR_EQ(fa->predicate, fb->predicate);
        HU_ASSERT_STR_EQ(fa->object, fb->object);
        HU_ASSERT_FLOAT_EQ(fa->confidence, fb->confidence, 0.0001f);
        HU_ASSERT_EQ((long long)fa->last_seen_at, (long long)fb->last_seen_at);
    }

    HU_ASSERT_FLOAT_EQ(a->style.formality, b->style.formality, 0.0001f);
    HU_ASSERT_FLOAT_EQ(a->style.verbosity, b->style.verbosity, 0.0001f);
    HU_ASSERT_FLOAT_EQ(a->style.emoji_frequency, b->style.emoji_frequency, 0.0001f);
    HU_ASSERT_FLOAT_EQ(a->style.humor_receptivity, b->style.humor_receptivity, 0.0001f);
    HU_ASSERT_FLOAT_EQ(a->style.lowercase_ratio, b->style.lowercase_ratio, 0.0001f);
    HU_ASSERT_FLOAT_EQ(a->style.abbreviation_ratio, b->style.abbreviation_ratio, 0.0001f);
}

static void simulation_b4_save_load_after_50_turns_round_trips_state(void) {
    hu_personal_model_t source;
    hu_personal_model_init(&source);
    sim_run_through(&source, k_simulation_50turns_count);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/sprint4_b4_pm_%d.bin", getpid());

    HU_ASSERT_EQ(hu_personal_model_save(&source, path), HU_OK);

    hu_personal_model_t loaded;
    hu_personal_model_init(&loaded);
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);

    b4_assert_models_equivalent(&source, &loaded);

    /* Build prompt on both — must be byte-identical. */
    char prompt_source[8192];
    char prompt_loaded[8192];
    size_t n_src = hu_personal_model_build_prompt(&source, prompt_source,
                                                  sizeof(prompt_source));
    size_t n_load = hu_personal_model_build_prompt(&loaded, prompt_loaded,
                                                   sizeof(prompt_loaded));
    HU_ASSERT_EQ((unsigned)n_src, (unsigned)n_load);
    HU_ASSERT_STR_EQ(prompt_source, prompt_loaded);

    unlink(path);
}

static void simulation_b4_two_cycle_save_load_preserves_state(void) {
    /* Cycle 1: run B1 → save → load → check equivalent.
     * Cycle 2: run B1 again on the loaded model (so interaction
     * count doubles, second pass refreshes existing facts) →
     * save → load → check equivalent.
     *
     * Pins that save/load is idempotent across multiple cycles —
     * a corruption in the persistence layer that only manifested
     * after a load would surface here, where the existing
     * `personal_model_save_load_round_trips` test (single cycle on
     * a fresh model) wouldn't catch it. */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/sprint4_b4_two_%d.bin", getpid());

    /* Cycle 1 */
    hu_personal_model_t cycle1_source;
    hu_personal_model_init(&cycle1_source);
    sim_run_through(&cycle1_source, k_simulation_50turns_count);
    HU_ASSERT_EQ(hu_personal_model_save(&cycle1_source, path), HU_OK);

    hu_personal_model_t cycle1_loaded;
    hu_personal_model_init(&cycle1_loaded);
    HU_ASSERT_EQ(hu_personal_model_load(&cycle1_loaded, path), HU_OK);
    b4_assert_models_equivalent(&cycle1_source, &cycle1_loaded);

    size_t cycle1_facts = cycle1_loaded.fact_count;
    size_t cycle1_topics = cycle1_loaded.topic_count;
    HU_ASSERT_TRUE(cycle1_facts > 0);

    /* Cycle 2: run another 50 turns on the loaded model. */
    sim_run_through(&cycle1_loaded, k_simulation_50turns_count);
    /* interaction_count should now be ~100. The exact number depends
     * on whether ingest counted agent turns the same way both cycles
     * — assert 100 (the simulator counts every turn). */
    HU_ASSERT_EQ((unsigned)cycle1_loaded.interaction_count, 100U);
    /* Style sample_count should also have ~doubled (only user turns
     * count). */
    HU_ASSERT_TRUE(cycle1_loaded.style.sample_count >
                   cycle1_source.style.sample_count);

    /* Save the cycle-2 model and reload. */
    HU_ASSERT_EQ(hu_personal_model_save(&cycle1_loaded, path), HU_OK);
    hu_personal_model_t cycle2_loaded;
    hu_personal_model_init(&cycle2_loaded);
    HU_ASSERT_EQ(hu_personal_model_load(&cycle2_loaded, path), HU_OK);

    b4_assert_models_equivalent(&cycle1_loaded, &cycle2_loaded);

    /* Topic count survives — even when facts hit dup paths in the
     * second cycle, topic bumping is idempotent. */
    HU_ASSERT_TRUE(cycle2_loaded.topic_count >= cycle1_topics);

    unlink(path);
}

/* ─────────────────────────────────────────────────────────────────── */
/*  Test runner                                                        */
/* ─────────────────────────────────────────────────────────────────── */

void run_personal_model_simulation_tests(void) {
    HU_RUN_TEST(simulation_b1_fixture_is_well_formed);
    HU_RUN_TEST(simulation_b1_turn_1_initial_state);
    HU_RUN_TEST(simulation_b1_turn_10_accumulating);
    HU_RUN_TEST(simulation_b1_turn_25_style_emerges);
    HU_RUN_TEST(simulation_b1_turn_50_terminal_state);
    HU_RUN_TEST(simulation_b1_turn_50_prompt_contract);
    HU_RUN_TEST(simulation_b2_fact_decay_halves_at_sim_end_plus_one_half_life);
    HU_RUN_TEST(simulation_b2_topic_decay_halves_at_sim_end_plus_one_half_life);
    HU_RUN_TEST(simulation_b2_style_freshness_decays_after_long_silence);
    HU_RUN_TEST(simulation_b2_apply_decay_prunes_after_long_drift);
    HU_RUN_TEST(simulation_b2_prompt_shrinks_after_long_drift);
    HU_RUN_TEST(simulation_b3_thousand_turn_invariants);
    HU_RUN_TEST(simulation_b3_thousand_turn_save_load_after_stress);
    HU_RUN_TEST(simulation_b4_save_load_after_50_turns_round_trips_state);
    HU_RUN_TEST(simulation_b4_two_cycle_save_load_preserves_state);
}
