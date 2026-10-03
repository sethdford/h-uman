/* tests/test_agent_turn_history_window.c — the backward history scans in
 * hu_agent_turn's final-response path must run in SHORT conversations.
 *
 * Both scans bounded their window as `hi > history_count - K` on a size_t:
 * with fewer than K entries the subtraction wrapped, the bound was huge, and
 * the loop body never ran. These tests drive a whole hu_agent_turn over the
 * scripted recording provider (tests/turn_test_fixture.h) with a history
 * below each window and assert the scan's observable effect:
 *   - W10 multimodal blob persistence (window 4): an image in the recent
 *     history lands in the memory facade's blob store;
 *   - re-ask detection (window 8): repeating an earlier question weakens the
 *     learned "helpfulness" value.
 * The re-ask scan also started at the turn's OWN user message, so once it ran
 * (8+ entries) every message over 10 chars matched itself; the long-history
 * control below pins that a new question no longer weakens helpfulness. */
#include "human/agent.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/graph.h"
#include "human/memory/neural_memory.h"
#include "human/provider.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/value_learning.h"

/* tests/ is not on src/agent's include path (same as test_turn_tail.c). */
hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len);

static bool hw_append(tf_fixture_t *f, hu_role_t role, const char *text) {
    return hu_agent_internal_append_history(&f->agent, role, text, strlen(text), NULL, 0, NULL,
                                            0) == HU_OK;
}

static const char k_png[] = "iVBORw0KGgo-fake-png-bytes";

/* Attach one base64 image part to the last history entry. The agent owns it
 * from here: hu_agent_clear_history frees the part's data and media type. */
static bool hw_attach_image(tf_fixture_t *f) {
    hu_owned_message_t *m = &f->agent.history[f->agent.history_count - 1];
    hu_content_part_t *parts =
        (hu_content_part_t *)f->alloc.alloc(f->alloc.ctx, sizeof(hu_content_part_t));
    if (!parts)
        return false;
    memset(parts, 0, sizeof(*parts));
    parts->tag = HU_CONTENT_PART_IMAGE_BASE64;
    parts->data.image_base64.data = hu_strndup(&f->alloc, k_png, sizeof(k_png) - 1);
    parts->data.image_base64.data_len = sizeof(k_png) - 1;
    parts->data.image_base64.media_type = hu_strndup(&f->alloc, "image/png", 9);
    parts->data.image_base64.media_type_len = 9;
    m->content_parts = parts;
    m->content_parts_count = 1;
    return parts->data.image_base64.data && parts->data.image_base64.media_type;
}

/* One image two entries back; after the turn the history holds 3 entries,
 * fewer than the 4-entry window, and the image is persisted as blob 1. */
static void turn_persists_an_image_from_a_history_shorter_than_the_window(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_graph_t *graph = NULL;
    hu_w7_facade_t *facade = NULL;
    HU_ASSERT_EQ(hu_graph_open(&f.alloc, ":memory:", 8, &graph), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(graph, &f.alloc, &facade), HU_OK);
    f.agent.w7_facade = facade; /* hu_agent_deinit closes it */
    f.agent.verifier_graph = graph;
    hu_memory_facade_t *mf = hu_w7_facade_memory_handle(facade);
    HU_ASSERT_NOT_NULL(mf);

    HU_ASSERT_TRUE(hw_append(&f, HU_ROLE_USER, "here is a photo of my garden"));
    HU_ASSERT_TRUE(hw_attach_image(&f));
    hu_memory_blob_t *got = NULL;
    HU_ASSERT_EQ(hu_memory_blob_get(mf, &f.alloc, 1, &got), HU_ERR_NOT_FOUND);

    const char *msg = "what do you think of it";
    HU_ASSERT_EQ(hu_agent_turn(&f.agent, msg, strlen(msg), &f.resp, &f.resp_len), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, 3);

    HU_ASSERT_EQ(hu_memory_blob_get(mf, &f.alloc, 1, &got), HU_OK);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_EQ(got->mime_type, "image/png");
    HU_ASSERT_EQ(got->bytes_len, sizeof(k_png) - 1);
    HU_ASSERT_TRUE(memcmp(got->bytes, k_png, sizeof(k_png) - 1) == 0);
    hu_memory_blob_free(&f.alloc, got);

    hu_allocator_t alloc = f.alloc; /* tf_close zeroes the fixture */
    tf_close(&f);
    hu_graph_close(graph, &alloc);
}

/* Seeds a "helpfulness" value, runs one turn of `msg` after `prior` history
 * entries (alternating user/assistant), and returns the value's importance
 * afterwards. Re-ask detection weakens it by 0.15. */
static double hw_helpfulness_after_turn(const char *const *prior, size_t prior_count,
                                        const char *msg, double *before, size_t *history_after) {
    tf_fixture_t f;
    double after = -1.0;
    *before = -1.0;
    *history_after = 0;
    if (!tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS))
        goto out;
    hu_value_engine_t ve;
    sqlite3 *db = hu_sqlite_memory_get_db(&f.mem);
    /* The daemon's intelligence cycle creates inferred_values; do it here. */
    if (!db || hu_value_engine_create(&f.alloc, db, &ve) != HU_OK ||
        hu_value_init_tables(&ve) != HU_OK)
        goto out;
    if (hu_value_learn_from_approval(&ve, "helpfulness", 11, 2.0, (int64_t)time(NULL)) != HU_OK)
        goto out;
    hu_value_t v;
    bool found = false;
    if (hu_value_get(&ve, "helpfulness", 11, &v, &found) != HU_OK || !found)
        goto out;
    *before = v.importance;
    for (size_t i = 0; i < prior_count; i++)
        if (!hw_append(&f, (i % 2) ? HU_ROLE_ASSISTANT : HU_ROLE_USER, prior[i]))
            goto out;
    if (hu_agent_turn(&f.agent, msg, strlen(msg), &f.resp, &f.resp_len) != HU_OK)
        goto out;
    *history_after = f.agent.history_count;
    found = false;
    if (hu_value_get(&ve, "helpfulness", 11, &v, &found) == HU_OK && found)
        after = v.importance;
out:
    tf_close(&f);
    return after;
}

static const char *const k_prior[] = {
    "hey there, quick question for you",
    "sure, go ahead",
    "can you list my saved project notes",
    "here are the notes I found",
};

/* Six entries after the turn (fewer than the 8-entry window): asking the same
 * question again is a re-ask, so helpfulness drops by exactly 0.15. */
static void turn_detects_a_reask_in_a_history_shorter_than_the_window(void) {
    double before = 0, after = 0;
    size_t n = 0;
    after =
        hw_helpfulness_after_turn(k_prior, 4, "can you list my saved project notes", &before, &n);
    HU_ASSERT_EQ(n, 6);
    HU_ASSERT_FLOAT_EQ(before, 0.6, 1e-9); /* a new value starts at 0.3 * strength */
    HU_ASSERT_FLOAT_EQ(after, before - 0.15, 1e-9);
}

/* Control: a question that repeats nothing in the history is not a re-ask,
 * so helpfulness is unchanged. */
static void turn_does_not_flag_a_new_question_as_a_reask(void) {
    double before = 0, after = 0;
    size_t n = 0;
    after = hw_helpfulness_after_turn(k_prior, 4, "what time does the hardware store close",
                                      &before, &n);
    HU_ASSERT_EQ(n, 6);
    HU_ASSERT_FLOAT_EQ(before, 0.6, 1e-9); /* a new value starts at 0.3 * strength */
    HU_ASSERT_FLOAT_EQ(after, before, 1e-9);
}

static const char *const k_long_prior[] = {
    "how was the drive up north",      "long but fine",
    "did you see the game last night", "yeah, wild ending",
    "are we still on for friday",      "yep, seven works",
    "can you send me that recipe",     "sent it over",
};

/* Ten entries after the turn: a question that repeats nothing must not count
 * as a re-ask. Before the fix the scan compared the message to itself here and
 * helpfulness fell 0.60 -> 0.45 on every such turn. */
static void turn_does_not_flag_a_new_question_in_a_long_history(void) {
    double before = 0, after = 0;
    size_t n = 0;
    after = hw_helpfulness_after_turn(k_long_prior, 8, "what time does the hardware store close",
                                      &before, &n);
    HU_ASSERT_EQ(n, 10);
    HU_ASSERT_FLOAT_EQ(before, 0.6, 1e-9);
    HU_ASSERT_FLOAT_EQ(after, before, 1e-9);
}
#endif /* HU_ENABLE_SQLITE */

void run_agent_turn_history_window_tests(void) {
    HU_TEST_SUITE("AgentTurnHistoryWindow");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turn_persists_an_image_from_a_history_shorter_than_the_window);
    HU_RUN_TEST(turn_detects_a_reask_in_a_history_shorter_than_the_window);
    HU_RUN_TEST(turn_does_not_flag_a_new_question_as_a_reask);
    HU_RUN_TEST(turn_does_not_flag_a_new_question_in_a_long_history);
#endif
}
