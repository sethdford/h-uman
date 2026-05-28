#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "test_framework.h"
#include <assert.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Test: style selection NULL → byte-identical to today (AC-8)
 * ────────────────────────────────────────────────────────────────────────── */

static void test_examples_null_style_matches_topic_only(void) {
    hu_allocator_t sys_alloc = hu_system_allocator();
    hu_allocator_t *alloc = &sys_alloc;
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    /* Create example bank with 3 examples */
    hu_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    bank.channel = hu_strdup(alloc, "telegram");

    hu_persona_example_t examples[3];
    memset(examples, 0, sizeof(examples));

    examples[0].context = hu_strdup(alloc, "chatting about work");
    examples[0].incoming = hu_strdup(alloc, "how's the project?");
    examples[0].response = hu_strdup(alloc, "Going well, almost done.");

    examples[1].context = hu_strdup(alloc, "chatting about food");
    examples[1].incoming = hu_strdup(alloc, "what's for dinner?");
    examples[1].response = hu_strdup(alloc, "Pizza sounds good!");

    examples[2].context = hu_strdup(alloc, "chatting about work");
    examples[2].incoming = hu_strdup(alloc, "any updates?");
    examples[2].response = hu_strdup(alloc, "Finished the report.");

    bank.examples = examples;
    bank.examples_count = 3;

    persona.example_banks = &bank;
    persona.example_banks_count = 1;

    /* Select with NULL style (original behavior) */
    const hu_persona_example_t *out_null[5];
    size_t count_null = 0;
    hu_error_t err = hu_persona_select_examples(&persona, "telegram", 8, "work", 4, out_null,
                                                &count_null, 5, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    /* Selector returns top-N (min(max_examples, bank size)) sorted by score,
     * NOT only the matching subset. With 3 examples and capacity 5, all 3 come
     * back: the two "work" matches first (score 1), the "food" example last
     * (score 0). This is today's behavior — AC-8 requires NULL style preserve it. */
    HU_ASSERT_EQ(count_null, 3);
    HU_ASSERT_EQ(out_null[0], &examples[0]);
    HU_ASSERT_EQ(out_null[1], &examples[2]);
    HU_ASSERT_EQ(out_null[2], &examples[1]);

    /* Clean up */
    for (int i = 0; i < 3; i++) {
        alloc->free(alloc->ctx, examples[i].context, strlen(examples[i].context) + 1);
        alloc->free(alloc->ctx, examples[i].incoming, strlen(examples[i].incoming) + 1);
        alloc->free(alloc->ctx, examples[i].response, strlen(examples[i].response) + 1);
    }
    alloc->free(alloc->ctx, bank.channel, strlen(bank.channel) + 1);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Test: two candidates equal on topic, style wins (AC-8)
 * ────────────────────────────────────────────────────────────────────────── */

static void test_examples_style_prefers_fingerprint_match(void) {
    hu_allocator_t sys_alloc = hu_system_allocator();
    hu_allocator_t *alloc = &sys_alloc;
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    /* Create example bank with 2 examples, same topic, different styles */
    hu_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    bank.channel = hu_strdup(alloc, "telegram");

    hu_persona_example_t examples[2];
    memset(examples, 0, sizeof(examples));

    /* Example 0: formal, long response */
    examples[0].context = hu_strdup(alloc, "work chat");
    examples[0].incoming = hu_strdup(alloc, "how's the project?");
    examples[0].response = hu_strdup(
        alloc,
        "The project is proceeding according to schedule with no major blockers at this time.");

    /* Example 1: casual, short response with abbreviations */
    examples[1].context = hu_strdup(alloc, "work chat");
    examples[1].incoming = hu_strdup(alloc, "how's the project?");
    examples[1].response = hu_strdup(alloc, "going well, ty for asking!");

    bank.examples = examples;
    bank.examples_count = 2;

    persona.example_banks = &bank;
    persona.example_banks_count = 1;

    /* Target style: casual with abbreviations (like example 1) */
    hu_communication_style_t style;
    memset(&style, 0, sizeof(style));
    style.lowercase_ratio = 0.8f;
    style.abbreviation_ratio = 0.3f;
    style.avg_message_length = 50;

    /* Select with style (should prefer example 1) */
    const hu_persona_example_t *out[5];
    size_t count = 0;
    hu_error_t err =
        hu_persona_select_examples(&persona, "telegram", 8, "work", 4, out, &count, 5, &style);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GT(count, 0);
    /* The style-matching example should be first or higher-ranked */
    HU_ASSERT_EQ(out[0], &examples[1]);

    /* Clean up */
    for (int i = 0; i < 2; i++) {
        alloc->free(alloc->ctx, examples[i].context, strlen(examples[i].context) + 1);
        alloc->free(alloc->ctx, examples[i].incoming, strlen(examples[i].incoming) + 1);
        alloc->free(alloc->ctx, examples[i].response, strlen(examples[i].response) + 1);
    }
    alloc->free(alloc->ctx, bank.channel, strlen(bank.channel) + 1);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Test: empty response handled gracefully
 * ────────────────────────────────────────────────────────────────────────── */

static void test_examples_style_with_empty_response(void) {
    hu_allocator_t sys_alloc = hu_system_allocator();
    hu_allocator_t *alloc = &sys_alloc;
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    hu_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    bank.channel = hu_strdup(alloc, "telegram");

    hu_persona_example_t examples[1];
    memset(examples, 0, sizeof(examples));

    examples[0].context = hu_strdup(alloc, "test");
    examples[0].incoming = hu_strdup(alloc, "hello");
    examples[0].response = hu_strdup(alloc, ""); /* empty response */

    bank.examples = examples;
    bank.examples_count = 1;

    persona.example_banks = &bank;
    persona.example_banks_count = 1;

    hu_communication_style_t style;
    memset(&style, 0, sizeof(style));
    style.lowercase_ratio = 0.5f;
    style.abbreviation_ratio = 0.1f;
    style.avg_message_length = 100;

    const hu_persona_example_t *out[5];
    size_t count = 0;
    hu_error_t err =
        hu_persona_select_examples(&persona, "telegram", 8, "test", 4, out, &count, 5, &style);
    HU_ASSERT_EQ(err, HU_OK);
    /* Should still return the example even with empty response */
    HU_ASSERT_EQ(count, 1);

    alloc->free(alloc->ctx, examples[0].context, strlen(examples[0].context) + 1);
    alloc->free(alloc->ctx, examples[0].incoming, strlen(examples[0].incoming) + 1);
    alloc->free(alloc->ctx, examples[0].response, strlen(examples[0].response) + 1);
    alloc->free(alloc->ctx, bank.channel, strlen(bank.channel) + 1);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Test: no topic with style (should use style as primary signal)
 * ────────────────────────────────────────────────────────────────────────── */

static void test_examples_no_topic_with_style(void) {
    hu_allocator_t sys_alloc = hu_system_allocator();
    hu_allocator_t *alloc = &sys_alloc;
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    hu_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    bank.channel = hu_strdup(alloc, "telegram");

    hu_persona_example_t examples[2];
    memset(examples, 0, sizeof(examples));

    examples[0].context = hu_strdup(alloc, "");
    examples[0].incoming = hu_strdup(alloc, "hi");
    examples[0].response = hu_strdup(alloc, "hey! how r u?"); /* casual, abbreviations */

    examples[1].context = hu_strdup(alloc, "");
    examples[1].incoming = hu_strdup(alloc, "hello");
    examples[1].response = hu_strdup(alloc, "Hello there. How are you doing?"); /* formal, long */

    bank.examples = examples;
    bank.examples_count = 2;

    persona.example_banks = &bank;
    persona.example_banks_count = 1;

    /* Style: casual */
    hu_communication_style_t style;
    memset(&style, 0, sizeof(style));
    style.lowercase_ratio = 0.7f;
    style.abbreviation_ratio = 0.4f;
    style.avg_message_length = 25;

    const hu_persona_example_t *out[5];
    size_t count = 0;
    hu_error_t err =
        hu_persona_select_examples(&persona, "telegram", 8, NULL, 0, out, &count, 5, &style);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    /* Without topic, style drives ranking */
    HU_ASSERT_EQ(out[0], &examples[0]);

    for (int i = 0; i < 2; i++) {
        alloc->free(alloc->ctx, examples[i].context, strlen(examples[i].context) + 1);
        alloc->free(alloc->ctx, examples[i].incoming, strlen(examples[i].incoming) + 1);
        alloc->free(alloc->ctx, examples[i].response, strlen(examples[i].response) + 1);
    }
    alloc->free(alloc->ctx, bank.channel, strlen(bank.channel) + 1);
}

void run_persona_examples_style_tests(void) {
    HU_TEST_SUITE("Persona.Examples.Style");
    HU_RUN_TEST(test_examples_null_style_matches_topic_only);
    HU_RUN_TEST(test_examples_style_prefers_fingerprint_match);
    HU_RUN_TEST(test_examples_style_with_empty_response);
    HU_RUN_TEST(test_examples_no_topic_with_style);
}
