/* tests/test_persona_social_insights.c
 *
 * Sprint A.5 wiring tests: render the personal model's reaction
 * signature into a prompt-ready paragraph. Pins format + edge cases
 * so the eventual prompt-builder splice can rely on a stable shape. */

#include "human/channels/imessage_ingest.h"
#include "human/channels/reaction_event.h"
#include "human/memory/personal_model.h"
#include "human/persona/social_insights.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

static void seed_reactions(hu_personal_model_t *model, const char *contact, int n_love,
                           const char *topic) {
    for (int i = 0; i < n_love; i++) {
        hu_reaction_event_t e = {0};
        e.channel_id = "imessage";
        e.sender_handle = contact;
        e.kind = HU_REACTION_LOVE;
        e.polarity = HU_REACTION_POSITIVE;
        e.timestamp_unix = 1700000000 + i;
        (void)hu_reaction_ingest_personal_model(model, &e, NULL, topic,
                                                /*is_from_me_target=*/true,
                                                /*in_group_chat=*/false);
    }
}

static void test_render_empty_model_returns_zero(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    char out[512] = {0};
    size_t n = hu_persona_render_social_insights(&model, out, sizeof(out));
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_TRUE(out[0] == '\0');
}

static void test_render_single_reactor_surfaces_handle(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    seed_reactions(&model, "Alice", 3, "let's hike Mount Tam Saturday");

    char out[1024] = {0};
    size_t n = hu_persona_render_social_insights(&model, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Reaction-derived insights:") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    HU_ASSERT_TRUE(strstr(out, "positive") != NULL);
    /* Topics line should appear too because love-reactions seed the
     * salient-topic extractor in the calibrate signature. */
    HU_ASSERT_TRUE(strstr(out, "Salient topics") != NULL);
}

static void test_render_multi_contact_lists_each(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    seed_reactions(&model, "Alice", 2, "hiking weekend");
    seed_reactions(&model, "Bob", 1, "that meeting");
    seed_reactions(&model, "Carol", 4, "shipping the release");

    char out[2048] = {0};
    size_t n = hu_persona_render_social_insights(&model, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Bob") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Carol") != NULL);
}

static void test_render_truncation_safe_when_buffer_small(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    seed_reactions(&model, "Alice", 5, "hiking weekend");
    seed_reactions(&model, "Bob", 5, "meetings work");

    /* Cap at 64 bytes — should still produce a valid NUL-terminated
     * truncation. */
    char small[64] = {0};
    size_t n = hu_persona_render_social_insights(&model, small, sizeof(small));
    /* Either 0 (refused render — too small) or > 0 with NUL term. */
    if (n > 0) {
        HU_ASSERT_TRUE(n < sizeof(small));
        HU_ASSERT_TRUE(small[n] == '\0' || small[sizeof(small) - 1] == '\0');
    }
}

static void test_render_null_inputs_return_zero(void) {
    HU_ASSERT_EQ((int)hu_persona_render_social_insights(NULL, NULL, 0), 0);
    char buf[8] = {0};
    HU_ASSERT_EQ((int)hu_persona_render_social_insights(NULL, buf, sizeof(buf)), 0);
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    HU_ASSERT_EQ((int)hu_persona_render_social_insights(&model, NULL, 0), 0);
    /* cap < 32 → refuse (need headroom for the prefix) */
    char tiny[16] = {0};
    HU_ASSERT_EQ((int)hu_persona_render_social_insights(&model, tiny, sizeof(tiny)), 0);
}

void run_persona_social_insights_tests(void) {
    HU_TEST_SUITE("persona_social_insights");
    HU_RUN_TEST(test_render_empty_model_returns_zero);
    HU_RUN_TEST(test_render_single_reactor_surfaces_handle);
    HU_RUN_TEST(test_render_multi_contact_lists_each);
    HU_RUN_TEST(test_render_truncation_safe_when_buffer_small);
    HU_RUN_TEST(test_render_null_inputs_return_zero);
}
