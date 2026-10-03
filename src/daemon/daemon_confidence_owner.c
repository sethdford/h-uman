/* daemon_confidence_owner.c — the owner bypass of the confidence boundary
 * (HU_CONFIDENCE_BOUNDARY). agent->memory_session_id is the batch's contact,
 * including Seth's own handle when he texts his twin; without this the
 * boundary would filter his self-chat like a stranger's. Contract:
 * include/human/daemon/share_queue.h. */
#include "human/daemon/share_queue.h"

#include "human/agent.h"
#include "human/memory/confidence_boundary.h"

static bool owner_of_agent(const void *ctx, const char *contact, size_t contact_len) {
    const hu_agent_t *agent = (const hu_agent_t *)ctx;
    return agent && hu_share_is_owner(agent->persona, contact, contact_len);
}

void hu_daemon_confidence_owner_wire(const struct hu_agent *agent) {
    hu_confidence_set_owner_resolver(agent ? owner_of_agent : NULL, agent);
}
