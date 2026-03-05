#include "seaclaw/agent/task_list.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SC_TASK_LIST_JSON_FILENAME "tasks.json"
#define SC_TASK_LIST_MAX_PATH 1024

struct sc_task_list {
    sc_allocator_t *alloc;
    sc_task_t *tasks;
    size_t count;
    size_t max_tasks;
    uint64_t next_id;
    char *dir_path; /* owned; NULL = in-memory only */
};

static const char *status_to_string(sc_task_list_status_t s) {
    switch (s) {
    case SC_TASK_LIST_PENDING: return "pending";
    case SC_TASK_LIST_CLAIMED: return "claimed";
    case SC_TASK_LIST_IN_PROGRESS: return "in_progress";
    case SC_TASK_LIST_COMPLETED: return "completed";
    case SC_TASK_LIST_FAILED: return "failed";
    case SC_TASK_LIST_CANCELLED: return "cancelled";
    default: return "pending";
    }
}

static sc_task_list_status_t status_from_string(const char *s) {
    if (!s) return SC_TASK_LIST_PENDING;
    if (strcmp(s, "claimed") == 0) return SC_TASK_LIST_CLAIMED;
    if (strcmp(s, "in_progress") == 0) return SC_TASK_LIST_IN_PROGRESS;
    if (strcmp(s, "completed") == 0) return SC_TASK_LIST_COMPLETED;
    if (strcmp(s, "failed") == 0) return SC_TASK_LIST_FAILED;
    if (strcmp(s, "cancelled") == 0) return SC_TASK_LIST_CANCELLED;
    return SC_TASK_LIST_PENDING;
}

static sc_task_t *find_task(sc_task_list_t *list, uint64_t task_id) {
    for (size_t i = 0; i < list->count; i++)
        if (list->tasks[i].id == task_id)
            return &list->tasks[i];
    return NULL;
}

static sc_error_t task_list_save(sc_task_list_t *list) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)list;
    return SC_OK;
#else
    if (!list || !list->dir_path)
        return SC_OK;
    char path[SC_TASK_LIST_MAX_PATH];
    size_t dlen = strlen(list->dir_path);
    if (dlen + 20 >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;
    if (dlen > 0 && list->dir_path[dlen - 1] == '/')
        snprintf(path, sizeof(path), "%s%s", list->dir_path, SC_TASK_LIST_JSON_FILENAME);
    else
        snprintf(path, sizeof(path), "%s/%s", list->dir_path, SC_TASK_LIST_JSON_FILENAME);

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, list->alloc) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) {
        sc_json_buf_free(&buf);
        return SC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < list->count; i++) {
        const sc_task_t *t = &list->tasks[i];
        if (i > 0 && sc_json_buf_append_raw(&buf, ",", 1) != SC_OK)
            goto fail;
        char nbuf[64];
        int nlen = snprintf(nbuf, sizeof(nbuf),
            "{\"id\":%llu,\"subject\":", (unsigned long long)t->id);
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto fail;
        sc_json_append_string(&buf, t->subject ? t->subject : "", t->subject ? strlen(t->subject) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"description\":");
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto fail;
        sc_json_append_string(&buf, t->description ? t->description : "",
            t->description ? strlen(t->description) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"owner\":%llu,\"status\":\"%s\"",
            (unsigned long long)t->owner_agent_id, status_to_string(t->status));
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto fail;
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"blocked_by\":[");
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto fail;
        for (size_t j = 0; j < t->blocked_by_count; j++) {
            if (j > 0 && sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto fail;
            nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)t->blocked_by[j]);
            if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto fail;
        }
        nlen = snprintf(nbuf, sizeof(nbuf), "],\"created_at\":%lld,\"updated_at\":%lld}",
            (long long)t->created_at, (long long)t->updated_at);
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto fail;
    }
    if (sc_json_buf_append_raw(&buf, "]", 1) != SC_OK) goto fail;

    size_t json_len = buf.len;
    char tmp_path[SC_TASK_LIST_MAX_PATH + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        sc_json_buf_free(&buf);
        return SC_ERR_IO;
    }
    size_t written = fwrite(buf.ptr, 1, json_len, f);
    sc_json_buf_free(&buf);
    fclose(f);
    if (written != json_len) {
        (void)remove(tmp_path);
        return SC_ERR_IO;
    }
    if (rename(tmp_path, path) != 0) {
        (void)remove(tmp_path);
        return SC_ERR_IO;
    }
    return SC_OK;
fail:
    sc_json_buf_free(&buf);
    return SC_ERR_OUT_OF_MEMORY;
#endif
}

static sc_error_t task_list_load(sc_task_list_t *list) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)list;
    return SC_OK;
#else
    if (!list || !list->dir_path)
        return SC_OK;
    char path[SC_TASK_LIST_MAX_PATH];
    size_t dlen = strlen(list->dir_path);
    if (dlen + 20 >= sizeof(path))
        return SC_OK;
    if (dlen > 0 && list->dir_path[dlen - 1] == '/')
        snprintf(path, sizeof(path), "%s%s", list->dir_path, SC_TASK_LIST_JSON_FILENAME);
    else
        snprintf(path, sizeof(path), "%s/%s", list->dir_path, SC_TASK_LIST_JSON_FILENAME);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return SC_OK;

    FILE *f = fopen(path, "rb");
    if (!f)
        return SC_OK;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return SC_OK;
    }
    char *content = (char *)list->alloc->alloc(list->alloc->ctx, (size_t)sz + 1);
    if (!content) {
        fclose(f);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(content, 1, (size_t)sz, f);
    fclose(f);
    content[nr] = '\0';

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(list->alloc, content, nr, &root);
    list->alloc->free(list->alloc->ctx, content, (size_t)sz + 1);
    if (err != SC_OK || !root || root->type != SC_JSON_ARRAY)
        return (root ? (void)sc_json_free(list->alloc, root), SC_OK : SC_OK);

    for (size_t i = 0; i < root->data.array.len && list->count < list->max_tasks; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;
        sc_task_t *t = &list->tasks[list->count];
        memset(t, 0, sizeof(*t));
        t->id = (uint64_t)sc_json_get_number(item, "id", 0);
        const char *subj = sc_json_get_string(item, "subject");
        const char *desc = sc_json_get_string(item, "description");
        t->owner_agent_id = (uint64_t)sc_json_get_number(item, "owner", 0);
        const char *status_str = sc_json_get_string(item, "status");
        t->status = status_from_string(status_str);
        t->created_at = (int64_t)sc_json_get_number(item, "created_at", 0);
        t->updated_at = (int64_t)sc_json_get_number(item, "updated_at", 0);

        if (subj) {
            size_t slen = strlen(subj);
            t->subject = sc_strndup(list->alloc, subj, slen);
            if (!t->subject)
                goto load_fail;
        }
        if (desc) {
            size_t dlen = strlen(desc);
            t->description = sc_strndup(list->alloc, desc, dlen);
            if (!t->description && dlen > 0)
                goto load_fail;
        }
        sc_json_value_t *blocked = sc_json_object_get(item, "blocked_by");
        if (blocked && blocked->type == SC_JSON_ARRAY && blocked->data.array.len > 0) {
            size_t bc = blocked->data.array.len;
            t->blocked_by = (uint64_t *)list->alloc->alloc(list->alloc->ctx, bc * sizeof(uint64_t));
            if (!t->blocked_by)
                goto load_fail;
            for (size_t j = 0; j < bc; j++) {
                sc_json_value_t *v = blocked->data.array.items[j];
                if (v && v->type == SC_JSON_NUMBER)
                    t->blocked_by[j] = (uint64_t)v->data.number;
                else if (v && v->type == SC_JSON_STRING)
                    t->blocked_by[j] = (uint64_t)atoll(v->data.string.ptr);
            }
            t->blocked_by_count = bc;
        }
        if (t->id >= list->next_id)
            list->next_id = t->id + 1;
        list->count++;
    }
    sc_json_free(list->alloc, root);
    return SC_OK;
load_fail:
    for (size_t k = 0; k < list->count; k++)
        sc_task_free(list->alloc, &list->tasks[k]);
    list->count = 0;
    sc_json_free(list->alloc, root);
    return SC_OK;
#endif
}

sc_task_list_t *sc_task_list_create(sc_allocator_t *alloc, const char *dir_path, size_t max_tasks) {
    if (!alloc || max_tasks == 0)
        return NULL;
    sc_task_list_t *list =
        (sc_task_list_t *)alloc->alloc(alloc->ctx, sizeof(sc_task_list_t));
    if (!list)
        return NULL;
    memset(list, 0, sizeof(*list));
    list->alloc = alloc;
    list->max_tasks = max_tasks;
    list->tasks = (sc_task_t *)alloc->alloc(alloc->ctx, max_tasks * sizeof(sc_task_t));
    if (!list->tasks) {
        alloc->free(alloc->ctx, list, sizeof(*list));
        return NULL;
    }
    memset(list->tasks, 0, max_tasks * sizeof(sc_task_t));
    list->next_id = 1;

    if (dir_path && dir_path[0] != '\0') {
        size_t dlen = strlen(dir_path);
        list->dir_path = (char *)alloc->alloc(alloc->ctx, dlen + 1);
        if (!list->dir_path) {
            alloc->free(alloc->ctx, list->tasks, max_tasks * sizeof(sc_task_t));
            alloc->free(alloc->ctx, list, sizeof(*list));
            return NULL;
        }
        memcpy(list->dir_path, dir_path, dlen + 1);
        (void)task_list_load(list);
    }
    return list;
}

void sc_task_list_destroy(sc_task_list_t *list) {
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        sc_task_free(list->alloc, &list->tasks[i]);
    list->alloc->free(list->alloc->ctx, list->tasks, list->max_tasks * sizeof(sc_task_t));
    if (list->dir_path)
        list->alloc->free(list->alloc->ctx, list->dir_path, strlen(list->dir_path) + 1);
    list->alloc->free(list->alloc->ctx, list, sizeof(*list));
}

sc_error_t sc_task_list_add(sc_task_list_t *list, const char *subject, const char *description,
                            const uint64_t *blocked_by, size_t blocked_by_count,
                            uint64_t *out_id) {
    if (!list || !subject || !out_id)
        return SC_ERR_INVALID_ARGUMENT;
    if (list->count >= list->max_tasks)
        return SC_ERR_OUT_OF_MEMORY;

    sc_task_t *t = &list->tasks[list->count];
    memset(t, 0, sizeof(*t));
    t->id = list->next_id++;
    t->subject = sc_strndup(list->alloc, subject, strlen(subject));
    if (!t->subject)
        return SC_ERR_OUT_OF_MEMORY;
    t->description =
        description ? sc_strndup(list->alloc, description, strlen(description)) : NULL;
    t->owner_agent_id = 0;
    t->status = SC_TASK_LIST_PENDING;
    t->created_at = (int64_t)time(NULL);
    t->updated_at = t->created_at;

    if (blocked_by && blocked_by_count > 0) {
        t->blocked_by =
            (uint64_t *)list->alloc->alloc(list->alloc->ctx, blocked_by_count * sizeof(uint64_t));
        if (!t->blocked_by) {
            list->alloc->free(list->alloc->ctx, t->subject, strlen(subject) + 1);
            if (t->description)
                list->alloc->free(list->alloc->ctx, t->description, strlen(description) + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(t->blocked_by, blocked_by, blocked_by_count * sizeof(uint64_t));
        t->blocked_by_count = blocked_by_count;
    }

    *out_id = t->id;
    list->count++;
    (void)task_list_save(list);
    return SC_OK;
}

bool sc_task_list_is_blocked(sc_task_list_t *list, uint64_t task_id) {
    if (!list)
        return true;
    sc_task_t *t = find_task(list, task_id);
    if (!t || !t->blocked_by || t->blocked_by_count == 0)
        return false;
    for (size_t i = 0; i < t->blocked_by_count; i++) {
        sc_task_t *dep = find_task(list, t->blocked_by[i]);
        if (!dep || dep->status != SC_TASK_LIST_COMPLETED)
            return true;
    }
    return false;
}

sc_error_t sc_task_list_claim(sc_task_list_t *list, uint64_t task_id, uint64_t agent_id) {
    if (!list)
        return SC_ERR_INVALID_ARGUMENT;
    sc_task_t *t = find_task(list, task_id);
    if (!t)
        return SC_ERR_NOT_FOUND;
    if (t->status != SC_TASK_LIST_PENDING)
        return SC_ERR_ALREADY_EXISTS;
    if (sc_task_list_is_blocked(list, task_id))
        return SC_ERR_INVALID_ARGUMENT;
    t->owner_agent_id = agent_id;
    t->status = SC_TASK_LIST_CLAIMED;
    t->updated_at = (int64_t)time(NULL);
    (void)task_list_save(list);
    return SC_OK;
}

sc_error_t sc_task_list_update_status(sc_task_list_t *list, uint64_t task_id,
                                      sc_task_list_status_t status) {
    if (!list)
        return SC_ERR_INVALID_ARGUMENT;
    sc_task_t *t = find_task(list, task_id);
    if (!t)
        return SC_ERR_NOT_FOUND;
    t->status = status;
    t->updated_at = (int64_t)time(NULL);
    (void)task_list_save(list);
    return SC_OK;
}

sc_error_t sc_task_list_next_available(sc_task_list_t *list, sc_task_t *out) {
    if (!list || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < list->count; i++) {
        sc_task_t *t = &list->tasks[i];
        if (t->status != SC_TASK_LIST_PENDING || t->owner_agent_id != 0)
            continue;
        if (sc_task_list_is_blocked(list, t->id))
            continue;
        *out = *t;
        if (out->subject)
            out->subject = sc_strndup(list->alloc, out->subject, strlen(out->subject));
        if (out->description)
            out->description =
                sc_strndup(list->alloc, out->description, strlen(out->description));
        if (out->blocked_by && out->blocked_by_count > 0) {
            uint64_t *cpy = (uint64_t *)list->alloc->alloc(
                list->alloc->ctx, out->blocked_by_count * sizeof(uint64_t));
            if (cpy) {
                memcpy(cpy, t->blocked_by, out->blocked_by_count * sizeof(uint64_t));
                out->blocked_by = cpy;
            } else {
                out->blocked_by = NULL;
                out->blocked_by_count = 0;
            }
        }
        return SC_OK;
    }
    return SC_ERR_NOT_FOUND;
}

sc_error_t sc_task_list_get(sc_task_list_t *list, uint64_t task_id, sc_task_t *out) {
    if (!list || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_task_t *t = find_task(list, task_id);
    if (!t) {
        memset(out, 0, sizeof(*out));
        return SC_ERR_NOT_FOUND;
    }
    *out = *t;
    if (out->subject)
        out->subject = sc_strndup(list->alloc, out->subject, strlen(out->subject));
    if (out->description)
        out->description =
            sc_strndup(list->alloc, out->description, strlen(out->description));
    if (out->blocked_by && out->blocked_by_count > 0) {
        uint64_t *cpy = (uint64_t *)list->alloc->alloc(
            list->alloc->ctx, out->blocked_by_count * sizeof(uint64_t));
        if (cpy) {
            memcpy(cpy, t->blocked_by, out->blocked_by_count * sizeof(uint64_t));
            out->blocked_by = cpy;
        } else {
            out->blocked_by = NULL;
            out->blocked_by_count = 0;
        }
    }
    return SC_OK;
}

sc_error_t sc_task_list_all(sc_task_list_t *list, sc_task_t **out, size_t *out_count) {
    if (!list || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (list->count == 0)
        return SC_OK;
    sc_task_t *arr =
        (sc_task_t *)list->alloc->alloc(list->alloc->ctx, list->count * sizeof(sc_task_t));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;
    memset(arr, 0, list->count * sizeof(sc_task_t));
    for (size_t i = 0; i < list->count; i++) {
        arr[i] = list->tasks[i];
        if (arr[i].subject)
            arr[i].subject =
                sc_strndup(list->alloc, arr[i].subject, strlen(arr[i].subject));
        if (arr[i].description)
            arr[i].description =
                sc_strndup(list->alloc, arr[i].description, strlen(arr[i].description));
        if (arr[i].blocked_by && arr[i].blocked_by_count > 0) {
            uint64_t *cpy = (uint64_t *)list->alloc->alloc(
                list->alloc->ctx, arr[i].blocked_by_count * sizeof(uint64_t));
            if (cpy) {
                memcpy(cpy, list->tasks[i].blocked_by,
                       arr[i].blocked_by_count * sizeof(uint64_t));
                arr[i].blocked_by = cpy;
            } else {
                arr[i].blocked_by = NULL;
                arr[i].blocked_by_count = 0;
            }
        }
    }
    *out = arr;
    *out_count = list->count;
    return SC_OK;
}

size_t sc_task_list_count_by_status(sc_task_list_t *list, sc_task_list_status_t status) {
    if (!list)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < list->count; i++)
        if (list->tasks[i].status == status)
            n++;
    return n;
}

bool sc_task_list_is_ready(sc_task_list_t *list, uint64_t task_id) {
    return list && !sc_task_list_is_blocked(list, task_id);
}

sc_error_t sc_task_list_query(sc_task_list_t *list, sc_task_list_status_t status,
                              sc_task_t **out, size_t *out_count) {
    if (!list || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    size_t n = sc_task_list_count_by_status(list, status);
    if (n == 0)
        return SC_OK;
    sc_task_t *arr =
        (sc_task_t *)list->alloc->alloc(list->alloc->ctx, n * sizeof(sc_task_t));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;
    memset(arr, 0, n * sizeof(sc_task_t));
    size_t j = 0;
    for (size_t i = 0; i < list->count && j < n; i++) {
        if (list->tasks[i].status != status)
            continue;
        arr[j] = list->tasks[i];
        if (arr[j].subject)
            arr[j].subject =
                sc_strndup(list->alloc, arr[j].subject, strlen(arr[j].subject));
        if (arr[j].description)
            arr[j].description =
                sc_strndup(list->alloc, arr[j].description, strlen(arr[j].description));
        if (arr[j].blocked_by && arr[j].blocked_by_count > 0) {
            uint64_t *cpy = (uint64_t *)list->alloc->alloc(
                list->alloc->ctx, arr[j].blocked_by_count * sizeof(uint64_t));
            if (cpy) {
                memcpy(cpy, list->tasks[i].blocked_by,
                       arr[j].blocked_by_count * sizeof(uint64_t));
                arr[j].blocked_by = cpy;
            } else {
                arr[j].blocked_by = NULL;
                arr[j].blocked_by_count = 0;
            }
        }
        j++;
    }
    *out = arr;
    *out_count = j;
    return SC_OK;
}

void sc_task_array_free(sc_allocator_t *alloc, sc_task_t *tasks, size_t count) {
    if (!alloc || !tasks)
        return;
    for (size_t i = 0; i < count; i++)
        sc_task_free(alloc, &tasks[i]);
    alloc->free(alloc->ctx, tasks, count * sizeof(sc_task_t));
}

void sc_task_free(sc_allocator_t *alloc, sc_task_t *task) {
    if (!alloc || !task)
        return;
    if (task->subject) {
        alloc->free(alloc->ctx, task->subject, strlen(task->subject) + 1);
        task->subject = NULL;
    }
    if (task->description) {
        alloc->free(alloc->ctx, task->description, strlen(task->description) + 1);
        task->description = NULL;
    }
    if (task->blocked_by) {
        alloc->free(alloc->ctx, task->blocked_by,
                    task->blocked_by_count * sizeof(uint64_t));
        task->blocked_by = NULL;
        task->blocked_by_count = 0;
    }
}

#if defined(SC_IS_TEST) && SC_IS_TEST
sc_error_t sc_task_list_serialize(sc_task_list_t *list, char **out_json, size_t *out_len) {
    if (!list || !out_json || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, list->alloc) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) {
        sc_json_buf_free(&buf);
        return SC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < list->count; i++) {
        const sc_task_t *t = &list->tasks[i];
        if (i > 0 && sc_json_buf_append_raw(&buf, ",", 1) != SC_OK)
            goto ser_fail;
        char nbuf[64];
        int nlen = snprintf(nbuf, sizeof(nbuf), "{\"id\":%llu,\"subject\":",
            (unsigned long long)t->id);
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto ser_fail;
        sc_json_append_string(&buf, t->subject ? t->subject : "", t->subject ? strlen(t->subject) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"description\":");
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto ser_fail;
        sc_json_append_string(&buf, t->description ? t->description : "",
            t->description ? strlen(t->description) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"owner\":%llu,\"status\":\"%s\"",
            (unsigned long long)t->owner_agent_id, status_to_string(t->status));
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto ser_fail;
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"blocked_by\":[");
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto ser_fail;
        for (size_t j = 0; j < t->blocked_by_count; j++) {
            if (j > 0 && sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto ser_fail;
            nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)t->blocked_by[j]);
            if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto ser_fail;
        }
        nlen = snprintf(nbuf, sizeof(nbuf), "],\"created_at\":%lld,\"updated_at\":%lld}",
            (long long)t->created_at, (long long)t->updated_at);
        if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK) goto ser_fail;
    }
    if (sc_json_buf_append_raw(&buf, "]", 1) != SC_OK) goto ser_fail;

    *out_json = (char *)list->alloc->alloc(list->alloc->ctx, buf.len + 1);
    if (!*out_json) {
        sc_json_buf_free(&buf);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(*out_json, buf.ptr, buf.len);
    (*out_json)[buf.len] = '\0';
    *out_len = buf.len;
    sc_json_buf_free(&buf);
    return SC_OK;
ser_fail:
    sc_json_buf_free(&buf);
    return SC_ERR_OUT_OF_MEMORY;
}

sc_error_t sc_task_list_deserialize(sc_task_list_t *list, const char *json, size_t json_len) {
    if (!list || !json)
        return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < list->count; i++)
        sc_task_free(list->alloc, &list->tasks[i]);
    list->count = 0;
    list->next_id = 1;

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(list->alloc, json, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_ARRAY) {
        if (root)
            sc_json_free(list->alloc, root);
        return err != SC_OK ? err : SC_ERR_JSON_PARSE;
    }

    for (size_t i = 0; i < root->data.array.len && list->count < list->max_tasks; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;
        sc_task_t *t = &list->tasks[list->count];
        memset(t, 0, sizeof(*t));
        t->id = (uint64_t)sc_json_get_number(item, "id", 0);
        const char *subj = sc_json_get_string(item, "subject");
        const char *desc = sc_json_get_string(item, "description");
        t->owner_agent_id = (uint64_t)sc_json_get_number(item, "owner", 0);
        const char *status_str = sc_json_get_string(item, "status");
        t->status = status_from_string(status_str);
        t->created_at = (int64_t)sc_json_get_number(item, "created_at", 0);
        t->updated_at = (int64_t)sc_json_get_number(item, "updated_at", 0);

        if (subj) {
            size_t slen = strlen(subj);
            t->subject = sc_strndup(list->alloc, subj, slen);
            if (!t->subject)
                goto des_fail;
        }
        if (desc) {
            size_t dlen = strlen(desc);
            t->description = sc_strndup(list->alloc, desc, dlen);
            if (!t->description && dlen > 0)
                goto des_fail;
        }
        sc_json_value_t *blocked = sc_json_object_get(item, "blocked_by");
        if (blocked && blocked->type == SC_JSON_ARRAY && blocked->data.array.len > 0) {
            size_t bc = blocked->data.array.len;
            t->blocked_by = (uint64_t *)list->alloc->alloc(list->alloc->ctx, bc * sizeof(uint64_t));
            if (!t->blocked_by)
                goto des_fail;
            for (size_t j = 0; j < bc; j++) {
                sc_json_value_t *v = blocked->data.array.items[j];
                if (v && v->type == SC_JSON_NUMBER)
                    t->blocked_by[j] = (uint64_t)v->data.number;
                else if (v && v->type == SC_JSON_STRING)
                    t->blocked_by[j] = (uint64_t)atoll(v->data.string.ptr);
            }
            t->blocked_by_count = bc;
        }
        if (t->id >= list->next_id)
            list->next_id = t->id + 1;
        list->count++;
    }
    sc_json_free(list->alloc, root);
    return SC_OK;
des_fail:
    for (size_t k = 0; k < list->count; k++)
        sc_task_free(list->alloc, &list->tasks[k]);
    list->count = 0;
    sc_json_free(list->alloc, root);
    return SC_ERR_OUT_OF_MEMORY;
}
#endif
