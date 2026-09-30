#include "human/config_mutator.h"
#include "human/config.h"
#include "human/core/arena.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/core/paths.h"
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
    "workspace",
};
#define N_EXACT (sizeof(ALLOWED_EXACT) / sizeof(ALLOWED_EXACT[0]))

static const char *ALLOWED_PREFIX[] = {
    "agent.",  "autonomy.",         "browser.", "channels.",  "diagnostics.", "http_request.",
    "memory.", "models.providers.", "runtime.", "scheduler.", "security.",    "session.",
    "tools.",
};
#define N_PREFIX (sizeof(ALLOWED_PREFIX) / sizeof(ALLOWED_PREFIX[0]))

#define CONFIG_MAX_SIZE ((size_t)1024 * 1024)

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
        alloc->free(alloc->ctx, (void *)tokens, count * sizeof(char *));
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

/* Write content to path atomically (0600: config files may hold api_keys).
 * The only config.json writer, so no path can truncate it in place or
 * report success for a write that did not land. */
static hu_error_t write_config_file(hu_allocator_t *alloc, const char *path, const char *content,
                                    size_t content_len) {
    (void)alloc;
    return hu_io_secure_write_atomic(path, HU_IO_PERM_SECRET, content, content_len);
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
    /* Sized by the resolved path, not by $HOME: $HU_STATE_DIR can be longer. */
    char buf[4096];
    int n = hu_paths_state_or(buf, sizeof(buf), ".", "config.json");
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    char *p = hu_strdup(alloc, buf);
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;
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

/* Trim `path`, check it against the allowlist, and require a value for SET.
 * On success *out_trimmed is a heap copy the caller frees (strlen + 1). */
static hu_error_t prepare_path(hu_allocator_t *alloc, hu_mutation_action_t action, const char *path,
                               const char *value_raw, char **out_trimmed) {
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
    *out_trimmed = trimmed;
    return HU_OK;
}

static void free_str(hu_allocator_t *alloc, char *s) {
    if (s)
        alloc->free(alloc->ctx, s, strlen(s) + 1);
}

hu_error_t hu_config_mutator_build_patch(hu_allocator_t *alloc, const char *path,
                                         const char *value_json, char **out, size_t *out_len) {
    if (!alloc || !path || !value_json || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    char **tokens = NULL;
    size_t count = 0;
    char *tok_buf = NULL;
    size_t tok_buf_len = 0;
    hu_error_t err = split_path(alloc, path, &tokens, &count, &tok_buf, &tok_buf_len);
    if (err != HU_OK)
        return err;
    hu_json_value_t *root = hu_json_object_new(alloc);
    hu_json_value_t *value = NULL;
    if (!root) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto done;
    }
    err = hu_json_parse(alloc, value_json, strlen(value_json), &value);
    if (err != HU_OK)
        goto done;
    hu_json_value_t *parent = ensure_and_walk(alloc, root, tokens, count);
    if (!parent) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto done;
    }
    err = hu_json_object_set(alloc, parent, tokens[count - 1], value);
    if (err != HU_OK)
        goto done;
    value = NULL; /* owned by root */
    err = hu_json_stringify(alloc, root, out, out_len);
done:
    if (value)
        hu_json_free(alloc, value);
    if (root)
        hu_json_free(alloc, root);
    alloc->free(alloc->ctx, (void *)tokens, count * sizeof(char *));
    alloc->free(alloc->ctx, tok_buf, tok_buf_len);
    return err;
}

/* A write must be a document the parser accepts: `doc` is parsed over the
 * default config, and its keys and value types pass the strict document
 * check. For a single-key mutation `doc` is the patch, not the whole file, so
 * keys already in the file cannot block an unrelated edit. Whole-config
 * judgments (provider, memory backend) are deliberately not applied: they
 * would be passed on the scratch config's defaults, not on what was written,
 * and a minimal build's default backend failed them for every write. */
static hu_error_t validate_config_doc(hu_allocator_t *alloc, const char *doc, size_t doc_len) {
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, doc, doc_len, &root);
    if (err != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_arena_t *arena = hu_arena_create(*alloc);
    if (!arena) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_config_t scratch;
    memset(&scratch, 0, sizeof(scratch));
    hu_allocator_t arena_alloc = hu_arena_allocator(arena);
    hu_config_apply_defaults(&scratch, &arena_alloc);
    scratch.arena = arena;
    scratch.allocator = arena_alloc;
    err = hu_config_parse_json(&scratch, doc, doc_len);
    if (err == HU_OK)
        err = hu_config_validate_document(root, true);
    hu_config_deinit(&scratch);
    hu_json_free(alloc, root);
    return err == HU_OK ? HU_OK : HU_ERR_INVALID_ARGUMENT;
}

/* Copy the file's current bytes to "<path>.bak" (0600). Best effort: a
 * missing backup does not block the write, and *out_backup stays NULL. */
static void backup_config(hu_allocator_t *alloc, const char *cfg_path, const char *content,
                          size_t content_len, char **out_backup) {
    size_t bak_len = strlen(cfg_path) + 5;
    char *backup_path = (char *)alloc->alloc(alloc->ctx, bak_len);
    if (!backup_path)
        return;
    int n = snprintf(backup_path, bak_len, "%s.bak", cfg_path);
    FILE *bak = NULL;
    if (n < 0 || (size_t)n >= bak_len ||
        hu_io_secure_open(backup_path, HU_IO_PERM_SECRET, "wb", &bak) != HU_OK || !bak) {
        alloc->free(alloc->ctx, backup_path, bak_len);
        return;
    }
    bool ok = fwrite(content, 1, content_len, bak) == content_len;
    ok = (fclose(bak) == 0) && ok;
    if (!ok) {
        alloc->free(alloc->ctx, backup_path, bak_len);
        return;
    }
    *out_backup = backup_path;
}

hu_error_t hu_config_mutator_mutate_at(hu_allocator_t *alloc, const char *cfg_path,
                                       hu_mutation_action_t action, const char *path,
                                       const char *value_raw, hu_mutation_options_t options,
                                       hu_mutation_result_t *out) {
    if (!alloc || !cfg_path || !cfg_path[0] || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    char *trimmed = NULL;
    hu_error_t err = prepare_path(alloc, action, path, value_raw, &trimmed);
    if (err != HU_OK)
        return err;

    char *content = NULL, *old_json = NULL, *new_json = NULL, *rendered = NULL, *patch = NULL;
    char *backup_path = NULL, *tok_buf = NULL;
    size_t content_len = 0, tok_buf_len = 0, token_count = 0, len = 0;
    char **tokens = NULL;
    hu_json_value_t *root = NULL;
    bool existed = false;

    err = read_config_file(alloc, cfg_path, &content, &content_len, &existed);
    if (err != HU_OK)
        goto done;
    /* An unparseable existing file is refused, never silently replaced by "{}"
     * plus one key: that would erase everything the user had. */
    err = hu_json_parse(alloc, content, content_len, &root);
    if (err != HU_OK || root->type != HU_JSON_OBJECT) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto done;
    }
    err = split_path(alloc, trimmed, &tokens, &token_count, &tok_buf, &tok_buf_len);
    if (err != HU_OK)
        goto done;

    hu_json_value_t *old_val = value_at_path(root, tokens, token_count);
    if (old_val)
        err = hu_json_stringify(alloc, old_val, &old_json, &len);
    else
        old_json = hu_strdup(alloc, "null");
    if (err != HU_OK || !old_json) {
        err = err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;
        goto done;
    }

    if (action == HU_MUTATION_SET) {
        /* value_raw is JSON; anything that does not parse is taken as a string. */
        hu_json_value_t *val = NULL;
        if (hu_json_parse(alloc, value_raw, strlen(value_raw), &val) != HU_OK)
            val = hu_json_string_new(alloc, value_raw, strlen(value_raw));
        hu_json_value_t *parent = val ? ensure_and_walk(alloc, root, tokens, token_count) : NULL;
        if (!parent || parent->type != HU_JSON_OBJECT) {
            if (val)
                hu_json_free(alloc, val);
            err = val ? HU_ERR_INVALID_ARGUMENT : HU_ERR_OUT_OF_MEMORY;
            goto done;
        }
        err = hu_json_object_set(alloc, parent, tokens[token_count - 1], val);
        if (err != HU_OK) {
            hu_json_free(alloc, val);
            goto done;
        }
    } else {
        hu_json_value_t *parent =
            (token_count == 1) ? root : value_at_path(root, tokens, token_count - 1);
        if (parent && parent->type == HU_JSON_OBJECT)
            (void)hu_json_object_remove(alloc, parent, tokens[token_count - 1]);
    }

    hu_json_value_t *new_val = value_at_path(root, tokens, token_count);
    if (new_val)
        err = hu_json_stringify(alloc, new_val, &new_json, &len);
    else
        new_json = hu_strdup(alloc, "null");
    if (err != HU_OK || !new_json) {
        err = err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;
        goto done;
    }

    if (action == HU_MUTATION_SET) {
        size_t patch_len = 0;
        err = hu_config_mutator_build_patch(alloc, trimmed, new_json, &patch, &patch_len);
        if (err == HU_OK)
            err = validate_config_doc(alloc, patch, patch_len);
        if (err != HU_OK)
            goto done;
    }

    bool changed = strcmp(old_json, new_json) != 0;
    if (options.apply && changed) {
        size_t rendered_len = 0;
        err = hu_json_stringify(alloc, root, &rendered, &rendered_len);
        if (err != HU_OK || !rendered) {
            err = err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;
            goto done;
        }
        if (existed)
            backup_config(alloc, cfg_path, content, content_len, &backup_path);
        err = write_config_file(alloc, cfg_path, rendered, strlen(rendered));
        if (err != HU_OK)
            goto done;
    }

    out->path = trimmed;
    out->changed = changed;
    out->applied = options.apply && changed;
    out->requires_restart = hu_config_mutator_path_requires_restart(trimmed);
    out->old_value_json = old_json;
    out->new_value_json = new_json;
    out->backup_path = backup_path;
    trimmed = old_json = new_json = backup_path = NULL; /* owned by *out */

done:
    free_str(alloc, trimmed);
    free_str(alloc, old_json);
    free_str(alloc, new_json);
    free_str(alloc, backup_path);
    free_str(alloc, rendered);
    free_str(alloc, patch);
    if (content)
        alloc->free(alloc->ctx, content, content_len + 1);
    if (tokens)
        alloc->free(alloc->ctx, (void *)tokens, token_count * sizeof(char *));
    if (tok_buf)
        alloc->free(alloc->ctx, tok_buf, tok_buf_len);
    if (root)
        hu_json_free(alloc, root);
    return err;
}

hu_error_t hu_config_mutator_replace_at(hu_allocator_t *alloc, const char *cfg_path,
                                        const char *raw, size_t raw_len) {
    if (!alloc || !cfg_path || !cfg_path[0] || !raw || raw_len == 0 || raw_len > CONFIG_MAX_SIZE)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = validate_config_doc(alloc, raw, raw_len);
    if (err != HU_OK)
        return err;
    char *content = NULL, *backup_path = NULL;
    size_t content_len = 0;
    bool existed = false;
    if (read_config_file(alloc, cfg_path, &content, &content_len, &existed) == HU_OK) {
        if (existed)
            backup_config(alloc, cfg_path, content, content_len, &backup_path);
        alloc->free(alloc->ctx, content, content_len + 1);
    }
    free_str(alloc, backup_path);
    /* Verbatim: the caller's document is what lands, byte for byte. */
    return write_config_file(alloc, cfg_path, raw, raw_len);
}

hu_error_t hu_config_mutator_mutate(hu_allocator_t *alloc, hu_mutation_action_t action,
                                    const char *path, const char *value_raw,
                                    hu_mutation_options_t options, hu_mutation_result_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(HU_IS_TEST)
    /* The default path is the real ~/.human/config.json: tests use
     * hu_config_mutator_mutate_at with a scratch file instead. */
    char *trimmed = NULL;
    hu_error_t err = prepare_path(alloc, action, path, value_raw, &trimmed);
    if (err != HU_OK)
        return err;
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
    if (err != HU_OK)
        return err;
    err = hu_config_mutator_mutate_at(alloc, cfg_path, action, path, value_raw, options, out);
    free_str(alloc, cfg_path);
    return err;
#endif
}
