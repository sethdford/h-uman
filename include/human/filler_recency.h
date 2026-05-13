#ifndef HU_FILLER_RECENCY_H
#define HU_FILLER_RECENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_FILLER_RECENCY_CHAT_ID_MAX 128
#define HU_FILLER_RECENCY_CAPACITY    32

typedef struct {
    char chat_id[HU_FILLER_RECENCY_CHAT_ID_MAX];
    size_t chat_id_len;
    uint16_t last_index;
    uint64_t last_used_seq;
    bool in_use;
} hu_filler_recency_entry_t;

typedef struct {
    hu_filler_recency_entry_t entries[HU_FILLER_RECENCY_CAPACITY];
    uint64_t next_seq;
} hu_filler_recency_t;

/* Record that filler at `index` was emitted for `chat_id` in `r`.
 * If `r` is NULL or `chat_id` is NULL, returns without error.
 * Overwrites the existing entry for that chat if present; otherwise
 * claims a free slot or evicts the least-recently-used entry. */
void hu_filler_recency_record(hu_filler_recency_t *r, const char *chat_id, size_t chat_id_len,
                              uint16_t index);

/* Return the last emitted filler index for `chat_id`, or -1 if none
 * has been recorded.  Returns -1 for NULL arguments. */
int32_t hu_filler_recency_last(const hu_filler_recency_t *r, const char *chat_id,
                               size_t chat_id_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_FILLER_RECENCY_H */
