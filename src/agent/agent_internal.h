/* Internal agent module API. Not installed; used only by agent/ sources. */
#ifndef SC_AGENT_INTERNAL_H
#define SC_AGENT_INTERNAL_H

#include "seaclaw/agent.h"
#include "seaclaw/observer.h"
#include "seaclaw/provider.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SC_OBS_SAFE_RECORD_EVENT(agent, ev)                                   \
    do {                                                                      \
        if ((agent)->observer) {                                              \
            (ev)->trace_id = (agent)->trace_id[0] ? (agent)->trace_id : NULL; \
            sc_observer_record_event(*(agent)->observer, (ev));               \
        }                                                                     \
    } while (0)

void sc_agent_internal_generate_trace_id(char *buf);
uint64_t sc_agent_internal_clock_diff_ms(clock_t start, clock_t end);
void sc_agent_internal_record_cost(sc_agent_t *agent, const sc_token_usage_t *usage);

sc_error_t sc_agent_internal_ensure_history_cap(sc_agent_t *agent, size_t need);
sc_error_t sc_agent_internal_append_history(sc_agent_t *agent, sc_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len);
sc_error_t sc_agent_internal_append_history_with_tool_calls(sc_agent_t *agent, const char *content,
                                                            size_t content_len,
                                                            const sc_tool_call_t *tool_calls,
                                                            size_t tool_calls_count);

void sc_agent_set_current_for_tools(sc_agent_t *agent);
void sc_agent_clear_current_for_tools(void);
void sc_agent_internal_process_mailbox_messages(sc_agent_t *agent);
void sc_agent_internal_maybe_tts(sc_agent_t *agent, const char *text, size_t text_len);

sc_policy_action_t sc_agent_internal_check_policy(sc_agent_t *agent, const char *tool_name,
                                                  const char *arguments);
sc_policy_action_t sc_agent_internal_evaluate_tool_policy(sc_agent_t *agent, const char *tool_name,
                                                          const char *args_json);
sc_tool_t *sc_agent_internal_find_tool(sc_agent_t *agent, const char *name, size_t name_len);

#endif /* SC_AGENT_INTERNAL_H */
