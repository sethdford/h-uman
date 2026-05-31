/* src/cli_ctl.c
 *
 * `human ctl guard` — runtime kill-switch inspector + edit-advice tool.
 *
 * Scope (deliberately narrow): READ-ONLY surface. Prints current
 * response_guard config state from ~/.human/config.json + a copy-
 * paste-ready JSON snippet operators can use to flip the relevant
 * knobs. The CLI does NOT mutate config — that's operator action
 * with vim/jq, after which they restart the daemon.
 *
 * Why read-only:
 *   - JSON mutation in C is complex (load, modify, serialize
 *     preserving format + comments). High chance of corrupting an
 *     operator's hand-curated config.
 *   - Operators already have vim/jq for config edits.
 *   - The VALUE the CLI adds is "tell me current state + remind me
 *     what to edit + advise restart." That's enough to close the
 *     ergonomic gap without taking on mutation risk.
 *   - Real runtime kill-switch (poke the daemon's atomic from a
 *     separate CLI process) requires IPC infrastructure that
 *     doesn't exist yet. Out of scope for this commit.
 *
 * Closes the "runtime CLI for kill switches" item from
 * docs/plans/2026-05-26-m3-dispatch-unification/STATUS.md as a
 * read-only tool. The IPC-driven mutation version is a future sprint.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_ctl_usage =
    "Usage: human ctl <subcommand>\n"
    "  guard status            Show response_guard runtime kill-switch state\n"
    "  guard disable-g9        Print the config edit to disable G9 globally\n"
    "  guard enable-g9         Print the config edit to enable G9 globally\n"
    "  guard list-channels     List channels currently exempt from G9\n"
    "\n"
    "Note: this CLI is read-only. To change config, edit ~/.human/config.json\n"
    "directly and restart the daemon. The CLI prints the exact JSON snippet\n"
    "to paste.\n";

static void print_guard_status(const hu_config_t *cfg) {
    const hu_response_guard_config_t *rg = &cfg->response_guard;
    printf("response_guard kill-switch state:\n");
    printf("  G9 (naked discourse opener):  %s\n",
           rg->naked_opener_enabled ? "ENABLED" : "DISABLED (globally)");
    printf("  G9 disabled channels:         ");
    if (rg->g9_disabled_channels_count == 0) {
        printf("(none — G9 fires on all channels)\n");
    } else {
        printf("[");
        for (size_t i = 0; i < rg->g9_disabled_channels_count; i++) {
            printf("%s%s", i > 0 ? ", " : "",
                   rg->g9_disabled_channels[i] ? rg->g9_disabled_channels[i] : "(null)");
        }
        printf("]\n");
    }
    printf("\n");
    printf("Config source: ~/.human/config.json (response_guard section).\n");
    printf("To change: edit the file directly, then restart the daemon.\n");
}

static void print_disable_g9_snippet(void) {
    printf("To disable G9 (naked discourse opener) globally, add to ~/.human/config.json:\n");
    printf("\n");
    printf("  {\n");
    printf("    \"response_guard\": {\n");
    printf("      \"naked_opener_enabled\": false\n");
    printf("    }\n");
    printf("  }\n");
    printf("\n");
    printf("Then restart the daemon. The daemon's startup applies this to the\n");
    printf("process-wide atomic via hu_response_guard_set_naked_opener_globally_disabled().\n");
    printf("\n");
    printf("REMINDER: G9 catches the Jordan-class \"tbh morning\" failure mode.\n");
    printf("Disabling it globally is a 3am-emergency lever, not a normal-operations\n");
    printf("setting. Consider 'guard list-channels' for per-channel disable instead.\n");
}

static void print_enable_g9_snippet(void) {
    printf("To enable G9 (naked discourse opener) globally, set:\n");
    printf("\n");
    printf("  {\n");
    printf("    \"response_guard\": {\n");
    printf("      \"naked_opener_enabled\": true\n");
    printf("    }\n");
    printf("  }\n");
    printf("\n");
    printf("Then restart the daemon. true is the default, so omitting the field\n");
    printf("entirely also re-enables.\n");
}

static void print_channel_list(const hu_config_t *cfg) {
    const hu_response_guard_config_t *rg = &cfg->response_guard;
    if (rg->g9_disabled_channels_count == 0) {
        printf("No channels currently disabled. G9 fires on all channels.\n");
        printf("\n");
        printf("To add a channel exemption, add to ~/.human/config.json:\n");
        printf("\n");
        printf("  {\n");
        printf("    \"response_guard\": {\n");
        printf("      \"g9_disabled_channels\": [\"voice\"]\n");
        printf("    }\n");
        printf("  }\n");
        printf("\n");
        printf("Then restart the daemon.\n");
    } else {
        printf("Channels exempt from G9 (count=%zu):\n", rg->g9_disabled_channels_count);
        for (size_t i = 0; i < rg->g9_disabled_channels_count; i++) {
            printf("  - %s\n",
                   rg->g9_disabled_channels[i] ? rg->g9_disabled_channels[i] : "(null)");
        }
        printf("\n");
        printf("To add or remove channels, edit response_guard.g9_disabled_channels[]\n");
        printf("in ~/.human/config.json and restart the daemon.\n");
    }
}

hu_error_t cmd_ctl(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("%s", s_ctl_usage);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *sub = argv[2];
    if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0 || strcmp(sub, "help") == 0) {
        printf("%s", s_ctl_usage);
        return HU_OK;
    }

    if (strcmp(sub, "guard") != 0) {
        fprintf(stderr, "unknown ctl subcommand: %s\n\n", sub);
        printf("%s", s_ctl_usage);
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (argc < 4) {
        printf("%s", s_ctl_usage);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *guard_sub = argv[3];

    /* enable-g9 / disable-g9 don't need to load config — they print
     * static snippets. status / list-channels do. Branch first to
     * skip the config load when we don't need it. */
    if (strcmp(guard_sub, "disable-g9") == 0) {
        print_disable_g9_snippet();
        return HU_OK;
    }
    if (strcmp(guard_sub, "enable-g9") == 0) {
        print_enable_g9_snippet();
        return HU_OK;
    }

    if (strcmp(guard_sub, "status") != 0 && strcmp(guard_sub, "list-channels") != 0) {
        fprintf(stderr, "unknown guard subcommand: %s\n\n", guard_sub);
        printf("%s", s_ctl_usage);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Load config to inspect current state. hu_config_load handles
     * the ~/.human/config.json default location + env override. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        fprintf(stderr, "failed to load config: %d\n", (int)err);
        return err;
    }

    if (strcmp(guard_sub, "status") == 0) {
        print_guard_status(&cfg);
    } else /* list-channels */ {
        print_channel_list(&cfg);
    }

    hu_config_deinit(&cfg);
    return HU_OK;
}
