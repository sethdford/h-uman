/*
 * GraphRAG (hu_graph_t) tests.
 * Requires HU_ENABLE_SQLITE. Uses :memory: when HU_IS_TEST (test build).
 */
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Retype policy truth table (spec 2026-09-29 §4.1): UNKNOWN may become any
 * type, TOPIC may become a name type, and nothing else ever changes. */
static void graph_retype_allowed_truth_table(void) {
    const hu_entity_type_t names[] = {HU_ENTITY_PERSON, HU_ENTITY_PLACE, HU_ENTITY_ORGANIZATION,
                                      HU_ENTITY_EVENT};
    for (size_t i = 0; i < 4; i++) {
        HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, names[i]));
        HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, names[i]));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], HU_ENTITY_TOPIC));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], HU_ENTITY_UNKNOWN));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], HU_ENTITY_EMOTION));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, names[i]));
        for (size_t j = 0; j < 4; j++) /* a name type is never swapped or downgraded */
            HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], names[j]));
    }
    HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, HU_ENTITY_TOPIC));
    HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, HU_ENTITY_EMOTION));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, HU_ENTITY_UNKNOWN));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, HU_ENTITY_TOPIC));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, HU_ENTITY_EMOTION));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, HU_ENTITY_UNKNOWN));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, HU_ENTITY_TOPIC));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, HU_ENTITY_UNKNOWN));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, HU_ENTITY_EMOTION));
    /* out-of-range enum values fail closed */
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed((hu_entity_type_t)42, HU_ENTITY_PERSON));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, (hu_entity_type_t)42));
}

#ifdef HU_ENABLE_SQLITE

static void graph_open_valid_path_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_error_t err = hu_graph_open(&alloc, "ignored", 7, &g);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(g);
    hu_graph_close(g, &alloc);
}

static void graph_open_null_alloc_returns_error(void) {
    hu_graph_t *g = NULL;
    hu_error_t err = hu_graph_open(NULL, "path", 4, &g);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(g);
}

static void graph_open_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_graph_open(&alloc, "path", 4, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_close_null_graph_no_op(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_close(NULL, &alloc);
}

static void graph_close_null_alloc_no_op(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);
    HU_ASSERT_NOT_NULL(g);
    hu_graph_close(g, NULL);
    hu_graph_close(g, &alloc);
}

static void graph_close_valid_releases(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);
    hu_graph_close(g, &alloc);
}

#include <sqlite3.h>

typedef struct typed_row {
    bool found;
    int type;
    int mention_count;
    int64_t last_seen;
    char provenance[64];
    double confidence;
} typed_row_t;

/* Read the columns hu_graph_find_entity does not expose (provenance, confidence). */
static typed_row_t typed_row(hu_graph_t *g, const char *cid, const char *name) {
    typed_row_t r;
    memset(&r, 0, sizeof(r));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(hu_graph_sqlite_connection(g),
                           "SELECT type, mention_count, last_seen, COALESCE(provenance, ''),"
                           " confidence FROM entities WHERE contact_id = ?1 AND name = ?2",
                           -1, &q, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_text(q, 1, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        r.found = true;
        r.type = sqlite3_column_int(q, 0);
        r.mention_count = sqlite3_column_int(q, 1);
        r.last_seen = sqlite3_column_int64(q, 2);
        snprintf(r.provenance, sizeof(r.provenance), "%s", (const char *)sqlite3_column_text(q, 3));
        r.confidence = sqlite3_column_double(q, 4);
    }
    sqlite3_finalize(q);
    return r;
}

static void graph_upsert_typed_insert_sets_type_provenance_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_PERSON,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    HU_ASSERT_TRUE(id > 0);
    typed_row_t r = typed_row(g, "c1", "Salim");
    HU_ASSERT_TRUE(r.found);
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_EQ(r.mention_count, 1);
    HU_ASSERT_STR_EQ(r.provenance, "names:nightly");
    HU_ASSERT_FLOAT_EQ(r.confidence, 0.8, 1e-6);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_upgrades_unknown_and_bumps(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Vanguard", 8, HU_ENTITY_UNKNOWN, NULL, &id1),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Vanguard", 8, HU_ENTITY_ORGANIZATION,
                                              "names:nightly", 0.8f, 0, &id2),
                 HU_OK);
    HU_ASSERT_EQ(id1, id2);
    typed_row_t r = typed_row(g, "c1", "Vanguard");
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(r.mention_count, 2);
    HU_ASSERT_STR_EQ(r.provenance, "names:nightly"); /* the legacy row had none */
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_never_downgrades_a_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_PERSON,
                                              "names:turn", 0.3f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_TOPIC,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    typed_row_t r = typed_row(g, "c1", "Salim");
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_EQ(r.mention_count, 2);
    HU_ASSERT_STR_EQ(r.provenance, "names:turn"); /* first writer wins */
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_topic_upgrades_to_place(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Tampa", 5, HU_ENTITY_TOPIC, NULL, &id), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Tampa", 5, HU_ENTITY_PLACE,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(typed_row(g, "c1", "Tampa").type, (int)HU_ENTITY_PLACE);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_leaves_emotion_alone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "grief", 5, HU_ENTITY_EMOTION, NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "grief", 5, HU_ENTITY_PERSON,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(typed_row(g, "c1", "grief").type, (int)HU_ENTITY_EMOTION);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_no_touch_retypes_without_bumping_or_creating(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Acme", 4, HU_ENTITY_UNKNOWN, NULL, &id),
                 HU_OK);
    typed_row_t before = typed_row(g, "c1", "Acme");
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION,
                                              "names:migrate", 0.6f, HU_GRAPH_UPSERT_NO_TOUCH, &id),
                 HU_OK);
    typed_row_t after = typed_row(g, "c1", "Acme");
    HU_ASSERT_EQ(after.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(after.mention_count, before.mention_count);
    HU_ASSERT_EQ(after.last_seen, before.last_seen);
    HU_ASSERT_STR_EQ(after.provenance, "names:migrate");
    /* retype-only never creates */
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Ghost", 5, HU_ENTITY_PERSON,
                                              "names:migrate", 0.6f, HU_GRAPH_UPSERT_NO_TOUCH, &id),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_FALSE(typed_row(g, "c1", "Ghost").found);
    /* retype-only with a disallowed retype leaves the row exactly as it was */
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Acme", 4, HU_ENTITY_TOPIC, "names:other",
                                              0.9f, HU_GRAPH_UPSERT_NO_TOUCH, &id),
                 HU_OK);
    typed_row_t kept = typed_row(g, "c1", "Acme");
    HU_ASSERT_EQ(kept.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(kept.mention_count, before.mention_count);
    HU_ASSERT_STR_EQ(kept.provenance, "names:migrate");
    hu_graph_close(g, &alloc);
}

/* M3: provenance is stamped on insert or on a real retype only. A plain bump
 * (same type) or a refused retype leaves it as it was; a real retype
 * overwrites it with the retyper's. */
static void graph_upsert_typed_stamps_provenance_only_on_retype(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Salim", 5, HU_ENTITY_PERSON, NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_PERSON,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    typed_row_t bump = typed_row(g, "c1", "Salim");
    HU_ASSERT_EQ(bump.mention_count, 2);
    HU_ASSERT_STR_EQ(bump.provenance, ""); /* same type: a bump, not a stamp */
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Pickleball", 10, HU_ENTITY_TOPIC, NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Pickleball", 10, HU_ENTITY_UNKNOWN,
                                              "names:turn", 0.3f, 0, &id),
                 HU_OK);
    typed_row_t refused = typed_row(g, "c1", "Pickleball");
    HU_ASSERT_STR_EQ(refused.provenance, ""); /* refused retype */
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Priya", 5, HU_ENTITY_UNKNOWN,
                                              "names:turn", 0.3f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Priya", 5, HU_ENTITY_PERSON,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    typed_row_t re = typed_row(g, "c1", "Priya");
    HU_ASSERT_EQ(re.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_STR_EQ(re.provenance, "names:nightly"); /* real retype stamps */
    hu_graph_close(g, &alloc);
}

/* The list query carries provenance so callers can tell unconfirmed rows. */
static void graph_list_entities_carries_provenance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Priya", 5, HU_ENTITY_UNKNOWN,
                                              "names:turn", 0.3f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Tampa", 5, HU_ENTITY_PLACE, NULL, &id), HU_OK);
    hu_graph_entity_t *ents = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_entities(g, &alloc, "c1", 2, 10, &ents, &n), HU_OK);
    HU_ASSERT_EQ((long)n, 2L);
    int seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ents[i].name, "Priya") == 0) {
            HU_ASSERT_STR_EQ(ents[i].provenance, "names:turn");
            seen++;
        } else {
            HU_ASSERT_STR_EQ(ents[i].provenance, "");
            seen++;
        }
    }
    HU_ASSERT_EQ(seen, 2);
    hu_graph_entities_free(&alloc, ents, n);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_rejects_bad_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity_typed(NULL, "c1", 2, "A", 1, HU_ENTITY_PERSON, NULL, 0.5f, 0, &id),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity_typed(g, "c1", 2, "A", 0, HU_ENTITY_PERSON, NULL, 0.5f, 0, &id),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity_typed(g, "c1", 2, NULL, 1, HU_ENTITY_PERSON, NULL, 0.5f, 0, &id),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity_typed(g, "c1", 2, "A", 1, HU_ENTITY_PERSON, NULL, 0.5f, 0, NULL),
        HU_ERR_INVALID_ARGUMENT);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_insert_new_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_update_existing_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id1 = 0;
    hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &id1);

    int64_t id2 = 0;
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &id2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(id1, id2);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_null_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, NULL, 5, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_empty_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, "alice", 0, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_null_out_id_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_error_t err = hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_with_metadata(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    const char *meta = "{\"role\":\"friend\"}";
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, "charlie", 7, HU_ENTITY_PERSON, meta, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, "", 0, "charlie", 7, &ent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ent.metadata_json);
    HU_ASSERT_TRUE(strstr(ent.metadata_json, "friend") != NULL);

    if (ent.name)
        alloc.free(alloc.ctx, ent.name, ent.name_len + 1);
    if (ent.metadata_json)
        alloc.free(alloc.ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);
    hu_graph_close(g, &alloc);
}

static void graph_find_entity_existing_returns_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_graph_upsert_entity(g, "", 0, "dave", 4, HU_ENTITY_ORGANIZATION, NULL, &id);

    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(g, "", 0, "dave", 4, &ent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ent.id, id);
    HU_ASSERT_NOT_NULL(ent.name);
    HU_ASSERT_EQ(ent.name_len, 4u);
    HU_ASSERT_EQ(ent.type, HU_ENTITY_ORGANIZATION);
    HU_ASSERT_TRUE(ent.mention_count >= 1);

    if (ent.name)
        alloc.free(alloc.ctx, ent.name, ent.name_len + 1);
    if (ent.metadata_json)
        alloc.free(alloc.ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);
    hu_graph_close(g, &alloc);
}

static void graph_find_entity_nonexistent_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(g, "", 0, "never_inserted", 14, &ent);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    hu_graph_close(g, &alloc);
}

static void graph_find_entity_null_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(g, "", 0, NULL, 5, &ent);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_find_entity_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);
    int64_t id = 0;
    hu_graph_upsert_entity(g, "", 0, "eve", 3, HU_ENTITY_PERSON, NULL, &id);

    hu_error_t err = hu_graph_find_entity(g, "", 0, "eve", 3, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_relation_insert_new_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &b);

    hu_error_t err =
        hu_graph_upsert_relation(g, "", 0, a, b, HU_REL_KNOWS, 1.0f, "met at work", 12);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_relation_update_weight_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "", 0, "x", 1, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "", 0, "y", 1, HU_ENTITY_PERSON, NULL, &b);

    hu_graph_upsert_relation(g, "", 0, a, b, HU_REL_WORKS_AT, 0.5f, NULL, 0);
    hu_error_t err = hu_graph_upsert_relation(g, "", 0, a, b, HU_REL_WORKS_AT, 0.9f, "updated", 7);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_relation_invalid_ids_returns_io(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_error_t err = hu_graph_upsert_relation(g, "", 0, 99999, 99998, HU_REL_KNOWS, 1.0f, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_IO);

    hu_graph_close(g, &alloc);
}

static void graph_neighbors_returns_adjacent_entities(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0, c = 0;
    hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_entity(g, "", 0, "charlie", 7, HU_ENTITY_PERSON, NULL, &c);
    hu_graph_upsert_relation(g, "", 0, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);
    hu_graph_upsert_relation(g, "", 0, a, c, HU_REL_KNOWS, 1.0f, NULL, 0);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err = hu_graph_neighbors(g, &alloc, "", 0, a, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(entities);
    HU_ASSERT_NOT_NULL(relations);

    hu_graph_entities_free(&alloc, entities, count);
    hu_graph_relations_free(&alloc, relations, count);
    hu_graph_close(g, &alloc);
}

static void graph_neighbors_no_neighbors_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0;
    hu_graph_upsert_entity(g, "", 0, "isolated", 8, HU_ENTITY_TOPIC, NULL, &a);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err = hu_graph_neighbors(g, &alloc, "", 0, a, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(entities);
    HU_ASSERT_NULL(relations);

    hu_graph_close(g, &alloc);
}

static void graph_neighbors_invalid_entity_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_graph_neighbors(g, &alloc, "", 0, 99999, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    hu_graph_close(g, &alloc);
}

static void graph_neighbors_null_params_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0;
    hu_graph_upsert_entity(g, "", 0, "a", 1, HU_ENTITY_PERSON, NULL, &a);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;

    hu_error_t err =
        hu_graph_neighbors(NULL, &alloc, "", 0, a, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_graph_neighbors(g, NULL, "", 0, a, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_build_context_with_data_returns_formatted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_relation(g, "", 0, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_context(g, &alloc, "", 0, "alice bob", 9, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "Knowledge Graph") != NULL);
    HU_ASSERT_TRUE(strstr(out, "alice") != NULL || strstr(out, "bob") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_build_context_empty_graph_returns_empty_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_context(g, &alloc, "", 0, "nonexistent", 11, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_build_context_null_query_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_context(g, &alloc, "", 0, NULL, 5, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_build_context_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    size_t out_len = 0;
    hu_error_t err = hu_graph_build_context(g, &alloc, "", 0, "query", 5, 1, 4096, NULL, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_build_contact_context_with_contact_prepends_header(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "contact_123", 10, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "contact_123", 10, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_relation(g, "contact_123", 10, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_contact_context(g, &alloc, "alice", 5, "contact_123", 10, 1,
                                                    4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    /* Verify output contains contact context header or entity data */
    HU_ASSERT_TRUE(strstr(out, "Knowledge") != NULL || strstr(out, "alice") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_build_contact_context_empty_contact_returns_plain_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0;
    hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &a);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_contact_context(g, &alloc, "alice", 5, NULL, 0, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);

    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_build_communities_with_data_returns_clusters(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0, c = 0;
    hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_entity(g, "", 0, "charlie", 7, HU_ENTITY_PERSON, NULL, &c);
    hu_graph_upsert_relation(g, "", 0, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);
    hu_graph_upsert_relation(g, "", 0, b, c, HU_REL_KNOWS, 1.0f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_communities(g, &alloc, "", 0, 10, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "Topic Clusters") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_build_communities_empty_graph_returns_header_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_communities(g, &alloc, "", 0, 10, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Topic Clusters") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_entities_free_null_alloc_no_op(void) {
    hu_graph_entities_free(NULL, (hu_graph_entity_t *)0x1, 1);
}

static void graph_entities_free_null_entities_no_op(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_entities_free(&alloc, NULL, 0);
}

static void graph_relations_free_null_alloc_no_op(void) {
    hu_graph_relations_free(NULL, (hu_graph_relation_t *)0x1, 1);
}

static void graph_relations_free_null_relations_no_op(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_relations_free(&alloc, NULL, 0);
}

static void entity_type_from_string_all_types(void) {
    HU_ASSERT_EQ(hu_entity_type_from_string("person", 6), HU_ENTITY_PERSON);
    HU_ASSERT_EQ(hu_entity_type_from_string("place", 5), HU_ENTITY_PLACE);
    HU_ASSERT_EQ(hu_entity_type_from_string("organization", 12), HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(hu_entity_type_from_string("event", 5), HU_ENTITY_EVENT);
    HU_ASSERT_EQ(hu_entity_type_from_string("topic", 5), HU_ENTITY_TOPIC);
    HU_ASSERT_EQ(hu_entity_type_from_string("emotion", 7), HU_ENTITY_EMOTION);
}

static void entity_type_from_string_null_returns_unknown(void) {
    HU_ASSERT_EQ(hu_entity_type_from_string(NULL, 5), HU_ENTITY_UNKNOWN);
}

static void entity_type_from_string_empty_returns_unknown(void) {
    HU_ASSERT_EQ(hu_entity_type_from_string("person", 0), HU_ENTITY_UNKNOWN);
}

static void entity_type_from_string_case_insensitive(void) {
    HU_ASSERT_EQ(hu_entity_type_from_string("PERSON", 6), HU_ENTITY_PERSON);
    HU_ASSERT_EQ(hu_entity_type_from_string("Place", 5), HU_ENTITY_PLACE);
}

static void entity_type_to_string_all_types(void) {
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_PERSON), "person");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_PLACE), "place");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_ORGANIZATION), "organization");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_EVENT), "event");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_TOPIC), "topic");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_EMOTION), "emotion");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_UNKNOWN), "unknown");
}

static void relation_type_from_string_all_types(void) {
    HU_ASSERT_EQ(hu_relation_type_from_string("knows", strlen("knows")), HU_REL_KNOWS);
    HU_ASSERT_EQ(hu_relation_type_from_string("family_of", strlen("family_of")), HU_REL_FAMILY_OF);
    HU_ASSERT_EQ(hu_relation_type_from_string("works_at", strlen("works_at")), HU_REL_WORKS_AT);
    HU_ASSERT_EQ(hu_relation_type_from_string("lives_in", strlen("lives_in")), HU_REL_LIVES_IN);
    HU_ASSERT_EQ(hu_relation_type_from_string("interested_in", strlen("interested_in")),
                 HU_REL_INTERESTED_IN);
    HU_ASSERT_EQ(hu_relation_type_from_string("discussed_with", strlen("discussed_with")),
                 HU_REL_DISCUSSED_WITH);
    HU_ASSERT_EQ(hu_relation_type_from_string("feels_about", strlen("feels_about")),
                 HU_REL_FEELS_ABOUT);
    HU_ASSERT_EQ(hu_relation_type_from_string("promised_to", strlen("promised_to")),
                 HU_REL_PROMISED_TO);
    HU_ASSERT_EQ(hu_relation_type_from_string("shared_experience", strlen("shared_experience")),
                 HU_REL_SHARED_EXPERIENCE);
    HU_ASSERT_EQ(hu_relation_type_from_string("related_to", strlen("related_to")),
                 HU_REL_RELATED_TO);
}

static void relation_type_from_string_aliases(void) {
    HU_ASSERT_EQ(hu_relation_type_from_string("family", 6), HU_REL_FAMILY_OF);
    HU_ASSERT_EQ(hu_relation_type_from_string("job", 3), HU_REL_WORKS_AT);
    HU_ASSERT_EQ(hu_relation_type_from_string("likes", 5), HU_REL_INTERESTED_IN);
    HU_ASSERT_EQ(hu_relation_type_from_string("is_a", 4), HU_REL_RELATED_TO);
}

static void relation_type_from_string_null_returns_related_to(void) {
    HU_ASSERT_EQ(hu_relation_type_from_string(NULL, 5), HU_REL_RELATED_TO);
}

static void relation_type_to_string_all_types(void) {
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_KNOWS), "knows");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_FAMILY_OF), "family_of");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_WORKS_AT), "works_at");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_LIVES_IN), "lives_in");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_INTERESTED_IN), "interested_in");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_DISCUSSED_WITH), "discussed_with");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_FEELS_ABOUT), "feels_about");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_PROMISED_TO), "promised_to");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_SHARED_EXPERIENCE), "shared_experience");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_RELATED_TO), "related_to");
}

static void graph_entity_special_chars_in_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    const char *name = "O'Brien";
    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, name, 7, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, "", 0, name, 7, &ent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ent.name);
    HU_ASSERT_EQ(ent.name_len, 7u);

    if (ent.name)
        alloc.free(alloc.ctx, ent.name, ent.name_len + 1);
    if (ent.metadata_json)
        alloc.free(alloc.ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);
    hu_graph_close(g, &alloc);
}

static void graph_entity_unicode_in_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    const char *name = "Jos\xc3\xa9";
    size_t len = 4;
    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, "", 0, name, len, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, "", 0, name, len, &ent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ent.name);

    if (ent.name)
        alloc.free(alloc.ctx, ent.name, ent.name_len + 1);
    if (ent.metadata_json)
        alloc.free(alloc.ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);
    hu_graph_close(g, &alloc);
}

static void graph_neighbors_max_results_respected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t center = 0;
    hu_graph_upsert_entity(g, "", 0, "center", 6, HU_ENTITY_PERSON, NULL, &center);
    for (int i = 0; i < 5; i++) {
        char name[16];
        int n = snprintf(name, sizeof(name), "n%d", i);
        int64_t id = 0;
        hu_graph_upsert_entity(g, "", 0, name, (size_t)n, HU_ENTITY_PERSON, NULL, &id);
        hu_graph_upsert_relation(g, "", 0, center, id, HU_REL_KNOWS, 1.0f, NULL, 0);
    }

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_graph_neighbors(g, &alloc, "", 0, center, 1, 2, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);

    hu_graph_entities_free(&alloc, entities, count);
    hu_graph_relations_free(&alloc, relations, count);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_null_graph_returns_error(void) {
    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(NULL, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_find_entity_null_graph_returns_error(void) {
    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(NULL, "", 0, "alice", 5, &ent);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_upsert_relation_null_graph_returns_error(void) {
    hu_error_t err = hu_graph_upsert_relation(NULL, "", 0, 1, 2, HU_REL_KNOWS, 1.0f, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_build_context_null_graph_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_context(NULL, &alloc, "", 0, "q", 1, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
}

static void graph_build_communities_null_params_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_communities(NULL, &alloc, "", 0, 10, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_leiden_communities_returns_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t e1 = 0, e2 = 0, e3 = 0;
    hu_graph_upsert_entity(g, "", 0, "Alice", 5, HU_ENTITY_PERSON, NULL, &e1);
    hu_graph_upsert_entity(g, "", 0, "Bob", 3, HU_ENTITY_PERSON, NULL, &e2);
    hu_graph_upsert_entity(g, "", 0, "Charlie", 7, HU_ENTITY_PERSON, NULL, &e3);
    hu_graph_upsert_relation(g, "", 0, e1, e2, HU_REL_KNOWS, 0.9f, NULL, 0);
    hu_graph_upsert_relation(g, "", 0, e2, e3, HU_REL_KNOWS, 0.8f, NULL, 0);
    hu_graph_upsert_relation(g, "", 0, e1, e3, HU_REL_KNOWS, 0.7f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_leiden_communities(g, &alloc, "", 0, 10, 100, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    if (out) {
        alloc.free(alloc.ctx, out, out_len + 1);
    }

    hu_graph_close(g, &alloc);
}

static void graph_reconsolidate_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t e1 = 0;
    hu_graph_upsert_entity(g, "", 0, "TestEntity", 10, HU_ENTITY_TOPIC, NULL, &e1);

    hu_error_t err =
        hu_graph_reconsolidate(g, &alloc, "", 0, "TestEntity", 10, "new context info", 16);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_FOUND);

    hu_graph_close(g, &alloc);
}

static void graph_entity_isolation_across_contacts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    /* Insert entity for contact_a */
    int64_t id_a = 0;
    hu_error_t err =
        hu_graph_upsert_entity(g, "contact_a", 9, "alice", 5, HU_ENTITY_PERSON, NULL, &id_a);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id_a > 0);

    /* Insert same-name entity for contact_b */
    int64_t id_b = 0;
    err = hu_graph_upsert_entity(g, "contact_b", 9, "alice", 5, HU_ENTITY_PERSON, NULL, &id_b);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id_b > 0);
    HU_ASSERT_TRUE(id_a != id_b); /* different entities */

    /* Find as contact_a — should get id_a */
    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, "contact_a", 9, "alice", 5, &ent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ent.id, id_a);
    if (ent.name)
        alloc.free(alloc.ctx, ent.name, ent.name_len + 1);
    if (ent.metadata_json)
        alloc.free(alloc.ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);

    /* Find as contact_b — should get id_b */
    err = hu_graph_find_entity(g, "contact_b", 9, "alice", 5, &ent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ent.id, id_b);
    if (ent.name)
        alloc.free(alloc.ctx, ent.name, ent.name_len + 1);
    if (ent.metadata_json)
        alloc.free(alloc.ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);

    /* Find as contact_c (never inserted) — should fail */
    err = hu_graph_find_entity(g, "contact_c", 9, "alice", 5, &ent);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    hu_graph_close(g, &alloc);
}

void run_graph_tests(void) {
    HU_TEST_SUITE("graph");
    HU_RUN_TEST(graph_retype_allowed_truth_table);
    HU_RUN_TEST(graph_upsert_typed_insert_sets_type_provenance_confidence);
    HU_RUN_TEST(graph_upsert_typed_upgrades_unknown_and_bumps);
    HU_RUN_TEST(graph_upsert_typed_never_downgrades_a_name);
    HU_RUN_TEST(graph_upsert_typed_topic_upgrades_to_place);
    HU_RUN_TEST(graph_upsert_typed_leaves_emotion_alone);
    HU_RUN_TEST(graph_upsert_typed_no_touch_retypes_without_bumping_or_creating);
    HU_RUN_TEST(graph_upsert_typed_stamps_provenance_only_on_retype);
    HU_RUN_TEST(graph_list_entities_carries_provenance);
    HU_RUN_TEST(graph_upsert_typed_rejects_bad_args);
    HU_RUN_TEST(graph_open_valid_path_succeeds);
    HU_RUN_TEST(graph_open_null_alloc_returns_error);
    HU_RUN_TEST(graph_open_null_out_returns_error);
    HU_RUN_TEST(graph_close_null_graph_no_op);
    HU_RUN_TEST(graph_close_null_alloc_no_op);
    HU_RUN_TEST(graph_close_valid_releases);
    HU_RUN_TEST(graph_upsert_entity_insert_new_succeeds);
    HU_RUN_TEST(graph_upsert_entity_update_existing_succeeds);
    HU_RUN_TEST(graph_upsert_entity_null_name_returns_error);
    HU_RUN_TEST(graph_upsert_entity_empty_name_returns_error);
    HU_RUN_TEST(graph_upsert_entity_null_out_id_returns_error);
    HU_RUN_TEST(graph_upsert_entity_with_metadata);
    HU_RUN_TEST(graph_upsert_entity_null_graph_returns_error);
    HU_RUN_TEST(graph_find_entity_existing_returns_data);
    HU_RUN_TEST(graph_find_entity_nonexistent_returns_not_found);
    HU_RUN_TEST(graph_find_entity_null_name_returns_error);
    HU_RUN_TEST(graph_find_entity_null_out_returns_error);
    HU_RUN_TEST(graph_find_entity_null_graph_returns_error);
    HU_RUN_TEST(graph_upsert_relation_insert_new_succeeds);
    HU_RUN_TEST(graph_upsert_relation_update_weight_succeeds);
    HU_RUN_TEST(graph_upsert_relation_invalid_ids_returns_io);
    HU_RUN_TEST(graph_upsert_relation_null_graph_returns_error);
    HU_RUN_TEST(graph_neighbors_returns_adjacent_entities);
    HU_RUN_TEST(graph_neighbors_no_neighbors_returns_empty);
    HU_RUN_TEST(graph_neighbors_invalid_entity_returns_empty);
    HU_RUN_TEST(graph_neighbors_null_params_returns_error);
    HU_RUN_TEST(graph_neighbors_max_results_respected);
    HU_RUN_TEST(graph_build_context_with_data_returns_formatted);
    HU_RUN_TEST(graph_build_context_empty_graph_returns_empty_string);
    HU_RUN_TEST(graph_build_context_null_query_returns_error);
    HU_RUN_TEST(graph_build_context_null_out_returns_error);
    HU_RUN_TEST(graph_build_context_null_graph_returns_error);
    HU_RUN_TEST(graph_build_contact_context_with_contact_prepends_header);
    HU_RUN_TEST(graph_build_contact_context_empty_contact_returns_plain_context);
    HU_RUN_TEST(graph_build_communities_with_data_returns_clusters);
    HU_RUN_TEST(graph_build_communities_empty_graph_returns_header_only);
    HU_RUN_TEST(graph_build_communities_null_params_returns_error);
    HU_RUN_TEST(graph_leiden_communities_returns_output);
    HU_RUN_TEST(graph_reconsolidate_no_crash);
    HU_RUN_TEST(graph_entities_free_null_alloc_no_op);
    HU_RUN_TEST(graph_entities_free_null_entities_no_op);
    HU_RUN_TEST(graph_relations_free_null_alloc_no_op);
    HU_RUN_TEST(graph_relations_free_null_relations_no_op);
    HU_RUN_TEST(entity_type_from_string_all_types);
    HU_RUN_TEST(entity_type_from_string_null_returns_unknown);
    HU_RUN_TEST(entity_type_from_string_empty_returns_unknown);
    HU_RUN_TEST(entity_type_from_string_case_insensitive);
    HU_RUN_TEST(entity_type_to_string_all_types);
    HU_RUN_TEST(relation_type_from_string_all_types);
    HU_RUN_TEST(relation_type_from_string_aliases);
    HU_RUN_TEST(relation_type_from_string_null_returns_related_to);
    HU_RUN_TEST(relation_type_to_string_all_types);
    HU_RUN_TEST(graph_entity_special_chars_in_name);
    HU_RUN_TEST(graph_entity_unicode_in_name);
    HU_RUN_TEST(graph_entity_isolation_across_contacts);
}

#else

void run_graph_tests(void) {
    HU_TEST_SUITE("graph");
    HU_RUN_TEST(graph_retype_allowed_truth_table);
}

#endif /* HU_ENABLE_SQLITE */
