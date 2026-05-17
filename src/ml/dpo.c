#include "human/ml/dpo.h"
#include "human/core/io_secure.h"
#include "human/provider.h"
#ifdef HU_ENABLE_SQLITE
#include "human/core/json.h"
#include "human/core/string.h"
#endif
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

hu_error_t hu_dpo_collector_create(hu_allocator_t *alloc,
#ifdef HU_ENABLE_SQLITE
                                   sqlite3 *db,
#else
                                   void *db,
#endif
                                   size_t max_pairs, hu_dpo_collector_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->db = db;
    out->max_pairs = max_pairs > 0 ? max_pairs : 10000;
    out->pair_count = 0;
    return HU_OK;
}

void hu_dpo_collector_deinit(hu_dpo_collector_t *collector) {
    if (collector)
        memset(collector, 0, sizeof(*collector));
}

hu_error_t hu_dpo_init_tables(hu_dpo_collector_t *collector) {
    if (!collector)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (!collector->db)
        return HU_OK;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS dpo_pairs("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "prompt TEXT, chosen TEXT, rejected TEXT, "
        "margin REAL, timestamp INTEGER, source TEXT);";
    char *err_msg = NULL;
    int rc = sqlite3_exec(collector->db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
#endif
    return HU_OK;
}

hu_error_t hu_dpo_record_pair(hu_dpo_collector_t *collector,
                              const hu_preference_pair_t *pair) {
    if (!collector || !pair)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_ENABLE_SQLITE
    if (collector->db) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(collector->db,
            "INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, timestamp, source) "
            "VALUES(?, ?, ?, ?, ?, ?)",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_IO;
        sqlite3_bind_text(stmt, 1, pair->prompt, (int)pair->prompt_len, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, pair->chosen, (int)pair->chosen_len, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, pair->rejected, (int)pair->rejected_len, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 4, pair->margin);
        sqlite3_bind_int64(stmt, 5, pair->timestamp);
        sqlite3_bind_text(stmt, 6, pair->source, (int)pair->source_len, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE)
            return HU_ERR_IO;

        /* Ring buffer eviction */
        if (collector->max_pairs > 0) {
            sqlite3_stmt *cnt_stmt = NULL;
            rc = sqlite3_prepare_v2(collector->db, "SELECT COUNT(*) FROM dpo_pairs", -1, &cnt_stmt, NULL);
            if (rc == SQLITE_OK && sqlite3_step(cnt_stmt) == SQLITE_ROW) {
                int64_t total = sqlite3_column_int64(cnt_stmt, 0);
                sqlite3_finalize(cnt_stmt);
                if ((size_t)total > collector->max_pairs) {
                    size_t excess = (size_t)total - collector->max_pairs;
                    char del_sql[128];
                    snprintf(del_sql, sizeof(del_sql),
                             "DELETE FROM dpo_pairs WHERE id IN "
                             "(SELECT id FROM dpo_pairs ORDER BY id LIMIT %zu)", excess);
                    sqlite3_exec(collector->db, del_sql, NULL, NULL, NULL);
                }
            } else if (cnt_stmt) {
                sqlite3_finalize(cnt_stmt);
            }
        }
    }
#endif
    collector->pair_count++;
    return HU_OK;
}

hu_error_t hu_dpo_record_from_feedback(hu_dpo_collector_t *collector,
                                       const char *prompt, size_t prompt_len,
                                       const char *response, size_t response_len,
                                       bool positive) {
    if (!collector || !prompt || !response)
        return HU_ERR_INVALID_ARGUMENT;

    hu_preference_pair_t pair;
    memset(&pair, 0, sizeof(pair));

    size_t plen = prompt_len < sizeof(pair.prompt) - 1 ? prompt_len : sizeof(pair.prompt) - 1;
    memcpy(pair.prompt, prompt, plen);
    pair.prompt_len = plen;

    if (positive) {
        size_t rlen = response_len < sizeof(pair.chosen) - 1 ? response_len : sizeof(pair.chosen) - 1;
        memcpy(pair.chosen, response, rlen);
        pair.chosen_len = rlen;
        pair.rejected_len = 0;
    } else {
        size_t rlen = response_len < sizeof(pair.rejected) - 1 ? response_len : sizeof(pair.rejected) - 1;
        memcpy(pair.rejected, response, rlen);
        pair.rejected_len = rlen;
        pair.chosen_len = 0;
    }

    pair.margin = 0.7;
    pair.timestamp = (int64_t)time(NULL);
    memcpy(pair.source, "user_feedback", 13);
    pair.source_len = 13;

    return hu_dpo_record_pair(collector, &pair);
}

hu_error_t hu_dpo_record_from_retry(hu_dpo_collector_t *collector,
                                    const char *prompt, size_t prompt_len,
                                    const char *rejected, size_t rejected_len,
                                    const char *chosen, size_t chosen_len) {
    if (!collector || !prompt || !rejected || !chosen)
        return HU_ERR_INVALID_ARGUMENT;

    hu_preference_pair_t pair;
    memset(&pair, 0, sizeof(pair));

    size_t plen = prompt_len < sizeof(pair.prompt) - 1 ? prompt_len : sizeof(pair.prompt) - 1;
    memcpy(pair.prompt, prompt, plen);
    pair.prompt_len = plen;

    size_t clen = chosen_len < sizeof(pair.chosen) - 1 ? chosen_len : sizeof(pair.chosen) - 1;
    memcpy(pair.chosen, chosen, clen);
    pair.chosen_len = clen;

    size_t rlen = rejected_len < sizeof(pair.rejected) - 1 ? rejected_len : sizeof(pair.rejected) - 1;
    memcpy(pair.rejected, rejected, rlen);
    pair.rejected_len = rlen;

    pair.margin = 0.8;
    pair.timestamp = (int64_t)time(NULL);
    memcpy(pair.source, "reflection_retry", 16);
    pair.source_len = 16;

    return hu_dpo_record_pair(collector, &pair);
}

hu_error_t hu_dpo_export_jsonl(hu_dpo_collector_t *collector,
                               const char *path, size_t path_len,
                               size_t *exported_count) {
    if (!collector || !exported_count)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    *exported_count = collector->pair_count;
    (void)path;
    (void)path_len;
    return HU_OK;
#else
#ifdef HU_ENABLE_SQLITE
    if (!collector->db || !path || path_len == 0) {
        *exported_count = 0;
        return HU_ERR_NOT_SUPPORTED;
    }

    char filepath[512];
    size_t flen = path_len < sizeof(filepath) - 1 ? path_len : sizeof(filepath) - 1;
    memcpy(filepath, path, flen);
    filepath[flen] = '\0';

    /* DPO preference pairs are JSONL output from collected feedback —
     * contains user conversation excerpts. 0600. */
    FILE *f = NULL;
    if (hu_io_secure_open(filepath, HU_IO_PERM_SECRET, "w", &f) != HU_OK || !f)
        return HU_ERR_IO;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(collector->db,
        "SELECT prompt, chosen, rejected, margin, source FROM dpo_pairs ORDER BY id",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fclose(f);
        return HU_ERR_IO;
    }

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *prompt = (const char *)sqlite3_column_text(stmt, 0);
        const char *chosen = (const char *)sqlite3_column_text(stmt, 1);
        const char *rejected = (const char *)sqlite3_column_text(stmt, 2);
        double margin = sqlite3_column_double(stmt, 3);
        const char *source = (const char *)sqlite3_column_text(stmt, 4);

        const char *fields[] = {prompt, chosen, rejected, NULL, source};
        const char *keys[] = {"prompt", "chosen", "rejected", NULL, "source"};
        fputc('{', f);
        for (int fi = 0; fi < 5; fi++) {
            if (fi == 3) {
                fprintf(f, "\"margin\":%.2f", margin);
            } else {
                const char *val = fields[fi] ? fields[fi] : "";
                fprintf(f, "\"%s\":\"", keys[fi]);
                for (const char *c = val; *c; c++) {
                    switch (*c) {
                    case '"':  fputs("\\\"", f); break;
                    case '\\': fputs("\\\\", f); break;
                    case '\n': fputs("\\n", f);  break;
                    case '\r': fputs("\\r", f);  break;
                    case '\t': fputs("\\t", f);  break;
                    default:   fputc(*c, f);     break;
                    }
                }
                fputc('"', f);
            }
            if (fi < 4)
                fputc(',', f);
        }
        fputs("}\n", f);
        count++;
    }
    sqlite3_finalize(stmt);
    fclose(f);
    *exported_count = count;
    return HU_OK;
#else
    *exported_count = 0;
    (void)path;
    (void)path_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

hu_error_t hu_dpo_export(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                         hu_dpo_export_t *out) {
    if (!collector || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->pairs = NULL;
    out->count = 0;
#ifdef HU_ENABLE_SQLITE
    if (!collector->db)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(collector->db,
        "SELECT prompt, chosen, rejected, margin, source, timestamp "
        "FROM dpo_pairs ORDER BY id",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    size_t cap = collector->pair_count > 0 ? collector->pair_count : 8;
    hu_preference_pair_t *pairs =
        alloc->alloc(alloc->ctx, cap * sizeof(hu_preference_pair_t));
    if (!pairs) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(pairs, 0, cap * sizeof(hu_preference_pair_t));

    size_t n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n == cap) {
            size_t old_cap = cap;
            cap *= 2;
            hu_preference_pair_t *grow =
                alloc->alloc(alloc->ctx, cap * sizeof(hu_preference_pair_t));
            if (!grow) {
                alloc->free(alloc->ctx, pairs, old_cap * sizeof(hu_preference_pair_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(grow, 0, cap * sizeof(hu_preference_pair_t));
            memcpy(grow, pairs, n * sizeof(hu_preference_pair_t));
            alloc->free(alloc->ctx, pairs, old_cap * sizeof(hu_preference_pair_t));
            pairs = grow;
        }
        hu_preference_pair_t *p = &pairs[n];
        const char *prompt = (const char *)sqlite3_column_text(stmt, 0);
        const char *chosen = (const char *)sqlite3_column_text(stmt, 1);
        const char *rejected = (const char *)sqlite3_column_text(stmt, 2);
        const char *source = (const char *)sqlite3_column_text(stmt, 4);
        if (prompt) {
            size_t L = strnlen(prompt, sizeof(p->prompt) - 1);
            memcpy(p->prompt, prompt, L);
            p->prompt_len = L;
        }
        if (chosen) {
            size_t L = strnlen(chosen, sizeof(p->chosen) - 1);
            memcpy(p->chosen, chosen, L);
            p->chosen_len = L;
        }
        if (rejected) {
            size_t L = strnlen(rejected, sizeof(p->rejected) - 1);
            memcpy(p->rejected, rejected, L);
            p->rejected_len = L;
        }
        p->margin = sqlite3_column_double(stmt, 3);
        p->timestamp = sqlite3_column_int64(stmt, 5);
        if (source) {
            size_t L = strnlen(source, sizeof(p->source) - 1);
            memcpy(p->source, source, L);
            p->source_len = L;
        }
        n++;
    }
    sqlite3_finalize(stmt);
    out->pairs = pairs;
    out->count = n;
    return HU_OK;
#else
    (void)collector;
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_dpo_get_best_examples(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                                    size_t max_examples, char **out_prompt_fragment,
                                    size_t *out_len) {
    if (!collector || !alloc || !out_prompt_fragment || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_prompt_fragment = NULL;
    *out_len = 0;
    if (max_examples == 0)
        return HU_OK;

#ifdef HU_ENABLE_SQLITE
    if (!collector->db)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT prompt, chosen, rejected FROM dpo_pairs "
                      "WHERE margin > 0.5 ORDER BY margin DESC LIMIT ?";
    if (sqlite3_prepare_v2(collector->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    int lim = (int)(max_examples > 10 ? 10 : max_examples);
    if (lim < 1)
        lim = 1;
    sqlite3_bind_int(stmt, 1, lim);

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        return err;
    }

    static const char header[] = "\n\n[Learned preferences from past conversations]\n";
    err = hu_json_buf_append_raw(&buf, header, sizeof(header) - 1u);
    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        sqlite3_finalize(stmt);
        return err;
    }

    size_t found = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && found < max_examples) {
        const char *prompt = (const char *)sqlite3_column_text(stmt, 0);
        const char *chosen = (const char *)sqlite3_column_text(stmt, 1);
        const char *rejected = (const char *)sqlite3_column_text(stmt, 2);
        if (!prompt || !chosen)
            continue;

        char entry[1024];
        int n;
        if (rejected && rejected[0]) {
            n = snprintf(entry, sizeof(entry),
                "When asked: \"%.*s\"\n  GOOD response: \"%.*s\"\n  BAD response: \"%.*s\"\n\n",
                (int)(strlen(prompt) < 200 ? strlen(prompt) : 200), prompt,
                (int)(strlen(chosen) < 300 ? strlen(chosen) : 300), chosen,
                (int)(strlen(rejected) < 200 ? strlen(rejected) : 200), rejected);
        } else {
            n = snprintf(entry, sizeof(entry),
                "When asked: \"%.*s\"\n  GOOD response: \"%.*s\"\n\n",
                (int)(strlen(prompt) < 200 ? strlen(prompt) : 200), prompt,
                (int)(strlen(chosen) < 300 ? strlen(chosen) : 300), chosen);
        }
        if (n > 0) {
            size_t append_len = (size_t)n >= sizeof(entry) ? sizeof(entry) - 1 : (size_t)n;
            err = hu_json_buf_append_raw(&buf, entry, append_len);
            if (err != HU_OK) {
                hu_json_buf_free(&buf);
                sqlite3_finalize(stmt);
                return err;
            }
        }
        found++;
    }
    sqlite3_finalize(stmt);

    if (found == 0) {
        hu_json_buf_free(&buf);
        return HU_OK;
    }

    size_t frag_len = buf.len;
    char *dup = hu_strndup(alloc, buf.ptr, frag_len);
    hu_json_buf_free(&buf);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out_prompt_fragment = dup;
    *out_len = frag_len;
    return HU_OK;
#else
    (void)collector;
    (void)alloc;
    (void)max_examples;
    return HU_OK;
#endif
}

hu_error_t hu_dpo_pair_count(hu_dpo_collector_t *collector, size_t *out) {
    if (!collector || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = collector->pair_count;
    return HU_OK;
}

hu_error_t hu_dpo_clear(hu_dpo_collector_t *collector) {
    if (!collector)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_ENABLE_SQLITE
    if (collector->db)
        sqlite3_exec(collector->db, "DELETE FROM dpo_pairs", NULL, NULL, NULL);
#endif
    collector->pair_count = 0;
    return HU_OK;
}

void hu_dpo_export_free(hu_allocator_t *alloc, hu_dpo_export_t *export_data) {
    if (!alloc || !export_data)
        return;
    if (export_data->pairs) {
        alloc->free(alloc->ctx, export_data->pairs,
                    export_data->count * sizeof(hu_preference_pair_t));
        export_data->pairs = NULL;
    }
    export_data->count = 0;
}

/* Phase 0 — renamed from `hu_dpo_train_step` to keep the M3 narrative
 * honest. The legacy name is preserved as an inline forwarding shim in
 * include/human/ml/dpo.h. See test_dpo_judge_naming.c for the
 * identical-return contract. */
hu_error_t hu_dpo_judge_step(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                             hu_provider_t *provider, const char *model, size_t model_len,
                             double beta, size_t batch_size,
                             hu_dpo_judge_result_t *out) {
    if (!collector || !alloc || !provider || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (beta <= 0.0)
        beta = 0.1;

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Deterministic mock: simulate training on 3 pairs */
    out->pairs_evaluated = 3;
    out->pairs_aligned = 2;
    out->alignment_score = 2.0 / 3.0;
    out->loss = 0.45;
    (void)collector;
    (void)alloc;
    (void)provider;
    (void)model;
    (void)model_len;
    (void)batch_size;
    return HU_OK;
#else
#ifdef HU_ENABLE_SQLITE
    if (!collector->db)
        return HU_ERR_NOT_SUPPORTED;
    if (!provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;

    int lim = (batch_size > 0 && batch_size < 100) ? (int)batch_size : 20;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT prompt, chosen, rejected, margin FROM dpo_pairs "
                      "WHERE chosen != '' AND rejected != '' "
                      "ORDER BY margin DESC LIMIT ?";
    if (sqlite3_prepare_v2(collector->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int(stmt, 1, lim);

    double total_loss = 0.0;
    size_t evaluated = 0;
    size_t aligned = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *prompt = (const char *)sqlite3_column_text(stmt, 0);
        const char *chosen = (const char *)sqlite3_column_text(stmt, 1);
        const char *rejected = (const char *)sqlite3_column_text(stmt, 2);
        if (!prompt || !chosen || !rejected)
            continue;

        /* Get log-probability proxy for chosen response via LLM scoring */
        static const char score_sys[] =
            "Rate how well this response answers the prompt on a scale of 0-100. "
            "Output ONLY a number.";

        char chosen_prompt[4096];
        int cn = snprintf(chosen_prompt, sizeof(chosen_prompt),
                          "Prompt: \"%.*s\"\nResponse: \"%.*s\"",
                          (int)(strlen(prompt) < 500 ? strlen(prompt) : 500), prompt,
                          (int)(strlen(chosen) < 1500 ? strlen(chosen) : 1500), chosen);

        char rejected_prompt[4096];
        int rn = snprintf(rejected_prompt, sizeof(rejected_prompt),
                          "Prompt: \"%.*s\"\nResponse: \"%.*s\"",
                          (int)(strlen(prompt) < 500 ? strlen(prompt) : 500), prompt,
                          (int)(strlen(rejected) < 1500 ? strlen(rejected) : 1500), rejected);

        char *chosen_out = NULL;
        size_t chosen_out_len = 0;
        char *rejected_out = NULL;
        size_t rejected_out_len = 0;

        hu_error_t e1 = provider->vtable->chat_with_system(
            provider->ctx, alloc, score_sys, sizeof(score_sys) - 1u,
            chosen_prompt, cn > 0 ? (size_t)cn : 0u,
            model ? model : "", model_len, 0.0,
            &chosen_out, &chosen_out_len);

        hu_error_t e2 = provider->vtable->chat_with_system(
            provider->ctx, alloc, score_sys, sizeof(score_sys) - 1u,
            rejected_prompt, rn > 0 ? (size_t)rn : 0u,
            model ? model : "", model_len, 0.0,
            &rejected_out, &rejected_out_len);

        double chosen_score = 50.0;
        double rejected_score = 50.0;

        if (e1 == HU_OK && chosen_out) {
            for (size_t ci = 0; ci < chosen_out_len; ci++) {
                if (chosen_out[ci] >= '0' && chosen_out[ci] <= '9') {
                    chosen_score = 0;
                    while (ci < chosen_out_len && chosen_out[ci] >= '0' && chosen_out[ci] <= '9') {
                        chosen_score = chosen_score * 10.0 + (double)(chosen_out[ci] - '0');
                        ci++;
                    }
                    break;
                }
            }
            alloc->free(alloc->ctx, chosen_out, chosen_out_len + 1u);
        }
        if (e2 == HU_OK && rejected_out) {
            for (size_t ci = 0; ci < rejected_out_len; ci++) {
                if (rejected_out[ci] >= '0' && rejected_out[ci] <= '9') {
                    rejected_score = 0;
                    while (ci < rejected_out_len && rejected_out[ci] >= '0' && rejected_out[ci] <= '9') {
                        rejected_score = rejected_score * 10.0 + (double)(rejected_out[ci] - '0');
                        ci++;
                    }
                    break;
                }
            }
            alloc->free(alloc->ctx, rejected_out, rejected_out_len + 1u);
        }

        /* Normalize scores to log-probability proxy */
        double log_ratio = (chosen_score - rejected_score) / 100.0;

        /* DPO loss: -log(sigmoid(beta * log_ratio)) */
        double sigmoid_val = 1.0 / (1.0 + exp(-beta * log_ratio));
        if (sigmoid_val < 1e-10)
            sigmoid_val = 1e-10;
        double pair_loss = -log(sigmoid_val);
        total_loss += pair_loss;

        if (chosen_score > rejected_score)
            aligned++;
        evaluated++;
    }
    sqlite3_finalize(stmt);

    out->pairs_evaluated = evaluated;
    out->pairs_aligned = aligned;
    out->alignment_score = evaluated > 0 ? (double)aligned / (double)evaluated : 0.0;
    out->loss = evaluated > 0 ? total_loss / (double)evaluated : 0.0;
    return HU_OK;
#else
    (void)provider;
    (void)model;
    (void)model_len;
    (void)beta;
    (void)batch_size;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}
