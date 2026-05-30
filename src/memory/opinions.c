typedef int hu_opinions_unused_; /* ISO C requires non-empty translation unit */

#ifdef HU_ENABLE_SQLITE

#include "human/memory/opinions.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/opinions_repo.h"
#include <ctype.h>
#include <string.h>

/* DDD Phase 3: SQL + the raw sqlite3 handle now live behind hu_opinions_repo_t
 * (src/memory/repos/opinions_repo_sqlite.c). This file keeps the upsert DECISION
 * logic (same-position vs supersede) and depends only on the backend-agnostic
 * repo interface — no <sqlite3.h>. */

static bool position_matches(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    return (a_len == 0 && b_len == 0) || (a && b && memcmp(a, b, a_len) == 0);
}

hu_error_t hu_opinions_upsert(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                              size_t topic_len, const char *position, size_t position_len,
                              float confidence, int64_t now_ts) {
    if (!alloc || !memory)
        return HU_ERR_INVALID_ARGUMENT;

    hu_opinions_repo_t repo;
    hu_error_t e = hu_opinions_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e; /* HU_ERR_NOT_SUPPORTED for non-sqlite, as before */

    bool found = false;
    int64_t old_id = 0;
    char *old_pos = NULL;
    size_t old_pos_len = 0;
    e = repo.vtable->find_active(repo.ctx, alloc, topic, topic_len, &found, &old_id, &old_pos,
                                 &old_pos_len);
    if (e != HU_OK) {
        repo.vtable->deinit(repo.ctx);
        return e;
    }

    if (found) {
        bool same_position = position_matches(position, position_len, old_pos, old_pos_len);
        if (old_pos)
            hu_str_free(alloc, old_pos);
        if (same_position) {
            /* Same position: update last_expressed, confidence. */
            e = repo.vtable->update_last_expressed(repo.ctx, old_id, now_ts, confidence);
        } else {
            /* Different position: insert new, then supersede old (atomic). */
            int64_t new_id = 0;
            e = repo.vtable->insert_superseding(repo.ctx, topic, topic_len, position, position_len,
                                                confidence, now_ts, old_id, &new_id);
        }
    } else {
        /* No existing: insert new. */
        int64_t new_id = 0;
        e = repo.vtable->insert(repo.ctx, topic, topic_len, position, position_len, confidence,
                                now_ts, now_ts, &new_id);
    }

    repo.vtable->deinit(repo.ctx);
    return e;
}

static hu_error_t opinions_query(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                                 size_t topic_len, bool superseded_only, hu_opinion_t **out,
                                 size_t *out_count) {
    if (!alloc || !memory || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_opinions_repo_t repo;
    hu_error_t e = hu_opinions_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e;
    e = repo.vtable->query(repo.ctx, alloc, topic, topic_len, superseded_only, out, out_count);
    repo.vtable->deinit(repo.ctx);
    return e;
}

hu_error_t hu_opinions_get(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                           size_t topic_len, hu_opinion_t **out, size_t *out_count) {
    return opinions_query(alloc, memory, topic, topic_len, false, out, out_count);
}

hu_error_t hu_opinions_get_superseded(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                                      size_t topic_len, hu_opinion_t **out, size_t *out_count) {
    return opinions_query(alloc, memory, topic, topic_len, true, out, out_count);
}

void hu_opinions_free(hu_allocator_t *alloc, hu_opinion_t *ops, size_t count) {
    if (!alloc || !ops)
        return;
    for (size_t i = 0; i < count; i++) {
        if (ops[i].topic)
            hu_str_free(alloc, ops[i].topic);
        if (ops[i].position)
            hu_str_free(alloc, ops[i].position);
    }
    alloc->free(alloc->ctx, ops, count * sizeof(hu_opinion_t));
}

#endif /* HU_ENABLE_SQLITE */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool hu_opinions_is_core_value(const char *topic, size_t topic_len, const char *const *core_values,
                               size_t count) {
    if (!topic || !core_values || count == 0)
        return false;
    for (size_t i = 0; i < count; i++) {
        const char *cv = core_values[i];
        if (!cv)
            continue;
        size_t cv_len = strlen(cv);
        if (topic_len != cv_len)
            continue;
        bool match = true;
        for (size_t j = 0; j < topic_len; j++) {
            if (tolower((unsigned char)topic[j]) != tolower((unsigned char)cv[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}
