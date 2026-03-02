#ifndef SC_CHANNEL_H
#define SC_CHANNEL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Channel types — messaging, policy
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_outbound_stage {
    SC_OUTBOUND_STAGE_CHUNK,
    SC_OUTBOUND_STAGE_FINAL,
} sc_outbound_stage_t;

typedef enum sc_dm_policy {
    SC_DM_POLICY_ALLOW,
    SC_DM_POLICY_DENY,
    SC_DM_POLICY_ALLOWLIST,
} sc_dm_policy_t;

typedef enum sc_group_policy {
    SC_GROUP_POLICY_OPEN,
    SC_GROUP_POLICY_MENTION_ONLY,
    SC_GROUP_POLICY_ALLOWLIST,
} sc_group_policy_t;

typedef struct sc_channel_policy {
    sc_dm_policy_t dm;
    sc_group_policy_t group;
    const char *const *allowlist; /* NULL-terminated or use allowlist_count */
    size_t allowlist_count;       /* 0 if allowlist is NULL */
} sc_channel_policy_t;

typedef struct sc_channel_message {
    const char *id;
    size_t id_len;
    const char *sender;
    size_t sender_len;
    const char *content;
    size_t content_len;
    const char *channel;
    size_t channel_len;
    uint64_t timestamp;
    const char *reply_target;   /* optional, NULL if none */
    size_t reply_target_len;     /* 0 if reply_target is NULL */
    int64_t message_id;         /* platform message ID, -1 if none */
    const char *first_name;     /* optional, sender's first name */
    size_t first_name_len;
    bool is_group;
    const char *sender_uuid;   /* optional, Signal privacy mode */
    size_t sender_uuid_len;
    const char *group_id;      /* optional, Signal group chats */
    size_t group_id_len;
} sc_channel_message_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_channel_vtable;

typedef struct sc_channel {
    void *ctx;
    const struct sc_channel_vtable *vtable;
} sc_channel_t;

/* media: array of const char* URLs; media_count 0 if none */
typedef struct sc_channel_vtable {
    sc_error_t (*start)(void *ctx);
    void (*stop)(void *ctx);
    sc_error_t (*send)(void *ctx,
        const char *target, size_t target_len,
        const char *message, size_t message_len,
        const char *const *media, size_t media_count);
    const char *(*name)(void *ctx);
    bool (*health_check)(void *ctx);

    /* Optional — may be NULL. If send_event is NULL, runtime uses send() for final. */
    sc_error_t (*send_event)(void *ctx,
        const char *target, size_t target_len,
        const char *message, size_t message_len,
        const char *const *media, size_t media_count,
        sc_outbound_stage_t stage);
    sc_error_t (*start_typing)(void *ctx, const char *recipient, size_t recipient_len);
    sc_error_t (*stop_typing)(void *ctx, const char *recipient, size_t recipient_len);
} sc_channel_vtable_t;

#endif /* SC_CHANNEL_H */
