#ifndef HU_CHANNEL_H
#define HU_CHANNEL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Channel types — messaging, policy
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_outbound_stage {
    HU_OUTBOUND_STAGE_CHUNK,
    HU_OUTBOUND_STAGE_FINAL,
} hu_outbound_stage_t;

typedef enum hu_dm_policy {
    HU_DM_POLICY_ALLOW,
    HU_DM_POLICY_DENY,
    HU_DM_POLICY_ALLOWLIST,
} hu_dm_policy_t;

typedef enum hu_group_policy {
    HU_GROUP_POLICY_OPEN,
    HU_GROUP_POLICY_MENTION_ONLY,
    HU_GROUP_POLICY_ALLOWLIST,
} hu_group_policy_t;

typedef struct hu_channel_policy {
    hu_dm_policy_t dm;
    hu_group_policy_t group;
    const char *const *allowlist; /* NULL-terminated or use allowlist_count */
    size_t allowlist_count;       /* 0 if allowlist is NULL */
} hu_channel_policy_t;

typedef struct hu_channel_message {
    const char *id;
    size_t id_len;
    const char *sender;
    size_t sender_len;
    const char *content;
    size_t content_len;
    const char *channel;
    size_t channel_len;
    uint64_t timestamp;
    const char *reply_target; /* optional, NULL if none */
    size_t reply_target_len;  /* 0 if reply_target is NULL */
    int64_t message_id;       /* platform message ID, -1 if none */
    const char *first_name;   /* optional, sender's first name */
    size_t first_name_len;
    bool is_group;
    const char *sender_uuid; /* optional, Signal privacy mode */
    size_t sender_uuid_len;
    const char *group_id; /* optional, Signal group chats */
    size_t group_id_len;
} hu_channel_message_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel conversation history entry (for load_conversation_history)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_channel_history_entry {
    bool from_me;
    char text[512];
    char timestamp[32];
} hu_channel_history_entry_t;

typedef struct hu_channel_response_constraints {
    uint32_t max_chars; /* 0 = unlimited */
} hu_channel_response_constraints_t;

typedef enum hu_reaction_type {
    HU_REACTION_NONE = 0,
    HU_REACTION_HEART,
    HU_REACTION_THUMBS_UP,
    HU_REACTION_THUMBS_DOWN,
    HU_REACTION_HAHA,
    HU_REACTION_EMPHASIS, /* !! */
    HU_REACTION_QUESTION, /* ? */
} hu_reaction_type_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_channel_vtable;

typedef struct hu_channel {
    void *ctx;
    const struct hu_channel_vtable *vtable;
} hu_channel_t;

/* media: array of const char* URLs; media_count 0 if none */
typedef struct hu_channel_vtable {
    hu_error_t (*start)(void *ctx);
    void (*stop)(void *ctx);
    hu_error_t (*send)(void *ctx, const char *target, size_t target_len, const char *message,
                       size_t message_len, const char *const *media, size_t media_count);
    const char *(*name)(void *ctx);
    bool (*health_check)(void *ctx);

    /* Optional — may be NULL. If send_event is NULL, runtime uses send() for final. */
    hu_error_t (*send_event)(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count,
                             hu_outbound_stage_t stage);
    hu_error_t (*start_typing)(void *ctx, const char *recipient, size_t recipient_len);
    hu_error_t (*stop_typing)(void *ctx, const char *recipient, size_t recipient_len);

    /* Optional — load native conversation history from the channel's own data store.
     * Returns entries in chronological order (oldest first). Caller owns entries array.
     * NULL = channel does not support history loading. */
    hu_error_t (*load_conversation_history)(void *ctx, hu_allocator_t *alloc,
                                            const char *contact_id, size_t contact_id_len,
                                            size_t limit, hu_channel_history_entry_t **out,
                                            size_t *out_count);

    /* Optional — return per-channel response constraints (max length, etc.).
     * NULL = no constraints. */
    hu_error_t (*get_response_constraints)(void *ctx, hu_channel_response_constraints_t *out);

    /* Optional — send a reaction to a specific message.
     * message_id is the platform message ID (e.g., ROWID for iMessage).
     * NULL = channel does not support reactions. */
    hu_error_t (*react)(void *ctx, const char *target, size_t target_len, int64_t message_id,
                        hu_reaction_type_t reaction);
} hu_channel_vtable_t;

#endif /* HU_CHANNEL_H */
