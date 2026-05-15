#ifndef HU_CONTACT_SEND_RECENCY_H
#define HU_CONTACT_SEND_RECENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_CONTACT_SEND_RECENCY_CHAT_ID_MAX 128
#define HU_CONTACT_SEND_RECENCY_CAPACITY    64

/* Identifies which daemon-level send path produced an outbound message.
 * Used by the reactive-priority gate: when the reactive path has fired
 * recently for a contact, proactive paths defer to avoid piling on. */
typedef enum {
    HU_SEND_PATH_NONE = 0,      /* no record */
    HU_SEND_PATH_REACTIVE = 1,  /* reactive turn (response to incoming) */
    HU_SEND_PATH_PROACTIVE = 2, /* proactive check-in */
    HU_SEND_PATH_PHOTO = 3,     /* proactive photo album */
    HU_SEND_PATH_MORNING = 4,   /* scheduled morning message */
    HU_SEND_PATH_SCHEDULED = 5, /* generic scheduler delivery */
    HU_SEND_PATH_OTHER = 99,
} hu_send_path_t;

typedef struct {
    char chat_id[HU_CONTACT_SEND_RECENCY_CHAT_ID_MAX];
    size_t chat_id_len;
    int64_t last_send_ts; /* epoch seconds; 0 if never */
    hu_send_path_t last_path;
    uint64_t last_used_seq; /* for LRU eviction */
    bool in_use;
} hu_contact_send_recency_entry_t;

typedef struct {
    hu_contact_send_recency_entry_t entries[HU_CONTACT_SEND_RECENCY_CAPACITY];
    uint64_t next_seq;
} hu_contact_send_recency_t;

/* Record that path `path` sent to `chat_id` at epoch second `ts`.
 * Overwrites the existing entry for that chat if present; otherwise
 * claims a free slot or evicts the least-recently-used entry. */
void hu_contact_send_recency_record(hu_contact_send_recency_t *r, const char *chat_id,
                                    size_t chat_id_len, int64_t ts, hu_send_path_t path);

/* Returns the epoch second of the last send to `chat_id`, or 0 if none.
 * Returns 0 for NULL arguments. */
int64_t hu_contact_send_recency_last_ts(const hu_contact_send_recency_t *r, const char *chat_id,
                                        size_t chat_id_len);

/* Returns the path that last sent to `chat_id`, or HU_SEND_PATH_NONE if none. */
hu_send_path_t hu_contact_send_recency_last_path(const hu_contact_send_recency_t *r,
                                                 const char *chat_id, size_t chat_id_len);

/* Convenience: returns true if the reactive path sent to `chat_id` within
 * the last `window_s` seconds (relative to `now`). False otherwise.
 * This is the gate predicate used by proactive paths. */
bool hu_contact_send_recency_reactive_within(const hu_contact_send_recency_t *r,
                                             const char *chat_id, size_t chat_id_len, int64_t now,
                                             int64_t window_s);

#ifdef __cplusplus
}
#endif

#endif /* HU_CONTACT_SEND_RECENCY_H */
