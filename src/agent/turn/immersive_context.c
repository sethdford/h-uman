/* HU_IMMERSIVE_CONTEXT composer (turn context assembly) — contract:
 * include/human/agent/immersive_context.h. */
#include "human/agent/immersive_context.h"
#include "human/core/log.h"
#include "human/providers/private_context.h"
#include <stdint.h>
#include <string.h>

/* One item never exceeds the budget, so a line longer than this is dropped
 * whole (it could never fit) rather than cut. */
#define HU_IMMERSIVE_ITEM_MAX      (HU_IMMERSIVE_CONTEXT_BUDGET_BYTES * 2)
#define HU_IMMERSIVE_ITEM_TOO_LONG SIZE_MAX

/* Second-person lead-in per priority group: plain sentences, not headings. */
static const char *const k_group_leads[] = {
    "Still open between you two:\n",
    "Something that has worked with them before:\n",
    "How they seem right now:\n",
    "On your mind about them:\n",
    "Where you hope this conversation goes:\n",
};
#define HU_IMMERSIVE_GROUPS (sizeof(k_group_leads) / sizeof(k_group_leads[0]))

typedef struct immersive_source {
    const char *text;
    size_t len;
    size_t group;
    size_t max_items;
    unsigned bit;
} immersive_source_t;

typedef struct item_cursor {
    const char *text;
    size_t len;
    size_t pos;
} item_cursor_t;

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static void trim(const char **s, size_t *n) {
    while (*n > 0 && is_space(**s)) {
        (*s)++;
        (*n)--;
    }
    while (*n > 0 && is_space((*s)[*n - 1]))
        (*n)--;
}

static bool starts_with(const char *s, size_t n, const char *prefix) {
    size_t p = strlen(prefix);
    return n >= p && memcmp(s, prefix, p) == 0;
}

/* Builder chrome → plain text: drop a list marker, a "[TAG: body]" or
 * "[TAG] body" wrapper, and markdown emphasis. Returns the new length in out
 * (0 = nothing left: a heading, tag-only line or placeholder). */
static size_t normalize(const char *s, size_t n, char *out, size_t cap) {
    trim(&s, &n);
    if (n == 0 || s[0] == '#' || starts_with(s, n, "[No "))
        return 0;
    if (starts_with(s, n, "- ") || starts_with(s, n, "* ")) {
        s += 2;
        n -= 2;
    } else if (starts_with(s, n, "\xE2\x80\xA2 ")) { /* "• " */
        s += 4;
        n -= 4;
    } else {
        size_t d = 0;
        while (d < n && s[d] >= '0' && s[d] <= '9')
            d++;
        if (d > 0 && d + 1 < n && (s[d] == '.' || s[d] == ')') && s[d + 1] == ' ') {
            s += d + 2;
            n -= d + 2;
        }
    }
    if (n > 0 && s[0] == '[') {
        const char *close = memchr(s, ']', n);
        if (close) {
            size_t inner = (size_t)(close - s) - 1;
            const char *colon = NULL;
            for (size_t i = 1; i + 1 <= inner; i++)
                if (s[i] == ':' && s[i + 1] == ' ') {
                    colon = s + i;
                    break;
                }
            if (colon && close == s + n - 1) { /* "[TAG: body]" */
                n = (size_t)(close - colon) - 2;
                s = colon + 2;
            } else if (!colon) { /* "[TAG] body" or a bare tag */
                n -= (size_t)(close - s) + 1;
                s = close + 1;
            }
        }
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '*')
            continue;
        if (o + 1 >= cap)
            return HU_IMMERSIVE_ITEM_TOO_LONG;
        out[o++] = s[i];
    }
    const char *t = out;
    trim(&t, &o);
    if (o > 0 && t != out)
        memmove(out, t, o);
    if (o > 0 && out[o - 1] == ':') /* the builder's own section label */
        return 0;
    return o;
}

/* Next item: one top-level line plus its indented continuation lines. Returns
 * its length in buf, HU_IMMERSIVE_ITEM_TOO_LONG for an oversize item (the
 * cursor still advances past it), or 0 when the field is exhausted. */
static size_t next_item(item_cursor_t *c, char *buf, size_t cap) {
    while (c->pos < c->len) {
        const char *line = c->text + c->pos;
        const char *nl = memchr(line, '\n', c->len - c->pos);
        size_t ll = nl ? (size_t)(nl - line) : c->len - c->pos;
        c->pos += ll + (nl ? 1 : 0);
        size_t n = normalize(line, ll, buf, cap);
        if (n == 0)
            continue;
        while (n != HU_IMMERSIVE_ITEM_TOO_LONG && c->pos < c->len && is_space(c->text[c->pos])) {
            const char *cl = c->text + c->pos;
            const char *cnl = memchr(cl, '\n', c->len - c->pos);
            size_t cll = cnl ? (size_t)(cnl - cl) : c->len - c->pos;
            const char *probe = cl;
            size_t pn = cll;
            trim(&probe, &pn);
            if (pn == 0)
                break;
            c->pos += cll + (cnl ? 1 : 0);
            if (n + 1 >= cap) {
                n = HU_IMMERSIVE_ITEM_TOO_LONG;
                break;
            }
            buf[n] = ' ';
            size_t m = normalize(cl, cll, buf + n + 1, cap - n - 1);
            n = m == HU_IMMERSIVE_ITEM_TOO_LONG ? m : n + (m > 0 ? 1 + m : 0);
        }
        return n;
    }
    return 0;
}

static void append_bytes(char *buf, size_t *len, const char *s, size_t n) {
    memcpy(buf + *len, s, n);
    *len += n;
}

hu_error_t hu_immersive_context_compose(hu_allocator_t *alloc, const hu_prompt_config_t *cfg,
                                        size_t budget, char **out, size_t *out_len,
                                        hu_immersive_context_stats_t *stats) {
    if (!alloc || !cfg || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    hu_immersive_context_stats_t st;
    memset(&st, 0, sizeof(st));
    if (stats)
        *stats = st;

    const immersive_source_t sources[] = {
        {cfg->commitment_context, cfg->commitment_context_len, 0, 3, HU_IMMERSIVE_CTX_COMMITMENT},
        {cfg->episodic_replay, cfg->episodic_replay_len, 1, 1, HU_IMMERSIVE_CTX_EPISODIC},
        {cfg->emotional_context, cfg->emotional_context_len, 2, 2, HU_IMMERSIVE_CTX_EMOTIONAL},
        {cfg->presence_context, cfg->presence_context_len, 2, 1, HU_IMMERSIVE_CTX_PRESENCE},
        {cfg->proactive_context, cfg->proactive_context_len, 3, 1, HU_IMMERSIVE_CTX_PROACTIVE},
        {cfg->superhuman_context, cfg->superhuman_context_len, 3, 1, HU_IMMERSIVE_CTX_SUPERHUMAN},
        {cfg->conv_goals_context, cfg->conv_goals_context_len, 4, 2, HU_IMMERSIVE_CTX_CONV_GOALS},
        {cfg->residue_carryover, cfg->residue_carryover_len, 4, 1, HU_IMMERSIVE_CTX_RESIDUE},
    };
    static const char heading[] = HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT;
    const size_t heading_len = sizeof(heading) - 1;
    if (budget <= heading_len + 1)
        return HU_OK;

    char *buf = (char *)alloc->alloc(alloc->ctx, budget + 1);
    char *item = (char *)alloc->alloc(alloc->ctx, HU_IMMERSIVE_ITEM_MAX);
    if (!buf || !item) {
        if (buf)
            alloc->free(alloc->ctx, buf, budget + 1);
        if (item)
            alloc->free(alloc->ctx, item, HU_IMMERSIVE_ITEM_MAX);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    append_bytes(buf, &len, heading, heading_len);
    bool lead_done[HU_IMMERSIVE_GROUPS] = {false};

    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
        const immersive_source_t *src = &sources[s];
        if (!src->text || src->len == 0)
            continue;
        item_cursor_t cur = {src->text, src->len, 0};
        size_t taken = 0;
        size_t n;
        while (taken < src->max_items && (n = next_item(&cur, item, HU_IMMERSIVE_ITEM_MAX)) != 0) {
            const char *lead = k_group_leads[src->group];
            size_t lead_len = lead_done[src->group] ? 0 : strlen(lead);
            /* item + "- " + "\n", plus the block's closing "\n" */
            if (n == HU_IMMERSIVE_ITEM_TOO_LONG || len + lead_len + n + 3 + 1 > budget) {
                st.truncated = true;
                continue;
            }
            if (lead_len > 0) {
                append_bytes(buf, &len, lead, lead_len);
                lead_done[src->group] = true;
            }
            append_bytes(buf, &len, "- ", 2);
            append_bytes(buf, &len, item, n);
            append_bytes(buf, &len, "\n", 1);
            taken++;
        }
        if (taken > 0) {
            st.fields_used++;
            st.field_mask |= src->bit;
            st.items_used += taken;
        }
    }
    alloc->free(alloc->ctx, item, HU_IMMERSIVE_ITEM_MAX);

    if (st.items_used == 0) {
        alloc->free(alloc->ctx, buf, budget + 1);
        if (stats)
            stats->truncated = st.truncated;
        return HU_OK;
    }
    append_bytes(buf, &len, "\n", 1); /* blank line closes the block */
    buf[len] = '\0';
    st.bytes = len;
    *out = buf;
    *out_len = len;
    if (stats)
        *stats = st;
    /* *out keeps the budget-sized allocation; record it so free(len + 1)
     * matches: shrink by copying when the block is shorter. */
    if (len < budget) {
        char *fit = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (!fit) {
            alloc->free(alloc->ctx, buf, budget + 1);
            *out = NULL;
            *out_len = 0;
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(fit, buf, len + 1);
        alloc->free(alloc->ctx, buf, budget + 1);
        *out = fit;
    }
    return HU_OK;
}

hu_gate_mode_t hu_immersive_context_mode(void) {
    return hu_gate_mode_from_env("HU_IMMERSIVE_CONTEXT", HU_GATE_OFF);
}

hu_error_t hu_immersive_context_for_prompt(hu_allocator_t *alloc, const hu_prompt_config_t *cfg,
                                           char **out, size_t *out_len) {
    if (!out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    /* HU_IMMERSIVE_CONTEXT activation gated on the shadow byte/field
     * distribution plus the n=40 blind A/B (scripts/blind_ab/) and the
     * matched-length specificity scorer: do not flip to default-ON without a
     * measurement showing replies with it read at least as much like the owner
     * (.claude/rules/feature-gate-requires-measurement.md,
     * docs/guides/immersive-context.md). */
    hu_gate_mode_t mode = hu_immersive_context_mode();
    if (mode == HU_GATE_OFF)
        return HU_OK;
    char *block = NULL;
    size_t block_len = 0;
    hu_immersive_context_stats_t st;
    hu_error_t err = hu_immersive_context_compose(alloc, cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES,
                                                  &block, &block_len, &st);
    if (err != HU_OK)
        return err;
    /* Counts, sizes and a field bitmask only — never the composed text. */
    hu_log_info("immersive_context", NULL,
                "[HU_IMMERSIVE_CONTEXT %s] fields_used=%zu field_mask=0x%02x items=%zu "
                "bytes=%zu truncated=%d",
                mode == HU_GATE_LIVE ? "live" : "shadow", st.fields_used, st.field_mask,
                st.items_used, st.bytes, st.truncated ? 1 : 0);
    if (mode != HU_GATE_LIVE) {
        if (block)
            alloc->free(alloc->ctx, block, block_len + 1);
        return HU_OK;
    }
    *out = block;
    *out_len = block_len;
    return HU_OK;
}
