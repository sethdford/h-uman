#include "seaclaw/core/http.h"
#include "synthetic_harness.h"

static const char SC_SYNTH_GW_PROMPT[] =
    "You are a test case generator for the seaclaw HTTP gateway.\n"
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

sc_error_t sc_synth_run_gateway(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                                sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics) {
    SC_SYNTH_LOG("=== Gateway HTTP Tests ===");
    sc_synth_metrics_init(metrics);
    int count = cfg->tests_per_category > 0 ? cfg->tests_per_category : 20;
    char prompt[4096];
    snprintf(prompt, sizeof(prompt), SC_SYNTH_GW_PROMPT, count);
    SC_SYNTH_LOG("generating %d gateway test cases via Gemini...", count);
    char *response = NULL;
    size_t response_len = 0;
    sc_error_t err =
        sc_synth_gemini_generate(alloc, gemini, prompt, strlen(prompt), &response, &response_len);
    if (err != SC_OK) {
        SC_SYNTH_LOG("Gemini generation failed: %s", sc_error_string(err));
        return err;
    }
    sc_json_value_t *root = NULL;
    err = sc_json_parse(alloc, response, response_len, &root);
    sc_synth_strfree(alloc, response, response_len);
    if (err != SC_OK || !root || root->type != SC_JSON_ARRAY) {
        SC_SYNTH_LOG("failed to parse test cases");
        if (root)
            sc_json_free(alloc, root);
        return SC_ERR_PARSE;
    }
    size_t n = root->data.array.len;
    SC_SYNTH_LOG("executing %zu gateway tests...", n);
    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        const char *name = sc_json_get_string(item, "name");
        const char *method = sc_json_get_string(item, "method");
        const char *path = sc_json_get_string(item, "path");
        const char *body_str = sc_json_get_string(item, "body");
        int exp_status = (int)sc_json_get_number(item, "expected_status", 200.0);
        if (!method || !path)
            continue;
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u%s", cfg->gateway_port, path);
        sc_http_response_t resp = {0};
        double t0 = sc_synth_now_ms();
        if (strcmp(method, "GET") == 0) {
            err = sc_http_get(alloc, url, NULL, &resp);
        } else {
            size_t bl = body_str ? strlen(body_str) : 0;
            err = sc_http_post_json(alloc, url, NULL, body_str ? body_str : "", bl, &resp);
        }
        double lat = sc_synth_now_ms() - t0;
        sc_synth_verdict_t v = SC_SYNTH_PASS;
        if (err != SC_OK) {
            v = SC_SYNTH_ERROR;
        } else if (resp.status_code != exp_status) {
            v = SC_SYNTH_FAIL;
        } else {
            sc_json_value_t *contains = sc_json_object_get(item, "expected_body_contains");
            if (contains && contains->type == SC_JSON_ARRAY && resp.body) {
                for (size_t k = 0; k < contains->data.array.len; k++) {
                    sc_json_value_t *pat = contains->data.array.items[k];
                    if (pat->type == SC_JSON_STRING && !strstr(resp.body, pat->data.string.ptr)) {
                        v = SC_SYNTH_FAIL;
                        break;
                    }
                }
            }
        }
        sc_synth_metrics_record(alloc, metrics, lat, v);
        if (v == SC_SYNTH_PASS) {
            SC_SYNTH_VERBOSE(cfg, "PASS  %s (%.1fms)", name ? name : "?", lat);
        } else {
            SC_SYNTH_LOG("%-5s %s (status=%ld exp=%d)", sc_synth_verdict_str(v), name ? name : "?",
                         resp.status_code, exp_status);
            if (cfg->regression_dir) {
                char *ij = NULL;
                size_t il = 0;
                (void)sc_json_stringify(alloc, item, &ij, &il);
                char reason[256];
                snprintf(reason, sizeof(reason), "status=%ld expected=%d", resp.status_code,
                         exp_status);
                sc_synth_test_case_t tc = {.name = (char *)(name ? name : "gw_test"),
                                           .category = (char *)"gateway",
                                           .input_json = ij,
                                           .verdict = v,
                                           .verdict_reason = reason,
                                           .latency_ms = lat};
                sc_synth_regression_save(alloc, cfg->regression_dir, &tc);
                if (ij)
                    alloc->free(alloc->ctx, ij, il);
            }
        }
        sc_http_response_free(alloc, &resp);
    }
    sc_synth_report_category("Gateway", metrics);
    sc_json_free(alloc, root);
    return SC_OK;
}
