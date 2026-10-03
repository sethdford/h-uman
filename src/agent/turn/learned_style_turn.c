/* Learned Style Profile — per-turn wiring. Contract:
 * include/human/agent/learned_style_turn.h.
 *
 * HU_LEARNED_STYLE activation gated on the blind-A/B human gate
 * (scripts/blind_ab_gate.py) plus the length-drift report
 * (scripts/learned_style_drift.py): do not flip to default-ON without a
 * measurement showing replies with the learned line read at least as much
 * like the owner AND land closer to his per-contact length distribution
 * (.claude/rules/feature-gate-requires-measurement.md). The promotion recipe
 * is in docs/guides/learned-style-runtime.md. */
#include "human/agent/learned_style_turn.h"

#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/persona.h"
#include "human/persona/learned_style.h"

#include <string.h>
#include <strings.h>

#define HU_LS_LINE_CAP 320

typedef enum { LS_HEAD_FULL = 0, LS_HEAD_LEAN, LS_HEAD_COMPACT } ls_head_kind_t;

static const char *ls_head_name(ls_head_kind_t k) {
    return k == LS_HEAD_LEAN ? "lean" : k == LS_HEAD_COMPACT ? "compact" : "full";
}

static void ls_count(hu_persona_style_opts_t *o, const char *entry) {
    char fb[1024];
    (void)hu_persona_style_opts_filter(o, entry, fb, sizeof(fb));
}

/* SHADOW: the sentences the LIVE build of this head kind would remove,
 * counted straight from the persona — no second head build, no RAG. Mirrors
 * the entries each builder filters (pinned by a SHADOW==LIVE count test). */
static size_t ls_count_head(const hu_persona_t *p, const char *ch, size_t ch_len,
                            ls_head_kind_t kind) {
    hu_persona_style_opts_t o = {NULL, 0, true, 0};
    if (kind == LS_HEAD_LEAN) {
        for (size_t i = 0; i < p->communication_rules_count && i < 12; i++)
            ls_count(&o, p->communication_rules[i]);
        for (size_t i = 0; i < p->style_rules_count; i++)
            ls_count(&o, p->style_rules[i]);
        const hu_persona_overlay_t *ov = hu_persona_find_overlay(p, ch, ch_len);
        if (ov) {
            ls_count(&o, ov->avg_length);
            for (size_t i = 0; i < ov->style_notes_count; i++)
                ls_count(&o, ov->style_notes[i]);
        }
    } else if (kind == LS_HEAD_COMPACT) {
        for (size_t i = 0; i < p->overlays_count; i++) {
            const hu_persona_overlay_t *ov = &p->overlays[i];
            if (!ov->channel || strlen(ov->channel) != ch_len ||
                strncasecmp(ov->channel, ch, ch_len) != 0)
                continue;
            ls_count(&o, ov->avg_length);
            for (size_t j = 0; j < ov->style_notes_count && j < 4; j++)
                ls_count(&o, ov->style_notes[j]);
            break;
        }
        for (size_t i = 0; i < p->communication_rules_count && i < 4; i++)
            ls_count(&o, p->communication_rules[i]);
    }
    return o.suppressed;
}

hu_error_t hu_agent_build_head_learned(hu_agent_t *agent, bool lean, const char *topic,
                                       size_t topic_len, const char *inbound, size_t inbound_len,
                                       char **out, size_t *out_len, hu_learned_style_turn_t *ls) {
    hu_learned_style_turn_t local;
    if (!ls)
        ls = &local;
    memset(ls, 0, sizeof(*ls));
    if (!agent)
        return HU_ERR_INVALID_ARGUMENT;

    /* Decide before building, so LIVE builds the head once, already right. */
    hu_gate_mode_t mode = hu_learned_style_mode();
    const hu_persona_t *p = agent->persona;
    const hu_contact_profile_t *cp = NULL;
    if (mode != HU_GATE_OFF && p && !agent->proactive_turn && agent->memory_session_id &&
        agent->memory_session_id_len > 0)
        cp = hu_persona_find_contact(p, agent->memory_session_id, agent->memory_session_id_len);
    hu_ls_shape_t shape = HU_LS_SHAPE_CASUAL;
    hu_learned_style_t st;
    memset(&st, 0, sizeof(st));
    char line[HU_LS_LINE_CAP];
    size_t line_len = 0;
    if (cp) {
        shape = hu_learned_style_shape_inbound(inbound, inbound_len);
        if (hu_learned_style_lookup_for(p->name, p->name ? strlen(p->name) : 0,
                                        agent->memory_session_id, agent->memory_session_id_len,
                                        shape, &st))
            line_len = hu_learned_style_render_line(
                &st, shape, cp->name, cp->name ? strlen(cp->name) : 0, line, sizeof(line));
    }
    hu_persona_style_opts_t opts = {line, line_len, true, 0};
    hu_persona_style_opts_t *use = (mode == HU_GATE_LIVE && line_len > 0) ? &opts : NULL;

    bool compact = false;
    hu_error_t err =
        lean ? hu_agent_build_lean_persona_head_ex(agent, inbound, inbound_len, use, out, out_len)
             : hu_agent_build_persona_head_ex(agent, topic, topic_len, use, out, out_len, &compact);
    if (err != HU_OK || !cp)
        return err; /* OFF / ineligible: the head is exactly what it was */

    const char *tag = mode == HU_GATE_LIVE ? "live" : "shadow";
    if (!st.found) {
        hu_log_info("learned_style", agent->observer,
                    "[learned_style %s] found=0 level=none shape=%s", tag,
                    hu_learned_style_shape_name(shape));
        return HU_OK;
    }
    ls->found = true;
    ls->line_bytes = line_len;
    ls_head_kind_t kind = lean ? LS_HEAD_LEAN : compact ? LS_HEAD_COMPACT : LS_HEAD_FULL;
    if (kind != LS_HEAD_FULL && line_len > 0) {
        size_t head_n =
            use ? opts.suppressed
                : ls_count_head(p, agent->active_channel, agent->active_channel_len, kind);
        size_t contact_n =
            agent->contact_context
                ? hu_learned_style_strip_contact(agent->contact_context, agent->contact_context_len,
                                                 NULL, 0, NULL)
                : 0;
        ls->suppressed_rules = head_n + contact_n;
        ls->live = use != NULL;
    }
    /* ONE aggregate line per turn: enums, counts and byte sizes only — never
     * the handle, the name or the line's text. */
    hu_log_info("learned_style", agent->observer,
                "[learned_style %s] found=1 level=%s shape=%s n=%u p50=%u p90=%u "
                "suppressed_rules=%zu line_bytes=%zu head=%s applied=%d",
                tag,
                st.from_bucket    ? "bucket"
                : st.from_contact ? "contact"
                                  : "global",
                hu_learned_style_shape_name(shape), (unsigned)st.n, (unsigned)st.len_p50,
                (unsigned)st.len_p90, ls->suppressed_rules, ls->line_bytes, ls_head_name(kind),
                ls->live ? 1 : 0);
    return HU_OK;
}
