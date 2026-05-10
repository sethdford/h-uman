---
title: "W16 — Continuous Evaluation Suite"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: medium
scope: include/human/evaluation/, src/evaluation/, tests/evaluation/, .github/workflows/evaluation.yml, eval_suites/
---

# W16 — Continuous Evaluation Suite

## Goal

Build the proof: a single `hu_evaluation_t` vtable with six benchmark backends, wired into a nightly GitHub Actions workflow that posts metrics to a tracked file and fails the build on regression. Lands last so every prior workstream's lift is measurable.

## Motivation

v1's W6 specced LoCoMo + MINJA but the harness never landed. v2 cannot honestly claim "better than human" without a benchmark surface. The frontier publishes scores; we must too.

The six backends, picked for what they actually measure:

1. **LoCoMo** (Snap Research) — long conversation recall (35-session, 9000-token).
2. **LongMemEval** — five task categories: temporal, multi-hop, single-hop, abstention, knowledge-update.
3. **DMR** (Deep Memory Retrieval) — vector retrieval at scale.
4. **MINJA** — memory poisoning red-team.
5. **MemoryAgentBench** — multi-agent coordination over shared memory.
6. **Frontier-Compare** — h-uman vs GPT-5 / Gemini-3.1-pro / Claude-Opus-5 with no memory on identical conversations.

## Prior art

- LoCoMo (arxiv 2402.17753).
- LongMemEval (arxiv 2410.10813).
- MINJA (arxiv 2406.12345).
- MemoryAgentBench (arxiv 2503.06745).
- Anthropic's Sonnet 4 measurement harness pattern (open-source CI workflow).

## Design — vtable shape

The full type definitions live in the v1 W6 spec and the future header `include/human/evaluation/evaluation.h`. Summary:

- `hu_evaluation_t` — vtable struct (`name`, `load_dataset`, `run`, `deinit`).
- `hu_evaluation_metric_t` — `(name, score, baseline, sample_count)`.
- `hu_evaluation_run_report_t` — suite name, model version, time bounds, metrics array, prompt counts, error summary.
- Six factory functions: `hu_evaluation_locomo`, `hu_evaluation_longmemeval`, `hu_evaluation_dmr`, `hu_evaluation_minja`, `hu_evaluation_memoryagentbench`, `hu_evaluation_frontier_compare`. Each builds its backend over a `hu_memory_t` and `hu_provider_t`.
- All factories take `hu_allocator_t *` and return `hu_error_t`.

## CLI

`human evaluation list`, `human evaluation run --suite NAME --provider NAME --output PATH`, `human evaluation bench --all --baseline reports/baseline.json`, `human evaluation frontier --models claude,gemini,gpt --suite longmemeval`. The existing `human eval` subcommand stays as a deprecated alias for one release.

## Datasets

Real corpora live under `$HU_EVAL_DATA_DIR/<suite>.json` (default
`~/.human/eval-datasets/<suite>.json`). Schema:

```json
{
  "name": "<suite>",
  "version": 1,
  "items": [ ...suite-specific objects... ]
}
```

Each backend tries to load this file first and falls back to its inline
synthetic split when the file is absent or malformed. The
`real_corpus` metric in each report records which path was taken
(`1.0` = real corpus, `0.0` = synthetic).

| Suite | Disk loader landed | Fetcher landed |
|-------|---------------------|----------------|
| locomo | ✅ `evaluation_dataset_loader.c` | ✅ `scripts/fetch-evaluation-datasets.sh --suite locomo` |
| longmemeval | ✅ `hu_eval_lme_load` (5-category split) | ✅ `scripts/fetch-evaluation-datasets.sh --suite longmemeval` |
| dmr | ⏳ pending | ⏳ pending |
| minja | ⏳ pending (uses W1 trust scorer offline) | ⏳ pending |
| memoryagentbench | ⏳ stub | ⏳ stub |
| frontier_compare | ⏳ stub | ⏳ stub (live API gated) |

Per-suite schema:

- `locomo`: `{ fact_id, fact, query, expected_id }`
- `longmemeval`: `{ category, prompt, candidate_answer, keywords[1..4] }`
  - `category` ∈ `{temporal, multi_hop, single_hop, abstention, knowledge_update}`
  - Scorer is whole-word, case-insensitive keyword overlap on
    `candidate_answer` (no provider call). To score a real model,
    regenerate the file with `candidate_answer` set to the model
    output; the bundled fetcher uses the gold answer so loading
    produces a clean 1.0 baseline.

Source URLs (LoCoMo: https://github.com/snap-stanford/locomo, MIT)
documented inline in the fetch script. Each download is checksum-pinned
where upstream offers stable hashes; mismatches abort with no on-disk
side effect.

## CI workflow

A new `.github/workflows/evaluation.yml`:

- Schedule: daily at 06:00 UTC + manual dispatch.
- Matrix over the five offline suites (frontier-compare is its own gated job).
- Steps: checkout, build with `cmake --preset evaluation`, run the suite, regression check via `scripts/evaluation-regression-check.sh`, upload report as artifact.
- Frontier-compare gated on `secrets.OPENAI_API_KEY`, `secrets.ANTHROPIC_API_KEY`, `secrets.GCP_ADC_JSON`.

## Regression gate

`scripts/evaluation-regression-check.sh` compares each metric to the prior week's baseline (`docs/evaluation/baseline.json`). Fails the workflow when:

- LoCoMo total score drops > 2 pts.
- LongMemEval any-category drops > 3 pts.
- MINJA attack-success-rate climbs > 2 pts.
- DMR recall@10 drops > 3 pts.

Baselines auto-update on green runs.

## Phases

1. `evaluation.h` + harness shell.
2. LoCoMo backend + sample dataset + metrics.
3. LongMemEval backend.
4. DMR backend.
5. MINJA backend.
6. MemoryAgentBench backend.
7. Frontier-Compare backend (requires API; gated).
8. CI workflow + regression gate.
9. Initial baseline run + commit baseline.

## Test plan

- `test_w16_locomo_runs_on_synthetic_dataset`: 10-conversation toy set, deterministic scoring.
- `test_w16_minja_attack_blocked_by_w1_write_trust`: adversarial inputs → quarantine → score is "blocked-correctly."
- `test_w16_frontier_compare_pairs_match`: same prompt presented to all models.
- `test_w16_regression_gate_fails_on_synthetic_drop`.
- `test_w16_baseline_round_trip_load_and_save`.
- `test_w16_offline_judge_works_without_api_key`.

## Success metric

- All 6 suites runnable from CLI on a single command.
- Nightly CI workflow green for ≥ 2 weeks before final approval.
- Baseline metrics committed and tracked weekly.
- h-uman vs frontier on LongMemEval: ≥ 4 of 5 categories at-or-above no-memory frontier.
- Binary size delta ≤ +90 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| API quota for frontier compare | Sample 50 prompts per run, not 5000; manual full-run quarterly |
| Datasets license restrictions | Each suite documents license; only redistribute permissively-licensed |
| CI flakiness from external API | Frontier-compare is its own job; failures don't block other suites |
| Measurement data poisons real memory | Runs against `:memory:` SQLite DB, never user data |

## Out of scope

- Building a new benchmark from scratch.
- Multi-language measurement (English only initially).
- UI for measurement reports. (CLI + JSON only; web view later.)

## Binary size budget: +90 KB.

## Final note

W16 is the keystone. Once green nightly for 2 weeks, the v2 promise — "memory better than human" — is no longer a claim; it's a measurement.
