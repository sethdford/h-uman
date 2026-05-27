#ifndef HU_CONFIG_TYPES_H
#define HU_CONFIG_TYPES_H

#include "human/core/allocator.h"
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

/* W13 Phase 4.1 — personalization (LoRA adapter auto-load).
 *
 * When `enabled` is true and `lora_adapter_path` resolves to a readable
 * `.lora` file, the daemon loads the adapter into the configured
 * provider on startup. Adapter id defaults to the basename when blank.
 *
 * Production deployments will typically set this from `human ml
 * lora-persona`'s output path; on-device personalization closes the
 * loop without a separate `human ml apply-adapter` invocation.
 *
 * `m3_adapter_probe_path` (optional): when set, the daemon probes the M3
 * frontier-adapter stub (`hu_m3_frontier_adapter_try_open`) at startup and
 * logs success or failure — validates the seam without loading LoRA.
 *
 * `m3_adapter_disabled` (Track D D1.3 rollback flag): when true, the
 * bootstrap path skips `hu_agent_m3_adapter_attach` entirely even if
 * `m3_adapter_probe_path` is set. The env var
 * `HUMAN_M3_ADAPTER_DISABLE=1` overrides the config and forces-disable
 * at runtime — the operator's kill switch when the bridge regresses
 * in production. Default false; the bridge stays opt-in via the
 * probe path config but kill-switchable independently.
 */
/* US-7.8 — MoLoRA static per-channel router (Init #02 phase 1).
 *
 * `enabled` is independent of `personalization.enabled` at the config level,
 * but the agent-turn hook in `src/agent/agent_turn.c` only consults the router
 * when both flags are true AND `HU_ENABLE_MOLORA` is compiled in. When this
 * block is absent from the JSON config, parser leaves `enabled = false` and
 * `count = 0`, which `hu_molora_router_init` treats as the disabled state
 * (identical to today's pre-story behavior).
 *
 * Channel ids are stored normalized (lowercase, no `:`-suffix, no whitespace)
 * — the parser applies `hu_molora_router_normalize_channel` before insertion.
 * Adapter paths are owned by this struct (parser strdups them); the router
 * borrows the pointers and asserts the config outlives it.
 *
 * Schema (Phase 1, flat map per design Q1):
 *   "molora": {
 *     "enabled": true,
 *     "channel_adapters": {
 *       "telegram":  "~/.human/adapters/seth/telegram.lora",
 *       "imessage":  "~/.human/adapters/seth/imessage.lora",
 *       "slack":     "~/.human/adapters/seth/slack.lora",
 *       "discord":   "~/.human/adapters/seth/discord.lora"
 *     }
 *   }
 *
 * Phase 2 will extend each entry to `{path, scale}` via a versioned schema
 * bump — Phase 1 stays flat for forward-compat. */
#define HU_MOLORA_CONFIG_MAX_CHANNELS     16
#define HU_MOLORA_CONFIG_CHANNEL_NAME_MAX 32

typedef struct hu_molora_channel_entry {
    /* Normalized channel id (parser-normalized; null-terminated). */
    char channel[HU_MOLORA_CONFIG_CHANNEL_NAME_MAX];
    /* Adapter path; owned by this struct (parser-strdup, merge-free). */
    char *adapter_path;
} hu_molora_channel_entry_t;

typedef struct hu_molora_config {
    bool enabled;
    size_t count;
    hu_molora_channel_entry_t entries[HU_MOLORA_CONFIG_MAX_CHANNELS];
} hu_molora_config_t;

/* M3 Bridge B Phase B4 (docs/plans/2026-05-26-m3-b4-mlx-local-sse/) —
 * configuration for the production daemon's HTTP-based mlx_local
 * provider when it talks to a streaming mlx-server.
 *
 * `streaming_enabled` (default FALSE — revised T3 2026-05-26): opts
 * INTO the SSE-streaming path for chat completions when true; uses
 * the buffered response shape when false. The default is FALSE
 * because mlx-server.py's `strip_thought_channels` postprocessor
 * currently only runs in the non-streaming response shape, so the
 * streaming path leaks raw `<|channel>thought` markers as visible
 * text. agent_stream.c carried an unconditional workaround forcing
 * this off since 2026-05-25; T3 replaced that workaround with this
 * gate so operators can opt IN once they've verified their server
 * strips thought markers in streaming mode (or once a client-side
 * filter ships in T4+).
 *
 * `first_token_budget_ms` (default 500): operator-visible budget for
 * the first SSE event to arrive after POST send. If the actual first-
 * token latency exceeds this, the provider logs warn-once with the
 * measured value so operators can decide whether the server has
 * regressed. Only takes effect when streaming_enabled is true.
 *
 * When streaming_enabled=true, the daemon emits one info-level log
 * line at startup confirming the opt-in state — operators should see
 * positive confirmation of the latency win, not just a silent flip. */
typedef struct hu_mlx_local_config {
    bool streaming_enabled;    /* default false (see comment above) */
    int first_token_budget_ms; /* default 500; <=0 keeps the default */
} hu_mlx_local_config_t;

typedef struct hu_personalization_config {
    bool enabled;
    char *lora_adapter_path;
    char *lora_adapter_id;
    char *m3_adapter_probe_path;
    bool m3_adapter_disabled;
    hu_molora_config_t molora;
    /* When true, the model router prefers the on-device model at ALL
     * cognitive tiers (not just REFLEXIVE). Combined with a configured
     * mlx_local on-device provider, this routes EVERY contact's reply
     * through the local LoRA-adapted model so the personalized voice
     * is applied on every turn — instead of the cloud-cold tone that
     * happens when cloud Gemini handles conversational+ tiers without
     * the adapter. Default false preserves the tier-graduated default. */
    bool force_local_mlx;
} hu_personalization_config_t;

/* Spec 2026-05-19 — reaction-loop pair-count auto-training trigger.
 *
 * `dpo_pair_training_threshold`: when the uncommitted DPO-pair count
 * (per `hu_dpo_pair_count`) reaches this value, the daemon enqueues a
 * LoRA training run through the shared training-runner entry. This is
 * additive to the existing learner-pending trigger (~10 W13 signals).
 *
 * Default 100 — see `docs/standards/ai/` and the spec's D-RL-1 decision:
 * roughly a busy day's worth of reactions for a heavy user, a week for a
 * light user; configurable for operators on slower hardware.
 *
 * Set to 0 to disable the pair-count trigger entirely; the daemon emits
 * one info-level log line on first tick per
 * `~/.claude/rules/silent-config-gated-subsystems.md`. */
typedef struct hu_learning_config {
    int dpo_pair_training_threshold;
    /* Spec 2026-05-19 M3 closure / AC-M3-7 — when true, the pair-count
     * trigger enqueues training with target=frontier_mlx (subprocess
     * to scripts/m3_mlx_lora_bridge.py against the served frontier
     * model) rather than target=huml_reference (in-process toy GPT).
     *
     * Default false — operators must opt in. The daemon emits one
     * info-level log line on first tick for both the disabled and
     * first-enabled paths per
     * ~/.claude/rules/silent-config-gated-subsystems.md. */
    bool m3_frontier_auto_training;
    /* M3 trivia closure (2026-05-26) — promotes HU_NIGHTLY_LORA_ENABLED env
     * var (src/daemon.c:4430) to a first-class config field. When false,
     * the 04:00 nightly LoRA training run is skipped. The env var is still
     * honored as a backwards-compatible fallback (config wins when set);
     * the daemon emits a one-shot warn if the env path was used. Default
     * false — opt-in like m3_frontier_auto_training, since the run blocks
     * the daemon loop for up to 30 min. */
    bool nightly_lora_enabled;
} hu_learning_config_t;

#define HU_LEARNING_DPO_PAIR_TRAINING_THRESHOLD_DEFAULT 100

/* Reflection loop config (M2 closure, 2026-05-26-reflection-loop spec T3).
 *
 * Periodic batch task that distills accumulated conversations into
 * typed, queryable patterns. Default disabled — operator opts in via
 * `{"reflection": {"enabled": true}}` in config.json. When enabled,
 * the daemon emits one info-level log line on first tick (per
 * silent-config-gated-subsystems.md).
 *
 * Trigger semantics (all three gates must pass for an idle-driven
 * run; `daily_floor_hours` bypass overrides idle):
 *   - At least `min_interval_hours` since last completed run.
 *   - At least `idle_threshold_hours` since last user activity.
 *   - OR: `daily_floor_hours` since last run (force-run, ignores idle).
 *
 * `local_shadow_mode` (Sprint 2): when true, every cloud reflection
 * also fires a local-Gemma reflection in shadow, and the eval harness
 * compares outputs. Default false — Sprint 2 work item. */
typedef struct hu_reflection_loop_config {
    bool enabled;
    bool local_shadow_mode;
    int min_interval_hours;
    int idle_threshold_hours;
    int daily_floor_hours;
    char provider[64];       /* default "gemini-3.5-flash" (per CLAUDE.md M3 row) */
    char local_provider[64]; /* default "gemma-4-31b-local"; only used when shadow_mode */
} hu_reflection_loop_config_t;

#define HU_REFLECTION_DEFAULT_MIN_INTERVAL_HOURS   12
#define HU_REFLECTION_DEFAULT_IDLE_THRESHOLD_HOURS 2
#define HU_REFLECTION_DEFAULT_DAILY_FLOOR_HOURS    24
#define HU_REFLECTION_DEFAULT_PROVIDER             "gemini-3.5-flash"
#define HU_REFLECTION_DEFAULT_LOCAL_PROVIDER       "gemma-4-31b-local"

/* US-7.7 (Sprint 7, P1) — Test-time persona scoring (best-of-N at inference).
 *
 * When `best_of_n >= 2` AND the active provider is `llamacpp`, the agent's
 * chat-dispatch site (src/agent/agent_turn.c) routes through the
 * `hu_best_of_n_chat` decorator (src/agent/best_of_n.c). The decorator
 * issues up to N completions, scores each via
 * `hu_communication_style_fidelity_score` (frozen signature in
 * include/human/memory/personal_model.h:535), and returns the candidate
 * with the highest fidelity.
 *
 * Defaults:
 *   - best_of_n        = 1   → behavior unchanged from current code
 *                              (single chat call, no scoring).
 *   - best_of_n_cost_cap_ms = 0 → no cap (run all N candidates to completion).
 *
 * The cap is a soft cap: it's checked after each completion returns, so the
 * N-th completion that pushes us over the cap still runs to completion
 * before we return the best-so-far. See src/agent/best_of_n.c for details.
 *
 * Cloud-provider misconfiguration (best_of_n >= 2 + cloud provider) emits a
 * doctor warning per AC-7.7.3 (src/doctor.c). */
typedef struct hu_inference_config {
    uint32_t best_of_n;             /* default 1 (disabled); >=2 enables best-of-N */
    uint32_t best_of_n_cost_cap_ms; /* default 0 (no cap); soft wall-clock cap */
} hu_inference_config_t;

typedef struct hu_behavior_config {
    uint32_t consecutive_limit;  /* max consecutive messages from self before skip (default 3) */
    uint32_t participation_pct;  /* max % of recent messages before skip (default 40) */
    uint32_t max_response_chars; /* max response length (default 300) */
    uint32_t min_response_chars; /* min response length (default 15) */
    uint32_t decay_days;         /* memory decay window in days (default 30) */
    uint32_t dedup_threshold;    /* memory dedup similarity % (default 70) */
    uint32_t
        missed_msg_threshold_sec; /* seconds before acknowledging missed message (default 1800) */
    uint32_t callback_window;     /* callback delay window in seconds (default 300) */
    uint32_t pattern_threshold;   /* conversation pattern match threshold % (default 50) */
    uint32_t tapback_skip_pct;    /* probability to skip tapback/reaction % (default 20) */
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
    const char *persona;
    const char *system_prompt;
    const char **enabled_tools;
    size_t enabled_tools_count;
    const char **enabled_skills;
    size_t enabled_skills_count;
    const char *role; /* lead, builder, reviewer, tester */
    uint8_t autonomy_level;
    double temperature;
    double budget_usd;
    uint32_t max_iterations;
    const char *description;  /* human-readable, for orchestrator matching */
    const char *capabilities; /* comma-sep tags for orchestrator capability matching */
    bool is_default;
} hu_named_agent_config_t;

void hu_named_agent_config_free(hu_allocator_t *alloc, hu_named_agent_config_t *cfg);

typedef struct hu_session_config {
    hu_dm_scope_t dm_scope;
    uint32_t idle_minutes;
    const hu_identity_link_t *identity_links;
    size_t identity_links_len;
} hu_session_config_t;

/* ── MCP config (used by mcp_manager) ─────────────────────────────────── */

typedef struct hu_mcp_config {
    bool enabled;
} hu_mcp_config_t;

/* ── Hook pipeline config ─────────────────────────────────────────────── */

#define HU_HOOKS_CONFIG_MAX 16

typedef struct hu_hook_config_entry {
    char *name;           /* human-readable hook name */
    char *event;          /* "pre_tool_execute" or "post_tool_execute" */
    char *command;        /* shell command to run */
    uint32_t timeout_sec; /* 0 = default (30s) */
    bool required;        /* if true, execution error => deny */
} hu_hook_config_entry_t;

typedef struct hu_hooks_config {
    hu_hook_config_entry_t entries[HU_HOOKS_CONFIG_MAX];
    size_t entries_count;
    bool enabled;
} hu_hooks_config_t;

#endif /* HU_CONFIG_TYPES_H */
