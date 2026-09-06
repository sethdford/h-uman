/* outbound/style_governor.c — measured-shape enforcement stage.
 *
 * Enforces the persona's MEASURED texting shape at egress (style card
 * 2026-07-12; the live numbers are in the persona style card — vs model
 * baseline 10% / 31%). Prompt rules ask for a distribution; this stage
 * enforces it deterministically.
 *
 * STYLE_GOVERNOR activation gated on the blind A/B rating-drip
 * measurement: do not flip default to LIVE without a human-tier verdict
 * showing shaped output is judged more Seth-like. HU_STYLE_GOVERNOR =
 * off (default) | shadow | live.
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md: one-shot log
 * line on first invocation states the resolved mode either way.
 */

#include "human/agent/style_governor.h"
#include "human/agent/outbound_pipeline.h"
#include "human/core/log.h"
#include "human/persona.h"
#include "human/persona/style_card.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ── Mode resolution (env, cached) ────────────────────────────────── */

static atomic_int s_mode = -1; /* -1 = unresolved */

hu_style_governor_mode_t hu_style_governor_mode(void) {
    int m = atomic_load_explicit(&s_mode, memory_order_relaxed);
    if (m >= 0)
        return (hu_style_governor_mode_t)m;
    const char *env = getenv("HU_STYLE_GOVERNOR");
    hu_style_governor_mode_t resolved = HU_STYLE_GOVERNOR_OFF;
    if (env && env[0]) {
        if (strcmp(env, "live") == 0)
            resolved = HU_STYLE_GOVERNOR_LIVE;
        else if (strcmp(env, "shadow") == 0)
            resolved = HU_STYLE_GOVERNOR_SHADOW;
    }
    atomic_store_explicit(&s_mode, (int)resolved, memory_order_relaxed);
    return resolved;
}

#if HU_IS_TEST
void hu_style_governor_set_mode_for_test(int mode) {
    atomic_store_explicit(&s_mode, mode, memory_order_relaxed);
}
#endif

/* ── Deterministic roll ───────────────────────────────────────────── */

/* FNV-1a 32-bit over the text from `basis`, reduced to 0-99. Same text and
 * basis → same roll: shaping is reproducible across processes. */
static unsigned fnv_roll(const char *text, size_t len, uint32_t basis) {
    uint32_t h = basis;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)text[i];
        h *= 16777619u;
    }
    return (unsigned)(h % 100u);
}

unsigned hu_style_governor_casing_roll(const char *text, size_t len) {
    /* Different offset basis than hu_style_governor_roll, so the casing and
     * period decisions for one message are not the same number. */
    return fnv_roll(text, len, 0x9747b28cu);
}

/* ── Action C: card-derived lowercase-start percentage (cached) ───────── */
static atomic_int s_lower_pct = -1; /* -1 = unresolved */

unsigned hu_style_governor_lowercase_start_pct(const struct hu_persona *persona) {
    int cached = atomic_load_explicit(&s_lower_pct, memory_order_relaxed);
    if (cached >= 0)
        return (unsigned)cached;
    unsigned pct = 100; /* never capitalize until resolved otherwise */
    const char *env = getenv("HU_STYLE_GOVERNOR_CASING");
    if (!(env && env[0] && strcmp(env, "off") == 0)) {
        hu_style_card_t card;
        hu_style_card_resolve(persona ? persona->name : NULL, persona ? persona->name_len : 0,
                              &card);
        double v = card.lowercase_start_rate * 100.0;
        if (v < 0.0)
            v = 0.0;
        if (v > 100.0)
            v = 100.0;
        pct = (unsigned)lround(v);
    }
    atomic_store_explicit(&s_lower_pct, (int)pct, memory_order_relaxed);
    return pct;
}

#if HU_IS_TEST
void hu_style_governor_reset_casing_for_test(void) {
    atomic_store_explicit(&s_lower_pct, -1, memory_order_relaxed);
}
#endif

unsigned hu_style_governor_roll(const char *text, size_t len) {
    /* ~90% of period-ending messages strip (across distinct messages). */
    return fnv_roll(text, len, 2166136261u);
}

/* ── Pure shaping core ────────────────────────────────────────────── */

/* Reciprocal-question boilerplate. The FINAL SENTENCE must equal one of
 * these exactly (after lowercasing, trimming, dropping a leading
 * so/and/but) — exact-phrase, not substring, so content-bearing
 * questions ("how was your day with the kids?") never match. */
static const char *const RECIPROCAL_BOILERPLATE[] = {
    "what about you",
    "how about you",
    "what's up with you",
    "whats up with you",
    "hbu",
    "wbu",
    "and you",
    "how about yourself",
    "what are you up to",
    "how was your day",
    "how's your day",
    "hows your day",
    "how are you",
    "you",
};

static char lower_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Normalize the sentence [s, e) into buf (lowercased, trimmed, leading
 * so/and/but + trailing '?' removed). Returns normalized length or 0. */
static size_t normalize_sentence(const char *text, size_t s, size_t e, char *buf, size_t cap) {
    while (s < e && (text[s] == ' ' || text[s] == '\n'))
        s++;
    while (e > s && (text[e - 1] == '?' || text[e - 1] == ' '))
        e--;
    if (e <= s || e - s >= cap)
        return 0;
    size_t n = 0;
    for (size_t i = s; i < e; i++)
        buf[n++] = lower_ch(text[i]);
    buf[n] = '\0';
    static const char *const lead[] = {"so ", "and ", "but "};
    for (size_t i = 0; i < sizeof(lead) / sizeof(lead[0]); i++) {
        size_t ll = strlen(lead[i]);
        if (n > ll && strncmp(buf, lead[i], ll) == 0) {
            memmove(buf, buf + ll, n - ll + 1);
            n -= ll;
            break;
        }
    }
    return n;
}

static bool starts_with_url(const char *s, size_t n) {
    return (n >= 4 && strncmp(s, "http", 4) == 0) || (n >= 4 && strncmp(s, "www.", 4) == 0);
}

/* First index >= from that starts a line's content (skips spaces/tabs). */
static size_t line_start(const char *s, size_t from, size_t n) {
    while (from < n && (s[from] == ' ' || s[from] == '\t'))
        from++;
    return from;
}

hu_error_t hu_style_governor_shape(hu_allocator_t *alloc, const char *text, size_t len,
                                   unsigned period_roll, char **out, size_t *out_len,
                                   unsigned *actions) {
    return hu_style_governor_shape_ex(alloc, text, len, period_roll, 0, 100, out, out_len, actions);
}

hu_error_t hu_style_governor_shape_ex(hu_allocator_t *alloc, const char *text, size_t len,
                                      unsigned period_roll, unsigned casing_roll,
                                      unsigned lowercase_start_pct, char **out, size_t *out_len,
                                      unsigned *actions) {
    if (!alloc || !text || !out || !out_len || !actions)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    *actions = 0;

    size_t cur = len;
    unsigned acts = 0;

    /* Action B — strip a trailing reciprocal-question boilerplate sentence
     * when real content precedes it (the card's ?-ending rate is ~1 in 10; the
     * model's 31% is mostly this assistant reciprocity reflex). */
    if (cur > 1 && text[cur - 1] == '?') {
        size_t s = cur - 1;
        while (s > 0 && text[s - 1] != '.' && text[s - 1] != '!' && text[s - 1] != '?' &&
               text[s - 1] != '\n')
            s--;
        if (s > 0) { /* whole-message boilerplate stays — nothing would remain */
            char norm[96];
            size_t n = normalize_sentence(text, s, cur, norm, sizeof(norm));
            bool boiler = false;
            for (size_t i = 0;
                 n > 0 && i < sizeof(RECIPROCAL_BOILERPLATE) / sizeof(RECIPROCAL_BOILERPLATE[0]);
                 i++) {
                if (strcmp(norm, RECIPROCAL_BOILERPLATE[i]) == 0) {
                    boiler = true;
                    break;
                }
            }
            if (boiler) {
                size_t kept = s;
                while (kept > 0 && (text[kept - 1] == ' ' || text[kept - 1] == '\n'))
                    kept--;
                if (kept >= 8) { /* keep only if real content survives */
                    cur = kept;
                    acts |= HU_STYLE_GOV_ACTION_QUESTION_STRIPPED;
                }
            }
        }
    }

    /* Action A — strip a single terminal '.' (never "..", "...", "…", '?',
     * '!'), hash-gated to ~90% of period-ending messages so the corpus
     * no-terminal-punct rate (~4 in 5 per the card) is approached, not overshot to 100%. */
    if (cur >= 2 && text[cur - 1] == '.' && text[cur - 2] != '.' &&
        period_roll < HU_STYLE_GOV_PERIOD_STRIP_PCT) {
        cur--;
        acts |= HU_STYLE_GOV_ACTION_PERIOD_STRIPPED;
    }

    /* Action C — capitalize a lowercase start (and the start of each later
     * line: one bubble per line), gated so the card's lowercase-start rate is
     * kept rather than driven to zero. Length never changes. */
    bool capitalize = false;
    if (lowercase_start_pct < 100 && casing_roll >= lowercase_start_pct) {
        size_t i = line_start(text, 0, cur);
        if (i < cur && text[i] >= 'a' && text[i] <= 'z' && !starts_with_url(text + i, cur - i))
            capitalize = true;
    }

    if (acts == 0 && !capitalize)
        return HU_OK;

    char *shaped = (char *)alloc->alloc(alloc->ctx, cur + 1);
    if (!shaped)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(shaped, text, cur);
    shaped[cur] = '\0';
    if (capitalize) {
        size_t i = 0;
        while (i < cur) {
            i = line_start(shaped, i, cur);
            if (i < cur && shaped[i] >= 'a' && shaped[i] <= 'z' &&
                !starts_with_url(shaped + i, cur - i))
                shaped[i] = (char)(shaped[i] - 32);
            while (i < cur && shaped[i] != '\n')
                i++;
            if (i < cur)
                i++; /* past the newline */
        }
        acts |= HU_STYLE_GOV_ACTION_START_CAPITALIZED;
    }
    *out = shaped;
    *out_len = cur;
    *actions = acts;
    return HU_OK;
}

size_t hu_style_governor_apply_inplace(hu_allocator_t *alloc, char *buf, size_t len) {
    if (!alloc || !buf || len == 0)
        return len;
    hu_style_governor_mode_t mode = hu_style_governor_mode();
    if (mode == HU_STYLE_GOVERNOR_OFF)
        return len;

    char *shaped = NULL;
    size_t shaped_len = 0;
    unsigned actions = 0;
    unsigned roll = hu_style_governor_roll(buf, len);
    unsigned casing = hu_style_governor_casing_roll(buf, len);
    if (hu_style_governor_shape_ex(alloc, buf, len, roll, casing,
                                   hu_style_governor_lowercase_start_pct(NULL), &shaped,
                                   &shaped_len, &actions) != HU_OK ||
        !shaped)
        return len;

    if (mode == HU_STYLE_GOVERNOR_SHADOW) {
        hu_log_info("style_governor", NULL,
                    "shadow: would shape reactive reply (actions=0x%x, %zu -> %zu bytes)", actions,
                    len, shaped_len);
        alloc->free(alloc->ctx, shaped, shaped_len + 1);
        return len;
    }

    /* LIVE — the governor only ever shrinks, so copying back into `buf`
     * (which already held `len` bytes) is always in-bounds. */
    memcpy(buf, shaped, shaped_len);
    buf[shaped_len] = '\0';
    alloc->free(alloc->ctx, shaped, shaped_len + 1);
    return shaped_len;
}

/* ── Pipeline stage ───────────────────────────────────────────────── */

static atomic_bool s_logged_mode = false;

static hu_outbound_verdict_t style_governor_run(hu_outbound_pipeline_stage_t *self,
                                                hu_outbound_message_t *msg,
                                                hu_outbound_context_t *ctx) {
    (void)self;
    hu_style_governor_mode_t mode = hu_style_governor_mode();
    bool expected = false;
    if (atomic_compare_exchange_strong(&s_logged_mode, &expected, true)) {
        static const char *const names[] = {"off", "shadow", "live"};
        hu_log_info("style_governor", NULL,
                    "style governor mode=%s (HU_STYLE_GOVERNOR; measured card targets from "
                    "the persona style card: mostly no terminal punct, few ?-endings, "
                    "lowercase start %u%%)",
                    names[mode], hu_style_governor_lowercase_start_pct(ctx ? ctx->persona : NULL));
    }
    if (mode == HU_STYLE_GOVERNOR_OFF || !msg || !msg->content || msg->content_len == 0 || !ctx ||
        !ctx->alloc)
        return hu_outbound_verdict_send();

    char *shaped = NULL;
    size_t shaped_len = 0;
    unsigned actions = 0;
    unsigned roll = hu_style_governor_roll(msg->content, msg->content_len);
    unsigned casing = hu_style_governor_casing_roll(msg->content, msg->content_len);
    if (hu_style_governor_shape_ex(ctx->alloc, msg->content, msg->content_len, roll, casing,
                                   hu_style_governor_lowercase_start_pct(ctx->persona), &shaped,
                                   &shaped_len, &actions) != HU_OK ||
        !shaped)
        return hu_outbound_verdict_send();

    if (mode == HU_STYLE_GOVERNOR_SHADOW) {
        hu_log_info("style_governor", NULL, "shadow: would shape (actions=0x%x, %zu -> %zu bytes)",
                    actions, msg->content_len, shaped_len);
        ctx->alloc->free(ctx->alloc->ctx, shaped, shaped_len + 1);
        return hu_outbound_verdict_send();
    }
    return hu_outbound_verdict_rewrite("style_governor: measured-shape enforcement", shaped,
                                       shaped_len);
}

hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_style_governor = {
    .name = "style_governor",
    .run = style_governor_run,
    .state = NULL,
};
