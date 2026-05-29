#include "human/security/arg_inspector.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_EXTRACT_STRINGS 64
#define MAX_STRING_LEN      4096

typedef struct {
    const char *ptr;
    size_t len;
} extracted_t;

/* Stage 1: Deep string extraction from JSON arguments.
 * Walks JSON structure to pull out all string values without full parsing. */
static size_t extract_json_strings(const char *json, size_t json_len, extracted_t *out,
                                   size_t out_cap) {
    size_t count = 0;
    if (!json || json_len == 0)
        return 0;

    size_t i = 0;
    while (i < json_len && count < out_cap) {
        if (json[i] == '"') {
            i++;
            size_t start = i;
            while (i < json_len) {
                if (json[i] == '\\' && i + 1 < json_len) {
                    i += 2;
                    continue;
                }
                if (json[i] == '"')
                    break;
                i++;
            }
            size_t len = i - start;
            if (len > 0 && len <= MAX_STRING_LEN) {
                out[count].ptr = json + start;
                out[count].len = len;
                count++;
            }
            if (i < json_len)
                i++;
        } else {
            i++;
        }
    }
    return count;
}

static bool has_prefix(const char *s, size_t slen, const char *prefix) {
    size_t plen = strlen(prefix);
    return slen >= plen && memcmp(s, prefix, plen) == 0;
}

/* Stage 2: Content-first risk scanning per extracted string. */
static uint32_t scan_string(const char *s, size_t len) {
    uint32_t flags = HU_ARG_RISK_NONE;

    /* Shell injection patterns */
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '`' || (s[i] == '$' && i + 1 < len && s[i + 1] == '('))
            flags |= HU_ARG_RISK_SHELL_INJECT;
        if (s[i] == ';' || s[i] == '|')
            flags |= HU_ARG_RISK_SHELL_INJECT;
    }
    if (hu_str_contains_ci_cstr(s, len, "&&"))
        flags |= HU_ARG_RISK_SHELL_INJECT;

    /* Path traversal */
    if (hu_str_contains_ci_cstr(s, len, "../") || hu_str_contains_ci_cstr(s, len, "..\\"))
        flags |= HU_ARG_RISK_PATH_TRAVERSAL;
    if (hu_str_contains_ci_cstr(s, len, "/etc/passwd") || hu_str_contains_ci_cstr(s, len, "/etc/shadow"))
        flags |= HU_ARG_RISK_PATH_TRAVERSAL;

    /* Suspicious URLs (exfiltration, C2) */
    if (has_prefix(s, len, "http://") || has_prefix(s, len, "https://")) {
        if (hu_str_contains_ci_cstr(s, len, "ngrok") || hu_str_contains_ci_cstr(s, len, "webhook.site") ||
            hu_str_contains_ci_cstr(s, len, "requestbin") || hu_str_contains_ci_cstr(s, len, "burpcollaborator"))
            flags |= HU_ARG_RISK_EXFILTRATION;
    }
    if (hu_str_contains_ci_cstr(s, len, "curl ") || hu_str_contains_ci_cstr(s, len, "wget "))
        flags |= HU_ARG_RISK_URL_SUSPECT;

    /* Secret/credential patterns */
    if (hu_str_contains_ci_cstr(s, len, "api_key") || hu_str_contains_ci_cstr(s, len, "apikey") ||
        hu_str_contains_ci_cstr(s, len, "secret_key") || hu_str_contains_ci_cstr(s, len, "password"))
        flags |= HU_ARG_RISK_SECRET_LEAK;
    if (has_prefix(s, len, "sk-") || has_prefix(s, len, "ghp_") || has_prefix(s, len, "glpat-"))
        flags |= HU_ARG_RISK_SECRET_LEAK;

    /* Prompt injection markers in tool args */
    if (hu_str_contains_ci_cstr(s, len, "ignore previous") || hu_str_contains_ci_cstr(s, len, "ignore all") ||
        hu_str_contains_ci_cstr(s, len, "system prompt") || hu_str_contains_ci_cstr(s, len, "you are now"))
        flags |= HU_ARG_RISK_PROMPT_INJECT;

    return flags;
}

static size_t append_finding(char *buf, size_t cap, size_t pos, const char *msg) {
    size_t mlen = strlen(msg);
    if (pos + mlen + 2 < cap) {
        if (pos > 0) {
            buf[pos++] = ';';
            buf[pos++] = ' ';
        }
        memcpy(buf + pos, msg, mlen);
        pos += mlen;
        buf[pos] = '\0';
    }
    return pos;
}

hu_error_t hu_arg_inspect(const char *tool_name, const char *args_json, size_t args_json_len,
                          hu_arg_inspection_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!args_json || args_json_len == 0) {
        out->overall_risk = HU_RISK_LOW;
        return HU_OK;
    }

    extracted_t strings[MAX_EXTRACT_STRINGS];
    size_t count = extract_json_strings(args_json, args_json_len, strings, MAX_EXTRACT_STRINGS);
    out->strings_extracted = count;

    uint32_t all_flags = HU_ARG_RISK_NONE;
    for (size_t i = 0; i < count; i++)
        all_flags |= scan_string(strings[i].ptr, strings[i].len);
    out->risk_flags = all_flags;

    /* Shell/spawn tools with injection patterns are always high risk */
    if (tool_name &&
        (strcmp(tool_name, "shell") == 0 || strcmp(tool_name, "spawn") == 0) &&
        (all_flags & HU_ARG_RISK_SHELL_INJECT))
        all_flags |= HU_ARG_RISK_SHELL_INJECT;

    /* Build findings summary */
    size_t pos = 0;
    if (all_flags & HU_ARG_RISK_SHELL_INJECT)
        pos = append_finding(out->findings, sizeof(out->findings), pos, "shell_injection_pattern");
    if (all_flags & HU_ARG_RISK_PATH_TRAVERSAL)
        pos = append_finding(out->findings, sizeof(out->findings), pos, "path_traversal");
    if (all_flags & HU_ARG_RISK_URL_SUSPECT)
        pos = append_finding(out->findings, sizeof(out->findings), pos, "suspect_url");
    if (all_flags & HU_ARG_RISK_SECRET_LEAK)
        pos = append_finding(out->findings, sizeof(out->findings), pos, "potential_secret_leak");
    if (all_flags & HU_ARG_RISK_PROMPT_INJECT)
        pos = append_finding(out->findings, sizeof(out->findings), pos, "prompt_injection_marker");
    if (all_flags & HU_ARG_RISK_EXFILTRATION)
        pos = append_finding(out->findings, sizeof(out->findings), pos, "data_exfiltration_target");
    out->findings_len = pos;

    /* Stage 3: Risk classification */
    if (all_flags & (HU_ARG_RISK_EXFILTRATION | HU_ARG_RISK_SECRET_LEAK))
        out->overall_risk = HU_RISK_HIGH;
    else if (all_flags & (HU_ARG_RISK_SHELL_INJECT | HU_ARG_RISK_PATH_TRAVERSAL |
                          HU_ARG_RISK_PROMPT_INJECT))
        out->overall_risk = HU_RISK_HIGH;
    else if (all_flags & HU_ARG_RISK_URL_SUSPECT)
        out->overall_risk = HU_RISK_MEDIUM;
    else
        out->overall_risk = HU_RISK_LOW;

    return HU_OK;
}

bool hu_arg_inspection_should_block(const hu_arg_inspection_t *insp,
                                    const hu_security_policy_t *policy) {
    if (!insp || !policy)
        return false;
    if (policy->block_high_risk_commands && insp->overall_risk == HU_RISK_HIGH)
        return true;
    if (insp->risk_flags & HU_ARG_RISK_EXFILTRATION)
        return true;
    return false;
}

bool hu_arg_inspection_needs_approval(const hu_arg_inspection_t *insp,
                                      const hu_security_policy_t *policy) {
    if (!insp || !policy)
        return false;
    if (policy->autonomy == HU_AUTONOMY_SUPERVISED && insp->overall_risk >= HU_RISK_MEDIUM)
        return true;
    if (policy->autonomy == HU_AUTONOMY_ASSISTED && insp->overall_risk == HU_RISK_HIGH)
        return true;
    if (insp->risk_flags & HU_ARG_RISK_PROMPT_INJECT)
        return true;
    return false;
}
