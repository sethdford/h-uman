/* Tests for context-driven proactive outreach.
 *
 * The temporal logic is pure (resolve/decide take now_ts) so these are fully
 * deterministic. To stay timezone-independent, tests convert resolved send
 * times back through localtime_r and assert relationships (future, weekday,
 * 19:00) rather than hardcoding an epoch's local weekday. */
#include "human/agent/contextual_proactive.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <string.h>
#include <time.h>

/* A fixed "now" used across tests. Its actual local weekday is computed in-test. */
#define CP_NOW ((int64_t)1700000000) /* 2023-11-14T22:13:20Z */

static struct tm local_of(int64_t ts) {
    time_t t = (time_t)ts;
    struct tm out;
    memset(&out, 0, sizeof(out));
#if defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

/* ── activation gate ──────────────────────────────────────────────────────── */

static void mode_parse_defaults_off(void) {
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str(NULL),
                 (int)HU_CONTEXTUAL_PROACTIVE_OFF);
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str(""), (int)HU_CONTEXTUAL_PROACTIVE_OFF);
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str("off"),
                 (int)HU_CONTEXTUAL_PROACTIVE_OFF);
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str("garbage"),
                 (int)HU_CONTEXTUAL_PROACTIVE_OFF);
}

static void mode_parse_shadow_and_on(void) {
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str("shadow"),
                 (int)HU_CONTEXTUAL_PROACTIVE_SHADOW);
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str("SHADOW"),
                 (int)HU_CONTEXTUAL_PROACTIVE_SHADOW);
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str("on"), (int)HU_CONTEXTUAL_PROACTIVE_ON);
    HU_ASSERT_EQ((int)hu_contextual_proactive_mode_from_str("live"),
                 (int)HU_CONTEXTUAL_PROACTIVE_ON);
}

/* ── message build references the stored topic, never fabricates ──────────── */

static void build_message_references_topic(void) {
    char buf[256];
    size_t n = hu_contextual_proactive_build_message("interview", 9, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "interview") != NULL); /* the specific comes from the topic */
    HU_ASSERT_TRUE(strstr(buf, "how'd") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "go?") != NULL);
}

static void build_message_empty_topic_no_fabrication(void) {
    char buf[256];
    /* A topicless contextual proactive must never be invented. */
    HU_ASSERT_EQ(hu_contextual_proactive_build_message("", 0, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_EQ(hu_contextual_proactive_build_message(NULL, 0, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
}

/* ── topic normalization strips filler ────────────────────────────────────── */

static void normalize_topic_strips_filler(void) {
    char buf[128];
    HU_ASSERT_TRUE(hu_contextual_proactive_normalize_topic("my interview", 12, buf, sizeof(buf)) >
                   0);
    HU_ASSERT_STR_EQ(buf, "interview");

    HU_ASSERT_TRUE(hu_contextual_proactive_normalize_topic("I have surgery", 14, buf, sizeof(buf)) >
                   0);
    HU_ASSERT_STR_EQ(buf, "surgery");

    HU_ASSERT_TRUE(
        hu_contextual_proactive_normalize_topic("the appointment.", 16, buf, sizeof(buf)) > 0);
    HU_ASSERT_STR_EQ(buf, "appointment");
}

/* ── topic quality: only noun-phrase-like topics are sendable ─────────────── */

/* Regression pins from the 2026-07-18 iMessage quality audit: the event
 * extractor can hand back whole clauses as "descriptions"; splicing those into
 * "how'd the %s go?" produced real sends like "how'd the It will be tomorrow.
 * Im working go?". An unprompted text has an asymmetric cost profile — a
 * skipped send costs nothing, a garbage send costs trust — so the predicate
 * biases hard toward precision. */
static void topic_sendable_accepts_noun_phrases(void) {
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("interview", 9));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("surgery", 7));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("internet installers", 19));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("dentist appointment", 19));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("job interview", 13));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("swimming lessons", 16));
}

static void topic_sendable_rejects_sentence_fragments(void) {
    /* The four real-world garbage topics that were sent or queued. */
    static const char *garbage[] = {
        "It will be tomorrow. Im working",
        "went over to your place to measure walls.  Im thinking about doing a wall",
        "Im meeting the v internet installers",
        "Okay, I'll try",
    };
    for (size_t i = 0; i < sizeof(garbage) / sizeof(garbage[0]); i++)
        HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable(garbage[i], strlen(garbage[i])));

    /* Structural rejects: sentence punctuation, clause words, length. */
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("meeting. then dinner", 20));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("you know that thing", 19));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("think we need groceries", 23));
    HU_ASSERT_FALSE(
        hu_contextual_proactive_topic_is_sendable("really long topic with far too many words", 41));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("", 0));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable(NULL, 0));
}

/* Second wave of real leaked topics (service-loop log, 2026-07-15..21): the
 * 07-18 clause-word gate misses bare question words and discourse markers —
 * "What's" splits at the apostrophe into "what"+"s", neither of which was in
 * the list, producing the scheduled send "how'd the What's go?". */
static void topic_sendable_rejects_question_words_and_discourse_markers(void) {
    static const char *leaked[] = {
        "What\xe2\x80\x99s", /* curly apostrophe, as iMessage delivers it */
        "What's",            /* ASCII apostrophe variant */
        "So",
        "How ya feeling",
        "it\xe2\x80\x99s a lot better",
    };
    for (size_t i = 0; i < sizeof(leaked) / sizeof(leaked[0]); i++)
        HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable(leaked[i], strlen(leaked[i])));

    /* Legit noun-phrase topics containing none of the new stop words must
     * keep passing — the additions must not regress recall. */
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("happy hour", 10));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("press release", 13));
}

/* End-to-end: decide() must drop obligations whose topic fails the predicate.
 * This inbound is the EXACT production message that generated the mangled
 * "how'd the It will be tomorrow. Im working go?" send on 2026-07-16. */
static void decide_sentence_like_description_no_obligation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_proactive_result_t res;
    static const char inbound[] = "It will be tomorrow. Im working today. ";
    HU_ASSERT_EQ(hu_contextual_proactive_decide(&alloc, inbound, sizeof(inbound) - 1, CP_NOW, &res),
                 HU_OK);
    /* Every extracted candidate is clause-like; none may survive the gate. */
    for (size_t i = 0; i < res.count; i++)
        HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable(res.items[i].topic,
                                                                 strlen(res.items[i].topic)));
    HU_ASSERT_EQ(res.count, (size_t)0);
}

/* ── temporal resolution: future, never past ──────────────────────────────── */

static void resolve_tomorrow_is_future_evening(void) {
    int64_t send = hu_contextual_proactive_resolve_send_at("tomorrow", 8, CP_NOW);
    HU_ASSERT_TRUE(send > CP_NOW);
    HU_ASSERT_TRUE(send <= CP_NOW + 2 * 86400LL);
    struct tm s = local_of(send);
    HU_ASSERT_EQ(s.tm_hour, HU_CONTEXTUAL_PROACTIVE_SEND_HOUR);
}

static void resolve_yesterday_and_vague_reject(void) {
    HU_ASSERT_EQ(hu_contextual_proactive_resolve_send_at("yesterday", 9, CP_NOW), (int64_t)0);
    HU_ASSERT_EQ(hu_contextual_proactive_resolve_send_at("this week", 9, CP_NOW), (int64_t)0);
    HU_ASSERT_EQ(hu_contextual_proactive_resolve_send_at("next month", 10, CP_NOW), (int64_t)0);
    HU_ASSERT_EQ(hu_contextual_proactive_resolve_send_at("last week", 9, CP_NOW), (int64_t)0);
}

static void resolve_weekday_lands_on_that_weekday(void) {
    /* "Friday" must resolve to the next Friday (wday==5) at SEND_HOUR, in the
     * future, within a week. TZ-independent: derive the weekday from the result. */
    int64_t send = hu_contextual_proactive_resolve_send_at("Friday", 6, CP_NOW);
    HU_ASSERT_TRUE(send > CP_NOW);
    HU_ASSERT_TRUE(send <= CP_NOW + 7 * 86400LL);
    struct tm s = local_of(send);
    HU_ASSERT_EQ(s.tm_wday, 5);
    HU_ASSERT_EQ(s.tm_hour, HU_CONTEXTUAL_PROACTIVE_SEND_HOUR);
}

static void resolve_next_weekday_is_following_week(void) {
    int64_t plain = hu_contextual_proactive_resolve_send_at("Monday", 6, CP_NOW);
    int64_t nxt = hu_contextual_proactive_resolve_send_at("next Monday", 11, CP_NOW);
    HU_ASSERT_TRUE(plain > CP_NOW);
    HU_ASSERT_TRUE(nxt > plain); /* "next Monday" is strictly later than the nearest Monday */
    struct tm s = local_of(nxt);
    HU_ASSERT_EQ(s.tm_wday, 1);
}

static void resolve_today_never_past(void) {
    /* "today" resolves to today's SEND_HOUR only if that is still in the
     * future; otherwise 0. It must never produce a past timestamp. */
    int64_t send = hu_contextual_proactive_resolve_send_at("today", 5, CP_NOW);
    HU_ASSERT_TRUE(send == 0 || send > CP_NOW);
}

static void resolve_in_n_days(void) {
    int64_t send = hu_contextual_proactive_resolve_send_at("in 3 days", 9, CP_NOW);
    HU_ASSERT_TRUE(send > CP_NOW + 2 * 86400LL);
    HU_ASSERT_TRUE(send <= CP_NOW + 4 * 86400LL);
    struct tm s = local_of(send);
    HU_ASSERT_EQ(s.tm_hour, HU_CONTEXTUAL_PROACTIVE_SEND_HOUR);
}

/* ── end-to-end detector: dated event message -> stored obligation ────────── */

static void decide_dated_event_stores_contextual_obligation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_proactive_result_t res;
    const char *msg = "my interview is on Friday";
    hu_error_t err = hu_contextual_proactive_decide(&alloc, msg, strlen(msg), CP_NOW, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)1);

    /* Topic is the REAL detected topic, not invented. */
    HU_ASSERT_TRUE(strstr(res.items[0].topic, "interview") != NULL);
    /* Frozen message references the stored topic. */
    HU_ASSERT_TRUE(strstr(res.items[0].message, "interview") != NULL);
    HU_ASSERT_TRUE(strstr(res.items[0].message, "how'd") != NULL);

    /* Fires in the future, after the event, on a Friday evening. */
    int64_t send_ms = res.items[0].send_at_ms;
    HU_ASSERT_TRUE(send_ms > CP_NOW * 1000);
    struct tm s = local_of(send_ms / 1000);
    HU_ASSERT_EQ(s.tm_wday, 5);
    HU_ASSERT_EQ(s.tm_hour, HU_CONTEXTUAL_PROACTIVE_SEND_HOUR);
    HU_ASSERT_TRUE(res.items[0].confidence >= HU_CONTEXTUAL_PROACTIVE_MIN_CONFIDENCE);
}

static void decide_no_temporal_no_obligation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_proactive_result_t res;
    /* No temporal reference at all -> no future-dated event -> no obligation. */
    const char *msg = "i really like coffee and the weather is nice";
    hu_error_t err = hu_contextual_proactive_decide(&alloc, msg, strlen(msg), CP_NOW, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)0);
}

static void decide_past_event_no_obligation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_proactive_result_t res;
    /* A detectable event but a PAST temporal ref must not schedule outreach. */
    const char *msg = "my interview was yesterday";
    hu_error_t err = hu_contextual_proactive_decide(&alloc, msg, strlen(msg), CP_NOW, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)0);
}

static void shadow_summary_captures_distribution(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_proactive_result_t res;
    const char *msg = "my interview is on Friday";
    HU_ASSERT_EQ(hu_contextual_proactive_decide(&alloc, msg, strlen(msg), CP_NOW, &res), HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)1);

    char metric[512];
    size_t n = hu_contextual_proactive_shadow_summary(&res, "user_a", metric, sizeof(metric));
    HU_ASSERT_TRUE(n > 0);
    /* The metric is the SHADOW evidence artifact: it must carry the contact, the
     * decision count, and the real topic + confidence — non-vacuously. */
    HU_ASSERT_TRUE(strstr(metric, "contextual_proactive(shadow)") != NULL);
    HU_ASSERT_TRUE(strstr(metric, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(metric, "decided 1") != NULL);
    HU_ASSERT_TRUE(strstr(metric, "interview") != NULL);
    HU_ASSERT_TRUE(strstr(metric, "0.") != NULL); /* confidence rendered */
}

static void shadow_summary_empty_result_is_zero(void) {
    hu_contextual_proactive_result_t res;
    memset(&res, 0, sizeof(res));
    char metric[64];
    HU_ASSERT_EQ(hu_contextual_proactive_shadow_summary(&res, "user_a", metric, sizeof(metric)),
                 (size_t)0);
    HU_ASSERT_EQ(metric[0], '\0');
}

static void mode_str_labels(void) {
    HU_ASSERT_STR_EQ(hu_contextual_proactive_mode_str(HU_CONTEXTUAL_PROACTIVE_OFF), "off");
    HU_ASSERT_STR_EQ(hu_contextual_proactive_mode_str(HU_CONTEXTUAL_PROACTIVE_SHADOW), "shadow");
    HU_ASSERT_STR_EQ(hu_contextual_proactive_mode_str(HU_CONTEXTUAL_PROACTIVE_ON), "on");
}

static void decide_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_proactive_result_t res;
    HU_ASSERT_EQ(hu_contextual_proactive_decide(NULL, "x", 1, CP_NOW, &res),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_contextual_proactive_decide(&alloc, "x", 1, CP_NOW, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL text is a no-op, not an error. */
    HU_ASSERT_EQ(hu_contextual_proactive_decide(&alloc, NULL, 0, CP_NOW, &res), HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)0);
}

/* ── Contractions must not evade the clause-word blocklist ──────────────
 * 2026-07-26 15:05 the daemon sent Seth's SISTER:
 *   "hey how are you doing with don't understand provide?"
 * She replied "Turn AI off".
 *
 * Every contraction in topic_clause_words is spelled apostrophe-less
 * ("dont","cant","wont","hes","shes","theyre") but the word-boundary scan
 * treats an apostrophe AS a boundary, so real "don't" tokenized to
 * "don" + "t" and matched neither entry. Those entries only ever fired when
 * the bare prefix was independently listed ("I'm" via "i", "what's" via
 * "what", "it's" via "it"); don't / won't / he's / she's / they're had no
 * backstop and sailed through. */
static void topic_contractions_are_blocked(void) {
    /* THE incident string. */
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("don't understand provide", 24));
    /* The apostrophe-less spelling was always blocked — the pair is the bug. */
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("dont understand provide", 23));
    /* The family that had no bare-prefix backstop. */
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("won't work", 10));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("he's late", 9));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("she's here", 10));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("they're coming", 14));
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable("can't make it", 13));
}

static void topic_curly_apostrophe_is_blocked(void) {
    /* iMessage autocorrects to U+2019, so the ASCII-only fold would miss it. */
    const char *curly = "don\xe2\x80\x99t understand provide";
    HU_ASSERT_FALSE(hu_contextual_proactive_topic_is_sendable(curly, strlen(curly)));
}

static void topic_real_events_still_send_after_the_fold(void) {
    /* The fix must not over-block: these are the whole point of the feature. */
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("interview", 9));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("dentist appointment", 19));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("internet installers", 19));
    HU_ASSERT_TRUE(hu_contextual_proactive_topic_is_sendable("parent teacher conference", 25));
}

void run_contextual_proactive_tests(void) {
    HU_TEST_SUITE("contextual_proactive");
    HU_RUN_TEST(mode_parse_defaults_off);
    HU_RUN_TEST(mode_parse_shadow_and_on);
    HU_RUN_TEST(build_message_references_topic);
    HU_RUN_TEST(build_message_empty_topic_no_fabrication);
    HU_RUN_TEST(normalize_topic_strips_filler);
    HU_RUN_TEST(topic_sendable_accepts_noun_phrases);
    HU_RUN_TEST(topic_sendable_rejects_sentence_fragments);
    HU_RUN_TEST(topic_sendable_rejects_question_words_and_discourse_markers);
    HU_RUN_TEST(decide_sentence_like_description_no_obligation);
    HU_RUN_TEST(resolve_tomorrow_is_future_evening);
    HU_RUN_TEST(resolve_yesterday_and_vague_reject);
    HU_RUN_TEST(resolve_weekday_lands_on_that_weekday);
    HU_RUN_TEST(resolve_next_weekday_is_following_week);
    HU_RUN_TEST(resolve_today_never_past);
    HU_RUN_TEST(resolve_in_n_days);
    HU_RUN_TEST(decide_dated_event_stores_contextual_obligation);
    HU_RUN_TEST(decide_no_temporal_no_obligation);
    HU_RUN_TEST(decide_past_event_no_obligation);
    HU_RUN_TEST(shadow_summary_captures_distribution);
    HU_RUN_TEST(shadow_summary_empty_result_is_zero);
    HU_RUN_TEST(mode_str_labels);
    HU_RUN_TEST(decide_null_args);
    HU_RUN_TEST(topic_contractions_are_blocked);
    HU_RUN_TEST(topic_curly_apostrophe_is_blocked);
    HU_RUN_TEST(topic_real_events_still_send_after_the_fold);
}
