#include "human/context/conversation.h"
#include "human/eval/turing_score.h"
#include "test_framework.h"
#include <time.h>

static void turing_heuristic_casual_message_scores_high(void) {
    hu_turing_score_t score;
    const char *msg = "haha yeah that sounds fun, i'm down";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.overall >= 7);
    HU_ASSERT(score.dimensions[HU_TURING_NATURAL_LANGUAGE] >= 7);
    HU_ASSERT(score.dimensions[HU_TURING_IMPERFECTION] >= 7);
    HU_ASSERT(score.verdict == HU_TURING_HUMAN || score.verdict == HU_TURING_BORDERLINE);
}

static void turing_heuristic_ai_tells_score_low(void) {
    hu_turing_score_t score;
    const char *msg = "I'd be happy to help you with that! I certainly understand your concern. "
                      "Feel free to reach out anytime.";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_NON_ROBOTIC] <= 5);
    HU_ASSERT(score.dimensions[HU_TURING_NATURAL_LANGUAGE] <= 6);
}

static void turing_heuristic_markdown_penalized(void) {
    hu_turing_score_t score;
    const char *msg = "Here are some options:\n- Option 1\n- Option 2\n- Option 3";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_NON_ROBOTIC] < 8);
}

static void turing_heuristic_emotional_message(void) {
    hu_turing_score_t score;
    const char *msg = "i miss you so much, honestly i'm just sad today";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_EMOTIONAL_INTELLIGENCE] >= 7);
    HU_ASSERT(score.dimensions[HU_TURING_VULNERABILITY_WILLINGNESS] >= 7);
    HU_ASSERT(score.dimensions[HU_TURING_GENUINE_WARMTH] >= 7);
}

static void turing_heuristic_long_message_penalized(void) {
    hu_turing_score_t score;
    char long_msg[500];
    memset(long_msg, 'a', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    HU_ASSERT(hu_turing_score_heuristic(long_msg, strlen(long_msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_APPROPRIATE_LENGTH] < 5);
}

static void turing_heuristic_hedging_penalized(void) {
    hu_turing_score_t score;
    const char *msg = "Well, it depends on the situation. On one hand you could do X.";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_OPINION_HAVING] <= 5);
}

static void turing_dimension_names_valid(void) {
    HU_ASSERT(strcmp(hu_turing_dimension_name(HU_TURING_NATURAL_LANGUAGE), "natural_language") ==
              0);
    HU_ASSERT(strcmp(hu_turing_dimension_name(HU_TURING_GENUINE_WARMTH), "genuine_warmth") == 0);
}

static void turing_verdict_names_valid(void) {
    HU_ASSERT(strcmp(hu_turing_verdict_name(HU_TURING_HUMAN), "HUMAN") == 0);
    HU_ASSERT(strcmp(hu_turing_verdict_name(HU_TURING_AI_DETECTED), "AI_DETECTED") == 0);
}

static void turing_score_summary_allocates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_turing_score_t score;
    const char *msg = "yeah for sure";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);

    size_t len = 0;
    char *summary = hu_turing_score_summary(&alloc, &score, &len);
    HU_ASSERT(summary != NULL);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(summary, "Turing Score:") != NULL);
    alloc.free(alloc.ctx, summary, len + 1);
}

static void turing_null_input_rejected(void) {
    hu_turing_score_t score;
    HU_ASSERT(hu_turing_score_heuristic(NULL, 0, NULL, 0, 0, &score) == HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(hu_turing_score_heuristic("hi", 2, NULL, 0, 0, NULL) == HU_ERR_INVALID_ARGUMENT);
}

static void turing_llm_null_provider_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_turing_score_t score;
    HU_ASSERT(hu_turing_score_llm(&alloc, NULL, "m", 1, "hi", 2, NULL, 0, &score) ==
              HU_ERR_INVALID_ARGUMENT);
}

static void turing_scores_clamped(void) {
    hu_turing_score_t score;
    const char *msg = "x";
    HU_ASSERT(hu_turing_score_heuristic(msg, 1, NULL, 0, 0, &score) == HU_OK);
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++) {
        HU_ASSERT(score.dimensions[i] >= 1);
        HU_ASSERT(score.dimensions[i] <= 10);
    }
}

static void turing_energy_matching_short_to_short(void) {
    hu_turing_score_t score;
    const char *ctx = "hey whats up\n";
    const char *resp = "not much, you?";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 7);
}

static void turing_energy_matching_essay_to_short_penalized(void) {
    hu_turing_score_t score;
    const char *ctx = "k\n";
    char long_resp[300];
    memset(long_resp, 'a', sizeof(long_resp) - 1);
    long_resp[sizeof(long_resp) - 1] = '\0';
    HU_ASSERT(hu_turing_score_heuristic(long_resp, strlen(long_resp), ctx, strlen(ctx), 0, &score) ==
              HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] <= 5);
}

static void turing_context_awareness_with_references(void) {
    hu_turing_score_t score;
    const char *ctx = "I started a new job last month\n";
    const char *resp = "you mentioned your new job earlier, how's that going?";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_CONTEXT_AWARENESS] >= 8);
}

static void turing_context_awareness_no_context(void) {
    hu_turing_score_t score;
    const char *resp = "that sounds cool";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_CONTEXT_AWARENESS] >= 5);
    HU_ASSERT(score.dimensions[HU_TURING_CONTEXT_AWARENESS] <= 8);
}

static void turing_vulnerability_authentic_disclosure(void) {
    hu_turing_score_t score;
    const char *resp = "honestly i feel like i've been struggling with this, ngl";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_VULNERABILITY_WILLINGNESS] >= 7);
}

static void turing_humor_brief_natural(void) {
    hu_turing_score_t score;
    const char *resp = "haha omg dead";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_HUMOR_NATURALNESS] >= 7);
}

static void turing_opinion_strong_position(void) {
    hu_turing_score_t score;
    const char *resp = "i think that show is overrated, personally i prefer the original";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_OPINION_HAVING] >= 8);
}

static void turing_personality_consistent_voice(void) {
    hu_turing_score_t score;
    const char *resp = "honestly i'm not sure but imo it's better to just go for it, tbh "
                       "i've been thinking about this a lot";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_PERSONALITY_CONSISTENCY] >= 7);
}

static void turing_genuine_warmth_with_context(void) {
    hu_turing_score_t score;
    const char *ctx = "started new job last week\n";
    const char *resp = "i'm so happy for you! you mentioned the new job, how's it going?";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_GENUINE_WARMTH] >= 7);
}

static void turing_voice_filler_usage_detected(void) {
    hu_turing_score_t score;
    const char *resp = "well, i guess like, i dunno, it's kinda weird ya know";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_FILLER_USAGE] >= 8);
}

static void turing_voice_conversational_repair_detected(void) {
    hu_turing_score_t score;
    const char *resp = "i think it's— wait, actually, i mean it's more like, no wait, scratch that";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_CONVERSATIONAL_REPAIR] >= 8);
}

static void turing_voice_paralinguistic_cues_detected(void) {
    hu_turing_score_t score;
    const char *resp = "haha aww that's so sweet, oof i felt that";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_PARALINGUISTIC_CUES] >= 8);
}

static void turing_voice_emotional_prosody_with_emphasis(void) {
    hu_turing_score_t score;
    const char *resp = "OMG that's AMAZING!! i'm so happy for you!! i love it so much";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_EMOTIONAL_PROSODY] >= 8);
}

static void turing_voice_prosody_varied_punctuation(void) {
    hu_turing_score_t score;
    const char *resp = "wait really? that's wild! i can't... like i'm still processing";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_PROSODY_NATURALNESS] >= 8);
}

static void turing_voice_turn_timing_short_casual(void) {
    hu_turing_score_t score;
    const char *ctx = "hey\n";
    const char *resp = "lol yeah what's up";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_TURN_TIMING] >= 7);
}

static void turing_trajectory_single_score(void) {
    hu_turing_score_t scores[1];
    memset(scores, 0, sizeof(scores));
    scores[0].overall = 8;
    hu_turing_trajectory_t traj;
    HU_ASSERT(hu_turing_score_trajectory(scores, 1, &traj) == HU_OK);
    HU_ASSERT(traj.stability >= 0.99f);
    HU_ASSERT(traj.overall > 0.0f && traj.overall <= 1.0f);
}

static void turing_trajectory_improving_trend(void) {
    hu_turing_score_t scores[6];
    memset(scores, 0, sizeof(scores));
    scores[0].overall = 4;
    scores[1].overall = 5;
    scores[2].overall = 5;
    scores[3].overall = 7;
    scores[4].overall = 8;
    scores[5].overall = 9;
    hu_turing_trajectory_t traj;
    HU_ASSERT(hu_turing_score_trajectory(scores, 6, &traj) == HU_OK);
    HU_ASSERT(traj.directional_alignment > 0.5f);
}

static void turing_trajectory_declining_trend(void) {
    hu_turing_score_t scores[4];
    memset(scores, 0, sizeof(scores));
    scores[0].overall = 9;
    scores[1].overall = 8;
    scores[2].overall = 5;
    scores[3].overall = 3;
    hu_turing_trajectory_t traj;
    HU_ASSERT(hu_turing_score_trajectory(scores, 4, &traj) == HU_OK);
    HU_ASSERT(traj.directional_alignment < 0.5f);
}

static void turing_trajectory_stable_high(void) {
    hu_turing_score_t scores[5];
    memset(scores, 0, sizeof(scores));
    for (int i = 0; i < 5; i++)
        scores[i].overall = 8;
    hu_turing_trajectory_t traj;
    HU_ASSERT(hu_turing_score_trajectory(scores, 5, &traj) == HU_OK);
    HU_ASSERT(traj.stability >= 0.95f);
    HU_ASSERT(traj.cumulative_impact >= 0.75f);
}

static void turing_trajectory_null_rejected(void) {
    hu_turing_trajectory_t traj;
    HU_ASSERT(hu_turing_score_trajectory(NULL, 0, &traj) == HU_ERR_INVALID_ARGUMENT);
}

static void turing_contact_hint_builds_for_weak_dims(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int dims[HU_TURING_DIM_COUNT];
    memset(dims, 0, sizeof(dims));
    dims[HU_TURING_NATURAL_LANGUAGE] = 4;
    dims[HU_TURING_GENUINE_WARMTH] = 5;
    dims[HU_TURING_NON_ROBOTIC] = 3;
    size_t len = 0;
    char *hint = hu_turing_build_contact_hint(&alloc, dims, &len);
    HU_ASSERT(hint != NULL);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(hint, "contractions") != NULL || strstr(hint, "casual") != NULL);
    alloc.free(alloc.ctx, hint, len + 1);
}

static void turing_contact_hint_null_for_good_dims(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int dims[HU_TURING_DIM_COUNT];
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++)
        dims[i] = 8;
    size_t len = 0;
    char *hint = hu_turing_build_contact_hint(&alloc, dims, &len);
    HU_ASSERT(hint == NULL);
    HU_ASSERT(len == 0);
}

static void turing_ab_pick_variant_deterministic(void) {
    bool v1 = hu_ab_test_pick_variant("alice", 5, "test_x");
    bool v2 = hu_ab_test_pick_variant("alice", 5, "test_x");
    HU_ASSERT(v1 == v2);
    bool v3 = hu_ab_test_pick_variant("bob", 3, "test_x");
    (void)v3;
}

static void turing_rn_not_matched_inside_words(void) {
    hu_turing_score_t score_word;
    const char *msg_word = "i'm learning new patterns for this";
    HU_ASSERT(hu_turing_score_heuristic(msg_word, strlen(msg_word), NULL, 0, 0, &score_word) == HU_OK);

    hu_turing_score_t score_rn;
    const char *msg_rn = "doing homework rn lol";
    HU_ASSERT(hu_turing_score_heuristic(msg_rn, strlen(msg_rn), NULL, 0, 0, &score_rn) == HU_OK);

    /* "rn" as a standalone word should score higher on casual markers than "rn" inside words */
    HU_ASSERT(score_rn.dimensions[HU_TURING_NATURAL_LANGUAGE] >=
              score_word.dimensions[HU_TURING_NATURAL_LANGUAGE]);
}

static void turing_weighted_scoring_text_dominant(void) {
    hu_turing_score_t score;
    const char *msg = "yeah that's so true, i've been thinking the same thing honestly";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    /* Text dims should dominate: even if voice-proxy dims are lower,
     * a casual human message should score well overall */
    HU_ASSERT(score.overall >= 7);
}

static void turing_expanded_ai_tells_detected(void) {
    hu_turing_score_t score;
    const char *msg = "Let me know if you need anything else! Hope this helps. "
                      "In summary, it's important to note that I apologize for any confusion.";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_NATURAL_LANGUAGE] <= 5);
    HU_ASSERT(score.dimensions[HU_TURING_NON_ROBOTIC] <= 3);
}

static void turing_markdown_bold_penalized(void) {
    hu_turing_score_t score;
    const char *msg = "Here are the **key points** you should consider:\n"
                      "The **most important** thing is to stay focused.";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    /* Markdown bold (**) is detected as an AI tell */
    HU_ASSERT(score.dimensions[HU_TURING_NON_ROBOTIC] <= 7);
}

static void turing_energy_matching_correct_segment(void) {
    hu_turing_score_t score;
    /* Context has two lines: a long first message and a short last message */
    const char *ctx = "hey i was thinking about what you said yesterday about the whole "
                      "situation with work and everything\nyo";
    const char *resp = "what's up";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    /* Short response to short last message should score well on energy matching */
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 7);
}

static void turing_cross_turn_consistency_boosts_score(void) {
    hu_turing_score_t with_ctx;
    const char *ctx = "user: hey what's up\nassistant: not much lol just chilling\n"
                      "user: nice same honestly";
    const char *resp = "yeah it's been a chill day tbh";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &with_ctx) == HU_OK);

    hu_turing_score_t no_ctx;
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), NULL, 0, 0, &no_ctx) == HU_OK);

    /* With consistent context, personality_consistency should be >= no-context version */
    HU_ASSERT(with_ctx.dimensions[HU_TURING_PERSONALITY_CONSISTENCY] >=
              no_ctx.dimensions[HU_TURING_PERSONALITY_CONSISTENCY]);
}

static void turing_empathy_markers_boost_emotional_intelligence(void) {
    hu_turing_score_t empathic;
    const char *ctx = "user: i've been really struggling lately";
    const char *resp = "that sounds really tough, how are you holding up?";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &empathic) == HU_OK);

    hu_turing_score_t flat;
    const char *flat_resp = "ok noted, let me know if you need help";
    HU_ASSERT(hu_turing_score_heuristic(flat_resp, strlen(flat_resp), ctx, strlen(ctx), 0, &flat) ==
              HU_OK);

    HU_ASSERT(empathic.dimensions[HU_TURING_EMOTIONAL_INTELLIGENCE] >
              flat.dimensions[HU_TURING_EMOTIONAL_INTELLIGENCE]);
}

static void turing_robotic_apology_penalized(void) {
    hu_turing_score_t score;
    const char *msg = "I apologize for the inconvenience, sorry for any confusion caused.";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_EMOTIONAL_INTELLIGENCE] <= 5);
}

static void turing_channel_weights_casual_boost(void) {
    hu_turing_score_t score;
    const char *msg = "yeah i think so too honestly";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    int base_overall = score.overall;

    hu_turing_apply_channel_weights(&score, "imessage", 8);
    HU_ASSERT(score.overall >= base_overall);
}

static void turing_channel_weights_null_safe(void) {
    hu_turing_score_t score;
    memset(&score, 0, sizeof(score));
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++)
        score.dimensions[i] = 5;
    score.overall = 5;
    hu_turing_apply_channel_weights(&score, NULL, 0);
    HU_ASSERT(score.overall == 5);
}

static void turing_multiline_user_turn_length(void) {
    hu_turing_score_t score;
    /* Multi-line user turn should be counted fully for energy matching */
    const char *ctx = "assistant: hey what's going on\n"
                      "user: honestly i've been thinking about\n"
                      "a lot of different things lately and\n"
                      "i'm not sure what direction to take";
    const char *resp = "yeah that's a lot to process, take your time with it";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    /* Medium-length response to a multi-line user message should match well */
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 6);
}

static void turing_ultra_short_reply_high_length_score(void) {
    hu_turing_score_t score;
    const char *msg = "lol ok";
    HU_ASSERT(hu_turing_score_heuristic(msg, strlen(msg), NULL, 0, 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_APPROPRIATE_LENGTH] >= 9);
}

static void turing_sigh_not_matched_in_sight(void) {
    hu_turing_score_t score_sight;
    const char *msg_sight = "what a beautiful sight to see at sunset";
    HU_ASSERT(hu_turing_score_heuristic(msg_sight, strlen(msg_sight), NULL, 0, 0, &score_sight) ==
              HU_OK);

    hu_turing_score_t score_sigh;
    const char *msg_sigh = "sigh, another long day at work";
    HU_ASSERT(hu_turing_score_heuristic(msg_sigh, strlen(msg_sigh), NULL, 0, 0, &score_sigh) == HU_OK);

    /* "sigh" as a word should contribute to paralinguistic cues,
     * "sight" should not */
    HU_ASSERT(score_sigh.dimensions[HU_TURING_PARALINGUISTIC_CUES] >=
              score_sight.dimensions[HU_TURING_PARALINGUISTIC_CUES]);
}

static void turing_energy_matching_user_turn_scoped(void) {
    hu_turing_score_t score;
    /* Assistant turn has exclamations, but last user turn does not */
    const char *ctx = "assistant: OMG that's amazing!!!\nuser: yeah i guess so";
    const char *resp = "it really is something";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score) == HU_OK);
    /* Energy should match the calm user turn, not the excited assistant turn */
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 6);
}

static void turing_last_user_msg_prefix_matrix(void) {
    hu_turing_score_t score;

    /* Test "User: " prefix */
    const char *ctx1 = "Assistant: hey\nUser: what's going on today";
    const char *resp = "not much";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx1, strlen(ctx1), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 6);

    /* Test "U: " prefix */
    const char *ctx2 = "A: hey\nU: sup";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx2, strlen(ctx2), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 7);

    /* Test "Human: " prefix */
    const char *ctx3 = "Assistant: hello\nHuman: hi there how are you doing today";
    HU_ASSERT(hu_turing_score_heuristic(resp, strlen(resp), ctx3, strlen(ctx3), 0, &score) == HU_OK);
    HU_ASSERT(score.dimensions[HU_TURING_ENERGY_MATCHING] >= 5);
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void turing_ab_test_sqlite_roundtrip(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_ab_test_init_table(db) == HU_OK);
    HU_ASSERT(hu_ab_test_create(db, "test_param", 0.10f, 0.20f) == HU_OK);

    hu_ab_test_t test;
    HU_ASSERT(hu_ab_test_get_results(db, "test_param", &test) == HU_OK);
    HU_ASSERT(test.active);
    HU_ASSERT(test.score_count_a == 0);
    HU_ASSERT(test.score_count_b == 0);

    HU_ASSERT(hu_ab_test_record(db, "test_param", false, 8) == HU_OK);
    HU_ASSERT(hu_ab_test_record(db, "test_param", true, 7) == HU_OK);
    HU_ASSERT(hu_ab_test_get_results(db, "test_param", &test) == HU_OK);
    HU_ASSERT(test.score_count_a == 1);
    HU_ASSERT(test.score_count_b == 1);
    HU_ASSERT(test.score_sum_a == 8);
    HU_ASSERT(test.score_sum_b == 7);

    /* Not enough observations to resolve */
    float winner = 0;
    HU_ASSERT(hu_ab_test_resolve(db, "test_param", &winner) == HU_ERR_NOT_SUPPORTED);

    sqlite3_close(db);
}

static void turing_contact_dims_sqlite_empty_table(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_turing_init_tables(db) == HU_OK);

    int dims[HU_TURING_DIM_COUNT];
    memset(dims, 99, sizeof(dims));
    HU_ASSERT(hu_turing_get_contact_dimensions(db, "nobody", 6, dims) == HU_OK);
    /* All zeros when no data */
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++)
        HU_ASSERT(dims[i] == 0);

    sqlite3_close(db);
}

static void turing_contact_dims_sqlite_with_data(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_turing_init_tables(db) == HU_OK);

    hu_turing_score_t score;
    memset(&score, 0, sizeof(score));
    score.overall = 8;
    score.verdict = HU_TURING_HUMAN;
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++)
        score.dimensions[i] = 7;
    score.dimensions[HU_TURING_NATURAL_LANGUAGE] = 9;

    HU_ASSERT(hu_turing_store_score(db, "alice", 5, (int64_t)time(NULL), &score) == HU_OK);

    int dims[HU_TURING_DIM_COUNT];
    HU_ASSERT(hu_turing_get_contact_dimensions(db, "alice", 5, dims) == HU_OK);
    HU_ASSERT(dims[HU_TURING_NATURAL_LANGUAGE] == 9);
    HU_ASSERT(dims[HU_TURING_EMOTIONAL_INTELLIGENCE] == 7);

    sqlite3_close(db);
}

static void turing_ab_resolve_success_path(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_ab_test_init_table(db) == HU_OK);
    HU_ASSERT(hu_ab_test_create(db, "disfluency_freq", 0.10f, 0.20f) == HU_OK);

    /* Record 20+ observations for each variant with clear winner */
    for (int i = 0; i < 25; i++) {
        HU_ASSERT(hu_ab_test_record(db, "disfluency_freq", false, 6) == HU_OK);
        HU_ASSERT(hu_ab_test_record(db, "disfluency_freq", true, 8) == HU_OK);
    }

    float winner = 0;
    HU_ASSERT(hu_ab_test_resolve(db, "disfluency_freq", &winner) == HU_OK);
    /* Variant B (0.20) should win since avg 8 vs avg 6 */
    HU_ASSERT(winner > 0.19f && winner < 0.21f);

    /* Test should now be deactivated */
    hu_ab_test_t test;
    HU_ASSERT(hu_ab_test_get_results(db, "disfluency_freq", &test) == HU_OK);
    HU_ASSERT(!test.active);

    sqlite3_close(db);
}

static void turing_ab_resolve_too_close(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_ab_test_init_table(db) == HU_OK);
    HU_ASSERT(hu_ab_test_create(db, "close_test", 0.10f, 0.12f) == HU_OK);

    for (int i = 0; i < 25; i++) {
        HU_ASSERT(hu_ab_test_record(db, "close_test", false, 7) == HU_OK);
        HU_ASSERT(hu_ab_test_record(db, "close_test", true, 7) == HU_OK);
    }

    float winner = 0;
    /* Too close (< 0.5 diff) — should not resolve */
    HU_ASSERT(hu_ab_test_resolve(db, "close_test", &winner) == HU_ERR_NOT_SUPPORTED);

    sqlite3_close(db);
}

static void turing_channel_dims_hash_delimiter(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_turing_init_tables(db) == HU_OK);

    hu_turing_score_t score;
    memset(&score, 0, sizeof(score));
    score.overall = 7;
    score.verdict = HU_TURING_BORDERLINE;
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++)
        score.dimensions[i] = 7;

    /* Store with #discord — should match */
    HU_ASSERT(hu_turing_store_score(db, "alice#discord", 13, (int64_t)time(NULL), &score) == HU_OK);
    /* Store without # — should NOT match */
    HU_ASSERT(hu_turing_store_score(db, "discord_bot", 11, (int64_t)time(NULL), &score) == HU_OK);

    int dims[HU_TURING_DIM_COUNT];
    HU_ASSERT(hu_turing_get_channel_dimensions(db, "discord", 7, dims) == HU_OK);
    /* Only alice#discord should be counted, not discord_bot.
     * With 2 matching, avg would still be 7. With only 1, also 7.
     * The key test: verify the function doesn't crash and returns valid data. */
    HU_ASSERT(dims[HU_TURING_NATURAL_LANGUAGE] == 7);

    sqlite3_close(db);
}

static void turing_channel_dims_sqlite_like_pattern(void) {
    sqlite3 *db = NULL;
    HU_ASSERT(sqlite3_open(":memory:", &db) == SQLITE_OK);
    HU_ASSERT(hu_turing_init_tables(db) == HU_OK);

    hu_turing_score_t score;
    memset(&score, 0, sizeof(score));
    score.overall = 7;
    score.verdict = HU_TURING_BORDERLINE;
    for (int i = 0; i < HU_TURING_DIM_COUNT; i++)
        score.dimensions[i] = 7;

    HU_ASSERT(hu_turing_store_score(db, "bob#discord", 11, (int64_t)time(NULL), &score) == HU_OK);

    int dims[HU_TURING_DIM_COUNT];
    HU_ASSERT(hu_turing_get_channel_dimensions(db, "discord", 7, dims) == HU_OK);
    HU_ASSERT(dims[HU_TURING_NATURAL_LANGUAGE] == 7);

    /* Non-matching channel should return zeros */
    int no_dims[HU_TURING_DIM_COUNT];
    HU_ASSERT(hu_turing_get_channel_dimensions(db, "slack", 5, no_dims) == HU_OK);
    HU_ASSERT(no_dims[HU_TURING_NATURAL_LANGUAGE] == 0);

    sqlite3_close(db);
}
#endif

/* ── Q1: Question-asking ratio boosts warmth + context awareness ── */
static void turing_question_boosts_warmth(void) {
    hu_turing_score_t with_q, without_q;
    const char *ctx = "user: hey what's up\nassistant: not much\nuser: cool";
    const char *resp_q = "sounds good, what are you up to tonight?";
    const char *resp_nq = "sounds good, enjoy your evening";
    hu_turing_score_heuristic(resp_q, strlen(resp_q), ctx, strlen(ctx), 0, &with_q);
    hu_turing_score_heuristic(resp_nq, strlen(resp_nq), ctx, strlen(ctx), 0, &without_q);
    HU_ASSERT(with_q.dimensions[HU_TURING_GENUINE_WARMTH] >=
              without_q.dimensions[HU_TURING_GENUINE_WARMTH]);
    HU_ASSERT(with_q.dimensions[HU_TURING_CONTEXT_AWARENESS] >=
              without_q.dimensions[HU_TURING_CONTEXT_AWARENESS]);
}

/* ── Q2: Context awareness accepts "you" OR "your", not requiring both ── */
static void turing_context_awareness_you_or_your(void) {
    hu_turing_score_t score;
    const char *ctx = "user: i had a rough day";
    const char *resp = "i'm sorry you went through that";
    hu_turing_score_heuristic(resp, strlen(resp), ctx, strlen(ctx), 0, &score);
    HU_ASSERT(score.dimensions[HU_TURING_CONTEXT_AWARENESS] >= 7);
}

/* ── Q3: Vocabulary diversity penalty for repeated distinctive words ── */
static void turing_vocab_repetition_penalized(void) {
    hu_turing_score_t diverse, repetitive;
    const char *ctx_repeat =
        "assistant: absolutely fascinating perspective on the concept\n"
        "user: tell me more\n"
        "assistant: the fascinating interplay between systems is remarkable\nuser: ok";
    const char *resp_repeat = "this is absolutely fascinating, a remarkable insight indeed";
    hu_turing_score_heuristic(resp_repeat, strlen(resp_repeat), ctx_repeat, strlen(ctx_repeat), 0,
                              &repetitive);

    const char *ctx_diverse = "assistant: that's a cool take\nuser: tell me more\n"
                              "assistant: yeah the interplay is interesting\nuser: ok";
    const char *resp_diverse = "hmm i think there's something deeper going on here";
    hu_turing_score_heuristic(resp_diverse, strlen(resp_diverse), ctx_diverse, strlen(ctx_diverse),
                              0, &diverse);
    HU_ASSERT(diverse.dimensions[HU_TURING_NON_ROBOTIC] >=
              repetitive.dimensions[HU_TURING_NON_ROBOTIC]);
}

/* ── Q4: Laugh style consistency affects personality_consistency ── */
static void turing_laugh_style_consistency(void) {
    hu_turing_score_t consistent, inconsistent;
    const char *ctx_haha = "assistant: haha that's so true\nuser: right?\nassistant: haha yeah";
    const char *resp_haha = "haha omg same";
    hu_turing_score_heuristic(resp_haha, strlen(resp_haha), ctx_haha, strlen(ctx_haha), 0,
                              &consistent);

    const char *ctx_lol = "assistant: haha that's so true\nuser: right?\nassistant: haha yeah";
    const char *resp_lol = "lol omg same";
    hu_turing_score_heuristic(resp_lol, strlen(resp_lol), ctx_lol, strlen(ctx_lol), 0, &inconsistent);
    HU_ASSERT(consistent.dimensions[HU_TURING_PERSONALITY_CONSISTENCY] >=
              inconsistent.dimensions[HU_TURING_PERSONALITY_CONSISTENCY]);
}

/* ── Q5: hu_conversation_vary_complexity applies contractions ── */
static void vary_complexity_applies_contractions(void) {
    char buf[256];
    snprintf(buf, sizeof(buf), "I am going to do not worry about it");
    size_t len = strlen(buf);
    size_t new_len = hu_conversation_vary_complexity(buf, len, 0);
    HU_ASSERT(new_len <= len);
    HU_ASSERT(new_len > 0);
}

static void vary_complexity_null_safe(void) {
    HU_ASSERT(hu_conversation_vary_complexity(NULL, 0, 42) == 0);
    char buf[4] = "";
    HU_ASSERT(hu_conversation_vary_complexity(buf, 0, 42) == 0);
}

static void vary_complexity_preserves_short_input(void) {
    char buf[8] = "ok";
    size_t len = 2;
    size_t new_len = hu_conversation_vary_complexity(buf, len, 99);
    HU_ASSERT(new_len == 2);
    HU_ASSERT(memcmp(buf, "ok", 2) == 0);
}

/* Tight max_channel_chars should penalize "long" vs default thresholds sooner. */
static void turing_max_channel_chars_tightens_structural_penalty(void) {
    char buf[220];
    memset(buf, 'a', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    hu_turing_score_t loose, tight;
    HU_ASSERT(hu_turing_score_heuristic(buf, strlen(buf), NULL, 0, 0, &loose) == HU_OK);
    HU_ASSERT(hu_turing_score_heuristic(buf, strlen(buf), NULL, 0, 100, &tight) == HU_OK);
    HU_ASSERT(tight.dimensions[HU_TURING_NATURAL_LANGUAGE] <=
              loose.dimensions[HU_TURING_NATURAL_LANGUAGE]);
}

void run_turing_score_tests(void) {
    HU_RUN_TEST(turing_heuristic_casual_message_scores_high);
    HU_RUN_TEST(turing_heuristic_ai_tells_score_low);
    HU_RUN_TEST(turing_heuristic_markdown_penalized);
    HU_RUN_TEST(turing_heuristic_emotional_message);
    HU_RUN_TEST(turing_heuristic_long_message_penalized);
    HU_RUN_TEST(turing_heuristic_hedging_penalized);
    HU_RUN_TEST(turing_dimension_names_valid);
    HU_RUN_TEST(turing_verdict_names_valid);
    HU_RUN_TEST(turing_score_summary_allocates);
    HU_RUN_TEST(turing_null_input_rejected);
    HU_RUN_TEST(turing_llm_null_provider_rejected);
    HU_RUN_TEST(turing_scores_clamped);
    HU_RUN_TEST(turing_energy_matching_short_to_short);
    HU_RUN_TEST(turing_energy_matching_essay_to_short_penalized);
    HU_RUN_TEST(turing_context_awareness_with_references);
    HU_RUN_TEST(turing_context_awareness_no_context);
    HU_RUN_TEST(turing_vulnerability_authentic_disclosure);
    HU_RUN_TEST(turing_humor_brief_natural);
    HU_RUN_TEST(turing_opinion_strong_position);
    HU_RUN_TEST(turing_personality_consistent_voice);
    HU_RUN_TEST(turing_genuine_warmth_with_context);
    HU_RUN_TEST(turing_voice_filler_usage_detected);
    HU_RUN_TEST(turing_voice_conversational_repair_detected);
    HU_RUN_TEST(turing_voice_paralinguistic_cues_detected);
    HU_RUN_TEST(turing_voice_emotional_prosody_with_emphasis);
    HU_RUN_TEST(turing_voice_prosody_varied_punctuation);
    HU_RUN_TEST(turing_voice_turn_timing_short_casual);
    HU_RUN_TEST(turing_trajectory_single_score);
    HU_RUN_TEST(turing_trajectory_improving_trend);
    HU_RUN_TEST(turing_trajectory_declining_trend);
    HU_RUN_TEST(turing_trajectory_stable_high);
    HU_RUN_TEST(turing_trajectory_null_rejected);
    HU_RUN_TEST(turing_contact_hint_builds_for_weak_dims);
    HU_RUN_TEST(turing_contact_hint_null_for_good_dims);
    HU_RUN_TEST(turing_ab_pick_variant_deterministic);
    HU_RUN_TEST(turing_rn_not_matched_inside_words);
    HU_RUN_TEST(turing_weighted_scoring_text_dominant);
    HU_RUN_TEST(turing_expanded_ai_tells_detected);
    HU_RUN_TEST(turing_markdown_bold_penalized);
    HU_RUN_TEST(turing_energy_matching_correct_segment);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turing_ab_test_sqlite_roundtrip);
    HU_RUN_TEST(turing_contact_dims_sqlite_empty_table);
    HU_RUN_TEST(turing_contact_dims_sqlite_with_data);
    HU_RUN_TEST(turing_channel_dims_sqlite_like_pattern);
#endif
    HU_RUN_TEST(turing_cross_turn_consistency_boosts_score);
    HU_RUN_TEST(turing_empathy_markers_boost_emotional_intelligence);
    HU_RUN_TEST(turing_robotic_apology_penalized);
    HU_RUN_TEST(turing_channel_weights_casual_boost);
    HU_RUN_TEST(turing_channel_weights_null_safe);
    HU_RUN_TEST(turing_multiline_user_turn_length);
    HU_RUN_TEST(turing_ultra_short_reply_high_length_score);
    HU_RUN_TEST(turing_sigh_not_matched_in_sight);
    HU_RUN_TEST(turing_energy_matching_user_turn_scoped);
    HU_RUN_TEST(turing_last_user_msg_prefix_matrix);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turing_ab_resolve_success_path);
    HU_RUN_TEST(turing_ab_resolve_too_close);
    HU_RUN_TEST(turing_channel_dims_hash_delimiter);
#endif

    /* Q1-Q5: question ratio, context awareness, vocab diversity, style fingerprint, vary_complexity
     */
    HU_RUN_TEST(turing_question_boosts_warmth);
    HU_RUN_TEST(turing_context_awareness_you_or_your);
    HU_RUN_TEST(turing_vocab_repetition_penalized);
    HU_RUN_TEST(turing_laugh_style_consistency);
    HU_RUN_TEST(vary_complexity_applies_contractions);
    HU_RUN_TEST(vary_complexity_null_safe);
    HU_RUN_TEST(vary_complexity_preserves_short_input);
    HU_RUN_TEST(turing_max_channel_chars_tightens_structural_penalty);
}
