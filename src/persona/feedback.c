#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#define HU_FEEDBACK_PATH_MAX 512

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static const char *feedback_dir_path(char *buf, size_t cap) {
    char base[HU_FEEDBACK_PATH_MAX];
    if (!hu_persona_base_dir(base, sizeof(base)))
        return NULL;
    int n = snprintf(buf, cap, "%s/feedback", base);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}
#endif

hu_error_t hu_persona_feedback_record(hu_allocator_t *alloc, const char *persona_name,
                                      size_t persona_name_len,
                                      const hu_persona_feedback_t *feedback) {
    if (!alloc || !persona_name || persona_name_len == 0 || !feedback)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)persona_name_len;
    (void)feedback;
    return HU_OK;
#else
    char dir_buf[HU_FEEDBACK_PATH_MAX];
    const char *dir = feedback_dir_path(dir_buf, sizeof(dir_buf));
    if (!dir)
        return HU_ERR_INVALID_ARGUMENT;

    char path[HU_FEEDBACK_PATH_MAX];
    int pn =
        snprintf(path, sizeof(path), "%s/%.*s.jsonl", dir, (int)persona_name_len, persona_name);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

#if defined(__unix__) || defined(__APPLE__)
    /* Ensure feedback directory exists */
    if (mkdir(dir, 0755) != 0) {
        /* Ignore EEXIST */
        if (errno != EEXIST)
            return HU_ERR_IO;
    }
#endif

    hu_json_buf_t buf;
    hu_json_buf_init(&buf, alloc);
    hu_json_buf_append_raw(&buf, "{", 1);
    hu_json_append_key_value(&buf, "channel", 7, feedback->channel ? feedback->channel : "cli",
                             feedback->channel_len > 0 ? feedback->channel_len : 3);
    hu_json_buf_append_raw(&buf, ",", 1);
    hu_json_append_key_value(&buf, "original", 8,
                             feedback->original_response ? feedback->original_response : "",
                             feedback->original_response_len);
    hu_json_buf_append_raw(&buf, ",", 1);
    hu_json_append_key_value(&buf, "corrected", 9,
                             feedback->corrected_response ? feedback->corrected_response : "",
                             feedback->corrected_response_len);
    hu_json_buf_append_raw(&buf, ",", 1);
    hu_json_append_key_value(&buf, "context", 7, feedback->context ? feedback->context : "",
                             feedback->context_len);
    hu_json_buf_append_raw(&buf, ",", 1);

    const char *cat = "general";
    size_t cat_len = 7;
    if (feedback->corrected_response && feedback->corrected_response_len > 0) {
        const char *c = feedback->corrected_response;
        size_t cl = feedback->corrected_response_len;
#define FB_HAS(kw) (memmem(c, cl, kw, sizeof(kw) - 1) != NULL)
        if (FB_HAS("listen") || FB_HAS("heard") || FB_HAS("validat"))
            { cat = "listening"; cat_len = 9; }
        else if (FB_HAS("sorry") || FB_HAS("repair") || FB_HAS("misunderst"))
            { cat = "repair"; cat_len = 6; }
        else if (FB_HAS("tone") || FB_HAS("formal") || FB_HAS("casual") ||
                 FB_HAS("mirror"))
            { cat = "mirroring"; cat_len = 9; }
        else if (FB_HAS("conflict") || FB_HAS("pushback") || FB_HAS("boundar"))
            { cat = "conflict"; cat_len = 8; }
        else if (FB_HAS("emoji") || FB_HAS("short") || FB_HAS("long"))
            { cat = "voice"; cat_len = 5; }
#undef FB_HAS
    }
    hu_json_append_key_value(&buf, "category", 8, cat, cat_len);

    hu_json_buf_append_raw(&buf, ",", 1);
    hu_json_append_key_int(&buf, "ts", 2, (long long)time(NULL));
    hu_json_buf_append_raw(&buf, "}\n", 2);

    if (!buf.ptr || buf.len == 0) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    size_t written = fwrite(buf.ptr, 1, buf.len, f);
    fclose(f);
    hu_json_buf_free(&buf);
    if (written != buf.len)
        return HU_ERR_IO;

    return HU_OK;
#endif
}

hu_error_t hu_persona_feedback_apply(hu_allocator_t *alloc, const char *persona_name,
                                     size_t persona_name_len) {
    if (!alloc || !persona_name || persona_name_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)persona_name_len;
    return HU_OK;
#else
#if defined(__unix__) || defined(__APPLE__)
    char dir_buf[HU_FEEDBACK_PATH_MAX];
    const char *dir = feedback_dir_path(dir_buf, sizeof(dir_buf));
    if (!dir)
        return HU_ERR_INVALID_ARGUMENT;

    char path[HU_FEEDBACK_PATH_MAX];
    int pn =
        snprintf(path, sizeof(path), "%s/%.*s.jsonl", dir, (int)persona_name_len, persona_name);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_OK; /* No feedback file is OK */

    /* Read all lines, parse each as JSON, collect corrections */
    char line_buf[8192];
    hu_persona_t persona = {0};
    hu_error_t load_err = hu_persona_load(alloc, persona_name, persona_name_len, &persona);
    if (load_err != HU_OK) {
        fclose(f);
        return load_err;
    }

    bool had_oom = false;

    /* For each line: parse, add corrected as example to appropriate channel bank */
    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t line_len = strlen(line_buf);
        while (line_len > 0 && (line_buf[line_len - 1] == '\n' || line_buf[line_len - 1] == '\r'))
            line_buf[--line_len] = '\0';
        if (line_len == 0)
            continue;

        hu_json_value_t *root = NULL;
        if (hu_json_parse(alloc, line_buf, line_len, &root) != HU_OK || !root ||
            root->type != HU_JSON_OBJECT) {
            if (root)
                hu_json_free(alloc, root);
            continue;
        }

        const char *ch = hu_json_get_string(root, "channel");
        const char *orig = hu_json_get_string(root, "original");
        const char *corr = hu_json_get_string(root, "corrected");
        const char *ctx = hu_json_get_string(root, "context");
        hu_json_free(alloc, root);

        if (!corr || !corr[0])
            continue;

        const char *channel = ch && ch[0] ? ch : "cli";
        size_t channel_len = strlen(channel);
        const char *context = ctx && ctx[0] ? ctx : "correction";
        const char *incoming = orig && orig[0] ? orig : "(assistant response)";

        /* Find or create example bank for this channel */
        hu_persona_example_bank_t *bank = NULL;
        for (size_t i = 0; i < persona.example_banks_count; i++) {
            if (persona.example_banks[i].channel &&
                strlen(persona.example_banks[i].channel) == channel_len &&
                memcmp(persona.example_banks[i].channel, channel, channel_len) == 0) {
                bank = &persona.example_banks[i];
                break;
            }
        }

        if (!bank) {
            size_t bc = persona.example_banks_count;
            hu_persona_example_bank_t *new_banks = (hu_persona_example_bank_t *)alloc->realloc(
                alloc->ctx, persona.example_banks, bc * sizeof(hu_persona_example_bank_t),
                (bc + 1) * sizeof(hu_persona_example_bank_t));
            if (!new_banks) {
                had_oom = true;
                continue;
            }
            persona.example_banks = new_banks;
            memset(&persona.example_banks[bc], 0, sizeof(hu_persona_example_bank_t));
            persona.example_banks[bc].channel = hu_strndup(alloc, channel, channel_len);
            if (!persona.example_banks[bc].channel) {
                had_oom = true;
                continue;
            }
            persona.example_banks_count = bc + 1;
            bank = &persona.example_banks[bc];
        }

        /* Append new example */
        size_t n = bank->examples_count;
        hu_persona_example_t *new_examples = (hu_persona_example_t *)alloc->realloc(
            alloc->ctx, bank->examples, n * sizeof(hu_persona_example_t),
            (n + 1) * sizeof(hu_persona_example_t));
        if (!new_examples) {
            had_oom = true;
            continue;
        }
        bank->examples = new_examples;
        bank->examples[n].context = hu_strdup(alloc, context);
        bank->examples[n].incoming = hu_strdup(alloc, incoming);
        bank->examples[n].response = hu_strdup(alloc, corr);
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
        char base[HU_FEEDBACK_PATH_MAX];
        if (hu_persona_base_dir(base, sizeof(base))) {
            char ex_base[HU_FEEDBACK_PATH_MAX];
            int bn = snprintf(ex_base, sizeof(ex_base), "%s/examples/%.*s", base,
                              (int)persona_name_len, persona_name);
            if (bn > 0 && (size_t)bn < sizeof(ex_base)) {
                for (size_t i = 0; i < persona.example_banks_count; i++) {
                    hu_persona_example_bank_t *b = &persona.example_banks[i];
                    if (!b->channel || !b->examples)
                        continue;
                    char ex_path[HU_FEEDBACK_PATH_MAX];
                    int pn2 = snprintf(ex_path, sizeof(ex_path), "%s/%s/examples.json", ex_base,
                                       b->channel);
                    if (pn2 <= 0 || (size_t)pn2 >= sizeof(ex_path))
                        continue;
                    char dir_path[HU_FEEDBACK_PATH_MAX];
                    int dn = snprintf(dir_path, sizeof(dir_path), "%s/%s", ex_base, b->channel);
                    if (dn > 0 && (size_t)dn < sizeof(dir_path))
                        (void)mkdir(dir_path, 0755);
                    hu_json_buf_t jbuf;
                    hu_json_buf_init(&jbuf, alloc);
                    hu_json_buf_append_raw(&jbuf, "{\"examples\":[", 12);
                    for (size_t j = 0; j < b->examples_count; j++) {
                        if (j > 0)
                            hu_json_buf_append_raw(&jbuf, ",", 1);
                        hu_json_buf_append_raw(&jbuf, "{\"context\":", 10);
                        hu_json_append_string(
                            &jbuf, b->examples[j].context,
                            b->examples[j].context ? strlen(b->examples[j].context) : 0);
                        hu_json_buf_append_raw(&jbuf, ",\"incoming\":", 11);
                        hu_json_append_string(
                            &jbuf, b->examples[j].incoming,
                            b->examples[j].incoming ? strlen(b->examples[j].incoming) : 0);
                        hu_json_buf_append_raw(&jbuf, ",\"response\":", 12);
                        hu_json_append_string(
                            &jbuf, b->examples[j].response,
                            b->examples[j].response ? strlen(b->examples[j].response) : 0);
                        hu_json_buf_append_raw(&jbuf, "}", 1);
                    }
                    hu_json_buf_append_raw(&jbuf, "]}", 2);
                    if (jbuf.ptr && jbuf.len > 0) {
                        FILE *ef = fopen(ex_path, "wb");
                        if (ef) {
                            (void)fwrite(jbuf.ptr, 1, jbuf.len, ef);
                            fclose(ef);
                        }
                    }
                    hu_json_buf_free(&jbuf);
                }
            }
        }
    }

    if (!had_oom)
        (void)unlink(path);
    hu_persona_deinit(alloc, &persona);
    return had_oom ? HU_ERR_OUT_OF_MEMORY : HU_OK;
#else
    (void)persona_name_len;
    return HU_OK;
#endif
#endif
}
