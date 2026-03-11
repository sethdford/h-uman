#include "human/context/humor.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

static void playful_with_humor_config_returns_directive_with_style(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "observational", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    humor.frequency = "medium";
    humor.signature_phrases_count = 0;
    humor.self_deprecation_count = 0;
    humor.never_during_count = 0;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, true, &out_len);

    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(d, "observational") != NULL);
    HU_ASSERT_TRUE(strstr(d, "HUMOR") != NULL);

    alloc.free(alloc.ctx, d, out_len + 1);
}

static void emotion_grief_in_never_during_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "deadpan", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    strncpy(humor.never_during[0], "grief", sizeof(humor.never_during[0]) - 1);
    humor.never_during_count = 1;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "grief", 5, true, &out_len);

    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(out_len, 0u);
}

static void no_humor_config_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    humor.style_count = 0;
    humor.signature_phrases_count = 0;
    humor.self_deprecation_count = 0;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, true, &out_len);

    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(out_len, 0u);
}

static void not_playful_low_frequency_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "observational", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    humor.frequency = "low";

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, false, &out_len);

    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(out_len, 0u);
}

static void directive_includes_signature_phrases_and_self_deprecation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "self_deprecating", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    strncpy(humor.signature_phrases[0], "as per usual", sizeof(humor.signature_phrases[0]) - 1);
    humor.signature_phrases_count = 1;
    strncpy(humor.self_deprecation_topics[0], "tech fails",
            sizeof(humor.self_deprecation_topics[0]) - 1);
    humor.self_deprecation_count = 1;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, true, &out_len);

    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_TRUE(strstr(d, "as per usual") != NULL);
    HU_ASSERT_TRUE(strstr(d, "tech fails") != NULL);
    HU_ASSERT_TRUE(strstr(d, "Rule of three") != NULL);

    alloc.free(alloc.ctx, d, out_len + 1);
}

void run_humor_tests(void) {
    HU_TEST_SUITE("humor");
    HU_RUN_TEST(playful_with_humor_config_returns_directive_with_style);
    HU_RUN_TEST(emotion_grief_in_never_during_returns_null);
    HU_RUN_TEST(no_humor_config_returns_null);
    HU_RUN_TEST(not_playful_low_frequency_returns_null);
    HU_RUN_TEST(directive_includes_signature_phrases_and_self_deprecation);
}
