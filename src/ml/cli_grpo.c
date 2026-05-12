/* src/ml/cli_grpo.c — Phase 4 Task 9 (RL SOTA)
 *
 * `human ml grpo-train` handler: trains a GRPO trainer (Group Relative
 * Policy Optimization, Shao et al. 2024 — DeepSeekMath §4.1.2) on the
 * `prompt` field of a JSONL preference-pair file. The chosen/rejected
 * columns are deliberately ignored — GRPO samples N rollouts from the
 * live policy and scores them with a hu_reward_source_t (synthetic
 * token-counting by default; RM/judge composition lands in Task 10 /
 * Phase 5).
 *
 * Validation gates (umbrella §10):
 *   R9 reward-hacking — explicit --reward-fn beats convenient default.
 *   R12 group-baseline degeneracy — N<2 has zero variance; N>1024 OOM.
 *   Phase 3 cli_rm precedent — --backend mlx demands --backbone-path.
 *
 * Structural pattern mirrors src/ml/cli_kto.c (Phase 3 Task 9) — argv
 * parser via strtol with R12 bounds, JSONL loader extracting only the
 * `prompt` field, trainer creation via hu_rl_trainer_create_grpo,
 * step loop with periodic metrics log, final save_adapter.
 *
 * Plan deviation note (round-3 fix SV1): dispatch lives in
 * src/main.c::cmd_ml (NOT src/ml/cli.c), matching the Phase 2/3
 * pattern — the umbrella §4.5 row 4 budget for this CLI is ≤15 LOC of
 * dispatch + help-text adds, and src/main.c::cmd_ml is where every
 * other ml subcommand is routed today.
 */
#include "human/ml/cli_grpo.h"
#include "human/ml/grpo.h"          /* Task 10: hu_grpo_set_reward_source */
#include "human/ml/rl_trainer.h"
#include "human/ml/dpo.h"
#include "human/ml/reward_model.h"
#include "human/ml/reward_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same one-flag get_opt helper as src/ml/cli_kto.c / src/ml/cli_rm.c —
 * advances *i on a hit so the caller's for-loop reads the next flag. */
static const char *grpo_get_opt(const char **argv, int argc, int *i, const char *flag) {
    if (strcmp(argv[*i], flag) == 0 && *i + 1 < argc) {
        (*i)++;
        return argv[*i];
    }
    return NULL;
}

/* Strict integer parser used by R12 bounds checks. Returns 0 + writes
 * *out on success; returns -1 (and leaves *out unchanged) on any of:
 *   - empty string
 *   - non-numeric prefix / trailing garbage
 *   - overflow beyond long range
 * Critic M1 + R12: --rollouts "abc" must reject at parse time, not
 * propagate atoi(...)==0 silently into the GRPO factory. */
static int grpo_parse_long(const char *s, long *out) {
    if (!s || !s[0]) return -1;
    char *endp = NULL;
    long v = strtol(s, &endp, 10);
    if (!endp || endp == s || *endp != '\0') return -1;
    *out = v;
    return 0;
}

hu_error_t hu_ml_cli_grpo_train(hu_allocator_t *alloc, int argc, const char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;

    const char *pairs_path     = NULL;
    const char *backend_str    = "auto";
    const char *reward_fn_arg  = NULL;
    const char *reward_model_arg = NULL;
    const char *backbone_path  = NULL;
    const char *adapter_out    = NULL;
    long  n_rollouts_raw = 4;
    long  max_iters_raw  = 100;
    long  max_new_tokens = 16;
    double clip_eps      = 0.2;
    /* R3 / MED-1: kl_beta is a tri-state sentinel.
     *   < 0  → trainer maps to literature default 0.04 (DeepSeek R1)
     *   == 0 → KL DISABLED (R4 escape valve)
     *   > 0  → literal value
     * The CLI default is "use the trainer's default" (-1 sentinel) so
     * the user has to explicitly pass --kl-beta 0 to opt out of KL. */
    double kl_beta       = -1.0;
    double learning_rate = 1e-2;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml grpo-train [options]\n"
                   "  --pairs <path>           JSONL prompt rows (chosen/rejected ignored)\n"
                   "  --backend {auto,huml,mlx}  default: auto (Apple → mlx if available; else huml)\n"
                   "  --backbone-path <path>   MLX-only: HF id or local path forwarded as --backbone-path\n"
                   "                           to mlx-lm-lora (e.g. mlx-community/gemma-3-4b-it-bf16)\n"
                   "  --reward-fn {synthetic,rm,judge}  REQUIRED (R9 — no implicit default)\n"
                   "  --reward-model <dir>     required when --reward-fn rm (Phase 3 RM checkpoint dir)\n"
                   "  --rollouts <N>           per-prompt rollouts (default: 4; rejects N<2 || N>1024 — R12)\n"
                   "  --clip-eps <f>           PPO ratio clip ε (default: 0.2)\n"
                   "  --kl-beta <f>            KL penalty coefficient (default: 0.04; pass 0 to disable)\n"
                   "  --iters <N>              training iterations (default: 100)\n"
                   "  --learning-rate <f>      structural-backward step size (default: 0.01)\n"
                   "  --max-new-tokens <N>     per-rollout completion cap (default: 16)\n"
                   "  --adapter-out <path>     output adapter (HUML: lm_head bytes file;\n"
                   "                           MLX: adapter directory)\n"
                   "\n"
                   "HUML backend trains the toy reference GPT — useful for gradient verification,\n"
                   "NOT for improving real chat. Use --backend mlx (or auto on Apple) for real\n"
                   "Gemma adapters via mlx-lm-lora's GRPO trainer.\n");
            return HU_OK;
        }
        const char *v;
        if      ((v = grpo_get_opt(argv, argc, &i, "--pairs")))           pairs_path = v;
        else if ((v = grpo_get_opt(argv, argc, &i, "--backend")))         backend_str = v;
        else if ((v = grpo_get_opt(argv, argc, &i, "--backbone-path")))   backbone_path = v;
        else if ((v = grpo_get_opt(argv, argc, &i, "--reward-fn")))       reward_fn_arg = v;
        else if ((v = grpo_get_opt(argv, argc, &i, "--reward-model")))    reward_model_arg = v;
        else if ((v = grpo_get_opt(argv, argc, &i, "--adapter-out")))     adapter_out = v;
        else if ((v = grpo_get_opt(argv, argc, &i, "--rollouts"))) {
            if (grpo_parse_long(v, &n_rollouts_raw) != 0) {
                fprintf(stderr, "[grpo-train] ERROR: --rollouts must be a base-10 integer "
                                "(got \"%s\")\n", v);
                return HU_ERR_INVALID_ARGUMENT;
            }
        }
        else if ((v = grpo_get_opt(argv, argc, &i, "--iters"))) {
            if (grpo_parse_long(v, &max_iters_raw) != 0 || max_iters_raw < 1) {
                fprintf(stderr, "[grpo-train] ERROR: --iters must be a positive integer "
                                "(got \"%s\")\n", v);
                return HU_ERR_INVALID_ARGUMENT;
            }
        }
        else if ((v = grpo_get_opt(argv, argc, &i, "--max-new-tokens"))) {
            if (grpo_parse_long(v, &max_new_tokens) != 0 || max_new_tokens < 1) {
                fprintf(stderr, "[grpo-train] ERROR: --max-new-tokens must be a positive integer "
                                "(got \"%s\")\n", v);
                return HU_ERR_INVALID_ARGUMENT;
            }
        }
        else if ((v = grpo_get_opt(argv, argc, &i, "--clip-eps")))        clip_eps = atof(v);
        else if ((v = grpo_get_opt(argv, argc, &i, "--kl-beta")))         kl_beta  = atof(v);
        else if ((v = grpo_get_opt(argv, argc, &i, "--learning-rate")))   learning_rate = atof(v);
    }

    /* ---------------- Validation gates (umbrella §10) -------------- */

    /* R12: explicit n_rollouts must be in [2, 1024]. N<2 collapses the
     * group baseline std to zero (no gradient signal); N>1024 is almost
     * certainly a typo and explodes the rollout array allocation. */
    if (n_rollouts_raw < 2 || n_rollouts_raw > 1024) {
        fprintf(stderr,
                "[grpo-train] ERROR: --rollouts must be in [2, 1024] (got %ld) — "
                "GRPO requires N >= 2 (group baseline std degenerates with N=1) "
                "per critic R12 / M1\n", n_rollouts_raw);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (n_rollouts_raw < 4) {
        fprintf(stderr,
                "[grpo-train] WARNING: --rollouts=%ld < 4; group baseline variance "
                "is high — recommended N >= 4 for stable training\n", n_rollouts_raw);
    }

    /* R9 reward-hacking pin: no implicit default for the reward source.
     * The CLI errors out if neither --reward-fn nor --reward-model was
     * supplied — picking the wrong source silently is the named risk. */
    if (!reward_fn_arg && !reward_model_arg) {
        fprintf(stderr,
                "[grpo-train] ERROR: --reward-fn is required (one of: synthetic, rm, judge) "
                "per umbrella §10 R9 reward-hacking guard. Pass --reward-fn synthetic for the "
                "cold-start token-counting reward, or --reward-fn rm --reward-model <dir> to "
                "load a Phase 3 reward-model checkpoint.\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Per-backend cross-flag validation. */
    hu_dpo_backend_t backend = HU_DPO_BACKEND_AUTO;
    if      (strcmp(backend_str, "huml") == 0) backend = HU_DPO_BACKEND_HUML;
    else if (strcmp(backend_str, "mlx")  == 0) backend = HU_DPO_BACKEND_MLX;
    else if (strcmp(backend_str, "auto") == 0) backend = HU_DPO_BACKEND_AUTO;
    else {
        fprintf(stderr,
                "[grpo-train] ERROR: --backend must be one of {auto, huml, mlx} (got \"%s\")\n",
                backend_str);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Phase 3 cli_rm precedent: --backend mlx demands --backbone-path,
     * otherwise the MLX subprocess crashes at popen() time with an
     * opaque "missing positional arg" error from the python wrapper. */
    if (backend == HU_DPO_BACKEND_MLX && (!backbone_path || !backbone_path[0])) {
        fprintf(stderr,
                "[grpo-train] ERROR: --backend mlx requires --backbone-path <path-or-hf-id>\n"
                "[grpo-train] e.g. mlx-community/gemma-3-4b-it-bf16 (forwarded as the\n"
                "[grpo-train] --backbone-path argument to mlx-lm-lora's GRPO trainer)\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Reward-fn-specific arg checks. The actual reward source wiring
     * happens AFTER trainer construction. */
    if (reward_fn_arg) {
        if (strcmp(reward_fn_arg, "synthetic") != 0 &&
            strcmp(reward_fn_arg, "rm")        != 0 &&
            strcmp(reward_fn_arg, "judge")     != 0) {
            fprintf(stderr,
                    "[grpo-train] ERROR: --reward-fn must be one of {synthetic, rm, judge} "
                    "(got \"%s\")\n", reward_fn_arg);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(reward_fn_arg, "rm") == 0 && (!reward_model_arg || !reward_model_arg[0])) {
            fprintf(stderr,
                    "[grpo-train] ERROR: --reward-fn rm requires --reward-model <dir>\n"
                    "[grpo-train] e.g. tests/fixtures/rm_synthetic_checkpoint (Phase 3 RM\n"
                    "[grpo-train] checkpoint with value_head.vh + rm_meta.json)\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
    } else if (reward_model_arg && reward_model_arg[0]) {
        /* Inferred --reward-fn=rm when only --reward-model is given —
         * still legal per R9 because the model arg encodes the choice. */
        reward_fn_arg = "rm";
    }

    if (!adapter_out) adapter_out = "./grpo-adapters";

    /* ---------------- Trainer construction -------------------------- */

    hu_rl_trainer_config_t cfg = {
        .backend       = backend,
        .beta          = 0.1, /* DPO-only; GRPO impl IGNOREs */
        .learning_rate = learning_rate,
        .max_iters     = (size_t)max_iters_raw,
        .model_id      = backbone_path,         /* MLX: forwarded; HUML: ignored */
        .adapter_out_dir = adapter_out,         /* MLX: writes adapter_model.safetensors here */
        .lambda_d      = 1.0,                   /* KTO-only; ignored by GRPO */
        .lambda_u      = 1.0,
        .n_rollouts    = (size_t)n_rollouts_raw,
        .clip_eps      = clip_eps,
        .kl_beta       = kl_beta,
        .gamma         = 0.5,                   /* SimPO-only; ignored */
        .length_norm   = false,
        .lambda_or     = 0.1,                   /* ORPO-only; ignored */
        .odds_clip     = 10.0,
    };

    /* --reward-fn judge is always not-supported in Phase 4 (Phase 5
     * lands the real multi-judge consensus impl per umbrella §10 R3).
     * Surfaced BEFORE trainer construction so the failure path doesn't
     * leak a half-built trainer. */
    if (strcmp(reward_fn_arg, "judge") == 0) {
        fprintf(stderr,
                "[grpo-train] --reward-fn judge is not implemented in Phase 4 — "
                "Phase 5 lands the real multi-judge consensus reward source per "
                "umbrella §10 R3.\n");
        return HU_ERR_NOT_SUPPORTED;
    }

    /* ---------------- Reward-model load (Task 10) ------------------- *
     *
     * For --reward-fn rm we load the Phase 3 RM checkpoint BEFORE
     * constructing the trainer so a missing/corrupt fixture doesn't
     * leak a half-built trainer on the failure path. The RM owns its
     * own backbone + value head; the GRPO trainer will only BORROW the
     * RM pointer (via hu_reward_source_create_rm). RM lifetime must
     * outlive trainer.deinit(), so we keep both in scope and tear
     * down in the reverse order at the end of the function. */
    hu_reward_model_t rm = {0};
    int rm_loaded = 0;
    if (strcmp(reward_fn_arg, "rm") == 0) {
        hu_error_t lerr = hu_reward_model_load(alloc, reward_model_arg, &rm);
        if (lerr != HU_OK) {
            fprintf(stderr,
                    "[grpo-train] failed to load reward model from \"%s\": error %d\n"
                    "[grpo-train] expected layout: <dir>/value_head.vh + <dir>/rm_meta.json\n"
                    "[grpo-train] regenerate via scripts/build-rm-fixture.sh\n",
                    reward_model_arg, (int)lerr);
            return lerr;
        }
        rm_loaded = 1;
    }

    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_grpo(alloc, &cfg, &trainer);
    if (err != HU_OK) {
        fprintf(stderr, "[grpo-train] failed to create GRPO trainer: error %d\n", (int)err);
        if (rm_loaded) rm.vtable->deinit(rm.ctx, alloc);
        return err;
    }

    /* ---------------- Reward source swap (Task 10) ------------------ *
     *
     * The HUML backend auto-installs a synthetic reward source inside
     * hu_grpo_huml_create. For --reward-fn rm we wrap the loaded RM in
     * a Phase 4 Task 4 hu_reward_source_t and swap it onto the trainer
     * via hu_grpo_set_reward_source. The trainer takes ownership of
     * the new source (by value-copy) and deinits the synthetic one.
     * The RM pointer inside the source is BORROWED — we (the CLI) keep
     * ownership of `rm` until the final cleanup below.
     *
     * For --reward-fn synthetic the default source is already what we
     * want; no swap needed. */
    if (strcmp(reward_fn_arg, "rm") == 0) {
        hu_reward_source_t rm_source = {0};
        err = hu_reward_source_create_rm(alloc, &rm, &rm_source);
        if (err != HU_OK) {
            fprintf(stderr,
                    "[grpo-train] failed to wrap reward model in reward source: error %d\n",
                    (int)err);
            trainer.vtable->deinit(trainer.ctx, alloc);
            rm.vtable->deinit(rm.ctx, alloc);
            return err;
        }
        err = hu_grpo_set_reward_source(&trainer, rm_source);
        if (err != HU_OK) {
            /* Setter rejected the swap (e.g. MLX backend) — drop the
             * source ourselves since the trainer never took ownership.
             * The CLI's earlier --backend mlx + --reward-fn rm path is
             * legal in Phase 4 (MLX subprocess can be wired to use a
             * remote RM in Phase 5); the in-process setter just isn't
             * the integration point for it. */
            fprintf(stderr,
                    "[grpo-train] cannot swap reward source on this trainer: error %d "
                    "(MLX backend scores in Python; --reward-fn rm wiring is HUML-only)\n",
                    (int)err);
            if (rm_source.vtable && rm_source.vtable->deinit) {
                rm_source.vtable->deinit(&rm_source);
            }
            trainer.vtable->deinit(trainer.ctx, alloc);
            rm.vtable->deinit(rm.ctx, alloc);
            return err;
        }
    }

    /* ---------------- JSONL prompt ingest --------------------------- *
     *
     * Mirrors the inline parser in src/ml/cli_kto.c / src/ml/cli_rm.c —
     * minimal {"prompt":"...","chosen":"...","rejected":"..."} extraction
     * with chosen/rejected silently ignored. GRPO only consumes the
     * prompt; rollouts are sampled from the live policy at step() time. */
    hu_preference_pair_t pairs[256];
    memset(pairs, 0, sizeof(pairs));
    size_t n_pairs = 0;

    if (pairs_path) {
        FILE *f = fopen(pairs_path, "r");
        if (!f) {
            fprintf(stderr, "[grpo-train] cannot open %s\n", pairs_path);
            trainer.vtable->deinit(trainer.ctx, alloc);
            if (rm_loaded) rm.vtable->deinit(rm.ctx, alloc);
            return HU_ERR_IO;
        }
        char line[8192];
        while (fgets(line, sizeof(line), f) && n_pairs < 256) {
            hu_preference_pair_t *p = &pairs[n_pairs];
            const char *pf = strstr(line, "\"prompt\"");
            if (!pf) continue;
            const char *ps = strchr(pf + 8, '"');
            if (!ps) continue;
            ps++;
            const char *pe = strchr(ps, '"');
            if (!pe) continue;
            size_t len = (size_t)(pe - ps);
            if (len >= sizeof(p->prompt)) len = sizeof(p->prompt) - 1;
            memcpy(p->prompt, ps, len);
            p->prompt[len] = '\0';
            p->prompt_len  = len;
            if (p->prompt_len == 0) continue;
            n_pairs++;
        }
        fclose(f);
    }

    if (n_pairs == 0) {
        fprintf(stderr, "[grpo-train] no valid prompts found in %s\n",
                pairs_path ? pairs_path : "(--pairs missing)");
        trainer.vtable->deinit(trainer.ctx, alloc);
        if (rm_loaded) rm.vtable->deinit(rm.ctx, alloc);
        return HU_ERR_INVALID_ARGUMENT;
    }

    fprintf(stderr,
            "[grpo-train] backend=%s, prompts=%zu, rollouts=%ld, iters=%ld, "
            "clip_eps=%.3f, kl_beta=%.3f, lr=%.4f, reward_fn=%s\n",
            trainer.vtable->name(trainer.ctx), n_pairs, n_rollouts_raw, max_iters_raw,
            clip_eps, kl_beta, learning_rate, reward_fn_arg);

    /* ---------------- Step loop ------------------------------------- */
    for (long it = 0; it < max_iters_raw; it++) {
        hu_rl_trainer_metrics_t m = {0};
        err = trainer.vtable->step(trainer.ctx, alloc, pairs, n_pairs, &m);
        if (err != HU_OK) {
            fprintf(stderr, "[grpo-train] step %ld failed: error %d\n", it, (int)err);
            break;
        }
        if ((it + 1) % 10 == 0 || it == 0) {
            fprintf(stderr,
                    "[grpo-train] iter=%ld loss=%.6f advantage_chosen=%.6f advantage_rejected=%.6f\n",
                    it + 1, m.final_loss, m.chosen_logprob_delta, m.rejected_logprob_delta);
        }
    }

    /* ---------------- Save adapter ---------------------------------- */
    if (err == HU_OK && adapter_out) {
        hu_error_t se = trainer.vtable->save_adapter(trainer.ctx, alloc, adapter_out);
        if (se == HU_ERR_NOT_SUPPORTED) {
            fprintf(stderr, "[grpo-train] adapter save not yet implemented for this backend\n");
        } else if (se != HU_OK) {
            fprintf(stderr, "[grpo-train] adapter save failed: error %d\n", (int)se);
        } else {
            fprintf(stderr, "[grpo-train] saved adapter to %s\n", adapter_out);
        }
    }

    /* Tear down in reverse construction order. The trainer's deinit
     * calls into the reward source's deinit (which frees its rm_ctx_t
     * scratch but does NOT touch the borrowed RM, per the Task 4
     * contract). After the trainer is gone the RM can be safely
     * deinitialized. */
    trainer.vtable->deinit(trainer.ctx, alloc);
    if (rm_loaded) rm.vtable->deinit(rm.ctx, alloc);
    return err;
}
