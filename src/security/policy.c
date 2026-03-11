#include "human/core/error.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/data/loader.h"
#include "human/core/json.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_MAX_ANALYSIS_LEN 16384

/* ── Tool risk level (for graduated autonomy) ───────────────────── */

hu_command_risk_level_t hu_tool_risk_level(const char *tool_name) {
    if (!tool_name)
        return HU_RISK_HIGH;
    if (strcmp(tool_name, "shell") == 0 || strcmp(tool_name, "spawn") == 0)
        return HU_RISK_HIGH;
    if (strcmp(tool_name, "file_write") == 0 || strcmp(tool_name, "file_edit") == 0)
        return HU_RISK_HIGH;
    if (strcmp(tool_name, "http_request") == 0)
        return HU_RISK_MEDIUM;
    if (strcmp(tool_name, "browser_open") == 0)
        return HU_RISK_MEDIUM;
    if (strcmp(tool_name, "file_read") == 0 || strcmp(tool_name, "memory_recall") == 0)
        return HU_RISK_LOW;
    return HU_RISK_MEDIUM; /* unknown tools default to medium */
}

/* Default fallback command lists */
static const char *DEFAULT_HIGH_RISK_COMMANDS[] = {
    "rm",     "mkfs",     "dd",    "shutdown",     "reboot",  "halt",    "poweroff", "sudo",
    "su",     "chown",    "chmod", "useradd",      "userdel", "usermod", "passwd",   "mount",
    "umount", "iptables", "ufw",   "firewall-cmd", "curl",    "wget",    "nc",       "ncat",
    "netcat", "scp",      "ssh",   "ftp",          "telnet"};
static const size_t DEFAULT_HIGH_RISK_COUNT = sizeof(DEFAULT_HIGH_RISK_COMMANDS) / sizeof(DEFAULT_HIGH_RISK_COMMANDS[0]);

static const char *DEFAULT_ALLOWED[] = {"git",  "npm",  "cargo", "ls", "cat",  "grep",
                                        "find", "echo", "pwd",   "wc", "head", "tail"};
static const size_t DEFAULT_ALLOWED_COUNT = sizeof(DEFAULT_ALLOWED) / sizeof(DEFAULT_ALLOWED[0]);

/* Runtime loaded command lists */
static const char **s_high_risk_commands = (const char **)DEFAULT_HIGH_RISK_COMMANDS;
static size_t s_high_risk_count = sizeof(DEFAULT_HIGH_RISK_COMMANDS) / sizeof(DEFAULT_HIGH_RISK_COMMANDS[0]);

static const char **s_default_allowed = (const char **)DEFAULT_ALLOWED;
static size_t s_default_allowed_count = sizeof(DEFAULT_ALLOWED) / sizeof(DEFAULT_ALLOWED[0]);

hu_error_t hu_policy_data_init(hu_allocator_t *alloc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    char *json_data = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_data_load(alloc, "security/command_lists.json", &json_data, &json_len);
    if (err != HU_OK)
        return HU_OK; /* Fail gracefully, keep defaults */

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, json_data, json_len, &root);
    alloc->free(alloc->ctx, json_data, json_len);
    if (err != HU_OK || !root)
        return HU_OK; /* Fail gracefully, keep defaults */

    /* Load high_risk commands */
    hu_json_value_t *high_risk_arr = hu_json_object_get(root, "high_risk");
    if (high_risk_arr && high_risk_arr->type == HU_JSON_ARRAY) {
        size_t count = high_risk_arr->data.array.len;
        if (count > 0) {
            const char **cmds = (const char **)alloc->alloc(alloc->ctx, count * sizeof(const char *));
            if (cmds) {
                memset(cmds, 0, count * sizeof(const char *));
                for (size_t i = 0; i < count; i++) {
                    hu_json_value_t *item = high_risk_arr->data.array.items[i];
                    if (item && item->type == HU_JSON_STRING) {
                        cmds[i] = hu_strndup(alloc, item->data.string.ptr, item->data.string.len);
                    }
                }
                s_high_risk_commands = cmds;
                s_high_risk_count = count;
            }
        }
    }

    /* Load default_allowed commands */
    hu_json_value_t *allowed_arr = hu_json_object_get(root, "default_allowed");
    if (allowed_arr && allowed_arr->type == HU_JSON_ARRAY) {
        size_t count = allowed_arr->data.array.len;
        if (count > 0) {
            const char **cmds = (const char **)alloc->alloc(alloc->ctx, count * sizeof(const char *));
            if (cmds) {
                memset(cmds, 0, count * sizeof(const char *));
                for (size_t i = 0; i < count; i++) {
                    hu_json_value_t *item = allowed_arr->data.array.items[i];
                    if (item && item->type == HU_JSON_STRING) {
                        cmds[i] = hu_strndup(alloc, item->data.string.ptr, item->data.string.len);
                    }
                }
                s_default_allowed = cmds;
                s_default_allowed_count = count;
            }
        }
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

void hu_policy_data_cleanup(hu_allocator_t *alloc) {
    if (!alloc)
        return;
    if (s_high_risk_commands != (const char **)DEFAULT_HIGH_RISK_COMMANDS) {
        for (size_t i = 0; i < s_high_risk_count; i++) {
            if (s_high_risk_commands[i])
                alloc->free(alloc->ctx, (char *)s_high_risk_commands[i], strlen(s_high_risk_commands[i]) + 1);
        }
        alloc->free(alloc->ctx, s_high_risk_commands, s_high_risk_count * sizeof(const char *));
    }
    if (s_default_allowed != (const char **)DEFAULT_ALLOWED) {
        for (size_t i = 0; i < s_default_allowed_count; i++) {
            if (s_default_allowed[i])
                alloc->free(alloc->ctx, (char *)s_default_allowed[i], strlen(s_default_allowed[i]) + 1);
        }
        alloc->free(alloc->ctx, s_default_allowed, s_default_allowed_count * sizeof(const char *));
    }
    s_high_risk_commands = (const char **)DEFAULT_HIGH_RISK_COMMANDS;
    s_high_risk_count = DEFAULT_HIGH_RISK_COUNT;
    s_default_allowed = (const char **)DEFAULT_ALLOWED;
    s_default_allowed_count = DEFAULT_ALLOWED_COUNT;
}

/* ── Rate tracker ───────────────────────────────────────────────── */

struct hu_rate_tracker {
    time_t *timestamps;
    size_t count;
    size_t cap;
    uint32_t max_actions;
    time_t window_secs;
    hu_allocator_t *alloc;
};

static void prune_timestamps(hu_rate_tracker_t *t) {
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

hu_rate_tracker_t *hu_rate_tracker_create(hu_allocator_t *alloc, uint32_t max_actions) {
    if (!alloc)
        return NULL;
    hu_rate_tracker_t *t = (hu_rate_tracker_t *)alloc->alloc(alloc->ctx, sizeof(hu_rate_tracker_t));
    if (!t)
        return NULL;
    t->timestamps = NULL;
    t->count = 0;
    t->cap = max_actions > 128 ? max_actions : 128;
    t->max_actions = max_actions;
    t->window_secs = 3600; /* 1 hour */
    t->alloc = alloc;
    t->timestamps = (time_t *)alloc->alloc(alloc->ctx, t->cap * sizeof(time_t));
    if (!t->timestamps) {
        alloc->free(alloc->ctx, t, sizeof(hu_rate_tracker_t));
        return NULL;
    }
    return t;
}

void hu_rate_tracker_destroy(hu_rate_tracker_t *t) {
    if (!t)
        return;
    if (t->timestamps)
        t->alloc->free(t->alloc->ctx, t->timestamps, t->cap * sizeof(time_t));
    t->alloc->free(t->alloc->ctx, t, sizeof(hu_rate_tracker_t));
}

#define HU_RATE_TRACKER_CAP_MAX 4096

bool hu_rate_tracker_record_action(hu_rate_tracker_t *t) {
    if (!t)
        return true;
    prune_timestamps(t);
    time_t now = time(NULL);
    if (t->count >= t->cap) {
        if (t->cap >= HU_RATE_TRACKER_CAP_MAX)
            return false;
        size_t new_cap = t->cap * 2;
        if (new_cap > HU_RATE_TRACKER_CAP_MAX)
            new_cap = HU_RATE_TRACKER_CAP_MAX;
        time_t *n = (time_t *)t->alloc->realloc(t->alloc->ctx, t->timestamps,
                                                t->cap * sizeof(time_t), new_cap * sizeof(time_t));
        if (!n)
            return false;
        t->timestamps = n;
        t->cap = new_cap;
    }
    t->timestamps[t->count++] = now;
    return t->count <= t->max_actions;
}

bool hu_rate_tracker_is_limited(hu_rate_tracker_t *t) {
    if (!t)
        return false;
    prune_timestamps(t);
    return t->count >= t->max_actions;
}

size_t hu_rate_tracker_count(hu_rate_tracker_t *t) {
    if (!t)
        return 0;
    prune_timestamps(t);
    return t->count;
}

uint32_t hu_rate_tracker_remaining(hu_rate_tracker_t *t) {
    if (!t)
        return 0;
    prune_timestamps(t);
    if (t->count >= t->max_actions)
        return 0;
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
    for (size_t i = 0; i < s_high_risk_count; i++) {
        if (strcasecmp(base, s_high_risk_commands[i]) == 0)
            return true;
    }
    return false;
}

static bool is_medium_risk_cmd(const char *base, const char *first_arg) {
    char arg_lower[256];
    if (first_arg) {
        const char *arg_end = strchr(first_arg, ' ');
        size_t alen = arg_end ? (size_t)(arg_end - first_arg) : strlen(first_arg);
        if (alen >= sizeof(arg_lower))
            alen = sizeof(arg_lower) - 1;
        memcpy(arg_lower, first_arg, alen);
        arg_lower[alen] = '\0';
        to_lower(arg_lower, alen, arg_lower);
    } else {
        arg_lower[0] = '\0';
    }

    if (strcmp(base, "git") == 0) {
        const char *med[] = {"commit",      "push",   "reset",  "clean",    "rebase", "merge",
                             "cherry-pick", "revert", "branch", "checkout", "switch", "tag"};
        for (size_t i = 0; i < sizeof(med) / sizeof(med[0]); i++)
            if (strcmp(arg_lower, med[i]) == 0)
                return true;
    }
    if (strcmp(base, "npm") == 0 || strcmp(base, "pnpm") == 0 || strcmp(base, "yarn") == 0) {
        const char *med[] = {"install", "add", "remove", "uninstall", "update", "publish"};
        for (size_t i = 0; i < sizeof(med) / sizeof(med[0]); i++)
            if (strcmp(arg_lower, med[i]) == 0)
                return true;
    }
    if (strcmp(base, "cargo") == 0) {
        const char *med[] = {"add", "remove", "install", "clean", "publish"};
        for (size_t i = 0; i < sizeof(med) / sizeof(med[0]); i++)
            if (strcmp(arg_lower, med[i]) == 0)
                return true;
    }
    if (strcmp(base, "touch") == 0 || strcmp(base, "mkdir") == 0 || strcmp(base, "mv") == 0 ||
        strcmp(base, "cp") == 0 || strcmp(base, "ln") == 0)
        return true;
    return false;
}

static const char *skip_env_assignments(const char *s) {
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        const char *end = s;
        while (*end && *end != ' ' && *end != '\t')
            end++;
        /* Check if it's KEY=value */
        const char *eq = strchr(s, '=');
        if (eq && eq < end && eq > s && (isalpha((unsigned char)s[0]) || s[0] == '_')) {
            s = end;
            continue;
        }
        break;
    }
    return s;
}

static bool contains_single_ampersand(const char *s) {
    if (!s)
        return false;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] != '&')
            continue;
        int prev = (i > 0) && (s[i - 1] == '&');
        int next = s[i + 1] && (s[i + 1] == '&');
        if (!prev && !next)
            return true;
    }
    return false;
}

static bool is_args_safe(const char *base, const char *full_cmd) {
    if (strcasecmp(base, "find") == 0) {
        if (strstr(full_cmd, "-exec") || strstr(full_cmd, "-ok"))
            return false;
    }
    if (strcasecmp(base, "git") == 0) {
        if (strstr(full_cmd, " config") || strstr(full_cmd, " alias") || strstr(full_cmd, " -c "))
            return false;
    }
    return true;
}

static void normalize_command(const char *cmd, size_t len, char *buf) {
    if (len > HU_MAX_ANALYSIS_LEN)
        len = HU_MAX_ANALYSIS_LEN;
    memcpy(buf, cmd, len);
    buf[len] = '\0';
    /* Replace && and || with nulls */
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '&' && buf[i + 1] == '&') {
            buf[i] = 0;
            buf[i + 1] = 0;
            i++;
        }
        if (buf[i] == '|' && buf[i + 1] == '|') {
            buf[i] = 0;
            buf[i + 1] = 0;
            i++;
        }
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == ';' || buf[i] == '|')
            buf[i] = 0;
    }
}

/* ── Policy API ─────────────────────────────────────────────────── */

hu_command_risk_level_t hu_policy_command_risk_level(const hu_security_policy_t *policy,
                                                     const char *command) {
    (void)policy;
    size_t cmd_len = command ? strlen(command) : 0;
    if (cmd_len > HU_MAX_ANALYSIS_LEN)
        return HU_RISK_HIGH;

    char norm[HU_MAX_ANALYSIS_LEN + 1];
    normalize_command(command, cmd_len, norm);

    bool saw_medium = false;
    const char *seg = norm;
    while (seg < norm + cmd_len) {
        while (seg < norm + cmd_len && (*seg == 0 || *seg == ' ' || *seg == '\t'))
            seg++;
        if (seg >= norm + cmd_len)
            break;

        const char *seg_end = seg;
        while (seg_end < norm + cmd_len && *seg_end)
            seg_end++;
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
        if (bnlen >= sizeof(lower_base))
            bnlen = sizeof(lower_base) - 1;
        to_lower(base_name, bnlen, lower_base);
        lower_base[bnlen] = '\0';

        if (is_high_risk_command(lower_base))
            return HU_RISK_HIGH;
        if (contains_str(cmd_part, "rm -rf /") || contains_str(cmd_part, "rm -fr /"))
            return HU_RISK_HIGH;

        const char *first_arg = sp ? sp + 1 : NULL;
        while (first_arg && *first_arg == ' ')
            first_arg++;
        if (is_medium_risk_cmd(lower_base, first_arg))
            saw_medium = true;

        seg = seg_end + 1;
    }

    return saw_medium ? HU_RISK_MEDIUM : HU_RISK_LOW;
}

hu_error_t hu_policy_validate_command(const hu_security_policy_t *policy, const char *command,
                                      bool approved, hu_command_risk_level_t *out_risk) {
    if (!policy || !command || !out_risk)
        return HU_ERR_INVALID_ARGUMENT;

    if (!hu_policy_is_command_allowed(policy, command))
        return HU_ERR_SECURITY_COMMAND_NOT_ALLOWED;

    hu_command_risk_level_t risk = hu_policy_command_risk_level(policy, command);

    if (risk == HU_RISK_HIGH) {
        if (policy->block_high_risk_commands)
            return HU_ERR_SECURITY_HIGH_RISK_BLOCKED;
        if (policy->autonomy == HU_AUTONOMY_SUPERVISED && !approved)
            return HU_ERR_SECURITY_APPROVAL_REQUIRED;
    }

    if (risk == HU_RISK_MEDIUM && policy->autonomy == HU_AUTONOMY_SUPERVISED &&
        policy->require_approval_for_medium_risk && !approved)
        return HU_ERR_SECURITY_APPROVAL_REQUIRED;

    *out_risk = risk;
    return HU_OK;
}

bool hu_policy_is_command_allowed(const hu_security_policy_t *policy, const char *command) {
    if (!policy || !command)
        return false;
    if (policy->autonomy == HU_AUTONOMY_READ_ONLY)
        return false;
    if (strlen(command) > HU_MAX_ANALYSIS_LEN)
        return false;

    if (contains_str(command, "`") || contains_str(command, "$(") || contains_str(command, "${"))
        return false;
    if (contains_str(command, "<(") || contains_str(command, ">("))
        return false;
    /* Block "tee" as a command (can write to arbitrary files) */
    if (strstr(command, " tee ") || strstr(command, "|tee") || strstr(command, "tee|") ||
        strncmp(command, "tee ", 4) == 0 || (strlen(command) >= 4 && strcmp(command, "tee") == 0))
        return false;
    if (contains_single_ampersand(command))
        return false;
    if (strchr(command, '>'))
        return false;
    if (strchr(command, '|'))
        return false;

    const char **allowed = policy->allowed_commands;
    size_t allowed_len = policy->allowed_commands_len;
    if (!allowed || allowed_len == 0) {
        allowed = s_default_allowed;
        allowed_len = s_default_allowed_count;
    }

    size_t cmd_len = strlen(command);
    char norm[HU_MAX_ANALYSIS_LEN + 1];
    normalize_command(command, cmd_len, norm);

    bool has_cmd = false;
    const char *seg = norm;
    while (seg < norm + cmd_len) {
        while (seg < norm + cmd_len && (*seg == 0 || *seg == ' ' || *seg == '\t'))
            seg++;
        if (seg >= norm + cmd_len)
            break;

        const char *seg_end = seg;
        while (seg_end < norm + cmd_len && *seg_end)
            seg_end++;

        const char *cmd_part = skip_env_assignments(seg);
        if (cmd_part != seg)
            return false;
        const char *sp = strchr(cmd_part, ' ');
        size_t fw_len = sp ? (size_t)(sp - cmd_part) : strlen(cmd_part);
        if (fw_len == 0) {
            seg = seg_end + 1;
            continue;
        }

        char base[128];
        size_t blen = fw_len < sizeof(base) - 1 ? fw_len : sizeof(base) - 1;
        memcpy(base, cmd_part, blen);
        base[blen] = '\0';
        const char *base_name = basename_ptr(base);
        if (!*base_name) {
            seg = seg_end + 1;
            continue;
        }

        has_cmd = true;
        bool found = false;
        for (size_t i = 0; i < allowed_len; i++) {
            const char *a = allowed[i];
            if (!a)
                continue;
            if (strcmp(a, "*") == 0 || strcasecmp(a, base_name) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        if (!is_args_safe(base_name, cmd_part))
            return false;

        seg = seg_end + 1;
    }
    return has_cmd;
}

bool hu_policy_can_act(const hu_security_policy_t *policy) {
    return policy && policy->autonomy != HU_AUTONOMY_READ_ONLY;
}

bool hu_policy_record_action(hu_security_policy_t *policy) {
    if (!policy)
        return true;
    if (!policy->tracker)
        return true;
    return hu_rate_tracker_record_action(policy->tracker);
}

bool hu_policy_is_rate_limited(const hu_security_policy_t *policy) {
    if (!policy || !policy->tracker)
        return false;
    return hu_rate_tracker_is_limited(policy->tracker);
}
