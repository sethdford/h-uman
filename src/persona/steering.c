/* SOTA-2026 init-01 — persona steering projection + directive renderer.
 *
 * S1 ships the prompt-side path: persona + personal-model state →
 * abstract trait-coefficient vector → human-readable directive
 * snippet injected into the system prompt. The same vector also
 * feeds the optional `hu_provider_vtable_t.apply_steering` hook;
 * S2 wires that into MLX / llama.cpp residual-stream addition.
 *
 * Design doc: docs/plans/2026-05-11-init-01-activation-steering.md.
 *
 * Determinism is load-bearing:
 *   - No clock reads. `now` is passed in.
 *   - No RNG.
 *   - No I/O.
 *   - The projection path does zero heap allocations; only the
 *     directive renderer allocates the result buffer.
 *
 * Boundary discipline: this TU depends only on core utilities,
 * `human/persona.h`, and `human/memory/personal_model.h`. No
 * provider / agent / config coupling — see design doc §3.2.
 */

#include "human/persona/steering.h"

#include "human/memory/personal_model.h"
#include "human/persona.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static float steering_clamp_unit(float x) {
    if (x > 1.0f)
        return 1.0f;
    if (x < -1.0f)
        return -1.0f;
    return x;
}

static int steering_streq_ci(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return *a == 0 && *b == 0;
}

static int steering_str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return 0;
    size_t hl = strlen(haystack);
    size_t nl = strlen(needle);
    if (nl == 0 || nl > hl)
        return 0;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k;
        for (k = 0; k < nl; k++) {
            char a = haystack[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (k == nl)
            return 1;
    }
    return 0;
}

/* ── Persona-string → coefficient lookups ──────────────────────────────────── */
/*
 * Each lookup table is stored as a string→float array. We do
 * substring-CI matches against the longest entry first so
 * "warm and tender" matches before plain "warm". Coefficients are
 * tuned so a "typical" persona phrase lands in [-0.7, +0.7], with
 * extremes saved for explicit boundary words like "cold" /
 * "tender".
 */

typedef struct steering_lookup_entry {
    const char *needle;
    float coef;
} steering_lookup_entry_t;

/* Order: longest / most specific first. Substring match is CI. */
static const steering_lookup_entry_t WARMTH_LOOKUP[] = {
    {"warm and tender", 0.85f},
    {"tender", 0.80f},
    {"affection", 0.75f},
    {"warm", 0.60f},
    {"kind", 0.55f},
    {"playful", 0.45f},
    {"open", 0.35f},
    {"neutral", 0.0f},
    {"reserved", -0.30f},
    {"cool", -0.50f},
    {"distant", -0.60f},
    {"cold", -0.80f},
    {NULL, 0.0f},
};

static const steering_lookup_entry_t FORMALITY_LOOKUP[] = {
    {"highly formal", 0.85f},
    {"formal", 0.50f},
    {"professional", 0.40f},
    {"neutral", 0.0f},
    {"relaxed", -0.30f},
    {"casual", -0.50f},
    {"informal", -0.55f},
    {"playful", -0.60f},
    {NULL, 0.0f},
};

static const steering_lookup_entry_t HUMOR_FREQ_LOOKUP[] = {
    {"constant", 0.70f},
    {"frequent", 0.50f},
    {"often", 0.40f},
    {"occasional", 0.20f},
    {"rare", -0.30f},
    {"never", -0.70f},
    {NULL, 0.0f},
};

static const steering_lookup_entry_t CONFRONT_COMFORT_LOOKUP[] = {
    {"confrontational", 0.65f},
    {"direct", 0.60f},
    {"assertive", 0.45f},
    {"balanced", 0.0f},
    {"diplomatic", -0.35f},
    {"avoidant", -0.60f},
    {"conflict-averse", -0.70f},
    {NULL, 0.0f},
};

static float steering_lookup(const steering_lookup_entry_t *table, const char *s) {
    if (!table || !s)
        return 0.0f;
    for (size_t i = 0; table[i].needle; i++) {
        if (steering_streq_ci(s, table[i].needle))
            return table[i].coef;
    }
    /* Fallback: substring CI search, longest entry first (the
     * table is ordered specific→generic above). */
    for (size_t i = 0; table[i].needle; i++) {
        if (steering_str_contains_ci(s, table[i].needle))
            return table[i].coef;
    }
    return 0.0f;
}

/* ── Persona projection helpers ─────────────────────────────────────────────── */

static float persona_warmth_coef(const struct hu_persona *p) {
    if (!p)
        return 0.0f;
    /* Weighted sum across the three "warmth signal" persona fields.
     * Weights sum to 1.0 so a max-everything input lands at the
     * source coefficient (then we add humor-receptivity later). */
    float sum = 0.0f;
    sum += 0.45f * steering_lookup(WARMTH_LOOKUP, p->emotional_range.ceiling);
    sum += 0.35f * steering_lookup(WARMTH_LOOKUP, p->emotional_range.floor);
    sum += 0.20f * steering_lookup(WARMTH_LOOKUP, p->core_anchor);
    return sum;
}

static float persona_formality_overlay_coef(const struct hu_persona *p) {
    if (!p || !p->overlays || p->overlays_count == 0)
        return 0.0f;
    /* Use the first overlay's formality as the default channel
     * register. The agent_turn caller will pass a per-channel
     * persona in a future sprint; for S1 the first overlay is
     * the right "default register" reading. */
    return steering_lookup(FORMALITY_LOOKUP, p->overlays[0].formality);
}

static float persona_humor_freq_coef(const struct hu_persona *p) {
    if (!p)
        return 0.0f;
    return steering_lookup(HUMOR_FREQ_LOOKUP, p->humor.frequency);
}

static float persona_confront_coef(const struct hu_persona *p) {
    if (!p)
        return 0.0f;
    return steering_lookup(CONFRONT_COMFORT_LOOKUP, p->conflict_style.confrontation_comfort);
}

/* ── Public API: vector projection ────────────────────────────────────────── */

hu_error_t hu_persona_steering_vector(const struct hu_persona *p,
                                      const struct hu_personal_model *m,
                                      long long now,
                                      float *out,
                                      size_t dim) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    /* Hard contract: callers pass exactly HU_STEERING_VEC_DIM.
     * This keeps the binary format wire-stable so future on-device
     * providers (init-04) can read newer vectors without resizing
     * their SAE-decoder tables. */
    if (dim != HU_STEERING_VEC_DIM)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < dim; i++)
        out[i] = 0.0f;

    /* Persona-derived axes (0..3). Not freshness-gated. */
    float warmth_persona  = persona_warmth_coef(p);
    float formal_overlay  = persona_formality_overlay_coef(p);
    float humor_persona   = persona_humor_freq_coef(p);
    float confront        = persona_confront_coef(p);

    /* Style-derived contributions, gated by freshness so a year-old
     * personal-model snapshot decays toward zero. */
    float style_freshness = 0.0f;
    float style_formality = 0.0f, style_verbosity = 0.0f, style_emoji = 0.0f;
    float style_humor_recept = 0.0f, style_lowercase = 0.0f, style_abbrev = 0.0f;
    int style_present = 0;

    if (m && m->style.last_observed_at > 0) {
        style_freshness = hu_personal_communication_style_freshness(&m->style, (int64_t)now);
        if (style_freshness > 0.0f) {
            style_present = 1;
            /* Center 0..1 percentages on 0 and multiply by 2 so a
             * max signal (1.0) → +1.0 deviation, neutral 0.5 → 0,
             * absent 0.0 → -1.0. Freshness scales the deviation. */
            style_formality    = (m->style.formality        - 0.5f) * 2.0f * style_freshness;
            style_verbosity    = (m->style.verbosity        - 0.5f) * 2.0f * style_freshness;
            style_emoji        = (m->style.emoji_frequency  - 0.5f) * 2.0f * style_freshness;
            style_humor_recept = (m->style.humor_receptivity - 0.5f) * 2.0f * style_freshness;
            style_lowercase    = (m->style.lowercase_ratio  - 0.5f) * 2.0f * style_freshness;
            style_abbrev       = (m->style.abbreviation_ratio - 0.5f) * 2.0f * style_freshness;
        }
    }

    /* WARMTH: persona signals plus a modest humor-receptivity bump.
     * Humor-receptivity tells us the user welcomes warmth-adjacent
     * playfulness; weight it lighter than the persona phrase. */
    out[HU_STEERING_AXIS_WARMTH] =
        steering_clamp_unit(warmth_persona + 0.30f * style_humor_recept);

    /* FORMALITY: overlay register plus style fingerprint. */
    out[HU_STEERING_AXIS_FORMALITY] =
        steering_clamp_unit(formal_overlay + 0.50f * style_formality);

    /* HUMOR_DENSITY: stated humor frequency plus observed
     * receptivity to humor. */
    out[HU_STEERING_AXIS_HUMOR_DENSITY] =
        steering_clamp_unit(humor_persona + 0.30f * style_humor_recept);

    /* HEDGING: inverted confrontation_comfort. A "direct" persona
     * (+0.6) hedges less (-0.6). No style component — the personal
     * model doesn't yet observe hedging frequency directly. */
    out[HU_STEERING_AXIS_HEDGING] = steering_clamp_unit(-1.0f * confront);

    /* VERBOSITY: style only. avg_message_length acts as a gentle
     * sanity gate — when we've seen at least ~40 chars on average
     * the verbosity signal counts, otherwise we don't trust it. */
    if (style_present && m->style.avg_message_length >= 40)
        out[HU_STEERING_AXIS_VERBOSITY] = steering_clamp_unit(style_verbosity);

    /* EMOJI / LOWERCASE / ABBREVIATION: pure style fingerprints. */
    if (style_present) {
        out[HU_STEERING_AXIS_EMOJI_AFFINITY]         = steering_clamp_unit(style_emoji);
        out[HU_STEERING_AXIS_LOWERCASE_AFFINITY]     = steering_clamp_unit(style_lowercase);
        out[HU_STEERING_AXIS_ABBREVIATION_AFFINITY]  = steering_clamp_unit(style_abbrev);
    }

    return HU_OK;
}

/* ── Directive renderer ───────────────────────────────────────────────────── */

/* Per-axis sentence templates. Indexed by axis; each entry has a
 * positive-direction phrase and a negative-direction phrase. The
 * intensity adverb is interpolated by the renderer (slot %s). */
typedef struct steering_axis_phrase {
    const char *positive; /* used when effective coef > 0 */
    const char *negative; /* used when effective coef < 0 */
} steering_axis_phrase_t;

static const steering_axis_phrase_t AXIS_PHRASES[HU_STEERING_AXIS__NAMED_COUNT] = {
    [HU_STEERING_AXIS_WARMTH] = {
        .positive = "Lean %s warmer in tone.",
        .negative = "Lean %s more reserved in tone.",
    },
    [HU_STEERING_AXIS_FORMALITY] = {
        .positive = "Speak %s more formally.",
        .negative = "Keep things %s casual.",
    },
    [HU_STEERING_AXIS_HUMOR_DENSITY] = {
        .positive = "Bring %s more humor into the reply.",
        .negative = "Keep humor %s dialed back.",
    },
    [HU_STEERING_AXIS_HEDGING] = {
        .positive = "Hedge %s more when expressing uncertainty.",
        .negative = "Project %s more confidence in your wording.",
    },
    [HU_STEERING_AXIS_VERBOSITY] = {
        .positive = "Stretch %s longer in your reply.",
        .negative = "Keep replies %s shorter.",
    },
    [HU_STEERING_AXIS_EMOJI_AFFINITY] = {
        .positive = "Sprinkle %s more emoji when it fits.",
        .negative = "Use %s fewer emoji.",
    },
    [HU_STEERING_AXIS_LOWERCASE_AFFINITY] = {
        .positive = "Lean %s into lowercase casing.",
        .negative = "Capitalize %s more conventionally.",
    },
    [HU_STEERING_AXIS_ABBREVIATION_AFFINITY] = {
        .positive = "Use %s more chat abbreviations (e.g. 'u', 'btw').",
        .negative = "Spell %s out instead of abbreviating.",
    },
};

static const char *steering_intensity_label(float magnitude) {
    float m = magnitude < 0 ? -magnitude : magnitude;
    if (m >= 0.70f)
        return "strongly";
    if (m >= 0.40f)
        return "moderately";
    return "slightly";
}

/* A small per-axis record we sort by descending |effective coef|
 * so the strongest signal appears first in the rendered directive. */
typedef struct steering_axis_record {
    hu_steering_axis_t axis;
    float effective;
} steering_axis_record_t;

hu_error_t hu_persona_steering_directive(hu_allocator_t *alloc,
                                         const float *vec,
                                         size_t dim,
                                         float boost,
                                         char **out,
                                         size_t *out_len) {
    if (!alloc || !vec || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (dim != HU_STEERING_VEC_DIM)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    /* Collect named axes whose boosted magnitude crosses the floor. */
    steering_axis_record_t records[HU_STEERING_AXIS__NAMED_COUNT];
    size_t record_count = 0;
    for (size_t i = 0; i < HU_STEERING_AXIS__NAMED_COUNT; i++) {
        float eff = vec[i] * boost;
        eff = steering_clamp_unit(eff);
        float mag = eff < 0 ? -eff : eff;
        if (mag >= HU_STEERING_DIRECTIVE_FLOOR) {
            records[record_count].axis = (hu_steering_axis_t)i;
            records[record_count].effective = eff;
            record_count++;
        }
    }

    if (record_count == 0)
        return HU_OK; /* quiet directive — no axis above floor */

    /* Insertion sort by |effective| descending. Stable; small N
     * (≤ HU_STEERING_AXIS__NAMED_COUNT = 8) so O(N²) is fine. */
    for (size_t i = 1; i < record_count; i++) {
        steering_axis_record_t key = records[i];
        float key_mag = key.effective < 0 ? -key.effective : key.effective;
        size_t j = i;
        while (j > 0) {
            float left_mag = records[j - 1].effective < 0 ? -records[j - 1].effective
                                                          : records[j - 1].effective;
            if (left_mag >= key_mag)
                break;
            records[j] = records[j - 1];
            j--;
        }
        records[j] = key;
    }

    /* Render. Two-pass: measure to size, then snprintf into the
     * allocation. The renderer is deterministic so both passes
     * see identical inputs. */
    static const char HEADER[] = "## Persona steering\n";
    size_t needed = sizeof(HEADER) - 1;

    /* Each line is at most ~96 chars; keep a generous per-axis
     * upper bound so we can size with one snprintf-to-NULL pass
     * rather than two scans. */
    char lines[HU_STEERING_AXIS__NAMED_COUNT][160];
    for (size_t i = 0; i < record_count; i++) {
        const steering_axis_phrase_t *phrase = &AXIS_PHRASES[records[i].axis];
        const char *adverb = steering_intensity_label(records[i].effective);
        const char *tmpl = records[i].effective >= 0 ? phrase->positive : phrase->negative;
        int n = snprintf(lines[i], sizeof(lines[i]), tmpl, adverb);
        if (n < 0)
            return HU_ERR_INVALID_ARGUMENT;
        if ((size_t)n >= sizeof(lines[i]))
            n = (int)sizeof(lines[i]) - 1;
        lines[i][n] = '\0';
        /* +1 for the trailing newline we append between sentences. */
        needed += (size_t)n + 1;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, needed + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    memcpy(buf + pos, HEADER, sizeof(HEADER) - 1);
    pos += sizeof(HEADER) - 1;
    for (size_t i = 0; i < record_count; i++) {
        size_t line_len = strlen(lines[i]);
        memcpy(buf + pos, lines[i], line_len);
        pos += line_len;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

const char *hu_persona_steering_axis_label(hu_steering_axis_t axis) {
    switch (axis) {
    case HU_STEERING_AXIS_WARMTH:
        return "warmth";
    case HU_STEERING_AXIS_FORMALITY:
        return "formality";
    case HU_STEERING_AXIS_HUMOR_DENSITY:
        return "humor_density";
    case HU_STEERING_AXIS_HEDGING:
        return "hedging";
    case HU_STEERING_AXIS_VERBOSITY:
        return "verbosity";
    case HU_STEERING_AXIS_EMOJI_AFFINITY:
        return "emoji_affinity";
    case HU_STEERING_AXIS_LOWERCASE_AFFINITY:
        return "lowercase_affinity";
    case HU_STEERING_AXIS_ABBREVIATION_AFFINITY:
        return "abbreviation_affinity";
    case HU_STEERING_AXIS__NAMED_COUNT:
    default:
        return "unknown";
    }
}
