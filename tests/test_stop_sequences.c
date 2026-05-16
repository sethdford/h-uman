#include "human/agent/stop_sequence_registry.h"
#include "test_framework.h"
#include <string.h>

static void registry_anthropic_has_human_user_stops(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("anthropic", 9, &seqs, &count), HU_OK);
    HU_ASSERT(count >= 2);
    bool has_human = false, has_user = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(seqs[i], "\n\nHuman:") == 0)
            has_human = true;
        if (strcmp(seqs[i], "\n\nUser:") == 0)
            has_user = true;
    }
    HU_ASSERT(has_human && has_user);
}

static void registry_openai_has_user_stops(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("openai", 6, &seqs, &count), HU_OK);
    HU_ASSERT(count >= 1);
}

static void registry_ollama_has_eot_and_im_end(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("ollama", 6, &seqs, &count), HU_OK);
    HU_ASSERT(count >= 2);
    bool has_eot = false, has_im_end = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(seqs[i], "<|eot_id|>") == 0)
            has_eot = true;
        if (strcmp(seqs[i], "<|im_end|>") == 0)
            has_im_end = true;
    }
    HU_ASSERT(has_eot && has_im_end);
}

static void registry_gemini_has_user_stops(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("gemini", 6, &seqs, &count), HU_OK);
    HU_ASSERT(count >= 1);
}

static void registry_openrouter_shares_openai_stops(void) {
    const char *const *or_seqs = NULL;
    const char *const *oa_seqs = NULL;
    size_t or_count = 0, oa_count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("openrouter", 10, &or_seqs, &or_count), HU_OK);
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("openai", 6, &oa_seqs, &oa_count), HU_OK);
    HU_ASSERT_EQ(or_count, oa_count);
}

static void registry_unknown_returns_empty(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("madeup", 6, &seqs, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT(seqs == NULL);
}

static void registry_null_provider_returns_empty(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup(NULL, 0, &seqs, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void registry_null_out_args_returns_error(void) {
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("anthropic", 9, NULL, &count),
                 HU_ERR_INVALID_ARGUMENT);
    const char *const *seqs = NULL;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("anthropic", 9, &seqs, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_stop_sequences_tests(void) {
    HU_TEST_SUITE("stop_sequences");
    HU_RUN_TEST(registry_anthropic_has_human_user_stops);
    HU_RUN_TEST(registry_openai_has_user_stops);
    HU_RUN_TEST(registry_ollama_has_eot_and_im_end);
    HU_RUN_TEST(registry_gemini_has_user_stops);
    HU_RUN_TEST(registry_openrouter_shares_openai_stops);
    HU_RUN_TEST(registry_unknown_returns_empty);
    HU_RUN_TEST(registry_null_provider_returns_empty);
    HU_RUN_TEST(registry_null_out_args_returns_error);
}
