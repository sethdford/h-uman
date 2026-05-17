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
size_t hu_agent_internal_recent_assistant_avg_len(const hu_agent_t *agent, size_t max_n);

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
