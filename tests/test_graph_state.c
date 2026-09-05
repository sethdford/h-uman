/* State-first read view over bitemporal relations (src/memory/graph_state.c).
 * Pure resolver tests: no DB. The graph-backed contract (superseded rows
 * never rendered as current by the grounding composer) lives in
 * tests/test_graph_grounding.c. */

#include "human/core/allocator.h"
#include "human/memory/graph_state.h"
#include "test_framework.h"

#include <string.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *sys_alloc(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

static hu_graph_relation_t rel(int64_t id, int64_t src, int64_t tgt, hu_relation_type_t type,
                               int64_t start, int64_t end, int64_t supersedes) {
    hu_graph_relation_t r;
    memset(&r, 0, sizeof(r));
    r.id = id;
    r.source_id = src;
    r.target_id = tgt;
    r.type = type;
    r.event_start = start;
    r.event_end = end;
    r.supersedes_id = supersedes;
    return r;
}

#define T_2024 1704067200000LL /* 2024-01-01 */
#define T_2025 1735689600000LL /* 2025-01-01 */
#define T_2026 1767225600000LL /* 2026-01-01 */
#define T_NOW  1788000000000LL /* 2026-08-30 */

static void test_state_is_current_honors_event_window(void) {
    hu_graph_relation_t open = rel(1, 1, 2, HU_REL_WORKS_AT, T_2025, 0, 0);
    hu_graph_relation_t closed = rel(2, 1, 3, HU_REL_WORKS_AT, T_2024, T_2025, 0);
    hu_graph_relation_t unknown_start = rel(3, 1, 4, HU_REL_KNOWS, 0, 0, 0);
    HU_ASSERT_TRUE(hu_graph_state_is_current(&open, T_NOW));
    HU_ASSERT_FALSE(hu_graph_state_is_current(&closed, T_NOW));
    HU_ASSERT_TRUE(hu_graph_state_is_current(&closed, T_2024 + 1)); /* held back then */
    HU_ASSERT_FALSE(hu_graph_state_is_current(&closed, T_2025));    /* cutover excluded */
    HU_ASSERT_FALSE(hu_graph_state_is_current(&open, T_2024));      /* not yet true */
    HU_ASSERT_TRUE(hu_graph_state_is_current(&unknown_start, T_2024));
    HU_ASSERT_FALSE(hu_graph_state_is_current(NULL, T_NOW));
}

/* user works_at Acme(2024, ended 2025) -> Globex(2025, ended 2026) -> Initech(2026, open).
 * Only Initech is current; its prev is Globex via supersedes_id. */
static void test_state_resolve_single_valued_keeps_only_open_head(void) {
    hu_graph_relation_t rels[3] = {
        rel(10, 1, 100, HU_REL_WORKS_AT, T_2024, T_2025, 0),
        rel(11, 1, 101, HU_REL_WORKS_AT, T_2025, T_2026, 10),
        rel(12, 1, 102, HU_REL_WORKS_AT, T_2026, 0, 11),
    };
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 3, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_TRUE(out[0].current);
    HU_ASSERT_EQ(out[0].rel->id, 12);
    HU_ASSERT_NOT_NULL(out[0].prev);
    HU_ASSERT_EQ(out[0].prev->id, 11);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

/* Same chain read AS OF mid-2025: Globex is the current head and Acme its prev. */
static void test_state_resolve_as_of_returns_historical_head(void) {
    hu_graph_relation_t rels[3] = {
        rel(10, 1, 100, HU_REL_WORKS_AT, T_2024, T_2025, 0),
        rel(11, 1, 101, HU_REL_WORKS_AT, T_2025, T_2026, 10),
        rel(12, 1, 102, HU_REL_WORKS_AT, T_2026, 0, 11),
    };
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 3, T_2025 + 86400000LL, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_TRUE(out[0].current);
    HU_ASSERT_EQ(out[0].rel->id, 11);
    HU_ASSERT_EQ(out[0].prev->id, 10);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

/* Two open rows in one single-valued group (legacy data without supersession):
 * the later event_start wins; the earlier is dropped, not rendered beside it. */
static void test_state_resolve_two_open_rows_latest_start_wins(void) {
    hu_graph_relation_t rels[2] = {
        rel(20, 1, 100, HU_REL_LIVES_IN, T_2024, 0, 0),
        rel(21, 1, 101, HU_REL_LIVES_IN, T_2026, 0, 0),
    };
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 2, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_EQ(out[0].rel->id, 21);
    HU_ASSERT_NULL(out[0].prev); /* nothing ended -> no predecessor */
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

/* A closed row whose replacement is NOT in the set (a 1-hop walk from the old
 * employer only sees the old edge) is kept as HISTORY, flagged not current. */
static void test_state_resolve_orphan_closed_row_is_history_not_current(void) {
    hu_graph_relation_t rels[1] = {rel(10, 1, 100, HU_REL_WORKS_AT, T_2024, T_2025, 0)};
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 1, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_FALSE(out[0].current);
    HU_ASSERT_EQ(out[0].rel->id, 10);
    HU_ASSERT_NULL(out[0].prev);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

/* Two closed rows, no current head: only the latest-ended survives as history. */
static void test_state_resolve_history_keeps_latest_ended_only(void) {
    hu_graph_relation_t rels[2] = {
        rel(10, 1, 100, HU_REL_WORKS_AT, T_2024, T_2025, 0),
        rel(11, 1, 101, HU_REL_WORKS_AT, T_2025, T_2026, 10),
    };
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 2, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_FALSE(out[0].current);
    HU_ASSERT_EQ(out[0].rel->id, 11);
    HU_ASSERT_EQ(out[0].prev->id, 10);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

/* Multi-valued types (KNOWS) coexist: every open row stays, in input order;
 * a closed KNOWS row with a different target is independent history. */
static void test_state_resolve_multi_valued_rows_coexist(void) {
    hu_graph_relation_t rels[3] = {
        rel(30, 1, 200, HU_REL_KNOWS, T_2024, 0, 0),
        rel(31, 1, 201, HU_REL_KNOWS, T_2025, 0, 0),
        rel(32, 1, 202, HU_REL_KNOWS, T_2024, T_2025, 0),
    };
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 3, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 3);
    HU_ASSERT_EQ(out[0].rel->id, 30);
    HU_ASSERT_TRUE(out[0].current);
    HU_ASSERT_EQ(out[1].rel->id, 31);
    HU_ASSERT_TRUE(out[1].current);
    HU_ASSERT_EQ(out[2].rel->id, 32);
    HU_ASSERT_FALSE(out[2].current);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

/* Different sources never share a group: Alice's and Bob's employers both render. */
static void test_state_resolve_groups_are_per_source(void) {
    hu_graph_relation_t rels[2] = {
        rel(40, 1, 100, HU_REL_WORKS_AT, T_2024, 0, 0),
        rel(41, 2, 101, HU_REL_WORKS_AT, T_2025, 0, 0),
    };
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 2, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 2);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
}

static void test_state_resolve_empty_and_invalid_inputs(void) {
    hu_graph_state_entry_t *out = (hu_graph_state_entry_t *)0x1;
    size_t n = 99;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), NULL, 0, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((int)n, 0);
    hu_graph_relation_t r = rel(1, 1, 2, HU_REL_KNOWS, 0, 0, 0);
    HU_ASSERT_EQ(hu_graph_state_resolve(NULL, &r, 1, T_NOW, &out, &n), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), &r, 1, T_NOW, NULL, &n),
                 HU_ERR_INVALID_ARGUMENT);
}

/* 512 of 525 live relations (2026-09-04) carry SECONDS in event_start /
 * event_end. A closed seconds row must still read as ended, an open one as
 * current, and its month must not render as "Jan 1970". */
static void test_state_seconds_unit_rows_read_correctly(void) {
    hu_graph_relation_t closed_s = rel(1, 1, 2, HU_REL_WORKS_AT, T_2024 / 1000, T_2025 / 1000, 0);
    hu_graph_relation_t open_s = rel(2, 1, 3, HU_REL_WORKS_AT, T_2025 / 1000, 0, 1);
    HU_ASSERT_FALSE(hu_graph_state_is_current(&closed_s, T_NOW));
    HU_ASSERT_TRUE(hu_graph_state_is_current(&closed_s, T_2024 + 1));
    HU_ASSERT_TRUE(hu_graph_state_is_current(&open_s, T_NOW));
    HU_ASSERT_TRUE(hu_graph_state_is_current(&open_s, T_NOW / 1000)); /* as_of in seconds too */
    hu_graph_relation_t rels[2] = {closed_s, open_s};
    hu_graph_state_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_state_resolve(sys_alloc(), rels, 2, T_NOW, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);
    HU_ASSERT_EQ(out[0].rel->id, 2);
    HU_ASSERT_EQ(out[0].prev->id, 1);
    sys_alloc()->free(sys_alloc()->ctx, out, n * sizeof(*out));
    char buf[16];
    HU_ASSERT_EQ((int)hu_graph_state_format_month(T_2025 / 1000, buf, sizeof(buf)), 8);
    HU_ASSERT_STR_EQ(buf, "Jan 2025");
}

static void test_state_format_month_utc(void) {
    char buf[16];
    HU_ASSERT_EQ((int)hu_graph_state_format_month(T_2025, buf, sizeof(buf)), 8);
    HU_ASSERT_STR_EQ(buf, "Jan 2025");
    HU_ASSERT_EQ((int)hu_graph_state_format_month(T_NOW, buf, sizeof(buf)), 8);
    HU_ASSERT_STR_EQ(buf, "Aug 2026");
    HU_ASSERT_EQ((int)hu_graph_state_format_month(0, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_graph_state_format_month(T_NOW, buf, 4), 0);
}

void run_graph_state_tests(void) {
    HU_TEST_SUITE("Graph state view");
    HU_RUN_TEST(test_state_is_current_honors_event_window);
    HU_RUN_TEST(test_state_resolve_single_valued_keeps_only_open_head);
    HU_RUN_TEST(test_state_resolve_as_of_returns_historical_head);
    HU_RUN_TEST(test_state_resolve_two_open_rows_latest_start_wins);
    HU_RUN_TEST(test_state_resolve_orphan_closed_row_is_history_not_current);
    HU_RUN_TEST(test_state_resolve_history_keeps_latest_ended_only);
    HU_RUN_TEST(test_state_resolve_multi_valued_rows_coexist);
    HU_RUN_TEST(test_state_resolve_groups_are_per_source);
    HU_RUN_TEST(test_state_resolve_empty_and_invalid_inputs);
    HU_RUN_TEST(test_state_seconds_unit_rows_read_correctly);
    HU_RUN_TEST(test_state_format_month_utc);
}
