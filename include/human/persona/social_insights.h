/* include/human/persona/social_insights.h
 *
 * Sprint A.5 wiring (docs/plans/2026-05-19-sprint-backlog.md):
 * renders a human-readable "social insights" paragraph from the
 * personal model's reaction signature. The output is designed to be
 * spliced into the persona prompt that goes to the LLM, so the
 * assistant can reference what it has learned about the user's
 * social graph from observed reactions.
 *
 * This is the smallest possible WIRE between today's three shipped
 * Tier-2 modules (identity / signatures / drift) and the runtime
 * prompt. Future Sprint A.6 work expands this into per-contact
 * adapters, calendar-aware proactive surfacing, etc. */

#ifndef HU_PERSONA_SOCIAL_INSIGHTS_H
#define HU_PERSONA_SOCIAL_INSIGHTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_personal_model;

/* Render a 1-5 sentence summary of what the personal model has learned
 * about the user's social graph from reaction data. Returns bytes
 * written (excluding NUL). 0 means "no insights available" (no
 * reaction-derived facts in the model), in which case callers should
 * NOT splice an empty paragraph into the prompt — let it be absent.
 *
 * Format pinned by tests:
 *   "Reaction-derived insights:
 *    - <contact1>: <N positive> / <M negative> recent reactions
 *    - <contact2>: ...
 *    Salient topics from reactions: hiking, weekend, ..."
 *
 * Designed for the agent prompt — concise, structured, no LLM call
 * needed at render time (the underlying signature was already
 * computed; this just formats it). */
size_t hu_persona_render_social_insights(const struct hu_personal_model *model, char *out,
                                         size_t cap);

/* Sprint A.7 consumer: read ~/.human/social_state.json (written every
 * 6h by hu_daemon_social_tick) and render the most actionable bits
 * into a prompt-ready paragraph. Highlights:
 *   - Stale contacts (top 3 by gap-days) — "you haven't heard from
 *     Alice in 21 days"
 *   - Pattern drift alerts (PRONOUNCED severity only) — "Bob's reply
 *     style has shifted recently"
 *
 * Returns bytes written, 0 if the file doesn't exist or is empty (no
 * paragraph to emit). NULL path → use the default ~/.human/social_state.json.
 *
 * Tolerant of missing fields and malformed JSON (returns 0 silently
 * rather than crashing — degradation should be invisible to the user). */
size_t hu_persona_render_social_state_snapshot(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_SOCIAL_INSIGHTS_H */
