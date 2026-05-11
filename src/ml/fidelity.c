#include "human/ml/fidelity.h"
#include "human/persona.h"
#include <string.h>

/* Track D D2.2 — persona-fidelity computation primitives.
 *
 * Both `human ml fidelity-status` and the gateway's
 * `metrics.fidelity` method delegate the math here so the
 * dashboard tile, the CLI output, and any future telemetry
 * agree on a single definition of "baseline mean". The two
 * surfaces only differ in *where* the resulting JSON gets
 * delivered and *how* the persona name is resolved. */

hu_error_t hu_ml_fidelity_resolve_target(hu_allocator_t *alloc,
                                         hu_communication_style_t *out_target,
                                         bool *out_synthetic) {
    (void)alloc;
    if (!out_target || !out_synthetic)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_target, 0, sizeof(*out_target));
    *out_synthetic = true;

    /* Try to load the user's accumulated communication-style
     * fingerprint from `~/.human/personal_model.bin`. A zero-sample
     * style is treated as "no real data yet" and falls through. */
    char pm_path[1024];
    if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
        hu_personal_model_t loaded;
        if (hu_personal_model_load(&loaded, pm_path) == HU_OK &&
            loaded.style.sample_count > 0U) {
            *out_target = loaded.style;
            *out_synthetic = false;
            return HU_OK;
        }
    }

    /* Synthetic fallback — same defaults as `human ml lora-baseline`
     * so the two surfaces stay numerically comparable. Any change here
     * MUST be mirrored in `lora-baseline`'s synthetic path or the
     * `check-lora-baseline.sh` gate will diverge from `metrics.fidelity`. */
    out_target->formality = 0.3f;
    out_target->verbosity = 0.5f;
    out_target->emoji_frequency = 0.2f;
    out_target->humor_receptivity = 0.6f;
    out_target->lowercase_ratio = 0.85f;
    out_target->abbreviation_ratio = 0.2f;
    out_target->avg_message_length = 60;
    out_target->sample_count = 1;
    return HU_OK;
}

hu_error_t hu_ml_fidelity_score_baseline(const hu_persona_t *persona,
                                         const hu_communication_style_t *target,
                                         hu_communication_style_set_summary_t *out_summary) {
    if (!persona || !target || !out_summary)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0U)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->min_score = 1.f;

    size_t scored = 0;
    size_t skipped = 0;
    float sum = 0.f;
    float mn = 1.f;
    float mx = 0.f;
    for (size_t b = 0; b < persona->example_banks_count; b++) {
        const hu_persona_example_bank_t *bank = &persona->example_banks[b];
        for (size_t i = 0; i < bank->examples_count; i++) {
            const char *r = bank->examples[i].response;
            if (!r || !r[0]) {
                skipped++;
                continue;
            }
            float s = hu_communication_style_fidelity_score(target, r, strlen(r));
            if (s < 0.f) {
                skipped++;
                continue;
            }
            sum += s;
            if (s < mn) mn = s;
            if (s > mx) mx = s;
            scored++;
        }
    }
    out_summary->scored = scored;
    out_summary->skipped = skipped;
    if (scored == 0) {
        out_summary->mean = 0.f;
        out_summary->min_score = 0.f;
        out_summary->max_score = 0.f;
    } else {
        out_summary->mean = sum / (float)scored;
        out_summary->min_score = mn;
        out_summary->max_score = mx;
    }
    return HU_OK;
}
