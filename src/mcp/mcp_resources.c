#include "human/mcp_resources.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

void hu_mcp_resource_registry_init(hu_mcp_resource_registry_t *reg) {
    if (!reg)
        return;
    memset(reg, 0, sizeof(*reg));
}

hu_error_t hu_mcp_resource_register(hu_mcp_resource_registry_t *reg, const char *uri,
                                    const char *name, const char *description,
                                    const char *mime_type) {
    if (!reg || !uri || !name)
        return HU_ERR_INVALID_ARGUMENT;
    if (reg->resource_count >= HU_MCP_MAX_RESOURCES)
        return HU_ERR_OUT_OF_MEMORY;

    hu_mcp_resource_t *r = &reg->resources[reg->resource_count];
    memset(r, 0, sizeof(*r));
    snprintf(r->uri, sizeof(r->uri), "%s", uri);
    snprintf(r->name, sizeof(r->name), "%s", name);
    if (description)
        snprintf(r->description, sizeof(r->description), "%s", description);
    if (mime_type)
        snprintf(r->mime_type, sizeof(r->mime_type), "%s", mime_type);

    reg->resource_count++;
    return HU_OK;
}

hu_error_t hu_mcp_resource_template_register(hu_mcp_resource_registry_t *reg,
                                             const char *uri_template, const char *name,
                                             const char *description, const char *mime_type) {
    if (!reg || !uri_template || !name)
        return HU_ERR_INVALID_ARGUMENT;
    if (reg->template_count >= HU_MCP_MAX_RESOURCES)
        return HU_ERR_OUT_OF_MEMORY;

    hu_mcp_resource_template_t *t = &reg->templates[reg->template_count];
    memset(t, 0, sizeof(*t));
    snprintf(t->uri_template, sizeof(t->uri_template), "%s", uri_template);
    snprintf(t->name, sizeof(t->name), "%s", name);
    if (description)
        snprintf(t->description, sizeof(t->description), "%s", description);
    if (mime_type)
        snprintf(t->mime_type, sizeof(t->mime_type), "%s", mime_type);

    reg->template_count++;
    return HU_OK;
}

hu_error_t hu_mcp_resource_list_json(hu_allocator_t *alloc, const hu_mcp_resource_registry_t *reg,
                                     char **out_json, size_t *out_len) {
    if (!alloc || !reg || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t buf_size = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_size);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos = hu_buf_appendf(buf, buf_size, pos, "{\"resources\":[");

    for (size_t i = 0; i < reg->resource_count; i++) {
        if (i > 0)
            pos = hu_buf_appendf(buf, buf_size, pos, ",");
        pos = hu_buf_appendf(buf, buf_size, pos,
                             "{\"uri\":\"%s\",\"name\":\"%s\",\"description\":\"%s\",\"mimeType\":\"%s\"}",
                             reg->resources[i].uri, reg->resources[i].name,
                             reg->resources[i].description, reg->resources[i].mime_type);
    }

    pos = hu_buf_appendf(buf, buf_size, pos, "]}");
    buf[pos] = '\0';
    *out_json = buf;
    *out_len = pos;
    return HU_OK;
}

/* MCP Prompts */

void hu_mcp_prompt_registry_init(hu_mcp_prompt_registry_t *reg) {
    if (!reg)
        return;
    memset(reg, 0, sizeof(*reg));
}

hu_error_t hu_mcp_prompt_register(hu_mcp_prompt_registry_t *reg, const hu_mcp_prompt_t *prompt) {
    if (!reg || !prompt)
        return HU_ERR_INVALID_ARGUMENT;
    if (reg->prompt_count >= HU_MCP_MAX_PROMPTS)
        return HU_ERR_OUT_OF_MEMORY;
    reg->prompts[reg->prompt_count++] = *prompt;
    return HU_OK;
}

hu_error_t hu_mcp_prompt_render(hu_allocator_t *alloc, const hu_mcp_prompt_t *prompt,
                                const char *const *arg_names, const char *const *arg_values,
                                size_t arg_count, char **out_text, size_t *out_len) {
    if (!alloc || !prompt || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* Start with the template text and substitute {{name}} placeholders */
    size_t tpl_len = prompt->template_text_len;
    if (tpl_len == 0)
        tpl_len = strlen(prompt->template_text);

    size_t buf_size = tpl_len * 2 + 1024;
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_size);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    size_t i = 0;
    while (i < tpl_len && pos < buf_size - 1) {
        if (i + 1 < tpl_len && prompt->template_text[i] == '{' &&
            prompt->template_text[i + 1] == '{') {
            /* Find closing }} */
            size_t end = i + 2;
            while (end + 1 < tpl_len &&
                   !(prompt->template_text[end] == '}' && prompt->template_text[end + 1] == '}'))
                end++;
            if (end + 1 < tpl_len) {
                size_t name_start = i + 2;
                size_t name_len = end - name_start;
                bool replaced = false;
                for (size_t a = 0; a < arg_count; a++) {
                    if (arg_names[a] && strlen(arg_names[a]) == name_len &&
                        memcmp(arg_names[a], prompt->template_text + name_start, name_len) == 0) {
                        size_t vlen = arg_values[a] ? strlen(arg_values[a]) : 0;
                        if (pos + vlen < buf_size) {
                            memcpy(buf + pos, arg_values[a], vlen);
                            pos += vlen;
                        }
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    /* Leave placeholder as-is */
                    size_t span = end + 2 - i;
                    if (pos + span < buf_size) {
                        memcpy(buf + pos, prompt->template_text + i, span);
                        pos += span;
                    }
                }
                i = end + 2;
                continue;
            }
        }
        buf[pos++] = prompt->template_text[i++];
    }
    buf[pos] = '\0';
    *out_text = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_mcp_prompt_list_json(hu_allocator_t *alloc, const hu_mcp_prompt_registry_t *reg,
                                   char **out_json, size_t *out_len) {
    if (!alloc || !reg || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t buf_size = 8192;
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_size);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos = hu_buf_appendf(buf, buf_size, pos, "{\"prompts\":[");

    for (size_t i = 0; i < reg->prompt_count; i++) {
        if (i > 0)
            pos = hu_buf_appendf(buf, buf_size, pos, ",");
        pos = hu_buf_appendf(buf, buf_size, pos,
                             "{\"name\":\"%s\",\"description\":\"%s\",\"arguments\":[",
                             reg->prompts[i].name, reg->prompts[i].description);

        for (size_t a = 0; a < reg->prompts[i].argument_count; a++) {
            if (a > 0)
                pos = hu_buf_appendf(buf, buf_size, pos, ",");
            pos = hu_buf_appendf(buf, buf_size, pos,
                                 "{\"name\":\"%s\",\"description\":\"%s\",\"required\":%s}",
                                 reg->prompts[i].arguments[a].name,
                                 reg->prompts[i].arguments[a].description,
                                 reg->prompts[i].arguments[a].required ? "true" : "false");
        }
        pos = hu_buf_appendf(buf, buf_size, pos, "]}");
    }

    pos = hu_buf_appendf(buf, buf_size, pos, "]}");
    buf[pos] = '\0';
    *out_json = buf;
    *out_len = pos;
    return HU_OK;
}
