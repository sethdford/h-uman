/* Phase F2 (2026-05-18) — C-side mirror of contact-routing storage.
 *
 * See include/human/ml/m3_contact_routes.h for the why. This file
 * implements:
 *   - JSON load (parses keys/values into a flat vector of routes)
 *   - lookup_or_default()
 *   - reload() (calls load() over the existing struct)
 *
 * Storage shape mirrors the Python writer
 * (scripts/m3_contact_routing.py):
 *   {
 *     "routes": { "<contact_id_hash>": {"adapter_path": "...", ...} },
 *     "default_adapter": "/path/..."
 *   }
 *
 * We only read the fields the C side cares about (adapter_path);
 * other Python-only fields (contact_label, promoted_at_ms, etc.)
 * are preserved on disk but ignored here.
 */

#include "human/ml/m3_contact_routes.h"

#include "human/core/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct route_entry {
    uint64_t contact_id_hash;
    char *adapter_path; /* owned */
    size_t adapter_path_len;
} route_entry_t;

struct hu_m3_contact_routes {
    hu_allocator_t *alloc;
    char *path; /* owned */
    route_entry_t *entries;
    size_t entry_count;
    size_t entry_cap;
    char *default_adapter; /* owned, may be NULL */
};

static void clear_entries(hu_m3_contact_routes_t *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->entry_count; i++) {
        if (r->entries[i].adapter_path) {
            r->alloc->free(r->alloc->ctx, r->entries[i].adapter_path,
                           r->entries[i].adapter_path_len + 1);
        }
    }
    if (r->entries) {
        r->alloc->free(r->alloc->ctx, r->entries, r->entry_cap * sizeof(route_entry_t));
        r->entries = NULL;
    }
    r->entry_count = 0;
    r->entry_cap = 0;
    if (r->default_adapter) {
        r->alloc->free(r->alloc->ctx, r->default_adapter, strlen(r->default_adapter) + 1);
        r->default_adapter = NULL;
    }
}

static char *strdup_alloc(hu_allocator_t *alloc, const char *src, size_t len) {
    char *dst = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!dst)
        return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

static hu_error_t reserve_entries(hu_m3_contact_routes_t *r, size_t need) {
    if (need <= r->entry_cap)
        return HU_OK;
    size_t new_cap = r->entry_cap ? r->entry_cap * 2 : 8;
    while (new_cap < need)
        new_cap *= 2;
    route_entry_t *new_arr =
        (route_entry_t *)r->alloc->alloc(r->alloc->ctx, new_cap * sizeof(route_entry_t));
    if (!new_arr)
        return HU_ERR_OUT_OF_MEMORY;
    if (r->entries) {
        memcpy(new_arr, r->entries, r->entry_count * sizeof(route_entry_t));
        r->alloc->free(r->alloc->ctx, r->entries, r->entry_cap * sizeof(route_entry_t));
    }
    r->entries = new_arr;
    r->entry_cap = new_cap;
    return HU_OK;
}

static hu_error_t load_from_disk(hu_m3_contact_routes_t *r) {
    clear_entries(r);
    FILE *fp = fopen(r->path, "rb");
    if (!fp)
        return HU_OK; /* missing file → empty routes, lookup returns NULL */

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > (1 << 22)) { /* 4 MB sanity cap */
        fclose(fp);
        return HU_OK;
    }
    rewind(fp);

    char *body = (char *)r->alloc->alloc(r->alloc->ctx, (size_t)sz + 1);
    if (!body) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(body, 1, (size_t)sz, fp);
    fclose(fp);
    body[got] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(r->alloc, body, got, &root);
    r->alloc->free(r->alloc->ctx, body, (size_t)sz + 1);
    if (err != HU_OK) {
        /* Malformed JSON → log + treat as empty. The chat path MUST
         * continue. Same stance as Python's load_routes(). */
        return HU_OK;
    }
    if (!root || root->type != HU_JSON_OBJECT) {
        hu_json_free(r->alloc, root);
        return HU_OK;
    }

    /* default_adapter (top-level) */
    const hu_json_value_t *dflt = hu_json_object_get(root, "default_adapter");
    if (dflt && dflt->type == HU_JSON_STRING && dflt->data.string.ptr) {
        r->default_adapter = strdup_alloc(r->alloc, dflt->data.string.ptr, dflt->data.string.len);
        if (!r->default_adapter) {
            hu_json_free(r->alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    /* routes: { "<hash>": { "adapter_path": "..." }, ... } */
    const hu_json_value_t *routes_obj = hu_json_object_get(root, "routes");
    if (routes_obj && routes_obj->type == HU_JSON_OBJECT) {
        for (size_t i = 0; i < routes_obj->data.object.len; i++) {
            const hu_json_pair_t *pair = &routes_obj->data.object.pairs[i];
            if (!pair->value || pair->value->type != HU_JSON_OBJECT)
                continue;
            /* Parse the contact_id_hash. Python writes it as a JSON
             * object KEY (which is always a string), but the value
             * is the decimal repr of a uint64_t. */
            char keybuf[32];
            size_t klen = pair->key_len < sizeof(keybuf) - 1 ? pair->key_len : sizeof(keybuf) - 1;
            memcpy(keybuf, pair->key, klen);
            keybuf[klen] = '\0';
            uint64_t hash = strtoull(keybuf, NULL, 10);
            if (hash == 0)
                continue; /* 0 is the "no value" sentinel; never a real route */

            const hu_json_value_t *adapter_val = hu_json_object_get(pair->value, "adapter_path");
            if (!adapter_val || adapter_val->type != HU_JSON_STRING)
                continue;
            if (!adapter_val->data.string.ptr || adapter_val->data.string.len == 0)
                continue;

            hu_error_t rerr = reserve_entries(r, r->entry_count + 1);
            if (rerr != HU_OK) {
                hu_json_free(r->alloc, root);
                return rerr;
            }
            char *ap =
                strdup_alloc(r->alloc, adapter_val->data.string.ptr, adapter_val->data.string.len);
            if (!ap) {
                hu_json_free(r->alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
            r->entries[r->entry_count].contact_id_hash = hash;
            r->entries[r->entry_count].adapter_path = ap;
            r->entries[r->entry_count].adapter_path_len = adapter_val->data.string.len;
            r->entry_count++;
        }
    }
    hu_json_free(r->alloc, root);
    return HU_OK;
}

hu_error_t hu_m3_contact_routes_create(hu_allocator_t *alloc, const char *path,
                                       hu_m3_contact_routes_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_m3_contact_routes_t *r = (hu_m3_contact_routes_t *)alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return HU_ERR_OUT_OF_MEMORY;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;

    /* Default path: $HOME/.human/training-data/m3_contact_routes.json */
    char default_path[2048];
    const char *use_path = path;
    if (!use_path || !use_path[0]) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            int n = snprintf(default_path, sizeof(default_path),
                             "%s/.human/training-data/m3_contact_routes.json", home);
            if (n > 0 && (size_t)n < sizeof(default_path))
                use_path = default_path;
        }
    }
    if (use_path) {
        size_t plen = strlen(use_path);
        r->path = strdup_alloc(alloc, use_path, plen);
        if (!r->path) {
            alloc->free(alloc->ctx, r, sizeof(*r));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    /* Load — failure is non-fatal; empty routes is a valid state. */
    if (r->path)
        (void)load_from_disk(r);
    *out = r;
    return HU_OK;
}

void hu_m3_contact_routes_destroy(hu_m3_contact_routes_t *routes) {
    if (!routes)
        return;
    clear_entries(routes);
    if (routes->path)
        routes->alloc->free(routes->alloc->ctx, routes->path, strlen(routes->path) + 1);
    routes->alloc->free(routes->alloc->ctx, routes, sizeof(*routes));
}

hu_error_t hu_m3_contact_routes_reload(hu_m3_contact_routes_t *routes) {
    if (!routes || !routes->path)
        return HU_ERR_INVALID_ARGUMENT;
    return load_from_disk(routes);
}

const char *hu_m3_contact_routes_lookup(const hu_m3_contact_routes_t *routes,
                                        uint64_t contact_id_hash) {
    if (!routes)
        return NULL;
    /* Linear scan over the entries vector. Bounded N (in practice a
     * handful of contacts have per-relationship adapters); for very
     * large maps a hashmap could replace this. */
    for (size_t i = 0; i < routes->entry_count; i++) {
        if (routes->entries[i].contact_id_hash == contact_id_hash) {
            return routes->entries[i].adapter_path;
        }
    }
    return routes->default_adapter; /* may be NULL */
}

size_t hu_m3_contact_routes_count(const hu_m3_contact_routes_t *routes) {
    return routes ? routes->entry_count : 0;
}

const char *hu_m3_contact_routes_default(const hu_m3_contact_routes_t *routes) {
    return routes ? routes->default_adapter : NULL;
}
