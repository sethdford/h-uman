#include "human/core/process_util.h"
#include "synthetic_harness.h"

static const char HU_SYNTH_CLI_PROMPT[] =
    "You are a test case generator for the human CLI tool.\n"
    "Generate %d diverse test cases that exercise different CLI commands.\n\n"
    "Return a JSON array where each element has:\n"
    "- \"name\": descriptive test name (snake_case)\n"
    "- \"argv\": array of CLI arguments (e.g. [\"version\"], [\"status\"])\n"
    "- \"expected_exit_code\": 0 for success, non-zero for expected errors\n"
    "- \"expected_stdout_contains\": array of strings that should appear in stdout\n\n"
    "Available commands:\n"
    "- version: prints version containing \"human\"\n"
    "- help / --help / -h: prints usage containing \"Usage:\"\n"
    "- status: prints runtime status\n"
    "- doctor: runs diagnostics\n"
    "- capabilities: shows capabilities\n"
    "- capabilities --json: capabilities as JSON\n"
    "- models list: lists models\n"
    "- memory stats: memory statistics\n"
    "- channel list: lists channels\n"
    "- hardware list: lists peripherals\n"
    "- sandbox: sandbox status\n"
    "- workspace show: current workspace\n\n"
    "Mix: 60%% happy path, 20%% edge cases, 20%% error cases.\n"
    "Return ONLY a JSON array.";

hu_error_t hu_synth_run_cli(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                            hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics) {
    HU_SYNTH_LOG("=== CLI Tests ===");
    hu_synth_metrics_init(metrics);
    int count = cfg->tests_per_category > 0 ? cfg->tests_per_category : 20;
    char prompt[4096];
    snprintf(prompt, sizeof(prompt), HU_SYNTH_CLI_PROMPT, count);
    HU_SYNTH_LOG("generating %d CLI test cases via Gemini...", count);
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err =
        hu_synth_gemini_generate(alloc, gemini, prompt, strlen(prompt), &response, &response_len);
    if (err != HU_OK) {
        HU_SYNTH_LOG("Gemini generation failed: %s", hu_error_string(err));
        return err;
    }
    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, response, response_len, &root);
    hu_synth_strfree(alloc, response, response_len);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY) {
        HU_SYNTH_LOG("failed to parse test cases");
        if (root)
            hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }
    size_t n = root->data.array.len;
    HU_SYNTH_LOG("executing %zu CLI tests...", n);
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        const char *name = hu_json_get_string(item, "name");
        hu_json_value_t *argv_arr = hu_json_object_get(item, "argv");
        int exp_exit = (int)hu_json_get_number(item, "expected_exit_code", 0.0);
        if (!argv_arr || argv_arr->type != HU_JSON_ARRAY)
            continue;
        size_t argc = argv_arr->data.array.len;
        const char **argv = (const char **)alloc->alloc(alloc->ctx, (argc + 2) * sizeof(char *));
        if (!argv)
            continue;
        argv[0] = cfg->binary_path;
        for (size_t j = 0; j < argc; j++) {
            hu_json_value_t *a = argv_arr->data.array.items[j];
            argv[j + 1] = (a->type == HU_JSON_STRING) ? a->data.string.ptr : "";
        }
        argv[argc + 1] = NULL;
        double t0 = hu_synth_now_ms();
        hu_run_result_t result = {0};
        err = hu_process_run(alloc, argv, NULL, 1024 * 1024, &result);
        double lat = hu_synth_now_ms() - t0;
        alloc->free(alloc->ctx, (void *)argv, (argc + 2) * sizeof(char *));
        hu_synth_verdict_t v = HU_SYNTH_PASS;
        if (err != HU_OK) {
            v = HU_SYNTH_ERROR;
        } else if (result.exit_code != exp_exit) {
            v = HU_SYNTH_FAIL;
        } else {
            hu_json_value_t *contains = hu_json_object_get(item, "expected_stdout_contains");
            if (contains && contains->type == HU_JSON_ARRAY && result.stdout_buf) {
                for (size_t k = 0; k < contains->data.array.len; k++) {
                    hu_json_value_t *pat = contains->data.array.items[k];
                    if (pat->type == HU_JSON_STRING &&
                        !strstr(result.stdout_buf, pat->data.string.ptr)) {
                        v = HU_SYNTH_FAIL;
                        break;
                    }
                }
            }
        }
        hu_synth_metrics_record(alloc, metrics, lat, v);
        if (v == HU_SYNTH_PASS) {
            HU_SYNTH_VERBOSE(cfg, "PASS  %s (%.1fms)", name ? name : "?", lat);
        } else {
            HU_SYNTH_LOG("%-5s %s (exit=%d exp=%d)", hu_synth_verdict_str(v), name ? name : "?",
                         result.exit_code, exp_exit);
            if (cfg->regression_dir) {
                char *ij = NULL;
                size_t il = 0;
                (void)hu_json_stringify(alloc, item, &ij, &il);
                char reason[256];
                snprintf(reason, sizeof(reason), "exit=%d expected=%d", result.exit_code, exp_exit);
                hu_synth_test_case_t tc = {.name = (char *)(name ? name : "cli_test"),
                                           .category = (char *)"cli",
                                           .input_json = ij,
                                           .verdict = v,
                                           .verdict_reason = reason,
                                           .latency_ms = lat};
                hu_synth_regression_save(alloc, cfg->regression_dir, &tc);
                if (ij)
                    alloc->free(alloc->ctx, ij, il);
            }
        }
        hu_run_result_free(alloc, &result);
    }
    hu_json_free(alloc, root);
    return HU_OK;
}
