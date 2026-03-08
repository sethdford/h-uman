/* In-memory LRU memory backend.
 * Pure in-memory store with LRU eviction — no disk I/O.
 * Uses doubly-linked list + hash table for O(1) lookup and eviction. */

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SC_LRU_BUCKETS   64
#define SC_LRU_HASH_MULT 31

typedef struct lru_entry {
    char *key;
    char *content;
    sc_memory_category_t category;
    char *session_id;
    char *created_at;
    char *updated_at;
    uint64_t last_access;
    struct lru_entry *hash_next;
    struct lru_entry *lru_prev;
    struct lru_entry *lru_next;
} lru_entry_t;

typedef struct sc_lru_memory {
    sc_allocator_t *alloc;
    lru_entry_t *buckets[SC_LRU_BUCKETS];
    lru_entry_t *lru_head; /* most recently used */
    lru_entry_t *lru_tail; /* least recently used */
    size_t count;
    size_t max_entries;
    uint64_t access_counter;
} sc_lru_memory_t;

static int contains_substring(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0)
        return 1;
    if (hlen < nlen)
        return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static uint32_t hash_key(const char *key, size_t len) {
    uint32_t h = 5381;
    for (size_t i = 0; i < len && key[i]; i++)
        h = (h * SC_LRU_HASH_MULT) + (unsigned char)key[i];
    return h % SC_LRU_BUCKETS;
}

static lru_entry_t *find_entry(sc_lru_memory_t *self, const char *key, size_t key_len) {
    uint32_t b = hash_key(key, key_len);
    for (lru_entry_t *e = self->buckets[b]; e; e = e->hash_next) {
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

static void free_stored_entry(sc_lru_memory_t *self, lru_entry_t *e) {
    if (!e || !self->alloc)
        return;
    if (e->key)
        self->alloc->free(self->alloc->ctx, e->key, strlen(e->key) + 1);
    if (e->content)
        self->alloc->free(self->alloc->ctx, e->content, strlen(e->content) + 1);
    if (e->created_at)
        self->alloc->free(self->alloc->ctx, e->created_at, strlen(e->created_at) + 1);
    if (e->updated_at)
        self->alloc->free(self->alloc->ctx, e->updated_at, strlen(e->updated_at) + 1);
    if (e->session_id)
        self->alloc->free(self->alloc->ctx, e->session_id, strlen(e->session_id) + 1);
    if (e->category.tag == SC_MEMORY_CATEGORY_CUSTOM && e->category.data.custom.name)
        self->alloc->free(self->alloc->ctx, (void *)e->category.data.custom.name,
                          e->category.data.custom.name_len + 1);
}

static void unlink_lru(sc_lru_memory_t *self, lru_entry_t *e) {
    if (e->lru_prev)
        e->lru_prev->lru_next = e->lru_next;
    else
        self->lru_head = e->lru_next;
    if (e->lru_next)
        e->lru_next->lru_prev = e->lru_prev;
    else
        self->lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void link_mru(sc_lru_memory_t *self, lru_entry_t *e) {
    e->lru_next = self->lru_head;
    e->lru_prev = NULL;
    if (self->lru_head)
        self->lru_head->lru_prev = e;
    else
        self->lru_tail = e;
    self->lru_head = e;
}

static void evict_lru(sc_lru_memory_t *self) {
    lru_entry_t *victim = self->lru_tail;
    if (!victim)
        return;
    const char *key = victim->key;
    size_t key_len = key ? strlen(key) : 0;
    uint32_t b = hash_key(key, key_len);
    lru_entry_t **pp = &self->buckets[b];
    for (; *pp; pp = &(*pp)->hash_next) {
        if (*pp == victim) {
            *pp = victim->hash_next;
            break;
        }
    }
    unlink_lru(self, victim);
    free_stored_entry(self, victim);
    self->alloc->free(self->alloc->ctx, victim, sizeof(lru_entry_t));
    self->count--;
}

static uint64_t next_access(sc_lru_memory_t *self) {
    return ++self->access_counter;
}

static char *now_timestamp(sc_lru_memory_t *self) {
    time_t t = time(NULL);
    return sc_sprintf(self->alloc, "%ld", (long)t);
}

static sc_error_t dup_category(sc_allocator_t *alloc, const sc_memory_category_t *cat,
                               sc_memory_category_t *out) {
    out->tag = cat->tag;
    if (cat->tag == SC_MEMORY_CATEGORY_CUSTOM && cat->data.custom.name) {
        size_t n = cat->data.custom.name_len;
        char *name = (char *)alloc->alloc(alloc->ctx, n + 1);
        if (!name)
            return SC_ERR_OUT_OF_MEMORY;
        memcpy(name, cat->data.custom.name, n);
        name[n] = '\0';
        out->data.custom.name = name;
        out->data.custom.name_len = n;
    }
    return SC_OK;
}

static void category_to_out(const sc_memory_category_t *src, sc_memory_entry_t *out,
                            sc_allocator_t *alloc) {
    out->category.tag = src->tag;
    if (src->tag == SC_MEMORY_CATEGORY_CUSTOM && src->data.custom.name) {
        size_t n = src->data.custom.name_len;
        char *name = (char *)alloc->alloc(alloc->ctx, n + 1);
        if (name) {
            memcpy(name, src->data.custom.name, n);
            name[n] = '\0';
            out->category.data.custom.name = name;
            out->category.data.custom.name_len = n;
        }
    }
}

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "memory_lru";
}

static sc_error_t impl_store(void *ctx, const char *key, size_t key_len, const char *content,
                             size_t content_len, const sc_memory_category_t *category,
                             const char *session_id, size_t session_id_len) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    sc_allocator_t *alloc = self->alloc;

    lru_entry_t *existing = find_entry(self, key, key_len);
    if (existing) {
        alloc->free(alloc->ctx, (void *)existing->content, strlen(existing->content) + 1);
        existing->content = sc_strndup(alloc, content, content_len);
        if (!existing->content)
            return SC_ERR_OUT_OF_MEMORY;

        alloc->free(alloc->ctx, (void *)existing->updated_at, strlen(existing->updated_at) + 1);
        existing->updated_at = now_timestamp(self);
        if (!existing->updated_at)
            return SC_ERR_OUT_OF_MEMORY;

        if (existing->category.tag == SC_MEMORY_CATEGORY_CUSTOM &&
            existing->category.data.custom.name)
            alloc->free(alloc->ctx, (void *)existing->category.data.custom.name,
                        existing->category.data.custom.name_len + 1);
        dup_category(alloc, category, &existing->category);

        if (existing->session_id)
            alloc->free(alloc->ctx, (void *)existing->session_id, strlen(existing->session_id) + 1);
        existing->session_id =
            session_id && session_id_len > 0 ? sc_strndup(alloc, session_id, session_id_len) : NULL;

        existing->last_access = next_access(self);
        unlink_lru(self, existing);
        link_mru(self, existing);
        return SC_OK;
    }

    if (self->max_entries == 0)
        return SC_OK;
    while (self->count >= self->max_entries)
        evict_lru(self);

    lru_entry_t *e = (lru_entry_t *)alloc->alloc(alloc->ctx, sizeof(lru_entry_t));
    if (!e)
        return SC_ERR_OUT_OF_MEMORY;
    memset(e, 0, sizeof(lru_entry_t));

    e->key = sc_strndup(alloc, key, key_len);
    e->content = sc_strndup(alloc, content, content_len);
    e->created_at = now_timestamp(self);
    e->updated_at = sc_strndup(alloc, e->created_at, strlen(e->created_at));
    e->session_id =
        session_id && session_id_len > 0 ? sc_strndup(alloc, session_id, session_id_len) : NULL;

    if (!e->key || !e->content || !e->created_at || !e->updated_at) {
        free_stored_entry(self, e);
        alloc->free(alloc->ctx, e, sizeof(lru_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    dup_category(alloc, category, &e->category);
    e->last_access = next_access(self);

    uint32_t b = hash_key(key, key_len);
    e->hash_next = self->buckets[b];
    self->buckets[b] = e;
    link_mru(self, e);
    self->count++;
    return SC_OK;
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc, const char *query, size_t query_len,
                              size_t limit, const char *session_id, size_t session_id_len,
                              sc_memory_entry_t **out, size_t *out_count) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;

    typedef struct {
        lru_entry_t *e;
        const char *map_key;
    } pair_t;
    pair_t *matches = NULL;
    size_t nmatches = 0;
    size_t cap = 16;

    matches = (pair_t *)alloc->alloc(alloc->ctx, cap * sizeof(pair_t));
    if (!matches)
        return SC_ERR_OUT_OF_MEMORY;

    for (uint32_t b = 0; b < SC_LRU_BUCKETS; b++) {
        for (lru_entry_t *e = self->buckets[b]; e; e = e->hash_next) {
            if (session_id && session_id_len > 0) {
                if (!e->session_id || strlen(e->session_id) != session_id_len ||
                    memcmp(e->session_id, session_id, session_id_len) != 0)
                    continue;
            }
            size_t key_len = e->key ? strlen(e->key) : 0;
            size_t content_len = e->content ? strlen(e->content) : 0;
            int key_match = e->key && contains_substring(e->key, key_len, query, query_len);
            int content_match =
                e->content && contains_substring(e->content, content_len, query, query_len);
            if (key_match || content_match) {
                if (nmatches >= cap) {
                    size_t new_cap = cap * 2;
                    pair_t *n = (pair_t *)alloc->realloc(alloc->ctx, matches, cap * sizeof(pair_t),
                                                         new_cap * sizeof(pair_t));
                    if (!n)
                        goto fail;
                    matches = n;
                    cap = new_cap;
                }
                matches[nmatches].e = e;
                matches[nmatches].map_key = e->key;
                nmatches++;
            }
        }
    }

    /* Sort by last_access desc */
    for (size_t i = 0; i < nmatches; i++) {
        for (size_t j = i + 1; j < nmatches; j++) {
            if (matches[j].e->last_access > matches[i].e->last_access) {
                pair_t t = matches[i];
                matches[i] = matches[j];
                matches[j] = t;
            }
        }
    }

    size_t take = limit < nmatches ? limit : nmatches;
    sc_memory_entry_t *results =
        (sc_memory_entry_t *)alloc->alloc(alloc->ctx, take * sizeof(sc_memory_entry_t));
    if (!results)
        goto fail;

    for (size_t i = 0; i < take; i++) {
        lru_entry_t *src = matches[i].e;
        sc_memory_entry_t *r = &results[i];
        memset(r, 0, sizeof(*r));
        r->id = sc_strndup(alloc, src->key, strlen(src->key));
        r->id_len = strlen(src->key);
        r->key = sc_strndup(alloc, src->key, strlen(src->key));
        r->key_len = strlen(src->key);
        r->content = sc_strndup(alloc, src->content, strlen(src->content));
        r->content_len = strlen(src->content);
        category_to_out(&src->category, r, alloc);
        r->timestamp = sc_strndup(alloc, src->updated_at, strlen(src->updated_at));
        r->timestamp_len = strlen(src->updated_at);
        r->session_id =
            src->session_id ? sc_strndup(alloc, src->session_id, strlen(src->session_id)) : NULL;
        r->session_id_len = r->session_id ? strlen(r->session_id) : 0;
        r->score = NAN;
    }

    alloc->free(alloc->ctx, matches, cap * sizeof(pair_t));
    *out = results;
    *out_count = take;
    return SC_OK;
fail:
    if (matches)
        alloc->free(alloc->ctx, matches, cap * sizeof(pair_t));
    return SC_ERR_OUT_OF_MEMORY;
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc, const char *key, size_t key_len,
                           sc_memory_entry_t *out, bool *found) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    *found = false;

    lru_entry_t *e = find_entry(self, key, key_len);
    if (!e)
        return SC_OK;

    e->last_access = next_access(self);
    unlink_lru(self, e);
    link_mru(self, e);

    memset(out, 0, sizeof(*out));
    out->id = sc_strndup(alloc, e->key, strlen(e->key));
    out->id_len = strlen(e->key);
    out->key = sc_strndup(alloc, e->key, strlen(e->key));
    out->key_len = strlen(e->key);
    out->content = sc_strndup(alloc, e->content, strlen(e->content));
    out->content_len = strlen(e->content);
    category_to_out(&e->category, out, alloc);
    out->timestamp = sc_strndup(alloc, e->updated_at, strlen(e->updated_at));
    out->timestamp_len = strlen(e->updated_at);
    out->session_id =
        e->session_id ? sc_strndup(alloc, e->session_id, strlen(e->session_id)) : NULL;
    out->session_id_len = out->session_id ? strlen(out->session_id) : 0;
    out->score = NAN;
    *found = true;
    return SC_OK;
}

static int category_matches(const sc_memory_category_t *filter, const sc_memory_category_t *entry) {
    if (!filter)
        return 1;
    if (filter->tag != entry->tag)
        return 0;
    if (filter->tag == SC_MEMORY_CATEGORY_CUSTOM) {
        if (!filter->data.custom.name || !entry->data.custom.name)
            return 0;
        if (filter->data.custom.name_len != entry->data.custom.name_len)
            return 0;
        return memcmp(filter->data.custom.name, entry->data.custom.name,
                      filter->data.custom.name_len) == 0;
    }
    return 1;
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc, const sc_memory_category_t *category,
                            const char *session_id, size_t session_id_len, sc_memory_entry_t **out,
                            size_t *out_count) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;

    sc_memory_entry_t *results = NULL;
    size_t n = 0;
    size_t cap = 16;

    results = (sc_memory_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(sc_memory_entry_t));
    if (!results)
        return SC_ERR_OUT_OF_MEMORY;

    for (lru_entry_t *e = self->lru_head; e; e = e->lru_next) {
        if (!category_matches(category, &e->category))
            continue;
        if (session_id && session_id_len > 0) {
            if (!e->session_id || strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                continue;
        }
        if (n >= cap) {
            size_t new_cap = cap * 2;
            sc_memory_entry_t *r = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!r) {
                for (size_t i = 0; i < n; i++)
                    sc_memory_entry_free_fields(alloc, &results[i]);
                alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            results = r;
            cap = new_cap;
        }
        sc_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = sc_strndup(alloc, e->key, strlen(e->key));
        r->id_len = strlen(e->key);
        r->key = sc_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->content = sc_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        category_to_out(&e->category, r, alloc);
        r->timestamp = sc_strndup(alloc, e->updated_at, strlen(e->updated_at));
        r->timestamp_len = strlen(e->updated_at);
        r->session_id =
            e->session_id ? sc_strndup(alloc, e->session_id, strlen(e->session_id)) : NULL;
        r->session_id_len = r->session_id ? strlen(r->session_id) : 0;
        r->score = NAN;
        n++;
    }

    *out = results;
    *out_count = n;
    return SC_OK;
}

static sc_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    *deleted = false;

    lru_entry_t *e = find_entry(self, key, key_len);
    if (!e)
        return SC_OK;

    uint32_t b = hash_key(key, key_len);
    lru_entry_t **pp = &self->buckets[b];
    for (; *pp; pp = &(*pp)->hash_next) {
        if (*pp == e) {
            *pp = e->hash_next;
            break;
        }
    }
    unlink_lru(self, e);
    free_stored_entry(self, e);
    self->alloc->free(self->alloc->ctx, e, sizeof(lru_entry_t));
    self->count--;
    *deleted = true;
    return SC_OK;
}

static sc_error_t impl_count(void *ctx, size_t *out) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    *out = self->count;
    return SC_OK;
}

static bool impl_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static void impl_deinit(void *ctx) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)ctx;
    for (uint32_t b = 0; b < SC_LRU_BUCKETS; b++) {
        while (self->buckets[b]) {
            lru_entry_t *e = self->buckets[b];
            self->buckets[b] = e->hash_next;
            free_stored_entry(self, e);
            self->alloc->free(self->alloc->ctx, e, sizeof(lru_entry_t));
        }
    }
    self->alloc->free(self->alloc->ctx, self, sizeof(sc_lru_memory_t));
}

static const sc_memory_vtable_t lru_vtable = {
    .name = impl_name,
    .store = impl_store,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

sc_memory_t sc_memory_lru_create(sc_allocator_t *alloc, size_t max_entries) {
    sc_lru_memory_t *self = (sc_lru_memory_t *)alloc->alloc(alloc->ctx, sizeof(sc_lru_memory_t));
    if (!self)
        return (sc_memory_t){.ctx = NULL, .vtable = NULL};
    memset(self, 0, sizeof(sc_lru_memory_t));
    self->alloc = alloc;
    self->max_entries = max_entries;
    return (sc_memory_t){
        .ctx = self,
        .vtable = &lru_vtable,
    };
}
