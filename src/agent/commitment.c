/* Commitment detection — promises, intentions, reminders, goals from text */
#include "seaclaw/agent/commitment.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t sc_commitment_id_counter;

static const char *PROMISE_PATTERNS[] = {"I will ", "I'll ", "I promise ", NULL};
static const char *INTENTION_PATTERNS[] = {"I'm going to ", "I am going to ", "I plan to ", NULL};
static const char *REMINDER_PATTERNS[] = {"remind me ", "don't let me forget ", "don't forget to ",
                                         NULL};
static const char *GOAL_PATTERNS[] = {"I want to ", "my goal is ", "I hope to ", NULL};
static const char *NEGATION_PREFIXES[] = {"not ", "n't ", "never ", NULL};

static bool clause_starts_with_negation(const char *text, size_t text_len, size_t clause_start) {
    if (clause_start >= text_len)
        return false;
    for (size_t n = 0; NEGATION_PREFIXES[n]; n++) {
        size_t nlen = strlen(NEGATION_PREFIXES[n]);
        if (clause_start + nlen <= text_len &&
            memcmp(text + clause_start, NEGATION_PREFIXES[n], nlen) == 0)
            return true;
    }
    return false;
}

static size_t extract_clause_end(const char *text, size_t text_len, size_t start) {
    for (size_t i = start; i < text_len; i++) {
        char c = text[i];
        if (c == '.' || c == ',' || c == '\n' || c == '!' || c == '?')
            return i;
    }
    return text_len;
}

static sc_error_t add_commitment(sc_allocator_t *alloc, sc_commitment_detect_result_t *result,
                                 const char *text, size_t text_len, size_t pattern_start,
                                 size_t pattern_len, sc_commitment_type_t type,
                                 const char *role, size_t role_len) {
    if (result->count >= SC_COMMITMENT_DETECT_MAX)
        return SC_OK;

    size_t clause_start = pattern_start + pattern_len;
    while (clause_start < text_len && isspace((unsigned char)text[clause_start]))
        clause_start++;
    if (clause_starts_with_negation(text, text_len, clause_start))
        return SC_OK;
    size_t clause_end = extract_clause_end(text, text_len, clause_start);
    if (clause_end <= clause_start)
        return SC_OK;

    size_t summary_len = clause_end - clause_start;
    char *summary = sc_strndup(alloc, text + clause_start, summary_len);
    if (!summary)
        return SC_ERR_OUT_OF_MEMORY;

    char id_buf[64];
    size_t id_val = ++sc_commitment_id_counter;
    int n = snprintf(id_buf, sizeof(id_buf), "commit-%zu", id_val);
    if (n <= 0 || (size_t)n >= sizeof(id_buf)) {
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return SC_ERR_INVALID_ARGUMENT;
    }
    char *id = sc_strndup(alloc, id_buf, (size_t)n);
    if (!id) {
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }

    const char *created_at = "2026-01-01T00:00:00Z";
    char *created_at_dup = sc_strndup(alloc, created_at, strlen(created_at));
    if (!created_at_dup) {
        alloc->free(alloc->ctx, id, (size_t)n + 1);
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }

    char *owner = role && role_len > 0 ? sc_strndup(alloc, role, role_len) : sc_strndup(alloc, "user", 4);
    if (!owner) {
        alloc->free(alloc->ctx, created_at_dup, strlen(created_at) + 1);
        alloc->free(alloc->ctx, id, (size_t)n + 1);
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }

    char *statement = sc_strndup(alloc, text + pattern_start, clause_end - pattern_start);
    if (!statement) {
        alloc->free(alloc->ctx, owner, (role && role_len > 0 ? role_len : 4) + 1);
        alloc->free(alloc->ctx, created_at_dup, strlen(created_at) + 1);
        alloc->free(alloc->ctx, id, (size_t)n + 1);
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_commitment_t *c = &result->commitments[result->count];
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->statement = statement;
    c->statement_len = clause_end - pattern_start;
    c->summary = summary;
    c->summary_len = summary_len;
    c->type = type;
    c->status = SC_COMMITMENT_ACTIVE;
    c->created_at = created_at_dup;
    c->emotional_weight = NULL;
    c->owner = owner;
    result->count++;
    return SC_OK;
}

static sc_error_t scan_patterns(sc_allocator_t *alloc, const char *text, size_t text_len,
                               const char *role, size_t role_len,
                               sc_commitment_detect_result_t *result,
                               const char *const *patterns, sc_commitment_type_t type) {
    for (size_t p = 0; patterns[p]; p++) {
        size_t plen = strlen(patterns[p]);
        if (plen > text_len)
            continue;
        for (size_t i = 0; i + plen <= text_len; i++) {
            if (memcmp(text + i, patterns[p], plen) != 0)
                continue;
            bool word_boundary = (i == 0) || isspace((unsigned char)text[i - 1]) ||
                                 text[i - 1] == ',' || text[i - 1] == '.';
            if (!word_boundary)
                continue;
            sc_error_t err = add_commitment(alloc, result, text, text_len, i, plen, type, role,
                                            role_len);
            if (err != SC_OK)
                return err;
            if (result->count >= SC_COMMITMENT_DETECT_MAX)
                return SC_OK;
        }
    }
    return SC_OK;
}

sc_error_t sc_commitment_detect(sc_allocator_t *alloc, const char *text, size_t text_len,
                               const char *role, size_t role_len,
                               sc_commitment_detect_result_t *result) {
    if (!alloc || !result)
        return SC_ERR_INVALID_ARGUMENT;
    result->count = 0;
    memset(result->commitments, 0, sizeof(result->commitments));
    if (!text || text_len == 0)
        return SC_OK;

    sc_error_t err;
    err = scan_patterns(alloc, text, text_len, role, role_len, result, PROMISE_PATTERNS,
                       SC_COMMITMENT_PROMISE);
    if (err != SC_OK)
        return err;
    err = scan_patterns(alloc, text, text_len, role, role_len, result, INTENTION_PATTERNS,
                        SC_COMMITMENT_INTENTION);
    if (err != SC_OK)
        return err;
    err = scan_patterns(alloc, text, text_len, role, role_len, result, REMINDER_PATTERNS,
                       SC_COMMITMENT_REMINDER);
    if (err != SC_OK)
        return err;
    err = scan_patterns(alloc, text, text_len, role, role_len, result, GOAL_PATTERNS,
                       SC_COMMITMENT_GOAL);
    if (err != SC_OK)
        return err;
    return SC_OK;
}

void sc_commitment_deinit(sc_commitment_t *c, sc_allocator_t *alloc) {
    if (!c || !alloc)
        return;
    if (c->id)
        alloc->free(alloc->ctx, c->id, strlen(c->id) + 1);
    if (c->statement)
        alloc->free(alloc->ctx, c->statement, c->statement_len + 1);
    if (c->summary)
        alloc->free(alloc->ctx, c->summary, c->summary_len + 1);
    if (c->created_at)
        alloc->free(alloc->ctx, c->created_at, strlen(c->created_at) + 1);
    if (c->emotional_weight)
        alloc->free(alloc->ctx, c->emotional_weight, strlen(c->emotional_weight) + 1);
    if (c->owner)
        alloc->free(alloc->ctx, c->owner, strlen(c->owner) + 1);
    memset(c, 0, sizeof(*c));
}

void sc_commitment_detect_result_deinit(sc_commitment_detect_result_t *r, sc_allocator_t *alloc) {
    if (!r || !alloc)
        return;
    for (size_t i = 0; i < r->count; i++)
        sc_commitment_deinit(&r->commitments[i], alloc);
    r->count = 0;
}
