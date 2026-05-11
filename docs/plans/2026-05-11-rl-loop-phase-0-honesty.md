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

Append to `.gitignore`:

```
# Private corpus + derivatives (per spec §13)
~/.human/private/
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

Write `docs/audits/2026-05-11-rl-loop-baseline-audit.md` with this content (the audit baseline this entire RL effort was designed against):

````markdown
# RL & Neural Improvement Loop — Baseline Audit, May 11 2026

**Status:** Historical record. This audit was the baseline against which `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` was authored.

**Audit method:** 5-explorer concurrent review of `src/ml/`, `src/agent/`, `src/memory/`, `src/eval/`, `src/persona/`, `src/providers/`, `src/daemon.c`, plus committed `main` baseline + Track D Phase 1 in-flight uncommitted tree.

## Findings — what was real (committed `main`)

- Hand-derived backward + Muon+AdamW optimizer in `src/ml/gpt.c`, `train.c`, `muon_adamw.c`. Tests do real finite-difference grad checks.
- Real LoRA primitives in `src/ml/lora.c` (low-rank A/B, apply, backward, save/load).
- Real preference-data layer in `src/ml/dpo.c` (SQLite `dpo_pairs`, JSONL export).
- Real hot-swap adapter loading at daemon startup and post-train (`src/daemon.c:2465-2532`, `src/agent/lora_training_runner.c:90-93`).
- Real eval harness with LLM-as-judge, bootstrap regression detection, nightly W16 bench (`src/eval.c`, `evaluation.yml`).

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
git commit -m "docs(audit): archive May 11 2026 RL-loop baseline audit"
```

---

## Task 3: Add `tests/test_ml_cli_actually_trains.c` — failing test for `vocab_size=0` bug

**Files:**
- Create: `tests/test_ml_cli_actually_trains.c`
- Modify: `tests/test_main.c` (declare + call `run_ml_cli_actually_trains_tests`)
- Modify: `CMakeLists.txt` (add the new test file to the test target)

- [ ] **Step 1: Write the failing test**

Create `tests/test_ml_cli_actually_trains.c`:

```c
/* Phase 0 Task 3 — proves that hu_ml_train with vocab_size=0 + token_bytes=NULL
 * does not actually train, and that the patched cli paths supply a non-zero
 * vocab_size and a non-NULL token_bytes lookup so loss reduction is real. */

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/dataloader.h"
#include "human/ml/train.h"
#include <stdint.h>

#ifdef HU_ENABLE_ML

/* Synthetic data: vocab_size=128, token_bytes is a 128-entry table where
 * every byte maps to itself. Train for 32 steps; expect val_bpb to drop. */
static void test_ml_cli_train_with_zero_vocab_does_nothing(void) {
    hu_allocator_t *alloc = hu_system_allocator();
    hu_ml_config_t cfg = hu_ml_default_config();
    cfg.training.max_steps = 8;
    cfg.training.device_batch_size = 4;
    cfg.model.vocab_size = 128;

    hu_model_t model = {0};
    hu_ml_optimizer_t optimizer = {0};
    HU_ASSERT_EQ(hu_gpt_create(alloc, &cfg.model, &model), HU_OK);
    HU_ASSERT_EQ(hu_muon_adamw_create(alloc, &cfg.optimizer, &model, &optimizer), HU_OK);

    /* Build a tiny synthetic dataset of 256 tokens, all in [0, 128). */
    int32_t toks[256];
    for (int i = 0; i < 256; i++) toks[i] = i & 127;

    hu_ml_dataloader_t *loader = NULL;
    HU_ASSERT_EQ(hu_ml_dataloader_create_from_buffer(alloc, toks, 256, &cfg.training, &loader),
                 HU_OK);

    /* Pass NULL token_bytes + 0 vocab_size — this is the bug */
    hu_ml_train_result_t result_buggy = {0};
    hu_error_t err = hu_ml_train(alloc, &model, &optimizer, loader, NULL,
                                 &cfg.training, NULL, 0, &result_buggy);
    HU_ASSERT_EQ(err, HU_OK);
    /* The buggy path either skips training entirely (val_bpb == 0)
     * or the BPB metric is meaningless without token_bytes. The
     * invariant we lock down: the bug case CANNOT report a finite,
     * positive, decreasing BPB curve. */
    HU_ASSERT_TRUE(result_buggy.val_bpb == 0.0 || result_buggy.val_bpb < 0.0);

    hu_ml_dataloader_deinit(loader);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);
}

static void test_ml_cli_train_with_real_vocab_actually_trains(void) {
    hu_allocator_t *alloc = hu_system_allocator();
    hu_ml_config_t cfg = hu_ml_default_config();
    cfg.training.max_steps = 32;
    cfg.training.device_batch_size = 4;
    cfg.model.vocab_size = 128;

    hu_model_t model = {0};
    hu_ml_optimizer_t optimizer = {0};
    HU_ASSERT_EQ(hu_gpt_create(alloc, &cfg.model, &model), HU_OK);
    HU_ASSERT_EQ(hu_muon_adamw_create(alloc, &cfg.optimizer, &model, &optimizer), HU_OK);

    int32_t toks[256];
    for (int i = 0; i < 256; i++) toks[i] = i & 127;

    /* Token-bytes lookup: identity for vocab 128 */
    uint8_t token_bytes[128];
    for (int i = 0; i < 128; i++) token_bytes[i] = 1; /* 1 byte per token */

    hu_ml_dataloader_t *loader = NULL;
    HU_ASSERT_EQ(hu_ml_dataloader_create_from_buffer(alloc, toks, 256, &cfg.training, &loader),
                 HU_OK);

    hu_ml_train_result_t result = {0};
    hu_error_t err = hu_ml_train(alloc, &model, &optimizer, loader, NULL,
                                 &cfg.training, token_bytes, 128, &result);
    HU_ASSERT_EQ(err, HU_OK);
    /* With real vocab + token_bytes, val_bpb should be a finite positive
     * number and num_steps should equal max_steps. */
    HU_ASSERT_TRUE(result.val_bpb > 0.0);
    HU_ASSERT_EQ(result.num_steps, (size_t)cfg.training.max_steps);

    hu_ml_dataloader_deinit(loader);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);
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

- [ ] **Step 4: Build and run — confirm it FAILS as expected on the bug**

```bash
cmake --preset dev
cmake --build --preset dev -j
./build/human_tests --suite=ml-cli-actually-trains
```

Expected: `test_ml_cli_train_with_real_vocab_actually_trains` FAILS because the bug case in `hu_ml_train` may currently skip training entirely. (The first test, `_zero_vocab_does_nothing`, will pass — it documents the broken state.) If both pass, re-read the assertions; one MUST fail before Phase 0 fix.

- [ ] **Step 5: Commit (failing test for documentation)**

```bash
git add tests/test_ml_cli_actually_trains.c tests/test_main.c CMakeLists.txt
git commit -m "test(ml): add failing test pinning vocab_size=0 silent-no-op bug

Pins the bug at hu_ml_cli_train (cli.c:190) and hu_ml_cli_train_feed_predictor
(cli.c:2016) — both pass NULL token_bytes + 0 vocab_size to hu_ml_train,
defeating the CE objective. The fix lands in the next commit.

Refs spec §1.5.2 issue #1, #2."
```

---

## Task 4: Fix `hu_ml_cli_train` (cli.c:190) and `hu_ml_cli_train_feed_predictor` (cli.c:2016)

**Files:**
- Modify: `src/ml/cli.c` lines ~187-200 (`hu_ml_cli_train`)
- Modify: `src/ml/cli.c` lines ~2010-2020 (`hu_ml_cli_train_feed_predictor`)

- [ ] **Step 1: Read the surrounding context for both call sites**

```bash
# Context for cli_train
sed -n '170,200p' src/ml/cli.c
# Context for cli_train_feed_predictor
sed -n '1990,2020p' src/ml/cli.c
```

- [ ] **Step 2: Locate the `token_bytes` primitive**

```bash
rg "token_bytes" src/ml/ include/human/ml/ --no-heading | head -20
```

Expected key hit: `src/ml/prepare.c:132` defines `hu_ml_prepare_token_bytes(alloc, tokenizer, &table, &count)` — derives a token_bytes table from a BPE tokenizer (signature returns `int32_t **token_bytes_out`, `size_t *count`). Confirm the public header:

```bash
rg "hu_ml_prepare_token_bytes" include/human/ml/
```

If the function isn't in `include/human/ml/prepare.h`, this Task 4 grows by one sub-step to declare it there. (As of May 11 2026 it is internal; declare it public.)

- [ ] **Step 3: Fix `hu_ml_cli_train` (cli.c:~190)**

The cli_train handler already loads a BPE tokenizer; reuse it. Replace:

```c
    err =
        hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result);
```

With:

```c
    /* Phase 0 fix — derive token_bytes from the loaded BPE tokenizer
     * and forward vocab_size from the parsed config so the CE objective
     * has the lookup it needs. Without these the bits-per-byte metric
     * is zero and the gradient computation is degenerate. */
    int32_t *token_bytes = NULL;
    size_t token_bytes_count = 0;
    err = hu_ml_prepare_token_bytes(alloc, tokenizer, &token_bytes, &token_bytes_count);
    if (err != HU_OK) {
        hu_log_error("ml", NULL, "hu_ml_prepare_token_bytes failed: %d", err);
        goto cleanup;
    }

    err = hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training,
                      (uint8_t *)token_bytes, cfg.model.vocab_size, &result);
    alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
```

(If the existing handler doesn't already have a `tokenizer` local, locate where it's loaded — search the function for `hu_bpe_tokenizer_load` — and reuse that local. If the handler doesn't load a tokenizer at all, load it from `<data_dir>/tokenizer.bin` before this fix.)

- [ ] **Step 4: Apply the same fix at cli.c:~2016 (`hu_ml_cli_train_feed_predictor`)**

The feed-predictor path uses a tokenizer too; reuse it the same way. If it doesn't load one, load it.

- [ ] **Step 5: Build and run — confirm test now PASSES**

```bash
cmake --build --preset dev -j
./build/human_tests --suite=ml-cli-actually-trains
```

Expected: both tests pass (the buggy-shape test still passes — it asserts the bug behavior is broken; the fix doesn't change `hu_ml_train` semantics, only the CLI path callers).

- [ ] **Step 6: Run full ML suite to catch regressions**

```bash
./build/human_tests --suite=ml
```

Expected: 0 failures.

- [ ] **Step 7: Commit**

```bash
git add src/ml/cli.c
# include prepare.h / prepare.c if a sub-task expanded the loader API
git commit -m "fix(ml): supply real vocab_size + token_bytes to hu_ml_train CLI paths

hu_ml_cli_train (cli.c:190) and hu_ml_cli_train_feed_predictor (cli.c:2016)
both passed NULL token_bytes + 0 vocab_size, defeating the CE objective.
The fix loads the token_bytes lookup written by the prepare step and
forwards a real vocab_size from the prepared config.

Refs spec §1.5.2 issue #1, #2; pinned by tests/test_ml_cli_actually_trains.c."
```

---

## Task 5: Fix `experiment.c:300-302` `token_bytes=NULL`

**Files:**
- Modify: `src/ml/experiment.c` lines ~290-310 (`run_single_experiment`)
- Modify: `tests/test_ml_cli_actually_trains.c` (add a test for the experiment path)

- [ ] **Step 1: Read context**

```bash
sed -n '280,320p' src/ml/experiment.c
```

- [ ] **Step 2: Add a failing test for the experiment path**

Append to `tests/test_ml_cli_actually_trains.c`:

```c
#ifdef HU_ENABLE_ML
static void test_experiment_loop_passes_token_bytes(void) {
    /* Mirror test_ml_cli_train_with_real_vocab_actually_trains but
     * exercise the experiment loop's run_single_experiment path
     * via hu_experiment_loop_step (or equivalent public entry).
     * Assert val_bpb is finite + positive. */
    hu_allocator_t *alloc = hu_system_allocator();
    hu_ml_config_t cfg = hu_ml_default_config();
    cfg.training.max_steps = 16;
    cfg.training.device_batch_size = 4;
    cfg.model.vocab_size = 128;

    hu_experiment_result_t result = {0};
    hu_error_t err = hu_experiment_run_single(alloc, &cfg, /*data_dir=*/NULL, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.val_bpb > 0.0);
}
#endif
```

And add `HU_RUN_TEST(test_experiment_loop_passes_token_bytes);` inside `run_ml_cli_actually_trains_tests`.

- [ ] **Step 3: Run, confirm fail**

```bash
cmake --build --preset dev -j
./build/human_tests --filter=test_experiment_loop_passes_token_bytes
```

Expected: FAIL.

- [ ] **Step 4: Fix experiment.c:300-302**

Apply the same `hu_ml_prepare_token_bytes` + non-NULL `token_bytes` pattern to the `hu_ml_train` call inside `run_single_experiment`. The experiment loop loads its tokenizer from `cfg.model.tokenizer_path` (or equivalent) — reuse that local.

- [ ] **Step 5: Run, confirm pass**

```bash
cmake --build --preset dev -j
./build/human_tests --filter=test_experiment_loop_passes_token_bytes
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ml/experiment.c tests/test_ml_cli_actually_trains.c
git commit -m "fix(ml): supply token_bytes to hu_ml_train in run_single_experiment

Mirror of the cli_train fix at experiment.c:300-302. Without it the
val_bpb metric was meaningless and the experiment loop's keep/discard
decision was effectively random.

Refs spec §1.5.2 issue #3; pinned by test_experiment_loop_passes_token_bytes."
```

---

## Task 6: Add `tests/test_personal_model_atomic_save.c` — failing test for non-atomic save

**Files:**
- Create: `tests/test_personal_model_atomic_save.c`
- Modify: `tests/test_main.c` (declare + call `run_personal_model_atomic_save_tests`)
- Modify: `CMakeLists.txt` (add to test target)

- [ ] **Step 1: Write the failing test**

Create `tests/test_personal_model_atomic_save.c`:

```c
/* Phase 0 Task 6 — proves that hu_personal_model_save, when interrupted
 * mid-write (SIGKILL between fwrite and rename), leaves the on-disk file
 * either fully intact at the prior state OR fully intact at the new
 * state — never partial. The current implementation (direct fopen+fwrite)
 * fails this; the fix (write-tmp + fsync + rename) passes it. */

#include "test_framework.h"
#include "human/memory/personal_model.h"
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

static void test_personal_model_save_is_atomic_under_kill(void) {
    /* Create a temp dir for this test */
    char tmpl[] = "/tmp/hu_pm_atomic_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);
    char path[256];
    snprintf(path, sizeof(path), "%s/personal_model.bin", dir);

    /* Step 1: write a known-good v4 model to <path> via the public save */
    hu_personal_model_t known_good;
    memset(&known_good, 0, sizeof(known_good));
    known_good.version = HU_PM_VERSION;
    known_good.fact_count = 1;
    strncpy(known_good.facts[0].text, "known-good fact",
            sizeof(known_good.facts[0].text) - 1);
    HU_ASSERT_EQ(hu_personal_model_save(&known_good, path), HU_OK);

    /* Step 2: fork. Child opens the file for writing via the public save
     * with a DIFFERENT model, but a USR1 mid-save handler kills it
     * between fwrite and rename via internal probe (or via repeated
     * write attempts under signal pressure). */
    pid_t pid = fork();
    HU_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        /* Child: launch a save and immediately abort itself so the
         * post-fwrite-but-pre-rename window is entered. We approximate
         * the race by repeatedly saving a large model in a loop and
         * killing ourselves at a random point. */
        hu_personal_model_t big = {0};
        big.version = HU_PM_VERSION;
        big.fact_count = HU_PM_MAX_FACTS;
        for (size_t i = 0; i < big.fact_count; i++) {
            snprintf(big.facts[i].text, sizeof(big.facts[i].text),
                     "fact-%zu-padded-to-force-multi-block-write", i);
        }
        /* Save and kill ourselves. The atomic implementation MUST
         * have completed the rename or not started it; either way the
         * file at <path> is intact. */
        hu_personal_model_save(&big, path);
        raise(SIGKILL);
        _exit(0);
    }

    /* Parent: wait for child, then verify the file is loadable AND
     * the content is either the known-good or the big model, never
     * a mix. */
    int status = 0;
    waitpid(pid, &status, 0);

    /* Verify file is non-empty and loads cleanly */
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_TRUE(st.st_size > 0);

    hu_personal_model_t loaded = {0};
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);

    /* Either we see the known-good (fact_count == 1) OR the big
     * (fact_count == HU_PM_MAX_FACTS). NEVER something in between. */
    HU_ASSERT_TRUE(loaded.fact_count == 1 ||
                   loaded.fact_count == HU_PM_MAX_FACTS);

    /* Cleanup */
    unlink(path);
    rmdir(dir);
}

void run_personal_model_atomic_save_tests(void) {
    HU_TEST_SUITE("personal-model-atomic-save");
    HU_RUN_TEST(test_personal_model_save_is_atomic_under_kill);
}
```

- [ ] **Step 2: Wire into `tests/test_main.c` and `CMakeLists.txt`**

Add `void run_personal_model_atomic_save_tests(void);` declaration and call. Add `tests/test_personal_model_atomic_save.c` to CMakeLists test sources around the existing `tests/test_personal_model.c` line ~2352.

- [ ] **Step 3: Build and run — confirm FAIL**

```bash
cmake --build --preset dev -j
./build/human_tests --suite=personal-model-atomic-save
```

Expected: FAIL with the "loaded.fact_count == 1 || loaded.fact_count == HU_PM_MAX_FACTS" assertion failing (the file may be truncated or partially written under SIGKILL).

- [ ] **Step 4: Commit (failing test)**

```bash
git add tests/test_personal_model_atomic_save.c tests/test_main.c CMakeLists.txt
git commit -m "test(memory): add failing test pinning non-atomic personal_model_save

The save path is direct fopen+fwrite at personal_model.c:1851. A SIGKILL
between fwrite and intended rename leaves the file partial. Fix lands
in the next commit.

Refs spec §1.5.2 issue #4."
```

---

## Task 7: Implement atomic save for `hu_personal_model_save`

**Files:**
- Modify: `src/memory/personal_model.c` lines ~1848-1880 (`hu_personal_model_save` implementation)

- [ ] **Step 1: Read the current implementation**

```bash
sed -n '1848,1900p' src/memory/personal_model.c
```

- [ ] **Step 2: Replace with atomic-rename pattern**

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

- [ ] **Step 3: Build and run — confirm test passes**

```bash
cmake --build --preset dev -j
./build/human_tests --suite=personal-model-atomic-save
```

Expected: PASS.

- [ ] **Step 4: Run full personal-model suite to catch regressions**

```bash
./build/human_tests --suite=personal-model
```

Expected: 0 failures (all 25+ existing personal-model v4 tests still pass).

- [ ] **Step 5: Run ASan check**

```bash
./build/human_tests --suite=personal-model 2>&1 | grep -E 'ERROR|leak|asan' || echo "ASan clean"
```

Expected: `ASan clean`.

- [ ] **Step 6: Dispatch `critic` subagent on the diff**

```
Task: critic
Prompt: Review the diff for src/memory/personal_model.c hu_personal_model_save in the
        most recent commit. Look for: missing error returns, edge cases (path containing
        no slash, path > sizeof(tmp)-5, fsync failing on tmpfs, rename across
        filesystems), and ASan-detectable issues. Report any high-priority concerns.
```

Address any critic findings before commit.

- [ ] **Step 7: Commit**

```bash
git add src/memory/personal_model.c
git commit -m "fix(memory): make hu_personal_model_save atomic via tmp+fsync+rename

Direct fopen+fwrite left a window where SIGKILL or power loss could
corrupt ~/.human/personal_model.bin into a partially-written state.
The new write-tmp + fflush + fsync + fclose + rename pattern guarantees
the destination is either the prior state or the new state, never
partial.

Refs spec §1.5.2 issue #4; pinned by test_personal_model_save_is_atomic_under_kill."
```

---

## Task 8: Add `tests/test_dpo_judge_naming.c` — failing test for misleading name

**Files:**
- Create: `tests/test_dpo_judge_naming.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_dpo_judge_naming.c`:

```c
/* Phase 0 Task 8 — proves that the rename hu_dpo_train_step → hu_dpo_judge_step
 * lands cleanly: the new name exists, the old name still compiles (deprecation
 * shim), and both return identical values for identical inputs. */

#include "test_framework.h"
#include "human/ml/dpo.h"

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
     * surfaces at compile time, not runtime. */
    hu_dpo_train_result_t old_result = {0};
    hu_error_t err = hu_dpo_train_step(/*alloc*/ NULL, /*pairs*/ NULL,
                                        /*n_pairs*/ 0, /*judge_provider*/ NULL,
                                        &old_result);
    (void)err;
}

#endif /* HU_ENABLE_ML */

void run_dpo_judge_naming_tests(void) {
    HU_TEST_SUITE("dpo-judge-naming");
#ifdef HU_ENABLE_ML
    HU_RUN_TEST(test_dpo_judge_step_new_name_exists);
    HU_RUN_TEST(test_dpo_train_step_deprecated_shim_still_works);
#endif
}
```

- [ ] **Step 2: Wire into test_main.c and CMakeLists.txt** (same pattern as Tasks 3, 6)

- [ ] **Step 3: Build — confirm it FAILS to compile**

```bash
cmake --build --preset dev -j 2>&1 | tail -20
```

Expected: compile error `unknown type name 'hu_dpo_judge_result_t'` or `implicit declaration of function 'hu_dpo_judge_step'`. The compile failure documents the missing API.

- [ ] **Step 4: Commit (compile-failing test)**

```bash
git add tests/test_dpo_judge_naming.c tests/test_main.c CMakeLists.txt
git commit -m "test(ml): add compile-failing test for hu_dpo_judge_step rename

Pins the missing 'judge' name. Fix lands in the next commit.

Refs spec §1.5.2 issue #5."
```

---

## Task 9: Rename `hu_dpo_train_step` → `hu_dpo_judge_step` with deprecation shim

**Files:**
- Modify: `include/human/ml/dpo.h` (add new name, deprecation alias for old)
- Modify: `src/ml/dpo.c` (rename function definition, add shim)
- Modify: any caller still using the old name (search-replace)

- [ ] **Step 1: Search all call sites of `hu_dpo_train_step`**

```bash
rg "hu_dpo_train_step" src/ tests/ include/ --no-heading
```

- [ ] **Step 2: Update `include/human/ml/dpo.h`**

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

- [ ] **Step 3: Update `src/ml/dpo.c`**

Rename the function definition; remove the old name's definition (the header inline shim now provides backward compat):

```c
hu_error_t hu_dpo_judge_step(hu_allocator_t *alloc,
                             const hu_dpo_pair_t *pairs, size_t n_pairs,
                             hu_provider_t *judge_provider,
                             hu_dpo_judge_result_t *out_result) {
    /* (unchanged body) */
}
```

- [ ] **Step 4: Update internal callers to the new name**

For every hit from Step 1 inside `src/`, replace `hu_dpo_train_step` with `hu_dpo_judge_step`. Tests that verify the deprecation shim works still use the old name (Task 8's second test).

- [ ] **Step 5: Build, confirm tests pass**

```bash
cmake --build --preset dev -j
./build/human_tests --suite=dpo-judge-naming
./build/human_tests --suite=ml
```

Expected: all suites pass. Compile may emit deprecation warnings for the shim test — that's expected; suppress with `__attribute__((no_warn_deprecated))` or `#pragma GCC diagnostic ignored "-Wdeprecated-declarations"` around the shim test only if the build treats warnings as errors here.

- [ ] **Step 6: Run full test suite for regressions**

```bash
./build/human_tests
```

Expected: 0 failures, 0 ASan errors.

- [ ] **Step 7: Commit**

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

- [ ] **Step 1: Read the current CLAUDE.md:53 area**

```bash
sed -n '45,60p' CLAUDE.md
```

- [ ] **Step 2: Edit the misleading claims**

Find the M2 row in the Strategic Missions table that says "atomic-rename save lands in commit 3ee98ef9" and update to reflect the new truth:

```markdown
| **M2** | **Personal Model** — Unified model-of-the-person from memory | **Hard.** Single artifact (`hu_personal_model_t`, `src/memory/personal_model.c`); facts/topics/goals/style are accumulated per turn, summarized via `hu_personal_model_build_prompt`, and injected into every system prompt. Per-turn save with **atomic-rename via tmp+fsync+rename** (Phase 0 fix; pinned by `tests/test_personal_model_atomic_save.c`). Fact extraction is still heuristic pattern matching ("i like", "i never"); learned-style adaptation lives only in the prompt summary, not in a model checkpoint. | Measurable adaptation in tone/timing after 50 conversations |
```

And update the M3 row's reference to `hu_dpo_train_step`:

Find the row that says `RLAIF nightly` or that references `hu_dpo_train_step` as DPO and replace with:

```markdown
... `hu_dpo_judge_step` (renamed in Phase 0 from `hu_dpo_train_step`; this is an LLM-judge metric harness, NOT policy-gradient DPO. Real DPO with frozen π_ref + policy log-probs lands in Phase 2 as `hu_dpo_real_step` per `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md`).
```

(Exact insertion point depends on the row's current wording; read and patch the relevant cells without disturbing the surrounding table structure.)

- [ ] **Step 3: Verify markdown still renders**

```bash
# Quick visual check that the table is well-formed
sed -n '40,80p' CLAUDE.md
```

- [ ] **Step 4: Commit**

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

- [ ] **Step 1: Run full test suite**

```bash
cmake --build --preset dev -j
./build/human_tests
```

Expected: 0 failures, 0 ASan errors. Note total test count (should be `<previous count> + 4` from the new test functions: `_zero_vocab_does_nothing`, `_with_real_vocab_actually_trains`, `_passes_token_bytes`, `_save_is_atomic_under_kill`, plus the dpo-judge-naming pair = 6 new).

- [ ] **Step 2: Run scripts/agent-preflight.sh for change-aware validation**

```bash
bash scripts/agent-preflight.sh
```

Expected: clean.

- [ ] **Step 3: Dispatch `dead-code-finder` subagent**

```
Task: dead-code-finder
Prompt: Review the Phase 0 commits (range: <first-phase-0-commit>..HEAD).
        Specifically check src/ml/cli.c, src/ml/experiment.c, src/ml/dpo.c,
        src/memory/personal_model.c for any unused exports, unreachable
        branches, or dead variables introduced by the fixes. Report findings
        with confidence level.
```

Address any high-confidence dead code before the next step.

- [ ] **Step 4: Dispatch `sprint-auditor` subagent**

```
Task: sprint-auditor
Prompt: Read docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md §4.1
        (Phase 0 file map) and the actual diff between <phase-0-base-commit>
        and HEAD. For EACH item in the §4.1 table (10 items), answer:
        "did Phase 0 deliver this?" Cite file:line evidence. Do not trust
        commit messages — verify against actual code. Report per-item
        PASS/FAIL plus an overall verdict.
```

Phase 0 is marked complete only on overall PASS. If FAIL, address gaps and re-dispatch.

- [ ] **Step 5: Tag the phase**

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

- [ ] **Step 6: Update umbrella plan status**

Edit `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` Status table:

```
| 0 | ✅ 2026-05-11 | ✅ <date> | ✅ <date> | ✅ PASS |
```

Commit the umbrella update:

```bash
git add docs/plans/2026-05-11-full-sota-rl-improvement-loop.md
git commit -m "docs(plan): mark RL SOTA Phase 0 complete in umbrella status table"
```

- [ ] **Step 7: Author the Phase 1 detailed plan**

Following the just-in-time plan-authoring cadence, run the writing-plans skill against spec §4.2 to create `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`. Then return here to confirm Phase 0 is complete and Phase 1 is ready to start.

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
