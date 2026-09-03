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

void run_hybrid_reconstructive_tests(void) {
    HU_TEST_SUITE("hybrid_reconstructive");
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
