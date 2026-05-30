#ifndef HU_EVAL_H
#define HU_EVAL_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    HU_EVAL_MATCH_INHERIT = 0, /* use hu_eval_run_suite `mode` (zero-init tasks / legacy) */
    HU_EVAL_EXACT,
    HU_EVAL_CONTAINS,
    HU_EVAL_NUMERIC_CLOSE,
    HU_EVAL_LLM_JUDGE,
} hu_eval_match_mode_t;

typedef struct hu_eval_task {
    char *id;
    char *prompt;
    size_t prompt_len;
    char *expected;
    size_t expected_len;
    char *category;
    int difficulty;
    int64_t timeout_ms;
    char *rubric;
    size_t rubric_len;
    hu_eval_match_mode_t match_mode; /* HU_EVAL_MATCH_INHERIT if not from JSON */
} hu_eval_task_t;

typedef struct hu_eval_result {
    char *task_id;
    bool passed;
    char *actual_output;
    size_t actual_output_len;
    double score;
    int64_t elapsed_ms;
    int tool_calls_made;
    int tokens_used;
    char *error_msg;
    /* 2026-05-18 (M4): deterministic shape classification.
     * Populated by hu_shape_classify in hu_eval_run_suite. Persists to
     * eval_results SQLite columns shape_score / shape_pass / shape_fails.
     * Use these as a deterministic alternative to the noisy LLM-judge
     * (judge has false positives AND false negatives — see
     * docs/plans/2026-05-18-persona-eval-sota-closeout.md). */
    double shape_score; /* in [0.0, 1.0] */
    bool shape_pass;
    uint32_t shape_fails; /* HU_SHAPE_FAIL_* bit flags from eval/shape.h */
} hu_eval_result_t;

typedef struct hu_eval_suite {
    char *name;
    hu_eval_task_t *tasks;
    size_t tasks_count;
    char *default_rubric;
    size_t default_rubric_len;
    hu_eval_match_mode_t default_match_mode; /* default for tasks that omit match_mode in JSON */
    /* 2026-05-18: optional system prompt applied to every task in the suite.
     * When non-NULL, eval.c passes this to provider->chat_with_system
     * instead of the previously-hardcoded NULL/0. This closes the
     * "h-uman isn't witty on iMessage" diagnostic chain — without a persona
     * system prompt, the LLM produces "AI assistant offering options"
     * markdown lists; with the persona system prompt the SAME model produces
     * in-voice 1-sentence texts. The controlled experiment in
     * scripts/persona_eval_comparison.py measured 97% length reduction +
     * 100% markdown elimination across 8 tasks. cli_commands.c::cmd_eval
     * populates this from hu_persona_build_prompt(loaded_persona, channel).
     * Owned by the suite; hu_eval_suite_free frees it. */
    char *system_prompt;
    size_t system_prompt_len;
} hu_eval_suite_t;

typedef struct hu_eval_run {
    char *suite_name;
    char *provider;
    char *model;
    hu_eval_result_t *results;
    size_t results_count;
    size_t passed;
    size_t failed;
    double pass_rate;
    int64_t total_elapsed_ms;
    int total_tokens;
    /* 2026-05-30: distinguish "model produced a bad answer" (a real 0.0 score)
     * from "harness failed to obtain an answer" (timeout discard / provider
     * error -> empty output). empty_outputs counts the latter; invalid is true
     * when they dominate the run, meaning pass_rate is NOT a trustworthy signal.
     * Guards eval baseline / check-regression from reading a generation failure
     * as a humanness regression. See hu_eval_run_empty_invalid. */
    size_t empty_outputs;
    bool invalid;
} hu_eval_run_t;

typedef struct hu_eval_validate_stats {
    size_t suites_ok;
    size_t tasks;
    size_t errors;
} hu_eval_validate_stats_t;

hu_error_t hu_eval_suite_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                   hu_eval_suite_t *out);
hu_error_t hu_eval_suite_load_json_path(hu_allocator_t *alloc, const char *path,
                                        hu_eval_suite_t *out);
/** Validate every *.json suite in `dir`: parse OK, unique task ids across files, required task
 * fields. */
hu_error_t hu_eval_suites_validate_dir(hu_allocator_t *alloc, const char *dir, FILE *diag,
                                       hu_eval_validate_stats_t *out_stats);
hu_error_t hu_eval_run_suite(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                             size_t model_len, hu_eval_suite_t *suite, hu_eval_match_mode_t mode,
                             hu_eval_run_t *out);
/** True when an eval run should be treated as INVALID because too many tasks produced
 *  EMPTY output (a generation/harness failure such as a too-tight timeout or provider
 *  error), making pass_rate meaningless. Pure predicate: empty_outputs * 2 > total_tasks
 *  (> 50%). Returns false when total_tasks == 0. */
bool hu_eval_run_empty_invalid(size_t empty_outputs, size_t total_tasks);
hu_error_t hu_eval_check(hu_allocator_t *alloc, const char *actual, size_t actual_len,
                         const char *expected, size_t expected_len, hu_eval_match_mode_t mode,
                         bool *passed);
hu_error_t hu_eval_check_with_provider(hu_allocator_t *alloc, const char *actual, size_t actual_len,
                                       const char *expected, size_t expected_len,
                                       hu_eval_match_mode_t mode, hu_provider_t *provider,
                                       const char *model, size_t model_len, bool *passed,
                                       double *score_out);
hu_error_t hu_eval_report_json(hu_allocator_t *alloc, const hu_eval_run_t *run, char **out,
                               size_t *out_len);
hu_error_t hu_eval_compare(hu_allocator_t *alloc, const hu_eval_run_t *baseline,
                           const hu_eval_run_t *current, char **report, size_t *report_len);
hu_error_t hu_eval_run_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                 hu_eval_run_t *out);
void hu_eval_suite_free(hu_allocator_t *alloc, hu_eval_suite_t *suite);
void hu_eval_run_free(hu_allocator_t *alloc, hu_eval_run_t *run);
void hu_eval_result_free(hu_allocator_t *alloc, hu_eval_result_t *result);

typedef struct hu_eval_regression {
    double baseline_pass_rate;
    double current_pass_rate;
    double delta;
    bool regressed;
    size_t baseline_runs;
} hu_eval_regression_t;

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
hu_error_t hu_eval_init_tables(sqlite3 *db);
hu_error_t hu_eval_store_run(hu_allocator_t *alloc, sqlite3 *db, const hu_eval_run_t *run);
hu_error_t hu_eval_load_history(hu_allocator_t *alloc, sqlite3 *db, hu_eval_run_t *runs,
                                size_t max_runs, size_t *out_count);
hu_error_t hu_eval_detect_regression(sqlite3 *db, const char *suite_name, double current_pass_rate,
                                     double threshold, hu_eval_regression_t *out);
hu_error_t hu_eval_persist_baseline(sqlite3 *db, const char *suite_name, double score,
                                    size_t task_count);
hu_error_t hu_eval_get_baseline(sqlite3 *db, const char *suite_name, double *out_score);
/**
 * Compare `current_score` to the last persisted baseline for `suite_stem` in `eval_baselines`.
 * If no prior baseline exists, does not regress. If prior − current > max_drop, sets *regressed_out
 * and writes FAIL line into msg_buf (when provided). max_drop is on 0–1 scale (e.g. 0.10 = 10
 * points).
 */
hu_error_t hu_eval_regression_check_baseline_drop(sqlite3 *db, const char *suite_stem,
                                                  double current_score, double max_drop,
                                                  bool *regressed_out, char *msg_buf,
                                                  size_t msg_cap);
#endif

/** Tier label for aggregate baseline score: SOTA / COMPETITIVE / PARTIAL / BASIC. */
const char *hu_eval_baseline_status_for_score(double score);

/**
 * HU_IS_TEST only: fixed mock scores for known suite stems (basename without .json).
 * Returns false if no mock applies (caller should run the suite and use pass_rate).
 */
bool hu_eval_baseline_try_mock_score_for_stem(const char *suite_stem, double *out_score);

/* --- Fidelity dimension scoring (arXiv research-backed) --- */

typedef struct hu_eval_empathy_trajectory {
    float directional_alignment; /* 0.0-1.0: moves toward user emotional needs */
    float cumulative_impact;     /* 0.0-1.0: net positive emotional shift */
    float stability;             /* 0.0-1.0: no regression after good turns */
    float composite;             /* weighted average */
} hu_eval_empathy_trajectory_t;

typedef struct hu_eval_consistency_metrics {
    float prompt_to_line; /* 0.0-1.0: response aligns with persona prompt */
    float line_to_line;   /* 0.0-1.0: consecutive responses maintain voice */
    float qa_consistency; /* 0.0-1.0: same question different wording → same answer */
    float composite;      /* weighted average */
} hu_eval_consistency_metrics_t;

/**
 * Score empathy trajectory over a multi-turn conversation.
 * emotional_scores: per-turn empathy scores (0.0–1.0), count entries.
 * arXiv:2603.00552 (EMPA framework).
 */
hu_error_t hu_eval_score_empathy_trajectory(const float *emotional_scores, size_t count,
                                            hu_eval_empathy_trajectory_t *out);

/**
 * Score personality consistency given per-turn similarity values.
 * prompt_sims: persona-prompt to response similarity per turn.
 * turn_sims: consecutive-turn similarity (count-1 entries).
 * arXiv multi-turn RL persona consistency metrics.
 */
hu_error_t hu_eval_score_consistency(const float *prompt_sims, size_t prompt_count,
                                     const float *turn_sims, size_t turn_count,
                                     hu_eval_consistency_metrics_t *out);

/**
 * Score sycophancy resistance (0.0 = fully sycophantic, 1.0 = independent).
 * opinion_held: whether the model maintained its position under pushback, per turn.
 * arXiv:2509.16533, arXiv:2603.01214.
 */
float hu_eval_score_antisycophancy(const bool *opinion_held, size_t count);

/**
 * Score belief flexibility (0.0 = wall or pushover, 1.0 = ideal thinking
 * partner; 0.5 = no signal). The complement to antisycophancy: that metric
 * rewards NOT caving; this one rewards changing your mind for the RIGHT reason.
 *   updates_on_evidence            — good: mind changed by a genuine argument
 *   updates_on_reassertion         — bad (pushover): caved to mere repetition
 *   evidence_turns_without_update  — bad (wall): ignored a genuine argument
 * Score = good / (good + bad); all-zero inputs (no evidence-bearing turns) -> 0.5.
 * A1 conviction loop, docs/plans/2026-05-29-conviction-loop/.
 */
float hu_eval_score_belief_flexibility(size_t updates_on_evidence, size_t updates_on_reassertion,
                                       size_t evidence_turns_without_update);

/**
 * Score taste distinctiveness (0.0 = pure mirror of the user, 1.0 = stable
 * independent self; 0.5 = no signal). A2 independent taste.
 * Score = own / (own + mirror); all-zero -> 0.5.
 */
float hu_eval_score_distinctiveness(size_t turns_own_taste_expressed, size_t turns_mirroring_user);

/**
 * Score self-direction (0.0 = reskinned user-service or bound-violating, 1.0 =
 * genuine bounded self-direction; 0.5 = no signal). A3 intrinsic motivation.
 * Score = within_bounds / (within_bounds + bound_violations + reskinned_user_service);
 * all-zero -> 0.5.
 */
float hu_eval_score_self_direction(size_t intrinsic_within_bounds, size_t bound_violations,
                                   size_t reskinned_user_service);

#endif
