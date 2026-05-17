#include "human/agent/process_reward.h"
#include <math.h>
#include <string.h>

hu_prm_config_t hu_prm_config_default(void) {
    hu_prm_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.provider = NULL;
    cfg.model = NULL;
    cfg.model_len = 0;
    cfg.correctness_threshold = 0.5;
    cfg.retry_threshold = 0.35;
    return cfg;
}

static bool prm_contains(const char *text, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen > len)
        return false;
    for (size_t i = 0; i <= len - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = text[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static bool has_digit(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (text[i] >= '0' && text[i] <= '9')
            return true;
    }
    return false;
}

static bool has_code_pattern(const char *text, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if ((text[i] == '(' && text[i + 1] == ')') ||
            (text[i] == '{' && text[i + 1] == '}') ||
            text[i] == '=')
            return true;
    }
    return false;
}

static bool has_citation(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '[' || text[i] == ']')
            return true;
    }
    return prm_contains(text, len, "http");
}

static double score_step_heuristic(const char *step, size_t step_len) {
    double score = 0.5;

    static const char *hedges[] = {"maybe", "perhaps", "i think", "not sure", "might"};
    for (size_t i = 0; i < sizeof(hedges) / sizeof(hedges[0]); i++) {
        if (prm_contains(step, step_len, hedges[i]))
            score -= 0.08;
    }

    if (has_digit(step, step_len))
        score += 0.05;
    if (has_code_pattern(step, step_len))
        score += 0.05;
    if (has_citation(step, step_len))
        score += 0.03;

    static const char *connectors[] = {"therefore", "because", "since", "thus", "hence", "consequently"};
    for (size_t i = 0; i < sizeof(connectors) / sizeof(connectors[0]); i++) {
        if (prm_contains(step, step_len, connectors[i])) {
            score += 0.06;
            break;
        }
    }

    if (score < 0.0)
        score = 0.0;
    if (score > 1.0)
        score = 1.0;
    return score;
}

static size_t split_steps(const char *text, size_t text_len,
                          const char **starts, size_t *lengths, size_t max_steps) {
    size_t count = 0;

    const char *p = text;
    const char *end = text + text_len;
    const char *seg_start = text;
    bool found_split = false;

    while (p + 1 < end) {
        if (p[0] == '\n' && p[1] == '\n') {
            found_split = true;
            size_t seg_len = (size_t)(p - seg_start);
            while (seg_len > 0 && (seg_start[0] == ' ' || seg_start[0] == '\n')) {
                seg_start++;
                seg_len--;
            }
            while (seg_len > 0 && (seg_start[seg_len - 1] == ' ' || seg_start[seg_len - 1] == '\n'))
                seg_len--;
            if (seg_len > 0 && count < max_steps) {
                starts[count] = seg_start;
                lengths[count] = seg_len;
                count++;
            }
            p += 2;
            while (p < end && (*p == '\n' || *p == ' '))
                p++;
            seg_start = p;
            continue;
        }
        p++;
    }

    if (found_split) {
        size_t seg_len = (size_t)(end - seg_start);
        while (seg_len > 0 && (seg_start[0] == ' ' || seg_start[0] == '\n')) {
            seg_start++;
            seg_len--;
        }
        while (seg_len > 0 && (seg_start[seg_len - 1] == ' ' || seg_start[seg_len - 1] == '\n'))
            seg_len--;
        if (seg_len > 0 && count < max_steps) {
            starts[count] = seg_start;
            lengths[count] = seg_len;
            count++;
        }
        return count;
    }

    p = text;
    seg_start = text;
    while (p < end) {
        if (*p == '\n' && p + 1 < end) {
            char next = p[1];
            bool is_list = (next >= '0' && next <= '9') || next == '-' || next == '*'
                || (next >= 'A' && next <= 'Z');
            if (is_list) {
                size_t seg_len = (size_t)(p - seg_start);
                while (seg_len > 0 && (seg_start[0] == ' ' || seg_start[0] == '\n')) {
                    seg_start++;
                    seg_len--;
                }
                while (seg_len > 0 && (seg_start[seg_len - 1] == ' ' || seg_start[seg_len - 1] == '\n'))
                    seg_len--;
                if (seg_len > 0 && count < max_steps) {
                    starts[count] = seg_start;
                    lengths[count] = seg_len;
                    count++;
                    found_split = true;
                }
                seg_start = p + 1;
            }
        }
        p++;
    }
    if (found_split) {
        size_t seg_len = (size_t)(end - seg_start);
        while (seg_len > 0 && (seg_start[0] == ' ' || seg_start[0] == '\n')) {
            seg_start++;
            seg_len--;
        }
        while (seg_len > 0 && (seg_start[seg_len - 1] == ' ' || seg_start[seg_len - 1] == '\n'))
            seg_len--;
        if (seg_len > 0 && count < max_steps) {
            starts[count] = seg_start;
            lengths[count] = seg_len;
            count++;
        }
        return count;
    }

    starts[0] = text;
    lengths[0] = text_len;
    return 1;
}

hu_error_t hu_prm_score_chain(hu_allocator_t *alloc, const hu_prm_config_t *config,
                              const char *reasoning, size_t reasoning_len,
                              hu_prm_result_t *result) {
    if (!alloc || !config || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    if (!reasoning || reasoning_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *step_starts[64];
    size_t step_lengths[64];
    size_t n = split_steps(reasoning, reasoning_len, step_starts, step_lengths, 64);

    hu_prm_step_t *steps = (hu_prm_step_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_prm_step_t));
    if (!steps)
        return HU_ERR_OUT_OF_MEMORY;

    double product = 1.0;
    bool all_valid = true;

    for (size_t i = 0; i < n; i++) {
        steps[i].text = step_starts[i];
        steps[i].text_len = step_lengths[i];
        steps[i].score = score_step_heuristic(step_starts[i], step_lengths[i]);
        steps[i].is_correct = steps[i].score >= config->correctness_threshold;
        product *= steps[i].score;
        if (!steps[i].is_correct)
            all_valid = false;
    }

    result->steps = steps;
    result->step_count = n;
    result->aggregate_score = pow(product, 1.0 / (double)n);
    result->chain_valid = all_valid;
    return HU_OK;
}

hu_error_t hu_prm_score_step(hu_allocator_t *alloc, const hu_prm_config_t *config,
                             const char *step, size_t step_len,
                             const char *context, size_t context_len,
                             double *score) {
    (void)context;
    (void)context_len;
    if (!alloc || !config || !step || step_len == 0 || !score)
        return HU_ERR_INVALID_ARGUMENT;

    *score = score_step_heuristic(step, step_len);
    return HU_OK;
}

void hu_prm_result_free(hu_allocator_t *alloc, hu_prm_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->steps) {
        alloc->free(alloc->ctx, result->steps,
                    result->step_count * sizeof(hu_prm_step_t));
        result->steps = NULL;
    }
    result->step_count = 0;
    result->aggregate_score = 0.0;
    result->chain_valid = false;
}

hu_error_t hu_prm_verify_steps(hu_allocator_t *alloc, const hu_prm_config_t *config,
                               const char **steps, const size_t *step_lens,
                               size_t step_count,
                               const char *context, size_t context_len,
                               hu_prm_verify_result_t *result) {
    if (!alloc || !config || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    if (!steps || !step_lens || step_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_prm_step_t *out_steps =
        (hu_prm_step_t *)alloc->alloc(alloc->ctx, step_count * sizeof(hu_prm_step_t));
    if (!out_steps)
        return HU_ERR_OUT_OF_MEMORY;

    double product = 1.0;
    size_t failing = 0;

    for (size_t i = 0; i < step_count; i++) {
        out_steps[i].text = steps[i];
        out_steps[i].text_len = step_lens[i];

        double score = 0.0;
        hu_error_t err = hu_prm_score_step(alloc, config, steps[i], step_lens[i],
                                           context, context_len, &score);
        if (err != HU_OK)
            score = 0.0;

        out_steps[i].score = score;
        out_steps[i].is_correct = score >= config->correctness_threshold;
        product *= score;
        if (!out_steps[i].is_correct)
            failing++;
    }

    result->steps = out_steps;
    result->step_count = step_count;
    result->aggregate_score = pow(product, 1.0 / (double)step_count);
    result->passed = result->aggregate_score >= config->retry_threshold;
    result->failing_step_count = failing;
    return HU_OK;
}

hu_error_t hu_prm_verify_reasoning(hu_allocator_t *alloc, const hu_prm_config_t *config,
                                   const char *reasoning, size_t reasoning_len,
                                   const char *context, size_t context_len,
                                   hu_prm_verify_result_t *result) {
    (void)context; (void)context_len;
    if (!alloc || !config || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    if (!reasoning || reasoning_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_prm_result_t chain_result;
    hu_error_t err = hu_prm_score_chain(alloc, config, reasoning, reasoning_len, &chain_result);
    if (err != HU_OK)
        return err;

    size_t failing = 0;
    for (size_t i = 0; i < chain_result.step_count; i++) {
        if (!chain_result.steps[i].is_correct)
            failing++;
    }

    result->steps = chain_result.steps;
    result->step_count = chain_result.step_count;
    result->aggregate_score = chain_result.aggregate_score;
    result->passed = chain_result.aggregate_score >= config->retry_threshold;
    result->failing_step_count = failing;
    return HU_OK;
}

void hu_prm_verify_result_free(hu_allocator_t *alloc, hu_prm_verify_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->steps) {
        alloc->free(alloc->ctx, result->steps,
                    result->step_count * sizeof(hu_prm_step_t));
        result->steps = NULL;
    }
    result->step_count = 0;
    result->aggregate_score = 0.0;
    result->passed = false;
    result->failing_step_count = 0;
}
