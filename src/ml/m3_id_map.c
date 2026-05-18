/* M3 id map — string→uint16 lookup for outcome clustering.
 *
 * See include/human/ml/m3_id_map.h for the why. This file is the how:
 *   - Two parallel `entry` vectors (model_names, adapter_paths).
 *   - Insertion appends; lookup is linear scan O(N). Bounded N (max
 *     65535 per space; in practice 2-20 for typical deployments).
 *   - Dirty flag avoids redundant disk writes.
 *   - JSON serialization uses the project's hu_json_buf_t helpers
 *     (consistent with how personal_model.c, config.c emit JSON).
 *   - Atomic save: tmp + fwrite + fflush + fsync + rename, matching
 *     hu_personal_model_save's Phase 0 crash-safety pattern. */

#include "human/ml/m3_id_map.h"

#include "human/core/json.h"
#include "human/core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct entry {
    char *name; /* owned, NUL-terminated */
    size_t name_len;
    uint16_t id;
} entry_t;

typedef struct vec {
    entry_t *items;
    size_t len;
    size_t cap;
    uint16_t next_id; /* next id to assign; starts at 1 (0 = unknown) */
} vec_t;

struct hu_m3_id_map {
    hu_allocator_t *alloc;
    char *path; /* owned; NULL for in-memory-only */
    vec_t models;
    vec_t adapters;
    bool dirty;
};

/* ─────────────────────────────────────────────────────────────────────
 * Vector helpers
 * ───────────────────────────────────────────────────────────────── */

static hu_error_t vec_reserve(hu_allocator_t *alloc, vec_t *v, size_t need) {
    if (need <= v->cap)
        return HU_OK;
    size_t new_cap = v->cap ? v->cap * 2 : 8;
    while (new_cap < need)
        new_cap *= 2;
    entry_t *new_items = (entry_t *)alloc->alloc(alloc->ctx, new_cap * sizeof(entry_t));
    if (!new_items)
        return HU_ERR_OUT_OF_MEMORY;
    if (v->items) {
        memcpy(new_items, v->items, v->len * sizeof(entry_t));
        alloc->free(alloc->ctx, v->items, v->cap * sizeof(entry_t));
    }
    v->items = new_items;
    v->cap = new_cap;
    return HU_OK;
}

static void vec_free(hu_allocator_t *alloc, vec_t *v) {
    for (size_t i = 0; i < v->len; i++) {
        if (v->items[i].name)
            alloc->free(alloc->ctx, v->items[i].name, v->items[i].name_len + 1);
    }
    if (v->items)
        alloc->free(alloc->ctx, v->items, v->cap * sizeof(entry_t));
    memset(v, 0, sizeof(*v));
}

/* Linear scan. Returns id if found, 0 otherwise. */
static uint16_t vec_find(const vec_t *v, const char *name, size_t name_len) {
    for (size_t i = 0; i < v->len; i++) {
        if (v->items[i].name_len == name_len && memcmp(v->items[i].name, name, name_len) == 0) {
            return v->items[i].id;
        }
    }
    return 0;
}

static uint16_t vec_insert(hu_allocator_t *alloc, vec_t *v, const char *name, size_t name_len) {
    /* Ceiling: uint16 can hold 1..65535. next_id == 0 means we'd wrap. */
    if (v->next_id == 0 || v->next_id == UINT16_MAX)
        return 0; /* room exhausted → unknown */
    if (vec_reserve(alloc, v, v->len + 1) != HU_OK)
        return 0;
    char *copy = (char *)alloc->alloc(alloc->ctx, name_len + 1);
    if (!copy)
        return 0;
    memcpy(copy, name, name_len);
    copy[name_len] = '\0';
    entry_t *e = &v->items[v->len++];
    e->name = copy;
    e->name_len = name_len;
    e->id = v->next_id++;
    return e->id;
}

/* ─────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────── */

static hu_error_t load_from_disk(hu_m3_id_map_t *map);

hu_error_t hu_m3_id_map_create(hu_allocator_t *alloc, const char *path, hu_m3_id_map_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_m3_id_map_t *map = (hu_m3_id_map_t *)alloc->alloc(alloc->ctx, sizeof(*map));
    if (!map)
        return HU_ERR_OUT_OF_MEMORY;
    memset(map, 0, sizeof(*map));
    map->alloc = alloc;
    map->models.next_id = 1;
    map->adapters.next_id = 1;

    if (path && path[0]) {
        size_t plen = strlen(path);
        map->path = (char *)alloc->alloc(alloc->ctx, plen + 1);
        if (!map->path) {
            alloc->free(alloc->ctx, map, sizeof(*map));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(map->path, path, plen + 1);
        /* Load existing entries if the file exists. Failure to load
         * (malformed, partial) → log + continue with empty map; the
         * daemon should NOT refuse to start over a corrupt id map. */
        hu_error_t lerr = load_from_disk(map);
        if (lerr != HU_OK && lerr != HU_ERR_IO) {
            hu_log_warn("m3_id_map", NULL, "id map at %s malformed (err=%d) — starting fresh", path,
                        (int)lerr);
        }
    }
    *out = map;
    return HU_OK;
}

void hu_m3_id_map_destroy(hu_m3_id_map_t *map) {
    if (!map)
        return;
    hu_allocator_t *alloc = map->alloc;
    vec_free(alloc, &map->models);
    vec_free(alloc, &map->adapters);
    if (map->path)
        alloc->free(alloc->ctx, map->path, strlen(map->path) + 1);
    alloc->free(alloc->ctx, map, sizeof(*map));
}

static uint16_t lookup_or_insert(hu_m3_id_map_t *map, vec_t *v, const char *name, size_t name_len) {
    if (!map || !name || name_len == 0)
        return 0;
    uint16_t found = vec_find(v, name, name_len);
    if (found)
        return found;
    uint16_t assigned = vec_insert(map->alloc, v, name, name_len);
    if (assigned)
        map->dirty = true;
    return assigned;
}

uint16_t hu_m3_id_map_lookup_or_insert_model(hu_m3_id_map_t *map, const char *model_name,
                                             size_t name_len) {
    return lookup_or_insert(map, map ? &map->models : NULL, model_name, name_len);
}

uint16_t hu_m3_id_map_lookup_or_insert_adapter(hu_m3_id_map_t *map, const char *adapter_path,
                                               size_t path_len) {
    return lookup_or_insert(map, map ? &map->adapters : NULL, adapter_path, path_len);
}

size_t hu_m3_id_map_model_count(const hu_m3_id_map_t *map) {
    return map ? map->models.len : 0;
}

size_t hu_m3_id_map_adapter_count(const hu_m3_id_map_t *map) {
    return map ? map->adapters.len : 0;
}

bool hu_m3_id_map_is_dirty(const hu_m3_id_map_t *map) {
    return map ? map->dirty : false;
}

/* ─────────────────────────────────────────────────────────────────────
 * Serialization
 * ───────────────────────────────────────────────────────────────── */

/* Single-error-propagation helper — no project-wide HU_TRY macro to call. */
#define TRY(expr)               \
    do {                        \
        hu_error_t _e = (expr); \
        if (_e != HU_OK)        \
            return _e;          \
    } while (0)

static hu_error_t serialize_object(hu_json_buf_t *buf, const char *key, const vec_t *v) {
    TRY(hu_json_append_key(buf, key, strlen(key)));
    TRY(hu_json_buf_append_raw(buf, "{", 1));
    for (size_t i = 0; i < v->len; i++) {
        if (i > 0)
            TRY(hu_json_buf_append_raw(buf, ",", 1));
        TRY(hu_json_append_string(buf, v->items[i].name, v->items[i].name_len));
        TRY(hu_json_buf_append_raw(buf, ":", 1));
        char idbuf[16];
        int n = snprintf(idbuf, sizeof(idbuf), "%u", (unsigned)v->items[i].id);
        if (n < 0 || (size_t)n >= sizeof(idbuf))
            return HU_ERR_INTERNAL;
        TRY(hu_json_buf_append_raw(buf, idbuf, (size_t)n));
    }
    TRY(hu_json_buf_append_raw(buf, "}", 1));
    return HU_OK;
}

hu_error_t hu_m3_id_map_save(hu_m3_id_map_t *map) {
    if (!map || !map->path || !map->path[0])
        return HU_OK; /* in-memory only — silent no-op */
    if (!map->dirty)
        return HU_OK;

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, map->alloc);
    if (err != HU_OK)
        return err;
    err = hu_json_buf_append_raw(&buf, "{", 1);
    if (err == HU_OK)
        err = serialize_object(&buf, "models", &map->models);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err == HU_OK)
        err = serialize_object(&buf, "adapters", &map->adapters);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "}\n", 2);
    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    /* Atomic write — same pattern as hu_personal_model_save Phase 0. */
    char tmp[2048];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", map->path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp)) {
        hu_json_buf_free(&buf);
        return HU_ERR_INVALID_ARGUMENT;
    }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    if (fwrite(buf.ptr, 1, buf.len, fp) != buf.len) {
        fclose(fp);
        (void)unlink(tmp);
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    if (fclose(fp) != 0) {
        (void)unlink(tmp);
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    if (rename(tmp, map->path) != 0) {
        (void)unlink(tmp);
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    hu_json_buf_free(&buf);
    map->dirty = false;
    return HU_OK;
}

static hu_error_t parse_section(hu_m3_id_map_t *map, const hu_json_value_t *section_obj,
                                vec_t *target) {
    if (!section_obj || section_obj->type != HU_JSON_OBJECT)
        return HU_OK; /* missing/empty section is fine */
    for (size_t i = 0; i < section_obj->data.object.len; i++) {
        const hu_json_pair_t *pair = &section_obj->data.object.pairs[i];
        if (!pair->value || pair->value->type != HU_JSON_NUMBER)
            continue;
        double v = pair->value->data.number;
        if (v < 1.0 || v > 65535.0)
            continue;
        uint16_t id = (uint16_t)v;
        hu_error_t err = vec_reserve(map->alloc, target, target->len + 1);
        if (err != HU_OK)
            return err;
        char *copy = (char *)map->alloc->alloc(map->alloc->ctx, pair->key_len + 1);
        if (!copy)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(copy, pair->key, pair->key_len);
        copy[pair->key_len] = '\0';
        entry_t *e = &target->items[target->len++];
        e->name = copy;
        e->name_len = pair->key_len;
        e->id = id;
        if (id >= target->next_id)
            target->next_id = (uint16_t)(id + 1);
    }
    return HU_OK;
}

static hu_error_t load_from_disk(hu_m3_id_map_t *map) {
    FILE *fp = fopen(map->path, "rb");
    if (!fp)
        return HU_ERR_IO; /* file missing — silent, caller treats as OK */

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > (1 << 24)) { /* 16 MB sanity cap */
        fclose(fp);
        return HU_ERR_IO;
    }
    rewind(fp);

    char *body = (char *)map->alloc->alloc(map->alloc->ctx, (size_t)sz + 1);
    if (!body) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(body, 1, (size_t)sz, fp);
    fclose(fp);
    body[got] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(map->alloc, body, got, &root);
    map->alloc->free(map->alloc->ctx, body, (size_t)sz + 1);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_OBJECT) {
        hu_json_free(map->alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const hu_json_value_t *models = hu_json_object_get(root, "models");
    const hu_json_value_t *adapters = hu_json_object_get(root, "adapters");
    err = parse_section(map, models, &map->models);
    if (err == HU_OK)
        err = parse_section(map, adapters, &map->adapters);
    hu_json_free(map->alloc, root);
    /* Loading from disk doesn't dirty the map — the in-memory state
     * matches what's on disk by definition. */
    return err;
}
