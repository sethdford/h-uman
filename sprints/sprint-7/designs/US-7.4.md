# Design for US-7.4 (P1): Raise LoRA rank and expand target modules in `finetune-gemma.py`

> **Sprint:** 7 (Digital Twin via Gemma DPO + Continuous Personalization)
> **Risk tier:** LOW (`scripts/` only; no C; no vtable)
> **Depends on:** US-7.1 (real DPO pass landed via `mlx_lm_lora.train`)
> **Author:** tech-lead
> **Status:** READY

---

## 0. Critical interface finding — read first

There is **no** `--target-modules` / `--lora-targets` / `--lora-layers` CLI flag on `mlx_lm_lora.train`. Target-module selection is passed via the existing **`--lora-parameters`** JSON flag, which the trainer already accepts (verified at `mlx_lm_lora/train.py:1046` `build_lora_config` and at `mlx_lm/tuner/utils.py:85` `linear_to_lora_layers`).

The JSON dict accepts:

- `rank` (int)
- `dropout` (float)
- `scale` (float)
- `keys` (list[str]) — **this is the target-modules field.** When `keys` is absent, `linear_to_lora_layers` auto-discovers all linear/embedding modules in the last `num_layers` blocks. When `keys` is present, only modules whose `named_modules` key matches one of the listed names are converted to LoRA.

The conventional names for transformer attention/MLP projections (Gemma, Llama, Qwen, Mistral) are `q_proj`, `k_proj`, `v_proj`, `o_proj`, `gate_proj`, `up_proj`, `down_proj`. These are the strings the user passes via `--target-modules` (CSV) and we materialize them into the `keys` list inside `--lora-parameters`.

**Implementer types this on the wire (verbatim shape, single argv token after `--lora-parameters`):**

```
--lora-parameters {"rank": 32, "dropout": 0.0, "scale": 10.0, "keys": ["q_proj", "k_proj", "v_proj", "o_proj"]}
```

(Passed as a single Python list element — `json.dumps(...)` — so `subprocess.run` does not split it on spaces. No shell escaping needed; `subprocess.run` receives the JSON string as a single argv slot.)

Note: stock `mlx_lm.lora` (used by Phase 1 SFT) honors the **same** `--lora-parameters` JSON flag — `mlx_lm/lora.py` parses it and routes through the same `linear_to_lora_layers`. So both the SFT cmd (Phase 1, via `mlx_lm.lora`) and the DPO cmd (Phase 2, via `mlx_lm_lora.train`) get the identical JSON token. One code path, two consumers.

---

## 1. Architecture / approach

This story is a **CLI surface + defaults change** in `scripts/finetune-gemma.py`. No behavior change in the trainer side; we are only adjusting what JSON we hand to the existing `--lora-parameters` flag.

Three concrete edits:

1. **Add a `--target-modules` CLI flag** that takes a CSV string (e.g. `q_proj,k_proj,v_proj,o_proj,gate_proj,up_proj,down_proj`), parsed once in `main()` into `args.target_modules: list[str]`. Default when flag is absent: `["q_proj", "k_proj", "v_proj", "o_proj"]` (QKVO — small expansion from today's auto-discovery, which was effectively QV on Gemma's last-N blocks per `mlx_lm`'s discovery output).
2. **Bump 31B `default_rank` from 16 → 32** in `MODEL_TARGETS["31b"]`. Leave e4b/e2b at 32 (out of scope per story).
3. **Materialize the JSON token** in both `run_sft()` and `run_dpo()` cmd lists. Construct via `json.dumps({"rank": args.rank, "dropout": 0.0, "scale": 10.0, "keys": args.target_modules})` and append `["--lora-parameters", <json_str>]` to the cmd list. This **replaces** the implicit auto-discovery — once we pass `keys`, mlx-lm's discovery branch is skipped, so the user gets exactly the seven (or four) modules they asked for, no more, no less.

Also: `version_adapter()`'s `lora_config` dict gains a `target_modules` field, so `train_config.json` records what was actually trained (AC-7.4.5). The existing `rank` field already records the bumped value; no schema migration needed because this file is per-run metadata, not consumed by any reader other than humans.

Why this shape over alternatives:
- **Why not a separate `--lora-keys-json` flag?** Users think in module names, not in JSON. CSV is the friendliest input that maps cleanly to the wire format. The CSV → JSON transformation lives in one place (`main()`), keeping `run_sft`/`run_dpo` free of parsing logic.
- **Why not bump e4b/e2b ranks too?** Out of scope per the story's explicit "Out of scope" line. Edge models have different memory budgets; rank tuning there is a separate experiment.
- **Why not auto-set `keys` based on detected model family?** YAGNI. The four/seven canonical names work for Gemma/Llama/Qwen/Mistral, which is every model in `MODEL_TARGETS`. If someone adds a model with different projection names, they pass `--target-modules` explicitly.

---

## 2. Concrete file plan

| Path | Action | Notes |
|---|---|---|
| `scripts/finetune-gemma.py` | **MODIFY** | (a) `MODEL_TARGETS["31b"]["default_rank"]: 16 → 32`. (b) Add `parser.add_argument("--target-modules", type=str, default=None, help="CSV of LoRA target module names (default: q_proj,k_proj,v_proj,o_proj)")`. (c) In `main()`, after the existing default-resolution block, add `args.target_modules = [s.strip() for s in args.target_modules.split(",")] if args.target_modules else ["q_proj","k_proj","v_proj","o_proj"]`. (d) Add helper `_build_lora_parameters_json(args) -> str` that returns `json.dumps({"rank": args.rank, "dropout": 0.0, "scale": 10.0, "keys": args.target_modules})`. (e) In `run_sft()` cmd list (lines 190–207), append `["--lora-parameters", _build_lora_parameters_json(args)]`. (f) In `run_dpo()` cmd list (post-US-7.1 rewrite), append the same. (g) In `run_finetune()` `lora_config` dict (lines 634–647), add `"target_modules": args.target_modules`. (h) In `run_train_all()` `target_args = argparse.Namespace(...)` (lines 708–729), add `target_modules=None` so default resolution kicks in. (i) In `run_speculative_draft_training()` `draft_args` Namespace (lines 421–441), same. Estimated diff: +25 / -3 LOC. |
| `tests/test_finetune_gemma_modules.py` | **ADD** | New file. 4 tests covering AC-7.4.1, AC-7.4.2, AC-7.4.5, plus one regression guard. Mocks `subprocess.run` and inspects the `--lora-parameters` JSON token in the captured cmd list. See §4. |
| `scripts/check-lora-ab.sh` | **READ-ONLY-DEP** | Must continue to exit 0; produces JSON with `delta` and `size_mb`. No changes. |
| `scripts/check-lora-baseline.sh` | **READ-ONLY-DEP** | Must continue to exit 0 with rank-32 default. No changes. |

Touched outside scope: **none**. No C, no vtable, no `src/`, no `mlx-server.py`, no `prepare-finetune.py`.

---

## 3. mlx-lm-lora target-modules flag — pinned spec

**Flag name on the wire:** `--lora-parameters` (NOT `--target-modules`, NOT `--lora-targets`, NOT `--lora-layers`).

**Argv shape (single token after the flag):**
```
--lora-parameters {"rank":32,"dropout":0.0,"scale":10.0,"keys":["q_proj","k_proj","v_proj","o_proj"]}
```

**Verified sources:**
- `/opt/homebrew/lib/python3.14/site-packages/mlx_lm/tuner/utils.py:85` — `if (keys := config.get("keys", None)) is None: ...` — confirms `keys` is the discovery override.
- `mlx_lm_lora/train.py:108` — `"lora_parameters": {"rank": 8, "dropout": 0.0, "scale": 10.0}` default; the dict is forwarded into `linear_to_lora_layers` via `build_lora_config` at line 1046.
- `mlx_lm_lora` README (Configuration section, fetched 2026-05-16): `--lora-parameters '{"rank": 8, "dropout": 0.0, "scale": 10.0}'` — same JSON dict; `keys` is supported but undocumented (it falls through to `mlx_lm`'s parser).
- `--num-layers` is **orthogonal** and still controls how many of the last N transformer blocks are touched; we leave it sourced from `MODEL_TARGETS[target]["default_num_layers"]` unchanged.

**Parsing note for the implementer:** Construct via `json.dumps(...)`, not f-string concatenation. `subprocess.run([..., "--lora-parameters", json_str, ...])` passes the entire JSON as one argv slot. No quoting needed (the shell is never involved — `subprocess.run` does not invoke `sh` unless `shell=True`, which we never use).

**`num_layers` interaction:** With `keys=["q_proj","k_proj","v_proj","o_proj"]` and `num_layers=8`, only the last 8 blocks get those four projections wrapped — same as today, just with O added. With seven modules and the same `num_layers`, the adapter is roughly `7/2 = 3.5×` larger plus the rank-2× from 16→32 — expect 31B adapter file size to grow from today's baseline by roughly `2 × 3.5 ≈ 7×` if the user opts into the full seven-module config. AC-7.4.3 explicitly does not gate on the direction of the delta, so this is benchmark data, not a regression.

---

## 4. Test plan

All tests in `tests/test_finetune_gemma_modules.py`. Module loaded via `importlib.util` (same seam pattern as `tests/test_finetune_gemma_dpo.py` from US-7.1).

**Module load helper (shared with US-7.1's test file — copy-paste, do not refactor a shared conftest in this story):**
```python
import importlib.util, pathlib, json
from types import SimpleNamespace
from unittest.mock import patch

_p = pathlib.Path(__file__).resolve().parent.parent / "scripts" / "finetune-gemma.py"
_spec = importlib.util.spec_from_file_location("finetune_gemma", _p)
fg = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(fg)
```

**Mock seam:** `patch.object(fg.subprocess, "run", return_value=SimpleNamespace(returncode=0))`. Capture `mock_run.call_args_list[i].args[0]` as the cmd list. Find the `--lora-parameters` token, take the next slot, `json.loads` it, assert against the dict.

| AC | Test function | Argv built via | Assertion |
|---|---|---|---|
| 7.4.1 | `test_target_modules_propagated_to_mlx_cmd` | `args = SimpleNamespace(target="31b", model=None, data=<tmp>, adapter_path=None, iters=10, batch_size=1, learning_rate=1e-6, rank=64, num_layers=8, max_seq_length=2048, target_modules=["q_proj","k_proj","v_proj","o_proj","gate_proj","up_proj","down_proj"], steps_per_report=5, steps_per_eval=20, save_every=20, mask_prompt=True, resume=False)`; call `fg.run_sft(args, <tmp_data>, <tmp_adapter>)` | `cmd = mock_run.call_args.args[0]`; `idx = cmd.index("--lora-parameters")`; `params = json.loads(cmd[idx+1])`; `assert params["rank"] == 64`; `assert params["keys"] == ["q_proj","k_proj","v_proj","o_proj","gate_proj","up_proj","down_proj"]` |
| 7.4.2 | `test_31b_default_rank_32_default_modules_qkvo` | Build argv via `fg.main()` style: invoke `argparse` programmatically with `["--target", "31b", "--data", str(tmp), "--iters", "1"]` and then call `fg.run_sft(args, ...)`. Easier path: assert directly on `MODEL_TARGETS["31b"]["default_rank"] == 32`, then call the helper `fg._build_lora_parameters_json(SimpleNamespace(rank=32, target_modules=["q_proj","k_proj","v_proj","o_proj"]))` and `json.loads` the result. | `params["rank"] == 32` AND `params["keys"] == ["q_proj","k_proj","v_proj","o_proj"]` |
| 7.4.3 | (shell, CI) `bash scripts/check-lora-ab.sh` | n/a — real binary; CI invocation | Exit 0; output JSON has both `"delta"` and `"size_mb"` keys (regardless of value). Validate with `jq -e '.delta != null and .size_mb != null'`. |
| 7.4.4 | (shell, CI) `bash scripts/check-lora-baseline.sh` | n/a | Exit 0 unchanged. |
| 7.4.5 | `test_train_config_records_target_modules` | Stage a tmp adapter dir, build minimal `lora_config = {"target": "31b", "rank": 32, "target_modules": ["q_proj","k_proj","v_proj","o_proj"], ...}`, call `fg.version_adapter(adapter_dir, lora_config)`, then read back `versioned / "train_config.json"`. | Parsed JSON contains `"target_modules": ["q_proj","k_proj","v_proj","o_proj"]` AND `"rank": 32`. |
| (regression) | `test_dpo_cmd_also_carries_lora_parameters` | Same fixture staging as US-7.1's `test_dpo_pass_invokes_real_dpo_subprocess`; call `fg.run_dpo(args, adapter_dir)` with non-empty DPO data on disk. | The second `subprocess.run` call (the `mlx_lm_lora.train` one) ALSO contains `--lora-parameters` with the same JSON shape. Catches the easy mistake of plumbing the flag through SFT only. |

**Determinism guarantees:**
- All `subprocess.run` calls are mocked; no real `mlx_lm` / `mlx_lm_lora` import at test time.
- No filesystem reads outside `tmp_path`; tests do not read `~/.human/`.
- JSON key ordering: `json.loads` then assert on dict equality (order-insensitive) — do NOT assert on the raw JSON string, since `json.dumps` key order is insertion-order in 3.7+ but the assertion shouldn't depend on it.

---

## 5. Risks (top 3)

1. **(MED × MED) Default-rank bump from 16 → 32 changes adapter file size and must be recorded in `version_adapter()` metadata.** Without the `target_modules` + bumped `rank` fields in `train_config.json`, a downstream consumer (US-7.5 nightly cron's promotion-gate, or `check-lora-ab.sh`'s size-tracking) cannot tell whether a regression in `size_mb` is a true regression or just "we changed the default." AC-7.4.5 already mandates `target_modules`; the implementer must also confirm the existing `"rank": args.rank` line at line 638 picks up the new 32 value (it does — `args.rank` defaults from `MODEL_TARGETS[target]["default_rank"]` at line 822 — but verify in the test). **Mitigation:** the `test_train_config_records_target_modules` test reads back the JSON and asserts both fields; add an assertion that `train_config["rank"]` equals whatever the implementer claims is the new default (32 for 31b).

2. **(MED × MED) Passing `keys` overrides mlx-lm's auto-discovery completely.** If a user passes `--target-modules q_proj` (typo or experiment), the resulting adapter trains ONLY the Q projections — much smaller than today's auto-discovered set, and probably under-fit. This is a footgun, not a bug. **Mitigation:** print the resolved `target_modules` list and the `--lora-parameters` JSON in the existing `run_finetune()` banner (lines 583–603) so the user sees what they asked for before training starts. Add `print(f"  Target modules: {','.join(args.target_modules)}")` next to `print(f"  LoRA rank: {args.rank}")`. No new flag to disable; user can always re-run without `--target-modules` to get the QKVO default.

3. **(LOW × MED) JSON token shape brittleness with subprocess argv.** If anyone refactors `run_sft`/`run_dpo` to use `shell=True` or `" ".join(cmd)`, the JSON's embedded spaces and braces break the shell parser. **Mitigation:** the regression-guard test asserts the JSON appears as a SINGLE argv element via `cmd.index("--lora-parameters")` + `cmd[idx+1]`. Any future refactor that joins the cmd into a shell string will fail this test immediately. Also: do not f-string the JSON into a longer argv element ("--lora-parameters={json}" is technically accepted by argparse but visually noisy and breaks the indexing test); keep flag and value as two separate list slots.

**Dismissed risks:**
- *Backward compat with existing trained adapters:* none. Old adapters at rank 16 still load fine; the rank field in `train_config.json` distinguishes them. No migration needed.
- *Performance regression in training time:* rank 32 + 4 modules ≈ 2× compute vs. rank 16 + auto-discovered (~2 modules). Out of scope for AC; this is the deliberate quality/cost knob the story exists to expose.
- *Concurrency / data integrity:* zero shared mutable state; pure CLI-argv construction.

---

## 6. Sequencing (implementer steps)

1. **Bump `MODEL_TARGETS["31b"]["default_rank"]` from 16 to 32.** One-line change. **Verify:** `python3 -c "import importlib.util; s=importlib.util.spec_from_file_location('fg','scripts/finetune-gemma.py'); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); assert m.MODEL_TARGETS['31b']['default_rank']==32; print('ok')"`.

2. **Add the `--target-modules` argparse flag, default resolution in `main()`, and the `_build_lora_parameters_json()` helper.** **Verify:** `python3 scripts/finetune-gemma.py --help | grep -- "--target-modules"` shows the flag.

3. **Plumb `--lora-parameters` into `run_sft()` cmd list.** Append `["--lora-parameters", _build_lora_parameters_json(args)]` to the cmd list before the `--grad-checkpoint` slot. **Verify:** test `test_target_modules_propagated_to_mlx_cmd` passes.

4. **Plumb the same `--lora-parameters` token into `run_dpo()`'s post-US-7.1 cmd list.** **Verify:** regression test `test_dpo_cmd_also_carries_lora_parameters` passes.

5. **Add `"target_modules": args.target_modules` to the `lora_config` dict in `run_finetune()`.** Also confirm the existing `"rank": args.rank` line writes the new 32 value. **Verify:** test `test_train_config_records_target_modules` passes.

6. **Update `run_train_all()` and `run_speculative_draft_training()` Namespace builders** to include `target_modules=None` (so the default resolution path picks up QKVO). **Verify:** `python3 scripts/finetune-gemma.py --train-all --data <tmp>` does not raise `AttributeError`. (Smoke-only; the actual training is mocked in CI.)

7. **Add the banner-print line in `run_finetune()`** so the operator sees the resolved target modules before training. No test — visual only. **Verify:** `python3 -m pytest tests/test_finetune_gemma_modules.py -x -v` — all 5 tests + 1 regression guard pass.

8. **Run the two gate scripts** to satisfy AC-7.4.3 and AC-7.4.4. **Verify:** `bash scripts/check-lora-baseline.sh` exit 0; `bash scripts/check-lora-ab.sh` exit 0 with valid `delta`/`size_mb` JSON output. Spawn `/verify` with the AC list; require `RESULT_verifier=PASS` before closing the task.

---

## 7. Open questions

1. **Should the `--target-modules` default include `gate_proj,up_proj,down_proj` (MLP) by default, or only QKVO?** Story text says "default target modules stay `q_proj,k_proj,v_proj,o_proj` (a small expansion from Q+V to QKVO)" — proceeding with QKVO. If Seth wants MLP included by default, swap the default list in step 2; the test fixture changes correspondingly. No design impact.

2. **Should we add a CI smoke test that asserts the JSON token survives `subprocess.run`'s argv handling end-to-end** (e.g., spawn a tiny Python child that prints `sys.argv` and assert the JSON round-trips)? Worth considering as a one-time sanity check but not part of this story unless requested — `subprocess.run` argv semantics are well-defined and the mocked unit tests prove the construction shape.

3. **AC-7.4.3 currently says "exit code 0 regardless of which wins — this story measures, not gates".** Confirmed reading. No question; just flagging that if `check-lora-ab.sh` is wired into the US-7.5 nightly cron as a hard gate, the rank-64/7-module experiment must not be run through that gate path during data collection. Out of scope here.

---

## 8. Acceptance criteria mapping

| AC | Covered by |
|---|---|
| AC-7.4.1 | `tests/test_finetune_gemma_modules.py::test_target_modules_propagated_to_mlx_cmd` |
| AC-7.4.2 | `tests/test_finetune_gemma_modules.py::test_31b_default_rank_32_default_modules_qkvo` |
| AC-7.4.3 | `bash scripts/check-lora-ab.sh` in CI; JSON shape asserted via `jq -e '.delta != null and .size_mb != null'` |
| AC-7.4.4 | `bash scripts/check-lora-baseline.sh` in CI; exit 0 |
| AC-7.4.5 | `tests/test_finetune_gemma_modules.py::test_train_config_records_target_modules` |
