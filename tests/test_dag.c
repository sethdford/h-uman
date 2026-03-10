#include "human/agent/dag.h"
#include "human/agent/dag_executor.h"
#include "human/agent/llm_compiler.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void dag_add_node_and_find(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    const char *deps[] = {"t0"};
    hu_error_t err = hu_dag_add_node(&dag, "t1", "web_search", "{\"q\":\"x\"}", deps, 1);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dag.node_count, 1);

    hu_dag_node_t *n = hu_dag_find_node(&dag, "t1", 2);
    HU_ASSERT_NOT_NULL(n);
    HU_ASSERT_STR_EQ(n->id, "t1");
    HU_ASSERT_STR_EQ(n->tool_name, "web_search");
    HU_ASSERT_STR_EQ(n->args_json, "{\"q\":\"x\"}");
    HU_ASSERT_EQ(n->dep_count, 1);
    HU_ASSERT_STR_EQ(n->deps[0], "t0");
    HU_ASSERT_EQ(n->status, HU_DAG_PENDING);

    hu_dag_deinit(&dag);
}

static void dag_validate_accepts_valid_dag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "shell", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t1", "web_search", "{}", (const char *[]){"t0"}, 1);
    hu_dag_add_node(&dag, "t2", "file_read", "{}", (const char *[]){"t0"}, 1);

    hu_error_t err = hu_dag_validate(&dag);
    HU_ASSERT_EQ(err, HU_OK);

    hu_dag_deinit(&dag);
}

static void dag_validate_detects_cycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", (const char *[]){"t2"}, 1);
    hu_dag_add_node(&dag, "t1", "b", "{}", (const char *[]){"t0"}, 1);
    hu_dag_add_node(&dag, "t2", "c", "{}", (const char *[]){"t1"}, 1);

    hu_error_t err = hu_dag_validate(&dag);
    HU_ASSERT_NEQ(err, HU_OK);

    hu_dag_deinit(&dag);
}

static void dag_validate_detects_missing_dep(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t1", "b", "{}", (const char *[]){"t99"}, 1);

    hu_error_t err = hu_dag_validate(&dag);
    HU_ASSERT_NEQ(err, HU_OK);

    hu_dag_deinit(&dag);
}

static void dag_validate_detects_duplicate_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t0", "b", "{}", NULL, 0);

    hu_error_t err = hu_dag_validate(&dag);
    HU_ASSERT_NEQ(err, HU_OK);

    hu_dag_deinit(&dag);
}

static void dag_parse_json_creates_nodes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    const char *json =
        "{\"tasks\":[{\"id\":\"t1\",\"tool\":\"web_search\",\"args\":{\"q\":\"x\"},\"deps\":[]},"
        "{\"id\":\"t2\",\"tool\":\"file_read\",\"args\":{},\"deps\":[\"t1\"]}]}";
    hu_error_t err = hu_dag_parse_json(&dag, &alloc, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dag.node_count, 2);

    hu_dag_node_t *n1 = hu_dag_find_node(&dag, "t1", 2);
    HU_ASSERT_NOT_NULL(n1);
    HU_ASSERT_STR_EQ(n1->tool_name, "web_search");

    hu_dag_node_t *n2 = hu_dag_find_node(&dag, "t2", 2);
    HU_ASSERT_NOT_NULL(n2);
    HU_ASSERT_STR_EQ(n2->tool_name, "file_read");
    HU_ASSERT_EQ(n2->dep_count, 1);
    HU_ASSERT_STR_EQ(n2->deps[0], "t1");

    hu_dag_deinit(&dag);
}

static void dag_is_complete_when_all_done(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t1", "b", "{}", (const char *[]){"t0"}, 1);

    HU_ASSERT_FALSE(hu_dag_is_complete(&dag));

    dag.nodes[0].status = HU_DAG_DONE;
    HU_ASSERT_FALSE(hu_dag_is_complete(&dag));

    dag.nodes[1].status = HU_DAG_DONE;
    HU_ASSERT_TRUE(hu_dag_is_complete(&dag));

    dag.nodes[1].status = HU_DAG_FAILED;
    HU_ASSERT_TRUE(hu_dag_is_complete(&dag));

    hu_dag_deinit(&dag);
}

static void dag_next_batch_returns_roots_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t1", "b", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t2", "c", "{}", (const char *[]){"t0", "t1"}, 2);

    hu_dag_batch_t batch;
    hu_error_t err = hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.count, 2);

    hu_dag_deinit(&dag);
}

static void dag_next_batch_returns_dependents_after_roots_done(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t1", "b", "{}", (const char *[]){"t0"}, 1);

    hu_dag_batch_t batch;
    hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(batch.count, 1);
    batch.nodes[0]->status = HU_DAG_DONE;
    batch.nodes[0]->result = hu_strdup(&dag.alloc, "done");
    batch.nodes[0]->result_len = 4;

    hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(batch.count, 1);
    HU_ASSERT_STR_EQ(batch.nodes[0]->id, "t1");

    hu_dag_deinit(&dag);
}

static void dag_resolve_vars_substitutes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t1", "a", "{}", NULL, 0);
    dag.nodes[0].status = HU_DAG_DONE;
    dag.nodes[0].result = hu_strdup(&alloc, "hello");
    dag.nodes[0].result_len = 5;

    char *resolved = NULL;
    size_t resolved_len = 0;
    const char *args = "prefix $t1 suffix";
    hu_error_t err = hu_dag_resolve_vars(&alloc, &dag, args, strlen(args), &resolved, &resolved_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resolved);
    HU_ASSERT_STR_EQ(resolved, "prefix hello suffix");
    hu_str_free(&alloc, resolved);

    hu_dag_deinit(&dag);
}

static void llm_compiler_build_prompt_includes_goal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    const char *goal = "Fetch the weather";
    const char *tools[] = {"web_search"};
    hu_error_t err =
        hu_llm_compiler_build_prompt(&alloc, goal, strlen(goal), tools, 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Fetch the weather") != NULL);
    hu_str_free(&alloc, out);
}

static void llm_compiler_build_prompt_includes_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    const char *goal = "Do something";
    const char *tools[] = {"web_search", "file_read"};
    hu_error_t err =
        hu_llm_compiler_build_prompt(&alloc, goal, strlen(goal), tools, 2, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "web_search") != NULL);
    HU_ASSERT_TRUE(strstr(out, "file_read") != NULL);
    hu_str_free(&alloc, out);
}

static void llm_compiler_parse_plan_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    const char *response =
        "```json\n{\"tasks\":[{\"id\":\"t1\",\"tool\":\"shell\",\"args\":{},\"deps\":[]}]}\n```";
    hu_error_t err = hu_llm_compiler_parse_plan(&alloc, response, strlen(response), &dag);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dag.node_count, 1);
    HU_ASSERT_STR_EQ(dag.nodes[0].id, "t1");
    HU_ASSERT_STR_EQ(dag.nodes[0].tool_name, "shell");

    hu_dag_deinit(&dag);
}

static void dag_resolve_vars_no_refs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t1", "a", "{}", NULL, 0);

    char *resolved = NULL;
    size_t resolved_len = 0;
    const char *args = "no vars here";
    hu_error_t err = hu_dag_resolve_vars(&alloc, &dag, args, strlen(args), &resolved, &resolved_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resolved);
    HU_ASSERT_STR_EQ(resolved, "no vars here");
    hu_str_free(&alloc, resolved);

    hu_dag_deinit(&dag);
}

static void dag_add_node_when_full(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    for (size_t i = 0; i < HU_DAG_MAX_NODES; i++) {
        char id[16];
        (void)snprintf(id, sizeof(id), "t%zu", i);
        hu_error_t err = hu_dag_add_node(&dag, id, "a", "{}", NULL, 0);
        HU_ASSERT_EQ(err, HU_OK);
    }

    hu_error_t err = hu_dag_add_node(&dag, "extra", "b", "{}", NULL, 0);
    HU_ASSERT_NEQ(err, HU_OK);

    hu_dag_deinit(&dag);
}

static void dag_parse_json_empty_tasks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    const char *json = "{\"tasks\":[]}";
    hu_error_t err = hu_dag_parse_json(&dag, &alloc, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dag.node_count, 0u);

    hu_dag_deinit(&dag);
}

static void dag_parse_json_missing_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    const char *json = "{\"tasks\":[{\"tool\":\"a\",\"args\":{},\"deps\":[]}]}";
    hu_error_t err = hu_dag_parse_json(&dag, &alloc, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dag.node_count, 0u);

    hu_dag_deinit(&dag);
}

static void dag_find_node_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t0", "a", "{}", NULL, 0);
    hu_dag_node_t *n = hu_dag_find_node(&dag, "nonexistent", 10);
    HU_ASSERT_NULL(n);

    hu_dag_deinit(&dag);
}

static void dag_next_batch_empty_dag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_batch_t batch;
    hu_error_t err = hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.count, 0u);

    hu_dag_deinit(&dag);
}

static void dag_resolve_vars_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    char *resolved = NULL;
    size_t resolved_len = 0;
    hu_error_t err = hu_dag_resolve_vars(&alloc, &dag, NULL, 0, &resolved, &resolved_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resolved);
    HU_ASSERT_EQ(resolved_len, 0u);
    hu_str_free(&alloc, resolved);

    hu_dag_deinit(&dag);
}

void run_dag_tests(void) {
    HU_TEST_SUITE("dag");
    HU_RUN_TEST(dag_add_node_and_find);
    HU_RUN_TEST(dag_validate_accepts_valid_dag);
    HU_RUN_TEST(dag_validate_detects_cycle);
    HU_RUN_TEST(dag_validate_detects_missing_dep);
    HU_RUN_TEST(dag_validate_detects_duplicate_id);
    HU_RUN_TEST(dag_parse_json_creates_nodes);
    HU_RUN_TEST(dag_is_complete_when_all_done);
    HU_RUN_TEST(dag_next_batch_returns_roots_first);
    HU_RUN_TEST(dag_next_batch_returns_dependents_after_roots_done);
    HU_RUN_TEST(dag_resolve_vars_substitutes);
    HU_RUN_TEST(dag_resolve_vars_no_refs);
    HU_RUN_TEST(dag_add_node_when_full);
    HU_RUN_TEST(dag_parse_json_empty_tasks);
    HU_RUN_TEST(dag_parse_json_missing_id);
    HU_RUN_TEST(dag_find_node_not_found);
    HU_RUN_TEST(dag_next_batch_empty_dag);
    HU_RUN_TEST(dag_resolve_vars_null_args);
    HU_RUN_TEST(llm_compiler_build_prompt_includes_goal);
    HU_RUN_TEST(llm_compiler_build_prompt_includes_tools);
    HU_RUN_TEST(llm_compiler_parse_plan_valid);
}
