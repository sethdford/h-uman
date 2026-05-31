/* Hook registry and shell execution */
#include "human/hook.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HU_IS_TEST
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Hook registry internals
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_HOOK_REGISTRY_INITIAL_CAP 8
#define HU_HOOK_MAX_STDOUT (1024 * 1024)  /* 1MB limit to prevent DoS */

struct hu_hook_registry {
    hu_hook_entry_t *entries;
    size_t count;
    size_t cap;
};

hu_error_t hu_hook_registry_create(hu_allocator_t *alloc, hu_hook_registry_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_hook_registry_t *reg = alloc->alloc(alloc->ctx, sizeof(hu_hook_registry_t));
    if (!reg)
        return HU_ERR_OUT_OF_MEMORY;

    reg->entries = alloc->alloc(alloc->ctx, HU_HOOK_REGISTRY_INITIAL_CAP * sizeof(hu_hook_entry_t));
    if (!reg->entries) {
        alloc->free(alloc->ctx, reg, sizeof(hu_hook_registry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    reg->count = 0;
    reg->cap = HU_HOOK_REGISTRY_INITIAL_CAP;
    *out = reg;
    return HU_OK;
}

static char *hu_hook_strdup(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s || len == 0)
        return NULL;
    char *dup = alloc->alloc(alloc->ctx, len + 1);
    if (!dup)
        return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

hu_error_t hu_hook_registry_add(hu_hook_registry_t *reg, hu_allocator_t *alloc,
                                const hu_hook_entry_t *entry) {
    if (!reg || !alloc || !entry)
        return HU_ERR_INVALID_ARGUMENT;
    if (!entry->command || entry->command_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Grow if needed */
    if (reg->count >= reg->cap) {
        size_t new_cap = reg->cap * 2;
        hu_hook_entry_t *new_entries = alloc->realloc(
            alloc->ctx, reg->entries, reg->cap * sizeof(hu_hook_entry_t),
            new_cap * sizeof(hu_hook_entry_t));
        if (!new_entries)
            return HU_ERR_OUT_OF_MEMORY;
        reg->entries = new_entries;
        reg->cap = new_cap;
    }

    hu_hook_entry_t *e = &reg->entries[reg->count];
    memset(e, 0, sizeof(*e));

    e->name = hu_hook_strdup(alloc, entry->name, entry->name_len);
    e->name_len = entry->name_len;
    e->event = entry->event;
    e->command = hu_hook_strdup(alloc, entry->command, entry->command_len);
    e->command_len = entry->command_len;
    if (!e->command) {
        if (e->name)
            alloc->free(alloc->ctx, e->name, e->name_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    e->timeout_sec = entry->timeout_sec ? entry->timeout_sec : 30;
    e->required = entry->required;

    reg->count++;
    return HU_OK;
}

size_t hu_hook_registry_count(const hu_hook_registry_t *reg) {
    return reg ? reg->count : 0;
}

const hu_hook_entry_t *hu_hook_registry_get(const hu_hook_registry_t *reg, size_t index) {
    if (!reg || index >= reg->count)
        return NULL;
    return &reg->entries[index];
}

void hu_hook_registry_destroy(hu_hook_registry_t *reg, hu_allocator_t *alloc) {
    if (!reg || !alloc)
        return;
    for (size_t i = 0; i < reg->count; i++) {
        hu_hook_entry_t *e = &reg->entries[i];
        if (e->name)
            alloc->free(alloc->ctx, e->name, e->name_len + 1);
        if (e->command)
            alloc->free(alloc->ctx, e->command, e->command_len + 1);
    }
    if (reg->entries)
        alloc->free(alloc->ctx, reg->entries, reg->cap * sizeof(hu_hook_entry_t));
    alloc->free(alloc->ctx, reg, sizeof(hu_hook_registry_t));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Shell escape: wrap in single quotes, escape embedded single quotes
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_hook_shell_escape(hu_allocator_t *alloc, const char *input, size_t input_len,
                                char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!input || input_len == 0) {
        char *empty = alloc->alloc(alloc->ctx, 3);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\'';
        empty[1] = '\'';
        empty[2] = '\0';
        *out = empty;
        *out_len = 2;
        return HU_OK;
    }

    /* Count single quotes to size the output */
    size_t sq_count = 0;
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '\'')
            sq_count++;
    }

    /* 'content' with each ' replaced by '\'' */
    size_t needed = 2 + input_len + sq_count * 3; /* 2 for outer quotes, +3 per ' -> '\'' */
    char *buf = alloc->alloc(alloc->ctx, needed + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    buf[pos++] = '\'';
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '\'') {
            buf[pos++] = '\'';  /* end current quote */
            buf[pos++] = '\\';
            buf[pos++] = '\'';  /* escaped literal quote */
            buf[pos++] = '\'';  /* reopen quote */
        } else {
            buf[pos++] = input[i];
        }
    }
    buf[pos++] = '\'';
    buf[pos] = '\0';

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Dangerous environment variables to strip
 * ────────────────────────────────────────────────────────────────────────── */

static const char *const HU_HOOK_DANGEROUS_ENV_PREFIXES[] = {
    "LD_PRELOAD",
    "LD_LIBRARY_PATH",
    "DYLD_INSERT_LIBRARIES",
    "DYLD_LIBRARY_PATH",
    "SUDO_ASKPASS",
    "SUDO_COMMAND",
    "SUDO_USER",
    "SUDO_GID",
    "SUDO_UID",
    NULL,
};

__attribute__((unused))
static bool hu_hook_is_dangerous_env(const char *var) {
    for (const char *const *p = HU_HOOK_DANGEROUS_ENV_PREFIXES; *p; p++) {
        size_t plen = strlen(*p);
        if (strncmp(var, *p, plen) == 0 && (var[plen] == '=' || var[plen] == '\0'))
            return true;
    }
    /* Block PATH overrides */
    if (strncmp(var, "PATH=", 5) == 0)
        return true;
    return false;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Test mock implementation
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef HU_IS_TEST

static hu_hook_mock_config_t hu_hook_mock_single = {.exit_code = 0, .stdout_data = NULL, .stdout_len = 0};
static const hu_hook_mock_config_t *hu_hook_mock_seq = NULL;
static size_t hu_hook_mock_seq_count = 0;
static size_t hu_hook_mock_seq_index = 0;
static size_t hu_hook_mock_calls = 0;
static char hu_hook_mock_last_cmd[4096];

void hu_hook_mock_set(const hu_hook_mock_config_t *config) {
    if (config) {
        hu_hook_mock_single = *config;
    } else {
        hu_hook_mock_single.exit_code = 0;
        hu_hook_mock_single.stdout_data = NULL;
        hu_hook_mock_single.stdout_len = 0;
    }
    hu_hook_mock_seq = NULL;
    hu_hook_mock_seq_count = 0;
    hu_hook_mock_seq_index = 0;
}

void hu_hook_mock_set_sequence(const hu_hook_mock_config_t *configs, size_t count) {
    hu_hook_mock_seq = configs;
    hu_hook_mock_seq_count = count;
    hu_hook_mock_seq_index = 0;
}

void hu_hook_mock_reset(void) {
    hu_hook_mock_single.exit_code = 0;
    hu_hook_mock_single.stdout_data = NULL;
    hu_hook_mock_single.stdout_len = 0;
    hu_hook_mock_seq = NULL;
    hu_hook_mock_seq_count = 0;
    hu_hook_mock_seq_index = 0;
    hu_hook_mock_calls = 0;
    hu_hook_mock_last_cmd[0] = '\0';
}

size_t hu_hook_mock_call_count(void) {
    return hu_hook_mock_calls;
}

const char *hu_hook_mock_last_command(void) {
    return hu_hook_mock_last_cmd;
}

static const hu_hook_mock_config_t *hu_hook_mock_next(void) {
    if (hu_hook_mock_seq && hu_hook_mock_seq_index < hu_hook_mock_seq_count) {
        return &hu_hook_mock_seq[hu_hook_mock_seq_index++];
    }
    return &hu_hook_mock_single;
}

#endif /* HU_IS_TEST */

/* ──────────────────────────────────────────────────────────────────────────
 * Build the full shell command with escaped context vars
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_HOOK_CMD_MAX ((size_t)(1u << 20)) /* 1 MiB cap for hook command string */

static bool hu_hook_size_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static hu_error_t hu_hook_build_command(hu_allocator_t *alloc, const hu_hook_entry_t *hook,
                                        const hu_hook_context_t *ctx, char **cmd_out,
                                        size_t *cmd_len_out) {
    /* Escape all context values */
    char *esc_tool = NULL, *esc_args = NULL, *esc_output = NULL;
    size_t esc_tool_len = 0, esc_args_len = 0, esc_output_len = 0;

    hu_error_t err = hu_hook_shell_escape(alloc, ctx->tool_name, ctx->tool_name_len,
                                          &esc_tool, &esc_tool_len);
    if (err != HU_OK)
        return err;

    err = hu_hook_shell_escape(alloc, ctx->args_json, ctx->args_json_len,
                               &esc_args, &esc_args_len);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
        return err;
    }

    if (ctx->result_output && ctx->result_output_len > 0) {
        err = hu_hook_shell_escape(alloc, ctx->result_output, ctx->result_output_len,
                                   &esc_output, &esc_output_len);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
            alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
            return err;
        }
    }

    /* Build: HOOK_TOOL_NAME=<escaped> HOOK_ARGS=<escaped> [HOOK_OUTPUT=<escaped>] <command> */
    size_t total = 16;
    if (!hu_hook_size_add(total, esc_tool_len, &total) || !hu_hook_size_add(total, 12, &total) ||
        !hu_hook_size_add(total, esc_args_len, &total) ||
        !hu_hook_size_add(total, hook->command_len, &total) || !hu_hook_size_add(total, 8, &total)) {
        alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
        alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
        if (esc_output)
            alloc->free(alloc->ctx, esc_output, esc_output_len + 1);
        return HU_ERR_LIMIT_REACHED;
    }
    if (esc_output) {
        size_t extra = 14 + esc_output_len + 32; /* HOOK_OUTPUT= + HOOK_SUCCESS= */
        if (!hu_hook_size_add(total, extra, &total)) {
            alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
            alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
            alloc->free(alloc->ctx, esc_output, esc_output_len + 1);
            return HU_ERR_LIMIT_REACHED;
        }
    }
    if (total > HU_HOOK_CMD_MAX) {
        alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
        alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
        if (esc_output)
            alloc->free(alloc->ctx, esc_output, esc_output_len + 1);
        return HU_ERR_LIMIT_REACHED;
    }

    char *cmd = alloc->alloc(alloc->ctx, total + 1);
    if (!cmd) {
        alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
        alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
        if (esc_output)
            alloc->free(alloc->ctx, esc_output, esc_output_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    int written;
    if (esc_output) {
        written = snprintf(cmd, total + 1,
                           "HOOK_TOOL_NAME=%s HOOK_ARGS=%s HOOK_OUTPUT=%s HOOK_SUCCESS=%s %.*s",
                           esc_tool, esc_args, esc_output,
                           ctx->result_success ? "true" : "false",
                           (int)hook->command_len, hook->command);
    } else {
        written = snprintf(cmd, total + 1,
                           "HOOK_TOOL_NAME=%s HOOK_ARGS=%s %.*s",
                           esc_tool, esc_args,
                           (int)hook->command_len, hook->command);
    }

    if (written < 0 || (size_t)written > total) {
        alloc->free(alloc->ctx, cmd, total + 1);
        alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
        alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
        if (esc_output)
            alloc->free(alloc->ctx, esc_output, esc_output_len + 1);
        *cmd_out = NULL;
        *cmd_len_out = 0;
        return HU_ERR_LIMIT_REACHED;
    }

    alloc->free(alloc->ctx, esc_tool, esc_tool_len + 1);
    alloc->free(alloc->ctx, esc_args, esc_args_len + 1);
    if (esc_output)
        alloc->free(alloc->ctx, esc_output, esc_output_len + 1);

    *cmd_out = cmd;
    *cmd_len_out = (size_t)written;
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Shell execution
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_hook_shell_execute(hu_allocator_t *alloc, const hu_hook_entry_t *hook,
                                 const hu_hook_context_t *ctx, int *exit_code,
                                 char **stdout_buf, size_t *stdout_len) {
    if (!alloc || !hook || !ctx || !exit_code || !stdout_buf || !stdout_len)
        return HU_ERR_INVALID_ARGUMENT;

    *exit_code = -1;
    *stdout_buf = NULL;
    *stdout_len = 0;

    /* Build the full command */
    char *cmd = NULL;
    size_t cmd_len = 0;
    hu_error_t err = hu_hook_build_command(alloc, hook, ctx, &cmd, &cmd_len);
    if (err != HU_OK)
        return err;

#ifdef HU_IS_TEST
    /* Mock execution */
    hu_hook_mock_calls++;
    size_t copy_len = cmd_len < sizeof(hu_hook_mock_last_cmd) - 1
                          ? cmd_len
                          : sizeof(hu_hook_mock_last_cmd) - 1;
    memcpy(hu_hook_mock_last_cmd, cmd, copy_len);
    hu_hook_mock_last_cmd[copy_len] = '\0';
    alloc->free(alloc->ctx, cmd, cmd_len + 1);

    const hu_hook_mock_config_t *mock = hu_hook_mock_next();
    *exit_code = mock->exit_code;
    if (mock->stdout_data && mock->stdout_len > 0) {
        char *out = alloc->alloc(alloc->ctx, mock->stdout_len + 1);
        if (!out)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(out, mock->stdout_data, mock->stdout_len);
        out[mock->stdout_len] = '\0';
        *stdout_buf = out;
        *stdout_len = mock->stdout_len;
    }
    return HU_OK;

#else
    /* Real execution via fork/exec with sanitized environment and timeout */
    (void)hu_hook_is_dangerous_env; /* used below in child */

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        alloc->free(alloc->ctx, cmd, cmd_len + 1);
        return HU_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        alloc->free(alloc->ctx, cmd, cmd_len + 1);
        return HU_ERR_IO;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe, sanitize env, exec */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Sanitize dangerous environment variables */
        extern char **environ;

        /* Collect names without malloc when short; heap only for long names. Never skip short names
         * on OOM (previous code dropped sanitization entirely if malloc failed). */
        char stack_names[32][256];
        char *heap_names[32];
        size_t stack_count = 0;
        size_t heap_count = 0;
        for (char **env = environ; env && *env && (stack_count + heap_count) < 32; env++) {
            if (!hu_hook_is_dangerous_env(*env))
                continue;
            const char *eq = strchr(*env, '=');
            if (!eq)
                continue;
            size_t nlen = (size_t)(eq - *env);
            if (nlen < sizeof(stack_names[0])) {
                memcpy(stack_names[stack_count], *env, nlen);
                stack_names[stack_count][nlen] = '\0';
                stack_count++;
            } else {
                char *name_copy = malloc(nlen + 1);
                if (name_copy) {
                    memcpy(name_copy, *env, nlen);
                    name_copy[nlen] = '\0';
                    heap_names[heap_count++] = name_copy;
                }
            }
        }

        for (size_t i = 0; i < stack_count; i++)
            unsetenv(stack_names[i]);
        for (size_t i = 0; i < heap_count; i++) {
            unsetenv(heap_names[i]);
            free(heap_names[i]);
        }

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent: read stdout and wait with timeout */
    close(pipefd[1]);

    /* Declare status early for limit check below */
    int status = 0;

    /* Read stdout with 1MB limit to prevent DoS */
    size_t buf_cap = 4096;
    char *buf = alloc->alloc(alloc->ctx, buf_cap);
    size_t buf_used = 0;
    bool stdout_limit_exceeded = false;
    if (buf) {
        ssize_t n;
        while ((n = read(pipefd[0], buf + buf_used, buf_cap - buf_used - 1)) > 0) {
            buf_used += (size_t)n;

            /* Check if we've exceeded the stdout limit */
            if (buf_used >= HU_HOOK_MAX_STDOUT) {
                stdout_limit_exceeded = true;
                break;
            }

            if (buf_used >= buf_cap - 1) {
                /* Calculate next capacity, but cap it at HU_HOOK_MAX_STDOUT */
                size_t new_cap = buf_cap * 2;
                if (new_cap > HU_HOOK_MAX_STDOUT)
                    new_cap = HU_HOOK_MAX_STDOUT;
                char *new_buf = alloc->realloc(alloc->ctx, buf, buf_cap, new_cap);
                if (!new_buf)
                    break;
                buf = new_buf;
                buf_cap = new_cap;
            }
        }
        buf[buf_used] = '\0';
    }
    close(pipefd[0]);

    /* Kill process if stdout limit was exceeded */
    if (stdout_limit_exceeded) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        if (buf)
            alloc->free(alloc->ctx, buf, buf_cap);
        alloc->free(alloc->ctx, cmd, cmd_len + 1);
        return HU_ERR_IO;
    }

    /* Wait with timeout: SIGTERM after timeout_sec, SIGKILL 2s later */
    uint32_t timeout = hook->timeout_sec ? hook->timeout_sec : 30;
    pid_t waited = 0;

    for (uint32_t elapsed = 0; elapsed < timeout; elapsed++) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited != 0)
            break;
        sleep(1);
    }

    if (waited == 0) {
        /* Timeout: send SIGTERM */
        kill(pid, SIGTERM);
        sleep(2);
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == 0) {
            /* Still alive: SIGKILL */
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        if (buf)
            alloc->free(alloc->ctx, buf, buf_cap);
        *exit_code = -1;
        alloc->free(alloc->ctx, cmd, cmd_len + 1);
        return HU_ERR_TIMEOUT;
    }

    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else {
        *exit_code = -1;
    }

    if (buf && buf_used > 0) {
        *stdout_buf = buf;
        *stdout_len = buf_used;
    } else if (buf) {
        alloc->free(alloc->ctx, buf, buf_cap);
    }

    alloc->free(alloc->ctx, cmd, cmd_len + 1);
    return HU_OK;
#endif /* HU_IS_TEST */
}
