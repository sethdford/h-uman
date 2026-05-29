#include "human/security/sensitivity.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static bool ends_with(const char *s, size_t s_len, const char *suffix) {
    size_t suffix_len = strlen(suffix);
    if (suffix_len > s_len)
        return false;
    for (size_t i = 0; i < suffix_len; i++) {
        if (tolower((unsigned char)s[s_len - suffix_len + i]) !=
            tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

/* Check if a sequence of digits of exactly `count` length starts at pos */
static bool is_digit_run(const char *s, size_t len, size_t pos, size_t count) {
    if (pos + count > len)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (!isdigit((unsigned char)s[pos + i]))
            return false;
    }
    return true;
}

/* ── S3 keyword detection (secrets, keys, credentials) ────────────────── */

static const char *S3_KEYWORDS[] = {
    "private key", "secret key", "api key", "ssh key",
    "access token", "refresh token", "bearer token",
    "password is", "my password", "the password",
    "social security number", "bank account number",
    "credit card number", "routing number",
    "BEGIN RSA PRIVATE", "BEGIN EC PRIVATE", "BEGIN PRIVATE KEY",
    "BEGIN OPENSSH PRIVATE",
};
#define S3_KEYWORD_COUNT (sizeof(S3_KEYWORDS) / sizeof(S3_KEYWORDS[0]))

/* ── S2 keyword detection (PII, semi-private) ─────────────────────────── */

static const char *S2_KEYWORDS[] = {
    "date of birth", "social security", "phone number",
    "home address", "mailing address", "street address",
    "bank account", "credit card", "debit card",
    "driver's license", "drivers license", "passport number",
    "medical record", "health insurance", "diagnosis",
    "salary", "compensation", "pay stub", "tax return",
    "my ssn", "my email is",
};
#define S2_KEYWORD_COUNT (sizeof(S2_KEYWORDS) / sizeof(S2_KEYWORDS[0]))

/* ── Pattern detection ────────────────────────────────────────────────── */

/* SSN pattern: 3 digits, separator, 2 digits, separator, 4 digits */
static bool has_ssn_pattern(const char *msg, size_t len) {
    for (size_t i = 0; i + 10 < len; i++) {
        if (is_digit_run(msg, len, i, 3) &&
            (msg[i + 3] == '-' || msg[i + 3] == ' ') &&
            is_digit_run(msg, len, i + 4, 2) &&
            (msg[i + 6] == '-' || msg[i + 6] == ' ') &&
            is_digit_run(msg, len, i + 7, 4)) {
            return true;
        }
    }
    return false;
}

/* Luhn checksum validation for credit card numbers */
static bool luhn_check(const int *digits, size_t count) {
    if (count < 13 || count > 19)
        return false;
    int sum = 0;
    bool alt = false;
    for (size_t i = count; i > 0; i--) {
        int d = digits[i - 1];
        if (alt) {
            d *= 2;
            if (d > 9)
                d -= 9;
        }
        sum += d;
        alt = !alt;
    }
    return (sum % 10) == 0;
}

/* Credit card: 4 groups of 4 digits separated by spaces or dashes, Luhn-validated */
static bool has_credit_card_pattern(const char *msg, size_t len) {
    for (size_t i = 0; i + 18 < len; i++) {
        if (is_digit_run(msg, len, i, 4) &&
            (msg[i + 4] == '-' || msg[i + 4] == ' ') &&
            is_digit_run(msg, len, i + 5, 4) &&
            (msg[i + 9] == '-' || msg[i + 9] == ' ') &&
            is_digit_run(msg, len, i + 10, 4) &&
            (msg[i + 14] == '-' || msg[i + 14] == ' ') &&
            is_digit_run(msg, len, i + 15, 4)) {
            int digits[16];
            size_t offsets[] = {0, 1, 2, 3, 5, 6, 7, 8, 10, 11, 12, 13, 15, 16, 17, 18};
            for (size_t k = 0; k < 16; k++)
                digits[k] = msg[i + offsets[k]] - '0';
            if (luhn_check(digits, 16))
                return true;
        }
    }
    /* Also check 16-digit runs without separators */
    for (size_t i = 0; i + 15 < len; i++) {
        if (is_digit_run(msg, len, i, 16)) {
            bool left_ok = (i == 0 || !isdigit((unsigned char)msg[i - 1]));
            bool right_ok = (i + 16 >= len || !isdigit((unsigned char)msg[i + 16]));
            if (left_ok && right_ok) {
                int digits[16];
                for (size_t k = 0; k < 16; k++)
                    digits[k] = msg[i + k] - '0';
                if (luhn_check(digits, 16))
                    return true;
            }
        }
    }
    return false;
}

/* Email pattern: word@word.word */
static bool has_email_pattern(const char *msg, size_t len) {
    for (size_t i = 1; i + 3 < len; i++) {
        if (msg[i] != '@')
            continue;
        bool left_ok = isalnum((unsigned char)msg[i - 1]) || msg[i - 1] == '.' || msg[i - 1] == '_';
        if (!left_ok)
            continue;
        for (size_t j = i + 1; j < len; j++) {
            if (msg[j] == '.') {
                if (j > i + 1 && j + 1 < len && isalpha((unsigned char)msg[j + 1]))
                    return true;
            } else if (!isalnum((unsigned char)msg[j]) && msg[j] != '-' && msg[j] != '_') {
                break;
            }
        }
    }
    return false;
}

/* Phone number: 10+ digits with optional country code and separators */
static bool has_phone_pattern(const char *msg, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (msg[i] == '+' || isdigit((unsigned char)msg[i])) {
            size_t digit_count = 0;
            while (i < len && (isdigit((unsigned char)msg[i]) || msg[i] == '-' ||
                               msg[i] == ' ' || msg[i] == '(' || msg[i] == ')' ||
                               msg[i] == '+' || msg[i] == '.')) {
                if (isdigit((unsigned char)msg[i]))
                    digit_count++;
                i++;
            }
            if (digit_count >= 10 && digit_count <= 15)
                return true;
        } else {
            i++;
        }
    }
    return false;
}

/* Private key header: -----BEGIN ... PRIVATE KEY----- */
static bool has_private_key_header(const char *msg, size_t len) {
    return hu_str_contains_ci_cstr(msg, len, "-----BEGIN") &&
           hu_str_contains_ci_cstr(msg, len, "PRIVATE KEY-----");
}

/* ── S3 path patterns ─────────────────────────────────────────────────── */

static const char *S3_PATH_SEGMENTS[] = {
    ".ssh/",     ".env",      "credentials",
    "id_rsa",    "id_dsa",    "id_ecdsa",  "id_ed25519",
    ".gnupg/",   ".aws/",     "secrets/",  "vault/",
};
#define S3_PATH_SEGMENT_COUNT (sizeof(S3_PATH_SEGMENTS) / sizeof(S3_PATH_SEGMENTS[0]))

static const char *S3_PATH_EXTENSIONS[] = {
    ".pem", ".key", ".p12", ".pfx", ".jks", ".keystore",
};
#define S3_PATH_EXT_COUNT (sizeof(S3_PATH_EXTENSIONS) / sizeof(S3_PATH_EXTENSIONS[0]))

/* ── S3 tool patterns ─────────────────────────────────────────────────── */

static const char *S3_TOOL_SEGMENTS[] = {
    "vault", "keychain", "credential", "secret",
    "ssh", "gpg", "pgp",
};
#define S3_TOOL_SEGMENT_COUNT (sizeof(S3_TOOL_SEGMENTS) / sizeof(S3_TOOL_SEGMENTS[0]))

/* ── Public API ───────────────────────────────────────────────────────── */

hu_sensitivity_result_t hu_sensitivity_classify_message(const char *msg, size_t msg_len) {
    hu_sensitivity_result_t result;
    memset(&result, 0, sizeof(result));
    result.level = HU_SENSITIVITY_S1;
    if (!msg || msg_len == 0)
        return result;

    int s3_signals = 0;
    int s2_signals = 0;
    const char *s3_reason = NULL;
    const char *s2_reason = NULL;

    for (size_t i = 0; i < S3_KEYWORD_COUNT; i++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, S3_KEYWORDS[i])) {
            s3_signals++;
            if (!s3_reason)
                s3_reason = S3_KEYWORDS[i];
        }
    }

    if (has_private_key_header(msg, msg_len)) {
        s3_signals++;
        if (!s3_reason)
            s3_reason = "private key block detected";
    }

    if (has_ssn_pattern(msg, msg_len)) {
        s3_signals++;
        if (!s3_reason)
            s3_reason = "SSN pattern detected";
    }

    if (s3_signals > 0) {
        result.level = HU_SENSITIVITY_S3;
        result.reason = s3_reason;
        result.signal_count = s3_signals;
        result.confidence = s3_signals >= 2 ? 0.95f : 0.80f;
        return result;
    }

    for (size_t i = 0; i < S2_KEYWORD_COUNT; i++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, S2_KEYWORDS[i])) {
            s2_signals++;
            if (!s2_reason)
                s2_reason = S2_KEYWORDS[i];
        }
    }

    if (has_credit_card_pattern(msg, msg_len)) {
        s2_signals++;
        if (!s2_reason)
            s2_reason = "credit card pattern detected (Luhn valid)";
    }

    if (has_email_pattern(msg, msg_len)) {
        s2_signals++;
        if (!s2_reason)
            s2_reason = "email address detected";
    }

    if (has_phone_pattern(msg, msg_len)) {
        s2_signals++;
        if (!s2_reason)
            s2_reason = "phone number detected";
    }

    if (s2_signals > 0) {
        result.level = HU_SENSITIVITY_S2;
        result.reason = s2_reason;
        result.signal_count = s2_signals;
        result.confidence = s2_signals >= 3 ? 0.90f : (s2_signals >= 2 ? 0.75f : 0.60f);
        return result;
    }

    result.confidence = 0.95f;
    return result;
}

hu_sensitivity_result_t hu_sensitivity_classify_path(const char *path, size_t path_len) {
    hu_sensitivity_result_t result = {HU_SENSITIVITY_S1, NULL, 0.0f, 0};
    if (!path || path_len == 0)
        return result;

    for (size_t i = 0; i < S3_PATH_SEGMENT_COUNT; i++) {
        if (hu_str_contains_ci_cstr(path, path_len, S3_PATH_SEGMENTS[i])) {
            result.level = HU_SENSITIVITY_S3;
            result.reason = S3_PATH_SEGMENTS[i];
            return result;
        }
    }

    for (size_t i = 0; i < S3_PATH_EXT_COUNT; i++) {
        if (ends_with(path, path_len, S3_PATH_EXTENSIONS[i])) {
            result.level = HU_SENSITIVITY_S3;
            result.reason = S3_PATH_EXTENSIONS[i];
            return result;
        }
    }

    return result;
}

hu_sensitivity_result_t hu_sensitivity_classify_tool(const char *tool_name, size_t tool_len) {
    hu_sensitivity_result_t result = {HU_SENSITIVITY_S1, NULL, 0.0f, 0};
    if (!tool_name || tool_len == 0)
        return result;

    for (size_t i = 0; i < S3_TOOL_SEGMENT_COUNT; i++) {
        if (hu_str_contains_ci_cstr(tool_name, tool_len, S3_TOOL_SEGMENTS[i])) {
            result.level = HU_SENSITIVITY_S3;
            result.reason = S3_TOOL_SEGMENTS[i];
            return result;
        }
    }

    return result;
}

hu_sensitivity_result_t hu_sensitivity_merge(const hu_sensitivity_result_t *a,
                                             const hu_sensitivity_result_t *b) {
    if (!a && !b)
        return (hu_sensitivity_result_t){HU_SENSITIVITY_S1, NULL, 0.0f, 0};
    if (!a)
        return *b;
    if (!b)
        return *a;
    return a->level >= b->level ? *a : *b;
}

bool hu_sensitivity_requires_local(hu_sensitivity_level_t level) {
    return level >= HU_SENSITIVITY_S3;
}

const char *hu_sensitivity_level_str(hu_sensitivity_level_t level) {
    switch (level) {
    case HU_SENSITIVITY_S1:
        return "S1";
    case HU_SENSITIVITY_S2:
        return "S2";
    case HU_SENSITIVITY_S3:
        return "S3";
    default:
        return "unknown";
    }
}
