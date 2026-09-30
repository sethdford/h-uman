/* Contract C2 — reconstructive hybrid retrieval (EverMemOS shape):
 * scene-select -> neighbour expansion -> rerank -> time-bounded filter ->
 * sufficiency check, wired behind hu_retrieval_options_t.reconstructive and
 * exercised through hu_hybrid_retrieve (the production symbol under test).
 *
 * SQLite-gated; a stub keeps run_hybrid_reconstructive_tests() resolvable
 * when SQLite is off (mirrors tests/test_semantic_index.c's pattern). */
#ifdef HU_ENABLE_SQLITE
#include "human/cli_commands.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/memory/vector/store_sqlite_vec.h"
#include "test_framework.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- stub embedder (deterministic 3-dim), lifted from test_semantic_index.c ---- */
static hu_error_t stub_embed(void *ctx, hu_allocator_t *alloc, const char *text, size_t len,
                             hu_embedding_t *out) {
    (void)ctx;
    float *v = (float *)alloc->alloc(alloc->ctx, 3 * sizeof(float));
    if (!v)
        return HU_ERR_OUT_OF_MEMORY;
    unsigned c = len ? (unsigned char)text[0] : 0;
    v[0] = (c % 3 == 0) ? 1.0f : 0.1f;
    v[1] = (c % 3 == 1) ? 1.0f : 0.1f;
    v[2] = (c % 3 == 2) ? 1.0f : 0.1f;
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= n;
    v[1] /= n;
    v[2] /= n;
    out->values = v;
    out->dim = 3;
    return HU_OK;
}
static hu_error_t stub_embed_batch(void *ctx, hu_allocator_t *alloc, const char **texts,
                                   const size_t *lens, size_t count, hu_embedding_t *out) {
    for (size_t i = 0; i < count; i++) {
        hu_error_t e = stub_embed(ctx, alloc, texts[i], lens[i], &out[i]);
        if (e != HU_OK)
            return e;
    }
    return HU_OK;
}
static size_t stub_dims(void *ctx) {
    (void)ctx;
    return 3;
}
static void stub_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}
static const hu_embedder_vtable_t stub_vt = {.embed = stub_embed,
                                             .embed_batch = stub_embed_batch,
                                             .dimensions = stub_dims,
                                             .deinit = stub_deinit};

/* Backdate a stored row's created_at (impl_store always writes "now"; the
 * scene/temporal logic under test needs controlled timestamps). Test-only:
 * builds the SQL from trusted, test-authored literals. */
static void set_created_at(hu_memory_t *mem, const char *key, const char *iso_ts) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE memories SET created_at='%s' WHERE key='%s'", iso_ts, key);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void fmt_iso(time_t t, char *buf, size_t buf_cap) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, buf_cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static bool result_has_key(const hu_retrieval_result_t *r, const char *key) {
    for (size_t i = 0; i < r->count; i++)
        if (r->entries[i].key && strcmp(r->entries[i].key, key) == 0)
            return true;
    return false;
}

static bool result_has_content_word(const hu_retrieval_result_t *r, const char *word) {
    for (size_t i = 0; i < r->count; i++)
        if (r->entries[i].content && strstr(r->entries[i].content, word))
            return true;
    return false;
}

/* strlen()-derived lengths throughout -- a hand-counted literal length is a
 * one-off-away key/content truncation that silently corrupts the WHERE
 * clause in set_created_at() above (caught during authoring of this file). */
static hu_error_t store_row(hu_memory_t *mem, const char *key, const char *content,
                            const char *session_id) {
    return mem->vtable->store(mem->ctx, key, strlen(key), content, strlen(content), NULL,
                              session_id, session_id ? strlen(session_id) : 0);
}

/* AC-1: scene-select picks the two-hit session over the one-hit session.
 * Session A has two matching rows, session B has one; with limit=2 the
 * reconstructive path must fill the answer from A alone, never from B. */
static void test_scene_select_prefers_two_hit_session_over_one_hit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    HU_ASSERT_EQ(store_row(&mem, "a1", "regatta sailing schedule saturday morning race", "A"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a2", "regatta sailing schedule friday committee meeting", "A"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "regatta sailing schedule postponed announcement", "B"),
                 HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 2;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "regatta sailing schedule";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    HU_ASSERT_TRUE(res.count > 0);
    HU_ASSERT_TRUE(result_has_key(&res, "a1") || result_has_key(&res, "a2"));
    HU_ASSERT_TRUE(!result_has_key(&res, "b1"));

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* AC-2: a temporal cue ("still") in the query prefers the newer of two rows
 * that share a key prefix (a superseded fact family), dropping the older one
 * even though both are keyword hits. */
static void test_temporal_cue_prefers_newer_same_prefix_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(store_row(&mem, "profile:city:2026-01-01", "I live in Springfield", NULL), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "profile:city:2026-06-01", "I live in Shelbyville now", NULL),
                 HU_OK);

    time_t now = time(NULL);
    char old_ts[64], new_ts[64];
    fmt_iso(now - (time_t)(180 * 86400), old_ts, sizeof(old_ts)); /* ~6 months ago */
    fmt_iso(now, new_ts, sizeof(new_ts));
    set_created_at(&mem, "profile:city:2026-01-01", old_ts);
    set_created_at(&mem, "profile:city:2026-06-01", new_ts);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "do I still live in shelbyville";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    HU_ASSERT_EQ((long)res.count, 1L);
    HU_ASSERT_TRUE(result_has_content_word(&res, "Shelbyville"));
    HU_ASSERT_TRUE(!result_has_content_word(&res, "Springfield"));

    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
}

/* AC-3: when the candidate pool only spans one scene, the sufficiency check
 * must fall back to the plain hybrid result -- never an empty one, even
 * though reconstructive=true was requested. */
static void test_sufficiency_fallback_returns_plain_result_for_one_scene(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    HU_ASSERT_EQ(store_row(&mem, "s1", "budget meeting notes for the project", "S"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "s2", "budget meeting followup action items", "S"), HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "budget meeting";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    /* Only one scene (session "S") exists -- reconstruction must decline and
     * the plain hybrid merge must still answer. */
    HU_ASSERT_TRUE(res.count > 0);

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* 2026-09-03 C2-ablation bug: every row reaching the caller through the plain
 * RRF+cross-encoder merge (the reconstructive path's fallback AND the plain
 * hybrid path) had entry.key = entry.content because hu_search_result_t
 * carried no key. The eval harnesses parse the session id out of the printed
 * key, so those rows scored as misses. Each entry's key must be the
 * memories.key of the row it came from, never its content. */
static void assert_entries_carry_stored_keys(const hu_retrieval_result_t *res) {
    HU_ASSERT_TRUE(res->count > 0);
    for (size_t i = 0; i < res->count; i++) {
        const hu_memory_entry_t *e = &res->entries[i];
        HU_ASSERT_NOT_NULL(e->key);
        HU_ASSERT_NOT_NULL(e->content);
        HU_ASSERT_EQ(e->key_len, 2u);
        HU_ASSERT_TRUE(strcmp(e->key, "s1") == 0 || strcmp(e->key, "s2") == 0);
        HU_ASSERT_TRUE(strcmp(e->key, e->content) != 0);
        HU_ASSERT_TRUE(e->content_len > e->key_len);
    }
}

static void store_one_scene_two_rows(hu_memory_t *mem) {
    HU_ASSERT_EQ(store_row(mem, "s1", "budget meeting notes for the project", "S"), HU_OK);
    HU_ASSERT_EQ(store_row(mem, "s2", "budget meeting followup action items", "S"), HU_OK);
}

static void test_fallback_path_entries_carry_memories_key_not_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    store_one_scene_two_rows(&mem);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true; /* one scene -> insufficient -> plain merge fallback */
    hu_retrieval_result_t res = {0};
    const char *q = "budget meeting";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);
    assert_entries_carry_stored_keys(&res);

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_plain_hybrid_entries_carry_memories_key_not_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    store_one_scene_two_rows(&mem);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = false; /* plain keyword+semantic RRF merge, no C2 */
    hu_retrieval_result_t res = {0};
    const char *q = "budget meeting";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);
    assert_entries_carry_stored_keys(&res);

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* The CLI line the benchmark harness parses: "  [n] <key> (<score>): <content>".
 * Driven by a real fallback-path retrieval so a content-in-key regression shows
 * up as the wrong token before " (". */
static void test_cli_hybrid_line_prints_key_then_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    store_one_scene_two_rows(&mem);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "budget meeting";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);
    HU_ASSERT_TRUE(res.count >= 2);

    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_cli_memory_search_emit(f, &res);
    rewind(f);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Line 1 is "  [1] sN (x.xxx): budget meeting ..." -- key first, then score. */
    HU_ASSERT_TRUE(strncmp(buf, "  [1] s", 7) == 0);
    HU_ASSERT_TRUE(buf[7] == '1' || buf[7] == '2');
    HU_ASSERT_TRUE(strncmp(buf + 8, " (", 2) == 0);
    HU_ASSERT_STR_CONTAINS(buf, "): budget meeting ");
    HU_ASSERT_STR_CONTAINS(buf, "\n  [2] s");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "[1] budget meeting");

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* ── HU_RECON_ABLATE stage ablation tests (Contract C2 ablation study,
 * docs/plans/2026-08-02-semantic-retrieval/memory-benchmarks-c2-ablation.json)
 * ─────────────────────────────────────────────────────────────────────────
 * Each test unsets HU_RECON_ABLATE immediately after the ablated call and
 * before any assertion on its result, so a failing HU_ASSERT (which
 * longjmp()s out of the test body) can never leave the env var set for a
 * later test. */

/* AC-4 (no_scene): with scene-select disabled, a low-scoring session's row
 * enters the shared rerank pool and can outrank a WEAKER row from the
 * top session -- proving scene-select, not just the final `limit` trim, is
 * what excludes it by default. */
static void test_ablate_no_scene_admits_low_scoring_session(void) {
    unsetenv("HU_RECON_ABLATE");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    /* hu_keyword_retrieve (and hu_semantic_retrieve) each truncate to
     * opts->limit BEFORE hybrid_reconstruct ever sees the candidates -- with
     * limit=2 and a single (keyword-only) source, the pool can never exceed
     * 2 rows, so scene-select would have nothing to exclude. A second,
     * independently-truncated source (semantic, via the stub embedder) is
     * what lets the pool exceed `limit` -- the same apparatus
     * test_scene_select_prefers_two_hit_session_over_one_hit above uses. */
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    /* Same fixture as test_scene_select_prefers_two_hit_session_over_one_hit
     * above (all three rows are FULL term-overlap hits, deliberately tied,
     * so keyword/semantic retrieval's own top-`limit` truncation admits a1+a2
     * -- not a1+b1 -- into the pool ahead of scene-select ever running; a
     * partial-overlap a2 was tried first and instead got truncated upstream
     * of scene-select entirely, which would have measured the wrong stage). */
    HU_ASSERT_EQ(store_row(&mem, "a1", "regatta sailing schedule saturday morning race", "A"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a2", "regatta sailing schedule friday committee meeting", "A"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "regatta sailing schedule postponed announcement", "B"),
                 HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 2;
    opts.reconstructive = true;
    const char *q = "regatta sailing schedule";

    hu_retrieval_result_t res_default = {0};
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res_default),
        HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_default, "a1"));
    HU_ASSERT_TRUE(result_has_key(&res_default, "a2"));
    HU_ASSERT_TRUE(!result_has_key(&res_default, "b1"));
    hu_retrieval_result_free(&alloc, &res_default);

    setenv("HU_RECON_ABLATE", "no_scene", 1);
    hu_retrieval_result_t res_ablated = {0};
    hu_error_t err =
        hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res_ablated);
    unsetenv("HU_RECON_ABLATE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "b1"));
    HU_ASSERT_TRUE(!result_has_key(&res_ablated, "a2"));

    hu_retrieval_result_free(&alloc, &res_ablated);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* AC-5 (no_neighbors): a non-matching row adjacent (by timestamp) to a
 * keyword-hit anchor is normally pulled in by neighbour expansion; disabling
 * it must leave the anchor's neighbourhood out of the result. A second
 * session (b1) keeps distinct_scenes_in_pool >= HU_RECON_MIN_SCENES in both
 * runs, isolating the neighbour-expansion effect from the sufficiency gate. */
static void test_ablate_no_neighbors_drops_session_adjacent_rows(void) {
    unsetenv("HU_RECON_ABLATE");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(store_row(&mem, "a0", "javelin throwing practice notes", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a1", "regatta sailing schedule saturday race", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a2", "javelin throwing recap results", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "regatta sailing schedule committee notice", "B"), HU_OK);
    set_created_at(&mem, "a0", "2026-01-01T00:00:00Z");
    set_created_at(&mem, "a1", "2026-01-02T00:00:00Z");
    set_created_at(&mem, "a2", "2026-01-03T00:00:00Z");
    set_created_at(&mem, "b1", "2026-01-02T00:00:00Z");

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    const char *q = "regatta sailing schedule";

    hu_retrieval_result_t res_default = {0};
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res_default),
        HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_default, "a0"));
    HU_ASSERT_TRUE(result_has_key(&res_default, "a2"));
    hu_retrieval_result_free(&alloc, &res_default);

    setenv("HU_RECON_ABLATE", "no_neighbors", 1);
    hu_retrieval_result_t res_ablated = {0};
    hu_error_t err =
        hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res_ablated);
    unsetenv("HU_RECON_ABLATE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result_has_key(&res_ablated, "a0"));
    HU_ASSERT_TRUE(!result_has_key(&res_ablated, "a2"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "a1"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "b1"));

    hu_retrieval_result_free(&alloc, &res_ablated);
    mem.vtable->deinit(mem.ctx);
}

/* AC-6 (no_rerank): the sufficiency floor is checked against work_scores[0],
 * which the rerank stage overwrites with a term-overlap FRACTION (0..1).
 * Skip rerank and that slot keeps the raw RRF pool score instead (~1/61 for
 * a single-source pool) -- always below HU_RECON_SCORE_FLOOR, so a query
 * that would otherwise reconstruct successfully (full term overlap) instead
 * falls back, losing the neighbour-expanded row that only the reconstructive
 * commit path returns. */
static void test_ablate_no_rerank_starves_sufficiency_floor(void) {
    unsetenv("HU_RECON_ABLATE");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(store_row(&mem, "a0", "unrelated context note", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a1", "alpha beta content here", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "alpha beta similar content", "B"), HU_OK);
    set_created_at(&mem, "a0", "2026-01-01T00:00:00Z");
    set_created_at(&mem, "a1", "2026-01-02T00:00:00Z");
    set_created_at(&mem, "b1", "2026-01-02T00:00:00Z");

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    const char *q = "alpha beta";

    hu_retrieval_result_t res_default = {0};
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res_default),
        HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_default, "a0"));
    hu_retrieval_result_free(&alloc, &res_default);

    setenv("HU_RECON_ABLATE", "no_rerank", 1);
    hu_retrieval_result_t res_ablated = {0};
    hu_error_t err =
        hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res_ablated);
    unsetenv("HU_RECON_ABLATE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result_has_key(&res_ablated, "a0"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "a1"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "b1"));

    hu_retrieval_result_free(&alloc, &res_ablated);
    mem.vtable->deinit(mem.ctx);
}

/* AC-7 (no_temporal): with the time-bounded filter disabled, a temporal-cue
 * query no longer drops the superseded (older) same-key-prefix row -- the
 * mirror image of test_temporal_cue_prefers_newer_same_prefix_row above. */
static void test_ablate_no_temporal_keeps_superseded_row(void) {
    unsetenv("HU_RECON_ABLATE");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(store_row(&mem, "profile:city:2026-01-01", "I live in Springfield", NULL), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "profile:city:2026-06-01", "I live in Shelbyville now", NULL),
                 HU_OK);
    time_t now = time(NULL);
    char old_ts[64], new_ts[64];
    fmt_iso(now - (time_t)(180 * 86400), old_ts, sizeof(old_ts));
    fmt_iso(now, new_ts, sizeof(new_ts));
    set_created_at(&mem, "profile:city:2026-01-01", old_ts);
    set_created_at(&mem, "profile:city:2026-06-01", new_ts);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    const char *q = "do I still live in shelbyville";

    setenv("HU_RECON_ABLATE", "no_temporal", 1);
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res);
    unsetenv("HU_RECON_ABLATE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((long)res.count, 2L);
    HU_ASSERT_TRUE(result_has_content_word(&res, "Shelbyville"));
    HU_ASSERT_TRUE(result_has_content_word(&res, "Springfield"));

    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
}

/* AC-8 (force_sufficient): mirrors AC-6's fixture and floor-starvation logic,
 * but with rerank left ON and force_sufficient overriding the floor instead
 * -- proving the flag routes through the reconstructive commit (returning
 * the neighbour-expanded a0) instead of AC-3's plain-hybrid fallback, on a
 * fixture that would otherwise fall back for the OPPOSITE reason (score
 * floor, not the MIN_SCENES case AC-3 covers). */
static void test_ablate_force_sufficient_returns_reconstruction(void) {
    unsetenv("HU_RECON_ABLATE");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(store_row(&mem, "a0", "unrelated context note", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a1", "alpha only content here", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "beta only content here", "B"), HU_OK);
    set_created_at(&mem, "a0", "2026-01-01T00:00:00Z");
    set_created_at(&mem, "a1", "2026-01-02T00:00:00Z");
    set_created_at(&mem, "b1", "2026-01-02T00:00:00Z");

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    /* 3-word query; a1 matches only "alpha" (1/3) and b1 only "beta" (1/3) --
     * both below HU_RECON_SCORE_FLOOR (0.34) even after rerank, so the
     * default run falls back to plain keyword results (no neighbours). */
    const char *q = "alpha beta gamma";

    hu_retrieval_result_t res_default = {0};
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res_default),
        HU_OK);
    HU_ASSERT_TRUE(!result_has_key(&res_default, "a0"));
    hu_retrieval_result_free(&alloc, &res_default);

    setenv("HU_RECON_ABLATE", "force_sufficient", 1);
    hu_retrieval_result_t res_ablated = {0};
    hu_error_t err =
        hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res_ablated);
    unsetenv("HU_RECON_ABLATE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "a0"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "a1"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "b1"));

    hu_retrieval_result_free(&alloc, &res_ablated);
    mem.vtable->deinit(mem.ctx);
}

/* AC-9 (scene_coverage_first): session A spans two day-buckets (two scenes),
 * both outscoring session B's single scene; with limit=2 the default picks
 * BOTH of A's scenes and drops B entirely. scene_coverage_first reorders
 * scene-select to take each session's best scene first, so B survives at
 * the cost of A's second (lower) scene -- the coverage/precision trade the
 * ablation study is measuring (memory-benchmarks-c2.json: multi-session
 * 0.8 vs plain-hybrid 1.0). */
static void test_ablate_scene_coverage_first_admits_second_session(void) {
    unsetenv("HU_RECON_ABLATE");
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    /* Needs a second (semantic) source, same as test_ablate_no_scene above --
     * a single keyword-only source is truncated to opts->limit upstream, so
     * the pool could never exceed `limit` and scene-select would have
     * nothing to reorder. */
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    /* Insertion order is deliberately a2, b1, a1 (not the a1/a2/b1 reading
     * order): every same-first-letter row ties at cosine 1.0 under the stub
     * embedder, and this vector store resolves an exact KNN tie in favor of
     * the MOST RECENTLY inserted rows -- inserting a1 last is what gives it
     * a semantic hit too (double-sourced: keyword AND semantic), clearly
     * outscoring the single-sourced a2 and b1 instead of leaving a three-way
     * near-tie for the scene sort to resolve unpredictably. a1/a2 are both
     * full 3/3 keyword hits; b1 matches only "alpha" (1/3) but shares their
     * semantic class, so it still enters the pool as its own (session-less,
     * since hu_semantic_retrieve never sets session_id) scene -- the same
     * mechanism test_ablate_no_scene above relies on. */
    HU_ASSERT_EQ(store_row(&mem, "a2", "alpha beta gamma notes two", "A"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "alpha only mention here", "B"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a1", "alpha beta gamma notes one", "A"), HU_OK);
    set_created_at(&mem, "a1", "2026-01-01T00:00:00Z");
    set_created_at(&mem, "a2", "2026-02-01T00:00:00Z");
    set_created_at(&mem, "b1", "2026-01-15T00:00:00Z");

    hu_retrieval_options_t opts = {0};
    opts.limit = 2;
    opts.reconstructive = true;
    const char *q = "alpha beta gamma";

    /* Default: a1 (day1) and a2 (day2) -- both session A -- are the two
     * highest-scoring scenes and together already cover `limit`, so B never
     * gets a look-in. */
    hu_retrieval_result_t res_default = {0};
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res_default),
        HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_default, "a1"));
    HU_ASSERT_TRUE(result_has_key(&res_default, "a2"));
    HU_ASSERT_TRUE(!result_has_key(&res_default, "b1"));
    hu_retrieval_result_free(&alloc, &res_default);

    /* Ablated: scene_coverage_first takes a1's scene (session A's best) and
     * B's scene first, dropping a2's scene instead of B's -- session
     * coverage over within-session precision. no_neighbors is stacked on
     * purpose: neighbour expansion (stage 2) pulls session-adjacent rows by
     * RAW session_id, not by the day-scene scene-select chose, so without
     * it a1's neighbour lookup on session "A" would silently re-admit a2
     * and erase the very trade-off this ablation is measuring -- a real
     * cross-stage interaction this study surfaced. */
    setenv("HU_RECON_ABLATE", "scene_coverage_first,no_neighbors", 1);
    hu_retrieval_result_t res_ablated = {0};
    hu_error_t err =
        hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res_ablated);
    unsetenv("HU_RECON_ABLATE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "a1"));
    HU_ASSERT_TRUE(result_has_key(&res_ablated, "b1"));
    HU_ASSERT_TRUE(!result_has_key(&res_ablated, "a2"));

    hu_retrieval_result_free(&alloc, &res_ablated);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* The plain (non-reconstructive) merge is what the daemon's memory loader
 * calls, with use_reranking=false. RRF must be the final order there: the
 * term-overlap "cross-encoder" re-sorted the fused pool by query-word
 * overlap before the cut to `limit`, which evicted every semantic-only hit
 * and collapsed production recall to the keyword-only number (LoCoMo R@10
 * 0.65 == keyword 0.65, LongMemEval 0.817 vs 1.0 for plain RRF; measured
 * 2026-09-20, memory-benchmarks-hybrid-plain-gemma-2026-09-20.json).
 *
 * Fixture (stub embedder = first-character class mod 3): the query "zebra
 * migration" is class 2. "insight:herd" is the semantic hit — class-2 text
 * with none of the query words. The two keyword hits carry every query word
 * but live under "experience:" keys, which the index write path never
 * embeds, so they get no fusion boost. Plain RRF top-2 = {herd, one keyword
 * hit}; an overlap rerank makes it {both keyword hits} and drops herd. */
static void test_plain_hybrid_without_reranking_keeps_semantic_only_hit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    HU_ASSERT_EQ(store_row(&mem, "insight:herd", "the herd crossed the river at dawn", "s1"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "insight:coffee", "cold morning coffee on the porch", "s1"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "insight:dinner", "dinner plans for friday night", "s1"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "experience:b1", "annual zebra migration begins", "s2"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "experience:b2", "facts about zebra migration", "s2"), HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 2;
    opts.reconstructive = false;
    opts.use_reranking = false; /* the memory loader's setting */
    hu_retrieval_result_t res = {0};
    const char *q = "zebra migration";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)2);
    HU_ASSERT_TRUE(result_has_key(&res, "insight:herd")); /* semantic-only hit survives */
    HU_ASSERT_TRUE(result_has_key(&res, "experience:b1") || result_has_key(&res, "experience:b2"));

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* The keyword leg of the plain merge must be the backend's ranked recall
 * (FTS5 BM25 on SQLite -- the list `human memory search <q>` prints), not
 * hu_keyword_retrieve's matched-word fraction. The fraction has no term
 * weighting and no length normalisation, so a padded row that mentions every
 * query word once outranks a short row that is nothing but the query; on
 * LoCoMo-10 (60 questions, seed 3, EmbeddingGemma index, 2026-09-20) that leg
 * held the evidence in 27/60 top-10 lists vs 39/60 for BM25 and the fused
 * R@10 was 0.783 vs 0.850 for RRF over the BM25 list, with the semantic leg
 * and the merge byte-identical (hybrid-plain-keyword-leg-2026-09-20.md).
 *
 * Fixture: every row lives under an "experience:" key, which the index write
 * path never embeds, so the semantic leg is empty and limit=1 returns the
 * keyword leg's rank 1. "stuffed" carries all three query words inside 120
 * filler words (fraction 1.0, rank 1 under the old scorer); "short" is the
 * two-word "zebra migration" (fraction 0.667). BM25 ranks short first
 * (-1.04 vs -0.78, measured on the same rows in sqlite). */
static void test_plain_hybrid_keyword_leg_ranks_by_backend_recall_not_word_fraction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    static const char filler[] =
        "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor "
        "incididunt ut labore et dolore magna aliqua enim ad minim veniam quis nostrud "
        "exercitation ullamco laboris nisi aliquip ex ea commodo consequat duis aute irure "
        "in reprehenderit voluptate ";
    char stuffed[1024];
    snprintf(stuffed, sizeof(stuffed), "zebra migration season %s%s%s", filler, filler, filler);
    HU_ASSERT_EQ(store_row(&mem, "experience:stuffed", stuffed, "s1"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "experience:short", "zebra migration", "s1"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "experience:f1", "cold morning coffee on the porch", "s1"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "experience:f2", "dinner plans for friday night", "s1"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "experience:f3", "the herd crossed the river at dawn", "s1"),
                 HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 1;
    opts.reconstructive = false;
    opts.use_reranking = false; /* the memory loader's setting */
    hu_retrieval_result_t res = {0};
    const char *q = "zebra migration season";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);
    HU_ASSERT_EQ(res.count, (size_t)1);
    HU_ASSERT_STR_EQ(res.entries[0].key, "experience:short");

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* HU_HYBRID_FUSION wiring in the plain merge (the memory loader's call).
 *
 * Fixture (stub embedder = first-character class mod 3; the query "zebra
 * migration" is class 2): the keyword leg is the two "experience:" rows --
 * never embedded -- where "short" is nothing but the query and "long" pads
 * it, so BM25 ranks short first. The semantic leg is the "insight:" rows:
 * "herd" is class 2 (cosine 1.0), the fillers are classes 0/1 (cosine ~0.21,
 * chosen so none starts with a class-2 letter). Four fillers keep the query
 * terms rare enough (2 of 7 rows) for FTS5 to give them a positive IDF. */
typedef struct fusion_fixture {
    hu_memory_t mem;
    hu_embedder_t emb;
    hu_vector_store_t vs;
} fusion_fixture_t;

static void fusion_fixture_open(fusion_fixture_t *f, hu_allocator_t *alloc) {
    f->mem = hu_sqlite_memory_create(alloc, ":memory:");
    HU_ASSERT_NOT_NULL(f->mem.vtable);
    f->emb = (hu_embedder_t){.ctx = NULL, .vtable = &stub_vt};
    f->vs = hu_vector_store_sqlite_vec_create(alloc, hu_sqlite_memory_get_db(&f->mem), 3);
    HU_ASSERT_NOT_NULL(f->vs.ctx);
    hu_sqlite_memory_set_semantic_index(&f->mem, &f->emb, &f->vs);
    static const char *const rows[][2] = {
        {"experience:short", "zebra migration"},
        {"experience:long", "zebra migration notes from a long field season on the plains"},
        {"insight:herd", "the herd crossed the river at dawn"},
        {"insight:coffee", "cold morning coffee on the porch"},
        {"insight:dinner", "dinner plans for friday night"},
        {"insight:grocery", "grocery list for the weekend"},
        {"insight:apple", "apple pie recipe from grandma"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        HU_ASSERT_EQ(store_row(&f->mem, rows[i][0], rows[i][1], "s1"), HU_OK);
}

static void fusion_fixture_close(fusion_fixture_t *f, hu_allocator_t *alloc) {
    hu_sqlite_memory_set_semantic_index(&f->mem, NULL, NULL);
    f->vs.vtable->deinit(f->vs.ctx, alloc);
    f->mem.vtable->deinit(f->mem.ctx);
}

/* Run the plain hybrid call (limit 4) and copy out the result keys in order. */
static size_t fusion_run(fusion_fixture_t *f, hu_allocator_t *alloc, char keys[][40],
                         double *top_score) {
    hu_retrieval_options_t opts = {0};
    opts.limit = 4;
    opts.reconstructive = false;
    opts.use_reranking = false; /* the memory loader's setting */
    hu_retrieval_result_t res = {0};
    const char *q = "zebra migration";
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(alloc, &f->mem, &f->emb, &f->vs, NULL, q, strlen(q), &opts, &res),
        HU_OK);
    HU_ASSERT_GT(res.count, 0u);
    for (size_t i = 0; i < res.count; i++)
        snprintf(keys[i], 40, "%s", res.entries[i].key ? res.entries[i].key : "");
    *top_score = res.scores[0];
    size_t n = res.count;
    hu_retrieval_result_free(alloc, &res);
    return n;
}

static size_t key_pos(char keys[][40], size_t n, const char *key) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(keys[i], key) == 0)
            return i;
    return n;
}

/* Default (unset), explicit "rrf", and an unknown value must all produce the
 * RRF merge exactly as it was before the gate existed: the same key order and
 * RRF-scale scores. The engine's recall appends spreading-activation rows
 * after its two FTS hits (measured on this fixture: short -1.97, long -1.24,
 * then herd +0.77 and coffee +0.21), so herd is keyword rank 3 AND semantic
 * rank 1 and RRF puts it first at 1/61 + 1/63. This test passed against the
 * pre-gate code unchanged. */
static void test_plain_hybrid_fusion_gate_default_is_rrf_order(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fusion_fixture_t f;
    fusion_fixture_open(&f, &alloc);
    char unset_keys[4][40], rrf_keys[4][40], bogus_keys[4][40];
    double s_unset = 0, s_rrf = 0, s_bogus = 0;

    unsetenv("HU_HYBRID_FUSION");
    size_t n_unset = fusion_run(&f, &alloc, unset_keys, &s_unset);
    setenv("HU_HYBRID_FUSION", "rrf", 1);
    size_t n_rrf = fusion_run(&f, &alloc, rrf_keys, &s_rrf);
    setenv("HU_HYBRID_FUSION", "bogus", 1);
    size_t n_bogus = fusion_run(&f, &alloc, bogus_keys, &s_bogus);
    unsetenv("HU_HYBRID_FUSION");

    HU_ASSERT_EQ(n_unset, 4u);
    HU_ASSERT_EQ(n_rrf, n_unset);
    HU_ASSERT_EQ(n_bogus, n_unset);
    for (size_t i = 0; i < n_unset; i++) {
        HU_ASSERT_STR_EQ(rrf_keys[i], unset_keys[i]);
        HU_ASSERT_STR_EQ(bogus_keys[i], unset_keys[i]);
    }
    HU_ASSERT_STR_EQ(unset_keys[0], "insight:herd");
    HU_ASSERT_STR_EQ(unset_keys[1], "experience:short");
    HU_ASSERT_FLOAT_EQ(s_unset, 1.0 / 61.0 + 1.0 / 63.0, 1e-6);
    HU_ASSERT_FLOAT_EQ(s_rrf, s_unset, 1e-9);
    HU_ASSERT_FLOAT_EQ(s_bogus, s_unset, 1e-9);
    fusion_fixture_close(&f, &alloc);
}

/* HU_HYBRID_FUSION=score reaches the merge: alpha=1 puts the best dense row
 * first with fused score 1.0 (RRF puts the keyword row first at 1/61);
 * alpha=0 puts the best BM25 row first with 1.0 -- "short", proving the
 * lower-is-better bm25() was flipped before normalising (a sign slip would
 * lead with "long"). */
static void test_plain_hybrid_score_fusion_alpha_extremes_reorder_the_merge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fusion_fixture_t f;
    fusion_fixture_open(&f, &alloc);
    char keys[4][40];
    double top = 0;

    setenv("HU_HYBRID_FUSION", "score", 1);
    setenv("HU_HYBRID_FUSION_ALPHA", "1", 1);
    size_t n = fusion_run(&f, &alloc, keys, &top);
    HU_ASSERT_EQ(n, 4u);
    HU_ASSERT_STR_EQ(keys[0], "insight:herd");
    HU_ASSERT_FLOAT_EQ(top, 1.0, 1e-6);

    setenv("HU_HYBRID_FUSION_ALPHA", "0", 1);
    n = fusion_run(&f, &alloc, keys, &top);
    HU_ASSERT_EQ(n, 4u);
    HU_ASSERT_STR_EQ(keys[0], "experience:short");
    HU_ASSERT_FLOAT_EQ(top, 1.0, 1e-6);
    HU_ASSERT_LT(key_pos(keys, n, "experience:short"), key_pos(keys, n, "experience:long"));
    HU_ASSERT_LT(key_pos(keys, n, "experience:long"), n);

    unsetenv("HU_HYBRID_FUSION");
    unsetenv("HU_HYBRID_FUSION_ALPHA");
    fusion_fixture_close(&f, &alloc);
}

/* The engine's graph rerank adds a boost (temporal: newest node +0.05;
 * entity edges: +0.03 each, cap 0.15) to bm25(), which is lower-is-better,
 * so in score mode the boost used to read as a PENALTY -- and the monotone
 * clamp then dragged every row below it down to the same value. Fixture:
 * "plain" and "boosted" carry the same query terms at the same length (equal
 * bm25); four earlier "Alice" rows give "boosted" four entity edges, and it
 * is the newest node, so its boost is 0.05 + 4*0.03 = 0.17. "plain" has no
 * shared entity and a successor, so its boost is 0. */
static void boost_fixture_open(fusion_fixture_t *f, hu_allocator_t *alloc) {
    f->mem = hu_sqlite_memory_create(alloc, ":memory:");
    HU_ASSERT_NOT_NULL(f->mem.vtable);
    f->emb = (hu_embedder_t){.ctx = NULL, .vtable = &stub_vt};
    f->vs = hu_vector_store_sqlite_vec_create(alloc, hu_sqlite_memory_get_db(&f->mem), 3);
    HU_ASSERT_NOT_NULL(f->vs.ctx);
    hu_sqlite_memory_set_semantic_index(&f->mem, &f->emb, &f->vs);
    static const char *const rows[][2] = {
        {"experience:a1", "lunch with Alice downtown"},
        {"experience:a2", "coffee with Alice again"},
        {"experience:a3", "movie night with Alice"},
        {"experience:a4", "a long walk with Alice"},
        {"experience:plain", "zebra migration seen by Bob"},
        {"experience:boosted", "zebra migration seen by Alice"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        HU_ASSERT_EQ(store_row(&f->mem, rows[i][0], rows[i][1], "s1"), HU_OK);
}

static size_t index_of_key(const hu_memory_entry_t *e, size_t n, const char *key) {
    for (size_t i = 0; i < n; i++)
        if (e[i].key && strcmp(e[i].key, key) == 0)
            return i;
    return n;
}

/* The boost-reporting recall returns exactly the vtable recall's rows, order
 * and scores, plus the boost the engine folded into each score. */
static void test_sqlite_recall_with_boosts_matches_recall_and_reports_boost(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fusion_fixture_t f;
    boost_fixture_open(&f, &alloc);
    const char *q = "zebra migration";
    hu_memory_entry_t *a = NULL, *b = NULL;
    size_t na = 0, nb = 0;
    double *boosts = NULL;
    HU_ASSERT_EQ(f.mem.vtable->recall(f.mem.ctx, &alloc, q, strlen(q), 10, NULL, 0, &a, &na),
                 HU_OK);
    HU_ASSERT_EQ(hu_sqlite_memory_recall_with_boosts(&f.mem, &alloc, q, strlen(q), 10, NULL, 0, &b,
                                                     &nb, &boosts),
                 HU_OK);
    HU_ASSERT_EQ(nb, na);
    HU_ASSERT_NOT_NULL(boosts);
    for (size_t i = 0; i < na; i++) {
        HU_ASSERT_STR_EQ(b[i].key, a[i].key);
        HU_ASSERT_FLOAT_EQ(b[i].score, a[i].score, 1e-12);
    }
    size_t ip = index_of_key(b, nb, "experience:plain");
    size_t ib = index_of_key(b, nb, "experience:boosted");
    HU_ASSERT_LT(ip, nb);
    HU_ASSERT_LT(ib, nb);
    HU_ASSERT_LT(ip, ib); /* equal bm25: recall keeps insertion order */
    HU_ASSERT_FLOAT_EQ(boosts[ip], 0.0, 1e-12);
    HU_ASSERT_FLOAT_EQ(boosts[ib], 0.17, 1e-9);
    /* raw bm25 = score - boost is equal for the two rows */
    HU_ASSERT_FLOAT_EQ(b[ib].score - boosts[ib], b[ip].score - boosts[ip], 1e-9);
    for (size_t i = 0; i < na; i++) {
        hu_memory_entry_free_fields(&alloc, &a[i]);
        hu_memory_entry_free_fields(&alloc, &b[i]);
    }
    alloc.free(alloc.ctx, a, na * sizeof(*a));
    alloc.free(alloc.ctx, b, nb * sizeof(*b));
    alloc.free(alloc.ctx, boosts, nb * sizeof(double));
    HU_ASSERT_EQ(hu_sqlite_memory_recall_with_boosts(NULL, &alloc, q, strlen(q), 10, NULL, 0, &b,
                                                     &nb, &boosts),
                 HU_ERR_NOT_SUPPORTED);
    fusion_fixture_close(&f, &alloc);
}

/* Score mode, alpha=0 (lexical only): the boosted row outranks its
 * equal-bm25 twin that recall lists first, with fused score 1.0. Under the
 * boost-as-penalty conversion it ranked below it. RRF keeps recall order. */
static void test_plain_hybrid_score_fusion_graph_boost_raises_boosted_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fusion_fixture_t f;
    boost_fixture_open(&f, &alloc);
    char keys[4][40];
    double top = 0;

    unsetenv("HU_HYBRID_FUSION");
    size_t n = fusion_run(&f, &alloc, keys, &top);
    HU_ASSERT_LT(key_pos(keys, n, "experience:plain"), key_pos(keys, n, "experience:boosted"));

    setenv("HU_HYBRID_FUSION", "score", 1);
    setenv("HU_HYBRID_FUSION_ALPHA", "0", 1);
    n = fusion_run(&f, &alloc, keys, &top);
    HU_ASSERT_STR_EQ(keys[0], "experience:boosted");
    HU_ASSERT_FLOAT_EQ(top, 1.0, 1e-6);
    HU_ASSERT_LT(key_pos(keys, n, "experience:boosted"), key_pos(keys, n, "experience:plain"));
    unsetenv("HU_HYBRID_FUSION");
    unsetenv("HU_HYBRID_FUSION_ALPHA");
    fusion_fixture_close(&f, &alloc);
}

/* The graph context row carries a constant 0.9, not a BM25 value. In score
 * mode it is tied with the best lexical hit (normalised 1.0) instead of being
 * min-max'd on the BM25 scale; as keyword rank 1 it then wins the RRF
 * tie-break at alpha=0. */
static void test_plain_hybrid_score_fusion_graph_row_ties_best_lexical_hit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    fusion_fixture_t f;
    fusion_fixture_open(&f, &alloc);
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t z = 0, m = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "zebra", 5, HU_ENTITY_TOPIC, NULL, &z), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "", 0, "migration", 9, HU_ENTITY_TOPIC, NULL, &m),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(g, "", 0, z, m, HU_REL_KNOWS, 1.0f, NULL, 0), HU_OK);

    setenv("HU_HYBRID_FUSION", "score", 1);
    setenv("HU_HYBRID_FUSION_ALPHA", "0", 1);
    hu_retrieval_options_t opts = {0};
    opts.limit = 4;
    hu_retrieval_result_t res = {0};
    const char *q = "zebra migration";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &f.mem, &f.emb, &f.vs, g, q, strlen(q), &opts, &res),
                 HU_OK);
    unsetenv("HU_HYBRID_FUSION");
    unsetenv("HU_HYBRID_FUSION_ALPHA");
    HU_ASSERT_GT(res.count, 1u);
    HU_ASSERT_STR_EQ(res.entries[0].key, "graph");
    HU_ASSERT_FLOAT_EQ(res.scores[0], 1.0, 1e-6);
    HU_ASSERT_STR_EQ(res.entries[1].key, "experience:short");
    HU_ASSERT_FLOAT_EQ(res.scores[1], 1.0, 1e-6);
    hu_retrieval_result_free(&alloc, &res);
    hu_graph_close(g, &alloc);
    fusion_fixture_close(&f, &alloc);
}

/* ---- no-vector keyword + graph merge (hu_hybrid_retrieve, !has_vector) ----
 * The configuration of any caller that passes a graph but no embedder/vector
 * store. The merge used to memcpy the entries out of both legs and then
 * hu_retrieval_result_free() them, which frees each entry's key/content --
 * so every returned string was already freed, and freeing the result freed
 * them again. It also ignored `limit`. */

static void no_vector_graph_open(hu_allocator_t *alloc, hu_memory_t *mem, hu_graph_t **g) {
    *mem = hu_sqlite_memory_create(alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem->vtable);
    HU_ASSERT_EQ(store_row(mem, "experience:short", "zebra migration", "s1"), HU_OK);
    HU_ASSERT_EQ(store_row(mem, "experience:long",
                           "zebra migration notes from a long field season on the plains", "s1"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(mem, "insight:zebra", "a zebra stood by the migration route", "s1"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(mem, "insight:coffee", "cold morning coffee on the porch", "s1"), HU_OK);
    *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(alloc, "x", 1, g), HU_OK);
    int64_t z = 0, m = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(*g, "", 0, "zebra", 5, HU_ENTITY_TOPIC, NULL, &z), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(*g, "", 0, "migration", 9, HU_ENTITY_TOPIC, NULL, &m),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(*g, "", 0, z, m, HU_REL_KNOWS, 1.0f, NULL, 0), HU_OK);
}

static void no_vector_graph_close(hu_allocator_t *alloc, hu_memory_t *mem, hu_graph_t *g) {
    hu_graph_close(g, alloc);
    mem->vtable->deinit(mem->ctx);
}

/* Every returned entry's key and content must be live memory. Pre-fix this
 * is a heap-use-after-free under ASan on the first strlen(). */
static void test_no_vector_graph_merge_entries_are_readable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    hu_graph_t *g = NULL;
    no_vector_graph_open(&alloc, &mem, &g);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t res = {0};
    const char *q = "zebra migration";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, g, q, strlen(q), &opts, &res), HU_OK);
    /* graph row + at least two keyword hits: the merge branch ran. */
    HU_ASSERT_GT(res.count, 2u);
    HU_ASSERT_NOT_NULL(res.scores);
    for (size_t i = 0; i < res.count; i++) {
        const hu_memory_entry_t *e = &res.entries[i];
        HU_ASSERT_NOT_NULL(e->key);
        HU_ASSERT_NOT_NULL(e->content);
        HU_ASSERT_EQ(strlen(e->key), e->key_len);
        HU_ASSERT_EQ(strlen(e->content), e->content_len);
    }
    HU_ASSERT_TRUE(result_has_key(&res, "graph"));
    HU_ASSERT_TRUE(result_has_key(&res, "experience:short"));
    HU_ASSERT_TRUE(result_has_content_word(&res, "zebra"));

    hu_retrieval_result_free(&alloc, &res);
    no_vector_graph_close(&alloc, &mem, g);
}

/* The merge is capped at opts.limit, and the graph row survives the cap: it
 * leads, matching the vector path, which puts graph first in the keyword RRF
 * list "so it gets rank 1". Keyword hits follow in their own order. */
static void test_no_vector_graph_merge_respects_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    hu_graph_t *g = NULL;
    no_vector_graph_open(&alloc, &mem, &g);
    const char *q = "zebra migration";

    /* Uncapped reference: how many rows the two legs produce together. */
    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t all = {0};
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, g, q, strlen(q), &opts, &all), HU_OK);
    HU_ASSERT_GT(all.count, 2u); /* > the limit below, so the cap must bite */

    opts.limit = 2;
    hu_retrieval_result_t res = {0};
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, g, q, strlen(q), &opts, &res), HU_OK);
    HU_ASSERT_EQ(res.count, 2u);
    HU_ASSERT_STR_EQ(res.entries[0].key, "graph");
    HU_ASSERT_STR_EQ(res.entries[1].key, all.entries[1].key); /* best keyword hit */

    opts.limit = 1;
    hu_retrieval_result_t one = {0};
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, g, q, strlen(q), &opts, &one), HU_OK);
    HU_ASSERT_EQ(one.count, 1u);
    HU_ASSERT_STR_EQ(one.entries[0].key, "graph");

    hu_retrieval_result_free(&alloc, &all);
    hu_retrieval_result_free(&alloc, &res);
    hu_retrieval_result_free(&alloc, &one);
    no_vector_graph_close(&alloc, &mem, g);
}

/* Every byte the merge allocates -- including the keyword entries the cap
 * drops -- is released by hu_retrieval_result_free on the result. Uses a
 * tracking allocator for the retrieve call only (the backend and graph keep
 * the system allocator), so LSan-less platforms still check the leak. */
static void test_no_vector_graph_merge_frees_everything(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_memory_t mem;
    hu_graph_t *g = NULL;
    no_vector_graph_open(&sys, &mem, &g);
    const char *q = "zebra migration";

    const size_t limits[] = {1, 2, 10};
    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++) {
        hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
        HU_ASSERT_NOT_NULL(ta);
        hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
        hu_retrieval_options_t opts = {0};
        opts.limit = limits[li];
        hu_retrieval_result_t res = {0};
        HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, g, q, strlen(q), &opts, &res),
                     HU_OK);
        HU_ASSERT_TRUE(res.count > 0 && res.count <= limits[li]);
        HU_ASSERT_TRUE(result_has_key(&res, "graph"));
        HU_ASSERT_GT(hu_tracking_allocator_total_allocated(ta), 0u);
        hu_retrieval_result_free(&alloc, &res);
        HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0u);
        hu_tracking_allocator_destroy(ta);
    }
    no_vector_graph_close(&sys, &mem, g);
}

void run_hybrid_reconstructive_tests(void) {
    HU_TEST_SUITE("hybrid_reconstructive");
    HU_RUN_TEST(test_sqlite_recall_with_boosts_matches_recall_and_reports_boost);
    HU_RUN_TEST(test_plain_hybrid_score_fusion_graph_boost_raises_boosted_row);
    HU_RUN_TEST(test_plain_hybrid_score_fusion_graph_row_ties_best_lexical_hit);
    HU_RUN_TEST(test_no_vector_graph_merge_entries_are_readable);
    HU_RUN_TEST(test_no_vector_graph_merge_respects_limit);
    HU_RUN_TEST(test_no_vector_graph_merge_frees_everything);
    HU_RUN_TEST(test_plain_hybrid_fusion_gate_default_is_rrf_order);
    HU_RUN_TEST(test_plain_hybrid_score_fusion_alpha_extremes_reorder_the_merge);
    HU_RUN_TEST(test_plain_hybrid_without_reranking_keeps_semantic_only_hit);
    HU_RUN_TEST(test_plain_hybrid_keyword_leg_ranks_by_backend_recall_not_word_fraction);
    HU_RUN_TEST(test_scene_select_prefers_two_hit_session_over_one_hit);
    HU_RUN_TEST(test_temporal_cue_prefers_newer_same_prefix_row);
    HU_RUN_TEST(test_sufficiency_fallback_returns_plain_result_for_one_scene);
    HU_RUN_TEST(test_fallback_path_entries_carry_memories_key_not_content);
    HU_RUN_TEST(test_plain_hybrid_entries_carry_memories_key_not_content);
    HU_RUN_TEST(test_cli_hybrid_line_prints_key_then_content);
    HU_RUN_TEST(test_ablate_no_scene_admits_low_scoring_session);
    HU_RUN_TEST(test_ablate_no_neighbors_drops_session_adjacent_rows);
    HU_RUN_TEST(test_ablate_no_rerank_starves_sufficiency_floor);
    HU_RUN_TEST(test_ablate_no_temporal_keeps_superseded_row);
    HU_RUN_TEST(test_ablate_force_sufficient_returns_reconstruction);
    HU_RUN_TEST(test_ablate_scene_coverage_first_admits_second_session);
}
#else
void run_hybrid_reconstructive_tests(void) {
    (void)0;
}
#endif
