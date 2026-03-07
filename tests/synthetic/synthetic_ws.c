#include "seaclaw/websocket/websocket.h"
#include "synthetic_harness.h"

static const char SC_SYNTH_WS_PROMPT[] =
    "You are a test case generator for the seaclaw WebSocket control protocol.\n"
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

sc_error_t sc_synth_run_ws(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                           sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics) {
    SC_SYNTH_LOG("=== WebSocket Tests ===");
    sc_synth_metrics_init(metrics);
    int count = cfg->tests_per_category > 0 ? cfg->tests_per_category : 15;
    char prompt[4096];
    snprintf(prompt, sizeof(prompt), SC_SYNTH_WS_PROMPT, count);
    SC_SYNTH_LOG("generating %d WS test cases via Gemini...", count);
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
    SC_SYNTH_LOG("executing %zu WS tests...", n);
    char ws_url[128];
    snprintf(ws_url, sizeof(ws_url), "ws://127.0.0.1:%u/ws", cfg->gateway_port);
    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        const char *name = sc_json_get_string(item, "name");
        sc_json_value_t *messages = sc_json_object_get(item, "messages");
        sc_json_value_t *exp_ok = sc_json_object_get(item, "expected_ok");
        if (!messages || messages->type != SC_JSON_ARRAY)
            continue;
        sc_ws_client_t *ws = NULL;
        double t0 = sc_synth_now_ms();
        err = sc_ws_connect(alloc, ws_url, &ws);
        if (err != SC_OK) {
            sc_synth_metrics_record(alloc, metrics, sc_synth_now_ms() - t0, SC_SYNTH_ERROR);
            SC_SYNTH_LOG("ERROR %s: ws connect failed", name ? name : "?");
            continue;
        }
        sc_synth_verdict_t v = SC_SYNTH_PASS;
        for (size_t j = 0; j < messages->data.array.len; j++) {
            sc_json_value_t *msg = messages->data.array.items[j];
            char *ms = NULL;
            size_t ml = 0;
            (void)sc_json_stringify(alloc, msg, &ms, &ml);
            if (!ms)
                continue;
            err = sc_ws_send(ws, ms, ml);
            alloc->free(alloc->ctx, ms, ml);
            if (err != SC_OK) {
                v = SC_SYNTH_ERROR;
                break;
            }
            char *rd = NULL;
            size_t rl = 0;
            err = sc_ws_recv(ws, alloc, &rd, &rl);
            if (err != SC_OK) {
                v = SC_SYNTH_ERROR;
                break;
            }
            if (exp_ok && exp_ok->type == SC_JSON_ARRAY && j < exp_ok->data.array.len) {
                sc_json_value_t *ej = exp_ok->data.array.items[j];
                bool expected = (ej->type == SC_JSON_BOOL) ? ej->data.boolean : true;
                sc_json_value_t *rj = NULL;
                if (sc_json_parse(alloc, rd, rl, &rj) == SC_OK) {
                    bool actual = sc_json_get_bool(rj, "ok", false);
                    if (actual != expected)
                        v = SC_SYNTH_FAIL;
                    sc_json_free(alloc, rj);
                }
            }
            alloc->free(alloc->ctx, rd, rl);
            if (v != SC_SYNTH_PASS)
                break;
        }
        double lat = sc_synth_now_ms() - t0;
        sc_synth_metrics_record(alloc, metrics, lat, v);
        if (v == SC_SYNTH_PASS) {
            SC_SYNTH_VERBOSE(cfg, "PASS  %s (%.1fms)", name ? name : "?", lat);
        } else {
            SC_SYNTH_LOG("%-5s %s", sc_synth_verdict_str(v), name ? name : "?");
            if (cfg->regression_dir) {
                char *ij = NULL;
                size_t il = 0;
                (void)sc_json_stringify(alloc, item, &ij, &il);
                sc_synth_test_case_t tc = {.name = (char *)(name ? name : "ws_test"),
                                           .category = (char *)"websocket",
                                           .input_json = ij,
                                           .verdict = v,
                                           .latency_ms = lat};
                sc_synth_regression_save(alloc, cfg->regression_dir, &tc);
                if (ij)
                    alloc->free(alloc->ctx, ij, il);
            }
        }
        sc_ws_close(ws, alloc);
    }
    sc_synth_report_category("WebSocket", metrics);
    sc_json_free(alloc, root);
    return SC_OK;
}
