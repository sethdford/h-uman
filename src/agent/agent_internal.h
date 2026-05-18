/* Internal agent module API. Not installed; used only by agent/ sources. */
#ifndef HU_AGENT_INTERNAL_H
#define HU_AGENT_INTERNAL_H

#include "human/agent.h"
#include "human/observer.h"
#include "human/provider.h"
#include "human/security.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define HU_OBS_SAFE_RECORD_EVENT(agent, ev)                                   \
    do {                                                                      \
        if ((agent)->observer) {                                              \
            (ev)->trace_id = (agent)->trace_id[0] ? (agent)->trace_id : NULL; \
            hu_observer_record_event(*(agent)->observer, (ev));               \
        }                                                                     \
    } while (0)

void hu_agent_internal_generate_trace_id(char *buf);
uint64_t hu_agent_internal_clock_diff_ms(clock_t start, clock_t end);
void hu_agent_internal_record_cost(hu_agent_t *agent, const hu_token_usage_t *usage);

/* Monotonic wall-clock in milliseconds, used to thread latency through
 * to hu_agent_m3_record_chat_outcome. Defined inline here so both
 * agent_stream.c and agent_turn.c can call it without duplicating the
 * implementation. Falls back to CLOCK_REALTIME when CLOCK_MONOTONIC is
 * unavailable; the caller only uses the difference, so monotonicity is
 * the property we care about. */
static inline uint64_t hu_agent_internal_monotonic_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
#else
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
#endif
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* Average content_len over the most-recent up-to-`max_n` assistant
 * turns in `agent->history` (skips system / user / tool entries).
 * Returns 0 when there are no qualifying turns; the response guard
 * treats 0 as "no signal, do not enforce length-anomaly (G5)".
 *
 * Used by the response_guard call sites to populate
 * `hu_guard_context_t.recent_avg_len` so a 1.8 KB CoT-dump in a
 * 150-byte channel is REJECTed at runtime, not just in unit tests.
 * (Sprint 33 — wires Sprint 31's G5 into production.)
 */
/* Sprint 41 — EWMA smoothing for G5 `recent_avg_len` (post-mortem #20).
 * Higher alpha weights the newest assistant turns more heavily. */
#define HU_GUARD_ASSISTANT_LEN_EWMA_ALPHA 0.35

size_t hu_agent_internal_recent_assistant_avg_len(const hu_agent_t *agent, size_t max_n);

/* Set / clear the active scene-direction text for the next turn. The
 * daemon owns the buffer (typically `hu_director_result_t.direction[512]`);
 * the agent only borrows a const pointer + length. Setter is a plain
 * field assignment — no allocation, no copy. Clear must be called when
 * the daemon's director_result goes out of scope, otherwise the guard
 * could read freed memory on the next turn.
 *
 * Used by the response_guard call sites to populate
 * `hu_guard_context_t.director_text` so a verbatim quote of "casual
 * short, dry" by the model triggers G6 → REJECT. (Sprint 34 — wires
 * Sprint 31's G6 into production.) */
void hu_agent_internal_set_scene_direction(hu_agent_t *agent, const char *text, size_t text_len);
void hu_agent_internal_clear_scene_direction(hu_agent_t *agent);

/* Sprint 37 — Push the about-to-go-stale director string into the
 * agent's ring buffer of recent directors. Slot 0 is most recent;
 * older entries shift down; the oldest is freed when the buffer fills.
 *
 * No-op if `text` is NULL or shorter than 30 bytes
 * (HU_GUARD_DIRECTOR_ECHO_MIN_MATCH — wouldn't trip G6 anyway).
 *
 * Allocates a NUL-terminated copy on `agent->alloc`, truncated to
 * HU_DIRECTOR_TEXT_CAP bytes. The agent owns the copy; freed by
 * `hu_agent_internal_free_director_history` (called by
 * `hu_agent_deinit`).
 *
 * Used by the daemon at end-of-turn, just before clearing
 * `agent->scene_direction_text`, so that G6 on the next turn can
 * still catch a model that quotes the *previous* turn's director. */
void hu_agent_internal_push_director_history(hu_agent_t *agent, const char *text, size_t text_len);

/* Free all director history slots; reset count to 0. Called by
 * `hu_agent_deinit`. Idempotent. */
void hu_agent_internal_free_director_history(hu_agent_t *agent);

/* Sprint 40 — Clear cross-turn state when the daemon switches to a
 * different contact/session key. Prevents director-history and scene
 * pointers from one recipient influencing G6 on another (post-mortem
 * rowid 56355 cross-contact fanout). Same-contact multi-turn history
 * is preserved until the key changes. */
void hu_agent_internal_reset_contact_boundary_state(hu_agent_t *agent);

hu_error_t hu_agent_internal_ensure_history_cap(hu_agent_t *agent, size_t need);
hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len);
hu_error_t hu_agent_internal_append_history_with_tool_calls(hu_agent_t *agent, const char *content,
                                                            size_t content_len,
                                                            const hu_tool_call_t *tool_calls,
                                                            size_t tool_calls_count);

void hu_agent_set_current_for_tools(hu_agent_t *agent);
void hu_agent_clear_current_for_tools(void);
void hu_agent_internal_process_mailbox_messages(hu_agent_t *agent);
void hu_agent_internal_maybe_tts(hu_agent_t *agent, const char *text, size_t text_len);

hu_policy_action_t hu_agent_internal_check_policy(hu_agent_t *agent, const char *tool_name,
                                                  const char *arguments);
hu_policy_action_t hu_agent_internal_evaluate_tool_policy(hu_agent_t *agent, const char *tool_name,
                                                          const char *args_json);
hu_tool_t *hu_agent_internal_find_tool(hu_agent_t *agent, const char *name, size_t name_len);

/* Canonical tool dispatch: pre/post hook pipeline + execute.
 * Returns HU_OK for normal completion (including hook-denied dispatch).
 * Caller frees *out via hu_tool_result_free. */
hu_error_t hu_agent_internal_dispatch_with_hooks(hu_agent_t *agent, hu_tool_t *tool,
                                                 const char *tool_name, size_t tool_name_len,
                                                 const char *args_json, size_t args_json_len,
                                                 const hu_json_value_t *args_parsed,
                                                 hu_tool_result_t *out);

/* Split pre/post hook helpers — for call sites that cannot use
 * hu_agent_internal_dispatch_with_hooks because they own execution
 * differently (streaming tools, parallel dispatcher, approval-retry).
 *
 * They MUST be called as a pair: pre returns true if the caller should
 * proceed to execute the tool; false means the pre-hook denied and *out
 * is already populated with the deny result. In either case the caller
 * is REQUIRED to invoke hu_agent_internal_post_hook_fire afterward so
 * auditors observe every dispatch attempt. */
bool hu_agent_internal_pre_hook_check(hu_agent_t *agent, const char *tool_name,
                                      size_t tool_name_len, const char *args_json,
                                      size_t args_json_len, hu_tool_result_t *out);

void hu_agent_internal_post_hook_fire(hu_agent_t *agent, const char *tool_name,
                                      size_t tool_name_len, const char *args_json,
                                      size_t args_json_len, const hu_tool_result_t *result);

/* Shared humanness thresholds used by both batch and streaming paths */
#ifndef HU_SYCOPHANCY_THRESHOLD
#define HU_SYCOPHANCY_THRESHOLD 0.5f
#endif
#ifndef HU_FACT_CONFIDENCE_MIN
#define HU_FACT_CONFIDENCE_MIN 0.6f
#endif
#ifndef HU_CONSISTENCY_DRIFT_THRESHOLD
#define HU_CONSISTENCY_DRIFT_THRESHOLD 0.3f
#endif
#ifndef HU_HUMOR_RISK_TOLERANCE
#define HU_HUMOR_RISK_TOLERANCE 0.4f
#endif
#ifndef HU_SOMATIC_TIRED_THRESHOLD
#define HU_SOMATIC_TIRED_THRESHOLD 0.3f
#endif
#ifndef HU_SOMATIC_LOW_THRESHOLD
#define HU_SOMATIC_LOW_THRESHOLD 0.5f
#endif
#ifndef HU_OPINION_FRICTION_COUNT
#define HU_OPINION_FRICTION_COUNT 2
#endif

#endif /* HU_AGENT_INTERNAL_H */
