#include "human/experience.h"
#include "human/memory.h"
#include "human/memory/vector.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#include <time.h>
#endif

#define HU_EXP_MAX 64
#define HU_EXP_TEXT_MAX 512
#define HU_EXP_KEY_PREFIX "experience:"
#define HU_EXP_KEY_PREFIX_LEN 11
#define HU_EXP_KEY_TASK_CHARS 16
#define HU_EXP_RECALL_LIMIT 5

#define HU_EXP_CONTACT_PREFIX "[contact:"
#define HU_EXP_CONTACT_PREFIX_LEN 8

typedef struct {
    char task[HU_EXP_TEXT_MAX];
    size_t task_len;
    char actions[HU_EXP_TEXT_MAX];
    size_t actions_len;
    char outcome[HU_EXP_TEXT_MAX];
    size_t outcome_len;
    double score;
    char contact_id[128];
    size_t contact_id_len;
} exp_entry_t;

static exp_entry_t *s_entries = NULL;
static size_t s_capacity = 0;
static size_t s_write_idx = 0;

static size_t copy_truncated(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    size_t n = src_len;
    if (n >= dst_cap) n = dst_cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

static size_t count_word_overlap(const char *query, size_t query_len,
                                 const char *stored, size_t stored_len) {
    size_t overlap = 0;
    const char *q = query;
    const char *q_end = query + query_len;
    while (q < q_end) {
        while (q < q_end && (isspace((unsigned char)*q) || !isalnum((unsigned char)*q))) q++;
        if (q >= q_end) break;
        const char *word_start = q;
        while (q < q_end && (isalnum((unsigned char)*q) || *q == '_' || *q == '-')) q++;
        size_t word_len = (size_t)(q - word_start);
        if (word_len == 0) continue;
        const char *s = stored;
        const char *s_end = stored + stored_len;
        while (s < s_end) {
            while (s < s_end && (isspace((unsigned char)*s) || !isalnum((unsigned char)*s))) s++;
            if (s >= s_end) break;
            const char *sw_start = s;
            while (s < s_end && (isalnum((unsigned char)*s) || *s == '_' || *s == '-')) s++;
            size_t sw_len = (size_t)(s - sw_start);
            if (sw_len == word_len && memcmp(word_start, sw_start, word_len) == 0) {
                overlap++;
                break;
            }
        }
    }
    return overlap;
}

hu_error_t hu_experience_store_init(hu_allocator_t *alloc, hu_memory_t *memory,
                                    hu_experience_store_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->memory = memory;
    out->embedder = NULL;
    out->vec_store = NULL;
#ifdef HU_ENABLE_SQLITE
    out->db = NULL;
#endif
    out->stored_count = 0;
    if (memory == NULL && s_entries == NULL) {
        s_entries = (exp_entry_t *)alloc->alloc(alloc->ctx, HU_EXP_MAX * sizeof(exp_entry_t));
        if (!s_entries) return HU_ERR_OUT_OF_MEMORY;
        s_capacity = HU_EXP_MAX;
        s_write_idx = 0;
    } else if (memory == NULL) {
        s_write_idx = 0;
    }
    return HU_OK;
}

hu_error_t hu_experience_store_init_semantic(hu_allocator_t *alloc, hu_memory_t *memory,
                                             hu_embedder_t *embedder,
                                             hu_vector_store_t *vec_store,
                                             hu_experience_store_t *out) {
    hu_error_t err = hu_experience_store_init(alloc, memory, out);
    if (err != HU_OK) return err;
    out->embedder = embedder;
    out->vec_store = vec_store;
    return HU_OK;
}

void hu_experience_store_deinit(hu_experience_store_t *store) {
    if (!store) return;
    hu_allocator_t *alloc = store->alloc;
    if (alloc && s_entries) {
        alloc->free(alloc->ctx, s_entries, s_capacity * sizeof(exp_entry_t));
        s_entries = NULL;
        s_capacity = 0;
        s_write_idx = 0;
    }
    store->alloc = NULL;
    store->memory = NULL;
    store->embedder = NULL;
    store->vec_store = NULL;
#ifdef HU_ENABLE_SQLITE
    store->db = NULL;
#endif
    store->stored_count = 0;
}

hu_error_t hu_experience_record(hu_experience_store_t *store,
                                const char *task, size_t task_len,
                                const char *actions, size_t actions_len,
                                const char *outcome, size_t outcome_len,
                                double score) {
    if (!store || !store->alloc || !task || !actions || !outcome)
        return HU_ERR_INVALID_ARGUMENT;

    if (store->memory && store->memory->vtable && store->memory->vtable->store) {
        size_t task_copy = task_len > HU_EXP_KEY_TASK_CHARS ? HU_EXP_KEY_TASK_CHARS : task_len;
        char key_buf[HU_EXP_KEY_PREFIX_LEN + HU_EXP_KEY_TASK_CHARS + 2];
        size_t key_len = HU_EXP_KEY_PREFIX_LEN;
        memcpy(key_buf, HU_EXP_KEY_PREFIX, HU_EXP_KEY_PREFIX_LEN);
        for (size_t i = 0; i < task_copy && key_len < sizeof(key_buf) - 1; i++) {
            unsigned char c = (unsigned char)task[i];
            key_buf[key_len++] = (char)(isalnum(c) || c == '_' || c == '-' || c == ' ' ? c : '_');
        }
        if (task_copy == 0) {
            memcpy(key_buf + key_len, "empty", 5);
            key_len += 5;
        }
        key_buf[key_len] = '\0';
        size_t content_len = (size_t)snprintf(NULL, 0,
                                            "Task: %.*s\nActions: %.*s\nOutcome: %.*s\nScore: %.4f",
                                            (int)task_len, task,
                                            (int)actions_len, actions,
                                            (int)outcome_len, outcome,
                                            score);
        char *content = (char *)store->alloc->alloc(store->alloc->ctx, content_len + 1);
        if (!content) return HU_ERR_OUT_OF_MEMORY;
        (void)snprintf(content, content_len + 1,
                      "Task: %.*s\nActions: %.*s\nOutcome: %.*s\nScore: %.4f",
                      (int)task_len, task,
                      (int)actions_len, actions,
                      (int)outcome_len, outcome,
                      score);
        hu_error_t err = store->memory->vtable->store(store->memory->ctx, key_buf, key_len,
                                                     content, content_len, NULL, "", 0);
        store->alloc->free(store->alloc->ctx, content, content_len + 1);
        if (err == HU_OK)
            store->stored_count++;
        return err;
    }

    if (!s_entries) return HU_ERR_INVALID_ARGUMENT;
    size_t idx = s_write_idx % HU_EXP_MAX;
    exp_entry_t *e = &s_entries[idx];
    e->task_len = copy_truncated(e->task, sizeof(e->task), task, task_len);
    e->actions_len = copy_truncated(e->actions, sizeof(e->actions), actions, actions_len);
    e->outcome_len = copy_truncated(e->outcome, sizeof(e->outcome), outcome, outcome_len);
    e->score = score;
    e->contact_id_len = 0;
    s_write_idx++;
    store->stored_count++;

#ifdef HU_ENABLE_SQLITE
    /* Persist to experience_log for distillation */
    if (store->db) {
        const char *log_sql =
            "INSERT INTO experience_log (task, actions, outcome, score, recorded_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5)";
        sqlite3_stmt *log_stmt = NULL;
        if (sqlite3_prepare_v2(store->db, log_sql, -1, &log_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(log_stmt, 1, task, (int)task_len, SQLITE_STATIC);
            sqlite3_bind_text(log_stmt, 2, actions, (int)actions_len, SQLITE_STATIC);
            sqlite3_bind_text(log_stmt, 3, outcome, (int)outcome_len, SQLITE_STATIC);
            sqlite3_bind_double(log_stmt, 4, score);
            sqlite3_bind_int64(log_stmt, 5, (int64_t)time(NULL));
            sqlite3_step(log_stmt);
            sqlite3_finalize(log_stmt);
        }
    }
#endif

    /* Semantic embedding: insert into vector store if available */
    if (store->embedder && store->embedder->vtable && store->embedder->vtable->embed &&
        store->vec_store && store->vec_store->vtable && store->vec_store->vtable->insert) {
        hu_embedding_t emb = {0};
        if (store->embedder->vtable->embed(store->embedder->ctx, store->alloc,
                                           task, task_len, &emb) == HU_OK) {
            char id_buf[32];
            int id_len = snprintf(id_buf, sizeof(id_buf), "exp_%zu", store->stored_count);
            char content_buf[HU_EXP_TEXT_MAX * 3 + 64];
            int clen = snprintf(content_buf, sizeof(content_buf),
                               "Task: %.*s\nActions: %.*s\nOutcome: %.*s\nScore: %.2f",
                               (int)e->task_len, e->task,
                               (int)e->actions_len, e->actions,
                               (int)e->outcome_len, e->outcome, score);
            (void)store->vec_store->vtable->insert(
                store->vec_store->ctx, store->alloc,
                id_buf, (size_t)id_len, &emb,
                content_buf, clen > 0 ? (size_t)clen : 0);
            hu_embedding_free(store->alloc, &emb);
        }
    }

    return HU_OK;
}

hu_error_t hu_experience_recall_similar(hu_experience_store_t *store,
                                        const char *task, size_t task_len,
                                        char **out_context, size_t *out_len) {
    if (!store || !store->alloc || !task || !out_context || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_context = NULL;
    *out_len = 0;

    if (store->memory && store->memory->vtable && store->memory->vtable->recall) {
        hu_memory_entry_t *entries = NULL;
        size_t count = 0;
        hu_error_t err = store->memory->vtable->recall(store->memory->ctx, store->alloc,
                                                      task, task_len, HU_EXP_RECALL_LIMIT,
                                                      "", 0, &entries, &count);
        if (err != HU_OK || !entries || count == 0)
            return err;

        size_t total = 0;
        size_t with_content = 0;
        for (size_t i = 0; i < count; i++) {
            if (entries[i].content && entries[i].content_len > 0) {
                total += entries[i].content_len;
                with_content++;
            }
        }
        if (with_content > 1)
            total += (with_content - 1) * 5;
        if (total == 0) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(store->alloc, &entries[i]);
            store->alloc->free(store->alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            return HU_OK;
        }
        char *buf = (char *)store->alloc->alloc(store->alloc->ctx, total + 1);
        if (!buf) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(store->alloc, &entries[i]);
            store->alloc->free(store->alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t pos = 0;
        for (size_t i = 0; i < count; i++) {
            if (entries[i].content && entries[i].content_len > 0) {
                if (pos > 0) {
                    memcpy(buf + pos, "\n---\n", 5);
                    pos += 5;
                }
                memcpy(buf + pos, entries[i].content, entries[i].content_len);
                pos += entries[i].content_len;
            }
        }
        buf[pos] = '\0';

        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(store->alloc, &entries[i]);
        store->alloc->free(store->alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

        *out_context = buf;
        *out_len = pos;
        return HU_OK;
    }

    /* Semantic recall path: embed query and search vector store */
    if (store->embedder && store->embedder->vtable && store->embedder->vtable->embed &&
        store->vec_store && store->vec_store->vtable && store->vec_store->vtable->search) {
        hu_embedding_t q_emb = {0};
        if (store->embedder->vtable->embed(store->embedder->ctx, store->alloc,
                                           task, task_len, &q_emb) == HU_OK) {
            hu_vector_entry_t *entries = NULL;
            size_t vcount = 0;
            hu_error_t serr = store->vec_store->vtable->search(
                store->vec_store->ctx, store->alloc, &q_emb, 3, &entries, &vcount);
            hu_embedding_free(store->alloc, &q_emb);
            if (serr == HU_OK && entries && vcount > 0) {
                size_t total = 0;
                for (size_t i = 0; i < vcount; i++) {
                    if (entries[i].content && entries[i].content_len > 0)
                        total += entries[i].content_len;
                }
                if (vcount > 1) total += (vcount - 1) * 5;
                if (total > 0) {
                    char *buf = (char *)store->alloc->alloc(store->alloc->ctx, total + 1);
                    if (buf) {
                        size_t pos = 0;
                        for (size_t i = 0; i < vcount; i++) {
                            if (entries[i].content && entries[i].content_len > 0) {
                                if (pos > 0) {
                                    memcpy(buf + pos, "\n---\n", 5);
                                    pos += 5;
                                }
                                memcpy(buf + pos, entries[i].content, entries[i].content_len);
                                pos += entries[i].content_len;
                            }
                        }
                        buf[pos] = '\0';
                        *out_context = buf;
                        *out_len = pos;
                        hu_vector_entries_free(store->alloc, entries, vcount);
                        return HU_OK;
                    }
                }
                hu_vector_entries_free(store->alloc, entries, vcount);
            }
        }
    }

    if (!s_entries || store->stored_count == 0) return HU_OK;
    size_t count = store->stored_count < HU_EXP_MAX ? store->stored_count : HU_EXP_MAX;
    size_t best_idx = 0;
    size_t best_overlap = 0;
    double best_score = -1.0;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (s_write_idx + HU_EXP_MAX - 1 - i) % HU_EXP_MAX;
        exp_entry_t *e = &s_entries[idx];
        size_t overlap = count_word_overlap(task, task_len, e->task, e->task_len);
        if (overlap > best_overlap || (overlap == best_overlap && e->score > best_score)) {
            best_overlap = overlap;
            best_score = e->score;
            best_idx = idx;
        }
    }
    exp_entry_t *e = &s_entries[best_idx];
    size_t need = (size_t)snprintf(NULL, 0,
                                  "Previous experience: Task: %.*s, Actions: %.*s, Outcome: %.*s (score: %.2f)",
                                  (int)e->task_len, e->task,
                                  (int)e->actions_len, e->actions,
                                  (int)e->outcome_len, e->outcome,
                                  e->score);
    char *buf = (char *)store->alloc->alloc(store->alloc->ctx, need + 1);
    if (!buf) return HU_ERR_OUT_OF_MEMORY;
    (void)snprintf(buf, need + 1,
                   "Previous experience: Task: %.*s, Actions: %.*s, Outcome: %.*s (score: %.2f)",
                   (int)e->task_len, e->task,
                   (int)e->actions_len, e->actions,
                   (int)e->outcome_len, e->outcome,
                   e->score);
    *out_context = buf;
    *out_len = need;
    return HU_OK;
}

hu_error_t hu_experience_build_prompt(hu_experience_store_t *store,
                                      const char *current_task, size_t task_len,
                                      char **out, size_t *out_len) {
    if (!store || !store->alloc || !current_task || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_error_t err = hu_experience_recall_similar(store, current_task, task_len, &ctx, &ctx_len);
    if (err != HU_OK) return err;
    if (!ctx || ctx_len == 0) {
        char *empty = (char *)store->alloc->alloc(store->alloc->ctx, 1);
        if (!empty) return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *out = empty;
        *out_len = 0;
        return HU_OK;
    }
    size_t prefix_len = (size_t)strlen("[EXPERIENCE]: ");
    size_t need = prefix_len + ctx_len + 1;
    char *prompt = (char *)store->alloc->alloc(store->alloc->ctx, need);
    if (!prompt) {
        store->alloc->free(store->alloc->ctx, ctx, ctx_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(prompt, "[EXPERIENCE]: ", prefix_len);
    memcpy(prompt + prefix_len, ctx, ctx_len + 1);
    store->alloc->free(store->alloc->ctx, ctx, ctx_len + 1);
    *out = prompt;
    *out_len = prefix_len + ctx_len;
    return HU_OK;
}

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_experience_init_tables(hu_allocator_t *alloc, sqlite3 *db) {
    if (!alloc || !db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS experiences ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "task TEXT, outcome TEXT, score REAL, lessons TEXT, created_at INTEGER)";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_experience_record_db(hu_allocator_t *alloc, sqlite3 *db,
                                   const char *task, size_t task_len,
                                   const char *outcome, size_t outcome_len,
                                   double score, const char *lessons, size_t lessons_len) {
    (void)alloc;
    if (!db || !task || !outcome)
        return HU_ERR_INVALID_ARGUMENT;

    const char *ins =
        "INSERT INTO experiences (task, outcome, score, lessons, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, ins, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    size_t tlen = task_len > 511 ? 511 : task_len;
    size_t olen = outcome_len > 511 ? 511 : outcome_len;
    size_t llen = lessons_len > 1023 ? 1023 : lessons_len;

    char task_buf[512];
    char outcome_buf[512];
    char lessons_buf[1024];
    memcpy(task_buf, task, tlen);
    task_buf[tlen] = '\0';
    memcpy(outcome_buf, outcome, olen);
    outcome_buf[olen] = '\0';
    if (lessons && lessons_len > 0) {
        memcpy(lessons_buf, lessons, llen);
        lessons_buf[llen] = '\0';
    } else {
        lessons_buf[0] = '\0';
    }

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, task_buf, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, outcome_buf, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, score);
    sqlite3_bind_text(stmt, 4, lessons_buf, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_experience_recall_db(hu_allocator_t *alloc, sqlite3 *db,
                                   const char *query, size_t query_len,
                                   hu_experience_entry_t *results, size_t max_results,
                                   size_t *out_count) {
    (void)alloc;
    if (!results || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    if (!db)
        return HU_OK;
#endif
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    char pattern[1024];
    size_t qlen = query_len > 500 ? 500 : query_len;
    if (qlen == 0 || !query) {
        pattern[0] = '%';
        pattern[1] = '\0';
    } else {
        pattern[0] = '%';
        memcpy(pattern + 1, query, qlen);
        pattern[1 + qlen] = '%';
        pattern[2 + qlen] = '\0';
    }

    const char *sel =
        "SELECT id, task, outcome, score, lessons, created_at FROM experiences "
        "WHERE task LIKE ?1 ORDER BY score DESC LIMIT ?2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)max_results);

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        hu_experience_entry_t *e = &results[count];
        e->id = sqlite3_column_int64(stmt, 0);
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        const char *o = (const char *)sqlite3_column_text(stmt, 2);
        const char *l = (const char *)sqlite3_column_text(stmt, 4);
        e->score = sqlite3_column_double(stmt, 3);
        e->timestamp = sqlite3_column_int64(stmt, 5);

        if (t) {
            size_t tl = (size_t)sqlite3_column_bytes(stmt, 1);
            if (tl > 511) tl = 511;
            memcpy(e->task, t, tl);
            e->task[tl] = '\0';
            e->task_len = tl;
        } else {
            e->task[0] = '\0';
            e->task_len = 0;
        }
        if (o) {
            size_t ol = (size_t)sqlite3_column_bytes(stmt, 2);
            if (ol > 511) ol = 511;
            memcpy(e->outcome, o, ol);
            e->outcome[ol] = '\0';
            e->outcome_len = ol;
        } else {
            e->outcome[0] = '\0';
            e->outcome_len = 0;
        }
        if (l) {
            size_t ll = (size_t)sqlite3_column_bytes(stmt, 4);
            if (ll > 1023) ll = 1023;
            memcpy(e->lessons, l, ll);
            e->lessons[ll] = '\0';
            e->lessons_len = ll;
        } else {
            e->lessons[0] = '\0';
            e->lessons_len = 0;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
