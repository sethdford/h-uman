#include "human/heartbeat.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_HEARTBEAT_MIN_INTERVAL 5
#define HU_HEARTBEAT_PATH         "HEARTBEAT.md"
#define HU_HEARTBEAT_MAX_CONTENT  65536

static bool is_markdown_header(const char *line) {
    size_t i = 0;
    while (line[i] == '#')
        i++;
    if (i == 0)
        return false;
    if (i == strlen(line))
        return true;
    return line[i] == ' ' || line[i] == '\t';
}

static bool is_empty_bullet(const char *line) {
    if (!line[0])
        return false;
    char c = line[0];
    if (c != '-' && c != '*' && c != '+')
        return false;
    const char *rest = line + 1;
    while (*rest == ' ' || *rest == '\t')
        rest++;
    if (!*rest)
        return true;
    if (strncmp(rest, "[ ]", 3) == 0 || strncmp(rest, "[x]", 3) == 0 ||
        strncmp(rest, "[X]", 3) == 0) {
        rest += 3;
        while (*rest == ' ' || *rest == '\t')
            rest++;
        return !*rest;
    }
    return false;
}

void hu_heartbeat_engine_init(hu_heartbeat_engine_t *engine, bool enabled,
                              uint32_t interval_minutes, const char *workspace_dir) {
    if (!engine)
        return;
    memset(engine, 0, sizeof(*engine));
    engine->enabled = enabled;
    engine->interval_minutes =
        interval_minutes < HU_HEARTBEAT_MIN_INTERVAL ? HU_HEARTBEAT_MIN_INTERVAL : interval_minutes;
    engine->workspace_dir = workspace_dir;
}

static const char *trim_left(const char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

hu_error_t hu_heartbeat_parse_tasks(hu_allocator_t *alloc, const char *content, char ***tasks_out,
                                    size_t *out_count) {
    if (!alloc || !content || !tasks_out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *tasks_out = NULL;
    *out_count = 0;
    char **list = alloc->alloc(alloc->ctx, 64 * sizeof(char *));
    if (!list)
        return HU_ERR_OUT_OF_MEMORY;
    size_t cap = 64, count = 0;
    const char *p = content;
    for (;;) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end == p) {
            if (!nl)
                break;
            p = nl + 1;
            continue;
        }
        char line[2048];
        size_t len = (size_t)(end - p);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        const char *trimmed = trim_left(line);
        while (*trimmed == ' ' || *trimmed == '\t')
            trimmed++;
        if (strncmp(trimmed, "- ", 2) == 0) {
            const char *task = trim_left(trimmed + 2);
            if (*task) {
                size_t tlen = strlen(task);
                char *dup = alloc->alloc(alloc->ctx, tlen + 1);
                if (dup) {
                    memcpy(dup, task, tlen + 1);
                    if (count >= cap) {
                        char **n = alloc->realloc(alloc->ctx, list, cap * sizeof(char *),
                                                  cap * 2 * sizeof(char *));
                        if (!n) {
                            alloc->free(alloc->ctx, dup, strlen(task) + 1);
                            break;
                        }
                        list = n;
                        cap *= 2;
                    }
                    list[count++] = dup;
                }
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (count == 0) {
        alloc->free(alloc->ctx, list, cap * sizeof(char *));
        *tasks_out = NULL;
    } else {
        *tasks_out = list;
    }
    *out_count = count;
    return HU_OK;
}

void hu_heartbeat_free_tasks(hu_allocator_t *alloc, char **tasks, size_t count) {
    if (!alloc || !tasks)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tasks[i])
            alloc->free(alloc->ctx, tasks[i], strlen(tasks[i]) + 1);
    }
    alloc->free(alloc->ctx, tasks, count * sizeof(char *));
}

bool hu_heartbeat_is_empty_content(const char *content) {
    if (!content)
        return true;
    const char *p = content;
    for (;;) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end <= p)
            return true;
        char line[512];
        size_t len = (size_t)(end - p);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        const char *t = trim_left(line);
        if (*t && !is_markdown_header(t) && !is_empty_bullet(t))
            return false;
        if (!nl)
            return true;
        p = nl + 1;
    }
}

hu_error_t hu_heartbeat_collect_tasks(hu_heartbeat_engine_t *engine, hu_allocator_t *alloc,
                                      char ***tasks_out, size_t *out_count) {
    if (!engine || !alloc || !tasks_out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *tasks_out = NULL;
    *out_count = 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", engine->workspace_dir, HU_HEARTBEAT_PATH);
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_OK; /* No file = empty */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)HU_HEARTBEAT_MAX_CONTENT) {
        fclose(f);
        return HU_OK;
    }
    char *buf = alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    if (hu_heartbeat_is_empty_content(buf)) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_OK;
    }
    hu_error_t err = hu_heartbeat_parse_tasks(alloc, buf, tasks_out, out_count);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    return err;
}

hu_error_t hu_heartbeat_tick(hu_heartbeat_engine_t *engine, hu_allocator_t *alloc,
                             hu_heartbeat_result_t *result) {
    if (!engine || !alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    result->outcome = HU_HEARTBEAT_SKIPPED_MISSING;
    result->task_count = 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", engine->workspace_dir, HU_HEARTBEAT_PATH);
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_OK;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)HU_HEARTBEAT_MAX_CONTENT) {
        fclose(f);
        return HU_OK;
    }
    char *buf = alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    if (hu_heartbeat_is_empty_content(buf)) {
        result->outcome = HU_HEARTBEAT_SKIPPED_EMPTY;
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_OK;
    }
    char **tasks = NULL;
    size_t count = 0;
    hu_error_t err = hu_heartbeat_parse_tasks(alloc, buf, &tasks, &count);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK)
        return err;
    result->outcome = HU_HEARTBEAT_PROCESSED;
    result->task_count = count;
    hu_heartbeat_free_tasks(alloc, tasks, count);
    return HU_OK;
}

hu_error_t hu_heartbeat_ensure_file(const char *workspace_dir, hu_allocator_t *alloc) {
    (void)alloc;
    if (!workspace_dir)
        return HU_ERR_INVALID_ARGUMENT;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", workspace_dir, HU_HEARTBEAT_PATH);
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return HU_OK;
    }
    f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;
    fputs("# Periodic Tasks\n\n"
          "# Add tasks below (one per line, starting with `- `)\n"
          "# The agent will check this file on each heartbeat tick.\n",
          f);
    fclose(f);
    return HU_OK;
}
