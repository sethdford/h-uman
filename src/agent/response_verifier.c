#include "human/agent/response_verifier.h"
#include "human/agent/self_rag.h"
#include "human/agent/world_model.h" /* sprint-2c Story A — wm->negatives */
#include "human/core/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* sprint-2c Story A — refusal & hedge templates per negative source.
 * Short enough to fit `refusal_text[256]` and `suggested_hedge[160]`
 * even with the negative's `reason` appended. */
#define HU_NEG_REFUSAL_HARD   "I can't help with that — you've asked me not to discuss it."
#define HU_NEG_REFUSAL_POLICY "I can't help with that — it would violate a safety policy."
#define HU_NEG_HEDGE_SOFT     "I'm not confident enough to commit to that — let me double-check first."
#define HU_NEG_HEDGE_CONFIRM  "I think we agreed not to bring this up — is that still right?"

/* sprint-2c Story A — matcher constants.
 *
 * Tokenize the NEGATIVE (not the claim) into ≥5-char non-stopword tokens.
 * Hit when `hits ≥ 0.3 × negative_tokens` where `hits` is the number of
 * negative tokens that appear as substrings of the lowercased claim.
 *
 * Why these numbers:
 *   - ≥5 chars filters generic 4-char verbs ("give", "card", "deal")
 *     while keeping topic words ("merger", "advice", "credit", "therapy",
 *     "medical", "close", "timing").
 *   - 0.3 catches the 1-in-3 case so a single topic-keyword tripwire
 *     fires ("merger" in "The merger talks") while a benign claim sharing
 *     zero topic words stays below threshold.
 *
 * Known false-positive class: short [policy] negatives like "no medical
 * advice" fire on benign claims sharing one topic word ("legal advice").
 * Conservative for safety policy. Tunable later via telemetry.
 *
 * Documented in `docs/plans/2026-05-12-wire-w11-negative-source-tags.md`. */
#define HU_NEG_MATCH_THRESHOLD 0.3f
#define HU_NEG_MIN_TOKEN_LEN   5

hu_verifier_config_t hu_verifier_default_config(void) {
    hu_verifier_config_t c = {0};
    c.mode = HU_VERIFY_SOFT;
    c.confidence_threshold = 0.6f;
    c.max_claims = 16;
    c.now_ms = 0;
    return c;
}

/* Format a unix ms timestamp into a short "Mon 2026-05-09 14:22" label.
 * Falls back to "(unknown)" when ts is 0. */
static void render_timestamp(int64_t ms, char *buf, size_t cap) {
    if (ms <= 0) {
        snprintf(buf, cap, "(unknown)");
        return;
    }
    time_t s = (time_t)(ms / 1000);
    struct tm tm_buf;
#if defined(_WIN32)
    gmtime_s(&tm_buf, &s);
#else
    gmtime_r(&s, &tm_buf);
#endif
    strftime(buf, cap, "%a %Y-%m-%d %H:%M", &tm_buf);
}

void hu_provenance_render(const hu_memory_relation_row_t *rel, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    if (!rel) {
        snprintf(buf, cap, "[no source]");
        return;
    }
    char ts_label[32];
    render_timestamp(rel->event_start, ts_label, sizeof(ts_label));
    /* If the relation has provenance text, prefer it; else fall back to a
     * generic "from memory" label. Receipt format must remain stable so the
     * renderer can string-split for channel-specific styling. */
    if (rel->provenance && rel->provenance_len > 0) {
        snprintf(buf, cap, "[from %.*s, %s]",
                 (int)(rel->provenance_len < 80 ? rel->provenance_len : 80), rel->provenance,
                 ts_label);
    } else {
        snprintf(buf, cap, "[from memory, %s]", ts_label);
    }
}

/* Case-insensitive word-boundary prefix check (skips leading whitespace).
 *
 * The boundary check (next char after prefix is non-alpha) only fires
 * when the prefix's LAST char is alpha — otherwise the prefix already
 * encodes its own boundary (trailing space, etc.) and double-checking
 * the next char would over-reject. Example: prefix "Maybe " matching
 * "Maybe Berlin ..." — the trailing space in the prefix is the
 * boundary; the next char ('B') is alpha but that's the next word,
 * not a continuation of the prefix. */
static bool rv_sentence_starts_with_ci(const char *s, size_t len, const char *prefix) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) i++;
    size_t pl = strlen(prefix);
    if (pl == 0 || len - i < pl) return false;
    for (size_t j = 0; j < pl; j++) {
        if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)prefix[j]))
            return false;
    }
    char last = prefix[pl - 1];
    bool last_is_alpha = isalpha((unsigned char)last) != 0;
    if (last_is_alpha && i + pl < len) {
        char c = s[i + pl];
        if (isalpha((unsigned char)c) || c == '_' || c == '\'')
            return false;
    }
    return true;
}

/* W11 — propositional-claim filter. Rejects sentence shapes the
 * heuristic verifier should NOT score as factual claims:
 *   - Opinion / mental-verb starts: "I think ...", "I believe ..."
 *   - Hedge starts: "Maybe ...", "In my opinion ..."
 *   - Imperative / request starts: "Tell me ...", "Could you ..."
 *
 * Surfaced by the 2026-05-16 W11 abstain calibration pack:
 * "I think the autumn light in Brooklyn is the best." and
 * "Tell me a joke about debuggers." both contained capitalized nouns
 * the verifier's token-overlap scorer could not find in an empty
 * graph, so they fired abstain. Filtering them out at extraction
 * time keeps the abstain decision focused on actual propositions. */
static bool rv_sentence_is_propositional_claim(const char *s, size_t len) {
    static const char *const k_skip[] = {
        "I think",   "I believe", "I feel",   "I guess",     "I suppose",
        "I assume",  "I hope",    "I imagine","I doubt",     "I wonder",
        "I reckon",  "I bet",
        "Maybe ",    "Perhaps ",  "Probably ","Possibly ",
        "In my opinion","It seems","It feels","Apparently ","Supposedly ",
        "Tell me",   "Show me",   "Give me",  "Help me",     "Let me",
        "Please",    "Could you", "Can you",  "Would you",   "Will you",
        "Shall we",  "Should I",  "Shall I",  "Do you",      "Are you",
        "Make me",   "Create ",   "Write ",   "Generate ",   "Send ",
        "Draft ",    "Brainstorm","Suggest ", "Recommend ",  "Explain ",
        "Summarize", "Translate ","Rewrite ",
        NULL,
    };
    for (size_t k = 0; k_skip[k]; k++) {
        if (rv_sentence_starts_with_ci(s, len, k_skip[k]))
            return false;
    }
    return true;
}

/* Lightweight extractor: split the draft into sentences (period / question /
 * exclamation), keep only those with at least 3 alpha-token "words" — that's
 * the cheap signal a sentence is declarative rather than a salutation or
 * ack. The verifier itself does the actual scoring. Questions are skipped
 * (they're not factual claims), and W11's propositional-claim filter drops
 * opinions / hedges / requests before they reach the scorer. */
static size_t extract_claims(const char *draft, size_t draft_len, hu_verifier_claim_t *out,
                             size_t cap) {
    size_t out_n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= draft_len && out_n < cap; i++) {
        bool eos = (i == draft_len || draft[i] == '.' || draft[i] == '!');
        bool question = (i < draft_len && draft[i] == '?');
        if (!eos && !question)
            continue;
        size_t end = i;
        while (start < end && isspace((unsigned char)draft[start]))
            start++;
        size_t len = end - start;
        start = i + 1;
        if (question || len < 4)
            continue;

        size_t words = 0;
        bool in_word = false;
        for (size_t j = end - len; j < end; j++) {
            bool alpha = isalpha((unsigned char)draft[j]) != 0;
            if (alpha && !in_word) {
                words++;
                in_word = true;
            } else if (!alpha) {
                in_word = false;
            }
        }
        if (words < 3)
            continue;

        /* W11 — drop opinions, hedges, and requests before scoring. */
        if (!rv_sentence_is_propositional_claim(draft + (end - len), len))
            continue;

        hu_verifier_claim_t *c = &out[out_n++];
        memset(c, 0, sizeof(*c));
        size_t copy = len < sizeof(c->text) - 1 ? len : sizeof(c->text) - 1;
        memcpy(c->text, draft + (end - len), copy);
        c->text[copy] = '\0';
    }
    return out_n;
}

#ifdef HU_ENABLE_SQLITE

/* Heuristic verification: load open-interval relations via the W7 facade
 * (same row set as the legacy SQL join on entities for names), then score
 * claim tokens against endpoint names + provenance + context. */
static float verify_claim_against_facade(hu_memory_facade_t *memory, hu_allocator_t *alloc,
                                         const char *contact_id, int cid_len, const char *claim,
                                         hu_provenance_receipt_t *out_receipt) {
    /* Tokenize claim into >= 4-char alpha tokens. Skip stopwords. */
    static const char *const stop[] = {"is",   "was",   "were", "will", "the",  "and",
                                        "this", "that",  "with", "have", "has",  "had",
                                        "for",  "from",  "your", "you",  "they", "them",
                                        "i'm",  "i've",  "i'll", "i'd",  NULL};
    char tokens[16][32] = {{0}};
    size_t nt = 0;
    size_t i = 0;
    while (claim[i] && nt < 16) {
        while (claim[i] && !isalpha((unsigned char)claim[i]))
            i++;
        size_t s = i;
        while (claim[i] && (isalpha((unsigned char)claim[i]) || claim[i] == '\''))
            i++;
        size_t l = i - s;
        if (l < 4 || l >= sizeof(tokens[0]))
            continue;
        char low[32];
        for (size_t k = 0; k < l; k++)
            low[k] = (char)tolower((unsigned char)claim[s + k]);
        low[l] = '\0';
        bool is_stop = false;
        for (size_t k = 0; stop[k]; k++)
            if (strcmp(low, stop[k]) == 0) {
                is_stop = true;
                break;
            }
        if (!is_stop) {
            memcpy(tokens[nt], low, l + 1);
            nt++;
        }
    }
    if (nt == 0)
        return 0.0f;

    if (!memory || !alloc)
        return 0.0f;

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_RELATION;
    q.variant = HU_MEMORY_QUERY_BY_ID;
    q.contact_id = contact_id;
    q.contact_id_len = (size_t)(cid_len > 0 ? cid_len : 0);
    q.as.by_id.id = HU_MEMORY_REL_VERIFIER_SCAN;
    q.as.by_id.limit = 64;

    hu_memory_record_t *recs = NULL;
    size_t nrec = 0;
    if (hu_memory_facade_read(memory, &q, alloc, &recs, &nrec) != HU_OK || nrec == 0) {
        if (recs)
            hu_memory_facade_records_free(memory, alloc, recs, nrec);
        return 0.0f;
    }

    float best_score = 0.0f;
    int64_t best_id = 0;
    int64_t best_es = 0, best_ee = 0;
    float best_conf = 0.0f;
    char best_prov[80] = {0};
    for (size_t ri = 0; ri < nrec; ri++) {
        const hu_memory_relation_row_t *rel = (const hu_memory_relation_row_t *)recs[ri].payload;
        if (!rel)
            continue;
        const char *prov = rel->provenance;
        const char *ctx = rel->context;
        const char *en_s = rel->source_name;
        const char *en_t = rel->target_name;

        size_t hits = 0;
        char joined[768] = {0};
        snprintf(joined, sizeof(joined), "%s %s %s %s", en_s ? en_s : "", en_t ? en_t : "",
                 prov ? prov : "", ctx ? ctx : "");
        for (size_t t = 0; t < nt; t++) {
            char low[768];
            size_t lj = strlen(joined);
            for (size_t k = 0; k < lj; k++)
                low[k] = (char)tolower((unsigned char)joined[k]);
            low[lj] = '\0';
            if (strstr(low, tokens[t]))
                hits++;
        }
        float row_score = (float)hits / (float)nt;
        if (row_score > best_score) {
            best_score = row_score;
            best_id = rel->id;
            best_es = rel->event_start;
            best_ee = rel->event_end;
            best_conf = rel->confidence;
            if (prov)
                snprintf(best_prov, sizeof(best_prov), "%s", prov);
        }
    }
    hu_memory_facade_records_free(memory, alloc, recs, nrec);

    if (out_receipt && best_id > 0) {
        memset(out_receipt, 0, sizeof(*out_receipt));
        out_receipt->graph_relation_id = best_id;
        out_receipt->event_start_ms = best_es;
        out_receipt->event_end_ms = best_ee;
        out_receipt->confidence = best_conf;
        /* best_prov is 80 bytes; out_receipt->source is 64. Width-bound
         * so GCC -Wformat-truncation=2 is quiet under -Werror. */
        snprintf(out_receipt->source, sizeof(out_receipt->source), "%.*s",
                 (int)(sizeof(out_receipt->source) - 1),
                 best_prov[0] ? best_prov : "memory");
        out_receipt->observed_at_ms = best_es;
        char ts[32];
        render_timestamp(best_es, ts, sizeof(ts));
        /* Width-bound best_prov; ts is already 32 bytes which fits. */
        snprintf(out_receipt->rendered, sizeof(out_receipt->rendered),
                 "[from %.60s, %s]",
                 best_prov[0] ? best_prov : "memory", ts);
    }
    return best_score;
}

#endif

/* sprint-2c Story A — tokenize `text` into up to `cap` lowercase
 * ≥HU_NEG_MIN_TOKEN_LEN-char, non-stopword tokens. Returns the count
 * written into `out`. Stopword list is intentionally minimal — the same
 * set the W4 facade-tokenizer already uses. */
static size_t tokenize_negative(const char *text, size_t len, char out[][32], size_t cap) {
    static const char *const stop[] = {"is",   "was",   "were", "will", "the",  "and",
                                        "this", "that",  "with", "have", "has",  "had",
                                        "for",  "from",  "your", "you",  "they", "them",
                                        "i'm",  "i've",  "i'll", "i'd",  NULL};
    size_t n = 0;
    size_t i = 0;
    while (i < len && n < cap) {
        while (i < len && !isalpha((unsigned char)text[i]))
            i++;
        size_t s = i;
        while (i < len && (isalpha((unsigned char)text[i]) || text[i] == '\''))
            i++;
        size_t l = i - s;
        if (l < HU_NEG_MIN_TOKEN_LEN || l >= 32)
            continue;
        char low[32];
        for (size_t k = 0; k < l; k++)
            low[k] = (char)tolower((unsigned char)text[s + k]);
        low[l] = '\0';
        bool is_stop = false;
        for (size_t k = 0; stop[k]; k++)
            if (strcmp(low, stop[k]) == 0) {
                is_stop = true;
                break;
            }
        if (!is_stop) {
            memcpy(out[n], low, l + 1);
            n++;
        }
    }
    return n;
}

/* sprint-2c Story A — count how many `negative_tokens` appear as substrings
 * of the lowercased `claim`. Returns hits/nt in [0, 1]. */
static float negative_match_score(char negative_tokens[][32], size_t nt, const char *claim) {
    if (nt == 0 || !claim || !*claim)
        return 0.0f;
    /* Lowercase the claim into a stack buffer (claims are bounded by
     * `hu_verifier_claim_t::text[256]` so 256 is the worst case). */
    char low[256];
    size_t clen = strlen(claim);
    if (clen >= sizeof(low))
        clen = sizeof(low) - 1;
    for (size_t k = 0; k < clen; k++)
        low[k] = (char)tolower((unsigned char)claim[k]);
    low[clen] = '\0';
    size_t hits = 0;
    for (size_t t = 0; t < nt; t++)
        if (strstr(low, negative_tokens[t]))
            hits++;
    return (float)hits / (float)nt;
}

/* sprint-2c Story A — outcome implied by a negative-source tag. */
static hu_verifier_outcome_t outcome_for_source(hu_negative_source_t s) {
    switch (s) {
    case HU_NEGATIVE_SOURCE_USER_EXPLICIT:
    case HU_NEGATIVE_SOURCE_SYSTEM_POLICY:
        return HU_VERIFY_RESULT_ABSTAIN;
    case HU_NEGATIVE_SOURCE_SELF_RAG_ABSTAIN:
    case HU_NEGATIVE_SOURCE_AUTO_EXTRACT:
        return HU_VERIFY_RESULT_HEDGED;
    }
    /* Unknown source → conservative ABSTAIN. */
    return HU_VERIFY_RESULT_ABSTAIN;
}

/* sprint-2c Story A — strictness lattice. */
static hu_verifier_outcome_t outcome_stricter(hu_verifier_outcome_t a, hu_verifier_outcome_t b) {
    if (a == HU_VERIFY_RESULT_ABSTAIN || b == HU_VERIFY_RESULT_ABSTAIN)
        return HU_VERIFY_RESULT_ABSTAIN;
    if (a == HU_VERIFY_RESULT_HEDGED || b == HU_VERIFY_RESULT_HEDGED)
        return HU_VERIFY_RESULT_HEDGED;
    return HU_VERIFY_RESULT_SUPPORTED;
}

/* sprint-2c Story A — render the refusal or hedge text for a matched
 * negative into `out`. `cap` is the buffer capacity. Always
 * NUL-terminates. When the negative carries a non-empty `reason`, the
 * HARD refusal appends " — you said: '<reason>'", width-bound to fit. */
static void render_negative_text(const hu_negative_memory_t *nm, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    const char *base = "";
    switch (nm->source) {
    case HU_NEGATIVE_SOURCE_USER_EXPLICIT:    base = HU_NEG_REFUSAL_HARD;   break;
    case HU_NEGATIVE_SOURCE_SYSTEM_POLICY:    base = HU_NEG_REFUSAL_POLICY; break;
    case HU_NEGATIVE_SOURCE_SELF_RAG_ABSTAIN: base = HU_NEG_HEDGE_SOFT;     break;
    case HU_NEGATIVE_SOURCE_AUTO_EXTRACT:     base = HU_NEG_HEDGE_CONFIRM;  break;
    }
    if (nm->reason[0]) {
        /* Width-bound the reason so the printed string can never exceed cap. */
        int reason_room = (int)cap - (int)strlen(base) - (int)sizeof(" — you said: ''") - 4;
        if (reason_room < 8)
            reason_room = 8;
        snprintf(out, cap, "%s — you said: '%.*s'", base, reason_room, nm->reason);
    } else {
        snprintf(out, cap, "%s", base);
    }
}

hu_verifier_outcome_t hu_negatives_scan_claim(const struct hu_world_model *wm,
                                              const char *claim,
                                              char *out_refusal, size_t refusal_cap,
                                              char *out_hedge, size_t hedge_cap,
                                              bool *out_policy_hit) {
    if (!wm || wm->negatives_count == 0 || !claim || !*claim)
        return HU_VERIFY_RESULT_SUPPORTED;

    hu_verifier_outcome_t worst = HU_VERIFY_RESULT_SUPPORTED;
    /* First match per strictness tier writes the rendered text; later
     * matches at the same tier are silent (deterministic, insertion-order
     * stable). */
    for (size_t i = 0; i < wm->negatives_count; i++) {
        const hu_negative_memory_t *nm = &wm->negatives[i];
        char ntoks[16][32] = {{0}};
        size_t nt = tokenize_negative(nm->text, strlen(nm->text), ntoks, 16);
        if (nt == 0)
            continue;
        float score = negative_match_score(ntoks, nt, claim);
        if (score < HU_NEG_MATCH_THRESHOLD)
            continue;
        hu_verifier_outcome_t o = outcome_for_source(nm->source);
        hu_verifier_outcome_t merged = outcome_stricter(worst, o);
        if (merged != worst) {
            if (o == HU_VERIFY_RESULT_ABSTAIN && out_refusal)
                render_negative_text(nm, out_refusal, refusal_cap);
            if (o == HU_VERIFY_RESULT_HEDGED && out_hedge)
                render_negative_text(nm, out_hedge, hedge_cap);
            if (out_policy_hit && nm->source == HU_NEGATIVE_SOURCE_SYSTEM_POLICY)
                *out_policy_hit = true;
            worst = merged;
            if (worst == HU_VERIFY_RESULT_ABSTAIN)
                break; /* Strictest possible — short-circuit. */
        }
    }
    return worst;
}

hu_error_t hu_response_verify(hu_allocator_t *alloc, hu_memory_facade_t *memory, const char *contact_id,
                              size_t contact_id_len, const char *draft, size_t draft_len,
                              const hu_verifier_config_t *cfg, hu_verifier_report_t *out_report) {
    return hu_response_verify_against_world_model(alloc, memory, /*wm=*/NULL, contact_id,
                                                  contact_id_len, draft, draft_len, cfg, out_report);
}

hu_error_t hu_response_verify_against_world_model(
    hu_allocator_t *alloc, hu_memory_facade_t *memory,
    const struct hu_world_model *wm,
    const char *contact_id, size_t contact_id_len,
    const char *draft, size_t draft_len,
    const hu_verifier_config_t *cfg, hu_verifier_report_t *out_report) {
    if (!alloc || !draft || !cfg || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_report, 0, sizeof(*out_report));

    if (cfg->mode == HU_VERIFY_OFF || draft_len == 0)
        return HU_OK;

    /* TELEMETRY mode runs full extraction + scoring (so callers can observe
     * claim counts and supported/flagged ratios) but never touches the draft.
     * The mutation gates below are SOFT-specific so this falls through naturally. */

    size_t max = cfg->max_claims > 0 && cfg->max_claims < 16 ? cfg->max_claims : 16;
    size_t n = extract_claims(draft, draft_len, out_report->claims, max);
    out_report->claims_extracted = n;
    if (n == 0) {
        /* sprint-2c Story A — questions and other non-propositional drafts
         * yield zero claims, but they can still be policy violations
         * (e.g. "Have you been to your therapy sessions?" when therapy
         * is on the negative list). Run a single whole-draft negative
         * pass so [hard]/[policy] still ABSTAIN and [soft]/[confirm]
         * still HEDGE under SOFT mode. */
        if (!wm)
            return HU_OK;
        char neg_refusal0[256] = {0};
        char neg_hedge0[160] = {0};
        bool policy_hit0 = false;
        hu_verifier_outcome_t o = hu_negatives_scan_claim(
            wm, draft, neg_refusal0, sizeof(neg_refusal0),
            neg_hedge0, sizeof(neg_hedge0), &policy_hit0);
        if (o == HU_VERIFY_RESULT_ABSTAIN) {
            out_report->outcome = HU_VERIFY_RESULT_ABSTAIN;
            snprintf(out_report->refusal_text, sizeof(out_report->refusal_text), "%s",
                     neg_refusal0);
            if (policy_hit0)
                hu_log_warn("response_verifier", NULL,
                            "negative-memory [policy] hit forced ABSTAIN for contact=%.*s",
                            (int)(contact_id_len > 64 ? 64 : contact_id_len),
                            contact_id ? contact_id : "");
        } else if (o == HU_VERIFY_RESULT_HEDGED) {
            out_report->outcome = HU_VERIFY_RESULT_HEDGED;
            if (cfg->mode == HU_VERIFY_SOFT) {
                snprintf(out_report->modified_draft, sizeof(out_report->modified_draft),
                         "%s %.*s.", neg_hedge0, (int)draft_len, draft);
                out_report->draft_modified = true;
            }
        }
        return HU_OK;
    }

    /* sprint-2c Story A — scan extracted claims against wm->negatives.
     * Runs BEFORE the facade pass so a [hard] / [policy] hit forces
     * ABSTAIN regardless of supporting evidence (the negative says
     * "don't say this" — facade verdicts are irrelevant). [soft] /
     * [confirm] hits HEDGE the draft below. */
    hu_verifier_outcome_t neg_outcome = HU_VERIFY_RESULT_SUPPORTED;
    char neg_refusal[256] = {0};
    char neg_hedge[160] = {0};
    bool policy_hit = false;
    if (wm) {
        for (size_t i = 0; i < n; i++) {
            hu_verifier_outcome_t o = hu_negatives_scan_claim(
                wm, out_report->claims[i].text,
                neg_refusal, sizeof(neg_refusal),
                neg_hedge, sizeof(neg_hedge),
                &policy_hit);
            neg_outcome = outcome_stricter(neg_outcome, o);
            if (neg_outcome == HU_VERIFY_RESULT_ABSTAIN)
                break;
        }
        if (neg_outcome == HU_VERIFY_RESULT_ABSTAIN) {
            out_report->outcome = HU_VERIFY_RESULT_ABSTAIN;
            snprintf(out_report->refusal_text, sizeof(out_report->refusal_text), "%s",
                     neg_refusal);
            out_report->claims_flagged = n;
            /* [policy] hits get a best-effort audit-log warning so security
             * tooling can grep for them. NULL observer = stdout fallback. */
            if (policy_hit)
                hu_log_warn("response_verifier", NULL,
                            "negative-memory [policy] hit forced ABSTAIN for contact=%.*s",
                            (int)(contact_id_len > 64 ? 64 : contact_id_len),
                            contact_id ? contact_id : "");
            return HU_OK;
        }
    }

#ifdef HU_ENABLE_SQLITE
    if (!memory) {
        for (size_t i = 0; i < n; i++) {
            hu_verifier_claim_t *c = &out_report->claims[i];
            c->score = 0.0f;
            c->supported = false;
            snprintf(c->suggested_hedge, sizeof(c->suggested_hedge),
                     "I'm not certain — I don't have memory backing this:");
        }
        out_report->claims_flagged = n;
        /* sprint-2c Story A — a [soft]/[confirm] negative-memory hit on the
         * no-facade path still surfaces a hedge. The outcome reflects the
         * negative rather than the generic "no graph available" path. */
        if (neg_outcome == HU_VERIFY_RESULT_HEDGED) {
            out_report->outcome = HU_VERIFY_RESULT_HEDGED;
            if (cfg->mode == HU_VERIFY_SOFT) {
                snprintf(out_report->modified_draft, sizeof(out_report->modified_draft),
                         "%s %.*s.", neg_hedge, (int)draft_len, draft);
                out_report->draft_modified = true;
            }
            return HU_OK;
        }
        if (cfg->abstain_threshold > 0.0f) {
            out_report->outcome = HU_VERIFY_RESULT_ABSTAIN;
            hu_self_rag_render_refusal(HU_REFUSAL_LOW_CONFIDENCE,
                                        out_report->refusal_text,
                                        sizeof(out_report->refusal_text));
        }
        return HU_OK;
    }
    int cid_len = contact_id ? (int)contact_id_len : 0;
    const char *cid = contact_id ? contact_id : "";

    bool any_modified = false;
    char rebuilt[2048] = {0};
    size_t rb_off = 0;

    for (size_t i = 0; i < n; i++) {
        hu_verifier_claim_t *c = &out_report->claims[i];
        c->score = verify_claim_against_facade(memory, alloc, cid, cid_len, c->text, &c->receipt);
        c->supported = c->score >= cfg->confidence_threshold;
        if (c->supported) {
            out_report->claims_supported++;
        } else {
            out_report->claims_flagged++;
            snprintf(c->suggested_hedge, sizeof(c->suggested_hedge),
                     "I'm not 100%% sure but");
        }

        if (cfg->mode == HU_VERIFY_SOFT && !c->supported && rb_off < sizeof(rebuilt) - 256) {
            int w = snprintf(rebuilt + rb_off, sizeof(rebuilt) - rb_off, "%s%s %s.",
                             rb_off == 0 ? "" : " ", c->suggested_hedge, c->text);
            if (w > 0) {
                rb_off += (size_t)w;
                any_modified = true;
            }
        } else if (cfg->mode == HU_VERIFY_SOFT && c->supported &&
                   rb_off < sizeof(rebuilt) - 256) {
            int w = snprintf(rebuilt + rb_off, sizeof(rebuilt) - rb_off, "%s%s %s.",
                             rb_off == 0 ? "" : " ", c->text,
                             c->receipt.rendered[0] ? c->receipt.rendered : "");
            if (w > 0)
                rb_off += (size_t)w;
        }
    }

    /* Abstention decision. Opt-in: callers that want the verifier to signal
     * refusal must set `abstain_threshold > 0`. The self-RAG backends handle
     * their own abstention; this path exists for callers that go through the
     * v1 verifier directly but still want an explicit abstention signal. */
    if (cfg->abstain_threshold > 0.0f) {
        float flagged_ratio = n > 0 ? (float)out_report->claims_flagged / (float)n : 0.0f;
        if (n > 0 && flagged_ratio >= cfg->abstain_threshold) {
            out_report->outcome = HU_VERIFY_RESULT_ABSTAIN;
            hu_self_rag_render_refusal(HU_REFUSAL_LOW_CONFIDENCE,
                                        out_report->refusal_text,
                                        sizeof(out_report->refusal_text));
            return HU_OK;
        }
    }

    /* sprint-2c Story A — a [soft]/[confirm] hit on the facade-supported
     * path also surfaces a hedge: the negative wins over the facade's
     * "looks supported" verdict because the negative is an explicit
     * "don't say this", not a missing-evidence signal. */
    if (neg_outcome == HU_VERIFY_RESULT_HEDGED) {
        if (cfg->mode == HU_VERIFY_SOFT) {
            snprintf(out_report->modified_draft, sizeof(out_report->modified_draft),
                     "%s %.*s.", neg_hedge, (int)draft_len, draft);
            out_report->draft_modified = true;
        }
        out_report->outcome = HU_VERIFY_RESULT_HEDGED;
        return HU_OK;
    }

    if (cfg->mode == HU_VERIFY_SOFT && any_modified) {
        snprintf(out_report->modified_draft, sizeof(out_report->modified_draft), "%s", rebuilt);
        out_report->draft_modified = true;
        out_report->outcome = HU_VERIFY_RESULT_HEDGED;
    } else {
        out_report->outcome = HU_VERIFY_RESULT_SUPPORTED;
    }

    return HU_OK;
#else
    (void)memory;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    /* sprint-2c Story A — even without SQLite the negative-memory pass
     * fires above and may have set the outcome already. Surface HEDGED
     * for SOFT-mode callers in the no-SQLite build too. */
    if (neg_outcome == HU_VERIFY_RESULT_HEDGED) {
        out_report->outcome = HU_VERIFY_RESULT_HEDGED;
        if (cfg->mode == HU_VERIFY_SOFT) {
            snprintf(out_report->modified_draft, sizeof(out_report->modified_draft),
                     "%s %.*s.", neg_hedge, (int)draft_len, draft);
            out_report->draft_modified = true;
        }
    }
    return HU_OK;
#endif
}
