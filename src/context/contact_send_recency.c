#include "human/contact_send_recency.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static size_t bounded_len(size_t chat_id_len) {
    if (chat_id_len >= HU_CONTACT_SEND_RECENCY_CHAT_ID_MAX) {
        return HU_CONTACT_SEND_RECENCY_CHAT_ID_MAX - 1;
    }
    return chat_id_len;
}

static int find_entry(const hu_contact_send_recency_t *r, const char *chat_id, size_t stored_len) {
    for (int i = 0; i < HU_CONTACT_SEND_RECENCY_CAPACITY; ++i) {
        const hu_contact_send_recency_entry_t *e = &r->entries[i];
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

static int find_free(const hu_contact_send_recency_t *r) {
    for (int i = 0; i < HU_CONTACT_SEND_RECENCY_CAPACITY; ++i) {
        if (!r->entries[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int find_lru(const hu_contact_send_recency_t *r) {
    int lru_idx = 0;
    uint64_t lru_seq = r->entries[0].last_used_seq;
    for (int i = 1; i < HU_CONTACT_SEND_RECENCY_CAPACITY; ++i) {
        if (r->entries[i].last_used_seq < lru_seq) {
            lru_seq = r->entries[i].last_used_seq;
            lru_idx = i;
        }
    }
    return lru_idx;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void hu_contact_send_recency_record(hu_contact_send_recency_t *r, const char *chat_id,
                                    size_t chat_id_len, int64_t ts, hu_send_path_t path) {
    if (!r || !chat_id) {
        return;
    }

    size_t stored_len = bounded_len(chat_id_len);

    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0) {
        slot = find_free(r);
        if (slot < 0) {
            slot = find_lru(r);
        }
        memcpy(r->entries[slot].chat_id, chat_id, stored_len);
        r->entries[slot].chat_id[stored_len] = '\0';
        r->entries[slot].chat_id_len = stored_len;
        r->entries[slot].in_use = true;
    }

    r->entries[slot].last_send_ts = ts;
    r->entries[slot].last_path = path;
    r->entries[slot].last_used_seq = ++r->next_seq;
}

int64_t hu_contact_send_recency_last_ts(const hu_contact_send_recency_t *r, const char *chat_id,
                                        size_t chat_id_len) {
    if (!r || !chat_id) {
        return 0;
    }
    size_t stored_len = bounded_len(chat_id_len);
    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0) {
        return 0;
    }
    return r->entries[slot].last_send_ts;
}

hu_send_path_t hu_contact_send_recency_last_path(const hu_contact_send_recency_t *r,
                                                 const char *chat_id, size_t chat_id_len) {
    if (!r || !chat_id) {
        return HU_SEND_PATH_NONE;
    }
    size_t stored_len = bounded_len(chat_id_len);
    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0) {
        return HU_SEND_PATH_NONE;
    }
    return r->entries[slot].last_path;
}

bool hu_contact_send_recency_reactive_within(const hu_contact_send_recency_t *r,
                                             const char *chat_id, size_t chat_id_len, int64_t now,
                                             int64_t window_s) {
    if (!r || !chat_id || window_s <= 0) {
        return false;
    }
    size_t stored_len = bounded_len(chat_id_len);
    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0) {
        return false;
    }
    const hu_contact_send_recency_entry_t *e = &r->entries[slot];
    if (e->last_path != HU_SEND_PATH_REACTIVE) {
        return false;
    }
    return (now - e->last_send_ts) >= 0 && (now - e->last_send_ts) < window_s;
}

bool hu_daemon_proactive_should_defer(const hu_contact_send_recency_t *r, const char *chat_id,
                                      size_t chat_id_len, int64_t now) {
    return hu_contact_send_recency_reactive_within(r, chat_id, chat_id_len, now,
                                                   HU_DAEMON_REACTIVE_GATE_WINDOW_S);
}
