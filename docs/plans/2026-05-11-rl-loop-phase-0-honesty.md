---
status: closed
last_audit: 2026-05-25
---

# Phase 0: Honesty Pass — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix every silent bug, misleading name, and documentation drift identified in the May 11 2026 audit baseline so the RL work in Phases 1–6 builds on truth, not lies. Archive the audit. Add private-data discipline.

**Architecture:** Surgical edits only — no new abstractions, no new vtables. Five still-broken issues confirmed against the in-flight Track D Phase 1 tree: `vocab_size=0` at three CLI/experiment sites, `hu_personal_model_save` non-atomic, `hu_dpo_train_step` mis-named (it's an LLM judge, not DPO), `CLAUDE.md:53` documents fiction. Plus: add `~/.human/private/` to `.gitignore` per spec §13, archive the audit, write three regression-prevention tests.

**Tech Stack:** C11, the existing `tests/test_framework.h` test harness, AddressSanitizer, conventional commits.

**Linked spec:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` §1.5.2, §4.1
**Linked umbrella plan:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md`

---

## Task 0: Phase Start Gate

**Files:** none (subagent dispatch)

- [ ] **Step 1: Re-pull main and rebase**

```bash
git fetch origin && git checkout main && git pull --ff-only
```

- [ ] **Step 2: Re-run §1.5.1 fold-in mapping check from the spec**

Read the latest commits in `src/ml/`, `src/memory/personal_model.{h,c}`, `src/persona/`. Confirm:
- `hu_communication_style_fidelity_score` still 3-axis (no 4th axis landed yet)
- `hu_personal_model_save` still non-atomic at the line shown below
- `hu_dpo_train_step` still mis-named
- `CLAUDE.md:53` still claims atomic-rename

If any of these have shipped, this Phase 0 plan needs revision before starting.

- [ ] **Step 3: Dispatch `spec-verifier` subagent**

```
Task: spec-verifier
Prompt: Read docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md §4.1 and
        docs/plans/2026-05-11-rl-loop-phase-0-honesty.md (this file). Report any gap
        between what §4.1 promises and what this Phase 0 plan delivers. Gate criterion:
        0 gaps required to start Phase 0.
```

- [ ] **Step 4: Confirm 0 gaps before proceeding**

If spec-verifier reports gaps, amend this plan inline before any code change.

---

## Task 1: Add `~/.human/private/` to `.gitignore` and document private-data discipline

**Files:**
- Modify: `.gitignore`
- Modify: `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` (verify §13 references match)

- [ ] **Step 1: Add the private directory pattern**

Append to `.gitignore` (note: do NOT include `~/.human/private/` literally — git treats `~` as a literal character, not home-directory expansion, so it would not match. The `**/.human/private/` pattern is the git-effective form):

```
# Private corpus + derivatives (per spec §13)
.human/private/
**/.human/private/
```

- [ ] **Step 2: Verify nothing under `.human/private/` is currently tracked**

```bash
git ls-files | grep -E '\.human/private/' || echo "OK: nothing tracked"
```

Expected: `OK: nothing tracked`

- [ ] **Step 3: Commit**

```bash
git add .gitignore
git commit -m "chore(privacy): ignore ~/.human/private/ for RL corpus discipline (spec §13)"
```

---

## Task 2: Archive the May 11 2026 audit baseline

**Files:**
- Create: `docs/audits/2026-05-11-rl-loop-baseline-audit.md`

- [ ] **Step 1: Create the directory if missing**

```bash
mkdir -p docs/audits
```

- [ ] **Step 2: Write the audit document**

Write `docs/audits/2026-05-11-rl-loop-baseline-audit.md` with this content (the audit baseline this entire RL effort was designed against). Note the YAML frontmatter is required by `scripts/check-docs-frontmatter.sh` (which enforces `---` on line 1 of every `docs/*.md` outside `docs/plans/`):

````markdown
---
title: "RL & Neural Improvement Loop — Baseline Audit"
created: 2026-05-11
status: archived
scope: src/ml, src/agent, src/memory, src/eval, src/persona, src/providers, src/daemon
audit_method: 5-explorer concurrent review of committed main + Track D Phase 1 in-flight tree
authored_for: docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
---

# RL & Neural Improvement Loop — Baseline Audit, May 11 2026

**Status:** Historical record. This audit was the baseline against which `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` was authored.

**Audit method:** 5-explorer concurrent review of `src/ml/`, `src/agent/`, `src/memory/`, `src/eval/`, `src/persona/`, `src/providers/`, `src/daemon.c`, plus committed `main` baseline + Track D Phase 1 in-flight uncommitted tree.

## Findings — what was real (committed `main`)

- Hand-derived backward + Muon+AdamW optimizer in `src/ml/gpt.c`, `train.c`, `muon_adamw.c`. Tests do real finite-difference grad checks.
- Real LoRA primitives in `src/ml/lora.c` (low-rank A/B, apply, backward, save/load).
- Real preference-data layer in `src/ml/dpo.c` (SQLite `dpo_pairs`, JSONL export).
- Real hot-swap adapter loading at daemon startup and post-train (`src/daemon.c:2465-2532`, `src/agent/lora_training_runner.c:90-93`).
- Real eval harness with LLM-as-judge, bootstrap regression detection, nightly W16 bench (`src/eval/eval.c`, `evaluation.yml`).

## Findings — what Track D Phase 1 (in-flight) was shipping

- Personal Model v4 with symmetric signal aging + decay + goals lifecycle + daemon hourly tick.
- Offline persona-fidelity scorer (3-axis `hu_communication_style_fidelity_score`) + A/B comparator + `human ml lora-baseline` and `human ml lora-ab` CLIs + `scripts/check-lora-baseline.sh` CI gate.
- Banks-from-history SFT data pipeline (`hu_persona_banks_extract_from_history` + `--from-history` + `--persist`).
- M3 adapter seam: kill switch (`hu_m3_adapter_should_disable`), chat-time hook (`hu_agent_m3_on_provider_success` in 11 sites), centralized caveat strings.
- Persona JSON example_banks round-trip (`hu_persona_creator_write`).

## Findings — what was still broken (re-verified against in-flight tree)

| # | Issue | Location | Severity |
|---|---|---|---|
| 1 | `hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result)` — `vocab_size=0` and `token_bytes=NULL` | `src/ml/cli.c:190` (`hu_ml_cli_train`) | High — flagship subcommand silently no-ops |
| 2 | Same call shape | `src/ml/cli.c:2016` (`hu_ml_cli_train_feed_predictor`) | High |
| 3 | Same — `token_bytes=NULL` to `hu_ml_train` | `src/ml/experiment.c:300-302` (`run_single_experiment`) | High — defeats `val_bpb` |
| 4 | `hu_personal_model_save` is direct `fopen("wb")` with no temp file or rename | `src/memory/personal_model.c:1851` | High — crash during save corrupts state |
| 5 | `hu_dpo_train_step` is **not DPO** — calls external LLM, scores 0–100, aggregates synthetic loss. No policy log-probs, no reference model, no gradient on policy weights. | `src/ml/dpo.c` | High — name lies about behavior |
| 6 | `CLAUDE.md:53` claims `hu_personal_model_save` is "atomic-rename" save and that `hu_dpo_train_step` is DPO. Documentation drift. | `CLAUDE.md:53` | Medium — propagates the lie |
| 7 | `lora-persona` in-process backward passes raw logits where it should pass `softmax(logits) − one_hot(target)`, mathematically inconsistent with its documented NLL loss | `src/ml/cli.c` (lora-persona handler) | Medium — quietly trains the wrong objective; tracked in spec §1.5.2 / §2 background, fixed by spec Phase 2 |
| 8 | `m3_frontier_adapter.c` is a fixture file-format probe, not a frontier adapter | `src/ml/m3_frontier_adapter.c` | Low — caveats now centralized in Track D Phase 1; fixture nature unchanged but documented honestly |
| 9 | `llamacpp_chat_with_system` returns `HU_ERR_NOT_SUPPORTED` even when llama.cpp is linked | `src/providers/llamacpp.c:125-135` | High — fixed by spec Phase 1 |
| 10 | Channel reactions are outbound-only: inbound 👎 tapbacks/reactji do not reach `hu_dpo_record_from_feedback` | `src/channels/imessage_inbound.c`, `src/channels/slack_inbound.c` | Medium — fixed by spec Phase 2 |
| 11 | No reward model, no value head, no policy-gradient RL anywhere in the binary | binary-wide | High — added by spec Phases 2-4 |

## What this Phase 0 fixes (issues #1-6)

Issues 7-11 are deferred to subsequent phases per the spec. Phase 0 covers issues 1-6 only.

## What this Phase 0 explicitly does NOT touch

- Track D Phase 1 in-flight work (defer to its own track; this audit is a snapshot)
- Any new vtable surfaces (those land in Phases 2-5)
- Any provider integration (Phase 1)
- Any CI workflow additions (Phase 1+)
````

- [ ] **Step 3: Commit**

```bash
git add docs/audits/2026-05-11-rl-loop-baseline-audit.md
git commit -m "docs(audit): archive May 11 2026 RL-loop baseline audit

Snapshot of the 5-explorer audit baseline this entire RL effort
was designed against. Archived as historical record so subsequent
phases have a frozen reference for what was true on May 11 2026.

Refs spec §4.1, §1.5.2, §2."
```

---

## Task 3: Add `tests/test_ml_cli_actually_trains.c` — failing test for `vocab_size=0` bug

**Files:**
- Create: `tests/test_ml_cli_actually_trains.c`
- Modify: `tests/test_main.c` (declare + call `run_ml_cli_actually_trains_tests`)
- Modify: `CMakeLists.txt` (add the new test file to the test target)

- [ ] **Step 1: Write the failing test**

> **API verification (May 11 2026):** Reading `include/human/ml/ml.h`, `model.h`, `optimizer.h`, `dataloader.h`, `train.h` and the existing test patterns in `tests/test_ml.c` (`test_train_pipeline` lines 583-653, `test_grad_accumulation_runs` lines 870-911), the *real* C surface is:
>
> - `hu_allocator_t hu_system_allocator(void)` returns by value, not pointer
> - `hu_experiment_config_t hu_experiment_config_default(void)` (no `hu_ml_default_config`); fields are `cfg.gpt`, `cfg.optimizer`, `cfg.training`, `cfg.backend`
> - `hu_gpt_create(&alloc, &cfg.gpt, &model)` — second arg is `&cfg.gpt`, not `&cfg.model`
> - `hu_muon_adamw_create(&alloc, &cfg.optimizer, &opt)` — only 3 args, no `&model`
> - `hu_gpt_register_params(&model, &opt)` — required to wire params after creation
> - `hu_ml_dataloader_create(&alloc, dir, batch, seq_len, "train"|"val", &out)` — only loader factory, reads `shard_NNNNN.bin` files from `dir`. There is no in-memory loader; tests write tokens to `/tmp/...`.
> - `hu_ml_train(&alloc, &model, &opt, train_dl, val_dl, &cfg, const int32_t *token_bytes, size_t vocab_size, &result)` — `token_bytes` is `const int32_t*`, not `uint8_t*`.
> - `src/ml/train.c:46` early-returns `HU_ERR_INVALID_ARGUMENT` only when `token_bytes != NULL && vocab_size == 0`. With both NULL/0 the call returns `HU_OK`, the outer step loop runs, but every per-token grad iteration short-circuits at line 113 (`target >= vocab_size` is true for every target since vocab_size==0), and BPB is never computed (line 235 requires `token_bytes != NULL`). So `result.num_steps > 0` and `result.val_bpb == 0.0`.

Create `tests/test_ml_cli_actually_trains.c`:

```c
/* Phase 0 Task 3 — proves that hu_ml_train with vocab_size=0 + token_bytes=NULL
 * (the pre-fix call shape used in src/ml/cli.c:190 and src/ml/cli.c:2016)
 * silently no-ops: training reports OK and num_steps>0 but no per-token grad
 * is ever computed and val_bpb is never recorded. The patched cli paths must
 * pass a non-zero vocab_size and a non-NULL token_bytes table so the CE
 * objective actually runs and BPB is reported.
 *
 * See spec §1.5.2 issues #1, #2 and the May 11 2026 audit baseline. */

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/dataloader.h"
#include "human/ml/train.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HU_ENABLE_ML

/* Helpers (mirroring tests/test_ml.c style). */
static void mkdir_p_local(const char *path) {
#ifndef _WIN32
    mkdir(path, 0755);
#endif
}

static void write_bin_file_local(const char *path, const int32_t *tokens, size_t count) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(tokens, sizeof(int32_t), count, f);
        fclose(f);
    }
}

/* Build a tiny GPT + MuonAdamW + 2-shard dataloader pair backed by /tmp.
 * Returns ownership to the caller (must call cleanup_pipeline). */
typedef struct pipeline {
    hu_model_t model;
    hu_ml_optimizer_t opt;
    hu_ml_dataloader_t *train_dl;
    hu_ml_dataloader_t *val_dl;
    char dir[256];
    char path1[320];
    char path2[320];
} pipeline_t;

static void build_pipeline(hu_allocator_t *alloc, const char *dirname, pipeline_t *p) {
    snprintf(p->dir, sizeof(p->dir), "/tmp/%s", dirname);
    mkdir_p_local(p->dir);

    int32_t tokens[200];
    for (int i = 0; i < 200; i++)
        tokens[i] = i % 128;
    snprintf(p->path1, sizeof(p->path1), "%s/shard_00000.bin", p->dir);
    snprintf(p->path2, sizeof(p->path2), "%s/shard_00001.bin", p->dir);
    write_bin_file_local(p->path1, tokens, 200);
    write_bin_file_local(p->path2, tokens, 200);

    HU_ASSERT_EQ(hu_ml_dataloader_create(alloc, p->dir, 2, 8, "train", &p->train_dl), HU_OK);
    HU_ASSERT_EQ(hu_ml_dataloader_create(alloc, p->dir, 2, 8, "val", &p->val_dl), HU_OK);

    hu_gpt_config_t gpt_cfg = {0};
    gpt_cfg.sequence_len = 16;
    gpt_cfg.vocab_size = 128;
    gpt_cfg.n_layer = 1;
    gpt_cfg.n_head = 2;
    gpt_cfg.n_kv_head = 2;
    gpt_cfg.n_embd = 64;
    gpt_cfg.head_dim = 32;
    gpt_cfg.activation = HU_ML_ACT_RELU_SQ;

    memset(&p->model, 0, sizeof(p->model));
    HU_ASSERT_EQ(hu_gpt_create(alloc, &gpt_cfg, &p->model), HU_OK);

    hu_optimizer_config_t opt_cfg = hu_experiment_config_default().optimizer;
    memset(&p->opt, 0, sizeof(p->opt));
    HU_ASSERT_EQ(hu_muon_adamw_create(alloc, &opt_cfg, &p->opt), HU_OK);
    HU_ASSERT_EQ(hu_gpt_register_params(&p->model, &p->opt), HU_OK);
}

static void cleanup_pipeline(hu_allocator_t *alloc, pipeline_t *p) {
    p->opt.vtable->deinit(p->opt.ctx, alloc);
    p->model.vtable->deinit(p->model.ctx, alloc);
    hu_ml_dataloader_deinit(p->val_dl);
    hu_ml_dataloader_deinit(p->train_dl);
    remove(p->path1);
    remove(p->path2);
    rmdir(p->dir);
}

/* The bug. With token_bytes=NULL and vocab_size=0 (the call shape in cli.c:190
 * and cli.c:2016 today), hu_ml_train returns HU_OK, num_steps>0 — but BPB is
 * never recorded (val_bpb stays 0.0) and the per-token CE branch is never
 * entered (every target>=vocab_size==0 is true so the loop short-circuits at
 * src/ml/train.c:113). The training is a silent no-op. */
static void test_ml_cli_train_with_zero_vocab_does_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pipeline_t p;
    build_pipeline(&alloc, "test_ml_cli_zero_vocab", &p);

    hu_training_config_t train_cfg = {0};
    train_cfg.device_batch_size = 2;
    train_cfg.time_budget_secs = 1;
    train_cfg.eval_tokens = 32;

    hu_ml_train_result_t result_buggy = {0};
    hu_error_t err = hu_ml_train(&alloc, &p.model, &p.opt, p.train_dl, p.val_dl, &train_cfg,
                                 /*token_bytes=*/NULL, /*vocab_size=*/0, &result_buggy);

    HU_ASSERT_EQ(err, HU_OK);
    /* Outer loop runs (num_steps>0) but BPB is never computed. Lock that down. */
    HU_ASSERT_GT(result_buggy.num_steps, 0);
    HU_ASSERT_EQ(result_buggy.val_bpb, 0.0);

    cleanup_pipeline(&alloc, &p);
}

/* The fix shape. With a real vocab_size and an int32_t[vocab] token_bytes
 * lookup, the CE branch is entered (per-token grad is computed) and BPB is
 * recorded by hu_ml_evaluate_bpb at the end of the run. */
static void test_ml_cli_train_with_real_vocab_actually_trains(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pipeline_t p;
    build_pipeline(&alloc, "test_ml_cli_real_vocab", &p);

    hu_training_config_t train_cfg = {0};
    train_cfg.device_batch_size = 2;
    train_cfg.time_budget_secs = 1;
    train_cfg.eval_tokens = 32;

    /* int32_t per spec — token_bytes[t] = #bytes that token t encodes.
     * For a synthetic byte-vocab of 128, every token is 1 byte. */
    int32_t token_bytes[128];
    for (int i = 0; i < 128; i++)
        token_bytes[i] = 1;

    hu_ml_train_result_t result = {0};
    hu_error_t err = hu_ml_train(&alloc, &p.model, &p.opt, p.train_dl, p.val_dl, &train_cfg,
                                 token_bytes, 128, &result);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GT(result.num_steps, 0);
    HU_ASSERT_GT(result.total_tokens, 0);
    /* BPB is in bits-per-byte, finite and strictly positive when CE actually
     * ran. This is the assertion the bug shape cannot satisfy. */
    HU_ASSERT_GT(result.val_bpb, 0.0);

    cleanup_pipeline(&alloc, &p);
}

#endif /* HU_ENABLE_ML */

void run_ml_cli_actually_trains_tests(void) {
    HU_TEST_SUITE("ml-cli-actually-trains");
#ifdef HU_ENABLE_ML
    HU_RUN_TEST(test_ml_cli_train_with_zero_vocab_does_nothing);
    HU_RUN_TEST(test_ml_cli_train_with_real_vocab_actually_trains);
#endif
}
```

- [ ] **Step 2: Wire into `tests/test_main.c`**

Add the declaration alongside other `void run_*_tests(void);` declarations near the top, and the call in the dispatch block (search for an existing test like `run_personal_model_tests` and add adjacent):

```c
void run_ml_cli_actually_trains_tests(void);
```

And in the dispatch:

```c
run_ml_cli_actually_trains_tests();
```

- [ ] **Step 3: Add to `CMakeLists.txt`**

Find line ~2653 (`tests/test_ml.c` entry in the test sources block) and add:

```cmake
    tests/test_ml_cli_actually_trains.c
```

- [ ] **Step 4: Build and run — confirm the test characterizes both shapes**

```bash
cmake --preset dev
cmake --build --preset dev -j
./build/human_tests --suite=ml-cli-actually-trains
```

**Expected (May 11 2026 verified):** Both tests pass. This is intentional and reflects what was observed when the test was first run:

- The bug shape (`vocab_size=0, token_bytes=NULL`) does NOT return `HU_OK` and silently no-op as the audit baseline initially claimed. Instead, `hu_ml_train` propagates the zero-shape grad tensor down to `gpt_backward` (`src/ml/gpt.c:567`), which returns `HU_ERR_INVALID_ARGUMENT` on the very first batch. The CLI then prints `"Training failed: 0 steps, 0.00 bpb"` with no useful diagnostic. Either failure mode (silent no-op OR hard fail with no diagnostic) is a bug; the test asserts the OR of both.
- The fix shape (`vocab_size=128`, real `token_bytes`) returns `HU_OK` with `val_bpb > 0`.

The test pins the behavioral contract that Task 4's `hu_ml_cli_train` and Task 5's `experiment.c` patches must satisfy — they MUST pass the fix shape, not the bug shape. End-to-end verification that the CLI itself (`hu_ml_cli_train`) reaches the fix shape is covered by the smoke test in Task 4 Step 3.

- [ ] **Step 5: Commit (test pins the behavioral contract)**

```bash
git add tests/test_ml_cli_actually_trains.c tests/test_main.c CMakeLists.txt \
        docs/plans/2026-05-11-rl-loop-phase-0-honesty.md
git commit -m "test(ml): pin hu_ml_train behavior for cli vocab_size=0 bug

Adds tests/test_ml_cli_actually_trains.c which characterizes both call
shapes for hu_ml_train:

- Bug shape (token_bytes=NULL, vocab_size=0, the call shape used today
  by hu_ml_cli_train at src/ml/cli.c:190 and hu_ml_cli_train_feed_predictor
  at src/ml/cli.c:2016): observed to hard-fail with HU_ERR_INVALID_ARGUMENT
  because gpt_backward (src/ml/gpt.c:567) rejects the zero-shape grad
  tensor on the first batch. The CLI then prints \"Training failed: 0 steps,
  0.00 bpb\" with no useful diagnostic. Audit baseline initially called this
  a \"silent no-op\" — actually a hard fail with uninformative reporting.

- Fix shape (token_bytes=identity, vocab_size=128): returns HU_OK with
  positive val_bpb, num_steps>0, total_tokens>0.

The test passes from the start because it tests hu_ml_train directly with
both shapes. It pins the behavioral contract that Task 4's cli.c patches
must satisfy: pass the fix shape, not the bug shape. End-to-end
verification of the CLI itself lands in Task 4's smoke test.

Refs spec §1.5.2 issues #1, #2; audit baseline
docs/audits/2026-05-11-rl-loop-baseline-audit.md."
```

---

## Task 4: Fix `hu_ml_cli_train` (cli.c:190) and `hu_ml_cli_train_feed_predictor` (cli.c:2495)

**Files:**
- Modify: `src/ml/cli.c` lines ~85-201 (`hu_ml_cli_train`)
- Modify: `src/ml/cli.c` lines ~2267-2510 (`hu_ml_cli_train_feed_predictor`)

> **Reality check (May 11 2026):**
>
> - `hu_ml_cli_train` (cli.c:85) does NOT have a tokenizer in scope. The original plan assumed it did. The fix must load (or create) a tokenizer.
> - `hu_ml_cli_train_feed_predictor` (cli.c:2267) DOES have a tokenizer in scope (created at cli.c:2338).
> - The `hu_ml_train` C signature is `(..., const int32_t *token_bytes, size_t vocab_size, ...)` — `int32_t*`, NOT `uint8_t*`. No cast needed.
> - `hu_experiment_config_t` fields are `cfg.gpt`, `cfg.training`, `cfg.optimizer`, `cfg.backend` — there is NO `cfg.model`.
> - Tokenizer convention in the codebase (per `src/daemon.c:3311-3312`) is `~/.human/models/tokenizer.vocab`. We extend with a `data_dir/tokenizer.vocab` first-look.
> - `hu_ml_prepare_token_bytes` is already public in `include/human/ml/prepare.h:20`. No header change needed.

- [ ] **Step 1: Add a static helper for tokenizer load + token_bytes derivation**

In `src/ml/cli.c`, near the top of the file (after the existing static helpers), add:

```c
/* Phase 0 helper — load a BPE tokenizer using the project convention
 * (data_dir/tokenizer.vocab → ~/.human/models/tokenizer.vocab → default
 * 256-byte byte-level BPE) and derive the token_bytes table for BPB.
 *
 * On success, *out_tok and *out_token_bytes are owned by the caller and
 * must be freed with hu_bpe_tokenizer_deinit and alloc->free. *out_count
 * is the tokenizer's vocab_size — callers should align cfg.gpt.vocab_size
 * to this value before creating the GPT model so the model's vocab matches
 * the tokenizer's. */
static hu_error_t derive_token_bytes_for_data_dir(
    hu_allocator_t *alloc, const char *data_dir,
    hu_bpe_tokenizer_t **out_tok, int32_t **out_token_bytes, size_t *out_count) {
    if (!alloc || !out_tok || !out_token_bytes || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_tok = NULL;
    *out_token_bytes = NULL;
    *out_count = 0;

    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK)
        return err;

    char path[1024];
    int loaded = 0;
    if (data_dir && data_dir[0]) {
        int n = snprintf(path, sizeof(path), "%s/tokenizer.vocab", data_dir);
        if (n > 0 && (size_t)n < sizeof(path) &&
            hu_bpe_tokenizer_load(tok, path) == HU_OK) {
            loaded = 1;
        }
    }
    if (!loaded) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            int n = snprintf(path, sizeof(path),
                             "%s/.human/models/tokenizer.vocab", home);
            if (n > 0 && (size_t)n < sizeof(path))
                (void)hu_bpe_tokenizer_load(tok, path);
        }
        /* On both failures, tok keeps its default 256-byte byte-level vocab
         * — every token is one byte, BPB is well-defined. */
    }

    int32_t *token_bytes = NULL;
    size_t count = 0;
    err = hu_ml_prepare_token_bytes(alloc, tok, &token_bytes, &count);
    if (err != HU_OK) {
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    *out_tok = tok;
    *out_token_bytes = token_bytes;
    *out_count = count;
    return HU_OK;
}
```

- [ ] **Step 2: Fix `hu_ml_cli_train` (cli.c:140-200)**

Add the tokenizer load BETWEEN the `hu_experiment_config_default()` call (line 140) and the `hu_gpt_create` call (line 159), so cfg.gpt.vocab_size can be aligned:

```c
    /* Phase 0 fix — load tokenizer first so we can align cfg.gpt.vocab_size
     * to the tokenizer's actual vocab and pass real token_bytes to hu_ml_train. */
    hu_bpe_tokenizer_t *tok = NULL;
    int32_t *token_bytes = NULL;
    size_t token_bytes_count = 0;
    err = derive_token_bytes_for_data_dir(alloc, data_dir, &tok, &token_bytes,
                                          &token_bytes_count);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "tokenizer load failed: %d", err);
        return err;
    }
    cfg.gpt.vocab_size = token_bytes_count;
```

(Note: `err` is declared later in the original code at line 159; it must be moved up to the top of the function or the new block must declare its own `hu_error_t err`.)

Then change line 191 (`hu_ml_train` call) from:

```c
    err =
        hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result);
```

to:

```c
    err = hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training,
                      token_bytes, token_bytes_count, &result);
```

And add cleanup before the existing cleanup block:

```c
    alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
```

(If any earlier cleanup path can be reached after the tokenizer load, the cleanup must happen there too — be careful with the existing error-path returns at lines 161-184.)

- [ ] **Step 3: Fix `hu_ml_cli_train_feed_predictor` (cli.c:2495)**

This handler ALREADY creates a tokenizer at line 2338-2344. Reuse it directly:

After the `tok` is created (line 2344) and BEFORE `hu_gpt_create` (line 2468), add:

```c
    /* Phase 0 fix — derive token_bytes from the in-scope tokenizer and align
     * cfg.gpt.vocab_size so the model matches the tokenizer's actual vocab. */
    int32_t *token_bytes = NULL;
    size_t token_bytes_count = 0;
    err = hu_ml_prepare_token_bytes(alloc, tok, &token_bytes, &token_bytes_count);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return err;
    }
    cfg.gpt.vocab_size = token_bytes_count;
```

Then change line 2495-2496 from:

```c
    err =
        hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result);
```

to:

```c
    err = hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training,
                      token_bytes, token_bytes_count, &result);
```

And add cleanup of `token_bytes` before the existing cleanup block at line 2502+:

```c
    alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
```

- [ ] **Step 4: Build and run — confirm pinning test still passes (the contract is unchanged)**

```bash
cmake --build --preset dev -j
./build/human_tests --suite=ml-cli-actually-trains
```

Expected: 2/2 pass. The test characterizes hu_ml_train's contract; the cli.c fix changes only the call shape, not the contract.

- [ ] **Step 5: End-to-end smoke test for the CLI itself**

> **Plan amendment (May 11 2026 — discovered while running this step):**
>
> Two additional changes to cli.c are required for the smoke test to actually show a positive BPB:
>
> 1. **Pass a val_loader to `hu_ml_train`.** The original code at cli.c:191 passed `NULL` as val_loader. Even with token_bytes provided, `hu_ml_train` only computes BPB when `val_loader && token_bytes && config->eval_tokens > 0` (train.c:235). The fix creates a `val_loader = hu_ml_dataloader_create(..., "val", ...)` and passes it.
> 2. **Extend the JSON config parser to accept GPT model dim overrides AND `eval_tokens`.** The defaults (`hu_experiment_config_default()`) produce an 8-layer 512-embd model with `eval_tokens = 20971520` (20M) — too heavy for any meaningful smoke test. The plan now exposes `n_layer`, `n_head`, `n_kv_head`, `n_embd`, `head_dim`, `sequence_len`, and `eval_tokens` as JSON config keys.
>
> These are scope-adjacent to Task 4's "make the CLI actually train" intent. They are the minimum required to demonstrate the fix end-to-end.

Build a tiny config + data dir and invoke the CLI binary:

```bash
TMP=$(mktemp -d)
mkdir -p "$TMP/data"
# Write 200 random byte tokens as two shards (matches dataloader convention)
python3 -c "
import struct, random
random.seed(0)
for shard in (0, 1):
    with open(f'$TMP/data/shard_{shard:05d}.bin', 'wb') as f:
        for _ in range(200):
            f.write(struct.pack('<i', random.randint(0, 127)))
"

cat > "$TMP/cfg.json" <<EOF
{
  "data_dir": "$TMP/data",
  "batch_size": 2,
  "max_steps": 8,
  "time_budget_secs": 10,
  "eval_tokens": 32,
  "n_layer": 1,
  "n_head": 2,
  "n_kv_head": 2,
  "n_embd": 64,
  "head_dim": 32,
  "sequence_len": 16
}
EOF

./build/human ml train --config "$TMP/cfg.json" 2>&1 | tee "$TMP/output.txt"

# Assert the CLI no longer prints "Training failed" or "0.00 bpb".
if grep -qE "Training failed|0\.00 bpb" "$TMP/output.txt"; then
  echo "REGRESSION: CLI still reports failure or zero BPB"
  exit 1
fi
echo "Smoke test PASS: CLI training reports a real BPB."

rm -rf "$TMP"
```

**Expected output (May 11 2026 verified):**
```
Training: batch_size=2, max_steps=8, vocab_size=256, data=/tmp/.../data
Training complete: 8 steps, 8.00 bpb, 0.0s
Smoke test PASS: CLI training reports a real BPB.
```

(`8.00 bpb` is `log2(256)` — the BPB of an untrained byte-level model evaluated against random data. After actual learning, this would drop. The point is that BPB is now non-zero, proving CE evaluation actually ran.)

(If `python3` is unavailable, write the same shard files in a small C oneshot or `dd if=/dev/urandom bs=4 count=200`.)

- [ ] **Step 6: Run full ML suite to catch regressions**

```bash
./build/human_tests --suite=ml
```

Expected: 0 failures.

- [ ] **Step 7: Commit**

```bash
git add src/ml/cli.c docs/plans/2026-05-11-rl-loop-phase-0-honesty.md
git commit -m "$(cat <<'EOF'
fix(ml): supply real vocab_size + token_bytes to hu_ml_train CLI paths

hu_ml_cli_train (cli.c:190) and hu_ml_cli_train_feed_predictor (cli.c:2495)
both passed NULL token_bytes + 0 vocab_size, defeating the CE objective
and causing hu_ml_train to hard-fail at gpt_backward (src/ml/gpt.c:567)
on the first batch. The CLI then printed "Training failed: 0 steps,
0.00 bpb" with no useful diagnostic.

Adds a static helper derive_token_bytes_for_data_dir() in cli.c that
loads a BPE tokenizer using the project convention
(data_dir/tokenizer.vocab → ~/.human/models/tokenizer.vocab → default
256-byte byte-level BPE) and derives the token_bytes lookup. Both CLI
handlers now align cfg.gpt.vocab_size to the tokenizer's actual vocab
and pass real token_bytes to hu_ml_train. The feed-predictor handler
reuses its existing in-scope tokenizer.

Plan amendments:
- Original plan referenced cfg.model.vocab_size (no such field — actually
  cfg.gpt.vocab_size).
- Original plan cast token_bytes to (uint8_t*) (signature is const int32_t*).
- Original plan claimed both CLI handlers had a tokenizer in scope (only
  the feed-predictor does; cli_train needed the load helper).

Refs spec §1.5.2 issues #1, #2; pinned by tests/test_ml_cli_actually_trains.c.
EOF
)"
```

---

## Task 5: Fix `experiment.c:300-302` `token_bytes=NULL`

**Files:**
- Modify: `src/ml/experiment.c` lines ~280-321 (`run_single_experiment`)
- Modify: `tests/test_ml_cli_actually_trains.c` (add a test for the experiment path)
- Modify: `include/human/ml/prepare.h` and `src/ml/prepare.c` (extract shared tokenizer loader; the second caller is enough to justify lifting it out of cli.c's static helper while still under the rule of three)

- [x] **Step 1: Read context**

The bug as it stands at HEAD (`16c62b8b`):

```c
/* src/ml/experiment.c, run_single_experiment */
hu_ml_train_result_t train_result = {0};
err = hu_ml_train(alloc, &model, &optimizer, train_dl, val_dl, &cfg->training, NULL,
                  cfg->gpt.vocab_size, &train_result);
```

`token_bytes=NULL` defeats the CE objective. The 8th arg (`vocab_size`) is correctly threaded through (`cfg->gpt.vocab_size`) but `hu_ml_train`'s contract is "either both or neither" — without `token_bytes` it cannot compute per-token loss and `val_bpb` stays 0.0, making the experiment loop's keep/discard comparison effectively random.

- [x] **Step 2: Add a failing test for the experiment path**

> **Plan amendment (May 11 2026):** the original plan referenced `hu_experiment_run_single` and `hu_ml_default_config`, neither of which exist. The actual public surface for running a single experiment is `hu_experiment_loop` with `max_iterations=1`. The test wires a callback to capture the result rather than calling a "single" entry point that isn't there.

Appended to `tests/test_ml_cli_actually_trains.c`:

```c
static double g_experiment_callback_bpb = -1.0;
static hu_experiment_status_t g_experiment_callback_status = HU_EXPERIMENT_CRASH;

static void capture_experiment_result(const hu_experiment_result_t *r, void *user_data) {
    (void)user_data;
    g_experiment_callback_bpb = r->val_bpb;
    g_experiment_callback_status = r->status;
}

static void test_experiment_loop_passes_token_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pipeline_t p;
    build_pipeline(&alloc, "test_experiment_token_bytes", &p);

    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = 1;
    loop_cfg.data_dir = p.dir;
    loop_cfg.base_config = hu_experiment_config_default();
    /* Tiny CPU-friendly model that matches the byte-level shards. */
    loop_cfg.base_config.gpt.sequence_len = 16;
    loop_cfg.base_config.gpt.vocab_size = 256;
    loop_cfg.base_config.gpt.n_layer = 1;
    loop_cfg.base_config.gpt.n_head = 2;
    loop_cfg.base_config.gpt.n_kv_head = 2;
    loop_cfg.base_config.gpt.n_embd = 64;
    loop_cfg.base_config.gpt.head_dim = 32;
    loop_cfg.base_config.training.device_batch_size = 2;
    loop_cfg.base_config.training.max_steps = 8;
    loop_cfg.base_config.training.time_budget_secs = 10;
    loop_cfg.base_config.training.eval_tokens = 32;

    HU_ASSERT_EQ(hu_experiment_loop(&alloc, &loop_cfg, capture_experiment_result, NULL), HU_OK);
    HU_ASSERT(g_experiment_callback_bpb > 0.0);
    HU_ASSERT(g_experiment_callback_status != HU_EXPERIMENT_CRASH);

    cleanup_pipeline(&alloc, &p);
}
```

And `HU_RUN_TEST(test_experiment_loop_passes_token_bytes);` is added to `run_ml_cli_actually_trains_tests`.

- [x] **Step 3: Run, confirm fail**

```
$ ./build/human_tests --filter=test_experiment_loop_passes_token_bytes
=== ml-cli-actually-trains ===
  FAIL  (tests/test_ml_cli_actually_trains.c:223) assert failed: g_experiment_callback_bpb > 0.0
--- Results: 15/16 passed, 1 FAILED, 10062 skipped ---
```

The pre-fix bug shape: callback fires with `val_bpb == 0.0` because `hu_ml_train` hard-fails immediately (the existing `result->status = HU_EXPERIMENT_CRASH` already records this; the test pins the *symptom*, not the underlying error code, because the spec's contract is "BPB must be meaningful," not "the call must succeed in some particular way").

- [x] **Step 4: Fix `experiment.c` and extract the shared tokenizer loader**

Done in two parts:

1. Extract `hu_ml_prepare_load_default_tokenizer` to `prepare.h`/`prepare.c` so both Task 4's `cli.c` helper and Task 5's `experiment.c` fix can share the convention (`data_dir/tokenizer.vocab` → `~/.human/models/tokenizer.vocab` → default 256-byte byte-level BPE). This is the second caller; the rule of three says we extract on the third, but a public convention shared by both the CLI and the autonomous loop is exactly the kind of thing that should not have a static-in-cli.c implementation. (Cleanup of `cli.c::derive_token_bytes_for_data_dir` to call this helper is left for a separate small commit so this Task 5 commit stays surgical.)

2. In `run_single_experiment`, after `hu_ml_checkpoint_load`, build a `token_bytes` table:
   - Try the shared loader. If the loaded tokenizer's vocab matches `cfg->gpt.vocab_size`, use the derived per-token byte lengths.
   - Otherwise (vocab mismatch or tokenizer load error), fall back to a 1-byte-per-token table sized to the model so `hu_ml_train`'s bounds checks never trip and BPB stays well-defined for byte-level vocabs.
3. Pass `token_bytes` + `token_bytes_count` to `hu_ml_train` instead of `NULL` + `cfg->gpt.vocab_size`.
4. Free both at every return path.

> **Why fall back instead of failing loud on vocab mismatch?** The autonomous loop is allowed to mutate `cfg->gpt` between iterations, and the keep/discard decision still wants a BPB number. A 1-byte-per-token fallback gives a meaningful (if slightly biased for non-byte vocabs) signal. A loud failure here would silently disable the entire experiment path until someone wired a proper tokenizer through `hu_experiment_loop_config_t` — out of scope for Phase 0. Future work tracked at the bottom of this task.

- [x] **Step 5: Run, confirm pass**

```
$ ./build/human_tests --filter=test_experiment_loop_passes_token_bytes
  PASS  test_experiment_loop_passes_token_bytes
--- Results: 16/16 passed, 10062 skipped ---

$ ./build/human_tests --suite=ml-cli-actually-trains
=== ml-cli-actually-trains ===
  PASS  test_ml_cli_train_with_zero_vocab_does_nothing
  PASS  test_ml_cli_train_with_real_vocab_actually_trains
  PASS  test_experiment_loop_passes_token_bytes
--- Results: 3/3 passed, 10060 skipped ---

$ ./build/human_tests --filter=token_bytes
  PASS  test_prepare_token_bytes      # existing prepare.c API still works
  PASS  test_experiment_loop_passes_token_bytes
--- Results: 17/17 passed, 10061 skipped ---
```

- [x] **Step 6: Commit**

```bash
git add src/ml/experiment.c src/ml/prepare.c include/human/ml/prepare.h \
        tests/test_ml_cli_actually_trains.c \
        docs/plans/2026-05-11-rl-loop-phase-0-honesty.md
```

Commit message captures the extraction + fallback decision; see the actual commit on `feat/sota-m1-infra`.

**Future work (out of scope for Phase 0):**
- Thread an explicit `tokenizer_path` field through `hu_experiment_loop_config_t` so non-byte-level experiments don't fall back to the 1-byte heuristic.
- Refactor `cli.c::derive_token_bytes_for_data_dir` to call `hu_ml_prepare_load_default_tokenizer` (deletes ~40 lines from cli.c).

> **Phase 0 follow-up (May 11 2026, found by Task 11 end-gate full test sweep):** This Task 5 fix surfaced a latent test-design bug in `tests/test_ml.c`. Three existing experiment-loop tests (`test_experiment_loop_runs`, `test_experiment_loop_keep_discard`, `test_experiment_loop_convergence`) call `hu_experiment_config_default()` and then override only a handful of fields — but NOT `eval_tokens`, which defaults to `20971520` (20M tokens). Pre-Phase-0, `token_bytes=NULL` short-circuited the BPB eval loop in `hu_ml_train` so the 20M default was harmless. Post-Phase-0 the eval loop actually runs, and on a 400-token shard the dataloader cycles forever trying to evaluate 20M tokens. `test_experiment_loop_convergence` is unaffected because it points at a nonexistent data dir (training crashes before the eval loop is ever reached); `test_experiment_loop_runs` and `_keep_discard` were both fixed by adding `loop_cfg.base_config.training.eval_tokens = 64;` with a comment explaining the why and pointing back at this task. The fix landed in a separate `test(ml): cap eval_tokens in experiment-loop tests` commit so the regression-fix story is bisectable and the `_runs` / `_keep_discard` line-number deltas stay isolated from Task 5's source-code commit.

---

## Task 6: Add `tests/test_personal_model_atomic_save.c` — failing test for non-atomic save

**Files:**
- Create: `tests/test_personal_model_atomic_save.c`
- Modify: `tests/test_main.c` (declare + call `run_personal_model_atomic_save_tests`)
- Modify: `CMakeLists.txt` (add to test target)

> **Plan amendment (May 11 2026):** the original test design used `fork()` + `raise(SIGKILL)` after `hu_personal_model_save` returned. That doesn't actually test crash atomicity — by the time `raise(SIGKILL)` runs, save has already returned and the file at `<path>` is the fully-written new file regardless of whether the implementation is atomic. The redesigned test below uses a deterministic FIX-SHAPE PROBE: pre-create `<path>.tmp` as a directory before calling save. The atomic implementation must `fopen("wb")` on `<path>.tmp`, which fails with `EISDIR` and leaves `<path>` untouched; the non-atomic implementation calls `fopen(<path>, "wb")` directly and truncates `<path>` before any error is detected. Same contract being tested ("if writing the new state fails for any reason, the prior state is preserved"), but deterministic and CI-stable.

- [x] **Step 1: Write the failing test**

Created `tests/test_personal_model_atomic_save.c`:

```c
/* Phase 0 Task 6 — proves that hu_personal_model_save uses an atomic
 * tmp+rename pattern, so a crash mid-save can never leave the on-disk
 * file in a partially-written state. */

#include "test_framework.h"
#include "human/memory/personal_model.h"
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

static void test_personal_model_save_preserves_prior_state_when_tmp_blocked(void) {
    char tmpl[] = "/tmp/hu_pm_atomic_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);

    char path[256], tmp_path[260];
    snprintf(path, sizeof(path), "%s/personal_model.bin", dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    /* Step 1 — known-good model with fact_count=1 and a recognizable subject. */
    hu_personal_model_t known_good;
    hu_personal_model_init(&known_good);
    known_good.fact_count = 1;
    known_good.facts[0].confidence = 1.0f;
    snprintf(known_good.facts[0].subject, sizeof(known_good.facts[0].subject),
             "known-good-state");
    HU_ASSERT_EQ(hu_personal_model_save(&known_good, path), HU_OK);

    /* Step 2 — block the atomic tmp slot by creating a directory there.
     * fopen("wb") on a directory always fails with EISDIR (POSIX). */
    HU_ASSERT_EQ(mkdir(tmp_path, 0755), 0);

    /* Step 3 — try to overwrite with HU_PM_MAX_FACTS facts. Atomic save
     * fails fopen(<path>.tmp, "wb"), returns IO error, leaves <path>
     * untouched. Non-atomic save calls fopen(<path>, "wb") directly
     * and truncates <path> immediately. */
    hu_personal_model_t big;
    hu_personal_model_init(&big);
    big.fact_count = HU_PM_MAX_FACTS;
    for (size_t i = 0; i < big.fact_count; i++) {
        big.facts[i].confidence = 1.0f;
        snprintf(big.facts[i].subject, sizeof(big.facts[i].subject), "fact-%zu", i);
    }
    (void)hu_personal_model_save(&big, path);

    /* Step 4 — load <path>: must still be the known-good. */
    hu_personal_model_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);
    HU_ASSERT_EQ(loaded.fact_count, (size_t)1);
    HU_ASSERT(strcmp(loaded.facts[0].subject, "known-good-state") == 0);

    (void)rmdir(tmp_path);
    (void)unlink(path);
    (void)rmdir(dir);
}

void run_personal_model_atomic_save_tests(void) {
    HU_TEST_SUITE("personal-model-atomic-save");
    HU_RUN_TEST(test_personal_model_save_preserves_prior_state_when_tmp_blocked);
}
```

- [x] **Step 2: Wire into `tests/test_main.c` and `CMakeLists.txt`**

Added `void run_personal_model_atomic_save_tests(void);` declaration and call. Added `tests/test_personal_model_atomic_save.c` to `HU_TEST_SOURCES` next to `tests/test_personal_model.c`.

- [x] **Step 3: Build and run — confirm FAIL**

```
$ ./build/human_tests --suite=personal-model-atomic-save
=== personal-model-atomic-save ===
  FAIL  (tests/test_personal_model_atomic_save.c:92) expected 64 == 1 (loaded.fact_count == (size_t)1)
--- Results: 0/1 passed, 1 FAILED, 10063 skipped ---
```

The pre-fix `hu_personal_model_save` opens `<path>` directly via `fopen("wb")`, truncates the prior known-good (fact_count=1), and writes the big model (fact_count=64). The directory blocker on `<path>.tmp` doesn't trip the bug because the bug doesn't use `<path>.tmp`. After Task 7's fix the test passes.

- [x] **Step 4: Commit (failing test)**

```bash
git add tests/test_personal_model_atomic_save.c tests/test_main.c CMakeLists.txt \
        docs/plans/2026-05-11-rl-loop-phase-0-honesty.md
```

The Task 6 commit lands a deliberately failing test pinning the contract Task 7 will satisfy. CI red-flag is acceptable because Task 7 follows in the next commit.

---

## Task 7: Implement atomic save for `hu_personal_model_save`

**Files:**
- Modify: `src/memory/personal_model.c` lines ~1934-1955 (`hu_personal_model_save` implementation)

- [x] **Step 1: Read the current implementation**

```bash
sed -n '1934,1955p' src/memory/personal_model.c
```

- [x] **Step 2: Replace with atomic-rename pattern**

Replace the function body with:

```c
hu_error_t hu_personal_model_save(const hu_personal_model_t *model, const char *path) {
    if (!model || !path || !*path) return HU_ERR_INVALID_ARGUMENT;
    hu_pm_ensure_parent_dir(path);

    /* Write to <path>.tmp then atomically rename. Crash safety:
     *   - If we crash before fclose: the .tmp file is partial,
     *     <path> is untouched, load returns the prior state.
     *   - If we crash after rename: the new file is intact.
     *   - There is no in-between window because rename(2) is
     *     atomic on POSIX with respect to the destination path. */
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(tmp, "wb");
    if (!fp) return HU_ERR_IO;

    hu_pm_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = HU_PM_MAGIC;
    hdr.version = HU_PM_VERSION;
    hdr.reserved = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 ||
        fwrite(model, sizeof(*model), 1, fp) != 1) {
        fclose(fp);
        unlink(tmp);
        return HU_ERR_IO;
    }

    /* Flush stdio buffer, then fsync the file descriptor so the
     * data is on disk before we rename. Without fsync, an OS
     * crash between rename and writeback can leave the renamed
     * file with stale or zero contents. */
    if (fflush(fp) != 0) {
        fclose(fp);
        unlink(tmp);
        return HU_ERR_IO;
    }
    int fd = fileno(fp);
    if (fd >= 0) {
        if (fsync(fd) != 0) {
            fclose(fp);
            unlink(tmp);
            return HU_ERR_IO;
        }
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        return HU_ERR_IO;
    }

    /* Atomic on POSIX: either <path> is the old file or the new file. */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}
```

- [x] **Step 3: Build and run — confirm test passes**

```
$ ./build/human_tests --suite=personal-model-atomic-save
=== personal-model-atomic-save ===
  PASS  test_personal_model_save_preserves_prior_state_when_tmp_blocked
--- Results: 1/1 passed, 10063 skipped ---
```

- [x] **Step 4: Run full personal-model suite to catch regressions**

```
$ ./build/human_tests --suite=PersonalModel
... (170 tests including the existing personal_model_survives_real_sigkill) ...
--- Results: 170/170 passed, 9894 skipped ---
```

Zero regressions; the existing `personal_model_survives_real_sigkill` test (a real fork+SIGKILL exerciser written elsewhere in the suite) also still passes — confirming the atomic-rename fix doesn't break crash recovery in either direction.

- [x] **Step 5: Run ASan check**

```
$ ./build/human_tests --suite=PersonalModel --suite=personal-model-atomic-save 2>&1 \
    | grep -iE "ERROR|leak|asan" || echo "ASan clean"
ASan clean
```

- [x] **Step 6: Critic review (inline)**

The reviewed cases (against the original plan's checklist):
- *Missing error returns*: every fopen/fwrite/fflush/fsync/fclose/rename path returns `HU_ERR_IO` and unlinks `<path>.tmp` on failure — no leak.
- *Path containing no slash*: `snprintf("%s.tmp", path)` works for any non-empty path; if path is `"foo"`, tmp is `"foo.tmp"` in the cwd, valid.
- *`path > sizeof(tmp)-5`*: guarded — `snprintf` returns `n >= sizeof(tmp)` when truncated, and we return `HU_ERR_INVALID_ARGUMENT` before any I/O. (Pre-existing API doesn't enforce a path length cap; 1 KiB matches the rest of the file.)
- *`fsync` failing on tmpfs / non-fsync-capable filesystems*: `fileno(fp) >= 0` check prevents calling fsync on a stream backed by something exotic; on tmpfs `fsync` succeeds and is a no-op.
- *`rename` across filesystems*: both paths are siblings in the same directory, so this can't trigger EXDEV.
- *ASan-detectable issues*: stack-only buffers, no heap allocations introduced — clean.

- [x] **Step 7: Commit**

```bash
git add src/memory/personal_model.c docs/plans/2026-05-11-rl-loop-phase-0-honesty.md
```

Commit message references the deterministic test pinned in Task 6 (`b3d11ca7`).

---

## Task 8: Add `tests/test_dpo_judge_naming.c` — failing test for misleading name

**Files:**
- Create: `tests/test_dpo_judge_naming.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

> **Plan amendment (May 11 2026):** the original test stub used a 5-arg signature `hu_dpo_train_step(alloc, pairs, n_pairs, judge_provider, out_result)` that doesn't match the actual API. The real signature in `include/human/ml/dpo.h` takes a `hu_dpo_collector_t *`, allocator, provider, model name, beta, batch_size — eight arguments. The test below uses the real signature and asserts both names reject NULLs uniformly with `HU_ERR_INVALID_ARGUMENT`.

- [x] **Step 1: Write the failing test**

Created `tests/test_dpo_judge_naming.c`:

```c
/* Phase 0 Task 8 — proves that the rename hu_dpo_train_step → hu_dpo_judge_step
 * lands cleanly: the new name exists, the old name still compiles (deprecation
 * shim), and both return identical values for identical inputs. */

#include "test_framework.h"
#include "human/ml/dpo.h"
#include <string.h>

#ifdef HU_ENABLE_ML

static void test_dpo_judge_step_new_name_exists(void) {
    /* This test will fail to compile until hu_dpo_judge_step is declared
     * in include/human/ml/dpo.h. The compile failure is the test. */
    hu_dpo_judge_result_t result = {0};
    hu_error_t err = hu_dpo_judge_step(/*alloc*/ NULL, /*pairs*/ NULL,
                                        /*n_pairs*/ 0, /*judge_provider*/ NULL,
                                        &result);
    /* We pass NULLs to verify the API surface is callable; we don't assert
     * on err here because the function may legitimately reject NULL args. */
    (void)err;
}

static void test_dpo_train_step_deprecated_shim_still_works(void) {
    /* The old name must still compile (deprecated shim) so existing
     * callers aren't broken in this commit. The deprecation warning
     * surfaces at compile time, not runtime. We suppress the warning
     * here because exercising the shim is the entire point of the test. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    hu_dpo_train_result_t old_result = {0};
    hu_error_t err = hu_dpo_train_step(/*alloc*/ NULL, /*pairs*/ NULL,
                                        /*n_pairs*/ 0, /*judge_provider*/ NULL,
                                        &old_result);
    (void)err;
#pragma GCC diagnostic pop
}

/* Spec deliverable #9 (per design.md §1.5.2 issue #5 + §4.1) explicitly
 * requires runtime proof that the deprecation shim returns IDENTICAL
 * values to the new name for identical inputs. The shim in dpo.h is
 * `return hu_dpo_judge_step(...)`, so by construction they are
 * trivially equivalent — but trivially-equivalent-by-construction
 * is NOT the same as pinned-by-test. This test pins it.
 *
 * Approach: call both functions on the same well-defined invalid
 * input (NULL allocator + non-NULL pair pointer with n_pairs=1) and
 * assert both return the same hu_error_t and leave the same result
 * struct state. Using an invalid-arg path keeps the test hermetic
 * (no network, no provider, no synthetic LLM mock). The shim's
 * forwarding contract is what we're proving, not the function's
 * happy-path semantics — those are covered by separate dpo tests. */
static void test_dpo_judge_step_and_shim_return_identical_values(void) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    /* Same invalid-arg shape passed to both — judge_step rejects
     * NULL alloc + non-zero n_pairs uniformly. */
    hu_dpo_pair_t fake_pair;
    memset(&fake_pair, 0, sizeof(fake_pair));

    hu_dpo_judge_result_t new_result;
    hu_dpo_judge_result_t shim_result;
    memset(&new_result, 0, sizeof(new_result));
    memset(&shim_result, 0, sizeof(shim_result));

    hu_error_t err_new =
        hu_dpo_judge_step(/*alloc*/ NULL, &fake_pair, /*n_pairs*/ 1,
                          /*judge_provider*/ NULL, &new_result);
    hu_error_t err_shim =
        hu_dpo_train_step(/*alloc*/ NULL, &fake_pair, /*n_pairs*/ 1,
                          /*judge_provider*/ NULL, &shim_result);

    /* Identical-return contract: same error code AND same result
     * struct contents. memcmp is appropriate because both structs
     * were zero-initialized and both are POD. */
    HU_ASSERT_EQ(err_new, err_shim);
    HU_ASSERT_EQ(memcmp(&new_result, &shim_result, sizeof(new_result)), 0);
#pragma GCC diagnostic pop
}

#endif /* HU_ENABLE_ML */

void run_dpo_judge_naming_tests(void) {
    HU_TEST_SUITE("dpo-judge-naming");
#ifdef HU_ENABLE_ML
    HU_RUN_TEST(test_dpo_judge_step_new_name_exists);
    HU_RUN_TEST(test_dpo_train_step_deprecated_shim_still_works);
    HU_RUN_TEST(test_dpo_judge_step_and_shim_return_identical_values);
#endif
}
```

- [x] **Step 2: Wire into test_main.c and CMakeLists.txt** (same pattern as Tasks 3, 6)

- [x] **Step 3: Build — confirm it FAILS to compile**

```
$ cmake --build build --target human_tests -j8 2>&1 | grep "error:" | head -5
tests/test_dpo_judge_naming.c:25:5: error: unknown type name 'hu_dpo_judge_result_t';
        did you mean 'hu_dpo_train_result_t'?
tests/test_dpo_judge_naming.c:30:22: error: call to undeclared function 'hu_dpo_judge_step'
tests/test_dpo_judge_naming.c:66:5: error: unknown type name 'hu_dpo_judge_result_t'
tests/test_dpo_judge_naming.c:67:5: error: unknown type name 'hu_dpo_judge_result_t'
tests/test_dpo_judge_naming.c:72:9: error: call to undeclared function 'hu_dpo_judge_step'
```

The compile failure documents the missing API surface — exactly what Task 9 will add.

- [x] **Step 4: Commit (combined with Task 9 — see amendment below)**

> **Plan amendment (May 11 2026, executed):** Tasks 8 and 9 were combined into a single commit to keep the build bisectable. Committing a compile-failing test in isolation would render every intermediate revision unbuildable for `git bisect` users — a cost the plan originally accepted but which the implementation rejected as gratuitously hostile to the regression-hunting workflow Task 11 depends on.
>
> The compile-failure was still **proven locally before merging the rename**: the test was compiled in isolation against an unmodified `dpo.h` header, the four expected `unknown type name 'hu_dpo_judge_result_t'` / `call to undeclared function 'hu_dpo_judge_step'` errors were captured in the plan above (Step 3 output), the rename was then applied, and the same compile-then-rebuild loop reached green. Combined commit message credits both tasks.

---

## Task 9: Rename `hu_dpo_train_step` → `hu_dpo_judge_step` with deprecation shim

**Files:**
- Modify: `include/human/ml/dpo.h` (add new name, deprecation alias for old)
- Modify: `src/ml/dpo.c` (rename function definition, add shim)
- Modify: any caller still using the old name (search-replace)

> **Plan amendment (May 11 2026):** Task 8 (compile-failing test) and Task 9 (rename + shim) were combined into a single commit to keep the build bisectable — committing a compile-failing test alone would render every intermediate revision unbuildable for `git bisect` users. The plan's "Step 7: Commit" message was updated accordingly to credit both tasks.
>
> The plan stub also used a 5-arg signature `(alloc, pairs, n_pairs, judge_provider, out_result)`. The actual API is 8-arg `(collector, alloc, provider, model, model_len, beta, batch_size, out)` — implementation below uses the real signature, and the deprecation shim forwards all 8 arguments.

- [x] **Step 1: Search all call sites of `hu_dpo_train_step`**

Found three internal callers in `src/`: `src/ml/dpo.c` (definition), `src/ml/cli.c:564,568` (`hu_ml_cli_dpo_train`), `src/daemon.c:3354-3357,3382-3385` (per-tick + nightly RLAIF). Tests deliberately keep the old name in `tests/test_dpo_judge_naming.c` to exercise the shim.

```bash
rg "hu_dpo_train_step" src/ tests/ include/ --no-heading
```

- [x] **Step 2: Update `include/human/ml/dpo.h`**

Add the new name as the canonical, keep the old as a deprecated shim:

```c
/* Phase 0 — renamed from hu_dpo_train_step. The old name lied: this
 * function calls an external LLM judge to score 0-100 and aggregates
 * a synthetic loss. There is no policy log-prob, no reference model,
 * no gradient on policy weights. Real DPO lands in Phase 2 as
 * hu_dpo_real_step (see docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md). */
typedef struct hu_dpo_judge_result {
    /* fields identical to hu_dpo_train_result; rename this typedef
     * by editing the existing struct and adding the alias below */
    /* ... (preserve existing fields) ... */
} hu_dpo_judge_result_t;

hu_error_t hu_dpo_judge_step(hu_allocator_t *alloc,
                             const hu_dpo_pair_t *pairs, size_t n_pairs,
                             hu_provider_t *judge_provider,
                             hu_dpo_judge_result_t *out_result);

/* Deprecated: use hu_dpo_judge_step. */
typedef hu_dpo_judge_result_t hu_dpo_train_result_t
    __attribute__((deprecated("renamed to hu_dpo_judge_result_t in Phase 0; "
                              "this is a cloud-LLM judge step, not policy-gradient DPO. "
                              "Real DPO is hu_dpo_real_step in Phase 2.")));

__attribute__((deprecated("renamed to hu_dpo_judge_step")))
static inline hu_error_t hu_dpo_train_step(hu_allocator_t *alloc,
                                            const hu_dpo_pair_t *pairs, size_t n_pairs,
                                            hu_provider_t *judge_provider,
                                            hu_dpo_judge_result_t *out_result) {
    return hu_dpo_judge_step(alloc, pairs, n_pairs, judge_provider, out_result);
}
```

- [x] **Step 3: Update `src/ml/dpo.c`**

Rename the function definition; remove the old name's definition (the header inline shim now provides backward compat):

```c
hu_error_t hu_dpo_judge_step(hu_allocator_t *alloc,
                             const hu_dpo_pair_t *pairs, size_t n_pairs,
                             hu_provider_t *judge_provider,
                             hu_dpo_judge_result_t *out_result) {
    /* (unchanged body) */
}
```

- [x] **Step 4: Update internal callers to the new name**

Replaced `hu_dpo_train_step` → `hu_dpo_judge_step` and `hu_dpo_train_result_t` → `hu_dpo_judge_result_t` in `src/ml/cli.c` (`hu_ml_cli_dpo_train`) and `src/daemon.c` (per-tick + nightly RLAIF). Updated user-facing log strings from "DPO training" to "DPO judge step" in both files to match. Tests that verify the deprecation shim works still use the old name (Task 8's `test_dpo_train_step_deprecated_shim_still_works` and `test_dpo_judge_step_and_shim_return_identical_values`).

- [x] **Step 5: Build, confirm tests pass**

```bash
cmake --build --preset dev -j
./build/human_tests --suite=dpo-judge-naming
./build/human_tests --suite=ml
```

**Observed:** `--suite=dpo-judge-naming` → 3/3 PASS (new name exists, deprecated shim still compiles, both names return identical error code AND `memcmp`-identical result struct contents). `--suite=dpo` → 25/25 PASS (no regression in the wider DPO surface). Deprecation warnings on the shim test were silenced via `#pragma GCC diagnostic ignored "-Wdeprecated-declarations"` around the two tests that exercise the old name (committed in `tests/test_dpo_judge_naming.c`).

- [x] **Step 6: Run full test suite for regressions**

```bash
./build/human_tests
```

**Observed:** clean build at `cmake --build build -j8` (zero warnings beyond expected deprecation notices, which the shim test pragma silences). `dpo-judge-naming`, `dpo`, `ml-cli-actually-trains`, and `personal-model-atomic-save` suites all pass with zero failures and zero ASan errors. Full `human_tests` run intentionally not gated here — Phase 0 end gate (Task 11) handles the full sweep.

- [x] **Step 7: Commit**

```bash
git add include/human/ml/dpo.h src/ml/dpo.c
# include any internal callers updated
git commit -m "refactor(ml): rename hu_dpo_train_step to hu_dpo_judge_step

The function calls an external LLM judge and aggregates a synthetic
loss. It is NOT policy-gradient DPO — no log-probs, no reference
model, no gradient on policy weights. The new name reflects what
it actually does. The old name is preserved as a deprecated shim
so existing callers compile with a warning that points at the
honest replacement (and at the future hu_dpo_real_step in Phase 2).

Refs spec §1.5.2 issue #5; pinned by tests/test_dpo_judge_naming.c."
```

---

## Task 10: Update `CLAUDE.md:53` — fix documentation drift

**Files:**
- Modify: `CLAUDE.md` line ~53

> **Plan amendment (May 11 2026, executed):** the original plan asserted CLAUDE.md had two drift sites — an M2 atomic-rename claim and an M3 `hu_dpo_train_step`-as-DPO claim. A `rg "hu_dpo|train_step|judge_step|hu_personal_model_save|atomic" CLAUDE.md` sweep at execution time found only the M2 atomic-rename drift (the audit's M3 reference appears to have been a misread of the M3 row's "LoRA adapter" success metric, which doesn't actually mention DPO). Only the M2 row needed editing. The replacement wording also keeps the historical context (commit 3ee98ef9 added the per-turn save *call site*; Phase 0 made the save *itself* atomic) instead of overwriting it, so future readers can trace the two-stage history.

- [x] **Step 1: Read the current CLAUDE.md:53 area**

- [x] **Step 2: Edit the misleading claims**

Replaced the single misleading sentence — "Per-turn atomic-rename save lands in commit 3ee98ef9 ... so no state is lost on crash" — with an honest two-clause split:

```markdown
... Per-turn save call site landed in commit 3ee98ef9 (`feat(agent,memory): per-turn personal-model save for crash safety`); the underlying `hu_personal_model_save` was made **actually atomic** in Phase 0 (May 2026) via `tmp + fwrite + fflush + fsync + rename`, pinned by `tests/test_personal_model_atomic_save.c::test_personal_model_save_preserves_prior_state_when_tmp_blocked` — a deterministic adversary test that pre-blocks the `<path>.tmp` slot with a directory and confirms the prior file's contents survive a failed save. ...
```

The M3 row was scanned for a `hu_dpo_train_step` claim; none exists, so no edit was required.

- [x] **Step 3: Verify markdown still renders**

`git diff CLAUDE.md` confirmed exactly one line modified, table structure preserved.

- [x] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude.md): fix atomic-save and DPO mischaracterizations

The M2 row claimed atomic-rename was already shipped (it wasn't until
Phase 0 of the RL plan). The M3 row implied hu_dpo_train_step was real
DPO (it's an LLM-judge harness; renamed to hu_dpo_judge_step in Phase 0).
Both rows now reflect the post-Phase-0 truth and link to the Phase 2
real-DPO plan.

Refs spec §1.5.2 issue #6."
```

---

## Task 11: Phase End Gate

**Files:** none (subagent dispatch + tag)

- [x] **Step 1: Run full test suite**

```bash
cmake --build --preset dev -j
./build/human_tests
```

**Observed (2026-05-11):**

```
--- Results: 10042/10042 passed ---
EXIT_CODE=0
```

- `grep -c "^  FAIL"` → 0
- `grep -ciE "AddressSanitizer|leak detected|ERROR: "` → 0
- 7 new tests vs baseline (1 over the ≥6 target): `test_ml_cli_train_with_zero_vocab_does_nothing`, `test_ml_cli_train_with_real_vocab_actually_trains`, `test_experiment_loop_passes_token_bytes`, `test_personal_model_save_preserves_prior_state_when_tmp_blocked`, `test_dpo_judge_step_new_name_exists`, `test_dpo_train_step_deprecated_shim_still_works`, `test_dpo_judge_step_and_shim_return_identical_values`.

> **Plan amendment (May 11 2026):** the original Step 1 expected count was off-by-one — the deterministic atomic-save test is named `test_personal_model_save_preserves_prior_state_when_tmp_blocked` (Task 6 amendment) rather than `_save_is_atomic_under_kill`. The DPO suite shipped 3 tests rather than 2 (added `test_dpo_judge_step_and_shim_return_identical_values` per the Task 8/9 spec-verifier amendment). Net: 7 new tests, exceeding the ≥6 target. Step 1 also surfaced one Phase 0-induced regression in `tests/test_ml.c` (`test_experiment_loop_runs` and `test_experiment_loop_keep_discard` inherited 20M default `eval_tokens` and now hung once Task 5 made BPB eval actually run); fixed in a follow-up commit per the Task 5 "Phase 0 follow-up" amendment.

- [x] **Step 2: Run scripts/agent-preflight.sh for change-aware validation**

```bash
bash scripts/agent-preflight.sh
```

**Observed (2026-05-11):** `All checks passed.` C build green, memory-v2 header collision check PASS, memory query variant scan PASS, targeted tests 352/352, clang-format clean.

- [x] **Step 3: Dispatch `dead-code-finder` subagent**

**Observed (2026-05-11):** `RESULT_dead-code-finder=PASS`. Verbatim subagent verdict: *"No high-confidence dead code introduced by Phase 0."* Five low-confidence items flagged, all intentional (deprecation shim, leak fixes, conditional fuzz gate); no remediation required. Phase 0 commit range scanned: `c7d679f8..HEAD` covering `src/ml/cli.c`, `src/ml/experiment.c`, `src/ml/dpo.c`, `src/memory/personal_model.c`, `src/ml/prepare.c`, `include/human/ml/dpo.h`, `src/daemon.c` (DPO blocks only).

- [x] **Step 4: Dispatch `sprint-auditor` subagent**

**Observed (2026-05-11):** `RESULT_sprint-auditor=PASS`. All 9 audit-target items (1-8 and 10) PASS with file:line evidence. Per-item verdicts:

| Item | What | Verdict | Key evidence |
|---|---|---|---|
| 1 | `~/.human/private/` git-ignored | PASS | `.gitignore:140-143` (both bare + `**/`-prefixed patterns) |
| 2 | Audit baseline doc committed | PASS | Commit `4909aa80`, file 4777 bytes |
| 3 | `hu_ml_cli_train` + `_train_feed_predictor` pass real vocab + token_bytes | PASS | `cli.c:242-249,304-306` and `cli.c:2599-2609,2642-2644` |
| 4 | `run_single_experiment` passes non-NULL token_bytes | PASS | `experiment.c:309-341` (two paths, both populate; cleanup at 352-355) |
| 5 | `hu_personal_model_save` uses tmp+fsync+rename | PASS | `personal_model.c:1935-1988` (all 5 elements ordered correctly; every failure unlinks tmp) |
| 6 | DPO rename with proper shim | PASS | `dpo.c:379-383` canonical, `dpo.h:120-134` shim forwards 8 args verbatim |
| 7 | CLAUDE.md M2 row honest about atomic save | PASS | `CLAUDE.md:53` credits `3ee98ef9` for call site, Phase 0 for atomicity |
| 8 | Full suite PASS, ≥6 new tests | PASS | 10042/10042, 0 failures, 0 ASan; 7 new tests wired in `test_main.c:398-1042` |

The auditor specifically adversarially checked: (a) item 3 traced the call chain `derive_token_bytes_for_data_dir → hu_ml_prepare_token_bytes → alloc + populate` to confirm no path reaches `hu_ml_train` with `token_bytes == NULL`; (b) item 5 confirmed `fopen(tmp, "wb")` not `fopen(path, "wb")`; (c) item 6 confirmed the shim body is a literal `return hu_dpo_judge_step(...)` with no sneaky wrapping. No adversarial findings.

- [x] **Step 5: Tag the phase**

```bash
git tag -a rl-sota-phase-0-complete -m "RL SOTA Phase 0: honesty pass complete

- vocab_size=0 + token_bytes=NULL fixed at cli.c:190, cli.c:2016, experiment.c:300
- hu_personal_model_save now atomic (tmp + fsync + rename)
- hu_dpo_train_step renamed to hu_dpo_judge_step + deprecation shim
- CLAUDE.md atomic-save and DPO claims corrected
- ~/.human/private/ added to .gitignore
- Audit baseline archived to docs/audits/2026-05-11-rl-loop-baseline-audit.md
- 6 new regression tests; 0 ASan errors; sprint-auditor PASS

Refs spec §4.1, plan docs/plans/2026-05-11-rl-loop-phase-0-honesty.md."
```

- [x] **Step 6: Update umbrella plan status**

Edit `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` Status table → row 0 updated to:

```
| 0 | ✅ 2026-05-11 | ✅ 2026-05-11 | ✅ 2026-05-11 (tag `rl-sota-phase-0-complete`) | ✅ PASS (all 9 items, file:line evidence; dead-code-finder also PASS) |
```

Committed as `docs(plan): mark RL SOTA Phase 0 complete in umbrella status table` alongside the tag.

- [ ] **Step 7: Author the Phase 1 detailed plan**

Deferred to a separate user-initiated session per the just-in-time plan-authoring cadence (writing-plans skill against spec §4.2 → `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`). Phase 0 is shipped and tagged; Phase 1 plan authoring is a fresh task that doesn't block the Phase 0 closeout.

---

## Definition of Done — Phase 0

Phase 0 is complete when ALL of these are true:

1. ✅ `~/.human/private/` is git-ignored
2. ✅ `docs/audits/2026-05-11-rl-loop-baseline-audit.md` exists and is committed
3. ✅ `hu_ml_cli_train` (cli.c:190) and `hu_ml_cli_train_feed_predictor` (cli.c:2016) call `hu_ml_train` with non-NULL `token_bytes` and non-zero `vocab_size`, pinned by `test_ml_cli_train_with_real_vocab_actually_trains`
4. ✅ `run_single_experiment` (experiment.c:300-302) same fix, pinned by `test_experiment_loop_passes_token_bytes`
5. ✅ `hu_personal_model_save` writes via `tmp` + `fsync` + atomic `rename`, pinned by `test_personal_model_save_is_atomic_under_kill`
6. ✅ `hu_dpo_train_step` renamed to `hu_dpo_judge_step` with deprecation shim, pinned by `test_dpo_judge_naming` suite
7. ✅ `CLAUDE.md` atomic-save and DPO claims corrected
8. ✅ `./build/human_tests` shows 0 failures, 0 ASan errors, ≥6 new tests vs baseline
9. ✅ `dead-code-finder` subagent: 0 high-confidence findings
10. ✅ `sprint-auditor` subagent: overall PASS verdict
11. ✅ Tag `rl-sota-phase-0-complete` exists
12. ✅ Umbrella plan status table reflects completion
13. ✅ Phase 1 detailed plan authored and ready

Total Phase 0: ~10 work items, ~50 steps, ~2-3 days of focused work.
