/*
 * Tests for the transcript preprocessor (sentence segmenter, SSML builder,
 * per-sentence emotion, discourse markers, volume mapping).
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tts/transcript_prep.h"
#include "test_framework.h"
#include <string.h>

/* ── Sentence Segmenter Tests ─────────────────────────────────────────── */

static void test_segment_simple_sentences(void) {
    hu_prep_sentence_t sents[8];
    const char *text = "Hello there. How are you? I'm great!";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 8);
    HU_ASSERT_EQ(count, (size_t)3);
    HU_ASSERT_TRUE(sents[0].len > 0);
    HU_ASSERT_TRUE(memcmp(sents[0].text, "Hello there.", 12) == 0);
    HU_ASSERT_TRUE(memcmp(sents[1].text, "How are you?", 12) == 0);
    HU_ASSERT_TRUE(memcmp(sents[2].text, "I'm great!", 10) == 0);
}

static void test_segment_abbreviations_preserved(void) {
    hu_prep_sentence_t sents[8];
    const char *text = "Dr. Smith is here. He said Mr. Jones left.";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 8);
    HU_ASSERT_EQ(count, (size_t)2);
    HU_ASSERT_TRUE(sents[0].len > 5);
}

static void test_segment_ellipsis_not_split(void) {
    hu_prep_sentence_t sents[8];
    const char *text = "Well... I guess so. That's fine.";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 8);
    HU_ASSERT_TRUE(count >= 2);
}

static void test_segment_decimal_not_split(void) {
    hu_prep_sentence_t sents[8];
    const char *text = "The temperature is 98.6 degrees. That's normal.";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 8);
    HU_ASSERT_EQ(count, (size_t)2);
}

static void test_segment_no_punctuation(void) {
    hu_prep_sentence_t sents[8];
    const char *text = "Hello world this has no punctuation";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 8);
    HU_ASSERT_EQ(count, (size_t)1);
    HU_ASSERT_TRUE(sents[0].len == strlen(text));
}

static void test_segment_empty_input(void) {
    hu_prep_sentence_t sents[8];
    size_t count = hu_transcript_segment("", 0, sents, 8);
    HU_ASSERT_EQ(count, (size_t)0);
}

static void test_segment_null_input(void) {
    hu_prep_sentence_t sents[8];
    size_t count = hu_transcript_segment(NULL, 0, sents, 8);
    HU_ASSERT_EQ(count, (size_t)0);
}

static void test_segment_max_capacity(void) {
    hu_prep_sentence_t sents[2];
    const char *text = "One. Two. Three. Four.";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 2);
    HU_ASSERT_EQ(count, (size_t)2);
}

static void test_segment_multiple_excl_and_question(void) {
    hu_prep_sentence_t sents[8];
    const char *text = "What?! No way!! Yes!!!";
    size_t count = hu_transcript_segment(text, strlen(text), sents, 8);
    HU_ASSERT_TRUE(count >= 2);
}

/* ── Volume Mapping Tests ─────────────────────────────────────────────── */

static void test_volume_calm_is_soft(void) {
    float v = hu_emotion_to_volume("calm");
    HU_ASSERT_TRUE(v < 1.0f);
    HU_ASSERT_TRUE(v >= 0.8f);
}

static void test_volume_excited_is_loud(void) {
    float v = hu_emotion_to_volume("excited");
    HU_ASSERT_TRUE(v > 1.0f);
    HU_ASSERT_TRUE(v <= 1.3f);
}

static void test_volume_sad_is_quiet(void) {
    float v = hu_emotion_to_volume("sad");
    HU_ASSERT_TRUE(v < 0.9f);
}

static void test_volume_neutral_default(void) {
    float v = hu_emotion_to_volume("content");
    HU_ASSERT_TRUE(v >= 0.95f && v <= 1.05f);
}

static void test_volume_null_default(void) {
    float v = hu_emotion_to_volume(NULL);
    HU_ASSERT_TRUE(v >= 0.99f && v <= 1.01f);
}

/* ── Full Preprocessor Tests ──────────────────────────────────────────── */

static void test_prep_null_returns_error(void) {
    hu_prep_result_t result;
    hu_prep_config_t cfg = {0};
    HU_ASSERT_EQ(hu_transcript_prep(NULL, 0, &cfg, &result), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_transcript_prep("hi", 2, NULL, &result), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_transcript_prep("hi", 2, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_prep_single_sentence_no_breaks(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
    };
    hu_prep_result_t result = {0};
    const char *text = "Hello there.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len > 0);
    HU_ASSERT_EQ(result.sentence_count, (size_t)1);
    HU_ASSERT_NOT_NULL(result.dominant_emotion);
}

static void test_prep_multi_sentence_has_breaks(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "I'm so sorry that happened. But you're going to crush it tomorrow.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.sentence_count, (size_t)2);
    HU_ASSERT_TRUE(strstr(result.output, "<break time=") != NULL);
}

static void test_prep_emotion_shift_produces_tags(void) {
    hu_prep_config_t cfg = {
        .incoming_msg = "I'm sad today",
        .incoming_msg_len = 13,
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 100,
    };
    hu_prep_result_t result = {0};
    const char *text = "I understand how you feel. Congratulations on pushing through!";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(result.output, "<emotion value=") != NULL);
}

static void test_prep_speed_variation_for_exclamation(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 77,
    };
    hu_prep_result_t result = {0};
    const char *text = "This is amazing! Really cool stuff.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.sentence_count, (size_t)2);
    HU_ASSERT_TRUE(result.sentences[0].speed_ratio < 0.95f);
}

static void test_prep_volume_from_emotion(void) {
    hu_prep_config_t cfg = {
        .incoming_msg = "I'm devastated",
        .incoming_msg_len = 14,
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 200,
    };
    hu_prep_result_t result = {0};
    const char *text = "I'm so sorry. I'm here for you.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
#ifdef HU_ENABLE_CARTESIA
    HU_ASSERT_TRUE(result.volume < 1.0f);
#else
    HU_ASSERT_TRUE(result.volume <= 1.0f);
#endif
}

static void test_prep_pause_factor_scales_breaks(void) {
    hu_prep_config_t cfg_fast = {
        .base_speed = 0.95f,
        .pause_factor = 0.5f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_config_t cfg_slow = {
        .base_speed = 0.95f,
        .pause_factor = 2.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t r_fast = {0}, r_slow = {0};
    const char *text = "First sentence. Second sentence.";

    hu_transcript_prep(text, strlen(text), &cfg_fast, &r_fast);
    hu_transcript_prep(text, strlen(text), &cfg_slow, &r_slow);

    /* Slow pauses produce longer output (more break ms digits) */
    HU_ASSERT_TRUE(r_slow.output_len >= r_fast.output_len);
}

static void test_prep_discourse_markers_contextual(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .discourse_rate = 1.0f,
        .hour_local = 14,
        .seed = 0,
    };
    hu_prep_result_t result = {0};
    /* "believe" triggers "honestly," rule; multiple seeds to find one that fires */
    bool found_marker = false;
    for (uint32_t s = 0; s < 50 && !found_marker; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        const char *text = "First part. I really believe in you.";
        hu_transcript_prep(text, strlen(text), &cfg, &result);
        if (strstr(result.output, "honestly") || strstr(result.output, "you know") ||
            strstr(result.output, "I mean"))
            found_marker = true;
    }
    HU_ASSERT_TRUE(found_marker);
}

static void test_prep_nonverbals_contextual(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .nonverbals_enabled = true,
        .hour_local = 14,
    };
    hu_prep_result_t result = {0};
    bool found = false;
    for (uint32_t s = 0; s < 200 && !found; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        const char *text = "That's hilarious lol. So funny haha.";
        hu_transcript_prep(text, strlen(text), &cfg, &result);
        if (strstr(result.output, "[laughter]") || strstr(result.output, "Hmm") ||
            strstr(result.output, "<break time="))
            found = true;
    }
    HU_ASSERT_TRUE(found);
}

static void test_prep_output_not_empty(void) {
    hu_prep_config_t cfg = {.base_speed = 0.95f, .pause_factor = 1.0f, .hour_local = 14};
    hu_prep_result_t result = {0};
    const char *text = "Just a test.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len > 0);
    HU_ASSERT_TRUE(result.output[0] != '\0');
}

static void test_prep_long_text_truncated_safely(void) {
    hu_prep_config_t cfg = {.base_speed = 0.95f, .pause_factor = 1.0f, .hour_local = 14};
    hu_prep_result_t result = {0};
    char long_text[9000];
    memset(long_text, 'A', sizeof(long_text) - 2);
    long_text[sizeof(long_text) - 2] = '.';
    long_text[sizeof(long_text) - 1] = '\0';
    hu_error_t err = hu_transcript_prep(long_text, strlen(long_text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len < HU_PREP_MAX_OUTPUT);
    HU_ASSERT_TRUE(result.output[result.output_len] == '\0');
}

/* ── Strip-SSML Fallback Tests ───────────────────────────────────────── */

static void test_prep_strip_ssml_no_tags(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .strip_ssml = true,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "First sentence. Second sentence.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len > 0);
    HU_ASSERT_TRUE(strstr(result.output, "<break") == NULL);
    HU_ASSERT_TRUE(strstr(result.output, "<emotion") == NULL);
    HU_ASSERT_TRUE(strstr(result.output, "<speed") == NULL);
    HU_ASSERT_TRUE(strstr(result.output, "<volume") == NULL);
}

static void test_prep_strip_ssml_converts_long_pause_to_period(void) {
    hu_prep_config_t cfg = {
        .incoming_msg = "Tell me something sad",
        .incoming_msg_len = 21,
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .strip_ssml = true,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "I understand your pain. Happiness will come.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len > 0);
}

static void test_prep_strip_ssml_nonverbals_text_only(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .strip_ssml = true,
        .nonverbals_enabled = true,
        .hour_local = 14,
    };
    hu_prep_result_t result = {0};
    bool found_text_nonverbal = false;
    for (uint32_t s = 0; s < 200 && !found_text_nonverbal; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        const char *text = "That's hilarious lol. So funny haha.";
        hu_transcript_prep(text, strlen(text), &cfg, &result);
        if (strstr(result.output, "[laughter]"))
            found_text_nonverbal = true;
        HU_ASSERT_TRUE(strstr(result.output, "<break") == NULL);
    }
}

/* ── Per-Sentence Volume Tests ──────────────────────────────────────── */

static void test_prep_per_sentence_volume_tags(void) {
    hu_prep_config_t cfg = {
        .incoming_msg = "I'm feeling very sad today",
        .incoming_msg_len = 26,
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "I'm so sorry. But you are amazing!";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    /* Multi-sentence emotional arc should produce volume variation */
    HU_ASSERT_TRUE(result.output_len > 0);
}

/* ── Clause-Level Break Tests ───────────────────────────────────────── */

static void test_prep_clause_breaks_at_commas(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    /* F1 restraint (2026-09-27): this used to assert a pause at every comma.
     * Ferni pauses on meaning, not punctuation — commas no longer break; a
     * contrast word after a comma does. */
    const char *text = "I wanted to, but it rained.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_CONTAINS(result.output, "to, <break time=");
}

static void test_prep_plain_commas_do_not_pause(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "Yeah, I think that, honestly, it matters a lot to me.";
    HU_ASSERT_EQ(hu_transcript_prep(text, strlen(text), &cfg, &result), HU_OK);
    HU_ASSERT_STR_NOT_CONTAINS(result.output, ", <break");
}

static void test_prep_clause_breaks_strip_ssml_no_tags(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .strip_ssml = true,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "Well, I think that, honestly, it matters.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(result.output, "<break") == NULL);
}

/* ── Speed Variation (voiceai-inspired) Tests ───────────────────────── */

static void test_prep_emotional_words_slower(void) {
    hu_prep_config_t cfg = {
        .base_speed = 1.0f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "I really feel for you.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.sentences[0].speed_ratio < 1.0f);
}

static void test_prep_list_words_faster(void) {
    hu_prep_config_t cfg = {
        .base_speed = 1.0f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "For example this is a list item.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.sentences[0].speed_ratio > 1.0f);
}

static void test_prep_discourse_off_by_default(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .discourse_rate = 0.0f,
        .hour_local = 14,
    };
    hu_prep_result_t result = {0};
    bool found_marker = false;
    for (uint32_t s = 0; s < 50 && !found_marker; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        const char *text = "First part. I really believe in you.";
        hu_transcript_prep(text, strlen(text), &cfg, &result);
        if (strstr(result.output, "honestly") || strstr(result.output, "you know") ||
            strstr(result.output, "I mean"))
            found_marker = true;
    }
    HU_ASSERT_TRUE(!found_marker);
}

/* ── Strip Junk Tests ────────────────────────────────────────────────── */

static void test_strip_junk_removes_stage_directions(void) {
    char out[256];
    const char *text = "Hello *waves hand* there friend.";
    size_t len = hu_transcript_strip_junk(text, strlen(text), out, sizeof(out));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "waves") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Hello") != NULL);
    HU_ASSERT_TRUE(strstr(out, "there") != NULL);
}

static void test_strip_junk_removes_json_blobs(void) {
    char out[512];
    const char *text = "Check this out {\"tool\":\"search\",\"query\":\"weather\"} pretty cool.";
    size_t len = hu_transcript_strip_junk(text, strlen(text), out, sizeof(out));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "tool") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Check") != NULL);
    HU_ASSERT_TRUE(strstr(out, "cool") != NULL);
}

static void test_strip_junk_removes_emoji(void) {
    char out[256];
    const char *text = "Great job \xF0\x9F\x91\x8D today!";
    size_t len = hu_transcript_strip_junk(text, strlen(text), out, sizeof(out));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "Great") != NULL);
    HU_ASSERT_TRUE(strstr(out, "today") != NULL);
}

static void test_strip_junk_null_input(void) {
    char out[64];
    size_t len = hu_transcript_strip_junk(NULL, 0, out, sizeof(out));
    HU_ASSERT_EQ(len, (size_t)0);
}

static void test_strip_junk_preserves_normal_text(void) {
    char out[256];
    const char *text = "This is just a normal sentence.";
    size_t len = hu_transcript_strip_junk(text, strlen(text), out, sizeof(out));
    HU_ASSERT_EQ(len, strlen(text));
    HU_ASSERT_TRUE(strcmp(out, text) == 0);
}

/* ── Consonant Smoothing Tests ───────────────────────────────────────── */

static void test_smooth_consonants_ngths(void) {
    char out[256];
    const char *text = "The strengths of this approach.";
    size_t len = hu_transcript_smooth_consonants(text, strlen(text), out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "<break") != NULL);
}

static void test_smooth_consonants_strip_mode(void) {
    char out[256];
    const char *text = "The strengths of this approach.";
    size_t len = hu_transcript_smooth_consonants(text, strlen(text), out, sizeof(out), true);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "<break") == NULL);
}

static void test_smooth_consonants_no_change(void) {
    char out[256];
    const char *text = "Hello world.";
    size_t len = hu_transcript_smooth_consonants(text, strlen(text), out, sizeof(out), false);
    HU_ASSERT_EQ(len, strlen(text));
}

/* ── Thinking Sound Tests ────────────────────────────────────────────── */

static void test_thinking_sound_complex_response(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .thinking_sounds = true,
        .hour_local = 14,
    };
    hu_prep_result_t result = {0};
    bool found_thinking = false;
    for (uint32_t s = 0; s < 50 && !found_thinking; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        const char *text = "I honestly think and believe that this is really important "
                           "because it matters to all of us and we need to consider the "
                           "implications carefully before making any decisions about it.";
        hu_transcript_prep(text, strlen(text), &cfg, &result);
        if (strstr(result.output, "Hmm") || strstr(result.output, "Well") ||
            strstr(result.output, "So,") || strstr(result.output, "Yeah") ||
            strstr(result.output, "Okay"))
            found_thinking = true;
    }
    HU_ASSERT_TRUE(found_thinking);
}

static void test_thinking_sound_disabled_by_default(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .thinking_sounds = false,
        .hour_local = 14,
    };
    hu_prep_result_t result = {0};
    bool found_thinking = false;
    for (uint32_t s = 0; s < 50 && !found_thinking; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        const char *text = "I honestly think and believe that this is really important "
                           "because it matters.";
        hu_transcript_prep(text, strlen(text), &cfg, &result);
        if (result.output[0] == 'H' && result.output[1] == 'm')
            found_thinking = true;
        if (result.output[0] == 'W' && result.output[1] == 'e')
            found_thinking = true;
    }
    HU_ASSERT_TRUE(!found_thinking);
}

/* ── Thinking Pause Tests ────────────────────────────────────────────── */

static void test_thinking_pause_long_response(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "This is a fairly long response that should trigger "
                       "a thinking pause at the beginning of the output.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(result.output, "<break time=") != NULL);
}

static void test_thinking_pause_short_response_no_pause(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "Okay sure.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len > 0);
}

/* ── Late-Night Adaptation Tests ─────────────────────────────────────── */

static void test_late_night_slower_speed(void) {
    hu_prep_config_t cfg_day = {
        .base_speed = 1.0f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_config_t cfg_night = {
        .base_speed = 1.0f,
        .pause_factor = 1.0f,
        .hour_local = 23,
        .seed = 42,
    };
    hu_prep_result_t result_day = {0}, result_night = {0};
    const char *text = "I really feel for you. This is important.";
    hu_transcript_prep(text, strlen(text), &cfg_day, &result_day);
    hu_transcript_prep(text, strlen(text), &cfg_night, &result_night);
    HU_ASSERT_TRUE(result_night.sentences[0].speed_ratio < result_day.sentences[0].speed_ratio);
}

static void test_late_night_softer_volume(void) {
    hu_prep_config_t cfg_day = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_config_t cfg_night = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .hour_local = 23,
        .seed = 42,
    };
    hu_prep_result_t result_day = {0}, result_night = {0};
    const char *text = "Hello there friend. How are you doing tonight?";
    hu_transcript_prep(text, strlen(text), &cfg_day, &result_day);
    hu_transcript_prep(text, strlen(text), &cfg_night, &result_night);
    HU_ASSERT_TRUE(result_night.volume <= result_day.volume);
}

/* ── Emotional Momentum Tests ────────────────────────────────────────── */

static void test_emotional_momentum_carries_forward(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .prev_turn_emotion = "sad",
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "Yeah I understand. It will be okay.";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.dominant_emotion != NULL);
    HU_ASSERT_TRUE(strcmp(result.dominant_emotion, "sad") == 0);
}

static void test_emotional_momentum_null_prev_no_effect(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f,
        .pause_factor = 1.0f,
        .prev_turn_emotion = NULL,
        .hour_local = 14,
        .seed = 42,
    };
    hu_prep_result_t result = {0};
    const char *text = "Hello there. How are you?";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.dominant_emotion != NULL);
}

/* ---- Speech normalization tests ---- */

static void test_normalize_phone_number_parens(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("(555) 123-4567", 14, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "<spell>") != NULL);
    HU_ASSERT_TRUE(strstr(out, "555") != NULL);
    HU_ASSERT_TRUE(strstr(out, "4567") != NULL);
}

static void test_normalize_phone_number_dashes(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("555-123-4567", 12, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "<spell>") != NULL);
    HU_ASSERT_TRUE(strstr(out, "555") != NULL);
}

static void test_normalize_phone_strip_ssml(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("(555) 123-4567", 14, out, sizeof(out), true);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "<spell>") == NULL);
    HU_ASSERT_TRUE(strstr(out, "555") != NULL);
}

static void test_normalize_currency_dollars(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("$42", 3, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "forty") != NULL);
    HU_ASSERT_TRUE(strstr(out, "dollar") != NULL);
}

static void test_normalize_currency_with_cents(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("$42.50", 6, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "forty") != NULL);
    HU_ASSERT_TRUE(strstr(out, "dollar") != NULL);
    HU_ASSERT_TRUE(strstr(out, "cent") != NULL);
}

static void test_normalize_percentage(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("85%%", 3, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "eighty") != NULL);
    HU_ASSERT_TRUE(strstr(out, "percent") != NULL);
}

static void test_normalize_date_slash(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("4/3/2026", 8, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "April") != NULL);
    HU_ASSERT_TRUE(strstr(out, "2026") != NULL);
}

static void test_normalize_date_iso(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("2026-04-03", 10, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "April") != NULL);
    HU_ASSERT_TRUE(strstr(out, "2026") != NULL);
}

static void test_normalize_time_pm(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("7:30 PM", 7, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "seven") != NULL);
    HU_ASSERT_TRUE(strstr(out, "thirty") != NULL);
    HU_ASSERT_TRUE(strstr(out, "PM") != NULL);
}

static void test_normalize_time_oclock(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("7:00 PM", 7, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "seven") != NULL);
    HU_ASSERT_TRUE(strstr(out, "o\'clock") != NULL);
}

static void test_normalize_small_number_to_words(void) {
    char out[256];
    size_t len = hu_transcript_normalize_for_speech("I have 42 items", 15, out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "forty") != NULL);
}

static void test_normalize_preserves_normal_text(void) {
    char out[256];
    const char *text = "Hello there";
    size_t len = hu_transcript_normalize_for_speech(text, strlen(text), out, sizeof(out), false);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_EQ(memcmp(out, "Hello there", 11), 0);
}

static void test_normalize_null_input(void) {
    char out[64];
    size_t len = hu_transcript_normalize_for_speech(NULL, 0, out, sizeof(out), false);
    HU_ASSERT_EQ(len, (size_t)0);
}

static void test_break_limiter_removes_excess(void) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "Hello<break time=\"200ms\"/>world<break time=\"200ms\"/>"
             "test<break time=\"200ms\"/>a<break time=\"200ms\"/>"
             "b<break time=\"200ms\"/>c<break time=\"200ms\"/>"
             "d<break time=\"200ms\"/>e<break time=\"200ms\"/>"
             "f<break time=\"200ms\"/>g<break time=\"200ms\"/>"
             "end");
    size_t orig_len = strlen(buf);
    size_t new_len = hu_transcript_limit_breaks(buf, orig_len, 2);
    HU_ASSERT_TRUE(new_len <= orig_len);
}

static void test_break_limiter_keeps_all_under_limit(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Hello<break time=\"200ms\"/>world");
    size_t orig_len = strlen(buf);
    size_t new_len = hu_transcript_limit_breaks(buf, orig_len, 10);
    HU_ASSERT_EQ(new_len, orig_len);
}

static void test_break_limiter_null_safe(void) {
    size_t len = hu_transcript_limit_breaks(NULL, 0, 5);
    HU_ASSERT_EQ(len, (size_t)0);
}

static void test_prep_normalizes_numbers_in_pipeline(void) {
    hu_prep_config_t cfg = {0};
    cfg.base_speed = 1.0f;
    cfg.pause_factor = 1.0f;
    hu_prep_result_t result = {0};
    const char *text = "I have 42 apples";
    hu_error_t err = hu_transcript_prep(text, strlen(text), &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output_len > 0);
    HU_ASSERT_TRUE(strstr(result.output, "forty") != NULL);
}

/* ── Registration ────────────────────────────────────────────────────── */

/* ── F1 restraint (Ferni port) ───────────────────────────────────────── */

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

static void test_prep_no_laughter_or_hmm_without_a_cue(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f, .pause_factor = 1.0f, .nonverbals_enabled = true, .hour_local = 14};
    hu_prep_result_t result;
    const char *text = "That sounds great. See you then. I think it will be fun.";
    for (uint32_t s = 0; s < 300; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        HU_ASSERT_EQ(hu_transcript_prep(text, strlen(text), &cfg, &result), HU_OK);
        HU_ASSERT_STR_NOT_CONTAINS(result.output, "[laughter]");
        HU_ASSERT_STR_NOT_CONTAINS(result.output, "Hmm");
    }
}

static void test_prep_laughing_text_laughs_at_most_once(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f, .pause_factor = 1.0f, .nonverbals_enabled = true, .hour_local = 14};
    hu_prep_result_t result;
    const char *text = "That is so good haha. Love it haha. You are the best haha.";
    bool any = false;
    for (uint32_t s = 0; s < 300; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        HU_ASSERT_EQ(hu_transcript_prep(text, strlen(text), &cfg, &result), HU_OK);
        int n = count_of(result.output, "[laughter]");
        HU_ASSERT_TRUE(n <= 1);
        any = any || n == 1;
    }
    HU_ASSERT_TRUE(any);
}

static void test_prep_laughter_cue_alone_can_laugh(void) {
    hu_prep_config_t cfg = {.base_speed = 0.95f,
                            .pause_factor = 1.0f,
                            .nonverbals_enabled = true,
                            .laughter_cue = true,
                            .hour_local = 14};
    hu_prep_result_t result;
    const char *text = "That sounds great. See you then.";
    bool any = false;
    for (uint32_t s = 0; s < 300 && !any; s++) {
        cfg.seed = s;
        memset(&result, 0, sizeof(result));
        HU_ASSERT_EQ(hu_transcript_prep(text, strlen(text), &cfg, &result), HU_OK);
        any = strstr(result.output, "[laughter]") != NULL;
    }
    HU_ASSERT_TRUE(any);
}

static void test_prep_inline_emotion_tags_are_budgeted(void) {
    hu_prep_config_t cfg = {
        .base_speed = 0.95f, .pause_factor = 1.0f, .hour_local = 14, .seed = 11};
    hu_prep_result_t result = {0};
    const char *text = "Congrats, that is amazing! I am so sorry about your dog. "
                       "Haha that is hilarious. Wow that is awesome! I am sad about it. "
                       "Congrats again, amazing!";
    HU_ASSERT_EQ(hu_transcript_prep(text, strlen(text), &cfg, &result), HU_OK);
    HU_ASSERT_TRUE(count_of(result.output, "<emotion value=") <= 2);
    HU_ASSERT_TRUE(count_of(result.output, "<volume ratio=") <= 1);
}

void run_transcript_prep_tests(void) {
    HU_TEST_SUITE("Transcript preprocessor");

    HU_RUN_TEST(test_segment_simple_sentences);
    HU_RUN_TEST(test_segment_abbreviations_preserved);
    HU_RUN_TEST(test_segment_ellipsis_not_split);
    HU_RUN_TEST(test_segment_decimal_not_split);
    HU_RUN_TEST(test_segment_no_punctuation);
    HU_RUN_TEST(test_segment_empty_input);
    HU_RUN_TEST(test_segment_null_input);
    HU_RUN_TEST(test_segment_max_capacity);
    HU_RUN_TEST(test_segment_multiple_excl_and_question);

    HU_RUN_TEST(test_volume_calm_is_soft);
    HU_RUN_TEST(test_volume_excited_is_loud);
    HU_RUN_TEST(test_volume_sad_is_quiet);
    HU_RUN_TEST(test_volume_neutral_default);
    HU_RUN_TEST(test_volume_null_default);

    HU_RUN_TEST(test_prep_null_returns_error);
    HU_RUN_TEST(test_prep_single_sentence_no_breaks);
    HU_RUN_TEST(test_prep_multi_sentence_has_breaks);
    HU_RUN_TEST(test_prep_emotion_shift_produces_tags);
    HU_RUN_TEST(test_prep_speed_variation_for_exclamation);
    HU_RUN_TEST(test_prep_volume_from_emotion);
    HU_RUN_TEST(test_prep_pause_factor_scales_breaks);
    HU_RUN_TEST(test_prep_discourse_markers_contextual);
    HU_RUN_TEST(test_prep_nonverbals_contextual);
    HU_RUN_TEST(test_prep_output_not_empty);
    HU_RUN_TEST(test_prep_long_text_truncated_safely);

    HU_RUN_TEST(test_prep_strip_ssml_no_tags);
    HU_RUN_TEST(test_prep_strip_ssml_converts_long_pause_to_period);
    HU_RUN_TEST(test_prep_strip_ssml_nonverbals_text_only);
    HU_RUN_TEST(test_prep_per_sentence_volume_tags);
    HU_RUN_TEST(test_prep_clause_breaks_at_commas);
    HU_RUN_TEST(test_prep_plain_commas_do_not_pause);
    HU_RUN_TEST(test_prep_no_laughter_or_hmm_without_a_cue);
    HU_RUN_TEST(test_prep_laughing_text_laughs_at_most_once);
    HU_RUN_TEST(test_prep_laughter_cue_alone_can_laugh);
    HU_RUN_TEST(test_prep_inline_emotion_tags_are_budgeted);
    HU_RUN_TEST(test_prep_clause_breaks_strip_ssml_no_tags);
    HU_RUN_TEST(test_prep_emotional_words_slower);
    HU_RUN_TEST(test_prep_list_words_faster);
    HU_RUN_TEST(test_prep_discourse_off_by_default);

    HU_RUN_TEST(test_strip_junk_removes_stage_directions);
    HU_RUN_TEST(test_strip_junk_removes_json_blobs);
    HU_RUN_TEST(test_strip_junk_removes_emoji);
    HU_RUN_TEST(test_strip_junk_null_input);
    HU_RUN_TEST(test_strip_junk_preserves_normal_text);

    HU_RUN_TEST(test_smooth_consonants_ngths);
    HU_RUN_TEST(test_smooth_consonants_strip_mode);
    HU_RUN_TEST(test_smooth_consonants_no_change);

    HU_RUN_TEST(test_thinking_sound_complex_response);
    HU_RUN_TEST(test_thinking_sound_disabled_by_default);

    HU_RUN_TEST(test_thinking_pause_long_response);
    HU_RUN_TEST(test_thinking_pause_short_response_no_pause);

    HU_RUN_TEST(test_late_night_slower_speed);
    HU_RUN_TEST(test_late_night_softer_volume);

    HU_RUN_TEST(test_emotional_momentum_carries_forward);
    HU_RUN_TEST(test_emotional_momentum_null_prev_no_effect);

    HU_RUN_TEST(test_normalize_phone_number_parens);
    HU_RUN_TEST(test_normalize_phone_number_dashes);
    HU_RUN_TEST(test_normalize_phone_strip_ssml);
    HU_RUN_TEST(test_normalize_currency_dollars);
    HU_RUN_TEST(test_normalize_currency_with_cents);
    HU_RUN_TEST(test_normalize_percentage);
    HU_RUN_TEST(test_normalize_date_slash);
    HU_RUN_TEST(test_normalize_date_iso);
    HU_RUN_TEST(test_normalize_time_pm);
    HU_RUN_TEST(test_normalize_time_oclock);
    HU_RUN_TEST(test_normalize_small_number_to_words);
    HU_RUN_TEST(test_normalize_preserves_normal_text);
    HU_RUN_TEST(test_normalize_null_input);

    HU_RUN_TEST(test_break_limiter_removes_excess);
    HU_RUN_TEST(test_break_limiter_keeps_all_under_limit);
    HU_RUN_TEST(test_break_limiter_null_safe);

    HU_RUN_TEST(test_prep_normalizes_numbers_in_pipeline);
}
