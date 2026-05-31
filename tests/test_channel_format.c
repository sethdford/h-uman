#include "human/channels/format.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

static void channel_format_imessage_strips_markdown_and_ai_phrases(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "**Bold** As an AI I can help.";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_format_outbound(&alloc, "imessage", 8, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "**") == NULL);
    HU_ASSERT(strstr(out, "As an AI") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_format_slack_mrkdwn_link_and_bold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "See [doc](https://example.com/a) and **bold** here.";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_format_outbound(&alloc, "slack", 5, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "<https://example.com/a|doc>") != NULL);
    HU_ASSERT(strstr(out, "*bold*") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_format_discord_trims_trailing_ws(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "keep\n  \t  ";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_format_outbound(&alloc, "discord", 7, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 4);
    HU_ASSERT(memcmp(out, "keep", 4) == 0);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_format_email_wraps_paragraphs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "Hello **world**.\n\nSecond *line*.";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_format_outbound(&alloc, "email", 5, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "<p>") != NULL);
    HU_ASSERT(strstr(out, "<strong>world</strong>") != NULL);
    HU_ASSERT(strstr(out, "<em>line</em>") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_format_cli_passthrough(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "plain **md**";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_format_outbound(&alloc, "cli", 3, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, strlen(in));
    HU_ASSERT(memcmp(out, in, out_len) == 0);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_format_imessage_strips_blockquote(void) {
    /* The model sometimes emits a Markdown blockquote ("> ...") quoting the
     * message it's replying to. A human texter never types a leading '>' — it
     * is the single loudest AI tell on the outbound wire. format_outbound must
     * strip it. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "> Driving to work- not ignoring u";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_format_outbound(&alloc, "imessage", 8, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* No leading '>' blockquote marker survives. */
    HU_ASSERT(out[0] != '>');
    HU_ASSERT(strstr(out, "Driving to work") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_strip_markdown_strips_line_leading_blockquote(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "> hello\n> world\nplain";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_channel_strip_markdown(&alloc, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hello\nworld\nplain");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_strip_markdown_keeps_inline_gt(void) {
    /* A '>' that is NOT line-leading is real content (comparison, arrow), not a
     * blockquote. It must be preserved. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "5 > 3 and a -> b";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_channel_strip_markdown(&alloc, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "5 > 3 and a -> b");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_strip_markdown_strips_leading_return_glyph(void) {
    /* The model sometimes leaks its own reply-framing glyph (U+21A9 ↩) at the
     * start of the body — a transcript artifact, not something a human types.
     * (The daemon's INTENTIONAL inline-quote is prepended later in the router,
     * downstream of this strip, so it is unaffected.) */
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "\xE2\x86\xA9 driving to work";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_channel_strip_markdown(&alloc, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "driving to work");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_strip_markdown_strips_leading_enter_glyph(void) {
    /* U+21B5 ↵ (the "return" arrow rendered in the screenshot). */
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "\xE2\x86\xB5 what makes u say this";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_channel_strip_markdown(&alloc, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "what makes u say this");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_plaintext_for_split_imessage_strips_markdown_and_closer(void) {
    /* Plaintext channels run the FULL chain (strip_markdown + ai-phrase +
     * assistant-closer) so bubbled replies match the single whole-reply path. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "**bold** Hope this helps!";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_channel_plaintext_for_split(&alloc, "imessage", 8, in, strlen(in), &out,
                                                    &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "**") == NULL);
    HU_ASSERT(strstr(out, "Hope this helps!") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_plaintext_for_split_slack_strips_markdown_no_mrkdwn(void) {
    /* Markup channels (slack→mrkdwn, email→HTML) must NOT be converted to markup
     * before the splitter — only plaintext markdown-strip. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "> quote\n**b** plain";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_channel_plaintext_for_split(&alloc, "slack", 5, in, strlen(in), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, ">") == NULL);   /* blockquote stripped */
    HU_ASSERT(strstr(out, "**") == NULL);  /* emphasis stripped */
    HU_ASSERT(strstr(out, "<") == NULL);   /* NOT converted to slack mrkdwn link/markup */
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void channel_strip_markdown_null_args(void) {
    char *out = NULL;
    size_t out_len = 0;
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_channel_strip_markdown(NULL, "x", 1, &out, &out_len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_channel_strip_markdown(&alloc, "x", 1, NULL, &out_len), HU_ERR_INVALID_ARGUMENT);
}

void run_channel_format_tests(void) {
    HU_TEST_SUITE("channel_format");
    HU_RUN_TEST(channel_format_imessage_strips_markdown_and_ai_phrases);
    HU_RUN_TEST(channel_format_slack_mrkdwn_link_and_bold);
    HU_RUN_TEST(channel_format_discord_trims_trailing_ws);
    HU_RUN_TEST(channel_format_email_wraps_paragraphs);
    HU_RUN_TEST(channel_format_cli_passthrough);
    HU_RUN_TEST(channel_format_imessage_strips_blockquote);
    HU_RUN_TEST(channel_strip_markdown_strips_line_leading_blockquote);
    HU_RUN_TEST(channel_strip_markdown_keeps_inline_gt);
    HU_RUN_TEST(channel_strip_markdown_strips_leading_return_glyph);
    HU_RUN_TEST(channel_strip_markdown_strips_leading_enter_glyph);
    HU_RUN_TEST(channel_plaintext_for_split_imessage_strips_markdown_and_closer);
    HU_RUN_TEST(channel_plaintext_for_split_slack_strips_markdown_no_mrkdwn);
    HU_RUN_TEST(channel_strip_markdown_null_args);
}
