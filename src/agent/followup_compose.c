/* Persona-composed follow-up nudges.
 * See include/human/agent/followup_compose.h for the full contract.
 *
 * Implementation notes:
 *   - Gate parse delegates to hu_gate_mode_from_env (canonical off|shadow|live
 *     vocabulary); a test seam can force a mode without touching the env.
 *   - The provider call follows the retrieval_planner_llm.c seam pattern: an
 *     injectable fn pointer for tests, and the real chat_with_system call
 *     compiled out entirely under HU_IS_TEST so the test binary can never
 *     reach a provider.
 *   - compose_finalize() is shared by BOTH the injected and production paths,
 *     so the guard audit, the single-line rule and the length cap are pinned
 *     by tests against the exact bytes production runs. */
#include "human/agent/followup_compose.h"
#include "human/agent/response_guard.h"

#include <stdio.h>
#include <string.h>

/* ── Gate ──────────────────────────────────────────────────────────────── */

static int s_mode_override = -1; /* -1 = defer to env */

hu_gate_mode_t hu_followup_compose_mode(void) {
    if (s_mode_override >= 0)
        return (hu_gate_mode_t)s_mode_override;
    return hu_gate_mode_from_env("HU_FOLLOWUP_COMPOSE", HU_GATE_OFF);
}

void hu_followup_compose_set_mode_for_test(int mode) {
    s_mode_override = mode;
}

/* ── Directive builder (pure) ──────────────────────────────────────────── */

size_t hu_followup_compose_directive(const char *contact_id, hu_followup_warmth_t warmth,
                                     unsigned read_age_hours, const char *channel, char *out,
                                     size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!contact_id || !contact_id[0] || !channel || !channel[0])
        return 0;

    const char *relationship;
    switch (warmth) {
    case HU_FOLLOWUP_WARMTH_CLOSE:
        relationship = "someone you're close to";
        break;
    case HU_FOLLOWUP_WARMTH_FRIEND:
        relationship = "a friend";
        break;
    case HU_FOLLOWUP_WARMTH_NONE:
    default:
        return 0; /* no-follow-up tiers never compose */
    }

    /* The persona prompt supplies the voice; this supplies only the situation.
     * "however you'd actually say it" + the explicit no-stock-phrasing clause
     * are load-bearing: without them the model converges on its own template
     * ("just checking in") across every contact, which is the same tell as the
     * hardcoded strings this replaces, only one layer down. Naming the elapsed
     * time lets the nudge scale — a 2-hour bump and a 2-day bump are not the
     * same sentence from a real person. */
    int n =
        snprintf(out, cap,
                 "%s (%s) read your last message on %s about %u hour%s ago and hasn't "
                 "replied yet. Nudge them — however you'd actually say it, this time, to "
                 "this person. Keep it to one short line under 120 characters. Don't use "
                 "stock check-in phrasing. Output only the message text, nothing else.",
                 contact_id, relationship, channel, read_age_hours, read_age_hours == 1 ? "" : "s");

    /* Refuse on encoding error or truncation — a clipped directive would
     * silently drop the trailing output-format instruction, and the model
     * would then narrate instead of emitting a bare line. */
    if (n > 0 && (size_t)n < cap)
        return (size_t)n;
    out[0] = '\0';
    return 0;
}

/* ── Finalize: trim + guard-audit the raw model text ───────────────────── */

/* Trim surrounding ASCII whitespace from the span in place. */
static void trim_span(const char **s, size_t *len) {
    while (*len > 0 && (unsigned char)(*s)[0] <= 32) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && (unsigned char)(*s)[*len - 1] <= 32)
        (*len)--;
}

/* Shared by the injected-fn and production paths. Rejects when the text is
 * empty after trimming, multi-line (deliberation / candidate-list shape), over
 * cap, or trips the outbound audit guards. A rejection means SEND NOTHING at
 * LIVE — never a template substitution. */
static hu_error_t compose_finalize(const char *raw, size_t raw_len, char *out, size_t cap) {
    trim_span(&raw, &raw_len);
    /* Models often wrap a requested one-liner in quotes; strip ONE pair. */
    if (raw_len >= 2 && raw[0] == '"' && raw[raw_len - 1] == '"') {
        raw++;
        raw_len -= 2;
        trim_span(&raw, &raw_len);
    }
    if (raw_len == 0 || raw_len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < raw_len; i++) {
        if (raw[i] == '\n' || raw[i] == '\r')
            return HU_ERR_INVALID_ARGUMENT; /* multi-line = candidate-list / deliberation shape */
    }
    /* The proactive path never crosses hu_response_guard_check, so the
     * outbound audit runs here (2026-07-12 deliberation-leak incident). */
    if (hu_guard_audit_self_talk_leak(raw, raw_len) ||
        hu_guard_audit_deliberation_leak(raw, raw_len))
        return HU_ERR_INVALID_ARGUMENT;

    memcpy(out, raw, raw_len);
    out[raw_len] = '\0';
    return HU_OK;
}

/* ── Compose ───────────────────────────────────────────────────────────── */

static hu_followup_compose_llm_fn_t s_test_llm = NULL;
static void *s_test_llm_ctx = NULL;

void hu_followup_compose_set_llm_for_test(hu_followup_compose_llm_fn_t fn, void *ctx) {
    s_test_llm = fn;
    s_test_llm_ctx = ctx;
}

hu_error_t hu_followup_compose_text(hu_allocator_t *alloc, const hu_persona_t *persona,
                                    hu_provider_t *provider, const char *channel,
                                    const char *directive, char *out, size_t cap) {
    if (!out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    out[0] = '\0';
    if (!directive || !directive[0])
        return HU_ERR_INVALID_ARGUMENT;

    if (s_test_llm) {
        char raw[HU_FOLLOWUP_COMPOSE_MAX * 2];
        raw[0] = '\0';
        hu_error_t err =
            s_test_llm(s_test_llm_ctx, "", 0, directive, strlen(directive), raw, sizeof(raw));
        if (err != HU_OK)
            return err;
        return compose_finalize(raw, strlen(raw), out, cap);
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Tests must be deterministic and free of provider I/O. */
    (void)alloc;
    (void)persona;
    (void)provider;
    (void)channel;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!alloc || !persona || !provider || !provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_INVALID_ARGUMENT;

    char *sys = NULL;
    size_t sys_len = 0;
    hu_error_t err = hu_persona_build_prompt(alloc, persona, channel, channel ? strlen(channel) : 0,
                                             NULL, 0, &sys, &sys_len);
    if (err != HU_OK || !sys || sys_len == 0) {
        if (sys)
            alloc->free(alloc->ctx, sys, sys_len + 1);
        return err != HU_OK ? err : HU_ERR_INVALID_ARGUMENT;
    }

    char *resp = NULL;
    size_t resp_len = 0;
    /* Temperature 0.7: a bump wants natural variance, not determinism — a
     * deterministic composer would just be a slower hardcoded string. Model
     * NULL -> provider default (the router's conversational tier). */
    err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, sys_len, directive,
                                             strlen(directive), NULL, 0, 0.7, &resp, &resp_len);
    alloc->free(alloc->ctx, sys, sys_len + 1);
    if (err != HU_OK || !resp || resp_len == 0) {
        if (resp)
            alloc->free(alloc->ctx, resp, resp_len);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }

    err = compose_finalize(resp, resp_len, out, cap);
    alloc->free(alloc->ctx, resp, resp_len);
    return err;
#endif
}

/* ── Final-text pick (pure) ────────────────────────────────────────────── */

const char *hu_followup_compose_pick(hu_gate_mode_t mode, hu_error_t compose_err,
                                     const char *composed, const char *template_text) {
    if (mode != HU_GATE_LIVE)
        return template_text; /* OFF / SHADOW: unchanged behavior */
    if (compose_err == HU_OK && composed && composed[0])
        return composed;
    /* LIVE and composition failed. Deliberately NOT template_text: falling
     * back would put the hardcoded string this module exists to remove in
     * front of a real contact, exactly when the composer is unhealthy and
     * nobody is watching. Skipping a bump costs nothing that matters. */
    return NULL;
}
