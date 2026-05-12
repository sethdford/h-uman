/*
 * Plugin auto-discovery — scans directories for plugin shared libraries.
 */

#include "human/plugin_discovery.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t hu_plugin_get_default_dir(char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;
    int n = snprintf(out, out_len, "%s/.human/plugins", home);
    if (n < 0 || (size_t)n >= out_len)
        return 0;
    return (size_t)n;
}

void hu_plugin_discovery_results_free(hu_allocator_t *alloc, hu_plugin_discovery_result_t *results,
                                      size_t count) {
    if (!alloc || !results)
        return;
    for (size_t i = 0; i < count; i++) {
        if (results[i].path)
            alloc->free(alloc->ctx, results[i].path, strlen(results[i].path) + 1);
        if (results[i].name)
            alloc->free(alloc->ctx, results[i].name, strlen(results[i].name) + 1);
        if (results[i].version)
            alloc->free(alloc->ctx, results[i].version, strlen(results[i].version) + 1);
    }
    alloc->free(alloc->ctx, results, sizeof(hu_plugin_discovery_result_t) * count);
}

#if HU_IS_TEST

hu_error_t hu_plugin_discover_and_load(hu_allocator_t *alloc, const char *dir,
                                       hu_plugin_host_t *host,
                                       hu_plugin_discovery_result_t **results,
                                       size_t *result_count) {
    (void)host;
    if (!alloc || !results || !result_count)
        return HU_ERR_INVALID_ARGUMENT;

    /* In test mode, simulate finding one mock plugin if dir is provided */
    if (dir && strlen(dir) > 0) {
        *result_count = 1;
        *results = (hu_plugin_discovery_result_t *)alloc->alloc(
            alloc->ctx, sizeof(hu_plugin_discovery_result_t));
        if (!*results)
            return HU_ERR_OUT_OF_MEMORY;
        memset(*results, 0, sizeof(hu_plugin_discovery_result_t));

        /* CodeQL cpp/potentially-dangerous-function flags strcpy — fine
         * for string literals into sized buffers like these, but every
         * agent + reviewer trips on it. Use memcpy with the explicit
         * length (we just allocated len+1 above) to satisfy the lint and
         * make the bound visible. */
        const char *mock_path = "mock-plugin.so";
        size_t mock_path_len = strlen(mock_path);
        (*results)[0].path = (char *)alloc->alloc(alloc->ctx, mock_path_len + 1);
        if ((*results)[0].path)
            memcpy((*results)[0].path, mock_path, mock_path_len + 1);

        const char *mock_name = "mock-plugin";
        size_t mock_name_len = strlen(mock_name);
        (*results)[0].name = (char *)alloc->alloc(alloc->ctx, mock_name_len + 1);
        if ((*results)[0].name)
            memcpy((*results)[0].name, mock_name, mock_name_len + 1);

        const char *mock_ver = "1.0.0";
        size_t mock_ver_len = strlen(mock_ver);
        (*results)[0].version = (char *)alloc->alloc(alloc->ctx, mock_ver_len + 1);
        if ((*results)[0].version)
            memcpy((*results)[0].version, mock_ver, mock_ver_len + 1);

        (*results)[0].load_error = HU_OK;
        return HU_OK;
    }

    *results = NULL;
    *result_count = 0;
    return HU_OK;
}

#elif defined(_WIN32)

hu_error_t hu_plugin_discover_and_load(hu_allocator_t *alloc, const char *dir,
                                       hu_plugin_host_t *host,
                                       hu_plugin_discovery_result_t **results,
                                       size_t *result_count) {
    (void)alloc;
    (void)dir;
    (void)host;
    (void)results;
    (void)result_count;
    return HU_ERR_NOT_SUPPORTED;
}

#else /* POSIX */

#include <dirent.h>

static bool has_plugin_ext(const char *name) {
    size_t len = strlen(name);
    if (len > 3 && strcmp(name + len - 3, ".so") == 0)
        return true;
    if (len > 6 && strcmp(name + len - 6, ".dylib") == 0)
        return true;
    return false;
}

static char *dup_str(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d)
        memcpy(d, s, len + 1);
    return d;
}

hu_error_t hu_plugin_discover_and_load(hu_allocator_t *alloc, const char *dir,
                                       hu_plugin_host_t *host,
                                       hu_plugin_discovery_result_t **results,
                                       size_t *result_count) {
    if (!alloc || !results || !result_count)
        return HU_ERR_INVALID_ARGUMENT;

    char dir_buf[512];
    if (!dir || strlen(dir) == 0) {
        size_t n = hu_plugin_get_default_dir(dir_buf, sizeof(dir_buf));
        if (n == 0) {
            *results = NULL;
            *result_count = 0;
            return HU_OK;
        }
        dir = dir_buf;
    }

    DIR *d = opendir(dir);
    if (!d) {
        *results = NULL;
        *result_count = 0;
        return HU_OK;
    }

    size_t cap = 8;
    *results = (hu_plugin_discovery_result_t *)alloc->alloc(
        alloc->ctx, sizeof(hu_plugin_discovery_result_t) * cap);
    if (!*results) {
        closedir(d);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *result_count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_plugin_ext(ent->d_name))
            continue;

        if (*result_count >= cap) {
            size_t new_cap = cap * 2;
            hu_plugin_discovery_result_t *new_arr = (hu_plugin_discovery_result_t *)alloc->alloc(
                alloc->ctx, sizeof(hu_plugin_discovery_result_t) * new_cap);
            if (!new_arr)
                break;
            memcpy(new_arr, *results, sizeof(hu_plugin_discovery_result_t) * cap);
            alloc->free(alloc->ctx, *results, sizeof(hu_plugin_discovery_result_t) * cap);
            *results = new_arr;
            cap = new_cap;
        }

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);

        hu_plugin_discovery_result_t *r = &(*results)[*result_count];
        memset(r, 0, sizeof(*r));
        r->path = dup_str(alloc, full_path);

        hu_plugin_info_t info = {0};
        hu_plugin_handle_t *handle = NULL;
        hu_error_t err = hu_plugin_load(alloc, full_path, host, &info, &handle);
        r->load_error = err;

        if (err == HU_OK) {
            r->name = dup_str(alloc, info.name);
            r->version = dup_str(alloc, info.version);
        } else {
            r->name = dup_str(alloc, ent->d_name);
        }
        (*result_count)++;
    }

    closedir(d);
    return HU_OK;
}

#endif /* platform */
