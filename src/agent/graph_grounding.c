#include "human/agent/graph_grounding.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/gate_mode.h"
#include "human/memory/graph.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

hu_graph_grounding_mode_t hu_graph_grounding_mode(void) {
    /* Default SHADOW as of 2026-05-31. A paired ON-vs-OFF A/B over 30 real
     * iMessage pairs (blinded Gemini judge; scripts/grounding_ab.py) measured
     * grounding's MARGINAL effect at a 43.3% ON-win-rate (ON 13 / OFF 17, 95%
     * Wilson CI [27.4, 60.8]) — i.e. NOT above 50%, CI crossing 50%, ON in fact
     * slightly losing. Per .claude/rules/feature-gate-requires-measurement.md, a
     * behavior that shapes the sent reply may not stay default-ON on an unproven
     * (here, negative) result: it runs in SHADOW (loaded + logged, NOT injected)
     * until a measurement substantiates it. Override: HU_GRAPH_GROUNDING=on
     * re-enables injection, =off disables entirely.
     *
     * 2026-07-25: the READ path behind this gate changed from static top-3
     * community summaries (query-independent; 274 shadow events collapsed to 5
     * distinct sizes) to query-conditioned composition
     * (hu_graph_ground_compose) per the MemORAI adaptive-retrieval consensus
     * (docs/research/2026-07-25-sota-gap-analysis.md §3). The gate default
     * stays SHADOW: promotion past shadow still requires a fresh blind A/B.
     * hu_graph_ground_compose fails open (no graph / no match -> empty). */
    switch (hu_gate_mode_from_env("HU_GRAPH_GROUNDING", HU_GATE_SHADOW)) {
    case HU_GATE_LIVE:
        return HU_GRAPH_GROUNDING_ON;
    case HU_GATE_SHADOW:
        return HU_GRAPH_GROUNDING_SHADOW;
    default:
        return HU_GRAPH_GROUNDING_OFF; /* "off" or any other value */
    }
}

/* ── Pure retrieval-scoring predicates ──────────────────────────────────── */

static bool gg_word_char(char c) {
    return isalnum((unsigned char)c) != 0;
}

/* Words too generic to carry relevance signal on their own. Kept tiny on
 * purpose: entity names in the graph are extracted noun phrases, so the
 * only stopwords that matter are the ones that sneak into multi-word
 * names ("the marina") or into casual message text. */
static bool gg_is_stopword(const char *w, size_t len) {
    static const char *const k_stop[] = {"the",  "and",  "for", "you",   "with", "that",
                                         "this", "have", "was", "are",   "but",  "not",
                                         "just", "what", "how", "about", "your", "its"};
    for (size_t i = 0; i < sizeof(k_stop) / sizeof(k_stop[0]); i++) {
        size_t sl = strlen(k_stop[i]);
        if (sl == len && strncasecmp(w, k_stop[i], len) == 0)
            return true;
    }
    return false;
}

/* Advance to the next scoreable word (alnum run, >= 3 chars, non-stopword)
 * in s[*pos..len). Returns false when exhausted. */
static bool gg_next_word(const char *s, size_t len, size_t *pos, size_t *w_start, size_t *w_len) {
    size_t i = *pos;
    while (i < len) {
        while (i < len && !gg_word_char(s[i]))
            i++;
        size_t start = i;
        while (i < len && gg_word_char(s[i]))
            i++;
        size_t wl = i - start;
        if (wl >= 3 && !gg_is_stopword(s + start, wl)) {
            *pos = i;
            *w_start = start;
            *w_len = wl;
            return true;
        }
    }
    *pos = i;
    return false;
}

/* Case-insensitive WORD-BOUNDARY containment: `w` must be bounded by
 * start/end-of-string or a non-alnum char on both sides, so "informal"
 * never matches needle "formal" (substring-classifier-pitfalls.md). */
static bool gg_contains_word_ci(const char *hay, size_t hay_len, const char *w, size_t w_len) {
    if (!hay || !w || w_len == 0 || hay_len < w_len)
        return false;
    for (size_t i = 0; i + w_len <= hay_len; i++) {
        if (strncasecmp(hay + i, w, w_len) != 0)
            continue;
        bool left_ok = (i == 0) || !gg_word_char(hay[i - 1]);
        bool right_ok = (i + w_len == hay_len) || !gg_word_char(hay[i + w_len]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

size_t hu_graph_ground_name_word_count(const char *name, size_t name_len) {
    if (!name || name_len == 0)
        return 0;
    size_t pos = 0, ws = 0, wl = 0, n = 0;
    while (gg_next_word(name, name_len, &pos, &ws, &wl))
        n++;
    return n;
}

size_t hu_graph_ground_entity_match_count(const char *msg, size_t msg_len, const char *name,
                                          size_t name_len) {
    if (!msg || msg_len == 0 || !name || name_len == 0)
        return 0;
    size_t pos = 0, ws = 0, wl = 0, matched = 0;
    while (gg_next_word(name, name_len, &pos, &ws, &wl)) {
        if (gg_contains_word_ci(msg, msg_len, name + ws, wl))
            matched++;
    }
    return matched;
}

double hu_graph_ground_score(size_t match_count, size_t name_word_count, int32_t mention_count,
                             int64_t last_seen_ms, int64_t now_ms) {
    if (match_count == 0 || name_word_count == 0)
        return 0.0;
    if (match_count > name_word_count)
        match_count = name_word_count;
    double coverage = (double)match_count / (double)name_word_count;
    double mention = 0.0;
    if (mention_count > 0) {
        int32_t capped = mention_count > 16 ? 16 : mention_count;
        mention = ((double)capped / 16.0) * 0.25;
    }
    double recency = 0.0;
    if (last_seen_ms > 0) {
        if (now_ms > last_seen_ms) {
            double age_days = (double)(now_ms - last_seen_ms) / 86400000.0;
            recency = 0.25 / (1.0 + age_days / 30.0);
        } else {
            recency = 0.25; /* seen "now" or clock skew: full recency credit */
        }
    }
    return coverage + mention + recency;
}

uint32_t hu_graph_ground_fingerprint(const char *content, size_t len) {
    if (!content || len == 0)
        return 0;
    if (len > 40)
        len = 40;
    uint32_t h = 2166136261u; /* FNV-1a 32-bit */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)content[i];
        h *= 16777619u;
    }
    return h;
}

/* ── Query-conditioned composition ──────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

enum {
    GG_CANDIDATE_LIMIT = 64, /* top entities by mention_count considered */
    GG_TOP_K = 4,            /* seed entities composed into the context */
    GG_NEIGHBORS_PER_SEED = 4,
    GG_CONTEXT_SNIPPET_MAX = 96, /* relation `context` excerpt cap */
};

/* Append at most `n` bytes of `s` to buf (capacity max_chars, current *len),
 * stopping at the cap. Returns false when the cap is hit. */
static bool gg_append(char *buf, size_t max_chars, size_t *len, const char *s, size_t n) {
    if (*len + n > max_chars)
        return false;
    memcpy(buf + *len, s, n);
    *len += n;
    return true;
}

/* Append a single-line snippet of relation context (": <text>"), stopping at
 * the first newline and GG_CONTEXT_SNIPPET_MAX bytes. Best-effort. */
static void gg_append_context_snippet(char *buf, size_t max_chars, size_t *len, const char *ctx,
                                      size_t ctx_len) {
    if (!ctx || ctx_len == 0)
        return;
    size_t n = ctx_len > GG_CONTEXT_SNIPPET_MAX ? GG_CONTEXT_SNIPPET_MAX : ctx_len;
    for (size_t i = 0; i < n; i++) {
        if (ctx[i] == '\n' || ctx[i] == '\r') {
            n = i;
            break;
        }
    }
    if (n == 0)
        return;
    if (*len + 2 + n > max_chars)
        return;
    buf[(*len)++] = ':';
    buf[(*len)++] = ' ';
    memcpy(buf + *len, ctx, n);
    *len += n;
}

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_graph_ground_compose(hu_memory_loader_t *loader, const char *contact_id,
                                   size_t contact_id_len, const char *msg, size_t msg_len,
                                   size_t max_chars, char **out, size_t *out_len,
                                   size_t *out_matched_entities) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (out_matched_entities)
        *out_matched_entities = 0;
    if (!loader || !out || !out_len || !contact_id || contact_id_len == 0 || !msg || msg_len == 0)
        return HU_OK; /* fail-open: no query -> no injection */
    if (max_chars == 0)
        max_chars = 600;
#ifdef HU_ENABLE_SQLITE
    hu_graph_t *g = loader->facade ? hu_w7_facade_graph_handle(loader->facade) : NULL;
    if (!g)
        return HU_OK; /* fail-open: no graph wired */
    hu_allocator_t *alloc = loader->alloc;

    hu_graph_entity_t *cands = NULL;
    size_t cand_count = 0;
    if (hu_graph_list_entities(g, alloc, contact_id, contact_id_len, GG_CANDIDATE_LIMIT, &cands,
                               &cand_count) != HU_OK ||
        cand_count == 0)
        return HU_OK;

    /* Score every candidate against the incoming message; keep the top-k
     * with score > 0. Selection sort over <= 64 items — no allocation. */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    double scores[GG_CANDIDATE_LIMIT];
    for (size_t i = 0; i < cand_count && i < GG_CANDIDATE_LIMIT; i++) {
        const hu_graph_entity_t *e = &cands[i];
        size_t words = hu_graph_ground_name_word_count(e->name, e->name_len);
        size_t hits = hu_graph_ground_entity_match_count(msg, msg_len, e->name, e->name_len);
        scores[i] = hu_graph_ground_score(hits, words, e->mention_count, e->last_seen, now_ms);
    }
    size_t seeds[GG_TOP_K];
    size_t seed_count = 0;
    for (size_t k = 0; k < GG_TOP_K; k++) {
        size_t best = (size_t)-1;
        double best_score = 0.0;
        for (size_t i = 0; i < cand_count && i < GG_CANDIDATE_LIMIT; i++) {
            if (scores[i] > best_score) {
                best_score = scores[i];
                best = i;
            }
        }
        if (best == (size_t)-1)
            break;
        seeds[seed_count++] = best;
        scores[best] = 0.0; /* consume */
    }
    if (seed_count == 0) {
        /* No lexical overlap with the graph: EMPTY injection. This is the
         * intended behavior — better no grounding than the same generic
         * community summaries on every turn (2026-07-22 shadow failure). */
        hu_graph_entities_free(alloc, cands, cand_count);
        return HU_OK;
    }

    char *buf = alloc->alloc(alloc->ctx, max_chars + 1);
    if (!buf) {
        hu_graph_entities_free(alloc, cands, cand_count);
        return HU_OK; /* fail-open */
    }
    size_t pos = 0;

    for (size_t s = 0; s < seed_count; s++) {
        const hu_graph_entity_t *seed = &cands[seeds[s]];
        if (!seed->name || seed->name_len == 0)
            continue;
        size_t line_start = pos;
        if (!gg_append(buf, max_chars, &pos, "- ", 2) ||
            !gg_append(buf, max_chars, &pos, seed->name, seed->name_len)) {
            pos = line_start;
            break;
        }
        const char *tname = hu_entity_type_to_string(seed->type);
        if (tname && seed->type != HU_ENTITY_UNKNOWN) {
            size_t type_start = pos;
            if (!(gg_append(buf, max_chars, &pos, " (", 2) &&
                  gg_append(buf, max_chars, &pos, tname, strlen(tname)) &&
                  gg_append(buf, max_chars, &pos, ")", 1)))
                pos = type_start; /* drop a half-written type suffix at the cap */
        }
        gg_append(buf, max_chars, &pos, "\n", 1);

        hu_graph_entity_t *nbrs = NULL;
        hu_graph_relation_t *rels = NULL;
        size_t ncount = 0;
        if (hu_graph_neighbors(g, alloc, contact_id, contact_id_len, seed->id, 1,
                               GG_NEIGHBORS_PER_SEED, &nbrs, &rels, &ncount) == HU_OK) {
            for (size_t i = 0; i < ncount; i++) {
                if (!nbrs[i].name || nbrs[i].name_len == 0)
                    continue;
                const char *rel_str = hu_relation_type_to_string(rels[i].type);
                size_t nb_start = pos;
                bool outward = (rels[i].source_id == seed->id);
                bool ok = gg_append(buf, max_chars, &pos, "  - ", 4) &&
                          gg_append(buf, max_chars, &pos, outward ? seed->name : nbrs[i].name,
                                    outward ? seed->name_len : nbrs[i].name_len) &&
                          gg_append(buf, max_chars, &pos, " ", 1) &&
                          gg_append(buf, max_chars, &pos, rel_str, strlen(rel_str)) &&
                          gg_append(buf, max_chars, &pos, " ", 1) &&
                          gg_append(buf, max_chars, &pos, outward ? nbrs[i].name : seed->name,
                                    outward ? nbrs[i].name_len : seed->name_len);
                if (!ok) {
                    pos = nb_start;
                    break;
                }
                gg_append_context_snippet(buf, max_chars, &pos, rels[i].context,
                                          rels[i].context_len);
                if (!gg_append(buf, max_chars, &pos, "\n", 1)) {
                    pos = nb_start;
                    break;
                }
            }
            hu_graph_entities_free(alloc, nbrs, ncount);
            hu_graph_relations_free(alloc, rels, ncount);
        }
    }

    hu_graph_entities_free(alloc, cands, cand_count);

    if (pos == 0) {
        alloc->free(alloc->ctx, buf, max_chars + 1);
        return HU_OK;
    }
    buf[pos] = '\0';
    /* Return a buffer sized EXACTLY to the content so callers freeing
     * (*out_len + 1) match the allocation size (codebase free-size contract). */
    char *exact = alloc->alloc(alloc->ctx, pos + 1);
    if (!exact) {
        alloc->free(alloc->ctx, buf, max_chars + 1);
        return HU_OK; /* fail-open */
    }
    memcpy(exact, buf, pos + 1);
    alloc->free(alloc->ctx, buf, max_chars + 1);
    *out = exact;
    *out_len = pos;
    if (out_matched_entities)
        *out_matched_entities = seed_count;
#else
    (void)max_chars;
#endif
    return HU_OK;
}
