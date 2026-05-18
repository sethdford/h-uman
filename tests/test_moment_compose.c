#include "human/moment.h"
#include "test_framework.h"

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
}
