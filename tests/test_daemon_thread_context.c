/* HU_THREAD_CONTEXT (2026-10-01): the reactive reply model gets the last real
 * chat.db messages as a "## Recent thread" block in conversation_context.
 * Pins the renderer (order, labels, budget keeps the newest, attachment
 * collapse, relative gaps, current-inbound skip, empty -> no block) and the
 * gate (OFF byte-identical, SHADOW unchanged, LIVE appends, cloud primary
 * skipped). Hermetic: entries are built in memory, no chat.db. */
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/daemon/thread_context.h"
#include "human/providers/local_only.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A fixed local "now"; timestamps are produced with the same localtime()
 * the renderer's mktime() inverts, so the test is timezone-independent. */
static const time_t k_now = 1790000000; /* 2026-09-21 */

static void entry(hu_channel_history_entry_t *e, bool from_me, const char *text, time_t at) {
    memset(e, 0, sizeof(*e));
    e->from_me = from_me;
    snprintf(e->text, sizeof(e->text), "%s", text);
    if (at > 0) {
        struct tm lt;
        localtime_r(&at, &lt);
        strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M:%S", &lt);
    }
}

static char *render(hu_allocator_t *a, const hu_channel_history_entry_t *es, size_t n,
                    const char *name, const char *cur, size_t budget, size_t *len,
                    hu_thread_context_stats_t *st) {
    char *out = NULL;
    *len = 0;
    HU_ASSERT_EQ(hu_thread_context_render(a, es, n, name, name ? strlen(name) : 0, cur,
                                          cur ? strlen(cur) : 0, k_now, budget, &out, len, st),
                 HU_OK);
    return out;
}

static void test_thread_render_orders_oldest_first_with_labels(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[3];
    entry(&es[0], false, "you around saturday?", k_now - 600);
    entry(&es[1], true, "ya should be", k_now - 540);
    entry(&es[2], false, "cool", k_now - 480);
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 3, "Mike Smith", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strncmp(out, HU_LOCAL_ONLY_BEGIN, strlen(HU_LOCAL_ONLY_BEGIN)), 0);
    HU_ASSERT_STR_CONTAINS(out, "Mike: you around saturday?\nyou: ya should be\nMike: cool\n");
    HU_ASSERT_STR_NOT_CONTAINS(out, "Smith");
    size_t el = strlen(HU_LOCAL_ONLY_END);
    HU_ASSERT_TRUE(len > el);
    HU_ASSERT_EQ(strcmp(out + len - el, HU_LOCAL_ONLY_END), 0);
    HU_ASSERT_EQ(st.lines, 3);
    HU_ASSERT_EQ(st.seth_lines, 1);
    HU_ASSERT_EQ(st.bytes, len);
    HU_ASSERT_EQ(st.dropped, 0);
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_unknown_contact_is_them(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[1];
    entry(&es[0], false, "hey", k_now - 60);
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 1, NULL, NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "\nthem: hey\n");
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_budget_keeps_newest(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[20];
    char text[256];
    for (int i = 0; i < 20; i++) {
        snprintf(text, sizeof(text), "msg%02d %s", i,
                 "padding padding padding padding padding padding padding padding padding "
                 "padding padding padding padding padding padding padding padding");
        entry(&es[i], (i % 2) == 1, text, k_now - (20 - i) * 60);
    }
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 20, "Mike", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_LE(len, HU_THREAD_CONTEXT_BUDGET);
    HU_ASSERT_STR_CONTAINS(out, "msg19");
    HU_ASSERT_STR_CONTAINS(out, "msg18");
    HU_ASSERT_STR_NOT_CONTAINS(out, "msg00");
    HU_ASSERT_STR_NOT_CONTAINS(out, "msg04");
    HU_ASSERT_TRUE(st.lines < 15);
    HU_ASSERT_EQ(st.lines + st.dropped, 20);
    /* the kept lines are the newest, still oldest-first */
    HU_ASSERT_TRUE(strstr(out, "msg18") < strstr(out, "msg19"));
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_caps_lines_at_max(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[25];
    char text[16];
    for (int i = 0; i < 25; i++) {
        snprintf(text, sizeof(text), "m%02d", i);
        entry(&es[i], false, text, k_now - (25 - i) * 60);
    }
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 25, "Mike", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(st.lines, HU_THREAD_CONTEXT_MAX_LINES);
    HU_ASSERT_EQ(st.dropped, 25 - HU_THREAD_CONTEXT_MAX_LINES);
    HU_ASSERT_STR_CONTAINS(out, "Mike: m24\n");
    HU_ASSERT_STR_NOT_CONTAINS(out, "m09");
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_collapses_attachments(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[5];
    entry(&es[0], false, "[Photo]", k_now - 300);
    entry(&es[1], false, "[Voice Message]", k_now - 290);
    entry(&es[2], false, "[image or attachment]", k_now - 280);
    entry(&es[3], true, "[you replied]", k_now - 270);
    entry(&es[4], false, "\xEF\xBF\xBC look at this", k_now - 260);
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 5, "Mike", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "Mike: [photo]\nMike: [voice memo]\nMike: [photo]\n");
    HU_ASSERT_STR_CONTAINS(out, "you: [attachment]\n");
    HU_ASSERT_STR_CONTAINS(out, "Mike: [attachment] look at this\n");
    HU_ASSERT_STR_NOT_CONTAINS(out, "Voice Message");
    HU_ASSERT_STR_NOT_CONTAINS(out, "\xEF\xBF\xBC");
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_relative_gaps(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[3];
    entry(&es[0], false, "first", k_now - 2 * 86400);
    entry(&es[1], true, "second", k_now - 2 * 86400 + 300); /* 5 min later: no marker */
    entry(&es[2], false, "third", k_now - 3 * 3600);        /* 44h55m later */
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 3, "Mike", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "[2d ago]\nMike: first\nyou: second\n[44h later]\nMike: third\n");
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_skips_current_inbound(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[3];
    entry(&es[0], true, "ok see you then", k_now - 900);
    entry(&es[1], false, "wait", k_now - 30);
    entry(&es[2], false, "are you still coming", k_now - 20);
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 3, "Mike", "wait\nare you still coming", HU_THREAD_CONTEXT_BUDGET,
                       &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "you: ok see you then\n");
    HU_ASSERT_STR_NOT_CONTAINS(out, "still coming");
    HU_ASSERT_STR_NOT_CONTAINS(out, "Mike: wait");
    HU_ASSERT_TRUE(st.skipped_current);
    HU_ASSERT_EQ(st.lines, 1);
    a.free(a.ctx, out, len + 1);
}

static void test_thread_render_empty_thread_no_block(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_thread_context_stats_t st;
    size_t len = 7;
    char *out = render(&a, NULL, 0, "Mike", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, 0);
    HU_ASSERT_EQ(st.bytes, 0);
    /* Only the current inbound in the thread: nothing left to show. */
    hu_channel_history_entry_t es[1];
    entry(&es[0], false, "hi", k_now - 5);
    out = render(&a, es, 1, "Mike", "hi", HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(st.lines, 0);
}

static void test_thread_render_one_line_per_message(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t es[1];
    entry(&es[0], false, "line one\nline two\r\nline three", k_now - 60);
    hu_thread_context_stats_t st;
    size_t len = 0;
    char *out = render(&a, es, 1, "Mike", NULL, HU_THREAD_CONTEXT_BUDGET, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "Mike: line one line two line three\n");
    a.free(a.ctx, out, len + 1);
}

/* ── the gate ───────────────────────────────────────────────────────── */

static void test_thread_context_mode_defaults_off(void) {
    unsetenv("HU_THREAD_CONTEXT");
    HU_ASSERT_EQ((int)hu_thread_context_mode(), (int)HU_GATE_OFF);
    setenv("HU_THREAD_CONTEXT", "shadow", 1);
    HU_ASSERT_EQ((int)hu_thread_context_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_THREAD_CONTEXT", "live", 1);
    HU_ASSERT_EQ((int)hu_thread_context_mode(), (int)HU_GATE_LIVE);
    setenv("HU_THREAD_CONTEXT", "bogus", 1);
    HU_ASSERT_EQ((int)hu_thread_context_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_THREAD_CONTEXT");
}

typedef struct apply_rig {
    hu_allocator_t a;
    hu_channel_history_entry_t es[3];
    char *convo;
    size_t convo_len;
} apply_rig_t;

static void apply_rig_init(apply_rig_t *r) {
    memset(r, 0, sizeof(*r));
    r->a = hu_system_allocator();
    entry(&r->es[0], false, "dinner tonight?", k_now - 600);
    entry(&r->es[1], true, "yes 7", k_now - 500);
    entry(&r->es[2], false, "perfect", k_now - 400);
    static const char base[] = "--- Scene Direction ---\nkeep it short";
    r->convo_len = sizeof(base) - 1;
    r->convo = (char *)r->a.alloc(r->a.ctx, r->convo_len + 1);
    memcpy(r->convo, base, r->convo_len + 1);
}

static void apply_rig_run(apply_rig_t *r, hu_gate_mode_t mode, bool local,
                          hu_thread_context_stats_t *st) {
    hu_daemon_thread_context_apply(&r->a, mode, local, r->es, 3, "Ann", 3, NULL, 0, k_now,
                                   &r->convo, &r->convo_len, st);
}

static void test_thread_apply_off_is_byte_identical(void) {
    apply_rig_t r;
    apply_rig_init(&r);
    char *before_ptr = r.convo;
    char before[128];
    memcpy(before, r.convo, r.convo_len + 1);
    size_t before_len = r.convo_len;
    hu_thread_context_stats_t st;
    memset(&st, 0xAB, sizeof(st));
    apply_rig_run(&r, HU_GATE_OFF, true, &st);
    HU_ASSERT_TRUE(r.convo == before_ptr);
    HU_ASSERT_EQ(r.convo_len, before_len);
    HU_ASSERT_EQ(memcmp(r.convo, before, before_len + 1), 0);
    HU_ASSERT_EQ(st.lines, 0);
    HU_ASSERT_EQ(st.bytes, 0);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
}

static void test_thread_apply_shadow_counts_without_changing_prompt(void) {
    apply_rig_t r;
    apply_rig_init(&r);
    char before[128];
    memcpy(before, r.convo, r.convo_len + 1);
    size_t before_len = r.convo_len;
    hu_thread_context_stats_t st;
    apply_rig_run(&r, HU_GATE_SHADOW, true, &st);
    HU_ASSERT_EQ(r.convo_len, before_len);
    HU_ASSERT_EQ(memcmp(r.convo, before, before_len + 1), 0);
    HU_ASSERT_EQ(st.lines, 3);
    HU_ASSERT_EQ(st.seth_lines, 1);
    HU_ASSERT_GT(st.bytes, 0);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
}

static void test_thread_apply_live_appends_block(void) {
    apply_rig_t r;
    apply_rig_init(&r);
    size_t before_len = r.convo_len;
    HU_ASSERT_TRUE(strstr(r.convo, HU_LOCAL_ONLY_BEGIN) == NULL);
    hu_thread_context_stats_t st;
    apply_rig_run(&r, HU_GATE_LIVE, true, &st);
    HU_ASSERT_EQ(r.convo_len, before_len + 2 + st.bytes);
    HU_ASSERT_EQ(strncmp(r.convo, "--- Scene Direction ---\nkeep it short\n\n## Recent thread",
                         strlen("--- Scene Direction ---\nkeep it short\n\n## Recent thread")),
                 0);
    HU_ASSERT_STR_CONTAINS(r.convo, "Ann: dinner tonight?\nyou: yes 7\nAnn: perfect\n");
    HU_ASSERT_EQ(strlen(r.convo), r.convo_len);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
}

static void test_thread_apply_live_into_empty_context(void) {
    apply_rig_t r;
    apply_rig_init(&r);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
    r.convo = NULL;
    r.convo_len = 0;
    hu_thread_context_stats_t st;
    apply_rig_run(&r, HU_GATE_LIVE, true, &st);
    HU_ASSERT_NOT_NULL(r.convo);
    HU_ASSERT_EQ(r.convo_len, st.bytes);
    HU_ASSERT_EQ(strncmp(r.convo, HU_LOCAL_ONLY_BEGIN, strlen(HU_LOCAL_ONLY_BEGIN)), 0);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
}

static void test_thread_apply_live_skips_cloud_primary(void) {
    apply_rig_t r;
    apply_rig_init(&r);
    size_t before_len = r.convo_len;
    hu_thread_context_stats_t st;
    apply_rig_run(&r, HU_GATE_LIVE, false, &st);
    HU_ASSERT_EQ(r.convo_len, before_len);
    HU_ASSERT_TRUE(strstr(r.convo, HU_LOCAL_ONLY_BEGIN) == NULL);
    HU_ASSERT_EQ(st.lines, 0);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
}

/* The LIVE block is exactly what the reliable provider strips before a cloud
 * attempt: stripping it restores the pre-LIVE conversation_context plus the
 * "\n\n" separator. */
static void test_thread_live_block_is_strippable(void) {
    apply_rig_t r;
    apply_rig_init(&r);
    char before[128];
    memcpy(before, r.convo, r.convo_len + 1);
    hu_thread_context_stats_t st;
    apply_rig_run(&r, HU_GATE_LIVE, true, &st);
    char *stripped = NULL;
    size_t stripped_len = 0;
    HU_ASSERT_EQ(hu_local_only_strip(&r.a, r.convo, r.convo_len, &stripped, &stripped_len), HU_OK);
    HU_ASSERT_NOT_NULL(stripped);
    HU_ASSERT_STR_NOT_CONTAINS(stripped, "dinner");
    HU_ASSERT_EQ(strncmp(stripped, before, strlen(before)), 0);
    HU_ASSERT_EQ(stripped_len, strlen(before) + 2);
    r.a.free(r.a.ctx, stripped, stripped_len + 1);
    r.a.free(r.a.ctx, r.convo, r.convo_len + 1);
}

void run_daemon_thread_context_tests(void) {
    HU_TEST_SUITE("daemon_thread_context");
    HU_RUN_TEST(test_thread_render_orders_oldest_first_with_labels);
    HU_RUN_TEST(test_thread_render_unknown_contact_is_them);
    HU_RUN_TEST(test_thread_render_budget_keeps_newest);
    HU_RUN_TEST(test_thread_render_caps_lines_at_max);
    HU_RUN_TEST(test_thread_render_collapses_attachments);
    HU_RUN_TEST(test_thread_render_relative_gaps);
    HU_RUN_TEST(test_thread_render_skips_current_inbound);
    HU_RUN_TEST(test_thread_render_empty_thread_no_block);
    HU_RUN_TEST(test_thread_render_one_line_per_message);
    HU_RUN_TEST(test_thread_context_mode_defaults_off);
    HU_RUN_TEST(test_thread_apply_off_is_byte_identical);
    HU_RUN_TEST(test_thread_apply_shadow_counts_without_changing_prompt);
    HU_RUN_TEST(test_thread_apply_live_appends_block);
    HU_RUN_TEST(test_thread_apply_live_into_empty_context);
    HU_RUN_TEST(test_thread_apply_live_skips_cloud_primary);
    HU_RUN_TEST(test_thread_live_block_is_strippable);
}
