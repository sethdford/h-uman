#include "seaclaw/agent/task_list.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sc_task_list {
    sc_allocator_t *alloc;
    sc_task_t *tasks;
    size_t count;
    size_t max_tasks;
    uint64_t next_id;
};

static sc_task_t *find_task(sc_task_list_t *list, uint64_t task_id) {
    for (size_t i = 0; i < list->count; i++)
        if (list->tasks[i].id == task_id)
            return &list->tasks[i];
    return NULL;
}

sc_task_list_t *sc_task_list_create(sc_allocator_t *alloc, size_t max_tasks) {
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
    return list;
}

void sc_task_list_destroy(sc_task_list_t *list) {
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        sc_task_free(list->alloc, &list->tasks[i]);
    list->alloc->free(list->alloc->ctx, list->tasks, list->max_tasks * sizeof(sc_task_t));
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
