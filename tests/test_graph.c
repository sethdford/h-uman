/*
 * GraphRAG (hu_graph_t) tests.
 * Requires HU_ENABLE_SQLITE. Uses :memory: when HU_IS_TEST (test build).
 */
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

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

static void graph_upsert_entity_insert_new_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_error_t err =
        hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_update_existing_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id1 = 0;
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &id1);

    int64_t id2 = 0;
    hu_error_t err =
        hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &id2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(id1, id2);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_null_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_error_t err =
        hu_graph_upsert_entity(g, NULL, 5, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_empty_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    hu_error_t err =
        hu_graph_upsert_entity(g, "alice", 0, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_null_out_id_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_error_t err =
        hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_with_metadata(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t id = 0;
    const char *meta = "{\"role\":\"friend\"}";
    hu_error_t err = hu_graph_upsert_entity(g, "charlie", 7, HU_ENTITY_PERSON,
                                            meta, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, "charlie", 7, &ent);
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
    hu_graph_upsert_entity(g, "dave", 4, HU_ENTITY_ORGANIZATION, NULL, &id);

    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(g, "dave", 4, &ent);
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
    hu_error_t err = hu_graph_find_entity(g, "never_inserted", 14, &ent);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    hu_graph_close(g, &alloc);
}

static void graph_find_entity_null_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(g, NULL, 5, &ent);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_find_entity_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);
    int64_t id = 0;
    hu_graph_upsert_entity(g, "eve", 3, HU_ENTITY_PERSON, NULL, &id);

    hu_error_t err = hu_graph_find_entity(g, "eve", 3, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_relation_insert_new_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &b);

    hu_error_t err = hu_graph_upsert_relation(g, a, b, HU_REL_KNOWS, 1.0f,
                                              "met at work", 12);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_relation_update_weight_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "x", 1, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "y", 1, HU_ENTITY_PERSON, NULL, &b);

    hu_graph_upsert_relation(g, a, b, HU_REL_WORKS_AT, 0.5f, NULL, 0);
    hu_error_t err =
        hu_graph_upsert_relation(g, a, b, HU_REL_WORKS_AT, 0.9f, "updated", 7);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_close(g, &alloc);
}

static void graph_upsert_relation_invalid_ids_returns_io(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    hu_error_t err =
        hu_graph_upsert_relation(g, 99999, 99998, HU_REL_KNOWS, 1.0f, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_IO);

    hu_graph_close(g, &alloc);
}

static void graph_neighbors_returns_adjacent_entities(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0, c = 0;
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_entity(g, "charlie", 7, HU_ENTITY_PERSON, NULL, &c);
    hu_graph_upsert_relation(g, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);
    hu_graph_upsert_relation(g, a, c, HU_REL_KNOWS, 1.0f, NULL, 0);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_graph_neighbors(g, &alloc, a, 1, 10, &entities, &relations, &count);
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
    hu_graph_upsert_entity(g, "isolated", 8, HU_ENTITY_TOPIC, NULL, &a);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_graph_neighbors(g, &alloc, a, 1, 10, &entities, &relations, &count);
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
    hu_error_t err = hu_graph_neighbors(g, &alloc, 99999, 1, 10, &entities,
                                        &relations, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    hu_graph_close(g, &alloc);
}

static void graph_neighbors_null_params_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0;
    hu_graph_upsert_entity(g, "a", 1, HU_ENTITY_PERSON, NULL, &a);

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;

    hu_error_t err =
        hu_graph_neighbors(NULL, &alloc, a, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_graph_neighbors(g, NULL, a, 1, 10, &entities, &relations, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_build_context_with_data_returns_formatted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_relation(g, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_context(g, &alloc, "alice bob", 9, 1, 4096, &out, &out_len);
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
        hu_graph_build_context(g, &alloc, "nonexistent", 11, 1, 4096, &out,
                               &out_len);
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
    hu_error_t err =
        hu_graph_build_context(g, &alloc, NULL, 5, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_build_context_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_context(g, &alloc, "query", 5, 1, 4096, NULL, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

static void graph_build_contact_context_with_contact_prepends_header(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_relation(g, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_contact_context(
        g, &alloc, "alice", 5, "contact_123", 10, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "Knowledge relevant to this contact") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void graph_build_contact_context_empty_contact_returns_plain_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    int64_t a = 0;
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &a);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_graph_build_contact_context(
        g, &alloc, "alice", 5, NULL, 0, 1, 4096, &out, &out_len);
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
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_entity(g, "charlie", 7, HU_ENTITY_PERSON, NULL, &c);
    hu_graph_upsert_relation(g, a, b, HU_REL_KNOWS, 1.0f, NULL, 0);
    hu_graph_upsert_relation(g, b, c, HU_REL_KNOWS, 1.0f, NULL, 0);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_communities(g, &alloc, 10, 4096, &out, &out_len);
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
    hu_error_t err =
        hu_graph_build_communities(g, &alloc, 10, 4096, &out, &out_len);
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
    HU_ASSERT_EQ(hu_entity_type_from_string("organization", 12),
                HU_ENTITY_ORGANIZATION);
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
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_ORGANIZATION),
                     "organization");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_EVENT), "event");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_TOPIC), "topic");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_EMOTION), "emotion");
    HU_ASSERT_STR_EQ(hu_entity_type_to_string(HU_ENTITY_UNKNOWN), "unknown");
}

static void relation_type_from_string_all_types(void) {
    HU_ASSERT_EQ(hu_relation_type_from_string("knows", strlen("knows")),
                 HU_REL_KNOWS);
    HU_ASSERT_EQ(hu_relation_type_from_string("family_of", strlen("family_of")),
                 HU_REL_FAMILY_OF);
    HU_ASSERT_EQ(hu_relation_type_from_string("works_at", strlen("works_at")),
                 HU_REL_WORKS_AT);
    HU_ASSERT_EQ(hu_relation_type_from_string("lives_in", strlen("lives_in")),
                 HU_REL_LIVES_IN);
    HU_ASSERT_EQ(hu_relation_type_from_string("interested_in",
                                              strlen("interested_in")),
                 HU_REL_INTERESTED_IN);
    HU_ASSERT_EQ(hu_relation_type_from_string("discussed_with",
                                              strlen("discussed_with")),
                 HU_REL_DISCUSSED_WITH);
    HU_ASSERT_EQ(hu_relation_type_from_string("feels_about", strlen("feels_about")),
                 HU_REL_FEELS_ABOUT);
    HU_ASSERT_EQ(hu_relation_type_from_string("promised_to", strlen("promised_to")),
                 HU_REL_PROMISED_TO);
    HU_ASSERT_EQ(hu_relation_type_from_string("shared_experience",
                                              strlen("shared_experience")),
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
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_INTERESTED_IN),
                     "interested_in");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_DISCUSSED_WITH),
                     "discussed_with");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_FEELS_ABOUT),
                     "feels_about");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_PROMISED_TO),
                     "promised_to");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_SHARED_EXPERIENCE),
                     "shared_experience");
    HU_ASSERT_STR_EQ(hu_relation_type_to_string(HU_REL_RELATED_TO),
                     "related_to");
}

static void graph_entity_special_chars_in_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    const char *name = "O'Brien";
    int64_t id = 0;
    hu_error_t err =
        hu_graph_upsert_entity(g, name, 7, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, name, 7, &ent);
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
    hu_error_t err =
        hu_graph_upsert_entity(g, name, len, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_graph_entity_t ent;
    err = hu_graph_find_entity(g, name, len, &ent);
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
    hu_graph_upsert_entity(g, "center", 6, HU_ENTITY_PERSON, NULL, &center);
    for (int i = 0; i < 5; i++) {
        char name[16];
        int n = snprintf(name, sizeof(name), "n%d", i);
        int64_t id = 0;
        hu_graph_upsert_entity(g, name, (size_t)n, HU_ENTITY_PERSON, NULL, &id);
        hu_graph_upsert_relation(g, center, id, HU_REL_KNOWS, 1.0f, NULL, 0);
    }

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t count = 0;
    hu_error_t err = hu_graph_neighbors(g, &alloc, center, 1, 2, &entities,
                                        &relations, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);

    hu_graph_entities_free(&alloc, entities, count);
    hu_graph_relations_free(&alloc, relations, count);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_entity_null_graph_returns_error(void) {
    int64_t id = 0;
    hu_error_t err =
        hu_graph_upsert_entity(NULL, "alice", 5, HU_ENTITY_PERSON, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_find_entity_null_graph_returns_error(void) {
    hu_graph_entity_t ent;
    hu_error_t err = hu_graph_find_entity(NULL, "alice", 5, &ent);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_upsert_relation_null_graph_returns_error(void) {
    hu_error_t err =
        hu_graph_upsert_relation(NULL, 1, 2, HU_REL_KNOWS, 1.0f, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void graph_build_context_null_graph_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_context(NULL, &alloc, "q", 1, 1, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
}

static void graph_build_communities_null_params_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_graph_open(&alloc, "x", 1, &g);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_graph_build_communities(NULL, &alloc, 10, 4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

void run_graph_tests(void) {
    HU_TEST_SUITE("graph");
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
}

#else

void run_graph_tests(void) {
    HU_TEST_SUITE("graph");
}

#endif /* HU_ENABLE_SQLITE */
