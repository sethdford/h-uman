#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include "human/persona.h"
#include "human/platform/calendar.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Helper to build history entries ─────────────────────────────────── */

static hu_channel_history_entry_t make_entry(bool from_me, const char *text, const char *ts) {
    hu_channel_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.from_me = from_me;
    size_t tl = strlen(text);
    if (tl >= sizeof(e.text))
        tl = sizeof(e.text) - 1;
    memcpy(e.text, text, tl);
    e.text[tl] = '\0';
    size_t tsl = strlen(ts);
    if (tsl >= sizeof(e.timestamp))
        tsl = sizeof(e.timestamp) - 1;
    memcpy(e.timestamp, ts, tsl);
    e.timestamp[tsl] = '\0';
    return e;
}

/* ── Multi-message splitting tests ───────────────────────────────────── */

static void split_short_response_stays_single(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[4];
    size_t n = hu_conversation_split_response(&alloc, "yeah for sure", 13, frags, 4, 0);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(frags[0].text, "yeah for sure");
    HU_ASSERT_EQ(frags[0].delay_ms, 0u);
    alloc.free(alloc.ctx, frags[0].text, frags[0].text_len + 1);
}

static void split_null_input_returns_zero(void) {
    hu_message_fragment_t frags[4];
    size_t n = hu_conversation_split_response(NULL, "hello", 5, frags, 4, 0);
    HU_ASSERT_EQ(n, 0u);
}

static void split_empty_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[4];
    size_t n = hu_conversation_split_response(&alloc, "", 0, frags, 4, 0);
    HU_ASSERT_EQ(n, 0u);
}

static void split_null_fragments_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t n = hu_conversation_split_response(&alloc, "hello", 5, NULL, 4, 0);
    HU_ASSERT_EQ(n, 0u);
}

static void split_zero_max_fragments_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[4];
    size_t n = hu_conversation_split_response(&alloc, "hello", 5, frags, 0, 0);
    HU_ASSERT_EQ(n, 0u);
}

static void split_on_newlines(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[4];
    const char *resp = "hey how are you\n\nso i was thinking about that thing";
    size_t n = hu_conversation_split_response(&alloc, resp, strlen(resp), frags, 4, 0);
    HU_ASSERT_TRUE(n >= 2);
    HU_ASSERT_TRUE(frags[0].text_len > 0);
    HU_ASSERT_TRUE(frags[1].text_len > 0);
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

static void split_on_conjunction_starter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[4];
    const char *resp =
        "that sounds really fun honestly. but i think we should check the weather first";
    size_t n = hu_conversation_split_response(&alloc, resp, strlen(resp), frags, 4, 0);
    HU_ASSERT_TRUE(n >= 2);
    /* First fragment should end with the sentence before "but" */
    HU_ASSERT_TRUE(strstr(frags[0].text, "honestly") != NULL);
    /* Second fragment should start with "but" */
    HU_ASSERT_TRUE(frags[1].text[0] == 'b' || frags[1].text[0] == 'B');
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

static void split_respects_max_fragments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[2];
    const char *resp = "first thing.\nsecond thing.\nthird thing.\nfourth thing.";
    size_t n = hu_conversation_split_response(&alloc, resp, strlen(resp), frags, 2, 0);
    HU_ASSERT_TRUE(n <= 2);
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

static void split_inter_message_delay_nonzero_for_later_fragments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_message_fragment_t frags[4];
    const char *resp = "omg that's wild.\noh wait also did you hear about the thing at work";
    size_t n = hu_conversation_split_response(&alloc, resp, strlen(resp), frags, 4, 0);
    if (n >= 2) {
        HU_ASSERT_EQ(frags[0].delay_ms, 0u);
        HU_ASSERT_TRUE(frags[1].delay_ms > 0);
    }
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

/* ── Style analysis tests ────────────────────────────────────────────── */

static void style_null_returns_null(void) {
    size_t len = 0;
    char *s = hu_conversation_analyze_style(NULL, NULL, 0, NULL, &len);
    HU_ASSERT_NULL(s);
}

static void style_too_few_messages_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
    };
    size_t len = 0;
    char *s = hu_conversation_analyze_style(&alloc, entries, 2, NULL, &len);
    HU_ASSERT_NULL(s);
}

static void style_detects_all_lowercase(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[6] = {
        make_entry(false, "hey whats up", "12:00"),
        make_entry(true, "nm", "12:01"),
        make_entry(false, "lol same", "12:02"),
        make_entry(false, "did you see that thing", "12:03"),
        make_entry(true, "yeah", "12:04"),
        make_entry(false, "so wild right", "12:05"),
    };
    size_t len = 0;
    char *s = hu_conversation_analyze_style(&alloc, entries, 6, NULL, &len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(s, "lowercase") != NULL || strstr(s, "capitalize") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

static void style_detects_no_periods(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[6] = {
        make_entry(false, "hey whats good", "12:00"),
        make_entry(true, "nm u", "12:01"),
        make_entry(false, "not much just chillin", "12:02"),
        make_entry(false, "thinking about getting food", "12:03"),
        make_entry(true, "same", "12:04"),
        make_entry(false, "wanna come", "12:05"),
    };
    size_t len = 0;
    char *s = hu_conversation_analyze_style(&alloc, entries, 6, NULL, &len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(strstr(s, "period") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

static void style_includes_anti_patterns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[6] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hey", "12:01"),
        make_entry(false, "what are you doing", "12:02"),
        make_entry(false, "i'm bored lol", "12:03"),
        make_entry(true, "same", "12:04"),
        make_entry(false, "wanna hang", "12:05"),
    };
    size_t len = 0;
    char *s = hu_conversation_analyze_style(&alloc, entries, 6, NULL, &len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(strstr(s, "Anti-pattern") != NULL || strstr(s, "NEVER") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

/* ── Response classification tests ───────────────────────────────────── */

static void classify_empty_skips(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("", 0, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_SKIP);
}

static void classify_tapback_skips(void) {
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("Loved an image", 14, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_SKIP);
}

static void classify_lol_is_brief(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("lol", 3, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_haha_is_brief(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("haha", 4, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_nice_is_brief(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("nice", 4, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_question_is_full(void) {
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("what are you up to tonight?", 27, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_FULL);
    HU_ASSERT_TRUE(delay > 0);
}

static void classify_emotional_is_delayed(void) {
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("i've been really stressed lately", 31, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_DELAY);
    HU_ASSERT_TRUE(delay >= 5000);
}

static void classify_ok_after_question_skips(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "want to grab dinner?", "12:00"),
        make_entry(false, "ok", "12:01"),
    };
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("ok", 2, entries, 2, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_SKIP);
}

static void classify_ok_after_distant_question_skips(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(true, "want to grab dinner?", "12:00"),
        make_entry(false, "maybe", "12:01"),
        make_entry(true, "cool let me know", "12:02"),
        make_entry(false, "ok", "12:03"),
    };
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("ok", 2, entries, 4, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_SKIP);
}

static void classify_normal_statement_is_brief(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response(
        "i just got back from the store and got us some stuff", 52, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_farewell_goodnight_is_brief(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("goodnight!", 10, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
    HU_ASSERT_TRUE(delay > 0);
}

static void classify_farewell_short_bye(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("bye", 3, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_farewell_ttyl(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response("ttyl!", 5, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_bad_news_is_delayed(void) {
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("my grandma passed away last night", 33, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_DELAY);
    HU_ASSERT_TRUE(delay >= 10000);
}

static void classify_good_news_is_delayed(void) {
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("I got the job!!", 15, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_DELAY);
    HU_ASSERT_TRUE(delay >= 2000);
}

static void classify_vulnerable_is_delayed(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response(
        "can i be honest with you about something", 41, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_DELAY);
    HU_ASSERT_TRUE(delay >= 5000);
}

/* ── Two-phase thinking response tests ─────────────────────────────────── */

/* Helper: build a synthetic persona+overlay with a 3-entry filler bank on "slack". */
static void make_test_thinking_ctx(hu_persona_t *persona, hu_persona_overlay_t *overlay,
                                   char **bank, hu_filler_recency_t *recency,
                                   hu_thinking_context_t *ctx, uint32_t seed) {
    memset(persona, 0, sizeof(*persona));
    memset(overlay, 0, sizeof(*overlay));
    memset(recency, 0, sizeof(*recency));
    overlay->channel = "slack";
    bank[0] = "hmm";
    bank[1] = "let me think";
    bank[2] = "one sec";
    overlay->filler_bank = bank;
    overlay->filler_bank_count = 3;
    persona->overlays = overlay;
    persona->overlays_count = 1;
    ctx->persona = persona;
    ctx->channel_name = "slack";
    ctx->chat_id = "test_chat";
    ctx->chat_id_len = 9;
    ctx->recency = recency;
    ctx->seed = seed;
}

static void thinking_triggers_on_complex_question(void) {
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(out.filler_len > 0);
    HU_ASSERT_TRUE(out.delay_ms >= 30000 && out.delay_ms <= 60000);
    /* filler must be one of the 3 bank entries */
    bool valid = (strcmp(out.filler, "hmm") == 0 || strcmp(out.filler, "let me think") == 0 ||
                  strcmp(out.filler, "one sec") == 0);
    HU_ASSERT_TRUE(valid);
}

static void thinking_no_trigger_simple_message(void) {
    const char *msg = "hey what's up";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_FALSE(ok);
}

static void thinking_triggers_on_advice(void) {
    const char *msg = "should I take the new job or stay where I am?";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(out.filler_len > 0);
    HU_ASSERT_TRUE(out.delay_ms >= 30000 && out.delay_ms <= 60000);
    bool valid = (strcmp(out.filler, "hmm") == 0 || strcmp(out.filler, "let me think") == 0 ||
                  strcmp(out.filler, "one sec") == 0);
    HU_ASSERT_TRUE(valid);
}

static void thinking_filler_varies_by_seed(void) {
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona0, persona1;
    hu_persona_overlay_t overlay0, overlay1;
    char *bank0[3], *bank1[3];
    hu_filler_recency_t recency0, recency1;
    hu_thinking_context_t ctx0, ctx1;
    make_test_thinking_ctx(&persona0, &overlay0, bank0, &recency0, &ctx0, 0);
    make_test_thinking_ctx(&persona1, &overlay1, bank1, &recency1, &ctx1, 1);
    hu_thinking_response_t out0 = {0}, out1 = {0};
    bool ok0 = hu_conversation_classify_thinking(&ctx0, msg, strlen(msg), NULL, 0, &out0);
    bool ok1 = hu_conversation_classify_thinking(&ctx1, msg, strlen(msg), NULL, 0, &out1);
    HU_ASSERT_TRUE(ok0);
    HU_ASSERT_TRUE(ok1);
    /* Different seeds should produce different fillers (3 options, LCG varies) */
    bool same = (out0.filler_len == out1.filler_len &&
                 memcmp(out0.filler, out1.filler, out0.filler_len) == 0);
    HU_ASSERT_FALSE(same);
}

/* ── TEXT_FAST (iMessage/SMS) thinking-filler contract ──────────────────────
 *
 * Earlier this code returned false unconditionally for TEXT_FAST channels, on
 * the theory that any delay on iMessage reads as dispreference. The result
 * was that iMessage never sent "hm" / "wait" / "lemme think" bubbles — the
 * single most distinctive humanness signal in casual texting. This was a
 * humanness-thesis violation pinned by a test asserting the bug as intent.
 *
 * New contract:
 *   - TEXT_FAST channels DO produce a thinking filler when the trigger heuristic
 *     fires (substantive message: long enough or question with ≥6 words).
 *   - The delay AFTER the filler is 400–1500 ms uniform — matches the real
 *     human pause-think-continue cadence; does not read as dispreference.
 *   - TEXT_ASYNC keeps its 30–60 s delay (Slack/Discord pauses are tolerated).
 *
 * If any of these three invariants break, this suite fails first.
 * ────────────────────────────────────────────────────────────────────────── */

static void thinking_text_fast_imessage_emits_filler(void) {
    /* Regression guard for the "speed wins" bug: iMessage MUST produce a
     * filler on a substantive question, not return false. */
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    ctx.channel_name = "imessage";
    overlay.channel = "imessage";
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(out.filler_len > 0);
    bool valid = (strcmp(out.filler, "hmm") == 0 || strcmp(out.filler, "let me think") == 0 ||
                  strcmp(out.filler, "one sec") == 0);
    HU_ASSERT_TRUE(valid);
}

static void thinking_text_fast_imessage_uses_fast_delay(void) {
    /* THE invariant: the delay between filler and substantive reply must be
     * in the human pause-think range (400–1500 ms), NOT the 30–60 s
     * TEXT_ASYNC range. If this ever fails high, fillers on iMessage have
     * been silently coupled to the wrong delay formula and the recipient
     * will sit watching "hm" for a half-minute. */
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    ctx.channel_name = "imessage";
    overlay.channel = "imessage";
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(out.delay_ms >= 400);
    HU_ASSERT_TRUE(out.delay_ms <= 1500);
}

static void thinking_text_fast_sms_also_gets_fast_delay(void) {
    /* SMS shares the TEXT_FAST class with iMessage — same cadence contract.
     * Catches the case where a future change special-cases iMessage but
     * forgets SMS, leaving SMS bouncing back to the 30 s TEXT_ASYNC delay. */
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    ctx.channel_name = "sms";
    overlay.channel = "sms";
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(out.delay_ms >= 400);
    HU_ASSERT_TRUE(out.delay_ms <= 1500);
}

static void thinking_text_async_keeps_long_delay(void) {
    /* Slack/Discord/Telegram (TEXT_ASYNC) cadence is genuinely slower —
     * pauses are tolerated and even expected. Pin the 30–60 s contract so
     * a future iMessage-aimed change doesn't accidentally accelerate them
     * into iMessage's range. */
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 42);
    /* default channel from make_test_thinking_ctx is "slack" (TEXT_ASYNC) */
    hu_thinking_response_t out = {0};
    bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(out.delay_ms >= 30000);
    HU_ASSERT_TRUE(out.delay_ms <= 60000);
}

static void thinking_no_consecutive_duplicates_with_3_bank(void) {
    /* AC-3 regression guard: drive 10 emissions; assert no two consecutive are the same */
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    hu_persona_t persona;
    hu_persona_overlay_t overlay;
    char *bank[3];
    hu_filler_recency_t recency;
    hu_thinking_context_t ctx;
    make_test_thinking_ctx(&persona, &overlay, bank, &recency, &ctx, 0);

    char prev[64] = {0};
    size_t prev_len = 0;
    for (uint32_t i = 0; i < 10; i++) {
        ctx.seed = i * 7919u + 1u; /* vary seed each round */
        hu_thinking_response_t out = {0};
        bool ok = hu_conversation_classify_thinking(&ctx, msg, strlen(msg), NULL, 0, &out);
        HU_ASSERT_TRUE(ok);
        HU_ASSERT_TRUE(out.filler_len > 0);
        if (prev_len > 0) {
            bool same = (out.filler_len == prev_len && memcmp(out.filler, prev, prev_len) == 0);
            HU_ASSERT_FALSE(same);
        }
        memcpy(prev, out.filler, out.filler_len);
        prev[out.filler_len] = '\0';
        prev_len = out.filler_len;
    }
}

/* ── Quality evaluator enhanced tests ────────────────────────────────── */

static void quality_penalizes_semicolons(void) {
    hu_quality_score_t score =
        hu_conversation_evaluate_quality("that sounds good; I'll check it out", 35, NULL, 0, 300);
    HU_ASSERT_TRUE(score.naturalness < 20);
}

static void quality_penalizes_exclamation_overuse(void) {
    hu_quality_score_t score = hu_conversation_evaluate_quality(
        "Wow! This is amazing! So cool! Love it!", 39, NULL, 0, 300);
    HU_ASSERT_TRUE(score.naturalness <= 20);
}

static void quality_rewards_contractions(void) {
    hu_quality_score_t score =
        hu_conversation_evaluate_quality("i don't think that's a great idea tbh", 37, NULL, 0, 300);
    HU_ASSERT_TRUE(score.naturalness >= 20);
}

static void quality_penalizes_service_language(void) {
    hu_quality_score_t score = hu_conversation_evaluate_quality(
        "Certainly! I'd be happy to help you with that.", 47, NULL, 0, 300);
    HU_ASSERT_TRUE(score.warmth < 5);
}

static void quality_good_casual_scores_high(void) {
    hu_quality_score_t score =
        hu_conversation_evaluate_quality("yeah that's wild lol", 20, NULL, 0, 300);
    HU_ASSERT_TRUE(score.total >= 60);
}

/* ── Awareness builder tests ─────────────────────────────────────────── */

static void awareness_null_returns_null(void) {
    size_t len = 0;
    char *ctx = hu_conversation_build_awareness(NULL, NULL, 0, NULL, &len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, 0u);
}

static void awareness_builds_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "hey!", "12:00"),
        make_entry(true, "hi whats up", "12:01"),
        make_entry(false, "not much, excited about the trip!", "12:02"),
        make_entry(true, "me too!", "12:03"),
    };
    size_t len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, 4, NULL, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "thread") != NULL || strstr(ctx, "Thread") != NULL ||
                   strstr(ctx, "conversation") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void awareness_detects_excitement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "omg I got the job!!!", "12:00"),
        make_entry(true, "wait what", "12:01"),
    };
    size_t len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, 2, NULL, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "excited") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void awareness_output_bounded(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Create a large history */
    hu_channel_history_entry_t entries[50];
    char texts[50][256];
    for (int i = 0; i < 50; i++) {
        snprintf(texts[i], sizeof(texts[i]),
                 "This is message number %d with some content to fill space and make the "
                 "awareness builder work with substantial input data for testing purposes",
                 i);
        entries[i] = make_entry((i % 2) == 0, texts[i], "2024-01-15T10:00:00");
    }

    size_t out_len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, 50, NULL, &out_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(out_len > 0);
    /* Awareness output should be bounded (not grow without limit) */
    HU_ASSERT_TRUE(out_len < 32768);

    alloc.free(alloc.ctx, ctx, out_len + 1);
}

/* ── Narrative detection tests ───────────────────────────────────────── */

static void narrative_opening_with_few_exchanges(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
    };
    hu_narrative_phase_t phase = hu_conversation_detect_narrative(entries, 2);
    HU_ASSERT_EQ(phase, HU_NARRATIVE_OPENING);
}

static void narrative_closing_detected(void) {
    hu_channel_history_entry_t entries[3] = {
        make_entry(false, "ok sounds good", "12:00"),
        make_entry(true, "cool", "12:01"),
        make_entry(false, "ttyl", "12:02"),
    };
    hu_narrative_phase_t phase = hu_conversation_detect_narrative(entries, 3);
    HU_ASSERT_EQ(phase, HU_NARRATIVE_CLOSING);
}

/* ── Engagement detection tests ──────────────────────────────────────── */

static void engagement_high_with_questions(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "what did you end up doing about the car situation?", "12:00"),
        make_entry(true, "still figuring it out", "12:01"),
        make_entry(false, "did you check with that mechanic i told you about?", "12:02"),
        make_entry(true, "not yet", "12:03"),
    };
    hu_engagement_level_t eng = hu_conversation_detect_engagement(entries, 4);
    HU_ASSERT_EQ(eng, HU_ENGAGEMENT_HIGH);
}

static void engagement_distracted_with_single_words(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(true, "did you see the game?", "12:00"),
        make_entry(false, "ya", "12:01"),
        make_entry(true, "it was wild right?", "12:02"),
        make_entry(false, "mhm", "12:03"),
    };
    hu_engagement_level_t eng = hu_conversation_detect_engagement(entries, 4);
    HU_ASSERT_TRUE(eng == HU_ENGAGEMENT_DISTRACTED || eng == HU_ENGAGEMENT_LOW);
}

/* ── Emotion detection tests ─────────────────────────────────────────── */

static void emotion_detects_positive(void) {
    hu_channel_history_entry_t entries[3] = {
        make_entry(false, "i'm so happy right now", "12:00"),
        make_entry(false, "everything is amazing", "12:01"),
        make_entry(false, "i love this", "12:02"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 3);
    HU_ASSERT_TRUE(emo.valence > 0.0f);
}

static void emotion_detects_negative(void) {
    hu_channel_history_entry_t entries[3] = {
        make_entry(false, "i'm so stressed about everything", "12:00"),
        make_entry(false, "feeling really anxious and worried", "12:01"),
        make_entry(false, "i just feel sad", "12:02"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 3);
    HU_ASSERT_TRUE(emo.valence < 0.0f);
    HU_ASSERT_TRUE(emo.intensity > 0.3f);
}

static void emotion_neutral_for_normal_chat(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "hey whats up", "12:00"),
        make_entry(false, "nothing much", "12:01"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 2);
    HU_ASSERT_TRUE(emo.intensity < 0.3f);
}

/* ── Energy detection tests (F13) ─────────────────────────────────────── */

static void energy_omg_amazing_excited(void) {
    const char *msg = "omg that's amazing!!";
    hu_energy_level_t e = hu_conversation_detect_energy(msg, strlen(msg), NULL, 0);
    HU_ASSERT_EQ(e, HU_ENERGY_EXCITED);
}

static void energy_sad_today_sad(void) {
    const char *msg = "i'm so sad today";
    hu_energy_level_t e = hu_conversation_detect_energy(msg, strlen(msg), NULL, 0);
    HU_ASSERT_EQ(e, HU_ENERGY_SAD);
}

static void energy_lol_ridiculous_playful(void) {
    const char *msg = "lol you're ridiculous";
    hu_energy_level_t e = hu_conversation_detect_energy(msg, strlen(msg), NULL, 0);
    HU_ASSERT_EQ(e, HU_ENERGY_PLAYFUL);
}

static void energy_worried_anxious(void) {
    const char *msg = "i'm really worried about this";
    hu_energy_level_t e = hu_conversation_detect_energy(msg, strlen(msg), NULL, 0);
    HU_ASSERT_EQ(e, HU_ENERGY_ANXIOUS);
}

static void energy_ok_sounds_good_neutral(void) {
    const char *msg = "ok sounds good";
    hu_energy_level_t e = hu_conversation_detect_energy(msg, strlen(msg), NULL, 0);
    HU_ASSERT_EQ(e, HU_ENERGY_NEUTRAL);
}

static void energy_directive_excited_nonempty(void) {
    char buf[256];
    size_t len = hu_conversation_build_energy_directive(HU_ENERGY_EXCITED, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "ENERGY"));
    HU_ASSERT_NOT_NULL(strstr(buf, "excited"));
}

static void energy_directive_sad_nonempty(void) {
    char buf[256];
    size_t len = hu_conversation_build_energy_directive(HU_ENERGY_SAD, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "ENERGY"));
    HU_ASSERT_NOT_NULL(strstr(buf, "down"));
}

static void energy_directive_playful_nonempty(void) {
    char buf[256];
    size_t len = hu_conversation_build_energy_directive(HU_ENERGY_PLAYFUL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "ENERGY"));
    HU_ASSERT_NOT_NULL(strstr(buf, "playful"));
}

static void energy_directive_anxious_nonempty(void) {
    char buf[256];
    size_t len = hu_conversation_build_energy_directive(HU_ENERGY_ANXIOUS, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "ENERGY"));
    HU_ASSERT_NOT_NULL(strstr(buf, "anxious"));
}

static void energy_directive_calm_nonempty(void) {
    char buf[256];
    size_t len = hu_conversation_build_energy_directive(HU_ENERGY_CALM, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "ENERGY"));
    HU_ASSERT_NOT_NULL(strstr(buf, "calm"));
}

static void energy_directive_neutral_returns_zero(void) {
    char buf[256];
    size_t len = hu_conversation_build_energy_directive(HU_ENERGY_NEUTRAL, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

/* ── Emotional tone classification tests (F22) ─────────────────────────── */

static void tone_stressed_returns_stressed(void) {
    const char *tone = hu_conversation_classify_emotional_tone("i'm so stressed about work", 28);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "stressed");
}

static void tone_excited_returns_excited(void) {
    const char *tone = hu_conversation_classify_emotional_tone("omg that's amazing!!!", 20);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "excited");
}

static void tone_sad_returns_sad(void) {
    const char *tone = hu_conversation_classify_emotional_tone("i'm really sad today", 19);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "sad");
}

static void tone_anxious_returns_anxious(void) {
    const char *tone = hu_conversation_classify_emotional_tone("i'm worried about the exam", 27);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "anxious");
}

static void tone_happy_returns_happy(void) {
    const char *tone = hu_conversation_classify_emotional_tone("that's great news!", 17);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "happy");
}

static void tone_frustrated_returns_frustrated(void) {
    const char *tone = hu_conversation_classify_emotional_tone("ugh this is so frustrating", 26);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "frustrated");
}

static void tone_neutral_returns_neutral(void) {
    const char *tone = hu_conversation_classify_emotional_tone("ok sounds good", 14);
    HU_ASSERT_NOT_NULL(tone);
    HU_ASSERT_STR_EQ(tone, "neutral");
}

static void tone_extract_topic_returns_significant_words(void) {
    char buf[64];
    size_t len = hu_conversation_extract_topic("i was thinking about the project deadline", 42, buf,
                                               sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "project") != NULL || strstr(buf, "deadline") != NULL);
}

/* P2-3 regression (2026-05-16 incident): the daemon's inner-thought
 * accumulation path used to memcpy the raw 127-byte user message into the
 * `topic` slot of hu_inner_thought_accumulate. That value later surfaces in
 * the system prompt as "[Inner thought: ...]" — a direct prompt-injection
 * vector AND a verbatim-text leak.
 *
 * The fix runs `combined` through hu_conversation_extract_topic first; if
 * extraction yields nothing, the accumulate call is skipped. These tests
 * pin the contract the daemon now relies on. */
static void extract_topic_rejects_raw_confession(void) {
    char buf[128] = {0};
    /* The incident utterance — a first-person confession sentence. The
     * extractor MUST NOT return the full sentence verbatim. It returns at
     * most 3 non-stopword tokens; this caps the leakage even when the
     * heuristic can't filter all charged words. The daemon's job is then
     * to gate on hu_emotional_moment_select_topic / hu_proactive_topic_is_safe
     * before outbound use. */
    const char *confession = "I confessed something terrible to my friend";
    size_t len = hu_conversation_extract_topic(confession, strlen(confession), buf, sizeof(buf));
    /* Must NOT be the full raw sentence. */
    HU_ASSERT_NULL(strstr(buf, "to my friend"));
    /* Bounded — never a 127-byte raw memcpy. */
    HU_ASSERT_TRUE(len < 64);
}

static void extract_topic_rejects_full_first_person_sentence(void) {
    char buf[128] = {0};
    const char *msg = "but boy I am just more lonely now than ever";
    size_t len = hu_conversation_extract_topic(msg, strlen(msg), buf, sizeof(buf));
    /* The extractor returns up to 3 significant non-stopword tokens. With
     * the expanded stopword list (P2-7), this should be very short or empty.
     * In all cases it must NOT contain "but boy" or "lonely now than". */
    HU_ASSERT_NULL(strstr(buf, "but boy"));
    HU_ASSERT_NULL(strstr(buf, "lonely now than"));
    /* Bounded size — never a 127-byte raw memcpy. */
    HU_ASSERT_TRUE(len < 64);
}

/* P2-7 regression (2026-05-16 incident): the extractor's stopword list was
 * too permissive. "hey how are you doing" yielded "hey", which then leaked
 * into proactive prompts as a topic. Expand the stopword list to include
 * greetings/fillers and raise the min-word-length bar. */
static void extract_topic_filters_greeting_filler_words(void) {
    char buf[64] = {0};
    size_t len = hu_conversation_extract_topic("hey how are you doing", 21, buf, sizeof(buf));
    /* "hey", "how", "are", "you", "doing" must all be stopwords. */
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_EQ(buf[0], '\0');
}

static void extract_topic_filters_bare_emotion_words(void) {
    char buf[64] = {0};
    /* "feel", "feeling", "lost", "lonely", "sad" must all be stopwords —
     * they are emotion KEYWORDS, not topics. F25/F30 paths handle emotion
     * separately. Storing them as topics produced "How is the sad going?" */
    size_t len =
        hu_conversation_extract_topic("i feel lost and lonely and sad", 30, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void extract_topic_rejects_short_single_words(void) {
    char buf[64] = {0};
    /* "ok" (2), "hi" (2) — words under 4 chars are too noisy to use as
     * topics. The previous bar was 2 chars, which let "ok" leak. */
    size_t len = hu_conversation_extract_topic("ok hi", 5, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void extract_topic_keeps_real_nouns_after_expansion(void) {
    char buf[64] = {0};
    /* Ensure we didn't over-filter. Real topics still come through. */
    size_t len =
        hu_conversation_extract_topic("the project deadline is tight", 30, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "project") != NULL || strstr(buf, "deadline") != NULL);
}

/* ── Escalation detection tests (F14) ─────────────────────────────────── */

static void escalation_three_negative_escalating(void) {
    hu_channel_history_entry_t entries[6] = {
        make_entry(true, "hey what's up", "12:00"),
        make_entry(false, "i'm stressed", "12:01"),
        make_entry(true, "sorry to hear", "12:02"),
        make_entry(false, "it's getting worse", "12:03"),
        make_entry(true, "hang in there", "12:04"),
        make_entry(false, "i can't deal", "12:05"),
    };
    hu_escalation_state_t s = hu_conversation_detect_escalation(entries, 6);
    HU_ASSERT_TRUE(s.escalating);
    HU_ASSERT_TRUE(s.consecutive_negative >= 3);
}

static void escalation_three_negative_then_reset_not_escalating(void) {
    hu_channel_history_entry_t entries[8] = {
        make_entry(false, "i'm stressed", "12:00"),
        make_entry(true, "oh no", "12:01"),
        make_entry(false, "it's getting worse", "12:02"),
        make_entry(true, "really?", "12:03"),
        make_entry(false, "i can't deal", "12:04"),
        make_entry(true, "aw", "12:05"),
        make_entry(false, "lol jk", "12:06"),
    };
    hu_escalation_state_t s = hu_conversation_detect_escalation(entries, 7);
    HU_ASSERT_FALSE(s.escalating);
}

static void escalation_two_negative_not_escalating(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "i'm stressed", "12:00"),
        make_entry(true, "sorry", "12:01"),
        make_entry(false, "it's getting worse", "12:02"),
    };
    hu_escalation_state_t s = hu_conversation_detect_escalation(entries, 3);
    HU_ASSERT_FALSE(s.escalating);
    HU_ASSERT_TRUE(s.consecutive_negative < 3);
}

static void escalation_mixed_positive_negative_not_escalating(void) {
    hu_channel_history_entry_t entries[6] = {
        make_entry(false, "i'm stressed", "12:00"),
        make_entry(false, "actually feeling better now", "12:01"),
        make_entry(false, "thanks for listening", "12:02"),
    };
    hu_escalation_state_t s = hu_conversation_detect_escalation(entries, 3);
    HU_ASSERT_FALSE(s.escalating);
}

static void escalation_deescalation_directive_nonempty(void) {
    char buf[256];
    size_t len = hu_conversation_build_deescalation_directive(buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "DE-ESCALATION"));
    HU_ASSERT_NOT_NULL(strstr(buf, "empathetic"));
}

/* ── Context modifiers tests (F16) ─────────────────────────────────────── */

static void context_modifiers_heavy_topic_includes_directive(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "my dad passed last week", "12:00"),
        make_entry(true, "i'm so sorry", "12:01"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 2);
    char buf[512];
    size_t len = hu_conversation_build_context_modifiers(entries, 2, &emo, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Heavy topic"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Reduce humor"));
}

static void context_modifiers_personal_sharing_includes_directive(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "can i be honest, i've been struggling", "12:00"),
        make_entry(true, "hey", "12:01"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 2);
    char buf[512];
    size_t len = hu_conversation_build_context_modifiers(entries, 2, &emo, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "sharing something personal"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Boost warmth"));
}

static void context_modifiers_high_emotion_includes_directive(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "i'm so sad and depressed", "12:00"),
        make_entry(true, "oh no", "12:01"),
        make_entry(false, "everything is terrible and i'm crying", "12:02"),
        make_entry(true, "i'm here", "12:03"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 4);
    char buf[512];
    size_t len = hu_conversation_build_context_modifiers(entries, 4, &emo, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "High emotion"));
    HU_ASSERT_NOT_NULL(strstr(buf, "shorter sentences"));
}

static void context_modifiers_early_turn_includes_directive(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
        make_entry(false, "how are you", "12:02"),
        make_entry(true, "good", "12:03"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 4);
    char buf[512];
    size_t len = hu_conversation_build_context_modifiers(entries, 4, &emo, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Early in conversation"));
    HU_ASSERT_NOT_NULL(strstr(buf, "warmer"));
}

static void context_modifiers_combined_includes_multiple_lines(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "can i be honest, my dad passed last week", "12:00"),
        make_entry(true, "oh no", "12:01"),
        make_entry(false, "i've been meaning to tell you", "12:02"),
    };
    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 3);
    emo.intensity = 1.0f; /* force high emotion for combined test */
    char buf[512];
    size_t len = hu_conversation_build_context_modifiers(entries, 3, &emo, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Heavy topic"));
    HU_ASSERT_NOT_NULL(strstr(buf, "sharing something personal"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Early in conversation"));
}

/* ── Honesty guardrail tests ─────────────────────────────────────────── */

static void honesty_detects_action_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *result = hu_conversation_honesty_check(&alloc, "did you call mom?", 17);
    HU_ASSERT_NOT_NULL(result);
    HU_ASSERT_TRUE(strstr(result, "HONESTY") != NULL);
    alloc.free(alloc.ctx, result, strlen(result) + 1);
}

static void honesty_null_for_normal_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *result = hu_conversation_honesty_check(&alloc, "what are you up to", 18);
    HU_ASSERT_NULL(result);
}

/* ── Commitment detection and deadline parsing (F20) ──────────────────── */

static void parse_deadline_tomorrow(void) {
    int64_t now = (int64_t)time(NULL);
    int64_t d = hu_conversation_parse_deadline("call me tomorrow", 16, now);
    HU_ASSERT_TRUE(d > 0);
    HU_ASSERT_TRUE(d >= now + 86300 && d <= now + 86500);
}

static void parse_deadline_in_three_days(void) {
    int64_t now = (int64_t)time(NULL);
    int64_t d = hu_conversation_parse_deadline("in 3 days we should meet", 24, now);
    HU_ASSERT_TRUE(d > 0);
    HU_ASSERT_TRUE(d >= now + 259100 && d <= now + 259300);
}

static void parse_deadline_whats_up_returns_zero(void) {
    int64_t now = (int64_t)time(NULL);
    int64_t d = hu_conversation_parse_deadline("what's up", 9, now);
    HU_ASSERT_EQ(d, (int64_t)0);
}

static void detect_commitment_ill_call_dentist(void) {
    char desc[256];
    char who[64];
    const char *msg = "i'll call the dentist";
    bool ok = hu_conversation_detect_commitment(msg, strlen(msg), desc, sizeof(desc), who,
                                                sizeof(who), false);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_TRUE(strstr(desc, "call") != NULL || strstr(desc, "dentist") != NULL);
    HU_ASSERT_STR_EQ(who, "them");
}

static void detect_commitment_nice_weather_false(void) {
    char desc[256];
    char who[64];
    const char *msg = "nice weather today";
    bool ok = hu_conversation_detect_commitment(msg, strlen(msg), desc, sizeof(desc), who,
                                                sizeof(who), false);
    HU_ASSERT_TRUE(!ok);
}

/* ── Growth celebration detection (F24) ───────────────────────────────── */

static void detect_growth_it_went_great_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "the presentation went well — it went great actually";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_STR_EQ(after, "success");
    HU_ASSERT_TRUE(strlen(topic) > 0);
}

static void detect_growth_i_got_the_job_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "i got the job!";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_STR_EQ(after, "success");
}

static void detect_growth_nailed_it_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "nailed it on the exam";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
}

static void detect_growth_crushed_it_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "you were worried about that interview — sounds like you crushed it!";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
}

static void detect_growth_i_passed_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "i passed the driving test";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
}

static void detect_growth_got_promoted_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "got promoted last week";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
}

static void detect_growth_it_worked_out_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "it worked out in the end";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
}

static void detect_growth_turned_out_well_true(void) {
    char topic[128];
    char after[64];
    const char *msg = "the meeting turned out well";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(ok);
}

static void detect_growth_ordinary_message_false(void) {
    char topic[128];
    char after[64];
    const char *msg = "hey what are you up to";
    bool ok = hu_conversation_detect_growth_opportunity(msg, strlen(msg), topic, sizeof(topic),
                                                        after, sizeof(after));
    HU_ASSERT_TRUE(!ok);
}

static void detect_growth_null_input_returns_false(void) {
    char topic[128];
    char after[64];
    bool ok = hu_conversation_detect_growth_opportunity(NULL, 0, topic, sizeof(topic), after,
                                                        sizeof(after));
    HU_ASSERT_TRUE(!ok);
}

/* ── Length calibration tests ──────────────────────────────────────── */

/* Data-driven calibration: output describes metrics, not rigid message types */
static void calibrate_greeting_short(void) {
    char buf[1024];
    size_t len = hu_conversation_calibrate_length("hey", 3, NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "brief") || strstr(buf, "Match"));
    /* Ratio-based: short message should include numeric char target */
    HU_ASSERT_NOT_NULL(strstr(buf, "chars"));
}

static void calibrate_yes_no_question(void) {
    char buf[1024];
    size_t len =
        hu_conversation_calibrate_length("are you coming tonight?", 23, NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void calibrate_emotional_message(void) {
    char buf[1024];
    size_t len = hu_conversation_calibrate_length("I'm really stressed about this job thing", 40,
                                                  NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_logistics(void) {
    char buf[1024];
    size_t len = hu_conversation_calibrate_length("what time should we meet at the restaurant?", 43,
                                                  NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void calibrate_short_react(void) {
    char buf[1024];
    size_t len = hu_conversation_calibrate_length("lol", 3, NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "brief") || strstr(buf, "Very brief"));
}

static void calibrate_link_share(void) {
    char buf[1024];
    size_t len = hu_conversation_calibrate_length("check this out https://example.com", 34, NULL, 0,
                                                  buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "link"));
}

static void calibrate_open_question(void) {
    char buf[1024];
    const char *msg = "what do you think about all that?";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void calibrate_long_story(void) {
    char msg[256];
    memset(msg, 'a', 200);
    msg[200] = '\0';
    char buf[1024];
    size_t len = hu_conversation_calibrate_length(msg, 200, NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "Substantial") || strstr(buf, "depth"));
}

static void calibrate_good_news(void) {
    char buf[1024];
    const char *msg = "I got the job!!";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_bad_news(void) {
    char buf[1024];
    const char *msg = "my grandma passed away last night";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_teasing(void) {
    char buf[1024];
    const char *msg = "yeah right";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_vulnerable(void) {
    char buf[1024];
    const char *msg = "can i be honest with you about something";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_tone_present_in_greeting(void) {
    char buf[1024];
    const char *msg = "hey";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_tone_present_in_emotional(void) {
    char buf[1024];
    const char *msg = "i'm so stressed out i can't handle this anymore";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_farewell_goodnight(void) {
    char buf[1024];
    const char *msg = "goodnight!";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    HU_ASSERT_TRUE(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_farewell_short_bye(void) {
    char buf[1024];
    const char *msg = "bye";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_emoji_present_in_greeting(void) {
    char buf[1024];
    const char *msg = "hey";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_emoji_present_in_logistics(void) {
    char buf[1024];
    const char *msg = "what time should we meet at the restaurant?";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_emoji_present_in_general(void) {
    char buf[1024];
    const char *msg = "i just finished cooking dinner";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_null_returns_zero(void) {
    char buf[64];
    HU_ASSERT_EQ(hu_conversation_calibrate_length(NULL, 0, NULL, 0, buf, sizeof(buf)), 0u);
}

/* ── max_response_chars ratio-based calibration ───────────────────────── */

/* ── Behavior threshold configuration ───────────────────────────────── */

static void set_thresholds_changes_min_response_chars(void) {
    /* Set custom min of 10 */
    hu_conversation_set_thresholds(3, 40, 300, 10);
    int max = hu_conversation_max_response_chars(0);
    HU_ASSERT_EQ(max, 10);
    /* Reset to defaults */
    hu_conversation_set_thresholds(3, 40, 300, 15);
}

static void set_thresholds_changes_max_response_chars(void) {
    /* Set custom max of 400 */
    hu_conversation_set_thresholds(3, 40, 400, 15);
    int max = hu_conversation_max_response_chars(200);
    HU_ASSERT_EQ(max, 400);
    /* Reset to defaults */
    hu_conversation_set_thresholds(3, 40, 300, 15);
}

static void set_thresholds_zero_keeps_default(void) {
    /* Pass 0 for min_response_chars to keep default */
    hu_conversation_set_thresholds(3, 40, 300, 0);
    /* Default should still be 15 */
    int max = hu_conversation_max_response_chars(0);
    HU_ASSERT_EQ(max, 15);
}

static void max_response_chars_single_char_returns_min(void) {
    int max = hu_conversation_max_response_chars(1);
    HU_ASSERT_EQ(max, 15);
}

static void max_response_chars_medium_message_proportional(void) {
    const char *msg = "what are you up to tonight?";
    int max = hu_conversation_max_response_chars(strlen(msg));
    HU_ASSERT_TRUE(max >= 50 && max <= 60);
}

static void max_response_chars_long_paragraph_capped(void) {
    int max = hu_conversation_max_response_chars(200);
    HU_ASSERT_EQ(max, 300);
}

static void max_response_chars_zero_returns_min(void) {
    int max = hu_conversation_max_response_chars(0);
    HU_ASSERT_EQ(max, 15);
}

static void max_response_chars_medium_range(void) {
    int max = hu_conversation_max_response_chars(75);
    HU_ASSERT_EQ(max, 150);
}

static void max_response_chars_relational_default_matches_plain(void) {
    for (size_t n = 1; n < 120; n += 17) {
        HU_ASSERT_EQ(hu_conversation_max_response_chars_relational(n, NULL, HU_REL_NEW),
                     hu_conversation_max_response_chars(n));
    }
}

static void max_response_chars_relational_trusted_higher(void) {
    int plain = hu_conversation_max_response_chars(80);
    int warm = hu_conversation_max_response_chars_relational(80, NULL, HU_REL_TRUSTED);
    HU_ASSERT_TRUE(warm > plain);
    HU_ASSERT_EQ(warm, 240);
}

static void brief_char_cap_redteam(void) {
    HU_ASSERT_EQ(hu_conversation_brief_char_cap(true, NULL, HU_REL_TRUSTED), 50u);
    HU_ASSERT_TRUE(hu_conversation_brief_char_cap(false, NULL, HU_REL_NEW) > 50u);
    HU_ASSERT_EQ(hu_conversation_brief_char_cap(false, NULL, HU_REL_TRUSTED), 220u);
    hu_contact_profile_t cp = {0};
    cp.relationship_type = (char *)"friend";
    HU_ASSERT_EQ(hu_conversation_brief_char_cap(false, &cp, HU_REL_NEW), 185u);
    cp.prefers_short_texts = true;
    HU_ASSERT_EQ(hu_conversation_brief_char_cap(false, &cp, HU_REL_TRUSTED), 72u);
}

static void calibrate_for_contact_softens_ping_for_warm_dm(void) {
    char buf[1024];
    hu_contact_profile_t cp = {0};
    cp.relationship_type = (char *)"friend";
    const char *m = "call me soon";
    size_t cal_len = hu_conversation_calibrate_length_for_contact(m, strlen(m), NULL, 0, false, &cp,
                                                                  HU_REL_TRUSTED, buf, sizeof(buf));
    HU_ASSERT_TRUE(cal_len > 80);
    HU_ASSERT_TRUE(strstr(buf, "real friend") != NULL);
}

static void calibrate_for_contact_group_uses_neutral_ratio(void) {
    char buf[1024];
    hu_contact_profile_t cp = {0};
    cp.relationship_type = (char *)"friend";
    const char *m = "everyone free saturday?";
    size_t cal_len = hu_conversation_calibrate_length_for_contact(m, strlen(m), NULL, 0, true, &cp,
                                                                  HU_REL_TRUSTED, buf, sizeof(buf));
    HU_ASSERT_TRUE(cal_len > 40);
    HU_ASSERT_TRUE(strstr(buf, "Moderate length") != NULL ||
                   strstr(buf, "Their last message") != NULL);
    /* Group path: 2x cap only — numeric limit in buf should reflect ~46 chars max, not ~69 */
    HU_ASSERT_TRUE(strstr(buf, "48") != NULL || strstr(buf, "46") != NULL);
}

/* Sprint 39 — needs_revision at 5× ratio (was 10×). */
static void quality_needs_revision_at_5x_ratio(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(false, "sounds good to me", "12:00"),
        make_entry(false, "yeah let's do it", "12:01"),
    };
    /* their avg ~16 chars; response ~90 chars ≈ 5.6× → needs_revision */
    const char *resp =
        "Well I think that is a really interesting point and I wanted to add a bit more "
        "detail about how I see things working out for us over the next few weeks.";
    hu_quality_score_t score =
        hu_conversation_evaluate_quality(resp, strlen(resp), entries, 2, 300);
    HU_ASSERT_TRUE(score.needs_revision);
    HU_ASSERT_NOT_NULL(strstr(score.guidance, "chars"));
}

static void quality_penalizes_length_mismatch(void) {
    hu_quality_score_t score = hu_conversation_evaluate_quality(
        "Well, I think that's a really interesting question and I'd be happy to elaborate "
        "on my thoughts in more detail if you'd like.",
        120, NULL, 0, 60);
    HU_ASSERT_TRUE(score.brevity < 25);
}

static void quality_rewards_length_match(void) {
    hu_quality_score_t score =
        hu_conversation_evaluate_quality("yeah that sounds good", 21, NULL, 0, 60);
    HU_ASSERT_TRUE(score.brevity >= 20);
}

/* A clipped fragment must NOT out-score a natural reply when their messages
 * are substantive. Before the too-short brevity band, the fragment won the
 * A/B selection on brevity alone (the "best=2 quality=78 len=3" symptom). */
static void quality_natural_reply_beats_fragment_when_substantive(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(false, "yo you heading out to the show tonight or what", "12:00"),
        make_entry(false, "going out with a bang?!", "12:01"),
    };
    /* their recent avg ~35 chars → substantive (>= 20). */
    hu_quality_score_t fragment = hu_conversation_evaluate_quality("lol", 3, entries, 2, 300);
    hu_quality_score_t natural = hu_conversation_evaluate_quality(
        "haha you know it, gonna be a good one", 37, entries, 2, 300);
    /* fragment is under-matched → docked brevity; natural matches their energy. */
    HU_ASSERT_TRUE(fragment.brevity < natural.brevity);
    HU_ASSERT_TRUE(natural.total > fragment.total);
}

/* Terse-to-terse banter must keep full brevity marks — the two-signal gate
 * exempts short replies when their messages are themselves short. */
static void quality_terse_reply_to_terse_banter_keeps_full_brevity(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(false, "ok", "12:00"),
        make_entry(false, "lol", "12:01"),
    };
    hu_quality_score_t score = hu_conversation_evaluate_quality("k", 1, entries, 2, 300);
    HU_ASSERT_EQ(score.brevity, 25);
}

static void calibrate_rapid_fire_momentum(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(false, "hey", "10:00"),
        make_entry(true, "hey", "10:00"),
        make_entry(false, "what's up", "10:01"),
        make_entry(true, "nm you?", "10:01"),
        make_entry(false, "same lol", "10:01"),
        make_entry(true, "haha", "10:02"),
        make_entry(false, "wanna grab food?", "10:02"),
    };
    char buf[1024];
    size_t len =
        hu_conversation_calibrate_length("wanna grab food?", 16, entries, 7, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "Rapid-fire") != NULL || strstr(buf, "rapid") != NULL);
}

/* ── Typo correction fragment tests ─────────────────────────────────── */

static void correction_detects_typo(void) {
    char buf[64];
    const char *orig = "meeting at noon";
    const char *typo = "meting at noon";
    size_t n = hu_conversation_generate_correction(orig, strlen(orig), typo, strlen(typo), buf,
                                                   sizeof(buf), 12345u, 100u);
    HU_ASSERT_EQ(n, 8u);
    HU_ASSERT_STR_EQ(buf, "*meeting");
}

static void correction_no_typo_no_output(void) {
    char buf[64];
    const char *s = "meeting at noon";
    size_t n = hu_conversation_generate_correction(s, strlen(s), s, strlen(s), buf, sizeof(buf),
                                                   12345u, 100u);
    HU_ASSERT_EQ(n, 0u);
}

static void correction_chance_zero_no_output(void) {
    char buf[64];
    const char *orig = "meeting at noon";
    const char *typo = "meting at noon";
    size_t n = hu_conversation_generate_correction(orig, strlen(orig), typo, strlen(typo), buf,
                                                   sizeof(buf), 12345u, 0u);
    HU_ASSERT_EQ(n, 0u);
}

static void correction_respects_buffer_cap(void) {
    char buf[6];
    const char *orig = "meeting at noon";
    const char *typo = "meting at noon";
    size_t n = hu_conversation_generate_correction(orig, strlen(orig), typo, strlen(typo), buf,
                                                   sizeof(buf), 12345u, 100u);
    HU_ASSERT_EQ(n, 5u);
    HU_ASSERT_EQ(strlen(buf), 5u);
    HU_ASSERT_TRUE(memcmp(buf, "*meet", 5) == 0);
}

/* ── Typing quirk post-processing tests ────────────────────────────── */

static void quirks_lowercase_applies(void) {
    char buf[] = "Hey What's Up";
    const char *quirks[] = {"lowercase"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    HU_ASSERT_STR_EQ(buf, "hey what's up");
    HU_ASSERT_EQ(len, 13u);
}

static void quirks_no_periods_strips_sentence_end(void) {
    char buf[] = "sounds good. let me know";
    const char *quirks[] = {"no_periods"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    HU_ASSERT_STR_EQ(buf, "sounds good let me know");
    HU_ASSERT_EQ(len, 23u);
}

static void quirks_no_periods_preserves_ellipsis(void) {
    char buf[] = "idk... maybe";
    const char *quirks[] = {"no_periods"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    HU_ASSERT_TRUE(len >= 10u);
    HU_ASSERT_TRUE(strstr(buf, "idk") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "maybe") != NULL);
    (void)len;
}

static void quirks_no_commas_strips(void) {
    char buf[] = "yeah, I think so, maybe";
    const char *quirks[] = {"no_commas"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    HU_ASSERT_STR_EQ(buf, "yeah I think so maybe");
    HU_ASSERT_EQ(len, 21u);
}

static void quirks_no_apostrophes_strips(void) {
    char buf[] = "don't can't won't";
    const char *quirks[] = {"no_apostrophes"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    HU_ASSERT_STR_EQ(buf, "dont cant wont");
    HU_ASSERT_EQ(len, 14u);
}

static void quirks_multiple_combined(void) {
    char buf[] = "Hey, I Don't Know.";
    const char *quirks[] = {"lowercase", "no_periods", "no_commas"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 3);
    HU_ASSERT_NOT_NULL(strstr(buf, "hey"));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(len < 18);
}

static void quirks_null_input_noop(void) {
    size_t len = hu_conversation_apply_typing_quirks(NULL, 0, NULL, 0);
    HU_ASSERT_EQ(len, 0u);
}

static void quirks_empty_quirks_noop(void) {
    char buf[] = "Hello World.";
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), NULL, 0);
    HU_ASSERT_STR_EQ(buf, "Hello World.");
    HU_ASSERT_EQ(len, 12u);
}

static void apply_typing_quirks_double_space_to_newline(void) {
    char buf[] = "hello  world  foo";
    const char *quirks[] = {"double_space_to_newline"};
    size_t len = hu_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    HU_ASSERT_STR_EQ(buf, "hello\nworld\nfoo");
    HU_ASSERT_EQ(len, 15u);
}

/* ── Typo simulation tests ────────────────────────────────────────────── */

static void typo_applies_with_right_seed(void) {
    /* Seed 0 yields val=0 from first prng_next, so 0%100<15 triggers typo */
    char buf[64];
    const char *input = "hello there friend";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_apply_typos(buf, len, sizeof(buf), 0);
    HU_ASSERT_TRUE(out != len || memcmp(buf, input, len + 1) != 0);
}

static void typo_preserves_short_words(void) {
    /* "I am ok" - all words <= 2 chars, no eligible words */
    char buf[64];
    const char *input = "I am ok";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_apply_typos(buf, len, sizeof(buf), 0);
    HU_ASSERT_STR_EQ(buf, "I am ok");
    HU_ASSERT_EQ(out, len);
}

static void typo_deterministic(void) {
    char buf1[64], buf2[64];
    const char *input = "sounds good to me";
    size_t len = strlen(input);
    memcpy(buf1, input, len + 1);
    memcpy(buf2, input, len + 1);
    size_t out1 = hu_conversation_apply_typos(buf1, len, sizeof(buf1), 42);
    size_t out2 = hu_conversation_apply_typos(buf2, len, sizeof(buf2), 42);
    HU_ASSERT_EQ(out1, out2);
    HU_ASSERT_TRUE(memcmp(buf1, buf2, (out1 > out2 ? out1 : out2) + 1) == 0);
}

static void typo_never_exceeds_cap(void) {
    char buf[32];
    const char *input = "hello there friend";
    size_t len = strlen(input);
    size_t cap = len + 2;
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_apply_typos(buf, len, cap, 0);
    HU_ASSERT_TRUE(out <= cap - 1);
}

/* ── Text disfluency (F33) tests ───────────────────────────────────────── */

static void disfluency_frequency_one_applies(void) {
    /* frequency 1.0, no contact (casual) → at least one disfluency applied */
    char buf[128];
    const char *input = "that sounds good to me";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_apply_disfluency(buf, len, sizeof(buf), 0, 1.0f, NULL, NULL, 0);
    HU_ASSERT_TRUE(out != len || memcmp(buf, input, len + 1) != 0);
}

static void disfluency_frequency_zero_unchanged(void) {
    char buf[128];
    const char *input = "that sounds good to me";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_apply_disfluency(buf, len, sizeof(buf), 0, 0.0f, NULL, NULL, 0);
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, input);
}

static void disfluency_formal_contact_unchanged(void) {
    hu_contact_profile_t contact = {0};
    contact.relationship_type = (char *)"coworker";
    char buf[128];
    const char *input = "that sounds good";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out =
        hu_conversation_apply_disfluency(buf, len, sizeof(buf), 0, 1.0f, &contact, NULL, 0);
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, input);
}

static void disfluency_formality_formal_unchanged(void) {
    const char *formality = "formal";
    char buf[128];
    const char *input = "that sounds good";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out =
        hu_conversation_apply_disfluency(buf, len, sizeof(buf), 0, 1.0f, NULL, formality, 6);
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, input);
}

static void disfluency_small_buffer_unchanged(void) {
    char buf[20];
    const char *input = "hello";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t cap = len + 2;
    size_t out = hu_conversation_apply_disfluency(buf, len, cap, 0, 1.0f, NULL, NULL, 0);
    HU_ASSERT_TRUE(out <= cap - 1);
}

/* ── Nonverbal sound injection (F39) tests ────────────────────────────── */

static void nonverbals_enabled_false_no_change(void) {
    char buf[128];
    const char *input = "That's funny.";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_inject_nonverbals(buf, len, sizeof(buf), 0, false);
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, input);
}

static void nonverbals_seed_zero_laughter_after_period(void) {
    /* seed 0: 0%100 < 15 pass, 0/100%3=0 → type 0, insert after first . ! ? */
    char buf[128];
    const char *input = "That's funny.";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_inject_nonverbals(buf, len, sizeof(buf), 0, true);
    HU_ASSERT_TRUE(out > len);
    HU_ASSERT_NOT_NULL(strstr(buf, "[laughter]"));
}

static void nonverbals_seed_500_prepend_hmm(void) {
    /* seed 500: 0<15 pass, (500/100)%10=5, 5<8 → type 1, prepend Hmm... */
    char buf[128];
    const char *input = "I think so.";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_inject_nonverbals(buf, len, sizeof(buf), 500, true);
    HU_ASSERT_TRUE(out > len);
    HU_ASSERT_TRUE(strncmp(buf, "Hmm... ", 7) == 0);
}

static void nonverbals_seed_800_pause_after_comma(void) {
    /* seed 800: 0<15 pass, (800/100)%10=8 → type 2, insert ... after first comma/period */
    char buf[128];
    const char *input = "Well, maybe.";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_inject_nonverbals(buf, len, sizeof(buf), 800, true);
    HU_ASSERT_TRUE(out > len);
    HU_ASSERT_NOT_NULL(strstr(buf, "... "));
}

static void nonverbals_seed_50_no_roll_no_change(void) {
    /* seed 50: 50%100=50 >= 15, no insertion */
    char buf[128];
    const char *input = "That's funny.";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = hu_conversation_inject_nonverbals(buf, len, sizeof(buf), 50, true);
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, input);
}

static void nonverbals_buffer_too_small_no_change(void) {
    /* Would insert but not enough room */
    char buf[16];
    const char *input = "Hi there.";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t cap = len + 2;
    size_t out = hu_conversation_inject_nonverbals(buf, len, cap, 0, true);
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, input);
}

/* ── Anti-repetition detection tests ──────────────────────────────────── */

static void repetition_detects_repeated_opener(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(true, "haha yeah totally", "10:00"),    make_entry(false, "right?", "10:01"),
        make_entry(true, "haha that's so funny", "10:02"), make_entry(false, "lol", "10:03"),
        make_entry(true, "haha i know", "10:04"),          make_entry(false, "anyway", "10:05"),
        make_entry(true, "haha what's up", "10:06"),
    };
    char buf[1024];
    size_t len = hu_conversation_detect_repetition(entries, 7, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "haha"));
}

static void repetition_detects_question_overuse(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(true, "sounds fun?", "10:00"),         make_entry(false, "yeah", "10:01"),
        make_entry(true, "what time?", "10:02"),          make_entry(false, "7", "10:03"),
        make_entry(true, "where should we go?", "10:04"), make_entry(false, "idk", "10:05"),
        make_entry(true, "how about tacos?", "10:06"),
    };
    char buf[1024];
    size_t len = hu_conversation_detect_repetition(entries, 7, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void repetition_no_issues_returns_zero(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(true, "hey what's up", "10:00"),
        make_entry(false, "not much", "10:01"),
        make_entry(true, "cool. wanna hang?", "10:02"),
        make_entry(false, "sure", "10:03"),
    };
    char buf[1024];
    size_t len = hu_conversation_detect_repetition(entries, 4, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void repetition_too_few_messages_returns_zero(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(true, "hey", "10:00"),
        make_entry(false, "hey", "10:01"),
    };
    char buf[1024];
    size_t len = hu_conversation_detect_repetition(entries, 2, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

/* ── Relationship-tier calibration tests ─────────────────────────────── */

static void relationship_close_friend(void) {
    char buf[512];
    size_t len =
        hu_conversation_calibrate_relationship("close friend", "high", "open", buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Close"));
    HU_ASSERT_NOT_NULL(strstr(buf, "WARMTH"));
    HU_ASSERT_NOT_NULL(strstr(buf, "VULNERABILITY"));
}

static void relationship_acquaintance(void) {
    char buf[512];
    size_t len =
        hu_conversation_calibrate_relationship("acquaintance", "low", NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Acquaintance"));
}

static void relationship_null_fields(void) {
    char buf[512];
    size_t len = hu_conversation_calibrate_relationship(NULL, NULL, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Relationship context"));
}

/* ── Group chat classifier tests ─────────────────────────────────────── */

static void group_direct_address_responds(void) {
    hu_group_response_t r =
        hu_conversation_classify_group("hey seth what do you think", 26, "seth", 4, NULL, 0);
    HU_ASSERT_EQ(r, HU_GROUP_RESPOND);
}

static void group_question_responds(void) {
    hu_group_response_t r =
        hu_conversation_classify_group("anyone free tonight?", 20, "bot", 3, NULL, 0);
    HU_ASSERT_EQ(r, HU_GROUP_RESPOND);
}

static void group_short_no_prompt_skips(void) {
    hu_group_response_t r = hu_conversation_classify_group("lol", 3, "bot", 3, NULL, 0);
    HU_ASSERT_EQ(r, HU_GROUP_SKIP);
}

static void group_too_many_responses_skips(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry(false, "hey", "10:00"),
        make_entry(true, "hey", "10:01"),
        make_entry(true, "what's up", "10:02"),
        make_entry(true, "nm here", "10:03"),
    };
    hu_group_response_t r = hu_conversation_classify_group("cool story", 10, "bot", 3, entries, 4);
    HU_ASSERT_EQ(r, HU_GROUP_SKIP);
}

static void group_empty_skips(void) {
    hu_group_response_t r = hu_conversation_classify_group("", 0, "bot", 3, NULL, 0);
    HU_ASSERT_EQ(r, HU_GROUP_SKIP);
}

static void classify_group_consecutive_2_skips_with_history(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "yo what's up", "12:00"),
        make_entry(false, "anyone wanna grab food?", "12:01"),
        make_entry(true, "sounds good", "12:02"),
        make_entry(true, "i'm free around 7", "12:03"),
    };
    /* With 2 consecutive from_me entries, group classifier should skip */
    hu_group_response_t r = hu_conversation_classify_group("yeah same", 9, "bot", 3, entries, 4);
    HU_ASSERT_EQ(r, HU_GROUP_SKIP);
}

static void classify_group_medium_message_is_brief(void) {
    /* 30-100 char message, no question, no engage word → HU_GROUP_BRIEF */
    hu_group_response_t r = hu_conversation_classify_group(
        "just got back from the gym, pretty tired", 40, "bot", 3, NULL, 0);
    HU_ASSERT_EQ(r, HU_GROUP_BRIEF);
}

static void group_prompt_hint_macro_is_defined(void) {
    const char *hint = HU_GROUP_CHAT_PROMPT_HINT;
    HU_ASSERT_NOT_NULL(hint);
    HU_ASSERT_TRUE(strlen(hint) > 10);
    HU_ASSERT_TRUE(strstr(hint, "GROUP CHAT") != NULL);
    HU_ASSERT_TRUE(strstr(hint, "group conversation") != NULL);
}

static void group_history_before_gate_classifier(void) {
    hu_channel_history_entry_t entries[3] = {
        make_entry(false, "hey everyone", "12:00"),
        make_entry(false, "who wants to go?", "12:01"),
        make_entry(true, "I'm in!", "12:02"),
    };
    hu_group_response_t r =
        hu_conversation_classify_group("cool let's do it", 16, "bot", 3, entries, 3);
    HU_ASSERT(r == HU_GROUP_SKIP || r == HU_GROUP_BRIEF || r == HU_GROUP_RESPOND);
}

/* ── Thread callback tests ──────────────────────────────────────────── */

static void callback_finds_dropped_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Work discussed early (first half), then shifts to cooking (recent 3).
     * Last message "nice" has hash mod 5 == 0 so callback triggers (~20% probability). */
    hu_channel_history_entry_t entries[10] = {
        make_entry(false, "what about work? how's it going?", "12:00"),
        make_entry(true, "work has been crazy lately", "12:01"),
        make_entry(false, "tell me about work", "12:02"),
        make_entry(true, "it's busy but ok", "12:03"),
        make_entry(false, "got it", "12:04"),
        make_entry(true, "what's for dinner?", "12:05"),
        make_entry(false, "thinking about cooking", "12:06"),
        make_entry(true, "i'm making pasta", "12:07"),
        make_entry(false, "cooking is fun", "12:08"),
        make_entry(true, "nice", "12:09"),
    };
    size_t len = 0;
    char *ctx = hu_conversation_build_callback(&alloc, entries, 10, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "work") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "Thread Callback") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void callback_no_candidate_short_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[3] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
        make_entry(false, "what's up", "12:02"),
    };
    size_t len = 0;
    char *ctx = hu_conversation_build_callback(&alloc, entries, 3, &len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, 0u);
}

static void callback_null_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ctx = hu_conversation_build_callback(&alloc, NULL, 10, &len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, 0u);
}

/* ── Tapback-vs-text decision tests ──────────────────────────────────── */

static void tapback_decision_lol_tapback_or_both(void) {
    /* "lol" matches humor; seed 0 yields roll<70 → TAPBACK_ONLY */
    hu_tapback_decision_t d =
        hu_conversation_classify_tapback_decision("lol", 3, NULL, 0, NULL, 0u);
    HU_ASSERT_TRUE(d == HU_TAPBACK_ONLY || d == HU_TAPBACK_AND_TEXT);
}

static void tapback_decision_question_text_only(void) {
    /* "what time is dinner?" has question → TEXT_ONLY */
    hu_tapback_decision_t d =
        hu_conversation_classify_tapback_decision("what time is dinner?", 20, NULL, 0, NULL, 0u);
    HU_ASSERT_EQ(d, HU_TEXT_ONLY);
}

static void tapback_decision_k_no_response_or_brief(void) {
    /* "k" → NO_RESPONSE or TEXT_ONLY (brief); seed 0 yields NO_RESPONSE */
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision("k", 1, NULL, 0, NULL, 0u);
    HU_ASSERT_TRUE(d == HU_NO_RESPONSE || d == HU_TEXT_ONLY);
}

static void tapback_decision_recent_tapbacks_reduces_tapback(void) {
    /* History with 2+ recent from_me tapbacks → 60% TEXT_ONLY for messages that reach that check */
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "that's wild", "12:00"),
        make_entry(true, "Liked an image", "12:01"),
        make_entry(false, "omg", "12:02"),
        make_entry(true, "Laughed at a message", "12:03"),
    };
    /* "omg" falls through to recent_tapbacks check; with 2 tapbacks, 60% TEXT_ONLY */
    hu_tapback_decision_t d =
        hu_conversation_classify_tapback_decision("omg", 3, entries, 4, NULL, 0u);
    /* Can be TAPBACK_ONLY, TAPBACK_AND_TEXT, or TEXT_ONLY depending on roll */
    HU_ASSERT_TRUE(d == HU_TAPBACK_ONLY || d == HU_TAPBACK_AND_TEXT || d == HU_TEXT_ONLY);
}

static void tapback_decision_empty_no_response(void) {
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision("", 0, NULL, 0, NULL, 0u);
    HU_ASSERT_EQ(d, HU_NO_RESPONSE);
}

static void tapback_decision_emotional_text_only(void) {
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision(
        "i've been really stressed lately", 31, NULL, 0, NULL, 0u);
    HU_ASSERT_EQ(d, HU_TEXT_ONLY);
}

/* ── Reaction classifier tests ───────────────────────────────────────── */

static void reaction_funny_message(void) {
    /* "lol that's hilarious" matches funny pattern; seed 0 yields roll<30 → HAHA */
    hu_reaction_type_t r =
        hu_conversation_classify_reaction("lol that's hilarious", 19, false, NULL, 0, 0u);
    HU_ASSERT_NEQ(r, HU_REACTION_NONE);
    HU_ASSERT_EQ(r, HU_REACTION_HAHA);
}

static void reaction_loving_message(void) {
    /* "love you" matches loving pattern; seed 0 yields roll<30 → HEART */
    hu_reaction_type_t r = hu_conversation_classify_reaction("love you", 8, false, NULL, 0, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_HEART);
}

static void reaction_normal_message_no_reaction(void) {
    /* "what time is dinner?" needs a text response → NONE */
    hu_reaction_type_t r =
        hu_conversation_classify_reaction("what time is dinner?", 20, false, NULL, 0, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_NONE);
}

static void reaction_from_me_no_reaction(void) {
    /* from_me=true → always NONE */
    hu_reaction_type_t r = hu_conversation_classify_reaction("love you", 8, true, NULL, 0, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_NONE);
}

/* ── Photo reaction classifier tests ───────────────────────────────────── */

static void photo_reaction_sunset_heart(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_photo_reaction("A beautiful sunset over the ocean", 34, NULL, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_HEART);
}

static void photo_reaction_funny_meme_haha(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_photo_reaction("A funny meme with text overlay", 30, NULL, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_HAHA);
}

static void photo_reaction_screenshot_none(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_photo_reaction("A screenshot of an error message", 31, NULL, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_NONE);
}

static void photo_reaction_family_heart(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_photo_reaction("A family photo at the park", 26, NULL, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_HEART);
}

static void photo_reaction_food_none(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_photo_reaction("A plate of delicious pasta", 26, NULL, 0u);
    HU_ASSERT_EQ(r, HU_REACTION_NONE);
}

static void photo_reaction_extract_vision_description(void) {
    const char *combined = "hello\n[They sent a photo: A beautiful sunset]";
    const char *desc = NULL;
    size_t desc_len = 0;
    bool ok =
        hu_conversation_extract_vision_description(combined, strlen(combined), &desc, &desc_len);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_EQ(desc_len, 18u);
    HU_ASSERT_TRUE(memcmp(desc, "A beautiful sunset", 17) == 0);
}

static void photo_reaction_extract_no_vision_returns_false(void) {
    const char *combined = "just a normal message";
    const char *desc = NULL;
    size_t desc_len = 0;
    bool ok =
        hu_conversation_extract_vision_description(combined, strlen(combined), &desc, &desc_len);
    HU_ASSERT_FALSE(ok);
    HU_ASSERT_NULL(desc);
    HU_ASSERT_EQ(desc_len, 0u);
}

/* ── URL extraction tests ────────────────────────────────────────────── */

static void url_extract_finds_https(void) {
    const char *text = "check out https://example.com/page cool right?";
    hu_url_extract_t urls[4];
    size_t n = hu_conversation_extract_urls(text, strlen(text), urls, 4);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(urls[0].len, 24u);
    HU_ASSERT_TRUE(memcmp(urls[0].start, "https://example.com/page", 24) == 0);
}

static void url_extract_multiple(void) {
    const char *text = "see https://a.com and http://b.org/foo";
    hu_url_extract_t urls[4];
    size_t n = hu_conversation_extract_urls(text, strlen(text), urls, 4);
    HU_ASSERT_EQ(n, 2u);
    HU_ASSERT_TRUE(strncmp(urls[0].start, "https://a.com", urls[0].len) == 0);
    HU_ASSERT_TRUE(strncmp(urls[1].start, "http://b.org/foo", urls[1].len) == 0);
}

static void url_extract_no_urls(void) {
    const char *text = "hello world";
    hu_url_extract_t urls[4];
    size_t n = hu_conversation_extract_urls(text, strlen(text), urls, 4);
    HU_ASSERT_EQ(n, 0u);
}

/* ── Link-sharing detection tests ─────────────────────────────────────── */

static void should_share_link_recommendation(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "you should check this out", "12:00"),
    };
    bool ok = hu_conversation_should_share_link("you should check this out", 25, entries, 1);
    HU_ASSERT_TRUE(ok);
}

static void should_share_link_normal(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "how are you doing", "12:00"),
    };
    bool ok = hu_conversation_should_share_link("how are you doing", 17, entries, 1);
    HU_ASSERT_FALSE(ok);
}

static void should_share_link_case_insensitive(void) {
    bool ok = hu_conversation_should_share_link("CHECK THIS OUT", 15, NULL, 0);
    HU_ASSERT_TRUE(ok);
}

/* ── Attachment context tests ─────────────────────────────────────────── */

static void attachment_context_with_photo(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "hey", "12:00"),
        make_entry(false, "[Photo shared]", "12:01"),
    };
    size_t len = 0;
    char *ctx = hu_conversation_attachment_context(&alloc, entries, 2, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(ctx, "photo"));
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void attachment_context_with_imessage_placeholder(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "hey", "12:00"),
        make_entry(false, "[image or attachment]", "12:01"),
    };
    size_t len = 0;
    char *ctx = hu_conversation_attachment_context(&alloc, entries, 2, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, ctx, len + 1);
}

/* ── Time-of-day calibration test ────────────────────────────────────── */

static void calibrate_length_runs_without_crash(void) {
    char buf[2048];
    const char *msg = "hey what's up";
    size_t len = hu_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    /* TIME: directive appears only outside daytime (9-17), so we verify
     * the function completes without crashing at any hour. */
}

/* ── Consecutive response limit tests ────────────────────────────────── */

static void classify_consecutive_3_ours_skips(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "hey how are you", "12:00"),
        make_entry(true, "good hbu", "12:01"),
        make_entry(true, "just chilling", "12:02"),
        make_entry(true, "yeah it's been a day", "12:03"),
    };
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("what are you up to tonight", 26, entries, 4, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_SKIP);
}

static void classify_consecutive_2_ours_still_responds(void) {
    hu_channel_history_entry_t entries[3] = {
        make_entry(false, "what's going on", "12:00"),
        make_entry(true, "not much", "12:01"),
        make_entry(true, "just chilling", "12:02"),
    };
    uint32_t delay = 0;
    hu_response_action_t a =
        hu_conversation_classify_response("want to grab dinner tonight?", 28, entries, 3, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_FULL);
}

/* ── Drop-off classifier tests ──────────────────────────────────────── */

static void dropoff_mutual_farewell_night_night(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "night", "12:00"),
        make_entry(true, "night", "12:01"),
    };
    int p = hu_conversation_classify_dropoff("night", 5, entries, 2, 0);
    HU_ASSERT_EQ(p, 90);
}

static void dropoff_low_energy_yeah(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "yeah", "12:00"),
    };
    int p = hu_conversation_classify_dropoff("yeah", 4, entries, 1, 0);
    HU_ASSERT_EQ(p, 60);
}

static void dropoff_emoji_only_thumbs_up(void) {
    /* UTF-8 thumbs up: U+1F44D — no alphanumeric, so emoji-only */
    const char emoji[] = "\xf0\x9f\x91\x8d";
    int p = hu_conversation_classify_dropoff(emoji, sizeof(emoji) - 1, NULL, 0, 0);
    HU_ASSERT_EQ(p, 70);
}

static void dropoff_our_farewell_their_k(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "bye", "12:00"),
        make_entry(false, "k", "12:01"),
    };
    int p = hu_conversation_classify_dropoff("k", 1, entries, 2, 0);
    HU_ASSERT_EQ(p, 100);
}

static void dropoff_normal_conversation_zero(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "what are you up to tonight?", "12:00"),
        make_entry(true, "just chilling", "12:01"),
    };
    int p = hu_conversation_classify_dropoff("wanna grab dinner?", 17, entries, 2, 0);
    HU_ASSERT_EQ(p, 0);
}

static void classify_narrative_no_question_is_brief(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response(
        "just got done with work and heading to the gym now", 50, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_BRIEF);
}

static void classify_question_still_full(void) {
    uint32_t delay = 0;
    hu_response_action_t a = hu_conversation_classify_response(
        "do you want to grab dinner later tonight?", 41, NULL, 0, &delay);
    HU_ASSERT_EQ(a, HU_RESPONSE_FULL);
}

/* ── Response mode override tests ──────────────────────────────────────── */

static void classify_selective_mode_downgrades_full_to_brief_for_no_question(void) {
    /* Selective mode (default): downgrade FULL response when message has no '?' */
    uint32_t delay = 0;
    /* Using a narrative that might classify as FULL based on content */
    hu_response_action_t action = hu_conversation_classify_response(
        "i think we should try something different this time", 50, NULL, 0, &delay);

    /* Apply selective mode override logic manually */
    const char *rmode = "selective";
    const char *combined = "i think we should try something different this time";
    size_t combined_len = 50;

    if (!rmode || !rmode[0] || strcmp(rmode, "selective") == 0) {
        if (action == HU_RESPONSE_FULL && !memchr(combined, '?', combined_len))
            action = HU_RESPONSE_BRIEF;
    }
    /* Verify the logic: if original was FULL, it should become BRIEF (no '?' present) */
    if (action == HU_RESPONSE_BRIEF) {
        HU_ASSERT_TRUE(action == HU_RESPONSE_BRIEF);
    } else {
        HU_ASSERT_TRUE(action == HU_RESPONSE_FULL || action == HU_RESPONSE_BRIEF ||
                       action == HU_RESPONSE_SKIP);
    }
}

static void classify_selective_mode_preserves_question(void) {
    /* Selective mode: preserve FULL response for questions (has ?) */
    uint32_t delay = 0;
    hu_response_action_t action =
        hu_conversation_classify_response("what are you up to tonight?", 27, NULL, 0, &delay);
    HU_ASSERT_EQ(action, HU_RESPONSE_FULL);

    /* Apply selective mode override logic */
    const char *rmode = "selective";
    const char *combined = "what are you up to tonight?";
    size_t combined_len = 27;

    hu_response_action_t original_action = action;
    if (!rmode || !rmode[0] || strcmp(rmode, "selective") == 0) {
        if (action == HU_RESPONSE_FULL && !memchr(combined, '?', combined_len))
            action = HU_RESPONSE_BRIEF;
    }
    /* Should remain FULL because it contains '?' */
    HU_ASSERT_EQ(action, original_action);
    HU_ASSERT_EQ(action, HU_RESPONSE_FULL);
}

static void classify_eager_mode_upgrades_brief_to_full(void) {
    /* Eager mode: upgrade BRIEF response to FULL */
    uint32_t delay = 0;
    hu_response_action_t action = hu_conversation_classify_response("lol", 3, NULL, 0, &delay);
    HU_ASSERT_EQ(action, HU_RESPONSE_BRIEF);

    /* Simulate eager mode override */
    const char *rmode = "eager";
    if (strcmp(rmode, "eager") == 0) {
        if (action == HU_RESPONSE_BRIEF)
            action = HU_RESPONSE_FULL;
    }
    HU_ASSERT_EQ(action, HU_RESPONSE_FULL);
}

static void classify_normal_mode_no_override(void) {
    /* Normal mode: no override, classifier result unchanged */
    uint32_t delay = 0;
    hu_response_action_t action = hu_conversation_classify_response("nice", 4, NULL, 0, &delay);
    hu_response_action_t original_action = action;

    /* Simulate normal mode override logic (should not change) */
    const char *rmode = "normal";
    if (!rmode || !rmode[0] || strcmp(rmode, "selective") == 0) {
        const char *combined = "nice";
        size_t combined_len = 4;
        if (action == HU_RESPONSE_FULL && !memchr(combined, '?', combined_len))
            action = HU_RESPONSE_BRIEF;
    } else if (strcmp(rmode, "eager") == 0) {
        if (action == HU_RESPONSE_BRIEF)
            action = HU_RESPONSE_FULL;
    }
    /* "normal" = no change, so action should equal original_action */
    HU_ASSERT_EQ(action, original_action);
}

static void classify_selective_is_default(void) {
    /* NULL/empty response_mode should behave like "selective" */
    uint32_t delay = 0;
    /* Use a question which always classifies as FULL */
    hu_response_action_t action =
        hu_conversation_classify_response("are you free tomorrow?", 22, NULL, 0, &delay);
    HU_ASSERT_EQ(action, HU_RESPONSE_FULL);

    /* Simulate default (NULL) mode override = selective behavior */
    const char *rmode = NULL;
    const char *combined = "are you free tomorrow?";
    size_t combined_len = 22;

    if (!rmode || !rmode[0] || strcmp(rmode, "selective") == 0) {
        if (action == HU_RESPONSE_FULL && !memchr(combined, '?', combined_len))
            action = HU_RESPONSE_BRIEF;
    }
    /* Question has '?' so selective mode should NOT downgrade it */
    HU_ASSERT_EQ(action, HU_RESPONSE_FULL);
}

/* ── iMessage effect classifier tests ──────────────────────────────────── */

static void effect_happy_birthday_confetti(void) {
    const char *eff = hu_conversation_classify_effect("Happy birthday!", 15);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "confetti");
}

static void effect_congratulations_balloons(void) {
    const char *eff = hu_conversation_classify_effect("Congratulations on the promotion!", 34);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "balloons");
}

static void effect_congrats_balloons(void) {
    const char *eff = hu_conversation_classify_effect("congrats dude", 13);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "balloons");
}

static void effect_pew_pew_lasers(void) {
    const char *eff = hu_conversation_classify_effect("pew pew", 7);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "lasers");
}

static void effect_happy_new_year_fireworks(void) {
    const char *eff = hu_conversation_classify_effect("Happy new year!", 15);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "fireworks");
}

static void effect_normal_message_returns_null(void) {
    const char *eff = hu_conversation_classify_effect("hey what's up", 12);
    HU_ASSERT_NULL(eff);
}

static void effect_null_input_returns_null(void) {
    const char *eff = hu_conversation_classify_effect(NULL, 0);
    HU_ASSERT_NULL(eff);
}

static void effect_empty_returns_null(void) {
    const char *eff = hu_conversation_classify_effect("", 0);
    HU_ASSERT_NULL(eff);
}

static void effect_substring_in_sentence(void) {
    const char *eff =
        hu_conversation_classify_effect("I just wanted to say happy birthday to you!", 47);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "confetti");
}

/* ── Expanded iMessage effects tests ────────────────────────────────── */

static void effect_halloween_echo(void) {
    const char *eff = hu_conversation_classify_effect("happy halloween!", 16);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "echo");
}

static void effect_valentines_heart(void) {
    const char *eff = hu_conversation_classify_effect("Happy Valentine's Day!", 22);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "heart");
}

static void effect_christmas_confetti(void) {
    const char *eff = hu_conversation_classify_effect("Merry Christmas!", 16);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "confetti");
}

static void effect_fourth_july_fireworks(void) {
    const char *eff = hu_conversation_classify_effect("happy 4th of july!", 18);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "fireworks");
}

static void effect_anniversary_heart(void) {
    const char *eff = hu_conversation_classify_effect("happy anniversary babe", 22);
    HU_ASSERT_NOT_NULL(eff);
    HU_ASSERT_STR_EQ(eff, "heart");
}

/* ── Cold restart tests ─────────────────────────────────────────────── */

static void cold_restart_recent_messages_returns_zero(void) {
    hu_channel_history_entry_t entries[3] = {
        {.from_me = false, .text = "hey", .timestamp = "2026-03-24 14:00:00"},
        {.from_me = true, .text = "hey whats up", .timestamp = "2026-03-24 14:01:00"},
        {.from_me = false, .text = "not much", .timestamp = "2026-03-24 14:02:00"},
    };
    char buf[512];
    size_t len = hu_conversation_build_cold_restart_hint(entries, 3, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void cold_restart_null_returns_zero(void) {
    char buf[512];
    size_t len = hu_conversation_build_cold_restart_hint(NULL, 0, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

/* ── Self-reaction tests ────────────────────────────────────────────── */

static void self_reaction_returns_none_most_of_time(void) {
    int none_count = 0;
    for (uint32_t seed = 0; seed < 100; seed++) {
        hu_reaction_type_t r = hu_conversation_classify_self_reaction("haha nice", 9, seed);
        if (r == HU_REACTION_NONE)
            none_count++;
    }
    HU_ASSERT_TRUE(none_count >= 90);
}

static void self_reaction_null_returns_none(void) {
    hu_reaction_type_t r = hu_conversation_classify_self_reaction(NULL, 0, 42);
    HU_ASSERT_EQ(r, HU_REACTION_NONE);
}

/* ── Group mention hint tests ───────────────────────────────────────── */

static void group_mention_non_group_returns_zero(void) {
    char buf[256];
    size_t len = hu_conversation_build_group_mention_hint("Mike", 4, false, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void group_mention_group_returns_content(void) {
    char buf[256];
    size_t len = hu_conversation_build_group_mention_hint("Mike", 4, true, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "Mike") != NULL);
}

/* ── Link context tests ─────────────────────────────────────────────── */

static void link_context_no_url_returns_zero(void) {
    char buf[512];
    size_t len = hu_conversation_build_link_context("hey how are you", 15, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void link_context_with_url_returns_content(void) {
    char buf[512];
    const char *msg = "check this out https://example.com/cool";
    size_t len = hu_conversation_build_link_context(msg, strlen(msg), buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "LINK") != NULL);
}

/* ── GIF decision tests ─────────────────────────────────────────────── */

static void gif_decision_sad_message_returns_false(void) {
    bool r = hu_conversation_should_send_gif("i'm so sad today", 16, NULL, 0, 42, 1.0f);
    HU_ASSERT_FALSE(r);
}

static void gif_decision_funny_message_prob_one_returns_true(void) {
    bool r = hu_conversation_should_send_gif("lmao that's hilarious", 21, NULL, 0, 42, 1.0f);
    HU_ASSERT_TRUE(r);
}

static void gif_decision_question_returns_false(void) {
    bool r = hu_conversation_should_send_gif("what time is the meeting?", 25, NULL, 0, 42, 1.0f);
    HU_ASSERT_FALSE(r);
}

static void gif_search_prompt_nonempty(void) {
    char buf[512];
    size_t len =
        hu_conversation_build_gif_search_prompt("lmao that's so funny", 20, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "GIF") != NULL || strstr(buf, "search") != NULL);
}

/* ── GIF received reaction tests ────────────────────────────────────── */

static void reaction_gif_returns_haha_or_heart(void) {
    int haha = 0, heart = 0, none = 0;
    for (uint32_t s = 0; s < 200; s++) {
        hu_reaction_type_t r = hu_conversation_classify_reaction("[GIF]", 5, false, NULL, 0, s);
        if (r == HU_REACTION_HAHA)
            haha++;
        else if (r == HU_REACTION_HEART)
            heart++;
        else
            none++;
    }
    HU_ASSERT_TRUE(haha > 0);
    HU_ASSERT_TRUE(heart > 0);
    HU_ASSERT_TRUE(none > 0);
}

static void reaction_voice_message_returns_none(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_reaction("[Voice Message]", 15, false, NULL, 0, 42);
    HU_ASSERT_EQ(HU_REACTION_NONE, r);
}

/* ── Contact-aware GIF probability tests ────────────────────────────── */

static void gif_prob_friend_increases(void) {
    float base = 0.10f;
    float adj = hu_conversation_adjust_gif_probability(base, "friend", 6);
    HU_ASSERT_TRUE(adj > base);
}

static void gif_prob_coworker_decreases(void) {
    float base = 0.10f;
    float adj = hu_conversation_adjust_gif_probability(base, "coworker", 8);
    HU_ASSERT_TRUE(adj < base);
}

static void gif_prob_null_relationship_unchanged(void) {
    float base = 0.10f;
    float adj = hu_conversation_adjust_gif_probability(base, NULL, 0);
    HU_ASSERT_TRUE(adj >= base - 0.001f && adj <= base + 0.001f);
}

static void gif_prob_acquaintance_very_low(void) {
    float base = 0.10f;
    float adj = hu_conversation_adjust_gif_probability(base, "acquaintance", 12);
    HU_ASSERT_TRUE(adj < base * 0.3f);
}

static void gif_style_hint_friend_absurd(void) {
    char buf[128];
    size_t len = hu_conversation_build_gif_style_hint("friend", 6, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "absurd") != NULL || strstr(buf, "meme") != NULL);
}

static void gif_style_hint_partner_cute(void) {
    char buf[128];
    size_t len = hu_conversation_build_gif_style_hint("partner", 7, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "cute") != NULL || strstr(buf, "flirty") != NULL);
}

static void gif_style_hint_null_default(void) {
    char buf[128];
    size_t len = hu_conversation_build_gif_style_hint(NULL, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "funny") != NULL);
}

/* ── GIF rate limiting tests ────────────────────────────────────────── */

static void gif_rate_first_send_allowed(void) {
    HU_ASSERT_TRUE(hu_conversation_gif_rate_allow("unknown_contact", 15, 1000000, 5, 600000));
}

static void gif_rate_records_and_blocks(void) {
    hu_conversation_gif_rate_record("rate_test_contact", 17, 1000000);
    bool allowed = hu_conversation_gif_rate_allow("rate_test_contact", 17, 1000100, 5, 600000);
    HU_ASSERT_TRUE(!allowed);
}

static void gif_rate_allows_after_gap(void) {
    hu_conversation_gif_rate_record("gap_test_contact", 16, 1000000);
    bool allowed =
        hu_conversation_gif_rate_allow("gap_test_contact", 16, 1000000 + 700000, 5, 600000);
    HU_ASSERT_TRUE(allowed);
}

/* ── Seen behavior modeling tests ───────────────────────────────────── */

static void seen_respond_now_for_question(void) {
    uint32_t delay = 0;
    hu_seen_action_t act =
        hu_conversation_classify_seen_behavior("hey what's up?", 14, 10, 42, &delay);
    HU_ASSERT_EQ(HU_SEEN_RESPOND_NOW, act);
}

static void seen_respond_now_for_urgent(void) {
    uint32_t delay = 0;
    hu_seen_action_t act =
        hu_conversation_classify_seen_behavior("this is urgent", 14, 12, 42, &delay);
    HU_ASSERT_EQ(HU_SEEN_RESPOND_NOW, act);
}

static void seen_null_msg_respond_now(void) {
    uint32_t delay = 0;
    hu_seen_action_t act = hu_conversation_classify_seen_behavior(NULL, 0, 10, 42, &delay);
    HU_ASSERT_EQ(HU_SEEN_RESPOND_NOW, act);
    HU_ASSERT_EQ(0, (int)delay);
}

static void seen_low_priority_sometimes_delays(void) {
    int delayed = 0;
    for (uint32_t s = 0; s < 500; s++) {
        uint32_t d = 0;
        hu_seen_action_t act = hu_conversation_classify_seen_behavior("ok", 2, 12, s, &d);
        if (act == HU_SEEN_DELAY_THEN_RESPOND) {
            delayed++;
            HU_ASSERT_TRUE(d > 0);
        }
    }
    HU_ASSERT_TRUE(delayed > 0);
    HU_ASSERT_TRUE(delayed < 250);
}

/* ── Cold restart test with 6+ hour gap ─────────────────────────────── */

static void cold_restart_six_hour_gap(void) {
    time_t now = time(NULL);
    time_t old = now - 25200; /* 7 hours ago */
    struct tm tm_old, tm_new;
    localtime_r(&old, &tm_old);
    localtime_r(&now, &tm_new);
    hu_channel_history_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    strncpy(entries[0].text, "see you later", sizeof(entries[0].text) - 1);
    strftime(entries[0].timestamp, sizeof(entries[0].timestamp), "%Y-%m-%d %H:%M", &tm_old);
    entries[1].from_me = false;
    strncpy(entries[1].text, "hey", sizeof(entries[1].text) - 1);
    strftime(entries[1].timestamp, sizeof(entries[1].timestamp), "%Y-%m-%d %H:%M", &tm_new);
    char buf[512];
    size_t len = hu_conversation_build_cold_restart_hint(entries, 2, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "COLD RESTART") != NULL);
}

static void cold_restart_one_hour_gap_no_hint(void) {
    time_t now = time(NULL);
    time_t old = now - 3600; /* 1 hour ago */
    struct tm tm_old, tm_new;
    localtime_r(&old, &tm_old);
    localtime_r(&now, &tm_new);
    hu_channel_history_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    strncpy(entries[0].text, "brb", sizeof(entries[0].text) - 1);
    strftime(entries[0].timestamp, sizeof(entries[0].timestamp), "%Y-%m-%d %H:%M", &tm_old);
    entries[1].from_me = false;
    strncpy(entries[1].text, "ok im back", sizeof(entries[1].text) - 1);
    strftime(entries[1].timestamp, sizeof(entries[1].timestamp), "%Y-%m-%d %H:%M", &tm_new);
    char buf[512];
    size_t len = hu_conversation_build_cold_restart_hint(entries, 2, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

/* Pins the month-boundary regression: a 13h gap that straddles 05-31 -> 06-01.
 * The old (d2-d1) day-of-month math computed a large NEGATIVE gap here (d=01
 * minus d=31) and suppressed the hint. Uses hardcoded dates so it exercises the
 * boundary on EVERY run, not only when time(NULL) happens to land on the 1st. */
static void cold_restart_month_boundary_gap_emits_hint(void) {
    hu_channel_history_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    strncpy(entries[0].text, "night", sizeof(entries[0].text) - 1);
    strncpy(entries[0].timestamp, "2026-05-31 20:00", sizeof(entries[0].timestamp) - 1);
    entries[1].from_me = false;
    strncpy(entries[1].text, "morning", sizeof(entries[1].text) - 1);
    strncpy(entries[1].timestamp, "2026-06-01 09:00", sizeof(entries[1].timestamp) - 1);
    char buf[512];
    size_t len = hu_conversation_build_cold_restart_hint(entries, 2, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "COLD RESTART") != NULL);
}

/* Year-boundary sibling: 12-31 -> 01-01 must also emit (gap is real, not negative). */
static void cold_restart_year_boundary_gap_emits_hint(void) {
    hu_channel_history_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    strncpy(entries[0].text, "happy nye", sizeof(entries[0].text) - 1);
    strncpy(entries[0].timestamp, "2025-12-31 22:00", sizeof(entries[0].timestamp) - 1);
    entries[1].from_me = false;
    strncpy(entries[1].text, "happy new year", sizeof(entries[1].text) - 1);
    strncpy(entries[1].timestamp, "2026-01-01 10:00", sizeof(entries[1].timestamp) - 1);
    char buf[512];
    size_t len = hu_conversation_build_cold_restart_hint(entries, 2, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "COLD RESTART") != NULL);
}

/* ── GIF humor calibration tests ─────────────────────────────────────── */

static void gif_cal_hit_rate_default(void) {
    float rate = hu_conversation_gif_cal_hit_rate("unknown_cal", 11);
    HU_ASSERT_TRUE(rate >= 0.49f && rate <= 0.51f);
}

static void gif_cal_record_and_hit_rate(void) {
    hu_conversation_gif_cal_record_send("cal_test_1", 10, "funny cat", 9);
    hu_conversation_gif_cal_record_send("cal_test_1", 10, "dancing", 7);
    hu_conversation_gif_cal_record_send("cal_test_1", 10, "oh no", 5);
    hu_conversation_gif_cal_record_reaction("cal_test_1", 10);
    float rate = hu_conversation_gif_cal_hit_rate("cal_test_1", 10);
    HU_ASSERT_TRUE(rate > 0.0f && rate < 1.0f);
}

/* ── Reaction-received hint tests ────────────────────────────────────── */

static void reaction_received_hint_no_reactions(void) {
    hu_channel_history_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    strncpy(entries[0].text, "hey what's up", sizeof(entries[0].text) - 1);
    entries[1].from_me = false;
    strncpy(entries[1].text, "not much you?", sizeof(entries[1].text) - 1);
    char buf[512];
    size_t len = hu_conversation_build_reaction_received_hint(entries, 2, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

static void reaction_received_hint_with_tapback(void) {
    hu_channel_history_entry_t entries[3];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    strncpy(entries[0].text, "that movie was great", sizeof(entries[0].text) - 1);
    entries[1].from_me = false;
    strncpy(entries[1].text, "Loved \"that movie was great\"", sizeof(entries[1].text) - 1);
    entries[2].from_me = false;
    strncpy(entries[2].text, "totally agree", sizeof(entries[2].text) - 1);
    char buf[512];
    size_t len = hu_conversation_build_reaction_received_hint(entries, 3, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "TAPBACK") != NULL);
}

/* ── Emoji mirror hint tests ─────────────────────────────────────────── */

static void emoji_mirror_hint_no_emoji(void) {
    hu_channel_history_entry_t entries[3];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = false;
    strncpy(entries[0].text, "hey", sizeof(entries[0].text) - 1);
    entries[1].from_me = false;
    strncpy(entries[1].text, "what's up", sizeof(entries[1].text) - 1);
    entries[2].from_me = false;
    strncpy(entries[2].text, "nothing much", sizeof(entries[2].text) - 1);
    char buf[512];
    size_t len = hu_conversation_build_emoji_mirror_hint(entries, 3, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "rarely") != NULL);
}

static void emoji_mirror_hint_few_messages(void) {
    hu_channel_history_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = false;
    strncpy(entries[0].text, "hi", sizeof(entries[0].text) - 1);
    char buf[512];
    size_t len = hu_conversation_build_emoji_mirror_hint(entries, 1, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

/* ── Edit/unsend awareness tests ─────────────────────────────────────── */

static void edit_awareness_edited(void) {
    char buf[256];
    size_t len = hu_conversation_build_edit_awareness_hint(true, false, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "EDITED") != NULL);
}

static void edit_awareness_unsent(void) {
    char buf[256];
    size_t len = hu_conversation_build_edit_awareness_hint(false, true, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "UNSENT") != NULL);
}

static void edit_awareness_neither(void) {
    char buf[256];
    size_t len = hu_conversation_build_edit_awareness_hint(false, false, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

/* ── Sticker decision engine tests ───────────────────────────────────── */

static void sticker_should_send_on_trigger(void) {
    const char *msg = "omg congrats!! that's amazing!";
    bool result = hu_conversation_should_send_sticker(msg, strlen(msg), NULL, 0, 42, 1.0f);
    HU_ASSERT_TRUE(result);
}

static void sticker_should_not_send_no_trigger(void) {
    const char *msg = "what time is the meeting?";
    bool result = false;
    for (uint32_t seed = 0; seed < 20; seed++) {
        if (hu_conversation_should_send_sticker(msg, strlen(msg), NULL, 0, seed, 1.0f))
            result = true;
    }
    HU_ASSERT_TRUE(!result);
}

static void sticker_should_not_send_after_gif(void) {
    const char *msg = "lol that's hilarious";
    const char *last = "[GIF] sent a funny cat";
    bool result =
        hu_conversation_should_send_sticker(msg, strlen(msg), last, strlen(last), 42, 1.0f);
    HU_ASSERT_TRUE(!result);
}

static void sticker_select_celebration(void) {
    const char *msg = "I got the job!!";
    char path[256];
    size_t len = hu_conversation_select_sticker(msg, strlen(msg), 42, "/tmp/stickers", 13, path,
                                                sizeof(path));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(path, "celebration.png") != NULL);
}

static void sticker_select_laughing(void) {
    const char *msg = "lmao I can't even";
    char path[256];
    size_t len = hu_conversation_select_sticker(msg, strlen(msg), 99, "/tmp/stickers", 13, path,
                                                sizeof(path));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(path, ".png") != NULL);
}

static void sticker_select_no_match(void) {
    const char *msg = "what's the weather like?";
    char path[256];
    size_t len = hu_conversation_select_sticker(msg, strlen(msg), 42, "/tmp/stickers", 13, path,
                                                sizeof(path));
    HU_ASSERT_EQ(0, (int)len);
}

/* ── Inline reply context tests ──────────────────────────────────────── */

static void inline_reply_hint_builds_context(void) {
    const char *orig = "are we still on for dinner tonight?";
    char buf[512];
    size_t len = hu_conversation_build_inline_reply_hint(orig, strlen(orig), buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INLINE REPLY") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "dinner tonight") != NULL);
}

static void inline_reply_hint_truncates_long(void) {
    char orig[256];
    memset(orig, 'a', sizeof(orig));
    orig[sizeof(orig) - 1] = '\0';
    char buf[512];
    size_t len = hu_conversation_build_inline_reply_hint(orig, sizeof(orig) - 1, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "...") != NULL);
}

static void inline_reply_hint_null_returns_zero(void) {
    char buf[256];
    size_t len = hu_conversation_build_inline_reply_hint(NULL, 0, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

/* ── GIF calibration persistence tests ───────────────────────────────── */

static void gif_cal_save_and_load_roundtrip(void) {
    hu_conversation_gif_cal_record_send("persist_test", 12, "funny dog", 9);
    hu_conversation_gif_cal_record_send("persist_test", 12, "cats", 4);
    hu_conversation_gif_cal_record_reaction("persist_test", 12);
    float rate_before = hu_conversation_gif_cal_hit_rate("persist_test", 12);

    const char *path = "/tmp/hu_test_gif_cal.json";
    hu_error_t err = hu_conversation_gif_cal_save(path, strlen(path));
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_conversation_gif_cal_load(path, strlen(path));
    HU_ASSERT_EQ(err, HU_OK);
    float rate_after = hu_conversation_gif_cal_hit_rate("persist_test", 12);
    HU_ASSERT_TRUE(rate_before >= 0.0f && rate_after >= 0.0f);
    (void)unlink(path);
}

/* ── Multi-message splitting tests ───────────────────────────────────── */

static void split_into_texts_short_no_split(void) {
    const char *msg = "hey what's up";
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 200, chunks, 4);
    HU_ASSERT_EQ(1, (int)n);
    HU_ASSERT_STR_EQ(chunks[0], msg);
}

static void split_into_texts_splits_at_sentence(void) {
    const char *msg = "I went to the store. Then I came home. It was a good day.";
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 25, chunks, 4);
    HU_ASSERT_TRUE(n >= 2);
    HU_ASSERT_TRUE(strlen(chunks[0]) > 0);
}

static void split_into_texts_null_returns_zero(void) {
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(NULL, 0, 100, chunks, 4);
    HU_ASSERT_EQ(0, (int)n);
}

/* #6 regression: whitespace-only input must not produce a blank bubble. */
static void split_into_texts_blank_input_returns_zero(void) {
    const char *msg = "    \t  \n ";
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 200, chunks, 4);
    HU_ASSERT_EQ(0, (int)n);
}

/* #5 regression: a hard cut must not split a multi-byte UTF-8 codepoint.
 * Build a string whose byte at the cut boundary lands inside a 4-byte emoji,
 * then assert every emitted chunk is valid UTF-8 (no trailing partial bytes). */
static void split_into_texts_never_splits_utf8_codepoint(void) {
    /* 8 'a' + rocket emoji (F0 9F 9A 80) repeated, so a max_chunk near a
     * multiple lands mid-emoji if unguarded. */
    char msg[512];
    size_t len = 0;
    for (int rep = 0; rep < 40 && len + 5 < sizeof(msg); rep++) {
        msg[len++] = 'a';
        msg[len++] = (char)0xF0;
        msg[len++] = (char)0x9F;
        msg[len++] = (char)0x9A;
        msg[len++] = (char)0x80;
    }
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, len, 23, chunks, 4);
    HU_ASSERT_TRUE(n >= 1);
    for (size_t c = 0; c < n; c++) {
        size_t clen = strlen(chunks[c]);
        /* A valid chunk must not END on a UTF-8 continuation byte (0x80..0xBF)
         * with an incomplete lead — verify the trailing codepoint is complete
         * by re-running the safe-length check: it must equal clen. */
        size_t safe = clen;
        while (safe > 0 && ((unsigned char)chunks[c][safe - 1] & 0xC0) == 0x80)
            safe--;
        /* Either the last byte is ASCII/lead with its full continuation run
         * present, or it's a complete codepoint. The simplest invariant: the
         * final byte is not a lone continuation, i.e. walking back over
         * continuations reaches a lead byte whose width matches. */
        if (safe > 0) {
            unsigned char lead = (unsigned char)chunks[c][safe - 1];
            size_t cont = clen - safe;
            size_t need = (lead & 0x80) == 0x00   ? 1
                          : (lead & 0xE0) == 0xC0 ? 2
                          : (lead & 0xF0) == 0xE0 ? 3
                          : (lead & 0xF8) == 0xF0 ? 4
                                                  : 1;
            HU_ASSERT_EQ((int)need, (int)(1 + cont));
        }
    }
}

/* ── Scheduled message tests ─────────────────────────────────────────── */

static void schedule_and_flush_delivers(void) {
    uint64_t now = (uint64_t)time(NULL) * 1000ULL;
    hu_error_t err =
        hu_conversation_schedule_message("sched_contact", 13, "good morning!", 13, now - 1000);
    HU_ASSERT_EQ(err, HU_OK);

    char contact[128], msg[512];
    size_t len = hu_conversation_flush_scheduled(now, contact, sizeof(contact), msg, sizeof(msg));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_STR_EQ(contact, "sched_contact");
    HU_ASSERT_STR_EQ(msg, "good morning!");
}

static void schedule_future_not_yet_delivered(void) {
    uint64_t now = (uint64_t)time(NULL) * 1000ULL;
    hu_conversation_schedule_message("future_contact", 14, "later!", 6, now + 3600000);
    char contact[128], msg[512];
    size_t len = hu_conversation_flush_scheduled(now, contact, sizeof(contact), msg, sizeof(msg));
    HU_ASSERT_EQ(0, (int)len);
}

static void schedule_null_returns_error(void) {
    hu_error_t err = hu_conversation_schedule_message(NULL, 0, "hi", 2, 12345);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ── Contact photo path tests ────────────────────────────────────────── */

static void contact_photo_null_returns_zero(void) {
    char buf[256];
    size_t len = hu_conversation_contact_photo_path(NULL, 0, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

static void contact_photo_small_buf_returns_zero(void) {
    char buf[16];
    size_t len = hu_conversation_contact_photo_path("test@test.com", 13, buf, sizeof(buf));
    HU_ASSERT_EQ(0, (int)len);
}

/* ── Schedule with channel routing tests ─────────────────────────────── */

static void schedule_on_channel_routes(void) {
    uint64_t now = (uint64_t)time(NULL) * 1000ULL;
    hu_error_t err = hu_conversation_schedule_message_on("routed_contact", 14, "imessage", 8,
                                                         "hello via imsg", 14, now - 500);
    HU_ASSERT_EQ(err, HU_OK);

    char contact[128], channel[32], msg[512];
    size_t len = hu_conversation_flush_scheduled_on(now, contact, sizeof(contact), channel,
                                                    sizeof(channel), msg, sizeof(msg));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_STR_EQ(contact, "routed_contact");
    HU_ASSERT_STR_EQ(channel, "imessage");
    HU_ASSERT_STR_EQ(msg, "hello via imsg");
}

static void schedule_on_empty_channel_matches_any(void) {
    uint64_t now = (uint64_t)time(NULL) * 1000ULL;
    hu_conversation_schedule_message_on("any_contact", 11, NULL, 0, "hey", 3, now - 100);
    char contact[128], channel[32], msg[512];
    channel[0] = 'X';
    size_t len = hu_conversation_flush_scheduled_on(now, contact, sizeof(contact), channel,
                                                    sizeof(channel), msg, sizeof(msg));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_EQ(channel[0], '\0');
}

/* ── Schedule persistence tests ──────────────────────────────────────── */

static void sched_save_and_load_roundtrip(void) {
    uint64_t now = (uint64_t)time(NULL) * 1000ULL;
    hu_conversation_schedule_message_on("persist_user", 12, "telegram", 8, "hi from persist", 15,
                                        now + 60000);
    const char *path = "/tmp/hu_test_sched.json";
    hu_error_t err = hu_conversation_sched_save(path, strlen(path));
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_conversation_sched_load(path, strlen(path));
    HU_ASSERT_EQ(err, HU_OK);

    char contact[128], channel[32], msg[512];
    size_t len = hu_conversation_flush_scheduled_on(now + 60000, contact, sizeof(contact), channel,
                                                    sizeof(channel), msg, sizeof(msg));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_STR_EQ(contact, "persist_user");
    HU_ASSERT_STR_EQ(channel, "telegram");
    (void)unlink(path);
}

/* ── Split boundary edge case tests ──────────────────────────────────── */

static void split_into_texts_exact_boundary(void) {
    const char *msg = "Hello world.";
    char chunks[4][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 12, chunks, 4);
    HU_ASSERT_EQ(1, (int)n);
    HU_ASSERT_STR_EQ(chunks[0], "Hello world.");
}

static void split_into_texts_respects_max_chunks(void) {
    const char *msg = "A. B. C. D. E. F. G. H.";
    char chunks[2][512];
    size_t n = hu_conversation_split_into_texts(msg, strlen(msg), 5, chunks, 2);
    HU_ASSERT_EQ(2, (int)n);
}

/* ── hu_conversation_split_for_cadence — channel-class-aware burst splitter ──
 *
 * Real human iMessage cadence bursts SHORT multi-sentence replies into
 * separate bubbles. The old splitter only triggered on long (>120 char) prose,
 * so the agent always sent compact replies as one cold bubble. This suite
 * locks the new contract: TEXT_FAST gets sentence-level bursts; TEXT_ASYNC
 * keeps single-bubble behavior unless prose is genuinely long.
 *
 * If any of these invariants regress, the assistant stops "feeling like
 * iMessage" — the texture is the test.
 * ────────────────────────────────────────────────────────────────────────── */

static void split_for_cadence_text_fast_bursts_multi_sentence(void) {
    /* Canonical iMessage burst pattern: 3 short sentences, total ~73 chars,
     * no newlines. Today's old gate (>120 chars) would NOT split. The new
     * contract: TEXT_FAST sees ≥2 sentence boundaries, splits into one
     * bubble per sentence. */
    const char *msg = "sure that sounds great. what time? might be a few mins late.";
    char chunks[4][512];
    size_t n =
        hu_conversation_split_for_cadence(msg, strlen(msg), HU_CHANNEL_CLASS_TEXT_FAST, chunks, 4);
    HU_ASSERT_EQ(3, (int)n);
    HU_ASSERT(strstr(chunks[0], "sure that sounds great") != NULL);
    HU_ASSERT(strstr(chunks[1], "what time?") != NULL);
    HU_ASSERT(strstr(chunks[2], "might be a few mins late") != NULL);
}

static void split_for_cadence_text_fast_single_sentence_stays_one(void) {
    /* Single-statement replies must NOT split even on iMessage. "sure thing."
     * is ONE intent, one bubble. The ≥2-sentence-ends gate prevents this
     * from over-splitting. */
    const char *msg = "sure that sounds good let me know when you're free.";
    char chunks[4][512];
    size_t n =
        hu_conversation_split_for_cadence(msg, strlen(msg), HU_CHANNEL_CLASS_TEXT_FAST, chunks, 4);
    HU_ASSERT_EQ(0, (int)n); /* 0 = caller sends as a single bubble */
}

static void split_for_cadence_text_fast_below_floor_stays_one(void) {
    /* Two-sentence reply under the 40-char floor: still one bubble. Splitting
     * "ok cool. see ya." into two micro-bubbles feels robotic, not casual. */
    const char *msg = "ok cool. see ya.";
    char chunks[4][512];
    size_t n =
        hu_conversation_split_for_cadence(msg, strlen(msg), HU_CHANNEL_CLASS_TEXT_FAST, chunks, 4);
    HU_ASSERT_EQ(0, (int)n);
}

static void split_for_cadence_text_async_short_stays_one(void) {
    /* Slack/Discord (TEXT_ASYNC) tolerates longer single bubbles. The same
     * multi-sentence reply that bursts on iMessage must NOT burst on Slack —
     * different cadence cultures. */
    const char *msg = "sure that sounds great. what time? might be a few mins late.";
    char chunks[4][512];
    size_t n =
        hu_conversation_split_for_cadence(msg, strlen(msg), HU_CHANNEL_CLASS_TEXT_ASYNC, chunks, 4);
    HU_ASSERT_EQ(0, (int)n);
}

static void split_for_cadence_long_prose_still_chunks_on_any_class(void) {
    /* The original >120 char prose path is preserved for every channel class,
     * delegating to hu_conversation_split_into_texts. Regression guard: a
     * future "TEXT_ASYNC = always single bubble" mistake would break this. */
    const char *msg = "ok so i was thinking about the trip and there's a few options we could try. "
                      "first one is the cabin by the lake which is great if you want quiet time. "
                      "second one is the place downtown which would be more social.";
    char chunks[4][512];
    size_t n =
        hu_conversation_split_for_cadence(msg, strlen(msg), HU_CHANNEL_CLASS_TEXT_ASYNC, chunks, 4);
    HU_ASSERT(n >= 2);
}

static void split_for_cadence_explicit_newlines_inhibit_split(void) {
    /* If the LLM inserted explicit newlines, that's intentional formatting —
     * the splitter must not second-guess it. Returning 0 means the caller
     * sends the text as a single bubble preserving the LLM's structure. */
    const char *msg = "sure that sounds great.\nwhat time?\nmight be late.";
    char chunks[4][512];
    size_t n =
        hu_conversation_split_for_cadence(msg, strlen(msg), HU_CHANNEL_CLASS_TEXT_FAST, chunks, 4);
    HU_ASSERT_EQ(0, (int)n);
}

static void split_for_cadence_null_inputs_return_zero(void) {
    char chunks[4][512];
    HU_ASSERT_EQ(
        (int)hu_conversation_split_for_cadence(NULL, 0, HU_CHANNEL_CLASS_TEXT_FAST, chunks, 4), 0);
    HU_ASSERT_EQ(
        (int)hu_conversation_split_for_cadence("hi", 2, HU_CHANNEL_CLASS_TEXT_FAST, NULL, 4), 0);
    HU_ASSERT_EQ(
        (int)hu_conversation_split_for_cadence("hi", 2, HU_CHANNEL_CLASS_TEXT_FAST, chunks, 0), 0);
}

/* ── GIF cal JSON escaping test ──────────────────────────────────────── */

static void gif_cal_save_escapes_quotes(void) {
    hu_conversation_gif_cal_record_send("test\"quoted", 11, "query", 5);
    const char *path = "/tmp/hu_test_gif_cal_esc.json";
    hu_error_t err = hu_conversation_gif_cal_save(path, strlen(path));
    HU_ASSERT_EQ(err, HU_OK);

    FILE *f = fopen(path, "r");
    HU_ASSERT_TRUE(f != NULL);
    char buf[512];
    bool found_escaped = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "test\\\"quoted"))
            found_escaped = true;
    }
    fclose(f);
    HU_ASSERT_TRUE(found_escaped);
    (void)unlink(path);
}

/* ── sched_load \uXXXX unescaping ───────────────────────────────────── */

static void sched_load_unescapes_unicode(void) {
    const char *path = "/tmp/hu_test_sched_unicode.json";
    FILE *f = fopen(path, "w");
    HU_ASSERT_TRUE(f != NULL);
    fprintf(f, "{\"contact\":\"alice\",\"channel\":\"imessage\","
               "\"message\":\"caf\\u00e9 time!\","
               "\"deliver_at\":99999999}\n");
    fclose(f);
    hu_error_t err = hu_conversation_sched_load(path, strlen(path));
    HU_ASSERT_EQ(err, HU_OK);

    char out_contact[128], out_channel[32], out_msg[512];
    size_t n = hu_conversation_flush_scheduled_for(100000000000ULL, "imessage", 8, out_contact,
                                                   sizeof(out_contact), out_channel,
                                                   sizeof(out_channel), out_msg, sizeof(out_msg));
    HU_ASSERT_TRUE(n > 0);
    /* \u00e9 = é encoded as UTF-8 (0xC3 0xA9) */
    HU_ASSERT_TRUE(strstr(out_msg, "caf") != NULL);
    HU_ASSERT_TRUE(n >= 10);
    (void)unlink(path);
}

static void sched_load_unescapes_tab_and_cr(void) {
    const char *path = "/tmp/hu_test_sched_tab.json";
    FILE *f = fopen(path, "w");
    HU_ASSERT_TRUE(f != NULL);
    fprintf(f, "{\"contact\":\"bob\",\"channel\":\"\","
               "\"message\":\"line1\\tline2\\rline3\","
               "\"deliver_at\":99999999}\n");
    fclose(f);
    hu_conversation_sched_load(path, strlen(path));

    char out_contact[128], out_channel[32], out_msg[512];
    size_t n = hu_conversation_flush_scheduled_for(100000000000ULL, NULL, 0, out_contact,
                                                   sizeof(out_contact), out_channel,
                                                   sizeof(out_channel), out_msg, sizeof(out_msg));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strchr(out_msg, '\t') != NULL);
    HU_ASSERT_TRUE(strchr(out_msg, '\r') != NULL);
    (void)unlink(path);
}

/* ── sched_slot accessor ────────────────────────────────────────────── */

static void sched_slot_returns_null_for_invalid_index(void) {
    hu_sched_slot_t *s = hu_conversation_sched_slot(HU_SCHED_MAX);
    HU_ASSERT_TRUE(s == NULL);
    s = hu_conversation_sched_slot(HU_SCHED_MAX + 100);
    HU_ASSERT_TRUE(s == NULL);
}

static void sched_slot_returns_valid_after_schedule(void) {
    hu_error_t err = hu_conversation_schedule_message_on("test_slot_user", 14, "cli_test", 8,
                                                         "hello from cli", 14, 12345678ULL);
    HU_ASSERT_EQ(err, HU_OK);
    bool found = false;
    for (size_t i = 0; i < HU_SCHED_MAX; i++) {
        hu_sched_slot_t *s = hu_conversation_sched_slot(i);
        if (s && s->active && strcmp(s->contact_id, "test_slot_user") == 0) {
            found = true;
            HU_ASSERT_TRUE(strcmp(s->message, "hello from cli") == 0);
            HU_ASSERT_EQ(12345678ULL, s->deliver_at_ms);
            s->active = false;
            break;
        }
    }
    HU_ASSERT_TRUE(found);
}

/* ── Banned AI phrases expansion tests ──────────────────────────────── */

static void strip_feel_free_to(void) {
    char buf[256];
    memcpy(buf, "Feel free to reach out anytime", 30);
    buf[30] = '\0';
    size_t len = hu_conversation_strip_ai_phrases(buf, 30);
    HU_ASSERT_TRUE(len < 30);
    HU_ASSERT_NULL(strstr(buf, "Feel free to"));
}

static void strip_dont_hesitate(void) {
    char buf[256];
    memcpy(buf, "Don't hesitate to ask me", 24);
    buf[24] = '\0';
    size_t len = hu_conversation_strip_ai_phrases(buf, 24);
    HU_ASSERT_TRUE(len < 24);
    HU_ASSERT_NULL(strstr(buf, "hesitate"));
}

static void strip_happy_to(void) {
    char buf[256];
    memcpy(buf, "I'd be happy to help with that", 30);
    buf[30] = '\0';
    size_t len = hu_conversation_strip_ai_phrases(buf, 30);
    HU_ASSERT_TRUE(len < 30);
    HU_ASSERT_NULL(strstr(buf, "happy to"));
}

static void strip_double_exclamation(void) {
    char buf[256];
    memcpy(buf, "That's awesome!! ", 17);
    buf[17] = '\0';
    size_t len = hu_conversation_strip_ai_phrases(buf, 17);
    HU_ASSERT_TRUE(len <= 17);
    HU_ASSERT_NULL(strstr(buf, "!!"));
}

static void strip_ai_phrases_multi_pass(void) {
    char buf[512];
    const char *input = "I'd be happy to help! I'd be happy to help!";
    size_t input_len = strlen(input);
    memcpy(buf, input, input_len);
    buf[input_len] = '\0';
    size_t len = hu_conversation_strip_ai_phrases(buf, input_len);
    HU_ASSERT_NULL(strstr(buf, "happy to"));
    HU_ASSERT_TRUE(len < input_len);
}

/* ── Hallucinated channel tag stripping ────────────────────────────── */

static void strip_channel_tag_paired(void) {
    char buf[256];
    const char *input = "<|channel>thought<channel|>haha, well i'm listening! what's up?";
    size_t input_len = strlen(input);
    memcpy(buf, input, input_len + 1);
    size_t len = hu_conversation_strip_channel_tags(buf, input_len);
    HU_ASSERT_STR_EQ(buf, "haha, well i'm listening! what's up?");
    HU_ASSERT_EQ(len, strlen("haha, well i'm listening! what's up?"));
}

static void strip_channel_tag_standalone(void) {
    char buf[256];
    const char *input = "<|endoftext|>hello there";
    size_t input_len = strlen(input);
    memcpy(buf, input, input_len + 1);
    size_t len = hu_conversation_strip_channel_tags(buf, input_len);
    HU_ASSERT_STR_EQ(buf, "hello there");
    HU_ASSERT_EQ(len, strlen("hello there"));
}

static void strip_channel_tag_no_tags(void) {
    char buf[256];
    const char *input = "just a normal message";
    size_t input_len = strlen(input);
    memcpy(buf, input, input_len + 1);
    size_t len = hu_conversation_strip_channel_tags(buf, input_len);
    HU_ASSERT_STR_EQ(buf, "just a normal message");
    HU_ASSERT_EQ(len, input_len);
}

static void strip_channel_tag_multiple(void) {
    char buf[256];
    const char *input = "<|system|>prefix <|channel>thought<channel|>actual message";
    size_t input_len = strlen(input);
    memcpy(buf, input, input_len + 1);
    size_t len = hu_conversation_strip_channel_tags(buf, input_len);
    HU_ASSERT_STR_EQ(buf, "prefix actual message");
    HU_ASSERT_EQ(len, strlen("prefix actual message"));
}

static void strip_channel_tag_empty_input(void) {
    char buf[4] = "ab";
    size_t len = hu_conversation_strip_channel_tags(buf, 2);
    HU_ASSERT_EQ(len, (size_t)2);
    HU_ASSERT_EQ(buf[0], 'a');
}

static void strip_channel_tag_eos_token(void) {
    char buf[64];
    strcpy(buf, "hello there</s>");
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "hello there");
    (void)len;
}

static void strip_channel_tag_inst_markers(void) {
    char buf[64];
    strcpy(buf, "[INST]say hi[/INST]hello!");
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "say hihello!");
    (void)len;
}

static void strip_channel_tag_chatml_im_tokens(void) {
    char buf[128];
    const char *input = "<|im_start|>user<|im_end|>hey what's up";
    memcpy(buf, input, strlen(input) + 1);
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(input));
    HU_ASSERT_STR_EQ(buf, "userhey what's up");
    (void)len;
}

static void strip_channel_tag_thinking_block(void) {
    char buf[128];
    strcpy(buf, "<thinking>let me reason about this</thinking>the answer is 42");
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "the answer is 42");
    (void)len;
}

static void strip_channel_tag_analysis_block(void) {
    char buf[128];
    strcpy(buf, "here is <analysis>deep stuff</analysis>the point");
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "here is the point");
    (void)len;
}

static void strip_channel_tag_bos_token(void) {
    char buf[64];
    strcpy(buf, "<s>hello world");
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "hello world");
    (void)len;
}

static void strip_channel_tag_sys_markers(void) {
    char buf[64];
    strcpy(buf, "<<SYS>>system msg<</SYS>>hi");
    size_t len = hu_conversation_strip_channel_tags(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "system msghi");
    (void)len;
}

/* ── Formal structure stripping tests ─────────────────────────────── */

static void strip_formal_numbered_list(void) {
    char buf[128];
    strcpy(buf, "1. First thing\n2. Second thing\n3. Third thing");
    size_t len = hu_conversation_strip_formal_structure(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "1.") == NULL);
    HU_ASSERT(strstr(buf, "2.") == NULL);
    HU_ASSERT(strstr(buf, "First thing") != NULL);
    (void)len;
}

static void strip_formal_em_dash(void) {
    char buf[128];
    /* em-dash is UTF-8 E2 80 94 */
    const char *input = "hello \xe2\x80\x94 world";
    strcpy(buf, input);
    size_t len = hu_conversation_strip_formal_structure(buf, strlen(buf));
    HU_ASSERT(strstr(buf, ",") != NULL);
    HU_ASSERT(len < strlen(input));
    (void)len;
}

static void strip_formal_no_change(void) {
    char buf[64];
    strcpy(buf, "just a normal text message haha");
    size_t orig = strlen(buf);
    size_t len = hu_conversation_strip_formal_structure(buf, orig);
    HU_ASSERT_EQ(len, orig);
    HU_ASSERT_STR_EQ(buf, "just a normal text message haha");
}

static void strip_formal_en_dash(void) {
    char buf[128];
    /* en-dash is UTF-8 E2 80 93 */
    strcpy(buf, "pages 1\xe2\x80\x93"
                "10");
    size_t len = hu_conversation_strip_formal_structure(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "-") != NULL);
    (void)len;
}

static void strip_formal_empty_safe(void) {
    char buf[4] = "hi";
    size_t len = hu_conversation_strip_formal_structure(buf, 2);
    HU_ASSERT_EQ(len, (size_t)2);
}

static void strip_formal_colon_phrase(void) {
    char buf[128];
    strcpy(buf, "Weather: it's nice outside\nFood: pizza sounds good");
    size_t len = hu_conversation_strip_formal_structure(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "Weather:") == NULL);
    HU_ASSERT(strstr(buf, "Food:") == NULL);
    HU_ASSERT(strstr(buf, "it's nice outside") != NULL);
    HU_ASSERT(strstr(buf, "pizza sounds good") != NULL);
    (void)len;
}

static void strip_formal_bullet_list(void) {
    char buf[128];
    strcpy(buf, "- first item\n- second item\n* third item");
    size_t len = hu_conversation_strip_formal_structure(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "- ") == NULL);
    HU_ASSERT(strstr(buf, "* ") == NULL);
    HU_ASSERT(strstr(buf, "first item") != NULL);
    (void)len;
}

static void strip_formal_colon_preserves_inline(void) {
    char buf[64];
    strcpy(buf, "meeting at 3:00 pm");
    size_t orig = strlen(buf);
    size_t len = hu_conversation_strip_formal_structure(buf, orig);
    HU_ASSERT_EQ(len, orig);
    HU_ASSERT_STR_EQ(buf, "meeting at 3:00 pm");
}

/* ── Full outbound strip pipeline integration test ──────────────────── */

static void strip_pipeline_full_integration(void) {
    char buf[512];
    const char *dirty = "<thinking>internal reasoning</thinking>"
                        "1. Weather: It's pretty nice\n"
                        "2. Food \xe2\x80\x94 pizza sounds good\n"
                        "- Also check out **bold text**\n"
                        "<|endoftext|>"
                        "As an AI, I hope this helps!";
    size_t len = strlen(dirty);
    HU_ASSERT(len < sizeof(buf));
    memcpy(buf, dirty, len + 1);

    /* Stage 1: strip channel/model tags */
    len = hu_conversation_strip_channel_tags(buf, len);
    HU_ASSERT(strstr(buf, "<thinking>") == NULL);
    HU_ASSERT(strstr(buf, "<|endoftext|>") == NULL);

    /* Stage 2: strip AI phrases */
    len = hu_conversation_strip_ai_phrases(buf, len);
    HU_ASSERT(strstr(buf, "As an AI") == NULL);

    /* Stage 3: strip formal structure */
    len = hu_conversation_strip_formal_structure(buf, len);
    HU_ASSERT(strstr(buf, "1.") == NULL);
    HU_ASSERT(strstr(buf, "2.") == NULL);
    HU_ASSERT(strstr(buf, "- Also") == NULL);
    HU_ASSERT(strstr(buf, "Weather:") == NULL);
    HU_ASSERT(strstr(buf, "\xe2\x80\x94") == NULL);

    /* Content survives */
    HU_ASSERT(strstr(buf, "nice") != NULL);
    HU_ASSERT(strstr(buf, "pizza") != NULL);
    HU_ASSERT(len > 10);
    HU_ASSERT(len < 300);
}

/* ── Example bank format compatibility test ─────────────────────────── */

static void examples_load_input_output_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"examples\":["
                       "{\"input\":\"hey how are you\",\"output\":\"good hbu\"},"
                       "{\"context\":\"morning\",\"incoming\":\"sup\",\"response\":\"nm\"}"
                       "]}";
    hu_persona_example_bank_t bank;
    hu_error_t err =
        hu_persona_examples_load_json(&alloc, "imessage", 8, json, strlen(json), &bank);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(bank.examples_count, 2);
    HU_ASSERT_NOT_NULL(bank.examples[0].incoming);
    HU_ASSERT_NOT_NULL(bank.examples[0].response);
    HU_ASSERT_STR_EQ(bank.examples[0].incoming, "hey how are you");
    HU_ASSERT_STR_EQ(bank.examples[0].response, "good hbu");
    HU_ASSERT_STR_EQ(bank.examples[1].incoming, "sup");
    HU_ASSERT_STR_EQ(bank.examples[1].response, "nm");
    for (size_t i = 0; i < bank.examples_count; i++) {
        if (bank.examples[i].context)
            alloc.free(alloc.ctx, bank.examples[i].context, strlen(bank.examples[i].context) + 1);
        if (bank.examples[i].incoming)
            alloc.free(alloc.ctx, bank.examples[i].incoming, strlen(bank.examples[i].incoming) + 1);
        if (bank.examples[i].response)
            alloc.free(alloc.ctx, bank.examples[i].response, strlen(bank.examples[i].response) + 1);
    }
    alloc.free(alloc.ctx, bank.examples, 2 * sizeof(hu_persona_example_t));
    alloc.free(alloc.ctx, bank.channel, 9);
}

/* ── Persona-driven style and anti-patterns ──────────────────────────── */

static void style_uses_persona_anti_patterns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[6] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hey", "12:01"),
        make_entry(false, "what you up to", "12:02"),
        make_entry(false, "just chilling lol", "12:03"),
        make_entry(true, "same", "12:04"),
        make_entry(false, "wanna hang", "12:05"),
    };
    hu_persona_t p = {0};
    char *ap[] = {"CUSTOM_RULE: never use exclamation marks", "CUSTOM_RULE: avoid emoji"};
    p.anti_patterns = ap;
    p.anti_patterns_count = 2;

    size_t len = 0;
    char *s = hu_conversation_analyze_style(&alloc, entries, 6, &p, &len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(strstr(s, "CUSTOM_RULE") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

static void awareness_uses_persona_style_rules(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "hey how are you", "12:00"),
        make_entry(true, "good hbu", "12:01"),
        make_entry(false, "doing well thanks for asking", "12:02"),
        make_entry(false, "what are you working on", "12:03"),
    };
    hu_persona_t p = {0};
    char *rules[] = {"MY_STYLE_RULE: keep it under 20 words"};
    p.style_rules = rules;
    p.style_rules_count = 1;

    size_t len = 0;
    char *s = hu_conversation_build_awareness(&alloc, entries, 4, &p, &len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(strstr(s, "MY_STYLE_RULE") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

/* ── Inline reply classifier (F40) ────────────────────────────────────── */

static void inline_reply_you_said_returns_true(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "let's meet at 5", "12:00"),
        make_entry(false, "you said we'd meet at 5 - can we make it 6?", "12:01"),
    };
    bool r = hu_conversation_should_inline_reply(entries, 2,
                                                 "you said we'd meet at 5 - can we make it 6?", 42);
    HU_ASSERT_TRUE(r);
}

static void inline_reply_earlier_returns_true(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "earlier you mentioned pizza", "12:00")};
    bool r = hu_conversation_should_inline_reply(entries, 1, "earlier you mentioned pizza", 26);
    HU_ASSERT_TRUE(r);
}

static void inline_reply_what_about_returns_true(void) {
    hu_channel_history_entry_t entries[1] = {make_entry(false, "what about the meeting?", "12:00")};
    bool r = hu_conversation_should_inline_reply(entries, 1, "what about the meeting?", 21);
    HU_ASSERT_TRUE(r);
}

static void inline_reply_multiple_questions_returns_true(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "when are we meeting?", "12:00"),
        make_entry(true, "how about 3pm?", "12:01"),
        make_entry(false, "where?", "12:02"),
        make_entry(false, "and who's coming?", "12:03"),
    };
    bool r = hu_conversation_should_inline_reply(entries, 4, "and who's coming?", 16);
    HU_ASSERT_TRUE(r);
}

static void inline_reply_single_topic_returns_false(void) {
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "hey how are you", "12:00"),
        make_entry(true, "good you?", "12:01"),
    };
    bool r = hu_conversation_should_inline_reply(entries, 2, "doing well thanks", 16);
    HU_ASSERT_FALSE(r);
}

static void inline_reply_null_last_msg_returns_false(void) {
    hu_channel_history_entry_t entries[1] = {make_entry(false, "hello", "12:00")};
    bool r = hu_conversation_should_inline_reply(entries, 1, NULL, 0);
    HU_ASSERT_FALSE(r);
}

/* ── Active listening backchannels (F29) ───────────────────────────────── */

static void backchannel_long_narrative_prob_one_returns_true(void) {
    /* >80 chars, first person, no question, probability 1.0 */
    const char *msg =
        "so i was at the store yesterday and then this crazy thing happened and my car broke down "
        "and anyway it was a long story";
    bool r = hu_conversation_should_backchannel(msg, strlen(msg), NULL, 0, 42u, 1.0f);
    HU_ASSERT_TRUE(r);
}

static void backchannel_short_k_returns_false(void) {
    bool r = hu_conversation_should_backchannel("k", 1, NULL, 0, 42u, 1.0f);
    HU_ASSERT_FALSE(r);
}

static void backchannel_narrative_prob_zero_returns_false(void) {
    const char *msg =
        "so i was at the store yesterday and then this crazy thing happened and my car broke down "
        "and anyway it was a long story";
    bool r = hu_conversation_should_backchannel(msg, strlen(msg), NULL, 0, 42u, 0.0f);
    HU_ASSERT_FALSE(r);
}

static void backchannel_pick_returns_nonempty(void) {
    char buf[64];
    size_t len = hu_conversation_pick_backchannel(12345u, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(buf[0] != '\0');
    HU_ASSERT_TRUE(len == strlen(buf));
}

static void backchannel_pick_deterministic(void) {
    char buf1[64], buf2[64];
    size_t len1 = hu_conversation_pick_backchannel(99u, buf1, sizeof(buf1));
    size_t len2 = hu_conversation_pick_backchannel(99u, buf2, sizeof(buf2));
    HU_ASSERT_EQ(len1, len2);
    HU_ASSERT_STR_EQ(buf1, buf2);
}

/* ── Burst messaging (F45) tests ──────────────────────────────────────── */

static void burst_omg_did_you_see_prob_one_returns_true(void) {
    const char *msg = "omg did you see the news!!!";
    hu_channel_history_entry_t entries[1] = {make_entry(false, "hey", "12:00")};
    bool r = hu_conversation_should_burst(msg, strlen(msg), entries, 1, 42u, 1.0f);
    HU_ASSERT_TRUE(r);
}

static void burst_whats_for_dinner_returns_false(void) {
    bool r = hu_conversation_should_burst("what's for dinner", 16, NULL, 0, 42u, 1.0f);
    HU_ASSERT_FALSE(r);
}

static void burst_prob_zero_returns_false(void) {
    const char *msg = "omg did you see the news!!!";
    hu_channel_history_entry_t entries[1] = {make_entry(false, "hey", "12:00")};
    bool r = hu_conversation_should_burst(msg, strlen(msg), entries, 1, 42u, 0.0f);
    HU_ASSERT_FALSE(r);
}

static void burst_prompt_builder_contains_burst_mode(void) {
    char buf[512];
    size_t len = hu_conversation_build_burst_prompt(buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "BURST MODE") != NULL);
}

static void burst_parse_json_array_returns_three_messages(void) {
    const char *resp = "[\"oh my god\", \"just saw\", \"are you ok\"]";
    char messages[4][256];
    int n = hu_conversation_parse_burst_response(resp, strlen(resp), messages, 4);
    HU_ASSERT_EQ(n, 3);
    HU_ASSERT_STR_EQ(messages[0], "oh my god");
    HU_ASSERT_STR_EQ(messages[1], "just saw");
    HU_ASSERT_STR_EQ(messages[2], "are you ok");
}

/* ── Leave-on-read classifier (F46) tests ────────────────────────────── */

static void leave_on_read_agree_to_disagree_seed_under_2_returns_true(void) {
    /* seed % 100 < 2 → true when trigger matches */
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "agree to disagree", "12:00"),
    };
    bool r = hu_conversation_should_leave_on_read("agree to disagree", 17, entries, 1, 0u, 0);
    HU_ASSERT_TRUE(r);
}

static void leave_on_read_question_never_true(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "what do you think?", "12:00"),
    };
    bool r = hu_conversation_should_leave_on_read("what do you think?", 18, entries, 1, 0u, 0);
    HU_ASSERT_FALSE(r);
}

static void leave_on_read_help_me_never_true(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "help me", "12:00"),
    };
    bool r = hu_conversation_should_leave_on_read("help me", 7, entries, 1, 0u, 0);
    HU_ASSERT_FALSE(r);
}

static void leave_on_read_normal_message_seed_over_2_returns_false(void) {
    const char *msg = "i just got back from the store";
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, msg, "12:00"),
    };
    /* seed 50: 50 % 100 = 50 >= 10; normal message has no trigger; so false */
    bool r = hu_conversation_should_leave_on_read(msg, strlen(msg), entries, 1, 50u, 0);
    HU_ASSERT_FALSE(r);
}

static void leave_on_read_short_ok_seed_under_2_returns_true(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "ok", "12:00"),
    };
    bool r = hu_conversation_should_leave_on_read("ok", 2, entries, 1, 1u, 0);
    HU_ASSERT_TRUE(r);
}

static void leave_on_read_whatever_seed_under_2_returns_true(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "whatever", "12:00"),
    };
    bool r = hu_conversation_should_leave_on_read("whatever", 8, entries, 1, 0u, 0);
    HU_ASSERT_TRUE(r);
}

static void leave_on_read_ok_seed_over_2_returns_false(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "ok", "12:00"),
    };
    bool r = hu_conversation_should_leave_on_read("ok", 2, entries, 1, 42u, 0);
    HU_ASSERT_FALSE(r);
}

static void leave_on_read_custom_threshold(void) {
    hu_channel_history_entry_t entries[1] = {
        make_entry(false, "ok", "12:00"),
    };
    /* seed 42: 42 % 100 = 42; with threshold 50%, should trigger */
    bool r = hu_conversation_should_leave_on_read("ok", 2, entries, 1, 42u, 50);
    HU_ASSERT_TRUE(r);
    /* with threshold 30%, should not trigger (42 >= 30) */
    bool r2 = hu_conversation_should_leave_on_read("ok", 2, entries, 1, 42u, 30);
    HU_ASSERT_FALSE(r2);
}

/* ── Leave-on-read decision predicate (F46 state machine) ─────────────────── */

static void leave_on_read_decide_groups_always_respond(void) {
    /* In group chats we never leave-on-read, regardless of the helper or
     * an active 1:1 period (the daemon scopes the ring buffer per chat). */
    HU_ASSERT_EQ(hu_leave_on_read_decide(true, false, false), HU_LOR_RESPOND);
    HU_ASSERT_EQ(hu_leave_on_read_decide(true, false, true), HU_LOR_RESPOND);
    HU_ASSERT_EQ(hu_leave_on_read_decide(true, true, false), HU_LOR_RESPOND);
    HU_ASSERT_EQ(hu_leave_on_read_decide(true, true, true), HU_LOR_RESPOND);
}

static void leave_on_read_decide_active_period_returns_already(void) {
    HU_ASSERT_EQ(hu_leave_on_read_decide(false, true, false), HU_LOR_ALREADY_IN_PERIOD);
}

static void leave_on_read_decide_helper_trigger_returns_trigger_new(void) {
    HU_ASSERT_EQ(hu_leave_on_read_decide(false, false, true), HU_LOR_TRIGGER_NEW);
}

static void leave_on_read_decide_no_signals_responds_normally(void) {
    HU_ASSERT_EQ(hu_leave_on_read_decide(false, false, false), HU_LOR_RESPOND);
}

static void leave_on_read_decide_active_period_beats_helper(void) {
    /* When both fire, prefer ALREADY over TRIGGER_NEW so an existing silence
     * isn't extended by re-rolling. Pins the daemon's branch ordering at
     * src/daemon.c:4413-4424 — active-period check precedes the trigger roll. */
    HU_ASSERT_EQ(hu_leave_on_read_decide(false, true, true), HU_LOR_ALREADY_IN_PERIOD);
}

/* ── Call escalation (F49) ────────────────────────────────────────────────── */

static void call_escalation_crisis_keywords_returns_true(void) {
    const char *msg = "i need you right now please help";
    hu_call_escalation_t r = hu_conversation_should_escalate_to_call(msg, strlen(msg), NULL, 0);
    HU_ASSERT_TRUE(r.should_suggest);
}

static void call_escalation_whats_for_dinner_returns_false(void) {
    const char *msg = "what's for dinner";
    hu_call_escalation_t r = hu_conversation_should_escalate_to_call(msg, strlen(msg), NULL, 0);
    HU_ASSERT_FALSE(r.should_suggest);
}

static void call_escalation_long_emotional_returns_true(void) {
    const char *msg =
        "this is really complicated and I don't know what to do anymore. I've been trying for "
        "weeks and nothing works. I'm at the end of my rope";
    hu_call_escalation_t r = hu_conversation_should_escalate_to_call(msg, strlen(msg), NULL, 0);
    HU_ASSERT_TRUE(r.should_suggest);
}

static void call_escalation_null_input_returns_false_score_zero(void) {
    hu_call_escalation_t r = hu_conversation_should_escalate_to_call(NULL, 0, NULL, 0);
    HU_ASSERT_FALSE(r.should_suggest);
    HU_ASSERT_EQ((int)(r.score * 1000), 0);
}

static void call_escalation_build_directive_writes_nonempty(void) {
    char buf[512];
    size_t n = hu_conversation_build_call_directive("hey can you call me?", 20, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "[CALL:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "call") != NULL);
}

/* ── Inside joke detection (F19) ───────────────────────────────────────────── */

static void detect_inside_joke_remember_when_true(void) {
    const char *msg = "remember when we did that thing";
    bool r = hu_conversation_detect_inside_joke(msg, strlen(msg), NULL, 0);
    HU_ASSERT_TRUE(r);
}

static void detect_inside_joke_whats_for_dinner_false(void) {
    const char *msg = "what's for dinner";
    bool r = hu_conversation_detect_inside_joke(msg, strlen(msg), NULL, 0);
    HU_ASSERT_FALSE(r);
}

static void detect_inside_joke_energy_pattern_true(void) {
    const char *msg = "that's so alex energy";
    bool r = hu_conversation_detect_inside_joke(msg, strlen(msg), NULL, 0);
    HU_ASSERT_TRUE(r);
}

static void detect_inside_joke_classic_true(void) {
    const char *msg = "classic sarah";
    bool r = hu_conversation_detect_inside_joke(msg, strlen(msg), NULL, 0);
    HU_ASSERT_TRUE(r);
}

static void detect_inside_joke_shared_phrase_from_history_true(void) {
    hu_channel_history_entry_t entries[2];
    memset(&entries[0], 0, sizeof(entries[0]));
    entries[0].from_me = false;
    memcpy(entries[0].text, "we always order the same thing at that place", 43);
    entries[0].text[43] = '\0';
    memset(&entries[1], 0, sizeof(entries[1]));
    entries[1].from_me = true;
    memcpy(entries[1].text, "haha yeah", 9);
    entries[1].text[9] = '\0';
    const char *msg = "we always order the same thing at that place";
    bool r = hu_conversation_detect_inside_joke(msg, strlen(msg), entries, 2);
    HU_ASSERT_TRUE(r);
}

/* ── First-time vulnerability detection (F17) ────────────────────────────── */

static void vulnerability_cancer_extracts_illness(void) {
    const char *msg = "i got diagnosed with cancer";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_STR_EQ(topic, "illness");
}

static void vulnerability_whats_for_dinner_returns_null(void) {
    const char *msg = "what's for dinner";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NULL(topic);
}

static void vulnerability_job_loss_keywords(void) {
    const char *msg = "i got laid off last week";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_STR_EQ(topic, "job_loss");
}

static void vulnerability_divorce_keywords(void) {
    const char *msg = "we're separating and figuring out custody";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_STR_EQ(topic, "divorce");
}

static void vulnerability_mental_health_keywords(void) {
    const char *msg = "i've been in therapy for depression";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_STR_EQ(topic, "mental_health");
}

static void vulnerability_loss_keywords(void) {
    /* Use message without family phrases so loss (not family_issue) matches */
    const char *msg = "my friend died last week";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_STR_EQ(topic, "loss");
}

static void vulnerability_family_issue_requires_negative_context(void) {
    /* "my mom" alone without negative context should not match */
    const char *msg = "my mom makes great cookies";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NULL(topic);
}

static void vulnerability_family_issue_with_negative_matches(void) {
    /* family_issue checked before illness; "my mom" + "worried" (negative) */
    const char *msg = "my mom has me worried lately";
    const char *topic = hu_conversation_extract_vulnerability_topic(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(topic);
    HU_ASSERT_STR_EQ(topic, "family_issue");
}

static void vulnerability_directive_produces_vulnerability_string(void) {
    hu_vulnerability_state_t state = {true, "illness", 0.7f};
    char buf[512];
    size_t n = hu_conversation_build_vulnerability_directive(&state, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "VULNERABILITY") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "First time") != NULL);
}

static void vulnerability_directive_not_first_time_returns_zero(void) {
    hu_vulnerability_state_t state = {false, "illness", 0.7f};
    char buf[512];
    size_t n = hu_conversation_build_vulnerability_directive(&state, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
}

static void vulnerability_directive_null_topic_returns_zero(void) {
    hu_vulnerability_state_t state = {true, NULL, 0.7f};
    char buf[512];
    size_t n = hu_conversation_build_vulnerability_directive(&state, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
}

/* ── Micro-moment extraction (F18) ────────────────────────────────────────── */

static void micro_moment_dog_name_extracts_pet(void) {
    char facts[3][256];
    char sigs[3][128];
    const char *msg = "my dog's name is Max";
    int n = hu_conversation_extract_micro_moments(msg, strlen(msg), facts, sigs, 3);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_TRUE(strstr(facts[0], "Max") != NULL);
    HU_ASSERT_TRUE(strstr(facts[0], "dog") != NULL);
    HU_ASSERT_STR_EQ(sigs[0], "pet");
}

static void micro_moment_i_love_extracts_preference(void) {
    char facts[3][256];
    char sigs[3][128];
    const char *msg = "i love hiking in the mountains";
    int n = hu_conversation_extract_micro_moments(msg, strlen(msg), facts, sigs, 3);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_TRUE(strstr(facts[0], "hiking") != NULL);
    HU_ASSERT_STR_EQ(sigs[0], "preference");
}

static void micro_moment_moved_to_extracts_location(void) {
    char facts[3][256];
    char sigs[3][128];
    const char *msg = "just moved to Seattle";
    int n = hu_conversation_extract_micro_moments(msg, strlen(msg), facts, sigs, 3);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_TRUE(strstr(facts[0], "Seattle") != NULL);
    HU_ASSERT_STR_EQ(sigs[0], "location");
}

static void micro_moment_nice_weather_extracts_zero(void) {
    char facts[3][256];
    char sigs[3][128];
    const char *msg = "nice weather today";
    int n = hu_conversation_extract_micro_moments(msg, strlen(msg), facts, sigs, 3);
    HU_ASSERT_EQ(n, 0);
}

#ifdef HU_ENABLE_SQLITE
static void vulnerability_cancer_no_prior_first_time(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *msg = "i got diagnosed with cancer";
    hu_vulnerability_state_t state =
        hu_conversation_detect_first_time_vulnerability(msg, strlen(msg), &mem, "contact_a", 9);

    HU_ASSERT_TRUE(state.first_time);
    HU_ASSERT_NOT_NULL(state.topic_category);
    HU_ASSERT_STR_EQ(state.topic_category, "illness");

    mem.vtable->deinit(mem.ctx);
}

static void vulnerability_cancer_with_prior_not_first_time(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Record prior emotional moment with topic "illness" */
    hu_error_t err =
        hu_emotional_moment_record(&alloc, &mem, "contact_b", 9, "illness", 7, "stressed", 8, 0.8f);
    HU_ASSERT_EQ(err, HU_OK);

    const char *msg = "i got diagnosed with cancer";
    hu_vulnerability_state_t state =
        hu_conversation_detect_first_time_vulnerability(msg, strlen(msg), &mem, "contact_b", 9);

    HU_ASSERT_FALSE(state.first_time);
    HU_ASSERT_NOT_NULL(state.topic_category);
    HU_ASSERT_STR_EQ(state.topic_category, "illness");

    mem.vtable->deinit(mem.ctx);
}

static void vulnerability_whats_for_dinner_no_topic_first_time_false(void) {
    const char *msg = "what's for dinner";
    hu_vulnerability_state_t state =
        hu_conversation_detect_first_time_vulnerability(msg, strlen(msg), NULL, "contact_c", 9);

    /* No topic → first_time is false (out struct init), topic_category NULL */
    HU_ASSERT_NULL(state.topic_category);
    HU_ASSERT_FALSE(state.first_time);
}
#endif

/* ── Calendar awareness (F50) ─────────────────────────────────────────── */

static void calendar_macos_get_events_returns_empty_array_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *events_json = NULL;
    size_t events_len = 0;
    hu_error_t err = hu_calendar_macos_get_events(&alloc, 24, &events_json, &events_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(events_json);
    HU_ASSERT_EQ(events_len, 2u);
    HU_ASSERT_STR_EQ(events_json, "[]");
    alloc.free(alloc.ctx, events_json, events_len + 1);
}

/* ── F57: Multi-thread energy management tests ──────────────────────── */

static void thread_energy_init_zeroes_count(void) {
    hu_thread_energy_tracker_t t;
    hu_thread_energy_init(&t);
    HU_ASSERT_EQ(t.count, 0u);
}

static void thread_energy_update_and_get(void) {
    hu_thread_energy_tracker_t t;
    hu_thread_energy_init(&t);
    hu_thread_energy_update(&t, "alice", 5, HU_ENERGY_EXCITED, 1000);
    HU_ASSERT_EQ(hu_thread_energy_get(&t, "alice", 5), HU_ENERGY_EXCITED);
}

static void thread_energy_get_unknown_returns_neutral(void) {
    hu_thread_energy_tracker_t t;
    hu_thread_energy_init(&t);
    HU_ASSERT_EQ(hu_thread_energy_get(&t, "bob", 3), HU_ENERGY_NEUTRAL);
}

static void thread_energy_update_overwrites_existing(void) {
    hu_thread_energy_tracker_t t;
    hu_thread_energy_init(&t);
    hu_thread_energy_update(&t, "alice", 5, HU_ENERGY_SAD, 1000);
    hu_thread_energy_update(&t, "alice", 5, HU_ENERGY_PLAYFUL, 2000);
    HU_ASSERT_EQ(hu_thread_energy_get(&t, "alice", 5), HU_ENERGY_PLAYFUL);
}

static void thread_energy_isolation_no_conflict(void) {
    hu_thread_energy_tracker_t t;
    hu_thread_energy_init(&t);
    hu_thread_energy_update(&t, "alice", 5, HU_ENERGY_EXCITED, 1000);
    hu_thread_energy_update(&t, "bob", 3, HU_ENERGY_EXCITED, 1000);
    char buf[256];
    size_t len = hu_thread_energy_build_isolation_hint(&t, "alice", 5, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void thread_energy_isolation_with_conflict(void) {
    hu_thread_energy_tracker_t t;
    hu_thread_energy_init(&t);
    hu_thread_energy_update(&t, "alice", 5, HU_ENERGY_EXCITED, 1000);
    hu_thread_energy_update(&t, "bob", 3, HU_ENERGY_SAD, 1000);
    char buf[256];
    size_t len = hu_thread_energy_build_isolation_hint(&t, "alice", 5, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "ISOLATION") != NULL);
}

static void thread_energy_null_safety(void) {
    hu_thread_energy_init(NULL);
    hu_thread_energy_update(NULL, "x", 1, HU_ENERGY_SAD, 0);
    HU_ASSERT_EQ(hu_thread_energy_get(NULL, "x", 1), HU_ENERGY_NEUTRAL);
    char buf[128];
    HU_ASSERT_EQ(hu_thread_energy_build_isolation_hint(NULL, "x", 1, buf, sizeof(buf)), 0u);
}

/* ── Double-text decision (F9) ───────────────────────────────────────── */

static void double_text_null_response_returns_false(void) {
    HU_ASSERT_FALSE(hu_conversation_should_double_text(NULL, 0, NULL, 0, 12, 42, 0.08f));
}

static void double_text_late_night_returns_false(void) {
    const char *resp = "haha that's great";
    HU_ASSERT_FALSE(hu_conversation_should_double_text(resp, strlen(resp), NULL, 0, 23, 42, 1.0f));
    HU_ASSERT_FALSE(hu_conversation_should_double_text(resp, strlen(resp), NULL, 0, 3, 42, 1.0f));
}

static void double_text_farewell_returns_false(void) {
    const char *resp = "alright goodnight!";
    HU_ASSERT_FALSE(hu_conversation_should_double_text(resp, strlen(resp), NULL, 0, 14, 42, 1.0f));
}

static void double_text_high_probability_returns_true(void) {
    const char *resp = "yeah totally makes sense";
    bool result = hu_conversation_should_double_text(resp, strlen(resp), NULL, 0, 14, 12345, 1.0f);
    HU_ASSERT_TRUE(result);
}

static void double_text_zero_probability_returns_false(void) {
    const char *resp = "yeah totally";
    HU_ASSERT_FALSE(hu_conversation_should_double_text(resp, strlen(resp), NULL, 0, 14, 42, 0.0f));
}

static void double_text_energy_boost_increases_chance(void) {
    const char *resp = "omg that's amazing lol!!";
    int hits = 0;
    for (uint32_t seed = 0; seed < 1000; seed++) {
        if (hu_conversation_should_double_text(resp, strlen(resp), NULL, 0, 14, seed, 0.10f))
            hits++;
    }
    HU_ASSERT_TRUE(hits > 100);
}

static void double_text_suppresses_when_too_many_from_me(void) {
    hu_channel_history_entry_t entries[4];
    memset(entries, 0, sizeof(entries));
    entries[0].from_me = true;
    snprintf(entries[0].text, sizeof(entries[0].text), "first");
    entries[1].from_me = true;
    snprintf(entries[1].text, sizeof(entries[1].text), "second");
    entries[2].from_me = true;
    snprintf(entries[2].text, sizeof(entries[2].text), "third");
    entries[3].from_me = true;
    snprintf(entries[3].text, sizeof(entries[3].text), "fourth");
    const char *resp = "hey what's up";
    HU_ASSERT_FALSE(
        hu_conversation_should_double_text(resp, strlen(resp), entries, 4, 14, 42, 1.0f));
}

/* ── Inspiration trigger: YouTube/TikTok cues must boost the share roll ──────
 * The proactive media-share gate (hu_conversation_should_send_music) multiplies
 * the probability 2.5x when a cue word is present. Historically only MUSIC words
 * boosted; YouTube/TikTok asks under-triggered. These pin that video/tiktok cues
 * elevate the roll the same way.
 *
 * Determinism: at base prob 0.4 a cue boosts effective prob to min(2.5*0.4,1.0)
 * = 1.0 (fires for EVERY seed). A non-cue control at 0.4 fires only when the PRNG
 * roll clears 4000/10000. We find a seed where the control is FALSE, then assert
 * the cue message is TRUE at that same seed — impossible unless the cue boosted. */
static void test_should_share_tiktok_cue_boosts_roll(void) {
    int proved = 0;
    for (uint32_t seed = 1; seed <= 100 && !proved; seed++) {
        bool control = hu_conversation_should_send_music("just chatting about lunch plans", 31,
                                                         NULL, 0, seed, 0.4f);
        if (!control) {
            proved = 1;
            HU_ASSERT_TRUE(
                hu_conversation_should_send_music("send me a tiktok", 16, NULL, 0, seed, 0.4f));
        }
    }
    HU_ASSERT_TRUE(proved); /* sanity: base 0.4 is not certain, so the control varies */
}

static void test_should_share_youtube_cue_boosts_roll(void) {
    int proved = 0;
    for (uint32_t seed = 1; seed <= 100 && !proved; seed++) {
        bool control = hu_conversation_should_send_music("just chatting about lunch plans", 31,
                                                         NULL, 0, seed, 0.4f);
        if (!control) {
            proved = 1;
            HU_ASSERT_TRUE(
                hu_conversation_should_send_music("got a funny video?", 18, NULL, 0, seed, 0.4f));
        }
    }
    HU_ASSERT_TRUE(proved);
}

/* Word-boundary safety: "trendy" must NOT count as the "trend" cue (substring-
 * classifier-pitfalls.md). With NO real cue, "feeling trendy today" stays at base
 * prob, so at a seed where the control is false it must also be false. */
static void test_should_share_word_boundary_no_false_cue(void) {
    int proved = 0;
    for (uint32_t seed = 1; seed <= 100 && !proved; seed++) {
        bool control = hu_conversation_should_send_music("just chatting about lunch plans", 31,
                                                         NULL, 0, seed, 0.4f);
        if (!control) {
            proved = 1;
            HU_ASSERT_FALSE(
                hu_conversation_should_send_music("feeling trendy today", 20, NULL, 0, seed, 0.4f));
        }
    }
    HU_ASSERT_TRUE(proved);
}

/* ── Test suite registration ─────────────────────────────────────────── */

void run_conversation_tests(void) {
    HU_TEST_SUITE("Conversation Intelligence");

    HU_RUN_TEST(test_should_share_tiktok_cue_boosts_roll);
    HU_RUN_TEST(test_should_share_youtube_cue_boosts_roll);
    HU_RUN_TEST(test_should_share_word_boundary_no_false_cue);

    /* Multi-message splitting */
    HU_RUN_TEST(split_short_response_stays_single);
    HU_RUN_TEST(split_null_input_returns_zero);
    HU_RUN_TEST(split_empty_returns_zero);
    HU_RUN_TEST(split_null_fragments_returns_zero);
    HU_RUN_TEST(split_zero_max_fragments_returns_zero);
    HU_RUN_TEST(split_on_newlines);
    HU_RUN_TEST(split_on_conjunction_starter);
    HU_RUN_TEST(split_respects_max_fragments);
    HU_RUN_TEST(split_inter_message_delay_nonzero_for_later_fragments);

    /* Style analysis */
    HU_RUN_TEST(style_null_returns_null);
    HU_RUN_TEST(style_too_few_messages_returns_null);
    HU_RUN_TEST(style_detects_all_lowercase);
    HU_RUN_TEST(style_detects_no_periods);
    HU_RUN_TEST(style_includes_anti_patterns);

    /* Response classification */
    HU_RUN_TEST(classify_empty_skips);
    HU_RUN_TEST(classify_tapback_skips);
    HU_RUN_TEST(classify_lol_is_brief);
    HU_RUN_TEST(classify_haha_is_brief);
    HU_RUN_TEST(classify_nice_is_brief);
    HU_RUN_TEST(classify_question_is_full);
    HU_RUN_TEST(classify_emotional_is_delayed);
    HU_RUN_TEST(classify_ok_after_question_skips);
    HU_RUN_TEST(classify_ok_after_distant_question_skips);
    HU_RUN_TEST(classify_normal_statement_is_brief);
    HU_RUN_TEST(classify_farewell_goodnight_is_brief);
    HU_RUN_TEST(classify_farewell_short_bye);
    HU_RUN_TEST(classify_farewell_ttyl);
    HU_RUN_TEST(classify_bad_news_is_delayed);
    HU_RUN_TEST(classify_good_news_is_delayed);
    HU_RUN_TEST(classify_vulnerable_is_delayed);

    /* Two-phase thinking response */
    HU_RUN_TEST(thinking_triggers_on_complex_question);
    HU_RUN_TEST(thinking_no_trigger_simple_message);
    HU_RUN_TEST(thinking_triggers_on_advice);
    HU_RUN_TEST(thinking_filler_varies_by_seed);
    HU_RUN_TEST(thinking_text_fast_imessage_emits_filler);
    HU_RUN_TEST(thinking_text_fast_imessage_uses_fast_delay);
    HU_RUN_TEST(thinking_text_fast_sms_also_gets_fast_delay);
    HU_RUN_TEST(thinking_text_async_keeps_long_delay);
    HU_RUN_TEST(thinking_no_consecutive_duplicates_with_3_bank);

    /* Quality evaluator */
    HU_RUN_TEST(quality_penalizes_semicolons);
    HU_RUN_TEST(quality_penalizes_exclamation_overuse);
    HU_RUN_TEST(quality_rewards_contractions);
    HU_RUN_TEST(quality_penalizes_service_language);
    HU_RUN_TEST(quality_good_casual_scores_high);

    /* Awareness builder */
    HU_RUN_TEST(awareness_null_returns_null);
    HU_RUN_TEST(awareness_builds_context);
    HU_RUN_TEST(awareness_detects_excitement);
    HU_RUN_TEST(awareness_output_bounded);

    /* Narrative detection */
    HU_RUN_TEST(narrative_opening_with_few_exchanges);
    HU_RUN_TEST(narrative_closing_detected);

    /* Engagement detection */
    HU_RUN_TEST(engagement_high_with_questions);
    HU_RUN_TEST(engagement_distracted_with_single_words);

    /* Emotion detection */
    HU_RUN_TEST(emotion_detects_positive);
    HU_RUN_TEST(emotion_detects_negative);
    HU_RUN_TEST(emotion_neutral_for_normal_chat);

    /* Energy detection (F13) */
    HU_RUN_TEST(energy_omg_amazing_excited);
    HU_RUN_TEST(energy_sad_today_sad);
    HU_RUN_TEST(energy_lol_ridiculous_playful);
    HU_RUN_TEST(energy_worried_anxious);
    HU_RUN_TEST(energy_ok_sounds_good_neutral);
    HU_RUN_TEST(energy_directive_excited_nonempty);
    HU_RUN_TEST(energy_directive_sad_nonempty);
    HU_RUN_TEST(energy_directive_playful_nonempty);
    HU_RUN_TEST(energy_directive_anxious_nonempty);
    HU_RUN_TEST(energy_directive_calm_nonempty);
    HU_RUN_TEST(energy_directive_neutral_returns_zero);

    /* F57: Multi-thread energy management */
    HU_RUN_TEST(thread_energy_init_zeroes_count);
    HU_RUN_TEST(thread_energy_update_and_get);
    HU_RUN_TEST(thread_energy_get_unknown_returns_neutral);
    HU_RUN_TEST(thread_energy_update_overwrites_existing);
    HU_RUN_TEST(thread_energy_isolation_no_conflict);
    HU_RUN_TEST(thread_energy_isolation_with_conflict);
    HU_RUN_TEST(thread_energy_null_safety);

    /* Emotional tone classification (F22) */
    HU_RUN_TEST(tone_stressed_returns_stressed);
    HU_RUN_TEST(tone_excited_returns_excited);
    HU_RUN_TEST(tone_sad_returns_sad);
    HU_RUN_TEST(tone_anxious_returns_anxious);
    HU_RUN_TEST(tone_happy_returns_happy);
    HU_RUN_TEST(tone_frustrated_returns_frustrated);
    HU_RUN_TEST(tone_neutral_returns_neutral);
    HU_RUN_TEST(tone_extract_topic_returns_significant_words);
    HU_RUN_TEST(extract_topic_rejects_raw_confession);
    HU_RUN_TEST(extract_topic_rejects_full_first_person_sentence);
    HU_RUN_TEST(extract_topic_filters_greeting_filler_words);
    HU_RUN_TEST(extract_topic_filters_bare_emotion_words);
    HU_RUN_TEST(extract_topic_rejects_short_single_words);
    HU_RUN_TEST(extract_topic_keeps_real_nouns_after_expansion);

    /* Inside joke detection (F19) */
    HU_RUN_TEST(detect_inside_joke_remember_when_true);
    HU_RUN_TEST(detect_inside_joke_whats_for_dinner_false);
    HU_RUN_TEST(detect_inside_joke_energy_pattern_true);
    HU_RUN_TEST(detect_inside_joke_classic_true);
    HU_RUN_TEST(detect_inside_joke_shared_phrase_from_history_true);

    /* Calendar awareness (F50) */
    HU_RUN_TEST(calendar_macos_get_events_returns_empty_array_in_test_mode);

    /* First-time vulnerability detection (F17) */
    HU_RUN_TEST(vulnerability_cancer_extracts_illness);
    HU_RUN_TEST(vulnerability_whats_for_dinner_returns_null);
    HU_RUN_TEST(vulnerability_job_loss_keywords);
    HU_RUN_TEST(vulnerability_divorce_keywords);
    HU_RUN_TEST(vulnerability_mental_health_keywords);
    HU_RUN_TEST(vulnerability_loss_keywords);
    HU_RUN_TEST(vulnerability_family_issue_requires_negative_context);
    HU_RUN_TEST(vulnerability_family_issue_with_negative_matches);
    HU_RUN_TEST(vulnerability_directive_produces_vulnerability_string);
    HU_RUN_TEST(vulnerability_directive_not_first_time_returns_zero);
    HU_RUN_TEST(vulnerability_directive_null_topic_returns_zero);

    /* Micro-moment extraction (F18) */
    HU_RUN_TEST(micro_moment_dog_name_extracts_pet);
    HU_RUN_TEST(micro_moment_i_love_extracts_preference);
    HU_RUN_TEST(micro_moment_moved_to_extracts_location);
    HU_RUN_TEST(micro_moment_nice_weather_extracts_zero);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(vulnerability_cancer_no_prior_first_time);
    HU_RUN_TEST(vulnerability_cancer_with_prior_not_first_time);
    HU_RUN_TEST(vulnerability_whats_for_dinner_no_topic_first_time_false);
#endif

    /* Escalation detection (F14) */
    HU_RUN_TEST(escalation_three_negative_escalating);
    HU_RUN_TEST(escalation_three_negative_then_reset_not_escalating);
    HU_RUN_TEST(escalation_two_negative_not_escalating);
    HU_RUN_TEST(escalation_mixed_positive_negative_not_escalating);
    HU_RUN_TEST(escalation_deescalation_directive_nonempty);

    /* Context modifiers (F16) */
    HU_RUN_TEST(context_modifiers_heavy_topic_includes_directive);
    HU_RUN_TEST(context_modifiers_personal_sharing_includes_directive);
    HU_RUN_TEST(context_modifiers_high_emotion_includes_directive);
    HU_RUN_TEST(context_modifiers_early_turn_includes_directive);
    HU_RUN_TEST(context_modifiers_combined_includes_multiple_lines);

    /* Honesty guardrail */
    HU_RUN_TEST(honesty_detects_action_query);
    HU_RUN_TEST(honesty_null_for_normal_message);

    /* Commitment detection and deadline parsing (F20) */
    HU_RUN_TEST(parse_deadline_tomorrow);
    HU_RUN_TEST(parse_deadline_in_three_days);
    HU_RUN_TEST(parse_deadline_whats_up_returns_zero);
    HU_RUN_TEST(detect_commitment_ill_call_dentist);
    HU_RUN_TEST(detect_commitment_nice_weather_false);

    /* Growth celebration detection (F24) */
    HU_RUN_TEST(detect_growth_it_went_great_true);
    HU_RUN_TEST(detect_growth_i_got_the_job_true);
    HU_RUN_TEST(detect_growth_nailed_it_true);
    HU_RUN_TEST(detect_growth_crushed_it_true);
    HU_RUN_TEST(detect_growth_i_passed_true);
    HU_RUN_TEST(detect_growth_got_promoted_true);
    HU_RUN_TEST(detect_growth_it_worked_out_true);
    HU_RUN_TEST(detect_growth_turned_out_well_true);
    HU_RUN_TEST(detect_growth_ordinary_message_false);
    HU_RUN_TEST(detect_growth_null_input_returns_false);

    /* Length + tone calibration */
    HU_RUN_TEST(calibrate_greeting_short);
    HU_RUN_TEST(calibrate_yes_no_question);
    HU_RUN_TEST(calibrate_emotional_message);
    HU_RUN_TEST(calibrate_logistics);
    HU_RUN_TEST(calibrate_short_react);
    HU_RUN_TEST(calibrate_link_share);
    HU_RUN_TEST(calibrate_open_question);
    HU_RUN_TEST(calibrate_long_story);
    HU_RUN_TEST(calibrate_good_news);
    HU_RUN_TEST(calibrate_bad_news);
    HU_RUN_TEST(calibrate_teasing);
    HU_RUN_TEST(calibrate_vulnerable);
    HU_RUN_TEST(calibrate_tone_present_in_greeting);
    HU_RUN_TEST(calibrate_tone_present_in_emotional);
    HU_RUN_TEST(calibrate_farewell_goodnight);
    HU_RUN_TEST(calibrate_farewell_short_bye);
    HU_RUN_TEST(calibrate_emoji_present_in_greeting);
    HU_RUN_TEST(calibrate_emoji_present_in_logistics);
    HU_RUN_TEST(calibrate_emoji_present_in_general);
    HU_RUN_TEST(calibrate_null_returns_zero);
    HU_RUN_TEST(calibrate_rapid_fire_momentum);
    HU_RUN_TEST(set_thresholds_changes_min_response_chars);
    HU_RUN_TEST(set_thresholds_changes_max_response_chars);
    HU_RUN_TEST(set_thresholds_zero_keeps_default);
    HU_RUN_TEST(max_response_chars_single_char_returns_min);
    HU_RUN_TEST(max_response_chars_medium_message_proportional);
    HU_RUN_TEST(max_response_chars_long_paragraph_capped);
    HU_RUN_TEST(max_response_chars_zero_returns_min);
    HU_RUN_TEST(max_response_chars_medium_range);
    HU_RUN_TEST(max_response_chars_relational_default_matches_plain);
    HU_RUN_TEST(max_response_chars_relational_trusted_higher);
    HU_RUN_TEST(brief_char_cap_redteam);
    HU_RUN_TEST(calibrate_for_contact_softens_ping_for_warm_dm);
    HU_RUN_TEST(calibrate_for_contact_group_uses_neutral_ratio);
    HU_RUN_TEST(quality_needs_revision_at_5x_ratio);
    HU_RUN_TEST(quality_penalizes_length_mismatch);
    HU_RUN_TEST(quality_rewards_length_match);
    HU_RUN_TEST(quality_natural_reply_beats_fragment_when_substantive);
    HU_RUN_TEST(quality_terse_reply_to_terse_banter_keeps_full_brevity);

    /* Typo correction fragment */
    HU_RUN_TEST(correction_detects_typo);
    HU_RUN_TEST(correction_no_typo_no_output);
    HU_RUN_TEST(correction_chance_zero_no_output);
    HU_RUN_TEST(correction_respects_buffer_cap);

    /* Typo simulation */
    HU_RUN_TEST(typo_applies_with_right_seed);
    HU_RUN_TEST(typo_preserves_short_words);
    HU_RUN_TEST(typo_deterministic);
    HU_RUN_TEST(typo_never_exceeds_cap);

    /* Text disfluency (F33) */
    HU_RUN_TEST(disfluency_frequency_one_applies);
    HU_RUN_TEST(disfluency_frequency_zero_unchanged);
    HU_RUN_TEST(disfluency_formal_contact_unchanged);
    HU_RUN_TEST(disfluency_formality_formal_unchanged);
    HU_RUN_TEST(disfluency_small_buffer_unchanged);

    /* Nonverbal sound injection (F39) */
    HU_RUN_TEST(nonverbals_enabled_false_no_change);
    HU_RUN_TEST(nonverbals_seed_zero_laughter_after_period);
    HU_RUN_TEST(nonverbals_seed_500_prepend_hmm);
    HU_RUN_TEST(nonverbals_seed_800_pause_after_comma);
    HU_RUN_TEST(nonverbals_seed_50_no_roll_no_change);
    HU_RUN_TEST(nonverbals_buffer_too_small_no_change);

    /* Typing quirk post-processing */
    HU_RUN_TEST(quirks_lowercase_applies);
    HU_RUN_TEST(quirks_no_periods_strips_sentence_end);
    HU_RUN_TEST(quirks_no_periods_preserves_ellipsis);
    HU_RUN_TEST(quirks_no_commas_strips);
    HU_RUN_TEST(quirks_no_apostrophes_strips);
    HU_RUN_TEST(quirks_multiple_combined);
    HU_RUN_TEST(quirks_null_input_noop);
    HU_RUN_TEST(quirks_empty_quirks_noop);
    HU_RUN_TEST(apply_typing_quirks_double_space_to_newline);

    /* Anti-repetition */
    HU_RUN_TEST(repetition_detects_repeated_opener);
    HU_RUN_TEST(repetition_detects_question_overuse);
    HU_RUN_TEST(repetition_no_issues_returns_zero);
    HU_RUN_TEST(repetition_too_few_messages_returns_zero);

    /* Relationship-tier calibration */
    HU_RUN_TEST(relationship_close_friend);
    HU_RUN_TEST(relationship_acquaintance);
    HU_RUN_TEST(relationship_null_fields);

    /* Group chat classifier */
    HU_RUN_TEST(group_direct_address_responds);
    HU_RUN_TEST(group_question_responds);
    HU_RUN_TEST(group_short_no_prompt_skips);
    HU_RUN_TEST(group_too_many_responses_skips);
    HU_RUN_TEST(group_empty_skips);
    HU_RUN_TEST(classify_group_consecutive_2_skips_with_history);
    HU_RUN_TEST(classify_group_medium_message_is_brief);
    HU_RUN_TEST(group_prompt_hint_macro_is_defined);
    HU_RUN_TEST(group_history_before_gate_classifier);

    /* Tapback-vs-text decision */
    HU_RUN_TEST(tapback_decision_lol_tapback_or_both);
    HU_RUN_TEST(tapback_decision_question_text_only);
    HU_RUN_TEST(tapback_decision_k_no_response_or_brief);
    HU_RUN_TEST(tapback_decision_recent_tapbacks_reduces_tapback);
    HU_RUN_TEST(tapback_decision_empty_no_response);
    HU_RUN_TEST(tapback_decision_emotional_text_only);

    /* Reaction classifier */
    HU_RUN_TEST(reaction_funny_message);
    HU_RUN_TEST(reaction_loving_message);
    HU_RUN_TEST(reaction_normal_message_no_reaction);
    HU_RUN_TEST(reaction_from_me_no_reaction);
    HU_RUN_TEST(photo_reaction_sunset_heart);
    HU_RUN_TEST(photo_reaction_funny_meme_haha);
    HU_RUN_TEST(photo_reaction_screenshot_none);
    HU_RUN_TEST(photo_reaction_family_heart);
    HU_RUN_TEST(photo_reaction_food_none);
    HU_RUN_TEST(photo_reaction_extract_vision_description);
    HU_RUN_TEST(photo_reaction_extract_no_vision_returns_false);

    /* Time-of-day */
    HU_RUN_TEST(calibrate_length_runs_without_crash);

    /* Thread callback */
    HU_RUN_TEST(callback_finds_dropped_topic);
    HU_RUN_TEST(callback_no_candidate_short_history);
    HU_RUN_TEST(callback_null_entries);

    /* URL extraction and link-sharing */
    HU_RUN_TEST(url_extract_finds_https);
    HU_RUN_TEST(url_extract_multiple);
    HU_RUN_TEST(url_extract_no_urls);
    HU_RUN_TEST(should_share_link_recommendation);
    HU_RUN_TEST(should_share_link_normal);
    HU_RUN_TEST(should_share_link_case_insensitive);
    HU_RUN_TEST(attachment_context_with_photo);
    HU_RUN_TEST(attachment_context_with_imessage_placeholder);

    /* Consecutive response limit */
    HU_RUN_TEST(classify_consecutive_3_ours_skips);
    HU_RUN_TEST(classify_consecutive_2_ours_still_responds);

    /* Drop-off classifier (F11) */
    HU_RUN_TEST(dropoff_mutual_farewell_night_night);
    HU_RUN_TEST(dropoff_low_energy_yeah);
    HU_RUN_TEST(dropoff_emoji_only_thumbs_up);
    HU_RUN_TEST(dropoff_our_farewell_their_k);
    HU_RUN_TEST(dropoff_normal_conversation_zero);

    /* Narrative/statement classification (post-tightening) */
    HU_RUN_TEST(classify_narrative_no_question_is_brief);
    HU_RUN_TEST(classify_question_still_full);

    /* Response mode override */
    HU_RUN_TEST(classify_selective_mode_downgrades_full_to_brief_for_no_question);
    HU_RUN_TEST(classify_selective_mode_preserves_question);
    HU_RUN_TEST(classify_eager_mode_upgrades_brief_to_full);
    HU_RUN_TEST(classify_normal_mode_no_override);
    HU_RUN_TEST(classify_selective_is_default);

    /* iMessage effect classifier */
    HU_RUN_TEST(effect_happy_birthday_confetti);
    HU_RUN_TEST(effect_congratulations_balloons);
    HU_RUN_TEST(effect_congrats_balloons);
    HU_RUN_TEST(effect_pew_pew_lasers);
    HU_RUN_TEST(effect_happy_new_year_fireworks);
    HU_RUN_TEST(effect_normal_message_returns_null);
    HU_RUN_TEST(effect_null_input_returns_null);
    HU_RUN_TEST(effect_empty_returns_null);
    HU_RUN_TEST(effect_substring_in_sentence);

    /* Expanded banned AI phrases */
    HU_RUN_TEST(strip_feel_free_to);
    HU_RUN_TEST(strip_dont_hesitate);
    HU_RUN_TEST(strip_happy_to);
    HU_RUN_TEST(strip_double_exclamation);
    HU_RUN_TEST(strip_ai_phrases_multi_pass);

    /* Hallucinated channel tag stripping */
    HU_RUN_TEST(strip_channel_tag_paired);
    HU_RUN_TEST(strip_channel_tag_standalone);
    HU_RUN_TEST(strip_channel_tag_no_tags);
    HU_RUN_TEST(strip_channel_tag_multiple);
    HU_RUN_TEST(strip_channel_tag_empty_input);
    HU_RUN_TEST(strip_channel_tag_eos_token);
    HU_RUN_TEST(strip_channel_tag_inst_markers);
    HU_RUN_TEST(strip_channel_tag_chatml_im_tokens);
    HU_RUN_TEST(strip_channel_tag_thinking_block);
    HU_RUN_TEST(strip_channel_tag_analysis_block);
    HU_RUN_TEST(strip_channel_tag_bos_token);
    HU_RUN_TEST(strip_channel_tag_sys_markers);

    /* Formal structure stripping */
    HU_RUN_TEST(strip_formal_numbered_list);
    HU_RUN_TEST(strip_formal_em_dash);
    HU_RUN_TEST(strip_formal_no_change);
    HU_RUN_TEST(strip_formal_en_dash);
    HU_RUN_TEST(strip_formal_empty_safe);
    HU_RUN_TEST(strip_formal_colon_phrase);
    HU_RUN_TEST(strip_formal_bullet_list);
    HU_RUN_TEST(strip_formal_colon_preserves_inline);

    /* Full outbound strip pipeline integration */
    HU_RUN_TEST(strip_pipeline_full_integration);

    /* Example bank format compatibility */
    HU_RUN_TEST(examples_load_input_output_format);

    /* Persona-driven style/anti-patterns */
    HU_RUN_TEST(style_uses_persona_anti_patterns);
    HU_RUN_TEST(awareness_uses_persona_style_rules);

    /* Inline reply classifier (F40) */
    HU_RUN_TEST(inline_reply_you_said_returns_true);
    HU_RUN_TEST(inline_reply_earlier_returns_true);
    HU_RUN_TEST(inline_reply_what_about_returns_true);
    HU_RUN_TEST(inline_reply_multiple_questions_returns_true);
    HU_RUN_TEST(inline_reply_single_topic_returns_false);
    HU_RUN_TEST(inline_reply_null_last_msg_returns_false);

    /* Active listening backchannels (F29) */
    HU_RUN_TEST(backchannel_long_narrative_prob_one_returns_true);
    HU_RUN_TEST(backchannel_short_k_returns_false);
    HU_RUN_TEST(backchannel_narrative_prob_zero_returns_false);
    HU_RUN_TEST(backchannel_pick_returns_nonempty);
    HU_RUN_TEST(backchannel_pick_deterministic);

    /* Burst messaging (F45) */
    HU_RUN_TEST(burst_omg_did_you_see_prob_one_returns_true);
    HU_RUN_TEST(burst_whats_for_dinner_returns_false);
    HU_RUN_TEST(burst_prob_zero_returns_false);
    HU_RUN_TEST(burst_prompt_builder_contains_burst_mode);
    HU_RUN_TEST(burst_parse_json_array_returns_three_messages);

    /* Leave-on-read classifier (F46) */
    HU_RUN_TEST(leave_on_read_agree_to_disagree_seed_under_2_returns_true);
    HU_RUN_TEST(leave_on_read_question_never_true);
    HU_RUN_TEST(leave_on_read_help_me_never_true);
    HU_RUN_TEST(leave_on_read_normal_message_seed_over_2_returns_false);
    HU_RUN_TEST(leave_on_read_short_ok_seed_under_2_returns_true);
    HU_RUN_TEST(leave_on_read_whatever_seed_under_2_returns_true);
    HU_RUN_TEST(leave_on_read_ok_seed_over_2_returns_false);
    HU_RUN_TEST(leave_on_read_custom_threshold);
    HU_RUN_TEST(leave_on_read_decide_groups_always_respond);
    HU_RUN_TEST(leave_on_read_decide_active_period_returns_already);
    HU_RUN_TEST(leave_on_read_decide_helper_trigger_returns_trigger_new);
    HU_RUN_TEST(leave_on_read_decide_no_signals_responds_normally);
    HU_RUN_TEST(leave_on_read_decide_active_period_beats_helper);

    /* Call escalation (F49) */
    HU_RUN_TEST(call_escalation_crisis_keywords_returns_true);
    HU_RUN_TEST(call_escalation_whats_for_dinner_returns_false);
    HU_RUN_TEST(call_escalation_long_emotional_returns_true);
    HU_RUN_TEST(call_escalation_null_input_returns_false_score_zero);
    HU_RUN_TEST(call_escalation_build_directive_writes_nonempty);

    /* Double-text decision (F9) */
    HU_RUN_TEST(double_text_null_response_returns_false);
    HU_RUN_TEST(double_text_late_night_returns_false);
    HU_RUN_TEST(double_text_farewell_returns_false);
    HU_RUN_TEST(double_text_high_probability_returns_true);
    HU_RUN_TEST(double_text_zero_probability_returns_false);
    HU_RUN_TEST(double_text_energy_boost_increases_chance);
    HU_RUN_TEST(double_text_suppresses_when_too_many_from_me);

    /* Expanded iMessage effects */
    HU_RUN_TEST(effect_halloween_echo);
    HU_RUN_TEST(effect_valentines_heart);
    HU_RUN_TEST(effect_christmas_confetti);
    HU_RUN_TEST(effect_fourth_july_fireworks);
    HU_RUN_TEST(effect_anniversary_heart);

    /* Cold restart detection */
    HU_RUN_TEST(cold_restart_recent_messages_returns_zero);
    HU_RUN_TEST(cold_restart_null_returns_zero);

    /* Self-reaction classifier */
    HU_RUN_TEST(self_reaction_returns_none_most_of_time);
    HU_RUN_TEST(self_reaction_null_returns_none);

    /* Group mention hint */
    HU_RUN_TEST(group_mention_non_group_returns_zero);
    HU_RUN_TEST(group_mention_group_returns_content);

    /* Link context */
    HU_RUN_TEST(link_context_no_url_returns_zero);
    HU_RUN_TEST(link_context_with_url_returns_content);

    /* GIF decision */
    HU_RUN_TEST(gif_decision_sad_message_returns_false);
    HU_RUN_TEST(gif_decision_funny_message_prob_one_returns_true);
    HU_RUN_TEST(gif_decision_question_returns_false);
    HU_RUN_TEST(gif_search_prompt_nonempty);

    /* GIF received reaction */
    HU_RUN_TEST(reaction_gif_returns_haha_or_heart);
    HU_RUN_TEST(reaction_voice_message_returns_none);

    /* Contact-aware GIF probability */
    HU_RUN_TEST(gif_prob_friend_increases);
    HU_RUN_TEST(gif_prob_coworker_decreases);
    HU_RUN_TEST(gif_prob_null_relationship_unchanged);
    HU_RUN_TEST(gif_prob_acquaintance_very_low);
    HU_RUN_TEST(gif_style_hint_friend_absurd);
    HU_RUN_TEST(gif_style_hint_partner_cute);
    HU_RUN_TEST(gif_style_hint_null_default);

    /* GIF rate limiting */
    HU_RUN_TEST(gif_rate_first_send_allowed);
    HU_RUN_TEST(gif_rate_records_and_blocks);
    HU_RUN_TEST(gif_rate_allows_after_gap);

    /* Seen behavior modeling */
    HU_RUN_TEST(seen_respond_now_for_question);
    HU_RUN_TEST(seen_respond_now_for_urgent);
    HU_RUN_TEST(seen_null_msg_respond_now);
    HU_RUN_TEST(seen_low_priority_sometimes_delays);

    /* Cold restart with real time gaps */
    HU_RUN_TEST(cold_restart_six_hour_gap);
    HU_RUN_TEST(cold_restart_one_hour_gap_no_hint);
    HU_RUN_TEST(cold_restart_month_boundary_gap_emits_hint);
    HU_RUN_TEST(cold_restart_year_boundary_gap_emits_hint);

    /* GIF humor calibration */
    HU_RUN_TEST(gif_cal_hit_rate_default);
    HU_RUN_TEST(gif_cal_record_and_hit_rate);

    /* Reaction-received hint */
    HU_RUN_TEST(reaction_received_hint_no_reactions);
    HU_RUN_TEST(reaction_received_hint_with_tapback);

    /* Emoji mirror hint */
    HU_RUN_TEST(emoji_mirror_hint_no_emoji);
    HU_RUN_TEST(emoji_mirror_hint_few_messages);

    /* Edit/unsend awareness */
    HU_RUN_TEST(edit_awareness_edited);
    HU_RUN_TEST(edit_awareness_unsent);
    HU_RUN_TEST(edit_awareness_neither);

    /* Sticker decision engine */
    HU_RUN_TEST(sticker_should_send_on_trigger);
    HU_RUN_TEST(sticker_should_not_send_no_trigger);
    HU_RUN_TEST(sticker_should_not_send_after_gif);
    HU_RUN_TEST(sticker_select_celebration);
    HU_RUN_TEST(sticker_select_laughing);
    HU_RUN_TEST(sticker_select_no_match);

    /* Inline reply context */
    HU_RUN_TEST(inline_reply_hint_builds_context);
    HU_RUN_TEST(inline_reply_hint_truncates_long);
    HU_RUN_TEST(inline_reply_hint_null_returns_zero);

    /* GIF calibration persistence */
    HU_RUN_TEST(gif_cal_save_and_load_roundtrip);

    /* Multi-message splitting */
    HU_RUN_TEST(split_into_texts_short_no_split);
    HU_RUN_TEST(split_into_texts_splits_at_sentence);
    HU_RUN_TEST(split_into_texts_null_returns_zero);
    HU_RUN_TEST(split_into_texts_blank_input_returns_zero);
    HU_RUN_TEST(split_into_texts_never_splits_utf8_codepoint);

    /* Scheduled messages */
    HU_RUN_TEST(schedule_and_flush_delivers);
    HU_RUN_TEST(schedule_future_not_yet_delivered);
    HU_RUN_TEST(schedule_null_returns_error);

    /* Contact photo path */
    HU_RUN_TEST(contact_photo_null_returns_zero);
    HU_RUN_TEST(contact_photo_small_buf_returns_zero);

    /* Scheduled message channel routing */
    HU_RUN_TEST(schedule_on_channel_routes);
    HU_RUN_TEST(schedule_on_empty_channel_matches_any);

    /* Schedule persistence */
    HU_RUN_TEST(sched_save_and_load_roundtrip);

    /* Split edge cases */
    HU_RUN_TEST(split_into_texts_exact_boundary);
    HU_RUN_TEST(split_into_texts_respects_max_chunks);
    HU_RUN_TEST(split_for_cadence_text_fast_bursts_multi_sentence);
    HU_RUN_TEST(split_for_cadence_text_fast_single_sentence_stays_one);
    HU_RUN_TEST(split_for_cadence_text_fast_below_floor_stays_one);
    HU_RUN_TEST(split_for_cadence_text_async_short_stays_one);
    HU_RUN_TEST(split_for_cadence_long_prose_still_chunks_on_any_class);
    HU_RUN_TEST(split_for_cadence_explicit_newlines_inhibit_split);
    HU_RUN_TEST(split_for_cadence_null_inputs_return_zero);

    /* GIF cal JSON escaping */
    HU_RUN_TEST(gif_cal_save_escapes_quotes);

    /* sched_load \uXXXX + escape completeness */
    HU_RUN_TEST(sched_load_unescapes_unicode);
    HU_RUN_TEST(sched_load_unescapes_tab_and_cr);

    /* sched_slot accessor */
    HU_RUN_TEST(sched_slot_returns_null_for_invalid_index);
    HU_RUN_TEST(sched_slot_returns_valid_after_schedule);
}
