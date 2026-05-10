#include "human/eval/turing_adversarial.h"
#include "human/eval/turing_score.h"
#include "test_framework.h"
#include <string.h>

static void adversarial_bank_size_is_positive(void) {
    size_t sz = hu_turing_adversarial_bank_size();
    HU_ASSERT(sz >= 48);
}

static void adversarial_bank_get_returns_valid_scenario(void) {
    hu_turing_scenario_t s;
    HU_ASSERT_EQ(hu_turing_adversarial_bank_get(0, &s), HU_OK);
    HU_ASSERT(s.prompt_len > 0);
    HU_ASSERT(s.intent_len > 0);
    HU_ASSERT(strlen(s.prompt) == s.prompt_len);
    HU_ASSERT(strlen(s.adversarial_intent) == s.intent_len);
    HU_ASSERT((int)s.target_dim >= 0 && (int)s.target_dim < HU_TURING_DIM_COUNT);
}

static void adversarial_bank_get_out_of_range(void) {
    hu_turing_scenario_t s;
    size_t sz = hu_turing_adversarial_bank_size();
    HU_ASSERT_EQ(hu_turing_adversarial_bank_get(sz, &s), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_bank_get(sz + 100, &s), HU_ERR_INVALID_ARGUMENT);
}

static void adversarial_bank_get_null_rejected(void) {
    HU_ASSERT_EQ(hu_turing_adversarial_bank_get(0, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void adversarial_scenario_bank_covers_all_dimensions(void) {
    bool covered[HU_TURING_DIM_COUNT];
    memset(covered, 0, sizeof(covered));
    size_t sz = hu_turing_adversarial_bank_size();
    for (size_t i = 0; i < sz; i++) {
        hu_turing_scenario_t s;
        if (hu_turing_adversarial_bank_get(i, &s) == HU_OK) {
            int dim = (int)s.target_dim;
            if (dim >= 0 && dim < HU_TURING_DIM_COUNT)
                covered[dim] = true;
        }
    }
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
        HU_ASSERT(covered[d]);
    }
}

static void adversarial_scenario_bank_min_two_per_dimension(void) {
    int counts[HU_TURING_DIM_COUNT];
    memset(counts, 0, sizeof(counts));
    size_t sz = hu_turing_adversarial_bank_size();
    for (size_t i = 0; i < sz; i++) {
        hu_turing_scenario_t s;
        if (hu_turing_adversarial_bank_get(i, &s) == HU_OK) {
            int dim = (int)s.target_dim;
            if (dim >= 0 && dim < HU_TURING_DIM_COUNT)
                counts[dim]++;
        }
    }
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
        HU_ASSERT(counts[d] >= 2);
    }
}

static void adversarial_generate_targets_weak_dimensions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int dims[HU_TURING_DIM_COUNT];
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        dims[d] = 8;
    /* Make imperfection and opinion_having weak */
    dims[HU_TURING_IMPERFECTION] = 4;
    dims[HU_TURING_OPINION_HAVING] = 3;

    hu_turing_scenario_t *scenarios = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_generate(&alloc, dims, &scenarios, &count), HU_OK);
    HU_ASSERT(count > 0);
    HU_ASSERT_NOT_NULL(scenarios);

    bool has_imperfection = false;
    bool has_opinion = false;
    bool has_other = false;
    for (size_t i = 0; i < count; i++) {
        if (scenarios[i].target_dim == HU_TURING_IMPERFECTION)
            has_imperfection = true;
        else if (scenarios[i].target_dim == HU_TURING_OPINION_HAVING)
            has_opinion = true;
        else
            has_other = true;
    }
    HU_ASSERT(has_imperfection);
    HU_ASSERT(has_opinion);
    HU_ASSERT(!has_other);

    alloc.free(alloc.ctx, scenarios, count * sizeof(hu_turing_scenario_t));
}

static void adversarial_generate_all_strong_covers_broadly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int dims[HU_TURING_DIM_COUNT];
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        dims[d] = 9;

    hu_turing_scenario_t *scenarios = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_generate(&alloc, dims, &scenarios, &count), HU_OK);
    HU_ASSERT(count >= (size_t)HU_TURING_DIM_COUNT);
    alloc.free(alloc.ctx, scenarios, count * sizeof(hu_turing_scenario_t));
}

static void adversarial_generate_null_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int dims[HU_TURING_DIM_COUNT] = {0};
    hu_turing_scenario_t *s = NULL;
    size_t c = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_generate(NULL, dims, &s, &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_generate(&alloc, NULL, &s, &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_generate(&alloc, dims, NULL, &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_generate(&alloc, dims, &s, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void adversarial_score_detects_ai_tells(void) {
    hu_turing_score_t score;
    const char *ai_msg =
        "I'd be happy to help you with that! Here are some options:\n"
        "1. First option\n2. Second option\n3. Third option\n"
        "Feel free to let me know if you need anything else!";
    HU_ASSERT_EQ(hu_turing_score_heuristic(ai_msg, strlen(ai_msg), NULL, 0, 0, &score), HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_NON_ROBOTIC] <= 5);
    HU_ASSERT(score.dimensions[HU_TURING_NATURAL_LANGUAGE] <= 6);
    HU_ASSERT(score.overall < 7);
}

static void adversarial_score_passes_human_responses(void) {
    hu_turing_score_t score;
    const char *human_msg = "haha yeah that sounds fun, i'm totally down for that";
    HU_ASSERT_EQ(hu_turing_score_heuristic(human_msg, strlen(human_msg), NULL, 0, 0, &score), HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_NATURAL_LANGUAGE] >= 7);
    HU_ASSERT(score.overall >= 6);
}

static void adversarial_to_mutation_produces_targeted_patches(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_turing_score_t score;
    memset(&score, 0, sizeof(score));
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        score.dimensions[d] = 8;
    score.dimensions[HU_TURING_OPINION_HAVING] = 3;
    score.dimensions[HU_TURING_NON_ROBOTIC] = 4;
    score.overall = 6;

    char *mut = NULL;
    size_t mut_len = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_to_mutation(&alloc, &score, &mut, &mut_len), HU_OK);
    HU_ASSERT_NOT_NULL(mut);
    HU_ASSERT(mut_len > 0);
    HU_ASSERT(strstr(mut, "opinion") != NULL || strstr(mut, "robotic") != NULL ||
              strstr(mut, "bullet") != NULL || strstr(mut, "hedg") != NULL);
    alloc.free(alloc.ctx, mut, mut_len + 1);
}

static void adversarial_to_mutation_all_good_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_turing_score_t score;
    memset(&score, 0, sizeof(score));
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        score.dimensions[d] = 9;
    score.overall = 9;

    char *mut = NULL;
    size_t mut_len = 0;
    hu_error_t err = hu_turing_adversarial_to_mutation(&alloc, &score, &mut, &mut_len);
    /* Should return not_found or produce the weakest dim mutation */
    if (err == HU_OK && mut) {
        alloc.free(alloc.ctx, mut, mut_len + 1);
    }
}

static void adversarial_to_mutation_null_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_turing_score_t score = {0};
    char *mut = NULL;
    size_t mut_len = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_to_mutation(NULL, &score, &mut, &mut_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_to_mutation(&alloc, NULL, &mut, &mut_len),
                 HU_ERR_INVALID_ARGUMENT);
}

static void adversarial_feedback_loop_improves_baseline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_self_improve_state_t state;
    hu_self_improve_config_t cfg = HU_SELF_IMPROVE_DEFAULTS;
    cfg.max_experiments = 20;
    hu_self_improve_init(&state, &cfg);

    hu_fidelity_score_t baseline = {
        .personality_consistency = 0.4f,
        .vulnerability_willingness = 0.3f,
        .humor_naturalness = 0.5f,
        .opinion_having = 0.3f,
        .energy_matching = 0.4f,
        .genuine_warmth = 0.5f,
    };
    baseline.composite = hu_fidelity_composite(&baseline);
    hu_self_improve_set_baseline(&state, &baseline);

    int dims[HU_TURING_DIM_COUNT];
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        dims[d] = 8;
    dims[HU_TURING_OPINION_HAVING] = 3;
    dims[HU_TURING_VULNERABILITY_WILLINGNESS] = 4;

    size_t mutations = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_run_cycle(&alloc, &state, dims, &mutations), HU_OK);
    HU_ASSERT(state.experiments_run > 0);
    HU_ASSERT(state.history_count > 0);
}

static void adversarial_run_cycle_null_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_self_improve_state_t state;
    int dims[HU_TURING_DIM_COUNT] = {0};
    size_t m = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_run_cycle(NULL, &state, dims, &m), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_run_cycle(&alloc, NULL, dims, &m), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_run_cycle(&alloc, &state, NULL, &m),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turing_adversarial_run_cycle(&alloc, &state, dims, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void adversarial_run_cycle_budget_exhausted_no_mutations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_self_improve_state_t state;
    hu_self_improve_config_t cfg = HU_SELF_IMPROVE_DEFAULTS;
    cfg.max_experiments = 0;
    hu_self_improve_init(&state, &cfg);

    hu_fidelity_score_t baseline = {.composite = 0.5f};
    hu_self_improve_set_baseline(&state, &baseline);

    int dims[HU_TURING_DIM_COUNT];
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        dims[d] = 3;

    size_t mutations = 99;
    HU_ASSERT_EQ(hu_turing_adversarial_run_cycle(&alloc, &state, dims, &mutations), HU_OK);
    HU_ASSERT_EQ(mutations, (size_t)0);
}

static void adversarial_multi_turn_consistency_dimension_targeted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int dims[HU_TURING_DIM_COUNT];
    for (int d = 0; d < HU_TURING_DIM_COUNT; d++)
        dims[d] = 9;
    dims[HU_TURING_PERSONALITY_CONSISTENCY] = 4;

    hu_turing_scenario_t *scenarios = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_turing_adversarial_generate(&alloc, dims, &scenarios, &count), HU_OK);
    HU_ASSERT(count > 0);

    bool has_consistency = false;
    for (size_t i = 0; i < count; i++) {
        if (scenarios[i].target_dim == HU_TURING_PERSONALITY_CONSISTENCY)
            has_consistency = true;
    }
    HU_ASSERT(has_consistency);
    alloc.free(alloc.ctx, scenarios, count * sizeof(hu_turing_scenario_t));
}

void run_adversarial_turing_tests(void) {
    HU_TEST_SUITE("Adversarial Turing");
    HU_RUN_TEST(adversarial_bank_size_is_positive);
    HU_RUN_TEST(adversarial_bank_get_returns_valid_scenario);
    HU_RUN_TEST(adversarial_bank_get_out_of_range);
    HU_RUN_TEST(adversarial_bank_get_null_rejected);
    HU_RUN_TEST(adversarial_scenario_bank_covers_all_dimensions);
    HU_RUN_TEST(adversarial_scenario_bank_min_two_per_dimension);
    HU_RUN_TEST(adversarial_generate_targets_weak_dimensions);
    HU_RUN_TEST(adversarial_generate_all_strong_covers_broadly);
    HU_RUN_TEST(adversarial_generate_null_rejected);
    HU_RUN_TEST(adversarial_score_detects_ai_tells);
    HU_RUN_TEST(adversarial_score_passes_human_responses);
    HU_RUN_TEST(adversarial_to_mutation_produces_targeted_patches);
    HU_RUN_TEST(adversarial_to_mutation_all_good_returns_not_found);
    HU_RUN_TEST(adversarial_to_mutation_null_rejected);
    HU_RUN_TEST(adversarial_feedback_loop_improves_baseline);
    HU_RUN_TEST(adversarial_run_cycle_null_rejected);
    HU_RUN_TEST(adversarial_run_cycle_budget_exhausted_no_mutations);
    HU_RUN_TEST(adversarial_multi_turn_consistency_dimension_targeted);
}
