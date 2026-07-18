#include "human/security/skill_trust.h"
#include "human/core/file.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool word_before_ok(const char *s, const char *w) {
    return w == s || (!isalnum((unsigned char)w[-1]) && w[-1] != '_');
}

static bool word_after_ok(const char *w, size_t wlen) {
    unsigned char c = (unsigned char)w[wlen];
    return (c == '\0') || ((!isalnum(c)) && c != '_');
}

static const char *find_word_ci(const char *s, const char *word) {
    size_t wlen = strlen(word);
    if (wlen == 0)
        return NULL;
    const char *p = s;
    for (;;) {
        p = hu_strcasestr(p, word);
        if (!p)
            return NULL;
        if (!word_before_ok(s, p) || !word_after_ok(p, wlen)) {
            p++;
            continue;
        }
        return p;
    }
}

static bool dangerous_rm_rf_root(const char *s) {
    const char *p = s;
    while ((p = hu_strcasestr(p, "rm -rf /")) != NULL) {
        const char *q = p + 8;
        if (*q == '\0' || isspace((unsigned char)*q) || *q == '*' || *q == '/')
            return true;
        p = q;
    }
    return false;
}

static bool dangerous_curl_pipe_shell(const char *s) {
    const char *p = s;
    for (;;) {
        p = find_word_ci(p, "curl");
        if (!p)
            return false;
        const char *pipe_pos = strchr(p + 4, '|');
        while (pipe_pos != NULL) {
            const char *r = pipe_pos + 1;
            while (*r && isspace((unsigned char)*r))
                r++;
            if ((hu_strcasestr(r, "sh") == r &&
                 (r[2] == '\0' || (!isalnum((unsigned char)r[2]) && r[2] != '_'))) ||
                (hu_strcasestr(r, "bash") == r &&
                 (r[4] == '\0' || (!isalnum((unsigned char)r[4]) && r[4] != '_'))))
                return true;
            pipe_pos = strchr(pipe_pos + 1, '|');
        }
        p++;
    }
}

static bool dangerous_dev_redirect(const char *s) {
    return hu_strcasestr(s, ">> /dev/") != NULL || hu_strcasestr(s, "> /dev/") != NULL;
}

static bool dangerous_mkfs(const char *s) {
    return find_word_ci(s, "mkfs") != NULL;
}

static bool dangerous_fork_bomb(const char *s) {
    return strcmp(s, ":(){ :|:& };:") == 0;
}

static bool dangerous_dd_dev(const char *s) {
    const char *ofdev = hu_strcasestr(s, "of=/dev/");
    const char *ifeq = hu_strcasestr(s, "if=");
    const char *ddw = find_word_ci(s, "dd");
    return ofdev != NULL && ifeq != NULL && ddw != NULL && ddw < ifeq;
}

static bool dangerous_chmod_777_root(const char *s) {
    const char *p = hu_strcasestr(s, "chmod 777 /");
    if (!p)
        return false;
    const char *q = p + 11;
    return *q == '\0' || isspace((unsigned char)*q) || *q == '*' || *q == '/';
}

static bool dangerous_eval_subshell(const char *s) {
    const char *p = s;
    for (;;) {
        p = hu_strcasestr(p, "eval");
        if (!p)
            return false;
        if (!word_before_ok(s, p) || !word_after_ok(p, 4)) {
            p++;
            continue;
        }
        const char *after = p + 4;
        if (*after != ' ' && *after != '\t') {
            p++;
            continue;
        }
        after++;
        while (*after == ' ' || *after == '\t')
            after++;
        if (strstr(after, "$(") != NULL)
            return true;
        p++;
    }
}

hu_error_t hu_skill_trust_verify_signature(const hu_skill_trust_config_t *cfg,
                                           const char *publisher_name, const char *manifest_json,
                                           size_t manifest_json_len, const char *signature_hex,
                                           size_t signature_hex_len) {
    (void)manifest_json_len;
    (void)signature_hex_len;
    if (!cfg || !publisher_name || !manifest_json || !signature_hex)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->trusted_publishers_count > 0 && !cfg->trusted_publishers)
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(HU_IS_TEST) || !HU_IS_TEST
    hu_log_info(
        "skill_trust", NULL,
        "warning: Ed25519 signature verification not yet implemented (publisher allowlist only)");
#endif

    for (size_t i = 0; i < cfg->trusted_publishers_count; i++) {
        const char *tn = cfg->trusted_publishers[i].name;
        if (tn && strcmp(publisher_name, tn) == 0)
            return HU_OK;
    }
    return HU_ERR_SECURITY_HIGH_RISK_BLOCKED;
}

hu_error_t hu_skill_trust_inspect_command(const char *command, size_t command_len) {
    if (!command || command_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char *buf = (char *)malloc(command_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, command, command_len);
    buf[command_len] = '\0';

    hu_error_t err = HU_OK;
    if (dangerous_rm_rf_root(buf) || dangerous_curl_pipe_shell(buf) ||
        dangerous_dev_redirect(buf) || dangerous_mkfs(buf) || dangerous_fork_bomb(buf) ||
        dangerous_dd_dev(buf) || dangerous_chmod_777_root(buf) || dangerous_eval_subshell(buf))
        err = HU_ERR_SECURITY_COMMAND_NOT_ALLOWED;

    free(buf);
    return err;
}

const char *hu_skill_trust_get_policy(hu_skill_sandbox_tier_t tier) {
    switch (tier) {
    case HU_SKILL_SANDBOX_NONE:
        return "unrestricted";
    case HU_SKILL_SANDBOX_BASIC:
        return "sandbox_basic";
    case HU_SKILL_SANDBOX_STRICT:
        return "sandbox_strict";
    default:
        return "sandbox_strict";
    }
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static void fprint_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', f);
        }
        fputc(*s, f);
    }
    fputc('"', f);
}

static int build_audit_path(char *out, size_t out_cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    int n = snprintf(out, out_cap, "%s/.human/skill_audit.log", home);
    if (n < 0 || (size_t)n >= out_cap)
        return -1;
    return 0;
}
#endif

hu_error_t hu_skill_trust_audit_record(hu_allocator_t *alloc, const hu_skill_audit_entry_t *entry) {
    if (!alloc || !entry)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)entry;
    return HU_OK;
#else
    (void)alloc;
    char path[4096];
    if (build_audit_path(path, sizeof(path)) != 0)
        return HU_ERR_IO;

    FILE *f = fopen(path, "a");
    if (!f)
        return HU_ERR_IO;

    const char *sn = entry->skill_name ? entry->skill_name : "";
    const char *ah = entry->args_hash ? entry->args_hash : "";

    fputc('{', f);
    fputs("\"skill\":", f);
    fprint_json_string(f, sn);
    fputs(",\"args_hash\":", f);
    fprint_json_string(f, ah);
    fprintf(f, ",\"time_ms\":%.17g,\"exit_code\":%d,\"allowed\":%s}\n", entry->execution_time_ms,
            entry->exit_code, entry->allowed ? "true" : "false");
    if (fclose(f) != 0)
        return HU_ERR_IO;
    return HU_OK;
#endif
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static int build_publishers_path(char *out, size_t out_cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    int n = snprintf(out, out_cap, "%s/.human/trusted_publishers.json", home);
    if (n < 0 || (size_t)n >= out_cap)
        return -1;
    return 0;
}

static hu_error_t read_file_contents(hu_allocator_t *alloc, const char *path, char **out_buf,
                                     size_t *out_len) {
    hu_error_t err = hu_file_slurp(alloc, path, 0, out_buf, out_len);
    if (err == HU_ERR_NOT_FOUND) {
        *out_buf = NULL;
        *out_len = 0;
        return HU_OK;
    }
    return err;
}
#endif

hu_error_t hu_skill_trust_load_publishers(hu_allocator_t *alloc, hu_publisher_key_t **out,
                                          size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    *out = NULL;
    *out_count = 0;
    return HU_OK;
#else
    *out = NULL;
    *out_count = 0;

    char path[4096];
    if (build_publishers_path(path, sizeof(path)) != 0)
        return HU_ERR_IO;

    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = read_file_contents(alloc, path, &raw, &raw_len);
    if (err != HU_OK)
        return err;
    if (!raw || raw_len == 0) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_len + 1);
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, raw, raw_len, &root);
    alloc->free(alloc->ctx, raw, raw_len + 1);
    raw = NULL;
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    size_t n = root->data.array.len;
    hu_publisher_key_t *arr = NULL;
    if (n > 0) {
        arr = (hu_publisher_key_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_publisher_key_t));
        if (!arr) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(arr, 0, n * sizeof(hu_publisher_key_t));
    }

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *el = root->data.array.items[i];
        if (!el || el->type != HU_JSON_OBJECT) {
            err = HU_ERR_PARSE;
            goto cleanup;
        }
        const char *name = hu_json_get_string(el, "name");
        const char *pk = hu_json_get_string(el, "public_key");
        if (!name || !pk) {
            err = HU_ERR_PARSE;
            goto cleanup;
        }
        arr[i].name = hu_strdup(alloc, name);
        arr[i].public_key_hex = hu_strdup(alloc, pk);
        if (!arr[i].name || !arr[i].public_key_hex) {
            err = HU_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }

    hu_json_free(alloc, root);
    *out = arr;
    *out_count = n;
    return HU_OK;

cleanup:
    for (size_t j = 0; j < n; j++) {
        hu_str_free(alloc, arr[j].name);
        hu_str_free(alloc, arr[j].public_key_hex);
    }
    if (arr)
        alloc->free(alloc->ctx, arr, n * sizeof(hu_publisher_key_t));
    hu_json_free(alloc, root);
    return err;
#endif
}

void hu_skill_trust_free_publishers(hu_allocator_t *alloc, hu_publisher_key_t *publishers,
                                    size_t count) {
    if (!alloc || !publishers || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        hu_str_free(alloc, publishers[i].name);
        hu_str_free(alloc, publishers[i].public_key_hex);
    }
    alloc->free(alloc->ctx, publishers, count * sizeof(hu_publisher_key_t));
}

void hu_skill_audit_entry_deinit(hu_skill_audit_entry_t *e, hu_allocator_t *alloc) {
    if (!e || !alloc)
        return;
    hu_str_free(alloc, e->skill_name);
    hu_str_free(alloc, e->args_hash);
    e->skill_name = NULL;
    e->args_hash = NULL;
}
