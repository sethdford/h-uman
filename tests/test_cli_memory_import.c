/* Task 4 — `human memory import-facts` feeds the grounding graph through the
 * superseding ingest path, in timestamp order. Exercises the real production
 * subcommand (cmd_memory) against a temp graph via HU_GRAPH_DB. */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/graph_ingest.h"
#include "test_framework.h"
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
    size_t imported = 0, skipped = 0;
    HU_ASSERT_EQ(hu_graph_import_facts_jsonl(&alloc, g, jpath, "asking_about", &imported, &skipped),
                 HU_OK);
    HU_ASSERT_EQ((long)imported, 3L);
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

void run_cli_memory_import_tests(void) {
    HU_TEST_SUITE("cli_memory_import");
    HU_RUN_TEST(test_import_facts_ingests_in_ts_order_and_supersedes);
    HU_RUN_TEST(test_import_facts_empty_file_is_not_success);
}
#else
void run_cli_memory_import_tests(void) {
    (void)0;
}
#endif
