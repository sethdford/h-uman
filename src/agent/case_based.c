#include "human/agent/case_based.h"

#include "human/memory/memory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* Compute case score: anchor-overlap (Jaccard-ish) + recency decay.
 *   overlap = |A ∩ B| / max(|A|, |B|)
 *   recency = exp(-age_days / 60)
 *   score   = 0.7 * overlap + 0.3 * recency
 */
static float score_case(const int64_t *q_anchors, size_t q_n, const int64_t *r_anchors, size_t r_n,
                        int64_t happened_at, int64_t now_ms) {
    size_t overlap = 0;
    size_t maxn = q_n > r_n ? q_n : r_n;
    if (maxn == 0)
        return 0.0f;
    for (size_t i = 0; i < q_n; i++)
        for (size_t j = 0; j < r_n; j++)
            if (q_anchors[i] == r_anchors[j]) {
                overlap++;
                break;
            }
    float ov = (float)overlap / (float)maxn;
    float rec = 1.0f;
    if (now_ms > 0 && happened_at > 0 && now_ms > happened_at) {
        double age_days = (double)(now_ms - happened_at) / (24.0 * 3600.0 * 1000.0);
        rec = (float)exp(-age_days / 60.0);
    }
    return 0.7f * ov + 0.3f * rec;
}

hu_error_t hu_case_record(hu_memory_facade_t *m, const char *contact_id, size_t contact_id_len,
                          const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count,
                          const char *plan_text, size_t plan_text_len, const char *outcome,
                          size_t outcome_len, int64_t happened_at, int64_t *out_id) {
    if (!m || !goal_verb || goal_verb_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_memory_case_payload_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.goal_verb = (char *)goal_verb;
    pl.goal_verb_len = goal_verb_len;
    pl.anchor_entity_ids = (int64_t *)anchor_entity_ids;
    pl.anchor_count = anchor_count;
    pl.plan_text = (char *)plan_text;
    pl.plan_text_len = plan_text_len;
    pl.outcome = (char *)outcome;
    pl.outcome_len = outcome_len;
    pl.happened_at = happened_at;

    hu_memory_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = HU_MEM_CASE;
    rec.contact_id = contact_id;
    rec.contact_id_len = contact_id_len;
    rec.event_start = happened_at;
    rec.payload = &pl;
    rec.payload_len = sizeof(pl);

    hu_error_t e = hu_memory_facade_write(m, &rec);
    if (e != HU_OK)
        return e;
    if (out_id)
        *out_id = hu_memory_facade_last_case_rowid(m);
    return HU_OK;
}

#else

hu_error_t hu_case_record(hu_memory_facade_t *m, const char *contact_id, size_t contact_id_len,
                          const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count,
                          const char *plan_text, size_t plan_text_len, const char *outcome,
                          size_t outcome_len, int64_t happened_at, int64_t *out_id) {
    (void)m;
    (void)contact_id;
    (void)contact_id_len;
    (void)goal_verb;
    (void)goal_verb_len;
    (void)anchor_entity_ids;
    (void)anchor_count;
    (void)plan_text;
    (void)plan_text_len;
    (void)outcome;
    (void)outcome_len;
    (void)happened_at;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

hu_error_t hu_case_recall(hu_memory_facade_t *m, hu_allocator_t *alloc, const char *contact_id,
                          size_t contact_id_len, const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count, int64_t now_ms,
                          size_t top_k, hu_case_record_t **out, size_t *out_count) {
    if (!m || !alloc || !goal_verb || !out || !out_count || top_k == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#ifndef HU_ENABLE_SQLITE
    (void)contact_id;
    (void)contact_id_len;
    (void)goal_verb_len;
    (void)anchor_entity_ids;
    (void)anchor_count;
    (void)now_ms;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_CASE;
    q.variant = HU_MEMORY_QUERY_CASE;
    q.contact_id = contact_id;
    q.contact_id_len = contact_id_len;
    q.as.cases.goal_verb = goal_verb;
    q.as.cases.goal_len = goal_verb_len;
    q.as.cases.limit = 1024;

    hu_memory_record_t *recs = NULL;
    size_t n = 0;
    hu_error_t err = hu_memory_facade_read(m, &q, alloc, &recs, &n);
    if (err != HU_OK) {
        if (recs)
            hu_memory_facade_records_free(m, alloc, recs, n);
        return err;
    }
    if (n == 0)
        return HU_OK;

    hu_case_record_t *cands = alloc->alloc(alloc->ctx, n * sizeof(hu_case_record_t));
    if (!cands) {
        hu_memory_facade_records_free(m, alloc, recs, n);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(cands, 0, n * sizeof(hu_case_record_t));

    for (size_t i = 0; i < n; i++) {
        hu_case_record_t *r = &cands[i];
        const hu_memory_case_payload_t *pl = (const hu_memory_case_payload_t *)recs[i].payload;
        if (!pl)
            continue;
        r->id = recs[i].id;
        r->happened_at = pl->happened_at;
        if (pl->goal_verb && pl->goal_verb_len > 0) {
            r->goal_verb = alloc->alloc(alloc->ctx, pl->goal_verb_len + 1);
            if (!r->goal_verb) {
                hu_case_records_free(alloc, cands, i);
                hu_memory_facade_records_free(m, alloc, recs, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(r->goal_verb, pl->goal_verb, pl->goal_verb_len);
            r->goal_verb[pl->goal_verb_len] = '\0';
            r->goal_verb_len = pl->goal_verb_len;
        }
        if (pl->plan_text && pl->plan_text_len > 0) {
            r->plan_text = alloc->alloc(alloc->ctx, pl->plan_text_len + 1);
            if (!r->plan_text) {
                hu_case_records_free(alloc, cands, i + 1);
                hu_memory_facade_records_free(m, alloc, recs, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(r->plan_text, pl->plan_text, pl->plan_text_len);
            r->plan_text[pl->plan_text_len] = '\0';
            r->plan_text_len = pl->plan_text_len;
        }
        if (pl->outcome && pl->outcome_len > 0) {
            r->outcome = alloc->alloc(alloc->ctx, pl->outcome_len + 1);
            if (!r->outcome) {
                hu_case_records_free(alloc, cands, i + 1);
                hu_memory_facade_records_free(m, alloc, recs, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(r->outcome, pl->outcome, pl->outcome_len);
            r->outcome[pl->outcome_len] = '\0';
            r->outcome_len = pl->outcome_len;
        }
        if (pl->anchor_entity_ids && pl->anchor_count > 0) {
            r->anchor_entity_ids =
                alloc->alloc(alloc->ctx, pl->anchor_count * sizeof(int64_t));
            if (!r->anchor_entity_ids) {
                hu_case_records_free(alloc, cands, i + 1);
                hu_memory_facade_records_free(m, alloc, recs, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(r->anchor_entity_ids, pl->anchor_entity_ids,
                   pl->anchor_count * sizeof(int64_t));
            r->anchor_count = pl->anchor_count;
        }
        r->score = score_case(anchor_entity_ids, anchor_count, r->anchor_entity_ids,
                              r->anchor_count, r->happened_at, now_ms);
    }

    hu_memory_facade_records_free(m, alloc, recs, n);

    /* Selection sort by score descending; cheap for n <= 1024. */
    for (size_t i = 0; i < n - 1 && i < top_k; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++)
            if (cands[j].score > cands[best].score)
                best = j;
        if (best != i) {
            hu_case_record_t tmp = cands[i];
            cands[i] = cands[best];
            cands[best] = tmp;
        }
    }

    size_t keep = n < top_k ? n : top_k;
    if (keep < n) {
        for (size_t i = keep; i < n; i++) {
            if (cands[i].goal_verb)
                alloc->free(alloc->ctx, cands[i].goal_verb, cands[i].goal_verb_len + 1);
            if (cands[i].plan_text)
                alloc->free(alloc->ctx, cands[i].plan_text, cands[i].plan_text_len + 1);
            if (cands[i].outcome)
                alloc->free(alloc->ctx, cands[i].outcome, cands[i].outcome_len + 1);
            if (cands[i].anchor_entity_ids)
                alloc->free(alloc->ctx, cands[i].anchor_entity_ids,
                            cands[i].anchor_count * sizeof(int64_t));
        }
    }

    *out = cands;
    *out_count = keep;
    return HU_OK;
#endif
}

void hu_case_records_free(hu_allocator_t *alloc, hu_case_record_t *records, size_t count) {
    if (!alloc || !records)
        return;
    for (size_t i = 0; i < count; i++) {
        if (records[i].goal_verb)
            alloc->free(alloc->ctx, records[i].goal_verb, records[i].goal_verb_len + 1);
        if (records[i].plan_text)
            alloc->free(alloc->ctx, records[i].plan_text, records[i].plan_text_len + 1);
        if (records[i].outcome)
            alloc->free(alloc->ctx, records[i].outcome, records[i].outcome_len + 1);
        if (records[i].anchor_entity_ids)
            alloc->free(alloc->ctx, records[i].anchor_entity_ids,
                        records[i].anchor_count * sizeof(int64_t));
    }
    alloc->free(alloc->ctx, records, 0);
}
