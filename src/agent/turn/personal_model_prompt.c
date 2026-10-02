/* src/agent/turn/personal_model_prompt.c — the personal-model prompt block
 * agent_turn_run renders per turn, carved out of agent_turn.c so the turn
 * file stays under its line pins (tests/test_turn_sources.c). Contract:
 * hu_turn_personal_model_prompt in include/human/agent/turn.h. */
#include "../agent_internal.h"
#include "human/agent/turn.h"
#include "human/config.h"
#include "human/memory.h"
#include "human/memory/confidence_boundary.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"

size_t hu_turn_personal_model_prompt(hu_agent_t *agent, char *buf, size_t cap) {
    if (!agent || !buf || cap == 0)
        return 0;
    /* HU_CONFIDENCE_BOUNDARY: other contacts' facts out of the view (live). */
    hu_personal_model_t *pm_owned = NULL;
    const hu_personal_model_t *pm_view =
        hu_confidence_pm_view(agent->alloc, &agent->personal_model, agent->memory_session_id,
                              agent->memory_session_id_len, &pm_owned);
    size_t pm_n = 0;
    if (pm_view && hu_personal_model_has_content(pm_view)) {
        /* T7 of docs/plans/2026-05-26-reflection-loop: when the reflection
         * loop is enabled in config, the per-channel slice is appended via
         * _build_prompt_with_reflection (db + channel + max_patterns).
         * Otherwise we fall back to the plain _build_prompt path so callers
         * with no SQLite memory backend (or reflection disabled) keep the
         * existing behavior. */
#ifdef HU_ENABLE_SQLITE
        if (agent->config && agent->config->reflection_loop.enabled && agent->memory &&
            agent->active_channel && agent->active_channel_len > 0) {
            sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
            const hu_persona_overlay_t *refl_overlay =
                agent->persona ? hu_persona_find_overlay(agent->persona, agent->active_channel,
                                                         agent->active_channel_len)
                               : NULL;
            pm_n = hu_personal_model_build_prompt_with_reflection(pm_view, refl_overlay, refl_db,
                                                                  agent->active_channel,
                                                                  /*max_patterns=*/5, buf, cap);
        } else
#endif
        {
            pm_n = hu_personal_model_build_prompt(pm_view, buf, cap);
        }
    }
    hu_confidence_pm_view_free(agent->alloc, pm_owned);
    return pm_n;
}
