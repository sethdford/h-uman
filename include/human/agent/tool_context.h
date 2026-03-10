#ifndef HU_AGENT_TOOL_CONTEXT_H
#define HU_AGENT_TOOL_CONTEXT_H

typedef struct hu_agent hu_agent_t;

/* Set/clear the current agent for tool execution (thread-local).
 * Called by agent loop at turn start/end so tools like send_message can access
 * the calling agent (mailbox, agent id). */
void hu_agent_set_current_for_tools(hu_agent_t *agent);
void hu_agent_clear_current_for_tools(void);
hu_agent_t *hu_agent_get_current_for_tools(void);

#endif /* HU_AGENT_TOOL_CONTEXT_H */
