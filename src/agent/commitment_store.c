/* Commitment store — persist and query commitments via memory backend */
#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_COMMITMENT_KEY_PREFIX "commitment:"
#define HU_COMMITMENT_CATEGORY   "commitment"

struct hu_commitment_store {
    hu_allocator_t *alloc;
    hu_memory_t *memory;
};

static const char *type_to_string(hu_commitment_type_t t) {
    switch (t) {
    case HU_COMMITMENT_PROMISE:
        return "promise";
    case HU_COMMITMENT_INTENTION:
        return "intention";
    case HU_COMMITMENT_REMINDER:
        return "reminder";
    case HU_COMMITMENT_GOAL:
        return "goal";
    }
    return "promise";
}

static const char *status_to_string(hu_commitment_status_t s) {
    switch (s) {
    case HU_COMMITMENT_ACTIVE:
        return "active";
    case HU_COMMITMENT_COMPLETED:
        return "completed";
    case HU_COMMITMENT_EXPIRED:
        return "expired";
    case HU_COMMITMENT_CANCELLED:
        return "cancelled";
    }
    return "active";
}

static hu_commitment_type_t string_to_type(const char *s, size_t len) {
    if (len == 7 && memcmp(s, "promise", 7) == 0)
        return HU_COMMITMENT_PROMISE;
    if (len == 9 && memcmp(s, "intention", 9) == 0)
        return HU_COMMITMENT_INTENTION;
    if (len == 8 && memcmp(s, "reminder", 8) == 0)
        return HU_COMMITMENT_REMINDER;
    if (len == 4 && memcmp(s, "goal", 4) == 0)
        return HU_COMMITMENT_GOAL;
    return HU_COMMITMENT_PROMISE;
}

static bool string_is_active(const char *s, size_t len) {
    return len == 6 && memcmp(s, "active", 6) == 0;
}

hu_error_t hu_commitment_store_create(hu_allocator_t *alloc, hu_memory_t *memory,
                                      hu_commitment_store_t **out) {
    if (!alloc || !memory || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_commitment_store_t *store =
        (hu_commitment_store_t *)alloc->alloc(alloc->ctx, sizeof(hu_commitment_store_t));
    if (!store)
        return HU_ERR_OUT_OF_MEMORY;
    store->alloc = alloc;
    store->memory = memory;
    *out = store;
    return HU_OK;
}

hu_error_t hu_commitment_store_save(hu_commitment_store_t *store,
                                    const hu_commitment_t *commitment,
                                    const char *session_id, size_t session_id_len) {
    if (!store || !commitment || !store->memory || !store->memory->vtable)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *obj = hu_json_object_new(store->alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_object_set(
        store->alloc, obj, "statement",
        hu_json_string_new(store->alloc, commitment->statement ? commitment->statement : "",
                          commitment->statement_len));
    hu_json_object_set(
        store->alloc, obj, "summary",
        hu_json_string_new(store->alloc, commitment->summary ? commitment->summary : "",
                          commitment->summary_len));
    hu_json_object_set(store->alloc, obj, "type",
                       hu_json_string_new(store->alloc, type_to_string(commitment->type),
                                         strlen(type_to_string(commitment->type))));
    hu_json_object_set(store->alloc, obj, "status",
                       hu_json_string_new(store->alloc, status_to_string(commitment->status),
                                         strlen(status_to_string(commitment->status))));
    hu_json_object_set(
        store->alloc, obj, "owner",
        hu_json_string_new(store->alloc, commitment->owner ? commitment->owner : "user",
                          commitment->owner ? strlen(commitment->owner) : 4));
    hu_json_object_set(
        store->alloc, obj, "created_at",
        hu_json_string_new(store->alloc, commitment->created_at ? commitment->created_at : "",
                          commitment->created_at ? strlen(commitment->created_at) : 0));
    double ew = commitment->emotional_weight
                    ? strtod(commitment->emotional_weight, NULL)
                    : 0.5;
    hu_json_object_set(store->alloc, obj, "emotional_weight",
                       hu_json_number_new(store->alloc, ew));

    char *content = NULL;
    size_t content_len = 0;
    hu_error_t err = hu_json_stringify(store->alloc, obj, &content, &content_len);
    hu_json_free(store->alloc, obj);
    if (err != HU_OK || !content)
        return err;

    char key_buf[256];
    size_t key_len = (size_t)snprintf(key_buf, sizeof(key_buf), "%s%s", HU_COMMITMENT_KEY_PREFIX,
                                      commitment->id ? commitment->id : "unknown");
    if (key_len >= sizeof(key_buf))
        key_len = sizeof(key_buf) - 1;

    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = HU_COMMITMENT_CATEGORY, .name_len = strlen(HU_COMMITMENT_CATEGORY)},
    };

    err = store->memory->vtable->store(store->memory->ctx, key_buf, key_len, content, content_len,
                                       &cat, session_id, session_id_len);
    store->alloc->free(store->alloc->ctx, content, content_len + 1);
    return err;
}

hu_error_t hu_commitment_store_list_active(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                           const char *session_id, size_t session_id_len,
                                           hu_commitment_t **out, size_t *out_count) {
    if (!store || !alloc || !out || !out_count || !store->memory || !store->memory->vtable)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = HU_COMMITMENT_CATEGORY, .name_len = strlen(HU_COMMITMENT_CATEGORY)},
    };

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = store->memory->vtable->list(store->memory->ctx, alloc, &cat, session_id,
                                                  session_id_len, &entries, &count);
    if (err != HU_OK)
        return err;
    if (!entries || count == 0)
        return HU_OK;

    hu_commitment_t *active = NULL;
    size_t active_count = 0;
    size_t cap = count;

    active = (hu_commitment_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_commitment_t));
    if (!active) {
        for (size_t j = 0; j < count; j++)
            hu_memory_entry_free_fields(alloc, &entries[j]);
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(active, 0, cap * sizeof(hu_commitment_t));

    for (size_t i = 0; i < count; i++) {
        hu_memory_entry_t *e = &entries[i];
        if (!e->content || e->content_len == 0)
            continue;

        hu_json_value_t *parsed = NULL;
        err = hu_json_parse(alloc, e->content, e->content_len, &parsed);
        if (err != HU_OK || !parsed || parsed->type != HU_JSON_OBJECT)
            continue;

        const char *status = hu_json_get_string(parsed, "status");
        if (!status || !string_is_active(status, strlen(status))) {
            hu_json_free(alloc, parsed);
            continue;
        }

        hu_commitment_t *c = &active[active_count];
        const char *stmt = hu_json_get_string(parsed, "statement");
        const char *sum = hu_json_get_string(parsed, "summary");
        const char *typ = hu_json_get_string(parsed, "type");
        const char *own = hu_json_get_string(parsed, "owner");
        const char *created = hu_json_get_string(parsed, "created_at");

        if (e->key && e->key_len > strlen(HU_COMMITMENT_KEY_PREFIX)) {
            c->id = hu_strndup(alloc, e->key + strlen(HU_COMMITMENT_KEY_PREFIX),
                               e->key_len - strlen(HU_COMMITMENT_KEY_PREFIX));
        }
        c->statement = stmt ? hu_strdup(alloc, stmt) : NULL;
        c->statement_len = c->statement ? strlen(c->statement) : 0;
        c->summary = sum ? hu_strdup(alloc, sum) : NULL;
        c->summary_len = c->summary ? strlen(c->summary) : 0;
        c->type = typ ? string_to_type(typ, strlen(typ)) : HU_COMMITMENT_PROMISE;
        c->status = HU_COMMITMENT_ACTIVE;
        c->created_at = created ? hu_strdup(alloc, created) : NULL;
        c->owner = own ? hu_strdup(alloc, own) : NULL;

        if (c->id && (c->statement || c->summary)) {
            active_count++;
        } else {
            hu_commitment_deinit(c, alloc);
        }
        hu_json_free(alloc, parsed);
    }

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

    *out = active;
    *out_count = active_count;
    return HU_OK;
}

hu_error_t hu_commitment_store_build_context(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                              const char *session_id, size_t session_id_len,
                                              char **out, size_t *out_len) {
    if (!store || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    hu_commitment_t *active = NULL;
    size_t count = 0;
    hu_error_t err = hu_commitment_store_list_active(store, alloc, session_id, session_id_len,
                                                      &active, &count);
    if (err != HU_OK || count == 0 || !active)
        return err;

    size_t cap = 512;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        for (size_t i = 0; i < count; i++)
            hu_commitment_deinit(&active[i], alloc);
        alloc->free(alloc->ctx, active, count * sizeof(hu_commitment_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    buf[0] = '\0';

    int n = snprintf(buf, cap, "### Active Commitments\n\n");
    if (n > 0)
        len += (size_t)n;

    for (size_t i = 0; i < count; i++) {
        hu_commitment_t *c = &active[i];
        const char *sum = c->summary ? c->summary : "(no summary)";
        const char *own = c->owner ? c->owner : "unknown";
        const char *created = c->created_at ? c->created_at : "";

        char line[512];
        int ln = snprintf(line, sizeof(line), "- %s (by %s, %s)\n", sum, own, created);
        if (ln <= 0)
            continue;
        while (len + (size_t)ln + 1 > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, buf, cap);
                for (size_t j = 0; j < count; j++)
                    hu_commitment_deinit(&active[j], alloc);
                alloc->free(alloc->ctx, active, count * sizeof(hu_commitment_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf = nb;
            cap = new_cap;
        }
        memcpy(buf + len, line, (size_t)ln);
        len += (size_t)ln;
        buf[len] = '\0';
    }

    for (size_t i = 0; i < count; i++)
        hu_commitment_deinit(&active[i], alloc);
    alloc->free(alloc->ctx, active, count * sizeof(hu_commitment_t));

    *out = buf;
    *out_len = len;
    return HU_OK;
}

void hu_commitment_store_destroy(hu_commitment_store_t *store) {
    if (!store)
        return;
    store->alloc->free(store->alloc->ctx, store, sizeof(hu_commitment_store_t));
}
