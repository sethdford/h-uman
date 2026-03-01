#include "seaclaw/memory/lifecycle.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <string.h>
#include <math.h>

typedef struct cache_slot {
    char *key;
    size_t key_len;
    bool used;
    sc_memory_entry_t entry;
} cache_slot_t;

struct sc_memory_cache {
    sc_allocator_t *alloc;
    cache_slot_t *slots;
    size_t max_entries;
    size_t count;
};

static bool key_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static sc_error_t entry_copy(sc_allocator_t *alloc, const sc_memory_entry_t *src,
    sc_memory_entry_t *dst) {
    memset(dst, 0, sizeof(*dst));
    if (src->id) {
        dst->id = sc_strndup(alloc, src->id, src->id_len);
        if (!dst->id) return SC_ERR_OUT_OF_MEMORY;
        dst->id_len = src->id_len;
    }
    if (src->key) {
        dst->key = sc_strndup(alloc, src->key, src->key_len);
        if (!dst->key) {
            if (dst->id) alloc->free(alloc->ctx, (void *)dst->id, dst->id_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        dst->key_len = src->key_len;
    }
    if (src->content) {
        dst->content = sc_strndup(alloc, src->content, src->content_len);
        if (!dst->content) {
            if (dst->id) alloc->free(alloc->ctx, (void *)dst->id, dst->id_len + 1);
            if (dst->key) alloc->free(alloc->ctx, (void *)dst->key, dst->key_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        dst->content_len = src->content_len;
    }
    dst->category = src->category;
    if (src->category.data.custom.name) {
        dst->category.data.custom.name = sc_strndup(alloc, src->category.data.custom.name,
            src->category.data.custom.name_len);
        if (!dst->category.data.custom.name) {
            sc_memory_entry_free_fields(alloc, dst);
            return SC_ERR_OUT_OF_MEMORY;
        }
        dst->category.data.custom.name_len = src->category.data.custom.name_len;
    }
    if (src->timestamp) {
        dst->timestamp = sc_strndup(alloc, src->timestamp, src->timestamp_len);
        if (!dst->timestamp) {
            sc_memory_entry_free_fields(alloc, dst);
            return SC_ERR_OUT_OF_MEMORY;
        }
        dst->timestamp_len = src->timestamp_len;
    }
    if (src->session_id) {
        dst->session_id = sc_strndup(alloc, src->session_id, src->session_id_len);
        if (!dst->session_id) {
            sc_memory_entry_free_fields(alloc, dst);
            return SC_ERR_OUT_OF_MEMORY;
        }
        dst->session_id_len = src->session_id_len;
    }
    dst->score = src->score;
    return SC_OK;
}

static void slot_free(sc_allocator_t *alloc, cache_slot_t *slot) {
    if (!slot || !slot->used) return;
    if (slot->key) alloc->free(alloc->ctx, slot->key, slot->key_len + 1);
    sc_memory_entry_free_fields(alloc, &slot->entry);
    slot->used = false;
}

/* Move slot at idx to front (index 0) by shifting others right */
static void move_to_front(cache_slot_t *slots, size_t count, size_t idx) {
    (void)count;
    if (idx == 0) return;
    cache_slot_t tmp = slots[idx];
    memmove(slots + 1, slots, idx * sizeof(cache_slot_t));
    slots[0] = tmp;
}

/* Shift slots[0..n-1] right by 1; new slot at 0. */
static void shift_right(cache_slot_t *slots, size_t n) {
    if (n == 0) return;
    memmove(slots + 1, slots, n * sizeof(cache_slot_t));
}

/* Shift slots[1..n] left to slots[0..n-1]; removes slot 0. */
static void shift_left(cache_slot_t *slots, size_t n) {
    if (n <= 1) return;
    memmove(slots, slots + 1, (n - 1) * sizeof(cache_slot_t));
}

sc_memory_cache_t *sc_memory_cache_create(sc_allocator_t *alloc, size_t max_entries) {
    if (!alloc || max_entries == 0) return NULL;
    sc_memory_cache_t *cache = (sc_memory_cache_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_memory_cache_t));
    if (!cache) return NULL;
    cache->alloc = alloc;
    cache->max_entries = max_entries;
    cache->count = 0;
    cache->slots = (cache_slot_t *)alloc->alloc(alloc->ctx,
        max_entries * sizeof(cache_slot_t));
    if (!cache->slots) {
        alloc->free(alloc->ctx, cache, sizeof(sc_memory_cache_t));
        return NULL;
    }
    memset(cache->slots, 0, max_entries * sizeof(cache_slot_t));
    return cache;
}

void sc_memory_cache_destroy(sc_memory_cache_t *cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->max_entries; i++) {
        slot_free(cache->alloc, &cache->slots[i]);
    }
    cache->alloc->free(cache->alloc->ctx, cache->slots,
        cache->max_entries * sizeof(cache_slot_t));
    cache->alloc->free(cache->alloc->ctx, cache, sizeof(sc_memory_cache_t));
}

sc_error_t sc_memory_cache_get(sc_memory_cache_t *cache, const char *key, size_t key_len,
    sc_memory_entry_t *out, bool *found) {
    if (!cache || !out || !found) return SC_ERR_INVALID_ARGUMENT;
    *found = false;

    for (size_t i = 0; i < cache->max_entries; i++) {
        cache_slot_t *slot = &cache->slots[i];
        if (!slot->used) continue;
        if (!key_eq(slot->key, slot->key_len, key, key_len)) continue;
        move_to_front(cache->slots, cache->count, i);
        sc_error_t err = entry_copy(cache->alloc, &cache->slots[0].entry, out);
        if (err != SC_OK) return err;
        *found = true;
        return SC_OK;
    }
    return SC_OK;
}

sc_error_t sc_memory_cache_put(sc_memory_cache_t *cache, const char *key, size_t key_len,
    const sc_memory_entry_t *entry) {
    if (!cache || !entry) return SC_ERR_INVALID_ARGUMENT;

    bool found_existing = false;
    for (size_t i = 0; i < cache->max_entries; i++) {
        cache_slot_t *slot = &cache->slots[i];
        if (!slot->used) continue;
        if (key_eq(slot->key, slot->key_len, key, key_len)) {
            slot_free(cache->alloc, slot);
            move_to_front(cache->slots, cache->count, i);
            found_existing = true;
            goto insert;
        }
    }

    if (cache->count >= cache->max_entries) {
        size_t lru = cache->max_entries - 1;
        slot_free(cache->alloc, &cache->slots[lru]);
        cache->count--;
    }

insert:;
    cache_slot_t *slot = &cache->slots[0];
    if (!found_existing && cache->count > 0) {
        shift_right(cache->slots, cache->count);
    }
    slot = &cache->slots[0];

    char *k = (char *)cache->alloc->alloc(cache->alloc->ctx, key_len + 1);
    if (!k) return SC_ERR_OUT_OF_MEMORY;
    memcpy(k, key, key_len);
    k[key_len] = '\0';
    slot->key = k;
    slot->key_len = key_len;

    sc_error_t err = entry_copy(cache->alloc, entry, &slot->entry);
    if (err != SC_OK) {
        cache->alloc->free(cache->alloc->ctx, k, key_len + 1);
        slot->key = NULL;
        slot->key_len = 0;
        return err;
    }
    slot->used = true;
    cache->count++;
    return SC_OK;
}

void sc_memory_cache_invalidate(sc_memory_cache_t *cache, const char *key, size_t key_len) {
    if (!cache) return;
    for (size_t i = 0; i < cache->max_entries; i++) {
        cache_slot_t *slot = &cache->slots[i];
        if (!slot->used) continue;
        if (key_eq(slot->key, slot->key_len, key, key_len)) {
            slot_free(cache->alloc, slot);
            shift_left(cache->slots + i, cache->count - i);
            cache->count--;
            return;
        }
    }
}

void sc_memory_cache_clear(sc_memory_cache_t *cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->max_entries; i++) {
        slot_free(cache->alloc, &cache->slots[i]);
    }
    cache->count = 0;
}

size_t sc_memory_cache_count(const sc_memory_cache_t *cache) {
    return cache ? cache->count : 0;
}
