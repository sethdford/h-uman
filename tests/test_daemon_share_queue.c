/* Saved shares — "saw this and thought of you" (spec 2026-09-28, Phase 5.4).
 * Seth texts a link to his own number with "save" or "for <name>"; the daemon
 * keeps it and offers it to the director when that person writes. */
#include "human/channel.h"
#include "human/daemon/message_router.h"
#include "human/daemon/share_queue.h"
#include "human/persona.h"
#include "test_framework.h"
#include "test_tmpdir.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool cap(const char *s, hu_share_capture_t *c) {
    return hu_share_capture_parse(s, strlen(s), c);
}

static void test_share_capture_needs_an_explicit_save(void) {
    hu_share_capture_t c;
    HU_ASSERT_TRUE(cap("save https://www.tiktok.com/@x/video/123", &c));
    HU_ASSERT_STR_EQ(c.url, "https://www.tiktok.com/@x/video/123");
    HU_ASSERT_STR_EQ(c.for_name, "");
    HU_ASSERT_TRUE(cap("for Mindy https://youtube.com/shorts/abc lol she'll love this", &c));
    HU_ASSERT_STR_EQ(c.url, "https://youtube.com/shorts/abc");
    HU_ASSERT_STR_EQ(c.for_name, "Mindy");
    HU_ASSERT_TRUE(cap("For mom: https://music.apple.com/us/song/x/1", &c));
    HU_ASSERT_STR_EQ(c.for_name, "mom");
    /* Seth testing how the daemon reacts to a link is a conversation, not a save */
    HU_ASSERT_FALSE(cap("https://www.tiktok.com/@x/video/123", &c));
    HU_ASSERT_FALSE(cap("check this out https://youtu.be/x", &c));
    HU_ASSERT_FALSE(cap("save me a seat", &c));                               /* no link */
    HU_ASSERT_FALSE(cap("for real https://a.example https://b.example", &c)); /* two links */
}

static hu_contact_profile_t g_contacts[3];
static hu_persona_t g_persona;

static const hu_persona_t *persona(void) {
    memset(g_contacts, 0, sizeof(g_contacts));
    g_contacts[0].contact_id = "+15550000001";
    g_contacts[0].name = "Mindy Ford Gibson";
    g_contacts[0].relationship = "sister";
    g_contacts[1].contact_id = "+15550000002";
    g_contacts[1].name = "Betty Ford";
    g_contacts[1].relationship = "mother";
    g_contacts[2].contact_id = "+15550000009";
    g_contacts[2].name = "Seth Ford";
    g_contacts[2].relationship = "test";
    memset(&g_persona, 0, sizeof(g_persona));
    g_persona.contacts = g_contacts;
    g_persona.contacts_count = 3;
    return &g_persona;
}

static void test_share_owner_and_contact_resolution(void) {
    const hu_persona_t *p = persona();
    HU_ASSERT_TRUE(hu_share_is_owner(p, "+15550000009", 12));
    HU_ASSERT_FALSE(hu_share_is_owner(p, "+15550000001", 12));
    char h[64];
    HU_ASSERT_TRUE(hu_share_resolve_contact(p, "mindy", h, sizeof(h)));
    HU_ASSERT_STR_EQ(h, "+15550000001");
    HU_ASSERT_TRUE(hu_share_resolve_contact(p, "Mom", h, sizeof(h))); /* by relationship */
    HU_ASSERT_STR_EQ(h, "+15550000002");
    HU_ASSERT_FALSE(hu_share_resolve_contact(p, "Zed", h, sizeof(h)));
    HU_ASSERT_FALSE(hu_share_resolve_contact(p, "ford", h, sizeof(h))); /* ambiguous surname */
}

static void test_share_queue_offers_tagged_first_then_anyone(void) {
    char path[512];
    HU_ASSERT_TRUE(hu_test_tmppath(path, sizeof(path), "share_queue.tsv"));
    unlink(path);
    char url[512];
    HU_ASSERT_FALSE(hu_share_queue_next(path, "+15550000001", url, sizeof(url)));
    HU_ASSERT_EQ(hu_share_queue_add(path, 100, "https://a.example/any", ""), HU_OK);
    HU_ASSERT_EQ(hu_share_queue_add(path, 200, "https://b.example/mindy", "+15550000001"), HU_OK);
    HU_ASSERT_TRUE(hu_share_queue_next(path, "+15550000001", url, sizeof(url)));
    HU_ASSERT_STR_EQ(url, "https://b.example/mindy"); /* hers first */
    HU_ASSERT_TRUE(hu_share_queue_next(path, "+15550000002", url, sizeof(url)));
    HU_ASSERT_STR_EQ(url, "https://a.example/any"); /* never her tagged one */
    HU_ASSERT_EQ(hu_share_queue_mark_sent(path, "https://b.example/mindy"), HU_OK);
    HU_ASSERT_TRUE(hu_share_queue_next(path, "+15550000001", url, sizeof(url)));
    HU_ASSERT_STR_EQ(url, "https://a.example/any");
    HU_ASSERT_EQ(hu_share_queue_mark_sent(path, "https://a.example/any"), HU_OK);
    HU_ASSERT_FALSE(hu_share_queue_next(path, "+15550000001", url, sizeof(url)));
    unlink(path);
}

/* Only Seth's own number files a save, and he gets a short ack; anyone else's
 * "save <link>" is just a message. */
static void test_share_capture_handle_files_and_acks(void) {
    const hu_persona_t *p = persona();
    char path[512], ack[160], url[512];
    HU_ASSERT_TRUE(hu_test_tmppath(path, sizeof(path), "share_capture.tsv"));
    unlink(path);
    const char *m = "for mindy https://www.tiktok.com/@x/video/9";
    HU_ASSERT_TRUE(
        hu_share_capture_handle(p, "+15550000009", 12, m, strlen(m), path, 100, ack, sizeof(ack)));
    HU_ASSERT_STR_CONTAINS(ack, "Mindy");
    HU_ASSERT_TRUE(hu_share_queue_next(path, "+15550000001", url, sizeof(url)));
    HU_ASSERT_STR_EQ(url, "https://www.tiktok.com/@x/video/9");
    const char *z = "for zed https://a.example/z";
    HU_ASSERT_TRUE(
        hu_share_capture_handle(p, "+15550000009", 12, z, strlen(z), path, 101, ack, sizeof(ack)));
    HU_ASSERT_STR_CONTAINS(ack, "anyone"); /* unknown name: kept, untagged */
    HU_ASSERT_FALSE(hu_share_capture_handle(p, "+15550000001", 12, m, strlen(m), path, 102, ack,
                                            sizeof(ack))); /* Mindy is not the owner */
    const char *talk = "https://www.tiktok.com/@x/video/9";
    HU_ASSERT_FALSE(hu_share_capture_handle(p, "+15550000009", 12, talk, strlen(talk), path, 103,
                                            ack, sizeof(ack))); /* a bare link is a test chat */
    unlink(path);
}

static int g_sq_sends;
static char g_sq_last[512];
static hu_error_t sq_send(void *ctx, const char *target, size_t target_len, const char *msg,
                          size_t msg_len, const char *const *media, size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    g_sq_sends++;
    snprintf(g_sq_last, sizeof(g_sq_last), "%.*s", (int)msg_len, msg);
    return HU_OK;
}

/* The saved link goes out as its own bubble (exactly the URL, so iMessage
 * unfurls it) and leaves the queue. */
static void test_share_send_saved_sends_the_link_once(void) {
    char path[512];
    HU_ASSERT_TRUE(hu_test_tmppath(path, sizeof(path), "share_send.tsv"));
    unlink(path);
    HU_ASSERT_EQ(hu_share_queue_add(path, 1, "https://youtube.com/shorts/q", "+15550000001"),
                 HU_OK);
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.send = sq_send;
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.vtable = &vt;
    g_sq_sends = 0;
    HU_ASSERT_TRUE(hu_share_send_saved(&ch, "+15550000001", 12, path));
    HU_ASSERT_EQ(g_sq_sends, 1);
    HU_ASSERT_STR_EQ(g_sq_last, "https://youtube.com/shorts/q");
    HU_ASSERT_FALSE(hu_share_send_saved(&ch, "+15550000001", 12, path)); /* nothing left */
    HU_ASSERT_EQ(g_sq_sends, 1);
    unlink(path);
}

/* rating_drip.py / voice_ab.py text Seth's own number and read his answer back
 * from chat.db, where any reply of ours is a from-me row too: 2026-10-01 the
 * twin answered "[h-uman rating 5-9/48] which sounds more like you?". */
static void test_tool_prompt_from_owner_is_recognised(void) {
    const hu_persona_t *p = persona();
    static const char rating[] = "[h-uman rating 5-9/48] which sounds more like you?\nA: hey";
    static const char voice[] = "[h-uman voice 3/12] which memo sounds like you?";
    HU_ASSERT_TRUE(hu_share_is_tool_prompt(p, "+15550000009", 12, rating, strlen(rating)));
    HU_ASSERT_TRUE(hu_share_is_tool_prompt(p, "+15550000009", 12, voice, strlen(voice)));
    /* the same text from anyone else is a conversation */
    HU_ASSERT_FALSE(hu_share_is_tool_prompt(p, "+15550000001", 12, rating, strlen(rating)));
    /* Seth chatting with the twin, or a #self-test, still gets an answer */
    HU_ASSERT_FALSE(hu_share_is_tool_prompt(p, "+15550000009", 12, "#text hey", 9));
    HU_ASSERT_FALSE(hu_share_is_tool_prompt(p, "+15550000009", 12, "what's [h-uman", 14));
    HU_ASSERT_FALSE(hu_share_is_tool_prompt(p, "+15550000009", 12, "[h-uman", 7));
    HU_ASSERT_FALSE(hu_share_is_tool_prompt(NULL, "+15550000009", 12, rating, strlen(rating)));
}

static void test_batch_withheld_for_a_tool_prompt_only(void) {
    const hu_persona_t *p = persona();
    static const char rating[] = "[h-uman rating 1-4/48] reply with 4 letters";
    HU_ASSERT_TRUE(
        hu_daemon_batch_withheld(p, "+15550000009", 12, rating, strlen(rating), false, NULL));
    HU_ASSERT_TRUE(
        hu_daemon_batch_withheld(p, "+15550000009", 12, rating, strlen(rating), true, NULL));
    HU_ASSERT_FALSE(hu_daemon_batch_withheld(p, "+15550000009", 12, "hey", 3, false, NULL));
    HU_ASSERT_FALSE(
        hu_daemon_batch_withheld(p, "+15550000001", 12, rating, strlen(rating), false, NULL));
}

void run_daemon_share_queue_tests(void) {
    HU_TEST_SUITE("daemon share queue");
    HU_RUN_TEST(test_share_capture_needs_an_explicit_save);
    HU_RUN_TEST(test_share_owner_and_contact_resolution);
    HU_RUN_TEST(test_tool_prompt_from_owner_is_recognised);
    HU_RUN_TEST(test_batch_withheld_for_a_tool_prompt_only);
    HU_RUN_TEST(test_share_queue_offers_tagged_first_then_anyone);
    HU_RUN_TEST(test_share_capture_handle_files_and_acks);
    HU_RUN_TEST(test_share_send_saved_sends_the_link_once);
}
