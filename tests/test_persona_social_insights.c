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

/* ── Sprint A.7 — social_state.json consumer tests ──────────────────
 *
 * Write a synthetic snapshot to a tmp file, render it, assert the
 * paragraph carries the expected handles + verbs. Pinned defensively
 * against missing files / malformed JSON / empty arrays. */

#include <stdio.h>
#include <unistd.h>

static const char *write_tmp_snapshot(const char *content, char *path_buf, size_t cap) {
    snprintf(path_buf, cap, "/tmp/hu_social_state_test_%d.json", (int)getpid());
    unlink(path_buf);
    FILE *f = fopen(path_buf, "w");
    if (!f)
        return NULL;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path_buf;
}

static void test_snapshot_render_missing_file_returns_zero(void) {
    char out[1024] = {0};
    size_t n = hu_persona_render_social_state_snapshot("/tmp/hu_does_not_exist_xyz.json", out,
                                                       sizeof(out));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_snapshot_render_empty_arrays_returns_zero(void) {
    const char *empty_json = "{\n"
                             "  \"generated_at_unix\": 1779642882,\n"
                             "  \"stale_contacts\": [],\n"
                             "  \"signatures\": [],\n"
                             "  \"drift_alerts\": []\n"
                             "}\n";
    char path_buf[128];
    const char *path = write_tmp_snapshot(empty_json, path_buf, sizeof(path_buf));
    HU_ASSERT_NOT_NULL(path);

    char out[1024] = {0};
    size_t n = hu_persona_render_social_state_snapshot(path, out, sizeof(out));
    HU_ASSERT_EQ((int)n, 0);
    unlink(path);
}

static void test_snapshot_render_stale_contacts_surfaces_handles(void) {
    const char *json = "{\n"
                       "  \"generated_at_unix\": 1779642882,\n"
                       "  \"stale_contacts\": [\n"
                       "    {\"contact\":\"+15551234567\",\"last_message_unix\":1777915021,"
                       "\"days_since_last\":21,\"historical_count\":42},\n"
                       "    {\"contact\":\"+15559876543\",\"last_message_unix\":1778000000,"
                       "\"days_since_last\":15,\"historical_count\":20}\n"
                       "  ],\n"
                       "  \"signatures\": [],\n"
                       "  \"drift_alerts\": []\n"
                       "}\n";
    char path_buf[128];
    const char *path = write_tmp_snapshot(json, path_buf, sizeof(path_buf));
    HU_ASSERT_NOT_NULL(path);

    char out[2048] = {0};
    size_t n = hu_persona_render_social_state_snapshot(path, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    /* Paragraph header + both handles + quiet-for-N-days framing must
     * appear so the LLM consumer can act on the signal. */
    HU_ASSERT_TRUE(strstr(out, "Recent social signals:") != NULL);
    HU_ASSERT_TRUE(strstr(out, "+15551234567") != NULL);
    HU_ASSERT_TRUE(strstr(out, "+15559876543") != NULL);
    HU_ASSERT_TRUE(strstr(out, "quiet for 21 days") != NULL);
    HU_ASSERT_TRUE(strstr(out, "42 historical messages") != NULL);
    unlink(path);
}

static void test_snapshot_render_drift_surfaces_dimension_name(void) {
    /* dimension 1 = response latency */
    const char *json = "{\n"
                       "  \"generated_at_unix\": 1779642882,\n"
                       "  \"stale_contacts\": [],\n"
                       "  \"signatures\": [],\n"
                       "  \"drift_alerts\": [\n"
                       "    {\"contact\":\"+15551234567\",\"dimension\":1,\"severity\":3,"
                       "\"sigma\":2.5,\"recent\":300.0,\"baseline\":120.0}\n"
                       "  ]\n"
                       "}\n";
    char path_buf[128];
    const char *path = write_tmp_snapshot(json, path_buf, sizeof(path_buf));
    HU_ASSERT_NOT_NULL(path);

    char out[1024] = {0};
    size_t n = hu_persona_render_social_state_snapshot(path, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "+15551234567") != NULL);
    HU_ASSERT_TRUE(strstr(out, "response latency") != NULL);
    HU_ASSERT_TRUE(strstr(out, "drifted") != NULL);
    unlink(path);
}

static void test_snapshot_render_caps_at_three_stale_contacts(void) {
    /* Five stale contacts in input; only top 3 should render to avoid
     * prompt bloat. */
    const char *json = "{\n"
                       "  \"generated_at_unix\": 1779642882,\n"
                       "  \"stale_contacts\": [\n"
                       "    "
                       "{\"contact\":\"+1111\",\"last_message_unix\":1,\"days_since_last\":30,"
                       "\"historical_count\":100},\n"
                       "    "
                       "{\"contact\":\"+2222\",\"last_message_unix\":2,\"days_since_last\":25,"
                       "\"historical_count\":90},\n"
                       "    "
                       "{\"contact\":\"+3333\",\"last_message_unix\":3,\"days_since_last\":20,"
                       "\"historical_count\":80},\n"
                       "    "
                       "{\"contact\":\"+4444\",\"last_message_unix\":4,\"days_since_last\":15,"
                       "\"historical_count\":70},\n"
                       "    "
                       "{\"contact\":\"+5555\",\"last_message_unix\":5,\"days_since_last\":14,"
                       "\"historical_count\":60}\n"
                       "  ],\n"
                       "  \"signatures\": [],\n"
                       "  \"drift_alerts\": []\n"
                       "}\n";
    char path_buf[128];
    const char *path = write_tmp_snapshot(json, path_buf, sizeof(path_buf));

    char out[2048] = {0};
    size_t n = hu_persona_render_social_state_snapshot(path, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "+1111") != NULL);
    HU_ASSERT_TRUE(strstr(out, "+2222") != NULL);
    HU_ASSERT_TRUE(strstr(out, "+3333") != NULL);
    /* 4th and 5th should NOT appear */
    HU_ASSERT_TRUE(strstr(out, "+4444") == NULL);
    HU_ASSERT_TRUE(strstr(out, "+5555") == NULL);
    unlink(path);
}

static void test_snapshot_render_handles_malformed_json_safely(void) {
    const char *garbage = "{ this is not valid json at all }}}{{{";
    char path_buf[128];
    const char *path = write_tmp_snapshot(garbage, path_buf, sizeof(path_buf));
    char out[1024] = {0};
    /* Don't crash; return 0 silently. */
    size_t n = hu_persona_render_social_state_snapshot(path, out, sizeof(out));
    HU_ASSERT_EQ((int)n, 0);
    unlink(path);
}

void run_persona_social_insights_tests(void) {
    HU_TEST_SUITE("persona_social_insights");
    HU_RUN_TEST(test_render_empty_model_returns_zero);
    HU_RUN_TEST(test_render_single_reactor_surfaces_handle);
    HU_RUN_TEST(test_render_multi_contact_lists_each);
    HU_RUN_TEST(test_render_truncation_safe_when_buffer_small);
    HU_RUN_TEST(test_render_null_inputs_return_zero);
    HU_RUN_TEST(test_snapshot_render_missing_file_returns_zero);
    HU_RUN_TEST(test_snapshot_render_empty_arrays_returns_zero);
    HU_RUN_TEST(test_snapshot_render_stale_contacts_surfaces_handles);
    HU_RUN_TEST(test_snapshot_render_drift_surfaces_dimension_name);
    HU_RUN_TEST(test_snapshot_render_caps_at_three_stale_contacts);
    HU_RUN_TEST(test_snapshot_render_handles_malformed_json_safely);
}
