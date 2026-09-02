/* graph_ingest.c — the one superseding ingest path for facts.
 *
 * See include/human/memory/graph_ingest.h. This exists because the two live
 * writers (daemon.c deep-extract, daemon_comfort_summary.c) called the legacy
 * ON-CONFLICT-update upsert, which never closes a prior edge: a user who
 * moved kept "lives_in king of prussia" AND "lives_in st pete" open, and
 * grounding surfaced both. Three of nine human detections at n=40
 * (2026-07-27) were exactly this stale event-state shape. */
#include "human/memory/graph_ingest.h"

#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* Belief variance for an extracted fact: the extractor gives a point
 * confidence, not a distribution. 0.1 is the W8 default for a single
 * observation; corroboration lowers it on later upserts. */
#define HU_GRAPH_INGEST_DEFAULT_VARIANCE 0.1f

hu_error_t hu_graph_ingest_fact(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *subject, const char *predicate, const char *object,
                                float confidence, int64_t now, const char *provenance) {
    if (!g || !contact_id || contact_id_len == 0 || !subject || !subject[0] || !predicate ||
        !predicate[0] || !object || !object[0])
        return HU_ERR_INVALID_ARGUMENT;
    if (confidence < 0.0f)
        confidence = 0.0f;
    if (confidence > 1.0f)
        confidence = 1.0f;

    size_t subj_len = strlen(subject);
    size_t obj_len = strlen(object);
    hu_relation_type_t type = hu_relation_type_from_string(predicate, strlen(predicate));

    int64_t src_id = 0, tgt_id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, contact_id, contact_id_len, subject, subj_len,
                                            HU_ENTITY_UNKNOWN, NULL, &src_id);
    if (err != HU_OK)
        return err;
    err = hu_graph_upsert_entity(g, contact_id, contact_id_len, object, obj_len, HU_ENTITY_UNKNOWN,
                                 NULL, &tgt_id);
    if (err != HU_OK)
        return err;
    if (src_id <= 0 || tgt_id <= 0)
        return HU_ERR_INTERNAL;

    /* event_start = now, event_end = 0 (open). The resolver closes a prior
     * open edge of the same single-valued type with a different target. */
    int64_t rel_id = 0;
    err = hu_graph_upsert_relation_with_belief(
        g, contact_id, contact_id_len, src_id, tgt_id, type, 1.0f, now, 0, confidence,
        HU_GRAPH_INGEST_DEFAULT_VARIANCE, object, obj_len, provenance,
        provenance ? strlen(provenance) : 0, &rel_id);
    if (err == HU_ERR_IO) {
        /* The SAME fact again: the belief upsert INSERTs and trips
         * UNIQUE(source, target, type) — that is corroboration, not a new
         * truth. The legacy upsert's ON CONFLICT bumps last_seen/weight on the
         * existing edge, which is exactly what a repeat observation means. */
        err = hu_graph_upsert_relation(g, contact_id, contact_id_len, src_id, tgt_id, type, 1.0f,
                                       object, obj_len);
    }
    return err;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_graph_ingest_fact(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *subject, const char *predicate, const char *object,
                                float confidence, int64_t now, const char *provenance) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)subject;
    (void)predicate;
    (void)object;
    (void)confidence;
    (void)now;
    (void)provenance;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
