---
title: "Persona-Eval SOTA Closeout — 2026-05-18 Audit Chain"
created: 2026-05-18
status: shipped
related:
  - docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md
  - docs/plans/2026-05-18-phase5-eval-honest-status.md
  - docs/plans/2026-05-18-adapter-routing-decision.md
  - .claude/rules/silent-config-gated-subsystems.md
  - scripts/persona_eval_comparison.py
  - scripts/eval_shape_classifier.py
  - scripts/eval_sota_scorecard.py
---

# Persona-Eval SOTA Closeout

> **Product question that started the chain**: "Why isn't h-uman more
> fun, witty, engaging — a real iMessage pro?"
>
> **Closing answer**: h-uman's model + adapter CAN produce Seth's voice.
> The eval framework was measuring base-model behavior with no persona
> scaffolding, hiding the actual capability. With the eval-bypass fix
> + compact persona prompt + deterministic shape classifier, post-fix
> mean shape-score is 0.350 vs pre-fix 0.053 (+559% relative,
> non-overlapping 95% bootstrap CIs). Production iMessage outbound
> (which goes through `agent_turn` not the eval framework) was never
> affected — it was always injecting the persona system prompt
> correctly at `agent_turn.c:2322`.

## The audit chain

```
Product Q ("not witty enough on iMessage")
   ↓ measure
First eval baseline: 25-37% pass rate (judge-noisy, n=4 runs)
   ↓ controlled experiment (Python harness, n=8)
Bare vs persona-wrapped: 97% length reduction, 100% markdown elimination
   ↓ trace
src/eval.c:572 passes NULL/0 as system prompt
   ↓ fix
33f8eaa5: load persona via hu_persona_load + thread through hu_eval_suite_t::system_prompt
   ↓ measure (post-fix)
Eval shape-score: pre 0.053 vs post 0.350; 95% CIs non-overlapping
   ↓ identify next bottleneck
16KB system prompt → 60-95s/task MLX latency, 75% timeouts
   ↓ fix
39c4e72a: hu_persona_build_prompt_compact (2.6KB), 2-3x faster, identical voice
   ↓ measure
Per-suite shape pass rates: imessage 35%, tier1-naturalness 75%
   ↓ infrastructure
Phase 5 eval gate (HU_ENABLE_RL_FULL build): PROMOTE verdict on
   real persona-scores [0.20, 0.50] CI vs baseline 0.053
```

## What shipped (all on main)

### Code fixes
| Commit | Change |
|---|---|
| `725b1e51` | fix(config,validate): allowlist reaction_collection + personalization |
| `33f8eaa5` | **fix(eval,persona): wire persona system prompt into eval runner** (the headline) |
| `39c4e72a` | fix(persona,eval): compact persona prompt for throughput |
| `0adfda15` | fix(eval): aggregate per-task elapsed_ms into total_elapsed_ms (B2) |
| `d24a5dba` | fix(scripts,live-fire): discriminating M3 demo cleanup |
| `963eb56a` | test(config,validate): pin reaction_collection + personalization whitelist |

### Tools & infrastructure
| File | Purpose |
|---|---|
| `scripts/persona_eval_comparison.py` | Bare-vs-persona controlled experiment (the diagnostic that proved the bypass) |
| `scripts/eval_shape_classifier.py` | Deterministic shape classifier, per-channel rules (iMessage/Telegram/Discord/Slack/email) |
| `scripts/eval_sota_scorecard.py` | Bootstrap-CI scorecard generator (Phase 5 methodology applied to shape score) |
| `scripts/verify-reaction-collection-fix.sh` | End-to-end verify script for the signal pipeline |
| `build-rl-sota/human` | HU_ENABLE_RL_FULL build — unlocks `eval gate`, `eval competitive`, `eval leaderboard` |

### Docs
| Doc | Captures |
|---|---|
| `.claude/rules/silent-config-gated-subsystems.md` | Prevention rule for the failure class that hid the original signal-pipeline gap |
| `docs/plans/2026-05-18-adapter-routing-decision.md` | persona-v8 + MoLoRA-imessage decision (single base, channel-scoped compose) |
| `docs/plans/2026-05-18-phase5-eval-honest-status.md` | Correction of stale parent-plan status; documents shipped Phase 5 code |
| `docs/plans/2026-05-18-persona-eval-sota-closeout.md` | This doc — the chain summarized |

## Empirical results

### Per-suite scorecard (data through commit `0adfda15`)

| Suite | n (pre/post) | Judge % (pre→post) | **Shape % (pre→post)** | **Mean shape (95% CI, pre→post)** | Non-NULL % (pre→post) |
|-------|--------------|--------------------|------------------------|-----------------------------------|------------------------|
| imessage-humanness | 4r/5r, 32t/40t | 15.6% → 25.0% | 0.0% → **35.0%** | **0.053 [0.013, 0.108] → 0.350 [0.200, 0.500]** | 40.6% → 35.0% |
| tier1-naturalness | -/1r, 12t | — / 50.0% | — / **75.0%** | — / **0.750 [0.500, 1.000]** | — / 75.0% |
| humor-engine | (post run in progress) | | | | |
| human-likeness | (post run in progress) | | | | |

**Headline numbers** (imessage-humanness, n=72):
- Mean shape-score: **0.053 → 0.350 (+0.297 absolute, +559% relative)**
- 95% CIs **non-overlapping** (pre upper 0.108 < post lower 0.200)
- Phase 5 eval gate verdict: **PROMOTE** with real persona-scores

### Categorical evidence (Python harness, n=8)

| | Pre-fix | Post-fix |
|---|---|---|
| Avg response length | 733 chars | 24 chars |
| Markdown response count | 8/8 | 0/8 |
| Example response (imsg-006) | 699-char markdown options list | `"yeah just sent it"` (17 chars) |
| Example response (imsg-008) | 1090-char markdown options list | `"damn that's brutal. what did they even say?"` (43 chars) |

## Important findings (knowledge to carry forward)

1. **The LLM judge is unreliable in both directions.** Pre-fix it gave PASS to AI-assistant markdown responses (false positives). Post-fix it gives FAIL to peak-Seth-voice responses like `"depends if i can get my cat to move off the keyboard"` (false negatives). **The shape classifier should be the primary metric, judge as secondary.**

2. **Smaller persona prompt → MORE characteristic voice.** The 16 KB `hu_persona_build_prompt` produces safer/more-generic outputs than the 2.6 KB `hu_persona_build_prompt_compact`. Empirically observed: imsg-008 with 16KB → `"damn that's brutal. what did they even say?"`; with 2.6KB → `"damn that's brutal. nothing like a public execution in front of the whole team to start the day"`. **The 5 example shots do the heavy lifting; the other 13 KB of context fields overconstrain.**

3. **The fused adapter alone does NOT produce Seth-voice.** Bare prompts to `seth-v3-fused` produce default AI-assistant markdown. The voice comes from prompt + few-shot examples; the adapter's marginal contribution is small. Implication for M2/M3 (next-adapter training): need more data (1K+ messages per [Cui et al. 2025](https://arxiv.org/abs/2507.04889)) OR rely on example-bank RAG for voice fidelity.

4. **Silent NULL defaults are a recurring failure class.** `reaction_collection.enabled=false` (config gate), `chat_with_system(NULL, 0, ...)` (eval bypass), `eval_run.total_elapsed_ms=0` (metric not summed) — all the same shape. The new `silent-config-gated-subsystems.md` rule prevents the first class; remaining classes need their own audits.

5. **Production iMessage outbound was never affected by the eval bypass.** `agent_turn.c:2322` correctly builds persona prompt via `hu_persona_build_prompt`, threads to `cfg.persona_prompt`, calls `hu_prompt_build_system`. The bug was unique to `human eval run`. **Production may benefit from compact-prompt mode for throughput (60-95s → 30s/turn), but quality is not affected.**

## What's still open (next-session work)

| ID | Item | Effort | Gate |
|----|------|--------|------|
| **U3** | Human-rater validation of shape classifier (50-pair κ score) | ~4 hours | Seth available |
| **M2** | `imessage-tapback-v1` ORPO adapter training | Multi-day | M1 (≥500 tapback rows in `dpo_pairs`) |
| **M3** | MoLoRA channel-scoped routing at `agent_turn.c:4879` | ~100 LOC | M2 promotes through gate |
| **M4** | C-side shape classifier integrated into `hu_eval_result_t` | ~200 LOC + tests | — |
| **M6** | Production iMessage A/B (send through actual daemon) | ~2 hours | — |
| **Apple FM bridge** | Swift subprocess server for `eval competitive --include apple-fm` | ~1 week | macOS 26+ entitlements |
| **Gemini Nano bridge** | Headless Chrome subprocess for `eval competitive --include gemini-nano` | ~3 days | Chrome with --enable-features=AIPromptAPI |

## How to use this work

### Run an eval through the persona-injected path
```bash
~/Documents/h-uman/build/human eval run eval_suites/imessage_humanness.json
# Look for the log line:
#   [eval] loaded persona 'seth' for channel 'imessage' (system prompt: 2617 bytes)
# If present: the persona is being injected. If absent: regression.
```

### Compute shape scores on existing runs
```bash
python3 scripts/eval_shape_classifier.py --suite imessage-humanness
python3 scripts/eval_shape_classifier.py --compare-runs 4 9 --verbose
```

### Generate the SOTA scorecard with bootstrap CI
```bash
python3 scripts/eval_sota_scorecard.py
python3 scripts/eval_sota_scorecard.py --markdown
```

### Run the Phase 5 promotion gate
```bash
SCORES=$(python3 -c "import sqlite3, sys; sys.path.insert(0,'scripts'); from eval_shape_classifier import classify; con=sqlite3.connect('/Users/sethford/.human/memory.db'); rows=con.execute(\"SELECT actual_output FROM eval_results WHERE run_id>=5\").fetchall(); print(','.join(str(classify(r[0])['score']) for r in rows))")
./build-rl-sota/human eval gate --persona-scores "$SCORES" --persona-baseline 0.053 --persona-delta-min 0.05
# Expected: PROMOTE with CI [0.20, 0.50] for post-fix data
```

## TL;DR

- **Started**: vague "not witty enough on iMessage" product complaint.
- **Audited**: traced through reaction-collection config, MLX kill cycle, eval framework NULL-system-prompt bypass.
- **Fixed**: 4 commits on main; eval framework now injects persona; compact prompt 6× smaller; per-channel shape classifier.
- **Measured**: shape-score 0.053 → 0.350 (+559%), bootstrap CIs non-overlapping, Phase 5 gate PROMOTE.
- **Honest gap**: human-rater validation of shape classifier; Apple FM / Gemini Nano competitive baselines; ORPO adapter training (gated on tapback corpus). Documented and tractable.
