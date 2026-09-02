/* Contract C3 (SOTA fleet) — the daemon's own outbound text becomes
 * first-class facts with provenance. hu_agent_facts_record_reply runs the
 * regex fact extractor + commitment detector against the daemon's OWN
 * reply, so the graph stops treating h-uman's output as Seth's and the
 * daemon can recall its own commitments later. */
#ifdef HU_ENABLE_SQLITE
#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/daemon/message_router.h"
#include "human/memory.h"
#include "human/memory/agent_facts.h"
#include "human/memory/graph.h"
#include "human/memory/graph_ingest.h"
#include "test_framework.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_graph_t *open_tmp_graph(hu_allocator_t *alloc, char *path, size_t cap) {
    snprintf(path, cap, "/tmp/hu_agent_facts_%d_%d.db", (int)getpid(), rand());
    unlink(path);
    hu_graph_t *g = NULL;
    if (hu_graph_open(alloc, path, strlen(path), &g) != HU_OK)
        return NULL;
    return g;
}

static int count_rows(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

static void set_gate(const char *mode) {
    if (mode && mode[0])
        setenv("HU_AGENT_FACTS", mode, 1);
    else
        unsetenv("HU_AGENT_FACTS");
}

/* ── OFF: no writes anywhere ─────────────────────────────────────────── */

static void agent_facts_off_mode_stores_nothing(void) {
    set_gate("off");
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char reply[] = "I'll send you the contractor's number tomorrow";
    HU_ASSERT_EQ(hu_agent_facts_record_reply(g, &mem, "contact_off", 11, reply, sizeof(reply) - 1,
                                             "msg-off", 1000),
                 HU_OK);

    sqlite3 *mdb = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(count_rows(mdb, "SELECT COUNT(*) FROM memories WHERE key LIKE 'agent-promise:%'"),
                 0);
    sqlite3 *gdb = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_rows(gdb, "SELECT COUNT(*) FROM relations WHERE provenance LIKE 'agent:%'"),
                 0);

    mem.vtable->deinit(mem.ctx);
    hu_graph_close(g, &alloc);
    unlink(path);
}

/* Also: OFF is a no-op even with NULL graph/memory (contract: default OFF
 * costs nothing and touches nothing). */
static void agent_facts_off_mode_tolerates_null_stores(void) {
    set_gate(NULL); /* unset -> hu_gate_mode_from_env's unset_default = OFF */
    static const char reply[] = "I'll call you tomorrow";
    HU_ASSERT_EQ(
        hu_agent_facts_record_reply(NULL, NULL, "c", 1, reply, sizeof(reply) - 1, "m", 1000),
        HU_OK);
}

/* ── LIVE: graph fact + promise row, both provenance-tagged ─────────────── */

static void agent_facts_live_stores_promise_and_agent_provenance_fact(void) {
    set_gate("on");
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* "i work at " triggers the regex extractor (subject relabelled
     * "assistant"); "i'll" triggers commitment detection. One reply
     * exercises both halves of the contract. */
    static const char reply[] = "I work at Acme. I'll send you the contractor's number tomorrow.";
    HU_ASSERT_EQ(hu_agent_facts_record_reply(g, &mem, "contact_live", 12, reply, sizeof(reply) - 1,
                                             "msg-live-1", 5000),
                 HU_OK);

    sqlite3 *gdb = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(
        count_rows(gdb, "SELECT COUNT(*) FROM relations WHERE provenance = 'agent:msg-live-1'"), 1);
    /* The entity graph records "assistant" as the subject, not "user" —
     * that's the whole point of the provenance split. */
    hu_graph_entity_t assistant_ent;
    memset(&assistant_ent, 0, sizeof(assistant_ent));
    HU_ASSERT_EQ(hu_graph_find_entity(g, "contact_live", 12, "assistant", 9, &assistant_ent),
                 HU_OK);

    sqlite3 *mdb = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(count_rows(mdb, "SELECT COUNT(*) FROM memories WHERE key LIKE "
                                 "'agent-promise:contact_live:%' AND category='core'"),
                 1);
    HU_ASSERT_EQ(count_rows(mdb, "SELECT COUNT(*) FROM memories WHERE content LIKE "
                                 "'%contractor%number%tomorrow%'"),
                 1);

    mem.vtable->deinit(mem.ctx);
    hu_graph_close(g, &alloc);
    unlink(path);
}

/* ── SHADOW: nothing written, but the call still succeeds ───────────────── */

static void agent_facts_shadow_mode_writes_nothing(void) {
    set_gate("shadow");
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char reply[] = "I work at Acme. I'll send it tomorrow.";
    HU_ASSERT_EQ(hu_agent_facts_record_reply(g, &mem, "contact_shadow", 14, reply,
                                             sizeof(reply) - 1, "msg-shadow", 6000),
                 HU_OK);

    sqlite3 *gdb = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_rows(gdb, "SELECT COUNT(*) FROM relations"), 0);
    sqlite3 *mdb = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(count_rows(mdb, "SELECT COUNT(*) FROM memories"), 0);

    mem.vtable->deinit(mem.ctx);
    hu_graph_close(g, &alloc);
    unlink(path);
}

/* ── The supersession guard: an agent-sourced write must not close a
 * Seth-sourced edge of the same (contact, subject, predicate). ────────── */

static void agent_provenance_does_not_supersede_seth_sourced_edge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);

    /* Seth-sourced: a real observation from chat.db. */
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "contact_sup", 11, "user", "lives_in", "st pete", 0.9f,
                                      100, "chat.db:1"),
                 HU_OK);
    /* Same (contact, subject, predicate) but agent-sourced — must NOT
     * supersede the Seth-sourced edge above, even though it targets a
     * different object. Without the graph.c guard this closes the Seth
     * edge at event_end=200, which is exactly the bug this contract
     * exists to prevent (h-uman's own paraphrase overwriting Seth's
     * ground truth). */
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "contact_sup", 11, "user", "lives_in",
                                      "somewhere the daemon guessed", 0.5f, 200, "agent:msg-sup-1"),
                 HU_OK);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    /* At t=250 (after both writes), the Seth-sourced edge must still be
     * OPEN — i.e. present in the window. */
    HU_ASSERT_EQ(
        hu_graph_relations_in_window(g, &alloc, "contact_sup", 11, 250, 250, 32, &rels, &n), HU_OK);
    bool seth_edge_open = false;
    size_t lives_in_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (rels[i].type != HU_REL_LIVES_IN)
            continue;
        lives_in_count++;
        if (rels[i].provenance && strcmp(rels[i].provenance, "chat.db:1") == 0)
            seth_edge_open = true;
    }
    HU_ASSERT_TRUE(seth_edge_open);
    /* BRANCH, not SUPERSEDE: both edges coexist (the agent one is stored
     * for recall, but doesn't erase Seth's). */
    HU_ASSERT_EQ((long)lives_in_count, 2L);
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}

/* A second Seth-sourced write (no "agent:" prefix) must still supersede
 * normally — the guard is scoped to agent provenance only, not a general
 * "never supersede lives_in" regression. */
static void non_agent_provenance_still_supersedes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);

    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "contact_reg", 11, "user", "lives_in", "king of prussia",
                                      0.9f, 100, "chat.db:1"),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "contact_reg", 11, "user", "lives_in", "st pete", 0.9f,
                                      200, "chat.db:2"),
                 HU_OK);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(
        hu_graph_relations_in_window(g, &alloc, "contact_reg", 11, 250, 250, 32, &rels, &n), HU_OK);
    size_t lives_in_count = 0;
    for (size_t i = 0; i < n; i++)
        if (rels[i].type == HU_REL_LIVES_IN)
            lives_in_count++;
    HU_ASSERT_EQ((long)lives_in_count, 1L); /* superseded down to one open edge */
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}

/* ── hu_agent_facts_dry_run — the CLI hook's underlying primitive ───────── */

static void dry_run_relabels_subject_and_finds_commitment(void) {
    static const char reply[] = "I work at Acme. I'll send you the contractor's number tomorrow.";
    hu_fact_extract_result_t facts;
    char commitment[512];
    char who[64];
    bool has_commitment = false;
    HU_ASSERT_EQ(hu_agent_facts_dry_run(reply, sizeof(reply) - 1, &facts, commitment,
                                        sizeof(commitment), who, sizeof(who), &has_commitment),
                 HU_OK);
    HU_ASSERT_TRUE(facts.fact_count >= 1);
    HU_ASSERT_EQ(strcmp(facts.facts[0].subject, "assistant"), 0);
    HU_ASSERT_TRUE(has_commitment);
    HU_ASSERT_EQ(strcmp(who, "me"), 0);
}

static void dry_run_rejects_invalid_arguments(void) {
    hu_fact_extract_result_t facts;
    HU_ASSERT_EQ(hu_agent_facts_dry_run(NULL, 0, &facts, NULL, 0, NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_agent_facts_dry_run("x", 1, NULL, NULL, 0, NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Router path: runs even with reaction collection OFF ─────────────── */
/* Production has reaction_collection.enabled=false. The first C3 wiring sat
 * behind that early return inside hu_daemon_register_reply_for_reactions and
 * never ran on a real reply. This drives the ROUTER entry point, not the
 * agent_facts function directly, with reaction collection off. */
static void agent_facts_router_path_runs_with_reaction_collection_off(void) {
    set_gate("on");
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_config_t *cfg = calloc(1, sizeof(*cfg));
    hu_agent_t *agent = calloc(1, sizeof(*agent));
    HU_ASSERT_NOT_NULL(cfg);
    HU_ASSERT_NOT_NULL(agent);
    HU_ASSERT_FALSE(cfg->reaction_collection.enabled); /* the production state */
    agent->verifier_graph = g;
    agent->memory = &mem;

    sqlite3 *gdb = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_rows(gdb, "SELECT COUNT(*) FROM relations WHERE provenance LIKE 'agent:%'"),
                 0); /* precondition */

    static const char reply[] = "I work at Acme. I'll send you the contractor's number tomorrow.";
    char ref[96];
    hu_daemon_register_reply_for_reactions(cfg, agent, "imessage", "contact_router", "prompt",
                                           reply, sizeof(reply) - 1, ref, sizeof(ref));

    HU_ASSERT_TRUE(
        count_rows(gdb, "SELECT COUNT(*) FROM relations WHERE provenance LIKE 'agent:%'") > 0);
    sqlite3 *mdb = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(count_rows(mdb, "SELECT COUNT(*) FROM memories WHERE key LIKE 'agent-promise:%'"),
                 1);

    free(agent);
    free(cfg);
    mem.vtable->deinit(mem.ctx);
    hu_graph_close(g, &alloc);
    unlink(path);
    set_gate(NULL);
}

void run_agent_facts_tests(void) {
    HU_TEST_SUITE("agent_facts");
    HU_RUN_TEST(agent_facts_off_mode_stores_nothing);
    HU_RUN_TEST(agent_facts_off_mode_tolerates_null_stores);
    HU_RUN_TEST(agent_facts_live_stores_promise_and_agent_provenance_fact);
    HU_RUN_TEST(agent_facts_shadow_mode_writes_nothing);
    HU_RUN_TEST(agent_facts_router_path_runs_with_reaction_collection_off);
    HU_RUN_TEST(agent_provenance_does_not_supersede_seth_sourced_edge);
    HU_RUN_TEST(non_agent_provenance_still_supersedes);
    HU_RUN_TEST(dry_run_relabels_subject_and_finds_commitment);
    HU_RUN_TEST(dry_run_rejects_invalid_arguments);
    unsetenv("HU_AGENT_FACTS");
}
#else
void run_agent_facts_tests(void) {
    (void)0;
}
#endif
