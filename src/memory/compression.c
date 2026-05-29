#include "human/memory/compression.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_COMPRESSION_ESCAPE_BUF 1024
#define HU_COMPRESSION_SQL_BUF    4096

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}

hu_error_t hu_compression_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] = "CREATE TABLE IF NOT EXISTS shared_references (\n"
                              "    id INTEGER PRIMARY KEY,\n"
                              "    contact_id TEXT NOT NULL,\n"
                              "    compressed_form TEXT NOT NULL,\n"
                              "    expanded_meaning TEXT NOT NULL,\n"
                              "    usage_count INTEGER DEFAULT 1,\n"
                              "    strength REAL DEFAULT 0.3,\n"
                              "    created_at INTEGER NOT NULL,\n"
                              "    last_used_at INTEGER,\n"
                              "    compression_stage INTEGER DEFAULT 1,\n"
                              "    UNIQUE(contact_id, compressed_form)\n"
                              ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_compression_insert_sql(const hu_shared_ref_t *ref, char *buf, size_t cap,
                                     size_t *out_len) {
    if (!ref || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ref->contact_id || !ref->compressed_form || !ref->expanded_meaning)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_COMPRESSION_ESCAPE_BUF];
    char compressed_esc[HU_COMPRESSION_ESCAPE_BUF];
    char expanded_esc[HU_COMPRESSION_ESCAPE_BUF];

    size_t ce_len, cfe_len, ee_len;
    escape_sql_string(ref->contact_id, ref->contact_id_len, contact_esc, sizeof(contact_esc),
                      &ce_len);
    escape_sql_string(ref->compressed_form, ref->compressed_form_len, compressed_esc,
                      sizeof(compressed_esc), &cfe_len);
    escape_sql_string(ref->expanded_meaning, ref->expanded_meaning_len, expanded_esc,
                      sizeof(expanded_esc), &ee_len);

    int n;
    if (ref->last_used_at > 0) {
        n = snprintf(buf, cap,
                     "INSERT OR REPLACE INTO shared_references "
                     "(id, contact_id, compressed_form, expanded_meaning, usage_count, strength, "
                     "created_at, last_used_at, compression_stage) VALUES "
                     "(%lld, '%s', '%s', '%s', %u, %f, %llu, %llu, %d)",
                     (long long)ref->id, contact_esc, compressed_esc, expanded_esc,
                     ref->usage_count, ref->strength, (unsigned long long)ref->created_at,
                     (unsigned long long)ref->last_used_at, (int)ref->stage);
    } else {
        n = snprintf(buf, cap,
                     "INSERT OR REPLACE INTO shared_references "
                     "(id, contact_id, compressed_form, expanded_meaning, usage_count, strength, "
                     "created_at, last_used_at, compression_stage) VALUES "
                     "(%lld, '%s', '%s', '%s', %u, %f, %llu, NULL, %d)",
                     (long long)ref->id, contact_esc, compressed_esc, expanded_esc,
                     ref->usage_count, ref->strength, (unsigned long long)ref->created_at,
                     (int)ref->stage);
    }
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_compression_query_sql(const char *contact_id, size_t contact_id_len, char *buf,
                                    size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_COMPRESSION_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc), &ce_len);

    int n = snprintf(buf, cap,
                     "SELECT id, contact_id, compressed_form, expanded_meaning, usage_count, "
                     "strength, created_at, last_used_at, compression_stage FROM shared_references "
                     "WHERE contact_id = '%s' ORDER BY strength DESC",
                     contact_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_compression_record_usage_sql(int64_t ref_id, uint64_t now_ms, char *buf, size_t cap,
                                           size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "UPDATE shared_references SET usage_count = usage_count + 1, last_used_at = "
                     "%llu WHERE id = %lld",
                     (unsigned long long)now_ms, (long long)ref_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

size_t hu_compression_find_in_message(const hu_shared_ref_t *refs, size_t ref_count,
                                      const char *message, size_t msg_len, size_t *match_indices,
                                      size_t max_matches) {
    if (!refs || !message || !match_indices || max_matches == 0)
        return 0;

    size_t count = 0;
    for (size_t i = 0; i < ref_count && count < max_matches; i++) {
        if (!refs[i].compressed_form || refs[i].compressed_form_len == 0)
            continue;
        if (hu_str_contains_ci(message, msg_len, refs[i].compressed_form,
                               refs[i].compressed_form_len)) {
            match_indices[count++] = i;
        }
    }
    return count;
}

bool hu_compression_should_deploy(const hu_shared_ref_t *ref, const hu_compression_config_t *config,
                                  bool in_conflict, uint32_t seed) {
    if (!ref || !config)
        return false;
    if (in_conflict && config->never_during_conflict)
        return false;
    if (ref->strength < config->min_strength_to_deploy)
        return false;

    /* LCG: seed * 1103515245 + 12345, map to [0, 1) */
    uint32_t next = (uint32_t)((uint64_t)seed * 1103515245ULL + 12345ULL);
    double r = (double)(next & 0x7FFFFFFFU) / (double)0x80000000UL;
    return r < config->deployment_probability;
}

double hu_compression_decay_strength(double current_strength, uint64_t last_used_ms,
                                     uint64_t now_ms, double decay_rate_per_week) {
    if (last_used_ms >= now_ms)
        return current_strength;
    uint64_t elapsed_ms = now_ms - last_used_ms;
    uint64_t ms_per_week = 7ULL * 86400ULL * 1000ULL;
    double weeks_elapsed = (double)elapsed_ms / (double)ms_per_week;
    double decayed = current_strength - (decay_rate_per_week * weeks_elapsed);
    if (decayed < 0.0)
        return 0.0;
    if (decayed > 1.0)
        return 1.0;
    return decayed;
}

bool hu_compression_should_advance(const hu_shared_ref_t *ref, uint32_t min_uses) {
    if (!ref)
        return false;
    if (ref->stage >= HU_COMPRESS_SINGLE_WORD)
        return false;
    return (uint32_t)ref->usage_count >= min_uses * (uint32_t)ref->stage;
}

hu_error_t hu_compression_build_prompt(hu_allocator_t *alloc, const hu_shared_ref_t *refs,
                                       size_t ref_count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (ref_count == 0 || !refs) {
        const char *empty = "[No shared references with this contact yet]";
        *out = hu_strndup(alloc, empty, strlen(empty));
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = strlen(empty);
        return HU_OK;
    }

    /* Build prompt: each ref as "- "compressed" = expanded (strength: X, used N times)" */
    char *result = NULL;
    size_t result_len = 0;
    const char *header = "[SHARED LANGUAGE with this contact]:\n";
    result = hu_strndup(alloc, header, strlen(header));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    result_len = strlen(header);

    for (size_t i = 0; i < ref_count; i++) {
        const hu_shared_ref_t *r = &refs[i];
        const char *cf = r->compressed_form ? r->compressed_form : "";
        size_t cf_len = r->compressed_form_len;
        const char *em = r->expanded_meaning ? r->expanded_meaning : "";
        size_t em_len = r->expanded_meaning_len;

        char line[512];
        int n = snprintf(line, sizeof(line), "- \"%.*s\" = %.*s (strength: %.2f, used %u times)\n",
                         (int)cf_len, cf, (int)em_len, em, r->strength, r->usage_count);
        if (n < 0 || (size_t)n >= sizeof(line))
            continue;

        char *new_result = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, line);
        hu_str_free(alloc, result);
        if (!new_result)
            return HU_ERR_OUT_OF_MEMORY;
        result = new_result;
        result_len = strlen(result);
    }

    const char *footer = "\nYou may use these naturally when relevant. Never explain them.";
    char *final = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, footer);
    hu_str_free(alloc, result);
    if (!final)
        return HU_ERR_OUT_OF_MEMORY;

    *out = final;
    *out_len = strlen(final);
    return HU_OK;
}

void hu_shared_ref_deinit(hu_allocator_t *alloc, hu_shared_ref_t *ref) {
    if (!alloc || !ref)
        return;
    if (ref->contact_id) {
        hu_str_free(alloc, ref->contact_id);
        ref->contact_id = NULL;
        ref->contact_id_len = 0;
    }
    if (ref->compressed_form) {
        hu_str_free(alloc, ref->compressed_form);
        ref->compressed_form = NULL;
        ref->compressed_form_len = 0;
    }
    if (ref->expanded_meaning) {
        hu_str_free(alloc, ref->expanded_meaning);
        ref->expanded_meaning = NULL;
        ref->expanded_meaning_len = 0;
    }
}
