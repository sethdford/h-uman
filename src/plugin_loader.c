#include "human/plugin_loader.h"
#include "human/core/string.h"
#include <stddef.h>
#include <string.h>

#define HU_PLUGIN_LOADED_MAX 32
static hu_plugin_handle_t *s_loaded[HU_PLUGIN_LOADED_MAX];
static size_t s_loaded_count = 0;

#if defined(_WIN32)
/* Windows: dlopen not available, return HU_ERR_NOT_SUPPORTED */
hu_error_t hu_plugin_load(hu_allocator_t *alloc, const char *path, hu_plugin_host_t *host,
                          hu_plugin_info_t *info_out, hu_plugin_handle_t **out_handle) {
    (void)alloc;
    (void)path;
    (void)host;
    (void)info_out;
    (void)out_handle;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_plugin_unload(hu_plugin_handle_t *handle) {
    (void)handle;
}

void hu_plugin_unload_all(void) {
    (void)0;
}

#elif defined(HU_IS_TEST) && HU_IS_TEST != 0
/* HU_IS_TEST: mock dlopen path — never actually load, return errors for testing */

typedef struct hu_plugin_handle {
    hu_allocator_t *alloc;
    char path_buf[256];
} hu_plugin_handle_t;

hu_error_t hu_plugin_load(hu_allocator_t *alloc, const char *path, hu_plugin_host_t *host,
                          hu_plugin_info_t *info_out, hu_plugin_handle_t **out_handle) {
    (void)host;
    if (!path || !info_out || !out_handle)
        return HU_ERR_INVALID_ARGUMENT;

    if (strcmp(path, "/nonexistent/plugin.so") == 0 || strstr(path, "nonexistent") != NULL)
        return HU_ERR_NOT_FOUND;

    if (strcmp(path, "/bad_api/plugin.so") == 0 || strstr(path, "bad_api") != NULL) {
        info_out->name = "bad-api";
        info_out->version = "1.0";
        info_out->description = "API version mismatch";
        info_out->api_version = 999;
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_plugin_handle_t *h = (hu_plugin_handle_t *)alloc->alloc(alloc->ctx, sizeof(*h));
    if (!h)
        return HU_ERR_OUT_OF_MEMORY;
    memset(h, 0, sizeof(*h));
    h->alloc = alloc;
    size_t len = strlen(path);
    if (len >= sizeof(h->path_buf))
        len = sizeof(h->path_buf) - 1;
    memcpy(h->path_buf, path, len);
    h->path_buf[len] = '\0';

    info_out->name = "mock-plugin";
    info_out->version = "1.0.0";
    info_out->description = "Mock plugin for tests";
    info_out->api_version = HU_PLUGIN_API_VERSION;

    *out_handle = h;
    if (s_loaded_count < HU_PLUGIN_LOADED_MAX)
        s_loaded[s_loaded_count++] = h;
    return HU_OK;
}

void hu_plugin_unload(hu_plugin_handle_t *handle) {
    if (!handle)
        return;
    for (size_t i = 0; i < s_loaded_count; i++) {
        if (s_loaded[i] == handle) {
            s_loaded_count--;
            if (i < s_loaded_count)
                s_loaded[i] = s_loaded[s_loaded_count];
            s_loaded[s_loaded_count] = NULL;
            break;
        }
    }
    if (handle->alloc)
        handle->alloc->free(handle->alloc->ctx, handle, sizeof(*handle));
}

void hu_plugin_unload_all(void) {
    while (s_loaded_count > 0) {
        hu_plugin_handle_t *h = s_loaded[s_loaded_count - 1];
        hu_plugin_unload(h);
    }
}

#else
/* POSIX: real dlopen/dlsym */

#include <dlfcn.h>
#include <stdlib.h>

typedef struct hu_plugin_handle {
    void *dl_handle;
    hu_allocator_t *alloc;
} hu_plugin_handle_t;

hu_error_t hu_plugin_load(hu_allocator_t *alloc, const char *path, hu_plugin_host_t *host,
                          hu_plugin_info_t *info_out, hu_plugin_handle_t **out_handle) {
    if (!alloc || !path || !host || !info_out || !out_handle)
        return HU_ERR_INVALID_ARGUMENT;

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        (void)dlerror();
        return HU_ERR_NOT_FOUND;
    }

    hu_plugin_init_fn init_fn;
    *(void **)&init_fn = dlsym(handle, "hu_plugin_init");
    if (!init_fn) {
        dlclose(handle);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_plugin_info_t info = {0};
    hu_error_t err = init_fn(host, &info);
    if (err != HU_OK) {
        dlclose(handle);
        return err;
    }

    if (info.api_version != HU_PLUGIN_API_VERSION) {
        dlclose(handle);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_plugin_handle_t *h = (hu_plugin_handle_t *)alloc->alloc(alloc->ctx, sizeof(*h));
    if (!h) {
        hu_plugin_deinit_fn deinit;
        *(void **)&deinit = dlsym(handle, "hu_plugin_deinit");
        if (deinit)
            deinit();
        dlclose(handle);
        return HU_ERR_OUT_OF_MEMORY;
    }
    h->dl_handle = handle;
    h->alloc = alloc;

    *info_out = info;
    *out_handle = h;
    if (s_loaded_count < HU_PLUGIN_LOADED_MAX)
        s_loaded[s_loaded_count++] = h;
    return HU_OK;
}

void hu_plugin_unload(hu_plugin_handle_t *handle) {
    if (!handle)
        return;
    for (size_t i = 0; i < s_loaded_count; i++) {
        if (s_loaded[i] == handle) {
            s_loaded_count--;
            if (i < s_loaded_count)
                s_loaded[i] = s_loaded[s_loaded_count];
            s_loaded[s_loaded_count] = NULL;
            break;
        }
    }
    void *h = handle->dl_handle;
    if (h) {
        hu_plugin_deinit_fn deinit;
        *(void **)&deinit = dlsym(h, "hu_plugin_deinit");
        if (deinit)
            deinit();
        dlclose(h);
    }
    hu_allocator_t *alloc = handle->alloc;
    if (alloc)
        alloc->free(alloc->ctx, handle, sizeof(*handle));
}

void hu_plugin_unload_all(void) {
    while (s_loaded_count > 0) {
        s_loaded_count--;
        hu_plugin_unload(s_loaded[s_loaded_count]);
        s_loaded[s_loaded_count] = NULL;
    }
}

#endif /* _WIN32 / HU_IS_TEST / POSIX */
