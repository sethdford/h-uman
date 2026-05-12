/* Init #14 — Public benchmark suite expansion (S1 adapter shims).
 *
 * See include/human/eval_public_suites.h for the public surface and
 * docs/plans/2026-05-11-init-14-public-benchmarks.md for the design.
 *
 * S1 deliberately reuses the existing hu_eval_* runner: each benchmark is
 * a tiny held-out smoke fixture committed under tests/fixtures/benchmarks/
 * <name>/smoke.json with synthetic personas only.  Under HU_IS_TEST the
 * runner uses the mock provider; without HU_IS_TEST the caller's
 * provider is used.  The fixtures are constructed so that an honest
 * scorer with a competent provider produces a high pass rate; the
 * checked-in floor is a small margin below that to catch real
 * regressions while tolerating rounding.
 */

#include "human/eval_public_suites.h"

#include "human/core/error.h"
#include "human/core/string.h"
#include "human/eval.h"
#include "human/provider.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

typedef struct hu_public_benchmark_table_entry {
    hu_public_benchmark_t id;
    const char *name;
    const char *fixture_path;
    double floor;
} hu_public_benchmark_table_entry_t;

/* Per-benchmark metadata. Floors are deliberately set below the
 * deterministic pass rate that the smoke fixture should produce under
 * the mock provider (every task's expected substring appears in its
 * prompt → 1.0 score).  A floor of 0.80 leaves room for adversarial
 * fixture mutations that drop one task, while still catching anything
 * that breaks the loader, runner, or scorer end-to-end. */
static const hu_public_benchmark_table_entry_t k_public_benchmark_table[] = {
    {HU_PUBLIC_BENCHMARK_LONGMEMEVAL, "longmemeval",
     "tests/fixtures/benchmarks/longmemeval/smoke.json", 0.80},
    {HU_PUBLIC_BENCHMARK_LOCOMO, "locomo",
     "tests/fixtures/benchmarks/locomo/smoke.json", 0.80},
    {HU_PUBLIC_BENCHMARK_KNOWU, "knowu",
     "tests/fixtures/benchmarks/knowu/smoke.json", 0.80},
    {HU_PUBLIC_BENCHMARK_EMPA, "empa",
     "tests/fixtures/benchmarks/empa/smoke.json", 0.80},
    {HU_PUBLIC_BENCHMARK_PROAGENTBENCH, "proagentbench",
     "tests/fixtures/benchmarks/proagentbench/smoke.json", 0.80},
};

static const size_t k_public_benchmark_table_count =
    sizeof(k_public_benchmark_table) / sizeof(k_public_benchmark_table[0]);

static const hu_public_benchmark_table_entry_t *
hu_public_benchmark_lookup(hu_public_benchmark_t b) {
    for (size_t i = 0; i < k_public_benchmark_table_count; i++) {
        if (k_public_benchmark_table[i].id == b)
            return &k_public_benchmark_table[i];
    }
    return NULL;
}

const char *hu_public_benchmark_name(hu_public_benchmark_t b) {
    const hu_public_benchmark_table_entry_t *e = hu_public_benchmark_lookup(b);
    return e ? e->name : "";
}

const char *hu_public_benchmark_fixture_path(hu_public_benchmark_t b) {
    const hu_public_benchmark_table_entry_t *e = hu_public_benchmark_lookup(b);
    return e ? e->fixture_path : NULL;
}

double hu_public_benchmark_floor(hu_public_benchmark_t b) {
    const hu_public_benchmark_table_entry_t *e = hu_public_benchmark_lookup(b);
    return e ? e->floor : 0.0;
}

bool hu_public_benchmark_from_string(const char *name, hu_public_benchmark_t *out) {
    if (!name || !out)
        return false;
    for (size_t i = 0; i < k_public_benchmark_table_count; i++) {
        if (strcmp(name, k_public_benchmark_table[i].name) == 0) {
            *out = k_public_benchmark_table[i].id;
            return true;
        }
    }
    return false;
}

/* Privacy scanner: refuse the fixture if it contains real PII anti-patterns.
 *
 * The contract is "no real users" — fixtures must use synthetic markers
 * (user_a, agent_a, example.com, project_alpha …). We veto on:
 *   - SSN-shape digit groups (\d{3}-\d{2}-\d{4})
 *   - Real email domains other than example.com / example.org
 *   - Phone-number-shape digit groups (10 consecutive digits)
 * This is intentionally a regression gate, not a thorough scrubber.
 */
static bool is_ssn_shape(const char *s, size_t len) {
    if (len < 11) return false;
    for (size_t i = 0; i + 11 <= len; i++) {
        if (isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i + 1]) &&
            isdigit((unsigned char)s[i + 2]) && s[i + 3] == '-' &&
            isdigit((unsigned char)s[i + 4]) && isdigit((unsigned char)s[i + 5]) &&
            s[i + 6] == '-' && isdigit((unsigned char)s[i + 7]) &&
            isdigit((unsigned char)s[i + 8]) && isdigit((unsigned char)s[i + 9]) &&
            isdigit((unsigned char)s[i + 10]))
            return true;
    }
    return false;
}

static bool has_long_digit_run(const char *s, size_t len) {
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        if (isdigit((unsigned char)s[i])) {
            run++;
            if (run >= 10) return true;
        } else {
            run = 0;
        }
    }
    return false;
}

static bool has_disallowed_email_domain(const char *s, size_t len) {
    /* Look for '@' followed by non-example domain. */
    for (size_t i = 0; i + 1 < len; i++) {
        if (s[i] != '@') continue;
        size_t j = i + 1;
        size_t domain_start = j;
        while (j < len &&
               (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '-' || s[j] == '_'))
            j++;
        size_t domain_len = j - domain_start;
        if (domain_len == 0) continue;
        if (domain_len >= 11 && strncmp(s + domain_start, "example.com", 11) == 0) continue;
        if (domain_len >= 11 && strncmp(s + domain_start, "example.org", 11) == 0) continue;
        if (domain_len >= 11 && strncmp(s + domain_start, "example.net", 11) == 0) continue;
        return true;
    }
    return false;
}

hu_error_t hu_public_benchmark_check_fixture_privacy(const char *json, size_t json_len) {
    if (!json || json_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (is_ssn_shape(json, json_len))
        return HU_ERR_INVALID_ARGUMENT;
    if (has_disallowed_email_domain(json, json_len))
        return HU_ERR_INVALID_ARGUMENT;
    if (has_long_digit_run(json, json_len))
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (int64_t)time(NULL) * 1000;
}

/* Read entire file into a heap buffer. Caller frees via alloc->free(). */
static hu_error_t slurp(hu_allocator_t *alloc, const char *path, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return HU_ERR_IO; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return HU_ERR_IO; }
    rewind(f);
    char *buf = alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) { fclose(f); return HU_ERR_OUT_OF_MEMORY; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return HU_OK;
}

hu_error_t hu_public_benchmark_run_smoke(hu_allocator_t *alloc, hu_public_benchmark_t b,
                                         hu_provider_t *provider, const char *model,
                                         size_t model_len,
                                         hu_public_benchmark_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    const hu_public_benchmark_table_entry_t *entry = hu_public_benchmark_lookup(b);
    if (!entry)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->floor = entry->floor;

    int64_t t_start = monotonic_ms();

    /* Slurp fixture and scan for PII before parsing — refusing early
     * keeps the privacy contract a property of the loader, not just
     * the tests. */
    char *json = NULL;
    size_t json_len = 0;
    hu_error_t err = slurp(alloc, entry->fixture_path, &json, &json_len);
    if (err != HU_OK)
        return err;

    err = hu_public_benchmark_check_fixture_privacy(json, json_len);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return err;
    }

    hu_eval_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    err = hu_eval_suite_load_json(alloc, json, json_len, &suite);
    alloc->free(alloc->ctx, json, json_len + 1);
    if (err != HU_OK)
        return err;

    hu_eval_run_t run;
    memset(&run, 0, sizeof(run));
    err = hu_eval_run_suite(alloc, provider, model, model_len, &suite, HU_EVAL_CONTAINS, &run);
    hu_eval_suite_free(alloc, &suite);
    if (err != HU_OK) {
        hu_eval_run_free(alloc, &run);
        return err;
    }

    out->name = hu_strdup(alloc, entry->name);
    out->tasks_run = run.results_count;
    out->tasks_passed = run.passed;
    out->score = run.pass_rate;
    out->passed_floor = run.pass_rate >= entry->floor;
    out->elapsed_ms = monotonic_ms() - t_start;

    hu_eval_run_free(alloc, &run);
    if (!out->name) return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}

void hu_public_benchmark_result_free(hu_allocator_t *alloc,
                                     hu_public_benchmark_result_t *r) {
    if (!alloc || !r) return;
    if (r->name) {
        alloc->free(alloc->ctx, r->name, strlen(r->name) + 1);
        r->name = NULL;
    }
}

hu_error_t hu_public_benchmark_result_to_json(hu_allocator_t *alloc,
                                              const hu_public_benchmark_result_t *r,
                                              char **out_json, size_t *out_len) {
    if (!alloc || !r || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"benchmark\":\"%s\",\"tasks_run\":%zu,\"tasks_passed\":%zu,"
                     "\"score\":%.4f,\"floor\":%.4f,\"passed_floor\":%s,"
                     "\"elapsed_ms\":%" PRId64 "}",
                     r->name ? r->name : "", r->tasks_run, r->tasks_passed, r->score,
                     r->floor, r->passed_floor ? "true" : "false", r->elapsed_ms);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    char *copy = alloc->alloc(alloc->ctx, (size_t)n + 1);
    if (!copy) return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, buf, (size_t)n + 1);
    *out_json = copy;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_public_benchmark_publish_results(const char *path, const char *json,
                                               size_t json_len) {
    if (!path || !*path || !json)
        return HU_ERR_INVALID_ARGUMENT;

    /* Atomic write: tmp + fwrite + fflush + fsync + rename. Pattern
     * pinned by tests/test_personal_model_atomic_save.c. */
    char tmp[1024];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(tmp, "wb");
    if (!fp) return HU_ERR_IO;

    if (fwrite(json, 1, json_len, fp) != json_len) {
        fclose(fp);
        (void)remove(tmp);
        return HU_ERR_IO;
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        (void)remove(tmp);
        return HU_ERR_IO;
    }
#if defined(__unix__) || defined(__APPLE__)
    {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            fclose(fp);
            (void)remove(tmp);
            return HU_ERR_IO;
        }
    }
#endif
    if (fclose(fp) != 0) {
        (void)remove(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}
