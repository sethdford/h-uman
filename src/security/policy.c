#include "seaclaw/security.h"
#include <stdint.h>
#include "seaclaw/core/error.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#define SC_MAX_ANALYSIS_LEN 16384

static const char *high_risk_commands[] = {
    "rm", "mkfs", "dd", "shutdown", "reboot", "halt",
    "poweroff", "sudo", "su", "chown", "chmod", "useradd",
    "userdel", "usermod", "passwd", "mount", "umount", "iptables",
    "ufw", "firewall-cmd", "curl", "wget", "nc", "ncat",
    "netcat", "scp", "ssh", "ftp", "telnet"
};
static const size_t high_risk_count =
    sizeof(high_risk_commands) / sizeof(high_risk_commands[0]);

static const char *default_allowed[] = {
    "git", "npm", "cargo", "ls", "cat", "grep", "find", "echo", "pwd", "wc", "head", "tail"
};
static const size_t default_allowed_count =
    sizeof(default_allowed) / sizeof(default_allowed[0]);

/* ── Rate tracker ───────────────────────────────────────────────── */

struct sc_rate_tracker {
    time_t *timestamps;
    size_t count;
    size_t cap;
    uint32_t max_actions;
    time_t window_secs;
    sc_allocator_t *alloc;
};

static void prune_timestamps(sc_rate_tracker_t *t) {
    time_t now = time(NULL);
    time_t cutoff = now - t->window_secs;
    size_t write = 0;
    for (size_t i = 0; i < t->count; i++) {
        if (t->timestamps[i] > cutoff) {
            t->timestamps[write++] = t->timestamps[i];
        }
    }
    t->count = write;
}

sc_rate_tracker_t *sc_rate_tracker_create(sc_allocator_t *alloc, uint32_t max_actions) {
    if (!alloc) return NULL;
    sc_rate_tracker_t *t = (sc_rate_tracker_t *)alloc->alloc(alloc->ctx, sizeof(sc_rate_tracker_t));
    if (!t) return NULL;
    t->timestamps = NULL;
    t->count = 0;
    t->cap = max_actions > 128 ? max_actions : 128;
    t->max_actions = max_actions;
    t->window_secs = 3600; /* 1 hour */
    t->alloc = alloc;
    t->timestamps = (time_t *)alloc->alloc(alloc->ctx, t->cap * sizeof(time_t));
    if (!t->timestamps) {
        alloc->free(alloc->ctx, t, sizeof(sc_rate_tracker_t));
        return NULL;
    }
    return t;
}

void sc_rate_tracker_destroy(sc_rate_tracker_t *t) {
    if (!t) return;
    if (t->timestamps) t->alloc->free(t->alloc->ctx, t->timestamps, t->cap * sizeof(time_t));
    t->alloc->free(t->alloc->ctx, t, sizeof(sc_rate_tracker_t));
}

bool sc_rate_tracker_record_action(sc_rate_tracker_t *t) {
    if (!t) return true;
    prune_timestamps(t);
    time_t now = time(NULL);
    if (t->count >= t->cap) {
        size_t new_cap = t->cap * 2;
        time_t *n = (time_t *)t->alloc->realloc(t->alloc->ctx, t->timestamps,
                                                 t->cap * sizeof(time_t),
                                                 new_cap * sizeof(time_t));
        if (!n) return false;
        t->timestamps = n;
        t->cap = new_cap;
    }
    t->timestamps[t->count++] = now;
    return t->count <= t->max_actions;
}

bool sc_rate_tracker_is_limited(sc_rate_tracker_t *t) {
    if (!t) return false;
    prune_timestamps(t);
    return t->count >= t->max_actions;
}

size_t sc_rate_tracker_count(sc_rate_tracker_t *t) {
    if (!t) return 0;
    prune_timestamps(t);
    return t->count;
}

uint32_t sc_rate_tracker_remaining(sc_rate_tracker_t *t) {
    if (!t) return 0;
    prune_timestamps(t);
    if (t->count >= t->max_actions) return 0;
    return (uint32_t)(t->max_actions - t->count);
}

/* ── Policy helpers ─────────────────────────────────────────────── */

static bool contains_str(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static void to_lower(const char *in, size_t len, char *out) {
    for (size_t i = 0; i < len && in[i]; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
}

static const char *basename_ptr(const char *path) {
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static bool is_high_risk_command(const char *base) {
    for (size_t i = 0; i < high_risk_count; i++) {
        if (strcasecmp(base, high_risk_commands[i]) == 0)
            return true;
    }
    return false;
}

static bool is_medium_risk_cmd(const char *base, const char *first_arg) {
    char arg_lower[256];
    if (first_arg) {
        const char *arg_end = strchr(first_arg, ' ');
        size_t alen = arg_end ? (size_t)(arg_end - first_arg) : strlen(first_arg);
        if (alen >= sizeof(arg_lower)) alen = sizeof(arg_lower) - 1;
        memcpy(arg_lower, first_arg, alen);
        arg_lower[alen] = '\0';
        to_lower(arg_lower, alen, arg_lower);
    } else {
        arg_lower[0] = '\0';
    }

    if (strcmp(base, "git") == 0) {
        const char *med[] = {"commit", "push", "reset", "clean", "rebase", "merge",
                             "cherry-pick", "revert", "branch", "checkout", "switch", "tag"};
        for (size_t i = 0; i < sizeof(med)/sizeof(med[0]); i++)
            if (strcmp(arg_lower, med[i]) == 0) return true;
    }
    if (strcmp(base, "npm") == 0 || strcmp(base, "pnpm") == 0 || strcmp(base, "yarn") == 0) {
        const char *med[] = {"install", "add", "remove", "uninstall", "update", "publish"};
        for (size_t i = 0; i < sizeof(med)/sizeof(med[0]); i++)
            if (strcmp(arg_lower, med[i]) == 0) return true;
    }
    if (strcmp(base, "cargo") == 0) {
        const char *med[] = {"add", "remove", "install", "clean", "publish"};
        for (size_t i = 0; i < sizeof(med)/sizeof(med[0]); i++)
            if (strcmp(arg_lower, med[i]) == 0) return true;
    }
    if (strcmp(base, "touch") == 0 || strcmp(base, "mkdir") == 0 ||
        strcmp(base, "mv") == 0 || strcmp(base, "cp") == 0 || strcmp(base, "ln") == 0)
        return true;
    return false;
}

static const char *skip_env_assignments(const char *s) {
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        const char *end = s;
        while (*end && *end != ' ' && *end != '\t') end++;
        /* Check if it's KEY=value */
        const char *eq = strchr(s, '=');
        if (eq && eq < end && eq > s &&
            (isalpha((unsigned char)s[0]) || s[0] == '_')) {
            s = end;
            continue;
        }
        break;
    }
    return s;
}

static bool contains_single_ampersand(const char *s) {
    if (!s) return false;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] != '&') continue;
        int prev = (i > 0) && (s[i-1] == '&');
        int next = s[i+1] && (s[i+1] == '&');
        if (!prev && !next) return true;
    }
    return false;
}

static bool is_args_safe(const char *base, const char *full_cmd) {
    if (strcasecmp(base, "find") == 0) {
        if (strstr(full_cmd, "-exec") || strstr(full_cmd, "-ok"))
            return false;
    }
    if (strcasecmp(base, "git") == 0) {
        if (strstr(full_cmd, " config") || strstr(full_cmd, " alias") ||
            strstr(full_cmd, " -c "))
            return false;
    }
    return true;
}

static void normalize_command(const char *cmd, size_t len, char *buf) {
    if (len > SC_MAX_ANALYSIS_LEN) len = SC_MAX_ANALYSIS_LEN;
    memcpy(buf, cmd, len);
    buf[len] = '\0';
    /* Replace && and || with nulls */
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '&' && buf[i+1] == '&') { buf[i] = 0; buf[i+1] = 0; i++; }
        if (buf[i] == '|' && buf[i+1] == '|') { buf[i] = 0; buf[i+1] = 0; i++; }
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == ';' || buf[i] == '|') buf[i] = 0;
    }
}

/* ── Policy API ─────────────────────────────────────────────────── */

sc_command_risk_level_t sc_policy_command_risk_level(const sc_security_policy_t *policy,
                                                      const char *command) {
    (void)policy;
    size_t cmd_len = command ? strlen(command) : 0;
    if (cmd_len > SC_MAX_ANALYSIS_LEN)
        return SC_RISK_HIGH;

    char norm[SC_MAX_ANALYSIS_LEN + 1];
    normalize_command(command, cmd_len, norm);

    bool saw_medium = false;
    const char *seg = norm;
    while (seg < norm + cmd_len) {
        while (seg < norm + cmd_len && (*seg == 0 || *seg == ' ' || *seg == '\t')) seg++;
        if (seg >= norm + cmd_len) break;

        const char *seg_end = seg;
        while (seg_end < norm + cmd_len && *seg_end) seg_end++;
        const char *cmd_part = skip_env_assignments(seg);

        const char *sp = strchr(cmd_part, ' ');
        size_t first_word_len = sp ? (size_t)(sp - cmd_part) : strlen(cmd_part);
        char base[128];
        size_t blen = first_word_len < sizeof(base) - 1 ? first_word_len : sizeof(base) - 1;
        memcpy(base, cmd_part, blen);
        base[blen] = '\0';

        const char *base_name = basename_ptr(base);
        char lower_base[128];
        size_t bnlen = strlen(base_name);
        if (bnlen >= sizeof(lower_base)) bnlen = sizeof(lower_base) - 1;
        to_lower(base_name, bnlen, lower_base);
        lower_base[bnlen] = '\0';

        if (is_high_risk_command(lower_base)) return SC_RISK_HIGH;
        if (contains_str(cmd_part, "rm -rf /") || contains_str(cmd_part, "rm -fr /"))
            return SC_RISK_HIGH;

        const char *first_arg = sp ? sp + 1 : NULL;
        while (first_arg && *first_arg == ' ') first_arg++;
        if (is_medium_risk_cmd(lower_base, first_arg)) saw_medium = true;

        seg = seg_end + 1;
    }

    return saw_medium ? SC_RISK_MEDIUM : SC_RISK_LOW;
}

sc_error_t sc_policy_validate_command(const sc_security_policy_t *policy,
                                      const char *command, bool approved,
                                      sc_command_risk_level_t *out_risk) {
    if (!policy || !command || !out_risk) return SC_ERR_INVALID_ARGUMENT;

    if (!sc_policy_is_command_allowed(policy, command))
        return SC_ERR_SECURITY_COMMAND_NOT_ALLOWED;

    sc_command_risk_level_t risk = sc_policy_command_risk_level(policy, command);

    if (risk == SC_RISK_HIGH) {
        if (policy->block_high_risk_commands)
            return SC_ERR_SECURITY_HIGH_RISK_BLOCKED;
        if (policy->autonomy == SC_AUTONOMY_SUPERVISED && !approved)
            return SC_ERR_SECURITY_APPROVAL_REQUIRED;
    }

    if (risk == SC_RISK_MEDIUM &&
        policy->autonomy == SC_AUTONOMY_SUPERVISED &&
        policy->require_approval_for_medium_risk &&
        !approved)
        return SC_ERR_SECURITY_APPROVAL_REQUIRED;

    *out_risk = risk;
    return SC_OK;
}

bool sc_policy_is_command_allowed(const sc_security_policy_t *policy,
                                  const char *command) {
    if (!policy || !command) return false;
    if (policy->autonomy == SC_AUTONOMY_READ_ONLY) return false;
    if (strlen(command) > SC_MAX_ANALYSIS_LEN) return false;

    if (contains_str(command, "`") || contains_str(command, "$(") ||
        contains_str(command, "${"))
        return false;
    if (contains_str(command, "<(") || contains_str(command, ">("))
        return false;
    /* Block "tee" as a command (can write to arbitrary files) */
    if (strstr(command, " tee ") || strstr(command, "|tee") || strstr(command, "tee|") ||
        strncmp(command, "tee ", 4) == 0 || (strlen(command) >= 4 && strcmp(command, "tee") == 0))
        return false;
    if (contains_single_ampersand(command)) return false;
    if (strchr(command, '>')) return false;
    if (strchr(command, '|')) return false;

    const char **allowed = policy->allowed_commands;
    size_t allowed_len = policy->allowed_commands_len;
    if (!allowed || allowed_len == 0) {
        allowed = default_allowed;
        allowed_len = default_allowed_count;
    }

    size_t cmd_len = strlen(command);
    char norm[SC_MAX_ANALYSIS_LEN + 1];
    normalize_command(command, cmd_len, norm);

    bool has_cmd = false;
    const char *seg = norm;
    while (seg < norm + cmd_len) {
        while (seg < norm + cmd_len && (*seg == 0 || *seg == ' ' || *seg == '\t')) seg++;
        if (seg >= norm + cmd_len) break;

        const char *seg_end = seg;
        while (seg_end < norm + cmd_len && *seg_end) seg_end++;

        const char *cmd_part = skip_env_assignments(seg);
        if (cmd_part != seg) return false;
        const char *sp = strchr(cmd_part, ' ');
        size_t fw_len = sp ? (size_t)(sp - cmd_part) : strlen(cmd_part);
        if (fw_len == 0) { seg = seg_end + 1; continue; }

        char base[128];
        size_t blen = fw_len < sizeof(base) - 1 ? fw_len : sizeof(base) - 1;
        memcpy(base, cmd_part, blen);
        base[blen] = '\0';
        const char *base_name = basename_ptr(base);
        if (!*base_name) { seg = seg_end + 1; continue; }

        has_cmd = true;
        bool found = false;
        for (size_t i = 0; i < allowed_len; i++) {
            const char *a = allowed[i];
            if (!a) continue;
            if (strcmp(a, "*") == 0 || strcasecmp(a, base_name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        if (!is_args_safe(base_name, cmd_part)) return false;

        seg = seg_end + 1;
    }
    return has_cmd;
}

bool sc_policy_can_act(const sc_security_policy_t *policy) {
    return policy && policy->autonomy != SC_AUTONOMY_READ_ONLY;
}

bool sc_policy_record_action(sc_security_policy_t *policy) {
    if (!policy) return true;
    if (!policy->tracker) return true;
    return sc_rate_tracker_record_action(policy->tracker);
}

bool sc_policy_is_rate_limited(const sc_security_policy_t *policy) {
    if (!policy || !policy->tracker) return false;
    return sc_rate_tracker_is_limited(policy->tracker);
}
