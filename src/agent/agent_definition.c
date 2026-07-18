#include "human/agent/agent_definition.h"
#include "human/core/file.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_bounds(const char *start, const char *end, const char **out_lo,
                        const char **out_hi) {
    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    *out_lo = start;
    *out_hi = end;
}

static char *dup_range(hu_allocator_t *alloc, const char *lo, const char *hi) {
    size_t n = (size_t)(hi - lo);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1u);
    if (!buf)
        return NULL;
    memcpy(buf, lo, n);
    buf[n] = '\0';
    return buf;
}

static hu_error_t append_cstr_array(hu_allocator_t *alloc, char ***arr, size_t *count, size_t *cap,
                                    char *item) {
    if (!item)
        return HU_ERR_OUT_OF_MEMORY;
    if (*count >= *cap) {
        size_t ncap = *cap ? (*cap * 2u) : 8u;
        size_t old_bytes = (*cap) * sizeof(char *);
        size_t new_bytes = ncap * sizeof(char *);
        char **next = (char **)alloc->realloc(alloc->ctx, *arr, old_bytes, new_bytes);
        if (!next) {
            alloc->free(alloc->ctx, item, strlen(item) + 1u);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *arr = next;
        *cap = ncap;
    }
    (*arr)[*count] = item;
    (*count)++;
    return HU_OK;
}

static hu_error_t read_file_if_present(hu_allocator_t *alloc, const char *path, char **out_text) {
    hu_error_t err = hu_file_slurp(alloc, path, 0, out_text, NULL);
    if (err == HU_ERR_NOT_FOUND) {
        *out_text = NULL;
        return HU_OK;
    }
    return err;
}

static hu_error_t parse_soul(hu_allocator_t *alloc, const char *text, hu_agent_definition_t *out) {
    if (!text || !text[0]) {
        out->soul_name = NULL;
        out->soul_voice = NULL;
        out->soul_traits = NULL;
        out->soul_traits_count = 0;
        out->soul_body = NULL;
        return HU_OK;
    }

    const char *body_start = text;
    const char *fm_lo = NULL;
    const char *fm_hi = NULL;

    if (strncmp(text, "---", 3) == 0) {
        const char *p = text + 3;
        if (*p == '\r')
            p++;
        if (*p == '\n')
            p++;
        const char *q = strstr(p, "\n---");
        if (q) {
            fm_lo = p;
            fm_hi = q;
            body_start = q + (sizeof("\n---") - 1u);
            if (*body_start == '\r')
                body_start++;
            if (*body_start == '\n')
                body_start++;
        }
    }

    if (!fm_lo) {
        out->soul_name = NULL;
        out->soul_voice = NULL;
        out->soul_traits = NULL;
        out->soul_traits_count = 0;
        out->soul_body = hu_strdup(alloc, text);
        if (!out->soul_body)
            return HU_ERR_OUT_OF_MEMORY;
        return HU_OK;
    }

    out->soul_name = NULL;
    out->soul_voice = NULL;
    out->soul_traits = NULL;
    out->soul_traits_count = 0;
    size_t traits_cap = 0;

    for (const char *line = fm_lo; line < fm_hi;) {
        const char *eol = memchr(line, '\n', (size_t)(fm_hi - line));
        const char *end = eol ? eol : fm_hi;
        const char *klo = NULL;
        const char *khi = NULL;
        trim_bounds(line, end, &klo, &khi);
        if (klo >= khi) {
            line = eol ? eol + 1 : fm_hi;
            continue;
        }
        const char *colon = memchr(klo, ':', (size_t)(khi - klo));
        if (!colon || colon >= khi) {
            line = eol ? eol + 1 : fm_hi;
            continue;
        }
        const char *key_lo = NULL;
        const char *key_hi = NULL;
        trim_bounds(klo, colon, &key_lo, &key_hi);
        const char *val_lo = NULL;
        const char *val_hi = NULL;
        trim_bounds(colon + 1, khi, &val_lo, &val_hi);

        size_t key_len = (size_t)(key_hi - key_lo);
        line = eol ? eol + 1 : fm_hi;

        if (key_len == 4 && strncmp(key_lo, "name", 4) == 0) {
            out->soul_name = dup_range(alloc, val_lo, val_hi);
            if (!out->soul_name)
                return HU_ERR_OUT_OF_MEMORY;
        } else if (key_len == 5 && strncmp(key_lo, "voice", 5) == 0) {
            out->soul_voice = dup_range(alloc, val_lo, val_hi);
            if (!out->soul_voice)
                return HU_ERR_OUT_OF_MEMORY;
        } else if (key_len == 6 && strncmp(key_lo, "traits", 6) == 0) {
            for (const char *t = val_lo; t < val_hi;) {
                const char *comma = memchr(t, ',', (size_t)(val_hi - t));
                const char *tend = comma ? comma : val_hi;
                const char *a = NULL;
                const char *b = NULL;
                trim_bounds(t, tend, &a, &b);
                if (a < b) {
                    char *one = dup_range(alloc, a, b);
                    hu_error_t er = append_cstr_array(alloc, &out->soul_traits,
                                                      &out->soul_traits_count, &traits_cap, one);
                    if (er != HU_OK)
                        return er;
                }
                t = comma ? comma + 1 : val_hi;
            }
        }
    }

    out->soul_body = hu_strdup(alloc, body_start);
    if (!out->soul_body)
        return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}

static hu_error_t parse_rules(hu_allocator_t *alloc, const char *text, hu_agent_definition_t *out) {
    out->rules = NULL;
    out->rules_count = 0;
    if (!text || !text[0])
        return HU_OK;

    size_t cap = 0;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end) {
        const char *h = strstr(p, "## ");
        if (!h)
            break;
        if (h != text && h[-1] != '\n') {
            p = h + 3;
            continue;
        }
        const char *next = strstr(h + 3, "\n## ");
        const char *sec_end = next ? next : end;
        char *sec = dup_range(alloc, h, sec_end);
        hu_error_t er = append_cstr_array(alloc, &out->rules, &out->rules_count, &cap, sec);
        if (er != HU_OK)
            return er;
        p = next ? next + 1 : end;
    }
    return HU_OK;
}

static hu_error_t parse_tools(hu_allocator_t *alloc, const char *text, hu_agent_definition_t *out) {
    out->enabled_tools = NULL;
    out->enabled_tools_count = 0;
    if (!text || !text[0])
        return HU_OK;

    size_t cap = 0;
    const char *line = text;
    const char *end = text + strlen(text);
    while (line < end) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        const char *lend = eol ? eol : end;
        const char *lo = NULL;
        const char *hi = NULL;
        trim_bounds(line, lend, &lo, &hi);
        if (lo + 2 <= hi && lo[0] == '-' && lo[1] == ' ') {
            const char *tlo = lo + 2;
            const char *thi = hi;
            trim_bounds(tlo, thi, &tlo, &thi);
            if (tlo < thi) {
                char *name = dup_range(alloc, tlo, thi);
                hu_error_t er = append_cstr_array(alloc, &out->enabled_tools,
                                                  &out->enabled_tools_count, &cap, name);
                if (er != HU_OK)
                    return er;
            }
        }
        line = eol ? eol + 1 : end;
    }
    return HU_OK;
}

hu_error_t hu_agent_definition_load(hu_allocator_t *alloc, const char *workspace_dir,
                                    hu_agent_definition_t *out) {
    if (!alloc || !workspace_dir || !out)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    const char *fixture = getenv("HU_AGENT_DEFINITION_FIXTURE");
    if (!fixture || !fixture[0]) {
        memset(out, 0, sizeof(*out));
        return HU_OK;
    }
    workspace_dir = fixture;
#endif

    memset(out, 0, sizeof(*out));

    char path[1024];
    hu_error_t err = HU_OK;
    char *soul_raw = NULL;
    char *rules_raw = NULL;
    char *memory_raw = NULL;
    char *tools_raw = NULL;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", workspace_dir, "SOUL.md") >= sizeof(path)) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    err = read_file_if_present(alloc, path, &soul_raw);
    if (err != HU_OK)
        goto cleanup;
    err = parse_soul(alloc, soul_raw, out);
    if (err != HU_OK)
        goto cleanup;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", workspace_dir, "RULES.md") >= sizeof(path)) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    err = read_file_if_present(alloc, path, &rules_raw);
    if (err != HU_OK)
        goto cleanup;
    err = parse_rules(alloc, rules_raw, out);
    if (err != HU_OK)
        goto cleanup;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", workspace_dir, "MEMORY.md") >= sizeof(path)) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    err = read_file_if_present(alloc, path, &memory_raw);
    if (err != HU_OK)
        goto cleanup;
    if (memory_raw) {
        out->memory_context = hu_strdup(alloc, memory_raw);
        if (!out->memory_context) {
            err = HU_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", workspace_dir, "TOOLS.md") >= sizeof(path)) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }
    err = read_file_if_present(alloc, path, &tools_raw);
    if (err != HU_OK)
        goto cleanup;
    err = parse_tools(alloc, tools_raw, out);
    if (err != HU_OK)
        goto cleanup;

cleanup:
    if (soul_raw)
        alloc->free(alloc->ctx, soul_raw, strlen(soul_raw) + 1u);
    if (rules_raw)
        alloc->free(alloc->ctx, rules_raw, strlen(rules_raw) + 1u);
    if (memory_raw)
        alloc->free(alloc->ctx, memory_raw, strlen(memory_raw) + 1u);
    if (tools_raw)
        alloc->free(alloc->ctx, tools_raw, strlen(tools_raw) + 1u);

    if (err != HU_OK) {
        hu_agent_definition_deinit(out, alloc);
        memset(out, 0, sizeof(*out));
    }
    return err;
}

void hu_agent_definition_deinit(hu_agent_definition_t *def, hu_allocator_t *alloc) {
    if (!def || !alloc)
        return;

    if (def->soul_name) {
        alloc->free(alloc->ctx, def->soul_name, strlen(def->soul_name) + 1u);
        def->soul_name = NULL;
    }
    if (def->soul_voice) {
        alloc->free(alloc->ctx, def->soul_voice, strlen(def->soul_voice) + 1u);
        def->soul_voice = NULL;
    }
    if (def->soul_body) {
        alloc->free(alloc->ctx, def->soul_body, strlen(def->soul_body) + 1u);
        def->soul_body = NULL;
    }
    for (size_t i = 0; i < def->soul_traits_count; i++) {
        if (def->soul_traits[i])
            alloc->free(alloc->ctx, def->soul_traits[i], strlen(def->soul_traits[i]) + 1u);
    }
    if (def->soul_traits) {
        alloc->free(alloc->ctx, def->soul_traits, def->soul_traits_count * sizeof(char *));
        def->soul_traits = NULL;
    }
    def->soul_traits_count = 0;

    for (size_t i = 0; i < def->rules_count; i++) {
        if (def->rules[i])
            alloc->free(alloc->ctx, def->rules[i], strlen(def->rules[i]) + 1u);
    }
    if (def->rules) {
        alloc->free(alloc->ctx, def->rules, def->rules_count * sizeof(char *));
        def->rules = NULL;
    }
    def->rules_count = 0;

    if (def->memory_context) {
        alloc->free(alloc->ctx, def->memory_context, strlen(def->memory_context) + 1u);
        def->memory_context = NULL;
    }

    for (size_t i = 0; i < def->enabled_tools_count; i++) {
        if (def->enabled_tools[i])
            alloc->free(alloc->ctx, def->enabled_tools[i], strlen(def->enabled_tools[i]) + 1u);
    }
    if (def->enabled_tools) {
        alloc->free(alloc->ctx, def->enabled_tools, def->enabled_tools_count * sizeof(char *));
        def->enabled_tools = NULL;
    }
    def->enabled_tools_count = 0;
}
