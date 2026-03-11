#include "human/agent/arbitrator.h"
#include "human/core/string.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#define DEFAULT_MAX_TOKENS 1500u
#define DEFAULT_MAX_DIRECTIVES 7u

static double clamp01(double x) { return CLAMP(x, 0.0, 1.0); }

size_t hu_directive_estimate_tokens(const char *content, size_t content_len) {
    (void)content;
    return (content_len / 4) + 1;
}

double hu_directive_compute_priority(uint32_t category, double recency,
                                    double emotional_weight,
                                    double relationship_depth,
                                    double conversation_phase, double novelty) {
    (void)category;
    recency = clamp01(recency);
    emotional_weight = clamp01(emotional_weight);
    relationship_depth = clamp01(relationship_depth);
    conversation_phase = clamp01(conversation_phase);
    novelty = clamp01(novelty);

    double sum = 0.30 * recency + 0.25 * emotional_weight + 0.15 * relationship_depth +
                 0.15 * conversation_phase + 0.15 * novelty;
    return clamp01(sum);
}

static bool source_matches(const hu_directive_t *d, const char *name) {
    if (!d->source || !name)
        return false;
    size_t nlen = strlen(name);
    if (d->source_len != nlen)
        return false;
    return strncmp(d->source, name, d->source_len) == 0;
}

static size_t find_idx(const hu_directive_t *directives, size_t count,
                       const char *source) {
    for (size_t i = 0; i < count; i++) {
        if (source_matches(&directives[i], source))
            return i;
    }
    return count; /* not found */
}

size_t hu_arbitrator_resolve_conflicts(hu_allocator_t *alloc,
                                      hu_directive_t *directives, size_t count,
                                      const hu_directive_conflict_t *conflict_table,
                                      size_t conflict_count) {
    for (size_t c = 0; c < conflict_count; c++) {
        const hu_directive_conflict_t *rule = &conflict_table[c];
        size_t ia = find_idx(directives, count, rule->source_a);
        size_t ib = find_idx(directives, count, rule->source_b);
        if (ia >= count || ib >= count)
            continue;

        size_t remove_idx;
        if (rule->winner) {
            if (source_matches(&directives[ia], rule->winner))
                remove_idx = ib;
            else if (source_matches(&directives[ib], rule->winner))
                remove_idx = ia;
            else
                continue;
        } else {
            remove_idx = (directives[ia].priority >= directives[ib].priority) ? ib : ia;
        }

        hu_directive_deinit(alloc, &directives[remove_idx]);
        memmove(directives + remove_idx, directives + remove_idx + 1,
                (count - remove_idx - 1) * sizeof(hu_directive_t));
        count--;
    }
    return count;
}

/* qsort has no context; use file-scope for comparator */
static const hu_directive_t *g_sort_candidates;

static int compare_priority_desc_qsort(const void *a, const void *b) {
    const size_t *pa = (const size_t *)a;
    const size_t *pb = (const size_t *)b;
    double da = g_sort_candidates[*pa].priority;
    double db = g_sort_candidates[*pb].priority;
    if (db > da)
        return 1;
    if (db < da)
        return -1;
    return 0;
}

hu_error_t hu_arbitrator_select(hu_allocator_t *alloc,
                                const hu_directive_t *candidates,
                                size_t candidate_count,
                                const hu_arbitration_config_t *config,
                                hu_arbitration_result_t *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    if (candidate_count > 0 && !candidates)
        return HU_ERR_INVALID_ARGUMENT;

    size_t max_tokens = config ? config->max_directive_tokens : DEFAULT_MAX_TOKENS;
    size_t max_directives = config ? config->max_directives : DEFAULT_MAX_DIRECTIVES;
    if (max_tokens == 0)
        max_tokens = DEFAULT_MAX_TOKENS;
    if (max_directives == 0)
        max_directives = DEFAULT_MAX_DIRECTIVES;

    memset(result, 0, sizeof(*result));

    if (candidate_count == 0)
        return HU_OK;

    /* First pass: required directives */
    size_t required_tokens = 0;
    size_t required_count = 0;
    for (size_t i = 0; i < candidate_count; i++) {
        if (candidates[i].required) {
            required_tokens += candidates[i].token_cost;
            required_count++;
        }
    }

    /* Build index array for non-required, sorted by priority desc */
    size_t optional_count = candidate_count - required_count;
    size_t *indices = (size_t *)alloc->alloc(alloc->ctx, optional_count * sizeof(size_t));
    if (!indices && optional_count > 0)
        return HU_ERR_OUT_OF_MEMORY;

    size_t idx = 0;
    for (size_t i = 0; i < candidate_count; i++) {
        if (!candidates[i].required)
            indices[idx++] = i;
    }

    size_t selected_count = required_count;
    size_t total_tokens = required_tokens;

    if (optional_count > 0) {
        g_sort_candidates = candidates;
        qsort(indices, optional_count, sizeof(size_t), compare_priority_desc_qsort);
    }

    /* Greedy: add optional by priority until budget or max_directives */
    size_t *selected_indices =
        (size_t *)alloc->alloc(alloc->ctx,
                              (required_count + optional_count) * sizeof(size_t));
    if (!selected_indices) {
        alloc->free(alloc->ctx, indices, optional_count * sizeof(size_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < candidate_count; i++) {
        if (candidates[i].required)
            selected_indices[out_idx++] = i;
    }

    for (size_t i = 0; i < optional_count; i++) {
        size_t k = indices[i];
        size_t cost = candidates[k].token_cost;
        if (total_tokens + cost <= max_tokens && selected_count < max_directives) {
            selected_indices[out_idx++] = k;
            selected_count++;
            total_tokens += cost;
        }
    }

    /* Allocate selected array and deep-copy */
    hu_directive_t *selected =
        (hu_directive_t *)alloc->alloc(alloc->ctx, selected_count * sizeof(hu_directive_t));
    if (!selected) {
        alloc->free(alloc->ctx, indices, optional_count * sizeof(size_t));
        alloc->free(alloc->ctx, selected_indices,
                    (required_count + optional_count) * sizeof(size_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < selected_count; i++) {
        const hu_directive_t *src = &candidates[selected_indices[i]];
        hu_directive_t *dst = &selected[i];
        memset(dst, 0, sizeof(*dst));
        dst->content =
            src->content && src->content_len > 0
                ? hu_strndup(alloc, src->content, src->content_len)
                : NULL;
        dst->content_len = src->content_len;
        dst->source =
            src->source && src->source_len > 0
                ? hu_strndup(alloc, src->source, src->source_len)
                : NULL;
        dst->source_len = src->source_len;
        dst->priority = src->priority;
        dst->token_cost = src->token_cost;
        dst->category = src->category;
        dst->required = src->required;

        if ((src->content && src->content_len > 0 && !dst->content) ||
            (src->source && src->source_len > 0 && !dst->source)) {
            result->selected = selected;
            result->selected_count = i;
            hu_arbitration_result_deinit(alloc, result);
            alloc->free(alloc->ctx, indices, optional_count * sizeof(size_t));
            alloc->free(alloc->ctx, selected_indices,
                       (required_count + optional_count) * sizeof(size_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    alloc->free(alloc->ctx, indices, optional_count * sizeof(size_t));
    alloc->free(alloc->ctx, selected_indices,
                (required_count + optional_count) * sizeof(size_t));

    result->selected = selected;
    result->selected_count = selected_count;
    result->total_tokens = total_tokens;
    result->suppressed_count = candidate_count - selected_count;

    return HU_OK;
}

void hu_directive_deinit(hu_allocator_t *alloc, hu_directive_t *d) {
    if (!alloc || !d)
        return;
    if (d->content) {
        alloc->free(alloc->ctx, d->content, strlen(d->content) + 1);
        d->content = NULL;
    }
    if (d->source) {
        alloc->free(alloc->ctx, d->source, strlen(d->source) + 1);
        d->source = NULL;
    }
    d->content_len = 0;
    d->source_len = 0;
}

void hu_arbitration_result_deinit(hu_allocator_t *alloc,
                                  hu_arbitration_result_t *result) {
    if (!alloc || !result)
        return;
    for (size_t i = 0; i < result->selected_count; i++)
        hu_directive_deinit(alloc, &result->selected[i]);
    if (result->selected) {
        alloc->free(alloc->ctx, result->selected,
                    result->selected_count * sizeof(hu_directive_t));
        result->selected = NULL;
    }
    result->selected_count = 0;
    result->total_tokens = 0;
    result->suppressed_count = 0;
}
