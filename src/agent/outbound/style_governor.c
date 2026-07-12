/* outbound/style_governor.c — measured-shape enforcement stage.
 *
 * Enforces the persona's MEASURED texting shape at egress (style card
 * 2026-07-12, n=1488: 79% no-terminal-punct, 9% ?-endings — vs model
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

unsigned hu_style_governor_roll(const char *text, size_t len) {
    /* FNV-1a 32-bit. Same text → same roll: shaping is reproducible and
     * ~90% of period-ending messages strip (across distinct messages). */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)text[i];
        h *= 16777619u;
    }
    return (unsigned)(h % 100u);
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

hu_error_t hu_style_governor_shape(hu_allocator_t *alloc, const char *text, size_t len,
                                   unsigned period_roll, char **out, size_t *out_len,
                                   unsigned *actions) {
    if (!alloc || !text || !out || !out_len || !actions)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    *actions = 0;

    size_t cur = len;
    unsigned acts = 0;

    /* Action B — strip a trailing reciprocal-question boilerplate sentence
     * when real content precedes it (measured ?-ending rate is 9%; the
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
     * no-terminal-punct rate (~79%) is approached, not overshot to 100%. */
    if (cur >= 2 && text[cur - 1] == '.' && text[cur - 2] != '.' &&
        period_roll < HU_STYLE_GOV_PERIOD_STRIP_PCT) {
        cur--;
        acts |= HU_STYLE_GOV_ACTION_PERIOD_STRIPPED;
    }

    if (acts == 0 || cur == len)
        return HU_OK;

    char *shaped = (char *)alloc->alloc(alloc->ctx, cur + 1);
    if (!shaped)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(shaped, text, cur);
    shaped[cur] = '\0';
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
    if (hu_style_governor_shape(alloc, buf, len, roll, &shaped, &shaped_len, &actions) != HU_OK ||
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
                    "style governor mode=%s (HU_STYLE_GOVERNOR; measured card "
                    "targets: 79%% no-terminal-punct, 9%% ?-endings)",
                    names[mode]);
    }
    if (mode == HU_STYLE_GOVERNOR_OFF || !msg || !msg->content || msg->content_len == 0 || !ctx ||
        !ctx->alloc)
        return hu_outbound_verdict_send();

    char *shaped = NULL;
    size_t shaped_len = 0;
    unsigned actions = 0;
    unsigned roll = hu_style_governor_roll(msg->content, msg->content_len);
    if (hu_style_governor_shape(ctx->alloc, msg->content, msg->content_len, roll, &shaped,
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
