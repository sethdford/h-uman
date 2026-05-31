/*
 * Crontab storage — ~/.human/crontab.json
 * Format: [{"id":"1","schedule":"0 * * * *","command":"echo hello","enabled":true}]
 * In HU_IS_TEST: uses temp path.
 */
#include "human/crontab.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HU_CRONTAB_FILE        "crontab.json"
#define HU_CRONTAB_DIR         ".human"
#define HU_CRONTAB_TEST_FILE   "human_crontab_test.json"
#define HU_CRONTAB_MAX_ENTRIES 256
#define HU_CRONTAB_ID_MAX      64

static hu_error_t ensure_dir(const char *path) {
    char dir[1024];
    const char *sep = strrchr(path, '/');
    if (!sep || sep <= path)
        return HU_OK;
    size_t dlen = (size_t)(sep - path);
    if (dlen >= sizeof(dir))
        return HU_ERR_INTERNAL;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
#ifndef _WIN32
    if (mkdir(dir, 0755) != 0) {
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
            return HU_OK;
        return HU_ERR_IO;
    }
#endif
    return HU_OK;
}

hu_error_t hu_crontab_get_path(hu_allocator_t *alloc, char **path, size_t *path_len) {
    if (!alloc || !path || !path_len)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    char *tmp = hu_platform_get_temp_dir(alloc);
    if (!tmp) {
        const char *t = getenv("TMPDIR");
        if (!t)
            t = getenv("TEMP");
        if (!t)
            t = "/tmp";
        size_t tlen = strlen(t);
        size_t cap = tlen + strlen(HU_CRONTAB_TEST_FILE) + 2;
        *path = (char *)alloc->alloc(alloc->ctx, cap);
        if (!*path)
            return HU_ERR_OUT_OF_MEMORY;
        int n = snprintf(*path, cap, "%s/%s", t, HU_CRONTAB_TEST_FILE);
        if (n < 0 || (size_t)n >= cap) {
            alloc->free(alloc->ctx, *path, cap);
            *path = NULL;
            return HU_ERR_INTERNAL;
        }
        *path_len = (size_t)n;
        return HU_OK;
    }
    size_t tlen = strlen(tmp);
    size_t cap = tlen + strlen(HU_CRONTAB_TEST_FILE) + 2;
    *path = (char *)alloc->alloc(alloc->ctx, cap);
    if (!*path) {
        alloc->free(alloc->ctx, tmp, tlen + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(*path, cap, "%s/%s", tmp, HU_CRONTAB_TEST_FILE);
    alloc->free(alloc->ctx, tmp, tlen + 1);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, *path, cap);
        *path = NULL;
        return HU_ERR_INTERNAL;
    }
    *path_len = (size_t)n;
    return HU_OK;
#else
    char *home = hu_platform_get_home_dir(alloc);
    if (!home)
        return HU_ERR_IO;
    size_t hlen = strlen(home);
    size_t need = hlen + strlen(HU_CRONTAB_DIR) + strlen(HU_CRONTAB_FILE) + 4;
    *path = (char *)alloc->alloc(alloc->ctx, need);
    if (!*path) {
        alloc->free(alloc->ctx, home, hlen + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(*path, need, "%s/%s/%s", home, HU_CRONTAB_DIR, HU_CRONTAB_FILE);
    alloc->free(alloc->ctx, home, hlen + 1);
    if (n <= 0 || (size_t)n >= need) {
        alloc->free(alloc->ctx, *path, need);
        return HU_ERR_INTERNAL;
    }
    *path_len = (size_t)n;
    return HU_OK;
#endif
}

static void free_entry(hu_allocator_t *alloc, hu_crontab_entry_t *e) {
    if (!e || !alloc)
        return;
    if (e->id)
        alloc->free(alloc->ctx, e->id, strlen(e->id) + 1);
    if (e->schedule)
        alloc->free(alloc->ctx, e->schedule, strlen(e->schedule) + 1);
    if (e->command)
        alloc->free(alloc->ctx, e->command, strlen(e->command) + 1);
}

void hu_crontab_entries_free(hu_allocator_t *alloc, hu_crontab_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        free_entry(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_crontab_entry_t));
}

hu_error_t hu_crontab_load(hu_allocator_t *alloc, const char *path, hu_crontab_entry_t **entries,
                           size_t *count) {
    if (!alloc || !path || !entries || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *entries = NULL;
    *count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        *entries = (hu_crontab_entry_t *)alloc->alloc(alloc->ctx, 0);
        return HU_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz >= 65536) {
        fclose(f);
        return HU_OK;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, rd, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY) {
        if (root)
            hu_json_free(alloc, root);
        return HU_OK;
    }

    size_t n = root->data.array.len;
    if (n > HU_CRONTAB_MAX_ENTRIES)
        n = HU_CRONTAB_MAX_ENTRIES;

    hu_crontab_entry_t *out =
        (hu_crontab_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_crontab_entry_t));
    if (!out) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(out, 0, n * sizeof(hu_crontab_entry_t));

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *id = hu_json_get_string(item, "id");
        const char *schedule = hu_json_get_string(item, "schedule");
        const char *command = hu_json_get_string(item, "command");
        if (!id || !schedule || !command)
            continue;

        out[j].id = hu_strndup(alloc, id, strlen(id));
        out[j].schedule = hu_strndup(alloc, schedule, strlen(schedule));
        out[j].command = hu_strndup(alloc, command, strlen(command));
        out[j].enabled = hu_json_get_bool(item, "enabled", true);
        if (out[j].id && out[j].schedule && out[j].command)
            j++;
        else {
            free_entry(alloc, &out[j]);
        }
    }
    hu_json_free(alloc, root);
    *entries = out;
    *count = j;
    return HU_OK;
}

static void generate_id(char *buf, size_t cap, size_t count) {
    snprintf(buf, cap, "%zu", count + 1);
}

hu_error_t hu_crontab_save(hu_allocator_t *alloc, const char *path,
                           const hu_crontab_entry_t *entries, size_t count) {
    if (!alloc || !path || !entries)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = ensure_dir(path);
    if (err != HU_OK)
        return err;

    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < count; i++) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, arr);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_object_set(alloc, obj, "id",
                           hu_json_string_new(alloc, entries[i].id, strlen(entries[i].id)));
        hu_json_object_set(
            alloc, obj, "schedule",
            hu_json_string_new(alloc, entries[i].schedule, strlen(entries[i].schedule)));
        hu_json_object_set(
            alloc, obj, "command",
            hu_json_string_new(alloc, entries[i].command, strlen(entries[i].command)));
        hu_json_object_set(alloc, obj, "enabled", hu_json_bool_new(alloc, entries[i].enabled));
        hu_json_array_push(alloc, arr, obj);
    }

    char *json_str = NULL;
    size_t json_len = 0;
    err = hu_json_stringify(alloc, arr, &json_str, &json_len);
    hu_json_free(alloc, arr);
    if (err != HU_OK || !json_str)
        return err;

    FILE *f = fopen(path, "wb");
    if (!f) {
        alloc->free(alloc->ctx, json_str, json_len + 1);
        return HU_ERR_IO;
    }
    fwrite(json_str, 1, json_len, f);
    fclose(f);
    alloc->free(alloc->ctx, json_str, json_len + 1);
    return HU_OK;
}

hu_error_t hu_crontab_add(hu_allocator_t *alloc, const char *path, const char *schedule,
                          size_t schedule_len, const char *command, size_t command_len,
                          char **new_id) {
    if (!alloc || !path || !schedule || !command || !new_id)
        return HU_ERR_INVALID_ARGUMENT;
    *new_id = NULL;

    hu_crontab_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_crontab_load(alloc, path, &entries, &count);
    if (err != HU_OK)
        return err;

    size_t new_cap = count + 1;
    hu_crontab_entry_t *nentries = (hu_crontab_entry_t *)alloc->realloc(
        alloc->ctx, entries, count * sizeof(hu_crontab_entry_t),
        new_cap * sizeof(hu_crontab_entry_t));
    if (!nentries) {
        hu_crontab_entries_free(alloc, entries, count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    entries = nentries;
    memset(&entries[count], 0, sizeof(hu_crontab_entry_t));

    char id_buf[HU_CRONTAB_ID_MAX];
    generate_id(id_buf, sizeof(id_buf), count);
    entries[count].id = hu_strndup(alloc, id_buf, strlen(id_buf));
    entries[count].schedule = hu_strndup(alloc, schedule, schedule_len);
    entries[count].command = hu_strndup(alloc, command, command_len);
    entries[count].enabled = true;

    if (!entries[count].id || !entries[count].schedule || !entries[count].command) {
        free_entry(alloc, &entries[count]);
        hu_crontab_entries_free(alloc, entries, count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    *new_id = hu_strndup(alloc, id_buf, strlen(id_buf));
    if (!*new_id) {
        free_entry(alloc, &entries[count]);
        hu_crontab_entries_free(alloc, entries, count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    err = hu_crontab_save(alloc, path, entries, count + 1);
    hu_crontab_entries_free(alloc, entries, count + 1);
    return err;
}

hu_error_t hu_crontab_remove(hu_allocator_t *alloc, const char *path, const char *id) {
    if (!alloc || !path || !id)
        return HU_ERR_INVALID_ARGUMENT;

    hu_crontab_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_crontab_load(alloc, path, &entries, &count);
    if (err != HU_OK)
        return err;

    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].id, id) == 0) {
            free_entry(alloc, &entries[i]);
            memset(&entries[i], 0, sizeof(entries[i]));
            continue;
        }
        if (j != i)
            entries[j] = entries[i];
        j++;
    }

    err = hu_crontab_save(alloc, path, entries, j);
    for (size_t i = 0; i < j; i++)
        free_entry(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_crontab_entry_t));
    return err;
}
