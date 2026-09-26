/* Hurt-signal hand-off. Contract: include/human/daemon/hurt_handoff.h. */
#include "human/core/allocator.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/daemon/hurt_handoff.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_HURT_SCAN_MAX 2048

/* Whole phrases, matched on word boundaries after normalization. Each one
 * addresses the owner ("you"/"u"/"we"/"I ... you"), so third-party trouble
 * ("my boss is mad at me") does not fire. */
static const char *const k_hurt_phrases[] = {
    "u mad at me",
    "you mad at me",
    "are you mad",
    "are u mad",
    "r u mad",
    "ru mad",
    "are you upset",
    "are u upset",
    "r u upset",
    "you upset with me",
    "u upset with me",
    "are you annoyed",
    "are u annoyed",
    "you annoyed with me",
    "u annoyed with me",
    "did i do something",
    "what did i do",
    "did i say something wrong",
    "did i upset you",
    "did i upset u",
    "did i make you mad",
    "did i make u mad",
    "are we ok",
    "are we okay",
    "r we ok",
    "are we good",
    "you ignoring me",
    "u ignoring me",
    "are you ignoring",
    "r u ignoring",
    "stop ignoring me",
    "scared w u",
    "scared with u",
    "scared with you",
    "whats wrong with you",
    "what's wrong with you",
    "what is wrong with you",
    "whats wrong with u",
    "what's wrong with u",
    "one word answers",
    "one word replies",
    "one word texts",
    "short with me",
    "cold with me",
    "distant with me",
};

/* "why are you being X", "why ru texting so X", "you're being X": a
 * second-person lead-in plus one of these descriptors anywhere in the text.
 * "why are you so sweet" has no descriptor and does not fire. */
static const char *const k_hurt_leads[] = {
    "why are you being", "why are u being",     "why u being",        "why ru being",
    "why r u being",     "you're being",        "youre being",        "ur being",
    "you are being",     "why are you so",      "why are u so",       "why u so",
    "why ru so",         "why r u so",          "why are you acting", "why u acting",
    "why ru acting",     "why are you texting", "why u texting",      "why ru texting",
    "why r u texting",
};
static const char *const k_hurt_descriptors[] = {
    "short", "cold",      "weird", "distant", "dry",     "off",       "mean",
    "quiet", "different", "rude",  "blunt",   "strange", "like this",
};

#define HU_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Lowercase, map the typographic apostrophe (U+2019) to ', collapse runs of
 * whitespace to one space, and collapse a letter repeated 3+ times to one
 * ("weirddd" -> "weird", "sooo" -> "so"). Returns the normalized length. */
static size_t hurt_normalize(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    if (len > HU_HURT_SCAN_MAX)
        len = HU_HURT_SCAN_MAX;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xE2 && i + 2 < len && (unsigned char)in[i + 1] == 0x80 &&
            (unsigned char)in[i + 2] == 0x99) {
            out[o++] = '\'';
            i += 2;
            continue;
        }
        if (isspace(c)) {
            if (o > 0 && out[o - 1] != ' ')
                out[o++] = ' ';
            continue;
        }
        char lc = (char)tolower(c);
        if (isalpha(c)) {
            size_t run = 1;
            while (i + run < len && tolower((unsigned char)in[i + run]) == lc)
                run++;
            if (run >= 3) {
                out[o++] = lc;
                i += run - 1;
                continue;
            }
        }
        out[o++] = lc;
    }
    out[o] = '\0';
    return o;
}

bool hu_hurt_signal_detect(const char *text, size_t len) {
    if (!text || len == 0)
        return false;
    char norm[HU_HURT_SCAN_MAX + 1];
    size_t n = hurt_normalize(text, len, norm, sizeof(norm));
    for (size_t i = 0; i < HU_ARRAY_LEN(k_hurt_phrases); i++) {
        if (hu_str_contains_word_ci_n(norm, n, k_hurt_phrases[i]))
            return true;
    }
    bool lead = false;
    for (size_t i = 0; i < HU_ARRAY_LEN(k_hurt_leads) && !lead; i++)
        lead = hu_str_contains_word_ci_n(norm, n, k_hurt_leads[i]);
    if (!lead)
        return false;
    for (size_t i = 0; i < HU_ARRAY_LEN(k_hurt_descriptors); i++) {
        if (hu_str_contains_word_ci_n(norm, n, k_hurt_descriptors[i]))
            return true;
    }
    return false;
}

hu_gate_mode_t hu_hurt_handoff_mode(void) {
    return hu_gate_mode_from_env("HU_HURT_HANDOFF", HU_GATE_OFF);
}

#ifdef HU_IS_TEST
static unsigned g_test_notify_count;
static char g_test_last_name[64];

unsigned hu_hurt_handoff_test_notify_count(void) {
    return g_test_notify_count;
}
const char *hu_hurt_handoff_test_last_name(void) {
    return g_test_last_name;
}
void hu_hurt_handoff_test_reset(void) {
    g_test_notify_count = 0;
    g_test_last_name[0] = '\0';
}
#endif

/* Tell the owner, locally, that a reply is his to write. Names the contact,
 * never quotes the message. On macOS: a Notification Center banner via
 * osascript, with the text passed as a run-handler argument so a contact
 * name can never be interpreted as AppleScript. */
static void hurt_notify_owner(const char *contact_name) {
    const char *who = (contact_name && contact_name[0]) ? contact_name : "Someone";
#ifdef HU_IS_TEST
    g_test_notify_count++;
    snprintf(g_test_last_name, sizeof(g_test_last_name), "%s", who);
#elif defined(__APPLE__) && defined(__MACH__)
    char body[256];
    snprintf(body, sizeof(body),
             "%s may be hurt or worried about you. h-uman held its reply - text them yourself.",
             who);
    static const char k_display[] =
        "display notification (item 1 of argv) with title \"h-uman\" sound name \"default\"";
    const char *argv[] = {
        "/usr/bin/osascript", "-e", "on run argv", "-e", k_display, "-e", "end run", body, NULL};
    hu_allocator_t alloc = hu_system_allocator();
    hu_run_result_t r;
    memset(&r, 0, sizeof(r));
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 4096, &r);
    if (err != HU_OK || !r.success)
        hu_log_warn("hurt_handoff", NULL,
                    "owner notification failed (err=%d); the auto-reply was still held", (int)err);
    hu_run_result_free(&alloc, &r);
#else
    hu_log_warn("hurt_handoff", NULL,
                "no owner notification on this platform; the auto-reply was held for %s", who);
#endif
}

bool hu_hurt_handoff_apply(hu_gate_mode_t mode, const char *text, size_t len,
                           const char *contact_name) {
    if (mode == HU_GATE_OFF || !hu_hurt_signal_detect(text, len))
        return false;
    if (mode == HU_GATE_SHADOW) {
        /* Length only: shadow telemetry never carries message text or who. */
        hu_log_info("hurt_handoff", NULL, "shadow: would hand off to owner (msg %zu B)", len);
        return false;
    }
    hu_log_warn("hurt_handoff", NULL,
                "hand-off to owner: auto-reply held on a hurt signal (msg %zu B)", len);
    hurt_notify_owner(contact_name);
    return true;
}
