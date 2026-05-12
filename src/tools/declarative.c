#include "human/tools/declarative.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#if (defined(__unix__) || defined(__APPLE__)) && !(defined(HU_IS_TEST) && HU_IS_TEST)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct hu_declarative_tool_ctx {
    hu_declarative_tool_def_t def;
} hu_declarative_tool_ctx_t;

static void free_nonnull_str(hu_allocator_t *alloc, char *s) {
    if (!alloc || !s)
        return;
    alloc->free(alloc->ctx, s, strlen(s) + 1);
}

void hu_declarative_tool_def_free(hu_declarative_tool_def_t *def, hu_allocator_t *alloc) {
    if (!def || !alloc)
        return;
    free_nonnull_str(alloc, def->name);
    free_nonnull_str(alloc, def->description);
    free_nonnull_str(alloc, def->parameters_json);
    free_nonnull_str(alloc, def->exec_url);
    free_nonnull_str(alloc, def->exec_method);
    free_nonnull_str(alloc, def->exec_command);
    free_nonnull_str(alloc, def->exec_chain);
    free_nonnull_str(alloc, def->exec_transform);
    memset(def, 0, sizeof(*def));
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_decl_exec_type_t parse_exec_type(const char *t) {
    if (!t)
        return HU_DECL_EXEC_HTTP;
    if (strcmp(t, "http") == 0)
        return HU_DECL_EXEC_HTTP;
    if (strcmp(t, "shell") == 0)
        return HU_DECL_EXEC_SHELL;
    if (strcmp(t, "chain") == 0)
        return HU_DECL_EXEC_CHAIN;
    if (strcmp(t, "transform") == 0)
        return HU_DECL_EXEC_TRANSFORM;
    return HU_DECL_EXEC_HTTP;
}

/* POSIX sh: wrap a value as a single shell word using single quotes; internal ' -> '\'' */
static hu_error_t shell_escape_single_quoted(hu_allocator_t *alloc, const char *val, char **out,
                                             size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!val)
        val = "";
    size_t vlen = strlen(val);
    size_t extra = 0;
    for (size_t i = 0; i < vlen; i++) {
        if (val[i] == '\'')
            extra += 3;
    }
    size_t need = 2 + vlen + extra + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    buf[pos++] = '\'';
    for (size_t i = 0; i < vlen; i++) {
        if (val[i] == '\'') {
            buf[pos++] = '\'';
            buf[pos++] = '\\';
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = val[i];
        }
    }
    buf[pos++] = '\'';
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

static hu_error_t buf_append(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap, const char *s,
                             size_t slen) {
    size_t need = *len + slen + 1;
    while (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 128;
        if (nc < need)
            nc = need;
        char *nb = (char *)alloc->realloc(alloc->ctx, *buf, *cap, nc);
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
    return HU_OK;
}

static hu_error_t substitute_placeholders(hu_allocator_t *alloc, const char *tmpl,
                                          const hu_json_value_t *args, int shell_escape_vals, char **out,
                                          size_t *out_len) {
    if (!tmpl) {
        char *e = hu_strdup(alloc, "");
        if (!e)
            return HU_ERR_OUT_OF_MEMORY;
        *out = e;
        *out_len = 0;
        return HU_OK;
    }

    size_t cap = strlen(tmpl) + 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    const char *p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (!end) {
                hu_error_t e = buf_append(alloc, &buf, &len, &cap, p, 1);
                if (e != HU_OK) {
                    alloc->free(alloc->ctx, buf, cap);
                    return e;
                }
                p++;
                continue;
            }
            const char *key_start = p + 2;
            size_t key_len = (size_t)(end - key_start);
            while (key_len > 0 && isspace((unsigned char)key_start[0])) {
                key_start++;
                key_len--;
            }
            while (key_len > 0 && isspace((unsigned char)key_start[key_len - 1]))
                key_len--;

            char key_stack[128];
            const char *key_ptr = key_start;
            if (key_len >= sizeof(key_stack)) {
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_INVALID_ARGUMENT;
            }
            memcpy(key_stack, key_start, key_len);
            key_stack[key_len] = '\0';
            key_ptr = key_stack;

            const char *val = "";
            if (args && args->type == HU_JSON_OBJECT)
                val = hu_json_get_string(args, key_ptr);
            if (!val)
                val = "";

            hu_error_t er;
            if (shell_escape_vals) {
                char *esc = NULL;
                size_t esc_len = 0;
                hu_error_t ee = shell_escape_single_quoted(alloc, val, &esc, &esc_len);
                if (ee != HU_OK) {
                    alloc->free(alloc->ctx, buf, cap);
                    return ee;
                }
                er = buf_append(alloc, &buf, &len, &cap, esc, esc_len);
                alloc->free(alloc->ctx, esc, esc_len + 1);
            } else {
                er = buf_append(alloc, &buf, &len, &cap, val, strlen(val));
            }
            if (er != HU_OK) {
                alloc->free(alloc->ctx, buf, cap);
                return er;
            }
            p = end + 2;
        } else {
            hu_error_t e = buf_append(alloc, &buf, &len, &cap, p, 1);
            if (e != HU_OK) {
                alloc->free(alloc->ctx, buf, cap);
                return e;
            }
            p++;
        }
    }
    *out = buf;
    *out_len = len;
    return HU_OK;
}

/* Reject obvious injection transports in assembled commands. */
static int decl_assembled_cmd_suspicious(const char *cmd) {
    if (!cmd)
        return 0;
    for (const char *p = cmd; *p; p++) {
        if (*p == '\n' || *p == '\r')
            return 1;
    }
    return 0;
}

static int decl_template_text_suspicious(const char *tmpl) {
    if (!tmpl)
        return 0;
    /* Unquoted subshell/backtick sequences in the template itself are high risk. */
    if (strstr(tmpl, "$(") != NULL || strchr(tmpl, '`') != NULL)
        return 1;
    return 0;
}
#endif /* !(HU_IS_TEST) */

static hu_error_t copy_def(hu_allocator_t *alloc, const hu_declarative_tool_def_t *src,
                           hu_declarative_tool_def_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->exec_type = src->exec_type;
#define CP(f)                                                                                      \
    do {                                                                                           \
        if (src->f) {                                                                              \
            dst->f = hu_strdup(alloc, src->f);                                                     \
            if (!dst->f)                                                                           \
                goto oom;                                                                          \
        }                                                                                          \
    } while (0)
    CP(name);
    CP(description);
    CP(parameters_json);
    CP(exec_url);
    CP(exec_method);
    CP(exec_command);
    CP(exec_chain);
    CP(exec_transform);
#undef CP
    return HU_OK;
oom:
    hu_declarative_tool_def_free(dst, alloc);
    return HU_ERR_OUT_OF_MEMORY;
}

static const char *decl_name(void *ctx) {
    hu_declarative_tool_ctx_t *c = (hu_declarative_tool_ctx_t *)ctx;
    return c && c->def.name ? c->def.name : "";
}

static const char *decl_desc(void *ctx) {
    hu_declarative_tool_ctx_t *c = (hu_declarative_tool_ctx_t *)ctx;
    return c && c->def.description ? c->def.description : "";
}

static const char *decl_params(void *ctx) {
    hu_declarative_tool_ctx_t *c = (hu_declarative_tool_ctx_t *)ctx;
    return c && c->def.parameters_json ? c->def.parameters_json : "{}";
}

static void decl_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_declarative_tool_ctx_t *c = (hu_declarative_tool_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    hu_declarative_tool_def_free(&c->def, alloc);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static hu_error_t decl_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                               hu_tool_result_t *out) {
    hu_declarative_tool_ctx_t *c = (hu_declarative_tool_ctx_t *)ctx;
    if (!c || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    switch (c->def.exec_type) {
    case HU_DECL_EXEC_CHAIN:
    case HU_DECL_EXEC_TRANSFORM: {
        (void)args;
        static const char msg[] = "chain/transform not yet wired";
        *out = hu_tool_result_fail(msg, sizeof(msg) - 1);
        return HU_OK;
    }
    case HU_DECL_EXEC_HTTP:
    case HU_DECL_EXEC_SHELL:
#if defined(HU_IS_TEST) && HU_IS_TEST
        (void)args;
        *out = hu_tool_result_ok("ok", 2);
        return HU_OK;
#else
        if (c->def.exec_type == HU_DECL_EXEC_HTTP) {
            if (!c->def.exec_url) {
                *out = hu_tool_result_fail("missing exec url", 18);
                return HU_OK;
            }
            char *url = NULL;
            size_t url_len = 0;
            hu_error_t se =
                substitute_placeholders(alloc, c->def.exec_url, args, 1, &url, &url_len);
            if (se != HU_OK)
                return se;
            if (decl_template_text_suspicious(c->def.exec_url)) {
                alloc->free(alloc->ctx, url, url_len + 1);
                *out = hu_tool_result_fail("exec url template rejected", 26);
                return HU_OK;
            }
            const char *method = c->def.exec_method ? c->def.exec_method : "GET";
            hu_http_response_t resp = {0};
            hu_error_t rr = hu_http_request(alloc, url, method, NULL, NULL, 0, &resp);
            alloc->free(alloc->ctx, url, url_len + 1);
            if (rr != HU_OK)
                return rr;
            if (resp.body && resp.body_len > 0) {
                *out = hu_tool_result_ok_owned(resp.body, resp.body_len);
                resp.body = NULL;
            } else {
                *out = hu_tool_result_ok("", 0);
            }
            hu_http_response_free(alloc, &resp);
            return HU_OK;
        }
        /* SHELL */
        if (!c->def.exec_command) {
            *out = hu_tool_result_fail("missing exec command", 22);
            return HU_OK;
        }
        char *cmdline = NULL;
        size_t cmd_len = 0;
        if (decl_template_text_suspicious(c->def.exec_command)) {
            *out = hu_tool_result_fail("exec command template rejected", 30);
            return HU_OK;
        }
        hu_error_t se =
            substitute_placeholders(alloc, c->def.exec_command, args, 1, &cmdline, &cmd_len);
        if (se != HU_OK)
            return se;
        if (strlen(cmdline) > 8192) {
            alloc->free(alloc->ctx, cmdline, cmd_len + 1);
            *out = hu_tool_result_fail("command too long", 16);
            return HU_OK;
        }
        if (decl_assembled_cmd_suspicious(cmdline)) {
            alloc->free(alloc->ctx, cmdline, cmd_len + 1);
            *out = hu_tool_result_fail("command rejected", 16);
            return HU_OK;
        }
        {
            const char *argv[] = {"/bin/sh", "-c", cmdline, NULL};
            hu_run_result_t run = {0};
            hu_error_t rr =
                hu_process_run_with_timeout(alloc, argv, NULL, 1048576, 30, &run);
            alloc->free(alloc->ctx, cmdline, cmd_len + 1);
            if (rr != HU_OK)
                return rr;
            if (run.stdout_buf && run.stdout_len > 0) {
                char *body = hu_strndup(alloc, run.stdout_buf, run.stdout_len);
                hu_run_result_free(alloc, &run);
                if (!body)
                    return HU_ERR_OUT_OF_MEMORY;
                *out = hu_tool_result_ok_owned(body, run.stdout_len);
            } else {
                hu_run_result_free(alloc, &run);
                *out = hu_tool_result_ok("", 0);
            }
            return HU_OK;
        }
#endif
    }
    return HU_ERR_NOT_SUPPORTED;
}

static const hu_tool_vtable_t declarative_vtable = {
    .execute = decl_execute,
    .name = decl_name,
    .description = decl_desc,
    .parameters_json = decl_params,
    .deinit = decl_deinit,
};

hu_error_t hu_declarative_tool_create(hu_allocator_t *alloc, const hu_declarative_tool_def_t *def,
                                      hu_tool_t *out) {
    if (!alloc || !def || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!def->name) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_declarative_tool_ctx_t *ctx =
        (hu_declarative_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_declarative_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));

    hu_error_t cr = copy_def(alloc, def, &ctx->def);
    if (cr != HU_OK) {
        alloc->free(alloc->ctx, ctx, sizeof(*ctx));
        return cr;
    }

    out->ctx = ctx;
    out->vtable = &declarative_vtable;
    return HU_OK;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static void free_defs_array(hu_allocator_t *alloc, hu_declarative_tool_def_t *defs, size_t n) {
    if (!defs || !alloc)
        return;
    for (size_t i = 0; i < n; i++)
        hu_declarative_tool_def_free(&defs[i], alloc);
    alloc->free(alloc->ctx, defs, n * sizeof(hu_declarative_tool_def_t));
}

static hu_error_t defs_push(hu_allocator_t *alloc, hu_declarative_tool_def_t **defs, size_t *count,
                            size_t *cap, const hu_declarative_tool_def_t *item) {
    if (*count >= *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        hu_declarative_tool_def_t *nb = (hu_declarative_tool_def_t *)alloc->realloc(
            alloc->ctx, *defs, (*cap) * sizeof(hu_declarative_tool_def_t),
            nc * sizeof(hu_declarative_tool_def_t));
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        *defs = nb;
        *cap = nc;
    }
    memset(&(*defs)[*count], 0, sizeof(hu_declarative_tool_def_t));
    hu_error_t e = copy_def(alloc, item, &(*defs)[*count]);
    if (e != HU_OK)
        return e;
    (*count)++;
    return HU_OK;
}

static hu_error_t load_one_json_file(hu_allocator_t *alloc, const char *path,
                                     hu_declarative_tool_def_t *out_def) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_IO;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    rewind(fp);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t pr = hu_json_parse(alloc, buf, n, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (pr != HU_OK)
        return pr;
    if (!root || root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    const char *name = hu_json_get_string(root, "name");
    if (!name) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    memset(out_def, 0, sizeof(*out_def));
    out_def->name = hu_strdup(alloc, name);
    const char *desc = hu_json_get_string(root, "description");
    if (desc)
        out_def->description = hu_strdup(alloc, desc);

    hu_json_value_t *params = hu_json_object_get(root, "parameters");
    if (params && params->type == HU_JSON_OBJECT) {
        char *pj = NULL;
        size_t pj_len = 0;
        if (hu_json_stringify(alloc, params, &pj, &pj_len) != HU_OK) {
            hu_declarative_tool_def_free(out_def, alloc);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        out_def->parameters_json = pj;
    } else if (params && params->type == HU_JSON_STRING) {
        out_def->parameters_json =
            hu_strndup(alloc, params->data.string.ptr, params->data.string.len);
    } else {
        out_def->parameters_json = hu_strdup(alloc, "{}");
    }

    if (!out_def->parameters_json) {
        hu_declarative_tool_def_free(out_def, alloc);
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_value_t *exec = hu_json_object_get(root, "execute");
    if (exec && exec->type == HU_JSON_OBJECT) {
        const char *etype = hu_json_get_string(exec, "type");
        out_def->exec_type = parse_exec_type(etype);
        const char *u = hu_json_get_string(exec, "url");
        if (u)
            out_def->exec_url = hu_strdup(alloc, u);
        const char *m = hu_json_get_string(exec, "method");
        if (m)
            out_def->exec_method = hu_strdup(alloc, m);
        const char *cmd = hu_json_get_string(exec, "command");
        if (cmd)
            out_def->exec_command = hu_strdup(alloc, cmd);
        const char *ch = hu_json_get_string(exec, "chain");
        if (ch)
            out_def->exec_chain = hu_strdup(alloc, ch);
        const char *tr = hu_json_get_string(exec, "transform");
        if (tr)
            out_def->exec_transform = hu_strdup(alloc, tr);
    }

    if (!out_def->name) {
        hu_declarative_tool_def_free(out_def, alloc);
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_free(alloc, root);
    return HU_OK;
}
#endif /* !(HU_IS_TEST) */

hu_error_t hu_declarative_tools_discover(hu_allocator_t *alloc, const char *dir,
                                         hu_declarative_tool_def_t **out, size_t *out_count) {
    if (!alloc || !dir || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    *out = NULL;
    *out_count = 0;
    return HU_OK;
#else
#if !(defined(__unix__) || defined(__APPLE__))
    *out = NULL;
    *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
#else
    DIR *d = opendir(dir);
    if (!d)
        return HU_ERR_IO;

    hu_declarative_tool_def_t *defs = NULL;
    size_t count = 0;
    size_t cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nl = strlen(name);
        if (nl < 6)
            continue;
        if (strcmp(name + nl - 5, ".json") != 0)
            continue;

        char path[4096];
        int pn =
            snprintf(path, sizeof path, "%s/%s", dir, name);
        if (pn < 0 || (size_t)pn >= sizeof(path))
            continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        hu_declarative_tool_def_t one;
        hu_error_t lr = load_one_json_file(alloc, path, &one);
        if (lr != HU_OK)
            continue;

        hu_error_t pe = defs_push(alloc, &defs, &count, &cap, &one);
        if (pe != HU_OK) {
            hu_declarative_tool_def_free(&one, alloc);
            free_defs_array(alloc, defs, count);
            closedir(d);
            return pe;
        }
    }
    closedir(d);
    *out = defs;
    *out_count = count;
    return HU_OK;
#endif
#endif
}
