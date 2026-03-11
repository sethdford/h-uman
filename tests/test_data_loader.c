#include "human/data/loader.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/context/conversation.h"
#include "human/security.h"
#ifdef HU_ENABLE_PERSONA
#include "human/persona/circadian.h"
#endif
#include "test_framework.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void test_data_loader_loads_embedded_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 10);
    HU_ASSERT_NOT_NULL(strstr(out, "GROUP CHAT"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_unknown_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "nonexistent/file.txt", &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_data_loader_falls_back_to_embedded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load(&alloc, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 10);
    HU_ASSERT_NOT_NULL(strstr(out, "GROUP CHAT"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_null_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    /* Test null allocator */
    hu_error_t err = hu_data_load_embedded(NULL, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Test null path */
    err = hu_data_load_embedded(&alloc, NULL, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Test null out pointer */
    err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", NULL, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Test null out_len pointer */
    err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", &out, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_data_loader_content_correct(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);

    /* Verify specific content */
    HU_ASSERT_NOT_NULL(strstr(out, "[GROUP CHAT]"));
    HU_ASSERT_NOT_NULL(strstr(out, "Keep responses to 1-2 sentences"));
    HU_ASSERT_NOT_NULL(strstr(out, "Don't dominate"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_safety_rules(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/safety_rules.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "## Safety"));
    HU_ASSERT_NOT_NULL(strstr(out, "Do not exfiltrate private data"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_default_identity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/default_identity.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "You are Human"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_reasoning_instruction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/reasoning_instruction.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "## Reasoning"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_autonomy_readonly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/autonomy_readonly.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "readonly mode"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_autonomy_supervised(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/autonomy_supervised.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "supervised mode"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_autonomy_full(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/autonomy_full.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "full autonomy mode"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_persona_reinforcement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/persona_reinforcement.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "real person"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ────────────────────────────────────────────────────────────────────────────
   Integration tests: verify externalized data system works end-to-end
   ──────────────────────────────────────────────────────────────────────────── */

/* Test 1: Verify all embedded data files load successfully */
static void integration_all_data_files_load_successfully(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Key data files that should all be embedded */
    const char *essential_paths[] = {
        "conversation/ai_disclosure_patterns.json",
        "conversation/filler_words.json",
        "conversation/engage_words.json",
        "conversation/backchannel_phrases.json",
        "conversation/emotional_words.json",
        "conversation/starters.json",
        "conversation/personal_sharing.json",
        "conversation/contractions.json",
        "conversation/conversation_intros.json",
        "conversation/time_gap_phrases.json",
        "conversation/crisis_keywords.json",
        "memory/emotion_prefixes.json",
        "memory/emotion_adjectives.json",
        "memory/relationship_words.json",
        "memory/topic_patterns.json",
        "memory/commitment_prefixes.json",
        "agent/commitment_patterns.json",
        "security/command_lists.json",
        "persona/circadian_phases.json",
        "persona/relationship_stages.json",
        "prompts/tone_hints.json",
    };

    size_t num_paths = sizeof(essential_paths) / sizeof(essential_paths[0]);
    for (size_t i = 0; i < num_paths; i++) {
        char *data = NULL;
        size_t data_len = 0;
        hu_error_t err = hu_data_load_embedded(&alloc, essential_paths[i], &data, &data_len);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(data);
        HU_ASSERT_TRUE(data_len > 0);
        alloc.free(alloc.ctx, data, data_len + 1);
    }
}

/* Test 2: Verify conversation data init succeeds */
static void integration_conversation_data_init_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_error_t err = hu_conversation_data_init(&alloc);
    HU_ASSERT_EQ(err, HU_OK);

    /* Cleanup after test */
    hu_conversation_data_cleanup();
}

/* Test 3: Verify AI disclosure detection works with loaded data */
static void integration_ai_disclosure_detection_with_loaded_data(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Initialize conversation data (loads AI disclosure patterns from JSON) */
    hu_error_t err = hu_conversation_data_init(&alloc);
    HU_ASSERT_EQ(err, HU_OK);

    /* Test that AI disclosure patterns are loaded and work */
    bool detected = hu_conversation_check_ai_disclosure("i'm an ai", 9);
    HU_ASSERT_TRUE(detected);

    detected = hu_conversation_check_ai_disclosure("i am an ai", 10);
    HU_ASSERT_TRUE(detected);

    detected = hu_conversation_check_ai_disclosure("as an ai assistant", 18);
    HU_ASSERT_TRUE(detected);

    /* Verify non-matching text is not detected */
    detected = hu_conversation_check_ai_disclosure("hello there how are you", 23);
    HU_ASSERT_FALSE(detected);

    hu_conversation_data_cleanup();
}

/* Test 4: Verify security policy data init succeeds */
static void integration_policy_data_init_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_error_t err = hu_policy_data_init(&alloc);
    HU_ASSERT_EQ(err, HU_OK);

    /* Cleanup after test */
    hu_policy_data_cleanup(&alloc);
}

/* Test 5: Verify circadian data init succeeds (if persona enabled) */
#ifdef HU_ENABLE_PERSONA
static void integration_circadian_data_init_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_error_t err = hu_circadian_data_init(&alloc);
    HU_ASSERT_EQ(err, HU_OK);

    /* Cleanup after test */
    hu_circadian_data_cleanup(&alloc);
}
#endif

/* Test 6: Verify all conversation JSON data files are valid JSON */
static void integration_conversation_json_files_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *json_paths[] = {
        "conversation/ai_disclosure_patterns.json",
        "conversation/filler_words.json",
        "conversation/engage_words.json",
        "conversation/backchannel_phrases.json",
        "conversation/emotional_words.json",
        "conversation/starters.json",
        "conversation/personal_sharing.json",
        "conversation/contractions.json",
        "conversation/conversation_intros.json",
        "conversation/time_gap_phrases.json",
        "conversation/crisis_keywords.json",
    };

    size_t num_paths = sizeof(json_paths) / sizeof(json_paths[0]);
    for (size_t i = 0; i < num_paths; i++) {
        char *data = NULL;
        size_t data_len = 0;
        hu_error_t err = hu_data_load_embedded(&alloc, json_paths[i], &data, &data_len);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(data);

        /* Verify the JSON parses successfully */
        hu_json_value_t *root = NULL;
        err = hu_json_parse(&alloc, data, data_len, &root);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(root);
        HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);

        hu_json_free(&alloc, root);
        alloc.free(alloc.ctx, data, data_len + 1);
    }
}

/* Test 7: Verify memory system JSON data files are valid */
static void integration_memory_json_files_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *json_paths[] = {
        "memory/emotion_prefixes.json",
        "memory/emotion_adjectives.json",
        "memory/relationship_words.json",
        "memory/topic_patterns.json",
        "memory/commitment_prefixes.json",
    };

    size_t num_paths = sizeof(json_paths) / sizeof(json_paths[0]);
    for (size_t i = 0; i < num_paths; i++) {
        char *data = NULL;
        size_t data_len = 0;
        hu_error_t err = hu_data_load_embedded(&alloc, json_paths[i], &data, &data_len);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(data);

        hu_json_value_t *root = NULL;
        err = hu_json_parse(&alloc, data, data_len, &root);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(root);
        HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);

        hu_json_free(&alloc, root);
        alloc.free(alloc.ctx, data, data_len + 1);
    }
}

/* Test 8: Verify persona JSON data files are valid */
static void integration_persona_json_files_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *json_paths[] = {
        "persona/circadian_phases.json",
        "persona/relationship_stages.json",
    };

    size_t num_paths = sizeof(json_paths) / sizeof(json_paths[0]);
    for (size_t i = 0; i < num_paths; i++) {
        char *data = NULL;
        size_t data_len = 0;
        hu_error_t err = hu_data_load_embedded(&alloc, json_paths[i], &data, &data_len);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(data);

        hu_json_value_t *root = NULL;
        err = hu_json_parse(&alloc, data, data_len, &root);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(root);
        HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);

        hu_json_free(&alloc, root);
        alloc.free(alloc.ctx, data, data_len + 1);
    }
}

/* Test 9: Verify security JSON data files are valid */
static void integration_security_json_files_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *json_paths[] = {
        "agent/commitment_patterns.json",
        "security/command_lists.json",
    };

    size_t num_paths = sizeof(json_paths) / sizeof(json_paths[0]);
    for (size_t i = 0; i < num_paths; i++) {
        char *data = NULL;
        size_t data_len = 0;
        hu_error_t err = hu_data_load_embedded(&alloc, json_paths[i], &data, &data_len);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(data);

        hu_json_value_t *root = NULL;
        err = hu_json_parse(&alloc, data, data_len, &root);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(root);
        HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);

        hu_json_free(&alloc, root);
        alloc.free(alloc.ctx, data, data_len + 1);
    }
}

/* Test 10: Verify JSON files contain expected arrays */
static void integration_json_files_have_expected_arrays(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Test that ai_disclosure_patterns has a "patterns" array */
    char *data = NULL;
    size_t data_len = 0;
    hu_error_t err = hu_data_load_embedded(&alloc, "conversation/ai_disclosure_patterns.json", &data, &data_len);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *root = NULL;
    err = hu_json_parse(&alloc, data, data_len, &root);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *patterns = hu_json_object_get(root, "patterns");
    HU_ASSERT_NOT_NULL(patterns);
    HU_ASSERT_EQ(patterns->type, HU_JSON_ARRAY);
    HU_ASSERT_TRUE(patterns->data.array.len > 0);

    hu_json_free(&alloc, root);
    alloc.free(alloc.ctx, data, data_len + 1);
}

/* Test 11: Verify filler words data loads and contains string array */
static void integration_filler_words_data_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();

    char *data = NULL;
    size_t data_len = 0;
    hu_error_t err = hu_data_load_embedded(&alloc, "conversation/filler_words.json", &data, &data_len);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *root = NULL;
    err = hu_json_parse(&alloc, data, data_len, &root);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_value_t *words = hu_json_object_get(root, "fillers");
    HU_ASSERT_NOT_NULL(words);
    HU_ASSERT_EQ(words->type, HU_JSON_ARRAY);
    HU_ASSERT_TRUE(words->data.array.len > 0);

    /* Verify that first element is a string */
    if (words->data.array.len > 0) {
        hu_json_value_t *first = words->data.array.items[0];
        HU_ASSERT_NOT_NULL(first);
        HU_ASSERT_EQ(first->type, HU_JSON_STRING);
    }

    hu_json_free(&alloc, root);
    alloc.free(alloc.ctx, data, data_len + 1);
}

void run_data_loader_tests(void) {
    HU_TEST_SUITE("data_loader");
    HU_RUN_TEST(test_data_loader_loads_embedded_default);
    HU_RUN_TEST(test_data_loader_unknown_path_returns_error);
    HU_RUN_TEST(test_data_loader_falls_back_to_embedded);
    HU_RUN_TEST(test_data_loader_null_arguments);
    HU_RUN_TEST(test_data_loader_content_correct);
    HU_RUN_TEST(test_data_loader_safety_rules);
    HU_RUN_TEST(test_data_loader_default_identity);
    HU_RUN_TEST(test_data_loader_reasoning_instruction);
    HU_RUN_TEST(test_data_loader_autonomy_readonly);
    HU_RUN_TEST(test_data_loader_autonomy_supervised);
    HU_RUN_TEST(test_data_loader_autonomy_full);
    HU_RUN_TEST(test_data_loader_persona_reinforcement);

    HU_TEST_SUITE("data_integration");
    HU_RUN_TEST(integration_all_data_files_load_successfully);
    HU_RUN_TEST(integration_conversation_data_init_succeeds);
    HU_RUN_TEST(integration_ai_disclosure_detection_with_loaded_data);
    HU_RUN_TEST(integration_policy_data_init_succeeds);
#ifdef HU_ENABLE_PERSONA
    HU_RUN_TEST(integration_circadian_data_init_succeeds);
#endif
    HU_RUN_TEST(integration_conversation_json_files_valid);
    HU_RUN_TEST(integration_memory_json_files_valid);
    HU_RUN_TEST(integration_persona_json_files_valid);
    HU_RUN_TEST(integration_security_json_files_valid);
    HU_RUN_TEST(integration_json_files_have_expected_arrays);
    HU_RUN_TEST(integration_filler_words_data_valid);
}
