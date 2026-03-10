#ifndef HU_CONFIG_TYPES_H
#define HU_CONFIG_TYPES_H

#include "human/security/sandbox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_DEFAULT_AGENT_TOKEN_LIMIT 200000u
#define HU_DEFAULT_MODEL_MAX_TOKENS  8192u

typedef struct hu_cron_config {
    bool enabled;
    uint32_t interval_minutes;
    uint32_t max_run_history;
} hu_cron_config_t;

typedef struct hu_net_proxy_config {
    bool enabled;
    bool deny_all;
    char *proxy_addr;
    char **allowed_domains;
    size_t allowed_domains_len;
} hu_net_proxy_config_t;

typedef struct hu_sandbox_config {
    bool enabled;
    hu_sandbox_backend_t backend;
    char **firejail_args;
    size_t firejail_args_len;
    hu_net_proxy_config_t net_proxy;
} hu_sandbox_config_t;

typedef struct hu_resource_limits {
    uint64_t max_file_size;
    uint64_t max_read_size;
    uint32_t max_memory_mb;
} hu_resource_limits_t;

/* hu_audit_config_t is defined in human/security/audit.h */

typedef struct hu_scheduler_config {
    uint32_t max_concurrent;
} hu_scheduler_config_t;

typedef struct hu_behavior_config {
    uint32_t consecutive_limit;      /* max consecutive messages from self before skip (default 3) */
    uint32_t participation_pct;      /* max % of recent messages before skip (default 40) */
    uint32_t max_response_chars;     /* max response length (default 300) */
    uint32_t min_response_chars;     /* min response length (default 15) */
    uint32_t decay_days;             /* memory decay window in days (default 30) */
    uint32_t dedup_threshold;        /* memory dedup similarity % (default 70) */
    uint32_t missed_msg_threshold_sec; /* seconds before acknowledging missed message (default 1800) */
} hu_behavior_config_t;

typedef enum hu_dm_scope {
    DirectScopeMain,
    DirectScopePerPeer,
    DirectScopePerChannelPeer,
    DirectScopePerAccountChannelPeer,
} hu_dm_scope_t;

typedef struct hu_identity_link {
    const char *canonical;
    const char **peers;
    size_t peers_len;
} hu_identity_link_t;

typedef struct hu_named_agent_config {
    const char *name;
    const char *provider;
    const char *model;
} hu_named_agent_config_t;

typedef struct hu_session_config {
    hu_dm_scope_t dm_scope;
    uint32_t idle_minutes;
    const hu_identity_link_t *identity_links;
    size_t identity_links_len;
} hu_session_config_t;

#endif /* HU_CONFIG_TYPES_H */
