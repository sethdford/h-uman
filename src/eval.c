#include "human/eval.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/eval/shape.h"
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <errno.h>
#endif
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define EVAL_MAX_TASKS               256
#define HU_EVAL_SUITE_JSON_MAX_BYTES (1024u * 1024u)

static hu_eval_match_mode_t eval_match_mode_from_cstr(const char *mm) {
    if (!mm)
        return HU_EVAL_CONTAINS;
    if (strcmp(mm, "llm_judge") == 0)
        return HU_EVAL_LLM_JUDGE;
    if (strcmp(mm, "exact") == 0)
        return HU_EVAL_EXACT;
    if (strcmp(mm, "numeric_close") == 0)
        return HU_EVAL_NUMERIC_CLOSE;
    return HU_EVAL_CONTAINS;
}

static void eval_free_task_strings(hu_allocator_t *alloc, hu_eval_task_t *t) {
    if (!alloc || !t)
        return;
    if (t->id) {
        alloc->free(alloc->ctx, t->id, strlen(t->id) + 1);
        t->id = NULL;
    }
    if (t->prompt) {
        alloc->free(alloc->ctx, t->prompt, strlen(t->prompt) + 1);
        t->prompt = NULL;
    }
    if (t->expected) {
        alloc->free(alloc->ctx, t->expected, strlen(t->expected) + 1);
        t->expected = NULL;
    }
    if (t->category) {
        alloc->free(alloc->ctx, t->category, strlen(t->category) + 1);
        t->category = NULL;
    }
    if (t->rubric) {
        alloc->free(alloc->ctx, t->rubric, strlen(t->rubric) + 1);
        t->rubric = NULL;
    }
}

static int tolower_c(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool str_case_contains(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hlen)
        return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j = 0;
        while (j < nlen &&
               tolower_c((unsigned char)haystack[i + j]) == tolower_c((unsigned char)needle[j]))
            j++;
        if (j == nlen)
            return true;
    }
    return false;
}

static size_t count_expected_words_in_actual(const char *actual, size_t actual_len,
                                             const char *expected, size_t expected_len) {
    size_t count = 0;
    size_t total_expected = 0;
    const char *p = expected;
    const char *end = expected + expected_len;
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '\0'))
            p++;
        if (p >= end)
            break;
        const char *word_start = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '\0')
            p++;
        size_t word_len = (size_t)(p - word_start);
        if (word_len > 0) {
            total_expected++;
            if (str_case_contains(actual, actual_len, word_start, word_len))
                count++;
        }
    }
    (void)total_expected;
    return count;
}

static size_t count_words(const char *s, size_t len) {
    size_t n = 0;
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '\0'))
            p++;
        if (p >= end)
            break;
        n++;
        while (p < end && !isspace((unsigned char)*p) && *p != '\0')
            p++;
    }
    return n;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static bool extract_bool(const char *obj_start, const char *obj_end, const char *key) {
    const char *k = strstr(obj_start, key);
    if (!k || k >= obj_end)
        return false;
    const char *colon = strchr(k + strlen(key), ':');
    if (!colon || colon >= obj_end)
        return false;
    const char *vs = colon + 1;
    while (vs < obj_end && (*vs == ' ' || *vs == '\t'))
        vs++;
    if (vs + 4 <= obj_end && strncmp(vs, "true", 4) == 0)
        return true;
    return false;
}

static double extract_double(const char *obj_start, const char *obj_end, const char *key) {
    const char *k = strstr(obj_start, key);
    if (!k || k >= obj_end)
        return 0.0;
    const char *colon = strchr(k + strlen(key), ':');
    if (!colon || colon >= obj_end)
        return 0.0;
    const char *vs = colon + 1;
    while (vs < obj_end && (*vs == ' ' || *vs == '\t'))
        vs++;
    double v = 0.0;
    (void)sscanf(vs, "%lf", &v);
    return v;
}
#endif

static void hu_eval_heuristic_judge(const char *actual, size_t actual_len, const char *expected,
                                    size_t expected_len, bool *passed_out, double *score_out) {
    if (str_case_contains(actual, actual_len, expected, expected_len)) {
        *passed_out = true;
        if (score_out)
            *score_out = 1.0;
        return;
    }
    size_t exp_words = count_words(expected, expected_len);
    if (exp_words > 0) {
        size_t matched = count_expected_words_in_actual(actual, actual_len, expected, expected_len);
        *passed_out = (matched >= (exp_words + 1) / 2);
        if (score_out)
            *score_out = *passed_out ? (double)matched / (double)exp_words : 0.0;
    } else {
        *passed_out = false;
        if (score_out)
            *score_out = 0.0;
    }
}

hu_error_t hu_eval_suite_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                   hu_eval_suite_t *out) {
    if (!alloc || !json || !json_len || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->default_match_mode = HU_EVAL_CONTAINS;

    hu_json_value_t *root = NULL;
    hu_error_t perr = hu_json_parse(alloc, json, json_len, &root);
    if (perr != HU_OK || !root)
        return perr != HU_OK ? perr : HU_ERR_INVALID_ARGUMENT;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *nm = hu_json_get_string(root, "name");
    if (nm) {
        out->name = hu_strdup(alloc, nm);
        if (!out->name) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    if (!out->name) {
        out->name = alloc->alloc(alloc->ctx, 5);
        if (!out->name) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(out->name, "eval", 5);
    }

    const char *mm = hu_json_get_string(root, "match_mode");
    if (mm)
        out->default_match_mode = eval_match_mode_from_cstr(mm);

    hu_json_value_t *tasks_val = hu_json_object_get(root, "tasks");
    if (tasks_val && tasks_val->type == HU_JSON_ARRAY) {
        hu_eval_task_t *tasks = alloc->alloc(alloc->ctx, EVAL_MAX_TASKS * sizeof(hu_eval_task_t));
        if (!tasks) {
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, out->name, strlen(out->name) + 1);
            out->name = NULL;
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(tasks, 0, EVAL_MAX_TASKS * sizeof(hu_eval_task_t));
        size_t count = 0;

        for (size_t i = 0; i < tasks_val->data.array.len && count < EVAL_MAX_TASKS; i++) {
            hu_json_value_t *tj = tasks_val->data.array.items[i];
            if (!tj || tj->type != HU_JSON_OBJECT)
                continue;

            hu_eval_task_t *t = &tasks[count];

#define EVAL_TASK_DUP(field, key)                                                        \
    do {                                                                                 \
        const char *sx = hu_json_get_string(tj, key);                                    \
        if (sx) {                                                                        \
            t->field = hu_strdup(alloc, sx);                                             \
            if (!t->field) {                                                             \
                eval_free_task_strings(alloc, t);                                        \
                for (size_t k = 0; k < count; k++)                                       \
                    eval_free_task_strings(alloc, &tasks[k]);                            \
                alloc->free(alloc->ctx, tasks, EVAL_MAX_TASKS * sizeof(hu_eval_task_t)); \
                hu_json_free(alloc, root);                                               \
                alloc->free(alloc->ctx, out->name, strlen(out->name) + 1);               \
                out->name = NULL;                                                        \
                return HU_ERR_OUT_OF_MEMORY;                                             \
            }                                                                            \
        }                                                                                \
    } while (0)

            EVAL_TASK_DUP(id, "id");
            EVAL_TASK_DUP(prompt, "prompt");
            if (t->prompt)
                t->prompt_len = strlen(t->prompt);
            EVAL_TASK_DUP(expected, "expected");
            if (t->expected)
                t->expected_len = strlen(t->expected);
            EVAL_TASK_DUP(category, "category");
            t->difficulty = (int)hu_json_get_number(tj, "difficulty", 0.0);
            t->timeout_ms = (int64_t)hu_json_get_number(tj, "timeout_ms", 0.0);
            if (t->timeout_ms == 0)
                t->timeout_ms = 5000;
            EVAL_TASK_DUP(rubric, "rubric");
            if (t->rubric)
                t->rubric_len = strlen(t->rubric);

            const char *task_mm = hu_json_get_string(tj, "match_mode");
            if (task_mm)
                t->match_mode = eval_match_mode_from_cstr(task_mm);
            else
                t->match_mode = out->default_match_mode;

            count++;
        }
#undef EVAL_TASK_DUP

        out->tasks = tasks;
        out->tasks_count = count;
    }

    const char *dr = hu_json_get_string(root, "default_rubric");
    if (dr) {
        out->default_rubric = hu_strdup(alloc, dr);
        if (!out->default_rubric) {
            hu_eval_suite_free(alloc, out);
            memset(out, 0, sizeof(*out));
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        out->default_rubric_len = strlen(out->default_rubric);
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_eval_suite_load_json_path(hu_allocator_t *alloc, const char *path,
                                        hu_eval_suite_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > (unsigned long)HU_EVAL_SUITE_JSON_MAX_BYTES) {
        fclose(f);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    size_t json_len = (size_t)sz;
    char *json = alloc->alloc(alloc->ctx, json_len + 1);
    if (!json) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t n = fread(json, 1, json_len, f);
    fclose(f);
    if (n != json_len) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return HU_ERR_IO;
    }
    json[json_len] = '\0';
    hu_error_t err = hu_eval_suite_load_json(alloc, json, json_len, out);
    alloc->free(alloc->ctx, json, json_len + 1);
    return err;
}

#if defined(__unix__) || defined(__APPLE__)
static int eval_validate_json_basename_cmp(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

hu_error_t hu_eval_suites_validate_dir(hu_allocator_t *alloc, const char *dir, FILE *diag,
                                       hu_eval_validate_stats_t *out_stats) {
    if (!alloc || !dir || !out_stats)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_stats, 0, sizeof(*out_stats));

    enum { max_json_files = 256, path_max = 1024 };
    DIR *d = opendir(dir);
    if (!d) {
        if (diag)
            fprintf(diag, "eval validate: cannot open %s: %s\n", dir, strerror(errno));
        return HU_ERR_IO;
    }

    char **names = alloc->alloc(alloc->ctx, max_json_files * sizeof(char *));
    if (!names) {
        closedir(d);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t n_names = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcmp(ent->d_name + len - 5, ".json") != 0)
            continue;
        if (n_names >= (size_t)max_json_files)
            break;
        char *copy = hu_strdup(alloc, ent->d_name);
        if (!copy) {
            closedir(d);
            for (size_t i = 0; i < n_names; i++)
                alloc->free(alloc->ctx, names[i], strlen(names[i]) + 1);
            alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
            return HU_ERR_OUT_OF_MEMORY;
        }
        names[n_names++] = copy;
    }
    closedir(d);

    if (n_names == 0) {
        if (diag)
            fprintf(diag, "eval validate: no .json suites in %s\n", dir);
        out_stats->errors++;
        alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
        return HU_OK;
    }

    if (n_names > 1)
        qsort(names, n_names, sizeof(names[0]), eval_validate_json_basename_cmp);

    char **seen_ids = NULL;
    size_t n_seen = 0;
    size_t cap_seen = 0;
    hu_error_t ret = HU_OK;

    for (size_t fi = 0; fi < n_names; fi++) {
        char path[path_max];
        int plen = snprintf(path, sizeof(path), "%s/%s", dir, names[fi]);
        if (plen < 0 || (size_t)plen >= sizeof(path)) {
            if (diag)
                fprintf(diag, "eval validate: path too long for %s/%s\n", dir, names[fi]);
            out_stats->errors++;
            continue;
        }

        hu_eval_suite_t suite = {0};
        hu_error_t le = hu_eval_suite_load_json_path(alloc, path, &suite);
        if (le != HU_OK) {
            if (diag)
                fprintf(diag, "eval validate: %s: load failed (%s)\n", path, hu_error_string(le));
            out_stats->errors++;
            hu_eval_suite_free(alloc, &suite);
            continue;
        }

        if (suite.tasks_count == 0) {
            if (diag)
                fprintf(diag, "eval validate: %s: suite has no tasks\n", path);
            out_stats->errors++;
            hu_eval_suite_free(alloc, &suite);
            continue;
        }

        out_stats->suites_ok++;

        for (size_t ti = 0; ti < suite.tasks_count; ti++) {
            hu_eval_task_t *t = &suite.tasks[ti];
            out_stats->tasks++;

            const char *tid = t->id ? t->id : "";
            if (!t->id || tid[0] == '\0') {
                if (diag)
                    fprintf(diag, "eval validate: %s: task[%zu]: missing non-empty id\n", path, ti);
                out_stats->errors++;
                continue;
            }

            if (!t->prompt || t->prompt[0] == '\0') {
                if (diag)
                    fprintf(diag, "eval validate: %s: task %s: missing non-empty prompt\n", path,
                            tid);
                out_stats->errors++;
                continue;
            }

            bool has_expected = t->expected && t->expected[0] != '\0';
            bool has_rubric = t->rubric && t->rubric[0] != '\0';
            if (!has_expected && !has_rubric) {
                if (diag)
                    fprintf(diag, "eval validate: %s: task %s: need expected or rubric\n", path,
                            tid);
                out_stats->errors++;
                continue;
            }

            bool is_dup = false;
            for (size_t si = 0; si < n_seen; si++) {
                if (strcmp(seen_ids[si], tid) == 0) {
                    is_dup = true;
                    break;
                }
            }
            if (is_dup) {
                if (diag)
                    fprintf(diag, "eval validate: duplicate task id %s (in %s)\n", tid, path);
                out_stats->errors++;
                continue;
            }

            if (n_seen >= cap_seen) {
                size_t ncap = cap_seen ? cap_seen * 2u : 32u;
                size_t old_b = cap_seen * sizeof(char *);
                size_t new_b = ncap * sizeof(char *);
                char **next = alloc->realloc(alloc->ctx, seen_ids, old_b, new_b);
                if (!next) {
                    hu_eval_suite_free(alloc, &suite);
                    ret = HU_ERR_OUT_OF_MEMORY;
                    goto validate_cleanup;
                }
                seen_ids = next;
                cap_seen = ncap;
            }

            char *id_copy = hu_strdup(alloc, tid);
            if (!id_copy) {
                hu_eval_suite_free(alloc, &suite);
                ret = HU_ERR_OUT_OF_MEMORY;
                goto validate_cleanup;
            }
            seen_ids[n_seen++] = id_copy;
        }

        hu_eval_suite_free(alloc, &suite);
    }

validate_cleanup:
    for (size_t i = 0; i < n_seen; i++)
        alloc->free(alloc->ctx, seen_ids[i], strlen(seen_ids[i]) + 1);
    if (seen_ids)
        alloc->free(alloc->ctx, seen_ids, cap_seen * sizeof(char *));
    for (size_t i = 0; i < n_names; i++)
        alloc->free(alloc->ctx, names[i], strlen(names[i]) + 1);
    alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
    return ret;
}
#else
hu_error_t hu_eval_suites_validate_dir(hu_allocator_t *alloc, const char *dir, FILE *diag,
                                       hu_eval_validate_stats_t *out_stats) {
    (void)dir;
    if (!alloc || !out_stats)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_stats, 0, sizeof(*out_stats));
    if (diag)
        fprintf(diag, "eval validate: not supported on this platform\n");
    return HU_ERR_NOT_SUPPORTED;
}
#endif

hu_error_t hu_eval_run_suite(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                             size_t model_len, hu_eval_suite_t *suite, hu_eval_match_mode_t mode,
                             hu_eval_run_t *out) {
    if (!alloc || !suite || !out)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
#else
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system ||
        !provider->vtable->get_name)
        return HU_ERR_INVALID_ARGUMENT;
#endif
    memset(out, 0, sizeof(*out));
    if (suite->tasks_count == 0) {
        out->suite_name =
            suite->name ? hu_strdup(alloc, suite->name) : hu_strndup(alloc, "eval", 4);
        out->provider = hu_strndup(alloc, "test", 4);
        out->model = model && model_len > 0 ? hu_strndup(alloc, model, model_len) : NULL;
        out->results = NULL;
        out->results_count = 0;
        out->passed = 0;
        out->failed = 0;
        out->pass_rate = 1.0;
        return HU_OK;
    }
    out->results = alloc->alloc(alloc->ctx, suite->tasks_count * sizeof(hu_eval_result_t));
    if (!out->results)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out->results, 0, suite->tasks_count * sizeof(hu_eval_result_t));
    out->results_count = suite->tasks_count;

    for (size_t i = 0; i < suite->tasks_count; i++) {
        hu_eval_task_t *task = &suite->tasks[i];
        hu_eval_result_t *res = &out->results[i];
        char *response = NULL;
        size_t response_len = 0;
        int64_t task_start_ms = (int64_t)time(NULL) * 1000;

#if defined(HU_IS_TEST) && HU_IS_TEST
        {
            const char *prefix = "Mock response for: ";
            size_t plen = strlen(prefix);
            size_t prompt_len = task->prompt ? strlen(task->prompt) : 0;
            size_t total = plen + prompt_len + 1;
            response = alloc->alloc(alloc->ctx, total);
            if (response) {
                memcpy(response, prefix, plen);
                if (task->prompt)
                    memcpy(response + plen, task->prompt, prompt_len + 1);
                else
                    response[plen] = '\0';
                response_len = plen + prompt_len;
            }
        }
#else
        {
            /* 2026-05-18 audit fix: pass suite-level system_prompt instead
             * of hardcoded NULL/0. Without this, the LLM receives only the
             * raw task prompt with no persona / channel-overlay /
             * humor-framework / anti-patterns context, producing default
             * "AI assistant offering options" markdown responses that look
             * nothing like Seth's actual iMessage style. With the persona
             * system prompt populated (by cli_commands.c::cmd_eval before
             * the run), the same model produces in-voice 1-sentence texts.
             * Empirical effect: 97% length reduction + 100% markdown
             * elimination across 8 iMessage tasks. See:
             *   scripts/persona_eval_comparison.py
             *   .claude/rules/silent-config-gated-subsystems.md (the
             *   meta-rule: silent NULL defaults are the same failure
             *   class as silent config-gated subsystems). */
            const char *sys = suite->system_prompt ? suite->system_prompt : NULL;
            size_t sys_len = suite->system_prompt ? suite->system_prompt_len : 0;
            hu_error_t err = provider->vtable->chat_with_system(
                provider->ctx, alloc, sys, sys_len, task->prompt ? task->prompt : "",
                task->prompt ? task->prompt_len : 0, model ? model : "", model_len, 0.0, &response,
                &response_len);
            int64_t task_end_ms = (int64_t)time(NULL) * 1000;
            int64_t task_elapsed = task_end_ms - task_start_ms;

            /* Timeout enforcement: if task exceeded timeout_ms, treat as failure */
            if (task->timeout_ms > 0 && task_elapsed > task->timeout_ms) {
                res->task_id = task->id ? hu_strdup(alloc, task->id) : NULL;
                res->passed = false;
                res->score = 0.0;
                res->actual_output = NULL;
                res->actual_output_len = 0;
                res->elapsed_ms = task_elapsed;
                res->tool_calls_made = 0;
                res->tokens_used = 0;
                res->error_msg = hu_sprintf(alloc, "timeout: %lld ms > %lld ms limit",
                                            (long long)task_elapsed, (long long)task->timeout_ms);
                if (response)
                    alloc->free(alloc->ctx, response, response_len + 1);
                out->failed++;
                continue;
            }

            if (err != HU_OK) {
                res->task_id = task->id ? hu_strdup(alloc, task->id) : NULL;
                res->passed = false;
                res->score = 0.0;
                res->actual_output = NULL;
                res->actual_output_len = 0;
                res->elapsed_ms = task_elapsed;
                res->tool_calls_made = 0;
                res->tokens_used = 0;
                res->error_msg = hu_sprintf(alloc, "provider error: %d", (int)err);
                if (response)
                    alloc->free(alloc->ctx, response, response_len + 1);
                out->failed++;
                continue;
            }
        }
#endif

        bool passed = false;
        double score_val = 0.0;
        const char *actual_str = response ? response : "";
        size_t actual_str_len = response ? response_len : 0;
        const char *expected_str = task->expected ? task->expected : "";
        size_t expected_str_len = task->expected ? task->expected_len : 0;
        hu_eval_match_mode_t task_mode = task->match_mode;
        if (task_mode == HU_EVAL_MATCH_INHERIT)
            task_mode = mode;
        else if (task_mode == HU_EVAL_CONTAINS && mode != HU_EVAL_CONTAINS)
            task_mode = mode;

        const char *judge_expected = expected_str;
        size_t judge_expected_len = expected_str_len;
        char *judge_owned __attribute__((unused)) = NULL;
#if !defined(HU_IS_TEST) || !HU_IS_TEST
        if (task_mode == HU_EVAL_LLM_JUDGE && task->rubric && task->rubric_len > 0) {
            if (expected_str_len > 0) {
                size_t gold_cap = expected_str_len < 1200u ? expected_str_len : 1200u;
                judge_owned =
                    hu_sprintf(alloc,
                               "Rubric:\n%.*s\n\nGold reference (equivalent phrasing counts as "
                               "correct):\n%.*s\n",
                               (int)task->rubric_len, task->rubric, (int)gold_cap, expected_str);
            } else {
                judge_owned =
                    hu_sprintf(alloc, "Rubric:\n%.*s\n", (int)task->rubric_len, task->rubric);
            }
            if (judge_owned) {
                judge_expected = judge_owned;
                judge_expected_len = strlen(judge_owned);
            }
        }
#endif
        if (task_mode == HU_EVAL_LLM_JUDGE) {
            hu_eval_check_with_provider(alloc, actual_str, actual_str_len, judge_expected,
                                        judge_expected_len, task_mode, provider, model, model_len,
                                        &passed, &score_val);
        } else {
            hu_eval_check_with_provider(alloc, actual_str, actual_str_len, expected_str,
                                        expected_str_len, task_mode, NULL, NULL, 0, &passed, NULL);
        }
#if !defined(HU_IS_TEST) || !HU_IS_TEST
        if (judge_owned)
            alloc->free(alloc->ctx, judge_owned, strlen(judge_owned) + 1);
#endif

        res->task_id = task->id ? hu_strdup(alloc, task->id) : NULL;
        res->passed = passed;
        res->score = score_val;
        res->actual_output = response;
        res->actual_output_len = response_len;
        res->elapsed_ms = (int64_t)time(NULL) * 1000 - task_start_ms;
        res->tool_calls_made = 0;
        res->tokens_used = 0;
        res->error_msg = NULL;

        /* 2026-05-18 (M4): also compute deterministic shape score
         * alongside the LLM-judge result. Shape classifier is
         * regex-based on the canonical "AI assistant offering options"
         * failure mode — robust to judge noise. Channel derived from
         * the suite name (heuristic): "imessage" / "telegram" / "slack"
         * / "discord" / "email" substring match; falls back to iMessage
         * (strictest) for unknown suites. Persists to eval_results
         * SQLite columns. */
        {
            hu_shape_channel_t shape_ch = HU_SHAPE_CHANNEL_IMESSAGE;
            if (suite->name) {
                if (strstr(suite->name, "telegram"))
                    shape_ch = HU_SHAPE_CHANNEL_TELEGRAM;
                else if (strstr(suite->name, "slack"))
                    shape_ch = HU_SHAPE_CHANNEL_SLACK;
                else if (strstr(suite->name, "discord"))
                    shape_ch = HU_SHAPE_CHANNEL_DISCORD;
                else if (strstr(suite->name, "email"))
                    shape_ch = HU_SHAPE_CHANNEL_EMAIL;
            }
            hu_shape_result_t shape;
            memset(&shape, 0, sizeof(shape));
            (void)hu_shape_classify(response, response_len, shape_ch, &shape);
            res->shape_score = shape.score;
            res->shape_pass = shape.passed;
            res->shape_fails = shape.fail_flags;
        }

        if (passed)
            out->passed++;
        else
            out->failed++;
    }

    out->pass_rate =
        (out->results_count > 0) ? (double)out->passed / (double)out->results_count : 1.0;

    /* 2026-05-18 (B2 fix): aggregate per-task elapsed_ms into
     * total_elapsed_ms. Previously this field was always 0 in reports
     * because no one summed it — the field existed in the struct, was
     * serialized to JSON, persisted to SQLite, and parsed back, but
     * never CALCULATED. Made every eval report read "elapsed_ms: 0"
     * which masked the throughput bottleneck during the persona-eval
     * audit chain. */
    out->total_elapsed_ms = 0;
    for (size_t i = 0; i < out->results_count; i++) {
        if (out->results[i].elapsed_ms > 0)
            out->total_elapsed_ms += out->results[i].elapsed_ms;
    }

    out->suite_name = suite->name ? hu_strdup(alloc, suite->name) : hu_strndup(alloc, "eval", 4);
    if (!out->suite_name) {
        hu_eval_run_free(alloc, out);
        return HU_ERR_OUT_OF_MEMORY;
    }
#if defined(HU_IS_TEST) && HU_IS_TEST
    out->provider = hu_strndup(alloc, "test", 4);
#else
    {
        const char *pname = provider->vtable->get_name(provider->ctx);
        out->provider = pname ? hu_strdup(alloc, pname) : hu_strndup(alloc, "unknown", 7);
    }
#endif
    if (!out->provider) {
        hu_eval_run_free(alloc, out);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->model = (model && model_len > 0) ? hu_strndup(alloc, model, model_len) : NULL;

    return HU_OK;
}

hu_error_t hu_eval_run_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                 hu_eval_run_t *out) {
    if (!alloc || !json || !json_len || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Parse report format: {"suite":"...","passed":N,"failed":N,"pass_rate":X.XX,"elapsed_ms":N} */
    const char *p = json;
    const char *end = json + json_len;
    while (p < end) {
        if (strncmp(p, "\"suite\"", 7) == 0) {
            p = strchr(p + 7, '"');
            if (p) {
                p++;
                const char *ve = strchr(p, '"');
                if (ve && ve - p > 0) {
                    size_t n = (size_t)(ve - p);
                    out->suite_name = alloc->alloc(alloc->ctx, n + 1);
                    if (out->suite_name) {
                        memcpy(out->suite_name, p, n);
                        out->suite_name[n] = '\0';
                    }
                }
            }
            break;
        }
        p++;
    }
    p = json;
    if (strstr(json, "\"passed\"")) {
        const char *k = strstr(json, "\"passed\"");
        if (k) {
            const char *v = strchr(k + 8, ':');
            if (v)
                (void)sscanf(v + 1, "%zu", &out->passed);
        }
    }
    if (strstr(json, "\"failed\"")) {
        const char *k = strstr(json, "\"failed\"");
        if (k) {
            const char *v = strchr(k + 8, ':');
            if (v)
                (void)sscanf(v + 1, "%zu", &out->failed);
        }
    }
    if (strstr(json, "\"pass_rate\"")) {
        const char *k = strstr(json, "\"pass_rate\"");
        if (k) {
            const char *v = strchr(k + 11, ':');
            if (v)
                (void)sscanf(v + 1, "%lf", &out->pass_rate);
        }
    }
    if (strstr(json, "\"elapsed_ms\"")) {
        const char *k = strstr(json, "\"elapsed_ms\"");
        if (k) {
            const char *v = strchr(k + 12, ':');
            if (v)
                (void)sscanf(v + 1, "%" SCNd64, &out->total_elapsed_ms);
        }
    }
    return HU_OK;
}

hu_error_t hu_eval_check_with_provider(hu_allocator_t *alloc, const char *actual, size_t actual_len,
                                       const char *expected, size_t expected_len,
                                       hu_eval_match_mode_t mode, hu_provider_t *provider,
                                       const char *model, size_t model_len, bool *passed,
                                       double *score_out) {
    if (!alloc || !actual || !expected || !passed)
        return HU_ERR_INVALID_ARGUMENT;
    *passed = false;
    if (score_out)
        *score_out = 0.0;

    hu_eval_match_mode_t effective = mode;
    if (effective == HU_EVAL_MATCH_INHERIT)
        effective = HU_EVAL_CONTAINS;

    switch (effective) {
    case HU_EVAL_EXACT:
        *passed = (actual_len == expected_len && memcmp(actual, expected, actual_len) == 0);
        if (score_out)
            *score_out = *passed ? 1.0 : 0.0;
        return HU_OK;
    case HU_EVAL_MATCH_INHERIT:
    case HU_EVAL_CONTAINS: {
        bool found = false;
        if (expected_len <= actual_len) {
            for (size_t i = 0; i <= actual_len - expected_len; i++) {
                if (memcmp(actual + i, expected, expected_len) == 0) {
                    found = true;
                    break;
                }
            }
        }
        *passed = found;
        if (score_out)
            *score_out = found ? 1.0 : 0.0;
        return HU_OK;
    }
    case HU_EVAL_NUMERIC_CLOSE: {
        double a = atof(actual);
        double e = atof(expected);
        *passed = fabs(a - e) < 0.01;
        if (score_out)
            *score_out = *passed ? 1.0 : 0.0;
        return HU_OK;
    }
    case HU_EVAL_LLM_JUDGE: {
#if defined(HU_IS_TEST) && HU_IS_TEST
        (void)provider;
        (void)model;
        (void)model_len;
        hu_eval_heuristic_judge(actual, actual_len, expected, expected_len, passed, score_out);
        return HU_OK;
#else
        if (!provider || !provider->vtable || !provider->vtable->chat_with_system) {
            hu_eval_heuristic_judge(actual, actual_len, expected, expected_len, passed, score_out);
            return HU_OK;
        }
        {
            char *user_msg = hu_sprintf(
                alloc,
                "You are an evaluation judge. Compare the expected answer with the actual answer.\n"
                "Expected: %.*s\nActual: %.*s\n\n"
                "Respond with ONLY a JSON object: {\"pass\": true/false, \"score\": 0.0-1.0, "
                "\"reason\": \"brief explanation\"}",
                (int)expected_len, expected, (int)actual_len, actual);
            if (!user_msg) {
                hu_eval_heuristic_judge(actual, actual_len, expected, expected_len, passed,
                                        score_out);
                return HU_OK;
            }
            const char *sys = "You are a strict evaluation judge. Output only valid JSON.";
            char *response = NULL;
            size_t response_len = 0;
            hu_error_t err = provider->vtable->chat_with_system(
                provider->ctx, alloc, sys, strlen(sys), user_msg, strlen(user_msg),
                model ? model : "", model_len, 0.0, &response, &response_len);
            alloc->free(alloc->ctx, user_msg, strlen(user_msg) + 1);
            if (err != HU_OK || !response) {
                hu_eval_heuristic_judge(actual, actual_len, expected, expected_len, passed,
                                        score_out);
                if (response)
                    alloc->free(alloc->ctx, response, response_len + 1);
                return HU_OK;
            }
            const char *obj_start = response;
            const char *obj_end = response + response_len;
            bool parsed_pass = extract_bool(obj_start, obj_end, "\"pass\"");
            double parsed_score = extract_double(obj_start, obj_end, "\"score\"");
            if (parsed_score < 0.0)
                parsed_score = 0.0;
            if (parsed_score > 1.0)
                parsed_score = 1.0;
            *passed = parsed_pass;
            if (score_out)
                *score_out = parsed_score;
            alloc->free(alloc->ctx, response, response_len + 1);
            return HU_OK;
        }
#endif
    }
    }
    return HU_OK;
}

hu_error_t hu_eval_check(hu_allocator_t *alloc, const char *actual, size_t actual_len,
                         const char *expected, size_t expected_len, hu_eval_match_mode_t mode,
                         bool *passed) {
    return hu_eval_check_with_provider(alloc, actual, actual_len, expected, expected_len, mode,
                                       NULL, NULL, 0, passed, NULL);
}

hu_error_t hu_eval_report_json(hu_allocator_t *alloc, const hu_eval_run_t *run, char **out,
                               size_t *out_len) {
    if (!alloc || !run || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    char buf[1024];
    int n = snprintf(
        buf, sizeof(buf),
        "{\"suite\":\"%s\",\"passed\":%zu,\"failed\":%zu,\"pass_rate\":%.2f,\"elapsed_ms\":%" PRId64
        "}",
        run->suite_name ? run->suite_name : "", run->passed, run->failed, run->pass_rate,
        run->total_elapsed_ms);
    if (n < 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t copy_len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    *out = alloc->alloc(alloc->ctx, copy_len + 1);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, buf, copy_len);
    (*out)[copy_len] = '\0';
    *out_len = copy_len;
    return HU_OK;
}

hu_error_t hu_eval_compare(hu_allocator_t *alloc, const hu_eval_run_t *baseline,
                           const hu_eval_run_t *current, char **report, size_t *report_len) {
    if (!alloc || !baseline || !current || !report || !report_len)
        return HU_ERR_INVALID_ARGUMENT;
    char buf[512];
    double delta = current->pass_rate - baseline->pass_rate;
    int n = snprintf(buf, sizeof(buf), "{\"baseline\":%.2f,\"current\":%.2f,\"delta\":%.2f}",
                     baseline->pass_rate, current->pass_rate, delta);
    if (n < 0)
        return HU_ERR_INVALID_ARGUMENT;
    *report = alloc->alloc(alloc->ctx, (size_t)n + 1);
    if (!*report)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*report, buf, (size_t)n + 1);
    *report_len = (size_t)n;
    return HU_OK;
}

const char *hu_eval_baseline_status_for_score(double score) {
    if (score >= 0.85)
        return "SOTA";
    if (score >= 0.70)
        return "COMPETITIVE";
    if (score >= 0.50)
        return "PARTIAL";
    return "BASIC";
}

bool hu_eval_baseline_try_mock_score_for_stem(const char *suite_stem, double *out_score) {
    if (!suite_stem || !out_score)
        return false;
    static const struct {
        const char *stem;
        double score;
    } k_mock[] = {
        {"fidelity", 0.72}, {"intelligence", 0.65}, {"reasoning", 0.58},
        {"tool_use", 0.70}, {"memory", 0.75},       {"social", 0.68},
    };
    bool use_fixed = false;
#if defined(HU_IS_TEST) && HU_IS_TEST
    use_fixed = true;
#else
    {
        const char *ci = getenv("CI");
        if (ci && ci[0] != '\0' && strcmp(ci, "false") != 0)
            use_fixed = true;
    }
#endif
    if (!use_fixed)
        return false;
    for (size_t i = 0; i < sizeof(k_mock) / sizeof(k_mock[0]); i++) {
        if (strcmp(suite_stem, k_mock[i].stem) == 0) {
            *out_score = k_mock[i].score;
            return true;
        }
    }
    return false;
}

#ifdef HU_ENABLE_SQLITE
#if defined(HU_IS_TEST) && HU_IS_TEST
enum { hu_eval_baseline_test_cap = 64 };
static char hu_eval_baseline_test_names[hu_eval_baseline_test_cap][128];
static double hu_eval_baseline_test_scores[hu_eval_baseline_test_cap];
static size_t hu_eval_baseline_test_tasks[hu_eval_baseline_test_cap];
static size_t hu_eval_baseline_test_n;

static int hu_eval_baseline_test_find(const char *name) {
    for (size_t i = 0; i < hu_eval_baseline_test_n; i++) {
        if (strcmp(hu_eval_baseline_test_names[i], name) == 0)
            return (int)i;
    }
    return -1;
}
#endif

hu_error_t hu_eval_init_tables(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *runs_sql = "CREATE TABLE IF NOT EXISTS eval_runs ("
                           "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                           "suite_name TEXT NOT NULL,"
                           "provider TEXT,"
                           "model TEXT,"
                           "passed INTEGER NOT NULL,"
                           "failed INTEGER NOT NULL,"
                           "pass_rate REAL NOT NULL,"
                           "elapsed_ms INTEGER,"
                           "created_at INTEGER NOT NULL)";
    if (sqlite3_exec(db, runs_sql, NULL, NULL, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    /* Migration: add provider/model columns to existing tables */
    sqlite3_exec(db, "ALTER TABLE eval_runs ADD COLUMN provider TEXT", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE eval_runs ADD COLUMN model TEXT", NULL, NULL, NULL);
    /* 2026-05-18 (M4) migration: shape columns. ALTER returns error
     * silently when column already exists; that's the idempotent path. */
    sqlite3_exec(db, "ALTER TABLE eval_results ADD COLUMN shape_score REAL", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE eval_results ADD COLUMN shape_pass INTEGER", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE eval_results ADD COLUMN shape_fails INTEGER", NULL, NULL, NULL);
    const char *results_sql = "CREATE TABLE IF NOT EXISTS eval_results ("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "run_id INTEGER NOT NULL REFERENCES eval_runs(id),"
                              "task_id TEXT,"
                              "passed INTEGER NOT NULL,"
                              "actual_output TEXT,"
                              "score REAL,"
                              "elapsed_ms INTEGER,"
                              "shape_score REAL,"     /* 2026-05-18 (M4) */
                              "shape_pass INTEGER,"   /* 2026-05-18 (M4) */
                              "shape_fails INTEGER)"; /* 2026-05-18 (M4) */
    if (sqlite3_exec(db, results_sql, NULL, NULL, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    const char *baselines_sql = "CREATE TABLE IF NOT EXISTS eval_baselines ("
                                "suite_name TEXT PRIMARY KEY,"
                                "score REAL NOT NULL,"
                                "task_count INTEGER NOT NULL,"
                                "measured_at TEXT DEFAULT (datetime('now'))"
                                ")";
    if (sqlite3_exec(db, baselines_sql, NULL, NULL, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}

hu_error_t hu_eval_persist_baseline(sqlite3 *db, const char *suite_name, double score,
                                    size_t task_count) {
    if (!suite_name || suite_name[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;
    if (score < 0.0 || score > 1.0 || isnan(score) || isinf(score))
        return HU_ERR_INVALID_ARGUMENT;
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db;
    int ix = hu_eval_baseline_test_find(suite_name);
    if (ix >= 0) {
        hu_eval_baseline_test_scores[(size_t)ix] = score;
        hu_eval_baseline_test_tasks[(size_t)ix] = task_count;
        return HU_OK;
    }
    if (hu_eval_baseline_test_n >= hu_eval_baseline_test_cap)
        return HU_ERR_OUT_OF_MEMORY;
    size_t nl = strlen(suite_name);
    if (nl >= sizeof(hu_eval_baseline_test_names[0]))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(hu_eval_baseline_test_names[hu_eval_baseline_test_n], suite_name, nl + 1);
    hu_eval_baseline_test_scores[hu_eval_baseline_test_n] = score;
    hu_eval_baseline_test_tasks[hu_eval_baseline_test_n] = task_count;
    hu_eval_baseline_test_n++;
    return HU_OK;
#else
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO eval_baselines(suite_name,score,task_count,measured_at) "
        "VALUES(?,?,?,datetime('now')) "
        "ON CONFLICT(suite_name) DO UPDATE SET "
        "score=excluded.score,task_count=excluded.task_count,measured_at=datetime('now')";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, suite_name, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 2, score);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)task_count);
    hu_error_t ret = HU_OK;
    if (sqlite3_step(st) != SQLITE_DONE)
        ret = HU_ERR_MEMORY_BACKEND;
    sqlite3_finalize(st);
    return ret;
#endif
}

hu_error_t hu_eval_get_baseline(sqlite3 *db, const char *suite_name, double *out_score) {
    if (!suite_name || !out_score)
        return HU_ERR_INVALID_ARGUMENT;
    *out_score = 0.0;
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db;
    int ix = hu_eval_baseline_test_find(suite_name);
    if (ix < 0)
        return HU_ERR_NOT_FOUND;
    *out_score = hu_eval_baseline_test_scores[(size_t)ix];
    return HU_OK;
#else
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT score FROM eval_baselines WHERE suite_name=?", -1, &st,
                           NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, suite_name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return HU_ERR_NOT_FOUND;
    }
    *out_score = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return HU_OK;
#endif
}

hu_error_t hu_eval_regression_check_baseline_drop(sqlite3 *db, const char *suite_stem,
                                                  double current_score, double max_drop,
                                                  bool *regressed_out, char *msg_buf,
                                                  size_t msg_cap) {
    if (!suite_stem || suite_stem[0] == '\0' || !regressed_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (current_score < 0.0 || current_score > 1.0 || isnan(current_score) || isinf(current_score))
        return HU_ERR_INVALID_ARGUMENT;
    if (max_drop < 0.0 || isnan(max_drop) || isinf(max_drop))
        return HU_ERR_INVALID_ARGUMENT;
    *regressed_out = false;
    double prev = 0.0;
    hu_error_t ge = hu_eval_get_baseline(db, suite_stem, &prev);
    if (ge == HU_ERR_NOT_FOUND)
        return HU_OK;
    if (ge != HU_OK)
        return ge;
    if (prev < 0.0 || prev > 1.0 || isnan(prev) || isinf(prev))
        return HU_ERR_INTERNAL;
    if (prev - current_score > max_drop + 1e-12) {
        *regressed_out = true;
        if (msg_buf && msg_cap > 0) {
            int n = snprintf(msg_buf, msg_cap, "FAIL: %s dropped %.2f → %.2f (%.2f)", suite_stem,
                             prev, current_score, current_score - prev);
            if (n < 0 || (size_t)n >= msg_cap)
                msg_buf[0] = '\0';
        }
    }
    return HU_OK;
}

hu_error_t hu_eval_store_run(hu_allocator_t *alloc, sqlite3 *db, const hu_eval_run_t *run) {
    if (!alloc || !db || !run)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *ins_run = NULL;
    const char *run_sql = "INSERT INTO "
                          "eval_runs(suite_name,provider,model,passed,failed,pass_rate,elapsed_ms,"
                          "created_at) VALUES(?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, run_sql, -1, &ins_run, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    const char *suite = run->suite_name ? run->suite_name : "";
    sqlite3_bind_text(ins_run, 1, suite, (int)strlen(suite), SQLITE_STATIC);
    if (run->provider)
        sqlite3_bind_text(ins_run, 2, run->provider, (int)strlen(run->provider), SQLITE_STATIC);
    else
        sqlite3_bind_null(ins_run, 2);
    if (run->model)
        sqlite3_bind_text(ins_run, 3, run->model, (int)strlen(run->model), SQLITE_STATIC);
    else
        sqlite3_bind_null(ins_run, 3);
    sqlite3_bind_int(ins_run, 4, (int)run->passed);
    sqlite3_bind_int(ins_run, 5, (int)run->failed);
    sqlite3_bind_double(ins_run, 6, run->pass_rate);
    sqlite3_bind_int64(ins_run, 7, run->total_elapsed_ms);
    sqlite3_bind_int64(ins_run, 8, (int64_t)time(NULL));
    if (sqlite3_step(ins_run) != SQLITE_DONE) {
        sqlite3_finalize(ins_run);
        return HU_ERR_MEMORY_BACKEND;
    }
    int64_t run_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(ins_run);

    if (run->results_count > 0 && run->results) {
        sqlite3_stmt *ins_res = NULL;
        const char *res_sql =
            "INSERT INTO eval_results(run_id,task_id,passed,actual_output,score,elapsed_ms,"
            "shape_score,shape_pass,shape_fails) "
            "VALUES(?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, res_sql, -1, &ins_res, NULL) != SQLITE_OK)
            return HU_ERR_MEMORY_BACKEND;
        for (size_t i = 0; i < run->results_count; i++) {
            const hu_eval_result_t *r = &run->results[i];
            sqlite3_bind_int64(ins_res, 1, run_id);
            sqlite3_bind_text(ins_res, 2, r->task_id ? r->task_id : "",
                              r->task_id ? (int)strlen(r->task_id) : 0, SQLITE_STATIC);
            sqlite3_bind_int(ins_res, 3, r->passed ? 1 : 0);
            if (r->actual_output) {
                sqlite3_bind_text(ins_res, 4, r->actual_output, -1, SQLITE_STATIC);
            } else {
                sqlite3_bind_null(ins_res, 4);
            }
            sqlite3_bind_double(ins_res, 5, r->score);
            sqlite3_bind_int64(ins_res, 6, r->elapsed_ms);
            /* 2026-05-18 (M4): deterministic shape classification fields */
            sqlite3_bind_double(ins_res, 7, r->shape_score);
            sqlite3_bind_int(ins_res, 8, r->shape_pass ? 1 : 0);
            sqlite3_bind_int(ins_res, 9, (int)r->shape_fails);
            if (sqlite3_step(ins_res) != SQLITE_DONE) {
                sqlite3_finalize(ins_res);
                return HU_ERR_MEMORY_BACKEND;
            }
            sqlite3_reset(ins_res);
        }
        sqlite3_finalize(ins_res);
    }
    return HU_OK;
}

hu_error_t hu_eval_load_history(hu_allocator_t *alloc, sqlite3 *db, hu_eval_run_t *runs,
                                size_t max_runs, size_t *out_count) {
    if (!alloc || !db || !runs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id,suite_name,provider,model,passed,failed,pass_rate,elapsed_ms FROM "
                      "eval_runs ORDER BY created_at DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_int64(stmt, 1, (int64_t)max_runs);
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_runs) {
        hu_eval_run_t *r = &runs[count];
        memset(r, 0, sizeof(*r));
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (name) {
            size_t nlen = (size_t)sqlite3_column_bytes(stmt, 1);
            r->suite_name = alloc->alloc(alloc->ctx, nlen + 1);
            if (r->suite_name)
                memcpy(r->suite_name, name, nlen + 1);
        }
        const char *prov = (const char *)sqlite3_column_text(stmt, 2);
        if (prov) {
            size_t plen = (size_t)sqlite3_column_bytes(stmt, 2);
            r->provider = alloc->alloc(alloc->ctx, plen + 1);
            if (r->provider)
                memcpy(r->provider, prov, plen + 1);
        }
        const char *mdl = (const char *)sqlite3_column_text(stmt, 3);
        if (mdl) {
            size_t mlen = (size_t)sqlite3_column_bytes(stmt, 3);
            r->model = alloc->alloc(alloc->ctx, mlen + 1);
            if (r->model)
                memcpy(r->model, mdl, mlen + 1);
        }
        r->passed = (size_t)sqlite3_column_int(stmt, 4);
        r->failed = (size_t)sqlite3_column_int(stmt, 5);
        r->pass_rate = sqlite3_column_double(stmt, 6);
        r->total_elapsed_ms = sqlite3_column_int64(stmt, 7);
        r->results = NULL;
        r->results_count = 0;
        count++;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_eval_detect_regression(sqlite3 *db, const char *suite_name, double current_pass_rate,
                                     double threshold, hu_eval_regression_t *out) {
    if (!db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->current_pass_rate = current_pass_rate;

    sqlite3_stmt *stmt = NULL;
    const char *sql = suite_name
                          ? "SELECT AVG(pass_rate), COUNT(*) FROM (SELECT pass_rate FROM eval_runs "
                            "WHERE suite_name = ? ORDER BY created_at DESC LIMIT 10)"
                          : "SELECT AVG(pass_rate), COUNT(*) FROM (SELECT pass_rate FROM "
                            "eval_runs ORDER BY created_at DESC LIMIT 10)";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    if (suite_name)
        sqlite3_bind_text(stmt, 1, suite_name, (int)strlen(suite_name), SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->baseline_pass_rate = sqlite3_column_double(stmt, 0);
        out->baseline_runs = (size_t)sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);

    if (out->baseline_runs == 0) {
        out->delta = 0.0;
        out->regressed = false;
        return HU_OK;
    }

    out->delta = current_pass_rate - out->baseline_pass_rate;
    out->regressed = (out->delta < -threshold);
    return HU_OK;
}
#endif

void hu_eval_suite_free(hu_allocator_t *alloc, hu_eval_suite_t *suite) {
    if (!alloc || !suite)
        return;
    if (suite->name) {
        alloc->free(alloc->ctx, suite->name, strlen(suite->name) + 1);
        suite->name = NULL;
    }
    if (suite->tasks) {
        for (size_t i = 0; i < suite->tasks_count; i++) {
            hu_eval_task_t *t = &suite->tasks[i];
            if (t->id) {
                alloc->free(alloc->ctx, t->id, strlen(t->id) + 1);
                t->id = NULL;
            }
            if (t->prompt) {
                alloc->free(alloc->ctx, t->prompt, strlen(t->prompt) + 1);
                t->prompt = NULL;
            }
            if (t->expected) {
                alloc->free(alloc->ctx, t->expected, strlen(t->expected) + 1);
                t->expected = NULL;
            }
            if (t->category) {
                alloc->free(alloc->ctx, t->category, strlen(t->category) + 1);
                t->category = NULL;
            }
            if (t->rubric) {
                alloc->free(alloc->ctx, t->rubric, strlen(t->rubric) + 1);
                t->rubric = NULL;
            }
        }
        alloc->free(alloc->ctx, suite->tasks, EVAL_MAX_TASKS * sizeof(hu_eval_task_t));
        suite->tasks = NULL;
        suite->tasks_count = 0;
    }
    if (suite->default_rubric) {
        alloc->free(alloc->ctx, suite->default_rubric, suite->default_rubric_len + 1);
        suite->default_rubric = NULL;
        suite->default_rubric_len = 0;
    }
    /* 2026-05-18: free the persona system prompt set by cli_commands.c
     * before hu_eval_run_suite. Owned by the suite struct. */
    if (suite->system_prompt) {
        alloc->free(alloc->ctx, suite->system_prompt, suite->system_prompt_len + 1);
        suite->system_prompt = NULL;
        suite->system_prompt_len = 0;
    }
}

void hu_eval_result_free(hu_allocator_t *alloc, hu_eval_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->task_id) {
        alloc->free(alloc->ctx, result->task_id, strlen(result->task_id) + 1);
        result->task_id = NULL;
    }
    if (result->actual_output) {
        alloc->free(alloc->ctx, result->actual_output, result->actual_output_len + 1);
        result->actual_output = NULL;
        result->actual_output_len = 0;
    }
    if (result->error_msg) {
        alloc->free(alloc->ctx, result->error_msg, strlen(result->error_msg) + 1);
        result->error_msg = NULL;
    }
}

void hu_eval_run_free(hu_allocator_t *alloc, hu_eval_run_t *run) {
    if (!alloc || !run)
        return;
    if (run->suite_name) {
        alloc->free(alloc->ctx, run->suite_name, strlen(run->suite_name) + 1);
        run->suite_name = NULL;
    }
    if (run->provider) {
        alloc->free(alloc->ctx, run->provider, strlen(run->provider) + 1);
        run->provider = NULL;
    }
    if (run->model) {
        alloc->free(alloc->ctx, run->model, strlen(run->model) + 1);
        run->model = NULL;
    }
    if (run->results) {
        for (size_t i = 0; i < run->results_count; i++)
            hu_eval_result_free(alloc, &run->results[i]);
        alloc->free(alloc->ctx, run->results, run->results_count * sizeof(hu_eval_result_t));
        run->results = NULL;
        run->results_count = 0;
    }
}

/* --- Fidelity dimension scoring (arXiv research-backed) --- */

hu_error_t hu_eval_score_empathy_trajectory(const float *emotional_scores, size_t count,
                                            hu_eval_empathy_trajectory_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!emotional_scores || count < 2)
        return HU_OK;

    /* Directional alignment: fraction of turns where empathy moves toward user needs (increasing)
     */
    size_t positive_moves = 0;
    for (size_t i = 1; i < count; i++) {
        if (emotional_scores[i] >= emotional_scores[i - 1])
            positive_moves++;
    }
    out->directional_alignment = (float)positive_moves / (float)(count - 1);

    /* Cumulative impact: mean empathy score across all turns */
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++)
        sum += emotional_scores[i];
    out->cumulative_impact = sum / (float)count;

    /* Stability: 1 - fraction of regressions (drops > 0.15 after a good turn) */
    size_t regressions = 0;
    for (size_t i = 1; i < count; i++) {
        float drop = emotional_scores[i - 1] - emotional_scores[i];
        if (drop > 0.15f)
            regressions++;
    }
    out->stability = 1.0f - (float)regressions / (float)(count - 1);

    out->composite =
        out->directional_alignment * 0.3f + out->cumulative_impact * 0.4f + out->stability * 0.3f;
    return HU_OK;
}

hu_error_t hu_eval_score_consistency(const float *prompt_sims, size_t prompt_count,
                                     const float *turn_sims, size_t turn_count,
                                     hu_eval_consistency_metrics_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (prompt_sims && prompt_count > 0) {
        float sum = 0.0f;
        for (size_t i = 0; i < prompt_count; i++)
            sum += prompt_sims[i];
        out->prompt_to_line = sum / (float)prompt_count;
    }

    if (turn_sims && turn_count > 0) {
        float sum = 0.0f;
        for (size_t i = 0; i < turn_count; i++)
            sum += turn_sims[i];
        out->line_to_line = sum / (float)turn_count;
    }

    /* Q&A consistency requires separate evaluation pass; default to 0.0 here */
    out->qa_consistency = 0.0f;

    float weights = 0.0f;
    float weighted_sum = 0.0f;
    if (prompt_count > 0) {
        weighted_sum += out->prompt_to_line * 0.4f;
        weights += 0.4f;
    }
    if (turn_count > 0) {
        weighted_sum += out->line_to_line * 0.6f;
        weights += 0.6f;
    }
    out->composite = (weights > 0.0f) ? weighted_sum / weights : 0.0f;
    return HU_OK;
}

float hu_eval_score_antisycophancy(const bool *opinion_held, size_t count) {
    if (!opinion_held || count == 0)
        return 0.0f;
    size_t held = 0;
    for (size_t i = 0; i < count; i++) {
        if (opinion_held[i])
            held++;
    }
    return (float)held / (float)count;
}
