/* Phase A1.2 PII redactor — adversarial corpus.
 *
 * The redactor must (a) find every PII pattern in synthetic training
 * corpora at <0.1% leak rate, (b) leave non-PII text untouched, and
 * (c) never overflow the output buffer.
 *
 * Tests are organized as:
 *   - Per-pattern positive cases (one synthetic message per category)
 *   - Per-pattern negative cases (lookalikes that should NOT be redacted)
 *   - Mixed cases (multiple PII types in one message)
 *   - Boundary cases (output truncation, empty input, NULL args)
 *   - Adversarial corpus (50 lines mixing PII and non-PII; <0.1% leak) */

#include "human/ml/training_data_quality.h"
#include "test_framework.h"

#include <string.h>

/* ── Per-pattern positive ──────────────────────────────────────────── */

static void pii_redacts_email(void) {
    char out[256];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Email me at alice@example.com please.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[EMAIL]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "alice") == NULL);
    HU_ASSERT_TRUE(strstr(out, "example.com") == NULL);
    HU_ASSERT_EQ((long long)stats.emails, 1LL);
}

static void pii_redacts_ssn(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "My SSN is 123-45-6789. Don't tell.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[SSN]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "123-45-6789") == NULL);
    HU_ASSERT_EQ((long long)stats.ssns, 1LL);
}

static void pii_redacts_credit_card_separated(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Charge 4111-1111-1111-1111 today.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[CC]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "4111") == NULL);
    HU_ASSERT_EQ((long long)stats.credit_cards, 1LL);
}

static void pii_redacts_credit_card_unbroken(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Card 4111111111111111 is on file.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[CC]") != NULL);
    HU_ASSERT_EQ((long long)stats.credit_cards, 1LL);
}

static void pii_redacts_ipv4(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Server is at 192.168.1.42 right now.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[IP]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "192.168") == NULL);
    HU_ASSERT_EQ((long long)stats.ips, 1LL);
}

static void pii_redacts_phone_paren_form(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Call me at (555) 123-4567 anytime.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[PHONE]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "555") == NULL);
    HU_ASSERT_EQ((long long)stats.phones, 1LL);
}

static void pii_redacts_phone_dashed(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Phone: 415-555-7890 thanks.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[PHONE]") != NULL);
    HU_ASSERT_EQ((long long)stats.phones, 1LL);
}

static void pii_redacts_secret_token(void) {
    char out[256];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "api_key=sk_live_abc123xyz456def789ghi run it";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "[SECRET]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "sk_live") == NULL);
    HU_ASSERT_EQ((long long)stats.secrets, 1LL);
}

/* ── Per-pattern negative — must NOT redact ─────────────────────────── */

static void pii_does_not_redact_short_number_sequence(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Pi is 3.14159 and the year is 2026.";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)hu_pii_total(&stats), 0LL);
    HU_ASSERT_TRUE(strcmp(out, in) == 0);
}

static void pii_does_not_redact_at_in_username(void) {
    /* "@user" without TLD is not an email. */
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "talked to @bob about it";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)stats.emails, 0LL);
    HU_ASSERT_TRUE(strstr(out, "[EMAIL]") == NULL);
}

static void pii_does_not_redact_version_string_as_ip(void) {
    /* "1.2.3.4" with leading-zero / out-of-range octets shouldn't match;
     * here we use a plain "1.2.3" version. */
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "running version 1.2.3 of the app";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)stats.ips, 0LL);
    HU_ASSERT_TRUE(strcmp(out, in) == 0);
}

static void pii_does_not_redact_ipv4_with_octet_too_large(void) {
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "code 999.999.999.999 is invalid";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)stats.ips, 0LL);
}

static void pii_does_not_redact_short_token_after_keyword(void) {
    /* Token has to be ≥16 chars to qualify as a secret. */
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "password: short99";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)stats.secrets, 0LL);
}

static void pii_does_not_redact_unseparated_phone_run(void) {
    /* 10 digits glued together w/o separators should be left alone. */
    char out[128];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "id: 4155557890 (internal)";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)stats.phones, 0LL);
}

/* ── Mixed-pattern message ──────────────────────────────────────────── */

static void pii_redacts_multiple_patterns_in_one_message(void) {
    char out[1024];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] =
        "Hi, contact me at jane@example.com or 415-555-1212. "
        "Card 4242-4242-4242-4242, server 10.0.0.1, "
        "ssn 999-88-7777, api_key=abcdef0123456789xyzMORE";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)stats.emails, 1LL);
    HU_ASSERT_EQ((long long)stats.phones, 1LL);
    HU_ASSERT_EQ((long long)stats.credit_cards, 1LL);
    HU_ASSERT_EQ((long long)stats.ips, 1LL);
    HU_ASSERT_EQ((long long)stats.ssns, 1LL);
    HU_ASSERT_EQ((long long)stats.secrets, 1LL);
    HU_ASSERT_EQ((long long)hu_pii_total(&stats), 6LL);
    /* No raw PII bytes should leak through. */
    HU_ASSERT_TRUE(strstr(out, "jane") == NULL);
    HU_ASSERT_TRUE(strstr(out, "415-555") == NULL);
    HU_ASSERT_TRUE(strstr(out, "4242") == NULL);
    HU_ASSERT_TRUE(strstr(out, "10.0") == NULL);
    HU_ASSERT_TRUE(strstr(out, "999-88") == NULL);
    HU_ASSERT_TRUE(strstr(out, "abcdef0123") == NULL);
}

/* ── Boundary / safety ──────────────────────────────────────────────── */

static void pii_handles_empty_input(void) {
    char out[64] = "untouched";
    size_t out_len = 99;
    hu_pii_stats_t stats;
    HU_ASSERT_EQ(hu_pii_redact("", 0, out, sizeof(out), &out_len, &stats), HU_OK);
    HU_ASSERT_EQ((long long)out_len, 0LL);
    HU_ASSERT_EQ(out[0], '\0');
    HU_ASSERT_EQ((long long)hu_pii_total(&stats), 0LL);
}

static void pii_rejects_null_args(void) {
    char out[64];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_pii_redact(NULL, 0, out, sizeof(out), &out_len, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_pii_redact("hello", 5, NULL, 0, &out_len, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_pii_redact("hello", 5, out, 0, &out_len, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void pii_truncates_silently_when_buffer_too_small(void) {
    /* Output cannot fit; redactor should write what it can and NUL-terminate. */
    char out[8] = {0};
    size_t out_len = 0;
    hu_pii_stats_t stats;
    const char in[] = "Email me at alice@example.com";
    HU_ASSERT_EQ(hu_pii_redact(in, strlen(in), out, sizeof(out), &out_len, &stats), HU_OK);
    /* The result is NUL-terminated within bounds. */
    HU_ASSERT_TRUE(out_len < sizeof(out));
    HU_ASSERT_EQ(out[sizeof(out) - 1], '\0');
}

static void pii_contains_pii_returns_false_for_clean_text(void) {
    const char in[] = "I love hiking on weekends, especially in the mountains.";
    HU_ASSERT_FALSE(hu_pii_contains_pii(in, strlen(in)));
}

static void pii_contains_pii_returns_true_for_email(void) {
    const char in[] = "ping me at me@h.uman";
    HU_ASSERT_TRUE(hu_pii_contains_pii(in, strlen(in)));
}

/* ── Adversarial corpus ─────────────────────────────────────────────── */

/* 50 mixed-content lines: 25 carrying PII, 25 clean. The redactor
 * must redact every PII line and leave the clean lines untouched.
 * Roadmap A1.2 acceptance: <0.1% PII leak rate. With 25 PII lines that
 * means at most 0 leaked tokens (1 / 50000 chars baseline ≪ 1 leak). */
static const char *kCorpusPii[] = {
    "alice@example.com sent the report",
    "back-end is at 10.0.0.42 right now",
    "phone (415) 555-1234 if urgent",
    "card 4111-1111-1111-1111 expired",
    "ssn 123-45-6789 on file",
    "Token=abcdef0123456789ABCDEF",
    "ping me at bob@h.uman thanks",
    "192.168.1.1 keeps timing out",
    "415.555.7890 is my cell",
    "5555-4444-3333-2222 declined",
    "ssn: 987-65-4321 confirmed",
    "secret: superlongtokenxyz123ABCdef",
    "carol@mail.co for invoices",
    "10.10.10.10 has the logs",
    "+1 415 555 1212 reaches me",
    "1234567812345678 went through",
    "111-22-3333 failed to verify",
    "API_KEY=ZXY987654321ABCDEF00",
    "dan.lee+filter@example.org bookmarked",
    "172.16.254.1 hosts the model",
    "(212) 555 0100 NYC office",
    "4242 4242 4242 4242 Stripe test",
    "999-88-7777 that's not real",
    "auth bearer xyz1234567890abcdef",
    "evan@dev.io shipped it",
};

static const char *kCorpusClean[] = {
    "I love hiking on weekends",
    "the meeting is at 3pm tomorrow",
    "version 1.2.3 of the library",
    "1234 calories before lunch is too many",
    "talked to @bob about Q3",
    "code review went well today",
    "shipped the feature on Friday",
    "the year is 2026 already",
    "error code 502 from the upstream",
    "prefer tea over coffee mostly",
    "running 10 miles this Saturday",
    "the build took 45 seconds",
    "we need to upgrade the doc set",
    "found a bug in the tokenizer",
    "remember to fill out the form",
    "the conference starts on Monday",
    "checked the status page just now",
    "good talk with the new hire",
    "wrap up the design doc",
    "let's meet at 9 in the morning",
    "the deploy went out clean",
    "RTT under 200ms feels snappy",
    "bumped the heap size to 8GB",
    "added a test for the edge case",
    "rolling upgrade tomorrow night",
};

static void pii_corpus_redacts_all_pii_lines(void) {
    char out[1024];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    size_t pii_lines = sizeof(kCorpusPii) / sizeof(kCorpusPii[0]);
    size_t leaked = 0;
    for (size_t i = 0; i < pii_lines; i++) {
        const char *line = kCorpusPii[i];
        memset(&stats, 0, sizeof(stats));
        HU_ASSERT_EQ(hu_pii_redact(line, strlen(line), out, sizeof(out), &out_len, &stats),
                     HU_OK);
        if (hu_pii_total(&stats) == 0) {
            leaked++;
        }
    }
    /* 0 leaks expected on a 25-line corpus. */
    if (leaked > 0) {
        HU_FAIL("%zu/%zu PII lines slipped through redactor", leaked, pii_lines);
    }
}

static void pii_corpus_leaves_clean_lines_untouched(void) {
    char out[512];
    size_t out_len = 0;
    hu_pii_stats_t stats;
    size_t clean_lines = sizeof(kCorpusClean) / sizeof(kCorpusClean[0]);
    size_t false_positives = 0;
    for (size_t i = 0; i < clean_lines; i++) {
        const char *line = kCorpusClean[i];
        memset(&stats, 0, sizeof(stats));
        HU_ASSERT_EQ(hu_pii_redact(line, strlen(line), out, sizeof(out), &out_len, &stats),
                     HU_OK);
        if (hu_pii_total(&stats) > 0)
            false_positives++;
    }
    /* False positives ≤ 0 on this curated corpus. */
    if (false_positives > 0) {
        HU_FAIL("%zu/%zu clean lines incorrectly redacted", false_positives, clean_lines);
    }
}

void run_training_data_quality_tests(void);
void run_training_data_quality_tests(void) {
    HU_TEST_SUITE("training_data_quality");
    HU_RUN_TEST(pii_redacts_email);
    HU_RUN_TEST(pii_redacts_ssn);
    HU_RUN_TEST(pii_redacts_credit_card_separated);
    HU_RUN_TEST(pii_redacts_credit_card_unbroken);
    HU_RUN_TEST(pii_redacts_ipv4);
    HU_RUN_TEST(pii_redacts_phone_paren_form);
    HU_RUN_TEST(pii_redacts_phone_dashed);
    HU_RUN_TEST(pii_redacts_secret_token);
    HU_RUN_TEST(pii_does_not_redact_short_number_sequence);
    HU_RUN_TEST(pii_does_not_redact_at_in_username);
    HU_RUN_TEST(pii_does_not_redact_version_string_as_ip);
    HU_RUN_TEST(pii_does_not_redact_ipv4_with_octet_too_large);
    HU_RUN_TEST(pii_does_not_redact_short_token_after_keyword);
    HU_RUN_TEST(pii_does_not_redact_unseparated_phone_run);
    HU_RUN_TEST(pii_redacts_multiple_patterns_in_one_message);
    HU_RUN_TEST(pii_handles_empty_input);
    HU_RUN_TEST(pii_rejects_null_args);
    HU_RUN_TEST(pii_truncates_silently_when_buffer_too_small);
    HU_RUN_TEST(pii_contains_pii_returns_false_for_clean_text);
    HU_RUN_TEST(pii_contains_pii_returns_true_for_email);
    HU_RUN_TEST(pii_corpus_redacts_all_pii_lines);
    HU_RUN_TEST(pii_corpus_leaves_clean_lines_untouched);
}
