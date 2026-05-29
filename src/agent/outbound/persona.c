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
#include "human/core/string.h"
#include "human/eval/shape.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Sprint 60 — shape-classifier REGENERATE conditions.
 *
 * The classifier returns hu_shape_result_t with .score, .passed, and
 * .fail_flags. We REGENERATE when EITHER of two conditions fires:
 *
 *   (a) !shape.passed
 *       Catches structural failures: bullet/numbered lists on
 *       no-markdown channels (BULLET_LIST/NUMBERED_LIST/HEADER/
 *       CODE_FENCE flags), way-too-long content, or score < 0.7.
 *       This is the classifier's own definition of "fail."
 *
 *   (b) shape.fail_flags & HU_PERSONA_SHAPE_AI_OPENER_MASK
 *       Catches single AI-assistant openers that don't drag the
 *       score below the passed threshold on their own ("Certainly!"
 *       deducts only 0.15, leaving 0.85 — passed=true). These
 *       openers are individually mild for the eval scoring scheme
 *       but unambiguous out-of-voice signals in production.
 *
 * Why not just lower the score threshold? Single AI openers score
 * 0.85; lowering the threshold to 0.85 would false-positive on a
 * variety of legitimate borderline content. Targeting the flag
 * bits directly catches the unambiguous case without collateral.
 *
 * Per Q-3 user decision: REGENERATE only (never REJECT). The LLM
 * gets one chance to rewrite. */
#define HU_PERSONA_SHAPE_AI_OPENER_MASK                                              \
    (HU_SHAPE_FAIL_DEPENDING_ON | HU_SHAPE_FAIL_HERE_ARE | HU_SHAPE_FAIL_CERTAINLY | \
     HU_SHAPE_FAIL_ABSOLUTELY | HU_SHAPE_FAIL_GREAT_QUESTION | HU_SHAPE_FAIL_I_UNDERSTAND)

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
static hu_outbound_verdict_t persona_run(hu_outbound_pipeline_stage_t *self, hu_outbound_message_t *msg,
                                         hu_outbound_context_t *ctx) {
    (void)self;
    (void)ctx;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();

    for (size_t i = 0; PROJECT_JARGON[i]; i++) {
        if (hu_str_contains_ci_cstr(msg->content, msg->content_len, PROJECT_JARGON[i])) {
            return hu_outbound_verdict_regenerate(
                "persona_project_jargon",
                "Skip the project talk. Reply like Seth would to family — "
                "casual, brief, no technical jargon.");
        }
    }

    for (size_t i = 0; AI_SELF_AWARE[i]; i++) {
        if (hu_str_contains_ci_cstr(msg->content, msg->content_len, AI_SELF_AWARE[i])) {
            return hu_outbound_verdict_regenerate(
                "persona_ai_self_aware",
                "Don't narrate yourself. Write what Seth would say, not what "
                "an assistant would say about itself.");
        }
    }

    /* Sprint 60 — deterministic shape classifier gate. The heuristics
     * above catch known DOMAIN failure modes (project jargon, AI
     * self-narration). hu_shape_classify catches STRUCTURAL failure
     * modes (length, markdown, AI-assistant openers like "Certainly!"
     * / "Great question!") with channel-aware rules.
     *
     * Channel resolution: ctx->channel_name is the iMessage/Slack/etc
     * string; NULL falls back to iMessage (strictest) via the
     * classifier's own default. Empty content also defers to the
     * classifier — its NULL/EMPTY_RESPONSE flags surface there.
     *
     * Threshold per design.md Q-3 (see header comment for rationale). */
    hu_shape_channel_t channel = hu_shape_channel_from_string(
        ctx ? ctx->channel_name : NULL, (ctx && ctx->channel_name) ? strlen(ctx->channel_name) : 0);
    hu_shape_result_t shape = {0};
    if (hu_shape_classify(msg->content, msg->content_len, channel, &shape) == HU_OK &&
        (!shape.passed || (shape.fail_flags & HU_PERSONA_SHAPE_AI_OPENER_MASK))) {
        return hu_outbound_verdict_regenerate(
            "persona_shape_off_voice",
            "Reply doesn't match Seth's voice for this channel. Strip the "
            "AI-assistant openers ('Certainly', 'Great question'), the bullet "
            "lists, and the boilerplate. Reply how Seth would: short, casual, "
            "no headers, no markdown.");
    }

    return hu_outbound_verdict_send();
}

hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_persona = {
    .name = "persona",
    .run = persona_run,
    .state = NULL,
};
