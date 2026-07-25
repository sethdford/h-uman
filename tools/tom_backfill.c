/* Offline ToM belief backfill for the SHADOW A/B experiment.
 *
 * Mirrors the daemon's live belief-recording path (src/daemon.c:11500-11534):
 * for each inbound message from a contact, run hu_fast_capture (the SAME
 * production extractor) and hu_tom_persist_belief the primary_topic
 * (HU_BELIEF_KNOWS, 0.8) + up to 3 entities (HU_BELIEF_KNOWS, 0.6).
 *
 * Writes to a SEPARATE backfill DB (argv) — NEVER the live ~/.human/memory.db.
 * Not part of the build; compiled ad hoc against libhuman_core.a (dev/ASan).
 *
 * Usage: tom_backfill <triples_profiled.json> <out_backfill.db>
 *   triples_profiled.json: [{contact_id, context, ...}]  (context = inbound msg)
 */
#include "human/agent/theory_of_mind.h"
#include "human/core/allocator.h"
#include "human/memory/fast_capture.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal JSON string-field extractor for our own well-formed export.
 * We only need contact_id + context per element; the file is machine-written
 * by export_seth_triples.py (no adversarial input). */
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)n + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[rd] = '\0';
    if (len)
        *len = rd;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: tom_backfill <triples.json> <out.db>\n");
        return 2;
    }
    const char *triples_path = argv[1];
    const char *db_path = argv[2];

    hu_allocator_t alloc = hu_system_allocator();
    if (hu_fast_capture_data_init(&alloc) != HU_OK) {
        fprintf(stderr, "fast_capture_data_init failed\n");
        return 3;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "open db failed: %s\n", sqlite3_errmsg(db));
        return 4;
    }
    if (hu_tom_user_beliefs_init_table(db) != HU_OK) {
        fprintf(stderr, "init table failed\n");
        sqlite3_close(db);
        return 5;
    }

    size_t flen = 0;
    char *json = slurp(triples_path, &flen);
    if (!json) {
        fprintf(stderr, "read triples failed\n");
        sqlite3_close(db);
        return 6;
    }

    /* Use the project's JSON parser via a tiny inline walk would be ideal, but
     * to stay dependency-light we parse with sqlite's json1 by loading the file
     * into a temp table. Simpler: shell out the field walk in Python would defeat
     * "same extractor". Instead, parse minimally: the export is an array of
     * objects with "contact_id" and "context" string fields. */
    /* Load via sqlite json1: */
    sqlite3_stmt *st = NULL;
    const char *q = "SELECT json_extract(value,'$.contact_id'), json_extract(value,'$.context') "
                    "FROM json_each(?1);";
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare json_each failed: %s\n", sqlite3_errmsg(db));
        free(json);
        sqlite3_close(db);
        return 7;
    }
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);

    int64_t now_ms = (int64_t)1717000000000LL; /* fixed wallclock (deterministic) */
    size_t msgs = 0, beliefs = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(st, 0);
        const char *ctx = (const char *)sqlite3_column_text(st, 1);
        if (!cid || !ctx || !*ctx)
            continue;
        msgs++;
        hu_fc_result_t fc;
        memset(&fc, 0, sizeof(fc));
        if (hu_fast_capture(&alloc, ctx, strlen(ctx), &fc) != HU_OK)
            continue;
        if (fc.primary_topic && fc.primary_topic[0]) {
            if (hu_tom_persist_belief(db, cid, fc.primary_topic, strlen(fc.primary_topic),
                                      HU_BELIEF_KNOWS, 0.8f, NULL, 0, 0, now_ms) == HU_OK)
                beliefs++;
        }
        for (size_t ei = 0; ei < fc.entity_count && ei < 3; ei++) {
            if (fc.entities[ei].name[0]) {
                if (hu_tom_persist_belief(db, cid, fc.entities[ei].name,
                                          strlen(fc.entities[ei].name), HU_BELIEF_KNOWS, 0.6f, NULL,
                                          0, 0, now_ms) == HU_OK)
                    beliefs++;
            }
        }
        hu_fc_result_deinit(&fc, &alloc);
    }
    sqlite3_finalize(st);

    /* Report per-contact belief counts. */
    sqlite3_stmt *cst = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT contact_id, COUNT(*) FROM tom_user_beliefs GROUP BY contact_id "
                           "ORDER BY COUNT(*) DESC;",
                           -1, &cst, NULL) == SQLITE_OK) {
        fprintf(stderr, "per-contact belief rows (deduped on contact+topic):\n");
        while (sqlite3_step(cst) == SQLITE_ROW)
            fprintf(stderr, "  %-22s %lld\n", sqlite3_column_text(cst, 0),
                    (long long)sqlite3_column_int64(cst, 1));
        sqlite3_finalize(cst);
    }

    printf("BACKFILL_DONE msgs=%zu belief_writes=%zu db=%s\n", msgs, beliefs, db_path);
    free(json);
    sqlite3_close(db);
    hu_fast_capture_data_cleanup(&alloc);
    return 0;
}
