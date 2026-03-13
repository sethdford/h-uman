/* ML CLI subcommands: train, experiment, prepare, status. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/cli.h"
#include "human/ml/experiment.h"
#include "human/ml/prepare.h"
#include "human/ml/tokenizer_ml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_int_arg(const char *val, int *out) {
    if (!val || !out)
        return -1;
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end == val || *end != '\0' || n < 0)
        return -1;
    *out = (int)n;
    return 0;
}

static const char *get_opt(const char **argv, int argc, int i, const char *opt) {
    if (i + 1 < argc && strcmp(argv[i], opt) == 0)
        return argv[i + 1];
    return NULL;
}

hu_error_t hu_ml_cli_train(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)alloc;
    const char *config_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--config");
        if (v) {
            config_path = v;
            break;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml train [--config <path>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)config_path;
    return HU_OK;
#else
    if (!config_path)
        config_path = "config.json";
    /* Stub: real impl would load JSON config and run training */
    printf("Training with config: %s\n", config_path);
    return HU_OK;
#endif
}

hu_error_t hu_ml_cli_experiment(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *config_path = NULL;
    const char *data_dir = NULL;
    int max_iterations = 10;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--config");
        if (v)
            config_path = v;
        v = get_opt(argv, argc, i, "--data-dir");
        if (v)
            data_dir = v;
        v = get_opt(argv, argc, i, "--max-iterations");
        if (v && parse_int_arg(v, &max_iterations) != 0) {
            fprintf(stderr, "Invalid --max-iterations: %s\n", v);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human experiment [--config <path>] [--max-iterations <N>] "
                   "[--data-dir <path>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)config_path;
    (void)data_dir;
    (void)max_iterations;
    return HU_OK;
#else
    if (!data_dir)
        data_dir = ".";
    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = max_iterations;
    loop_cfg.base_config = hu_experiment_config_default();
    loop_cfg.data_dir = data_dir;
    loop_cfg.convergence_threshold = 0.0;
    (void)config_path;
    return hu_experiment_loop(alloc, &loop_cfg, NULL, NULL);
#endif
}

hu_error_t hu_ml_cli_prepare(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *input_dir = NULL;
    const char *output_dir = NULL;
    int vocab_size = 8192;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--input");
        if (v)
            input_dir = v;
        v = get_opt(argv, argc, i, "--output");
        if (v)
            output_dir = v;
        v = get_opt(argv, argc, i, "--vocab-size");
        if (v && parse_int_arg(v, &vocab_size) != 0) {
            fprintf(stderr, "Invalid --vocab-size: %s\n", v);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml prepare [--input <dir>] [--output <dir>] "
                   "[--vocab-size <N>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)input_dir;
    (void)output_dir;
    (void)vocab_size;
    return HU_OK;
#else
    if (!input_dir || !output_dir) {
        fprintf(stderr, "prepare requires --input and --output\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK)
        return err;
    /* Use byte-level tokenizer; optional BPE training would need corpus scan */
    (void)vocab_size;
    err = hu_ml_prepare_tokenize_dir(alloc, tok, input_dir, output_dir);
    hu_bpe_tokenizer_deinit(tok);
    return err;
#endif
}

hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
#ifdef HU_IS_TEST
    printf("No experiments found\n");
    return HU_OK;
#else
    const char *path = "results.tsv";
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("No experiments found\n");
        return HU_OK;
    }
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '\0' && line[0] != '#') {
            printf("%s", line);
            count++;
        }
    }
    fclose(f);
    if (count == 0)
        printf("No experiments found\n");
    return HU_OK;
#endif
}
