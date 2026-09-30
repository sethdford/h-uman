/* tests/test_turn_retrieve.c — contract tests for hu_turn_retrieve
 * (src/agent/turn/turn_retrieve.c, S3 of the hu_agent_turn carve), driven
 * directly with a hand-built context. */
#include "human/agent/turn.h"
#include "human/cognition/dual_process.h"
#include "test_framework.h"
#include <string.h>

static void turn_retrieve_rejects_a_null_context(void) {
    HU_ASSERT_EQ(hu_turn_retrieve(NULL), HU_ERR_INVALID_ARGUMENT);
}

#ifdef HU_ENABLE_SQLITE
#include "turn_test_fixture.h"

static hu_turn_ctx_t *tr_ctx(tf_fixture_t *f, const char *msg) {
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f->agent, msg, strlen(msg), &f->resp, &f->resp_len);
    if (turn_ctx)
        turn_ctx->perception.cognition_budget =
            hu_cognition_get_budget(HU_COGNITION_FAST, f->agent.max_tool_iterations);
    return turn_ctx;
}

static void turn_retrieve_without_memory_produces_no_context(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = tr_ctx(&f, "what is my favorite color");
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->retrieval.memory_ctx_nonempty = true; /* sentinel: the stage must overwrite it */
    HU_ASSERT_EQ(hu_turn_retrieve(turn_ctx), HU_OK);
    HU_ASSERT_NULL(turn_ctx->retrieval.memory_ctx);
    HU_ASSERT_NULL(turn_ctx->retrieval.graph_ctx);
    HU_ASSERT_FALSE(turn_ctx->retrieval.memory_ctx_nonempty);
    HU_ASSERT_NULL(turn_ctx->retrieval.instruction_ctx); /* scratch workspace + state dir */
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Inputs match the characterization case srag_personal_retrieves. If this
 * fails while the goldens pass, the input does not reach the branch: change
 * the stored fact or the query, never the assertion. */
static void turn_retrieve_personal_query_loads_stored_memory(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tf_store(&f, "fav_color", "favorite color: teal", NULL));
    f.agent.sota.srag_config.enabled = true;
    hu_turn_ctx_t *turn_ctx = tr_ctx(&f, "what is my favorite color");
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_retrieve(turn_ctx), HU_OK);
    HU_ASSERT_NOT_NULL(turn_ctx->retrieval.memory_ctx);
    HU_ASSERT_STR_CONTAINS(turn_ctx->retrieval.memory_ctx, "teal");
    HU_ASSERT_TRUE(turn_ctx->retrieval.memory_ctx_nonempty);
    HU_ASSERT_EQ(turn_ctx->retrieval.memory_ctx_len, strlen(turn_ctx->retrieval.memory_ctx));
    hu_turn_ctx_free(turn_ctx); /* owns memory_ctx: ASan proves it is released exactly once */
    tf_close(&f);
}

static void turn_retrieve_creative_query_skips_retrieval(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tf_store(&f, "fav_color", "favorite color: teal", NULL));
    f.agent.sota.srag_config.enabled = true;
    hu_turn_ctx_t *turn_ctx = tr_ctx(&f, "write a short poem about my favorite color");
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->retrieval.rag_strategy_used = HU_RAG_GRAPH; /* sentinel */
    HU_ASSERT_EQ(hu_turn_retrieve(turn_ctx), HU_OK);
    HU_ASSERT_NULL(turn_ctx->retrieval.memory_ctx);
    HU_ASSERT_FALSE(turn_ctx->retrieval.memory_ctx_nonempty);
    HU_ASSERT_EQ(turn_ctx->retrieval.rag_strategy_used, HU_RAG_NONE); /* Self-RAG skip: no pick */
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_turn_retrieve_tests(void) {
    HU_TEST_SUITE("TurnRetrieve");
    HU_RUN_TEST(turn_retrieve_rejects_a_null_context);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turn_retrieve_without_memory_produces_no_context);
    HU_RUN_TEST(turn_retrieve_personal_query_loads_stored_memory);
    HU_RUN_TEST(turn_retrieve_creative_query_skips_retrieval);
#endif
}
