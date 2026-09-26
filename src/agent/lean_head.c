/* Lean persona head. Contract: include/human/agent.h. */
#include "human/agent.h"
#include "human/agent/model_router.h"
#include "human/config.h"
#include "human/core/paths.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/persona/rag.h"
#include <stdio.h>
#include <string.h>

/* Lean persona: identity + output constraint + core_anchor + reinforcement
 * + anti_patterns + style_rules + channel overlay. Moved verbatim out of
 * hu_agent_turn_stream_v2 (2026-09-26) so offline tools can render the exact
 * head the daemon sends on the llm_decides path instead of approximating it. */
hu_error_t hu_agent_build_lean_persona_head(hu_agent_t *agent, const char *msg, size_t msg_len,
                                            char **out, size_t *out_len) {
    if (!agent || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!agent->persona)
        return HU_OK;
    char lp[16384];
    size_t lpo = 0;
    {
        const hu_persona_t *pp = agent->persona;
        if (pp->identity) {
            int n = snprintf(lp + lpo, sizeof(lp) - lpo, "You ARE this person: %s\n", pp->identity);
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
        }
        if (pp->biography) {
            int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s\n", pp->biography);
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
        }
        static const char constraint[] =
            "Output ONLY what this person would actually type — nothing else. "
            "No reasoning, no parentheses, no meta-commentary, no analysis. "
            "Just the raw text message, exactly as it would appear on screen.\n";
        int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s", constraint);
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
        for (size_t ri = 0; ri < pp->communication_rules_count && ri < 12; ri++) {
            if (pp->communication_rules[ri]) {
                n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n", pp->communication_rules[ri]);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
        }
        n = snprintf(lp + lpo, sizeof(lp) - lpo, "\n");
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
    }
    const hu_persona_t *p = agent->persona;
    if (p->core_anchor) {
        int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s\n\n", p->core_anchor);
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
    }
    for (size_t i = 0; i < p->immersive_reinforcement_count && i < 10; i++) {
        if (p->immersive_reinforcement[i]) {
            int n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n", p->immersive_reinforcement[i]);
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
        }
    }
    if (p->anti_patterns_count > 0) {
        int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nNEVER do:\n");
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
        for (size_t i = 0; i < p->anti_patterns_count; i++) {
            if (p->anti_patterns[i]) {
                n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n", p->anti_patterns[i]);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
        }
    }
    if (p->style_rules_count > 0) {
        int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nStyle:\n");
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
        for (size_t i = 0; i < p->style_rules_count; i++) {
            if (p->style_rules[i]) {
                n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n", p->style_rules[i]);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
        }
    }
    /* Add examples to prime the model on correct tone.
     * hu_persona_select_examples writes up to N pointers into out[]
     * (signature: const hu_persona_example_t **out). Previously this
     * passed &exs of a single pointer (1×8 bytes) with capacity 5,
     * which produced a stack-buffer-overflow caught by ASan and a
     * misuse of exs[ei] as an object instead of a pointer below. */
    {
        const hu_persona_example_t *exs[5] = {NULL};
        size_t ex_count = 0;
        hu_persona_select_examples(p, agent->active_channel, agent->active_channel_len, NULL, 0,
                                   exs, &ex_count, 5, &agent->personal_model.style);
        if (ex_count > 0) {
            int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nExamples of how you text:\n");
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
            for (size_t ei = 0; ei < ex_count; ei++) {
                if (exs[ei] && exs[ei]->incoming && exs[ei]->response) {
                    n = snprintf(lp + lpo, sizeof(lp) - lpo, "them: %s\nyou: %s\n\n",
                                 exs[ei]->incoming, exs[ei]->response);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
            }
        }
    }
    /* RAG-over-own-messages voice grounding (default off): retrieve
     * Seth's most-similar real past messages to THIS incoming message and
     * inject them as dynamic few-shot grounding — the SOTA RAG leg next to
     * the fine-tuned adapter + personal model.
     *
     * Register-conditional (live A/B 2026-05-29, rag-ab-live-verdict.json):
     * RAG grounding HELPS the substantive register (+0.110) but slightly
     * hurts casual (-0.078, richer context fights curt brevity). So gate it
     * on ANALYTICAL/DEEP turns only; REFLEXIVE/CONVERSATIONAL and unknown
     * tier (turn_tier < 0) skip it. */
    if (agent->config && agent->config->agent.rag_grounding_enabled &&
        agent->turn_tier >= (int)HU_TIER_ANALYTICAL) {
        char qbuf[512];
        size_t qn = msg_len < sizeof(qbuf) - 1 ? msg_len : sizeof(qbuf) - 1;
        if (msg && qn > 0) {
            memcpy(qbuf, msg, qn);
            qbuf[qn] = '\0';
            char cpath[768];
            int pn = hu_paths_state(cpath, sizeof(cpath), "voice_corpus.jsonl");
            if (pn > 0 && (size_t)pn < sizeof(cpath)) {
                char rag_buf[2048];
                size_t rn = hu_persona_rag_ground_from_file(qbuf, cpath, 3, rag_buf,
                                                            sizeof(rag_buf), agent->alloc);
                if (rn > 0) {
                    int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\n%s", rag_buf);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
            }
        }
    }
    const hu_persona_overlay_t *ov =
        hu_persona_find_overlay(p, agent->active_channel, agent->active_channel_len);
    if (ov) {
        int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nChannel style:");
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
        if (ov->formality) {
            n = snprintf(lp + lpo, sizeof(lp) - lpo, " %s.", ov->formality);
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
        }
        if (ov->avg_length) {
            n = snprintf(lp + lpo, sizeof(lp) - lpo, " Length: %s.", ov->avg_length);
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
        }
        if (ov->emoji_usage) {
            n = snprintf(lp + lpo, sizeof(lp) - lpo, " Emoji: %s.", ov->emoji_usage);
            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                lpo += (size_t)n;
        }
        for (size_t i = 0; i < ov->style_notes_count; i++) {
            if (ov->style_notes[i]) {
                n = snprintf(lp + lpo, sizeof(lp) - lpo, " %s.", ov->style_notes[i]);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
        }
        n = snprintf(lp + lpo, sizeof(lp) - lpo, "\n");
        if (n > 0 && lpo + (size_t)n < sizeof(lp))
            lpo += (size_t)n;
    }
    /* The ABSOLUTE RULES block is no longer appended here. It used to
     * be added ONLY in this lean branch, which left the batch path
     * (all of production) without it. hu_agent_finalize_system_prompt
     * now appends it once, on every path, as the final bytes inside
     * the prompt budget. */
    if (lpo > 0) {
        *out = hu_strndup(agent->alloc, lp, lpo);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = lpo;
    }
    return HU_OK;
}
