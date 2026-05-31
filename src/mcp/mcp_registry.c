#include "human/mcp_registry.h"
#include <string.h>

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define HU_MCP_NAME_MAX 63
#define HU_MCP_CMD_MAX  255
#define HU_MCP_ARGS_MAX 511

struct hu_mcp_registry {
    hu_allocator_t *alloc;
    hu_mcp_registry_entry_t entries[HU_MCP_REGISTRY_ENTRIES_MAX];
    int count;
};

static int find_by_name(hu_mcp_registry_t *reg, const char *name) {
    for (int i = 0; i < reg->count; i++)
        if (strcmp(reg->entries[i].name, name) == 0)
            return i;
    return -1;
}

hu_mcp_registry_t *hu_mcp_registry_create(hu_allocator_t *alloc) {
    if (!alloc)
        return NULL;
    hu_mcp_registry_t *reg = (hu_mcp_registry_t *)alloc->alloc(alloc->ctx, sizeof(*reg));
    if (!reg)
        return NULL;
    memset(reg, 0, sizeof(*reg));
    reg->alloc = alloc;
    return reg;
}

void hu_mcp_registry_destroy(hu_mcp_registry_t *reg) {
    if (!reg)
        return;
    for (int i = 0; i < reg->count; i++)
        hu_mcp_registry_stop(reg, reg->entries[i].name);
    reg->alloc->free(reg->alloc->ctx, reg, sizeof(*reg));
}

hu_error_t hu_mcp_registry_add(hu_mcp_registry_t *reg, const char *name, const char *command,
                               const char *args) {
    if (!reg || !name || !command)
        return HU_ERR_INVALID_ARGUMENT;
    if (find_by_name(reg, name) >= 0)
        return HU_ERR_ALREADY_EXISTS;
    if (reg->count >= HU_MCP_REGISTRY_ENTRIES_MAX)
        return HU_ERR_OUT_OF_MEMORY;

    hu_mcp_registry_entry_t *e = &reg->entries[reg->count];
    size_t nlen = strlen(name);
    size_t clen = strlen(command);
    size_t alen = args ? strlen(args) : 0;

    if (nlen > HU_MCP_NAME_MAX || clen > HU_MCP_CMD_MAX || alen > HU_MCP_ARGS_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, nlen + 1);
    memcpy(e->command, command, clen + 1);
    if (args && alen > 0)
        memcpy(e->args, args, alen + 1);
    e->running = false;
    e->pid = -1;
    e->stdin_fd = -1;
    e->stdout_fd = -1;
    reg->count++;
    return HU_OK;
}

hu_error_t hu_mcp_registry_remove(hu_mcp_registry_t *reg, const char *name) {
    if (!reg || !name)
        return HU_ERR_INVALID_ARGUMENT;
    int idx = find_by_name(reg, name);
    if (idx < 0)
        return HU_ERR_NOT_FOUND;

    hu_mcp_registry_stop(reg, name);

    if (idx < reg->count - 1)
        memmove(&reg->entries[idx], &reg->entries[idx + 1],
                (size_t)(reg->count - idx - 1) * sizeof(hu_mcp_registry_entry_t));
    reg->count--;
    return HU_OK;
}

hu_error_t hu_mcp_registry_list(hu_mcp_registry_t *reg, hu_mcp_registry_entry_t *out, int max,
                                int *count) {
    if (!reg || !out || !count || max <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    int n = reg->count < max ? reg->count : max;
    memcpy(out, reg->entries, (size_t)n * sizeof(hu_mcp_registry_entry_t));
    *count = n;
    return HU_OK;
}

hu_error_t hu_mcp_registry_start(hu_mcp_registry_t *reg, const char *name) {
    if (!reg || !name)
        return HU_ERR_INVALID_ARGUMENT;
    int idx = find_by_name(reg, name);
    if (idx < 0)
        return HU_ERR_NOT_FOUND;

    hu_mcp_registry_entry_t *e = &reg->entries[idx];
    if (e->running)
        return HU_OK;

#if defined(HU_IS_TEST) && HU_IS_TEST != 0
    e->running = true;
    e->pid = 12345;
    return HU_OK;
#else
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) != 0)
        return HU_ERR_IO;
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return HU_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return HU_ERR_IO;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        char *argv[32];
        int argc = 0;
        argv[argc++] = (char *)e->command;
        if (e->args[0]) {
            char buf[512];
            memcpy(buf, e->args, sizeof(buf));
            buf[HU_MCP_ARGS_MAX] = '\0';
            char *p = buf;
            while (argc < 31 && *p) {
                while (*p == ' ' || *p == '\t')
                    *p++ = '\0';
                if (!*p)
                    break;
                argv[argc++] = p;
                while (*p && *p != ' ' && *p != '\t')
                    p++;
            }
        }
        argv[argc] = NULL;

        execvp(e->command, argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    e->stdin_fd = stdin_pipe[1];
    e->stdout_fd = stdout_pipe[0];

    e->running = true;
    e->pid = (int)pid;
    return HU_OK;
#endif
}

hu_error_t hu_mcp_registry_stop(hu_mcp_registry_t *reg, const char *name) {
    if (!reg || !name)
        return HU_ERR_INVALID_ARGUMENT;
    int idx = find_by_name(reg, name);
    if (idx < 0)
        return HU_ERR_NOT_FOUND;

    hu_mcp_registry_entry_t *e = &reg->entries[idx];
    if (!e->running) {
        e->pid = -1;
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST != 0
    e->running = false;
    e->pid = -1;
    return HU_OK;
#else
    if (e->pid > 0) {
        kill((pid_t)e->pid, SIGTERM);
        waitpid((pid_t)e->pid, NULL, 0);
    }
    if (e->stdin_fd >= 0) {
        close(e->stdin_fd);
        e->stdin_fd = -1;
    }
    if (e->stdout_fd >= 0) {
        close(e->stdout_fd);
        e->stdout_fd = -1;
    }
    e->running = false;
    e->pid = -1;
    return HU_OK;
#endif
}
