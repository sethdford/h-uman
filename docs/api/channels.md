---
title: Channel API
description: sc_channel_t vtable for CLI, Telegram, Discord, Slack, and other channels
updated: 2026-03-02
---

# Channel API

Channels implement the `sc_channel_t` vtable for inbound/outbound messaging (CLI, Telegram, Discord, Slack, etc.).

## Types

### Policy

```c
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
    const char *const *allowlist;
    size_t allowlist_count;
} sc_channel_policy_t;
```

### Message

```c
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
} sc_channel_message_t;
```

### Channel Vtable

```c
typedef struct sc_channel {
    void *ctx;
    const struct sc_channel_vtable *vtable;
} sc_channel_t;

typedef struct sc_channel_vtable {
    sc_error_t (*start)(void *ctx);
    void (*stop)(void *ctx);
    sc_error_t (*send)(void *ctx,
        const char *target, size_t target_len,
        const char *message, size_t message_len,
        const char *const *media, size_t media_count);
    const char *(*name)(void *ctx);
    bool (*health_check)(void *ctx);

    /* Optional — may be NULL */
    sc_error_t (*send_event)(void *ctx,
        const char *target, size_t target_len,
        const char *message, size_t message_len,
        const char *const *media, size_t media_count,
        sc_outbound_stage_t stage);
    sc_error_t (*start_typing)(void *ctx, const char *recipient, size_t recipient_len);
    sc_error_t (*stop_typing)(void *ctx, const char *recipient, size_t recipient_len);
} sc_channel_vtable_t;
```

## Channel Manager

```c
typedef enum sc_channel_listener_type {
    SC_CHANNEL_LISTENER_POLLING,
    SC_CHANNEL_LISTENER_GATEWAY,
    SC_CHANNEL_LISTENER_WEBHOOK,
    SC_CHANNEL_LISTENER_SEND_ONLY,
    SC_CHANNEL_LISTENER_NONE,
} sc_channel_listener_type_t;

typedef struct sc_channel_entry {
    const char *name;
    const char *account_id;
    sc_channel_t channel;
    sc_channel_listener_type_t listener_type;
} sc_channel_entry_t;

sc_error_t sc_channel_manager_init(sc_channel_manager_t *mgr, sc_allocator_t *alloc);
void sc_channel_manager_deinit(sc_channel_manager_t *mgr);
void sc_channel_manager_set_bus(sc_channel_manager_t *mgr, sc_bus_t *bus);

sc_error_t sc_channel_manager_register(sc_channel_manager_t *mgr,
    const char *name, const char *account_id,
    const sc_channel_t *channel,
    sc_channel_listener_type_t listener_type);

sc_error_t sc_channel_manager_start_all(sc_channel_manager_t *mgr);
void sc_channel_manager_stop_all(sc_channel_manager_t *mgr);

const sc_channel_entry_t *sc_channel_manager_entries(const sc_channel_manager_t *mgr, size_t *out_count);
size_t sc_channel_manager_count(const sc_channel_manager_t *mgr);
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_channel_manager_t mgr;
sc_channel_manager_init(&mgr, &alloc);

sc_channel_t cli;
sc_cli_create(&alloc, &cli);

sc_channel_manager_register(&mgr, "cli", "default", &cli, SC_CHANNEL_LISTENER_NONE);
sc_channel_manager_start_all(&mgr);

/* send via channel */
sc_channel_entry_t *e = (sc_channel_entry_t *)mgr.entries;
e[0].channel.vtable->send(e[0].channel.ctx, "user", 4, "Hello!", 6, NULL, 0);

sc_channel_manager_stop_all(&mgr);
sc_cli_destroy(&cli);
sc_channel_manager_deinit(&mgr);
```

## Channel Creation

Channels have module-specific factory functions, e.g.:

- `sc_cli_create` — CLI channel
- `sc_telegram_create` — Telegram (requires `SC_ENABLE_TELEGRAM`)
- `sc_discord_create` — Discord
- `sc_dispatch_create` — dispatch to multiple channels

See `src/channels/` for implementations.

## Thread Safety

- Channel manager is not thread-safe.
- `start` / `stop` / `send` may be called from different threads depending on listener type.
