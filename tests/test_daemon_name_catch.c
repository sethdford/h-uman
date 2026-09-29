/* Per-turn name catcher wiring (spec 2026-09-29 §4.2): HU_NAME_CATCH gate,
 * pure write decision, graph effects per mode, and the daemon call site
 * feeding the contact's inbound text only. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/daemon/name_catch.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_name_catch_action_truth_table(void) {
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_OFF, HU_NAME_KNOWN), (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_OFF, HU_NAME_CAPITALIZED),
                 (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_SHADOW, HU_NAME_KNOWN), (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_SHADOW, HU_NAME_CAPITALIZED),
                 (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_LIVE, HU_NAME_KNOWN), (int)HU_NAME_CATCH_BUMP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_LIVE, HU_NAME_CAPITALIZED),
                 (int)HU_NAME_CATCH_INSERT);
}

static void test_name_catch_mode_defaults_off(void) {
    unsetenv("HU_NAME_CATCH");
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_OFF);
    setenv("HU_NAME_CATCH", "shadow", 1);
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_NAME_CATCH", "live", 1);
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_LIVE);
    setenv("HU_NAME_CATCH", "garbage", 1);
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_NAME_CATCH");
}

/* M4 + group skip: the catcher never reads a group chat or Seth's own
 * self-chat (the configured loopback handle). */
static void test_eligible_skips_group_and_self_chat(void) {
    static hu_config_t cfg; /* zeroed */
    hu_channel_loop_msg_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.session_key, sizeof(m.session_key), "%s", "+15550001111");
    HU_ASSERT_TRUE(hu_name_catch_eligible(&m, NULL));
    HU_ASSERT_TRUE(hu_name_catch_eligible(&m, &cfg)); /* no self handle configured */
    char self[] = "+15550009999";
    cfg.channels.imessage.loopback_handle = self;
    HU_ASSERT_TRUE(hu_name_catch_eligible(&m, &cfg));
    snprintf(m.session_key, sizeof(m.session_key), "%s", self);
    HU_ASSERT_FALSE(hu_name_catch_eligible(&m, &cfg)); /* Seth's own text */
    snprintf(m.session_key, sizeof(m.session_key), "%s", "+15550001111");
    m.is_group = true;
    HU_ASSERT_FALSE(hu_name_catch_eligible(&m, &cfg));
    HU_ASSERT_FALSE(hu_name_catch_eligible(&m, NULL));
    HU_ASSERT_FALSE(hu_name_catch_eligible(NULL, &cfg));
    cfg.channels.imessage.loopback_handle = NULL;
}

static void test_bad_args_are_rejected_and_counts_zeroed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_name_catch_counts_t c = {9, 9, 9};
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, NULL, HU_GATE_LIVE, "c", 1, "hi Ann", 6, &c),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((long)(c.known + c.fresh + c.written), 0L);
}

/* The call site is compiled out of test builds (HU_IS_TEST), so pin it by
 * source: the catcher is fed each raw inbound message, never the reply
 * (no self-reinforcing hallucination). Source-presence style, like
 * test_gate_comment_exists_at_agent_turn_1471. */
static void test_daemon_feeds_the_catcher_inbound_text_only(void) {
    FILE *f = fopen("src/daemon.c", "r");
    HU_ASSERT_NOT_NULL(f);
    char line[512], call[1024] = {0};
    bool in_call = false, gated = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "hu_name_catch_eligible(&msgs[batch_start], config)") != NULL)
            gated = true;
        if (!in_call && strstr(line, "hu_daemon_name_catch_tick(") != NULL)
            in_call = true;
        if (in_call) {
            strncat(call, line, sizeof(call) - strlen(call) - 1);
            if (strchr(line, ';') != NULL)
                break;
        }
    }
    fclose(f);
    HU_ASSERT_STR_CONTAINS(call, "msgs[b].content");
    HU_ASSERT_STR_NOT_CONTAINS(call, "response");
    HU_ASSERT_STR_NOT_CONTAINS(call, "combined");
    HU_ASSERT_TRUE(gated); /* group chats and self-chat never reach the catcher */
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#define NC_CID "+15550001111"

typedef struct nc_row {
    bool found;
    int type;
    int mentions;
    char provenance[32];
    double confidence;
} nc_row_t;

static nc_row_t nc_row(hu_graph_t *g, const char *cid, const char *name) {
    nc_row_t r;
    memset(&r, 0, sizeof(r));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(hu_graph_sqlite_connection(g),
                           "SELECT type, mention_count, COALESCE(provenance, ''), confidence"
                           " FROM entities WHERE contact_id = ?1 AND name = ?2",
                           -1, &q, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_text(q, 1, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        r.found = true;
        r.type = sqlite3_column_int(q, 0);
        r.mentions = sqlite3_column_int(q, 1);
        snprintf(r.provenance, sizeof(r.provenance), "%s", (const char *)sqlite3_column_text(q, 2));
        r.confidence = sqlite3_column_double(q, 3);
    }
    sqlite3_finalize(q);
    return r;
}

/* NC_CID knows Salim (PERSON), tampa (lowercase PLACE) and Pickleball (TOPIC). */
static hu_graph_t *nc_graph(hu_allocator_t *alloc) {
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(alloc, ":memory:", 8, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, NC_CID, strlen(NC_CID), "Salim", 5, HU_ENTITY_PERSON, NULL, &id),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, NC_CID, strlen(NC_CID), "tampa", 5, HU_ENTITY_PLACE, NULL, &id),
        HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, NC_CID, strlen(NC_CID), "Pickleball", 10,
                                        HU_ENTITY_TOPIC, NULL, &id),
                 HU_OK);
    return g;
}

static const char k_text[] = "saw Salim and Priya back in tampa";

static void test_live_bumps_known_and_inserts_new(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), k_text,
                                      strlen(k_text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 2L); /* Salim, tampa */
    HU_ASSERT_EQ((long)c.fresh, 1L); /* Priya */
    HU_ASSERT_EQ((long)c.written, 3L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Salim").mentions, 2);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Salim").type, (int)HU_ENTITY_PERSON); /* bump never retypes */
    HU_ASSERT_EQ(nc_row(g, NC_CID, "tampa").mentions, 2);
    nc_row_t p = nc_row(g, NC_CID, "Priya");
    HU_ASSERT_TRUE(p.found);
    HU_ASSERT_EQ(p.type, (int)HU_ENTITY_UNKNOWN);
    HU_ASSERT_STR_EQ(p.provenance, "names:turn");
    HU_ASSERT_FLOAT_EQ(p.confidence, 0.3, 1e-6);
    hu_graph_close(g, &alloc);
}

static void test_shadow_counts_but_writes_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_SHADOW, NC_CID, strlen(NC_CID), k_text,
                                      strlen(k_text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 2L);
    HU_ASSERT_EQ((long)c.fresh, 1L);
    HU_ASSERT_EQ((long)c.written, 0L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Salim").mentions, 1);
    HU_ASSERT_FALSE(nc_row(g, NC_CID, "Priya").found);
    hu_graph_close(g, &alloc);
}

static void test_off_does_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_OFF, NC_CID, strlen(NC_CID), k_text,
                                      strlen(k_text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)(c.known + c.fresh + c.written), 0L);
    HU_ASSERT_FALSE(nc_row(g, NC_CID, "Priya").found);
    hu_graph_close(g, &alloc);
}

static void test_topic_names_are_not_known(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    const char *text = "more pickleball tonight";
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), text,
                                      strlen(text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 0L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Pickleball").mentions, 1);
    hu_graph_close(g, &alloc);
}

/* Review Focus 4: a contact with no graph yet still gets their names recorded. */
static void test_first_message_from_empty_contact_records_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, ":memory:", 8, &g), HU_OK);
    hu_name_catch_counts_t c;
    const char *cid = "+15550002222", *text = "dinner with Priya";
    HU_ASSERT_EQ(
        hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, cid, strlen(cid), text, strlen(text), &c),
        HU_OK);
    HU_ASSERT_EQ((long)c.fresh, 1L);
    HU_ASSERT_EQ((long)c.written, 1L);
    HU_ASSERT_TRUE(nc_row(g, cid, "Priya").found);
    hu_graph_close(g, &alloc);
}

/* I3(c): a name the catcher recorded itself (UNKNOWN, names:turn) is KNOWN
 * only in its own spelling until the nightly pass types it; then any casing. */
static void test_unconfirmed_caught_name_matches_case_sensitively(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    const char *first = "dinner with Priya", *lower = "saw priya today";
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), first,
                                      strlen(first), &c),
                 HU_OK);
    nc_row_t caught = nc_row(g, NC_CID, "Priya");
    HU_ASSERT_STR_EQ(caught.provenance, "names:turn");
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), lower,
                                      strlen(lower), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 0L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Priya").mentions, 1); /* "priya" did not bump it */
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, NC_CID, strlen(NC_CID), "Priya", 5,
                                              HU_ENTITY_PERSON, "names:nightly", 0.8f,
                                              HU_GRAPH_UPSERT_NO_TOUCH, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), lower,
                                      strlen(lower), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 1L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Priya").mentions, 2);
    hu_graph_close(g, &alloc);
}

/* M3 through the catcher: a Capitalized word matching a legacy TOPIC row is
 * bumped but never relabelled names:turn (the retype to UNKNOWN is refused). */
static void test_catcher_does_not_stamp_a_legacy_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    const char *text = "more Pickleball tonight";
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), text,
                                      strlen(text), &c),
                 HU_OK);
    nc_row_t r = nc_row(g, NC_CID, "Pickleball");
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_TOPIC);
    HU_ASSERT_EQ(r.mentions, 2);
    HU_ASSERT_STR_EQ(r.provenance, "");
    hu_graph_close(g, &alloc);
}

static void test_tick_follows_the_env_gate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    const char *text = "dinner with Priya";
    unsetenv("HU_NAME_CATCH");
    hu_daemon_name_catch_tick(&alloc, g, NC_CID, strlen(NC_CID), text, strlen(text));
    HU_ASSERT_FALSE(nc_row(g, NC_CID, "Priya").found);
    setenv("HU_NAME_CATCH", "live", 1);
    hu_daemon_name_catch_tick(&alloc, g, NC_CID, strlen(NC_CID), text, strlen(text));
    unsetenv("HU_NAME_CATCH");
    HU_ASSERT_TRUE(nc_row(g, NC_CID, "Priya").found);
    hu_graph_close(g, &alloc);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_name_catch_tests(void) {
    HU_TEST_SUITE("daemon_name_catch");
    HU_RUN_TEST(test_name_catch_action_truth_table);
    HU_RUN_TEST(test_name_catch_mode_defaults_off);
    HU_RUN_TEST(test_eligible_skips_group_and_self_chat);
    HU_RUN_TEST(test_bad_args_are_rejected_and_counts_zeroed);
    HU_RUN_TEST(test_daemon_feeds_the_catcher_inbound_text_only);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_live_bumps_known_and_inserts_new);
    HU_RUN_TEST(test_shadow_counts_but_writes_nothing);
    HU_RUN_TEST(test_off_does_nothing);
    HU_RUN_TEST(test_topic_names_are_not_known);
    HU_RUN_TEST(test_first_message_from_empty_contact_records_names);
    HU_RUN_TEST(test_unconfirmed_caught_name_matches_case_sensitively);
    HU_RUN_TEST(test_catcher_does_not_stamp_a_legacy_row);
    HU_RUN_TEST(test_tick_follows_the_env_gate);
#endif
}
