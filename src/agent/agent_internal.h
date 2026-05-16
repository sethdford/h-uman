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

#include "human/core/json.h"

/**
 * Execute a tool with the hook pipeline wrapped around it.
 *
 * Canonical single-tool dispatcher for paths that previously called
 * `tool->vtable->execute(...)` directly without firing the hook pipeline
 * (DAG worker, orchestrator, approval-retry). Centralizing the dispatch
 * closes the security gap surfaced by the audit on 2026-05-16:
 *   "Hook pipeline is invoked in the dispatcher's parallel path but NOT
 *    in the sequential / polling execution path."
 *
 * Sequence:
 *   1. If agent->hook_registry is non-NULL, fire the pre-tool hook. If the
 *      decision is HU_HOOK_DENY, write a failure result into *out and
 *      return without calling the tool. The hook's message is copied into
 *      the result.
 *   2. Call tool->vtable->execute(tool->ctx, agent->alloc, args_parsed, out).
 *   3. If agent->hook_registry is non-NULL, fire the post-tool hook
 *      (informational — the result has already been produced).
 *
 * Returns HU_OK for normal completion (including hook-denied dispatch — the
 * caller inspects out->success to learn what happened).
 * Returns HU_ERR_INVALID_ARGUMENT for NULL agent / tool / out.
 *
 * The caller retains ownership of `args_parsed` and frees `*out` via
 * hu_tool_result_free when done.
 */
hu_error_t hu_agent_internal_dispatch_with_hooks(hu_agent_t *agent, hu_tool_t *tool,
                                                 const char *tool_name, size_t tool_name_len,
                                                 const char *args_json, size_t args_json_len,
                                                 const hu_json_value_t *args_parsed,
                                                 hu_tool_result_t *out);

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
