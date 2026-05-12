#ifndef HU_AGENT_TYPING_SIMULATOR_H
#define HU_AGENT_TYPING_SIMULATOR_H

/* SOTA-2026 init #11 — Stephanie2 typing-simulation half (S1).
 *
 * Public surface for human-paced outgoing messages. Channels that natively
 * support typing indicators (Telegram sendChatAction, Slack postMessage,
 * iMessage AX bubble) pulse the indicator while we "type"; channels without
 * native indicators fall back to a sleep-only latency.
 *
 * The PRISM proactivity-gate half of the design (`hu_proactivity_gate`) is
 * deferred to a follow-up sprint — see docs/plans/2026-05-11-init-11-…md.
 *
 * Determinism contract: same `hu_typing_profile_t` + same message length
 * ⇒ same chosen typing-budget. Achieved by a per-profile xorshift64 seeded
 * from the profile's `seed` field (zero ⇒ a fixed fallback constant, NOT
 * `time(NULL)`, so HU_IS_TEST schedules are reproducible even when the
 * caller forgot to set a seed).
 *
 * Under HU_IS_TEST: no real sleep is executed and no real `start_typing`
 * pulses fire on the channel. The chosen latency budget and the action
 * trace are recorded into a global test-only buffer that the test reads
 * back via `hu_typing_test_take_last`.
 */

#include "human/channel.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard wall-clock ceiling for the whole simulation. Long messages that
 * would exceed this fall back to an immediate send. 15 s matches the
 * design note D4: longer animations annoy users. */
#define HU_TYPING_HARD_CEILING_MS 15000u

/* Per-(persona, channel) typing profile. Owned by the persona overlay —
 * see hu_persona_overlay_t. Init #02 (MoLoRA channels) eventually owns the
 * JSON load/save of the overlay-attached profile; this header just defines
 * the wire/struct shape and the read-side defaults.
 *
 * Field documentation:
 *   avg_wpm            — words-per-minute baseline (5 chars per word
 *                        convention). 0 ⇒ instant send (no animation).
 *   wpm_stddev         — Gaussian-ish variance applied per chunk, in WPM.
 *                        Clamped at [10, 200] effective WPM.
 *   pause_on_comma_ms  — extra wall-time after a ',' / ';' / ':'.
 *   pause_on_period_ms — extra wall-time after '.' / '!' / '?' / '\n'.
 *   jitter_pct         — global multiplicative jitter, 0–100. 10 means
 *                        ±10 % swing on the chosen budget.
 *   instant            — bypass simulation entirely (emergency override).
 *   seed               — xorshift64 seed. 0 ⇒ fixed reproducible default.
 */
typedef struct hu_typing_profile {
    uint16_t avg_wpm;
    uint16_t wpm_stddev;
    uint16_t pause_on_comma_ms;
    uint16_t pause_on_period_ms;
    uint8_t  jitter_pct;
    bool     instant;
    uint32_t seed;
} hu_typing_profile_t;

#define HU_TYPING_PROFILE_DEFAULTS                                                                \
    {                                                                                             \
        .avg_wpm = 65, .wpm_stddev = 12, .pause_on_comma_ms = 180, .pause_on_period_ms = 420,     \
        .jitter_pct = 10, .instant = false, .seed = 0u                                            \
    }

/* Trace of which actions the simulator chose. Used by tests under
 * HU_IS_TEST to assert determinism and ordering; in production builds,
 * the buffer is not populated and reading it yields zero entries. */
typedef enum hu_typing_action_kind {
    HU_TYPING_ACTION_START_TYPING = 0,
    HU_TYPING_ACTION_STOP_TYPING,
    HU_TYPING_ACTION_SEND,
} hu_typing_action_kind_t;

typedef struct hu_typing_action {
    uint32_t                elapsed_ms;
    hu_typing_action_kind_t kind;
} hu_typing_action_t;

/* Maximum schedule rows captured in test mode. Long messages refresh the
 * indicator every 4 s for up to 15 s ⇒ at most ~5 refresh pulses; the
 * cap is set generously to make accidental overrun loud. */
#define HU_TYPING_SCHEDULE_CAP 32

/* Capability probe — returns true iff the channel exposes BOTH a
 * start_typing and a stop_typing vtable entry. Channels without both
 * gracefully fall back to a single sleep budget, no fake pulses. */
bool hu_channel_supports_typing(const hu_channel_t *ch);

/* Compute the typing latency budget for a message of `message_len`
 * bytes under the given profile. Result is clamped at
 * HU_TYPING_HARD_CEILING_MS. Pure function: no side effects, no
 * allocation, deterministic given the profile seed. */
uint32_t hu_typing_compute_budget_ms(const hu_typing_profile_t *profile, size_t message_len);

/* Resolve the active typing profile for a channel.
 *
 * Read order:
 *   1. The persona overlay for `channel_name`, IF the overlay carries a
 *      typing profile (Init #02 wires this; for now overlay→typing is
 *      not yet a persona field, so this stays a default-only path).
 *   2. A persona-global default profile (also future work).
 *   3. `HU_TYPING_PROFILE_DEFAULTS`.
 *
 * `persona` is intentionally untyped here (void *) so this header does
 * not pull in `human/persona.h` for callers that only need the typing
 * surface; the implementation casts to `const hu_persona_t *` only
 * when `HU_ENABLE_PERSONA` is on. NULL persona ⇒ defaults. */
void hu_typing_profile_resolve(const void *persona, const char *channel_name,
                               hu_typing_profile_t *out);

/* Send-with-typing wrapper. Composes (in order):
 *   1. If channel supports typing AND profile is not instant:
 *        ch->vtable->start_typing(target),
 *        sleep budget_ms (refreshing the indicator every ≤ 4 s),
 *        ch->vtable->stop_typing(target).
 *   2. If channel does NOT support typing AND profile is not instant:
 *        sleep budget_ms only.
 *   3. ch->vtable->send(target, message, media).
 *
 * Hard ceiling: budget never exceeds HU_TYPING_HARD_CEILING_MS.
 *
 * `profile` is optional: NULL means "send instantly, no typing", which
 * preserves backward compatibility with existing call sites that pass
 * no profile. Existing `ch->vtable->send(...)` call sites do NOT need
 * to be modified — adoption is gradual, opt-in per call site.
 *
 * Returns the underlying send()'s error; HU_OK on success. Returns
 * HU_ERR_INVALID_ARGUMENT on NULL channel or NULL vtable. */
hu_error_t hu_typing_send(hu_channel_t *ch, const char *target, size_t target_len,
                          const char *message, size_t message_len, const char *const *media,
                          size_t media_count, const hu_typing_profile_t *profile);

/* Test-only: read back the schedule captured by the most recent
 * hu_typing_send() invocation. Returns the number of actions written to
 * `out` (capped at HU_TYPING_SCHEDULE_CAP) and the total chosen budget
 * in `out_budget_ms`. In non-HU_IS_TEST builds this is a no-op and
 * returns 0. The buffer is reset on every hu_typing_send() call. */
size_t hu_typing_test_take_last(hu_typing_action_t *out, size_t out_cap, uint32_t *out_budget_ms);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TYPING_SIMULATOR_H */
