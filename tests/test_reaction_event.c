/* tests/test_reaction_event.c */
#include "test_framework.h"
#include "human/channels/reaction_event.h"
#include <string.h>

static void test_reaction_event_imessage_tapback_2000_is_love(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(2000, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_LOVE);
    HU_ASSERT_EQ(pol, HU_REACTION_POSITIVE);
}
/* AUTHORITATIVE source for the full code set is the actual switch in
 * src/channels/imessage.c:1812-1832 (handles 2000-2006) plus the positive-set
 * filter at imessage.c:1890. The comment block at imessage.c:1017 is stale
 * (omits 2006). Tests below pin every code: 2000=love, 2001=like,
 * 2002=dislike, 2003=laugh, 2004=emphasis, 2005=question, 2006=custom_emoji.
 * v1 of this plan mapped 2003→DISLIKE — that would have trained negative DPO
 * signals on every laugh tapback. Fixed in v2 + 2006 added in v3. */
static void test_reaction_event_imessage_tapback_2002_is_dislike(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(2002, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_DISLIKE);
    HU_ASSERT_EQ(pol, HU_REACTION_NEGATIVE);
}
static void test_reaction_event_imessage_tapback_2003_is_laugh(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(2003, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_LAUGH);
    HU_ASSERT_EQ(pol, HU_REACTION_POSITIVE);
}
static void test_reaction_event_imessage_tapback_2004_is_emphasize(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(2004, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_EMPHASIZE);
    HU_ASSERT_EQ(pol, HU_REACTION_POSITIVE);
}
static void test_reaction_event_imessage_tapback_2005_is_question(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(2005, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_QUESTION);
    HU_ASSERT_EQ(pol, HU_REACTION_NEUTRAL);
}
/* Code 2006 = custom emoji / Apple Sticker tapback (added in macOS 14+).
 * Live in src/channels/imessage.c at line 1830 (switch) and line 1890
 * (positive set). v1 of this plan claimed 2006 doesn't exist — wrong. */
static void test_reaction_event_imessage_tapback_2006_is_custom_emoji(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(2006, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_CUSTOM_EMOJI);
    HU_ASSERT_EQ(pol, HU_REACTION_POSITIVE);
}
/* Removal codes: 3000-3006 (offset +1000). 3003 should still resolve to LAUGH. */
static void test_reaction_event_imessage_removal_3003_is_laugh(void) {
    hu_reaction_kind_t kind = HU_REACTION_UNKNOWN;
    hu_reaction_polarity_t pol = HU_REACTION_NEUTRAL;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(3003, &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_LAUGH);
}
static void test_reaction_event_slack_thumbsup_is_like_positive(void) {
    hu_reaction_kind_t kind; hu_reaction_polarity_t pol;
    HU_ASSERT_EQ(hu_reaction_normalize_slack("+1", &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_LIKE);
    HU_ASSERT_EQ(pol, HU_REACTION_POSITIVE);
}
static void test_reaction_event_slack_thumbsdown_is_dislike_negative(void) {
    hu_reaction_kind_t kind; hu_reaction_polarity_t pol;
    HU_ASSERT_EQ(hu_reaction_normalize_slack("-1", &kind, &pol), HU_OK);
    HU_ASSERT_EQ(kind, HU_REACTION_DISLIKE);
    HU_ASSERT_EQ(pol, HU_REACTION_NEGATIVE);
}
static void test_reaction_event_unknown_imessage_code_returns_error(void) {
    hu_reaction_kind_t kind; hu_reaction_polarity_t pol;
    HU_ASSERT_EQ(hu_reaction_normalize_imessage(9999, &kind, &pol), HU_ERR_INVALID_ARGUMENT);
}

void run_reaction_event_tests(void) {
    HU_RUN_TEST(test_reaction_event_imessage_tapback_2000_is_love);
    HU_RUN_TEST(test_reaction_event_imessage_tapback_2002_is_dislike);
    HU_RUN_TEST(test_reaction_event_imessage_tapback_2003_is_laugh);
    HU_RUN_TEST(test_reaction_event_imessage_tapback_2004_is_emphasize);
    HU_RUN_TEST(test_reaction_event_imessage_tapback_2005_is_question);
    HU_RUN_TEST(test_reaction_event_imessage_tapback_2006_is_custom_emoji);
    HU_RUN_TEST(test_reaction_event_imessage_removal_3003_is_laugh);
    HU_RUN_TEST(test_reaction_event_slack_thumbsup_is_like_positive);
    HU_RUN_TEST(test_reaction_event_slack_thumbsdown_is_dislike_negative);
    HU_RUN_TEST(test_reaction_event_unknown_imessage_code_returns_error);
}
