/* Commitment store — persist and query commitments via memory backend */
#include "seaclaw/agent/commitment.h"
#include "seaclaw/agent/commitment_store.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_COMMITMENT_KEY_PREFIX "commitment:"
#define SC_COMMITMENT_CATEGORY   "commitment"

struct sc_commitment_store {
    sc_allocator_t *alloc;
    sc_memory_t *memory;
};

static const char *type_to_string(sc_commitment_type_t t) {
    switch (t) {
    case SC_COMMITMENT_PROMISE:
        return "promise";
    case SC_COMMITMENT_INTENTION:
        return "intention";
    case SC_COMMITMENT_REMINDER:
        return "reminder";
    case SC_COMMITMENT_GOAL:
        return "goal";
    }
    return "promise";
}

static const char *status_to_string(sc_commitment_status_t s) {
    switch (s) {
    case SC_COMMITMENT_ACTIVE:
        return "active";
    case SC_COMMITMENT_COMPLETED:
        return "completed";
    case SC_COMMITMENT_EXPIRED:
        return "expired";
    case SC_COMMITMENT_CANCELLED:
        return "cancelled";
    }
    return "active";
}

static sc_commitment_type_t string_to_type(const char *s, size_t len) {
    if (len == 7 && memcmp(s, "promise", 7) == 0)
        return SC_COMMITMENT_PROMISE;
    if (len == 9 && memcmp(s, "intention", 9) == 0)
        return SC_COMMITMENT_INTENTION;
    if (len == 8 && memcmp(s, "reminder", 8) == 0)
        return SC_COMMITMENT_REMINDER;
    if (len == 4 && memcmp(s, "goal", 4) == 0)
        return SC_COMMITMENT_GOAL;
    return SC_COMMITMENT_PROMISE;
}

static bool string_is_active(const char *s, size_t len) {
    return len == 6 && memcmp(s, "active", 6) == 0;
}

sc_error_t sc_commitment_store_create(sc_allocator_t *alloc, sc_memory_t *memory,
                                      sc_commitment_store_t **out) {
    if (!alloc || !memory || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_commitment_store_t *store =
        (sc_commitment_store_t *)alloc->alloc(alloc->ctx, sizeof(sc_commitment_store_t));
    if (!store)
        return SC_ERR_OUT_OF_MEMORY;
    store->alloc = alloc;
    store->memory = memory;
    *out = store;
    return SC_OK;
}

sc_error_t sc_commitment_store_save(sc_commitment_store_t *store,
                                    const sc_commitment_t *commitment,
                                    const char *session_id, size_t session_id_len) {
    if (!store || !commitment || !store->memory->vtable)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *obj = sc_json_object_new(store->alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_object_set(
        store->alloc, obj, "statement",
        sc_json_string_new(store->alloc, commitment->statement ? commitment->statement : "",
                          commitment->statement_len));
    sc_json_object_set(
        store->alloc, obj, "summary",
        sc_json_string_new(store->alloc, commitment->summary ? commitment->summary : "",
                          commitment->summary_len));
    sc_json_object_set(store->alloc, obj, "type",
                       sc_json_string_new(store->alloc, type_to_string(commitment->type),
                                         strlen(type_to_string(commitment->type))));
    sc_json_object_set(store->alloc, obj, "status",
                       sc_json_string_new(store->alloc, status_to_string(commitment->status),
                                         strlen(status_to_string(commitment->status))));
    sc_json_object_set(
        store->alloc, obj, "owner",
        sc_json_string_new(store->alloc, commitment->owner ? commitment->owner : "user",
                          commitment->owner ? strlen(commitment->owner) : 4));
    sc_json_object_set(
        store->alloc, obj, "created_at",
        sc_json_string_new(store->alloc, commitment->created_at ? commitment->created_at : "",
                          commitment->created_at ? strlen(commitment->created_at) : 0));
    double ew = commitment->emotional_weight
                    ? strtod(commitment->emotional_weight, NULL)
                    : 0.5;
    sc_json_object_set(store->alloc, obj, "emotional_weight",
                       sc_json_number_new(store->alloc, ew));

    char *content = NULL;
    size_t content_len = 0;
    sc_error_t err = sc_json_stringify(store->alloc, obj, &content, &content_len);
    sc_json_free(store->alloc, obj);
    if (err != SC_OK || !content)
        return err;

    char key_buf[256];
    size_t key_len = (size_t)snprintf(key_buf, sizeof(key_buf), "%s%s", SC_COMMITMENT_KEY_PREFIX,
                                      commitment->id ? commitment->id : "unknown");
    if (key_len >= sizeof(key_buf))
        key_len = sizeof(key_buf) - 1;

    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = SC_COMMITMENT_CATEGORY, .name_len = strlen(SC_COMMITMENT_CATEGORY)},
    };

    err = store->memory->vtable->store(store->memory->ctx, key_buf, key_len, content, content_len,
                                       &cat, session_id, session_id_len);
    store->alloc->free(store->alloc->ctx, content, content_len + 1);
    return err;
}

sc_error_t sc_commitment_store_list_active(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                           const char *session_id, size_t session_id_len,
                                           sc_commitment_t **out, size_t *out_count) {
    if (!store || !alloc || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = SC_COMMITMENT_CATEGORY, .name_len = strlen(SC_COMMITMENT_CATEGORY)},
    };

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = store->memory->vtable->list(store->memory->ctx, alloc, &cat, session_id,
                                                  session_id_len, &entries, &count);
    if (err != SC_OK)
        return err;
    if (!entries || count == 0)
        return SC_OK;

    sc_commitment_t *active = NULL;
    size_t active_count = 0;
    size_t cap = count;

    active = (sc_commitment_t *)alloc->alloc(alloc->ctx, cap * sizeof(sc_commitment_t));
    if (!active) {
        for (size_t j = 0; j < count; j++)
            sc_memory_entry_free_fields(alloc, &entries[j]);
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(active, 0, cap * sizeof(sc_commitment_t));

    for (size_t i = 0; i < count; i++) {
        sc_memory_entry_t *e = &entries[i];
        if (!e->content || e->content_len == 0)
            continue;

        sc_json_value_t *parsed = NULL;
        err = sc_json_parse(alloc, e->content, e->content_len, &parsed);
        if (err != SC_OK || !parsed || parsed->type != SC_JSON_OBJECT)
            continue;

        const char *status = sc_json_get_string(parsed, "status");
        if (!status || !string_is_active(status, strlen(status))) {
            sc_json_free(alloc, parsed);
            continue;
        }

        sc_commitment_t *c = &active[active_count];
        const char *stmt = sc_json_get_string(parsed, "statement");
        const char *sum = sc_json_get_string(parsed, "summary");
        const char *typ = sc_json_get_string(parsed, "type");
        const char *own = sc_json_get_string(parsed, "owner");
        const char *created = sc_json_get_string(parsed, "created_at");

        if (e->key && e->key_len > strlen(SC_COMMITMENT_KEY_PREFIX)) {
            c->id = sc_strndup(alloc, e->key + strlen(SC_COMMITMENT_KEY_PREFIX),
                               e->key_len - strlen(SC_COMMITMENT_KEY_PREFIX));
        }
        c->statement = stmt ? sc_strdup(alloc, stmt) : NULL;
        c->statement_len = c->statement ? strlen(c->statement) : 0;
        c->summary = sum ? sc_strdup(alloc, sum) : NULL;
        c->summary_len = c->summary ? strlen(c->summary) : 0;
        c->type = typ ? string_to_type(typ, strlen(typ)) : SC_COMMITMENT_PROMISE;
        c->status = SC_COMMITMENT_ACTIVE;
        c->created_at = created ? sc_strdup(alloc, created) : NULL;
        c->owner = own ? sc_strdup(alloc, own) : NULL;

        if (c->id && (c->statement || c->summary)) {
            active_count++;
        } else {
            sc_commitment_deinit(c, alloc);
        }
        sc_json_free(alloc, parsed);
    }

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));

    *out = active;
    *out_count = active_count;
    return SC_OK;
}

sc_error_t sc_commitment_store_build_context(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                              const char *session_id, size_t session_id_len,
                                              char **out, size_t *out_len) {
    if (!store || !alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    sc_commitment_t *active = NULL;
    size_t count = 0;
    sc_error_t err = sc_commitment_store_list_active(store, alloc, session_id, session_id_len,
                                                      &active, &count);
    if (err != SC_OK || count == 0 || !active)
        return err;

    size_t cap = 512;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        for (size_t i = 0; i < count; i++)
            sc_commitment_deinit(&active[i], alloc);
        alloc->free(alloc->ctx, active, count * sizeof(sc_commitment_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    buf[0] = '\0';

    int n = snprintf(buf, cap, "### Active Commitments\n\n");
    if (n > 0)
        len += (size_t)n;

    for (size_t i = 0; i < count; i++) {
        sc_commitment_t *c = &active[i];
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
                    sc_commitment_deinit(&active[j], alloc);
                alloc->free(alloc->ctx, active, count * sizeof(sc_commitment_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            buf = nb;
            cap = new_cap;
        }
        memcpy(buf + len, line, (size_t)ln);
        len += (size_t)ln;
        buf[len] = '\0';
    }

    for (size_t i = 0; i < count; i++)
        sc_commitment_deinit(&active[i], alloc);
    alloc->free(alloc->ctx, active, count * sizeof(sc_commitment_t));

    *out = buf;
    *out_len = len;
    return SC_OK;
}

void sc_commitment_store_destroy(sc_commitment_store_t *store) {
    if (!store)
        return;
    store->alloc->free(store->alloc->ctx, store, sizeof(sc_commitment_store_t));
}
