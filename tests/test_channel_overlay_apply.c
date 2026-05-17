/*
 * Per-Tier-1-channel overlay-apply tests.
 *
 * For each of telegram / discord / slack / imessage:
 *   - create the channel
 *   - bind a persona whose channel-specific overlay specifies behavior
 *     (formality, length cap, emoji policy)
 *   - call the channel's send() and assert the captured last_message
 *     reflects the overlay transformation
 *
 * Plus a regression guard: when persona is NULL (or no matching overlay),
 * the captured text equals the raw input — pre-overlay behavior is
 * preserved (AC-6).
 *
 * These tests touch hu_telegram_set_persona / hu_discord_set_persona /
 * hu_slack_set_persona / hu_imessage_set_persona and the production
 * send-path overlay invocation; the production symbols are real, not
 * reimplemented (see ~/.claude/rules/tests-that-pin-bugs.md and the
 * project-level test-references-production-symbol rule).
 *
 * See docs/plans/2026-05-16-audit-followups/01-persona-overlay-wiring.md
 * for the acceptance criteria (AC-1 through AC-4 plus AC-6).
 */

#include "human/channel.h"
#include "human/channels/discord.h"
#include "human/channels/imessage.h"
#include "human/channels/slack.h"
#include "human/channels/telegram.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_allocator_t alloc_;

static void *t_alloc(void *c, size_t s) {
    (void)c;
    return malloc(s);
}
static void t_free(void *c, void *p, size_t s) {
    (void)c;
    (void)s;
    free(p);
}
static void *t_realloc(void *c, void *p, size_t os, size_t ns) {
    (void)c;
    (void)os;
    return realloc(p, ns);
}

static void setup(void) {
    alloc_.alloc = t_alloc;
    alloc_.free = t_free;
    alloc_.realloc = t_realloc;
    alloc_.ctx = NULL;
}

/* Build a persona pinning one named channel to a formal+no-emoji overlay. */
static void make_formal_persona(hu_persona_t *p, hu_persona_overlay_t *ov,
                                const char *channel_name) {
    memset(p, 0, sizeof(*p));
    memset(ov, 0, sizeof(*ov));
    ov->channel = (char *)channel_name;
    ov->formality = (char *)"formal";
    ov->emoji_usage = (char *)"none";
    ov->avg_length = (char *)"max_chars=40";
    p->overlays = ov;
    p->overlays_count = 1;
}

/* Build a persona pinning one named channel to a casual+emoji overlay. */
static void make_casual_persona(hu_persona_t *p, hu_persona_overlay_t *ov,
                                const char *channel_name) {
    memset(p, 0, sizeof(*p));
    memset(ov, 0, sizeof(*ov));
    ov->channel = (char *)channel_name;
    ov->formality = (char *)"casual";
    ov->emoji_usage = (char *)"high";
    p->overlays = ov;
    p->overlays_count = 1;
}

/* ── Telegram ──────────────────────────────────────────────────────────── */

static void telegram_overlay_formal_capitalizes_and_swaps(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_telegram_create(&alloc_, "fake-token-1234567890", 21, &ch), (int)HU_OK);
    hu_persona_t persona;
    hu_persona_overlay_t ov;
    make_formal_persona(&persona, &ov, "telegram");
    hu_telegram_set_persona(&ch, &persona);

    const char *msg = "hey, gonna grab coffee";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "12345", 5, msg, strlen(msg), NULL, 0), (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_telegram_test_get_last_message(&ch, &got_len);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_CONTAINS(got, "Hello");
    HU_ASSERT_STR_CONTAINS(got, "going to");
    HU_ASSERT_STR_NOT_CONTAINS(got, "hey");
    HU_ASSERT_LE((long)got_len, 40L);

    hu_telegram_destroy(&ch);
}

static void telegram_overlay_null_persona_is_identity(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_telegram_create(&alloc_, "fake-token-1234567890", 21, &ch), (int)HU_OK);
    /* No persona bound. */
    const char *msg = "hey there";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "12345", 5, msg, strlen(msg), NULL, 0), (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_telegram_test_get_last_message(&ch, &got_len);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_EQ(got, "hey there");
    hu_telegram_destroy(&ch);
}

/* ── Discord ───────────────────────────────────────────────────────────── */

static void discord_overlay_strips_emoji_and_truncates(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_discord_create(&alloc_, "fake-discord-tok", 16, &ch), (int)HU_OK);

    hu_persona_t persona;
    hu_persona_overlay_t ov;
    memset(&persona, 0, sizeof(persona));
    memset(&ov, 0, sizeof(ov));
    ov.channel = (char *)"discord";
    ov.emoji_usage = (char *)"none";
    ov.avg_length = (char *)"max_chars=30";
    persona.overlays = &ov;
    persona.overlays_count = 1;
    hu_discord_set_persona(&ch, &persona);

    /* Emoji + a 50-char body. */
    const char *msg = "hello \xF0\x9F\x98\x80 this is a longer message that should be truncated";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "channel-id", 10, msg, strlen(msg), NULL, 0),
                 (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_discord_test_get_last_message(&ch, &got_len);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_LE((long)got_len, 30L);
    for (size_t i = 0; i < got_len; i++) {
        HU_ASSERT((unsigned char)got[i] != 0xF0);
    }
    HU_ASSERT_STR_CONTAINS(got, "hello");

    hu_discord_destroy(&ch);
}

static void discord_overlay_null_persona_is_identity(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_discord_create(&alloc_, "fake-discord-tok", 16, &ch), (int)HU_OK);
    const char *msg = "raw text";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "chan", 4, msg, strlen(msg), NULL, 0), (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_discord_test_get_last_message(&ch, &got_len);
    HU_ASSERT_STR_EQ(got, "raw text");
    hu_discord_destroy(&ch);
}

/* ── Slack ─────────────────────────────────────────────────────────────── */

static void slack_overlay_formal_strips_emoji_and_swaps(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_slack_create(&alloc_, "xoxb-fake", 9, &ch), (int)HU_OK);

    hu_persona_t persona;
    hu_persona_overlay_t ov;
    make_formal_persona(&persona, &ov, "slack");
    hu_slack_set_persona(&ch, &persona);

    /* Casual + emoji input. */
    const char *msg = "yeah \xF0\x9F\x91\x8D agreed lol";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "C12345", 6, msg, strlen(msg), NULL, 0), (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_slack_test_get_last_message(&ch, &got_len);
    HU_ASSERT_NOT_NULL(got);
    /* "yeah" -> "yes", "lol" -> "", emoji stripped, leading cap. */
    HU_ASSERT_STR_CONTAINS(got, "Yes");
    HU_ASSERT_STR_NOT_CONTAINS(got, "yeah");
    HU_ASSERT_STR_NOT_CONTAINS(got, "lol");
    for (size_t i = 0; i < got_len; i++) {
        HU_ASSERT((unsigned char)got[i] != 0xF0);
    }

    hu_slack_destroy(&ch);
}

static void slack_overlay_null_persona_is_identity(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_slack_create(&alloc_, "xoxb-fake", 9, &ch), (int)HU_OK);
    const char *msg = "yeah lol";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "C12345", 6, msg, strlen(msg), NULL, 0), (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_slack_test_get_last_message(&ch, &got_len);
    HU_ASSERT_STR_EQ(got, "yeah lol");
    hu_slack_destroy(&ch);
}

/* ── iMessage ──────────────────────────────────────────────────────────── */

static void imessage_overlay_casual_lowercases(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_imessage_create(&alloc_, "+15555550100", 12, NULL, 0, &ch), (int)HU_OK);

    hu_persona_t persona;
    hu_persona_overlay_t ov;
    make_casual_persona(&persona, &ov, "imessage");
    hu_imessage_set_persona(&ch, &persona);

    const char *msg = "Hello, going to grab coffee";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "+15555550100", 12, msg, strlen(msg), NULL, 0),
                 (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_imessage_test_get_last_message(&ch, &got_len);
    HU_ASSERT_NOT_NULL(got);
    /* Casual: "Hello" -> "hey", "going to" -> "gonna", lowercase leading. */
    HU_ASSERT_STR_CONTAINS(got, "gonna");
    /* First char must be lower-case. */
    HU_ASSERT(got_len > 0);
    HU_ASSERT(got[0] >= 'a' && got[0] <= 'z');

    hu_imessage_destroy(&ch);
}

static void imessage_overlay_null_persona_is_identity(void) {
    setup();
    hu_channel_t ch = {0};
    HU_ASSERT_EQ((int)hu_imessage_create(&alloc_, "+15555550100", 12, NULL, 0, &ch), (int)HU_OK);
    const char *msg = "Hello world";
    HU_ASSERT_EQ((int)ch.vtable->send(ch.ctx, "+15555550100", 12, msg, strlen(msg), NULL, 0),
                 (int)HU_OK);
    size_t got_len = 0;
    const char *got = hu_imessage_test_get_last_message(&ch, &got_len);
    HU_ASSERT_STR_EQ(got, "Hello world");
    hu_imessage_destroy(&ch);
}

/* ── Cross-channel divergence: same agent reply, four channels, three
 *    distinct rendered outputs (slack-formal, imessage-casual, discord-
 *    truncated, telegram-identity). This pins the M1-thesis behavior the
 *    whole spec exists for.                                                 */

static void same_input_yields_different_text_per_channel(void) {
    setup();
    const char *raw = "hey, gonna grab coffee \xF0\x9F\x98\x80 talk later";

    /* Slack: formal, no emoji. */
    hu_channel_t s = {0};
    HU_ASSERT_EQ((int)hu_slack_create(&alloc_, "xoxb-x", 6, &s), (int)HU_OK);
    hu_persona_t sp;
    hu_persona_overlay_t so;
    make_formal_persona(&sp, &so, "slack");
    hu_slack_set_persona(&s, &sp);
    HU_ASSERT_EQ((int)s.vtable->send(s.ctx, "C", 1, raw, strlen(raw), NULL, 0), (int)HU_OK);
    size_t s_len = 0;
    const char *s_out = hu_slack_test_get_last_message(&s, &s_len);

    /* iMessage: casual, leaves emoji. */
    hu_channel_t i = {0};
    HU_ASSERT_EQ((int)hu_imessage_create(&alloc_, "+1", 2, NULL, 0, &i), (int)HU_OK);
    hu_persona_t ip;
    hu_persona_overlay_t io;
    make_casual_persona(&ip, &io, "imessage");
    hu_imessage_set_persona(&i, &ip);
    HU_ASSERT_EQ((int)i.vtable->send(i.ctx, "+1", 2, raw, strlen(raw), NULL, 0), (int)HU_OK);
    size_t i_len = 0;
    const char *i_out = hu_imessage_test_get_last_message(&i, &i_len);

    /* Telegram: no overlay bound (raw pass-through). */
    hu_channel_t t = {0};
    HU_ASSERT_EQ((int)hu_telegram_create(&alloc_, "fake-token-1234567890", 21, &t), (int)HU_OK);
    HU_ASSERT_EQ((int)t.vtable->send(t.ctx, "1", 1, raw, strlen(raw), NULL, 0), (int)HU_OK);
    size_t t_len = 0;
    const char *t_out = hu_telegram_test_get_last_message(&t, &t_len);

    HU_ASSERT_NOT_NULL(s_out);
    HU_ASSERT_NOT_NULL(i_out);
    HU_ASSERT_NOT_NULL(t_out);
    /* Slack and iMessage MUST differ (formal vs casual). */
    HU_ASSERT(strcmp(s_out, i_out) != 0);
    /* Slack MUST differ from raw (formality applied). */
    HU_ASSERT(strcmp(s_out, raw) != 0);
    /* Telegram MUST equal raw (no overlay). */
    HU_ASSERT_STR_EQ(t_out, raw);

    hu_slack_destroy(&s);
    hu_imessage_destroy(&i);
    hu_telegram_destroy(&t);
}

void run_channel_overlay_apply_tests(void);
void run_channel_overlay_apply_tests(void) {
    HU_TEST_SUITE("channel_overlay_apply");
    HU_RUN_TEST(telegram_overlay_formal_capitalizes_and_swaps);
    HU_RUN_TEST(telegram_overlay_null_persona_is_identity);
    HU_RUN_TEST(discord_overlay_strips_emoji_and_truncates);
    HU_RUN_TEST(discord_overlay_null_persona_is_identity);
    HU_RUN_TEST(slack_overlay_formal_strips_emoji_and_swaps);
    HU_RUN_TEST(slack_overlay_null_persona_is_identity);
    HU_RUN_TEST(imessage_overlay_casual_lowercases);
    HU_RUN_TEST(imessage_overlay_null_persona_is_identity);
    HU_RUN_TEST(same_input_yields_different_text_per_channel);
}
