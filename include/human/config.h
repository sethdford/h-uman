#ifndef HU_CONFIG_H
#define HU_CONFIG_H

#include "human/cognition/metacognition.h"
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
    /* Phase 1 (RL SOTA) — llamacpp-specific tuning. These fields are
     * read from `providers[].context_size`/`.threads`/`.use_gpu`/
     * `.n_gpu_layers` in the JSON config and forwarded into
     * hu_llamacpp_config_t when the entry's name is "llamacpp" /
     * "llama.cpp". Zero values mean "use llama.cpp's default".
     * Other providers ignore these fields. */
    size_t context_size;
    int threads;
    bool use_gpu;
    int n_gpu_layers;
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

/* 2026-05 audit follow-up — per-model fallback chain. When the primary
 * provider is local (e.g. mlx_local) and serves a model name that the
 * cloud fallback (e.g. gemini) doesn't recognize, the reliable provider's
 * fallback path would otherwise call the cloud with an unknown model →
 * silent failure. This struct lets the operator declare "if request asks
 * for model X, the fallback should substitute Y" — wired through to
 * hu_reliable_create_ex's model_fallbacks arg.
 *
 * JSON shape under reliability:
 *   "model_fallbacks": [
 *     { "model": "gemma-4-31b-it-4bit", "fallbacks": ["gemini-3.5-flash"] }
 *   ]
 */
typedef struct hu_config_model_fallback {
    char *model;            /* the model name a request asks for */
    char **fallback_models; /* substitutes tried in order on retry */
    size_t fallback_models_len;
} hu_config_model_fallback_t;

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
    hu_config_model_fallback_t *model_fallbacks;
    size_t model_fallbacks_len;
} hu_reliability_config_t;

typedef struct hu_router_config {
    char *fast;          /* provider name for simple tasks */
    char *standard;      /* provider name for default */
    char *powerful;      /* provider name for complex tasks */
    int complexity_low;  /* below this -> fast (default 50) */
    int complexity_high; /* above this -> powerful (default 500) */
} hu_router_config_t;

#define HU_ENSEMBLE_CONFIG_PROVIDER_NAMES_MAX 8

typedef struct hu_ensemble_config {
    char *providers[HU_ENSEMBLE_CONFIG_PROVIDER_NAMES_MAX]; /* e.g. "openai", "anthropic" */
    size_t providers_len;
    char *strategy; /* "round_robin", "best_for_task", "consensus"; default round_robin if NULL */
} hu_ensemble_config_t;

typedef struct hu_persona_channel_entry {
    char *channel;
    char *persona;
} hu_persona_channel_entry_t;

typedef struct hu_agent_config {
    bool llm_compiler_enabled;
    bool hula_enabled;
    bool mcts_planner_enabled;
    bool tool_routing_enabled;
    bool tree_of_thought;
    bool constitutional_ai;
    char *constitutional_principles; /* formatted principle list for system prompt injection */
    /* US-7.9: when true, persona->style_rules are enforced via a pure
     * string-pattern self-critique pass after LLM generation.  Default
     * false → zero effect on existing behavior. */
    bool constitutional_style_rules_enabled;
    bool speculative_cache;
    bool multi_agent;
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
    /* Fleet: pooled sub-agents (spawn) limits — see docs/standards/ai/fleet.md */
    uint32_t fleet_max_spawn_depth;  /* 0 = unlimited; default from merge */
    uint32_t fleet_max_total_spawns; /* 0 = unlimited lifetime spawns per pool */
    double fleet_budget_usd;         /* 0 = unlimited; requires shared cost tracker */
    char *default_profile;
    char *persona;
    hu_persona_channel_entry_t *persona_channels;
    size_t persona_channels_count;
    hu_persona_channel_entry_t *persona_contacts;
    size_t persona_contacts_count;
    float context_pressure_warn;         /* warn at this ratio (default 0.85) */
    float context_pressure_compact;      /* auto-compact at this ratio (default 0.95) */
    float context_compact_target;        /* compact until below this ratio (default 0.70) */
    hu_metacog_settings_t metacognition; /* agent.metacognition in config.json */
    char *mr_reflexive_model;            /* model router: fast backchannel model */
    char *mr_conversational_model;       /* model router: standard chat model */
    char *mr_analytical_model;           /* model router: capable reasoning model */
    char *mr_deep_model;                 /* model router: most capable model */
    bool mr_judge_enabled;               /* model router: enable LLM-as-Judge classification */
    char *mr_judge_model;     /* model router: model to use for judge (default: reflexive) */
    char *s3_local_model;     /* dedicated model for S3 (private) content; NULL = use degradation
                                 fallback */
    char *mr_on_device_model; /* model router: on-device model name (default: apple-foundationmodel)
                               */
    bool mr_on_device_enabled; /* model router: enable on-device routing (default: true on macOS) */
    hu_mlx_local_routing_t
        mlx_local_routing;     /* AC-1: tri-state routing policy (OFF/AUTO/FORCE).
                                * AUTO is default: local when healthy, else cloud.
                                * Legacy mlx_local_enabled maps: true→FORCE, false→AUTO. */
    bool mr_mlx_local_enabled; /* DEPRECATED: for backward compat only. Prefer mlx_local_routing.
                                * model router: route REFLEXIVE/CONVERSATIONAL to the Seth-voice
                                * mlx_local LoRA when the local server probes healthy (Dermot C2).
                                * Default false (opt-in). ANALYTICAL/DEEP always stay cloud. */
    char *mr_mlx_local_model;  /* model router: local adapter identifier the mlx provider serves
                                * (e.g. "seth-lora-v4-repair-..."); NULL = feature inert */
    char *fallback_model;      /* Provider-degradation fallback model name. When the primary
                                * default_model fails (HU_ERR_NETWORK / HU_ERR_TIMEOUT / etc),
                                * hu_provider_degrade_chat retries with this model on the SAME
                                * provider instance before emitting the honest-failure canned
                                * message. NULL (default) preserves today's "no fallback, honest
                                * fail" behavior. NOTE: this is a same-provider fallback. To
                                * route to a DIFFERENT provider on MLX-down (e.g. cloud Gemini
                                * when local mlx_local crashes), a provider-router change is
                                * required (US-13 follow-up, scoped in
                                * docs/plans/2026-05-26-sprint-56-gemma-as-seth/us13-followup.md). */
    bool prompt_cache_enabled; /* enable cross-turn system prompt dedup (default true) */
    bool agent_comm_enabled;
    uint32_t best_of_n;        /* best-of-N candidates (0 or 1 = disabled, max 5) */
    char *context_engine_type; /* "legacy" (default) or "rag" */
    /* Claude Code feature integration */
    uint8_t permission_level;       /* 0=ReadOnly, 1=WorkspaceWrite, 2=DangerFullAccess */
    bool session_auto_save;         /* auto-save session after each turn */
    char *session_dir;              /* directory for session JSON files */
    bool discover_instructions;     /* discover .human.md/HUMAN.md files */
    bool compaction_use_structured; /* use XML structured summaries */
    char *self_rag_mode;     /* "off", "telemetry", "soft", "strict" (env: HU_SELF_RAG_MODE) */
    bool self_rag_streaming; /* enable streaming self-RAG (env: HU_SELF_RAG_STREAMING) */
    /* RAG-over-own-messages voice grounding: retrieve the user's most-similar
     * past sent messages and inject them as per-turn few-shot grounding.
     * Default off — flip on for the LoRA-vs-RAG A/B ("measure before optimize").
     * Corpus = ~/.human/voice_corpus.jsonl (the harvested sent-message corpus). */
    bool rag_grounding_enabled;
    /* Activation steering: when true, the agent maps the active persona overlay's
     * traits to residual-stream steering coefficients and sends them to the local
     * model (mlx_local) as a "steering" field. Default off; the local server
     * clamps to the measured-safe range and treats absent/zero as a no-op. */
    bool activation_steering_enabled;
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

typedef struct hu_feeds_config {
    bool enabled;
    char *gmail_client_id;
    char *gmail_client_secret;
    char *gmail_refresh_token;
    char *twitter_bearer_token;
    char *interests;
    double relevance_threshold;
    uint32_t poll_interval_rss;
    uint32_t poll_interval_gmail;
    uint32_t poll_interval_imessage;
    uint32_t poll_interval_twitter;
    uint32_t poll_interval_file_ingest;
    uint32_t max_items_per_poll;
    uint32_t retention_days;
} hu_feeds_config_t;

typedef struct hu_heartbeat_config {
    bool enabled;
    uint32_t interval_minutes;
} hu_heartbeat_config_t;

#define HU_CHANNEL_CONFIG_MAX 24

/* Shared daemon behavior config — embedded in per-channel config structs.
 * Daemon reads from the active channel's config, falling back to defaults.
 *
 * All string fields (e.g. response_mode) are arena-allocated by hu_config_load.
 * hu_config_deinit destroys the arena, freeing all strings at once.
 * Do not call free() on individual fields. */
typedef struct hu_channel_daemon_config {
    char *response_mode;          /* "selective" (default), "normal", "eager" */
    int user_response_window_sec; /* 0 = use default (120s) */
    int poll_interval_sec;        /* 0 = use channel-specific default (see bootstrap) */
    bool voice_enabled;           /* enable TTS on this channel */
    bool llm_decides;             /* bypass heuristic gating, let LLM decide responses */
} hu_channel_daemon_config_t;

typedef struct hu_email_channel_config {
    char *smtp_host;
    uint16_t smtp_port;
    char *from_address;
    char *smtp_user;
    char *smtp_pass;
    char *imap_host;
    uint16_t imap_port;
    hu_channel_daemon_config_t daemon;
} hu_email_channel_config_t;

typedef struct hu_imap_channel_config {
    char *imap_host;
    uint16_t imap_port;
    char *imap_username;
    char *imap_password;
    char *imap_folder;
    bool imap_use_tls;
    char *smtp_host;
    uint16_t smtp_port;
    char *from_address;
} hu_imap_channel_config_t;

#define HU_REACTION_COLLECTION_CHANNELS_MAX 4

typedef struct hu_reaction_collection_config {
    bool enabled;
    char channels[HU_REACTION_COLLECTION_CHANNELS_MAX][32];
    size_t channel_count;
    int poll_interval_seconds;
    /* Optional override for iMessage chat.db. If empty: HU_CHATDB env, then
     * $HOME/Library/Messages/chat.db. Must be absolute when set. */
    char chatdb_path[256];
} hu_reaction_collection_config_t;

typedef struct hu_follow_up_watcher_config {
    bool enabled;
    int interval_seconds; /* default 300 (5 min) */
} hu_follow_up_watcher_config_t;

typedef struct hu_proactive_throttle_config {
    bool enabled;
    int per_contact_daily_max; /* default 1 */
    /* M3 Dispatch T8b (2026-05-26) — `use_unified_dispatch` removed.
     * The flag was T3's rollout safety switch for unified vs legacy
     * proactive composition. T7 flipped the default to true; T8 deleted
     * the legacy branch; this commit (T8b) removes the now-vestigial
     * flag entirely. To disable proactive sends, operators use
     * `initiative.enabled = false` (kills the initiative subsystem) or
     * remove `proactive_channel` from individual contact configs (kills
     * per-contact). See docs/plans/2026-05-26-m3-dispatch-unification/. */
} hu_proactive_throttle_config_t;

/* Initiative Layer — see docs/plans/2026-05-25-initiative-layer/.
 *
 * Periodic proposer that ticks during awake hours and asks "should I bring
 * something up to Seth?" — even when no inbound event fires. Disabled by
 * default; flip enabled=true after one week of dry-run logs and threshold
 * tuning per the spec. */
typedef struct hu_initiative_config {
    bool enabled;                /* default false (kill switch, AC-7) */
    int tick_interval_sec;       /* default 1800 = 30 min (DECISION-1) */
    double confidence_threshold; /* default 0.85 (DECISION-2) */
    char *propose_model;         /* default "gemini-3.5-flash" (DECISION-3) */
    int per_contact_min_seconds; /* default 600 = 10 min; recency floor */

    /* Sprint 55 T4 delivery wire — added to land FIRED → iMessage send.
     * Per the spec's T5 the safe default is dry-run mode; flip after a week
     * of observation. */
    char *target_handle; /* iMessage handle to deliver to (e.g. "+14845661687").
                            NULL = no delivery — FIRED decisions logged + dropped
                            with a WARN so operator sees the gap. */
    bool dry_run;        /* default true — log would-have-been-sent messages
                            instead of delivering. Flip false ONLY after dry-run
                            observation proves thresholds are tuned correctly. */
} hu_initiative_config_t;

/* Prompt-Budget Compression — see docs/plans/2026-05-25-director-compression/.
 *
 * Phase 1 is OBSERVABILITY only — the budget object accumulates per-field
 * byte counts across turns; `enabled` defaults to false so the trim
 * behavior stays off until operators have a week of data and decide to
 * flip it on. Phase 2 (Task 4) wires the actual trim of DEAD fields. */
#define HU_PROMPT_BUDGET_ALLOWLIST_MAX 10

typedef struct hu_prompt_budget_config {
    bool enabled;                 /* default false (Phase 1 ships gated OFF) */
    int dead_field_min_bytes;     /* default 16 — fields below this mean are DEAD */
    int min_samples_before_tag;   /* default 100 — turns needed before tagging */
    const char **field_allowlist; /* fields to keep even if DEAD (names like "memory_context") */
    size_t field_allowlist_count; /* number of allowlisted fields (max
                                     HU_PROMPT_BUDGET_ALLOWLIST_MAX) */
} hu_prompt_budget_config_t;

/* Sprint 41 follow-up #2 — operator-facing runtime knobs for the
 * response_guard subsystem. The guard itself runs unconditionally; this
 * config is the kill-switch surface so operators can disable specific
 * detectors at runtime without recompiling (e.g. operator sees a G9
 * false-positive burst at 3am and silences the rule until they can
 * tune marker lists). All fields default to "enabled" so a fresh
 * config.json keeps the protective defaults. */
typedef struct hu_response_guard_config {
    /* G9 — naked discourse-marker opener. Default true. Set to false in
     * config.json under {"response_guard": {"naked_opener_enabled":
     * false}} to disable the detector process-wide. The setter
     * hu_response_guard_set_naked_opener_globally_disabled() applies
     * !naked_opener_enabled at daemon startup. */
    bool naked_opener_enabled;
    /* Sprint 41 follow-up #4 — per-channel G9 disable list. When a
     * channel name appears here, G9 does not fire on outbound messages
     * to that channel. Use case: voice channel where short backchannel-
     * style openings can be valid (listener hears them as conversational
     * fillers, not written gibberish). Caller-owned heap strings;
     * freed via hu_config_deinit. */
    char **g9_disabled_channels;
    size_t g9_disabled_channels_count;
} hu_response_guard_config_t;

/* Hard cap on the disabled-channels list — bounds the parse-time array
 * allocation. Generous; production channel inventory is small. */
#define HU_RESPONSE_GUARD_MAX_DISABLED_CHANNELS 32

typedef struct hu_imessage_action_surface_v2_config {
    bool enabled;                  /* master gate (default true on macOS, false elsewhere) */
    float thread_affinity_default; /* persona_thread_affinity default (default 0.3) */
    int min_reply_delay_ms;        /* persona pacing floor (default 1500) */
    int reply_delay_variance_ms;   /* persona pacing jitter (default 600) */
    char *sticker_dir;             /* local sticker directory (default ~/.human/stickers) */
} hu_imessage_action_surface_v2_config_t;

typedef struct hu_imessage_channel_config {
    char *default_target;
    char **allow_from;
    size_t allow_from_count;
    int poll_interval_sec;
    int user_response_window_sec; /* DEPRECATED: use daemon.user_response_window_sec */
    char *response_mode;          /* DEPRECATED: use daemon.response_mode */
    bool use_imsg_cli;            /* prefer steipete/imsg CLI for send/react when available */
    char *loopback_handle;        /* treat is_from_me=1 from this handle as incoming (self-test) */
    hu_imessage_action_surface_v2_config_t action_surface_v2;
    hu_channel_daemon_config_t daemon;
} hu_imessage_channel_config_t;

typedef struct hu_gmail_channel_config {
    char *client_id;
    char *client_secret;
    char *refresh_token;
    int poll_interval_sec;
    hu_channel_daemon_config_t daemon;
} hu_gmail_channel_config_t;

#define HU_DISCORD_CHANNEL_IDS_MAX 16
typedef struct hu_discord_channel_config {
    char *token;
    char *guild_id;
    char *bot_id;
    char *channel_ids[HU_DISCORD_CHANNEL_IDS_MAX];
    size_t channel_ids_count;
    hu_channel_daemon_config_t daemon;
} hu_discord_channel_config_t;

#define HU_TELEGRAM_ALLOW_FROM_MAX 16
typedef struct hu_telegram_channel_config {
    char *token;
    char *allow_from[HU_TELEGRAM_ALLOW_FROM_MAX];
    size_t allow_from_count;
    hu_channel_daemon_config_t daemon;
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
    char *transport_type; /* "stdio" (default), "sse", "http" */
    char *command;        /* stdio: binary path */
    char *args[HU_MCP_SERVER_ARGS_MAX];
    size_t args_count;
    char *url; /* sse/http: endpoint URL */
    bool auto_connect;
    uint32_t timeout_ms; /* 0 = use default (30s) */
    /* OAuth2 PKCE authentication (optional, for HTTP/SSE servers) */
    char *oauth_client_id;
    char *oauth_auth_url;
    char *oauth_token_url;
    char *oauth_scopes;
    char *oauth_redirect_uri;
} hu_mcp_server_entry_t;

#define HU_SLACK_CHANNEL_IDS_MAX 16
typedef struct hu_slack_channel_config {
    char *token;
    char *channel_ids[HU_SLACK_CHANNEL_IDS_MAX];
    size_t channel_ids_count;
    hu_channel_daemon_config_t daemon;
} hu_slack_channel_config_t;

typedef struct hu_whatsapp_channel_config {
    char *phone_number_id;
    char *token;
    char *verify_token;
    hu_channel_daemon_config_t daemon;
} hu_whatsapp_channel_config_t;

#define HU_SIGNAL_ALLOW_FROM_MAX 16
typedef struct hu_signal_channel_config {
    /* signal-cli daemon endpoint, e.g. "http://localhost:8080". When NULL or
     * empty, the channel is disabled and bootstrap skips it (matches the
     * is_configured() pattern used by the other Tier-2 channels). */
    char *http_url;
    /* The signal account/phone number we're posting from, e.g. "+15551234567". */
    char *account;
    /* Optional 1:1 sender allowlist. Empty -> accept all 1:1 messages. */
    char *allow_from[HU_SIGNAL_ALLOW_FROM_MAX];
    size_t allow_from_count;
    /* Optional group allowlist (group ids). Used together with group_policy. */
    char *group_allow_from[HU_SIGNAL_ALLOW_FROM_MAX];
    size_t group_allow_from_count;
    /* "open" (default), "allowlist", or "disabled". */
    char *group_policy;
    hu_channel_daemon_config_t daemon;
} hu_signal_channel_config_t;

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
    hu_channel_daemon_config_t daemon;
} hu_matrix_channel_config_t;

typedef struct hu_irc_channel_config {
    char *server;
    uint16_t port;
    hu_channel_daemon_config_t daemon;
} hu_irc_channel_config_t;

typedef struct hu_nostr_channel_config {
    char *nak_path;
    char *bot_pubkey;
    char *relay_url;
    char *seckey_hex;
    hu_channel_daemon_config_t daemon;
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
    hu_signal_channel_config_t signal;
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
    hu_channel_daemon_config_t default_daemon;
    struct {
        char **apps; /* app names to monitor, NULL = all */
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
    /* W15 envelope encryption opt-in. When true AND a keystore is
     * attached to the active backend, memory rows are wrapped via
     * hu_encrypted_store_wrap on insert and unwrapped on read.
     * Default false — encryption-by-default is an M3 milestone. */
    bool encrypt_at_rest;
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

typedef struct hu_voice_settings {
    char *local_stt_endpoint; /* e.g. "http://localhost:8000/v1/audio/transcriptions" */
    char *local_tts_endpoint; /* e.g. "http://localhost:8880/v1/audio/speech" */
    char *stt_provider;       /* "gemini", "groq", "local" — NULL = auto */
    char *tts_provider;       /* "openai", "cartesia", "local" — NULL = auto */
    char *tts_voice;          /* voice name, NULL = default */
    char *tts_model;          /* model name, NULL = default */
    char *stt_model;          /* model name, NULL = default */
    char *stt_language;       /* BCP-47 STT language hint, NULL = auto-detect */
    char *mode;           /* "sonata", "realtime", "webrtc" — NULL = sonata (default pipeline) */
    char *realtime_model; /* OpenAI Realtime model, e.g. "gpt-4o-realtime-preview" */
    char *realtime_voice; /* Voice for Realtime, e.g. "alloy" */
    char *vertex_access_token;
    char *vertex_region;
    char *vertex_project;
    bool privacy_mode; /* true = on-device only: STT+TTS never contact Cartesia/cloud (ADR
                          2026-05-31) */
} hu_voice_settings_t;

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
    /** When set, DPO exports write to `<dpo_export_dir>/dpo_preferences.jsonl`. */
    char *dpo_export_dir;
    char *data_dir; /* overrides ~/.human/data/ for hu_data_load() */
    char *temp_dir; /* overrides platform temp dir */
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
    hu_ensemble_config_t ensemble;
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
    hu_voice_settings_t voice;
    hu_session_config_t session;
    hu_identity_config_t identity;
    hu_cost_config_t cost;
    hu_peripherals_config_t peripherals;
    hu_hardware_config_t hardware;
    hu_cron_config_t cron;
    hu_scheduler_config_t scheduler;
    hu_personalization_config_t personalization;
    hu_mlx_local_config_t mlx_local; /* B4: streaming gate + first-token budget */
    hu_learning_config_t learning;   /* Spec 2026-05-19 — DPO pair-count training trigger */
    hu_intrinsic_config_t intrinsic; /* A3 — intrinsic motivation (default off) */
    hu_prosocial_routines_config_t prosocial_routines; /* C — scheduled routines (default off) */
    hu_reflection_loop_config_t reflection_loop;       /* M2 reflection-loop */
    hu_inference_config_t inference; /* US-7.7 — best-of-N at inference (default off) */
    hu_behavior_config_t behavior;
    hu_node_entry_t nodes[HU_NODES_MAX];
    size_t nodes_len;
    hu_mcp_config_t mcp;
    hu_hooks_config_t hooks;
    hu_mcp_server_entry_t mcp_servers[HU_MCP_SERVERS_MAX];
    size_t mcp_servers_len;
    hu_policy_config_t policy;
    hu_plugins_config_t plugins;
    hu_feeds_config_t feeds;
    hu_reaction_collection_config_t reaction_collection;
    hu_follow_up_watcher_config_t follow_up_watcher;
    hu_proactive_throttle_config_t proactive_throttle;
    hu_initiative_config_t initiative;
    hu_prompt_budget_config_t prompt_budget;
    hu_response_guard_config_t response_guard;
    char *auto_update;                    /* "off" (default), "check", or "apply" */
    uint32_t update_check_interval_hours; /* default 24; 0 = use default */
    hu_arena_t *arena;
    hu_allocator_t allocator;

    struct {
        char *default_image_model;
        char *default_video_model;
        char *vertex_project;
        char *vertex_region;
        char *veo_storage_uri;
    } media_gen;
} hu_config_t;

hu_error_t hu_config_load(hu_allocator_t *backing, hu_config_t *out);
hu_error_t hu_config_load_from(hu_allocator_t *backing, const char *path, hu_config_t *out);
hu_error_t hu_config_migrate(hu_allocator_t *alloc, hu_json_value_t *root);
void hu_config_deinit(hu_config_t *cfg);
hu_error_t hu_config_parse_json(hu_config_t *cfg, const char *content, size_t len);

/* Seed the static (non-JSON) defaults into a freshly memset config. The
 * production load path (hu_config_load) calls this before overlaying parsed
 * JSON; callers that drive hu_config_parse_json directly (e.g. tests) must
 * call this first to get the same defaults real users see. */
void hu_config_apply_defaults(hu_config_t *cfg, hu_allocator_t *a);
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
