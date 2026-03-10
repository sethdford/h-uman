#include "human/memory/inbox.h"
#include "human/core/string.h"
#include "human/memory/ingest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

hu_error_t hu_inbox_init(hu_inbox_watcher_t *watcher, hu_allocator_t *alloc, hu_memory_t *memory,
                         const char *inbox_dir, size_t inbox_dir_len) {
    if (!watcher || !alloc || !memory)
        return HU_ERR_INVALID_ARGUMENT;
    memset(watcher, 0, sizeof(*watcher));
    watcher->alloc = alloc;
    watcher->memory = memory;

    if (inbox_dir && inbox_dir_len > 0) {
        watcher->inbox_dir = hu_strndup(alloc, inbox_dir, inbox_dir_len);
        watcher->inbox_dir_len = inbox_dir_len;
    } else {
#ifdef HU_IS_TEST
        watcher->inbox_dir = hu_strndup(alloc, "/tmp/human-test-inbox", 23);
        watcher->inbox_dir_len = 23;
#else
        const char *home = getenv("HOME");
        if (!home)
            home = "/tmp";
        char buf[1024];
        int n = snprintf(buf, sizeof(buf), "%s/" HU_INBOX_DEFAULT_DIR, home);
        if (n <= 0 || (size_t)n >= sizeof(buf))
            return HU_ERR_INVALID_ARGUMENT;
        watcher->inbox_dir = hu_strndup(alloc, buf, (size_t)n);
        watcher->inbox_dir_len = (size_t)n;
#endif
    }
    if (!watcher->inbox_dir)
        return HU_ERR_OUT_OF_MEMORY;

    char proc_buf[1024];
    int pn = snprintf(proc_buf, sizeof(proc_buf), "%.*s/processed", (int)watcher->inbox_dir_len,
                      watcher->inbox_dir);
    if (pn > 0 && (size_t)pn < sizeof(proc_buf)) {
        watcher->processed_dir = hu_strndup(alloc, proc_buf, (size_t)pn);
        watcher->processed_dir_len = (size_t)pn;
    }

#if !defined(_WIN32) && !defined(__CYGWIN__) && !defined(HU_IS_TEST)
    mkdir(watcher->inbox_dir, 0755);
    if (watcher->processed_dir)
        mkdir(watcher->processed_dir, 0755);
#endif

    return HU_OK;
}

hu_error_t hu_inbox_poll(hu_inbox_watcher_t *watcher, size_t *processed_count) {
    if (!watcher || !processed_count)
        return HU_ERR_INVALID_ARGUMENT;
    *processed_count = 0;

#ifdef HU_IS_TEST
    return HU_OK;
#else
#if !defined(_WIN32) && !defined(__CYGWIN__)
    DIR *dir = opendir(watcher->inbox_dir);
    if (!dir)
        return HU_OK;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && *processed_count < HU_INBOX_MAX_FILES) {
        if (ent->d_name[0] == '.')
            continue;
        if (strstr(ent->d_name, "..") != NULL)
            continue;
        if (strchr(ent->d_name, '/') != NULL || strchr(ent->d_name, '\\') != NULL)
            continue;

        char path[1024];
        int n = snprintf(path, sizeof(path), "%.*s/%s", (int)watcher->inbox_dir_len,
                         watcher->inbox_dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path))
            continue;

        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (S_ISLNK(st.st_mode))
            continue;
        if (st.st_size > HU_INBOX_MAX_FILE_SIZE)
            continue;

        hu_error_t err;
        if (watcher->provider && watcher->provider->vtable)
            err = hu_ingest_file_with_provider(watcher->alloc, watcher->memory, watcher->provider,
                                               path, (size_t)n, watcher->model, watcher->model_len);
        else
            err = hu_ingest_file(watcher->alloc, watcher->memory, path, (size_t)n);
        if (err != HU_OK)
            continue;

        if (watcher->processed_dir) {
            char dest[1024];
            int dn = snprintf(dest, sizeof(dest), "%.*s/%s", (int)watcher->processed_dir_len,
                              watcher->processed_dir, ent->d_name);
            if (dn > 0 && (size_t)dn < sizeof(dest))
                rename(path, dest);
        }
        (*processed_count)++;
        watcher->files_ingested++;
    }
    closedir(dir);
    return HU_OK;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

void hu_inbox_deinit(hu_inbox_watcher_t *watcher) {
    if (!watcher || !watcher->alloc)
        return;
    if (watcher->inbox_dir)
        watcher->alloc->free(watcher->alloc->ctx, watcher->inbox_dir, watcher->inbox_dir_len + 1);
    if (watcher->processed_dir)
        watcher->alloc->free(watcher->alloc->ctx, watcher->processed_dir,
                             watcher->processed_dir_len + 1);
    memset(watcher, 0, sizeof(*watcher));
}
