#ifndef HU_AGENT_CONTACT_STAGE_TURN_H
#define HU_AGENT_CONTACT_STAGE_TURN_H

/* Per-contact relationship stage for the turn (DEF-16).
 *
 * agent->relationship used to be one agent-wide state incremented on every
 * turn for every contact. hu_contact_stage_refresh() replaces it with the
 * stage of `contact` alone, derived from that contact's interaction data in
 * the session store plus the persona's declared Dunbar layer
 * (persona/contact_stage.h), persists it to frontier_state.rel_stage, and
 * marks it derived so turn counting cannot raise it.
 *
 * Called where a turn learns its contact: the daemon's per-batch context load
 * (before the length calibration reads the stage) and the turn entry (every
 * path: reactive, proactive, gateway). Ungated: it is a correctness fix.
 *
 * Logs counts only — never the contact id. Once per process it logs the
 * persisted (before) vs derived (after) stage distribution:
 *   [contact_stage] distribution contacts=N before=a/b/c/d after=a/b/c/d
 * (NEW/FAMILIAR/TRUSTED/DEEP). */

#include "human/agent.h"
#include "human/persona/relationship.h"

#include <stddef.h>

/* Returns the stage now in agent->relationship. With no memory backend, no
 * SQLite, or an empty contact it changes nothing and returns the current
 * stage. NULL agent -> HU_REL_NEW. */
hu_relationship_stage_t hu_contact_stage_refresh(hu_agent_t *agent, const char *contact,
                                                 size_t contact_len);

#endif /* HU_AGENT_CONTACT_STAGE_TURN_H */
