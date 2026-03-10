#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/lifecycle.h"
#include <math.h>
#include <string.h>

typedef struct cache_slot {
    char *key;
    size_t key_len;
    bool used;
    hu_memory_entry_t entry;
} cache_slot_t;

struct hu_memory_cache {
    hu_allocator_t *alloc;
    cache_slot_t *slots;
    size_t max_entries;
    size_t count;
};

static bool key_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static hu_error_t entry_copy(hu_allocator_t *alloc, const hu_memory_entry_t *src,
                             hu_memory_entry_t *dst) {
    memset(dst, 0, sizeof(*dst));
    if (src->id) {
        dst->id = hu_strndup(alloc, src->id, src->id_len);
        if (!dst->id)
            return HU_ERR_OUT_OF_MEMORY;
        dst->id_len = src->id_len;
    }
    if (src->key) {
        dst->key = hu_strndup(alloc, src->key, src->key_len);
        if (!dst->key) {
            if (dst->id)
                alloc->free(alloc->ctx, (void *)dst->id, dst->id_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        dst->key_len = src->key_len;
    }
    if (src->content) {
        dst->content = hu_strndup(alloc, src->content, src->content_len);
        if (!dst->content) {
            if (dst->id)
                alloc->free(alloc->ctx, (void *)dst->id, dst->id_len + 1);
            if (dst->key)
                alloc->free(alloc->ctx, (void *)dst->key, dst->key_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        dst->content_len = src->content_len;
    }
    dst->category = src->category;
    if (src->category.data.custom.name) {
        dst->category.data.custom.name =
            hu_strndup(alloc, src->category.data.custom.name, src->category.data.custom.name_len);
        if (!dst->category.data.custom.name) {
            hu_memory_entry_free_fields(alloc, dst);
            return HU_ERR_OUT_OF_MEMORY;
        }
        dst->category.data.custom.name_len = src->category.data.custom.name_len;
    }
    if (src->timestamp) {
        dst->timestamp = hu_strndup(alloc, src->timestamp, src->timestamp_len);
        if (!dst->timestamp) {
            hu_memory_entry_free_fields(alloc, dst);
            return HU_ERR_OUT_OF_MEMORY;
        }
        dst->timestamp_len = src->timestamp_len;
    }
    if (src->session_id) {
        dst->session_id = hu_strndup(alloc, src->session_id, src->session_id_len);
        if (!dst->session_id) {
            hu_memory_entry_free_fields(alloc, dst);
            return HU_ERR_OUT_OF_MEMORY;
        }
        dst->session_id_len = src->session_id_len;
    }
    if (src->source && src->source_len > 0) {
        dst->source = hu_strndup(alloc, src->source, src->source_len);
        if (!dst->source) {
            hu_memory_entry_free_fields(alloc, dst);
            return HU_ERR_OUT_OF_MEMORY;
        }
        dst->source_len = src->source_len;
    }
    dst->score = src->score;
    return HU_OK;
}

static void slot_free(hu_allocator_t *alloc, cache_slot_t *slot) {
    if (!slot || !slot->used)
        return;
    if (slot->key)
        alloc->free(alloc->ctx, slot->key, slot->key_len + 1);
    hu_memory_entry_free_fields(alloc, &slot->entry);
    slot->used = false;
}

/* Move slot at idx to front (index 0) by shifting others right */
static void move_to_front(cache_slot_t *slots, size_t count, size_t idx) {
    (void)count;
    if (idx == 0)
        return;
    cache_slot_t tmp = slots[idx];
    memmove(slots + 1, slots, idx * sizeof(cache_slot_t));
    slots[0] = tmp;
}

/* Shift slots[0..n-1] right by 1; new slot at 0. */
static void shift_right(cache_slot_t *slots, size_t n) {
    if (n == 0)
        return;
    memmove(slots + 1, slots, n * sizeof(cache_slot_t));
}

/* Shift slots[1..n] left to slots[0..n-1]; removes slot 0. */
static void shift_left(cache_slot_t *slots, size_t n) {
    if (n <= 1)
        return;
    memmove(slots, slots + 1, (n - 1) * sizeof(cache_slot_t));
}

hu_memory_cache_t *hu_memory_cache_create(hu_allocator_t *alloc, size_t max_entries) {
    if (!alloc || max_entries == 0)
        return NULL;
    hu_memory_cache_t *cache =
        (hu_memory_cache_t *)alloc->alloc(alloc->ctx, sizeof(hu_memory_cache_t));
    if (!cache)
        return NULL;
    cache->alloc = alloc;
    cache->max_entries = max_entries;
    cache->count = 0;
    cache->slots = (cache_slot_t *)alloc->alloc(alloc->ctx, max_entries * sizeof(cache_slot_t));
    if (!cache->slots) {
        alloc->free(alloc->ctx, cache, sizeof(hu_memory_cache_t));
        return NULL;
    }
    memset(cache->slots, 0, max_entries * sizeof(cache_slot_t));
    return cache;
}

void hu_memory_cache_destroy(hu_memory_cache_t *cache) {
    if (!cache)
        return;
    for (size_t i = 0; i < cache->max_entries; i++) {
        slot_free(cache->alloc, &cache->slots[i]);
    }
    cache->alloc->free(cache->alloc->ctx, cache->slots, cache->max_entries * sizeof(cache_slot_t));
    cache->alloc->free(cache->alloc->ctx, cache, sizeof(hu_memory_cache_t));
}

hu_error_t hu_memory_cache_get(hu_memory_cache_t *cache, const char *key, size_t key_len,
                               hu_memory_entry_t *out, bool *found) {
    if (!cache || !out || !found)
        return HU_ERR_INVALID_ARGUMENT;
    *found = false;

    for (size_t i = 0; i < cache->max_entries; i++) {
        cache_slot_t *slot = &cache->slots[i];
        if (!slot->used)
            continue;
        if (!key_eq(slot->key, slot->key_len, key, key_len))
            continue;
        move_to_front(cache->slots, cache->count, i);
        hu_error_t err = entry_copy(cache->alloc, &cache->slots[0].entry, out);
        if (err != HU_OK)
            return err;
        *found = true;
        return HU_OK;
    }
    return HU_OK;
}

hu_error_t hu_memory_cache_put(hu_memory_cache_t *cache, const char *key, size_t key_len,
                               const hu_memory_entry_t *entry) {
    if (!cache || !entry)
        return HU_ERR_INVALID_ARGUMENT;

    bool found_existing = false;
    for (size_t i = 0; i < cache->max_entries; i++) {
        cache_slot_t *slot = &cache->slots[i];
        if (!slot->used)
            continue;
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
    if (!k)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(k, key, key_len);
    k[key_len] = '\0';
    slot->key = k;
    slot->key_len = key_len;

    hu_error_t err = entry_copy(cache->alloc, entry, &slot->entry);
    if (err != HU_OK) {
        cache->alloc->free(cache->alloc->ctx, k, key_len + 1);
        slot->key = NULL;
        slot->key_len = 0;
        return err;
    }
    slot->used = true;
    cache->count++;
    return HU_OK;
}

void hu_memory_cache_invalidate(hu_memory_cache_t *cache, const char *key, size_t key_len) {
    if (!cache)
        return;
    for (size_t i = 0; i < cache->max_entries; i++) {
        cache_slot_t *slot = &cache->slots[i];
        if (!slot->used)
            continue;
        if (key_eq(slot->key, slot->key_len, key, key_len)) {
            slot_free(cache->alloc, slot);
            shift_left(cache->slots + i, cache->count - i);
            cache->count--;
            return;
        }
    }
}

void hu_memory_cache_clear(hu_memory_cache_t *cache) {
    if (!cache)
        return;
    for (size_t i = 0; i < cache->max_entries; i++) {
        slot_free(cache->alloc, &cache->slots[i]);
    }
    cache->count = 0;
}

size_t hu_memory_cache_count(const hu_memory_cache_t *cache) {
    return cache ? cache->count : 0;
}
