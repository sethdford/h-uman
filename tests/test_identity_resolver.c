#include "human/memory/identity_resolver.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------- canonicalization helpers -------------------- */

static void canonicalize_phone_us_format_keeps_last_10_digits(void) {
    char buf[32];
    /* "+1 (555) 123-4567" → "5551234567" */
    HU_ASSERT_EQ(hu_identity_canonicalize_phone("+1 (555) 123-4567", buf, sizeof(buf)), (size_t)10);
    HU_ASSERT_STR_EQ(buf, "5551234567");
}

static void canonicalize_phone_intl_format_matches_us_format(void) {
    char a[32], b[32];
    hu_identity_canonicalize_phone("+15551234567", a, sizeof(a));
    hu_identity_canonicalize_phone("(555) 123-4567", b, sizeof(b));
    HU_ASSERT_STR_EQ(a, b);
}

static void canonicalize_phone_too_short_returns_zero(void) {
    char buf[32];
    /* 555-1234 is only 7 digits — too short to be a real US number */
    size_t n = hu_identity_canonicalize_phone("555-12", buf, sizeof(buf));
    HU_ASSERT_EQ(n, (size_t)0);
}

static void canonicalize_email_lowercases(void) {
    char buf[64];
    HU_ASSERT_TRUE(hu_identity_canonicalize_email("Alice@Example.COM", buf, sizeof(buf)) > 0);
    HU_ASSERT_STR_EQ(buf, "alice@example.com");
}

static void canonicalize_email_gmail_dots_collapse(void) {
    char a[64], b[64];
    hu_identity_canonicalize_email("alice.smith@gmail.com", a, sizeof(a));
    hu_identity_canonicalize_email("alicesmith@gmail.com", b, sizeof(b));
    HU_ASSERT_STR_EQ(a, b);
    HU_ASSERT_STR_EQ(a, "alicesmith@gmail.com");
}

static void canonicalize_email_non_gmail_preserves_dots(void) {
    char buf[64];
    hu_identity_canonicalize_email("alice.smith@yahoo.com", buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "alice.smith@yahoo.com");
}

static void canonicalize_email_rejects_non_email(void) {
    char buf[64];
    HU_ASSERT_EQ(hu_identity_canonicalize_email("not-an-email", buf, sizeof(buf)), (size_t)0);
}

/* -------------------- resolve: HIGH-confidence merges -------------------- */

static void resolve_two_phone_formats_merges_high(void) {
    const char *handles[] = {"+15551234567", "(555) 123-4567"};
    const char *channels[] = {"imessage", "imessage"};
    const char *names[] = {"Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 2, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)1);
    HU_ASSERT_EQ((int)g.contacts[0].merge_confidence, (int)HU_IDENTITY_CONFIDENCE_HIGH);
    HU_ASSERT_EQ(g.contacts[0].alias_count, (size_t)2);
}

static void resolve_gmail_dot_variants_merges_high(void) {
    const char *handles[] = {"alice.smith@gmail.com", "alicesmith@gmail.com"};
    const char *channels[] = {"email", "email"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, NULL, 2, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)1);
    HU_ASSERT_EQ((int)g.contacts[0].merge_confidence, (int)HU_IDENTITY_CONFIDENCE_HIGH);
}

/* The headline scenario: Alice on iMessage (phone) + email + Slack ID.
 * Phone+email use a SHARED display name "Alice" → LOW bridges Slack in.
 * Result is one contact at LOW (weakest link). */
static void resolve_phone_email_slack_via_name_lowers_to_low(void) {
    const char *handles[] = {"+15551234567", "alice@example.com", "U07ALICE"};
    const char *channels[] = {"imessage", "email", "slack"};
    const char *names[] = {"Alice", "Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 3, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)1);
    /* Phone and email are different strong-key KINDS, so they don't
     * HIGH-merge directly; only the LOW name bridge unites all three.
     * Weakest link is LOW. */
    HU_ASSERT_EQ((int)g.contacts[0].merge_confidence, (int)HU_IDENTITY_CONFIDENCE_LOW);
    HU_ASSERT_EQ(g.contacts[0].alias_count, (size_t)3);
}

/* -------------------- privacy-critical adversarial tests -------------------- */

static void resolve_two_strangers_same_first_name_stay_low(void) {
    /* Two Slack IDs, both "Alice", different people. Best we can say
     * is LOW — must NOT auto-merge for fact unification. */
    const char *handles[] = {"U07ALICE_A", "U07ALICE_B"};
    const char *channels[] = {"slack", "slack"};
    const char *names[] = {"Alice Smith", "Alice Jones"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 2, &g), HU_OK);
    /* They MAY merge (same first-name token "alice") but ONLY at LOW. */
    HU_ASSERT_EQ(g.contact_count, (size_t)1);
    HU_ASSERT_EQ((int)g.contacts[0].merge_confidence, (int)HU_IDENTITY_CONFIDENCE_LOW);
}

static void resolve_same_name_different_phones_do_not_merge(void) {
    /* Two phone numbers, both labeled "Alice". Phones are different
     * strong keys → explicit "different people" signal. Must NOT merge. */
    const char *handles[] = {"+15551234567", "+15559999999"};
    const char *channels[] = {"imessage", "imessage"};
    const char *names[] = {"Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 2, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)2);
}

static void resolve_same_name_different_gmail_addresses_do_not_merge(void) {
    /* Two gmail addresses, both "alice" prefix but different
     * canonical forms (alice@ vs alicesmith@). Both labeled "Alice".
     * Same first-name token but distinct strong keys of the same kind
     * → DO NOT merge. */
    const char *handles[] = {"alice@gmail.com", "alicesmith@gmail.com"};
    const char *channels[] = {"email", "email"};
    const char *names[] = {"Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 2, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)2);
}

static void resolve_similar_email_and_slack_name_does_not_high_merge(void) {
    /* alice@a.com (email) + a Slack user with display name "Alice Bob".
     * First-name token matches → LOW only. Must NOT HIGH-merge. */
    const char *handles[] = {"alice@a.com", "U_ALICEBOB"};
    const char *channels[] = {"email", "slack"};
    const char *names[] = {NULL, "Alice Bob"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 2, &g), HU_OK);
    /* Email has strong key + token "alice" (extracted from local
     * part? NO — name_token is built only from display_names. So
     * email here has no name_token, slack has "alice". No bridge.
     * They stay as TWO contacts. This is the conservative outcome. */
    HU_ASSERT_EQ(g.contact_count, (size_t)2);
}

/* -------------------- isolation and overflow -------------------- */

static void resolve_lone_phone_stays_alone(void) {
    const char *handles[] = {"+15551234567"};
    const char *channels[] = {"imessage"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, NULL, 1, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)1);
    HU_ASSERT_EQ(g.contacts[0].alias_count, (size_t)1);
    /* Single-alias contact → no merge happened → NONE. */
    HU_ASSERT_EQ((int)g.contacts[0].merge_confidence, (int)HU_IDENTITY_CONFIDENCE_NONE);
}

static void resolve_overflow_clamps_to_max_contacts(void) {
    /* 257 handles. First 256 fit, last is dropped silently. No crash. */
    static const size_t N = HU_IDENTITY_MAX_CONTACTS + 1;
    const char **handles = (const char **)calloc(N, sizeof(char *));
    const char **channels = (const char **)calloc(N, sizeof(char *));
    HU_ASSERT_TRUE(handles && channels);
    char (*hstor)[32] = calloc(N, sizeof(*hstor));
    HU_ASSERT_TRUE(hstor != NULL);
    for (size_t i = 0; i < N; i++) {
        snprintf(hstor[i], sizeof(hstor[i]), "user_%zu", i);
        handles[i] = hstor[i];
        channels[i] = "slack";
    }
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, NULL, N, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)HU_IDENTITY_MAX_CONTACTS);
    free(hstor);
    free((void *)handles);
    free((void *)channels);
}

static void resolve_empty_input_returns_empty_graph(void) {
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(NULL, NULL, NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)0);
}

static void resolve_null_out_returns_error(void) {
    const char *h[] = {"x"};
    const char *c[] = {"slack"};
    HU_ASSERT_EQ(hu_identity_resolve(h, c, NULL, 1, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* -------------------- lookup -------------------- */

static void lookup_returns_canonical_contact(void) {
    const char *handles[] = {"+15551234567", "(555) 123-4567"};
    const char *channels[] = {"imessage", "imessage"};
    const char *names[] = {"Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 2, &g), HU_OK);

    const hu_identity_contact_t *c = hu_identity_lookup(&g, "+15551234567");
    HU_ASSERT_TRUE(c != NULL);
    const hu_identity_contact_t *c2 = hu_identity_lookup(&g, "(555) 123-4567");
    HU_ASSERT_TRUE(c2 != NULL);
    /* Same handle's contact pointer. */
    HU_ASSERT_TRUE(c == c2);
    HU_ASSERT_STR_EQ(c->canonical_name, "Alice");
}

static void lookup_unknown_handle_returns_null(void) {
    hu_identity_graph_t g;
    memset(&g, 0, sizeof(g));
    HU_ASSERT_TRUE(hu_identity_lookup(&g, "unknown") == NULL);
}

/* -------------------- persistence -------------------- */

static void save_load_roundtrip_preserves_graph(void) {
    const char *handles[] = {"+15551234567", "alice@gmail.com", "U07ALICE"};
    const char *channels[] = {"imessage", "email", "slack"};
    const char *names[] = {"Alice", "Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 3, &g), HU_OK);

    char path[] = "/tmp/hu_identity_test_XXXXXX";
    int fd = mkstemp(path);
    HU_ASSERT_TRUE(fd >= 0);
    close(fd);
    /* Unlink so the save creates a fresh file (mkstemp leaves an
     * empty file the loader would consider corrupt). */
    unlink(path);

    HU_ASSERT_EQ(hu_identity_save(&g, path), HU_OK);
    hu_identity_graph_t g2;
    HU_ASSERT_EQ(hu_identity_load(&g2, path), HU_OK);
    HU_ASSERT_EQ(g2.contact_count, g.contact_count);
    HU_ASSERT_EQ(g2.contacts[0].alias_count, g.contacts[0].alias_count);
    HU_ASSERT_EQ((int)g2.contacts[0].merge_confidence, (int)g.contacts[0].merge_confidence);
    HU_ASSERT_STR_EQ(g2.contacts[0].canonical_name, g.contacts[0].canonical_name);
    unlink(path);
}

static void load_missing_file_returns_not_found(void) {
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_load(&g, "/tmp/hu_identity_test_does_not_exist_xyz"),
                 HU_ERR_NOT_FOUND);
}

/* -------------------- weakest-link enforcement -------------------- */

static void weakest_link_is_low_when_mixing_high_and_low_merges(void) {
    /* Phone canonical match (HIGH) PLUS a name-only Slack bridge (LOW).
     * Result must be LOW. */
    const char *handles[] = {"+15551234567", "(555) 123-4567", "U07ALICE"};
    const char *channels[] = {"imessage", "imessage", "slack"};
    const char *names[] = {"Alice", "Alice", "Alice"};
    hu_identity_graph_t g;
    HU_ASSERT_EQ(hu_identity_resolve(handles, channels, names, 3, &g), HU_OK);
    HU_ASSERT_EQ(g.contact_count, (size_t)1);
    HU_ASSERT_EQ((int)g.contacts[0].merge_confidence, (int)HU_IDENTITY_CONFIDENCE_LOW);
}

/* -------------------- runner -------------------- */

void run_identity_resolver_tests(void) {
    HU_TEST_SUITE("identity_resolver");

    /* canonicalization */
    HU_RUN_TEST(canonicalize_phone_us_format_keeps_last_10_digits);
    HU_RUN_TEST(canonicalize_phone_intl_format_matches_us_format);
    HU_RUN_TEST(canonicalize_phone_too_short_returns_zero);
    HU_RUN_TEST(canonicalize_email_lowercases);
    HU_RUN_TEST(canonicalize_email_gmail_dots_collapse);
    HU_RUN_TEST(canonicalize_email_non_gmail_preserves_dots);
    HU_RUN_TEST(canonicalize_email_rejects_non_email);

    /* merges */
    HU_RUN_TEST(resolve_two_phone_formats_merges_high);
    HU_RUN_TEST(resolve_gmail_dot_variants_merges_high);
    HU_RUN_TEST(resolve_phone_email_slack_via_name_lowers_to_low);

    /* privacy adversarial */
    HU_RUN_TEST(resolve_two_strangers_same_first_name_stay_low);
    HU_RUN_TEST(resolve_same_name_different_phones_do_not_merge);
    HU_RUN_TEST(resolve_same_name_different_gmail_addresses_do_not_merge);
    HU_RUN_TEST(resolve_similar_email_and_slack_name_does_not_high_merge);

    /* isolation + overflow */
    HU_RUN_TEST(resolve_lone_phone_stays_alone);
    HU_RUN_TEST(resolve_overflow_clamps_to_max_contacts);
    HU_RUN_TEST(resolve_empty_input_returns_empty_graph);
    HU_RUN_TEST(resolve_null_out_returns_error);

    /* lookup */
    HU_RUN_TEST(lookup_returns_canonical_contact);
    HU_RUN_TEST(lookup_unknown_handle_returns_null);

    /* persistence */
    HU_RUN_TEST(save_load_roundtrip_preserves_graph);
    HU_RUN_TEST(load_missing_file_returns_not_found);

    /* weakest-link */
    HU_RUN_TEST(weakest_link_is_low_when_mixing_high_and_low_merges);
}
