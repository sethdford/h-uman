/* HU_IMMERSIVE_CONTEXT composer (turn context assembly) — contract:
 * include/human/agent/immersive_context.h. */
#include "human/agent/immersive_context.h"
#include "human/agent.h"
#include "human/core/log.h"
#include "human/providers/private_context.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* One item never exceeds the budget, so a line longer than this is dropped
 * whole (it could never fit) rather than cut. */
#define HU_IMMERSIVE_ITEM_MAX      (HU_IMMERSIVE_CONTEXT_BUDGET_BYTES * 2)
#define HU_IMMERSIVE_ITEM_TOO_LONG SIZE_MAX

/* Second-person lead-in per group: plain sentences about THEM, not headings. */
static const char *const k_group_leads[] = {
    "Still open between you two:\n",  "How they seem right now:\n",
    "On your mind about them:\n",     "Where you hope this conversation goes:\n",
    "From your last conversation:\n",
};
#define HU_IMMERSIVE_GROUPS (sizeof(k_group_leads) / sizeof(k_group_leads[0]))

/* First words that make a sentence an instruction to the model rather than a
 * fact about the person ("Generate a natural follow-up", "Follow up
 * naturally", "Be present", "Don't over-invest"). Matched as whole words. */
static const char *const k_directive_words[] = {
    "acknowledge", "always", "ask",      "avoid",    "be",    "bring", "calibrate",
    "celebrate",   "check",  "consider", "do",       "don't", "draw",  "encourage",
    "express",     "focus",  "follow",   "generate", "give",  "go",    "honor",
    "keep",        "lead",   "lean",     "let",      "make",  "match", "mention",
    "mirror",      "never",  "note",     "offer",    "open",  "pick",  "reference",
    "remember",    "reply",  "respond",  "say",      "share", "show",  "slow",
    "start",       "stay",   "take",     "tell",     "try",   "use",   "validate",
    "weave",
};

/* Phrases that mark AI framing or instructions anywhere in a sentence. */
static const char *const k_directive_phrases[] = {
    "you are tracking", "you're tracking", "you should",   "you must",
    "the user",         "the assistant",   "this message",
};

/* Item labels that carry only a metric, never a fact ("Confidence: 70%"). */
static const char *const k_metric_labels[] = {"confidence", "quality", "score", "intensity",
                                              "valence"};

/* Short words dedupe ignores; content words are >= 4 letters and not here. */
static const char *const k_stop_words[] = {"they",  "their", "them", "with", "this", "that",
                                           "about", "have",  "from", "your", "what", "when",
                                           "were",  "been",  "will", "said", "words"};

typedef struct immersive_source {
    const char *text;
    size_t len;
    size_t group;
    size_t max_items;
    unsigned bit;
    bool commitment; /* "summary (by OWNER, DATE)" → "summary (their words)" */
    bool directives; /* builder text mixes facts with instructions: keep facts */
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

static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '\'';
}

/* Case-insensitive whole-word search (substring-classifier-pitfalls.md). */
static bool contains_word_ci(const char *hay, size_t hay_len, const char *w, size_t wlen) {
    if (!hay || wlen == 0 || hay_len < wlen)
        return false;
    for (size_t i = 0; i + wlen <= hay_len; i++) {
        size_t k = 0;
        while (k < wlen && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)w[k]))
            k++;
        if (k != wlen)
            continue;
        bool left = i == 0 || !isalnum((unsigned char)hay[i - 1]);
        bool right = i + wlen == hay_len || !isalnum((unsigned char)hay[i + wlen]);
        if (left && right)
            return true;
    }
    return false;
}

static bool word_in_list(const char *w, size_t n, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (strlen(list[i]) == n && strncasecmp(w, list[i], n) == 0)
            return true;
    return false;
}

/* Builder chrome → plain text: list markers, "[TAG: body]" / "[TAG] body"
 * wrappers, an ALL-CAPS "TAG: " prefix and markdown emphasis go. Returns the
 * new length (0 = a heading, placeholder or section label: no item). */
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
    trim(&s, &n);
    { /* "COMMITMENT FOLLOW-UP: body" */
        size_t i = 0, letters = 0;
        while (i < n && (isupper((unsigned char)s[i]) || s[i] == ' ' || s[i] == '-' ||
                         s[i] == '<' || s[i] == '>' || s[i] == '_')) {
            letters += isupper((unsigned char)s[i]) ? 1 : 0;
            i++;
        }
        if (letters >= 2 && i + 1 < n && s[i] == ':' && s[i + 1] == ' ') {
            s += i + 2;
            n -= i + 2;
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

/* Next item: one top-level line. Its indented continuation lines (a goal's
 * "Success signal:", a wrapped directive) are always consumed and dropped —
 * also when the first line is oversize, so they never surface as items of
 * their own. Returns the length in buf, HU_IMMERSIVE_ITEM_TOO_LONG, or 0 when
 * the field is exhausted. */
static size_t next_item(item_cursor_t *c, char *buf, size_t cap) {
    while (c->pos < c->len) {
        const char *line = c->text + c->pos;
        const char *nl = memchr(line, '\n', c->len - c->pos);
        size_t ll = nl ? (size_t)(nl - line) : c->len - c->pos;
        c->pos += ll + (nl ? 1 : 0);
        size_t n = normalize(line, ll, buf, cap);
        while (c->pos < c->len && is_space(c->text[c->pos])) {
            const char *cl = c->text + c->pos;
            const char *cnl = memchr(cl, '\n', c->len - c->pos);
            size_t cll = cnl ? (size_t)(cnl - cl) : c->len - c->pos;
            const char *probe = cl;
            size_t pn = cll;
            trim(&probe, &pn);
            if (pn == 0)
                break;
            c->pos += cll + (cnl ? 1 : 0);
        }
        if (n == 0)
            continue;
        return n;
    }
    return 0;
}

/* Drop "(...)" groups that hold a digit: dates, ids, metrics, attempt counts. */
static size_t strip_numeric_parens(char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '(') {
            size_t j = i + 1;
            bool digit = false;
            while (j < n && s[j] != ')') {
                digit = digit || isdigit((unsigned char)s[j]);
                j++;
            }
            if (j < n && digit) {
                while (o > 0 && s[o - 1] == ' ')
                    o--;
                i = j;
                continue;
            }
        }
        s[o++] = s[i];
    }
    return o;
}

static bool sentence_is_directive(const char *s, size_t n) {
    trim(&s, &n);
    size_t w = 0;
    while (w < n && (is_word_char(s[w]) || s[w] == '-'))
        w++;
    if (w == 0)
        return true; /* no words: nothing about them */
    if (word_in_list(s, w, k_directive_words,
                     sizeof(k_directive_words) / sizeof(k_directive_words[0])))
        return true;
    for (size_t p = 0; p < sizeof(k_directive_phrases) / sizeof(k_directive_phrases[0]); p++)
        if (contains_word_ci(s, n, k_directive_phrases[p], strlen(k_directive_phrases[p])))
            return true;
    size_t colon = 0;
    while (colon < n && s[colon] != ':')
        colon++;
    return colon < n && word_in_list(s, colon, k_metric_labels,
                                     sizeof(k_metric_labels) / sizeof(k_metric_labels[0]));
}

/* Keep only the sentences that state a fact; a kept sentence loses a
 * " — <directive>" tail ("They took a risk — honor that by …"). */
static size_t keep_fact_sentences(char *s, size_t n) {
    size_t o = 0, i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n &&
               !((s[i] == '.' || s[i] == '!' || s[i] == '?') && (i + 1 == n || s[i + 1] == ' ')))
            i++;
        size_t end = i < n ? i + 1 : n; /* include the terminator */
        i = end;
        while (i < n && s[i] == ' ')
            i++;
        const char *dash = NULL;
        for (size_t k = start; k + 5 <= end; k++)
            if (memcmp(s + k, " \xE2\x80\x94 ", 5) == 0 &&
                sentence_is_directive(s + k + 5, end - k - 5)) {
                dash = s + k;
                break;
            }
        size_t keep_end = dash ? (size_t)(dash - s) : end;
        if (sentence_is_directive(s + start, keep_end - start))
            continue;
        if (o > 0)
            s[o++] = ' ';
        memmove(s + o, s + start, keep_end - start);
        o += keep_end - start;
        if (dash)
            s[o++] = '.';
    }
    return o;
}

/* "call my sister tomorrow (by user, 2026-10-01)" → "call my sister tomorrow
 * (their words)": who said it, without the store's id/date chrome. */
static size_t rewrite_commitment(char *s, size_t n, size_t cap) {
    const char *by = NULL;
    for (size_t i = 0; i + 4 < n; i++)
        if (memcmp(s + i, " (by ", 5) == 0)
            by = s + i;
    if (!by || s[n - 1] != ')')
        return n;
    bool theirs = starts_with(by + 5, n - (size_t)(by + 5 - s), "user");
    static const char k_theirs[] = " (their words)";
    static const char k_yours[] = " (your words)";
    const char *tag = theirs ? k_theirs : k_yours;
    size_t tlen = strlen(tag);
    size_t head = (size_t)(by - s);
    if (head + tlen >= cap)
        return n;
    memcpy(s + head, tag, tlen);
    return head + tlen;
}

/* Duplicate when >= 2/3 of the item's content words already appear in Core
 * Memory or earlier in the block. */
static bool is_duplicate(const char *item, size_t n, const char *mem, size_t mem_len,
                         const char *block, size_t block_len) {
    size_t words = 0, hits = 0;
    /* A builder label ("Active commitments: …") is not content: compare the
     * body only, or the label's own words dilute a real repeat below 2/3. */
    for (size_t k = 0; k + 1 < n && k < 40 && item[k] != '.'; k++)
        if (item[k] == ':' && item[k + 1] == ' ') {
            item += k + 2;
            n -= k + 2;
            break;
        }
    for (size_t i = 0; i < n;) {
        while (i < n && !isalnum((unsigned char)item[i]))
            i++;
        size_t start = i;
        while (i < n && isalnum((unsigned char)item[i]))
            i++;
        size_t wl = i - start;
        if (wl < 4 || word_in_list(item + start, wl, k_stop_words,
                                   sizeof(k_stop_words) / sizeof(k_stop_words[0])))
            continue;
        words++;
        if (contains_word_ci(mem, mem_len, item + start, wl) ||
            contains_word_ci(block, block_len, item + start, wl))
            hits++;
    }
    return words > 0 && hits * 3 >= words * 2;
}

static void append_bytes(char *buf, size_t *len, const char *s, size_t n) {
    memcpy(buf + *len, s, n);
    *len += n;
}

/* An item ready to emit, or 0 when it is chrome, a directive or a duplicate. */
static size_t shape_item(const immersive_source_t *src, char *item, size_t n,
                         const hu_prompt_config_t *cfg, const char *block, size_t block_len,
                         hu_immersive_context_stats_t *st) {
    if (src->commitment)
        n = rewrite_commitment(item, n, HU_IMMERSIVE_ITEM_MAX);
    else
        n = strip_numeric_parens(item, n);
    /* Commitments and goals are verb phrases by construction ("tell mom
     * about the trip"); only the free-text builders carry directives. */
    size_t facts = src->directives ? keep_fact_sentences(item, n) : n;
    if (facts == 0) {
        st->directives_dropped++;
        return 0;
    }
    if (is_duplicate(item, facts, cfg->memory_context, cfg->memory_context_len, block, block_len)) {
        st->duplicates_dropped++;
        return 0;
    }
    return facts;
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

    /* Priority order. episodic_replay is NOT a source: it holds global
     * problem-solving patterns retrieved by message text, not memories shared
     * with this person. presence_context is NOT a source: it is an attention
     * directive to the model ("Light mode. Brief, breezy."), not a fact about
     * them. Both are already-rendered builder output; the composer keeps only
     * the facts in it. */
    const immersive_source_t sources[] = {
        {cfg->commitment_context, cfg->commitment_context_len, 0, 3, HU_IMMERSIVE_CTX_COMMITMENT,
         true, false},
        {cfg->emotional_context, cfg->emotional_context_len, 1, 2, HU_IMMERSIVE_CTX_EMOTIONAL,
         false, true},
        {cfg->proactive_context, cfg->proactive_context_len, 2, 1, HU_IMMERSIVE_CTX_PROACTIVE,
         false, true},
        {cfg->superhuman_context, cfg->superhuman_context_len, 2, 1, HU_IMMERSIVE_CTX_SUPERHUMAN,
         false, true},
        {cfg->conv_goals_context, cfg->conv_goals_context_len, 3, 2, HU_IMMERSIVE_CTX_CONV_GOALS,
         false, false},
        {cfg->residue_carryover, cfg->residue_carryover_len, 4, 1, HU_IMMERSIVE_CTX_RESIDUE, false,
         true},
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
            if (n != HU_IMMERSIVE_ITEM_TOO_LONG) {
                n = shape_item(src, item, n, cfg, buf + heading_len, len - heading_len, &st);
                if (n == 0)
                    continue;
            }
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
            *stats = st;
        return HU_OK;
    }
    append_bytes(buf, &len, "\n", 1); /* blank line closes the block */
    buf[len] = '\0';
    st.bytes = len;
    if (stats)
        *stats = st;
    char *fit = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!fit) {
        alloc->free(alloc->ctx, buf, budget + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(fit, buf, len + 1);
    alloc->free(alloc->ctx, buf, budget + 1);
    *out = fit;
    *out_len = len;
    return HU_OK;
}

bool hu_immersive_context_turn_is_local(const struct hu_agent *agent) {
    if (!agent)
        return false;
    bool routed = agent->turn_model && agent->turn_model_len > 0;
    return hu_private_context_attempt_is_local(
        &agent->provider, routed ? agent->turn_model : agent->model_name,
        routed ? agent->turn_model_len : agent->model_name_len);
}

hu_gate_mode_t hu_immersive_context_mode(void) {
    return hu_gate_mode_from_env("HU_IMMERSIVE_CONTEXT", HU_GATE_OFF);
}

hu_error_t hu_immersive_context_for_prompt(hu_allocator_t *alloc, const hu_prompt_config_t *cfg,
                                           char **out, size_t *out_len) {
    if (!out || !out_len || !cfg)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    /* HU_IMMERSIVE_CONTEXT activation gated on the shadow byte/field
     * distribution plus an n=40 blind A/B through hu_agent_turn with real
     * contact state (docs/guides/immersive-context.md — that harness does not
     * exist yet) and the matched-length specificity scorer: do not flip to
     * default-ON without a measurement showing replies with it read at least
     * as much like the owner (.claude/rules/feature-gate-requires-measurement.md). */
    hu_gate_mode_t mode = hu_immersive_context_mode();
    if (mode == HU_GATE_OFF)
        return HU_OK;
    const char *tag = mode == HU_GATE_LIVE ? "live" : "shadow";
    /* Owner memory text: composed only for a turn resolved to a local
     * provider AND a local model. A later reroute to a cloud model is
     * stripped per attempt (providers/private_context.h). */
    if (!cfg->private_context_local) {
        hu_log_info("immersive_context", NULL,
                    "[HU_IMMERSIVE_CONTEXT %s] skipped: provider or model not local", tag);
        return HU_OK;
    }
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
                "bytes=%zu truncated=%d directives_dropped=%zu duplicates_dropped=%zu",
                tag, st.fields_used, st.field_mask, st.items_used, st.bytes, st.truncated ? 1 : 0,
                st.directives_dropped, st.duplicates_dropped);
    if (mode != HU_GATE_LIVE) {
        if (block)
            alloc->free(alloc->ctx, block, block_len + 1);
        return HU_OK;
    }
    *out = block;
    *out_len = block_len;
    return HU_OK;
}
