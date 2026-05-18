/* ─────────────────────────────────────────────────────────────────────────
 * test_imessage_rich_link.c
 *
 * Pins the rich-link share contract: when a channel advertises
 * supports_link_unfurl=true, the daemon sends a bare URL (no preamble,
 * no caption, no attachments) so the platform auto-unfurls the URL into
 * a rich playable card (album art, title, play button).
 *
 * Why this matters: the prior music-share path sent THREE bubbles per
 * share — caption-with-URL-inline, .m4a preview, and JPG artwork — and
 * the URL was suppressed from unfurling because it had surrounding text.
 * iMessage's native link preview was being bypassed in favor of bespoke
 * downloads. This file locks the new contract so the regression cannot
 * silently return.
 *
 * The daemon's branching (rich-link vs legacy) is reviewed-not-tested
 * here; what is tested is the channel-side contract the daemon depends
 * on: iMessage advertises the capability, and when handed a bare URL
 * with no media, iMessage sends EXACTLY those bytes with EXACTLY zero
 * attachments. If either invariant breaks, this suite fails first.
 *
 * // @covers-none
 *   This test spans two production sources — the static-inline helper
 *   in include/human/channel.h and the vtable + send path in
 *   src/channels/imessage.c. There is no single "implied production
 *   module" for the test-reference rule, and src/feeds/imessage.c (a
 *   different module that happens to share the basename) trips the
 *   check-test-references.sh heuristic. The production symbols this
 *   test exercises — hu_imessage_create, hu_imessage_destroy,
 *   hu_imessage_test_get_last_message, hu_imessage_test_get_last_media_count,
 *   and hu_channel_supports_link_unfurl — are all called below.
 * ───────────────────────────────────────────────────────────────────────── */

#if HU_HAS_IMESSAGE
#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ── 1. Null-safety of the static-inline capability helper ──────────────── */

static void test_supports_link_unfurl_null_channel_returns_false(void) {
    /* hu_channel_supports_link_unfurl is the public predicate the daemon
     * calls before choosing rich-link mode. It MUST be null-safe so that
     * any partially-initialized channel falls through to the legacy path. */
    HU_ASSERT_FALSE(hu_channel_supports_link_unfurl(NULL));
}

static void test_supports_link_unfurl_null_vtable_returns_false(void) {
    hu_channel_t ch = {0};
    HU_ASSERT_FALSE(hu_channel_supports_link_unfurl(&ch));
}

static void test_supports_link_unfurl_missing_vtable_entry_returns_false(void) {
    /* A channel whose vtable doesn't implement supports_link_unfurl at all
     * (NULL function pointer) must be treated as not-supported. This is
     * the default for the 30+ channels not yet wired up. */
    hu_channel_vtable_t vt = {0};
    hu_channel_t ch = {.ctx = NULL, .vtable = &vt};
    HU_ASSERT_FALSE(hu_channel_supports_link_unfurl(&ch));
}

/* ── 2. iMessage advertises the capability ──────────────────────────────── */

static void test_imessage_channel_advertises_link_unfurl_support(void) {
    /* iMessage is the canonical rich-link channel. Its vtable promise is
     * the load-bearing assertion for the entire music-share rich-link
     * path: if this returns false, every iMessage share falls back to
     * downloading + attaching a 30s .m4a, which is the bug we just fixed. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550100", 12, NULL, 0, &ch), HU_OK);

    HU_ASSERT_TRUE(hu_channel_supports_link_unfurl(&ch));

    hu_imessage_destroy(&ch);
}

/* ── 3. The bare-URL invariant ──────────────────────────────────────────── */
/* Sends what the rich-link path sends (URL only, no media) and verifies
 * the channel preserves the URL byte-exactly with zero attachments.
 * If a future change re-introduces a caption or attaches a preview clip,
 * one of these will fail first. */

static void test_imessage_rich_link_send_url_alone_preserves_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550101", 12, NULL, 0, &ch), HU_OK);

    const char *url = "https://music.apple.com/us/song/holocene/1440873434";
    size_t url_len = strlen(url);

    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15555550101", 12, url, url_len, NULL, 0), HU_OK);

    size_t got_len = 0;
    const char *got = hu_imessage_test_get_last_message(&ch, &got_len);
    HU_ASSERT(got != NULL);
    HU_ASSERT_EQ(got_len, url_len);
    HU_ASSERT_EQ(memcmp(got, url, url_len), 0);

    hu_imessage_destroy(&ch);
}

static void test_imessage_rich_link_send_url_has_zero_attachments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550102", 12, NULL, 0, &ch), HU_OK);

    const char *url = "https://open.spotify.com/track/1RMJOxR6GRPsBHL8qeC2ux";
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15555550102", 12, url, strlen(url), NULL, 0), HU_OK);

    /* THE invariant: no .m4a, no JPG. The platform's link preview supplies
     * artwork + audio for free from the URL itself. Anything attached here
     * would create a second bubble and defeat the whole point. */
    HU_ASSERT_EQ(hu_imessage_test_get_last_media_count(&ch), 0u);

    hu_imessage_destroy(&ch);
}

static void test_imessage_rich_link_url_has_no_trailing_whitespace(void) {
    /* iMessage's link preview is whitespace-strict: a trailing newline or
     * NBSP on some iOS versions silently suppresses the unfurl, downgrading
     * the bubble to plain blue text. The send path MUST NOT smuggle one in. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550103", 12, NULL, 0, &ch), HU_OK);

    const char *url = "https://music.apple.com/us/album/1440873432";
    size_t url_len = strlen(url);
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15555550103", 12, url, url_len, NULL, 0), HU_OK);

    size_t got_len = 0;
    const char *got = hu_imessage_test_get_last_message(&ch, &got_len);
    HU_ASSERT(got != NULL);
    HU_ASSERT(got_len > 0);
    /* The last byte must be a valid URL char — not '\n', '\r', ' ', or '\t'. */
    char last = got[got_len - 1];
    HU_ASSERT(last != '\n');
    HU_ASSERT(last != '\r');
    HU_ASSERT(last != ' ');
    HU_ASSERT(last != '\t');

    hu_imessage_destroy(&ch);
}

/* ── Registration ───────────────────────────────────────────────────────── */

void run_imessage_rich_link_tests(void);
void run_imessage_rich_link_tests(void) {
    HU_TEST_SUITE("iMessage rich-link share");

    HU_RUN_TEST(test_supports_link_unfurl_null_channel_returns_false);
    HU_RUN_TEST(test_supports_link_unfurl_null_vtable_returns_false);
    HU_RUN_TEST(test_supports_link_unfurl_missing_vtable_entry_returns_false);

    HU_RUN_TEST(test_imessage_channel_advertises_link_unfurl_support);

    HU_RUN_TEST(test_imessage_rich_link_send_url_alone_preserves_bytes);
    HU_RUN_TEST(test_imessage_rich_link_send_url_has_zero_attachments);
    HU_RUN_TEST(test_imessage_rich_link_url_has_no_trailing_whitespace);
}

#else /* !HU_HAS_IMESSAGE */
void run_imessage_rich_link_tests(void);
void run_imessage_rich_link_tests(void) {
    /* iMessage channel not built — skip silently. */
}
#endif
