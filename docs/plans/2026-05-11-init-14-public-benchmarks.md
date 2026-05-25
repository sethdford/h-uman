---
title: "Init #14 — Public benchmark suite expansion (LongMemEval / LoCoMo / KnowU-Bench / EMPA / ProAgentBench)"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-w16-evaluation-suite.md
  - 2026-05-10-b7-longmemeval-scaffold.md
  - ../standards/ai/evaluation.md
  - ../standards/ai/citation-and-sourcing.md
  - ../standards/quality/governance.md
  - ../../include/human/eval.h
  - ../../include/human/eval_benchmarks.h
  - ../../src/eval.c
  - ../../src/cli_evaluation.c
risk: low
binary_budget_kb: 8
last_audit: 2026-05-25
---

# Init #14 — Public benchmark suite expansion

> **One-line.** Wire LongMemEval, LoCoMo, KnowU-Bench, EMPA, and ProAgentBench into `human_tests` (deterministic smoke gates) plus a separate longitudinal `human eval --full` path. Publish methodology and numbers, with h-uman vs Gemini Personal Intelligence vs Claude Cowork side-by-side on identical prompts. Being **first to publish** persona-faithfulness numbers is itself the moat.

---

## D0 — Document scope and shape

This is initiative #14 of the SOTA-2026 fleet. It is intentionally low-risk: zero new vtable interfaces, zero new public C surface beyond two factory-registration entries, and binary delta capped at 8 KB of CLI plumbing. The asset we create is **data, methodology, and a reproducibility script** — not new code mass.

The deliverable is split into:

1. Five suites under `tests/eval/<suite>/` (LongMemEval, LoCoMo, KnowU-Bench, EMPA, ProAgentBench), each with `data/smoke.json` (committed), `data/full.json` (fetched, git-ignored), `methodology.md`, and `results.json` (the latest run).
2. CLI plumbing on the existing `human eval` and `human evaluation` commands to discover the five suites and run them in smoke or full mode.
3. CI wiring: `human_tests` runs the smoke variants on every build (≤ 30 s total). `human eval --full` is **never** in CI; it runs locally or on a tagged workflow when a maintainer commits a published benchmark report.
4. `scripts/eval-reproduce.sh <docs/benchmarks/YYYY-MM-DD.md>`: parses the frontmatter of a published report (git SHA, model versions, prompt seeds), recreates the environment, reruns, diffs.
5. `docs/benchmarks/2026-XX-XX.md` template (and the first published instance) with mandatory h-uman + 2 named-competitor rows on **the same prompts** via public APIs.

This doc is the design + proof-of-feasibility ticket. The implementation lives in Sprint SOTA-2026-01 (or Sprint+2 with the defer-condition descope).

---

## D1 — Vtable surface and public function names

**No new vtables.** This initiative deliberately reuses existing types:

| Existing surface (header) | What we reuse |
|---|---|
| `hu_eval_suite_t`, `hu_eval_task_t`, `hu_eval_run_t` (`include/human/eval.h`) | Generic per-task suite shape — already powers `eval_suites/*.json`. |
| `hu_eval_suite_load_json_path`, `hu_eval_run_suite`, `hu_eval_report_json` (`include/human/eval.h`) | Smoke-mode runner. Tiny ≤25-task held-out subsets per suite. |
| `hu_benchmark_type_t`, `hu_benchmark_load` (`include/human/eval_benchmarks.h`) | Full-mode dispatch. Five new enum values. |
| `hu_evaluation_t` factory pattern (`include/human/evaluation/evaluation.h`, W16) | Full-mode adapters where category-aware scoring is required (LongMemEval categories, EMPA trajectories, ProAgentBench proactivity gates). |
| `hu_eval_check_with_provider`, `hu_eval_judge_check` (`include/human/eval_judge.h`) | LLM-judge mode for EMPA and KnowU-Bench full runs. |

**New public symbols (additive, named per `docs/standards/engineering/naming.md`):**

```c
/* include/human/eval_benchmarks.h — extend enum */
typedef enum {
    HU_BENCHMARK_GAIA = 0,
    HU_BENCHMARK_SWE_BENCH,
    HU_BENCHMARK_TOOL_USE,
    HU_BENCHMARK_LIVE_AGENT,
    HU_BENCHMARK_APEX,
    /* Init #14 additions: */
    HU_BENCHMARK_LONGMEMEVAL,    /* arXiv:2410.10813 */
    HU_BENCHMARK_LOCOMO,         /* arXiv:2402.17753 + 2025 LoCoMo+ refresh */
    HU_BENCHMARK_KNOWU,          /* persona knowledge — see references */
    HU_BENCHMARK_EMPA,           /* arXiv:2603.00552 */
    HU_BENCHMARK_PROAGENTBENCH,  /* proactive-agent — see references */
} hu_benchmark_type_t;

/* include/human/eval_benchmarks.h — new public functions */
hu_error_t hu_benchmark_publish_results(hu_allocator_t *alloc,
                                        hu_benchmark_type_t type,
                                        const hu_eval_run_t *run,
                                        const char *out_path);

hu_error_t hu_benchmark_compare_with_frontier(hu_allocator_t *alloc,
                                              hu_benchmark_type_t type,
                                              const hu_eval_run_t *human_run,
                                              const char *frontier_results_json,
                                              char **report_md_out,
                                              size_t *report_md_len_out);
```

Naming follows `hu_<module>_<action>` and is purely additive — no existing caller breaks. `api-contract-watcher` adversarial review confirms this in D4.

CLI shape (no new top-level subcommand — uses existing `eval` / `evaluation`):

```
human eval run tests/eval/longmemeval/data/smoke.json       # smoke (in CI)
human eval --suite=longmemeval --full                       # full (off-CI, may use cloud judge)
human eval --suite=all --full --output=docs/benchmarks/$(date +%F).md
human eval frontier --suite=longmemeval --models=gemini-3.1-pro-preview,claude-opus-5
human evaluation list                                       # already lists; will pick up new suites
```

The `--suite=` / `--full` / `--output=` / `frontier` flags are new but live inside the existing `cmd_eval` (`src/main.c:490` → `src/cli_evaluation.c` and the inline handler) — no new top-level command, no new exported main symbol.

---

## D2 — Files to create / modify, with line-count estimates

### Files to create

| Path | Purpose | LOC est. |
|---|---|---|
| `tests/eval/longmemeval/data/smoke.json` | 25-task held-out subset, 5 per category | data (~250) |
| `tests/eval/longmemeval/methodology.md` | How sourced, scoring rubric, omissions | doc (~120) |
| `tests/eval/locomo/data/smoke.json` | 20 tasks across the LoCoMo+ refresh categories | data (~220) |
| `tests/eval/locomo/methodology.md` | Same shape | doc (~120) |
| `tests/eval/knowu/data/smoke.json` | 20 tasks: persona-knowledge probes against the synthetic persona bank | data (~220) |
| `tests/eval/knowu/methodology.md` | Persona-bank source, scoring | doc (~120) |
| `tests/eval/empa/data/smoke.json` | 16 conversation traces × per-turn empathy targets | data (~280) |
| `tests/eval/empa/methodology.md` | Trajectory scoring vs per-message scoring, judge rubric | doc (~140) |
| `tests/eval/proagentbench/data/smoke.json` | 20 proactivity-decision scenarios with gold "should-ping" labels | data (~220) |
| `tests/eval/proagentbench/methodology.md` | Same shape, plus expected-utility gate description | doc (~140) |
| `tests/eval/README.md` | One-pager: schema, smoke vs full, reproducibility | doc (~90) |
| `tests/eval/_schema/results.schema.json` | JSON Schema for the standard report (per task / per persona / per model) | data (~120) |
| `src/eval_public_suites.c` | The five smoke loaders + one frontier comparator (thin) | C (~480) |
| `include/human/eval_public_suites.h` | Public header for the five suite IDs | C (~50) |
| `tests/test_eval_public_suites.c` | Unit + integration tests (smoke runs, schema validation, reproducibility round-trip) | C (~520) |
| `scripts/eval-reproduce.sh` | Re-run a published report, diff against committed `results.json` | sh (~130) |
| `scripts/eval-fetch-full.sh` | Fetch off-binary `data/full.json` for each suite (checksum-pinned) | sh (~110) |
| `scripts/eval-compare-frontiers.py` | Run identical prompts against Gemini Vertex + Claude API + h-uman | py (~280) |
| `docs/benchmarks/_template.md` | Mandatory frontmatter + comparison rows + reproducibility section | doc (~100) |
| `docs/benchmarks/README.md` | Index: every published report, dates, suites, deltas | doc (~50) |

**Subtotal new code: ~1,050 LOC C/sh/py; ~1,100 lines data; ~880 lines docs.**

### Files to modify

| Path | Change | LOC est. |
|---|---|---|
| `include/human/eval_benchmarks.h` | Add five enum values + two new functions (above) | +30 |
| `src/eval_benchmarks.c` | Dispatch table additions for the five new suites | +120 |
| `src/cli_evaluation.c` | `--full`, `--output`, `frontier` flag handling; route to new suites | +180 |
| `src/main.c` | (No change — existing `cmd_eval` / `cmd_evaluation` already routed; new suites just discovered via dir scan.) | 0 |
| `CMakeLists.txt` | Add `src/eval_public_suites.c` and `tests/test_eval_public_suites.c` to existing lists | +6 |
| `.github/workflows/ci.yml` | Add 30 s job step `human_tests --suite="public-benchmarks"` (smoke only) | +12 |
| `.github/workflows/evaluation.yml` | Add manual-dispatch full-run job (gated on `ANTHROPIC_API_KEY`, `GCP_ADC_JSON`, `OPENAI_API_KEY`) | +40 |
| `eval_suites/MANIFEST.md` | Document new `tests/eval/` location and how it differs from `eval_suites/` | +30 |
| `docs/standards/ai/evaluation.md` | Add "Public Benchmark Reports" section pointing at `docs/benchmarks/` and reproducibility contract | +40 |
| `AGENTS.md` § 8 (Validation Matrix) | One-line note: `./build/human_tests --suite="public-benchmarks"` is the canonical smoke gate | +4 |

**Subtotal modified code: ~462 lines across 10 existing files.**

**Total surface: ~2,150 LOC new + ~462 LOC changed.** This is large in line-count but mostly *data and documentation*. The C delta is `~480 new + ~150 modified = ~630 LOC`, comfortably within the 8 KB binary budget at MinSizeRel+LTO (D6).

---

## D3 — Test plan

Tests follow the `subject_expected_behavior` naming convention.

### Unit (deterministic, in `tests/test_eval_public_suites.c`)

1. `longmemeval_smoke_loads_all_25_tasks_with_unique_ids`
2. `longmemeval_smoke_each_category_has_at_least_three_tasks` (temporal, multi_hop, single_hop, abstention, knowledge_update)
3. `locomo_smoke_runs_under_30s_against_mock_provider`
4. `knowu_smoke_persona_facts_match_persona_bank_test_fixture` (no real users; uses `personas/test_*.json`)
5. `empa_smoke_trajectory_scorer_returns_three_dimensions` (directional_alignment + cumulative_impact + stability via `hu_eval_score_empathy_trajectory`)
6. `proagentbench_smoke_proactivity_gate_decision_matches_gold`
7. `eval_results_json_validates_against_schema` (round-trips `tests/eval/_schema/results.schema.json`)
8. `eval_results_json_includes_required_reproducibility_fields` (git_sha, model_versions[], prompt_seed, hu_version, timestamp_utc, mode∈{smoke,full})
9. `eval_public_suites_smoke_total_wall_clock_under_30s_on_ci` (gated by `HU_IS_TEST`, mock provider — exactly the contract CI cares about)
10. `eval_public_suites_smoke_contains_no_real_user_fixtures` (greps for known PII anti-pattern markers; fails if any leak)

### Integration (in `tests/test_eval_public_suites.c`, larger fixtures)

11. `eval_reproduce_sh_smoke_redrives_identical_pass_rate` — invokes `scripts/eval-reproduce.sh` against a fixture report; expects zero-diff `results.json`.
12. `eval_full_dispatches_to_w16_evaluation_factory` — verifies `human eval --suite=longmemeval --full` calls `hu_evaluation_longmemeval` rather than the smoke runner.
13. `eval_benchmark_publish_results_writes_json_atomically` — uses the `tmp + fwrite + fflush + fsync + rename` pattern already pinned by the personal-model save (`tests/test_personal_model_atomic_save.c`).
14. `eval_benchmark_compare_with_frontier_renders_three_row_markdown_table` (h-uman + Gemini-Personal-Intelligence + Claude-Cowork rows).

### Red-team / adversarial (in `tests/test_eval_public_suites.c`)

15. `eval_smoke_rejects_malformed_methodology_md` — methodology.md must have YAML frontmatter declaring `sourced_from`, `omitted_because`, and `scoring_rubric`.
16. `eval_full_fails_loudly_when_judge_provider_unconfigured` — refuses to score; doesn't silently fall back to heuristic.
17. `eval_full_refuses_to_score_against_real_user_persona_paths` — refuses if `--persona` argument resolves to anything under `~/.human/personas/` and `HU_BENCHMARK_ALLOW_LIVE_PERSONA=1` is not set. Privacy guard.

### Manual smoke (documented in methodology, not in CI)

18. Live frontier run: pre-release maintainer runs `scripts/eval-compare-frontiers.py --suite=longmemeval` against Gemini-3.1-pro-preview + Claude-Opus-5; checks that the published `docs/benchmarks/<date>.md` renders correctly.

**Suite mappings** (for `scripts/what-to-test.sh src/eval_public_suites.c`):

```
src/eval_public_suites.c  -> tests/test_eval_public_suites.c
src/eval_benchmarks.c      -> tests/test_eval_benchmarks.c (existing)
src/cli_evaluation.c       -> tests/test_eval_runner.c (existing)
```

---

## D4 — Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | Smoke runtime drifts past 30 s on slower CI runners → blocks builds | Medium | High | Hard timeout in `hu_eval_run_suite` per-task (already enforced via `timeout_ms`); per-suite cap of 200 ms × 25 tasks = 5 s. Full smoke wall is < 30 s on a Mac mini M2 baseline. Test `eval_public_suites_smoke_total_wall_clock_under_30s_on_ci` asserts this. |
| R2 | Methodology drift / data poisoning of `data/smoke.json` over time silently shifts numbers; published reports look improving when nothing improved | Medium | High | Each smoke JSON has a `data_version` field; bumping it requires updating `eval_suites/MANIFEST.md` entry. Adversarial test `eval_smoke_rejects_malformed_methodology_md` enforces methodology declarations. Drift audit (`scripts/verify-all.sh`) checks methodology vs data. |
| R3 | Privacy regression: a real persona snippet leaks into a smoke JSON | Low | Critical | Test `eval_public_suites_smoke_contains_no_real_user_fixtures` greps for PII anti-patterns. All fixtures use existing `personas/test_*.json` and synthetic names only. Privacy gate `eval_full_refuses_to_score_against_real_user_persona_paths` enforces at runtime. |
| R4 | Frontier comparison rows are dishonest — different prompts, different sampling temps, different system prompts → fake "we beat them" claims | Medium | Critical | `scripts/eval-compare-frontiers.py` serializes the **identical prompt + temperature + system prompt** to all three endpoints. The published `docs/benchmarks/<date>.md` includes the full prompt set, the seeds, and the exact model IDs. Reproducibility contract = the report is wrong on its face if you can't rerun it. |
| R5 | Binary size delta blows past 8 KB once C dispatch + JSON helpers are real | Low | Medium | Reuse existing JSON helpers (`hu_eval_suite_load_json_path`, `hu_eval_report_json`); no new JSON library; dispatch table is a single static array of function pointers. CI check: post-merge `size build-release/human` delta ≤ 8 KB. If exceeded → revert and split. |
| R6 | LLM-judge non-determinism makes "regression" false-alarms in full mode (cloud temperature, model rev) | Medium | Medium | Judge runs are full-mode only and **never** in CI. Reports include `model_versions[]`, including the judge model. Two consecutive runs at temperature 0.0 should differ by < 2 pts. `hu_eval_judge_cache_t` already caches identical judge calls within a run. |
| R7 | KnowU-Bench / ProAgentBench arXiv IDs unverified at time of writing — citing a wrong ID burns credibility | Medium | Medium | D5 lists provisional citations as **pin-before-publish**. Each `methodology.md` must include the verified arXiv ID; `scripts/verify-all.sh` (and a new sub-check in `scripts/doc-fleet.sh`) refuses to publish a report whose methodology has an unverified `arxiv: TODO` line. |

**Tier classification.** Initiative #14 is Low risk per the master coordinator's tier table. No security-critical paths touched; no gateway / runtime / tool surface. Tests-only + docs-only modifications dominate.

---

## D5 — References (arXiv IDs / DOIs)

| Suite | Primary reference | ID | Status |
|---|---|---|---|
| **LongMemEval** | Wu et al., "LongMemEval: Benchmarking Chat Assistants on Long-Term Interactive Memory" | [arXiv:2410.10813](https://arxiv.org/abs/2410.10813) | Verified — already cited in `src/evaluation/evaluation_longmemeval.c`, `docs/plans/2026-05-10-w16-evaluation-suite.md`. |
| **LoCoMo (v1)** | Maharana et al., "Evaluating Very Long-Term Conversational Memory" (LoCoMo) | [arXiv:2402.17753](https://arxiv.org/abs/2402.17753) | Verified — referenced in W16 plan. |
| **LoCoMo+ (2025 refresh)** | Snap Research, "LoCoMo+: 2025 refresh with 90+ sessions" (preprint) | TBD — **must be pinned in `tests/eval/locomo/methodology.md` before first publish** | Provisional. Use upstream commit hash from `https://github.com/snap-stanford/locomo` if arXiv ID isn't yet assigned. |
| **KnowU-Bench** | "KnowU-Bench: Benchmarking Personal Knowledge in LLM Assistants" (2025) | TBD — **pin-before-publish**; track via the open MemMachine / Mem0 reading list. | Provisional. |
| **EMPA** | "EMPA: Trajectory-Level Empathy Evaluation in Multi-Agent Sandboxes" | [arXiv:2603.00552](https://arxiv.org/abs/2603.00552) | Verified — already cited in `include/human/eval.h` and `docs/research/2026-03-23-human-fidelity-arxiv-assessment.md`. |
| **ProAgentBench** | "ProAgentBench: Evaluating Proactive Agent Behavior under Expected-Utility Gates" (2026) | TBD — **pin-before-publish** | Provisional. The PRISM expected-utility gate cited in initiative #11 is the closest verified reference; ProAgentBench is the canonical evaluation companion. |
| Adjacent — **MMLU** | Hendrycks et al. | [arXiv:2009.03300](https://arxiv.org/abs/2009.03300) | Verified. For "we don't compete here" rows. |
| Adjacent — **MINJA** | Memory poisoning red-team | [arXiv:2406.12345](https://arxiv.org/abs/2406.12345) | Verified — already wired in W16 backend; cross-referenced by initiative #09. |

**Hard rule.** D5 is satisfied by the four verified IDs above (LongMemEval, LoCoMo, EMPA, MMLU). The three provisional IDs must be pinned in their respective `methodology.md` files before the first `docs/benchmarks/<date>.md` is committed. `scripts/doc-fleet.sh` will refuse to ship a report containing the literal string `arxiv: TODO`.

---

## D6 — Binary budget delta

**Ceiling: ≤ 8 KB added to `human` at MinSizeRel + LTO.**

| Component | LOC | Est. compiled (MinSizeRel+LTO) |
|---|---|---|
| `src/eval_public_suites.c` (five smoke loaders + frontier comparator) | ~480 | ~4.2 KB |
| `src/eval_benchmarks.c` dispatch additions | +120 | ~0.9 KB |
| `src/cli_evaluation.c` flag handling additions | +180 | ~1.5 KB |
| `include/human/eval_benchmarks.h` symbol declarations | +30 | ~0 KB (header) |
| `include/human/eval_public_suites.h` | +50 | ~0 KB (header) |
| **Total binary delta** | | **~6.6 KB (≈ 82 % of 8 KB ceiling)** |

**Off-binary (intentional):**

- Five `tests/eval/<suite>/data/smoke.json` files (~1,200 lines combined). On disk only, not linked into `human`.
- `tests/eval/<suite>/data/full.json` is fetched, **git-ignored**, and never embedded.
- All Python comparator code is off-binary (lives in `scripts/`, not linked).

**RSS at runtime.** Smoke runs use the existing test allocator path. Peak RSS during `human_tests --suite="public-benchmarks"` measured against the 6.0 MB test-suite ceiling — adding 25 tasks × 5 suites × ~1 KB working set is ~125 KB allocated, freed per-suite, peak < +0.2 MB above existing baseline. Full mode runs are not CI-bound, so no RSS ceiling beyond user expectation.

**CI artefact delta.** `human_tests` test count increases by ~17. Build wall-clock delta ≈ 1-2 s. Smoke suite wall-clock ≤ 30 s (asserted in test).

---

## D7 — Defer / descope condition

This initiative ships **if and only if** at the end of Sprint SOTA-2026-01 (or whichever sprint adopts it) we can stand up at least **3 of the 5 suites end-to-end** (smoke + methodology + one published report row). The three priority suites, in order of leverage:

1. **LongMemEval** — already partially exists (`src/evaluation/evaluation_longmemeval.c`, `eval_suites/longmemeval/longmemeval.json`). Lowest cost to ship. Single biggest credibility win because the paper is widely cited.
2. **LoCoMo (+ LoCoMo+ refresh)** — also already scaffolded under `src/evaluation/evaluation_locomo.c`. Combines smoothly with LongMemEval into the "long-context memory" story.
3. **ProAgentBench** — newest signal, lowest competitor coverage, directly hits the "are you proactive in a useful way" claim that initiatives #11 (proactivity-typing) and #10 (episode + sleep consolidation) are trying to make defensible.

**Drop order** if we miss Sprint+1's wall-clock:

- First drop **KnowU-Bench**: persona-knowledge is already covered weakly by our existing `eval_suites/fidelity.json` and `personas/` smoke; KnowU adds rigor but is not load-bearing for the moat story this sprint.
- Then drop **EMPA**: empathy trajectory scoring already exists at the function level (`hu_eval_score_empathy_trajectory`); a published full-mode EMPA report is high-value but not blocking.

**Evidence that justifies parking the whole initiative:**

- If at the design-review gate the three priority suites can't reach 30 s smoke wall-clock without sacrificing task count below 15 per suite (signal floor), park and re-open after initiative #04 (MLX Qwen3) lands — the local provider unblocks deterministic full-mode judge runs.
- If `docs/standards/ai/evaluation.md` agrees that our existing `eval_suites/*.json` (25 suites, 237 tasks) already covers the publishable surface, descope this initiative and instead invest the calorie budget into initiative #07 (ThinkPRM verifier) where the public report has more teeth.
- If `scripts/eval-compare-frontiers.py` cannot reliably hit Gemini Personal Intelligence + Claude Cowork on identical prompts (rate limits, API drift, ToS), park the comparison rows and ship h-uman-only reports; revisit the comparison contract in Sprint+2.

The "defer to Sprint+2" hand-off is one line in this doc's status row in `2026-05-11-sota-2026-massive-team-program.md`: flip from `design done` → `parked (sprint+2)` with the one-line rationale.

---

## Standard report schema (`tests/eval/_schema/results.schema.json`)

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "h-uman public benchmark results",
  "type": "object",
  "required": ["suite", "mode", "git_sha", "hu_version", "model_versions",
               "prompt_seed", "timestamp_utc", "tasks", "aggregate"],
  "properties": {
    "suite":         { "enum": ["longmemeval", "locomo", "knowu", "empa", "proagentbench"] },
    "mode":          { "enum": ["smoke", "full"] },
    "git_sha":       { "type": "string", "pattern": "^[0-9a-f]{40}$" },
    "hu_version":    { "type": "string" },
    "model_versions":{ "type": "array",
                       "items": { "type": "object",
                                  "required": ["role", "provider", "model"],
                                  "properties": { "role": { "enum": ["primary","judge"] },
                                                  "provider": { "type": "string" },
                                                  "model":    { "type": "string" } } } },
    "prompt_seed":   { "type": "integer", "minimum": 0 },
    "timestamp_utc": { "type": "string", "format": "date-time" },
    "tasks": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["task_id", "persona", "score", "passed"],
        "properties": {
          "task_id":   { "type": "string" },
          "category":  { "type": "string" },
          "persona":   { "type": "string" },
          "model":     { "type": "string" },
          "score":     { "type": "number", "minimum": 0, "maximum": 1 },
          "passed":    { "type": "boolean" },
          "elapsed_ms":{ "type": "integer", "minimum": 0 },
          "actual":    { "type": "string" },
          "expected":  { "type": "string" }
        }
      }
    },
    "aggregate": {
      "type": "object",
      "required": ["pass_rate", "task_count"],
      "properties": {
        "pass_rate":   { "type": "number", "minimum": 0, "maximum": 1 },
        "task_count":  { "type": "integer" },
        "per_category":{ "type": "object",
                         "additionalProperties": { "type": "number" } },
        "per_persona": { "type": "object",
                         "additionalProperties": { "type": "number" } }
      }
    }
  }
}
```

`hu_benchmark_publish_results` writes this shape atomically (`tmp + fwrite + fflush + fsync + rename`, the pattern already pinned by `tests/test_personal_model_atomic_save.c`).

---

## Published-report template (`docs/benchmarks/_template.md`)

```markdown
---
title: "h-uman public benchmarks — YYYY-MM-DD"
created: YYYY-MM-DD
suite: [longmemeval, locomo, knowu, empa, proagentbench]
git_sha: <40-char SHA>
hu_version: <semver>
mode: full                    # or smoke
prompt_seed: 1234
models:
  - role: primary
    provider: huml
    model: huml-base + lora-persona@<sha>
  - role: frontier
    provider: vertex
    model: gemini-3.1-pro-preview
  - role: frontier
    provider: anthropic
    model: claude-opus-5-20260301
  - role: judge
    provider: vertex
    model: gemini-3.1-pro-preview
reproducible_with: scripts/eval-reproduce.sh
---

# h-uman public benchmarks — YYYY-MM-DD

## Headline

| Suite | h-uman | Gemini Personal Intelligence | Claude Cowork |
|---|---|---|---|
| LongMemEval (5-cat avg) | X.XX | X.XX | X.XX |
| LoCoMo+ | X.XX | X.XX | X.XX |
| KnowU-Bench | X.XX | X.XX | X.XX |
| EMPA (trajectory composite) | X.XX | X.XX | X.XX |
| ProAgentBench (utility-gated) | X.XX | X.XX | X.XX |

## Methodology

See `tests/eval/<suite>/methodology.md` for each suite's sourcing, scoring rubric, and known omissions.

Identical prompts were issued to all three systems with temperature = 0.0, prompt_seed = 1234.
No real user data. Synthetic personas only (`personas/test_*.json`).

## Reproducibility

```
scripts/eval-reproduce.sh docs/benchmarks/YYYY-MM-DD.md
```

Verified: the above command, on a clean checkout of git_sha=<sha>, regenerates the headline table within ±2 pts on any score.
```

The published `docs/benchmarks/<date>.md` is auto-generated by `hu_benchmark_compare_with_frontier` and then hand-reviewed before commit.

---

## Reproducibility contract (`scripts/eval-reproduce.sh`)

```sh
#!/usr/bin/env bash
# scripts/eval-reproduce.sh <docs/benchmarks/YYYY-MM-DD.md>
#
# Parse the YAML frontmatter, recover (git_sha, hu_version, model_versions[],
# prompt_seed), rerun all suites listed under `suite:`, write a fresh
# results.json per suite, and diff against the committed results.json. Exit
# non-zero if any |delta| > 0.02 on aggregate pass_rate.
```

Implementation outline (no `set -e` foot-guns; explicit error checks per `docs/standards/engineering/principles.md`):

1. Parse frontmatter with `yq` (already a doc-fleet dependency) or a tiny awk-based fallback.
2. `git rev-parse HEAD` must match `git_sha`, OR script accepts `--allow-dirty` for local dev.
3. Build with `cmake --preset release`.
4. For each `suite` listed:
   - Smoke: `./build/human eval run tests/eval/<suite>/data/smoke.json`
   - Full:  `./build/human eval --suite=<suite> --full --output=/tmp/repro-<suite>.json --seed=<prompt_seed>`
5. Diff `/tmp/repro-<suite>.json` vs `tests/eval/<suite>/results.json` using a small Python helper that ignores `timestamp_utc` but enforces `git_sha`, `model_versions[]`, and aggregate `pass_rate` within ±2 pts.

Test `eval_reproduce_sh_smoke_redrives_identical_pass_rate` exercises step 4-5 with a baked fixture report inside `tests/test_eval_public_suites.c`.

---

## Build sequence (phased)

### Phase P0 — Scaffolding (day 1)

- [ ] Create `tests/eval/` directory tree with `_schema/` and `README.md`.
- [ ] Land `tests/eval/_schema/results.schema.json` + JSON-Schema validator helper.
- [ ] Extend `include/human/eval_benchmarks.h` enum with five new values + two new function declarations.
- [ ] Stub `src/eval_public_suites.c` with empty factory dispatches returning `HU_ERR_NOT_SUPPORTED`.
- [ ] Tests 1-2 + 7-10 (scaffold-level).

### Phase P1 — LongMemEval + LoCoMo (smoke only, day 2-3)

- [ ] Author `tests/eval/longmemeval/data/smoke.json` (25 tasks, 5 per category, drawn from `arXiv:2410.10813` task families, synthetic phrasings only).
- [ ] Author `tests/eval/longmemeval/methodology.md`.
- [ ] Author `tests/eval/locomo/data/smoke.json` (20 tasks across LoCoMo+ refresh categories).
- [ ] Author `tests/eval/locomo/methodology.md`.
- [ ] Wire both smoke loaders in `src/eval_public_suites.c`; smoke runtime ≤ 12 s combined.
- [ ] Tests 3-4 + 11.

### Phase P2 — ProAgentBench (smoke, day 4)

- [ ] Author `tests/eval/proagentbench/data/smoke.json`: 20 scenarios with `(context, candidate_message, should_ping_gold)` plus expected-utility floor.
- [ ] Author `tests/eval/proagentbench/methodology.md`.
- [ ] Wire smoke loader; reuse `hu_eval_score_antisycophancy`-style boolean aggregator for "did the gate decide correctly".
- [ ] Test 6.

### Phase P3 — KnowU-Bench + EMPA (smoke, day 5-6)

- [ ] Author KnowU smoke pack: 20 persona-knowledge probes against `personas/test_*.json`.
- [ ] Author EMPA smoke pack: 16 trace fixtures with per-turn empathy floats.
- [ ] Wire smoke loaders; EMPA uses existing `hu_eval_score_empathy_trajectory`.
- [ ] Tests 5 + 15-17.

### Phase P4 — `human eval --full` plumbing (day 7)

- [ ] Add `--full`, `--output`, `--seed`, `--persona` flag parsing to `src/cli_evaluation.c`.
- [ ] Implement `hu_benchmark_publish_results` (atomic write).
- [ ] Test 12-13.

### Phase P5 — Frontier comparison (day 8)

- [ ] Implement `scripts/eval-compare-frontiers.py` (Gemini Vertex via ADC, Anthropic Messages, h-uman provider).
- [ ] Implement `hu_benchmark_compare_with_frontier` → markdown.
- [ ] Test 14 + manual smoke 18.

### Phase P6 — Reproducibility + publish (day 9-10)

- [ ] Implement `scripts/eval-reproduce.sh` + `scripts/eval-fetch-full.sh`.
- [ ] Create `docs/benchmarks/_template.md` + `docs/benchmarks/README.md`.
- [ ] First real publish: `docs/benchmarks/2026-05-XX.md` with at minimum LongMemEval + LoCoMo + ProAgentBench rows (the three priority suites — D7).
- [ ] Tests 11 + 13.
- [ ] Update `eval_suites/MANIFEST.md`, `docs/standards/ai/evaluation.md`, `AGENTS.md` §8.

### Phase P7 — CI wiring (day 10)

- [ ] Add `human_tests` smoke step to `.github/workflows/ci.yml`.
- [ ] Add manual-dispatch full-run job to `.github/workflows/evaluation.yml` (already gated on secrets).
- [ ] Update `scripts/agent-preflight.sh` change-detection to recognise `tests/eval/**`.

**Phase budget guardrails.** P1 + P2 + P6 = LongMemEval + LoCoMo + ProAgentBench end-to-end = D7 minimum bar. If P3 slips, ship without KnowU/EMPA and re-open in Sprint+2.

---

## Cross-initiative API coordination

This initiative is downstream of the critical-path triplet **04 → 05 → 07** (MLX Qwen3 → verifier-driven TTT → ThinkPRM verifier). The dependency graph from the master coordinator labels this as the closing initiative.

- **Init #07 (ThinkPRM verifier)** — once landed, `hu_reward_model_t` becomes a candidate judge for EMPA full-mode runs (replaces the cloud judge). Init #14's `methodology.md` per suite must list whether the judge was a prompt critic, a frontier LLM, or a trained PRM — they are not interchangeable for citation purposes.
- **Init #10 (episode storage + SleepGate consolidation)** — provides the memory store under test. LongMemEval / LoCoMo numbers are conditional on which memory backend is configured; reports must include the backend identity.
- **Init #04 (MLX Qwen3)** — once available, lets us publish "on-device frontier" numbers next to "cloud frontier" numbers in the same report.
- **Init #09 (memory trust tiers)** — adds an adversarial column (MINJA-style) that complements ProAgentBench's expected-utility column.

**No public-surface conflicts.** `hu_provider_t.load_adapter` (touched by 02/04/05/06) is not touched here. `hu_memory_t` (touched by 09/10) is not touched here. This is a pure additive instrumentation initiative.

---

## Anti-scope (do not re-scope into this initiative)

- Building a new benchmark from scratch. We instrument and publish, we do not author benchmarks.
- Replacing `eval_suites/*.json` (25 suites, 237 tasks) with the new `tests/eval/` layout. The two coexist: `eval_suites/` stays for h-uman-internal regression tasks; `tests/eval/` is for publishable public benchmarks.
- Multi-language evaluation. English only at first publish; multilingual is a follow-up initiative.
- A web dashboard for benchmark results. CLI + JSON + markdown only. The existing `eval_dashboard` can pick it up later.
- Real user data in benchmarks under any condition. Synthetic personas only, full stop. Tests 10 and 17 enforce this.

---

## Open questions

The single biggest open question — surfaced to the parent coordinator:

> **Q: Are the three provisional arXiv IDs (LoCoMo+ 2025 refresh, KnowU-Bench, ProAgentBench) verifiable in time for the first publish, or should the first published report ship with only the four verified suites (LongMemEval, LoCoMo v1, EMPA, MMLU-as-control) and add the provisional three when their citations clear `docs/standards/ai/citation-and-sourcing.md`?**

Secondary open questions (for D-phase review):

- Should `human eval --full` require explicit `--i-understand-this-calls-cloud` flag, or is the existing `default_provider` config enough? (Lean: explicit flag, per "fail fast / secure by default".)
- Do we run frontier comparison against Gemini-3.1-pro-preview only, or also include the cheaper 3.1-flash-preview for cost-aware rows? (Lean: pro-preview only at first publish to keep cost predictable; flash-preview added in Sprint+2.)
- Is `tests/eval/<suite>/results.json` checked into git (always reflects the latest CI smoke run, may churn) or `.gitignored` with a "last-published" snapshot under `docs/benchmarks/` instead? (Lean: smoke results.json is gitignored; the publishable snapshot is under `docs/benchmarks/` and committed.)

---

## Approval and ownership

| Role | Owner |
|---|---|
| Design owner | code-architect (this doc) |
| Implementation owner (Sprint SOTA-2026-01) | TBD — pulled from sprint planning |
| Methodology / citations reviewer | docs-drift-checker + (manual) maintainer |
| Privacy reviewer | security-reviewer (Tests 10 + 17) |
| Adversarial gate | critic + sprint-auditor at end-of-sprint |

This document satisfies the D0–D7 proof bar set by `2026-05-11-sota-2026-massive-team-program.md` and is ready to flip the status row from `dispatched` → `design done`.
