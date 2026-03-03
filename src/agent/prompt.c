/*
 * System prompt builder — identity, tools, memory, constraints.
 */
#include "seaclaw/agent/prompt.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_PROMPT_INIT_CAP 8192

static sc_error_t append(sc_allocator_t *alloc, char **buf, size_t *len, size_t *cap, const char *s,
                         size_t slen) {
    while (*len + slen + 1 > *cap) {
        size_t new_cap = *cap ? *cap * 2 : SC_PROMPT_INIT_CAP;
        char *nb = (char *)alloc->realloc(alloc->ctx, *buf, *cap, new_cap);
        if (!nb)
            return SC_ERR_OUT_OF_MEMORY;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, slen);
    (*buf)[*len + slen] = '\0';
    *len += slen;
    return SC_OK;
}

sc_error_t sc_prompt_build_system(sc_allocator_t *alloc, const sc_prompt_config_t *config,
                                  char **out, size_t *out_len) {
    if (!alloc || !config || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    size_t cap = SC_PROMPT_INIT_CAP;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    sc_error_t err;

    /* Identity */
    err = append(alloc, &buf, &len, &cap,
                 "You are SeaClaw, an AI assistant. Respond helpfully and concisely.\n\n", 64);
    if (err != SC_OK)
        goto fail;

    if (config->workspace_dir && config->workspace_dir_len > 0) {
        err = append(alloc, &buf, &len, &cap, "Workspace: ", 11);
        if (err != SC_OK)
            goto fail;
        err = append(alloc, &buf, &len, &cap, config->workspace_dir, config->workspace_dir_len);
        if (err != SC_OK)
            goto fail;
        err = append(alloc, &buf, &len, &cap, "\n\n", 2);
        if (err != SC_OK)
            goto fail;
    }

    if (config->provider_name && config->provider_name_len > 0) {
        char line[128];
        int n = snprintf(line, sizeof(line), "Provider: %.*s\n", (int)config->provider_name_len,
                         config->provider_name);
        if (n > 0) {
            err = append(alloc, &buf, &len, &cap, line, (size_t)n);
            if (err != SC_OK)
                goto fail;
        }
    }
    if (config->model_name && config->model_name_len > 0) {
        char line[128];
        int n = snprintf(line, sizeof(line), "Model: %.*s\n", (int)config->model_name_len,
                         config->model_name);
        if (n > 0) {
            err = append(alloc, &buf, &len, &cap, line, (size_t)n);
            if (err != SC_OK)
                goto fail;
        }
    }

    /* Tools section */
    err = append(alloc, &buf, &len, &cap, "## Available Tools\n\n", 20);
    if (err != SC_OK)
        goto fail;
    if (config->tools && config->tools_count > 0) {
        for (size_t i = 0; i < config->tools_count; i++) {
            const sc_tool_t *t = &config->tools[i];
            if (t->vtable && t->vtable->name) {
                const char *name = t->vtable->name(t->ctx);
                if (name) {
                    char line[256];
                    int n = snprintf(line, sizeof(line), "- %s\n", name);
                    if (n > 0) {
                        err = append(alloc, &buf, &len, &cap, line, (size_t)n);
                        if (err != SC_OK)
                            goto fail;
                    }
                }
            }
        }
        err = append(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    } else {
        err = append(alloc, &buf, &len, &cap, "(none)\n\n", 8);
        if (err != SC_OK)
            goto fail;
    }

    /* Memory context */
    err = append(alloc, &buf, &len, &cap, "## Memory Context\n\n", 19);
    if (err != SC_OK)
        goto fail;
    if (config->memory_context && config->memory_context_len > 0) {
        err = append(alloc, &buf, &len, &cap, config->memory_context, config->memory_context_len);
        if (err != SC_OK)
            goto fail;
        err = append(alloc, &buf, &len, &cap, "\n\n", 2);
        if (err != SC_OK)
            goto fail;
    } else {
        err = append(alloc, &buf, &len, &cap, "(none)\n\n", 8);
        if (err != SC_OK)
            goto fail;
    }

    /* Autonomy */
    if (config->autonomy_level == 0) {
        err = append(
            alloc, &buf, &len, &cap,
            "## Rules\n\nYou are in readonly mode. Do not execute tools that modify state.\n\n",
            71);
        if (err != SC_OK)
            goto fail;
    } else if (config->autonomy_level == 1) {
        err = append(alloc, &buf, &len, &cap,
                     "## Rules\n\nYou are in supervised mode. Ask before running destructive or "
                     "high-impact commands.\n\n",
                     89);
        if (err != SC_OK)
            goto fail;
    } else if (config->autonomy_level == 2) {
        err = append(alloc, &buf, &len, &cap,
                     "## Rules\n\nYou are in full autonomy mode. Execute tools as needed.\n\n", 61);
        if (err != SC_OK)
            goto fail;
    }

    /* Safety */
    err = append(alloc, &buf, &len, &cap,
                 "## Safety\n\n- Do not exfiltrate private data.\n- Do not run destructive "
                 "commands without asking.\n- Prefer trash over rm when available.\n\n",
                 114);
    if (err != SC_OK)
        goto fail;

    /* Custom instructions */
    if (config->custom_instructions && config->custom_instructions_len > 0) {
        err = append(alloc, &buf, &len, &cap, config->custom_instructions,
                     config->custom_instructions_len);
        if (err != SC_OK)
            goto fail;
        if (config->custom_instructions[config->custom_instructions_len - 1] != '\n') {
            err = append(alloc, &buf, &len, &cap, "\n", 1);
            if (err != SC_OK)
                goto fail;
        }
    }

    *out = buf;
    *out_len = len;
    return SC_OK;

fail:
    alloc->free(alloc->ctx, buf, cap);
    return err;
}

sc_error_t sc_prompt_build_static(sc_allocator_t *alloc, const sc_prompt_config_t *config,
                                  char **out, size_t *out_len) {
    if (!alloc || !config || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    sc_prompt_config_t no_mem = *config;
    no_mem.memory_context = NULL;
    no_mem.memory_context_len = 0;
    return sc_prompt_build_system(alloc, &no_mem, out, out_len);
}

sc_error_t sc_prompt_build_with_cache(sc_allocator_t *alloc, const char *static_prompt,
                                      size_t static_prompt_len, const char *memory_context,
                                      size_t memory_context_len, char **out, size_t *out_len) {
    if (!alloc || !static_prompt || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    if (!memory_context || memory_context_len == 0) {
        *out = (char *)alloc->alloc(alloc->ctx, static_prompt_len + 1);
        if (!*out)
            return SC_ERR_OUT_OF_MEMORY;
        memcpy(*out, static_prompt, static_prompt_len);
        (*out)[static_prompt_len] = '\0';
        *out_len = static_prompt_len;
        return SC_OK;
    }

    size_t mem_header_len = 19;
    size_t total = static_prompt_len + mem_header_len + memory_context_len + 3;
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    memcpy(buf + pos, static_prompt, static_prompt_len);
    pos += static_prompt_len;
    memcpy(buf + pos, "## Memory Context\n\n", mem_header_len);
    pos += mem_header_len;
    memcpy(buf + pos, memory_context, memory_context_len);
    pos += memory_context_len;
    memcpy(buf + pos, "\n\n", 2);
    pos += 2;
    buf[pos] = '\0';

    *out = buf;
    *out_len = pos;
    return SC_OK;
}
