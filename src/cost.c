#include "seaclaw/cost.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef SC_IS_TEST
#include <sys/stat.h>
#endif

static double sanitize_price(double v) {
    if (isfinite(v) && v > 0.0)
        return v;
    return 0.0;
}

void sc_token_usage_init(sc_cost_entry_t *u, const char *model, uint64_t input_tokens,
                         uint64_t output_tokens, double input_price_per_million,
                         double output_price_per_million) {
    if (!u)
        return;
    memset(u, 0, sizeof(*u));
    u->model = model;
    u->input_tokens = input_tokens;
    u->output_tokens = output_tokens;
    u->total_tokens = input_tokens + output_tokens;
    double inp = sanitize_price(input_price_per_million);
    double out = sanitize_price(output_price_per_million);
    u->cost_usd = (double)input_tokens / 1000000.0 * inp + (double)output_tokens / 1000000.0 * out;
    u->timestamp_secs = (int64_t)time(NULL);
}

sc_error_t sc_cost_tracker_init(sc_cost_tracker_t *t, sc_allocator_t *alloc,
                                const char *workspace_dir, bool enabled, double daily_limit,
                                double monthly_limit, uint32_t warn_percent) {
    if (!t || !alloc || !workspace_dir)
        return SC_ERR_INVALID_ARGUMENT;
    memset(t, 0, sizeof(*t));

    t->alloc = alloc;
    t->enabled = enabled;
    t->daily_limit_usd = daily_limit;
    t->monthly_limit_usd = monthly_limit;
    t->warn_at_percent = warn_percent > 100 ? 100 : warn_percent;

    size_t wlen = strlen(workspace_dir);
    size_t path_len = wlen + 20;
    t->storage_path = (char *)alloc->alloc(alloc->ctx, path_len);
    if (!t->storage_path)
        return SC_ERR_OUT_OF_MEMORY;
    snprintf(t->storage_path, path_len, "%s/state/costs.jsonl", workspace_dir);

    t->history = NULL;
    t->history_count = 0;
    t->history_cap = 0;

    return SC_OK;
}

static void free_record_model(sc_allocator_t *alloc, sc_cost_record_t *r) {
    if (!alloc || !r)
        return;
    if (r->usage.model) {
        alloc->free(alloc->ctx, (void *)r->usage.model, strlen(r->usage.model) + 1);
        r->usage.model = NULL;
    }
}

void sc_cost_tracker_deinit(sc_cost_tracker_t *t) {
    if (!t || !t->alloc)
        return;
    if (t->records) {
        t->alloc->free(t->alloc->ctx, t->records, t->record_cap * sizeof(sc_cost_record_t));
        t->records = NULL;
    }
    if (t->history) {
        for (size_t i = 0; i < t->history_count; i++)
            free_record_model(t->alloc, &t->history[i]);
        t->alloc->free(t->alloc->ctx, t->history, t->history_cap * sizeof(sc_cost_record_t));
        t->history = NULL;
    }
    if (t->storage_path) {
        t->alloc->free(t->alloc->ctx, t->storage_path, strlen(t->storage_path) + 1);
        t->storage_path = NULL;
    }
    t->record_count = 0;
    t->record_cap = 0;
    t->history_count = 0;
    t->history_cap = 0;
}

sc_budget_check_t sc_cost_check_budget(const sc_cost_tracker_t *t, double estimated_cost_usd,
                                       sc_budget_info_t *info_out) {
    if (!t)
        return SC_BUDGET_ALLOWED;
    if (!t->enabled)
        return SC_BUDGET_ALLOWED;
    if (!isfinite(estimated_cost_usd) || estimated_cost_usd < 0.0)
        return SC_BUDGET_ALLOWED;

    double session_cost = sc_cost_session_total(t);
    double projected = session_cost + estimated_cost_usd;

    if (projected > t->daily_limit_usd && info_out) {
        info_out->current_usd = session_cost;
        info_out->limit_usd = t->daily_limit_usd;
        info_out->period = SC_USAGE_PERIOD_DAY;
        return SC_BUDGET_EXCEEDED;
    }
    if (projected > t->monthly_limit_usd && info_out) {
        info_out->current_usd = session_cost;
        info_out->limit_usd = t->monthly_limit_usd;
        info_out->period = SC_USAGE_PERIOD_MONTH;
        return SC_BUDGET_EXCEEDED;
    }
    double warn_threshold = (double)t->warn_at_percent / 100.0;
    double daily_warn = t->daily_limit_usd * warn_threshold;
    if (projected >= daily_warn && info_out) {
        info_out->current_usd = session_cost;
        info_out->limit_usd = t->daily_limit_usd;
        info_out->period = SC_USAGE_PERIOD_DAY;
        return SC_BUDGET_WARNING;
    }
    return SC_BUDGET_ALLOWED;
}

sc_error_t sc_cost_record_usage(sc_cost_tracker_t *t, const sc_cost_entry_t *usage,
                               uint64_t job_id) {
    if (!t || !usage)
        return SC_ERR_INVALID_ARGUMENT;
    if (!t->enabled)
        return SC_OK;
    if (!isfinite(usage->cost_usd) || usage->cost_usd < 0.0)
        return SC_OK;
    if (t->record_count >= SC_COST_MAX_RECORDS)
        return SC_OK;

    if (t->record_count >= t->record_cap) {
        size_t new_cap = t->record_cap ? t->record_cap * 2 : 8;
        if (new_cap > SC_COST_MAX_RECORDS)
            new_cap = SC_COST_MAX_RECORDS;
        sc_cost_record_t *nrec = (sc_cost_record_t *)t->alloc->realloc(
            t->alloc->ctx, t->records, t->record_cap * sizeof(sc_cost_record_t),
            new_cap * sizeof(sc_cost_record_t));
        if (!nrec)
            return SC_ERR_OUT_OF_MEMORY;
        t->records = nrec;
        t->record_cap = new_cap;
    }

    sc_cost_record_t *rec = &t->records[t->record_count];
    rec->usage = *usage;
    if (usage->model) {
        strncpy(rec->model_buf, usage->model, sizeof(rec->model_buf) - 1);
        rec->model_buf[sizeof(rec->model_buf) - 1] = '\0';
    } else {
        rec->model_buf[0] = '\0';
    }
    rec->usage.model = rec->model_buf;
    strncpy(rec->session_id, "current", sizeof(rec->session_id) - 1);
    rec->session_id[sizeof(rec->session_id) - 1] = '\0';
    rec->job_id = job_id;
    t->record_count++;

#ifndef SC_IS_TEST
    if (t->storage_path && t->storage_path[0]) {
        const char *dir_end = strrchr(t->storage_path, '/');
        if (dir_end && dir_end > t->storage_path) {
            size_t dlen = (size_t)(dir_end - t->storage_path);
            char dir_buf[1024];
            if (dlen < sizeof(dir_buf)) {
                memcpy(dir_buf, t->storage_path, dlen);
                dir_buf[dlen] = '\0';
                mkdir(dir_buf, 0755);
            }
        }
        FILE *f = fopen(t->storage_path, "a");
        if (f) {
            fprintf(f,
                    "{\"model\":\"%s\",\"input_tokens\":%llu,\"output_tokens\":%llu,\"cost_usd\":%."
                    "8f,\"timestamp\":%lld,\"session\":\"%s\",\"job_id\":%llu}\n",
                    usage->model ? usage->model : "", (unsigned long long)usage->input_tokens,
                    (unsigned long long)usage->output_tokens, usage->cost_usd,
                    (long long)usage->timestamp_secs, "current", (unsigned long long)job_id);
            fclose(f);
        }
    }
#endif

    return SC_OK;
}

double sc_cost_session_total(const sc_cost_tracker_t *t) {
    if (!t)
        return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < t->record_count; i++) {
        total += t->records[i].usage.cost_usd;
    }
    return total;
}

uint64_t sc_cost_session_tokens(const sc_cost_tracker_t *t) {
    if (!t)
        return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < t->record_count; i++) {
        total += t->records[i].usage.total_tokens;
    }
    return total;
}

size_t sc_cost_request_count(const sc_cost_tracker_t *t) {
    return t ? t->record_count : 0;
}

static void tm_from_secs(int64_t secs, struct tm *out) {
    time_t t = (time_t)secs;
    struct tm *p = localtime(&t);
    if (p)
        *out = *p;
}

static bool same_day(int64_t a_secs, int64_t b_secs) {
    struct tm ta, tb;
    tm_from_secs(a_secs, &ta);
    tm_from_secs(b_secs, &tb);
    return ta.tm_year == tb.tm_year && ta.tm_mon == tb.tm_mon && ta.tm_mday == tb.tm_mday;
}

static bool same_month(int64_t a_secs, int64_t b_secs) {
    struct tm ta, tb;
    tm_from_secs(a_secs, &ta);
    tm_from_secs(b_secs, &tb);
    return ta.tm_year == tb.tm_year && ta.tm_mon == tb.tm_mon;
}

static double sum_cost_for_period(const sc_cost_tracker_t *t, int64_t at_secs,
                                  bool (*pred)(int64_t, int64_t)) {
    double total = 0.0;
    for (size_t i = 0; i < t->record_count; i++) {
        if (pred(t->records[i].usage.timestamp_secs, at_secs))
            total += t->records[i].usage.cost_usd;
    }
    for (size_t i = 0; i < t->history_count; i++) {
        if (pred(t->history[i].usage.timestamp_secs, at_secs))
            total += t->history[i].usage.cost_usd;
    }
    return total;
}

void sc_cost_get_summary(const sc_cost_tracker_t *t, int64_t at_secs, sc_cost_summary_t *out) {
    if (!t || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->session_cost_usd = sc_cost_session_total(t);
    out->request_count = t->record_count;
    out->total_tokens = sc_cost_session_tokens(t);
    if (at_secs == 0)
        at_secs = (int64_t)time(NULL);
    out->daily_cost_usd = sum_cost_for_period(t, at_secs, same_day);
    out->monthly_cost_usd = sum_cost_for_period(t, at_secs, same_month);
    for (size_t i = 0; i < t->history_count; i++) {
        out->total_tokens += t->history[i].usage.total_tokens;
        out->request_count++;
    }
}

#if !defined(SC_IS_TEST)
static sc_error_t parse_cost_line(sc_allocator_t *alloc, const char *line, size_t len,
                                  sc_cost_record_t *out) {
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, line, len, &root);
    if (err != SC_OK)
        return err;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    const char *model = sc_json_get_string(root, "model");
    double cost = sc_json_get_number(root, "cost_usd", 0.0);
    double ts = sc_json_get_number(root, "timestamp", 0.0);
    uint64_t job_id = (uint64_t)sc_json_get_number(root, "job_id", 0.0);
    uint64_t inp = (uint64_t)sc_json_get_number(root, "input_tokens", 0.0);
    uint64_t out_tok = (uint64_t)sc_json_get_number(root, "output_tokens", 0.0);
    uint64_t total = (uint64_t)sc_json_get_number(root, "tokens", 0.0);
    if (total == 0)
        total = inp + out_tok;

    memset(out, 0, sizeof(*out));
    out->usage.model = model ? sc_strdup(alloc, model) : NULL;
    out->usage.input_tokens = inp;
    out->usage.output_tokens = out_tok;
    out->usage.total_tokens = total;
    out->usage.cost_usd = isfinite(cost) ? cost : 0.0;
    out->usage.timestamp_secs = (int64_t)ts;
    strncpy(out->session_id, "history", sizeof(out->session_id) - 1);
    out->session_id[sizeof(out->session_id) - 1] = '\0';
    out->job_id = job_id;

    sc_json_free(alloc, root);
    return SC_OK;
}
#endif

sc_error_t sc_cost_load_history(sc_cost_tracker_t *t) {
    if (!t || !t->alloc)
        return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_IS_TEST
    return SC_OK;
#else
    if (!t->storage_path || !t->storage_path[0])
        return SC_OK;

    FILE *f = fopen(t->storage_path, "r");
    if (!f)
        return SC_OK;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0)
            continue;
        if (t->history_count >= SC_COST_MAX_RECORDS)
            break;

        if (t->history_count >= t->history_cap) {
            size_t new_cap = t->history_cap ? t->history_cap * 2 : 64;
            if (new_cap > SC_COST_MAX_RECORDS)
                new_cap = SC_COST_MAX_RECORDS;
            sc_cost_record_t *n = (sc_cost_record_t *)t->alloc->realloc(
                t->alloc->ctx, t->history, t->history_cap * sizeof(sc_cost_record_t),
                new_cap * sizeof(sc_cost_record_t));
            if (!n) {
                fclose(f);
                return SC_ERR_OUT_OF_MEMORY;
            }
            t->history = n;
            t->history_cap = new_cap;
        }

        sc_cost_record_t rec = {0};
        if (parse_cost_line(t->alloc, line, len, &rec) == SC_OK) {
            t->history[t->history_count++] = rec;
        } else if (rec.usage.model) {
            t->alloc->free(t->alloc->ctx, (void *)rec.usage.model, strlen(rec.usage.model) + 1);
        }
    }
    fclose(f);
    return SC_OK;
#endif
}

sc_error_t sc_cost_get_usage_json(sc_allocator_t *alloc, const sc_cost_tracker_t *t,
                                  int64_t at_secs, char **out_json) {
    if (!alloc || !t || !out_json)
        return SC_ERR_INVALID_ARGUMENT;
    *out_json = NULL;

    sc_cost_summary_t s = {0};
    sc_cost_get_summary(t, at_secs, &s);

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, cap,
                     "{\"session_cost_usd\":%.6f,\"daily_cost_usd\":%.6f,\"monthly_cost_usd\":%.6f,"
                     "\"total_tokens\":%llu,\"request_count\":%zu}",
                     s.session_cost_usd, s.daily_cost_usd, s.monthly_cost_usd,
                     (unsigned long long)s.total_tokens, s.request_count);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return SC_ERR_IO;
    }
    *out_json = buf;
    return SC_OK;
}
