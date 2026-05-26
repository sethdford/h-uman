/* outbound/moderation.c — violence/hate/self-harm/PII gate.
 *
 * Wraps the existing local moderation primitive `hu_moderation_check`
 * with verdict semantics appropriate for outbound messaging:
 *
 *   - violence flagged   → REGENERATE with de-escalation hint
 *   - hate flagged       → REGENERATE with de-escalation hint
 *   - self_harm flagged  → SEND (crisis-hotline routing lives upstream
 *                          in agent_turn.c; do NOT block the user from
 *                          reaching a struggling contact)
 *   - sexual flagged     → REGENERATE
 *
 * Plus a CHEAP PII pattern check that runs first (no LLM call needed):
 *
 *   - SSN-shape pattern  → REJECT  (NNN-NN-NNNN)
 *   - Credit-card-shape  → REJECT  (16 contiguous digits, Luhn-ish)
 *
 * PII is REJECT (not REGENERATE) because the LLM was likely instructed
 * to NOT include such data; if it appears, regeneration is unlikely to
 * fix it (root cause is upstream prompt/context). REJECT also avoids a
 * costly LLM retry on an inherently-leaky context.
 *
 * Latency budget: PII checks <1ms, moderation_check ~10ms.
 *
 * Per Q-4 user decision: always run for proactive/F25/temporal;
 * scheduled gets a lighter PII-only path. pipeline_configs.c selects.
 *
 * Corpus: no rows directly exercise moderation (the [SAFETY] block in
 * #6 is caught by `shape` first). The contract is pinned by
 * adversarial-test fixtures rather than corpus rows.
 *
 * Returns:
 *   SEND        — clean
 *   REGENERATE  — violence/hate/sexual flagged
 *   REJECT      — PII pattern detected (and never recoverable)
 */

#include "human/agent/outbound_pipeline.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "human/security/moderation.h"

/* SSN pattern: NNN-NN-NNNN with dashes (strictest form to avoid
 * false positives on phone numbers or area codes). */
static int has_ssn_pattern(const char *s, size_t len) {
    for (size_t i = 0; i + 11 <= len; i++) {
        if (isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i + 1]) &&
            isdigit((unsigned char)s[i + 2]) && s[i + 3] == '-' &&
            isdigit((unsigned char)s[i + 4]) && isdigit((unsigned char)s[i + 5]) &&
            s[i + 6] == '-' && isdigit((unsigned char)s[i + 7]) &&
            isdigit((unsigned char)s[i + 8]) && isdigit((unsigned char)s[i + 9]) &&
            isdigit((unsigned char)s[i + 10])) {
            /* Bound check — must not be inside a longer digit run
             * (else it's a partial of something else). */
            int left_ok = (i == 0) || !isdigit((unsigned char)s[i - 1]);
            int right_ok = (i + 11 == len) || !isdigit((unsigned char)s[i + 11]);
            if (left_ok && right_ok)
                return 1;
        }
    }
    return 0;
}

/* Credit-card-shape: 13-19 contiguous digits, ignoring spaces and
 * dashes between groups. This is the SHAPE check; a Luhn validator
 * could be added for fewer false positives, but the shape alone is
 * a strong PII signal. */
static int has_cc_pattern(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        /* Find start of a digit run. */
        while (i < len && !isdigit((unsigned char)s[i]))
            i++;
        if (i >= len)
            break;
        size_t digits = 0;
        size_t j = i;
        while (j < len) {
            if (isdigit((unsigned char)s[j])) {
                digits++;
                j++;
            } else if (s[j] == ' ' || s[j] == '-') {
                /* Group separators allowed; only advance. */
                j++;
            } else {
                break;
            }
        }
        if (digits >= 13 && digits <= 19)
            return 1;
        i = j;
    }
    return 0;
}

static hu_outbound_verdict_t moderation_run(hu_outbound_pipeline_stage_t *self, hu_outbound_message_t *msg,
                                            hu_outbound_context_t *ctx) {
    (void)self;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();
    if (!ctx || !ctx->alloc)
        return hu_outbound_verdict_send();

    /* PII checks first — cheap and definitive. */
    if (has_ssn_pattern(msg->content, msg->content_len)) {
        return hu_outbound_verdict_reject("moderation_pii_ssn");
    }
    if (has_cc_pattern(msg->content, msg->content_len)) {
        return hu_outbound_verdict_reject("moderation_pii_cc");
    }

    /* Local moderation classifier. */
    hu_moderation_result_t result = {0};
    if (hu_moderation_check(ctx->alloc, msg->content, msg->content_len, &result) != HU_OK) {
        /* If moderation can't run, fail open — better to send than
         * silently drop legitimate messages on infra failure. */
        return hu_outbound_verdict_send();
    }
    if (!result.flagged)
        return hu_outbound_verdict_send();

    /* self_harm is intentionally NOT blocked — crisis-routing lives
     * upstream in agent_turn.c (the canned-decline replacement landed
     * in band-aid commit 4ba65b6b). Self-harm content in an outbound
     * message could be Seth checking on a struggling friend — don't
     * block reach. */
    if (result.self_harm && !result.violence && !result.hate && !result.sexual) {
        return hu_outbound_verdict_send();
    }

    if (result.violence || result.hate) {
        return hu_outbound_verdict_regenerate(
            "moderation_violence_hate",
            "Avoid endorsing harm. De-escalate. Acknowledge feelings "
            "without amplifying. Redirect to constructive alternatives.");
    }
    if (result.sexual) {
        return hu_outbound_verdict_regenerate(
            "moderation_sexual", "Keep it appropriate for the recipient relationship.");
    }

    /* Flagged but no specific category matched — REGENERATE conservatively. */
    return hu_outbound_verdict_regenerate("moderation_flagged",
                                          "Rewrite to be appropriate for this contact.");
}

hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_moderation = {
    .name = "moderation",
    .run = moderation_run,
    .state = NULL,
};
