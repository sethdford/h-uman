/* outbound/sensitive.c — owner's-own-data disclosure gate.
 *
 * Asks the question no other outbound stage asks: does this reply contain
 * SETH's protected data? response_guard screens for AI-tells, crosstalk for
 * other contacts' content bleeding in, moderation for violence/hate. None of
 * them look at the owner's own address, cards, or credentials.
 *
 * See include/human/agent/outbound_sensitive.h for the tier doctrine and why
 * hu_sensitivity_classify_message is deliberately NOT reused wholesale.
 *
 * SENSITIVE activation gated on a shadow-mode false-positive measurement: do
 * not flip HU_OUTBOUND_SENSITIVE to `live` without reading a shadow sample
 * and confirming that legitimate city / employer / kid-name messages are not
 * being flagged. Seth says those things constantly and blocking them is worse
 * than the leak. Default is SHADOW (not OFF) precisely so that sample
 * accumulates at zero risk.
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md: one-shot log on
 * first invocation states the resolved mode, and a separate one-shot log
 * fires when no protected values are declared (the fail-open path), naming
 * the config key that would populate them.
 *
 * Returns:
 *   SEND       — clean, or SHADOW, or a TRUST_GATED finding (not yet enforced)
 *   REGENERATE — LIVE + NEVER_SEND + a regenerate still in budget
 *   REWRITE    — LIVE + NEVER_SEND + budget spent → safe deflection
 */

#include "human/agent/outbound_sensitive.h"

#include "human/agent/outbound_pipeline.h"
#include "human/core/log.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Longest declared value we will match. Bounds the window overlap below, so
 * no match can straddle a window boundary undetected. */
#define SENS_MAX_VALUE_LEN 256u

/* Source bytes normalized per pass. Normalization never expands (each input
 * byte yields at most one output byte), so the destination is the same size. */
#define SENS_WINDOW 4096u
/* Overlap is in SOURCE bytes but bounds a match measured in NORMALIZED chars,
 * and normalization discards separators — so a 256-char normalized value can
 * span considerably more than 256 source bytes ("4341 . . . 34th - - st").
 * 4x the value cap keeps a heavily-punctuated match from straddling a window
 * boundary and being missed; the cost is re-scanning 1 KiB per window. */
#define SENS_OVERLAP (4u * SENS_MAX_VALUE_LEN)

/* ── Mode resolution (env, cached) ────────────────────────────────────── */

static atomic_int s_mode = -1; /* -1 = unresolved */

hu_sensitive_mode_t hu_outbound_sensitive_mode(void) {
    int m = atomic_load_explicit(&s_mode, memory_order_relaxed);
    if (m >= 0)
        return (hu_sensitive_mode_t)m;
    const char *env = getenv("HU_OUTBOUND_SENSITIVE");
    /* DEFAULT SHADOW — see the header. Only explicit "off"/"live" move it. */
    hu_sensitive_mode_t resolved = HU_SENSITIVE_MODE_SHADOW;
    if (env && env[0]) {
        if (strcmp(env, "live") == 0)
            resolved = HU_SENSITIVE_MODE_LIVE;
        else if (strcmp(env, "off") == 0)
            resolved = HU_SENSITIVE_MODE_OFF;
    }
    atomic_store_explicit(&s_mode, (int)resolved, memory_order_relaxed);
    return resolved;
}

#if HU_IS_TEST
void hu_outbound_sensitive_set_mode_for_test(int mode) {
    atomic_store_explicit(&s_mode, mode, memory_order_relaxed);
}
#endif

/* ── Protected-set provider ───────────────────────────────────────────── */

/* Same plain-static storage as hu_outbound_crosstalk_set_lookup: registered
 * once during startup, read-only thereafter. */
static hu_sensitive_provider_fn_t s_provider = NULL;
static void *s_provider_userdata = NULL;

void hu_outbound_sensitive_set_provider(hu_sensitive_provider_fn_t fn, void *userdata) {
    s_provider = fn;
    s_provider_userdata = userdata;
}

const hu_sensitive_set_t *hu_outbound_sensitive_current_set(void) {
    if (!s_provider)
        return NULL;
    const hu_sensitive_set_t *set = NULL;
    if (s_provider(s_provider_userdata, &set) != 0)
        return NULL;
    return set;
}

/* ── Names + default tiers ────────────────────────────────────────────── */

const char *hu_sensitive_tier_str(hu_sensitive_tier_t tier) {
    switch (tier) {
    case HU_SENSITIVE_TIER_NONE:
        return "none";
    case HU_SENSITIVE_TIER_NEVER_SEND:
        return "never_send";
    case HU_SENSITIVE_TIER_TRUST_GATED:
        return "trust_gated";
    }
    return "unknown";
}

const char *hu_sensitive_category_str(hu_sensitive_category_t cat) {
    switch (cat) {
    case HU_SENSITIVE_CAT_NONE:
        return "none";
    case HU_SENSITIVE_CAT_STREET_ADDRESS:
        return "street_address";
    case HU_SENSITIVE_CAT_HARD_SECRET:
        return "hard_secret";
    case HU_SENSITIVE_CAT_EMPLOYER:
        return "employer";
    case HU_SENSITIVE_CAT_CITY:
        return "city";
    case HU_SENSITIVE_CAT_FAMILY_NAME:
        return "family_name";
    case HU_SENSITIVE_CAT_FINANCIAL:
        return "financial";
    }
    return "unknown";
}

hu_sensitive_tier_t hu_sensitive_category_default_tier(hu_sensitive_category_t cat) {
    switch (cat) {
    case HU_SENSITIVE_CAT_STREET_ADDRESS:
    case HU_SENSITIVE_CAT_HARD_SECRET:
        return HU_SENSITIVE_TIER_NEVER_SEND;
    case HU_SENSITIVE_CAT_EMPLOYER:
    case HU_SENSITIVE_CAT_CITY:
    case HU_SENSITIVE_CAT_FAMILY_NAME:
    case HU_SENSITIVE_CAT_FINANCIAL:
        return HU_SENSITIVE_TIER_TRUST_GATED;
    case HU_SENSITIVE_CAT_NONE:
        return HU_SENSITIVE_TIER_NONE;
    }
    return HU_SENSITIVE_TIER_NONE;
}

/* ── Normalization ────────────────────────────────────────────────────── */

/* Content bytes: alphanumerics plus any high byte (so non-ASCII names survive
 * intact). Everything else is a separator. */
static bool sens_content_byte(unsigned char c) {
    return isalnum(c) || c >= 0x80;
}

size_t hu_sensitive_normalize(const char *src, size_t src_len, char *dst, size_t cap) {
    if (!dst || cap == 0)
        return 0;
    dst[0] = '\0';
    if (!src || src_len == 0)
        return 0;

    size_t w = 0;
    bool pending_space = false;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];

        if (sens_content_byte(c)) {
            if (pending_space) {
                if (w + 2 >= cap)
                    break; /* no room for separator + byte */
                dst[w++] = ' ';
                pending_space = false;
            } else if (w + 1 >= cap) {
                break;
            }
            dst[w++] = (char)tolower(c);
            continue;
        }

        /* Digit-group separator BETWEEN digits is dropped outright, so
         * "$1,250,000" and "1250000" normalize alike. Elsewhere ',' and '.'
         * are ordinary separators. */
        if ((c == ',' || c == '.') && i > 0 && i + 1 < src_len &&
            isdigit((unsigned char)src[i - 1]) && isdigit((unsigned char)src[i + 1]))
            continue;

        if (w > 0)
            pending_space = true; /* never leading; trailing never emitted */
    }
    dst[w] = '\0';
    return w;
}

/* Word-boundary containment over two NORMALIZED strings. Because normalized
 * text is content bytes separated by single spaces, a boundary is a space or
 * a string end.
 *
 * Bare substring matching here would be the bug in
 * ~/.claude/rules/substring-classifier-pitfalls.md: a protected "ford" would
 * fire on "afford", a protected city "erie" on "experience". */
static bool sens_contains_word(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || hay[i - 1] == ' ';
        bool right_ok = (i + nlen == hlen) || hay[i + nlen] == ' ';
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

/* ── Street-address shape ─────────────────────────────────────────────── */

static const char *const SENS_STREET_TYPES[] = {
    "st",      "street", "ave",     "avenue", "rd",     "road",   "blvd",    "boulevard",
    "ln",      "lane",   "dr",      "drive",  "ct",     "court",  "way",     "pl",
    "place",   "ter",    "terrace", "cir",    "circle", "pkwy",   "parkway", "hwy",
    "highway", "trl",    "trail",   "loop",   "sq",     "square", "aly",     "alley",
};
#define SENS_STREET_TYPE_COUNT (sizeof(SENS_STREET_TYPES) / sizeof(SENS_STREET_TYPES[0]))

static const char *const SENS_DIRECTIONALS[] = {
    "n", "s", "e", "w", "ne", "nw", "se", "sw", "north", "south", "east", "west",
};
#define SENS_DIRECTIONAL_COUNT (sizeof(SENS_DIRECTIONALS) / sizeof(SENS_DIRECTIONALS[0]))

/* Unit designators end the matchable core: they are the part that differs
 * between "4341 34th St S" and "4341 34TH ST S APT 652". */
static const char *const SENS_UNIT_TOKENS[] = {
    "apt", "apartment", "unit", "suite", "ste", "fl", "floor", "rm", "room", "bldg", "building",
};
#define SENS_UNIT_TOKEN_COUNT (sizeof(SENS_UNIT_TOKENS) / sizeof(SENS_UNIT_TOKENS[0]))

#define SENS_MAX_TOKENS 32u

typedef struct sens_token {
    const char *p;
    size_t len;
} sens_token_t;

static size_t sens_tokenize(const char *norm, size_t len, sens_token_t *out, size_t max) {
    size_t n = 0;
    size_t i = 0;
    while (i < len && n < max) {
        while (i < len && norm[i] == ' ')
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && norm[i] != ' ')
            i++;
        out[n].p = norm + start;
        out[n].len = i - start;
        n++;
    }
    return n;
}

static bool sens_token_in(const sens_token_t *t, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        size_t l = strlen(list[i]);
        if (t->len == l && memcmp(t->p, list[i], l) == 0)
            return true;
    }
    return false;
}

static bool sens_token_is_house_number(const sens_token_t *t) {
    if (t->len == 0 || t->len > 6)
        return false;
    for (size_t i = 0; i < t->len; i++)
        if (!isdigit((unsigned char)t->p[i]))
            return false;
    return true;
}

/* How far past the house number a street-type token may sit. Covers
 * "4341 34th st" (1) through "1 north grand central ave" (4). */
#define SENS_STREET_TYPE_LOOKAHEAD 4u

size_t hu_sensitive_address_core(const char *src, size_t src_len, char *dst, size_t cap) {
    if (!dst || cap == 0)
        return 0;
    dst[0] = '\0';

    char norm[SENS_MAX_VALUE_LEN + 1];
    size_t nlen = hu_sensitive_normalize(src, src_len, norm, sizeof(norm));
    if (nlen == 0)
        return 0;

    sens_token_t toks[SENS_MAX_TOKENS];
    size_t ntok = sens_tokenize(norm, nlen, toks, SENS_MAX_TOKENS);
    if (ntok < 2)
        return 0;

    for (size_t h = 0; h + 1 < ntok; h++) {
        if (!sens_token_is_house_number(&toks[h]))
            continue;

        size_t limit = h + 1 + SENS_STREET_TYPE_LOOKAHEAD;
        if (limit > ntok)
            limit = ntok;

        for (size_t t = h + 1; t < limit; t++) {
            /* A unit designator before any street type means this digit run
             * was not a house number (e.g. "apt 652 ..."). */
            if (sens_token_in(&toks[t], SENS_UNIT_TOKENS, SENS_UNIT_TOKEN_COUNT))
                break;
            if (!sens_token_in(&toks[t], SENS_STREET_TYPES, SENS_STREET_TYPE_COUNT))
                continue;

            size_t last = t;
            /* Trailing directional is part of the address identity:
             * "34th St S" and "34th St N" are different streets. */
            if (last + 1 < ntok &&
                sens_token_in(&toks[last + 1], SENS_DIRECTIONALS, SENS_DIRECTIONAL_COUNT))
                last++;

            const char *begin = toks[h].p;
            const char *end = toks[last].p + toks[last].len;
            size_t span = (size_t)(end - begin);
            if (span == 0 || span + 1 > cap)
                return 0;
            memcpy(dst, begin, span);
            dst[span] = '\0';
            return span;
        }
    }
    return 0;
}

/* ── The pure predicate ───────────────────────────────────────────────── */

/* Does one declared value appear in an already-normalized window? */
static bool sens_value_matches(const hu_sensitive_value_t *v, const char *win, size_t win_len) {
    if (!v->value || v->value_len == 0 || v->value_len > SENS_MAX_VALUE_LEN)
        return false;

    char buf[SENS_MAX_VALUE_LEN + 1];

    if (v->category == HU_SENSITIVE_CAT_STREET_ADDRESS) {
        /* Shape AND owner-value match. A declared address with no extractable
         * core is SKIPPED, never downgraded to a substring match: falling back
         * would let a mis-declared value like "waterfront place" fire on the
         * owner's real messages about living on the water. */
        size_t core_len = hu_sensitive_address_core(v->value, v->value_len, buf, sizeof(buf));
        if (core_len == 0)
            return false;
        return sens_contains_word(win, win_len, buf, core_len);
    }

    size_t vlen = hu_sensitive_normalize(v->value, v->value_len, buf, sizeof(buf));
    if (vlen == 0)
        return false;
    return sens_contains_word(win, win_len, buf, vlen);
}

bool hu_sensitive_scan(const char *text, size_t text_len, const hu_sensitive_set_t *set,
                       hu_sensitive_finding_t *out) {
    hu_sensitive_finding_t found;
    memset(&found, 0, sizeof(found));
    if (out)
        memset(out, 0, sizeof(*out));
    if (!text || text_len == 0)
        return false;

    /* 1. Shape-definitive secrets. Need no declared value, so they work on a
     *    completely unconfigured deployment. */
    hu_hard_secret_kind_t kind = hu_sensitivity_hard_secret_shape(text, text_len);
    if (kind != HU_HARD_SECRET_NONE) {
        found.tier = HU_SENSITIVE_TIER_NEVER_SEND;
        found.category = HU_SENSITIVE_CAT_HARD_SECRET;
        found.secret_kind = kind;
        if (out)
            *out = found;
        return true; /* already the highest tier — nothing can outrank it */
    }

    /* 2. Declared values. Empty/absent set → fail open (address + tier-b
     *    checks become no-ops); the stage logs that once. */
    if (!set || !set->values || set->count == 0)
        return false;

    char win[SENS_WINDOW + 1];
    size_t step = SENS_WINDOW - SENS_OVERLAP;

    for (size_t start = 0; start < text_len;) {
        size_t chunk = text_len - start;
        if (chunk > SENS_WINDOW)
            chunk = SENS_WINDOW;
        size_t win_len = hu_sensitive_normalize(text + start, chunk, win, sizeof(win));

        for (size_t i = 0; i < set->count; i++) {
            const hu_sensitive_value_t *v = &set->values[i];
            if (!sens_value_matches(v, win, win_len))
                continue;

            hu_sensitive_tier_t tier = v->tier != HU_SENSITIVE_TIER_NONE
                                           ? v->tier
                                           : hu_sensitive_category_default_tier(v->category);
            if (tier <= found.tier)
                continue; /* highest tier wins */
            found.tier = tier;
            found.category = v->category;
            found.secret_kind = HU_HARD_SECRET_NONE;
            if (tier == HU_SENSITIVE_TIER_NEVER_SEND) {
                if (out)
                    *out = found;
                return true; /* cannot be outranked; stop early */
            }
        }

        if (start + chunk >= text_len)
            break;
        start += step;
    }

    if (found.tier == HU_SENSITIVE_TIER_NONE)
        return false;
    if (out)
        *out = found;
    return true;
}

/* ── Shared decision + logging ────────────────────────────────────────── */

static atomic_uint_least32_t s_mode_logged = 0;
static atomic_uint_least32_t s_no_values_logged = 0;

static void sens_log_mode_once(hu_sensitive_mode_t mode) {
    if (atomic_fetch_or_explicit(&s_mode_logged, 1u, memory_order_relaxed) != 0)
        return;
    const char *name = mode == HU_SENSITIVE_MODE_LIVE     ? "live"
                       : mode == HU_SENSITIVE_MODE_SHADOW ? "shadow"
                                                          : "off";
    hu_log_info("outbound_sensitive", NULL,
                "sensitive-disclosure gate mode=%s (HU_OUTBOUND_SENSITIVE; default shadow). "
                "shadow logs would-block decisions without altering any message.",
                name);
}

static void sens_log_no_values_once(void) {
    if (atomic_fetch_or_explicit(&s_no_values_logged, 1u, memory_order_relaxed) != 0)
        return;
    hu_log_info("outbound_sensitive", NULL,
                "no protected values declared — street-address and trust-gated checks are "
                "INACTIVE (card/SSN/credential shape checks still run). Populate "
                "privacy.never_send / privacy.trust_gated in config.json to enable them.");
}

/* Static reason strings — the verdict contract requires non-owned literals. */
static const char *sens_reason_for(const hu_sensitive_finding_t *f) {
    if (f->category == HU_SENSITIVE_CAT_HARD_SECRET)
        return "sensitive_never_send_hard_secret";
    if (f->category == HU_SENSITIVE_CAT_STREET_ADDRESS)
        return "sensitive_never_send_street_address";
    return "sensitive_never_send";
}

#define SENS_REGENERATE_HINT                                                                   \
    "Do NOT include any street address, account or card number, password, or access token in " \
    "the reply. Answer the message without that detail — do not mention that anything was "    \
    "withheld."

/* Log a finding. Categories and kinds only — never the matched bytes
 * (.claude/rules/quality-gates.md: never log secrets). */
static void sens_log_finding(const hu_sensitive_finding_t *f, hu_sensitive_mode_t mode,
                             const char *action) {
    hu_log_warn("outbound_sensitive", NULL,
                "%s: owner-data disclosure tier=%s category=%s secret_kind=%s mode=%s", action,
                hu_sensitive_tier_str(f->tier), hu_sensitive_category_str(f->category),
                hu_sensitivity_hard_secret_kind_str(f->secret_kind),
                mode == HU_SENSITIVE_MODE_LIVE ? "live" : "shadow");
}

/* ── Pipeline stage ───────────────────────────────────────────────────── */

static hu_outbound_verdict_t sensitive_run(hu_outbound_pipeline_stage_t *self,
                                           hu_outbound_message_t *msg, hu_outbound_context_t *ctx) {
    (void)self;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();
    if (!ctx || !ctx->alloc)
        return hu_outbound_verdict_send();

    hu_sensitive_mode_t mode = hu_outbound_sensitive_mode();
    sens_log_mode_once(mode);
    if (mode == HU_SENSITIVE_MODE_OFF)
        return hu_outbound_verdict_send();

    const hu_sensitive_set_t *set = hu_outbound_sensitive_current_set();
    if (!set || set->count == 0)
        sens_log_no_values_once();

    hu_sensitive_finding_t f;
    if (!hu_sensitive_scan(msg->content, msg->content_len, set, &f))
        return hu_outbound_verdict_send();

    if (f.tier == HU_SENSITIVE_TIER_TRUST_GATED) {
        /* TODO(contact-trust): tier (b) is DETECTED but NOT ENFORCED. Employer,
         * city, kids' names and financial amounts are fine for a known contact
         * and should only be suppressed for an unknown sender — enforcing them
         * without a trust signal would block the owner's ordinary messages,
         * which is the failure this whole stage is designed to avoid.
         *
         * The interface this needs is a recipient-trust predicate:
         *
         *     bool hu_contact_is_known(const char *contact_id, size_t len);
         *
         * When it lands, replace this early return with:
         *
         *     if (!hu_contact_is_known(ctx->recipient_contact_id,
         *                              ctx->recipient_contact_id_len))
         *         <fall through to the NEVER_SEND ladder below>
         *
         * Two signals already exist and were deliberately NOT adopted here,
         * because neither answers "may I tell this person where I live":
         *   - hu_channel_trust() classifies the CHANNEL (dm vs group), not the
         *     recipient.
         *   - hu_daemon_get_trust_state() / hu_tcal_* track how much the AI
         *     trusts a contact's ASSERTIONS (inbound provenance), which is a
         *     different axis from outbound disclosure.
         * The nearest usable proxy is a persona contact profile
         * (hu_persona_t.contacts[] keyed by contact_id, reachable through
         * ctx->persona) combined with channels.imessage.allow_from.
         *
         * Until then: log in shadow so the drip shows how often tier (b) would
         * fire and against which categories, and always SEND. */
        if (mode == HU_SENSITIVE_MODE_SHADOW)
            sens_log_finding(&f, mode, "SHADOW would-flag (trust-gated, not enforced)");
        return hu_outbound_verdict_send();
    }

    /* NEVER_SEND. */
    if (mode == HU_SENSITIVE_MODE_SHADOW) {
        sens_log_finding(&f, mode, "SHADOW would-block");
        return hu_outbound_verdict_send();
    }

    sens_log_finding(&f, mode, "BLOCK");

    /* Regenerate once with an explicit non-disclosure instruction. The
     * pipeline decrements the budget and hands the hint to the caller, which
     * owns the LLM call. */
    if (ctx->regenerate_budget > 0)
        return hu_outbound_verdict_regenerate(sens_reason_for(&f), SENS_REGENERATE_HINT);

    /* Budget spent and it STILL discloses — deflect rather than truncate. A
     * truncation would leave a mangled half-message; the deflection is a
     * coherent reply that happens to carry no protected value. */
    size_t dlen = strlen(HU_SENSITIVE_DEFLECTION);
    char *deflection = (char *)ctx->alloc->alloc(ctx->alloc->ctx, dlen + 1);
    if (!deflection)
        return hu_outbound_verdict_reject(sens_reason_for(&f)); /* fail closed */
    memcpy(deflection, HU_SENSITIVE_DEFLECTION, dlen + 1);
    return hu_outbound_verdict_rewrite(sens_reason_for(&f), deflection, dlen);
}

hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_sensitive = {
    .name = "sensitive",
    .run = sensitive_run,
    .state = NULL,
};

/* ── Reactive-path in-place entry ─────────────────────────────────────── */

size_t hu_outbound_sensitive_apply_inplace(char *buf, size_t len, size_t cap) {
    if (!buf || len == 0 || cap == 0)
        return len;

    hu_sensitive_mode_t mode = hu_outbound_sensitive_mode();
    sens_log_mode_once(mode);
    if (mode == HU_SENSITIVE_MODE_OFF)
        return len;

    const hu_sensitive_set_t *set = hu_outbound_sensitive_current_set();
    if (!set || set->count == 0)
        sens_log_no_values_once();

    hu_sensitive_finding_t f;
    if (!hu_sensitive_scan(buf, len, set, &f))
        return len;

    /* Tier (b) is detect-only until a recipient-trust predicate exists — see
     * the TODO in sensitive_run. */
    if (f.tier == HU_SENSITIVE_TIER_TRUST_GATED) {
        if (mode == HU_SENSITIVE_MODE_SHADOW)
            sens_log_finding(&f, mode, "SHADOW would-flag (trust-gated, not enforced)");
        return len;
    }

    if (mode == HU_SENSITIVE_MODE_SHADOW) {
        sens_log_finding(&f, mode, "SHADOW would-block");
        return len;
    }

    sens_log_finding(&f, mode, "BLOCK (reactive path — no regenerate available)");

    size_t dlen = strlen(HU_SENSITIVE_DEFLECTION);
    if (dlen + 1 <= cap) {
        memcpy(buf, HU_SENSITIVE_DEFLECTION, dlen + 1);
        return dlen;
    }
    /* Cannot fit the deflection — empty the buffer rather than ship a
     * partially-scrubbed body. Fail closed. */
    buf[0] = '\0';
    return 0;
}
