/* Offline recall comparison: regex/dict extractor (hu_fact_extract) vs LLM
 * extractor (hu_fact_extract_llm) over real inbound iMessage text.
 *
 * Purpose: the dictionary scanner hu_fast_capture recovered only ~2 beliefs
 * from 192 casual messages (starved-subsystems finding). hu_fact_extract_llm
 * exists but is unwired+unmeasured. This tool measures whether the LLM path
 * recovers substantial signal on the SAME corpus — the decision metric for
 * whether wiring it is worth it. No daemon wiring; offline only.
 *
 * Usage: extract_yield <triples_profiled.json> <base_url> <model>
 *   base_url e.g. http://127.0.0.1:8743/v1   model e.g. gemma-4-31b-it-8bit
 * Compiled ad hoc (dev/ASan). Output: per-message + aggregate facts counts.
 */
#include "human/core/allocator.h"
#include "human/memory/fact_extract.h"
#include "human/memory/fact_extract_llm.h"
#include "human/provider.h"
#include "human/providers/openai.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
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
    return b;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: extract_yield <triples.json> <base_url> <model>\n");
        return 2;
    }
    const char *triples_path = argv[1];
    const char *base_url = argv[2];
    const char *model = argv[3];

    hu_allocator_t alloc = hu_system_allocator();

    hu_provider_t prov;
    memset(&prov, 0, sizeof(prov));
    if (hu_openai_create(&alloc, "local", 5, base_url, strlen(base_url), &prov) != HU_OK) {
        fprintf(stderr, "provider create failed\n");
        return 3;
    }

    /* Parse the triples via sqlite json1 (same approach as the backfill tool). */
    sqlite3 *mem = NULL;
    sqlite3_open(":memory:", &mem);
    char *json = slurp(triples_path);
    if (!json) {
        fprintf(stderr, "read triples failed\n");
        return 4;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(mem,
                       "SELECT json_extract(value,'$.contact_id'), json_extract(value,'$.context') "
                       "FROM json_each(?1);",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);

    size_t msgs = 0;
    size_t regex_total = 0, llm_total = 0;
    size_t regex_hit_msgs = 0, llm_hit_msgs = 0;
    int64_t now_ts = 1717000000;

    /* Sample cap from env to limit cost during a smoke (default: all). */
    const char *cap_env = getenv("YIELD_CAP");
    size_t cap = cap_env ? (size_t)strtoul(cap_env, NULL, 10) : (size_t)-1;

    while (sqlite3_step(st) == SQLITE_ROW && msgs < cap) {
        const char *ctx = (const char *)sqlite3_column_text(st, 1);
        if (!ctx || !*ctx)
            continue;
        size_t clen = strlen(ctx);
        msgs++;

        /* Regex/dict path (deterministic, the current production fast-path). */
        hu_fact_extract_result_t rfx;
        memset(&rfx, 0, sizeof(rfx));
        hu_fact_extract(ctx, clen, &rfx);
        regex_total += rfx.fact_count;
        if (rfx.fact_count > 0)
            regex_hit_msgs++;

        /* LLM path (the proposed fix). */
        hu_fact_extract_result_t lfx;
        memset(&lfx, 0, sizeof(lfx));
        hu_error_t e =
            hu_fact_extract_llm(&alloc, &prov, model, strlen(model), ctx, clen, now_ts, &lfx);
        size_t lc = (e == HU_OK) ? lfx.fact_count : 0;
        llm_total += lc;
        if (lc > 0)
            llm_hit_msgs++;

        /* Print a few sample extractions for eyeball validation. */
        if (msgs <= 12) {
            fprintf(stderr, "  [%2zu] regex=%zu llm=%zu | %.50s\n", msgs, rfx.fact_count, lc, ctx);
            for (size_t i = 0; i < lc && i < 3; i++)
                fprintf(stderr, "         llm: %s %s %s\n", lfx.facts[i].subject,
                        lfx.facts[i].predicate, lfx.facts[i].object);
        }
    }
    sqlite3_finalize(st);

    printf("YIELD_DONE msgs=%zu\n", msgs);
    printf("  regex/dict: %zu facts total, %zu/%zu msgs had >=1 (%.0f%% recall)\n", regex_total,
           regex_hit_msgs, msgs, msgs ? 100.0 * regex_hit_msgs / msgs : 0);
    printf("  llm:        %zu facts total, %zu/%zu msgs had >=1 (%.0f%% recall)\n", llm_total,
           llm_hit_msgs, msgs, msgs ? 100.0 * llm_hit_msgs / msgs : 0);
    printf("  facts/msg:  regex=%.2f  llm=%.2f  (lift=%.1fx)\n",
           msgs ? (double)regex_total / msgs : 0, msgs ? (double)llm_total / msgs : 0,
           regex_total ? (double)llm_total / regex_total : (llm_total ? 999.0 : 0));

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
    free(json);
    sqlite3_close(mem);
    return 0;
}
