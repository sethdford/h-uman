#include "human/config_mutator.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <unistd.h>
#endif

static const char *ALLOWED_EXACT[] = {
    "default_temperature", "reasoning_effort",
    "memory.backend",      "memory.profile",
    "memory.auto_save",    "memory.consolidation_interval_hours",
    "gateway.host",        "gateway.port",
    "tunnel.provider",     "agents.defaults.model.primary",
};
#define N_EXACT (sizeof(ALLOWED_EXACT) / sizeof(ALLOWED_EXACT[0]))

static const char *ALLOWED_PREFIX[] = {
    "agent.",  "autonomy.",         "browser.", "channels.",  "diagnostics.", "http_request.",
    "memory.", "models.providers.", "runtime.", "scheduler.", "security.",    "session.",
    "tools.",
};
#define N_PREFIX (sizeof(ALLOWED_PREFIX) / sizeof(ALLOWED_PREFIX[0]))

#define CONFIG_MAX_SIZE (1024 * 1024)

#if !defined(HU_IS_TEST) || !HU_IS_TEST
/* Split "a.b.c" into tokens. *out_tokens and *out_buf must be freed by caller. */
static hu_error_t split_path(hu_allocator_t *alloc, const char *path, char ***out_tokens,
                             size_t *out_count, char **out_buf, size_t *out_buf_len) {
    while (*path == ' ' || *path == '\t')
        path++;
    size_t plen = strlen(path);
    while (plen > 0 && (path[plen - 1] == ' ' || path[plen - 1] == '\t'))
        plen--;
    if (plen == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char *buf = (char *)alloc->alloc(alloc->ctx, plen + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, path, plen);
    buf[plen] = '\0';

    size_t count = 1;
    for (size_t i = 0; i < plen; i++) {
        if (buf[i] == '.') {
            buf[i] = '\0';
            count++;
        }
    }

    char **tokens = (char **)alloc->alloc(alloc->ctx, count * sizeof(char *));
    if (!tokens) {
        alloc->free(alloc->ctx, buf, plen + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t idx = 0;
    char *p = buf;
    while (p < buf + plen) {
        if (*p) {
            if (idx >= count)
                break;
            tokens[idx++] = p;
            while (*p)
                p++;
        }
        p++;
    }
    if (idx != count) {
        alloc->free(alloc->ctx, tokens, count * sizeof(char *));
        alloc->free(alloc->ctx, buf, plen + 1);
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_tokens = tokens;
    *out_count = count;
    *out_buf = buf;
    if (out_buf_len)
        *out_buf_len = plen + 1;
    return HU_OK;
}

/* Walk JSON object following path. Returns NULL if any segment missing. */
static hu_json_value_t *value_at_path(const hu_json_value_t *root, char **tokens,
                                      size_t token_count) {
    const hu_json_value_t *cur = root;
    for (size_t i = 0; i < token_count; i++) {
        if (!cur || cur->type != HU_JSON_OBJECT)
            return NULL;
        cur = hu_json_object_get(cur, tokens[i]);
        if (!cur)
            return NULL;
    }
    return (hu_json_value_t *)cur;
}

/* Ensure obj has key, creating empty object if missing. Returns the target (parent of last
 * segment). */
static hu_json_value_t *ensure_and_walk(hu_allocator_t *alloc, hu_json_value_t *root, char **tokens,
                                        size_t token_count) {
    if (token_count == 0)
        return root;
    hu_json_value_t *cur = root;
    for (size_t i = 0; i < token_count - 1; i++) {
        hu_json_value_t *next = hu_json_object_get(cur, tokens[i]);
        if (!next) {
            next = hu_json_object_new(alloc);
            if (!next)
                return NULL;
            hu_error_t err = hu_json_object_set(alloc, cur, tokens[i], next);
            if (err != HU_OK) {
                hu_json_free(alloc, next);
                return NULL;
            }
        }
        cur = next;
    }
    return cur;
}

/* Write content to path, optionally via atomic rename. */
static hu_error_t write_config_file(hu_allocator_t *alloc, const char *path, const char *content,
                                    size_t content_len) {
    size_t tmp_len = strlen(path) + 5;
    char *tmp_path = (char *)alloc->alloc(alloc->ctx, tmp_len);
    if (!tmp_path)
        return HU_ERR_OUT_OF_MEMORY;
    int tn = snprintf(tmp_path, tmp_len, "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= tmp_len) {
        alloc->free(alloc->ctx, tmp_path, tmp_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        alloc->free(alloc->ctx, tmp_path, tmp_len);
        return HU_ERR_IO;
    }
    size_t n = fwrite(content, 1, content_len, f);
    fclose(f);
    if (n != content_len) {
        (void)remove(tmp_path);
        alloc->free(alloc->ctx, tmp_path, tmp_len);
        return HU_ERR_IO;
    }
    if (rename(tmp_path, path) != 0) {
        f = fopen(path, "wb");
        if (f) {
            (void)fwrite(content, 1, content_len, f);
            fclose(f);
        }
        (void)remove(tmp_path);
    }
    alloc->free(alloc->ctx, tmp_path, tmp_len);
    return HU_OK;
}

/* Read config file. Returns content (caller frees), or NULL/empty on missing. */
static hu_error_t read_config_file(hu_allocator_t *alloc, const char *path, char **out_content,
                                   size_t *out_len, bool *existed) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *existed = false;
        char *empty = (char *)alloc->alloc(alloc->ctx, 4);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(empty, "{}\n", 4);
        *out_content = empty;
        *out_len = 3;
        return HU_OK;
    }
    *existed = true;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > CONFIG_MAX_SIZE) {
        fclose(f);
        return sz < 0 ? HU_ERR_IO : HU_ERR_INVALID_ARGUMENT;
    }
    rewind(f);
    size_t n = (size_t)sz;
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t read_n = fread(buf, 1, n, f);
    fclose(f);
    if (read_n != n) {
        alloc->free(alloc->ctx, buf, n + 1);
        return HU_ERR_IO;
    }
    buf[n] = '\0';
    *out_content = buf;
    *out_len = n;
    return HU_OK;
}
#endif

static bool is_allowed_path(const char *path) {
    if (!path || !path[0])
        return false;
    for (size_t i = 0; i < N_EXACT; i++) {
        if (strcmp(path, ALLOWED_EXACT[i]) == 0)
            return true;
    }
    for (size_t i = 0; i < N_PREFIX; i++) {
        size_t plen = strlen(ALLOWED_PREFIX[i]);
        if (strncmp(path, ALLOWED_PREFIX[i], plen) == 0)
            return true;
    }
    return false;
}

void hu_config_mutator_free_result(hu_allocator_t *alloc, hu_mutation_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->path) {
        alloc->free(alloc->ctx, result->path, strlen(result->path) + 1);
        result->path = NULL;
    }
    if (result->old_value_json) {
        alloc->free(alloc->ctx, result->old_value_json, strlen(result->old_value_json) + 1);
        result->old_value_json = NULL;
    }
    if (result->new_value_json) {
        alloc->free(alloc->ctx, result->new_value_json, strlen(result->new_value_json) + 1);
        result->new_value_json = NULL;
    }
    if (result->backup_path) {
        alloc->free(alloc->ctx, result->backup_path, strlen(result->backup_path) + 1);
        result->backup_path = NULL;
    }
}

hu_error_t hu_config_mutator_default_path(hu_allocator_t *alloc, char **out_path) {
    if (!alloc || !out_path)
        return HU_ERR_INVALID_ARGUMENT;
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    size_t n = strlen(home) + 20;
    char *p = (char *)alloc->alloc(alloc->ctx, n);
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;
    int pn = snprintf(p, n, "%s/.human/config.json", home);
    if (pn < 0 || (size_t)pn >= n) {
        alloc->free(alloc->ctx, p, n);
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_path = p;
    return HU_OK;
}

bool hu_config_mutator_path_requires_restart(const char *path) {
    if (!path)
        return false;
    if (strncmp(path, "channels.", 9) == 0)
        return true;
    if (strncmp(path, "runtime.", 8) == 0)
        return true;
    if (strcmp(path, "memory.backend") == 0 || strcmp(path, "memory.profile") == 0)
        return true;
    return false;
}

hu_error_t hu_config_mutator_get_path_value_json(hu_allocator_t *alloc, const char *path,
                                                 char **out_json) {
    if (!alloc || !path || !path[0] || !out_json)
        return HU_ERR_INVALID_ARGUMENT;
    if (!is_allowed_path(path))
        return HU_ERR_PERMISSION_DENIED;
#if defined(HU_IS_TEST)
    /* In test mode: return null for any path to avoid file I/O */
    char *dup = hu_strdup(alloc, "null");
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out_json = dup;
    return HU_OK;
#else
    char *cfg_path = NULL;
    hu_error_t err = hu_config_mutator_default_path(alloc, &cfg_path);
    if (err != HU_OK)
        return err;

    char *content = NULL;
    size_t content_len = 0;
    bool existed = false;
    err = read_config_file(alloc, cfg_path, &content, &content_len, &existed);
    alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
    if (err != HU_OK)
        return err;

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, content, content_len, &root);
    alloc->free(alloc->ctx, content, content_len + 1);
    if (err != HU_OK)
        return err;

    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        root = hu_json_object_new(alloc);
        if (!root)
            return HU_ERR_OUT_OF_MEMORY;
    }

    char **tokens = NULL;
    size_t token_count = 0;
    char *tok_buf = NULL;
    size_t tok_buf_len = 0;
    err = split_path(alloc, path, &tokens, &token_count, &tok_buf, &tok_buf_len);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        return err;
    }

    hu_json_value_t *val = value_at_path(root, tokens, token_count);

    char *json_str = NULL;
    size_t json_len = 0;
    if (val) {
        err = hu_json_stringify(alloc, val, &json_str, &json_len);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            return err;
        }
    } else {
        json_str = hu_strdup(alloc, "null");
        if (!json_str) {
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
    alloc->free(alloc->ctx, tok_buf, tok_buf_len);
    hu_json_free(alloc, root);

    *out_json = json_str;
    return HU_OK;
#endif
}

hu_error_t hu_config_mutator_mutate(hu_allocator_t *alloc, hu_mutation_action_t action,
                                    const char *path, const char *value_raw,
                                    hu_mutation_options_t options, hu_mutation_result_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    /* Trim path */
    while (*path == ' ' || *path == '\t')
        path++;
    size_t plen = strlen(path);
    while (plen > 0 && (path[plen - 1] == ' ' || path[plen - 1] == '\t'))
        plen--;

    if (plen == 0)
        return HU_ERR_INVALID_ARGUMENT;
    char *trimmed = (char *)alloc->alloc(alloc->ctx, plen + 1);
    if (!trimmed)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(trimmed, path, plen);
    trimmed[plen] = '\0';

    if (!is_allowed_path(trimmed)) {
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return HU_ERR_PERMISSION_DENIED;
    }

    if (action == HU_MUTATION_SET && (!value_raw || !value_raw[0])) {
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return HU_ERR_INVALID_ARGUMENT;
    }

#if defined(HU_IS_TEST)
    /* Stub: no file write in test */
    out->path = trimmed;
    out->changed = true;
    out->applied = options.apply;
    out->requires_restart = hu_config_mutator_path_requires_restart(trimmed);
    out->old_value_json = hu_strdup(alloc, "null");
    out->new_value_json = value_raw ? hu_strdup(alloc, value_raw) : hu_strdup(alloc, "null");
    out->backup_path = NULL;
    if (!out->old_value_json || !out->new_value_json) {
        hu_config_mutator_free_result(alloc, out);
        return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
#else
    char *cfg_path = NULL;
    hu_error_t err = hu_config_mutator_default_path(alloc, &cfg_path);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return err;
    }

    char *content = NULL;
    size_t content_len = 0;
    bool existed = false;
    err = read_config_file(alloc, cfg_path, &content, &content_len, &existed);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return err;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, content, content_len, &root);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, content, content_len + 1);
        alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return err;
    }

    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        root = hu_json_object_new(alloc);
        if (!root) {
            alloc->free(alloc->ctx, content, content_len + 1);
            alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
            alloc->free(alloc->ctx, trimmed, plen + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    char **tokens = NULL;
    size_t token_count = 0;
    char *tok_buf = NULL;
    size_t tok_buf_len = 0;
    err = split_path(alloc, trimmed, &tokens, &token_count, &tok_buf, &tok_buf_len);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, content, content_len + 1);
        alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return err;
    }

    hu_json_value_t *old_val = value_at_path(root, tokens, token_count);
    char *old_value_json = NULL;
    size_t old_len = 0;
    if (old_val) {
        err = hu_json_stringify(alloc, old_val, &old_value_json, &old_len);
    } else {
        old_value_json = hu_strdup(alloc, "null");
    }
    if (!old_value_json) {
        alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
        alloc->free(alloc->ctx, tok_buf, tok_buf_len);
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, content, content_len + 1);
        alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    if (action == HU_MUTATION_SET) {
        hu_json_value_t *parsed_val = NULL;
        hu_error_t parse_err = hu_json_parse(alloc, value_raw, strlen(value_raw), &parsed_val);
        if (parse_err != HU_OK) {
            parsed_val = hu_json_string_new(alloc, value_raw, strlen(value_raw));
        }
        if (!parsed_val) {
            alloc->free(alloc->ctx, old_value_json, strlen(old_value_json) + 1);
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, content, content_len + 1);
            alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
            alloc->free(alloc->ctx, trimmed, plen + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_value_t *parent = ensure_and_walk(alloc, root, tokens, token_count);
        if (!parent || parent->type != HU_JSON_OBJECT) {
            hu_json_free(alloc, parsed_val);
            alloc->free(alloc->ctx, old_value_json, strlen(old_value_json) + 1);
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, content, content_len + 1);
            alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
            alloc->free(alloc->ctx, trimmed, plen + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        err = hu_json_object_set(alloc, parent, tokens[token_count - 1], parsed_val);
        if (err != HU_OK) {
            hu_json_free(alloc, parsed_val);
            alloc->free(alloc->ctx, old_value_json, strlen(old_value_json) + 1);
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, content, content_len + 1);
            alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
            alloc->free(alloc->ctx, trimmed, plen + 1);
            return err;
        }
    } else {
        hu_json_value_t *parent =
            (token_count == 1) ? root : value_at_path(root, tokens, token_count - 1);
        if (parent && parent->type == HU_JSON_OBJECT) {
            (void)hu_json_object_remove(alloc, parent, tokens[token_count - 1]);
        }
    }

    hu_json_value_t *new_val = value_at_path(root, tokens, token_count);
    char *new_value_json = NULL;
    size_t new_len = 0;
    if (new_val) {
        err = hu_json_stringify(alloc, new_val, &new_value_json, &new_len);
    } else {
        new_value_json = hu_strdup(alloc, "null");
    }
    if (!new_value_json) {
        alloc->free(alloc->ctx, old_value_json, strlen(old_value_json) + 1);
        alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
        alloc->free(alloc->ctx, tok_buf, tok_buf_len);
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, content, content_len + 1);
        alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
        alloc->free(alloc->ctx, trimmed, plen + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    bool changed = (strcmp(old_value_json, new_value_json) != 0);
    char *backup_path = NULL;

    if (options.apply && changed) {
        char *rendered = NULL;
        size_t rendered_len = 0;
        err = hu_json_stringify(alloc, root, &rendered, &rendered_len);
        if (err != HU_OK || !rendered) {
            alloc->free(alloc->ctx, old_value_json, strlen(old_value_json) + 1);
            alloc->free(alloc->ctx, new_value_json, strlen(new_value_json) + 1);
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, content, content_len + 1);
            alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
            alloc->free(alloc->ctx, trimmed, plen + 1);
            return err;
        }
        size_t rn = rendered_len > 0 ? rendered_len : strlen(rendered);
        if (rn == 0 || rendered[rn - 1] != '\n') {
            char *with_nl = (char *)alloc->alloc(alloc->ctx, rn + 2);
            if (with_nl) {
                memcpy(with_nl, rendered, rn);
                with_nl[rn] = '\n';
                with_nl[rn + 1] = '\0';
                alloc->free(alloc->ctx, rendered, rn + 1);
                rendered = with_nl;
                rn++;
            }
        }
        if (existed) {
            size_t bak_len = strlen(cfg_path) + 5;
            backup_path = (char *)alloc->alloc(alloc->ctx, bak_len);
            if (backup_path) {
                int bkn = snprintf(backup_path, bak_len, "%s.bak", cfg_path);
                if (bkn < 0 || (size_t)bkn >= bak_len) {
                    alloc->free(alloc->ctx, backup_path, bak_len);
                    backup_path = NULL;
                } else {
                    FILE *bak = fopen(backup_path, "wb");
                    if (bak) {
                        (void)fwrite(content, 1, content_len, bak);
                        fclose(bak);
                    }
                }
            }
        }
        err = write_config_file(alloc, cfg_path, rendered, rn);
        alloc->free(alloc->ctx, rendered, strlen(rendered) + 1);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, old_value_json, strlen(old_value_json) + 1);
            alloc->free(alloc->ctx, new_value_json, strlen(new_value_json) + 1);
            if (backup_path)
                alloc->free(alloc->ctx, backup_path, strlen(backup_path) + 1);
            alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
            alloc->free(alloc->ctx, tok_buf, tok_buf_len);
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, content, content_len + 1);
            alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);
            alloc->free(alloc->ctx, trimmed, plen + 1);
            return err;
        }
    }

    alloc->free(alloc->ctx, tokens, token_count * sizeof(char *));
    alloc->free(alloc->ctx, tok_buf, tok_buf_len);
    hu_json_free(alloc, root);
    alloc->free(alloc->ctx, content, content_len + 1);
    alloc->free(alloc->ctx, cfg_path, strlen(cfg_path) + 1);

    out->path = trimmed;
    out->changed = changed;
    out->applied = options.apply && changed;
    out->requires_restart = hu_config_mutator_path_requires_restart(trimmed);
    out->old_value_json = old_value_json;
    out->new_value_json = new_value_json;
    out->backup_path = backup_path;
    return HU_OK;
#endif
}
