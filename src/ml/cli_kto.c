/* src/ml/cli_kto.c — Phase 3 Task 9
 *
 * `human ml kto-train` handler: trains a KTO trainer on one-sided
 * preference signals. Two-sided pairs are valid input but will be
 * silently skipped by the KTO step() implementation.
 */
#include "human/ml/cli_kto.h"
#include "human/ml/rl_trainer.h"
#include "human/ml/dpo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kto_get_opt(const char **argv, int argc, int *i, const char *flag) {
    if (strcmp(argv[*i], flag) == 0 && *i + 1 < argc) {
        (*i)++;
        return argv[*i];
    }
    return NULL;
}

hu_error_t hu_ml_cli_kto_train(hu_allocator_t *alloc, int argc, const char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;

    const char *pairs_path = NULL;
    const char *backend_str = "auto";
    const char *adapter_out = "./kto-adapters";
    int max_iters = 100;
    double beta = 0.1;
    double lambda_d = 1.0;
    double lambda_u = 1.0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml kto-train [options]\n"
                   "  --pairs <path>         JSONL one-sided preference signals\n"
                   "  --backend {auto,huml,mlx}  default: auto\n"
                   "  --iters <N>            training iterations (default: 100)\n"
                   "  --beta <float>         KTO temperature (default: 0.1)\n"
                   "  --lambda-d <float>     desirable signal weight (default: 1.0)\n"
                   "  --lambda-u <float>     undesirable signal weight (default: 1.0)\n"
                   "  --adapter-out <dir>    output adapter directory\n"
                   "\n"
                   "HUML backend trains the toy reference GPT — useful for gradient\n"
                   "verification, NOT for improving real chat. Use --backend mlx (or\n"
                   "auto on Apple) for real Gemma adapters.\n");
            return HU_OK;
        }
        const char *v;
        if ((v = kto_get_opt(argv, argc, &i, "--pairs")))          pairs_path = v;
        else if ((v = kto_get_opt(argv, argc, &i, "--backend")))   backend_str = v;
        else if ((v = kto_get_opt(argv, argc, &i, "--iters")))     max_iters = atoi(v);
        else if ((v = kto_get_opt(argv, argc, &i, "--beta")))      beta = atof(v);
        else if ((v = kto_get_opt(argv, argc, &i, "--lambda-d")))  lambda_d = atof(v);
        else if ((v = kto_get_opt(argv, argc, &i, "--lambda-u")))  lambda_u = atof(v);
        else if ((v = kto_get_opt(argv, argc, &i, "--adapter-out"))) adapter_out = v;
    }

    hu_dpo_backend_t backend = HU_DPO_BACKEND_AUTO;
    if (strcmp(backend_str, "huml") == 0) backend = HU_DPO_BACKEND_HUML;
    else if (strcmp(backend_str, "mlx") == 0) backend = HU_DPO_BACKEND_MLX;

    hu_rl_trainer_config_t cfg = {
        .backend = backend,
        .beta = beta,
        .learning_rate = 1e-3,
        .max_iters = (size_t)max_iters,
        .adapter_out_dir = adapter_out,
        .lambda_d = lambda_d,
        .lambda_u = lambda_u,
    };

    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_kto(alloc, &cfg, &trainer);
    if (err != HU_OK) {
        fprintf(stderr, "[kto-train] failed to create KTO trainer: error %d\n", (int)err);
        return err;
    }
    fprintf(stderr, "[kto-train] backend=%s, iters=%d, beta=%.2f, lambda_d=%.2f, lambda_u=%.2f\n",
            trainer.vtable->name(trainer.ctx), max_iters, beta, lambda_d, lambda_u);

    hu_preference_pair_t pairs[256];
    memset(pairs, 0, sizeof(pairs));
    size_t n_pairs = 0;

    if (pairs_path) {
        FILE *f = fopen(pairs_path, "r");
        if (!f) {
            fprintf(stderr, "[kto-train] cannot open %s\n", pairs_path);
            trainer.vtable->deinit(trainer.ctx, alloc);
            return HU_ERR_IO;
        }
        char line[8192];
        while (fgets(line, sizeof(line), f) && n_pairs < 256) {
            hu_preference_pair_t *p = &pairs[n_pairs];
            const char *pf = strstr(line, "\"prompt\"");
            const char *cf = strstr(line, "\"chosen\"");
            const char *rf = strstr(line, "\"rejected\"");
            if (!pf) continue;

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
            if (p->chosen_len == 0 && p->rejected_len == 0) {
                fprintf(stderr, "[kto-train] [skip] pair with no chosen/rejected\n");
                memset(p, 0, sizeof(*p));
                continue;
            }
            n_pairs++;
        }
        fclose(f);
    }

    if (n_pairs == 0) {
        fprintf(stderr, "[kto-train] no valid pairs found\n");
        trainer.vtable->deinit(trainer.ctx, alloc);
        return HU_ERR_INVALID_ARGUMENT;
    }

    fprintf(stderr, "[kto-train] loaded %zu signals\n", n_pairs);

    for (int it = 0; it < max_iters; it++) {
        hu_rl_trainer_metrics_t m = {0};
        err = trainer.vtable->step(trainer.ctx, alloc, pairs, n_pairs, &m);
        if (err != HU_OK) {
            fprintf(stderr, "[kto-train] step %d failed: error %d\n", it, (int)err);
            break;
        }
        if ((it + 1) % 10 == 0 || it == 0) {
            fprintf(stderr, "[kto-train] iter=%d loss=%.6f chosen_delta=%.6f rejected_delta=%.6f\n",
                    it + 1, m.final_loss, m.chosen_logprob_delta, m.rejected_logprob_delta);
        }
    }

    if (adapter_out && err == HU_OK) {
        hu_error_t se = trainer.vtable->save_adapter(trainer.ctx, alloc, adapter_out);
        if (se == HU_ERR_NOT_SUPPORTED) {
            fprintf(stderr, "[kto-train] adapter save not yet implemented for this backend\n");
        } else if (se != HU_OK) {
            fprintf(stderr, "[kto-train] adapter save failed: error %d\n", (int)se);
        }
    }

    trainer.vtable->deinit(trainer.ctx, alloc);
    return err;
}
