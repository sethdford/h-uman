#include "channel_harness.h"
#include "human/core/http.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n"
           "Channel conversation & chaos tests for human.\n\n"
           "  --binary PATH          Path to human binary (default: ./human)\n"
           "  --model MODEL          Gemini model (default: gemini-2.5-flash)\n"
           "  --port PORT            Gateway port (default: 3198)\n"
           "  --channels LIST        Comma-separated channels or 'all' (default: all)\n"
           "  --count N              Tests per channel (default: 5)\n"
           "  --chaos MODE           none|message|infra|all (default: none)\n"
           "  --concurrency N        Pressure workers (default: 4)\n"
           "  --duration SECS        Pressure duration (default: 10)\n"
           "  --real-imessage TARGET Real iMessage target (phone/email)\n"
           "  --regression-dir DIR   Save failures for replay\n"
           "  --verbose              Verbose output\n"
           "  --help                 Show help\n"
           "\nEnv: GEMINI_API_KEY (required)\n",
           prog);
}

/* Parse comma-separated channel list */
static void parse_channels(const char *list, const char ***out, size_t *count) {
    size_t n = 1;
    for (const char *p = list; *p; p++)
        if (*p == ',')
            n++;
    const char **arr = (const char **)malloc(n * sizeof(const char *));
    if (!arr) {
        *out = NULL;
        *count = 0;
        return;
    }
    char *dup = strdup(list);
    if (!dup) {
        free(arr);
        *out = NULL;
        *count = 0;
        return;
    }
    size_t i = 0;
    char *tok = strtok(dup, ",");
    while (tok && i < n) {
        arr[i++] = tok;
        tok = strtok(NULL, ",");
    }
    *out = arr;
    *count = i;
    /* NOTE: arr and dup leak intentionally — program lifetime */
}

static hu_chaos_mode_t parse_chaos(const char *s) {
    if (!s || !strcmp(s, "none"))
        return HU_CHAOS_NONE;
    if (!strcmp(s, "message"))
        return HU_CHAOS_MESSAGE;
    if (!strcmp(s, "infra"))
        return HU_CHAOS_INFRA;
    if (!strcmp(s, "all"))
        return HU_CHAOS_ALL;
    return HU_CHAOS_NONE;
}

int main(int argc, char **argv) {
    hu_channel_test_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.binary_path = "./human";
    cfg.gemini_model = "gemini-2.5-flash";
    cfg.gateway_port = 3198;
    cfg.tests_per_channel = 5;
    cfg.concurrency = 4;
    cfg.duration_secs = 10;
    cfg.all_channels = true;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--binary") && i + 1 < argc)
            cfg.binary_path = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            cfg.gemini_model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            cfg.gateway_port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) {
            const char *list = argv[++i];
            if (strcmp(list, "all") == 0)
                cfg.all_channels = true;
            else {
                cfg.all_channels = false;
                parse_channels(list, &cfg.channels, &cfg.channel_count);
            }
        } else if (!strcmp(argv[i], "--count") && i + 1 < argc)
            cfg.tests_per_channel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chaos") && i + 1 < argc)
            cfg.chaos = parse_chaos(argv[++i]);
        else if (!strcmp(argv[i], "--concurrency") && i + 1 < argc)
            cfg.concurrency = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            cfg.duration_secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--real-imessage") && i + 1 < argc)
            cfg.real_imessage_target = argv[++i];
        else if (!strcmp(argv[i], "--regression-dir") && i + 1 < argc)
            cfg.regression_dir = argv[++i];
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
            cfg.verbose = true;
        else {
            fprintf(stderr, "Unknown: %s\n", argv[i]);
            return 1;
        }
    }

    cfg.gemini_api_key = getenv("GEMINI_API_KEY");
    if (!cfg.gemini_api_key) {
        fprintf(stderr, "error: GEMINI_API_KEY not set\n");
        return 1;
    }

    hu_allocator_t alloc = hu_system_allocator();
    HU_CH_LOG("=== human Channel Conversation & Chaos Tests ===");
    HU_CH_LOG("binary: %s  model: %s  port: %u  chaos: %d", cfg.binary_path, cfg.gemini_model,
              cfg.gateway_port, cfg.chaos);

    size_t reg_count = 0;
    const hu_channel_test_entry_t *reg = hu_channel_test_registry(&reg_count);
    HU_CH_LOG("registered channels: %zu", reg_count);

    hu_synth_gemini_ctx_t *gemini = NULL;
    hu_error_t err = hu_synth_gemini_init(&alloc, cfg.gemini_api_key, cfg.gemini_model, &gemini);
    if (err != HU_OK) {
        fprintf(stderr, "Gemini init failed: %s\n", hu_error_string(err));
        return 1;
    }

    hu_synth_metrics_t conv_m = {0}, chaos_m = {0}, pressure_m = {0}, real_m = {0};

    /* Conversations */
    HU_CH_LOG("--- Conversation Tests ---");
    hu_channel_run_conversations(&alloc, &cfg, gemini, &conv_m);

    /* Chaos */
    if (cfg.chaos != HU_CHAOS_NONE) {
        HU_CH_LOG("--- Chaos Tests ---");
        hu_channel_run_chaos(&alloc, &cfg, gemini, &chaos_m);
    }

    /* Pressure */
    if (cfg.concurrency > 0 && cfg.duration_secs > 0) {
        HU_CH_LOG("--- Pressure Tests ---");
        hu_channel_run_pressure(&alloc, &cfg, gemini, &pressure_m);
    }

    /* Real iMessage */
    if (cfg.real_imessage_target) {
        HU_CH_LOG("--- Real iMessage ---");
        hu_channel_run_real_imessage(&alloc, &cfg, gemini, &real_m);
    }

    /* Report */
    HU_CH_LOG("========== Final Report ==========");
    if (conv_m.total)
        hu_synth_report_category("Conversations", &conv_m);
    if (chaos_m.total)
        hu_synth_report_category("Chaos", &chaos_m);
    if (pressure_m.total)
        hu_synth_report_category("Pressure", &pressure_m);
    if (real_m.total)
        hu_synth_report_category("Real-iMsg", &real_m);

    int tf = conv_m.failed + chaos_m.failed + pressure_m.failed + real_m.failed;
    int te = conv_m.errors + chaos_m.errors + pressure_m.errors + real_m.errors;
    int tp = conv_m.passed + chaos_m.passed + pressure_m.passed + real_m.passed;
    int ta = conv_m.total + chaos_m.total + pressure_m.total + real_m.total;
    HU_CH_LOG("Total: %d/%d passed, %d failed, %d errors", tp, ta, tf, te);

    hu_synth_metrics_free(&alloc, &conv_m);
    hu_synth_metrics_free(&alloc, &chaos_m);
    hu_synth_metrics_free(&alloc, &pressure_m);
    hu_synth_metrics_free(&alloc, &real_m);
    hu_synth_gemini_deinit(&alloc, gemini);
    return (tf > 0 || te > 0) ? 1 : 0;
}
