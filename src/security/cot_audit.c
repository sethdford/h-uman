#include "human/security/cot_audit.h"
#include "human/core/string.h"
#include <string.h>

hu_error_t hu_cot_audit(hu_allocator_t *alloc, const char *cot_text, size_t cot_len, hu_cot_audit_result_t *out) {
    if (!alloc || !cot_text || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->verdict = HU_COT_SAFE;
    out->confidence = 1.0;
    int signals = 0;
    if (hu_str_contains_ci_cstr(cot_text, cot_len, "ignore previous") || hu_str_contains_ci_cstr(cot_text, cot_len, "override") || hu_str_contains_ci_cstr(cot_text, cot_len, "system prompt")) { out->goal_hijack_detected = true; signals++; }
    if (hu_str_contains_ci_cstr(cot_text, cot_len, "escalat") || hu_str_contains_ci_cstr(cot_text, cot_len, "sudo") || hu_str_contains_ci_cstr(cot_text, cot_len, "admin")) { out->privilege_escalation_detected = true; signals++; }
    if (hu_str_contains_ci_cstr(cot_text, cot_len, "exfiltrat") || hu_str_contains_ci_cstr(cot_text, cot_len, "send to external") || hu_str_contains_ci_cstr(cot_text, cot_len, "api key")) { out->data_exfiltration_detected = true; signals++; }
    if (signals >= 2) { out->verdict = HU_COT_BLOCKED; out->confidence = 0.95; }
    else if (signals == 1) { out->verdict = HU_COT_SUSPICIOUS; out->confidence = 0.7; }
    out->reason = NULL; out->reason_len = 0;
    return HU_OK;
}

void hu_cot_audit_result_free(hu_allocator_t *alloc, hu_cot_audit_result_t *result) {
    if (!alloc || !result) return;
    if (result->reason) { alloc->free(alloc->ctx, result->reason, result->reason_len + 1); result->reason = NULL; }
}
