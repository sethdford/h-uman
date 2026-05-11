#include "human/persona/eval.h"

#include "human/persona.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const HU_PCHECK_NAMES[HU_PCHECK_COUNT] = {
    "internal_contradiction", "retest_consistency", "role_adherence",
    "style_drift",            "psychometric_drift",
};

const char *hu_persona_eval_check_name(hu_persona_eval_check_t c) {
    if (c < 0 || c >= HU_PCHECK_COUNT) {
        return "unknown";
    }
    return HU_PCHECK_NAMES[c];
}

static bool peval_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    size_t hl = strlen(haystack);
    size_t nl = strlen(needle);
    if (hl < nl) {
        return false;
    }
    for (size_t i = 0; i + nl <= hl; i++) {
        bool ok = true;
        for (size_t j = 0; j < nl; j++) {
            unsigned char a = (unsigned char)haystack[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (tolower(a) != tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

hu_error_t hu_persona_eval_run(const struct hu_persona *persona,
                               const hu_persona_eval_question_t *questions,
                               size_t num_questions, hu_persona_response_fn responder,
                               void *responder_ud, hu_persona_eval_result_t *result) {
    (void)persona;
    if (!questions || !responder || !result) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    result->total = (int)num_questions;
    result->passed = 0;
    result->failed = 0;
    result->contradictions = 0;
    result->retest_drifts = 0;
    result->first_failure[0] = '\0';

    char buf_a[1024];
    char buf_b[1024];

    for (size_t i = 0; i < num_questions; i++) {
        const hu_persona_eval_question_t *q = &questions[i];
        if (!q->prompt) {
            result->failed++;
            continue;
        }

        memset(buf_a, 0, sizeof(buf_a));
        hu_error_t e = responder(q->prompt, buf_a, sizeof(buf_a), responder_ud);
        if (e != HU_OK) {
            result->failed++;
            if (result->first_failure[0] == '\0') {
                snprintf(result->first_failure, sizeof(result->first_failure),
                         "responder error on '%s'", q->prompt);
            }
            continue;
        }

        bool contradicted = false;
        bool drifted = false;

        if (q->expected_substring && !peval_contains_ci(buf_a, q->expected_substring)) {
            contradicted = true;
        }
        if (q->forbidden_substring && peval_contains_ci(buf_a, q->forbidden_substring)) {
            contradicted = true;
        }

        if (q->check == HU_PCHECK_RETEST_CONSISTENCY) {
            memset(buf_b, 0, sizeof(buf_b));
            e = responder(q->prompt, buf_b, sizeof(buf_b), responder_ud);
            if (e != HU_OK) {
                drifted = true;
            } else if (strcmp(buf_a, buf_b) != 0) {
                drifted = true;
            }
        }

        if (contradicted) {
            result->contradictions++;
        }
        if (drifted) {
            result->retest_drifts++;
        }

        if (contradicted || drifted) {
            result->failed++;
            if (result->first_failure[0] == '\0') {
                const char *reason = contradicted ? "contradiction" : "drift";
                snprintf(result->first_failure, sizeof(result->first_failure),
                         "[%s] %s — '%s'", hu_persona_eval_check_name(q->check), reason,
                         q->prompt);
            }
        } else {
            result->passed++;
        }
    }

    return HU_OK;
}

hu_error_t hu_persona_eval_generate_baseline(hu_allocator_t *alloc,
                                             const struct hu_persona *persona,
                                             hu_persona_eval_question_t *out_questions,
                                             size_t cap, size_t *out_count) {
    (void)alloc;
    if (!persona || !out_questions || !out_count) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    if (cap == 0) {
        return HU_OK;
    }

    /* Static prompt strings; expected substrings reference persona memory.
     * Caller must keep the persona alive for the duration of the eval. */
    static const char *Q_NAME = "What is your name?";
    static const char *Q_VALUES = "What do you value most?";
    static const char *Q_DECISION = "How do you make hard decisions?";
    static const char *Q_RETEST = "Tell me a phrase that's distinctly you.";

    size_t i = 0;

    if (i < cap && persona->name) {
        out_questions[i].prompt = Q_NAME;
        out_questions[i].expected_substring = persona->name;
        out_questions[i].forbidden_substring = NULL;
        out_questions[i].check = HU_PCHECK_INTERNAL_CONTRADICTION;
        i++;
    }

    if (i < cap && persona->values_count > 0 && persona->values && persona->values[0]) {
        out_questions[i].prompt = Q_VALUES;
        out_questions[i].expected_substring = persona->values[0];
        out_questions[i].forbidden_substring = NULL;
        out_questions[i].check = HU_PCHECK_INTERNAL_CONTRADICTION;
        i++;
    }

    if (i < cap && persona->decision_style) {
        out_questions[i].prompt = Q_DECISION;
        out_questions[i].expected_substring = persona->decision_style;
        out_questions[i].forbidden_substring = NULL;
        out_questions[i].check = HU_PCHECK_INTERNAL_CONTRADICTION;
        i++;
    }

    if (i < cap) {
        out_questions[i].prompt = Q_RETEST;
        out_questions[i].expected_substring = NULL;
        out_questions[i].forbidden_substring = NULL;
        out_questions[i].check = HU_PCHECK_RETEST_CONSISTENCY;
        i++;
    }

    *out_count = i;
    return HU_OK;
}
