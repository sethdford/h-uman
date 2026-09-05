---
title: Promotion readiness — seth-m3-outcomes-20260904-212919-glm / -20260905-030710-glm
date: 2026-09-05
status: informational (read-only research, no promotion action taken)
---

## 1. What "promotion" means mechanically here

Three separate, largely unrelated LoRA pipelines exist in this repo. Only the third governs these two adapters.

- **C `hu_lora_nightly_run`** (`src/ml/lora_nightly.c:22-88` config defaults, `:370-420` swap gate, wired at `src/daemon.c:3455`) operates on `~/.human/adapters/vNN` + `~/.human/adapter-current` (confirmed: `adapter-current -> .../v28`, mtime Sep 5 04:00 — it runs nightly). `base_model` defaults empty (`lora_nightly.c:44-65` comment: hardcoding it was wrong 3× in the past) so this path is **export+rotate only, never trains**, and its `gate_verdict_path` default is `~/.human/blind_ab_gate.json` (`:37-38`). **Not the pipeline for our two adapters** — different directory entirely.
- **C `lora_training_runner.c`** (`run_promotion_gate` `:220-368`, post-train `POST /v1/adapters/swap` `:488-513`) is the "frontier" M3 dispatch path, gated on `hu_eval_gate_t` (mt_bench/ifeval/persona-score composite), not `blind_ab_gate.json`. Also unrelated to the GLM nightly-retrain flow.
- **The actual path**: `scripts/nightly-retrain.sh:383-386` calls `scripts/training_loop.py` (`mlx_lm lora` SFT, confirmed in `~/.human/logs/nightly-retrain.log:1152` and `:1286`), writing to `~/.human/training-data/adapters/seth-m3-outcomes-<ts>[-glm]`, explicitly logged "(staged, not promoted)" (`nightly-retrain.sh:384`). `training_loop.py` records the registry row itself via `adapter_registry.record_training` (grep confirms `training_loop.py:1595-1610`) — **`scripts/register_v6_adapter.py` was never run** on either adapter (their registry rows lack the `validation`/`weights`/`human_gate` fields that script adds — compare the plain rows above to the `seth-glm-air-v6-orpo-real-20260802-190128` row, which has `weights.lora_b_nonzero`, `human_gate`, `promoted:false`).

**What must change for a staged adapter to actually serve:**
1. `~/.human/config.json` → `personalization.lora_adapter_path` **and** `mlx_local.adapter_path` (currently both `.../seth-glm-air-v6-orpo-real-20260802-190128` — verified via `python3 -c json.load`).
2. Either (a) `human-serve.sh restart` — `scripts/human-serve.sh` reads `mlx_local.adapter_path` only at process start (`read_config()`, ~line 50-58) — **this stops :8741**; or (b) a live `POST /v1/adapters/swap` (implemented inline in `scripts/mlx-server.py:1-33`, hot-swaps without restart, used by `hu_mlx_admin_swap_adapter` in C and `scripts/m3_outcome_driver.py:412-437`).
3. Ideally `scripts/register_v6_adapter.py --adapter <dir> --log <train log>` first, to record `human_gate`/`weights`/`promoted` provenance — but its `--corpus-manifest` **defaults to `~/.human/training-data/glm-v6-pref/manifest.json`** (`register_v6_adapter.py:67`), an unrelated ORPO-preference manifest (dated Jul 27) — running it unmodified against these SFT adapters would write **wrong corpus provenance** into the registry. A correct manifest for `m3-outcomes.jsonl` does not exist and would need to be authored or the flag pointed elsewhere.

## 2. Which measurements gate promotion, and what exists for these two adapters

Per `.claude/rules/feature-gate-requires-measurement.md`, activation requires a measurement, not a green build. Checked each:

| Gate | Script | State for these adapters |
|---|---|---|
| C promotion-gate file | `~/.human/blind_ab_gate.json` | **Stale & for a different adapter.** Content: `arm.adapter = "seth-glm-air-v5-20260725-093742"`, `timestamp: 2026-07-29`, `effective_verdict: "ADVISORY"`. File mtime Sep 2 06:56 — older than **both** staged adapters (Sep 4 21:33, Sep 5 03:15), so even if this path were wired to them, `hu_lora_gate_verdict_fresh` (`lora_nightly.c:147-148`) would demote it to ABSENT/STALE (`:384-393`). It isn't wired to them anyway (§1). |
| Classifier-tier gate (AUC) | `scripts/blind_ab/classifier_gate.py` | **Ran twice, refused both times, scored the wrong adapter.** `served_adapter()` (`classifier_gate.py:41-44`) reads `personalization.lora_adapter_path` from config.json when `--adapter` isn't passed — and `nightly-retrain.sh:441` calls it with `--cycle-dir "$CYCLE_DIR" --in-window`, **no `--adapter` flag**. Confirmed: `~/.human/logs/classifier-gate-2026-09-05.report.json` has `"adapter": ".../seth-glm-air-v6-orpo-real-20260802-190128"` — the **served** v6 adapter, not either staged one. Both nights' runs additionally **failed outright**: 09-04 21:34 crashed (`ValueError: [logsumexp] Received empty array`, `nightly-retrain.log:1236`), 09-05 03:18 produced a report but "FATAL: report has no auc_oriented; no gate file written" (`:1466`) — no gate file was written on either night. |
| LUAR authorship | `~/.human/logs/luar-authorship-2026-09-02.json` | **Stale, predates both adapters** (Sep 2 vs. trained Sep 4/Sep 5). AUC 0.782 recorded there is not about these adapters. |
| Fidelity nightly | `~/.human/logs/eval-fidelity-nightly-latest.json` | `verdict: "DEFERRED"`, "generation is failing, not the adapter", exit_code 2 (timestamp Sep 5 05:25) — **no signal produced**, and not attributable to either staged adapter specifically. |
| DPO regression / alignment | `~/.human/logs/dpo-training-results.jsonl` | Both rows present (`adapter_id: seth-m3-outcomes-20260904-212919-glm` / `-20260905-030710-glm`) but `"alignment_score": null` for both — **not measured**. |
| Blind A/B human sheet | `~/blind_ab_run`, `~/.human/blind_ab_human/*` | Grepped both trees for either adapter id: **zero hits**. No human rating sheet has ever been built against them. |

**Conclusion: neither adapter has a single passing (or even completed) measurement of any kind.** Every gate either refused, scored the wrong artifact, or is stale.

## 3. Are the two adapters functionally identical?

Same corpus size (n_pairs=239, same `n_pairs_by_source.outcomes: 239`), same hyperparameters (`adapter_config.json`: rank 8, scale 2.0, iters 500, lr 1e-05, num_layers 8 — byte-identical between the two files except for `adapter_path`/`config`/`data` tmpdir paths), train_loss 2.943→2.942 (noise-level difference), val_loss identical **3.438** for both. **Not weight-identical**: `sha256sum` of `adapters.safetensors` differs —
`88e4ab11...780c3f` (0904) vs `a59f6b95...56efb` (0905). So: same recipe, same corpus snapshot, materially the same measured loss, but genuinely two independent training runs (different Adam noise / data shuffling under the same seed=42 but a different temp data dir each run) — not a re-run producing a bitwise duplicate. Practically interchangeable; the later one (0905-030710) is the natural pick since it also saw a couple more days of corpus growth potential (though `n_pairs` came out identical at 239).

## 4. Ready-to-run checklist (in order)

```bash
# 1. Baseline check — confirm nothing changed under you (read-only)
python3 -c "import json; print(json.load(open('/Users/sethford/.human/config.json'))['personalization'])"

# 2. Run the classifier gate EXPLICITLY against the candidate (not the served default).
#    Requires serving stopped (loads its own base+adapter pair) — STOPS :8741.
launchctl bootout gui/$(id -u)/ai.human.mlx-server
python3 scripts/blind_ab/classifier_gate.py --cycle-dir ~/blind_ab_run \
  --adapter ~/.human/training-data/adapters/seth-m3-outcomes-20260905-030710-glm --in-window
# refuses if n<20 trials, or (as seen twice already) can crash/refuse outright — read output.

# 3. Fidelity nightly against the candidate specifically (check its --adapter/--candidate flag
#    before running; do NOT let it silently resolve the served adapter as "base").
python3 scripts/eval_fidelity_nightly.py --adapter .../seth-m3-outcomes-20260905-030710-glm ...

# 4. Build a fresh blind-A/B human sheet with real Seth pairs vs this candidate
#    (see the blind-ab-pipeline skill) — the ONLY gate with a human in the loop.
#    This is the slow step (rating latency), and the one nothing above substitutes for.

# 5. Register provenance (fix --corpus-manifest first, see §1 item 3 — do not use the default):
python3 scripts/register_v6_adapter.py \
  --adapter ~/.human/training-data/adapters/seth-m3-outcomes-20260905-030710-glm \
  --log ~/.human/logs/nightly-retrain.log \
  --corpus-manifest <a manifest describing m3-outcomes.jsonl — does not exist yet, must be authored>
# refuses (exit non-zero) if scale != 2.0 or lora_b all-zero — both already satisfied here.

# 6. Only after (2)-(4) show a genuine PASS: point config.json at the new adapter, then EITHER
launchctl kickstart -k gui/$(id -u)/ai.human.mlx-server        # STOPS+restarts :8741, OR
curl -X POST http://127.0.0.1:8741/v1/adapters/swap \
  -H 'Content-Type: application/json' \
  -d '{"adapter_path":"/Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260905-030710-glm/adapters.safetensors"}'
# (b) is a hot-swap and does NOT stop :8741; (a) does.
```
Steps 2 and the training itself are the only ones in this whole flow that stop :8741; step 6b does not.

## 5. Risks

- **LoRA scale**: both `adapter_config.json` show `scale: 2.0` — satisfies `.claude/rules/lora-scale-default-or-die.md`. Not a risk.
- **Feedback loop — partially guarded, one open question.** `scripts/m3_outcome_driver.py:206-244` (`select_training_outcomes`) explicitly filters to `adapter_id == 0` (comment at `:222-223`: "prevents the self-amplification feedback loop where each adapter overfits to its predecessor") — by design the corpus should exclude any output generated *while* an adapter was live. **However**, direct inspection of `~/.human/training-data/m3-outcomes.jsonl` (348 rows, all `"src":"production_outcomes"`) shows timestamps (field `t`, epoch-ms) spanning 2026-05-24 through **2026-09-04 22:24 UTC** — i.e. **35 of 348 rows postdate the v6 adapter's promotion** (2026-08-02 20:14 UTC per `registry.json`'s promotion evidence string), during which :8741 has continuously served `personalization.lora_adapter_path = seth-glm-air-v6-orpo-real-...` per current config. If those 35 rows' `adapter_id` field is genuinely 0 despite v6 being configured, that's consistent with the memory note `serving_adapter_inert_on_8741` (LoRA never actually injected into the forward pass on this server) — **UNCONFIRMED**: I did not trace where `outcome.adapter_id` is populated at generation time (`src/ml/m3_frontier_adapter.c` only shows it being serialized, not set) to prove the 35 rows were literally base-model output rather than a bookkeeping gap. Flag for a human to check before trusting the corpus is loop-free.
- **Corpus size mismatch with the task framing**: the task described "the same 313-row corpus" — 313 is exactly the row count *before* the v6 promotion cutoff; the live file now has 348 (35 grew since), while both training runs report `n_pairs: 239` (resolved/deduped from raw rows, per `training_loop.py`'s `resolve_hashes_against_db`). None of these three numbers (313 / 348 / 239) are inconsistent with each other, just different stages of the same pipeline — worth knowing which one a rater actually saw.
- **No measurement at all (see §2)** is itself the dominant risk: promoting either adapter today would be an `HU_LORA_ALLOW_UNMEASURED_PROMOTION=1`-equivalent decision, exactly what `.claude/rules/no-number-without-a-measurement.md` and `feature-gate-requires-measurement.md` exist to block.
