#ifndef SC_CONFIG_H
#define SC_CONFIG_H

#include "seaclaw/config_types.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/slice.h"
#include "seaclaw/security/audit.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sc_provider_entry {
    char *name;
    char *api_key;
    char *base_url;
    bool native_tools;
    bool ws_streaming;
} sc_provider_entry_t;

typedef struct sc_diagnostics_config {
    char *backend;
    char *otel_endpoint;
    char *otel_service_name;
    bool log_tool_calls;
    bool log_message_receipts;
    bool log_message_payloads;
    bool log_llm_io;
} sc_diagnostics_config_t;

typedef struct sc_autonomy_config {
    char *level;
    bool workspace_only;
    uint32_t max_actions_per_hour;
    bool require_approval_for_medium_risk;
    bool block_high_risk_commands;
    char **allowed_commands;
    size_t allowed_commands_len;
    char **allowed_paths;
    size_t allowed_paths_len;
} sc_autonomy_config_t;

typedef struct sc_runtime_config {
    char *kind;
    char *docker_image;
    char *gce_project;
    char *gce_zone;
    char *gce_instance;
} sc_runtime_config_t;

typedef struct sc_reliability_config {
    char *primary_provider; /* used when default_provider is "reliable" */
    uint32_t provider_retries;
    uint64_t provider_backoff_ms;
    uint64_t channel_initial_backoff_secs;
    uint64_t channel_max_backoff_secs;
    uint64_t scheduler_poll_secs;
    uint32_t scheduler_retries;
    char **fallback_providers;
    size_t fallback_providers_len;
} sc_reliability_config_t;

typedef struct sc_router_config {
    char *fast;          /* provider name for simple tasks */
    char *standard;      /* provider name for default */
    char *powerful;      /* provider name for complex tasks */
    int complexity_low;  /* below this -> fast (default 50) */
    int complexity_high; /* above this -> powerful (default 500) */
} sc_router_config_t;

typedef struct sc_persona_channel_entry {
    char *channel;
    char *persona;
} sc_persona_channel_entry_t;

typedef struct sc_agent_config {
    bool compact_context;
    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    bool parallel_tools;
    char *tool_dispatcher;
    uint64_t token_limit;
    uint64_t session_idle_timeout_secs;
    uint32_t compaction_keep_recent;
    uint32_t compaction_max_summary_chars;
    uint32_t compaction_max_source_chars;
    uint64_t message_timeout_secs;
    uint32_t pool_max_concurrent;
    char *default_profile;
    char *persona;
    sc_persona_channel_entry_t *persona_channels;
    size_t persona_channels_count;
    sc_persona_channel_entry_t *persona_contacts;
    size_t persona_contacts_count;
    float context_pressure_warn;    /* warn at this ratio (default 0.85) */
    float context_pressure_compact; /* auto-compact at this ratio (default 0.95) */
    float context_compact_target;   /* compact until below this ratio (default 0.70) */
} sc_agent_config_t;

typedef struct sc_policy_config {
    bool enabled;
    char *rules_json;
} sc_policy_config_t;

typedef struct sc_plugins_config {
    bool enabled;
    char *plugin_dir;
    char **plugin_paths;
    size_t plugin_paths_len;
} sc_plugins_config_t;

typedef struct sc_heartbeat_config {
    bool enabled;
    uint32_t interval_minutes;
} sc_heartbeat_config_t;

#define SC_CHANNEL_CONFIG_MAX 24

typedef struct sc_email_channel_config {
    char *smtp_host;
    uint16_t smtp_port;
    char *from_address;
    char *smtp_user;
    char *smtp_pass;
    char *imap_host;
    uint16_t imap_port;
} sc_email_channel_config_t;

typedef struct sc_imap_channel_config {
    char *imap_host;
    uint16_t imap_port;
    char *imap_username;
    char *imap_password;
    char *imap_folder;
    bool imap_use_tls;
} sc_imap_channel_config_t;

typedef struct sc_imessage_channel_config {
    char *default_target;
    char **allow_from;
    size_t allow_from_count;
    int poll_interval_sec;
} sc_imessage_channel_config_t;

typedef struct sc_gmail_channel_config {
    char *client_id;
    char *client_secret;
    char *refresh_token;
    int poll_interval_sec;
} sc_gmail_channel_config_t;

#define SC_DISCORD_CHANNEL_IDS_MAX 16
typedef struct sc_discord_channel_config {
    char *token;
    char *guild_id;
    char *bot_id;
    char *channel_ids[SC_DISCORD_CHANNEL_IDS_MAX];
    size_t channel_ids_count;
} sc_discord_channel_config_t;

#define SC_TELEGRAM_ALLOW_FROM_MAX 16
typedef struct sc_telegram_channel_config {
    char *token;
    char *allow_from[SC_TELEGRAM_ALLOW_FROM_MAX];
    size_t allow_from_count;
} sc_telegram_channel_config_t;

#define SC_MCP_SERVERS_MAX     16
#define SC_MCP_SERVER_ARGS_MAX 16

#define SC_NODES_MAX 16

typedef struct sc_node_entry {
    char *name;
    char *status;
} sc_node_entry_t;

typedef struct sc_mcp_server_entry {
    char *name;
    char *command;
    char *args[SC_MCP_SERVER_ARGS_MAX];
    size_t args_count;
} sc_mcp_server_entry_t;

#define SC_SLACK_CHANNEL_IDS_MAX 16
typedef struct sc_slack_channel_config {
    char *token;
    char *channel_ids[SC_SLACK_CHANNEL_IDS_MAX];
    size_t channel_ids_count;
} sc_slack_channel_config_t;

typedef struct sc_whatsapp_channel_config {
    char *phone_number_id;
    char *token;
    char *verify_token;
} sc_whatsapp_channel_config_t;

typedef struct sc_line_channel_config {
    char *channel_token;
    char *channel_secret;
    char *user_id;
} sc_line_channel_config_t;

typedef struct sc_google_chat_channel_config {
    char *webhook_url;
} sc_google_chat_channel_config_t;

typedef struct sc_facebook_channel_config {
    char *page_id;
    char *page_access_token;
    char *verify_token;
    char *app_secret;
} sc_facebook_channel_config_t;

typedef struct sc_instagram_channel_config {
    char *business_account_id;
    char *access_token;
    char *verify_token;
    char *app_secret;
} sc_instagram_channel_config_t;

typedef struct sc_twitter_channel_config {
    char *api_key;
    char *api_secret;
    char *access_token;
    char *access_token_secret;
    char *bearer_token;
} sc_twitter_channel_config_t;

typedef struct sc_google_rcs_channel_config {
    char *agent_id;
    char *token;
    char *service_account_json_path;
} sc_google_rcs_channel_config_t;

typedef struct sc_mqtt_channel_config {
    char *broker_url;
    char *inbound_topic;
    char *outbound_topic;
    char *username;
    char *password;
    int qos;
} sc_mqtt_channel_config_t;

typedef struct sc_matrix_channel_config {
    char *homeserver;
    char *access_token;
} sc_matrix_channel_config_t;

typedef struct sc_irc_channel_config {
    char *server;
    uint16_t port;
} sc_irc_channel_config_t;

typedef struct sc_nostr_channel_config {
    char *nak_path;
    char *bot_pubkey;
    char *relay_url;
    char *seckey_hex;
} sc_nostr_channel_config_t;

typedef struct sc_channels_config {
    bool cli;
    char *default_channel;
    bool suppress_tool_progress;
    char *channel_config_keys[SC_CHANNEL_CONFIG_MAX];
    size_t channel_config_counts[SC_CHANNEL_CONFIG_MAX];
    size_t channel_config_len;
    sc_email_channel_config_t email;
    sc_imap_channel_config_t imap;
    sc_imessage_channel_config_t imessage;
    sc_gmail_channel_config_t gmail;
    sc_discord_channel_config_t discord;
    sc_telegram_channel_config_t telegram;
    sc_slack_channel_config_t slack;
    sc_whatsapp_channel_config_t whatsapp;
    sc_line_channel_config_t line;
    sc_google_chat_channel_config_t google_chat;
    sc_facebook_channel_config_t facebook;
    sc_instagram_channel_config_t instagram;
    sc_twitter_channel_config_t twitter;
    sc_google_rcs_channel_config_t google_rcs;
    sc_mqtt_channel_config_t mqtt;
    sc_matrix_channel_config_t matrix;
    sc_irc_channel_config_t irc;
    sc_nostr_channel_config_t nostr;
} sc_channels_config_t;

typedef struct sc_memory_config {
    char *profile;
    char *backend;
    bool auto_save;
    char *sqlite_path;
    uint32_t max_entries;
    char *postgres_url;
    char *postgres_schema;
    char *postgres_table;
    char *redis_host;
    uint16_t redis_port;
    char *redis_key_prefix;
    char *api_base_url;
    char *api_key;
    uint32_t api_timeout_ms;
} sc_memory_config_t;

typedef struct sc_tunnel_config {
    char *provider;
    char *domain;
} sc_tunnel_config_t;

typedef struct sc_config_gateway {
    bool enabled;
    uint16_t port;
    char *host;
    bool require_pairing;
    char *auth_token; /* optional; when set, used for WebSocket auth alongside pairing */
    bool allow_public_bind;
    uint32_t pair_rate_limit_per_minute;
    int rate_limit_requests;   /* 0 = use pair_rate_limit_per_minute */
    int rate_limit_window;     /* seconds, 0 = 60 */
    char *webhook_hmac_secret; /* optional, for X-Signature verification */
    char *control_ui_dir;      /* path to built Control UI static files */
    char **cors_origins;
    size_t cors_origins_len;
} sc_config_gateway_t;

typedef struct sc_secrets_config {
    bool encrypt;
} sc_secrets_config_t;
typedef struct sc_browser_config {
    bool enabled;
} sc_browser_config_t;
typedef struct sc_security_config {
    char *sandbox;
    uint8_t autonomy_level;
    sc_sandbox_config_t sandbox_config;
    sc_resource_limits_t resource_limits;
    sc_audit_config_t audit;
} sc_security_config_t;

#define SC_TOOL_MODEL_OVERRIDES_MAX 16

typedef struct sc_tool_model_override {
    char *tool_name;
    char *provider;
    char *model;
} sc_tool_model_override_t;

typedef struct sc_tools_config {
    uint64_t shell_timeout_secs;
    uint32_t shell_max_output_bytes;
    uint32_t max_file_size_bytes;
    uint32_t web_fetch_max_chars;
    char *web_search_provider;
    char **enabled_tools;
    size_t enabled_tools_len;
    char **disabled_tools;
    size_t disabled_tools_len;
    sc_tool_model_override_t model_overrides[SC_TOOL_MODEL_OVERRIDES_MAX];
    size_t model_overrides_len;
} sc_tools_config_t;

typedef struct sc_identity_config {
    char *format;
} sc_identity_config_t;

typedef struct sc_cost_config {
    bool enabled;
    double daily_limit_usd;
    double monthly_limit_usd;
    uint8_t warn_at_percent;
    bool allow_override;
} sc_cost_config_t;

typedef struct sc_peripherals_config {
    bool enabled;
    char *datasheet_dir;
} sc_peripherals_config_t;

typedef struct sc_hardware_config {
    bool enabled;
    char *transport;
    char *serial_port;
    uint32_t baud_rate;
    char *probe_target;
} sc_hardware_config_t;

#define SC_CONFIG_VERSION_CURRENT 2

typedef struct sc_config {
    int config_version; /* schema version for migration; default 1 */
    char *workspace_dir;
    char *config_path;
    char *workspace_dir_override;
    char *api_key;
    sc_provider_entry_t *providers;
    size_t providers_len;
    char *default_provider;
    char *default_model;
    double default_temperature;
    double temperature;
    uint32_t max_tokens;
    char *memory_backend;
    bool memory_auto_save;
    bool heartbeat_enabled;
    uint32_t heartbeat_interval_minutes;
    char *gateway_host;
    uint16_t gateway_port;
    bool workspace_only;
    uint32_t max_actions_per_hour;
    sc_diagnostics_config_t diagnostics;
    sc_autonomy_config_t autonomy;
    sc_runtime_config_t runtime;
    sc_reliability_config_t reliability;
    sc_router_config_t router;
    sc_agent_config_t agent;
    sc_heartbeat_config_t heartbeat;
    sc_channels_config_t channels;
    sc_memory_config_t memory;
    sc_tunnel_config_t tunnel;
    sc_config_gateway_t gateway;
    sc_secrets_config_t secrets;
    sc_browser_config_t browser;
    sc_security_config_t security;
    sc_tools_config_t tools;
    sc_session_config_t session;
    sc_identity_config_t identity;
    sc_cost_config_t cost;
    sc_peripherals_config_t peripherals;
    sc_hardware_config_t hardware;
    sc_cron_config_t cron;
    sc_scheduler_config_t scheduler;
    sc_node_entry_t nodes[SC_NODES_MAX];
    size_t nodes_len;
    sc_mcp_server_entry_t mcp_servers[SC_MCP_SERVERS_MAX];
    size_t mcp_servers_len;
    sc_policy_config_t policy;
    sc_plugins_config_t plugins;
    sc_arena_t *arena;
    sc_allocator_t allocator;
} sc_config_t;

sc_error_t sc_config_load(sc_allocator_t *backing, sc_config_t *out);
sc_error_t sc_config_migrate(sc_allocator_t *alloc, sc_json_value_t *root);
void sc_config_deinit(sc_config_t *cfg);
sc_error_t sc_config_parse_json(sc_config_t *cfg, const char *content, size_t len);
void sc_config_apply_env_overrides(sc_config_t *cfg);
sc_error_t sc_config_save(const sc_config_t *cfg);
sc_error_t sc_config_validate(const sc_config_t *cfg);
sc_error_t sc_config_validate_strict(const sc_config_t *cfg, const sc_json_value_t *root,
                                     bool strict);
const char *sc_config_get_provider_key(const sc_config_t *cfg, const char *name);
const char *sc_config_default_provider_key(const sc_config_t *cfg);
bool sc_config_provider_requires_api_key(const char *provider);
const char *sc_config_get_provider_base_url(const sc_config_t *cfg, const char *name);
bool sc_config_get_provider_native_tools(const sc_config_t *cfg, const char *name);
const char *sc_config_get_web_search_provider(const sc_config_t *cfg);
size_t sc_config_get_channel_configured_count(const sc_config_t *cfg, const char *key);
bool sc_config_get_provider_ws_streaming(const sc_config_t *cfg, const char *name);
bool sc_config_get_tool_model_override(const sc_config_t *cfg, const char *tool_name,
                                       const char **provider_out, const char **model_out);

/** Returns channel-specific persona if configured, else NULL. Uses global persona as fallback. */
const char *sc_config_persona_for_channel(const sc_config_t *cfg, const char *channel);
const char *sc_config_persona_for_contact(const sc_config_t *cfg, const char *contact_id);

/* Config hot-reload support */
void sc_config_set_reload_requested(void);
bool sc_config_get_and_clear_reload_requested(void);

#endif /* SC_CONFIG_H */
