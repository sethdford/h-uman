#include "human/security/skill_trust.h"
#include "human/core/file.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/paths.h"
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
    int n = hu_paths_state(out, out_cap, "skill_audit.log");
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
