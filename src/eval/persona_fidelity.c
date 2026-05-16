/* persona_fidelity — multi-turn composite fidelity scoring.
 *
 * See include/human/eval/persona_fidelity.h for the contract. This file
 * composes existing primitives (no new math beyond mean/stderr); the
 * intent is to make the composite measurable as a single number that a
 * CI gate or A/B comparator can act on.
 */

#include "human/eval/persona_fidelity.h"
#include "human/eval/consistency.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── Composite weights ────────────────────────────────────────────────
 *
 * Three axes, weights sum to 1.0. Defaults chosen as follows:
 *
 *   style_match (0.50)  — the only axis tied to the user's *learned*
 *                          fingerprint. EWMA over every observed user
 *                          message, so this is what "sounds like the
 *                          user" actually means in our system today.
 *   trait_coverage (0.30) — references persona traits + preferred vocab,
 *                          penalizes avoided vocab. Tied to the
 *                          *authored* persona, not learned style.
 *   line_consistency (0.20) — turn-to-turn drift. Lowest weight because
 *                          a high score here can come from boring
 *                          repetition, not genuine consistency.
 *
 * TUNING NOTE: these are best-guesses, not measured optima. The
 * canonical pass for retuning is to run check-lora-baseline.sh against
 * tests/fixtures/lora_baseline_persona.json with each weight permuted
 * by ±0.1, pick the configuration that maximizes separation between
 * matched and mismatched persona pairs. Today there is no such study —
 * treat these as a placeholder until the eval corpus is large enough
 * to optimize over. */
static const float HU_PF_W_STYLE = 0.50f;
static const float HU_PF_W_TRAITS = 0.30f;
static const float HU_PF_W_LINE = 0.20f;

static float clamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/* Per-turn composite from the three axes. Caller passes already-clamped
 * axis scores; this is just a weighted sum (no NaN guard — callers must
 * not pass NaN). */
static float turn_composite(float style, float traits, float line) {
    return clamp01(style * HU_PF_W_STYLE + traits * HU_PF_W_TRAITS + line * HU_PF_W_LINE);
}

hu_error_t hu_persona_fidelity_score_l1(const hu_communication_style_t *target,
                                        const char *const *responses, const size_t *lens, size_t n,
                                        const char *const *traits, size_t traits_count,
                                        const char *const *preferred_vocab, size_t preferred_count,
                                        const char *const *avoided_vocab, size_t avoided_count,
                                        hu_persona_fidelity_score_t *out) {

    if (!target || !out || !responses || !lens || n == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    double sum_style = 0.0, sum_traits = 0.0, sum_line = 0.0;
    double sum_composite = 0.0, sum_composite_sq = 0.0;
    float min_c = 1.0f, max_c = 0.0f;

    /* Previous turn for line consistency. Starts NULL — turn 0 always
     * gets a neutral 0.5 line score so it doesn't anchor the mean to
     * an artificial 0. */
    const char *prev = NULL;
    size_t prev_len = 0;

    for (size_t i = 0; i < n; i++) {
        if (!responses[i] || lens[i] == 0) {
            out->turns_skipped++;
            continue;
        }

        float style = hu_communication_style_fidelity_score(target, responses[i], lens[i]);
        if (style < 0.0f) {
            out->turns_skipped++;
            continue;
        }
        style = clamp01(style);

        float t = 0.0f;
        if (traits_count > 0 || preferred_count > 0 || avoided_count > 0) {
            (void)hu_consistency_score_prompt_alignment(responses[i], lens[i], traits, traits_count,
                                                        preferred_vocab, preferred_count,
                                                        avoided_vocab, avoided_count, &t);
        } else {
            /* No persona lexical fingerprint provided — neutral score
             * rather than zero so this axis doesn't drag the composite
             * down for personas authored without traits/vocab arrays. */
            t = 0.5f;
        }
        t = clamp01(t);

        float line = 0.5f;
        if (prev) {
            (void)hu_consistency_score_line(prev, prev_len, responses[i], lens[i], &line);
            line = clamp01(line);
        }

        sum_style += style;
        sum_traits += t;
        sum_line += line;

        float c = turn_composite(style, t, line);
        sum_composite += c;
        sum_composite_sq += (double)c * (double)c;
        if (c < min_c)
            min_c = c;
        if (c > max_c)
            max_c = c;

        prev = responses[i];
        prev_len = lens[i];
        out->turns_scored++;
    }

    if (out->turns_scored == 0) {
        out->min_turn = 0.0f;
        out->max_turn = 0.0f;
        return HU_OK;
    }

    double k = (double)out->turns_scored;
    out->style_match_mean = (float)(sum_style / k);
    out->trait_coverage_mean = (float)(sum_traits / k);
    out->line_consistency_mean = (float)(sum_line / k);
    out->composite = (float)(sum_composite / k);
    out->min_turn = min_c;
    out->max_turn = max_c;

    if (out->turns_scored > 1) {
        double mean = sum_composite / k;
        double var = (sum_composite_sq / k) - (mean * mean);
        if (var < 0.0)
            var = 0.0; /* float drift */
        out->composite_stderr = (float)sqrt(var / k);
    }

    return HU_OK;
}

hu_error_t hu_persona_fidelity_ab_score(const hu_communication_style_t *target,
                                        const char *const *responses_a, const size_t *lens_a,
                                        size_t n_a, const char *const *responses_b,
                                        const size_t *lens_b, size_t n_b, const char *const *traits,
                                        size_t traits_count, const char *const *preferred_vocab,
                                        size_t preferred_count, const char *const *avoided_vocab,
                                        size_t avoided_count, float min_improvement_stderr,
                                        hu_persona_fidelity_ab_t *out) {

    if (!target || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    hu_error_t err_a = hu_persona_fidelity_score_l1(target, responses_a, lens_a, n_a, traits,
                                                    traits_count, preferred_vocab, preferred_count,
                                                    avoided_vocab, avoided_count, &out->set_a);
    if (err_a != HU_OK)
        return err_a;

    hu_error_t err_b = hu_persona_fidelity_score_l1(target, responses_b, lens_b, n_b, traits,
                                                    traits_count, preferred_vocab, preferred_count,
                                                    avoided_vocab, avoided_count, &out->set_b);
    if (err_b != HU_OK)
        return err_b;

    out->delta = out->set_b.composite - out->set_a.composite;

    /* Combined stderr for the delta: sqrt(σ_a^2 + σ_b^2). Treats the two
     * sets as independent samples — which they are when comparing
     * pre-LoRA vs post-LoRA (different model weights produce
     * independent outputs). */
    double sa = (double)out->set_a.composite_stderr;
    double sb = (double)out->set_b.composite_stderr;
    out->delta_stderr = (float)sqrt(sa * sa + sb * sb);

    bool both_scored = (out->set_a.turns_scored > 0 && out->set_b.turns_scored > 0);
    if (!both_scored) {
        out->improved = false;
        return HU_OK;
    }

    /* When stderr is 0 (n<=1 on either side), any positive delta wins.
     * That's intentional for the smoke-test path — production gates
     * should require n >= 10 before trusting the verdict. */
    if (out->delta_stderr <= 0.0f)
        out->improved = (out->delta > 0.0f);
    else
        out->improved = (out->delta >= min_improvement_stderr * out->delta_stderr);

    return HU_OK;
}

/* ── L2 — LLM judge wrapper ───────────────────────────────────────────
 *
 * Issues a single hu_eval_judge_check call with a question that asks the
 * judge to score persona fidelity using the supplied rubric. The question
 * is stock; the per-call variance is in `persona_description` (which
 * persona) and `rubric_text` (which axes / weights).
 *
 * The question template is intentionally short — the rubric carries the
 * grading criteria, and the judge model is expected to do the heavy
 * lifting. Pushing the rubric into the question would inflate token
 * count without adding signal. */

#define HU_PF_JUDGE_QUESTION_TEMPLATE                                                  \
    "You are grading whether an assistant response matches a target persona. "         \
    "Persona description:\n%.*s\n\n"                                                   \
    "Rate the response on the rubric's dimensions and give an overall 1-5 score, "     \
    "where 5 = indistinguishable from the persona and 1 = clearly a different voice. " \
    "Reply with the score, then a one-sentence reason."

hu_error_t hu_persona_fidelity_judge(hu_allocator_t *alloc, hu_provider_t *provider,
                                     const char *model, size_t model_len,
                                     const char *persona_description, size_t persona_desc_len,
                                     const char *response, size_t response_len,
                                     const char *rubric_text, size_t rubric_text_len,
                                     int pass_threshold, hu_eval_judge_cache_t *cache,
                                     hu_eval_judge_result_t *out) {

    if (!provider || !out || !rubric_text || rubric_text_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!response || response_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!persona_description || persona_desc_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Cap persona description so the question template doesn't blow past
     * a reasonable budget. 1024 bytes is enough for a paragraph-shaped
     * persona summary; longer descriptions should be summarized at the
     * call site (and probably belong in the persona file, not the
     * judge call). */
    size_t pd_len = persona_desc_len;
    if (pd_len > 1024)
        pd_len = 1024;

    /* Format the question. The template + persona_description fit in a
     * 2 KB stack buffer with room to spare; if the template grows,
     * promote this to alloc->malloc. */
    char question[2048];
    int written = snprintf(question, sizeof(question), HU_PF_JUDGE_QUESTION_TEMPLATE, (int)pd_len,
                           persona_description);
    if (written < 0 || (size_t)written >= sizeof(question))
        return HU_ERR_INTERNAL;

    /* eval_judge_check requires a non-NULL `expected`. For persona
     * fidelity we don't have a ground-truth answer — we're comparing
     * against the *persona description* itself. Pass the persona desc
     * as expected so the prompt template renders meaningfully
     * ("Expected answer: <persona description>") and the judge knows
     * the target. Truncated to the same 1024-byte cap as above. */
    return hu_eval_judge_check(alloc, provider, model, model_len, question, (size_t)written,
                               response, response_len, persona_description, pd_len, rubric_text,
                               rubric_text_len, pass_threshold, cache, out);
}
