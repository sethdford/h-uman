/* Task 3 of docs/superpowers/plans/2026-09-01-sota-e2e.md — one superseding
 * ingest path for facts. A changed fact (lives_in KoP -> lives_in St Pete)
 * must CLOSE the prior edge so the grounding read sees one current truth.
 * The two live writers used the legacy non-superseding upsert; three of nine
 * human detections at n=40 were stale event-state facts. */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/graph_ingest.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static hu_graph_t *open_tmp_graph(hu_allocator_t *alloc, char *path, size_t cap) {
    snprintf(path, cap, "/tmp/hu_graph_ingest_%d.db", (int)getpid());
    unlink(path);
    hu_graph_t *g = NULL;
    if (hu_graph_open(alloc, path, strlen(path), &g) != HU_OK)
        return NULL;
    return g;
}

/* hu_graph_find_entity hands the caller heap-owned name/metadata_json (see
 * tests/test_graph.c); release both so LeakSanitizer stays clean. */
static void free_entity_strings(hu_allocator_t *alloc, hu_graph_entity_t *ent) {
    if (ent->name)
        alloc->free(alloc->ctx, ent->name, ent->name_len + 1);
    if (ent->metadata_json)
        alloc->free(alloc->ctx, ent->metadata_json, strlen(ent->metadata_json) + 1);
}

/* Count open LIVES_IN edges for "self" true at time `at`; report whether the
 * single hit points at the entity named `expect_target`. The window read does
 * not resolve names, so compare ids via hu_graph_find_entity. */
static size_t open_lives_in_at(hu_graph_t *g, hu_allocator_t *alloc, int64_t at,
                               const char *expect_target, bool *matches_out) {
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    if (matches_out)
        *matches_out = false;
    if (hu_graph_relations_in_window(g, alloc, "self", 4, at, at, 32, &rels, &n) != HU_OK)
        return (size_t)-1;
    hu_graph_entity_t want;
    memset(&want, 0, sizeof(want));
    bool have_want = expect_target && hu_graph_find_entity(g, "self", 4, expect_target,
                                                           strlen(expect_target), &want) == HU_OK;
    size_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        if (rels[i].type != HU_REL_LIVES_IN)
            continue;
        hits++;
        if (have_want && matches_out && rels[i].target_id == want.id)
            *matches_out = true;
    }
    hu_graph_relations_free(alloc, rels, n);
    free_entity_strings(alloc, &want);
    return hits;
}

static void test_changed_fact_supersedes_prior_edge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "lives_in", "king of prussia", 0.9f,
                                      100, "test:1"),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_ingest_fact(g, "self", 4, "user", "lives_in", "st pete", 0.9f, 200, "test:2"),
        HU_OK);
    bool is_new = false, is_old = false;
    /* Now: exactly one open LIVES_IN, and it is the newer place. */
    HU_ASSERT_EQ((long)open_lives_in_at(g, &alloc, 250, "st pete", &is_new), 1L);
    HU_ASSERT_TRUE(is_new);
    /* Then: at t=150 the old place was the truth. */
    HU_ASSERT_EQ((long)open_lives_in_at(g, &alloc, 150, "king of prussia", &is_old), 1L);
    HU_ASSERT_TRUE(is_old);
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_same_fact_twice_is_one_edge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "works_at", "acme", 0.8f, 100, "t"),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "works_at", "acme", 0.8f, 300, "t"),
                 HU_OK);
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_relations_in_window(g, &alloc, "self", 4, 400, 400, 32, &rels, &n),
                 HU_OK);
    size_t works = 0;
    for (size_t i = 0; i < n; i++)
        if (rels[i].type == HU_REL_WORKS_AT)
            works++;
    HU_ASSERT_EQ((long)works, 1L);
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_unknown_predicate_maps_to_related_to_not_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "visiting", "tampa", 0.7f, 100, "t"),
                 HU_OK);
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_relations_in_window(g, &alloc, "self", 4, 150, 150, 32, &rels, &n),
                 HU_OK);
    HU_ASSERT_EQ((long)n, 1L);
    HU_ASSERT_EQ((int)rels[0].type, (int)HU_REL_RELATED_TO);
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_rejects_empty_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "lives_in", "", 0.9f, 1, "t"),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_graph_ingest_fact(NULL, "self", 4, "user", "lives_in", "x", 0.9f, 1, "t"),
                 HU_ERR_INVALID_ARGUMENT);
    hu_graph_close(g, &alloc);
    unlink(path);
}

/* Both live callers hand `now` in SECONDS (daemon deep-extract: time(NULL);
 * chat.db backfill: message dates). The graph speaks milliseconds; the
 * ingest boundary must convert, or every window read is off by 1000x. */
static void test_seconds_now_is_stored_as_milliseconds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "works_at", "Acme", 0.9f,
                                      1788289998LL /* seconds */, "t"),
                 HU_OK);
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, &alloc, "self", 4, 8, &rels, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_EQ(rels[0].event_start, 1788289998000LL);
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}


/* ── Name hygiene at the write path (2026-09-23) ──────────────────────
 *
 * Measured motivation: the grounding read ranks by mention_count with no type
 * filter, and on the live graph the top candidates were "user" (527 mentions,
 * 7 rows), two raw phone numbers, "work", "you", "home" — ahead of "Utah",
 * "Zillow", "St Petersburg FL". These tests pin that the junk never enters,
 * that REAL facts about the persona survive (resolved, not dropped), and that
 * matching is exact rather than substring. */

static bool entity_exists(hu_graph_t *g, hu_allocator_t *alloc, const char *cid,
                          const char *name) {
    hu_graph_entity_t ent;
    memset(&ent, 0, sizeof(ent));
    bool found = hu_graph_find_entity(g, cid, strlen(cid), name, strlen(name), &ent) == HU_OK;
    if (found)
        free_entity_strings(alloc, &ent);
    return found;
}

static void test_self_placeholder_subject_resolves_to_the_contact_node(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    const char *cid = "+15551234567";

    /* A real fact about the persona, written with the extractor's placeholder
     * subject. It must be KEPT — dropping it would lose information. */
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, cid, strlen(cid), "user", "lives_in", "St Petersburg FL",
                                      0.9f, 1778454858LL, "test"),
                 HU_OK);

    /* The junk node must not exist... */
    HU_ASSERT_TRUE(!entity_exists(g, &alloc, cid, "user"));
    /* ...and the fact must have attached to the contact's own PERSON node. */
    HU_ASSERT_TRUE(entity_exists(g, &alloc, cid, cid));
    /* The object is a real name and is kept as-is. */
    HU_ASSERT_TRUE(entity_exists(g, &alloc, cid, "St Petersburg FL"));

    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_nonreferential_subject_or_object_is_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    const char *cid = "+15551234567";

    /* ("user", "likes", "it") grounds nothing. */
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, cid, strlen(cid), "user", "likes", "it", 0.9f,
                                      1778454858LL, "test"),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(!entity_exists(g, &alloc, cid, "it"));

    HU_ASSERT_EQ(hu_graph_ingest_fact(g, cid, strlen(cid), "you", "lives_in", "Utah", 0.9f,
                                      1778454858LL, "test"),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(!entity_exists(g, &alloc, cid, "you"));
    /* The real name on the other end must not be created by a rejected fact. */
    HU_ASSERT_TRUE(!entity_exists(g, &alloc, cid, "Utah"));

    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_real_names_are_untouched(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    const char *cid = "+15551234567";
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, cid, strlen(cid), "Annette", "works_at", "Vanguard", 0.9f,
                                      1778454858LL, "test"),
                 HU_OK);
    HU_ASSERT_TRUE(entity_exists(g, &alloc, cid, "Annette"));
    HU_ASSERT_TRUE(entity_exists(g, &alloc, cid, "Vanguard"));
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_matching_is_exact_not_substring(void) {
    /* substring-classifier-pitfalls: a short, high-frequency ban list is
     * exactly where substring matching eats real names. "Ituri" contains
     * "it"; "Userman" contains "user"; "Theyer" contains "they". */
    HU_ASSERT_TRUE(!hu_graph_name_is_nonreferential("Ituri", 5));
    HU_ASSERT_TRUE(!hu_graph_name_is_nonreferential("Theyer", 6));
    HU_ASSERT_TRUE(!hu_graph_name_is_self_placeholder("Userman", 7));
    HU_ASSERT_TRUE(!hu_graph_name_is_self_placeholder("Mike", 4));
    /* ...but the bare words, in any case and with stray padding, do match. */
    HU_ASSERT_TRUE(hu_graph_name_is_nonreferential("it", 2));
    HU_ASSERT_TRUE(hu_graph_name_is_nonreferential("IT", 2));
    HU_ASSERT_TRUE(hu_graph_name_is_self_placeholder("  User ", 7));
    HU_ASSERT_TRUE(hu_graph_name_is_self_placeholder("me", 2));
    /* Empty is neither. */
    HU_ASSERT_TRUE(!hu_graph_name_is_self_placeholder("", 0));
    HU_ASSERT_TRUE(!hu_graph_name_is_nonreferential(NULL, 0));
}

void run_graph_ingest_tests(void) {
    HU_TEST_SUITE("graph_ingest");
    HU_RUN_TEST(test_self_placeholder_subject_resolves_to_the_contact_node);
    HU_RUN_TEST(test_nonreferential_subject_or_object_is_rejected);
    HU_RUN_TEST(test_real_names_are_untouched);
    HU_RUN_TEST(test_matching_is_exact_not_substring);
    HU_RUN_TEST(test_seconds_now_is_stored_as_milliseconds);
    HU_RUN_TEST(test_changed_fact_supersedes_prior_edge);
    HU_RUN_TEST(test_same_fact_twice_is_one_edge);
    HU_RUN_TEST(test_unknown_predicate_maps_to_related_to_not_dropped);
    HU_RUN_TEST(test_rejects_empty_fields);
}
#else
void run_graph_ingest_tests(void) {
    (void)0;
}
#endif
