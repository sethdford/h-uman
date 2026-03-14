#ifndef HU_CONFIG_H
#define HU_CONFIG_H

#include "human/config_types.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/slice.h"
#include "human/security/audit.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_provider_entry {
    char *name;
    char *api_key;
    char *base_url;
    bool native_tools;
    bool ws_streaming;
} hu_provider_entry_t;

typedef struct hu_diagnostics_config {
    char *backend;
    char *otel_endpoint;
    char *otel_service_name;
    bool log_tool_calls;
    bool log_message_receipts;
    bool log_message_payloads;
    bool log_llm_io;
} hu_diagnostics_config_t;

typedef struct hu_autonomy_config {
    char *level;
    bool workspace_only;
    uint32_t max_actions_per_hour;
    bool require_approval_for_medium_risk;
    bool block_high_risk_commands;
    char **allowed_commands;
    size_t allowed_commands_len;
    char **allowed_paths;
    size_t allowed_paths_len;
} hu_autonomy_config_t;

typedef struct hu_runtime_config {
    char *kind;
    char *docker_image;
    char *gce_project;
    char *gce_zone;
    char *gce_instance;
} hu_runtime_config_t;

typedef struct hu_reliability_config {
    char *primary_provider; /* used when default_provider is "reliable" */
    uint32_t provider_retries;
    uint64_t provider_backoff_ms;
    uint64_t channel_initial_backoff_secs;
    uint64_t channel_max_backoff_secs;
    uint64_t scheduler_poll_secs;
    uint32_t scheduler_retries;
    char **fallback_providers;
    size_t fallback_providers_len;
} hu_reliability_config_t;

typedef struct hu_router_config {
    char *fast;          /* provider name for simple tasks */
    char *standard;      /* provider name for default */
    char *powerful;      /* provider name for complex tasks */
    int complexity_low;  /* below this -> fast (default 50) */
    int complexity_high; /* above this -> powerful (default 500) */
} hu_router_config_t;

typedef struct hu_persona_channel_entry {
    char *channel;
    char *persona;
} hu_persona_channel_entry_t;

typedef struct hu_agent_config {
    bool llm_compiler_enabled;
    bool tool_routing_enabled;
    bool tree_of_thought;
    bool constitutional_ai;
    bool speculative_cache;
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
    hu_persona_channel_entry_t *persona_channels;
    size_t persona_channels_count;
    hu_persona_channel_entry_t *persona_contacts;
    size_t persona_contacts_count;
    float context_pressure_warn;    /* warn at this ratio (default 0.85) */
    float context_pressure_compact; /* auto-compact at this ratio (default 0.95) */
    float context_compact_target;   /* compact until below this ratio (default 0.70) */
} hu_agent_config_t;

typedef struct hu_policy_config {
    bool enabled;
    char *rules_json;
} hu_policy_config_t;

typedef struct hu_plugins_config {
    bool enabled;
    char *plugin_dir;
    char **plugin_paths;
    size_t plugin_paths_len;
} hu_plugins_config_t;

typedef struct hu_heartbeat_config {
    bool enabled;
    uint32_t interval_minutes;
} hu_heartbeat_config_t;

#define HU_CHANNEL_CONFIG_MAX 24

typedef struct hu_email_channel_config {
    char *smtp_host;
    uint16_t smtp_port;
    char *from_address;
    char *smtp_user;
    char *smtp_pass;
    char *imap_host;
    uint16_t imap_port;
} hu_email_channel_config_t;

typedef struct hu_imap_channel_config {
    char *imap_host;
    uint16_t imap_port;
    char *imap_username;
    char *imap_password;
    char *imap_folder;
    bool imap_use_tls;
} hu_imap_channel_config_t;

typedef struct hu_imessage_channel_config {
    char *default_target;
    char **allow_from;
    size_t allow_from_count;
    int poll_interval_sec;
    int user_response_window_sec; /* 0 = use default (45s); how long to wait for real user */
    char *response_mode;          /* "selective" (default), "normal", "eager" */
} hu_imessage_channel_config_t;

typedef struct hu_gmail_channel_config {
    char *client_id;
    char *client_secret;
    char *refresh_token;
    int poll_interval_sec;
} hu_gmail_channel_config_t;

#define HU_DISCORD_CHANNEL_IDS_MAX 16
typedef struct hu_discord_channel_config {
    char *token;
    char *guild_id;
    char *bot_id;
    char *channel_ids[HU_DISCORD_CHANNEL_IDS_MAX];
    size_t channel_ids_count;
} hu_discord_channel_config_t;

#define HU_TELEGRAM_ALLOW_FROM_MAX 16
typedef struct hu_telegram_channel_config {
    char *token;
    char *allow_from[HU_TELEGRAM_ALLOW_FROM_MAX];
    size_t allow_from_count;
} hu_telegram_channel_config_t;

#define HU_MCP_SERVERS_MAX     16
#define HU_MCP_SERVER_ARGS_MAX 16

#define HU_NODES_MAX 16

typedef struct hu_node_entry {
    char *name;
    char *status;
} hu_node_entry_t;

typedef struct hu_mcp_server_entry {
    char *name;
    char *command;
    char *args[HU_MCP_SERVER_ARGS_MAX];
    size_t args_count;
} hu_mcp_server_entry_t;

#define HU_SLACK_CHANNEL_IDS_MAX 16
typedef struct hu_slack_channel_config {
    char *token;
    char *channel_ids[HU_SLACK_CHANNEL_IDS_MAX];
    size_t channel_ids_count;
} hu_slack_channel_config_t;

typedef struct hu_whatsapp_channel_config {
    char *phone_number_id;
    char *token;
    char *verify_token;
} hu_whatsapp_channel_config_t;

typedef struct hu_line_channel_config {
    char *channel_token;
    char *channel_secret;
    char *user_id;
} hu_line_channel_config_t;

typedef struct hu_google_chat_channel_config {
    char *webhook_url;
} hu_google_chat_channel_config_t;

typedef struct hu_facebook_channel_config {
    char *page_id;
    char *page_access_token;
    char *verify_token;
    char *app_secret;
} hu_facebook_channel_config_t;

typedef struct hu_instagram_channel_config {
    char *business_account_id;
    char *access_token;
    char *verify_token;
    char *app_secret;
} hu_instagram_channel_config_t;

typedef struct hu_twitter_channel_config {
    char *api_key;
    char *api_secret;
    char *access_token;
    char *access_token_secret;
    char *bearer_token;
} hu_twitter_channel_config_t;

typedef struct hu_tiktok_channel_config {
    char *client_key;
    char *client_secret;
    char *access_token;
} hu_tiktok_channel_config_t;

typedef struct hu_google_rcs_channel_config {
    char *agent_id;
    char *token;
    char *service_account_json_path;
} hu_google_rcs_channel_config_t;

typedef struct hu_mqtt_channel_config {
    char *broker_url;
    char *inbound_topic;
    char *outbound_topic;
    char *username;
    char *password;
    int qos;
} hu_mqtt_channel_config_t;

typedef struct hu_matrix_channel_config {
    char *homeserver;
    char *access_token;
} hu_matrix_channel_config_t;

typedef struct hu_irc_channel_config {
    char *server;
    uint16_t port;
} hu_irc_channel_config_t;

typedef struct hu_nostr_channel_config {
    char *nak_path;
    char *bot_pubkey;
    char *relay_url;
    char *seckey_hex;
} hu_nostr_channel_config_t;

typedef struct hu_lark_channel_config {
    char *app_id;
    char *app_secret;
    char *webhook_url;
} hu_lark_channel_config_t;

typedef struct hu_dingtalk_channel_config {
    char *app_key;
    char *app_secret;
    char *webhook_url;
} hu_dingtalk_channel_config_t;

typedef struct hu_teams_channel_config {
    char *webhook_url;
} hu_teams_channel_config_t;

typedef struct hu_twilio_channel_config {
    char *account_sid;
    char *auth_token;
    char *from_number;
    char *to_number;
} hu_twilio_channel_config_t;

typedef struct hu_onebot_channel_config {
    char *api_base;
    char *access_token;
    char *user_id;
} hu_onebot_channel_config_t;

typedef struct hu_qq_channel_config {
    char *app_id;
    char *bot_token;
    char *channel_id;
    bool sandbox;
} hu_qq_channel_config_t;

typedef struct hu_channels_config {
    bool cli;
    char *default_channel;
    bool suppress_tool_progress;
    char *channel_config_keys[HU_CHANNEL_CONFIG_MAX];
    size_t channel_config_counts[HU_CHANNEL_CONFIG_MAX];
    size_t channel_config_len;
    hu_email_channel_config_t email;
    hu_imap_channel_config_t imap;
    hu_imessage_channel_config_t imessage;
    hu_gmail_channel_config_t gmail;
    hu_discord_channel_config_t discord;
    hu_telegram_channel_config_t telegram;
    hu_slack_channel_config_t slack;
    hu_whatsapp_channel_config_t whatsapp;
    hu_line_channel_config_t line;
    hu_google_chat_channel_config_t google_chat;
    hu_facebook_channel_config_t facebook;
    hu_instagram_channel_config_t instagram;
    hu_twitter_channel_config_t twitter;
    hu_tiktok_channel_config_t tiktok;
    hu_google_rcs_channel_config_t google_rcs;
    hu_mqtt_channel_config_t mqtt;
    hu_matrix_channel_config_t matrix;
    hu_irc_channel_config_t irc;
    hu_nostr_channel_config_t nostr;
    hu_lark_channel_config_t lark;
    hu_dingtalk_channel_config_t dingtalk;
    hu_teams_channel_config_t teams;
    hu_twilio_channel_config_t twilio;
    hu_onebot_channel_config_t onebot;
    hu_qq_channel_config_t qq;
    struct {
        char **apps;         /* app names to monitor, NULL = all */
        size_t apps_count;
        int poll_interval_sec;
    } pwa;
} hu_channels_config_t;

typedef struct hu_memory_config {
    char *profile;
    char *backend;
    bool auto_save;
    uint32_t consolidation_interval_hours; /* 0 = disabled, default 24 */
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
} hu_memory_config_t;

typedef struct hu_tunnel_config {
    char *provider;
    char *domain;
} hu_tunnel_config_t;

typedef struct hu_config_gateway {
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
} hu_config_gateway_t;

typedef struct hu_secrets_config {
    bool encrypt;
} hu_secrets_config_t;
typedef struct hu_browser_config {
    bool enabled;
} hu_browser_config_t;
typedef struct hu_security_config {
    char *sandbox;
    uint8_t autonomy_level;
    hu_sandbox_config_t sandbox_config;
    hu_resource_limits_t resource_limits;
    hu_audit_config_t audit;
} hu_security_config_t;

#define HU_TOOL_MODEL_OVERRIDES_MAX 16

typedef struct hu_tool_model_override {
    char *tool_name;
    char *provider;
    char *model;
} hu_tool_model_override_t;

typedef struct hu_tools_config {
    uint64_t shell_timeout_secs;
    uint32_t shell_max_output_bytes;
    uint32_t max_file_size_bytes;
    uint32_t web_fetch_max_chars;
    char *web_search_provider;
    char **enabled_tools;
    size_t enabled_tools_len;
    char **disabled_tools;
    size_t disabled_tools_len;
    hu_tool_model_override_t model_overrides[HU_TOOL_MODEL_OVERRIDES_MAX];
    size_t model_overrides_len;
} hu_tools_config_t;

typedef struct hu_identity_config {
    char *format;
} hu_identity_config_t;

typedef struct hu_cost_config {
    bool enabled;
    double daily_limit_usd;
    double monthly_limit_usd;
    uint8_t warn_at_percent;
    bool allow_override;
} hu_cost_config_t;

typedef struct hu_peripherals_config {
    bool enabled;
    char *datasheet_dir;
} hu_peripherals_config_t;

typedef struct hu_hardware_config {
    bool enabled;
    char *transport;
    char *serial_port;
    uint32_t baud_rate;
    char *probe_target;
} hu_hardware_config_t;

#define HU_CONFIG_VERSION_CURRENT 2

typedef struct hu_config {
    int config_version; /* schema version for migration; default 1 */
    char *workspace_dir;
    char *config_path;
    char *workspace_dir_override;
    char *api_key;
    hu_provider_entry_t *providers;
    size_t providers_len;
    char *default_provider;
    char *default_model;
    double default_temperature;
    double temperature;
    uint32_t max_tokens;
    char *memory_backend;
    bool memory_auto_save;
    uint32_t consolidation_interval_hours; /* 0 = disabled, default 24 */
    bool heartbeat_enabled;
    uint32_t heartbeat_interval_minutes;
    char *gateway_host;
    uint16_t gateway_port;
    bool workspace_only;
    uint32_t max_actions_per_hour;
    hu_diagnostics_config_t diagnostics;
    hu_autonomy_config_t autonomy;
    hu_runtime_config_t runtime;
    hu_reliability_config_t reliability;
    hu_router_config_t router;
    hu_agent_config_t agent;
    hu_heartbeat_config_t heartbeat;
    hu_channels_config_t channels;
    hu_memory_config_t memory;
    hu_tunnel_config_t tunnel;
    hu_config_gateway_t gateway;
    hu_secrets_config_t secrets;
    hu_browser_config_t browser;
    hu_security_config_t security;
    hu_tools_config_t tools;
    hu_session_config_t session;
    hu_identity_config_t identity;
    hu_cost_config_t cost;
    hu_peripherals_config_t peripherals;
    hu_hardware_config_t hardware;
    hu_cron_config_t cron;
    hu_scheduler_config_t scheduler;
    hu_behavior_config_t behavior;
    hu_node_entry_t nodes[HU_NODES_MAX];
    size_t nodes_len;
    hu_mcp_server_entry_t mcp_servers[HU_MCP_SERVERS_MAX];
    size_t mcp_servers_len;
    hu_policy_config_t policy;
    hu_plugins_config_t plugins;
    hu_arena_t *arena;
    hu_allocator_t allocator;
} hu_config_t;

hu_error_t hu_config_load(hu_allocator_t *backing, hu_config_t *out);
hu_error_t hu_config_migrate(hu_allocator_t *alloc, hu_json_value_t *root);
void hu_config_deinit(hu_config_t *cfg);
hu_error_t hu_config_parse_json(hu_config_t *cfg, const char *content, size_t len);
void hu_config_apply_env_overrides(hu_config_t *cfg);
hu_error_t hu_config_save(const hu_config_t *cfg);
hu_error_t hu_config_validate(const hu_config_t *cfg);
hu_error_t hu_config_validate_strict(const hu_config_t *cfg, const hu_json_value_t *root,
                                     bool strict);
const char *hu_config_get_provider_key(const hu_config_t *cfg, const char *name);
const char *hu_config_default_provider_key(const hu_config_t *cfg);
bool hu_config_provider_requires_api_key(const char *provider);
const char *hu_config_get_provider_base_url(const hu_config_t *cfg, const char *name);
bool hu_config_get_provider_native_tools(const hu_config_t *cfg, const char *name);
const char *hu_config_get_web_search_provider(const hu_config_t *cfg);
size_t hu_config_get_channel_configured_count(const hu_config_t *cfg, const char *key);
bool hu_config_get_provider_ws_streaming(const hu_config_t *cfg, const char *name);
bool hu_config_get_tool_model_override(const hu_config_t *cfg, const char *tool_name,
                                       const char **provider_out, const char **model_out);

/** Returns channel-specific persona if configured, else NULL. Uses global persona as fallback. */
const char *hu_config_persona_for_channel(const hu_config_t *cfg, const char *channel);
const char *hu_config_persona_for_contact(const hu_config_t *cfg, const char *contact_id);

/* Config hot-reload support */
void hu_config_set_reload_requested(void);
bool hu_config_get_and_clear_reload_requested(void);

#endif /* HU_CONFIG_H */
