/* Hurt-signal hand-off (src/daemon/daemon_hurt_handoff.c): a 1:1 message that
 * says the contact is hurt or worried about the owner must not get an
 * auto-reply when the gate is LIVE; the owner is notified instead. The
 * positive cases are the messages from the 2026-09-26 incident. */
#include "human/daemon/hurt_handoff.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static bool hit(const char *s) {
    return hu_hurt_signal_detect(s, strlen(s));
}

static void hurt_detect_fires_on_incident_messages(void) {
    HU_ASSERT_TRUE(hit("U mad at me?"));
    HU_ASSERT_TRUE(hit("Why u being short?"));
    HU_ASSERT_TRUE(hit("See this is why i get scared w u..."));
    HU_ASSERT_TRUE(hit("Why ru texting so weirddd now"));
}

static void hurt_detect_fires_on_common_variants(void) {
    HU_ASSERT_TRUE(hit("are we ok??"));
    HU_ASSERT_TRUE(hit("Did I do something wrong"));
    HU_ASSERT_TRUE(hit("why are you ignoring me"));
    HU_ASSERT_TRUE(hit("you\xE2\x80\x99re being so cold")); /* typographic apostrophe */
    HU_ASSERT_TRUE(hit("ARE YOU MAD"));
    HU_ASSERT_TRUE(hit("why   are  you\tbeing   distant"));
}

static void hurt_detect_ignores_third_party_and_lookalikes(void) {
    HU_ASSERT_FALSE(hit("my boss is mad at me"));
    HU_ASSERT_FALSE(hit("i'm mad at my sister lol"));
    HU_ASSERT_FALSE(hit("why are you so sweet"));
    HU_ASSERT_FALSE(hit("what did you do today"));
    HU_ASSERT_FALSE(hit("short on time today, call later?"));
    HU_ASSERT_FALSE(hit("it's cold outside"));
    HU_ASSERT_FALSE(hit("Have to go to din for my dads bday"));
    HU_ASSERT_FALSE(hit("Heyo"));
    HU_ASSERT_FALSE(hit("you made my day"));
}

static void hurt_detect_null_and_empty_are_false(void) {
    HU_ASSERT_FALSE(hu_hurt_signal_detect(NULL, 5));
    HU_ASSERT_FALSE(hu_hurt_signal_detect("", 0));
}

static void hurt_apply_off_never_holds_or_notifies(void) {
    hu_hurt_handoff_test_reset();
    const char *m = "U mad at me?";
    HU_ASSERT_FALSE(hu_hurt_handoff_apply(HU_GATE_OFF, m, strlen(m), "Lexi"));
    HU_ASSERT_EQ(hu_hurt_handoff_test_notify_count(), 0u);
}

static void hurt_apply_shadow_detects_but_replies_and_does_not_notify(void) {
    hu_hurt_handoff_test_reset();
    const char *m = "U mad at me?";
    HU_ASSERT_FALSE(hu_hurt_handoff_apply(HU_GATE_SHADOW, m, strlen(m), "Lexi"));
    HU_ASSERT_EQ(hu_hurt_handoff_test_notify_count(), 0u);
}

static void hurt_apply_live_holds_reply_and_notifies_owner(void) {
    hu_hurt_handoff_test_reset();
    const char *m = "Why u being short?";
    HU_ASSERT_TRUE(hu_hurt_handoff_apply(HU_GATE_LIVE, m, strlen(m), "Lexi"));
    HU_ASSERT_EQ(hu_hurt_handoff_test_notify_count(), 1u);
    HU_ASSERT_STR_EQ(hu_hurt_handoff_test_last_name(), "Lexi");
}

static void hurt_apply_live_ordinary_message_replies_normally(void) {
    hu_hurt_handoff_test_reset();
    const char *m = "Otw home finally";
    HU_ASSERT_FALSE(hu_hurt_handoff_apply(HU_GATE_LIVE, m, strlen(m), "Lexi"));
    HU_ASSERT_EQ(hu_hurt_handoff_test_notify_count(), 0u);
}

static void hurt_apply_live_unknown_name_still_notifies(void) {
    hu_hurt_handoff_test_reset();
    const char *m = "are we ok";
    HU_ASSERT_TRUE(hu_hurt_handoff_apply(HU_GATE_LIVE, m, strlen(m), NULL));
    HU_ASSERT_STR_EQ(hu_hurt_handoff_test_last_name(), "Someone");
}

static void hurt_mode_defaults_off_and_reads_env(void) {
    const char *prev = getenv("HU_HURT_HANDOFF");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HU_HURT_HANDOFF");
    HU_ASSERT_EQ((int)hu_hurt_handoff_mode(), (int)HU_GATE_OFF);
    setenv("HU_HURT_HANDOFF", "shadow", 1);
    HU_ASSERT_EQ((int)hu_hurt_handoff_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_HURT_HANDOFF", "live", 1);
    HU_ASSERT_EQ((int)hu_hurt_handoff_mode(), (int)HU_GATE_LIVE);
    if (saved) {
        setenv("HU_HURT_HANDOFF", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_HURT_HANDOFF");
    }
}

void run_daemon_hurt_handoff_tests(void) {
    HU_TEST_SUITE("daemon_hurt_handoff");
    HU_RUN_TEST(hurt_detect_fires_on_incident_messages);
    HU_RUN_TEST(hurt_detect_fires_on_common_variants);
    HU_RUN_TEST(hurt_detect_ignores_third_party_and_lookalikes);
    HU_RUN_TEST(hurt_detect_null_and_empty_are_false);
    HU_RUN_TEST(hurt_apply_off_never_holds_or_notifies);
    HU_RUN_TEST(hurt_apply_shadow_detects_but_replies_and_does_not_notify);
    HU_RUN_TEST(hurt_apply_live_holds_reply_and_notifies_owner);
    HU_RUN_TEST(hurt_apply_live_ordinary_message_replies_normally);
    HU_RUN_TEST(hurt_apply_live_unknown_name_still_notifies);
    HU_RUN_TEST(hurt_mode_defaults_off_and_reads_env);
}
