/* graph_ingest.c — the one superseding ingest path for facts.
 *
 * See include/human/memory/graph_ingest.h. This exists because the two live
 * writers (daemon.c deep-extract, daemon_comfort_summary.c) called the legacy
 * ON-CONFLICT-update upsert, which never closes a prior edge: a user who
 * moved kept "lives_in king of prussia" AND "lives_in st pete" open, and
 * grounding surfaced both. Three of nine human detections at n=40
 * (2026-07-27) were exactly this stale event-state shape. */
#include "human/memory/graph_ingest.h"
#include "human/core/json.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct import_fact {
    char contact[128];
    char subject[128];
    char predicate[64];
    char object[256];
    char source[128];
    float confidence;
    int64_t ts;
} import_fact_t;

static int import_fact_cmp_ts(const void *a, const void *b) {
    int64_t x = ((const import_fact_t *)a)->ts, y = ((const import_fact_t *)b)->ts;
    return (x > y) - (x < y);
}

/* One JSON object per line: {contact, subject, predicate, object, confidence,
 * ts, source}. Facts are ingested in ts order so supersession is chronological
 * (a lives_in from May closes a lives_in from March, never the reverse).
 * Prints {"imported":N,"skipped":M} and fails when nothing was imported — an
 * empty import must not look like a finished one. */
hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path,
                                       const char *exclude, size_t *imported_out,
                                       size_t *skipped_out) {
    if (imported_out)
        *imported_out = 0;
    if (skipped_out)
        *skipped_out = 0;
    if (!alloc || !g || !path)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return HU_ERR_NOT_FOUND;
    size_t cap = 256, n = 0, skipped = 0;
    import_fact_t *facts = (import_fact_t *)alloc->alloc(alloc->ctx, cap * sizeof(*facts));
    if (!facts) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        hu_json_value_t *v = NULL;
        if (hu_json_parse(alloc, line, len, &v) != HU_OK || !v) {
            skipped++;
            continue;
        }
        const char *pred = hu_json_get_string(v, "predicate");
        const char *obj = hu_json_get_string(v, "object");
        const char *subj = hu_json_get_string(v, "subject");
        const char *contact = hu_json_get_string(v, "contact");
        const char *src = hu_json_get_string(v, "source");
        bool excluded = false;
        if (exclude && pred) {
            /* comma-separated, whole-token match */
            const char *p = exclude;
            size_t pl = strlen(pred);
            while (*p) {
                const char *e = strchr(p, ',');
                size_t tl = e ? (size_t)(e - p) : strlen(p);
                if (tl == pl && strncmp(p, pred, pl) == 0) {
                    excluded = true;
                    break;
                }
                p = e ? e + 1 : p + tl;
            }
        }
        if (!pred || !obj || !obj[0] || excluded) {
            skipped++;
            hu_json_free(alloc, v);
            continue;
        }
        if (n == cap) {
            size_t ncap = cap * 2;
            import_fact_t *nf = (import_fact_t *)alloc->alloc(alloc->ctx, ncap * sizeof(*nf));
            if (!nf) {
                hu_json_free(alloc, v);
                break;
            }
            memcpy(nf, facts, n * sizeof(*nf));
            alloc->free(alloc->ctx, facts, cap * sizeof(*facts));
            facts = nf;
            cap = ncap;
        }
        import_fact_t *f = &facts[n++];
        memset(f, 0, sizeof(*f));
        snprintf(f->contact, sizeof(f->contact), "%s", contact && contact[0] ? contact : "self");
        snprintf(f->subject, sizeof(f->subject), "%s", subj && subj[0] ? subj : "user");
        snprintf(f->predicate, sizeof(f->predicate), "%s", pred);
        snprintf(f->object, sizeof(f->object), "%s", obj);
        snprintf(f->source, sizeof(f->source), "%s", src ? src : "import");
        f->confidence = (float)hu_json_get_number(v, "confidence", 0.5);
        f->ts = (int64_t)hu_json_get_number(v, "ts", 0);
        hu_json_free(alloc, v);
    }
    fclose(fp);
    qsort(facts, n, sizeof(*facts), import_fact_cmp_ts);

    size_t imported = 0;
    for (size_t i = 0; i < n; i++) {
        const import_fact_t *f = &facts[i];
        if (hu_graph_ingest_fact(g, f->contact, strlen(f->contact), f->subject, f->predicate,
                                 f->object, f->confidence, f->ts, f->source) == HU_OK)
            imported++;
        else
            skipped++;
    }
    alloc->free(alloc->ctx, facts, cap * sizeof(*facts));
    if (imported_out)
        *imported_out = imported;
    if (skipped_out)
        *skipped_out = skipped;
    return imported > 0 ? HU_OK : HU_ERR_NOT_FOUND;
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

hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path,
                                       const char *exclude, size_t *imported_out,
                                       size_t *skipped_out) {
    (void)alloc;
    (void)g;
    (void)path;
    (void)exclude;
    if (imported_out)
        *imported_out = 0;
    if (skipped_out)
        *skipped_out = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
