#ifndef HU_AGENT_CONTACT_STAGE_TURN_H
#define HU_AGENT_CONTACT_STAGE_TURN_H

/* Per-contact relationship stage for the turn (DEF-16).
 *
 * agent->relationship used to be one agent-wide state incremented on every
 * turn for every contact. hu_contact_stage_refresh() derives the stage of
 * `contact` alone from the contact's own messages, Seth's own replies (the
 * learned-style profile) and the persona's declared Dunbar layer
 * (persona/contact_stage.h).
 *
 * Gate: HU_REL_STAGE_DERIVED=off|shadow|live (default OFF).
 *   off    — nothing: the old agent-wide stage, no query, no write.
 *   shadow — derives and logs "[contact_stage shadow] prev=… stage=…";
 *            changes nothing and writes nothing.
 *   live   — agent->relationship.stage = the derived stage and .derived =
 *            true (turn counting stops raising it); the derived row goes to
 *            its own table (contact_rel_stage). session_count and
 *            total_turns are NOT touched, and nothing is written to
 *            frontier_state's rel_* columns beyond what the old end-of-turn
 *            save writes, so turning the gate off needs no data cleanup.
 *            A failed derivation (no memory backend, query error) resets the
 *            stage to the persona prior alone (NEW without one) instead of
 *            leaving the previous contact's stage in place.
 *
 * Called where a turn learns its contact: the daemon's per-batch context load
 * (before the length calibration reads the stage) and the turn entry (every
 * path). The owner-level norms (median contact) and the learned-style reply
 * counts are cached for HU_CONTACT_STAGE_NORMS_TTL_S; a turn costs one
 * indexed per-contact count.
 *
 * Logs carry counts only, never the contact id. Each norms recompute logs the
 * persisted (before) vs derived (after) stage distribution:
 *   [contact_stage] distribution contacts=N before=a/b/c/d after=a/b/c/d … */

#include "human/agent.h"
#include "human/core/gate_mode.h"
#include "human/persona/relationship.h"

#include <stddef.h>
#include <stdint.h>

#define HU_CONTACT_STAGE_NORMS_TTL_S 3600

/* $HU_REL_STAGE_DERIVED=off|shadow|live, default OFF. */
hu_gate_mode_t hu_contact_stage_mode(void);

/* Returns the stage now in agent->relationship (unchanged unless LIVE).
 * NULL agent -> HU_REL_NEW. */
hu_relationship_stage_t hu_contact_stage_refresh(hu_agent_t *agent, const char *contact,
                                                 size_t contact_len);

/* refresh() at an explicit clock (tests: the norms cache TTL). */
hu_relationship_stage_t hu_contact_stage_refresh_at(hu_agent_t *agent, const char *contact,
                                                    size_t contact_len, int64_t now_unix);

/* Drop the cached norms (tests, persona reload). */
void hu_contact_stage_cache_reset(void);

#endif /* HU_AGENT_CONTACT_STAGE_TURN_H */
