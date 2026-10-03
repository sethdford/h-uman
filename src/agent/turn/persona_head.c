/* Persona head selection for a turn (HU_PERSONA_HEAD off|shadow|live), with the
 * HU_LEARNED_STYLE options hook (hu_agent_build_persona_head_ex). Moved out of
 * agent_turn.c on the 2026-10-02 integration train: #586 added the _ex variant
 * and pushed agent_turn.c past its TS_AGENT_TURN_C_MAX_LINES hand pin, which
 * main's #592 had tightened after #586 branched. Behaviour unchanged. */
#include "human/agent.h"
#include "human/agent/prompt_trim.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/persona.h"

hu_error_t hu_agent_build_persona_head(hu_agent_t *agent, const char *topic, size_t topic_len,
                                       char **out, size_t *out_len) {
    return hu_agent_build_persona_head_ex(agent, topic, topic_len, NULL, out, out_len, NULL);
}

hu_error_t hu_agent_build_persona_head_ex(hu_agent_t *agent, const char *topic, size_t topic_len,
                                          hu_persona_style_opts_t *opts, char **out,
                                          size_t *out_len, bool *compact_built) {
    if (compact_built)
        *compact_built = false;
    if (!agent || !agent->alloc || !agent->persona || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    const char *ch = agent->active_channel;
    size_t ch_len = agent->active_channel_len;
    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_PERSONA_HEAD", HU_GATE_OFF);
    if (mode == HU_GATE_LIVE) {
        hu_error_t cerr = hu_persona_build_prompt_compact_immersive_ex(
            agent->alloc, agent->persona, ch, ch_len, opts, out, out_len);
        if (cerr == HU_OK && compact_built)
            *compact_built = true;
        if (cerr == HU_OK)
            return HU_OK;
        /* fail-safe: any compact-build failure reverts to OFF behavior */
    }
    hu_error_t err = hu_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len, topic,
                                             topic_len, out, out_len);
    if (err != HU_OK)
        return err;
    if (mode == HU_GATE_SHADOW) {
        char *compact = NULL;
        size_t compact_len = 0;
        if (hu_persona_build_prompt_compact_immersive(agent->alloc, agent->persona, ch, ch_len,
                                                      &compact, &compact_len) == HU_OK) {
            hu_log_info("persona_head", agent->observer,
                        "shadow: full_head=%zu compact_head=%zu budget=%d", *out_len, compact_len,
                        HU_PROMPT_TRIM_BUDGET_BYTES);
            agent->alloc->free(agent->alloc->ctx, compact, compact_len + 1);
        }
    }
    return HU_OK;
}
