#ifndef SC_CONFIG_TYPES_H
#define SC_CONFIG_TYPES_H

#include "seaclaw/security/sandbox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_DEFAULT_AGENT_TOKEN_LIMIT 200000u
#define SC_DEFAULT_MODEL_MAX_TOKENS   8192u

typedef struct sc_cron_config {
    bool enabled;
    uint32_t interval_minutes;
    uint32_t max_run_history;
} sc_cron_config_t;

typedef struct sc_net_proxy_config {
    bool enabled;
    bool deny_all;
    char *proxy_addr;
    char **allowed_domains;
    size_t allowed_domains_len;
} sc_net_proxy_config_t;

typedef struct sc_sandbox_config {
    bool enabled;
    sc_sandbox_backend_t backend;
    char **firejail_args;
    size_t firejail_args_len;
    sc_net_proxy_config_t net_proxy;
} sc_sandbox_config_t;

typedef struct sc_resource_limits {
    uint64_t max_file_size;
    uint64_t max_read_size;
    uint32_t max_memory_mb;
} sc_resource_limits_t;

typedef struct sc_audit_config {
    bool enabled;
    char *log_path;
} sc_audit_config_t;

typedef struct sc_scheduler_config {
    uint32_t max_concurrent;
} sc_scheduler_config_t;

typedef enum sc_dm_scope {
    DirectScopeMain,
    DirectScopePerPeer,
    DirectScopePerChannelPeer,
    DirectScopePerAccountChannelPeer,
} sc_dm_scope_t;

typedef struct sc_identity_link {
    const char *canonical;
    const char **peers;
    size_t peers_len;
} sc_identity_link_t;

typedef struct sc_named_agent_config {
    const char *name;
    const char *provider;
    const char *model;
} sc_named_agent_config_t;

typedef struct sc_session_config {
    sc_dm_scope_t dm_scope;
    uint32_t idle_minutes;
    const sc_identity_link_t *identity_links;
    size_t identity_links_len;
} sc_session_config_t;

#endif /* SC_CONFIG_TYPES_H */
