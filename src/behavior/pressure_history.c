#include "human/behavior/pressure_history.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Threshold for "this user message is a reassertion of an earlier one."
 *
 * We compute max(Jaccard, overlap_coefficient) and check against this number.
 * The overlap term catches the common reassertion pattern where the user
 * adds emphasis around the same claim (e.g. "Berlin is in France" → "I'm
 * telling you, Berlin is in France"). A tighter threshold (≥ 0.5) keeps
 * unrelated short messages from triggering false positives. */
#define PH_SIM_THRESHOLD 0.5f

void hu_pressure_history_init(hu_pressure_history_t *h) {
    if (!h) {
        return;
    }
    memset(h, 0, sizeof(*h));
}

/* Normalize: lowercase ASCII letters/digits; collapse runs of any other
 * character to a single space; trim. Caller-provided buffer is sized at
 * HU_PRESSURE_HISTORY_MSG_BYTES. */
static size_t ph_normalize(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    if (!src || src_len == 0 || !dst || dst_cap == 0) {
        if (dst && dst_cap > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    size_t out = 0;
    int prev_space = 1; /* start with space-state to trim leading */
    for (size_t i = 0; i < src_len && out + 1 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            dst[out++] = (char)c;
            prev_space = 0;
        } else if (c >= 'A' && c <= 'Z') {
            dst[out++] = (char)(c + 32);
            prev_space = 0;
        } else if (!prev_space) {
            dst[out++] = ' ';
            prev_space = 1;
        }
    }
    while (out > 0 && dst[out - 1] == ' ') {
        out--;
    }
    dst[out] = '\0';
    return out;
}

/* Trigram similarity over normalized strings. Returns the larger of
 * Jaccard and overlap coefficient, so we catch both "two messages share
 * most of their content" (Jaccard) and "the shorter message is contained
 * in the longer one" (overlap). Returns 0..1. */
static float ph_trigram_similarity(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len < 3 || b_len < 3) {
        /* For very short strings, fall back to exact match. */
        if (a_len == b_len && a_len > 0 && memcmp(a, b, a_len) == 0) {
            return 1.0f;
        }
        return 0.0f;
    }

    /* Bit-set across all printable ASCII trigrams (24-bit space). Allocating
     * a 16M-bit set is too much; use two compact 256-bucket presence vectors
     * keyed by FNV-1a (mod 4093 — prime). Collisions inflate similarity
     * slightly but stay below the threshold for unrelated text. */
    enum { PH_BUCKETS = 4093 };
    uint8_t in_a[PH_BUCKETS];
    uint8_t in_b[PH_BUCKETS];
    memset(in_a, 0, sizeof(in_a));
    memset(in_b, 0, sizeof(in_b));

    size_t a_total = 0;
    for (size_t i = 0; i + 3 <= a_len; i++) {
        uint32_t h = 2166136261u;
        h = (h ^ (uint8_t)a[i]) * 16777619u;
        h = (h ^ (uint8_t)a[i + 1]) * 16777619u;
        h = (h ^ (uint8_t)a[i + 2]) * 16777619u;
        size_t k = (size_t)(h % PH_BUCKETS);
        if (!in_a[k]) {
            in_a[k] = 1;
            a_total++;
        }
    }
    size_t b_total = 0;
    for (size_t i = 0; i + 3 <= b_len; i++) {
        uint32_t h = 2166136261u;
        h = (h ^ (uint8_t)b[i]) * 16777619u;
        h = (h ^ (uint8_t)b[i + 1]) * 16777619u;
        h = (h ^ (uint8_t)b[i + 2]) * 16777619u;
        size_t k = (size_t)(h % PH_BUCKETS);
        if (!in_b[k]) {
            in_b[k] = 1;
            b_total++;
        }
    }
    if (a_total == 0 || b_total == 0) {
        return 0.0f;
    }
    size_t inter = 0;
    size_t uni = 0;
    for (size_t k = 0; k < PH_BUCKETS; k++) {
        if (in_a[k] || in_b[k]) {
            uni++;
            if (in_a[k] && in_b[k]) {
                inter++;
            }
        }
    }
    if (uni == 0) {
        return 0.0f;
    }
    float jaccard = (float)inter / (float)uni;
    size_t smaller = a_total < b_total ? a_total : b_total;
    float overlap = smaller > 0 ? (float)inter / (float)smaller : 0.0f;
    return jaccard > overlap ? jaccard : overlap;
}

void hu_pressure_history_observe(hu_pressure_history_t *h, uint32_t turn_index,
                                 const char *user_message, size_t user_message_len,
                                 hu_trust_action_t last_action) {
    if (!h) {
        return;
    }
    hu_pressure_entry_t *e = &h->entries[h->head % HU_PRESSURE_HISTORY_CAP];
    e->turn_index = turn_index;
    e->last_action = last_action;
    e->normalized_len = ph_normalize(user_message, user_message_len, e->normalized,
                                     sizeof(e->normalized));
    h->head = (h->head + 1) % HU_PRESSURE_HISTORY_CAP;
    if (h->count < (size_t)UINT32_MAX) {
        h->count++;
    }
}

hu_error_t hu_pressure_history_inspect(const hu_pressure_history_t *h, const char *user_message,
                                       size_t user_message_len,
                                       bool *out_reasserted_after_pushback,
                                       uint32_t *out_reassertion_count) {
    if (out_reasserted_after_pushback) {
        *out_reasserted_after_pushback = false;
    }
    if (out_reassertion_count) {
        *out_reassertion_count = 0;
    }
    if (!h || !user_message || user_message_len == 0) {
        return HU_OK;
    }

    char norm[HU_PRESSURE_HISTORY_MSG_BYTES];
    size_t norm_len = ph_normalize(user_message, user_message_len, norm, sizeof(norm));
    if (norm_len < 3) {
        return HU_OK;
    }

    size_t available = h->count < HU_PRESSURE_HISTORY_CAP ? h->count : HU_PRESSURE_HISTORY_CAP;
    uint32_t reasserts = 0;
    bool after_pushback = false;
    for (size_t i = 0; i < available; i++) {
        const hu_pressure_entry_t *e = &h->entries[i];
        if (e->normalized_len < 3) {
            continue;
        }
        float sim = ph_trigram_similarity(norm, norm_len, e->normalized, e->normalized_len);
        if (sim >= PH_SIM_THRESHOLD) {
            reasserts++;
            if (e->last_action == HU_TRUST_PUSH_BACK ||
                e->last_action == HU_TRUST_REFUSE_TO_AGREE) {
                after_pushback = true;
            }
        }
    }
    if (out_reassertion_count) {
        *out_reassertion_count = reasserts;
    }
    if (out_reasserted_after_pushback) {
        *out_reasserted_after_pushback = after_pushback;
    }
    return HU_OK;
}

void hu_pressure_history_apply_to_trust_input(const hu_pressure_history_t *h,
                                              const char *user_message,
                                              size_t user_message_len,
                                              hu_trust_input_t *trust_in) {
    if (!h || !trust_in) {
        return;
    }
    bool after = false;
    uint32_t count = 0;
    (void)hu_pressure_history_inspect(h, user_message, user_message_len, &after, &count);
    if (after) {
        trust_in->user_reasserted_after_pushback = true;
    }
    if (count > 0) {
        uint64_t bumped = (uint64_t)trust_in->user_pressure_count + (uint64_t)count;
        if (bumped > UINT32_MAX) {
            bumped = UINT32_MAX;
        }
        trust_in->user_pressure_count = (uint32_t)bumped;
    }
}
