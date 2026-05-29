#include "human/security/mcp_audit.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

#define HU_MCP_AUDIT_INITIAL_CAP 8
#define HU_MCP_AUDIT_MAX_LENGTH 2048
#define HU_MCP_AUDIT_BASE64_RUN 50

static int risk_ord(hu_mcp_audit_risk_t r) {
    switch (r) {
        case HU_MCP_AUDIT_CLEAN: return 0;
        case HU_MCP_AUDIT_LOW: return 1;
        case HU_MCP_AUDIT_MEDIUM: return 2;
        case HU_MCP_AUDIT_HIGH: return 3;
        case HU_MCP_AUDIT_CRITICAL: return 4;
    }
    return 0;
}

static hu_mcp_audit_risk_t max_risk(hu_mcp_audit_risk_t a, hu_mcp_audit_risk_t b) {
    return risk_ord(a) >= risk_ord(b) ? a : b;
}

static void add_finding(hu_allocator_t *alloc, hu_mcp_audit_result_t *result,
                        hu_mcp_audit_risk_t risk, const char *pattern, const char *desc) {
    if (result->finding_count >= result->findings_cap) {
        size_t new_cap = result->findings_cap == 0 ? HU_MCP_AUDIT_INITIAL_CAP : result->findings_cap * 2;
        hu_mcp_audit_finding_t *new_findings = (hu_mcp_audit_finding_t *)alloc->realloc(
            alloc->ctx, result->findings,
            result->findings_cap * sizeof(hu_mcp_audit_finding_t),
            new_cap * sizeof(hu_mcp_audit_finding_t));
        if (!new_findings)
            return;
        result->findings = new_findings;
        result->findings_cap = new_cap;
    }
    result->findings[result->finding_count].risk = risk;
    result->findings[result->finding_count].pattern = pattern;
    result->findings[result->finding_count].description = desc;
    result->finding_count++;
    result->overall_risk = max_risk(result->overall_risk, risk);
}

static bool is_base64_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

static bool has_long_base64_run(const char *s, size_t len) {
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        if (is_base64_char(s[i]))
            run++;
        else
            run = 0;
        if (run > HU_MCP_AUDIT_BASE64_RUN)
            return true;
    }
    return false;
}

static bool has_invisible_unicode(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < len) {
            if ((unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] >= 0x8B && (unsigned char)s[i + 2] <= 0x8D)
                return true;
            if ((unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x8C)
                return true;
        }
        if (c == 0xEF && i + 2 < len && (unsigned char)s[i + 1] == 0xBB && (unsigned char)s[i + 2] == 0xBF)
            return true;
    }
    return false;
}

static const char *INJECTION_PATTERNS[][2] = {
    {"ignore previous", "Prompt injection: ignore previous instructions"},
    {"ignore all previous", "Prompt injection: ignore all previous instructions"},
    {"system:", "Prompt injection: system prompt override"},
    {"you are now", "Prompt injection: role override"},
    {"disregard", "Prompt injection: disregard instructions"},
    {"override instructions", "Prompt injection: override instructions"},
    {"new instructions:", "Prompt injection: new instructions"},
    {"forget everything", "Prompt injection: forget everything"},
};

#define INJECTION_COUNT (sizeof(INJECTION_PATTERNS) / sizeof(INJECTION_PATTERNS[0]))

static void run_checks(hu_allocator_t *alloc, hu_mcp_audit_result_t *result,
                      const char *description, size_t desc_len) {
    for (size_t i = 0; i < INJECTION_COUNT; i++) {
        if (hu_str_contains_ci_cstr(description, desc_len, INJECTION_PATTERNS[i][0])) {
            add_finding(alloc, result, HU_MCP_AUDIT_CRITICAL, INJECTION_PATTERNS[i][0], INJECTION_PATTERNS[i][1]);
        }
    }
    if (has_long_base64_run(description, desc_len))
        add_finding(alloc, result, HU_MCP_AUDIT_HIGH, "base64_run", "Long base64-like content detected");
    if (desc_len > HU_MCP_AUDIT_MAX_LENGTH)
        add_finding(alloc, result, HU_MCP_AUDIT_MEDIUM, "excessive_length", "Description exceeds 2048 characters");
    if (has_invisible_unicode(description, desc_len))
        add_finding(alloc, result, HU_MCP_AUDIT_HIGH, "invisible_unicode", "Zero-width or invisible Unicode detected");
    if (hu_str_contains_ci_cstr(description, desc_len, "http://") || hu_str_contains_ci_cstr(description, desc_len, "https://"))
        add_finding(alloc, result, HU_MCP_AUDIT_MEDIUM, "url", "URL in description (exfiltration risk)");
}

hu_error_t hu_mcp_audit_tool_description(hu_allocator_t *alloc, const char *description, size_t desc_len, hu_mcp_audit_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!description)
        return HU_ERR_INVALID_ARGUMENT;
    out->overall_risk = HU_MCP_AUDIT_CLEAN;
    out->findings = NULL;
    out->finding_count = 0;
    out->findings_cap = 0;
    if (desc_len == 0)
        return HU_OK;
    run_checks(alloc, out, description, desc_len);
    return HU_OK;
}

hu_error_t hu_mcp_audit_server(hu_allocator_t *alloc, const char **descriptions, size_t count, hu_mcp_audit_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!descriptions && count > 0)
        return HU_ERR_INVALID_ARGUMENT;
    out->overall_risk = HU_MCP_AUDIT_CLEAN;
    out->findings = NULL;
    out->finding_count = 0;
    out->findings_cap = 0;
    for (size_t i = 0; i < count; i++) {
        if (!descriptions[i])
            return HU_ERR_INVALID_ARGUMENT;
        size_t len = strlen(descriptions[i]);
        run_checks(alloc, out, descriptions[i], len);
    }
    return HU_OK;
}

void hu_mcp_audit_result_free(hu_allocator_t *alloc, hu_mcp_audit_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->findings) {
        alloc->free(alloc->ctx, result->findings, result->findings_cap * sizeof(hu_mcp_audit_finding_t));
        result->findings = NULL;
    }
    result->finding_count = 0;
    result->findings_cap = 0;
    result->overall_risk = HU_MCP_AUDIT_CLEAN;
}
