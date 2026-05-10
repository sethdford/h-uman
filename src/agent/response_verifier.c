#include "human/agent/response_verifier.h"
#include "human/agent/self_rag.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Lightweight extractor: split the draft into sentences (period / question /
 * exclamation), keep only those with at least 3 alpha-token "words" — that's
 * the cheap signal a sentence is declarative rather than a salutation or
 * ack. The verifier itself does the actual scoring. Questions are skipped
 * (they're not factual claims). */
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
        snprintf(out_receipt->source, sizeof(out_receipt->source), "%s",
                 best_prov[0] ? best_prov : "memory");
        out_receipt->observed_at_ms = best_es;
        char ts[32];
        render_timestamp(best_es, ts, sizeof(ts));
        snprintf(out_receipt->rendered, sizeof(out_receipt->rendered), "[from %s, %s]",
                 best_prov[0] ? best_prov : "memory", ts);
    }
    return best_score;
}

#endif

hu_error_t hu_response_verify(hu_allocator_t *alloc, hu_memory_facade_t *memory, const char *contact_id,
                              size_t contact_id_len, const char *draft, size_t draft_len,
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
    if (n == 0)
        return HU_OK;

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
    return HU_OK;
#endif
}
