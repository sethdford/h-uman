/* ─────────────────────────────────────────────────────────────────────────
 * test_follow_up.c
 *
 * Pins hu_followup_compute_send_time — the circadian-aware delay predicate
 * that decides when to fire a follow-up to a contact who read our message
 * but hasn't replied.
 *
 * Three invariant clusters:
 *   1. Refusals — NONE warmth, NULL input, zero read_at all return 0.
 *   2. Base-delay matrix — CLOSE lands in [42, 78] min, FRIEND in [84, 156].
 *   3. Chronotype-aware snap — a late-night read snaps forward to the next
 *      active hour for the contact's chronotype; an OWL contact can still
 *      get a follow-up at midnight-ish, but a LARK gets the same read
 *      bumped to the next morning.
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/follow_up.h"
#include "test_framework.h"

#include <stdint.h>

/* Jan 1, 2026 00:00:00 UTC — a fixed, easy-to-reason-about baseline. */
#define BASELINE_MS 1767225600000ULL
#define HOUR_MS     (3600ULL * 1000ULL)
#define MIN_MS      (60ULL * 1000ULL)

static uint64_t at_hour(uint8_t utc_hour) {
    return BASELINE_MS + (uint64_t)utc_hour * HOUR_MS;
}

/* Extract the local hour-of-day (0–23) from a wall-clock ms timestamp. */
static uint8_t local_hour(uint64_t ms, int tz_offset_seconds) {
    int64_t adjusted = (int64_t)(ms / 1000ULL) + (int64_t)tz_offset_seconds;
    int64_t day_sec = adjusted % 86400;
    if (day_sec < 0)
        day_sec += 86400;
    return (uint8_t)(day_sec / 3600);
}

/* ── Refusal cases ──────────────────────────────────────────────────────── */

static void followup_null_input_returns_zero(void) {
    HU_ASSERT_EQ(hu_followup_compute_send_time(NULL), 0ULL);
}

static void followup_warmth_none_returns_zero(void) {
    /* Default warmth for acquaintances / unknown relationships should drop
     * the follow-up entirely — we don't bump strangers. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_NONE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    HU_ASSERT_EQ(hu_followup_compute_send_time(&in), 0ULL);
}

static void followup_zero_read_at_returns_zero(void) {
    /* read_at_ms == 0 means we don't actually know when (or whether) the
     * contact read the message. Don't fabricate a follow-up time. */
    hu_followup_input_t in = {
        .read_at_ms = 0,
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    HU_ASSERT_EQ(hu_followup_compute_send_time(&in), 0ULL);
}

/* ── Base delay matrix ──────────────────────────────────────────────────── */

static void followup_close_friend_lands_within_42_78_min_band(void) {
    /* CLOSE: base 60 min, jitter 70-130% → [42, 78] min when no snap fires.
     * 14:00 UTC is an active hour for any chronotype, so no snap. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    uint64_t send_at = hu_followup_compute_send_time(&in);
    HU_ASSERT(send_at > 0);
    uint64_t delta_min = (send_at - in.read_at_ms) / MIN_MS;
    HU_ASSERT(delta_min >= 42);
    HU_ASSERT(delta_min <= 78);
}

static void followup_friend_lands_within_84_156_min_band(void) {
    /* FRIEND: base 120 min, jitter 70-130% → [84, 156] min when no snap. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_FRIEND,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 7,
    };
    uint64_t send_at = hu_followup_compute_send_time(&in);
    HU_ASSERT(send_at > 0);
    uint64_t delta_min = (send_at - in.read_at_ms) / MIN_MS;
    HU_ASSERT(delta_min >= 84);
    HU_ASSERT(delta_min <= 156);
}

/* ── Chronotype-aware snap ──────────────────────────────────────────────── */

static void followup_late_night_read_snaps_to_next_morning_intermediate(void) {
    /* INTERMEDIATE active 07-22. Read at 23:00 + 60min jittered = 00:42-01:18
     * → quiet → snap forward to next 07:00. Verify the resulting send hour
     * is in [7, 22]. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(23),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    uint64_t send_at = hu_followup_compute_send_time(&in);
    HU_ASSERT(send_at > 0);
    uint8_t hour = local_hour(send_at, 0);
    HU_ASSERT(hour >= 7 && hour <= 22);
}

static void followup_late_night_read_owl_can_still_fire_late(void) {
    /* OWL active 09-23. Read at 21:00 + 60min jittered = 21:42-22:18 →
     * still active for OWL → no snap, fires natural delay. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(21),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_EVENING_OWL,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    uint64_t send_at = hu_followup_compute_send_time(&in);
    HU_ASSERT(send_at > 0);
    uint64_t delta_min = (send_at - in.read_at_ms) / MIN_MS;
    /* Natural CLOSE delay band, no snap inflation */
    HU_ASSERT(delta_min >= 42);
    HU_ASSERT(delta_min <= 78);
}

static void followup_early_morning_read_lark_lands_in_active_band(void) {
    /* LARK active 06-21. Read at 05:00 + 60min = 06:00-ish → at the active
     * boundary. Verify the snap settled into an active hour. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(5),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_MORNING_LARK,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    uint64_t send_at = hu_followup_compute_send_time(&in);
    HU_ASSERT(send_at > 0);
    uint8_t hour = local_hour(send_at, 0);
    HU_ASSERT(hour >= 6 && hour <= 21);
}

/* ── Determinism + jitter ───────────────────────────────────────────────── */

static void followup_same_seed_yields_same_output(void) {
    /* Pure predicate: same input → same output, no hidden state. */
    hu_followup_input_t in = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 42,
    };
    uint64_t a = hu_followup_compute_send_time(&in);
    uint64_t b = hu_followup_compute_send_time(&in);
    HU_ASSERT(a > 0);
    HU_ASSERT_EQ(a, b);
}

static void followup_different_seeds_yield_different_outputs(void) {
    /* Two distant seeds should land in different jitter slots. This catches
     * a regression where jitter is silently disabled and every contact gets
     * the exact same send_at. */
    hu_followup_input_t in_a = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    hu_followup_input_t in_b = in_a;
    in_b.seed = 100000;
    uint64_t a = hu_followup_compute_send_time(&in_a);
    uint64_t b = hu_followup_compute_send_time(&in_b);
    HU_ASSERT(a > 0);
    HU_ASSERT(b > 0);
    HU_ASSERT(a != b);
}

/* ── tz offset semantics ────────────────────────────────────────────────── */

static void followup_tz_offset_affects_snap_decision(void) {
    /* Same UTC moment, two contacts in opposite hemispheres.
     *   PST (UTC-8): UTC 14:00 → local 06:00 = LARK active (06-21) → no snap.
     *   SGT (UTC+8): UTC 14:00 → local 22:00 = LARK quiet (06-21) → snap.
     * The SGT send_at must be strictly later than the PST send_at because
     * snapping advances the timestamp; both use the same read_at and seed. */
    hu_followup_input_t in_pst = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_MORNING_LARK,
        .local_tz_offset_seconds = -28800,
        .seed = 1,
    };
    hu_followup_input_t in_sgt = in_pst;
    in_sgt.local_tz_offset_seconds = 28800;

    uint64_t send_pst = hu_followup_compute_send_time(&in_pst);
    uint64_t send_sgt = hu_followup_compute_send_time(&in_sgt);

    HU_ASSERT(send_pst > 0);
    HU_ASSERT(send_sgt > 0);
    HU_ASSERT(send_sgt > send_pst);
}

/* ── Warmth-string → tier mapping ───────────────────────────────────────── */

static void followup_warmth_from_string_null_and_empty(void) {
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string(NULL), (int)HU_FOLLOWUP_WARMTH_NONE);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string(""), (int)HU_FOLLOWUP_WARMTH_NONE);
}

static void followup_warmth_from_string_close_keywords(void) {
    /* "close", "high", "warm" — all CLOSE per persona conventions in
     * src/context/conversation.c which used "high" / "warm" for close. */
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("close"), (int)HU_FOLLOWUP_WARMTH_CLOSE);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("Close Friend"),
                 (int)HU_FOLLOWUP_WARMTH_CLOSE);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("HIGH"), (int)HU_FOLLOWUP_WARMTH_CLOSE);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("warm"), (int)HU_FOLLOWUP_WARMTH_CLOSE);
}

static void followup_warmth_from_string_friend_keyword(void) {
    /* Plain "friend" (without "close" qualifier) → FRIEND tier. */
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("friend"), (int)HU_FOLLOWUP_WARMTH_FRIEND);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("Friend"), (int)HU_FOLLOWUP_WARMTH_FRIEND);
}

static void followup_warmth_from_string_close_friend_maps_to_close(void) {
    /* "close friend" must map to CLOSE, not FRIEND — CLOSE keywords are
     * checked first, so the more-specific tier wins. */
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("close friend"),
                 (int)HU_FOLLOWUP_WARMTH_CLOSE);
}

static void followup_warmth_from_string_acquaintance_returns_none(void) {
    /* Acquaintance / colleague / etc. — no follow-up. */
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("acquaintance"), (int)HU_FOLLOWUP_WARMTH_NONE);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("colleague"), (int)HU_FOLLOWUP_WARMTH_NONE);
    HU_ASSERT_EQ((int)hu_followup_warmth_from_string("unknown"), (int)HU_FOLLOWUP_WARMTH_NONE);
}

/* ── Template selector ──────────────────────────────────────────────────── */

static void followup_template_close_returns_bump_text(void) {
    const char *t = hu_followup_template_for_warmth(HU_FOLLOWUP_WARMTH_CLOSE);
    HU_ASSERT(t != NULL);
    HU_ASSERT(strlen(t) > 0);
    HU_ASSERT(strlen(t) < 60); /* templates stay short — they're bumps, not paragraphs */
}

static void followup_template_friend_returns_thoughts_text(void) {
    const char *t = hu_followup_template_for_warmth(HU_FOLLOWUP_WARMTH_FRIEND);
    HU_ASSERT(t != NULL);
    HU_ASSERT(strlen(t) > 0);
    HU_ASSERT(strlen(t) < 60);
}

static void followup_template_close_and_friend_differ(void) {
    /* Different tier → different template. If a future change accidentally
     * makes both tiers return the same string, this fires. */
    const char *c = hu_followup_template_for_warmth(HU_FOLLOWUP_WARMTH_CLOSE);
    const char *f = hu_followup_template_for_warmth(HU_FOLLOWUP_WARMTH_FRIEND);
    HU_ASSERT(c != NULL);
    HU_ASSERT(f != NULL);
    HU_ASSERT(strcmp(c, f) != 0);
}

static void followup_template_none_returns_null(void) {
    /* NONE warmth → no template at all. The daemon glue uses NULL as the
     * sentinel for "don't schedule." */
    HU_ASSERT(hu_followup_template_for_warmth(HU_FOLLOWUP_WARMTH_NONE) == NULL);
}

/* ── Composite decision ─────────────────────────────────────────────────── */

static void followup_decide_close_friend_yields_decision(void) {
    hu_followup_input_t in = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_CLOSE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    hu_followup_decision_t d = hu_followup_decide(&in);
    HU_ASSERT(d.should_schedule);
    HU_ASSERT(d.send_at_ms > in.read_at_ms);
    HU_ASSERT(d.template_text != NULL);
}

static void followup_decide_none_warmth_returns_no_schedule(void) {
    hu_followup_input_t in = {
        .read_at_ms = at_hour(14),
        .warmth = HU_FOLLOWUP_WARMTH_NONE,
        .contact_chronotype = HU_CHRONO_INTERMEDIATE,
        .local_tz_offset_seconds = 0,
        .seed = 1,
    };
    hu_followup_decision_t d = hu_followup_decide(&in);
    HU_ASSERT_FALSE(d.should_schedule);
    HU_ASSERT_EQ(d.send_at_ms, 0ULL);
    HU_ASSERT(d.template_text == NULL);
}

static void followup_decide_null_input_returns_no_schedule(void) {
    hu_followup_decision_t d = hu_followup_decide(NULL);
    HU_ASSERT_FALSE(d.should_schedule);
    HU_ASSERT_EQ(d.send_at_ms, 0ULL);
    HU_ASSERT(d.template_text == NULL);
}

/* ── Registration ───────────────────────────────────────────────────────── */

void run_follow_up_tests(void);
void run_follow_up_tests(void) {
    HU_TEST_SUITE("follow-up scheduler");

    HU_RUN_TEST(followup_null_input_returns_zero);
    HU_RUN_TEST(followup_warmth_none_returns_zero);
    HU_RUN_TEST(followup_zero_read_at_returns_zero);

    HU_RUN_TEST(followup_close_friend_lands_within_42_78_min_band);
    HU_RUN_TEST(followup_friend_lands_within_84_156_min_band);

    HU_RUN_TEST(followup_late_night_read_snaps_to_next_morning_intermediate);
    HU_RUN_TEST(followup_late_night_read_owl_can_still_fire_late);
    HU_RUN_TEST(followup_early_morning_read_lark_lands_in_active_band);

    HU_RUN_TEST(followup_same_seed_yields_same_output);
    HU_RUN_TEST(followup_different_seeds_yield_different_outputs);

    HU_RUN_TEST(followup_tz_offset_affects_snap_decision);

    HU_RUN_TEST(followup_warmth_from_string_null_and_empty);
    HU_RUN_TEST(followup_warmth_from_string_close_keywords);
    HU_RUN_TEST(followup_warmth_from_string_friend_keyword);
    HU_RUN_TEST(followup_warmth_from_string_close_friend_maps_to_close);
    HU_RUN_TEST(followup_warmth_from_string_acquaintance_returns_none);

    HU_RUN_TEST(followup_template_close_returns_bump_text);
    HU_RUN_TEST(followup_template_friend_returns_thoughts_text);
    HU_RUN_TEST(followup_template_close_and_friend_differ);
    HU_RUN_TEST(followup_template_none_returns_null);

    HU_RUN_TEST(followup_decide_close_friend_yields_decision);
    HU_RUN_TEST(followup_decide_none_warmth_returns_no_schedule);
    HU_RUN_TEST(followup_decide_null_input_returns_no_schedule);
}
