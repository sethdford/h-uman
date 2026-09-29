#include "human/agent.h"
#include "human/agent/autodream.h"
#include "human/agent/graph_grounding.h"
#include "human/agent/model_router.h"
#include "human/agent/scheduler.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Pure retrieval-scoring predicates (no DB) ──────────────────────────── */

/* Word-boundary matching pins the substring-classifier-pitfalls contract:
 * an entity named "formal" must NOT be selected by the message "that was
 * so informal" — the exact opposite-intent overlap class that rule
 * documents. */
static void test_ground_match_count_respects_word_boundaries(void) {
    const char *msg1 = "that was so informal lol";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg1, strlen(msg1), "formal", 6), 0);
    const char *msg2 = "lukewarm at best";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg2, strlen(msg2), "warm", 4), 0);
    const char *msg3 = "such warm friend energy";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg3, strlen(msg3), "warm", 4), 1);
    /* Case-insensitive across boundaries, multi-word entity names count
     * per-word: "Alice" matches one of the two words of "Alice Friend". */
    const char *msg4 = "did ALICE text you back";
    HU_ASSERT_EQ((int)hu_graph_ground_entity_match_count(msg4, strlen(msg4), "Alice Friend", 12),
                 1);
}

static void test_ground_name_word_count_skips_stopwords_and_short_words(void) {
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count("the marina", 10), 1);
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count("Bo", 2), 0);
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count(NULL, 0), 0);
    HU_ASSERT_EQ((int)hu_graph_ground_name_word_count("Alice Friend", 12), 2);
}

static void test_ground_score_zero_without_match_and_coverage_dominates(void) {
    /* No lexical overlap -> 0.0: empty injection is the VALID outcome for an
     * irrelevant message (never fall back to generic filler). */
    HU_ASSERT_TRUE(hu_graph_ground_score(0, 2, 100, 1000, 2000) == 0.0);
    HU_ASSERT_TRUE(hu_graph_ground_score(1, 0, 100, 1000, 2000) == 0.0);
    /* Full-name coverage outranks half coverage at identical stats. */
    double full = hu_graph_ground_score(2, 2, 5, 1000, 2000);
    double half = hu_graph_ground_score(1, 2, 5, 1000, 2000);
    HU_ASSERT_TRUE(full > half);
    /* Mention + recency boosts are bounded: a half-coverage entity with
     * maxed boosts cannot outrank a full-coverage entity with the same
     * boosts (coverage is the dominant term). */
    double half_boosted = hu_graph_ground_score(1, 2, 1000000, 2000, 2000);
    double full_plain = hu_graph_ground_score(2, 2, 1000000, 2000, 2000);
    HU_ASSERT_TRUE(full_plain > half_boosted);
}

static void test_ground_fingerprint_varies_with_content(void) {
    const char *a = "- sailboat (topic)";
    const char *b = "- guitar (topic)";
    HU_ASSERT_TRUE(hu_graph_ground_fingerprint(a, strlen(a)) !=
                   hu_graph_ground_fingerprint(b, strlen(b)));
    HU_ASSERT_EQ((int)hu_graph_ground_fingerprint(NULL, 0), 0);
    HU_ASSERT_EQ((int)hu_graph_ground_fingerprint(a, 0), 0);
}

/* ── Query-conditioned composition (SQLite graph store) ─────────────────── */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void seed_run(sqlite3 *db, const char *sql) {
    char *emsg = NULL;
    (void)sqlite3_exec(db, sql, NULL, NULL, &emsg);
    if (emsg)
        sqlite3_free(emsg);
}

/* Two DISJOINT entity clusters for "alice" so different conversations
 * compose different context: (sailboat -> marina) and (guitar -> teacher).
 * Plus a "formal" entity to pin the word-boundary contract at compose
 * level, and a "bob"-scoped entity that must never leak into alice's
 * context. */
static void seed_alice_graph(hu_graph_t *graph) {
    int64_t boat = 0, marina = 0, guitar = 0, teacher = 0, formal = 0, bobs = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "sailboat", 8, HU_ENTITY_TOPIC, NULL, &boat),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "marina", 6, HU_ENTITY_PLACE, NULL, &marina),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "guitar", 6, HU_ENTITY_TOPIC, NULL, &guitar),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "teacher", 7, HU_ENTITY_PERSON, NULL, &teacher),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "alice", 5, "formal", 6, HU_ENTITY_TOPIC, NULL, &formal),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(graph, "bob", 3, "sailboat", 8, HU_ENTITY_TOPIC, NULL, &bobs),
        HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(graph, "alice", 5, boat, marina, HU_REL_RELATED_TO, 1.0f,
                                          "docked at slip 14 since spring", 30),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(graph, "alice", 5, guitar, teacher, HU_REL_RELATED_TO,
                                          1.0f, "fingerstyle lessons every tuesday", 33),
                 HU_OK);
}

typedef struct gg_fixture {
    hu_allocator_t alloc;
    hu_graph_t *graph;
    hu_w7_facade_t *facade;
    hu_memory_loader_t loader;
} gg_fixture_t;

static void gg_fixture_open(gg_fixture_t *fx) {
    fx->alloc = hu_system_allocator();
    fx->graph = NULL;
    HU_ASSERT_EQ(hu_graph_open(&fx->alloc, ":memory:", strlen(":memory:"), &fx->graph), HU_OK);
    seed_alice_graph(fx->graph);
    fx->facade = NULL;
    HU_ASSERT_EQ(hu_w7_facade_open(fx->graph, &fx->alloc, &fx->facade), HU_OK);
    hu_memory_loader_init(&fx->loader, &fx->alloc, NULL, NULL, 10, 4000);
    hu_memory_loader_set_facade(&fx->loader, fx->facade);
}

static void gg_fixture_close(gg_fixture_t *fx) {
    hu_w7_facade_close(fx->facade, &fx->alloc);
    hu_graph_close(fx->graph, &fx->alloc);
}

/* Relevant entity in the message -> that entity's graph content (its name,
 * its 1-hop relation, and the relation's context text) is selected. */
static void test_compose_selects_relevant_entity_content(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(matched >= 1);
    HU_ASSERT_TRUE(strstr(out, "sailboat") != NULL);
    HU_ASSERT_TRUE(strstr(out, "docked at slip 14") != NULL); /* relation context text */
    HU_ASSERT_TRUE(strstr(out, "guitar") == NULL);            /* disjoint cluster stays out */
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* 2026-09-01: after the backfill (571 entities) a once-mentioned entity sat
 * far below the 64-entity popularity cap and could never seed grounding even
 * when the message named it verbatim. Candidates must be message-driven too. */
static void test_compose_finds_low_mention_entity_beyond_candidate_cap(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    /* 80 filler entities, each mentioned 3x so they outrank a single mention. */
    for (int i = 0; i < 80; i++) {
        char name[32];
        snprintf(name, sizeof(name), "filler%02d", i);
        int64_t id = 0;
        for (int k = 0; k < 3; k++)
            hu_graph_upsert_entity(fx.graph, "alice", 5, name, strlen(name), HU_ENTITY_TOPIC, NULL,
                                   &id);
    }
    int64_t vg = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx.graph, "alice", 5, "vanguard", 8, HU_ENTITY_ORGANIZATION,
                                        NULL, &vg),
                 HU_OK);
    /* Below the popularity cap: list_entities(64) must NOT contain it ... */
    hu_graph_entity_t *top = NULL;
    size_t top_n = 0;
    HU_ASSERT_EQ(hu_graph_list_entities(fx.graph, &fx.alloc, "alice", 5, 64, &top, &top_n), HU_OK);
    bool in_top = false;
    for (size_t i = 0; i < top_n; i++)
        if (top[i].id == vg)
            in_top = true;
    hu_graph_entities_free(&fx.alloc, top, top_n);
    HU_ASSERT_FALSE(in_top);
    /* ... yet the composer still seeds it from the message. */
    const char *msg = "hows vanguard treating you these days";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_TRUE(matched >= 1);
    if (out)
        fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Irrelevant message -> EMPTY injection. Empty is VALID and better than
 * the pre-2026-07 behavior (same generic community summaries every turn). */
static void test_compose_irrelevant_message_returns_empty(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "wanna grab tacos tonight";
    char *out = (char *)0x1;
    size_t out_len = 99, matched = 99;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    HU_ASSERT_EQ((int)matched, 0);
    gg_fixture_close(&fx);
}

/* Compose-level pin of the word-boundary contract: entity "formal" exists,
 * but the message "that was so informal" must not seed it. */
static void test_compose_word_boundary_prevents_false_seed(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "that was so informal";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)matched, 0);
    gg_fixture_close(&fx);
}

/* max_chars is a hard cap on the composed block (the block additionally
 * participates in the HU_PROMPT_TRIM graph span downstream). */
static void test_compose_respects_budget_cap(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 48, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(out_len <= 48);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* DONE-(b) synthetic multi-conversation demo: two different incoming
 * messages against the SAME contact graph compose DIFFERENT content with
 * DIFFERENT relevance fingerprints — the measurable inverse of the old
 * failure signature (274 shadow events collapsing to 5 distinct sizes of
 * identical generic summaries). */
static void test_compose_varies_with_conversation(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg_a = "hows the sailboat coming along";
    const char *msg_b = "hows guitar practice going";
    char *out_a = NULL, *out_b = NULL;
    size_t len_a = 0, len_b = 0, matched_a = 0, matched_b = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg_a, strlen(msg_a), 0, &out_a,
                                         &len_a, &matched_a),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg_b, strlen(msg_b), 0, &out_b,
                                         &len_b, &matched_b),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out_a);
    HU_ASSERT_NOT_NULL(out_b);
    HU_ASSERT_TRUE(matched_a >= 1);
    HU_ASSERT_TRUE(matched_b >= 1);
    HU_ASSERT_TRUE(strstr(out_a, "sailboat") != NULL);
    HU_ASSERT_TRUE(strstr(out_a, "guitar") == NULL);
    HU_ASSERT_TRUE(strstr(out_b, "guitar") != NULL);
    HU_ASSERT_TRUE(strstr(out_b, "sailboat") == NULL);
    HU_ASSERT_TRUE(strcmp(out_a, out_b) != 0);
    HU_ASSERT_TRUE(hu_graph_ground_fingerprint(out_a, len_a) !=
                   hu_graph_ground_fingerprint(out_b, len_b));
    fx.alloc.free(fx.alloc.ctx, out_a, len_a + 1);
    fx.alloc.free(fx.alloc.ctx, out_b, len_b + 1);
    gg_fixture_close(&fx);
}

/* Contact scoping: bob's graph knows "sailboat" too, but bob has no
 * relations and alice's rows must not leak into bob's composition. */
static void test_compose_scopes_to_contact(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "bob", 3, msg, strlen(msg), 0, &out, &out_len,
                                         &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out); /* bob's own sailboat entity matches... */
    HU_ASSERT_TRUE(strstr(out, "docked at slip 14") == NULL); /* ...alice's relation doesn't */
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Contact-anchored fallback (2026-09-27): casual texts rarely NAME an entity,
 * so lexical seeding returned nothing for 40/40 real moments. With the
 * fallback flag, a lexical miss seeds from the contact's OWN top entities. */
static void test_compose_ex_contact_fallback_fills_lexical_miss(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "wanna grab tacos tonight"; /* names nothing in the graph */
    char *out = NULL;
    size_t out_len = 0, matched = 99;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "alice", 5, msg, strlen(msg), 0,
                                            HU_GG_CONTACT_FALLBACK, &out, &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_EQ((int)matched, 0); /* no LEXICAL match: the shadow log must see 0 */
    HU_ASSERT_TRUE(strstr(out, "slip 14") != NULL || strstr(out, "fingerstyle") != NULL);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* flags=0 keeps the pre-existing contract byte-for-byte: a lexical miss is empty. */
static void test_compose_ex_without_flag_keeps_empty_on_miss(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "wanna grab tacos tonight";
    char *out = (char *)0x1;
    size_t out_len = 99, matched = 99;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "alice", 5, msg, strlen(msg), 0, 0, &out,
                                            &out_len, &matched),
                 HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    gg_fixture_close(&fx);
}

/* When the message DOES name an entity, the flag changes nothing: identical
 * bytes and matched count to plain compose. */
static void test_compose_ex_fallback_inert_on_lexical_hit(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "hows the sailboat coming along";
    char *a = NULL, *b = NULL;
    size_t alen = 0, blen = 0, am = 0, bm = 0;
    HU_ASSERT_EQ(
        hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &a, &alen, &am),
        HU_OK);
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "alice", 5, msg, strlen(msg), 0,
                                            HU_GG_CONTACT_FALLBACK, &b, &blen, &bm),
                 HU_OK);
    HU_ASSERT_NOT_NULL(a);
    HU_ASSERT_NOT_NULL(b);
    HU_ASSERT_EQ((int)alen, (int)blen);
    HU_ASSERT_TRUE(memcmp(a, b, alen) == 0);
    HU_ASSERT_EQ((int)am, (int)bm);
    HU_ASSERT_TRUE(am > 0);
    fx.alloc.free(fx.alloc.ctx, a, alen + 1);
    fx.alloc.free(fx.alloc.ctx, b, blen + 1);
    gg_fixture_close(&fx);
}

/* The fallback never crosses contacts: bob gets only bob's entities (never
 * alice's relation text), and a contact with no entities gets nothing. */
static void test_compose_ex_fallback_scoped_to_contact(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "wanna grab tacos tonight";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "bob", 3, msg, strlen(msg), 0,
                                            HU_GG_CONTACT_FALLBACK, &out, &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out); /* bob has one entity of his own */
    HU_ASSERT_TRUE(strstr(out, "sailboat") != NULL);
    HU_ASSERT_TRUE(strstr(out, "slip 14") == NULL);
    HU_ASSERT_TRUE(strstr(out, "fingerstyle") == NULL);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);

    char *none = (char *)0x1;
    size_t none_len = 99;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "carol", 5, msg, strlen(msg), 0,
                                            HU_GG_CONTACT_FALLBACK, &none, &none_len, NULL),
                 HU_OK);
    HU_ASSERT_TRUE(none == NULL);
    HU_ASSERT_EQ((int)none_len, 0);
    gg_fixture_close(&fx);
}

/* The fallback never volunteers an EMOTION entity, even the contact's most-
 * mentioned one: surfacing "grief" on every unrelated casual text is the
 * opposite of human. (A lexical hit on it, when the contact names it, is fine
 * and unaffected.) Critic finding on af1d94b31. */
static void test_compose_ex_fallback_skips_emotion_entities(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    for (int i = 0; i < 5; i++) /* 5 mentions: outranks every other alice entity */
        HU_ASSERT_EQ(hu_graph_upsert_entity(fx.graph, "alice", 5, "heartbreak", 10,
                                            HU_ENTITY_EMOTION, NULL, &id),
                     HU_OK);
    const char *msg = "wanna grab tacos tonight";
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "alice", 5, msg, strlen(msg), 0,
                                            HU_GG_CONTACT_FALLBACK, &out, &out_len, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out); /* other entities still seed */
    HU_ASSERT_TRUE(strstr(out, "heartbreak") == NULL);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);

    const char *named = "still thinking about the heartbreak";
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, named, strlen(named), 0, &out,
                                         &out_len, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out); /* the lexical path still grounds on it when named */
    HU_ASSERT_TRUE(strstr(out, "heartbreak") != NULL);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Owner ("self") facts are matched only when the message carries the entity's
 * FULL name: a topic phrase like "different direction" must not seed on a lone
 * shared word ("different"), while plain lexical matching would seed it. */
static void test_compose_ex_require_full_name_blocks_partial_hits(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx.graph, "self", 4, "different direction", 19,
                                        HU_ENTITY_TOPIC, NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "self", 4, "tampa bay", 9, HU_ENTITY_PLACE, NULL, &id),
        HU_OK);
    const char *partial = "i went a totally different way";
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "self", 4, partial, strlen(partial), 0, &out,
                                         &out_len, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out); /* plain lexical seeds on the lone word... */
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    out = (char *)0x1;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "self", 4, partial, strlen(partial), 0,
                                            HU_GG_REQUIRE_FULL_NAME, &out, &out_len, NULL),
                 HU_OK);
    HU_ASSERT_TRUE(out == NULL); /* ...full-name mode does not */

    const char *full = "hows the weather down in tampa bay";
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "self", 4, full, strlen(full), 0,
                                            HU_GG_REQUIRE_FULL_NAME, &out, &out_len, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "tampa bay") != NULL);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Placeholder predicate: pronoun-like subjects the extractors write ("user",
 * "you", "assistant") and the contact's own id / phone number are not
 * referents, so the fallback must never spend a seed on them. Measured
 * 2026-09-28: for 3 of 4 active contacts the fallback's #1 seed was one. */
static void test_ground_is_placeholder_name(void) {
    HU_ASSERT_TRUE(hu_graph_ground_is_placeholder_name("user", 4, "+15551234567", 12));
    HU_ASSERT_TRUE(hu_graph_ground_is_placeholder_name("User", 4, "+15551234567", 12));
    HU_ASSERT_TRUE(hu_graph_ground_is_placeholder_name("assistant", 9, "+15551234567", 12));
    HU_ASSERT_TRUE(hu_graph_ground_is_placeholder_name("you", 3, "+15551234567", 12));
    HU_ASSERT_TRUE(hu_graph_ground_is_placeholder_name("+15551234567", 12, "+15551234567", 12));
    HU_ASSERT_TRUE(hu_graph_ground_is_placeholder_name("+1 (801) 555-0100", 17, "alice", 5));
    HU_ASSERT_FALSE(hu_graph_ground_is_placeholder_name("Utah", 4, "+15551234567", 12));
    HU_ASSERT_FALSE(hu_graph_ground_is_placeholder_name("user research", 13, "alice", 5));
    HU_ASSERT_FALSE(hu_graph_ground_is_placeholder_name("youth soccer", 12, "alice", 5));
    HU_ASSERT_FALSE(hu_graph_ground_is_placeholder_name("sailboat", 8, "alice", 5));
}

/* The fallback skips placeholders even when they are the contact's most-
 * mentioned rows, and still seeds real entities. */
static void test_compose_ex_fallback_skips_placeholders(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    for (int i = 0; i < 6; i++) { /* outrank every real alice entity */
        HU_ASSERT_EQ(
            hu_graph_upsert_entity(fx.graph, "alice", 5, "user", 4, HU_ENTITY_PERSON, NULL, &id),
            HU_OK);
        HU_ASSERT_EQ(
            hu_graph_upsert_entity(fx.graph, "alice", 5, "alice", 5, HU_ENTITY_PERSON, NULL, &id),
            HU_OK);
    }
    const char *msg = "wanna grab tacos tonight";
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "alice", 5, msg, strlen(msg), 0,
                                            HU_GG_CONTACT_FALLBACK, &out, &out_len, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "- user") == NULL);
    HU_ASSERT_TRUE(strstr(out, "- alice") == NULL);
    HU_ASSERT_TRUE(strstr(out, "sailboat") != NULL || strstr(out, "guitar") != NULL);
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* The fallback honors the same output budget as lexical composition. */
static void test_compose_ex_fallback_respects_budget(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    const char *msg = "wanna grab tacos tonight";
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "alice", 5, msg, strlen(msg), 40,
                                            HU_GG_CONTACT_FALLBACK, &out, &out_len, NULL),
                 HU_OK);
    HU_ASSERT_TRUE(out_len <= 40);
    if (out)
        fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Caller contract for the fallback gate, exercised through the REAL live-path
 * loader hu_agent_load_graph_grounding with grounding ON and a lexical miss:
 *   fallback OFF    -> nothing (pre-existing behavior)
 *   fallback SHADOW -> nothing injected (logged only)
 *   fallback LIVE   -> injected on ANALYTICAL turns
 *   fallback LIVE   -> still dropped on casual turns (the 2026-05-29 measured
 *                      casual-register gate is NOT overridden). */
static size_t load_grounding_len(gg_fixture_t *fx, const char *fb_mode, int tier) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);
    agent->alloc = &fx->alloc;
    agent->memory_session_id = "alice";
    agent->memory_session_id_len = 5;
    agent->turn_tier = tier;
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    if (fb_mode)
        setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", fb_mode, 1);
    else
        unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    const char *msg = "wanna grab tacos tonight";
    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_agent_load_graph_grounding(agent, &fx->loader, msg, strlen(msg), &ctx, &ctx_len);
    if (ctx)
        fx->alloc.free(fx->alloc.ctx, ctx, ctx_len + 1);
    unsetenv("HU_GRAPH_GROUNDING");
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    free(agent);
    return ctx_len;
}

/* Self-facts gate through the real loader: the contact ("alice") has no
 * lexical hit and the contact fallback is OFF, but the message names one of
 * the OWNER's facts. OFF/SHADOW inject nothing; LIVE injects it under an
 * "About you:" label on analytical turns; casual turns still drop it. */
static char *load_self_facts(gg_fixture_t *fx, const char *mode, int tier, size_t *len_out) {
    int64_t id = 0;
    (void)hu_graph_upsert_entity(fx->graph, "self", 4, "tampa bay", 9, HU_ENTITY_PLACE, NULL, &id);
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);
    agent->alloc = &fx->alloc;
    agent->memory_session_id = "alice";
    agent->memory_session_id_len = 5;
    agent->turn_tier = tier;
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    if (mode)
        setenv("HU_GRAPH_GROUNDING_SELF_FACTS", mode, 1);
    else
        unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
    const char *msg = "hows the weather down in tampa bay";
    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_agent_load_graph_grounding(agent, &fx->loader, msg, strlen(msg), &ctx, &ctx_len);
    unsetenv("HU_GRAPH_GROUNDING");
    unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
    free(agent);
    *len_out = ctx_len;
    return ctx;
}

static void test_load_grounding_self_facts_gate(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    size_t len = 99;
    HU_ASSERT_TRUE(load_self_facts(&fx, NULL, (int)HU_TIER_ANALYTICAL, &len) == NULL);
    HU_ASSERT_TRUE(load_self_facts(&fx, "shadow", (int)HU_TIER_ANALYTICAL, &len) == NULL);
    HU_ASSERT_TRUE(load_self_facts(&fx, "live", (int)HU_TIER_REFLEXIVE, &len) == NULL);
    char *ctx = load_self_facts(&fx, "live", (int)HU_TIER_ANALYTICAL, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "About you:") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "tampa bay") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "slip 14") == NULL); /* alice's facts only via her own path */
    fx.alloc.free(fx.alloc.ctx, ctx, len + 1);
    gg_fixture_close(&fx);
}

static void test_self_facts_mode_defaults_off(void) {
    unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
    HU_ASSERT_EQ((int)hu_graph_grounding_self_facts_mode(), (int)HU_GATE_OFF);
    setenv("HU_GRAPH_GROUNDING_SELF_FACTS", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_self_facts_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_GRAPH_GROUNDING_SELF_FACTS", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_self_facts_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
}

static void test_load_grounding_contact_fallback_gate(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    HU_ASSERT_EQ((int)load_grounding_len(&fx, NULL, (int)HU_TIER_ANALYTICAL), 0);
    HU_ASSERT_EQ((int)load_grounding_len(&fx, "shadow", (int)HU_TIER_ANALYTICAL), 0);
    HU_ASSERT_TRUE(load_grounding_len(&fx, "live", (int)HU_TIER_ANALYTICAL) > 0);
    HU_ASSERT_EQ((int)load_grounding_len(&fx, "live", (int)HU_TIER_REFLEXIVE), 0);
    gg_fixture_close(&fx);
}

/* Fail-open: loader without a facade (no graph wired) -> empty, HU_OK. */
static void test_compose_no_graph_is_failopen(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_loader_t loader;
    hu_memory_loader_init(&loader, &alloc, NULL, NULL, 10, 4000);
    const char *msg = "hows the sailboat";
    char *out = (char *)0x1;
    size_t out_len = 99, matched = 99;
    HU_ASSERT_EQ(
        hu_graph_ground_compose(&loader, "alice", 5, msg, strlen(msg), 0, &out, &out_len, &matched),
        HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    HU_ASSERT_EQ((int)matched, 0);
}

/* ── State-first read path (2026-09-04) ──────────────────────────────────
 * The write side closes a superseded WORKS_AT / LIVES_IN row (event_end +
 * supersedes_id); the composer must render the CURRENT head with its
 * predecessor, and an old edge only as dated history — never two employers
 * as if both held (the event-state tell in the n=40 human gate). */
static void seed_user_job_chain(gg_fixture_t *fx) {
    int64_t user = 0, acme = 0, globex = 0, initech = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx->graph, "alice", 5, "user", 4, HU_ENTITY_PERSON, NULL, &user),
        HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "alice", 5, "Acme", 4, HU_ENTITY_ORGANIZATION,
                                        NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "alice", 5, "Globex", 6, HU_ENTITY_ORGANIZATION,
                                        NULL, &globex),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "alice", 5, "Initech", 7, HU_ENTITY_ORGANIZATION,
                                        NULL, &initech),
                 HU_OK);
    /* Acme (2024) -> Globex (2025) -> Initech (2026): each supersedes the prior. */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(fx->graph, "alice", 5, user, acme, HU_REL_WORKS_AT,
                                             1.0f, 1704067200000LL, 0, 1.0f, NULL, 0, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(fx->graph, "alice", 5, user, globex, HU_REL_WORKS_AT,
                                             1.0f, 1735689600000LL, 0, 1.0f, NULL, 0, NULL, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(fx->graph, "alice", 5, user, initech, HU_REL_WORKS_AT,
                                             1.0f, 1767225600000LL, 0, 1.0f, NULL, 0, NULL, 0),
                 HU_OK);
}

static void test_compose_renders_current_employer_with_predecessor(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_user_job_chain(&fx);
    const char *msg = "hows the new gig at initech";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "user works_at Initech (was: Globex)");
    HU_ASSERT_STR_NOT_CONTAINS(out, "works_at Globex");
    HU_ASSERT_STR_NOT_CONTAINS(out, "Acme");
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Seeding from the OLD employer walks only the closed edge: it must read as
 * dated history, not as a current job. */
static void test_compose_marks_superseded_employer_as_history(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_user_job_chain(&fx);
    const char *msg = "still talk to anyone from acme";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "user works_at Acme (until Jan 2025)");
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* Seeding from the person walks every edge of the chain; only the head
 * renders, and the cap can no longer be filled by history. */
static void test_compose_from_person_seed_collapses_chain_to_head(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_user_job_chain(&fx);
    const char *msg = "what is the user up to";
    char *out = NULL;
    size_t out_len = 0, matched = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose(&fx.loader, "alice", 5, msg, strlen(msg), 0, &out,
                                         &out_len, &matched),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "user works_at Initech (was: Globex)");
    HU_ASSERT_STR_NOT_CONTAINS(out, "works_at Globex");
    HU_ASSERT_STR_NOT_CONTAINS(out, "works_at Acme");
    fx.alloc.free(fx.alloc.ctx, out, out_len + 1);
    gg_fixture_close(&fx);
}

/* The legacy prompt builder shares the read path: same contract. */
static void test_build_context_hides_superseded_employer(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_user_job_chain(&fx);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(
        hu_graph_build_context(fx.graph, &fx.alloc, "alice", 5, "user", 4, 1, 2048, &out, &out_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "(works_at) -> [Initech] (was: Globex)");
    HU_ASSERT_STR_NOT_CONTAINS(out, "[Globex]\n");
    HU_ASSERT_STR_NOT_CONTAINS(out, "[Acme]");
    fx.alloc.free(fx.alloc.ctx, out, 2048 + 1);
    gg_fixture_close(&fx);
}
#endif

static void test_graph_grounding_mode_parse(void) {
    /* Default SHADOW as of 2026-05-31: the grounding A/B (n=30, ON-win-rate 43.3%,
     * CI [27.4,60.8]) did NOT substantiate injection, so unset => SHADOW until a
     * measurement clears 50%. See src/agent/graph_grounding.c +
     * docs/research/2026-05-31-graphrag-grounding-ab.md. The 2026-07 query-
     * conditioned read-path rebuild deliberately did NOT change gate semantics:
     * promotion past shadow stays human-gated on a fresh blind A/B. */
    unsetenv("HU_GRAPH_GROUNDING");
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_SHADOW);
    setenv("HU_GRAPH_GROUNDING", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_SHADOW);
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_ON);
    /* Explicit off (disable) override. */
    setenv("HU_GRAPH_GROUNDING", "off", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    /* Unknown values disable (fail-safe to off, not silently on). */
    setenv("HU_GRAPH_GROUNDING", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    unsetenv("HU_GRAPH_GROUNDING");
}

/* The contact fallback ships OFF by default and fails safe to OFF on junk. */
static void test_contact_fallback_mode_parse(void) {
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    HU_ASSERT_EQ((int)hu_graph_grounding_contact_fallback_mode(), (int)HU_GG_FALLBACK_OFF);
    setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_contact_fallback_mode(), (int)HU_GG_FALLBACK_SHADOW);
    setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", "live", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_contact_fallback_mode(), (int)HU_GG_FALLBACK_LIVE);
    setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", "off", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_contact_fallback_mode(), (int)HU_GG_FALLBACK_OFF);
    setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_contact_fallback_mode(), (int)HU_GG_FALLBACK_OFF);
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
}

#ifdef HU_ENABLE_SQLITE

/* AC-1.1: Verify that autodream writes community_summaries after a runner invocation */
static void test_autodream_tick_populates_community_summaries_for_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *graph = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, ":memory:", strlen(":memory:"), &graph), HU_OK);

    sqlite3 *gdb = hu_graph_sqlite_connection(graph);

    /* Seed entities table with contact_id and community_id columns,
     * and relationships to give the community real structure */
    const char *create_entities_sql = "CREATE TABLE IF NOT EXISTS entities ("
                                      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                      "  name TEXT NOT NULL,"
                                      "  entity_type TEXT NOT NULL DEFAULT 'unknown',"
                                      "  contact_id TEXT,"
                                      "  community_id INTEGER,"
                                      "  mention_count INTEGER DEFAULT 1)";
    seed_run(gdb, create_entities_sql);

    const char *create_relationships_sql = "CREATE TABLE IF NOT EXISTS relations ("
                                           "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                           "  source_id INTEGER,"
                                           "  target_id INTEGER,"
                                           "  contact_id TEXT,"
                                           "  relationship_type TEXT DEFAULT 'knows',"
                                           "  weight REAL DEFAULT 1.0,"
                                           "  context TEXT,"
                                           "  event_start INTEGER,"
                                           "  event_end INTEGER DEFAULT 0,"
                                           "  last_seen INTEGER)";
    seed_run(gdb, create_relationships_sql);

    /* Seed entities for a test contact with community_id set (this is what
     * the community summarizer reads to generate summaries) */
    const char *seed_entities_sql =
        "INSERT INTO entities (name, entity_type, contact_id, community_id, mention_count) VALUES"
        "  ('Alice Friend', 'person', 'TestContact', 1, 5),"
        "  ('Bob Colleague', 'person', 'TestContact', 1, 3),"
        "  ('Carol Neighbor', 'person', 'TestContact', 1, 2),"
        "  ('David Other', 'person', 'TestContact', 2, 4),"
        "  ('Eve Another', 'person', 'TestContact', 2, 2)";
    seed_run(gdb, seed_entities_sql);

    /* Seed relationships between entities in the same community so the
     * summarizer has graph structure to count. Relations join with entities
     * via source_id = e.id to count live edges per community. */
    const char *seed_relations_sql = "INSERT INTO relations (source_id, target_id, contact_id, "
                                     "context, event_start, event_end) VALUES"
                                     "  (1, 2, 'TestContact', 'work_together', 1000, 0),"
                                     "  (2, 3, 'TestContact', 'neighborhood', 2000, 0),"
                                     "  (4, 5, 'TestContact', 'friends_group', 3000, 0)";
    seed_run(gdb, seed_relations_sql);

    /* Assert the table is empty before the runner.
     * The summarizer's ensure_autodream_schema will create community_summaries on first call. */
    sqlite3_stmt *pre_check = NULL;
    const char *count_sql =
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='community_summaries'";
    HU_ASSERT_EQ(sqlite3_prepare_v2(gdb, count_sql, -1, &pre_check, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(pre_check), SQLITE_ROW);
    int table_exists_pre = sqlite3_column_int(pre_check, 0);
    sqlite3_finalize(pre_check);
    /* Table should not exist yet before calling the summarizer */
    HU_ASSERT_EQ(table_exists_pre, 0);

    /* Invoke the real production summarizer for community 1 */
    hu_error_t runner_result = hu_autodream_summarize_community(
        &alloc, graph, "TestContact", strlen("TestContact"), 1, (int64_t)time(NULL) * 1000);
    HU_ASSERT_EQ(runner_result, HU_OK);

    /* After runner completes, query the table for inserted rows */
    const char *count_summaries_sql =
        "SELECT COUNT(*) FROM community_summaries WHERE contact_id = 'TestContact'";
    sqlite3_stmt *post_check = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(gdb, count_summaries_sql, -1, &post_check, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(post_check), SQLITE_ROW);
    int post_count = sqlite3_column_int(post_check, 0);
    sqlite3_finalize(post_check);

    /* AC-1.1 contract: after autodream summarizer invocation, table MUST have >= 1 row
     * for the contact. The runner was called with real seeded entities and community_id,
     * so it must have written at least one community_summaries row. */
    HU_ASSERT_TRUE(post_count >= 1);

    hu_graph_close(graph, &alloc);
}

#endif

/* AC-1.3: Compliance test that the gate comment exists in source. Checks the
 * comment is PRESENT (not at a hardcoded line — that pinned line 1471 and broke
 * whenever code was inserted above it; presence is the real contract). */
static void test_gate_comment_exists_at_agent_turn_1471(void) {
    FILE *f = fopen("src/agent/agent_turn.c", "r");
    HU_ASSERT_NOT_NULL(f);

    char buf[512];
    bool found_comment = false;
    /* Search the whole file: the gate comment PRESENCE is the contract; its
     * line shifts whenever agent_turn.c is edited (the ToM helper), so do not
     * pin a line range. */
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "GraphRAG activation gated") != NULL) {
            found_comment = true;
            break;
        }
    }
    fclose(f);
    HU_ASSERT_TRUE(found_comment);
}

/* Regression (GraphRAG calibration audit 2026-05-31): the Self-RAG
 * memory-relevance !should_use branch must NOT free graph_ctx.
 * hu_srag_verify_relevance scores memory_ctx ONLY; coupling graph_ctx's lifetime
 * to that verdict silently defeated GraphRAG grounding whenever flat memory was
 * judged irrelevant. Pins the decoupling: between the "Self-RAG: verify
 * relevance" comment and the "behavior_memory_ctx_nonempty" line that follows
 * the block, no graph_ctx free may appear. (Source-presence style, like the
 * gate-comment test above — fails on the pre-fix code that freed graph_ctx.) */
static void test_srag_memory_miss_does_not_free_graph_ctx(void) {
    FILE *f = fopen("src/agent/agent_turn.c", "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[512];
    bool in_block = false;
    bool freed_graph_in_block = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (!in_block) {
            if (strstr(buf, "Self-RAG: verify relevance") != NULL)
                in_block = true;
            continue;
        }
        /* The statement immediately following the Self-RAG block. */
        if (strstr(buf, "behavior_memory_ctx_nonempty") != NULL)
            break;
        if (strstr(buf, "graph_ctx") != NULL && strstr(buf, "free") != NULL)
            freed_graph_in_block = true;
    }
    fclose(f);
    HU_ASSERT_FALSE(freed_graph_in_block);
}

void run_graph_grounding_tests(void) {
    HU_TEST_SUITE("GraphRAG grounding");
    HU_RUN_TEST(test_graph_grounding_mode_parse);
    HU_RUN_TEST(test_contact_fallback_mode_parse);
    HU_RUN_TEST(test_gate_comment_exists_at_agent_turn_1471);
    HU_RUN_TEST(test_srag_memory_miss_does_not_free_graph_ctx);
    HU_RUN_TEST(test_ground_match_count_respects_word_boundaries);
    HU_RUN_TEST(test_ground_name_word_count_skips_stopwords_and_short_words);
    HU_RUN_TEST(test_ground_score_zero_without_match_and_coverage_dominates);
    HU_RUN_TEST(test_ground_fingerprint_varies_with_content);
    HU_RUN_TEST(test_ground_is_placeholder_name);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_compose_selects_relevant_entity_content);
    HU_RUN_TEST(test_compose_finds_low_mention_entity_beyond_candidate_cap);
    HU_RUN_TEST(test_compose_irrelevant_message_returns_empty);
    HU_RUN_TEST(test_compose_word_boundary_prevents_false_seed);
    HU_RUN_TEST(test_compose_respects_budget_cap);
    HU_RUN_TEST(test_compose_varies_with_conversation);
    HU_RUN_TEST(test_compose_scopes_to_contact);
    HU_RUN_TEST(test_compose_ex_contact_fallback_fills_lexical_miss);
    HU_RUN_TEST(test_compose_ex_without_flag_keeps_empty_on_miss);
    HU_RUN_TEST(test_compose_ex_fallback_inert_on_lexical_hit);
    HU_RUN_TEST(test_compose_ex_fallback_scoped_to_contact);
    HU_RUN_TEST(test_compose_ex_fallback_respects_budget);
    HU_RUN_TEST(test_compose_ex_fallback_skips_emotion_entities);
    HU_RUN_TEST(test_compose_ex_fallback_skips_placeholders);
    HU_RUN_TEST(test_compose_ex_require_full_name_blocks_partial_hits);
    HU_RUN_TEST(test_load_grounding_self_facts_gate);
    HU_RUN_TEST(test_self_facts_mode_defaults_off);
    HU_RUN_TEST(test_load_grounding_contact_fallback_gate);
    HU_RUN_TEST(test_compose_no_graph_is_failopen);
    HU_RUN_TEST(test_compose_renders_current_employer_with_predecessor);
    HU_RUN_TEST(test_compose_marks_superseded_employer_as_history);
    HU_RUN_TEST(test_compose_from_person_seed_collapses_chain_to_head);
    HU_RUN_TEST(test_build_context_hides_superseded_employer);
    HU_RUN_TEST(test_autodream_tick_populates_community_summaries_for_contact);
#endif
}
