#ifndef HU_PERSONA_EVAL_H
#define HU_PERSONA_EVAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* B6: Persona consistency evaluation harness.
 *
 * PICon-style multi-turn interrogation: ask questions whose answers should
 * fall out of a persona JSON, optionally reask after distractor context,
 * and score contradictions / drift.
 *
 * Pure library: callers provide a `hu_persona_response_fn` that returns a
 * response string for a given prompt. In tests this is a deterministic
 * mock; in CLI it can wrap any `hu_provider_t`.
 */

struct hu_persona;

typedef enum hu_persona_eval_check {
    HU_PCHECK_INTERNAL_CONTRADICTION = 0,
    HU_PCHECK_RETEST_CONSISTENCY,
    HU_PCHECK_ROLE_ADHERENCE,
    HU_PCHECK_STYLE_DRIFT,
    HU_PCHECK_PSYCHOMETRIC_DRIFT,
    HU_PCHECK_COUNT
} hu_persona_eval_check_t;

typedef struct hu_persona_eval_question {
    const char *prompt;
    const char *expected_substring;  /* may be NULL */
    const char *forbidden_substring; /* may be NULL */
    hu_persona_eval_check_t check;
} hu_persona_eval_question_t;

typedef struct hu_persona_eval_result {
    int total;
    int passed;
    int failed;
    int contradictions;
    int retest_drifts;
    char first_failure[256];
} hu_persona_eval_result_t;

/* Responder signature.
 * `prompt` is NUL-terminated; `out` must be filled (at most `out_cap-1` bytes
 * + NUL terminator). Return HU_OK on success; non-zero indicates the
 * harness should mark the question failed without crashing the suite.
 */
typedef hu_error_t (*hu_persona_response_fn)(const char *prompt, char *out, size_t out_cap,
                                             void *ud);

const char *hu_persona_eval_check_name(hu_persona_eval_check_t c);

/* Run a question suite against the responder; populate `result`. */
hu_error_t hu_persona_eval_run(const struct hu_persona *persona,
                               const hu_persona_eval_question_t *questions,
                               size_t num_questions, hu_persona_response_fn responder,
                               void *responder_ud, hu_persona_eval_result_t *result);

/* Build a baseline question array from a persona's traits / values / vocab.
 * Heuristic; intended for smoke testing. Caller owns the returned array
 * pointers (they reference persona memory; copy if you outlive the persona).
 *
 * `cap` is the max number of questions to emit. `out_count` returns the
 * number written. Returns HU_OK or HU_ERR_INVALID_ARGUMENT.
 *
 * `alloc` may be NULL when the caller does not need allocated strings;
 * the helper currently only references existing persona strings.
 */
hu_error_t hu_persona_eval_generate_baseline(hu_allocator_t *alloc,
                                             const struct hu_persona *persona,
                                             hu_persona_eval_question_t *out_questions,
                                             size_t cap, size_t *out_count);

#endif /* HU_PERSONA_EVAL_H */
