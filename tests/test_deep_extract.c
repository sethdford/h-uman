#include "human/core/allocator.h"
#include "human/memory/deep_extract.h"
#include "test_framework.h"
#include <string.h>

static void deep_extract_build_prompt_includes_conversation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *conv = "User: I have a meeting with Alice.\nAssistant: Good luck!";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_deep_extract_build_prompt(&alloc, conv, strlen(conv), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Extract structured") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Conversation:") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void deep_extract_parse_extracts_facts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"facts\":[{\"subject\":\"Alice\",\"predicate\":\"works_at\",\"object\":\"Acme\","
        "\"confidence\":0.9}],\"relations\":[],\"summary\":\"User mentioned Alice.\"}";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_parse(&alloc, json, strlen(json), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "Alice");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "works_at");
    HU_ASSERT_STR_EQ(out.facts[0].object, "Acme");
    HU_ASSERT_FLOAT_EQ(out.facts[0].confidence, 0.9, 0.001);
    HU_ASSERT_NOT_NULL(out.summary);
    HU_ASSERT_TRUE(strstr(out.summary, "Alice") != NULL);
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void deep_extract_parse_extracts_relations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"facts\":[],\"relations\":[{\"entity_a\":\"Bob\",\"relation\":\"reports_to\","
        "\"entity_b\":\"Alice\",\"confidence\":0.85}],\"summary\":\"Bob reports to Alice.\"}";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_parse(&alloc, json, strlen(json), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.relation_count, 1);
    HU_ASSERT_STR_EQ(out.relations[0].entity_a, "Bob");
    HU_ASSERT_STR_EQ(out.relations[0].relation, "reports_to");
    HU_ASSERT_STR_EQ(out.relations[0].entity_b, "Alice");
    HU_ASSERT_FLOAT_EQ(out.relations[0].confidence, 0.85, 0.001);
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void deep_extract_parse_handles_empty_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_parse(&alloc, "", 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 0);
    HU_ASSERT_EQ(out.relation_count, 0);
    HU_ASSERT_NULL(out.summary);
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void deep_extract_parse_handles_malformed_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_parse(&alloc, "{invalid json", 13, &out);
    HU_ASSERT_NEQ(err, HU_OK);
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_work_at(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I work at Google";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "works_at");
    HU_ASSERT_STR_EQ(out.facts[0].object, "Google");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_like(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I like hiking";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "likes");
    HU_ASSERT_STR_EQ(out.facts[0].object, "hiking");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_live_in(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I live in Austin";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "lives_in");
    HU_ASSERT_STR_EQ(out.facts[0].object, "Austin");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_null_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, NULL, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 0);
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_case_insensitive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I WORK AT Acme Corp";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "works_at");
    HU_ASSERT_STR_EQ(out.facts[0].object, "Acme Corp");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_no_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "hello how are you";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 0);
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_is_a(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I'm a software engineer";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "is_a");
    HU_ASSERT_STR_EQ(out.facts[0].object, "software engineer");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_loves(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I love cooking pasta";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "loves");
    HU_ASSERT_STR_EQ(out.facts[0].object, "cooking pasta");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_hates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I hate mornings";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "hates");
    HU_ASSERT_STR_EQ(out.facts[0].object, "mornings");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "my name is Sarah";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "name");
    HU_ASSERT_STR_EQ(out.facts[0].object, "Sarah");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_extracts_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "my job is teaching";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 1);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "job");
    HU_ASSERT_STR_EQ(out.facts[0].object, "teaching");
    hu_deep_extract_result_deinit(&out, &alloc);
}

static void lightweight_build_prompt_null_alloc(void) {
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_deep_extract_build_prompt(NULL, "hello", 5, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
}

static void lightweight_multiple_facts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I work at Google. I live in Austin";
    hu_deep_extract_result_t out;
    hu_error_t err = hu_deep_extract_lightweight(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.fact_count, 2);
    HU_ASSERT_STR_EQ(out.facts[0].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[0].predicate, "works_at");
    HU_ASSERT_STR_EQ(out.facts[0].object, "Google");
    HU_ASSERT_STR_EQ(out.facts[1].subject, "user");
    HU_ASSERT_STR_EQ(out.facts[1].predicate, "lives_in");
    HU_ASSERT_STR_EQ(out.facts[1].object, "Austin");
    hu_deep_extract_result_deinit(&out, &alloc);
}

void run_deep_extract_tests(void) {
    HU_TEST_SUITE("deep_extract");
    HU_RUN_TEST(deep_extract_build_prompt_includes_conversation);
    HU_RUN_TEST(deep_extract_parse_extracts_facts);
    HU_RUN_TEST(deep_extract_parse_extracts_relations);
    HU_RUN_TEST(deep_extract_parse_handles_empty_response);
    HU_RUN_TEST(deep_extract_parse_handles_malformed_json);
    HU_RUN_TEST(lightweight_extracts_work_at);
    HU_RUN_TEST(lightweight_extracts_like);
    HU_RUN_TEST(lightweight_extracts_live_in);
    HU_RUN_TEST(lightweight_extracts_case_insensitive);
    HU_RUN_TEST(lightweight_null_input);
    HU_RUN_TEST(lightweight_no_match);
    HU_RUN_TEST(lightweight_extracts_is_a);
    HU_RUN_TEST(lightweight_extracts_loves);
    HU_RUN_TEST(lightweight_extracts_hates);
    HU_RUN_TEST(lightweight_extracts_name);
    HU_RUN_TEST(lightweight_extracts_job);
    HU_RUN_TEST(lightweight_build_prompt_null_alloc);
    HU_RUN_TEST(lightweight_multiple_facts);
}
