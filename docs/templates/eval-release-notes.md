---
title: Eval release notes template
description: Template for publishing evaluation results in release notes or PRs
updated: 2026-03-22
---

# Eval Release Notes Template

Copy this into your release notes or PR description when publishing evaluation results.

---

## Evaluation Summary — [date]

**Claim**: [One sentence: "pass rate R on suite S under judge J with provider P."]

### Configuration

| Field | Value |
| ----- | ----- |
| Build | `[commit SHA or tag]` |
| Agent provider | `[HUMAN_PROVIDER]` |
| Agent model | `[HUMAN_MODEL]` |
| Judge model | `[ADV_EVAL_MODEL]` — pinned version for this claim |
| Autonomy level | `[HUMAN_AUTONOMY or default]` |
| Suite manifest version | `[from eval_suites/MANIFEST.md]` |
| Tools allowed | `[list or "all default"]` |
| Date | `[UTC]` |

### System quality vs model quality

Separate what the **Human runtime** (prompts, tool wiring, harness) is responsible for from what the **base model** (`HUMAN_MODEL`) is responsible for.

- **Same suite + judge, different models**: If you change only `HUMAN_MODEL`, differences in pass rate are primarily **model** effects, not regressions in Human’s C code or default prompts.
- **Smaller / cheaper models** (e.g. `gpt-4o-mini`) often underperform on: warm refusal of romantic or intimate roleplay, strict “no fabricated live data” discipline, and subtle legal or epistemic boundaries—even when system safety text says the right thing.
- **Stronger chat models** (e.g. `gpt-4o`, Claude Sonnet-class) are expected to align better with those probes under the same system prompt.
- **Static `human eval run`**: Suites such as [`eval_suites/adversarial.json`](../../eval_suites/adversarial.json) and [`eval_suites/reasoning_basic.json`](../../eval_suites/reasoning_basic.json) use `match_mode: llm_judge`. The runner asks the configured provider to judge each task; when a task includes a `rubric`, it is combined with the gold `expected` string in the judge prompt (see `hu_eval_run_suite` in `src/eval/eval.c`).

When publishing numbers, state both **agent model** and **judge model** so readers can reproduce and interpret failures.

### Results

| Suite | Tasks | Passed | Failed | Pass Rate | Delta vs Last |
| ----- | ----: | -----: | -----: | --------: | ------------: |
| `reasoning_basic` | 10 | | | | |
| `tool_use_basic` | 5 | | | | |
| `adversarial` | 10 | | | | |
| `capability_edges` | 10 | | | | |
| `human_likeness` | 8 | | | | |
| `tool_capability` | 8 | | | | |
| `multi_turn` | 6 | | | | |
| `coding_basic` | 5 | | | | |
| **Total** | | | | | |

### Failure Modes (top buckets)

| Bucket | Count | Example Task ID | Brief Description |
| ------ | ----: | --------------- | ----------------- |
| | | | |

### Known Limitations

- [e.g. "No real tool loop for `human eval run`; tool_capability scores are text-based rubric only."]
- [e.g. "multi_turn scenarios run each turn as a fresh `human agent -m`; no session state persists."]
- [e.g. "Agent model `gpt-4o-mini`: known weak spots on hl-008-style romantic boundaries and tc-008-style live sports facts despite safety rules—compare to a stronger model before treating as a product defect."]

### Regressions

- [e.g. "adversarial pass rate dropped from 0.90 to 0.80 vs last run on 2026-03-14; investigating hypothetical_harm bucket."]
- None / list here.

### Checklist

- [ ] Suite manifest version noted above
- [ ] Judge model pinned (same model for every number in this report)
- [ ] Failure modes listed with task ids
- [ ] Regressions compared to **same** suite + judge from prior run
- [ ] If prompts were tuned against any suite, that suite is marked "training" (not held-out evidence)
- [ ] Report artifact (JSON) attached or linked: `build/redteam-fleet-reports/live-[timestamp]/`
