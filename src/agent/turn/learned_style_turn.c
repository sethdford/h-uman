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

#define HU_LS_LINE_CAP 320

typedef enum { LS_HEAD_FULL = 0, LS_HEAD_LEAN, LS_HEAD_COMPACT } ls_head_kind_t;

static const char *ls_head_name(ls_head_kind_t k) {
    return k == LS_HEAD_LEAN ? "lean" : k == LS_HEAD_COMPACT ? "compact" : "full";
}

/* Entries of the contact profile LIVE strips (Dynamic sentences, Pattern
 * lines), counted on the same rendering the prompt receives. */
static size_t ls_contact_suppressible(hu_allocator_t *alloc, const hu_contact_profile_t *cp) {
    char *ctx = NULL;
    size_t ctx_len = 0;
    if (hu_contact_profile_build_context(alloc, cp, &ctx, &ctx_len) != HU_OK || !ctx)
        return 0;
    size_t n = hu_learned_style_strip_contact(ctx, ctx_len, NULL, 0, NULL);
    alloc->free(alloc->ctx, ctx, ctx_len + 1);
    return n;
}

void hu_agent_learned_style_apply(hu_agent_t *agent, const char *msg, size_t msg_len, char **head,
                                  size_t *head_len, hu_learned_style_turn_t *out) {
    hu_learned_style_turn_t local;
    if (!out)
        out = &local;
    memset(out, 0, sizeof(*out));
    hu_gate_mode_t mode = hu_learned_style_mode();
    if (mode == HU_GATE_OFF)
        return; /* OFF: no lookup, no log, head untouched */
    if (!agent || !agent->alloc || !agent->persona || !head || !*head || !head_len ||
        !agent->memory_session_id || agent->memory_session_id_len == 0)
        return;
    /* Eligibility: a known 1:1 contact. Group chat ids and strangers are not
     * persona contacts, and the learner only measures persona contacts. */
    const hu_persona_t *p = agent->persona;
    const hu_contact_profile_t *cp =
        hu_persona_find_contact(p, agent->memory_session_id, agent->memory_session_id_len);
    if (!cp)
        return;

    const char *tag = mode == HU_GATE_LIVE ? "live" : "shadow";
    hu_learned_style_set_persona(p->name, p->name ? strlen(p->name) : 0);
    hu_ls_shape_t shape = hu_learned_style_shape(msg, msg_len);
    hu_learned_style_t ls;
    if (!hu_learned_style_lookup(agent->memory_session_id, agent->memory_session_id_len, shape,
                                 &ls)) {
        hu_log_info("learned_style", agent->observer,
                    "[learned_style %s] found=0 level=none shape=%s", tag,
                    hu_learned_style_shape_name(shape));
        return;
    }
    out->found = true;

    char line[HU_LS_LINE_CAP];
    size_t line_len = hu_learned_style_render_line(
        &ls, shape, cp->name, cp->name ? strlen(cp->name) : 0, line, sizeof(line));
    out->line_bytes = line_len;

    ls_head_kind_t kind = LS_HEAD_FULL;
    if (agent->lean_prompt)
        kind = LS_HEAD_LEAN;
    else if (hu_gate_mode_from_env("HU_PERSONA_HEAD", HU_GATE_OFF) == HU_GATE_LIVE)
        kind = LS_HEAD_COMPACT;

    /* Rebuild the active head with the line in and the fixed-length entries
     * out. SHADOW builds it only to count, then discards it. */
    hu_persona_style_opts_t opts = {line, line_len, true, 0};
    char *nh = NULL;
    size_t nhl = 0;
    hu_error_t err = HU_ERR_NOT_SUPPORTED;
    if (line_len > 0 && kind == LS_HEAD_LEAN)
        err = hu_agent_build_lean_persona_head_ex(agent, msg, msg_len, &opts, &nh, &nhl);
    else if (line_len > 0 && kind == LS_HEAD_COMPACT)
        err = hu_persona_build_prompt_compact_immersive_ex(
            agent->alloc, p, agent->active_channel, agent->active_channel_len, &opts, &nh, &nhl);
    out->suppressed_rules = opts.suppressed + ls_contact_suppressible(agent->alloc, cp);

    if (err == HU_OK && nh && mode == HU_GATE_LIVE) {
        agent->alloc->free(agent->alloc->ctx, *head, *head_len + 1);
        *head = nh;
        *head_len = nhl;
        nh = NULL;
        out->live = true;
    }
    if (nh)
        agent->alloc->free(agent->alloc->ctx, nh, nhl + 1);

    /* ONE aggregate line per turn: enums, counts and byte sizes only — never
     * the handle, the name or the line's text. */
    hu_log_info("learned_style", agent->observer,
                "[learned_style %s] found=1 level=%s shape=%s n=%u p50=%u p90=%u "
                "suppressed_rules=%zu line_bytes=%zu head=%s applied=%d",
                tag,
                ls.from_bucket    ? "bucket"
                : ls.from_contact ? "contact"
                                  : "global",
                hu_learned_style_shape_name(shape), (unsigned)ls.n, (unsigned)ls.len_p50,
                (unsigned)ls.len_p90, out->suppressed_rules, out->line_bytes, ls_head_name(kind),
                out->live ? 1 : 0);
}
