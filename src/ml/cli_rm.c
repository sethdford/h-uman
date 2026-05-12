/* src/ml/cli_rm.c — Phase 3 Task 10
 *
 * `human ml rm-train` handler: trains a reward model on two-sided
 * preference pairs using Bradley-Terry loss. One-sided KTO pairs
 * (chosen_len == 0 OR rejected_len == 0) are filtered at load time.
 */
#include "human/ml/cli_rm.h"
#include "human/ml/reward_model.h"
#include "human/ml/dpo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *rm_get_opt(const char **argv, int argc, int *i, const char *flag) {
    if (strcmp(argv[*i], flag) == 0 && *i + 1 < argc) {
        (*i)++;
        return argv[*i];
    }
    return NULL;
}

hu_error_t hu_ml_cli_rm_train(hu_allocator_t *alloc, int argc, const char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;

    const char *pairs_path = NULL;
    const char *backend_str = "huml";
    const char *save_dir = NULL;
    int max_iters = 200;
    double learning_rate = 1e-2;
    size_t vocab_size = 32;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml rm-train [options]\n"
                   "  --pairs <path>         JSONL preference pairs (two-sided only)\n"
                   "  --backend {huml,mlx}   default: huml\n"
                   "  --save <dir>           save trained RM to directory\n"
                   "  --iters <N>            training iterations (default: 200)\n"
                   "  --learning-rate <f>    SGD learning rate (default: 0.01)\n"
                   "  --vocab-size <N>       HUML toy GPT vocab (default: 32)\n"
                   "\n"
                   "HUML backend trains the toy reference GPT — useful for gradient\n"
                   "verification, NOT for improving real chat. Use --backend mlx for\n"
                   "real Qwen-scale reward model training.\n");
            return HU_OK;
        }
        const char *v;
        if ((v = rm_get_opt(argv, argc, &i, "--pairs")))          pairs_path = v;
        else if ((v = rm_get_opt(argv, argc, &i, "--backend")))   backend_str = v;
        else if ((v = rm_get_opt(argv, argc, &i, "--save")))      save_dir = v;
        else if ((v = rm_get_opt(argv, argc, &i, "--iters")))     max_iters = atoi(v);
        else if ((v = rm_get_opt(argv, argc, &i, "--learning-rate"))) learning_rate = atof(v);
        else if ((v = rm_get_opt(argv, argc, &i, "--vocab-size"))) vocab_size = (size_t)atoi(v);
    }

    hu_reward_model_backend_t backend = HU_REWARD_MODEL_BACKEND_HUML;
    if (strcmp(backend_str, "mlx") == 0) backend = HU_REWARD_MODEL_BACKEND_MLX;

    hu_reward_model_config_t cfg = {
        .backend = backend,
        .vocab_size = vocab_size,
        .hidden_dim = vocab_size,
    };
    hu_reward_model_t rm = {0};
    hu_error_t err;
    if (backend == HU_REWARD_MODEL_BACKEND_HUML) {
        err = hu_reward_model_create_huml(alloc, &cfg, &rm);
    } else {
        err = hu_reward_model_create_mlx(alloc, &cfg, &rm);
    }
    if (err != HU_OK) {
        fprintf(stderr, "[rm-train] failed to create reward model: error %d\n", (int)err);
        return err;
    }

    hu_preference_pair_t pairs[256];
    memset(pairs, 0, sizeof(pairs));
    size_t n_pairs = 0;

    if (pairs_path) {
        FILE *f = fopen(pairs_path, "r");
        if (!f) {
            fprintf(stderr, "[rm-train] cannot open %s\n", pairs_path);
            rm.vtable->deinit(rm.ctx, alloc);
            return HU_ERR_IO;
        }
        char line[8192];
        while (fgets(line, sizeof(line), f) && n_pairs < 256) {
            hu_preference_pair_t *p = &pairs[n_pairs];
            /* Minimal JSON parsing for {"prompt":"...","chosen":"...","rejected":"..."} */
            const char *pf = strstr(line, "\"prompt\"");
            const char *cf = strstr(line, "\"chosen\"");
            const char *rf = strstr(line, "\"rejected\"");
            if (!pf) continue;

            /* Extract prompt */
            const char *ps = strchr(pf + 8, '"');
            if (ps) {
                ps++;
                const char *pe = strchr(ps, '"');
                if (pe) {
                    size_t len = (size_t)(pe - ps);
                    if (len >= sizeof(p->prompt)) len = sizeof(p->prompt) - 1;
                    memcpy(p->prompt, ps, len);
                    p->prompt[len] = '\0';
                    p->prompt_len = len;
                }
            }
            /* Extract chosen */
            if (cf) {
                const char *cs = strchr(cf + 8, '"');
                if (cs) {
                    cs++;
                    const char *ce = strchr(cs, '"');
                    if (ce) {
                        size_t len = (size_t)(ce - cs);
                        if (len >= sizeof(p->chosen)) len = sizeof(p->chosen) - 1;
                        memcpy(p->chosen, cs, len);
                        p->chosen[len] = '\0';
                        p->chosen_len = len;
                    }
                }
            }
            /* Extract rejected */
            if (rf) {
                const char *rs = strchr(rf + 10, '"');
                if (rs) {
                    rs++;
                    const char *re = strchr(rs, '"');
                    if (re) {
                        size_t len = (size_t)(re - rs);
                        if (len >= sizeof(p->rejected)) len = sizeof(p->rejected) - 1;
                        memcpy(p->rejected, rs, len);
                        p->rejected[len] = '\0';
                        p->rejected_len = len;
                    }
                }
            }
            if (p->prompt_len == 0) continue;
            if (p->chosen_len == 0 || p->rejected_len == 0) {
                fprintf(stderr, "[rm-train] [skip] one-sided KTO pair — RM requires two-sided\n");
                memset(p, 0, sizeof(*p));
                continue;
            }
            n_pairs++;
        }
        fclose(f);
    }

    if (n_pairs == 0) {
        fprintf(stderr, "[rm-train] no valid two-sided pairs found\n");
        rm.vtable->deinit(rm.ctx, alloc);
        return HU_ERR_INVALID_ARGUMENT;
    }

    fprintf(stderr, "[rm-train] loaded %zu pairs, iters=%d, lr=%.4f\n",
            n_pairs, max_iters, learning_rate);

    hu_reward_model_train_config_t tcfg = {
        .max_iters = (size_t)max_iters,
        .learning_rate = learning_rate,
        .log_every = 10,
    };
    hu_reward_model_train_metrics_t metrics = {0};
    err = hu_reward_model_train(&rm, alloc, pairs, n_pairs, &tcfg, &metrics);
    if (err != HU_OK) {
        fprintf(stderr, "[rm-train] training failed: error %d\n", (int)err);
        rm.vtable->deinit(rm.ctx, alloc);
        return err;
    }

    fprintf(stderr, "[rm-train] done: initial_loss=%.6f final_loss=%.6f iters=%zu\n",
            metrics.initial_loss, metrics.final_loss, metrics.iters_completed);

    if (save_dir) {
        err = hu_reward_model_save(&rm, save_dir);
        if (err == HU_ERR_NOT_SUPPORTED) {
            fprintf(stderr, "[rm-train] save not yet implemented (Task 9)\n");
        } else if (err != HU_OK) {
            fprintf(stderr, "[rm-train] save failed: error %d\n", (int)err);
        }
    }

    rm.vtable->deinit(rm.ctx, alloc);
    return HU_OK;
}
