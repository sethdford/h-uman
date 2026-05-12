/* SOTA-2026 init #11 — Stephanie2 typing-simulation half (S1).
 *
 * Deterministic, ASan-clean, sleep-free under HU_IS_TEST. The PRISM
 * proactivity-gate half is deferred: this file ships only the typing
 * simulator that decorates `hu_channel_t.send`.
 */

#include "human/agent/typing_simulator.h"

#include "human/channel.h"
#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef HU_IS_TEST
#include <time.h>
#endif

/* Per the design (D7), 4-second refreshes keep the channel-side
 * indicator alive (Telegram/Slack/iMessage all expire near 5 s). */
#define HU_TYPING_REFRESH_INTERVAL_MS 4000u

/* Average characters per "word" — 5 is the standard typing-test
 * convention. We don't tokenize because the message can contain any
 * byte sequence including control chars; the schedule is governed by
 * raw byte length, which matches user-perceived latency well enough. */
#define HU_TYPING_CHARS_PER_WORD 5u

/* Fixed fallback seed when the caller passes 0 — keeps tests
 * reproducible even if the caller forgot to set one. NOT
 * `time(NULL)`: the design requires same-profile-same-budget. */
#define HU_TYPING_DEFAULT_SEED 0x9E3779B97F4A7C15ull

/* ──────────────────────────────────────────────────────────────────
 * Test-mode capture buffer
 *
 * Under HU_IS_TEST we never actually sleep or hit the channel's
 * typing vtable. Instead we record what we would have done into a
 * file-static ring that tests read back.
 * ────────────────────────────────────────────────────────────────── */

#if HU_IS_TEST
static hu_typing_action_t g_last_schedule[HU_TYPING_SCHEDULE_CAP];
static size_t             g_last_schedule_len = 0u;
static uint32_t           g_last_budget_ms    = 0u;

static void typing_test_reset(void) {
    g_last_schedule_len = 0u;
    g_last_budget_ms    = 0u;
}

static void typing_test_record(uint32_t elapsed_ms, hu_typing_action_kind_t kind) {
    if (g_last_schedule_len >= HU_TYPING_SCHEDULE_CAP)
        return;
    g_last_schedule[g_last_schedule_len].elapsed_ms = elapsed_ms;
    g_last_schedule[g_last_schedule_len].kind       = kind;
    g_last_schedule_len++;
}
#else
static void typing_test_reset(void) {}
static void typing_test_record(uint32_t elapsed_ms, hu_typing_action_kind_t kind) {
    (void)elapsed_ms;
    (void)kind;
}
#endif

/* ──────────────────────────────────────────────────────────────────
 * Deterministic PRNG
 *
 * xorshift64* — small, fast, full-period, deterministic from a seed.
 * We use it to add bounded jitter to the typing budget without adding
 * a dependency or pulling in libc rand() (which is not reentrant and
 * shares global state with the rest of the daemon).
 * ────────────────────────────────────────────────────────────────── */

static uint64_t typing_xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* ──────────────────────────────────────────────────────────────────
 * Pure budget computation
 *
 * Wall-time = (chars / chars_per_ms) + comma/period pause counts
 *           + bounded jitter; clamped to HU_TYPING_HARD_CEILING_MS.
 *
 * Determinism: with seed != 0, identical (profile, message_len) tuples
 * always produce identical budgets. With seed == 0 we substitute a
 * fixed fallback seed (NOT clock time), so tests still see a stable
 * value when callers forget to seed.
 * ────────────────────────────────────────────────────────────────── */

uint32_t hu_typing_compute_budget_ms(const hu_typing_profile_t *profile, size_t message_len) {
    if (!profile || profile->instant)
        return 0u;
    /* Treat avg_wpm == 0 as "instant send, no animation" so callers
     * can disable simulation per-message without touching the global
     * profile. */
    if (profile->avg_wpm == 0u)
        return 0u;
    if (message_len == 0u)
        return 0u;

    /* Effective WPM with bounded jitter. We don't actually want a
     * Gaussian here — bounded uniform is plenty for "this feels
     * vaguely human" and is much cheaper. */
    uint32_t avg_wpm = (uint32_t)profile->avg_wpm;
    if (avg_wpm < 10u)
        avg_wpm = 10u;
    if (avg_wpm > 200u)
        avg_wpm = 200u;

    uint64_t state = profile->seed ? (uint64_t)profile->seed : HU_TYPING_DEFAULT_SEED;
    /* Stir the message length in too so different messages on the
     * same profile don't get the same jitter direction. */
    state ^= ((uint64_t)message_len) * 0xBF58476D1CE4E5B9ull;

    /* Base typing time. chars/sec = wpm * chars_per_word / 60.
     * ms = chars * 60_000 / (wpm * chars_per_word). */
    uint64_t chars   = (uint64_t)message_len;
    uint64_t base_ms = chars * 60000ull / ((uint64_t)avg_wpm * HU_TYPING_CHARS_PER_WORD);

    /* Add pause budget by counting punctuation. This is the cheapest
     * possible approximation — we scan the message length only when
     * the caller passes a buffer; the budget function is pure-on-
     * length, so we use an average punctuation rate of one pause per
     * ~20 chars (sentence) and one per ~80 chars (comma). */
    uint64_t period_pauses = chars / 80ull;          /* coarse */
    uint64_t comma_pauses  = chars / 200ull;
    base_ms += period_pauses * (uint64_t)profile->pause_on_period_ms;
    base_ms += comma_pauses  * (uint64_t)profile->pause_on_comma_ms;

    /* Bounded jitter. jitter_pct=10 means ±10%. We pick from a
     * 2^32 window and remap to [-jitter, +jitter]. */
    if (profile->jitter_pct > 0u) {
        uint32_t pct  = profile->jitter_pct > 100u ? 100u : (uint32_t)profile->jitter_pct;
        uint64_t r    = typing_xorshift64(&state);
        /* Signed swing: r as int64 then scaled. */
        int64_t  swing_signed = (int64_t)(r % (2ull * pct + 1ull)) - (int64_t)pct;
        int64_t  delta        = ((int64_t)base_ms * swing_signed) / 100;
        int64_t  jittered     = (int64_t)base_ms + delta;
        if (jittered < 0)
            jittered = 0;
        base_ms = (uint64_t)jittered;
    }

    if (base_ms > (uint64_t)HU_TYPING_HARD_CEILING_MS)
        base_ms = (uint64_t)HU_TYPING_HARD_CEILING_MS;
    return (uint32_t)base_ms;
}

/* ──────────────────────────────────────────────────────────────────
 * Capability probe
 * ────────────────────────────────────────────────────────────────── */

bool hu_channel_supports_typing(const hu_channel_t *ch) {
    if (!ch || !ch->vtable)
        return false;
    return ch->vtable->start_typing != NULL && ch->vtable->stop_typing != NULL;
}

/* ──────────────────────────────────────────────────────────────────
 * Profile resolution
 *
 * The overlay-attached typing profile is owned by Init #02 (MoLoRA
 * channels) per the design's cross-initiative map; this function
 * defines the read-side contract and returns defaults until the
 * overlay field lands. We avoid pulling persona.h here so callers
 * that only need the typing surface stay light.
 * ────────────────────────────────────────────────────────────────── */

void hu_typing_profile_resolve(const void *persona, const char *channel_name,
                               hu_typing_profile_t *out) {
    if (!out)
        return;
    static const hu_typing_profile_t kDefaults = HU_TYPING_PROFILE_DEFAULTS;
    *out = kDefaults;
    (void)persona;
    (void)channel_name;
    /* Init #02 wires per-channel overlay lookup here. For now the
     * default profile applies everywhere — this is the documented S1
     * scope. */
}

/* ──────────────────────────────────────────────────────────────────
 * Sleep shim
 *
 * Real sleep in production builds; no-op under HU_IS_TEST so the test
 * suite stays fast and deterministic.
 * ────────────────────────────────────────────────────────────────── */

static void typing_sleep_ms(uint32_t ms) {
#if HU_IS_TEST
    (void)ms;
#else
    if (ms == 0u)
        return;
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
#endif
}

/* ──────────────────────────────────────────────────────────────────
 * Public send-with-typing wrapper
 * ────────────────────────────────────────────────────────────────── */

hu_error_t hu_typing_send(hu_channel_t *ch, const char *target, size_t target_len,
                          const char *message, size_t message_len, const char *const *media,
                          size_t media_count, const hu_typing_profile_t *profile) {
    if (!ch || !ch->vtable || !ch->vtable->send)
        return HU_ERR_INVALID_ARGUMENT;

    typing_test_reset();

    uint32_t budget_ms = 0u;
    if (profile)
        budget_ms = hu_typing_compute_budget_ms(profile, message_len);

#if HU_IS_TEST
    g_last_budget_ms = budget_ms;
#endif

    bool use_typing = budget_ms > 0u && hu_channel_supports_typing(ch);

    if (use_typing) {
        typing_test_record(0u, HU_TYPING_ACTION_START_TYPING);
#if !HU_IS_TEST
        (void)ch->vtable->start_typing(ch->ctx, target, target_len);
#endif
        /* Refresh the indicator every ≤ 4 s so it doesn't expire. We
         * record (in test mode) every refresh pulse so tests can
         * assert "long messages got at least 2 pulses". */
        uint32_t elapsed = 0u;
        while (elapsed + HU_TYPING_REFRESH_INTERVAL_MS < budget_ms) {
            typing_sleep_ms(HU_TYPING_REFRESH_INTERVAL_MS);
            elapsed += HU_TYPING_REFRESH_INTERVAL_MS;
            typing_test_record(elapsed, HU_TYPING_ACTION_START_TYPING);
#if !HU_IS_TEST
            (void)ch->vtable->start_typing(ch->ctx, target, target_len);
#endif
        }
        /* Final tail sleep. */
        typing_sleep_ms(budget_ms - elapsed);
        typing_test_record(budget_ms, HU_TYPING_ACTION_STOP_TYPING);
#if !HU_IS_TEST
        (void)ch->vtable->stop_typing(ch->ctx, target, target_len);
#endif
    } else if (budget_ms > 0u) {
        /* Channel has no native typing indicator — fall back to a
         * single sleep budget. No fake "typing…" UX, just a delay
         * that makes the send feel paced. */
        typing_sleep_ms(budget_ms);
    }

    typing_test_record(budget_ms, HU_TYPING_ACTION_SEND);
    return ch->vtable->send(ch->ctx, target, target_len, message, message_len, media, media_count);
}

/* ──────────────────────────────────────────────────────────────────
 * Test introspection
 * ────────────────────────────────────────────────────────────────── */

size_t hu_typing_test_take_last(hu_typing_action_t *out, size_t out_cap, uint32_t *out_budget_ms) {
#if HU_IS_TEST
    if (out_budget_ms)
        *out_budget_ms = g_last_budget_ms;
    if (!out || out_cap == 0u)
        return g_last_schedule_len;
    size_t n = g_last_schedule_len < out_cap ? g_last_schedule_len : out_cap;
    memcpy(out, g_last_schedule, n * sizeof(*out));
    return n;
#else
    (void)out;
    (void)out_cap;
    if (out_budget_ms)
        *out_budget_ms = 0u;
    return 0u;
#endif
}
