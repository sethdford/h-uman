#ifndef HU_MEMORY_SELF_AWARENESS_REPO_H
#define HU_MEMORY_SELF_AWARENESS_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DDD Phase 3: repository for the self_awareness aggregate's two tables
 * (self_awareness_stats + reciprocity_scores). SQL + the raw sqlite3 handle
 * live ONLY in src/memory/repos/self_awareness_repo_sqlite.c; domain code
 * (src/context/self_awareness.c) depends on this vtable, never on <sqlite3.h>.
 * The ratio math, directive text, and reciprocity-compute stay in the domain.
 * The executor layer migrated here already used bound parameters, reproduced
 * verbatim. Gated to match the executors' HU_ENABLE_SQLITE gate. */
#ifdef HU_ENABLE_SQLITE

/* Raw self_awareness_stats row (the columns the domain reads). last_topic is
 * NUL-terminated into the caller's fixed buffer (no allocation). */
typedef struct hu_self_awareness_stats_row {
    uint32_t messages_sent_week;
    uint32_t initiations_week;
    char last_topic[128];
    size_t last_topic_len;
    uint32_t topic_repeat_count;
} hu_self_awareness_stats_row_t;

struct hu_self_awareness_repo_vtable;
typedef struct hu_self_awareness_repo {
    void *ctx;
    const struct hu_self_awareness_repo_vtable *vtable;
} hu_self_awareness_repo_t;

typedef struct hu_self_awareness_repo_vtable {
    /* self_awareness_stats upsert for a recorded send (INSERT ... ON CONFLICT).
     * init_inc is 0/1; topic may be NULL (binds NULL). */
    hu_error_t (*stats_record_send)(void *ctx, const char *contact_id, size_t cid_len, int init_inc,
                                    const char *topic, size_t topic_len, int64_t now_ts);
    /* self_awareness_stats read. *found=false if no row; *out filled otherwise. */
    hu_error_t (*stats_get)(void *ctx, const char *contact_id, size_t cid_len, bool *found,
                            hu_self_awareness_stats_row_t *out);
    /* reciprocity_scores: one metric value (0.0 if missing or on error). */
    double (*reciprocity_get)(void *ctx, const char *contact_id, size_t cid_len,
                              const char *metric);
    /* reciprocity_scores: set one metric (INSERT OR REPLACE). */
    hu_error_t (*reciprocity_set)(void *ctx, const char *contact_id, size_t cid_len,
                                  const char *metric, double value, int64_t now_ts);
    void (*deinit)(void *ctx);
} hu_self_awareness_repo_vtable_t;

/* Factory: sqlite-backed repo from `mem`. HU_ERR_NOT_SUPPORTED for non-sqlite.
 * Caller owns *out and must call out->vtable->deinit. */
hu_error_t hu_self_awareness_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                         hu_self_awareness_repo_t *out);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_SELF_AWARENESS_REPO_H */
