#include "human/moment.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void compose_from_inputs_rejects_null_out(void) {
    hu_error_t err =
        hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, NULL, 1700000000, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void compose_from_inputs_zero_inputs_produces_safe_defaults(void) {
    hu_moment_t m = {0};
    hu_error_t err = hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, NULL, 1700000000, &m);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(m.time_since_their_last_msg_s, -1);
    HU_ASSERT_EQ(m.time_since_our_last_msg_s, -1);
    HU_ASSERT_FALSE(m.thread_is_continuation);
    HU_ASSERT_FALSE(m.topic_still_open);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_NONE);
    HU_ASSERT_EQ(m.suggested_brevity, HU_MOMENT_BREVITY_MIRROR);
    HU_ASSERT_EQ(m.defer_send_until_s, 0);
    HU_ASSERT_EQ(m.composed_at_s, 1700000000);
    HU_ASSERT_EQ(m.source_flags, 0u);
}

static void compose_computes_time_since_their_last_when_provided(void) {
    hu_moment_t m = {0};
    int64_t now = 1700000000;
    hu_moment_compose_from_inputs(NULL, NULL, NULL, now - 600, /* their */
                                  now - 300,                   /* ours  */
                                  NULL, now, &m);
    HU_ASSERT_EQ(m.time_since_their_last_msg_s, 600);
    HU_ASSERT_EQ(m.time_since_our_last_msg_s, 300);
    HU_ASSERT_TRUE(m.source_flags & HU_MOMENT_SRC_LAST_THEIR_TS);
    HU_ASSERT_TRUE(m.source_flags & HU_MOMENT_SRC_LAST_OUR_TS);
}

static void compose_clamps_negative_delta_to_zero_on_clock_skew(void) {
    hu_moment_t m = {0};
    int64_t now = 1700000000;
    hu_moment_compose_from_inputs(NULL, NULL, NULL,
                                  now + 60, /* their ts is in the future — bad clock */
                                  -1, NULL, now, &m);
    HU_ASSERT_EQ(m.time_since_their_last_msg_s, 0);
    HU_ASSERT_TRUE(m.source_flags &
                   HU_MOMENT_SRC_LAST_THEIR_TS); /* future ts is >= 0; bit must still set */
}

static void compose_keeps_minus_one_when_timestamp_missing(void) {
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, NULL, 1700000000, &m);
    HU_ASSERT_EQ(m.time_since_their_last_msg_s, -1);
    HU_ASSERT_EQ(m.time_since_our_last_msg_s, -1);
    HU_ASSERT_FALSE(m.source_flags & HU_MOMENT_SRC_LAST_THEIR_TS);
}

/* ---- Phase field tests (Task 1.3) ---- */

/* 1779609600 = 2026-05-24T08:00:00Z (verified via Python datetime) */
#define TS_8AM_UTC ((int64_t)1779609600)
/* 1779591600 = 2026-05-24T03:00:00Z */
#define TS_3AM_UTC ((int64_t)1779591600)

/* phase_local always uses the machine's local TZ (NULL passed to phase_for_tz),
   so we cannot assert specific phase values on phase_local without controlling
   the test machine's timezone. Instead we exercise the hour→phase boundary
   table deterministically via phase_theirs, which honors contact_tz when set. */

static void compose_phase_theirs_is_morning_for_8am_in_contact_utc(void) {
    /* 08:00 UTC falls in MORNING window (07:30–11:00) */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "UTC", TS_8AM_UTC, &m);
    HU_ASSERT_EQ(m.phase_theirs, HU_MOMENT_PHASE_MORNING);
}

static void compose_phase_theirs_is_deep_night_for_3am_in_contact_utc(void) {
    /* 03:00 UTC falls in DEEP_NIGHT window (00:00–05:30) */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "UTC", TS_3AM_UTC, &m);
    HU_ASSERT_EQ(m.phase_theirs, HU_MOMENT_PHASE_DEEP_NIGHT);
}

static void compose_phase_theirs_uses_contact_tz_when_provided(void) {
    /* 08:00 UTC = 01:00 PDT (America/Los_Angeles, UTC-7 in May).
       Their phase should be DEEP_NIGHT; local (UTC) should be MORNING. */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "America/Los_Angeles", TS_8AM_UTC, &m);
    HU_ASSERT_EQ(m.phase_theirs, HU_MOMENT_PHASE_DEEP_NIGHT);
    HU_ASSERT_TRUE(m.source_flags & HU_MOMENT_SRC_CONTACT_TZ);
}

static void compose_phase_theirs_falls_back_to_local_when_tz_unknown(void) {
    /* No contact_tz → phase_theirs mirrors phase_local; flag not set. */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, NULL, TS_8AM_UTC, &m);
    HU_ASSERT_EQ(m.phase_theirs, m.phase_local);
    HU_ASSERT_FALSE(m.source_flags & HU_MOMENT_SRC_CONTACT_TZ);
}

static void compose_flags_unusual_hour_when_their_phase_is_deep_night(void) {
    /* Their tz resolves to 01:00 → DEEP_NIGHT → unusual_hour = true */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "America/Los_Angeles", TS_8AM_UTC, &m);
    HU_ASSERT_TRUE(m.it_is_unusual_hour_for_them);
}

static void compose_does_not_flag_unusual_hour_when_their_phase_is_morning(void) {
    /* Their tz = UTC → 08:00 → MORNING → unusual_hour = false */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "UTC", TS_8AM_UTC, &m);
    HU_ASSERT_FALSE(m.it_is_unusual_hour_for_them);
}

/* ---- Thread continuation + topic state tests (Task 1.4) ---- */

static void compose_marks_continuation_when_last_inbound_within_30m(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 60}; /* 1 minute ago — well within 30-minute window */
    bool ob[] = {false};
    const char *tx[] = {"hey"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_TRUE(m.thread_is_continuation);
    hu_moment_history_free(h);
}

static void compose_does_not_mark_continuation_when_gap_exceeds_30m(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 7200}; /* 2 hours ago — outside window */
    bool ob[] = {false};
    const char *tx[] = {"hey"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 7200, -1, "UTC", now, &m);
    HU_ASSERT_FALSE(m.thread_is_continuation);
    hu_moment_history_free(h);
}

static void compose_marks_topic_open_when_last_inbound_recent_and_text_substantive(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 120}; /* 2 minutes ago */
    bool ob[] = {false};
    const char *tx[] = {"monday standup got pushed to wednesday"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 120, -1, "UTC", now, &m);
    HU_ASSERT_TRUE(m.topic_still_open);
    HU_ASSERT_TRUE(m.topic_hint[0] != '\0');
    hu_moment_history_free(h);
}

/* ---- Style inference tests (Task 1.5) ---- */

static void compose_avg_length_words_matches_inbound_messages(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 600, now - 500, now - 400, now - 300, now - 200};
    bool ob[] = {false, false, false, false, false};
    const char *tx[] = {"yeah totally", "yep", "ok cool", "lol same", "for real"};
    struct hu_conversation_history_t *h = hu_moment_history_create(5, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 200, -1, "UTC", now, &m);
    /* word counts: 2 + 1 + 2 + 2 + 2 = 9, avg = 9/5 = 1.8 → rounds to 2 */
    HU_ASSERT_TRUE(m.their_avg_length_words >= 2 && m.their_avg_length_words <= 3);
    HU_ASSERT_EQ(m.their_recent_tone, HU_MOMENT_TONE_TERSE);
    hu_moment_history_free(h);
}

static void compose_detects_lowercase_when_inbound_is_lowercase(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    const char *tx[] = {"yeah totally same"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_TRUE(m.they_use_lowercase);
    HU_ASSERT_FALSE(m.they_use_emoji);
    HU_ASSERT_FALSE(m.they_use_punctuation_eol);
    hu_moment_history_free(h);
}

static void compose_detects_emoji_when_inbound_contains_emoji(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    const char *tx[] = {"lol \xF0\x9F\x98\x82 same"}; /* U+1F602 😂 in UTF-8 */
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_TRUE(m.they_use_emoji);
    hu_moment_history_free(h);
}

static void compose_distressed_tone_for_long_vent_with_negative_markers(void) {
    int64_t now = 1779609600;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    const char *tx[] = {"ugh i can't believe this happened again, i'm so tired of it, "
                        "nothing ever works out and i don't know what to do anymore"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    HU_ASSERT_NOT_NULL(h);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.their_recent_tone, HU_MOMENT_TONE_DISTRESSED);
    hu_moment_history_free(h);
}

/* ---- suggested_open decision tree tests (Task 1.6) ---- */

/* Saves the current TZ env so a test can override it deterministically and
 * restore it after. Used to control phase_local (which reads the machine TZ
 * via phase_for_tz(now_s, NULL)). */
typedef struct {
    char saved[64];
    bool was_set;
} tz_env_save_t;

static void tz_env_override(tz_env_save_t *s, const char *tz) {
    const char *cur = getenv("TZ");
    s->was_set = (cur != NULL);
    if (s->was_set)
        snprintf(s->saved, sizeof s->saved, "%s", cur);
    setenv("TZ", tz, 1);
    tzset();
}

static void tz_env_restore(tz_env_save_t *s) {
    if (s->was_set)
        setenv("TZ", s->saved, 1);
    else
        unsetenv("TZ");
    tzset();
}

static void compose_open_is_NONE_on_continuation(void) {
    int64_t now = TS_8AM_UTC;
    int64_t ts[] = {now - 60}; /* 1 min ago — continuation */
    bool ob[] = {false};
    const char *tx[] = {"hey"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_TRUE(m.thread_is_continuation);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_NONE);
    hu_moment_history_free(h);
}

static void compose_open_is_RECONNECT_after_4_days(void) {
    int64_t now = TS_8AM_UTC;
    int64_t four_days_ago = now - 4 * 86400;
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, four_days_ago, four_days_ago, "UTC", now, &m);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_RECONNECT);
}

static void compose_open_is_ACKNOWLEDGE_GAP_at_their_3am(void) {
    /* 08:00 UTC = 01:00 PDT → DEEP_NIGHT for the contact → unusual_hour. */
    int64_t now = TS_8AM_UTC;
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, now - 7200, -1, "America/Los_Angeles", now, &m);
    HU_ASSERT_TRUE(m.it_is_unusual_hour_for_them);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_ACKNOWLEDGE_GAP);
}

static void compose_open_is_GREET_MORNING_at_7am_fresh_day(void) {
    /* phase_local depends on machine TZ. Force TZ=UTC so 08:00 UTC = MORNING. */
    tz_env_save_t saved;
    tz_env_override(&saved, "UTC");

    int64_t now = TS_8AM_UTC;
    int64_t last_their = now - 14 * 3600; /* 14h ago — fresh-day gap (>8h) */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, last_their, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.phase_local, HU_MOMENT_PHASE_MORNING);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_GREET_MORNING);

    tz_env_restore(&saved);
}

static void compose_open_is_GREET_NIGHT_when_they_say_night_at_11pm(void) {
    /* 23:00 UTC = NIGHT in UTC. Force TZ=UTC. Inbound is 2h old, outside the
     * 30-min continuation window, so rule 1 (continuation→NONE) doesn't fire
     * and the tree reaches rule 5 (phase=NIGHT + "night"/"tomorrow" → GREET_NIGHT). */
    tz_env_save_t saved;
    tz_env_override(&saved, "UTC");

    int64_t now = TS_8AM_UTC + 15 * 3600; /* 08:00 UTC + 15h = 23:00 UTC */
    int64_t two_hours_ago = now - 2 * 3600;
    int64_t ts[] = {two_hours_ago};
    bool ob[] = {false};
    const char *tx[] = {"night talk tomorrow"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, two_hours_ago, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.phase_local, HU_MOMENT_PHASE_NIGHT);
    HU_ASSERT_FALSE(m.thread_is_continuation);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_GREET_NIGHT);
    hu_moment_history_free(h);

    tz_env_restore(&saved);
}

static void compose_open_is_NONE_in_unremarkable_midday(void) {
    /* Midday, recent-but-not-continuation contact, no special markers. */
    tz_env_save_t saved;
    tz_env_override(&saved, "UTC");

    int64_t now = TS_8AM_UTC + 4 * 3600; /* 12:00 UTC = MIDDAY */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, now - 7200, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.phase_local, HU_MOMENT_PHASE_MIDDAY);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_NONE);

    tz_env_restore(&saved);
}

/* Regression guard: even at fresh-day morning conditions, if the contact's
 * timezone resolves to DEEP_NIGHT, we MUST NOT greet them with "good morning".
 * Per spec §Success Criteria #5 — zero "good morning at 3am" regressions. */
static void compose_does_NOT_suggest_morning_greet_at_3am(void) {
    tz_env_save_t saved;
    tz_env_override(&saved, "UTC"); /* phase_local = MORNING */

    int64_t now = TS_8AM_UTC;
    int64_t fresh_day = now - 14 * 3600;
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, fresh_day, -1, "America/Los_Angeles", now, &m);
    /* phase_local=MORNING WOULD say GREET_MORNING — but unusual-hour overrides. */
    HU_ASSERT_EQ(m.phase_local, HU_MOMENT_PHASE_MORNING);
    HU_ASSERT_TRUE(m.it_is_unusual_hour_for_them);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_ACKNOWLEDGE_GAP);
    HU_ASSERT_TRUE(m.suggested_open != HU_MOMENT_OPEN_GREET_MORNING);

    tz_env_restore(&saved);
}

/* ---- suggested_close / suggested_brevity / defer_send tests (Task 1.7) ---- */

static void compose_close_is_NONE_in_unremarkable_midday(void) {
    tz_env_save_t saved;
    tz_env_override(&saved, "UTC");
    int64_t now = TS_8AM_UTC + 4 * 3600; /* 12:00 MIDDAY */
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, now - 7200, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.suggested_close, HU_MOMENT_OPEN_NONE);
    tz_env_restore(&saved);
}

static void compose_close_is_GREET_NIGHT_at_night_with_warm_tone(void) {
    tz_env_save_t saved;
    tz_env_override(&saved, "UTC");
    int64_t now = TS_8AM_UTC + 15 * 3600; /* 23:00 NIGHT */
    int64_t ts[] = {now - 4 * 3600};
    bool ob[] = {false};
    const char *tx[] = {"yeah that sounds really good thanks for thinking of me"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 4 * 3600, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.their_recent_tone, HU_MOMENT_TONE_WARM);
    HU_ASSERT_EQ(m.suggested_close, HU_MOMENT_OPEN_GREET_NIGHT);
    hu_moment_history_free(h);
    tz_env_restore(&saved);
}

static void compose_brevity_is_TERSE_when_terse_tone_and_short_avg(void) {
    int64_t now = TS_8AM_UTC;
    int64_t ts[] = {now - 600, now - 500, now - 400, now - 300, now - 200};
    bool ob[] = {false, false, false, false, false};
    const char *tx[] = {"k", "yep", "ok", "lol", "sure"};
    struct hu_conversation_history_t *h = hu_moment_history_create(5, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 200, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.their_recent_tone, HU_MOMENT_TONE_TERSE);
    HU_ASSERT_EQ(m.suggested_brevity, HU_MOMENT_BREVITY_TERSE);
    hu_moment_history_free(h);
}

static void compose_brevity_is_SHORT_when_avg_is_moderate(void) {
    int64_t now = TS_8AM_UTC;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    /* 10 words, warm — not terse, not distressed */
    const char *tx[] = {"hey want to grab lunch tomorrow if you are free"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.suggested_brevity, HU_MOMENT_BREVITY_SHORT);
    hu_moment_history_free(h);
}

static void compose_brevity_is_MEDIUM_when_distressed(void) {
    int64_t now = TS_8AM_UTC;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    const char *tx[] = {"ugh i can't believe this happened again, i'm so tired of it, "
                        "nothing ever works out and i don't know what to do anymore"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "UTC", now, &m);
    HU_ASSERT_EQ(m.their_recent_tone, HU_MOMENT_TONE_DISTRESSED);
    HU_ASSERT_EQ(m.suggested_brevity, HU_MOMENT_BREVITY_MEDIUM);
    hu_moment_history_free(h);
}

static void compose_brevity_is_MIRROR_when_no_history(void) {
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "UTC", TS_8AM_UTC, &m);
    HU_ASSERT_EQ(m.suggested_brevity, HU_MOMENT_BREVITY_MIRROR);
}

static void compose_defer_send_is_zero_when_their_phase_is_not_deep_night(void) {
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "UTC", TS_8AM_UTC, &m);
    HU_ASSERT_EQ(m.defer_send_until_s, 0);
}

static void compose_defer_send_is_future_8am_when_deep_night_and_no_continuation(void) {
    /* Their tz LA: 08:00 UTC = 01:00 PDT → DEEP_NIGHT. No history (no continuation). */
    int64_t now = TS_8AM_UTC;
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, "America/Los_Angeles", now, &m);
    HU_ASSERT_EQ(m.phase_theirs, HU_MOMENT_PHASE_DEEP_NIGHT);
    HU_ASSERT_TRUE(m.defer_send_until_s > now);
    HU_ASSERT_TRUE(m.defer_send_until_s < now + 86400); /* within the next 24h */
}

static void compose_defer_send_is_zero_when_replying_to_night_signoff(void) {
    /* Their tz: DEEP_NIGHT, BUT they just said "good night" — we may reply now. */
    int64_t now = TS_8AM_UTC;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    const char *tx[] = {"night talk tomorrow"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, now - 60, -1, "America/Los_Angeles", now, &m);
    HU_ASSERT_EQ(m.phase_theirs, HU_MOMENT_PHASE_DEEP_NIGHT);
    HU_ASSERT_EQ(m.defer_send_until_s, 0);
    hu_moment_history_free(h);
}

/* ---- source_flag bookkeeping tests (Task 1.8) ---- */

/* The compose predicate never dereferences persona/overlay (yet), so tests can
 * pass any non-NULL pointer to assert the provenance flag is set. */
static void compose_records_persona_source_flag_when_persona_provided(void) {
    hu_moment_t m = {0};
    struct hu_persona_t *fake_persona = (struct hu_persona_t *)(uintptr_t)1;
    hu_moment_compose_from_inputs(fake_persona, NULL, NULL, -1, -1, NULL, TS_8AM_UTC, &m);
    HU_ASSERT_TRUE(m.source_flags & HU_MOMENT_SRC_PERSONA);
    HU_ASSERT_FALSE(m.source_flags & HU_MOMENT_SRC_OVERLAY);
}

static void compose_records_overlay_source_flag_when_overlay_provided(void) {
    hu_moment_t m = {0};
    struct hu_persona_overlay_t *fake_overlay = (struct hu_persona_overlay_t *)(uintptr_t)1;
    hu_moment_compose_from_inputs(NULL, fake_overlay, NULL, -1, -1, NULL, TS_8AM_UTC, &m);
    HU_ASSERT_TRUE(m.source_flags & HU_MOMENT_SRC_OVERLAY);
    HU_ASSERT_FALSE(m.source_flags & HU_MOMENT_SRC_PERSONA);
}

static void compose_records_history_source_flag_when_history_nonempty(void) {
    int64_t now = TS_8AM_UTC;
    int64_t ts[] = {now - 60};
    bool ob[] = {false};
    const char *tx[] = {"hey"};
    struct hu_conversation_history_t *h = hu_moment_history_create(1, ts, ob, tx);
    hu_moment_t m = {0};
    hu_moment_compose_from_inputs(NULL, NULL, h, -1, -1, NULL, now, &m);
    HU_ASSERT_TRUE(m.source_flags & HU_MOMENT_SRC_HISTORY);
    hu_moment_history_free(h);
}

/* ---- Public wrapper tests (Task 1.9) ---- */

/* The wrapper is intentionally stubbed until Phase 3 Task 3.2 wires it to
 * the real agent/contact accessors. Pinning the stub behavior here ensures
 * the symbol links and that Phase 3 will replace this test with a real one. */
static void compose_public_wrapper_returns_not_supported_until_phase3(void) {
    hu_moment_t m = {0};
    hu_error_t err = hu_moment_compose(NULL, NULL, "imessage", TS_8AM_UTC, &m);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void compose_public_wrapper_rejects_null_out(void) {
    hu_error_t err = hu_moment_compose(NULL, NULL, "imessage", TS_8AM_UTC, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ---- Downstream predicate tests (Task 1.10) ---- */

static void should_defer_send_false_when_zero(void) {
    hu_moment_t m = {0};
    HU_ASSERT_FALSE(hu_moment_should_defer_send(&m));
}

static void should_defer_send_true_when_positive(void) {
    hu_moment_t m = {0};
    m.defer_send_until_s = 1779609600;
    HU_ASSERT_TRUE(hu_moment_should_defer_send(&m));
}

static void should_defer_send_false_when_null(void) {
    HU_ASSERT_FALSE(hu_moment_should_defer_send(NULL));
}

static void should_trigger_followup_true_when_silence_long_topic_open(void) {
    hu_moment_t m = {0};
    m.time_since_their_last_msg_s = 21600; /* 6h */
    m.time_since_our_last_msg_s = 21600;
    m.topic_still_open = true;
    m.it_is_unusual_hour_for_them = false;
    HU_ASSERT_TRUE(hu_moment_should_trigger_followup(&m, 6 * 3600));
}

static void should_trigger_followup_false_when_silence_too_short(void) {
    hu_moment_t m = {0};
    m.time_since_their_last_msg_s = 60;
    m.time_since_our_last_msg_s = 60;
    m.topic_still_open = true;
    HU_ASSERT_FALSE(hu_moment_should_trigger_followup(&m, 6 * 3600));
}

static void should_trigger_followup_false_when_topic_closed(void) {
    hu_moment_t m = {0};
    m.time_since_their_last_msg_s = 21600;
    m.time_since_our_last_msg_s = 21600;
    m.topic_still_open = false;
    HU_ASSERT_FALSE(hu_moment_should_trigger_followup(&m, 6 * 3600));
}

static void should_trigger_followup_false_when_unusual_hour(void) {
    hu_moment_t m = {0};
    m.time_since_their_last_msg_s = 21600;
    m.time_since_our_last_msg_s = 21600;
    m.topic_still_open = true;
    m.it_is_unusual_hour_for_them = true;
    HU_ASSERT_FALSE(hu_moment_should_trigger_followup(&m, 6 * 3600));
}

static void should_trigger_followup_false_when_null(void) {
    HU_ASSERT_FALSE(hu_moment_should_trigger_followup(NULL, 3600));
}

static void brevity_cap_words_terse_is_8(void) {
    hu_moment_t m = {0};
    m.suggested_brevity = HU_MOMENT_BREVITY_TERSE;
    HU_ASSERT_EQ(hu_moment_brevity_cap_words(&m), 8);
}

static void brevity_cap_words_short_is_25(void) {
    hu_moment_t m = {0};
    m.suggested_brevity = HU_MOMENT_BREVITY_SHORT;
    HU_ASSERT_EQ(hu_moment_brevity_cap_words(&m), 25);
}

static void brevity_cap_words_medium_is_60(void) {
    hu_moment_t m = {0};
    m.suggested_brevity = HU_MOMENT_BREVITY_MEDIUM;
    HU_ASSERT_EQ(hu_moment_brevity_cap_words(&m), 60);
}

static void brevity_cap_words_long_is_no_cap(void) {
    hu_moment_t m = {0};
    m.suggested_brevity = HU_MOMENT_BREVITY_LONG;
    HU_ASSERT_EQ(hu_moment_brevity_cap_words(&m), 0);
}

static void brevity_cap_words_mirror_uses_their_avg_with_slack(void) {
    hu_moment_t m = {0};
    m.suggested_brevity = HU_MOMENT_BREVITY_MIRROR;
    m.their_avg_length_words = 10;
    /* 10 * 1.3 = 13. (10*13+5)/10 = 13 */
    HU_ASSERT_EQ(hu_moment_brevity_cap_words(&m), 13);
}

static void brevity_cap_words_null_returns_zero(void) {
    HU_ASSERT_EQ(hu_moment_brevity_cap_words(NULL), 0);
}

void run_moment_compose_tests(void) {
    HU_TEST_SUITE("moment_compose");
    HU_RUN_TEST(compose_from_inputs_rejects_null_out);
    HU_RUN_TEST(compose_from_inputs_zero_inputs_produces_safe_defaults);
    HU_RUN_TEST(compose_computes_time_since_their_last_when_provided);
    HU_RUN_TEST(compose_clamps_negative_delta_to_zero_on_clock_skew);
    HU_RUN_TEST(compose_keeps_minus_one_when_timestamp_missing);
    HU_RUN_TEST(compose_phase_theirs_is_morning_for_8am_in_contact_utc);
    HU_RUN_TEST(compose_phase_theirs_is_deep_night_for_3am_in_contact_utc);
    HU_RUN_TEST(compose_phase_theirs_uses_contact_tz_when_provided);
    HU_RUN_TEST(compose_phase_theirs_falls_back_to_local_when_tz_unknown);
    HU_RUN_TEST(compose_flags_unusual_hour_when_their_phase_is_deep_night);
    HU_RUN_TEST(compose_does_not_flag_unusual_hour_when_their_phase_is_morning);
    HU_RUN_TEST(compose_marks_continuation_when_last_inbound_within_30m);
    HU_RUN_TEST(compose_does_not_mark_continuation_when_gap_exceeds_30m);
    HU_RUN_TEST(compose_marks_topic_open_when_last_inbound_recent_and_text_substantive);
    HU_RUN_TEST(compose_avg_length_words_matches_inbound_messages);
    HU_RUN_TEST(compose_detects_lowercase_when_inbound_is_lowercase);
    HU_RUN_TEST(compose_detects_emoji_when_inbound_contains_emoji);
    HU_RUN_TEST(compose_distressed_tone_for_long_vent_with_negative_markers);
    HU_RUN_TEST(compose_open_is_NONE_on_continuation);
    HU_RUN_TEST(compose_open_is_RECONNECT_after_4_days);
    HU_RUN_TEST(compose_open_is_ACKNOWLEDGE_GAP_at_their_3am);
    HU_RUN_TEST(compose_open_is_GREET_MORNING_at_7am_fresh_day);
    HU_RUN_TEST(compose_open_is_GREET_NIGHT_when_they_say_night_at_11pm);
    HU_RUN_TEST(compose_open_is_NONE_in_unremarkable_midday);
    HU_RUN_TEST(compose_does_NOT_suggest_morning_greet_at_3am);
    HU_RUN_TEST(compose_close_is_NONE_in_unremarkable_midday);
    HU_RUN_TEST(compose_close_is_GREET_NIGHT_at_night_with_warm_tone);
    HU_RUN_TEST(compose_brevity_is_TERSE_when_terse_tone_and_short_avg);
    HU_RUN_TEST(compose_brevity_is_SHORT_when_avg_is_moderate);
    HU_RUN_TEST(compose_brevity_is_MEDIUM_when_distressed);
    HU_RUN_TEST(compose_brevity_is_MIRROR_when_no_history);
    HU_RUN_TEST(compose_defer_send_is_zero_when_their_phase_is_not_deep_night);
    HU_RUN_TEST(compose_defer_send_is_future_8am_when_deep_night_and_no_continuation);
    HU_RUN_TEST(compose_defer_send_is_zero_when_replying_to_night_signoff);
    HU_RUN_TEST(compose_records_persona_source_flag_when_persona_provided);
    HU_RUN_TEST(compose_records_overlay_source_flag_when_overlay_provided);
    HU_RUN_TEST(compose_records_history_source_flag_when_history_nonempty);
    HU_RUN_TEST(compose_public_wrapper_returns_not_supported_until_phase3);
    HU_RUN_TEST(compose_public_wrapper_rejects_null_out);
    HU_RUN_TEST(should_defer_send_false_when_zero);
    HU_RUN_TEST(should_defer_send_true_when_positive);
    HU_RUN_TEST(should_defer_send_false_when_null);
    HU_RUN_TEST(should_trigger_followup_true_when_silence_long_topic_open);
    HU_RUN_TEST(should_trigger_followup_false_when_silence_too_short);
    HU_RUN_TEST(should_trigger_followup_false_when_topic_closed);
    HU_RUN_TEST(should_trigger_followup_false_when_unusual_hour);
    HU_RUN_TEST(should_trigger_followup_false_when_null);
    HU_RUN_TEST(brevity_cap_words_terse_is_8);
    HU_RUN_TEST(brevity_cap_words_short_is_25);
    HU_RUN_TEST(brevity_cap_words_medium_is_60);
    HU_RUN_TEST(brevity_cap_words_long_is_no_cap);
    HU_RUN_TEST(brevity_cap_words_mirror_uses_their_avg_with_slack);
    HU_RUN_TEST(brevity_cap_words_null_returns_zero);
}
