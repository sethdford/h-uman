# Design for US-7.1 (P0): Activate the DPO preference pass in `finetune-gemma.py`

> **Sprint:** 7 (Digital Twin via Gemma DPO + Continuous Personalization)
> **Risk tier:** MEDIUM (`scripts/` only)
> **Author:** tech-lead
> **Status:** READY (with one AC-text refinement flagged in §7)

---

## 0. Critical interface finding — read first

**`mlx_lm` (Apple's official package — both the installed 0.31.2 and `main` HEAD on `ml-explore/mlx-lm`) does NOT support DPO.**

Hard evidence:

- `/opt/homebrew/lib/python3.14/site-packages/mlx_lm/lora.py:107` — `choices=["lora", "dora", "full"]` for `--fine-tune-type`. No `dpo` value.
- `mlx_lm` subcommand list: `benchmark, cache_prompt, chat, convert, evaluate, fuse, generate, lora, manage, perplexity, awq, dwq, dynamic_quant, gptq, server, upload, share`. No `dpo` subcommand.
- Upstream `ml-explore/mlx-lm@main/mlx_lm/lora.py`: same three choices, same line. PR #794 ("feat: DPO Trainer support") **closed unmerged**. PR #417 ("Implement dpo tuning") **still open**, no merge ETA.

Therefore the AC-7.1.1 literal string `"--fine-tune-type dpo"` cannot be produced by stock `mlx_lm` today, period. The implementer cannot guess their way out of this.

### Path forward — recommended **Option A**

Use the third-party fork **`mlx-lm-lora`** (PyPI package `mlx-lm-lora`, latest **v2.1.0**, repo `Goekdeniz-Guelmez/mlx-lm-lora`). It is a drop-in superset of `mlx_lm.lora` that adds DPO, ORPO, CPO, KTO, GRPO, online-DPO, XPO, and QAT, all behind a single `mlx_lm_lora.train` CLI. README pulled and validated 2026-05-16.

Verified CLI shape for DPO:

```
python3 -m mlx_lm_lora.train \
  --model <hf-id> \
  --train \
  --train-mode dpo \
  --train-type lora \
  --data <dir-with-train.jsonl-and-valid.jsonl> \
  --adapter-path <out> \
  --resume-adapter-file <sft-adapter>/adapters.safetensors \
  --beta 0.1 \
  --dpo-cpo-loss-type sigmoid \
  --iters <n> --batch-size <n> --learning-rate <lr> \
  --num-layers <n> --max-seq-length <n> \
  --grad-checkpoint
```

Verified data format (matches what `prepare_dpo_from_db` already emits):

```jsonl
{"prompt": "...", "chosen": "...", "rejected": "..."}
{"system": "...", "prompt": "...", "chosen": "...", "rejected": "..."}   // optional system
```

Required keys: `prompt`, `chosen`, `rejected`. Optional: `system`. File naming: `train.jsonl` + `valid.jsonl` in a single directory passed via `--data` (same convention as `mlx_lm.lora`). Default beta 0.1; default loss type `sigmoid`; if `--reference-model-path` is omitted, the main model is used as its own reference (frozen pre-LoRA pass).

### Why Option A over B and C

- **Option B (re-implement DPO loop in Python using `mlx` primitives)** — would mirror the math in `src/ml/dpo.c` but on MLX tensors. Cost: easily 400–600 LOC of new Python + tokenizer wiring + reference-logprob caching + safetensors I/O + tests for the loss math. This is a separate L-sized story; doing it inside an M-sized US-7.1 will blow the estimate and the risk tier. Defer to US-7.10 territory.
- **Option C (block the story pending AC rewrite)** — only the literal string in AC-7.1.1 is problematic; the *intent* (run real DPO, pass both `chosen` and `rejected`) is implementable today via Option A. The AC needs one word swapped (see §7); not worth blocking on.

### Behavior difference vs. literal AC text

With Option A the implementer-emitted command will contain `--train-mode dpo` (the mlx-lm-lora flag for selecting DPO) and `--train-type lora` (the fork's renamed equivalent of `--fine-tune-type lora`). It will **not** contain the substring `--fine-tune-type dpo`. AC-7.1.1's assertion must be updated to match the real interface — see Open Questions.

---

## 1. Architecture / approach

After this story, `finetune-gemma.py --dpo` performs two sequential `subprocess.run` invocations:

1. **Phase 1 (unchanged):** `python -m mlx_lm lora --fine-tune-type lora ...` against `train.jsonl` (chat-format messages). Produces `adapters.safetensors` at `adapter-path`. This stays on stock `mlx_lm` — no need to migrate the SFT path.
2. **Phase 2 (rewritten):** `python -m mlx_lm_lora.train --train --train-mode dpo --train-type lora --resume-adapter-file <phase1>/adapters.safetensors --data <dpo_prepared> --beta 0.1 --dpo-cpo-loss-type sigmoid ...` against a `dpo_prepared/{train,valid}.jsonl` containing `{prompt, chosen, rejected}` triples (NOT the chat-messages format Phase 2 currently writes). The DPO pass *continues training* the SFT adapter — same `--adapter-path` — using the preference loss.

The "DPO available vs. unavailable" boundary is **a single data-presence check** in `find_dpo_data()` — same as today. The `--dpo` flag (true by default) is intent; the actual gating is "do we have a `dpo/pairs.jsonl` or `dpo_pairs.db` with at least one valid `{prompt, chosen, rejected}` row?" If no, print a warning and skip. If yes, run the real DPO subprocess. `--sft-only` short-circuits the entire Phase 2 regardless of data presence (AC-7.1.4).

Why this shape: it preserves the working SFT path (de-risks the change), keeps the dependency surface small (one new pip package), and the data format swap from "chat messages with chosen only" to "{prompt, chosen, rejected}" is exactly what `prepare_dpo_from_db` already produces — that helper is dead code today that we are now wiring in. The current `run_dpo()` body's SQLite-read-and-write-chat-messages block gets deleted and replaced with a call to `prepare_dpo_from_db()`.

---

## 2. Concrete file plan

| Path | Action | Notes |
|---|---|---|
| `scripts/finetune-gemma.py` | **MODIFY** | Rewrite `run_dpo()` (lines 292–389) to call `prepare_dpo_from_db()` for `.db` sources / copy for `.jsonl` sources, then `subprocess.run([python, -m, mlx_lm_lora.train, --train-mode, dpo, ...])`. Add `--from-corrections` flag pass-through (only affects search path in `find_dpo_data`, used by US-7.2/US-7.5). Add an early-exit branch when no DPO data found that prints `[dpo] no preference data found at <searched paths>; skipping (SFT adapter retained)`. Estimated diff: -90 / +60 LOC. |
| `scripts/finetune-gemma.py::run_dpo` | (sub-change) | Build `cmd` list as a Python list literal so the test can `assertIn` on argv tokens, not on a joined string. |
| `tests/test_finetune_gemma_dpo.py` | **ADD** | New file. 5 tests, one per AC. All mock `subprocess.run` via `unittest.mock.patch` at the module level (`scripts.finetune_gemma.subprocess.run`). No real subprocess, no real `mlx_lm`, no real model. See §4. |
| `tests/fixtures/dpo_pairs_min.jsonl` | **ADD** | 3 rows of `{prompt, chosen, rejected}`, deterministic, no PII. Used by AC-7.1.1 / AC-7.1.4. |
| `tests/fixtures/dpo_pairs_min.db` | **ADD** | SQLite file with the same 3 rows in a `dpo_pairs(prompt, chosen, rejected, margin)` table. Used by AC-7.1.1's DB-path branch. Build once via a `conftest.py` helper if regenerating from a fixture .sql is preferred — either way it must be checked in. |
| `tests/conftest.py` (or new `tests/test_finetune_gemma_dpo.py` setup) | **READ-ONLY-DEP** if exists, else ignore | We don't need a project-level conftest; pytest discovers `tests/test_*.py` adjacent fixtures fine. |
| `scripts/finetune-gemma.py` (importability) | **MODIFY** | The current file uses `-` in its name, blocking `import scripts.finetune_gemma`. Add `__name__ == "__main__"` guarded entry point (already present at line 834) and have the test load the module via `importlib.util.spec_from_file_location("finetune_gemma", "scripts/finetune-gemma.py")`. No rename required — keeps git-blame clean. |
| `scripts/check-lora-ab.sh` | **READ-ONLY-DEP** | Must continue to exit 0. No changes. |
| `scripts/check-lora-baseline.sh` | **READ-ONLY-DEP** | Must continue to exit 0. No changes. |
| `tests/fixtures/lora_baseline_persona.json` | **READ-ONLY-DEP** | Persona fixture consumed by both gate scripts. |
| `tests/fixtures/lora_ab_before.json`, `lora_ab_after.json` | **READ-ONLY-DEP** | A/B fixture pair. |
| `requirements.txt` or `pyproject.toml` (whichever holds Python deps) | **MODIFY** | Add `mlx-lm-lora>=2.1.0` to the optional `[finetune]` extras (or wherever `mlx-lm` is pinned today). If no pinned-deps file exists, add a one-line `pip install mlx-lm-lora` note in `scripts/finetune-gemma.py`'s docstring next to the existing `pip install mlx-lm` line. |

Touched outside scope: **none**. No C files, no vtable interfaces, no `src/ml/dpo.c`, no `src/security/`, no provider code.

---

## 3. mlx_lm_lora.train DPO interface notes (verified)

Sources consulted: `pip show mlx-lm`, `/opt/homebrew/lib/python3.14/site-packages/mlx_lm/lora.py`, `https://raw.githubusercontent.com/ml-explore/mlx-lm/main/mlx_lm/lora.py`, `https://api.github.com/search/issues?q=dpo+repo:ml-explore/mlx-lm+is:pr`, `https://raw.githubusercontent.com/Goekdeniz-Guelmez/mlx-lm-lora/main/README.md`, `https://pypi.org/pypi/mlx-lm-lora/json`, plus the existing `scripts/finetune-gemma.py::find_dpo_data` and `prepare_dpo_from_db`.

**Module entry:** `python3 -m mlx_lm_lora.train` (NOT `mlx_lm`).

**DPO-selecting flags (required):**
- `--train`
- `--train-mode dpo`
- `--train-type lora` (the fork's renamed `--fine-tune-type`; values are still `lora|dora|full`)

**DPO-specific flags:**
- `--beta` (KL penalty, default 0.1)
- `--dpo-cpo-loss-type` (`sigmoid|hinge|ipo|dpop`, default `sigmoid`)
- `--delta` (margin for hinge loss only)
- `--reference-model-path` (optional; defaults to the main model — i.e., the pre-LoRA frozen base — when omitted)

**SFT-passthrough flags accepted unchanged:** `--model`, `--data`, `--adapter-path`, `--resume-adapter-file`, `--iters`, `--batch-size`, `--learning-rate`, `--num-layers`, `--max-seq-length`, `--grad-checkpoint`, `--mask-prompt`, `--save-every`, `--steps-per-report`, `--steps-per-eval`.

**Data format (per-line JSON in `{train,valid}.jsonl`):**
```json
{"prompt": "...", "chosen": "...", "rejected": "..."}
```
- Required keys: `prompt`, `chosen`, `rejected`.
- Optional: `system` (string) injected ahead of `prompt`.
- File naming convention: same as `mlx_lm.lora` — directory passed via `--data` must contain `train.jsonl` (required) and `valid.jsonl` (strongly recommended; mlx-lm-lora will warn or fail if absent).

**Existing helper we now use:** `prepare_dpo_from_db(db_path, output_dir)` at `scripts/finetune-gemma.py:254` already emits exactly this format with a 90/10 train/valid split. We were carrying this dead code; this story finally calls it.

**Pin:** `mlx-lm-lora>=2.1.0,<3` (2.x is the current stable line on PyPI as of 2026-05-16). Pin upper bound to a major version to avoid silent CLI breakage.

---

## 4. Test plan

All Python tests live in `tests/test_finetune_gemma_dpo.py`. The module is loaded via `importlib.util` because `finetune-gemma.py` has a hyphen. All five tests mock `subprocess.run` and assert against the captured `cmd: list[str]`.

**Module load helper (top of test file):**
```python
import importlib.util, pathlib
_p = pathlib.Path(__file__).resolve().parent.parent / "scripts" / "finetune-gemma.py"
_spec = importlib.util.spec_from_file_location("finetune_gemma", _p)
fg = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(fg)
```

**Mock seam:** `unittest.mock.patch.object(fg.subprocess, "run", return_value=SimpleNamespace(returncode=0))`. Capture `mock_run.call_args_list` and inspect `args[0]` (the cmd list).

| AC | Test function | Fixture | Mock seam | Assertion |
|---|---|---|---|---|
| 7.1.1 | `tests/test_finetune_gemma_dpo.py::test_dpo_pass_invokes_real_dpo_subprocess` | `tests/fixtures/dpo_pairs_min.jsonl` staged into a tmp dir as `<tmp>/dpo/pairs.jsonl`, plus a synthetic `<tmp>/train.jsonl` so the SFT phase pre-condition passes | `subprocess.run` (returncode=0 for both phases) | The second `subprocess.run` call's cmd list contains both `"--train-mode"` immediately followed by `"dpo"` AND `"-m"` immediately followed by `"mlx_lm_lora.train"`. Also assert the data dir passed via `--data` contains a `train.jsonl` whose first row has both `"chosen"` and `"rejected"` keys (proves the formatter passes both fields, AC requirement). |
| 7.1.2 | (shell) `bash scripts/check-lora-ab.sh` invoked from CI after a fixture DPO-trained adapter is staged. NOT a Python unit test. | `tests/fixtures/lora_baseline_persona.json`, `tests/fixtures/lora_ab_before.json`, `tests/fixtures/lora_ab_after.json` (all existing) | n/a — real binary | `check-lora-ab.sh` exit 0, parsed `delta >= 0.10` (current FLOOR). **Note:** the AC text says `≥ 0.05 above the SFT-only baseline` — this is a *delta-of-deltas* phrasing that's stricter than the existing gate. See Open Questions; for v1 we satisfy the existing gate (≥0.10 absolute) and document the stricter delta as a US-7.4 / US-7.6 follow-on. |
| 7.1.3 | `tests/test_finetune_gemma_dpo.py::test_dpo_missing_data_nonfatal` | Empty tmp data dir (no `dpo/`, no `dpo_pairs.db` anywhere on the search path). Patch `fg.find_dpo_data` to return `None` for full determinism (avoids accidentally finding a real `convo-training*/dpo_pairs.db` in the repo). | `subprocess.run`, `fg.find_dpo_data` | `run_dpo()` returns 0; `subprocess.run` was called **zero** times for the DPO phase; captured stdout contains the literal substring `"no preference data"` (or `"skipping"` — pick one and pin it). |
| 7.1.4 | `tests/test_finetune_gemma_dpo.py::test_sft_only_skips_dpo_entirely` | Same fixture staging as 7.1.1 (real DPO data present on disk) | `subprocess.run` | With `args.sft_only=True, args.dpo=True`, only ONE `subprocess.run` call is made (the SFT phase); no captured cmd list contains the token `"dpo"` anywhere. |
| 7.1.5 | (shell) `bash scripts/check-lora-baseline.sh` in CI | `tests/fixtures/lora_baseline_persona.json` | n/a — real binary | Exit 0. Unchanged from today; this AC is a regression guard. |
| (bonus) | `tests/test_finetune_gemma_dpo.py::test_from_corrections_flag_resolves_db` | `tests/fixtures/dpo_pairs_min.db` staged into tmp under the `--from-corrections` search path | `subprocess.run` | Phase 2 runs and the JSONL it materialises matches the DB contents. Required for AC-7.2.3 but cheapest to land here while the test file is being authored. |

**Determinism guarantees:**
- No real `subprocess.run` invocations (all patched).
- No network — `mlx-lm-lora` is never imported by the test; the cmd list is a list of strings.
- No real model weights — we never reach training.
- `random.shuffle` in `prepare_dpo_from_db` is seeded via `random.seed(0)` in test setup (add a `random.seed(0)` call inside `prepare_dpo_from_db` guarded by an env var `HU_DPO_DETERMINISTIC=1` set in tests, OR patch `random.shuffle` to a no-op).
- All fixture data is committed; no `~/.human/` reads at test time (test sets `HOME=<tmp>` if any path resolution leaks).

---

## 5. Risk analysis (top 3, likelihood × impact ordered)

1. **(HIGH × MED) The DPO data format Phase 2 currently writes is chat-messages, not `{prompt, chosen, rejected}`.** The current `run_dpo` body builds `{"messages": [{role:system}, {role:user}, {role:assistant}]}`. mlx-lm-lora's DPO loader rejects that and expects flat `{prompt, chosen, rejected}`. The implementer MUST delete the chat-messages writing block (lines 322–346) and route through `prepare_dpo_from_db()` (which already exists and emits the correct format). Mitigation: AC-7.1.1's test asserts the materialised `train.jsonl` first row has `chosen` AND `rejected` keys at top level — this catches the regression at unit-test time before any real training run.

2. **(MED × HIGH) `mlx-lm-lora` is a third-party fork with a single maintainer.** If it becomes unmaintained or its CLI changes incompatibly, the entire DPO path breaks. Mitigation: pin `mlx-lm-lora>=2.1.0,<3` in the deps file; document the dependency in `scripts/finetune-gemma.py`'s module docstring with a one-line note about why we chose it over stock `mlx_lm`; add a follow-on backlog entry for "migrate DPO to stock `mlx_lm` if upstream PR #417 merges." The story explicitly does NOT take on Option B (implementing DPO ourselves), but US-7.10 already plans an internal `hu_rl_trainer_t` vtable — that is the long-term escape hatch.

3. **(MED × MED) `--reference-model-path` defaulting may not produce the expected loss curve when resuming from an SFT-trained adapter.** mlx-lm-lora's default is "main model = reference"; when we `--resume-adapter-file <sft.safetensors>`, the model loaded for the policy *also* has SFT weights, so the reference is no longer the true pre-SFT base. This can produce a near-zero loss gradient (policy ≈ reference) on the first step. Mitigation: pass `--reference-model-path <base-hf-id>` explicitly, set to `resolve_model(args)` (the un-fine-tuned base). This matches the standard DPO recipe (reference = SFT-frozen base, policy = SFT + new adapter being trained). Add this to the implementer's cmd list. (This is the one place where careful reading of the mlx-lm-lora README saved us a confusing first-run debug session.)

**Other risks considered and dismissed:**
- *Backward compat with existing `--dpo` default-true:* unchanged behavior when no DPO data is found (still exits 0, still produces SFT adapter); AC-7.1.3 + AC-7.1.5 cover this explicitly.
- *Concurrency:* `run_dpo` runs sequentially after `run_sft`; no shared mutable state, no threads.
- *Data integrity:* DPO pass only writes to its own `adapter-path`; it `--resume-adapter-file`s but writes to the same path. If Phase 2 fails, Phase 1 adapter is overwritten only on successful save — same crash semantics as today. If we want strict "Phase 2 failure leaves Phase 1 intact" we can `cp -r` the adapter dir to `<adapter>.sft-backup` before Phase 2; **out of scope** for this story unless Seth asks.

---

## 6. Sequencing (implementer steps, dependency-ordered)

1. **Add `mlx-lm-lora>=2.1.0,<3` to the project's Python deps.** If the project uses `requirements.txt`, append the line; if it uses `pyproject.toml`, add to the `[project.optional-dependencies] finetune` (or equivalent) section. **Verify:** `python3 -m pip install mlx-lm-lora` succeeds locally OR the deps file diff lands cleanly.

2. **Author the test fixtures.** Create `tests/fixtures/dpo_pairs_min.jsonl` (3 lines, deterministic, no PII) and `tests/fixtures/dpo_pairs_min.db` (SQLite with one table `dpo_pairs(prompt, chosen, rejected, margin)`, same 3 rows). Commit a small Python snippet in the test file itself as a fallback regenerator if the binary fixture goes stale. **Verify:** `python3 -c "import sqlite3; rows=sqlite3.connect('tests/fixtures/dpo_pairs_min.db').execute('select prompt,chosen,rejected from dpo_pairs').fetchall(); assert len(rows)==3; print('ok')"`.

3. **Write `tests/test_finetune_gemma_dpo.py` with all 5 tests + the bonus `from_corrections` test.** Tests should fail at this point (the production code still does SFT-on-chosen-only). **Verify:** `python3 -m pytest tests/test_finetune_gemma_dpo.py -x -v` shows 5 failing tests with messages indicating the expected `--train-mode dpo` token is missing from the captured cmd list.

4. **Rewrite `run_dpo()` in `scripts/finetune-gemma.py`.** Delete lines 309–353 (the SQLite-read-and-write-chat-messages block plus the JSONL `shutil.copy` branch) and replace with: (a) call `find_dpo_data(data_dir)`; if `None`, log `"[dpo] no preference data found at <searched>; skipping (SFT adapter retained)"` and `return 0`. (b) call `prepare_dpo_from_db(dpo_source, data_dir / "dpo_prepared")` for `.db` paths; for `.jsonl` paths, validate the first row has `prompt`/`chosen`/`rejected` and copy to `data_dir / "dpo_prepared" / "train.jsonl"` (also write an empty-but-valid `valid.jsonl` if absent). (c) build the cmd list invoking `[python, -m, "mlx_lm_lora.train", "--model", model, "--train", "--train-mode", "dpo", "--train-type", "lora", "--data", str(dpo_data_dir), "--adapter-path", str(adapter_dir), "--resume-adapter-file", str(adapter_dir/"adapters.safetensors"), "--reference-model-path", model, "--beta", "0.1", "--dpo-cpo-loss-type", "sigmoid", "--iters", str(dpo_iters), "--batch-size", "1", "--learning-rate", str(dpo_lr), "--num-layers", str(args.num_layers), "--max-seq-length", str(args.max_seq_length), "--grad-checkpoint"]`. **Verify:** `python3 -m pytest tests/test_finetune_gemma_dpo.py -x -v` — all 5 tests + the bonus pass; 0 failures.

5. **Verify the SFT path is untouched.** **Verify:** the existing `tests/test_lora.c`, `tests/test_dpo.c` (pure-C unit tests of `hu_dpo_*` collector APIs that do not exercise the Python script) still pass. Run `./build/human_tests --suite=DPO`. Exit code 0; 0 ASan errors.

6. **Run the two gate scripts to satisfy AC-7.1.2 and AC-7.1.5.** Build the C binary (`cmake --preset dev && cmake --build --preset dev --target human`), then `bash scripts/check-lora-baseline.sh` (AC-7.1.5) and `bash scripts/check-lora-ab.sh` (AC-7.1.2). **Verify:** both print `PASS` and exit 0.

7. **Spawn `/verify` with the AC list.** The verifier agent should re-run steps 4, 5, 6 in isolation and produce `RESULT_verifier=PASS`. **Verify:** `RESULT_verifier=PASS` returned; if INCONCLUSIVE or FAIL, do not close the task — surface to lead.

---

## 7. Open questions for Seth

1. **AC-7.1.1 literal-string assertion.** The AC says `assert the shell command string contains "--fine-tune-type dpo"`. Stock `mlx_lm` cannot produce this string; only the fork `mlx-lm-lora` can, and its flag is spelled `--train-mode dpo` (plus `--train-type lora`). Proposal: amend AC-7.1.1 to read `"... the shell command argv contains both '--train-mode' followed by 'dpo' AND '-m' followed by 'mlx_lm_lora.train', and the materialised train.jsonl rows contain both 'chosen' and 'rejected' top-level keys."` This preserves intent (real DPO is invoked, both fields are passed) without pinning to a non-existent stock-`mlx_lm` flag spelling.

2. **AC-7.1.2 delta semantics.** The AC says `the A/B delta is ≥ 0.05 above the SFT-only baseline`. The existing `check-lora-ab.sh` gate is a single absolute delta (≥0.10 by default). Implementing a *delta-of-deltas* gate (DPO-fidelity-delta minus SFT-only-fidelity-delta ≥ 0.05) requires either two adapter runs in CI or a fixture-based comparison. Proposal: for v1, satisfy the existing absolute gate (the current floor is well above 0.05) and add a US-7.6 follow-on to introduce the delta-of-deltas gate once judgment-PPL lands. If Seth wants the stricter gate in this story, the estimate grows from M to L.

3. **Dependency on `mlx-lm-lora`.** OK to add a third-party fork as a required dep for the local fine-tune path? It's a single-maintainer repo. The alternative (Option B) is implementing DPO ourselves in Python on top of `mlx` primitives — a substantially larger story.

---

## 8. Acceptance criteria mapping

| AC | Covered by |
|---|---|
| AC-7.1.1 | `tests/test_finetune_gemma_dpo.py::test_dpo_pass_invokes_real_dpo_subprocess` (subject to AC-text refinement per Open Question 1) |
| AC-7.1.2 | `bash scripts/check-lora-ab.sh` exit 0 in CI (subject to Open Question 2) |
| AC-7.1.3 | `tests/test_finetune_gemma_dpo.py::test_dpo_missing_data_nonfatal` |
| AC-7.1.4 | `tests/test_finetune_gemma_dpo.py::test_sft_only_skips_dpo_entirely` |
| AC-7.1.5 | `bash scripts/check-lora-baseline.sh` exit 0 in CI |
