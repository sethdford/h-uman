#include "human/agent/task_list.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define HU_TASK_LIST_JSON_FILENAME "tasks.json"
#define HU_TASK_LIST_MAX_PATH      1024

struct hu_task_list {
    hu_allocator_t *alloc;
    hu_task_t *tasks;
    size_t count;
    size_t max_tasks;
    uint64_t next_id;
    char *dir_path; /* owned; NULL = in-memory only */
};

static const char *status_to_string(hu_task_list_status_t s) {
    switch (s) {
    case HU_TASK_LIST_PENDING:
        return "pending";
    case HU_TASK_LIST_CLAIMED:
        return "claimed";
    case HU_TASK_LIST_IN_PROGRESS:
        return "in_progress";
    case HU_TASK_LIST_COMPLETED:
        return "completed";
    case HU_TASK_LIST_FAILED:
        return "failed";
    case HU_TASK_LIST_CANCELLED:
        return "cancelled";
    default:
        return "pending";
    }
}

static hu_task_list_status_t status_from_string(const char *s) {
    if (!s)
        return HU_TASK_LIST_PENDING;
    if (strcmp(s, "claimed") == 0)
        return HU_TASK_LIST_CLAIMED;
    if (strcmp(s, "in_progress") == 0)
        return HU_TASK_LIST_IN_PROGRESS;
    if (strcmp(s, "completed") == 0)
        return HU_TASK_LIST_COMPLETED;
    if (strcmp(s, "failed") == 0)
        return HU_TASK_LIST_FAILED;
    if (strcmp(s, "cancelled") == 0)
        return HU_TASK_LIST_CANCELLED;
    return HU_TASK_LIST_PENDING;
}

static hu_task_t *find_task(hu_task_list_t *list, uint64_t task_id) {
    for (size_t i = 0; i < list->count; i++)
        if (list->tasks[i].id == task_id)
            return &list->tasks[i];
    return NULL;
}

static hu_error_t task_list_save(hu_task_list_t *list) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)list;
    return HU_OK;
#else
    if (!list || !list->dir_path)
        return HU_OK;
    char path[HU_TASK_LIST_MAX_PATH];
    size_t dlen = strlen(list->dir_path);
    if (dlen + 20 >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    if (dlen > 0 && list->dir_path[dlen - 1] == '/')
        snprintf(path, sizeof(path), "%s%s", list->dir_path, HU_TASK_LIST_JSON_FILENAME);
    else
        snprintf(path, sizeof(path), "%s/%s", list->dir_path, HU_TASK_LIST_JSON_FILENAME);

    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, list->alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (hu_json_buf_append_raw(&buf, "[", 1) != HU_OK) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < list->count; i++) {
        const hu_task_t *t = &list->tasks[i];
        if (i > 0 && hu_json_buf_append_raw(&buf, ",", 1) != HU_OK)
            goto fail;
        char nbuf[64];
        int nlen =
            snprintf(nbuf, sizeof(nbuf), "{\"id\":%llu,\"subject\":", (unsigned long long)t->id);
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto fail;
        hu_json_append_string(&buf, t->subject ? t->subject : "",
                              t->subject ? strlen(t->subject) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"description\":");
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto fail;
        hu_json_append_string(&buf, t->description ? t->description : "",
                              t->description ? strlen(t->description) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"owner\":%llu,\"status\":\"%s\"",
                        (unsigned long long)t->owner_agent_id, status_to_string(t->status));
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto fail;
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"blocked_by\":[");
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto fail;
        for (size_t j = 0; j < t->blocked_by_count; j++) {
            if (j > 0 && hu_json_buf_append_raw(&buf, ",", 1) != HU_OK)
                goto fail;
            nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)t->blocked_by[j]);
            if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
                goto fail;
        }
        nlen = snprintf(nbuf, sizeof(nbuf), "],\"created_at\":%lld,\"updated_at\":%lld}",
                        (long long)t->created_at, (long long)t->updated_at);
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto fail;
    }
    if (hu_json_buf_append_raw(&buf, "]", 1) != HU_OK)
        goto fail;

    size_t json_len = buf.len;
    char tmp_path[HU_TASK_LIST_MAX_PATH + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    size_t written = fwrite(buf.ptr, 1, json_len, f);
    hu_json_buf_free(&buf);
    fclose(f);
    if (written != json_len) {
        (void)remove(tmp_path);
        return HU_ERR_IO;
    }
    if (rename(tmp_path, path) != 0) {
        (void)remove(tmp_path);
        return HU_ERR_IO;
    }
    return HU_OK;
fail:
    hu_json_buf_free(&buf);
    return HU_ERR_OUT_OF_MEMORY;
#endif
}

static hu_error_t task_list_load(hu_task_list_t *list) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)list;
    return HU_OK;
#else
    if (!list || !list->dir_path)
        return HU_OK;
    char path[HU_TASK_LIST_MAX_PATH];
    size_t dlen = strlen(list->dir_path);
    if (dlen + 20 >= sizeof(path))
        return HU_OK;
    if (dlen > 0 && list->dir_path[dlen - 1] == '/')
        snprintf(path, sizeof(path), "%s%s", list->dir_path, HU_TASK_LIST_JSON_FILENAME);
    else
        snprintf(path, sizeof(path), "%s/%s", list->dir_path, HU_TASK_LIST_JSON_FILENAME);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return HU_OK;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_OK;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return HU_OK;
    }
    char *content = (char *)list->alloc->alloc(list->alloc->ctx, (size_t)sz + 1);
    if (!content) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(content, 1, (size_t)sz, f);
    fclose(f);
    content[nr] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(list->alloc, content, nr, &root);
    list->alloc->free(list->alloc->ctx, content, (size_t)sz + 1);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY)
        return (root ? (void)hu_json_free(list->alloc, root), HU_OK : HU_OK);

    for (size_t i = 0; i < root->data.array.len && list->count < list->max_tasks; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        hu_task_t *t = &list->tasks[list->count];
        memset(t, 0, sizeof(*t));
        t->id = (uint64_t)hu_json_get_number(item, "id", 0);
        const char *subj = hu_json_get_string(item, "subject");
        const char *desc = hu_json_get_string(item, "description");
        t->owner_agent_id = (uint64_t)hu_json_get_number(item, "owner", 0);
        const char *status_str = hu_json_get_string(item, "status");
        t->status = status_from_string(status_str);
        t->created_at = (int64_t)hu_json_get_number(item, "created_at", 0);
        t->updated_at = (int64_t)hu_json_get_number(item, "updated_at", 0);

        if (subj) {
            size_t slen = strlen(subj);
            t->subject = hu_strndup(list->alloc, subj, slen);
            if (!t->subject)
                goto load_fail;
        }
        if (desc) {
            size_t desc_len = strlen(desc);
            t->description = hu_strndup(list->alloc, desc, desc_len);
            if (!t->description && desc_len > 0)
                goto load_fail;
        }
        hu_json_value_t *blocked = hu_json_object_get(item, "blocked_by");
        if (blocked && blocked->type == HU_JSON_ARRAY && blocked->data.array.len > 0) {
            size_t bc = blocked->data.array.len;
            t->blocked_by = (uint64_t *)list->alloc->alloc(list->alloc->ctx, bc * sizeof(uint64_t));
            if (!t->blocked_by)
                goto load_fail;
            for (size_t j = 0; j < bc; j++) {
                hu_json_value_t *v = blocked->data.array.items[j];
                if (v && v->type == HU_JSON_NUMBER)
                    t->blocked_by[j] = (uint64_t)v->data.number;
                else if (v && v->type == HU_JSON_STRING)
                    t->blocked_by[j] = (uint64_t)atoll(v->data.string.ptr);
            }
            t->blocked_by_count = bc;
        }
        if (t->id >= list->next_id)
            list->next_id = t->id + 1;
        list->count++;
    }
    hu_json_free(list->alloc, root);
    return HU_OK;
load_fail:
    for (size_t k = 0; k < list->count; k++)
        hu_task_free(list->alloc, &list->tasks[k]);
    list->count = 0;
    hu_json_free(list->alloc, root);
    return HU_OK;
#endif
}

hu_task_list_t *hu_task_list_create(hu_allocator_t *alloc, const char *dir_path, size_t max_tasks) {
    if (!alloc || max_tasks == 0)
        return NULL;
    hu_task_list_t *list = (hu_task_list_t *)alloc->alloc(alloc->ctx, sizeof(hu_task_list_t));
    if (!list)
        return NULL;
    memset(list, 0, sizeof(*list));
    list->alloc = alloc;
    list->max_tasks = max_tasks;
    list->tasks = (hu_task_t *)alloc->alloc(alloc->ctx, max_tasks * sizeof(hu_task_t));
    if (!list->tasks) {
        alloc->free(alloc->ctx, list, sizeof(*list));
        return NULL;
    }
    memset(list->tasks, 0, max_tasks * sizeof(hu_task_t));
    list->next_id = 1;

    if (dir_path && dir_path[0] != '\0') {
        size_t dlen = strlen(dir_path);
        list->dir_path = (char *)alloc->alloc(alloc->ctx, dlen + 1);
        if (!list->dir_path) {
            alloc->free(alloc->ctx, list->tasks, max_tasks * sizeof(hu_task_t));
            alloc->free(alloc->ctx, list, sizeof(*list));
            return NULL;
        }
        memcpy(list->dir_path, dir_path, dlen + 1);
        (void)task_list_load(list);
    }
    return list;
}

void hu_task_list_destroy(hu_task_list_t *list) {
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        hu_task_free(list->alloc, &list->tasks[i]);
    list->alloc->free(list->alloc->ctx, list->tasks, list->max_tasks * sizeof(hu_task_t));
    if (list->dir_path)
        list->alloc->free(list->alloc->ctx, list->dir_path, strlen(list->dir_path) + 1);
    list->alloc->free(list->alloc->ctx, list, sizeof(*list));
}

hu_error_t hu_task_list_add(hu_task_list_t *list, const char *subject, const char *description,
                            const uint64_t *blocked_by, size_t blocked_by_count, uint64_t *out_id) {
    if (!list || !subject || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (list->count >= list->max_tasks)
        return HU_ERR_OUT_OF_MEMORY;

    hu_task_t *t = &list->tasks[list->count];
    memset(t, 0, sizeof(*t));
    t->id = list->next_id++;
    t->subject = hu_strndup(list->alloc, subject, strlen(subject));
    if (!t->subject)
        return HU_ERR_OUT_OF_MEMORY;
    t->description = description ? hu_strndup(list->alloc, description, strlen(description)) : NULL;
    t->owner_agent_id = 0;
    t->status = HU_TASK_LIST_PENDING;
    t->created_at = (int64_t)time(NULL);
    t->updated_at = t->created_at;

    if (blocked_by && blocked_by_count > 0) {
        t->blocked_by =
            (uint64_t *)list->alloc->alloc(list->alloc->ctx, blocked_by_count * sizeof(uint64_t));
        if (!t->blocked_by) {
            list->alloc->free(list->alloc->ctx, t->subject, strlen(subject) + 1);
            if (t->description)
                list->alloc->free(list->alloc->ctx, t->description, strlen(description) + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(t->blocked_by, blocked_by, blocked_by_count * sizeof(uint64_t));
        t->blocked_by_count = blocked_by_count;
    }

    *out_id = t->id;
    list->count++;
    (void)task_list_save(list);
    return HU_OK;
}

bool hu_task_list_is_blocked(hu_task_list_t *list, uint64_t task_id) {
    if (!list)
        return true;
    hu_task_t *t = find_task(list, task_id);
    if (!t || !t->blocked_by || t->blocked_by_count == 0)
        return false;
    for (size_t i = 0; i < t->blocked_by_count; i++) {
        hu_task_t *dep = find_task(list, t->blocked_by[i]);
        if (!dep || dep->status != HU_TASK_LIST_COMPLETED)
            return true;
    }
    return false;
}

hu_error_t hu_task_list_claim(hu_task_list_t *list, uint64_t task_id, uint64_t agent_id) {
    if (!list)
        return HU_ERR_INVALID_ARGUMENT;
    hu_task_t *t = find_task(list, task_id);
    if (!t)
        return HU_ERR_NOT_FOUND;
    if (t->status != HU_TASK_LIST_PENDING)
        return HU_ERR_ALREADY_EXISTS;
    if (hu_task_list_is_blocked(list, task_id))
        return HU_ERR_INVALID_ARGUMENT;
    t->owner_agent_id = agent_id;
    t->status = HU_TASK_LIST_CLAIMED;
    t->updated_at = (int64_t)time(NULL);
    (void)task_list_save(list);
    return HU_OK;
}

hu_error_t hu_task_list_update_status(hu_task_list_t *list, uint64_t task_id,
                                      hu_task_list_status_t status) {
    if (!list)
        return HU_ERR_INVALID_ARGUMENT;
    hu_task_t *t = find_task(list, task_id);
    if (!t)
        return HU_ERR_NOT_FOUND;
    t->status = status;
    t->updated_at = (int64_t)time(NULL);
    (void)task_list_save(list);
    return HU_OK;
}

hu_error_t hu_task_list_next_available(hu_task_list_t *list, hu_task_t *out) {
    if (!list || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < list->count; i++) {
        hu_task_t *t = &list->tasks[i];
        if (t->status != HU_TASK_LIST_PENDING || t->owner_agent_id != 0)
            continue;
        if (hu_task_list_is_blocked(list, t->id))
            continue;
        *out = *t;
        if (out->subject)
            out->subject = hu_strndup(list->alloc, out->subject, strlen(out->subject));
        if (out->description)
            out->description = hu_strndup(list->alloc, out->description, strlen(out->description));
        if (out->blocked_by && out->blocked_by_count > 0) {
            uint64_t *cpy = (uint64_t *)list->alloc->alloc(list->alloc->ctx, out->blocked_by_count *
                                                                                 sizeof(uint64_t));
            if (cpy) {
                memcpy(cpy, t->blocked_by, out->blocked_by_count * sizeof(uint64_t));
                out->blocked_by = cpy;
            } else {
                out->blocked_by = NULL;
                out->blocked_by_count = 0;
            }
        }
        return HU_OK;
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_task_list_get(hu_task_list_t *list, uint64_t task_id, hu_task_t *out) {
    if (!list || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_task_t *t = find_task(list, task_id);
    if (!t) {
        memset(out, 0, sizeof(*out));
        return HU_ERR_NOT_FOUND;
    }
    *out = *t;
    if (out->subject)
        out->subject = hu_strndup(list->alloc, out->subject, strlen(out->subject));
    if (out->description)
        out->description = hu_strndup(list->alloc, out->description, strlen(out->description));
    if (out->blocked_by && out->blocked_by_count > 0) {
        uint64_t *cpy = (uint64_t *)list->alloc->alloc(list->alloc->ctx,
                                                       out->blocked_by_count * sizeof(uint64_t));
        if (cpy) {
            memcpy(cpy, t->blocked_by, out->blocked_by_count * sizeof(uint64_t));
            out->blocked_by = cpy;
        } else {
            out->blocked_by = NULL;
            out->blocked_by_count = 0;
        }
    }
    return HU_OK;
}

hu_error_t hu_task_list_all(hu_task_list_t *list, hu_task_t **out, size_t *out_count) {
    if (!list || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (list->count == 0)
        return HU_OK;
    hu_task_t *arr =
        (hu_task_t *)list->alloc->alloc(list->alloc->ctx, list->count * sizeof(hu_task_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    memset(arr, 0, list->count * sizeof(hu_task_t));
    for (size_t i = 0; i < list->count; i++) {
        arr[i] = list->tasks[i];
        if (arr[i].subject)
            arr[i].subject = hu_strndup(list->alloc, arr[i].subject, strlen(arr[i].subject));
        if (arr[i].description)
            arr[i].description =
                hu_strndup(list->alloc, arr[i].description, strlen(arr[i].description));
        if (arr[i].blocked_by && arr[i].blocked_by_count > 0) {
            uint64_t *cpy = (uint64_t *)list->alloc->alloc(
                list->alloc->ctx, arr[i].blocked_by_count * sizeof(uint64_t));
            if (cpy) {
                memcpy(cpy, list->tasks[i].blocked_by, arr[i].blocked_by_count * sizeof(uint64_t));
                arr[i].blocked_by = cpy;
            } else {
                arr[i].blocked_by = NULL;
                arr[i].blocked_by_count = 0;
            }
        }
    }
    *out = arr;
    *out_count = list->count;
    return HU_OK;
}

size_t hu_task_list_count_by_status(hu_task_list_t *list, hu_task_list_status_t status) {
    if (!list)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < list->count; i++)
        if (list->tasks[i].status == status)
            n++;
    return n;
}

bool hu_task_list_is_ready(hu_task_list_t *list, uint64_t task_id) {
    return list && !hu_task_list_is_blocked(list, task_id);
}

hu_error_t hu_task_list_query(hu_task_list_t *list, hu_task_list_status_t status, hu_task_t **out,
                              size_t *out_count) {
    if (!list || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    size_t n = hu_task_list_count_by_status(list, status);
    if (n == 0)
        return HU_OK;
    hu_task_t *arr = (hu_task_t *)list->alloc->alloc(list->alloc->ctx, n * sizeof(hu_task_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    memset(arr, 0, n * sizeof(hu_task_t));
    size_t j = 0;
    for (size_t i = 0; i < list->count && j < n; i++) {
        if (list->tasks[i].status != status)
            continue;
        arr[j] = list->tasks[i];
        if (arr[j].subject)
            arr[j].subject = hu_strndup(list->alloc, arr[j].subject, strlen(arr[j].subject));
        if (arr[j].description)
            arr[j].description =
                hu_strndup(list->alloc, arr[j].description, strlen(arr[j].description));
        if (arr[j].blocked_by && arr[j].blocked_by_count > 0) {
            uint64_t *cpy = (uint64_t *)list->alloc->alloc(
                list->alloc->ctx, arr[j].blocked_by_count * sizeof(uint64_t));
            if (cpy) {
                memcpy(cpy, list->tasks[i].blocked_by, arr[j].blocked_by_count * sizeof(uint64_t));
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
    return HU_OK;
}

void hu_task_array_free(hu_allocator_t *alloc, hu_task_t *tasks, size_t count) {
    if (!alloc || !tasks)
        return;
    for (size_t i = 0; i < count; i++)
        hu_task_free(alloc, &tasks[i]);
    alloc->free(alloc->ctx, tasks, count * sizeof(hu_task_t));
}

void hu_task_free(hu_allocator_t *alloc, hu_task_t *task) {
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
        alloc->free(alloc->ctx, task->blocked_by, task->blocked_by_count * sizeof(uint64_t));
        task->blocked_by = NULL;
        task->blocked_by_count = 0;
    }
}

#if defined(HU_IS_TEST) && HU_IS_TEST
hu_error_t hu_task_list_serialize(hu_task_list_t *list, char **out_json, size_t *out_len) {
    if (!list || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, list->alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (hu_json_buf_append_raw(&buf, "[", 1) != HU_OK) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < list->count; i++) {
        const hu_task_t *t = &list->tasks[i];
        if (i > 0 && hu_json_buf_append_raw(&buf, ",", 1) != HU_OK)
            goto ser_fail;
        char nbuf[64];
        int nlen =
            snprintf(nbuf, sizeof(nbuf), "{\"id\":%llu,\"subject\":", (unsigned long long)t->id);
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto ser_fail;
        hu_json_append_string(&buf, t->subject ? t->subject : "",
                              t->subject ? strlen(t->subject) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"description\":");
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto ser_fail;
        hu_json_append_string(&buf, t->description ? t->description : "",
                              t->description ? strlen(t->description) : 0);
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"owner\":%llu,\"status\":\"%s\"",
                        (unsigned long long)t->owner_agent_id, status_to_string(t->status));
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto ser_fail;
        nlen = snprintf(nbuf, sizeof(nbuf), ",\"blocked_by\":[");
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto ser_fail;
        for (size_t j = 0; j < t->blocked_by_count; j++) {
            if (j > 0 && hu_json_buf_append_raw(&buf, ",", 1) != HU_OK)
                goto ser_fail;
            nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)t->blocked_by[j]);
            if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
                goto ser_fail;
        }
        nlen = snprintf(nbuf, sizeof(nbuf), "],\"created_at\":%lld,\"updated_at\":%lld}",
                        (long long)t->created_at, (long long)t->updated_at);
        if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
            goto ser_fail;
    }
    if (hu_json_buf_append_raw(&buf, "]", 1) != HU_OK)
        goto ser_fail;

    *out_json = (char *)list->alloc->alloc(list->alloc->ctx, buf.len + 1);
    if (!*out_json) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(*out_json, buf.ptr, buf.len);
    (*out_json)[buf.len] = '\0';
    *out_len = buf.len;
    hu_json_buf_free(&buf);
    return HU_OK;
ser_fail:
    hu_json_buf_free(&buf);
    return HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_task_list_deserialize(hu_task_list_t *list, const char *json, size_t json_len) {
    if (!list || !json)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < list->count; i++)
        hu_task_free(list->alloc, &list->tasks[i]);
    list->count = 0;
    list->next_id = 1;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(list->alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY) {
        if (root)
            hu_json_free(list->alloc, root);
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;
    }

    for (size_t i = 0; i < root->data.array.len && list->count < list->max_tasks; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        hu_task_t *t = &list->tasks[list->count];
        memset(t, 0, sizeof(*t));
        t->id = (uint64_t)hu_json_get_number(item, "id", 0);
        const char *subj = hu_json_get_string(item, "subject");
        const char *desc = hu_json_get_string(item, "description");
        t->owner_agent_id = (uint64_t)hu_json_get_number(item, "owner", 0);
        const char *status_str = hu_json_get_string(item, "status");
        t->status = status_from_string(status_str);
        t->created_at = (int64_t)hu_json_get_number(item, "created_at", 0);
        t->updated_at = (int64_t)hu_json_get_number(item, "updated_at", 0);

        if (subj) {
            size_t slen = strlen(subj);
            t->subject = hu_strndup(list->alloc, subj, slen);
            if (!t->subject)
                goto des_fail;
        }
        if (desc) {
            size_t dlen = strlen(desc);
            t->description = hu_strndup(list->alloc, desc, dlen);
            if (!t->description && dlen > 0)
                goto des_fail;
        }
        hu_json_value_t *blocked = hu_json_object_get(item, "blocked_by");
        if (blocked && blocked->type == HU_JSON_ARRAY && blocked->data.array.len > 0) {
            size_t bc = blocked->data.array.len;
            t->blocked_by = (uint64_t *)list->alloc->alloc(list->alloc->ctx, bc * sizeof(uint64_t));
            if (!t->blocked_by)
                goto des_fail;
            for (size_t j = 0; j < bc; j++) {
                hu_json_value_t *v = blocked->data.array.items[j];
                if (v && v->type == HU_JSON_NUMBER)
                    t->blocked_by[j] = (uint64_t)v->data.number;
                else if (v && v->type == HU_JSON_STRING)
                    t->blocked_by[j] = (uint64_t)atoll(v->data.string.ptr);
            }
            t->blocked_by_count = bc;
        }
        if (t->id >= list->next_id)
            list->next_id = t->id + 1;
        list->count++;
    }
    hu_json_free(list->alloc, root);
    return HU_OK;
des_fail:
    for (size_t k = 0; k < list->count; k++)
        hu_task_free(list->alloc, &list->tasks[k]);
    list->count = 0;
    hu_json_free(list->alloc, root);
    return HU_ERR_OUT_OF_MEMORY;
}
#endif
