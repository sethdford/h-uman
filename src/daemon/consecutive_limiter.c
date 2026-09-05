/* Per-contact consecutive-reply limiter — see include/human/daemon/consecutive_limiter.h. */
#include "human/daemon/consecutive_limiter.h"
#include "human/daemon/reactive_gates.h"

#include <string.h>

static hu_consec_slot_t *find_slot(hu_consec_limiter_t *l, const char *key, size_t key_len) {
    if (!l || !key || key_len == 0 || key_len >= HU_CONSEC_KEY_MAX)
        return NULL;
    for (size_t i = 0; i < l->used; i++) {
        hu_consec_slot_t *s = &l->slots[i];
        if (s->key_len == key_len && memcmp(s->key, key, key_len) == 0)
            return s;
    }
    return NULL;
}

void hu_consec_limiter_init(hu_consec_limiter_t *l) {
    if (l)
        memset(l, 0, sizeof(*l));
}

void hu_consec_limiter_note_reply(hu_consec_limiter_t *l, const char *key, size_t key_len,
                                  int64_t now_unix) {
    hu_consec_slot_t *s = find_slot(l, key, key_len);
    if (!s) {
        if (!l || !key || key_len == 0 || key_len >= HU_CONSEC_KEY_MAX ||
            l->used >= HU_CONSEC_MAX_CONTACTS)
            return;
        s = &l->slots[l->used++];
        memset(s, 0, sizeof(*s));
        memcpy(s->key, key, key_len);
        s->key_len = key_len;
    }
    if (s->count < UINT32_MAX)
        s->count++;
    s->last_reply_unix = now_unix;
}

void hu_consec_limiter_reset(hu_consec_limiter_t *l, const char *key, size_t key_len) {
    hu_consec_slot_t *s = find_slot(l, key, key_len);
    if (s)
        s->count = 0;
}

bool hu_consec_limiter_should_silence(hu_consec_limiter_t *l, const char *key, size_t key_len,
                                      uint32_t cap, uint32_t reset_secs, int64_t now_unix,
                                      uint32_t *out_count) {
    hu_consec_slot_t *s = find_slot(l, key, key_len);
    if (!s) {
        if (out_count)
            *out_count = 0;
        return false;
    }
    if (hu_reactive_consecutive_burst_expired(s->last_reply_unix, now_unix, reset_secs))
        s->count = 0;
    if (out_count)
        *out_count = s->count;
    return hu_reactive_consecutive_limit_reached(s->count, cap);
}

uint32_t hu_consec_limiter_count(const hu_consec_limiter_t *l, const char *key, size_t key_len) {
    hu_consec_slot_t *s = find_slot((hu_consec_limiter_t *)l, key, key_len);
    return s ? s->count : 0;
}
