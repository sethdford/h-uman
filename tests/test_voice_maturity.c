#ifdef HU_ENABLE_PERSONA

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona/voice_maturity.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

static void voice_profile_init_null_safe(void) {
    hu_voice_profile_init(NULL);
}

static void voice_profile_init_sets_defaults(void) {
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);

    HU_ASSERT_EQ(profile.stage, HU_VOICE_FORMAL);
    HU_ASSERT_EQ(profile.interaction_count, 0);
    HU_ASSERT_EQ(profile.shared_topics, 0);
    HU_ASSERT_EQ(profile.emotional_exchanges, 0);
    HU_ASSERT_FLOAT_EQ(profile.warmth_score, 0.2f, 0.01f);
    HU_ASSERT_FLOAT_EQ(profile.humor_allowance, 0.1f, 0.01f);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.0f, 0.01f);
}

static void voice_compute_stage_formal_low_interactions(void) {
    hu_voice_stage_t stage = hu_voice_compute_stage(0, 0, 0.0f);
    HU_ASSERT_EQ(stage, HU_VOICE_FORMAL);

    stage = hu_voice_compute_stage(0, 0, 0.5f);
    HU_ASSERT_EQ(stage, HU_VOICE_FORMAL);

    stage = hu_voice_compute_stage(3, 2, 0.2f);
    HU_ASSERT_EQ(stage, HU_VOICE_FORMAL);
}

static void voice_compute_stage_warm(void) {
    hu_voice_stage_t stage = hu_voice_compute_stage(5, 0, 0.3f);
    HU_ASSERT_EQ(stage, HU_VOICE_WARM);

    stage = hu_voice_compute_stage(10, 2, 0.5f);
    HU_ASSERT_EQ(stage, HU_VOICE_WARM);

    stage = hu_voice_compute_stage(5, 0, 0.3f);
    HU_ASSERT_EQ(stage, HU_VOICE_WARM);
}

static void voice_compute_stage_candid(void) {
    hu_voice_stage_t stage = hu_voice_compute_stage(20, 8, 0.5f);
    HU_ASSERT_EQ(stage, HU_VOICE_CANDID);

    stage = hu_voice_compute_stage(25, 10, 0.6f);
    HU_ASSERT_EQ(stage, HU_VOICE_CANDID);

    stage = hu_voice_compute_stage(20, 8, 0.5f);
    HU_ASSERT_EQ(stage, HU_VOICE_CANDID);
}

static void voice_compute_stage_intimate(void) {
    hu_voice_stage_t stage = hu_voice_compute_stage(50, 20, 0.8f);
    HU_ASSERT_EQ(stage, HU_VOICE_INTIMATE);

    stage = hu_voice_compute_stage(100, 50, 100.0f);
    HU_ASSERT_EQ(stage, HU_VOICE_INTIMATE);
}

static void voice_compute_stage_boundary_warm_vs_formal(void) {
    hu_voice_stage_t stage = hu_voice_compute_stage(5, 0, 0.29f);
    HU_ASSERT_EQ(stage, HU_VOICE_FORMAL);

    stage = hu_voice_compute_stage(5, 0, 0.3f);
    HU_ASSERT_EQ(stage, HU_VOICE_WARM);
}

static void voice_compute_stage_boundary_candid_vs_warm(void) {
    hu_voice_stage_t stage = hu_voice_compute_stage(19, 7, 0.5f);
    HU_ASSERT_EQ(stage, HU_VOICE_WARM);

    stage = hu_voice_compute_stage(20, 8, 0.5f);
    HU_ASSERT_EQ(stage, HU_VOICE_CANDID);
}

static void voice_profile_update_null_safe(void) {
    hu_voice_profile_update(NULL, true, true, true);
}

static void voice_profile_update_increments(void) {
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);

    hu_voice_profile_update(&profile, false, false, false);
    HU_ASSERT_EQ(profile.interaction_count, 1);
    HU_ASSERT_EQ(profile.emotional_exchanges, 0);
    HU_ASSERT_EQ(profile.shared_topics, 0);

    hu_voice_profile_update(&profile, true, false, false);
    HU_ASSERT_EQ(profile.interaction_count, 2);
    HU_ASSERT_EQ(profile.emotional_exchanges, 1);
    HU_ASSERT_FLOAT_EQ(profile.warmth_score, 0.22f, 0.01f);

    hu_voice_profile_update(&profile, false, true, false);
    HU_ASSERT_EQ(profile.shared_topics, 1);

    hu_voice_profile_update(&profile, false, false, true);
    HU_ASSERT_FLOAT_EQ(profile.humor_allowance, 0.11f, 0.01f);
}

static void voice_profile_update_warmth_caps_at_one(void) {
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    profile.warmth_score = 0.99f;

    hu_voice_profile_update(&profile, true, false, false);
    HU_ASSERT_TRUE(profile.warmth_score <= 1.0f);
}

static void voice_profile_update_humor_caps(void) {
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    profile.humor_allowance = 0.79f;

    hu_voice_profile_update(&profile, false, false, true);
    HU_ASSERT_TRUE(profile.humor_allowance <= 0.8f);
}

static void voice_compute_stage_updated_after_interactions(void) {
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);

    for (int i = 0; i < 6; i++)
        hu_voice_profile_update(&profile, true, false, false);

    HU_ASSERT_EQ(profile.stage, HU_VOICE_WARM);
}

static void voice_build_guidance_null_profile_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(NULL, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void voice_build_guidance_null_alloc_returns_invalid(void) {
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void voice_build_guidance_null_out_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, NULL, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void voice_build_guidance_null_out_len_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    char *out = NULL;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, &out, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void voice_build_guidance_formal_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_GT(out_len, 0);
    HU_ASSERT_TRUE(strstr(out, "Formal") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Stage:") != NULL);
    HU_ASSERT_TRUE(strstr(out, "warmth:") != NULL);
    HU_ASSERT_TRUE(strstr(out, "humor:") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void voice_build_guidance_warm_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    profile.stage = HU_VOICE_WARM;
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Warm") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void voice_build_guidance_candid_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    profile.stage = HU_VOICE_CANDID;
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Candid") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void voice_build_guidance_intimate_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile = {0};
    hu_voice_profile_init(&profile);
    profile.stage = HU_VOICE_INTIMATE;
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Intimate") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── Vulnerability tracking tests ─────────────────────────────────── */

static void voice_vulnerability_from_content_null_returns_zero(void) {
    HU_ASSERT_FLOAT_EQ(hu_voice_vulnerability_from_content(NULL, 0), 0.0f, 0.01f);
    HU_ASSERT_FLOAT_EQ(hu_voice_vulnerability_from_content("", 0), 0.0f, 0.01f);
}

static void voice_vulnerability_from_content_neutral_low(void) {
    const char *msg = "hey what time is the meeting tomorrow?";
    float score = hu_voice_vulnerability_from_content(msg, strlen(msg));
    HU_ASSERT_FLOAT_EQ(score, 0.0f, 0.01f);
}

static void voice_vulnerability_from_content_emotional_scores(void) {
    const char *msg = "i feel so overwhelmed, i'm scared and struggling";
    float score = hu_voice_vulnerability_from_content(msg, strlen(msg));
    HU_ASSERT_TRUE(score >= 0.3f);
}

static void voice_vulnerability_from_content_multiple_markers(void) {
    const char *msg = "i feel vulnerable, i trust you, i'm grateful, opened up about my secret";
    float score = hu_voice_vulnerability_from_content(msg, strlen(msg));
    HU_ASSERT_TRUE(score >= 0.5f);
}

static void voice_vulnerability_from_content_caps_at_one(void) {
    const char *msg = "i feel scared, i'm worried, i'm sad, i'm lonely, i'm hurt, "
                      "i miss you, i love you, i'm grateful, vulnerable, trust you, "
                      "opened up, never told anyone, struggling, going through hard time";
    float score = hu_voice_vulnerability_from_content(msg, strlen(msg));
    HU_ASSERT_TRUE(score <= 1.0f);
}

static void voice_profile_update_increases_vulnerability(void) {
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.0f, 0.01f);

    /* Emotional exchange should increase vulnerability */
    hu_voice_profile_update(&profile, true, false, false);
    HU_ASSERT_TRUE(profile.vulnerability_level > 0.0f);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.05f, 0.01f);

    /* Multiple emotional exchanges accumulate */
    hu_voice_profile_update(&profile, true, false, false);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.10f, 0.01f);
}

static void voice_profile_update_no_emotion_no_vulnerability_change(void) {
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);

    hu_voice_profile_update(&profile, false, true, true);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.0f, 0.01f);
}

static void voice_vulnerability_caps_at_one(void) {
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);
    profile.vulnerability_level = 0.98f;

    hu_voice_profile_update(&profile, true, false, false);
    HU_ASSERT_TRUE(profile.vulnerability_level <= 1.0f);
}

static void voice_vulnerability_decay_reduces(void) {
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);
    profile.vulnerability_level = 0.5f;

    hu_voice_vulnerability_decay(&profile, 2.0f);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.3f, 0.01f);
}

static void voice_vulnerability_decay_floors_at_zero(void) {
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);
    profile.vulnerability_level = 0.1f;

    hu_voice_vulnerability_decay(&profile, 10.0f);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.0f, 0.01f);
}

static void voice_vulnerability_decay_null_safe(void) {
    hu_voice_vulnerability_decay(NULL, 1.0f);
}

static void voice_vulnerability_decay_zero_hours_no_change(void) {
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);
    profile.vulnerability_level = 0.5f;

    hu_voice_vulnerability_decay(&profile, 0.0f);
    HU_ASSERT_FLOAT_EQ(profile.vulnerability_level, 0.5f, 0.01f);
}

static void voice_build_guidance_includes_vulnerability(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_profile_t profile;
    hu_voice_profile_init(&profile);
    profile.vulnerability_level = 0.5f;
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_voice_build_guidance(&profile, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "vulnerability: 50%"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── Sprint 6 US-14: hu_voice_maturity_build_directive tests ─────────── */

static void voice_build_directive_null_buf_returns_zero(void) {
    size_t written = hu_voice_maturity_build_directive(HU_VOICE_FORMAL, NULL, 256);
    HU_ASSERT_EQ(written, (size_t)0);
}

static void voice_build_directive_zero_len_returns_zero(void) {
    char buf[256];
    size_t written = hu_voice_maturity_build_directive(HU_VOICE_FORMAL, buf, 0);
    HU_ASSERT_EQ(written, (size_t)0);
}

static void voice_build_directive_all_stages_distinct_nonempty(void) {
    char formal[512], warm[512], candid[512], intimate[512];

    size_t nf = hu_voice_maturity_build_directive(HU_VOICE_FORMAL, formal, sizeof(formal));
    size_t nw = hu_voice_maturity_build_directive(HU_VOICE_WARM, warm, sizeof(warm));
    size_t nc = hu_voice_maturity_build_directive(HU_VOICE_CANDID, candid, sizeof(candid));
    size_t ni = hu_voice_maturity_build_directive(HU_VOICE_INTIMATE, intimate, sizeof(intimate));

    /* All must produce a non-empty NUL-terminated string */
    HU_ASSERT_GT(nf, (size_t)0);
    HU_ASSERT_GT(nw, (size_t)0);
    HU_ASSERT_GT(nc, (size_t)0);
    HU_ASSERT_GT(ni, (size_t)0);

    /* Each stage must produce a distinct directive */
    HU_ASSERT_TRUE(strcmp(formal, warm) != 0);
    HU_ASSERT_TRUE(strcmp(formal, candid) != 0);
    HU_ASSERT_TRUE(strcmp(formal, intimate) != 0);
    HU_ASSERT_TRUE(strcmp(warm, candid) != 0);
    HU_ASSERT_TRUE(strcmp(warm, intimate) != 0);
    HU_ASSERT_TRUE(strcmp(candid, intimate) != 0);
}

static void voice_build_directive_formal_differs_from_intimate(void) {
    char formal[512], intimate[512];
    hu_voice_maturity_build_directive(HU_VOICE_FORMAL, formal, sizeof(formal));
    hu_voice_maturity_build_directive(HU_VOICE_INTIMATE, intimate, sizeof(intimate));
    HU_ASSERT_TRUE(strlen(formal) > 0);
    HU_ASSERT_TRUE(strlen(intimate) > 0);
    HU_ASSERT_TRUE(strcmp(formal, intimate) != 0);
}

void run_voice_maturity_tests(void) {
    HU_TEST_SUITE("VoiceMaturity");

    HU_RUN_TEST(voice_profile_init_null_safe);
    HU_RUN_TEST(voice_profile_init_sets_defaults);
    HU_RUN_TEST(voice_compute_stage_formal_low_interactions);
    HU_RUN_TEST(voice_compute_stage_warm);
    HU_RUN_TEST(voice_compute_stage_candid);
    HU_RUN_TEST(voice_compute_stage_intimate);
    HU_RUN_TEST(voice_compute_stage_boundary_warm_vs_formal);
    HU_RUN_TEST(voice_compute_stage_boundary_candid_vs_warm);
    HU_RUN_TEST(voice_profile_update_null_safe);
    HU_RUN_TEST(voice_profile_update_increments);
    HU_RUN_TEST(voice_profile_update_warmth_caps_at_one);
    HU_RUN_TEST(voice_profile_update_humor_caps);
    HU_RUN_TEST(voice_compute_stage_updated_after_interactions);
    HU_RUN_TEST(voice_build_guidance_null_profile_returns_invalid);
    HU_RUN_TEST(voice_build_guidance_null_alloc_returns_invalid);
    HU_RUN_TEST(voice_build_guidance_null_out_returns_invalid);
    HU_RUN_TEST(voice_build_guidance_null_out_len_returns_invalid);
    HU_RUN_TEST(voice_build_guidance_formal_returns_ok);
    HU_RUN_TEST(voice_build_guidance_warm_returns_ok);
    HU_RUN_TEST(voice_build_guidance_candid_returns_ok);
    HU_RUN_TEST(voice_build_guidance_intimate_returns_ok);

    HU_TEST_SUITE("voice_maturity_directive");
    HU_RUN_TEST(voice_build_directive_null_buf_returns_zero);
    HU_RUN_TEST(voice_build_directive_zero_len_returns_zero);
    HU_RUN_TEST(voice_build_directive_all_stages_distinct_nonempty);
    HU_RUN_TEST(voice_build_directive_formal_differs_from_intimate);

    HU_TEST_SUITE("VoiceVulnerability");
    HU_RUN_TEST(voice_vulnerability_from_content_null_returns_zero);
    HU_RUN_TEST(voice_vulnerability_from_content_neutral_low);
    HU_RUN_TEST(voice_vulnerability_from_content_emotional_scores);
    HU_RUN_TEST(voice_vulnerability_from_content_multiple_markers);
    HU_RUN_TEST(voice_vulnerability_from_content_caps_at_one);
    HU_RUN_TEST(voice_profile_update_increases_vulnerability);
    HU_RUN_TEST(voice_profile_update_no_emotion_no_vulnerability_change);
    HU_RUN_TEST(voice_vulnerability_caps_at_one);
    HU_RUN_TEST(voice_vulnerability_decay_reduces);
    HU_RUN_TEST(voice_vulnerability_decay_floors_at_zero);
    HU_RUN_TEST(voice_vulnerability_decay_null_safe);
    HU_RUN_TEST(voice_vulnerability_decay_zero_hours_no_change);
    HU_RUN_TEST(voice_build_guidance_includes_vulnerability);
}

#else

void run_voice_maturity_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_PERSONA */
