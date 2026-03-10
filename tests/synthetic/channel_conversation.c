#include "channel_harness.h"
#include "human/core/json.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char CONV_PROMPT[] =
    "You are a test case generator for messaging channel integration testing.\n"
    "Generate %d conversation scenarios for the channel '%s'.\n\n"
    "Return a JSON array where each element has:\n"
    "- \"session_key\": a realistic sender ID for this channel type (phone number, username, "
    "email)\n"
    "- \"turns\": array of objects with:\n"
    "  - \"user\": the message the user sends (realistic, varied)\n"
    "  - \"expect\": a pipe-separated regex pattern the response should match\n\n"
    "Scenarios should include:\n"
    "- Simple greetings and questions\n"
    "- Multi-turn follow-ups that reference previous context\n"
    "- Tool-triggering requests (weather, calculations)\n"
    "- Edge cases (very short messages, questions, emoji-only)\n\n"
    "Each scenario should have 2-4 turns.\n"
    "Return ONLY a JSON array, no markdown.";

static bool match_pattern(const char *text, const char *pattern) {
    if (!text || !pattern || !pattern[0])
        return true;
    /* Simple pipe-separated substring match */
    char buf[512];
    size_t plen = strlen(pattern);
    if (plen >= sizeof(buf))
        plen = sizeof(buf) - 1;
    memcpy(buf, pattern, plen);
    buf[plen] = '\0';
    char *tok = strtok(buf, "|");
    while (tok) {
        /* Case-insensitive substring search */
        const char *t = text;
        size_t tlen = strlen(tok);
        while (*t) {
            if (strncasecmp(t, tok, tlen) == 0)
                return true;
            t++;
        }
        tok = strtok(NULL, "|");
    }
    return false;
}

static hu_error_t run_scenario(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                               const hu_channel_test_entry_t *entry, hu_json_value_t *scenario,
                               hu_synth_metrics_t *metrics) {
    const char *session_key = hu_json_get_string(scenario, "session_key");
    if (!session_key)
        session_key = "test_user";
    size_t sk_len = strlen(session_key);

    hu_json_value_t *turns = hu_json_object_get(scenario, "turns");
    if (!turns || turns->type != HU_JSON_ARRAY || turns->data.array.len == 0)
        return HU_OK;

    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        HU_CH_LOG("failed to create channel %s: %s", entry->name, hu_error_string(err));
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return err;
    }

    for (size_t i = 0; i < turns->data.array.len; i++) {
        hu_json_value_t *turn = turns->data.array.items[i];
        const char *user_msg = hu_json_get_string(turn, "user");
        const char *expect = hu_json_get_string(turn, "expect");
        if (!user_msg)
            continue;

        double t0 = hu_synth_now_ms();

        /* Inject user message */
        err = entry->inject(&ch, session_key, sk_len, user_msg, strlen(user_msg));
        if (err != HU_OK) {
            hu_synth_metrics_record(alloc, metrics, hu_synth_now_ms() - t0, HU_SYNTH_ERROR);
            HU_CH_LOG("inject failed on %s: %s", entry->name, hu_error_string(err));
            continue;
        }

        /* Poll to verify injection */
        if (entry->poll) {
            hu_channel_loop_msg_t msgs[16];
            size_t count = 0;
            err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
            if (err != HU_OK || count == 0) {
                hu_synth_metrics_record(alloc, metrics, hu_synth_now_ms() - t0, HU_SYNTH_FAIL);
                HU_CH_LOG("poll failed on %s turn %zu: err=%s count=%zu", entry->name, i,
                          hu_error_string(err), count);
                if (cfg->regression_dir) {
                    char reason[256];
                    snprintf(reason, sizeof(reason), "poll returned count=%zu", count);
                    hu_synth_test_case_t tc = {
                        .name = (char *)entry->name,
                        .category = (char *)"conversation",
                        .input_json = (char *)user_msg,
                        .verdict = HU_SYNTH_FAIL,
                        .verdict_reason = reason,
                    };
                    hu_synth_regression_save(alloc, cfg->regression_dir, &tc);
                }
                continue;
            }
        }

        /* Simulate agent response via channel send */
        const char *response = "This is a test response from the agent.";
        size_t resp_len = strlen(response);
        if (ch.vtable && ch.vtable->send) {
            err = ch.vtable->send(ch.ctx, session_key, sk_len, response, resp_len, NULL, 0);
        }

        /* Capture what was sent and verify pattern */
        if (entry->get_last) {
            size_t last_len = 0;
            const char *last = entry->get_last(&ch, &last_len);
            if (last && last_len > 0) {
                double lat = hu_synth_now_ms() - t0;
                hu_synth_verdict_t v =
                    match_pattern(last, expect ? expect : "") ? HU_SYNTH_PASS : HU_SYNTH_FAIL;
                if (v == HU_SYNTH_FAIL && cfg->regression_dir) {
                    char reason[256];
                    snprintf(reason, sizeof(reason), "response did not match expect pattern");
                    hu_synth_test_case_t tc = {
                        .name = (char *)entry->name,
                        .category = (char *)"conversation",
                        .input_json = (char *)user_msg,
                        .verdict = HU_SYNTH_FAIL,
                        .verdict_reason = reason,
                    };
                    hu_synth_regression_save(alloc, cfg->regression_dir, &tc);
                }
                HU_CH_VERBOSE(cfg, "%s %s turn %zu: injected '%.*s', sent '%.*s' (%.1fms)",
                              v == HU_SYNTH_PASS ? "PASS" : "FAIL", entry->name, i,
                              (int)(strlen(user_msg) > 40 ? 40 : strlen(user_msg)), user_msg,
                              (int)(last_len > 40 ? 40 : last_len), last, lat);
                hu_synth_metrics_record(alloc, metrics, lat, v);
            } else {
                hu_synth_metrics_record(alloc, metrics, hu_synth_now_ms() - t0, HU_SYNTH_FAIL);
                HU_CH_LOG("get_last returned NULL on %s turn %zu", entry->name, i);
            }
        } else {
            hu_synth_metrics_record(alloc, metrics, hu_synth_now_ms() - t0, HU_SYNTH_PASS);
        }
    }

    entry->destroy(&ch);
    return HU_OK;
}

hu_error_t hu_channel_run_conversations(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini,
                                        hu_synth_metrics_t *metrics) {
    hu_synth_metrics_init(metrics);
    size_t reg_count = 0;
    const hu_channel_test_entry_t *reg = hu_channel_test_registry(&reg_count);

    for (size_t ci = 0; ci < reg_count; ci++) {
        const hu_channel_test_entry_t *entry = &reg[ci];

        /* Filter by channel list */
        if (!cfg->all_channels) {
            bool found = false;
            for (size_t j = 0; j < cfg->channel_count; j++) {
                if (strcmp(cfg->channels[j], entry->name) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
        }

        HU_CH_LOG("testing channel: %s", entry->name);

        /* Generate scenarios via Gemini */
        int count = cfg->tests_per_channel > 0 ? cfg->tests_per_channel : 3;
        char prompt[4096];
        snprintf(prompt, sizeof(prompt), CONV_PROMPT, count, entry->name);

        char *response = NULL;
        size_t response_len = 0;
        hu_error_t err = hu_synth_gemini_generate(alloc, gemini, prompt, strlen(prompt), &response,
                                                  &response_len);
        if (err != HU_OK) {
            HU_CH_LOG("Gemini generation failed for %s: %s", entry->name, hu_error_string(err));
            hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
            continue;
        }

        hu_json_value_t *root = NULL;
        err = hu_json_parse(alloc, response, response_len, &root);
        hu_synth_strfree(alloc, response, response_len);
        if (err != HU_OK || !root || root->type != HU_JSON_ARRAY) {
            HU_CH_LOG("failed to parse Gemini response for %s", entry->name);
            if (root)
                hu_json_free(alloc, root);
            hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
            continue;
        }

        HU_CH_LOG("  generated %zu scenarios for %s", root->data.array.len, entry->name);

        for (size_t si = 0; si < root->data.array.len; si++) {
            run_scenario(alloc, cfg, entry, root->data.array.items[si], metrics);
        }

        hu_json_free(alloc, root);
    }

    return HU_OK;
}
