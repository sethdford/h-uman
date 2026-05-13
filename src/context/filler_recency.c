#include "human/filler_recency.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static size_t bounded_len(size_t chat_id_len) {
    /* Stored length is capped so the copied bytes fit in chat_id[]. The
     * caller may pass a length larger than HU_FILLER_RECENCY_CHAT_ID_MAX - 1;
     * we clamp rather than refuse so the long-id truncation test passes. */
    if (chat_id_len >= HU_FILLER_RECENCY_CHAT_ID_MAX) {
        return HU_FILLER_RECENCY_CHAT_ID_MAX - 1;
    }
    return chat_id_len;
}

/* Return the index of an existing entry for this chat_id, or -1. */
static int find_entry(const hu_filler_recency_t *r, const char *chat_id, size_t stored_len) {
    for (int i = 0; i < HU_FILLER_RECENCY_CAPACITY; ++i) {
        const hu_filler_recency_entry_t *e = &r->entries[i];
        if (!e->in_use) {
            continue;
        }
        if (e->chat_id_len != stored_len) {
            continue;
        }
        if (memcmp(e->chat_id, chat_id, stored_len) == 0) {
            return i;
        }
    }
    return -1;
}

/* Return the index of a free slot, or -1 if all are in use. */
static int find_free(const hu_filler_recency_t *r) {
    for (int i = 0; i < HU_FILLER_RECENCY_CAPACITY; ++i) {
        if (!r->entries[i].in_use) {
            return i;
        }
    }
    return -1;
}

/* Return the index of the entry with the smallest last_used_seq (LRU). */
static int find_lru(const hu_filler_recency_t *r) {
    int lru_idx = 0;
    uint64_t lru_seq = r->entries[0].last_used_seq;
    for (int i = 1; i < HU_FILLER_RECENCY_CAPACITY; ++i) {
        if (r->entries[i].last_used_seq < lru_seq) {
            lru_seq = r->entries[i].last_used_seq;
            lru_idx = i;
        }
    }
    return lru_idx;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void hu_filler_recency_record(hu_filler_recency_t *r, const char *chat_id, size_t chat_id_len,
                              uint16_t index) {
    if (!r || !chat_id) {
        return;
    }

    size_t stored_len = bounded_len(chat_id_len);

    /* Update existing entry if present. */
    int slot = find_entry(r, chat_id, stored_len);

    if (slot < 0) {
        /* Need a new slot — prefer free, fall back to LRU eviction. */
        slot = find_free(r);
        if (slot < 0) {
            slot = find_lru(r);
        }
        /* Populate identity fields for the new occupant. */
        memcpy(r->entries[slot].chat_id, chat_id, stored_len);
        r->entries[slot].chat_id[stored_len] = '\0';
        r->entries[slot].chat_id_len = stored_len;
        r->entries[slot].in_use = true;
    }

    r->entries[slot].last_index = index;
    r->entries[slot].last_used_seq = ++r->next_seq;
}

int32_t hu_filler_recency_last(const hu_filler_recency_t *r, const char *chat_id,
                               size_t chat_id_len) {
    if (!r || !chat_id) {
        return -1;
    }

    size_t stored_len = bounded_len(chat_id_len);

    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0) {
        return -1;
    }
    return (int32_t)r->entries[slot].last_index;
}
