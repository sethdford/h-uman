#ifndef HU_AGENT_H
#define HU_AGENT_H

#include "human/agent/app_config.h"
#include "human/agent/approval_gate.h"
#include "human/agent/chaos.h"
#include "human/agent/checkpoint.h"
#include "human/agent/commitment_store.h"
#include "human/agent/data_quality.h"
#include "human/agent/degradation.h"
#include "human/agent/gvr.h"
#include "human/agent/instruction_discover.h"
#include "human/agent/kv_cache.h"
#include "human/agent/mailbox.h"
#include "human/agent/mar.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/scratchpad.h"
#include "human/agent/self_model.h"
#include "human/agent/spawn.h"
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_commitment.h"
#include "human/agent/superhuman_emotional.h"
#include "human/agent/superhuman_predictive.h"
#include "human/agent/superhuman_silence.h"
#include "human/agent/task_list.h"
#include "human/agent/team.h"
#include "human/agent/timing.h"
#include "human/agent/token_budget.h"
#include "human/agent/workflow_event.h"
#include "human/agent/worktree.h"
#include "human/behavior/pressure_history.h"
#include "human/channel.h"
#include "human/contact_send_recency.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/slice.h"
#include "human/cost.h"
#include "human/filler_recency.h"
#include "human/memory.h"
#include "human/memory/policy.h"
#include "human/memory/retrieval.h"
#include "human/ml/m3_frontier_adapter.h"
#include "human/security/delegation.h"
#include "human/security/escalate.h"
#include "human/tools/validation.h"
#include "human/usage.h"
#include "human/webhook.h"
#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/meta_learning.h"
#endif
#include "human/agent/growth_narrative.h"
#include "human/agent/process_reward.h"
#include "human/agent/reflection.h"
#include "human/cognition/attachment.h"
#include "human/cognition/dual_process.h"
#include "human/cognition/emotional.h"
#include "human/cognition/metacognition.h"
#include "human/cognition/novelty.h"
#include "human/cognition/rupture_repair.h"
#include "human/cognition/trust.h"
#include "human/hook.h"
#include "human/memory/adaptive_rag.h"
#include "human/memory/personal_model.h"
#include "human/memory/self_rag.h"
#include "human/memory/stm.h"
#include "human/memory/tiers.h"
#include "human/ml/dpo.h"
#include "human/observability/bth_metrics.h"
#include "human/observer.h"
#include "human/permission.h"
#include "human/persona.h"
#include "human/persona/circadian.h"
#include "human/persona/creative_voice.h"
#include "human/persona/genuine_boundaries.h"
#include "human/persona/narrative_self.h"
#include "human/persona/relationship.h"
#include "human/persona/somatic.h"
#include "human/persona/voice_maturity.h"
#include "human/provider.h"
#include "human/security.h"
#include "human/security/audit.h"
#include "human/security/policy_engine.h"
#include "human/tool.h"
#include "human/voice.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Agent — orchestration loop, history, tool dispatch, slash commands
 * ────────────────────────────────────────────────────────────────────────── */

/* Owned chat message (content is heap-allocated, caller owns lifecycle) */
typedef struct hu_owned_message {
    hu_role_t role;
    char *content; /* owned; freed by hu_agent_clear_history */
    size_t content_len;
    char *name; /* optional, for tool results */
    size_t name_len;
    char *tool_call_id; /* optional */
    size_t tool_call_id_len;
    hu_tool_call_t *tool_calls; /* optional, owned; for assistant with tool_calls */
    size_t tool_calls_count;
    hu_content_part_t *content_parts; /* optional, owned; for multimodal messages */
    size_t content_parts_count;
} hu_owned_message_t;

typedef struct hu_agent hu_agent_t;

/* SOTA neural subsystem fields, extracted from hu_agent_t to reduce struct size.
 * Embedded as hu_agent_t::sota — always present, never NULL. */
typedef struct hu_agent_extensions {
    hu_srag_config_t srag_config;
    hu_adaptive_rag_t adaptive_rag;
    hu_tier_manager_t tier_manager;
    hu_prm_config_t prm_config;
    hu_dpo_collector_t dpo_collector;
    int64_t current_trajectory_id; /* ML trajectory for RL training (0 = inactive) */
    bool sota_initialized;

    hu_gvr_config_t gvr_config;
    hu_provider_degradation_config_t degradation_config;
    hu_token_budget_config_t token_budget;
    hu_tool_validator_t tool_validator;
    hu_dq_config_t dq_config;
    hu_mar_config_t mar_config;
    hu_mem_policy_t mem_policy;
    hu_chaos_engine_t chaos_engine;
    hu_checkpoint_store_t checkpoint_store;
    hu_scratchpad_t scratchpad;
    hu_escalate_protocol_t escalate_protocol;
} hu_agent_extensions_t;

typedef struct hu_frontier_state {
    hu_somatic_state_t somatic;
    hu_novelty_tracker_t novelty;
    hu_attachment_state_t attachment;
    hu_rupture_state_t rupture;
    hu_narrative_self_t narrative;
    hu_creative_voice_t creative_voice;
    hu_growth_narrative_t growth;
    hu_genuine_boundary_set_t boundaries;
    hu_tcal_state_t trust;
    bool initialized;
} hu_frontier_state_t;

/* Cognition, caching, and workflow infrastructure extracted from hu_agent_t.
 * Embedded as hu_agent_t::infra — always present, never NULL. */
typedef struct hu_agent_infra {
    /* Cognition subsystems */
    hu_emotional_cognition_t emotional_cognition;
    hu_metacognition_t metacognition;
    hu_cognition_mode_t current_cognition_mode;
#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *cognition_db; /* shared DB for evolving + episodic */
#endif

    /* Cross-turn caches */
    struct hu_prompt_cache *prompt_cache;     /* owned; NULL until first turn */
    struct hu_tool_cache_ttl *tool_cache_ttl; /* owned; NULL until first turn */
    hu_kv_cache_manager_t *kv_cache;          /* owned; NULL until first turn */
    struct hu_context_engine *context_engine; /* owned; NULL = use legacy behavior */
    struct hu_acp_inbox *acp_inbox;           /* owned; NULL until multi_agent enabled */
    struct hu_speculative_cache *speculative_cache;
    struct hu_semantic_cache *response_cache; /* optional; embedding-based response cache */

    /* W10: last model version seen — triggers KV cache invalidation on change */
    char kv_last_model[64];

    /* Workflow infrastructure */
    struct hu_idempotency_registry *idempotency_registry; /* optional; NULL = no dedup */
    hu_workflow_event_log_t *workflow_log;                /* optional; NULL = no event logging */
    hu_gate_manager_t *gate_manager;                      /* optional; NULL = no approval gates */
    hu_delegation_registry_t *delegation_registry;        /* optional; NULL = no delegation */
    hu_webhook_manager_t *webhook_manager;                /* optional; NULL = no webhooks */
} hu_agent_infra_t;

/* Optional context pressure config. Pass to hu_agent_from_config; NULL = use defaults. */
typedef struct hu_agent_context_config {
    uint64_t token_limit;   /* 0 = resolve from model at runtime */
    float pressure_warn;    /* warn at this ratio (default 0.85) */
    float pressure_compact; /* auto-compact at this ratio (default 0.95) */
    float compact_target;   /* compact until below this ratio (default 0.70) */
    bool compact_context;
    bool llm_compiler_enabled;
    bool mcts_planner_enabled;
    bool tree_of_thought;
    bool constitutional_ai;
    /* US-7.9: pure string-pattern style self-critique (post-gen). */
    bool constitutional_style_rules_enabled;
    bool speculative_cache;
    bool tool_routing_enabled;
    bool multi_agent;
    bool hula_enabled;
    bool compaction_use_structured; /* use XML structured summaries in compaction */
} hu_agent_context_config_t;

/* Called when a tool needs user approval before execution.
 * tool_name/args describe the pending action.
 * Return true to approve, false to deny. */
typedef bool (*hu_agent_approval_cb)(void *ctx, const char *tool_name, const char *args);

struct hu_agent {
    hu_allocator_t *alloc;
    const struct hu_config *config;
    hu_provider_t provider;
    hu_tool_t *tools;
    size_t tools_count;
    hu_tool_spec_t *tool_specs; /* owned; built from tools */
    size_t tool_specs_count;
    hu_memory_t *memory;                     /* optional, may be NULL */
    hu_personal_model_t personal_model;      /* in-process model-of-user (facts, style, topics) */
    hu_retrieval_engine_t *retrieval_engine; /* optional; when set, memory_loader uses it */
    hu_session_store_t *session_store;       /* optional, may be NULL */
    hu_observer_t *observer;                 /* optional, may be NULL */
    hu_bth_metrics_t *bth_metrics;           /* optional; set by daemon for BTH observability */
    /* Sprint 46 R5.3 — in-process PersonaEval classifier. Loaded lazily at
     * init time; NULL when model file absent (downstream calls degrade
     * gracefully to neutral 0.5). Owned by the agent; freed in hu_agent_deinit. */
    struct hu_persona_eval_model *persona_eval;
    /* Sprint 55 B3 — per-agent prompt budget observer. Accumulates per-
     * field byte stats across every hu_prompt_build_system call. When
     * non-NULL AND cfg.prompt_budget_trim_enabled=true, the builder
     * uses it to skip DEAD-tagged fields. Owned by the agent; init
     * during hu_agent_init (when cfg.prompt_budget.enabled=true);
     * freed in hu_agent_deinit. NULL on the legacy path = zero
     * behavioral change. The doctor check inspects this pointer via
     * the registry adapter ctx. */
    struct hu_prompt_budget *prompt_budget;
    hu_security_policy_t *policy;      /* optional, may be NULL */
    hu_cost_tracker_t *cost_tracker;   /* optional, may be NULL */
    hu_usage_tracker_t *usage_tracker; /* optional, per-provider token tracking */

    char *model_name; /* owned */
    size_t model_name_len;
    char *default_provider; /* owned */
    size_t default_provider_len;
    double temperature;
    char *workspace_dir; /* owned */
    size_t workspace_dir_len;
    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    bool auto_save;
    uint8_t autonomy_level;    /* 0=readonly, 1=supervised, 2=full */
    char *custom_instructions; /* optional user system instructions */
    size_t custom_instructions_len;

    /* Per-turn context (set by daemon before hu_agent_turn, not owned) */
    const char *contact_context;
    size_t contact_context_len;
    const char *conversation_context;
    size_t conversation_context_len;
    /* Active scene-direction text for this turn (e.g. "casual short, dry").
     * Set by the daemon before hu_agent_turn / hu_agent_turn_stream_v2; not
     * owned by the agent. The response_guard uses this as
     * `hu_guard_context_t.director_text` to reject any verbatim quote of
     * the directive in the model's reply (G6, Sprint 31).
     * NULL / 0 = no signal — guard does not enforce G6 for this turn. */
    const char *scene_direction_text;
    size_t scene_direction_text_len;

    /* Sprint 37 — Ring buffer of recent director strings (excluding the
     * currently-active one in `scene_direction_text`). Owned by the
     * agent, allocated/freed on `agent->alloc`. Each entry is a
     * NUL-terminated copy of a past director string, truncated to
     * HU_DIRECTOR_TEXT_CAP bytes. G6 iterates the buffer to catch
     * cross-turn director echoes — model output that quotes a previous
     * turn's director rather than the current one.
     *
     * Slot indexing: most-recent-first. Slot 0 = previous turn,
     * slot 1 = two turns ago, etc.
     *
     * `director_history_count` is the number of valid slots, capped
     * at HU_DIRECTOR_HISTORY_MAX. Push is idempotent on NULL or text
     * shorter than HU_GUARD_DIRECTOR_ECHO_MIN_MATCH (30 bytes — would
     * not trip G6 anyway). */
#ifndef HU_DIRECTOR_HISTORY_MAX
#define HU_DIRECTOR_HISTORY_MAX 4
#endif
#ifndef HU_DIRECTOR_TEXT_CAP
#define HU_DIRECTOR_TEXT_CAP 256
#endif
    char *director_history[HU_DIRECTOR_HISTORY_MAX];
    size_t director_history_lens[HU_DIRECTOR_HISTORY_MAX];
    size_t director_history_count;

    uint32_t max_response_chars;

    /* Per-turn model override (set by daemon/CLI, not owned; NULL = use default) */
    const char *turn_model;
    size_t turn_model_len;
    double turn_temperature;  /* 0.0 = use agent default */
    int turn_thinking_budget; /* 0 = no thinking config */
    int turn_tier;            /* hu_cognitive_tier_t from model router, -1 = unset */
    bool proactive_turn;      /* true = proactive check-in; skip storing prompt as memory */
    hu_timing_model_t *timing_model;

    /* Per-turn A/B evaluation: channel history for quality scoring (set by daemon, not owned) */
    const hu_channel_history_entry_t *ab_history_entries;
    size_t ab_history_count;

    /* Per-contact memory scoping (set by daemon, not owned) */
    const char *memory_session_id;
    size_t memory_session_id_len;

    hu_owned_message_t *history; /* owned array; grows */
    size_t history_count;
    size_t history_cap;
    uint64_t total_tokens;

    /* Context pressure: token_limit from config (0 = resolve from model) */
    uint64_t token_limit;
    float context_pressure_warn;
    float context_pressure_compact;
    float context_compact_target;
    bool compact_context_enabled;
    bool context_pressure_warning_85_emitted;
    bool context_pressure_warning_95_emitted;

    /* Cached static portion of system prompt (rebuilt only when config changes) */
    char *cached_static_prompt;
    size_t cached_static_prompt_len;
    size_t cached_static_prompt_cap;

    /* Response verifier telemetry (W4 wire). Updated every turn the verifier
     * runs. The wire fires whenever `verifier_graph` is non-NULL OR the agent
     * has any memory at all (graph-less verification still extracts claims;
     * supported-vs-flagged is only meaningful when verifier_graph is set).
     * Tests inspect these to prove the verifier ran on the response path. */
    struct hu_graph *verifier_graph;  /* optional, not owned. Decoupled from
                                       * agent->memory: legacy vector store is
                                       * hu_memory_t; W7 dispatching facade is
                                       * hu_memory_facade_t (graph-only verify path). */
    uint64_t verifier_runs;           /* total invocations across the agent's lifetime */
    uint64_t verifier_claims_total;   /* total claims extracted */
    uint64_t verifier_claims_flagged; /* total claims scoring below threshold */
    uint64_t injection_blocks;        /* input guard: high-risk injection detections */

    /* W5 producer telemetry (FIX 9). Counts persona deltas the agent proposed
     * to its evolver during inbound user-message processing. The 3 AM evolver
     * (FIX 3) reads the same table on the consumer side. */
    uint64_t persona_deltas_proposed;

    /* W7+W9 facade handle (FIX 12). Opaque `struct hu_w7_facade`: holds the
     * W7 `hu_memory_facade_t` plus world-model / self-RAG helpers so agent_turn
     * does not need to include W7/W9 headers directly. Wired via
     * hu_agent_bind_sqlite_graph (daemon, CLI, spawn). Legacy chat memory remains
     * `agent->memory` (hu_memory_t). */
    struct hu_w7_facade *w7_facade;
    struct hu_audit_log *w15_audit_log; /* W15 audit log for facade write/erase ops */
    uint64_t world_model_loads;         /* telemetry: per-turn world_model render count */

    /* B8 — Optional theory-of-mind scenario merged into the rendered world
     * model on the next turn (eval / benchmark hook). Empty strings disable
     * the merge. Set via `hu_agent_set_tom_scenario`; consumed by
     * `hu_w7_render_world_model` in agent_turn / agent_stream. NEVER
     * populated on production turns. */
    char tom_scenario_premise[256];
    char tom_scenario_question[256];
    char tom_scenario_category[64];

    /* W11 self-RAG telemetry (FIX 12b). Sibling to verifier_* counters --
     * self-RAG runs alongside hu_response_verify on the response path, with
     * a richer atomic-claim model and an explicit abstention outcome. */
    uint64_t self_rag_runs;
    uint64_t self_rag_claims_total;
    uint64_t self_rag_claims_flagged;
    uint64_t self_rag_abstentions;
    /* W11 P0 #1 — counts ABSTAINED outcomes that actually replaced the
     * user-visible response with the refusal template (i.e. ran under
     * SOFT or STRICT mode). `self_rag_abstentions` counts ABSTAINED
     * outcomes regardless of whether the response was swapped (telemetry
     * mode increments it without rendering). The delta between the two
     * is the rate at which we silently shipped unverified drafts. */
    uint64_t self_rag_refusals_rendered;

    /* B11 P1 — Cross-turn pressure history. The single-message detector
     * (`hu_pressure_detect`) catches authority cues / shouting / reassertion
     * phrasing within one turn. Sycophancy attacks are usually *cumulative*:
     * the user reasserts the same wrong claim three or four turns in a row.
     * `pressure_history` records a small fixed-size window of recent user
     * messages + the assistant's last trust action, so `agent_turn` can ask
     * "is this a reassertion of a recent claim I already pushed back on?"
     * before calling `hu_trust_calibrate`. Init via `memset(&agent, 0,
     * sizeof(agent))` (the struct is POD with no embedded pointers).
     * Footprint: ~1.2 KB. */
    hu_pressure_history_t pressure_history;

    /* W14 sleep-time compute scheduler handle (FIX 13). Same opaque-tag
     * trick as w7_facade above. Opened by hu_agent_bind_sqlite_graph after
     * hu_w7_facade_open; ticked once per main-loop iteration; closed BEFORE
     * w7_facade_close (the scheduler borrows the facade's memory handle). */
    struct hu_w14_scheduler *w14_scheduler;
    uint64_t scheduler_ticks;       /* telemetry: total ticks since startup */
    int64_t scheduler_last_tick_ms; /* telemetry: most recent tick wall time */

    volatile sig_atomic_t cancel_requested; /* set by SIGINT handler to abort turn */

    hu_agent_approval_cb approval_cb; /* optional; if NULL, approval-required = denied */
    void *approval_ctx;

    /* TTS (text-to-speech) auto-playback */
    bool tts_enabled;                /* if true, play responses as audio */
    hu_voice_config_t *voice_config; /* optional; if NULL, TTS is skipped even if enabled */

    hu_arena_t *turn_arena; /* per-turn arena for ephemeral allocations */

    hu_agent_pool_t *agent_pool;
    hu_mailbox_t *mailbox;
    uint64_t agent_id;                /* used for mailbox registration; 0 = use (uintptr_t)agent */
    uint32_t spawn_depth;             /* 0 = root session; +1 per nested agent_spawn */
    struct hu_skillforge *skillforge; /* optional; loaded skills for prompt injection */
    hu_embedder_t *skill_route_embedder; /* optional; NOT owned — cosine skill routing when set */
    struct hu_agent_registry *agent_registry; /* optional; named agent definitions */
    hu_worktree_manager_t *worktree_mgr;
    hu_team_t *team;
    hu_task_list_t *task_list;
    hu_policy_engine_t *policy_engine;

    hu_audit_logger_t *audit_logger;
    struct hu_undo_stack *undo_stack;

    /* Delegation and authorization */
    char delegation_token_id[64]; /* current delegation token for agent-to-agent authorization */

    /* Superhuman intelligence features */
    struct hu_awareness *awareness;      /* optional; bus-based situational awareness */
    struct hu_cron_scheduler *scheduler; /* optional; in-memory cron scheduler for agent jobs */
    hu_reflection_config_t reflection;
    struct hu_outcome_tracker *outcomes; /* optional; tracks tool results and user corrections */

    /* W13 on-device learner. Optional. When set, signal sources
     * (delta_observer, outcome tracker) emit pending training signals
     * through the bridge in src/ml/learner_bridge.c. The W14 sleep
     * scheduler later drains the pending buffer and trains. NULL on
     * disabled installations or when the learner backend declined to
     * open — agent code must guard with `agent->learner != NULL`. */
    struct hu_learner *learner;

#ifdef HU_ENABLE_ML
    /* M3 frontier adapter stub (optional). Opened from
     * `personalization.m3_adapter_probe_path` at bootstrap when set;
     * `hu_agent_m3_on_provider_success` runs after each successful provider
     * LLM interaction (chat, stream_chat, GVR, constitutional, metacog regen,
     * guard retry, streaming rethink). Owned; closed in `hu_agent_deinit`. */
    struct hu_m3_frontier_adapter *m3_adapter;
    /* Phase C2 (2026-05-18): string→uint16 id map for outcome clustering.
     * Resolves agent->model_name and the provider's active adapter into
     * stable small ids that go into the outcome record's `m` and `a`
     * fields. Persists to ~/.human/training-data/m3_id_map.json so ids
     * are stable across daemon restarts. Owned; destroyed in
     * `hu_agent_deinit`. NULL when not attached → outcomes record 0. */
    struct hu_m3_id_map *m3_id_map;
    /* Phase G1 (2026-05-18): per-contact adapter routes — loaded
     * alongside the id_map from ~/.human/training-data/m3_contact_routes.json.
     * Read by `hu_agent_m3_route_per_turn` at the start of each turn.
     * NULL = no routes file present; turn proceeds with current adapter. */
    struct hu_m3_contact_routes *m3_contact_routes;
    /* G1: tracks the currently-loaded MLX adapter (path) so per-turn
     * routing only fires the HTTP swap call when the target adapter
     * differs from what's already loaded. Owned; freed in
     * `hu_agent_deinit`. NULL = no swap has been performed in this
     * session (next route lookup with a non-NULL adapter will swap). */
    char *m3_active_adapter_path;
#endif

    /* Spec 2026-05-19 self-model-scaffold — Phase B (AC-SM-1, AC-SM-2).
     *
     * Per-turn behavioral observation ring. Initialized in
     * `hu_agent_from_config` and freed in `hu_agent_deinit`. The write
     * site is `hu_agent_m3_record_chat_outcome` (single-canonical-write-
     * site invariant — see AC-SM-2 grep test). Records hashes / sizes /
     * enums only; NEVER content (AC-SM-7).
     *
     * The struct is embedded by value: it owns a small heap slab for the
     * record slots but the outer struct lives inline on the agent. Zero-
     * cost when HU_ENABLE_SELF_MODEL is OFF — the stub init returns HU_OK
     * without allocating and record() is a no-op. */
    hu_agent_behavior_log_t behavior_log;

    /* Spec 2026-05-19 self-model-scaffold — Phase B.
     *
     * Per-turn metrics staged by callers BEFORE invoking the canonical
     * write site `hu_agent_m3_on_provider_success`. The fields are
     * additive: callers that have not been migrated leave them zero;
     * `on_provider_success` records whatever is present. This shape lets
     * the canonical write site live inside one function without touching
     * every one of the 11 call paths to `on_provider_success`.
     *
     * Cleared by `on_provider_success` after every recorded turn so a
     * stale stash never bleeds into the next turn's record. */
    struct {
        uint32_t response_length_chars;
        uint32_t response_length_tokens_est;
        uint32_t tool_sequence_hash;
        uint16_t tool_count;
        uint8_t emotional_register;
        uint8_t persona_delta_kind;
        uint32_t response_latency_ms;
        bool has_data; /* true once any field has been stashed for this turn */
    } behavior_log_pending;

    /* Per-turn behavioral state (Spec 2026-05-19 AC-SM-1 follow-up).
     * Reset at turn start, populated as the turn progresses:
     *   - tool_count + tool_sequence_hash: incremented at each tool
     *     dispatch via hu_agent_turn_state_track_tool()
     *   - emotional_register: set after world-model merge_self_emotion
     *     via hu_agent_turn_state_set_emotional_register()
     *   - persona_delta_kind: set when a delta is applied via
     *     hu_agent_turn_state_set_persona_delta()
     *
     * hu_agent_internal_emit_behavior_record() reads from here when the
     * stash (behavior_log_pending above) left the corresponding field
     * zero — "override pattern" where explicit stashes win and the
     * turn-state fills in the rest.
     *
     * Privacy: counters + hash + enums only. No content. */
    struct {
        uint16_t tool_count;
        uint32_t tool_sequence_hash; /* running FNV-1a of "name1|name2|..." */
        uint8_t emotional_register;  /* hu_agent_emotional_register_t */
        uint8_t persona_delta_kind;  /* hu_agent_persona_delta_kind_t */
    } current_turn_state;

    bool chain_of_thought;    /* inject reasoning instructions into prompt */
    bool on_device_available; /* true if on-device inference server was detected at startup */
    char *persona_prompt;     /* custom identity override; owned */
    size_t persona_prompt_len;

    /* Set by channel before turn; used for per-channel persona overlays. Not owned. */
    const char *active_channel;
    size_t active_channel_len;

    /* Set by cron dispatch before turn; used for per-automation cost tracking. 0 = interactive. */
    uint64_t active_job_id;

    char trace_id[37]; /* UUID v4 hex string + NUL, regenerated per conversation turn */

    char session_id[64]; /* current session persistence ID; empty = no active session */

    hu_stm_buffer_t stm; /* short-term memory buffer for session context */

    hu_commitment_store_t *commitment_store; /* optional; when memory is set */

    hu_pattern_radar_t radar; /* pattern observation tracker */

    hu_superhuman_registry_t superhuman;
    hu_superhuman_commitment_ctx_t superhuman_commitment_ctx;
    hu_superhuman_emotional_ctx_t superhuman_emotional_ctx;
    hu_superhuman_silence_ctx_t superhuman_silence_ctx;

    bool llm_compiler_enabled;
    bool mcts_planner_enabled;
    bool tool_routing_enabled;
    bool tree_of_thought_enabled;
    bool hula_enabled;
    bool compaction_use_structured; /* use XML structured summaries in compaction */

    bool constitutional_enabled;
    /* US-7.9: enables the pure-string style self-critique pass after
     * each LLM generation (separate from the LLM-judge constitutional
     * pass above).  Default false. */
    bool style_rules_enabled;
    bool multi_agent_enabled;
    bool lean_prompt; /* strip heavy contexts for fast local-model texting */

#ifdef HU_ENABLE_SQLITE
    hu_meta_params_t meta_params;
#endif

    hu_relationship_state_t relationship; /* session-based warmth adaptation */

    hu_persona_t *persona; /* loaded from config; owned; NULL if no persona configured */
    hu_voice_profile_t voice_profile;
    bool voice_profile_initialized;
    bool humanness_ctx_owned; /* true when conversation_context was built by humanness module */
    /* Pointer + length of the humanness allocation. Tracked separately from
     * conversation_context because the daemon may legitimately overwrite
     * conversation_context with its own buffer between turns; the free path
     * must only release what humanness actually allocated to avoid the
     * production double-free fixed in tests/test_humanness_context.c
     * (free_context_handles_daemon_override_after_humanness_owned). */
    char *humanness_ctx_buf;
    size_t humanness_ctx_buf_len;
    char *persona_name;
    size_t persona_name_len;

    /* Tool-level streaming: set before turn to enable tool execute_streaming callbacks */
    void (*tool_stream_cb)(void *ctx, const char *data, size_t len);
    void *tool_stream_ctx;

    /* SOTA neural subsystems (extracted to reduce main struct field count) */
    hu_agent_extensions_t sota;
    hu_frontier_state_t frontiers;

    /* Permission tiers */
    hu_permission_level_t permission_level;      /* effective (may be escalated) */
    hu_permission_level_t permission_base_level; /* configured base level */
    bool permission_escalated;                   /* true during temporary escalation */

    /* Hook pipeline: pre/post tool execution interception */
    hu_hook_registry_t *hook_registry; /* optional; NULL = no hooks */

    /* Instruction file discovery cache */
    hu_instruction_discovery_t *instruction_discovery;

    /* Cognition, caching, and workflow infrastructure (extracted) */
    hu_agent_infra_t infra;

    /* Media generation: tool-produced file paths accumulated per turn */
    char *generated_media[4];
    size_t generated_media_count;

    /* Per-chat filler anti-repetition LRU (PCTT Task 3).
     * Tracks the last filler index emitted per chat_id so the selector can
     * avoid picking the same filler twice in a row.  In-memory only; loss on
     * restart is acceptable.  Zero-initialised by memset in hu_agent_from_config. */
    hu_filler_recency_t filler_recency;

    /* Per-contact send-path recency (memory-scoping FU-1, plan
     * docs/plans/2026-05-15-memory-scoping-followups.md).  Tracks the last
     * outbound path (reactive vs proactive vs scheduled vs photo vs morning)
     * so proactive paths in the daemon can DEFER when a reactive turn has
     * fired for the same contact within the recency window.  In-memory only;
     * loss on restart is acceptable.  Zero-initialised by memset in
     * hu_agent_from_config. */
    hu_contact_send_recency_t contact_send_recency;
};

/* Create agent from minimal config (no full config loader yet).
 * ctx_cfg: optional context pressure config; NULL = use defaults. */
hu_error_t hu_agent_from_config(
    hu_agent_t *out, hu_allocator_t *alloc, hu_provider_t provider, const hu_tool_t *tools,
    size_t tools_count, hu_memory_t *memory, hu_session_store_t *session_store,
    hu_observer_t *observer, hu_security_policy_t *policy, const char *model_name,
    size_t model_name_len, const char *default_provider, size_t default_provider_len,
    double temperature, const char *workspace_dir, size_t workspace_dir_len,
    uint32_t max_tool_iterations, uint32_t max_history_messages, bool auto_save,
    uint8_t autonomy_level, const char *custom_instructions, size_t custom_instructions_len,
    const char *persona, size_t persona_len, const hu_agent_context_config_t *ctx_cfg);

/* Phase 4A: Narrow facade over hu_config_t. Accepts app_config projection
 * instead of full config, reducing agent-core coupling on the 65-substruct
 * god-config. Provider and tools still injected (provider vtable is caller's
 * responsibility to construct/pass). */
hu_error_t hu_agent_from_app_config(hu_agent_t *out, hu_allocator_t *alloc, hu_provider_t provider,
                                    const hu_tool_t *tools, size_t tools_count, hu_memory_t *memory,
                                    hu_session_store_t *session_store, hu_observer_t *observer,
                                    hu_security_policy_t *policy,
                                    const hu_agent_app_config_t *app_cfg);

void hu_agent_deinit(hu_agent_t *agent);

#ifdef HU_ENABLE_SQLITE
struct hu_graph;
/* Wire `agent->verifier_graph`, open W7 facade (`agent->w7_facade`), and W14 scheduler when
 * missing. Matches daemon/CLI/spawn production paths. `graph` is not owned. Idempotent when
 * `agent->w7_facade` is already set (still refreshes `verifier_graph`). */
hu_error_t hu_agent_bind_sqlite_graph(hu_agent_t *agent, struct hu_graph *graph,
                                      hu_allocator_t *alloc);
#endif

/* Optional: set mailbox and register agent for multi-agent messaging. Caller owns mailbox. */
void hu_agent_set_mailbox(hu_agent_t *agent, hu_mailbox_t *mailbox);

/* Optional: share parent's SkillForge for prompt catalog + skill_run. Caller owns skillforge.
 * Pass NULL to clear. */
void hu_agent_set_skillforge(hu_agent_t *agent, struct hu_skillforge *skillforge);

/* Optional: share session cost accounting with spawned workers (borrowed pointer). */
void hu_agent_set_cost_tracker(hu_agent_t *agent, hu_cost_tracker_t *tracker);

/* Optional: set shared task list for multi-agent collaboration. Caller owns task_list. */
void hu_agent_set_task_list(hu_agent_t *agent, hu_task_list_t *task_list);

/* Optional: set retrieval engine for semantic/hybrid recall. Caller owns engine lifecycle. */
void hu_agent_set_retrieval_engine(hu_agent_t *agent, hu_retrieval_engine_t *engine);

/* Optional embedder for semantic skill routing in hu_agent_turn (same instance as retrieval is
 * typical). Not owned by the agent — cleared on deinit without calling embedder deinit. */
void hu_agent_set_skill_route_embedder(hu_agent_t *agent, hu_embedder_t *embedder);

/* Optional: set awareness for situational context injection. Caller owns awareness lifecycle. */
struct hu_awareness;
void hu_agent_set_awareness(hu_agent_t *agent, struct hu_awareness *awareness);

/* Get the agent's awareness (may be NULL). */
const struct hu_awareness *hu_agent_get_awareness(const hu_agent_t *agent);

/* Optional: set outcome tracker for continuous learning. Caller owns tracker lifecycle. */
struct hu_outcome_tracker;
void hu_agent_set_outcomes(hu_agent_t *agent, struct hu_outcome_tracker *tracker);

/* Optional: set on-device learner for personalisation signal collection.
 * Caller owns learner lifecycle (open with hu_learner_open_default, free
 * with hu_learner_close). When set, signal-source modules emit signals
 * into the learner's pending buffer; the sleep-time scheduler trains
 * later. Pass NULL to detach. */
struct hu_learner;
void hu_agent_set_learner(hu_agent_t *agent, struct hu_learner *learner);

/* M3 frontier adapter (stub): attach is a no-op when HU_ENABLE_ML is off.
 * `hu_agent_m3_on_provider_success` is safe to call always (no-op when ML off
 * or no adapter); call after each successful frontier LLM interaction
 * (primary chat/stream_chat, GVR, constitutional, metacog regen, guard retry,
 * streaming rethink). */
void hu_agent_m3_adapter_attach(hu_agent_t *agent, const char *path);
void hu_agent_m3_on_provider_success(hu_agent_t *agent);

/* Spec 2026-05-19 self-model-scaffold — Phase B helper.
 *
 * Stash per-turn behavioral metrics that `hu_agent_m3_on_provider_success`
 * will fold into the behavior log on the next call. Idempotent within a
 * single turn (subsequent stashes overwrite — last writer wins). Safe to
 * call with NULL agent (silently no-ops). All scalar fields; never any
 * content strings.
 *
 * Callers populate the fields they have available; un-stashed fields
 * remain zero. The function is intentionally a one-shot batched stash
 * rather than a per-field setter so callers can drop in a single line
 * before invoking `hu_agent_m3_on_provider_success`. */
typedef struct hu_agent_behavior_stash {
    uint32_t response_length_chars;
    uint32_t response_length_tokens_est;
    uint32_t tool_sequence_hash;
    uint16_t tool_count;
    uint8_t emotional_register; /* hu_agent_emotional_register_t */
    uint8_t persona_delta_kind; /* hu_agent_persona_delta_kind_t */
    uint32_t response_latency_ms;
} hu_agent_behavior_stash_t;

void hu_agent_m3_stash_behavior_metrics(hu_agent_t *agent, const hu_agent_behavior_stash_t *stash);

/* Per-turn state helpers (Spec 2026-05-19 AC-SM-1 follow-up).
 *
 * Caller policy:
 *   - reset() at the start of each new turn (one canonical site each in
 *     agent_turn.c + agent_stream.c entry)
 *   - track_tool() at each tool dispatch (4+ sites; cheap to call,
 *     safe with NULL args)
 *   - set_emotional_register() once after world_model_merge_self_emotion
 *   - set_persona_delta() in persona delta apply success branch
 *
 * All are NULL-safe and lock-free. Per AC-SM-7 (privacy): no content
 * captured — counters + hash + enums only. */
void hu_agent_turn_state_reset(hu_agent_t *agent);
void hu_agent_turn_state_track_tool(hu_agent_t *agent, const char *tool_name, size_t tool_name_len);
void hu_agent_turn_state_set_emotional_register(hu_agent_t *agent, uint8_t reg);
void hu_agent_turn_state_set_persona_delta(hu_agent_t *agent, uint8_t kind);

/* Phase G1 (2026-05-18) — per-turn contact-routing hook.
 *
 * Called at the start of each chat turn. Hashes the agent's
 * memory_session_id, looks up the target adapter in
 * `agent->m3_contact_routes`, and (if different from the currently
 * loaded adapter) POSTs /v1/adapters/swap on the MLX server.
 *
 * No-op when:
 *   - agent or its m3_contact_routes is NULL (cold install or
 *     missing routes file)
 *   - lookup returns NULL (no specific route + no default_adapter)
 *   - lookup returns the same path as agent->m3_active_adapter_path
 *     (already loaded — no swap needed)
 *
 * On swap failure (MLX server unreachable, swap returns non-200):
 * log a warning and continue with the currently-loaded adapter.
 * Inference correctness MUST NOT depend on this hook succeeding;
 * routing is an optimization, not a correctness contract.
 *
 * The MLX URL is read from the HUMAN_MLX_URL env var or defaults
 * to http://127.0.0.1:8741 (matching scripts/mlx-server.py). */
void hu_agent_m3_route_per_turn(hu_agent_t *agent);

/* Phase B1 redefined (2026-05-17 round 2): record a structured inference
 * outcome to the M3 adapter's ring buffer. Safe to call always (no-op when
 * ML off, no adapter attached, or agent NULL). Computes prompt/response
 * hashes via hu_m3_outcome_hash_bytes — callers don't have to.
 *
 * Args:
 *   - prompt / prompt_len: the system+user prompt sent to the model
 *   - response / response_len: the cleaned final response
 *   - latency_ms: end-to-end inference duration
 *   - contact_id / contact_id_len: per-contact partition key; pass NULL
 *     when there's no contact context (e.g. agent --once)
 *   - guard_decision: the response_guard chain's terminal decision
 *   - turn_kind: 1 = stream-final, 2 = post-stream batch, 3 = proactive
 *   - usage: optional pointer to the provider's reported token counts.
 *     When non-NULL AND any field is nonzero, prompt_tokens /
 *     completion_tokens land in the outcome record as the provider
 *     reported them. When NULL or all-zero, falls back to a bytes/4
 *     estimate (~English BPE rule). The bytes/4 fallback never reports
 *     zero when there's real content, which is what the M3 driver's
 *     selection policy depends on. Phase C1 (2026-05-18): prefer real
 *     counts when available; the estimate is a 20-40% under-counter
 *     and worse for structured / non-English text. See
 *     `docs/plans/2026-05-17-m3-phase-c-plan.md` C1.
 *
 * Model/adapter ids are looked up from the agent's current configuration
 * inside this helper (set to 0 when unknown). */
void hu_agent_m3_record_chat_outcome(hu_agent_t *agent, const char *prompt, size_t prompt_len,
                                     const char *response, size_t response_len, uint64_t latency_ms,
                                     const char *contact_id, size_t contact_id_len,
                                     hu_m3_guard_decision_t guard_decision, uint8_t turn_kind,
                                     const hu_token_usage_t *usage);

/* Point the agent at a hu_voice_config_t (e.g. from hu_voice_config_from_settings).
 * Borrowed pointers inside that struct must outlive the agent. Pass NULL to disable TTS. */
void hu_agent_set_voice_config(hu_agent_t *agent, hu_voice_config_t *voice_cfg);

/* Run one conversation turn: send to provider, process tool calls, iterate. */
hu_error_t hu_agent_turn(hu_agent_t *agent, const char *msg, size_t msg_len, char **response_out,
                         size_t *response_len_out);

/* Optional: if non-NULL, called for each streaming token delta (CLI mode).
 * Provider must support streaming. When provided, uses stream_chat when available. */
typedef void (*hu_agent_stream_token_cb)(const char *delta, size_t len, void *ctx);

hu_error_t hu_agent_turn_stream(hu_agent_t *agent, const char *msg, size_t msg_len,
                                hu_agent_stream_token_cb on_token, void *token_ctx,
                                char **response_out, size_t *response_len_out);

/* Rich streaming callback: emits typed events including text, thinking, tool calls,
 * and tool results. Used by the gateway and channels for Claude Desktop-style streaming. */
typedef enum hu_agent_stream_event_type {
    HU_AGENT_STREAM_TEXT,        /* assistant text delta */
    HU_AGENT_STREAM_THINKING,    /* reasoning content delta */
    HU_AGENT_STREAM_TOOL_START,  /* tool call beginning (name + id) */
    HU_AGENT_STREAM_TOOL_ARGS,   /* tool arguments delta */
    HU_AGENT_STREAM_TOOL_RESULT, /* tool execution complete (result in data) */
} hu_agent_stream_event_type_t;

typedef struct hu_agent_stream_event {
    hu_agent_stream_event_type_t type;
    const char *data;
    size_t data_len;
    const char *tool_name;
    size_t tool_name_len;
    const char *tool_call_id;
    size_t tool_call_id_len;
    bool is_error; /* for TOOL_RESULT: was the tool execution an error? */
} hu_agent_stream_event_t;

typedef void (*hu_agent_stream_event_cb)(const hu_agent_stream_event_t *event, void *ctx);

/* Streaming turn with rich event callback: streams text between tool calls,
 * executes tools inline, and resumes streaming (Claude Desktop-style). */
hu_error_t hu_agent_turn_stream_v2(hu_agent_t *agent, const char *msg, size_t msg_len,
                                   hu_agent_stream_event_cb on_event, void *event_ctx,
                                   char **response_out, size_t *response_len_out);

/* Run a single message without history (no tool loop for simplicity in Phase 4). */
hu_error_t hu_agent_run_single(hu_agent_t *agent, const char *system_prompt,
                               size_t system_prompt_len, const char *user_message,
                               size_t user_message_len, char **response_out,
                               size_t *response_len_out);

/* Canonical tool-dispatch helper.
 *
 * Wraps every tool invocation in the agent's hook pipeline:
 *   1. Pre-tool hook (if a registry is configured). A DENY decision skips
 *      execution AND short-circuits *out with a "denied by hook" failure.
 *   2. Tool execute() (only if the pre-hook did NOT deny).
 *   3. Post-tool hook (if a registry is configured). Fires UNCONDITIONALLY
 *      — including on the pre-deny path — so auditors observe every
 *      dispatch attempt regardless of outcome.
 *
 * This is the single canonical entry point for tool dispatch. New call sites
 * MUST go through this helper; the audit on 2026-05-16 documented multiple
 * scattered hook-and-execute call sites in src/agent/agent_turn.c and
 * src/agent/agent_stream.c that are progressively migrating to it.
 *
 * Returns HU_OK for normal completion (including hook-denied dispatch).
 * HU_ERR_INVALID_ARGUMENT if agent/tool/out is NULL. The caller frees *out
 * via hu_tool_result_free. */
hu_error_t hu_agent_dispatch_tool(hu_agent_t *agent, hu_tool_t *tool, const char *tool_name,
                                  size_t tool_name_len, const char *args_json, size_t args_json_len,
                                  const hu_json_value_t *args_parsed, hu_tool_result_t *out);

void hu_agent_clear_history(hu_agent_t *agent);

/* Handle slash commands: /help, /quit, /clear, /model, /status.
 * Returns owned response string or NULL if not a slash command.
 * Caller must free the returned string. */
char *hu_agent_handle_slash_command(hu_agent_t *agent, const char *message, size_t message_len);

/* Estimate tokens for a string (rough: ~4 chars per token). */
uint32_t hu_agent_estimate_tokens(const char *text, size_t len);

/* Execute a structured plan (Tier 1.4 planner integration).
 * plan_json format: {"steps": [{"tool": "name", "args": {...}, "description": "..."}]}
 * Returns a summary of execution results. Caller must free summary_out. */
hu_error_t hu_agent_execute_plan(hu_agent_t *agent, const char *plan_json, size_t plan_json_len,
                                 char **summary_out, size_t *summary_len_out);

/* Switch persona mid-conversation. name=NULL or name_len=0 clears the persona.
 * Requires HU_ENABLE_PERSONA to be compiled in; returns HU_ERR_NOT_SUPPORTED otherwise. */
hu_error_t hu_agent_set_persona(hu_agent_t *agent, const char *name, size_t name_len);

/* B8 — Set / clear an optional theory-of-mind scenario merged into the world
 * model on subsequent turns. Pass `NULL` or `""` for any field to clear; all
 * three must be non-empty for the merge to fire. Truncates to the agent's
 * fixed-size buffers. Test / benchmark hook only. */
void hu_agent_set_tom_scenario(hu_agent_t *agent, const char *premise, const char *question,
                               const char *category);

/* W11 P1 — Apply self-RAG verification + (under SOFT/STRICT) the refusal /
 * hedge swap to `draft` against the agent's W7 facade. Single canonical seam
 * shared by `agent_turn` and `agent_stream`; tests can call it directly to
 * exercise the bridge → swap → telemetry chain without needing a real
 * provider draft.
 *
 * `mode` mirrors `hu_verify_mode_t` (OFF/TELEMETRY/SOFT/STRICT). Modes that
 * cannot rewrite (OFF, TELEMETRY) leave `*swapped_out == NULL` even on
 * ABSTAINED outcomes; SOFT/STRICT allocate a new buffer with the deterministic
 * refusal template (or hedge / rewrite) and transfer ownership through
 * `*swapped_out` / `*swapped_len_out`.
 *
 * Side effects on `agent`:
 *   - `self_rag_runs++` on every successful verifier call
 *   - `self_rag_claims_total/flagged += <claim totals>`
 *   - `self_rag_abstentions++` on ABSTAINED outcome (any mode)
 *   - `self_rag_refusals_rendered++` only when ABSTAINED AND a swap actually
 *     happened (i.e. SOFT/STRICT path with a non-empty refusal template)
 *
 * Returns HU_OK with `*swapped_out == NULL` when no swap occurs. Returns
 * HU_ERR_INVALID_ARGUMENT when `agent` lacks a W7 facade or memory session id
 * (the verifier requires both). Tests passing `swapped_out == NULL` get the
 * telemetry-only path even under SOFT/STRICT. */
hu_error_t hu_agent_self_rag_apply(hu_agent_t *agent, const char *draft, size_t draft_len, int mode,
                                   char **swapped_out, size_t *swapped_len_out);

/* W11 P1 — Read-only snapshot of self-RAG telemetry. Any out pointer may be
 * NULL. Safe with `agent == NULL` (writes 0 into every non-NULL out). */
void hu_agent_self_rag_telemetry(const hu_agent_t *agent, uint64_t *runs, uint64_t *abstentions,
                                 uint64_t *refusals_rendered, uint64_t *claims_total,
                                 uint64_t *claims_flagged);

/* Run memory consolidation (merge similar entries, decay old). */
hu_error_t hu_agent_consolidate_memory(hu_agent_t *agent);

/* Reload configuration from ~/.human/config.json:
 * - Re-parse hooks and rebuild hook registry
 * - Update permission level if changed
 * - Re-discover instruction files
 * Returns summary of what changed. Caller must free. */
hu_error_t hu_agent_reload_config(hu_agent_t *agent, char **summary_out, size_t *summary_len_out);

#endif /* HU_AGENT_H */
