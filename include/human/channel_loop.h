#ifndef HU_CHANNEL_LOOP_H
#define HU_CHANNEL_LOOP_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Poll callback — returns messages; caller frees. The daemon drives each
 * channel's poll itself; these are the shared message types it uses.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_channel_loop_msg {
    char session_key[128];
    char content[4096];
    bool is_group;
    int64_t message_id;  /* platform message ID for reactions; -1 if unknown */
    bool has_attachment; /* true if message has image or audio attachment (vision/transcription) */
    bool has_video;      /* true if message has video attachment (.mov, .mp4, .m4v) */
    char guid[96];       /* iMessage message GUID for inline reply tracking */
    bool was_edited;     /* message was edited after initial send (iMessage) */
    char reply_to_guid[96]; /* thread_originator_guid: message this is a reply to */
    bool was_unsent;        /* message was retracted/unsent (iMessage, date_retracted > 0) */
    int64_t timestamp_sec;  /* message origin time (Unix epoch); 0 = use poll time */
    char chat_id[128];      /* thread/chat identifier (e.g. iMessage chat.guid for groups) */
} hu_channel_loop_msg_t;

typedef hu_error_t (*hu_channel_loop_poll_fn)(void *channel_ctx, hu_allocator_t *alloc,
                                              hu_channel_loop_msg_t *msgs, size_t max_msgs,
                                              size_t *out_count);

#endif /* HU_CHANNEL_LOOP_H */
