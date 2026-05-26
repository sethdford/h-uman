/* outbound/persona.c — Seth-voice fidelity check.
 *
 * Detects content that doesn't match the persona's expected shape
 * for the conversation. Two layered heuristics:
 *
 *   1. PROJECT JARGON detection — corpus #11-16
 *      h-uman is a personal-conversation agent. Project/technical
 *      jargon ("Replay MCP", "LoRA", "adapter", "checkpoint",
 *      "fine-tune", etc.) should not appear in outbound to ANY
 *      contact via the persona surface. If the recipient is a
 *      technical coworker, that conversation belongs in a different
 *      channel (slack/email), not the persona's casual register.
 *
 *      The hardcoded list is intentionally SMALL and stable — it's
 *      the universal-failure set, not a growing blocklist. Words
 *      like "MCP", "LoRA", "GPU", "tensor", "checkpoint", "adapter"
 *      ARE legitimate in technical channels; the persona stage
 *      runs on the persona surface (proactive/F25/temporal), not
 *      on every channel.
 *
 *      → REGENERATE with hint "Skip the project talk."
 *
 *   2. AI-SELF-AWARE phrasing — corpus #17, #18 (BORDERLINE class)
 *      Phrases like "I've been kind of quiet lately", "let me know
 *      if you'd like me to", "as your assistant" don't sound like
 *      Seth talking to family — they sound like an assistant
 *      narrating itself. Catch and regenerate.
 *
 *      → REGENERATE with hint "Don't narrate yourself."
 *
 * Both layers are case-insensitive substring scans. Cheap (<1ms).
 *
 * Per Q-3 user decision: REGENERATE only (never REJECT). False
 * positives shouldn't drop legitimate messages — the LLM can usually
 * fix the shape on retry.
 *
 * Future enhancement: consult ctx->persona->contact_profiles to
 * allow technical terms for technical contacts. Phase D scope.
 *
 * Corpus coverage:
 *   #6     — "[SAFETY] This response touches on violence..." — Caught
 *            by `shape` first (>60 chars + multi-sentence). Backstop
 *            here on the "This response" phrase.
 *   #11-16 — "Replay MCP" jargon to family — REGENERATE
 *   #17-18 — "I've been kind of quiet lately" — REGENERATE
 *   #19-24 — All clean, must remain SEND.
 */

#include "human/agent/outbound_pipeline.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Project / technical jargon. Case-insensitive substring match.
 * Order doesn't matter; we walk the list. */
static const char *const PROJECT_JARGON[] = {
    /* The actual corpus jargon */
    "replay mcp",
    " mcp ",
    " mcp.",
    " mcp,",
    " mcp?",
    " mcp!",
    /* Broader ML jargon — leak indicators */
    "lora",
    "adapter",
    "checkpoint",
    "fine-tune",
    "fine tune",
    "tensor",
    "fidelity score",
    "training run",
    "eval harness",
    NULL,
};

/* AI-self-aware phrases that betray the assistant. */
static const char *const AI_SELF_AWARE[] = {
    "i've been kind of quiet",
    "i've been a little quiet",
    "i've been quiet lately",
    "as your assistant",
    "let me know if you'd like",
    "let me know if you would like",
    "happy to help",
    "this response",
    "the response",
    NULL,
};

/* Case-insensitive substring search. */
static int contains_ci(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen)
        return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t k = 0;
        while (k < nlen &&
               tolower((unsigned char)haystack[i + k]) == tolower((unsigned char)needle[k])) {
            k++;
        }
        if (k == nlen)
            return 1;
    }
    return 0;
}

static hu_outbound_verdict_t persona_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                         hu_outbound_context_t *ctx) {
    (void)self;
    (void)ctx;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();

    for (size_t i = 0; PROJECT_JARGON[i]; i++) {
        if (contains_ci(msg->content, msg->content_len, PROJECT_JARGON[i])) {
            return hu_outbound_verdict_regenerate(
                "persona_project_jargon",
                "Skip the project talk. Reply like Seth would to family — "
                "casual, brief, no technical jargon.");
        }
    }

    for (size_t i = 0; AI_SELF_AWARE[i]; i++) {
        if (contains_ci(msg->content, msg->content_len, AI_SELF_AWARE[i])) {
            return hu_outbound_verdict_regenerate(
                "persona_ai_self_aware",
                "Don't narrate yourself. Write what Seth would say, not what "
                "an assistant would say about itself.");
        }
    }

    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_persona = {
    .name = "persona",
    .run = persona_run,
    .state = NULL,
};
