#ifndef HU_PERSONA_LIFE_EVENTS_H
#define HU_PERSONA_LIFE_EVENTS_H

/* Life-event lifecycle for the modeled person.
 *
 * WHY THIS EXISTS (2026-07-27, human blind-A/B cycle 4, n=40):
 * 2 of 9 detections were the model asserting a WRONG STATE for an in-progress
 * life event. Asked "or are you still moving", it replied "done moving all
 * settled in now" while the real Seth was mid-move ("Moving the 23rd"). Asked
 * "when is your last day?", it replied "last day's friday yep still going
 * through with it" against a real answer of "officially the 31st but it was
 * today as I am on vacation the next two weeks".
 *
 * The model was NOT hallucinating. `core.identity` states life transitions as
 * completed steady-state ("Chief Architect at Raymond James ... Lives in a
 * waterfront place in St. Petersburg"), and the persona schema had no
 * `as_of`, no `valid_until`, and no in-progress representation anywhere. Given
 * a prose blob whose only tenses are "before" and "after", the model has no
 * way to express "during" — so it picks a plausible completion and states it
 * confidently. Same failure family as the HorizonBench result in
 * docs/research/2026-07-25-sota-gap-analysis.md section 3.
 *
 * THE CONTRACT: a confident wrong state is worse than an admitted unknown.
 * When an event's state is in-progress, unknown, or stale, the built prompt
 * must instruct the model to hedge or ask ("still packing?", "how'd the move
 * go?") rather than assert a completion. This mirrors the anti-AI-disclosure
 * guard: the failure is not a missing fact, it is an unearned certainty.
 *
 * SCOPE: this models the PERSON the persona describes (Seth's own life), not a
 * contact's calendar. Contact-side events live in `temporal_events`
 * (src/context/temporal_events.c), which is a separate, contact-keyed store
 * whose read path is a proactive check-in trigger — see that file. */

#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lifecycle state of a life event.
 *
 * UNKNOWN is deliberately the zero value: a persona that declares no state (or
 * an unparseable one) lands here, and UNKNOWN is the state that triggers the
 * do-not-assert guidance. Failing closed toward "hedge" is the safe default —
 * the failure this module exists to prevent is asserting a state you do not
 * have. */
typedef enum hu_life_event_state {
    HU_LIFE_EVENT_STATE_UNKNOWN = 0,
    HU_LIFE_EVENT_STATE_PENDING,     /* not started yet */
    HU_LIFE_EVENT_STATE_IN_PROGRESS, /* underway right now */
    HU_LIFE_EVENT_STATE_COMPLETED,   /* finished, confirmed */
    HU_LIFE_EVENT_STATE_CANCELLED,   /* called off, will not happen */
} hu_life_event_state_t;

/* A single life event.
 *
 * `as_of` is when the declared state was last CONFIRMED, not when the event
 * happens. It is what makes staleness derivable: a `pending` move whose
 * `expected_date` passed three weeks ago and has not been re-confirmed since
 * is not "completed" — it is unknown, and the model must not guess. */
typedef struct hu_life_event {
    char description[192];       /* "moving to the waterfront place in st pete" */
    hu_life_event_state_t state; /* declared state */
    int64_t as_of;               /* epoch secs the state was confirmed; 0 = never */
    int64_t expected_date;       /* epoch secs it is expected to resolve; 0 = open-ended */
} hu_life_event_t;

/* Parse a state string. EXACT (case-insensitive) token match against a closed
 * vocabulary — deliberately not substring and not word-boundary matching.
 *
 * ~/.claude/rules/substring-classifier-pitfalls.md prescribes word-boundary
 * matching for bucket keywords that overlap ("informal" wrongly matching
 * "formal"). That fix is insufficient HERE: word boundaries are non-alphanumeric
 * characters, and `_` is one, so "not_completed" WOULD word-match "completed"
 * and invert the meaning — the precise failure the rule warns about, surviving
 * the rule's own fix. For a closed enum vocabulary the correct matcher is
 * neither: it is exact. Unrecognized input returns UNKNOWN (fail toward
 * hedging), so a typo can never manufacture a completion claim. */
hu_life_event_state_t hu_life_event_state_from_string(const char *s, size_t len);

/* Human-readable label for a state (stable, lowercase, used in the prompt). */
const char *hu_life_event_state_str(hu_life_event_state_t state);

/* Parse "YYYY-MM-DD" to epoch seconds at UTC midnight. Returns 0 on any
 * malformed input, which callers treat as "unset".
 *
 * Uses a pure days-from-civil computation rather than mktime/timegm: both are
 * timezone-sensitive, and project rule requires deterministic tests. */
int64_t hu_life_event_parse_date(const char *s, size_t len);

/* THE PURE PREDICATE — derives the state to actually use at `now_ts`.
 *
 * Extracted as a standalone pure function per
 * .claude/rules/security-predicate-extraction.md: the decision that governs
 * whether the model is allowed to assert a completion is exactly the decision
 * that must be exhaustively unit-testable without building a persona or a
 * prompt. Inputs are facts, output is one small enum, no mutable state.
 *
 * The declared state is NOT always the effective state — staleness demotes it.
 * See the implementation for the policy and its rationale. */
hu_life_event_state_t hu_life_event_effective_state(const hu_life_event_t *ev, int64_t now_ts);

/* True when the model must NOT assert that this event has completed (or
 * otherwise resolved) — i.e. the effective state carries no confirmed
 * resolution. This is the predicate the directive text hangs off. */
bool hu_life_event_must_not_assert_completion(const hu_life_event_t *ev, int64_t now_ts);

/* Runtime gate. off (default) | shadow | live, via HU_LIFE_EVENTS.
 *
 * Per .claude/rules/feature-gate-requires-measurement.md this ships default
 * OFF and is promoted only by a measurement, never by a green suite. */
hu_gate_mode_t hu_life_events_gate(void);

/* Render the life-event block into `out`.
 *
 * Emits nothing (out_len = 0, HU_OK) when `count` is 0 — so a persona with no
 * `life_events` array is byte-identical to today's prompt. Events whose
 * effective state is COMPLETED or CANCELLED render as settled facts; every
 * other effective state renders WITH the do-not-assert-completion guidance.
 *
 * Truncates rather than overflowing; always NUL-terminates. */
hu_error_t hu_life_events_build_directive(const hu_life_event_t *events, size_t count,
                                          int64_t now_ts, char *out, size_t cap, size_t *out_len);

#endif /* HU_PERSONA_LIFE_EVENTS_H */
