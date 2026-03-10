#include "channel_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__MACH__)

static bool messages_app_running(void) {
    return system("pgrep -q Messages") == 0;
}

static hu_error_t send_imessage_osascript(const char *target, const char *message) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "osascript -e '"
             "tell application \"Messages\"\n"
             "  set targetBuddy to buddy \"%s\" of (service 1 whose service type is iMessage)\n"
             "  send \"%s\" to targetBuddy\n"
             "end tell' 2>/dev/null",
             target, message);
    int ret = system(cmd);
    return ret == 0 ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_channel_run_real_imessage(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini,
                                        hu_synth_metrics_t *metrics) {
    hu_synth_metrics_init(metrics);

    if (!cfg->real_imessage_target) {
        HU_CH_LOG("no --real-imessage target specified, skipping");
        return HU_OK;
    }

    HU_CH_LOG("Real iMessage test to: %s", cfg->real_imessage_target);

    if (!messages_app_running()) {
        HU_CH_LOG("WARNING: Messages.app not running, test may fail");
    }

    /* Generate a conversation starter via Gemini */
    const char *prompt =
        "Generate a short, friendly test message (1 sentence) that could be sent to someone "
        "to verify an iMessage integration is working. Include a unique identifier like a number "
        "so we can verify the response references it. Return ONLY the message text, no JSON.";
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err =
        hu_synth_gemini_generate(alloc, gemini, prompt, strlen(prompt), &response, &response_len);
    if (err != HU_OK) {
        HU_CH_LOG("Gemini failed to generate message: %s", hu_error_string(err));
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return err;
    }

    /* Trim whitespace/quotes from response */
    char *msg = response;
    while (*msg == '"' || *msg == ' ' || *msg == '\n')
        msg++;
    size_t mlen = strlen(msg);
    while (mlen > 0 && (msg[mlen - 1] == '"' || msg[mlen - 1] == '\n' || msg[mlen - 1] == ' '))
        mlen--;
    msg[mlen] = '\0';

    HU_CH_LOG("sending: \"%s\"", msg);

    double t0 = hu_synth_now_ms();
    err = send_imessage_osascript(cfg->real_imessage_target, msg);
    double send_lat = hu_synth_now_ms() - t0;

    if (err != HU_OK) {
        HU_CH_LOG("FAIL: osascript send failed (%.1fms)", send_lat);
        hu_synth_metrics_record(alloc, metrics, send_lat, HU_SYNTH_FAIL);
        if (cfg->regression_dir) {
            hu_synth_test_case_t tc = {.name = (char *)"real_imessage_send",
                                       .category = (char *)"real_imessage",
                                       .input_json = msg,
                                       .verdict = HU_SYNTH_FAIL,
                                       .verdict_reason = (char *)"osascript failed"};
            hu_synth_regression_save(alloc, cfg->regression_dir, &tc);
        }
    } else {
        HU_CH_LOG("PASS: message sent successfully (%.1fms)", send_lat);
        hu_synth_metrics_record(alloc, metrics, send_lat, HU_SYNTH_PASS);
    }

    hu_synth_strfree(alloc, response, response_len);
    return HU_OK;
}

#else /* Not macOS */

hu_error_t hu_channel_run_real_imessage(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini,
                                        hu_synth_metrics_t *metrics) {
    (void)alloc;
    (void)cfg;
    (void)gemini;
    hu_synth_metrics_init(metrics);
    HU_CH_LOG("real iMessage not supported on this platform");
    return HU_ERR_NOT_SUPPORTED;
}

#endif
