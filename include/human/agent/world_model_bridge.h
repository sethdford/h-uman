#ifndef HU_AGENT_WORLD_MODEL_BRIDGE_H
#define HU_AGENT_WORLD_MODEL_BRIDGE_H

/* W9 wire bridge (FIX 12).
 *
 * Historically the W7 facade and legacy store shared the `hu_memory_t` name
 * (now split: `hu_memory_facade_t` vs legacy `hu_memory_t`). This bridge still
 * isolates W7/W9/W11 headers from `agent_turn.c` so the TU stays lean.
 *
 * This bridge gives `agent_turn.c` and `daemon.c` a way to use the W7 facade
 * + `hu_world_model_load` without paying that include cost. The bridge owns
 * its own translation unit (`world_model_bridge.c`) where ONLY the W7
 * headers are pulled in; everyone else talks to the bridge through the
 * unique opaque tag `struct hu_w7_facade`.
 *
 * Same pattern as `agent->verifier_graph` (FIX 2): isolate the type
 * collision behind a fresh forward declaration. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/personal_model.h"
#include "human/provider.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque W7 facade handle. Define lives in world_model_bridge.c. */
struct hu_w7_facade;
typedef struct hu_w7_facade hu_w7_facade_t;

/* Optional persona context (P1.1-P1.3) threaded into hu_w7_render_world_model
 * for persona-grounded ToM synthesis. All fields are optional:
 *   - `persona == NULL` : no persona merge happens (back-compat)
 *   - `channel == NULL` or `channel_len == 0` : persona merged but channel
 *     overlay skipped
 *   - `delta_limit == 0` : no persona-delta SQL read; pass e.g. 8 to fold
 *     the most recent applied deltas in.
 *
 * Wrapped in a struct because the bridge function already has 14 params. */
struct hu_persona;
/* Story F.1 — `tools`/`tools_count` are optional. When set, the bridge
 * calls `hu_world_model_merge_self_capabilities` so the snapshot's
 * `self_model.capabilities[]` is populated. NULL/0 → merge skipped
 * (back-compat with all callers from before F.1). Forward-declared
 * here so callers do not have to include `human/tool.h`.
 *
 * Lifetime: the bridge only reads tool *names* via
 * `tools[i].vtable->name(ctx)` and copies them into the inline slab,
 * so the caller can free the registry the moment this call returns. */
struct hu_tool;

typedef struct hu_persona_context {
    const struct hu_persona *persona;
    const char *channel;
    size_t channel_len;
    size_t delta_limit;
    const struct hu_tool *tools;
    size_t tools_count;
    /* Story F.2 — tools actually used in recent turns (HU_ROLE_TOOL names).
     * Optional; NULL/0 → merge skipped. Pointers are read-only for the
     * duration of the bridge call; names are copied into the snapshot. */
    const char *const *recent_tools_used;
    size_t recent_tools_used_count;
} hu_persona_context_t;

/* Forward-declare provider type at file scope so all `_with_provider`
 * entrypoints below agree on `struct hu_provider *` regardless of whether
 * the caller has already included `human/provider.h`. Without this, each
 * function prototype implicitly declares a scope-local `struct hu_provider`
 * and source-file definitions that pull in the full type get "conflicting
 * types" errors. */
struct hu_provider;

/* Open a W7 facade backed by `graph` (the v1 backends). Caller owns the
 * returned pointer and must close with hu_w7_facade_close. */
hu_error_t hu_w7_facade_open(struct hu_graph *graph, hu_allocator_t *alloc, hu_w7_facade_t **out);

void hu_w7_facade_close(hu_w7_facade_t *facade, hu_allocator_t *alloc);

/* Borrow the W7 memory facade held inside `facade`. NULL if `facade` is NULL.
 * Valid until `hu_w7_facade_close`. */
hu_memory_facade_t *hu_w7_facade_memory_handle(hu_w7_facade_t *facade);

/* Returns the SQLite connection backing the facade's knowledge graph
 * (where autodream writes community_summaries), or NULL. */
struct sqlite3 *hu_w7_facade_graph_db(hu_w7_facade_t *facade);

/* Render the cached world model for `contact_id` into a prompt-ready text
 * block. Returns HU_OK with `*out_text == NULL`, `*out_len == 0` when there
 * is no information worth surfacing -- callers should treat that as "no
 * world model context available" and skip injection.
 *
 * The text format mirrors the persona/personal_model sections in the system
 * prompt (FIX 1): a labeled markdown block with subsections for goals,
 * negatives, theory-of-mind, and recent topics. Caller owns the returned
 * pointer and must free with `alloc->free`.
 *
 * `now_ms == 0` means "use OS clock".
 *
 * Optional ToM scenario (B8): when `tom_premise`, `tom_question`, and
 * `tom_category` are all non-NULL with positive lengths, their synthesized ToM
 * is merged into the loaded world model before formatting. Pass NULL / 0 for
 * each to skip (normal agent turns).
 *
 * M2 ↔ W9 bridge: when `pm` is non-NULL, personal model signal (goals,
 * topics, style, emotion) is merged into the world model before rendering.
 * Pass NULL to skip (backward-compatible).
 *
 * P1.1-P1.3 persona-grounded ToM: when `persona_ctx` is non-NULL with a
 * non-NULL `persona`, the bridge calls `hu_world_model_merge_persona` after
 * load (and after the personal-model / ToM-scenario merges). Pass NULL to
 * skip (backward-compatible). */
hu_error_t hu_w7_render_world_model(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len, int64_t now_ms,
                                    char **out_text, size_t *out_len, const char *tom_premise,
                                    size_t tom_premise_len, const char *tom_question,
                                    size_t tom_question_len, const char *tom_category,
                                    size_t tom_category_len, const hu_personal_model_t *pm,
                                    const hu_persona_context_t *persona_ctx);

/* W11 self-RAG outcome enum, mirrored at the bridge layer so callers don't
 * have to include `human/agent/self_rag.h` (which pulls in W7). */
typedef enum hu_w11_outcome {
    HU_W11_OUTCOME_SUPPORTED = 0,
    HU_W11_OUTCOME_HEDGED = 1,
    HU_W11_OUTCOME_REWRITTEN = 2,
    HU_W11_OUTCOME_ABSTAINED = 3,
} hu_w11_outcome_t;

/* W11 self-RAG verification (FIX 12b). Atomic-claim decomposition + scoring
 * of `draft` against the W7 facade and the W9 world model for `contact_id`.
 *
 * `mode` mirrors the response_verifier modes:
 *   0 = OFF (returns SUPPORTED, no work)
 *   1 = TELEMETRY (extract claims, do NOT modify draft)
 *   2 = SOFT (prepend hedges to flagged claims)
 *   3 = STRICT (rewrite via corrective-RAG)
 *
 * Returns HU_OK on success. Outputs:
 *   `*out_outcome` -- which branch fired
 *   `*out_claims_total` -- claims extracted (for telemetry parity with W4)
 *   `*out_claims_flagged` -- claims that scored below the abstain threshold
 *   `*out_modified` -- when non-NULL and the backend rewrote the draft, the
 *       new buffer is allocated via `alloc` and ownership transfers to caller.
 *       Pass NULL to skip modification entirely.
 *   `*out_modified_len` -- length of the new buffer when allocated.
 *
 * `now_ms == 0` means "use OS clock". */
hu_error_t hu_w11_self_rag_verify(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                  const char *contact_id, size_t contact_id_len, const char *draft,
                                  size_t draft_len, int mode, int64_t now_ms,
                                  hu_w11_outcome_t *out_outcome, size_t *out_claims_total,
                                  size_t *out_claims_flagged, char **out_modified,
                                  size_t *out_modified_len);

/* W11 self-RAG verification with provider-backed claim checking.
 *
 * Same as hu_w11_self_rag_verify but passes `provider` through to the
 * atomic backend so it can make real LLM calls to verify individual
 * claims against retrieved evidence. The provider is only used in STRICT
 * mode and only in non-test builds. When `provider` is NULL this
 * behaves identically to the providerless variant. */
hu_error_t hu_w11_self_rag_verify_with_provider(
    hu_w7_facade_t *facade, hu_allocator_t *alloc, struct hu_provider *provider,
    const char *contact_id, size_t contact_id_len, const char *draft, size_t draft_len, int mode,
    int64_t now_ms, hu_w11_outcome_t *out_outcome, size_t *out_claims_total,
    size_t *out_claims_flagged, char **out_modified, size_t *out_modified_len);

/* ── W14 sleep-time compute scheduler bridge (FIX 13) ─────────────────────
 *
 * The W14 scheduler pulls `human/memory/memory.h`; daemon code uses this
 * bridge so it can tick the scheduler without including scheduler internals
 * in every TU. */

struct hu_w14_scheduler;
typedef struct hu_w14_scheduler hu_w14_scheduler_t;

/* Open a scheduler over the same `hu_memory_facade_t` the W7 facade owns. The
 * scheduler does not take ownership of the facade — both must outlive
 * the lifetime of the daemon main loop. Returns HU_OK and `*out_sched`
 * is non-NULL on success; on failure `*out_sched` is NULL. */
hu_error_t hu_w14_scheduler_open(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                 hu_w14_scheduler_t **out_sched);

/* Close the scheduler. Safe with NULL. Must be called BEFORE
 * hu_w7_facade_close because the scheduler borrows the facade's memory
 * handle. */
void hu_w14_scheduler_close(hu_w14_scheduler_t *s, hu_allocator_t *alloc);

/* Run one tick of the scheduler. `now_ms` is unix-ms; pass 0 to use the
 * OS clock. The daemon main loop should call this once per minute (or
 * more often — the scheduler's own per-tick budget bounds the work). */
hu_error_t hu_w14_scheduler_tick(hu_w14_scheduler_t *s, int64_t now_ms);

/* Enqueue a counterfactual-rehearsal job for `contact_id`. The runner
 * is registered automatically at open(); this is just an enqueue
 * helper that hides the hu_job_spec_t shape from the daemon. */
hu_error_t hu_w14_scheduler_enqueue_counterfactual(hu_w14_scheduler_t *s, const char *contact_id,
                                                   size_t contact_id_len, int budget_ms);

/* Enqueue the three AutoDream phases (quarantine, community, decay) as
 * separate scheduler jobs. Mirrors what the legacy 3 AM cron in
 * daemon.c does, but routes through the W14 queue so it can be paced,
 * preempted by higher-priority jobs, and deferred when not on AC.
 *
 * `now_ms == 0` means "ASAP"; pass a unix-ms value to schedule for a
 * specific wall time (e.g. the next 3 AM boundary). */
hu_error_t hu_w14_scheduler_enqueue_autodream(hu_w14_scheduler_t *s, int64_t now_ms, int budget_ms);

/* Status snapshot for `human ml status` and friends. Always populates
 * the out fields even on partial probe failure. NULL output pointers
 * are tolerated (any subset may be queried). */
hu_error_t hu_w14_scheduler_status(hu_w14_scheduler_t *s, size_t *out_jobs_pending,
                                   size_t *out_jobs_completed_today, int *out_battery_pct,
                                   int *out_on_ac_power);

/* ── W12 goal-conditioned planner recall bridge ───────────────────────────
 *
 * Bridge for `hu_planner_plan` + manual execution with payload-preserving
 * facade reads. Callers in TUs that include legacy `human/memory.h` (e.g.
 * `agent_turn.c`, `memory_loader.c`) cannot use the planner or facade
 * types directly due to the `struct hu_memory` tag collision. This bridge
 * runs the plan in world_model_bridge.c (where only W7 headers are
 * visible) and returns pre-formatted text ready for prompt injection.
 *
 * `contact_id` scopes the retrieval to one contact (may be "" for global).
 * `query` is the user message or search goal. `limit` caps the number of
 * records surfaced (0 = default 5). `max_chars` caps output text length
 * (0 = default 4000).
 *
 * Returns HU_OK with `*out_text = NULL, *out_len = 0` when the planner
 * finds nothing. Returns a non-OK error on planner/facade failure so the
 * caller can fall back to the v1 recall path. Caller owns `*out_text`
 * and must free via `alloc->free`. */
hu_error_t hu_w12_planner_recall(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                 const char *contact_id, size_t contact_id_len, const char *query,
                                 size_t query_len, size_t limit, size_t max_chars, char **out_text,
                                 size_t *out_len);

/* W12 — Planner recall with an explicit LLM provider. Uses the LLM
 * retrieval planner backend (`hu_planner_llm`) for plan emission when
 * `provider` is non-NULL and HU_IS_TEST is unset; falls back through the
 * goal-conditioned and heuristic backends otherwise.
 *
 * Forward-declares `hu_provider_t` via `human/provider.h`; this is the
 * only place in this header that needs the provider type, so callers
 * that don't use the LLM path pay no include cost.
 *
 * Returns HU_OK on success (including the "no records" case where
 * `*out_text == NULL`, `*out_len == 0`). */
hu_error_t hu_w12_planner_recall_with_provider(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                               struct hu_provider *provider, const char *model,
                                               size_t model_len, const char *contact_id,
                                               size_t contact_id_len, const char *query,
                                               size_t query_len, size_t limit, size_t max_chars,
                                               char **out_text, size_t *out_len);

/* Enqueue the persona evolver as a scheduler job. Mirrors the
 * AutoDream bridge pattern: when the scheduler is available the
 * daemon enqueues instead of running synchronously, so the work
 * is paced and battery-gated. `budget_ms` of 0 means unlimited. */
hu_error_t hu_w14_scheduler_enqueue_persona_evolver(hu_w14_scheduler_t *s, int64_t now_ms,
                                                    int budget_ms);

/* Enqueue a LoRA training job. Mirrors the persona-evolver / AutoDream
 * enqueue helpers: callers pass timing + budget and the bridge hides the
 * hu_job_spec_t shape. */
hu_error_t hu_w14_scheduler_enqueue_lora(hu_w14_scheduler_t *s, int64_t now_ms, int budget_ms);

/* Enqueue a training data extraction job. Extracts new conversations
 * into JSONL training data and generates auto-DPO pairs. The runner
 * auto-enqueues a LoRA training job when enough examples accumulate. */
hu_error_t hu_w14_scheduler_enqueue_training_data_extract(hu_w14_scheduler_t *s, int64_t now_ms,
                                                          int budget_ms);

/* W14 P0 #4 — runner registration helpers.
 *
 * The bridge cannot embed `hu_lora_runner_ctx_t` / `hu_kv_cache_manager_t` /
 * `hu_belief_reverify_ctx_t` directly without re-introducing the legacy
 * memory.h collision. Callers include the relevant runner headers
 * (lora_runner.h / kv_cache.h / belief_reverify_runner.h) and pass
 * already-constructed contexts in. NULL ctx is rejected. */
struct hu_lora_runner_ctx;
struct hu_kv_cache_manager;
struct hu_belief_reverify_ctx;
struct hu_training_data_runner_ctx;
struct hu_lora_retrain_ctx;
struct hu_scheduler;

hu_error_t hu_w14_scheduler_register_lora_runner(hu_w14_scheduler_t *s,
                                                 struct hu_lora_runner_ctx *ctx);
hu_error_t hu_w14_scheduler_register_kv_prewarm_runner(hu_w14_scheduler_t *s,
                                                       struct hu_kv_cache_manager *mgr);
hu_error_t hu_w14_scheduler_register_belief_reverify(hu_w14_scheduler_t *s,
                                                     struct hu_belief_reverify_ctx *ctx);
hu_error_t hu_w14_scheduler_register_training_data_runner(hu_w14_scheduler_t *s,
                                                          struct hu_training_data_runner_ctx *ctx);

/* US-7.5: register the nightly LoRA retrain runner. The bridge stashes the
 * ctx pointer so `hu_w14_scheduler_status_save` can serialize the
 * `lora_retrain` block (last_run_ts, last_outcome, pairs_consumed). The
 * caller owns the storage. */
hu_error_t hu_w14_scheduler_register_lora_retrain_runner(hu_w14_scheduler_t *s,
                                                         struct hu_lora_retrain_ctx *ctx);

/* US-7.5: enqueue one nightly LoRA retrain job. Sets requires_idle=true,
 * requires_ac_power=true, interval_sec=86400, budget_ms=90min default. */
hu_error_t hu_w14_scheduler_enqueue_lora_retrain_nightly(hu_w14_scheduler_t *s, int64_t now_ms,
                                                         int budget_ms);

/* Surface the inner `hu_scheduler_t *` so callers can wire it into a
 * `hu_lora_runner_ctx_t::scheduler` (`struct hu_scheduler *`) for the follow-up KV-warm
 * enqueue path. The bridge retains ownership; callers must not
 * `hu_scheduler_close` the returned pointer. */
struct hu_scheduler *hu_w14_scheduler_inner(hu_w14_scheduler_t *s);

/* Persist the scheduler's current status to `~/.human/scheduler.status`
 * as a tiny hand-emitted JSON document so out-of-process tools (the
 * `human ml status` CLI, the doctor command, monitoring) can read it
 * without IPC into the running daemon.
 *
 * Best-effort: returns HU_OK whenever the file is written or the path
 * resolution would no-op (HOME unset). Mirrors the proven pattern used
 * by `~/.human/imessage.poll_status`. The daemon should call this on
 * the same cadence it ticks the scheduler. */
hu_error_t hu_w14_scheduler_status_save(hu_w14_scheduler_t *s);

/* Resolve the canonical status file path. Returns false on overflow or
 * when HOME is unset. */
bool hu_w14_scheduler_status_path(char *out_path, size_t cap);

/* W15 — audit log bridge.
 *
 * Opens a SQLite-backed audit log, registers the facade audit hook so every
 * successful write/erase is logged, and returns the log handle through
 * `*out`. The handle must be freed with `hu_w7_audit_log_close` AFTER
 * `hu_w7_facade_close` (the facade does not fire the hook after close, so
 * ordering is safe). Pass NULL for `contact_id` for unscoped logging.
 *
 * These wrappers keep `human/security/audit_log.h` out of TUs that include
 * `human/agent.h` (which pulls in the legacy `human/memory.h`). */
struct hu_audit_log;
hu_error_t hu_w7_audit_log_open(hu_w7_facade_t *facade, hu_allocator_t *alloc, const char *db_path,
                                const char *contact_id, struct hu_audit_log **out);
void hu_w7_audit_log_close(struct hu_audit_log *log, hu_allocator_t *alloc);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_WORLD_MODEL_BRIDGE_H */
