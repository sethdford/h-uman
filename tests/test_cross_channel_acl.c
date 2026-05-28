#ifdef HU_ENABLE_SQLITE

#include "human/memory/cross_channel.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

/* AC-1: THE TRUST PROPERTY.
 *
 * A family-relationship-type fact MUST NEVER reach a coworker turn.
 * This is the highest-priority test in this entire sprint. If this
 * fails, the privacy property is broken.
 *
 * Per tests-that-pin-bugs.md: the test name asserts the DANGEROUS case
 * is BLOCKED, not that some score landed in a band. */
static void test_family_fact_never_reaches_coworker_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona); /* loads safe-default ACL */

    /* Pre-built item with family origin */
    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_001",
        .text = (char *)"wife mentioned needing new running shoes",
        .text_len = strlen("wife mentioned needing new running shoes"),
        .origin_channel = "imessage",
        .origin_contact_id = "contact_wife",
        .origin_relationship_type = "family",
        .observed_at_ms = 1000,
        .confidence = 0.9,
    }};
    size_t count = 1;

    /* Apply filter for a coworker turn */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "coworker", items, &count), HU_OK);

    /* The family fact MUST be filtered out */
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

static void test_partner_fact_never_reaches_coworker_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_002",
        .text = (char *)"partner wants to move to Portland",
        .text_len = strlen("partner wants to move to Portland"),
        .origin_channel = "telegram",
        .origin_contact_id = "contact_partner",
        .origin_relationship_type = "partner",
        .observed_at_ms = 2000,
        .confidence = 0.95,
    }};
    size_t count = 1;

    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "coworker", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

static void test_acl_same_relationship_always_allowed(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_003",
        .text = (char *)"family event next weekend",
        .text_len = strlen("family event next weekend"),
        .origin_channel = "imessage",
        .origin_contact_id = "contact_mother",
        .origin_relationship_type = "family",
        .observed_at_ms = 3000,
        .confidence = 0.88,
    }};
    size_t count = 1;

    /* family origin + family turn → kept */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "family", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 1);
    hu_persona_free(&persona);
}

static void test_acl_deny_unknown_blocks_null_relationship(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_004",
        .text = (char *)"some random fact",
        .text_len = strlen("some random fact"),
        .origin_channel = "slack",
        .origin_contact_id = "contact_unknown",
        .origin_relationship_type = "acquaintance",
        .observed_at_ms = 4000,
        .confidence = 0.5,
    }};
    size_t count = 1;

    /* turn_relationship_type = NULL → deny under default policy */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, NULL, items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

/* Additional allowed pairs (AC-3/AC-4 positive cases) */
static void test_acl_friend_fact_reaches_family_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_005",
        .text = (char *)"friend recommended a restaurant",
        .text_len = strlen("friend recommended a restaurant"),
        .origin_channel = "discord",
        .origin_contact_id = "contact_friend",
        .origin_relationship_type = "friend",
        .observed_at_ms = 5000,
        .confidence = 0.92,
    }};
    size_t count = 1;

    /* friend origin + family turn → friend in family's allow_list? No, denied */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "family", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

static void test_acl_close_friend_fact_reaches_family_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_006",
        .text = (char *)"close friend is getting married",
        .text_len = strlen("close friend is getting married"),
        .origin_channel = "imessage",
        .origin_contact_id = "contact_close_friend",
        .origin_relationship_type = "close_friend",
        .observed_at_ms = 6000,
        .confidence = 0.85,
    }};
    size_t count = 1;

    /* close_friend origin + family turn → close_friend in family's allow_list? Yes */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "family", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 1);
    hu_persona_free(&persona);
}

/* Sensitive denied pairs (AC-2: privacy-violating combinations) */
static void test_acl_family_fact_never_reaches_work_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_007",
        .text = (char *)"health issue discussed with partner",
        .text_len = strlen("health issue discussed with partner"),
        .origin_channel = "imessage",
        .origin_contact_id = "contact_family",
        .origin_relationship_type = "family",
        .observed_at_ms = 7000,
        .confidence = 0.98,
    }};
    size_t count = 1;

    /* family origin + work turn → MUST be denied */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "work", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

static void test_acl_partner_fact_never_reaches_acquaintance_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_008",
        .text = (char *)"partner and I planning a trip",
        .text_len = strlen("partner and I planning a trip"),
        .origin_channel = "telegram",
        .origin_contact_id = "contact_partner",
        .origin_relationship_type = "partner",
        .observed_at_ms = 8000,
        .confidence = 0.96,
    }};
    size_t count = 1;

    /* partner origin + acquaintance turn → MUST be denied */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "acquaintance", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

/* Filter boundary cases */
static void test_acl_filter_empty_items_array(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[1];
    size_t count = 0;

    /* empty array → count stays 0, no crash */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "family", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

static void test_acl_filter_all_items_filtered(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);

    hu_cross_channel_item_t items[] = {
        {
            .source_type = HU_XCHAN_FACT,
            .item_id = "fact_009",
            .text = (char *)"family secret",
            .text_len = strlen("family secret"),
            .origin_channel = "imessage",
            .origin_contact_id = "contact_family",
            .origin_relationship_type = "family",
            .observed_at_ms = 9000,
            .confidence = 0.9,
        },
        {
            .source_type = HU_XCHAN_FACT,
            .item_id = "fact_010",
            .text = (char *)"partner secret",
            .text_len = strlen("partner secret"),
            .origin_channel = "imessage",
            .origin_contact_id = "contact_partner",
            .origin_relationship_type = "partner",
            .observed_at_ms = 9001,
            .confidence = 0.92,
        },
    };
    size_t count = 2;

    /* both items denied for coworker turn → count becomes 0 */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "coworker", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

/* Malformed/empty ACL case (AC-5: fail-closed) */
static void test_acl_empty_acl_denies_everything(void) {
    hu_persona_t persona = {0};
    memset(&persona, 0, sizeof(persona));
    /* Manually construct an empty ACL (no rules) */
    persona.cross_channel_acl.rule_count = 0;
    persona.cross_channel_acl.rules = NULL;

    hu_cross_channel_item_t items[] = {{
        .source_type = HU_XCHAN_FACT,
        .item_id = "fact_011",
        .text = (char *)"any fact",
        .text_len = strlen("any fact"),
        .origin_channel = "slack",
        .origin_contact_id = "contact_any",
        .origin_relationship_type = "acquaintance",
        .observed_at_ms = 10000,
        .confidence = 0.8,
    }};
    size_t count = 1;

    /* empty ACL → deny everything (AC-5: fail-closed) */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "coworker", items, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_persona_free(&persona);
}

void run_cross_channel_acl_tests(void) {
    HU_TEST_SUITE("cross_channel_acl");
    HU_RUN_TEST(test_family_fact_never_reaches_coworker_turn);
    HU_RUN_TEST(test_partner_fact_never_reaches_coworker_turn);
    HU_RUN_TEST(test_acl_same_relationship_always_allowed);
    HU_RUN_TEST(test_acl_deny_unknown_blocks_null_relationship);
    HU_RUN_TEST(test_acl_friend_fact_reaches_family_turn);
    HU_RUN_TEST(test_acl_close_friend_fact_reaches_family_turn);
    HU_RUN_TEST(test_acl_family_fact_never_reaches_work_turn);
    HU_RUN_TEST(test_acl_partner_fact_never_reaches_acquaintance_turn);
    HU_RUN_TEST(test_acl_filter_empty_items_array);
    HU_RUN_TEST(test_acl_filter_all_items_filtered);
    HU_RUN_TEST(test_acl_empty_acl_denies_everything);
}

#else

void run_cross_channel_acl_tests(void) {
    (void)0;
}

#endif
