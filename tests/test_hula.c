#include "human/agent/hula.h"
#include "human/agent/hula_analytics.h"
#include "human/agent/hula_compiler.h"
#include "human/agent/hula_emergence.h"
#include "human/agent/hula_lite.h"
#include "human/agent/spawn.h"
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/hula_sdk.h"
#include "human/permission.h"
#include "human/provider.h"
#include "human/security.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ── Stub tool for testing ──────────────────────────────────────────────── */

static hu_error_t echo_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                               hu_tool_result_t *out) {
    (void)ctx;
    const char *text = hu_json_get_string(args, "text");
    if (text) {
        size_t len = strlen(text);
        char *dup = hu_strndup(alloc, text, len);
        *out = hu_tool_result_ok_owned(dup, len);
    } else {
        *out = hu_tool_result_ok("echo", 4);
    }
    return HU_OK;
}

static const char *echo_name(void *ctx) {
    (void)ctx;
    return "echo";
}
static const char *echo_desc(void *ctx) {
    (void)ctx;
    return "Echoes text";
}
static const char *echo_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}";
}

static const hu_tool_vtable_t echo_vtable = {
    .execute = echo_execute,
    .name = echo_name,
    .description = echo_desc,
    .parameters_json = echo_params,
};

static hu_error_t fail_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                               hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_fail("intentional failure", 19);
    return HU_OK;
}

static const char *fail_name(void *ctx) {
    (void)ctx;
    return "fail_tool";
}
static const char *fail_desc(void *ctx) {
    (void)ctx;
    return "Always fails";
}
static const char *fail_params(void *ctx) {
    (void)ctx;
    return "{}";
}

static const hu_tool_vtable_t fail_vtable = {
    .execute = fail_execute,
    .name = fail_name,
    .description = fail_desc,
    .parameters_json = fail_params,
};

static int flaky_hits;

static hu_error_t flaky_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                hu_tool_result_t *out) {
    (void)ctx;
    flaky_hits++;
    if (flaky_hits < 3) {
        *out = hu_tool_result_fail("flaky", 5);
        return HU_OK;
    }
    return echo_execute(ctx, alloc, args, out);
}

static const char *flaky_name(void *ctx) {
    (void)ctx;
    return "flaky_tool";
}
static const char *flaky_desc(void *ctx) {
    (void)ctx;
    return "Fails twice";
}
static const char *flaky_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}";
}

static const hu_tool_vtable_t flaky_vtable = {
    .execute = flaky_execute,
    .name = flaky_name,
    .description = flaky_desc,
    .parameters_json = flaky_params,
};

/* High-risk tool name for policy tests — must never execute if policy blocks first */
static hu_error_t shell_stub_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                     hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_fail("shell stub invoked (policy should block)", 40);
    return HU_OK;
}

static const char *shell_stub_name(void *ctx) {
    (void)ctx;
    return "shell";
}
static const char *shell_stub_desc(void *ctx) {
    (void)ctx;
    return "Shell stub";
}
static const char *shell_stub_params(void *ctx) {
    (void)ctx;
    return "{}";
}

static const hu_tool_vtable_t shell_stub_vtable = {
    .execute = shell_stub_execute,
    .name = shell_stub_name,
    .description = shell_stub_desc,
    .parameters_json = shell_stub_params,
};

static hu_tool_t make_tools(hu_tool_t *buf) {
    (void)buf;
    buf[0] = (hu_tool_t){.ctx = NULL, .vtable = &echo_vtable};
    buf[1] = (hu_tool_t){.ctx = NULL, .vtable = &fail_vtable};
    return buf[0];
}

/* ── Op name tests ──────────────────────────────────────────────────────── */

static void hula_op_names_correct(void) {
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_CALL), "call");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_SEQ), "seq");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_PAR), "par");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_BRANCH), "branch");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_LOOP), "loop");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_DELEGATE), "delegate");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_EMIT), "emit");
    HU_ASSERT_STR_EQ(hu_hula_op_name(HU_HULA_TRY), "try");
}

static void hula_pred_names_correct(void) {
    HU_ASSERT_STR_EQ(hu_hula_pred_name(HU_HULA_PRED_SUCCESS), "success");
    HU_ASSERT_STR_EQ(hu_hula_pred_name(HU_HULA_PRED_FAILURE), "failure");
    HU_ASSERT_STR_EQ(hu_hula_pred_name(HU_HULA_PRED_CONTAINS), "contains");
    HU_ASSERT_STR_EQ(hu_hula_pred_name(HU_HULA_PRED_NOT_CONTAINS), "not_contains");
    HU_ASSERT_STR_EQ(hu_hula_pred_name(HU_HULA_PRED_ALWAYS), "always");
}

static void hula_status_names_correct(void) {
    HU_ASSERT_STR_EQ(hu_hula_status_name(HU_HULA_PENDING), "pending");
    HU_ASSERT_STR_EQ(hu_hula_status_name(HU_HULA_DONE), "done");
    HU_ASSERT_STR_EQ(hu_hula_status_name(HU_HULA_FAILED), "failed");
    HU_ASSERT_STR_EQ(hu_hula_status_name(HU_HULA_SKIPPED), "skipped");
    HU_ASSERT_STR_EQ(hu_hula_status_name(HU_HULA_CANCELLED), "cancelled");
}

/* ── Program lifecycle ──────────────────────────────────────────────────── */

static void hula_program_init_sets_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_program_init(&prog, alloc, "test_prog", 9);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prog.name, "test_prog");
    HU_ASSERT_EQ(prog.version, HU_HULA_VERSION);
    hu_hula_program_deinit(&prog);
}

static void hula_program_alloc_node_returns_node(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "test", 4);

    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_CALL, "n1");
    HU_ASSERT_NOT_NULL(n);
    HU_ASSERT_EQ(n->op, HU_HULA_CALL);
    HU_ASSERT_STR_EQ(n->id, "n1");
    HU_ASSERT_EQ(prog.node_count, 1u);

    hu_hula_program_deinit(&prog);
}

static void hula_program_alloc_node_respects_max(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "test", 4);

    for (size_t i = 0; i < HU_HULA_MAX_NODES; i++) {
        char id[16];
        (void)snprintf(id, sizeof(id), "n%zu", i);
        HU_ASSERT_NOT_NULL(hu_hula_program_alloc_node(&prog, HU_HULA_CALL, id));
    }
    HU_ASSERT_NULL(hu_hula_program_alloc_node(&prog, HU_HULA_CALL, "overflow"));

    hu_hula_program_deinit(&prog);
}

/* ── Parse JSON ─────────────────────────────────────────────────────────── */

static void hula_parse_simple_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"greet\",\"version\":1,"
                       "\"root\":{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\","
                       "\"args\":{\"text\":\"hello\"}}}";
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prog.name, "greet");
    HU_ASSERT_NOT_NULL(prog.root);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_CALL);
    HU_ASSERT_STR_EQ(prog.root->id, "c1");
    HU_ASSERT_STR_EQ(prog.root->tool_name, "echo");
    HU_ASSERT_NOT_NULL(prog.root->args_json);

    hu_hula_program_deinit(&prog);
}

static void hula_parse_seq_with_children(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"pipeline\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"a\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"b\"}}"
        "]}}";
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_SEQ);
    HU_ASSERT_EQ(prog.root->children_count, 2u);
    HU_ASSERT_STR_EQ(prog.root->children[0]->id, "c1");
    HU_ASSERT_STR_EQ(prog.root->children[1]->id, "c2");

    hu_hula_program_deinit(&prog);
}

static void hula_parse_branch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"cond\",\"root\":{\"op\":\"branch\",\"id\":\"b1\","
                       "\"pred\":\"success\","
                       "\"then\":{\"op\":\"call\",\"id\":\"t1\",\"tool\":\"echo\",\"args\":{}},"
                       "\"else\":{\"op\":\"call\",\"id\":\"e1\",\"tool\":\"echo\",\"args\":{}}"
                       "}}";
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_BRANCH);
    HU_ASSERT_EQ(prog.root->pred, HU_HULA_PRED_SUCCESS);
    HU_ASSERT_EQ(prog.root->children_count, 2u);

    hu_hula_program_deinit(&prog);
}

static void hula_parse_loop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"retry\",\"root\":{\"op\":\"loop\",\"id\":\"l1\","
        "\"pred\":\"always\",\"max_iter\":3,"
        "\"body\":{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"x\"}}"
        "}}";
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_LOOP);
    HU_ASSERT_EQ(prog.root->max_iter, 3u);
    HU_ASSERT_EQ(prog.root->pred, HU_HULA_PRED_ALWAYS);
    HU_ASSERT_EQ(prog.root->children_count, 1u);

    hu_hula_program_deinit(&prog);
}

static void hula_parse_delegate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"spawn\",\"root\":{\"op\":\"delegate\",\"id\":\"d1\","
                       "\"goal\":\"search the web\",\"model\":\"gpt-4\"}}";
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_DELEGATE);
    HU_ASSERT_STR_EQ(prog.root->goal, "search the web");
    HU_ASSERT_STR_EQ(prog.root->delegate_model, "gpt-4");

    hu_hula_program_deinit(&prog);
}

static void hula_parse_emit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"out\",\"root\":{\"op\":\"emit\",\"id\":\"e1\","
                       "\"emit_key\":\"response\",\"emit_value\":\"done\"}}";
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_EMIT);
    HU_ASSERT_STR_EQ(prog.root->emit_key, "response");
    HU_ASSERT_STR_EQ(prog.root->emit_value, "done");

    hu_hula_program_deinit(&prog);
}

static void hula_parse_invalid_json_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, "not json", 8, &prog);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void hula_parse_missing_root_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    const char *json = "{\"name\":\"empty\"}";
    hu_error_t err = hu_hula_parse_json(&alloc, json, strlen(json), &prog);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Validate ───────────────────────────────────────────────────────────── */

static void hula_validate_valid_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_CALL, "c1");
    n->tool_name = hu_strdup(&alloc, "echo");
    prog.root = n;

    const char *tools[] = {"echo", "fail_tool"};
    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, tools, 2, &v);
    HU_ASSERT_TRUE(v.valid);
    HU_ASSERT_EQ(v.diag_count, 0u);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

static void hula_validate_unknown_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_CALL, "c1");
    n->tool_name = hu_strdup(&alloc, "nonexistent");
    prog.root = n;

    const char *tools[] = {"echo"};
    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, tools, 1, &v);
    HU_ASSERT_FALSE(v.valid);
    HU_ASSERT_GT(v.diag_count, 0u);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

static void hula_validate_call_missing_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_CALL, "c1");
    prog.root = n;

    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, NULL, 0, &v);
    HU_ASSERT_FALSE(v.valid);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

static void hula_validate_seq_no_children(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_SEQ, "s1");
    prog.root = n;

    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, NULL, 0, &v);
    HU_ASSERT_FALSE(v.valid);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

static void hula_validate_no_root(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);

    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, NULL, 0, &v);
    HU_ASSERT_FALSE(v.valid);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

static void hula_validate_delegate_missing_goal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_DELEGATE, "d1");
    prog.root = n;

    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, NULL, 0, &v);
    HU_ASSERT_FALSE(v.valid);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

static void hula_validate_emit_missing_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_hula_program_init(&prog, alloc, "v", 1);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_EMIT, "e1");
    prog.root = n;

    hu_hula_validation_t v;
    hu_hula_validate(&prog, &alloc, NULL, 0, &v);
    HU_ASSERT_FALSE(v.valid);

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
}

/* ── Execute ────────────────────────────────────────────────────────────── */

static void hula_exec_simple_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"run\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"echo\",\"args\":{\"text\":\"hi\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);

    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "c1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(r->output, "hi");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_seq_propagates_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"pipe\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"first\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"second\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *rs = hu_hula_exec_result(&exec, "s1");
    HU_ASSERT_NOT_NULL(rs);
    HU_ASSERT_EQ(rs->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(rs->output, "second");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_call_substitutes_dollar_node_id_in_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"refs\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"first\",\"tool\":\"echo\",\"args\":{\"text\":\"alpha\"}},"
        "{\"op\":\"call\",\"id\":\"second\",\"tool\":\"echo\",\"args\":{\"text\":\"pre.$first."
        "suf\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r2 = hu_hula_exec_result(&exec, "second");
    HU_ASSERT_NOT_NULL(r2);
    HU_ASSERT_EQ(r2->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(r2->output, "pre.alpha.suf");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_call_dollar_ref_unknown_id_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"badref\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"echo\",\"args\":{\"text\":\"$noSuchId\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "c1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_FAILED);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_call_args_double_dollar_is_literal_dollar(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"esc\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"echo\",\"args\":{\"text\":\"$$5 and $$\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "c1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(r->output, "$5 and $");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_seq_short_circuits_on_failure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"fail_pipe\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"fail_tool\",\"args\":{}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"unreachable\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *rs = hu_hula_exec_result(&exec, "s1");
    HU_ASSERT_NOT_NULL(rs);
    HU_ASSERT_EQ(rs->status, HU_HULA_FAILED);

    const hu_hula_result_t *r2 = hu_hula_exec_result(&exec, "c2");
    HU_ASSERT_NOT_NULL(r2);
    HU_ASSERT_EQ(r2->status, HU_HULA_PENDING);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_par_runs_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"par\",\"root\":{\"op\":\"par\",\"id\":\"p1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"a\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"b\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c1")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c2")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "p1")->status, HU_HULA_DONE);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_branch_takes_then_on_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Seq: call echo (succeeds) → branch on success → then echo "yes" */
    const char *json =
        "{\"name\":\"br\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"ok\"}},"
        "{\"op\":\"branch\",\"id\":\"b1\",\"pred\":\"success\","
        "\"then\":{\"op\":\"call\",\"id\":\"t1\",\"tool\":\"echo\",\"args\":{\"text\":\"yes\"}},"
        "\"else\":{\"op\":\"call\",\"id\":\"e1\",\"tool\":\"echo\",\"args\":{\"text\":\"no\"}}"
        "}]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "t1")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "t1")->output, "yes");
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "e1")->status, HU_HULA_PENDING);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_branch_takes_else_on_failure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"br\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"fail_tool\",\"args\":{}},"
        "{\"op\":\"branch\",\"id\":\"b1\",\"pred\":\"success\","
        "\"then\":{\"op\":\"call\",\"id\":\"t1\",\"tool\":\"echo\",\"args\":{\"text\":\"yes\"}},"
        "\"else\":{\"op\":\"call\",\"id\":\"e1\",\"tool\":\"echo\",\"args\":{\"text\":\"fallback\"}"
        "}"
        "}]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    /* seq short-circuits on c1 failure, so branch doesn't run */
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "s1")->status, HU_HULA_FAILED);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_loop_bounded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"loop\",\"root\":{\"op\":\"loop\",\"id\":\"l1\","
        "\"pred\":\"always\",\"max_iter\":3,"
        "\"body\":{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"tick\"}}"
        "}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "l1")->status, HU_HULA_DONE);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_delegate_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"spawn\",\"root\":{\"op\":\"delegate\",\"id\":\"d1\","
                       "\"goal\":\"find docs\"}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "d1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(r->output, "find docs");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_emit_static_value(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"out\",\"root\":{\"op\":\"emit\",\"id\":\"e1\","
                       "\"emit_key\":\"result\",\"emit_value\":\"completed\"}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "e1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(r->output, "completed");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_call_unknown_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"bad\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"nonexistent\",\"args\":{}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c1")->status, HU_HULA_FAILED);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── Round-trip (parse → serialize → parse) ─────────────────────────────── */

static void hula_roundtrip_preserves_structure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"rt\",\"version\":1,"
                       "\"root\":{\"op\":\"seq\",\"id\":\"s1\","
                       "\"children\":["
                       "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"x\"}}"
                       "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_hula_to_json(&alloc, &prog, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_GT(out_len, 0);

    hu_hula_program_t prog2;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, out, out_len, &prog2), HU_OK);
    HU_ASSERT_STR_EQ(prog2.name, "rt");
    HU_ASSERT_EQ(prog2.root->op, HU_HULA_SEQ);
    HU_ASSERT_EQ(prog2.root->children_count, 1u);
    HU_ASSERT_STR_EQ(prog2.root->children[0]->tool_name, "echo");

    hu_str_free(&alloc, out);
    hu_hula_program_deinit(&prog2);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_emit_resolves_ref(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"ref\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"data\"}},"
        "{\"op\":\"emit\",\"id\":\"e1\",\"emit_key\":\"out\",\"emit_value\":\"$c1\"}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "e1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(r->output, "data");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_exec_nested_seq_par(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"nested\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"par\",\"id\":\"p1\",\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"a\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"b\"}}"
        "]},"
        "{\"op\":\"call\",\"id\":\"c3\",\"tool\":\"echo\",\"args\":{\"text\":\"final\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c1")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c2")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c3")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "s1")->output, "final");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── Bridge: plan → HuLa ────────────────────────────────────────────────── */

static void hula_from_plan_flat_seq(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_step_t steps[2] = {
        {.tool_name = "echo", .args_json = "{\"text\":\"a\"}", .description = "first"},
        {.tool_name = "echo", .args_json = "{\"text\":\"b\"}", .description = "second"},
    };
    hu_plan_t plan = {.steps = steps, .steps_count = 2, .steps_cap = 2};

    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_from_plan(&alloc, &plan, "flat", 4, &prog), HU_OK);
    HU_ASSERT_NOT_NULL(prog.root);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_SEQ);
    HU_ASSERT_EQ(prog.root->children_count, 2u);
    HU_ASSERT_STR_EQ(prog.root->children[0]->tool_name, "echo");
    HU_ASSERT_STR_EQ(prog.root->children[1]->tool_name, "echo");

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "s0")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "s1")->status, HU_HULA_DONE);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_from_plan_with_deps(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_step_t steps[3] = {
        {.tool_name = "echo", .args_json = "{\"text\":\"a\"}"},
        {.tool_name = "echo", .args_json = "{\"text\":\"b\"}"},
        {.tool_name = "echo",
         .args_json = "{\"text\":\"c\"}",
         .depends_count = 2,
         .depends_on = {0, 1}},
    };
    hu_plan_t plan = {.steps = steps, .steps_count = 3, .steps_cap = 3};

    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_from_plan(&alloc, &plan, "deps", 4, &prog), HU_OK);
    HU_ASSERT_NOT_NULL(prog.root);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_SEQ);
    /* Wave 0: s0 + s1 (parallel), Wave 1: s2 */
    HU_ASSERT_EQ(prog.root->children_count, 2u);
    HU_ASSERT_EQ(prog.root->children[0]->op, HU_HULA_PAR);
    HU_ASSERT_EQ(prog.root->children[0]->children_count, 2u);
    HU_ASSERT_EQ(prog.root->children[1]->op, HU_HULA_CALL);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "s2")->status, HU_HULA_DONE);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── Bridge: DAG → HuLa ────────────────────────────────────────────────── */

static void hula_from_dag_parallel_roots(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);
    hu_dag_add_node(&dag, "t0", "echo", "{\"text\":\"x\"}", NULL, 0);
    hu_dag_add_node(&dag, "t1", "echo", "{\"text\":\"y\"}", NULL, 0);
    hu_dag_add_node(&dag, "t2", "echo", "{\"text\":\"z\"}", (const char *[]){"t0", "t1"}, 2);

    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_from_dag(&alloc, &dag, "dagp", 4, &prog), HU_OK);
    HU_ASSERT_NOT_NULL(prog.root);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_SEQ);
    /* Wave 0: t0 + t1 (par), Wave 1: t2 */
    HU_ASSERT_EQ(prog.root->children_count, 2u);
    HU_ASSERT_EQ(prog.root->children[0]->op, HU_HULA_PAR);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "t2")->status, HU_HULA_DONE);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
    hu_dag_deinit(&dag);
}

/* ── Policy: locked blocks execution ────────────────────────────────────── */

static void hula_policy_locked_blocks_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"blocked\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"echo\",\"args\":{\"text\":\"nope\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.autonomy = HU_AUTONOMY_LOCKED;

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, &policy, NULL), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "c1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_FAILED);
    HU_ASSERT_STR_CONTAINS(r->error, "locked");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* Wave A: security_agent permission gate denies shell under READ_ONLY. */
static void hula_security_agent_denies_shell_under_read_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"sec\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"shell\",\"args\":{}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[3];
    make_tools(tools);
    tools[2] = (hu_tool_t){.ctx = NULL, .vtable = &shell_stub_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.permission_level = HU_PERM_READ_ONLY;

    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 3, NULL, NULL), HU_OK);
    hu_hula_exec_set_security_agent(&exec, &agent);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "c1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_FAILED);
    HU_ASSERT_STR_CONTAINS(r->error, "denied");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_policy_high_risk_blocked(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"risky\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"shell\",\"args\":{}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.autonomy = HU_AUTONOMY_AUTONOMOUS;
    policy.block_high_risk_commands = true;

    hu_tool_t tools[3];
    make_tools(tools);
    tools[2] = (hu_tool_t){.ctx = NULL, .vtable = &shell_stub_vtable};
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 3, &policy, NULL), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    const hu_hula_result_t *r = hu_hula_exec_result(&exec, "c1");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(r->status, HU_HULA_FAILED);
    HU_ASSERT_STR_CONTAINS(r->error, "blocked by policy: high risk");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── Trace: produces valid JSON array ───────────────────────────────────── */

static void hula_trace_records_execution(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"traced\",\"root\":{\"op\":\"seq\",\"id\":\"s1\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"hi\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"bye\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    size_t trace_len = 0;
    const char *trace = hu_hula_exec_trace(&exec, &trace_len);
    HU_ASSERT_NOT_NULL(trace);
    HU_ASSERT_GT(trace_len, 2u);
    HU_ASSERT_TRUE(trace[0] == '[');
    HU_ASSERT_TRUE(trace[trace_len - 1] == ']');
    HU_ASSERT_STR_CONTAINS(trace, "\"c1\"");
    HU_ASSERT_STR_CONTAINS(trace, "\"c2\"");
    HU_ASSERT_STR_CONTAINS(trace, "\"done\"");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── Observer: events are emitted ───────────────────────────────────────── */

typedef struct {
    int start_count;
    int call_count;
    bool last_success;
} test_obs_ctx_t;

static void test_obs_record(void *ctx, const hu_observer_event_t *ev) {
    test_obs_ctx_t *t = (test_obs_ctx_t *)ctx;
    if (ev->tag == HU_OBSERVER_EVENT_TOOL_CALL_START)
        t->start_count++;
    if (ev->tag == HU_OBSERVER_EVENT_TOOL_CALL) {
        t->call_count++;
        t->last_success = ev->data.tool_call.success;
    }
}

static const hu_observer_vtable_t test_obs_vtable = {
    .record_event = test_obs_record,
    .name = NULL,
    .flush = NULL,
    .deinit = NULL,
};

static void hula_observer_receives_events(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"obs\",\"root\":{\"op\":\"call\",\"id\":\"c1\","
                       "\"tool\":\"echo\",\"args\":{\"text\":\"hey\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);

    test_obs_ctx_t obs_ctx = {0};
    hu_observer_t obs = {.ctx = &obs_ctx, .vtable = &test_obs_vtable};

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, NULL, &obs), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    HU_ASSERT_EQ(obs_ctx.start_count, 1);
    HU_ASSERT_EQ(obs_ctx.call_count, 1);
    HU_ASSERT_TRUE(obs_ctx.last_success);

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── E2E: full pipeline parse → validate → policy → execute → trace ─── */

static void hula_e2e_full_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* 1. Parse a multi-step program from JSON */
    const char *json =
        "{\"name\":\"e2e_pipeline\",\"version\":1,"
        "\"root\":{\"op\":\"seq\",\"id\":\"main\","
        "\"children\":["
        "{\"op\":\"par\",\"id\":\"gather\",\"children\":["
        "{\"op\":\"call\",\"id\":\"fetch1\",\"tool\":\"echo\",\"args\":{\"text\":\"data_a\"}},"
        "{\"op\":\"call\",\"id\":\"fetch2\",\"tool\":\"echo\",\"args\":{\"text\":\"data_b\"}}"
        "]},"
        "{\"op\":\"branch\",\"id\":\"check\",\"pred\":\"success\","
        "\"then\":{\"op\":\"call\",\"id\":\"process\",\"tool\":\"echo\",\"args\":{\"text\":"
        "\"processed\"}},"
        "\"else\":{\"op\":\"emit\",\"id\":\"err_out\",\"emit_key\":\"error\",\"emit_value\":"
        "\"failed\"}"
        "},"
        "{\"op\":\"emit\",\"id\":\"result\",\"emit_key\":\"output\",\"emit_value\":\"$process\"}"
        "]}}";

    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    HU_ASSERT_STR_EQ(prog.name, "e2e_pipeline");

    /* 2. Validate against known tools */
    const char *tool_names[] = {"echo", "fail_tool"};
    hu_hula_validation_t v;
    HU_ASSERT_EQ(hu_hula_validate(&prog, &alloc, tool_names, 2, &v), HU_OK);
    HU_ASSERT_TRUE(v.valid);
    hu_hula_validation_deinit(&alloc, &v);

    /* 3. Serialize → re-parse (round-trip) */
    char *serialized = NULL;
    size_t serialized_len = 0;
    HU_ASSERT_EQ(hu_hula_to_json(&alloc, &prog, &serialized, &serialized_len), HU_OK);
    HU_ASSERT_STR_CONTAINS(serialized, "e2e_pipeline");
    hu_str_free(&alloc, serialized);

    /* 4. Execute with policy (autonomous, no blocking) + observer + trace */
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.autonomy = HU_AUTONOMY_AUTONOMOUS;

    test_obs_ctx_t obs_ctx = {0};
    hu_observer_t obs = {.ctx = &obs_ctx, .vtable = &test_obs_vtable};

    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, &policy, &obs), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);

    /* 5. Verify results across the entire tree */
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "fetch1")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "fetch1")->output, "data_a");
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "fetch2")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "fetch2")->output, "data_b");
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "gather")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "process")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "process")->output, "processed");
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "result")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "result")->output, "processed");
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "main")->status, HU_HULA_DONE);

    /* 6. Verify observer captured all tool calls */
    HU_ASSERT_EQ(obs_ctx.start_count, 3); /* fetch1, fetch2, process */
    HU_ASSERT_EQ(obs_ctx.call_count, 3);

    /* 7. Verify trace log is valid JSON with all nodes */
    size_t trace_len = 0;
    const char *trace = hu_hula_exec_trace(&exec, &trace_len);
    HU_ASSERT_NOT_NULL(trace);
    HU_ASSERT_TRUE(trace[0] == '[');
    HU_ASSERT_TRUE(trace[trace_len - 1] == ']');
    HU_ASSERT_STR_CONTAINS(trace, "\"fetch1\"");
    HU_ASSERT_STR_CONTAINS(trace, "\"fetch2\"");
    HU_ASSERT_STR_CONTAINS(trace, "\"process\"");
    HU_ASSERT_STR_CONTAINS(trace, "\"result\"");
    HU_ASSERT_STR_CONTAINS(trace, "\"main\"");

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

/* ── Compiler + native text helpers ─────────────────────────────────────── */

static void hula_compiler_build_prompt_lists_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[2];
    make_tools(tools);
    char *p = NULL;
    size_t plen = 0;
    HU_ASSERT_EQ(hu_hula_compiler_build_prompt(&alloc, "do thing", 8, tools, 1, NULL, 0, &p, &plen),
                 HU_OK);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strstr(p, "do thing") != NULL);
    HU_ASSERT_TRUE(strstr(p, "echo") != NULL);
    hu_str_free(&alloc, p);
}

static void hula_compiler_parse_accepts_fenced_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *raw =
        "Here:\n```json\n{\"name\":\"x\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"a\","
        "\"tool\":\"echo\",\"args\":{\"text\":\"hi\"}}}\n```\n";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_compiler_parse_response(&alloc, raw, strlen(raw), &prog), HU_OK);
    HU_ASSERT_STR_EQ(prog.name, "x");
    HU_ASSERT_NOT_NULL(prog.root);
    HU_ASSERT_EQ(prog.root->op, HU_HULA_CALL);
    hu_hula_program_deinit(&prog);
}

static void hula_extract_and_strip_program_tags(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *inner =
        "{\"name\":\"t\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"c\",\"tool\":\"echo\","
        "\"args\":{\"text\":\"z\"}}}";
    char wrapped[512];
    int wn =
        snprintf(wrapped, sizeof(wrapped), "Preamble <hula_program>%s</hula_program> tail", inner);
    HU_ASSERT_TRUE(wn > 0 && (size_t)wn < sizeof(wrapped));

    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_extract_program_from_text(&alloc, wrapped, (size_t)wn, &prog), HU_OK);
    HU_ASSERT_STR_EQ(prog.name, "t");
    hu_hula_program_deinit(&prog);

    char *stripped = NULL;
    size_t slen = 0;
    HU_ASSERT_EQ(hu_hula_strip_program_tags(&alloc, wrapped, (size_t)wn, &stripped, &slen), HU_OK);
    HU_ASSERT_NOT_NULL(stripped);
    HU_ASSERT_TRUE(strstr(stripped, "<hula_program>") == NULL);
    HU_ASSERT_TRUE(strstr(stripped, "Preamble") != NULL);
    HU_ASSERT_TRUE(strstr(stripped, "tail") != NULL);
    alloc.free(alloc.ctx, stripped, slen + 1);

    const char *slice = NULL;
    size_t slice_len = 0;
    HU_ASSERT_EQ(hu_hula_program_source_slice_from_text(wrapped, (size_t)wn, &slice, &slice_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(slice);
    HU_ASSERT_EQ(slice_len, strlen(inner));
    HU_ASSERT_TRUE(memcmp(slice, inner, slice_len) == 0);
}

static void hula_cost_estimate_bounds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *par_json = "{\"name\":\"p\",\"root\":{\"op\":\"par\",\"id\":\"r\",\"children\":["
                           "{\"op\":\"call\",\"id\":\"a\",\"tool\":\"echo\",\"args\":{}},"
                           "{\"op\":\"call\",\"id\":\"b\",\"tool\":\"echo\",\"args\":{}}]}}";
    hu_hula_program_t parp;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, par_json, strlen(par_json), &parp), HU_OK);
    hu_hula_cost_estimate_t est;
    hu_hula_estimate_cost(&parp, &est);
    HU_ASSERT_EQ(est.estimated_tool_calls, 2u);
    HU_ASSERT_EQ(est.max_parallel_width, 2u);
    hu_hula_program_deinit(&parp);

    const char *loop_json =
        "{\"name\":\"l\",\"root\":{\"op\":\"loop\",\"id\":\"L\",\"pred\":\"always\",\"max_iter\":2,"
        "\"body\":{\"op\":\"call\",\"id\":\"x\",\"tool\":\"echo\",\"args\":{}}}}";
    hu_hula_program_t loopp;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, loop_json, strlen(loop_json), &loopp), HU_OK);
    hu_hula_estimate_cost(&loopp, &est);
    HU_ASSERT_EQ(est.max_loop_iterations_bound, 2u);
    HU_ASSERT_EQ(est.estimated_tool_calls, 2u);
    hu_hula_program_deinit(&loopp);

    const char *del_json = "{\"name\":\"d\",\"root\":{\"op\":\"seq\",\"id\":\"s\",\"children\":["
                           "{\"op\":\"delegate\",\"id\":\"d1\",\"goal\":\"sub\"},"
                           "{\"op\":\"call\",\"id\":\"c\",\"tool\":\"echo\",\"args\":{}}]}}";
    hu_hula_program_t delp;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, del_json, strlen(del_json), &delp), HU_OK);
    hu_hula_estimate_cost(&delp, &est);
    HU_ASSERT_EQ(est.estimated_tool_calls, 2u);
    hu_hula_program_deinit(&delp);
}

static void hula_parse_delegate_with_program_children(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"d\",\"root\":{\"op\":\"delegate\",\"id\":\"root\",\"goal\":\"g\","
        "\"children\":[{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{}}]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    HU_ASSERT_EQ(prog.root->children_count, 1u);
    HU_ASSERT_STR_EQ(prog.root->children[0]->id, "c1");
    hu_hula_program_deinit(&prog);
}

/* Borrowed pool/spawn_cfg pointers must survive until hu_hula_exec_run completes. */
static void hula_exec_set_spawn_stores_borrowed_references(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&alloc, 2);
    HU_ASSERT_NOT_NULL(pool);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_program_init(&prog, alloc, "t", 1), HU_OK);
    hu_hula_node_t *n = hu_hula_program_alloc_node(&prog, HU_HULA_CALL, "c1");
    HU_ASSERT_NOT_NULL(n);
    n->tool_name = hu_strdup(&alloc, "echo");
    n->args_json = hu_strdup(&alloc, "{\"text\":\"x\"}");
    prog.root = n;

    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 1, NULL, NULL), HU_OK);
    hu_spawn_config_t sc;
    memset(&sc, 0, sizeof(sc));
    hu_hula_exec_set_spawn(&exec, pool, &sc);
    HU_ASSERT_EQ((void *)exec.pool, (void *)pool);
    HU_ASSERT_EQ((void *)exec.spawn_cfg, (void *)&sc);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
    hu_agent_pool_destroy(pool);
}

#if defined(__unix__) || defined(__APPLE__)
static void hula_emergence_persist_scan_promote(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char td[] = "/tmp/hu_hula_em_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(td));

    char tr[512];
    (void)snprintf(tr, sizeof(tr), "%s/traces", td);
    HU_ASSERT_EQ(mkdir(tr, 0755), 0);

    char sk[512];
    (void)snprintf(sk, sizeof(sk), "%s/skills", td);

    static const char *trace_body =
        "{\"version\":1,\"success\":true,\"trace\":["
        "{\"op\":\"call\",\"tool\":\"echo\"},{\"op\":\"call\",\"tool\":\"grep\"}]}";

    char fpath[640];
    (void)snprintf(fpath, sizeof(fpath), "%s/a.json", tr);
    FILE *f1 = fopen(fpath, "wb");
    HU_ASSERT_NOT_NULL(f1);
    fputs(trace_body, f1);
    fclose(f1);
    (void)snprintf(fpath, sizeof(fpath), "%s/b.json", tr);
    FILE *f2 = fopen(fpath, "wb");
    HU_ASSERT_NOT_NULL(f2);
    fputs(trace_body, f2);
    fclose(f2);

    const char *tr_json = "[{\"op\":\"call\",\"tool\":\"z\"}]";
    HU_ASSERT_EQ(hu_hula_trace_persist(&alloc, tr, tr_json, strlen(tr_json), "unitprog", 8, true,
                                       NULL, 0, NULL, 0),
                 HU_OK);

    char **patterns = NULL;
    size_t pc = 0;
    size_t *freqs = NULL;
    HU_ASSERT_EQ(hu_hula_emergence_scan(&alloc, tr, 2, 2, &patterns, &pc, &freqs), HU_OK);
    HU_ASSERT_TRUE(pc >= 1u);
    bool found = false;
    for (size_t i = 0; i < pc; i++) {
        if (patterns[i] && strcmp(patterns[i], "echo|grep") == 0 && freqs[i] >= 2) {
            found = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found);
    for (size_t i = 0; i < pc; i++)
        alloc.free(alloc.ctx, patterns[i], strlen(patterns[i]) + 1);
    alloc.free(alloc.ctx, patterns, pc * sizeof(char *));
    alloc.free(alloc.ctx, freqs, pc * sizeof(size_t));

    HU_ASSERT_EQ(hu_hula_emergence_promote(&alloc, sk, "echo|grep", 9, "emerged_skill", 13), HU_OK);

    char skill_json_path[640];
    char skill_md_path[640];
    (void)snprintf(skill_json_path, sizeof(skill_json_path), "%s/emerged_skill.skill.json", sk);
    (void)snprintf(skill_md_path, sizeof(skill_md_path), "%s/emerged_skill_HULA.md", sk);
    HU_ASSERT_EQ(access(skill_json_path, R_OK), 0);
    HU_ASSERT_EQ(access(skill_md_path, R_OK), 0);

    DIR *d = opendir(tr);
    HU_ASSERT_NOT_NULL(d);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char delp[768];
        (void)snprintf(delp, sizeof(delp), "%s/%s", tr, de->d_name);
        (void)unlink(delp);
    }
    closedir(d);
    (void)rmdir(tr);

    (void)unlink(skill_json_path);
    (void)unlink(skill_md_path);
    (void)rmdir(sk);
    (void)rmdir(td);
}

static void hula_trace_persist_embeds_program_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char td[] = "/tmp/hu_hula_progemb_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(td));
    const char *trace_a = "[{\"op\":\"call\",\"id\":\"z\",\"tool\":\"echo\"}]";
    const char *prog =
        "{\"name\":\"emb\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"c\",\"tool\":\"echo\","
        "\"args\":{\"text\":\"x\"}}}";
    HU_ASSERT_EQ(hu_hula_trace_persist(&alloc, td, trace_a, strlen(trace_a), "emb", 3, true, prog,
                                       strlen(prog), NULL, 0),
                 HU_OK);
    DIR *d = opendir(td);
    HU_ASSERT_NOT_NULL(d);
    char path[640];
    path[0] = '\0';
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        (void)snprintf(path, sizeof(path), "%s/%s", td, de->d_name);
        break;
    }
    closedir(d);
    HU_ASSERT_TRUE(path[0] != '\0');
    FILE *fp = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t nr = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[nr] = '\0';
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, buf, nr, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_json_value_t *po = hu_json_object_get(root, "program");
    HU_ASSERT_NOT_NULL(po);
    HU_ASSERT_TRUE(po->type == HU_JSON_OBJECT);
    const char *nm = hu_json_get_string(po, "name");
    HU_ASSERT_TRUE(nm && strcmp(nm, "emb") == 0);
    hu_json_free(&alloc, root);
    (void)unlink(path);
    (void)rmdir(td);
}

static void hula_trace_persist_embeds_program_from_to_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"tj\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"c\",\"tool\":\"echo\","
        "\"args\":{\"text\":\"y\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    char *pj = NULL;
    size_t pjl = 0;
    HU_ASSERT_EQ(hu_hula_to_json(&alloc, &prog, &pj, &pjl), HU_OK);
    HU_ASSERT_NOT_NULL(pj);
    char td[] = "/tmp/hu_hula_tjsnap_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(td));
    HU_ASSERT_EQ(hu_hula_trace_persist(&alloc, td, "[]", 2, "tj", 2, true, pj, pjl, NULL, 0),
                 HU_OK);
    hu_str_free(&alloc, pj);
    hu_hula_program_deinit(&prog);

    DIR *d = opendir(td);
    HU_ASSERT_NOT_NULL(d);
    char path[640];
    path[0] = '\0';
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        (void)snprintf(path, sizeof(path), "%s/%s", td, de->d_name);
        break;
    }
    closedir(d);
    HU_ASSERT_TRUE(path[0] != '\0');
    FILE *fp = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t nr = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[nr] = '\0';
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, buf, nr, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_json_value_t *po = hu_json_object_get(root, "program");
    HU_ASSERT_NOT_NULL(po);
    const char *nm = hu_json_get_string(po, "name");
    HU_ASSERT_TRUE(nm && strcmp(nm, "tj") == 0);
    hu_json_free(&alloc, root);
    (void)unlink(path);
    (void)rmdir(td);
}

static void hula_trace_persist_embeds_program_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *trace_a = "[]";
    const char *prog =
        "{\"name\":\"ps\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"c\",\"tool\":\"echo\","
        "\"args\":{\"text\":\"x\"}}}";
    const char *src = "{\"audit\":\"model\"}";
    char td[] = "/tmp/hu_hula_psrc_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(td));
    HU_ASSERT_EQ(hu_hula_trace_persist(&alloc, td, trace_a, strlen(trace_a), "ps", 2, true, prog,
                                       strlen(prog), src, strlen(src)),
                 HU_OK);
    DIR *d = opendir(td);
    HU_ASSERT_NOT_NULL(d);
    char path[640];
    path[0] = '\0';
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        (void)snprintf(path, sizeof(path), "%s/%s", td, de->d_name);
        break;
    }
    closedir(d);
    HU_ASSERT_TRUE(path[0] != '\0');
    FILE *fp = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(fp);
    char buf[4096];
    size_t nr = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[nr] = '\0';
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, buf, nr, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    const char *got = hu_json_get_string(root, "program_source");
    HU_ASSERT_TRUE(got && strcmp(got, src) == 0);
    hu_json_free(&alloc, root);
    (void)unlink(path);
    (void)rmdir(td);
}
#endif

typedef struct {
    const char *json;
} hula_compiler_mock_chat_ctx_t;

static hu_error_t hula_compiler_mock_chat(void *ctx, hu_allocator_t *alloc,
                                          const hu_chat_request_t *req, const char *model,
                                          size_t model_len, double temperature,
                                          hu_chat_response_t *out) {
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    const hula_compiler_mock_chat_ctx_t *c = (const hula_compiler_mock_chat_ctx_t *)ctx;
    size_t n = strlen(c->json);
    char *dup = hu_strndup(alloc, c->json, n);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out, 0, sizeof(*out));
    out->content = dup;
    out->content_len = n;
    return HU_OK;
}

static void hula_compiler_chat_compile_execute_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[2];
    (void)make_tools(tools);
    static const char json[] =
        "{\"name\":\"mc\",\"version\":1,\"root\":{\"op\":\"par\",\"id\":\"r\",\"children\":["
        "{\"op\":\"call\",\"id\":\"a\",\"tool\":\"echo\",\"args\":{\"text\":\"x\"}},"
        "{\"op\":\"call\",\"id\":\"b\",\"tool\":\"echo\",\"args\":{\"text\":\"y\"}}]}}";
    hula_compiler_mock_chat_ctx_t mctx = {.json = json};
    bool ok = false;
    HU_ASSERT_EQ(hu_hula_compiler_chat_compile_execute(&alloc, "goal", 4, tools, 2, NULL, NULL,
                                                       NULL, NULL, hula_compiler_mock_chat, &mctx,
                                                       "gpt-4", 5, 0.7, NULL, 0, NULL, NULL, &ok),
                 HU_OK);
    HU_ASSERT_TRUE(ok);
}

static void hula_exec_try_catch_invokes_catch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"t\",\"version\":1,\"root\":{\"op\":\"try\",\"id\":\"tr\","
                       "\"body\":{\"op\":\"call\",\"id\":\"b\",\"tool\":\"fail_tool\",\"args\":{}},"
                       "\"catch\":{\"op\":\"call\",\"id\":\"c\",\"tool\":\"echo\",\"args\":{"
                       "\"text\":\"recovered\"}}"
                       "}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "c")->output, "recovered");
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_emit_slot_resolves_in_call_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"s\",\"version\":1,\"root\":{\"op\":\"seq\",\"id\":\"root\","
        "\"children\":["
        "{\"op\":\"emit\",\"id\":\"e1\",\"emit_key\":\"r\",\"emit_value\":\"hello\"},"
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"$r\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "c1")->output, "hello");
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_budget_tool_calls_stop_second(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"b\",\"version\":1,\"root\":{\"op\":\"seq\",\"id\":\"s\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"a\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"b\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 2), HU_OK);
    hu_hula_exec_set_budget(&exec, 0, 0, 1);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c1")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c2")->status, HU_HULA_FAILED);
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_required_capability_denied(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"p\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\","
        "\"args\":{\"text\":\"x\"},\"required_capability\":\"shell\"}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_security_policy_t pol;
    memset(&pol, 0, sizeof(pol));
    pol.autonomy = HU_AUTONOMY_AUTONOMOUS;
    pol.hula_capability_allowlist = "net";
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, &pol, NULL), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c1")->status, HU_HULA_FAILED);
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

typedef struct {
    hu_hula_exec_t *ex;
} hula_cancel_obs_ctx_t;

static void hula_cancel_obs_record(void *ctx, const hu_observer_event_t *ev) {
    hula_cancel_obs_ctx_t *c = (hula_cancel_obs_ctx_t *)ctx;
    if (ev->tag == HU_OBSERVER_EVENT_HULA_NODE_START && ev->data.hula_node_start.node_id &&
        strcmp(ev->data.hula_node_start.node_id, "c2") == 0 && c->ex)
        hu_hula_exec_cancel(c->ex, "stop", 4);
}

static const hu_observer_vtable_t hula_cancel_obs_vtable = {
    .record_event = hula_cancel_obs_record,
    .name = NULL,
    .flush = NULL,
    .deinit = NULL,
};

static void hula_exec_cancel_marks_following_node(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"x\",\"version\":1,\"root\":{\"op\":\"seq\",\"id\":\"s\","
        "\"children\":["
        "{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\",\"args\":{\"text\":\"a\"}},"
        "{\"op\":\"call\",\"id\":\"c2\",\"tool\":\"echo\",\"args\":{\"text\":\"b\"}}"
        "]}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_hula_exec_t exec;
    hula_cancel_obs_ctx_t cctx = {.ex = &exec};
    hu_observer_t obs = {.ctx = &cctx, .vtable = &hula_cancel_obs_vtable};
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, NULL, &obs), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c1")->status, HU_HULA_DONE);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "c2")->status, HU_HULA_CANCELLED);
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static int hula_lifecycle_ns;
static int hula_lifecycle_ne;
static int hula_lifecycle_pe;

static void hula_lifecycle_obs_record(void *ctx, const hu_observer_event_t *ev) {
    (void)ctx;
    if (ev->tag == HU_OBSERVER_EVENT_HULA_NODE_START)
        hula_lifecycle_ns++;
    if (ev->tag == HU_OBSERVER_EVENT_HULA_NODE_END)
        hula_lifecycle_ne++;
    if (ev->tag == HU_OBSERVER_EVENT_HULA_PROGRAM_END)
        hula_lifecycle_pe++;
}

static const hu_observer_vtable_t hula_lifecycle_obs_vtable = {
    .record_event = hula_lifecycle_obs_record,
    .name = NULL,
    .flush = NULL,
    .deinit = NULL,
};

static void hula_observer_emits_hula_lifecycle(void) {
    hula_lifecycle_ns = hula_lifecycle_ne = hula_lifecycle_pe = 0;
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"o\",\"root\":{\"op\":\"call\",\"id\":\"c1\",\"tool\":\"echo\","
                       "\"args\":{\"text\":\"z\"}}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[2];
    make_tools(tools);
    hu_observer_t obs = {.ctx = NULL, .vtable = &hula_lifecycle_obs_vtable};
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init_full(&exec, alloc, &prog, tools, 2, NULL, &obs), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hula_lifecycle_ns, 1);
    HU_ASSERT_EQ(hula_lifecycle_ne, 1);
    HU_ASSERT_EQ(hula_lifecycle_pe, 1);
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_retry_flaky_eventually_succeeds(void) {
    flaky_hits = 0;
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"f\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"f1\","
                       "\"tool\":\"flaky_tool\","
                       "\"args\":{\"text\":\"done\"},\"retry_count\":2}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    hu_tool_t tools[3];
    make_tools(tools);
    tools[2] = (hu_tool_t){.ctx = NULL, .vtable = &flaky_vtable};
    hu_hula_exec_t exec;
    HU_ASSERT_EQ(hu_hula_exec_init(&exec, alloc, &prog, tools, 3), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_run(&exec), HU_OK);
    HU_ASSERT_EQ(hu_hula_exec_result(&exec, "f1")->status, HU_HULA_DONE);
    HU_ASSERT_STR_EQ(hu_hula_exec_result(&exec, "f1")->output, "done");
    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
}

static void hula_expand_template_substitutes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *tmpl = "Hello {{name}}!";
    const char *vars = "{\"name\":\"HuLa\"}";
    char *out = NULL;
    size_t ol = 0;
    HU_ASSERT_EQ(hu_hula_expand_template(&alloc, tmpl, strlen(tmpl), vars, strlen(vars), &out, &ol),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "Hello HuLa!");
    hu_str_free(&alloc, out);
}

static void hula_lite_produces_valid_program(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *src = "program hello\nseq root\n  call hi echo\n    text Hello from HuLa lite\n";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_hula_lite_to_json(&alloc, src, strlen(src), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "\"name\":\"hello\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"op\":\"seq\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"op\":\"call\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"tool\":\"echo\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"text\":\"Hello from HuLa lite\"") != NULL);
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, out, out_len, &prog), HU_OK);
    HU_ASSERT_STR_EQ(prog.name, "hello");
    hu_hula_program_deinit(&prog);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void hula_compiler_prompt_includes_finance_domain_example(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[2];
    (void)make_tools(tools);
    char *p = NULL;
    size_t pl = 0;
    HU_ASSERT_EQ(hu_hula_compiler_build_prompt(&alloc, "goal", 4, tools, 1, "finance", 7, &p, &pl),
                 HU_OK);
    HU_ASSERT_TRUE(strstr(p, "Domain example:") != NULL);
    hu_str_free(&alloc, p);
}

static int hula_repair_chat_i;

static hu_error_t hula_repair_mock_chat(void *ctx, hu_allocator_t *alloc,
                                        const hu_chat_request_t *req, const char *model,
                                        size_t model_len, double temperature,
                                        hu_chat_response_t *out) {
    (void)ctx;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)req;
    hula_repair_chat_i++;
    const char *j =
        (hula_repair_chat_i == 1)
            ? "{\"name\":\"bad\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"a\",\"tool\":"
              "\"nope_tool\",\"args\":{}}}"
            : "{\"name\":\"ok\",\"version\":1,\"root\":{\"op\":\"call\",\"id\":\"r\",\"tool\":"
              "\"echo\",\"args\":{\"text\":\"fixed\"}}}";
    size_t n = strlen(j);
    char *dup = hu_strndup(alloc, j, n);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out, 0, sizeof(*out));
    out->content = dup;
    out->content_len = n;
    return HU_OK;
}

static void hula_compiler_repair_validates_on_second_attempt(void) {
    hula_repair_chat_i = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tools[2];
    (void)make_tools(tools);
    bool ok = false;
    HU_ASSERT_EQ(hu_hula_compiler_chat_compile_execute(&alloc, "g", 1, tools, 2, NULL, NULL, NULL,
                                                       NULL, hula_repair_mock_chat, NULL, "m", 1,
                                                       0.0, NULL, 0, NULL, NULL, &ok),
                 HU_OK);
    HU_ASSERT_TRUE(ok);
    HU_ASSERT_EQ(hula_repair_chat_i, 2);
}

static void hula_delegate_handoff_fields_roundtrip_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"d\",\"version\":1,\"root\":{\"op\":\"delegate\",\"id\":\"dg\",\"goal\":\"do\","
        "\"context\":\"ctx\",\"result_key\":\"rk\",\"agent_id\":\"agentA\"}}";
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, strlen(json), &prog), HU_OK);
    char *out = NULL;
    size_t ol = 0;
    HU_ASSERT_EQ(hu_hula_to_json(&alloc, &prog, &out, &ol), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "ctx") != NULL);
    HU_ASSERT_TRUE(strstr(out, "result_key") != NULL);
    HU_ASSERT_TRUE(strstr(out, "agentA") != NULL);
    hu_str_free(&alloc, out);
    hu_hula_program_deinit(&prog);
}

#if defined(__unix__) || defined(__APPLE__)
static void hula_analytics_summarize_empty_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char tpl[] = "/tmp/hula_an_XXXXXX";
    HU_ASSERT_TRUE(mkdtemp(tpl) != NULL);
    char *sum = NULL;
    size_t sl = 0;
    HU_ASSERT_EQ(hu_hula_analytics_summarize(&alloc, tpl, &sum, &sl), HU_OK);
    HU_ASSERT_TRUE(strstr(sum, "\"file_count\":0") != NULL);
    hu_str_free(&alloc, sum);
    (void)rmdir(tpl);
}
#endif

void run_hula_tests(void) {
    HU_TEST_SUITE("hula");

    HU_RUN_TEST(hula_op_names_correct);
    HU_RUN_TEST(hula_pred_names_correct);
    HU_RUN_TEST(hula_status_names_correct);

    HU_RUN_TEST(hula_program_init_sets_name);
    HU_RUN_TEST(hula_program_alloc_node_returns_node);
    HU_RUN_TEST(hula_program_alloc_node_respects_max);

    HU_RUN_TEST(hula_parse_simple_call);
    HU_RUN_TEST(hula_parse_seq_with_children);
    HU_RUN_TEST(hula_parse_branch);
    HU_RUN_TEST(hula_parse_loop);
    HU_RUN_TEST(hula_parse_delegate);
    HU_RUN_TEST(hula_parse_emit);
    HU_RUN_TEST(hula_parse_invalid_json_returns_error);
    HU_RUN_TEST(hula_parse_missing_root_returns_error);

    HU_RUN_TEST(hula_validate_valid_call);
    HU_RUN_TEST(hula_validate_unknown_tool);
    HU_RUN_TEST(hula_validate_call_missing_tool);
    HU_RUN_TEST(hula_validate_seq_no_children);
    HU_RUN_TEST(hula_validate_no_root);
    HU_RUN_TEST(hula_validate_delegate_missing_goal);
    HU_RUN_TEST(hula_validate_emit_missing_key);

    HU_RUN_TEST(hula_exec_simple_call);
    HU_RUN_TEST(hula_exec_seq_propagates_output);
    HU_RUN_TEST(hula_exec_call_substitutes_dollar_node_id_in_args);
    HU_RUN_TEST(hula_exec_call_dollar_ref_unknown_id_fails);
    HU_RUN_TEST(hula_exec_call_args_double_dollar_is_literal_dollar);
    HU_RUN_TEST(hula_exec_seq_short_circuits_on_failure);
    HU_RUN_TEST(hula_exec_par_runs_all);
    HU_RUN_TEST(hula_exec_branch_takes_then_on_success);
    HU_RUN_TEST(hula_exec_branch_takes_else_on_failure);
    HU_RUN_TEST(hula_exec_loop_bounded);
    HU_RUN_TEST(hula_exec_delegate_stub);
    HU_RUN_TEST(hula_exec_emit_static_value);
    HU_RUN_TEST(hula_exec_call_unknown_tool);
    HU_RUN_TEST(hula_roundtrip_preserves_structure);
    HU_RUN_TEST(hula_exec_emit_resolves_ref);
    HU_RUN_TEST(hula_exec_nested_seq_par);
    HU_RUN_TEST(hula_exec_try_catch_invokes_catch);
    HU_RUN_TEST(hula_emit_slot_resolves_in_call_args);
    HU_RUN_TEST(hula_budget_tool_calls_stop_second);
    HU_RUN_TEST(hula_required_capability_denied);
    HU_RUN_TEST(hula_exec_cancel_marks_following_node);
    HU_RUN_TEST(hula_observer_emits_hula_lifecycle);
    HU_RUN_TEST(hula_retry_flaky_eventually_succeeds);
    HU_RUN_TEST(hula_expand_template_substitutes);
    HU_RUN_TEST(hula_lite_produces_valid_program);
    HU_RUN_TEST(hula_delegate_handoff_fields_roundtrip_json);

    HU_RUN_TEST(hula_from_plan_flat_seq);
    HU_RUN_TEST(hula_from_plan_with_deps);
    HU_RUN_TEST(hula_from_dag_parallel_roots);

    HU_RUN_TEST(hula_policy_locked_blocks_call);
    HU_RUN_TEST(hula_security_agent_denies_shell_under_read_only);
    HU_RUN_TEST(hula_policy_high_risk_blocked);

    HU_RUN_TEST(hula_trace_records_execution);
    HU_RUN_TEST(hula_observer_receives_events);

    HU_RUN_TEST(hula_e2e_full_pipeline);

    HU_RUN_TEST(hula_compiler_build_prompt_lists_tools);
    HU_RUN_TEST(hula_compiler_prompt_includes_finance_domain_example);
    HU_RUN_TEST(hula_compiler_chat_compile_execute_mock);
    HU_RUN_TEST(hula_compiler_repair_validates_on_second_attempt);
    HU_RUN_TEST(hula_compiler_parse_accepts_fenced_json);
    HU_RUN_TEST(hula_extract_and_strip_program_tags);
    HU_RUN_TEST(hula_cost_estimate_bounds);
    HU_RUN_TEST(hula_parse_delegate_with_program_children);
    HU_RUN_TEST(hula_exec_set_spawn_stores_borrowed_references);
#if defined(__unix__) || defined(__APPLE__)
    HU_RUN_TEST(hula_analytics_summarize_empty_dir);
    HU_RUN_TEST(hula_emergence_persist_scan_promote);
    HU_RUN_TEST(hula_trace_persist_embeds_program_field);
    HU_RUN_TEST(hula_trace_persist_embeds_program_from_to_json);
    HU_RUN_TEST(hula_trace_persist_embeds_program_source);
#endif
}
