#include "human/agent/graph_grounding.h"
#include "human/agent.h"
#include "human/agent/model_router.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/memory/graph.h"
#include "human/memory/graph_state.h"
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

hu_graph_grounding_fallback_mode_t hu_graph_grounding_contact_fallback_mode(void) {
    /* Default OFF: the fallback changes what reaches the prompt, so it ships
     * dark and advances only on a measurement (feature-gate-requires-
     * measurement.md). SHADOW logs what it would inject on every tier. */
    switch (hu_gate_mode_from_env("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", HU_GATE_OFF)) {
    case HU_GATE_LIVE:
        return HU_GG_FALLBACK_LIVE;
    case HU_GATE_SHADOW:
        return HU_GG_FALLBACK_SHADOW;
    default:
        return HU_GG_FALLBACK_OFF;
    }
}

hu_gate_mode_t hu_graph_grounding_self_facts_mode(void) {
    /* Default OFF: owner facts reaching the prompt change what is sent. */
    return hu_gate_mode_from_env("HU_GRAPH_GROUNDING_SELF_FACTS", HU_GATE_OFF);
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

bool hu_graph_ground_is_placeholder_name(const char *name, size_t name_len, const char *contact_id,
                                         size_t contact_id_len) {
    static const char *const k_placeholders[] = {"user", "assistant", "you",  "me",     "i",
                                                 "we",   "they",      "them", "someone"};
    if (!name || name_len == 0)
        return true;
    for (size_t k = 0; k < sizeof(k_placeholders) / sizeof(k_placeholders[0]); k++) {
        size_t pl = strlen(k_placeholders[k]);
        if (name_len == pl && strncasecmp(name, k_placeholders[k], pl) == 0)
            return true;
    }
    if (contact_id && contact_id_len == name_len && memcmp(name, contact_id, name_len) == 0)
        return true;
    /* Phone-number shaped: only digits and + ( ) - . space, with >= 7 digits. */
    size_t digits = 0;
    for (size_t k = 0; k < name_len; k++) {
        unsigned char c = (unsigned char)name[k];
        if (isdigit(c))
            digits++;
        else if (!strchr("+()-. ", c))
            return false;
    }
    return digits >= 7;
}

/* ── Query-conditioned composition ──────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

enum {
    GG_CANDIDATE_LIMIT = 64, /* top entities by mention_count considered */
    GG_MATCH_LIMIT = 32,     /* message-driven candidates (name shares a word) */
    GG_SCORE_CAP = GG_CANDIDATE_LIMIT + GG_MATCH_LIMIT,
    GG_TOP_K = 4, /* seed entities composed into the context */
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

/* Top-k selection over scores[0..min(n, GG_SCORE_CAP)): indices of the k
 * highest strictly-positive scores, best first. Consumes (zeroes) the picked
 * scores. Shared by the lexical and contact-fallback seed paths. */
static size_t gg_pick_top_k(double *scores, size_t n, size_t seeds[GG_TOP_K]) {
    size_t count = 0;
    for (size_t k = 0; k < GG_TOP_K; k++) {
        size_t best = (size_t)-1;
        double best_score = 0.0;
        for (size_t i = 0; i < n && i < GG_SCORE_CAP; i++) {
            if (scores[i] > best_score) {
                best_score = scores[i];
                best = i;
            }
        }
        if (best == (size_t)-1)
            break;
        seeds[count++] = best;
        scores[best] = 0.0; /* consume */
    }
    return count;
}

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_graph_ground_compose(hu_memory_loader_t *loader, const char *contact_id,
                                   size_t contact_id_len, const char *msg, size_t msg_len,
                                   size_t max_chars, char **out, size_t *out_len,
                                   size_t *out_matched_entities) {
    return hu_graph_ground_compose_ex(loader, contact_id, contact_id_len, msg, msg_len, max_chars,
                                      0, out, out_len, out_matched_entities);
}

hu_error_t hu_graph_ground_compose_ex(hu_memory_loader_t *loader, const char *contact_id,
                                      size_t contact_id_len, const char *msg, size_t msg_len,
                                      size_t max_chars, unsigned flags, char **out, size_t *out_len,
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
                               &cand_count) != HU_OK)
        cand_count = 0;
    /* Message-driven candidates: entities whose name shares a word with the
     * message, regardless of popularity. Without this, anything beyond the
     * top-64 by mention_count could never seed (2026-09-01: "Vanguard" at
     * rank 96 of 571). Appended and de-duplicated by id. */
    hu_graph_entity_t *word_hits = NULL;
    size_t hit_count = 0;
    if (hu_graph_find_entities_matching(g, alloc, contact_id, contact_id_len, msg, msg_len,
                                        GG_MATCH_LIMIT, &word_hits, &hit_count) == HU_OK &&
        hit_count > 0) {
        size_t total = cand_count + hit_count;
        hu_graph_entity_t *merged =
            (hu_graph_entity_t *)alloc->alloc(alloc->ctx, total * sizeof(*merged));
        if (merged) {
            size_t merged_n = 0;
            for (size_t i = 0; i < cand_count; i++)
                merged[merged_n++] = cands[i];
            for (size_t j = 0; j < hit_count; j++) {
                bool dup = false;
                for (size_t i = 0; i < cand_count && !dup; i++)
                    dup = (cands[i].id == word_hits[j].id);
                if (dup) {
                    /* free the duplicate's strings; the struct itself lives in word_hits[] */
                    if (word_hits[j].name)
                        alloc->free(alloc->ctx, word_hits[j].name, word_hits[j].name_len + 1);
                    if (word_hits[j].metadata_json)
                        alloc->free(alloc->ctx, word_hits[j].metadata_json,
                                    strlen(word_hits[j].metadata_json) + 1);
                } else {
                    merged[merged_n++] = word_hits[j];
                }
            }
            if (cands)
                alloc->free(alloc->ctx, cands, cand_count * sizeof(*cands));
            alloc->free(alloc->ctx, word_hits, hit_count * sizeof(*word_hits));
            cands = merged;
            cand_count = merged_n;
        } else {
            hu_graph_entities_free(alloc, word_hits, hit_count);
        }
    }
    if (cand_count == 0) {
        if (cands)
            hu_graph_entities_free(alloc, cands, 0);
        return HU_OK;
    }
    /* Score every candidate against the incoming message; keep the top-k
     * with score > 0. Selection sort over <= GG_SCORE_CAP items. */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    double scores[GG_SCORE_CAP];
    for (size_t i = 0; i < cand_count && i < GG_SCORE_CAP; i++) {
        const hu_graph_entity_t *e = &cands[i];
        size_t words = hu_graph_ground_name_word_count(e->name, e->name_len);
        size_t hits = hu_graph_ground_entity_match_count(msg, msg_len, e->name, e->name_len);
        bool partial = (flags & HU_GG_REQUIRE_FULL_NAME) && hits < words;
        scores[i] =
            partial ? 0.0
                    : hu_graph_ground_score(hits, words, e->mention_count, e->last_seen, now_ms);
    }
    size_t seeds[GG_TOP_K];
    size_t seed_count = gg_pick_top_k(scores, cand_count, seeds);
    bool via_fallback = false;
    if (seed_count == 0 && (flags & HU_GG_CONTACT_FALLBACK)) {
        /* Lexical miss, fallback requested: seed from THIS contact's own
         * entities, ranked by the same mention + recency terms the lexical
         * score adds on top of coverage (coverage pinned at 1/1). Candidates
         * are contact-scoped by hu_graph_list_entities, so nothing crosses
         * contacts. Unlike the 2026-07-22 static summaries, this differs per
         * contact and tracks recency. */
        for (size_t i = 0; i < cand_count && i < GG_SCORE_CAP; i++) {
            const hu_graph_entity_t *e = &cands[i];
            /* EMOTION entities are never volunteered unprompted: the most-
             * mentioned feeling ("heartbreak") would otherwise surface on
             * every unrelated casual text. The lexical path still grounds
             * on one when the contact names it. */
            bool eligible = e->type != HU_ENTITY_EMOTION &&
                            hu_graph_ground_name_word_count(e->name, e->name_len) > 0 &&
                            !hu_graph_ground_is_placeholder_name(e->name, e->name_len, contact_id,
                                                                 contact_id_len);
            scores[i] = eligible
                            ? hu_graph_ground_score(1, 1, e->mention_count, e->last_seen, now_ms)
                            : 0.0;
        }
        seed_count = gg_pick_top_k(scores, cand_count, seeds);
        via_fallback = seed_count > 0;
    }
    if (seed_count == 0) {
        /* No lexical overlap with the graph (and no fallback): EMPTY
         * injection. Better no grounding than the same generic community
         * summaries on every turn (2026-07-22 shadow failure). */
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
        /* Fetch twice the render budget so a superseded chain has room to
         * collapse: the state view keeps one head per (source, type) and
         * marks history, so "user works_at Vanguard" can never sit beside
         * "user works_at Raymond James" as if both held (2026-09-04). */
        if (hu_graph_neighbors(g, alloc, contact_id, contact_id_len, seed->id, 1,
                               (size_t)GG_NEIGHBORS_PER_SEED * 2u, &nbrs, &rels,
                               &ncount) == HU_OK) {
            hu_graph_state_entry_t *view = NULL;
            size_t view_n = 0;
            if (hu_graph_state_resolve(alloc, rels, ncount, now_ms, &view, &view_n) != HU_OK)
                view_n = 0;
            size_t rendered = 0;
            for (size_t k = 0; k < view_n && rendered < GG_NEIGHBORS_PER_SEED; k++) {
                size_t i = (size_t)(view[k].rel - rels);
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
                char suffix[96];
                size_t suffix_len =
                    hu_graph_relation_state_suffix(g, alloc, &view[k], suffix, sizeof(suffix));
                if (ok && suffix_len > 0)
                    ok = gg_append(buf, max_chars, &pos, suffix, suffix_len);
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
                rendered++;
            }
            if (view)
                alloc->free(alloc->ctx, view, view_n * sizeof(*view));
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
        *out_matched_entities = via_fallback ? 0 : seed_count;
#else
    (void)max_chars;
    (void)flags;
#endif
    return HU_OK;
}

/* SHADOW contract for grounding sub-gates: log size + fingerprint only (never
 * text), then drop. */
static void gg_log_shadow_and_free(hu_memory_loader_t *loader, const char *what, char *buf,
                                   size_t len, int tier) {
    hu_log_info("graph_grounding", NULL, "%s shadow: %zu bytes tier=%d fp=%08x (not injected)",
                what, len, tier, (unsigned)hu_graph_ground_fingerprint(buf, len));
    if (buf)
        loader->alloc->free(loader->alloc->ctx, buf, len + 1);
}

/* *ctx = *ctx + ("\n" if non-empty) + label + add. Takes ownership of add; on
 * allocation failure keeps *ctx unchanged and drops add (fail-open). */
static bool gg_append_labeled(hu_memory_loader_t *loader, char **ctx, size_t *ctx_len,
                              const char *label, char *add, size_t add_len) {
    hu_allocator_t *a = loader->alloc;
    size_t lab = strlen(label), sep = *ctx_len > 0 ? 1 : 0;
    size_t n = *ctx_len + sep + lab + add_len;
    char *buf = a->alloc(a->ctx, n + 1);
    if (buf) {
        size_t pos = 0;
        if (*ctx_len > 0) {
            memcpy(buf, *ctx, *ctx_len);
            pos = *ctx_len;
            buf[pos++] = '\n';
        }
        memcpy(buf + pos, label, lab);
        memcpy(buf + pos + lab, add, add_len);
        buf[n] = '\0';
        if (*ctx)
            a->free(a->ctx, *ctx, *ctx_len + 1);
        *ctx = buf;
        *ctx_len = n;
    }
    a->free(a->ctx, add, add_len + 1);
    return buf != NULL;
}

/* Graph grounding load, shared by BOTH turn paths (see agent.h). Composes
 * QUERY-CONDITIONED graph context for the incoming message (entity-overlap
 * scored, 1-hop; empty when nothing matches — see hu_graph_ground_compose)
 * per the HU_GRAPH_GROUNDING gate: SHADOW logs size + relevance fingerprint
 * and drops; ON (live) injects only on ANALYTICAL/DEEP turns, mirroring the
 * RAG leg's live A/B verdict (2026-05-29: substantive +0.110, casual -0.078)
 * — casual/unknown-tier turns log and drop the loaded context. */
void hu_agent_load_graph_grounding(hu_agent_t *agent, void *loader_v, const char *msg,
                                   size_t msg_len, char **graph_ctx, size_t *graph_ctx_len) {
    if (!agent || !loader_v || !graph_ctx || !graph_ctx_len)
        return;
    hu_memory_loader_t *loader = (hu_memory_loader_t *)loader_v;
    hu_graph_grounding_mode_t graph_mode = hu_graph_grounding_mode();
    if (graph_mode == HU_GRAPH_GROUNDING_OFF || !agent->memory_session_id ||
        agent->memory_session_id_len == 0)
        return;
    size_t matched_entities = 0;
    bool via_fallback = false, via_self = false;
    hu_graph_ground_compose(loader, agent->memory_session_id, agent->memory_session_id_len, msg,
                            msg_len, 0, graph_ctx, graph_ctx_len, &matched_entities);
    /* Contact-anchored fallback on a lexical miss. Activation gated on a
     * blind A/B: SHADOW (default when enabled for measurement) logs what the
     * contact's own facts would inject and drops it; LIVE adopts it and then
     * falls through to the SAME tier gate below, so the 2026-05-29 measured
     * casual-register drop still applies. Default OFF. */
    if (*graph_ctx_len == 0) {
        hu_graph_grounding_fallback_mode_t fb_mode = hu_graph_grounding_contact_fallback_mode();
        if (fb_mode != HU_GG_FALLBACK_OFF) {
            char *fb = NULL;
            size_t fb_len = 0;
            hu_graph_ground_compose_ex(loader, agent->memory_session_id,
                                       agent->memory_session_id_len, msg, msg_len, 0,
                                       HU_GG_CONTACT_FALLBACK, &fb, &fb_len, NULL);
            if (fb_mode == HU_GG_FALLBACK_SHADOW) {
                gg_log_shadow_and_free(loader, "contact_fallback", fb, fb_len, agent->turn_tier);
            } else if (fb) {
                *graph_ctx = fb;
                *graph_ctx_len = fb_len;
                via_fallback = true;
            }
        }
    }
    /* Owner ("self") facts the message names in full. Activation gated on a
     * blind A/B, default OFF: SHADOW logs size only; LIVE appends them under
     * an "About you:" label (so the model never mistakes them for the
     * contact's), then the same tier gate below applies. */
    hu_gate_mode_t self_mode = hu_graph_grounding_self_facts_mode();
    if (self_mode != HU_GATE_OFF) {
        char *sf = NULL;
        size_t sf_len = 0;
        hu_graph_ground_compose_ex(loader, "self", 4, msg, msg_len, 0, HU_GG_REQUIRE_FULL_NAME, &sf,
                                   &sf_len, NULL);
        if (self_mode == HU_GATE_SHADOW)
            gg_log_shadow_and_free(loader, "self_facts", sf, sf_len, agent->turn_tier);
        else if (sf)
            via_self =
                gg_append_labeled(loader, graph_ctx, graph_ctx_len, "About you:\n", sf, sf_len);
    }
    const char *drop_reason = NULL;
    if (graph_mode == HU_GRAPH_GROUNDING_SHADOW) {
        /* Shadow contract: size AND a relevance fingerprint (matched-entity
         * count + content hash), so the pre-2026-07 failure signature (274
         * events, 5 distinct sizes, constant content) is distinguishable
         * from conversation-varying injection straight from the log. */
        hu_log_info("graph_grounding", NULL,
                    "shadow: %zu graph_context bytes matched=%zu fp=%08x (not injected)",
                    *graph_ctx_len, matched_entities,
                    (unsigned)hu_graph_ground_fingerprint(*graph_ctx, *graph_ctx_len));
        drop_reason = "shadow";
    } else if (graph_mode == HU_GRAPH_GROUNDING_ON && agent->turn_tier < (int)HU_TIER_ANALYTICAL) {
        hu_log_info("graph_grounding", NULL,
                    "live: %zu bytes skipped for casual register (tier=%d)", *graph_ctx_len,
                    agent->turn_tier);
        drop_reason = "casual";
    }
    if (!drop_reason && *graph_ctx_len > 0)
        /* LIVE injection is otherwise silent; this line is what makes the
         * fallback / self-facts activation measurable (sizes only, no text). */
        hu_log_info("graph_grounding", NULL,
                    "live: injected %zu bytes tier=%d lexical=%zu fallback=%d self=%d fp=%08x",
                    *graph_ctx_len, agent->turn_tier, matched_entities, via_fallback, via_self,
                    (unsigned)hu_graph_ground_fingerprint(*graph_ctx, *graph_ctx_len));
    if (drop_reason) {
        if (*graph_ctx)
            agent->alloc->free(agent->alloc->ctx, *graph_ctx, *graph_ctx_len + 1);
        *graph_ctx = NULL;
        *graph_ctx_len = 0;
    }
}
