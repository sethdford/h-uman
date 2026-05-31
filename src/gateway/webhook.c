#include "human/webhook.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_WEBHOOKS 100
#define MAX_EVENTS_PER_WEBHOOK 1000

typedef struct {
    hu_webhook_t webhook;
    hu_webhook_event_t *events;
    size_t event_count;
    size_t event_cap;
} webhook_entry_t;

struct hu_webhook_manager {
    webhook_entry_t entries[MAX_WEBHOOKS];
    size_t count;
    hu_allocator_t *alloc;
};

static char *gen_webhook_id(hu_allocator_t *alloc) {
    static unsigned long counter = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "webhook_%lu_%ld", counter++, time(NULL));
    return hu_strdup(alloc, buf);
}

hu_error_t hu_webhook_manager_create(hu_allocator_t *alloc, hu_webhook_manager_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_webhook_manager_t *mgr = (hu_webhook_manager_t *)alloc->alloc(alloc->ctx, sizeof(*mgr));
    if (!mgr)
        return HU_ERR_OUT_OF_MEMORY;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    *out = mgr;
    return HU_OK;
}

void hu_webhook_manager_destroy(hu_allocator_t *alloc, hu_webhook_manager_t *mgr) {
    if (!mgr)
        return;
    for (size_t i = 0; i < mgr->count; i++) {
        webhook_entry_t *e = &mgr->entries[i];
        if (e->webhook.id)
            alloc->free(alloc->ctx, e->webhook.id, strlen(e->webhook.id) + 1);
        if (e->webhook.path)
            alloc->free(alloc->ctx, e->webhook.path, strlen(e->webhook.path) + 1);
        for (size_t j = 0; j < e->event_count; j++) {
            if (e->events[j].webhook_id)
                alloc->free(alloc->ctx, e->events[j].webhook_id, strlen(e->events[j].webhook_id) + 1);
            if (e->events[j].event_data)
                alloc->free(alloc->ctx, e->events[j].event_data, strlen(e->events[j].event_data) + 1);
        }
        if (e->events)
            alloc->free(alloc->ctx, e->events, e->event_cap * sizeof(hu_webhook_event_t));
    }
    alloc->free(alloc->ctx, mgr, sizeof(*mgr));
}

hu_error_t hu_webhook_register(hu_allocator_t *alloc, hu_webhook_manager_t *mgr, const char *path,
                               char **out_id) {
    if (!alloc || !mgr || !path || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (mgr->count >= MAX_WEBHOOKS)
        return HU_ERR_OUT_OF_MEMORY;

    char *id = gen_webhook_id(alloc);
    if (!id)
        return HU_ERR_OUT_OF_MEMORY;

    char *path_dup = hu_strdup(alloc, path);
    if (!path_dup) {
        alloc->free(alloc->ctx, id, strlen(id) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    webhook_entry_t *e = &mgr->entries[mgr->count];
    e->webhook.id = id;
    e->webhook.path = path_dup;
    e->webhook.registered_at = time(NULL);
    e->event_cap = 16;
    e->events = (hu_webhook_event_t *)alloc->alloc(alloc->ctx, e->event_cap * sizeof(hu_webhook_event_t));
    if (!e->events) {
        alloc->free(alloc->ctx, id, strlen(id) + 1);
        alloc->free(alloc->ctx, path_dup, strlen(path_dup) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(e->events, 0, e->event_cap * sizeof(hu_webhook_event_t));
    e->event_count = 0;

    mgr->count++;
    *out_id = id;
    return HU_OK;
}

hu_error_t hu_webhook_unregister(hu_allocator_t *alloc, hu_webhook_manager_t *mgr,
                                 const char *webhook_id) {
    if (!mgr || !webhook_id)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].webhook.id, webhook_id) == 0) {
            webhook_entry_t *e = &mgr->entries[i];
            if (e->webhook.id)
                alloc->free(alloc->ctx, e->webhook.id, strlen(e->webhook.id) + 1);
            if (e->webhook.path)
                alloc->free(alloc->ctx, e->webhook.path, strlen(e->webhook.path) + 1);
            for (size_t j = 0; j < e->event_count; j++) {
                if (e->events[j].webhook_id)
                    alloc->free(alloc->ctx, e->events[j].webhook_id, strlen(e->events[j].webhook_id) + 1);
                if (e->events[j].event_data)
                    alloc->free(alloc->ctx, e->events[j].event_data, strlen(e->events[j].event_data) + 1);
            }
            if (e->events)
                alloc->free(alloc->ctx, e->events, e->event_cap * sizeof(hu_webhook_event_t));
            if (i + 1 < mgr->count)
                memmove(e, e + 1, (mgr->count - i - 1) * sizeof(webhook_entry_t));
            mgr->count--;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_webhook_receive_event(hu_allocator_t *alloc, hu_webhook_manager_t *mgr,
                                    const char *webhook_id, const char *event_data) {
    if (!mgr || !webhook_id || !event_data)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].webhook.id, webhook_id) == 0) {
            webhook_entry_t *e = &mgr->entries[i];
            if (e->event_count >= MAX_EVENTS_PER_WEBHOOK)
                return HU_ERR_OUT_OF_MEMORY;

            if (e->event_count >= e->event_cap) {
                size_t new_cap = e->event_cap * 2;
                hu_webhook_event_t *new_events =
                    (hu_webhook_event_t *)alloc->realloc(alloc->ctx, e->events,
                                                         e->event_cap * sizeof(hu_webhook_event_t),
                                                         new_cap * sizeof(hu_webhook_event_t));
                if (!new_events)
                    return HU_ERR_OUT_OF_MEMORY;
                e->events = new_events;
                e->event_cap = new_cap;
            }

            hu_webhook_event_t *evt = &e->events[e->event_count];
            evt->webhook_id = hu_strdup(alloc, webhook_id);
            evt->event_data = hu_strdup(alloc, event_data);
            evt->received_at = time(NULL);
            if (!evt->webhook_id || !evt->event_data) {
                if (evt->webhook_id)
                    alloc->free(alloc->ctx, evt->webhook_id, strlen(evt->webhook_id) + 1);
                if (evt->event_data)
                    alloc->free(alloc->ctx, evt->event_data, strlen(evt->event_data) + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            e->event_count++;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_webhook_poll(hu_allocator_t *alloc, hu_webhook_manager_t *mgr,
                           const char *webhook_id, hu_webhook_event_t **out_events,
                           size_t *out_count) {
    if (!mgr || !webhook_id || !out_events || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].webhook.id, webhook_id) == 0) {
            webhook_entry_t *e = &mgr->entries[i];
            if (e->event_count == 0) {
                *out_events = NULL;
                *out_count = 0;
                return HU_OK;
            }

            hu_webhook_event_t *result =
                (hu_webhook_event_t *)alloc->alloc(alloc->ctx, e->event_count * sizeof(hu_webhook_event_t));
            if (!result)
                return HU_ERR_OUT_OF_MEMORY;

            memcpy(result, e->events, e->event_count * sizeof(hu_webhook_event_t));
            *out_events = result;
            *out_count = e->event_count;

            e->event_count = 0;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_webhook_list(hu_allocator_t *alloc, hu_webhook_manager_t *mgr,
                           hu_webhook_t **out_webhooks, size_t *out_count) {
    if (!mgr || !out_webhooks || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    if (mgr->count == 0) {
        *out_webhooks = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_webhook_t *result = (hu_webhook_t *)alloc->alloc(alloc->ctx, mgr->count * sizeof(hu_webhook_t));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < mgr->count; i++) {
        result[i].id = hu_strdup(alloc, mgr->entries[i].webhook.id);
        result[i].path = hu_strdup(alloc, mgr->entries[i].webhook.path);
        result[i].registered_at = mgr->entries[i].webhook.registered_at;
        if (!result[i].id || !result[i].path) {
            for (size_t j = 0; j <= i; j++) {
                if (result[j].id)
                    alloc->free(alloc->ctx, result[j].id, strlen(result[j].id) + 1);
                if (result[j].path)
                    alloc->free(alloc->ctx, result[j].path, strlen(result[j].path) + 1);
            }
            alloc->free(alloc->ctx, result, mgr->count * sizeof(hu_webhook_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    *out_webhooks = result;
    *out_count = mgr->count;
    return HU_OK;
}

void hu_webhook_free_webhooks(hu_allocator_t *alloc, hu_webhook_t *webhooks, size_t count) {
    if (!webhooks)
        return;
    for (size_t i = 0; i < count; i++) {
        if (webhooks[i].id)
            alloc->free(alloc->ctx, webhooks[i].id, strlen(webhooks[i].id) + 1);
        if (webhooks[i].path)
            alloc->free(alloc->ctx, webhooks[i].path, strlen(webhooks[i].path) + 1);
    }
    alloc->free(alloc->ctx, webhooks, count * sizeof(hu_webhook_t));
}

void hu_webhook_free_events(hu_allocator_t *alloc, hu_webhook_event_t *events, size_t count) {
    if (!events)
        return;
    for (size_t i = 0; i < count; i++) {
        if (events[i].webhook_id)
            alloc->free(alloc->ctx, events[i].webhook_id, strlen(events[i].webhook_id) + 1);
        if (events[i].event_data)
            alloc->free(alloc->ctx, events[i].event_data, strlen(events[i].event_data) + 1);
    }
    alloc->free(alloc->ctx, events, count * sizeof(hu_webhook_event_t));
}
