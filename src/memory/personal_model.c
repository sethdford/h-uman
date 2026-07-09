#include "human/memory/personal_model.h"
#include "human/core/log.h"
#include "human/memory/anticipatory.h"
#include "human/memory/fact_extract_llm.h" /* LLM fact-extraction fallback (casual-text recall) */
#include "human/memory/causal_attribution.h"
#include "human/memory/emotional_context.h"
#include "human/memory/identity_continuity.h"
#include "human/memory/identity_resolver.h" /* hu_identity_graph_t for the setter borrow */
#include "human/memory/minja_guard.h"
#include "human/persona.h"
#include "human/persona/social_insights.h"
#include "human/persona/style_adapter.h"
#include "human/platform.h"
#include "human/reflection.h" /* T7 — reflection slice for build_prompt_with_reflection */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Schema version — kept near the top so `hu_personal_model_init` can
 * stamp freshly-initialized models at the current version. The
 * persistence section re-uses this constant for the on-disk header.
 *
 * SOTA-2026 init-09: v5 adds `hu_provenance_t` per fact + pending-facts
 * quarantine queue. On-disk struct size grew; the existing version-mismatch
 * path in the loader resets the model rather than partial-reads. */
#define HU_PM_VERSION 5u

/* Sprint B.8 wire — borrowed identity graph pointer. Set by daemon at
 * startup via hu_personal_model_set_identity_graph(&g_identity_graph).
 * Single-threaded daemon → no lock needed. NULL means "no graph yet"
 * (first-run path) and the prompt builder silently skips the
 * IDENTITY block. */
static const hu_identity_graph_t *s_identity_graph_for_prompt = NULL;

void hu_personal_model_set_identity_graph(const void *graph) {
    s_identity_graph_for_prompt = (const hu_identity_graph_t *)graph;
}

/* LLM fact-extraction fallback — process-lifetime borrows injected by the
 * daemon/agent at startup. NULL provider (the default) disables the fallback.
 * See hu_personal_model_set_llm_extractor in the header. */
static hu_allocator_t *s_llm_extract_alloc = NULL;
static hu_provider_t *s_llm_extract_provider = NULL;
static char s_llm_extract_model[80] = {0};

void hu_personal_model_set_llm_extractor(void *alloc, void *provider, const char *model,
                                         size_t model_len) {
    s_llm_extract_alloc = (hu_allocator_t *)alloc;
    s_llm_extract_provider = (hu_provider_t *)provider;
    size_t n = model_len < sizeof(s_llm_extract_model) - 1 ? model_len
                                                           : sizeof(s_llm_extract_model) - 1;
    if (model && n > 0)
        memcpy(s_llm_extract_model, model, n);
    s_llm_extract_model[n] = '\0';
}

/* Runtime gate: 0=off (default), 1=shadow, 2=live. Mirrors the off|shadow|on
 * convention used by HU_SALIENCE / HU_TOM_DIRECTIVE / HU_GRAPH_GROUNDING. */
static int llm_fact_extract_gate(void) {
    const char *v = getenv("HU_LLM_FACT_EXTRACT");
    if (!v || !*v)
        return 0;
    if (strcmp(v, "on") == 0 || strcmp(v, "live") == 0)
        return 2;
    if (strcmp(v, "shadow") == 0)
        return 1;
    return 0;
}

/* Minimum message length worth an LLM round-trip. Short acks ("ok", "C?",
 * "lol") rarely carry extractable facts and the regex pass already missed
 * them — skip to keep the fallback cheap and the daemon poll loop responsive. */
#define HU_LLM_FACT_EXTRACT_MIN_LEN 16u

/* Fallback entry point, called from hu_personal_model_ingest ONLY when the
 * regex fast-path produced zero facts. On LIVE it overwrites *extracted with
 * the LLM result (so the caller's existing stamp/promote/merge flow handles
 * it); on SHADOW it logs what it WOULD extract and leaves *extracted empty.
 * Soft-fails (provider error, empty/malformed JSON) leave *extracted empty —
 * never breaks ingest. */
static void maybe_llm_fact_fallback(const char *message, size_t message_len, int64_t timestamp,
                                    hu_fact_extract_result_t *extracted) {
    int gate = llm_fact_extract_gate();
    if (gate == 0)
        return;
    if (!s_llm_extract_provider || !s_llm_extract_alloc)
        return;
    if (message_len < HU_LLM_FACT_EXTRACT_MIN_LEN)
        return;

    hu_fact_extract_result_t llm = {0};
    hu_error_t err = hu_fact_extract_llm(s_llm_extract_alloc, s_llm_extract_provider,
                                         s_llm_extract_model, strlen(s_llm_extract_model), message,
                                         message_len, timestamp, &llm);
    if (err != HU_OK || llm.fact_count == 0)
        return;

    if (gate == 1) {
        /* SHADOW: observe, do not merge. */
        hu_log_info("llm_fact_extract", NULL,
                    "shadow: would extract %zu fact(s) from a regex-missed message",
                    llm.fact_count);
        return;
    }
    /* LIVE: hand the LLM batch to the caller's stamp/promote/merge flow. */
    *extracted = llm;
}

void hu_personal_model_init(hu_personal_model_t *model) {
    if (!model)
        return;
    memset(model, 0, sizeof(*model));
    model->version = HU_PM_VERSION;
    model->created_at = 0;
}

static size_t append_fmt(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (*off >= cap)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap > *off ? cap - *off : 0, fmt, ap);
    va_end(ap);
    if (w < 0)
        return 0;
    if ((size_t)w >= cap - *off) {
        *off = cap > 0 ? cap - 1 : 0;
        if (cap > 0)
            buf[*off] = '\0';
        return (size_t)w;
    }
    *off += (size_t)w;
    return (size_t)w;
}

static const char *formality_desc(float f) {
    if (f < 0.33f)
        return "casual";
    if (f < 0.66f)
        return "balanced";
    return "formal";
}

static const char *verbosity_desc(float v) {
    if (v < 0.33f)
        return "terse";
    if (v < 0.66f)
        return "moderate";
    return "verbose";
}

/* Output-shaping helpers — convert observed style metrics into concise
 * directive language. The frontier model receives both the observation
 * (Communication style: …) and an explicit instruction to mirror it.
 * Without this, models default to their training-distribution register
 * regardless of who they're talking to. */
static const char *register_directive(float formality) {
    if (formality < 0.33f)
        return "casual register";
    if (formality < 0.66f)
        return "balanced register";
    return "formal register";
}

static const char *length_directive(uint32_t avg_chars, char *buf, size_t cap) {
    /* Round to a friendly bucket so the model isn't told to hit "73.0
     * chars" exactly — the EWMA average is noisy. */
    unsigned bucket;
    if (avg_chars < 60U)
        bucket = 50U;
    else if (avg_chars < 120U)
        bucket = 100U;
    else if (avg_chars < 220U)
        bucket = 200U;
    else if (avg_chars < 400U)
        bucket = 300U;
    else
        bucket = 500U;
    snprintf(buf, cap, "keep replies ~%u chars", bucket);
    return buf;
}

static const char *emoji_directive(float freq) {
    if (freq < 0.05f)
        return "no emoji";
    if (freq < 0.20f)
        return "rare emoji";
    return "mirror their emoji use";
}

static const char *humor_directive(float receptivity) {
    if (receptivity < 0.20f)
        return "stay serious";
    if (receptivity < 0.50f)
        return "light humor ok";
    return "playful tone welcome";
}

/* Minimum observation count before the directive is trustworthy.
 * Below this, the EWMA-smoothed metrics are dominated by their
 * priors (0.5 formality, 0.0 verbosity, …) and would push the model
 * toward "balanced register, terse, no emoji" regardless of the user.
 * Three samples is enough for the EWMA to lean toward the user's
 * actual signal while still being a tight bound on warm-up cost. */
#define HU_PM_DIRECTIVE_MIN_SAMPLES 3U

/* Topic-engagement directive — only fires when a topic has been
 * mentioned at least this many times (≥3 distinct facts whose object
 * matches). Keeps fly-by mentions from licensing the model to act on
 * topics the user only glanced at, while still firing on topics that
 * have entered the conversation enough times to be a real signal. */
#define HU_PM_TOPIC_DIRECTIVE_MIN_MENTIONS 3U

/* Chronotype inference — minimum total hourly observations before we
 * classify. Below this, the histogram is too sparse to call (a single
 * insomnia-driven 03:00 burst would otherwise look like an evening-owl
 * pattern). 30 puts us in the right order of magnitude — roughly two
 * weeks of daily use at 2-3 messages/day. */
#define HU_PM_CHRONOTYPE_MIN_SAMPLES 30U

/* Negative-fact predicates — markers that "user <pred> <obj>" expresses
 * something to actively avoid recommending or doing.
 *
 * P2-6 (2026-05-16): predicates are now stored as third-person paraphrases
 * (see src/memory/fact_extract.c). Match the paraphrased forms. */
static const char *kNegativeFactMarkers[] = {
    "dislikes", "hates", "does not want",     "cannot stand",
    "never",    "avoid", "not interested in", "allergic to",
};

static bool predicate_is_negative(const char *predicate) {
    if (!predicate || !*predicate)
        return false;
    for (size_t i = 0; i < sizeof(kNegativeFactMarkers) / sizeof(kNegativeFactMarkers[0]); i++) {
        if (strcasecmp(predicate, kNegativeFactMarkers[i]) == 0)
            return true;
    }
    return false;
}

static void sort_topic_order(const hu_personal_model_t *model, size_t *order) {
    for (size_t i = 0; i < model->topic_count; i++)
        order[i] = i;
    for (size_t i = 0; i + 1 < model->topic_count; i++) {
        for (size_t j = i + 1; j < model->topic_count; j++) {
            float si = model->topics[order[i]].interest_score;
            float sj = model->topics[order[j]].interest_score;
            if (sj > si) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
}

hu_chronotype_t hu_personal_model_infer_chronotype(const hu_personal_model_t *model) {
    if (!model)
        return HU_CHRONO_UNKNOWN;

    /* Sum the early and late windows; total is computed separately
     * so the concentration test compares against the full
     * distribution. Hour 0 (midnight) is bucketed with the evening-owl
     * window: someone messaging through the night belongs with the late
     * group, not the early group. The middle window (10..20) is
     * implicit — early + late + middle == total — so we don't track it. */
    uint32_t early = 0;
    uint32_t late = 0;
    uint32_t total = 0;

    for (int h = 0; h < 24; h++) {
        uint32_t bucket = model->active_hours[h];
        total += bucket;
        if (h == 0 || h >= 21)
            late += bucket;
        else if (h >= 5 && h <= 9)
            early += bucket;
    }

    if (total < HU_PM_CHRONOTYPE_MIN_SAMPLES)
        return HU_CHRONO_UNKNOWN;

    /* Concentration threshold: the dominant window must hold at least
     * 40% of total mass to count. Without this gate, a near-flat
     * distribution where early=11 / late=10 / middle=20 would tip into
     * MORNING_LARK on a single message tie-break — exactly the kind of
     * spurious classification the inference wants to avoid. */
    const float concentration_floor = 0.4f * (float)total;
    const float ratio = 1.5f;

    if ((float)early >= ratio * (float)late && (float)early >= concentration_floor)
        return HU_CHRONO_MORNING_LARK;
    if ((float)late >= ratio * (float)early && (float)late >= concentration_floor)
        return HU_CHRONO_EVENING_OWL;
    return HU_CHRONO_INTERMEDIATE;
}

bool hu_personal_model_has_content(const hu_personal_model_t *model) {
    if (!model)
        return false;
    if (model->fact_count > 0 || model->topic_count > 0 || model->goal_count > 0)
        return true;
    if (model->style.sample_count > 0U)
        return true;
    if (model->core.user_name[0] != '\0' || model->core.user_bio[0] != '\0' ||
        model->core.user_preferences[0] != '\0')
        return true;
    return false;
}

/* Below this effective confidence threshold a fact is considered too
 * stale (or too low-confidence) to surface in the prompt. Threshold is
 * deliberately loose — facts with raw confidence ≥ 0.6 still survive
 * for ~1.5 half-lives (~135 days) before falling off, while a freshly-
 * extracted 0.3-confidence fact is dropped immediately. */
#define HU_PM_FACT_PROMPT_MIN_CONFIDENCE 0.30f

/* Choose the wording for the recently-completed acknowledgment
 * directive based on the active channel's overlay. Returns a
 * pointer to a static const string — the caller never frees it.
 *
 * The directive's *intent* is constant ("when something they
 * recently finished comes up, acknowledge it before moving on,
 * but don't let the ack dominate the reply"). What changes per
 * overlay is the *register*: tone, brevity, emoji license. We
 * keep the variants short and prescriptive — long directive
 * lines bloat the system prompt and (per several internal evals)
 * actively degrade adherence vs short ones.
 *
 * Three families of variants:
 *   1. formal → strict register, no emoji, demand brevity.
 *   2. casual + permissive avg_length / emoji → loose register,
 *      mild emoji license.
 *   3. neutral fallback (overlay missing or unrecognized) →
 *      the existing "warmly acknowledge" wording, preserved
 *      exactly so the legacy `_build_prompt` callers see no
 *      visible drift. */
/* ── Directive variant telemetry (Track D D2.2) ───────────────────────
 *
 * Static counters incremented every time the prompt builder fires
 * a directive variant. Read via
 * `hu_personal_model_directive_telemetry_snapshot`. The atomic
 * builtins keep this thread-safe under the multi-threaded agent
 * loop without pulling in <stdatomic.h> on platforms where it's
 * stricter (and without a mutex — the operation is just an
 * increment). On compilers without `__atomic_*` (rare on the
 * supported toolchains: clang ≥ 3.3, gcc ≥ 4.7), the fallback
 * is a non-atomic increment which is acceptable for telemetry —
 * a missed count under contention is preferable to a lock on
 * every agent turn. */
#if defined(__GNUC__) || defined(__clang__)
#define HU_DIRECTIVE_INC(ptr)        __atomic_fetch_add((ptr), 1, __ATOMIC_RELAXED)
#define HU_DIRECTIVE_LOAD(ptr)       __atomic_load_n((ptr), __ATOMIC_RELAXED)
#define HU_DIRECTIVE_STORE(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_RELAXED)
#else
#define HU_DIRECTIVE_INC(ptr)        ((*(ptr))++)
#define HU_DIRECTIVE_LOAD(ptr)       (*(ptr))
#define HU_DIRECTIVE_STORE(ptr, val) (*(ptr) = (val))
#endif

static uint64_t s_directive_counts[HU_DIRECTIVE_VARIANT__COUNT];

void hu_personal_model_directive_telemetry_snapshot(hu_directive_telemetry_t *out) {
    if (!out)
        return;
    uint64_t total = 0;
    for (size_t i = 0; i < HU_DIRECTIVE_VARIANT__COUNT; i++) {
        out->counts[i] = HU_DIRECTIVE_LOAD(&s_directive_counts[i]);
        total += out->counts[i];
    }
    out->total = total;
}

void hu_personal_model_directive_telemetry_reset(void) {
    for (size_t i = 0; i < HU_DIRECTIVE_VARIANT__COUNT; i++) {
        HU_DIRECTIVE_STORE(&s_directive_counts[i], 0U);
    }
}

const char *hu_personal_model_directive_variant_label(hu_directive_variant_t v) {
    switch (v) {
    case HU_DIRECTIVE_VARIANT_NULL_OVERLAY:
        return "null_overlay";
    case HU_DIRECTIVE_VARIANT_DEFAULT:
        return "default";
    case HU_DIRECTIVE_VARIANT_FORMAL_TERSE:
        return "formal_terse";
    case HU_DIRECTIVE_VARIANT_CASUAL_EMOJI:
        return "casual_emoji";
    case HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT:
        return "casual_or_short";
    case HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI:
        return "adaptive_emoji";
    case HU_DIRECTIVE_VARIANT__COUNT:
    default:
        return "unknown";
    }
}

/* Decision logic broken out from `acknowledgment_directive_for_overlay`
 * so the variant tag is computed in one place — both the wording
 * and the telemetry counter agree on which branch fired. */
static hu_directive_variant_t
directive_variant_for_overlay(const struct hu_persona_overlay *overlay) {
    if (!overlay)
        return HU_DIRECTIVE_VARIANT_NULL_OVERLAY;
    const char *form = overlay->formality && overlay->formality[0] ? overlay->formality : NULL;
    const char *length = overlay->avg_length && overlay->avg_length[0] ? overlay->avg_length : NULL;
    const char *emoji =
        overlay->emoji_usage && overlay->emoji_usage[0] ? overlay->emoji_usage : NULL;

    bool short_length = false;
    if (length) {
        if (strcmp(length, "short") == 0)
            short_length = true;
        else {
            int n = atoi(length);
            if (n > 0 && n <= 30)
                short_length = true;
        }
    }
    bool formal = form && (strcmp(form, "formal") == 0 || strcmp(form, "professional") == 0);
    bool casual = form && (strcmp(form, "casual") == 0 || strcmp(form, "playful") == 0);
    bool emoji_ok = emoji && (strcmp(emoji, "moderate") == 0 || strcmp(emoji, "high") == 0 ||
                              strcmp(emoji, "frequent") == 0);

    /* Formal trumps everything else — never permit emoji or playful wording. */
    if (formal)
        return HU_DIRECTIVE_VARIANT_FORMAL_TERSE;
    if (casual && emoji_ok)
        return HU_DIRECTIVE_VARIANT_CASUAL_EMOJI;
    if (casual || short_length)
        return HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT;
    if (emoji_ok)
        return HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI;
    /* Final fallback — overlay present but no useful signal. */
    return HU_DIRECTIVE_VARIANT_DEFAULT;
}

static const char *acknowledgment_directive_for_overlay(const struct hu_persona_overlay *overlay) {
    hu_directive_variant_t v = directive_variant_for_overlay(overlay);
    /* Telemetry — increment the counter for the variant that
     * fired. Done before returning the wording so even if the
     * caller never inspects the string, the fire is still
     * observed. */
    if ((size_t)v < HU_DIRECTIVE_VARIANT__COUNT)
        HU_DIRECTIVE_INC(&s_directive_counts[v]);
    switch (v) {
    case HU_DIRECTIVE_VARIANT_NULL_OVERLAY:
    case HU_DIRECTIVE_VARIANT_DEFAULT:
        return "Note: when a recently-completed item comes up in the "
               "conversation, acknowledge it warmly (a brief congrats or "
               "check-in) before moving on. Don't let the acknowledgment "
               "dominate the reply.\n";
    case HU_DIRECTIVE_VARIANT_FORMAL_TERSE:
        return "Note: when a recently-completed item comes up in the "
               "conversation, briefly acknowledge it (a respectful one-liner, "
               "no emoji) before moving on. Keep the acknowledgment to a "
               "single sentence.\n";
    case HU_DIRECTIVE_VARIANT_CASUAL_EMOJI:
        return "Style note: when something they recently finished comes up, "
               "give them a quick congrats first (an emoji is fine if it "
               "fits) before moving on. Stay natural — don't make a big "
               "deal of it.\n";
    case HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT:
        return "Note: when something they recently finished comes up, "
               "lead with a quick congrats or check-in (one sentence) "
               "before moving on. Don't let it dominate the reply.\n";
    case HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI:
        return "Note: when a recently-completed item comes up, acknowledge "
               "it warmly (a brief congrats — emoji okay if it fits) "
               "before moving on. Don't let the acknowledgment dominate "
               "the reply.\n";
    case HU_DIRECTIVE_VARIANT__COUNT:
    default:
        /* Unreachable — directive_variant_for_overlay only returns the
         * 6 named values. Keep a safe fallback string anyway. */
        return "Note: when a recently-completed item comes up in the "
               "conversation, acknowledge it warmly before moving on.\n";
    }
}

size_t hu_personal_model_build_prompt(const hu_personal_model_t *model, char *buf, size_t cap) {
    return hu_personal_model_build_prompt_with_overlay(model, NULL, buf, cap);
}

size_t hu_personal_model_build_prompt_with_overlay(const hu_personal_model_t *model,
                                                   const struct hu_persona_overlay *overlay,
                                                   char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!model)
        return 0;

    size_t n = 0;
    append_fmt(buf, cap, &n, "[Personal Context]\n");
    bool detail = false;

    if (model->core.user_name[0] != '\0') {
        append_fmt(buf, cap, &n, "Name: %s\n", model->core.user_name);
        detail = true;
    }
    if (model->core.user_bio[0] != '\0') {
        append_fmt(buf, cap, &n, "Bio: %s\n", model->core.user_bio);
        detail = true;
    }
    if (model->core.user_preferences[0] != '\0') {
        append_fmt(buf, cap, &n, "Preferences: %s\n", model->core.user_preferences);
        detail = true;
    }

    if (model->style.sample_count > 0U) {
        append_fmt(buf, cap, &n, "Communication style: %s, %s, avg %u chars\n",
                   formality_desc(model->style.formality), verbosity_desc(model->style.verbosity),
                   (unsigned)model->style.avg_message_length);
        detail = true;
    }

    /* SOTA personalization wire — convert style observations into an
     * explicit output-shaping directive. The model already sees the
     * observation block above; this line tells it what to *do* with
     * those observations. Gated on:
     *   - sample_count >= warm-up minimum (avoid steering on EWMA prior)
     *   - freshness >= 0.3 (avoid steering on a year-old style fingerprint
     *     when the user has been quiet for months and may have shifted
     *     register; same `now = updated_at` clock used elsewhere). */
    const int64_t pm_now = model->updated_at > 0 ? model->updated_at : 0;
    float style_freshness = hu_personal_communication_style_freshness(&model->style, pm_now);
    if (model->style.sample_count >= HU_PM_DIRECTIVE_MIN_SAMPLES && style_freshness >= 0.3f) {
        /* Blend toward neutral as freshness fades — this softens the
         * cliff at the 0.3 gate. At freshness=1.0 the blended struct
         * is identical to the raw EWMA; at freshness=0.3 the directive
         * has already drifted ~70% of the way to neutral, so it
         * surfaces a much weaker steering signal right before it
         * disappears entirely on the next turn. */
        hu_communication_style_t eff =
            hu_personal_communication_style_blend_with_freshness(&model->style, pm_now);
        char length_buf[64];
        const char *len_dir =
            length_directive(eff.avg_message_length, length_buf, sizeof(length_buf));
        append_fmt(buf, cap, &n, "Mirror their style: %s, %s, %s, %s", len_dir,
                   register_directive(eff.formality), emoji_directive(eff.emoji_frequency),
                   humor_directive(eff.humor_receptivity));
        /* Punctuation/case axes — only emit when the BLENDED ratio
         * crosses ~half so a single capitalized message in a casual
         * thread doesn't bounce the directive off, AND a stale style
         * fingerprint that's drifted toward neutral no longer trips
         * the threshold (the raw 0.7 lowercase from a year ago blends
         * to ~0.55 at one half-life, still above 0.5; at two half-lives
         * it's ~0.525, still above; at three it's ~0.51, still above —
         * so the threshold is essentially as before for the lifetime of
         * the directive, just lightly softened). */
        if (eff.lowercase_ratio >= 0.5f) {
            append_fmt(buf, cap, &n, ", type lowercase");
        }
        if (eff.abbreviation_ratio >= 0.4f) {
            append_fmt(buf, cap, &n, ", ok to use 'u'/'rn'/'btw'");
        }
        append_fmt(buf, cap, &n, ".\n");
        detail = true;
    }

    /* Goals — gated on `hu_personal_goal_effective_priority` so a
     * goal not referenced for more than ~4 half-lives (480 days)
     * silently drops out of the prompt instead of dominating
     * forever. Inactive goals always fail this gate. */
    bool any_goal = false;
    for (size_t g = 0; g < model->goal_count; g++) {
        if (!model->goals[g].active || model->goals[g].description[0] == '\0')
            continue;
        if (hu_personal_goal_effective_priority(&model->goals[g], pm_now) < HU_PM_FORGET_FLOOR)
            continue;
        if (!any_goal) {
            append_fmt(buf, cap, &n, "Active goals: ");
            any_goal = true;
        } else {
            append_fmt(buf, cap, &n, ", ");
        }
        append_fmt(buf, cap, &n, "%s", model->goals[g].description);
        const hu_personal_goal_t *gl = &model->goals[g];
        if (gl->deadline != 0 || gl->progress > 0.0f) {
            append_fmt(buf, cap, &n, " (");
            bool need_sep = false;
            if (gl->deadline != 0) {
                time_t dt = (time_t)gl->deadline;
                struct tm tm_buf;
                struct tm *tm = hu_platform_localtime_r(&dt, &tm_buf);
                if (tm) {
                    char ds[16];
                    strftime(ds, sizeof(ds), "%Y-%m-%d", tm);
                    append_fmt(buf, cap, &n, "deadline: %s", ds);
                    need_sep = true;
                }
            }
            if (gl->progress > 0.0f) {
                if (need_sep)
                    append_fmt(buf, cap, &n, ", ");
                append_fmt(buf, cap, &n, "%d%% done", (int)(gl->progress * 100.0f));
            }
            append_fmt(buf, cap, &n, ")");
        }
    }
    if (any_goal) {
        append_fmt(buf, cap, &n, "\n");
        detail = true;
    }

    /* Recently-completed goals scratchpad — surface goals deactivated
     * within `HU_PM_COMPLETED_GOAL_RETAIN_SEC` so the model has
     * context that a previously-active goal is now done. Useful for
     * tone matching ("congratulations on shipping X" instead of
     * "let me know how X is going") and for the model to ask
     * follow-up questions. The retention window matches the goal-
     * pruning path in apply_decay so what the prompt sees and what
     * the model stores stay in sync. */
    bool any_completed = false;
    for (size_t g = 0; g < model->goal_count; g++) {
        if (model->goals[g].description[0] == '\0')
            continue;
        if (!hu_personal_goal_is_recently_completed(&model->goals[g], pm_now))
            continue;
        if (!any_completed) {
            append_fmt(buf, cap, &n, "Recently completed: ");
            any_completed = true;
        } else {
            append_fmt(buf, cap, &n, ", ");
        }
        append_fmt(buf, cap, &n, "%s", model->goals[g].description);
    }
    if (any_completed) {
        append_fmt(buf, cap, &n, "\n");
        /* Behavioral directive — pair the structural "Recently
         * completed: …" list with an explicit instruction to
         * acknowledge it. The wording is overlay-tuned (formal
         * → terse one-liner, casual → permit emoji, etc.) so
         * the directive's register matches the channel. The
         * NULL-overlay path preserves the original wording
         * verbatim — legacy callers see no visible drift. */
        append_fmt(buf, cap, &n, "%s", acknowledgment_directive_for_overlay(overlay));
        detail = true;
    }

    /* Stale-fact decay — every fact loop in this function uses
     * `hu_heuristic_fact_effective_confidence` so an 8-month-old "I work
     * at Acme" never overrides a 2-week-old "I work at Initech" when
     * the prompt window is tight. `now` is the model's own
     * `updated_at` (set on every ingest) — close enough to wall time
     * for this filter and avoids passing a clock through the API. */
    const int64_t now = pm_now;

    if (model->fact_count > 0) {
        bool any_fact = false;
        size_t emitted = 0;
        for (size_t i = 0; i < model->fact_count && emitted < 8U; i++) {
            const hu_heuristic_fact_t *f = &model->facts[i];
            if (hu_heuristic_fact_effective_confidence(f, now) < HU_PM_FACT_PROMPT_MIN_CONFIDENCE)
                continue;
            if (!any_fact) {
                append_fmt(buf, cap, &n, "Key facts: ");
                any_fact = true;
            } else {
                append_fmt(buf, cap, &n, ", ");
            }
            append_fmt(buf, cap, &n, "%s %s %s", f->subject, f->predicate, f->object);
            emitted++;
        }
        if (any_fact) {
            append_fmt(buf, cap, &n, "\n");
            detail = true;
        }
    }

    /* SOTA personalization wire — pull out negative facts ("i don't like
     * X", "i hate Y", "i'm allergic to Z") and surface them as explicit
     * constraints. The "Key facts:" line above contains them too, but
     * frontier models routinely miss negation in dense fact lists; the
     * dedicated "Avoid:" line gives the constraint its own slot in the
     * prompt where it can't be glossed over. Stale negative facts are
     * also subject to the decay filter — a years-old "I don't drink
     * coffee" shouldn't keep gating a current latte recommendation. */
    {
        bool any_avoid = false;
        size_t scan_max = model->fact_count > HU_PM_MAX_FACTS ? HU_PM_MAX_FACTS : model->fact_count;
        for (size_t i = 0; i < scan_max; i++) {
            const hu_heuristic_fact_t *f = &model->facts[i];
            if (!predicate_is_negative(f->predicate))
                continue;
            if (f->object[0] == '\0')
                continue;
            if (hu_heuristic_fact_effective_confidence(f, now) < HU_PM_FACT_PROMPT_MIN_CONFIDENCE)
                continue;
            if (!any_avoid) {
                append_fmt(buf, cap, &n, "Avoid: ");
                any_avoid = true;
            } else {
                append_fmt(buf, cap, &n, ", ");
            }
            append_fmt(buf, cap, &n, "%s", f->object);
        }
        if (any_avoid) {
            append_fmt(buf, cap, &n, ".\n");
            detail = true;
        }
    }

    if (model->topic_count > 0) {
        size_t order[HU_PM_MAX_TOPICS];
        sort_topic_order(model, order);
        size_t max_t = model->topic_count > 6U ? 6U : model->topic_count;
        /* Topics — gated on `hu_personal_topic_effective_score`. A topic
         * mentioned 200× two years ago shouldn't dominate over a topic
         * mentioned 5× last week; the effective score combines raw
         * interest with `last_mentioned`-driven exponential decay. */
        bool any_topic = false;
        for (size_t i = 0; i < max_t; i++) {
            const hu_personal_topic_t *t = &model->topics[order[i]];
            float eff = hu_personal_topic_effective_score(t, pm_now);
            if (eff < HU_PM_FORGET_FLOOR)
                continue;
            if (!any_topic) {
                append_fmt(buf, cap, &n, "Top interests: ");
                any_topic = true;
            } else {
                append_fmt(buf, cap, &n, ", ");
            }
            append_fmt(buf, cap, &n, "%s (%.2f)", t->name, (double)eff);
        }
        if (any_topic) {
            append_fmt(buf, cap, &n, "\n");
            detail = true;
        } else {
            /* No topic survived the freshness gate — skip the directive
             * loop below too, since it depends on the same data. */
            goto skip_topic_directive;
        }

        /* SOTA personalization wire — convert observed-topic frequency
         * into an actionable engagement directive. The "Top interests:"
         * line above is a passive observation; this line tells the
         * frontier model what to *do* when those topics appear. Gating:
         *   - mention_count >= HU_PM_TOPIC_DIRECTIVE_MIN_MENTIONS (3)
         *     keeps fly-by mentions from licensing substantive
         *     follow-ups before sustained interest is established.
         *   - interest_score >= 0.5 mirrors the observation block —
         *     the same threshold the user would expect to see hit.
         *   - cap at 3 named topics so the directive stays tight and
         *     decodable; the broader interest list is still in the
         *     observation line above. */
        size_t engage_count = 0;
        for (size_t i = 0; i < max_t && engage_count < 3U; i++) {
            const hu_personal_topic_t *t = &model->topics[order[i]];
            if (t->mention_count < HU_PM_TOPIC_DIRECTIVE_MIN_MENTIONS)
                continue;
            /* Match the observation line: gate on effective decayed
             * score, not just raw interest_score, so a stale 0.95
             * topic doesn't license the directive. */
            if (hu_personal_topic_effective_score(t, pm_now) < 0.5f)
                continue;
            if (t->name[0] == '\0')
                continue;
            if (engage_count == 0) {
                append_fmt(buf, cap, &n, "Engage substantively when these come up: ");
            } else {
                append_fmt(buf, cap, &n, ", ");
            }
            append_fmt(buf, cap, &n, "%s", t->name);
            engage_count++;
        }
        if (engage_count > 0)
            append_fmt(buf, cap, &n, ".\n");
    skip_topic_directive:;
    }

    /* Surface day-of-week activity pattern when enough data is present. */
    {
        uint32_t day_total = 0;
        for (int d = 0; d < 7; d++)
            day_total += model->active_days[d];
        if (day_total >= 14U) {
            static const char *const day_names[] = {"Sun", "Mon", "Tue", "Wed",
                                                    "Thu", "Fri", "Sat"};
            int peak_day = 0;
            for (int d = 1; d < 7; d++) {
                if (model->active_days[d] > model->active_days[peak_day])
                    peak_day = d;
            }
            int quiet_day = 0;
            for (int d = 1; d < 7; d++) {
                if (model->active_days[d] < model->active_days[quiet_day])
                    quiet_day = d;
            }
            if (model->active_days[peak_day] > model->active_days[quiet_day] + 2U) {
                append_fmt(buf, cap, &n, "Most active day: %s. Least active: %s.\n",
                           day_names[peak_day], day_names[quiet_day]);
                detail = true;
            }
        }
    }

    {
        hu_chronotype_t chrono = hu_personal_model_infer_chronotype(model);
        if (chrono != HU_CHRONO_UNKNOWN) {
            const char *label = NULL;
            switch (chrono) {
            case HU_CHRONO_MORNING_LARK:
                label = "Morning person (most active early)";
                break;
            case HU_CHRONO_EVENING_OWL:
                label = "Night owl (most active late)";
                break;
            case HU_CHRONO_INTERMEDIATE:
                label = "Flexible schedule";
                break;
            default:
                break;
            }
            if (label) {
                append_fmt(buf, cap, &n, "Chronotype: %s\n", label);
                detail = true;
            }
        }
    }

    /* Sprint A.5 wire — splice reaction-derived social insights into
     * the persona prompt. Pulls top reactors + salient topics from the
     * model's reaction-derived facts (filtered by
     * source_hint=="reaction_ingest"). Returns 0 when there are no
     * reactions; in that case we don't emit an empty paragraph. */
    {
        char social_buf[1024];
        size_t social_n = hu_persona_render_social_insights(model, social_buf, sizeof(social_buf));
        if (social_n > 0) {
            append_fmt(buf, cap, &n, "%s\n", social_buf);
            detail = true;
        }
    }

    /* Sprint A.7 wire — splice the social_state.json snapshot (written
     * every 6h by hu_daemon_social_tick) into the prompt. This brings
     * stale-contact + drift signals to the LLM's attention without
     * waiting for the user to ask "any updates?" The reader is
     * tolerant of missing files; we silently skip when the snapshot
     * isn't there yet (first daemon run, or non-Apple build). */
    {
        char snap_buf[1024];
        size_t snap_n = hu_persona_render_social_state_snapshot(NULL, snap_buf, sizeof(snap_buf));
        if (snap_n > 0) {
            append_fmt(buf, cap, &n, "%s\n", snap_buf);
            detail = true;
        }
    }

    /* Sprint B.2 wire — surface tender emotional context for each
     * distinct contact in the model. The agent_turn site doesn't have
     * the recipient handle plumbed into this function, so instead of
     * making this contact-specific we walk every distinct provenance
     * contact_handle. The LLM then knows "Alice recently mentioned: her
     * mother is sick" regardless of who it's currently messaging, and
     * uses that contextually.
     *
     * Dedup is done by linear scan against a small bounded array of
     * seen handles — N contacts in a personal model is small, and we
     * cap emissions at the lookup limit anyway. */
    {
#define HU_EMOCTX_MAX_DISTINCT_CONTACTS 8
        const char *seen[HU_EMOCTX_MAX_DISTINCT_CONTACTS] = {0};
        size_t seen_count = 0;
        for (size_t fi = 0; fi < model->fact_count && seen_count < HU_EMOCTX_MAX_DISTINCT_CONTACTS;
             fi++) {
            const char *h = model->facts[fi].provenance.contact_handle;
            if (!h || !h[0])
                continue;
            bool dup = false;
            for (size_t s = 0; s < seen_count; s++) {
                if (seen[s] && strcasecmp(seen[s], h) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            seen[seen_count++] = h;

            char emo_buf[256];
            /* now=0 → wall clock in production; tests don't reach this
             * file's contact-walk path (they call emotional_context
             * directly with explicit `now`). */
            size_t emo_n =
                hu_emotional_context_for_contact(model, h, 0, 0, emo_buf, sizeof(emo_buf));
            if (emo_n > 0) {
                append_fmt(buf, cap, &n, "%s\n", emo_buf);
                detail = true;
            }

            /* Sprint B.7 wire — surface upcoming events for the same
             * set of contacts. Same now=0 → wall-clock semantics. The
             * two contexts coexist: a contact may have BOTH a tender
             * recent event AND an upcoming one (e.g. her mother is
             * sick AND her birthday is next week — both useful for
             * the agent). */
            char ant_buf[256];
            size_t ant_n = hu_anticipatory_for_contact(model, h, 0, 0, ant_buf, sizeof(ant_buf));
            if (ant_n > 0) {
                append_fmt(buf, cap, &n, "%s\n", ant_buf);
                detail = true;
            }

            /* Sprint B.6 wire — surface causal attribution ("what
             * works with this contact?"). Cheap aggregate; renders
             * only when total_reactions > 0. Same now=0 semantics. */
            hu_causal_attribution_summary_t cas;
            size_t cas_count = hu_causal_attribution_summarize(model, h, &cas);
            if (cas_count > 0) {
                char cas_buf[256];
                size_t cas_n = hu_causal_attribution_render(h, &cas, 0, cas_buf, sizeof(cas_buf));
                if (cas_n > 0) {
                    append_fmt(buf, cap, &n, "%s\n", cas_buf);
                    detail = true;
                }
            }

            /* Sprint B B-loop — derive a "STYLE HINT:" line from the
             * same causal_attribution counts. This is the act-on side
             * of WHAT WORKS: not just "5 positive / 1 negative" but
             * "keep current tone" / "try shorter, warmer." Renders
             * silently to 0 below the MIN_REACTIONS=3 floor. */
            char style_buf[256];
            size_t style_n = hu_style_adapter_render_hint(model, h, style_buf, sizeof(style_buf));
            if (style_n > 0) {
                append_fmt(buf, cap, &n, "%s\n", style_buf);
                detail = true;
            }
        }
#undef HU_EMOCTX_MAX_DISTINCT_CONTACTS
    }

    /* Sprint B.8 wire — one-shot identity-merge suggestion. Surfaces
     * at most one candidate per prompt build. Skips silently when no
     * graph is wired (first-run path). */
    if (s_identity_graph_for_prompt) {
        char ident_buf[256];
        size_t ident_n = hu_identity_continuity_suggest(model, s_identity_graph_for_prompt,
                                                        ident_buf, sizeof(ident_buf));
        if (ident_n > 0) {
            append_fmt(buf, cap, &n, "%s\n", ident_buf);
            detail = true;
        }
    }

    if (!detail)
        append_fmt(buf, cap, &n, "(No detailed personal data yet.)\n");

    return n;
}

/* ── T7: reflection slice appendage ──────────────────────────────
 *
 * Phase 1 of docs/plans/2026-05-26-reflection-loop. Wraps
 * `_build_prompt_with_overlay` and tacks on a "Recent observations"
 * section pulled from the reflection_patterns SQLite table.
 *
 * Side effect: every pattern we surface here gets marked via
 * hu_reflection_mark_surfaced so the same observation doesn't reach
 * the model on EVERY turn. This is a coarse-but-correct heuristic
 * for Phase 1: if the slice appears in a system prompt, we assume
 * the model can see it; init_proposer's separate path then becomes
 * the second-chance "should I actually mention this?" signal for
 * surfaced-but-unused patterns.
 *
 * Compiled to a thin wrapper when HU_ENABLE_SQLITE is off: just
 * calls through to `_with_overlay`. */

#ifdef HU_ENABLE_SQLITE
size_t hu_personal_model_build_prompt_with_reflection(const hu_personal_model_t *model,
                                                      const struct hu_persona_overlay *overlay,
                                                      struct sqlite3 *db, const char *channel,
                                                      int max_patterns, char *buf, size_t cap) {
    size_t n = hu_personal_model_build_prompt_with_overlay(model, overlay, buf, cap);
    if (!buf || cap == 0 || n >= cap - 1)
        return n;

    /* No db or no channel → behave as the overlay-only wrapper. */
    if (!db || !channel || !*channel || max_patterns <= 0)
        return n;

    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    if (hu_reflection_query_for_system_prompt(db, channel, max_patterns, &patterns, &count) !=
            HU_OK ||
        count == 0) {
        free(patterns); /* may be NULL — free(NULL) is fine */
        /* Even with no patterns, the latest prose summary (if any) is
         * worth surfacing — it's a digest of the most recent run. */
        char *summary = hu_reflection_latest_prose_summary(db);
        if (summary && *summary) {
            append_fmt(buf, cap, &n, "\nLatest reflection: %s\n", summary);
        }
        free(summary);
        return n;
    }

    append_fmt(buf, cap, &n, "\nRecent observations about Seth (from reflection):\n");
    for (int i = 0; i < count; i++) {
        append_fmt(buf, cap, &n, "- %s (confidence %.2f)\n", patterns[i].observation,
                   patterns[i].confidence);
    }

    /* Latest run's prose summary — gives the model a 2-3 sentence
     * digest of the most recent reflection run alongside the bullets. */
    char *summary = hu_reflection_latest_prose_summary(db);
    if (summary && *summary) {
        append_fmt(buf, cap, &n, "Latest reflection summary: %s\n", summary);
    }
    free(summary);

    /* Mark each surfaced pattern as surfaced so it doesn't return on
     * the next turn. The query already filtered out already-surfaced
     * rows, so this is idempotent across multiple build_prompt calls
     * within the same turn (no double-counting). */
    uint64_t surfaced_now_ms = (uint64_t)time(NULL) * 1000;
    for (int i = 0; i < count; i++) {
        hu_reflection_mark_surfaced(db, patterns[i].id);
        /* T8: record per-channel lineage so a thumbs_down on this turn
         * can attribute the contradiction back to these patterns. */
        hu_reflection_note_surfaced(db, patterns[i].id, channel, surfaced_now_ms);
    }
    free(patterns);
    return n;
}
#else  /* !HU_ENABLE_SQLITE */
size_t hu_personal_model_build_prompt_with_reflection(const hu_personal_model_t *model,
                                                      const struct hu_persona_overlay *overlay,
                                                      struct sqlite3 *db, const char *channel,
                                                      int max_patterns, char *buf, size_t cap) {
    (void)db;
    (void)channel;
    (void)max_patterns;
    return hu_personal_model_build_prompt_with_overlay(model, overlay, buf, cap);
}
#endif /* HU_ENABLE_SQLITE */

static bool ci_haystack_contains(const char *hay, const char *needle, size_t needle_len) {
    if (!hay || !needle || needle_len == 0)
        return false;
    for (const char *p = hay; *p != '\0'; p++) {
        size_t k;
        for (k = 0; k < needle_len && p[k] != '\0'; k++) {
            if (tolower((unsigned char)p[k]) != tolower((unsigned char)needle[k]))
                break;
        }
        if (k == needle_len)
            return true;
    }
    return false;
}

static void bump_topic(hu_personal_model_t *model, const char *name, int64_t ts) {
    if (!name || name[0] == '\0')
        return;

    for (size_t i = 0; i < model->topic_count; i++) {
        if (strcasecmp(model->topics[i].name, name) == 0) {
            model->topics[i].mention_count++;
            model->topics[i].last_mentioned = ts;
            if (model->topics[i].interest_score < 1.0f - 0.05f)
                model->topics[i].interest_score += 0.05f;
            else
                model->topics[i].interest_score = 1.0f;
            return;
        }
    }
    if (model->topic_count >= HU_PM_MAX_TOPICS) {
        /* Evict the least-recently-mentioned topic to make room. */
        size_t lru = 0;
        for (size_t i = 1; i < model->topic_count; i++) {
            if (model->topics[i].last_mentioned < model->topics[lru].last_mentioned)
                lru = i;
        }
        model->topics[lru] = model->topics[model->topic_count - 1];
        model->topic_count--;
    }
    hu_personal_topic_t *t = &model->topics[model->topic_count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->interest_score = 0.3f;
    t->mention_count = 1U;
    t->last_mentioned = ts;
}

/* Decrease topic salience after a NEGATIVE reaction. Symmetric to
 * `bump_topic`: if the topic is present, decrement its mention_count
 * (saturating at 1 — never zero, since the topic was clearly mentioned)
 * and pull `interest_score` down by 0.05. If the topic is NOT present
 * we DO NOT add a new slot at low salience — a dislike on a topic the
 * model has never heard of shouldn't materialize a new low-interest
 * topic entry. Returns true iff a slot was actually touched. */
static bool decay_topic_for_negative_reaction(hu_personal_model_t *model, const char *name,
                                              int64_t ts) {
    if (!name || name[0] == '\0')
        return false;
    for (size_t i = 0; i < model->topic_count; i++) {
        if (strcasecmp(model->topics[i].name, name) == 0) {
            if (model->topics[i].mention_count > 1U)
                model->topics[i].mention_count--;
            if (model->topics[i].interest_score > 0.05f)
                model->topics[i].interest_score -= 0.05f;
            else
                model->topics[i].interest_score = 0.0f;
            model->topics[i].last_mentioned = ts;
            return true;
        }
    }
    return false;
}

/* Topic-extraction stopwords for reaction-target text.
 *
 * Per ~/.claude/rules/audit-verify-before-allege.md: there ARE existing
 * stopword filters in the codebase (src/context/conversation.c::is_stopword,
 * src/context/style_tracker.c::is_stopword, src/agent/retrieval_planner.c::
 * is_stopword), but each is `static` inside its TU with a different set
 * tailored to its use case (e.g. conversation.c adds emotion keywords
 * because that module already surfaces emotion through a sibling channel).
 * The reaction-topic case wants the standard "function-word" filter plus a
 * couple of imperatives ("let's", "lets") common in iMessage reactions.
 * Rather than promote one of the three to a header and refactor all four
 * call sites, we inline a fourth local copy. This is the cheapest correct
 * fix; a follow-up could unify them into a shared core/text/stopwords.h. */
static bool reaction_topic_is_stopword(const char *w, size_t len) {
    /* All entries lowercase. Length comparison short-circuits, so we
     * pay strncasecmp only on length match. */
    static const char *const kStop[] = {
        "the",   "and",    "but",   "for",   "with",   "this",  "that",  "what", "when", "where",
        "were",  "will",   "would", "could", "should", "have",  "has",   "had",  "your", "you",
        "are",   "was",    "get",   "got",   "let",    "lets",  "let's", "just", "from", "about",
        "into",  "onto",   "than",  "then",  "them",   "they",  "there", "here", "been", "being",
        "going", "didn't", "wasn",  "won't", "isn't",  "doesn", NULL,
    };
    for (const char *const *sw = kStop; *sw != NULL; sw++) {
        size_t swlen = strlen(*sw);
        if (swlen == len && strncasecmp(w, *sw, len) == 0)
            return true;
    }
    return false;
}

/* Minimum candidate-token length (in BYTES). Tokens shorter than this
 * are dropped before stopword check — keeps "to", "in", "of" etc. out
 * even if the stopword table later evolves. Matches the spec
 * ("each word ≥ 4 chars"). */
#define HU_PM_REACTION_TOPIC_MIN_LEN 4U

/* Has this normalized name already been touched in this call? Bounded
 * dedup array sized to HU_PM_MAX_TOPICS so the function does at most
 * one bump per unique token, even if "hike" appears twice in the
 * preview. */
static bool name_already_touched(const char names[][HU_PM_MAX_FIELD], size_t n,
                                 const char *candidate) {
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(names[i], candidate) == 0)
            return true;
    }
    return false;
}

size_t hu_personal_model_bump_topics_from_reaction(hu_personal_model_t *model,
                                                   const hu_reaction_event_t *event,
                                                   const char *target_text, int64_t now_unix) {
    if (!model || !event)
        return 0;
    /* Removals don't roll back topic salience — see header. */
    if (event->is_removal)
        return 0;
    /* Neutral reactions (QUESTION, UNKNOWN) don't affect topic salience.
     * Custom-emoji is treated by the producer as POSITIVE (see the
     * imessage.c normalizer); we trust event->polarity here rather than
     * re-deriving from event->kind. */
    if (event->polarity == HU_REACTION_NEUTRAL)
        return 0;
    if (!target_text || target_text[0] == '\0')
        return 0;

    const bool positive = (event->polarity == HU_REACTION_POSITIVE);
    size_t touched = 0;
    /* Track which token names have already been processed this call so
     * repeats in the same preview ("hike, hike, hike") only bump once. */
    char seen[HU_PM_MAX_TOPICS][HU_PM_MAX_FIELD];
    size_t seen_n = 0;

    size_t len = strlen(target_text);
    size_t i = 0;
    while (i < len) {
        /* Skip leading non-alnum (spaces, punctuation, opening quotes). */
        while (i < len && !isalnum((unsigned char)target_text[i]))
            i++;
        if (i >= len)
            break;
        size_t start = i;
        /* Word body: keep letters, digits, and intra-word apostrophes
         * ("don't") — but cut at any other punctuation. We then strip
         * any trailing apostrophe in the normalization step. */
        while (i < len && (isalnum((unsigned char)target_text[i]) || target_text[i] == '\'')) {
            i++;
        }
        size_t end = i;
        if (end <= start)
            continue;

        /* Trim leading/trailing apostrophes from the token. */
        while (start < end && target_text[start] == '\'')
            start++;
        while (end > start && target_text[end - 1] == '\'')
            end--;
        if (end <= start)
            continue;

        size_t tok_len = end - start;
        if (tok_len < HU_PM_REACTION_TOPIC_MIN_LEN)
            continue;
        if (tok_len >= HU_PM_MAX_FIELD)
            tok_len = HU_PM_MAX_FIELD - 1;

        /* Lowercase normalize into a stack buffer. */
        char norm[HU_PM_MAX_FIELD];
        for (size_t k = 0; k < tok_len; k++)
            norm[k] = (char)tolower((unsigned char)target_text[start + k]);
        norm[tok_len] = '\0';

        if (reaction_topic_is_stopword(norm, tok_len))
            continue;

        /* Dedup within this call. Explicit cast because C-pre-C2X is strict
         * about adding `const` to a pointer-to-array's element type. */
        if (name_already_touched((const char (*)[HU_PM_MAX_FIELD])seen, seen_n, norm))
            continue;
        if (seen_n < HU_PM_MAX_TOPICS) {
            strncpy(seen[seen_n], norm, sizeof(seen[seen_n]) - 1);
            seen[seen_n][sizeof(seen[seen_n]) - 1] = '\0';
            seen_n++;
        }

        if (positive) {
            size_t before = model->topic_count;
            bump_topic(model, norm, now_unix);
            /* bump_topic always touches a slot when it returns (either
             * existing entry bumped, LRU evicted then new entry added,
             * or new entry appended) — so unconditionally count one.
             * The `before` comparison above is unused but kept as a
             * paper-trail hook for any future "did we hit the cap?"
             * observability. */
            (void)before;
            touched++;
        } else {
            /* Negative polarity. Only count when an existing slot was
             * actually decremented — never materialize a new low-
             * salience slot for a dislike on an unseen topic. */
            if (decay_topic_for_negative_reaction(model, norm, now_unix))
                touched++;
        }
    }
    return touched;
}

static bool fact_key_dup(const hu_heuristic_fact_t *a, const hu_heuristic_fact_t *b) {
    return strcmp(a->subject, b->subject) == 0 && strcmp(a->predicate, b->predicate) == 0;
}

/* All-lowercase detection: a message is "lowercase-styled" when it has
 * letters and zero of them are uppercase. Empty / number-only messages
 * are excluded so they don't bias the ratio. Mirrors the rule used in
 * src/context/behavioral.c::is_all_lowercase. */
static bool message_is_all_lowercase(const char *m, size_t len) {
    bool saw_letter = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)m[i];
        if (c >= 'A' && c <= 'Z')
            return false;
        if ((c >= 'a' && c <= 'z'))
            saw_letter = true;
    }
    return saw_letter;
}

/* Chat-shorthand detection — same vocabulary the linguistic-mirror module
 * already trusts (`src/context/behavioral.c::contains_abbrev`). Single-
 * pass, word-boundary aware. */
static bool message_has_abbreviation(const char *m, size_t len) {
    static const char *kAbbrevs[] = {"u", "ur", "rn", "btw", "ty", "yw", "lmk", "tbh", "imo"};
    for (size_t i = 0; i < len; i++) {
        bool at_start = (i == 0) || !isalpha((unsigned char)m[i - 1]);
        if (!at_start)
            continue;
        for (size_t k = 0; k < sizeof(kAbbrevs) / sizeof(kAbbrevs[0]); k++) {
            const char *abbr = kAbbrevs[k];
            size_t alen = strlen(abbr);
            if (i + alen > len)
                continue;
            bool match = true;
            for (size_t j = 0; j < alen; j++) {
                if (tolower((unsigned char)m[i + j]) != abbr[j]) {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;
            bool at_end = (i + alen == len) || !isalpha((unsigned char)m[i + alen]);
            if (at_end)
                return true;
        }
    }
    return false;
}

static void update_style_from_message(hu_communication_style_t *style, const char *message,
                                      size_t message_len, int64_t timestamp) {
    if (!style || !message || message_len == 0)
        return;

    uint32_t prev_n = style->sample_count;
    style->sample_count++;
    /* Stamp freshness — drives `hu_personal_communication_style_freshness`
     * so a year-old style fingerprint stops shaping the prompt when the
     * user has been quiet for months. Caller passes 0 when they don't
     * have a wall clock available; we leave the previous stamp alone in
     * that case so a clock-less ingest doesn't reset freshness to "old". */
    if (timestamp > 0)
        style->last_observed_at = timestamp;

    uint32_t len = (uint32_t)message_len;
    if (prev_n == 0U) {
        style->avg_message_length = len;
    } else {
        style->avg_message_length =
            (uint32_t)(((uint64_t)style->avg_message_length * (uint64_t)prev_n + len) /
                       (uint64_t)style->sample_count);
    }

    /* Verbosity: map length to 0..1 (500+ chars treated as fully verbose). */
    float verb = fminf(1.0f, (float)len / 500.0f);
    style->verbosity = style->verbosity * 0.85f + verb * 0.15f;

    /* Lowercase / abbreviation EWMA — same 0.85/0.15 mix as verbosity so
     * the ratios converge at similar pace and a transient capitalized
     * message doesn't flip a heavily-lowercase user's directive off. */
    float lower_obs = message_is_all_lowercase(message, message_len) ? 1.0f : 0.0f;
    style->lowercase_ratio = style->lowercase_ratio * 0.85f + lower_obs * 0.15f;
    float abbr_obs = message_has_abbreviation(message, message_len) ? 1.0f : 0.0f;
    style->abbreviation_ratio = style->abbreviation_ratio * 0.85f + abbr_obs * 0.15f;

    /* Rough emoji proxy: UTF-8 leading bytes 0xF0.. often start emoji sequences. */
    size_t emoji_hits = 0;
    for (size_t i = 0; i < message_len; i++) {
        unsigned char c = (unsigned char)message[i];
        if (c >= 240U)
            emoji_hits++;
    }
    float emoji_slice =
        message_len > 0 ? fminf(1.0f, (float)emoji_hits * 4.0f / (float)message_len) : 0.0f;
    style->emoji_frequency = style->emoji_frequency * 0.9f + emoji_slice * 0.1f;

    /* Formality cues */
    float form_adj = 0.0f;
    if (ci_haystack_contains(message, "please", 6) ||
        ci_haystack_contains(message, "thank you", 9) ||
        ci_haystack_contains(message, "would you", 9))
        form_adj += 0.08f;
    if (ci_haystack_contains(message, "lol", 3) || ci_haystack_contains(message, "haha", 4) ||
        ci_haystack_contains(message, "btw", 3))
        form_adj -= 0.06f;
    style->formality = fmaxf(0.0f, fminf(1.0f, style->formality * 0.9f + (0.5f + form_adj) * 0.1f));

    if (ci_haystack_contains(message, "lol", 3) || ci_haystack_contains(message, "haha", 4))
        style->humor_receptivity = fminf(1.0f, style->humor_receptivity * 0.9f + 0.1f * 0.8f);
}

static void bump_temporal(hu_personal_model_t *model, int64_t timestamp) {
    if (timestamp <= 0)
        return;
    time_t t = (time_t)timestamp;
    struct tm tm_buf;
    struct tm *tm = hu_platform_localtime_r(&t, &tm_buf);
    if (!tm)
        return;
    int h = tm->tm_hour;
    int d = tm->tm_wday;
    if (h >= 0 && h < 24 && model->active_hours[h] < 255)
        model->active_hours[h]++;
    if (d >= 0 && d < 7 && model->active_days[d] < 255)
        model->active_days[d]++;
}

hu_error_t hu_personal_model_ingest(hu_personal_model_t *model, const char *message,
                                    size_t message_len, bool from_user, int64_t timestamp,
                                    const hu_provenance_t *prov) {
    if (!model)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len > 0 && !message)
        return HU_ERR_INVALID_ARGUMENT;

    model->interaction_count++;

    if (timestamp > 0) {
        if (model->created_at == 0)
            model->created_at = timestamp;
        model->updated_at = timestamp;
    }

    if (!from_user)
        return HU_OK;

    if (message_len == 0)
        return HU_OK;

    /* SOTA-2026 init-09 §2.4: provenance + MINJA gate.
     *
     * NULL is permitted (defaults to USER_DIRECT) only because the self-test
     * harness at the bottom of this file and a handful of legacy tests pass
     * NULL; production call sites in agent_turn.c / agent_stream.c /
     * feeds/processor.c MUST pass a non-NULL hu_provenance_t derived from
     * the active channel via `hu_channel_trust_stamp`. */
    hu_provenance_t effective_prov;
    if (prov) {
        effective_prov = *prov;
    } else {
        effective_prov = hu_provenance_user_direct(timestamp);
    }

    /* MINJA / memory-injection gate for low-trust inbound content. A
     * detected payload is quarantined silently: no style update, no
     * fact extraction, no temporal bump. The interaction_count bump
     * above is intentional — we still observed an interaction, just
     * a poisoned one. */
    if (effective_prov.tier <= HU_TRUST_THIRD_PARTY) {
        if (hu_minja_detect(message, message_len, NULL)) {
            hu_minja_quarantine_log(message, message_len, &effective_prov);
            return HU_OK;
        }
    }

    bump_temporal(model, timestamp);
    update_style_from_message(&model->style, message, message_len, timestamp);

    hu_fact_extract_result_t extracted;
    hu_error_t err = hu_fact_extract(message, message_len, &extracted);
    if (err != HU_OK)
        return err;

    /* LLM fact-extraction fallback. The regex pass above matches ~43
     * first-person prefixes; casual/indirect text ("Did you not get the
     * email?") yields nothing — which is why the production graph held only
     * 67 relations over 1796 messages. When the fast-path found NO facts and
     * an LLM extractor has been injected, fall back to it. Gated OFF -> SHADOW
     * -> LIVE by HU_LLM_FACT_EXTRACT (default OFF, zero cost). On LIVE this
     * overwrites `extracted`, so the stamp/promote/merge flow below treats the
     * LLM facts exactly like regex facts. */
    if (extracted.fact_count == 0)
        maybe_llm_fact_fallback(message, message_len, timestamp, &extracted);

    /* Stamp provenance on every extracted fact before merge — the
     * checked merge consults `fact->provenance.tier` for the overwrite
     * decision. */
    for (size_t i = 0; i < extracted.fact_count; i++)
        extracted.facts[i].provenance = effective_prov;

    /* USER_DIRECT facts may promote matching pending entries. */
    if (effective_prov.tier >= HU_TRUST_USER_DIRECT) {
        (void)hu_personal_model_promote_pending_facts(model, &extracted, timestamp);
    }

    return hu_personal_model_merge_facts_checked(model, &extracted, &effective_prov);
}

hu_error_t hu_personal_model_merge_facts(hu_personal_model_t *model,
                                         const hu_fact_extract_result_t *facts) {
    if (!model || !facts)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t ts = model->updated_at;

    for (size_t i = 0; i < facts->fact_count; i++) {
        const hu_heuristic_fact_t *nf = &facts->facts[i];
        bool dup = false;
        for (size_t j = 0; j < model->fact_count; j++) {
            if (fact_key_dup(&model->facts[j], nf)) {
                /* Refresh: re-asserting a known fact bumps its
                 * `last_seen_at` (so decay restarts) and lifts its
                 * confidence toward the new observation's confidence
                 * via a small EWMA — never above 1.0. The full triple
                 * is preserved; we only update freshness + confidence. */
                hu_heuristic_fact_t *existing = &model->facts[j];
                if (ts > existing->last_seen_at)
                    existing->last_seen_at = ts;
                float lifted =
                    existing->confidence + 0.1f * (nf->confidence - existing->confidence);
                if (lifted > 1.0f)
                    lifted = 1.0f;
                if (lifted < 0.0f)
                    lifted = 0.0f;
                existing->confidence = lifted;
                bump_topic(model, nf->object, ts);
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (model->fact_count >= HU_PM_MAX_FACTS) {
            /* Evict the lowest-confidence fact to make room. */
            size_t victim = 0;
            for (size_t j = 1; j < model->fact_count; j++) {
                if (model->facts[j].confidence < model->facts[victim].confidence)
                    victim = j;
            }
            if (nf->confidence <= model->facts[victim].confidence)
                continue;
            model->facts[victim] = model->facts[model->fact_count - 1];
            model->fact_count--;
        }
        model->facts[model->fact_count] = *nf;
        /* Stamp last_seen on insert so decay starts now, not at 0. */
        model->facts[model->fact_count].last_seen_at = ts;
        model->fact_count++;
        bump_topic(model, nf->object, ts);
    }
    return HU_OK;
}

/* ── SOTA-2026 init-09 §2.5: trust-gated merge + pending-facts queue ── */

/* Find an index in pending_facts whose subject+predicate matches `nf`,
 * or HU_PM_MAX_PENDING_FACTS if not found. */
static size_t pending_fact_index(const hu_personal_model_t *model, const hu_heuristic_fact_t *nf) {
    for (size_t i = 0; i < model->pending_fact_count; i++) {
        if (fact_key_dup(&model->pending_facts[i], nf))
            return i;
    }
    return HU_PM_MAX_PENDING_FACTS;
}

/* Insert / corroborate a fact in the pending-facts quarantine queue.
 * Returns true when the fact was inserted (or its corroboration count
 * was bumped); false on overflow. */
static bool pending_fact_admit(hu_personal_model_t *model, const hu_heuristic_fact_t *nf,
                               int64_t ts) {
    size_t idx = pending_fact_index(model, nf);
    if (idx < model->pending_fact_count) {
        /* Corroboration only when the new handle differs from the
         * stored one — same source repeating itself never promotes. */
        if (strncmp(model->pending_facts[idx].provenance.contact_handle,
                    nf->provenance.contact_handle, HU_PROV_HANDLE_MAX) != 0 &&
            model->pending_corroboration_count[idx] < 255) {
            model->pending_corroboration_count[idx]++;
        }
        if (ts > model->pending_facts[idx].last_seen_at)
            model->pending_facts[idx].last_seen_at = ts;
        return true;
    }
    if (model->pending_fact_count >= HU_PM_MAX_PENDING_FACTS) {
        /* Evict the lowest-corroboration entry to make room. */
        size_t victim = 0;
        for (size_t j = 1; j < model->pending_fact_count; j++) {
            if (model->pending_corroboration_count[j] < model->pending_corroboration_count[victim])
                victim = j;
        }
        model->pending_facts[victim] = model->pending_facts[model->pending_fact_count - 1];
        model->pending_since[victim] = model->pending_since[model->pending_fact_count - 1];
        model->pending_corroboration_count[victim] =
            model->pending_corroboration_count[model->pending_fact_count - 1];
        model->pending_fact_count--;
    }
    model->pending_facts[model->pending_fact_count] = *nf;
    model->pending_facts[model->pending_fact_count].last_seen_at = ts;
    model->pending_since[model->pending_fact_count] = ts;
    model->pending_corroboration_count[model->pending_fact_count] = 1;
    model->pending_fact_count++;
    return true;
}

/* Remove pending_facts[idx], compacting the queue. */
static void pending_fact_remove(hu_personal_model_t *model, size_t idx) {
    if (idx >= model->pending_fact_count)
        return;
    size_t last = model->pending_fact_count - 1;
    if (idx != last) {
        model->pending_facts[idx] = model->pending_facts[last];
        model->pending_since[idx] = model->pending_since[last];
        model->pending_corroboration_count[idx] = model->pending_corroboration_count[last];
    }
    model->pending_fact_count--;
}

hu_error_t hu_personal_model_merge_facts_checked(hu_personal_model_t *model,
                                                 const hu_fact_extract_result_t *facts,
                                                 const hu_provenance_t *prov) {
    if (!model || !facts)
        return HU_ERR_INVALID_ARGUMENT;
    int64_t ts = model->updated_at;
    hu_trust_tier_t src_tier = prov ? prov->tier : HU_TRUST_USER_DIRECT;

    for (size_t i = 0; i < facts->fact_count; i++) {
        const hu_heuristic_fact_t *nf = &facts->facts[i];

        /* Check existing live facts for a key collision. */
        bool dup_live = false;
        for (size_t j = 0; j < model->fact_count; j++) {
            hu_heuristic_fact_t *ef = &model->facts[j];
            if (!fact_key_dup(ef, nf))
                continue;
            dup_live = true;
            /* MemoryGraft guard: a lower-trust source cannot overwrite
             * a higher-trust stored fact. The contradiction is logged
             * and the existing fact is preserved unchanged. */
            if (!hu_trust_can_overwrite(nf->provenance.tier, ef->provenance.tier)) {
                if (prov)
                    hu_minja_quarantine_log(nf->object, strlen(nf->object), prov);
                break;
            }
            /* Same-or-higher trust: refresh confidence + freshness +
             * provenance (most recent observation wins). */
            if (ts > ef->last_seen_at)
                ef->last_seen_at = ts;
            float lifted = ef->confidence + 0.1f * (nf->confidence - ef->confidence);
            if (lifted > 1.0f)
                lifted = 1.0f;
            if (lifted < 0.0f)
                lifted = 0.0f;
            ef->confidence = lifted;
            ef->provenance = nf->provenance;
            bump_topic(model, nf->object, ts);
            break;
        }
        if (dup_live)
            continue;

        /* THIRD_PARTY-or-below novel facts go to pending-quarantine
         * rather than the live array. Promotion path:
         *   - user re-states the fact in a USER_DIRECT message →
         *     `hu_personal_model_promote_pending_facts`
         *   - HU_PM_PENDING_FACT_PROMOTE_CORROBORATION distinct
         *     low-trust sources corroborate (see admit()) */
        if (src_tier <= HU_TRUST_THIRD_PARTY) {
            (void)pending_fact_admit(model, nf, ts);
            /* If corroboration hit promotion threshold, lift to live. */
            size_t pidx = pending_fact_index(model, nf);
            if (pidx < model->pending_fact_count &&
                model->pending_corroboration_count[pidx] >=
                    HU_PM_PENDING_FACT_PROMOTE_CORROBORATION &&
                model->fact_count < HU_PM_MAX_FACTS) {
                model->facts[model->fact_count] = model->pending_facts[pidx];
                model->facts[model->fact_count].last_seen_at = ts;
                model->fact_count++;
                pending_fact_remove(model, pidx);
                bump_topic(model, nf->object, ts);
            }
            continue;
        }

        /* FIRST_PARTY-or-higher novel facts go straight to live with
         * the existing eviction policy. */
        if (model->fact_count >= HU_PM_MAX_FACTS) {
            size_t victim = 0;
            for (size_t j = 1; j < model->fact_count; j++) {
                if (model->facts[j].confidence < model->facts[victim].confidence)
                    victim = j;
            }
            if (nf->confidence <= model->facts[victim].confidence)
                continue;
            model->facts[victim] = model->facts[model->fact_count - 1];
            model->fact_count--;
        }
        model->facts[model->fact_count] = *nf;
        model->facts[model->fact_count].last_seen_at = ts;
        model->fact_count++;
        bump_topic(model, nf->object, ts);
    }
    return HU_OK;
}

size_t hu_personal_model_promote_pending_facts(hu_personal_model_t *model,
                                               const hu_fact_extract_result_t *user_direct_facts,
                                               int64_t now) {
    if (!model || !user_direct_facts)
        return 0;
    size_t promoted = 0;
    for (size_t i = 0; i < user_direct_facts->fact_count; i++) {
        const hu_heuristic_fact_t *uf = &user_direct_facts->facts[i];
        size_t pidx = pending_fact_index(model, uf);
        if (pidx >= model->pending_fact_count)
            continue;
        if (model->fact_count < HU_PM_MAX_FACTS) {
            model->facts[model->fact_count] = model->pending_facts[pidx];
            /* Tier remains the pending entry's recorded provenance — we
             * do NOT launder it to USER_DIRECT just because the user
             * happened to re-state the same key. The user's own ingest
             * will add a separate USER_DIRECT fact via the normal merge
             * path; this promotion preserves the third-party trail. */
            model->facts[model->fact_count].last_seen_at = now;
            model->fact_count++;
            promoted++;
        }
        pending_fact_remove(model, pidx);
    }
    return promoted;
}

size_t hu_personal_model_expire_pending_facts(hu_personal_model_t *model, int64_t now) {
    if (!model)
        return 0;
    size_t expired = 0;
    for (size_t i = 0; i < model->pending_fact_count;) {
        int64_t since = model->pending_since[i];
        if (since > 0 && (now - since) >= HU_PM_PENDING_FACT_TTL_SEC) {
            pending_fact_remove(model, i);
            expired++;
        } else {
            i++;
        }
    }
    return expired;
}

const hu_heuristic_fact_t *hu_personal_model_query_preference(const hu_personal_model_t *model,
                                                              const char *topic, size_t topic_len) {
    if (!model || !topic || topic_len == 0)
        return NULL;

    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];
        if (ci_haystack_contains(f->object, topic, topic_len) ||
            ci_haystack_contains(f->predicate, topic, topic_len))
            return f;
    }
    return NULL;
}

/* ── Symmetric signal aging — topics, goals, style ─────────────────────
 *
 * Same shape as `hu_heuristic_fact_effective_confidence` (see
 * fact_extract.c) — exponential 0.5^(age/half_life) decay computed
 * via a tiny pre-computed lookup table with linear interpolation,
 * no math.h dependency at the call site. The table is intentionally
 * duplicated here so the personal-model API has zero
 * cross-translation-unit dependency on fact_extract internals. */

static float hu_pm_pow_half_lookup(float k) {
    /* Powers of 0.5, 0..10 — same slope as the fact decay table. */
    static const float pow_half[] = {
        1.000000f, 0.500000f, 0.250000f, 0.125000f, 0.062500f, 0.031250f,
        0.015625f, 0.007812f, 0.003906f, 0.001953f, 0.000977f,
    };
    if (k <= 0.f)
        return 1.f;
    if (k >= 10.f)
        return 0.f;
    int idx = (int)k;
    float frac = k - (float)idx;
    float lo = pow_half[idx];
    float hi = pow_half[idx + 1];
    return lo + (hi - lo) * frac;
}

float hu_personal_topic_effective_score(const hu_personal_topic_t *topic, int64_t now) {
    if (!topic)
        return 0.f;
    /* No-decay-data: callers that haven't stamped last_mentioned yet
     * (synthetic test fixtures, freshly-loaded model) get the raw
     * score back. Same convention as fact decay. */
    if (topic->last_mentioned <= 0 || now <= topic->last_mentioned)
        return topic->interest_score;
    int64_t age = now - topic->last_mentioned;
    float k = (float)age / (float)HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC;
    return topic->interest_score * hu_pm_pow_half_lookup(k);
}

float hu_personal_goal_effective_priority(const hu_personal_goal_t *goal, int64_t now) {
    if (!goal)
        return 0.f;
    /* Inactive goals never claim prompt space. */
    if (!goal->active)
        return 0.f;
    /* Empty slot — neither created nor referenced. */
    if (goal->last_referenced <= 0 && goal->created_at <= 0)
        return 0.f;
    /* Anchor: if last_referenced is unset, fall back to created_at so
     * a freshly-inserted goal is treated as just-touched (priority
     * 1.0) rather than infinitely old. */
    int64_t anchor = goal->last_referenced > 0 ? goal->last_referenced : goal->created_at;
    if (now <= anchor)
        return 1.0f; /* implicit max priority right after a reference */
    int64_t age = now - anchor;
    float k = (float)age / (float)HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC;
    return hu_pm_pow_half_lookup(k);
}

size_t hu_personal_model_describe_recently_completed(const hu_personal_model_t *model, int64_t now,
                                                     char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!model)
        return 0;

    /* Walk goals once, appending description-by-description with
     * truncation guard. We keep this in a single pass (no two-stage
     * allocate-then-format) because the goal array is small (≤ 8)
     * and the description field is fixed-size. */
    size_t written = 0;
    bool any = false;
    bool truncated = false;
    /* Reserve a few bytes at the end for ", …" so we never write
     * past `cap - 1` (the NUL slot). The ellipsis is ASCII "..."
     * to keep us out of the multi-byte UTF-8 truncation mess. */
    const char *ellipsis = ", ...";
    const size_t ellipsis_len = 5;
    for (size_t i = 0; i < model->goal_count; i++) {
        if (model->goals[i].description[0] == '\0')
            continue;
        if (!hu_personal_goal_is_recently_completed(&model->goals[i], now))
            continue;
        const char *sep = any ? ", " : "";
        size_t sep_len = any ? 2 : 0;
        size_t desc_len = strlen(model->goals[i].description);
        /* Need: written + sep_len + desc_len + 1 (NUL) <= cap.
         * If not, try to fit ellipsis instead — but only when we
         * already wrote at least one description. */
        if (written + sep_len + desc_len + 1 > cap) {
            if (any && written + ellipsis_len + 1 <= cap) {
                memcpy(buf + written, ellipsis, ellipsis_len);
                written += ellipsis_len;
                truncated = true;
            }
            break;
        }
        if (sep_len) {
            memcpy(buf + written, sep, sep_len);
            written += sep_len;
        }
        memcpy(buf + written, model->goals[i].description, desc_len);
        written += desc_len;
        any = true;
    }
    (void)truncated;
    buf[written] = '\0';
    return written;
}

size_t hu_personal_model_get_recently_completed_goals(const hu_personal_model_t *model, int64_t now,
                                                      const hu_personal_goal_t **out_buf,
                                                      size_t out_cap) {
    if (!model || !out_buf || out_cap == 0)
        return 0;
    size_t written = 0;
    for (size_t i = 0; i < model->goal_count && written < out_cap; i++) {
        if (model->goals[i].description[0] == '\0')
            continue;
        if (!hu_personal_goal_is_recently_completed(&model->goals[i], now))
            continue;
        out_buf[written++] = &model->goals[i];
    }
    return written;
}

bool hu_personal_goal_is_recently_completed(const hu_personal_goal_t *goal, int64_t now) {
    if (!goal)
        return false;
    /* Active goals are not "recently completed" — by definition.
     * We surface them via the existing "Active goals" line. */
    if (goal->active)
        return false;
    /* An inactive goal with no resolution timestamp has no signal
     * to say *when* it was completed; safer to skip than to misreport. */
    if (goal->last_referenced <= 0)
        return false;
    /* Inside the retention window? */
    int64_t age = now - goal->last_referenced;
    if (age < 0)
        return true; /* clock skew: treat as just-completed */
    return age <= HU_PM_COMPLETED_GOAL_RETAIN_SEC;
}

float hu_personal_communication_style_freshness(const hu_communication_style_t *style,
                                                int64_t now) {
    if (!style)
        return 0.f;
    /* Style aggregates are EWMA-tracked; sample_count == 0 means the
     * aggregate is meaningless regardless of timestamp, so the
     * directive should never fire. */
    if (style->sample_count == 0U)
        return 0.f;
    /* No-decay-data: pre-migration models that have samples but no
     * last_observed_at should fall back to "fresh" so we don't
     * silently drop their directive. The migration path on load
     * stamps last_observed_at to the model's updated_at, so this
     * branch is mainly for synthetic fixtures. */
    if (style->last_observed_at <= 0 || now <= style->last_observed_at)
        return 1.0f;
    int64_t age = now - style->last_observed_at;
    float k = (float)age / (float)HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;
    return hu_pm_pow_half_lookup(k);
}

hu_communication_style_t
hu_personal_communication_style_blend_with_freshness(const hu_communication_style_t *style,
                                                     int64_t now) {
    hu_communication_style_t out;
    memset(&out, 0, sizeof(out));
    if (!style)
        return out;

    out = *style;
    /* Only the 0..1 axes are blended. Pass-through fields (sample_count,
     * avg_message_length, last_observed_at) are already copied by the
     * full struct copy above. */
    float fr = hu_personal_communication_style_freshness(style, now);
    /* Clamp defensively — freshness is in [0,1] mathematically but a
     * future change to the lookup table or a buggy fixture could land
     * outside the range. We don't want clamp surprises propagating
     * into the prompt. */
    if (fr < 0.f)
        fr = 0.f;
    if (fr > 1.f)
        fr = 1.f;
    const float drift_to_neutral = 1.f - fr;
    const float NEUTRAL = 0.5f;

#define HU_PM_BLEND(field) out.field = style->field * fr + NEUTRAL * drift_to_neutral
    HU_PM_BLEND(formality);
    HU_PM_BLEND(verbosity);
    HU_PM_BLEND(emoji_frequency);
    HU_PM_BLEND(humor_receptivity);
    HU_PM_BLEND(lowercase_ratio);
    HU_PM_BLEND(abbreviation_ratio);
#undef HU_PM_BLEND
    return out;
}

/* Track D D2.2 — persona-fidelity scorer feature extraction.
 *
 * Walks the response once, populates three observed-feature ratios
 * that mirror the EWMA-tracked fields on `hu_communication_style_t`.
 * Pulled out of the public scorer so test fixtures can poke at the
 * intermediate numbers without depending on the score-aggregation
 * formula. */
typedef struct hu_pm_response_features {
    float lowercase_ratio;    /* of letters in the response */
    float abbreviation_ratio; /* of words that match the shorthand list */
    size_t byte_len;
} hu_pm_response_features_t;

/* Same shorthand vocabulary as `update_style_from_message`. Keeping
 * the two in sync matters: the scorer's "abbreviation_ratio" must
 * mean the same thing as the EWMA's "abbreviation_ratio" or the
 * scoring delta is meaningless. */
static const char *HU_PM_FIDELITY_ABBREVS[] = {"u", "rn", "btw", "ty", "lmk", "yw"};
#define HU_PM_FIDELITY_ABBREV_COUNT \
    (sizeof(HU_PM_FIDELITY_ABBREVS) / sizeof(HU_PM_FIDELITY_ABBREVS[0]))

static bool fidelity_word_is_abbrev(const char *w, size_t wl) {
    for (size_t i = 0; i < HU_PM_FIDELITY_ABBREV_COUNT; i++) {
        const char *a = HU_PM_FIDELITY_ABBREVS[i];
        size_t al = strlen(a);
        if (al != wl)
            continue;
        size_t k = 0;
        while (k < wl) {
            unsigned char x = (unsigned char)w[k];
            unsigned char y = (unsigned char)a[k];
            if (x >= 'A' && x <= 'Z')
                x = (unsigned char)(x + 32);
            if (y >= 'A' && y <= 'Z')
                y = (unsigned char)(y + 32);
            if (x != y)
                break;
            k++;
        }
        if (k == wl)
            return true;
    }
    return false;
}

static void hu_pm_extract_response_features(const char *response, size_t response_len,
                                            hu_pm_response_features_t *out) {
    memset(out, 0, sizeof(*out));
    out->byte_len = response_len;
    if (!response || response_len == 0)
        return;

    size_t total_letters = 0, lower_letters = 0;
    size_t total_words = 0, abbrev_words = 0;
    size_t i = 0;
    while (i < response_len) {
        unsigned char c = (unsigned char)response[i];
        bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!is_alpha) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < response_len) {
            unsigned char d = (unsigned char)response[i];
            if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')))
                break;
            total_letters++;
            if (d >= 'a' && d <= 'z')
                lower_letters++;
            i++;
        }
        size_t wl = i - start;
        total_words++;
        if (fidelity_word_is_abbrev(response + start, wl))
            abbrev_words++;
    }

    if (total_letters > 0)
        out->lowercase_ratio = (float)lower_letters / (float)total_letters;
    if (total_words > 0)
        out->abbreviation_ratio = (float)abbrev_words / (float)total_words;
}

/* Triangular axis match: 1.0 when observed == target, drops linearly
 * to 0.0 when the gap is >= 1.0 (the full range of a [0,1] axis).
 * Cheaper and more interpretable than gaussian-based matching, and
 * easier to test. */
static float hu_pm_axis_match(float observed, float target) {
    float diff = observed - target;
    if (diff < 0.f)
        diff = -diff;
    if (diff >= 1.f)
        return 0.f;
    return 1.f - diff;
}

/* Length axis: triangular match over a relative-error window. Match
 * is 1.0 when observed length == target length, drops to 0.0 when
 * the relative error is >= 1.0 (e.g. observed is 2× the target or
 * 0× the target). For target == 0 we return 1.0 (no length signal
 * to match against). */
static float hu_pm_length_match(size_t observed, uint32_t target) {
    if (target == 0)
        return 1.f;
    float t = (float)target;
    float o = (float)observed;
    float diff = o - t;
    if (diff < 0.f)
        diff = -diff;
    float rel = diff / t;
    if (rel >= 1.f)
        return 0.f;
    return 1.f - rel;
}

float hu_communication_style_fidelity_score(const hu_communication_style_t *target,
                                            const char *response, size_t response_len) {
    if (!target || !response || response_len == 0)
        return -1.f;
    if (target->sample_count == 0U)
        return -1.f;

    hu_pm_response_features_t f;
    hu_pm_extract_response_features(response, response_len, &f);

    /* Three axis scores → mean. We don't weight: each axis contributes
     * equally to the overall fidelity. Future improvement: weight
     * axes by sample_count's confidence in each EWMA, so a freshly-
     * observed style has more influence than a barely-warmed one. */
    float lower_match = hu_pm_axis_match(f.lowercase_ratio, target->lowercase_ratio);
    float abbrev_match = hu_pm_axis_match(f.abbreviation_ratio, target->abbreviation_ratio);
    float length_match = hu_pm_length_match(f.byte_len, target->avg_message_length);
    return (lower_match + abbrev_match + length_match) / 3.f;
}

/* Internal: score one response set, accumulating into `out`. */
static void hu_pm_score_response_set(const hu_communication_style_t *target, const char *const *set,
                                     const size_t *lens, size_t n,
                                     hu_communication_style_set_summary_t *out) {
    memset(out, 0, sizeof(*out));
    out->min_score = 1.f;
    out->max_score = 0.f;
    if (!set || n == 0)
        return;
    float sum = 0.f;
    for (size_t i = 0; i < n; i++) {
        const char *resp = set[i];
        size_t resp_len = lens ? lens[i] : (resp ? strlen(resp) : 0);
        if (!resp || resp_len == 0) {
            out->skipped++;
            continue;
        }
        float s = hu_communication_style_fidelity_score(target, resp, resp_len);
        if (s < 0.f) {
            out->skipped++;
            continue;
        }
        sum += s;
        if (s < out->min_score)
            out->min_score = s;
        if (s > out->max_score)
            out->max_score = s;
        out->scored++;
    }
    if (out->scored == 0) {
        /* No comparable scores — leave mean at 0. min stays 1.f
         * (the upper bound) so callers can safely detect the
         * "no signal" case without a separate flag. */
        out->mean = 0.f;
        return;
    }
    out->mean = sum / (float)out->scored;
}

hu_error_t hu_communication_style_compare_response_sets(
    const hu_communication_style_t *target, const char *const *set_a, const size_t *lens_a,
    size_t n_a, const char *const *set_b, const size_t *lens_b, size_t n_b,
    hu_communication_style_set_summary_t *out_a, hu_communication_style_set_summary_t *out_b,
    float *out_delta) {
    if (!target || !out_a || !out_b || !out_delta)
        return HU_ERR_INVALID_ARGUMENT;
    /* Refuse on a fingerprintless target — the fidelity scorer
     * itself returns -1.0 in this case, which would give us a
     * useless 0/0 summary. Cleaner to fail the whole compare. */
    if (target->sample_count == 0U)
        return HU_ERR_INVALID_ARGUMENT;
    /* Sets are allowed to be empty individually; the summaries
     * report `scored=0` and the delta is whatever the other side
     * reports (or 0 when both are empty). */
    hu_pm_score_response_set(target, set_a, lens_a, n_a, out_a);
    hu_pm_score_response_set(target, set_b, lens_b, n_b, out_b);
    *out_delta = out_b->mean - out_a->mean;
    return HU_OK;
}

/* === Phase 5 Task 1 (RL SOTA) — v2 fidelity scorer (4th axis) ============
 *
 * Everything below this line is the OPT-IN v2 surface. No call site of the
 * v1 scorer or the v1 comparator above is modified — round-1 BLOCKER-1
 * pins v1 byte-stability. The v2 surface adds:
 *   - a vocabulary-driven decision-style feature extractor
 *   - a composite decision-style match against the target's EWMA-tracked
 *     hedging / question / imperative ratios
 *   - the 4-axis scorer (3 v1 axes + 1 new composite axis, equal weight)
 *   - a per-set scorer + batch comparator that mirror the v1 comparator
 *     contract on top of the v2 scorer (used by the Phase 5 Task 9
 *     competitive harness for baseline-vs-policy comparison).
 *
 * Determinism: pure CPU, no allocations, no I/O. */

typedef struct hu_pm_v2_vocab_entry {
    const char *word;
    size_t len;
} hu_pm_v2_vocab_entry_t;

/* Hedging vocabulary — words the user types when they want to soften a
 * commitment ("maybe we could", "might be a good idea"). Deliberately
 * excludes "try" because real text uses "try" in both hedging ("we could
 * try X") and imperative ("try X now") positions; the imperative side of
 * "try" is hard to disambiguate without a parser, so it stays out of both
 * vocabularies and we lean on the dedicated word lists below. */
static const hu_pm_v2_vocab_entry_t HU_PM_FIDELITY_HEDGES_V2[] = {
    {"maybe", 5}, {"perhaps", 7}, {"possibly", 8}, {"possible", 8}, {"might", 5},
    {"could", 5}, {"would", 5},   {"probably", 8}, {"somewhat", 8}, {"kinda", 5},
    {"sorta", 5}, {"likely", 6},  {"unlikely", 8}, {"consider", 8}, {"seem", 4},
    {"seems", 5}, {"seemed", 6},  {"appears", 7},  {"appear", 6},   {"suppose", 7},
};
#define HU_PM_FIDELITY_HEDGE_V2_COUNT \
    (sizeof(HU_PM_FIDELITY_HEDGES_V2) / sizeof(HU_PM_FIDELITY_HEDGES_V2[0]))

/* Imperative-verb vocabulary — words that, when they sit at sentence
 * start, signal "do X now" framing. We score sentence-initial position
 * only (not bag-of-words) so "I will fix it" doesn't get counted as
 * imperative even though "fix" is in the table. */
static const hu_pm_v2_vocab_entry_t HU_PM_FIDELITY_IMPERATIVES_V2[] = {
    {"do", 2},    {"check", 5}, {"fix", 3},    {"stop", 4},  {"run", 3},    {"use", 3},
    {"make", 4},  {"go", 2},    {"see", 3},    {"tell", 4},  {"call", 4},   {"send", 4},
    {"ship", 4},  {"build", 5}, {"open", 4},   {"close", 5}, {"add", 3},    {"remove", 6},
    {"set", 3},   {"get", 3},   {"pick", 4},   {"start", 5}, {"finish", 6}, {"save", 4},
    {"load", 4},  {"read", 4},  {"write", 5},  {"print", 5}, {"log", 3},    {"pause", 5},
    {"skip", 4},  {"merge", 5}, {"commit", 6}, {"push", 4},  {"pull", 4},   {"deploy", 6},
    {"keep", 4},  {"drop", 4},  {"kill", 4},   {"ask", 3},   {"answer", 6}, {"reply", 5},
    {"reach", 5}, {"focus", 5},
};
#define HU_PM_FIDELITY_IMPERATIVE_V2_COUNT \
    (sizeof(HU_PM_FIDELITY_IMPERATIVES_V2) / sizeof(HU_PM_FIDELITY_IMPERATIVES_V2[0]))

static bool pm_v2_word_in_vocab(const char *w, size_t wl, const hu_pm_v2_vocab_entry_t *vocab,
                                size_t vocab_n) {
    for (size_t i = 0; i < vocab_n; i++) {
        if (vocab[i].len != wl)
            continue;
        size_t k = 0;
        while (k < wl) {
            unsigned char x = (unsigned char)w[k];
            unsigned char y = (unsigned char)vocab[i].word[k];
            if (x >= 'A' && x <= 'Z')
                x = (unsigned char)(x + 32);
            if (y >= 'A' && y <= 'Z')
                y = (unsigned char)(y + 32);
            if (x != y)
                break;
            k++;
        }
        if (k == wl)
            return true;
    }
    return false;
}

typedef struct hu_pm_v2_response_features {
    /* v1 axes (mirror of hu_pm_response_features_t): */
    float lowercase_ratio;
    float abbreviation_ratio;
    size_t byte_len;
    /* v2 decision-style axes: */
    float hedging_ratio;    /* hedge words / total words */
    float question_ratio;   /* sentences ending in '?' / total sentences */
    float imperative_ratio; /* sentences whose first word is an imperative verb /
                             * total sentences */
} hu_pm_v2_response_features_t;

/* Single-pass walker: extracts both v1 and v2 features. The v1 fields
 * (lowercase_ratio, abbreviation_ratio, byte_len) are byte-identical to
 * hu_pm_extract_response_features so v2 and v1 agree on their shared
 * axes; the only difference is the additional decision-style counters. */
static void hu_pm_extract_response_features_v2(const char *response, size_t response_len,
                                               hu_pm_v2_response_features_t *out) {
    memset(out, 0, sizeof(*out));
    out->byte_len = response_len;
    if (!response || response_len == 0)
        return;

    size_t total_letters = 0, lower_letters = 0;
    size_t total_words = 0, abbrev_words = 0, hedge_words = 0;
    size_t total_sentences = 0, question_sentences = 0, imperative_sentences = 0;

    /* Per-sentence state — reset on every '.', '?', or '!' boundary. */
    bool sentence_has_content = false;
    bool sentence_first_word_seen = false;
    bool sentence_first_word_is_imperative = false;
    bool sentence_terminated_by_question = false;

    size_t i = 0;
    while (i < response_len) {
        unsigned char c = (unsigned char)response[i];
        bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (is_alpha) {
            size_t start = i;
            while (i < response_len) {
                unsigned char d = (unsigned char)response[i];
                if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')))
                    break;
                total_letters++;
                if (d >= 'a' && d <= 'z')
                    lower_letters++;
                i++;
            }
            size_t wl = i - start;
            total_words++;
            sentence_has_content = true;
            if (fidelity_word_is_abbrev(response + start, wl))
                abbrev_words++;
            if (pm_v2_word_in_vocab(response + start, wl, HU_PM_FIDELITY_HEDGES_V2,
                                    HU_PM_FIDELITY_HEDGE_V2_COUNT))
                hedge_words++;
            if (!sentence_first_word_seen) {
                sentence_first_word_seen = true;
                sentence_first_word_is_imperative =
                    pm_v2_word_in_vocab(response + start, wl, HU_PM_FIDELITY_IMPERATIVES_V2,
                                        HU_PM_FIDELITY_IMPERATIVE_V2_COUNT);
            }
            continue;
        }
        if (c == '.' || c == '?' || c == '!') {
            if (sentence_has_content) {
                total_sentences++;
                sentence_terminated_by_question = (c == '?');
                if (sentence_terminated_by_question)
                    question_sentences++;
                if (sentence_first_word_is_imperative)
                    imperative_sentences++;
            }
            sentence_has_content = false;
            sentence_first_word_seen = false;
            sentence_first_word_is_imperative = false;
            sentence_terminated_by_question = false;
        }
        i++;
    }
    /* Trailing sentence without a terminator still counts so single-line
     * responses ("ship it") are scored for imperative framing. Such a
     * sentence cannot be a question (no '?' terminator). */
    if (sentence_has_content) {
        total_sentences++;
        if (sentence_first_word_is_imperative)
            imperative_sentences++;
    }

    if (total_letters > 0)
        out->lowercase_ratio = (float)lower_letters / (float)total_letters;
    if (total_words > 0) {
        out->abbreviation_ratio = (float)abbrev_words / (float)total_words;
        out->hedging_ratio = (float)hedge_words / (float)total_words;
    }
    if (total_sentences > 0) {
        out->question_ratio = (float)question_sentences / (float)total_sentences;
        out->imperative_ratio = (float)imperative_sentences / (float)total_sentences;
    }
}

/* Composite 4th-axis match. Mean of three triangular sub-axis matches
 * against the target's EWMA-tracked decision-style ratios. When the
 * target has no decision-style fingerprint (all three sub-axes are
 * zero), the composite collapses to the neutral 0.5 so an
 * un-fingerprinted target neither rewards nor penalises the response
 * on this axis — the v2 score becomes essentially v1 + 0.5 averaged
 * across 4 axes. */
static float hu_pm_decision_style_match(const hu_pm_v2_response_features_t *f,
                                        const hu_communication_style_t *target) {
    const float eps = 1e-6f;
    if (fabsf(target->hedging_ratio) < eps && fabsf(target->question_ratio) < eps &&
        fabsf(target->imperative_ratio) < eps) {
        return 0.5f;
    }
    float h = hu_pm_axis_match(f->hedging_ratio, target->hedging_ratio);
    float q = hu_pm_axis_match(f->question_ratio, target->question_ratio);
    float im = hu_pm_axis_match(f->imperative_ratio, target->imperative_ratio);
    return (h + q + im) / 3.f;
}

float hu_communication_style_fidelity_score_v2(const hu_communication_style_t *target,
                                               const char *response, size_t response_len) {
    if (!target || !response || response_len == 0)
        return -1.f;
    if (target->sample_count == 0U)
        return -1.f;

    hu_pm_v2_response_features_t f;
    hu_pm_extract_response_features_v2(response, response_len, &f);

    float lower_match = hu_pm_axis_match(f.lowercase_ratio, target->lowercase_ratio);
    float abbrev_match = hu_pm_axis_match(f.abbreviation_ratio, target->abbreviation_ratio);
    float length_match = hu_pm_length_match(f.byte_len, target->avg_message_length);
    float decision_match = hu_pm_decision_style_match(&f, target);
    return (lower_match + abbrev_match + length_match + decision_match) / 4.f;
}

static void hu_pm_score_response_set_v2(const hu_communication_style_t *target,
                                        const char *const *set, const size_t *lens, size_t n,
                                        hu_communication_style_set_summary_t *out) {
    memset(out, 0, sizeof(*out));
    out->min_score = 1.f;
    out->max_score = 0.f;
    if (!set || n == 0)
        return;
    float sum = 0.f;
    for (size_t i = 0; i < n; i++) {
        const char *resp = set[i];
        size_t resp_len = lens ? lens[i] : (resp ? strlen(resp) : 0);
        if (!resp || resp_len == 0) {
            out->skipped++;
            continue;
        }
        float s = hu_communication_style_fidelity_score_v2(target, resp, resp_len);
        if (s < 0.f) {
            out->skipped++;
            continue;
        }
        sum += s;
        if (s < out->min_score)
            out->min_score = s;
        if (s > out->max_score)
            out->max_score = s;
        out->scored++;
    }
    if (out->scored == 0) {
        out->mean = 0.f;
        return;
    }
    out->mean = sum / (float)out->scored;
}

hu_error_t hu_communication_style_compare_response_sets_v2(
    const hu_communication_style_t *target, const char *const *set_a, const size_t *lens_a,
    size_t n_a, const char *const *set_b, const size_t *lens_b, size_t n_b,
    hu_communication_style_set_summary_t *out_a, hu_communication_style_set_summary_t *out_b,
    float *out_delta) {
    if (!target || !out_a || !out_b || !out_delta)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0U)
        return HU_ERR_INVALID_ARGUMENT;
    hu_pm_score_response_set_v2(target, set_a, lens_a, n_a, out_a);
    hu_pm_score_response_set_v2(target, set_b, lens_b, n_b, out_b);
    *out_delta = out_b->mean - out_a->mean;
    return HU_OK;
}

/* Case-insensitive whole-word search: does `msg` contain `word`
 * as a substring with non-letter bounds on either side? Used by
 * `hu_personal_model_touch_goals_in_message`. The bounds check
 * keeps "feature" from matching inside "features" only when it
 * really matters, but for length-5+ word fragments inside other
 * words ("learn" inside "learning") we still bump — that's the
 * intended heuristic: a related word shows engagement. */
static bool message_contains_word_ci(const char *msg, size_t msg_len, const char *word,
                                     size_t word_len) {
    if (msg_len < word_len || word_len < 5)
        return false;
    for (size_t i = 0; i + word_len <= msg_len; i++) {
        size_t k = 0;
        while (k < word_len) {
            unsigned char a = (unsigned char)msg[i + k];
            unsigned char b = (unsigned char)word[k];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + 32);
            if (a != b)
                break;
            k++;
        }
        if (k == word_len)
            return true;
    }
    return false;
}

/* Completion-verb table — kept tight on purpose. Adding a verb here
 * trades recall for precision (more goals get auto-deactivated, but
 * also more false positives). Each entry has the verb literal and
 * its length so the linear scan stays branch-light.
 *
 * Verbs are matched case-insensitively as bare substrings; the
 * negation guard below filters out "not done", "haven't shipped",
 * "without finishing". Single-word verbs only — "wrapped up" would
 * need bigram matching, so we list only "wrapped" and let the
 * content-word co-occurrence rule decide. */
typedef struct hu_pm_completion_verb {
    const char *verb;
    size_t len;
} hu_pm_completion_verb_t;

static const hu_pm_completion_verb_t HU_PM_COMPLETION_VERBS[] = {
    {"shipped", 7}, {"finished", 8}, {"completed", 9}, {"wrapped", 7},
    {"done", 4},    {"resolved", 8}, {"closed", 6},
};

#define HU_PM_COMPLETION_VERB_COUNT \
    (sizeof(HU_PM_COMPLETION_VERBS) / sizeof(HU_PM_COMPLETION_VERBS[0]))

/* Returns the offset of the first occurrence of `needle` in `hay`
 * (case-insensitive substring match), or SIZE_MAX if absent. The
 * boundary semantics match `message_contains_word_ci` but the
 * search returns the offset so the caller can run the negation
 * scan from there.
 *
 * Unlike message_contains_word_ci, this DOES require word
 * boundaries on both sides (an alpha char before or after kills the
 * match) so "doneness" doesn't match "done" — important since
 * substring matches on completion verbs are higher-stakes than
 * substring matches on goal content words. */
static size_t find_word_ci_with_boundary(const char *hay, size_t hay_len, const char *needle,
                                         size_t needle_len) {
    if (hay_len < needle_len || needle_len == 0)
        return (size_t)-1;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (i > 0) {
            unsigned char prev = (unsigned char)hay[i - 1];
            if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z'))
                continue;
        }
        size_t k = 0;
        while (k < needle_len) {
            unsigned char a = (unsigned char)hay[i + k];
            unsigned char b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + 32);
            if (a != b)
                break;
            k++;
        }
        if (k != needle_len)
            continue;
        if (i + needle_len < hay_len) {
            unsigned char next = (unsigned char)hay[i + needle_len];
            if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'))
                continue;
        }
        return i;
    }
    return (size_t)-1;
}

/* Negation scan — does the 12-char window before `pos` in `msg`
 * contain "not", "n't", or "without"? Used as the precision lever
 * for completion detection: "I haven't shipped" should NOT count as
 * a completion of a "ship X" goal. Window is generous enough to
 * catch "I have not yet shipped" but tight enough that an unrelated
 * "not" 30 chars earlier doesn't bleed in. */
static bool has_negation_before(const char *msg, size_t msg_len, size_t pos) {
    (void)msg_len;
    size_t start = pos > 12 ? pos - 12 : 0;
    size_t window = pos - start;
    const char *w = msg + start;
    /* "not" — 3 chars */
    if (find_word_ci_with_boundary(w, window, "not", 3) != (size_t)-1)
        return true;
    /* "n't" — 3 chars; appears at end of contractions like haven't, didn't.
     * No word boundary required on the LEFT here ("haven't" → "n't" follows
     * an alpha), so a literal substring search is correct. */
    for (size_t i = 0; i + 3 <= window; i++) {
        unsigned char a = (unsigned char)w[i];
        unsigned char b = (unsigned char)w[i + 1];
        unsigned char c = (unsigned char)w[i + 2];
        if (a == 'n' && b == '\'' && c == 't')
            return true;
        if (a == 'N' && b == '\'' && (c == 't' || c == 'T'))
            return true;
    }
    if (find_word_ci_with_boundary(w, window, "without", 7) != (size_t)-1)
        return true;
    return false;
}

size_t hu_personal_model_resolve_goals_in_message(hu_personal_model_t *model, const char *msg,
                                                  size_t msg_len, int64_t now) {
    if (!model || !msg || msg_len == 0)
        return 0;
    (void)now; /* timestamp not currently stored; reserved for future
                * "completed_at" field if we add one. */

    /* Pre-scan: where does each completion verb sit in the message?
     * One pass, store the earliest offset per verb. SIZE_MAX = absent.
     * The full-message scan is shared across all goals so we don't
     * re-walk the message text once per goal. */
    size_t verb_pos[HU_PM_COMPLETION_VERB_COUNT];
    bool any_verb = false;
    for (size_t v = 0; v < HU_PM_COMPLETION_VERB_COUNT; v++) {
        verb_pos[v] = find_word_ci_with_boundary(msg, msg_len, HU_PM_COMPLETION_VERBS[v].verb,
                                                 HU_PM_COMPLETION_VERBS[v].len);
        if (verb_pos[v] != (size_t)-1)
            any_verb = true;
    }
    if (!any_verb)
        return 0;

    size_t resolved = 0;
    for (size_t g = 0; g < model->goal_count; g++) {
        hu_personal_goal_t *goal = &model->goals[g];
        if (!goal->active || goal->description[0] == '\0')
            continue;

        /* Walk content words in the description (5+ chars, alpha-only).
         * For each, check whether any completion verb co-occurs in the
         * message AND no negation precedes the verb. First match wins. */
        const char *p = goal->description;
        const char *end = p + strnlen(goal->description, sizeof(goal->description));
        bool resolved_this_goal = false;
        while (p < end && !resolved_this_goal) {
            while (p < end && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                p++;
            const char *w = p;
            while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                p++;
            size_t wl = (size_t)(p - w);
            if (wl < 5)
                continue;
            if (find_word_ci_with_boundary(msg, msg_len, w, wl) == (size_t)-1)
                continue;
            /* Goal content word is in the message. Now check that some
             * completion verb is also there AND not negated. */
            for (size_t v = 0; v < HU_PM_COMPLETION_VERB_COUNT; v++) {
                if (verb_pos[v] == (size_t)-1)
                    continue;
                if (has_negation_before(msg, msg_len, verb_pos[v]))
                    continue;
                goal->active = false;
                goal->progress = 1.0f;
                /* Stamp last_referenced to the completion time so
                 * `hu_personal_goal_is_recently_completed` can find
                 * this goal in its retention window even if the
                 * caller didn't run `touch_goals_in_message` first
                 * (the production per_turn_tick helper does, but
                 * unit tests and future call sites might call
                 * resolve directly). */
                if (now > goal->last_referenced)
                    goal->last_referenced = now;
                resolved++;
                resolved_this_goal = true;
                break;
            }
        }
    }
    return resolved;
}

size_t hu_personal_model_touch_goals_in_message(hu_personal_model_t *model, const char *msg,
                                                size_t msg_len, int64_t now) {
    if (!model || !msg || msg_len == 0)
        return 0;
    size_t bumped = 0;
    for (size_t g = 0; g < model->goal_count; g++) {
        hu_personal_goal_t *goal = &model->goals[g];
        if (!goal->active || goal->description[0] == '\0')
            continue;
        /* Walk content words in the description. A "word" is a
         * maximal alpha-only run; we only check words >= 5 chars
         * to filter out particles ("the", "and", "of"). The first
         * match triggers the bump and we skip the rest of the goal. */
        const char *p = goal->description;
        const char *end = p + strnlen(goal->description, sizeof(goal->description));
        bool matched = false;
        while (p < end && !matched) {
            while (p < end && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                p++;
            const char *w = p;
            while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                p++;
            size_t wl = (size_t)(p - w);
            if (wl >= 5 && message_contains_word_ci(msg, msg_len, w, wl)) {
                if (now > goal->last_referenced)
                    goal->last_referenced = now;
                bumped++;
                matched = true;
            }
        }
    }
    return bumped;
}

hu_personal_model_turn_tick_result_t hu_personal_model_per_turn_tick(hu_personal_model_t *model,
                                                                     const char *msg,
                                                                     size_t msg_len, bool from_user,
                                                                     int64_t now) {
    hu_personal_model_turn_tick_result_t r;
    memset(&r, 0, sizeof(r));
    if (!model) {
        r.ingest_error = HU_ERR_INVALID_ARGUMENT;
        return r;
    }
    /* Phase 1: ingest. Errors are reported back to the caller but
     * don't abort the rest of the tick — a partially-failed ingest
     * still benefits from goal-mention bumping and decay pruning of
     * pre-existing entries. */
    r.ingest_error = hu_personal_model_ingest(model, msg, msg_len, from_user, now, NULL);

    /* Phase 2: bump last_referenced on any active goal whose
     * description shares a content-word with the message. Runs
     * BEFORE resolve so the same goal can be touched then resolved
     * (the resolve phase honors the touch by reading active=true
     * before flipping it to false; the freshness stamp is a
     * downstream concern that future "recently completed" surfaces
     * can pick up). */
    r.goals_touched = hu_personal_model_touch_goals_in_message(model, msg, msg_len, now);

    /* Phase 3: deactivate goals the message indicates are complete.
     * Runs AFTER touch so a touch-then-resolve in the same turn
     * leaves the goal with both an updated last_referenced AND
     * active=false. */
    r.goals_resolved = hu_personal_model_resolve_goals_in_message(model, msg, msg_len, now);

    /* Phase 4: prune anything below the forget floor. Runs LAST so
     * a goal resolved in this turn (now active=false → effective
     * priority drops to 0) gets pruned in the same tick instead of
     * lingering for an idle-decay cycle. Idempotent at fixed `now`,
     * sub-microsecond cost (88 elements × float-multiply). */
    r.entries_pruned = hu_personal_model_apply_decay(model, now);

    return r;
}

bool hu_personal_model_idle_due(int64_t *last_inout, int64_t now, int64_t interval) {
    if (!last_inout || now <= 0 || interval <= 0)
        return false;
    /* First-call semantics: any non-positive sentinel ("never run")
     * forces a fire on the next call. We treat "first call" as
     * `*last_inout <= 0` rather than `== 0` so callers that
     * deliberately seed with -1 also get the run-immediately
     * behaviour. */
    if (*last_inout <= 0) {
        *last_inout = now;
        return true;
    }
    /* Normal interval check. We compare the difference rather than
     * the sum to avoid overflow on near-INT64_MAX timestamps. */
    if (now - *last_inout < interval)
        return false;
    *last_inout = now;
    return true;
}

size_t hu_personal_model_apply_decay(hu_personal_model_t *model, int64_t now) {
    if (!model)
        return 0;
    size_t pruned = 0;

    /* Facts — re-use the canonical decay function from fact_extract. */
    {
        size_t kept = 0;
        for (size_t i = 0; i < model->fact_count; i++) {
            float eff = hu_heuristic_fact_effective_confidence(&model->facts[i], now);
            if (eff < HU_PM_FORGET_FLOOR) {
                pruned++;
                continue;
            }
            if (kept != i)
                model->facts[kept] = model->facts[i];
            kept++;
        }
        if (kept < model->fact_count) {
            /* Zero out the now-vacant tails so save/load round-trips
             * don't carry ghost data in unused slots. */
            memset(&model->facts[kept], 0, (model->fact_count - kept) * sizeof(model->facts[0]));
            model->fact_count = kept;
        }
    }

    /* Topics — same compaction shape. */
    {
        size_t kept = 0;
        for (size_t i = 0; i < model->topic_count; i++) {
            float eff = hu_personal_topic_effective_score(&model->topics[i], now);
            if (eff < HU_PM_FORGET_FLOOR) {
                pruned++;
                continue;
            }
            if (kept != i)
                model->topics[kept] = model->topics[i];
            kept++;
        }
        if (kept < model->topic_count) {
            memset(&model->topics[kept], 0, (model->topic_count - kept) * sizeof(model->topics[0]));
            model->topic_count = kept;
        }
    }

    /* Goals — keep when either (a) effective priority is above the
     * forget floor (active + still relevant), OR (b) the goal is in
     * the recently-completed retention window. The retention path
     * lets the prompt builder surface "Recently completed: …" lines
     * for ~7 days post-resolution; after that, the goal joins the
     * regular pruning fate. */
    {
        size_t kept = 0;
        for (size_t i = 0; i < model->goal_count; i++) {
            float eff = hu_personal_goal_effective_priority(&model->goals[i], now);
            bool keep = (eff >= HU_PM_FORGET_FLOOR) ||
                        hu_personal_goal_is_recently_completed(&model->goals[i], now);
            if (!keep) {
                pruned++;
                continue;
            }
            if (kept != i)
                model->goals[kept] = model->goals[i];
            kept++;
        }
        if (kept < model->goal_count) {
            memset(&model->goals[kept], 0, (model->goal_count - kept) * sizeof(model->goals[0]));
            model->goal_count = kept;
        }
    }

    return pruned;
}

/* ── M2 P1: Persistence ─────────────────────────────────────────────────
 *
 * Binary format:
 *   magic   uint32_t  "HUPM" (0x4D505548 LE)
 *   version uint32_t  matches `model->version` at write time
 *   reserved uint64_t zero, room for future framing without breaking
 *   body    hu_personal_model_t  raw struct bytes (POD)
 *
 * Choice of binary over JSON: ~6KB on disk vs ~25KB, zero dependencies,
 * and the struct is POD with fixed-size buffers so memcpy is safe.
 * The version field forces a fresh state on schema bumps; this is
 * intentional because old facts/topics may be incompatible with new
 * code (e.g. a tightened type discriminator).
 *
 * Path is created if needed; intermediate directories are NOT created
 * by this code — callers should pre-create `~/.human/personal_model/`
 * (the daemon's onboard wizard does this for the user). */

#define HU_PM_MAGIC 0x4D505548u /* "HUPM" little-endian */
/* v1 → v2: hu_communication_style_t gained `lowercase_ratio` and
 *          `abbreviation_ratio` fields.
 * v2 → v3: hu_heuristic_fact_t gained `last_seen_at` for freshness
 *          tracking and exponential confidence decay.
 * v3 → v4: hu_personal_goal_t gained `last_referenced` and
 *          hu_communication_style_t gained `last_observed_at` —
 *          symmetric signal-aging machinery for topics/goals/style.
 *          Old saves fail magic+version check and the caller falls
 *          back to a fresh-default model. Forward-compatible
 *          progressive migration is tracked separately under the
 *          schema-migration slice (Track #7 in the SOTA program).
 * HU_PM_VERSION is defined at the top of this file. */

typedef struct hu_pm_header {
    uint32_t magic;
    uint32_t version;
    uint64_t reserved; /* always 0; reserved for future framing */
} hu_pm_header_t;

/* Best-effort `mkdir -p` for the parent of `path`.
 *
 * Walks the path string from the front, calling `mkdir(0700)` at every '/'
 * boundary so the personal model can land cleanly on first run even when
 * `~/.human/` does not exist yet. 0700 mode is deliberate — the personal
 * model is sensitive user data; only the owner should read it. We swallow
 * EEXIST and ignore failures on intermediate components so a pre-existing
 * directory tree (the common case) is a no-op. The final mkdir result is
 * also ignored — the subsequent `fopen` will surface any real failure as
 * HU_ERR_IO. */
static void hu_pm_ensure_parent_dir(const char *path) {
    if (!path || !*path) {
        return;
    }
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        return;
    }
    size_t parent_len = (size_t)(last_slash - path);
    char buf[1024];
    if (parent_len + 1 >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, parent_len);
    buf[parent_len] = '\0';
    /* Walk from the start, creating each component. Skip leading '/'. */
    for (size_t i = 1; i < parent_len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0700);
            buf[i] = '/';
        }
    }
    (void)mkdir(buf, 0700);
}

const char *hu_personal_model_resolve_default_path(char *buf, size_t cap) {
    if (!buf || cap == 0) {
        return NULL;
    }
    buf[0] = '\0';
    const char *override = getenv("HUMAN_PERSONAL_MODEL_PATH");
    if (override && override[0]) {
        size_t len = strlen(override);
        if (len + 1 > cap) {
            return NULL;
        }
        memcpy(buf, override, len + 1);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        return NULL;
    }
    int n = snprintf(buf, cap, "%s/.human/personal_model.bin", home);
    if (n <= 0 || (size_t)n >= cap) {
        return NULL;
    }
    return buf;
}

/* ── v3 → v4 progressive migration ─────────────────────────────────────
 *
 * v3 differs from v4 only in two struct layouts:
 *   - `hu_personal_goal_t` gained `last_referenced` (one int64 per goal).
 *   - `hu_communication_style_t` gained `last_observed_at` (one int64).
 *
 * These shifts cascade through every field after them, so the v3 file
 * cannot be `fread`-ed directly into the live `hu_personal_model_t`.
 * Instead we mirror the v3 layout in two private POD structs, read the
 * v3 model whole, and field-copy it into the v4 in-memory model with
 * the new fields zero-filled. Facts are unchanged across v3↔v4 (they
 * already had `last_seen_at` since v2→v3) so the entire fact array
 * survives the migration cleanly. Topics never changed; only goals
 * and style need handling.
 *
 * The v3 structs and assembled `hu_pm_v3_model_t` are file-static —
 * tests that need a v3 fixture must mirror these layouts themselves
 * (see `tests/test_personal_model.c::personal_model_loads_v3_save`)
 * so they stay locked to whatever the previous binary release shipped. */

typedef struct hu_pm_v3_goal {
    char description[512];
    bool active;
    int64_t created_at;
    /* v3 had no `last_referenced` here — that's the migration delta. */
    int64_t deadline;
    float progress;
} hu_pm_v3_goal_t;

typedef struct hu_pm_v3_style {
    float formality;
    float verbosity;
    float emoji_frequency;
    float humor_receptivity;
    float lowercase_ratio;
    float abbreviation_ratio;
    uint32_t avg_message_length;
    uint32_t sample_count;
    /* v3 had no `last_observed_at` here. */
} hu_pm_v3_style_t;

typedef struct hu_pm_v3_model {
    hu_core_memory_t core;
    hu_heuristic_fact_t facts[HU_PM_MAX_FACTS];
    size_t fact_count;
    hu_pm_v3_style_t style;
    hu_personal_topic_t topics[HU_PM_MAX_TOPICS];
    size_t topic_count;
    hu_pm_v3_goal_t goals[HU_PM_MAX_GOALS];
    size_t goal_count;
    uint8_t active_hours[24];
    uint8_t active_days[7];
    int64_t created_at;
    int64_t updated_at;
    uint32_t interaction_count;
    uint32_t version;
} hu_pm_v3_model_t;

/* On-disk version constant — kept distinct from `HU_PM_VERSION` so the
 * loader path can recognize a legacy save without trusting the field
 * value inside the body (defense in depth). */
#define HU_PM_VERSION_V3 3u

static void hu_pm_migrate_v3_to_v4(const hu_pm_v3_model_t *v3, hu_personal_model_t *out) {
    hu_personal_model_init(out);
    out->core = v3->core;

    /* Facts — byte-identical layout, copy whole. */
    size_t nf = v3->fact_count > HU_PM_MAX_FACTS ? HU_PM_MAX_FACTS : v3->fact_count;
    for (size_t i = 0; i < nf; i++)
        out->facts[i] = v3->facts[i];
    out->fact_count = nf;

    /* Style — copy known fields, zero the new `last_observed_at`. The
     * freshness function falls back to "fully fresh" when sample_count
     * is non-zero and last_observed_at is 0, so migrated models keep
     * their style directive instead of silently dropping it. */
    out->style.formality = v3->style.formality;
    out->style.verbosity = v3->style.verbosity;
    out->style.emoji_frequency = v3->style.emoji_frequency;
    out->style.humor_receptivity = v3->style.humor_receptivity;
    out->style.lowercase_ratio = v3->style.lowercase_ratio;
    out->style.abbreviation_ratio = v3->style.abbreviation_ratio;
    out->style.avg_message_length = v3->style.avg_message_length;
    out->style.sample_count = v3->style.sample_count;
    out->style.last_observed_at = 0;

    /* Topics — byte-identical layout, copy whole. */
    size_t nt = v3->topic_count > HU_PM_MAX_TOPICS ? HU_PM_MAX_TOPICS : v3->topic_count;
    for (size_t i = 0; i < nt; i++)
        out->topics[i] = v3->topics[i];
    out->topic_count = nt;

    /* Goals — copy known fields, zero the new `last_referenced`.
     * The effective-priority function uses `created_at` as a fallback
     * when `last_referenced` is 0, so migrated goals don't suddenly
     * decay to nothing. */
    size_t ng = v3->goal_count > HU_PM_MAX_GOALS ? HU_PM_MAX_GOALS : v3->goal_count;
    for (size_t i = 0; i < ng; i++) {
        memcpy(out->goals[i].description, v3->goals[i].description,
               sizeof(out->goals[i].description));
        out->goals[i].active = v3->goals[i].active;
        out->goals[i].created_at = v3->goals[i].created_at;
        out->goals[i].last_referenced = 0;
        out->goals[i].deadline = v3->goals[i].deadline;
        out->goals[i].progress = v3->goals[i].progress;
    }
    out->goal_count = ng;

    memcpy(out->active_hours, v3->active_hours, sizeof(out->active_hours));
    memcpy(out->active_days, v3->active_days, sizeof(out->active_days));
    out->created_at = v3->created_at;
    out->updated_at = v3->updated_at;
    out->interaction_count = v3->interaction_count;
    /* Stamp current schema — the in-memory model is now v4 even though
     * the on-disk file was v3. Re-saving will write a v4 file. */
    out->version = HU_PM_VERSION;
}

hu_error_t hu_personal_model_save(const hu_personal_model_t *model, const char *path) {
    if (!model || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;
    hu_pm_ensure_parent_dir(path);

    /* Phase 0 Task 7 — atomic write via tmp + fsync + rename. Crash safety:
     *   - Crash before fclose: <path>.tmp is partial, <path> is untouched,
     *     load returns the prior state.
     *   - Crash after rename: <path> is the new file, intact.
     *   - No in-between window: rename(2) is atomic on POSIX with respect
     *     to the destination path. */
    char tmp[1024];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(tmp, "wb");
    if (!fp)
        return HU_ERR_IO;

    hu_pm_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = HU_PM_MAGIC;
    hdr.version = HU_PM_VERSION;
    hdr.reserved = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 || fwrite(model, sizeof(*model), 1, fp) != 1) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    /* fflush drains stdio buffers; fsync forces the kernel page cache
     * to disk so a power loss between rename and writeback can't leave
     * the renamed file with stale or zero contents. */
    if (fflush(fp) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    int fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (fclose(fp) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_personal_model_load(hu_personal_model_t *out, const char *path) {
    if (!out || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;
    hu_personal_model_init(out);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_NOT_FOUND;
    hu_pm_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return HU_ERR_PARSE;
    }
    if (hdr.magic != HU_PM_MAGIC) {
        fclose(fp);
        return HU_ERR_PARSE;
    }

    /* Progressive migration — when the on-disk version is the previous
     * schema (v3) we read the v3 layout and field-copy into the live
     * v4 model. Any other non-current version is a hard parse error
     * (defaults preserved). Future migrations should add another arm
     * to this switch rather than touching the v3 path. */
    if (hdr.version == HU_PM_VERSION) {
        hu_personal_model_t tmp;
        if (fread(&tmp, sizeof(tmp), 1, fp) != 1) {
            fclose(fp);
            hu_personal_model_init(out);
            return HU_ERR_PARSE;
        }
        fclose(fp);
        /* Defensive: clamp counts that could overflow on a corrupted file. */
        if (tmp.fact_count > HU_PM_MAX_FACTS)
            tmp.fact_count = HU_PM_MAX_FACTS;
        if (tmp.topic_count > HU_PM_MAX_TOPICS)
            tmp.topic_count = HU_PM_MAX_TOPICS;
        if (tmp.goal_count > HU_PM_MAX_GOALS)
            tmp.goal_count = HU_PM_MAX_GOALS;
        *out = tmp;
        return HU_OK;
    }

    if (hdr.version == HU_PM_VERSION_V3) {
        hu_pm_v3_model_t v3;
        if (fread(&v3, sizeof(v3), 1, fp) != 1) {
            fclose(fp);
            hu_personal_model_init(out);
            return HU_ERR_PARSE;
        }
        fclose(fp);
        /* Same defensive clamps the v4 path applies. */
        if (v3.fact_count > HU_PM_MAX_FACTS)
            v3.fact_count = HU_PM_MAX_FACTS;
        if (v3.topic_count > HU_PM_MAX_TOPICS)
            v3.topic_count = HU_PM_MAX_TOPICS;
        if (v3.goal_count > HU_PM_MAX_GOALS)
            v3.goal_count = HU_PM_MAX_GOALS;
        hu_pm_migrate_v3_to_v4(&v3, out);
        return HU_OK;
    }

    fclose(fp);
    /* Out is already re-initialized to defaults so the caller can
     * keep walking on a schema mismatch beyond N-1. */
    return HU_ERR_PARSE;
}

/* Load per-contact personal model facts from the global database.
 *
 * For now, this is a simple pass-through that loads the entire global
 * model. The per-contact filtering happens at prompt-build time when
 * rendering facts by their provenance.contact_handle (per stakeholder
 * spirit-pass decision: AC-2.1 satisfied by autoresponder-side filtering).
 *
 * Future: when the storage backend supports SQLite queries, this will
 * filter to facts WHERE provenance.contact_handle == contact_handle. */
hu_error_t hu_personal_model_load_for_contact(hu_personal_model_t *out, const char *contact_handle,
                                              const char *path) {
    if (!out || !contact_handle || !*contact_handle || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;
    /* Load global model; the facts retain their per-contact provenance. */
    return hu_personal_model_load(out, path);
}

/* Ingest a message into the per-contact personal model and save atomically.
 *
 * Extracts facts using hu_personal_model_ingest (which automatically
 * stamps facts with their provenance), then atomically saves the updated
 * model back to the database. */
hu_error_t hu_personal_model_ingest_for_contact(hu_personal_model_t *model,
                                                const char *contact_handle, const char *message,
                                                size_t message_len, bool from_user, int64_t ts,
                                                const char *db_path) {
    if (!model || !contact_handle || !*contact_handle || !message || message_len == 0 || !db_path ||
        !*db_path)
        return HU_ERR_INVALID_ARGUMENT;

    /* Ingest the message. The function will extract facts and stamp them
     * with their provenance (including contact_handle from the context). */
    hu_error_t err = hu_personal_model_ingest(model, message, message_len, from_user, ts, NULL);
    if (err != HU_OK)
        return err;

    /* Atomically save the updated model to the database. */
    return hu_personal_model_save(model, db_path);
}
