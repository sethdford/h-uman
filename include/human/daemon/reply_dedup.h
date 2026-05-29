#ifndef HU_DAEMON_REPLY_DEDUP_H
#define HU_DAEMON_REPLY_DEDUP_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Outbound reply idempotency (BUG #2).
 *
 * The iMessage poll watermark (last_rowid) is persisted only at the end of a
 * poll batch, AFTER replies are sent. If the daemon crashes after vtable->send
 * but before the watermark is persisted, the same inbound is re-polled on
 * restart and re-replied — a duplicate message to the contact. The inbound
 * echo `sent_ring` does NOT cover this (it filters inbound echoes only).
 *
 * This store records, per contact (chat_id), the highest inbound message rowid
 * we have ALREADY replied to. It is updated + persisted immediately after a
 * successful reactive send, and loaded at daemon startup, so a replayed inbound
 * is recognized and skipped. Semantics are at-least-once + dedup-on-replay:
 * we persist only AFTER send confirms success, so a crash BEFORE send never
 * suppresses a reply (no dropped replies), and a crash AFTER send is
 * de-duplicated on restart.
 *
 * The decision is the pure predicate hu_daemon_already_replied, extracted per
 * .claude/rules/security-predicate-extraction.md and pinned by
 * tests/test_reply_dedup.c. Mirrors the fixed-capacity LRU shape of
 * hu_contact_send_recency_t. */

#define HU_REPLY_DEDUP_CHAT_ID_MAX 128
#define HU_REPLY_DEDUP_CAPACITY    128

typedef struct {
    char chat_id[HU_REPLY_DEDUP_CHAT_ID_MAX];
    size_t chat_id_len;
    int64_t last_replied_rowid; /* highest inbound rowid replied to for this chat */
    uint64_t last_used_seq;     /* for LRU eviction */
    bool in_use;
} hu_reply_dedup_entry_t;

typedef struct {
    hu_reply_dedup_entry_t entries[HU_REPLY_DEDUP_CAPACITY];
    uint64_t next_seq;
} hu_reply_dedup_t;

/* Record that we replied to inbound `rowid` for `chat_id`. The stored value is
 * monotonic — it only ever increases, so an out-of-order replay of an older
 * rowid cannot lower the watermark. Claims a free slot or evicts the LRU entry.
 * No-op on NULL args or rowid <= 0. */
void hu_reply_dedup_record(hu_reply_dedup_t *r, const char *chat_id, size_t chat_id_len,
                           int64_t rowid);

/* Pure predicate: returns true iff we have ALREADY replied to inbound `rowid`
 * (or a newer one) for `chat_id` — i.e. the recorded watermark for that contact
 * is >= rowid. Returns false for NULL args, a non-positive rowid, or a contact
 * with nothing recorded (so a brand-new inbound always proceeds). */
bool hu_daemon_already_replied(const hu_reply_dedup_t *r, const char *chat_id, size_t chat_id_len,
                               int64_t rowid);

/* Persist the store to `path` (JSON, one entry per line). Atomic: writes to
 * <path>.tmp, fsyncs, then renames over `path`, so a crash mid-write leaves the
 * prior file intact. Returns HU_OK on success. A store with no entries writes
 * an empty array. */
hu_error_t hu_reply_dedup_save(const hu_reply_dedup_t *r, const char *path, size_t path_len);

/* Load the store from `path`, replacing current contents. Missing file is not
 * an error for the caller's purposes but returns HU_ERR_IO (caller treats an
 * absent file as "empty store"). Malformed lines are skipped. */
hu_error_t hu_reply_dedup_load(hu_reply_dedup_t *r, const char *path, size_t path_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_REPLY_DEDUP_H */
