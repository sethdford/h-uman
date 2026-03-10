#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include "human/memory/retrieval.h"
#include "human/memory/retrieval/query_expansion.h"
#include "human/skillforge.h"
#include "human/tool.h"
#include "human/tools/calendar_tool.h"
#include "human/tools/crm.h"
#include "human/tools/firebase.h"
#include "human/tools/gcloud.h"
#include "human/tools/invoice.h"
#include "human/tools/jira.h"
#include "human/tools/spreadsheet.h"
#include "human/tools/web_search_providers.h"
#include "human/tools/workflow.h"
#include "test_framework.h"
#include <string.h>

/* ─── Query Expansion ────────────────────────────────────────────────────── */

static void test_query_expand_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_expanded_query_t eq = {0};
    hu_error_t err = hu_query_expand(&alloc, "how do embeddings work", 22, &eq);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(eq.fts5_query);
    hu_expanded_query_free(&alloc, &eq);
}

static void test_query_expand_null_alloc(void) {
    hu_expanded_query_t eq = {0};
    hu_error_t err = hu_query_expand(NULL, "how do embeddings work", 22, &eq);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_query_expand_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_expanded_query_t eq = {0};
    hu_error_t err = hu_query_expand(&alloc, NULL, 22, &eq);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_query_expand_stop_words_removed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_expanded_query_t eq = {0};
    const char *q = "the a an is are";
    hu_error_t err = hu_query_expand(&alloc, q, strlen(q), &eq);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(eq.filtered_count < eq.original_count);
    hu_expanded_query_free(&alloc, &eq);
}

static void test_expanded_query_free_null_alloc(void) {
    hu_expanded_query_t eq = {0};
    hu_expanded_query_free(NULL, &eq);
    /* Should not crash */
}

/* ─── Temporal Decay ─────────────────────────────────────────────────────── */

static void test_temporal_decay_recent(void) {
    const char *ts = "2026-03-06T12:00:00Z";
    double score = hu_temporal_decay_score(1.0, 0.01, ts, strlen(ts));
    HU_ASSERT_TRUE(score > 0.9);
    HU_ASSERT_TRUE(score <= 1.0);
}

static void test_temporal_decay_old(void) {
    const char *ts = "2020-01-01T00:00:00Z";
    double score = hu_temporal_decay_score(1.0, 0.01, ts, strlen(ts));
    HU_ASSERT_TRUE(score < 0.5);
    HU_ASSERT_TRUE(score >= 0.0);
}

static void test_temporal_decay_null_timestamp(void) {
    double score = hu_temporal_decay_score(1.0, 0.5, NULL, 0);
    HU_ASSERT_FLOAT_EQ(score, 1.0, 1e-9);
}

static void test_temporal_decay_zero_factor(void) {
    const char *ts = "2020-01-01T00:00:00Z";
    double score = hu_temporal_decay_score(1.0, 0.0, ts, strlen(ts));
    HU_ASSERT_FLOAT_EQ(score, 1.0, 1e-9);
}

/* ─── Business Tools ─────────────────────────────────────────────────────── */

static void test_spreadsheet_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_spreadsheet_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_invoice_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_invoice_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_crm_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_crm_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_jira_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_jira_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_calendar_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_calendar_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_workflow_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_workflow_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_gcloud_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_gcloud_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_firebase_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_firebase_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);
    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Web Search Providers (null/invalid arg paths only) ───────────────────── */

static void test_web_search_brave_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_brave(NULL, "q", 1, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_brave_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_brave(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_brave_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_brave(&alloc, "q", 1, 0, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_duckduckgo_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_duckduckgo(NULL, "q", 1, 1, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_duckduckgo_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_duckduckgo(&alloc, NULL, 0, 1, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_duckduckgo_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_duckduckgo(&alloc, "q", 1, 0, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_exa_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_exa(NULL, "q", 1, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_exa_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_exa(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_exa_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_exa(&alloc, "q", 1, 0, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_perplexity_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_perplexity(NULL, "q", 1, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_perplexity_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_perplexity(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_perplexity_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_perplexity(&alloc, "q", 1, 0, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_searxng_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_searxng(NULL, "q", 1, 1, "https://search.example.com", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_searxng_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err =
        hu_web_search_searxng(&alloc, NULL, 0, 1, "https://search.example.com", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_searxng_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err =
        hu_web_search_searxng(&alloc, "q", 1, 0, "https://search.example.com", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_firecrawl(NULL, "q", 1, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_firecrawl(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_null_api_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_firecrawl(&alloc, "q", 1, 1, NULL, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_web_search_firecrawl(&alloc, "q", 1, 1, "key", NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_zero_query_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_firecrawl(&alloc, "q", 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_count_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_firecrawl(&alloc, "q", 1, 0, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_firecrawl_count_over_ten(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_firecrawl(&alloc, "q", 1, 11, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(NULL, "q", 1, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_null_api_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "q", 1, 1, NULL, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_web_search_tavily(&alloc, "q", 1, 1, "key", NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_zero_query_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "q", 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_count_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "q", 1, 0, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_tavily_count_over_ten(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_tavily(&alloc, "q", 1, 11, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_jina_null_alloc(void) {
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_jina(NULL, "q", 1, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_jina_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_jina(&alloc, NULL, 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_jina_null_api_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_jina(&alloc, "q", 1, 1, NULL, &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_jina_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_web_search_jina(&alloc, "q", 1, 1, "key", NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_web_search_jina_zero_query_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_result_t result = {0};
    hu_error_t err = hu_web_search_jina(&alloc, "q", 0, 1, "key", &result);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ─── Config Getters ──────────────────────────────────────────────────────── */

static void test_config_get_provider_key_null(void) {
    const char *key = hu_config_get_provider_key(NULL, "openai");
    HU_ASSERT_NULL(key);
}

static void test_config_get_provider_key_null_name(void) {
    hu_config_t cfg = {0};
    const char *key = hu_config_get_provider_key(&cfg, NULL);
    HU_ASSERT_NULL(key);
}

static void test_config_get_web_search_provider_null(void) {
    /* NULL cfg returns default "duckduckgo", not NULL */
    const char *provider = hu_config_get_web_search_provider(NULL);
    HU_ASSERT_NOT_NULL(provider);
}

/* ─── Doctor ─────────────────────────────────────────────────────────────── */

static void test_doctor_parse_df_basic(void) {
    const char *df_output = "Filesystem  1K-blocks  Used  Available\n"
                            "/dev/disk1  500000  200000  300000";
    unsigned long mb = hu_doctor_parse_df_available_mb(df_output, strlen(df_output));
    HU_ASSERT_TRUE(mb > 0);
}

static void test_doctor_parse_df_null(void) {
    unsigned long mb = hu_doctor_parse_df_available_mb(NULL, 0);
    HU_ASSERT_EQ(mb, 0u);
}

static void test_doctor_truncate_null_alloc(void) {
    char *out = NULL;
    hu_error_t err = hu_doctor_truncate_for_display(NULL, "hello", 5, 10, &out);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ─── SkillForge ─────────────────────────────────────────────────────────── */

static void test_skillforge_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_error_t err = hu_skillforge_create(&alloc, &sf);
    HU_ASSERT_EQ(err, HU_OK);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_create_null_alloc(void) {
    hu_skillforge_t sf = {0};
    hu_error_t err = hu_skillforge_create(NULL, &sf);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_skillforge_list_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skill_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_skillforge_list_skills(&sf, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_get_unknown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "nonexistent");
    HU_ASSERT_NULL(s);
    hu_skillforge_destroy(&sf);
}

/* ─── Run ────────────────────────────────────────────────────────────────── */

void run_coverage_new_tests(void) {
    HU_TEST_SUITE("coverage_new");

    HU_RUN_TEST(test_query_expand_basic);
    HU_RUN_TEST(test_query_expand_null_alloc);
    HU_RUN_TEST(test_query_expand_null_query);
    HU_RUN_TEST(test_query_expand_stop_words_removed);
    HU_RUN_TEST(test_expanded_query_free_null_alloc);

    HU_RUN_TEST(test_temporal_decay_recent);
    HU_RUN_TEST(test_temporal_decay_old);
    HU_RUN_TEST(test_temporal_decay_null_timestamp);
    HU_RUN_TEST(test_temporal_decay_zero_factor);

    HU_RUN_TEST(test_spreadsheet_create);
    HU_RUN_TEST(test_invoice_create);
    HU_RUN_TEST(test_crm_create);
    HU_RUN_TEST(test_jira_create);
    HU_RUN_TEST(test_calendar_create);
    HU_RUN_TEST(test_workflow_create);
    HU_RUN_TEST(test_gcloud_create);
    HU_RUN_TEST(test_firebase_create);

    HU_RUN_TEST(test_web_search_brave_null_alloc);
    HU_RUN_TEST(test_web_search_brave_null_query);
    HU_RUN_TEST(test_web_search_brave_zero_count);
    HU_RUN_TEST(test_web_search_duckduckgo_null_alloc);
    HU_RUN_TEST(test_web_search_duckduckgo_null_query);
    HU_RUN_TEST(test_web_search_duckduckgo_zero_count);
    HU_RUN_TEST(test_web_search_exa_null_alloc);
    HU_RUN_TEST(test_web_search_exa_null_query);
    HU_RUN_TEST(test_web_search_exa_zero_count);
    HU_RUN_TEST(test_web_search_perplexity_null_alloc);
    HU_RUN_TEST(test_web_search_perplexity_null_query);
    HU_RUN_TEST(test_web_search_perplexity_zero_count);
    HU_RUN_TEST(test_web_search_searxng_null_alloc);
    HU_RUN_TEST(test_web_search_searxng_null_query);
    HU_RUN_TEST(test_web_search_searxng_zero_count);

    HU_RUN_TEST(test_web_search_firecrawl_null_alloc);
    HU_RUN_TEST(test_web_search_firecrawl_null_query);
    HU_RUN_TEST(test_web_search_firecrawl_null_api_key);
    HU_RUN_TEST(test_web_search_firecrawl_null_out);
    HU_RUN_TEST(test_web_search_firecrawl_zero_query_len);
    HU_RUN_TEST(test_web_search_firecrawl_count_zero);
    HU_RUN_TEST(test_web_search_firecrawl_count_over_ten);

    HU_RUN_TEST(test_web_search_tavily_null_alloc);
    HU_RUN_TEST(test_web_search_tavily_null_query);
    HU_RUN_TEST(test_web_search_tavily_null_api_key);
    HU_RUN_TEST(test_web_search_tavily_null_out);
    HU_RUN_TEST(test_web_search_tavily_zero_query_len);
    HU_RUN_TEST(test_web_search_tavily_count_zero);
    HU_RUN_TEST(test_web_search_tavily_count_over_ten);

    HU_RUN_TEST(test_web_search_jina_null_alloc);
    HU_RUN_TEST(test_web_search_jina_null_query);
    HU_RUN_TEST(test_web_search_jina_null_api_key);
    HU_RUN_TEST(test_web_search_jina_null_out);
    HU_RUN_TEST(test_web_search_jina_zero_query_len);

    HU_RUN_TEST(test_config_get_provider_key_null);
    HU_RUN_TEST(test_config_get_provider_key_null_name);
    HU_RUN_TEST(test_config_get_web_search_provider_null);

    HU_RUN_TEST(test_doctor_parse_df_basic);
    HU_RUN_TEST(test_doctor_parse_df_null);
    HU_RUN_TEST(test_doctor_truncate_null_alloc);

    HU_RUN_TEST(test_skillforge_create_destroy);
    HU_RUN_TEST(test_skillforge_create_null_alloc);
    HU_RUN_TEST(test_skillforge_list_empty);
    HU_RUN_TEST(test_skillforge_get_unknown);
}
