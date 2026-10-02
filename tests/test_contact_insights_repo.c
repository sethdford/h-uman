/* contact_insights repo (src/memory/repos/contact_insights_repo_sqlite.c) +
 * the memory loader's HU_INSIGHT_STREAM block.
 * Item 3 of docs/plans/2026-09-06-better-than-human. */

#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent/curiosity_gaps.h"
#include "human/agent/memory_loader.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/contact_insights_repo.h"
#include "human/memory/engines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_2024 1704067200000LL /* 2024-01-01 */
#define T_2025 1735689600000LL /* 2025-01-01 */
#define T_2026 1767225600000LL /* 2026-01-01 */

static const char k_contact[] = "+15550001111";

static void seed_three(hu_memory_t *mem) {
    HU_ASSERT_EQ(hu_contact_insights_add(mem, k_contact, strlen(k_contact), "fact",
                                         "started at Initech in january", 0.8, T_2026, "test",
                                         NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(mem, k_contact, strlen(k_contact), "thread",
                                         "still hunting for a place near the water", 0.7, T_2025,
                                         "test", NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(mem, k_contact, strlen(k_contact), "inside_ref",
                                         "the biscuit sandwich incident", 0.9, T_2024, "test",
                                         NULL),
                 HU_OK);
}

static void render_orders_newest_first_with_month_and_caps(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem);

    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strlen(out), len);
    const char *p26 = strstr(out, "Initech");
    const char *p25 = strstr(out, "near the water");
    const char *p24 = strstr(out, "biscuit");
    HU_ASSERT_NOT_NULL(p26);
    HU_ASSERT_NOT_NULL(p25);
    HU_ASSERT_NOT_NULL(p24);
    HU_ASSERT_TRUE(p26 < p25 && p25 < p24); /* newest as_of first */
    HU_ASSERT_NOT_NULL(strstr(out, "(as of Jan 2026)"));
    HU_ASSERT_TRUE(out[0] == '-' && out[len - 1] == '\n');
    a.free(a.ctx, out, len + 1);

    /* max_items caps rows */
    out = NULL;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 1, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Initech"));
    HU_ASSERT_NULL(strstr(out, "biscuit"));
    a.free(a.ctx, out, len + 1);

    /* max_bytes drops a whole line, never cuts mid-line */
    out = NULL;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 60, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(len <= 60);
    HU_ASSERT_TRUE(out[len - 1] == '\n');
    a.free(a.ctx, out, len + 1);

    /* another contact sees nothing */
    out = (char *)0x1;
    HU_ASSERT_EQ(hu_contact_insights_render(&mem, &a, "+15559999999", 12, 8, 900, 0.5, &out, &len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, (size_t)0);

    mem.vtable->deinit(mem.ctx);
}

/* HU_INSIGHT_WIDE gates ONLY curator_wide rows; persona rows are unaffected. */
static void curator_wide_rows_follow_the_insight_wide_gate(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact",
                                         "persona note Initech", 0.9, 1767225600000LL,
                                         "extractor:v2:k3:a3", NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "plan",
                                         "wide note Priya surgery", 0.9, 1767225600001LL,
                                         "curator_wide:extractor:v2:k3:a3", NULL),
                 HU_OK);
    const char *modes[] = {NULL, "shadow", "garbage", "live"};
    const bool wide_seen[] = {false, false, false, true};
    for (size_t i = 0; i < 4; i++) {
        if (modes[i])
            setenv("HU_INSIGHT_WIDE", modes[i], 1);
        else
            unsetenv("HU_INSIGHT_WIDE");
        char *out = NULL;
        size_t len = 0;
        HU_ASSERT_EQ(hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5,
                                                &out, &len),
                     HU_OK);
        HU_ASSERT_NOT_NULL(out);
        HU_ASSERT_NOT_NULL(strstr(out, "persona note Initech"));
        HU_ASSERT_EQ(strstr(out, "Priya") != NULL, wide_seen[i]);
        a.free(a.ctx, out, len + 1);
    }
    unsetenv("HU_INSIGHT_WIDE");
    mem.vtable->deinit(mem.ctx);
}

static void retired_and_low_confidence_rows_are_not_rendered(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact", "old job", 0.9,
                                         T_2025, "test", &id),
                 HU_OK);
    HU_ASSERT_TRUE(id > 0);
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "guess",
                                         "maybe likes jazz", 0.3, T_2026, "test", NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_retire(&mem, id, T_2026), HU_OK);
    char *out = (char *)0x1;
    size_t len = 99;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NULL(out); /* retired + below-threshold = nothing to say */
    HU_ASSERT_EQ(len, (size_t)0);
    mem.vtable->deinit(mem.ctx);
}

static void add_is_idempotent_for_the_same_note(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    for (int i = 0; i < 3; i++)
        HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact",
                                             "runs on saturdays", 0.8, T_2026, "test", NULL),
                     HU_OK);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    size_t n = 0;
    for (const char *p = out; (p = strstr(p, "saturdays")) != NULL; p++)
        n++;
    HU_ASSERT_EQ(n, (size_t)1);
    a.free(a.ctx, out, len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void loader_block_follows_the_gate(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, NULL, 8, 4096), HU_OK);

    static const int modes[] = {HU_GATE_OFF, HU_GATE_SHADOW, HU_GATE_LIVE};
    for (size_t i = 0; i < 3; i++) {
        hu_memory_loader_set_insight_mode_for_test(modes[i]);
        char *ctx = NULL;
        size_t ctx_len = 0;
        hu_error_t err =
            hu_memory_loader_load(&loader, "hey", 3, k_contact, strlen(k_contact), &ctx, &ctx_len);
        HU_ASSERT_EQ(err, HU_OK);
        bool has_block = ctx && strstr(ctx, "What you actually remember about them") != NULL;
        bool has_note = ctx && strstr(ctx, "Initech") != NULL;
        if (modes[i] == HU_GATE_LIVE) {
            HU_ASSERT_TRUE(has_block);
            HU_ASSERT_TRUE(has_note);
            HU_ASSERT_EQ(strlen(ctx), ctx_len);
        } else {
            HU_ASSERT_FALSE(has_block); /* off and shadow leave the prompt untouched */
            HU_ASSERT_FALSE(has_note);
        }
        if (ctx)
            a.free(a.ctx, ctx, ctx_len + 1);
    }
    hu_memory_loader_set_insight_mode_for_test(-1);
    HU_ASSERT_EQ((int)hu_memory_loader_insight_mode(), (int)HU_GATE_OFF); /* env unset → off */
    mem.vtable->deinit(mem.ctx);
}

/* Candidates are newest first, as the repo fetches them. */
static const char *const k_cands[] = {
    "cramps insane, feels sick",           /* 0: newest */
    "bought an outfit for $150",           /* 1 */
    "dad not ideal, rough time at home",   /* 2 */
    "hates her closet, needs new dresses", /* 3 */
    "sister Mara moving to Denver",        /* 4: oldest */
};

static void select_puts_a_word_shared_with_the_message_first(void) {
    size_t idx[3];
    const char q[] = "mara says denver is cold already";
    size_t n = hu_contact_insights_select(k_cands, 5, q, strlen(q), 3, idx);
    HU_ASSERT_EQ(n, (size_t)3);
    HU_ASSERT_EQ(idx[0], (size_t)4); /* the oldest note, but it is what she is talking about */
    HU_ASSERT_EQ(idx[1], (size_t)0); /* then the newest, for what is going on with her now */
    HU_ASSERT_EQ(idx[2], (size_t)1);
}

static void select_ranks_more_shared_words_higher(void) {
    size_t idx[2];
    const char q[] = "my dad again. home is rough";
    size_t n = hu_contact_insights_select(k_cands, 5, q, strlen(q), 2, idx);
    HU_ASSERT_EQ(n, (size_t)2);
    HU_ASSERT_EQ(idx[0], (size_t)2); /* rough + home ("dad" is under 4 letters) */
    HU_ASSERT_EQ(idx[1], (size_t)0);
}

static void select_tolerates_a_plural_either_way(void) {
    size_t idx[1];
    static const char *const two[] = {"bought an outfit for $150", "cramps insane, feels sick"};
    const char q1[] = "this cramp is killing me"; /* note says "cramps" */
    HU_ASSERT_EQ(hu_contact_insights_select(two, 2, q1, strlen(q1), 1, idx), (size_t)1);
    HU_ASSERT_EQ(idx[0], (size_t)1);
    const char q2[] = "my closets are a mess"; /* note says "closet" */
    HU_ASSERT_EQ(hu_contact_insights_select(k_cands, 5, q2, strlen(q2), 1, idx), (size_t)1);
    HU_ASSERT_EQ(idx[0], (size_t)3);
}

static void select_without_a_message_keeps_the_newest(void) {
    size_t idx[5];
    HU_ASSERT_EQ(hu_contact_insights_select(k_cands, 5, NULL, 0, 2, idx), (size_t)2);
    HU_ASSERT_EQ(idx[0], (size_t)0);
    HU_ASSERT_EQ(idx[1], (size_t)1);
    HU_ASSERT_EQ(hu_contact_insights_select(k_cands, 2, "hey", 3, 5, idx), (size_t)2);
    HU_ASSERT_EQ(hu_contact_insights_select(k_cands, 0, "hey", 3, 5, idx), (size_t)0);
}

static void render_for_query_reaches_past_the_newest(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem);
    for (int i = 0; i < 6; i++) {
        char note[64];
        snprintf(note, sizeof(note), "filler note number %d", i);
        HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact", note, 0.8,
                                             T_2026 + 1000 + i, "test", NULL),
                     HU_OK);
    }
    char *out = NULL;
    size_t len = 0;
    const char q[] = "remember the biscuit thing lol";
    HU_ASSERT_EQ(hu_contact_insights_render_for_query(&mem, &a, k_contact, strlen(k_contact), q,
                                                      strlen(q), 3, 900, 0.5, &out, &len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "biscuit")); /* oldest of nine, outside the newest 8 */
    HU_ASSERT_TRUE(strstr(out, "biscuit") < strchr(out, '\n')); /* ranked first */
    HU_ASSERT_NOT_NULL(strstr(out, "filler note number 5"));
    HU_ASSERT_NULL(strstr(out, "Initech"));
    size_t lines = 0;
    for (size_t i = 0; i < len; i++)
        lines += out[i] == '\n';
    HU_ASSERT_EQ(lines, (size_t)3);
    a.free(a.ctx, out, len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void loader_block_uses_the_incoming_message(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem);
    for (int i = 0; i < HU_INSIGHT_MAX_ITEMS; i++) {
        char note[64];
        snprintf(note, sizeof(note), "newer filler %d", i);
        HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact", note, 0.8,
                                             T_2026 + 1000 + i, "test", NULL),
                     HU_OK);
    }
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, NULL, 8, 4096), HU_OK);
    hu_memory_loader_set_insight_mode_for_test(HU_GATE_LIVE);
    char *ctx = NULL;
    size_t ctx_len = 0;
    const char q[] = "found a place near the water!!";
    HU_ASSERT_EQ(
        hu_memory_loader_load(&loader, q, strlen(q), k_contact, strlen(k_contact), &ctx, &ctx_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "still hunting for a place near the water"));
    a.free(a.ctx, ctx, ctx_len + 1);
    hu_memory_loader_set_insight_mode_for_test(-1);
    mem.vtable->deinit(mem.ctx);
}

static void recent_text_keeps_only_notes_since_the_cutoff(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem); /* as_of 2026, 2025, 2024 */
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_contact_insights_recent_text(&mem, &a, k_contact, strlen(k_contact), T_2025, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strlen(out), len);
    HU_ASSERT_NOT_NULL(strstr(out, "Initech"));
    HU_ASSERT_NOT_NULL(strstr(out, "near the water"));
    HU_ASSERT_NULL(strstr(out, "biscuit")); /* 2024: older than the cutoff */
    HU_ASSERT_NOT_NULL(strchr(out, '\n'));
    a.free(a.ctx, out, len + 1);
    /* nothing that recent: NULL, OK */
    HU_ASSERT_EQ(hu_contact_insights_recent_text(&mem, &a, k_contact, strlen(k_contact), T_2026 + 1,
                                                 &out, &len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, (size_t)0);
    mem.vtable->deinit(mem.ctx);
}

static void loader_offers_a_curiosity_gap_only_when_live(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem); /* all older than 30 days: every topic is a gap */
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, NULL, 8, 4096), HU_OK);
    hu_memory_loader_set_insight_mode_for_test(HU_GATE_LIVE);
    static const char *const modes[] = {"off", "shadow", "live"};
    for (size_t i = 0; i < 3; i++) {
        hu_curiosity_gaps_reset_for_test();
        setenv("HU_CURIOSITY_GAPS", modes[i], 1);
        char *ctx = NULL;
        size_t ctx_len = 0;
        HU_ASSERT_EQ(hu_memory_loader_load(&loader, "ugh long day", 12, k_contact,
                                           strlen(k_contact), &ctx, &ctx_len),
                     HU_OK);
        HU_ASSERT_NOT_NULL(ctx);
        bool has_gap = strstr(ctx, "what they have coming up") != NULL;
        HU_ASSERT_EQ(has_gap, i == 2);
        a.free(a.ctx, ctx, ctx_len + 1);
    }
    /* a question from them: answer it, no gap even when live */
    hu_curiosity_gaps_reset_for_test();
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, "u around?", 9, k_contact, strlen(k_contact), &ctx,
                                       &ctx_len),
                 HU_OK);
    HU_ASSERT_TRUE(ctx == NULL || strstr(ctx, "what they have coming up") == NULL);
    if (ctx)
        a.free(a.ctx, ctx, ctx_len + 1);
    unsetenv("HU_CURIOSITY_GAPS");
    hu_memory_loader_set_insight_mode_for_test(-1);
    mem.vtable->deinit(mem.ctx);
}

void run_contact_insights_repo_tests(void) {
    HU_TEST_SUITE("contact insights (insight stream)");
    HU_RUN_TEST(select_puts_a_word_shared_with_the_message_first);
    HU_RUN_TEST(select_ranks_more_shared_words_higher);
    HU_RUN_TEST(select_tolerates_a_plural_either_way);
    HU_RUN_TEST(select_without_a_message_keeps_the_newest);
    HU_RUN_TEST(render_for_query_reaches_past_the_newest);
    HU_RUN_TEST(loader_block_uses_the_incoming_message);
    HU_RUN_TEST(recent_text_keeps_only_notes_since_the_cutoff);
    HU_RUN_TEST(loader_offers_a_curiosity_gap_only_when_live);
    HU_RUN_TEST(render_orders_newest_first_with_month_and_caps);
    HU_RUN_TEST(curator_wide_rows_follow_the_insight_wide_gate);
    HU_RUN_TEST(retired_and_low_confidence_rows_are_not_rendered);
    HU_RUN_TEST(add_is_idempotent_for_the_same_note);
    HU_RUN_TEST(loader_block_follows_the_gate);
}

#else

void run_contact_insights_repo_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
