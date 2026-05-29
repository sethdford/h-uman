/* test_attachment.c — pins the attachment-style perception model
 * (src/cognition/attachment.c). EMA over relational signals -> style
 * classification (secure/anxious/avoidant/disorganized) -> adaptation
 * directive. Pure + deterministic; build_context allocation freed (ASan).
 *
 * cognition/ is the perception layer of the Modeled-Person context; this test
 * also guards the build_context low-confidence suppression that keeps an
 * under-determined style from leaking a directive into the prompt. */
#include "human/cognition/attachment.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <string.h>

/* init seeds neutral mid-point signals and an UNKNOWN style. */
static void attachment_init_is_neutral_unknown(void) {
    hu_attachment_state_t s;
    hu_attachment_init(&s);
    HU_ASSERT_EQ((int)s.user_style, (int)HU_ATTACH_UNKNOWN);
    HU_ASSERT_TRUE(s.user_confidence == 0.0f);
    HU_ASSERT_TRUE(s.adaptation_directive == NULL);
}

/* style_name maps every enum value to its stable lowercase string. */
static void attachment_style_name_maps_all(void) {
    HU_ASSERT_STR_EQ(hu_attachment_style_name(HU_ATTACH_SECURE), "secure");
    HU_ASSERT_STR_EQ(hu_attachment_style_name(HU_ATTACH_ANXIOUS), "anxious");
    HU_ASSERT_STR_EQ(hu_attachment_style_name(HU_ATTACH_AVOIDANT), "avoidant");
    HU_ASSERT_STR_EQ(hu_attachment_style_name(HU_ATTACH_DISORGANIZED), "disorganized");
    HU_ASSERT_STR_EQ(hu_attachment_style_name(HU_ATTACH_UNKNOWN), "unknown");
}

/* high proximity-seeking + separation distress over repeated turns classifies
 * ANXIOUS (prox>0.7 && distress>0.6 is the first branch). */
static void attachment_high_proximity_distress_is_anxious(void) {
    hu_attachment_state_t s;
    hu_attachment_init(&s);
    /* args: message_frequency, emotional_share_ratio, gap_distress_signal,
     * independence_signal, growth_from_relationship */
    for (int i = 0; i < 3; i++)
        hu_attachment_update(&s, 1.0f, 0.5f, 1.0f, 0.1f, 0.4f);
    HU_ASSERT_EQ((int)s.user_style, (int)HU_ATTACH_ANXIOUS);
}

/* low proximity + low safe-haven over repeated turns classifies AVOIDANT. */
static void attachment_low_proximity_safe_is_avoidant(void) {
    hu_attachment_state_t s;
    hu_attachment_init(&s);
    /* low message_frequency + low emotional_share -> avoidant */
    for (int i = 0; i < 3; i++)
        hu_attachment_update(&s, 0.0f, 0.0f, 0.1f, 0.9f, 0.3f);
    HU_ASSERT_EQ((int)s.user_style, (int)HU_ATTACH_AVOIDANT);
}

/* build_context SUPPRESSES output while the style is under-determined
 * (confidence <= 0.3): returns OK with *out == NULL — no directive leaks. */
static void attachment_build_context_suppresses_low_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_attachment_state_t s;
    hu_attachment_init(&s); /* confidence 0 */
    char *ctx = (char *)0x1;
    size_t len = 99;
    HU_ASSERT_EQ(hu_attachment_build_context(&alloc, &s, &ctx, &len), HU_OK);
    HU_ASSERT_TRUE(ctx == NULL);
    HU_ASSERT_EQ(len, 0u);
}

/* once a style is confidently established, build_context emits the style name
 * + an adaptation directive; the allocation is freed (ASan). */
static void attachment_build_context_emits_when_confident(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_attachment_state_t s;
    hu_attachment_init(&s);
    for (int i = 0; i < 5; i++) /* drive far from neutral -> confidence > 0.3 */
        hu_attachment_update(&s, 1.0f, 0.5f, 1.0f, 0.1f, 0.4f);
    HU_ASSERT_TRUE(s.user_confidence > 0.3f);

    char *ctx = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_attachment_build_context(&alloc, &s, &ctx, &len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "ATTACHMENT") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, hu_attachment_style_name(s.user_style)) != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

void run_attachment_tests(void);
void run_attachment_tests(void) {
    HU_TEST_SUITE("attachment");
    HU_RUN_TEST(attachment_init_is_neutral_unknown);
    HU_RUN_TEST(attachment_style_name_maps_all);
    HU_RUN_TEST(attachment_high_proximity_distress_is_anxious);
    HU_RUN_TEST(attachment_low_proximity_safe_is_avoidant);
    HU_RUN_TEST(attachment_build_context_suppresses_low_confidence);
    HU_RUN_TEST(attachment_build_context_emits_when_confident);
}
