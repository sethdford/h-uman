#ifndef HU_AGENT_INIT_PROPOSER_H
#define HU_AGENT_INIT_PROPOSER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdint.h>

/* Initiative Layer — proposer subsystem (T1 skeleton).
 *
 * Per docs/plans/2026-05-25-initiative-layer/. Periodically asks
 * "given everything I know about Seth's life right now, should I
 * bring something up?" — even when no inbound event fired.
 *
 * T1 scope: skeleton tick + governor pre-checks ONLY. Always returns
 * SKIP. LLM call lands in T3.
 *
 * Design decisions (resolved 2026-05-25):
 *   - Cadence: 30 min during awake hours
 *   - Confidence gate: hard threshold 0.85
 *   - Model tier: gemini-3.5-flash (conversational)
 *   - Awake source: autoresponder.json schedules (NOT in any quiet window)
 */

/* Forward declarations to avoid pulling the entire daemon graph into the
 * public header. Definitions live in config.h / agent.h / governor.h /
 * provider.h. */
struct hu_initiative_config;
struct hu_autoresponder_config;
struct hu_proactive_budget;
struct hu_provider;

/* Single per-tick outcome the daemon logs at INFO level. */
typedef enum hu_init_proposer_result {
    HU_INIT_RESULT_SKIP = 0,           /* tick fired, no proposal warranted */
    HU_INIT_RESULT_GATED_QUIET = 1,    /* in autoresponder quiet hours */
    HU_INIT_RESULT_GATED_BUDGET = 2,   /* daily proactive budget exhausted */
    HU_INIT_RESULT_GATED_RECENCY = 3,  /* Seth texted h-uman recently (<per_contact_min_seconds) */
    HU_INIT_RESULT_GATED_INTERVAL = 4, /* not enough time since last tick */
    HU_INIT_RESULT_FIRED = 5,          /* T3+: actually fired a proposal */
    HU_INIT_RESULT_LLM_ERROR = 6,      /* T3: provider call failed; skipped silently */
    HU_INIT_RESULT_PARSE_ERROR = 7,    /* T3: LLM response wasn't valid JSON */
    HU_INIT_RESULT_LOW_CONFIDENCE = 8, /* T3: LLM proposed but confidence < threshold */
    HU_INIT_RESULT_NEGATIVE = 9,       /* T3: LLM returned should_propose=false */
    /* M3 Dispatch T2 (2026-05-26) — the LLM returned a high-confidence
     * draft but response_guard_check_ex rejected it (G1–G9 detector
     * fired). The draft has been captured as a DPO negative pair for
     * future LoRA training. Daemon caller skips the send. */
    HU_INIT_RESULT_GUARD_REJECT = 10,
} hu_init_proposer_result_t;

/* Sprint 41 follow-up #2 — single-source-of-truth proactive arbiter.
 *
 * Read-only governor check that other proactive subsystems (daemon_proactive,
 * follow-up watcher, scheduled cron) can call BEFORE generating + sending
 * any proactive outbound. Wraps the same gate stack `hu_init_proposer_tick`
 * uses (quiet hours + daily budget + per-contact recency) but does NOT
 * touch tick state — no watermark bump, no tick-id increment, no logging.
 *
 * The interval gate is INTENTIONALLY EXCLUDED — it's specific to
 * init_proposer's polling cadence and would over-gate callers with their
 * own scheduling discipline. Callers that want interval-style throttling
 * should use `hu_proactive_throttle` instead.
 *
 * Returns the same result enum as `hu_init_proposer_tick` so log lines
 * stay schema-compatible across subsystems:
 *   HU_INIT_RESULT_SKIP          → gates passed, caller MAY proceed
 *   HU_INIT_RESULT_GATED_QUIET   → autoresponder DND window
 *   HU_INIT_RESULT_GATED_BUDGET  → daily proactive budget exhausted
 *   HU_INIT_RESULT_GATED_RECENCY → user texted h-uman within recency_floor
 *
 * NULL ar_cfg / budget mean "operator opted out of that gate" — mirrors
 * what `hu_init_proposer_tick` and the per-gate predicates already do. */
hu_init_proposer_result_t
hu_init_proposer_governor_check_only(const struct hu_initiative_config *cfg,
                                     const struct hu_autoresponder_config *ar_cfg,
                                     int32_t tz_offset_seconds, struct hu_proactive_budget *budget,
                                     int64_t last_inbound_unix, int64_t now_unix);

/* Tick entry point.
 *
 * Called once per daemon outer loop. Internally rate-limits to
 * cfg->tick_interval_sec (default 1800). When gated, emits a single log
 * line naming the dominant reason; when SKIP, emits the same shape so
 * operators can see the system is alive.
 *
 * Parameters:
 *   cfg                   — initiative config; if cfg->enabled is false,
 *                           emits one-shot disabled-warning and returns OK.
 *   ar_cfg                — autoresponder config for quiet-hour gating.
 *                           If NULL, treated as "no quiet hours configured"
 *                           and the quiet check is skipped (operator
 *                           silently disabling not allowed — see AC-6).
 *   tz_offset_seconds     — local TZ offset for autoresponder window math.
 *   budget                — optional proactive budget for daily-cap check.
 *                           If NULL, the budget check is skipped (treated as
 *                           always-available; not recommended in prod).
 *   last_inbound_unix     — wall-clock seconds of the most recent inbound
 *                           message FROM the proposed recipient. 0 means
 *                           never (no recency gate).
 *   now_unix              — current wall-clock seconds.
 *   last_tick_unix_inout  — caller-owned watermark of the previous tick.
 *                           Updated to now_unix on a non-gated tick. The
 *                           interval check uses this.
 *   tick_id_inout         — monotonic tick counter; incremented per
 *                           non-gated tick. Logged.
 *   out_result            — written with the per-tick outcome (one of
 *                           HU_INIT_RESULT_*); never NULL.
 *
 * Returns HU_OK on a normal tick (including gated ticks).
 * Returns HU_ERR_INVALID_ARGUMENT on NULL required args. */
hu_error_t hu_init_proposer_tick(const struct hu_initiative_config *cfg,
                                 const struct hu_autoresponder_config *ar_cfg,
                                 int32_t tz_offset_seconds, struct hu_proactive_budget *budget,
                                 int64_t last_inbound_unix, int64_t now_unix,
                                 int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
                                 hu_init_proposer_result_t *out_result);

/* Test-only: reset the one-shot warn guards (enabled/disabled log lines)
 * so each test starts with a clean slate. No-op outside HU_IS_TEST. */
void hu_init_proposer_reset_warn_guards_for_test(void);

/* ──────────────────────────────────────────────────────────────────────────
 * T2 — Context bundle assembly (AC-2 partial)
 *
 * The proposer needs the SAME rich context the agent_turn prompt builder
 * uses, plus initiative-specific signals (recent messages, F30/F31/F129
 * affordances — when those are wired by the proactive-ext-completion plan).
 *
 * T2 ships a thin observation struct + an assembly helper. T3 will pass
 * this to the analytical-tier LLM as the "propose-or-skip" prompt input. */

/* Per-source byte counts. Indexed by HU_INIT_FIELD_*. */
typedef enum hu_init_field {
    HU_INIT_FIELD_PERSONA = 0,
    HU_INIT_FIELD_CONTACT,
    HU_INIT_FIELD_CONVERSATION,
    HU_INIT_FIELD_MEMORY,
    HU_INIT_FIELD_PERSONAL_MODEL,
    HU_INIT_FIELD_AWARENESS,
    HU_INIT_FIELD_INSTRUCTION,
    HU_INIT_FIELD_STM,
    /* T8 of docs/plans/2026-05-26-reflection-loop. Unsurfaced reflection
     * patterns (confidence >= 0.6, retired=0, surfaced_to_user=0)
     * formatted as bullet lines. Populated by `assemble_context` from
     * the agent's SQLite memory backend when reflection is enabled. */
    HU_INIT_FIELD_REFLECTION,
    HU_INIT_FIELD_COUNT, /* sentinel */
} hu_init_field_t;

/* Lightweight bundle: pointers + byte counts into agent-owned strings.
 * Most fields BORROW pointers — caller must not free them. Lifetime is
 * tied to the calling agent_turn (don't store between ticks).
 *
 * Exception (T8): HU_INIT_FIELD_REFLECTION's content[] pointer aliases
 * the inline `reflection_buf` below — owned by the bundle itself, so
 * stack-allocated bundles get a self-contained reflection slice without
 * a separate allocation. Don't free content[REFLECTION] either. */
#define HU_INIT_REFLECTION_BUF_MAX 1024
#define HU_INIT_PERSONA_BUF_MAX    256
typedef struct hu_init_context_bundle {
    /* Per-source content pointers (any may be NULL if the field is empty). */
    const char *content[HU_INIT_FIELD_COUNT];
    size_t bytes[HU_INIT_FIELD_COUNT];
    size_t total_bytes;
    /* Per-tick metadata. */
    int64_t now_unix;
    int64_t last_inbound_unix; /* 0 if never */
    /* T8: inline storage for the reflection slice. Sized for ~10 short
     * observations worth of bullets — beyond that the proposer drops
     * the overflow. */
    char reflection_buf[HU_INIT_REFLECTION_BUF_MAX];
    /* Inline storage for a compact persona summary (name + identity). The
     * proposer previously left the PERSONA field stubbed-to-zero, so the
     * silence-biased prompt saw "thin context" and returned should_propose=
     * false every tick. A one-line persona descriptor gives the model the
     * "who am I / who am I texting" grounding it needs to decide. Aliased by
     * content[HU_INIT_FIELD_PERSONA]; owned by the bundle — do not free. */
    char persona_buf[HU_INIT_PERSONA_BUF_MAX];
} hu_init_context_bundle_t;

/* Forward-declared so we don't pull include/human/agent.h into this header
 * (avoids transitive dep cycles). Defined in include/human/agent.h. */
struct hu_agent;

/* Assemble the proposer's context bundle from the agent's current cached
 * context strings. Cheap — no allocation, no LLM call. Caller-owned out;
 * function memsets to zero before populating. */
hu_error_t hu_init_proposer_assemble_context(const struct hu_agent *agent, int64_t now_unix,
                                             int64_t last_inbound_unix,
                                             hu_init_context_bundle_t *out);

/* Format a one-line operator-visible summary of the bundle into a caller-
 * owned buffer:
 *
 *   "fields=N total=X persona=A contact=B conversation=C memory=D ..."
 *
 * Pure predicate over the bundle — no I/O, no allocation. Returns the
 * number of bytes written (excluding NUL). On out_cap=0, returns 0. */
size_t hu_init_proposer_format_context_summary(const hu_init_context_bundle_t *bundle, char *out,
                                               size_t out_cap);

/* ──────────────────────────────────────────────────────────────────────────
 * T3 — LLM call + decision gate (AC-3 partial)
 *
 * Three pure predicates + one integration tick that wires them together
 * with a provider call. The predicates exist so the decision logic can be
 * tested without spinning a real HTTP path.
 *
 * Flow inside hu_init_proposer_tick_with_provider:
 *   1. Build the system + user prompt from the bundle (pure)
 *   2. Call provider->chat_with_system(...) → response text
 *   3. Parse the response as JSON (pure) → hu_init_decision_t
 *   4. Evaluate the decision against cfg->confidence_threshold (pure)
 *   5. Log + return result enum
 */

#define HU_INIT_DRAFT_MAX       512
#define HU_INIT_SKIP_REASON_MAX 256

/* Structured decision parsed from the LLM's JSON response.
 *
 *   {
 *     "should_propose": bool,    // true = propose, false = skip this tick
 *     "confidence": 0.0..1.0,    // model's self-reported confidence
 *     "draft": "the message...", // candidate text if should_propose=true
 *     "reason": "why skipping"   // present when should_propose=false
 *   }
 *
 * The draft/reason strings are copied into fixed-size buffers so callers
 * don't need to track ownership. */
typedef struct hu_init_decision {
    bool should_propose;
    double confidence;
    char draft[HU_INIT_DRAFT_MAX];
    size_t draft_len;
    char skip_reason[HU_INIT_SKIP_REASON_MAX];
    size_t skip_reason_len;
} hu_init_decision_t;

/* Build the "propose-or-skip" prompt pair for the LLM. Returns the bytes
 * written to each output buffer (excluding NUL). The system prompt sets up
 * the role + JSON-only output contract; the user message embeds the bundle
 * fields verbatim.
 *
 * If a buffer would overflow, the function truncates (NUL-terminates) and
 * still returns. Caller is responsible for sizing — recommended:
 *   system_prompt: 1024 bytes
 *   user_message: 16384 bytes (matches the daemon's typical context size)
 *
 * Pure predicate — no I/O, no allocation. */
size_t hu_init_proposer_build_propose_prompt(const hu_init_context_bundle_t *bundle,
                                             char *out_system_prompt, size_t system_prompt_cap,
                                             char *out_user_message, size_t user_message_cap);

/* Parse the LLM's response (raw JSON or text-with-JSON-substring) into a
 * structured decision. Returns HU_OK and populates *out on success;
 * HU_ERR_JSON_PARSE if no valid JSON object can be extracted; or
 * HU_ERR_INVALID_ARGUMENT on NULL args.
 *
 * Defensive parsing: the function locates the FIRST top-level '{...}' run
 * and parses that, so the LLM can prefix or suffix prose without breaking
 * the decision. Required fields default to safe values (should_propose=false,
 * confidence=0.0) if absent, so a missing field always means SKIP.
 *
 * Pure predicate — no I/O, no allocation outside the out struct. */
hu_error_t hu_init_proposer_parse_response(const char *response, size_t response_len,
                                           hu_init_decision_t *out);

/* Evaluate a parsed decision against the confidence threshold. Returns the
 * tick result enum (FIRED / LOW_CONFIDENCE / NEGATIVE). Pure. */
hu_init_proposer_result_t hu_init_proposer_evaluate_decision(const hu_init_decision_t *decision,
                                                             double confidence_threshold);

/* Tick variant that wires the provider call. Same semantics as
 * hu_init_proposer_tick when provider is NULL — the LLM step is skipped
 * and the tick returns SKIP after the governor checks (preserves T1/T2
 * behavior for tests + the disabled-default-launch state).
 *
 * When provider is non-NULL AND all governor gates pass:
 *   - assembles context bundle (T2)
 *   - builds prompt (T3 pure)
 *   - calls provider->vtable->chat_with_system with cfg->propose_model
 *   - parses JSON (T3 pure)
 *   - evaluates against cfg->confidence_threshold
 *   - returns FIRED / LOW_CONFIDENCE / NEGATIVE / LLM_ERROR / PARSE_ERROR
 *   - on FIRED, populates *out_decision so the caller can send the draft
 *
 * out_decision: caller-owned; populated only when result == FIRED. Pass
 *               NULL if you don't need the draft (e.g. dry-run mode). */
hu_error_t hu_init_proposer_tick_with_provider(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc, int64_t last_inbound_unix,
    int64_t now_unix, int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
    hu_init_proposer_result_t *out_result, hu_init_decision_t *out_decision);

/* ──────────────────────────────────────────────────────────────────────────
 * M3 Dispatch Unification — T1 (2026-05-26)
 *
 * Pure-addition extension that lets daemon_proactive's scheduler pass
 * the rich per-contact context (memory recall, weather, calendar, feeds,
 * channel + recipient identity) THROUGH init_proposer so the same
 * propose-or-skip machinery composes both initiative-driven AND
 * daemon-proactive-driven sends.
 *
 * Spec: docs/plans/2026-05-26-m3-dispatch-unification/{requirements,
 * design,tasks}.md.
 *
 * Backwards-compatibility note: existing callers continue to use
 * `hu_init_proposer_tick_with_provider`. They are unchanged. T1 only
 * adds the _ex extension; T2-T8 wire callers over to it. */

/* Per-tick compose inputs. Lifetimes are tied to the caller's tick frame;
 * init_proposer copies what it needs into the prompt and does NOT retain
 * pointers past return. NULL pointer + 0 len for any source means "this
 * source is unpopulated for this contact this tick" (skipped cleanly). */
typedef struct hu_proactive_compose_inputs {
    /* Identity of the proactive target. */
    const char *contact_id;
    size_t contact_id_len;
    const char *channel_name;
    size_t channel_name_len;

    /* Pre-built context fragments. The caller (daemon_proactive) is
     * responsible for running output-safety filters on memory_context
     * BEFORE populating; init_proposer trusts the bytes as-is. */
    const char *memory_context;
    size_t memory_context_len;
    const char *weather_context;
    size_t weather_context_len;
    const char *calendar_context;
    size_t calendar_context_len;
    const char *feeds_context;
    size_t feeds_context_len;
    /* Due follow-ups: concrete triggers from stored commitments with
     * due times. Caller (daemon) populates this by querying
     * hu_superhuman_delayed_followup_list_due and formatting as
     * "- <topic> (due <timestamp>)\n". init_proposer includes it
     * as a labeled section so the proposer sees concrete reasons to
     * reach out (F25 concrete triggers). Empty = no due followups. */
    const char *due_followups_context;
    size_t due_followups_context_len;

    /* Optional defensive callback: if non-NULL, init_proposer calls it
     * on memory_context before inclusion and treats a `false` return as
     * "skip the memory_context source for this tick". Lets us migrate
     * `hu_daemon_callback_content_is_safe` into the unified pipeline
     * without coupling init_proposer to that specific predicate.
     * Receives the same (ptr, len) the caller passed. */
    bool (*content_is_safe)(const char *content, size_t content_len);
} hu_proactive_compose_inputs_t;

/* T1 extension to hu_init_proposer_tick_with_provider.
 *
 * Identical semantics to the original except:
 *   - When `inputs` is non-NULL, the propose-or-skip prompt is built
 *     from the inputs' rich context fragments instead of from the
 *     agent's cached context strings. This is the path daemon_proactive
 *     will use once T4 wires it up.
 *   - When `inputs` is NULL, behaves byte-identically to
 *     `hu_init_proposer_tick_with_provider` — required for AC-6
 *     backwards compatibility.
 *
 * The function does NOT send. On FIRED it populates *out_decision and
 * returns; the caller (daemon_proactive scheduler) is responsible for
 * the channel-vtable send + throttle/budget records. This preserves
 * the test seam (init_proposer is unit-testable without a channel
 * mock) AND lets the daemon apply its existing send-cap machinery
 * uniformly. */
hu_error_t hu_init_proposer_tick_with_provider_ex(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc,
    const hu_proactive_compose_inputs_t *inputs, int64_t last_inbound_unix, int64_t now_unix,
    int64_t *last_tick_unix_inout, uint64_t *tick_id_inout, hu_init_proposer_result_t *out_result,
    hu_init_decision_t *out_decision);

/* Pure helper exposed for testing: render the propose-or-skip USER
 * message from a compose_inputs struct (no agent dependency, no I/O).
 * Returns bytes written (excluding NUL); 0 on overflow. The system
 * prompt is unchanged from the existing
 * hu_init_proposer_build_propose_prompt path — only the USER message
 * gets the rich-context shape. */
size_t hu_init_proposer_build_propose_user_message_ex(const hu_proactive_compose_inputs_t *inputs,
                                                      int64_t now_unix, int64_t last_inbound_unix,
                                                      char *out, size_t out_cap);

/* M3 Dispatch T2 — pure verdict-mapping helper. Maps the outcome of
 * hu_response_guard_check_ex (run on a FIRED decision's draft) to the
 * appropriate tick result:
 *
 *   HU_GUARD_OK       → keep FIRED (caller sends decision.draft as-is)
 *   HU_GUARD_REWROTE  → keep FIRED (caller MUST swap draft for the rewrite)
 *   HU_GUARD_REJECT   → downgrade to HU_INIT_RESULT_GUARD_REJECT
 *
 * Pure — no I/O. Exposed so the post-FIRE verdict logic is unit-testable
 * without spinning a provider. Takes `int` rather than the enum directly
 * so this header doesn't require a transitive include of response_guard.h;
 * callers pass `(int)guard_outcome`. */
hu_init_proposer_result_t hu_init_proposer_evaluate_guard_outcome(int guard_outcome);

#endif /* HU_AGENT_INIT_PROPOSER_H */
