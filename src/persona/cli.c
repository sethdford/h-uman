#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef SC_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define SC_PERSONA_PATH_MAX 512

#if !(defined(SC_IS_TEST) && SC_IS_TEST)
static bool str_in_arr(const char *str, char **arr, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (arr[i] && strcmp(arr[i], str) == 0)
            return true;
    return false;
}

static void diff_scalar(const char *label, const char *old_val, const char *new_val) {
    const char *o = old_val && old_val[0] ? old_val : "(none)";
    const char *n = new_val && new_val[0] ? new_val : "(none)";
    if (strcmp(o, n) == 0)
        return;
    fprintf(stdout, "%s:\n  - \"%s\"\n  + \"%s\"\n", label, o, n);
}

static void diff_arr(const char *label, char **a_arr, size_t a_count, char **b_arr,
                     size_t b_count) {
    bool any = false;
    for (size_t i = 0; i < a_count; i++)
        if (a_arr[i] && !str_in_arr(a_arr[i], b_arr, b_count)) {
            if (!any) {
                fprintf(stdout, "%s:\n", label);
                any = true;
            }
            fprintf(stdout, "  - %s\n", a_arr[i]);
        }
    for (size_t i = 0; i < b_count; i++)
        if (b_arr[i] && !str_in_arr(b_arr[i], a_arr, a_count)) {
            if (!any) {
                fprintf(stdout, "%s:\n", label);
                any = true;
            }
            fprintf(stdout, "  + %s\n", b_arr[i]);
        }
}
#endif

static const char *persona_dir_path(char *buf, size_t cap) {
    return sc_persona_base_dir(buf, cap);
}

sc_error_t sc_persona_cli_parse(int argc, const char **argv, sc_persona_cli_args_t *out) {
    if (!argv || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (argc < 3)
        return SC_ERR_INVALID_ARGUMENT;
    if (strcmp(argv[1], "persona") != 0)
        return SC_ERR_INVALID_ARGUMENT;

    const char *action = argv[2];
    if (strcmp(action, "create") == 0) {
        out->action = SC_PERSONA_ACTION_CREATE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-imessage") == 0)
                out->from_imessage = true;
            else if (strcmp(argv[i], "--from-gmail") == 0) {
                out->from_gmail = true;
                if (i + 1 < argc) {
                    out->gmail_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-facebook") == 0) {
                out->from_facebook = true;
                if (i + 1 < argc) {
                    out->facebook_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-response") == 0 && i + 1 < argc)
                out->response_file = argv[++i];
            else if (strcmp(argv[i], "--interactive") == 0)
                out->interactive = true;
        }
    } else if (strcmp(action, "update") == 0) {
        out->action = SC_PERSONA_ACTION_UPDATE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-imessage") == 0)
                out->from_imessage = true;
            else if (strcmp(argv[i], "--from-gmail") == 0) {
                out->from_gmail = true;
                if (i + 1 < argc) {
                    out->gmail_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-facebook") == 0) {
                out->from_facebook = true;
                if (i + 1 < argc) {
                    out->facebook_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-response") == 0 && i + 1 < argc)
                out->response_file = argv[++i];
            else if (strcmp(argv[i], "--interactive") == 0)
                out->interactive = true;
        }
    } else if (strcmp(action, "show") == 0) {
        out->action = SC_PERSONA_ACTION_SHOW;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "list") == 0) {
        out->action = SC_PERSONA_ACTION_LIST;
    } else if (strcmp(action, "delete") == 0) {
        out->action = SC_PERSONA_ACTION_DELETE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "validate") == 0) {
        out->action = SC_PERSONA_ACTION_VALIDATE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "feedback") == 0) {
        if (argc < 5 || strcmp(argv[3], "apply") != 0)
            return SC_ERR_INVALID_ARGUMENT;
        out->action = SC_PERSONA_ACTION_FEEDBACK_APPLY;
        out->name = argv[4];
    } else if (strcmp(action, "diff") == 0) {
        out->action = SC_PERSONA_ACTION_DIFF;
        if (argc < 5)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        out->diff_name = argv[4];
    } else if (strcmp(action, "export") == 0) {
        out->action = SC_PERSONA_ACTION_EXPORT;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "merge") == 0) {
        out->action = SC_PERSONA_ACTION_MERGE;
        if (argc < 6)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        out->merge_sources = (const char **)(argv + 4);
        out->merge_sources_count = (size_t)(argc - 4);
    } else if (strcmp(action, "import") == 0) {
        out->action = SC_PERSONA_ACTION_IMPORT;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-stdin") == 0)
                out->import_file = NULL;
            else if (strcmp(argv[i], "--from-file") == 0 && i + 1 < argc) {
                out->import_file = argv[i + 1];
                i++;
            }
        }
    } else {
        return SC_ERR_INVALID_ARGUMENT;
    }
    return SC_OK;
}

sc_error_t sc_persona_cli_run(sc_allocator_t *alloc, const sc_persona_cli_args_t *args) {
    if (!alloc || !args)
        return SC_ERR_INVALID_ARGUMENT;

    switch (args->action) {
    case SC_PERSONA_ACTION_SHOW: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for show\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        sc_persona_t p = {0};
        sc_error_t err = sc_persona_load(alloc, args->name, strlen(args->name), &p);
        if (err != SC_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return err;
        }
        char *prompt = NULL;
        size_t prompt_len = 0;
        err = sc_persona_build_prompt(alloc, &p, NULL, 0, &prompt, &prompt_len);
        if (err == SC_OK && prompt) {
            fprintf(stdout, "%s", prompt);
            alloc->free(alloc->ctx, prompt, prompt_len + 1);
        }
        sc_persona_deinit(alloc, &p);
        return err;
    }
    case SC_PERSONA_ACTION_LIST: {
#if defined(__unix__) || defined(__APPLE__)
        char dir_buf[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return SC_ERR_NOT_FOUND;
        }
        DIR *d = opendir(dir);
        if (!d) {
            return SC_OK;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '\0' || e->d_name[0] == '.')
                continue;
            size_t len = strlen(e->d_name);
            if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
                continue;
            char name[256];
            size_t name_len = len - 5;
            if (name_len >= sizeof(name))
                continue;
            memcpy(name, e->d_name, name_len);
            name[name_len] = '\0';
            fprintf(stdout, "%s\n", name);
        }
        closedir(d);
        return SC_OK;
#else
        fprintf(stderr, "Persona list requires POSIX (opendir/readdir)\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_DELETE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for delete\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(__unix__) || defined(__APPLE__)
        char path[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(path, sizeof(path));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return SC_ERR_NOT_FOUND;
        }
        int n = snprintf(path, sizeof(path), "%s/%s.json", dir, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (unlink(path) != 0) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return SC_ERR_NOT_FOUND;
        }
        fprintf(stdout, "Persona deleted: %s\n", args->name);
        return SC_OK;
#else
        fprintf(stderr, "Persona delete requires POSIX (unlink)\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_VALIDATE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for validate\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(SC_IS_TEST) && SC_IS_TEST
        (void)alloc;
        fprintf(stdout, "Persona '%s' is valid.\n", args->name);
        return SC_OK;
#else
#if defined(__unix__) || defined(__APPLE__)
        char base[SC_PERSONA_PATH_MAX];
        if (!sc_persona_base_dir(base, sizeof(base))) {
            fprintf(stderr, "Could not resolve persona directory (HOME or SC_PERSONA_DIR)\n");
            return SC_ERR_NOT_FOUND;
        }
        char path[SC_PERSONA_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s.json", base, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return SC_ERR_NOT_FOUND;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return SC_ERR_IO;
        }
        long sz = ftell(f);
        if (sz < 0 || sz > (long)(1024 * 1024)) {
            fclose(f);
            return sz < 0 ? SC_ERR_IO : SC_ERR_INVALID_ARGUMENT;
        }
        rewind(f);
        char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
        if (!buf) {
            fclose(f);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t read_len = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (read_len != (size_t)sz) {
            alloc->free(alloc->ctx, buf, (size_t)sz + 1);
            return SC_ERR_IO;
        }
        buf[read_len] = '\0';
        char *err_msg = NULL;
        size_t err_len = 0;
        sc_error_t err = sc_persona_validate_json(alloc, buf, read_len, &err_msg, &err_len);
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        if (err != SC_OK) {
            fprintf(stderr, "Persona '%s' is invalid: %s\n", args->name,
                    err_msg ? err_msg : "unknown");
            if (err_msg)
                alloc->free(alloc->ctx, err_msg, err_len + 1);
            return err;
        }
        fprintf(stdout, "Persona '%s' is valid.\n", args->name);
        return SC_OK;
#else
        fprintf(stderr, "Persona validate requires POSIX\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
#endif
    }
    case SC_PERSONA_ACTION_FEEDBACK_APPLY: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for feedback apply\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        sc_error_t err = sc_persona_feedback_apply(alloc, args->name, strlen(args->name));
        if (err != SC_OK) {
            fprintf(stderr, "Failed to apply feedback for persona: %s\n", args->name);
            return err;
        }
        fprintf(stdout, "Feedback applied to persona: %s\n", args->name);
        return SC_OK;
    }
    case SC_PERSONA_ACTION_DIFF: {
        if (!args->name || !args->name[0] || !args->diff_name || !args->diff_name[0]) {
            fprintf(stderr, "Persona diff requires two persona names\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(SC_IS_TEST) && SC_IS_TEST
        (void)alloc;
        return SC_OK;
#else
        sc_persona_t a = {0}, b = {0};
        sc_error_t err = sc_persona_load(alloc, args->name, strlen(args->name), &a);
        if (err != SC_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return err;
        }
        err = sc_persona_load(alloc, args->diff_name, strlen(args->diff_name), &b);
        if (err != SC_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->diff_name);
            sc_persona_deinit(alloc, &a);
            return err;
        }

        diff_scalar("identity", a.identity, b.identity);
        diff_arr("traits", a.traits, a.traits_count, b.traits, b.traits_count);
        diff_arr("preferred_vocab", a.preferred_vocab, a.preferred_vocab_count, b.preferred_vocab,
                 b.preferred_vocab_count);
        diff_arr("avoided_vocab", a.avoided_vocab, a.avoided_vocab_count, b.avoided_vocab,
                 b.avoided_vocab_count);
        diff_arr("slang", a.slang, a.slang_count, b.slang, b.slang_count);
        diff_arr("communication_rules", a.communication_rules, a.communication_rules_count,
                 b.communication_rules, b.communication_rules_count);
        diff_arr("values", a.values, a.values_count, b.values, b.values_count);
        diff_scalar("decision_style", a.decision_style, b.decision_style);

        /* Overlays: compare by channel */
        bool overlay_any = false;
        for (size_t i = 0; i < a.overlays_count; i++) {
            const sc_persona_overlay_t *oa = &a.overlays[i];
            size_t ch_len = oa->channel ? strlen(oa->channel) : 0;
            const sc_persona_overlay_t *ob = sc_persona_find_overlay(&b, oa->channel, ch_len);
            if (!ob) {
                if (!overlay_any) {
                    fprintf(stdout, "overlays:\n");
                    overlay_any = true;
                }
                fprintf(stdout, "  - %s\n", oa->channel ? oa->channel : "(unknown)");
            } else {
                const char *af = oa->formality ? oa->formality : "";
                const char *bf = ob->formality ? ob->formality : "";
                const char *aa = oa->avg_length ? oa->avg_length : "";
                const char *ba = ob->avg_length ? ob->avg_length : "";
                const char *ae = oa->emoji_usage ? oa->emoji_usage : "";
                const char *be = ob->emoji_usage ? ob->emoji_usage : "";
                if (strcmp(af, bf) != 0 || strcmp(aa, ba) != 0 || strcmp(ae, be) != 0) {
                    if (!overlay_any) {
                        fprintf(stdout, "overlays:\n");
                        overlay_any = true;
                    }
                    fprintf(stdout, "  ~ %s (formality/avg_length/emoji changed)\n",
                            oa->channel ? oa->channel : "(unknown)");
                }
            }
        }
        for (size_t i = 0; i < b.overlays_count; i++) {
            const sc_persona_overlay_t *ob = &b.overlays[i];
            size_t ch_len = ob->channel ? strlen(ob->channel) : 0;
            const sc_persona_overlay_t *oa = sc_persona_find_overlay(&a, ob->channel, ch_len);
            if (!oa) {
                if (!overlay_any) {
                    fprintf(stdout, "overlays:\n");
                    overlay_any = true;
                }
                fprintf(stdout, "  + %s\n", ob->channel ? ob->channel : "(unknown)");
            }
        }

        sc_persona_deinit(alloc, &a);
        sc_persona_deinit(alloc, &b);
        return SC_OK;
#endif
    }
    case SC_PERSONA_ACTION_EXPORT: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for export\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(__unix__) || defined(__APPLE__)
        char path[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(path, sizeof(path));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return SC_ERR_NOT_FOUND;
        }
        int n = snprintf(path, sizeof(path), "%s/%s.json", dir, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return SC_ERR_NOT_FOUND;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return SC_ERR_IO;
        }
        long sz = ftell(f);
        if (sz < 0 || sz > (long)(1024 * 1024)) {
            fclose(f);
            return sz < 0 ? SC_ERR_IO : SC_ERR_INVALID_ARGUMENT;
        }
        rewind(f);
        char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
        if (!buf) {
            fclose(f);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t read_len = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (read_len != (size_t)sz) {
            alloc->free(alloc->ctx, buf, (size_t)sz + 1);
            return SC_ERR_IO;
        }
        buf[read_len] = '\0';
        fprintf(stdout, "%s", buf);
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return SC_OK;
#else
        fprintf(stderr, "Persona export requires POSIX\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_MERGE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Output persona name required for merge\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (!args->merge_sources || args->merge_sources_count < 2) {
            fprintf(stderr, "Merge requires at least 2 source personas\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        sc_persona_t *partials = (sc_persona_t *)alloc->alloc(
            alloc->ctx, args->merge_sources_count * sizeof(sc_persona_t));
        if (!partials)
            return SC_ERR_OUT_OF_MEMORY;
        memset(partials, 0, args->merge_sources_count * sizeof(sc_persona_t));
        sc_error_t err = SC_OK;
        for (size_t i = 0; i < args->merge_sources_count; i++) {
            err = sc_persona_load(alloc, args->merge_sources[i], strlen(args->merge_sources[i]),
                                  &partials[i]);
            if (err != SC_OK) {
                fprintf(stderr, "Persona not found: %s\n", args->merge_sources[i]);
                for (size_t j = 0; j < i; j++)
                    sc_persona_deinit(alloc, &partials[j]);
                alloc->free(alloc->ctx, partials, args->merge_sources_count * sizeof(sc_persona_t));
                return err;
            }
        }
        sc_persona_t merged = {0};
        err = sc_persona_creator_synthesize(alloc, partials, args->merge_sources_count, args->name,
                                            strlen(args->name), &merged);
        for (size_t i = 0; i < args->merge_sources_count; i++)
            sc_persona_deinit(alloc, &partials[i]);
        alloc->free(alloc->ctx, partials, args->merge_sources_count * sizeof(sc_persona_t));
        if (err != SC_OK) {
            fprintf(stderr, "Failed to merge personas\n");
            return err;
        }
        err = sc_persona_creator_write(alloc, &merged);
        sc_persona_deinit(alloc, &merged);
        if (err != SC_OK)
            return err;
        fprintf(stdout, "Merged persona created: %s\n", args->name);
        return SC_OK;
    }
    case SC_PERSONA_ACTION_IMPORT: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for import\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        char *json = NULL;
        size_t json_len = 0;
        if (args->import_file) {
            FILE *f = fopen(args->import_file, "rb");
            if (!f) {
                fprintf(stderr, "Could not open file: %s\n", args->import_file);
                return SC_ERR_IO;
            }
            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                return SC_ERR_IO;
            }
            long sz = ftell(f);
            if (sz < 0 || sz > (long)(1024 * 1024)) {
                fclose(f);
                return sz < 0 ? SC_ERR_IO : SC_ERR_INVALID_ARGUMENT;
            }
            rewind(f);
            json = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
            if (!json) {
                fclose(f);
                return SC_ERR_OUT_OF_MEMORY;
            }
            json_len = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[json_len] = '\0';
        } else {
            size_t cap = 64 * 1024;
            json = (char *)alloc->alloc(alloc->ctx, cap);
            if (!json)
                return SC_ERR_OUT_OF_MEMORY;
            size_t pos = 0;
            int c;
            while (pos < cap - 1 && (c = getchar()) != EOF)
                json[pos++] = (char)c;
            json[pos] = '\0';
            json_len = pos;
        }
        char *err_msg = NULL;
        size_t err_len = 0;
        sc_error_t err = sc_persona_validate_json(alloc, json, json_len, &err_msg, &err_len);
        if (err != SC_OK) {
            fprintf(stderr, "Invalid persona JSON: %s\n", err_msg ? err_msg : "unknown");
            if (err_msg)
                alloc->free(alloc->ctx, err_msg, err_len + 1);
            alloc->free(alloc->ctx, json, json_len + 1);
            return err;
        }
#if defined(__unix__) || defined(__APPLE__)
        char dir_buf[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            alloc->free(alloc->ctx, json, json_len + 1);
            return SC_ERR_NOT_FOUND;
        }
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *override = getenv("SC_PERSONA_DIR");
            if (!override || !override[0]) {
                const char *home = getenv("HOME");
                if (home && home[0]) {
                    char parent[SC_PERSONA_PATH_MAX];
                    int pn = snprintf(parent, sizeof(parent), "%s/.seaclaw", home);
                    if (pn > 0 && (size_t)pn < sizeof(parent))
                        (void)mkdir(parent, 0755);
                }
            }
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "Could not create persona directory\n");
                alloc->free(alloc->ctx, json, json_len + 1);
                return SC_ERR_IO;
            }
        }
#endif
        char out_path[SC_PERSONA_PATH_MAX];
        int on = snprintf(out_path, sizeof(out_path), "%s/%s.json", dir, args->name);
        if (on <= 0 || (size_t)on >= sizeof(out_path)) {
            alloc->free(alloc->ctx, json, json_len + 1);
            return SC_ERR_INVALID_ARGUMENT;
        }
        FILE *out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "Could not write persona: %s\n", args->name);
            alloc->free(alloc->ctx, json, json_len + 1);
            return SC_ERR_IO;
        }
        size_t written = fwrite(json, 1, json_len, out);
        fclose(out);
        alloc->free(alloc->ctx, json, json_len + 1);
        if (written != json_len)
            return SC_ERR_IO;
        fprintf(stdout, "Persona imported: %s\n", args->name);
        return SC_OK;
#else
        alloc->free(alloc->ctx, json, json_len + 1);
        fprintf(stderr, "Persona import requires POSIX\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_CREATE:
    case SC_PERSONA_ACTION_UPDATE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for create/update\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (!args->from_imessage && !args->from_gmail && !args->from_facebook &&
            !args->response_file) {
            fprintf(stderr, "No source specified. Use --from-imessage, --from-gmail, "
                            "--from-facebook, or --from-response <path>.\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (args->from_facebook && !args->facebook_export_path) {
            fprintf(stderr, "Facebook export requires a file path. Use: --from-facebook "
                            "/path/to/export.json\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (args->from_gmail && !args->gmail_export_path) {
            fprintf(stderr, "Gmail export requires a file path. Use: --from-gmail "
                            "/path/to/export.json\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(SC_IS_TEST) && SC_IS_TEST
        (void)alloc;
        return SC_OK;
#else
        /* Step 2: --from-response — read AI response, parse, write persona */
        if (args->response_file) {
            FILE *rf = fopen(args->response_file, "rb");
            if (!rf) {
                fprintf(stderr, "Could not open response file: %s\n", args->response_file);
                return SC_ERR_IO;
            }
            if (fseek(rf, 0, SEEK_END) != 0) {
                fclose(rf);
                return SC_ERR_IO;
            }
            long rsz = ftell(rf);
            if (rsz < 0 || rsz > (long)(1024 * 1024)) {
                fclose(rf);
                fprintf(stderr, "Response file too large or invalid\n");
                return SC_ERR_INVALID_ARGUMENT;
            }
            rewind(rf);
            char *response = (char *)alloc->alloc(alloc->ctx, (size_t)rsz + 1);
            if (!response) {
                fclose(rf);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t rlen = fread(response, 1, (size_t)rsz, rf);
            fclose(rf);
            if (rlen != (size_t)rsz) {
                alloc->free(alloc->ctx, response, (size_t)rsz + 1);
                return SC_ERR_IO;
            }
            response[rlen] = '\0';

            sc_persona_t partial = {0};
            sc_error_t perr =
                sc_persona_analyzer_parse_response(alloc, response, rlen, "unknown", 7, &partial);
            alloc->free(alloc->ctx, response, (size_t)rsz + 1);
            if (perr != SC_OK) {
                fprintf(stderr, "Failed to parse AI response\n");
                return perr;
            }
            partial.name = sc_strdup(alloc, args->name);
            if (!partial.name) {
                sc_persona_deinit(alloc, &partial);
                return SC_ERR_OUT_OF_MEMORY;
            }
            partial.name_len = strlen(args->name);

            sc_error_t werr = sc_persona_creator_write(alloc, &partial);
            sc_persona_deinit(alloc, &partial);
            if (werr != SC_OK)
                return werr;
            char dir_buf[SC_PERSONA_PATH_MAX];
            const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
            if (dir)
                fprintf(stdout, "Persona created at %s/%s.json\n", dir, args->name);
            else
                fprintf(stdout, "Persona created at ~/.seaclaw/personas/%s.json\n", args->name);
            return SC_OK;
        }

        /* Step 1: extract messages, build prompt, write to .pending */
        char base[SC_PERSONA_PATH_MAX];
        if (!sc_persona_base_dir(base, sizeof(base))) {
            fprintf(stderr, "Could not resolve persona directory (HOME or SC_PERSONA_DIR)\n");
            return SC_ERR_NOT_FOUND;
        }
        char pending_dir[SC_PERSONA_PATH_MAX];
        int pn = snprintf(pending_dir, sizeof(pending_dir), "%s/.pending", base);
        if (pn <= 0 || (size_t)pn >= sizeof(pending_dir))
            return SC_ERR_INVALID_ARGUMENT;
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *override = getenv("SC_PERSONA_DIR");
            if (!override || !override[0]) {
                const char *home = getenv("HOME");
                if (home && home[0]) {
                    char parent[SC_PERSONA_PATH_MAX];
                    int pp = snprintf(parent, sizeof(parent), "%s/.seaclaw", home);
                    if (pp > 0 && (size_t)pp < sizeof(parent))
                        (void)mkdir(parent, 0755);
                }
            }
            if (mkdir(base, 0755) != 0 && errno != EEXIST)
                return SC_ERR_IO;
            if (mkdir(pending_dir, 0755) != 0 && errno != EEXIST)
                return SC_ERR_IO;
        }
#endif
        bool wrote_prompt = false;
        sc_allocator_t sys = sc_system_allocator();

        if (args->from_imessage) {
#if defined(__APPLE__) && defined(__MACH__) && defined(SC_ENABLE_SQLITE)
            const char *home = getenv("HOME");
            if (!home || !home[0]) {
                fprintf(stderr, "HOME not set\n");
                return SC_ERR_NOT_FOUND;
            }
            char db_path[SC_PERSONA_PATH_MAX];
            int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
            if (n <= 0 || (size_t)n >= sizeof(db_path))
                return SC_ERR_INVALID_ARGUMENT;
            sqlite3 *db = NULL;
            if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
                if (db)
                    sqlite3_close(db);
                fprintf(stderr, "Could not open iMessage chat.db (Full Disk Access required)\n");
                return SC_ERR_IO;
            }
            char query[512];
            size_t query_len = 0;
            sc_error_t qerr =
                sc_persona_sampler_imessage_query(query, sizeof(query), &query_len, 500);
            if (qerr != SC_OK) {
                sqlite3_close(db);
                return qerr;
            }
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
                sqlite3_close(db);
                return SC_ERR_IO;
            }
            char **messages = (char **)alloc->alloc(alloc->ctx, 500 * sizeof(char *));
            if (!messages) {
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t msg_count = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && msg_count < 500) {
                const char *text = (const char *)sqlite3_column_text(stmt, 0);
                if (text && text[0]) {
                    messages[msg_count] = sc_strdup(alloc, text);
                    if (!messages[msg_count]) {
                        for (size_t i = 0; i < msg_count; i++)
                            alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                        alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                        sqlite3_finalize(stmt);
                        sqlite3_close(db);
                        return SC_ERR_OUT_OF_MEMORY;
                    }
                    msg_count++;
                }
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);

            if (msg_count > 0) {
                size_t prompt_cap = 1024 * 1024;
                char *prompt_buf = (char *)alloc->alloc(alloc->ctx, prompt_cap);
                if (!prompt_buf) {
                    for (size_t i = 0; i < msg_count; i++)
                        alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                    alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                    return SC_ERR_OUT_OF_MEMORY;
                }
                size_t prompt_len = 0;
                const char **msg_ptrs =
                    (const char **)alloc->alloc(alloc->ctx, msg_count * sizeof(const char *));
                if (!msg_ptrs) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    for (size_t i = 0; i < msg_count; i++)
                        alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                    alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                    return SC_ERR_OUT_OF_MEMORY;
                }
                for (size_t i = 0; i < msg_count; i++)
                    msg_ptrs[i] = messages[i];
                sc_error_t berr = sc_persona_analyzer_build_prompt(
                    msg_ptrs, msg_count, "imessage", prompt_buf, prompt_cap, &prompt_len);
                alloc->free(alloc->ctx, msg_ptrs, msg_count * sizeof(const char *));
                for (size_t i = 0; i < msg_count; i++)
                    alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                if (berr != SC_OK) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return berr;
                }
                char prompt_path[SC_PERSONA_PATH_MAX];
                int path_n = snprintf(prompt_path, sizeof(prompt_path), "%s/%s_imessage_prompt.txt",
                                      pending_dir, args->name);
                if (path_n <= 0 || (size_t)path_n >= sizeof(prompt_path)) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return SC_ERR_INVALID_ARGUMENT;
                }
                FILE *pf = fopen(prompt_path, "wb");
                if (!pf) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return SC_ERR_IO;
                }
                size_t written = fwrite(prompt_buf, 1, prompt_len, pf);
                fclose(pf);
                alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                if (written != prompt_len)
                    return SC_ERR_IO;
                wrote_prompt = true;
                fprintf(stdout, "Found %zu messages from iMessage\n", msg_count);
            }
#else
            fprintf(stderr, "iMessage sampling requires macOS and SQLite\n");
            return SC_ERR_NOT_SUPPORTED;
#endif
        }
        if (args->from_facebook && args->facebook_export_path) {
            FILE *f = fopen(args->facebook_export_path, "rb");
            if (!f) {
                fprintf(stderr, "Could not open Facebook export: %s\n", args->facebook_export_path);
                return SC_ERR_IO;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > (long)(10 * 1024 * 1024)) {
                fclose(f);
                fprintf(stderr, "Facebook export file too large or empty\n");
                return SC_ERR_INVALID_ARGUMENT;
            }
            char *json = (char *)malloc((size_t)sz + 1);
            if (!json) {
                fclose(f);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t nr = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[nr] = '\0';

            char **messages = NULL;
            size_t msg_count = 0;
            sc_error_t perr = sc_persona_sampler_facebook_parse(json, nr, &messages, &msg_count);
            free(json);
            if (perr != SC_OK) {
                fprintf(stderr, "Failed to parse Facebook export\n");
                return perr;
            }
            if (msg_count > 0 && messages) {
                size_t prompt_cap = 1024 * 1024;
                char *prompt_buf = (char *)alloc->alloc(alloc->ctx, prompt_cap);
                if (!prompt_buf) {
                    for (size_t i = 0; i < msg_count; i++)
                        sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                    sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                    return SC_ERR_OUT_OF_MEMORY;
                }
                size_t prompt_len = 0;
                sc_error_t berr =
                    sc_persona_analyzer_build_prompt((const char **)messages, msg_count, "facebook",
                                                     prompt_buf, prompt_cap, &prompt_len);
                for (size_t i = 0; i < msg_count; i++)
                    sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                if (berr != SC_OK) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return berr;
                }
                char prompt_path[SC_PERSONA_PATH_MAX];
                int path_n = snprintf(prompt_path, sizeof(prompt_path), "%s/%s_facebook_prompt.txt",
                                      pending_dir, args->name);
                if (path_n <= 0 || (size_t)path_n >= sizeof(prompt_path)) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return SC_ERR_INVALID_ARGUMENT;
                }
                FILE *pf = fopen(prompt_path, "wb");
                if (!pf) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return SC_ERR_IO;
                }
                size_t written = fwrite(prompt_buf, 1, prompt_len, pf);
                fclose(pf);
                alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                if (written != prompt_len)
                    return SC_ERR_IO;
                wrote_prompt = true;
                fprintf(stdout, "Found %zu messages from Facebook\n", msg_count);
            }
        }
        if (args->from_gmail && args->gmail_export_path) {
            FILE *f = fopen(args->gmail_export_path, "rb");
            if (!f) {
                fprintf(stderr, "Could not open Gmail export: %s\n", args->gmail_export_path);
                return SC_ERR_IO;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > (long)(10 * 1024 * 1024)) {
                fclose(f);
                fprintf(stderr, "Gmail export file too large or empty\n");
                return SC_ERR_INVALID_ARGUMENT;
            }
            char *json = (char *)malloc((size_t)sz + 1);
            if (!json) {
                fclose(f);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t nr = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[nr] = '\0';

            char **messages = NULL;
            size_t msg_count = 0;
            sc_error_t perr = sc_persona_sampler_gmail_parse(json, nr, &messages, &msg_count);
            free(json);
            if (perr != SC_OK) {
                fprintf(stderr, "Failed to parse Gmail export\n");
                return perr;
            }
            if (msg_count > 0 && messages) {
                size_t prompt_cap = 1024 * 1024;
                char *prompt_buf = (char *)alloc->alloc(alloc->ctx, prompt_cap);
                if (!prompt_buf) {
                    for (size_t i = 0; i < msg_count; i++)
                        sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                    sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                    return SC_ERR_OUT_OF_MEMORY;
                }
                size_t prompt_len = 0;
                sc_error_t berr =
                    sc_persona_analyzer_build_prompt((const char **)messages, msg_count, "gmail",
                                                     prompt_buf, prompt_cap, &prompt_len);
                for (size_t i = 0; i < msg_count; i++)
                    sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                if (berr != SC_OK) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return berr;
                }
                char prompt_path[SC_PERSONA_PATH_MAX];
                int path_n = snprintf(prompt_path, sizeof(prompt_path), "%s/%s_gmail_prompt.txt",
                                      pending_dir, args->name);
                if (path_n <= 0 || (size_t)path_n >= sizeof(prompt_path)) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return SC_ERR_INVALID_ARGUMENT;
                }
                FILE *pf = fopen(prompt_path, "wb");
                if (!pf) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return SC_ERR_IO;
                }
                size_t written = fwrite(prompt_buf, 1, prompt_len, pf);
                fclose(pf);
                alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                if (written != prompt_len)
                    return SC_ERR_IO;
                wrote_prompt = true;
                fprintf(stdout, "Found %zu messages from Gmail\n", msg_count);
            }
        }

        if (!wrote_prompt) {
            fprintf(stderr, "No messages found from any source.\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        fprintf(stdout, "Analysis prompt written to %s\n", pending_dir);
        fprintf(stdout, "Run this prompt through your AI provider, save the response, then run:\n");
        fprintf(stdout, "  seaclaw persona create %s --from-response <path>\n", args->name);
        return SC_OK;
#endif
    }
    }
    return SC_ERR_INVALID_ARGUMENT;
}
