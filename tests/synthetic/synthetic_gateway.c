#include "human/core/http.h"
#include "synthetic_harness.h"

static const char HU_SYNTH_GW_PROMPT[] =
    "You are a test case generator for the human HTTP gateway.\n"
    "Generate %d diverse test cases for HTTP endpoints.\n\n"
    "Return a JSON array where each element has:\n"
    "- \"name\": test name (snake_case)\n"
    "- \"method\": \"GET\" or \"POST\"\n"
    "- \"path\": URL path (e.g. \"/health\")\n"
    "- \"body\": request body (empty for GET)\n"
    "- \"expected_status\": HTTP status code\n"
    "- \"expected_body_contains\": array of expected strings in body\n\n"
    "Endpoints: GET /health->200, GET /ready->200, GET /v1/models->200,\n"
    "GET /api/status->200, POST /v1/chat/completions->200,\n"
    "invalid paths->404, malformed JSON->400.\n"
    "Mix: 40%% happy, 30%% edge, 30%% error.\n"
    "Return ONLY a JSON array.";

hu_error_t hu_synth_run_gateway(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                                hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics) {
    HU_SYNTH_LOG("=== Gateway HTTP Tests ===");
    hu_synth_metrics_init(metrics);
    int count = cfg->tests_per_category > 0 ? cfg->tests_per_category : 20;
    char prompt[4096];
    snprintf(prompt, sizeof(prompt), HU_SYNTH_GW_PROMPT, count);
    HU_SYNTH_LOG("generating %d gateway test cases via Gemini...", count);
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
    HU_SYNTH_LOG("executing %zu gateway tests...", n);
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        const char *name = hu_json_get_string(item, "name");
        const char *method = hu_json_get_string(item, "method");
        const char *path = hu_json_get_string(item, "path");
        const char *body_str = hu_json_get_string(item, "body");
        int exp_status = (int)hu_json_get_number(item, "expected_status", 200.0);
        if (!method || !path)
            continue;
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u%s", cfg->gateway_port, path);
        hu_http_response_t resp = {0};
        double t0 = hu_synth_now_ms();
        if (strcmp(method, "GET") == 0) {
            err = hu_http_get(alloc, url, NULL, &resp);
        } else {
            size_t bl = body_str ? strlen(body_str) : 0;
            err = hu_http_post_json(alloc, url, NULL, body_str ? body_str : "", bl, &resp);
        }
        double lat = hu_synth_now_ms() - t0;
        hu_synth_verdict_t v = HU_SYNTH_PASS;
        if (err != HU_OK) {
            v = HU_SYNTH_ERROR;
        } else if (resp.status_code != exp_status) {
            v = HU_SYNTH_FAIL;
        } else {
            hu_json_value_t *contains = hu_json_object_get(item, "expected_body_contains");
            if (contains && contains->type == HU_JSON_ARRAY && resp.body) {
                for (size_t k = 0; k < contains->data.array.len; k++) {
                    hu_json_value_t *pat = contains->data.array.items[k];
                    if (pat->type == HU_JSON_STRING && !strstr(resp.body, pat->data.string.ptr)) {
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
            HU_SYNTH_LOG("%-5s %s (status=%ld exp=%d)", hu_synth_verdict_str(v), name ? name : "?",
                         resp.status_code, exp_status);
            if (cfg->regression_dir) {
                char *ij = NULL;
                size_t il = 0;
                (void)hu_json_stringify(alloc, item, &ij, &il);
                char reason[256];
                snprintf(reason, sizeof(reason), "status=%ld expected=%d", resp.status_code,
                         exp_status);
                hu_synth_test_case_t tc = {.name = (char *)(name ? name : "gw_test"),
                                           .category = (char *)"gateway",
                                           .input_json = ij,
                                           .verdict = v,
                                           .verdict_reason = reason,
                                           .latency_ms = lat};
                hu_synth_regression_save(alloc, cfg->regression_dir, &tc);
                if (ij)
                    alloc->free(alloc->ctx, ij, il);
            }
        }
        hu_http_response_free(alloc, &resp);
    }
    hu_json_free(alloc, root);
    return HU_OK;
}
