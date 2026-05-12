#ifdef HU_GATEWAY_POSIX

#include "cp_internal.h"
#include "human/agent/hula_analytics.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void default_hula_trace_dir(char *buf, size_t cap) {
    const char *h = getenv("HOME");
    if (!h || !h[0]) {
        buf[0] = '\0';
        return;
    }
    (void)snprintf(buf, cap, "%s/.human/hula_traces", h);
}

/* Prefer HU_HULA_TRACE_DIR (same as CLI `human hula run`) so dashboard lists match persist location. */
static void resolve_hula_trace_dir(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    const char *e = getenv("HU_HULA_TRACE_DIR");
    if (e && e[0]) {
        if (e[0] != '/' || strstr(e, "..")) {
            default_hula_trace_dir(buf, cap);
            return;
        }
        size_t el = strlen(e);
        if (el < cap) {
            memcpy(buf, e, el + 1);
        }
        return;
    }
    default_hula_trace_dir(buf, cap);
}

static bool safe_trace_basename(const char *name) {
    if (!name || !name[0])
        return false;
    size_t n = strlen(name);
    if (n < 6 || strcmp(name + n - 5, ".json") != 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 32)
            return false;
        if (name[i] == '/' || name[i] == '\\')
            return false;
        if (name[i] == '.' && i + 1 < n && name[i + 1] == '.')
            return false;
    }
    return true;
}

static bool hula_traces_params_want_trace_window(const hu_json_value_t *params) {
    if (!params || params->type != HU_JSON_OBJECT)
        return false;
    return hu_json_object_get(params, "trace_limit") != NULL ||
           hu_json_object_get(params, "trace_offset") != NULL;
}

/* Optional trace array window: steal moved elements into a new array, replace `trace` on record. */
static void hula_traces_apply_trace_window(hu_allocator_t *alloc, hu_json_value_t *record,
                                           const hu_json_value_t *params, hu_json_value_t *wrap) {
    if (!alloc || !record || record->type != HU_JSON_OBJECT || !params || !wrap)
        return;
    hu_json_value_t *tr = hu_json_object_get(record, "trace");
    if (!tr || tr->type != HU_JSON_ARRAY)
        return;

    size_t total = tr->data.array.len;
    double offd = hu_json_get_number(params, "trace_offset", 0.0);
    double limd = hu_json_get_number(params, "trace_limit", 200.0);
    size_t off = 0;
    if (offd == offd && offd > 0.0 && offd < 1e15)
        off = (size_t)offd;
    if (off > total)
        off = total;
    size_t lim = 200;
    if (limd == limd && limd > 0.0 && limd < 1e9)
        lim = (size_t)limd;
    if (lim > 1000)
        lim = 1000;
    size_t avail = total > off ? total - off : 0;
    size_t cnt = avail < lim ? avail : lim;

    hu_json_value_t *na = hu_json_array_new(alloc);
    if (!na)
        return;
    for (size_t j = 0; j < cnt; j++) {
        hu_json_value_t *it = tr->data.array.items[off + j];
        if (hu_json_array_push(alloc, na, it) != HU_OK) {
            hu_json_free(alloc, na);
            return;
        }
        tr->data.array.items[off + j] = NULL;
    }
    if (hu_json_object_set(alloc, record, "trace", na) != HU_OK) {
        hu_json_free(alloc, na);
        return;
    }
    bool trunc = (off + cnt < total);
    hu_json_object_set(alloc, wrap, "trace_total_steps", hu_json_number_new(alloc, (double)total));
    hu_json_object_set(alloc, wrap, "trace_offset", hu_json_number_new(alloc, (double)off));
    hu_json_object_set(alloc, wrap, "trace_limit", hu_json_number_new(alloc, (double)lim));
    hu_json_object_set(alloc, wrap, "trace_returned_count", hu_json_number_new(alloc, (double)cnt));
    hu_json_object_set(alloc, wrap, "trace_truncated", hu_json_bool_new(alloc, trunc));
}

hu_error_t cp_hula_traces_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
#if !(defined(__unix__) || defined(__APPLE__))
    (void)alloc;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char dir[512];
    resolve_hula_trace_dir(dir, sizeof(dir));
    if (!dir[0])
        return HU_ERR_NOT_FOUND;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr) {
        hu_json_free(alloc, obj);
        return HU_ERR_OUT_OF_MEMORY;
    }

    DIR *d = opendir(dir);
    if (!d) {
        hu_json_object_set(alloc, obj, "traces", arr);
        hu_json_object_set(alloc, obj, "directory", hu_json_string_new(alloc, dir, strlen(dir)));
        hu_error_t er = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return er;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (!safe_trace_basename(de->d_name))
            continue;
        char path[768];
        int pn = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (pn <= 0 || (size_t)pn >= sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        hu_json_value_t *row = hu_json_object_new(alloc);
        if (!row)
            continue;
        cp_json_set_str(alloc, row, "id", de->d_name);
        hu_json_object_set(alloc, row, "size", hu_json_number_new(alloc, (double)st.st_size));
        hu_json_object_set(alloc, row, "mtime", hu_json_number_new(alloc, (double)st.st_mtime));
        hu_json_array_push(alloc, arr, row);
    }
    closedir(d);

    hu_json_object_set(alloc, obj, "traces", arr);
    hu_json_object_set(alloc, obj, "directory", hu_json_string_new(alloc, dir, strlen(dir)));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
#endif
}

hu_error_t cp_hula_traces_get(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
#if !(defined(__unix__) || defined(__APPLE__))
    (void)alloc;
    (void)root;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_json_value_t *params = root ? hu_json_object_get(root, "params") : NULL;
    const char *id = params ? hu_json_get_string(params, "id") : NULL;
    if (!id || !safe_trace_basename(id))
        return HU_ERR_INVALID_ARGUMENT;

    char dir[512];
    resolve_hula_trace_dir(dir, sizeof(dir));
    if (!dir[0])
        return HU_ERR_NOT_FOUND;

    char path[768];
    int pn = snprintf(path, sizeof(path), "%s/%s", dir, id);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        fclose(f);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    cp_json_set_str(alloc, obj, "id", id);
    hu_json_value_t *parsed = NULL;
    if (hu_json_parse(alloc, buf, rd, &parsed) == HU_OK && parsed) {
        if (parsed->type == HU_JSON_OBJECT && params &&
            hula_traces_params_want_trace_window(params))
            hula_traces_apply_trace_window(alloc, parsed, params, obj);
        hu_json_object_set(alloc, obj, "record", parsed);
    } else
        hu_json_object_set(alloc, obj, "raw", hu_json_string_new(alloc, buf, rd));
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
#endif
}

hu_error_t cp_hula_traces_delete(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
#if !(defined(__unix__) || defined(__APPLE__))
    (void)alloc;
    (void)root;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_json_value_t *params = root ? hu_json_object_get(root, "params") : NULL;
    const char *id = params ? hu_json_get_string(params, "id") : NULL;
    if (!id || !safe_trace_basename(id))
        return HU_ERR_INVALID_ARGUMENT;

    char dir[512];
    resolve_hula_trace_dir(dir, sizeof(dir));
    if (!dir[0])
        return HU_ERR_NOT_FOUND;
    char path[768];
    int pn = snprintf(path, sizeof(path), "%s/%s", dir, id);
    if (pn <= 0 || (size_t)pn >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    if (unlink(path) != 0)
        return HU_ERR_NOT_FOUND;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "deleted", hu_json_bool_new(alloc, true));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
#endif
}

hu_error_t cp_hula_traces_analytics(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                    const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                    char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
#if !(defined(__unix__) || defined(__APPLE__))
    (void)alloc;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char dir[512];
    resolve_hula_trace_dir(dir, sizeof(dir));
    if (!dir[0])
        return HU_ERR_NOT_FOUND;
    char *payload = NULL;
    size_t plen = 0;
    hu_error_t err = hu_hula_analytics_summarize(alloc, dir, &payload, &plen);
    if (err != HU_OK)
        return err;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj) {
        hu_str_free(alloc, payload);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_value_t *inner = NULL;
    if (payload && hu_json_parse(alloc, payload, plen, &inner) == HU_OK && inner)
        hu_json_object_set(alloc, obj, "summary", inner);
    else
        cp_json_set_str(alloc, obj, "summary_raw", payload ? payload : "");
    hu_str_free(alloc, payload);
    hu_error_t ser = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return ser;
#endif
}

#endif /* HU_GATEWAY_POSIX */
