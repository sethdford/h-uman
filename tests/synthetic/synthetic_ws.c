#include "human/websocket/websocket.h"
#include "synthetic_harness.h"

static const char HU_SYNTH_WS_PROMPT[] =
    "You are a test case generator for the human WebSocket control protocol.\n"
    "Generate %d diverse test cases for JSON-RPC over WebSocket.\n\n"
    "Return a JSON array where each element has:\n"
    "- \"name\": test name\n"
    "- \"messages\": array of JSON-RPC requests to send\n"
    "- \"expected_ok\": array of booleans (expected ok field per response)\n\n"
    "Request format: {\"type\":\"req\",\"id\":\"N\",\"method\":\"...\",\"params\":{}}\n"
    "Public methods: health, capabilities, connect\n"
    "Auth: auth.token with {\"token\":\"test\"}\n"
    "Authenticated: config.get, tools.catalog, models.list, sessions.list\n\n"
    "Scenarios: public calls, auth flow, unauth access (ok:false), unknown method.\n"
    "Mix: 40%% public, 30%% auth, 30%% error.\nReturn ONLY a JSON array.";

hu_error_t hu_synth_run_ws(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                           hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics) {
    HU_SYNTH_LOG("=== WebSocket Tests ===");
    hu_synth_metrics_init(metrics);
    int count = cfg->tests_per_category > 0 ? cfg->tests_per_category : 15;
    char prompt[4096];
    snprintf(prompt, sizeof(prompt), HU_SYNTH_WS_PROMPT, count);
    HU_SYNTH_LOG("generating %d WS test cases via Gemini...", count);
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
    HU_SYNTH_LOG("executing %zu WS tests...", n);
    char ws_url[128];
    snprintf(ws_url, sizeof(ws_url), "ws://127.0.0.1:%u/ws", cfg->gateway_port);
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        const char *name = hu_json_get_string(item, "name");
        hu_json_value_t *messages = hu_json_object_get(item, "messages");
        hu_json_value_t *exp_ok = hu_json_object_get(item, "expected_ok");
        if (!messages || messages->type != HU_JSON_ARRAY)
            continue;
        hu_ws_client_t *ws = NULL;
        double t0 = hu_synth_now_ms();
        err = hu_ws_connect(alloc, ws_url, &ws);
        if (err != HU_OK) {
            hu_synth_metrics_record(alloc, metrics, hu_synth_now_ms() - t0, HU_SYNTH_ERROR);
            HU_SYNTH_LOG("ERROR %s: ws connect failed", name ? name : "?");
            continue;
        }
        hu_synth_verdict_t v = HU_SYNTH_PASS;
        for (size_t j = 0; j < messages->data.array.len; j++) {
            hu_json_value_t *msg = messages->data.array.items[j];
            char *ms = NULL;
            size_t ml = 0;
            (void)hu_json_stringify(alloc, msg, &ms, &ml);
            if (!ms)
                continue;
            err = hu_ws_send(ws, ms, ml);
            alloc->free(alloc->ctx, ms, ml);
            if (err != HU_OK) {
                v = HU_SYNTH_ERROR;
                break;
            }
            char *rd = NULL;
            size_t rl = 0;
            err = hu_ws_recv(ws, alloc, &rd, &rl);
            if (err != HU_OK) {
                v = HU_SYNTH_ERROR;
                break;
            }
            if (exp_ok && exp_ok->type == HU_JSON_ARRAY && j < exp_ok->data.array.len) {
                hu_json_value_t *ej = exp_ok->data.array.items[j];
                bool expected = (ej->type == HU_JSON_BOOL) ? ej->data.boolean : true;
                hu_json_value_t *rj = NULL;
                if (hu_json_parse(alloc, rd, rl, &rj) == HU_OK) {
                    bool actual = hu_json_get_bool(rj, "ok", false);
                    if (actual != expected)
                        v = HU_SYNTH_FAIL;
                    hu_json_free(alloc, rj);
                }
            }
            alloc->free(alloc->ctx, rd, rl);
            if (v != HU_SYNTH_PASS)
                break;
        }
        double lat = hu_synth_now_ms() - t0;
        hu_synth_metrics_record(alloc, metrics, lat, v);
        if (v == HU_SYNTH_PASS) {
            HU_SYNTH_VERBOSE(cfg, "PASS  %s (%.1fms)", name ? name : "?", lat);
        } else {
            HU_SYNTH_LOG("%-5s %s", hu_synth_verdict_str(v), name ? name : "?");
            if (cfg->regression_dir) {
                char *ij = NULL;
                size_t il = 0;
                (void)hu_json_stringify(alloc, item, &ij, &il);
                hu_synth_test_case_t tc = {.name = (char *)(name ? name : "ws_test"),
                                           .category = (char *)"websocket",
                                           .input_json = ij,
                                           .verdict = v,
                                           .latency_ms = lat};
                hu_synth_regression_save(alloc, cfg->regression_dir, &tc);
                if (ij)
                    alloc->free(alloc->ctx, ij, il);
            }
        }
        hu_ws_close(ws, alloc);
    }
    hu_json_free(alloc, root);
    return HU_OK;
}
