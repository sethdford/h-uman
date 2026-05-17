#include "human/humanness.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

#define S(lit) (lit), (sizeof(lit) - 1)

/* ── Shared References Tests ─────────────────────────────────────────────── */

static void shared_refs_null_args(void) {
    hu_error_t err = hu_shared_references_find(NULL, S("c"), S("hi"), S("mem"), NULL, NULL, 3);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}

static void shared_refs_empty_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_shared_reference_t *refs = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_shared_references_find(&alloc, S("user1"), S("hello"), NULL, 0, &refs, &count, 3);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
    HU_ASSERT(refs == NULL);
}

static void shared_refs_finds_overlap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_shared_reference_t *refs = NULL;
    size_t count = 0;
    const char *memory = "last week they mentioned learning piano. "
                         "yesterday they talked about their project deadline";
    const char *msg = "my piano practice is going well";
    hu_error_t err = hu_shared_references_find(&alloc, S("user1"), msg, strlen(msg), memory,
                                               strlen(memory), &refs, &count, 3);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count > 0);
    HU_ASSERT(refs != NULL);
    HU_ASSERT(refs[0].reference != NULL);
    HU_ASSERT(refs[0].reference_len > 0);
    hu_shared_references_free(&alloc, refs, count);
}

static void shared_refs_build_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_shared_reference_t ref = {0};
    ref.reference = "the Monday morning panic";
    ref.reference_len = strlen(ref.reference);
    ref.recency = 0.8;

    size_t len = 0;
    char *directive = hu_shared_references_build_directive(&alloc, &ref, 1, &len);
    HU_ASSERT(directive != NULL);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(directive, "shorthand") != NULL);
    HU_ASSERT(strstr(directive, "Monday morning panic") != NULL);
    alloc.free(alloc.ctx, directive, len + 1);
}

static void shared_refs_directive_null_on_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *d = hu_shared_references_build_directive(&alloc, NULL, 0, NULL);
    HU_ASSERT(d == NULL);
}

/* ── Emotional Pacing Tests ──────────────────────────────────────────────── */

static void pacing_classify_grief(void) {
    hu_emotional_weight_t w = hu_emotional_weight_classify(S("my mother passed away yesterday"));
    HU_ASSERT(w == HU_WEIGHT_GRIEF);
}

static void pacing_classify_heavy(void) {
    hu_emotional_weight_t w = hu_emotional_weight_classify(S("i feel so depressed and lonely"));
    HU_ASSERT(w == HU_WEIGHT_HEAVY);
}

static void pacing_classify_normal(void) {
    hu_emotional_weight_t w = hu_emotional_weight_classify(S("can you explain how DNS works"));
    HU_ASSERT(w == HU_WEIGHT_NORMAL);
}

static void pacing_classify_light(void) {
    hu_emotional_weight_t w = hu_emotional_weight_classify(S("nice weather today"));
    HU_ASSERT(w == HU_WEIGHT_LIGHT);
}

static void pacing_classify_empty(void) {
    hu_emotional_weight_t w = hu_emotional_weight_classify(NULL, 0);
    HU_ASSERT(w == HU_WEIGHT_LIGHT);
}

static void pacing_adjust_grief_adds_delay(void) {
    uint64_t adjusted = hu_emotional_pacing_adjust(2000, HU_WEIGHT_GRIEF);
    HU_ASSERT(adjusted == 8000);
}

static void pacing_adjust_heavy_adds_delay(void) {
    uint64_t adjusted = hu_emotional_pacing_adjust(2000, HU_WEIGHT_HEAVY);
    HU_ASSERT(adjusted == 5000);
}

static void pacing_adjust_light_unchanged(void) {
    uint64_t adjusted = hu_emotional_pacing_adjust(2000, HU_WEIGHT_LIGHT);
    HU_ASSERT(adjusted == 2000);
}

/* ── Silence Intuition Tests ─────────────────────────────────────────────── */

static void silence_explicit_question_full_response(void) {
    hu_silence_response_t r = hu_silence_intuit(S("tell me more"), HU_WEIGHT_HEAVY, 3, true);
    HU_ASSERT(r == HU_SILENCE_FULL_RESPONSE);
}

static void silence_grief_no_question_presence(void) {
    hu_silence_response_t r = hu_silence_intuit(S("my dad died"), HU_WEIGHT_GRIEF, 2, false);
    HU_ASSERT(r == HU_SILENCE_PRESENCE_ONLY);
}

static void silence_grief_with_question_presence(void) {
    hu_silence_response_t r =
        hu_silence_intuit(S("my dad died. what do i do?"), HU_WEIGHT_GRIEF, 2, false);
    /* Has a question mark, so even in grief — respond */
    HU_ASSERT(r == HU_SILENCE_FULL_RESPONSE);
}

static void silence_heavy_short_acknowledge(void) {
    hu_silence_response_t r = hu_silence_intuit(S("i feel so lonely"), HU_WEIGHT_HEAVY, 3, false);
    HU_ASSERT(r == HU_SILENCE_BRIEF_ACKNOWLEDGE);
}

static void silence_vent_acknowledge(void) {
    hu_silence_response_t r = hu_silence_intuit(S("ugh"), HU_WEIGHT_LIGHT, 8, false);
    HU_ASSERT(r == HU_SILENCE_BRIEF_ACKNOWLEDGE);
}

static void silence_empty_msg(void) {
    hu_silence_response_t r = hu_silence_intuit(NULL, 0, HU_WEIGHT_LIGHT, 0, false);
    HU_ASSERT(r == HU_SILENCE_ACTUAL_SILENCE);
}

static void silence_build_presence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ack = hu_silence_build_acknowledgment(&alloc, HU_SILENCE_PRESENCE_ONLY, &len);
    HU_ASSERT(ack != NULL);
    HU_ASSERT(strcmp(ack, "I'm here.") == 0);
    alloc.free(alloc.ctx, ack, len + 1);
}

static void silence_build_brief(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ack = hu_silence_build_acknowledgment(&alloc, HU_SILENCE_BRIEF_ACKNOWLEDGE, &len);
    HU_ASSERT(ack != NULL);
    HU_ASSERT(strcmp(ack, "I hear you.") == 0);
    alloc.free(alloc.ctx, ack, len + 1);
}

static void silence_build_full_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *ack = hu_silence_build_acknowledgment(&alloc, HU_SILENCE_FULL_RESPONSE, NULL);
    HU_ASSERT(ack == NULL);
}

/* ── Emotional Residue Carryover Tests ───────────────────────────────────── */

static void carryover_null_out(void) {
    hu_error_t err = hu_residue_carryover_compute(NULL, NULL, NULL, 0, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}

static void carryover_empty_residues(void) {
    hu_residue_carryover_t c = {0};
    hu_error_t err = hu_residue_carryover_compute(NULL, NULL, NULL, 0, 1000, &c);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(c.intensity < 0.01);
}

static void carryover_grief_detected(void) {
    double valences[] = {-0.8};
    double intensities[] = {0.9};
    int64_t timestamps[] = {1000};
    hu_residue_carryover_t c = {0};
    hu_error_t err = hu_residue_carryover_compute(valences, intensities, timestamps, 1, 2000, &c);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(c.had_grief);
    HU_ASSERT(c.had_vulnerability);
    HU_ASSERT(c.net_valence < 0);
}

static void carryover_old_residue_decays(void) {
    double valences[] = {-0.9};
    double intensities[] = {0.9};
    int64_t timestamps[] = {0};
    hu_residue_carryover_t c = {0};
    /* 4 days later — beyond the 72h window */
    hu_error_t err =
        hu_residue_carryover_compute(valences, intensities, timestamps, 1, 4 * 24 * 3600, &c);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(!c.had_grief); /* too old */
}

static void carryover_grief_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_residue_carryover_t c = {
        .net_valence = -0.7,
        .intensity = 0.8,
        .had_grief = true,
        .hours_since = 6,
    };
    size_t len = 0;
    char *d = hu_residue_carryover_build_directive(&alloc, &c, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(strstr(d, "grief") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

static void carryover_low_intensity_no_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_residue_carryover_t c = {
        .net_valence = -0.1,
        .intensity = 0.1,
        .hours_since = 2,
    };
    char *d = hu_residue_carryover_build_directive(&alloc, &c, NULL);
    HU_ASSERT(d == NULL);
}

/* ── Curiosity Engine Tests ──────────────────────────────────────────────── */

static void curiosity_null_args(void) {
    hu_error_t err = hu_curiosity_generate(NULL, S("c"), S("mem"), S("msg"), NULL, NULL, 2);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}

static void curiosity_empty_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_curiosity_prompt_t *prompts = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_curiosity_generate(&alloc, S("user1"), NULL, 0, S("hello"), &prompts, &count, 2);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
}

static void curiosity_finds_triggers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_curiosity_prompt_t *prompts = NULL;
    size_t count = 0;
    const char *memory = "They mentioned learning guitar last month. "
                         "They were planning a trip to Japan.";
    hu_error_t err = hu_curiosity_generate(&alloc, S("user1"), memory, strlen(memory), S("hello"),
                                           &prompts, &count, 3);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count > 0);
    HU_ASSERT(prompts[0].question != NULL);
    hu_curiosity_prompts_free(&alloc, prompts, count);
}

static void curiosity_build_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_curiosity_prompt_t p = {0};
    p.question = "How is the guitar learning going?";
    p.question_len = strlen(p.question);

    size_t len = 0;
    char *d = hu_curiosity_build_directive(&alloc, &p, 1, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(strstr(d, "curious") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

/* P2-8 regression (2026-05-16 incident): hu_curiosity_generate used to
 * memcpy up to 60 chars around a trigger word into the
 *   "How is the %s going?"
 * template, producing garbage like "How is the i like hiking going?".
 *
 * The fix: gate the question on hu_proactive_topic_is_safe — if the
 * substring contains first-person pronouns or charged emotional keywords,
 * skip emitting that prompt. */
static void curiosity_skips_first_person_substring(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_curiosity_prompt_t *prompts = NULL;
    size_t count = 0;
    /* Memory context with a trigger word ("learning") embedded inside a
     * first-person confession-style sentence. The old code would build
     * "How is the i'm learning to lie better going?" — a garbage,
     * leakage-prone message. */
    const char *memory = "I'm learning to lie better lately.";
    hu_error_t err = hu_curiosity_generate(&alloc, S("user1"), memory, strlen(memory), S("hello"),
                                           &prompts, &count, 2);
    HU_ASSERT(err == HU_OK);

    /* If any prompt landed, none of them may contain first-person leakage. */
    for (size_t i = 0; i < count; i++) {
        HU_ASSERT(strstr(prompts[i].question, "i'm") == NULL);
        HU_ASSERT(strstr(prompts[i].question, "I'm") == NULL);
        HU_ASSERT(strstr(prompts[i].question, " i ") == NULL);
        /* No "How is the i" prefix garbage. */
        HU_ASSERT(strstr(prompts[i].question, "How is the i") == NULL);
        HU_ASSERT(strstr(prompts[i].question, "to lie better") == NULL);
    }
    if (count > 0)
        hu_curiosity_prompts_free(&alloc, prompts, count);
}

static void curiosity_skips_emotion_keyword_substring(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_curiosity_prompt_t *prompts = NULL;
    size_t count = 0;
    /* Substring with a charged emotion keyword that must never become a
     * topic in a chirpy "How is the X going?" message. */
    const char *memory = "feeling lonely and considering quitting therapy";
    hu_error_t err = hu_curiosity_generate(&alloc, S("user1"), memory, strlen(memory), S("hello"),
                                           &prompts, &count, 2);
    HU_ASSERT(err == HU_OK);

    for (size_t i = 0; i < count; i++) {
        HU_ASSERT(strstr(prompts[i].question, "lonely") == NULL);
        HU_ASSERT(strstr(prompts[i].question, "quitting therapy") == NULL);
    }
    if (count > 0)
        hu_curiosity_prompts_free(&alloc, prompts, count);
}

/* ── Unasked Question Detector Tests ─────────────────────────────────────── */

static void absence_null_args(void) {
    hu_error_t err = hu_absence_detect(NULL, S("msg"), NULL, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}

static void absence_short_msg_no_signal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_absence_signal_t *signals = NULL;
    size_t count = 0;
    hu_error_t err = hu_absence_detect(&alloc, S("hi"), &signals, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
}

static void absence_detects_missing_emotion(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_absence_signal_t *signals = NULL;
    size_t count = 0;
    const char *msg = "I had my big presentation today and it went okay I guess";
    hu_error_t err = hu_absence_detect(&alloc, msg, strlen(msg), &signals, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count > 0);
    HU_ASSERT(signals[0].topic != NULL);
    HU_ASSERT(strcmp(signals[0].topic, "presentation") == 0);
    HU_ASSERT(signals[0].missing_aspect != NULL);
    hu_absence_signals_free(&alloc, signals, count);
}

static void absence_no_detection_when_emotion_present(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_absence_signal_t *signals = NULL;
    size_t count = 0;
    const char *msg = "I had my presentation and I felt really nervous about it";
    hu_error_t err = hu_absence_detect(&alloc, msg, strlen(msg), &signals, &count);
    HU_ASSERT(err == HU_OK);
    /* "felt" is present, so no absence for presentation */
    HU_ASSERT(count == 0);
}

static void absence_build_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_absence_signal_t sig = {0};
    sig.topic = "interview";
    sig.topic_len = strlen(sig.topic);
    sig.missing_aspect = "how they felt about the interview";
    sig.missing_aspect_len = strlen(sig.missing_aspect);
    sig.confidence = 0.7;

    size_t len = 0;
    char *d = hu_absence_build_directive(&alloc, &sig, 1, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(strstr(d, "interview") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

/* ── Evolving Opinion Tests ──────────────────────────────────────────────── */

static void opinion_null_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *d = hu_evolved_opinion_build_directive(&alloc, NULL, 0, 0.5, NULL);
    HU_ASSERT(d == NULL);
}

static void opinion_below_conviction_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_t op = {
        .topic = "testing",
        .topic_len = 7,
        .stance = "unit tests are overrated",
        .stance_len = 24,
        .conviction = 0.3,
        .interactions = 5,
    };
    char *d = hu_evolved_opinion_build_directive(&alloc, &op, 1, 0.5, NULL);
    HU_ASSERT(d == NULL);
}

static void opinion_above_conviction_builds_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_t op = {
        .topic = "code review",
        .topic_len = 11,
        .stance = "pair programming beats async review",
        .stance_len = 35,
        .conviction = 0.85,
        .interactions = 12,
    };
    size_t len = 0;
    char *d = hu_evolved_opinion_build_directive(&alloc, &op, 1, 0.5, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(strstr(d, "code review") != NULL);
    HU_ASSERT(strstr(d, "firmly") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

static void opinion_moderate_conviction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_t op = {
        .topic = "testing",
        .topic_len = 7,
        .stance = "integration tests matter more",
        .stance_len = 29,
        .conviction = 0.6,
        .interactions = 4,
    };
    size_t len = 0;
    char *d = hu_evolved_opinion_build_directive(&alloc, &op, 1, 0.5, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(strstr(d, "moderately") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

/* ── Imperfect Delivery Tests ────────────────────────────────────────────── */

static void certainty_with_tools(void) {
    hu_certainty_level_t c = hu_certainty_classify(S("what's the weather"), false, 2);
    HU_ASSERT(c == HU_CERTAIN);
}

static void certainty_with_memory(void) {
    hu_certainty_level_t c = hu_certainty_classify(S("how's it going"), true, 0);
    HU_ASSERT(c == HU_MOSTLY_SURE);
}

static void certainty_opinion_question(void) {
    hu_certainty_level_t c =
        hu_certainty_classify(S("what do you think about remote work"), false, 0);
    HU_ASSERT(c == HU_UNCERTAIN);
}

static void certainty_philosophical(void) {
    hu_certainty_level_t c = hu_certainty_classify(S("what is the meaning of life"), false, 0);
    HU_ASSERT(c == HU_GENUINELY_UNSURE);
}

static void certainty_empty(void) {
    hu_certainty_level_t c = hu_certainty_classify(NULL, 0, false, 0);
    HU_ASSERT(c == HU_CERTAIN);
}

static void imperfect_certain_no_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *d = hu_imperfect_delivery_directive(&alloc, HU_CERTAIN, NULL);
    HU_ASSERT(d == NULL);
}

static void imperfect_uncertain_has_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *d = hu_imperfect_delivery_directive(&alloc, HU_UNCERTAIN, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(d, "uncertainty") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

static void imperfect_genuinely_unsure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *d = hu_imperfect_delivery_directive(&alloc, HU_GENUINELY_UNSURE, &len);
    HU_ASSERT(d != NULL);
    HU_ASSERT(strstr(d, "don't know") != NULL);
    alloc.free(alloc.ctx, d, len + 1);
}

/* ── Test Runner ─────────────────────────────────────────────────────────── */

int run_humanness_tests(void) {
    HU_TEST_SUITE("humanness");

    /* Shared references */
    HU_RUN_TEST(shared_refs_null_args);
    HU_RUN_TEST(shared_refs_empty_memory);
    HU_RUN_TEST(shared_refs_finds_overlap);
    HU_RUN_TEST(shared_refs_build_directive);
    HU_RUN_TEST(shared_refs_directive_null_on_empty);

    /* Emotional pacing */
    HU_RUN_TEST(pacing_classify_grief);
    HU_RUN_TEST(pacing_classify_heavy);
    HU_RUN_TEST(pacing_classify_normal);
    HU_RUN_TEST(pacing_classify_light);
    HU_RUN_TEST(pacing_classify_empty);
    HU_RUN_TEST(pacing_adjust_grief_adds_delay);
    HU_RUN_TEST(pacing_adjust_heavy_adds_delay);
    HU_RUN_TEST(pacing_adjust_light_unchanged);

    /* Silence intuition */
    HU_RUN_TEST(silence_explicit_question_full_response);
    HU_RUN_TEST(silence_grief_no_question_presence);
    HU_RUN_TEST(silence_grief_with_question_presence);
    HU_RUN_TEST(silence_heavy_short_acknowledge);
    HU_RUN_TEST(silence_vent_acknowledge);
    HU_RUN_TEST(silence_empty_msg);
    HU_RUN_TEST(silence_build_presence);
    HU_RUN_TEST(silence_build_brief);
    HU_RUN_TEST(silence_build_full_returns_null);

    /* Emotional residue carryover */
    HU_RUN_TEST(carryover_null_out);
    HU_RUN_TEST(carryover_empty_residues);
    HU_RUN_TEST(carryover_grief_detected);
    HU_RUN_TEST(carryover_old_residue_decays);
    HU_RUN_TEST(carryover_grief_directive);
    HU_RUN_TEST(carryover_low_intensity_no_directive);

    /* Curiosity engine */
    HU_RUN_TEST(curiosity_null_args);
    HU_RUN_TEST(curiosity_empty_memory);
    HU_RUN_TEST(curiosity_finds_triggers);
    HU_RUN_TEST(curiosity_build_directive);
    HU_RUN_TEST(curiosity_skips_first_person_substring);
    HU_RUN_TEST(curiosity_skips_emotion_keyword_substring);

    /* Unasked question detector */
    HU_RUN_TEST(absence_null_args);
    HU_RUN_TEST(absence_short_msg_no_signal);
    HU_RUN_TEST(absence_detects_missing_emotion);
    HU_RUN_TEST(absence_no_detection_when_emotion_present);
    HU_RUN_TEST(absence_build_directive);

    /* Evolving opinion */
    HU_RUN_TEST(opinion_null_returns_null);
    HU_RUN_TEST(opinion_below_conviction_returns_null);
    HU_RUN_TEST(opinion_above_conviction_builds_directive);
    HU_RUN_TEST(opinion_moderate_conviction);

    /* Imperfect delivery */
    HU_RUN_TEST(certainty_with_tools);
    HU_RUN_TEST(certainty_with_memory);
    HU_RUN_TEST(certainty_opinion_question);
    HU_RUN_TEST(certainty_philosophical);
    HU_RUN_TEST(certainty_empty);
    HU_RUN_TEST(imperfect_certain_no_directive);
    HU_RUN_TEST(imperfect_uncertain_has_directive);
    HU_RUN_TEST(imperfect_genuinely_unsure);

    return 0;
}
