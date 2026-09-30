/* Task 4 — `human memory import-facts` feeds the grounding graph through the
 * superseding ingest path, in timestamp order. Exercises the real production
 * subcommand (cmd_memory) against a temp graph via HU_GRAPH_DB. */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/graph_ingest.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

hu_error_t cmd_memory(hu_allocator_t *alloc, int argc, char **argv);

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(body, f);
        fclose(f);
    }
}

static size_t open_edges_of_type(hu_allocator_t *alloc, hu_graph_t *g, hu_relation_type_t t,
                                 int64_t at) {
    hu_graph_relation_t *rels = NULL;
    size_t n = 0, hits = 0;
    if (hu_graph_relations_in_window(g, alloc, "self", 4, at, at, 64, &rels, &n) != HU_OK)
        return (size_t)-1;
    for (size_t i = 0; i < n; i++)
        if (rels[i].type == t)
            hits++;
    hu_graph_relations_free(alloc, rels, n);
    return hits;
}

static void test_import_facts_ingests_in_ts_order_and_supersedes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char jpath[128];
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_%d.jsonl", (int)getpid());
    /* Deliberately OUT of order in the file: the newer lives_in comes first. */
    write_file(jpath,
               "{\"contact\":\"self\",\"subject\":\"user\",\"predicate\":\"lives_in\",\"object\":"
               "\"st pete\",\"confidence\":0.9,\"ts\":200,\"source\":\"t:2\"}\n"
               "{\"contact\":\"self\",\"subject\":\"user\",\"predicate\":\"lives_in\",\"object\":"
               "\"king of prussia\",\"confidence\":0.9,\"ts\":100,\"source\":\"t:1\"}\n"
               "{\"contact\":\"self\",\"subject\":\"user\",\"predicate\":\"works_at\",\"object\":"
               "\"acme\",\"confidence\":0.8,\"ts\":150,\"source\":\"t:3\"}\n"
               "{\"contact\":\"self\",\"subject\":\"user\",\"predicate\":\"asking_about\","
               "\"object\":\"weather\",\"confidence\":0.5,\"ts\":160,\"source\":\"t:4\"}\n");
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "ignored-in-test", 15, &g), HU_OK);
    size_t imported = 0, entities = 0, skipped = 0;
    HU_ASSERT_EQ(hu_graph_import_facts_jsonl(&alloc, g, jpath, "asking_about", &imported, &entities,
                                             &skipped),
                 HU_OK);
    HU_ASSERT_EQ((long)imported, 3L);
    HU_ASSERT_EQ((long)entities, 0L);
    HU_ASSERT_EQ((long)skipped, 1L);
    /* At t=250 exactly one open LIVES_IN (KoP closed at 200), one WORKS_AT,
     * and the excluded predicate never became an edge. */
    HU_ASSERT_EQ((long)open_edges_of_type(&alloc, g, HU_REL_LIVES_IN, 250), 1L);
    HU_ASSERT_EQ((long)open_edges_of_type(&alloc, g, HU_REL_WORKS_AT, 250), 1L);
    HU_ASSERT_EQ((long)open_edges_of_type(&alloc, g, HU_REL_RELATED_TO, 250), 0L);
    /* And at t=150 the old place was still the truth. */
    HU_ASSERT_EQ((long)open_edges_of_type(&alloc, g, HU_REL_LIVES_IN, 150), 1L);
    hu_graph_close(g, &alloc);
    unlink(jpath);
}

static void test_import_facts_empty_file_is_not_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char gpath[128], jpath[128];
    snprintf(gpath, sizeof(gpath), "/tmp/hu_cli_import_e_%d.db", (int)getpid());
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_e_%d.jsonl", (int)getpid());
    unlink(gpath);
    setenv("HU_GRAPH_DB", gpath, 1);
    write_file(jpath, "\n");
    char *argv[] = {"human", "memory", "import-facts", jpath, NULL};
    HU_ASSERT_NEQ(cmd_memory(&alloc, 4, argv), HU_OK);
    unlink(gpath);
    unlink(jpath);
    unsetenv("HU_GRAPH_DB");
}

static void test_ground_refuses_a_missing_graph_without_creating_it(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char gpath[128];
    snprintf(gpath, sizeof(gpath), "/tmp/hu_cli_ground_missing_%d.db", (int)getpid());
    unlink(gpath);
    setenv("HU_GRAPH_DB", gpath, 1);
    char *argv[] = {"human", "memory", "ground", "--full", "+15550000001", "did Salim call", NULL};
    HU_ASSERT_EQ(cmd_memory(&alloc, 6, argv), HU_ERR_NOT_FOUND);
    HU_ASSERT_TRUE(access(gpath, F_OK) != 0); /* nothing was created */
    unsetenv("HU_GRAPH_DB");
}

typedef struct ent_row {
    bool found;
    int type;
    int mentions;
    char provenance[64];
} ent_row_t;

static ent_row_t ent_row(hu_graph_t *g, const char *cid, const char *name) {
    ent_row_t r;
    memset(&r, 0, sizeof(r));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(hu_graph_sqlite_connection(g),
                           "SELECT type, mention_count, COALESCE(provenance, '') FROM entities"
                           " WHERE contact_id = ?1 AND name = ?2",
                           -1, &q, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_text(q, 1, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        r.found = true;
        r.type = sqlite3_column_int(q, 0);
        r.mentions = sqlite3_column_int(q, 1);
        snprintf(r.provenance, sizeof(r.provenance), "%s", (const char *)sqlite3_column_text(q, 2));
    }
    sqlite3_finalize(q);
    return r;
}

#define ENT_CID "+15550000001"

/* Entity lines (spec §4.3) are typed through hu_graph_upsert_entity_typed and
 * counted apart from facts; a bad type, a missing contact and a placeholder
 * name are skipped, never guessed. */
static void test_import_entity_lines_are_typed_and_counted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char jpath[128];
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_ent_%d.jsonl", (int)getpid());
    write_file(jpath, "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Salim\","
                      "\"type\":\"person\",\"source\":\"names:nightly\",\"confidence\":0.8}\n"
                      "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"lake house\","
                      "\"type\":\"topic\",\"source\":\"names:nightly\",\"confidence\":0.8}\n"
                      "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Jupiter\","
                      "\"type\":\"planet\"}\n"
                      "{\"kind\":\"entity\",\"name\":\"Nobody\",\"type\":\"person\"}\n"
                      "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"user\","
                      "\"type\":\"person\"}\n"
                      "{\"contact\":\"self\",\"subject\":\"user\",\"predicate\":\"works_at\","
                      "\"object\":\"acme\",\"confidence\":0.8,\"ts\":150,\"source\":\"t:3\"}\n");
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "ignored-in-test", 15, &g), HU_OK);
    size_t imported = 0, entities = 0, skipped = 0;
    HU_ASSERT_EQ(
        hu_graph_import_facts_jsonl(&alloc, g, jpath, NULL, &imported, &entities, &skipped), HU_OK);
    HU_ASSERT_EQ((long)imported, 1L);
    HU_ASSERT_EQ((long)entities, 2L);
    HU_ASSERT_EQ((long)skipped, 3L); /* planet, missing contact, placeholder "user" */
    ent_row_t salim = ent_row(g, ENT_CID, "Salim");
    HU_ASSERT_TRUE(salim.found);
    HU_ASSERT_EQ(salim.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_STR_EQ(salim.provenance, "names:nightly");
    HU_ASSERT_EQ(ent_row(g, ENT_CID, "lake house").type, (int)HU_ENTITY_TOPIC);
    HU_ASSERT_FALSE(ent_row(g, ENT_CID, "Jupiter").found);
    HU_ASSERT_FALSE(ent_row(g, ENT_CID, "user").found);
    hu_graph_close(g, &alloc);
    unlink(jpath);
}

/* "retype_only": the migration types what exists and creates nothing. */
static void test_import_retype_only_line_retypes_without_touching_or_creating(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char jpath[128];
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_rt_%d.jsonl", (int)getpid());
    write_file(jpath, "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Vanguard\","
                      "\"type\":\"org\",\"source\":\"names:migrate\",\"confidence\":0.6,"
                      "\"retype_only\":true}\n"
                      "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Ghost\","
                      "\"type\":\"person\",\"source\":\"names:migrate\",\"retype_only\":true}\n");
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "ignored-in-test", 15, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, ENT_CID, strlen(ENT_CID), "Vanguard", 8,
                                        HU_ENTITY_UNKNOWN, NULL, &id),
                 HU_OK);
    size_t imported = 0, entities = 0, skipped = 0;
    HU_ASSERT_EQ(
        hu_graph_import_facts_jsonl(&alloc, g, jpath, NULL, &imported, &entities, &skipped), HU_OK);
    HU_ASSERT_EQ((long)entities, 1L);
    HU_ASSERT_EQ((long)skipped, 1L);
    ent_row_t v = ent_row(g, ENT_CID, "Vanguard");
    HU_ASSERT_EQ(v.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(v.mentions, 1);
    HU_ASSERT_STR_EQ(v.provenance, "names:migrate");
    HU_ASSERT_FALSE(ent_row(g, ENT_CID, "Ghost").found);
    hu_graph_close(g, &alloc);
    unlink(jpath);
}

/* Success iff N+E > 0, through the real subcommand: an entity-only file is a
 * real import; a file whose only line is invalid is not. */
static void test_import_entity_only_file_is_success_through_the_cli(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char gpath[128], jpath[128];
    snprintf(gpath, sizeof(gpath), "/tmp/hu_cli_import_eo_%d.db", (int)getpid());
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_eo_%d.jsonl", (int)getpid());
    setenv("HU_GRAPH_DB", gpath, 1);
    char *argv[] = {"human", "memory", "import-facts", jpath, NULL};
    write_file(jpath, "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Salim\","
                      "\"type\":\"person\"}\n");
    HU_ASSERT_EQ(cmd_memory(&alloc, 4, argv), HU_OK);
    write_file(jpath, "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Salim\","
                      "\"type\":\"planet\"}\n");
    HU_ASSERT_NEQ(cmd_memory(&alloc, 4, argv), HU_OK);
    unlink(gpath);
    unlink(jpath);
    unsetenv("HU_GRAPH_DB");
}

void run_cli_memory_import_tests(void) {
    HU_TEST_SUITE("cli_memory_import");
    HU_RUN_TEST(test_import_facts_ingests_in_ts_order_and_supersedes);
    HU_RUN_TEST(test_import_facts_empty_file_is_not_success);
    HU_RUN_TEST(test_import_entity_lines_are_typed_and_counted);
    HU_RUN_TEST(test_import_retype_only_line_retypes_without_touching_or_creating);
    HU_RUN_TEST(test_import_entity_only_file_is_success_through_the_cli);
    HU_RUN_TEST(test_ground_refuses_a_missing_graph_without_creating_it);
}
#else
void run_cli_memory_import_tests(void) {
    (void)0;
}
#endif
