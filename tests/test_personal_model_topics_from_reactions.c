/* Tests for hu_personal_model_bump_topics_from_reaction.
 *
 * The reaction-derived topic bumper bridges the reaction-facts pipeline
 * to the existing topic-salience system: a positive reaction on a
 * target message bubbles the message's content words into
 * `model->topics[]` so the topics the user reacts to (not just the
 * topics they type about) feed prompt enrichment.
 *
 * These tests pin:
 *   1. Single positive reaction inserts new topic slots.
 *   2. Repeated positive reactions monotonically bump mention_count
 *      AND interest_score.
 *   3. Stopwords are filtered (function words never land in topics).
 *   4. Sub-min-length tokens (< 4 chars) are filtered.
 *   5. Negative reactions DECREMENT (don't bump) and don't materialize
 *      new low-salience slots on previously-unseen tokens.
 *   6. NULL/empty target text is a no-op returning 0.
 *   7. Topic-slot exhaustion uses LRU eviction (via bump_topic), no crash.
 *   8. Integration: hu_reaction_ingest_personal_model produces facts AND
 *      topics for the same event.
 */

#include "human/channels/reaction_event.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"

#include <string.h>

/* Forward decl from src/channels/imessage_ingest.c — wired separately. */
extern hu_error_t hu_reaction_ingest_personal_model(struct hu_personal_model *model,
                                                    const hu_reaction_event_t *event,
                                                    const char *custom_emoji,
                                                    const char *target_text_preview,
                                                    bool is_from_me_target, bool in_group_chat);

/* Helper: minimal LOVE reaction event with a configurable target text
 * preview. Sender / timestamps are fixed so tests are deterministic. */
static void make_love_event(hu_reaction_event_t *e) {
    memset(e, 0, sizeof(*e));
    e->channel_id = "imessage";
    e->target_thread_id = "iMessage;-;+15551234567";
    e->target_message_ref = "OUR-MSG-001";
    e->sender_handle = "+15551234567";
    e->kind = HU_REACTION_LOVE;
    e->polarity = HU_REACTION_POSITIVE;
    e->timestamp_unix = 1700000000;
    e->is_removal = 0;
}

static void make_dislike_event(hu_reaction_event_t *e) {
    make_love_event(e);
    e->kind = HU_REACTION_DISLIKE;
    e->polarity = HU_REACTION_NEGATIVE;
}

/* Find a topic slot by case-insensitive name. Returns NULL when missing. */
static const hu_personal_topic_t *find_topic(const hu_personal_model_t *m, const char *name) {
    for (size_t i = 0; i < m->topic_count; i++) {
        if (strcasecmp(m->topics[i].name, name) == 0)
            return &m->topics[i];
    }
    return NULL;
}

/* 1. Single love reaction on "let's hike Mount Tam Saturday" should
 *    produce topic slots for content tokens ("hike", "mount", "saturday")
 *    while filtering "let's" (stopword) and "tam" (too short — 3 chars). */
static void single_love_reaction_inserts_content_topics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_reaction_event_t e;
    make_love_event(&e);

    size_t touched = hu_personal_model_bump_topics_from_reaction(
        &m, &e, "let's hike Mount Tam Saturday", e.timestamp_unix);
    /* hike, mount, saturday — three distinct content words.
     * "let's" is a stopword, "tam" is < 4 chars. */
    HU_ASSERT_EQ(touched, 3U);

    HU_ASSERT_TRUE(find_topic(&m, "hike") != NULL);
    HU_ASSERT_TRUE(find_topic(&m, "mount") != NULL);
    HU_ASSERT_TRUE(find_topic(&m, "saturday") != NULL);
    HU_ASSERT_TRUE(find_topic(&m, "let's") == NULL);
    HU_ASSERT_TRUE(find_topic(&m, "lets") == NULL);
    HU_ASSERT_TRUE(find_topic(&m, "tam") == NULL);

    const hu_personal_topic_t *hike = find_topic(&m, "hike");
    HU_ASSERT_EQ(hike->mention_count, 1U);
    HU_ASSERT_EQ(hike->last_mentioned, (int64_t)1700000000);
}

/* 2. Three positive reactions on hiking-related text → "hike"
 *    mention_count grows monotonically and interest_score stays
 *    bounded but rises. */
static void repeated_positive_reactions_increase_mention_count(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_reaction_event_t e;
    make_love_event(&e);

    hu_personal_model_bump_topics_from_reaction(&m, &e, "love hiking trail", e.timestamp_unix);
    hu_personal_model_bump_topics_from_reaction(&m, &e, "another hiking trip",
                                                e.timestamp_unix + 60);
    hu_personal_model_bump_topics_from_reaction(&m, &e, "hiking again Saturday",
                                                e.timestamp_unix + 120);

    const hu_personal_topic_t *hiking = find_topic(&m, "hiking");
    HU_ASSERT_TRUE(hiking != NULL);
    HU_ASSERT_EQ(hiking->mention_count, 3U);
    /* Initial 0.3 + 2 bumps of 0.05 each = 0.4. The first call adds the
     * slot at 0.3 (no extra bump); subsequent calls bump by 0.05. */
    HU_ASSERT_TRUE(hiking->interest_score >= 0.39f);
    HU_ASSERT_TRUE(hiking->interest_score <= 0.41f);
    HU_ASSERT_EQ(hiking->last_mentioned, (int64_t)1700000120);
}

/* 3. Stopwords are filtered: a target text composed entirely of
 *    function words and short tokens produces NO topics. */
static void stopwords_and_short_tokens_filtered(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_reaction_event_t e;
    make_love_event(&e);

    size_t touched = hu_personal_model_bump_topics_from_reaction(
        &m, &e, "the and but for with this that you me", e.timestamp_unix);
    HU_ASSERT_EQ(touched, 0U);
    HU_ASSERT_EQ(m.topic_count, (size_t)0);
}

/* 4. Sub-min-length filter: 3-char and shorter tokens never reach
 *    the bumper, even if they aren't in the stopword list. */
static void short_words_filtered(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_reaction_event_t e;
    make_love_event(&e);

    /* "tea", "cat", "bay" are 3 chars — under HU_PM_REACTION_TOPIC_MIN_LEN. */
    size_t touched =
        hu_personal_model_bump_topics_from_reaction(&m, &e, "tea cat bay", e.timestamp_unix);
    HU_ASSERT_EQ(touched, 0U);
    HU_ASSERT_EQ(m.topic_count, (size_t)0);
}

/* 5. Negative reaction on an UNSEEN topic does not materialize a slot;
 *    negative reaction on a KNOWN topic decrements its salience. */
static void negative_reaction_decrements_known_does_not_add_unknown(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    /* First: bump "meetings" with two positive reactions. */
    hu_reaction_event_t pos;
    make_love_event(&pos);
    hu_personal_model_bump_topics_from_reaction(&m, &pos, "endless meetings", pos.timestamp_unix);
    hu_personal_model_bump_topics_from_reaction(&m, &pos, "boring meetings again",
                                                pos.timestamp_unix + 60);
    const hu_personal_topic_t *meet = find_topic(&m, "meetings");
    HU_ASSERT_TRUE(meet != NULL);
    HU_ASSERT_EQ(meet->mention_count, 2U);
    float meet_score_before = meet->interest_score;

    /* Now: a dislike reaction on "meetings tomorrow gardening" should
     * decrement "meetings" (known) but NOT add "gardening" (unknown). */
    hu_reaction_event_t neg;
    make_dislike_event(&neg);
    size_t pre_topic_count = m.topic_count;
    size_t touched = hu_personal_model_bump_topics_from_reaction(
        &m, &neg, "meetings tomorrow gardening", neg.timestamp_unix + 120);

    /* Touched count: "meetings" (decremented), "tomorrow" + "gardening"
     * were unseen so they don't count. So touched == 1. */
    HU_ASSERT_EQ(touched, 1U);

    /* meetings mention_count decremented from 2 → 1. */
    meet = find_topic(&m, "meetings");
    HU_ASSERT_TRUE(meet != NULL);
    HU_ASSERT_EQ(meet->mention_count, 1U);
    HU_ASSERT_TRUE(meet->interest_score < meet_score_before);

    /* "gardening" must not have been added. */
    HU_ASSERT_TRUE(find_topic(&m, "gardening") == NULL);
    HU_ASSERT_TRUE(find_topic(&m, "tomorrow") == NULL);
    /* topic_count is unchanged (no slot added or removed). */
    HU_ASSERT_EQ(m.topic_count, pre_topic_count);
}

/* 6. NULL / empty / NULL-event / removal-event paths all return 0
 *    and leave the model untouched. */
static void null_and_no_op_paths(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_reaction_event_t e;
    make_love_event(&e);

    HU_ASSERT_EQ(hu_personal_model_bump_topics_from_reaction(NULL, &e, "hike", 0), 0U);
    HU_ASSERT_EQ(hu_personal_model_bump_topics_from_reaction(&m, NULL, "hike", 0), 0U);
    HU_ASSERT_EQ(hu_personal_model_bump_topics_from_reaction(&m, &e, NULL, e.timestamp_unix), 0U);
    HU_ASSERT_EQ(hu_personal_model_bump_topics_from_reaction(&m, &e, "", e.timestamp_unix), 0U);

    /* Removal events are no-ops. */
    e.is_removal = 1;
    HU_ASSERT_EQ(
        hu_personal_model_bump_topics_from_reaction(&m, &e, "hiking trail", e.timestamp_unix), 0U);
    e.is_removal = 0;

    /* Neutral polarity (QUESTION-style) is a no-op. */
    e.polarity = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(
        hu_personal_model_bump_topics_from_reaction(&m, &e, "hiking trail", e.timestamp_unix), 0U);

    HU_ASSERT_EQ(m.topic_count, (size_t)0);
}

/* 7. Topic-slot exhaustion: keep flooding topics; the function MUST
 *    NOT crash and topic_count stays bounded by HU_PM_MAX_TOPICS. */
static void topic_slot_exhaustion_does_not_crash(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_reaction_event_t e;
    make_love_event(&e);

    /* Each token in this preview is >= 4 chars, lowercase-already,
     * stopword-free. That's 18 distinct topics, more than HU_PM_MAX_TOPICS. */
    const char *flood =
        "apples bananas cherries durian elderberry figs grapes honeydew kiwi lemons "
        "mangoes nectarines oranges papayas quince raspberries strawberries tangerines";

    /* The bumper's LRU eviction path runs inside bump_topic; the return
     * value reflects the number of distinct content tokens we walked. */
    size_t touched = hu_personal_model_bump_topics_from_reaction(&m, &e, flood, e.timestamp_unix);
    HU_ASSERT_TRUE(touched >= HU_PM_MAX_TOPICS);
    /* topic_count cannot exceed the capacity. */
    HU_ASSERT_TRUE(m.topic_count <= (size_t)HU_PM_MAX_TOPICS);
    /* The LAST few tokens in `flood` should survive in the array
     * (LRU evicts the earlier ones). "tangerines" was last and must
     * be present. */
    HU_ASSERT_TRUE(find_topic(&m, "tangerines") != NULL);
}

/* 8. Integration: the public reaction-ingest entry point must produce
 *    BOTH a fact and at least one topic for a positive reaction. */
static void integration_reaction_ingest_produces_both_facts_and_topics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ(m.fact_count, (size_t)0);
    HU_ASSERT_EQ(m.topic_count, (size_t)0);

    hu_reaction_event_t e;
    make_love_event(&e);

    hu_error_t rc = hu_reaction_ingest_personal_model(&m, &e,
                                                      /*custom_emoji=*/NULL,
                                                      /*target_text_preview=*/"hiking trail",
                                                      /*is_from_me_target=*/true,
                                                      /*in_group_chat=*/false);
    HU_ASSERT_EQ(rc, HU_OK);

    /* Fact path: at least one fact recorded with sender as subject. */
    HU_ASSERT_TRUE(m.fact_count >= 1);
    /* Topic path: "hiking" and "trail" both bubbled up. */
    HU_ASSERT_TRUE(find_topic(&m, "hiking") != NULL);
    HU_ASSERT_TRUE(find_topic(&m, "trail") != NULL);
}

void run_personal_model_topics_from_reactions_tests(void);
void run_personal_model_topics_from_reactions_tests(void) {
    HU_TEST_SUITE("personal_model_topics_from_reactions");
    HU_RUN_TEST(single_love_reaction_inserts_content_topics);
    HU_RUN_TEST(repeated_positive_reactions_increase_mention_count);
    HU_RUN_TEST(stopwords_and_short_tokens_filtered);
    HU_RUN_TEST(short_words_filtered);
    HU_RUN_TEST(negative_reaction_decrements_known_does_not_add_unknown);
    HU_RUN_TEST(null_and_no_op_paths);
    HU_RUN_TEST(topic_slot_exhaustion_does_not_crash);
    HU_RUN_TEST(integration_reaction_ingest_produces_both_facts_and_topics);
}
