---
title: Channel API
description: hu_channel_t vtable for CLI, Telegram, Discord, Slack, and other channels
updated: 2026-03-02
---

# Channel API

Channels implement the `hu_channel_t` vtable for inbound/outbound messaging (CLI, Telegram, Discord, Slack, etc.).

## Types

### Policy

```c
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
    const char *const *allowlist;
    size_t allowlist_count;
} hu_channel_policy_t;
```

### Message

```c
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
    const char *reply_target;
    size_t reply_target_len;
    int64_t message_id;
    const char *first_name;
    size_t first_name_len;
    bool is_group;
    const char *sender_uuid;
    size_t sender_uuid_len;
    const char *group_id;
    size_t group_id_len;
} hu_channel_message_t;
```

### Channel Vtable

```c
typedef struct hu_channel {
    void *ctx;
    const struct hu_channel_vtable *vtable;
} hu_channel_t;

typedef struct hu_channel_vtable {
    hu_error_t (*start)(void *ctx);
    void (*stop)(void *ctx);
    hu_error_t (*send)(void *ctx,
        const char *target, size_t target_len,
        const char *message, size_t message_len,
        const char *const *media, size_t media_count);
    const char *(*name)(void *ctx);
    bool (*health_check)(void *ctx);

    /* Optional — may be NULL */
    hu_error_t (*send_event)(void *ctx,
        const char *target, size_t target_len,
        const char *message, size_t message_len,
        const char *const *media, size_t media_count,
        hu_outbound_stage_t stage);
    hu_error_t (*start_typing)(void *ctx, const char *recipient, size_t recipient_len);
    hu_error_t (*stop_typing)(void *ctx, const char *recipient, size_t recipient_len);
} hu_channel_vtable_t;
```

## Channel Manager

```c
typedef enum hu_channel_listener_type {
    HU_CHANNEL_LISTENER_POLLING,
    HU_CHANNEL_LISTENER_GATEWAY,
    HU_CHANNEL_LISTENER_WEBHOOK,
    HU_CHANNEL_LISTENER_SEND_ONLY,
    HU_CHANNEL_LISTENER_NONE,
} hu_channel_listener_type_t;

typedef struct hu_channel_entry {
    const char *name;
    const char *account_id;
    hu_channel_t channel;
    hu_channel_listener_type_t listener_type;
} hu_channel_entry_t;

hu_error_t hu_channel_manager_init(hu_channel_manager_t *mgr, hu_allocator_t *alloc);
void hu_channel_manager_deinit(hu_channel_manager_t *mgr);
void hu_channel_manager_set_bus(hu_channel_manager_t *mgr, hu_bus_t *bus);

hu_error_t hu_channel_manager_register(hu_channel_manager_t *mgr,
    const char *name, const char *account_id,
    const hu_channel_t *channel,
    hu_channel_listener_type_t listener_type);

hu_error_t hu_channel_manager_start_all(hu_channel_manager_t *mgr);
void hu_channel_manager_stop_all(hu_channel_manager_t *mgr);

const hu_channel_entry_t *hu_channel_manager_entries(const hu_channel_manager_t *mgr, size_t *out_count);
size_t hu_channel_manager_count(const hu_channel_manager_t *mgr);
```

## Usage Example

```c
hu_allocator_t alloc = hu_system_allocator();
hu_channel_manager_t mgr;
hu_channel_manager_init(&mgr, &alloc);

hu_channel_t cli;
hu_cli_create(&alloc, &cli);

hu_channel_manager_register(&mgr, "cli", "default", &cli, HU_CHANNEL_LISTENER_NONE);
hu_channel_manager_start_all(&mgr);

/* send via channel */
hu_channel_entry_t *e = (hu_channel_entry_t *)mgr.entries;
e[0].channel.vtable->send(e[0].channel.ctx, "user", 4, "Hello!", 6, NULL, 0);

hu_channel_manager_stop_all(&mgr);
hu_cli_destroy(&cli);
hu_channel_manager_deinit(&mgr);
```

## Channel Creation

Channels have module-specific factory functions, e.g.:

- `hu_cli_create` — CLI channel
- `hu_telegram_create` — Telegram (requires `HU_ENABLE_TELEGRAM`)
- `hu_discord_create` — Discord
- `hu_dispatch_create` — dispatch to multiple channels

See `src/channels/` for implementations.

## Thread Safety

- Channel manager is not thread-safe.
- `start` / `stop` / `send` may be called from different threads depending on listener type.
