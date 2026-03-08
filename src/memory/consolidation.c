#include "seaclaw/memory/consolidation.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory/connections.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SC_CONS_MAX_TOKENS 64

static void tokenize(const char *s, size_t len, const char **tokens, size_t *count) {
    *count = 0;
    if (!s || len == 0)
        return;
    size_t i = 0;
    while (i < len && *count < SC_CONS_MAX_TOKENS) {
        while (i < len && (unsigned char)s[i] <= ' ')
            i++;
        if (i >= len)
            break;
        tokens[(*count)++] = s + i;
        while (i < len && (unsigned char)s[i] > ' ')
            i++;
    }
}

static size_t token_len(const char *token, const char *end) {
    size_t n = 0;
    while (token + n < end && (unsigned char)token[n] > ' ')
        n++;
    return n;
}

uint32_t sc_similarity_score(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (!a || !b)
        return 0;
    if (a_len == 0 && b_len == 0)
        return 100;

    const char *a_tokens[SC_CONS_MAX_TOKENS];
    const char *b_tokens[SC_CONS_MAX_TOKENS];
    size_t a_count = 0, b_count = 0;
    tokenize(a, a_len, a_tokens, &a_count);
    tokenize(b, b_len, b_tokens, &b_count);

    if (a_count == 0 && b_count == 0)
        return 100;
    if (a_count == 0 || b_count == 0)
        return 0;

    size_t shared = 0;
    const char *a_end = a + a_len;
    const char *b_end = b + b_len;

    for (size_t i = 0; i < a_count; i++) {
        size_t ai_len = token_len(a_tokens[i], a_end);
        for (size_t j = 0; j < b_count; j++) {
            size_t bj_len = token_len(b_tokens[j], b_end);
            if (ai_len == bj_len && memcmp(a_tokens[i], b_tokens[j], ai_len) == 0) {
                shared++;
                break;
            }
        }
    }

    size_t total_unique = a_count + b_count - shared;
    if (total_unique == 0)
        return 100;
    return (uint32_t)((shared * 100) / total_unique);
}

static time_t parse_iso_timestamp(const char *ts, size_t ts_len) {
    if (!ts || ts_len == 0)
        return 0;
    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));
    if (ts_len >= 19 && ts[4] == '-' && ts[7] == '-' && ts[10] == 'T') {
        tm_buf.tm_year = (int)strtol(ts, NULL, 10) - 1900;
        tm_buf.tm_mon = (int)strtol(ts + 5, NULL, 10) - 1;
        tm_buf.tm_mday = (int)strtol(ts + 8, NULL, 10);
        tm_buf.tm_hour = (int)strtol(ts + 11, NULL, 10);
        tm_buf.tm_min = (int)strtol(ts + 14, NULL, 10);
        tm_buf.tm_sec = (int)strtol(ts + 17, NULL, 10);
        return mktime(&tm_buf);
    }
    long raw = strtol(ts, NULL, 10);
    return raw > 0 ? (time_t)raw : 0;
}

static int compare_timestamp(const char *ts_a, size_t ts_a_len, const char *ts_b, size_t ts_b_len) {
    if (!ts_a || ts_a_len == 0)
        return 1;
    if (!ts_b || ts_b_len == 0)
        return -1;
    /* ISO 8601 timestamps (YYYY-MM-DDThh:mm:ssZ) sort lexicographically */
    size_t cmp_len = ts_a_len < ts_b_len ? ts_a_len : ts_b_len;
    int cmp = memcmp(ts_a, ts_b, cmp_len);
    if (cmp != 0)
        return cmp < 0 ? -1 : 1;
    if (ts_a_len != ts_b_len)
        return ts_a_len < ts_b_len ? -1 : 1;
    return 0;
}

sc_error_t sc_memory_consolidate(sc_allocator_t *alloc, sc_memory_t *memory,
                                 const sc_consolidation_config_t *config) {
    if (!alloc || !memory || !memory->vtable || !config)
        return SC_ERR_INVALID_ARGUMENT;
    if (!memory->vtable->list || !memory->vtable->forget)
        return SC_ERR_NOT_SUPPORTED;

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != SC_OK)
        return err;
    if (!entries || count == 0)
        return SC_OK;

    bool *to_forget = (bool *)alloc->alloc(alloc->ctx, count * sizeof(bool));
    if (!to_forget) {
        for (size_t i = 0; i < count; i++)
            sc_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(to_forget, 0, count * sizeof(bool));

    for (size_t i = 0; i < count; i++) {
        if (to_forget[i])
            continue;
        for (size_t j = i + 1; j < count; j++) {
            if (to_forget[j])
                continue;
            uint32_t sim = sc_similarity_score(entries[i].content, entries[i].content_len,
                                               entries[j].content, entries[j].content_len);
            if (sim >= config->dedup_threshold) {
                int cmp = compare_timestamp(entries[i].timestamp, entries[i].timestamp_len,
                                            entries[j].timestamp, entries[j].timestamp_len);
                if (cmp <= 0)
                    to_forget[i] = true;
                else
                    to_forget[j] = true;
            }
        }
    }

    /* Time-based decay: mark old entries for removal */
    if (config->decay_days > 0) {
        time_t now = time(NULL);
        time_t threshold = now - (time_t)(config->decay_days * 86400);
        for (size_t i = 0; i < count; i++) {
            if (to_forget[i])
                continue;
            if (entries[i].timestamp && entries[i].timestamp_len > 0) {
                time_t ts = parse_iso_timestamp(entries[i].timestamp, entries[i].timestamp_len);
                if (ts > 0 && ts < threshold) {
                    to_forget[i] = true;
                }
            }
        }
    }

#ifndef SC_IS_TEST
    if (config->provider && config->provider->vtable &&
        config->provider->vtable->chat_with_system) {
        size_t surviving = 0;
        for (size_t i = 0; i < count; i++)
            if (!to_forget[i])
                surviving++;

        if (surviving >= 2) {
            sc_memory_entry_t *surv = (sc_memory_entry_t *)alloc->alloc(
                alloc->ctx, surviving * sizeof(sc_memory_entry_t));
            if (surv) {
                size_t si = 0;
                for (size_t i = 0; i < count; i++)
                    if (!to_forget[i])
                        surv[si++] = entries[i];

                char *prompt = NULL;
                size_t prompt_len = 0;
                if (sc_connections_build_prompt(alloc, surv, surviving, &prompt, &prompt_len) ==
                        SC_OK &&
                    prompt) {
                    char *response = NULL;
                    size_t response_len = 0;
                    const char *sys = "Return JSON only.";
                    sc_error_t chat_err = config->provider->vtable->chat_with_system(
                        config->provider->ctx, alloc, sys, 17, prompt, prompt_len, config->model,
                        config->model_len, 0.2, &response, &response_len);
                    alloc->free(alloc->ctx, prompt, SC_CONN_PROMPT_CAP);

                    if (chat_err == SC_OK && response) {
                        sc_connection_result_t conn_result = {0};
                        if (sc_connections_parse(alloc, response, response_len, surviving,
                                                 &conn_result) == SC_OK) {
                            (void)sc_connections_store_insights(alloc, memory, &conn_result, surv,
                                                                surviving);
                            sc_connection_result_deinit(&conn_result, alloc);
                        }
                        alloc->free(alloc->ctx, response, response_len + 1);
                    }
                }
                alloc->free(alloc->ctx, surv, surviving * sizeof(sc_memory_entry_t));
            }
        }
    }
#endif

    for (size_t i = 0; i < count; i++) {
        if (to_forget[i] && entries[i].key) {
            bool deleted = false;
            (void)memory->vtable->forget(memory->ctx, entries[i].key, entries[i].key_len, &deleted);
        }
    }

    alloc->free(alloc->ctx, to_forget, count * sizeof(bool));

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));

    size_t new_count = 0;
    err = memory->vtable->count(memory->ctx, &new_count);
    if (err != SC_OK)
        return SC_OK;

    if (new_count <= config->max_entries)
        return SC_OK;

    err = memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != SC_OK || !entries || count <= config->max_entries)
        return SC_OK;

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            int cmp = compare_timestamp(entries[i].timestamp, entries[i].timestamp_len,
                                        entries[j].timestamp, entries[j].timestamp_len);
            if (cmp > 0) {
                sc_memory_entry_t tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    size_t to_remove = count - config->max_entries;
    for (size_t i = 0; i < to_remove && i < count; i++) {
        if (entries[i].key) {
            bool deleted = false;
            (void)memory->vtable->forget(memory->ctx, entries[i].key, entries[i].key_len, &deleted);
        }
    }

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));

    return SC_OK;
}
