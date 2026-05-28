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

void run_cross_channel_acl_tests(void) {
    HU_TEST_SUITE("cross_channel_acl");
    HU_RUN_TEST(test_family_fact_never_reaches_coworker_turn);
    HU_RUN_TEST(test_partner_fact_never_reaches_coworker_turn);
    HU_RUN_TEST(test_acl_same_relationship_always_allowed);
    HU_RUN_TEST(test_acl_deny_unknown_blocks_null_relationship);
}

#else

void run_cross_channel_acl_tests(void) {
    (void)0;
}

#endif
