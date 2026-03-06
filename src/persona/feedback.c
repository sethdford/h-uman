#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SC_FEEDBACK_PATH_MAX 512

static const char *feedback_dir_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = ".";
    int n = snprintf(buf, cap, "%s/.seaclaw/personas/feedback", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

sc_error_t sc_persona_feedback_record(sc_allocator_t *alloc, const char *persona_name,
                                      size_t persona_name_len,
                                      const sc_persona_feedback_t *feedback) {
    if (!alloc || !persona_name || persona_name_len == 0 || !feedback)
        return SC_ERR_INVALID_ARGUMENT;

#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)persona_name_len;
    (void)feedback;
    return SC_OK;
#else
    char dir_buf[SC_FEEDBACK_PATH_MAX];
    const char *dir = feedback_dir_path(dir_buf, sizeof(dir_buf));
    if (!dir)
        return SC_ERR_INVALID_ARGUMENT;

    char path[SC_FEEDBACK_PATH_MAX];
    int pn =
        snprintf(path, sizeof(path), "%s/%.*s.jsonl", dir, (int)persona_name_len, persona_name);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;

#if defined(__unix__) || defined(__APPLE__)
    /* Ensure feedback directory exists */
    if (mkdir(dir, 0755) != 0) {
        /* Ignore EEXIST */
        if (errno != EEXIST)
            return SC_ERR_IO;
    }
#endif

    sc_json_buf_t buf;
    sc_json_buf_init(&buf, alloc);
    sc_json_buf_append_raw(&buf, "{", 1);
    sc_json_append_key_value(&buf, "channel", 7, feedback->channel ? feedback->channel : "cli",
                             feedback->channel_len > 0 ? feedback->channel_len : 3);
    sc_json_buf_append_raw(&buf, ",", 1);
    sc_json_append_key_value(&buf, "original", 8,
                             feedback->original_response ? feedback->original_response : "",
                             feedback->original_response_len);
    sc_json_buf_append_raw(&buf, ",", 1);
    sc_json_append_key_value(&buf, "corrected", 9,
                             feedback->corrected_response ? feedback->corrected_response : "",
                             feedback->corrected_response_len);
    sc_json_buf_append_raw(&buf, ",", 1);
    sc_json_append_key_value(&buf, "context", 7, feedback->context ? feedback->context : "",
                             feedback->context_len);
    sc_json_buf_append_raw(&buf, ",", 1);
    sc_json_append_key_int(&buf, "ts", 2, (long long)time(NULL));
    sc_json_buf_append_raw(&buf, "}\n", 2);

    if (!buf.ptr || buf.len == 0) {
        sc_json_buf_free(&buf);
        return SC_ERR_OUT_OF_MEMORY;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        sc_json_buf_free(&buf);
        return SC_ERR_IO;
    }
    size_t written = fwrite(buf.ptr, 1, buf.len, f);
    fclose(f);
    sc_json_buf_free(&buf);
    if (written != buf.len)
        return SC_ERR_IO;

    return SC_OK;
#endif
}

sc_error_t sc_persona_feedback_apply(sc_allocator_t *alloc, const char *persona_name,
                                     size_t persona_name_len) {
    if (!alloc || !persona_name || persona_name_len == 0)
        return SC_ERR_INVALID_ARGUMENT;

#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)persona_name_len;
    return SC_OK;
#else
#if defined(__unix__) || defined(__APPLE__)
    char dir_buf[SC_FEEDBACK_PATH_MAX];
    const char *dir = feedback_dir_path(dir_buf, sizeof(dir_buf));
    if (!dir)
        return SC_ERR_INVALID_ARGUMENT;

    char path[SC_FEEDBACK_PATH_MAX];
    int pn =
        snprintf(path, sizeof(path), "%s/%.*s.jsonl", dir, (int)persona_name_len, persona_name);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return SC_OK; /* No feedback file is OK */

    /* Read all lines, parse each as JSON, collect corrections */
    char line_buf[8192];
    sc_persona_t persona = {0};
    sc_error_t load_err = sc_persona_load(alloc, persona_name, persona_name_len, &persona);
    if (load_err != SC_OK) {
        fclose(f);
        return load_err;
    }

    /* For each line: parse, add corrected as example to appropriate channel bank */
    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t line_len = strlen(line_buf);
        while (line_len > 0 && (line_buf[line_len - 1] == '\n' || line_buf[line_len - 1] == '\r'))
            line_buf[--line_len] = '\0';
        if (line_len == 0)
            continue;

        sc_json_value_t *root = NULL;
        if (sc_json_parse(alloc, line_buf, line_len, &root) != SC_OK || !root ||
            root->type != SC_JSON_OBJECT) {
            if (root)
                sc_json_free(alloc, root);
            continue;
        }

        const char *ch = sc_json_get_string(root, "channel");
        const char *orig = sc_json_get_string(root, "original");
        const char *corr = sc_json_get_string(root, "corrected");
        const char *ctx = sc_json_get_string(root, "context");
        sc_json_free(alloc, root);

        if (!corr || !corr[0])
            continue;

        const char *channel = ch && ch[0] ? ch : "cli";
        size_t channel_len = strlen(channel);
        const char *context = ctx && ctx[0] ? ctx : "correction";
        const char *incoming = orig && orig[0] ? orig : "(assistant response)";

        /* Find or create example bank for this channel */
        sc_persona_example_bank_t *bank = NULL;
        for (size_t i = 0; i < persona.example_banks_count; i++) {
            if (persona.example_banks[i].channel &&
                strlen(persona.example_banks[i].channel) == channel_len &&
                memcmp(persona.example_banks[i].channel, channel, channel_len) == 0) {
                bank = &persona.example_banks[i];
                break;
            }
        }

        if (!bank) {
            /* Create new bank - would require realloc of example_banks; keep simple: skip */
            continue;
        }

        /* Append new example */
        size_t n = bank->examples_count;
        sc_persona_example_t *new_examples = (sc_persona_example_t *)alloc->realloc(
            alloc->ctx, bank->examples, n * sizeof(sc_persona_example_t),
            (n + 1) * sizeof(sc_persona_example_t));
        if (!new_examples)
            continue;
        bank->examples = new_examples;
        bank->examples[n].context = sc_strdup(alloc, context);
        bank->examples[n].incoming = sc_strdup(alloc, incoming);
        bank->examples[n].response = sc_strdup(alloc, corr);
        if (bank->examples[n].context && bank->examples[n].incoming && bank->examples[n].response) {
            bank->examples_count++;
        } else {
            if (bank->examples[n].context)
                alloc->free(alloc->ctx, bank->examples[n].context,
                            strlen(bank->examples[n].context) + 1);
            if (bank->examples[n].incoming)
                alloc->free(alloc->ctx, bank->examples[n].incoming,
                            strlen(bank->examples[n].incoming) + 1);
            if (bank->examples[n].response)
                alloc->free(alloc->ctx, bank->examples[n].response,
                            strlen(bank->examples[n].response) + 1);
        }
    }

    fclose(f);

    /* Rewrite example bank files for modified banks */
    {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char base[SC_FEEDBACK_PATH_MAX];
            int bn = snprintf(base, sizeof(base), "%s/.seaclaw/personas/examples/%.*s", home,
                              (int)persona_name_len, persona_name);
            if (bn > 0 && (size_t)bn < sizeof(base)) {
                for (size_t i = 0; i < persona.example_banks_count; i++) {
                    sc_persona_example_bank_t *b = &persona.example_banks[i];
                    if (!b->channel || !b->examples)
                        continue;
                    char ex_path[SC_FEEDBACK_PATH_MAX];
                    int pn2 =
                        snprintf(ex_path, sizeof(ex_path), "%s/%s/examples.json", base, b->channel);
                    if (pn2 <= 0 || (size_t)pn2 >= sizeof(ex_path))
                        continue;
                    char dir_path[SC_FEEDBACK_PATH_MAX];
                    int dn = snprintf(dir_path, sizeof(dir_path), "%s/%s", base, b->channel);
                    if (dn > 0 && (size_t)dn < sizeof(dir_path))
                        (void)mkdir(dir_path, 0755);
                    sc_json_buf_t jbuf;
                    sc_json_buf_init(&jbuf, alloc);
                    sc_json_buf_append_raw(&jbuf, "{\"examples\":[", 12);
                    for (size_t j = 0; j < b->examples_count; j++) {
                        if (j > 0)
                            sc_json_buf_append_raw(&jbuf, ",", 1);
                        sc_json_buf_append_raw(&jbuf, "{\"context\":", 10);
                        sc_json_append_string(
                            &jbuf, b->examples[j].context,
                            b->examples[j].context ? strlen(b->examples[j].context) : 0);
                        sc_json_buf_append_raw(&jbuf, ",\"incoming\":", 11);
                        sc_json_append_string(
                            &jbuf, b->examples[j].incoming,
                            b->examples[j].incoming ? strlen(b->examples[j].incoming) : 0);
                        sc_json_buf_append_raw(&jbuf, ",\"response\":", 12);
                        sc_json_append_string(
                            &jbuf, b->examples[j].response,
                            b->examples[j].response ? strlen(b->examples[j].response) : 0);
                        sc_json_buf_append_raw(&jbuf, "}", 1);
                    }
                    sc_json_buf_append_raw(&jbuf, "]}", 2);
                    if (jbuf.ptr && jbuf.len > 0) {
                        FILE *ef = fopen(ex_path, "wb");
                        if (ef) {
                            (void)fwrite(jbuf.ptr, 1, jbuf.len, ef);
                            fclose(ef);
                        }
                    }
                    sc_json_buf_free(&jbuf);
                }
            }
        }
    }

    (void)unlink(path);
    sc_persona_deinit(alloc, &persona);
    return SC_OK;
#else
    (void)persona_name_len;
    return SC_OK;
#endif
#endif
}
