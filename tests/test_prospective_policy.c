/* tests/test_prospective_policy.c
 *
 * Prospective memory v2 — the pure decisions (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.4).
 * No database and no model: every predicate's truth table, per
 * .claude/rules/security-predicate-extraction.md. */
#include "test_framework.h"

#include "human/core/string.h"
#include "human/memory/prospective_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void policy_column_spellings_round_trip(void) {
    hu_prospective_cue_kind_t k = HU_PM_CUE_TIME;
    HU_ASSERT_STR_EQ(hu_prospective_cue_kind_str(HU_PM_CUE_KEYWORD), "keyword");
    HU_ASSERT_STR_EQ(hu_prospective_cue_kind_str(HU_PM_CUE_TIME), "time");
    HU_ASSERT_STR_EQ(hu_prospective_cue_kind_str(HU_PM_CUE_AFTER_EVENT), "after_event");
    HU_ASSERT_NULL(hu_prospective_cue_kind_str((hu_prospective_cue_kind_t)9));
    HU_ASSERT_TRUE(hu_prospective_cue_kind_parse("keyword", &k));
    HU_ASSERT_EQ(k, HU_PM_CUE_KEYWORD);
    HU_ASSERT_FALSE(hu_prospective_cue_kind_parse("Keyword", &k));
    HU_ASSERT_FALSE(hu_prospective_cue_kind_parse(NULL, &k));

    static const char *const names[] = {"pending", "surfaced", "done", "canceled", "expired"};
    hu_prospective_status_t s = HU_PM_PENDING;
    for (int i = 0; i < 5; i++) {
        HU_ASSERT_STR_EQ(hu_prospective_status_str((hu_prospective_status_t)i), names[i]);
        HU_ASSERT_TRUE(hu_prospective_status_parse(names[i], &s));
        HU_ASSERT_EQ(s, i);
    }
    HU_ASSERT_FALSE(hu_prospective_status_parse("fired", &s));
    HU_ASSERT_NULL(hu_prospective_status_str((hu_prospective_status_t)7));

    HU_ASSERT_NULL(hu_prospective_outcome_str(HU_PM_OUTCOME_NONE));
    HU_ASSERT_STR_EQ(hu_prospective_outcome_str(HU_PM_OUTCOME_USED), "used");
    HU_ASSERT_STR_EQ(hu_prospective_outcome_str(HU_PM_OUTCOME_IGNORED), "ignored");
    HU_ASSERT_STR_EQ(hu_prospective_outcome_str(HU_PM_OUTCOME_SUPPRESSED), "suppressed");
    HU_ASSERT_STR_EQ(hu_prospective_source_str(HU_PM_SOURCE_EXTRACTOR), "extractor");
    HU_ASSERT_STR_EQ(hu_prospective_source_str(HU_PM_SOURCE_PROMISE_KEEPER), "promise_keeper");
    HU_ASSERT_STR_EQ(hu_prospective_source_str(HU_PM_SOURCE_FOLLOWUP), "followup");
    HU_ASSERT_STR_EQ(hu_prospective_verdict_str(HU_PM_VERDICT_RESOLVED), "already_resolved");
    HU_ASSERT_STR_EQ(hu_prospective_verdict_str(HU_PM_VERDICT_PARSE_FAIL), "parse_fail");
}

static void fired_mapping_matches_spec(void) {
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_PENDING), 0);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_SURFACED), 0);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_DONE), 1);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_CANCELED), 2);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_EXPIRED), 3);
}

static void gates_default_off_and_banner_names_the_key(void) {
    unsetenv("HU_PROSPECTIVE");
    unsetenv("HU_PROSPECTIVE_TIME");
    HU_ASSERT_EQ(hu_prospective_gate_mode(), HU_GATE_OFF);
    HU_ASSERT_EQ(hu_prospective_time_gate_mode(), HU_GATE_OFF);
    setenv("HU_PROSPECTIVE", "shadow", 1);
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    HU_ASSERT_EQ(hu_prospective_gate_mode(), HU_GATE_SHADOW);
    HU_ASSERT_EQ(hu_prospective_time_gate_mode(), HU_GATE_LIVE);
    setenv("HU_PROSPECTIVE", "bogus", 1);
    HU_ASSERT_EQ(hu_prospective_gate_mode(), HU_GATE_OFF); /* unknown fails closed */
    unsetenv("HU_PROSPECTIVE");
    unsetenv("HU_PROSPECTIVE_TIME");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_OFF, false),
                           "HU_PROSPECTIVE=shadow|live");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_OFF, true),
                           "HU_PROSPECTIVE_TIME=shadow|live");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_SHADOW, false), "SHADOW");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_LIVE, true), "LIVE");
}

static hu_prospective_filter_facts_t facts(hu_prospective_cue_kind_t kind) {
    hu_prospective_filter_facts_t f;
    memset(&f, 0, sizeof(f));
    f.cue_kind = kind;
    f.status = HU_PM_PENDING;
    f.now = 1000000;
    f.grace_s = HU_PROSPECTIVE_TIME_GRACE_S;
    return f;
}

static void filter_keyword_truth_table(void) {
    hu_prospective_filter_facts_t f = facts(HU_PM_CUE_KEYWORD);
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP); /* not cued */
    f.keyword_in_text = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.expires_at = f.now + 1;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.expires_at = f.now; /* inclusive, like the legacy sweep */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_EXPIRE);
    f.expires_at = 0;
    for (int st = HU_PM_SURFACED; st <= HU_PM_EXPIRED; st++) {
        f.status = (hu_prospective_status_t)st;
        HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    }
    f.status = HU_PM_PENDING;
    f.is_group = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    f.expires_at = f.now - 1; /* a group turn never writes, not even an expiry */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    f.is_group = false;
    f.expires_at = 0;
    f.is_self = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    HU_ASSERT_EQ(hu_prospective_filter(NULL), HU_PM_FILTER_SKIP);
}

static void filter_time_due_grace_and_daily_cap(void) {
    hu_prospective_filter_facts_t f = facts(HU_PM_CUE_TIME);
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP); /* no due_at */
    f.due_at = f.now + 60;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP); /* not due yet */
    f.due_at = f.now;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.due_at = f.now - f.grace_s; /* the last moment it may still fire */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.due_at = f.now - f.grace_s - 1;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_EXPIRE); /* never fires late */
    f.due_at = f.now - 60;
    f.surfaced_today = 1;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_CAPPED);
    f.surfaced_today = 0;
    f.keyword_in_text = true; /* irrelevant to a time cue */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.is_self = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    hu_prospective_filter_facts_t e = facts(HU_PM_CUE_AFTER_EVENT);
    e.keyword_in_text = true;
    e.due_at = e.now - 1;
    HU_ASSERT_EQ(hu_prospective_filter(&e), HU_PM_FILTER_SKIP); /* reserved, never eligible */
}

static void parse_verdict_truth_table(void) {
    static const struct {
        const char *raw;
        hu_prospective_verdict_t v;
    } cases[] = {
        {"fire", HU_PM_VERDICT_FIRE},
        {"Fire.", HU_PM_VERDICT_FIRE},
        {"  FIRE\n", HU_PM_VERDICT_FIRE},
        {"**fire**", HU_PM_VERDICT_FIRE},
        {"already_resolved", HU_PM_VERDICT_RESOLVED},
        {"Already resolved.", HU_PM_VERDICT_RESOLVED},
        {"resolved", HU_PM_VERDICT_RESOLVED},
        {"cancel", HU_PM_VERDICT_CANCEL},
        {"canceled", HU_PM_VERDICT_CANCEL},
        {"cancelled - they called it off", HU_PM_VERDICT_CANCEL},
        {"not_now", HU_PM_VERDICT_NOT_NOW},
        {"Not now.", HU_PM_VERDICT_NOT_NOW},
        {"not-now", HU_PM_VERDICT_NOT_NOW},
        {"<think>maybe fire</think>not_now", HU_PM_VERDICT_NOT_NOW},
        {"<think>fire is tempting", HU_PM_VERDICT_PARSE_FAIL},
        {"lol yeah def bring it up", HU_PM_VERDICT_PARSE_FAIL},
        {"The answer is fire", HU_PM_VERDICT_PARSE_FAIL},
        {"firework", HU_PM_VERDICT_PARSE_FAIL},
        {"not", HU_PM_VERDICT_PARSE_FAIL},
        {"", HU_PM_VERDICT_PARSE_FAIL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        HU_ASSERT_EQ(hu_prospective_parse_verdict(cases[i].raw, strlen(cases[i].raw)), cases[i].v);
    HU_ASSERT_EQ(hu_prospective_parse_verdict(NULL, 4), HU_PM_VERDICT_PARSE_FAIL);
    /* the length bounds the read: "fire" inside a longer buffer */
    HU_ASSERT_EQ(hu_prospective_parse_verdict("fireworks", 4), HU_PM_VERDICT_FIRE);
}

static void decide_fails_toward_silence(void) {
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_FIRE), HU_PM_ACT_SURFACE);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_RESOLVED), HU_PM_ACT_MARK_DONE);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_CANCEL), HU_PM_ACT_MARK_CANCELED);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_NOT_NOW), HU_PM_ACT_KEEP_PENDING);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_PARSE_FAIL), HU_PM_ACT_KEEP_PENDING);
    /* a model error never acts, whatever verdict value is lying around */
    HU_ASSERT_EQ(hu_prospective_decide(false, HU_PM_VERDICT_FIRE), HU_PM_ACT_KEEP_PENDING);
    HU_ASSERT_EQ(hu_prospective_decide(false, HU_PM_VERDICT_RESOLVED), HU_PM_ACT_KEEP_PENDING);
    HU_ASSERT_EQ(hu_prospective_decide(false, HU_PM_VERDICT_CANCEL), HU_PM_ACT_KEEP_PENDING);
}

static void key_terms_are_content_words(void) {
    char t[HU_PROSPECTIVE_KEY_TERMS_MAX][HU_PROSPECTIVE_KEY_TERM_LEN];
    size_t n =
        hu_prospective_key_terms("Ask how the new TACO place was", t, HU_PROSPECTIVE_KEY_TERMS_MAX);
    HU_ASSERT_EQ(n, (size_t)2);
    HU_ASSERT_STR_EQ(t[0], "taco");
    HU_ASSERT_STR_EQ(t[1], "place");
    n = hu_prospective_key_terms("send her the guitar teacher's number", t, 6);
    HU_ASSERT_EQ(n, (size_t)3);
    HU_ASSERT_STR_EQ(t[1], "teacher"); /* possessive stripped */
    n = hu_prospective_key_terms("don't forget the taco taco place", t, 6);
    HU_ASSERT_EQ(n, (size_t)2); /* contraction and stop word dropped, duplicate folded */
    HU_ASSERT_EQ(hu_prospective_key_terms("ask about it", t, 6), (size_t)0);
    HU_ASSERT_EQ(hu_prospective_key_terms("alpha bravo charlie delta echoes foxtrot golfs", t, 6),
                 (size_t)6);
    HU_ASSERT_EQ(hu_prospective_key_terms(NULL, t, 6), (size_t)0);
}

static void reply_uses_action_needs_half_the_key_terms(void) {
    HU_ASSERT_TRUE(hu_prospective_reply_uses_action("ask how the new taco place was",
                                                    "wait how was the TACO place??", 29));
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action("ask how the new taco place was",
                                                     "how was your weekend", 20));
    static const char *act = "check if he booked the dentist appointment";
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action(act, "did you book the dentist", 24));
    HU_ASSERT_TRUE(hu_prospective_reply_uses_action(act, "dentist appointment when", 24));
    /* plural-tolerant both ways */
    HU_ASSERT_TRUE(hu_prospective_reply_uses_action("send the lasagna recipes",
                                                    "here's the lasagna recipe", 25));
    /* no key terms (non-ASCII word splits short): never provable */
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action("ask about the caf\xc3\xa9",
                                                     "how was the caf\xc3\xa9", 17));
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action("send the lasagna recipe", NULL, 0));
    /* I2 (fix round 1): "text"/"tomorrow" are intention-verb/time filler,
     * not evidence of WHAT the intention is about — without them in the
     * stoplist, "ok i'll text you tomorrow" hit 2 of [text, tomorrow,
     * dentist] and marked the item DONE though the dentist never came up. */
    static const char *dentist_act = "text her tomorrow about the dentist";
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action(dentist_act, "ok i'll text you tomorrow", 25));
}

static void after_delivery_status_table(void) {
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(true, 0, 2), HU_PM_DONE);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(true, 1, 2), HU_PM_DONE);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(false, 0, 2), HU_PM_PENDING);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(false, 1, 2), HU_PM_EXPIRED);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(false, 5, 2), HU_PM_EXPIRED);
}

static void render_styles_are_exact(void) {
    const char *acts[] = {"ask how the taco place was", "send the lasagna recipe", "c", "d"};
    const char *cues[] = {"taco place", "lasagna", "c", "d"};
    char buf[1024];
    size_t len = 0;
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_SOFT, acts, NULL, 2, buf, sizeof(buf), &len),
                 (size_t)2);
    HU_ASSERT_STR_EQ(buf, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ask "
                          "how the taco place was | send the lasagna recipe]");
    HU_ASSERT_EQ(len, strlen(buf));
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_LEGACY, acts, cues, 1, buf, sizeof(buf), &len),
                 (size_t)1);
    HU_ASSERT_STR_EQ(buf, "[PROSPECTIVE MEMORY: Remember to: ask how the taco place was "
                          "(triggered by: taco place)]");
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_SOFT, acts, NULL, 4, buf, sizeof(buf), &len),
                 (size_t)3); /* render cap */
    HU_ASSERT_EQ(
        hu_prospective_render(HU_PM_RENDER_DUE_LIST, acts, NULL, 2, buf, sizeof(buf), &len),
        (size_t)2);
    HU_ASSERT_STR_EQ(buf, "- ask how the taco place was\n- send the lasagna recipe\n");
    /* legacy loop parity: an action that cannot fit renders nothing */
    char *huge = (char *)malloc(1100);
    HU_ASSERT_NOT_NULL(huge);
    memset(huge, 'x', 1099);
    huge[1099] = '\0';
    const char *big[] = {huge};
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_LEGACY, big, big, 1, buf, sizeof(buf), &len),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(len, (size_t)0);
    free(huge);
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_LEGACY, acts, NULL, 1, buf, sizeof(buf), &len),
                 (size_t)0); /* legacy needs cues */
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_SOFT, NULL, NULL, 1, buf, sizeof(buf), &len),
                 (size_t)0);
}

/* F4 fix round 1, I1 (controller ruling on task-2-brief.md, spec §4.1
 * source=promise_keeper): a commitment the CONTACT made
 * (hu_commitment_t.owner == "user") must render as a question about THEM,
 * never first person as if Seth owed the follow-through. Real summaries
 * come from text AFTER "I'll "/"I promise "/"remind me "/"my goal is "
 * (src/agent/commitment.c:25-30,87), so they routinely still carry a
 * leaked pattern prefix ("I'll ", "to ") and/or first-person pronouns
 * mid-clause — both must be neutralized, or the item renders nothing
 * rather than leak Seth's voice as the contact's. */
static void commitment_action_flips_contact_promises_into_a_question(void) {
    char buf[128];
    /* Seth's own commitment passes through unchanged (first person is
     * correct: Seth reminding himself). */
    HU_ASSERT_EQ(
        hu_prospective_commitment_action("send the lasagna recipe", false, buf, sizeof(buf)),
        strlen("send the lasagna recipe"));
    HU_ASSERT_STR_EQ(buf, "send the lasagna recipe");

    /* "I'll text you when I land" -> summary "text you when I land"
     * (promise pattern "I'll " already stripped by commitment.c). Bare
     * mid-clause "I" must flip to "they". */
    HU_ASSERT_EQ(hu_prospective_commitment_action("text you when I land", true, buf, sizeof(buf)),
                 strlen("ask if they still need to text you when they land"));
    HU_ASSERT_STR_EQ(buf, "ask if they still need to text you when they land");

    /* "I promise I'll be there" -> summary "I'll be there" (only "I
     * promise " is stripped by commitment.c's pattern match, so the
     * summary itself still starts with the leaked "I'll " fragment). */
    HU_ASSERT_EQ(hu_prospective_commitment_action("I'll be there", true, buf, sizeof(buf)),
                 strlen("ask if they still need to be there"));
    HU_ASSERT_STR_EQ(buf, "ask if they still need to be there");

    /* "remind me to call my mom" -> summary "to call my mom" (the
     * reminder pattern "remind me " leaves the "to" behind). */
    HU_ASSERT_EQ(hu_prospective_commitment_action("to call my mom", true, buf, sizeof(buf)),
                 strlen("ask if they still need to call their mom"));
    HU_ASSERT_STR_EQ(buf, "ask if they still need to call their mom");

    /* None of the three renders leaks a first-person pronoun. */
    HU_ASSERT_FALSE(hu_str_contains_word_ci(buf, "i"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci(buf, "me"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci(buf, "my"));

    /* Pronoun-free input needs no rewrite, just the question frame. */
    HU_ASSERT_EQ(hu_prospective_commitment_action("book the flight", true, buf, sizeof(buf)),
                 strlen("ask if they still need to book the flight"));
    HU_ASSERT_STR_EQ(buf, "ask if they still need to book the flight");

    /* A summary that is nothing but the leaked pattern (empty after the
     * leading strip) renders nothing rather than an empty-looking claim. */
    HU_ASSERT_EQ(hu_prospective_commitment_action("to ", true, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");

    /* NULL/empty summary and a too-small buffer never fabricate text. */
    HU_ASSERT_EQ(hu_prospective_commitment_action(NULL, true, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(hu_prospective_commitment_action("", true, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_EQ(hu_prospective_commitment_action("send the lasagna recipe", true, buf, 10),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
}

static void judge_prompt_carries_history_intention_and_cue(void) {
    size_t sl = 0;
    const char *sys = hu_prospective_judge_system(&sl);
    HU_ASSERT_EQ(sl, strlen(sys));
    HU_ASSERT_STR_CONTAINS(sys, "not_now");
    HU_ASSERT_STR_CONTAINS(sys, "already_resolved");

    char buf[2048];
    static const char hist[] = "them: lasagna night friday?\nme: yes!\n";
    size_t n =
        hu_prospective_judge_user(buf, sizeof(buf), hist, sizeof(hist) - 1,
                                  "send the lasagna recipe", "lasagna", HU_PM_CUE_KEYWORD, 0);
    HU_ASSERT_TRUE(n > 0 && n == strlen(buf));
    HU_ASSERT_STR_CONTAINS(buf, "conversation (oldest first):\nthem: lasagna night friday?\n");
    HU_ASSERT_STR_CONTAINS(buf, "intention: send the lasagna recipe");
    HU_ASSERT_STR_CONTAINS(buf, "cue: they just mentioned \"lasagna\"");
    HU_ASSERT_TRUE(strcmp(buf + n - 7, "answer:") == 0);
    n = hu_prospective_judge_user(buf, sizeof(buf), NULL, 0, "send the lasagna recipe", NULL,
                                  HU_PM_CUE_TIME, 2 * 86400 + 5);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(buf, "(none)");
    HU_ASSERT_STR_CONTAINS(buf, "it came due 2 day(s) ago");

    /* a long history keeps its most recent lines, cut at a line start */
    char *longh = (char *)malloc(6000);
    HU_ASSERT_NOT_NULL(longh);
    size_t pos = 0;
    for (int i = 0; pos + 40 < 6000; i++)
        pos += (size_t)snprintf(longh + pos, 6000 - pos, "them: line %04d of the history\n", i);
    char big[6144];
    n = hu_prospective_judge_user(big, sizeof(big), longh, pos, "act", "cue", HU_PM_CUE_KEYWORD, 0);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NULL(strstr(big, "line 0000"));
    HU_ASSERT_STR_CONTAINS(big, "\nthem: line ");
    free(longh);
    HU_ASSERT_EQ(hu_prospective_judge_user(buf, 16, hist, sizeof(hist) - 1, "act", "cue",
                                           HU_PM_CUE_KEYWORD, 0),
                 (size_t)0); /* does not fit: nothing half-written is used */
}

/* M1 (fix round 1): when the only '\n' in the last 4000 bytes of history is
 * the trailing byte (nothing after it to keep), trimming "at the line start"
 * throws the whole tail away and reports "(none)" — as if there were no
 * conversation at all, when there are up to 4499 bytes of it right before
 * that newline. The fix keeps the raw tail instead of trimming to empty. */
static void judge_user_keeps_tail_when_only_newline_is_trailing(void) {
    char *longh = (char *)malloc(4500);
    HU_ASSERT_NOT_NULL(longh);
    memset(longh, 'x', 4499);
    memcpy(longh + 4400, "MARKER_TAIL", 11); /* well inside the last 4000 bytes */
    longh[4499] = '\n';                      /* the ONLY newline, at the very end */
    char big[8192];
    size_t n = hu_prospective_judge_user(big, sizeof(big), longh, 4500, "act", "cue",
                                         HU_PM_CUE_KEYWORD, 0);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NULL(strstr(big, "(none)"));
    HU_ASSERT_NOT_NULL(strstr(big, "MARKER_TAIL"));
    free(longh);
}

static void local_day_start_is_a_stable_midnight(void) {
    int64_t now = 1790000000;
    int64_t d = hu_prospective_local_day_start(now);
    HU_ASSERT_TRUE(d <= now);
    HU_ASSERT_TRUE(now - d < 86400 + 3600);
    HU_ASSERT_EQ(hu_prospective_local_day_start(d), d);
}

void run_prospective_policy_tests(void) {
    HU_TEST_SUITE("prospective policy");
    HU_RUN_TEST(policy_column_spellings_round_trip);
    HU_RUN_TEST(fired_mapping_matches_spec);
    HU_RUN_TEST(gates_default_off_and_banner_names_the_key);
    HU_RUN_TEST(filter_keyword_truth_table);
    HU_RUN_TEST(filter_time_due_grace_and_daily_cap);
    HU_RUN_TEST(parse_verdict_truth_table);
    HU_RUN_TEST(decide_fails_toward_silence);
    HU_RUN_TEST(key_terms_are_content_words);
    HU_RUN_TEST(reply_uses_action_needs_half_the_key_terms);
    HU_RUN_TEST(after_delivery_status_table);
    HU_RUN_TEST(render_styles_are_exact);
    HU_RUN_TEST(commitment_action_flips_contact_promises_into_a_question);
    HU_RUN_TEST(judge_prompt_carries_history_intention_and_cue);
    HU_RUN_TEST(judge_user_keeps_tail_when_only_newline_is_trailing);
    HU_RUN_TEST(local_day_start_is_a_stable_midnight);
}
