/*
 * Predictive draft suggestions — Sprint 1 Story 1 tests.
 *
 * Covers the pure prompt builder, the reaction-signature renderer, and the
 * response parser. The end-to-end generator (`hu_predictive_drafts_generate`)
 * is exercised in the no-provider branch only — the LLM path needs a
 * configured provider and is verified by the CLI smoke test, not unit
 * tests.
 */

#include "human/memory/personal_model.h"
#include "human/predictive_drafts.h"
#include "test_framework.h"

#include <string.h>

/* Helper: add one reaction-derived fact (mirrors test_calibration_reactions.c). */
static void add_reaction_fact(hu_personal_model_t *m, const char *subject, const char *predicate,
                              const char *object, int64_t last_seen) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    f->type = HU_KNOWLEDGE_PROPOSITIONAL;
    strncpy(f->subject, subject, sizeof(f->subject) - 1);
    strncpy(f->predicate, predicate, sizeof(f->predicate) - 1);
    strncpy(f->object, object, sizeof(f->object) - 1);
    strncpy(f->source_hint, "reaction_ingest", sizeof(f->source_hint) - 1);
    f->confidence = 0.8f;
    f->last_seen_at = last_seen;
}

/* ── prompt builder ─────────────────────────────────────────────────── */

static void test_build_prompt_empty_inputs_still_returns_nonzero(void) {
    char buf[1024];
    size_t n = hu_predictive_drafts_build_prompt(NULL, NULL, NULL, NULL, NULL, 3, buf, sizeof(buf));
    HU_ASSERT_GT(n, (size_t)0);
    /* Even with no inputs the prompt must reference the recipient and the
     * JSON output format so the model can do something. */
    HU_ASSERT_NOT_NULL(strstr(buf, "(unknown)"));
    HU_ASSERT_NOT_NULL(strstr(buf, "drafts"));
    HU_ASSERT_NOT_NULL(strstr(buf, "JSON"));
}

static void test_build_prompt_includes_contact_and_channel(void) {
    char buf[2048];
    size_t n =
        hu_predictive_drafts_build_prompt("alice", "imessage", "User cares about hiking.",
                                          "Last 3 messages were about Saturday plans.",
                                          "alice reacts positively to hiking", 3, buf, sizeof(buf));
    HU_ASSERT_GT(n, (size_t)0);
    HU_ASSERT_NOT_NULL(strstr(buf, "alice"));
    HU_ASSERT_NOT_NULL(strstr(buf, "imessage"));
    HU_ASSERT_NOT_NULL(strstr(buf, "hiking"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Saturday plans"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Reaction signature"));
}

static void test_build_prompt_omits_reaction_section_when_empty(void) {
    char buf[1024];
    hu_predictive_drafts_build_prompt("bob", "slack", "persona", "history", NULL, 3, buf,
                                      sizeof(buf));
    HU_ASSERT_NULL(strstr(buf, "Reaction signature"));
}

static void test_build_prompt_omits_channel_section_when_empty(void) {
    char buf[1024];
    hu_predictive_drafts_build_prompt("bob", NULL, NULL, NULL, NULL, 3, buf, sizeof(buf));
    HU_ASSERT_NULL(strstr(buf, "Channel:"));
}

static void test_build_prompt_n_values_clamped_into_range(void) {
    char buf1[1024], buf3[1024], buf99[1024];
    hu_predictive_drafts_build_prompt("a", NULL, NULL, NULL, NULL, 1, buf1, sizeof(buf1));
    hu_predictive_drafts_build_prompt("a", NULL, NULL, NULL, NULL, 3, buf3, sizeof(buf3));
    hu_predictive_drafts_build_prompt("a", NULL, NULL, NULL, NULL, 99, buf99, sizeof(buf99));
    /* n=99 is clamped to MAX_N=8; the string "8 entries" should appear. */
    HU_ASSERT_NOT_NULL(strstr(buf99, "8 entries"));
    HU_ASSERT_NOT_NULL(strstr(buf1, "1 entries"));
    HU_ASSERT_NOT_NULL(strstr(buf3, "3 entries"));
}

static void test_build_prompt_truncates_on_small_buffer_without_overflow(void) {
    char buf[64];
    size_t n = hu_predictive_drafts_build_prompt(
        "alice", "imessage", "long persona summary that will not fit into the buffer at all",
        "long recent history that also will not fit", "long reaction signature summary", 3, buf,
        sizeof(buf));
    HU_ASSERT_GT(n, (size_t)0);
    /* NUL-terminated within bounds. */
    HU_ASSERT_EQ(buf[sizeof(buf) - 1], '\0');
    /* Truncation is fine; we only require no overflow + NUL-termination. */
}

static void test_build_prompt_zero_cap_writes_nothing(void) {
    char buf[16] = "ZZZZZZZZZZZZZZZ";
    size_t n = hu_predictive_drafts_build_prompt("x", NULL, NULL, NULL, NULL, 3, buf, 0);
    HU_ASSERT_EQ(n, (size_t)0);
    /* Buffer untouched on cap == 0 (no null-deref on `out` allowed). */
    HU_ASSERT_EQ(buf[0], 'Z');
}

static void test_build_prompt_handles_special_characters_in_handle(void) {
    char buf[2048];
    size_t n = hu_predictive_drafts_build_prompt("alice+test@example.com", "imessage", NULL, NULL,
                                                 NULL, 3, buf, sizeof(buf));
    HU_ASSERT_GT(n, (size_t)0);
    HU_ASSERT_NOT_NULL(strstr(buf, "alice+test@example.com"));
}

static void test_build_prompt_handles_unicode_handle(void) {
    /* Chinese characters + emoji. UTF-8 multi-byte must pass through. */
    char buf[2048];
    const char *handle = "陈丽 \xf0\x9f\x91\x8b"; /* "Chen Li 👋" */
    size_t n = hu_predictive_drafts_build_prompt(handle, "telegram", NULL, NULL, NULL, 3, buf,
                                                 sizeof(buf));
    HU_ASSERT_GT(n, (size_t)0);
    HU_ASSERT_NOT_NULL(strstr(buf, handle));
}

/* ── reaction-signature renderer ────────────────────────────────────── */

static void test_render_signature_empty_model_writes_empty(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[256];
    memset(buf, 'X', sizeof(buf));
    size_t n = hu_predictive_drafts_render_signature(&m, "alice", buf, sizeof(buf));
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
}

static void test_render_signature_known_reactor_includes_handle_and_count(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "hiking saturday", 1700000000);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "the hiking trail", 1700001000);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "hiking weekend plans", 1700002000);

    char buf[256];
    size_t n = hu_predictive_drafts_render_signature(&m, "Alice", buf, sizeof(buf));
    HU_ASSERT_GT(n, (size_t)0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Alice"));
    HU_ASSERT_NOT_NULL(strstr(buf, "positive"));
    HU_ASSERT_NOT_NULL(strstr(buf, "hiking"));
}

static void test_render_signature_unknown_contact_writes_empty(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "hiking saturday", 1700000000);

    char buf[256];
    size_t n = hu_predictive_drafts_render_signature(&m, "carol", buf, sizeof(buf));
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
}

/* ── response parser ────────────────────────────────────────────────── */

static void test_parse_response_strict_json_three_drafts(void) {
    const char *resp = "{\"drafts\":["
                       "{\"text\":\"hey, free "
                       "saturday?\",\"confidence\":0.8,\"rationale\":\"recent hiking topic\"},"
                       "{\"text\":\"thinking of climbing this "
                       "weekend\",\"confidence\":0.7,\"rationale\":\"matches reactions\"},"
                       "{\"text\":\"random thought — coffee "
                       "soon?\",\"confidence\":0.4,\"rationale\":\"low signal\"}"
                       "]}";
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response(resp, strlen(resp), &set);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(set.draft_count, (size_t)3);
    HU_ASSERT_STR_EQ(set.drafts[0].text, "hey, free saturday?");
    HU_ASSERT_NOT_NULL(strstr(set.drafts[0].rationale, "hiking"));
    /* Confidence stored as float ~0.8. Allow small slack. */
    HU_ASSERT_TRUE(set.drafts[0].confidence > 0.7f && set.drafts[0].confidence < 0.9f);
}

static void test_parse_response_json_with_surrounding_prose_still_parses(void) {
    const char *resp = "Here are the drafts:\n"
                       "{\"drafts\":[{\"text\":\"yo!\",\"confidence\":0.5,\"rationale\":\"\"}]}\n"
                       "Hope this helps.";
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response(resp, strlen(resp), &set);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(set.draft_count, (size_t)1);
    HU_ASSERT_STR_EQ(set.drafts[0].text, "yo!");
}

static void test_parse_response_numbered_list_fallback(void) {
    const char *resp = "1. hey, free saturday?\n"
                       "2. thinking of climbing this weekend\n"
                       "3. coffee tomorrow?\n";
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response(resp, strlen(resp), &set);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(set.draft_count, (size_t)3);
    HU_ASSERT_STR_EQ(set.drafts[0].text, "hey, free saturday?");
    HU_ASSERT_STR_EQ(set.drafts[2].text, "coffee tomorrow?");
}

static void test_parse_response_empty_string_returns_parse_error(void) {
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response("", 0, &set);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_PARSE);
}

static void test_parse_response_garbage_text_returns_parse_error(void) {
    const char *resp = "I'm just a humble assistant, nothing to draft here.";
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response(resp, strlen(resp), &set);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_PARSE);
}

static void test_parse_response_clamps_at_max_n(void) {
    /* Build a JSON response with 12 drafts; expect exactly MAX_N = 8. */
    char resp[2048];
    size_t off = 0;
    off += (size_t)snprintf(resp + off, sizeof(resp) - off, "{\"drafts\":[");
    for (int i = 0; i < 12; i++) {
        off += (size_t)snprintf(resp + off, sizeof(resp) - off,
                                "%s{\"text\":\"d%d\",\"confidence\":0.5,\"rationale\":\"\"}",
                                i == 0 ? "" : ",", i);
    }
    off += (size_t)snprintf(resp + off, sizeof(resp) - off, "]}");

    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response(resp, strlen(resp), &set);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(set.draft_count, (size_t)HU_PREDICTIVE_DRAFT_MAX_N);
}

static void test_parse_response_null_inputs_return_invalid(void) {
    hu_predictive_draft_set_t set;
    HU_ASSERT_EQ((int)hu_predictive_drafts_parse_response(NULL, 0, &set),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_predictive_drafts_parse_response("{}", 2, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_parse_response_confidence_clamped_to_unit_range(void) {
    const char *resp = "{\"drafts\":[{\"text\":\"a\",\"confidence\":1.5,\"rationale\":\"\"},"
                       "{\"text\":\"b\",\"confidence\":-0.3,\"rationale\":\"\"}]}";
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_parse_response(resp, strlen(resp), &set);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(set.draft_count, (size_t)2);
    HU_ASSERT_TRUE(set.drafts[0].confidence <= 1.0f);
    HU_ASSERT_TRUE(set.drafts[1].confidence >= 0.0f);
}

/* ── generator: no-grounding refusal ────────────────────────────────── */

static void test_generate_no_grounding_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_personal_model_t m;
    hu_personal_model_init(&m); /* empty: no facts, no topics, no goals, no name */
    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_generate(&alloc, &m, "alice", "imessage", NULL,
                                                   HU_PREDICTIVE_DRAFT_DEFAULT_N, &set);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_FOUND);
}

static void test_generate_null_args_return_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_predictive_draft_set_t set;
    HU_ASSERT_EQ((int)hu_predictive_drafts_generate(NULL, NULL, "alice", NULL, "hist", 3, &set),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_predictive_drafts_generate(&alloc, NULL, NULL, NULL, "hist", 3, &set),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_predictive_drafts_generate(&alloc, NULL, "alice", NULL, "hist", 3, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_predictive_drafts_generate(&alloc, NULL, "", NULL, "hist", 3, &set),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

/* Symmetric test for the --model override (A3 of the autoresponder
 * loop). Same contract shape as the provider override below: setter
 * tolerates NULL, empty, and over-long names without crashing or
 * leaking. */
static void test_set_model_override_accepts_null_empty_and_name(void) {
    hu_predictive_drafts_set_model_override("gemini-3.1-flash-lite");
    hu_predictive_drafts_set_model_override(NULL);
    hu_predictive_drafts_set_model_override("");
    hu_predictive_drafts_set_model_override(
        "a-very-long-model-id-string-that-should-be-safely-truncated-by-snprintf-not-overflow");
    hu_predictive_drafts_set_model_override(NULL);
}

/* B1 provider-override mechanism: setter accepts NULL, empty, and a real
 * name without crashing; subsequent calls revert when cleared. There's
 * no public getter for the override slot, so the contract here is
 * "calling the setter in these patterns must not crash, must not leak,
 * and must remain safe across repeated invocations." A failing
 * implementation (e.g. unbounded strcpy) would crash under ASan. */
static void test_set_provider_override_accepts_null_empty_and_name(void) {
    /* Pattern users hit on the CLI: --provider gemini → run → reset. */
    hu_predictive_drafts_set_provider_override("gemini");
    hu_predictive_drafts_set_provider_override(NULL); /* CLI restore on exit */
    hu_predictive_drafts_set_provider_override("");   /* CLI restore on no override */
    /* Repeated set with a much longer name should truncate, not overflow. */
    hu_predictive_drafts_set_provider_override("a_very_long_provider_name_designed_to_overflow_the_"
                                               "64_byte_static_buffer_used_for_storage");
    hu_predictive_drafts_set_provider_override(NULL);
}

void run_predictive_drafts_tests(void) {
    HU_TEST_SUITE("predictive_drafts");

    HU_RUN_TEST(test_build_prompt_empty_inputs_still_returns_nonzero);
    HU_RUN_TEST(test_build_prompt_includes_contact_and_channel);
    HU_RUN_TEST(test_build_prompt_omits_reaction_section_when_empty);
    HU_RUN_TEST(test_build_prompt_omits_channel_section_when_empty);
    HU_RUN_TEST(test_build_prompt_n_values_clamped_into_range);
    HU_RUN_TEST(test_build_prompt_truncates_on_small_buffer_without_overflow);
    HU_RUN_TEST(test_build_prompt_zero_cap_writes_nothing);
    HU_RUN_TEST(test_build_prompt_handles_special_characters_in_handle);
    HU_RUN_TEST(test_build_prompt_handles_unicode_handle);

    HU_RUN_TEST(test_render_signature_empty_model_writes_empty);
    HU_RUN_TEST(test_render_signature_known_reactor_includes_handle_and_count);
    HU_RUN_TEST(test_render_signature_unknown_contact_writes_empty);

    HU_RUN_TEST(test_parse_response_strict_json_three_drafts);
    HU_RUN_TEST(test_parse_response_json_with_surrounding_prose_still_parses);
    HU_RUN_TEST(test_parse_response_numbered_list_fallback);
    HU_RUN_TEST(test_parse_response_empty_string_returns_parse_error);
    HU_RUN_TEST(test_parse_response_garbage_text_returns_parse_error);
    HU_RUN_TEST(test_parse_response_clamps_at_max_n);
    HU_RUN_TEST(test_parse_response_null_inputs_return_invalid);
    HU_RUN_TEST(test_parse_response_confidence_clamped_to_unit_range);

    HU_RUN_TEST(test_generate_no_grounding_returns_not_found);
    HU_RUN_TEST(test_generate_null_args_return_invalid);
    HU_RUN_TEST(test_set_provider_override_accepts_null_empty_and_name);
    HU_RUN_TEST(test_set_model_override_accepts_null_empty_and_name);
}
