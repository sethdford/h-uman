#include "human/eval/cli_eval.h"

#include <stdio.h>
#include <string.h>

static hu_error_t print_subcommand_help(const char *name) {
    printf("human eval %s — Phase 5 RL SOTA eval tooling\n\n", name);
    printf("Options:\n  --help    Show this help\n\n");
    printf("Related subcommands: competitive, leaderboard, gate\n");
    return HU_OK;
}

hu_error_t hu_eval_cli_competitive(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return print_subcommand_help("competitive");
    }
    printf("human eval competitive: run side-by-side scorecard (see competitive_harness)\n");
    return HU_OK;
}

hu_error_t hu_eval_cli_leaderboard(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return print_subcommand_help("leaderboard");
    }
    printf("human eval leaderboard: MT-Bench / IFEval / AlpacaEval runners\n");
    return HU_OK;
}

hu_error_t hu_eval_cli_gate(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return print_subcommand_help("gate");
    }
    printf("human eval gate: LoRA promotion gate (bootstrap CI)\n");
    return HU_OK;
}

#ifdef HU_IS_TEST
bool hu_build_has_competitive_eval(void) {
#ifdef HU_ENABLE_RL_FULL
    return true;
#else
    return false;
#endif
}
#endif
