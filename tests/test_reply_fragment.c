/* tests/test_reply_fragment.c — fragment detector, guard repair policy,
 * question-aware G5 cap, bubble-split cleanliness and the final outbound
 * check. Real 2026-09-30 sends anchor the battery:
 *   - "Wait, did we actually lock" (guard repair retry, sent in place of a
 *     94-char answer to "walk me through it")
 *   - "Nah too windy. just" | "hung out by the water" | "peaceful" (split) */
#include "human/agent/choreography.h"
#include "human/agent/guard_repair.h"
#include "human/agent/response_guard.h"
#include "human/context/conversation.h"
#include "human/context/reply_fragment.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S(lit) (lit), (sizeof(lit) - 1)

/* ── (a) detector battery ─────────────────────────────────────────────── */

static void fragment_detector_flags_the_real_fragments(void) {
    static const char *const frags[] = {
        "Wait, did we actually lock",        /* 2026-09-30 guard repair */
        "Nah too windy. just",               /* 2026-09-30 bubble split */
        "I was thinking we could go to the", /* article */
        "yeah and",                          /* conjunction */
        "It's not that bad because",         /* subordinator */
        "honestly it was very",              /* intensifier */
        "we should probably head out but",   /* conjunction */
        "so basically i'm",                  /* contracted auxiliary */
        "yeah i think it's",                 /* contracted auxiliary */
        "Let me check and,",                 /* clause comma */
        "here's the plan:",                  /* colon promising more */
        "He said (and I quote",              /* unclosed paren */
        "she said \"meet at the",            /* unclosed quote */
        "Did you end up booking the place",  /* sentence-case inverted question, no ? */
        "Sounds good. Are we still on for",  /* sentence-case inverted question, no ? */
    };
    for (size_t i = 0; i < sizeof(frags) / sizeof(frags[0]); i++) {
        if (!hu_reply_is_fragment(frags[i], strlen(frags[i])))
            printf("    not flagged: \"%s\"\n", frags[i]);
        HU_ASSERT_TRUE(hu_reply_is_fragment(frags[i], strlen(frags[i])));
    }
}

static void fragment_detector_passes_casual_complete_texts(void) {
    static const char *const ok[] = {
        "lol",
        "peaceful",
        "haha yeah",
        "nah im good",
        "did you eat", /* lowercase casual question, no ? */
        "omg yes",
        "same",
        "on my way",
        "sounds good",
        "yeah we should",
        "i'd love to",
        "where you at",
        "what's it about",
        "not yet",
        "i like it though",
        "ok",
        "k",
        "\xF0\x9F\x91\x8D", /* thumbs up emoji */
        "see you soon :)",
        "can't even",
        "nah too windy. just hung out by the water",
        "Nah too windy. Just hung out by the water",
        "Sounds good!",
        "Wait, did we actually lock it in?",
        "lmao what",
        "for sure",
        "kind of tired tbh",
        "i'm down",
        "it was fine i guess",
        "idk when",
        "just because",
        "yeah let's",
        "Thanksgiving is at my mom's, then we drive back Friday...",
        "we'll see",
        "(kidding)",
        "she said \"no way\"",
        "see you at 5",
        "",
        "   ",
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        if (hu_reply_is_fragment(ok[i], strlen(ok[i])))
            printf("    false positive: \"%s\"\n", ok[i]);
        HU_ASSERT_FALSE(hu_reply_is_fragment(ok[i], strlen(ok[i])));
    }
    HU_ASSERT_FALSE(hu_reply_is_fragment(NULL, 5));
}

static void fragment_dangling_word_list_is_case_and_apostrophe_insensitive(void) {
    HU_ASSERT_TRUE(hu_reply_word_is_dangling(S("JUST")));
    HU_ASSERT_TRUE(hu_reply_word_is_dangling(S("The")));
    HU_ASSERT_TRUE(hu_reply_word_is_dangling(S("it\xE2\x80\x99s"))); /* curly apostrophe */
    HU_ASSERT_FALSE(hu_reply_word_is_dangling(S("peaceful")));
    HU_ASSERT_FALSE(hu_reply_word_is_dangling(S("justice")));
    HU_ASSERT_FALSE(hu_reply_word_is_dangling(S("to")));
}

static void fragment_trim_to_sentence_stays_under_the_cap(void) {
    const char *t = "First thing. Second thing is longer. Third";
    HU_ASSERT_EQ(hu_reply_trim_to_sentence(t, strlen(t), 100),
                 strlen("First thing. Second thing is longer."));
    HU_ASSERT_EQ(hu_reply_trim_to_sentence(t, strlen(t), 20), strlen("First thing."));
    HU_ASSERT_EQ(hu_reply_trim_to_sentence(t, strlen(t), 5), 0u);
    HU_ASSERT_EQ(hu_reply_trim_to_sentence(S("no boundary here"), 100), 0u);
    /* "3.5" is not a sentence end. */
    HU_ASSERT_EQ(hu_reply_trim_to_sentence(S("it was 3.5 miles"), 100), 0u);
}

static void fragment_drop_dangling_tail_only_after_a_break(void) {
    HU_ASSERT_EQ(hu_reply_drop_dangling_tail(S("Nah too windy. just")), strlen("Nah too windy."));
    HU_ASSERT_EQ(hu_reply_drop_dangling_tail(S("sounds good, and")), strlen("sounds good"));
    /* Mid-clause: dropping "very" would leave "it was" — still broken. Leave it. */
    HU_ASSERT_EQ(hu_reply_drop_dangling_tail(S("it was very")), strlen("it was very"));
    /* Not a function word: never invent, never drop. */
    HU_ASSERT_EQ(hu_reply_drop_dangling_tail(S("Wait, did we actually lock")),
                 strlen("Wait, did we actually lock"));
    HU_ASSERT_EQ(hu_reply_drop_dangling_tail(S("lol")), 3u);
    /* The whole text is the dangling word: nothing to keep, leave it. */
    HU_ASSERT_EQ(hu_reply_drop_dangling_tail(S("just")), 4u);
}

/* ── (e) final outbound check ─────────────────────────────────────────── */

static void final_check_drops_a_lone_dangling_word_and_counts(void) {
    uint64_t before = hu_reply_final_fragment_count();
    size_t n = hu_reply_final_check(S("Nah too windy. just"), NULL);
    HU_ASSERT_EQ(n, strlen("Nah too windy."));
    HU_ASSERT_EQ(hu_reply_final_fragment_count(), before + 1);
    /* A fragment without a lone function word is counted, not altered. */
    n = hu_reply_final_check(S("Wait, did we actually lock"), NULL);
    HU_ASSERT_EQ(n, strlen("Wait, did we actually lock"));
    HU_ASSERT_EQ(hu_reply_final_fragment_count(), before + 2);
}

static void final_check_leaves_well_formed_replies_byte_identical(void) {
    static const char *const ok[] = {
        "lol",
        "peaceful",
        "nah too windy. just hung out by the water, peaceful",
        "Sounds good! See you at 5.",
        "did you eat",
        "haha yeah we should",
    };
    uint64_t before = hu_reply_final_fragment_count();
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        HU_ASSERT_EQ(hu_reply_final_check(ok[i], strlen(ok[i]), NULL), strlen(ok[i]));
    HU_ASSERT_EQ(hu_reply_final_fragment_count(), before);
}

/* ── (d) splitters ────────────────────────────────────────────────────── */

static void cut_is_clean_rejects_dangling_left_and_one_word_tail(void) {
    const char *t = "Nah too windy. just hung out by the water";
    /* After "just": left ends on a dangling word. */
    HU_ASSERT_FALSE(hu_reply_cut_is_clean(t, strlen(t), strlen("Nah too windy. just")));
    /* After "windy.": clean. */
    HU_ASSERT_TRUE(hu_reply_cut_is_clean(t, strlen(t), strlen("Nah too windy.")));
    const char *u = "nah too windy, just hung out by the water. peaceful";
    /* Before "peaceful": a 1-word tail. */
    HU_ASSERT_FALSE(hu_reply_cut_is_clean(u, strlen(u), strlen(u) - strlen("peaceful")));
}

static hu_message_plan_t rf_double_text(hu_allocator_t *alloc, const char *msg) {
    hu_choreography_config_t c = hu_choreography_config_default();
    c.double_text_probability = 1.0f;
    c.burst_probability = 0.0f;
    hu_message_plan_t plan = {0};
    HU_ASSERT_EQ(hu_choreography_plan(alloc, msg, strlen(msg), &c, 10u, &plan), HU_OK);
    return plan;
}

/* The kayak case with its "peaceful" tail: the sentence end nearest the
 * middle leaves a 1-word bubble, so the split takes the comma instead. */
static void double_text_never_leaves_a_one_word_tail(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_plan_t plan =
        rf_double_text(&alloc, "nah too windy, just hung out by the water. peaceful");
    HU_ASSERT_EQ(plan.segment_count, 2u);
    HU_ASSERT_STR_EQ(plan.segments[0].text, "nah too windy,");
    HU_ASSERT_STR_EQ(plan.segments[1].text, "just hung out by the water. peaceful");
    hu_choreography_plan_free(&alloc, &plan);
}

static bool rf_ends_on_dangling_word(const char *c, size_t cl) {
    size_t w = cl;
    while (w > 0 && c[w - 1] != ' ')
        w--;
    return hu_reply_word_is_dangling(c + w, cl - w);
}

/* Long replies with no punctuation in reach: break before "and"/"but"
 * (a clause boundary), never after a dangling function word. */
static void long_split_prefers_a_conjunction_over_a_dangling_space(void) {
    const char *msg = "so the plan is we drive up to the lake house early on thursday morning "
                      "and then my sister brings the pies over when she gets in from the airport "
                      "later that night";
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 100, chunks, 4);
    HU_ASSERT_EQ(2, (int)n);
    HU_ASSERT_STR_EQ(chunks[0],
                     "so the plan is we drive up to the lake house early on thursday morning");
    HU_ASSERT_FALSE(rf_ends_on_dangling_word(chunks[0], strlen(chunks[0])));
}

static void long_split_space_fallback_skips_dangling_words(void) {
    /* No sentence end, comma, "and" or "but" anywhere: the space fallback
     * must not end a bubble on "the"/"just"/"very". */
    const char *msg = "we were just sitting around the fire pit with the dogs for hours talking "
                      "about the best of the unforgettable summers with the neighbors on our "
                      "street growing up together";
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 100, chunks, 4);
    HU_ASSERT(n >= 2);
    for (size_t i = 0; i + 1 < n; i++) {
        const char *c = chunks[i];
        size_t cl = strlen(c);
        HU_ASSERT_FALSE(rf_ends_on_dangling_word(c, cl));
    }
}

/* Byte-identical pin: well-formed replies split exactly as before this
 * change (goldens captured from the pre-change splitter). */
static void splitters_leave_well_formed_replies_byte_identical(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_plan_t p1 = rf_double_text(&alloc, "nah too windy. just hung out by the water");
    HU_ASSERT_EQ(p1.segment_count, 2u);
    HU_ASSERT_STR_EQ(p1.segments[0].text, "nah too windy.");
    HU_ASSERT_STR_EQ(p1.segments[1].text, "just hung out by the water");
    hu_choreography_plan_free(&alloc, &p1);
    hu_message_plan_t p2 =
        rf_double_text(&alloc, "yeah we should totally do that, maybe next weekend or so");
    HU_ASSERT_EQ(p2.segment_count, 2u);
    HU_ASSERT_STR_EQ(p2.segments[0].text, "yeah we should totally do that,");
    hu_choreography_plan_free(&alloc, &p2);

    const char *msg = "honestly the thing that got me about the whole trip was how quiet it was up "
                      "there at night with no cars. anyway we should go back sometime soon";
    char chunks[4][512];
    HU_ASSERT_EQ(2, (int)hu_conversation_split_into_texts(msg, strlen(msg), 100, chunks, 4));
    HU_ASSERT_STR_EQ(chunks[0], "honestly the thing that got me about the whole trip was how quiet "
                                "it was up there at night with no cars.");
    HU_ASSERT_STR_EQ(chunks[1], "anyway we should go back sometime soon");

    const char *burst = "sure that sounds great. what time? might be a few mins late.";
    HU_ASSERT_EQ(3, (int)hu_conversation_split_for_cadence(burst, strlen(burst),
                                                           HU_CHANNEL_CLASS_TEXT_FAST, chunks, 4));
    HU_ASSERT_STR_EQ(chunks[2], "might be a few mins late.");
}

/* ── (b) guard repair policy ──────────────────────────────────────────── */

static hu_guard_report_t rf_length_only(void) {
    hu_guard_report_t r;
    memset(&r, 0, sizeof(r));
    r.detected_length_anomaly = true;
    r.max_repetition_run = 2; /* the logged "repetition_run" is a stat, not a violation */
    return r;
}

/* Thanksgiving, length-only shape: an answer over the cap, a fragment
 * retry. The original is trimmed to a sentence end under the cap. */
static void guard_repair_fragment_retry_keeps_the_original_trimmed(void) {
    const char *orig = "Honestly not much of a plan yet. Mom wants everyone at her place by noon. "
                       "Then we eat way too much and watch football till we pass out on the couch "
                       "like every year since forever, you know how it goes with my family";
    hu_guard_report_t rep = rf_length_only();
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(orig, strlen(orig), &rep, S("Wait, did we actually lock"), 80);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_TRIMMED);
    HU_ASSERT_EQ(d.len, strlen("Honestly not much of a plan yet. Mom wants everyone at her place "
                               "by noon."));
    HU_ASSERT_STR_EQ(d.reason, "retry_fragment");
}

static void guard_repair_original_under_the_cap_is_kept_whole(void) {
    const char *orig = "Thanksgiving's at my mom's, noon. Bring the pie.";
    hu_guard_report_t rep = rf_length_only();
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(orig, strlen(orig), &rep, S("Wait, did we actually lock"), 900);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_ORIGINAL);
    HU_ASSERT_EQ(d.len, strlen(orig));
}

/* A complete but collapsed retry (< 40%) of a length-only original: the
 * original (trimmed) wins — the 767 -> 29 pattern. */
static void guard_repair_collapsed_retry_loses_to_the_original(void) {
    const char *orig = "Honestly not much of a plan yet. Mom wants everyone at her place by noon. "
                       "Then we eat way too much and watch football till we pass out.";
    hu_guard_report_t rep = rf_length_only();
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(orig, strlen(orig), &rep, S("not sure yet lol"), 80);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_TRIMMED);
    HU_ASSERT_STR_EQ(d.reason, "retry_collapsed");
}

static void guard_repair_good_retry_is_kept(void) {
    const char *orig = "Honestly not much of a plan yet. Mom wants everyone at her place by noon.";
    hu_guard_report_t rep = rf_length_only();
    const char *retry = "mom's at noon, then football and a food coma like always";
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(orig, strlen(orig), &rep, retry, strlen(retry), 80);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_RETRY);
    HU_ASSERT_EQ(d.len, strlen(retry));
    HU_ASSERT_STR_EQ(d.reason, "ok");
}

/* The original leaked (director echo, the 2026-09-30 thanksgiving flags):
 * it is never sent, and neither is the fragment. */
static void guard_repair_leaky_original_never_sends_the_fragment(void) {
    const char *orig = "so whats the plan, admit he hasn't really thought about it yet lol";
    hu_guard_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.detected_director_echo = true;
    rep.max_repetition_run = 2;
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(orig, strlen(orig), &rep, S("Wait, did we actually lock"), 900);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_NONE);
    HU_ASSERT_EQ(d.len, 0u);
    /* A fragment retry with a complete first sentence keeps that sentence. */
    d = hu_guard_repair_decide(orig, strlen(orig), &rep, S("Nah too windy. just"), 900);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED);
    HU_ASSERT_EQ(d.len, strlen("Nah too windy."));
    /* A runaway repetition loop is never kept either. */
    memset(&rep, 0, sizeof(rep));
    rep.detected_degenerate_repetition = true;
    rep.detected_length_anomaly = true;
    d = hu_guard_repair_decide(orig, strlen(orig), &rep, S("ok"), 900);
    HU_ASSERT_EQ(d.kept, HU_GUARD_REPAIR_KEPT_RETRY);
}

static void guard_repair_resolve_swaps_in_the_trimmed_original(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *orig = "Honestly not much of a plan yet. Mom wants everyone at her place by noon. "
                       "Then we eat way too much and watch football till we pass out on the couch "
                       "like every year since forever, you know how it goes with my family and "
                       "all their opinions about everything under the sun, which is a lot, and "
                       "somehow it is still my favorite day of the whole year even with the drive "
                       "home in the dark and the traffic and the leftovers sliding around the "
                       "trunk the entire way back up the highway at night in the cold";
    hu_guard_report_t rep = rf_length_only();
    hu_guard_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.recent_avg_len = 28;
    ctx.length_anomaly_mult = HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT;
    char *retry = hu_strndup(&alloc, S("Wait, did we actually lock"));
    size_t retry_len = strlen(retry);
    hu_guard_repair_kept_t k =
        hu_guard_repair_resolve(&alloc, NULL, orig, strlen(orig), &rep, &ctx, &retry, &retry_len);
    HU_ASSERT_EQ(k, HU_GUARD_REPAIR_KEPT_TRIMMED);
    HU_ASSERT_NOT_NULL(retry);
    HU_ASSERT(retry_len <= HU_GUARD_LENGTH_ANOMALY_FLOOR);
    HU_ASSERT_EQ(memcmp(retry, orig, retry_len), 0);
    HU_ASSERT_EQ(retry[retry_len - 1], '.');
    HU_ASSERT_FALSE(hu_reply_is_fragment(retry, retry_len));
    alloc.free(alloc.ctx, retry, retry_len + 1);
}

static void guard_repair_resolve_none_frees_the_retry(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_guard_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.detected_semantic_leak = true;
    hu_guard_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    char *retry = hu_strndup(&alloc, S("Wait, did we actually lock"));
    size_t retry_len = strlen(retry);
    hu_guard_repair_kept_t k =
        hu_guard_repair_resolve(&alloc, NULL, S("leaky text"), &rep, &ctx, &retry, &retry_len);
    HU_ASSERT_EQ(k, HU_GUARD_REPAIR_KEPT_NONE);
    HU_ASSERT_NULL(retry);
    HU_ASSERT_EQ(retry_len, 0u);
}

/* ── (c) G5 against recent_avg for questions / requests ───────────────── */

static void inbound_ask_detector(void) {
    HU_ASSERT_TRUE(hu_guard_inbound_is_ask(
        S("so whats the plan for thanksgiving this year, walk me through it")));
    HU_ASSERT_TRUE(hu_guard_inbound_is_ask(S("did you end up renting that kayak")));
    HU_ASSERT_TRUE(hu_guard_inbound_is_ask(S("can you send me the address")));
    HU_ASSERT_TRUE(hu_guard_inbound_is_ask(S("tell me about the trip")));
    HU_ASSERT_TRUE(hu_guard_inbound_is_ask(S("you around?")));
    HU_ASSERT_FALSE(hu_guard_inbound_is_ask(S("lol nice")));
    HU_ASSERT_FALSE(hu_guard_inbound_is_ask(S("i know what you mean")));
    HU_ASSERT_FALSE(hu_guard_inbound_is_ask(S("ok sounds good")));
    HU_ASSERT_FALSE(hu_guard_inbound_is_ask(NULL, 0));
}

static hu_guard_outcome_t rf_guard(const char *text, size_t len, const hu_guard_context_t *ctx) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t oc = HU_GUARD_OK;
    hu_guard_report_t rep;
    memset(&rep, 0, sizeof(rep));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, text, len, ctx, &out, &out_len, &oc, &rep),
                 HU_OK);
    if (oc == HU_GUARD_REWROTE && out)
        alloc.free(alloc.ctx, out, out_len + 1);
    return oc;
}

static void rf_fill_answer(char *buf, size_t want) {
    static const char sentence[] = "We head to my mom's around noon, then dinner at three. ";
    size_t i = 0;
    while (i + sizeof(sentence) - 1 < want) {
        memcpy(buf + i, sentence, sizeof(sentence) - 1);
        i += sizeof(sentence) - 1;
    }
    buf[i] = '\0';
}

static void guard_length_does_not_reject_an_answer_on_recent_avg(void) {
    static char answer[1200];
    rf_fill_answer(answer, 500);
    size_t alen = strlen(answer);
    HU_ASSERT(alen > HU_GUARD_LENGTH_ANOMALY_FLOOR && alen < HU_GUARD_LENGTH_ASK_CEILING);
    hu_guard_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.recent_avg_len = 28; /* the thanksgiving conversation's average */
    ctx.length_anomaly_mult = HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT;
    /* Not an ask: judged against recent_avg x 6 = 168 -> REJECT (today's behaviour). */
    HU_ASSERT_EQ(rf_guard(answer, alen, &ctx), HU_GUARD_REJECT);
    HU_ASSERT_EQ(hu_guard_length_cap(&ctx), (size_t)HU_GUARD_LENGTH_ANOMALY_FLOOR);
    /* An ask: the relative cap rises to the ask ceiling -> OK. */
    ctx.inbound_is_ask = true;
    HU_ASSERT_EQ(hu_guard_length_cap(&ctx), (size_t)HU_GUARD_LENGTH_ASK_CEILING);
    HU_ASSERT_EQ(rf_guard(answer, alen, &ctx), HU_GUARD_OK);
    /* Past the ceiling a dump still trips G5, ask or not (2026-05-12: 979). */
    rf_fill_answer(answer, 1000);
    HU_ASSERT_EQ(rf_guard(answer, strlen(answer), &ctx), HU_GUARD_REJECT);
    /* A learned per-contact baseline is raised the same way. */
    ctx.learned_avg_message_length = 40;
    rf_fill_answer(answer, 500);
    HU_ASSERT_EQ(rf_guard(answer, strlen(answer), &ctx), HU_GUARD_OK);
    /* No baseline at all: no length check (unchanged). */
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_guard_length_cap(&ctx), (size_t)SIZE_MAX);
}

void run_reply_fragment_tests(void) {
    HU_TEST_SUITE("Reply Fragment");
    HU_RUN_TEST(fragment_detector_flags_the_real_fragments);
    HU_RUN_TEST(fragment_detector_passes_casual_complete_texts);
    HU_RUN_TEST(fragment_dangling_word_list_is_case_and_apostrophe_insensitive);
    HU_RUN_TEST(fragment_trim_to_sentence_stays_under_the_cap);
    HU_RUN_TEST(fragment_drop_dangling_tail_only_after_a_break);
    HU_RUN_TEST(final_check_drops_a_lone_dangling_word_and_counts);
    HU_RUN_TEST(final_check_leaves_well_formed_replies_byte_identical);
    HU_RUN_TEST(cut_is_clean_rejects_dangling_left_and_one_word_tail);
    HU_RUN_TEST(double_text_never_leaves_a_one_word_tail);
    HU_RUN_TEST(long_split_prefers_a_conjunction_over_a_dangling_space);
    HU_RUN_TEST(long_split_space_fallback_skips_dangling_words);
    HU_RUN_TEST(splitters_leave_well_formed_replies_byte_identical);
    HU_RUN_TEST(guard_repair_fragment_retry_keeps_the_original_trimmed);
    HU_RUN_TEST(guard_repair_original_under_the_cap_is_kept_whole);
    HU_RUN_TEST(guard_repair_collapsed_retry_loses_to_the_original);
    HU_RUN_TEST(guard_repair_good_retry_is_kept);
    HU_RUN_TEST(guard_repair_leaky_original_never_sends_the_fragment);
    HU_RUN_TEST(guard_repair_resolve_swaps_in_the_trimmed_original);
    HU_RUN_TEST(guard_repair_resolve_none_frees_the_retry);
    HU_RUN_TEST(inbound_ask_detector);
    HU_RUN_TEST(guard_length_does_not_reject_an_answer_on_recent_avg);
}
