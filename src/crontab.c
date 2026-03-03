/*
 * Crontab storage — ~/.seaclaw/crontab.json
 * Format: [{"id":"1","schedule":"0 * * * *","command":"echo hello","enabled":true}]
 * In SC_IS_TEST: uses temp path.
 */
#include "seaclaw/crontab.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SC_CRONTAB_FILE        "crontab.json"
#define SC_CRONTAB_DIR         ".seaclaw"
#define SC_CRONTAB_TEST_FILE   "seaclaw_crontab_test.json"
#define SC_CRONTAB_MAX_ENTRIES 256
#define SC_CRONTAB_ID_MAX      64

static sc_error_t ensure_dir(const char *path) {
    char dir[1024];
    const char *sep = strrchr(path, '/');
    if (!sep || sep <= path)
        return SC_OK;
    size_t dlen = (size_t)(sep - path);
    if (dlen >= sizeof(dir))
        return SC_ERR_INTERNAL;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
#ifndef _WIN32
    if (mkdir(dir, 0755) != 0) {
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
            return SC_OK;
        return SC_ERR_IO;
    }
#endif
    return SC_OK;
}

sc_error_t sc_crontab_get_path(sc_allocator_t *alloc, char **path, size_t *path_len) {
    if (!alloc || !path || !path_len)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    char *tmp = sc_platform_get_temp_dir(alloc);
    if (!tmp) {
        const char *t = getenv("TMPDIR");
        if (!t)
            t = getenv("TEMP");
        if (!t)
            t = "/tmp";
        size_t tlen = strlen(t);
        *path = (char *)alloc->alloc(alloc->ctx, tlen + strlen(SC_CRONTAB_TEST_FILE) + 2);
        if (!*path)
            return SC_ERR_OUT_OF_MEMORY;
        int n = snprintf(*path, tlen + 50, "%s/%s", t, SC_CRONTAB_TEST_FILE);
        *path_len = (size_t)n;
        return SC_OK;
    }
    size_t tlen = strlen(tmp);
    *path = (char *)alloc->alloc(alloc->ctx, tlen + strlen(SC_CRONTAB_TEST_FILE) + 2);
    if (!*path) {
        alloc->free(alloc->ctx, tmp, tlen + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(*path, tlen + 50, "%s/%s", tmp, SC_CRONTAB_TEST_FILE);
    alloc->free(alloc->ctx, tmp, tlen + 1);
    *path_len = (size_t)n;
    return SC_OK;
#else
    char *home = sc_platform_get_home_dir(alloc);
    if (!home)
        return SC_ERR_IO;
    size_t hlen = strlen(home);
    size_t need = hlen + strlen(SC_CRONTAB_DIR) + strlen(SC_CRONTAB_FILE) + 4;
    *path = (char *)alloc->alloc(alloc->ctx, need);
    if (!*path) {
        alloc->free(alloc->ctx, home, hlen + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(*path, need, "%s/%s/%s", home, SC_CRONTAB_DIR, SC_CRONTAB_FILE);
    alloc->free(alloc->ctx, home, hlen + 1);
    if (n <= 0 || (size_t)n >= need) {
        alloc->free(alloc->ctx, *path, need);
        return SC_ERR_INTERNAL;
    }
    *path_len = (size_t)n;
    return SC_OK;
#endif
}

static void free_entry(sc_allocator_t *alloc, sc_crontab_entry_t *e) {
    if (!e || !alloc)
        return;
    if (e->id)
        alloc->free(alloc->ctx, e->id, strlen(e->id) + 1);
    if (e->schedule)
        alloc->free(alloc->ctx, e->schedule, strlen(e->schedule) + 1);
    if (e->command)
        alloc->free(alloc->ctx, e->command, strlen(e->command) + 1);
}

void sc_crontab_entries_free(sc_allocator_t *alloc, sc_crontab_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        free_entry(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_crontab_entry_t));
}

sc_error_t sc_crontab_load(sc_allocator_t *alloc, const char *path, sc_crontab_entry_t **entries,
                           size_t *count) {
    if (!alloc || !path || !entries || !count)
        return SC_ERR_INVALID_ARGUMENT;
    *entries = NULL;
    *count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        *entries = (sc_crontab_entry_t *)alloc->alloc(alloc->ctx, 0);
        return SC_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz >= 65536) {
        fclose(f);
        return SC_OK;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, buf, rd, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != SC_OK || !root || root->type != SC_JSON_ARRAY) {
        if (root)
            sc_json_free(alloc, root);
        return SC_OK;
    }

    size_t n = root->data.array.len;
    if (n > SC_CRONTAB_MAX_ENTRIES)
        n = SC_CRONTAB_MAX_ENTRIES;

    sc_crontab_entry_t *out =
        (sc_crontab_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_crontab_entry_t));
    if (!out) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(out, 0, n * sizeof(sc_crontab_entry_t));

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;
        const char *id = sc_json_get_string(item, "id");
        const char *schedule = sc_json_get_string(item, "schedule");
        const char *command = sc_json_get_string(item, "command");
        if (!id || !schedule || !command)
            continue;

        out[j].id = sc_strndup(alloc, id, strlen(id));
        out[j].schedule = sc_strndup(alloc, schedule, strlen(schedule));
        out[j].command = sc_strndup(alloc, command, strlen(command));
        out[j].enabled = sc_json_get_bool(item, "enabled", true);
        if (out[j].id && out[j].schedule && out[j].command)
            j++;
        else {
            free_entry(alloc, &out[j]);
        }
    }
    sc_json_free(alloc, root);
    *entries = out;
    *count = j;
    return SC_OK;
}

static void generate_id(char *buf, size_t cap, size_t count) {
    snprintf(buf, cap, "%zu", count + 1);
}

sc_error_t sc_crontab_save(sc_allocator_t *alloc, const char *path,
                           const sc_crontab_entry_t *entries, size_t count) {
    if (!alloc || !path || !entries)
        return SC_ERR_INVALID_ARGUMENT;

    sc_error_t err = ensure_dir(path);
    if (err != SC_OK)
        return err;

    sc_json_value_t *arr = sc_json_array_new(alloc);
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < count; i++) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) {
            sc_json_free(alloc, arr);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_object_set(alloc, obj, "id",
                           sc_json_string_new(alloc, entries[i].id, strlen(entries[i].id)));
        sc_json_object_set(
            alloc, obj, "schedule",
            sc_json_string_new(alloc, entries[i].schedule, strlen(entries[i].schedule)));
        sc_json_object_set(
            alloc, obj, "command",
            sc_json_string_new(alloc, entries[i].command, strlen(entries[i].command)));
        sc_json_object_set(alloc, obj, "enabled", sc_json_bool_new(alloc, entries[i].enabled));
        sc_json_array_push(alloc, arr, obj);
    }

    char *json_str = NULL;
    size_t json_len = 0;
    err = sc_json_stringify(alloc, arr, &json_str, &json_len);
    sc_json_free(alloc, arr);
    if (err != SC_OK || !json_str)
        return err;

    FILE *f = fopen(path, "wb");
    if (!f) {
        alloc->free(alloc->ctx, json_str, json_len + 1);
        return SC_ERR_IO;
    }
    fwrite(json_str, 1, json_len, f);
    fclose(f);
    alloc->free(alloc->ctx, json_str, json_len + 1);
    return SC_OK;
}

sc_error_t sc_crontab_add(sc_allocator_t *alloc, const char *path, const char *schedule,
                          size_t schedule_len, const char *command, size_t command_len,
                          char **new_id) {
    if (!alloc || !path || !schedule || !command || !new_id)
        return SC_ERR_INVALID_ARGUMENT;
    *new_id = NULL;

    sc_crontab_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = sc_crontab_load(alloc, path, &entries, &count);
    if (err != SC_OK)
        return err;

    size_t new_cap = count + 1;
    sc_crontab_entry_t *nentries = (sc_crontab_entry_t *)alloc->realloc(
        alloc->ctx, entries, count * sizeof(sc_crontab_entry_t),
        new_cap * sizeof(sc_crontab_entry_t));
    if (!nentries) {
        sc_crontab_entries_free(alloc, entries, count);
        return SC_ERR_OUT_OF_MEMORY;
    }
    entries = nentries;
    memset(&entries[count], 0, sizeof(sc_crontab_entry_t));

    char id_buf[SC_CRONTAB_ID_MAX];
    generate_id(id_buf, sizeof(id_buf), count);
    entries[count].id = sc_strndup(alloc, id_buf, strlen(id_buf));
    entries[count].schedule = sc_strndup(alloc, schedule, schedule_len);
    entries[count].command = sc_strndup(alloc, command, command_len);
    entries[count].enabled = true;

    if (!entries[count].id || !entries[count].schedule || !entries[count].command) {
        free_entry(alloc, &entries[count]);
        sc_crontab_entries_free(alloc, entries, count);
        return SC_ERR_OUT_OF_MEMORY;
    }

    *new_id = sc_strndup(alloc, id_buf, strlen(id_buf));
    if (!*new_id) {
        free_entry(alloc, &entries[count]);
        sc_crontab_entries_free(alloc, entries, count);
        return SC_ERR_OUT_OF_MEMORY;
    }

    err = sc_crontab_save(alloc, path, entries, count + 1);
    sc_crontab_entries_free(alloc, entries, count + 1);
    return err;
}

sc_error_t sc_crontab_remove(sc_allocator_t *alloc, const char *path, const char *id) {
    if (!alloc || !path || !id)
        return SC_ERR_INVALID_ARGUMENT;

    sc_crontab_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = sc_crontab_load(alloc, path, &entries, &count);
    if (err != SC_OK)
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

    err = sc_crontab_save(alloc, path, entries, j);
    for (size_t i = 0; i < j; i++)
        free_entry(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_crontab_entry_t));
    return err;
}
