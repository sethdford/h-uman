#ifndef HU_AGENT_FOLLOWUP_COMPOSE_H
#define HU_AGENT_FOLLOWUP_COMPOSE_H

/* Persona-composed follow-up nudges (SOTA roadmap #3: no deterministic
 * English ever reaches a contact).
 *
 * `hu_followup_template_for_warmth` still hands real people two hardcoded
 * strings — "hey, just bumping this" and "any thoughts on this?". They are
 * word-for-word identical every time, to every contact, forever. That is the
 * single most legible tell left on the proactive surface: a human who bumps a
 * thread writes a slightly different sentence each time, shaped by who they
 * are talking to and how long it has been.
 *
 * This module composes that nudge with the PRODUCTION persona prompt
 * (hu_persona_build_prompt) plus a short situational directive, once at
 * schedule time, and freezes the result. The scheduled-send loop never
 * regenerates it — same frozen-at-detect discipline as contextual_proactive,
 * which exists because regenerating at send time let one contact's topic bleed
 * into another's message.
 *
 * WHAT HAPPENS WHEN COMPOSITION FAILS — the deliberate part:
 *
 *   OFF (default)  static template, exactly as before. No behavior change.
 *   SHADOW         compose, log what it WOULD have sent, still send the
 *                  template. Measurement without behavior change.
 *   LIVE           send the composed nudge. If composition fails for ANY
 *                  reason — provider down, guard audit trips, multi-line,
 *                  over-long — SEND NOTHING.
 *
 * The LIVE failure path does NOT fall back to the template. Falling back is
 * the obvious design and it is wrong here: it would reintroduce the exact
 * hardcoded string this module exists to eliminate, at precisely the moments
 * nobody is watching. A follow-up is a nicety — skipping one costs almost
 * nothing, while a robotic one costs persona fidelity with a real person. When
 * in doubt, say nothing.
 *
 * Safety shape:
 *   - The directive builder is PURE (no clock, no env, no I/O) so prompts are
 *     unit-testable. It receives only structured context — contact id, warmth
 *     tier, read-age, channel — never the raw inbound message, so no quoted
 *     text can splice into the prompt.
 *   - The provider call sits behind HU_IS_TEST with an injectable fn pointer
 *     (pattern: src/agent/retrieval_planner_llm.c). The test binary can never
 *     reach a provider.
 *   - Composed output is rejected when empty, multi-line, over cap, or when it
 *     trips hu_guard_audit_self_talk_leak / _deliberation_leak. The proactive
 *     path never crosses hu_response_guard_check, so that audit runs here.
 *   - Activation OFF -> SHADOW -> LIVE via HU_FOLLOWUP_COMPOSE, default OFF,
 *     per .claude/rules/feature-gate-requires-measurement.md. Do NOT flip to
 *     live without a measurement showing composed nudges read as more human
 *     than the templates.
 *
 * Pinned by tests/test_followup_compose.c. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/follow_up.h" /* hu_followup_warmth_t */
#include "human/persona.h"
#include "human/provider.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max composed nudge length, NUL included. A bump is one short line; the
 * directive asks for under 120 chars and finalize rejects anything that does
 * not fit, so an over-runy model reply is dropped rather than truncated
 * mid-word. */
#define HU_FOLLOWUP_COMPOSE_MAX 192

/* Current gate mode. Reads HU_FOLLOWUP_COMPOSE (off|shadow|live), default OFF
 * — an unrecognized value fails closed, per hu_gate_mode_parse. */
hu_gate_mode_t hu_followup_compose_mode(void);

/* Test seam: force a mode without touching the environment. Pass -1 to revert
 * to reading the env. */
void hu_followup_compose_set_mode_for_test(int mode);

/* Build the situational directive handed to the model alongside the persona
 * prompt. PURE — no clock, env, or I/O.
 *
 * Returns the length written, or 0 (and out[0]='\0') on bad input, on a
 * no-follow-up warmth tier, or when the directive would be truncated. A
 * clipped directive would silently drop its trailing output-format
 * instruction, so truncation refuses rather than emits. */
size_t hu_followup_compose_directive(const char *contact_id, hu_followup_warmth_t warmth,
                                     unsigned read_age_hours, const char *channel, char *out,
                                     size_t cap);

/* Injectable composer for tests. */
typedef hu_error_t (*hu_followup_compose_llm_fn_t)(void *ctx, const char *system, size_t system_len,
                                                   const char *directive, size_t directive_len,
                                                   char *out, size_t cap);
void hu_followup_compose_set_llm_for_test(hu_followup_compose_llm_fn_t fn, void *ctx);

/* Compose the nudge. Writes a validated single-line message to `out` on HU_OK;
 * on any error `out` is an empty string and the caller must NOT send. */
hu_error_t hu_followup_compose_text(hu_allocator_t *alloc, const hu_persona_t *persona,
                                    hu_provider_t *provider, const char *channel,
                                    const char *directive, char *out, size_t cap);

/* Decide what to actually send. PURE — the whole activation policy in one
 * testable predicate (.claude/rules/security-predicate-extraction.md).
 *
 * Returns the text to send, or NULL meaning SEND NOTHING:
 *   OFF / SHADOW          -> template_text (unchanged behavior)
 *   LIVE, compose ok      -> composed
 *   LIVE, compose failed  -> NULL, and the caller must skip the follow-up.
 *                            It must NOT substitute the template; see the
 *                            failure-path rationale at the top of this file. */
const char *hu_followup_compose_pick(hu_gate_mode_t mode, hu_error_t compose_err,
                                     const char *composed, const char *template_text);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_FOLLOWUP_COMPOSE_H */
